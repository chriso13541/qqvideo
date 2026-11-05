#include "subtitles.h"
#include <stdlib.h>
#include <string.h>

/* A cue with no end time (-1) that is also the LAST cue would otherwise
 * stay on screen forever; cap it. Real players use ~5s for the same case. */
#define OPEN_ENDED_CAP_MS 5000
/* How far back to look for long cues that are still active. Overlaps are
 * rare (song lyrics over dialogue, signs), so a small window is plenty. */
#define LOOKBACK 16

static void image_free(lumen_sub_image_t *im) {
    if (!im) return;
    for (int i = 0; i < im->count; i++) free(im->rects[i].rgba);
    free(im->rects);
    free(im);
}

static lumen_sub_image_t *image_copy(const lumen_sub_image_t *src) {
    lumen_sub_image_t *im = (lumen_sub_image_t *)calloc(1, sizeof(*im));
    if (!im) return NULL;
    im->canvas_w = src->canvas_w;
    im->canvas_h = src->canvas_h;
    im->rects = (lumen_sub_rect_t *)calloc((size_t)(src->count > 0 ? src->count : 1), sizeof(lumen_sub_rect_t));
    if (!im->rects) { free(im); return NULL; }
    for (int i = 0; i < src->count; i++) {
        const lumen_sub_rect_t *r = &src->rects[i];
        if (r->w <= 0 || r->h <= 0 || !r->rgba) continue;
        const size_t n = (size_t)r->w * r->h * 4;
        uint8_t *px = (uint8_t *)malloc(n);
        if (!px) { image_free(im); return NULL; }
        memcpy(px, r->rgba, n);
        im->rects[im->count] = *r;
        im->rects[im->count].rgba = px;
        im->count++;
    }
    return im;
}

static int same_image(const lumen_sub_image_t *a, const lumen_sub_image_t *b) {
    if (!a || !b) return a == b;
    if (a->count != b->count) return 0;
    for (int i = 0; i < a->count; i++)
        if (a->rects[i].x != b->rects[i].x || a->rects[i].y != b->rects[i].y ||
            a->rects[i].w != b->rects[i].w || a->rects[i].h != b->rects[i].h) return 0;
    return 1;
}

void lumen_sub_track_free(lumen_sub_track_t *t) {
    for (int i = 0; i < t->count; i++) { free(t->cues[i].text); image_free(t->cues[i].image); }
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

static int insert_cue(lumen_sub_track_t *t, int pos, int64_t start_ms, int64_t end_ms, char *text,
                      lumen_sub_image_t *image, int is_image);

int lumen_sub_track_add(lumen_sub_track_t *t, int64_t start_ms, int64_t end_ms, const char *text) {
    int pos = upper_bound(t, start_ms);
    /* duplicates share start_ms, so they sit immediately before pos */
    for (int i = pos - 1; i >= 0 && t->cues[i].start_ms == start_ms; i--)
        if (!t->cues[i].is_image && strcmp(t->cues[i].text, text) == 0) return 0;
    char *copy = strdup(text);
    if (!copy) return 0;
    if (insert_cue(t, pos, start_ms, end_ms, copy, NULL, 0)) return 1;
    free(copy);
    return 0;
}

int lumen_sub_track_add_image(lumen_sub_track_t *t, int64_t start_ms, int64_t end_ms,
                              const lumen_sub_image_t *image) {
    int pos = upper_bound(t, start_ms);
    for (int i = pos - 1; i >= 0 && t->cues[i].start_ms == start_ms; i--)   /* re-delivered after a seek */
        if (t->cues[i].is_image && same_image(t->cues[i].image, image)) return 0;
    char *empty = strdup("");
    lumen_sub_image_t *copy = image ? image_copy(image) : NULL;
    if (!empty || (image && !copy)) { free(empty); image_free(copy); return 0; }
    if (insert_cue(t, pos, start_ms, end_ms, empty, copy, 1)) return 1;
    free(empty);
    image_free(copy);
    return 0;
}

static int insert_cue(lumen_sub_track_t *t, int pos, int64_t start_ms, int64_t end_ms, char *text,
                      lumen_sub_image_t *image, int is_image) {

    if (t->count == t->cap) {
        int ncap = t->cap ? t->cap * 2 : 256;
        lumen_cue_t *n = (lumen_cue_t *)realloc(t->cues, (size_t)ncap * sizeof(*n));
        if (!n) return 0;
        t->cues = n;
        t->cap = ncap;
    }
    memmove(&t->cues[pos + 1], &t->cues[pos], (size_t)(t->count - pos) * sizeof(lumen_cue_t));
    t->cues[pos].start_ms = start_ms;
    t->cues[pos].end_ms = end_ms;
    t->cues[pos].text = text;
    t->cues[pos].image = image;
    t->cues[pos].is_image = is_image;
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
        if (t->cues[i].is_image) continue;            /* drawn as an image, not text */
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

const lumen_sub_image_t *lumen_sub_track_image_at(const lumen_sub_track_t *t, int64_t t_ms) {
    if (!t || t->count == 0) return NULL;
    /* An image track shows one picture at a time; each event replaces the
     * previous one (a clear event shows nothing). So: the latest image or
     * clear that has started, if it hasn't ended. */
    for (int i = upper_bound(t, t_ms) - 1; i >= 0; i--) {
        if (!t->cues[i].is_image) continue;
        if (!t->cues[i].image || t_ms >= effective_end(t, i)) return NULL;
        return t->cues[i].image;
    }
    return NULL;
}
