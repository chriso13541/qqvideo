/*
 * demux_synthetic.c
 *
 * A demuxer plugin that manufactures fake packets instead of parsing a
 * real container -- now extended to emit TWO interleaved streams (a fake
 * "SYNT" video stream and a fake "SYNA" audio stream), to exercise the
 * core's multi-stream routing the same way a real MP4 (H264 + AAC) would,
 * without needing a real container parser yet.
 */
#include "../../include/lumen_plugin.h"
#include <stdlib.h>
#include <string.h>

#define TOTAL_TICKS 10  /* each tick emits one video packet + one audio packet */

struct lumen_demuxer_ctx {
    int tick;             /* which tick (0..TOTAL_TICKS-1) we're on */
    int emitted_video_this_tick;
};

static int synth_probe(const uint8_t *header_bytes, size_t header_len, const char *file_ext) {
    (void)header_bytes; (void)header_len;
    return file_ext && strcmp(file_ext, ".synt") == 0;
}

static int synth_open(lumen_demuxer_ctx_t **out_ctx, const char *path, lumen_stream_table_t *out_table) {
    (void)path;
    lumen_demuxer_ctx_t *ctx = (lumen_demuxer_ctx_t *)calloc(1, sizeof(*ctx));
    *out_ctx = ctx;

    memset(out_table, 0, sizeof(*out_table));
    out_table->stream_count = 2;
    out_table->duration_sec = TOTAL_TICKS * 0.1;

    lumen_stream_desc_t *v = &out_table->streams[0];
    v->stream_index = 0;
    v->type = LUMEN_STREAM_VIDEO;
    strncpy(v->codec_fourcc, "SYNT", sizeof(v->codec_fourcc) - 1);
    v->width = 64;
    v->height = 64;
    v->frame_rate = 10.0; /* matches TOTAL_TICKS=10 over duration_sec=1.0 below */

    lumen_stream_desc_t *a = &out_table->streams[1];
    a->stream_index = 1;
    a->type = LUMEN_STREAM_AUDIO;
    strncpy(a->codec_fourcc, "SYNA", sizeof(a->codec_fourcc) - 1);
    a->sample_rate = 48000;
    a->channels = 2;

    return 0;
}

static int synth_read_packet(lumen_demuxer_ctx_t *ctx, lumen_packet_t *out_pkt) {
    if (ctx->tick >= TOTAL_TICKS) return 1; /* EOF */

    if (!ctx->emitted_video_this_tick) {
        size_t size = 64 * 64;
        uint8_t *data = (uint8_t *)malloc(size);
        memset(data, (ctx->tick * 23) & 0xFF, size);
        out_pkt->data = data;
        out_pkt->size = size;
        out_pkt->pts = ctx->tick;
        out_pkt->stream_index = 0; /* video */
        out_pkt->keyframe = (ctx->tick == 0);
        ctx->emitted_video_this_tick = 1;
        return 0;
    } else {
        size_t size = 480; /* 10ms of fake stereo s16 audio at 48kHz: 480 samples/channel */
        uint8_t *data = (uint8_t *)malloc(size);
        memset(data, (ctx->tick * 17) & 0xFF, size);
        out_pkt->data = data;
        out_pkt->size = size;
        out_pkt->pts = ctx->tick;
        out_pkt->stream_index = 1; /* audio */
        out_pkt->keyframe = 0;
        ctx->emitted_video_this_tick = 0;
        ctx->tick++;
        return 0;
    }
}

static void synth_packet_free(lumen_packet_t *pkt) {
    free(pkt->data);
    pkt->data = NULL;
}

static void synth_close(lumen_demuxer_ctx_t *ctx) {
    free(ctx);
}

static const lumen_demuxer_vtable_t VTABLE = {
    .probe = synth_probe,
    .open = synth_open,
    .read_packet = synth_read_packet,
    .packet_free = synth_packet_free,
    .close = synth_close,
};

static const lumen_plugin_descriptor_t DESCRIPTOR = {
    .abi_version = LUMEN_ABI_VERSION,
    .kind = LUMEN_PLUGIN_DEMUXER,
    .name = "synthetic-demuxer",
    .version = "0.2.0 (multi-stream)",
    .vtable = { .demuxer = &VTABLE },
};

LUMEN_EXPORT const lumen_plugin_descriptor_t *lumen_get_plugin(void) {
    return &DESCRIPTOR;
}
