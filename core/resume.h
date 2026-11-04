/* resume.h -- remember where unfinished videos were stopped.
 *
 * A small text file next to preferences.ini (resume.txt, in
 * ~/.config/qqvideo/ or %APPDATA%\qqvideo\) holding, per video, the
 * position it was stopped at. On the next open the player offers to
 * continue from there.
 *
 * A video is identified by its full path plus its size, so a different
 * file later saved under the same name isn't mistaken for it.
 *
 * Rules, so the list only holds things you'd actually want to resume:
 *   - videos shorter than 2 minutes aren't remembered;
 *   - a position in the first 30 s doesn't overwrite a saved one
 *     (opening a file and closing it again keeps its old spot);
 *   - watching to the end (the last 5%, at least the last 30 s, i.e.
 *     the credits) forgets the video;
 *   - at most 300 videos, the least recently played dropped first.
 */
#ifndef LUMEN_RESUME_H
#define LUMEN_RESUME_H

#include <stdint.h>

/* Saved position for `path`, if there is one: returns 1 and sets *pos_ms. */
int  lumen_resume_lookup(const char *path, int64_t *pos_ms);

/* Record that `path` is at pos_ms of dur_ms (on close, and periodically
 * while playing). Applies the rules above: saves, forgets, or leaves the
 * entry alone. */
void lumen_resume_update(const char *path, int64_t pos_ms, int64_t dur_ms);

/* Forget every saved position (Preferences > Playback). */
void lumen_resume_clear(void);

#endif
