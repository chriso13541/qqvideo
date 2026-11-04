/* retro_style.cpp -- see retro_style.h. */
#include "retro_style.h"
#include "imgui_internal.h"   /* window list, for decorating windows */
#include <math.h>
#include <string.h>

/* Everything is snapped to whole pixels so 1px lines stay 1px, crisp. */
static inline ImVec2 snap(ImVec2 v) { return ImVec2(floorf(v.x), floorf(v.y)); }

void qq_bevel(ImDrawList *dl, ImVec2 a, ImVec2 b, ImU32 tl_outer, ImU32 br_outer, ImU32 tl_inner, ImU32 br_inner) {
    a = snap(a); b = snap(b);
    if (b.x - a.x < 4 || b.y - a.y < 4) return;
    /* outer ring */
    dl->AddRectFilled(ImVec2(a.x, a.y),         ImVec2(b.x - 1, a.y + 1), tl_outer);   /* top    */
    dl->AddRectFilled(ImVec2(a.x, a.y),         ImVec2(a.x + 1, b.y - 1), tl_outer);   /* left   */
    dl->AddRectFilled(ImVec2(a.x, b.y - 1),     ImVec2(b.x, b.y),         br_outer);   /* bottom */
    dl->AddRectFilled(ImVec2(b.x - 1, a.y),     ImVec2(b.x, b.y),         br_outer);   /* right  */
    /* inner ring */
    dl->AddRectFilled(ImVec2(a.x + 1, a.y + 1), ImVec2(b.x - 2, a.y + 2), tl_inner);
    dl->AddRectFilled(ImVec2(a.x + 1, a.y + 1), ImVec2(a.x + 2, b.y - 2), tl_inner);
    dl->AddRectFilled(ImVec2(a.x + 1, b.y - 2), ImVec2(b.x - 1, b.y - 1), br_inner);
    dl->AddRectFilled(ImVec2(b.x - 2, a.y + 1), ImVec2(b.x - 1, b.y - 1), br_inner);
}

/* ---- hooks called from the patched ImGui (declared in imconfig.h) ---- */

void QQ_RetroRaised(ImDrawList *dl, const ImVec2 &a, const ImVec2 &b, unsigned int face, bool pressed) {
    ImVec2 p = snap(a), q = snap(b);
    dl->AddRectFilled(p, q, face);
    if (pressed)   /* a pushed-in button: sunken, like a text box */
        qq_bevel(dl, p, q, QQ_BLACK, QQ_HILIGHT, QQ_SHADOW, QQ_LIGHT);
    else
        qq_bevel(dl, p, q, QQ_HILIGHT, QQ_BLACK, QQ_LIGHT, QQ_SHADOW);
}

void QQ_RetroSunken(ImDrawList *dl, const ImVec2 &a, const ImVec2 &b, unsigned int fill) {
    ImVec2 p = snap(a), q = snap(b);
    dl->AddRectFilled(p, q, fill);
    qq_bevel(dl, p, q, QQ_SHADOW, QQ_HILIGHT, QQ_DKSHADOW, QQ_LIGHT);
}

/* ---- window decorations: Bluecurve "Grape" ----------------------------
 * Titled windows (the file browser) are drawn like the Bluecurve Grape
 * window-manager theme from Red Hat Linux 8/9: a purple title bar with a
 * fine diagonal stripe texture, white title text with a drop shadow, a
 * rounded lavender close button, and a black outline with rounded corners
 * around a light 4px border. Colors measured from the theme's xfwm4
 * pixmaps; everything is drawn here, pixel by pixel. */

#define HEX(c) IM_COL32(((c) >> 16) & 0xFF, ((c) >> 8) & 0xFF, (c) & 0xFF, 0xFF)

/* Active title bar, top to bottom, between the top outline and the bottom
 * line: a highlight, then a "cylinder" gradient darkest in the middle. */
