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
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
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
    int             output_frame_counter; /* audio fallback when no timestamps */
    double          frame_duration_ms;    /* video: fallback spacing for frames with no pts */
    int64_t         last_pts_ms;          /* last emitted pts, for the no-pts fallback */
    int64_t         samples_out;          /* audio: samples emitted, for the no-pts fallback */

    AVPacket       *pending;              /* packet libavcodec refused with EAGAIN (frames
                                           * were waiting); resent from receive() */
    int             skip_start, skip_end; /* sample trimming for the next frame out */
    uint8_t        *pad_buf;              /* packet copy + zeroed padding, see padded() */
    size_t          pad_cap;
    struct SwsContext *sws;               /* video: lazily created, only if needed */
    SwrContext        *swr;               /* audio: lazily created on first frame */
    int                channels;
    int                bitmap;            /* image subtitles (PGS, VobSub, DVB) */
    enum AVCodecID     bitmap_codec;

    /* Hardware decoding: the GPU decodes, frames are copied back to
     * memory (av_hwframe_transfer_data) and converted like any other. */
    AVBufferRef       *hw_device;         /* NULL = software */
    enum AVPixelFormat hw_pix_fmt;        /* frames in GPU memory come in this format */
    const char        *hw_name;           /* "d3d11va", "vaapi", ... */
    int                hw_active;         /* the current stream really decodes on the GPU */
    AVFrame           *swframe;           /* GPU frames copied back here */
    const AVCodec     *codec;             /* for reopening in software after a GPU failure */
    int                hw_errors;         /* GPU decode errors (3 = give up on the GPU) */
    int                hw_said_cpu;       /* "can't decode this video" already logged */
};

/* ---- hardware decoding ----------------------------------------------------
 * Tried in this order where FFmpeg was built with them (`ffmpeg -hwaccels`)
 * and the codec supports them; the first device that opens wins. Off with
 * QQVIDEO_HWDEC=0 (the core sets it from Preferences > Video). */
static const enum AVHWDeviceType k_hw_types[] = {
#if defined(_WIN32)
    AV_HWDEVICE_TYPE_D3D11VA, AV_HWDEVICE_TYPE_DXVA2,
#elif defined(__APPLE__)
    AV_HWDEVICE_TYPE_VIDEOTOOLBOX,
#else
    AV_HWDEVICE_TYPE_VAAPI, AV_HWDEVICE_TYPE_CUDA,
#endif
};

/* libavcodec offers the formats it can decode this stream to: take the
 * GPU's if it's among them, else fall back to software for this stream
 * (the GPU can't do this profile / size -- 4:4:4 H.264, say). */
static enum AVPixelFormat pick_format(AVCodecContext *avctx, const enum AVPixelFormat *fmts) {
    lumen_decoder_ctx_t *ctx = (lumen_decoder_ctx_t *)avctx->opaque;
    for (const enum AVPixelFormat *p = fmts; *p != AV_PIX_FMT_NONE; p++)
        if (*p == ctx->hw_pix_fmt) {
            if (!ctx->hw_active) printf("decoder_libav: hardware decoding (%s)\n", ctx->hw_name);
            ctx->hw_active = 1;
            return *p;
        }
    if (ctx->hw_active || !ctx->hw_said_cpu)
        printf("decoder_libav: %s can't decode this video -- using the CPU\n", ctx->hw_name);
    ctx->hw_active = 0;
    ctx->hw_said_cpu = 1;
    for (const enum AVPixelFormat *p = fmts; *p != AV_PIX_FMT_NONE; p++) {
        const AVPixFmtDescriptor *d = av_pix_fmt_desc_get(*p);
        if (d && !(d->flags & AV_PIX_FMT_FLAG_HWACCEL)) return *p;
    }
    return AV_PIX_FMT_NONE;
}

