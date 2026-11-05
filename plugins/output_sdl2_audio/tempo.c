/* tempo.c -- see tempo.h */
#include "tempo.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

struct tempo {
    int     ch, rate;
    int     win;                /* W: window length, frames (~30 ms) */
    int     hop;                /* W/2: output hop (and the overlap) */
    int     search;             /* +- this many frames to find the best seam (~10 ms) */
    float  *hann;               /* W */

    /* Input not yet consumed: frames [in_base, in_base + in_len). */
    float  *in;                 /* interleaved */
    float  *mono;               /* channel average, for the correlation */
    int64_t in_base;
    int     in_len, in_cap, mono_cap;

    double  ana_pos;            /* where the next slice ideally starts (input frames) */
    int64_t nat_pos;            /* where the previous slice naturally continues; -1 = none yet */
    float  *overlap;            /* second half of the previous windowed slice, hop * ch */

    float  *out;                /* produced, not yet taken */
    int     out_len, out_cap;
};

static int grow(float **buf, int *cap, int need_frames, int ch) {
    if (need_frames <= *cap) return 0;
    int cap2 = *cap ? *cap : 4096;
    while (cap2 < need_frames) cap2 *= 2;
    float *b = (float *)realloc(*buf, (size_t)cap2 * ch * sizeof(float));
    if (!b) return -1;
    *buf = b;
    *cap = cap2;
    return 0;
}

tempo_t *tempo_new(int channels, int sample_rate) {
    if (channels < 1 || sample_rate < 1000) return NULL;
    tempo_t *t = (tempo_t *)calloc(1, sizeof(*t));
    if (!t) return NULL;
    t->ch = channels;
    t->rate = sample_rate;
    t->win = (int)(sample_rate * 0.030) & ~1;
    t->hop = t->win / 2;
    t->search = (int)(sample_rate * 0.010);
    t->hann = (float *)malloc((size_t)t->win * sizeof(float));
    t->overlap = (float *)calloc((size_t)t->hop * channels, sizeof(float));
    if (!t->hann || !t->overlap) { tempo_free(t); return NULL; }
    /* periodic Hann: two halves overlapped at hop = W/2 sum to exactly 1 */
    for (int k = 0; k < t->win; k++) t->hann[k] = (float)(0.5 - 0.5 * cos(2.0 * M_PI * k / t->win));
    tempo_reset(t);
    return t;
}

void tempo_free(tempo_t *t) {
    if (!t) return;
    free(t->hann);
    free(t->in);
    free(t->mono);
    free(t->overlap);
    free(t->out);
    free(t);
}

void tempo_reset(tempo_t *t) {
    t->in_base = 0;
    t->in_len = 0;
    t->ana_pos = 0;
    t->nat_pos = -1;
    t->out_len = 0;
    memset(t->overlap, 0, (size_t)t->hop * t->ch * sizeof(float));
}

double tempo_input_pos(const tempo_t *t) { return t->ana_pos; }

const float *tempo_take(tempo_t *t, int *frames) {
    *frames = t->out_len;
    t->out_len = 0;
    return t->out;
}

/* Normalized cross-correlation of the hop-long stretches at a and b
 * (absolute frame positions), on every 2nd frame: plenty to find a seam. */
static float similarity(const tempo_t *t, int64_t a, int64_t b) {
    const float *x = t->mono + (a - t->in_base), *y = t->mono + (b - t->in_base);
    float xy = 0, xx = 1e-9f;
    for (int k = 0; k < t->hop; k += 2) {
        xy += x[k] * y[k];
        xx += x[k] * x[k];
    }
    return xy / sqrtf(xx);
}

/* One output hop. Returns 0 if more input is needed first. */
static int step(tempo_t *t, double speed) {
    const int64_t ideal = (int64_t)floor(t->ana_pos);
    int64_t lo = ideal - t->search, hi = ideal + t->search;
    if (lo < t->in_base) lo = t->in_base;
    if (t->nat_pos < 0) lo = hi = ideal;                         /* first slice: no seam to match */
    const int64_t in_end = t->in_base + t->in_len;
    if (hi + t->win > in_end) return 0;
    if (t->nat_pos >= 0 && t->nat_pos + t->hop > in_end) return 0;

    int64_t best = ideal < lo ? lo : ideal;
    if (t->nat_pos >= 0 && hi > lo) {
        float best_s = -1e30f;
        for (int64_t c = lo; c <= hi; c++) {
            float s = similarity(t, c, t->nat_pos);
            if (s > best_s) { best_s = s; best = c; }
        }
    }

    /* Overlap-add: first half joins the previous slice's tail. */
    if (grow(&t->out, &t->out_cap, t->out_len + t->hop, t->ch) < 0) return -1;
    const float *seg = t->in + (best - t->in_base) * t->ch;
    float *o = t->out + (size_t)t->out_len * t->ch;
    for (int k = 0; k < t->hop; k++)
        for (int c = 0; c < t->ch; c++)
            o[k * t->ch + c] = t->overlap[k * t->ch + c] + seg[k * t->ch + c] * t->hann[k];
    for (int k = 0; k < t->hop; k++)
        for (int c = 0; c < t->ch; c++)
            t->overlap[k * t->ch + c] = seg[(t->hop + k) * t->ch + c] * t->hann[t->hop + k];
    t->out_len += t->hop;

    t->nat_pos = best + t->hop;
    t->ana_pos += t->hop * speed;

    /* Drop input no future step can reach. */
    int64_t keep_from = (int64_t)floor(t->ana_pos) - t->search;
    if (t->nat_pos < keep_from) keep_from = t->nat_pos;
    int64_t drop = keep_from - t->in_base;
    if (drop > 0) {
        if (drop > t->in_len) drop = t->in_len;
        memmove(t->in, t->in + drop * t->ch, (size_t)(t->in_len - drop) * t->ch * sizeof(float));
        memmove(t->mono, t->mono + drop, (size_t)(t->in_len - drop) * sizeof(float));
        t->in_len -= (int)drop;
        t->in_base += drop;
    }
    return 1;
}

int tempo_feed(tempo_t *t, const int16_t *in, int frames, double speed) {
    if (speed < 0.1) speed = 0.1;
    if (speed > 8.0) speed = 8.0;
    if (grow(&t->in, &t->in_cap, t->in_len + frames, t->ch) < 0) return -1;
    if (grow(&t->mono, &t->mono_cap, t->in_len + frames, 1) < 0) return -1;
    float *dst = t->in + (size_t)t->in_len * t->ch;
    float *mdst = t->mono + t->in_len;
    for (int i = 0; i < frames; i++) {
        float sum = 0;
        for (int c = 0; c < t->ch; c++) {
            float v = (float)in[i * t->ch + c];
            dst[i * t->ch + c] = v;
            sum += v;
        }
        mdst[i] = sum / (float)t->ch;
    }
    t->in_len += frames;
    int r;
    while ((r = step(t, speed)) == 1) { }
    return r < 0 ? -1 : 0;
}
