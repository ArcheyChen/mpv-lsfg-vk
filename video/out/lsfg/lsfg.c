#include "video/out/lsfg/lsfg.h"
#include "video/out/lsfg/lsfg_dll.h"

#include "config.h"
#include "common/common.h"
#include "common/msg.h"
#include "ta/ta_talloc.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

enum {
    LSFG_LEVELS = 7,
    LSFG_GAMMA_STAGES = 7,
    LSFG_DELTA_STAGES = 3,
    LSFG_MAX_M = 2,
    LSFG_MAX_TEMPORAL = 3,
    LSFG_MAX_BUFFER_BINDINGS = 1,
    LSFG_MAX_SAMPLED_BINDINGS = 12,
    LSFG_MAX_STORAGE_BINDINGS = 7,
    LSFG_MAX_DESC_BINDINGS = LSFG_MAX_BUFFER_BINDINGS +
                             LSFG_MAX_SAMPLED_BINDINGS +
                             LSFG_MAX_STORAGE_BINDINGS,
};

enum lsfg_sampler_mode {
    LSFG_SAMPLER_EDGE = 0,
    LSFG_SAMPLER_BORDER_BLACK = 1,
    LSFG_SAMPLER_BORDER_WHITE = 2,
};

struct lsfg_constants {
    uint32_t input_offset[2];
    uint32_t first_iter;
    uint32_t first_iter_s;
    uint32_t advanced_color_kind;
    uint32_t hdr_support;
    float resolution_inv_scale;
    float timestamp;
    float ui_threshold;
    uint32_t pad[3];
};

struct lsfg_pass_spec {
    uint32_t shader_id;
    uint32_t resource_id;
    int sampled;
    int storage;
    int buffers;
    int sampler0_mode;
    int sampler1_mode;
    uint32_t group_add;
    uint32_t group_shift;
    pl_pass pass;
    pl_gpu pass_gpu;
};

struct lsfg_alpha0_level {
    pl_tex temp0[LSFG_MAX_M];
    pl_tex temp1[LSFG_MAX_M];
    pl_tex out[LSFG_MAX_M * 2];
};

struct lsfg_alpha1_level {
    int temporal;
    pl_tex out[LSFG_MAX_TEMPORAL][LSFG_MAX_M * 2];
};

struct lsfg_gamma0_stage {
    pl_tex out[3];
};

struct lsfg_gamma1_stage {
    pl_tex temp0[LSFG_MAX_M * 2];
    pl_tex temp1[LSFG_MAX_M * 2];
    pl_tex out;
};

struct lsfg_delta0_stage {
    pl_tex out0[3];
    pl_tex out1[LSFG_MAX_M];
};

struct lsfg_delta1_stage {
    pl_tex temp0[LSFG_MAX_M * 2];
    pl_tex temp1[LSFG_MAX_M * 2];
    pl_tex out0;
    pl_tex out1;
};

struct mp_lsfg_ctx {
    struct mp_log *log;
    struct mp_lsfg_opts opts;
    bool initialized;
    bool need_soft_reset;
    bool need_recreate;
    bool logged_path_notice;
    uint32_t phase_index;

    bool dag_ready;
    pl_gpu dag_gpu;
    pl_fmt dag_out_fmt;
    int dag_w;
    int dag_h;
    int flow_w;
    int flow_h;
    int dag_m;
    bool dag_perf;
    bool dag_hdr;
    float dag_flow_scale;
    uint64_t frame_index;

    pl_fmt fmt_rgba8;
    pl_fmt fmt_r8;
    pl_fmt fmt_rgba16f;

    pl_tex black;
    pl_tex mipmaps[LSFG_LEVELS];
    struct lsfg_alpha0_level alpha0[LSFG_LEVELS];
    struct lsfg_alpha1_level alpha1[LSFG_LEVELS];
    pl_tex beta0_out[2];
    pl_tex beta1_temp0[2];
    pl_tex beta1_temp1[2];
    pl_tex beta1_out[6];
    struct lsfg_gamma0_stage gamma0[LSFG_GAMMA_STAGES];
    struct lsfg_gamma1_stage gamma1[LSFG_GAMMA_STAGES];
    struct lsfg_delta0_stage delta0[LSFG_DELTA_STAGES];
    struct lsfg_delta1_stage delta1[LSFG_DELTA_STAGES];

    pl_buf cbuf_pre;
    pl_buf cbuf_main;
    struct lsfg_constants cpre;
    struct lsfg_constants cmain;

    struct lsfg_pass_spec pass_mipmaps;
    struct lsfg_pass_spec pass_generate;
    struct lsfg_pass_spec pass_alpha[4];
    struct lsfg_pass_spec pass_beta[5];
    struct lsfg_pass_spec pass_gamma[5];
    struct lsfg_pass_spec pass_delta[10];

    struct lsfg_dll_ctx *dll;
    char *dll_path;
};

static int lsfg_max_component_depth(pl_fmt fmt)
{
    int max_depth = 0;
    for (int i = 0; i < fmt->num_components; i++)
        max_depth = MPMAX(max_depth, fmt->component_depth[i]);
    return max_depth;
}

static const char *lsfg_image_format_qualifier(pl_fmt fmt)
{
    if (!fmt || !fmt->num_components)
        return NULL;

    int depth = lsfg_max_component_depth(fmt);
    switch (fmt->type) {
    case PL_FMT_UNORM:
        if (fmt->num_components == 1) return depth <= 8 ? "r8" : "r16";
        if (fmt->num_components == 2) return depth <= 8 ? "rg8" : "rg16";
        if (fmt->num_components == 4) return depth <= 8 ? "rgba8" : "rgba16";
        break;
    case PL_FMT_FLOAT:
        if (fmt->num_components == 1) return depth <= 16 ? "r16f" : "r32f";
        if (fmt->num_components == 2) return depth <= 16 ? "rg16f" : "rg32f";
        if (fmt->num_components == 4) return depth <= 16 ? "rgba16f" : "rgba32f";
        break;
    case PL_FMT_UINT:
        if (fmt->num_components == 1) {
            if (depth <= 8) return "r8ui";
            if (depth <= 16) return "r16ui";
            return "r32ui";
        }
        if (fmt->num_components == 2) {
            if (depth <= 8) return "rg8ui";
            if (depth <= 16) return "rg16ui";
            return "rg32ui";
        }
        if (fmt->num_components == 4) {
            if (depth <= 8) return "rgba8ui";
            if (depth <= 16) return "rgba16ui";
            return "rgba32ui";
        }
        break;
    default:
        break;
    }

    return NULL;
}

static int lsfg_shift_dim(int dim, int shift)
{
    return MPMAX(1, dim >> shift);
}

static int lsfg_add_shift_dim(int dim, int add, int shift)
{
    return MPMAX(1, (dim + add) >> shift);
}

static uint32_t lsfg_resource_id(uint32_t shader_id, bool perf, bool fp16)
{
    // In upstream lsfg-vk, mipmaps (255) and generate (256) are shared
    // between quality/performance variants and do not use the perf offset.
    bool perf_variant = perf && shader_id != 255 && shader_id != 256;
    return 49 + shader_id + (perf_variant ? 23 : 0) + (fp16 ? 0 : 49);
}

static void lsfg_init_constants(struct lsfg_constants *cb, bool hdr, float flow_scale, float ts)
{
    if (!cb)
        return;
    memset(cb, 0, sizeof(*cb));
    cb->advanced_color_kind = hdr ? 2u : 0u;
    cb->hdr_support = hdr ? 1u : 0u;
    cb->resolution_inv_scale = flow_scale;
    cb->timestamp = ts;
    cb->ui_threshold = 0.5f;
}

