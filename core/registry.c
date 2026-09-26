#include "registry.h"
#include "minijson.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#if defined(_WIN32)
  #include <windows.h>
#else
  #include <dirent.h>
#endif

static char *read_whole_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc(sz + 1);
    size_t n = fread(buf, 1, sz, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

static void path_join(char *out, size_t out_sz, const char *dir, const char *name) {
    snprintf(out, out_sz, "%s/%s", dir, name);
}

typedef struct { lumen_registry_entry_t *entry; int *count; int cap; } ext_collect_ctx_t;

static void collect_ext(const char *s, void *ud) {
    ext_collect_ctx_t *c = (ext_collect_ctx_t *)ud;
    if (*c->count < c->cap) strncpy(c->entry->extensions[(*c->count)++], s, 15);
}
static void collect_fourcc(const char *s, void *ud) {
    ext_collect_ctx_t *c = (ext_collect_ctx_t *)ud;
    if (*c->count < c->cap) strncpy(c->entry->fourccs[(*c->count)++], s, 7);
}

static int parse_manifest_into(const char *manifest_path, const char *plugin_dir, lumen_registry_entry_t *e) {
    char *text = read_whole_file(manifest_path);
    if (!text) return -1;
    mj_value_t *root = mj_parse(text);
    free(text);
    if (!root) return -1;

    memset(e, 0, sizeof(*e));
    strncpy(e->name, mj_get_string(root, "name", "unknown"), sizeof(e->name) - 1);
    strncpy(e->version, mj_get_string(root, "version", "0.0.0"), sizeof(e->version) - 1);
    const char *kind_str = mj_get_string(root, "kind", "");
    if (strcmp(kind_str, "demuxer") == 0) e->kind = LUMEN_PLUGIN_DEMUXER;
    else if (strcmp(kind_str, "decoder") == 0) e->kind = LUMEN_PLUGIN_DECODER;
    else if (strcmp(kind_str, "output-video") == 0) e->kind = LUMEN_PLUGIN_OUTPUT_VIDEO;
    else if (strcmp(kind_str, "output-audio") == 0) e->kind = LUMEN_PLUGIN_OUTPUT_AUDIO;
    else { mj_free(root); return -1; }

    const char *lib = mj_get_string(root, "library", NULL);
    if (!lib) { mj_free(root); return -1; }
    /* `lib` in the manifest is a bare base name (e.g. "demux_synthetic"),
     * NOT a filename with extension. The actual filename is platform-
     * specific (libdemux_synthetic.so on Linux/macOS,
     * demux_synthetic.dll on Windows) so a single manifest works
     * unmodified on every platform -- the registry decorates the name,
     * the manifest never hardcodes an OS-specific extension. */
    char libfile[256];
#if defined(_WIN32)
    /* MinGW keeps the Unix-style "lib" prefix on Windows DLLs (unlike
     * native MSVC builds), so the actual built artifact is
     * libdemux_synthetic.dll, not demux_synthetic.dll. Match that. */
    snprintf(libfile, sizeof(libfile), "lib%s.dll", lib);
#else
    snprintf(libfile, sizeof(libfile), "lib%s.so", lib);
#endif
    path_join(e->library_path, sizeof(e->library_path), plugin_dir, libfile);
    strncpy(e->manifest_path, manifest_path, sizeof(e->manifest_path) - 1);

    mj_value_t *claims = mj_get(root, "claims");
    if (claims) {
        ext_collect_ctx_t ctx;
        if (e->kind == LUMEN_PLUGIN_DEMUXER) {
            ctx.entry = e; ctx.count = &e->extension_count; ctx.cap = 24;
            mj_foreach_string(mj_get(claims, "extensions"), collect_ext, &ctx);
        } else if (e->kind == LUMEN_PLUGIN_DECODER) {
            ctx.entry = e; ctx.count = &e->fourcc_count; ctx.cap = 32;
            mj_foreach_string(mj_get(claims, "fourccs"), collect_fourcc, &ctx);
        }
    }

    mj_free(root);
    return 0;
}

static void registry_push(lumen_registry_t *reg, const lumen_registry_entry_t *e) {
    if (reg->count == reg->capacity) {
        reg->capacity = reg->capacity ? reg->capacity * 2 : 8;
        reg->entries = (lumen_registry_entry_t *)realloc(reg->entries, sizeof(lumen_registry_entry_t) * reg->capacity);
    }
    reg->entries[reg->count++] = *e;
}

static int has_suffix(const char *s, const char *suffix) {
    size_t ls = strlen(s), lf = strlen(suffix);
    return ls >= lf && strcmp(s + ls - lf, suffix) == 0;
}

int lumen_registry_scan(const char *plugin_dir, lumen_registry_t *reg) {
    memset(reg, 0, sizeof(*reg));

#if defined(_WIN32)
    char pattern[512];
    snprintf(pattern, sizeof(pattern), "%s\\*.manifest.json", plugin_dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        char manifest_path[512];
        path_join(manifest_path, sizeof(manifest_path), plugin_dir, fd.cFileName);
        lumen_registry_entry_t e;
        if (parse_manifest_into(manifest_path, plugin_dir, &e) == 0) registry_push(reg, &e);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(plugin_dir);
    if (!d) return -1;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (!has_suffix(ent->d_name, ".manifest.json")) continue;
        char manifest_path[512];
        path_join(manifest_path, sizeof(manifest_path), plugin_dir, ent->d_name);
        lumen_registry_entry_t e;
        if (parse_manifest_into(manifest_path, plugin_dir, &e) == 0) registry_push(reg, &e);
    }
    closedir(d);
#endif
    return reg->count;
}

static void lower_copy(char *out, const char *in, size_t n) {
    size_t i = 0;
    for (; in[i] && i < n - 1; i++) out[i] = (char)tolower((unsigned char)in[i]);
    out[i] = '\0';
}

const lumen_registry_entry_t *lumen_registry_find_demuxer_by_ext(const lumen_registry_t *reg, const char *ext) {
    char want[16]; lower_copy(want, ext, sizeof(want));
    for (int i = 0; i < reg->count; i++) {
        const lumen_registry_entry_t *e = &reg->entries[i];
        if (e->kind != LUMEN_PLUGIN_DEMUXER) continue;
        for (int j = 0; j < e->extension_count; j++) {
            char have[16]; lower_copy(have, e->extensions[j], sizeof(have));
            if (strcmp(have, want) == 0) return e;
        }
    }
    return NULL;
}

const lumen_registry_entry_t *lumen_registry_find_decoder_by_fourcc(const lumen_registry_t *reg, const char *fourcc) {
    for (int i = 0; i < reg->count; i++) {
        const lumen_registry_entry_t *e = &reg->entries[i];
        if (e->kind != LUMEN_PLUGIN_DECODER) continue;
        for (int j = 0; j < e->fourcc_count; j++) {
            if (strncmp(e->fourccs[j], fourcc, 8) == 0) return e;
        }
    }
    return NULL;
}

const lumen_registry_entry_t *lumen_registry_find_first(const lumen_registry_t *reg, lumen_plugin_kind_t kind) {
    for (int i = 0; i < reg->count; i++) {
        if (reg->entries[i].kind == kind) return &reg->entries[i];
    }
    return NULL;
}

static const char *kind_name(lumen_plugin_kind_t k) {
    switch (k) {
        case LUMEN_PLUGIN_DEMUXER:      return "demuxer";
        case LUMEN_PLUGIN_DECODER:      return "decoder";
        case LUMEN_PLUGIN_OUTPUT_VIDEO: return "output-video";
        case LUMEN_PLUGIN_OUTPUT_AUDIO: return "output-audio";
    }
    return "?";
}

void lumen_registry_print(const lumen_registry_t *reg) {
    printf("%-22s %-9s %-8s %-30s claims\n", "NAME", "KIND", "VERSION", "LIBRARY");
    for (int i = 0; i < reg->count; i++) {
        const lumen_registry_entry_t *e = &reg->entries[i];
        printf("%-22s %-9s %-8s %-30s ", e->name, kind_name(e->kind), e->version, e->library_path);
        if (e->kind == LUMEN_PLUGIN_DEMUXER) {
            for (int j = 0; j < e->extension_count; j++) printf("%s ", e->extensions[j]);
        } else if (e->kind == LUMEN_PLUGIN_DECODER) {
            for (int j = 0; j < e->fourcc_count; j++) printf("%s ", e->fourccs[j]);
        }
        printf("\n");
    }
}

void lumen_registry_free(lumen_registry_t *reg) {
    free(reg->entries);
    reg->entries = NULL;
    reg->count = reg->capacity = 0;
}

int lumen_registry_add_from_manifest(lumen_registry_t *reg,
                                      const char *manifest_path,
                                      const char *dll_path) {
    lumen_registry_entry_t e;
    /* Parse the manifest using the same function as the directory scanner.
     * We pass the directory part of manifest_path as plugin_dir -- it won't
     * be used because we'll overwrite library_path with dll_path below. */
    char dir[512];
    strncpy(dir, manifest_path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';
    char *sep = strrchr(dir, '/');
    if (!sep) sep = strrchr(dir, '\\');
    if (sep) *sep = '\0'; else strncpy(dir, ".", sizeof(dir) - 1);

    if (parse_manifest_into(manifest_path, dir, &e) != 0) return -1;

    /* Override the library path with the exact DLL path the packages scanner
     * found (dll_path already has the correct platform-specific filename). */
    strncpy(e.library_path, dll_path, sizeof(e.library_path) - 1);
    e.library_path[sizeof(e.library_path) - 1] = '\0';

    registry_push(reg, &e);
    return 0;
}
