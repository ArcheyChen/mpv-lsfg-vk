#include "video/out/lsfg/lsfg_dll.h"

#include "config.h"
#include "common/common.h"
#include "common/msg.h"
#include "ta/ta_talloc.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#if HAVE_SPIRV_CROSS
#include <spirv_cross/spirv_cross_c.h>
#endif

struct lsfg_rsrc_entry {
    uint32_t id;
    uint8_t *data;
    size_t size;
    char *glsl;
};

struct lsfg_dll_ctx {
    struct mp_log *log;
    char *loaded_path;
    struct lsfg_rsrc_entry *entries;
    size_t num_entries;
};

static bool path_exists(const char *path)
{
    struct stat st;
    return path && path[0] && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static char *lsfg_read_text_file(void *ta_parent, const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp)
        return NULL;

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    long size = ftell(fp);
    if (size < 0) {
        fclose(fp);
        return NULL;
    }
    rewind(fp);

    char *buf = talloc_array(ta_parent, char, (size_t)size + 1);
    if (!buf) {
        fclose(fp);
        return NULL;
    }

    size_t got = fread(buf, 1, (size_t)size, fp);
    fclose(fp);
    if (got != (size_t)size) {
        talloc_free(buf);
        return NULL;
    }
    buf[size] = '\0';
    return buf;
}

static char *vdf_unescape_path(void *ta_parent, const char *src)
{
    if (!src)
        return NULL;

    size_t n = strlen(src);
    char *out = talloc_array(ta_parent, char, n + 1);
    if (!out)
        return NULL;

    size_t j = 0;
    for (size_t i = 0; i < n; i++) {
        if (src[i] == '\\' && (i + 1) < n) {
            char c = src[i + 1];
            if (c == '\\' || c == '"') {
                out[j++] = c;
                i++;
                continue;
            }
        }
        out[j++] = src[i];
    }
    out[j] = '\0';
    return out;
}

static char *dll_from_library_dir(void *ta_parent, const char *libdir)
{
    if (!libdir || !libdir[0])
        return NULL;
    char *candidate = talloc_asprintf(ta_parent,
        "%s/steamapps/common/Lossless Scaling/Lossless.dll", libdir);
    if (path_exists(candidate))
        return candidate;
    talloc_free(candidate);
    return NULL;
}

