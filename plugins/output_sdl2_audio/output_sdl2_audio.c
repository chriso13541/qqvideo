/*
 * output_sdl2_audio.c
 *
 * Restructured for the persistent-window architecture, same split as
 * output_sdl2.cpp: open() now only initializes the SDL audio
 * subsystem -- no device is opened yet, since no file's sample
 * rate/channels are known at app startup. load_stream() opens (or, if
 * switching from a file with different parameters, closes and
 * reopens) the actual device. SDL doesn't support changing format on
 * an already-open device, so a format change between files means a
 * real close+reopen under the hood -- but the plugin object/ctx stays
 * alive across that, same as video's load_stream.
 *
 * Pause-silencing and seek-flush behavior (pump_ui calling
 * SDL_PauseAudioDevice/SDL_ClearQueuedAudio) and volume scaling are
 * unchanged from before this restructuring.
 */
#include "../../include/lumen_plugin.h"
#include <SDL2/SDL.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

struct lumen_output_ctx {
    SDL_AudioDeviceID device; /* 0 until load_stream() opens a real device */
    lumen_playback_state_t *state;
    int device_paused;
    int was_muted;              /* tracks last-seen mute state to detect transitions */
    int64_t last_seen_seek_generation;
};

static int sdl2_audio_open(lumen_output_ctx_t **out_ctx) {
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "lumen: SDL_InitSubSystem(AUDIO) failed: %s\n", SDL_GetError());
        return -1;
    }
    lumen_output_ctx_t *ctx = (lumen_output_ctx_t *)calloc(1, sizeof(*ctx));
    *out_ctx = ctx;
    printf("  [sdl2-audio] opened (idle, no file loaded yet)\n");
    return 0;
}

static int sdl2_audio_load_stream(lumen_output_ctx_t *ctx, int sample_rate, int channels, int sample_fmt) {
    (void)sample_fmt;

    if (ctx->device) {
        SDL_CloseAudioDevice(ctx->device);
        ctx->device = 0;
    }

    SDL_AudioSpec wanted, obtained;
    SDL_zero(wanted);
    wanted.freq = sample_rate > 0 ? sample_rate : 44100;
    wanted.format = AUDIO_S16SYS;
    wanted.channels = (Uint8)(channels > 0 ? channels : 2);
    wanted.samples = 4096;

    SDL_AudioDeviceID dev = SDL_OpenAudioDevice(NULL, 0, &wanted, &obtained, 0);
    if (dev == 0) {
        fprintf(stderr, "lumen: SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        return -1;
    }
    ctx->device = dev;
    ctx->device_paused = 0;
    SDL_PauseAudioDevice(dev, 0);
    printf("  [sdl2-audio] loaded stream %dHz %dch\n", obtained.freq, obtained.channels);
    return 0;
}

static void sdl2_audio_bind_state(lumen_output_ctx_t *ctx, lumen_playback_state_t *state) {
    ctx->state = state;
}

static int sdl2_audio_pump_ui(lumen_output_ctx_t *ctx, lumen_playback_state_t *state) {
    if (!state || !ctx->device) return 0;

    if (state->paused != ctx->device_paused) {
        SDL_PauseAudioDevice(ctx->device, state->paused ? 1 : 0);
        if (state->paused) {
            SDL_ClearQueuedAudio(ctx->device);
        }
        ctx->device_paused = state->paused;
    }

    /* Clear stale pre-mute audio for instant silence when muting */
    if (state->muted != ctx->was_muted) {
        if (state->muted) SDL_ClearQueuedAudio(ctx->device);
        ctx->was_muted = state->muted;
    }

    if (state->seek_generation != ctx->last_seen_seek_generation) {
        SDL_ClearQueuedAudio(ctx->device);
        ctx->last_seen_seek_generation = state->seek_generation;
    }

    return 0;
}

static int sdl2_audio_present(lumen_output_ctx_t *ctx, const lumen_frame_t *frame) {
    if (!ctx->device || !frame->audio.data || frame->audio.size == 0) return 0;
    if (ctx->state && ctx->state->muted) return 0; /* muted -- pump_ui already cleared stale audio */

    float volume = ctx->state ? ctx->state->volume : 1.0f;
    if (volume >= 0.999f) {
        SDL_QueueAudio(ctx->device, frame->audio.data, (Uint32)frame->audio.size);
    } else {
        size_t n_samples = frame->audio.size / sizeof(int16_t);
        int16_t *scaled = (int16_t *)malloc(frame->audio.size);
        const int16_t *src = (const int16_t *)frame->audio.data;
        for (size_t i = 0; i < n_samples; i++) {
            scaled[i] = (int16_t)((float)src[i] * volume);
        }
        SDL_QueueAudio(ctx->device, scaled, (Uint32)frame->audio.size);
        free(scaled);
    }
    return 0;
}

static void sdl2_audio_close(lumen_output_ctx_t *ctx) {
    if (!ctx) return;
    if (ctx->device) SDL_CloseAudioDevice(ctx->device);
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    free(ctx);
}

static const lumen_audio_output_vtable_t VTABLE = {
    .open = sdl2_audio_open,
    .load_stream = sdl2_audio_load_stream,
    .bind_state = sdl2_audio_bind_state,
    .pump_ui = sdl2_audio_pump_ui,
    .present = sdl2_audio_present,
    .close = sdl2_audio_close,
};

static const lumen_plugin_descriptor_t DESCRIPTOR = {
    .abi_version = LUMEN_ABI_VERSION,
    .kind = LUMEN_PLUGIN_OUTPUT_AUDIO,
    .name = "sdl2-audio-output",
    .version = "0.3.0 (persistent device lifecycle)",
    .vtable = { .audio_output = &VTABLE },
};

#ifndef LUMEN_BUILTIN
LUMEN_EXPORT const lumen_plugin_descriptor_t *lumen_get_plugin(void) {
    return &DESCRIPTOR;
}
#endif

const lumen_plugin_descriptor_t *lumen_audio_output_builtin(void) {
    return &DESCRIPTOR;
}
