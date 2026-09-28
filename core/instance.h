/* instance.h -- "one qqvideo at a time" (Preferences > General).
 *
 * The running qqvideo listens on a per-user local socket:
 *   $XDG_RUNTIME_DIR/qqvideo.sock  (fallback /tmp/qqvideo-<uid>.sock, mode 0600)
 * A second launch with files connects, hands them over as text lines
 *   OPEN <absolute path>   play now (replaces the queue)
 *   ADD <absolute path>    append to the queue
 *   RAISE                  bring the window to the front
 * and exits. Linux/POSIX only for now; on Windows these are no-ops and every
 * launch opens its own window. */
#ifndef LUMEN_INSTANCE_H
#define LUMEN_INSTANCE_H

#include "lumen_plugin.h"

/* Returns 1 if another instance took the files (caller should exit),
 * 0 if none is running (caller becomes the instance). */
int  lumen_instance_forward(const char *const *paths, int n, int enqueue);

/* Called from the UI loops: starts/stops listening as the preference
 * changes, and turns incoming messages into the same requests the File
 * menu makes (open_file_requested / pending_add_paths / raise). */
void lumen_instance_poll(lumen_playback_state_t *st);

void lumen_instance_shutdown(void);

#endif
