/*
 * output_sdl2.cpp
 *
 * The video output plugin -- restructured for a PERSISTENT window
 * (open the app, get a real blank player like VLC's idle state, load
 * files into it, return to blank when one ends) instead of a window
 * that only exists for the duration of one file.
 *
 * The split that makes this possible: open() now creates the window
 * and renderer ONCE, with no file-specific dimensions at all (none are
 * known yet at app startup). load_stream() -- called once per file,
 * including the very first one -- is what (re)creates the texture
 * sized for that file's resolution and resizes the window to fit,
 * reusing the same window/renderer across as many files as get loaded
 * in this session. close() only tears anything down on actual app
 * exit, not between files.
 *
 * Idle rendering (no file loaded, ctx->texture == NULL) just clears to
 * a background color and skips the video blit -- still draws the menu
 * bar so File > Open File... is always reachable, and the control bar
 * draws degenerately-but-harmlessly (0:00 / 0:00, a Play button that
 * does nothing meaningful since there's nothing to play).
 *
 * Everything else (ImGui/SDL_Renderer backend choice, why pump_ui
 * exists separately from present, the stale-frame-drop pacing logic,
 * the pause-duration clock-shift fix) is unchanged from before this
 * restructuring -- see README for the history of why each exists.
 */
#include "../../include/lumen_plugin.h"
#include <SDL2/SDL.h>
#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"
#include "../../third_party/tinyfiledialogs/tinyfiledialogs.h"
#include "lumen_font.h"  /* Roboto font data (compiled separately in lumen_font.cpp) */

/* App icon embedded as a PNG byte array -- used for SDL_SetWindowIcon
 * (the title-bar icon). The .ico file + Windows .rc handles the taskbar. */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"
#include "qqvideo_icon.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>   /* lroundf: volume steps */

#define LUMEN_SDL2_DROP_THRESHOLD_MS 100
#define LUMEN_UI_REDRAW_INTERVAL_MS 16 /* ~60fps cap on actual redraws/presents */
#define LUMEN_IDLE_WINDOW_W 640
#define LUMEN_IDLE_WINDOW_H 360

/* Forward declarations -- pkg_render calls pkg_close, defined below it */
static void pkg_open(lumen_output_ctx_t *ctx);
static void prefs_open(lumen_output_ctx_t *ctx);
static void pkg_close(lumen_output_ctx_t *ctx);
static void pkg_render(lumen_output_ctx_t *ctx);

struct lumen_output_ctx {
    SDL_Window   *window;
    SDL_Renderer *renderer;
    SDL_Texture  *texture;        /* NULL until load_stream() -- idle state has no texture */
    int           width, height;  /* current file's video dimensions; 0 while idle */
    Uint64        start_ticks;
    int           have_start;
    int           fullscreen;
    long          frames_rendered;
    long          frames_dropped;
    Uint64        last_ui_redraw_ticks;
    int           have_last_redraw;
    lumen_playback_state_t *state; /* bound via bind_state(); NULL until then */
    float         seek_preview_ms; /* local copy while the user is actively dragging the seek bar */
    int           min_w, min_h;    /* minimum window size, computed from the control bar (0 = not yet) */
    int           have_frame;      /* texture holds a real frame (not yet after load: it's uninitialized
                                      memory, which shows up as solid green) */
    int           dragging_seek;   /* persisted across frames -- see draw_controls() for why */
    int64_t       last_seen_seek_generation;
    int           was_paused;      /* detects the pause->resume transition, not just current state */
    Uint64        pause_started_ticks;
    Uint64        last_mouse_move_ticks; /* for fullscreen auto-hide (3s timeout) */
    int           controls_visible;      /* 1 = draw chrome/cursor; 0 = hidden (fullscreen idle) */
    int           last_queue_pos;        /* detect queue-position changes to update window title */

    /* Async "Add to Queue" dialog -- runs on a background thread so the
     * decode/render/audio loop keeps going while the user picks a file.
     * Without this, tinyfd_openFileDialog blocks the entire pipeline
     * (audio device drains silently, video freezes, desync on resume).
     * Only one dialog can be in flight at a time (guarded by dialog_running).
     * Thread writes result to dialog_result_path under dialog_mutex;
     * pump_ui reads it on the main thread each frame and writes it to
     * state->pending_add_paths (which main.c drains between files). */
    SDL_Thread  *dialog_thread;
    SDL_mutex   *dialog_mutex;
    SDL_atomic_t dialog_running;       /* 1 while the dialog is open */
    SDL_atomic_t dialog_result_ready;  /* 1 when thread has a path waiting */
    char         dialog_result_path[512];
    int          dialog_purpose;       /* 0 = add_to_queue, 1 = open_file, 2 = add subtitle track */
    ImFont      *sub_font;             /* large font for subtitles; NULL = use the UI font */
    float        volume_before_mute;   /* saved for the mute/unmute toggle */

    /* ---- Package Manager (second system window) ---- */
    ImGuiContext *main_imgui_ctx; /* the primary video window's ImGui context */
    ImGuiContext *pkg_imgui_ctx;  /* separate context for the package manager */
    SDL_Window   *pkg_window;
    SDL_Renderer *pkg_renderer;
    int           pkg_window_open; /* 1 while the window is shown */

    /* ---- Preferences (third system window, same pattern) ---- */
    ImGuiContext *pref_imgui_ctx;
    SDL_Window   *pref_window;
    SDL_Renderer *pref_renderer;
    int           pref_window_open;

    /* Per-container codec disable list. Each entry blocks one (demuxer, decoder)
     * pair: e.g., {"mp4-demuxer", "h264-decoder"} means H.264 is disabled only
     * when playing MP4, not MKV. Phase 2 will enforce this in player_core.c;
     * for now it is stored here for UI state and future wiring. */
    struct PkgDisabledPair { char demuxer[64]; char codec_plugin[64]; };
    PkgDisabledPair pkg_disabled[32];
    int             pkg_disabled_count;
};

/* Background thread for any file-picker dialog (Add to Queue or Open File).
 * Runs independently of the decode/render loop. The purpose field in ctx
 * (set before spawning) tells pump_ui what to do with the result. */
static int SDLCALL file_dialog_thread(void *userdata) {
    lumen_output_ctx_t *ctx = (lumen_output_ctx_t *)userdata;

    static const char *media_patterns[] = {
        "*.mp4", "*.m4v", "*.mov", "*.m4a",
        "*.mkv", "*.webm", "*.avi",
        "*.h264", "*.264", "*.aac"
    };
    static const char *sub_patterns[] = { "*.srt", "*.ass", "*.ssa", "*.vtt" };
    char *picked;
    if (ctx->dialog_purpose == 2) {
        picked = tinyfd_openFileDialog("qqvideo -- Add Subtitle Track", "",
            (int)(sizeof(sub_patterns) / sizeof(sub_patterns[0])),
            sub_patterns, "Subtitle files", 0);
    } else {
        picked = tinyfd_openFileDialog(
            ctx->dialog_purpose == 1 ? "qqvideo -- Open File" : "qqvideo -- Add to Queue", "",
            (int)(sizeof(media_patterns) / sizeof(media_patterns[0])),
            media_patterns, "Media files", 0);
    }

    /* Immediate stack copy from tinyfd's static buffer -- narrows the
     * race window with any concurrent tinyfd call to single-instruction width. */
    char local[512];
    if (picked) {
        strncpy(local, picked, 511);
        local[511] = '\0';
    } else {
        local[0] = '\0'; /* user cancelled */
    }

    SDL_LockMutex(ctx->dialog_mutex);
    strncpy(ctx->dialog_result_path, local, 511);
    ctx->dialog_result_path[511] = '\0';
    SDL_UnlockMutex(ctx->dialog_mutex);

    SDL_AtomicSet(&ctx->dialog_result_ready, 1);
    SDL_AtomicSet(&ctx->dialog_running, 0);
    return 0;
}

