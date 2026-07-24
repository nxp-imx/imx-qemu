/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Vhost-user-media daemon header.
 *
 * Copyright 2025-2026 NXP
 */

#ifndef VHOST_USER_MEDIA_H
#define VHOST_USER_MEDIA_H

#include <stdint.h>
#include <stdbool.h>
#include <glib.h>
#include <linux/videodev2.h>

#include "virtio_media_protocol.h"

/*
 * A single MMAP buffer mapping entry.
 */
struct media_mmap_entry {
    uint32_t v4l2_offset;    /* V4L2 mem_offset from QUERYBUF */
    uint32_t len;            /* Length of the mapping */
    int dma_fd;              /* dma-buf fd from VIDIOC_EXPBUF */
    uint32_t buf_index;      /* V4L2 buffer index */
    uint32_t plane_index;    /* Plane index */
    enum v4l2_buf_type buf_type; /* Buffer type */
    uint32_t grant_ref_header; /* First grant ref (refs[0]) */
    uint32_t grant_ref_count;  /* Number of grant refs */
    uint32_t *refs;            /* Full grant ref array (grant_ref_count entries) */
    /*
     * Superseded by a REQBUFS reallocation: the host buffer this maps was
     * freed/detached from the queue, so this entry must not be reused for a
     * new MMAP of the same v4l2_offset. Its grant refs are still held (the
     * guest may keep a stale mapping) and released only on the matching
     * MUNMAP.
     */
    bool stale;
};

/*
 * A dma-buf the backend exported from a guest buffer's grant references, for a
 * V4L2_MEMORY_DMABUF buffer plane. The guest imports one of its own dma-bufs,
 * grants its pages to this (backend) domain and sends the grant references once
 * with a DMABUF_ATTACH command carrying a guest-assigned resource_id. We turn
 * them into a local dma-buf (xengnttab_dmabuf_exp_from_refs) and remember it
 * under resource_id; every subsequent QBUF/PREPARE_BUF of that plane carries
 * only resource_id (in v4l2_buffer m.fd / plane m.fd), which we resolve back to
 * this local fd to QBUF into the real V4L2 device.
 *
 * Keyed by resource_id (guest-assigned, never reused within a session). The
 * export lives from DMABUF_ATTACH until DMABUF_DETACH (or session close), which
 * mirrors the guest's per-(buffer, plane) import lifetime.
 */
struct media_dmabuf_export {
    uint32_t resource_id;        /* guest-assigned id - key */
    int dma_fd;                  /* local dma-buf fd from exp_from_refs */
};

/*
 * A session represents one open() on the V4L2 device.
 */
struct media_session {
    uint32_t id;             /* Session ID */
    int v4l2_fd;             /* File descriptor to the V4L2 device */
    GList *mmaps;            /* List of media_mmap_entry */
    struct vhost_user_media *media; /* Back pointer */
    guint poll_source_id;    /* GLib source ID for fd polling (0 if none) */
    bool output_streaming;   /* OUTPUT (bitstream) queue has been STREAMON */
    bool output_qbuf_done;   /* At least one buffer QBUF-ed on OUTPUT queue */

    /*
     * Memory model of each logical queue, as set at REQBUFS time. Defaults to
     * V4L2_MEMORY_MMAP (host owns the buffers). Set to V4L2_MEMORY_DMABUF when
     * the guest requests DMABUF and it is allowed, in which case QBUF planes
     * carry a resource_id resolved to a local dma-buf fd (see struct
     * media_dmabuf_export).
     */
    enum v4l2_memory output_memory;  /* OUTPUT (bitstream) queue memory */
    enum v4l2_memory capture_memory; /* CAPTURE (frame) queue memory */

    GList *dmabuf_exports;   /* List of media_dmabuf_export, keyed by id */
};

/*
 * Callback type for sending events to the guest.
 */
typedef void (*send_event_fn)(struct vhost_user_media *m,
                              const void *event, size_t event_len);

/*
 * Callback type for registering a session fd for polling.
 */
typedef void (*register_poll_fn)(struct vhost_user_media *m,
                                 struct media_session *s);

/*
 * Main vhost-user-media device state.
 */
struct vhost_user_media {
    char *v4l2_device;       /* Path to the V4L2 device (e.g. /dev/video0) */
    GList *sessions;         /* List of media_session */

    /* SHM region management (unused with grant refs, kept for future) */
    uint64_t shm_next_offset; /* Next free offset in SHM region */

    /* Xen grant device for dma-buf import/export */
    void *gntdev_handle;     /* xengnttab_handle */
    uint32_t domu_domid;     /* DomU domain ID for grant refs */

    /*
     * Worker thread pool used to release imported dma-buf grants off the
     * main loop. xengnttab_dmabuf_imp_release() blocks for ~2s per buffer
     * (it tears down the grant mapping of a wave6 dma-buf); doing that
     * synchronously on the single-threaded GLib main loop during a source
     * change starves the command virtqueue and makes the guest time out.
     * Created lazily on first use.
     */
    GThreadPool *grant_release_pool;

    /* Event sending callback (set by the vhost-user glue) */
    send_event_fn send_event;

    /* Session poll registration callback (set by the vhost-user glue) */
    register_poll_fn register_poll;

    /* Opaque pointer for the vhost-user glue layer */
    void *opaque;
};

/*
 * Process a command from the command virtqueue.
 */
void vhost_media_process_command(struct vhost_user_media *m,
                                 const uint8_t *cmd_buf, size_t cmd_len,
                                 uint8_t *resp_buf, size_t resp_buf_size,
                                 size_t *resp_len);

/*
 * Destroy a session and free its resources.
 */
void vhost_media_destroy_session(struct vhost_user_media *m,
                                 struct media_session *s);

/*
 * Send a DQBUF event to the guest.
 */
void vhost_media_send_dqbuf_event(struct vhost_user_media *m,
                                  uint32_t session_id,
                                  const struct v4l2_buffer *buf);

/*
 * Send a V4L2 event to the guest.
 */
void vhost_media_send_v4l2_event(struct vhost_user_media *m,
                                 uint32_t session_id,
                                 const struct v4l2_event *event);

/*
 * Send an error event to the guest.
 */
void vhost_media_send_error_event(struct vhost_user_media *m,
                                  uint32_t session_id,
                                  uint32_t error_code);

/*
 * Poll a session for dequeued buffers and events.
 */
void vhost_media_poll_session(struct vhost_user_media *m,
                              struct media_session *s,
                              GIOCondition events);

/*
 * Ioctl code lookup.
 */
unsigned long virtio_media_code_to_ioctl(uint32_t code);

#endif /* VHOST_USER_MEDIA_H */
