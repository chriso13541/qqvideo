/* prefs.h -- user preferences, saved between runs.
 *
 * A plain key=value file at the platform's per-user config location:
 *   Linux:   $XDG_CONFIG_HOME/qqvideo/preferences.ini  (~/.config/... by default)
 *   Windows: %APPDATA%\qqvideo\preferences.ini
 * Unknown keys are ignored and missing ones keep their defaults, so the
 * file survives version changes in both directions. */
#ifndef LUMEN_PREFS_H
#define LUMEN_PREFS_H

#include "lumen_plugin.h"

void lumen_prefs_defaults(lumen_playback_state_t *st);
void lumen_prefs_load(lumen_playback_state_t *st);     /* also fills st->prefs_path */
int  lumen_prefs_save(const lumen_playback_state_t *st);

#endif
