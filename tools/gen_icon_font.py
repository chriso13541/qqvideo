#!/usr/bin/env python3
"""Regenerate the embedded Remix Icon subset used by the qqvideo UI.

Pinned to Remix Icon 4.8.0: the last Apache-2.0 release. 4.9+ moved to
the custom "Remix Icon License", which isn't an open-source license.

qqvideo doesn't ship the whole Remix Icon font (~3000 glyphs, ~500 KB);
it embeds only the handful of icons the player actually draws, cut out
of the official remixicon.ttf with fontTools.

Usage (needs `pip install fonttools`, and the remixicon npm package or a
release zip unpacked somewhere):

    npm pack remixicon@4.8.0 && tar xzf remixicon-4.8.0.tgz
    python3 tools/gen_icon_font.py package/fonts/remixicon.ttf

Writes:
    third_party/remixicon/remixicon_font.cpp   the subset TTF as a C array
    third_party/remixicon/lumen_icons.h        ICON_* UTF-8 string macros

To add an icon: find its name on https://remixicon.com, look up its
codepoint in package/fonts/remixicon.css (".ri-<name>:before"), add it to
ICONS below, and re-run. Then use ICON_<NAME> in output_sdl2.cpp.
"""
import io
import os
import sys

from fontTools import subset
from fontTools.ttLib import TTFont

# (macro name, Remix Icon name, codepoint) -- names are for humans only.
ICONS = [
    ("PLAY",            "play-fill",            0xF00A),
    ("STOP",            "stop-fill",            0xF1A0),
    ("SKIP_BACK",       "skip-back-fill",       0xF13F),
    ("SKIP_FORWARD",    "skip-forward-fill",    0xF143),
    ("FULLSCREEN",      "fullscreen-line",      0xED9C),
    ("FULLSCREEN_EXIT", "fullscreen-exit-line", 0xED9A),
    ("PLAYLIST",        "play-list-fill",       0xF010),
    ("VOLUME_UP",       "volume-up-fill",       0xF2A1),
    ("VOLUME_DOWN",     "volume-down-fill",     0xF29B),
    ("VOLUME_MUTE",     "volume-mute-fill",     0xF29D),
    # built-in file browser
    ("FOLDER",          "folder-fill",          0xED61),
    ("FOLDER_OPEN",     "folder-open-fill",     0xED6F),
    ("FILE",            "file-fill",            0xECE0),
    ("FILE_VIDEO",      "file-video-fill",      0xF3C7),
    ("FILE_TEXT",       "file-text-fill",       0xED0E),
    ("ARROW_UP",        "arrow-up-line",        0xEA76),
    ("ARROW_LEFT",      "arrow-left-line",      0xEA60),
    ("ARROW_RIGHT",     "arrow-right-line",     0xEA6C),
    ("HOME",            "home-4-fill",          0xEE1C),
    ("DRIVE",           "hard-drive-2-fill",    0xEDF8),
    ("HISTORY",         "history-line",         0xEE17),
    ("REFRESH",         "refresh-line",         0xF064),
    ("COMPUTER",        "computer-line",        0xEBCA),
    ("DOWNLOAD",        "download-2-fill",      0xEC53),
    ("FILM",            "film-fill",            0xED20),
]

# Custom glyphs drawn here and added to the subset, for icons Remix
# doesn't have in the shape we want. Rectangles are (x0, y0, x1, y1) in
# Remix's 24x24 SVG grid (y down), so they line up with the real icons.
# Codepoints come from the start of the Private Use Area, below Remix's.
CUSTOM = [
    # Chunky pause: every Remix pause has 2-unit bars, which come out
    # about 1px wide at 16px -- reads as "| |". These are 4.5 units (3px).
    ("PAUSE", "qq-pause-thick", 0xE000, [(5.5, 5, 10, 19), (14, 5, 18.5, 19)]),
]

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR = os.path.join(ROOT, "third_party", "remixicon")


