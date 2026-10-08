#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Laine Jones
"""
Generic Install.info icon: a green "install" down-arrow dropping onto a base
slab (= put onto the system). Emitted as a WBPROJECT icon whose DefaultTool is
IconX, so double-clicking it runs the accompanying AmigaDOS script.

Reusable for any 'Execute'-style install script -- just name the output
Install.info and drop it next to a file called Install.

    makeicon_install.py Install.info [more.info ...]   # write icon(s)
    makeicon_install.py --png preview.png              # colour PNG preview
    makeicon_install.py --tool SYS:Tools/IconX out.info  # override DefaultTool
"""
import sys
import iconlib

W, H = 48, 40
DEFAULT_TOOL = "IconX"

#                 idx (r,  g,  b)
PALETTE = [
    (149, 149, 149),  # 0  transparent
    (150, 226, 150),  # 1  arrow green, light
    (86, 196, 96),    # 2  arrow green, mid
    (40, 150, 66),    # 3  arrow green, dark
    (0, 0, 0),        # 4  black outline
    (236, 238, 242),  # 5  slab light (top)
    (176, 182, 196),  # 6  slab mid
    (110, 116, 132),  # 7  slab dark (front/shade)
    (255, 255, 255),  # 8  white highlight
]
TRANSPARENT = 0
#              0  1  2  3  4  5  6  7  8
PLANAR_MAP = [ 0, 3, 3, 1, 1, 2, 0, 1, 2]  # ->grey/blk/wht/blu

def _new(): return [[TRANSPARENT] * W for _ in range(H)]
def _set(g, x, y, c):
    if 0 <= x < W and 0 <= y < H: g[y][x] = c
def _hline(g, x0, x1, y, c):
    for x in range(x0, x1 + 1): _set(g, x, y, c)
def _vline(g, x, y0, y1, c):
    for y in range(y0, y1 + 1): _set(g, x, y, c)

CX = 23
def _in_arrow(x, y):
    if 19 <= x <= 27 and 4 <= y <= 22:          # shaft
        return True
    if 22 <= y <= 32:                           # head
        half = 10 - (y - 22)
        if half >= 0 and CX - half <= x <= CX + half:
            return True
    return False

def _draw_arrow(g):
    for y in range(0, H):
        for x in range(0, W):
            if not _in_arrow(x, y):
                continue
            # light upper-left -> dark lower-right shading
            t = (x - 13) + (y - 4)
            g[y][x] = 1 if t < 12 else (3 if t > 30 else 2)
    # black outline: any arrow pixel bordering a non-arrow pixel
    for y in range(0, H):
        for x in range(0, W):
            if not _in_arrow(x, y):
                continue
            if (not _in_arrow(x-1, y) or not _in_arrow(x+1, y)
                    or not _in_arrow(x, y-1) or not _in_arrow(x, y+1)):
                g[y][x] = 4

def _draw_slab(g):
    sx0, sx1, sy0, sy1 = 4, 43, 33, 37
    for y in range(sy0, sy1 + 1):
        _hline(g, sx0, sx1, y, 6)
    _hline(g, sx0, sx1, sy0, 5)              # top highlight
    _hline(g, sx0, sx1, sy1, 7)              # bottom shade
    # black outline
    _hline(g, sx0 - 1, sx1 + 1, sy0 - 1, 4)
    _hline(g, sx0 - 1, sx1 + 1, sy1 + 1, 4)
    _vline(g, sx0 - 1, sy0 - 1, sy1 + 1, 4)
    _vline(g, sx1 + 1, sy0 - 1, sy1 + 1, 4)

def build_cidx():
    g = _new()
    _draw_slab(g)
    _draw_arrow(g)
    return g

def main():
    args = sys.argv[1:]
    tool = DEFAULT_TOOL
    if args and args[0] == "--tool":
        tool = args[1]; args = args[2:]
    if not args:
        print(__doc__); return
    cidx = build_cidx()
    if args[0] == "--png":
        iconlib.write_png(args[1], cidx, PALETTE, W, H, TRANSPARENT); return
    data = iconlib.build_info(cidx, PALETTE, PLANAR_MAP, W, H, TRANSPARENT,
                              icon_type=4, default_tool=tool)
    for path in args:
        with open(path, "wb") as f:
            f.write(data)
        print("wrote", path, "(%d bytes, DefaultTool=%s)" % (len(data), tool))

if __name__ == "__main__":
    main()
