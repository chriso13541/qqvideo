/*
 * demux_libav.c -- libavformat demuxer for Linux system-FFmpeg builds
 *
 * demux_mp4.c generalized: libavformat's API is container-agnostic, so
 * the same wrapper handles MP4/MOV, Matroska/WebM, AVI, MPEG-TS, Ogg,
 * FLAC and MP3 -- the manifest's extension list decides what it claims.
 *
 * The security-relevant part is open_ex(). With a distro libavcodec,
 * every decoder the distro enabled is mapped into the process the moment
 * any libav plugin loads, so "not installed = not present" no longer
 * holds. What still holds is "not allowed = not reachable", and two
 * libavformat options enforce it:
 *
 *   codec_whitelist  -- avformat_find_stream_info() opens decoders to
 *                       probe streams; it forwards this list to every
 *                       one it opens, and avcodec_open2 refuses anything
 *                       not on it. Built from the fourccs the core says
 *                       are allowed for THIS file (installed decoders
 *                       minus Playback Rules), mapped to every decoder
 *                       name the system libavcodec has for each (e.g.
 *                       AV1 -> "libdav1d,libaom-av1,av1").
 *   format_whitelist -- only the container formats this plugin claims.
 */
#include "../../include/lumen_plugin.h"
#include "../common/libav_common.h"
#include <libavutil/intreadwrite.h>
#include <stdlib.h>
#include <string.h>

#define PLUGIN_VERSION "0.3.0"

/* libavformat demuxer names matching the manifest's extensions.
 * ("mov,mp4,m4a,3gp,3g2,mj2" is one demuxer whose name is that whole
 * list; "matroska,webm" likewise.) */
static const char *FORMAT_WHITELIST =
    "mov,mp4,m4a,3gp,3g2,mj2,matroska,webm,avi,mpegts,ogg,flac,mp3,aac,"
    "srt,ass,webvtt";   /* standalone subtitle files ("Add Subtitle Track...") */

struct lumen_demuxer_ctx {
    AVFormatContext *fmt_ctx;
    AVPacket        *avpkt;
    /* Timeline origin for pts_ms/duration_ms, in AV_TIME_BASE units:
     * the video stream's start time, because decoder_libav numbers video
     * frames from 0 at the FIRST video frame. Subtracting it puts subtitle
     * cues on the same clock as position_ms (matters for MPEG-TS, whose
     * timestamps start at ~1.4s, and MP4s with edit lists). */
    int64_t          origin_us;
};

static int is_wanted(const AVStream *st) {
    enum AVMediaType t = st->codecpar->codec_type;
    if (st->disposition & AV_DISPOSITION_ATTACHED_PIC) return 0; /* cover art */
    return t == AVMEDIA_TYPE_VIDEO || t == AVMEDIA_TYPE_AUDIO || t == AVMEDIA_TYPE_SUBTITLE;
}

static int libav_probe(const uint8_t *header_bytes, size_t header_len, const char *file_ext) {
    (void)header_bytes; (void)header_len; (void)file_ext;
    return 1; /* selection is by manifest extension claims; libavformat probes content itself */
}

/* Appends every decoder name the system libavcodec has for `id` to buf. */
static void append_decoder_names(char *buf, size_t sz, enum AVCodecID id) {
    void *it = NULL;
    const AVCodec *c;
    while ((c = av_codec_iterate(&it))) {
        if (c->id != id || !av_codec_is_decoder(c)) continue;
        if (buf[0]) strncat(buf, ",", sz - strlen(buf) - 1);
        strncat(buf, c->name, sz - strlen(buf) - 1);
    }
}

