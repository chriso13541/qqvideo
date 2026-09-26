/*
 * main.c -- lumen-play
 *
 * Usage:
 *   lumen-play [--list]
 *   lumen-play <file>
 *
 * The app finds its packages/ directory automatically next to the exe.
 * No --plugins flag or manual manifest copying is needed.
 *
 * With no arguments: launches into a persistent blank-player window
 * (like VLC's startup state) with the queue empty. The user loads files
 * via File > Open File... or File > Add to Queue... from the menu.
 *
 * Queue semantics:
 *   - "Open File..." plays a file immediately (as before), and adds
 *     it to the queue at the current position so it shows up there.
 *   - "Add to Queue..." adds a file to the END of the queue without
 *     interrupting current playback (the output plugin writes it to
 *     pending_add_paths; main.c drains that between files).
 *   - When a file ends naturally, the next queue item plays
 *     automatically (auto-advance). When the queue is exhausted, the
 *     app returns to idle.
 *   - Double-clicking a queue item in the UI jumps to it immediately.
 */
#include "registry.h"
#include "packages.h"
#include "player_core.h"
#include "../third_party/tinyfiledialogs/tinyfiledialogs.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#if defined(_WIN32)
  #include <windows.h>
#elif defined(__linux__)
  #include <unistd.h>
#endif

/* ---- Exe-relative plugins discovery ---------------------------------- */

static void get_exe_dir(char *buf, size_t bufsize) {
#if defined(_WIN32)
    char exe_path[1024];
    DWORD len = GetModuleFileNameA(NULL, exe_path, (DWORD)sizeof(exe_path));
    if (len > 0) {
        char *sep = strrchr(exe_path, '\\');
        if (!sep) sep = strrchr(exe_path, '/');
        if (sep) *sep = '\0';
        strncpy(buf, exe_path, bufsize - 1);
        buf[bufsize - 1] = '\0';
        return;
    }
#elif defined(__linux__)
    char exe_path[1024];
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len > 0) {
        exe_path[len] = '\0';
        char *sep = strrchr(exe_path, '/');
        if (sep) *sep = '\0';
        strncpy(buf, exe_path, bufsize - 1);
        buf[bufsize - 1] = '\0';
        return;
    }
#endif
    strncpy(buf, ".", bufsize - 1);
    buf[bufsize - 1] = '\0';
}

/* ---- File dialog helper ---------------------------------------------- */

static char *prompt_for_file(void) {
    static const char *patterns[] = {
        "*.mp4", "*.m4v", "*.mov", "*.m4a",
        "*.mkv", "*.webm", "*.avi",
        "*.h264", "*.264", "*.aac"
    };
    char *selected = tinyfd_openFileDialog(
        "Lumen -- Open Media File", "",
        sizeof(patterns) / sizeof(patterns[0]),
        patterns, "Media files", 0);
    if (!selected) return NULL;
    char *copy = (char *)malloc(strlen(selected) + 1);
    strcpy(copy, selected);
    return copy;
}

/* ---- Queue ----------------------------------------------------------- */

#define QUEUE_MAX 256

static char *q_paths[QUEUE_MAX];       /* heap-allocated full paths */
static char *q_names[QUEUE_MAX];       /* heap-allocated basenames for display */
static int   q_len  = 0;
static int   q_pos  = -1;             /* currently playing item; -1 while idle */
static int   q_exhausted = 0;         /* set when the queue plays through to the end;
                                        * prevents the q_pos<0 && q_len>0 branch from
                                        * immediately restarting from item 0, which was
                                        * the root cause of the last-item-loops bug */

static const char *basename_of(const char *path) {
    const char *s = strrchr(path, '/');
    if (!s) s = strrchr(path, '\\');
    return s ? s + 1 : path;
}

/* Appends path to the end of the queue. Returns the new item's index,
 * or -1 if the queue is full. Does NOT change q_pos. */
static int queue_append(const char *path) {
    if (q_len >= QUEUE_MAX) return -1;
    q_paths[q_len] = strdup(path);
    q_names[q_len] = strdup(basename_of(path));
    q_exhausted = 0; /* new items mean we're no longer exhausted */
    return q_len++;
}

/* Inserts path at position `at`, shifting later items down.
 * Returns the inserted index, or -1 if the queue is full. */
