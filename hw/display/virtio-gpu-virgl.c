/*
 * Virtio GPU Device
 *
 * Copyright Red Hat, Inc. 2013-2014
 *
 * Authors:
 *     Dave Airlie <airlied@redhat.com>
 *     Gerd Hoffmann <kraxel@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/iov.h"
#include "trace.h"
#include "hw/virtio/virtio.h"
#include "hw/virtio/virtio-gpu.h"
#include "hw/virtio/virtio-gpu-bswap.h"
#include "hw/virtio/virtio-iommu.h"
#include <epoxy/egl.h>

#include "ui/egl-helpers.h"

#include <virglrenderer.h>

static bool use_async_cb = false;
static bool use_per_ctx_fence = true;
static struct virgl_renderer_callbacks virtio_gpu_3d_cbs;
static struct virgl_renderer_callbacks virtio_gpu_3d_cbs_egl;

#if VIRGL_RENDERER_CALLBACKS_VERSION >= 4
static void *
virgl_get_egl_display(G_GNUC_UNUSED void *cookie)
{
    return qemu_egl_display;
}
#endif

static void virgl_cmd_create_resource_2d(VirtIOGPU *g,
                                         struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_resource_create_2d c2d;
    struct virgl_renderer_resource_create_args args;

    VIRTIO_GPU_FILL_CMD(c2d);
    trace_virtio_gpu_cmd_res_create_2d(c2d.resource_id, c2d.format,
                                       c2d.width, c2d.height);

    args.handle = c2d.resource_id;
    args.target = 2;
    args.format = c2d.format;
    args.bind = (1 << 1);
    args.width = c2d.width;
    args.height = c2d.height;
    args.depth = 1;
    args.array_size = 1;
    args.last_level = 0;
    args.nr_samples = 0;
    args.flags = VIRTIO_GPU_RESOURCE_FLAG_Y_0_TOP;
    virgl_renderer_resource_create(&args, NULL, 0);

    struct virtio_gpu_simple_resource *res = g_new0(struct virtio_gpu_simple_resource, 1);
    res->resource_id = c2d.resource_id;
    QTAILQ_INSERT_HEAD(&g->reslist, res, next);
}

static void virgl_cmd_create_resource_3d(VirtIOGPU *g,
                                         struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_resource_create_3d c3d;
    struct virgl_renderer_resource_create_args args;

    VIRTIO_GPU_FILL_CMD(c3d);
    trace_virtio_gpu_cmd_res_create_3d(c3d.resource_id, c3d.format,
                                       c3d.width, c3d.height, c3d.depth);

    args.handle = c3d.resource_id;
    args.target = c3d.target;
    args.format = c3d.format;
    args.bind = c3d.bind;
    args.width = c3d.width;
    args.height = c3d.height;
    args.depth = c3d.depth;
    args.array_size = c3d.array_size;
    args.last_level = c3d.last_level;
    args.nr_samples = c3d.nr_samples;
    args.flags = c3d.flags;
    virgl_renderer_resource_create(&args, NULL, 0);

    struct virtio_gpu_simple_resource *res = g_new0(struct virtio_gpu_simple_resource, 1);
    res->resource_id = c3d.resource_id;
    QTAILQ_INSERT_HEAD(&g->reslist, res, next);
}

static void virgl_cmd_resource_unref(VirtIOGPU *g,
                                     struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_resource_unref unref;
    struct iovec *res_iovs = NULL;
    int num_iovs = 0;

    VIRTIO_GPU_FILL_CMD(unref);
    trace_virtio_gpu_cmd_res_unref(unref.resource_id);

    virgl_renderer_resource_detach_iov(unref.resource_id,
                                       &res_iovs,
                                       &num_iovs);
    if (res_iovs != NULL && num_iovs != 0) {
        virtio_gpu_cleanup_mapping_iov(g, res_iovs, num_iovs);
    }
    virgl_renderer_resource_unref(unref.resource_id);
}

static void virgl_cmd_context_create(VirtIOGPU *g,
                                     struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_ctx_create cc;

    VIRTIO_GPU_FILL_CMD(cc);
    trace_virtio_gpu_cmd_ctx_create(cc.hdr.ctx_id,
                                    cc.debug_name);

    if (cc.context_init) {
        virgl_renderer_context_create_with_flags(cc.hdr.ctx_id,
                                                 cc.context_init,
                                                 cc.nlen,
                                                 cc.debug_name);
        return;
    }

    virgl_renderer_context_create(cc.hdr.ctx_id, cc.nlen, cc.debug_name);
}

static void virgl_cmd_context_destroy(VirtIOGPU *g,
                                      struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_ctx_destroy cd;

    VIRTIO_GPU_FILL_CMD(cd);
    trace_virtio_gpu_cmd_ctx_destroy(cd.hdr.ctx_id);

    virgl_renderer_context_destroy(cd.hdr.ctx_id);
}

static void virtio_gpu_rect_update(VirtIOGPU *g, int idx, int x, int y,
                                int width, int height)
{
    if (!g->parent_obj.scanout[idx].con) {
        return;
    }

    dpy_gl_update(g->parent_obj.scanout[idx].con, x, y, width, height);
}

static void virgl_cmd_resource_flush(VirtIOGPU *g,
                                     struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_resource_flush rf;
    int i;

    VIRTIO_GPU_FILL_CMD(rf);
    trace_virtio_gpu_cmd_res_flush(rf.resource_id,
                                   rf.r.width, rf.r.height, rf.r.x, rf.r.y);

    for (i = 0; i < g->parent_obj.conf.max_outputs; i++) {
        if (g->parent_obj.scanout[i].resource_id != rf.resource_id) {
            continue;
        }
        virtio_gpu_rect_update(g, i, rf.r.x, rf.r.y, rf.r.width, rf.r.height);
    }
}

static void virgl_cmd_set_scanout(VirtIOGPU *g,
                                  struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_set_scanout ss;
    int ret;

    VIRTIO_GPU_FILL_CMD(ss);
    trace_virtio_gpu_cmd_set_scanout(ss.scanout_id, ss.resource_id,
                                     ss.r.width, ss.r.height, ss.r.x, ss.r.y);

    if (ss.scanout_id >= g->parent_obj.conf.max_outputs) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: illegal scanout id specified %d",
                      __func__, ss.scanout_id);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT_ID;
        return;
    }
    g->parent_obj.enable = 1;

    if (ss.resource_id && ss.r.width && ss.r.height) {
        struct virgl_renderer_resource_info info;
        void *d3d_tex2d = NULL;

#ifdef HAVE_VIRGL_D3D_INFO_EXT
        struct virgl_renderer_resource_info_ext ext;
        memset(&ext, 0, sizeof(ext));
        ret = virgl_renderer_resource_get_info_ext(ss.resource_id, &ext);
        info = ext.base;
        d3d_tex2d = ext.d3d_tex2d;
#else
        memset(&info, 0, sizeof(info));
        ret = virgl_renderer_resource_get_info(ss.resource_id, &info);
#endif
        if (ret) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: illegal resource specified %d\n",
                          __func__, ss.resource_id);
            cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
            return;
        }
        qemu_console_resize(g->parent_obj.scanout[ss.scanout_id].con,
                            ss.r.width, ss.r.height);
        virgl_renderer_force_ctx_0();
        dpy_gl_scanout_texture(
            g->parent_obj.scanout[ss.scanout_id].con, info.tex_id,
            info.flags & VIRTIO_GPU_RESOURCE_FLAG_Y_0_TOP,
            info.width, info.height,
            ss.r.x, ss.r.y, ss.r.width, ss.r.height,
            d3d_tex2d);
    } else {
        dpy_gfx_replace_surface(
            g->parent_obj.scanout[ss.scanout_id].con, NULL);
        dpy_gl_scanout_disable(g->parent_obj.scanout[ss.scanout_id].con);
    }
    g->parent_obj.scanout[ss.scanout_id].resource_id = ss.resource_id;
}

static void virgl_cmd_submit_3d(VirtIOGPU *g,
                                struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_cmd_submit cs;
    void *buf;
    size_t s;

    VIRTIO_GPU_FILL_CMD(cs);
    trace_virtio_gpu_cmd_ctx_submit(cs.hdr.ctx_id, cs.size);

    buf = g_malloc(cs.size);
    s = iov_to_buf(cmd->elem.out_sg, cmd->elem.out_num,
                   sizeof(cs), buf, cs.size);
    if (s != cs.size) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: size mismatch (%zd/%d)",
                      __func__, s, cs.size);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
        goto out;
    }

    if (virtio_gpu_stats_enabled(g->parent_obj.conf)) {
        g->stats.req_3d++;
        g->stats.bytes_3d += cs.size;
    }

    virgl_renderer_submit_cmd(buf, cs.hdr.ctx_id, cs.size / 4);

out:
    g_free(buf);
}

static void virgl_cmd_transfer_to_host_2d(VirtIOGPU *g,
                                          struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_transfer_to_host_2d t2d;
    struct virtio_gpu_box box;

    VIRTIO_GPU_FILL_CMD(t2d);
    trace_virtio_gpu_cmd_res_xfer_toh_2d(t2d.resource_id);

    box.x = t2d.r.x;
    box.y = t2d.r.y;
    box.z = 0;
    box.w = t2d.r.width;
    box.h = t2d.r.height;
    box.d = 1;

    virgl_renderer_transfer_write_iov(t2d.resource_id,
                                      0,
                                      0,
                                      0,
                                      0,
                                      (struct virgl_box *)&box,
                                      t2d.offset, NULL, 0);
}

static void virgl_cmd_transfer_to_host_3d(VirtIOGPU *g,
                                          struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_transfer_host_3d t3d;

    VIRTIO_GPU_FILL_CMD(t3d);
    trace_virtio_gpu_cmd_res_xfer_toh_3d(t3d.resource_id);

    virgl_renderer_transfer_write_iov(t3d.resource_id,
                                      t3d.hdr.ctx_id,
                                      t3d.level,
                                      t3d.stride,
                                      t3d.layer_stride,
                                      (struct virgl_box *)&t3d.box,
                                      t3d.offset, NULL, 0);
}

static void
virgl_cmd_transfer_from_host_3d(VirtIOGPU *g,
                                struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_transfer_host_3d tf3d;

    VIRTIO_GPU_FILL_CMD(tf3d);
    trace_virtio_gpu_cmd_res_xfer_fromh_3d(tf3d.resource_id);

    virgl_renderer_transfer_read_iov(tf3d.resource_id,
                                     tf3d.hdr.ctx_id,
                                     tf3d.level,
                                     tf3d.stride,
                                     tf3d.layer_stride,
                                     (struct virgl_box *)&tf3d.box,
                                     tf3d.offset, NULL, 0);
}


static void virgl_resource_attach_backing(VirtIOGPU *g,
                                          struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_resource_attach_backing att_rb;
    struct iovec *res_iovs;
    uint32_t res_niov;
    int ret;

    VIRTIO_GPU_FILL_CMD(att_rb);
    trace_virtio_gpu_cmd_res_back_attach(att_rb.resource_id);

    ret = virtio_gpu_create_mapping_iov(g, att_rb.nr_entries, sizeof(att_rb),
                                        cmd, NULL, &res_iovs, &res_niov);
    if (ret != 0) {
        cmd->error = VIRTIO_GPU_RESP_ERR_UNSPEC;
        return;
    }

    ret = virgl_renderer_resource_attach_iov(att_rb.resource_id,
                                             res_iovs, res_niov);

    if (ret != 0)
        virtio_gpu_cleanup_mapping_iov(g, res_iovs, res_niov);
}

static void virgl_resource_detach_backing(VirtIOGPU *g,
                                          struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_resource_detach_backing detach_rb;
    struct iovec *res_iovs = NULL;
    int num_iovs = 0;

    VIRTIO_GPU_FILL_CMD(detach_rb);
    trace_virtio_gpu_cmd_res_back_detach(detach_rb.resource_id);

    virgl_renderer_resource_detach_iov(detach_rb.resource_id,
                                       &res_iovs,
                                       &num_iovs);
    if (res_iovs == NULL || num_iovs == 0) {
        return;
    }
    virtio_gpu_cleanup_mapping_iov(g, res_iovs, num_iovs);
}


static void virgl_cmd_ctx_attach_resource(VirtIOGPU *g,
                                          struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_ctx_resource att_res;

    VIRTIO_GPU_FILL_CMD(att_res);
    trace_virtio_gpu_cmd_ctx_res_attach(att_res.hdr.ctx_id,
                                        att_res.resource_id);

    virgl_renderer_ctx_attach_resource(att_res.hdr.ctx_id, att_res.resource_id);
}

static void virgl_cmd_ctx_detach_resource(VirtIOGPU *g,
                                          struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_ctx_resource det_res;

    VIRTIO_GPU_FILL_CMD(det_res);
    trace_virtio_gpu_cmd_ctx_res_detach(det_res.hdr.ctx_id,
                                        det_res.resource_id);

    virgl_renderer_ctx_detach_resource(det_res.hdr.ctx_id, det_res.resource_id);
}

static void virgl_cmd_get_capset_info(VirtIOGPU *g,
                                      struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_get_capset_info info;
    struct virtio_gpu_resp_capset_info resp;

    VIRTIO_GPU_FILL_CMD(info);

    memset(&resp, 0, sizeof(resp));
    if (info.capset_index == 0) {
        resp.capset_id = VIRTIO_GPU_CAPSET_VIRGL;
        virgl_renderer_get_cap_set(resp.capset_id,
                                   &resp.capset_max_version,
                                   &resp.capset_max_size);
    } else if (info.capset_index == 1) {
        resp.capset_id = VIRTIO_GPU_CAPSET_VIRGL2;
        virgl_renderer_get_cap_set(resp.capset_id,
                                   &resp.capset_max_version,
                                   &resp.capset_max_size);
    } else if (info.capset_index == 2) {
        resp.capset_id = VIRTIO_GPU_CAPSET_VENUS;
        virgl_renderer_get_cap_set(resp.capset_id,
                                   &resp.capset_max_version,
                                   &resp.capset_max_size);
    } else {
        resp.capset_max_version = 0;
        resp.capset_max_size = 0;
    }
    resp.hdr.type = VIRTIO_GPU_RESP_OK_CAPSET_INFO;
    virtio_gpu_ctrl_response(g, cmd, &resp.hdr, sizeof(resp));
}

static void virgl_cmd_get_capset(VirtIOGPU *g,
                                 struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_get_capset gc;
    struct virtio_gpu_resp_capset *resp;
    uint32_t max_ver, max_size;
    VIRTIO_GPU_FILL_CMD(gc);

    virgl_renderer_get_cap_set(gc.capset_id, &max_ver,
                               &max_size);
    if (!max_size) {
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
        return;
    }

    resp = g_malloc0(sizeof(*resp) + max_size);
    resp->hdr.type = VIRTIO_GPU_RESP_OK_CAPSET;
    virgl_renderer_fill_caps(gc.capset_id,
                             gc.capset_version,
                             (void *)resp->capset_data);
    virtio_gpu_ctrl_response(g, cmd, &resp->hdr, sizeof(*resp) + max_size);
    g_free(resp);
}

static void virgl_cmd_resource_create_blob(VirtIOGPU *g,
                                           struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_simple_resource *res;
    struct virtio_gpu_resource_create_blob cblob;
    int ret;

    VIRTIO_GPU_FILL_CMD(cblob);
    virtio_gpu_create_blob_bswap(&cblob);
    trace_virtio_gpu_cmd_res_create_blob(cblob.resource_id, cblob.size);

    if (cblob.resource_id == 0) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: resource id 0 is not allowed\n",
                      __func__);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
        return;
    }

    res = virtio_gpu_find_resource(g, cblob.resource_id);
    if (res) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: resource already exists %d\n",
                      __func__, cblob.resource_id);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
        return;
    }

    res = g_new0(struct virtio_gpu_simple_resource, 1);
    QTAILQ_INSERT_HEAD(&g->reslist, res, next);

    res->resource_id = cblob.resource_id;
    res->blob_size = cblob.size;

    if (cblob.blob_mem != VIRTIO_GPU_BLOB_MEM_HOST3D) {
        ret = virtio_gpu_create_mapping_iov(g, cblob.nr_entries, sizeof(cblob),
                                            cmd, &res->addrs, &res->iov,
                                            &res->iov_cnt);
        if (ret != 0) {
            cmd->error = VIRTIO_GPU_RESP_ERR_UNSPEC;
            return;
        }
    }

    if (cblob.blob_mem == VIRTIO_GPU_BLOB_MEM_GUEST) {
        virtio_gpu_init_udmabuf(res);
    }
    const struct virgl_renderer_resource_create_blob_args virgl_args = {
        .res_handle = cblob.resource_id,
        .ctx_id = cblob.hdr.ctx_id,
        .blob_mem = cblob.blob_mem,
        .blob_id = cblob.blob_id,
        .blob_flags = cblob.blob_flags,
        .size = cblob.size,
        .iovecs = res->iov,
        .num_iovs = res->iov_cnt,
    };
    ret = virgl_renderer_resource_create_blob(&virgl_args);
    if (ret) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: virgl blob create error: %s\n",
                      __func__, strerror(-ret));
        cmd->error = VIRTIO_GPU_RESP_ERR_UNSPEC;
    }
}

static void virgl_cmd_resource_map_blob(VirtIOGPU *g,
                                        struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_simple_resource *res;
    struct virtio_gpu_resource_map_blob mblob;
    int ret;
    uint64_t size;
    struct virtio_gpu_resp_map_info resp;

    VIRTIO_GPU_FILL_CMD(mblob);
    virtio_gpu_map_blob_bswap(&mblob);

    if (mblob.resource_id == 0) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: resource id 0 is not allowed\n",
                      __func__);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
        return;
    }

    res = virtio_gpu_find_resource(g, mblob.resource_id);
    if (!res) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: resource does not exist %d\n",
                      __func__, mblob.resource_id);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
        return;
    }

    ret = virgl_renderer_resource_map(res->resource_id, &res->mapped, &size);
    if (ret) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: resource map error: %s\n",
                      __func__, strerror(-ret));
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
        return;
    }

    memory_region_init_ram_device_ptr(&res->region, OBJECT(g), NULL, size, res->mapped);
    memory_region_add_subregion(&g->parent_obj.hostmem, mblob.offset, &res->region);
    memory_region_set_enabled(&res->region, true);

    memset(&resp, 0, sizeof(resp));
    resp.hdr.type = VIRTIO_GPU_RESP_OK_MAP_INFO;
    virgl_renderer_resource_get_map_info(mblob.resource_id, &resp.map_info);
    virtio_gpu_ctrl_response(g, cmd, &resp.hdr, sizeof(resp));
}

static void virgl_cmd_resource_unmap_blob(VirtIOGPU *g,
                                        struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_simple_resource *res;
    struct virtio_gpu_resource_unmap_blob ublob;
    VIRTIO_GPU_FILL_CMD(ublob);
    virtio_gpu_unmap_blob_bswap(&ublob);

    if (ublob.resource_id == 0) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: resource id 0 is not allowed\n",
                      __func__);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
        return;
    }

    res = virtio_gpu_find_resource(g, ublob.resource_id);
    if (!res) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: resource does not exist %d\n",
                      __func__, ublob.resource_id);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
        return;
    }

    virtio_gpu_virgl_resource_unmap(g, res);
}

void virtio_gpu_virgl_process_cmd(VirtIOGPU *g,
                                      struct virtio_gpu_ctrl_command *cmd)
{
    VIRTIO_GPU_FILL_CMD(cmd->cmd_hdr);

fprintf(stderr, "++++++ QEMU: virgl_process_cmd: 0x%x\n", cmd->cmd_hdr.type);
    virgl_renderer_force_ctx_0();
    switch (cmd->cmd_hdr.type) {
    case VIRTIO_GPU_CMD_CTX_CREATE:
        virgl_cmd_context_create(g, cmd);
        break;
    case VIRTIO_GPU_CMD_CTX_DESTROY:
        virgl_cmd_context_destroy(g, cmd);
        break;
    case VIRTIO_GPU_CMD_RESOURCE_CREATE_2D:
        virgl_cmd_create_resource_2d(g, cmd);
        break;
    case VIRTIO_GPU_CMD_RESOURCE_CREATE_3D:
        virgl_cmd_create_resource_3d(g, cmd);
        break;
    case VIRTIO_GPU_CMD_SUBMIT_3D:
        virgl_cmd_submit_3d(g, cmd);
        break;
    case VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D:
        virgl_cmd_transfer_to_host_2d(g, cmd);
        break;
    case VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D:
        virgl_cmd_transfer_to_host_3d(g, cmd);
        break;
    case VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D:
        virgl_cmd_transfer_from_host_3d(g, cmd);
        break;
    case VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING:
        virgl_resource_attach_backing(g, cmd);
        break;
    case VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING:
        virgl_resource_detach_backing(g, cmd);
        break;
    case VIRTIO_GPU_CMD_SET_SCANOUT:
        virgl_cmd_set_scanout(g, cmd);
        break;
    case VIRTIO_GPU_CMD_RESOURCE_FLUSH:
        virgl_cmd_resource_flush(g, cmd);
        break;
    case VIRTIO_GPU_CMD_RESOURCE_UNREF:
        virgl_cmd_resource_unref(g, cmd);
        break;
    case VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE:
        /* TODO add security */
        virgl_cmd_ctx_attach_resource(g, cmd);
        break;
    case VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE:
        /* TODO add security */
        virgl_cmd_ctx_detach_resource(g, cmd);
        break;
    case VIRTIO_GPU_CMD_RESOURCE_ASSIGN_UUID:
        virtio_gpu_resource_assign_uuid(g, cmd);
        break;
    case VIRTIO_GPU_CMD_GET_CAPSET_INFO:
        virgl_cmd_get_capset_info(g, cmd);
        break;
    case VIRTIO_GPU_CMD_GET_CAPSET:
        virgl_cmd_get_capset(g, cmd);
        break;
    case VIRTIO_GPU_CMD_GET_DISPLAY_INFO:
        virtio_gpu_get_display_info(g, cmd);
        break;
    case VIRTIO_GPU_CMD_GET_EDID:
        virtio_gpu_get_edid(g, cmd);
        break;
    case VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB:
        virgl_cmd_resource_create_blob(g, cmd);
        break;
    case VIRTIO_GPU_CMD_RESOURCE_MAP_BLOB:
        virgl_cmd_resource_map_blob(g, cmd);
        break;
    case VIRTIO_GPU_CMD_RESOURCE_UNMAP_BLOB:
        virgl_cmd_resource_unmap_blob(g, cmd);
        break;
    default:
        cmd->error = VIRTIO_GPU_RESP_ERR_UNSPEC;
        break;
    }

    if (cmd->finished) {
        return;
    }
    if (cmd->error) {
        fprintf(stderr, "%s: ctrl 0x%x, error 0x%x\n", __func__,
                cmd->cmd_hdr.type, cmd->error);
        virtio_gpu_ctrl_response_nodata(g, cmd, cmd->error);
        return;
    }
    if (!(cmd->cmd_hdr.flags & VIRTIO_GPU_FLAG_FENCE)) {
        virtio_gpu_ctrl_response_nodata(g, cmd, VIRTIO_GPU_RESP_OK_NODATA);
        return;
    }

    trace_virtio_gpu_fence_ctrl(cmd->cmd_hdr.fence_id, cmd->cmd_hdr.type);

    if (use_per_ctx_fence && (cmd->cmd_hdr.flags & VIRTIO_GPU_FLAG_INFO_RING_IDX)) {
        uint32_t flags = 0;

        virgl_renderer_context_create_fence(cmd->cmd_hdr.ctx_id, flags,
                                            cmd->cmd_hdr.ring_idx,
                                            cmd->cmd_hdr.fence_id);
    } else {
        virgl_renderer_create_fence(cmd->cmd_hdr.fence_id, 0);
    }
}