static int fill_stream_table(AVFormatContext *fmt_ctx, lumen_stream_table_t *out_table) {
    memset(out_table, 0, sizeof(*out_table));
    int n = 0;
    for (unsigned i = 0; i < fmt_ctx->nb_streams && i < LUMEN_MAX_STREAMS; i++) {
        AVStream *st = fmt_ctx->streams[i];
        AVCodecParameters *cp = st->codecpar;
        if (!is_wanted(st)) continue;

        lumen_stream_desc_t *sd = &out_table->streams[n++];
        sd->stream_index = (int)i;
        sd->type = cp->codec_type == AVMEDIA_TYPE_VIDEO ? LUMEN_STREAM_VIDEO
                 : cp->codec_type == AVMEDIA_TYPE_AUDIO ? LUMEN_STREAM_AUDIO
                 : LUMEN_STREAM_SUBTITLE;

        const lumen_libav_codec_t *c = lumen_libav_by_id(cp->codec_id);
        strncpy(sd->codec_fourcc, c ? c->fourcc : "UNKN", sizeof(sd->codec_fourcc) - 1);

        if (sd->type == LUMEN_STREAM_VIDEO) {
            sd->width = cp->width;
            sd->height = cp->height;
            double fps = av_q2d(st->avg_frame_rate);
            if (fps <= 0.0 || fps > 1000.0) fps = av_q2d(st->r_frame_rate);
            sd->frame_rate = (fps > 0.0 && fps <= 1000.0) ? fps : 0.0;
        } else if (sd->type == LUMEN_STREAM_AUDIO) {
            sd->sample_rate = cp->sample_rate;
            sd->channels = cp->ch_layout.nb_channels;
        }
        sd->extradata = cp->extradata;
        sd->extradata_size = cp->extradata_size;

        const AVDictionaryEntry *lang  = av_dict_get(st->metadata, "language", NULL, 0);
        const AVDictionaryEntry *title = av_dict_get(st->metadata, "title", NULL, 0);
        if (lang && strcmp(lang->value, "und") != 0)
            strncpy(sd->language, lang->value, sizeof(sd->language) - 1);
        if (title) strncpy(sd->title, title->value, sizeof(sd->title) - 1);
        sd->is_default = (st->disposition & AV_DISPOSITION_DEFAULT) ? 1 : 0;
        sd->is_forced  = (st->disposition & AV_DISPOSITION_FORCED) ? 1 : 0;
    }
    out_table->stream_count = n;
    out_table->duration_sec = (fmt_ctx->duration > 0) ? (double)fmt_ctx->duration / AV_TIME_BASE : 0.0;
    return n;
}

static int64_t compute_origin_us(AVFormatContext *fmt_ctx) {
    int vi = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (vi >= 0) {
        AVStream *vs = fmt_ctx->streams[vi];
        if (vs->start_time != AV_NOPTS_VALUE)
            return av_rescale_q(vs->start_time, vs->time_base, AV_TIME_BASE_Q);
    }
    return fmt_ctx->start_time != AV_NOPTS_VALUE ? fmt_ctx->start_time : 0;
}

static int libav_open_ex(lumen_demuxer_ctx_t **out_ctx, const char *path,
                         lumen_stream_table_t *out_table,
                         const char *const *allowed, int allowed_count) {
    char codec_wl[1024] = "";
    for (int i = 0; i < allowed_count; i++) {
        const lumen_libav_codec_t *c = lumen_libav_by_fourcc(allowed[i]);
        if (c) append_decoder_names(codec_wl, sizeof(codec_wl), c->id);
    }
    /* An EMPTY whitelist would mean "no restriction" to libavformat --
     * the opposite of what an empty allow-list means. Use a name no
     * decoder has so nothing can be opened during probing. */
    if (!codec_wl[0]) strcpy(codec_wl, "none");

    AVDictionary *opts = NULL;
    av_dict_set(&opts, "codec_whitelist", codec_wl, 0);
    av_dict_set(&opts, "format_whitelist", FORMAT_WHITELIST, 0);

    AVFormatContext *fmt_ctx = NULL;
    int rc = avformat_open_input(&fmt_ctx, path, NULL, &opts);
    av_dict_free(&opts);
    if (rc < 0) {
        char err[128];
        av_strerror(rc, err, sizeof(err));
        fprintf(stderr, "demux_libav: cannot open '%s': %s\n", path, err);
        return -1;
    }
    if (avformat_find_stream_info(fmt_ctx, NULL) < 0) {
        avformat_close_input(&fmt_ctx);
        return -1;
    }

    lumen_demuxer_ctx_t *ctx = (lumen_demuxer_ctx_t *)calloc(1, sizeof(*ctx));
    ctx->fmt_ctx = fmt_ctx;
    ctx->avpkt = av_packet_alloc();
    ctx->origin_us = compute_origin_us(fmt_ctx);
    *out_ctx = ctx;
    fill_stream_table(fmt_ctx, out_table);
    return 0;
}

/* Legacy entry point (a core that doesn't know open_ex): nothing is
 * allowed rather than everything -- fail closed. */
static int libav_open(lumen_demuxer_ctx_t **out_ctx, const char *path, lumen_stream_table_t *out_table) {
    return libav_open_ex(out_ctx, path, out_table, NULL, 0);
}