static int queue_insert(const char *path, int at) {
    if (q_len >= QUEUE_MAX) return -1;
    if (at < 0) at = 0;
    if (at > q_len) at = q_len;
    memmove(q_paths + at + 1, q_paths + at, (q_len - at) * sizeof(*q_paths));
    memmove(q_names + at + 1, q_names + at, (q_len - at) * sizeof(*q_names));
    q_paths[at] = strdup(path);
    q_names[at] = strdup(basename_of(path));
    q_exhausted = 0; /* same as queue_append: new items clear exhausted state */
    q_len++;
    return at;
}

/* Clears the entire queue, freeing all paths. Used by "Open File" to
 * start a fresh queue: the user is explicitly saying "play THIS file",
 * so everything queued up before it should be discarded. */
static void queue_clear(void) {
    for (int i = 0; i < q_len; i++) {
        free(q_paths[i]);
        free(q_names[i]);
        q_paths[i] = NULL;
        q_names[i] = NULL;
    }
    q_len = 0;
    q_pos = -1;
    q_exhausted = 0;
}

/* Drains pending_add_paths from the playback state into the queue and
 * clears the pending buffer. Called between every file play so the
 * output plugin's "Add to Queue" dialog results are processed without
 * needing to interrupt current playback. */
static void drain_pending_adds(lumen_session_t *session) {
    /* Access the shared playback state via a pointer we keep to it.
     * The session holds the state; we reach it via the session API. */
    lumen_playback_state_t *state = lumen_session_state(session);
    for (int i = 0; i < state->pending_add_count; i++) {
        if (state->pending_add_paths[i][0]) {
            queue_append(state->pending_add_paths[i]);
        }
    }
    state->pending_add_count = 0;
}

/* Pushes current queue metadata into playback_state so the output
 * plugin can read it for display -- must be called before every
 * idle/play invocation since main.c owns the queue data. */
static void push_queue_state(lumen_session_t *session) {
    lumen_playback_state_t *state = lumen_session_state(session);
    state->queue_names = (const char * const *)q_names;
    state->queue_count = q_len;
    state->queue_position = q_pos;
}

/* ---- Entry point ----------------------------------------------------- */

/* Converts the packages scan result into the flat pkg_entries[] array that
 * output_sdl2.cpp reads for the Package Manager UI. Kept entirely in C so
 * it never needs to be called from C++ code (no lumen_plugin.h typedef
 * visibility issues). Called after every lumen_packages_scan(). */
static void push_pkg_entries(lumen_session_t *session,
                              const lumen_packages_t *pkgs) {
    lumen_playback_state_t *st = lumen_session_state(session);
    st->pkg_entry_count = 0;

    for (int i = 0; i < pkgs->demuxer_count && st->pkg_entry_count < LUMEN_PKG_UI_MAX; i++) {
        const lumen_pkg_demuxer_t *d = &pkgs->demuxers[i];
        lumen_pkg_ui_entry_t *e = &st->pkg_entries[st->pkg_entry_count++];
        strncpy(e->name,     d->display_name, sizeof(e->name)-1);
        strncpy(e->detail,   d->extensions,   sizeof(e->detail)-1);
        strncpy(e->version,  d->version,      sizeof(e->version)-1);
        strncpy(e->dir_path, d->dir_path,     sizeof(e->dir_path)-1);
        strncpy(e->plugin,   d->plugin,       sizeof(e->plugin)-1);
        e->type      = LUMEN_PKG_UI_DEMUXER;
        e->installed = d->installed;
    }
    for (int i = 0; i < pkgs->video_codec_count && st->pkg_entry_count < LUMEN_PKG_UI_MAX; i++) {
        const lumen_pkg_codec_t *k = &pkgs->video_codecs[i];
        lumen_pkg_ui_entry_t *e = &st->pkg_entries[st->pkg_entry_count++];
        strncpy(e->name,     k->display_name, sizeof(e->name)-1);
        strncpy(e->detail,   k->fourcc,       sizeof(e->detail)-1);
        strncpy(e->version,  k->version,      sizeof(e->version)-1);
        strncpy(e->dir_path, k->dir_path,     sizeof(e->dir_path)-1);
        strncpy(e->plugin,   k->plugin,       sizeof(e->plugin)-1);
        e->type      = LUMEN_PKG_UI_VIDEO;
        e->installed = k->installed;
    }
    for (int i = 0; i < pkgs->audio_codec_count && st->pkg_entry_count < LUMEN_PKG_UI_MAX; i++) {
        const lumen_pkg_codec_t *k = &pkgs->audio_codecs[i];
        lumen_pkg_ui_entry_t *e = &st->pkg_entries[st->pkg_entry_count++];
        strncpy(e->name,     k->display_name, sizeof(e->name)-1);
        strncpy(e->detail,   k->fourcc,       sizeof(e->detail)-1);
        strncpy(e->version,  k->version,      sizeof(e->version)-1);
        strncpy(e->dir_path, k->dir_path,     sizeof(e->dir_path)-1);
        strncpy(e->plugin,   k->plugin,       sizeof(e->plugin)-1);
        e->type      = LUMEN_PKG_UI_AUDIO;
        e->installed = k->installed;
    }
}

