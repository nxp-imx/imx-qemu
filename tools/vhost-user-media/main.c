/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Vhost-user-media main entry point.
 * Uses libvhost-user-glib to interface with QEMU.
 *
 * Copyright 2025-2026 NXP
 */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <linux/videodev2.h>
#include <glib.h>
#include <glib-unix.h>
#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>

#include <xengnttab.h>

#include "libvhost-user-glib.h"
#include "libvhost-user.h"

#include "vhost-user-media.h"

#define VHOST_USER_MEDIA_MAX_QUEUES 2

struct vu_media_device {
    VugDev dev;
    struct vhost_user_media media;
    GMainLoop *loop;

    /* Event queue state */
    VuVirtq *eventq;
    GList *event_bufs; /* Available event buffer descriptors */

    /*
     * Events produced while the guest's event virtqueue had no available
     * buffer. They must not be dropped (e.g. a DQBUF event carrying
     * V4L2_BUF_FLAG_LAST): losing one wedges the guest pipeline waiting for
     * EOS. Queued here in FIFO order and flushed once the guest replenishes
     * the eventq.
     */
    GQueue *pending_events;
};

/* A single event awaiting an available eventq buffer. */
struct pending_event {
    size_t len;
    uint8_t data[];
};

static struct vu_media_device vu_media;

/*
 * Try to deliver one event into an available eventq buffer.
 * Returns true if delivered, false if the eventq had no available buffer.
 */
static bool vu_media_deliver_event(struct vu_media_device *dev,
                                   const void *event, size_t event_len)
{
    VuDev *vudev = &dev->dev.parent;
    VuVirtq *vq = vu_get_queue(vudev, 1); /* eventq is queue index 1 */
    VuVirtqElement *elem;
    size_t written = 0;
    int i;

    /* Pop an available descriptor from the event queue */
    elem = vu_queue_pop(vudev, vq, sizeof(VuVirtqElement));
    if (!elem)
        return false;

    /* Write the event into the device-writable descriptor */
    for (i = 0; i < elem->in_num && written < event_len; i++) {
        size_t chunk = event_len - written;
        if (chunk > elem->in_sg[i].iov_len)
            chunk = elem->in_sg[i].iov_len;
        memcpy(elem->in_sg[i].iov_base, (const uint8_t *)event + written,
               chunk);
        written += chunk;
    }

    /* Return the descriptor to the guest */
    vu_queue_push(vudev, vq, elem, written);
    vu_queue_notify(vudev, vq);
    g_free(elem);
    return true;
}

/*
 * Flush events that were queued while the eventq had no available buffer.
 * Delivers in FIFO order and stops as soon as the eventq runs dry again so
 * ordering is preserved.
 */
static void vu_media_flush_pending_events(struct vu_media_device *dev)
{
    while (!g_queue_is_empty(dev->pending_events)) {
        struct pending_event *pe = g_queue_peek_head(dev->pending_events);

        if (!vu_media_deliver_event(dev, pe->data, pe->len))
            break; /* still no buffer; keep the rest queued in order */

        g_queue_pop_head(dev->pending_events);
        g_free(pe);
    }
}

/*
 * Send an event to the guest via the event virtqueue.
 *
 * Events must never be dropped: losing a DQBUF event that carries
 * V4L2_BUF_FLAG_LAST wedges the guest pipeline waiting for EOS. If the eventq
 * has no available buffer (or events are already queued and must stay in
 * order), stash the event and flush it once the guest replenishes the eventq.
 */
static void vu_media_send_event(struct vhost_user_media *m,
                                const void *event, size_t event_len)
{
    struct vu_media_device *dev = &vu_media;

    /* Preserve order: if events are already pending, this one goes behind. */
    if (g_queue_is_empty(dev->pending_events) &&
        vu_media_deliver_event(dev, event, event_len))
        return;

    struct pending_event *pe = g_malloc(sizeof(*pe) + event_len);
    pe->len = event_len;
    memcpy(pe->data, event, event_len);
    g_queue_push_tail(dev->pending_events, pe);

    /* The guest may already have buffers waiting; try to drain now. */
    vu_media_flush_pending_events(dev);
}

/*
 * Handle a command from the command virtqueue (queue index 0).
 */
