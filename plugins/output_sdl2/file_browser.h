/* file_browser.h -- qqvideo's built-in file picker.
 *
 * A file browser drawn with ImGui inside the player window, so opening a
 * file needs nothing outside qqvideo itself: no zenity, kdialog, GTK or Qt
 * on Linux, and the same look on Windows. Uses only C++17 <filesystem>
 * (plus GetLogicalDrives on Windows, to list drive letters).
 *
 * Paths in and out are UTF-8 on every platform, same as SDL and FFmpeg.
 *
 * Usage, all on the UI thread:
 *     lumen_fb_open(fb, "Open File", LUMEN_FB_MEDIA, NULL);   // once
 *     ...every frame, between ImGui::NewFrame() and ImGui::Render():
 *     char path[1024];
 *     if (lumen_fb_draw(fb, path, sizeof(path)) == LUMEN_FB_PICKED) open(path);
 */
#pragma once
#include <stddef.h>

enum lumen_fb_kind {
    LUMEN_FB_MEDIA     = 0,   /* video/audio files the demuxer can open */
    LUMEN_FB_SUBTITLES = 1,   /* .srt .ass .ssa .vtt */
};

enum lumen_fb_result {
    LUMEN_FB_NONE      = 0,   /* still open (or not open at all) */
    LUMEN_FB_PICKED    = 1,   /* out holds the chosen file; the browser closed */
    LUMEN_FB_CANCELLED = 2,   /* closed without a choice */
};

struct lumen_file_browser;

/* config_dir: where to keep the last folder and recent folders between
 * runs (file_browser.ini). NULL or "" = don't remember anything. */
lumen_file_browser *lumen_fb_create(const char *config_dir);
void lumen_fb_destroy(lumen_file_browser *fb);

/* start_dir: folder to open in (e.g. the playing video's folder when
 * adding subtitles). NULL or "" = the last folder used, else home. */
void lumen_fb_open(lumen_file_browser *fb, const char *title, lumen_fb_kind kind, const char *start_dir);
bool lumen_fb_is_open(const lumen_file_browser *fb);
lumen_fb_result lumen_fb_draw(lumen_file_browser *fb, char *out, size_t out_size);

/* True for extensions the browser lists as media / subtitles. Also used
 * to sort out drag-and-dropped files. ext may include the dot. */
bool lumen_fb_is_media_ext(const char *path);
bool lumen_fb_is_subtitle_ext(const char *path);
