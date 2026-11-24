# MiSTer Minimig-AGA RTG: hardware notes

What the native `MINIMIG` driver (`src/drv_minimig.c`) is written from.
Facts only, read from the core's public source,
[MiSTer-devel/Minimig-AGA_MiSTer](https://github.com/MiSTer-devel/Minimig-AGA_MiSTer)
(GPL-3.0; commit `575b8e7`), and from the P96 driver that ships with it,
`extra/rtg_driver/MiSTer.card.asm` (LGPL-2.1). No code was taken over.

The "card" is not a graphics chip: the core lets the 68k write a frame
buffer in the DE10-Nano's DDR3, and the MiSTer framework's scaler
(`sys/ascal.vhd`) shows that buffer instead of the Amiga picture. There
is no blitter, no sprite, no CRTC and no pixel clock: the scaler
takes a size, a pixel format, a row length and a start address, and
scales the picture to whatever video mode MiSTer.ini sets for HDMI.

## 1. Finding it

* With a 32-bit CPU (`z3cpu`, `rtl/cpu_wrapper.v` 115: not the 68EC020,
  whose bus is 24 bits) the core has a Zorro III autoconfig board,
  manufacturer **0x139C**, product **0x30**: `rtl/cpu_wrapper.v` 790-801
  (the nibbles; `ac_rtg <= z3cpu` at 816). It is 16 MB in the Zorro III
  space; the base the OS writes goes to `rtg_base` (867-871). With the
  68EC020 chosen on the System page (Main_MiSTer `menu.cpp`: CPU bits
  $23; the plain "68020" is $03 and gets the board) there is none. The
  core's README (117) says "only for 68030 and 68EC030", which the RTL
  does not bear out.
* The board came with commit 3ea7f0e (2026-11-04). Every release core
  before it (the latest, `releases/Minimig_20260907.rbf`, is commit
  d760626) has only the fixed addresses below, and no `ec020`: a 68020
  there is a full 32-bit one. Its own boards are the Zorro II fast RAM
  (manufacturer 2011, product $51), the Toccata (18260/12, except in CDTV
  mode), the A2065 (0x0202/$70, a real Commodore ID) and, with Zorro III
  RAM, 0x139C/$10; the driver takes one of the first three, or
  0x139C, as the sign that it is on the core before reading $B80100.
* The low 8 MB of the board are the frame buffer
  (`rtl/memory_router.v` 41: `cpu_addr[31:23] == rtg_base`), the
  registers are at **board + $800100** (`rtl/cpu_wrapper.v` 132:
  `cpu_addr[23]` set, `[22:16]` clear; `MiSTer.card.asm` 94).
* Older cores (and the P96 driver's fallback) have fixed addresses: the
  frame buffer at **$02000000** (`memory_router.v` 41, `cpu_addr[31:24]
  == 8'h02`) and the registers at **$B80100** (`rtl/gary.v` 198,
  `rtl/fastchip.v` 251). The P96 driver uses them only with a 68020 or
  better and only when the ID register reads $5001 (`MiSTer.card.asm`
  361-370). Manufacturer 0x139C is also the core's Zorro III fast RAM
  (product 0x10, `cpu_wrapper.v` 774-789).
* The AP060 fork (apolkosnik/Minimig-AGA_MiSTer, branch AP060, commit
  1204c53: a 68060 in TG68K's place) has only the fixed addresses, with
  the same `rtg.v`, `gary.v` 198 and `memory_router.v` 39. Its own test
  `tests/ap060/asm/t_reset2.s` reads and writes $B80106.
* Under mmu.library (MMULib's 68040/68060.library, as on that 68060)
  neither window is in any table, so both are blank: reads give 0, and
  the ID reads $0000. This is the problem commit 3ea7f0e names. Before
  reading them, the driver, PrismSetup and PrismProbe make $B80000
  (64 KB) and $02000000 (8 MB) valid, cache-inhibited and unmoved, in
  the default context and its supervisor context (`src/minimig_mmu.h`).

## 2. Registers (`rtl/rtg.v` 21-31, 61-111)

16-bit registers, offsets from the register base:

| Offset | Bits | Meaning |
|---|---|---|
| $00 | 15:0 | ADDRESS[31:16]: frame buffer start, the scaler's (DDR) address |
| $02 | 15:0 | ADDRESS[15:0] |
| $04 | 4:0 | FORMAT (below). Only five bits are kept (`rtg.v` 72, `FB_FORMAT` is `[4:0]`) |
| $06 | 0 | ENABLE: 1 = the scaler shows the frame buffer, 0 = the Amiga picture. Cleared by reset (66) |
| $08 | 11:0 | HSIZE, pixels |
| $0A | 11:0 | VSIZE, lines |
| $0C | 13:0 | STRIDE, bytes per row (0 = the scaler works it out from HSIZE) |
| $0E | 15:0 | ID, read only: **$5001** (98) |
| $300-$6FF | | CLUT: 256 longwords $00RRGGBB (`rs[11:10] == 1`, 62; board $B80400 in the comment). The high word must be written first: its low byte is held and goes in with the low word (111) |

All registers read back.

* **Address**: the 68k's frame buffer offset plus **$27000000**, where
  the core puts it in DDR (`memory_router.v` 61: `ramaddr[26:23] = 1110`
  for the RTG window, i.e. $07000000 into the core's DDR area at
  $20000000; `MiSTer.card.asm` 90). The scaler copies it at its output
  vertical sync (`sys/ascal.vhd` 1713-1717), so a new start never tears.
  Being two 16-bit writes, a change of the high word can be caught
  between them for one frame.
* **Stride**: any value (`ascal.vhd` 2007-2011); the start and row
  addresses need not be aligned to the scaler's 16-byte word
  (`pixoffset`, 721-731), but each row is fetched as
  ceil(HSIZE x bytes per pixel / 256) bursts from that address
  (2004-2005), so a start that is not 16-byte aligned can lose the
  last pixels of a row. The driver keeps starts and rows on 16 bytes.
* Size limits: HSIZE/VSIZE 12 bits, STRIDE 14 bits (16383 bytes). The
  scaler's line buffers hold `OHRES` pixels, 2304 by default
  (`ascal.vhd` 128); the P96 driver offers up to 2048x2048.

## 3. Pixel formats

FORMAT (`ascal.vhd` 41-45): bits 2:0 = 011 8-bit (CLUT), 100 16-bit,
101 24-bit, 110 32-bit; bit 3 = 1-5-5-5 instead of 5-6-5; bit 4 =
exchange red and blue. The P96 driver's table also sets a bit 5 "swap
bytes", which the core drops (above).

The CPU's accesses to the frame buffer are byte-swapped on the way into
DDR (`rtl/cpu_wrapper.v` 164-166) and the scaler swaps back as it reads
(`ascal.vhd` 734-745, `BYTESWAP`), so the bytes lie in DDR in the order
the 68k wrote them. The scaler then takes pixels as (`shift_opix`,
`ascal.vhd` 700-717; bit 4 applied at 2302-2304):

| FORMAT | Bytes as the 68k sees them | PrismRTG format | P96 driver |
|---|---|---|---|
| $03 | pen | `PF_CLUT8` | RGBFB_CLUT (offered) |
| $04 | `gggrrrrr bbbbbggg` | `PF_BGR565LE` | B5G6R5PC |
| $14 | `gggbbbbb rrrrrggg` | `PF_RGB565LE` | R5G6B5PC (offered) |
| $0C | `gggrrrrr xbbbbbgg` | `PF_BGR555LE` | |
| $1C | `gggbbbbb xrrrrrgg` | `PF_RGB555LE` | |
| $05 | R, G, B | `PF_RGB24` | R8G8B8 |
| $15 | B, G, R | `PF_BGR24` | B8G8R8 (offered) |
| $06 | R, G, B, x | `PF_RGBA32` | R8G8B8A8 (offered) |
| $16 | B, G, R, x | `PF_BGRA32` | B8G8R8A8 |

The four formats the P96 driver offers (`MiSTer.card.asm` 399-402) are
the ones known to work on the core; the rows they confirm fix the byte
order of the others. The 15-bit rows follow from `shift_opix` alone
(the P96 driver's table lists them with bit 4 the other way round, but
never offers them).

PrismRTG's MINIMIG driver offers CLUT8, `PF_RGB565LE`, `PF_RGB555LE`,
`PF_BGR24` and `PF_BGRA32` (FORMAT $16, not MiSTer.card's $06). $16 was
checked on the AP060 core: right colours on screen and PrismBench exact
at 32-bit (2026-11-24).

## 4. What there is not

* No vertical blank status or interrupt (`MiSTer.card.asm`
  `WaitVerticalSync` does nothing); the Amiga's own beam is not in step
  with the HDMI output. The start address is latched at the scaler's
  vertical sync anyway (2).
* No blitter and no hardware sprite. The frame buffer is DDR, but the
  CPU reaches it uncached, 16 bits at a time, through the core's chip-bus
  side (on the AP060 core across two clock domains): far slower than fast
  RAM, reads above all. Shadow mode (`PBF_SHADOW`, drawing in fast RAM
  and writing only the changes) was tried on the AP060 core's 68060 and
  was much slower, so the driver draws straight into the frame buffer.
* No monitor power control: ENABLE = 0 shows the Amiga's picture, not a
  blank one.
