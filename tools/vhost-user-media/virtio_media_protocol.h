/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Virtio-media protocol definitions for the vhost-user-media daemon.
 * Ported from the kernel driver (drivers/media/virtio/protocol.h).
 *
 * Copyright (c) 2024-2025 Google LLC.
 * Copyright 2025 NXP
 */

#ifndef VIRTIO_MEDIA_PROTOCOL_H
#define VIRTIO_MEDIA_PROTOCOL_H

#include <stdint.h>
#include <linux/videodev2.h>

/*
 * Command codes.
 */
#define VIRTIO_MEDIA_CMD_OPEN          1
#define VIRTIO_MEDIA_CMD_CLOSE         2
#define VIRTIO_MEDIA_CMD_IOCTL         3
#define VIRTIO_MEDIA_CMD_MMAP          4
#define VIRTIO_MEDIA_CMD_MUNMAP        5
#define VIRTIO_MEDIA_CMD_DMABUF_ATTACH 6
#define VIRTIO_MEDIA_CMD_DMABUF_DETACH 7

/*
 * Event codes.
 */
#define VIRTIO_MEDIA_EVT_ERROR  0
#define VIRTIO_MEDIA_EVT_DQBUF  1
#define VIRTIO_MEDIA_EVT_EVENT  2

/*
 * MMAP flags.
 */
#define VIRTIO_MEDIA_MMAP_FLAG_RW (1 << 0)

/*
 * Maximum number of planes per buffer.
 */
#define VIRTIO_MEDIA_MAX_PLANES VIDEO_MAX_PLANES

/*
 * Command/Response headers.
 */
struct virtio_media_cmd_header {
    uint32_t cmd;
    uint32_t __reserved;
};

struct virtio_media_resp_header {
    uint32_t status;
    uint32_t __reserved;
};

/*
 * OPEN command.
 */
struct virtio_media_cmd_open {
    struct virtio_media_cmd_header hdr;
};

struct virtio_media_resp_open {
    struct virtio_media_resp_header hdr;
    uint32_t session_id;
    uint32_t __reserved;
};

/*
 * CLOSE command (no response expected).
 */
struct virtio_media_cmd_close {
    struct virtio_media_cmd_header hdr;
    uint32_t session_id;
    uint32_t __reserved;
};

/*
 * IOCTL command.
 */
struct virtio_media_cmd_ioctl {
    struct virtio_media_cmd_header hdr;
    uint32_t session_id;
    uint32_t code;
};

struct virtio_media_resp_ioctl {
    struct virtio_media_resp_header hdr;
};

/*
 * SG entry for USERPTR buffers.
 */
struct virtio_media_sg_entry {
    uint64_t start;
    uint32_t len;
    uint32_t __reserved;
};

/*
 * MMAP command.
 */
struct virtio_media_cmd_mmap {
    struct virtio_media_cmd_header hdr;
    uint32_t session_id;
    uint32_t flags;
    uint32_t offset;
};

struct virtio_media_resp_mmap {
    struct virtio_media_resp_header hdr;
    uint32_t grant_ref_header; /* First grant ref */
    uint32_t grant_ref_count;  /* Number of grant refs (pages) */
    uint64_t len;              /* Buffer length in bytes */
};

/*
 * MUNMAP command.
 */
struct virtio_media_cmd_munmap {
    struct virtio_media_cmd_header hdr;
    uint32_t grant_ref_header; /* First grant ref to unmap */
    uint32_t grant_ref_count;  /* Number of grant refs */
};

struct virtio_media_resp_munmap {
    struct virtio_media_resp_header hdr;
};

/*
 * DMABUF_ATTACH command.
 *
 * For a V4L2_MEMORY_DMABUF queue the guest owns the buffers. Before a dma-buf
 * plane is first queued (or after it is rebound to a different dma-buf), the
 * guest grants the plane's pages to this (backend) domain and sends their
 * grant references with this command together with a guest-assigned
 * resource_id. The header is followed, in the device-readable part of the
 * chain, by a uint32_t refs[num_refs] array. The backend turns the grant
 * references back into a local dma-buf (xengnttab_dmabuf_exp_from_refs()) and
 * remembers it under resource_id; subsequent QBUF/PREPARE_BUF of the same
 * plane only carry resource_id (in v4l2_buffer m.fd / plane m.fd).
 */
struct virtio_media_cmd_dmabuf_attach {
    struct virtio_media_cmd_header hdr;
    uint32_t session_id;
    uint32_t resource_id;
    uint32_t num_refs;
    uint32_t __reserved;
};

struct virtio_media_resp_dmabuf_attach {
    struct virtio_media_resp_header hdr;
};

/*
 * DMABUF_DETACH command.
 *
 * Releases the local dma-buf the backend exported for resource_id, letting the
 * guest end the foreign grants. Sent on rebind, on a failed QBUF that would
 * have used the backing, or when the queue's buffers are freed (REQBUFS /
 * session close).
 */
struct virtio_media_cmd_dmabuf_detach {
    struct virtio_media_cmd_header hdr;
    uint32_t session_id;
    uint32_t resource_id;
};

struct virtio_media_resp_dmabuf_detach {
    struct virtio_media_resp_header hdr;
};

/*
 * Event structures.
 */
struct virtio_media_event_header {
    uint32_t event;
    uint32_t session_id;
};

struct virtio_media_event_error {
    struct virtio_media_event_header hdr;
    uint32_t error_code;
    uint32_t __reserved;
};

struct virtio_media_event_dqbuf {
    struct virtio_media_event_header hdr;
    struct v4l2_buffer buffer;
    struct v4l2_plane planes[VIRTIO_MEDIA_MAX_PLANES];
};

struct virtio_media_event_event {
    struct virtio_media_event_header hdr;
    struct v4l2_event event;
};

/*
 * Maximum size of an event payload.
 */
#define VIRTIO_MEDIA_EVENT_MAX_SIZE sizeof(struct virtio_media_event_dqbuf)

/*
 * Virtio device configuration area layout.
 * Offset 0: device_caps (u32) - V4L2 device capabilities
 * Offset 4: device_type (u32) - VFL_TYPE_*
 * Offset 8: card[32] - device card name
 */
struct virtio_media_config {
    uint32_t device_caps;
    uint32_t device_type;
    char card[32];
};

/*
 * SHM region ID for MMAP buffers.
 */
#define VIRTIO_MEDIA_SHM_MMAP 0

/*
 * Convert a VIDIOC_* ioctl number to the virtio-media ioctl code.
 * The code is the _IOC_NR part of the ioctl number.
 */
#define VIRTIO_MEDIA_IOCTL_CODE(ioctl) (((ioctl) >> _IOC_NRSHIFT) & _IOC_NRMASK)

#endif /* VIRTIO_MEDIA_PROTOCOL_H */