static void vu_media_handle_commandq(VuDev *vudev, int qidx)
{
    VuVirtq *vq = vu_get_queue(vudev, qidx);
    VuVirtqElement *elem;

    while ((elem = vu_queue_pop(vudev, vq, sizeof(VuVirtqElement)))) {
        /* Gather the device-readable (command) data */
        uint8_t cmd_buf[8192];
        size_t cmd_len = 0;
        int i;

        for (i = 0; i < elem->out_num && cmd_len < sizeof(cmd_buf); i++) {
            size_t chunk = elem->out_sg[i].iov_len;
            if (cmd_len + chunk > sizeof(cmd_buf))
                chunk = sizeof(cmd_buf) - cmd_len;
            memcpy(cmd_buf + cmd_len, elem->out_sg[i].iov_base, chunk);
            cmd_len += chunk;
        }

        /* Prepare response buffer */
        uint8_t resp_buf[8192];
        size_t resp_len = 0;
        size_t resp_buf_size = sizeof(resp_buf);

        /* Also calculate available device-writable space */
        size_t in_total = 0;
        for (i = 0; i < elem->in_num; i++)
            in_total += elem->in_sg[i].iov_len;
        if (resp_buf_size > in_total)
            resp_buf_size = in_total;

        /* Process the command */
        memset(resp_buf, 0, resp_buf_size);
        /*
         * Pick up the DomU domid negotiated over vhost-user. It is filled
         * in by libvhost-user-xen when the guest memory table is set, well
         * before any MMAP command, and reflects the real domain id even
         * across DomU recreations.
         */
        if (vudev->xen_domainid)
            vu_media.media.domu_domid = vudev->xen_domainid;
        /*
         * Let the command layer reach the VuDev for the shared-object APIs
         * (vu_add/rm_shared_object in handle_cmd_mmap / cleanup paths).
         */
        vu_media.media.opaque = vudev;
        vhost_media_process_command(&vu_media.media, cmd_buf, cmd_len,
                                    resp_buf, resp_buf_size, &resp_len);

        /* Write response back to device-writable descriptors */
        size_t written = 0;
        for (i = 0; i < elem->in_num && written < resp_len; i++) {
            size_t chunk = resp_len - written;
            if (chunk > elem->in_sg[i].iov_len)
                chunk = elem->in_sg[i].iov_len;
            memcpy(elem->in_sg[i].iov_base, resp_buf + written, chunk);
            written += chunk;
        }

        vu_queue_push(vudev, vq, elem, written);
        vu_queue_notify(vudev, vq);
        g_free(elem);
    }
}

/*
 * Handle the event queue (queue index 1).
 * The guest sends us empty buffers that we fill with events later. When it
 * replenishes the eventq, flush any events that were stashed because no buffer
 * was available at the time they were produced.
 */
static void vu_media_handle_eventq(VuDev *vudev, int qidx)
{
    vu_media_flush_pending_events(&vu_media);
}

static void vu_media_queue_set_started(VuDev *vudev, int qidx, bool started)
{
    g_debug("Queue %d started=%d", qidx, started);

    if (qidx == 0) {
        vu_set_queue_handler(vudev, vu_get_queue(vudev, 0),
                             started ? vu_media_handle_commandq : NULL);
    } else if (qidx == 1) {
        vu_set_queue_handler(vudev, vu_get_queue(vudev, 1),
                             started ? vu_media_handle_eventq : NULL);
    }
}

static uint64_t vu_media_get_features(VuDev *vudev)
{
    return 0;
}

static void vu_media_set_features(VuDev *vudev, uint64_t features)
{
}

static int vu_media_get_config(VuDev *vudev, uint8_t *config, uint32_t len)
{
    struct virtio_media_config cfg;
    struct v4l2_capability cap;
    int fd;

    memset(&cfg, 0, sizeof(cfg));

    /* Query the V4L2 device capabilities */
    fd = open(vu_media.media.v4l2_device, O_RDWR);
    if (fd >= 0) {
        if (ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0) {
            cfg.device_caps = cap.device_caps;
            cfg.device_type = 0; /* VFL_TYPE_VIDEO */
            strncpy(cfg.card, (char *)cap.card, sizeof(cfg.card) - 1);
        }
        close(fd);
    }

    if (len > sizeof(cfg))
        len = sizeof(cfg);
    memcpy(config, &cfg, len);

    return 0;
}

