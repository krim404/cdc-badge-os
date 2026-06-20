#!/usr/bin/env python3
"""Generate the built-in-font glyph charts as self-contained SVGs for the docs.

The badge draws text and list/menu item icons with the built-in Adafruit-GFX
glcdfont, which is the full IBM-PC code page 437. That font does NOT exist on a
developer's PC, so this tool reads the actual glyph bitmaps out of glcdfont.c and
the `UI_ICON_*` constants out of host_api.h and emits two SVGs into the website
assets:

  * ui-icons.svg  - the named UI_ICON_* pictographs (0x01-0x1F): glyph + name + byte.
  * cp437-map.svg - the complete 16x16 code page (every byte 0x00-0xFF: glyph + hex).

Every lit pixel becomes an SVG <rect>, so the output needs no font and no
third-party library - it matches exactly what the panel shows.

Usage:
    tools/gen_symbol_chart.py [--font <glcdfont.c>] [--header <host_api.h>] [--outdir <dir>]
"""

import argparse
import os
import re
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_FONT = os.path.join(REPO_ROOT, "components", "Adafruit-GFX", "glcdfont.c")
DEFAULT_HEADER = os.path.join(
    REPO_ROOT, "components", "plugin_manager", "include", "plugin_manager", "host_api.h"
)
DEFAULT_OUTDIR = os.path.join(REPO_ROOT, "website", "src", "assets")

GLYPH_W, GLYPH_H = 5, 8        # glcdfont cell (columns x rows)

# Bytes the text renderer intercepts as control codes (Adafruit_GFX::write
# treats them as newline / carriage-return). Their glyphs cannot be drawn via
# text or the icon field; they appear in the charts rendered straight from the
# bitmap, marked, for completeness only.
CONTROL_BYTES = {0x0A, 0x0D}


def parse_font(path):
    text = open(path, "r", encoding="utf-8", errors="replace").read()
    m = re.search(r"font\[\]\s*=\s*\{(.*?)\};", text, re.S)
    if not m:
        sys.exit(f"could not find font[] array in {path}")
    bytes_ = [int(b, 16) for b in re.findall(r"0x[0-9A-Fa-f]{2}", m.group(1))]
    if len(bytes_) < 256 * GLYPH_W:
        sys.exit(f"font[] has {len(bytes_)} bytes, expected >= {256 * GLYPH_W}")
    return [bytes_[c * GLYPH_W:(c + 1) * GLYPH_W] for c in range(256)]


def parse_icons(path):
    """Return {value: name} for the primary UI_ICON_* glyph defines (0x01-0x1F)."""
    text = open(path, "r", encoding="utf-8", errors="replace").read()
    out = {}
    for name, val in re.findall(
        r"#define\s+(UI_ICON_[A-Z_]+)\s+(0x[0-9A-Fa-f]+|\d+)\b", text
    ):
        v = int(val, 16) if val.lower().startswith("0x") else int(val)
        if 1 <= v <= 31 and v not in out:   # keep the primary (first) name per byte
            out[v] = name[len("UI_ICON_"):]
    return out


def glyph_rects(glyph, ox, oy, scale):
    rects = []
    for col in range(GLYPH_W):
        bits = glyph[col]
        for row in range(GLYPH_H):
            if (bits >> row) & 1:
                rects.append(
                    f'<rect x="{ox + col*scale}" y="{oy + row*scale}" '
                    f'width="{scale}" height="{scale}"/>'
                )
    return rects


def svg_open(width, height):
    return [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" '
        f'viewBox="0 0 {width} {height}" font-family="monospace">',
        f'<rect width="{width}" height="{height}" fill="#ffffff"/>',
    ]


