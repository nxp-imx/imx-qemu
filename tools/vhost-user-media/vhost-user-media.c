/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Vhost-user-media daemon - virtio-media backend for Xen/QEMU.
 *
 * This daemon implements the virtio-media protocol, acting as a V4L2 ioctl
 * proxy between a guest (DomU) and a real V4L2 device on the host (Dom0).
 *
 * Copyright 2025 NXP
 */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <linux/videodev2.h>
#include <glib.h>
#include <glib-unix.h>
#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>

#include <xengnttab.h>

#include "vhost-user-media.h"

/* Number of event buffers pre-allocated on the event queue */
#define NUM_EVENT_BUFS 16

/* Maximum number of virtqueues: commandq + eventq */
#define VHOST_USER_MEDIA_MAX_QUEUES 2

/*
 * Session management
 */

static uint32_t next_session_id = 1;

static struct media_session *find_session(struct vhost_user_media *m,
                                          uint32_t session_id)
{
    GList *l;

    for (l = m->sessions; l != NULL; l = l->next) {
        struct media_session *s = l->data;
        if (s->id == session_id)
            return s;
    }
    return NULL;
}

/*
 * Register the session V4L2 fd poll source once the backend decoder can
 * make forward progress, i.e. the OUTPUT (bitstream) queue is streaming
 * and has at least one queued buffer.  Registering earlier would make
 * GLib busy-loop on the EPOLLERR that v4l2_m2m_poll() returns while both
 * queues are idle.  Registration happens at most once per session.
 */
static void maybe_register_poll(struct vhost_user_media *m,
                                struct media_session *s)
{
    if (s->poll_source_id)
        return;
    if (!s->output_streaming || !s->output_qbuf_done)
        return;
    if (m->register_poll) {
        m->register_poll(m, s);
        g_message("Session %u: poll registered (OUTPUT streaming + QBUF done)",
                  s->id);
    }
}

static struct media_session *create_session(struct vhost_user_media *m)
{
    struct media_session *s;
    int fd;

    fd = open(m->v4l2_device, O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        g_warning("Failed to open %s: %s", m->v4l2_device, strerror(errno));
        return NULL;
    }

    s = g_new0(struct media_session, 1);
    s->id = next_session_id++;
    s->v4l2_fd = fd;
    s->media = m;
    s->poll_source_id = 0;

    m->sessions = g_list_append(m->sessions, s);

    /*
     * NOTE: Do NOT register the V4L2 fd poll source here.
     *
     * The backend V4L2 stateful/m2m decoder returns EPOLLERR from
     * v4l2_m2m_poll() as long as neither the OUTPUT nor the CAPTURE
     * queue is streaming with a queued buffer.  If we registered the
     * poll source right after open(), GLib would busy-loop on the
     * unconditional POLLERR and starve the command queue.
     *
     * Instead we defer registration until the OUTPUT (bitstream) queue
     * has been STREAMON-ed and at least one buffer has been QBUF-ed,
     * i.e. once the backend can actually make forward progress.  See
     * maybe_register_poll().
     */
    g_debug("Created session %u (fd=%d)", s->id, fd);
    return s;
}

void vhost_media_destroy_session(struct vhost_user_media *m,
                            struct media_session *s)
{
    g_debug("Destroying session %u", s->id);

    /* Remove poll source */
    if (s->poll_source_id) {
        g_source_remove(s->poll_source_id);
        s->poll_source_id = 0;
    }

    /* Release all MMAP buffer grant refs for this session */
    for (GList *l = s->mmaps; l != NULL; l = l->next) {
        struct media_mmap_entry *entry = l->data;
        if (entry->dma_fd >= 0) {
            xengnttab_dmabuf_imp_release(
                (xengnttab_handle *)m->gntdev_handle, entry->dma_fd);
            close(entry->dma_fd);
        }
        g_free(entry);
    }
    g_list_free(s->mmaps);

    if (s->v4l2_fd >= 0)
        close(s->v4l2_fd);

    m->sessions = g_list_remove(m->sessions, s);
    g_free(s);
}

/*
 * Command handlers
 */

static void handle_cmd_open(struct vhost_user_media *m,
                            const void *cmd_buf, size_t cmd_len,
                            void *resp_buf, size_t *resp_len)
{
    struct virtio_media_resp_open *resp = resp_buf;
    struct media_session *s;

    s = create_session(m);
    if (!s) {
        resp->hdr.status = ENODEV;
        *resp_len = sizeof(*resp);
        return;
    }

    resp->hdr.status = 0;
    resp->session_id = s->id;
    *resp_len = sizeof(*resp);
}

static void handle_cmd_close(struct vhost_user_media *m,
                             const void *cmd_buf, size_t cmd_len)
{
    const struct virtio_media_cmd_close *cmd = cmd_buf;
    struct media_session *s;

    s = find_session(m, cmd->session_id);
    if (s)
        vhost_media_destroy_session(m, s);
}