static bool lsfg_streq(const char *a, const char *b)
{
    if (a == b)
        return true;
    if (!a || !b)
        return false;
    return strcmp(a, b) == 0;
}

static char *lsfg_replace_all(void *ta_parent, const char *src,
                              const char *needle, const char *replacement)
{
    if (!src || !needle || !needle[0])
        return talloc_strdup(ta_parent, src ? src : "");

    char *out = talloc_strdup(ta_parent, "");
    if (!out)
        return NULL;

    const size_t nlen = strlen(needle);
    const char *p = src;
    while (1) {
        const char *m = strstr(p, needle);
        if (!m)
            break;
        out = talloc_asprintf_append_buffer(out, "%.*s%s",
                                            (int)(m - p), p, replacement);
        if (!out)
            return NULL;
        p = m + nlen;
    }

    out = talloc_asprintf_append_buffer(out, "%s", p);
    return out;
}

static char *lsfg_inject_sampler_helpers(void *ta_parent, const char *src,
                                         int sampler0_mode, int sampler1_mode)
{
    if (!src)
        return NULL;

    const char *insert = src;
    while (*insert) {
        const char *line_start = insert;
        const char *line_end = strchr(insert, '\n');
        if (!line_end)
            line_end = insert + strlen(insert);
        else
            line_end += 1;

        const char *p = line_start;
        while (*p == ' ' || *p == '\t' || *p == '\r')
            p++;
        if (*p == '#') {
            insert = line_end;
            continue;
        }
        break;
    }

    size_t head_len = insert - src;
    char *head = talloc_strndup(NULL, src, head_len);
    if (!head)
        return NULL;

    char *helper = talloc_asprintf(NULL,
        "const int LSFG_SAMPLER_EDGE = 0;\n"
        "const int LSFG_SAMPLER_BORDER_BLACK = 1;\n"
        "const int LSFG_SAMPLER_BORDER_WHITE = 2;\n"
        "const int LSFG_MODE_SAMPLER0 = %d;\n"
        "const int LSFG_MODE_SAMPLER1 = %d;\n"
        "vec4 lsfg_border_value(int mode)\n"
        "{\n"
        "    return mode == LSFG_SAMPLER_BORDER_WHITE ? vec4(1.0) : vec4(0.0);\n"
        "}\n"
        "vec4 lsfg_texlod(sampler2D tex, int sid, vec2 uv, float lod)\n"
        "{\n"
        "    int mode = sid == 0 ? LSFG_MODE_SAMPLER0 : LSFG_MODE_SAMPLER1;\n"
        "    if (mode == LSFG_SAMPLER_EDGE)\n"
        "        return textureLod(tex, clamp(uv, vec2(0.0), vec2(1.0)), lod);\n"
        "    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))))\n"
        "        return lsfg_border_value(mode);\n"
        "    return textureLod(tex, uv, lod);\n"
        "}\n"
        "vec4 lsfg_texlodoffset(sampler2D tex, int sid, vec2 uv, float lod, ivec2 offset)\n"
        "{\n"
        "    vec2 tex_sz = max(vec2(textureSize(tex, int(lod))), vec2(1.0));\n"
        "    vec2 uv_off = uv + vec2(offset) / tex_sz;\n"
        "    return lsfg_texlod(tex, sid, uv_off, lod);\n"
        "}\n",
        sampler0_mode, sampler1_mode);
    if (!helper) {
        talloc_free(head);
        return NULL;
    }
    helper = talloc_asprintf_append_buffer(helper,
        "#define lsfg_imageStore(img, p, v) do { \\\n"
        "    ivec2 _lsfg_sz = imageSize(img); \\\n"
        "    if (all(greaterThanEqual((p), ivec2(0))) && all(lessThan((p), _lsfg_sz))) \\\n"
        "        imageStore((img), (p), (v)); \\\n"
        "} while (false)\n");
    if (!helper) {
        talloc_free(head);
        return NULL;
    }

    char *out = talloc_asprintf(ta_parent, "%s%s%s", head, helper, insert);
    talloc_free(helper);
    talloc_free(head);
    return out;
}

static char *lsfg_preprocess_shader(void *ta_parent, const char *src,
                                    uint32_t shader_id, const char *out_img_fmt,
                                    int sampler0_mode, int sampler1_mode)
{
    void *tmp = talloc_new(NULL);
    if (!tmp)
        return NULL;

    char *s = talloc_strdup(tmp, src);
    if (!s) {
        talloc_free(tmp);
        return NULL;
    }

    s = lsfg_replace_all(tmp, s,
            "#extension GL_EXT_samplerless_texture_functions : require\n", "");
    s = lsfg_replace_all(tmp, s,
            "layout(set = 0, binding = 16) uniform sampler Sampler;\n", "");
    s = lsfg_replace_all(tmp, s,
            "layout(set = 0, binding = 17) uniform sampler Sampler1;\n", "");
    s = lsfg_replace_all(tmp, s, "uniform texture2D ", "uniform sampler2D ");
    s = lsfg_replace_all(tmp, s, "textureLodOffset(sampler2D(", "lsfg_texlodoffset(");
    s = lsfg_replace_all(tmp, s, "textureLod(sampler2D(", "lsfg_texlod(");
    s = lsfg_replace_all(tmp, s, "imageStore(", "lsfg_imageStore(");
    s = lsfg_replace_all(tmp, s, ", Sampler1), ", ", 1, ");
    s = lsfg_replace_all(tmp, s, ", Sampler), ", ", 0, ");
    s = lsfg_replace_all(tmp, s, ", Sampler1),", ", 1,");
    s = lsfg_replace_all(tmp, s, ", Sampler),", ", 0,");

    if (shader_id == 256 && out_img_fmt) {
        char *decl = talloc_asprintf(tmp,
            "layout(set = 0, binding = 48, %s) uniform writeonly image2D Output1;",
            out_img_fmt);
        s = lsfg_replace_all(tmp, s,
            "layout(set = 0, binding = 48) uniform writeonly image2D Output1;",
            decl ? decl : "");
    }

    char *with_helpers = lsfg_inject_sampler_helpers(tmp, s, sampler0_mode, sampler1_mode);
    if (!with_helpers) {
        talloc_free(tmp);
        return NULL;
    }
    char *ret = talloc_steal(ta_parent, with_helpers);
    talloc_free(tmp);
    return ret;
}

static bool lsfg_create_tex(pl_gpu gpu, int w, int h, pl_fmt fmt, bool storable, pl_tex *out)
{
    if (!gpu || !fmt || !out || w < 1 || h < 1)
        return false;

    pl_tex_destroy(gpu, out);
    *out = pl_tex_create(gpu, pl_tex_params(
        .w = w,
        .h = h,
        .format = fmt,
        .sampleable = true,
        .storable = storable,
    ));
    return *out != NULL;
}

static void lsfg_destroy_tex_arr(pl_gpu gpu, pl_tex *arr, int n)
{
    if (!gpu || !arr)
        return;
    for (int i = 0; i < n; i++)
        pl_tex_destroy(gpu, &arr[i]);
}

static void lsfg_destroy_pass_spec(struct lsfg_pass_spec *spec)
{
    if (!spec || !spec->pass || !spec->pass_gpu)
        return;
    pl_pass_destroy(spec->pass_gpu, &spec->pass);
    spec->pass = NULL;
    spec->pass_gpu = NULL;
}

