/*
 * packages.h -- qqvideo package filesystem scanner
 *
 * Scans the packages/ directory tree. The layout has two top-level
 * categories, each with named subdirectories:
 *
 *   packages/demuxers/<id>/       one per container format
 *       package.json              container metadata
 *       demux_<id>.dll            present = installed
 *       demux_<id>.manifest.json
 *
 *   packages/codecs/video/<id>/   one per video codec
 *       package.json
 *       decoder_<id>.dll          present = installed
 *       decoder_<id>.manifest.json
 *
 *   packages/codecs/audio/<id>/   one per audio codec
 *       package.json
 *       decoder_<id>.dll
 *       decoder_<id>.manifest.json
 *
 * Codecs are SHARED across all containers. "Installed" means the DLL
 * exists in the codec folder. "Enabled for a specific container" is
 * tracked separately in disabled_pairs (see lumen_plugin.h), not by
 * file presence. This avoids duplicating large codec DLLs per container.
 */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LUMEN_PKG_MAX_DEMUXERS  8
#define LUMEN_PKG_MAX_CODECS   16
#define LUMEN_PKG_PATH_MAX    512

typedef struct {
    char id[32];
    char display_name[64];
    char extensions[128];   /* e.g. ".mp4, .m4v, .mov" */
    char plugin[64];         /* e.g. "demux_mp4"        */
    char version[32];
    char dll_path[LUMEN_PKG_PATH_MAX];
    char manifest_path[LUMEN_PKG_PATH_MAX];
    int  installed;          /* 1 iff dll_path points to an existing file */
    char dir_path[LUMEN_PKG_PATH_MAX];  /* directory containing package.json */
} lumen_pkg_demuxer_t;

typedef struct {
    char id[32];
    char display_name[64];
    char fourcc[8];
    char codec_type[8];      /* "video" or "audio"    */
    char plugin[64];
    char version[32];
    char dll_path[LUMEN_PKG_PATH_MAX];
    char manifest_path[LUMEN_PKG_PATH_MAX];
    int  installed;
    char dir_path[LUMEN_PKG_PATH_MAX];  /* directory containing package.json */
} lumen_pkg_codec_t;

typedef struct {
    lumen_pkg_demuxer_t demuxers[LUMEN_PKG_MAX_DEMUXERS];
    int                 demuxer_count;
    lumen_pkg_codec_t   video_codecs[LUMEN_PKG_MAX_CODECS];
    int                 video_codec_count;
    lumen_pkg_codec_t   audio_codecs[LUMEN_PKG_MAX_CODECS];
    int                 audio_codec_count;
    char                packages_dir[LUMEN_PKG_PATH_MAX];
} lumen_packages_t;

/* Scan packages_dir and populate *out. Returns 0 on success, -1 if
 * packages_dir cannot be opened. Partial results are still returned. */
int  lumen_packages_scan(const char *packages_dir, lumen_packages_t *out);
int  lumen_packages_rescan(lumen_packages_t *pkgs);
void lumen_packages_print(const lumen_packages_t *pkgs);

#ifdef __cplusplus
} /* extern "C" */
#endif