/*
 * Handle buffer-related ioctls (QBUF, DQBUF, QUERYBUF, PREPARE_BUF).
 *
 * The guest sends the descriptor chain as:
 *   device-readable: [v4l2_buffer][v4l2_plane[length]]
 *   device-writable: [resp_header][v4l2_buffer][v4l2_plane[length]]
 *
 * We need to:
 * 1. Parse v4l2_buffer from the payload
 * 2. If multiplanar, parse the planes array that follows
 * 3. Fix up m.planes pointer to point to our local planes array
 * 4. Call the ioctl
 * 5. Write back the updated v4l2_buffer + planes to the response
 */
static void handle_buffer_ioctl(struct vhost_user_media *m,
                                struct media_session *s,
                                unsigned long v4l2_ioctl_nr,
                                const uint8_t *payload, size_t payload_len,
                                uint8_t *resp_buf, size_t resp_buf_size,
                                size_t *resp_len)
{
    struct virtio_media_resp_ioctl *resp =
        (struct virtio_media_resp_ioctl *)resp_buf;
    struct v4l2_buffer vbuf;
    struct v4l2_plane planes[VIDEO_MAX_PLANES];
    uint8_t *resp_payload = resp_buf + sizeof(*resp);
    int ret;

    if (payload_len < sizeof(struct v4l2_buffer)) {
        resp->hdr.status = EINVAL;
        *resp_len = sizeof(*resp);
        return;
    }

    /* Copy v4l2_buffer from payload */
    memcpy(&vbuf, payload, sizeof(vbuf));
    memset(planes, 0, sizeof(planes));

    if (V4L2_TYPE_IS_MULTIPLANAR(vbuf.type)) {
        uint32_t nplanes = vbuf.length;
        if (nplanes > VIDEO_MAX_PLANES)
            nplanes = VIDEO_MAX_PLANES;

        /* Planes array follows v4l2_buffer in the payload */
        size_t planes_offset = sizeof(struct v4l2_buffer);
        size_t planes_size = nplanes * sizeof(struct v4l2_plane);

        if (payload_len >= planes_offset + planes_size) {
            memcpy(planes, payload + planes_offset, planes_size);
        }

        /* Fix up the planes pointer */
        vbuf.m.planes = planes;
    }

    /*
     * For QBUF with MMAP memory, the buffer is already on the device.
     * For DQBUF, we just need to dequeue.
     */
    vbuf.memory = V4L2_MEMORY_MMAP;

    ret = ioctl(s->v4l2_fd, v4l2_ioctl_nr, &vbuf);
    if (ret < 0) {
        resp->hdr.status = errno;
        /* Still write back the buffer for error reporting */
        memcpy(resp_payload, &vbuf, sizeof(vbuf));
        if (V4L2_TYPE_IS_MULTIPLANAR(vbuf.type)) {
            memcpy(resp_payload + sizeof(vbuf), planes,
                   vbuf.length * sizeof(struct v4l2_plane));
            *resp_len = sizeof(*resp) + sizeof(vbuf) +
                        vbuf.length * sizeof(struct v4l2_plane);
        } else {
            *resp_len = sizeof(*resp) + sizeof(vbuf);
        }
        return;
    }

    /* Success: write back updated v4l2_buffer + planes */
    resp->hdr.status = 0;

    /*
     * Once a buffer has been queued on the OUTPUT (bitstream) queue, the
     * backend decoder can start making forward progress.  Combined with
     * OUTPUT STREAMON this is the point at which polling the backend fd
     * becomes meaningful (see maybe_register_poll()).
     */
    if (v4l2_ioctl_nr == VIDIOC_QBUF && V4L2_TYPE_IS_OUTPUT(vbuf.type)) {
        s->output_qbuf_done = true;
        maybe_register_poll(m, s);
    }

    /* Save planes pointer before copying (it's a kernel pointer now) */
    struct v4l2_plane *saved_planes = vbuf.m.planes;
    memcpy(resp_payload, &vbuf, sizeof(vbuf));

    if (V4L2_TYPE_IS_MULTIPLANAR(vbuf.type) && saved_planes) {
        uint32_t nplanes = vbuf.length;
        if (nplanes > VIDEO_MAX_PLANES)
            nplanes = VIDEO_MAX_PLANES;
        memcpy(resp_payload + sizeof(vbuf), saved_planes,
               nplanes * sizeof(struct v4l2_plane));
        *resp_len = sizeof(*resp) + sizeof(vbuf) +
                    nplanes * sizeof(struct v4l2_plane);
    } else {
        *resp_len = sizeof(*resp) + sizeof(vbuf);
    }
}

