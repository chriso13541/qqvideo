/* subtitles.h -- per-track cue storage and "what's on screen at time t".
 *
 * Cues arrive from decoders as the demuxer reads ahead (embedded tracks)
 * or all at once (an external .srt loaded via "Add Subtitle Track...").
 * Tracks are kept sorted by start time. Seeking backwards makes the
 * demuxer deliver cues it already delivered, so exact duplicates are
 * dropped on insert rather than stored twice. */
#ifndef LUMEN_SUBTITLES_H
#define LUMEN_SUBTITLES_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    int64_t start_ms;
    int64_t end_ms;     /* -1 = until the next cue starts */
    char   *text;
} lumen_cue_t;

typedef struct {
    lumen_cue_t *cues;
    int          count, cap;
} lumen_sub_track_t;

void lumen_sub_track_free(lumen_sub_track_t *t);

/* Takes a copy of `text`. Returns 1 if added, 0 if it was a duplicate. */
int  lumen_sub_track_add(lumen_sub_track_t *t, int64_t start_ms, int64_t end_ms, const char *text);

/* Writes every cue active at t_ms (overlapping cues joined by '\n') into
 * out. Returns the number of active cues (0 => out is ""). */
int  lumen_sub_track_text_at(const lumen_sub_track_t *t, int64_t t_ms, char *out, size_t n);

#endif
