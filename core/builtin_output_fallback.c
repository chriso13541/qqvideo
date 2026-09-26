/*
 * builtin_output_fallback.c
 *
 * Weak-symbol stubs for the built-in video/audio output functions.
 * When output_sdl2.cpp and output_sdl2_audio.c are compiled into lumen-play
 * (SDL2 present), they provide the real implementations and these stubs are
 * silently overridden by the linker. When SDL2 is absent these stubs are used,
 * which lets the exe link cleanly and gives a clear runtime error rather than a
 * build failure.
 */
#include "../include/lumen_plugin.h"

__attribute__((weak))
const lumen_plugin_descriptor_t *lumen_video_output_builtin(void) {
    return NULL; /* overridden by output_sdl2.cpp when SDL2 is available */
}

__attribute__((weak))
const lumen_plugin_descriptor_t *lumen_audio_output_builtin(void) {
    return NULL; /* overridden by output_sdl2_audio.c when SDL2 is available */
}