static const unsigned int GRAPE_TITLE_ACTIVE[20] = {
    0x816ABB, 0x685B8B, 0x625587, 0x5D4F82, 0x57497E, 0x52437A, 0x504179,
    0x4E3F77, 0x4C3D76, 0x4B3B74, 0x493974, 0x4B3B74, 0x4E3E78, 0x52437B,
    0x56477E, 0x5B4D82, 0x615487, 0x685B8C, 0x6F6291, 0x746795,
};
static const unsigned int GRAPE_TITLE_INACTIVE[20] = {
    0xD5D9E7, 0xCFD4E2, 0xC9CFDE, 0xC3C9DA, 0xC3C9DA, 0xC3C9DA, 0xC3C9DA,
    0xC3C9DA, 0xC3C9DA, 0xC3C9DA, 0xC3C9DA, 0xC3C9DA, 0xC3C9DA, 0xC3C9DA,
    0xC3C9DA, 0xC3C9DA, 0xC1C7D8, 0xBEC5D6, 0xBCC2D4, 0xBCC2D4,
};
#define GRAPE_TITLE_BOTTOM_ACTIVE   0x080808
#define GRAPE_TITLE_BOTTOM_INACTIVE 0xADB4C9
#define GRAPE_FRAME_BAND            0xE6E6E6   /* the 4px border inside the outline */

/* Rounded corners: x of the outline pixel on the first five rows (row 0 is
 * the horizontal edge, which starts at x = 5). Mirrored for the other
 * corners. ImGui's own rounded window background (QQ_WM_ROUNDING) sits
 * just inside this curve, so nothing shows outside it. */
static const int CORNER[5] = { 5, 3, 2, 1, 1 };

static inline ImU32 rgb_scale(unsigned int c, float k) {
    return IM_COL32((int)(((c >> 16) & 0xFF) * k), (int)(((c >> 8) & 0xFF) * k), (int)((c & 0xFF) * k), 0xFF);
}
static inline ImU32 rgb_tint(unsigned int c, float t) {      /* mix toward white */
    auto m = [t](int v) { return (int)(v + (255 - v) * t); };
    return IM_COL32(m((c >> 16) & 0xFF), m((c >> 8) & 0xFF), m(c & 0xFF), 0xFF);
}
static inline void px(ImDrawList *dl, ImVec2 o, float x, float y, float w, float h, ImU32 c) {
    dl->AddRectFilled(ImVec2(o.x + x, o.y + y), ImVec2(o.x + x + w, o.y + y + h), c);
}

void QQ_RetroTitleBar(ImDrawList *dl, const ImVec2 &a, const ImVec2 &b, bool active) {
    ImVec2 p = snap(a), q = snap(b);
    const int W = (int)(q.x - p.x), H = (int)(q.y - p.y);
    const unsigned int *rows = active ? GRAPE_TITLE_ACTIVE : GRAPE_TITLE_INACTIVE;
    if (W < 16 || H < 6) { dl->AddRectFilled(p, q, HEX(rows[10])); return; }
    const int n = H - 2;                       /* gradient rows: 1 .. H-2 */
    for (int y = 1; y < H; y++) {
        const int inset = y < 5 ? CORNER[y] + 1 : 0;   /* inside the rounded corner */
        if (y == H - 1) {                       /* bottom line under the bar */
            px(dl, p, inset, y, W - 2 * inset, 1, HEX(active ? GRAPE_TITLE_BOTTOM_ACTIVE : GRAPE_TITLE_BOTTOM_INACTIVE));
            continue;
        }
        const int i = y - 1;
        const int idx = n > 1 ? (i * 19 + (n - 1) / 2) / (n - 1) : 0;   /* resample the 20-row table */
        const unsigned int base = rows[idx];
        px(dl, p, inset, y, W - 2 * inset, 1, HEX(base));
        /* The Bluecurve stripes: every 5px a darker pixel with a lighter one
         * beside it, shifting one pixel left per row, so they read as fine
         * embossed diagonals. Not on the highlight rows or the last row. */
        if (active && idx >= 2 && idx <= 18) {
            const ImU32 dark = rgb_scale(base, 0.645f), light = rgb_tint(base, 0.275f);
            int x0 = ((3 - y) % 5 + 5) % 5;              /* (x + y) % 5 == 3 */
            for (int x = x0; x < W - inset; x += 5) {
                if (x >= inset)         px(dl, p, x, y, 1, 1, dark);
                if (x + 1 < W - inset && x + 1 >= inset) px(dl, p, x + 1, y, 1, 1, light);
            }
        }
    }
}