static void toggle_fullscreen(lumen_output_ctx_t *ctx) {
    ctx->fullscreen = !ctx->fullscreen;
    SDL_SetWindowFullscreen(ctx->window, ctx->fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    /* Controls always start visible on any transition:
     * entering fullscreen → user just clicked something, show chrome
     * leaving fullscreen  → always show chrome in windowed mode */
    ctx->last_mouse_move_ticks = SDL_GetTicks64();
    ctx->controls_visible = 1;
    SDL_ShowCursor(SDL_ENABLE);
}

static void format_ms(int64_t ms, char *out, size_t out_size) {
    if (ms < 0) ms = 0;
    int64_t total_sec = ms / 1000;
    int h = (int)(total_sec / 3600);
    int m = (int)((total_sec % 3600) / 60);
    int s = (int)(total_sec % 60);
    if (h > 0) snprintf(out, out_size, "%d:%02d:%02d", h, m, s);
    else snprintf(out, out_size, "%d:%02d", m, s);
}

static int handle_events(lumen_output_ctx_t *ctx) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        /* Route events to the correct ImGui context based on window ID.
         * Each context stores its backend data independently, so we must
         * switch before calling ImGui_ImplSDL2_ProcessEvent. */
        Uint32 evt_win_id = 0;
        if (e.type == SDL_WINDOWEVENT)  evt_win_id = e.window.windowID;
        else if (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) evt_win_id = e.key.windowID;
        else if (e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP) evt_win_id = e.button.windowID;
        else if (e.type == SDL_MOUSEMOTION)     evt_win_id = e.motion.windowID;
        else if (e.type == SDL_MOUSEWHEEL)      evt_win_id = e.wheel.windowID;

        Uint32 pkg_win_id = ctx->pkg_window ? SDL_GetWindowID(ctx->pkg_window) : 0;
        Uint32 pref_win_id = ctx->pref_window ? SDL_GetWindowID(ctx->pref_window) : 0;
        bool for_pref = (pref_win_id != 0 && evt_win_id == pref_win_id);
        /* for_pkg = "belongs to an auxiliary window, not the video window";
         * the main-window key/mouse handling below checks it. */
        bool for_pkg = (pkg_win_id != 0 && evt_win_id == pkg_win_id) || for_pref;

        ImGuiContext *target = for_pref ? ctx->pref_imgui_ctx
                             : (for_pkg ? ctx->pkg_imgui_ctx : NULL);
        if (target) {
            ImGui::SetCurrentContext(target);
            ImGui_ImplSDL2_ProcessEvent(&e);
            ImGui::SetCurrentContext(ctx->main_imgui_ctx);
        } else {
            ImGui_ImplSDL2_ProcessEvent(&e);
        }

        /* Auxiliary window X button → hide it (don't quit the whole app) */
        if (e.type == SDL_WINDOWEVENT && for_pkg &&
            e.window.event == SDL_WINDOWEVENT_CLOSE) {
            if (for_pref) { SDL_HideWindow(ctx->pref_window); ctx->pref_window_open = 0; }
            else          pkg_close(ctx);
            continue;
        }

        /* Main window X button OR SDL_QUIT → quit.
         * When the package manager window exists (even hidden), SDL has two
         * tracked windows and does NOT send SDL_QUIT when just the main window
         * is closed -- it only sends SDL_WINDOWEVENT_CLOSE for that window.
         * We must check for that event explicitly, otherwise the app hangs
         * indefinitely waiting for a SDL_QUIT that never arrives. */
        bool is_main_window_close = (e.type == SDL_WINDOWEVENT &&
                                     !for_pkg &&
                                     e.window.event == SDL_WINDOWEVENT_CLOSE &&
                                     e.window.windowID == SDL_GetWindowID(ctx->window));
        if (e.type == SDL_QUIT || is_main_window_close) return 1;
        if (e.type == SDL_MOUSEMOTION && !for_pkg) {
            ctx->last_mouse_move_ticks = SDL_GetTicks64();
            if (!ctx->controls_visible) {
                ctx->controls_visible = 1;
                SDL_ShowCursor(SDL_ENABLE);
                ctx->have_last_redraw = 0;
            }
        }
        if (e.type == SDL_KEYDOWN && !for_pkg) {
            if (e.key.keysym.scancode == SDL_SCANCODE_ESCAPE) {
                if (ctx->fullscreen) toggle_fullscreen(ctx);
            }
            if (e.key.keysym.scancode == SDL_SCANCODE_F || e.key.keysym.scancode == SDL_SCANCODE_F11) {
                toggle_fullscreen(ctx);
            }
            /* Left/Right: jump 10 s. Up/Down: volume +/-5%. (VLC/mpv keys)
             * Seeks go in the direction of travel (see seek_ex): with
             * keyframes every ~10 s, "position +10 s, snapped back to the
             * keyframe before it" could land where you started. The new
             * position is the seek target right away, so repeated presses
             * (or holding the key) keep adding up. */
            if ((e.key.keysym.scancode == SDL_SCANCODE_RIGHT || e.key.keysym.scancode == SDL_SCANCODE_LEFT) &&
                !ImGui::GetIO().WantCaptureKeyboard &&
                ctx->state && ctx->state->has_file) {
                lumen_playback_state_t *st = ctx->state;
                int fwd = e.key.keysym.scancode == SDL_SCANCODE_RIGHT;
                int64_t base = st->seek_requested ? st->seek_target_ms : st->position_ms;
                int64_t target = base + (fwd ? 10000 : -10000);
                if (target < 0) target = 0;
                if (st->duration_ms > 0 && target > st->duration_ms) target = st->duration_ms;
                st->seek_target_ms = target;
                st->seek_flags = fwd ? LUMEN_SEEK_FORWARD : LUMEN_SEEK_BACKWARD;
                st->seek_requested = 1;
            }
            if ((e.key.keysym.scancode == SDL_SCANCODE_UP || e.key.keysym.scancode == SDL_SCANCODE_DOWN) &&
                !ImGui::GetIO().WantCaptureKeyboard && ctx->state) {
                lumen_playback_state_t *st = ctx->state;
                /* Whole percent steps, clamped: 97% + 5 -> 100%, 3% - 5 -> 0%. */
                int pct = (int)lroundf(st->volume * 100.0f);
                pct += (e.key.keysym.scancode == SDL_SCANCODE_UP) ? 5 : -5;
                if (pct > 100) pct = 100;
                if (pct < 0) pct = 0;
                st->volume = (float)pct / 100.0f;
                st->muted = 0;       /* like dragging the slider: changing volume unmutes */
            }
            /* V: cycle None -> each available subtitle track -> None (VLC's key) */
            if (e.key.keysym.scancode == SDL_SCANCODE_V &&
                !ImGui::GetIO().WantCaptureKeyboard &&
                ctx->state && ctx->state->has_file) {
                lumen_playback_state_t *st = ctx->state;
                int next = st->subtitle_selected + 1;
                while (next < st->subtitle_track_count && !st->subtitle_tracks[next].available) next++;
                st->subtitle_selected = (next < st->subtitle_track_count) ? next : -1;
            }
            if (e.key.keysym.scancode == SDL_SCANCODE_SPACE &&
                !ImGui::GetIO().WantCaptureKeyboard &&
                ctx->state && ctx->state->has_file) {
                ctx->state->paused = !ctx->state->paused;
            }
        }
    }
    return 0;
}

/* ---- Package Manager: second system window --------------------------------
 *
 * Runs as a completely independent SDL_Window with its own SDL_Renderer and
 * its own ImGui context. ImGui backends store their state in
 * IO.BackendPlatformUserData which is per-context, so calling
 * ImGui_ImplSDL2_InitForSDLRenderer once per context (while that context is
 * current) gives us a fully independent second window on the same thread.
 *
 * Event dispatch: SDL events carry window.windowID so handle_events() routes
 * them to the right ImGui context before processing them.
 * -------------------------------------------------------------------------- */

static bool pkg_is_disabled(lumen_output_ctx_t *ctx,
                             const char *demuxer, const char *codec_plugin) {
    for (int i = 0; i < ctx->pkg_disabled_count; i++) {
        if (strcmp(ctx->pkg_disabled[i].demuxer, demuxer) == 0 &&
            strcmp(ctx->pkg_disabled[i].codec_plugin, codec_plugin) == 0)
            return true;
    }
    return false;
}

static void pkg_toggle_disabled(lumen_output_ctx_t *ctx,
                                 const char *demuxer, const char *codec_plugin) {
    for (int i = 0; i < ctx->pkg_disabled_count; i++) {
        if (strcmp(ctx->pkg_disabled[i].demuxer, demuxer) == 0 &&
            strcmp(ctx->pkg_disabled[i].codec_plugin, codec_plugin) == 0) {
            ctx->pkg_disabled[i] = ctx->pkg_disabled[--ctx->pkg_disabled_count];
            goto sync;
        }
    }
    if (ctx->pkg_disabled_count < 32) {
        strncpy(ctx->pkg_disabled[ctx->pkg_disabled_count].demuxer,     demuxer,     63);
        strncpy(ctx->pkg_disabled[ctx->pkg_disabled_count].codec_plugin, codec_plugin, 63);
        ctx->pkg_disabled_count++;
    }
sync:
    /* Sync local UI list → shared state so player_core.c sees it on the
     * next file open. The output plugin owns the source of truth here;
     * state->disabled_pairs is the read-only view for player_core. */
    if (ctx->state) {
        lumen_playback_state_t *s = ctx->state;
        s->disabled_pair_count = 0;
        for (int i = 0; i < ctx->pkg_disabled_count && i < LUMEN_MAX_DISABLED_PAIRS; i++) {
            strncpy(s->disabled_pairs[i].demuxer_name, ctx->pkg_disabled[i].demuxer,     63);
            strncpy(s->disabled_pairs[i].codec_name,   ctx->pkg_disabled[i].codec_plugin, 63);
            s->disabled_pair_count++;
        }
    }
}

/* Creates a fixed-size auxiliary OS window (Package Manager, Preferences)
 * with its own renderer and its own ImGui context, so styles/fonts/state
 * stay independent from the video window. Leaves the main context current.
 * Returns false on failure (nothing left allocated). */
static bool aux_window_create(lumen_output_ctx_t *ctx, const char *title, int w, int h,
                              SDL_Window **win, SDL_Renderer **ren, ImGuiContext **ictx) {
    /* Fixed size: close/minimize allowed, resize not. */
    *win = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h, SDL_WINDOW_SHOWN);
    if (!*win) {
        fprintf(stderr, "lumen: SDL_CreateWindow(%s) failed: %s\n", title, SDL_GetError());
        return false;
    }
    *ren = SDL_CreateRenderer(*win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!*ren) *ren = SDL_CreateRenderer(*win, -1, SDL_RENDERER_SOFTWARE);
    if (!*ren) {
        SDL_DestroyWindow(*win);
        *win = NULL;
        return false;
    }
    *ictx = ImGui::CreateContext();
    ImGui::SetCurrentContext(*ictx);
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = NULL;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    ImGui::StyleColorsLight();
    void *font_copy = IM_ALLOC(lumen_font_size);          /* same Roboto as the main window */
    if (font_copy) {
        memcpy(font_copy, lumen_font_data, lumen_font_size);
        if (!io.Fonts->AddFontFromMemoryTTF(font_copy, (int)lumen_font_size, 15.0f)) IM_FREE(font_copy);
    }
    ImGui_ImplSDL2_InitForSDLRenderer(*win, *ren);
    ImGui_ImplSDLRenderer2_Init(*ren);
    ImGui::SetCurrentContext(ctx->main_imgui_ctx);
    return true;
}

static void aux_window_destroy(SDL_Window **win, SDL_Renderer **ren, ImGuiContext **ictx) {
    if (*ictx) {
        ImGui::SetCurrentContext(*ictx);
        ImGui_ImplSDLRenderer2_Shutdown();
        ImGui_ImplSDL2_Shutdown();
        ImGui::DestroyContext(*ictx);
        *ictx = NULL;
    }
    if (*ren) { SDL_DestroyRenderer(*ren); *ren = NULL; }
    if (*win) { SDL_DestroyWindow(*win);   *win = NULL; }
}

static void pkg_open(lumen_output_ctx_t *ctx) {
    if (ctx->pkg_window) {
        SDL_ShowWindow(ctx->pkg_window);
        ctx->pkg_window_open = 1;
        return; /* already created -- just un-hide */
    }
    if (!aux_window_create(ctx, "Lumen Package Manager", 600, 440,
                           &ctx->pkg_window, &ctx->pkg_renderer, &ctx->pkg_imgui_ctx))
        return;
    ctx->pkg_window_open = 1;
    printf("  [pkg] Package Manager window opened\n");
}

/* ---- Preferences window ------------------------------------------------ */

static void prefs_open(lumen_output_ctx_t *ctx) {
    if (ctx->pref_window) {
        SDL_ShowWindow(ctx->pref_window);
        SDL_RaiseWindow(ctx->pref_window);
        ctx->pref_window_open = 1;
        return;
    }
    if (!aux_window_create(ctx, "qqvideo Preferences", 500, 560,
                           &ctx->pref_window, &ctx->pref_renderer, &ctx->pref_imgui_ctx))
        return;
    ctx->pref_window_open = 1;
}

