#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Laine Jones
"""
iconlib - shared machinery for generating AmigaOS .info icons.

Given a chunky colour-index image (cidx[h][w]), a PALETTE (list of (r,g,b),
index 0 = transparent/background) and a PLANAR_MAP (colour index -> 4-colour
planar index 0..3), it emits a DUAL .info:
  * a classic 4-colour planar Image (works on any Workbench 1.3/3.1), and
  * an appended OS3.5-style colour icon (FORM ICON / FACE+IMAG) with its own
    embedded RGB palette (OS 3.2's icon.library prefers this).

It also writes a colour PNG preview (no external deps).

The colour-icon RLE and FORM layout were validated against OS 3.2's own glow
icons by makeicon_amelinium.py; this is the same code, generalised.
"""
import struct, zlib

# ---- OS3.5 colour-icon RLE (bit-level PackBits) -------------------------
class _BitWriter:
    def __init__(self): self.out = bytearray(); self.acc = 0; self.nbits = 0
    def write(self, n, v):
        for i in range(n - 1, -1, -1):
            self.acc = (self.acc << 1) | ((v >> i) & 1)
            self.nbits += 1
            if self.nbits == 8:
                self.out.append(self.acc); self.acc = 0; self.nbits = 0
    def flush(self):
        if self.nbits:
            self.out.append((self.acc << (8 - self.nbits)) & 0xFF)
        return bytes(self.out)

def _rle_encode(symbols, depth):
    bw = _BitWriter(); i = 0; n = len(symbols)
    while i < n:
        run = 1
        while i + run < n and symbols[i + run] == symbols[i] and run < 128:
            run += 1
        if run >= 2:
            bw.write(8, 257 - run); bw.write(depth, symbols[i]); i += run
        else:
            start = i; cnt = 0
            while i < n and cnt < 128:
                if i + 1 < n and symbols[i + 1] == symbols[i]:
                    break
                i += 1; cnt += 1
            bw.write(8, cnt - 1)
            for j in range(start, i):
                bw.write(depth, symbols[j])
    return bw.flush()

def _iff_chunk(cid, data):
    c = cid + struct.pack(">I", len(data)) + data
    return c + b"\x00" if len(data) & 1 else c

def _imag(flat, depth, palette, transparent):
    img = _rle_encode(flat, depth)
    palbytes = []
    for (r, g, b) in palette:
        palbytes += [r, g, b]
    pal = _rle_encode(palbytes, 8)
    hdr = struct.pack(">BBBBBBHH",
        transparent, len(palette) - 1, 0x03, 1, 1, depth,
        len(img) - 1, len(pal) - 1)
    return hdr + img + pal, len(pal)

def _colour_form(cidx, cidx_sel, palette, transparent, w, h):
    """cidx_sel None -> the select image is the normal one, palette-brightened
    (old behaviour). Otherwise cidx_sel is a genuinely different select image,
    drawn with the same palette."""
    flat = [cidx[y][x] for y in range(h) for x in range(w)]
    depth = max(1, (len(palette) - 1).bit_length())
    imag1, pl1 = _imag(flat, depth, palette, transparent)
    if cidx_sel is None:
        sel_pal = [(min(255, int(r * 1.18)), min(255, int(g * 1.18)),
                    min(255, int(b * 1.18))) for (r, g, b) in palette]
        imag2, pl2 = _imag(flat, depth, sel_pal, transparent)
    else:
        flatS = [cidx_sel[y][x] for y in range(h) for x in range(w)]
        imag2, pl2 = _imag(flatS, depth, palette, transparent)
    maxpal = max(pl1, pl2)
    face = struct.pack(">BBBBH", w - 1, h - 1, 0, 0, maxpal - 1)
    form = (b"ICON" + _iff_chunk(b"FACE", face)
            + _iff_chunk(b"IMAG", imag1) + _iff_chunk(b"IMAG", imag2))
    return b"FORM" + struct.pack(">I", len(form)) + form

# ---- classic planar image ----------------------------------------------
def _planar(cidx, planar_map, w, h, depth=2):
    roww = (w + 15) // 16
    data = bytearray()
    for p in range(depth):
        for y in range(h):
            for wd in range(roww):
                word = 0
                for bit in range(16):
                    x = wd * 16 + bit
                    if x < w:
                        if (planar_map[cidx[y][x]] >> p) & 1:
                            word |= 1 << (15 - bit)
                data += struct.pack(">H", word)
    return bytes(data)

# ---- DiskObject wrapper (WBTOOL) ----------------------------------------
# flags: GFLG_GADGIMAGE(0x0004) always; with a select image also GFLG_GADGHIMAGE
# (0x0002) so the classic renderer swaps to SelectRender instead of complement.
def _disk_object(w, h, stack, has_sel=False, icon_type=3, has_default_tool=False,
                 has_drawer=False, has_tool_types=False):
    """icon_type: 2 = WBDRAWER (a directory; 1 is WBDISK - Workbench ignores a disk icon inside a drawer), 3 = WBTOOL (an executable),
    4 = WBPROJECT (a data/script file launched via its DefaultTool).
    has_default_tool sets do_DefaultTool non-NULL so a DefaultTool string is
    expected to follow the image data. has_drawer sets do_DrawerData non-NULL so
    a 56-byte DrawerData is expected right after the DiskObject."""
    PLACE = 0x00000064
    flags = 0x0004 | (0x0002 if has_sel else 0x0000)
    gadget = struct.pack(">IhhhhHHHIIIiIHI",
        0, 0, 0, w, h, flags, 0x0003, 0x0001,
        PLACE, (PLACE if has_sel else 0), 0, 0, 0, 0, 0)
    return (struct.pack(">HH", 0xE310, 1) + gadget
        + struct.pack(">BB", icon_type, 0)    # do_Type, pad
        + struct.pack(">I", 1 if has_default_tool else 0)   # do_DefaultTool ptr
        + struct.pack(">I", 1 if has_tool_types else 0)   # do_ToolTypes ptr
        + struct.pack(">II", 0x80000000, 0x80000000)        # CurrentX/Y = NO_POS
        + struct.pack(">I", 1 if has_drawer else 0)         # do_DrawerData ptr
        + struct.pack(">I", 0)                # do_ToolWindow ptr
        + struct.pack(">i", stack))

