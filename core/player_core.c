#include "player_core.h"
#include "plugin_loader.h"
#include "subtitles.h"
#include "sidecar.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pthread.h>   /* POSIX threads; MinGW-w64 provides them via winpthreads */
#if defined(_WIN32)
  #include <windows.h>
  static void sleep_ms(int ms) { Sleep((DWORD)ms); }
  static int64_t now_ms(void) { return (int64_t)GetTickCount64(); }
#else
  #include <unistd.h>
  #include <time.h>
  static void sleep_ms(int ms) { usleep((unsigned int)ms * 1000); }
  static int64_t now_ms(void) {
      struct timespec t;
      clock_gettime(CLOCK_MONOTONIC, &t);
      return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
  }
#endif

static const char *file_ext(const char *path) {
    const char *dot = strrchr(path, '.');
    return dot ? dot : "";
}

/* "lib/x/libdecoder_h264.so" -> "decoder_h264" (what the Package Manager
 * UI stores in its rules, since that's what package dirs are keyed by). */
static void plugin_basename(const char *library_path, char *out, size_t n) {
    const char *b = strrchr(library_path, '/');
    const char *b2 = strrchr(library_path, '\\');
    if (b2 > b) b = b2;
    b = b ? b + 1 : library_path;
    if (strncmp(b, "lib", 3) == 0) b += 3;
    strncpy(out, b, n - 1);
    out[n - 1] = '\0';
    char *dot = strrchr(out, '.');
    if (dot) *dot = '\0';
}

/* Playback Rules check. Rules were written by the UI using plugin file
 * base names ("demux_mp4"/"decoder_h264"), but this used to compare them
 * against MANIFEST names ("mp4-demuxer"/"h264-decoder") -- they never
 * matched, so disabling a codec for a container silently did nothing.
 * Accept either spelling, and for the codec side also a bare fourcc,
 * which is what system-FFmpeg mode stores (one decoder plugin serves
 * every codec there, so rules have to be per-codec, not per-plugin). */
static int rule_blocks(const lumen_playback_state_t *state,
                       const lumen_registry_entry_t *demux_entry,
                       const lumen_registry_entry_t *dec_entry,
                       const char *fourcc) {
    char dbase[64], cbase[64];
    plugin_basename(demux_entry->library_path, dbase, sizeof(dbase));
    plugin_basename(dec_entry->library_path, cbase, sizeof(cbase));
    for (int j = 0; j < state->disabled_pair_count; j++) {
        const lumen_disabled_pair_t *r = &state->disabled_pairs[j];
        int dm = strcmp(r->demuxer_name, demux_entry->name) == 0 || strcmp(r->demuxer_name, dbase) == 0;
        int cm = strcmp(r->codec_name, dec_entry->name) == 0 || strcmp(r->codec_name, cbase) == 0 ||
                 strcmp(r->codec_name, fourcc) == 0;
        if (dm && cm) return 1;
    }
    return 0;
}

/* Per-stream runtime state: one decoder instance per stream, exactly as
 * many as the demuxer reported, NOT one decoder shared across streams. */
typedef struct {
    int in_use;
    lumen_stream_type_t type;
    lumen_loaded_plugin_t decoder_plugin;
    const lumen_decoder_vtable_t *dec_vt;
    lumen_decoder_ctx_t *dec_ctx;
    int sub_track;      /* subtitle streams: index into the file's track list */
} stream_state_t;

/* Every packet starts as "no timing info" (-1), not 0: 0 is a real time. */
static void reset_pkt(lumen_packet_t *pkt) {
    memset(pkt, 0, sizeof(*pkt));
    pkt->pts_ms = -1;
    pkt->duration_ms = -1;
}

/* ---- Subtitles ------------------------------------------------------- */

/* "English (SDH) [SRT]", "Spanish - forced [ASS]", "Track 3 [PGS]" */
static void make_track_label(const lumen_stream_desc_t *sd, int number, char *out, size_t n) {
    char base[80];
    if (sd->title[0])          snprintf(base, sizeof(base), "%s", sd->title);
    else if (sd->language[0])  snprintf(base, sizeof(base), "%s",
                                        lumen_language_name(sd->language) ? lumen_language_name(sd->language)
                                                                          : sd->language);
    else                       snprintf(base, sizeof(base), "Track %d", number);
    snprintf(out, n, "%s%s [%s]", base, sd->is_forced ? " - forced" : "", sd->codec_fourcc);
}

static const char *path_basename(const char *p) {
    const char *a = strrchr(p, '/'), *b = strrchr(p, '\\');
    const char *s = a > b ? a : b;
    return s ? s + 1 : p;
}

/* Adds a track entry to the UI list; returns its index or -1 if full. */
static int add_track_entry(lumen_playback_state_t *st, const char *label, int available, int external) {
    if (st->subtitle_track_count >= LUMEN_MAX_SUB_TRACKS) return -1;
    int i = st->subtitle_track_count++;
    snprintf(st->subtitle_tracks[i].label, sizeof(st->subtitle_tracks[i].label), "%s", label);
    st->subtitle_tracks[i].available = available;
    st->subtitle_tracks[i].external = external;
    st->subtitle_tracks[i].file[0] = '\0';
    return i;
}

/* Full paths of external tracks, by track index -- so adding a file that
 * was already found next to the movie selects it instead of duplicating. */
static char g_ext_paths[LUMEN_MAX_SUB_TRACKS][512];

/* "Add Subtitle Track...": open a standalone subtitle file with whatever
 * demuxer claims its extension (demux_libav claims .srt/.ass/.ssa/.vtt),
 * decode every cue up front -- the whole file is a few KB -- then close
 * it. The new track is selected immediately, which is what a user who
 * just picked a file expects. */
