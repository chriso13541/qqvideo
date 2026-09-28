#include "prefs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
  #include <direct.h>
  #define MKDIR(p) _mkdir(p)
#else
  #include <sys/stat.h>
  #define MKDIR(p) mkdir(p, 0755)
#endif

void lumen_prefs_defaults(lumen_playback_state_t *st) {
    st->precise_seek = 1;
    st->pref_frameskip = 1;
    st->pref_buffer_enabled = 0;
    st->pref_buffer_seconds = 10;
    st->pref_buffer_mb = 512;
    st->pref_single_instance = 0;
    st->pref_instance_enqueue = 0;
    st->pref_remember_volume = 1;
    st->pref_resize_window = 1;
    st->pref_seek_step_s = 10;
    st->pref_decode_threads = 0;
    st->pref_scale_smooth = 1;
    st->pref_keep_aspect = 1;
    st->pref_audio_device[0] = '\0';
    st->pref_volume_step = 5;
    st->pref_av_offset_ms = 0;
    st->pref_sub_autoload = 1;
    st->pref_sub_lang[0] = '\0';
    st->pref_sub_scale_pct = 100;
}

/* Builds the config directory path; creates it if `create`. */
static int config_dir(char *out, size_t n, int create) {
#if defined(_WIN32)
    const char *base = getenv("APPDATA");
    if (!base || !*base) return -1;
    snprintf(out, n, "%s\\qqvideo", base);
#else
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    if (xdg && *xdg) {
        if (create) MKDIR(xdg);           /* the base may not exist yet on a fresh account */
        snprintf(out, n, "%s/qqvideo", xdg);
    } else if (home && *home) {
        char cfg[512];
        snprintf(cfg, sizeof(cfg), "%s/.config", home);
        if (create) MKDIR(cfg);
        snprintf(out, n, "%s/qqvideo", cfg);
    } else {
        return -1;
    }
#endif
    if (create) MKDIR(out);   /* fine if it already exists */
    return 0;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

void lumen_prefs_load(lumen_playback_state_t *st) {
    char dir[480];
    st->prefs_path[0] = '\0';
    if (config_dir(dir, sizeof(dir), 0) != 0) return;
#if defined(_WIN32)
    snprintf(st->prefs_path, sizeof(st->prefs_path), "%s\\preferences.ini", dir);
#else
    snprintf(st->prefs_path, sizeof(st->prefs_path), "%s/preferences.ini", dir);
#endif
    FILE *f = fopen(st->prefs_path, "r");
    if (!f) return;                               /* first run: defaults */
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char key[64], sval[256] = "";
        int v = 0;
        if (line[0] == '#') continue;
        /* strings first ("key = anything to end of line"), then numbers */
        if (sscanf(line, " %63[^= ] = %255[^\r\n]", key, sval) < 1) continue;
        if (!strcmp(key, "audio_device")) { snprintf(st->pref_audio_device, sizeof(st->pref_audio_device), "%s", sval); continue; }
        if (!strcmp(key, "subtitle_language")) { snprintf(st->pref_sub_lang, sizeof(st->pref_sub_lang), "%s", sval); continue; }
        if (sscanf(sval, "%d", &v) != 1) continue;
        if      (!strcmp(key, "precise_seek"))    st->precise_seek = v != 0;
        else if (!strcmp(key, "frame_skipping"))  st->pref_frameskip = v != 0;
        else if (!strcmp(key, "buffer_enabled"))  st->pref_buffer_enabled = v != 0;
        else if (!strcmp(key, "buffer_seconds"))  st->pref_buffer_seconds = clampi(v, 1, 120);
        else if (!strcmp(key, "buffer_mb"))       st->pref_buffer_mb = clampi(v, 64, 16384);
        else if (!strcmp(key, "single_instance")) st->pref_single_instance = v != 0;
        else if (!strcmp(key, "instance_enqueue")) st->pref_instance_enqueue = v != 0;
        else if (!strcmp(key, "remember_volume")) st->pref_remember_volume = v != 0;
        else if (!strcmp(key, "volume"))          st->volume = (float)clampi(v, 0, 100) / 100.0f;
        else if (!strcmp(key, "resize_window"))   st->pref_resize_window = v != 0;
        else if (!strcmp(key, "seek_step"))       st->pref_seek_step_s = clampi(v, 1, 300);
        else if (!strcmp(key, "decode_threads"))  st->pref_decode_threads = clampi(v, 0, 64);
        else if (!strcmp(key, "smooth_scaling"))  st->pref_scale_smooth = v != 0;
        else if (!strcmp(key, "keep_aspect"))     st->pref_keep_aspect = v != 0;
        else if (!strcmp(key, "volume_step"))     st->pref_volume_step = clampi(v, 1, 25);
        else if (!strcmp(key, "audio_delay_ms"))  st->pref_av_offset_ms = clampi(v, -2000, 2000);
        else if (!strcmp(key, "subtitle_autoload")) st->pref_sub_autoload = v != 0;
        else if (!strcmp(key, "subtitle_size"))   st->pref_sub_scale_pct = clampi(v, 50, 250);
    }
    fclose(f);
}