/* Set up the first hardware decoder that works for this codec. */
static void hw_setup(lumen_decoder_ctx_t *ctx, const AVCodec *codec) {
    const char *env = getenv("QQVIDEO_HWDEC");
    if (env && (env[0] == '0' || !strcmp(env, "off") || !strcmp(env, "no"))) return;
    for (size_t t = 0; t < sizeof(k_hw_types) / sizeof(k_hw_types[0]); t++) {
        enum AVPixelFormat fmt = AV_PIX_FMT_NONE;
        for (int i = 0;; i++) {
            const AVCodecHWConfig *cfg = avcodec_get_hw_config(codec, i);
            if (!cfg) break;
            if ((cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) && cfg->device_type == k_hw_types[t]) {
                fmt = cfg->pix_fmt;
                break;
            }
        }
        if (fmt == AV_PIX_FMT_NONE) continue;                 /* codec can't use this one */
        AVBufferRef *dev = NULL;
        if (av_hwdevice_ctx_create(&dev, k_hw_types[t], NULL, NULL, 0) < 0) continue;   /* no such GPU/driver */
        ctx->hw_device = dev;
        ctx->hw_pix_fmt = fmt;
        ctx->hw_name = av_hwdevice_get_type_name(k_hw_types[t]);
        ctx->avctx->hw_device_ctx = av_buffer_ref(dev);
        ctx->avctx->opaque = ctx;
        ctx->avctx->get_format = pick_format;
        return;
    }
}

/* libavcodec needs AV_INPUT_BUFFER_PADDING_SIZE zero bytes after every
 * packet (decoders over-read for speed; text subtitle decoders need the 0
 * to find the end of the string -- without it, SRT cues picked up stale
 * heap bytes: "arrest him.>!"). The Lumen ABI never promised padding, so
 * rather than trust each demuxer, copy into our own padded buffer. The
 * copy is tiny next to decoding the packet. */