static int libav_read_packet(lumen_demuxer_ctx_t *ctx, lumen_packet_t *out_pkt) {
    for (;;) {
        if (av_read_frame(ctx->fmt_ctx, ctx->avpkt) < 0) {
            av_packet_unref(ctx->avpkt);
            return 1;
        }
        int si = ctx->avpkt->stream_index;
        AVStream *st = ctx->fmt_ctx->streams[si];
        enum AVMediaType t = st->codecpar->codec_type;
        if (si >= LUMEN_MAX_STREAMS || !is_wanted(st)) {
            av_packet_unref(ctx->avpkt);
            continue;
        }

        /* libavcodec requires AV_INPUT_BUFFER_PADDING_SIZE zeroed bytes
         * after every packet -- decoders read past the end for speed, and
         * text subtitle decoders rely on hitting a 0 to find the end of the
         * string. Without it, the SRT decoder ran on into stale heap bytes
         * and cues came out as "arrest him.>!" / "arrest him.not...". */
        out_pkt->data = (uint8_t *)malloc((size_t)ctx->avpkt->size + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!out_pkt->data) { av_packet_unref(ctx->avpkt); return -1; }
        memcpy(out_pkt->data, ctx->avpkt->data, ctx->avpkt->size);
        memset(out_pkt->data + ctx->avpkt->size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
        out_pkt->size = ctx->avpkt->size;
        out_pkt->pts = ctx->avpkt->pts;
        out_pkt->stream_index = si;
        out_pkt->keyframe = (ctx->avpkt->flags & AV_PKT_FLAG_KEY) ? 1 : 0;

        /* Millisecond timing on the video frame clock (see origin_us). */
        int64_t ts = ctx->avpkt->pts != AV_NOPTS_VALUE ? ctx->avpkt->pts : ctx->avpkt->dts;
        out_pkt->pts_ms = -1;
        out_pkt->duration_ms = -1;
        if (ts != AV_NOPTS_VALUE) {
            int64_t us = av_rescale_q(ts, st->time_base, AV_TIME_BASE_Q) - ctx->origin_us;
            out_pkt->pts_ms = us / 1000;
        }
        if (ctx->avpkt->duration > 0)
            out_pkt->duration_ms = av_rescale_q(ctx->avpkt->duration, st->time_base, (AVRational){1, 1000});

        if (t == AVMEDIA_TYPE_AUDIO) {
            /* Priming samples (MP4 AAC encoder delay) and trailing padding
             * (Matroska/Ogg DiscardPadding) arrive as side data; the
             * decoder trims them. Layout per avpacket.h: u32le samples to
             * skip from the start, then u32le samples to discard at the end. */
            size_t side_size = 0;
            const uint8_t *side = av_packet_get_side_data(ctx->avpkt, AV_PKT_DATA_SKIP_SAMPLES, &side_size);
            if (side && side_size >= 8) {
                out_pkt->skip_samples_start = (int)AV_RL32(side);
                out_pkt->skip_samples_end   = (int)AV_RL32(side + 4);
            } else if (side && side_size >= 4) {
                out_pkt->skip_samples_start = (int)AV_RL32(side);
            }
        }
        av_packet_unref(ctx->avpkt);
        return 0;
    }
}

static void libav_packet_free(lumen_packet_t *pkt) {
    free(pkt->data);
    pkt->data = NULL;
}

static int libav_seek(lumen_demuxer_ctx_t *ctx, int64_t target_ms) {
    int idx = av_find_default_stream_index(ctx->fmt_ctx);
    if (idx < 0) return -1;
    AVStream *st = ctx->fmt_ctx->streams[idx];
    int64_t ts = av_rescale_q(target_ms, (AVRational){1, 1000}, st->time_base);
    if (st->start_time != AV_NOPTS_VALUE) ts += st->start_time; /* MPEG-TS etc. don't start at 0 */
    if (av_seek_frame(ctx->fmt_ctx, idx, ts, AVSEEK_FLAG_BACKWARD) < 0) return -1;
    av_packet_unref(ctx->avpkt);
    return 0;
}

static void libav_close(lumen_demuxer_ctx_t *ctx) {
    if (!ctx) return;
    av_packet_free(&ctx->avpkt);
    avformat_close_input(&ctx->fmt_ctx);
    free(ctx);
}

static const lumen_demuxer_vtable_t VTABLE = {
    .probe = libav_probe,
    .open = libav_open,
    .read_packet = libav_read_packet,
    .packet_free = libav_packet_free,
    .seek = libav_seek,
    .close = libav_close,
    .open_ex = libav_open_ex,
};

static lumen_plugin_descriptor_t DESCRIPTOR = {
    .abi_version = LUMEN_ABI_VERSION,
    .kind = LUMEN_PLUGIN_DEMUXER,
    .name = "libav-demuxer",
    .version = PLUGIN_VERSION,
    .vtable = { .demuxer = &VTABLE },
};

LUMEN_EXPORT const lumen_plugin_descriptor_t *lumen_get_plugin(void) {
    DESCRIPTOR.version = lumen_libav_version_string(PLUGIN_VERSION);
    return &DESCRIPTOR;
}
