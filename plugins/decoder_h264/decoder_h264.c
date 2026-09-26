/*
 * decoder_h264.c
 *
 * A REAL decoder plugin: thin wrapper around libavcodec's H.264 decoder.
 * This is the pattern discussed for every other codec you'd want to
 * support -- pull just the one decoder you need out of ffmpeg, wrap it
 * in the lumen_decoder_vtable_t shape, and ship it as its own small
 * shared library. The security property that matters: if you never
 * install this .so, libavcodec's H.264 code (and any future CVE in it)
 * is never mapped into the lumen-play process at all.
 *
 * In production you'd build a private, minimal libavcodec with only
 * `--enable-decoder=h264` so this plugin is a few hundred KB instead of
 * linking the whole codec library -- this sandbox links against the
 * distro's full libavcodec just to prove the wrapper logic works.
 */
#include "../../include/lumen_plugin.h"
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct lumen_decoder_ctx {
    AVCodecContext *avctx;
    AVPacket       *avpkt;
    AVFrame        *avframe;
    int             flush_sent;
    int             output_frame_counter; /* display-order frame index, assigned at EMIT time */
    double          frame_duration_ms;    /* real time between frames, for output-side pacing */
};

static int h264_probe(const char *codec_fourcc) {
    return strncmp(codec_fourcc, "H264", 4) == 0 || strncmp(codec_fourcc, "AVC1", 4) == 0;
}