static char *find_dll_near_executable(void *ta_parent)
{
#if !defined(__linux__)
    (void)ta_parent;
    return NULL;
#else
    char exe_path[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (n <= 0 || n >= (ssize_t)sizeof(exe_path))
        return NULL;
    exe_path[n] = '\0';

    char *slash = strrchr(exe_path, '/');
    if (!slash || slash == exe_path)
        return NULL;
    *slash = '\0';

    char *candidate = talloc_asprintf(ta_parent, "%s/Lossless.dll", exe_path);
    if (candidate && path_exists(candidate))
        return candidate;
    talloc_free(candidate);
    return NULL;
#endif
}

static char *find_dll_from_libraryfolders(void *ta_parent, const char *steam_root)
{
    if (!ta_parent || !steam_root || !steam_root[0])
        return NULL;

    const char *rel_paths[] = {
        "config/libraryfolders.vdf",
        "steamapps/libraryfolders.vdf",
    };

    for (int rp = 0; rp < MP_ARRAY_SIZE(rel_paths); rp++) {
        char *vdf_path = talloc_asprintf(NULL, "%s/%s", steam_root, rel_paths[rp]);
        if (!vdf_path)
            continue;
        if (!path_exists(vdf_path)) {
            talloc_free(vdf_path);
            continue;
        }

        char *text = lsfg_read_text_file(NULL, vdf_path);
        talloc_free(vdf_path);
        if (!text)
            continue;

        const char *p = text;
        while ((p = strstr(p, "\"path\""))) {
            p += 6;
            const char *q = strchr(p, '"');
            if (!q)
                break;
            q++;
            const char *e = q;
            bool escaped = false;
            while (*e) {
                if (!escaped && *e == '"')
                    break;
                escaped = (!escaped && *e == '\\');
                if (escaped && *e != '\\')
                    escaped = false;
                e++;
            }
            if (*e != '"') {
                p = q;
                continue;
            }

            char *raw = talloc_strndup(NULL, q, e - q);
            char *libdir = vdf_unescape_path(NULL, raw);
            talloc_free(raw);
            char *dll = dll_from_library_dir(ta_parent, libdir);
            talloc_free(libdir);
            if (dll) {
                talloc_free(text);
                return dll;
            }
            p = e + 1;
        }

        talloc_free(text);
    }

    return NULL;
}

static char *find_default_dll_path(void *ta_parent)
{
    if (!ta_parent)
        return NULL;

    const char *home = getenv("HOME");
    static const char *const steam_roots[] = {
        ".local/share/Steam",
        ".steam/steam",
        ".steam/debian-installation",
        ".var/app/com.valvesoftware.Steam/.local/share/Steam",
        "snap/steam/common/.local/share/Steam",
    };

    if (home && home[0]) {
        for (int i = 0; i < MP_ARRAY_SIZE(steam_roots); i++) {
            char *steam_root = talloc_asprintf(NULL, "%s/%s", home, steam_roots[i]);
            if (!steam_root)
                continue;

            char *from_vdf = find_dll_from_libraryfolders(ta_parent, steam_root);
            if (from_vdf) {
                talloc_free(steam_root);
                return from_vdf;
            }

            char *fallback_lib = dll_from_library_dir(ta_parent, steam_root);
            talloc_free(steam_root);
            if (fallback_lib)
                return fallback_lib;
        }
    }

    const char *xdg = getenv("XDG_DATA_HOME");
    if (xdg && xdg[0]) {
        char *steam_root = talloc_asprintf(NULL, "%s/Steam", xdg);
        if (steam_root) {
            char *from_vdf = find_dll_from_libraryfolders(ta_parent, steam_root);
            if (from_vdf) {
                talloc_free(steam_root);
                return from_vdf;
            }
            char *fallback_lib = dll_from_library_dir(ta_parent, steam_root);
            talloc_free(steam_root);
            if (fallback_lib)
                return fallback_lib;
        }
    }

    char *near_exe = find_dll_near_executable(ta_parent);
    if (near_exe)
        return near_exe;

    if (path_exists("Lossless.dll"))
        return talloc_strdup(ta_parent, "Lossless.dll");

    return NULL;
}

static inline bool checked_add(size_t a, size_t b, size_t *out)
{
    if (a > SIZE_MAX - b)
        return false;
    *out = a + b;
    return true;
}

static bool bounds_ok(size_t off, size_t need, size_t size)
{
    size_t end = 0;
    return checked_add(off, need, &end) && end <= size;
}

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

struct pe_section {
    uint32_t vaddr;
    uint32_t vsize;
    uint32_t foff;
    uint32_t fsize;
};

static bool rva_to_off(uint32_t rva, const struct pe_section *secs, int nsec, size_t *off)
{
    for (int i = 0; i < nsec; i++) {
        uint32_t start = secs[i].vaddr;
        uint32_t span = secs[i].vsize ? secs[i].vsize : secs[i].fsize;
        uint32_t end = start + span;
        if (rva < start || rva >= end)
            continue;
        *off = (size_t)secs[i].foff + (size_t)(rva - start);
        return true;
    }
    return false;
}

static void clear_entries(struct lsfg_dll_ctx *ctx)
{
    if (!ctx)
        return;
    for (size_t i = 0; i < ctx->num_entries; i++)
        free(ctx->entries[i].data);
    free(ctx->entries);
    ctx->entries = NULL;
    ctx->num_entries = 0;
    TA_FREEP(&ctx->loaded_path);
}

static bool append_entry(struct lsfg_dll_ctx *ctx, uint32_t id, const uint8_t *src, size_t len)
{
    struct lsfg_rsrc_entry *nr = realloc(ctx->entries, sizeof(*nr) * (ctx->num_entries + 1));
    if (!nr)
        return false;
    ctx->entries = nr;

    uint8_t *copy = malloc(len ? len : 1);
    if (!copy)
        return false;
    if (len)
        memcpy(copy, src, len);

    ctx->entries[ctx->num_entries] = (struct lsfg_rsrc_entry) {
        .id = id,
        .data = copy,
        .size = len,
        .glsl = NULL,
    };
    ctx->num_entries++;
    return true;
}

static int cmp_entry(const void *a, const void *b)
{
    const struct lsfg_rsrc_entry *ea = a;
    const struct lsfg_rsrc_entry *eb = b;
    if (ea->id < eb->id) return -1;
    if (ea->id > eb->id) return 1;
    return 0;
}

static bool parse_resources(struct lsfg_dll_ctx *ctx, const uint8_t *dll, size_t dll_size)
{
    if (!bounds_ok(0, 0x40, dll_size) || rd16(dll) != 0x5A4D)
        return false;

    uint32_t pe_off = rd32(dll + 0x3C);
    if (!bounds_ok(pe_off, 24, dll_size) || rd32(dll + pe_off) != 0x00004550)
        return false;

    size_t coff = pe_off + 4;
    uint16_t sec_count = rd16(dll + coff + 2);
    uint16_t opt_size = rd16(dll + coff + 16);
    size_t opt = coff + 20;
    if (!bounds_ok(opt, opt_size, dll_size) || opt_size < 128 || rd16(dll + opt) != 0x20B)
        return false;

    uint32_t rsrc_rva = rd32(dll + opt + 112 + 8 * 2);
    uint32_t rsrc_size = rd32(dll + opt + 112 + 8 * 2 + 4);
    if (!rsrc_rva || !rsrc_size)
        return false;

    size_t sec_tbl = opt + opt_size;
    if (!bounds_ok(sec_tbl, (size_t)sec_count * 40, dll_size))
        return false;

    struct pe_section *secs = talloc_array(NULL, struct pe_section, sec_count);
    if (!secs)
        return false;
    for (int i = 0; i < sec_count; i++) {
        size_t so = sec_tbl + (size_t)i * 40;
        secs[i] = (struct pe_section) {
            .vsize = rd32(dll + so + 8),
            .vaddr = rd32(dll + so + 12),
            .fsize = rd32(dll + so + 16),
            .foff = rd32(dll + so + 20),
        };
    }

    size_t rsrc_base = 0;
    if (!rva_to_off(rsrc_rva, secs, sec_count, &rsrc_base)) {
        talloc_free(secs);
        return false;
    }

    if (!bounds_ok(rsrc_base, 16, dll_size)) {
        talloc_free(secs);
        return false;
    }

    uint16_t type_named = rd16(dll + rsrc_base + 12);
    uint16_t type_ids = rd16(dll + rsrc_base + 14);
    size_t type_entries = rsrc_base + 16;
    size_t type_count = (size_t)type_named + (size_t)type_ids;
    if (!bounds_ok(type_entries, type_count * 8, dll_size)) {
        talloc_free(secs);
        return false;
    }

    size_t rcdata_dir_rel = 0;
    bool found_rcdata = false;
    for (size_t i = 0; i < type_count; i++) {
        size_t eoff = type_entries + i * 8;
        uint32_t name = rd32(dll + eoff);
        uint32_t off = rd32(dll + eoff + 4);
        if (name != 10 || !(off & 0x80000000u))
            continue;
        rcdata_dir_rel = off & 0x7fffffffu;
        found_rcdata = true;
        break;
    }
    if (!found_rcdata || !bounds_ok(rsrc_base + rcdata_dir_rel, 16, dll_size)) {
        talloc_free(secs);
        return false;
    }

    size_t name_dir = rsrc_base + rcdata_dir_rel;
    uint16_t name_named = rd16(dll + name_dir + 12);
    uint16_t name_ids = rd16(dll + name_dir + 14);
    size_t name_count = (size_t)name_named + (size_t)name_ids;
    size_t name_entries = name_dir + 16;
    if (!bounds_ok(name_entries, name_count * 8, dll_size)) {
        talloc_free(secs);
        return false;
    }

    for (size_t i = 0; i < name_count; i++) {
        size_t ne = name_entries + i * 8;
        uint32_t name = rd32(dll + ne);
        uint32_t off = rd32(dll + ne + 4);
        uint32_t id = name & 0xffffu;
        if (!(off & 0x80000000u))
            continue;

        size_t lang_dir = rsrc_base + (off & 0x7fffffffu);
        if (!bounds_ok(lang_dir, 16, dll_size))
            continue;
        uint16_t lang_named = rd16(dll + lang_dir + 12);
        uint16_t lang_ids = rd16(dll + lang_dir + 14);
        size_t lang_count = (size_t)lang_named + (size_t)lang_ids;
        size_t lang_entries = lang_dir + 16;
        if (!lang_count || !bounds_ok(lang_entries, lang_count * 8, dll_size))
            continue;

        size_t le = lang_entries;
        uint32_t loff = rd32(dll + le + 4);
        if (loff & 0x80000000u)
            continue;
        size_t data_entry = rsrc_base + loff;
        if (!bounds_ok(data_entry, 16, dll_size))
            continue;

        uint32_t data_rva = rd32(dll + data_entry);
        uint32_t data_size = rd32(dll + data_entry + 4);
        size_t data_off = 0;
        if (!data_size || !rva_to_off(data_rva, secs, sec_count, &data_off) ||
            !bounds_ok(data_off, data_size, dll_size))
            continue;

        if (!append_entry(ctx, id, dll + data_off, data_size)) {
            talloc_free(secs);
            return false;
        }
    }

    talloc_free(secs);

    if (!ctx->num_entries)
        return false;
    qsort(ctx->entries, ctx->num_entries, sizeof(ctx->entries[0]), cmp_entry);
    return true;
}

static struct lsfg_rsrc_entry *find_entry(struct lsfg_dll_ctx *ctx, uint32_t id)
{
    size_t lo = 0;
    size_t hi = ctx->num_entries;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        uint32_t mid_id = ctx->entries[mid].id;
        if (mid_id == id)
            return &ctx->entries[mid];
        if (mid_id < id)
            lo = mid + 1;
        else
            hi = mid;
    }
    return NULL;
}