/*
 * Handle VIDIOC_REQBUFS.
 *
 * We force V4L2_MEMORY_MMAP since the host owns the buffers.
 * We also strip DMABUF and USERPTR capabilities from the response.
 */
static void handle_reqbufs_ioctl(struct vhost_user_media *m,
                                 struct media_session *s,
                                 const uint8_t *payload, size_t payload_len,
                                 uint8_t *resp_buf, size_t resp_buf_size,
                                 size_t *resp_len)
{
    struct virtio_media_resp_ioctl *resp =
        (struct virtio_media_resp_ioctl *)resp_buf;
    struct v4l2_requestbuffers reqbufs;
    uint8_t *resp_payload = resp_buf + sizeof(*resp);
    int ret;

    if (payload_len < sizeof(reqbufs)) {
        resp->hdr.status = EINVAL;
        *resp_len = sizeof(*resp);
        return;
    }

    memcpy(&reqbufs, payload, sizeof(reqbufs));

    /* Force MMAP memory type - host owns the buffers */
    reqbufs.memory = V4L2_MEMORY_MMAP;

    ret = ioctl(s->v4l2_fd, VIDIOC_REQBUFS, &reqbufs);
    if (ret < 0) {
        resp->hdr.status = errno;
        memcpy(resp_payload, &reqbufs, sizeof(reqbufs));
        *resp_len = sizeof(*resp) + sizeof(reqbufs);
        return;
    }

    /* Strip DMABUF and USERPTR capabilities */
    reqbufs.capabilities &= ~V4L2_BUF_CAP_SUPPORTS_DMABUF;
    reqbufs.capabilities &= ~V4L2_BUF_CAP_SUPPORTS_USERPTR;

    resp->hdr.status = 0;
    memcpy(resp_payload, &reqbufs, sizeof(reqbufs));
    *resp_len = sizeof(*resp) + sizeof(reqbufs);
}

static void handle_cmd_ioctl(struct vhost_user_media *m,
                             const uint8_t *desc_buf, size_t desc_len,
                             uint8_t *resp_buf, size_t resp_buf_size,
                             size_t *resp_len)
{
    const struct virtio_media_cmd_ioctl *cmd =
        (const struct virtio_media_cmd_ioctl *)desc_buf;
    struct virtio_media_resp_ioctl *resp =
        (struct virtio_media_resp_ioctl *)resp_buf;
    struct media_session *s;
    uint32_t code = cmd->code;
    int ret;

    s = find_session(m, cmd->session_id);
    if (!s) {
        resp->hdr.status = EINVAL;
        *resp_len = sizeof(*resp);
        return;
    }

    const uint8_t *payload = desc_buf + sizeof(*cmd);
    size_t payload_len = desc_len - sizeof(*cmd);
    uint8_t *resp_payload = resp_buf + sizeof(*resp);
    size_t resp_payload_size = resp_buf_size - sizeof(*resp);

    /*
     * Reconstruct the ioctl number from the code.
     */
    unsigned long v4l2_ioctl_nr = virtio_media_code_to_ioctl(code);
    if (v4l2_ioctl_nr == 0) {
        g_warning("Unknown ioctl code %u", code);
        resp->hdr.status = ENOTTY;
        *resp_len = sizeof(*resp);
        return;
    }

    /*
     * Special handling for buffer-related ioctls.
     * The guest sends: [v4l2_buffer][v4l2_plane[length]] in the payload.
     * We need to fixup the m.planes pointer before calling ioctl.
     */
    if (v4l2_ioctl_nr == VIDIOC_QBUF || v4l2_ioctl_nr == VIDIOC_DQBUF ||
        v4l2_ioctl_nr == VIDIOC_QUERYBUF || v4l2_ioctl_nr == VIDIOC_PREPARE_BUF) {
        handle_buffer_ioctl(m, s, v4l2_ioctl_nr, payload, payload_len,
                            resp_buf, resp_buf_size, resp_len);
        return;
    }

    /*
     * Special handling for REQBUFS: force MMAP memory type and
     * strip DMABUF/USERPTR capabilities from the response.
     */
    if (v4l2_ioctl_nr == VIDIOC_REQBUFS) {
        handle_reqbufs_ioctl(m, s, payload, payload_len,
                             resp_buf, resp_buf_size, resp_len);
        return;
    }

    /*
     * Generic ioctl handling for all other ioctls.
     */
    unsigned int dir = _IOC_DIR(v4l2_ioctl_nr);
    size_t ioctl_size = _IOC_SIZE(v4l2_ioctl_nr);

    if (ioctl_size > resp_payload_size || ioctl_size > payload_len) {
        if (dir == _IOC_WRITE && payload_len >= ioctl_size) {
            /* OK */
        } else if (dir == _IOC_READ && ioctl_size <= resp_payload_size) {
            /* OK */
        } else if (dir == (_IOC_READ | _IOC_WRITE)) {
            if (payload_len < ioctl_size || ioctl_size > resp_payload_size) {
                g_warning("ioctl 0x%lx: buffer too small (payload=%zu, resp=%zu, need=%zu)",
                          v4l2_ioctl_nr, payload_len, resp_payload_size, ioctl_size);
                resp->hdr.status = EINVAL;
                *resp_len = sizeof(*resp);
                return;
            }
        }
    }

    /* Prepare ioctl buffer */
    uint8_t ioctl_buf[4096];
    if (ioctl_size > sizeof(ioctl_buf)) {
        resp->hdr.status = EINVAL;
        *resp_len = sizeof(*resp);
        return;
    }

    memset(ioctl_buf, 0, ioctl_size);

    /* Copy input payload for _IOW and _IOWR */
    if (dir & _IOC_WRITE) {
        size_t copy_len = (payload_len < ioctl_size) ? payload_len : ioctl_size;
        memcpy(ioctl_buf, payload, copy_len);
    }

    /* Execute the ioctl */
    ret = ioctl(s->v4l2_fd, v4l2_ioctl_nr, ioctl_buf);
    if (ret < 0) {
        resp->hdr.status = errno;
        if (dir & _IOC_READ) {
            memcpy(resp_payload, ioctl_buf, ioctl_size);
            *resp_len = sizeof(*resp) + ioctl_size;
        } else {
            *resp_len = sizeof(*resp);
        }
        return;
    }

    /* Copy output for _IOR and _IOWR */
    resp->hdr.status = 0;

    /*
     * Track OUTPUT (bitstream) queue STREAMON.  Together with the first
     * QBUF on the OUTPUT queue this triggers deferred poll registration
     * so we never busy-loop on the backend EPOLLERR (see
     * maybe_register_poll()).
     */
    if (v4l2_ioctl_nr == VIDIOC_STREAMON) {
        enum v4l2_buf_type btype = *(enum v4l2_buf_type *)ioctl_buf;
        if (V4L2_TYPE_IS_OUTPUT(btype)) {
            s->output_streaming = true;
            maybe_register_poll(m, s);
        }
    }

    if (dir & _IOC_READ) {
        memcpy(resp_payload, ioctl_buf, ioctl_size);
        *resp_len = sizeof(*resp) + ioctl_size;
    } else {
        *resp_len = sizeof(*resp);
    }
}

