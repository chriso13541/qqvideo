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
#include "tempo.h"          /* playback speed without the chipmunk effect */
#include <SDL2/SDL.h>
#include <stdlib.h>
#include <math.h>   /* tanhf, lrintf: the volume limiter */
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
    /* What's open now, so a device change (Audio > Audio Device) can reopen
     * the same format on the new device immediately. */
    int     cur_rate, cur_channels;
    char    cur_device[128];

    /* Playback speed (state->playback_speed): audio is time-stretched
     * without changing pitch (tempo.c). The clock then runs at media
     * speed: what's queued in the device covers `queued_speed` times as
     * much media time as device time. */
    tempo_t *tempo;
    int      tempo_rate, tempo_ch;
    int64_t  tempo_base_pts;    /* media pts of the stretcher's input position 0; -1 = not set */
    double   queued_speed;      /* speed of the audio most recently queued (1.0 = untouched) */
};

static void tempo_drop(lumen_output_ctx_t *ctx) {
    if (ctx->tempo) tempo_reset(ctx->tempo);
    ctx->tempo_base_pts = -1;
}

static int sdl2_audio_open(lumen_output_ctx_t **out_ctx) {
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "lumen: SDL_InitSubSystem(AUDIO) failed: %s\n", SDL_GetError());
        return -1;
    }
    lumen_output_ctx_t *ctx = (lumen_output_ctx_t *)calloc(1, sizeof(*ctx));
    if (!ctx) return -1;
    ctx->tempo_base_pts = -1;
    ctx->queued_speed = 1.0;
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

    /* Preferences > Audio > output device ("" = system default). If the
     * chosen device is gone (unplugged headset), fall back to the default. */
    const char *devname = (ctx->state && ctx->state->pref_audio_device[0]) ? ctx->state->pref_audio_device : NULL;
    ctx->cur_rate = sample_rate;
    ctx->cur_channels = channels;
    snprintf(ctx->cur_device, sizeof(ctx->cur_device), "%s", devname ? devname : "");
    SDL_AudioDeviceID dev = SDL_OpenAudioDevice(devname, 0, &wanted, &obtained, 0);
    if (!dev && devname) {
        fprintf(stderr, "lumen: audio device '%s' unavailable (%s) -- using the default\n", devname, SDL_GetError());
        dev = SDL_OpenAudioDevice(NULL, 0, &wanted, &obtained, 0);
    }
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
    tempo_free(ctx->tempo);                 /* new format: a new stretcher when needed */
    ctx->tempo = NULL;
    ctx->tempo_base_pts = -1;
    ctx->queued_speed = 1.0;
    SDL_PauseAudioDevice(dev, 0);
    printf("  [sdl2-audio] loaded stream %dHz %dch\n", obtained.freq, obtained.channels);
    return 0;
}

static void sdl2_audio_bind_state(lumen_output_ctx_t *ctx, lumen_playback_state_t *state) {
    ctx->state = state;
}

static int sdl2_audio_pump_ui(lumen_output_ctx_t *ctx, lumen_playback_state_t *state) {
    if (!state || !ctx->device) return 0;

    /* Audio > Audio Device (or Preferences) picked another device: move to
     * it NOW, same format. What was queued on the old device (~0.25 s) is
     * lost; the audio clock restarts with the next audio queued, and video
     * stays in sync with it. */
    if (ctx->cur_rate > 0 && strcmp(state->pref_audio_device, ctx->cur_device) != 0) {
        int was_paused = ctx->device_paused;
        printf("  [sdl2-audio] switching output to '%s'\n",
               state->pref_audio_device[0] ? state->pref_audio_device : "system default");
        if (sdl2_audio_load_stream(ctx, ctx->cur_rate, ctx->cur_channels, LUMEN_SAMPLEFMT_S16) != 0 || !ctx->device)
            return 0;
        if (was_paused || state->paused) {                    /* keep a paused player paused */
            SDL_PauseAudioDevice(ctx->device, 1);
            ctx->device_paused = 1;
        }
    }

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
        tempo_drop(ctx);                          /* its held-back audio is from before the seek */
        ctx->last_seen_seek_generation = state->seek_generation;
    }

    return 0;
}

/* The volume slider's 100% plays this much louder than the file itself
 * (about +2.3 dB), and every other setting scales with it: 50% = 0.65x.
 * Many files are mastered quietly, and 100% being merely "as recorded"
 * was too quiet next to other players. */
#define LUMEN_VOLUME_GAIN 1.3f

/* Above full scale a boosted sample would clip (harsh distortion on loud
 * peaks). Instead, samples beyond 80% of full scale are squeezed smoothly
 * into the room that's left: identical below the knee, and the output can
 * approach but never pass full scale. */
#define LIMIT_KNEE 26214.0f            /* 0.8 * 32767 */
static int16_t soft_limit(float x) {
    float a = x < 0 ? -x : x;
    if (a > LIMIT_KNEE) {
        const float room = 32767.0f - LIMIT_KNEE;
        a = LIMIT_KNEE + room * tanhf((a - LIMIT_KNEE) / room);
        x = x < 0 ? -a : a;
    }
    return (int16_t)lrintf(x);
}

/* Volume, the limiter, and into the device queue. */
static void queue_samples_f(lumen_output_ctx_t *ctx, const float *in, size_t n, float gain) {
    int16_t *out = (int16_t *)malloc(n * sizeof(int16_t));
    if (!out) return;
    for (size_t i = 0; i < n; i++) out[i] = soft_limit(in[i] * gain);
    SDL_QueueAudio(ctx->device, out, (Uint32)(n * sizeof(int16_t)));
    free(out);
}

