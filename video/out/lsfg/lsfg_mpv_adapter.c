#include "video/out/lsfg/lsfg_mpv_adapter.h"

#include "common/common.h"
#include "common/msg.h"
#include "ta/ta_talloc.h"
#include "video/mp_image.h"
#include "video/out/lsfg/lsfg.h"

struct mpv_lsfg_adapter {
    struct mp_lsfg_ctx *core;

    pl_tex prev_rgb;
    pl_tex cur_rgb;
    pl_tex gen_rgb;
    pl_fmt work_fmt;

    bool prev_valid;
    uint64_t prev_frame_id;
    bool gen_valid;
    uint64_t gen_frame_id;
    uint32_t gen_phase_index;
    bool hard_failed;

    struct voctrl_lsfg_stats stats;
};

static void destroy_targets(struct mpv_lsfg_adapter *a, pl_gpu gpu)
{
    if (!a || !gpu)
        return;
    pl_tex_destroy(gpu, &a->prev_rgb);
    pl_tex_destroy(gpu, &a->cur_rgb);
    pl_tex_destroy(gpu, &a->gen_rgb);
    a->work_fmt = NULL;
    a->prev_valid = false;
    a->prev_frame_id = 0;
    a->gen_valid = false;
    a->gen_frame_id = 0;
    a->gen_phase_index = 0;
}

static bool create_target(pl_gpu gpu, int w, int h, pl_fmt fmt, bool storable, pl_tex *out)
{
    pl_tex_destroy(gpu, out);
    *out = pl_tex_create(gpu, pl_tex_params(
        .w = w,
        .h = h,
        .format = fmt,
        .sampleable = true,
        .renderable = true,
        .storable = storable,
        .blit_src = true,
        .blit_dst = true,
    ));
    return *out != NULL;
}

static bool format_has_caps(pl_fmt fmt, enum pl_fmt_caps caps)
{
    return fmt && ((fmt->caps & caps) == caps);
}

static bool compute_format_supported(pl_fmt fmt)
{
    if (!fmt || !fmt->num_components)
        return false;

    int max_depth = 0;
    for (int i = 0; i < fmt->num_components; i++)
        max_depth = MPMAX(max_depth, fmt->component_depth[i]);

    switch (fmt->type) {
    case PL_FMT_UNORM:
        return (fmt->num_components == 1 || fmt->num_components == 2 ||
                fmt->num_components == 4) &&
               (max_depth == 8 || max_depth == 16);
    case PL_FMT_FLOAT:
        return (fmt->num_components == 1 || fmt->num_components == 2 ||
                fmt->num_components == 4) &&
               (max_depth == 16 || max_depth == 32);
    case PL_FMT_UINT:
        return (fmt->num_components == 1 || fmt->num_components == 2 ||
                fmt->num_components == 4) &&
               (max_depth == 8 || max_depth == 16 || max_depth == 32);
    default:
        return false;
    }
}

static bool blit_compatible(pl_fmt src, pl_fmt dst)
{
    if (!src || !dst)
        return false;

    if (src->internal_size != dst->internal_size)
        return false;

    if (src->type == PL_FMT_UINT || dst->type == PL_FMT_UINT)
        return src->type == dst->type;
    if (src->type == PL_FMT_SINT || dst->type == PL_FMT_SINT)
        return src->type == dst->type;

    return true;
}

