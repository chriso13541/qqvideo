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

#include "../include/lumen_plugin.h"   /* lumen_sub_image_t */

typedef struct {
    int64_t start_ms;
    int64_t end_ms;     /* -1 = until the next cue starts */
    char   *text;       /* "" for image cues */
    lumen_sub_image_t *image;   /* image subtitle (own copy), NULL = text cue or a "clear" */
    int     is_image;   /* from an image track: an image, or a clear (image NULL) */
} lumen_cue_t;

typedef struct {
    lumen_cue_t *cues;
    int          count, cap;
} lumen_sub_track_t;

void lumen_sub_track_free(lumen_sub_track_t *t);

/* Takes a copy of `text`. Returns 1 if added, 0 if it was a duplicate. */
int  lumen_sub_track_add(lumen_sub_track_t *t, int64_t start_ms, int64_t end_ms, const char *text);

/* An image cue (copied), or with image == NULL a "clear the screen" event
 * of an image track. Returns 1 if added, 0 if a duplicate (or no memory). */
int  lumen_sub_track_add_image(lumen_sub_track_t *t, int64_t start_ms, int64_t end_ms,
                               const lumen_sub_image_t *image);

/* The image on screen at t_ms: the most recent cue that has started, if
 * it's an image that hasn't ended. NULL = none. Valid until the track is
 * freed (images never move once stored). */
const lumen_sub_image_t *lumen_sub_track_image_at(const lumen_sub_track_t *t, int64_t t_ms);

/* Writes every cue active at t_ms (overlapping cues joined by '\n') into
 * out. Returns the number of active cues (0 => out is ""). */
int  lumen_sub_track_text_at(const lumen_sub_track_t *t, int64_t t_ms, char *out, size_t n);

#endif