static void load_external_subtitles(const lumen_registry_t *reg, lumen_playback_state_t *st,
                                    lumen_sub_track_t *tracks, const char *path,
                                    const char *label, int select, int quiet) {
    const char *err = NULL;
    for (int i = 0; i < st->subtitle_track_count; i++) {
        if (st->subtitle_tracks[i].external && strcmp(g_ext_paths[i], path) == 0) {
            if (select) st->subtitle_selected = i;   /* already loaded: just pick it */
            return;
        }
    }
    const lumen_registry_entry_t *de = lumen_registry_find_demuxer_by_ext(reg, file_ext(path));
    if (!de) {
        if (quiet) return;
        snprintf(st->error_msg, sizeof(st->error_msg),
                 "Can't read subtitle files of type '%s'.\nSupported: .srt .ass .ssa .vtt", file_ext(path));
        return;
    }
    lumen_loaded_plugin_t dp;
    if (lumen_load_plugin(de->library_path, &dp, &err) != 0) return;
    const lumen_demuxer_vtable_t *dmx = dp.desc->vtable.demuxer;

    const char *allowed[128];
    int allowed_count = 0;
    for (int i = 0; i < reg->count; i++)
        if (reg->entries[i].kind == LUMEN_PLUGIN_DECODER)
            for (int j = 0; j < reg->entries[i].fourcc_count && allowed_count < 128; j++)
                allowed[allowed_count++] = reg->entries[i].fourccs[j];

    lumen_demuxer_ctx_t *dctx = NULL;
    lumen_stream_table_t table;
    int rc = dmx->open_ex ? dmx->open_ex(&dctx, path, &table, allowed, allowed_count)
                          : dmx->open(&dctx, path, &table);
    if (rc != 0) {
        if (!quiet)
            snprintf(st->error_msg, sizeof(st->error_msg), "Couldn't open subtitle file:\n%s", path_basename(path));
        lumen_unload_plugin(&dp);
        return;
    }

    int cues = 0, track = -1;
    for (int i = 0; i < table.stream_count && track < 0; i++) {
        const lumen_stream_desc_t *sd = &table.streams[i];
        if (sd->type != LUMEN_STREAM_SUBTITLE) continue;
        const lumen_registry_entry_t *ce = lumen_registry_find_decoder_by_fourcc(reg, sd->codec_fourcc);
        if (!ce) continue;
        lumen_loaded_plugin_t cp;
        if (lumen_load_plugin(ce->library_path, &cp, &err) != 0) continue;
        const lumen_decoder_vtable_t *dec = cp.desc->vtable.decoder;
        lumen_decoder_ctx_t *cctx = NULL;
        if (dec->open(&cctx, sd) == 0) {
            track = add_track_entry(st, label, 1, 1);
            lumen_packet_t pkt;
            reset_pkt(&pkt);
            while (track >= 0 && dmx->read_packet(dctx, &pkt) == 0) {
                lumen_frame_t f;
                if (pkt.stream_index == sd->stream_index && dec->decode(cctx, &pkt, &f) == 0) {
                    cues += lumen_sub_track_add(&tracks[track], f.subtitle.start_ms, f.subtitle.end_ms, f.subtitle.text);
                    dec->frame_free(&f);
                }
                dmx->packet_free(&pkt);
                reset_pkt(&pkt);
            }
            dec->close(cctx);
        }
        lumen_unload_plugin(&cp);
    }
    dmx->close(dctx);
    lumen_unload_plugin(&dp);

    if (track < 0 || cues == 0) {
        if (track >= 0) {                              /* nothing usable: drop the entry */
            lumen_sub_track_free(&tracks[track]);
            st->subtitle_track_count--;
        }
        if (!quiet)
            snprintf(st->error_msg, sizeof(st->error_msg), "No subtitles found in:\n%s", path_basename(path));
        return;
    }
    snprintf(g_ext_paths[track], sizeof(g_ext_paths[track]), "%s", path);
    snprintf(st->subtitle_tracks[track].file, sizeof(st->subtitle_tracks[track].file), "%s", path_basename(path));
    if (select) st->subtitle_selected = track;
    printf("lumen: loaded %d subtitle cue(s) from '%s' as track %d\n", cues, path, track);
}

static void reset_subtitle_state(lumen_playback_state_t *st) {
    st->subtitle_track_count = 0;
    st->subtitle_selected = -1;
    st->subtitle_text[0] = '\0';
    st->subtitle_add_requested = 0;
}

/* The persistent part of the player -- video/audio outputs opened ONCE
 * and reused across as many files as get played in this session,
 * which is the whole point of the restructuring: launching the app
 * gives you a real, visible "blank player" (the window from
 * lumen_session_open) rather than something that only exists for the
 * duration of one file. */
struct lumen_session {
    const lumen_registry_t *reg;

    const lumen_video_output_vtable_t *vout;
    lumen_output_ctx_t *vout_ctx;
    lumen_loaded_plugin_t vout_plugin;
    int have_vout;

    const lumen_audio_output_vtable_t *aout;
    lumen_output_ctx_t *aout_ctx;
    lumen_loaded_plugin_t aout_plugin;
    int have_aout;

    lumen_playback_state_t state; /* persists across files -- volume/fullscreen carry over,
                                    * per-file fields (position, duration, pause, seek) get
                                    * reset at the start of each lumen_session_play_file() call */
};

lumen_session_t *lumen_session_open(const lumen_registry_t *reg) {
    lumen_session_t *session = (lumen_session_t *)calloc(1, sizeof(*session));
    session->reg = reg;
    session->state.volume = 1.0f;
    session->state.precise_seek = 1;

    /* Video and audio output are BUILT INTO the executable -- they are not
     * loaded from the plugin registry. Accessing them directly means:
     * - The app always opens a window regardless of what's in plugins/ or packages/
     * - They cannot be accidentally deleted or misconfigured by the user
     * - No DLL loading overhead for the rendering layer
     *
     * Codecs (decoders, demuxers) are still loaded from packages/ at playback
     * time, so they remain hot-swappable per CVE. */

    /* Forward declarations for the built-in output implementations
     * (defined in output_sdl2.cpp and output_sdl2_audio.c, compiled
     * directly into lumen-play via CMakeLists.txt). */
    extern const lumen_plugin_descriptor_t *lumen_video_output_builtin(void);
    extern const lumen_plugin_descriptor_t *lumen_audio_output_builtin(void);

    const lumen_plugin_descriptor_t *vout_desc = lumen_video_output_builtin();
    if (!vout_desc || !vout_desc->vtable.video_output) {
        fprintf(stderr, "lumen: built-in video output is unavailable\n");
        free(session);
        return NULL;
    }
    session->vout = vout_desc->vtable.video_output;
    if (session->vout->open(&session->vout_ctx) != 0) {
        fprintf(stderr, "lumen: built-in video output failed to open\n");
        free(session);
        return NULL;
    }
    session->have_vout = 1;
    if (session->vout->bind_state) session->vout->bind_state(session->vout_ctx, &session->state);
    printf("lumen: opened video output '%s' (%s)\n", vout_desc->name, vout_desc->version);

    const lumen_plugin_descriptor_t *aout_desc = lumen_audio_output_builtin();
    if (aout_desc && aout_desc->vtable.audio_output) {
        session->aout = aout_desc->vtable.audio_output;
        if (session->aout->open(&session->aout_ctx) == 0) {
            session->have_aout = 1;
            if (session->aout->bind_state) session->aout->bind_state(session->aout_ctx, &session->state);
            printf("lumen: opened audio output '%s' (%s)\n", aout_desc->name, aout_desc->version);
        } else {
            fprintf(stderr, "lumen: built-in audio output failed to open -- continuing without audio\n");
        }
    }

    return session;
}

lumen_idle_result_t lumen_session_idle(lumen_session_t *session) {
    session->state.has_file = 0;
    while (1) {
        if (session->have_vout && session->vout->pump_ui &&
            session->vout->pump_ui(session->vout_ctx, &session->state)) {
            return LUMEN_IDLE_QUIT; /* window closed / ESC */
        }
        if (session->have_aout && session->aout->pump_ui) {
            session->aout->pump_ui(session->aout_ctx, &session->state);
        }
        if (session->state.quit_requested) {
            return LUMEN_IDLE_QUIT;
        }
        if (session->state.open_file_requested) {
            session->state.open_file_requested = 0; /* consumed -- don't leak into the next file's state */
            return LUMEN_IDLE_OPEN_FILE;
        }
        /* Items were added via "Add to Queue" dialog while idle. Return
         * immediately so main.c can drain pending_add_paths and let the
         * normal q_pos < 0 && q_len > 0 && !q_exhausted auto-start
         * branch fire. Without this, the idle loop keeps blocking even
         * after the user successfully queued something, and nothing plays
         * until they also explicitly open a file -- confirmed as the
         * cause of "add to queue while idle does nothing". */
        if (session->state.pending_add_count > 0) {
            return LUMEN_IDLE_QUEUE_UPDATED;
        }
        sleep_ms(10);
    }
}