static void handle_cmd_mmap(struct vhost_user_media *m,
                            const void *cmd_buf, size_t cmd_len,
                            void *resp_buf, size_t resp_buf_size,
                            size_t *resp_len)
{
    const struct virtio_media_cmd_mmap *cmd = cmd_buf;
    struct virtio_media_resp_mmap *resp = resp_buf;
    struct media_session *s;
    struct media_mmap_entry *entry;
    struct v4l2_exportbuffer expbuf = {0};
    struct v4l2_buffer vbuf = {0};
    struct v4l2_plane planes[VIDEO_MAX_PLANES] = {0};
    uint32_t offset = cmd->offset;
    int dma_fd = -1;
    uint32_t buf_length = 0;
    uint32_t page_count;
    uint32_t *refs = NULL;
    int ret;

    s = find_session(m, cmd->session_id);
    if (!s) {
        resp->hdr.status = EINVAL;
        *resp_len = sizeof(*resp);
        return;
    }

    /* Check if we already have this mapping */
    for (GList *l = s->mmaps; l != NULL; l = l->next) {
        entry = l->data;
        if (entry->v4l2_offset == offset) {
            /* Return existing mapping (with full grant ref array) */
            if (sizeof(*resp) +
                (size_t)entry->grant_ref_count * sizeof(uint32_t) >
                resp_buf_size) {
                g_warning("MMAP: resp buffer too small for %u refs",
                          entry->grant_ref_count);
                resp->hdr.status = ENOSPC;
                *resp_len = sizeof(*resp);
                return;
            }
            resp->hdr.status = 0;
            resp->grant_ref_header = entry->grant_ref_header;
            resp->grant_ref_count = entry->grant_ref_count;
            resp->len = entry->len;
            memcpy((uint8_t *)resp_buf + sizeof(*resp), entry->refs,
                   (size_t)entry->grant_ref_count * sizeof(uint32_t));
            *resp_len = sizeof(*resp) +
                (size_t)entry->grant_ref_count * sizeof(uint32_t);
            return;
        }
    }

    /*
     * Decode the offset to find which buffer/plane this refers to.
     * Strategy: iterate over all allocated buffers to find the one with
     * matching m.mem_offset.
     */
    bool found = false;
    uint32_t found_index = 0;
    uint32_t found_plane = 0;
    enum v4l2_buf_type found_type = 0;

    enum v4l2_buf_type types[] = {
        V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
        V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
        V4L2_BUF_TYPE_VIDEO_CAPTURE,
        V4L2_BUF_TYPE_VIDEO_OUTPUT,
    };

    for (int t = 0; t < 4 && !found; t++) {
        for (uint32_t idx = 0; idx < 32 && !found; idx++) {
            memset(&vbuf, 0, sizeof(vbuf));
            memset(planes, 0, sizeof(planes));
            vbuf.type = types[t];
            vbuf.memory = V4L2_MEMORY_MMAP;
            vbuf.index = idx;
            if (V4L2_TYPE_IS_MULTIPLANAR(types[t])) {
                vbuf.length = VIDEO_MAX_PLANES;
                vbuf.m.planes = planes;
            }

            if (ioctl(s->v4l2_fd, VIDIOC_QUERYBUF, &vbuf) < 0)
                break;

            if (V4L2_TYPE_IS_MULTIPLANAR(types[t])) {
                for (uint32_t p = 0; p < vbuf.length; p++) {
                    if (planes[p].m.mem_offset == offset) {
                        found = true;
                        found_index = idx;
                        found_plane = p;
                        found_type = types[t];
                        buf_length = planes[p].length;
                        break;
                    }
                }
            } else {
                if (vbuf.m.offset == offset) {
                    found = true;
                    found_index = idx;
                    found_plane = 0;
                    found_type = types[t];
                    buf_length = vbuf.length;
                }
            }
        }
    }

    if (!found) {
        g_warning("MMAP: cannot find buffer for offset 0x%x", offset);
        resp->hdr.status = EINVAL;
        *resp_len = sizeof(*resp);
        return;
    }

    /* Export the buffer as a dma-buf */
    memset(&expbuf, 0, sizeof(expbuf));
    expbuf.type = found_type;
    expbuf.index = found_index;
    expbuf.plane = found_plane;
    expbuf.flags = O_RDWR;

    if (ioctl(s->v4l2_fd, VIDIOC_EXPBUF, &expbuf) < 0) {
        g_warning("VIDIOC_EXPBUF failed: %s", strerror(errno));
        resp->hdr.status = errno;
        *resp_len = sizeof(*resp);
        return;
    }
    dma_fd = expbuf.fd;

    /* Convert dma-buf to grant refs via xengnttab */
    page_count = (buf_length + 4095) / 4096;
    refs = g_new0(uint32_t, page_count);

    ret = xengnttab_dmabuf_imp_to_refs((xengnttab_handle *)m->gntdev_handle,
                                       m->domu_domid, dma_fd,
                                       page_count, refs);
    if (ret < 0) {
        g_warning("xengnttab_dmabuf_imp_to_refs failed: %s", strerror(errno));
        close(dma_fd);
        g_free(refs);
        resp->hdr.status = errno ? errno : EIO;
        *resp_len = sizeof(*resp);
        return;
    }

    /* Record the mapping */
    entry = g_new0(struct media_mmap_entry, 1);
    entry->v4l2_offset = offset;
    entry->len = buf_length;
    entry->dma_fd = dma_fd;
    entry->buf_index = found_index;
    entry->plane_index = found_plane;
    entry->buf_type = found_type;
    entry->grant_ref_header = refs[0];
    entry->grant_ref_count = page_count;
    entry->refs = refs; /* ownership transferred; freed on munmap */
    s->mmaps = g_list_append(s->mmaps, entry);

    g_debug("MMAP: offset=0x%x -> grant_ref=%u count=%u len=%u (buf %u plane %u)",
            offset, refs[0], page_count, buf_length,
            found_index, found_plane);

    if (sizeof(*resp) + (size_t)page_count * sizeof(uint32_t) >
        resp_buf_size) {
        g_warning("MMAP: resp buffer too small for %u refs", page_count);
        resp->hdr.status = ENOSPC;
        *resp_len = sizeof(*resp);
        return;
    }

    resp->hdr.status = 0;
    resp->grant_ref_header = entry->grant_ref_header;
    resp->grant_ref_count = entry->grant_ref_count;
    resp->len = buf_length;
    memcpy((uint8_t *)resp_buf + sizeof(*resp), entry->refs,
           (size_t)page_count * sizeof(uint32_t));
    *resp_len = sizeof(*resp) + (size_t)page_count * sizeof(uint32_t);
}