static bool is_spirv_blob(const uint8_t *data, size_t size)
{
    return size >= 4 && rd32(data) == 0x07230203u;
}

static char *spirv_to_glsl(struct lsfg_dll_ctx *ctx, const uint8_t *data, size_t size)
{
#if !HAVE_SPIRV_CROSS
    MP_ERR(ctx->log, "lsfg: spirv-cross support is not available in this build\n");
    return NULL;
#else
    if (!data || size < 20 || (size % 4) != 0)
        return NULL;

    spvc_result r = SPVC_SUCCESS;
    spvc_context sc = NULL;
    spvc_parsed_ir ir = NULL;
    spvc_compiler compiler = NULL;
    spvc_compiler_options opts = NULL;
    const char *out = NULL;
    char *ret = NULL;

    r = spvc_context_create(&sc);
    if (r != SPVC_SUCCESS)
        goto done;
    r = spvc_context_parse_spirv(sc, (const SpvId *)data, size / 4, &ir);
    if (r != SPVC_SUCCESS)
        goto done;
    r = spvc_context_create_compiler(sc, SPVC_BACKEND_GLSL, ir,
                                     SPVC_CAPTURE_MODE_TAKE_OWNERSHIP, &compiler);
    if (r != SPVC_SUCCESS)
        goto done;
    r = spvc_compiler_create_compiler_options(compiler, &opts);
    if (r != SPVC_SUCCESS)
        goto done;
    r = spvc_compiler_options_set_uint(opts, SPVC_COMPILER_OPTION_GLSL_VERSION, 450);
    if (r != SPVC_SUCCESS)
        goto done;
    r = spvc_compiler_options_set_bool(opts, SPVC_COMPILER_OPTION_GLSL_ES, SPVC_FALSE);
    if (r != SPVC_SUCCESS)
        goto done;
    r = spvc_compiler_options_set_bool(opts, SPVC_COMPILER_OPTION_GLSL_VULKAN_SEMANTICS, SPVC_TRUE);
    if (r != SPVC_SUCCESS)
        goto done;
    r = spvc_compiler_options_set_bool(opts, SPVC_COMPILER_OPTION_GLSL_ENABLE_420PACK_EXTENSION, SPVC_TRUE);
    if (r != SPVC_SUCCESS)
        goto done;
    r = spvc_compiler_install_compiler_options(compiler, opts);
    if (r != SPVC_SUCCESS)
        goto done;
    r = spvc_compiler_compile(compiler, &out);
    if (r != SPVC_SUCCESS || !out)
        goto done;

    ret = talloc_strdup(ctx, out);
done:
    if (r != SPVC_SUCCESS)
        mp_err(ctx->log, "lsfg: spirv-cross conversion failed: %s\n",
               sc ? spvc_context_get_last_error_string(sc) : "unknown");
    if (sc)
        spvc_context_destroy(sc);
    return ret;
#endif
}

