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
 *   protocol_whitelist -- "file" only: nothing is fetched from the network
 *                       or read through pipe:/concat:/subfile: tricks.
 */
#include "../../include/lumen_plugin.h"
#include "../common/libav_common.h"
#include <libavutil/intreadwrite.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#if defined(_WIN32)
#include <windows.h>      /* MultiByteToWideChar: removing a failed clip */
#include <wchar.h>
#endif

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
    /* Local files only. Without this, libavformat treats a "path" that
     * looks like a URL as one -- "http://", "tcp:", "pipe:", "concat:",
     * "subfile," -- and the formats it reads can name further inputs,
     * so a file could make the player open network connections or read
     * other local files. Nothing qqvideo opens is meant to be a URL. */
    av_dict_set(&opts, "protocol_whitelist", "file", 0);

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

static int libav_seek_ex(lumen_demuxer_ctx_t *ctx, int64_t target_ms, int flags) {
    int idx = av_find_default_stream_index(ctx->fmt_ctx);
    if (idx < 0) return -1;
    AVStream *st = ctx->fmt_ctx->streams[idx];
    int64_t ts = av_rescale_q(target_ms, (AVRational){1, 1000}, st->time_base);
    if (st->start_time != AV_NOPTS_VALUE) ts += st->start_time; /* MPEG-TS etc. don't start at 0 */
    /* avformat_seek_file() takes an allowed window around the target:
     * forward = keyframe in [ts, +inf), backward = keyframe in (-inf, ts]. */
    int rc = (flags & LUMEN_SEEK_FORWARD)
        ? avformat_seek_file(ctx->fmt_ctx, idx, ts, ts, INT64_MAX, 0)
        : avformat_seek_file(ctx->fmt_ctx, idx, INT64_MIN, ts, ts, 0);
    if (rc < 0) return -1;
    av_packet_unref(ctx->avpkt);
    return 0;
}

static int libav_seek(lumen_demuxer_ctx_t *ctx, int64_t target_ms) {
    return libav_seek_ex(ctx, target_ms, LUMEN_SEEK_BACKWARD);
}

static void libav_close(lumen_demuxer_ctx_t *ctx) {
    if (!ctx) return;
    av_packet_free(&ctx->avpkt);
    avformat_close_input(&ctx->fmt_ctx);
    free(ctx);
}

/* ---- Clips (Video > Clip Back / Clip Forward) ---------------------------
 * Copies a stretch of the file into a new one without re-encoding: fast,
 * lossless, and no encoders needed. The catch with copying is that a clip
 * must start on a keyframe, so packets are copied from the last keyframe
 * at or before start_ms, with timestamps shifted so start_ms becomes 0:
 *   MP4 (from MP4/MOV sources): the frames before start_ms get negative
 *        timestamps and the muxer writes an edit list, so players start
 *        exactly at start_ms (what `ffmpeg -ss T -i in -c copy` does).
 *   Matroska (everything else, any codec fits): no edit lists, so the
 *        clip starts at that keyframe, up to a GOP early.
 * Nothing is decoded: the input is opened with codec_whitelist "none". */

static int has_ext(const char *path, const char *ext) {
    size_t n = strlen(path), e = strlen(ext);
    if (n < e) return 0;
    for (size_t i = 0; i < e; i++) {
        char c = path[n - e + i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != ext[i]) return 0;
    }
    return 1;
}