int lumen_prefs_save(const lumen_playback_state_t *st) {
    char dir[480];
    if (!st->prefs_path[0] || config_dir(dir, sizeof(dir), 1) != 0) return -1;
    char tmp[540];
    snprintf(tmp, sizeof(tmp), "%s.tmp", st->prefs_path);
    FILE *f = fopen(tmp, "w");
    if (!f) return -1;
    fprintf(f, "# qqvideo preferences -- written by Tools > Preferences\n");
    fprintf(f, "precise_seek = %d\n", st->precise_seek ? 1 : 0);
    fprintf(f, "frame_skipping = %d\n", st->pref_frameskip ? 1 : 0);
    fprintf(f, "buffer_enabled = %d\n", st->pref_buffer_enabled ? 1 : 0);
    fprintf(f, "buffer_seconds = %d\n", st->pref_buffer_seconds);
    fprintf(f, "buffer_mb = %d\n", st->pref_buffer_mb);
    fprintf(f, "single_instance = %d\n", st->pref_single_instance ? 1 : 0);
    fprintf(f, "instance_enqueue = %d\n", st->pref_instance_enqueue ? 1 : 0);
    fprintf(f, "remember_volume = %d\n", st->pref_remember_volume ? 1 : 0);
    fprintf(f, "volume = %d\n", (int)(st->volume * 100.0f + 0.5f));
    fprintf(f, "resize_window = %d\n", st->pref_resize_window ? 1 : 0);
    fprintf(f, "seek_step = %d\n", st->pref_seek_step_s);
    fprintf(f, "decode_threads = %d\n", st->pref_decode_threads);
    fprintf(f, "smooth_scaling = %d\n", st->pref_scale_smooth ? 1 : 0);
    fprintf(f, "keep_aspect = %d\n", st->pref_keep_aspect ? 1 : 0);
    fprintf(f, "audio_device = %s\n", st->pref_audio_device);
    fprintf(f, "volume_step = %d\n", st->pref_volume_step);
    fprintf(f, "audio_delay_ms = %d\n", st->pref_av_offset_ms);
    fprintf(f, "subtitle_autoload = %d\n", st->pref_sub_autoload ? 1 : 0);
    fprintf(f, "subtitle_language = %s\n", st->pref_sub_lang);
    fprintf(f, "subtitle_size = %d\n", st->pref_sub_scale_pct);
    if (fclose(f) != 0) return -1;
    /* write-then-rename, so a crash mid-save can't leave a truncated file */
#if defined(_WIN32)
    remove(st->prefs_path);
#endif
    return rename(tmp, st->prefs_path);
}