struct lsfg_dll_ctx *lsfg_dll_create(void *ta_parent, struct mp_log *log)
{
    struct lsfg_dll_ctx *ctx = talloc_zero(ta_parent, struct lsfg_dll_ctx);
    if (!ctx)
        return NULL;
    ctx->log = log;
    return ctx;
}

void lsfg_dll_destroy(struct lsfg_dll_ctx **ctx)
{
    if (!ctx || !*ctx)
        return;
    clear_entries(*ctx);
    talloc_free(*ctx);
    *ctx = NULL;
}

bool lsfg_dll_load(struct lsfg_dll_ctx *ctx, const char *user_path)
{
    if (!ctx)
        return false;

    void *tmp = talloc_new(NULL);
    if (!tmp)
        return false;

    char *dll_path = NULL;
    if (user_path && user_path[0])
        dll_path = talloc_strdup(tmp, user_path);
    else
        dll_path = find_default_dll_path(tmp);

    if (!dll_path) {
        mp_err(ctx->log, "lsfg: unable to locate Lossless.dll\n");
        talloc_free(tmp);
        return false;
    }

    if (ctx->loaded_path && strcmp(ctx->loaded_path, dll_path) == 0 && ctx->num_entries > 0) {
        talloc_free(tmp);
        return true;
    }

    FILE *fp = fopen(dll_path, "rb");
    if (!fp) {
        mp_err(ctx->log, "lsfg: unable to open Lossless.dll: %s (%s)\n",
               dll_path, mp_strerror(errno));
        talloc_free(tmp);
        return false;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        talloc_free(tmp);
        return false;
    }
    long sz = ftell(fp);
    if (sz <= 0) {
        fclose(fp);
        talloc_free(tmp);
        return false;
    }
    rewind(fp);

    uint8_t *buf = malloc((size_t)sz);
    if (!buf) {
        fclose(fp);
        talloc_free(tmp);
        return false;
    }
    if (fread(buf, 1, (size_t)sz, fp) != (size_t)sz) {
        free(buf);
        fclose(fp);
        talloc_free(tmp);
        return false;
    }
    fclose(fp);

    clear_entries(ctx);
    bool ok = parse_resources(ctx, buf, (size_t)sz);
    free(buf);
    if (!ok) {
        clear_entries(ctx);
        mp_err(ctx->log, "lsfg: failed to parse RT_RCDATA resources from %s\n", dll_path);
        talloc_free(tmp);
        return false;
    }

    ctx->loaded_path = talloc_strdup(ctx, dll_path);
    mp_info(ctx->log, "lsfg: loaded %zu resources from %s\n", ctx->num_entries, ctx->loaded_path);
    talloc_free(tmp);
    return true;
}

const char *lsfg_dll_get_glsl(struct lsfg_dll_ctx *ctx, uint32_t resource_id)
{
    if (!ctx || !ctx->num_entries)
        return NULL;

    struct lsfg_rsrc_entry *e = find_entry(ctx, resource_id);
    if (!e)
        return NULL;
    if (e->glsl)
        return e->glsl;
    if (!is_spirv_blob(e->data, e->size)) {
        mp_err(ctx->log, "lsfg: resource %u is not SPIR-V\n", resource_id);
        return NULL;
    }

    e->glsl = spirv_to_glsl(ctx, e->data, e->size);
    return e->glsl;
}

const char *lsfg_dll_loaded_path(const struct lsfg_dll_ctx *ctx)
{
    return ctx ? ctx->loaded_path : NULL;
}
