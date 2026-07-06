/*
 * Vhost-user-media virtio device
 *
 * Based on vhost-user-video.c
 * Copyright (c) 2021 Linaro Ltd
 * Copyright 2025 NXP
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/qdev-properties.h"
#include "hw/virtio/virtio-bus.h"
#include "hw/virtio/vhost-user-media.h"
#include "qemu/error-report.h"

static void vhost_user_media_start(VirtIODevice *vdev)
{
    VHostUserMEDIA *media = VHOST_USER_MEDIA(vdev);
    BusState *qbus = BUS(qdev_get_parent_bus(DEVICE(vdev)));
    VirtioBusClass *k = VIRTIO_BUS_GET_CLASS(qbus);
    int ret;
    int i;

    if (!k->set_guest_notifiers) {
        error_report("binding does not support guest notifiers");
        return;
    }

    ret = vhost_dev_enable_notifiers(&media->vhost_dev, vdev);
    if (ret < 0) {
        error_report("Error enabling host notifiers: %d", -ret);
        return;
    }

    ret = k->set_guest_notifiers(qbus->parent, media->vhost_dev.nvqs, true);
    if (ret < 0) {
        error_report("Error binding guest notifier: %d", -ret);
        goto err_host_notifiers;
    }

    media->vhost_dev.acked_features = vdev->guest_features;
    media->vhost_dev.vq_index_end = media->vhost_dev.nvqs;

    ret = vhost_dev_start(&media->vhost_dev, vdev, true);
    if (ret < 0) {
        error_report("Error starting vhost-user-media: %d", -ret);
        goto err_guest_notifiers;
    }

    for (i = 0; i < media->vhost_dev.nvqs; i++) {
        vhost_virtqueue_mask(&media->vhost_dev, vdev, i, false);
    }

    return;

err_guest_notifiers:
    k->set_guest_notifiers(qbus->parent, media->vhost_dev.nvqs, false);
err_host_notifiers:
    vhost_dev_disable_notifiers(&media->vhost_dev, vdev);
}

static void vhost_user_media_stop(VirtIODevice *vdev)
{
    VHostUserMEDIA *media = VHOST_USER_MEDIA(vdev);
    BusState *qbus = BUS(qdev_get_parent_bus(DEVICE(vdev)));
    VirtioBusClass *k = VIRTIO_BUS_GET_CLASS(qbus);
    int ret;

    if (!k->set_guest_notifiers) {
        return;
    }

    vhost_dev_stop(&media->vhost_dev, vdev, true);

    ret = k->set_guest_notifiers(qbus->parent, media->vhost_dev.nvqs, false);
    if (ret < 0) {
        error_report("vhost guest notifier cleanup failed: %d", ret);
        return;
    }

    vhost_dev_disable_notifiers(&media->vhost_dev, vdev);
}

static int vhost_user_media_set_status(VirtIODevice *vdev, uint8_t status)
{
    VHostUserMEDIA *media = VHOST_USER_MEDIA(vdev);
    bool should_start = virtio_device_should_start(vdev, status);

    if (vhost_dev_is_started(&media->vhost_dev) == should_start) {
        return 0;
    }

    if (should_start) {
        vhost_user_media_start(vdev);
    } else {
        vhost_user_media_stop(vdev);
    }

    return 0;
}

static const int user_feature_bits[] = {
    VIRTIO_F_VERSION_1,
    VIRTIO_RING_F_INDIRECT_DESC,
    VIRTIO_RING_F_EVENT_IDX,
    VIRTIO_F_NOTIFY_ON_EMPTY,
    VIRTIO_F_RING_PACKED,
    VIRTIO_F_IOMMU_PLATFORM,
    VIRTIO_F_RING_RESET,
    VIRTIO_F_IN_ORDER,
    VIRTIO_F_NOTIFICATION_DATA,
    VHOST_INVALID_FEATURE_BIT
};

static uint64_t vhost_user_media_get_features(VirtIODevice *vdev,
                                              uint64_t features, Error **errp)
{
    VHostUserMEDIA *media = VHOST_USER_MEDIA(vdev);

    return vhost_get_features(&media->vhost_dev, user_feature_bits, features);
}

static void vhost_user_media_get_config(VirtIODevice *vdev, uint8_t *config)
{
    VHostUserMEDIA *media = VHOST_USER_MEDIA(vdev);
    int ret;

    memset(config, 0, sizeof(struct virtio_media_config));

    ret = vhost_dev_get_config(&media->vhost_dev, config,
                               sizeof(struct virtio_media_config),
                               NULL);
    if (ret) {
        error_report("vhost-user-media: get config failed (%d)", ret);
    }
}

static bool
vhost_user_media_guest_notifier_pending(VirtIODevice *vdev, int idx)
{
    VHostUserMEDIA *media = VHOST_USER_MEDIA(vdev);

    /*
     * Add the check for configure interrupt, Use VIRTIO_CONFIG_IRQ_IDX -1
     * as the Marco of configure interrupt.s IDX, If this driver does not
     * support, the function will return
     */

    if (idx == VIRTIO_CONFIG_IRQ_IDX) {
        return false;
    }

    return vhost_virtqueue_pending(&media->vhost_dev, idx);
}