static void lsfg_destroy_runtime(struct mp_lsfg_ctx *ctx)
{
    if (!ctx)
        return;

    pl_gpu gpu = ctx->dag_gpu;

    lsfg_destroy_pass_spec(&ctx->pass_mipmaps);
    lsfg_destroy_pass_spec(&ctx->pass_generate);
    for (int i = 0; i < 4; i++)
        lsfg_destroy_pass_spec(&ctx->pass_alpha[i]);
    for (int i = 0; i < 5; i++)
        lsfg_destroy_pass_spec(&ctx->pass_beta[i]);
    for (int i = 0; i < 5; i++)
        lsfg_destroy_pass_spec(&ctx->pass_gamma[i]);
    for (int i = 0; i < 10; i++)
        lsfg_destroy_pass_spec(&ctx->pass_delta[i]);

    if (gpu) {
        pl_buf_destroy(gpu, &ctx->cbuf_pre);
        pl_buf_destroy(gpu, &ctx->cbuf_main);
        pl_tex_destroy(gpu, &ctx->black);
        lsfg_destroy_tex_arr(gpu, ctx->mipmaps, LSFG_LEVELS);
        for (int i = 0; i < LSFG_LEVELS; i++) {
            lsfg_destroy_tex_arr(gpu, ctx->alpha0[i].temp0, LSFG_MAX_M);
            lsfg_destroy_tex_arr(gpu, ctx->alpha0[i].temp1, LSFG_MAX_M);
            lsfg_destroy_tex_arr(gpu, ctx->alpha0[i].out, LSFG_MAX_M * 2);
            for (int j = 0; j < LSFG_MAX_TEMPORAL; j++)
                lsfg_destroy_tex_arr(gpu, ctx->alpha1[i].out[j], LSFG_MAX_M * 2);
        }
        lsfg_destroy_tex_arr(gpu, ctx->beta0_out, 2);
        lsfg_destroy_tex_arr(gpu, ctx->beta1_temp0, 2);
        lsfg_destroy_tex_arr(gpu, ctx->beta1_temp1, 2);
        lsfg_destroy_tex_arr(gpu, ctx->beta1_out, 6);
        for (int i = 0; i < LSFG_GAMMA_STAGES; i++) {
            lsfg_destroy_tex_arr(gpu, ctx->gamma0[i].out, 3);
            lsfg_destroy_tex_arr(gpu, ctx->gamma1[i].temp0, LSFG_MAX_M * 2);
            lsfg_destroy_tex_arr(gpu, ctx->gamma1[i].temp1, LSFG_MAX_M * 2);
            pl_tex_destroy(gpu, &ctx->gamma1[i].out);
        }
        for (int i = 0; i < LSFG_DELTA_STAGES; i++) {
            lsfg_destroy_tex_arr(gpu, ctx->delta0[i].out0, 3);
            lsfg_destroy_tex_arr(gpu, ctx->delta0[i].out1, LSFG_MAX_M);
            lsfg_destroy_tex_arr(gpu, ctx->delta1[i].temp0, LSFG_MAX_M * 2);
            lsfg_destroy_tex_arr(gpu, ctx->delta1[i].temp1, LSFG_MAX_M * 2);
            pl_tex_destroy(gpu, &ctx->delta1[i].out0);
            pl_tex_destroy(gpu, &ctx->delta1[i].out1);
        }
    }

    ctx->dag_ready = false;
    ctx->dag_gpu = NULL;
    ctx->dag_out_fmt = NULL;
    ctx->dag_w = 0;
    ctx->dag_h = 0;
    ctx->flow_w = 0;
    ctx->flow_h = 0;
}

static void lsfg_setup_pass_specs(struct mp_lsfg_ctx *ctx)
{
    int m = ctx->dag_m;
    int bnb = LSFG_SAMPLER_BORDER_BLACK;
    int bnw = LSFG_SAMPLER_BORDER_WHITE;
    int eab = LSFG_SAMPLER_EDGE;

    ctx->pass_mipmaps = (struct lsfg_pass_spec) {
        .shader_id = 255,
        .sampled = 1,
        .storage = 7,
        .buffers = 1,
        .sampler0_mode = bnb,
        .sampler1_mode = eab,
        .group_add = 63,
        .group_shift = 6,
    };
    ctx->pass_generate = (struct lsfg_pass_spec) {
        .shader_id = 256,
        .sampled = 5,
        .storage = 1,
        .buffers = 1,
        .sampler0_mode = bnb,
        .sampler1_mode = eab,
        .group_add = 15,
        .group_shift = 4,
    };

    ctx->pass_alpha[0] = (struct lsfg_pass_spec) {
        .shader_id = 267, .sampled = 1, .storage = m, .buffers = 0, .sampler0_mode = bnb, .sampler1_mode = eab, .group_add = 7, .group_shift = 3,
    };
    ctx->pass_alpha[1] = (struct lsfg_pass_spec) {
        .shader_id = 268, .sampled = m, .storage = m, .buffers = 0, .sampler0_mode = bnb, .sampler1_mode = eab, .group_add = 7, .group_shift = 3,
    };
    ctx->pass_alpha[2] = (struct lsfg_pass_spec) {
        .shader_id = 269, .sampled = m, .storage = 2 * m, .buffers = 0, .sampler0_mode = bnb, .sampler1_mode = eab, .group_add = 7, .group_shift = 3,
    };
    ctx->pass_alpha[3] = (struct lsfg_pass_spec) {
        .shader_id = 270, .sampled = 2 * m, .storage = 2 * m, .buffers = 0, .sampler0_mode = bnb, .sampler1_mode = eab, .group_add = 7, .group_shift = 3,
    };

    ctx->pass_beta[0] = (struct lsfg_pass_spec) {
        .shader_id = 275, .sampled = 6 * m, .storage = 2, .buffers = 0, .sampler0_mode = bnw, .sampler1_mode = eab, .group_add = 7, .group_shift = 3,
    };
    ctx->pass_beta[1] = (struct lsfg_pass_spec) {
        .shader_id = 276, .sampled = 2, .storage = 2, .buffers = 0, .sampler0_mode = bnb, .sampler1_mode = eab, .group_add = 7, .group_shift = 3,
    };
    ctx->pass_beta[2] = (struct lsfg_pass_spec) {
        .shader_id = 277, .sampled = 2, .storage = 2, .buffers = 0, .sampler0_mode = bnb, .sampler1_mode = eab, .group_add = 7, .group_shift = 3,
    };
    ctx->pass_beta[3] = (struct lsfg_pass_spec) {
        .shader_id = 278, .sampled = 2, .storage = 2, .buffers = 0, .sampler0_mode = bnb, .sampler1_mode = eab, .group_add = 7, .group_shift = 3,
    };
    ctx->pass_beta[4] = (struct lsfg_pass_spec) {
        .shader_id = 279, .sampled = 2, .storage = 6, .buffers = 1, .sampler0_mode = bnb, .sampler1_mode = eab, .group_add = 31, .group_shift = 5,
    };

    ctx->pass_gamma[0] = (struct lsfg_pass_spec) {
        .shader_id = 257, .sampled = (4 * m) + 1, .storage = 3, .buffers = 1, .sampler0_mode = bnw, .sampler1_mode = eab, .group_add = 7, .group_shift = 3,
    };
    ctx->pass_gamma[1] = (struct lsfg_pass_spec) {
        .shader_id = 259, .sampled = 3, .storage = 2 * m, .buffers = 0, .sampler0_mode = bnb, .sampler1_mode = eab, .group_add = 7, .group_shift = 3,
    };
    ctx->pass_gamma[2] = (struct lsfg_pass_spec) {
        .shader_id = 260, .sampled = 2 * m, .storage = 2 * m, .buffers = 0, .sampler0_mode = bnb, .sampler1_mode = eab, .group_add = 7, .group_shift = 3,
    };
    ctx->pass_gamma[3] = (struct lsfg_pass_spec) {
        .shader_id = 261, .sampled = 2 * m, .storage = 2 * m, .buffers = 0, .sampler0_mode = bnb, .sampler1_mode = eab, .group_add = 7, .group_shift = 3,
    };
    ctx->pass_gamma[4] = (struct lsfg_pass_spec) {
        .shader_id = 262, .sampled = (2 * m) + 2, .storage = 1, .buffers = 1, .sampler0_mode = bnb, .sampler1_mode = eab, .group_add = 7, .group_shift = 3,
    };

    ctx->pass_delta[0] = (struct lsfg_pass_spec) {
        .shader_id = 257, .sampled = (4 * m) + 1, .storage = 3, .buffers = 1, .sampler0_mode = bnw, .sampler1_mode = eab, .group_add = 7, .group_shift = 3,
    };
    ctx->pass_delta[1] = (struct lsfg_pass_spec) {
        .shader_id = 263, .sampled = 3, .storage = 2 * m, .buffers = 0, .sampler0_mode = bnb, .sampler1_mode = eab, .group_add = 7, .group_shift = 3,
    };
    ctx->pass_delta[2] = (struct lsfg_pass_spec) {
        .shader_id = 264, .sampled = 2 * m, .storage = 2 * m, .buffers = 0, .sampler0_mode = bnb, .sampler1_mode = eab, .group_add = 7, .group_shift = 3,
    };
    ctx->pass_delta[3] = (struct lsfg_pass_spec) {
        .shader_id = 265, .sampled = 2 * m, .storage = 2 * m, .buffers = 0, .sampler0_mode = bnb, .sampler1_mode = eab, .group_add = 7, .group_shift = 3,
    };
    ctx->pass_delta[4] = (struct lsfg_pass_spec) {
        .shader_id = 266, .sampled = (2 * m) + 2, .storage = 1, .buffers = 1, .sampler0_mode = bnb, .sampler1_mode = eab, .group_add = 7, .group_shift = 3,
    };
    ctx->pass_delta[5] = (struct lsfg_pass_spec) {
        .shader_id = 258, .sampled = (4 * m) + 2, .storage = m, .buffers = 1, .sampler0_mode = bnw, .sampler1_mode = eab, .group_add = 7, .group_shift = 3,
    };
    ctx->pass_delta[6] = (struct lsfg_pass_spec) {
        .shader_id = 271, .sampled = m, .storage = m, .buffers = 0, .sampler0_mode = bnb, .sampler1_mode = eab, .group_add = 7, .group_shift = 3,
    };
    ctx->pass_delta[7] = (struct lsfg_pass_spec) {
        .shader_id = 272, .sampled = m, .storage = m, .buffers = 0, .sampler0_mode = bnb, .sampler1_mode = eab, .group_add = 7, .group_shift = 3,
    };
    ctx->pass_delta[8] = (struct lsfg_pass_spec) {
        .shader_id = 273, .sampled = m, .storage = m, .buffers = 0, .sampler0_mode = bnb, .sampler1_mode = eab, .group_add = 7, .group_shift = 3,
    };
    ctx->pass_delta[9] = (struct lsfg_pass_spec) {
        .shader_id = 274, .sampled = m + 1, .storage = 1, .buffers = 1, .sampler0_mode = bnb, .sampler1_mode = eab, .group_add = 7, .group_shift = 3,
    };
}