# DrawerData (56 bytes): a NewWindow (48) + dd_CurrentX/Y. Defines the window
# that opens when the drawer is double-clicked.
def _drawer_data():
    nw = struct.pack(">hhhhBBIIIIIIIhhHHH",
        48, 32, 380, 160,          # LeftEdge, TopEdge, Width, Height
        255, 255,                  # DetailPen, BlockPen
        0,                         # IDCMPFlags
        0x0000100F,                # Flags: SIZE|DRAG|DEPTH|CLOSE|SIZEBRIGHT
        0, 0, 0, 0, 0,             # FirstGadget, CheckMark, Title, Screen, BitMap
        90, 40,                    # MinWidth, MinHeight
        0xFFFF, 0xFFFF,            # MaxWidth, MaxHeight
        1)                         # Type = WBENCHSCREEN
    return nw + struct.pack(">ii", 0, 0)      # dd_CurrentX, dd_CurrentY

# A DiskObject string (DefaultTool/ToolWindow): LONG size (incl. NUL) + bytes.
def _do_string(s):
    b = s.encode("latin-1") + b"\x00"
    return struct.pack(">I", len(b)) + b

def _image(w, h, depth=2):
    return struct.pack(">hhhhhIBBI", 0, 0, w, h, depth, 0x00000064, 0x03, 0x00, 0)

def build_info(cidx, palette, planar_map, w=48, h=40, transparent=0, stack=16384,
               cidx_sel=None, icon_type=3, default_tool=None, drawer=False, tool_types=None,
               classic=None, classic_sel=None, classic_map=None):
    """cidx_sel: optional, a genuinely different SELECT (click) image. When
    given, the icon carries a real SelectRender (classic) and a second colour
    IMAG (OS3.5), instead of highlight-by-complement.

    icon_type / default_tool: pass icon_type=4 (WBPROJECT) with a default_tool
    (e.g. "IconX") to make a double-clickable data/script icon whose DefaultTool
    runs it. The DefaultTool string is serialised after the image data, before
    the appended OS3.5 colour FORM.

    drawer=True makes a WBDRAWER icon (do_Type forced to 2) with a DrawerData
    block, so the icon represents a directory that opens a window.

    tool_types: optional list of ToolType strings (e.g. ["WINDOW=NIL:"]),
    serialised after the DefaultTool as a LONG (count+1)*4 and one sized
    string each.

    classic / classic_sel / classic_map: optional hand-made 4-colour images for the
    planar (OS 3.0/3.1) part, instead of reducing the colour image - a glow icon's
    shading doesn't survive 4 pens.  classic_map maps their indices to pens 0..3;
    with no classic_sel the selected image is the classic one complemented."""
    has = cidx_sel is not None
    has_dt = default_tool is not None
    if drawer:
        icon_type = 2                   # WBDRAWER (1 = WBDISK: invisible in a drawer)
    parts = [_disk_object(w, h, stack, has, icon_type, has_dt, drawer, bool(tool_types))]
    if drawer:
        parts.append(_drawer_data())
    if classic is not None:
        cmap = classic_map or [0, 1, 2, 3]
        parts += [_image(w, h), _planar(classic, cmap, w, h)]
        if has:
            if classic_sel is None:     # complement every non-background pixel, as Workbench would
                pens = [cmap[i] for i in range(len(cmap))]
                classic_sel = [[(3 - pens[c]) if pens[c] else 0 for c in row] for row in classic]
                parts += [_image(w, h), _planar(classic_sel, [0, 1, 2, 3], w, h)]
            else:
                parts += [_image(w, h), _planar(classic_sel, cmap, w, h)]
    else:
        parts += [_image(w, h), _planar(cidx, planar_map, w, h)]
        if has:
            parts += [_image(w, h), _planar(cidx_sel, planar_map, w, h)]
    if has_dt:
        parts.append(_do_string(default_tool))
    if tool_types:
        parts.append(struct.pack(">I", (len(tool_types) + 1) * 4))
        parts += [_do_string(tt) for tt in tool_types]
    parts.append(_colour_form(cidx, cidx_sel, palette, transparent, w, h))
    return b"".join(parts)

# ---- colour PNG preview -------------------------------------------------
def write_png(path, cidx, palette, w=48, h=40, transparent=0, scale=6, bg=(176, 176, 176)):
    sw, sh = w * scale, h * scale
    raw = bytearray()
    for y in range(sh):
        raw.append(0)
        for x in range(sw):
            v = cidx[y // scale][x // scale]
            raw += bytes(bg if v == transparent else palette[v])
    def chunk(typ, data):
        c = typ + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xffffffff)
    png = (b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", sw, sh, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
        + chunk(b"IEND", b""))
    with open(path, "wb") as f:
        f.write(png)
    print("wrote", path)
