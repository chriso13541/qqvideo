#include "plugin_loader.h"
#include <string.h>
#include <stdio.h>

#if defined(_WIN32)
  #include <windows.h>
  typedef HMODULE os_handle_t;
  static os_handle_t os_load(const char *path)        { return LoadLibraryA(path); }
  static void       *os_sym(os_handle_t h, const char *n) { return (void *)GetProcAddress(h, n); }
  static void        os_unload(os_handle_t h)          { FreeLibrary(h); }
#else
  #include <dlfcn.h>
  typedef void *os_handle_t;
  static os_handle_t os_load(const char *path)        { return dlopen(path, RTLD_NOW | RTLD_LOCAL); }
  static void       *os_sym(os_handle_t h, const char *n) { return dlsym(h, n); }
  static void        os_unload(os_handle_t h)          { dlclose(h); }
#endif

int lumen_load_plugin(const char *path, lumen_loaded_plugin_t *out_plugin, const char **out_err) {
    memset(out_plugin, 0, sizeof(*out_plugin));

    os_handle_t h = os_load(path);
    if (!h) {
        *out_err = "failed to load shared library (missing file or unresolved symbols)";
        return -1;
    }

    void *sym = os_sym(h, LUMEN_PLUGIN_ENTRYPOINT_NAME);
    if (!sym) {
        os_unload(h);
        *out_err = "library does not export " LUMEN_PLUGIN_ENTRYPOINT_NAME "() -- not a Lumen plugin";
        return -1;
    }

    lumen_get_plugin_fn get_plugin = (lumen_get_plugin_fn)sym;
    const lumen_plugin_descriptor_t *desc = get_plugin();
    if (!desc) {
        os_unload(h);
        *out_err = "plugin entrypoint returned NULL";
        return -1;
    }

    if (desc->abi_version != LUMEN_ABI_VERSION) {
        os_unload(h);
        *out_err = "plugin ABI version mismatch -- rebuild the plugin against the current lumen_plugin.h";
        return -1;
    }

    out_plugin->os_handle = (void *)h;
    out_plugin->desc = desc;
    strncpy(out_plugin->path, path, sizeof(out_plugin->path) - 1);
    return 0;
}

void lumen_unload_plugin(lumen_loaded_plugin_t *plugin) {
    if (plugin->os_handle) {
        os_unload((os_handle_t)plugin->os_handle);
        plugin->os_handle = NULL;
        plugin->desc = NULL;
    }
}