static pl_fmt pick_work_format(struct mpv_lsfg_adapter *a, pl_gpu gpu, pl_tex ref)
{
    enum pl_fmt_caps req = PL_FMT_CAP_SAMPLEABLE |
                           PL_FMT_CAP_RENDERABLE |
                           PL_FMT_CAP_STORABLE |
                           PL_FMT_CAP_BLITTABLE;

    pl_fmt ref_fmt = ref->params.format;
    if (format_has_caps(ref_fmt, req) && compute_format_supported(ref_fmt))
        return ref_fmt;

    for (int i = 0; i < gpu->num_formats; i++) {
        pl_fmt cand = gpu->formats[i];
        if (!format_has_caps(cand, req))
            continue;
        if (!compute_format_supported(cand))
            continue;
        if (cand->type != ref_fmt->type)
            continue;
        if (cand->num_components != ref_fmt->num_components)
            continue;
        if (cand->internal_size != ref_fmt->internal_size)
            continue;
        return cand;
    }

    int min_depth = 0;
    for (int i = 0; i < ref_fmt->num_components; i++)
        min_depth = MPMAX(min_depth, ref_fmt->component_depth[i]);
    min_depth = MPMAX(min_depth, 8);

    pl_fmt fmt = pl_find_fmt(gpu, PL_FMT_UNORM, 4, 8, 8, req);
    if (fmt && compute_format_supported(fmt))
        return fmt;

    fmt = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 16, 0, req);
    if (fmt && compute_format_supported(fmt))
        return fmt;

    fmt = pl_find_fmt(gpu, ref_fmt->type, ref_fmt->num_components,
                      min_depth, 0, req);
    if (fmt && compute_format_supported(fmt))
        return fmt;

    return NULL;
}

static bool ensure_targets(struct mpv_lsfg_adapter *a, pl_gpu gpu,
                           pl_tex fmt_ref, int w, int h)
{
    pl_fmt work_fmt = pick_work_format(a, gpu, fmt_ref);
    if (!work_fmt)
        return false;

    if (a->prev_rgb &&
        a->cur_rgb &&
        a->gen_rgb &&
        a->prev_rgb->params.w == w &&
        a->prev_rgb->params.h == h &&
        a->prev_rgb->params.format == work_fmt)
    {
        return true;
    }

    destroy_targets(a, gpu);

    if (!create_target(gpu, w, h, work_fmt, false, &a->prev_rgb))
        return false;
    if (!create_target(gpu, w, h, work_fmt, false, &a->cur_rgb))
        return false;
    if (!create_target(gpu, w, h, work_fmt, true, &a->gen_rgb))
        return false;

    a->work_fmt = work_fmt;
    return true;
}

static struct pl_frame target_frame_from_tex(pl_tex tex, const struct pl_frame *target_ref,
                                             int ref_w, int ref_h, bool include_overlays)
{
    struct pl_frame target = *target_ref;
    if (ref_w > 0 && ref_h > 0) {
        float sx = (float)tex->params.w / (float)ref_w;
        float sy = (float)tex->params.h / (float)ref_h;
        target.crop.x0 = target_ref->crop.x0 * sx;
        target.crop.y0 = target_ref->crop.y0 * sy;
        target.crop.x1 = target_ref->crop.x1 * sx;
        target.crop.y1 = target_ref->crop.y1 * sy;
    } else {
        target.crop = (struct pl_rect2df) {
            .x0 = 0.0f,
            .y0 = 0.0f,
            .x1 = tex->params.w,
            .y1 = tex->params.h,
        };
    }
    if (!include_overlays) {
        target.num_overlays = 0;
        target.overlays = NULL;
    }
    target.repr.sys = PL_COLOR_SYSTEM_RGB;
    target.num_planes = 1;
    target.planes[0] = (struct pl_plane) {
        .texture = tex,
        .components = 4,
        .component_mapping = {0, 1, 2, 3},
    };
    return target;
}

static void blit_full(pl_gpu gpu, pl_tex src, pl_tex dst)
{
    bool same_size = src->params.w == dst->params.w &&
                     src->params.h == dst->params.h;
    struct pl_rect3d src_rc = {
        .x1 = src->params.w,
        .y1 = src->params.h,
    };
    struct pl_rect3d dst_rc = {
        .x1 = dst->params.w,
        .y1 = dst->params.h,
    };

    pl_tex_blit(gpu, &(struct pl_tex_blit_params) {
        .src = src,
        .dst = dst,
        .src_rc = src_rc,
        .dst_rc = dst_rc,
        .sample_mode = same_size ? PL_TEX_SAMPLE_NEAREST : PL_TEX_SAMPLE_LINEAR,
    });
}

