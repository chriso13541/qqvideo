/* retro_style.h -- the classic Windows 9x/2000 "3D bevel" look for ImGui.
 *
 * Gray button faces with a white highlight on the top/left edges and a
 * dark shadow on the bottom/right ("raised"), the reverse for text boxes
 * and anything pressed ("sunken"), navy menu highlights with white text
 * and gradient title bars.
 *
 * Two halves:
 *  - The QQ_Retro* hooks declared in third_party/imgui/imconfig.h, which
 *    the patched ImGui widgets call to draw their frames (see
 *    third_party/imgui/QQVIDEO_PATCHES.md).
 *  - The functions below, called by output_sdl2.cpp: the color scheme,
 *    and bevel frames around windows, popups and menus, which ImGui can
 *    draw for us without any patching.
 */
#pragma once
#include "imgui.h"

/* Classic palette (Windows 2000 "button face" family). */
#define QQ_FACE      IM_COL32(212, 208, 200, 255)
#define QQ_HILIGHT   IM_COL32(255, 255, 255, 255)
#define QQ_LIGHT     IM_COL32(223, 223, 223, 255)
#define QQ_SHADOW    IM_COL32(128, 128, 128, 255)
#define QQ_DKSHADOW  IM_COL32( 64,  64,  64, 255)
#define QQ_BLACK     IM_COL32(  0,   0,   0, 255)
#define QQ_NAVY      IM_COL32( 10,  36, 106, 255)

/* Corner rounding for titled windows (Bluecurve frame). Push it as
 * ImGuiStyleVar_WindowRounding around such a window: ImGui's rounded
 * background then sits just inside the pixel-drawn rounded outline. */
#define QQ_WM_ROUNDING 9.0f

/* Apply colors and sizes to the CURRENT ImGui context (call once per
 * context, after ImGui::StyleColorsLight()). */
void qq_retro_apply_style(void);

/* Draw bevel frames around every visible window of the current context.
 * Call once per frame, just before ImGui::Render(). */
void qq_retro_decorate_windows(void);

/* Push/pop the navy menu highlight. Wrap the main menu bar with these:
 * list-style Selectables elsewhere (file browser, queue) keep a light
 * highlight, since only menu items switch their text to white. */
void qq_retro_push_menu_colors(void);
void qq_retro_pop_menu_colors(void);

/* ---- Bluecurve window frame pieces (also used for qqvideo's own title
 * bar on the main window, see output_sdl2.cpp) ---- */
enum qq_wm_glyph { QQ_WM_CLOSE, QQ_WM_MAXIMIZE, QQ_WM_RESTORE, QQ_WM_MINIMIZE, QQ_WM_MENU };
/* An 18x16 title-bar button centered on `center`. */
void qq_wm_button(ImDrawList *dl, ImVec2 center, qq_wm_glyph glyph, bool active, bool hovered, bool pressed);
/* The black outline with Bluecurve's rounded corners, around a-b. */
void qq_wm_frame_outline(ImDrawList *dl, ImVec2 a, ImVec2 b);
/* x of the outline on corner row 0..4 (row 0 is the edge); 0 below that.
 * The same shape cuts the window's corners on Windows (SetWindowRgn). */
int  qq_wm_corner_inset(int row);
#define QQ_WM_TITLE_H 22.0f      /* Bluecurve's title bar height */

/* Two-pixel bevel outline (no fill): tl_* on the top/left edges, br_* on
 * the bottom/right; outer ring first, then inner. */
void qq_bevel(ImDrawList *dl, ImVec2 a, ImVec2 b, ImU32 tl_outer, ImU32 br_outer, ImU32 tl_inner, ImU32 br_inner);