/* Grey explanatory text under a setting. */
static void pref_note(const char *text) {
    ImGui::Indent(24.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
    ImGui::Unindent(24.0f);
    ImGui::Spacing();
}

static void prefs_render(lumen_output_ctx_t *ctx) {
    if (!ctx->pref_window || !ctx->pref_window_open || !ctx->pref_imgui_ctx || !ctx->state) return;
    lumen_playback_state_t *st = ctx->state;

    ImGui::SetCurrentContext(ctx->pref_imgui_ctx);
    ImGui_ImplSDLRenderer2_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();

    int win_w, win_h;
    SDL_GetWindowSize(ctx->pref_window, &win_w, &win_h);
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2((float)win_w, (float)win_h));
    ImGui::Begin("##prefs", NULL, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                  ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);

    /* ---- Playback ---- */
    ImGui::SeparatorText("Playback");
    bool precise = st->precise_seek != 0;
    if (ImGui::Checkbox("Precise seeking", &precise)) { st->precise_seek = precise; st->prefs_dirty = 1; }
    pref_note("Seeks land on the exact time you pick. Off: jump to the nearest "
              "keyframe instead -- instant, but up to several seconds away.");

    bool skip = st->pref_frameskip != 0;
    if (ImGui::Checkbox("Adaptive frame skipping", &skip)) { st->pref_frameskip = skip; st->prefs_dirty = 1; }
    pref_note("When video falls behind, skip frames nothing else depends on (and "
              "then the deblocking filter) so more frames arrive on time. Audio "
              "always comes first either way; video that falls far behind jumps "
              "ahead to the next keyframe.");

    /* ---- Buffering ---- */
    ImGui::SeparatorText("Buffering");
    bool buf = st->pref_buffer_enabled != 0;
    if (ImGui::Checkbox("Buffer ahead", &buf)) { st->pref_buffer_enabled = buf; st->prefs_dirty = 1; }
    pref_note("Decode ahead into memory before playback starts and after seeking, "
              "and stop to refill if it runs dry instead of stuttering. Smooths "
              "out heavy scenes; it can't make a CPU that's too slow on average "
              "keep up -- then it just turns stutter into short pauses.");

    ImGui::BeginDisabled(!buf);
    ImGui::Indent(24.0f);
    ImGui::SetNextItemWidth(220.0f);
    ImGui::SliderInt("Buffer size", &st->pref_buffer_seconds, 1, 60, "%d s");
    if (ImGui::IsItemDeactivatedAfterEdit()) st->prefs_dirty = 1;   /* save on release, not every frame */

    /* Memory cap: up to half the machine's RAM. */
    int ram_mb = SDL_GetSystemRAM();
    int max_mb = ram_mb > 0 ? ram_mb / 2 : 4096;
    if (max_mb < 256) max_mb = 256;
    if (st->pref_buffer_mb > max_mb) st->pref_buffer_mb = max_mb;
    ImGui::SetNextItemWidth(220.0f);
    ImGui::SliderInt("Memory limit", &st->pref_buffer_mb, 64, max_mb, "%d MB");
    if (ImGui::IsItemDeactivatedAfterEdit()) st->prefs_dirty = 1;

    /* What those limits mean for the video that's open right now:
     * decoded frames are big (a 4K frame is ~12 MB). */
    if (st->has_file && ctx->width > 0 && ctx->height > 0) {
        double frame_mb = (double)ctx->width * ctx->height * 1.5 / (1024.0 * 1024.0);
        double fps = st->video_fps > 0 ? st->video_fps : 24.0;
        int frames = (int)(st->pref_buffer_mb / frame_mb);
        double secs = frames / fps;
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("This video (%dx%d) is %.1f MB per frame: %d MB holds %d frames (%.1fs). "
                           "The %s limit applies first.",
                           ctx->width, ctx->height, frame_mb, st->pref_buffer_mb, frames, secs,
                           secs < st->pref_buffer_seconds ? "memory" : "time");
        ImGui::PopStyleColor();
        if (st->buffering)
            ImGui::Text("Buffering... %d%%", st->buffer_fill_pct);
        else if (buf)
            ImGui::Text("Decoded ahead: %.1f s", st->buffer_ahead_ms / 1000.0);
    }
    ImGui::Unindent(24.0f);
    ImGui::EndDisabled();

    /* Measured decoding speed: the honest answer to "will buffering help?" */
    if (st->has_file && st->decode_speed_pct > 0) {
        ImGui::Indent(24.0f);
        ImGui::Text("Decoding speed for this video: %.2fx real time", st->decode_speed_pct / 100.0);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        if (st->decode_speed_pct >= 110)
            ImGui::TextWrapped("Faster than playback: a buffer builds up and absorbs heavy scenes.");
        else if (st->decode_speed_pct >= 95)
            ImGui::TextWrapped("About real time: buffering helps with heavy scenes, but keep an eye on it.");
        else
            ImGui::TextWrapped("Slower than playback on average: buffering can only turn stutter into "
                               "pauses. Frame skipping, keyframe seeking or a lower resolution will help more.");
        ImGui::PopStyleColor();
        ImGui::Unindent(24.0f);
    }

    /* ---- footer ---- */
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    if (st->prefs_path[0]) ImGui::TextWrapped("Saved automatically to %s", st->prefs_path);
    else                   ImGui::TextWrapped("No config directory found -- settings last until qqvideo closes.");
    ImGui::PopStyleColor();
    if (ImGui::Button("Close")) { SDL_HideWindow(ctx->pref_window); ctx->pref_window_open = 0; }

    ImGui::End();
    ImGui::Render();
    SDL_SetRenderDrawColor(ctx->pref_renderer, 240, 240, 240, 255);
    SDL_RenderClear(ctx->pref_renderer);
    ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), ctx->pref_renderer);
    SDL_RenderPresent(ctx->pref_renderer);
    ImGui::SetCurrentContext(ctx->main_imgui_ctx);
}

static void pkg_close(lumen_output_ctx_t *ctx) {
    if (!ctx->pkg_window) return;
    SDL_HideWindow(ctx->pkg_window);
    ctx->pkg_window_open = 0;
}

/* Drawn once per frame when the package manager is open.
 * Switches to the pkg ImGui context, renders, then switches back. */
static void pkg_render(lumen_output_ctx_t *ctx) {
    if (!ctx->pkg_window || !ctx->pkg_window_open || !ctx->pkg_imgui_ctx) return;

    lumen_playback_state_t *state = ctx->state;

    ImGui::SetCurrentContext(ctx->pkg_imgui_ctx);

    ImGui_ImplSDLRenderer2_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();

    /* Single full-window ImGui panel -- the OS provides the title bar/
     * min/close decorations; we just fill the client area. */
    int win_w, win_h;
    SDL_GetWindowSize(ctx->pkg_window, &win_w, &win_h);
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2((float)win_w, (float)win_h));

    ImGuiWindowFlags wf = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                          ImGuiWindowFlags_NoMove    | ImGuiWindowFlags_NoScrollbar |
                          ImGuiWindowFlags_NoSavedSettings;
    ImGui::Begin("##pkg_main", NULL, wf);

    if (ImGui::BeginTabBar("##pkg_tabs")) {

        ImVec4 col_inst = ImVec4(0.1f,0.55f,0.1f,1.f);
        ImVec4 col_miss = ImVec4(0.55f,0.55f,0.55f,1.f);
        ImGuiTableFlags tf = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg
                           | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;

        /* Helper: copy a DLL (and any manifest alongside it) into the
         * target package directory, then ask main.c to rescan.
         * Runs entirely in C++ with only Windows/POSIX APIs -- no packages.h. */
        auto do_install = [&](const char *pkg_dir) {
            static const char *exts[] = {"*.dll","*.so"};
            char *picked = tinyfd_openFileDialog(
                "Install Package -- select compiled .dll", "",
                (int)(sizeof(exts)/sizeof(exts[0])), exts, "Plugin (.dll/.so)", 0);
            if (!picked || !picked[0]) return;

            /* basename of selected file */
            const char *sep = picked + strlen(picked);
            while (sep > picked && *sep != '/' && *sep != '\\') sep--;
            if (*sep == '/' || *sep == '\\') sep++;
            const char *fname = sep;

            /* Copy DLL to package dir -- portable C, no windows.h needed */
            char dst[512];
            snprintf(dst, sizeof(dst), "%s/%s", pkg_dir, fname);
            { FILE *s=fopen(picked,"rb"),*d=fopen(dst,"wb");
              if(s&&d){char buf[65536];size_t n;
                while((n=fread(buf,1,sizeof(buf),s))>0)fwrite(buf,1,n,d);}
              if(s)fclose(s);if(d)fclose(d); }
            /* Look for a matching .manifest.json alongside the selected DLL */
            char src_dir[512]; strncpy(src_dir, picked, sizeof(src_dir)-1);
            char *ld = strrchr(src_dir,'/'); if(!ld) ld=strrchr(src_dir,'\\');
            if(ld)*ld='\0'; else strncpy(src_dir,".",sizeof(src_dir)-1);

            char base[256]; strncpy(base, fname, sizeof(base)-1);
            char *dot = strrchr(base,'.'); if(dot)*dot='\0';
            /* strip lib prefix if present */
            const char *bname = (strncmp(base,"lib",3)==0) ? base+3 : base;

            char man_src[512], man_dst[512];
            snprintf(man_src, sizeof(man_src), "%s/%s.manifest.json", src_dir, bname);
            snprintf(man_dst, sizeof(man_dst), "%s/%s.manifest.json", pkg_dir, bname);
            { FILE *s=fopen(man_src,"rb"),*d=fopen(man_dst,"wb");
              if(s&&d){char buf[65536];size_t n;
                while((n=fread(buf,1,sizeof(buf),s))>0)fwrite(buf,1,n,d);}
              if(s)fclose(s);if(d)fclose(d); }
            /* Signal main.c to rescan and refresh pkg_entries */
            if (state) state->rescan_packages_requested = 1;
        };

        /* ---- Tab 1: Packages ----------------------------------------- */
        if (ImGui::BeginTabItem("Packages")) {
#ifdef LUMEN_SYSTEM_FFMPEG
            ImGui::TextDisabled(
                "Codecs come from your system FFmpeg. To get a missing one,\n"
                "install or upgrade FFmpeg (e.g. a /usr/local source build) and rebuild qqvideo.");
#else
            ImGui::TextDisabled(
                "Packages are .dll files you build and drop in. "
                "See packages/BUILD.md for instructions.");
#endif
            ImGui::Separator();

            float tbl_h = (float)win_h - 155.0f;
            if (ImGui::BeginTable("##pkgs", 4, tf, ImVec2(0, tbl_h))) {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("Name",    ImGuiTableColumnFlags_WidthStretch, 0.35f);
                ImGui::TableSetupColumn("Type",    ImGuiTableColumnFlags_WidthStretch, 0.18f);
                ImGui::TableSetupColumn("Status",  ImGuiTableColumnFlags_WidthStretch, 0.27f);
                ImGui::TableSetupColumn("",        ImGuiTableColumnFlags_WidthFixed,   80.0f);
                ImGui::TableHeadersRow();

                int count = state ? state->pkg_entry_count : 0;

                /* Section headers */
                const int TYPES[] = {LUMEN_PKG_UI_DEMUXER, LUMEN_PKG_UI_VIDEO, LUMEN_PKG_UI_AUDIO, LUMEN_PKG_UI_SUBTITLE};
                const char *LABELS[] = {"Demuxers (container readers)", "Video Decoders", "Audio Decoders", "Subtitle Decoders"};

                for (int ti = 0; ti < 4; ti++) {
                    /* Section header row */
                    ImGui::TableNextRow(ImGuiTableRowFlags_None, 20.0f);
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(210,215,230,220));
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, IM_COL32(210,215,230,220));
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(LABELS[ti]);

                    for (int i = 0; i < count; i++) {
                        const lumen_pkg_ui_entry_t &e = state->pkg_entries[i];
                        if (e.type != TYPES[ti]) continue;

                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::Text("  %s", e.name[0] ? e.name : e.plugin);
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", e.detail);

                        ImGui::TableSetColumnIndex(1);
                        ImGui::TextDisabled("%s",
                            ti==0 ? "Demuxer" : ti==1 ? "Video" : ti==2 ? "Audio" : "Subtitle");

                        /* Empty dir_path = provided by the system FFmpeg
                         * (see push_pkg_entries_system in main.c): there is
                         * no package folder to install into. */
                        const bool from_system = (e.dir_path[0] == '\0');

                        ImGui::TableSetColumnIndex(2);
                        if (from_system && e.installed)
                            ImGui::TextColored(col_inst, "+ Available (system)");
                        else if (from_system)
                            ImGui::TextColored(col_miss, "- Not in system FFmpeg");
                        else if (e.installed)
                            ImGui::TextColored(col_inst, "+ Installed  v%s", e.version);
                        else
                            ImGui::TextColored(col_miss, "- Not installed");

                        ImGui::TableSetColumnIndex(3);
                        if (from_system) {
                            ImGui::TextDisabled(e.installed ? "(system)" : "--");
                            if (!e.installed && ImGui::IsItemHovered())
                                ImGui::SetTooltip(
                                    "Your FFmpeg was built without this decoder.\n"
                                    "Install an FFmpeg that has it, then rebuild qqvideo.");
                        } else if (!e.installed) {
                            char btn[48];
                            snprintf(btn, sizeof(btn), "Install...##ins%d", i);
                            if (ImGui::SmallButton(btn))
                                do_install(e.dir_path);
                            if (ImGui::IsItemHovered())
                                ImGui::SetTooltip(
                                    "Build this package then pick the .dll here.\n"
                                    "See packages/BUILD.md.\n\nPackage dir:\n%s",
                                    e.dir_path);
                        } else {
                            ImGui::TextDisabled("(installed)");
                        }
                    }
                }
                ImGui::EndTable();
            }
