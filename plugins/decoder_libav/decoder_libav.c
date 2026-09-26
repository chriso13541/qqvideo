/*
 * decoder_libav.c -- one decoder plugin for every codec in the allowlist
 *
 * Replaces decoder_h264.c + decoder_aac.c for Linux system-FFmpeg builds.
 * Those two were already thin wrappers around the same
 * send_packet/receive_frame loop; once libavcodec comes from the distro
 * (one .so holding every decoder), there's no size or isolation benefit
 * left in shipping one Lumen plugin per codec, so they merge here.
 *
 * Which codecs are reachable is decided by plugins/common/libav_common.h
 * (the allowlist) intersected with what the system libavcodec actually
 * has -- probe() checks the latter at runtime, which is how Fedora's
 * patent-stripped ffmpeg-free (no H.264/HEVC) shows up as "unavailable"
 * instead of as a confusing failure at play time.
 *
 * Behavior carried over from the per-codec plugins, deliberately:
 *   - extradata is copied in before avcodec_open2 (AVCC/esds handling)
 *   - pts assigned at EMIT time from a display-order counter, reseeded
 *     (not zeroed) on flush -- see the v7 notes in lumen_plugin.h
 *   - drain() after EOF so B-frame-buffered frames aren't lost
 *   - skip_samples_start trimming (MP4 AAC priming, Opus pre-skip, ...)
 *
 * New here: video frames in ANY pixel format are converted to YUV420P
 * via libswscale. decoder_h264 memcpy'd planes assuming 8-bit 4:2:0,
 * which is fine for typical H.264 but garbage for 10-bit HEVC/AV1/VP9
 * (yuv420p10le) or 4:4:4 content -- common once more codecs exist.
 */
#include "../../include/lumen_plugin.h"
#include "../common/libav_common.h"
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define PLUGIN_VERSION "0.3.0"

struct lumen_decoder_ctx {
    AVCodecContext *avctx;
    AVPacket       *avpkt;
    AVFrame        *avframe;
    int             kind;                 /* LUMEN_PROBE_VIDEO / _AUDIO */
    int             flush_sent;
    int             output_frame_counter;
    double          frame_duration_ms;    /* video pacing */

    struct SwsContext *sws;               /* video: lazily created, only if needed */
    SwrContext        *swr;               /* audio: lazily created on first frame */
    int                channels;
};

/* ---- probe --------------------------------------------------------- */

static int libav_probe(const char *codec_fourcc) {
    const lumen_libav_codec_t *c = lumen_libav_by_fourcc(codec_fourcc);
    if (!c) return 0;                           /* not on the allowlist */
    if (!avcodec_find_decoder(c->id)) return 0; /* system FFmpeg built without it */
    return c->kind;
}

/* ---- open ---------------------------------------------------------- */