/* Title-bar buttons, 18x16, centered on `c`. The lavender face, outline
 * and bevel are shared; the glyph shapes are Bluecurve's (measured from
 * the Grape xfwm4 pixmaps), drawn as pixel runs: {x, y, width}. */
struct run_t { int x, y, w; };
static const run_t GLYPH_CLOSE[] = {
    {6, 4, 1}, {10, 4, 1}, {5, 5, 3}, {9, 5, 3}, {6, 6, 5}, {7, 7, 3},
    {6, 8, 5}, {5, 9, 3}, {9, 9, 3}, {6, 10, 1}, {10, 10, 1}, {0, 0, 0} };
static const run_t GLYPH_MAXIMIZE[] = {            /* a window: thick top, thin sides */
    {5, 4, 7}, {5, 5, 7}, {5, 6, 7}, {5, 7, 1}, {11, 7, 1}, {5, 8, 1}, {11, 8, 1},
    {5, 9, 1}, {11, 9, 1}, {5, 10, 7}, {0, 0, 0} };
static const run_t GLYPH_RESTORE[] = {             /* two overlapping windows */
    {7, 3, 6}, {12, 4, 1}, {12, 5, 1}, {12, 6, 1}, {12, 7, 1},
    {5, 5, 6}, {5, 6, 6}, {5, 7, 6}, {5, 8, 1}, {10, 8, 1}, {5, 9, 1}, {10, 9, 1}, {5, 10, 6}, {0, 0, 0} };
static const run_t GLYPH_MINIMIZE[] = {            /* a bar at the bottom */
    {5, 8, 7}, {5, 9, 7}, {5, 10, 7}, {0, 0, 0} };
static const run_t GLYPH_MENU[] = {                /* the chevron */
    {5, 5, 1}, {11, 5, 1}, {4, 6, 3}, {10, 6, 3}, {5, 7, 3}, {9, 7, 3},
    {6, 8, 5}, {7, 9, 3}, {8, 10, 1}, {0, 0, 0} };

void qq_wm_button(ImDrawList *dl, ImVec2 center, qq_wm_glyph glyph, bool active, bool hovered, bool pressed) {
    ImVec2 c = snap(center);
    ImVec2 o(c.x - 9, c.y - 8);
    unsigned int outline, hl, fill_tl, fill_br, shadow, gcol;
    if (pressed)     { outline = 0x74769B; hl = fill_tl = fill_br = shadow = 0xB0B7C8; gcol = 0x605973; }
    else if (active) { outline = 0x74769B; hl = 0xF7F6FB; fill_tl = 0xDED9ED; fill_br = 0xEDE9F4; shadow = 0xBEC0D4; gcol = 0x605973; }
    else             { outline = 0xA5ADC3; hl = 0xE0E3ED; fill_tl = 0xDFE2EC; fill_br = 0xD8DCE8; shadow = 0xDBDFEA; gcol = 0x979F98; }
    ImU32 ftl = HEX(fill_tl), fbr = HEX(fill_br);
    if (hovered && !pressed) { ftl = rgb_tint(fill_tl, 0.45f); fbr = rgb_tint(fill_br, 0.45f); }
    const ImU32 fmid = IM_COL32(((((fill_tl >> 16) & 0xFF) + ((fill_br >> 16) & 0xFF)) / 2),
                                ((((fill_tl >> 8) & 0xFF) + ((fill_br >> 8) & 0xFF)) / 2),
                                (((fill_tl & 0xFF) + (fill_br & 0xFF)) / 2), 0xFF);
    /* face: a diagonal gradient, top-left to bottom-right */
    dl->AddRectFilledMultiColor(ImVec2(o.x + 2, o.y + 2), ImVec2(o.x + 16, o.y + 14),
                                ftl, hovered && !pressed ? ftl : fmid, fbr, hovered && !pressed ? fbr : fmid);
    const ImU32 O = HEX(outline), L = HEX(hl), S = HEX(shadow);
    px(dl, o, 2, 0, 14, 1, O);  px(dl, o, 2, 15, 14, 1, O);             /* top / bottom edge */
    px(dl, o, 0, 2, 1, 12, O);  px(dl, o, 17, 2, 1, 12, O);             /* left / right edge */
    px(dl, o, 1, 1, 1, 1, O);   px(dl, o, 16, 1, 1, 1, O);              /* rounded corners */
    px(dl, o, 1, 14, 1, 1, O);  px(dl, o, 16, 14, 1, 1, O);
    px(dl, o, 2, 1, 14, 1, L);  px(dl, o, 1, 2, 1, 12, L);              /* highlight */
    px(dl, o, 2, 14, 14, 1, S); px(dl, o, 16, 2, 1, 12, S);             /* shadow */
    const run_t *g = glyph == QQ_WM_CLOSE ? GLYPH_CLOSE : glyph == QQ_WM_MAXIMIZE ? GLYPH_MAXIMIZE
                   : glyph == QQ_WM_RESTORE ? GLYPH_RESTORE : glyph == QQ_WM_MINIMIZE ? GLYPH_MINIMIZE : GLYPH_MENU;
    const ImU32 G = HEX(gcol);
    for (; g->w; g++) px(dl, o, g->x, g->y, g->w, 1, G);
}