#ifndef LUMEN_SYSTEM_FFMPEG
            ImGui::TextDisabled("+ Installed   - Not installed   |   Click Install... to add a package.");
#endif
            ImGui::EndTabItem();
        }

        /* ---- Tab 2: Playback Rules ------------------------------------ */
        if (ImGui::BeginTabItem("Playback Rules")) {
            ImGui::TextDisabled(
                "Disable a codec for a specific container to block it\n"
                "from being used when playing that file type.");
            ImGui::Spacing();

            int count = state ? state->pkg_entry_count : 0;
            bool any_demuxer = false;
            for (int i = 0; i < count; i++)
                if (state->pkg_entries[i].type == LUMEN_PKG_UI_DEMUXER &&
                    state->pkg_entries[i].installed) { any_demuxer = true; break; }

            if (!any_demuxer) {
                ImGui::TextColored(col_miss,
                    "No demuxers installed yet.\n"
                    "Install a demuxer package to configure playback rules.");
            } else {
                for (int di = 0; di < count; di++) {
                    const lumen_pkg_ui_entry_t &d = state->pkg_entries[di];
                    if (d.type != LUMEN_PKG_UI_DEMUXER || !d.installed) continue;

                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0,0,0.6f,1));
                    bool open = ImGui::TreeNodeEx(d.name[0]?d.name:d.plugin,
                        ImGuiTreeNodeFlags_DefaultOpen);
                    ImGui::PopStyleColor();
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", d.detail);

                    if (open) {
                        ImGui::Indent(12.f);
                        for (int ci = 0; ci < count; ci++) {
                            const lumen_pkg_ui_entry_t &k = state->pkg_entries[ci];
                            if ((k.type != LUMEN_PKG_UI_VIDEO && k.type != LUMEN_PKG_UI_AUDIO &&
                                 k.type != LUMEN_PKG_UI_SUBTITLE)
                                || !k.installed) continue;
                            bool blocked = pkg_is_disabled(ctx, d.plugin, k.plugin);
                            ImVec4 col = blocked ? ImVec4(0.85f,0.55f,0.0f,1.f) : col_inst;
                            const char *kind = (k.type==LUMEN_PKG_UI_VIDEO)?"Video":
                                               (k.type==LUMEN_PKG_UI_AUDIO)?"Audio":"Subtitle";
                            ImGui::PushStyleColor(ImGuiCol_Text, col);
                            ImGui::Text("[%s] %s -- %s", kind,
                                k.name[0]?k.name:k.plugin, blocked?"DISABLED":"enabled");
                            ImGui::PopStyleColor();
                            ImGui::SameLine();
                            char btn[48];
                            snprintf(btn, sizeof(btn), "%s##r%d%d",
                                blocked?"Enable":"Disable", di, ci);
                            if (ImGui::SmallButton(btn))
                                pkg_toggle_disabled(ctx, d.plugin, k.plugin);
                        }
                        ImGui::Unindent(12.f);
                        ImGui::TreePop();
                    }
                }
            }
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    /* Close button at the bottom, matching VLC's dialog layout */
    float btn_w = 80.0f;
    ImGui::SetCursorPosX((float)win_w - btn_w - ImGui::GetStyle().WindowPadding.x);
    ImGui::SetCursorPosY((float)win_h - ImGui::GetFrameHeightWithSpacing() - ImGui::GetStyle().WindowPadding.y);
    if (ImGui::Button("Close", ImVec2(btn_w, 0))) {
        pkg_close(ctx);
    }

    ImGui::End();
    ImGui::Render();

    SDL_SetRenderDrawColor(ctx->pkg_renderer, 240, 240, 240, 255);
    SDL_RenderClear(ctx->pkg_renderer);
    ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), ctx->pkg_renderer);
    SDL_RenderPresent(ctx->pkg_renderer);

    ImGui::SetCurrentContext(ctx->main_imgui_ctx);
}

static void draw_menu_bar(lumen_output_ctx_t *ctx) {
    lumen_playback_state_t *state = ctx->state;
    if (!state) return;

    bool dialog_open = SDL_AtomicGet(&ctx->dialog_running) != 0;

    /* Every file picker (Open File, Add to Queue, Add Subtitle Track) uses
     * the same background thread so the decode/render loop keeps running
     * while the user picks a file. dialog_purpose tells pump_ui how to use
     * the result (0 = append to queue, 1 = replace queue, 2 = subtitle). */
    auto spawn_dialog = [ctx](int purpose) {
                if (ctx->dialog_thread) {
                    SDL_WaitThread(ctx->dialog_thread, NULL);
                    ctx->dialog_thread = NULL;
                }
                ctx->dialog_purpose = purpose;
                SDL_AtomicSet(&ctx->dialog_result_ready, 0);
                SDL_AtomicSet(&ctx->dialog_running, 1);
                ctx->dialog_thread = SDL_CreateThread(file_dialog_thread, "qqvideo_file_dialog", ctx);
                if (!ctx->dialog_thread) {
                    fprintf(stderr, "lumen: SDL_CreateThread failed: %s\n", SDL_GetError());
                    SDL_AtomicSet(&ctx->dialog_running, 0);
                }
    };

    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {

            if (ImGui::MenuItem("Open File...", NULL, false, !dialog_open)) {
                spawn_dialog(1); /* purpose 1: replace queue with selected file */
            }
            if (dialog_open) ImGui::SetItemTooltip("Waiting for file picker...");
            ImGui::Separator();
            if (ImGui::MenuItem("Add to Queue...", NULL, false, !dialog_open)) {
                spawn_dialog(0); /* purpose 0: append selected file to queue */
            }
            if (dialog_open) ImGui::SetItemTooltip("Waiting for file picker...");
            ImGui::Separator();
            if (ImGui::MenuItem("Quit")) {
                state->quit_requested = 1;
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Subtitles")) {
            bool has_file = state->has_file != 0;
            if (ImGui::MenuItem("Add Subtitle Track...", NULL, false, has_file && !dialog_open)) {
                spawn_dialog(2);
            }
            if (!has_file) ImGui::SetItemTooltip("Open a video first");
            else if (dialog_open) ImGui::SetItemTooltip("Waiting for file picker...");

            if (ImGui::BeginMenu("Subtitle Track", has_file)) {
                if (ImGui::MenuItem("None", NULL, state->subtitle_selected < 0)) {
                    state->subtitle_selected = -1;
                }
                if (state->subtitle_track_count > 0) ImGui::Separator();
                for (int i = 0; i < state->subtitle_track_count; i++) {
                    char item[128];
                    /* ##id suffix keeps two tracks with the same label distinct */
                    snprintf(item, sizeof(item), "%s##sub%d", state->subtitle_tracks[i].label, i);
                    if (ImGui::MenuItem(item, NULL, state->subtitle_selected == i,
                                        state->subtitle_tracks[i].available != 0)) {
                        state->subtitle_selected = i;
                    }
                    if (state->subtitle_tracks[i].file[0])
                        ImGui::SetItemTooltip("%s", state->subtitle_tracks[i].file);
                }
                ImGui::EndMenu();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Tools")) {
            bool precise = state->precise_seek != 0;
            if (ImGui::MenuItem("Precise Seeking", NULL, &precise)) { state->precise_seek = precise ? 1 : 0; state->prefs_dirty = 1; }
            ImGui::SetItemTooltip(
                "On: seeks land on the exact time you pick.\n"
                "Off: seeks jump to the nearest keyframe instead -- instant,\n"
                "but up to several seconds away. Faster on slow CPUs with 4K.");
            ImGui::Separator();
            if (ImGui::MenuItem("Package Manager...")) {
                pkg_open(ctx);
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Preferences...")) prefs_open(ctx);
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }
}
/* Queue panel -- drawn above the control bar when state->show_queue is
 * set. Shows all queued items with the currently-playing one highlighted.
 * Double-clicking an item requests a jump to it via queue_jump_requested.
 *
 * Also shows pending_add_paths inline -- these are paths written by the
 * "Add to Queue" dialog but not yet drained by main.c (that happens
 * between file plays). Showing them here gives IMMEDIATE visual feedback
 * that the file was added, rather than making the user wait until the
 * current movie ends for the queue to visually update. */
static void draw_queue_panel(lumen_output_ctx_t *ctx) {
    lumen_playback_state_t *state = ctx->state;
    int total_visible = (state ? state->queue_count : 0) + (state ? state->pending_add_count : 0);
    if (!state || !state->show_queue || total_visible == 0) return;

    int win_w, win_h;
    SDL_GetWindowSize(ctx->window, &win_w, &win_h);

    /* Compute panel height from ImGui style metrics instead of hardcoded
     * constants. The old values (item_h=22, header_h=26) didn't account
     * for the WindowPadding on both the outer panel AND the inner child
     * window, causing items to be clipped -- confirmed directly from
     * the screenshot where the single item was half-visible.
     *
     * Derivation (using ImGui::GetStyle() values):
     *   outer panel h = 2*wpad + title_row + 1px(sep) + isp + child_h
     *   child_h       = 2*wpad + n * item_h
     *   total         = 4*wpad + title_row_h + 1 + isp + n * item_h
     *
     * item_h uses GetFrameHeightWithSpacing() (not GetTextLineHeight)
     * because each Selectable row takes a full frame height including
     * padding and the item spacing between rows. */
    float wpad     = ImGui::GetStyle().WindowPadding.y;
    float isp      = ImGui::GetStyle().ItemSpacing.y;
    float item_h   = ImGui::GetFrameHeightWithSpacing();
    float title_h  = ImGui::GetFrameHeightWithSpacing(); /* title + [x] button row */

    const int MAX_VISIBLE = 5;
    int capped = total_visible < MAX_VISIBLE ? total_visible : MAX_VISIBLE;
    float panel_h = 4.0f * wpad + title_h + 1.0f + isp + (float)capped * item_h;
    const float panel_w = ((float)win_w * 0.35f > 280.0f ? (float)win_w * 0.35f : 280.0f);
    const float ctrl_bar_h = 60.0f;

    ImGui::SetNextWindowPos(ImVec2((float)win_w - panel_w, (float)win_h - ctrl_bar_h - panel_h));
    ImGui::SetNextWindowSize(ImVec2(panel_w, panel_h));
    ImGui::SetNextWindowBgAlpha(0.88f);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                              ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                              ImGuiWindowFlags_NoFocusOnAppearing;
    ImGui::Begin("##queue", NULL, flags);

    /* Title + [x] close button on the same line */
    ImGui::TextColored(ImVec4(0.3f,0.3f,0.8f,1.0f), "Queue (%d item%s)",
        total_visible, total_visible == 1 ? "" : "s");
    ImGui::SameLine();
    /* Push the button to the right edge so it doesn't float mid-title */
    float btn_w = ImGui::CalcTextSize("x").x + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - btn_w - ImGui::GetStyle().WindowPadding.x);
    if (ImGui::SmallButton("x")) {
        state->show_queue = 0;
    }
    ImGui::Separator();

    ImGui::BeginChild("##qscroll", ImVec2(0, 0), false, 0);

    /* Committed queue items */
    for (int i = 0; i < state->queue_count; i++) {
        bool is_current = (i == state->queue_position);
        if (is_current) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.1f, 0.5f, 0.9f, 1.0f));

        const char *name = state->queue_names ? state->queue_names[i] : "(unknown)";

        char label[32];
        snprintf(label, sizeof(label), "%s%d.##qlbl%d", is_current ? "> " : "  ", i + 1, i);
        ImGui::Selectable(label, is_current, 0, ImVec2(34, item_h));
        ImGui::SameLine();

        char keyed[520];
        snprintf(keyed, sizeof(keyed), "%s##qitem%d", name, i);
        ImGui::Selectable(keyed, is_current, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0, item_h));
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
            state->queue_jump_requested = 1;
            state->queue_jump_index = i;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", name);
        if (is_current) ImGui::PopStyleColor();
    }

    /* Pending items (not yet drained by main.c -- shown immediately for feedback) */
    if (state->pending_add_count > 0) {
        if (state->queue_count > 0) ImGui::Separator();
        for (int i = 0; i < state->pending_add_count; i++) {
            const char *path = state->pending_add_paths[i];
            const char *name = strrchr(path, '/');
            if (!name) name = strrchr(path, '\\');
            name = name ? name + 1 : path;

            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
            char label[48];
            snprintf(label, sizeof(label), "  %d.##qplbl%d", state->queue_count + i + 1, i);
            ImGui::Selectable(label, false, 0, ImVec2(34, item_h));
            ImGui::SameLine();
            char display[530];
            snprintf(display, sizeof(display), "%s  (pending)##qpend%d", name, i);
            ImGui::Selectable(display, false, 0, ImVec2(0, item_h));
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", path);
            ImGui::PopStyleColor();
        }
    }

    ImGui::EndChild();
    ImGui::End();
}

