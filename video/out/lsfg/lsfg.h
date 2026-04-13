#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <libplacebo/gpu.h>

struct mp_log;
struct mp_lsfg_ctx;

struct mp_lsfg_opts {
    bool enable;
    bool strict;
    int multiplier; // 2/3/4
    float flow_scale;
    int performance_mode; // 0: quality, 1: performance
    const char *dll_path; // optional override for Lossless.dll
};

struct mp_lsfg_frame_info {
    bool display_synced;
    bool still;
    bool has_current;
    bool pending_reset;
    bool queue_more;
    bool pts_monotonic;
    int num_frames;
};

struct mp_lsfg_runtime {
    bool enabled;
    bool valid_pair;
    bool would_generate;
    bool fallback;
    bool requested_recreate;
    uint32_t phase_index;
    float phase_t;
};

struct mp_lsfg_ctx *mp_lsfg_create(void *ta_parent, struct mp_log *log);
void mp_lsfg_destroy(struct mp_lsfg_ctx **ctx);

void mp_lsfg_update_opts(struct mp_lsfg_ctx *ctx, const struct mp_lsfg_opts *opts);
void mp_lsfg_on_reset(struct mp_lsfg_ctx *ctx);
void mp_lsfg_on_reconfig(struct mp_lsfg_ctx *ctx);

// `runtime` may be NULL when caller does not need telemetry.
void mp_lsfg_begin_frame(struct mp_lsfg_ctx *ctx,
                         const struct mp_lsfg_frame_info *info,
                         struct mp_lsfg_runtime *runtime);

enum mp_lsfg_gen_status {
    MP_LSFG_GEN_OK = 0,
    MP_LSFG_GEN_DISABLED,
    MP_LSFG_GEN_BAD_INPUT,
    MP_LSFG_GEN_SIZE_MISMATCH,
    MP_LSFG_GEN_FORMAT_MISMATCH,
    MP_LSFG_GEN_OUTPUT_NOT_STORABLE,
    MP_LSFG_GEN_PASS_CREATE_FAILED,
};

const char *mp_lsfg_gen_status_str(enum mp_lsfg_gen_status status);

enum mp_lsfg_gen_status mp_lsfg_generate(struct mp_lsfg_ctx *ctx, pl_gpu gpu,
                                         pl_tex prev_rgb, pl_tex cur_rgb, pl_tex out_rgb,
                                         float phase_t);