def add_custom_glyphs(font):
    """Draw the CUSTOM rectangles into the (TrueType) font as new glyphs."""
    from fontTools.pens.ttGlyphPen import TTGlyphPen
    # Remix maps its 24-unit SVG grid onto the em: x_font = s*x,
    # y_font = ascent - s*y (checked against pause-fill's outline).
    upm = font["head"].unitsPerEm
    s = upm / 24.0
    asc = font["hhea"].ascent
    order = font.getGlyphOrder()
    for _, name, cp, rects in CUSTOM:
        pen = TTGlyphPen(None)
        for x0, y0, x1, y1 in rects:          # clockwise = filled in TrueType
            X0, X1 = round(s * x0), round(s * x1)
            Y0, Y1 = round(asc - s * y1), round(asc - s * y0)
            pen.moveTo((X0, Y0)); pen.lineTo((X0, Y1)); pen.lineTo((X1, Y1)); pen.lineTo((X1, Y0))
            pen.closePath()
        order.append(name)
        font["glyf"][name] = pen.glyph()
        font["hmtx"][name] = (upm, 0)
        for table in font["cmap"].tables:
            if table.isUnicode():
                table.cmap[cp] = name
    font.setGlyphOrder(order)
    font["maxp"].numGlyphs = len(order)


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    src = sys.argv[1]

    font = TTFont(src)
    cmap = font.getBestCmap()
    missing = [n for _, n, cp in ICONS if cp not in cmap]
    if missing:
        sys.exit("not in this remixicon.ttf (codepoints moved?): " + ", ".join(missing))

    opts = subset.Options()
    opts.layout_features = []
    opts.name_IDs = ["*"]          # keep the copyright/license name records
    opts.notdef_outline = True
    opts.hinting = False
    sub = subset.Subsetter(opts)
    sub.populate(unicodes=[cp for _, _, cp in ICONS])
    sub.subset(font)
    add_custom_glyphs(font)
    buf = io.BytesIO()
    font.save(buf)
    data = buf.getvalue()

    os.makedirs(OUT_DIR, exist_ok=True)
    all_icons = ICONS + [(m, n, cp) for m, n, cp, _ in CUSTOM]
    lo = min(cp for _, _, cp in all_icons)
    hi = max(cp for _, _, cp in all_icons)

    with open(os.path.join(OUT_DIR, "remixicon_font.cpp"), "w") as f:
        f.write("/* Remix Icon 4.8.0 (https://github.com/Remix-Design/RemixIcon), Apache-2.0.\n"
                " * Subset of remixicon.ttf containing only the icons in lumen_icons.h.\n"
                " * GENERATED by tools/gen_icon_font.py -- do not edit by hand. */\n"
                "#include <stddef.h>\n")
        f.write("extern const unsigned int  remixicon_font_size = %d;\n" % len(data))
        f.write("extern const unsigned char remixicon_font_data[] = {\n")
        for i in range(0, len(data), 16):
            f.write("    " + ", ".join("0x%02x" % b for b in data[i:i + 16]) + ",\n")
        f.write("};\n")

    with open(os.path.join(OUT_DIR, "lumen_icons.h"), "w") as f:
        f.write("/* Remix Icon codepoints used by the qqvideo UI (Apache-2.0), plus a\n"
                " * few qqvideo-drawn glyphs (qq-*) from CUSTOM in the generator.\n"
                " * GENERATED by tools/gen_icon_font.py -- do not edit by hand.\n"
                " * Each ICON_* is a UTF-8 string literal, so it can be glued onto\n"
                " * an ImGui label: ImGui::Button(ICON_PLAY \"##play\"). */\n"
                "#pragma once\n\n")
        f.write("extern const unsigned int  remixicon_font_size;\n")
        f.write("extern const unsigned char remixicon_font_data[];\n\n")
        f.write("#define ICON_RANGE_MIN 0x%04X\n" % lo)
        f.write("#define ICON_RANGE_MAX 0x%04X\n\n" % hi)
        for macro, name, cp in all_icons:
            utf8 = "".join("\\x%02x" % b for b in chr(cp).encode("utf-8"))
            label = name if name.startswith("qq-") else "ri-" + name
            f.write('#define ICON_%-16s "%s"   /* %s U+%04X */\n' % (macro, utf8, label, cp))

    print("wrote %d-byte subset (%d icons) to %s" % (len(data), len(all_icons), OUT_DIR))


if __name__ == "__main__":
    main()
