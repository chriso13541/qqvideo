#include "subtitles.h"
#include <stdlib.h>
#include <string.h>

/* A cue with no end time (-1) that is also the LAST cue would otherwise
 * stay on screen forever; cap it. Real players use ~5s for the same case. */
#define OPEN_ENDED_CAP_MS 5000
/* How far back to look for long cues that are still active. Overlaps are
 * rare (song lyrics over dialogue, signs), so a small window is plenty. */
#define LOOKBACK 16

void lumen_sub_track_free(lumen_sub_track_t *t) {
    for (int i = 0; i < t->count; i++) free(t->cues[i].text);
    free(t->cues);
    memset(t, 0, sizeof(*t));
}

/* index of the first cue with start_ms > x */
static int upper_bound(const lumen_sub_track_t *t, int64_t x) {
    int lo = 0, hi = t->count;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (t->cues[mid].start_ms <= x) lo = mid + 1; else hi = mid;
    }
    return lo;
}

int lumen_sub_track_add(lumen_sub_track_t *t, int64_t start_ms, int64_t end_ms, const char *text) {
    int pos = upper_bound(t, start_ms);
    /* duplicates share start_ms, so they sit immediately before pos */
    for (int i = pos - 1; i >= 0 && t->cues[i].start_ms == start_ms; i--)
        if (strcmp(t->cues[i].text, text) == 0) return 0;

    if (t->count == t->cap) {
        int ncap = t->cap ? t->cap * 2 : 256;
        lumen_cue_t *n = (lumen_cue_t *)realloc(t->cues, (size_t)ncap * sizeof(*n));
        if (!n) return 0;
        t->cues = n;
        t->cap = ncap;
    }
    char *copy = strdup(text);
    if (!copy) return 0;
    memmove(&t->cues[pos + 1], &t->cues[pos], (size_t)(t->count - pos) * sizeof(lumen_cue_t));
    t->cues[pos].start_ms = start_ms;
    t->cues[pos].end_ms = end_ms;
    t->cues[pos].text = copy;
    t->count++;
    return 1;
}

static int64_t effective_end(const lumen_sub_track_t *t, int i) {
    const lumen_cue_t *c = &t->cues[i];
    if (c->end_ms >= 0) return c->end_ms;
    for (int j = i + 1; j < t->count; j++)          /* next cue that starts later */
        if (t->cues[j].start_ms > c->start_ms) return t->cues[j].start_ms;
    return c->start_ms + OPEN_ENDED_CAP_MS;
}

int lumen_sub_track_text_at(const lumen_sub_track_t *t, int64_t t_ms, char *out, size_t n) {
    out[0] = '\0';
    if (!t || t->count == 0 || n == 0) return 0;
    int last = upper_bound(t, t_ms) - 1;   /* last cue that has started */
    int first = last - LOOKBACK + 1;
    if (first < 0) first = 0;

    int active = 0;
    size_t len = 0;
    for (int i = first; i <= last; i++) {
        if (t_ms >= effective_end(t, i)) continue;
        const char *s = t->cues[i].text;
        size_t sl = strlen(s);
        if (active > 0 && len + 1 < n) out[len++] = '\n';
        if (len + sl >= n) sl = n - len - 1;
        memcpy(out + len, s, sl);
        len += sl;
        out[len] = '\0';
        active++;
    }
    return active;
}