static int virtio_gpu_virgl_fence_read(VirtIOGPU *g, uint64_t *value)
{
    ssize_t ret;

    do {
        ret = read(g->read_pipe, value, sizeof(*value));
    } while ((ret == -1 && errno == EINTR));

    if (ret < 0) {
        if (errno != EAGAIN)
            error_report("%s: failed: %s", __func__, strerror(errno));
        return -errno;
    }

    return 0;
}

static int virtio_gpu_virgl_fence_write(VirtIOGPU *g, uint64_t value)
{
    ssize_t ret;

    do {
        ret = write(g->write_pipe, &value, sizeof(value));
    } while (ret < 0 && (errno == EINTR || errno == EAGAIN));

    if (ret < 0) {
        error_report("%s: failed: %s", __func__, strerror(errno));
        return ret;
    }

    return 0;
}

static void virtio_gpu_virgl_fence_event(void *opaque)
{
    struct virtio_gpu_ctrl_command *cmd;
    VirtIOGPU *g = opaque;
    uint64_t fence;

    QTAILQ_FOREACH(cmd, &g->fenceq, next) {
        if (!virtio_queue_ready(cmd->vq)) {
            return;
        }
    }

    while (!virtio_gpu_virgl_fence_read(g, &fence)) {
        QTAILQ_FOREACH(cmd, &g->fenceq, next) {
            /*
             * the guest can end up emitting fences out of order
             * so we should check all fenced cmds not just the first one.
             */
            if (cmd->cmd_hdr.fence_id > fence) {
                continue;
            }
            trace_virtio_gpu_fence_resp(cmd->cmd_hdr.fence_id);
            virtio_gpu_ctrl_response_nodata(g, cmd, VIRTIO_GPU_RESP_OK_NODATA);
            QTAILQ_REMOVE(&g->fenceq, cmd, next);
            g_free(cmd);
            g->inflight--;
            if (virtio_gpu_stats_enabled(g->parent_obj.conf)) {
                fprintf(stderr, "inflight: %3d (-)\r", g->inflight);
            }
        }
    }
}

