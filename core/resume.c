/* resume.c -- see resume.h.
 *
 * File format, one video per line, most recently played first:
 *     <position ms>\t<duration ms>\t<file size>\t<saved at, unix time>\t<full path>
 * The path is the last field, so it may itself contain tabs. Paths with
 * newlines aren't stored. Written to resume.txt.tmp and renamed over the
 * old file, so a crash mid-write can't leave it half-written. */
#include "resume.h"
#include "prefs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#if defined(_WIN32)
  #include <windows.h>
#else
  #include <limits.h>
#endif

#define RESUME_MAX_ENTRIES   300
#define RESUME_MIN_DURATION  120000   /* don't remember clips under 2 min */
#define RESUME_MIN_POSITION   30000   /* positions in the first 30 s don't count */
#define RESUME_TAIL_MIN       30000   /* "finished" = within the last 30 s ... */
#define RESUME_TAIL_PERCENT       5   /* ... or the last 5%, whichever is longer */
#define RESUME_PATH_MAX        4096

typedef struct {
    long long pos_ms, dur_ms;
    long long size;
    long long saved_at;
    char *path;
} entry_t;

typedef struct { entry_t *v; int n, cap; } list_t;

static int resume_file(char *out, size_t n) {
    char dir[480];
    if (lumen_prefs_config_dir(dir, sizeof(dir), 1) != 0) return -1;
#if defined(_WIN32)
    snprintf(out, n, "%s\\resume.txt", dir);
#else
    snprintf(out, n, "%s/resume.txt", dir);
#endif
    return 0;
}

/* Full, absolute path (the queue may hold relative ones) and file size.
 * Returns 0 if the file exists. */
static int identify(const char *path, char *abs, size_t n, long long *size) {
#if defined(_WIN32)
    if (!_fullpath(abs, path, n)) return -1;
    struct _stati64 st;
    if (_stati64(abs, &st) != 0) return -1;
#else
    char tmp[PATH_MAX];
    if (!realpath(path, tmp)) return -1;
    if (strlen(tmp) >= n) return -1;
    strcpy(abs, tmp);
    struct stat st;
    if (stat(abs, &st) != 0) return -1;
#endif
    *size = (long long)st.st_size;
    return 0;
}

static void list_free(list_t *l) {
    for (int i = 0; i < l->n; i++) free(l->v[i].path);
    free(l->v);
    l->v = NULL; l->n = l->cap = 0;
}

static int list_push(list_t *l, entry_t e) {
    if (l->n == l->cap) {
        int ncap = l->cap ? l->cap * 2 : 64;
        entry_t *nv = (entry_t *)realloc(l->v, (size_t)ncap * sizeof(*nv));
        if (!nv) return -1;
        l->v = nv; l->cap = ncap;
    }
    l->v[l->n++] = e;
    return 0;
}

static void list_load(list_t *l) {
    char file[540];
    if (resume_file(file, sizeof(file)) != 0) return;
    FILE *f = fopen(file, "r");
    if (!f) return;
    char line[RESUME_PATH_MAX + 128];
    while (l->n < RESUME_MAX_ENTRIES && fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        if (len == 0 || line[len - 1] != '\n') {            /* too long: skip to the next line */
            int c;
            while (len && line[len - 1] != '\n' && (c = fgetc(f)) != EOF && c != '\n') {}
            continue;
        }
        line[--len] = '\0';
        if (len && line[len - 1] == '\r') line[--len] = '\0';
        entry_t e;
        int consumed = 0;
        if (sscanf(line, "%lld\t%lld\t%lld\t%lld\t%n", &e.pos_ms, &e.dur_ms,
                   &e.size, &e.saved_at, &consumed) != 4 || consumed <= 0 || !line[consumed])
            continue;                                         /* malformed line: ignore */
        if (e.pos_ms < 0 || e.dur_ms <= 0 || e.pos_ms > e.dur_ms) continue;
        e.path = strdup(line + consumed);
        if (!e.path || list_push(l, e) != 0) { free(e.path); break; }
    }
    fclose(f);
}