static bool lsfg_compile_spec(struct mp_lsfg_ctx *ctx, pl_gpu gpu,
                              struct lsfg_pass_spec *spec)
{
    if (!ctx || !gpu || !spec)
        return false;
    if (spec->pass && spec->pass_gpu == gpu)
        return true;

    lsfg_destroy_pass_spec(spec);

    bool fp16 = false;
    spec->resource_id = lsfg_resource_id(spec->shader_id, ctx->dag_perf, fp16);
    if (!lsfg_dll_load(ctx->dll, ctx->dll_path))
        return false;
    const char *glsl = lsfg_dll_get_glsl(ctx->dll, spec->resource_id);
    if (!glsl) {
        mp_err(ctx->log, "lsfg: missing resource %u in DLL path %s\n",
               spec->resource_id, lsfg_dll_loaded_path(ctx->dll));
        return false;
    }
    char *src = talloc_strdup(NULL, glsl);

    const char *out_fmt = NULL;
    if (spec->shader_id == 256)
        out_fmt = lsfg_image_format_qualifier(ctx->dag_out_fmt);
    char *shader = lsfg_preprocess_shader(NULL, src, spec->shader_id, out_fmt,
                                          spec->sampler0_mode, spec->sampler1_mode);
    talloc_free(src);
    if (!shader) {
        mp_err(ctx->log, "lsfg: failed to preprocess shader %u\n", spec->resource_id);
        return false;
    }

    int num_desc = spec->buffers + spec->sampled + spec->storage;
    struct pl_desc *descs = talloc_array(shader, struct pl_desc, num_desc);
    if (!descs) {
        talloc_free(shader);
        return false;
    }

    int idx = 0;
    for (int i = 0; i < spec->buffers; i++) {
        descs[idx++] = (struct pl_desc) {
            .name = "CB",
            .type = PL_DESC_BUF_UNIFORM,
            .binding = i,
            .access = PL_DESC_ACCESS_READONLY,
        };
    }
    for (int i = 0; i < spec->sampled; i++) {
        descs[idx++] = (struct pl_desc) {
            .name = "Input",
            .type = PL_DESC_SAMPLED_TEX,
            .binding = 32 + i,
            .access = PL_DESC_ACCESS_READONLY,
        };
    }
    for (int i = 0; i < spec->storage; i++) {
        descs[idx++] = (struct pl_desc) {
            .name = "Output",
            .type = PL_DESC_STORAGE_IMG,
            .binding = 48 + i,
            .access = PL_DESC_ACCESS_WRITEONLY,
        };
    }

    struct pl_pass_params params = {
        .type = PL_PASS_COMPUTE,
        .descriptors = descs,
        .num_descriptors = num_desc,
        .glsl_shader = shader,
    };

    spec->pass = pl_pass_create(gpu, &params);
    spec->pass_gpu = gpu;
    if (!spec->pass) {
        mp_err(ctx->log, "lsfg: pass creation failed (resource=%u)\n", spec->resource_id);
        talloc_free(shader);
        return false;
    }

    talloc_free(shader);
    return true;
}