static bool present_to_swapchain(pl_gpu gpu, pl_renderer rr, pl_tex src,
                                 const struct pl_frame *target_ref, pl_tex dst)
{
    bool copied = false;
    bool can_blit_copy = blit_compatible(src->params.format, dst->params.format);

    if (can_blit_copy) {
        blit_full(gpu, src, dst);
        copied = true;
    } else {
        struct pl_frame src_frame = target_frame_from_tex(src, target_ref,
                                                          dst->params.w, dst->params.h, false);
        struct pl_render_params copy_params = pl_render_default_params;
        copy_params.skip_anti_aliasing = true;
        copied = pl_render_image(rr, &src_frame, target_ref, &copy_params);
    }

    if (!copied)
        return false;

    if (target_ref->num_overlays > 0) {
        struct pl_render_params overlay_params = pl_render_default_params;
        overlay_params.background = PL_CLEAR_SKIP;
        overlay_params.border = PL_CLEAR_SKIP;
        overlay_params.skip_anti_aliasing = true;
        if (!pl_render_image(rr, NULL, target_ref, &overlay_params))
            return false;
    }

    return true;
}

static void fail_strict(struct mpv_lsfg_adapter *a, struct vo *vo,
                        pl_gpu gpu, const char *reason)
{
    if (!a->hard_failed) {
        MP_ERR(vo, "lsfg strict: disabling lsfg after fatal error (%s)\n",
               reason ? reason : "unknown");
    }
    a->hard_failed = true;
    mp_lsfg_on_reset(a->core);
    destroy_targets(a, gpu);
}

static void pick_processing_size(struct vo *vo, struct vo_frame *frame, int processing_res,
                                 pl_tex swap_tex, int *out_w, int *out_h)
{
    int w = swap_tex->params.w;
    int h = swap_tex->params.h;

    if (processing_res == 0) {
        if (frame->current && frame->current->params.w > 0 &&
            frame->current->params.h > 0)
        {
            w = frame->current->params.w;
            h = frame->current->params.h;
        } else if (vo->params && vo->params->w > 0 && vo->params->h > 0) {
            w = vo->params->w;
            h = vo->params->h;
        }
    }

    *out_w = MPMAX(1, w);
    *out_h = MPMAX(1, h);
}

struct mpv_lsfg_adapter *mpv_lsfg_adapter_create(void *ta_parent, struct mp_log *log)
{
    struct mpv_lsfg_adapter *a = talloc_zero(ta_parent, struct mpv_lsfg_adapter);
    if (!a)
        return NULL;
    a->core = mp_lsfg_create(a, log);
    return a;
}

void mpv_lsfg_adapter_destroy(struct mpv_lsfg_adapter **adapter, pl_gpu gpu)
{
    if (!adapter || !*adapter)
        return;
    destroy_targets(*adapter, gpu);
    mp_lsfg_destroy(&(*adapter)->core);
    talloc_free(*adapter);
    *adapter = NULL;
}

void mpv_lsfg_adapter_update_opts(struct mpv_lsfg_adapter *adapter,
                                  const struct mpv_lsfg_opts *opts)
{
    if (!adapter || !adapter->core)
        return;
    struct mp_lsfg_opts core_opts = {
        .enable = opts ? opts->enable : false,
        .strict = opts ? opts->strict : false,
        .multiplier = opts ? opts->multiplier : 2,
        .flow_scale = opts ? opts->flow_scale : 1.0f,
        .performance_mode = opts ? opts->performance_mode : 0,
        .dll_path = opts ? opts->dll_path : NULL,
    };
    mp_lsfg_update_opts(adapter->core, &core_opts);
    if (!opts || !opts->enable)
        adapter->hard_failed = false;
}