/* Height of the control bar; the min-size math and the video letterbox use it. */
#define LUMEN_CTRL_BAR_H 60.0f

/* Minimum window size = the narrowest the control bar can get with its
 * two button groups side by side at normal spacing, never overlapping.
 * Measured with the WIDEST variant of every label ("Fullscreen" not
 * "Windowed", "Unmute" not "Mute", a two-digit queue count), so a label
 * changing can't make them collide either. Height leaves a 16:9 video
 * area of that width between the menu bar and the control bar. Needs the
 * font, so it's computed on the first frame, then handed to SDL, which
 * also enlarges the window right away if it's currently smaller. */
static void compute_min_size(lumen_output_ctx_t *ctx) {
    ImGuiStyle &st = ImGui::GetStyle();
    auto btn = [&](const char *label) { return ImGui::CalcTextSize(label).x + st.FramePadding.x * 2; };
    float left  = 54.0f /* Play/Pause, fixed width */ + btn("|<") + btn("Stop") + btn(">|") + btn("Fullscreen")
                + st.ItemSpacing.x * 4;
    float right = btn("Queue (99)") + btn("Unmute") + 100.0f /* volume slider */
                + ImGui::CalcTextSize("Vol").x + st.ItemSpacing.x * 3;
    float w = st.WindowPadding.x + left + st.ItemSpacing.x + right + st.WindowPadding.x;
    ctx->min_w = (int)(w + 0.5f);
    ctx->min_h = (int)(ImGui::GetFrameHeight() + LUMEN_CTRL_BAR_H + w * 9.0f / 16.0f + 0.5f);
    SDL_SetWindowMinimumSize(ctx->window, ctx->min_w, ctx->min_h);
    printf("  [sdl2-video] minimum window size %dx%d\n", ctx->min_w, ctx->min_h);
}

/* VLC-style seek bar: a track that fills in as the movie plays, plus a
 * round handle at the current position that grows under the mouse, and a
 * tooltip with the time under the cursor. Click anywhere to jump there,
 * or drag; the seek happens on release. */
static void draw_seek_bar(lumen_output_ctx_t *ctx, float width) {
    lumen_playback_state_t *state = ctx->state;
    bool has_file = state->has_file && state->duration_ms > 0;
    const float h = 20.0f;                        /* hit area height */
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##seek", ImVec2(width, h));
    bool hovered = has_file && ImGui::IsItemHovered();
    bool active  = has_file && ImGui::IsItemActive();

    const float r_handle = 7.0f;                  /* keep the handle inside the hit area (r + ring < h/2) */
    float x0 = p0.x + r_handle, x1 = p0.x + width - r_handle;
    float cy = p0.y + h * 0.5f;
    float dur = (float)(has_file ? state->duration_ms : 1);

    if (active) {                                 /* click or drag: follow the mouse */
        float t = (ImGui::GetIO().MousePos.x - x0) / (x1 - x0);
        t = t < 0 ? 0 : (t > 1 ? 1 : t);
        ctx->dragging_seek = true;
        ctx->seek_preview_ms = t * dur;
    }
    if (has_file && ImGui::IsItemDeactivated() && ctx->dragging_seek) {
        ctx->dragging_seek = false;
        if (!state->seek_requested) {
            state->seek_target_ms = (int64_t)ctx->seek_preview_ms;
            state->seek_flags = LUMEN_SEEK_BACKWARD;
            state->seek_requested = 1;
        }
    }

    float pos = has_file ? (ctx->dragging_seek ? ctx->seek_preview_ms : (float)state->position_ms) : 0.0f;
    float frac = pos / dur;
    frac = frac < 0 ? 0 : (frac > 1 ? 1 : frac);
    float hx = x0 + (x1 - x0) * frac;

    ImDrawList *dl = ImGui::GetWindowDrawList();
    /* Solid colors rather than the theme's pale slider tint: the played
     * part should read at a glance against the unplayed track. */
    const ImU32 col_track = IM_COL32(236, 236, 236, 255);
    const ImU32 col_edge  = IM_COL32(150, 150, 150, 255);
    const ImU32 col_fill  = (hovered || active) ? IM_COL32(40, 110, 225, 255) : IM_COL32(52, 120, 230, 255);
    float th = (hovered || active) ? 10.0f : 8.0f;  /* track thickens under the mouse */
    dl->AddRectFilled(ImVec2(x0, cy - th / 2), ImVec2(x1, cy + th / 2), col_track, th / 2);
    dl->AddRect(ImVec2(x0, cy - th / 2), ImVec2(x1, cy + th / 2), col_edge, th / 2);
    if (has_file) {
        /* YouTube-style buffered range: from the playhead to how far ahead
         * is already decoded ("Buffer ahead", or the normal read-ahead). */
        if (!ctx->dragging_seek && state->buffer_ahead_ms > 0) {
            float bf = ((float)state->position_ms + (float)state->buffer_ahead_ms) / dur;
            bf = bf > 1 ? 1 : bf;
            float bx = x0 + (x1 - x0) * bf;
            if (bx > hx) dl->AddRectFilled(ImVec2(hx - th / 2, cy - th / 2), ImVec2(bx, cy + th / 2),
                                           IM_COL32(168, 190, 228, 255), th / 2);
        }
        if (hx > x0) dl->AddRectFilled(ImVec2(x0, cy - th / 2), ImVec2(hx, cy + th / 2), col_fill, th / 2);
        float rr = (hovered || active) ? r_handle : r_handle - 1.5f;
        dl->AddCircleFilled(ImVec2(hx, cy), rr + 2.0f, IM_COL32(255, 255, 255, 255), 24);  /* white ring */
        dl->AddCircle(ImVec2(hx, cy), rr + 2.0f, IM_COL32(90, 90, 90, 200), 24, 1.0f);     /* keeps it visible on the light track */
        dl->AddCircleFilled(ImVec2(hx, cy), rr, col_fill, 24);
    }

    if (hovered || active) {                       /* time under the cursor */
        float t = (ImGui::GetIO().MousePos.x - x0) / (x1 - x0);
        t = t < 0 ? 0 : (t > 1 ? 1 : t);
        char when[16];
        format_ms((int64_t)((active ? ctx->seek_preview_ms / dur : t) * dur), when, sizeof(when));
        ImGui::SetTooltip("%s", when);
    }
}