int lumen_session_is_quit_requested(const lumen_session_t *session) {
    return session->state.quit_requested;
}

lumen_playback_state_t *lumen_session_state(lumen_session_t *session) {
    return &session->state;
}

void lumen_session_close(lumen_session_t *session) {
    if (!session) return;
    /* Output plugins are built into the exe -- close() tears down the window/
     * audio device, but there is no DLL to unload (no lumen_unload_plugin). */
    if (session->have_vout) session->vout->close(session->vout_ctx);
    if (session->have_aout) session->aout->close(session->aout_ctx);
    free(session);
}

/* ---- A/V sync: audio is the master clock ------------------------------
 *
 * Video frames are held until the audio clock (what the speakers are
 * playing right now, from the audio output's get_clock) reaches their pts,
 * and dropped if they're too late. Pausing freezes the audio clock, so
 * video pauses with it; a stall in decoding lets audio keep going and
 * video catches up by dropping. Only when there's no usable audio clock
 * (silent file, audio not started yet, audio track ended) does the video
 * output pace itself against the wall clock, as it always used to. */


static int audio_clock(lumen_session_t *session, const lumen_audio_output_vtable_t *aout,
                       int64_t *clock_ms, int *buffered_ms) {
    if (!aout || !aout->get_clock) return 0;
    return aout->get_clock(session->aout_ctx, clock_ms, buffered_ms);
}

/* Pumps both outputs' UI and checks for anything that ends playback of
 * this file. Returns 1 (and fills out_stats) if playback must stop. */
static int pump_and_check(lumen_session_t *session, const lumen_audio_output_vtable_t *aout,
                          lumen_playback_state_t *state, lumen_play_stats_t *out_stats) {
    if (session->have_vout && session->vout->pump_ui &&
        session->vout->pump_ui(session->vout_ctx, state)) {
        state->quit_requested = 1;
        return 1;
    }
    if (aout && aout->pump_ui && aout->pump_ui(session->aout_ctx, state)) return 1;
    if (state->quit_requested) return 1;
    if (state->open_file_requested) {
        out_stats->open_file_requested = 1;
        return 1;
    }
    if (state->queue_jump_requested) {
        out_stats->queue_jump_index = state->queue_jump_index;
        state->queue_jump_requested = 0;
        return 1;
    }
    if (state->stop_requested) {
        out_stats->stop_requested = 1;
        state->stop_requested = 0;
        return 1;
    }
    return 0;
}

/* =====================================================================
 * Playback engine: a DECODE THREAD feeding the MAIN (UI) thread
 * =====================================================================
 *
 * Until now one loop did everything -- UI, reading the file, decoding,
 * presenting -- so a 4K frame that took 200 ms to decode froze the UI for
 * 200 ms, and a stalled read (network share) froze it for as long as the
 * stall. Now:
 *
 *   decode thread: demuxer + decoders only. Reads packets, decodes them,
 *                  and pushes frames into two queues (video, audio) and
 *                  subtitle cues into the cue store. Performs seeks, and
 *                  runs the adaptive skip policy (it owns the decoders).
 *   main thread:   UI, subtitle text, feeding the audio device from the
 *                  audio queue, and showing each video frame when the
 *                  audio clock (or, without audio, the wall clock)
 *                  reaches it. Never waits on a decoder or on disk.
 *
 * Everything shared lives in engine_t under ONE mutex; nothing is held
 * locked across a decode, a read, or an output call.
 *
 * Seeking uses SERIAL numbers (as ffplay does): each seek bumps the serial,
 * every frame is tagged with the serial it was decoded under, and frames
 * with a stale serial are thrown away instead of shown -- so frames already
 * queued, or decoded while a seek was on its way, can never flash up. */

#define LUMEN_VIDEO_LATE_DROP_MS 100    /* later than this: skip it (if more are queued) */
#define VQ_SOFT        8                /* decode this far ahead normally */
#define VQ_HARD        16               /* ...further only if audio is running low */
#define VQ_MAX_BYTES   ((size_t)192 << 20)  /* a 4K frame is ~12 MB: bound memory, not count */
#define AQ_CAP         512              /* decoded audio frames */
#define AQ_AHEAD_MAX_MS 3000            /* audio decoded ahead can't exceed this */
#define AQ_AUDIO_ONLY_MS 500            /* audio-only files: decode this far ahead */
#define DEVICE_TARGET_MS 250            /* keep this much queued in the audio device */

typedef struct {
    lumen_frame_t                 frame;
    const lumen_decoder_vtable_t *dec;      /* owner, for frame_free */
    int                           serial;
} qitem_t;

typedef struct {
    pthread_t       thread;
    int             thread_started;
    pthread_mutex_t lock;
    pthread_cond_t  cond;        /* decode thread sleeps here: waiting for room / a command */

    qitem_t vq[VQ_HARD];  int vq_head, vq_count; size_t vq_bytes;
    qitem_t aq[AQ_CAP];   int aq_head, aq_count; int64_t aq_ms;

    /* main -> decode */
    int     abort;
    int     seek_pending;
    int64_t seek_target;
    int     seek_flags;
    int     seek_precise;
    int     serial;              /* current serial (main bumps it on seek) */
    /* decode -> main */
    int     dec_serial;          /* serial the decode thread is producing */
    int     eof;                 /* decode thread reached end of file (for dec_serial) */
    /* audio clock snapshot, main -> decode (for the skip policy) */
    int     clk_valid;
    int64_t clk_ms, clk_at;
    int     clk_buf_ms;          /* audio queued in the device at snapshot time */
    int     clk_running;

    /* decode-thread-only (set up before start, read after join) */
    const lumen_demuxer_vtable_t *dmx;
    lumen_demuxer_ctx_t          *dctx;
    stream_state_t               *streams;
    lumen_sub_track_t            *sub_tracks;
    int  has_video, has_audio;
    int  frames_decoded, video_frames, audio_frames, frames_failed;
    /* Precise-seek catch-up (decode thread only): after seeking to the
     * keyframe before catch_target, discard video frames and trim audio
     * until the target is reached. -1 = not catching up. */
    int64_t catch_target;
    int     catch_video, catch_audio;
    int     catch_skip;           /* set_skip level currently applied while catching up */
    int     catch_discarded;
    int     half_frame_ms;        /* half a video frame: tolerance for "the frame AT the target" */
} engine_t;

/* ---- queues (call with lock held) ---- */