void mpv_lsfg_adapter_begin_frame(struct mpv_lsfg_adapter *adapter,
                                  const struct mpv_lsfg_frame_info *info,
                                  struct mpv_lsfg_runtime *runtime)
{
    if (!adapter || !adapter->core)
        return;
    struct mp_lsfg_frame_info core_info = {
        .display_synced = info ? info->display_synced : false,
        .still = info ? info->still : false,
        .has_current = info ? info->has_current : false,
        .pending_reset = info ? info->pending_reset : false,
        .queue_more = info ? info->queue_more : false,
        .pts_monotonic = info ? info->pts_monotonic : true,
        .num_frames = info ? info->num_frames : 0,
    };
    struct mp_lsfg_runtime core_runtime = {0};
    mp_lsfg_begin_frame(adapter->core, &core_info, &core_runtime);
    if (runtime) {
        *runtime = (struct mpv_lsfg_runtime) {
            .enabled = core_runtime.enabled,
            .valid_pair = core_runtime.valid_pair,
            .would_generate = core_runtime.would_generate,
            .fallback = core_runtime.fallback,
            .requested_recreate = core_runtime.requested_recreate,
            .phase_index = core_runtime.phase_index,
            .phase_t = core_runtime.phase_t,
        };
    }
}

void mpv_lsfg_adapter_soft_reset(struct mpv_lsfg_adapter *adapter)
{
    if (!adapter || !adapter->core)
        return;
    mp_lsfg_on_reset(adapter->core);
}

void mpv_lsfg_adapter_on_reset(struct mpv_lsfg_adapter *adapter)
{
    if (!adapter || !adapter->core)
        return;
    mp_lsfg_on_reset(adapter->core);
    adapter->prev_valid = false;
    adapter->hard_failed = false;
    adapter->stats = (struct voctrl_lsfg_stats){0};
}

void mpv_lsfg_adapter_on_reconfig(struct mpv_lsfg_adapter *adapter, pl_gpu gpu)
{
    if (!adapter || !adapter->core)
        return;
    mp_lsfg_on_reconfig(adapter->core);
    destroy_targets(adapter, gpu);
    adapter->hard_failed = false;
    adapter->stats = (struct voctrl_lsfg_stats){0};
}

