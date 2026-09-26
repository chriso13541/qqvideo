/*
 * demux_rawaac.c
 *
 * Validation-only demuxer (same role as demux_rawh264.c was for H.264):
 * splits a raw ADTS AAC elementary stream (`ffmpeg -c:a aac -f adts
 * out.aac`) into per-frame packets using libavcodec's own AAC parser,
 * so decoder_aac.c can be tested in isolation before demux_mp4.c exists
 * to provide real container-sourced AAC streams.
 *
 * Sample rate and channel count are reported as 0 (unknown) in the
 * stream table -- ADTS headers carry that info per-frame in the
 * bitstream itself, not as separate container metadata, so there is
 * nothing meaningful to report until the decoder actually reads a
 * frame. A real MP4 demuxer, by contrast, CAN report these upfront
 * (they live in the esds box), and should.
 */
#include "../../include/lumen_plugin.h"
#include <libavcodec/avcodec.h>
#include <stdlib.h>
#include <string.h>

struct lumen_demuxer_ctx {
    uint8_t *filedata;
    long     filesize;
    long     offset;
    int      flushed;
    int      frame_counter;
    AVCodecParserContext *parser;
    AVCodecContext       *dummy_avctx;
};

static int rawaac_probe(const uint8_t *header_bytes, size_t header_len, const char *file_ext) {
    if (file_ext && (strcmp(file_ext, ".aac") == 0 || strcmp(file_ext, ".adts") == 0)) return 1;
    /* ADTS sync word: 12 bits of 1s at the start of every frame header */
    return header_len >= 2 && header_bytes[0] == 0xFF && (header_bytes[1] & 0xF0) == 0xF0;
}

static int rawaac_open(lumen_demuxer_ctx_t **out_ctx, const char *path, lumen_stream_table_t *out_table) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = (uint8_t *)malloc(size);
    if (fread(buf, 1, size, f) != (size_t)size) { fclose(f); free(buf); return -1; }
    fclose(f);

    lumen_demuxer_ctx_t *ctx = (lumen_demuxer_ctx_t *)calloc(1, sizeof(*ctx));
    ctx->filedata = buf;
    ctx->filesize = size;
    ctx->parser = av_parser_init(AV_CODEC_ID_AAC);
    if (!ctx->parser) { free(buf); free(ctx); return -1; }
    ctx->dummy_avctx = avcodec_alloc_context3(NULL);
    *out_ctx = ctx;

    memset(out_table, 0, sizeof(*out_table));
    out_table->stream_count = 1;
    lumen_stream_desc_t *a = &out_table->streams[0];
    a->stream_index = 0;
    a->type = LUMEN_STREAM_AUDIO;
    strncpy(a->codec_fourcc, "AAC", sizeof(a->codec_fourcc) - 1);
    a->sample_rate = 0; /* unknown until first ADTS frame header is parsed */
    a->channels = 0;
    /* No extradata: ADTS carries its config (sample rate, channels,
     * object type) per-frame in each frame's own header, unlike MP4's
     * esds box. NULL/0 is correct here -- see decoder_aac.c. */
    a->extradata = NULL;
    a->extradata_size = 0;
    return 0;
}

static int rawaac_read_packet(lumen_demuxer_ctx_t *ctx, lumen_packet_t *out_pkt) {
    while (1) {
        const uint8_t *in_buf = NULL;
        int in_size = 0;
        if (ctx->offset < ctx->filesize) {
            in_buf = ctx->filedata + ctx->offset;
            in_size = (int)(ctx->filesize - ctx->offset);
        } else if (!ctx->flushed) {
            ctx->flushed = 1;
        } else {
            return 1;
        }

        uint8_t *out_buf = NULL;
        int out_size = 0;
        int consumed = av_parser_parse2(ctx->parser, ctx->dummy_avctx, &out_buf, &out_size,
                                         in_buf, in_size, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
        ctx->offset += consumed;

        if (out_size > 0) {
            out_pkt->data = (uint8_t *)malloc(out_size);
            memcpy(out_pkt->data, out_buf, out_size);
            out_pkt->size = out_size;
            out_pkt->pts = ctx->frame_counter++; /* coding order; AAC has no B-frame-style reordering */
            out_pkt->stream_index = 0;
            out_pkt->keyframe = 1; /* every AAC frame decodes independently */
            return 0;
        }

        if (in_buf == NULL && out_size == 0) return 1;
    }
}

static void rawaac_packet_free(lumen_packet_t *pkt) {
    free(pkt->data);
    pkt->data = NULL;
}

static void rawaac_close(lumen_demuxer_ctx_t *ctx) {
    if (!ctx) return;
    if (ctx->parser) av_parser_close(ctx->parser);
    if (ctx->dummy_avctx) avcodec_free_context(&ctx->dummy_avctx);
    free(ctx->filedata);
    free(ctx);
}

static const lumen_demuxer_vtable_t VTABLE = {
    .probe = rawaac_probe,
    .open = rawaac_open,
    .read_packet = rawaac_read_packet,
    .packet_free = rawaac_packet_free,
    .close = rawaac_close,
};

static const lumen_plugin_descriptor_t DESCRIPTOR = {
    .abi_version = LUMEN_ABI_VERSION,
    .kind = LUMEN_PLUGIN_DEMUXER,
    .name = "rawaac-demuxer",
    .version = "0.1.0 (validation-only, libavcodec parser wrapper)",
    .vtable = { .demuxer = &VTABLE },
};

LUMEN_EXPORT const lumen_plugin_descriptor_t *lumen_get_plugin(void) {
    return &DESCRIPTOR;
}
