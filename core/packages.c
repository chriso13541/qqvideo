/*
 * packages.c -- qqvideo package filesystem scanner
 *
 * Layout:  packages/demuxers/<id>/   and   packages/codecs/{video,audio}/<id>/
 */
#include "packages.h"
#include "minijson.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
  #include <windows.h>
  #define PATH_SEP '\\'
#else
  #include <sys/types.h>
  #include <dirent.h>
  #include <sys/stat.h>
  #define PATH_SEP '/'
#endif

static int file_exists(const char *p) {
#if defined(_WIN32)
    return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES;
#else
    struct stat s; return stat(p, &s) == 0;
#endif
}
static int is_dir(const char *p) {
#if defined(_WIN32)
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat s; return stat(p, &s) == 0 && S_ISDIR(s.st_mode);
#endif
}
static void pj(char *o, size_t n, const char *a, const char *b) {
    snprintf(o, n, "%s%c%s", a, PATH_SEP, b);
}
static void rjf(const char *path, const char *key, char *out, size_t sz) {
    out[0] = '\0';
    FILE *f = fopen(path, "r"); if (!f) return;
    fseek(f, 0, SEEK_END); long s = ftell(f); rewind(f);
    if (s <= 0 || s > 65536) { fclose(f); return; }
    char *buf = (char*)malloc(s+1); if (!buf) { fclose(f); return; }
    fread(buf, 1, s, f); buf[s] = '\0'; fclose(f);
    mj_value_t *root = mj_parse(buf);
    if (root) {
        const char *v = mj_get_string(root, key, NULL);
        if (v) { strncpy(out, v, sz-1); out[sz-1] = '\0'; }
        mj_free(root);
    }
    free(buf);
}
static void find_dll(const char *dir, const char *plugin,
                      char *dll, size_t dsz, char *man, size_t msz) {
    dll[0] = man[0] = '\0';
    char p[LUMEN_PKG_PATH_MAX];
#if defined(_WIN32)
    snprintf(p, sizeof(p), "%s%clib%s.dll", dir, PATH_SEP, plugin);
#else
    snprintf(p, sizeof(p), "%s%clib%s.so",  dir, PATH_SEP, plugin);
#endif
    if (file_exists(p)) {
        strncpy(dll, p, dsz-1);
        snprintf(man, msz, "%s%c%s.manifest.json", dir, PATH_SEP, plugin);
    }
}

/* Scan one demuxer subdirectory */
static int scan_one_demuxer(const char *dir, lumen_pkg_demuxer_t *out) {
    memset(out, 0, sizeof(*out));
    char json[LUMEN_PKG_PATH_MAX]; pj(json, sizeof(json), dir, "package.json");
    if (!file_exists(json)) return 0;
    rjf(json, "id",           out->id,           sizeof(out->id));
    rjf(json, "display_name", out->display_name, sizeof(out->display_name));
    rjf(json, "extensions",   out->extensions,   sizeof(out->extensions));
    rjf(json, "plugin",       out->plugin,        sizeof(out->plugin));
    rjf(json, "version",      out->version,       sizeof(out->version));
    if (!out->id[0]) return 0;
    if (out->plugin[0])
        find_dll(dir, out->plugin,
                 out->dll_path, sizeof(out->dll_path),
                 out->manifest_path, sizeof(out->manifest_path));
    out->installed = (out->dll_path[0] != '\0');
    strncpy(out->dir_path, dir, sizeof(out->dir_path) - 1);
    return 1;
}

/* Scan one codec subdirectory */
static int scan_one_codec(const char *dir, lumen_pkg_codec_t *out) {
    memset(out, 0, sizeof(*out));
    char json[LUMEN_PKG_PATH_MAX]; pj(json, sizeof(json), dir, "package.json");
    if (!file_exists(json)) return 0;
    rjf(json, "id",           out->id,           sizeof(out->id));
    rjf(json, "display_name", out->display_name, sizeof(out->display_name));
    rjf(json, "fourcc",       out->fourcc,        sizeof(out->fourcc));
    rjf(json, "codec_type",   out->codec_type,    sizeof(out->codec_type));
    rjf(json, "plugin",       out->plugin,        sizeof(out->plugin));
    rjf(json, "version",      out->version,       sizeof(out->version));
    if (!out->id[0]) return 0;
    if (out->plugin[0])
        find_dll(dir, out->plugin,
                 out->dll_path, sizeof(out->dll_path),
                 out->manifest_path, sizeof(out->manifest_path));
    out->installed = (out->dll_path[0] != '\0');
    strncpy(out->dir_path, dir, sizeof(out->dir_path) - 1);
    return 1;
}

