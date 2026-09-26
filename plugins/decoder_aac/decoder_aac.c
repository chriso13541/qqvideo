/*
 * decoder_aac.c
 *
 * A REAL decoder plugin: thin wrapper around libavcodec's AAC decoder,
 * same pattern as decoder_h264.c. The one new wrinkle audio brings: the
 * AAC decoder's native output is commonly planar float
 * (AV_SAMPLE_FMT_FLTP -- each channel in its own buffer, float samples),
 * but lumen_frame_t's audio union wants a single interleaved buffer in
 * a fixed format (we standardize on signed 16-bit interleaved, the
 * format basically every audio output API -- WASAPI, ALSA, CoreAudio --
 * accepts directly without further conversion).
 *
 * Converting planar-float to interleaved-int16 by hand is a classic
 * source of subtle bugs (clipping, rounding, channel-order mistakes).
 * Per the same "wrap, don't reimplement" principle as the codec itself:
 * this uses libswresample (ffmpeg's own format/resample converter,
 * libswresample/swresample.h) to do the conversion, rather than writing
 * float-to-int16 math by hand.
 */
#include "../../include/lumen_plugin.h"
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct lumen_decoder_ctx {
    AVCodecContext *avctx;
    AVPacket       *avpkt;
    AVFrame        *avframe;
    SwrContext     *swr;
    int             channels;
    int             flush_sent;
    int             output_frame_counter; /* same display-order reasoning as decoder_h264.c */
};

static int aac_probe(const char *codec_fourcc) {
    /* tolerate "AAC", "AAC ", "MP4A" -- containers spell this differently */
    return strncmp(codec_fourcc, "AAC", 3) == 0 || strncmp(codec_fourcc, "MP4A", 4) == 0;
}

static int aac_open(lumen_decoder_ctx_t **out_ctx, const lumen_stream_desc_t *stream) {
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_AAC);
    if (!codec) return -1;

    lumen_decoder_ctx_t *ctx = (lumen_decoder_ctx_t *)calloc(1, sizeof(*ctx));
    /* `channels` may legitimately be 0 here: a raw ADTS elementary
     * stream demuxer can't know channel count until it's actually
     * decoded the first frame's header, unlike a real container
     * (MP4's esds box, for instance) which states it upfront. Treat 0
     * as "unknown, resolve from the first decoded frame" rather than
     * guessing -- see convert_and_fill(). */
    ctx->channels = stream->channels;

    ctx->avctx = avcodec_alloc_context3(codec);
    if (!ctx->avctx) { free(ctx); return -1; }
    ctx->avctx->sample_rate = stream->sample_rate; /* 0 is fine; AAC is self-describing per ADTS frame */
    av_channel_layout_default(&ctx->avctx->ch_layout, ctx->channels > 0 ? ctx->channels : 2);

    /* MP4 stores AAC as raw access units (no per-frame ADTS header) plus
     * an AudioSpecificConfig in the esds box, given to us here as
     * extradata -- without it, the decoder has no way to know sample
     * rate/channels/object type for a raw stream. demux_rawaac's ADTS
     * frames carry that same info per-frame instead, so extradata stays
     * NULL there and this block is simply skipped. */
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
    /* swr context is allocated lazily on first decoded frame, once we
     * know the decoder's actual native sample format -- AAC can decode
     * to FLTP, S16P, etc. depending on profile, and we shouldn't assume. */
    ctx->swr = NULL;
    *out_ctx = ctx;
    return 0;
}

/* Converts one decoded AVFrame (whatever planar/packed format the AAC
 * decoder produced) into a freshly malloc'd interleaved S16 buffer via
 * libswresample, and fills out the lumen_frame_t audio union. */
static int convert_and_fill(lumen_decoder_ctx_t *ctx, const AVFrame *avf, lumen_frame_t *out) {
    if (ctx->channels <= 0) ctx->channels = avf->ch_layout.nb_channels;

    if (!ctx->swr) {
        SwrContext *swr = NULL;
        AVChannelLayout out_layout;
        av_channel_layout_default(&out_layout, ctx->channels);
        int rc = swr_alloc_set_opts2(&swr,
            &out_layout, AV_SAMPLE_FMT_S16, avf->sample_rate,
            &avf->ch_layout, (enum AVSampleFormat)avf->format, avf->sample_rate,
            0, NULL);
        av_channel_layout_uninit(&out_layout);
        if (rc < 0 || !swr || swr_init(swr) < 0) {
            if (swr) swr_free(&swr);
            return -1;
        }
        ctx->swr = swr;
    }

    int max_out_samples = avf->nb_samples + 16; /* small slack for resampler latency */
    int out_buf_size = max_out_samples * ctx->channels * (int)sizeof(int16_t);
    uint8_t *out_buf = (uint8_t *)malloc(out_buf_size);

    int converted = swr_convert(ctx->swr, &out_buf, max_out_samples,
                                 (const uint8_t **)avf->data, avf->nb_samples);
    if (converted < 0) {
        free(out_buf);
        return -1;
    }

    memset(out, 0, sizeof(*out));
    out->type = LUMEN_STREAM_AUDIO;
    out->audio.data = out_buf;
    out->audio.size = (size_t)converted * ctx->channels * sizeof(int16_t);
    out->audio.sample_rate = avf->sample_rate;
    out->audio.channels = ctx->channels;
    out->audio.sample_fmt = LUMEN_SAMPLEFMT_S16;
    out->audio.nb_samples = converted;
    return 0;
}