static bool lsfg_dispatch(struct mp_lsfg_ctx *ctx, pl_gpu gpu, struct lsfg_pass_spec *spec,
                          pl_buf *buffers, int nb, pl_tex *sampled, int ns,
                          pl_tex *storage, int no, int w, int h)
{
    if (!ctx || !gpu || !spec || !spec->pass)
        return false;
    if (nb != spec->buffers || ns != spec->sampled || no != spec->storage) {
        mp_err(ctx->log,
               "lsfg: descriptor count mismatch shader=%u got(b=%d,s=%d,o=%d) need(b=%d,s=%d,o=%d)\n",
               spec->shader_id, nb, ns, no, spec->buffers, spec->sampled, spec->storage);
        return false;
    }

    struct pl_desc_binding binds[LSFG_MAX_DESC_BINDINGS] = {0};
    int idx = 0;
    for (int i = 0; i < nb; i++) {
        if (!buffers[i])
            return false;
        binds[idx++].object = buffers[i];
    }
    for (int i = 0; i < ns; i++) {
        if (!sampled[i])
            return false;
        binds[idx] = (struct pl_desc_binding) {
            .object = sampled[i],
            .address_mode = PL_TEX_ADDRESS_CLAMP,
            .sample_mode = PL_TEX_SAMPLE_LINEAR,
        };
        idx++;
    }
    for (int i = 0; i < no; i++) {
        if (!storage[i])
            return false;
        binds[idx++].object = storage[i];
    }

    int gx = lsfg_add_shift_dim(w, spec->group_add, spec->group_shift);
    int gy = lsfg_add_shift_dim(h, spec->group_add, spec->group_shift);

    struct pl_pass_run_params run = {
        .pass = spec->pass,
        .desc_bindings = binds,
    };
    run.compute_groups[0] = gx;
    run.compute_groups[1] = gy;
    run.compute_groups[2] = 1;
    pl_pass_run(gpu, &run);
    return true;
}

static bool lsfg_find_formats(struct mp_lsfg_ctx *ctx, pl_gpu gpu)
{
    enum pl_fmt_caps req = PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_STORABLE;

    ctx->fmt_rgba8 = pl_find_fmt(gpu, PL_FMT_UNORM, 4, 8, 8, req);
    ctx->fmt_r8 = pl_find_fmt(gpu, PL_FMT_UNORM, 1, 8, 8, req);
    ctx->fmt_rgba16f = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 16, 0, req);
    return ctx->fmt_rgba8 && ctx->fmt_r8 && ctx->fmt_rgba16f;
}

static bool lsfg_alloc_runtime(struct mp_lsfg_ctx *ctx, pl_gpu gpu)
{
    int m = ctx->dag_m;
    float flow_scale = ctx->dag_flow_scale;
    float inv_flow = 1.0f / flow_scale;
    int flow_w = MPMAX(1, (int)((float)ctx->dag_w / inv_flow));
    int flow_h = MPMAX(1, (int)((float)ctx->dag_h / inv_flow));
    ctx->flow_w = flow_w;
    ctx->flow_h = flow_h;

    static const unsigned char black_pixels[4 * 4 * 4] = {0};
    pl_tex_destroy(gpu, &ctx->black);
    ctx->black = pl_tex_create(gpu, pl_tex_params(
        .w = 4,
        .h = 4,
        .format = ctx->fmt_rgba8,
        .sampleable = true,
        .initial_data = black_pixels,
    ));
    if (!ctx->black)
        return false;

    lsfg_init_constants(&ctx->cpre, ctx->dag_hdr, inv_flow, 0.5f);
    lsfg_init_constants(&ctx->cmain, ctx->dag_hdr, inv_flow, 0.5f);
    ctx->cbuf_pre = pl_buf_create(gpu, pl_buf_params(
        .size = sizeof(ctx->cpre),
        .host_writable = true,
        .uniform = true,
        .initial_data = &ctx->cpre
    ));
    ctx->cbuf_main = pl_buf_create(gpu, pl_buf_params(
        .size = sizeof(ctx->cmain),
        .host_writable = true,
        .uniform = true,
        .initial_data = &ctx->cmain
    ));
    if (!ctx->cbuf_pre || !ctx->cbuf_main)
        return false;

    for (int i = 0; i < LSFG_LEVELS; i++) {
        if (!lsfg_create_tex(gpu, lsfg_shift_dim(flow_w, i), lsfg_shift_dim(flow_h, i),
                             ctx->fmt_r8, true, &ctx->mipmaps[i]))
            return false;
    }

    for (int level = 0; level < LSFG_LEVELS; level++) {
        int mw = ctx->mipmaps[level]->params.w;
        int mh = ctx->mipmaps[level]->params.h;
        int half_w = lsfg_add_shift_dim(mw, 1, 1);
        int half_h = lsfg_add_shift_dim(mh, 1, 1);
        int quarter_w = lsfg_add_shift_dim(half_w, 1, 1);
        int quarter_h = lsfg_add_shift_dim(half_h, 1, 1);

        for (int i = 0; i < m; i++) {
            if (!lsfg_create_tex(gpu, half_w, half_h, ctx->fmt_rgba8, true,
                                 &ctx->alpha0[level].temp0[i]))
                return false;
            if (!lsfg_create_tex(gpu, half_w, half_h, ctx->fmt_rgba8, true,
                                 &ctx->alpha0[level].temp1[i]))
                return false;
        }
        for (int i = 0; i < 2 * m; i++) {
            if (!lsfg_create_tex(gpu, quarter_w, quarter_h, ctx->fmt_rgba8, true,
                                 &ctx->alpha0[level].out[i]))
                return false;
        }

        int temporal = level == 0 ? 3 : 2;
        ctx->alpha1[level].temporal = temporal;
        for (int t = 0; t < temporal; t++) {
            for (int i = 0; i < 2 * m; i++) {
                if (!lsfg_create_tex(gpu, quarter_w, quarter_h, ctx->fmt_rgba8, true,
                                     &ctx->alpha1[level].out[t][i]))
                    return false;
            }
        }
    }

    int beta_w = ctx->alpha1[0].out[0][0]->params.w;
    int beta_h = ctx->alpha1[0].out[0][0]->params.h;
    for (int i = 0; i < 2; i++) {
        if (!lsfg_create_tex(gpu, beta_w, beta_h, ctx->fmt_rgba8, true, &ctx->beta0_out[i]))
            return false;
        if (!lsfg_create_tex(gpu, beta_w, beta_h, ctx->fmt_rgba8, true, &ctx->beta1_temp0[i]))
            return false;
        if (!lsfg_create_tex(gpu, beta_w, beta_h, ctx->fmt_rgba8, true, &ctx->beta1_temp1[i]))
            return false;
    }
    for (int i = 0; i < 6; i++) {
        if (!lsfg_create_tex(gpu, lsfg_shift_dim(beta_w, i), lsfg_shift_dim(beta_h, i),
                             ctx->fmt_r8, true, &ctx->beta1_out[i]))
            return false;
    }

    for (int j = 0; j < LSFG_GAMMA_STAGES; j++) {
        int level = 6 - j;
        int w = ctx->alpha1[level].out[0][0]->params.w;
        int h = ctx->alpha1[level].out[0][0]->params.h;

        for (int i = 0; i < 3; i++) {
            if (!lsfg_create_tex(gpu, w, h, ctx->fmt_rgba8, true, &ctx->gamma0[j].out[i]))
                return false;
        }
        for (int i = 0; i < 2 * m; i++) {
            if (!lsfg_create_tex(gpu, w, h, ctx->fmt_rgba8, true, &ctx->gamma1[j].temp0[i]))
                return false;
            if (!lsfg_create_tex(gpu, w, h, ctx->fmt_rgba8, true, &ctx->gamma1[j].temp1[i]))
                return false;
        }
        if (!lsfg_create_tex(gpu, w, h, ctx->fmt_rgba16f, true, &ctx->gamma1[j].out))
            return false;

        if (j < 4)
            continue;
        int d = j - 4;
        for (int i = 0; i < 3; i++) {
            if (!lsfg_create_tex(gpu, w, h, ctx->fmt_rgba8, true, &ctx->delta0[d].out0[i]))
                return false;
        }
        for (int i = 0; i < m; i++) {
            if (!lsfg_create_tex(gpu, w, h, ctx->fmt_rgba8, true, &ctx->delta0[d].out1[i]))
                return false;
        }
        for (int i = 0; i < 2 * m; i++) {
            if (!lsfg_create_tex(gpu, w, h, ctx->fmt_rgba8, true, &ctx->delta1[d].temp0[i]))
                return false;
            if (!lsfg_create_tex(gpu, w, h, ctx->fmt_rgba8, true, &ctx->delta1[d].temp1[i]))
                return false;
        }
        if (!lsfg_create_tex(gpu, w, h, ctx->fmt_rgba16f, true, &ctx->delta1[d].out0))
            return false;
        if (!lsfg_create_tex(gpu, w, h, ctx->fmt_rgba16f, true, &ctx->delta1[d].out1))
            return false;
    }

    return true;
}

