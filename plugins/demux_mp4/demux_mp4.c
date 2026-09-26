/*
 * demux_mp4.c
 *
 * A REAL container demuxer: thin wrapper around libavformat's
 * avformat_open_input/av_read_frame, the same "wrap, don't reimplement"
 * pattern as every other real plugin in this tree. This is the plugin
 * the whole multi-stream core upgrade was for -- a real MP4/MOV file
 * with genuinely interleaved H.264 video and AAC audio, demuxed and
 * routed to decoder_h264.c / decoder_aac.c exactly as they were already
 * validated against (raw streams) to handle.
 *
 * Two real container gotchas this plugin exists to handle correctly
 * (see lumen_plugin.h's extradata field and decoder_h264.c/decoder_aac.c):
 *   - MP4 stores H.264 as length-prefixed NAL units (AVCC) with SPS/PPS
 *     in the avcC box, NOT Annex-B start-code-prefixed NALs.
 *   - MP4 stores AAC as raw access units with audio config in the esds
 *     box, NOT per-frame ADTS headers.
 * Both are solved by copying AVStream's codecpar->extradata through to
 * the decoder via lumen_stream_desc_t.extradata -- libavcodec then
 * auto-detects the right framing convention on its own.
 */
#include "../../include/lumen_plugin.h"
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <stdlib.h>
#include <string.h>

struct lumen_demuxer_ctx {
    AVFormatContext *fmt_ctx;
    AVPacket        *avpkt; /* reused across read_packet() calls */
};

static int mp4_probe(const uint8_t *header_bytes, size_t header_len, const char *file_ext) {
    if (file_ext && (strcmp(file_ext, ".mp4") == 0 || strcmp(file_ext, ".m4v") == 0 ||
                      strcmp(file_ext, ".mov") == 0 || strcmp(file_ext, ".m4a") == 0)) {
        return 1;
    }
    /* MP4/MOV's box structure puts a 4-byte size then "ftyp" at offset 4 */
    return header_len >= 8 && memcmp(header_bytes + 4, "ftyp", 4) == 0;
}

/* Maps the handful of codecs this tree has decoder plugins for to the
 * fourcc strings those plugins' probe() functions match. Anything else
 * falls through to "UNKN", which correctly finds no installed decoder
 * and gets skipped -- see player_core.c's per-stream graceful
 * degradation, exercised for real the moment someone opens an MP4 with
 * a codec this tree doesn't have a plugin for yet (HEVC, AV1, ...). */
static void codec_id_to_fourcc(enum AVCodecID id, char *out, size_t out_size) {
    const char *s;
    switch (id) {
        case AV_CODEC_ID_H264: s = "H264"; break;
        case AV_CODEC_ID_HEVC: s = "HEVC"; break;
        case AV_CODEC_ID_AV1:  s = "AV01"; break;
        case AV_CODEC_ID_VP9:  s = "VP09"; break;
        case AV_CODEC_ID_AAC:  s = "AAC";  break;
        case AV_CODEC_ID_MP3:  s = "MP3";  break;
        case AV_CODEC_ID_OPUS: s = "OPUS"; break;
        case AV_CODEC_ID_VORBIS: s = "VORB"; break;
        case AV_CODEC_ID_AC3:  s = "AC3";  break;
        default: s = "UNKN"; break;
    }
    strncpy(out, s, out_size - 1);
    out[out_size - 1] = '\0';
}

static int mp4_open(lumen_demuxer_ctx_t **out_ctx, const char *path, lumen_stream_table_t *out_table) {
    AVFormatContext *fmt_ctx = NULL;
    if (avformat_open_input(&fmt_ctx, path, NULL, NULL) < 0) return -1;
    if (avformat_find_stream_info(fmt_ctx, NULL) < 0) {
        avformat_close_input(&fmt_ctx);
        return -1;
    }

    lumen_demuxer_ctx_t *ctx = (lumen_demuxer_ctx_t *)calloc(1, sizeof(*ctx));
    ctx->fmt_ctx = fmt_ctx;
    ctx->avpkt = av_packet_alloc();
    *out_ctx = ctx;

    memset(out_table, 0, sizeof(*out_table));
    int n = 0;
    for (unsigned i = 0; i < fmt_ctx->nb_streams; i++) {
        if (i >= LUMEN_MAX_STREAMS) break; /* can't represent more streams than our fixed table holds */

        AVStream *st = fmt_ctx->streams[i];
        AVCodecParameters *cp = st->codecpar;
        if (cp->codec_type != AVMEDIA_TYPE_VIDEO && cp->codec_type != AVMEDIA_TYPE_AUDIO) {
            continue; /* subtitle/data streams: not modeled in this ABI yet, correctly skipped */
        }

        lumen_stream_desc_t *sd = &out_table->streams[n++];
        sd->stream_index = (int)i; /* MUST match what av_read_frame reports per packet -- see read_packet() */
        sd->type = (cp->codec_type == AVMEDIA_TYPE_VIDEO) ? LUMEN_STREAM_VIDEO : LUMEN_STREAM_AUDIO;
        codec_id_to_fourcc(cp->codec_id, sd->codec_fourcc, sizeof(sd->codec_fourcc));

        if (sd->type == LUMEN_STREAM_VIDEO) {
            sd->width = cp->width;
            sd->height = cp->height;
            double fps = av_q2d(st->avg_frame_rate);
            if (fps <= 0.0 || fps > 1000.0) fps = av_q2d(st->r_frame_rate);
            sd->frame_rate = (fps > 0.0 && fps <= 1000.0) ? fps : 0.0;
        } else {
            sd->sample_rate = cp->sample_rate;
            sd->channels = cp->ch_layout.nb_channels;
        }

        /* The actual fix this plugin exists to apply: hand the
         * container's codec config (avcC/esds) through to whichever
         * decoder claims this fourcc. */
        sd->extradata = cp->extradata;
        sd->extradata_size = cp->extradata_size;
    }
    out_table->stream_count = n;
    out_table->duration_sec = (fmt_ctx->duration > 0) ? (double)fmt_ctx->duration / AV_TIME_BASE : 0.0;
    return 0;
}

