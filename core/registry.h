/*
 * registry.h
 *
 * The "package database" half of the system. Scans a plugins directory
 * for *.manifest.json sidecar files and builds an in-memory index of
 * what's INSTALLED and what each plugin CLAIMS to handle.
 *
 * Crucially: scanning the registry never loads a single line of plugin
 * code. A manifest is just text the core reads about a plugin; the
 * plugin's actual .dll/.so is only dlopen'd in plugin_loader.c, and only
 * once the registry has decided (based on manifest claims) that this
 * specific plugin is actually needed for the file being opened. This is
 * the property that makes "uninstalled = not loaded = not attack surface"
 * actually true.
 */
#ifndef LUMEN_REGISTRY_H
#define LUMEN_REGISTRY_H

#include "../include/lumen_plugin.h"

typedef struct {
    char name[64];
    char version[32];
    lumen_plugin_kind_t kind;
    char library_path[512];     /* absolute path to the .dll/.so */
    char manifest_path[512];

    /* claims -- only the relevant ones are populated depending on kind */
    char extensions[24][16];    /* demuxer: ".mp4", ".mkv", ... */
    int  extension_count;
    char fourccs[32][8];        /* decoder: "H264", "VP09", ... */
    int  fourcc_count;
} lumen_registry_entry_t;

typedef struct {
    lumen_registry_entry_t *entries;
    int count;
    int capacity;
} lumen_registry_t;

/* Scans `plugin_dir` for *.manifest.json files and populates `reg`.
 * Returns number of plugins indexed, or -1 on error reading the directory. */
int lumen_registry_scan(const char *plugin_dir, lumen_registry_t *reg);

/* Find an installed demuxer plugin that claims this file extension
 * (e.g. ".mkv", lowercase, including the dot). Returns NULL if none installed. */
const lumen_registry_entry_t *lumen_registry_find_demuxer_by_ext(const lumen_registry_t *reg, const char *ext);

/* Find an installed decoder plugin that claims this fourcc. */
const lumen_registry_entry_t *lumen_registry_find_decoder_by_fourcc(const lumen_registry_t *reg, const char *fourcc);

/* Find the first installed plugin of a given kind -- used for output
 * plugins, which aren't selected by claim but by user/config choice.
 * A real build would let the user pick (e.g. "sdl2-output" vs
 * "console-output"); this is the simplest viable policy for now. */
const lumen_registry_entry_t *lumen_registry_find_first(const lumen_registry_t *reg, lumen_plugin_kind_t kind);

void lumen_registry_print(const lumen_registry_t *reg);
void lumen_registry_free(lumen_registry_t *reg);

/* Load a single plugin entry from an explicit manifest + DLL path pair.
 * Used when building the registry from the packages/ tree (where each
 * package has its own subdirectory) rather than a flat plugins/ scan.
 * Returns 0 on success, -1 on parse error or if the registry is full. */
int lumen_registry_add_from_manifest(lumen_registry_t *reg,
                                      const char *manifest_path,
                                      const char *dll_path);

#endif
