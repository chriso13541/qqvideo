#include "plugin_loader.h"
#include <string.h>
#include <stdio.h>

#if defined(_WIN32)
  #include <windows.h>
  typedef HMODULE os_handle_t;
  /* Resolve the plugin's own dependencies (e.g. FFmpeg DLLs) from the
   * plugin's folder, the .exe's folder and the system folders only --
   * never the current directory, where a planted avcodec-*.dll next to a
   * video opened from Downloads would otherwise be picked up. That's how
   * a packaged release (tools/build-windows.sh) loads: everything it
   * needs sits next to the plugins or the .exe.
   *
   * If that can't find a dependency (ERROR_MOD_NOT_FOUND) -- typically a
   * development build run from MSYS2, where FFmpeg's DLLs live in
   * C:\msys64\ucrt64\bin and are only reachable through PATH -- the
   * absolute folders on PATH are added to the search (AddDllDirectory)
   * and the load is retried with the same restricted flags. Not the
   * default search order: that can include the current directory (Wine
   * searches it even after SetDllDirectory("")), and relative PATH
   * entries like "." or "" are skipped for the same reason. */
  #ifndef LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR
  #define LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR 0x00000100
  #endif
  #ifndef LOAD_LIBRARY_SEARCH_DEFAULT_DIRS
  #define LOAD_LIBRARY_SEARCH_DEFAULT_DIRS 0x00001000
  #endif
  #include <wchar.h>
  #include <stdlib.h>
  typedef void *(WINAPI *add_dll_dir_fn)(const wchar_t *);

  static int is_absolute_w(const wchar_t *p) {
      return (((p[0] | 0x20) >= L'a' && (p[0] | 0x20) <= L'z') && p[1] == L':' && (p[2] == L'\\' || p[2] == L'/'))
          || (p[0] == L'\\' && p[1] == L'\\');                     /* \\server\share */
  }

  /* Once per process: add each absolute PATH folder to the DLL search list. */
  static int add_path_dirs(void) {
      static int done = 0, added = 0;
      if (done) return added;
      done = 1;
      add_dll_dir_fn add = (add_dll_dir_fn)(void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "AddDllDirectory");
      DWORD n = GetEnvironmentVariableW(L"PATH", NULL, 0);
      if (!add || n == 0) return 0;
      wchar_t *buf = (wchar_t *)malloc(n * sizeof(wchar_t));
      if (!buf) return 0;
      if (GetEnvironmentVariableW(L"PATH", buf, n) > 0) {
          for (wchar_t *tok = buf; tok; ) {
              wchar_t *semi = wcschr(tok, L';');
              if (semi) *semi = L'\0';
              if (is_absolute_w(tok) && add(tok)) added++;
              tok = semi ? semi + 1 : NULL;
          }
      }
      free(buf);
      return added;
  }

  static os_handle_t os_load(const char *path) {
      /* An absolute, backslashed path: LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR
       * requires one (plugin paths are built with '/' and may be relative,
       * e.g. a relative QQVIDEO_PLUGIN_DIR). */
      char win_path[1024];
      DWORD len = GetFullPathNameA(path, (DWORD)sizeof(win_path), win_path, NULL);
      if (len == 0 || len >= sizeof(win_path)) snprintf(win_path, sizeof(win_path), "%s", path);
      for (char *c = win_path; *c; c++) if (*c == '/') *c = '\\';

      const DWORD flags = LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS;
      HMODULE h = LoadLibraryExA(win_path, NULL, flags);
      if (!h && GetLastError() == ERROR_MOD_NOT_FOUND &&
          GetFileAttributesA(win_path) != INVALID_FILE_ATTRIBUTES && add_path_dirs() > 0) {
          /* the plugin exists, one of its DLLs isn't next to it: try PATH's folders */
          h = LoadLibraryExA(win_path, NULL, flags);
          if (h) fprintf(stderr, "lumen: %s: its DLLs were found through PATH, not next to it "
                                 "(fine for a dev build; a packaged release has them alongside)\n", path);
      }
      return h;
  }
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