static size_t video_bytes(const lumen_frame_t *f) {
    return (size_t)f->video.width * (size_t)f->video.height * 3 / 2;
}
static int64_t audio_ms(const lumen_frame_t *f) {
    return f->audio.sample_rate > 0 ? (int64_t)f->audio.nb_samples * 1000 / f->audio.sample_rate : 0;
}
static void vq_pop(engine_t *e, qitem_t *out) {
    *out = e->vq[e->vq_head];
    e->vq_head = (e->vq_head + 1) % VQ_HARD;
    e->vq_count--;
    e->vq_bytes -= video_bytes(&out->frame);
}
static void aq_pop(engine_t *e, qitem_t *out) {
    *out = e->aq[e->aq_head];
    e->aq_head = (e->aq_head + 1) % AQ_CAP;
    e->aq_count--;
    e->aq_ms -= audio_ms(&out->frame);
}
static void queues_clear(engine_t *e) {
    qitem_t it;
    while (e->vq_count > 0) { vq_pop(e, &it); it.dec->frame_free(&it.frame); }
    while (e->aq_count > 0) { aq_pop(e, &it); it.dec->frame_free(&it.frame); }
}

/* Room for the decode thread to read another packet? (lock held) */
static int has_room(const engine_t *e) {
    int aq_ok = e->aq_count < AQ_CAP - 8 && e->aq_ms < AQ_AHEAD_MAX_MS;
    if (!aq_ok) return 0;
    if (e->has_video) {
        int reserve = (int)e->aq_ms + (e->clk_valid ? e->clk_buf_ms : 0);
        return (e->vq_count < VQ_SOFT && e->vq_bytes < VQ_MAX_BYTES) ||
               (reserve < 150 && e->vq_count < VQ_HARD - 1);
    }
    if (e->has_audio) return e->aq_ms < AQ_AUDIO_ONLY_MS;
    return 1;
}

/* Decode thread: hand a frame to the main thread. Waits if the queue is
 * full; drops the frame if a seek or stop arrives meanwhile. (lock NOT held) */
static void push_frame(engine_t *e, lumen_frame_t *f, const lumen_decoder_vtable_t *dec) {
    int video = f->type == LUMEN_STREAM_VIDEO;
    pthread_mutex_lock(&e->lock);
    while (!e->abort && !e->seek_pending && (video ? e->vq_count >= VQ_HARD : e->aq_count >= AQ_CAP))
        pthread_cond_wait(&e->cond, &e->lock);
    if (e->abort || e->seek_pending) {
        pthread_mutex_unlock(&e->lock);
        dec->frame_free(f);
        return;
    }
    qitem_t it = { *f, dec, e->dec_serial };
    if (video) {
        e->vq[(e->vq_head + e->vq_count) % VQ_HARD] = it;
        e->vq_count++;
        e->vq_bytes += video_bytes(f);
        e->video_frames++;
    } else {
        e->aq[(e->aq_head + e->aq_count) % AQ_CAP] = it;
        e->aq_count++;
        e->aq_ms += audio_ms(f);
        e->audio_frames++;
    }
    e->frames_decoded++;
    pthread_mutex_unlock(&e->lock);
}

/* ---- Adaptive decode skipping (decode thread) ------------------------
 *
 * When video decoding can't keep up, audio starves too. Dropping late
 * frames AFTER decoding them saves nothing -- the time goes into decoding
 * -- so ask the decoder to skip work (set_skip): first frames nothing
 * depends on, then the deblocking filter. Back off after a calm stretch;
 * a premature step-down doubles the stretch needed next time. */

typedef struct {
    int     level;        /* 0..2, current set_skip level */
    int     behind;       /* consecutive decoded frames that arrived (nearly) late */
    int     warmup;       /* frames to ignore after start/seek while buffers refill */
    int64_t hold_until;   /* pts: after a level change, give it time to take effect */
    int64_t comfy_since;  /* pts where the current comfortable stretch began, -1 = none */
    int64_t comfy_ms;     /* how long a comfortable stretch must last to step down */
    int64_t last_down;    /* pts of the last step down, -1 = none */
} skip_state_t;

#define SKIP_HOLD_MS        500
#define SKIP_COMFY_MS      3000
#define SKIP_COMFY_MAX_MS 60000
#define SKIP_REGRET_MS    10000

static void skip_reset(skip_state_t *k) {
    if (k->comfy_ms <= 0) k->comfy_ms = SKIP_COMFY_MS;
    k->behind = 0;
    k->warmup = 30;
    k->hold_until = -1;
    k->comfy_since = -1;
}

static void skip_change(stream_state_t *ss, skip_state_t *k, int level, int64_t pts) {
    k->level = level;
    k->behind = 0;
    k->comfy_since = -1;
    k->hold_until = pts + SKIP_HOLD_MS;
    ss->dec_vt->set_skip(ss->dec_ctx, level);
}

static void skip_policy(engine_t *e, stream_state_t *ss, skip_state_t *k, int64_t pts) {
    static int disabled = -1;          /* QQVIDEO_FRAMESKIP=0: always decode everything */
    if (disabled < 0) { const char *v = getenv("QQVIDEO_FRAMESKIP"); disabled = v && v[0] == '0'; }
    if (disabled || !ss->dec_vt->set_skip) return;
    if (k->warmup > 0) { k->warmup--; return; }
    if (pts < k->hold_until) return;

    /* The audio clock lives on the main thread; use its latest snapshot,
     * advanced by the time since it was taken. Audio "reserve" counts
     * decoded-but-not-yet-queued audio too. */
    pthread_mutex_lock(&e->lock);
    int valid = e->clk_valid;
    int64_t clk = e->clk_ms + (e->clk_running ? now_ms() - e->clk_at : 0);
    int reserve = e->clk_buf_ms + (int)e->aq_ms;
    pthread_mutex_unlock(&e->lock);
    if (!valid) return;
    int64_t slack = pts - clk;

    if (reserve < 150 || slack < 40) {
        k->comfy_since = -1;
        if (++k->behind >= 6 && k->level < 2) {
            if (k->last_down >= 0 && pts - k->last_down < SKIP_REGRET_MS && k->comfy_ms < SKIP_COMFY_MAX_MS)
                k->comfy_ms *= 2;
            skip_change(ss, k, k->level + 1, pts);
            printf("lumen: decoding can't keep up -- %s\n", k->level == 1
                   ? "skipping non-reference frames (level 1)"
                   : "also skipping the deblocking filter (level 2)");
        }
        return;
    }
    k->behind = 0;
    if (k->level == 0) return;
    if (reserve >= 300 && slack >= 120 && k->comfy_since < 0) k->comfy_since = pts;
    if (k->comfy_since >= 0 && pts - k->comfy_since >= k->comfy_ms) {
        skip_change(ss, k, k->level - 1, pts);
        k->last_down = pts;
        printf("lumen: decoding keeping up -- skip level %d\n", k->level);
    }
}

/* ---- the decode thread ---- */

/* Drops the first `ms` of an interleaved S16 audio frame, in place. */
static void trim_audio_front(lumen_frame_t *f, int64_t ms) {
    int ch = f->audio.channels > 0 ? f->audio.channels : 1;
    int64_t n = ms * f->audio.sample_rate / 1000;
    if (n <= 0) return;
    if (n >= f->audio.nb_samples) n = f->audio.nb_samples;
    size_t bytes = (size_t)n * ch * sizeof(int16_t);
    memmove(f->audio.data, f->audio.data + bytes, f->audio.size - bytes);
    f->audio.size -= bytes;
    f->audio.nb_samples -= (int)n;
    f->pts += n * 1000 / f->audio.sample_rate;
}