static int libav_export_clip(const char *path, int64_t start_ms, int64_t end_ms,
                             const char *out_dir, const char *stem,
                             char *out_path, size_t out_size, char *err, size_t err_size) {
    AVFormatContext *in = NULL, *out = NULL;
    AVPacket *pkt = NULL;
    AVDictionary *opts = NULL;
    int *map = NULL, *done = NULL, ret = -1, header_written = 0, copied = 0, created = 0;
    char fail[160] = "";
    out_path[0] = '\0';
    if (end_ms <= start_ms) { snprintf(err, err_size, "the clip is empty"); return -1; }

    av_dict_set(&opts, "codec_whitelist", "none", 0);            /* copy only: never decode */
    av_dict_set(&opts, "format_whitelist", FORMAT_WHITELIST, 0);
    av_dict_set(&opts, "protocol_whitelist", "file", 0);
    int rc = avformat_open_input(&in, path, NULL, &opts);
    av_dict_free(&opts);
    if (rc < 0) { av_strerror(rc, fail, sizeof(fail)); snprintf(err, err_size, "couldn't open the video (%s)", fail); return -1; }
    avformat_find_stream_info(in, NULL);      /* best effort: container headers usually suffice */

    /* Output container: MP4 for MP4/MOV sources (exact start via edit
     * list, plays everywhere), WebM for WebM, Matroska for the rest. */
    const int from_mov = strstr(in->iformat->name, "mp4") != NULL;
    int has_video = 0;
    for (unsigned i = 0; i < in->nb_streams; i++)
        if (in->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
            !(in->streams[i]->disposition & AV_DISPOSITION_ATTACHED_PIC)) has_video = 1;
    const char *fmt = from_mov ? "mp4" : has_ext(path, ".webm") ? "webm" : "matroska";
    const char *ext = from_mov ? (has_video ? ".mp4" : ".m4a") : has_ext(path, ".webm") ? ".webm" : (has_video ? ".mkv" : ".mka");

    /* A name that isn't taken yet: stem.ext, stem-2.ext, ... */
    for (int n = 1; n < 100; n++) {
        if (n == 1) snprintf(out_path, out_size, "%s%s%s%s", out_dir,
#if defined(_WIN32)
                             "\\",
#else
                             "/",
#endif
                             stem, ext);
        else snprintf(out_path, out_size, "%s%s%s-%d%s", out_dir,
#if defined(_WIN32)
                      "\\",
#else
                      "/",
#endif
                      stem, n, ext);
        if (avio_check(out_path, 0) < 0) break;               /* doesn't exist */
        if (n == 99) { snprintf(err, err_size, "too many clips with this name"); goto end; }
    }

    if (avformat_alloc_output_context2(&out, NULL, fmt, out_path) < 0 || !out) {
        snprintf(err, err_size, "this FFmpeg can't write %s files", fmt);
        goto end;
    }
    map = (int *)calloc(in->nb_streams, sizeof(int));
    done = (int *)calloc(in->nb_streams, sizeof(int));
    if (!map || !done) { snprintf(err, err_size, "out of memory"); goto end; }

    /* Streams: video, audio and subtitles the output container can hold. */
    int vin = -1;
    for (unsigned i = 0; i < in->nb_streams; i++) {
        AVStream *ist = in->streams[i];
        map[i] = -1;
        if (!is_wanted(ist)) continue;
        if (avformat_query_codec(out->oformat, ist->codecpar->codec_id, FF_COMPLIANCE_NORMAL) != 1) continue;
        AVStream *ost = avformat_new_stream(out, NULL);
        if (!ost || avcodec_parameters_copy(ost->codecpar, ist->codecpar) < 0) { snprintf(err, err_size, "out of memory"); goto end; }
        /* Same container family: keep the codec tag (e.g. HEVC's hvc1,
         * which Apple players need); otherwise let the muxer choose. */
        if (!from_mov) ost->codecpar->codec_tag = 0;
        ost->time_base = ist->time_base;
        ost->disposition = ist->disposition;
        av_dict_copy(&ost->metadata, ist->metadata, 0);        /* language, title */
        map[i] = ost->index;
        if (vin < 0 && ist->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) vin = (int)i;
    }
    if (out->nb_streams == 0) { snprintf(err, err_size, "nothing in this file can be copied into a clip"); goto end; }

    /* Start: the keyframe at or before start_ms (on the video stream). */
    const int64_t origin_us = compute_origin_us(in);
    const int64_t start_us = start_ms * 1000 + origin_us, end_us = end_ms * 1000 + origin_us;
    {
        int si = vin >= 0 ? vin : av_find_default_stream_index(in);
        AVStream *st = in->streams[si];
        int64_t ts = av_rescale_q(start_us, AV_TIME_BASE_Q, st->time_base);
        if (avformat_seek_file(in, si, INT64_MIN, ts, ts, 0) < 0)
            av_seek_frame(in, -1, 0, AVSEEK_FLAG_BACKWARD);      /* fall back to the start */
    }

    if (!(out->oformat->flags & AVFMT_NOFILE)) {
        rc = avio_open(&out->pb, out_path, AVIO_FLAG_WRITE);
        if (rc < 0) { av_strerror(rc, fail, sizeof(fail)); snprintf(err, err_size, "couldn't create %s (%s)", out_path, fail); goto end; }
        created = 1;
    }
    if (from_mov) av_dict_set(&opts, "movflags", "+faststart", 0);   /* plays while downloading/streaming */
    rc = avformat_write_header(out, &opts);
    av_dict_free(&opts);
    if (rc < 0) { av_strerror(rc, fail, sizeof(fail)); snprintf(err, err_size, "couldn't start the clip (%s)", fail); goto end; }
    header_written = 1;

    pkt = av_packet_alloc();
    if (!pkt) { snprintf(err, err_size, "out of memory"); goto end; }
    int started = vin < 0;                  /* video: from its first keyframe on */
    /* Done when every audio/video stream is past the end. (Not subtitles:
     * a track with no cue after the end would read the whole rest of a
     * two-hour film.) */
    int remaining = 0;
    for (unsigned i = 0; i < in->nb_streams; i++)
        if (map[i] >= 0 && in->streams[i]->codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE) remaining++;
    if (remaining == 0) remaining = 1;      /* subtitles only: stop on the cut-off below */
    while (remaining > 0 && av_read_frame(in, pkt) >= 0) {
        const int si = pkt->stream_index;
        if (si < 0 || (unsigned)si >= in->nb_streams || map[si] < 0 || done[si]) { av_packet_unref(pkt); continue; }
        AVStream *ist = in->streams[si], *ost = out->streams[map[si]];
        const int64_t t = pkt->dts != AV_NOPTS_VALUE ? pkt->dts : pkt->pts;
        if (t == AV_NOPTS_VALUE) { av_packet_unref(pkt); continue; }
        const int64_t t_us = av_rescale_q(t, ist->time_base, AV_TIME_BASE_Q);
        if (t_us > end_us + 10 * AV_TIME_BASE) { av_packet_unref(pkt); break; }   /* well past: stop */
        /* Past the end (decode order: once dts > end, nothing later shows
         * before it). Frames decoded before end but shown after are kept:
         * earlier frames may need them. */
        if (t_us > end_us) {
            done[si] = 1;
            if (ist->codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE) remaining--;
            av_packet_unref(pkt);
            continue;
        }
        if (si == vin && !started) {
            if (!(pkt->flags & AV_PKT_FLAG_KEY)) { av_packet_unref(pkt); continue; }
            started = 1;
        }
        if (!started) { av_packet_unref(pkt); continue; }   /* audio etc. before the first keyframe */
        /* Subtitles that ended before the clip starts: leave them out. */
        if (ist->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE) {
            int64_t dur_us = pkt->duration > 0 ? av_rescale_q(pkt->duration, ist->time_base, AV_TIME_BASE_Q) : 0;
            if (t_us + dur_us < start_us) { av_packet_unref(pkt); continue; }
        }
        /* Shift so start_ms is 0 (earlier frames go negative: see above). */
        const int64_t shift = av_rescale_q(start_us, AV_TIME_BASE_Q, ist->time_base);
        if (pkt->pts != AV_NOPTS_VALUE) pkt->pts -= shift;
        if (pkt->dts != AV_NOPTS_VALUE) pkt->dts -= shift;
        av_packet_rescale_ts(pkt, ist->time_base, ost->time_base);
        pkt->stream_index = map[si];
        pkt->pos = -1;
        rc = av_interleaved_write_frame(out, pkt);              /* takes the packet's data */
        if (rc < 0) { av_strerror(rc, fail, sizeof(fail)); snprintf(err, err_size, "couldn't write the clip (%s)", fail); goto end; }
        copied++;
    }
    if (copied == 0) { snprintf(err, err_size, "there's nothing in that part of the video"); goto end; }
    rc = av_write_trailer(out);
    header_written = 0;
    if (rc < 0) { av_strerror(rc, fail, sizeof(fail)); snprintf(err, err_size, "couldn't finish the clip (%s)", fail); goto end; }
    ret = 0;

end:
    if (header_written) av_write_trailer(out);
    av_packet_free(&pkt);
    if (out) {
        if (out->pb && !(out->oformat->flags & AVFMT_NOFILE)) avio_closep(&out->pb);
        avformat_free_context(out);
    }
    avformat_close_input(&in);
    free(map);
    free(done);
    if (ret != 0) {
        if (created) {          /* don't leave a broken clip behind -- only ever the file we made */
#if defined(_WIN32)
            wchar_t w[1100];
            if (MultiByteToWideChar(CP_UTF8, 0, out_path, -1, w, 1100) > 0) _wremove(w);
#else
            remove(out_path);
#endif
        }
        out_path[0] = '\0';
    }
    return ret;
}

static const lumen_demuxer_vtable_t VTABLE = {
    .probe = libav_probe,
    .open = libav_open,
    .read_packet = libav_read_packet,
    .packet_free = libav_packet_free,
    .seek = libav_seek,
    .close = libav_close,
    .open_ex = libav_open_ex,
    .seek_ex = libav_seek_ex,
    .export_clip = libav_export_clip,
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