static void handle_cmd_munmap(struct vhost_user_media *m,
                              const void *cmd_buf, size_t cmd_len,
                              void *resp_buf, size_t *resp_len)
{
    const struct virtio_media_cmd_munmap *cmd = cmd_buf;
    struct virtio_media_resp_munmap *resp = resp_buf;
    uint32_t grant_ref_header = cmd->grant_ref_header;
    uint32_t grant_ref_count = cmd->grant_ref_count;

    /* Find and remove the mapping from any session */
    for (GList *sl = m->sessions; sl != NULL; sl = sl->next) {
        struct media_session *s = sl->data;
        for (GList *l = s->mmaps; l != NULL; l = l->next) {
            struct media_mmap_entry *entry = l->data;
            if (entry->grant_ref_header == grant_ref_header &&
                entry->grant_ref_count == grant_ref_count) {
                /* Release the imported dma-buf grant refs */
                xengnttab_dmabuf_imp_release(
                    (xengnttab_handle *)m->gntdev_handle, entry->dma_fd);
                close(entry->dma_fd);
                s->mmaps = g_list_remove(s->mmaps, entry);
                g_free(entry->refs);
                g_free(entry);
                resp->hdr.status = 0;
                *resp_len = sizeof(*resp);
                return;
            }
        }
    }

    resp->hdr.status = EINVAL;
    *resp_len = sizeof(*resp);
}