struct context_fence {
    uint32_t ctx_id;
    uint64_t queue_id;
    uint64_t fence_id;
    uint32_t ctx_fence;
};

static int
virtio_gpu_virgl_write_context_fence(VirtIOGPU *g, uint32_t ctx_id,
                                     uint32_t queue_id, uint64_t fence_id,
                                     bool ctx_fence)
{
    struct context_fence *f = g_malloc(sizeof(*f));

    f->ctx_id = ctx_id;
    f->queue_id = queue_id;
    f->fence_id = fence_id;
    f->ctx_fence = ctx_fence;

    int err = virtio_gpu_virgl_fence_write(g, (uintptr_t)f);
    if (err) {
        g_free(f);
        return err;
    }

    return 0;
}

static int
virtio_gpu_virgl_read_context_fence(VirtIOGPU *g, uint32_t *ctx_id,
                                     uint64_t *queue_id, uint64_t *fence_id,
                                     uint32_t *ctx_fence)
{
    struct context_fence *f;
    uint64_t ptr;

    int err = virtio_gpu_virgl_fence_read(g, &ptr);
    if (err)
        return err;

    f = (void *)ptr;
    *ctx_id = f->ctx_id;
    *queue_id = f->queue_id;
    *fence_id = f->fence_id;
    *ctx_fence = f->ctx_fence;

    g_free(f);

    return 0;
}