static int vu_media_set_config(VuDev *vudev, const uint8_t *data,
                               uint32_t offset, uint32_t size,
                               uint32_t flags)
{
    /*
     * The virtio-media config space (device_caps/device_type/card) is
     * read-only and derived from the host V4L2 device. There is nothing
     * for the guest to set, so just accept the write.
     *
     * Note: libvhost-user only advertises VHOST_USER_PROTOCOL_F_CONFIG
     * when both get_config and set_config are provided, and the QEMU
     * frontend requires that feature when supports_config is enabled.
     */
    return 0;
}

static void vu_media_panic(VuDev *vudev, const char *msg)
{
    g_critical("vhost-user-media panic: %s", msg);
    if (vu_media.loop)
        g_main_loop_quit(vu_media.loop);
}

/*
 * Advertise VHOST_USER_PROTOCOL_F_SHARED_OBJECT on top of the libvhost-user
 * defaults. Required so QEMU forwards BACKEND_SHARED_OBJECT_ADD/REMOVE/LOOKUP
 * and GET_SHARED_OBJECT to/from this daemon (host-object model: host pages +
 * UUID). F_BACKEND_REQ, which carries these backend-channel messages, is
 * already in the libvhost-user default set.
 */
static uint64_t vu_media_get_protocol_features(VuDev *vudev)
{
    return 1ULL << VHOST_USER_PROTOCOL_F_SHARED_OBJECT;
}

/*
 * Producer side of the host-object handoff: QEMU asks us (via
 * GET_SHARED_OBJECT) for the fd behind a UUID we registered with
 * vu_add_shared_object when the host buffer was exported. Return the owning
 * entry's dma_fd; libvhost-user duplicates it over SCM_RIGHTS to the
 * requesting consumer daemon, so we must NOT close it here (its lifetime is
 * the MMAP entry's). Returns -1 if no live buffer carries this UUID.
 */
static int vu_media_get_shared_object(VuDev *vudev, const unsigned char *uuid)
{
    int fd = vhost_media_lookup_shared_object(&vu_media.media, uuid);

    if (fd < 0)
        g_warning("get_shared_object: no live buffer for requested UUID");
    return fd;
}

static const VuDevIface vuiface = {
    .get_features = vu_media_get_features,
    .set_features = vu_media_set_features,
    .queue_set_started = vu_media_queue_set_started,
    .get_config = vu_media_get_config,
    .set_config = vu_media_set_config,
    .get_protocol_features = vu_media_get_protocol_features,
    .get_shared_object = vu_media_get_shared_object,
};

/*
 * GLib source to poll V4L2 session fds for events.
 */
static gboolean
session_poll_callback(gint fd, GIOCondition condition,
                                      gpointer user_data)
{
    struct media_session *s = user_data;

    vhost_media_poll_session(&vu_media.media, s, condition);
    return G_SOURCE_CONTINUE;
}

/*
 * Register a session's V4L2 fd for polling in the GLib main loop.
 */
static void vu_media_register_poll(struct vhost_user_media *m,
                                   struct media_session *s)
{
    s->poll_source_id = g_unix_fd_add(s->v4l2_fd,
                                      G_IO_IN | G_IO_OUT | G_IO_PRI |
                                      G_IO_ERR | G_IO_HUP,
                                      session_poll_callback, s);
    g_debug("Registered poll for session %u (fd=%d, source=%u)",
            s->id, s->v4l2_fd, s->poll_source_id);
}

static gboolean hangup(gpointer data)
{
    GMainLoop *loop = data;
    g_main_loop_quit(loop);
    return TRUE;
}

