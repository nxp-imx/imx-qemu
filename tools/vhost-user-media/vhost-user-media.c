/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Vhost-user-media daemon - virtio-media backend for Xen/QEMU.
 *
 * This daemon implements the virtio-media protocol, acting as a V4L2 ioctl
 * proxy between a guest (DomU) and a real V4L2 device on the host (Dom0).
 *
 * Copyright 2025-2026 NXP
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

static void queue_grant_release(struct vhost_user_media *m, int dma_fd,
                                uint32_t *refs);

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

    /* Release all MMAP buffer grant refs for this session (off main loop) */
    for (GList *l = s->mmaps; l != NULL; l = l->next) {
        struct media_mmap_entry *entry = l->data;
        queue_grant_release(m, entry->dma_fd, entry->refs);
        g_free(entry);
    }
    g_list_free(s->mmaps);

    if (s->v4l2_fd >= 0)
        close(s->v4l2_fd);

    m->sessions = g_list_remove(m->sessions, s);
    g_free(s);
}

/*
 * A deferred grant-release job. The imported dma-buf's grant refs are torn
 * down (xengnttab_dmabuf_imp_release) and its fd closed on a worker thread so
 * the ~2s-per-buffer cost never blocks the GLib main loop / command queue.
 */
struct grant_release_job {
    void *gntdev_handle;
    int dma_fd;
    uint32_t *refs;
};

static void grant_release_worker(gpointer data, gpointer user_data)
{
    struct grant_release_job *job = data;

    (void)user_data;
    if (job->dma_fd >= 0) {
        xengnttab_dmabuf_imp_release((xengnttab_handle *)job->gntdev_handle,
                                     job->dma_fd);
        close(job->dma_fd);
    }
    g_free(job->refs);
    g_free(job);
}

/*
 * Hand an imported dma-buf's grant refs off to the release worker pool. The
 * caller must already have unlinked the owning entry so nothing else touches
 * @dma_fd/@refs; ownership of @refs transfers to the job. The pool is created
 * lazily and uses a single worker so releases stay serialized (matching the
 * previous synchronous behaviour) but off the main loop.
 *
 * Because the actual release (xengnttab_dmabuf_imp_release, ~2s each) runs
 * asynchronously on the single worker, a MUNMAP command may be answered with
 * status=0 ("accepted") before the grant is truly released; under a burst of
 * MUNMAPs the release can lag by N * ~2s. This is intentional (never block the
 * main loop) and only delays freeing the host buffer's memory - the guest has
 * already dropped its mapping, so correctness is unaffected.
 */
