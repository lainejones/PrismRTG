# SAGA (Apollo Vampire V4) - hardware notes

The facts `src/drv_saga.c` is written from. There is no data book; they
were read from the Apollo team's own Picasso96 driver source and written
down here. **The driver has not run on a Vampire yet** (test driver,
`BOARD=SAGA`).

## Sources

* **flype44/Vampire** (github.com/flype44/Vampire, flype and the
  APOLLO-Team, MPL-2.0), `VIDEO/sagacard/`: `SAGACard.h` (register
  addresses, formats, sprite and PiP registers), `SAGACard_FindCard.c`
  (detection), `SAGACard_InitCard.c` (memory, flags), `SAGACard_Settings.c`
  (the size table), `SAGACard_Support.c` (format, display start, modulo,
  palette), `SAGACard_Sprite.c` (the hardware sprite). Read in October 2026
  (SAGA.card 1.0 beta 1, 24.5.21).
* **ezrec/saga-drivers** (github.com/ezrec/saga-drivers, Jason S.
  McMullan, MIT, 2016), `include/saga/video.h`: the same display start,
  format and palette registers on the earlier V600 core.

No code was taken from either; only the numbers below.

## Detection

Autoconfig manufacturer $1398 (APOLLO-Team), product 5 (Vampire V4,
V4+). The registers are in the custom chip range at $DFF000.

## Frame buffer

SAGA has no video memory of its own: it shows ordinary fast RAM. The
Apollo driver allocates 4 MB (MEMF_FAST) and aligns it to 32 bytes; the
display start is rounded to 16 bytes. There is no blitter for RTG
(BIF_NOBLITTER): all drawing is the CPU's.

## Registers (offsets from $DFF000)

| Offset | Size | What |
|---|---|---|
| $1D0 | W | sprite X (screen x + 16) |
| $1D2 | W | sprite Y (screen y + 8) |
| $1E6 | W | display modulo: bytes skipped after each row |
| $1EC | L | display start (address, 16-byte aligned) |
| $1F4 | W | FORMAT: size number << 8 \| pixel format; 0 = RTG display off |
| $388 | L | palette: index << 24 \| R << 16 \| G << 8 \| B, one entry per write |
| $38C | L | PiP palette |
| $3A2 | W x3 | sprite colours 1-3, 12-bit 0RGB |
| $3D0-$3D6 | W | PiP window x/y start, x/y stop (x + 16) |
| $3D8 | L | PiP display start |
| $3DC | W | PiP pixel format |
| $3DE | W | PiP modulo |
| $3E0 | W | PiP colour key |
| $3E2 | W | PiP DMA row length |
| $800 | L x16 | sprite image: 16 rows, plane 0 << 16 \| plane 1 |

Pixel formats: 0 off, 1 CLUT8, 2 RGB16 (R5G6B5), 3 RGB15, 4 RGB24
(R,G,B), 5 RGB32 (A,R,G,B), 6 YUV422, 8-10 planar 1/2/4.

## Sizes

The display timing is fixed per size; the number goes into FORMAT's high
byte.

| No. | Size | | No. | Size |
|---|---|---|---|---|
| 1 | 320x200 | | 9 | 304x224 |
| 2 | 320x240 | | A | 1280x720 |
| 3 | 320x256 | | B | 640x360 |
| 4 | 640x400 | | C | 800x600 |
| 5 | 640x480 | | D | 1024x768 |
| 6 | 640x512 | | E | 720x576 |
| 7 | 960x540 | | F | 848x480 |
| 8 | 480x270 | | | |

## Not used yet

* No vertical blank interrupt of its own (the Apollo driver's
  WaitVerticalSync does nothing); the chipset's is used.
* No split register for screen dragging ("not available on V4" in the
  Apollo driver's flags). The PiP layer - a second display start, format
  and window - could show a dragged screen over the one behind it; not
  tried.