static int h264_open(lumen_decoder_ctx_t **out_ctx, const lumen_stream_desc_t *stream) {
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) return -1;

    lumen_decoder_ctx_t *ctx = (lumen_decoder_ctx_t *)calloc(1, sizeof(*ctx));

    /* Real frame rate when the container provided one (MP4 always
     * does); otherwise assume 30fps rather than refuse to pace at all --
     * an approximate pacing is far better than blasting through the
     * whole file in under a second, which is what NO pacing would do
     * on hardware this fast. */
    double fps = (stream->frame_rate > 0.0) ? stream->frame_rate : 30.0;
    ctx->frame_duration_ms = 1000.0 / fps;

    ctx->avctx = avcodec_alloc_context3(codec);
    if (!ctx->avctx) { free(ctx); return -1; }

    /* If the demuxer gave us extradata (MP4's avcC box: SPS/PPS plus a
     * version byte that starts with 0x01), libavcodec auto-detects AVCC
     * mode -- length-prefixed NAL units, no start codes -- and switches
     * its parsing accordingly. Without extradata (demux_rawh264's case),
     * it stays in Annex-B mode, start-code-prefixed, exactly as already
     * validated. Same decoder, both container conventions, no plugin-side
     * branching needed -- libavcodec handles the distinction internally. */
    if (stream->extradata && stream->extradata_size > 0) {
        ctx->avctx->extradata = (uint8_t *)av_malloc(stream->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
        memcpy(ctx->avctx->extradata, stream->extradata, stream->extradata_size);
        memset(ctx->avctx->extradata + stream->extradata_size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
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

/* Copies one AVFrame's planes into freshly malloc'd, tightly-packed
 * buffers so the frame outlives ffmpeg's internal buffer pool and the
 * core's generic frame_free() doesn't need to know anything about
 * AVFrame reference counting. */
static void copy_avframe_to_lumen_frame(const AVFrame *avf, lumen_frame_t *out) {
    memset(out, 0, sizeof(*out));
    out->type = LUMEN_STREAM_VIDEO;
    out->video.width = avf->width;
    out->video.height = avf->height;
    out->video.pixel_format = LUMEN_PIXFMT_YUV420P;
    /* NOTE: out->pts is deliberately NOT set here -- see callers. A raw
     * Annex-B elementary stream carries no real presentation timestamps
     * (that metadata normally lives in the container, e.g. MP4's moov
     * atom), so there is nothing meaningful to read off avf->pts here.
     * The correct display-order index is the order frames are actually
     * emitted from decode()/drain(), which the caller already knows. */

    int plane_heights[3] = { avf->height, avf->height / 2, avf->height / 2 };
    for (int p = 0; p < 3; p++) {
        int w = (p == 0) ? avf->width : (avf->width + 1) / 2;
        int h = plane_heights[p];
        out->video.planes[p] = (uint8_t *)malloc((size_t)w * h);
        out->video.stride[p] = w;
        for (int row = 0; row < h; row++) {
            memcpy(out->video.planes[p] + row * w,
                   avf->data[p] + row * avf->linesize[p],
                   w);
        }
    }
}

static int h264_decode(lumen_decoder_ctx_t *ctx, const lumen_packet_t *pkt, lumen_frame_t *out_frame) {
    ctx->avpkt->data = pkt->data;
    ctx->avpkt->size = (int)pkt->size;
    ctx->avpkt->pts = pkt->pts;

    int send_rc = avcodec_send_packet(ctx->avctx, ctx->avpkt);
    if (send_rc < 0 && send_rc != AVERROR(EAGAIN)) return -1;

    int recv_rc = avcodec_receive_frame(ctx->avctx, ctx->avframe);
    if (recv_rc == AVERROR(EAGAIN) || recv_rc == AVERROR_EOF) {
        return 1; /* no frame yet -- normal during B-frame reordering / startup */
    }
    if (recv_rc < 0) return -1;

    copy_avframe_to_lumen_frame(ctx->avframe, out_frame);
    out_frame->pts = (int64_t)(ctx->output_frame_counter * ctx->frame_duration_ms + 0.5);
    ctx->output_frame_counter++;
    av_frame_unref(ctx->avframe);
    return 0;
}

static void h264_frame_free(lumen_frame_t *frame) {
    for (int p = 0; p < 4; p++) {
        free(frame->video.planes[p]);
        frame->video.planes[p] = NULL;
    }
}

/* Flush libavcodec's internal reorder buffer. Must be called repeatedly
 * after the demuxer reports EOF -- B-frames mean there can be 1-2 frames
 * still held inside avctx that were never returned to a normal decode()
 * call. Without this, the last frames of any B-frame-using stream are
 * silently dropped (confirmed: an 8-frame result from a real 10-frame
 * libx264 stream before this existed). */
static int h264_drain(lumen_decoder_ctx_t *ctx, lumen_frame_t *out_frame) {
    if (!ctx->flush_sent) {
        avcodec_send_packet(ctx->avctx, NULL);
        ctx->flush_sent = 1;
    }
    int recv_rc = avcodec_receive_frame(ctx->avctx, ctx->avframe);
    if (recv_rc < 0) return 1; /* AVERROR_EOF -- nothing left buffered */

    copy_avframe_to_lumen_frame(ctx->avframe, out_frame);
    out_frame->pts = (int64_t)(ctx->output_frame_counter * ctx->frame_duration_ms + 0.5);
    ctx->output_frame_counter++;
    av_frame_unref(ctx->avframe);
    return 0;
}

static void h264_flush(lumen_decoder_ctx_t *ctx, int64_t resume_at_ms) {
    avcodec_flush_buffers(ctx->avctx);
    ctx->flush_sent = 0; /* a future drain() after EOF needs to send its own flush again */
    /* Reseed (don't zero!) the frame counter so the NEXT emitted frame's
     * pts continues from roughly where we just seeked to, not from 0.
     * Zeroing here was the original (wrong) approach -- it made pacing
     * resync correctly but broke position tracking, since position_ms
     * is read directly from this same pts. */
    ctx->output_frame_counter = (int)(resume_at_ms / ctx->frame_duration_ms);
}

static void h264_close(lumen_decoder_ctx_t *ctx) {
    if (!ctx) return;
    av_frame_free(&ctx->avframe);
    av_packet_free(&ctx->avpkt);
    avcodec_free_context(&ctx->avctx);
    free(ctx);
}

static const lumen_decoder_vtable_t VTABLE = {
    .probe = h264_probe,
    .open = h264_open,
    .decode = h264_decode,
    .frame_free = h264_frame_free,
    .drain = h264_drain,
    .flush = h264_flush,
    .close = h264_close,
};

static const lumen_plugin_descriptor_t DESCRIPTOR = {
    .abi_version = LUMEN_ABI_VERSION,
    .kind = LUMEN_PLUGIN_DECODER,
    .name = "h264-decoder",
    .version = "0.2.0 (libavcodec wrapper)",
    .vtable = { .decoder = &VTABLE },
};

LUMEN_EXPORT const lumen_plugin_descriptor_t *lumen_get_plugin(void) {
    return &DESCRIPTOR;
}