int main(int argc, char *argv[])
{
    g_autoptr(GError) error = NULL;
    g_autoptr(GSocket) socket = NULL;
    char *socket_path = NULL;
    char *v4l2_device = NULL;
    int socket_fd = -1;

    GOptionEntry entries[] = {
        { "socket-path", 's', 0, G_OPTION_ARG_STRING, &socket_path,
          "Unix socket path", "PATH" },
        { "socket-fd", 'f', 0, G_OPTION_ARG_INT, &socket_fd,
          "Unix socket fd", "FD" },
        { "v4l2-device", 'd', 0, G_OPTION_ARG_STRING, &v4l2_device,
          "V4L2 device path", "PATH" },
        { NULL }
    };

    g_autoptr(GOptionContext) context =
        g_option_context_new("- vhost-user-media daemon");
    g_option_context_add_main_entries(context, entries, NULL);
    if (!g_option_context_parse(context, &argc, &argv, &error)) {
        g_printerr("Option parsing failed: %s\n", error->message);
        exit(EXIT_FAILURE);
    }

    if (!v4l2_device) {
        g_printerr("Must specify --v4l2-device\n");
        exit(EXIT_FAILURE);
    }

    if (!socket_path && socket_fd < 0) {
        g_printerr("Must specify --socket-path or --socket-fd\n");
        exit(EXIT_FAILURE);
    }

    /* Initialize the media device state */
    memset(&vu_media, 0, sizeof(vu_media));
    vu_media.media.v4l2_device = v4l2_device;
    vu_media.media.send_event = vu_media_send_event;
    vu_media.media.register_poll = vu_media_register_poll;

    vu_media.pending_events = g_queue_new();

    /* Open Xen grant device for dma-buf import/export */
    vu_media.media.gntdev_handle = xengnttab_open(NULL, 0);
    if (!vu_media.media.gntdev_handle) {
        g_printerr("Failed to open xengnttab device\n");
        exit(EXIT_FAILURE);
    }

    /* Set up the socket */
    if (socket_path) {
        g_autoptr(GSocketAddress) addr =
            g_unix_socket_address_new(socket_path);
        g_autoptr(GSocket) bind_socket =
            g_socket_new(G_SOCKET_FAMILY_UNIX, G_SOCKET_TYPE_STREAM,
                         G_SOCKET_PROTOCOL_DEFAULT, &error);

        if (!g_socket_bind(bind_socket, addr, false, &error)) {
            g_printerr("Failed to bind to %s: %s\n",
                       socket_path, error->message);
            exit(EXIT_FAILURE);
        }
        if (!g_socket_listen(bind_socket, &error)) {
            g_printerr("Failed to listen on %s: %s\n",
                       socket_path, error->message);
            exit(EXIT_FAILURE);
        }
        g_message("Awaiting connection on %s", socket_path);
        socket = g_socket_accept(bind_socket, NULL, &error);
        if (!socket) {
            g_printerr("Failed to accept: %s\n", error->message);
            exit(EXIT_FAILURE);
        }
    } else {
        socket = g_socket_new_from_fd(socket_fd, &error);
        if (!socket) {
            g_printerr("Failed to create socket from fd %d: %s\n",
                       socket_fd, error->message);
            exit(EXIT_FAILURE);
        }
    }

    /* Create main loop */
    vu_media.loop = g_main_loop_new(NULL, FALSE);
    g_unix_signal_add(SIGHUP, hangup, vu_media.loop);
    g_unix_signal_add(SIGINT, hangup, vu_media.loop);

    /* Initialize vhost-user-glib */
    if (!vug_init(&vu_media.dev, VHOST_USER_MEDIA_MAX_QUEUES,
                  g_socket_get_fd(socket), vu_media_panic, &vuiface)) {
        g_printerr("Failed to initialize libvhost-user-glib\n");
        exit(EXIT_FAILURE);
    }

    g_message("vhost-user-media: device=%s, entering main loop", v4l2_device);

    g_main_loop_run(vu_media.loop);

    g_message("Main loop exited, cleaning up");
    g_main_loop_unref(vu_media.loop);

    /* Cleanup sessions */
    while (vu_media.media.sessions) {
        struct media_session *s = vu_media.media.sessions->data;
        vhost_media_destroy_session(&vu_media.media, s);
    }

    /* Drain and free the grant-release worker pool (waits for pending jobs) */
    if (vu_media.media.grant_release_pool)
        g_thread_pool_free(vu_media.media.grant_release_pool, FALSE, TRUE);

    /* Close Xen grant device */
    if (vu_media.media.gntdev_handle)
        xengnttab_close(vu_media.media.gntdev_handle);

    return 0;
}
