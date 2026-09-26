/*
 * decoder_passthrough.c
 *
 * Claims the fake "SYNT" VIDEO codec and just wraps incoming packet
 * bytes as a one-plane video frame -- no real decoding. Proves the
 * decode stage of the pipeline. See plugins/decoder_h264 for a REAL
 * decoder (wrapping libavcodec).
 */
#include "../../include/lumen_plugin.h"
#include <stdlib.h>
#include <string.h>

struct lumen_decoder_ctx {
    int width, height;
};

static int pt_probe(const char *codec_fourcc) {
    return strncmp(codec_fourcc, "SYNT", 4) == 0;
}

static int pt_open(lumen_decoder_ctx_t **out_ctx, const lumen_stream_desc_t *stream) {
    lumen_decoder_ctx_t *ctx = (lumen_decoder_ctx_t *)calloc(1, sizeof(*ctx));
    ctx->width = stream->width;
    ctx->height = stream->height;
    *out_ctx = ctx;
    return 0;
}

static int pt_decode(lumen_decoder_ctx_t *ctx, const lumen_packet_t *pkt, lumen_frame_t *out_frame) {
    memset(out_frame, 0, sizeof(*out_frame));
    out_frame->type = LUMEN_STREAM_VIDEO;
    out_frame->pts = pkt->pts;
    out_frame->video.planes[0] = (uint8_t *)malloc(pkt->size);
    memcpy(out_frame->video.planes[0], pkt->data, pkt->size);
    out_frame->video.stride[0] = ctx->width;
    out_frame->video.width = ctx->width;
    out_frame->video.height = ctx->height;
    out_frame->video.pixel_format = LUMEN_PIXFMT_YUV420P;
    return 0;
}

static void pt_frame_free(lumen_frame_t *frame) {
    free(frame->video.planes[0]);
    frame->video.planes[0] = NULL;
}

static int pt_drain(lumen_decoder_ctx_t *ctx, lumen_frame_t *out_frame) {
    (void)ctx; (void)out_frame;
    return 1; /* nothing ever buffered */
}

static void pt_close(lumen_decoder_ctx_t *ctx) {
    free(ctx);
}

static const lumen_decoder_vtable_t VTABLE = {
    .probe = pt_probe,
    .open = pt_open,
    .decode = pt_decode,
    .frame_free = pt_frame_free,
    .drain = pt_drain,
    .close = pt_close,
};

static const lumen_plugin_descriptor_t DESCRIPTOR = {
    .abi_version = LUMEN_ABI_VERSION,
    .kind = LUMEN_PLUGIN_DECODER,
    .name = "passthrough-decoder",
    .version = "0.2.0",
    .vtable = { .decoder = &VTABLE },
};

LUMEN_EXPORT const lumen_plugin_descriptor_t *lumen_get_plugin(void) {
    return &DESCRIPTOR;
}