/*
 * Ioctl code lookup table.
 * Maps virtio-media ioctl codes (_IOC_NR) to full V4L2 ioctl numbers.
 */
unsigned long virtio_media_code_to_ioctl(uint32_t code)
{
    /* The code is _IOC_NR(VIDIOC_xxx) */
    switch (code) {
    case _IOC_NR(VIDIOC_QUERYCAP):       return VIDIOC_QUERYCAP;
    case _IOC_NR(VIDIOC_ENUM_FMT):       return VIDIOC_ENUM_FMT;
    case _IOC_NR(VIDIOC_G_FMT):          return VIDIOC_G_FMT;
    case _IOC_NR(VIDIOC_S_FMT):          return VIDIOC_S_FMT;
    case _IOC_NR(VIDIOC_REQBUFS):        return VIDIOC_REQBUFS;
    case _IOC_NR(VIDIOC_QUERYBUF):       return VIDIOC_QUERYBUF;
    case _IOC_NR(VIDIOC_G_FBUF):         return VIDIOC_G_FBUF;
    case _IOC_NR(VIDIOC_S_FBUF):         return VIDIOC_S_FBUF;
    case _IOC_NR(VIDIOC_OVERLAY):        return VIDIOC_OVERLAY;
    case _IOC_NR(VIDIOC_QBUF):           return VIDIOC_QBUF;
    case _IOC_NR(VIDIOC_EXPBUF):         return VIDIOC_EXPBUF;
    case _IOC_NR(VIDIOC_DQBUF):          return VIDIOC_DQBUF;
    case _IOC_NR(VIDIOC_STREAMON):       return VIDIOC_STREAMON;
    case _IOC_NR(VIDIOC_STREAMOFF):      return VIDIOC_STREAMOFF;
    case _IOC_NR(VIDIOC_G_PARM):         return VIDIOC_G_PARM;
    case _IOC_NR(VIDIOC_S_PARM):         return VIDIOC_S_PARM;
    case _IOC_NR(VIDIOC_G_STD):          return VIDIOC_G_STD;
    case _IOC_NR(VIDIOC_S_STD):          return VIDIOC_S_STD;
    case _IOC_NR(VIDIOC_ENUMSTD):        return VIDIOC_ENUMSTD;
    case _IOC_NR(VIDIOC_ENUMINPUT):      return VIDIOC_ENUMINPUT;
    case _IOC_NR(VIDIOC_G_CTRL):         return VIDIOC_G_CTRL;
    case _IOC_NR(VIDIOC_S_CTRL):         return VIDIOC_S_CTRL;
    case _IOC_NR(VIDIOC_G_TUNER):        return VIDIOC_G_TUNER;
    case _IOC_NR(VIDIOC_S_TUNER):        return VIDIOC_S_TUNER;
    case _IOC_NR(VIDIOC_G_AUDIO):        return VIDIOC_G_AUDIO;
    case _IOC_NR(VIDIOC_S_AUDIO):        return VIDIOC_S_AUDIO;
    case _IOC_NR(VIDIOC_QUERYCTRL):      return VIDIOC_QUERYCTRL;
    case _IOC_NR(VIDIOC_QUERYMENU):      return VIDIOC_QUERYMENU;
    case _IOC_NR(VIDIOC_G_INPUT):        return VIDIOC_G_INPUT;
    case _IOC_NR(VIDIOC_S_INPUT):        return VIDIOC_S_INPUT;
    case _IOC_NR(VIDIOC_G_OUTPUT):       return VIDIOC_G_OUTPUT;
    case _IOC_NR(VIDIOC_S_OUTPUT):       return VIDIOC_S_OUTPUT;
    case _IOC_NR(VIDIOC_ENUMOUTPUT):     return VIDIOC_ENUMOUTPUT;
    case _IOC_NR(VIDIOC_G_AUDOUT):       return VIDIOC_G_AUDOUT;
    case _IOC_NR(VIDIOC_S_AUDOUT):       return VIDIOC_S_AUDOUT;
    case _IOC_NR(VIDIOC_G_MODULATOR):    return VIDIOC_G_MODULATOR;
    case _IOC_NR(VIDIOC_S_MODULATOR):    return VIDIOC_S_MODULATOR;
    case _IOC_NR(VIDIOC_G_FREQUENCY):    return VIDIOC_G_FREQUENCY;
    case _IOC_NR(VIDIOC_S_FREQUENCY):    return VIDIOC_S_FREQUENCY;
    case _IOC_NR(VIDIOC_CROPCAP):        return VIDIOC_CROPCAP;
    case _IOC_NR(VIDIOC_G_CROP):         return VIDIOC_G_CROP;
    case _IOC_NR(VIDIOC_S_CROP):         return VIDIOC_S_CROP;
    case _IOC_NR(VIDIOC_TRY_FMT):        return VIDIOC_TRY_FMT;
    case _IOC_NR(VIDIOC_G_SELECTION):    return VIDIOC_G_SELECTION;
    case _IOC_NR(VIDIOC_S_SELECTION):    return VIDIOC_S_SELECTION;
    case _IOC_NR(VIDIOC_G_ENC_INDEX):    return VIDIOC_G_ENC_INDEX;
    case _IOC_NR(VIDIOC_ENCODER_CMD):    return VIDIOC_ENCODER_CMD;
    case _IOC_NR(VIDIOC_TRY_ENCODER_CMD): return VIDIOC_TRY_ENCODER_CMD;
    case _IOC_NR(VIDIOC_DECODER_CMD):    return VIDIOC_DECODER_CMD;
    case _IOC_NR(VIDIOC_TRY_DECODER_CMD): return VIDIOC_TRY_DECODER_CMD;
    case _IOC_NR(VIDIOC_SUBSCRIBE_EVENT): return VIDIOC_SUBSCRIBE_EVENT;
    case _IOC_NR(VIDIOC_UNSUBSCRIBE_EVENT): return VIDIOC_UNSUBSCRIBE_EVENT;
    case _IOC_NR(VIDIOC_G_EXT_CTRLS):   return VIDIOC_G_EXT_CTRLS;
    case _IOC_NR(VIDIOC_S_EXT_CTRLS):   return VIDIOC_S_EXT_CTRLS;
    case _IOC_NR(VIDIOC_TRY_EXT_CTRLS): return VIDIOC_TRY_EXT_CTRLS;
    case _IOC_NR(VIDIOC_ENUM_FRAMESIZES): return VIDIOC_ENUM_FRAMESIZES;
    case _IOC_NR(VIDIOC_ENUM_FRAMEINTERVALS): return VIDIOC_ENUM_FRAMEINTERVALS;
    case _IOC_NR(VIDIOC_PREPARE_BUF):   return VIDIOC_PREPARE_BUF;
    case _IOC_NR(VIDIOC_CREATE_BUFS):   return VIDIOC_CREATE_BUFS;
    case _IOC_NR(VIDIOC_QUERY_EXT_CTRL): return VIDIOC_QUERY_EXT_CTRL;
    default:
        return 0;
    }
}