static int mp4_read_packet(lumen_demuxer_ctx_t *ctx, lumen_packet_t *out_pkt) {
    while (1) {
        int rc = av_read_frame(ctx->fmt_ctx, ctx->avpkt);
        if (rc < 0) {
            av_packet_unref(ctx->avpkt);
            return 1; /* EOF (or an unrecoverable read error -- either way, stop cleanly) */
        }

        AVStream *st = ctx->fmt_ctx->streams[ctx->avpkt->stream_index];
        if ((st->codecpar->codec_type != AVMEDIA_TYPE_VIDEO && st->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) ||
            ctx->avpkt->stream_index >= LUMEN_MAX_STREAMS) {
            av_packet_unref(ctx->avpkt);
            continue; /* packet for a stream we didn't report (subtitle, or beyond our table) */
        }

        out_pkt->data = (uint8_t *)malloc(ctx->avpkt->size);
        memcpy(out_pkt->data, ctx->avpkt->data, ctx->avpkt->size);
        out_pkt->size = ctx->avpkt->size;
        /* Raw container pts in the stream's own time_base -- the
         * decoder plugins in this tree assign their own display-order
         * pts at emit time instead of trusting this (same reasoning as
         * demux_rawh264.c), so no time_base conversion happens here.
         * A real A/V sync implementation would need it; see README. */
        out_pkt->pts = ctx->avpkt->pts;
        out_pkt->stream_index = ctx->avpkt->stream_index;
        out_pkt->keyframe = (ctx->avpkt->flags & AV_PKT_FLAG_KEY) ? 1 : 0;

        if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            /* MP4 records AAC encoder delay (priming samples the
             * encoder needed for lookahead, which must be discarded
             * from decoded output, not from the compressed bitstream)
             * as side data on the packet -- typically only the very
             * first packet of the stream. Without reading this, the
             * first decoded frame contains samples that don't belong
             * in playback (confirmed: an exact one-frame offset
             * against ffmpeg's own reference decode before this
             * existed). Format per ffmpeg's avpacket.h: first 4 bytes
             * little-endian = samples to skip from the start. */
            size_t side_size = 0;
            uint8_t *side = av_packet_get_side_data(ctx->avpkt, AV_PKT_DATA_SKIP_SAMPLES, &side_size);
            if (side && side_size >= 4) {
                out_pkt->skip_samples_start = (int)(side[0] | (side[1] << 8) | (side[2] << 16) | (side[3] << 24));
            }
        }

        av_packet_unref(ctx->avpkt);
        return 0;
    }
}

static void mp4_packet_free(lumen_packet_t *pkt) {
    free(pkt->data);
    pkt->data = NULL;
}

static int mp4_seek(lumen_demuxer_ctx_t *ctx, int64_t target_ms) {
    /* av_seek_frame takes a timestamp in the TARGET STREAM's own
     * time_base, not a fixed unit -- av_rescale_q converts our plain
     * milliseconds into whatever units libavformat's default stream
     * (av_find_default_stream_index, normally the video stream) uses
     * internally. AVSEEK_FLAG_BACKWARD lands on the nearest keyframe
     * AT OR BEFORE the target rather than the nearest one after it --
     * the standard "seek to here or just before" behavior every real
     * player uses, since you can't decode forward from a keyframe
     * that's still in the future relative to where you asked to land. */
    int stream_idx = av_find_default_stream_index(ctx->fmt_ctx);
    if (stream_idx < 0) return -1;

    AVStream *st = ctx->fmt_ctx->streams[stream_idx];
    int64_t target_ts = av_rescale_q(target_ms, (AVRational){1, 1000}, st->time_base);

    int rc = av_seek_frame(ctx->fmt_ctx, stream_idx, target_ts, AVSEEK_FLAG_BACKWARD);
    if (rc < 0) return -1;

    av_packet_unref(ctx->avpkt); /* discard anything left over from before the seek */
    return 0;
}

static void mp4_close(lumen_demuxer_ctx_t *ctx) {
    if (!ctx) return;
    av_packet_free(&ctx->avpkt);
    avformat_close_input(&ctx->fmt_ctx);
    free(ctx);
}

static const lumen_demuxer_vtable_t VTABLE = {
    .probe = mp4_probe,
    .open = mp4_open,
    .read_packet = mp4_read_packet,
    .packet_free = mp4_packet_free,
    .seek = mp4_seek,
    .close = mp4_close,
};

static const lumen_plugin_descriptor_t DESCRIPTOR = {
    .abi_version = LUMEN_ABI_VERSION,
    .kind = LUMEN_PLUGIN_DEMUXER,
    .name = "mp4-demuxer",
    .version = "0.1.0 (libavformat wrapper)",
    .vtable = { .demuxer = &VTABLE },
};

LUMEN_EXPORT const lumen_plugin_descriptor_t *lumen_get_plugin(void) {
    return &DESCRIPTOR;
}
