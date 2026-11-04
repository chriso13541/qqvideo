#!/usr/bin/env python3
"""Draw the qqvideo app icon and write every file that uses it.

The icon is drawn here in code (no source image): the "qq" wordmark,
white fading to Bluecurve lavender, on a beveled tile in the Bluecurve
"Grape" purples -- lit from the top left like early-2000s desktop icons,
with a dark outline, a highlight just inside it, a soft sheen and a drop
shadow. The letters are Roboto Bold, the font the player's UI uses
(third_party/fonts/roboto.ttf).

Every size is drawn on its own, at 8x and scaled down. The large ones get
outlined, shadowed letters; at 16-32 px those details would smear the two
q's together, so the small icons use bigger, plain white letters instead.

Usage:  python3 tools/gen_app_icon.py [--preview FILE] [--no-write]

Writes:
    resources/qqvideo.ico                  Windows icon, 16-256 px (BMP entries, PNG at 256)
    resources/icons/qqvideo-<N>.png        Linux hicolor icons, 16-256 px
    resources/qqvideo_icon.h               64 px PNG for SDL_SetWindowIcon
Needs Pillow (pip install pillow).
"""
import argparse
import io
import os
import struct

from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RES = os.path.join(ROOT, "resources")
SIZES = [16, 24, 32, 48, 64, 128, 256]
SS = 8                                  # supersampling factor

# Bluecurve Grape (from the theme's title bar and menu colors)
TILE_LIGHT = (0x9C, 0x88, 0xD2)         # top-left of the tile
TILE_MID = (0x6B, 0x5B, 0x95)
TILE_DARK = (0x3E, 0x31, 0x66)          # bottom-right
OUTLINE = (0x24, 0x1C, 0x3A)
HILITE = (0xD9, 0xD0, 0xF2)             # bevel highlight inside the outline
GLYPH_TOP = (0xFF, 0xFF, 0xFF)          # letters: white ...
GLYPH_BOTTOM = (0xDE, 0xD9, 0xED)       # ... to Bluecurve lavender


def lerp(a, b, t):
    return tuple(int(round(x + (y - x) * t)) for x, y in zip(a, b))


def diag_gradient(w, h, c0, c1, c2):
    """Top-left c0 -> middle c1 -> bottom-right c2. Computed at <= 256 px and
    scaled up: it's smooth, so that's indistinguishable and much faster."""
    gw, gh = min(w, 256), min(h, 256)
    g = Image.new("RGB", (gw, gh))
    px = g.load()
    for y in range(gh):
        for x in range(gw):
            t = (x / max(gw - 1, 1) * 0.35 + y / max(gh - 1, 1) * 0.65)   # mostly top-to-bottom
            px[x, y] = lerp(c0, c1, t * 2) if t < 0.5 else lerp(c1, c2, (t - 0.5) * 2)
    return g.resize((w, h), Image.BILINEAR) if (gw, gh) != (w, h) else g


def vgrad(w, h, c0, c1):
    g = Image.new("RGB", (w, h))
    d = ImageDraw.Draw(g)
    for y in range(h):
        d.line([(0, y), (w, y)], fill=lerp(c0, c1, y / max(h - 1, 1)))
    return g


def rounded_mask(w, h, box, r):
    m = Image.new("L", (w, h), 0)
    ImageDraw.Draw(m).rounded_rectangle(box, radius=r, fill=255)
    return m


def params(size):
    """Per-size tuning. text = letter height as a share of the tile;
    weight = Roboto weight (700 Bold, 900 Black: heavier strokes survive
    being scaled down to a few pixels); detail = the letters' dark outline,
    shadow and lavender fade (large sizes only -- small ones are plain white)."""
    if size <= 16:
        return dict(margin=0.5, radius=3, outline=1, shadow=0, text=0.86, weight=900, detail=False)
    if size <= 24:
        return dict(margin=0.75, radius=4, outline=1, shadow=1, text=0.80, weight=900, detail=False)
    if size <= 32:
        return dict(margin=1, radius=5, outline=1, shadow=1, text=0.74, weight=800, detail=False)
    if size <= 48:
        return dict(margin=1.5, radius=7, outline=1.25, shadow=2, text=0.66, weight=700, detail=True)
    s = size / 64.0
    return dict(margin=2 * s, radius=10 * s, outline=1.5 * s, shadow=2.5 * s, text=0.62, weight=700, detail=True)