static bool lsfg_compile_all_passes(struct mp_lsfg_ctx *ctx, pl_gpu gpu)
{
    if (!lsfg_compile_spec(ctx, gpu, &ctx->pass_mipmaps))
        return false;
    if (!lsfg_compile_spec(ctx, gpu, &ctx->pass_generate))
        return false;
    for (int i = 0; i < 4; i++) {
        if (!lsfg_compile_spec(ctx, gpu, &ctx->pass_alpha[i]))
            return false;
    }
    for (int i = 0; i < 5; i++) {
        if (!lsfg_compile_spec(ctx, gpu, &ctx->pass_beta[i]))
            return false;
    }
    for (int i = 0; i < 5; i++) {
        if (!lsfg_compile_spec(ctx, gpu, &ctx->pass_gamma[i]))
            return false;
    }
    for (int i = 0; i < 10; i++) {
        if (!lsfg_compile_spec(ctx, gpu, &ctx->pass_delta[i]))
            return false;
    }
    return true;
}

static bool lsfg_ensure_runtime(struct mp_lsfg_ctx *ctx, pl_gpu gpu, pl_tex out_rgb)
{
    bool perf = ctx->opts.performance_mode != 0;
    int m = perf ? 1 : 2;
    bool hdr = out_rgb->params.format->type == PL_FMT_FLOAT;
    float flow_scale = ctx->opts.flow_scale;
    if (flow_scale < 0.25f || flow_scale > 1.0f) {
        mp_err(ctx->log,
               "lsfg: flow-scale %.3f out of supported range [0.25, 1.0]\n",
               flow_scale);
        return false;
    }

    bool recreate = ctx->need_recreate ||
                    !ctx->dag_ready ||
                    ctx->dag_gpu != gpu ||
                    ctx->dag_w != out_rgb->params.w ||
                    ctx->dag_h != out_rgb->params.h ||
                    ctx->dag_m != m ||
                    ctx->dag_perf != perf ||
                    ctx->dag_hdr != hdr ||
                    ctx->dag_out_fmt != out_rgb->params.format ||
                    fabsf(ctx->dag_flow_scale - flow_scale) > 1e-6f;

    if (!recreate)
        return true;

    lsfg_destroy_runtime(ctx);

    ctx->dag_gpu = gpu;
    ctx->dag_out_fmt = out_rgb->params.format;
    ctx->dag_w = out_rgb->params.w;
    ctx->dag_h = out_rgb->params.h;
    ctx->dag_m = m;
    ctx->dag_perf = perf;
    ctx->dag_hdr = hdr;
    ctx->dag_flow_scale = flow_scale;
    ctx->frame_index = 0;
    ctx->need_recreate = false;

    if (!lsfg_find_formats(ctx, gpu)) {
        mp_err(ctx->log, "lsfg: missing required internal formats (rgba8/r8/rgba16f)\n");
        return false;
    }
    lsfg_setup_pass_specs(ctx);
    if (!lsfg_alloc_runtime(ctx, gpu)) {
        mp_err(ctx->log, "lsfg: failed to allocate full-DAG runtime textures\n");
        return false;
    }
    if (!lsfg_compile_all_passes(ctx, gpu)) {
        mp_err(ctx->log, "lsfg: failed to compile full-DAG passes\n");
        return false;
    }

    ctx->dag_ready = true;
    return true;
}

static bool lsfg_run_prepass(struct mp_lsfg_ctx *ctx, pl_gpu gpu, pl_tex cur_rgb)
{
    int m = ctx->dag_m;
    size_t fi = (size_t)ctx->frame_index;

    pl_buf pre_cb[1] = { ctx->cbuf_pre };

    pl_tex mip_in[1] = { cur_rgb };
    if (!lsfg_dispatch(ctx, gpu, &ctx->pass_mipmaps, pre_cb, 1,
                       mip_in, 1, ctx->mipmaps, 7, ctx->flow_w, ctx->flow_h))
        return false;

    for (int i = 0; i < LSFG_LEVELS; i++) {
        int level = 6 - i;
        int w = ctx->alpha0[level].temp0[0]->params.w;
        int h = ctx->alpha0[level].temp0[0]->params.h;

        pl_tex a0_in[1] = { ctx->mipmaps[level] };
        if (!lsfg_dispatch(ctx, gpu, &ctx->pass_alpha[0], NULL, 0,
                           a0_in, 1, ctx->alpha0[level].temp0, m, w, h))
            return false;
        if (!lsfg_dispatch(ctx, gpu, &ctx->pass_alpha[1], NULL, 0,
                           ctx->alpha0[level].temp0, m, ctx->alpha0[level].temp1, m, w, h))
            return false;

        int qw = ctx->alpha0[level].out[0]->params.w;
        int qh = ctx->alpha0[level].out[0]->params.h;
        if (!lsfg_dispatch(ctx, gpu, &ctx->pass_alpha[2], NULL, 0,
                           ctx->alpha0[level].temp1, m, ctx->alpha0[level].out, 2 * m, qw, qh))
            return false;

        int temporal = ctx->alpha1[level].temporal;
        int ti = (int)(fi % (size_t)temporal);
        if (!lsfg_dispatch(ctx, gpu, &ctx->pass_alpha[3], NULL, 0,
                           ctx->alpha0[level].out, 2 * m, ctx->alpha1[level].out[ti], 2 * m, qw, qh))
            return false;
    }

    {
        int temporal = ctx->alpha1[0].temporal;
        int ti = (int)(fi % (size_t)temporal);
        int sampled = 0;
        pl_tex beta0_in[LSFG_MAX_SAMPLED_BINDINGS] = {0};
        int s0 = (ti + temporal - 2) % temporal;
        int s1 = (ti + temporal - 1) % temporal;
        int s2 = ti;
        for (int i = 0; i < 2 * m; i++)
            beta0_in[sampled++] = ctx->alpha1[0].out[s0][i];
        for (int i = 0; i < 2 * m; i++)
            beta0_in[sampled++] = ctx->alpha1[0].out[s1][i];
        for (int i = 0; i < 2 * m; i++)
            beta0_in[sampled++] = ctx->alpha1[0].out[s2][i];

        int w = ctx->beta0_out[0]->params.w;
        int h = ctx->beta0_out[0]->params.h;
        if (!lsfg_dispatch(ctx, gpu, &ctx->pass_beta[0], NULL, 0,
                           beta0_in, sampled, ctx->beta0_out, 2, w, h))
            return false;
    }

    {
        int w = ctx->beta1_temp0[0]->params.w;
        int h = ctx->beta1_temp0[0]->params.h;
        if (!lsfg_dispatch(ctx, gpu, &ctx->pass_beta[1], NULL, 0,
                           ctx->beta0_out, 2, ctx->beta1_temp0, 2, w, h))
            return false;
        if (!lsfg_dispatch(ctx, gpu, &ctx->pass_beta[2], NULL, 0,
                           ctx->beta1_temp0, 2, ctx->beta1_temp1, 2, w, h))
            return false;
        if (!lsfg_dispatch(ctx, gpu, &ctx->pass_beta[3], NULL, 0,
                           ctx->beta1_temp1, 2, ctx->beta1_temp0, 2, w, h))
            return false;
        if (!lsfg_dispatch(ctx, gpu, &ctx->pass_beta[4], pre_cb, 1,
                           ctx->beta1_temp0, 2, ctx->beta1_out, 6, w, h))
            return false;
    }

    return true;
}