/* Scan all immediate subdirs of parent_dir calling scan_one for each */
static int scan_demuxer_dir(const char *parent, lumen_pkg_demuxer_t *arr, int max) {
    int n = 0;
    if (!is_dir(parent)) return 0;
#if defined(_WIN32)
    char search[LUMEN_PKG_PATH_MAX];
    snprintf(search, sizeof(search), "%s\\*", parent);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(search, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!strcmp(fd.cFileName,".") || !strcmp(fd.cFileName,"..")) continue;
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (n >= max) break;
        char sub[LUMEN_PKG_PATH_MAX]; pj(sub, sizeof(sub), parent, fd.cFileName);
        if (scan_one_demuxer(sub, &arr[n])) n++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(parent); if (!d) return 0;
    struct dirent *de;
    while ((de = readdir(d)) && n < max) {
        if (de->d_name[0] == '.') continue;
        char sub[LUMEN_PKG_PATH_MAX]; pj(sub, sizeof(sub), parent, de->d_name);
        if (!is_dir(sub)) continue;
        if (scan_one_demuxer(sub, &arr[n])) n++;
    }
    closedir(d);
#endif
    return n;
}

static int scan_codec_dir(const char *parent, lumen_pkg_codec_t *arr, int max) {
    int n = 0;
    if (!is_dir(parent)) return 0;
#if defined(_WIN32)
    char search[LUMEN_PKG_PATH_MAX];
    snprintf(search, sizeof(search), "%s\\*", parent);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(search, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!strcmp(fd.cFileName,".") || !strcmp(fd.cFileName,"..")) continue;
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (n >= max) break;
        char sub[LUMEN_PKG_PATH_MAX]; pj(sub, sizeof(sub), parent, fd.cFileName);
        if (scan_one_codec(sub, &arr[n])) n++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(parent); if (!d) return 0;
    struct dirent *de;
    while ((de = readdir(d)) && n < max) {
        if (de->d_name[0] == '.') continue;
        char sub[LUMEN_PKG_PATH_MAX]; pj(sub, sizeof(sub), parent, de->d_name);
        if (!is_dir(sub)) continue;
        if (scan_one_codec(sub, &arr[n])) n++;
    }
    closedir(d);
#endif
    return n;
}

/* ---- Public API ------------------------------------------------------- */

int lumen_packages_scan(const char *packages_dir, lumen_packages_t *out) {
    memset(out, 0, sizeof(*out));
    strncpy(out->packages_dir, packages_dir, sizeof(out->packages_dir)-1);
    if (!is_dir(packages_dir)) {
        fprintf(stderr, "lumen/pkg: packages dir not found: %s\n", packages_dir);
        return -1;
    }
    char buf[LUMEN_PKG_PATH_MAX];

    pj(buf, sizeof(buf), packages_dir, "demuxers");
    out->demuxer_count = scan_demuxer_dir(buf, out->demuxers, LUMEN_PKG_MAX_DEMUXERS);

    char codecs[LUMEN_PKG_PATH_MAX];
    pj(codecs, sizeof(codecs), packages_dir, "codecs");
    pj(buf, sizeof(buf), codecs, "video");
    out->video_codec_count = scan_codec_dir(buf, out->video_codecs, LUMEN_PKG_MAX_CODECS);
    pj(buf, sizeof(buf), codecs, "audio");
    out->audio_codec_count = scan_codec_dir(buf, out->audio_codecs, LUMEN_PKG_MAX_CODECS);

    printf("lumen/pkg: %d demuxer(s), %d video codec(s), %d audio codec(s) from '%s'\n",
           out->demuxer_count, out->video_codec_count, out->audio_codec_count, packages_dir);
    return 0;
}

int lumen_packages_rescan(lumen_packages_t *pkgs) {
    char dir[LUMEN_PKG_PATH_MAX];
    strncpy(dir, pkgs->packages_dir, sizeof(dir)-1);
    return lumen_packages_scan(dir, pkgs);
}

void lumen_packages_print(const lumen_packages_t *pkgs) {
    printf("Demuxers (%d):\n", pkgs->demuxer_count);
    for (int i = 0; i < pkgs->demuxer_count; i++) {
        const lumen_pkg_demuxer_t *d = &pkgs->demuxers[i];
        printf("  [%s] %-20s  %s\n", d->installed?"INST":"----",
               d->display_name, d->extensions);
    }
    printf("Video codecs (%d):\n", pkgs->video_codec_count);
    for (int i = 0; i < pkgs->video_codec_count; i++) {
        const lumen_pkg_codec_t *c = &pkgs->video_codecs[i];
        printf("  [%s] %-20s  %s\n", c->installed?"INST":"----",
               c->display_name, c->fourcc);
    }
    printf("Audio codecs (%d):\n", pkgs->audio_codec_count);
    for (int i = 0; i < pkgs->audio_codec_count; i++) {
        const lumen_pkg_codec_t *c = &pkgs->audio_codecs[i];
        printf("  [%s] %-20s  %s\n", c->installed?"INST":"----",
               c->display_name, c->fourcc);
    }
}
