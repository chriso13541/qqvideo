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

void QQ_RetroTitleBar(ImDrawList *dl, const ImVec2 &a, const ImVec2 &b, bool active) {
    ImVec2 p = snap(a), q = snap(b);
    ImU32 l = active ? QQ_NAVY : IM_COL32(128, 128, 128, 255);
    ImU32 r = active ? IM_COL32(166, 202, 240, 255) : IM_COL32(192, 192, 192, 255);
    dl->AddRectFilledMultiColor(p, q, l, r, r, l);   /* left-to-right gradient */
}

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
        ImVec2 a = w->Pos, b = ImVec2(w->Pos.x + w->Size.x, w->Pos.y + w->Size.y);
        ImDrawList *dl = w->DrawList;
        dl->PushClipRect(a, b, false);

        if (f & ImGuiWindowFlags_Tooltip) {
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