static void
vhost_user_media_guest_notifier_mask(VirtIODevice *vdev, int idx, bool mask)
{
    VHostUserMEDIA *media = VHOST_USER_MEDIA(vdev);

    /*
     * Add the check for configure interrupt, Use VIRTIO_CONFIG_IRQ_IDX -1
     * as the Marco of configure interrupt.s IDX, If this driver does not
     * support, the function will return
     */

    if (idx == VIRTIO_CONFIG_IRQ_IDX) {
        return;
    }

    vhost_virtqueue_mask(&media->vhost_dev, vdev, idx, mask);
}

static void do_vhost_user_cleanup(VirtIODevice *vdev, VHostUserMEDIA *media)
{
    vhost_user_cleanup(&media->vhost_user);
    virtio_delete_queue(media->command_vq);
    virtio_delete_queue(media->event_vq);
    virtio_cleanup(vdev);
    g_free(media->vhost_dev.vqs);
    media->vhost_dev.vqs = NULL;
}

static int vhost_user_media_handle_config_change(struct vhost_dev *dev)
{
    return 0;
}

const VhostDevConfigOps media_ops = {
    .vhost_dev_config_notifier = vhost_user_media_handle_config_change,
};

static int vhost_user_media_connect(DeviceState *dev)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(dev);
    VHostUserMEDIA *media = VHOST_USER_MEDIA(vdev);

    if (media->connected) {
        return 0;
    }
    media->connected = true;

    /* restore vhost state */
    if (virtio_device_started(vdev, vdev->status)) {
        vhost_user_media_start(vdev);
    }

    return 0;
}

static void vhost_user_media_disconnect(DeviceState *dev)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(dev);
    VHostUserMEDIA *media = VHOST_USER_MEDIA(vdev);

    if (!media->connected) {
        return;
    }
    media->connected = false;

    if (media->vhost_dev.started) {
        vhost_user_media_stop(vdev);
    }
}

static void vhost_user_media_event(void *opaque, QEMUChrEvent event)
{
    DeviceState *dev = opaque;
    VirtIODevice *vdev = VIRTIO_DEVICE(dev);
    VHostUserMEDIA *media = VHOST_USER_MEDIA(dev);

    switch (event) {
    case CHR_EVENT_OPENED:
        if (vhost_user_media_connect(dev) < 0) {
            qemu_chr_fe_disconnect(&media->conf.chardev);
            return;
        }
        break;

    case CHR_EVENT_CLOSED:
        vhost_user_media_disconnect(dev);
        break;

    default:
        break;
    }
}

static void vhost_user_media_handle_output(VirtIODevice *vdev, VirtQueue *vq)
{
    /* Nothing - handled by vhost-user backend */
}

