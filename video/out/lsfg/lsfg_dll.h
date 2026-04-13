#pragma once

#include <stdbool.h>
#include <stdint.h>

struct mp_log;
struct lsfg_dll_ctx;

struct lsfg_dll_ctx *lsfg_dll_create(void *ta_parent, struct mp_log *log);
void lsfg_dll_destroy(struct lsfg_dll_ctx **ctx);

bool lsfg_dll_load(struct lsfg_dll_ctx *ctx, const char *user_path);
const char *lsfg_dll_get_glsl(struct lsfg_dll_ctx *ctx, uint32_t resource_id);
const char *lsfg_dll_loaded_path(const struct lsfg_dll_ctx *ctx);