static int aac_decode(lumen_decoder_ctx_t *ctx, const lumen_packet_t *pkt, lumen_frame_t *out_frame) {
    ctx->avpkt->data = pkt->data;
    ctx->avpkt->size = (int)pkt->size;

    int send_rc = avcodec_send_packet(ctx->avctx, ctx->avpkt);
    if (send_rc < 0 && send_rc != AVERROR(EAGAIN)) return -1;

    int recv_rc = avcodec_receive_frame(ctx->avctx, ctx->avframe);
    if (recv_rc == AVERROR(EAGAIN) || recv_rc == AVERROR_EOF) {
        return 1; /* needs more input -- normal, not every packet yields a frame immediately */
    }
    if (recv_rc < 0) return -1;

    int crc = convert_and_fill(ctx, ctx->avframe, out_frame);
    av_frame_unref(ctx->avframe);
    if (crc < 0) return -1;

    if (pkt->skip_samples_start > 0) {
        int skip = pkt->skip_samples_start;
        int bytes_per_sample_set = ctx->channels * (int)sizeof(int16_t);
        size_t skip_bytes = (size_t)skip * bytes_per_sample_set;
        if (skip_bytes >= out_frame->audio.size) {
            /* The whole frame was priming -- nothing real to present
             * from this packet. Free what convert_and_fill allocated
             * and tell the caller "no frame this round" rather than
             * routing an empty buffer downstream. */
            free(out_frame->audio.data);
            out_frame->audio.data = NULL;
            return 1;
        }
        memmove(out_frame->audio.data, out_frame->audio.data + skip_bytes, out_frame->audio.size - skip_bytes);
        out_frame->audio.size -= skip_bytes;
        out_frame->audio.nb_samples -= skip;
    }

    out_frame->pts = ctx->output_frame_counter++;
    return 0;
}

static void aac_frame_free(lumen_frame_t *frame) {
    free(frame->audio.data);
    frame->audio.data = NULL;
}

/* AAC decoding is far less prone to multi-frame buffering than H.264's
 * B-frames, but avcodec_send_packet(ctx, NULL) can still surface a final
 * frame depending on the specific AAC profile/SBR state -- draining
 * costs nothing when there's nothing buffered (receive_frame just
 * returns AVERROR_EOF immediately), so it's cheap correctness insurance
 * rather than something to skip "because AAC probably doesn't need it." */
static int aac_drain(lumen_decoder_ctx_t *ctx, lumen_frame_t *out_frame) {
    if (!ctx->flush_sent) {
        avcodec_send_packet(ctx->avctx, NULL);
        ctx->flush_sent = 1;
    }
    int recv_rc = avcodec_receive_frame(ctx->avctx, ctx->avframe);
    if (recv_rc < 0) return 1;

    int crc = convert_and_fill(ctx, ctx->avframe, out_frame);
    av_frame_unref(ctx->avframe);
    if (crc < 0) return 1;

    out_frame->pts = ctx->output_frame_counter++;
    return 0;
}

static void aac_flush(lumen_decoder_ctx_t *ctx, int64_t resume_at_ms) {
    avcodec_flush_buffers(ctx->avctx);
    ctx->flush_sent = 0;
    /* Unlike decoder_h264.c, AAC's output_frame_counter isn't currently
     * read by anything for position tracking (route_frame() in
     * player_core.c only reads position from VIDEO frames) -- resetting
     * it to 0 is harmless for now. Accepting resume_at_ms anyway keeps
     * the signature consistent and ready if that ever changes (e.g.
     * an audio-only file using audio for position display). */
    (void)resume_at_ms;
    ctx->output_frame_counter = 0;
}

static void aac_close(lumen_decoder_ctx_t *ctx) {
    if (!ctx) return;
    if (ctx->swr) swr_free(&ctx->swr);
    av_frame_free(&ctx->avframe);
    av_packet_free(&ctx->avpkt);
    avcodec_free_context(&ctx->avctx);
    free(ctx);
}

static const lumen_decoder_vtable_t VTABLE = {
    .probe = aac_probe,
    .open = aac_open,
    .decode = aac_decode,
    .frame_free = aac_frame_free,
    .drain = aac_drain,
    .flush = aac_flush,
    .close = aac_close,
};

static const lumen_plugin_descriptor_t DESCRIPTOR = {
    .abi_version = LUMEN_ABI_VERSION,
    .kind = LUMEN_PLUGIN_DECODER,
    .name = "aac-decoder",
    .version = "0.2.0 (libavcodec + libswresample wrapper, encoder-delay aware)",
    .vtable = { .decoder = &VTABLE },
};

LUMEN_EXPORT const lumen_plugin_descriptor_t *lumen_get_plugin(void) {
    return &DESCRIPTOR;
}