static void virgl_write_fence_async(VirtIOGPU *g, uint32_t fence)
{
    if (use_per_ctx_fence)
        virtio_gpu_virgl_write_context_fence(g, 0, 0, fence, false);
    else
        virtio_gpu_virgl_fence_write(g, fence);
}

static void virgl_write_fence(void *opaque, uint32_t fence)
{
    VirtIOGPU *g = opaque;
    struct virtio_gpu_ctrl_command *cmd, *tmp;

    if (use_async_cb)
        return virgl_write_fence_async(g, fence);

    QTAILQ_FOREACH_SAFE(cmd, &g->fenceq, next, tmp) {
        /*
         * the guest can end up emitting fences out of order
         * so we should check all fenced cmds not just the first one.
         */
        if (cmd->cmd_hdr.fence_id > fence) {
            continue;
        }
        trace_virtio_gpu_fence_resp(cmd->cmd_hdr.fence_id);
        virtio_gpu_ctrl_response_nodata(g, cmd, VIRTIO_GPU_RESP_OK_NODATA);
        QTAILQ_REMOVE(&g->fenceq, cmd, next);
        g_free(cmd);
        g->inflight--;
        if (virtio_gpu_stats_enabled(g->parent_obj.conf)) {
            fprintf(stderr, "inflight: %3d (-)\r", g->inflight);
        }
    }
}