static int libav_open(lumen_decoder_ctx_t **out_ctx, const lumen_stream_desc_t *stream) {
    const lumen_libav_codec_t *c = lumen_libav_by_fourcc(stream->codec_fourcc);
    if (!c) return -1;
    const AVCodec *codec = avcodec_find_decoder(c->id);
    if (!codec) {
        fprintf(stderr, "decoder_libav: system FFmpeg has no decoder for %s\n", c->fourcc);
        return -1;
    }

    lumen_decoder_ctx_t *ctx = (lumen_decoder_ctx_t *)calloc(1, sizeof(*ctx));
    ctx->kind = c->kind;
    ctx->avctx = avcodec_alloc_context3(codec);
    if (!ctx->avctx) { free(ctx); return -1; }

    if (c->kind == LUMEN_PROBE_VIDEO) {
        double fps = (stream->frame_rate > 0.0) ? stream->frame_rate : 30.0;
        ctx->frame_duration_ms = 1000.0 / fps;
    } else {
        ctx->channels = stream->channels; /* 0 = unknown until first frame (raw ADTS) */
        ctx->avctx->sample_rate = stream->sample_rate;
        if (stream->channels > 0)
            av_channel_layout_default(&ctx->avctx->ch_layout, stream->channels);
    }

    if (stream->extradata && stream->extradata_size > 0) {
        ctx->avctx->extradata = (uint8_t *)av_mallocz(stream->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
        memcpy(ctx->avctx->extradata, stream->extradata, stream->extradata_size);
        ctx->avctx->extradata_size = stream->extradata_size;
    }

    if (avcodec_open2(ctx->avctx, codec, NULL) < 0) {
        avcodec_free_context(&ctx->avctx);
        free(ctx);
        return -1;
    }
    ctx->avpkt = av_packet_alloc();
    ctx->avframe = av_frame_alloc();
    *out_ctx = ctx;
    return 0;
}

/* ---- video: any pixfmt -> tightly-packed YUV420P ------------------- */

static int emit_video(lumen_decoder_ctx_t *ctx, const AVFrame *avf, lumen_frame_t *out) {
    int w = avf->width, h = avf->height;
    int cw = (w + 1) / 2, ch = (h + 1) / 2;   /* round UP for odd sizes */

    memset(out, 0, sizeof(*out));
    out->type = LUMEN_STREAM_VIDEO;
    out->video.width = w;
    out->video.height = h;
    out->video.pixel_format = LUMEN_PIXFMT_YUV420P;

    int pw[3] = { w, cw, cw }, ph[3] = { h, ch, ch };
    for (int p = 0; p < 3; p++) {
        out->video.planes[p] = (uint8_t *)malloc((size_t)pw[p] * ph[p]);
        out->video.stride[p] = pw[p];
        if (!out->video.planes[p]) return -1;
    }

    enum AVPixelFormat fmt = (enum AVPixelFormat)avf->format;
    if (fmt == AV_PIX_FMT_YUV420P || fmt == AV_PIX_FMT_YUVJ420P) {
        /* Fast path: already 8-bit 4:2:0 -- straight plane copy, which
         * keeps output bit-identical to ffmpeg's own rawvideo decode. */
        for (int p = 0; p < 3; p++)
            for (int row = 0; row < ph[p]; row++)
                memcpy(out->video.planes[p] + (size_t)row * pw[p],
                       avf->data[p] + (size_t)row * avf->linesize[p], pw[p]);
        return 0;
    }

    /* Anything else (10-bit, 4:2:2, 4:4:4, NV12 from some decoders...) */
    ctx->sws = sws_getCachedContext(ctx->sws, w, h, fmt, w, h, AV_PIX_FMT_YUV420P,
                                    SWS_BILINEAR, NULL, NULL, NULL);
    if (!ctx->sws) return -1;
    sws_scale(ctx->sws, (const uint8_t *const *)avf->data, avf->linesize, 0, h,
              out->video.planes, out->video.stride);
    return 0;
}

/* ---- audio: any sample fmt -> interleaved S16 ---------------------- */

static int emit_audio(lumen_decoder_ctx_t *ctx, const AVFrame *avf, lumen_frame_t *out) {
    if (ctx->channels <= 0) ctx->channels = avf->ch_layout.nb_channels;

    if (!ctx->swr) {
        AVChannelLayout out_layout;
        av_channel_layout_default(&out_layout, ctx->channels);
        int rc = swr_alloc_set_opts2(&ctx->swr,
            &out_layout, AV_SAMPLE_FMT_S16, avf->sample_rate,
            &avf->ch_layout, (enum AVSampleFormat)avf->format, avf->sample_rate, 0, NULL);
        av_channel_layout_uninit(&out_layout);
        if (rc < 0 || !ctx->swr || swr_init(ctx->swr) < 0) {
            if (ctx->swr) swr_free(&ctx->swr);
            return -1;
        }
    }

    int max_out = avf->nb_samples + 16;
    uint8_t *buf = (uint8_t *)malloc((size_t)max_out * ctx->channels * sizeof(int16_t));
    if (!buf) return -1;
    int n = swr_convert(ctx->swr, &buf, max_out, (const uint8_t **)avf->data, avf->nb_samples);
    if (n < 0) { free(buf); return -1; }

    memset(out, 0, sizeof(*out));
    out->type = LUMEN_STREAM_AUDIO;
    out->audio.data = buf;
    out->audio.size = (size_t)n * ctx->channels * sizeof(int16_t);
    out->audio.sample_rate = avf->sample_rate;
    out->audio.channels = ctx->channels;
    out->audio.sample_fmt = LUMEN_SAMPLEFMT_S16;
    out->audio.nb_samples = n;
    return 0;
}

/* Returns 0 = frame emitted, 1 = nothing to emit, <0 = error */
static int emit(lumen_decoder_ctx_t *ctx, lumen_frame_t *out, int skip_samples, int skip_end) {
    int rc = (ctx->kind == LUMEN_PROBE_VIDEO) ? emit_video(ctx, ctx->avframe, out)
                                              : emit_audio(ctx, ctx->avframe, out);
    av_frame_unref(ctx->avframe);
    if (rc < 0) {
        if (ctx->kind == LUMEN_PROBE_VIDEO) for (int p = 0; p < 4; p++) free(out->video.planes[p]);
        return -1;
    }

    if (ctx->kind == LUMEN_PROBE_VIDEO) {
        out->pts = (int64_t)(ctx->output_frame_counter * ctx->frame_duration_ms + 0.5);
        ctx->output_frame_counter++;
        return 0;
    }

    if (skip_samples > 0) {
        size_t bps = (size_t)ctx->channels * sizeof(int16_t);
        size_t skip_bytes = (size_t)skip_samples * bps;
        if (skip_bytes >= out->audio.size) {       /* whole frame was priming */
            free(out->audio.data);
            out->audio.data = NULL;
            return 1;
        }
        memmove(out->audio.data, out->audio.data + skip_bytes, out->audio.size - skip_bytes);
        out->audio.size -= skip_bytes;
        out->audio.nb_samples -= skip_samples;
    }
    if (skip_end > 0) {
        if (skip_end >= out->audio.nb_samples) {    /* whole frame is padding */
            free(out->audio.data);
            out->audio.data = NULL;
            return 1;
        }
        out->audio.nb_samples -= skip_end;
        out->audio.size = (size_t)out->audio.nb_samples * ctx->channels * sizeof(int16_t);
    }
    out->pts = ctx->output_frame_counter++;
    return 0;
}

/* ---- vtable -------------------------------------------------------- */

static int libav_decode(lumen_decoder_ctx_t *ctx, const lumen_packet_t *pkt, lumen_frame_t *out) {
    ctx->avpkt->data = pkt->data;
    ctx->avpkt->size = (int)pkt->size;
    /* Audio pts is our own emit counter, and we don't know the stream
     * time_base here -- passing a raw pts makes libavcodec warn "Could
     * not update timestamps for skipped samples" whenever a decoder trims
     * its own pre-skip (Opus does). */
    ctx->avpkt->pts = (ctx->kind == LUMEN_PROBE_VIDEO) ? pkt->pts : AV_NOPTS_VALUE;
    ctx->avpkt->flags = pkt->keyframe ? AV_PKT_FLAG_KEY : 0;

    int send_rc = avcodec_send_packet(ctx->avctx, ctx->avpkt);
    if (send_rc < 0 && send_rc != AVERROR(EAGAIN)) return -1;

    int recv_rc = avcodec_receive_frame(ctx->avctx, ctx->avframe);
    if (recv_rc == AVERROR(EAGAIN) || recv_rc == AVERROR_EOF) return 1;
    if (recv_rc < 0) return -1;
    return emit(ctx, out, pkt->skip_samples_start, pkt->skip_samples_end);
}

static int libav_drain(lumen_decoder_ctx_t *ctx, lumen_frame_t *out) {
    if (!ctx->flush_sent) {
        avcodec_send_packet(ctx->avctx, NULL);
        ctx->flush_sent = 1;
    }
    for (;;) {
        if (avcodec_receive_frame(ctx->avctx, ctx->avframe) < 0) return 1;
        int rc = emit(ctx, out, 0, 0);
        if (rc == 0) return 0;
        if (rc < 0) return 1;
    }
}

static void libav_frame_free(lumen_frame_t *frame) {
    if (frame->type == LUMEN_STREAM_VIDEO) {
        for (int p = 0; p < 4; p++) { free(frame->video.planes[p]); frame->video.planes[p] = NULL; }
    } else {
        free(frame->audio.data);
        frame->audio.data = NULL;
    }
}

static void libav_flush(lumen_decoder_ctx_t *ctx, int64_t resume_at_ms) {
    avcodec_flush_buffers(ctx->avctx);
    ctx->flush_sent = 0;
    if (ctx->kind == LUMEN_PROBE_VIDEO)
        ctx->output_frame_counter = (int)(resume_at_ms / ctx->frame_duration_ms);
    else
        ctx->output_frame_counter = 0;
}

static void libav_close(lumen_decoder_ctx_t *ctx) {
    if (!ctx) return;
    if (ctx->swr) swr_free(&ctx->swr);
    if (ctx->sws) sws_freeContext(ctx->sws);
    av_frame_free(&ctx->avframe);
    av_packet_free(&ctx->avpkt);
    avcodec_free_context(&ctx->avctx);
    free(ctx);
}

static const lumen_decoder_vtable_t VTABLE = {
    .probe = libav_probe,
    .open = libav_open,
    .decode = libav_decode,
    .frame_free = libav_frame_free,
    .drain = libav_drain,
    .flush = libav_flush,
    .close = libav_close,
};

static lumen_plugin_descriptor_t DESCRIPTOR = {
    .abi_version = LUMEN_ABI_VERSION,
    .kind = LUMEN_PLUGIN_DECODER,
    .name = "libav-decoder",
    .version = PLUGIN_VERSION,
    .vtable = { .decoder = &VTABLE },
};

LUMEN_EXPORT const lumen_plugin_descriptor_t *lumen_get_plugin(void) {
    DESCRIPTOR.version = lumen_libav_version_string(PLUGIN_VERSION);
    return &DESCRIPTOR;
}