def build_named(font, icons):
    """The 31 named UI_ICON_* glyphs: glyph + name + byte, in a grid."""
    scale = 6
    cols = 4
    cell_w, cell_h = 150, 104
    pad_top = 64
    gw, gh = GLYPH_W * scale, GLYPH_H * scale
    items = sorted(icons.items())
    rows = (len(items) + cols - 1) // cols
    width = cols * cell_w
    height = pad_top + rows * cell_h
    P = svg_open(width, height)
    P.append(
        f'<text x="{width//2}" y="26" text-anchor="middle" font-size="18" '
        f'font-weight="bold" fill="#111">CDC Badge UI_ICON_* icons</text>'
    )
    P.append(
        f'<text x="{width//2}" y="46" text-anchor="middle" font-size="11" fill="#555">'
        f'CP437 glyphs from the built-in 6x8 font - the byte you pass as a '
        f'list / context-menu item icon</text>'
    )
    for i, (val, name) in enumerate(items):
        cx = (i % cols) * cell_w
        cy = pad_top + (i // cols) * cell_h
        bg = "#fde2e2" if val in CONTROL_BYTES else "#fff"
        P.append(
            f'<rect x="{cx+4}" y="{cy+4}" width="{cell_w-8}" height="{cell_h-8}" '
            f'fill="{bg}" stroke="#ddd"/>'
        )
        ox = cx + (cell_w - gw) // 2
        oy = cy + 16
        P.append('<g fill="#000">')
        P.extend(glyph_rects(font[val], ox, oy, scale))
        P.append('</g>')
        ty = oy + gh + 22
        P.append(
            f'<text x="{cx+cell_w//2}" y="{ty}" text-anchor="middle" font-size="13" '
            f'font-weight="bold" fill="#111">{name}</text>'
        )
        sub = f'0x{val:02X} ({val})'
        if val in CONTROL_BYTES:
            sub += '  not drawable'
        P.append(
            f'<text x="{cx+cell_w//2}" y="{ty+16}" text-anchor="middle" font-size="11" '
            f'fill="#666">{sub}</text>'
        )
    P.append('</svg>')
    return "\n".join(P)


def build_codepage(font, icons):
    """The complete 16x16 CP437 code page: glyph + hex for every byte."""
    scale = 4
    cell_w, cell_h = 36, 46
    hdr = 22
    title_h = 74
    gw, gh = GLYPH_W * scale, GLYPH_H * scale
    grid_x, grid_y = hdr, title_h + hdr
    grid_w = hdr + 16 * cell_w
    grid_bottom = grid_y + 16 * cell_h

    leg_items = sorted(icons.items())
    leg_cols = 3
    leg_rows = (len(leg_items) + leg_cols - 1) // leg_cols
    leg_y = grid_bottom + 42
    leg_col_w = (grid_w - hdr) // leg_cols
    leg_row_h = 17

    width = grid_w + 6
    height = leg_y + leg_rows * leg_row_h + 24
    P = svg_open(width, height)
    P.append(
        f'<text x="{width//2}" y="26" text-anchor="middle" font-size="17" '
        f'font-weight="bold" fill="#111">CDC Badge built-in font - complete CP437 map</text>'
    )
    P.append(
        f'<text x="{width//2}" y="46" text-anchor="middle" font-size="10.5" fill="#555">'
        f'Every glyph the 6x8 glcdfont can draw. 0x20-0x7E = ASCII, 0x80-0xFF = CP437 '
        f'high half, 0x01-0x1F = pictographs (UI_ICON_*, tinted).</text>'
    )
    P.append(
        f'<text x="{width//2}" y="62" text-anchor="middle" font-size="10.5" fill="#555">'
        f'Pixel-accurate from glcdfont - NOT the GFX fonts. Pink = control byte the '
        f'text renderer eats (0x0A/0x0D); shown from the bitmap only.</text>'
    )
    for c in range(16):
        cx = grid_x + c * cell_w + cell_w // 2
        P.append(
            f'<text x="{cx}" y="{grid_y - 6}" text-anchor="middle" font-size="11" '
            f'font-weight="bold" fill="#333">{c:X}</text>'
        )
    for r in range(16):
        ry = grid_y + r * cell_h + cell_h // 2 + 4
        P.append(
            f'<text x="{hdr//2 + 2}" y="{ry}" text-anchor="middle" font-size="11" '
            f'font-weight="bold" fill="#333">{r:X}</text>'
        )
    for val in range(256):
        r, c = val // 16, val % 16
        cx = grid_x + c * cell_w
        cy = grid_y + r * cell_h
        fill = "#fde2e2" if val in CONTROL_BYTES else ("#fff6d6" if val in icons else "#ffffff")
        P.append(
            f'<rect x="{cx}" y="{cy}" width="{cell_w}" height="{cell_h}" '
            f'fill="{fill}" stroke="#e2e2e2"/>'
        )
        ox = cx + (cell_w - gw) // 2
        P.append('<g fill="#000">')
        P.extend(glyph_rects(font[val], ox, cy + 4, scale))
        P.append('</g>')
        P.append(
            f'<text x="{cx + cell_w//2}" y="{cy + cell_h - 5}" text-anchor="middle" '
            f'font-size="8.5" fill="#888">{val:02X}</text>'
        )
    P.append(
        f'<text x="{hdr}" y="{grid_bottom + 28}" font-size="13" font-weight="bold" '
        f'fill="#111">UI_ICON_* names (byte passed as a list / context-menu item icon)</text>'
    )
    for i, (val, name) in enumerate(leg_items):
        col, row = i // leg_rows, i % leg_rows
        lx = hdr + col * leg_col_w
        ly = leg_y + row * leg_row_h
        P.append('<g fill="#000">')
        P.extend(glyph_rects(font[val], lx, ly - 9, 2))
        P.append('</g>')
        suffix = "  (not drawable)" if val in CONTROL_BYTES else ""
        P.append(
            f'<text x="{lx + 16}" y="{ly}" font-size="11" fill="#222">'
            f'0x{val:02X} {name}{suffix}</text>'
        )
    P.append('</svg>')
    return "\n".join(P)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--font", default=DEFAULT_FONT)
    ap.add_argument("--header", default=DEFAULT_HEADER)
    ap.add_argument("--outdir", default=DEFAULT_OUTDIR)
    args = ap.parse_args()

    font = parse_font(args.font)
    icons = parse_icons(args.header)
    if not icons:
        sys.exit("no UI_ICON_* glyph defines found")
    os.makedirs(args.outdir, exist_ok=True)
    for name, svg in {
        "ui-icons.svg": build_named(font, icons),
        "cp437-map.svg": build_codepage(font, icons),
    }.items():
        path = os.path.join(args.outdir, name)
        open(path, "w", encoding="utf-8").write(svg + "\n")
        print(f"wrote {path}")


if __name__ == "__main__":
    main()