static void deliver(engine_t *e, stream_state_t *ss, skip_state_t *skip, lumen_frame_t *f) {
    if (ss->type == LUMEN_STREAM_SUBTITLE) {
        if (ss->sub_track >= 0) {
            pthread_mutex_lock(&e->lock);
            lumen_sub_track_add(&e->sub_tracks[ss->sub_track], f->subtitle.start_ms,
                                f->subtitle.end_ms, f->subtitle.text);
            pthread_mutex_unlock(&e->lock);
        }
        ss->dec_vt->frame_free(f);
        return;
    }
    if (e->catch_target >= 0) {
        if (ss->type == LUMEN_STREAM_VIDEO && e->catch_video) {
            /* Half a frame of tolerance, so the frame on screen AT the
             * target (pts a hair before it) is kept rather than the next
             * one -- but at 60 fps not the frame before that either. */
            if (f->pts >= 0 && f->pts < e->catch_target - e->half_frame_ms) {
                ss->dec_vt->frame_free(f);
                e->catch_discarded++;
                return;
            }
            e->catch_video = 0;
            if (ss->dec_vt->set_skip && e->catch_skip != skip->level)
                ss->dec_vt->set_skip(ss->dec_ctx, skip->level);     /* back to normal */
        }
        if (ss->type == LUMEN_STREAM_AUDIO && e->catch_audio) {
            if (f->pts >= 0 && f->pts + audio_ms(f) <= e->catch_target) {
                ss->dec_vt->frame_free(f);
                return;
            }
            if (f->pts >= 0 && f->pts < e->catch_target) trim_audio_front(f, e->catch_target - f->pts);
            e->catch_audio = 0;
        }
        if (!e->catch_video && !e->catch_audio) {
            if (e->catch_discarded)
                printf("lumen: precise seek -- decoded %d frame(s) past the keyframe to reach %lld ms\n",
                       e->catch_discarded, (long long)e->catch_target);
            e->catch_target = -1;
        }
    }
    if (ss->type == LUMEN_STREAM_VIDEO) skip_policy(e, ss, skip, f->pts);
    push_frame(e, f, ss->dec_vt);
}

static void *decode_thread_main(void *arg) {
    engine_t *e = (engine_t *)arg;
    lumen_packet_t pkt;
    reset_pkt(&pkt);
    skip_state_t skip;
    memset(&skip, 0, sizeof(skip));
    skip.last_down = -1;
    skip_reset(&skip);
    int eof = 0;
    e->catch_target = -1;

    for (;;) {
        pthread_mutex_lock(&e->lock);
        while (!e->abort && !e->seek_pending && (eof || !has_room(e)))
            pthread_cond_wait(&e->cond, &e->lock);
        if (e->abort) { pthread_mutex_unlock(&e->lock); break; }
        if (e->seek_pending) {
            int64_t target = e->seek_target;
            int flags = e->seek_flags;
            int precise = e->seek_precise;
            int serial = e->serial;
            if (precise) flags = LUMEN_SEEK_BACKWARD;   /* keyframe before, then decode up to the target */
            e->seek_pending = 0;
            pthread_mutex_unlock(&e->lock);

            int rc = e->dmx->seek_ex ? e->dmx->seek_ex(e->dctx, target, flags)
                   : e->dmx->seek    ? e->dmx->seek(e->dctx, target)
                   : -2;
            if (rc == -2) {
                fprintf(stderr, "lumen: this demuxer doesn't support seeking -- ignoring request\n");
            } else if (rc != 0) {
                /* e.g. a forward jump with no keyframe left before the end:
                 * playback simply carries on from where the demuxer is. */
                fprintf(stderr, "lumen: no keyframe %s %lldms -- continuing\n",
                        (flags & LUMEN_SEEK_FORWARD) ? "after" : "before", (long long)target);
            } else {
                for (int i = 0; i < LUMEN_MAX_STREAMS; i++)
                    if (e->streams[i].in_use && e->streams[i].dec_vt->flush)
                        e->streams[i].dec_vt->flush(e->streams[i].dec_ctx, target);
            }
            e->catch_target = (rc == 0 && precise) ? target : -1;
            e->catch_video = e->has_video;
            e->catch_audio = e->has_audio;
            e->catch_skip = skip.level;
            e->catch_discarded = 0;
            skip_reset(&skip);            /* keep the level: the hardware didn't change */
            eof = 0;
            pthread_mutex_lock(&e->lock);
            e->dec_serial = serial;
            e->eof = 0;
            queues_clear(e);              /* anything queued is from before the seek */
            pthread_mutex_unlock(&e->lock);
            continue;
        }
        pthread_mutex_unlock(&e->lock);

        /* Reading and decoding happen with the lock released: the main
         * thread keeps running the UI no matter how long these take. */
        if (e->dmx->read_packet(e->dctx, &pkt) != 0) {
            for (int i = 0; i < LUMEN_MAX_STREAMS; i++) {     /* EOF: flush decoders */
                stream_state_t *ss = &e->streams[i];
                if (!ss->in_use || !ss->dec_vt->drain || ss->type == LUMEN_STREAM_SUBTITLE) continue;
                lumen_frame_t f;
                while (ss->dec_vt->drain(ss->dec_ctx, &f) == 0) deliver(e, ss, &skip, &f);
            }
            eof = 1;
            pthread_mutex_lock(&e->lock);
            e->eof = 1;
            pthread_mutex_unlock(&e->lock);
            reset_pkt(&pkt);
            continue;
        }
        if (pkt.stream_index < 0 || pkt.stream_index >= LUMEN_MAX_STREAMS || !e->streams[pkt.stream_index].in_use) {
            e->dmx->packet_free(&pkt);
            reset_pkt(&pkt);
            continue;
        }
        stream_state_t *ss = &e->streams[pkt.stream_index];
        /* Catching up to a precise-seek target: frames well before it are
         * never shown, so skip decoding the ones nothing else depends on.
         * Within 250 ms of the target decode everything, so the frame we
         * land on is exact. */
        if (e->catch_target >= 0 && e->catch_video && ss->type == LUMEN_STREAM_VIDEO && ss->dec_vt->set_skip) {
            int want = (pkt.pts_ms >= 0 && pkt.pts_ms < e->catch_target - 250)
                     ? (skip.level > 1 ? skip.level : 1) : skip.level;
            if (want != e->catch_skip) {
                ss->dec_vt->set_skip(ss->dec_ctx, want);
                e->catch_skip = want;
            }
        }
        lumen_frame_t f;
        int rc = ss->dec_vt->decode(ss->dec_ctx, &pkt, &f);
        if (rc == 0) deliver(e, ss, &skip, &f);
        else if (rc < 0) e->frames_failed++;
        /* A packet can produce more than one frame: collect them all. */
        if (rc >= 0 && ss->type != LUMEN_STREAM_SUBTITLE && ss->dec_vt->receive)
            while (ss->dec_vt->receive(ss->dec_ctx, &f) == 0) deliver(e, ss, &skip, &f);
        e->dmx->packet_free(&pkt);
        reset_pkt(&pkt);
    }
    return NULL;
}

