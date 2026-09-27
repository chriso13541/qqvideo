/*
 * player_core.h
 *
 * Session-based API: open persistent video/audio outputs ONCE (this is
 * what makes the app launch into a real, visible "blank player" like
 * VLC's idle window, rather than only existing for the duration of one
 * file), then play files into that same session one at a time,
 * returning to an idle state between them instead of exiting.
 *
 * Typical use (see main.c):
 *   session = lumen_session_open(reg);
 *   while (running) {
 *     if (have a file to play) {
 *       lumen_session_play_file(session, path, &stats);
 *       // ends on EOF, error, Open-File, or Quit
 *     } else {
 *       result = lumen_session_idle(session); // blocks until something happens
 *       if (result == LUMEN_IDLE_QUIT) break;
 *       if (result == LUMEN_IDLE_OPEN_FILE) path = prompt_for_file();
 *     }
 *   }
 *   lumen_session_close(session);
 */
#ifndef LUMEN_PLAYER_CORE_H
#define LUMEN_PLAYER_CORE_H

#include "registry.h"

typedef struct lumen_session lumen_session_t;

typedef struct {
    int frames_decoded;   /* video + audio combined */
    int video_frames;
    int audio_frames;
    int frames_failed;
    int video_dropped;    /* video frames skipped for arriving too late vs. the audio clock */
    /* Set if playback stopped because the user clicked File > Open
     * File... (not a real quit) -- the caller's loop uses this to
     * decide whether to prompt for a new file and play it, vs. going
     * back to the idle state because the file simply ended, vs.
     * actually exiting (quit_requested, checked separately via
     * lumen_session_is_quit_requested()). */
    int open_file_requested;

    /* Set if playback stopped because the user clicked Stop -- main.c
     * calls queue_clear() and returns to idle rather than advancing. */
    int stop_requested;

    /* >= 0 if playback stopped because the user double-clicked a queue item */
    int queue_jump_index;
} lumen_play_stats_t;

/* Opens persistent video/audio output plugins (whichever are installed
 * -- first found, same policy as decoder/demuxer selection elsewhere)
 * ONCE, before any file is loaded. Requires a working VIDEO output to
 * succeed (no display, no window, no point to the persistent-window
 * feature at all) -- audio failing to open is fine and degrades
 * gracefully, same as it always has. Returns NULL on failure. */
lumen_session_t *lumen_session_open(const lumen_registry_t *reg);

typedef enum {
    LUMEN_IDLE_OPEN_FILE,    /* user chose File > Open File... */
    LUMEN_IDLE_QUIT,         /* user quit / closed the window while idle */
    LUMEN_IDLE_QUEUE_UPDATED /* pending_add_count > 0: items were added to queue
                               * via "Add to Queue" while idle -- caller should drain
                               * and let the main loop handle auto-start */
} lumen_idle_result_t;

/* Pumps UI/events with no file loaded (a real, visible, interactive
 * "blank player"), blocking with small internal sleeps until the user
 * does something that ends the idle state. */
lumen_idle_result_t lumen_session_idle(lumen_session_t *session);

/* Plays one file using the session's already-open outputs,
 * reconfiguring them for this file's actual parameters via
 * load_stream(). Returns when the file ends, fails, or the user
 * requests something via the UI (Open File or Quit) -- either way,
 * the session's outputs stay open afterward; only player_core.c's
 * per-file demuxer/decoders get torn down, not the window/device. */
int lumen_session_play_file(lumen_session_t *session, const char *path, lumen_play_stats_t *out_stats);

/* Returns a pointer to the session's shared playback state. Used by
 * main.c to read/write queue state (pending additions, queue display
 * arrays) without needing a separate dedicated API for each field. */
lumen_playback_state_t *lumen_session_state(lumen_session_t *session);

/* True if Quit was requested (window closed, ESC, or File > Quit) at
 * any point -- checked by the caller's loop after idle or play_file
 * returns, since either can be where the user actually quit from. */
int lumen_session_is_quit_requested(const lumen_session_t *session);

/* Tears down the video/audio outputs for real -- call once, on actual
 * app exit. */
void lumen_session_close(lumen_session_t *session);

#endif
