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
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char key[64];
        int v;
        if (line[0] == '#' || sscanf(line, " %63[^= ] = %d", key, &v) != 2) continue;
        if      (!strcmp(key, "precise_seek"))    st->precise_seek = v != 0;
        else if (!strcmp(key, "frame_skipping"))  st->pref_frameskip = v != 0;
        else if (!strcmp(key, "buffer_enabled"))  st->pref_buffer_enabled = v != 0;
        else if (!strcmp(key, "buffer_seconds"))  st->pref_buffer_seconds = clampi(v, 1, 120);
        else if (!strcmp(key, "buffer_mb"))       st->pref_buffer_mb = clampi(v, 64, 16384);
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
    if (fclose(f) != 0) return -1;
    /* write-then-rename, so a crash mid-save can't leave a truncated file */
#if defined(_WIN32)
    remove(st->prefs_path);
#endif
    return rename(tmp, st->prefs_path);
}