int lumen_session_play_file(lumen_session_t *session, const char *path, lumen_play_stats_t *out_stats) {
    memset(out_stats, 0, sizeof(*out_stats));
    out_stats->queue_jump_index = -1;
    out_stats->stop_requested = 0;
    const lumen_registry_t *reg = session->reg;
    lumen_playback_state_t *state = &session->state;
    const char *err = NULL;

    /* Reset per-file fields. Deliberately NOT touching volume,
     * fullscreen-related state owned by the output plugin itself, or
     * seek_generation (monotonic for the whole session, not per-file)
     * -- those should carry over between files, matching how a real
     * media player remembers your volume setting across tracks. */
    state->paused = 0;
    state->quit_requested = 0;
    state->open_file_requested = 0;
    state->seek_requested = 0;
    state->seek_target_ms = 0;
    state->position_ms = 0;
    state->duration_ms = 0;
    state->has_file = 0;
    reset_subtitle_state(state);   /* None is the default for every new file */

    const lumen_registry_entry_t *demux_entry = lumen_registry_find_demuxer_by_ext(reg, file_ext(path));
    if (!demux_entry) {
        fprintf(stderr, "lumen: no installed demuxer claims extension '%s'\n", file_ext(path));
        return 1;
    }
    lumen_loaded_plugin_t demux_plugin;
    if (lumen_load_plugin(demux_entry->library_path, &demux_plugin, &err) != 0) {
        fprintf(stderr, "lumen: failed to load demuxer '%s': %s\n", demux_entry->name, err);
        return 1;
    }
    printf("lumen: loaded demuxer plugin '%s' (%s)\n", demux_plugin.desc->name, demux_plugin.desc->version);

    const lumen_demuxer_vtable_t *dmx = demux_plugin.desc->vtable.demuxer;
    lumen_demuxer_ctx_t *dctx = NULL;
    lumen_stream_table_t table;

    /* Allow-list for this file: every fourcc an installed decoder claims,
     * minus Playback Rules for this container. libav-backed demuxers turn
     * it into libavformat's codec_whitelist (see open_ex in lumen_plugin.h). */
    const char *allowed[128];
    int allowed_count = 0;
    for (int i = 0; i < reg->count; i++) {
        const lumen_registry_entry_t *e = &reg->entries[i];
        if (e->kind != LUMEN_PLUGIN_DECODER) continue;
        for (int j = 0; j < e->fourcc_count && allowed_count < 128; j++) {
            if (!rule_blocks(state, demux_entry, e, e->fourccs[j]))
                allowed[allowed_count++] = e->fourccs[j];
        }
    }

    int open_rc = dmx->open_ex ? dmx->open_ex(&dctx, path, &table, allowed, allowed_count)
                               : dmx->open(&dctx, path, &table);
    if (open_rc != 0) {
        fprintf(stderr, "lumen: demuxer failed to open '%s'\n", path);
        lumen_unload_plugin(&demux_plugin);
        return 1;
    }
    state->has_file = 1;
    state->duration_ms = (int64_t)(table.duration_sec * 1000.0);
    printf("lumen: demuxer found %d stream(s), duration=%.1fs\n", table.stream_count, table.duration_sec);

    stream_state_t streams[LUMEN_MAX_STREAMS];
    memset(streams, 0, sizeof(streams));
    lumen_sub_track_t sub_tracks[LUMEN_MAX_SUB_TRACKS];
    memset(sub_tracks, 0, sizeof(sub_tracks));
    int sub_number = 0;

    for (int i = 0; i < table.stream_count; i++) {
        const lumen_stream_desc_t *sd = &table.streams[i];
        const char *type_name = (sd->type == LUMEN_STREAM_VIDEO) ? "video"
                              : (sd->type == LUMEN_STREAM_AUDIO) ? "audio" : "subtitle";

        if (sd->stream_index < 0 || sd->stream_index >= LUMEN_MAX_STREAMS) {
            fprintf(stderr, "lumen: stream %d index out of range, skipping\n", i);
            continue;
        }

        /* Subtitle streams: always LISTED in the menu; decoded only if a
         * decoder claims the codec and no Playback Rule blocks it. Image
         * subtitles (PGS/VobSub) show up grayed out instead of vanishing. */
        if (sd->type == LUMEN_STREAM_SUBTITLE) {
            char label[96];
            make_track_label(sd, ++sub_number, label, sizeof(label));
            const lumen_registry_entry_t *se = lumen_registry_find_decoder_by_fourcc(reg, sd->codec_fourcc);
            int ok = se && !rule_blocks(state, demux_entry, se, sd->codec_fourcc);
            stream_state_t *ss = &streams[sd->stream_index];
            if (ok && lumen_load_plugin(se->library_path, &ss->decoder_plugin, &err) == 0) {
                ss->dec_vt = ss->decoder_plugin.desc->vtable.decoder;
                if (ss->dec_vt->open(&ss->dec_ctx, sd) == 0) {
                    ss->in_use = 1;
                    ss->type = LUMEN_STREAM_SUBTITLE;
                } else {
                    lumen_unload_plugin(&ss->decoder_plugin);
                    ok = 0;
                }
            } else {
                ok = 0;
            }
            if (!ok) {
                size_t l = strlen(label);
                int image = !strcmp(sd->codec_fourcc, "PGS") || !strcmp(sd->codec_fourcc, "VOBS") ||
                            !strcmp(sd->codec_fourcc, "DVBS");
                snprintf(label + l, sizeof(label) - l, "%s",
                         se ? " (disabled)" : image ? " (image-based, not supported yet)" : " (not supported)");
            }
            ss->sub_track = add_track_entry(state, label, ok, 0);
            if (ss->sub_track < 0 && ss->in_use) {   /* list full: stop decoding it */
                ss->dec_vt->close(ss->dec_ctx);
                lumen_unload_plugin(&ss->decoder_plugin);
                ss->in_use = 0;
            }
            printf("lumen: stream %d (subtitle, codec='%s') -> %s\n", sd->stream_index,
                   sd->codec_fourcc, ok ? "listed + decoding" : "listed, not decodable");
            continue;
        }

        const lumen_registry_entry_t *dec_entry = lumen_registry_find_decoder_by_fourcc(reg, sd->codec_fourcc);
        if (!dec_entry) {
            fprintf(stderr, "lumen: no installed decoder for %s codec '%s' (stream %d)\n",
                    type_name, sd->codec_fourcc, sd->stream_index);
            /* Write a user-visible error if this was the video stream --
             * audio-only fallback is silent but a missing video codec leaves
             * the user staring at a black screen with no explanation. */
            if (sd->type == LUMEN_STREAM_VIDEO) {
#ifdef LUMEN_SYSTEM_FFMPEG
                snprintf(state->error_msg, sizeof(state->error_msg),
                    "No decoder available for video codec '%s'.\n"
                    "Your system FFmpeg doesn't include it, or qqvideo\n"
                    "doesn't support it yet (see Tools > Package Manager).",
                    sd->codec_fourcc);
#else
                snprintf(state->error_msg, sizeof(state->error_msg),
                    "No decoder installed for video codec '%s'.\n"
                    "Install a %s decoder plugin to play this file.",
                    sd->codec_fourcc, sd->codec_fourcc);
#endif
            }
            continue;
        }

        /* Check the Package Manager's per-container block list. */
        {
            if (rule_blocks(state, demux_entry, dec_entry, sd->codec_fourcc)) {
                fprintf(stderr, "lumen: codec '%s' (decoder '%s') is disabled for '%s' -- blocked by Playback Rules\n",
                        sd->codec_fourcc, dec_entry->name, demux_entry->name);
                snprintf(state->error_msg, sizeof(state->error_msg),
                    "Codec '%s' is disabled for this container (%s).\n"
                    "Re-enable it in Tools > Package Manager if needed.",
                    sd->codec_fourcc, demux_entry->name);
                continue;
            }
        }

        stream_state_t *ss = &streams[sd->stream_index];
        if (lumen_load_plugin(dec_entry->library_path, &ss->decoder_plugin, &err) != 0) {
            fprintf(stderr, "lumen: failed to load decoder '%s': %s\n", dec_entry->name, err);
            continue;
        }
        ss->dec_vt = ss->decoder_plugin.desc->vtable.decoder;
        if (ss->dec_vt->open(&ss->dec_ctx, sd) != 0) {
            fprintf(stderr, "lumen: decoder '%s' failed to initialize for stream %d\n", dec_entry->name, sd->stream_index);
            lumen_unload_plugin(&ss->decoder_plugin);
            continue;
        }
        ss->in_use = 1;
        ss->type = sd->type;
        printf("lumen: stream %d (%s, codec='%s') -> decoder '%s' (%s)\n",
               sd->stream_index, type_name, sd->codec_fourcc,
               ss->decoder_plugin.desc->name, ss->decoder_plugin.desc->version);
    }

    /* Subtitle files that live next to the movie (or in Subs/): listed
     * after the embedded tracks, loaded now (a few KB each), NOT selected
     * -- None stays the default until the user picks one. */
    {
        static lumen_sidecar_t found[LUMEN_SIDECAR_MAX];
        int nfound = lumen_find_sidecar_subs(path, found, LUMEN_SIDECAR_MAX);
        for (int i = 0; i < nfound; i++)
            load_external_subtitles(reg, state, sub_tracks, found[i].path, found[i].label, 0, 1);
        if (nfound) printf("lumen: %d subtitle file(s) found next to the movie\n", nfound);
    }

    /* Reconfigure the SESSION's already-open outputs for this file via
     * load_stream(), rather than opening fresh ones -- the whole point
     * of the restructuring. `vout`/`aout` here are local: NULL means
     * "don't use for this file" (no video/audio stream present, no
     * output installed, or this file's load_stream() failed), exactly
     * like the old open()-failure fallback, just renamed. */
    const lumen_video_output_vtable_t *vout = NULL;
    const lumen_audio_output_vtable_t *aout = NULL;

    for (int i = 0; i < table.stream_count; i++) {
        const lumen_stream_desc_t *sd = &table.streams[i];
        if (!streams[sd->stream_index].in_use) continue;

        if (sd->type == LUMEN_STREAM_VIDEO && !vout) {
            if (session->have_vout) {
                if (session->vout->load_stream(session->vout_ctx, sd->width, sd->height, LUMEN_PIXFMT_YUV420P) == 0) {
                    vout = session->vout;
                } else {
                    fprintf(stderr, "lumen: video output failed to configure for this file -- continuing without video display\n");
                }
            } else {
                fprintf(stderr, "lumen: no video output available -- video will decode but not display\n");
            }
        }
        if (sd->type == LUMEN_STREAM_AUDIO && !aout) {
            if (session->have_aout) {
                if (session->aout->load_stream(session->aout_ctx, sd->sample_rate, sd->channels, LUMEN_SAMPLEFMT_S16) == 0) {
                    aout = session->aout;
                } else {
                    fprintf(stderr, "lumen: audio output failed to configure for this file -- continuing without audio\n");
                }
            } else {
                fprintf(stderr, "lumen: no audio output available -- audio will decode but not play\n");
            }
        }
    }

    /* ---- start the decode thread (see "Playback engine" above) ---- */
    static engine_t eng;          /* static: the queues are large, keep them off the stack */
    engine_t *e = &eng;
    memset(e, 0, sizeof(*e));
    pthread_mutex_init(&e->lock, NULL);
    pthread_cond_init(&e->cond, NULL);
    e->dmx = dmx;
    e->dctx = dctx;
    e->streams = streams;
    e->sub_tracks = sub_tracks;
    for (int i = 0; i < LUMEN_MAX_STREAMS; i++) {
        if (!streams[i].in_use) continue;
        if (streams[i].type == LUMEN_STREAM_VIDEO) e->has_video = 1;
        if (streams[i].type == LUMEN_STREAM_AUDIO) e->has_audio = 1;
    }
    e->half_frame_ms = 20;                       /* ~24-25 fps if the rate is unknown */
    for (int i = 0; i < table.stream_count; i++)
        if (table.streams[i].type == LUMEN_STREAM_VIDEO && table.streams[i].frame_rate > 1.0)
            e->half_frame_ms = (int)(500.0 / table.streams[i].frame_rate);
    if (pthread_create(&e->thread, NULL, decode_thread_main, e) == 0) {
        e->thread_started = 1;
    } else {
        fprintf(stderr, "lumen: couldn't start the decode thread\n");
    }

    int stop_requested = !e->thread_started;
    int was_paused = 0;
    int64_t wall_anchor = 0;      /* wall-clock pacing: frame pts p is due at wall_anchor + p */
    int wall_anchor_valid = 0;

    while (!stop_requested) {
        /* Pumping the session's video output EVERY iteration keeps the SDL
         * event loop alive even when this file has no decodable video. */
        if (pump_and_check(session, aout, state, out_stats)) {
            stop_requested = 1;
            break;
        }

        /* Subtitles: load a newly added file, then publish what's on
         * screen now. Done before the pause check so picking a track
         * while paused shows its text immediately. The cue store is shared
         * with the decode thread (embedded tracks), hence the lock. */
        if (state->subtitle_add_requested) {
            state->subtitle_add_requested = 0;
            char label[96];
            lumen_sidecar_label(path_basename(state->subtitle_add_path), NULL, label, sizeof(label));
            pthread_mutex_lock(&e->lock);
            load_external_subtitles(reg, state, sub_tracks, state->subtitle_add_path, label, 1, 0);
            pthread_mutex_unlock(&e->lock);
        }
        {
            int sel = state->subtitle_selected;
            if (sel >= 0 && sel < state->subtitle_track_count && state->subtitle_tracks[sel].available) {
                pthread_mutex_lock(&e->lock);
                lumen_sub_track_text_at(&sub_tracks[sel], state->position_ms,
                                        state->subtitle_text, sizeof(state->subtitle_text));
                pthread_mutex_unlock(&e->lock);
            } else {
                state->subtitle_text[0] = '\0';
            }
        }

        if (state->seek_requested) {
            /* Hand the seek to the decode thread and bump the serial:
             * everything queued or still being decoded becomes stale and
             * is discarded rather than shown. */
            pthread_mutex_lock(&e->lock);
            e->seek_pending = 1;
            e->seek_target = state->seek_target_ms;
            e->seek_flags = state->seek_flags;
            e->seek_precise = state->precise_seek;
            e->serial++;
            e->eof = 0;
            queues_clear(e);
            pthread_cond_broadcast(&e->cond);
            pthread_mutex_unlock(&e->lock);
            state->position_ms = state->seek_target_ms;
            state->seek_generation++;         /* audio output drops its queued sound */
            state->seek_requested = 0;
            state->seek_flags = LUMEN_SEEK_BACKWARD;   /* back to the default for the next seek */
            wall_anchor_valid = 0;
            continue;
        }

        if (state->paused) {
            was_paused = 1;
            sleep_ms(10);
            continue;
        }
        if (was_paused) { was_paused = 0; wall_anchor_valid = 0; }

        int64_t aclk;
        int abuf;
        int have_aclk = audio_clock(session, aout, &aclk, &abuf);
        if (!have_aclk) abuf = 0;
        int did_work = 0;

        /* 1. Feed the audio device from the decoded-audio queue, keeping
         *    ~DEVICE_TARGET_MS queued there. Output calls happen unlocked. */
        while (abuf < DEVICE_TARGET_MS) {
            qitem_t it;
            int got = 0, stale = 0;
            pthread_mutex_lock(&e->lock);
            if (e->aq_count > 0) {
                aq_pop(e, &it);
                got = 1;
                stale = it.serial != e->serial;
                pthread_cond_signal(&e->cond);
            }
            pthread_mutex_unlock(&e->lock);
            if (!got) break;
            if (!stale && aout) {
                if (aout->present(session->aout_ctx, &it.frame) != 0) stop_requested = 1;
                abuf += (int)audio_ms(&it.frame);
            }
            it.dec->frame_free(&it.frame);
            did_work = 1;
        }
        /* Read the clock again AFTER feeding: right after a seek (or at
         * the start) the first read is still "no audio yet", and deciding
         * video timing on it showed the first frame ~165 ms early. */
        have_aclk = audio_clock(session, aout, &aclk, &abuf);
        if (!have_aclk) abuf = 0;
        if (!vout && aout && have_aclk) state->position_ms = aclk < 0 ? 0 : aclk;

        /* Publish the audio clock for the decode thread's skip policy. */
        pthread_mutex_lock(&e->lock);
        e->clk_valid = have_aclk;
        e->clk_ms = aclk;
        e->clk_at = now_ms();
        e->clk_buf_ms = abuf;
        e->clk_running = have_aclk && abuf > 0;
        int serial = e->serial;
        pthread_mutex_unlock(&e->lock);

        /* 2. Show the next video frame if its moment has come -- by the
         *    audio clock when there is one (audio is the master clock),
         *    else by the wall clock. Late frames are skipped if newer ones
         *    are waiting. */
        for (;;) {
            qitem_t it;
            int got = 0, stale = 0, more = 0;
            int64_t now = now_ms();
            pthread_mutex_lock(&e->lock);
            if (e->vq_count > 0) {
                qitem_t *h = &e->vq[e->vq_head];
                stale = h->serial != e->serial;
                int64_t clk;
                int use_audio = have_aclk && (abuf > 0 || state->paused);
                if (use_audio)               clk = aclk;
                else if (wall_anchor_valid)  clk = now - wall_anchor;
                else                         clk = h->frame.pts;   /* first frame: show now, anchor on it */
                int64_t d = h->frame.pts - clk;
                if (stale || !vout || d <= 2) {
                    vq_pop(e, &it);
                    got = 1;
                    more = e->vq_count > 0;
                    pthread_cond_signal(&e->cond);
                    if (!stale && vout && -d > LUMEN_VIDEO_LATE_DROP_MS && more) stale = 2;  /* late: drop */
                }
            }
            pthread_mutex_unlock(&e->lock);
            if (!got) break;
            did_work = 1;
            if (stale == 2) out_stats->video_dropped++;
            if (!stale) {
                state->position_ms = it.frame.pts;
                if (vout) {
                    /* Keep the wall anchor continuous so pacing carries on
                     * seamlessly if the audio clock goes away. */
                    wall_anchor = now - it.frame.pts;
                    wall_anchor_valid = 1;
                    state->video_sync_external = 1;   /* already timed: show immediately */
                    if (vout->present(session->vout_ctx, &it.frame) != 0) stop_requested = 1;
                }
            }
            it.dec->frame_free(&it.frame);
            if (!stale) break;        /* at most one frame shown per pass */
        }

        /* 3. Finished? Decoder drained, queues empty, last audio played. */
        pthread_mutex_lock(&e->lock);
        int done = e->eof && e->dec_serial == serial && e->vq_count == 0 && e->aq_count == 0;
        pthread_mutex_unlock(&e->lock);
        if (done && abuf <= 0) break;

        if (!did_work) sleep_ms(2);
    }

    /* ---- stop the decode thread, then clean up ---- */
    if (e->thread_started) {
        pthread_mutex_lock(&e->lock);
        e->abort = 1;
        pthread_cond_broadcast(&e->cond);
        pthread_mutex_unlock(&e->lock);
        pthread_join(e->thread, NULL);
    }
    queues_clear(e);      /* frames never shown -- before their decoders close */
    pthread_cond_destroy(&e->cond);
    pthread_mutex_destroy(&e->lock);
    out_stats->frames_decoded = e->frames_decoded;
    out_stats->video_frames = e->video_frames;
    out_stats->audio_frames = e->audio_frames;
    out_stats->frames_failed = e->frames_failed;

    printf("lumen: done -- %d video frame(s) (%d dropped as late), %d audio frame(s), %d failed%s\n",
           out_stats->video_frames, out_stats->video_dropped, out_stats->audio_frames, out_stats->frames_failed,
           stop_requested ? " (stopped by output)" : "");

    /* Note: deliberately NOT closing vout/aout here -- the session owns
     * their lifecycle now. Only this file's demuxer/decoders go away. */
    for (int i = 0; i < LUMEN_MAX_STREAMS; i++) {
        if (streams[i].in_use) {
            streams[i].dec_vt->close(streams[i].dec_ctx);
            lumen_unload_plugin(&streams[i].decoder_plugin);
        }
    }
    dmx->close(dctx);
    lumen_unload_plugin(&demux_plugin);
    for (int i = 0; i < LUMEN_MAX_SUB_TRACKS; i++) lumen_sub_track_free(&sub_tracks[i]);
    reset_subtitle_state(state);

    state->has_file = 0;     /* back to idle rendering once the caller goes there */
    state->position_ms = 0;  /* reset so seek bar and timers show 0:00 in idle */
    state->duration_ms = 0;
    return 0;
}