static void virtio_gpu_virgl_context_fence_event(void *opaque)
{
    struct virtio_gpu_ctrl_command *cmd;
    VirtIOGPU *g = opaque;
    uint64_t fence_id;
    uint64_t queue_id;
    uint32_t ctx_id;
    uint32_t ctx_fence;

    QTAILQ_FOREACH(cmd, &g->fenceq, next) {
        if (!virtio_queue_ready(cmd->vq)) {
            return;
        }
    }

    while (!virtio_gpu_virgl_read_context_fence(g, &ctx_id, &queue_id,
                                                &fence_id, &ctx_fence)) {
        QTAILQ_FOREACH(cmd, &g->fenceq, next) {
            /*
             * the guest can end up emitting fences out of order
             * so we should check all fenced cmds not just the first one.
             */
            if (cmd->cmd_hdr.fence_id > fence_id) {
                continue;
            }
            if (!!(cmd->cmd_hdr.flags & VIRTIO_GPU_FLAG_INFO_RING_IDX) ^ !!ctx_fence) {
                continue;
            }
            if (cmd->cmd_hdr.flags & VIRTIO_GPU_FLAG_INFO_RING_IDX) {
                if (cmd->cmd_hdr.ring_idx != queue_id) {
                    continue;
                }
                if (cmd->cmd_hdr.ctx_id != ctx_id) {
                    continue;
                }
            }
            trace_virtio_gpu_fence_resp(cmd->cmd_hdr.fence_id);
            virtio_gpu_ctrl_response_nodata(g, cmd, VIRTIO_GPU_RESP_OK_NODATA);
            QTAILQ_REMOVE(&g->fenceq, cmd, next);
            g_free(cmd);
            g->inflight--;
            if (virtio_gpu_stats_enabled(g->parent_obj.conf)) {
                fprintf(stderr, "inflight: %3d (-)\r", g->inflight);
            }
        }
    }
}