static uint8_t *padded(lumen_decoder_ctx_t *ctx, const uint8_t *data, size_t size) {
    size_t need = size + AV_INPUT_BUFFER_PADDING_SIZE;
    if (need > ctx->pad_cap) {
        uint8_t *n = (uint8_t *)realloc(ctx->pad_buf, need);
        if (!n) return NULL;
        ctx->pad_buf = n;
        ctx->pad_cap = need;
    }
    if (size) memcpy(ctx->pad_buf, data, size);
    memset(ctx->pad_buf + size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    return ctx->pad_buf;
}

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

    if (c->kind == LUMEN_PROBE_SUBTITLE) {
        ctx->bitmap = c->bitmap;
        ctx->bitmap_codec = c->id;
        if (stream->width > 0 && stream->height > 0) {   /* image subtitles' canvas, if known */
            ctx->avctx->width = stream->width;
            ctx->avctx->height = stream->height;
        }
        /* The demuxer hands us pts_ms/duration_ms, so tell libavcodec the
         * packet clock is milliseconds: it then fills AVSubtitle.pts and
         * end_display_time (from packet duration) on that same clock. */
        ctx->avctx->pkt_timebase = (AVRational){1, 1000};
    } else if (c->kind == LUMEN_PROBE_VIDEO) {
        ctx->avctx->pkt_timebase = (AVRational){1, 1000};  /* see libav_decode() */
        ctx->last_pts_ms = -1;
        double fps = (stream->frame_rate > 0.0) ? stream->frame_rate : 30.0;
        ctx->frame_duration_ms = 1000.0 / fps;
    } else {
        ctx->avctx->pkt_timebase = (AVRational){1, 1000};
        ctx->last_pts_ms = -1;
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

    if (c->kind == LUMEN_PROBE_VIDEO) {
        /* Multithreaded decoding: frame threads (several frames in flight)
         * plus slice threads where the codec supports them. 0 = one thread
         * per CPU core, chosen by libavcodec. Roughly doubles decode speed
         * on a dual core. QQVIDEO_DECODE_THREADS=N overrides (1 = off), for
         * troubleshooting. Audio stays single-threaded: it's cheap, and
         * frame threads add latency. */
        const char *env = getenv("QQVIDEO_DECODE_THREADS");
        ctx->avctx->thread_count = env ? atoi(env) : 0;
        ctx->avctx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
        ctx->codec = codec;
        hw_setup(ctx, codec);
        if (!ctx->hw_device) {
            const char *hv = getenv("QQVIDEO_HWDEC");
            if (!hv || (hv[0] != '0' && strcmp(hv, "off") && strcmp(hv, "no")))
                printf("decoder_libav: no hardware decoder for %s here -- using the CPU\n", c->fourcc);
        }
    }
    if (avcodec_open2(ctx->avctx, codec, NULL) < 0) {
        avcodec_free_context(&ctx->avctx);
        free(ctx);
        return -1;
    }
    ctx->avpkt = av_packet_alloc();
    ctx->pending = av_packet_alloc();
    ctx->avframe = av_frame_alloc();
    *out_ctx = ctx;
    return 0;
}

/* The GPU failed mid-stream (driver error, lost device, failed copy-back):
 * rebuild the decoder in software from the same stream parameters and
 * carry on. The picture resumes at the next keyframe. */
static void hw_give_up(lumen_decoder_ctx_t *ctx, const char *why) {
    if (!ctx->hw_device || !ctx->codec) return;
    AVCodecParameters *par = avcodec_parameters_alloc();
    AVCodecContext *n = avcodec_alloc_context3(ctx->codec);
    int ok = par && n && avcodec_parameters_from_context(par, ctx->avctx) >= 0 &&
             avcodec_parameters_to_context(n, par) >= 0;
    if (ok) {
        n->pkt_timebase = ctx->avctx->pkt_timebase;
        n->thread_count = ctx->avctx->thread_count;
        n->thread_type = ctx->avctx->thread_type;
        n->skip_frame = ctx->avctx->skip_frame;
        n->skip_loop_filter = ctx->avctx->skip_loop_filter;
        ok = avcodec_open2(n, ctx->codec, NULL) >= 0;
    }
    avcodec_parameters_free(&par);
    if (!ok) { avcodec_free_context(&n); return; }
    printf("decoder_libav: %s failed (%s) -- switching to the CPU\n", ctx->hw_name, why);
    avcodec_free_context(&ctx->avctx);
    ctx->avctx = n;
    av_buffer_unref(&ctx->hw_device);
    ctx->hw_active = 0;
    av_packet_unref(ctx->pending);
}

/* A decode error while the GPU decodes: three of those and it's the GPU. */
static void hw_error(lumen_decoder_ctx_t *ctx, const char *why) {
    if (ctx->hw_device && ctx->hw_active && ++ctx->hw_errors >= 3) hw_give_up(ctx, why);
}

/* ---- video: any pixfmt -> tightly-packed YUV420P ------------------- */

/* 10-bit -> 8-bit, rounded: the whole conversion for yuv420p10le (HEVC
 * Main10, AV1/VP9 10-bit), since the size doesn't change. Written so the
 * compiler vectorizes it (no branches; "v - (v >> 8)" clamps 256 -> 255):
 * 3.4 ms per 4K frame at -O3 vs 8.9 ms through swscale (measured), i.e.
 * ~13% of a CPU core freed at 24 fps -- on exactly the 10-bit 4K HEVC case
 * where the CPU is shortest. Output is within 1 level of swscale's. */
static void shift_10_to_8(const uint8_t *src, int sstride, uint8_t *dst, int dstride, int w, int h) {
    for (int y = 0; y < h; y++) {
        const uint16_t *sp = (const uint16_t *)(src + (size_t)y * sstride);
        uint8_t *dp = dst + (size_t)y * dstride;
        for (int x = 0; x < w; x++) {
            uint16_t v = (uint16_t)((sp[x] + 2u) >> 2);
            dp[x] = (uint8_t)(v - (v >> 8));
        }
    }
}

/* ---- video frame buffers --------------------------------------------------
 * All three planes of a frame live in one block, recycled through a small
 * pool instead of malloc/free per frame: a 1080p frame is 3 MB, and at
 * 240 fps that was 720 MB/s of fresh memory -- on Windows the heap hands
 * blocks that big straight to the OS and back (zero-filled pages, page
 * faults) every time. Frames are allocated on the decode thread and freed
 * on the main thread, hence the (tiny) lock. Blocks of another size (the
 * resolution changed) are simply freed. */
#define VBUF_HDR   64                     /* keeps the planes 64-byte aligned */
#define VBUF_POOL  32
static void   *g_vbuf[VBUF_POOL];
static int     g_vbuf_n;
static size_t  g_vbuf_size;
static char    g_vbuf_lock;

static void vbuf_lock(void)   { while (__atomic_test_and_set(&g_vbuf_lock, __ATOMIC_ACQUIRE)) { } }
static void vbuf_unlock(void) { __atomic_clear(&g_vbuf_lock, __ATOMIC_RELEASE); }

static uint8_t *vbuf_get(size_t size) {
    void *b = NULL;
    vbuf_lock();
    if (size != g_vbuf_size) {            /* new resolution: drop the old blocks */
        for (int i = 0; i < g_vbuf_n; i++) free(g_vbuf[i]);
        g_vbuf_n = 0;
        g_vbuf_size = size;
    } else if (g_vbuf_n > 0) {
        b = g_vbuf[--g_vbuf_n];
    }
    vbuf_unlock();
    if (!b) {
        b = malloc(VBUF_HDR + size);
        if (!b) return NULL;
        *(size_t *)b = size;
    }
    return (uint8_t *)b + VBUF_HDR;
}

static void vbuf_put(uint8_t *plane0) {
    if (!plane0) return;
    void *b = plane0 - VBUF_HDR;
    const size_t size = *(size_t *)b;
    vbuf_lock();
    if (size == g_vbuf_size && g_vbuf_n < VBUF_POOL) { g_vbuf[g_vbuf_n++] = b; b = NULL; }
    vbuf_unlock();
    free(b);
}

static void video_frame_release(lumen_frame_t *f) {
    vbuf_put(f->video.planes[0]);         /* planes 1 and 2 are inside the same block */
    for (int p = 0; p < 4; p++) f->video.planes[p] = NULL;
}

/* P010/P016's interleaved UV (16-bit, value in the top bits) -> separate
 * 8-bit U and V planes. */
static void split_uv(const uint8_t *src, int sstride, uint8_t *u, uint8_t *v, int dstride, int cw, int ch) {
    for (int y = 0; y < ch; y++) {
        uint8_t *up = u + (size_t)y * dstride, *vp = v + (size_t)y * dstride;
        const uint16_t *sp = (const uint16_t *)(src + (size_t)y * sstride);
        for (int x = 0; x < cw; x++) { up[x] = (uint8_t)(sp[2 * x] >> 8); vp[x] = (uint8_t)(sp[2 * x + 1] >> 8); }
    }
}

static int emit_video(lumen_decoder_ctx_t *ctx, const AVFrame *avf, lumen_frame_t *out) {
    const char *hw = NULL;
    if (ctx->hw_device && avf->format == ctx->hw_pix_fmt) {
        /* Decoded on the GPU: copy it back to memory (NV12 or P010, usually). */
        if (!ctx->swframe && !(ctx->swframe = av_frame_alloc())) return -1;
        av_frame_unref(ctx->swframe);
        if (av_hwframe_transfer_data(ctx->swframe, avf, 0) < 0) { hw_error(ctx, "copying a frame back"); return -1; }
        avf = ctx->swframe;
        hw = ctx->hw_name;
    }
    int w = avf->width, h = avf->height;
    int cw = (w + 1) / 2, ch = (h + 1) / 2;   /* round UP for odd sizes */

    memset(out, 0, sizeof(*out));
    out->type = LUMEN_STREAM_VIDEO;
    out->video.width = w;
    out->video.height = h;
    out->video.pixel_format = LUMEN_PIXFMT_YUV420P;
    out->video.hwaccel = hw;

    int pw[3] = { w, cw, cw }, ph[3] = { h, ch, ch };
    const size_t ysz = (size_t)w * h, csz = (size_t)cw * ch;
    uint8_t *blk = vbuf_get(ysz + 2 * csz);
    if (!blk) return -1;
    out->video.planes[0] = blk;
    out->video.planes[1] = blk + ysz;
    out->video.planes[2] = blk + ysz + csz;
    for (int p = 0; p < 3; p++) out->video.stride[p] = pw[p];

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

    if (fmt == AV_PIX_FMT_YUV420P10LE) {
        for (int p = 0; p < 3; p++)
            shift_10_to_8(avf->data[p], avf->linesize[p], out->video.planes[p], pw[p], pw[p], ph[p]);
        return 0;
    }

    /* 10-bit from the GPU: P010 (10 bits in the top of 16, U and V
     * interleaved). A plain loop here is twice as fast as swscale (1.75 vs
     * 3.88 ms per 1080p frame, measured). 8-bit NV12 goes through swscale
     * below: its SIMD de-interleave beats a plain loop (0.36 vs 0.69 ms). */
    if (fmt == AV_PIX_FMT_P010LE || fmt == AV_PIX_FMT_P016LE) {
        for (int y = 0; y < h; y++) {
            const uint16_t *sp = (const uint16_t *)(avf->data[0] + (size_t)y * avf->linesize[0]);
            uint8_t *dp = out->video.planes[0] + (size_t)y * pw[0];
            for (int x = 0; x < w; x++) dp[x] = (uint8_t)(sp[x] >> 8);
        }
        split_uv(avf->data[1], avf->linesize[1], out->video.planes[1], out->video.planes[2], cw, cw, ch);
        return 0;
    }

    /* Anything else (12-bit, 4:2:2, 4:4:4, NV12 from some decoders...) */
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
    /* Real presentation time in ms (packets go in with ms pts and
     * pkt_timebase = 1/1000, so best_effort_timestamp comes out in ms). */
    int64_t ts = ctx->avframe->best_effort_timestamp;
    int sr = ctx->avframe->sample_rate;
    int rc = (ctx->kind == LUMEN_PROBE_VIDEO) ? emit_video(ctx, ctx->avframe, out)
                                              : emit_audio(ctx, ctx->avframe, out);
    av_frame_unref(ctx->avframe);
    if (rc < 0) {
        if (ctx->kind == LUMEN_PROBE_VIDEO) video_frame_release(out);
        return -1;
    }

    if (ctx->kind == LUMEN_PROBE_VIDEO) {
        /* Use the container's timestamp. The old frame counter assumed
         * decoding resumed exactly at the seek target, but seeks land on
         * the keyframe BEFORE it -- frames from 10s got labeled 15s, and
         * the seek bar + subtitles stayed that far off until the next
         * seek. Counting also drifted on variable-frame-rate video.
         * The counter survives only as a fallback for streams with no
         * timestamps at all (raw .h264). */
        if (ts != AV_NOPTS_VALUE)
            out->pts = ts;
        else
            out->pts = ctx->last_pts_ms < 0 ? 0 : ctx->last_pts_ms + (int64_t)(ctx->frame_duration_ms + 0.5);
        ctx->last_pts_ms = out->pts;
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
    if (ts != AV_NOPTS_VALUE) {
        out->pts = ts + (sr > 0 ? (int64_t)skip_samples * 1000 / sr : 0);
    } else if (sr > 0) {
        /* No timestamps (raw ADTS .aac): time from samples emitted. This
         * used to be a frame INDEX, which is meaningless as a clock. */
        out->pts = ctx->samples_out * 1000 / sr;
    }
    ctx->samples_out += out->audio.nb_samples;
    ctx->output_frame_counter++;
    return 0;
}

/* ---- subtitles: libavcodec ASS events -> plain text ---------------- */

/* Every text subtitle decoder in libavcodec (SubRip, WebVTT, mov_text,
 * ASS itself) outputs ASS "Dialogue" events:
 *     ReadOrder,Layer,Style,Name,MarginL,MarginR,MarginV,Effect,Text
 * so one converter handles all of them. The Text field can contain:
 *   {\i1}, {\an8}, {\c&H00FF00&} ...  override tags  -> dropped
 *   \N / \n                            line breaks     -> '\n'
 *   \h                                  hard space      -> ' '
 *   {\p1} m 0 0 l 100 0 ... {\p0}      vector drawing  -> skipped entirely
 *     (without that last rule, typesetting in anime/fansub ASS files would
 *      show up as lines of raw drawing coordinates) */
static void append_plain(const char *ass, char *out, size_t cap, size_t *len) {
    const char *t = ass;
    for (int commas = 0; *t && commas < 8; t++)
        if (*t == ',') commas++;

    int drawing = 0;
    size_t start_len = *len;
    while (*t && *len + 1 < cap) {
        if (*t == '{') {
            const char *close = strchr(t, '}');
            if (!close) break;
            for (const char *q = t; q < close; q++) {        /* look for \pN */
                if (q[0] == '\\' && q[1] == 'p' && q[2] >= '0' && q[2] <= '9')
                    drawing = (q[2] != '0');
            }
            t = close + 1;
            continue;
        }
        if (t[0] == '\\' && (t[1] == 'N' || t[1] == 'n')) { if (!drawing) out[(*len)++] = '\n'; t += 2; continue; }
        if (t[0] == '\\' && t[1] == 'h')                   { if (!drawing) out[(*len)++] = ' ';  t += 2; continue; }
        if (!drawing && *t != '\r') out[(*len)++] = *t;
        t++;
    }
    /* trim trailing whitespace/newlines this event added */
    while (*len > start_len && (out[*len - 1] == '\n' || out[*len - 1] == ' ')) (*len)--;
    out[*len] = '\0';
}

/* Image subtitles: each AVSubtitleRect is 8-bit palette indices plus a
 * palette of 32-bit 0xAARRGGBB colors; turn them into RGBA pictures. */
static lumen_sub_image_t *bitmap_image(lumen_decoder_ctx_t *ctx, const AVSubtitle *sub) {
    int n = 0;
    for (unsigned i = 0; i < sub->num_rects; i++)
        if (sub->rects[i]->type == SUBTITLE_BITMAP && sub->rects[i]->w > 0 && sub->rects[i]->h > 0) n++;
    if (n == 0) return NULL;
    lumen_sub_image_t *im = (lumen_sub_image_t *)calloc(1, sizeof(*im));
    if (!im) return NULL;
    im->rects = (lumen_sub_rect_t *)calloc((size_t)n, sizeof(lumen_sub_rect_t));
    if (!im->rects) { free(im); return NULL; }
    im->canvas_w = ctx->avctx->width;
    im->canvas_h = ctx->avctx->height;
    if ((im->canvas_w <= 0 || im->canvas_h <= 0) && ctx->bitmap_codec == AV_CODEC_ID_DVB_SUBTITLE) {
        im->canvas_w = 720;                     /* DVB's default display, when none is given */
        im->canvas_h = 576;
    }
    for (unsigned i = 0; i < sub->num_rects; i++) {
        const AVSubtitleRect *r = sub->rects[i];
        if (r->type != SUBTITLE_BITMAP || r->w <= 0 || r->h <= 0 || !r->data[0] || !r->data[1]) continue;
        uint8_t *px = (uint8_t *)malloc((size_t)r->w * r->h * 4);
        if (!px) break;
        const uint32_t *pal = (const uint32_t *)r->data[1];
        const int ncolors = r->nb_colors > 0 && r->nb_colors <= 256 ? r->nb_colors : 256;
        for (int y = 0; y < r->h; y++) {
            const uint8_t *src = r->data[0] + (size_t)y * r->linesize[0];
            uint8_t *dst = px + (size_t)y * r->w * 4;
            for (int x = 0; x < r->w; x++) {
                const uint32_t c = src[x] < ncolors ? pal[src[x]] : 0;
                dst[x * 4 + 0] = (uint8_t)(c >> 16);    /* R */
                dst[x * 4 + 1] = (uint8_t)(c >> 8);     /* G */
                dst[x * 4 + 2] = (uint8_t)c;            /* B */
                dst[x * 4 + 3] = (uint8_t)(c >> 24);    /* A */
            }
        }
        lumen_sub_rect_t *o = &im->rects[im->count++];
        o->x = r->x; o->y = r->y; o->w = r->w; o->h = r->h;
        o->rgba = px;
    }
    if (im->count == 0) { free(im->rects); free(im); return NULL; }
    return im;
}

static void bitmap_image_free(lumen_sub_image_t *im) {
    if (!im) return;
    for (int i = 0; i < im->count; i++) free(im->rects[i].rgba);
    free(im->rects);
    free(im);
}

static int decode_subtitle(lumen_decoder_ctx_t *ctx, const lumen_packet_t *pkt, lumen_frame_t *out) {
    AVSubtitle sub;
    int got = 0;
    ctx->avpkt->data = padded(ctx, pkt->data, pkt->size);
    if (!ctx->avpkt->data) return -1;
    ctx->avpkt->size = (int)pkt->size;
    ctx->avpkt->pts = pkt->pts_ms >= 0 ? pkt->pts_ms : AV_NOPTS_VALUE;
    ctx->avpkt->duration = pkt->duration_ms > 0 ? pkt->duration_ms : 0;
    if (avcodec_decode_subtitle2(ctx->avctx, &sub, &got, ctx->avpkt) < 0) return -1;
    if (!got) return 1;

    char text[1024];
    size_t len = 0;
    text[0] = '\0';
    for (unsigned i = 0; i < sub.num_rects; i++) {
        const AVSubtitleRect *r = sub.rects[i];
        const char *src = (r->type == SUBTITLE_ASS) ? r->ass : (r->type == SUBTITLE_TEXT ? r->text : NULL);
        if (!src || !*src) continue;
        if (len > 0 && len + 1 < sizeof(text)) text[len++] = '\n';
        if (r->type == SUBTITLE_ASS) {
            append_plain(src, text, sizeof(text), &len);
        } else {
            size_t n = strlen(src);
            if (len + n >= sizeof(text)) n = sizeof(text) - len - 1;
            memcpy(text + len, src, n);
            len += n;
            text[len] = '\0';
        }
    }

    int64_t base = (sub.pts != AV_NOPTS_VALUE) ? sub.pts / 1000 : pkt->pts_ms;
    int64_t start = base + sub.start_display_time;
    int64_t end = -1;
    if (sub.end_display_time > 0 && sub.end_display_time != UINT32_MAX)
        end = base + sub.end_display_time;
    else if (pkt->duration_ms > 0 && !ctx->bitmap)    /* PGS: the next event ends it */
        end = start + pkt->duration_ms;

    if (ctx->bitmap) {
        /* An image -- or, with no rectangles, a "clear the screen" event
         * (PGS ends every subtitle that way): emitted too, as an empty cue. */
        lumen_sub_image_t *im = bitmap_image(ctx, &sub);
        avsubtitle_free(&sub);
        if (base < 0) { bitmap_image_free(im); return 1; }
        memset(out, 0, sizeof(*out));
        out->type = LUMEN_STREAM_SUBTITLE;
        out->pts = start;
        out->subtitle.text = strdup("");
        out->subtitle.start_ms = start;
        out->subtitle.end_ms = end;
        out->subtitle.image = im;
        if (!out->subtitle.text) { bitmap_image_free(im); return -1; }
        return 0;
    }
    avsubtitle_free(&sub);

    /* Empty events are "clear the screen" markers (mov_text uses them
     * between lines); with end times on every real cue, nothing to emit. */
    if (len == 0 || base < 0) return 1;

    memset(out, 0, sizeof(*out));
    out->type = LUMEN_STREAM_SUBTITLE;
    out->pts = start;
    out->subtitle.text = strdup(text);
    out->subtitle.start_ms = start;
    out->subtitle.end_ms = end;
    return out->subtitle.text ? 0 : -1;
}

/* ---- vtable -------------------------------------------------------- */

/* FFmpeg's decode contract: after sending a packet, keep calling
 * receive_frame until it says EAGAIN -- a packet may produce zero, one or
 * several frames. And send_packet itself may answer EAGAIN ("collect my
 * frames first"); the old code ignored that and the packet was lost.
 * libav_receive() is the one place frames come out. */
static int libav_receive(lumen_decoder_ctx_t *ctx, lumen_frame_t *out) {
    if (ctx->kind == LUMEN_PROBE_SUBTITLE) return 1;
    for (;;) {
        int rc = avcodec_receive_frame(ctx->avctx, ctx->avframe);
        if (rc == 0) {
            /* Trimming info belongs to the packet just sent: apply it to
             * the first frame out, then clear it. */
            int e = emit(ctx, out, ctx->skip_start, ctx->skip_end);
            ctx->skip_start = ctx->skip_end = 0;
            if (e == 1) continue;            /* whole frame trimmed away */
            return e < 0 ? -1 : 0;
        }
        if (rc == AVERROR(EAGAIN) && ctx->pending->data) {
            /* Room again: send the packet that was refused earlier. */
            int s = avcodec_send_packet(ctx->avctx, ctx->pending);
            if (s == AVERROR(EAGAIN)) return 1;
            av_packet_unref(ctx->pending);
            if (s < 0) return -1;
            continue;
        }
        if (rc != AVERROR(EAGAIN) && rc != AVERROR_EOF && ctx->kind == LUMEN_PROBE_VIDEO) hw_error(ctx, "decoding");
        return (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) ? 1 : -1;
    }
}

static int libav_decode(lumen_decoder_ctx_t *ctx, const lumen_packet_t *pkt, lumen_frame_t *out) {
    if (ctx->kind == LUMEN_PROBE_SUBTITLE) return decode_subtitle(ctx, pkt, out);

    ctx->avpkt->data = padded(ctx, pkt->data, pkt->size);
    if (!ctx->avpkt->data) return -1;
    ctx->avpkt->size = (int)pkt->size;
    /* Millisecond timestamps from the demuxer (v16 pts_ms), with
     * pkt_timebase = 1/1000 set at open, so frames come out in ms. */
    ctx->avpkt->pts = pkt->pts_ms >= 0 ? pkt->pts_ms : AV_NOPTS_VALUE;
    ctx->avpkt->dts = AV_NOPTS_VALUE;
    ctx->avpkt->flags = pkt->keyframe ? AV_PKT_FLAG_KEY : 0;
    /* Lead-in cut by an edit list: libavcodec decodes it (later frames
     * reference it) and drops the frames/samples it produces. */
    if (pkt->discard) ctx->avpkt->flags |= AV_PKT_FLAG_DISCARD;
    ctx->skip_start = pkt->skip_samples_start;
    ctx->skip_end = pkt->skip_samples_end;

    int send_rc = avcodec_send_packet(ctx->avctx, ctx->avpkt);
    if (send_rc == AVERROR(EAGAIN)) {
        /* Frames are waiting. Keep a (refcounted) copy of this packet --
         * our padded buffer is reused -- and resend it from receive(). */
        if (ctx->pending->data) av_packet_unref(ctx->pending);
        if (av_packet_ref(ctx->pending, ctx->avpkt) < 0) return -1;
    } else if (send_rc < 0) {
        if (ctx->kind == LUMEN_PROBE_VIDEO) hw_error(ctx, "decoding");
        return -1;
    }
    return libav_receive(ctx, out);
}

static void libav_set_skip(lumen_decoder_ctx_t *ctx, int level) {
    if (ctx->kind != LUMEN_PROBE_VIDEO) return;
    /* Read per frame by libavcodec, frame threads included. */
    ctx->avctx->skip_frame       = level >= 1 ? AVDISCARD_NONREF : AVDISCARD_DEFAULT;
    ctx->avctx->skip_loop_filter = level >= 2 ? AVDISCARD_ALL    : AVDISCARD_DEFAULT;
}

static int libav_drain(lumen_decoder_ctx_t *ctx, lumen_frame_t *out) {
    if (ctx->kind == LUMEN_PROBE_SUBTITLE) return 1;  /* subtitle decoders don't buffer */
    if (!ctx->flush_sent) {
        if (ctx->pending->data) {                     /* never lose a held packet */
            avcodec_send_packet(ctx->avctx, ctx->pending);
            av_packet_unref(ctx->pending);
        }
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
        video_frame_release(frame);
    } else if (frame->type == LUMEN_STREAM_SUBTITLE) {
        free(frame->subtitle.text);
        frame->subtitle.text = NULL;
        bitmap_image_free(frame->subtitle.image);     /* the core keeps its own copy */
        frame->subtitle.image = NULL;
    } else {
        free(frame->audio.data);
        frame->audio.data = NULL;
    }
}

static void libav_flush(lumen_decoder_ctx_t *ctx, int64_t resume_at_ms) {
    avcodec_flush_buffers(ctx->avctx);
    av_packet_unref(ctx->pending);
    ctx->skip_start = ctx->skip_end = 0;
    ctx->flush_sent = 0;
    (void)resume_at_ms;       /* real timestamps resume on their own now */
    ctx->last_pts_ms = -1;
    ctx->output_frame_counter = 0;
    ctx->samples_out = 0;
}

static void libav_close(lumen_decoder_ctx_t *ctx) {
    if (!ctx) return;
    if (ctx->swr) swr_free(&ctx->swr);
    if (ctx->sws) sws_freeContext(ctx->sws);
    av_frame_free(&ctx->swframe);
    av_buffer_unref(&ctx->hw_device);
    free(ctx->pad_buf);
    av_frame_free(&ctx->avframe);
    av_packet_free(&ctx->avpkt);
    av_packet_free(&ctx->pending);
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
    .receive = libav_receive,
    .set_skip = libav_set_skip,
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
