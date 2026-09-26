/*
 * decoder_passthrough_audio.c
 *
 * Claims the fake "SYNA" AUDIO codec, wraps incoming packet bytes as a
 * one-buffer audio frame -- no real decoding. This is the audio-side
 * counterpart to decoder_passthrough.c, proving that the core's
 * multi-stream routing correctly sends audio packets to an audio
 * decoder and routes the resulting frames to the audio output, entirely
 * separately from the video path running concurrently.
 */
#include "../../include/lumen_plugin.h"
#include <stdlib.h>
#include <string.h>

struct lumen_decoder_ctx {
    int sample_rate, channels;
};

static int pta_probe(const char *codec_fourcc) {
    return strncmp(codec_fourcc, "SYNA", 4) == 0;
}

static int pta_open(lumen_decoder_ctx_t **out_ctx, const lumen_stream_desc_t *stream) {
    lumen_decoder_ctx_t *ctx = (lumen_decoder_ctx_t *)calloc(1, sizeof(*ctx));
    ctx->sample_rate = stream->sample_rate;
    ctx->channels = stream->channels;
    *out_ctx = ctx;
    return 0;
}

static int pta_decode(lumen_decoder_ctx_t *ctx, const lumen_packet_t *pkt, lumen_frame_t *out_frame) {
    memset(out_frame, 0, sizeof(*out_frame));
    out_frame->type = LUMEN_STREAM_AUDIO;
    out_frame->pts = pkt->pts;
    out_frame->audio.data = (uint8_t *)malloc(pkt->size);
    memcpy(out_frame->audio.data, pkt->data, pkt->size);
    out_frame->audio.size = pkt->size;
    out_frame->audio.sample_rate = ctx->sample_rate;
    out_frame->audio.channels = ctx->channels;
    out_frame->audio.sample_fmt = LUMEN_SAMPLEFMT_S16;
    out_frame->audio.nb_samples = (int)(pkt->size / (sizeof(int16_t) * ctx->channels));
    return 0;
}

static void pta_frame_free(lumen_frame_t *frame) {
    free(frame->audio.data);
    frame->audio.data = NULL;
}

static int pta_drain(lumen_decoder_ctx_t *ctx, lumen_frame_t *out_frame) {
    (void)ctx; (void)out_frame;
    return 1;
}

static void pta_close(lumen_decoder_ctx_t *ctx) {
    free(ctx);
}

static const lumen_decoder_vtable_t VTABLE = {
    .probe = pta_probe,
    .open = pta_open,
    .decode = pta_decode,
    .frame_free = pta_frame_free,
    .drain = pta_drain,
    .close = pta_close,
};

static const lumen_plugin_descriptor_t DESCRIPTOR = {
    .abi_version = LUMEN_ABI_VERSION,
    .kind = LUMEN_PLUGIN_DECODER,
    .name = "passthrough-audio-decoder",
    .version = "0.1.0",
    .vtable = { .decoder = &VTABLE },
};

LUMEN_EXPORT const lumen_plugin_descriptor_t *lumen_get_plugin(void) {
    return &DESCRIPTOR;
}