void QQ_RetroCloseButton(ImDrawList *dl, const ImVec2 &a, const ImVec2 &b, bool active, bool hovered, bool pressed) {
    /* centered on ImGui's (smaller) close-button box */
    qq_wm_button(dl, ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f), QQ_WM_CLOSE, active, hovered, pressed);
}

int qq_wm_corner_inset(int row) { return row >= 0 && row < 5 ? CORNER[row] : 0; }

/* Outline (+ optionally the 4px light band) around a titled window,
 * following the rounded corners. Drawn after the window's contents (the
 * band sits inside WindowPadding, so it never covers any). */
static void grape_frame(ImDrawList *dl, ImVec2 a, ImVec2 b, float title_h, bool band = true) {
    ImVec2 p = snap(a), q = snap(b);
    const int W = (int)(q.x - p.x), H = (int)(q.y - p.y), T = (int)title_h;
    if (W < 16 || H < T + 12) return;
    const ImU32 O = QQ_BLACK, B = HEX(GRAPE_FRAME_BAND);
    /* light band: left/right below the title bar, bottom along the curve */
    if (band) {
    px(dl, p, 1, T, 4, H - 5 - T, B);
    px(dl, p, W - 5, T, 4, H - 5 - T, B);
    for (int k = 1; k <= 4; k++) {               /* k = rows up from the bottom edge */
        const int in = CORNER[k] + 1;
        px(dl, p, in, H - 1 - k, W - 2 * in, 1, B);
    }
    }
    /* outline: straight edges, then the four corners */
    px(dl, p, CORNER[0], 0, W - 2 * CORNER[0], 1, O);
    px(dl, p, CORNER[0], H - 1, W - 2 * CORNER[0], 1, O);
    px(dl, p, 0, 5, 1, H - 10, O);
    px(dl, p, W - 1, 5, 1, H - 10, O);
    for (int k = 1; k <= 4; k++) {
        px(dl, p, CORNER[k], k, 1, 1, O);         px(dl, p, W - 1 - CORNER[k], k, 1, 1, O);
        px(dl, p, CORNER[k], H - 1 - k, 1, 1, O); px(dl, p, W - 1 - CORNER[k], H - 1 - k, 1, 1, O);
    }
    /* corner runs, so the curve is closed (e.g. row 1 spans x3..x4) */
    for (int k = 1; k <= 4; k++) {
        const int run = CORNER[k - 1] - CORNER[k];
        if (run > 1) {
            px(dl, p, CORNER[k] + 1, k, run - 1, 1, O);         px(dl, p, W - CORNER[k - 1], k, run - 1, 1, O);
            px(dl, p, CORNER[k] + 1, H - 1 - k, run - 1, 1, O); px(dl, p, W - CORNER[k - 1], H - 1 - k, run - 1, 1, O);
        }
    }
}

void qq_wm_frame_outline(ImDrawList *dl, ImVec2 a, ImVec2 b) { grape_frame(dl, a, b, 0.0f, false); }

/* ---- color scheme ---------------------------------------------------- */