static void vhost_user_media_device_realize(DeviceState *dev, Error **errp)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(dev);
    VHostUserMEDIA *media = VHOST_USER_MEDIA(dev);
    int ret;

    if (!media->conf.chardev.chr) {
        error_setg(errp, "missing chardev");
        return;
    }

    if (!vhost_user_init(&media->vhost_user, &media->conf.chardev, errp)) {
        return;
    }

    virtio_init(vdev, VIRTIO_ID_MEDIA, sizeof(struct virtio_media_config));

    /* one command queue and one event queue */
    media->vhost_dev.nvqs = 2;
    media->vhost_dev.vqs = g_new0(struct vhost_virtqueue,
                                  media->vhost_dev.nvqs);
    media->vhost_dev.vq_index = 0;

    vhost_dev_set_config_notifier(&media->vhost_dev, &media_ops);
    media->vhost_user.supports_config = true;

    ret = vhost_dev_init(&media->vhost_dev, &media->vhost_user,
                         VHOST_BACKEND_TYPE_USER, 0, errp);
    if (ret < 0) {
        error_report("vhost-user-media: vhost_dev_init failed: %s",
                     strerror(-ret));
        goto vhost_dev_init_failed;
    }

    media->command_vq = virtio_add_queue(vdev, 256,
                                         vhost_user_media_handle_output);
    if (!media->command_vq) {
        error_setg_errno(errp, -1, "virtio_add_queue() failed");
        goto cmd_q_fail;
    }

    media->event_vq = virtio_add_queue(vdev, 256,
                                       vhost_user_media_handle_output);
    if (!media->event_vq) {
        error_setg_errno(errp, -1, "virtio_add_queue() failed");
        goto event_q_fail;
    }

    qemu_chr_fe_set_handlers(&media->conf.chardev, NULL,
                             NULL, vhost_user_media_event,
                             NULL, (void *)dev, NULL, true);

    return;

event_q_fail:
    virtio_delete_queue(media->command_vq);
cmd_q_fail:
    vhost_dev_cleanup(&media->vhost_dev);
vhost_dev_init_failed:
    g_free(media->vhost_dev.vqs);
    media->vhost_dev.vqs = NULL;
    virtio_cleanup(vdev);
    vhost_user_cleanup(&media->vhost_user);
}

static void vhost_user_media_device_unrealize(DeviceState *dev)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(dev);
    VHostUserMEDIA *media = VHOST_USER_MEDIA(dev);

    vhost_user_media_set_status(vdev, 0);
    vhost_dev_cleanup(&media->vhost_dev);
    do_vhost_user_cleanup(vdev, media);
}

static const VMStateDescription vhost_user_media_vmstate = {
    .name = "vhost-user-media",
    .unmigratable = 1,
};

static const Property vhost_user_media_properties[] = {
    DEFINE_PROP_CHR("chardev", VHostUserMEDIA, conf.chardev),
    DEFINE_PROP_UINT64("hostmem", VHostUserMEDIA, conf.hostmem, 0),
};

static void vhost_user_media_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    VirtioDeviceClass *vdc = VIRTIO_DEVICE_CLASS(klass);

    device_class_set_props(dc, vhost_user_media_properties);
    dc->vmsd = &vhost_user_media_vmstate;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    vdc->realize = vhost_user_media_device_realize;
    vdc->unrealize = vhost_user_media_device_unrealize;
    vdc->get_features = vhost_user_media_get_features;
    vdc->get_config = vhost_user_media_get_config;
    vdc->set_status = vhost_user_media_set_status;
    vdc->guest_notifier_mask = vhost_user_media_guest_notifier_mask;
    vdc->guest_notifier_pending = vhost_user_media_guest_notifier_pending;
}

static const TypeInfo vhost_user_media_info = {
    .name = TYPE_VHOST_USER_MEDIA,
    .parent = TYPE_VIRTIO_DEVICE,
    .instance_size = sizeof(VHostUserMEDIA),
    .class_init = vhost_user_media_class_init,
};

static void vhost_user_media_register_types(void)
{
    type_register_static(&vhost_user_media_info);
}

type_init(vhost_user_media_register_types)
