/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Vhost-user-media daemon header.
 *
 * Copyright 2025 NXP
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
 * Poll a session for dequeued buffers and events.
 */
void vhost_media_poll_session(struct vhost_user_media *m,
                              struct media_session *s);

/*
 * Ioctl code lookup.
 */
unsigned long virtio_media_code_to_ioctl(uint32_t code);

#endif /* VHOST_USER_MEDIA_H */
