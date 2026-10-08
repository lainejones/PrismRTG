#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Laine Jones
"""
Generic WBDRAWER icon: a small 3D filing drawer. Emitted as a drawer icon (with
DrawerData) so it represents a directory that opens a window on double-click.

    makeicon_drawer.py MyDrawer.info [...]   # write drawer icon(s)
    makeicon_drawer.py --png preview.png     # colour PNG preview
"""
import sys
import iconlib

W, H = 48, 40

PALETTE = [
    (149, 149, 149),  # 0 transparent
    (206, 178, 130),  # 1 drawer face, light
    (176, 146, 96),   # 2 drawer face, mid
    (138, 110, 66),   # 3 drawer face, dark
    (0, 0, 0),        # 4 black outline
    (226, 202, 158),  # 5 top lid, light
    (120, 126, 140),  # 6 handle, dark
    (208, 212, 220),  # 7 handle, light
    (255, 255, 255),  # 8 white highlight
]
TRANSPARENT = 0
#              0  1  2  3  4  5  6  7  8
PLANAR_MAP = [ 0, 2, 0, 1, 1, 2, 1, 2, 2]  # ->grey/blk/wht/blu

def _new(): return [[TRANSPARENT] * W for _ in range(H)]
def _set(g, x, y, c):
    if 0 <= x < W and 0 <= y < H: g[y][x] = c
def _hline(g, x0, x1, y, c):
    for x in range(x0, x1 + 1): _set(g, x, y, c)
def _vline(g, x, y0, y1, c):
    for y in range(y0, y1 + 1): _set(g, x, y, c)
def _rect(g, x0, y0, x1, y1, c):
    for y in range(y0, y1 + 1): _hline(g, x0, x1, y, c)

# box front
FX0, FY0, FX1, FY1 = 7, 12, 40, 37
LID = 5   # depth of the 3D top lid

def _draw(g):
    # 3D top lid: parallelogram sloping up to the right
    for i in range(LID + 1):
        y = FY0 - i
        x0 = FX0 + i
        x1 = FX1 + i
        _hline(g, x0, x1, y, 5)
    # right depth side
    for i in range(LID + 1):
        _vline(g, FX1 + i, FY0 - i, FY1 - i, 3)
    # front face with a subtle gradient
    for y in range(FY0, FY1 + 1):
        for x in range(FX0, FX1 + 1):
            t = (x - FX0) + (y - FY0)
            g[y][x] = 1 if t < 14 else (3 if t > 42 else 2)
    # two drawer grooves
    _hline(g, FX0 + 2, FX1 - 2, FY0 + 8, 3)
    _hline(g, FX0 + 2, FX1 - 2, FY0 + 17, 3)
    # handle on the lower drawer
    _rect(g, 18, FY0 + 20, 29, FY0 + 22, 6)
    _hline(g, 18, 29, FY0 + 20, 7)
    # outlines
    _hline(g, FX0, FX1, FY0, 4)                       # front top edge
    _hline(g, FX0, FX1, FY1, 4)                       # front bottom
    _vline(g, FX0, FY0, FY1, 4)                       # front left
    _vline(g, FX1, FY0, FY1, 4)                       # front right (inner)
    # lid outline
    for i in range(LID + 1):
        _set(g, FX0 + i, FY0 - i, 4); _set(g, FX1 + i, FY0 - i, 4)
    _hline(g, FX0 + LID, FX1 + LID, FY0 - LID, 4)     # lid back edge
    _vline(g, FX1 + LID, FY0 - LID, FY1 - LID, 4)     # depth right edge
    for i in range(LID + 1):
        _set(g, FX1 + i, FY1 - i, 4)

def build_cidx():
    g = _new(); _draw(g); return g

def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__); return
    cidx = build_cidx()
    if args[0] == "--png":
        iconlib.write_png(args[1], cidx, PALETTE, W, H, TRANSPARENT); return
    data = iconlib.build_info(cidx, PALETTE, PLANAR_MAP, W, H, TRANSPARENT, drawer=True)
    for path in args:
        with open(path, "wb") as f:
            f.write(data)
        print("wrote", path, "(%d bytes, WBDRAWER)" % len(data))

if __name__ == "__main__":
    main()