/*
 * Process a command from the command virtqueue.
 */
void vhost_media_process_command(struct vhost_user_media *m,
                                 const uint8_t *cmd_buf, size_t cmd_len,
                                 uint8_t *resp_buf, size_t resp_buf_size,
                                 size_t *resp_len)
{
    const struct virtio_media_cmd_header *hdr =
        (const struct virtio_media_cmd_header *)cmd_buf;

    if (cmd_len < sizeof(*hdr)) {
        g_warning("Command too short: %zu bytes", cmd_len);
        struct virtio_media_resp_header *resp =
            (struct virtio_media_resp_header *)resp_buf;
        resp->status = EINVAL;
        *resp_len = sizeof(*resp);
        return;
    }

    switch (hdr->cmd) {
    case VIRTIO_MEDIA_CMD_OPEN:
        handle_cmd_open(m, cmd_buf, cmd_len, resp_buf, resp_len);
        break;
    case VIRTIO_MEDIA_CMD_CLOSE:
        handle_cmd_close(m, cmd_buf, cmd_len);
        *resp_len = 0; /* No response for CLOSE */
        break;
    case VIRTIO_MEDIA_CMD_IOCTL:
        handle_cmd_ioctl(m, cmd_buf, cmd_len, resp_buf, resp_buf_size,
                         resp_len);
        break;
    case VIRTIO_MEDIA_CMD_MMAP:
        handle_cmd_mmap(m, cmd_buf, cmd_len, resp_buf, resp_buf_size,
                        resp_len);
        break;
    case VIRTIO_MEDIA_CMD_MUNMAP:
        handle_cmd_munmap(m, cmd_buf, cmd_len, resp_buf, resp_len);
        break;
    default:
        g_warning("Unknown command %u", hdr->cmd);
        {
            struct virtio_media_resp_header *resp =
                (struct virtio_media_resp_header *)resp_buf;
            resp->status = ENOTTY;
            *resp_len = sizeof(*resp);
        }
        break;
    }
}