static void queue_grant_release(struct vhost_user_media *m, int dma_fd,
                                uint32_t *refs)
{
    struct grant_release_job *job;

    if (dma_fd < 0) {
        g_free(refs);
        return;
    }

    if (!m->grant_release_pool) {
        m->grant_release_pool =
            g_thread_pool_new(grant_release_worker, m, 1, FALSE, NULL);
        if (!m->grant_release_pool) {
            /* Fall back to a synchronous release if the pool can't start. */
            xengnttab_dmabuf_imp_release((xengnttab_handle *)m->gntdev_handle,
                                         dma_fd);
            close(dma_fd);
            g_free(refs);
            return;
        }
    }

    job = g_new0(struct grant_release_job, 1);
    job->gntdev_handle = m->gntdev_handle;
    job->dma_fd = dma_fd;
    job->refs = refs;
    g_thread_pool_push(m->grant_release_pool, job, NULL);
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
     * Once a buffer has been queued on the OUTPUT (bitstream) queue and that
     * queue is STREAMON-ed, the backend decoder can make forward progress and
     * polling its fd becomes meaningful (see maybe_register_poll()).
     */
    if (v4l2_ioctl_nr == VIDIOC_QBUF) {
        if (V4L2_TYPE_IS_OUTPUT(vbuf.type)) {
            s->output_qbuf_done = true;
        }
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

    /*
     * REQBUFS (with any count) frees the queue's previous buffers on the host
     * and allocates a fresh set. On a source change the client re-REQBUFS the
     * CAPTURE queue with a new count without an intervening REQBUFS(0), and the
     * host reuses the same v4l2 mem_offsets for the new buffers.
     *
     * We must therefore invalidate the mmap entries of this queue: the host
     * buffer each one maps has just been freed/detached, so a subsequent MMAP
     * of a reused offset must NOT return the old entry's (now dangling) grant
     * refs. Mark them stale instead of releasing them here: the guest driver
     * refcounts each buffer's grant map and only sends MUNMAP once the last
     * reference (owning queue released by this REQBUFS, plus any still-open
     * mmap or exported dma-buf) is dropped. So a stale entry keeps its imported
     * grant refs (and pins the old host buffer's memory) until the matching
     * MUNMAP arrives; a new MMAP of the same offset skips stale entries (see
     * handle_cmd_mmap()) and re-exports the new host buffer, getting fresh
     * grant refs. This mirrors vb2's REQBUFS semantics -- put the old buffer's
     * memory (refcounted, freed on last user) AND remove it from the queue so
     * its index/offset slot cannot alias a new buffer -- across the grant
     * boundary.
     */
    for (GList *l = s->mmaps; l != NULL; l = l->next) {
        struct media_mmap_entry *entry = l->data;

        if (entry->buf_type == reqbufs.type) {
            entry->stale = true;
        }
    }

    /* Strip DMABUF and USERPTR capabilities */
    reqbufs.capabilities &= ~V4L2_BUF_CAP_SUPPORTS_DMABUF;
    reqbufs.capabilities &= ~V4L2_BUF_CAP_SUPPORTS_USERPTR;

    resp->hdr.status = 0;
    memcpy(resp_payload, &reqbufs, sizeof(reqbufs));
    *resp_len = sizeof(*resp) + sizeof(reqbufs);
}

/*
 * Handle extended-control ioctls (G_EXT_CTRLS, S_EXT_CTRLS, TRY_EXT_CTRLS).
 *
 * The guest serialises the arguments contiguously in the payload:
 *
 *   [struct v4l2_ext_controls]
 *   [struct v4l2_ext_control ctrl[count]]              (when count > 0)
 *   [ctrl[i].ptr payload]...                           (compound controls,
 *                                                        each ctrl with size>0,
 *                                                        in order; S/TRY only)
 *
 * struct v4l2_ext_controls has a userspace 'controls' pointer and each
 * compound control has a userspace 'ptr'; neither is valid in this address
 * space.  Fix them up to point into our local scratch copy before calling the
 * ioctl, then write the (possibly updated) struct + array + compound payloads
 * back to the response in the same contiguous layout the guest expects.
 */
static void handle_ext_ctrls_ioctl(struct vhost_user_media *m,
                                    struct media_session *s,
                                    unsigned long v4l2_ioctl_nr,
                                    const uint8_t *payload, size_t payload_len,
                                    uint8_t *resp_buf, size_t resp_buf_size,
                                    size_t *resp_len)
{
    struct virtio_media_resp_ioctl *resp =
        (struct virtio_media_resp_ioctl *)resp_buf;
    uint8_t *resp_payload = resp_buf + sizeof(*resp);
    size_t resp_payload_size = resp_buf_size - sizeof(*resp);
    struct v4l2_ext_controls ctrls;
    struct v4l2_ext_control *ctrl_array = NULL;
    size_t array_size;
    size_t off;
    uint32_t i;
    int ret;

    if (payload_len < sizeof(ctrls)) {
        resp->hdr.status = EINVAL;
        *resp_len = sizeof(*resp);
        return;
    }

    /* Top-level struct comes first. */
    memcpy(&ctrls, payload, sizeof(ctrls));

    array_size = (size_t)ctrls.count * sizeof(struct v4l2_ext_control);
    if (ctrls.count > 0 && payload_len < sizeof(ctrls) + array_size) {
        resp->hdr.status = EINVAL;
        *resp_len = sizeof(*resp);
        return;
    }

    /*
     * Fix up the 'controls' pointer to reference the array that immediately
     * follows the struct in the payload.  We DQBUF into a mutable scratch copy
     * so the backend can write results (G/TRY) back into it.
     */
    if (ctrls.count > 0) {
        ctrl_array = g_memdup2(payload + sizeof(ctrls), array_size);
        ctrls.controls = ctrl_array;
    } else {
        ctrls.controls = NULL;
    }

    /*
     * Fix up each compound control's ->ptr to reference its payload, which the
     * guest serialised after the array in the same order.  Simple (non-array)
     * controls carry size == 0 and use the inline value, so they need no
     * fixup.  The wave6 decoder only exposes simple integer/menu controls, but
     * handle compound ones for completeness / future controls.
     */
    off = sizeof(ctrls) + array_size;
    for (i = 0; i < ctrls.count; i++) {
        uint32_t sz = ctrl_array[i].size;

        if (sz == 0)
            continue;
        if (payload_len < off + sz) {
            g_free(ctrl_array);
            resp->hdr.status = EINVAL;
            *resp_len = sizeof(*resp);
            return;
        }
        ctrl_array[i].ptr = g_memdup2(payload + off, sz);
        off += sz;
    }

    ret = ioctl(s->v4l2_fd, v4l2_ioctl_nr, &ctrls);
    resp->hdr.status = (ret < 0) ? errno : 0;

    /*
     * Write the result back in the guest's layout: the top-level struct
     * followed by the control array.  Restore the struct's 'controls' pointer
     * value is irrelevant to the guest (it keeps its own), so we leave the
     * fixed-up pointer as-is; the guest ignores it.  Compound payloads are
     * copied back into their slots so G_EXT_CTRLS can return array/string
     * controls.
     */
    if (sizeof(ctrls) <= resp_payload_size) {
        memcpy(resp_payload, &ctrls, sizeof(ctrls));
        *resp_len = sizeof(*resp) + sizeof(ctrls);

        if (ctrls.count > 0 &&
            sizeof(ctrls) + array_size <= resp_payload_size) {
            memcpy(resp_payload + sizeof(ctrls), ctrl_array, array_size);
            *resp_len = sizeof(*resp) + sizeof(ctrls) + array_size;

            off = sizeof(ctrls) + array_size;
            for (i = 0; i < ctrls.count; i++) {
                uint32_t sz = ctrl_array[i].size;

                if (sz == 0 || !ctrl_array[i].ptr)
                    continue;
                if (off + sz > resp_payload_size)
                    break;
                memcpy(resp_payload + off, ctrl_array[i].ptr, sz);
                off += sz;
                *resp_len = sizeof(*resp) + off;
            }

            /*
             * The guest reconstructs each compound control's ->ptr from its
             * own saved userspace pointers, but never let a host-local
             * pointer leak back across the wire: zero ->ptr in the echoed
             * array (in the response buffer, after the payloads have been
             * copied out above). Done last so the copy-back loop still had
             * the valid host pointers.
             */
            {
                struct v4l2_ext_control *resp_array =
                    (struct v4l2_ext_control *)(resp_payload +
                                                sizeof(ctrls));

                for (i = 0; i < ctrls.count; i++) {
                    if (resp_array[i].size > 0)
                        memset(&resp_array[i].ptr, 0,
                               sizeof(resp_array[i].ptr));
                }
            }
        }
    } else {
        *resp_len = sizeof(*resp);
    }

    /* Free compound payload copies, then the array. */
    for (i = 0; i < ctrls.count; i++) {
        if (ctrl_array[i].size && ctrl_array[i].ptr)
            g_free(ctrl_array[i].ptr);
    }
    g_free(ctrl_array);
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
     * Special handling for extended-control ioctls.  The guest serialises
     * struct v4l2_ext_controls followed by its v4l2_ext_control[] array (and,
     * for compound controls, each control's payload) contiguously in the
     * payload.  struct v4l2_ext_controls carries a userspace 'controls'
     * pointer that is meaningless here, so we must fix it up (and each
     * compound control's ->ptr) to point into our local copy before calling
     * the ioctl.  The generic path below would memcpy the flat struct with a
     * bogus pointer and the backend would fail with EFAULT.
     */
    if (v4l2_ioctl_nr == VIDIOC_G_EXT_CTRLS ||
        v4l2_ioctl_nr == VIDIOC_S_EXT_CTRLS ||
        v4l2_ioctl_nr == VIDIOC_TRY_EXT_CTRLS) {
        handle_ext_ctrls_ioctl(m, s, v4l2_ioctl_nr, payload, payload_len,
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

    /*
     * STREAMOFF returns every buffer of the queue to the client and stops the
     * stream; track OUTPUT streaming state for poll registration.
     */
    if (v4l2_ioctl_nr == VIDIOC_STREAMOFF) {
        enum v4l2_buf_type btype = *(enum v4l2_buf_type *)ioctl_buf;
        if (V4L2_TYPE_IS_OUTPUT(btype)) {
            s->output_streaming = false;
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

    /*
     * Check if we already have this mapping. Skip stale entries: after a
     * REQBUFS reallocation (source change) the host reuses v4l2 mem_offsets,
     * so an entry with a matching offset may map an already-freed host buffer.
     * Reusing it would hand the guest dangling grant refs (stale image, and
     * Dom0 "grant still in use"). A stale entry is kept only so its grant refs
     * survive until the matching MUNMAP; it must never satisfy a new MMAP.
     */
    for (GList *l = s->mmaps; l != NULL; l = l->next) {
        entry = l->data;
        if (!entry->stale && entry->v4l2_offset == offset) {
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

    /*
     * Invariant underpinning MUNMAP's grant-ref matching: no two live entries
     * may share a grant_ref_header. It holds because a not-yet-released import
     * (any entry still on s->mmaps, stale or not) pins its grant refs, so Xen
     * cannot hand the same ref to this fresh import. Concretely the pin comes
     * from xengnttab_dmabuf_imp_to_refs() keeping the refs mapped until the
     * matching xengnttab_dmabuf_imp_release() (issued from handle_cmd_munmap()
     * via queue_grant_release()); refs shared across the per-device gntdev
     * handle m->gntdev_handle, so this also holds across sessions.
     *
     * If it is ever violated (Xen/gntdev behaviour change), MUNMAP could match
     * and release the wrong entry -> silent host-buffer corruption. Fail this
     * MMAP instead: release the just-created import and return EIO. A failed
     * mmap is recoverable; wrong-buffer release is not.
     */
    for (GList *l = s->mmaps; l != NULL; l = l->next) {
        struct media_mmap_entry *other = l->data;

        if (other->grant_ref_header == entry->grant_ref_header) {
            g_warning("MMAP: duplicate grant_ref=%u for offset=0x%x (existing "
                      "offset=0x%x stale=%d) -- refusing to avoid ambiguous "
                      "MUNMAP matching",
                      entry->grant_ref_header, offset,
                      other->v4l2_offset, other->stale);
            queue_grant_release(m, entry->dma_fd, entry->refs);
            g_free(entry);
            resp->hdr.status = EIO;
            *resp_len = sizeof(*resp);
            return;
        }
    }

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

    /*
     * Find and remove the mapping from any session. Match on
     * (grant_ref_header, grant_ref_count), NOT on v4l2_offset. This is
     * unambiguous because grant_ref_header is unique among live entries: any
     * entry still on s->mmaps (stale or not) has an unreleased import that
     * pins its grant refs, so Xen cannot reuse the same ref for another live
     * entry (a ref is reused across REQBUFS generations only after the old
     * entry's MUNMAP released it and removed it from s->mmaps). v4l2_offset,
     * by contrast, IS reused across generations. So this targets exactly the
     * entry the guest is unmapping, including a stale one left by a source
     * change: the stale flag only excludes an entry from offset-based MMAP
     * reuse (see handle_cmd_mmap()), it does not affect MUNMAP, so every stale
     * entry is still reclaimed here by its own MUNMAP. (handle_cmd_mmap()
     * warns if the uniqueness invariant is ever violated.)
     */
    for (GList *sl = m->sessions; sl != NULL; sl = sl->next) {
        struct media_session *s = sl->data;
        for (GList *l = s->mmaps; l != NULL; l = l->next) {
            struct media_mmap_entry *entry = l->data;
            if (entry->grant_ref_header == grant_ref_header &&
                entry->grant_ref_count == grant_ref_count) {
                /* Release the imported dma-buf grant refs off the main loop */
                s->mmaps = g_list_remove(s->mmaps, entry);
                queue_grant_release(m, entry->dma_fd, entry->refs);
                g_free(entry);
                resp->hdr.status = 0;
                *resp_len = sizeof(*resp);
                return;
            }
        }
    }

    /*
     * No entry found: the buffer's grant refs were already released (a repeated
     * or late MUNMAP). MUNMAP is idempotent - its goal (releasing the grant) is
     * already achieved - so report success instead of EINVAL, which the guest
     * otherwise logs as "host failed to unmap buffer: -22". Keep a debug log so
     * a genuinely bogus grant_ref (one that was never MMAP'd) is still visible.
     */
    g_debug("MUNMAP: no entry for grant_ref=%u count=%u; already released "
            "(idempotent success)",
            grant_ref_header, grant_ref_count);
    resp->hdr.status = 0;
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
 * Send an error event to the guest.  The guest driver flags the session as
 * errored and wakes any poller so user-space sees EPOLLERR / dequeue errors.
 */
void vhost_media_send_error_event(struct vhost_user_media *m,
                                  uint32_t session_id,
                                  uint32_t error_code)
{
    struct virtio_media_event_error evt;

    memset(&evt, 0, sizeof(evt));
    evt.hdr.event = VIRTIO_MEDIA_EVT_ERROR;
    evt.hdr.session_id = session_id;
    evt.error_code = error_code;

    if (m->send_event)
        m->send_event(m, &evt, sizeof(evt));
}
/*
 * Result of trying to drain the CAPTURE queue.
 */
enum drain_result {
    DRAIN_MORE,      /* dequeued 0+ buffers, queue then returned EAGAIN */
    DRAIN_COMPLETE,  /* DQBUF returned EPIPE: drain finished, decoder idle */
    DRAIN_ERROR,     /* DQBUF failed with an unexpected error */
};

/*
 * Drain the CAPTURE queue: DQBUF every ready decoded frame and forward it to
 * the guest.  The final buffer of a drain carries V4L2_BUF_FLAG_LAST; getting
 * it out to the guest is what lets the guest driver mark is_capture_last and
 * return -EPIPE on subsequent DQBUF, so the application pipeline can stop.
 *
 * The fd is O_NONBLOCK, so DQBUF returns -1/EAGAIN when nothing is ready and
 * -1/EPIPE once the drain is complete.
 *
 * Returns:
 *   DRAIN_COMPLETE if the drain finished (DQBUF reported EPIPE),
 *   DRAIN_MORE     if it merely ran out of ready buffers (EAGAIN),
 *   DRAIN_ERROR    on any other DQBUF failure.
 */
static enum drain_result drain_capture_queue(struct vhost_user_media *m,
                                             struct media_session *s)
{
    struct v4l2_buffer vbuf;
    struct v4l2_plane planes[VIDEO_MAX_PLANES];

    for (;;) {
        memset(&vbuf, 0, sizeof(vbuf));
        memset(planes, 0, sizeof(planes));
        vbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        vbuf.memory = V4L2_MEMORY_MMAP;
        vbuf.length = VIDEO_MAX_PLANES;
        vbuf.m.planes = planes;

        if (ioctl(s->v4l2_fd, VIDIOC_DQBUF, &vbuf) != 0) {
            if (errno == EAGAIN)
                return DRAIN_MORE;
            if (errno == EPIPE)
                return DRAIN_COMPLETE;
            return DRAIN_ERROR;
        }

        g_debug("Session %u: DQBUF capture index=%u flags=0x%x bytesused=%u",
                s->id, vbuf.index, vbuf.flags,
                vbuf.m.planes ? vbuf.m.planes[0].bytesused : 0);
        vhost_media_send_dqbuf_event(m, s->id, &vbuf);
    }
}

/*
 * Poll a session's V4L2 fd for events (DQBUF, V4L2 events).
 * Called from the main loop when the fd is readable.
 */
void vhost_media_poll_session(struct vhost_user_media *m,
                              struct media_session *s,
                              GIOCondition events)
{
    struct v4l2_buffer vbuf;
    struct v4l2_plane planes[VIDEO_MAX_PLANES];

    /*
     * Dispatch strictly according to what poll() reported.  The V4L2 fd is
     * opened O_NONBLOCK, but we still must not DQBUF a queue that is not
     * signalled ready: doing CAPTURE DQBUF unconditionally when only
     * EPOLLOUT (OUTPUT consumed) fired just wastes an EAGAIN round-trip and
     * hides which queue actually has work.
     *   G_IO_PRI -> a V4L2 event is pending       -> DQEVENT
     *   G_IO_IN  -> CAPTURE has a decoded frame -> DQBUF CAPTURE
     *   G_IO_OUT -> OUTPUT buffer consumed       -> DQBUF OUTPUT
     *
     * G_IO_PRI (events) MUST be drained before G_IO_IN (CAPTURE buffers).
     * On a dynamic resolution change the wave6 backend signals G_IO_PRI
     * (V4L2_EVENT_SOURCE_CHANGE) together with G_IO_IN (the implicit-drain
     * CAPTURE buffer carrying V4L2_BUF_FLAG_LAST) in the same poll wakeup.
     * The V4L2 stateful decoder API requires SOURCE_CHANGE to be delivered
     * to userspace *before* the LAST buffer, so that when the client
     * dequeues LAST it already knows a resolution change is pending and runs
     * the STREAMOFF/REQBUFS/STREAMON reconfiguration instead of treating it
     * as an end-of-stream drain.  If we forwarded the LAST buffer first, the
     * guest would receive eos-then-source_change (reordered), GStreamer would
     * treat the LAST buffer as a plain drain completion and never reconfigure
     * the CAPTURE queue, and the pipeline would hang on seek/trick play.
     */
    if (events & G_IO_PRI) {
        struct v4l2_event v4l2_evt;
        while (ioctl(s->v4l2_fd, VIDIOC_DQEVENT, &v4l2_evt) == 0) {
            g_debug("Session %u: V4L2 event type=%u", s->id, v4l2_evt.type);
            vhost_media_send_v4l2_event(m, s->id, &v4l2_evt);
        }
    }

    if (events & G_IO_IN) {
        enum drain_result r = drain_capture_queue(m, s);
        if (r == DRAIN_COMPLETE && s->poll_source_id) {
            /* Drain finished on a normal readable poll. */
            g_message("Session %u: CAPTURE drain complete (EPIPE)", s->id);
            g_source_remove(s->poll_source_id);
            s->poll_source_id = 0;
        } else if (r == DRAIN_ERROR) {
            g_warning("Session %u: CAPTURE DQBUF error: %s",
                      s->id, strerror(errno));
            vhost_media_send_error_event(m, s->id, errno);
        }
    }

    if (events & G_IO_OUT) {
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
        /*
         * A genuine backend error (vb2_queue_error()) makes DQBUF return
         * -EIO; EAGAIN merely means the queue ran dry.  Report the former.
         */
        if (errno == EIO) {
            g_warning("Session %u: OUTPUT DQBUF error: %s",
                      s->id, strerror(errno));
            vhost_media_send_error_event(m, s->id, errno);
        }
    }

    /*
     * EPOLLERR / EPOLLHUP handling.  Done last, after draining every
     * data-carrying condition above.
     *
     * v4l2_m2m_poll() ORs EPOLLERR (from v4l2_m2m_poll_for_data() when neither
     * queue has a buffer queued to the driver) together with EPOLLPRI
     * (v4l2_event_pending()) in a single return, so a poll wakeup can report
     * G_IO_ERR alongside a pending V4L2 event or a ready buffer.  We must not
     * bail out on G_IO_ERR before servicing G_IO_OUT/G_IO_PRI, or we would
     * drop a source-change/EOS event and hang the guest.
     *
     * A bare EPOLLERR is NOT a reliable error signal.  v4l2_m2m_poll_for_data()
     * raises EPOLLERR whenever neither queue currently has a buffer on its
     * queued_list (vb2 q->error is 0) - a benign, transient condition that
     * occurs constantly around EOS/drain and whenever the guest has dequeued
     * everything and not yet re-queued.  It was confirmed on the wave6 backend
     * that every such wakeup carried SRC/DST q->error==0 (queued_list empty),
     * i.e. no vb2_queue_error() ever fired.  The daemon's own queued-buffer
     * counters cannot disambiguate this: the poll wakeup reflects the kernel's
     * queued_list at wakeup time, but by the time this callback runs the guest
     * may already have issued a QBUF, so the count no longer matches what the
     * driver saw.  Hence we must NOT derive "real error" from EPOLLERR at all.
     *
     * A genuine backend error (vb2_queue_error(): unsupported profile, device
     * run timeout, ...) sets q->error, which makes the very next QBUF/DQBUF on
     * that queue return -EIO.  That is the reliable, unambiguous signal and it
     * is handled on the data path: DQBUF EIO is reported as an error event from
     * the CAPTURE (drain_capture_queue -> DRAIN_ERROR) and OUTPUT loops, and a
     * QBUF EIO is propagated to the guest as the ioctl status.
     *
     * Therefore: ignore bare EPOLLERR here; only EPOLLHUP (real fd teardown)
     * is reported as an error event.
     */
    if (events & (G_IO_ERR | G_IO_HUP)) {
        if (events & G_IO_HUP) {
            g_warning("Session %u: backend EPOLLHUP, reporting error to guest",
                      s->id);
            vhost_media_send_error_event(m, s->id, EIO);
        }

        /*
         * The decoder keeps re-asserting EPOLLERR while both queues are idle,
         * so drop the poll source to avoid a GLib busy-loop that starves the
         * command queue.  It is re-registered on the next QBUF.
         */
        if (s->poll_source_id) {
            g_source_remove(s->poll_source_id);
            s->poll_source_id = 0;
        }
    }
}
