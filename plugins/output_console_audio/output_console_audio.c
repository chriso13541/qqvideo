/*
 * output_console_audio.c
 *
 * Prints a line per presented AUDIO frame instead of playing sound --
 * stands in for a real output_wasapi.c / output_alsa.c, the same way
 * output_console.c stands in for a real video renderer.
 */
#include "../../include/lumen_plugin.h"
#include <stdlib.h>
#include <stdio.h>

struct lumen_output_ctx {
    int sample_rate, channels;
    int frame_count;
};

static int ca_open(lumen_output_ctx_t **out_ctx) {
    lumen_output_ctx_t *ctx = (lumen_output_ctx_t *)calloc(1, sizeof(*ctx));
    *out_ctx = ctx;
    printf("  [console-audio] opened (idle, no file loaded yet)\n");
    return 0;
}

static int ca_load_stream(lumen_output_ctx_t *ctx, int sample_rate, int channels, int sample_fmt) {
    (void)sample_fmt;
    ctx->sample_rate = sample_rate;
    ctx->channels = channels;
    ctx->frame_count = 0;
    printf("  [console-audio] loaded stream %dHz %dch\n", sample_rate, channels);
    return 0;
}

static int ca_present(lumen_output_ctx_t *ctx, const lumen_frame_t *frame) {
    ctx->frame_count++;
    uint8_t sample = frame->audio.data ? frame->audio.data[0] : 0;
    unsigned long checksum = 0;
    if (frame->audio.data) {
        for (size_t i = 0; i < frame->audio.size; i++) checksum += frame->audio.data[i];
        checksum %= 1000000;
    }
    printf("  [console-audio] frame #%d pts=%lld %d samples sample_byte=0x%02x checksum=%lu\n",
           ctx->frame_count, (long long)frame->pts, frame->audio.nb_samples, sample, checksum);
    return 0;
}

static void ca_close(lumen_output_ctx_t *ctx) {
    printf("  [console-audio] closed after %d frames\n", ctx->frame_count);
    free(ctx);
}

static const lumen_audio_output_vtable_t VTABLE = {
    .open = ca_open,
    .load_stream = ca_load_stream,
    .present = ca_present,
    .close = ca_close,
};

static const lumen_plugin_descriptor_t DESCRIPTOR = {
    .abi_version = LUMEN_ABI_VERSION,
    .kind = LUMEN_PLUGIN_OUTPUT_AUDIO,
    .name = "console-audio-output",
    .version = "0.1.0",
    .vtable = { .audio_output = &VTABLE },
};

LUMEN_EXPORT const lumen_plugin_descriptor_t *lumen_get_plugin(void) {
    return &DESCRIPTOR;
}