static void virgl_write_context_fence(void *opaque, uint32_t ctx_id,
                                      uint32_t queue_id, uint64_t fence_id)
{
    VirtIOGPU *g = opaque;

    virtio_gpu_virgl_write_context_fence(g, ctx_id, queue_id, fence_id, true);
}

static virgl_renderer_gl_context
virgl_create_context(void *opaque, int scanout_idx,
                     struct virgl_renderer_gl_ctx_param *params)
{
    VirtIOGPU *g = opaque;
    QEMUGLContext ctx;
    QEMUGLParams qparams;

    qparams.major_ver = params->major_ver;
    qparams.minor_ver = params->minor_ver;

    ctx = dpy_gl_ctx_create(g->parent_obj.scanout[scanout_idx].con, &qparams);
    return (virgl_renderer_gl_context)ctx;
}

static void virgl_destroy_context(void *opaque, virgl_renderer_gl_context ctx)
{
    VirtIOGPU *g = opaque;
    QEMUGLContext qctx = (QEMUGLContext)ctx;

    dpy_gl_ctx_destroy(g->parent_obj.scanout[0].con, qctx);
}

static int virgl_make_context_current(void *opaque, int scanout_idx,
                                      virgl_renderer_gl_context ctx)
{
    VirtIOGPU *g = opaque;
    QEMUGLContext qctx = (QEMUGLContext)ctx;

    return dpy_gl_ctx_make_current(g->parent_obj.scanout[scanout_idx].con,
                                   qctx);
}