static bool lsfg_run_main(struct mp_lsfg_ctx *ctx, pl_gpu gpu, pl_tex prev_rgb, pl_tex cur_rgb, pl_tex out_rgb)
{
    int m = ctx->dag_m;
    size_t fi = (size_t)ctx->frame_index;
    pl_buf main_cb[1] = { ctx->cbuf_main };

    for (int j = 0; j < LSFG_GAMMA_STAGES; j++) {
        int level = 6 - j;
        int temporal = ctx->alpha1[level].temporal;
        int ti = (int)(fi % (size_t)temporal);
        int tp = (ti + temporal - 1) % temporal;
        int w = ctx->gamma0[j].out[0]->params.w;
        int h = ctx->gamma0[j].out[0]->params.h;

        pl_tex g0_in[LSFG_MAX_SAMPLED_BINDINGS] = {0};
        int g0n = 0;
        for (int i = 0; i < 2 * m; i++)
            g0_in[g0n++] = ctx->alpha1[level].out[tp][i];
        for (int i = 0; i < 2 * m; i++)
            g0_in[g0n++] = ctx->alpha1[level].out[ti][i];
        g0_in[g0n++] = (j == 0) ? ctx->black : ctx->gamma1[j - 1].out;

        if (!lsfg_dispatch(ctx, gpu, &ctx->pass_gamma[0], main_cb, 1,
                           g0_in, g0n, ctx->gamma0[j].out, 3, w, h))
            return false;
        if (!lsfg_dispatch(ctx, gpu, &ctx->pass_gamma[1], NULL, 0,
                           ctx->gamma0[j].out, 3, ctx->gamma1[j].temp0, 2 * m, w, h))
            return false;
        if (!lsfg_dispatch(ctx, gpu, &ctx->pass_gamma[2], NULL, 0,
                           ctx->gamma1[j].temp0, 2 * m, ctx->gamma1[j].temp1, 2 * m, w, h))
            return false;
        if (!lsfg_dispatch(ctx, gpu, &ctx->pass_gamma[3], NULL, 0,
                           ctx->gamma1[j].temp1, 2 * m, ctx->gamma1[j].temp0, 2 * m, w, h))
            return false;

        pl_tex g1_in[LSFG_MAX_SAMPLED_BINDINGS] = {0};
        int g1n = 0;
        for (int i = 0; i < 2 * m; i++)
            g1_in[g1n++] = ctx->gamma1[j].temp0[i];
        g1_in[g1n++] = (j == 0) ? ctx->black : ctx->gamma1[j - 1].out;
        g1_in[g1n++] = ctx->beta1_out[6 - j];

        pl_tex g1_out[1] = { ctx->gamma1[j].out };
        if (!lsfg_dispatch(ctx, gpu, &ctx->pass_gamma[4], main_cb, 1,
                           g1_in, g1n, g1_out, 1, w, h))
            return false;

        if (j < 4)
            continue;

        int d = j - 4;
        pl_tex d0a_in[LSFG_MAX_SAMPLED_BINDINGS] = {0};
        int d0a_n = 0;
        for (int i = 0; i < 2 * m; i++)
            d0a_in[d0a_n++] = ctx->alpha1[level].out[tp][i];
        for (int i = 0; i < 2 * m; i++)
            d0a_in[d0a_n++] = ctx->alpha1[level].out[ti][i];
        d0a_in[d0a_n++] = (d == 0) ? ctx->black : ctx->delta1[d - 1].out0;

        if (!lsfg_dispatch(ctx, gpu, &ctx->pass_delta[0], main_cb, 1,
                           d0a_in, d0a_n, ctx->delta0[d].out0, 3, w, h))
            return false;

        pl_tex d0b_in[LSFG_MAX_SAMPLED_BINDINGS] = {0};
        int d0b_n = 0;
        for (int i = 0; i < 2 * m; i++)
            d0b_in[d0b_n++] = ctx->alpha1[level].out[tp][i];
        for (int i = 0; i < 2 * m; i++)
            d0b_in[d0b_n++] = ctx->alpha1[level].out[ti][i];
        d0b_in[d0b_n++] = ctx->gamma1[j - 1].out;
        d0b_in[d0b_n++] = (d == 0) ? ctx->black : ctx->delta1[d - 1].out0;

        if (!lsfg_dispatch(ctx, gpu, &ctx->pass_delta[5], main_cb, 1,
                           d0b_in, d0b_n, ctx->delta0[d].out1, m, w, h))
            return false;

        if (!lsfg_dispatch(ctx, gpu, &ctx->pass_delta[1], NULL, 0,
                           ctx->delta0[d].out0, 3, ctx->delta1[d].temp0, 2 * m, w, h))
            return false;
        if (!lsfg_dispatch(ctx, gpu, &ctx->pass_delta[2], NULL, 0,
                           ctx->delta1[d].temp0, 2 * m, ctx->delta1[d].temp1, 2 * m, w, h))
            return false;
        if (!lsfg_dispatch(ctx, gpu, &ctx->pass_delta[3], NULL, 0,
                           ctx->delta1[d].temp1, 2 * m, ctx->delta1[d].temp0, 2 * m, w, h))
            return false;

        pl_tex d1a_in[LSFG_MAX_SAMPLED_BINDINGS] = {0};
        int d1a_n = 0;
        for (int i = 0; i < 2 * m; i++)
            d1a_in[d1a_n++] = ctx->delta1[d].temp0[i];
        d1a_in[d1a_n++] = (d == 0) ? ctx->black : ctx->delta1[d - 1].out0;
        d1a_in[d1a_n++] = ctx->beta1_out[6 - j];
        pl_tex d1a_out[1] = { ctx->delta1[d].out0 };
        if (!lsfg_dispatch(ctx, gpu, &ctx->pass_delta[4], main_cb, 1,
                           d1a_in, d1a_n, d1a_out, 1, w, h))
            return false;

        if (!lsfg_dispatch(ctx, gpu, &ctx->pass_delta[6], NULL, 0,
                           ctx->delta0[d].out1, m, ctx->delta1[d].temp0, m, w, h))
            return false;
        if (!lsfg_dispatch(ctx, gpu, &ctx->pass_delta[7], NULL, 0,
                           ctx->delta1[d].temp0, m, ctx->delta1[d].temp1, m, w, h))
            return false;
        if (!lsfg_dispatch(ctx, gpu, &ctx->pass_delta[8], NULL, 0,
                           ctx->delta1[d].temp1, m, ctx->delta1[d].temp0, m, w, h))
            return false;

        pl_tex d1b_in[LSFG_MAX_SAMPLED_BINDINGS] = {0};
        int d1b_n = 0;
        for (int i = 0; i < m; i++)
            d1b_in[d1b_n++] = ctx->delta1[d].temp0[i];
        d1b_in[d1b_n++] = (d == 0) ? ctx->black : ctx->delta1[d - 1].out1;
        pl_tex d1b_out[1] = { ctx->delta1[d].out1 };
        if (!lsfg_dispatch(ctx, gpu, &ctx->pass_delta[9], main_cb, 1,
                           d1b_in, d1b_n, d1b_out, 1, w, h))
            return false;
    }

    pl_tex gen_in[5] = {
        prev_rgb,
        cur_rgb,
        ctx->gamma1[6].out,
        ctx->delta1[2].out0,
        ctx->delta1[2].out1,
    };
    pl_tex gen_out[1] = { out_rgb };
    if (!lsfg_dispatch(ctx, gpu, &ctx->pass_generate, main_cb, 1,
                       gen_in, 5, gen_out, 1, ctx->dag_w, ctx->dag_h))
        return false;

    return true;
}