static int list_save(const list_t *l) {
    char file[540], tmp[560];
    if (resume_file(file, sizeof(file)) != 0) return -1;
    snprintf(tmp, sizeof(tmp), "%s.tmp", file);
    FILE *f = fopen(tmp, "w");
    if (!f) return -1;
    for (int i = 0; i < l->n && i < RESUME_MAX_ENTRIES; i++)
        fprintf(f, "%lld\t%lld\t%lld\t%lld\t%s\n", (long long)l->v[i].pos_ms, (long long)l->v[i].dur_ms,
                l->v[i].size, l->v[i].saved_at, l->v[i].path);
    if (fclose(f) != 0) { remove(tmp); return -1; }
#if defined(_WIN32)
    /* MoveFileEx replaces in one step (plain rename() fails if the target exists). */
    return MoveFileExA(tmp, file, MOVEFILE_REPLACE_EXISTING) ? 0 : -1;
#else
    return rename(tmp, file);
#endif
}

static int list_find(const list_t *l, const char *abs) {
    for (int i = 0; i < l->n; i++)
        if (strcmp(l->v[i].path, abs) == 0) return i;
    return -1;
}

static void list_remove(list_t *l, int i) {
    free(l->v[i].path);
    memmove(&l->v[i], &l->v[i + 1], (size_t)(l->n - i - 1) * sizeof(entry_t));
    l->n--;
}

int lumen_resume_lookup(const char *path, int64_t *pos_ms) {
    char abs[RESUME_PATH_MAX];
    long long size;
    if (identify(path, abs, sizeof(abs), &size) != 0) return 0;
    list_t l = {0};
    list_load(&l);
    int found = 0, i = list_find(&l, abs);
    if (i >= 0 && l.v[i].size == size) {   /* same name, different size: a different file */
        *pos_ms = (int64_t)l.v[i].pos_ms;
        found = 1;
    }
    list_free(&l);
    return found;
}

void lumen_resume_update(const char *path, int64_t pos_ms, int64_t dur_ms) {
    if (dur_ms < RESUME_MIN_DURATION || pos_ms < 0) return;
    char abs[RESUME_PATH_MAX];
    long long size;
    if (identify(path, abs, sizeof(abs), &size) != 0) return;
    if (strchr(abs, '\n') || strchr(abs, '\r')) return;   /* can't be stored on one line */

    int64_t tail = dur_ms * RESUME_TAIL_PERCENT / 100;
    if (tail < RESUME_TAIL_MIN) tail = RESUME_TAIL_MIN;
    const int finished = pos_ms >= dur_ms - tail;
    if (!finished && pos_ms < RESUME_MIN_POSITION) return;   /* barely started: keep any old spot */

    list_t l = {0};
    list_load(&l);
    int i = list_find(&l, abs);
    if (finished) {
        if (i < 0) { list_free(&l); return; }                  /* nothing to forget */
        list_remove(&l, i);
    } else {
        entry_t e = { (long long)pos_ms, (long long)dur_ms, size, (long long)time(NULL), NULL };
        if (i >= 0) {
            if (l.v[i].pos_ms == (long long)pos_ms && i == 0) { list_free(&l); return; }   /* unchanged */
            e.path = l.v[i].path;                                   /* reuse, move to the front */
            memmove(&l.v[1], &l.v[0], (size_t)i * sizeof(entry_t));
        } else {
            e.path = strdup(abs);
            if (!e.path) { list_free(&l); return; }
            if (list_push(&l, e) != 0) { free(e.path); list_free(&l); return; }   /* make room */
            memmove(&l.v[1], &l.v[0], (size_t)(l.n - 1) * sizeof(entry_t));
        }
        l.v[0] = e;
    }
    if (list_save(&l) != 0) fprintf(stderr, "lumen: couldn't save resume positions\n");
    list_free(&l);
}

void lumen_resume_clear(void) {
    char file[540];
    if (resume_file(file, sizeof(file)) != 0) return;
    remove(file);
    printf("lumen: forgot all saved resume positions\n");
}