static void *TMP_virgl_get_egl_display(void *opaque)
{
    return eglGetCurrentDisplay();
}

static struct virgl_renderer_callbacks virtio_gpu_3d_cbs = {
    .version             = 1,
    .write_fence         = virgl_write_fence,
    .create_gl_context   = virgl_create_context,
    .destroy_gl_context  = virgl_destroy_context,
    .make_current        = virgl_make_context_current,
};

static struct virgl_renderer_callbacks virtio_gpu_3d_cbs_egl = {
    .version             = 4,
    .write_fence         = virgl_write_fence,
    .write_context_fence = virgl_write_context_fence,
    .create_gl_context   = virgl_create_context,
    .destroy_gl_context  = virgl_destroy_context,
    .make_current        = virgl_make_context_current,
    .get_egl_display     = TMP_virgl_get_egl_display,
};

static void virtio_gpu_print_stats(void *opaque)
{
    VirtIOGPU *g = opaque;

    if (g->stats.requests) {
        fprintf(stderr, "stats: vq req %4d, %3d -- 3D %4d (%5d)\n",
                g->stats.requests,
                g->stats.max_inflight,
                g->stats.req_3d,
                g->stats.bytes_3d);
        g->stats.requests     = 0;
        g->stats.max_inflight = 0;
        g->stats.req_3d       = 0;
        g->stats.bytes_3d     = 0;
    } else {
        fprintf(stderr, "stats: idle\r");
    }
    timer_mod(g->print_stats, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 1000);
}