/*
 * Send a DQBUF event to the guest via the event queue.
 */
void vhost_media_send_dqbuf_event(struct vhost_user_media *m,
                                  uint32_t session_id,
                                  const struct v4l2_buffer *buf)
{
    struct virtio_media_event_dqbuf evt;

    memset(&evt, 0, sizeof(evt));
    evt.hdr.event = VIRTIO_MEDIA_EVT_DQBUF;
    evt.hdr.session_id = session_id;
    memcpy(&evt.buffer, buf, sizeof(*buf));

    if (V4L2_TYPE_IS_MULTIPLANAR(buf->type) && buf->m.planes) {
        uint32_t nplanes = buf->length;
        if (nplanes > VIRTIO_MEDIA_MAX_PLANES)
            nplanes = VIRTIO_MEDIA_MAX_PLANES;
        memcpy(evt.planes, buf->m.planes, nplanes * sizeof(struct v4l2_plane));
    }

    /* Queue the event - this will be picked up by the vhost-user event queue */
    if (m->send_event)
        m->send_event(m, &evt, sizeof(evt));
}

/*
 * Send a V4L2 event to the guest via the event queue.
 */
void vhost_media_send_v4l2_event(struct vhost_user_media *m,
                                 uint32_t session_id,
                                 const struct v4l2_event *event)
{
    struct virtio_media_event_event evt;

    memset(&evt, 0, sizeof(evt));
    evt.hdr.event = VIRTIO_MEDIA_EVT_EVENT;
    evt.hdr.session_id = session_id;
    memcpy(&evt.event, event, sizeof(*event));

    if (m->send_event)
        m->send_event(m, &evt, sizeof(evt));
}

/*
 * Poll a session's V4L2 fd for events (DQBUF, V4L2 events).
 * Called from the main loop when the fd is readable.
 */
void vhost_media_poll_session(struct vhost_user_media *m,
                              struct media_session *s)
{
    /* Check for dequeued CAPTURE buffers */
    struct v4l2_buffer vbuf;
    struct v4l2_plane planes[VIDEO_MAX_PLANES];

    memset(&vbuf, 0, sizeof(vbuf));
    memset(planes, 0, sizeof(planes));
    vbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    vbuf.memory = V4L2_MEMORY_MMAP;
    vbuf.length = VIDEO_MAX_PLANES;
    vbuf.m.planes = planes;

    while (ioctl(s->v4l2_fd, VIDIOC_DQBUF, &vbuf) == 0) {
        g_debug("Session %u: DQBUF capture index=%u", s->id, vbuf.index);
        vhost_media_send_dqbuf_event(m, s->id, &vbuf);

        /* Reset for next attempt */
        memset(&vbuf, 0, sizeof(vbuf));
        memset(planes, 0, sizeof(planes));
        vbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        vbuf.memory = V4L2_MEMORY_MMAP;
        vbuf.length = VIDEO_MAX_PLANES;
        vbuf.m.planes = planes;
    }

    /* Try OUTPUT queue too */
    memset(&vbuf, 0, sizeof(vbuf));
    memset(planes, 0, sizeof(planes));
    vbuf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    vbuf.memory = V4L2_MEMORY_MMAP;
    vbuf.length = VIDEO_MAX_PLANES;
    vbuf.m.planes = planes;

    while (ioctl(s->v4l2_fd, VIDIOC_DQBUF, &vbuf) == 0) {
        g_debug("Session %u: DQBUF output index=%u", s->id, vbuf.index);
        vhost_media_send_dqbuf_event(m, s->id, &vbuf);

        memset(&vbuf, 0, sizeof(vbuf));
        memset(planes, 0, sizeof(planes));
        vbuf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        vbuf.memory = V4L2_MEMORY_MMAP;
        vbuf.length = VIDEO_MAX_PLANES;
        vbuf.m.planes = planes;
    }

    /* Check for V4L2 events */
    struct v4l2_event v4l2_evt;
    while (ioctl(s->v4l2_fd, VIDIOC_DQEVENT, &v4l2_evt) == 0) {
        g_debug("Session %u: V4L2 event type=%u", s->id, v4l2_evt.type);
        vhost_media_send_v4l2_event(m, s->id, &v4l2_evt);
    }
}