const char *mp_lsfg_gen_status_str(enum mp_lsfg_gen_status status)
{
    switch (status) {
    case MP_LSFG_GEN_OK: return "ok";
    case MP_LSFG_GEN_DISABLED: return "disabled";
    case MP_LSFG_GEN_BAD_INPUT: return "bad-input";
    case MP_LSFG_GEN_SIZE_MISMATCH: return "size-mismatch";
    case MP_LSFG_GEN_FORMAT_MISMATCH: return "format-mismatch";
    case MP_LSFG_GEN_OUTPUT_NOT_STORABLE: return "output-not-storable";
    case MP_LSFG_GEN_PASS_CREATE_FAILED: return "pass-create-failed";
    default: return "unknown";
    }
}

struct mp_lsfg_ctx *mp_lsfg_create(void *ta_parent, struct mp_log *log)
{
    struct mp_lsfg_ctx *ctx = talloc_zero(ta_parent, struct mp_lsfg_ctx);
    if (!ctx)
        return NULL;

    ctx->log = log;
    ctx->opts.strict = true;
    ctx->opts.multiplier = 2;
    ctx->opts.flow_scale = 1.0f;
    ctx->dll = lsfg_dll_create(ctx, log);
    return ctx;
}

void mp_lsfg_destroy(struct mp_lsfg_ctx **ctx)
{
    if (!ctx || !*ctx)
        return;
    lsfg_destroy_runtime(*ctx);
    lsfg_dll_destroy(&(*ctx)->dll);
    TA_FREEP(ctx);
}

void mp_lsfg_update_opts(struct mp_lsfg_ctx *ctx, const struct mp_lsfg_opts *opts)
{
    if (!ctx || !opts)
        return;

    bool path_changed = !lsfg_streq(ctx->dll_path, opts->dll_path);

    if (!ctx->initialized) {
        ctx->opts = *opts;
        ctx->opts.dll_path = NULL;
        TA_FREEP(&ctx->dll_path);
        ctx->dll_path = opts->dll_path ? talloc_strdup(ctx, opts->dll_path) : NULL;
        ctx->opts.dll_path = ctx->dll_path;
        ctx->initialized = true;
        return;
    }

    bool was_enabled = ctx->opts.enable;
    bool recreate_change =
        ctx->opts.multiplier != opts->multiplier ||
        ctx->opts.performance_mode != opts->performance_mode ||
        fabsf(ctx->opts.flow_scale - opts->flow_scale) > 1e-6f ||
        path_changed;

    ctx->opts = *opts;
    ctx->opts.dll_path = NULL;
    if (path_changed) {
        TA_FREEP(&ctx->dll_path);
        ctx->dll_path = opts->dll_path ? talloc_strdup(ctx, opts->dll_path) : NULL;
    }
    ctx->opts.dll_path = ctx->dll_path;

    if (!opts->enable) {
        ctx->need_soft_reset = true;
        return;
    }

    if (!was_enabled && opts->enable) {
        ctx->need_soft_reset = true;
        return;
    }

    if (recreate_change)
        ctx->need_recreate = true;
}

void mp_lsfg_on_reset(struct mp_lsfg_ctx *ctx)
{
    if (!ctx)
        return;
    ctx->need_soft_reset = true;
}

void mp_lsfg_on_reconfig(struct mp_lsfg_ctx *ctx)
{
    if (!ctx)
        return;
    ctx->need_recreate = true;
}

void mp_lsfg_begin_frame(struct mp_lsfg_ctx *ctx,
                         const struct mp_lsfg_frame_info *info,
                         struct mp_lsfg_runtime *runtime)
{
    if (!ctx || !info)
        return;

    struct mp_lsfg_runtime dummy = {0};
    if (!runtime)
        runtime = &dummy;
    memset(runtime, 0, sizeof(*runtime));

    runtime->enabled = ctx->opts.enable;
    if (!ctx->opts.enable)
        return;

    if (ctx->need_recreate) {
        runtime->requested_recreate = true;
        ctx->phase_index = 0;
    }

    if (ctx->need_soft_reset) {
        ctx->need_soft_reset = false;
        ctx->phase_index = 0;
        ctx->frame_index = 0;
    }

    bool valid_pair = info->display_synced &&
                      !info->still &&
                      info->has_current &&
                      info->num_frames >= 2 &&
                      !info->pending_reset &&
                      !info->queue_more &&
                      info->pts_monotonic;

    runtime->valid_pair = valid_pair;
    if (!valid_pair) {
        runtime->fallback = true;
        return;
    }

    unsigned steps = (unsigned)(ctx->opts.multiplier > 1 ? ctx->opts.multiplier - 1 : 1);
    ctx->phase_index = (ctx->phase_index % steps) + 1;

    runtime->phase_index = ctx->phase_index;
    runtime->phase_t = (float)ctx->phase_index / (float)(steps + 1);
    runtime->would_generate = true;

    if (!ctx->logged_path_notice) {
        mp_info(ctx->log,
                "lsfg full-DAG path is active (mpv in-process).\n");
        ctx->logged_path_notice = true;
    }
}

enum mp_lsfg_gen_status mp_lsfg_generate(struct mp_lsfg_ctx *ctx, pl_gpu gpu,
                                         pl_tex prev_rgb, pl_tex cur_rgb, pl_tex out_rgb,
                                         float phase_t)
{
    if (!ctx || !gpu || !prev_rgb || !cur_rgb || !out_rgb)
        return MP_LSFG_GEN_BAD_INPUT;
    if (!ctx->opts.enable)
        return MP_LSFG_GEN_DISABLED;

    if (prev_rgb->params.w != cur_rgb->params.w ||
        prev_rgb->params.h != cur_rgb->params.h ||
        prev_rgb->params.w != out_rgb->params.w ||
        prev_rgb->params.h != out_rgb->params.h)
        return MP_LSFG_GEN_SIZE_MISMATCH;

    if (prev_rgb->params.format != cur_rgb->params.format ||
        prev_rgb->params.format != out_rgb->params.format)
        return MP_LSFG_GEN_FORMAT_MISMATCH;

    if (!out_rgb->params.storable ||
        !(out_rgb->params.format->caps & PL_FMT_CAP_STORABLE))
        return MP_LSFG_GEN_OUTPUT_NOT_STORABLE;

    if (!lsfg_ensure_runtime(ctx, gpu, out_rgb))
        return MP_LSFG_GEN_PASS_CREATE_FAILED;

    ctx->cmain.timestamp = MPCLAMP(phase_t, 0.0f, 1.0f);
    pl_buf_write(gpu, ctx->cbuf_main, 0, &ctx->cmain, sizeof(ctx->cmain));

    if (!lsfg_run_prepass(ctx, gpu, cur_rgb))
        return MP_LSFG_GEN_PASS_CREATE_FAILED;
    if (!lsfg_run_main(ctx, gpu, prev_rgb, cur_rgb, out_rgb))
        return MP_LSFG_GEN_PASS_CREATE_FAILED;

    ctx->frame_index++;
    return MP_LSFG_GEN_OK;
}
