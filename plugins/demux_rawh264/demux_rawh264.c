/*
 * demux_rawh264.c
 *
 * A REAL demuxer plugin (not synthetic this time): reads a raw Annex-B
 * H.264 elementary stream (the kind you get from
 * `ffmpeg -c:v copy -f h264 out.h264`) and splits it into one packet
 * per access unit (frame), using libavcodec's own AV_CODEC_ID_H264
 * parser to find frame boundaries -- the same mechanism ffmpeg's own
 * "h264" demuxer uses internally. This is deliberately NOT a hand-rolled
 * start-code scanner: NAL/AU boundary detection is genuinely fiddly
 * (emulation prevention bytes, multi-NAL access units, etc.), so this
 * plugin wraps the parser ffmpeg already ships rather than re-deriving
 * it -- same "wrap, don't reimplement" pattern as decoder_h264.c.
 *
 * Claims the ".h264" / ".264" extensions and always reports the video
 * codec as "H264", so it pairs with decoder_h264.c through the
 * registry exactly the way any container+codec pair would.
 */
#include "../../include/lumen_plugin.h"
#include <libavcodec/avcodec.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct lumen_demuxer_ctx {
    uint8_t *filedata;
    long     filesize;
    long     offset;
    int      flushed;
    int      frame_counter;
    AVCodecParserContext *parser;
    AVCodecContext       *dummy_avctx; /* parser needs *a* context; never opened/decoded with */
};

static int rawh264_probe(const uint8_t *header_bytes, size_t header_len, const char *file_ext) {
    if (file_ext && (strcmp(file_ext, ".h264") == 0 || strcmp(file_ext, ".264") == 0)) return 1;
    /* also sniff the Annex-B start code as a fallback */
    return header_len >= 4 && header_bytes[0] == 0 && header_bytes[1] == 0 &&
           (header_bytes[2] == 1 || (header_bytes[2] == 0 && header_bytes[3] == 1));
}

static int rawh264_open(lumen_demuxer_ctx_t **out_ctx, const char *path, lumen_stream_table_t *out_table) {
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
    ctx->offset = 0;
    ctx->parser = av_parser_init(AV_CODEC_ID_H264);
    if (!ctx->parser) { free(buf); free(ctx); return -1; }
    ctx->dummy_avctx = avcodec_alloc_context3(NULL);
    *out_ctx = ctx;

    memset(out_table, 0, sizeof(*out_table));
    out_table->stream_count = 1;
    lumen_stream_desc_t *v = &out_table->streams[0];
    v->stream_index = 0;
    v->type = LUMEN_STREAM_VIDEO;
    strncpy(v->codec_fourcc, "H264", sizeof(v->codec_fourcc) - 1);
    /* Real width/height live in the SPS inside the bitstream, which the
     * decoder plugin parses for us on the first decoded frame -- a raw
     * elementary stream demuxer doesn't need to duplicate that. */
    v->width = 0;
    v->height = 0;
    /* Raw Annex-B carries no container-level frame-rate metadata (SPS's
     * VUI parameters sometimes do, but parsing that is out of scope for
     * a validation-only demuxer). 0 = unknown; decoder_h264.c falls
     * back to a sane assumed default for pacing purposes. */
    v->frame_rate = 0.0;
    /* No extradata: Annex-B carries SPS/PPS in-band (as their own NAL
     * units in the bitstream), unlike MP4's avcC box. Leaving this
     * NULL/0 is correct, not an oversight -- see decoder_h264.c. */
    v->extradata = NULL;
    v->extradata_size = 0;
    return 0;
}

static int rawh264_read_packet(lumen_demuxer_ctx_t *ctx, lumen_packet_t *out_pkt) {
    while (1) {
        const uint8_t *in_buf = NULL;
        int in_size = 0;
        if (ctx->offset < ctx->filesize) {
            in_buf = ctx->filedata + ctx->offset;
            in_size = (int)(ctx->filesize - ctx->offset);
        } else if (!ctx->flushed) {
            ctx->flushed = 1; /* one final call with NULL flushes any buffered access unit */
        } else {
            return 1; /* truly done */
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
            /* This is BITSTREAM/CODING order, not presentation order --
             * raw Annex-B has no timestamps of its own. Decoder plugins
             * must not trust this as a display-order pts; see
             * decoder_h264.c's output_frame_counter for why. */
            out_pkt->pts = ctx->frame_counter++;
            out_pkt->stream_index = 0;
            out_pkt->keyframe = ctx->parser->key_frame == 1;
            return 0;
        }

        if (in_buf == NULL && out_size == 0) return 1; /* flush produced nothing more -- EOF */
    }
}

static void rawh264_packet_free(lumen_packet_t *pkt) {
    free(pkt->data);
    pkt->data = NULL;
}

static void rawh264_close(lumen_demuxer_ctx_t *ctx) {
    if (!ctx) return;
    if (ctx->parser) av_parser_close(ctx->parser);
    if (ctx->dummy_avctx) avcodec_free_context(&ctx->dummy_avctx);
    free(ctx->filedata);
    free(ctx);
}

static const lumen_demuxer_vtable_t VTABLE = {
    .probe = rawh264_probe,
    .open = rawh264_open,
    .read_packet = rawh264_read_packet,
    .packet_free = rawh264_packet_free,
    .close = rawh264_close,
};

static const lumen_plugin_descriptor_t DESCRIPTOR = {
    .abi_version = LUMEN_ABI_VERSION,
    .kind = LUMEN_PLUGIN_DEMUXER,
    .name = "rawh264-demuxer",
    .version = "0.2.0 (libavcodec parser wrapper)",
    .vtable = { .demuxer = &VTABLE },
};

LUMEN_EXPORT const lumen_plugin_descriptor_t *lumen_get_plugin(void) {
    return &DESCRIPTOR;
}
