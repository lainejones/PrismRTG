#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Laine Jones
"""
Prism's own icons: a glass prism splitting a white beam into a spectrum.

    mkicons.py prefs  out/pkg/Prism/PrismPrefs.info   # prism + a settings slider
    mkicons.py prism  out/pkg/Prism/Prism.info        # the prism alone (monitor file)
    mkicons.py prefs --png preview.png                # enlarged colour preview

Colour (OS 3.5+) icons with a 4-colour classic image as the fallback, built
with the icon writer in tools/icons (iconlib.py); AMIGA_TOOLS=<dir> uses
another copy.
"""
import os
import sys

sys.path.insert(0, os.environ.get('AMIGA_TOOLS') or
                os.path.join(os.path.dirname(os.path.abspath(__file__)), 'icons'))
import iconlib  # noqa: E402

W, H = 48, 40

PALETTE = [
    (149, 149, 149),  # 0  transparent
    (16, 20, 36),     # 1  outline
    (255, 255, 255),  # 2  beam / highlight
    (206, 226, 246),  # 3  glass, light
    (150, 186, 226),  # 4  glass, mid
    (96, 136, 196),   # 5  glass, dark
    (230, 48, 48),    # 6  red
    (246, 142, 30),   # 7  orange
    (250, 222, 40),   # 8  yellow
    (64, 190, 76),    # 9  green
    (48, 120, 232),   # 10 blue
    (140, 70, 200),   # 11 violet
    (226, 230, 236),  # 12 slider track, light
    (104, 110, 126),  # 13 slider shade
    (250, 250, 252),  # 14 slider knob
]
T = 0
# classic 4-colour fallback (0 grey, 1 black, 2 white, 3 blue)
#             0  1  2  3  4  5  6  7  8  9 10 11 12 13 14
PLANAR_MAP = [0, 1, 2, 2, 3, 3, 1, 3, 2, 3, 3, 1, 2, 1, 2]


def new():
    return [[T] * W for _ in range(H)]


def put(g, x, y, c):
    if 0 <= x < W and 0 <= y < H:
        g[y][x] = c


def line(g, x0, y0, x1, y1, c):
    dx, dy = abs(x1 - x0), abs(y1 - y0)
    sx, sy = (1 if x0 < x1 else -1), (1 if y0 < y1 else -1)
    err = dx - dy
    while True:
        put(g, x0, y0, c)
        if x0 == x1 and y0 == y1:
            return
        e2 = 2 * err
        if e2 > -dy:
            err -= dy
            x0 += sx
        if e2 < dx:
            err += dx
            y0 += sy


def draw_prism(g, top, base):
    """Glass triangle, apex (cx, top), base at y = base, plus beam and spectrum."""
    cx, half = 21, 13
    h = base - top

    def edge(y):                       # left and right x of the triangle at row y
        t = (y - top) / h
        return cx - round(half * t), cx + round(half * t)

    # spectrum first (the triangle's edge is drawn over its start): six
    # bands fanning out from the right face to the icon's edge
    bands = [6, 7, 8, 9, 10, 11]
    ox, oy = cx + 5, top + round(h * 0.52)
    for x in range(ox, W):
        t = (x - ox) / (W - 1 - ox)
        y0 = oy - 1 + round(t * 1)
        spread = 2 + t * 10
        for i, c in enumerate(bands):
            ya = y0 + round(i * spread / 6)
            yb = y0 + round((i + 1) * spread / 6)
            for y in range(ya, max(ya + 1, yb)):
                put(g, x, y, c)

    # the white beam coming in from the left, two pixels thick
    bx, by = cx - 7, top + round(h * 0.60)
    for x in range(0, bx + 1):
        y = by + round((bx - x) * 0.22)
        put(g, x, y, 2)
        put(g, x, y + 1, 2)

    # glass: lighter on the left face, darker to the right
    for y in range(top, base + 1):
        xl, xr = edge(y)
        for x in range(xl, xr + 1):
            t = (x - xl) / max(1, xr - xl)
            g[y][x] = 3 if t < 0.34 else 4 if t < 0.70 else 5
    # the beam bending through the glass
    line(g, bx, by, ox, oy, 2)
    line(g, bx, by + 1, ox, oy + 1, 3)
    # a highlight down the left face
    for y in range(top + 4, base - 3):
        xl, _ = edge(y)
        put(g, xl + 2, y, 2)
    # outline
    for y in range(top, base + 1):
        xl, xr = edge(y)
        put(g, xl, y, 1)
        put(g, xr, y, 1)
        if y > top:
            pl, pr = edge(y - 1)
            for x in range(xl, pl):
                put(g, x, y, 1)
            for x in range(pr + 1, xr + 1):
                put(g, x, y, 1)
    xl, xr = edge(base)
    for x in range(xl, xr + 1):
        put(g, x, base, 1)


def draw_slider(g, y):
    """A settings slider under the prism: track, shade and a knob."""
    for x in range(5, 43):
        put(g, x, y, 13)
        put(g, x, y + 1, 12)
        put(g, x, y + 2, 12)
        put(g, x, y + 3, 13)
    for x in (4, 43):
        for yy in range(y + 1, y + 3):
            put(g, x, yy, 13)
    kx = 28
    for yy in range(y - 2, y + 6):
        for x in range(kx, kx + 5):
            edge = yy in (y - 2, y + 5) or x in (kx, kx + 4)
            put(g, x, yy, 1 if edge else 14)
    for x in range(6, kx):                      # the part already "set": spectrum blue
        put(g, x, y + 1, 10)
        put(g, x, y + 2, 10)


def build(kind):
    g = new()
    if kind == 'prefs':
        draw_prism(g, 1, 27)
        draw_slider(g, 32)
    else:
        draw_prism(g, 4, 34)
    return g


def main():
    if len(sys.argv) < 3 or sys.argv[1] not in ('prefs', 'prism'):
        print(__doc__)
        return 1
    g = build(sys.argv[1])
    if sys.argv[2] == '--png':
        iconlib.write_png(sys.argv[3], g, PALETTE, W, H, T)
        return 0
    open(sys.argv[2], 'wb').write(iconlib.build_info(g, PALETTE, PLANAR_MAP, W, H, T))
    return 0


if __name__ == '__main__':
    sys.exit(main())
