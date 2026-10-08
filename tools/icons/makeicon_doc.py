#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Laine Jones
"""
Generic document icon: a sheet of paper with a folded corner and text lines.
Emitted as a WBPROJECT icon whose DefaultTool opens it -- default
SYS:Utilities/MultiView, which renders text and AmigaGuide via datatypes.

    makeicon_doc.py MyDoc.guide.info [...]              # write icon(s)
    makeicon_doc.py --png preview.png                   # colour PNG preview
    makeicon_doc.py --tool C:More A314Mount.doc.info    # override DefaultTool
"""
import sys
import iconlib

W, H = 48, 40
DEFAULT_TOOL = "SYS:Utilities/MultiView"

PALETTE = [
    (149, 149, 149),  # 0 transparent
    (255, 255, 255),  # 1 page white
    (222, 225, 233),  # 2 page edge shade
    (150, 154, 166),  # 3 folded-corner back
    (104, 146, 226),  # 4 text line, light
    (54, 92, 178),    # 5 text line, dark
    (0, 0, 0),        # 6 black outline
    (250, 250, 246),  # 7 highlight
]
TRANSPARENT = 0
#              0  1  2  3  4  5  6  7
PLANAR_MAP = [ 0, 2, 2, 0, 3, 3, 1, 2]  # ->grey/blk/wht/blu

def _new(): return [[TRANSPARENT] * W for _ in range(H)]
def _set(g, x, y, c):
    if 0 <= x < W and 0 <= y < H: g[y][x] = c
def _hline(g, x0, x1, y, c):
    for x in range(x0, x1 + 1): _set(g, x, y, c)
def _vline(g, x, y0, y1, c):
    for y in range(y0, y1 + 1): _set(g, x, y, c)

PX0, PY0, PX1, PY1 = 10, 3, 37, 37
F = 10                                   # folded-corner size

def _draw(g):
    for y in range(PY0, PY1 + 1):
        for x in range(PX0, PX1 + 1):
            if y <= PY0 + F and x >= PX1 - F:
                diag = (PX1 - F) + (y - PY0)
                if x > diag: continue
                _set(g, x, y, 3)
            else:
                _set(g, x, y, 1)
    for y in range(PY0 + F + 1, PY1):
        _set(g, PX1 - 1, y, 2); _set(g, PX1, y, 2)
    _hline(g, PX0 + 1, PX1, PY1 - 1, 2)
    # text lines
    for j, ly in enumerate(range(PY0 + 6, PY1 - 3, 4)):
        rx = (PX1 - 12) if j == 0 else (PX1 - 4)
        _hline(g, PX0 + 3, rx, ly, 4)
        _hline(g, PX0 + 3, rx, ly + 1, 5)
    # outline
    _hline(g, PX0, PX1 - F, PY0, 6)
    for i in range(F + 1):
        _set(g, PX1 - F + i, PY0 + i, 6)
    _vline(g, PX1, PY0 + F, PY1, 6)
    _hline(g, PX0, PX1, PY1, 6)
    _vline(g, PX0, PY0, PY1, 6)

def build_cidx():
    g = _new(); _draw(g); return g

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