static void draw_controls(lumen_output_ctx_t *ctx) {
    lumen_playback_state_t *state = ctx->state;
    if (!state) return;
    if (!ctx->min_w) compute_min_size(ctx);

    int win_w, win_h;
    SDL_GetWindowSize(ctx->window, &win_w, &win_h);

    const float bar_height = LUMEN_CTRL_BAR_H;
    ImGui::SetNextWindowPos(ImVec2(0, (float)win_h - bar_height));
    ImGui::SetNextWindowSize(ImVec2((float)win_w, bar_height));
    ImGui::SetNextWindowBgAlpha(0.75f);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                              ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                              ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing;
    ImGui::Begin("##controls", NULL, flags);

    /* --- Row 1: seek bar with time labels -------------------------------- */
    char cur_str[16], dur_str[16];
    format_ms(state->position_ms, cur_str, sizeof(cur_str));
    format_ms(state->duration_ms, dur_str, sizeof(dur_str));

    bool has_file = state->has_file;
    ImGui::AlignTextToFramePadding();
    ImGui::Text("%s", cur_str);
    ImGui::SameLine();
    /* 20 px bar, nudged up so it lines up with the frame-height time labels */
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (ImGui::GetFrameHeight() - 20.0f) * 0.5f);
    draw_seek_bar(ctx, ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(dur_str).x - ImGui::GetStyle().ItemSpacing.x);
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::Text("%s", dur_str);

    /* --- Row 2 left: transport controls ---------------------------------- */
    if (!has_file) ImGui::BeginDisabled();

    /* Play / Pause -- leftmost, full words for clarity */
    if (ImGui::Button(state->paused ? "Play" : "Pause", ImVec2(54, 0))) {
        state->paused = !state->paused;
    }
    ImGui::SameLine();

    /* Previous track */
    if (ImGui::Button("|<")) {
        int prev = state->queue_position > 0 ? state->queue_position - 1 : 0;
        state->queue_jump_requested = 1;
        state->queue_jump_index = prev;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Previous");
    ImGui::SameLine();

    /* Stop -- clears queue and returns to idle */
    if (ImGui::Button("Stop")) {
        state->stop_requested = 1;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Stop and clear queue");
    ImGui::SameLine();

    /* Next track */
    if (ImGui::Button(">|")) {
        state->queue_jump_requested = 1;
        state->queue_jump_index = state->queue_position + 1;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Next");
    ImGui::SameLine();

    if (!has_file) ImGui::EndDisabled();

    /* Fullscreen (always available regardless of has_file) */
    if (ImGui::Button(ctx->fullscreen ? "Windowed" : "Fullscreen")) {
        toggle_fullscreen(ctx);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(ctx->fullscreen ? "Windowed" : "Fullscreen");

    /* --- Row 2 right: Queue, Mute, Volume -- right-aligned -------------- */
    /* Compute how much space the right cluster needs, then jump there. */
    ImGuiStyle &style = ImGui::GetStyle();
    char queue_btn[32];
    int total_q = state->queue_count + state->pending_add_count;
    if (total_q > 0) snprintf(queue_btn, sizeof(queue_btn), "Queue (%d)", total_q);
    else             strncpy(queue_btn, "Queue", sizeof(queue_btn));

    const char *mute_label = state->muted ? "Unmute" : "Mute";
    float vol_slider_w = 100.0f;

    float right_w = ImGui::CalcTextSize(queue_btn).x  + style.FramePadding.x * 2 + style.ItemSpacing.x
                  + ImGui::CalcTextSize(mute_label).x + style.FramePadding.x * 2 + style.ItemSpacing.x
                  + ImGui::CalcTextSize("Vol").x       + style.ItemSpacing.x
                  + vol_slider_w                       + style.WindowPadding.x;

    float right_x = ImGui::GetWindowWidth() - right_w;
    float left_cursor = ImGui::GetCursorPosX();
    /* Only jump right if there's actually space; otherwise stay sequential */
    if (right_x > left_cursor + style.ItemSpacing.x)
        ImGui::SameLine(right_x);
    else
        ImGui::SameLine();

    if (ImGui::Button(queue_btn)) state->show_queue = !state->show_queue;
    ImGui::SameLine();

    /* Mute/Unmute: toggling saves or restores the pre-mute volume level
     * so the slider position is preserved while muted. */
    if (ImGui::Button(mute_label)) {
        if (state->muted) {
            state->muted = 0;
            state->volume = ctx->volume_before_mute > 0.0f ? ctx->volume_before_mute : 0.5f;
        } else {
            ctx->volume_before_mute = state->volume;
            state->muted = 1;
        }
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(state->muted ? "Unmute (restore %.0f%%)" : "Mute",
                          state->muted ? ctx->volume_before_mute * 100.0f : 0.0f);
    ImGui::SameLine();

    ImGui::SetNextItemWidth(vol_slider_w);
    float volume_pct = state->volume * 100.0f;
    if (ImGui::SliderFloat("Vol", &volume_pct, 0.0f, 100.0f, "%.0f%%")) {
        state->volume = volume_pct / 100.0f;
        /* Dragging the slider while muted implicitly unmutes */
        if (state->muted) state->muted = 0;
    }

    ImGui::End();
}

/* Where the video picture lands in the window (letterboxed, aspect kept).
 * Shared by the texture blit and the subtitle renderer so text always sits
 * on the picture itself, not on the black bars. Returns false if there's
 * no video to place. */
static bool video_dest_rect(lumen_output_ctx_t *ctx, int win_w, int win_h,
                            float menu_h, float ctrl_h, SDL_Rect *dest) {
    if (!ctx->texture || ctx->width <= 0 || ctx->height <= 0 || !ctx->state || !ctx->state->has_file)
        return false;
    /* Fullscreen: fill the whole screen, bars overlay the video.
     * Windowed: inset below the menu bar and above the control bar. */
    int area_y, area_h;
    if (ctx->fullscreen) {
        area_y = 0;
        area_h = win_h;
    } else {
        area_y = (int)menu_h;
        area_h = win_h - (int)menu_h - (int)ctrl_h;
    }
    if (area_h < 1) area_h = 1;

    double video_aspect = (double)ctx->width / (double)ctx->height;
    double area_aspect  = (double)win_w / (double)area_h;
    if (area_aspect > video_aspect) {
        dest->h = area_h;
        dest->w = (int)(area_h * video_aspect);
        dest->x = (win_w - dest->w) / 2;
        dest->y = area_y;
    } else {
        dest->w = win_w;
        dest->h = (int)(win_w / video_aspect);
        dest->x = 0;
        dest->y = area_y + (area_h - dest->h) / 2;
    }
    return true;
}

/* Subtitles: white text with a black outline, centered near the bottom of
 * the picture, sized relative to the picture height (so it scales with the
 * window and fullscreen), word-wrapped at 90% of the picture width. Drawn
 * on ImGui's background draw list: that renders after the video texture
 * but beneath every menu, popup and panel. */
static void draw_subtitles(lumen_output_ctx_t *ctx, const SDL_Rect &v, int win_h,
                           bool controls_shown, float ctrl_h) {
    const char *text = ctx->state->subtitle_text;
    if (!text[0]) return;

    ImFont *font = ctx->sub_font ? ctx->sub_font : ImGui::GetFont();
    float size = (float)v.h * 0.055f;
    if (size < 16.0f) size = 16.0f;
    if (size > 72.0f) size = 72.0f;
    float scale = size / font->FontSize;
    float wrap_w = (float)v.w * 0.90f;

    /* Split on explicit newlines, then word-wrap each piece. */
    struct { const char *b, *e; } lines[48];
    int n = 0;
    const char *p = text;
    while (*p && n < 48) {
        const char *eol = strchr(p, '\n');
        if (!eol) eol = p + strlen(p);
        const char *b = p;
        while (b < eol && n < 48) {
            const char *brk = font->CalcWordWrapPositionA(scale, b, eol, wrap_w);
            if (brk <= b) brk = eol;                  /* single overlong word */
            const char *e = brk;
            while (e > b && e[-1] == ' ') e--;        /* no trailing spaces */
            lines[n].b = b; lines[n].e = e; n++;
            b = brk;
            while (b < eol && *b == ' ') b++;         /* no leading spaces */
        }
        if (b == p && n < 48) { lines[n].b = p; lines[n].e = p; n++; } /* blank line */
        p = (*eol == '\n') ? eol + 1 : eol;
    }

    float line_h = size * 1.15f;
    float bottom = (float)(v.y + v.h) - (float)v.h * 0.06f;
    /* Fullscreen with the control bar showing: lift text above the bar
     * instead of hiding it underneath. */
    if (controls_shown && bottom > (float)win_h - ctrl_h - 8.0f)
        bottom = (float)win_h - ctrl_h - 8.0f;
    float y = bottom - line_h * (float)n;

    ImDrawList *dl = ImGui::GetBackgroundDrawList();
    float o = size * 0.06f;
    if (o < 1.5f) o = 1.5f;
    const ImU32 outline = IM_COL32(0, 0, 0, 230), fill = IM_COL32(255, 255, 255, 255);
    static const float dirs[8][2] = {{-1,-1},{0,-1},{1,-1},{-1,0},{1,0},{-1,1},{0,1},{1,1}};
    for (int i = 0; i < n; i++, y += line_h) {
        if (lines[i].b == lines[i].e) continue;
        float w = font->CalcTextSizeA(size, FLT_MAX, 0.0f, lines[i].b, lines[i].e).x;
        ImVec2 pos((float)v.x + ((float)v.w - w) * 0.5f, y);
        for (int d = 0; d < 8; d++)
            dl->AddText(font, size, ImVec2(pos.x + dirs[d][0] * o, pos.y + dirs[d][1] * o),
                        outline, lines[i].b, lines[i].e);
        dl->AddText(font, size, pos, fill, lines[i].b, lines[i].e);
    }
}

static void maybe_redraw(lumen_output_ctx_t *ctx) {
    Uint64 now = SDL_GetTicks64();
    if (ctx->have_last_redraw && (now - ctx->last_ui_redraw_ticks) < LUMEN_UI_REDRAW_INTERVAL_MS) {
        return;
    }
    ctx->last_ui_redraw_ticks = now;
    ctx->have_last_redraw = 1;

    /* Update fullscreen auto-hide state. Only mouse movement (tracked in
     * handle_events) resets the timer; keyboard shortcuts like Space do not.
     * Windowed mode always shows the controls. */
    if (ctx->fullscreen) {
        int should_show = (now - ctx->last_mouse_move_ticks) < 3000;
        if (should_show != ctx->controls_visible) {
            ctx->controls_visible = should_show;
            SDL_ShowCursor(should_show ? SDL_ENABLE : SDL_DISABLE);
        }
    } else {
        /* Windowed: always visible, always show cursor */
        ctx->controls_visible = 1;
        SDL_ShowCursor(SDL_ENABLE);
    }

    ImGui_ImplSDLRenderer2_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();

    float menu_h = ImGui::GetFrameHeight();
    float ctrl_h = 60.0f;

    /* In windowed mode, both bars are always visible.
     * In fullscreen:
     *   - Menu bar ("File") is ALWAYS hidden -- the Windows title bar is
     *     already gone (SDL fullscreen removes it), and the ImGui menu bar
     *     would just be a distraction covering the top of the video.
     *     File > Open etc. are accessible by going Windowed first.
     *   - Control bar auto-hides after 3s of no mouse movement,
     *     reappears immediately on mouse move. */
    bool show_menu     = !ctx->fullscreen;
    bool show_controls = !ctx->fullscreen || ctx->controls_visible;

    if (show_menu)     draw_menu_bar(ctx);
    if (show_controls) draw_queue_panel(ctx);
    if (show_controls) draw_controls(ctx);

    /* Error overlay -- shown when player_core.c sets state->error_msg
     * (missing decoder, blocked codec). Displayed in the video area centre. */
    {
        lumen_playback_state_t *st = ctx->state;
        if (st && st->error_msg[0] != '\0') {
            int ww, wh;
            SDL_GetWindowSize(ctx->window, &ww, &wh);
            ImGui::SetNextWindowPos(ImVec2((float)ww * 0.5f, (float)wh * 0.45f),
                                    ImGuiCond_Always, ImVec2(0.5f, 0.5f));
            ImGui::SetNextWindowBgAlpha(0.92f);
            ImGuiWindowFlags ef = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                  ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                                  ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize;
            ImGui::Begin("##err_overlay", NULL, ef);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.90f, 0.15f, 0.15f, 1.0f));
            ImGui::Text("  Codec Error  ");
            ImGui::PopStyleColor();
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::SetNextWindowSize(ImVec2(360.0f, 0));
            ImGui::TextWrapped("%s", st->error_msg);
            ImGui::Spacing();
            ImGui::TextDisabled("Click anywhere or press any key to dismiss");
            ImGui::Spacing();
            ImGui::End();
            /* Dismiss on any click or keypress */
            if (ImGui::IsMouseClicked(0) || ImGui::IsKeyPressed(ImGuiKey_Space) ||
                ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                st->error_msg[0] = '\0';
            }
        }
    }

    int win_w, win_h;
    SDL_GetWindowSize(ctx->window, &win_w, &win_h);
    SDL_Rect dest;
    bool have_video = video_dest_rect(ctx, win_w, win_h, menu_h, ctrl_h, &dest) && ctx->have_frame;
    if (have_video) draw_subtitles(ctx, dest, win_h, ctx->fullscreen && show_controls, ctrl_h);
    if (ctx->state && ctx->state->has_file && ctx->state->buffering) {
        /* "Buffer ahead" is filling: say so, so a pause doesn't look like a hang. */
        char msg[48];
        snprintf(msg, sizeof(msg), "Buffering... %d%%", ctx->state->buffer_fill_pct);
        ImVec2 ts = ImGui::CalcTextSize(msg);
        float cx = (float)win_w * 0.5f, cy = menu_h + ((float)win_h - menu_h - ctrl_h) * 0.5f;
        float bw = ts.x + 40.0f, bh = ts.y + 26.0f;
        ImDrawList *dl = ImGui::GetBackgroundDrawList();
        ImVec2 a(cx - bw / 2, cy - bh / 2), b(cx + bw / 2, cy + bh / 2);
        dl->AddRectFilled(a, b, IM_COL32(0, 0, 0, 170), 6.0f);
        dl->AddText(ImVec2(cx - ts.x / 2, a.y + 7.0f), IM_COL32(255, 255, 255, 255), msg);
        float px = a.x + 12.0f, pw = bw - 24.0f, py = b.y - 9.0f;       /* progress strip */
        dl->AddRectFilled(ImVec2(px, py), ImVec2(px + pw, py + 3.0f), IM_COL32(90, 90, 90, 255), 1.5f);
        dl->AddRectFilled(ImVec2(px, py), ImVec2(px + pw * ctx->state->buffer_fill_pct / 100.0f, py + 3.0f),
                          IM_COL32(52, 120, 230, 255), 1.5f);
    }

    ImGui::Render();

    /* Window title update (unchanged) */
    lumen_playback_state_t *state = ctx->state;
    if (state) {
        int qpos = state->queue_position;
        if (qpos != ctx->last_queue_pos) {
            ctx->last_queue_pos = qpos;
            if (state->queue_names && qpos >= 0 && qpos < state->queue_count) {
                char title[512];
                snprintf(title, sizeof(title), "%s - qqvideo", state->queue_names[qpos]);
                SDL_SetWindowTitle(ctx->window, title);
            } else {
                SDL_SetWindowTitle(ctx->window, "qqvideo");
            }
        }
    }

    SDL_SetRenderDrawColor(ctx->renderer, 0, 0, 0, 255);
    SDL_RenderClear(ctx->renderer);

    if (have_video) SDL_RenderCopy(ctx->renderer, ctx->texture, NULL, &dest);

    ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), ctx->renderer);
    SDL_RenderPresent(ctx->renderer);

    /* Render the package manager window if it's open */
    if (ctx->pkg_window_open) pkg_render(ctx);
    if (ctx->pref_window_open) prefs_render(ctx);
}