void qq_retro_apply_style(void) {
    ImGuiStyle &st = ImGui::GetStyle();
    ImVec4 *c = st.Colors;
    auto rgb = [](int r, int g, int b, float a = 1.0f) { return ImVec4(r / 255.f, g / 255.f, b / 255.f, a); };
    const ImVec4 face = rgb(212, 208, 200);

    c[ImGuiCol_Text]                 = rgb(0, 0, 0);
    c[ImGuiCol_TextDisabled]         = rgb(128, 128, 128);
    c[ImGuiCol_WindowBg]             = face;
    c[ImGuiCol_ChildBg]              = rgb(0, 0, 0, 0.0f);
    c[ImGuiCol_PopupBg]              = face;
    c[ImGuiCol_Border]               = rgb(128, 128, 128);
    c[ImGuiCol_BorderShadow]         = rgb(0, 0, 0, 0.0f);
    c[ImGuiCol_FrameBg]              = rgb(255, 255, 255);         /* text boxes: white wells */
    c[ImGuiCol_FrameBgHovered]       = rgb(255, 255, 255);
    c[ImGuiCol_FrameBgActive]        = rgb(255, 255, 255);
    c[ImGuiCol_TitleBg]              = rgb(128, 128, 128);
    c[ImGuiCol_TitleBgActive]        = rgb(10, 36, 106);
    c[ImGuiCol_TitleBgCollapsed]     = rgb(128, 128, 128);
    c[ImGuiCol_MenuBarBg]            = face;
    c[ImGuiCol_ScrollbarBg]          = rgb(232, 230, 225);
    c[ImGuiCol_ScrollbarGrab]        = face;
    c[ImGuiCol_ScrollbarGrabHovered] = face;
    c[ImGuiCol_ScrollbarGrabActive]  = face;
    c[ImGuiCol_CheckMark]            = rgb(0, 0, 0);
    c[ImGuiCol_SliderGrab]           = face;
    c[ImGuiCol_SliderGrabActive]     = rgb(200, 196, 188);
    c[ImGuiCol_Button]               = face;
    c[ImGuiCol_ButtonHovered]        = rgb(222, 219, 212);         /* faint, so you see what's under the mouse */
    c[ImGuiCol_ButtonActive]         = face;
    /* List highlight (file browser, queue, sidebars): light, black text
     * stays readable. Menus switch to navy + white text instead (see
     * qq_retro_push_menu_colors). */
    c[ImGuiCol_Header]               = rgb(184, 200, 232);
    c[ImGuiCol_HeaderHovered]        = rgb(206, 218, 240);
    c[ImGuiCol_HeaderActive]         = rgb(170, 190, 228);
    c[ImGuiCol_Separator]            = rgb(128, 128, 128);
    c[ImGuiCol_SeparatorHovered]     = rgb(10, 36, 106);
    c[ImGuiCol_SeparatorActive]      = rgb(10, 36, 106);
    c[ImGuiCol_ResizeGrip]           = rgb(0, 0, 0, 0.0f);
    c[ImGuiCol_ResizeGripHovered]    = rgb(128, 128, 128, 0.5f);
    c[ImGuiCol_ResizeGripActive]     = rgb(10, 36, 106, 0.7f);
    c[ImGuiCol_Tab]                  = rgb(200, 196, 188);
    c[ImGuiCol_TabHovered]           = rgb(222, 219, 212);
    c[ImGuiCol_TabSelected]          = face;
    c[ImGuiCol_TabSelectedOverline]  = rgb(10, 36, 106);
    c[ImGuiCol_TabDimmed]            = rgb(200, 196, 188);
    c[ImGuiCol_TabDimmedSelected]    = face;
    c[ImGuiCol_TableHeaderBg]        = face;
    c[ImGuiCol_TableBorderStrong]    = rgb(128, 128, 128);
    c[ImGuiCol_TableBorderLight]     = rgb(192, 192, 192);
    c[ImGuiCol_TableRowBg]           = rgb(0, 0, 0, 0.0f);
    c[ImGuiCol_TableRowBgAlt]        = rgb(0, 0, 0, 0.035f);
    c[ImGuiCol_TextSelectedBg]       = rgb(49, 106, 197, 0.40f);
    c[ImGuiCol_NavCursor]            = rgb(0, 0, 0);
    c[ImGuiCol_ModalWindowDimBg]     = rgb(0, 0, 0, 0.25f);

    /* Square everything; the bevels replace ImGui's own 1px borders. */
    st.WindowRounding = st.ChildRounding = st.FrameRounding = st.PopupRounding = 0.0f;
    st.ScrollbarRounding = st.GrabRounding = st.TabRounding = 0.0f;
    st.WindowBorderSize = 0.0f;
    st.ChildBorderSize  = 1.0f;   /* nonzero keeps bordered panes padded; our sunken bevel covers the line */
    st.PopupBorderSize  = 0.0f;
    st.FrameBorderSize  = 0.0f;
    st.TabBorderSize    = 1.0f;
    st.ScrollbarSize    = 16.0f;
    st.GrabMinSize      = 12.0f;
    /* FramePadding/WindowPadding stay at ImGui's defaults: the control
     * bar's fixed 60px height is laid out for them. */
    st.WindowTitleAlign = ImVec2(0.0f, 0.5f);
}

