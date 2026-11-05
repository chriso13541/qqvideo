# qqvideo changes to the vendored Dear ImGui (v1.91.5)

qqvideo draws its UI in the classic Windows 9x/2000 "3D bevel" style.
ImGui has no way to draw bevels through its style settings, so a few of
its rendering calls are routed to hooks in
`plugins/output_sdl2/retro_style.cpp`. Every change is wrapped in
`#ifdef IMGUI_QQ_RETRO` (defined in `imconfig.h`) and marked with a
`qqvideo:` comment; the original code is kept in the `#else` branch, so
removing the define gives stock ImGui back.

**When upgrading ImGui**, re-apply these (search the old files for
`qqvideo:`), then rebuild and look at a menu, a button, the file
browser and Tools > Preferences.

## imconfig.h
- `#define IMGUI_QQ_RETRO 1` and declarations of the hooks:
  `QQ_RetroRaised`, `QQ_RetroSunken`, `QQ_RetroTitleBar`, `QQ_RetroCloseButton`,
  `QQ_RetroAlwaysActive` (windows that always draw their title bar as active).

## imgui_widgets.cpp
| Function | Change |
|---|---|
| `ButtonEx` | Frame drawn with `QQ_RetroRaised`; pressed = sunken, label shifted 1px down-right. |
| `ArrowButtonEx` | Raised frame. |
| `CloseButton` | Drawn entirely by `QQ_RetroCloseButton` (Bluecurve-style rounded lavender button; active/inactive/pressed). Active also when `QQ_RetroAlwaysActive(root window name)`. |
| `Checkbox` | Box drawn with `QQ_RetroSunken`. |
| `DragScalar` | Sunken frame. |
| `SliderScalar` | Sunken track; grab drawn raised, full track height. |
| `InputTextEx` | Sunken frame (single-line). |
| `BeginCombo` | One sunken well with a raised arrow button inside its right end (pressed while open). |
| `ScrollbarEx` | Raised thumb. |
| `BeginMenuEx` | Label (and submenu arrow) drawn white while highlighted/open. |
| `MenuItemEx` | Label, shortcut and check mark drawn white while highlighted. |

## imgui.cpp
| Function | Change |
|---|---|
| `RenderWindowDecorations` | Title bar drawn with `QQ_RetroTitleBar` (Bluecurve Grape: striped purple gradient when active, light blue-gray when not); active also when `QQ_RetroAlwaysActive(window->Name)`. |
| `RenderWindowTitleBarContents` | Title text white with a drop shadow when active, gray-green when not (same `QQ_RetroAlwaysActive` rule). |

Not patched (done from qqvideo's own code instead): bevel frames around
popups, menus and bordered child panes, and the rounded Bluecurve frame
around titled windows (`qq_retro_decorate_windows`), colors and sizes (`qq_retro_apply_style`),
and the navy menu highlight (`qq_retro_push_menu_colors`).
