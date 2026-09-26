/*
 * plugin_loader.h
 *
 * Thin cross-platform wrapper around dlopen/dlsym (POSIX) and
 * LoadLibrary/GetProcAddress (Windows). This is the ONLY place in the
 * core that touches OS-specific dynamic loading -- everything above this
 * layer just works with lumen_plugin_descriptor_t.
 */
#ifndef LUMEN_PLUGIN_LOADER_H
#define LUMEN_PLUGIN_LOADER_H

#include "../include/lumen_plugin.h"

typedef struct lumen_loaded_plugin {
    void *os_handle;                          /* HMODULE or void* from dlopen */
    const lumen_plugin_descriptor_t *desc;    /* result of calling the entrypoint */
    char path[512];
} lumen_loaded_plugin_t;

/* Loads the shared library at `path`, resolves lumen_get_plugin, calls it,
 * and validates abi_version. Returns 0 on success. On failure, *out_err
 * is set to a human-readable static string (do not free). */
int lumen_load_plugin(const char *path, lumen_loaded_plugin_t *out_plugin, const char **out_err);

void lumen_unload_plugin(lumen_loaded_plugin_t *plugin);

#endif
