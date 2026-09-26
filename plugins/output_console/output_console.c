/*
 * output_console.c
 *
 * Prints a line per presented VIDEO frame instead of rendering pixels.
 * Kind is now "output-video" specifically (v2 ABI split video/audio
 * output into separate plugin kinds) -- see output_console_audio.c for
 * the audio-side counterpart.
 */
#include "../../include/lumen_plugin.h"
#include <stdlib.h>
#include <stdio.h>

struct lumen_output_ctx {
    int width, height;
    int frame_count;
};

static int co_open(lumen_output_ctx_t **out_ctx) {
    lumen_output_ctx_t *ctx = (lumen_output_ctx_t *)calloc(1, sizeof(*ctx));
    *out_ctx = ctx;
    printf("  [console-video] opened (idle, no file loaded yet)\n");
    return 0;
}

static int co_load_stream(lumen_output_ctx_t *ctx, int width, int height, int pixel_format) {
    (void)pixel_format;
    ctx->width = width;
    ctx->height = height;
    ctx->frame_count = 0;
    printf("  [console-video] loaded stream %dx%d\n", width, height);
    return 0;
}

static int co_present(lumen_output_ctx_t *ctx, const lumen_frame_t *frame) {
    ctx->frame_count++;
    uint8_t sample = frame->video.planes[0] ? frame->video.planes[0][0] : 0;
    unsigned long checksum = 0;
    if (frame->video.planes[0]) {
        for (int i = 0; i < frame->video.width * frame->video.height; i++) checksum += frame->video.planes[0][i];
        checksum %= 100000;
    }
    printf("  [console-video] frame #%d pts=%lld %dx%d sample_byte=0x%02x luma_checksum=%lu\n",
           ctx->frame_count, (long long)frame->pts, frame->video.width, frame->video.height, sample, checksum);
    return 0;
}

static void co_close(lumen_output_ctx_t *ctx) {
    printf("  [console-video] closed after %d frames\n", ctx->frame_count);
    free(ctx);
}

static const lumen_video_output_vtable_t VTABLE = {
    .open = co_open,
    .load_stream = co_load_stream,
    .present = co_present,
    .close = co_close,
};

static const lumen_plugin_descriptor_t DESCRIPTOR = {
    .abi_version = LUMEN_ABI_VERSION,
    .kind = LUMEN_PLUGIN_OUTPUT_VIDEO,
    .name = "console-video-output",
    .version = "0.2.0",
    .vtable = { .video_output = &VTABLE },
};

LUMEN_EXPORT const lumen_plugin_descriptor_t *lumen_get_plugin(void) {
    return &DESCRIPTOR;
}
