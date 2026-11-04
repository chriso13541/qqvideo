/* snapshot.h -- Video > Take Snapshot: save a picture as a PNG in the
 * user's Pictures folder, named qqvideo-<date>_<time>.png.
 *
 * The PNG is encoded and written on a background thread (a 4K frame takes
 * a moment to compress, and playback mustn't stall for it). The UI thread
 * polls for the outcome to tell the user where it went, or why it didn't.
 *
 * Paths are UTF-8 on every platform. The Pictures folder is:
 *   Windows  the shell's Pictures folder (follows OneDrive/redirection)
 *   Linux    XDG_PICTURES_DIR from user-dirs.dirs, else ~/Pictures
 * and is created if it doesn't exist yet. An existing file is never
 * overwritten: a second snapshot in the same second gets "-2", and so on.
 */
#pragma once
#include <stddef.h>

/* Start saving a w x h RGB picture (3 bytes per pixel, rows top to bottom,
 * no padding). Takes ownership of `rgb` (malloc'd), whatever happens.
 * Returns 0 if the save was started, -1 if it couldn't be (then `err`
 * says why). */
int lumen_snapshot_save(unsigned char *rgb, int w, int h, char *err, size_t err_size);

/* Outcome of the most recent finished save. Returns 1 when a save has
 * finished since the last call (filling ok and msg: the file's path, or
 * the reason it failed), else 0. */
int lumen_snapshot_poll(int *ok, char *msg, size_t msg_size);

/* Wait (up to max_ms) for saves still in progress, so quitting right
 * after a snapshot doesn't leave half a PNG behind. */
void lumen_snapshot_wait(int max_ms);

/* The user's Videos folder (Windows known folder / XDG_VIDEOS_DIR /
 * ~/Videos), created if missing, as UTF-8. Video > Clip saves there.
 * Returns 0, or -1 if it can't be found or made. */
int lumen_videos_dir(char *out, size_t out_size);

/* "<prefix>-2026-11-04_00-22-15" for the local time now: the shared
 * naming of snapshots and clips. */
void lumen_media_stamp(const char *prefix, char *out, size_t out_size);