static int sdl2_open(lumen_output_ctx_t **out_ctx) {
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");

    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "lumen: SDL_InitSubSystem(VIDEO) failed: %s\n", SDL_GetError());
        return -1;
    }

    lumen_output_ctx_t *ctx = (lumen_output_ctx_t *)calloc(1, sizeof(*ctx));
    ctx->last_queue_pos = -2;
    ctx->last_mouse_move_ticks = SDL_GetTicks64();
    ctx->controls_visible = 1;
    ctx->pkg_window = NULL;
    ctx->pkg_renderer = NULL;
    ctx->pkg_imgui_ctx = NULL;
    ctx->pkg_window_open = 0;
    ctx->pkg_disabled_count = 0;
    ctx->dialog_thread = NULL;
    SDL_AtomicSet(&ctx->dialog_running, 0);
    SDL_AtomicSet(&ctx->dialog_result_ready, 0);
    ctx->dialog_result_path[0] = '\0';
    ctx->dialog_mutex = SDL_CreateMutex();
    if (!ctx->dialog_mutex) {
        fprintf(stderr, "lumen: SDL_CreateMutex failed: %s\n", SDL_GetError());
        /* Non-fatal -- dialog thread won't start but everything else works */
    }

    ctx->window = SDL_CreateWindow("qqvideo",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        LUMEN_IDLE_WINDOW_W, LUMEN_IDLE_WINDOW_H,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!ctx->window) {
        fprintf(stderr, "lumen: SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        free(ctx);
        return -1;
    }

    /* Set the window icon from the embedded PNG (title-bar chrome icon).
     * stbi_load_from_memory decodes the 64×64 RGBA PNG in qqvideo_icon.h
     * into raw pixel data that SDL can use as a surface. */
    {
        int w, h, channels;
        unsigned char *pixels = stbi_load_from_memory(
            qqvideo_icon_png, (int)qqvideo_icon_png_size,
            &w, &h, &channels, 4 /* force RGBA */);
        if (pixels) {
            SDL_Surface *icon = SDL_CreateRGBSurfaceFrom(
                pixels, w, h, 32, w * 4,
                0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000);
            if (icon) {
                SDL_SetWindowIcon(ctx->window, icon);
                SDL_FreeSurface(icon);
            }
            stbi_image_free(pixels);
        }
    }

    ctx->renderer = SDL_CreateRenderer(ctx->window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ctx->renderer) {
        fprintf(stderr, "lumen: no accelerated SDL renderer available, falling back to software rendering\n");
        ctx->renderer = SDL_CreateRenderer(ctx->window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!ctx->renderer) {
        fprintf(stderr, "lumen: SDL_CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(ctx->window);
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        free(ctx);
        return -1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = NULL;
    /* Without this flag, ImGui_ImplSDL2_NewFrame() calls SDL_ShowCursor(SDL_ENABLE)
     * every frame to manage its own cursor shapes -- silently overriding our
     * SDL_ShowCursor(SDL_DISABLE) the instant the next frame renders, which is
     * exactly why the cursor stayed visible even after we hid it. This flag
     * tells ImGui "leave the SDL cursor alone, we own it". The only thing lost
     * is ImGui's custom resize/move cursor shapes, which a video player has no
     * use for anyway. */
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    ImGui::StyleColorsLight();

    /* Load Roboto (embedded in lumen_font.cpp) at 15px -- smooth anti-aliased
     * TrueType rendering, replacing the default ProggyClean bitmap font.
     * AddFontFromMemoryTTF takes ownership of the data buffer and frees it
     * with IM_FREE, so we always pass a heap copy of the static array. */
    void *font_copy = IM_ALLOC(lumen_font_size);
    if (font_copy) {
        memcpy(font_copy, lumen_font_data, lumen_font_size);
        ImFont *font = io.Fonts->AddFontFromMemoryTTF(font_copy, (int)lumen_font_size, 15.0f);
        if (!font) {
            fprintf(stderr, "lumen: failed to load embedded Roboto font -- falling back to ProggyClean\n");
            IM_FREE(font_copy);  /* only free on failure; success means ImGui owns it */
        } else {
            /* Second, large copy of Roboto just for subtitles. Rasterized at
             * 48px so it stays crisp when scaled to video size, with extra
             * glyph ranges subtitles actually use: accented Latin (Latin
             * Extended-A) and General Punctuation -- curly quotes, em dashes,
             * ellipses -- which the UI font's default range lacks and would
             * otherwise render as '?'. */
            static const ImWchar sub_ranges[] = {
                0x0020, 0x00FF,   /* Basic Latin + Latin-1 */
                0x0100, 0x017F,   /* Latin Extended-A */
                0x2010, 0x205E,   /* General Punctuation */
                0x266A, 0x266B,   /* music notes (lyrics) -- used if the font has them */
                0,
            };
            void *sub_copy = IM_ALLOC(lumen_font_size);
            if (sub_copy) {
                memcpy(sub_copy, lumen_font_data, lumen_font_size);
                ctx->sub_font = io.Fonts->AddFontFromMemoryTTF(sub_copy, (int)lumen_font_size, 48.0f, NULL, sub_ranges);
                if (!ctx->sub_font) IM_FREE(sub_copy);
            }
        }
    } else {
        fprintf(stderr, "lumen: failed to allocate font buffer -- falling back to ProggyClean\n");
    }
    ImGui_ImplSDL2_InitForSDLRenderer(ctx->window, ctx->renderer);
    ImGui_ImplSDLRenderer2_Init(ctx->renderer);

    /* Store the main context pointer so pkg_open() and pkg_render() can
     * switch back to it after using the package manager's own context. */
    ctx->main_imgui_ctx = ImGui::GetCurrentContext();

    *out_ctx = ctx;
    printf("  [sdl2-video] window opened (idle, no file loaded yet)\n");
    return 0;
}

static int sdl2_load_stream(lumen_output_ctx_t *ctx, int width, int height, int pixel_format) {
    (void)pixel_format;

    if (ctx->texture) {
        SDL_DestroyTexture(ctx->texture);
        ctx->texture = NULL;
    }

    ctx->texture = SDL_CreateTexture(ctx->renderer, SDL_PIXELFORMAT_IYUV,
        SDL_TEXTUREACCESS_STREAMING, width, height);
    if (!ctx->texture) {
        fprintf(stderr, "lumen: SDL_CreateTexture failed: %s\n", SDL_GetError());
        return -1;
    }
    ctx->width = width;
    ctx->have_frame = 0;
    ctx->height = height;

    /* Skip resizing if the window is in any expanded state. There are two
     * distinct cases both now handled:
     *
     * 1. SDL fullscreen (ctx->fullscreen == 1): our Fullscreen button was
     *    used. SDL_SetWindowSize takes the window out of SDL's fullscreen
     *    mode on Windows, confirmed directly.
     *
     * 2. Windows OS maximize (SDL_WINDOW_MAXIMIZED flag): the user clicked
     *    the title-bar □ button instead of our Fullscreen button.
     *    ctx->fullscreen stays 0 in this case, so the previous check alone
     *    didn't catch it. SDL_SetWindowSize on a Windows-maximized window
     *    calls SetWindowPos internally, which strips WS_MAXIMIZE and shrinks
     *    the window back to the 90% size -- confirmed as the exact cause
     *    from the screenshot (⧉ still showing, video content shrunken).
     *
     * In both cases the letterbox rect in maybe_redraw() adapts to any
     * window size, so skipping the resize loses nothing visually. */
    Uint32 wflags = SDL_GetWindowFlags(ctx->window);
    int skip_resize = ctx->fullscreen
                   || (wflags & SDL_WINDOW_FULLSCREEN)  /* SDL fullscreen (any mode) */
                   || (wflags & SDL_WINDOW_MAXIMIZED);  /* OS title-bar maximize */

    if (!skip_resize) {
        int target_w = width > 0 ? width : LUMEN_IDLE_WINDOW_W;
        int target_h = height > 0 ? height : LUMEN_IDLE_WINDOW_H;
        SDL_Rect display_bounds;
        if (SDL_GetDisplayUsableBounds(0, &display_bounds) == 0 && display_bounds.w > 0 && display_bounds.h > 0) {
            int max_w = (int)(display_bounds.w * 0.9);
            int max_h = (int)(display_bounds.h * 0.9);
            if (target_w > max_w || target_h > max_h) {
                double scale_w = (double)max_w / target_w;
                double scale_h = (double)max_h / target_h;
                double scale = (scale_w < scale_h) ? scale_w : scale_h;
                target_w = (int)(target_w * scale);
                target_h = (int)(target_h * scale);
            }
        }
        /* Small videos (e.g. 320x240) would make the window narrower than
         * the control bar and the buttons overlap: scale up, aspect kept,
         * to at least the minimum size. */
        int mw = ctx->min_w ? ctx->min_w : 520, mh = ctx->min_h ? ctx->min_h : 380;
        if (target_w < mw) { target_h = (int)((double)target_h * mw / target_w + 0.5); target_w = mw; }
        if (target_h < mh) target_h = mh;
        SDL_SetWindowSize(ctx->window, target_w, target_h);
        SDL_SetWindowPosition(ctx->window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
        printf("  [sdl2-video] loaded stream %dx%d, window resized to %dx%d\n", width, height, target_w, target_h);
    } else {
        const char *reason = ctx->fullscreen            ? "SDL fullscreen"      :
                             (wflags & SDL_WINDOW_FULLSCREEN) ? "SDL fullscreen (flags)" : "OS maximized";
        printf("  [sdl2-video] loaded stream %dx%d, window kept (%s)\n", width, height, reason);
    }

    /* Fresh pacing/diagnostic state for the new file -- these were
     * previously reset implicitly by close()+open() between files;
     * now that the window stays open across files, load_stream() is
     * what has to do this instead. */
    ctx->have_start = 0;
    ctx->frames_rendered = 0;
    ctx->frames_dropped = 0;
    ctx->was_paused = 0;

    return 0;
}

static void sdl2_bind_state(lumen_output_ctx_t *ctx, lumen_playback_state_t *state) {
    ctx->state = state;
}

static int sdl2_pump_ui(lumen_output_ctx_t *ctx, lumen_playback_state_t *state) {
    /* Check whether the background "Add to Queue" dialog thread has a result.
     * We do this here (main thread) rather than in the dialog thread so that
     * pending_add_paths is only ever written from the main thread -- no sync
     * needed for the shared state struct beyond the mutex protecting the
     * intermediate dialog_result_path buffer. */
    if (SDL_AtomicGet(&ctx->dialog_result_ready) && state) {
        SDL_LockMutex(ctx->dialog_mutex);
        bool has_path = ctx->dialog_result_path[0] != '\0';
        if (ctx->dialog_purpose == 2) {
            /* Add Subtitle Track: the core loads it on its next loop pass */
            if (has_path) {
                strncpy(state->subtitle_add_path, ctx->dialog_result_path, 511);
                state->subtitle_add_path[511] = '\0';
                state->subtitle_add_requested = 1;
            }
        } else if (ctx->dialog_purpose == 1) {
            /* Open File: write path for main.c and signal it to switch files.
             * On cancel (empty path) we do nothing -- playback continues. */
            if (has_path) {
                strncpy(state->open_file_pending_path, ctx->dialog_result_path, 511);
                state->open_file_pending_path[511] = '\0';
                state->open_file_requested = 1;
            }
        } else {
            /* Add to Queue: append path to pending list for main.c to drain */
            if (has_path && state->pending_add_count < LUMEN_PENDING_QUEUE_MAX) {
                strncpy(state->pending_add_paths[state->pending_add_count],
                        ctx->dialog_result_path, 511);
                state->pending_add_paths[state->pending_add_count][511] = '\0';
                state->pending_add_count++;
            }
        }
        ctx->dialog_result_path[0] = '\0';
        SDL_UnlockMutex(ctx->dialog_mutex);
        SDL_AtomicSet(&ctx->dialog_result_ready, 0);
    }

    if (handle_events(ctx)) {
        /* Window closed or ESC. Without also setting quit_requested here,
         * player_core.c sets stop_requested internally but main.c's
         * is_quit_requested() check stays false -- so the queue advances
         * to the next item instead of exiting. This is the root cause of
         * "closing the window moves to the next queue item" confirmed
         * directly; setting it here is the minimal correct fix. */
        if (state) state->quit_requested = 1;
        return 1;
    }

    if (state) {
        Uint64 now = SDL_GetTicks64();
        if (state->paused && !ctx->was_paused) {
            ctx->pause_started_ticks = now;
            ctx->was_paused = 1;
        } else if (!state->paused && ctx->was_paused) {
            Uint64 paused_duration = now - ctx->pause_started_ticks;
            ctx->start_ticks += paused_duration;
            ctx->was_paused = 0;
        }
    }

    maybe_redraw(ctx);
    return 0;
}

static int sdl2_present(lumen_output_ctx_t *ctx, const lumen_frame_t *frame) {
    if (handle_events(ctx)) {
        if (ctx->state) ctx->state->quit_requested = 1;
        return 1;
    }

    Uint64 now = SDL_GetTicks64();
    if (!ctx->have_start) {
        /* Anchor on this frame's pts: frames carry real container time
         * now, and a file's first frame isn't always at exactly 0. */
        ctx->start_ticks = now - (Uint64)(frame->pts > 0 ? frame->pts : 0);
        ctx->have_start = 1;
    }
    if (ctx->state && ctx->state->seek_generation != ctx->last_seen_seek_generation) {
        ctx->start_ticks = now - (Uint64)frame->pts;
        ctx->last_seen_seek_generation = ctx->state->seek_generation;
    }
    if (ctx->state && ctx->state->video_sync_external) {
        /* player_core already held this frame until the AUDIO clock reached
         * it (audio is the master clock). Show it now, and keep the
         * wall-clock anchor in step so pacing continues seamlessly if the
         * audio clock goes away (audio track ends, decode starves it). */
        ctx->start_ticks = now - (Uint64)(frame->pts > 0 ? frame->pts : 0);
        ctx->have_frame = 1;
        SDL_UpdateYUVTexture(ctx->texture, NULL,
            frame->video.planes[0], frame->video.stride[0],
            frame->video.planes[1], frame->video.stride[1],
            frame->video.planes[2], frame->video.stride[2]);
        maybe_redraw(ctx);
        ctx->frames_rendered++;
        return 0;
    }

    Uint64 target = ctx->start_ticks + (Uint64)frame->pts;

    if (target > now) {
        SDL_Delay((Uint32)(target - now));
    } else {
        Uint64 lateness = now - target;
        if (lateness > LUMEN_SDL2_DROP_THRESHOLD_MS) {
            ctx->frames_dropped++;
            return 0;
        }
    }

    ctx->have_frame = 1;
    SDL_UpdateYUVTexture(ctx->texture, NULL,
        frame->video.planes[0], frame->video.stride[0],
        frame->video.planes[1], frame->video.stride[1],
        frame->video.planes[2], frame->video.stride[2]);

    maybe_redraw(ctx);
    ctx->frames_rendered++;
    return 0;
}

static void sdl2_close(lumen_output_ctx_t *ctx) {
    if (!ctx) return;
    printf("  [sdl2-video] closed -- %ld frames rendered, %ld dropped (too stale to bother)\n",
           ctx->frames_rendered, ctx->frames_dropped);
    /* Join the dialog thread if it's still running -- must happen before
     * we free ctx, because the thread holds a pointer to ctx. */
    if (ctx->dialog_thread) {
        SDL_WaitThread(ctx->dialog_thread, NULL);
        ctx->dialog_thread = NULL;
    }
    if (ctx->dialog_mutex) {
        SDL_DestroyMutex(ctx->dialog_mutex);
        ctx->dialog_mutex = NULL;
    }
    /* Tear down the package manager window and its own ImGui context */
    aux_window_destroy(&ctx->pkg_window, &ctx->pkg_renderer, &ctx->pkg_imgui_ctx);
    aux_window_destroy(&ctx->pref_window, &ctx->pref_renderer, &ctx->pref_imgui_ctx);

    /* Tear down the main window's ImGui context */
    ImGui::SetCurrentContext(ctx->main_imgui_ctx);
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    if (ctx->texture) SDL_DestroyTexture(ctx->texture);
    if (ctx->renderer) SDL_DestroyRenderer(ctx->renderer);
    if (ctx->window) SDL_DestroyWindow(ctx->window);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    free(ctx);
}

static const lumen_video_output_vtable_t VTABLE = {
    sdl2_open,
    sdl2_load_stream,
    sdl2_bind_state,
    sdl2_pump_ui,
    sdl2_present,
    sdl2_close,
};

static lumen_plugin_descriptor_t make_descriptor() {
    lumen_plugin_descriptor_t d;
    memset(&d, 0, sizeof(d));
    d.abi_version = LUMEN_ABI_VERSION;
    d.kind = LUMEN_PLUGIN_OUTPUT_VIDEO;
    d.name = "sdl2-video-output";
    d.version = "0.3.0 (persistent window)";
    d.vtable.video_output = &VTABLE;
    return d;
}
static const lumen_plugin_descriptor_t DESCRIPTOR = make_descriptor();

/* DLL export -- used when loaded as a plugin (not used when LUMEN_BUILTIN is
 * defined, i.e. when this source is compiled into the main executable). */
#ifndef LUMEN_BUILTIN
extern "C" LUMEN_EXPORT const lumen_plugin_descriptor_t *lumen_get_plugin(void) {
    return &DESCRIPTOR;
}
#endif

/* Built-in accessor -- called directly by player_core.c when compiled into exe. */
extern "C" const lumen_plugin_descriptor_t *lumen_video_output_builtin(void) {
    return &DESCRIPTOR;
}
