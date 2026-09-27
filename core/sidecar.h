/* sidecar.h -- find subtitle files that belong to a media file.
 *
 * Looks next to the movie and in a Subs/ or Subtitles/ folder (any case),
 * including Subs/<movie name>/ as many releases lay out TV episodes.
 *
 *   - Name match: a file whose name starts with the movie's name
 *     ("Movie.srt", "Movie.en.srt", "Movie_eng_forced.srt") always counts.
 *   - If the movie is the ONLY video in its folder, every subtitle file
 *     there counts too ("English.srt", "Subs/2_English.srt").
 *   - With several videos in the folder (a season), only name matches
 *     count, so S01E02's subtitles never attach to S01E01.
 *
 * Results: name matches first, then the rest, each group alphabetical. */
#ifndef LUMEN_SIDECAR_H
#define LUMEN_SIDECAR_H

#include <stddef.h>

#define LUMEN_SIDECAR_MAX 24

typedef struct {
    char path[512];
    char label[96];   /* "English (SDH) - forced [SRT file]" or "commentary [SRT file]" */
    char file[128];   /* file name, for the menu tooltip */
} lumen_sidecar_t;

int lumen_find_sidecar_subs(const char *media_path, lumen_sidecar_t *out, int max);

/* Label for an external subtitle file, parsed from its name. `stem` is the
 * movie's file name without extension (NULL if unknown). */
void lumen_sidecar_label(const char *file_name, const char *stem, char *out, size_t n);

/* "eng"/"en"/"english" -> "English". NULL if not recognized. */
const char *lumen_language_name(const char *code);

#endif