static void virtio_gpu_fence_poll(void *opaque)
{
    VirtIOGPU *g = opaque;

    virgl_renderer_poll();
    virtio_gpu_process_cmdq(g);
    if (!QTAILQ_EMPTY(&g->cmdq) || !QTAILQ_EMPTY(&g->fenceq)) {
        timer_mod(g->fence_poll, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 10);
    }
}

void virtio_gpu_virgl_fence_poll(VirtIOGPU *g)
{
    virtio_gpu_fence_poll(g);
}

void virtio_gpu_virgl_reset_scanout(VirtIOGPU *g)
{
    int i;

    for (i = 0; i < g->parent_obj.conf.max_outputs; i++) {
        dpy_gfx_replace_surface(g->parent_obj.scanout[i].con, NULL);
        dpy_gl_scanout_disable(g->parent_obj.scanout[i].con);
    }
}

void virtio_gpu_virgl_reset(VirtIOGPU *g)
{
    virgl_renderer_reset();
}

static int virtio_gpu_virgl_init_pipe(VirtIOGPU *g)
{
    int fds[2];
    int ret;

    if (!g_unix_open_pipe(fds, FD_CLOEXEC, NULL)) {
        return -errno;
    }
    if (!g_unix_set_fd_nonblocking(fds[0], true, NULL)) {
        ret = -errno;
        goto fail;
    }
    if (!g_unix_set_fd_nonblocking(fds[1], true, NULL)) {
        ret = -errno;
        goto fail;
    }
    g->read_pipe = fds[0];
    g->write_pipe = fds[1];

    return 0;

fail:
    close(fds[0]);
    close(fds[1]);
    return ret;
}

int virtio_gpu_virgl_init(VirtIOGPU *g)
{
    int ret;
    uint32_t flags = 0;
    struct virgl_renderer_callbacks *callbacks;
    const char *var = g_getenv("SDL_VIDEODRIVER");

#if VIRGL_RENDERER_CALLBACKS_VERSION >= 4
    if (qemu_egl_display) {
        virtio_gpu_3d_cbs.version = 4;
        virtio_gpu_3d_cbs.get_egl_display = virgl_get_egl_display;
    }
#endif
#ifdef VIRGL_RENDERER_D3D11_SHARE_TEXTURE
    if (qemu_egl_angle_d3d) {
        flags |= VIRGL_RENDERER_D3D11_SHARE_TEXTURE;
    }
#endif

#ifdef VIRGL_RENDERER_VENUS
    flags |= VIRGL_RENDERER_VENUS;
    flags |= VIRGL_RENDERER_RENDER_SERVER;
#endif

    /*
     * FIXME:
     * This might conflict with the code above
     * Which should be prefered?
     */
    if (eglGetCurrentDisplay())
        virtio_gpu_3d_cbs.get_egl_display = TMP_virgl_get_egl_display;

    if (use_async_cb)
        flags |= VIRGL_RENDERER_ASYNC_FENCE_CB;
    if (use_per_ctx_fence)
        flags |= VIRGL_RENDERER_THREAD_SYNC;

    if (var && !strcmp(var, "wayland"))
	    callbacks = &virtio_gpu_3d_cbs_egl;
    else
	    callbacks = &virtio_gpu_3d_cbs;

    ret = virgl_renderer_init(g, VIRGL_RENDERER_VENUS | VIRGL_RENDERER_RENDER_SERVER, callbacks);
    if (ret != 0) {
        error_report("virgl could not be initialized: %d", ret);
        return ret;
    }

    ret = virtio_gpu_virgl_init_pipe(g);
    if (ret != 0) {
        error_report("fence notifier could not be initialized: %d", ret);
        return ret;
    }

    if (use_per_ctx_fence)
        qemu_set_fd_handler(g->read_pipe, virtio_gpu_virgl_context_fence_event, NULL, g);
    else
        qemu_set_fd_handler(g->read_pipe, virtio_gpu_virgl_fence_event, NULL, g);

    g->fence_poll = timer_new_ms(QEMU_CLOCK_VIRTUAL,
                                 virtio_gpu_fence_poll, g);

    if (virtio_gpu_stats_enabled(g->parent_obj.conf)) {
        g->print_stats = timer_new_ms(QEMU_CLOCK_VIRTUAL,
                                      virtio_gpu_print_stats, g);
        timer_mod(g->print_stats, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 1000);
    }
    return 0;
}

int virtio_gpu_virgl_get_num_capsets(VirtIOGPU *g)
{
    uint32_t capset2_max_ver, capset2_max_size, num_capsets;
    num_capsets = 1;

    virgl_renderer_get_cap_set(VIRTIO_GPU_CAPSET_VIRGL2,
                               &capset2_max_ver,
                               &capset2_max_size);
    num_capsets += capset2_max_ver ? 1 : 0;

    virgl_renderer_get_cap_set(VIRTIO_GPU_CAPSET_VENUS,
                               &capset2_max_ver,
                               &capset2_max_size);
    num_capsets += capset2_max_size ? 1 : 0;

    return num_capsets;
}
