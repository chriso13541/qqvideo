/*
 * libav_common.h -- shared by decoder_libav and demux_libav
 *
 * Header-only on purpose: each plugin is its own .so with hidden
 * visibility, so these are `static` copies per plugin rather than a
 * third shared library to keep in sync.
 *
 * THE TABLE BELOW IS THE ALLOWLIST. A codec that isn't listed here can't
 * be claimed by decoder_libav, can't be reported with a real fourcc by
 * demux_libav, and never makes it into libavformat's codec_whitelist --
 * so even though the system libavcodec.so contains every decoder the
 * distro enabled (MagicYUV included), only these are reachable.
 * Adding a codec = adding a row here + its fourcc to
 * decoder_libav's manifest.
 */
#ifndef LUMEN_LIBAV_COMMON_H
#define LUMEN_LIBAV_COMMON_H

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <stdio.h>
#include <string.h>

/* Minimum supported: FFmpeg 5.1 (libavcodec 59.37). That's where the
 * AVChannelLayout API (ch_layout, swr_alloc_set_opts2) arrived; the old
 * channels/channel_layout fields were removed in 7.0, so supporting only
 * the new API keeps one code path for Debian 12 (5.1) through current. */
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(59, 37, 100)
#error "qqvideo needs FFmpeg >= 5.1 (libavcodec >= 59.37.100)"
#endif

typedef struct {
    const char     *fourcc;   /* what demuxers report / manifests claim */
    enum AVCodecID  id;
    int             kind;     /* LUMEN_PROBE_VIDEO / LUMEN_PROBE_AUDIO */
} lumen_libav_codec_t;

static const lumen_libav_codec_t LUMEN_LIBAV_CODECS[] = {
    /* video */
    { "H264", AV_CODEC_ID_H264,       1 },
    { "HEVC", AV_CODEC_ID_HEVC,       1 },
    { "AV01", AV_CODEC_ID_AV1,        1 },
    { "VP09", AV_CODEC_ID_VP9,        1 },
    { "VP08", AV_CODEC_ID_VP8,        1 },
    { "MPG2", AV_CODEC_ID_MPEG2VIDEO, 1 },
    { "MP4V", AV_CODEC_ID_MPEG4,      1 },
    /* audio */
    { "AAC",  AV_CODEC_ID_AAC,        2 },
    { "MP3",  AV_CODEC_ID_MP3,        2 },
    { "OPUS", AV_CODEC_ID_OPUS,       2 },
    { "VORB", AV_CODEC_ID_VORBIS,     2 },
    { "FLAC", AV_CODEC_ID_FLAC,       2 },
    { "AC3",  AV_CODEC_ID_AC3,        2 },
    { "EAC3", AV_CODEC_ID_EAC3,       2 },
};
#define LUMEN_LIBAV_CODEC_COUNT (int)(sizeof(LUMEN_LIBAV_CODECS) / sizeof(LUMEN_LIBAV_CODECS[0]))

static inline const lumen_libav_codec_t *lumen_libav_by_fourcc(const char *fourcc) {
    /* Aliases used by older demuxers in this tree (demux_rawh264 etc.) */
    if (strcmp(fourcc, "AVC1") == 0) fourcc = "H264";
    if (strcmp(fourcc, "MP4A") == 0) fourcc = "AAC";
    for (int i = 0; i < LUMEN_LIBAV_CODEC_COUNT; i++)
        if (strcmp(LUMEN_LIBAV_CODECS[i].fourcc, fourcc) == 0) return &LUMEN_LIBAV_CODECS[i];
    return NULL;
}

static inline const lumen_libav_codec_t *lumen_libav_by_id(enum AVCodecID id) {
    for (int i = 0; i < LUMEN_LIBAV_CODEC_COUNT; i++)
        if (LUMEN_LIBAV_CODECS[i].id == id) return &LUMEN_LIBAV_CODECS[i];
    return NULL;
}

/* Builds a plugin version string that says which FFmpeg is ACTUALLY
 * loaded at runtime, e.g. "0.3.0 (FFmpeg 7.1, libavcodec 61.19.100)".
 * player_core.c already prints every plugin's version when it loads it,
 * so with apt's ffmpeg and a /usr/local source build both installed,
 * the log answers "which one am I running?" with no extra plumbing.
 *
 * Also warns if the runtime major differs from the headers this plugin
 * was compiled against. The soname (libavcodec.so.60 vs .61) normally
 * makes that impossible, so if it ever fires, something is being
 * force-loaded (LD_LIBRARY_PATH, LD_PRELOAD) and a rebuild is due. */
static inline const char *lumen_libav_version_string(const char *plugin_version) {
    static char buf[160];
    if (buf[0]) return buf;
    unsigned v = avcodec_version();
    snprintf(buf, sizeof(buf), "%s (FFmpeg %s, libavcodec %u.%u.%u)",
             plugin_version, av_version_info(), v >> 16, (v >> 8) & 0xff, v & 0xff);
    if ((int)(v >> 16) != LIBAVCODEC_VERSION_MAJOR) {
        fprintf(stderr,
                "qqvideo: WARNING libavcodec major mismatch: built against %d, running %u "
                "-- rebuild qqvideo against the FFmpeg you have installed\n",
                LIBAVCODEC_VERSION_MAJOR, v >> 16);
    }
    return buf;
}

#endif