def draw_icon(size):
    p = params(size)
    W = size * SS
    img = Image.new("RGBA", (W, W), (0, 0, 0, 0))

    m = p["margin"] * SS
    sh = p["shadow"] * SS
    tile = (m, m, W - m - sh - 1, W - m - sh - 1)          # leave room for the drop shadow
    r = p["radius"] * SS
    ow = max(SS, int(round(p["outline"] * SS)))

    # drop shadow (soft, down-right)
    if sh > 0:
        shadow = Image.new("L", (W, W), 0)
        ImageDraw.Draw(shadow).rounded_rectangle((tile[0] + sh, tile[1] + sh, tile[2] + sh, tile[3] + sh),
                                                 radius=r, fill=110)
        shadow = shadow.filter(ImageFilter.GaussianBlur(sh * 0.6))
        img.paste((0, 0, 0, 255), (0, 0), shadow)

    # outline, then the gradient body inset by the outline width
    img.paste(OUTLINE + (255,), (0, 0), rounded_mask(W, W, tile, r))
    inner = (tile[0] + ow, tile[1] + ow, tile[2] - ow, tile[3] - ow)
    body = diag_gradient(W, W, TILE_LIGHT, TILE_MID, TILE_DARK)
    img.paste(body, (0, 0), rounded_mask(W, W, inner, max(r - ow, 1)))

    # bevel highlight: a light ring just inside the outline, top and left
    # edges only (fades out toward the bottom right)
    ring = rounded_mask(W, W, inner, max(r - ow, 1))
    inner2 = (inner[0] + ow, inner[1] + ow, inner[2] - ow, inner[3] - ow)
    ImageDraw.Draw(ring).rounded_rectangle(inner2, radius=max(r - 2 * ow, 1), fill=0)
    fade = Image.linear_gradient("L").rotate(45, expand=False).resize((W, W))   # light at top-left
    fade = Image.eval(fade, lambda v: 255 - v)
    ring = Image.composite(ring, Image.new("L", (W, W), 0), fade)
    img.paste(HILITE + (255,), (0, 0), Image.eval(ring, lambda v: int(v * 0.85)))

    # soft sheen across the top third (the era's "lit plastic" look)
    sheen = Image.new("L", (W, W), 0)
    ImageDraw.Draw(sheen).ellipse((inner[0] - W * 0.3, inner[1] - W * 0.55, inner[2] + W * 0.3, inner[1] + W * 0.38),
                                  fill=60)
    sheen = Image.composite(sheen, Image.new("L", (W, W), 0), rounded_mask(W, W, inner2, max(r - 2 * ow, 1)))
    img.paste((255, 255, 255, 255), (0, 0), sheen.filter(ImageFilter.GaussianBlur(SS)))

    cx0, cy0, cx1, cy1 = inner2
    d = ImageDraw.Draw(img)

    # the "qq" wordmark, centered on its ink (not on the font's line box)
    def roboto(px):
        f = ImageFont.truetype(os.path.join(ROOT, "third_party", "fonts", "roboto.ttf"), int(px))
        try:
            f.set_variation_by_axes([p["weight"], 100])   # Roboto is variable: weight, width
        except Exception:
            pass
        return f
    font = roboto((cy1 - cy0) * p["text"])
    mask = Image.new("L", (W, W), 0)
    md = ImageDraw.Draw(mask)
    bb = md.textbbox((0, 0), "qq", font=font)
    ink_w, ink_h = bb[2] - bb[0], bb[3] - bb[1]
    if ink_w > (cx1 - cx0) * 0.86:                  # keep clear of the tile's edges
        font = roboto(font.size * (cx1 - cx0) * 0.86 / ink_w)
        bb = md.textbbox((0, 0), "qq", font=font)
    tx = (cx0 + cx1) / 2 - (bb[0] + bb[2]) / 2
    ty = (cy0 + cy1) / 2 - (bb[1] + bb[3]) / 2
    md.text((tx, ty), "qq", font=font, fill=255)
    glyph_mask_layer(img, mask, size, ow, p["detail"])

    return img.resize((size, size), Image.LANCZOS)