bool mpv_lsfg_adapter_render(struct mpv_lsfg_adapter *a,
                             const struct mpv_lsfg_render_params *rp,
                             bool *out_is_interpolated)
{
    if (out_is_interpolated)
        *out_is_interpolated = false;
    if (!a || !rp || !rp->runtime || !rp->mix || !rp->target ||
        !rp->swap_tex || !rp->render_params)
    {
        return false;
    }

    bool lsfg_path = rp->runtime->enabled &&
                     rp->runtime->valid_pair &&
                     !rp->runtime->fallback &&
                     !a->hard_failed;
    bool lsfg_should_generate = rp->runtime->would_generate &&
                                rp->frame->repeat &&
                                !rp->frame->redraw &&
                                rp->mix->num_frames > 1;
    int lsfg_w = rp->swap_tex->params.w;
    int lsfg_h = rp->swap_tex->params.h;

    if (lsfg_path) {
        pick_processing_size(rp->vo, rp->frame, rp->processing_res, rp->swap_tex,
                             &lsfg_w, &lsfg_h);
        if (!ensure_targets(a, rp->gpu, rp->swap_tex, lsfg_w, lsfg_h)) {
            if (rp->strict) {
                fail_strict(a, rp->vo, rp->gpu, "target-alloc-failed");
                lsfg_path = false;
            } else {
                MP_WARN(rp->vo, "lsfg: failed to allocate offscreen targets, using fallback path\n");
                lsfg_path = false;
            }
        }
    }

    if (!lsfg_path)
        return false;

    bool can_use_cached_gen = lsfg_should_generate &&
                              a->gen_valid &&
                              !rp->frame->redraw &&
                              a->gen_frame_id == rp->frame->frame_id &&
                              a->gen_phase_index == rp->runtime->phase_index;
    bool can_generate = lsfg_should_generate &&
                        a->prev_valid &&
                        !can_use_cached_gen;
    bool reuse_prev = !can_generate &&
                      !can_use_cached_gen &&
                      a->prev_valid &&
                      !rp->frame->redraw &&
                      a->prev_frame_id == rp->frame->frame_id;
    pl_tex present_src = NULL;
    int src_index = can_generate ? 1 : 0;
    uint64_t rendered_id = 0;
    bool rendered_new = false;

    if (can_use_cached_gen) {
        present_src = a->gen_rgb;
    } else if (reuse_prev) {
        present_src = a->prev_rgb;
    } else {
        const struct pl_frame *src_mix = rp->mix->num_frames > src_index
                                       ? rp->mix->frames[src_index]
                                       : (rp->mix->num_frames > 0 ? rp->mix->frames[0] : NULL);
        if (!src_mix)
            return false;

        struct pl_frame src_frame = *src_mix;
        src_frame.num_overlays = 0;
        src_frame.overlays = NULL;
        struct pl_frame off_target = target_frame_from_tex(a->cur_rgb, rp->target,
                                                           rp->swap_tex->params.w,
                                                           rp->swap_tex->params.h,
                                                           false);

        bool off_ok = pl_render_image(rp->rr, &src_frame, &off_target, rp->render_params);
        if (!off_ok) {
            if (rp->strict) {
                fail_strict(a, rp->vo, rp->gpu, "offscreen-render-failed");
                return false;
            }
            MP_WARN(rp->vo, "lsfg: offscreen render failed, using fallback path\n");
            return false;
        }

        present_src = a->cur_rgb;
        rendered_new = true;
        rendered_id = rp->frame->frame_id + (uint64_t)src_index;
    }

    if (can_generate) {
        enum mp_lsfg_gen_status gen = mp_lsfg_generate(a->core, rp->gpu,
                                                       a->prev_rgb, a->cur_rgb, a->gen_rgb,
                                                       rp->runtime->phase_t);
        if (gen == MP_LSFG_GEN_OK) {
            present_src = a->gen_rgb;
            a->gen_valid = true;
            a->gen_frame_id = rp->frame->frame_id;
            a->gen_phase_index = rp->runtime->phase_index;
        } else {
            const char *reason = mp_lsfg_gen_status_str(gen);
            if (rp->strict) {
                fail_strict(a, rp->vo, rp->gpu, reason);
                return false;
            }
            MP_WARN(rp->vo, "lsfg: generate pass failed (%s), using current frame\n", reason);
        }
    }

    bool present_ok = present_to_swapchain(rp->gpu, rp->rr, present_src, rp->target, rp->swap_tex);
    if (!present_ok) {
        if (rp->strict) {
            fail_strict(a, rp->vo, rp->gpu, "present-path-failed");
            return false;
        }
        MP_WARN(rp->vo, "lsfg: present path failed, using fallback path\n");
        return false;
    }

    if (rendered_new) {
        pl_tex swap = a->prev_rgb;
        a->prev_rgb = a->cur_rgb;
        a->cur_rgb = swap;
        a->prev_valid = true;
        a->prev_frame_id = rendered_id;
    }

    if (out_is_interpolated)
        *out_is_interpolated = present_src == a->gen_rgb;
    return true;
}

void mpv_lsfg_adapter_record_frame(struct mpv_lsfg_adapter *adapter, bool enabled,
                                   bool is_interpolated, bool fallback_used)
{
    if (!adapter || !enabled)
        return;
    adapter->stats.total++;
    if (is_interpolated) {
        adapter->stats.interpolated++;
    } else if (fallback_used) {
        adapter->stats.fallback++;
    } else {
        adapter->stats.real++;
    }
}

void mpv_lsfg_adapter_get_stats(struct mpv_lsfg_adapter *adapter,
                                struct voctrl_lsfg_stats *stats)
{
    if (!stats)
        return;
    if (!adapter) {
        *stats = (struct voctrl_lsfg_stats){0};
        return;
    }
    *stats = adapter->stats;
}
