/*
 * vhost-user-media virtio device
 *
 * Copyright 2025 NXP
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef _VHOST_USER_MEDIA_H_
#define _VHOST_USER_MEDIA_H_

#include "hw/virtio/virtio.h"
#include "hw/virtio/vhost.h"
#include "hw/virtio/vhost-user.h"
#include "chardev/char-fe.h"

#define VIRTIO_ID_MEDIA 48

#define TYPE_VHOST_USER_MEDIA "vhost-user-media-device"
#define VHOST_USER_MEDIA(obj) \
        OBJECT_CHECK(VHostUserMEDIA, (obj), TYPE_VHOST_USER_MEDIA)

/* SHM region ID for MMAP buffers */
#define VIRTIO_MEDIA_SHM_ID 0

/* Configuration area layout */
struct virtio_media_config {
    uint32_t device_caps;
    uint32_t device_type;
    char card[32];
};

typedef struct {
    CharFrontend chardev;
    uint64_t hostmem;  /* Size of the SHM region for MMAP buffers */
} VHostUserMEDIAConf;

typedef struct {
    /*< private >*/
    VirtIODevice parent;
    VHostUserMEDIAConf conf;
    struct vhost_dev vhost_dev;
    VhostUserState vhost_user;
    VirtQueue *command_vq;
    VirtQueue *event_vq;
    bool connected;
    /*< public >*/
} VHostUserMEDIA;

#define TYPE_VHOST_USER_MEDIA_PCI "vhost-user-media-pci-base"

#endif /* _VHOST_USER_MEDIA_H_ */