void qq_retro_push_menu_colors(void) {
    ImVec4 navy(10 / 255.f, 36 / 255.f, 106 / 255.f, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Header, navy);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, navy);
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, navy);
}
void qq_retro_pop_menu_colors(void) { ImGui::PopStyleColor(3); }

/* ---- window frames ----------------------------------------------------- */

void qq_retro_decorate_windows(void) {
    ImGuiContext &g = *GImGui;
    for (ImGuiWindow *w : g.Windows) {
        if (!w->Active || w->Hidden) continue;           /* only windows drawn this frame */
        const ImGuiWindowFlags f = w->Flags;
        const bool main_menu_bar = strcmp(w->Name, "##MainMenuBar") == 0;
        if (strcmp(w->Name, "##qq_titlebar") == 0) continue;   /* draws its own Bluecurve chrome */
        ImVec2 a = w->Pos, b = ImVec2(w->Pos.x + w->Size.x, w->Pos.y + w->Size.y);
        ImDrawList *dl = w->DrawList;
        dl->PushClipRect(a, b, false);

        if (!(f & (ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_ChildWindow | ImGuiWindowFlags_Tooltip))) {
            /* titled windows (file browser): Bluecurve Grape frame */
            grape_frame(dl, a, b, w->TitleBarHeight);
        } else if (f & ImGuiWindowFlags_Tooltip) {
            /* tooltips: a plain 1px black frame, as they were */
            ImVec2 p = snap(a), q = snap(b);
            dl->AddRect(p, q, QQ_BLACK);
        } else if (f & (ImGuiWindowFlags_Popup | ImGuiWindowFlags_ChildMenu | ImGuiWindowFlags_Modal)) {
            /* menus, dropdowns, dialogs: raised */
            qq_bevel(dl, a, b, QQ_LIGHT, QQ_BLACK, QQ_HILIGHT, QQ_SHADOW);
        } else if (f & ImGuiWindowFlags_ChildWindow) {
            /* bordered panes inside a window (file browser lists): sunken */
            if (w->ChildFlags & ImGuiChildFlags_Borders)
                qq_bevel(dl, a, b, QQ_SHADOW, QQ_HILIGHT, QQ_DKSHADOW, QQ_LIGHT);
        } else if (!main_menu_bar) {
            /* top-level panels (control bar, queue): raised -- except a
             * window that fills its whole OS window (Preferences, Package
             * Manager), where the OS frame already is the edge. */
            bool fills_viewport = w->Size.x >= g.IO.DisplaySize.x - 1 && w->Size.y >= g.IO.DisplaySize.y - 1;
            if (!fills_viewport)
                qq_bevel(dl, a, b, QQ_LIGHT, QQ_BLACK, QQ_HILIGHT, QQ_SHADOW);
        }
        /* main menu bar: a thin etched line under it */
        if (main_menu_bar) {
            ImVec2 p = snap(ImVec2(a.x, b.y - 2)), q = snap(b);
            dl->AddRectFilled(p, ImVec2(q.x, p.y + 1), QQ_SHADOW);
            dl->AddRectFilled(ImVec2(p.x, p.y + 1), q, QQ_HILIGHT);
        }
        dl->PopClipRect();
    }
}