def glyph_mask_layer(img, mask, size, ow, detail=True):
    """Paint a white-to-lavender glyph from `mask`, with a dark outline and
    shadow if `detail`."""
    W = img.size[0]
    if not detail:
        img.paste(GLYPH_TOP + (255,), (0, 0), mask)      # plain white: most contrast
        return
    off = max(SS, int(round(size / 32 * SS)))
    shadow = mask.filter(ImageFilter.MaxFilter(3)).filter(ImageFilter.GaussianBlur(off * 0.5))
    sh = Image.new("L", (W, W), 0)
    sh.paste(shadow, (off, off))
    img.paste((0, 0, 0, 255), (0, 0), Image.eval(sh, lambda v: int(v * 0.45)))
    k = max(3, (ow // 2) * 2 + 1)
    outline = mask.filter(ImageFilter.MaxFilter(k if k % 2 else k + 1))
    img.paste(OUTLINE + (255,), (0, 0), outline)
    img.paste(vgrad(W, W, GLYPH_TOP, GLYPH_BOTTOM), (0, 0), mask)


def png_bytes(im):
    b = io.BytesIO()
    im.save(b, "PNG", optimize=True)
    return b.getvalue()


def dib_bytes(im):
    """A classic 32-bit BMP icon entry: BITMAPINFOHEADER (height doubled
    for the mask), bottom-up BGRA pixels, then the 1-bit AND mask
    (1 = transparent), each mask row padded to 32 bits."""
    w, h = im.size
    px = im.convert("RGBA").load()
    hdr = struct.pack("<IiiHHIIiiII", 40, w, h * 2, 1, 32, 0, 0, 0, 0, 0, 0)
    color = bytearray()
    for y in range(h - 1, -1, -1):
        for x in range(w):
            r, g, b, a = px[x, y]
            color += bytes((b, g, r, a))
    stride = ((w + 31) // 32) * 4
    mask = bytearray()
    for y in range(h - 1, -1, -1):
        row = bytearray(stride)
        for x in range(w):
            if px[x, y][3] == 0:
                row[x // 8] |= 0x80 >> (x % 8)
        mask += row
    return hdr + bytes(color) + bytes(mask)


def write_ico(path, images):
    """ICO in the layout Microsoft recommends: classic BMP entries up to
    128 px and PNG only for 256 px. Every Windows icon reader (Explorer's
    thumbnail cache, shortcuts, the taskbar, older resource tools) handles
    BMP entries, while PNG ones at small sizes trip some of them up."""
    data = [png_bytes(im) if im.size[0] >= 256 else dib_bytes(im) for im in images]
    out = io.BytesIO()
    out.write(struct.pack("<HHH", 0, 1, len(images)))
    offset = 6 + 16 * len(images)
    for im, d in zip(images, data):
        w = im.size[0]
        out.write(struct.pack("<BBBBHHII", w if w < 256 else 0, w if w < 256 else 0, 0, 0, 1, 32, len(d), offset))
        offset += len(d)
    for d in data:
        out.write(d)
    with open(path, "wb") as f:
        f.write(out.getvalue())


def write_header(path, png):
    with open(path, "w") as f:
        f.write("/* qqvideo app icon -- 64x64 RGBA PNG, embedded for SDL_SetWindowIcon.\n"
                " * GENERATED by tools/gen_app_icon.py -- do not edit by hand. */\n"
                "#pragma once\n#include <stddef.h>\n")
        f.write("static const unsigned int  qqvideo_icon_png_size = %d;\n" % len(png))
        f.write("static const unsigned char qqvideo_icon_png[] = {\n")
        for i in range(0, len(png), 16):
            f.write("    " + ", ".join("0x%02x" % b for b in png[i:i + 16]) + ",\n")
        f.write("};\n")


def preview(images, path, bg=(212, 208, 200)):
    """All sizes at 1x, plus the small ones blown up 4x, on the UI's gray."""
    pad = 12
    w = sum(im.size[0] for im in images) + pad * (len(images) + 1)
    small = [im for im in images if im.size[0] <= 48]
    zw = sum(im.size[0] * 4 for im in small) + pad * (len(small) + 1)
    h = 256 + pad * 2 + 48 * 4 + pad * 2
    sheet = Image.new("RGBA", (max(w, zw), h), bg + (255,))
    x = pad
    for im in images:
        sheet.alpha_composite(im, (x, pad + 256 - im.size[1]))
        x += im.size[0] + pad
    x = pad
    for im in small:
        z = im.resize((im.size[0] * 4, im.size[1] * 4), Image.NEAREST)
        sheet.alpha_composite(z, (x, 256 + pad * 3 + 48 * 4 - z.size[1]))
        x += z.size[0] + pad
    sheet.save(path)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--preview", help="also write a preview sheet PNG here")
    ap.add_argument("--no-write", action="store_true", help="only make the preview")
    a = ap.parse_args()

    images = [draw_icon(s) for s in SIZES]
    if a.preview:
        preview(images, a.preview)
    if a.no_write:
        return
    os.makedirs(os.path.join(RES, "icons"), exist_ok=True)
    for im in images:
        im.save(os.path.join(RES, "icons", "qqvideo-%d.png" % im.size[0]), optimize=True)
    by_size = {im.size[0]: im for im in images}
    write_ico(os.path.join(RES, "qqvideo.ico"), images)
    write_header(os.path.join(RES, "qqvideo_icon.h"), png_bytes(by_size[64]))
    print("wrote %d sizes to resources/" % len(images))


if __name__ == "__main__":
    main()