static int sdl2_audio_present(lumen_output_ctx_t *ctx, const lumen_frame_t *frame) {
    if (!ctx->device || !frame->audio.data || frame->audio.size == 0) return 0;
    /* Muted = queue silence rather than dropping frames: the audio clock
     * keeps running (video is timed against it), and unmuting is instant. */
    float volume = ctx->state ? (ctx->state->muted ? 0.0f : ctx->state->volume) : 1.0f;
    const float gain = volume * LUMEN_VOLUME_GAIN;
    const int rate = frame->audio.sample_rate, ch = ctx->cur_channels > 0 ? ctx->cur_channels : 2;
    double speed = ctx->state ? ctx->state->playback_speed : 1.0;
    if (!(speed > 0)) speed = 1.0;
    if (speed < 0.25) speed = 0.25;
    if (speed > 4.0) speed = 4.0;

    if (speed > 0.999 && speed < 1.001) {
        /* Normal speed: as it always was. */
        if (ctx->tempo_base_pts >= 0) tempo_drop(ctx);   /* back from another speed */
        ctx->queued_speed = 1.0;
        if (gain > 0.999f && gain < 1.001f) {
            SDL_QueueAudio(ctx->device, frame->audio.data, (Uint32)frame->audio.size);
        } else {
            size_t n_samples = frame->audio.size / sizeof(int16_t);
            int16_t *scaled = (int16_t *)malloc(frame->audio.size);
            if (!scaled) return 0;
            const int16_t *src = (const int16_t *)frame->audio.data;
            for (size_t i = 0; i < n_samples; i++)
                scaled[i] = soft_limit((float)src[i] * gain);
            SDL_QueueAudio(ctx->device, scaled, (Uint32)frame->audio.size);
            free(scaled);
        }
        if (frame->pts >= 0 && rate > 0)
            ctx->queued_end_pts = frame->pts + (int64_t)frame->audio.nb_samples * 1000 / rate;
        return 0;
    }

    /* Another speed: time-stretch, keeping the pitch. */
    if (rate <= 0) return 0;
    if (!ctx->tempo || ctx->tempo_rate != rate || ctx->tempo_ch != ch) {
        tempo_free(ctx->tempo);
        ctx->tempo = tempo_new(ch, rate);
        ctx->tempo_rate = rate;
        ctx->tempo_ch = ch;
        ctx->tempo_base_pts = -1;
        if (!ctx->tempo) return 0;
    }
    if (ctx->tempo_base_pts < 0) {          /* (re)starting: input position 0 is this frame */
        tempo_reset(ctx->tempo);
        ctx->tempo_base_pts = frame->pts >= 0 ? frame->pts : (ctx->queued_end_pts >= 0 ? ctx->queued_end_pts : 0);
    }
    const int frames = (int)(frame->audio.size / sizeof(int16_t) / (size_t)ch);
    if (tempo_feed(ctx->tempo, (const int16_t *)frame->audio.data, frames, speed) < 0) return 0;
    int got = 0;
    const float *out = tempo_take(ctx->tempo, &got);
    if (got > 0) queue_samples_f(ctx, out, (size_t)got * ch, gain);
    ctx->queued_speed = speed;
    ctx->queued_end_pts = ctx->tempo_base_pts + (int64_t)(tempo_input_pos(ctx->tempo) * 1000.0 / rate);
    return 0;
}

/* pts of the sample coming out of the speakers now -- see lumen_plugin.h */
static int sdl2_audio_get_clock(lumen_output_ctx_t *ctx, int64_t *clock_ms, int *buffered_ms) {
    if (!ctx->device || ctx->queued_end_pts < 0 || ctx->bytes_per_ms <= 0) return 0;
    Uint32 queued = SDL_GetQueuedAudioSize(ctx->device);
    int buf = (int)(queued / ctx->bytes_per_ms);
    Uint64 now = SDL_GetTicks64();
    /* Device time -> media time at the playback speed (1.0 normally). */
    const double sp = ctx->queued_speed > 0 ? ctx->queued_speed : 1.0;

    /* Everything up to `pulled` has left the queue for the device. Queuing
     * new audio raises queued_end_pts and `queued` equally, so this only
     * changes when the device takes another buffer. */
    int64_t pulled = ctx->queued_end_pts - (int64_t)(buf * sp);
    if (pulled != ctx->last_pulled) {
        ctx->last_pulled = pulled;
        ctx->pulled_ticks = now;
        ctx->frozen_elapsed = 0;
    }
    /* The buffer taken at pulled_ticks started playing then and lasts
     * chunk_ms: audible now = its start + time elapsed within it. */
    Uint64 elapsed = ctx->device_paused ? ctx->frozen_elapsed : now - ctx->pulled_ticks;
    if (elapsed > (Uint64)ctx->chunk_ms) elapsed = (Uint64)ctx->chunk_ms;
    *clock_ms = pulled - (int64_t)((ctx->chunk_ms - (int64_t)elapsed) * sp);
    *buffered_ms = buf;                       /* device time: what the core keeps topped up */
    return 1;
}

static void sdl2_audio_close(lumen_output_ctx_t *ctx) {
    if (!ctx) return;
    if (ctx->device) SDL_CloseAudioDevice(ctx->device);
    tempo_free(ctx->tempo);
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