int main(int argc, char **argv) {
    char exe_dir[1024];
    char auto_packages_dir[1100];
    get_exe_dir(exe_dir, sizeof(exe_dir));
    snprintf(auto_packages_dir, sizeof(auto_packages_dir), "%s/packages", exe_dir);

    const char *cli_file = NULL;
    int         list_only = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--list") == 0) {
            list_only = 1;
        } else if (argv[i][0] != '-') {
            cli_file = argv[i];
        }
    }

    lumen_registry_t reg;
    memset(&reg, 0, sizeof(reg));

    /* Scan the packages/ directory -- this is the single source of truth for
     * both the Package Manager UI and the registry used by the player.
     * No separate plugins/ directory is needed or used. */
    lumen_packages_t pkgs;
    int npkg = lumen_packages_scan(auto_packages_dir, &pkgs);
    if (npkg < 0) {
        fprintf(stderr, "lumen: packages directory not found: %s\n", auto_packages_dir);
        fprintf(stderr, "       (auto-detected from exe: %s)\n", exe_dir);
        /* Non-fatal -- user can still open the app, just can't play anything */
    }

    /* Build the registry from installed packages. Demuxers and codecs are
     * separate -- codecs work with any container, so no duplication. */
    int reg_count = 0;
    for (int i = 0; i < pkgs.demuxer_count; i++) {
        const lumen_pkg_demuxer_t *d = &pkgs.demuxers[i];
        if (d->installed && d->manifest_path[0] && d->dll_path[0])
            if (lumen_registry_add_from_manifest(&reg, d->manifest_path, d->dll_path) == 0)
                reg_count++;
    }
    for (int i = 0; i < pkgs.video_codec_count; i++) {
        const lumen_pkg_codec_t *k = &pkgs.video_codecs[i];
        if (k->installed && k->manifest_path[0] && k->dll_path[0])
            if (lumen_registry_add_from_manifest(&reg, k->manifest_path, k->dll_path) == 0)
                reg_count++;
    }
    for (int i = 0; i < pkgs.audio_codec_count; i++) {
        const lumen_pkg_codec_t *k = &pkgs.audio_codecs[i];
        if (k->installed && k->manifest_path[0] && k->dll_path[0])
            if (lumen_registry_add_from_manifest(&reg, k->manifest_path, k->dll_path) == 0)
                reg_count++;
    }
    printf("lumen: loaded %d codec/demuxer package(s) from '%s'\n\n",
           reg_count, auto_packages_dir);

    if (list_only) {
        lumen_registry_print(&reg);
        lumen_packages_print(&pkgs);
        lumen_registry_free(&reg);
        return 0;
    }

    lumen_session_t *session = lumen_session_open(&reg);
    if (!session) {
        fprintf(stderr, "lumen: could not open a player session (no usable video output)\n");
        lumen_registry_free(&reg);
        return 1;
    }

    /* Snapshot registry data into the shared state for the Package Manager UI. */
    {
        lumen_playback_state_t *st = lumen_session_state(session);
        st->installed_plugin_count = 0;
        for (int i = 0; i < reg.count && i < LUMEN_MAX_PLUGIN_INFOS; i++) {
            const lumen_registry_entry_t *e = &reg.entries[i];
            lumen_plugin_info_t *info = &st->installed_plugins[i];
            strncpy(info->name,    e->name,    sizeof(info->name) - 1);
            strncpy(info->version, e->version, sizeof(info->version) - 1);
            switch (e->kind) {
                case LUMEN_PLUGIN_DEMUXER:      strncpy(info->kind_str, "Demuxer",   sizeof(info->kind_str)-1); break;
                case LUMEN_PLUGIN_DECODER:      strncpy(info->kind_str, "Decoder",   sizeof(info->kind_str)-1); break;
                case LUMEN_PLUGIN_OUTPUT_VIDEO: strncpy(info->kind_str, "Video Out", sizeof(info->kind_str)-1); break;
                case LUMEN_PLUGIN_OUTPUT_AUDIO: strncpy(info->kind_str, "Audio Out", sizeof(info->kind_str)-1); break;
                default:                        strncpy(info->kind_str, "Other",     sizeof(info->kind_str)-1); break;
            }
            info->claims[0] = '\0';
            for (int j = 0; j < e->fourcc_count; j++) {
                if (j > 0) strncat(info->claims, ", ", sizeof(info->claims) - strlen(info->claims) - 1);
                strncat(info->claims, e->fourccs[j], sizeof(info->claims) - strlen(info->claims) - 1);
            }
            for (int j = 0; j < e->extension_count; j++) {
                if (strlen(info->claims) > 0)
                    strncat(info->claims, ", ", sizeof(info->claims) - strlen(info->claims) - 1);
                strncat(info->claims, e->extensions[j], sizeof(info->claims) - strlen(info->claims) - 1);
            }
            st->installed_plugin_count++;
        }
        /* Store the packages directory path so the Package Manager window
         * can call lumen_packages_scan() itself and get real filesystem state. */
        strncpy(st->packages_dir, pkgs.packages_dir, sizeof(st->packages_dir) - 1);
    }

    push_pkg_entries(session, &pkgs);

    /* If a file was given on the command line, pre-load the queue with
     * it so the first idle loop iteration immediately starts playing. */
    if (cli_file) {
        queue_append(cli_file);
    }

    int rc = 0;

    while (1) {
        /* Always drain pending queue additions (from output plugin's
         * "Add to Queue" dialogs) and push current queue state to the
         * output plugin BEFORE every decision, so the UI and main.c
         * are always looking at the same picture. */
        drain_pending_adds(session);
        push_queue_state(session);

        /* Package Manager requested a rescan (user clicked Install...) */
        {
            lumen_playback_state_t *st = lumen_session_state(session);
            if (st->rescan_packages_requested) {
                st->rescan_packages_requested = 0;
                lumen_packages_rescan(&pkgs);
                /* Re-register any newly installed packages in the registry */
                lumen_registry_free(&reg);
                memset(&reg, 0, sizeof(reg));
                for (int i = 0; i < pkgs.demuxer_count; i++) {
                    const lumen_pkg_demuxer_t *d = &pkgs.demuxers[i];
                    if (d->installed && d->manifest_path[0])
                        lumen_registry_add_from_manifest(&reg, d->manifest_path, d->dll_path);
                }
                for (int i = 0; i < pkgs.video_codec_count; i++) {
                    const lumen_pkg_codec_t *k = &pkgs.video_codecs[i];
                    if (k->installed && k->manifest_path[0])
                        lumen_registry_add_from_manifest(&reg, k->manifest_path, k->dll_path);
                }
                for (int i = 0; i < pkgs.audio_codec_count; i++) {
                    const lumen_pkg_codec_t *k = &pkgs.audio_codecs[i];
                    if (k->installed && k->manifest_path[0])
                        lumen_registry_add_from_manifest(&reg, k->manifest_path, k->dll_path);
                }
                push_pkg_entries(session, &pkgs);
            }
        }

        /* --- Decide what to do next --- */

        if (q_pos < 0 && q_len == 0) {
            /* Queue empty -- sit in the real idle window until the user
             * does something. This is the VLC-style blank-startup state:
             * no file dialog is forced on the user; they choose. */
            lumen_idle_result_t idle = lumen_session_idle(session);
            drain_pending_adds(session); /* process any adds that happened while idle */
            push_queue_state(session);

            if (idle == LUMEN_IDLE_QUIT) break;
            if (idle == LUMEN_IDLE_QUEUE_UPDATED) continue; /* main loop auto-start handles it */

            /* LUMEN_IDLE_OPEN_FILE: the dialog ran in a background thread
             * (output_sdl2.cpp) and already wrote the selected path to
             * state->open_file_pending_path before setting open_file_requested.
             * Read it here -- calling prompt_for_file() again would open a
             * SECOND blocking dialog, which is the "open file twice" bug. */
            {
                lumen_playback_state_t *st = lumen_session_state(session);
                char *picked = NULL;
                if (st->open_file_pending_path[0] != '\0') {
                    picked = strdup(st->open_file_pending_path);
                    st->open_file_pending_path[0] = '\0';
                }
                if (!picked) continue; /* cancelled -- back to idle */
                queue_clear();
                queue_append(picked);
                q_pos = 0;
                free(picked);
                push_queue_state(session);
            }

        } else if (q_pos < 0 && q_len > 0 && !q_exhausted) {
            /* Queue has items but nothing is playing yet AND the queue
             * hasn't been exhausted -- auto-start from item 0.
             * Guarded by !q_exhausted: without this guard, after the
             * last item played and q_pos was reset to -1, this branch
             * would fire immediately on the next loop iteration and
             * restart from item 0, causing the "loops forever" bug. */
            q_pos = 0;
            push_queue_state(session);

        } else if (q_pos < 0 && q_len > 0 && q_exhausted) {
            /* Queue has items but was exhausted -- the user played
             * through everything. Stay idle until they explicitly
             * open a file or add new items (which clears q_exhausted). */
            lumen_idle_result_t idle = lumen_session_idle(session);
            drain_pending_adds(session);
            push_queue_state(session);
            if (idle == LUMEN_IDLE_QUIT) break;
            if (idle == LUMEN_IDLE_QUEUE_UPDATED) continue;
            if (idle == LUMEN_IDLE_OPEN_FILE) {
                /* Same fix: path is already in open_file_pending_path. */
                lumen_playback_state_t *st = lumen_session_state(session);
                char *picked = NULL;
                if (st->open_file_pending_path[0] != '\0') {
                    picked = strdup(st->open_file_pending_path);
                    st->open_file_pending_path[0] = '\0';
                }
                if (picked) {
                    queue_clear();
                    queue_append(picked);
                    q_pos = 0;
                    free(picked);
                    push_queue_state(session);
                }
            }
            continue;

        } else if (q_pos >= q_len) {
            /* Queue exhausted -- reset position, return to true idle. */
            q_pos = -1;
            push_queue_state(session);
            continue;
        }

        /* --- Play the current queue item --- */
        const char *play_path = q_paths[q_pos];
        printf("lumen: playing queue item %d/%d: %s\n", q_pos + 1, q_len, play_path);

        lumen_play_stats_t stats;
        rc = lumen_session_play_file(session, play_path, &stats);

        /* Always drain and push after a play ends */
        drain_pending_adds(session);

        if (lumen_session_is_quit_requested(session)) break;

        /* --- Handle the reason playback stopped --- */

        if (stats.queue_jump_index >= 0) {
            /* User double-clicked a queue item -- jump there. */
            q_pos = stats.queue_jump_index;

        } else if (stats.stop_requested) {
            /* Stop button: clear the entire queue and return to the
             * idle blank-player state, same as when the app first opened. */
            queue_clear();

        } else if (stats.open_file_requested) {
            /* File > Open File... -- the path was collected by the background
             * dialog thread in output_sdl2.cpp and written to
             * state->open_file_pending_path. Use it directly rather than
             * calling prompt_for_file() (which would block the main thread
             * with a second dialog and freeze the window). */
            lumen_playback_state_t *st = lumen_session_state(session);
            char *picked = NULL;
            if (st->open_file_pending_path[0] != '\0') {
                picked = strdup(st->open_file_pending_path);
                st->open_file_pending_path[0] = '\0';
            }
            if (picked) {
                queue_clear();
                queue_append(picked);
                q_pos = 0;
                free(picked);
            }
            /* If path is empty (shouldn't normally happen since the dialog
             * only sets open_file_requested when a file was actually picked),
             * just leave q_pos as-is and re-play the current item. */

        } else {
            /* File ended naturally -- advance to the next queue item. */
            if (q_pos + 1 < q_len) {
                q_pos++;
            } else {
                /* Reached the end of the queue -- back to idle.
                 * Set q_exhausted so the "has items" branch above
                 * doesn't immediately restart from item 0. */
                q_pos = -1;
                q_exhausted = 1;
            }
        }

        push_queue_state(session);
    }

    lumen_session_close(session);

    /* Free queue memory */
    for (int i = 0; i < q_len; i++) {
        free(q_paths[i]);
        free(q_names[i]);
    }

    lumen_registry_free(&reg);
    return rc;
}
