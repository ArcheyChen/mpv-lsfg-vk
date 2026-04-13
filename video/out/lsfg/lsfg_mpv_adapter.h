#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <libplacebo/gpu.h>
#include <libplacebo/renderer.h>

#include "video/out/vo.h"

struct mp_log;

struct mpv_lsfg_adapter;

struct mpv_lsfg_opts {
    bool enable;
    bool strict;
    int multiplier; // 2/3/4
    float flow_scale;
    int performance_mode; // 0: quality, 1: performance
    const char *dll_path;
};

struct mpv_lsfg_frame_info {
    bool display_synced;
    bool still;
    bool has_current;
    bool pending_reset;
    bool queue_more;
    bool pts_monotonic;
    int num_frames;
};

struct mpv_lsfg_runtime {
    bool enabled;
    bool valid_pair;
    bool would_generate;
    bool fallback;
    bool requested_recreate;
    uint32_t phase_index;
    float phase_t;
};

struct mpv_lsfg_render_params {
    struct vo *vo;
    pl_gpu gpu;
    pl_renderer rr;
    struct vo_frame *frame;
    const struct pl_frame_mix *mix;
    const struct pl_frame *target;
    const struct pl_render_params *render_params;
    pl_tex swap_tex;
    const struct mpv_lsfg_runtime *runtime;
    bool strict;
    int processing_res;
};

struct mpv_lsfg_adapter *mpv_lsfg_adapter_create(void *ta_parent, struct mp_log *log);
void mpv_lsfg_adapter_destroy(struct mpv_lsfg_adapter **adapter, pl_gpu gpu);

void mpv_lsfg_adapter_update_opts(struct mpv_lsfg_adapter *adapter,
                                  const struct mpv_lsfg_opts *opts);
void mpv_lsfg_adapter_begin_frame(struct mpv_lsfg_adapter *adapter,
                                  const struct mpv_lsfg_frame_info *info,
                                  struct mpv_lsfg_runtime *runtime);
void mpv_lsfg_adapter_soft_reset(struct mpv_lsfg_adapter *adapter);
void mpv_lsfg_adapter_on_reset(struct mpv_lsfg_adapter *adapter);
void mpv_lsfg_adapter_on_reconfig(struct mpv_lsfg_adapter *adapter, pl_gpu gpu);

bool mpv_lsfg_adapter_render(struct mpv_lsfg_adapter *adapter,
                             const struct mpv_lsfg_render_params *params,
                             bool *out_is_interpolated);

void mpv_lsfg_adapter_record_frame(struct mpv_lsfg_adapter *adapter,
                                   bool enabled,
                                   bool is_interpolated,
                                   bool fallback_used);
void mpv_lsfg_adapter_get_stats(struct mpv_lsfg_adapter *adapter,
                                struct voctrl_lsfg_stats *stats);
