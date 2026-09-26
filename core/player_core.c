#include "player_core.h"
#include "plugin_loader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
  #include <windows.h>
  static void sleep_ms(int ms) { Sleep((DWORD)ms); }
#else
  #include <unistd.h>
  static void sleep_ms(int ms) { usleep((unsigned int)ms * 1000); }
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
} stream_state_t;

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

static int route_frame(const lumen_frame_t *frame,
                        const lumen_video_output_vtable_t *vout, lumen_output_ctx_t *vout_ctx,
                        const lumen_audio_output_vtable_t *aout, lumen_output_ctx_t *aout_ctx,
                        lumen_playback_state_t *state,
                        lumen_play_stats_t *stats) {
    int stop = 0;
    if (frame->type == LUMEN_STREAM_VIDEO) {
        state->position_ms = frame->pts;
        if (vout && vout->present(vout_ctx, frame) != 0) stop = 1;
        stats->video_frames++;
    } else {
        if (aout && aout->present(aout_ctx, frame) != 0) stop = 1;
        stats->audio_frames++;
    }
    stats->frames_decoded++;
    return stop;
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

    for (int i = 0; i < table.stream_count; i++) {
        const lumen_stream_desc_t *sd = &table.streams[i];
        const char *type_name = (sd->type == LUMEN_STREAM_VIDEO) ? "video" : "audio";

        if (sd->stream_index < 0 || sd->stream_index >= LUMEN_MAX_STREAMS) {
            fprintf(stderr, "lumen: stream %d index out of range, skipping\n", i);
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

    lumen_packet_t pkt;
    int stop_requested = 0;
    memset(&pkt, 0, sizeof(pkt));

    while (!stop_requested) {
        /* ALWAYS call the session's video output pump_ui to keep the SDL
         * event loop running. If we guarded this with (vout && ...) then
         * when no video decoder was found (e.g., H.265 not installed),
         * vout is NULL, pump_ui is never called, and the window freezes
         * indefinitely -- the "missing codec hang" bug. The session always
         * has a vout; it's only the file-specific local vout that may be
         * NULL when this file had no decodable video stream. */
        if (session->have_vout && session->vout->pump_ui &&
            session->vout->pump_ui(session->vout_ctx, state)) {
            stop_requested = 1;
            if (state) state->quit_requested = 1;
            break;
        }
        if (aout && aout->pump_ui && aout->pump_ui(session->aout_ctx, state)) {
            stop_requested = 1;
            break;
        }
        if (state->quit_requested) {
            stop_requested = 1;
            break;
        }
        if (state->open_file_requested) {
            stop_requested = 1;
            out_stats->open_file_requested = 1;
            break;
        }
        if (state->queue_jump_requested) {
            stop_requested = 1;
            out_stats->queue_jump_index = state->queue_jump_index;
            state->queue_jump_requested = 0;
            break;
        }
        if (state->stop_requested) {
            stop_requested = 1;
            out_stats->stop_requested = 1;
            state->stop_requested = 0;
            break;
        }

        if (state->seek_requested) {
            if (dmx->seek) {
                if (dmx->seek(dctx, state->seek_target_ms) == 0) {
                    for (int i = 0; i < LUMEN_MAX_STREAMS; i++) {
                        if (streams[i].in_use && streams[i].dec_vt->flush) {
                            streams[i].dec_vt->flush(streams[i].dec_ctx, state->seek_target_ms);
                        }
                    }
                    state->position_ms = state->seek_target_ms;
                    state->seek_generation++;
                } else {
                    fprintf(stderr, "lumen: seek to %lldms failed\n", (long long)state->seek_target_ms);
                }
            } else {
                fprintf(stderr, "lumen: this demuxer doesn't support seeking -- ignoring request\n");
            }
            state->seek_requested = 0;
            dmx->packet_free(&pkt);
            memset(&pkt, 0, sizeof(pkt));
            continue;
        }

        if (state->paused) {
            sleep_ms(10);
            continue;
        }

        int rc = dmx->read_packet(dctx, &pkt);
        if (rc != 0) break; /* EOF or unrecoverable error */

        if (pkt.stream_index < 0 || pkt.stream_index >= LUMEN_MAX_STREAMS || !streams[pkt.stream_index].in_use) {
            dmx->packet_free(&pkt);
            memset(&pkt, 0, sizeof(pkt));
            continue;
        }
        stream_state_t *ss = &streams[pkt.stream_index];
        lumen_frame_t frame;
        int drc = ss->dec_vt->decode(ss->dec_ctx, &pkt, &frame);
        if (drc == 0) {
            if (route_frame(&frame, vout, session->vout_ctx, aout, session->aout_ctx, state, out_stats)) stop_requested = 1;
            ss->dec_vt->frame_free(&frame);
        } else if (drc < 0) {
            out_stats->frames_failed++;
        }
        dmx->packet_free(&pkt);
        memset(&pkt, 0, sizeof(pkt));
    }

    if (!stop_requested) {
        for (int i = 0; i < LUMEN_MAX_STREAMS; i++) {
            stream_state_t *ss = &streams[i];
            if (!ss->in_use || !ss->dec_vt->drain) continue;
            lumen_frame_t frame;
            while (!stop_requested && ss->dec_vt->drain(ss->dec_ctx, &frame) == 0) {
                if (route_frame(&frame, vout, session->vout_ctx, aout, session->aout_ctx, state, out_stats)) stop_requested = 1;
                ss->dec_vt->frame_free(&frame);
            }
        }
    }

    printf("lumen: done -- %d video frame(s), %d audio frame(s), %d failed%s\n",
           out_stats->video_frames, out_stats->audio_frames, out_stats->frames_failed,
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

    state->has_file = 0;     /* back to idle rendering once the caller goes there */
    state->position_ms = 0;  /* reset so seek bar and timers show 0:00 in idle */
    state->duration_ms = 0;
    return 0;
}
