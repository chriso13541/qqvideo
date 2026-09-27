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

    /* Audio clock (see get_clock in lumen_plugin.h) */
    double  bytes_per_ms;       /* device rate * channels * 2 bytes / 1000 */
    int     chunk_ms;           /* duration of one device buffer (obtained.samples) */
    int64_t queued_end_pts;     /* pts just past the last sample queued; -1 = none since load/seek */
    /* Interpolation state. The device pulls audio out of the queue one
     * whole buffer (~85 ms) at a time, so "end of queue minus what's still
     * queued" only moves in 85 ms steps -- a clock like that put video up
     * to 81 ms off (measured). Between pulls, time is interpolated from
     * when the last pull happened. */
    int64_t last_pulled;        /* queued_end_pts - queued at the last pull */
    Uint64  pulled_ticks;       /* when that pull was first seen */
    Uint64  frozen_elapsed;     /* interpolation progress captured at pause */
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
    ctx->bytes_per_ms = (double)obtained.freq * obtained.channels * 2 / 1000.0;
    ctx->chunk_ms = (int)((long)obtained.samples * 1000 / obtained.freq);
    ctx->queued_end_pts = -1;
    ctx->last_pulled = INT64_MIN;
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
        /* Pausing the device stops playback immediately AND keeps the
         * queue. This used to also SDL_ClearQueuedAudio(), which threw away
         * everything buffered (~0.65 s measured): video resumed where it
         * stopped, audio resumed ahead of it, and each pause added more
         * lip-sync error. */
        Uint64 now = SDL_GetTicks64();
        if (state->paused) ctx->frozen_elapsed = now - ctx->pulled_ticks;   /* freeze the clock */
        else               ctx->pulled_ticks = now - ctx->frozen_elapsed;   /* ...and resume it */
        SDL_PauseAudioDevice(ctx->device, state->paused ? 1 : 0);
        ctx->device_paused = state->paused;
    }

    /* Clear stale pre-mute audio for instant silence when muting */
    ctx->was_muted = state->muted;   /* mute = queue silence, see present() */

    if (state->seek_generation != ctx->last_seen_seek_generation) {
        SDL_ClearQueuedAudio(ctx->device);        /* pre-seek audio is stale: drop it */
        ctx->queued_end_pts = -1;                 /* clock invalid until new audio is queued */
        ctx->last_pulled = INT64_MIN;
        ctx->last_seen_seek_generation = state->seek_generation;
    }

    return 0;
}

static int sdl2_audio_present(lumen_output_ctx_t *ctx, const lumen_frame_t *frame) {
    if (!ctx->device || !frame->audio.data || frame->audio.size == 0) return 0;
    /* Muted = queue silence rather than dropping frames: the audio clock
     * keeps running (video is timed against it), and unmuting is instant. */
    float volume = ctx->state ? (ctx->state->muted ? 0.0f : ctx->state->volume) : 1.0f;
    if (volume >= 0.999f) {
        SDL_QueueAudio(ctx->device, frame->audio.data, (Uint32)frame->audio.size);
    } else {
        size_t n_samples = frame->audio.size / sizeof(int16_t);
        int16_t *scaled = (int16_t *)malloc(frame->audio.size);
        if (!scaled) return 0;
        const int16_t *src = (const int16_t *)frame->audio.data;
        for (size_t i = 0; i < n_samples; i++) {
            scaled[i] = (int16_t)((float)src[i] * volume);
        }
        SDL_QueueAudio(ctx->device, scaled, (Uint32)frame->audio.size);
        free(scaled);
    }
    if (frame->pts >= 0 && frame->audio.sample_rate > 0)
        ctx->queued_end_pts = frame->pts + (int64_t)frame->audio.nb_samples * 1000 / frame->audio.sample_rate;
    return 0;
}

/* pts of the sample coming out of the speakers now -- see lumen_plugin.h */
static int sdl2_audio_get_clock(lumen_output_ctx_t *ctx, int64_t *clock_ms, int *buffered_ms) {
    if (!ctx->device || ctx->queued_end_pts < 0 || ctx->bytes_per_ms <= 0) return 0;
    Uint32 queued = SDL_GetQueuedAudioSize(ctx->device);
    int buf = (int)(queued / ctx->bytes_per_ms);
    Uint64 now = SDL_GetTicks64();

    /* Everything up to `pulled` has left the queue for the device. Queuing
     * new audio raises queued_end_pts and `queued` equally, so this only
     * changes when the device takes another buffer. */
    int64_t pulled = ctx->queued_end_pts - buf;
    if (pulled != ctx->last_pulled) {
        ctx->last_pulled = pulled;
        ctx->pulled_ticks = now;
        ctx->frozen_elapsed = 0;
    }
    /* The buffer taken at pulled_ticks started playing then and lasts
     * chunk_ms: audible now = its start + time elapsed within it. */
    Uint64 elapsed = ctx->device_paused ? ctx->frozen_elapsed : now - ctx->pulled_ticks;
    if (elapsed > (Uint64)ctx->chunk_ms) elapsed = (Uint64)ctx->chunk_ms;
    *clock_ms = pulled - ctx->chunk_ms + (int64_t)elapsed;
    *buffered_ms = buf;
    return 1;
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
    .get_clock = sdl2_audio_get_clock,
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
