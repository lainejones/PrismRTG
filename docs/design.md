# Prism — design

> Development notes, in the order things happened. The project was called
> "Prism" while it was being written and is published as **PrismRTG**; the
> files kept their short names. Read the README first.

Prism is our own RTG (ReTargetable Graphics) system for AmigaOS 3.x: a from-scratch
replacement for Picasso96, built for fun and scoped to the hardware we own.

## Scope

| Machine | CPU | Board | Chip | VRAM | Formats we care about |
|---|---|---|---|---|---|
| A4000 | 68060 @ 50 (TF4060) | MNT ZZ9000 (Zorro III) | Xilinx Zynq (ARM does the blits) | 128 MB | 8 / 16 / 32-bit |
| A2000 | 68030 @ 50 (TF536), no FPU | Village Tronic GBAPII++ (Picasso II remake, Zorro II) | Cirrus GD542x | 2 MB | 8 / 16-bit |

Development box with no real hardware: WinUAE config **`prism p2`** (OS 3.2.3, 68030, no FPU,
emulated **Picasso II+**, GD5428, 2 MB). Its `DH0:` is `C:\Amiga\Prism-P2`, a private copy of
the clean 3.2.3 box, so Prism tests never disturb the shared test machines.
WinUAE has no ZZ9000 emulation, so the ZZ9000 driver is tested on the real A4000 only.

Non-goals: other boards, Picasso96 `.card` binary compatibility, OS 4 / MorphOS, 68000.

## Architecture

```
  applications ──► graphics.library / intuition / layers   (OS ROM)
                          │  patched by SetFunction
                          ▼
                    prism.library  ── display DB, screens, bitmaps,
                          │            software renderer, CGX shim (later)
                          ▼   PrismBoard function table
             ┌────────────┴────────────┐
       zz9000.driver             picasso2.driver       (LIBS:Prism/)
             │                         │
          ZZ9000                  Cirrus GD542x
```

* **Board drivers** are small and dumb: find the board, set a mode, set palette, set the
  display start, switch the monitor between native and RTG, wait for vblank, drive the
  hardware cursor, and optionally accelerate fills/copies/colour expansion. Our own
  interface (`src/prismboard.h`), not P96's BoardInfo: it only grows when a feature needs it.
* **prism.library** owns everything else. Every accelerated operation has a CPU fallback in
  the software renderer, so a driver can start with no acceleration at all.
* **Loader** (`PrismInit`) runs from the Startup-Sequence before `LoadWB` (or as a
  `DEVS:Monitors` entry), opens prism.library, which loads the drivers and installs patches.

### Safety rule

Prism installs **side by side** with Picasso96 and only takes over when started. P96 stays
the default on both real machines until Prism earns the slot, so a bad build can never leave
a machine without a display. (The A2000 can't be harness-rebooted: `ColdReboot()` greyscreens
it and it needs a power-cycle.)

## Milestones

| | Goal | Done when |
|---|---|---|
| **M0** | Bare-metal spike | `PrismTest` takes over each board from a Shell, sets 640x480x8 (+16-bit), draws a test pattern, then returns the monitor to native video. No OS integration. |
| **M1** | Screens | RTG ModeIDs appear in ScreenMode; `OpenScreen` on one gives a chunky bitmap in VRAM and the monitor switches. Drawing by direct pokes only. |
| **M2** | Graphics patches | `BltBitMap`, `RectFill`, `Text`/`BltTemplate`, `ScrollRaster`, `Read/WritePixel`, `AllocBitMap` (chunky + friend) work on RTG bitmaps; Workbench runs in 8-bit. |
| **M3** | Real use | 16/32-bit, pen/palette mapping, hardware cursor, screen dragging + native/RTG switching, blitter acceleration on both boards. |
| **M4** | Apps | `cybergraphics.library` shim covering what IBrowse / Amelinium / AWeb actually call. |
| **M5** | Prefs | GadTools prefs editor for modes (EnvEdit style). |
| **M6** | Install + boot | `DEVS:Monitors/Prism` starts PrismD before IPrefs so Workbench opens on a Prism mode from ScreenMode prefs; left mouse button at boot skips it; Install/Uninstall package. |
| **M7** | True colour | 24-bit screens on the Picasso II (GD5426/28: 640x400 + 640x480, VCLK = 3x), 32-bit on the ZZ9000 (untested); pens drawn through the screen's colours; CGX 24/32-bit requests get them. |
| **M8** | VRAM + double buffering | Screens beyond VRAM page to fast RAM and back (LRU, never the shown or a locked bitmap); `AllocScreenBuffer`/`ChangeScreenBuffer` flip the card at vblank and deliver the DBufInfo messages. |

## Hardware references

* [`zz9000-hw.md`](zz9000-hw.md) — ZZ9000 registers, mode setting, blitter, sprite.
* [`cirrus-picasso2-hw.md`](cirrus-picasso2-hw.md) — Picasso II Zorro layout + GD542x programming.

## Licence

GPL version 3 (changed from MIT on 2026-10-03, Laine's decision). The drivers are still
written by us from the hardware references in `docs/` (register facts, not code): the
ZZ9000 driver and firmware are GPL-3.0-or-later, which is compatible, but NetBSD and
Linux (GPL-2.0-only) sources are not to be pasted into this tree.

## M1 as built (2026-10-03)

`PrismD` is a background program (it becomes `prism.library` later). It:

* **Display database:** patches `NextDisplayInfo`, `FindDisplayInfo`, `GetDisplayInfoData`,
  `ModeNotAvailable`, `OpenMonitor`, `CloseMonitor`. Prism ModeIDs are `0x7A00xxxx` (one "monitor",
  low 12 bits = mode index). A `DisplayInfoHandle` for a Prism mode points at Prism's own mode record.
  graphics.library's private `AddDisplayInfoData()` cannot create records for new IDs
  (`tools/dbtest.c`), so patching is the route.
* **Screens:** patches `OpenScreenTagList`, `OpenScreen`, `CloseScreen`. A screen on a Prism mode gets a
  VRAM framebuffer (first-fit allocator), and Intuition gets `SA_BitMap` = a bitmap whose planes all
  alias **one** chip-RAM plane, so Intuition's planar drawing can't land anywhere harmful. Intuition
  renders through its own copy at `&Screen->BitMap`, so screens are matched by that shadow plane.
* **Display:** `MakeVPort` returns `MVP_OK` without a copper list for Prism viewports, `MrgCop`
  merges with them unlinked, and `LoadView` makes the card follow `IntuitionBase->FirstScreen`:
  mode, display start, palette, monitor switch. The mode is programmed again on each return from
  native video (WinUAE needs it, and it costs nothing).
* **Palette:** `SetRGB32`, `LoadRGB32`, `SetRGB4`, `LoadRGB4` forward the ColorMap to the DAC when
  the viewport is the one on the card.
* **Clients** find a `PrismSem` semaphore named `prism` (`src/prism.h`) with `screenInfo()` (the
  framebuffer of a Prism screen) and the daemon's task (Ctrl-C = quit).

Not in M1: graphics.library drawing into Prism screens (M2 — until then Workbench on a Prism mode
would be invisible), a mouse pointer on the card (M3), `BestModeIDA`, 16-bit screens.

## M2 as built (2026-10-03)

Goal met in WinUAE: **Workbench 3.2 runs in 8-bit on the Picasso II+**, with colour icons, drawer
windows, a scrolling Shell and window moves restored from backing store.

* **Prism bitmaps** (`src/bitmap.c`): an ordinary `struct BitMap` whose planes all alias one chip-RAM
  dummy plane, marked by `Flags` bit 7 + an index in `pad` (both survive Intuition copying the struct).
  Pixels are 8-bit chunky: screens in VRAM, friend bitmaps (`AllocBitMap` with a Prism friend: layers'
  backing store, Workbench's buffers) in fast RAM. Anything we fail to intercept draws into the dummy
  plane, harmlessly and invisibly.
* **Trampolines** (`src/stubs.S`): every rendering patch saves `d0-d7/a0-a6`, calls `h_<name>(regs)`;
  return 0 = fall through to the original untouched, 1 = handled. `call_regs()` lets a handler run the
  original first (pen allocation).
* **Renderer** (`src/render.c`): BltBitMap (chunky/planar both ways, minterms, plane masks),
  BltBitMapRastPort, BltMaskBitMapRastPort, ClipBlit, BltTemplate, BltPattern, RectFill, Text, Draw,
  PolyDraw, Read/WritePixel, SetRast, ScrollRaster(BF), EraseRect, Read/WritePixelArray8/Line8,
  WriteChunkyPixels. `clip_rp()` walks a layer's ClipRects (visible → the screen bitmap, obscured →
  its 16-pixel-aligned backing store). Solid fills and in-VRAM copies use the board's blitter.
* **Lock order**: layer lock first, then Prism's lock — Intuition calls us holding layers.
* **Workbench's mode** comes from Intuition's private prefs, not an `SA_DisplayID` tag. While a task is
  inside the original `OpenScreen(TagList)`, a Prism mode lookup marks it, and its displayable
  `AllocBitMap` returns a Prism VRAM bitmap (Intuition owns and frees it).
* **Palette**: `ObtainPen` / `ObtainBestPenA` / `SetRGB32CM` re-sync the DAC (Workbench's icon pens).
* **Cold card**: PrismD programs a mode at startup — a cold Cirrus drops CPU VRAM writes until its
  linear window is set, and screens are drawn before they are shown.

Test box: create `SYS:Prism/boot-prism` (host: `C:\Amiga\Prism-P2\Prism\boot-prism`) and the box boots
Workbench on PRISM:640x480 (Startup-Sequence starts PrismD before IPrefs and swaps ScreenMode prefs;
without the flag the native prefs go back).

Not yet: a mouse pointer on the card (M3), 16/32-bit screens, AreaEnd/Flood/ellipses, italic text,
color fonts, SuperBitMap layers, planar backing store (never seen on 3.2), screen dragging.

## M3 so far (2026-10-03)

* **Pointer on the card** (`src/pointer.c`): MoveSprite / ChangeSprite / ChangeExtSpriteA run the
  original, then sprite 0's image (16/32/64 wide) goes to the board's hardware cursor (GD542x: 64x64,
  2 colours + invert; pen 3 folds onto the nearer colour). On a Prism screen Intuition's sprite X is
  always 0 although Y is right, so X = Screen->MouseX with Y's hot-spot offset. Moves from
  input.device apply immediately if Prism's lock is free, else on PrismD's next tick (every frame).
* **16-bit screens**: `PRISM:WxH 16bit` modes in the board's 16-bit format. Intuition still sees 256
  pens; each screen has a pen -> RGB table rebuilt from its ColorMap. The renderer is bpp-aware
  (`put_px`, 16-bit blits with minterms evaluated in pen space). A new 16-bit screen is painted pen 0
  when its colours first load (cleared memory is black, but Intuition assumes pen 0). Workbench runs
  in 16-bit as well as 8-bit.
* **Speed**: text builds a 1-bit template of the whole string in fast RAM (byte-wise glyph OR, bold,
  underline) and expands it 8 bits at a time; JAM2 rows are composed in fast RAM (nibble table, long
  writes) and copied out once. `PrismBench` measures the common calls:

  | WinUAE, emulated 030 | Prism 8-bit before | after | Prism 16-bit | native AGA 8 planes |
  |---|---|---|---|---|
  | Text JAM1, 40 chars | 96/s | 147/s | 154/s | 329/s |
  | Text JAM2, 40 chars | 96/s | 168/s | 129/s | 327/s |
  | ClipBlit 200x100 | 141/s | 143/s | 95/s | 53/s |
  | Line ~390 px | 223/s | 212/s | 195/s | 192/s |

  (WinUAE finishes card blits instantly, so fills/scrolls are not meaningful here; real numbers come
  from the A2000.)

Still open for M3: screen dragging, Cirrus colour-expansion for text (CPU-fed template, row padding
differs between WinUAE and silicon), ZZ9000 cursor + blitter (needs the A4000).

## M4 as built (2026-10-03)

`cybergraphics.library` 41 lives inside PrismD (`src/cgx.c`): built with `MakeLibrary` and added to
exec's list unless another one is running; PrismD won't quit while it is open. It covers mode queries
(`IsCyberModeID`, `BestCModeIDTagList` — 24/32-bit requests get Prism's 16-bit modes —
`AllocCModeListTagList`, `GetCyberIDAttr`), bitmap access (`GetCyberMapAttr`, `LockBitMapTagList`),
pixel arrays through RastPort clipping (`Read/WritePixelArray` in RGB/RGBA/ARGB/LUT8/GREY8,
`WriteLUTPixelArray`, `ScalePixelArray`, `Read/WriteRGBPixel`, `Fill/Invert/MovePixelArray`),
`DoCDrawMethodTagList` (hook per visible piece) and `ExtractColor`; `CModeRequest` and `CVideoCtrl`
are stubs. Calls are counted and the first few of each logged.

Off-screen bitmaps may be any CyberGraphX format (`AllocBitMap` with `BMF_SPECIALFMT`: LUT8,
RGB15/16 both byte orders, RGB24, BGR24, ARGB32, BGRA32, RGBA32); blits convert through 0x00RRGGBB.
Pen drawing stays limited to 8/16-bit bitmaps.

Verified in WinUAE on Workbench at PRISM:640x480 16bit: MultiView shows a JPEG in true colour
(picture.datatype), Workbench draws its backdrop through `WritePixelArray`, AWeb 3.6 renders a local
page with exact RGB colours and JPEG/PNG images, Amelinium 0.7.5 runs (TrueType text, CGX fills).
Screenshots in `docs/screenshots/`. `tools/PrismMouse ... CLICK` clicks from inside the Amiga, which
is how the test got past Amelinium's screen-mode requester.

## M5 as built (2026-10-03)

Settings live in `ENV:Prism.prefs` (plus `ENVARC:` on Save), plain `KEY=VALUE` text read by
`src/prefs.c`, which PrismD and the editor share: `BOARD=AUTO|PICASSO2|ZZ9000`, `BLITTER=ON|OFF`,
`LOG=ON|OFF`, and one `MODE=WxHxD ON|OFF Hz` per candidate mode. PrismD reads it at start-up;
`BOARD=`/`LOG` on its command line still win, `PREFS=` points it at another file.

**ModeIDs are stable now.** Every candidate (6 sizes x 8/16-bit) has a fixed slot and its ID is
`$7A00` + depth slot << 8 + size slot, so switching a mode off never renumbers the others. 8-bit IDs
are unchanged; 16-bit moved to `$7A0001xx` (640x480 16-bit is `$7A000101`). New sizes go at the end
of `prefs_sizes`.

**Refresh:** the Picasso II driver has VESA 70/72/75 Hz timings beside 60 Hz; `PrismMode.refresh`
picks one (0 = the size's first entry) and the driver writes back what it used. A rate the board
can't do falls back to the default with a message. **Blitter off** NULLs the board's fill/copy
callbacks so everything draws on the CPU (a diagnostic for the real GBAPII++). **Log off** makes
`dbg()` return at once.

`PrismPrefs` (`tools/prismprefs.c`) is a fixed-size GadTools window, font-sensitive: the mode list
(on/off, size, depth, refresh, and "live" for modes the running PrismD offers, found with
`ModeNotAvailable`), Offered + Refresh for the selected mode, Board / Use blitter / Debug log, a status
line and Save / Use / Defaults / Cancel (Esc cancels). Like the system editors it takes
`FROM= USE SAVE` to work without a window. Changes take effect when PrismD next starts.

Verified in WinUAE: PrismMouse clicks toggled a mode, set 72 Hz, ticked Debug log and pressed Use;
the written file matched. Booting with `640x480x16 ON 75` + `BLITTER=OFF`, Workbench came up on
`$7A000101` at 75 Hz drawn by the CPU, with PrismPrefs showing the live modes
(`docs/screenshots/m5-*.png`).

## M6 as built (2026-10-03)

Prism now starts the way Picasso96 does. OS 3.2's Startup-Sequence runs `LoadMonDrvs` (every file
in `DEVS:Monitors`) before `IPrefs`, so `DEVS:Monitors/Prism` (`tools/prismmon.c`, built as
`out/PrismMon`) launches `C:PrismD` with `SystemTags(SYS_Asynch)`, input/output on NIL: (or
`T:PrismD.log` when the prefs say `LOG=ON`), and waits up to 5 s for the `prism` semaphore. IPrefs
then finds the PRISM: modes and Workbench opens on whatever `ENVARC:Sys/ScreenMode.prefs` names.

* **Way out:** holding the left mouse button during boot (CIA-A PRA bit 6) skips Prism; Workbench
  falls back to a native mode by itself because the Prism ModeID isn't available.
* **No board / no mode:** PrismD exits, the launcher times out (5 s), Intuition opens Workbench on
  the default native mode. Verified with `BOARD=ZZ9000` on the Picasso II box.
* **Package:** `tools/mkpkg.sh` (run by build.sh) stages `out/pkg/Prism` + drawer icon: PrismD,
  Prism (monitor), PrismPrefs, `Install` / `Uninstall` (AmigaDOS scripts, IconX icons), ReadMe.
  Install copies to C:, DEVS:Monitors, SYS:Prefs and writes a default `ENVARC:Prism.prefs` if there
  is none; Uninstall deletes them all.

Verified in WinUAE: install from the package drawer, ScreenMode set to PRISM:640x480 8bit in
ENVARC, plain reboot (no test hook) -> Workbench on the card; BOARD=ZZ9000 -> native Workbench;
Uninstall leaves nothing behind. Not yet testable here: the mouse-button skip (needs a real hand).

## M7 as built (2026-10-03)

**True-colour screens.** A third prefs depth slot (ModeIDs `$7A0002xx`, `MODE=WxHx24` in
`ENV:Prism.prefs`) gives each board's true-colour format: `PF_BGR24` on the Picasso II (SR7 `x5`, HDR
`0xC5`, B,G,R bytes), `PF_BGRA32` on the ZZ9000 (named "32bit"; untested). On the GD5426/28 the
VCLK runs at 3x the pixel clock with the horizontal CRTC values unchanged (hw doc §2.3), so the pixel
clock limit is 86/3 MHz: **640x400 and 640x480** at 24 bits. The 5434 doesn't triple; it is allowed
57.3 MHz until measured on the GBAPII++.

**Rendering.** Intuition still sees 256 pens. A 24/32-bit bitmap turns a pen into a pixel through
its screen's `rgbTab` (pen -> 0x00RRGGBB, already kept for cgx) and `pf_put`; reads go back to pens
by exact colour match. The renderer gained 24/32-bit paths for pixels, fills, text (JAM2 rows built
in fast RAM), masked blits (colour copy through the mask) and pen minterms; copies between formats
go through 0x00RRGGBB as before. `rp_is_prism` now takes 24/32-bit bitmaps that have a screen's
colours; a bare CGX image buffer still isn't drawn into with pens.

**Blitter at 24 bits.** Copies are byte copies, so they work unchanged. The 5426/28 can't colour-
expand 24-bit, so a fill writes its first row on the CPU and the blitter doubles it down (1, 2, 4 ...
rows per copy); greys (all bytes equal) fill as bytes in one go. VRAM clears are byte fills at any
depth. Also fixed: ClipBlit's temporary buffer didn't keep the source's pixel format (16-bit
ClipBlits went through the wrong conversion).

**cybergraphics:** `BestCModeIDTagList` gives 24/32-bit requests a true-colour mode, falling back to
16-bit when none is on (too big for the card or switched off).

Verified in WinUAE: the M2 graphics test on PRISM:640x480 24bit matches the 8-bit one; Workbench runs
in 24-bit (backdrop picture through WritePixelArray, icons, Clock, console text); MultiView shows a
PNG in true colour (`docs/screenshots/m7-*.png`). 16-bit Workbench unchanged.

| PrismBench, WinUAE 030 | 8-bit | 16-bit | 24-bit |
|---|---|---|---|
| RectFill 100x100 | 3116/s | 3095/s | 1650/s (386 before row doubling) |
| Text JAM1 / JAM2, 40 chars | 149 / 169 | 156 / 130 | 127 / 77 |
| ClipBlit 200x100 | 142/s | 95/s | 63/s |
| ScrollRaster 400x100 | 2433/s | 2416/s | 935/s |
| Line ~390 px | 191/s | 188/s | 128/s |

## M8 as built (2026-10-03)

**VRAM paging** (`bitmap.c`). Any Prism bitmap lives in VRAM or fast RAM and moves under `lock`,
which every drawing path holds. `pbm_new(vram=TRUE)` takes VRAM if it fits after evicting, else fast
RAM. Allocating VRAM evicts the least recently shown bitmap that is neither on display nor locked by
a cybergraphics client (`LockBitMapTagList` counts, `UnLockBitMap` releases); showing a bitmap
(`show_pbm` in prismd.c) pages it back in first. Rendering, cgx and the blitter only ever see
`p->pix` / `p->inVram`, so a bitmap in fast RAM is drawn on the CPU and goes back to the blitter when
it returns. Caveat as with P96: a program that keeps `GetCyberMapAttr(DISPADR)` across a screen
switch without locking can be left with a stale address.

**Double buffering.** A displayable bitmap whose friend is a screen's bitmap, at the screen's size,
becomes one of that screen's buffers (`owner`), in VRAM if it fits. `ChangeVPBitMap` (LVO -0x3AE,
what `ChangeScreenBuffer` calls) is patched: on a Prism ViewPort it sets `RasInfo->BitMap`, records
the screen's front buffer, moves the display start (paging the buffer in if needed), waits for the
vertical blank and replies `dbi_DispMessage` and `dbi_SafeMessage`. A freed buffer that was on
display hands the display back to the screen's own bitmap.

**Screens are no longer CUSTOMBITMAP.** Explicit-ModeID screens used to be opened with `SA_BitMap`,
and Intuition refuses `AllocScreenBuffer(s, NULL, ...)` on those. They now open like Workbench: the
original OpenScreen allocates the bitmap and the AllocBitMap hook gives it a Prism one (`pending`);
the `SA_BitMap` path remains only for when another open is already being watched.

Verified in WinUAE (`PrismScreen MANY` / `DBUF`, `tools/m8tests.c`):
* four 1024x768 8-bit screens (3 MB) in 1984 KB of VRAM: each brought to the front twice, background
  and marker pixels right every time; the log shows the LRU swaps;
* 100 `ChangeScreenBuffer` flips, every DispMessage and SafeMessage delivered, the right buffer shown:
  35 fps at 1024x768 8-bit, 48.5 fps at 640x480 24-bit (vblank-bound);
* Workbench on PRISM 640x480 24bit (900 KB) + a double-buffered 1024x768 "game" (1.5 MB): Workbench
  paged out while the game ran, back intact afterwards (Clock kept drawing into it meanwhile).

## Speed pass (2026-10-03)

PrismBench got icon-sized tests (64x32 planar and friend blits, 8x8 fill, 4-char text) and a
pixel-exact check of planar blits (every pen, odd x offsets, distinct colours so 16/24-bit read-back
is exact). Measured against native AGA on the same emulated 030, then:

* **Planar images** (icons, gadget imagery) were the big gap - 10x slower than native. Planar to
  chunky now reads 32 pixels per plane as one long and transposes 8x8 bit blocks in three swap
  stages (5+ planes; a byte table for shallower images), and planar copies into 8-bit bitmaps convert
  straight into the destination row. 24/32-bit destinations cache each pen's pixel bytes per blit.
* **Text**: a glyph row up to 24 pixels wide is one unaligned long read, masked, shifted and ORed into
  the template (68020+); JAM2 rows at 16/24/32 bits use a nibble table (4 template bits -> 4 pixels as
  2/3/4 long writes), cached across rows and calls.
* **ClipBlit** inside one window with both rectangles in a single visible ClipRect (scrolling lists,
  text views) is one direct blit - the card's blitter in VRAM - instead of a temporary buffer.

| PrismBench (emulated 030) | native AGA | 8-bit before -> after | 16-bit before -> after | 24-bit before -> after |
|---|---|---|---|---|
| Blit planar 64x32 | 404/s | 39 -> 221 | 37 -> 149 | 30 -> 94 |
| Text JAM1, 40 chars | 329 | 152 -> 211 | 161 -> 236 | 130 -> 182 |
| Text JAM2, 40 chars | 327 | 175 -> 288 | 132 -> 262 | 72 -> 222 |
| Text JAM2, 4 chars | 1202 | 753 -> 868 | 830 -> 994 | 522 -> 883 |
| ClipBlit 200x100 | 53 | 141 -> 2890 | 95 -> 2888 | 63 -> 2872 |

Unchanged and already at or above native: fills (3100/s vs 145), ScrollRaster (2420 vs 30), lines,
WritePixel. Still slower than native: planar images (CPU conversion vs the Amiga blitter) and short
strings (per-call overhead: patch entry, layer lock, clipping). WinUAE does card blits instantly, so
blitter-bound numbers will be lower on the real A2000; CPU-bound ones should track the 030.

## Real hardware: the A2000's GBAPII++ (2026-10-03)

Driven over NetHarness with Picasso96 parked (its `DEVS:Monitors/GBA-PII++` moved to
`SYS:Storage/Monitors`); no monitor attached, so everything is checked by reading back. Details in
`docs/cirrus-picasso2-hw.md` §9. What it took:

* **Wake the chip** in `Picasso2_Probe` (ISA wake-up sequence) - a cold GD5434 ignores I/O.
* **ECS palettes.** On ECS graphics.library's ColorMap keeps 4 bits a gun, and Prism read colours back
  from it, so every Prism screen on the A2000 was a 4096-colour screen. PrismD now records the full
  32-bit values passing through SetRGB32 / LoadRGB32 / SetRGB32CM / ObtainPen (/ ObtainBestPen for a
  newly allocated pen) per screen, and uses them while the ColorMap still agrees in its top 4 bits.
  A WinUAE config with an ECS chipset (`prism p2 ecs`) reproduces the A2000's case.

PrismBench on the real A2000 (68030 @ 50 MHz), native = NTSC hires interlaced, 16 colours:

| | native ECS | Prism 8-bit | 16-bit | 24-bit |
|---|---|---|---|---|
| RectFill 100x100 | 166/s | 2524 | 1987 | 794 |
| Text JAM1 / JAM2, 40 chars | 295 / 292 | 262 / 290 | 276 / 217 | 195 / 169 |
| Text JAM2, 4 chars | 1009 | 1019 | 1014 | 853 |
| ClipBlit 200x100 | 61 | 1159 | 657 | 533 |
| ScrollRaster 400x100 | 34 | 710 | 380 | 230 |
| Line ~390 px | 219 | 221 | 231 | 146 |
| WritePixel | 3068 | 4356 | 4342 | 4259 |
| Blit planar 64x32 (icons) | 415 | 217 | 143 | 92 |

Real blitter numbers are lower than WinUAE's (which finishes card blits instantly) but still 15-21x
native for fills, copies and scrolls; text is level with native at 8 bits.

## Prism vs Picasso96 on the same card (A2000 GBAPII++, 2026-10-03)

Same machine, same PrismBench, 640x480; P96 = rtg.library 43.760 with its GBA-PII++ driver (modes
`$51041000` / `$51041100` / `$51041201`), Prism = this tree after the speed pass + ECS palette fix.

| calls/s | P96 8 | Prism 8 | P96 16 | Prism 16 | P96 24 | Prism 24 |
|---|---|---|---|---|---|---|
| RectFill 100x100 | 2598 | 2524 | 2108 | 1987 | 885 | 794 |
| Text JAM1, 40 chars | 1162 | 262 | 1107 | 276 | 394 | 195 |
| Text JAM2, 40 chars | 1163 | 290 | 1101 | 217 | 208 | 169 |
| Text JAM2, 4 chars | 2012 | 1019 | 1966 | 1014 | 1159 | 853 |
| ClipBlit 200x100 | 973 | 1159 | 600 | 657 | 385 | 533 |
| ScrollRaster 400x100 | 641 | 710 | 366 | 380 | 209 | 230 |
| Line ~390 px | 1465 | 221 | 1256 | 231 | 958 | 146 |
| WritePixel | 5253 | 4356 | 5186 | 4342 | 5106 | 4259 |
| Blit planar 64x32 | 295 | 217 | 156 | 143 | 107 | 92 |
| Blit friend 64x32 | 1848 | 731 | 1624 | 453 | 1397 | 330 |

Prism is level on fills and ahead on ClipBlit and ScrollRaster. The gaps, biggest first:
1. **Text ~4x** at 8/16 bits: P96 expands the glyph template with the Cirrus blitter (CPU-fed
   colour expansion). Prism expands on the CPU.
2. **Lines ~6x**: Prism plots every pixel through the draw-mode rule; P96 draws runs. Software fix.
3. **Friend blits 2.5-4x**: P96 keeps off-screen friend bitmaps in VRAM and blits them; Prism keeps
   them in fast RAM and copies over Zorro II with the CPU.
4. Per-call overhead (4-char text 2x, WritePixel 1.2x).

## Closing the gaps to Picasso96 (2026-10-03)

* **Text on the blitter.** `expandRect` (board API) = Cirrus system-to-screen colour expansion: the
  CPU streams the 1-bit template into the VRAM window (written into the scratch area so any surplus
  is harmless) and the BLT writes fg/bg pixels. The driver **calibrates itself** on the first mode
  set: it expands a known template into scratch VRAM with byte- and long-padded rows, reads it back,
  and only uses a scheme that comes out exact (short timeout + BLT reset if the chip is left waiting).
  Real GD5434 and WinUAE: rows padded to 1 byte. **Transparency (JAM1)** needs the key registers on
  silicon: background = ~fg and GR34/35 = that key (XFree86's trick); WinUAE just skips 0 bits.
  24-bit expansion is not attempted (Linux says the 543x can't; in WinUAE a refused 24-bit blit left
  the chip swallowing ordinary VRAM writes). Text pieces that are byte-aligned go to the chip
  straight from the template; clipped ones are repacked.
* **Fixed 8-pixel fonts** (topaz 8): the template is one byte per character per row straight from
  the font data, contiguous, streamed as plain longs - most of the text win.
* **Lines**: run-slice for mostly-horizontal lines (a row's run length by one division; pixel-exact
  against the old Bresenham, brute-forced over every slope up to 200x200), runs written as aligned
  words/longs (Zorro II is 16 bits wide); no VRAM read-back when the write mask is full.
* **Friend bitmaps in free VRAM** (never evicting), blitted to the screen with the new `copyBetween`
  (two pitches); VRAM allocator raised to 256 blocks.

PrismBench has pixel-exact checks for lines (12 slopes) and text (JAM1 over colour, JAM2,
INVERSVID) against graphics.library's own planar rendering; all pass on the real card at 8/16/24.

| A2000 GBAPII++, calls/s | P96 8 | **Prism 8** | P96 16 | **Prism 16** | P96 24 | **Prism 24** |
|---|---|---|---|---|---|---|
| RectFill 100x100 | 2598 | 2539 | 2108 | 2008 | 885 | 800 |
| Text JAM1, 40 chars | 1162 | 1152 | 1107 | 1154 | 394 | 232 |
| Text JAM2, 40 chars | 1163 | 1152 | 1101 | 1148 | 208 | 194 |
| Text JAM2, 4 chars | 2012 | **2204** | 1966 | **2172** | 1159 | 878 |
| ClipBlit 200x100 | 973 | **1165** | 600 | **655** | 385 | **533** |
| ScrollRaster 400x100 | 641 | **715** | 366 | **381** | 209 | **229** |
| Line ~390 px | 1465 | 1148 | 1256 | 1128 | 958 | 592 |
| WritePixel | 5253 | 4468 | 5186 | 4450 | 5106 | 4357 |
| Blit planar 64x32 | 295 | 217 | 156 | 141 | 107 | 92 |
| Blit friend 64x32 | 1848 | **2620** | 1624 | **2229** | 1397 | **2046** |

Level with P96 on text and fills at 8/16 bits, ahead on copies, scrolls, short strings and friend
blits. Still behind: lines (~1.1-1.6x), planar icons (~1.1-1.4x), 24-bit text JAM1 (1.7x, CPU),
WritePixel (~1.2x per-call overhead).

## Optimisation pass on real hardware (2026-10-03)

**The benchmark was lying.** PrismBench did soft-float time maths after every call; on the FPU-less
A2000 that cost ~115 us per iteration, more than most of the calls it timed, and squeezed every
ratio towards 1. It now reads the EClock once per 8 calls, integers only. Numbers from before this
section are not comparable with numbers after it. It also gained micro-tests (SetAPen alone,
WritePixel alone, LockLayerRom pair, lines by run count) and exactness checks for lines, text, fills
and the aligned icon path.

**What things cost on the A2000** (`tools/iotime`, TF536 68030 @ 50 MHz, Zorro II):

| | |
|---|---|
| one access to the card, register or VRAM, byte or word | ~0.8 us |
| VRAM long write / long read | 1.4 / 2.2 us |
| chip RAM long read | 1.7 us |
| ObtainSemaphore + ReleaseSemaphore | 6.5 us |
| LockLayerRom + UnlockLayerRom | 18 us |
| Forbid + Permit | 2.4 us |
| SetAPen | 12 us |

So a blit that made ~50 register accesses spent 40+ us before moving a pixel, and a single-pixel
call spent 24 us on two semaphores. What was done:

* **Blitter register traffic** (`drv_picasso2.c`): index+data as one 16-bit write; registers that
  already hold the value are skipped (pitches, mode, raster op, colours, key - *not* the address
  registers: WinUAE's chip counts in them during a blit); the start write leaves the index on GR31 so
  waiting is reads only; every operation returns with the blitter idle so none waits first. A fill
  went from ~47 accesses to ~10. Three more self-tests at the first mode set decide what is safe on
  the chip in front of us: word writes arrive (GD5434: yes), GR1/GR11 may stay set between blits
  without mangling CPU writes (5434: yes; 5426/28: no, so they are cleared there), pattern fills
  work with the source pitch left at 0 (both: yes).
* **Quick locking** (`draw_lock`): a short operation stays in `Forbid()` when neither the layer's
  semaphore nor Prism's is held by another task - same exclusion, 2.4 us instead of 24. Used for
  pixels, lines, small fills, text and small chunky blits (up to 8192 pixels); anything bigger, or
  contended, takes the semaphores as before.
* **Single pixels** find their ClipRect directly instead of going through the general walk.
* **Lines**: divisions once per line (run lengths are then dx/dy or dx/dy+1 by a running
  remainder), separate tight loops per pixel size when the line lies inside one piece, steep lines
  as a pointer walk; runs written as words/longs. All brute-forced against the original stepping.
* **Icons**: planar-to-chunky in generated assembly (`tools/gen_p2c.py` -> `src/p2c.S`; the
  generator simulates its own output against a reference before printing it), mask in a register,
  two rows parked in address registers; plane pointers set up once per blit.
* **Small fills** go to the blitter from 12 pixels (a blit is ~10 accesses now); CPU rows use
  aligned word/long writes.
* **Non-Prism calls leave early**: the patch stubs test the bitmap's flag byte and jump straight to
  the original function without saving registers. Native-screen WritePixel with PrismD loaded went
  from 8.0k to 10.4k calls/s.
* Tried and dropped: copying runs of set bits for 24-bit JAM1 text (slower even on the A2000).

**Prism vs Picasso96, same A2000, corrected benchmark** (calls/s; P96 = rtg.library 43.760):

| | P96 8 | **Prism 8** | P96 16 | **Prism 16** | P96 24 | **Prism 24** |
|---|---|---|---|---|---|---|
| RectFill 100x100 | 3902 | **4047** | 2865 | 2814 | 1001 | 972 |
| RectFill 8x8 | 5094 | **6201** | 4918 | **6404** | 4787 | 4203 |
| Text JAM1, 40 chars | 1343 | **1510** | 1280 | **1485** | 416 | 280 |
| Text JAM2, 40 chars | 1350 | **1459** | 1277 | **1463** | 214 | 201 |
| Text JAM2, 4 chars | 2678 | **3649** | 2619 | **3503** | 1354 | 1060 |
| ClipBlit 200x100 | 1107 | **1308** | 652 | **733** | 406 | **578** |
| ScrollRaster 400x100 | 698 | **789** | 383 | **411** | 213 | **240** |
| Line ~390 px | 1805 | **1917** | 1488 | **1911** | 1087 | **1248** |
| WritePixel (+SetAPen) | 14646 | 12813 | 14206 | 12668 | 13718 | 11995 |
| Blit planar 64x32 (icons) | 306 | **359** | 160 | **168** | 108 | 103 |
| Blit friend 64x32 | 2378 | **4668** | 2025 | **3568** | 1684 | **3116** |

At 8 and 16 bits Prism is ahead on everything but single pixels (about 10 us a call behind) and
level on large 16-bit fills. 24-bit is behind on text (no blitter text at 24 bits; P96 is faster
there by some means we haven't matched) and small fills. Run-to-run noise is about 5%.

## Installed on the A2000 (2026-10-03)

Prism now boots the A2000: `C:PrismD`, `DEVS:Monitors/Prism`, `SYS:Prefs/PrismPrefs`, tools in
`SYS:Prism/`, Workbench on PRISM:800x600 16bit. Picasso96's monitor file is parked in
`SYS:Storage/Monitors` (and its ScreenMode prefs kept as `ENVARC:Sys/screenmode.prefs.p96`).

* **Boot failsafe.** `DEVS:Monitors/Prism` creates `ENVARC:Prism.booting` before starting PrismD;
  PrismD deletes it after a minute (3600 vertical blanks). Found at boot, the launcher deletes it and
  skips Prism once, so a machine with nobody at the keyboard comes up native with its network.
* **32-bit on the GD5434** (the true-colour slot now tries each format the board has, per size:
  24-bit where the triple clock allows, 32-bit above that).
* **CoerceMode patched.** Intuition asks graphics which mode shows each screen on the front screen's
  monitor; for 800x600 graphics looked for a substitute, found none, and Intuition hid the screen's
  ViewPort (DHeight 0, VP_HIDE) - the picture still showed (Prism ignores the ViewPort) but the mouse
  limits were empty and the pointer sat at +-32767. Prism answers "itself" for its own screens.
  Things that were NOT the cause, each tested: Resolution ticks, EXTENDED_MODE / ID layout,
  MonitorSpec limits, PropertyFlags, raster limits, overscan sizes. `tools/prismdiag` prints the
  symptoms; Picasso96's values for the same mode are in the comment history (commit c55339b).
* The harness `REBOOT` hung this A2000 twice out of ~10 tonight (the known warm-reset lock); a
  power cycle always recovered it and Prism was never the cause.

## ZZ9000 on the A4000 (2026-10-03)

First run of `drv_zz9000.c` on the real card (firmware 2.8, TF4060 68060).
The driver was first checked against the v2.8.1 driver and firmware sources;
registers, mailbox layout and mode ids already matched. Changes:

* Prism's VRAM ends at $DF0000: the firmware writes captured native video
  into card memory from $DFF2F8 / $E00000, and Prism pages bitmaps anywhere.
* The pan address that shows native video is chosen like ZZ9000.card 2.13:
  config query (regs $E8/$EA, key 1) plus capability bit 6 (reg $E6).
* Borrowing the card from P96 (PrismTest): the mailbox header is saved and
  put back, because ZZ9000.card caches the bytes it wrote there. The test
  framebuffer sits at VRAM+$2000000 and is drawn before the mode is set.

Results: test patterns at 640x480 8-bit, 800x600 16-bit, 1024x768 32-bit and
1280x1024 16-bit looked right on a Dell P190S (digital input). Then P96 was
parked and the A4000 booted Prism: Workbench on $7A000102 (800x600 16-bit),
network and harness fine, failsafe marker cleared after a minute.
PrismBench exactness checks (planar blit, line, text, fill): 0 wrong at 8,
16 and 32-bit. So the big-endian 565 and BGRA32 paths are right.

No blitter and no hardware cursor yet (flags = 0), so there is no pointer
and everything is drawn by the 68060:

| 800x600           | 8-bit | 16-bit | 32-bit |
|-------------------|-------|--------|--------|
| RectFill 100x100 Kpix/s | 3444 | 1825 | 1669 |
| Text JAM1 chars/s | 42773 | 43606 | 41820 |
| ClipBlit Kpix/s   | 1957 | 1282 | 813 |
| ScrollRaster Kpix/s | 3216 | 1657 | 861 |
| WritePixel /s     | 76573 | 75012 | 65240 |

Next: the firmware blitter (mailbox OP_FILLRECT / OP_COPYRECT /
OP_RECT_TEMPLATE) and the sprite, then a comparison against P96.

Back to P96: move `DEVS:Monitors/Prism(+.info)` away, move
`SYS:Storage/Monitors/ZZ9000(+.info)` back, copy
`ENVARC:Sys/ScreenMode.prefs.p96` over `ScreenMode.prefs`, reboot.

### ZZ9000 blitter and cursor (2026-10-03, same day)

`drv_zz9000.c` now uses the firmware's mailbox commands on Zorro III:
OP_FILLRECT, OP_COPYRECT (overlap-safe), OP_COPYRECT_NOMASK (copyBetween, as
an 8-bit byte copy), OP_RECT_TEMPLATE (text; JAM1/JAM2, template staged at
board+$3210000, copied as longs) and the cursor (OP_SPRITE_CLUT_BITMAP,
OP_SPRITE_XY, register $48 for on/off; 32x48 pixels). Facts found:

* Commands are synchronous: the DMA_OP write returns with the blit done.
* The mailbox is one shared structure, so every command is built and sent
  under Forbid() (drawing tasks and the cursor would otherwise collide).
* Fill/copy pitches are in 32-bit words, the template op takes bytes.
* A colour's mailbox bytes are the pixel's VRAM bytes (16-bit: value << 16);
  8-bit takes the last byte.
* New flag PBF_BLIT_32: fills and text expansion at 4 bytes a pixel.

`PrismTest BLIT` (fills + overlapping copies against a CPU shadow) matched
at 8, 16 and 32-bit; PrismBench checks 0 wrong at all three depths.

| 800x600                 | 8-bit CPU | 8-bit | 16-bit | 32-bit |
|-------------------------|-----------|-------|--------|--------|
| RectFill 100x100 /s     | 344       | 23743 | 21301  | 17677  |
| RectFill 8x8 /s         | 30560     | 35515 | 35771  | 34833  |
| Text JAM1 40 chars /s   | 1069      | 5455  | 5446   | 5420   |
| ClipBlit 200x100 /s     | 98        | 8574  | 4566   | 2459   |
| ScrollRaster 400x100 /s | 80        | 11484 | 7066   | 4503   |
| Blit friend 64x32 /s    | 1457      | 28867 | 28974  | 27502  |

Still on the CPU: lines (OP_DRAWLINE exists), planar-to-chunky blits
(OP_P2C / OP_P2D exist), WritePixel. The pointer has not been seen by eye yet.

### Lines on the ZZ9000, and the native tie rule (2026-10-03)

New board hook `drawLine` (solid, unclipped, longer than 40 pixels):
OP_DRAWLINE with the error seed L/2. 800x600, ~390 px lines: 27,000 /s at
8 and 16-bit, 26,000 at 32-bit (CPU: 6,000; P96 on the same card: 18,000).

Adding lines whose long delta is exactly twice the short one to PrismBench's
line check showed that Prism's Bresenham (all three CPU paths, since M1)
rounded exact halves the other way from graphics.library: 358 wrong pixels.
The native rule is round(i * S / L) with halves rounded up, from the
starting end, in every direction. Fixed by working in doubled deltas with
the error moved half a unit (`line_solid` comment; brute-forced in Python
against the formula for all deltas up to 90). The check now also draws short
(CPU) and patterned tie lines: 0 wrong at 8, 16 and 32-bit.
The A2000's installed PrismD still has the old rule until it is updated.

P96 on the same A4000 / ZZ9000 (firmware 2.8, ZZ9000.card 2.13), 800x600
8-bit, per second - P96 / Prism: RectFill 100x100 19933 / 23743, RectFill
8x8 26148 / 35515, Text 40 chars 4894 / 5455, Text 4 chars 9844 / 16222,
ClipBlit 5493 / 8575, ScrollRaster 5520 / 11484, planar blit 241 / 1028,
friend blit 11178 / 28868, line 18134 / 27020, WritePixel 92941 / 75120.
(P96's 16-bit run reported wrong pixels in the planar, line and fill
checks; not investigated.)

### WritePixel and call overhead on the A4000 (2026-10-03)

`tools/zztime.c` on the TF4060: a ROM call costs ~0.55 us (Move), Forbid +
Permit 0.95 us, a semaphore pair 1.5 us, a fast-RAM read ~60-90 ns, a write
20 ns, a card write 0.5 us, a card read 0.8 us, chip RAM read 1 us.
PrismBench's own loop adds ~2 us a call (ReadEClock is 11 us, every 8 calls).
So calls and memory reads are what a pixel pays for. Changes:

* `f_WritePixel` + its own stub: the plain case (pen, full mask, visible
  part of the layer, locks free) runs with only its arguments saved.
* Every handled call returns by reading back d0/d1/a0/a1 instead of all 15
  registers (the C code preserves the rest).
* `QUICK_FORBID` / `QUICK_PERMIT`: the Forbid quick-lock without the two
  exec calls (the real pair only when a task switch is pending).

800x600 8-bit, per second, P96 / Prism before / after: WritePixel 92941 /
75120 / 118232; without SetAPen 120955 / 94229 / 168329; RectFill 8x8
26148 / 35515 / 37429. Checked in the emulator (Picasso II) first, then on
the A4000: all exactness checks 0 wrong.

### Mode test (2026-10-03)

PrismPrefs has a Test button (and `PrismPrefs TEST=<slot>` from a shell):
it shows the selected mode, as set in the window, for 10 seconds with a
test picture and a label, then returns; a requester offers to switch the
mode off if it didn't look right. PrismD does the work through two new
entries in the public semaphore (version 2: `testBegin`, `testEnd`): a
bitmap of its own, pinned in VRAM, and the card set straight to the mode -
so a mode that is off, or a rate that isn't the saved one, can be tried
without a restart. Any screen coming to the front ends it. Checked in the
emulator (picture + label, "can't show that mode" for 1280x1024 24-bit on
2 MB) and from the shell on the A4000. The button itself has not been
clicked by anyone yet.

No EDID: the ZZ9000 firmware (2.8.1) has no DDC code; whether the GBAPII++
wires the Cirrus DDC pins is unknown (to probe on the A2000).

### Installer (2026-10-04)

`dist/Install_Prism` is a script for the standard Installer; the old
Execute scripts are gone. `tools/prismsetup.c` (C:PrismSetup) does what the
Installer language can't: `DETECT` finds the card and the Picasso96 monitor
in DEVS:Monitors whose icon says `BOARDTYPE=` that card (ZZ9000; PicassoII,
PicassoII+, GBA-PII++), answering through ENV:PrismCard / PrismCardName /
PrismP96Mon; `WBMODE=<slot>` writes ENVARC:Sys/screenmode.prefs.
ENV:PrismFakeCard=1..4 is a test hook for machines without a card.

The script: checks OS 3.0+/68020+, offers Update/Remove when installed,
warns when no card (or a Zorro II ZZ9000) is found, copies the files, asks
before moving the P96 monitor to SYS:Storage/Monitors (name noted in
ENVARC:Prism.p96monitor), backs up the screen mode to
screenmode.prefs.before-prism and sets a Prism Workbench mode. Remove undoes
all of it. Click-tested on the KS 3.1 emulator box over NetHarness with a
simulated GBAPII++ and the A2000's real P96 monitor icon: install and remove
both left the right files. Found there: writing ENV:Sys/screenmode.prefs
makes IPrefs reset Workbench under the installer, so only ENVARC: is
written. Not tested: the no-card and Zorro II messages, Update, Expert
mode, OS 3.2's Installer, and a real boot after installing this way.

### Installer and PrismPrefs on the real A4000 (2026-10-04)

The A4000 was put back to plain Picasso96 (Prism deleted, ZZ9000 monitor
back, P96 screen mode) and `Install_Prism` run with OS 3.2's Installer,
clicked over NetHarness: it found "ZZ9000 (Zorro III)" and the P96 monitor
"ZZ9000", moved it, set 800x600 16-bit; after the reboot Prism was up on
$7A000102 with all PrismBench checks at 0. Update (run again) also works;
it no longer repeats the welcome and Workbench pages. Remove was tested in
the emulator only. Screenshots: `a4000-installer.png`,
`a4000-prismprefs.png` (both grabbed from the Prism screen).

Found on the way: relative mouse movement was at half speed on Prism
screens (MonitorInfo.MouseTicks was 1,1; now 22,22 = one pixel a count, as
P96 reports). NetHarness MOVETO 100,100 / 400,300 / 700,500 now lands
exactly. PrismPrefs' Test button and its requester were clicked on the
A4000: the test ran, "No" switched the mode off in the list.

The mode test pictures (800x600 8-bit, 1024x768 16-bit, 1280x1024 32-bit,
640x480 16-bit) were seen on the A4000's monitor by Laine on 2026-10-04: all good.

### A2000 re-measured with the current build (2026-10-04)

GBAPII++ / TF536, 640x480, calls/s. "P96" and "Prism 10-03" are the table
above (measured 2026-10-03 18:21); "now" is the build with the WritePixel
short cut, the cheaper return and the line tie fix. All PrismBench checks
0 wrong at 640x480 and 800x600, 8/16/24/32-bit.

| | P96 8 | Prism 10-03 | **now** | P96 16 | Prism 10-03 | **now** | P96 24 | Prism 10-03 | **now** |
|---|---|---|---|---|---|---|---|---|---|
| RectFill 100x100 | 3902 | 4047 | **3987** | 2865 | 2814 | **2807** | 1001 | 972 | **786** |
| RectFill 8x8 | 5094 | 6201 | **6765** | 4918 | 6404 | **6475** | 4787 | 4203 | **4209** |
| Text JAM1 40 | 1343 | 1510 | **1516** | 1280 | 1485 | **1478** | 416 | 280 | **279** |
| Text JAM2 4 | 2678 | 3649 | **3665** | 2619 | 3503 | **3470** | 1354 | 1060 | **1062** |
| ClipBlit 200x100 | 1107 | 1308 | **1408** | 652 | 733 | **730** | 406 | 578 | **430** |
| ScrollRaster 400x100 | 698 | 789 | **819** | 383 | 411 | **409** | 213 | 240 | **188** |
| Line ~390 px | 1805 | 1917 | **2007** | 1488 | 1911 | **1945** | 1087 | 1248 | **1245** |
| WritePixel (+SetAPen) | 14646 | 12813 | **23480** | 14206 | 12668 | **22773** | 13718 | 11995 | **19158** |
| Blit planar 64x32 | 306 | 359 | **361** | 160 | 168 | **173** | 108 | 103 | **103** |
| Blit friend 64x32 | 2378 | 4668 | **4125** | 2025 | 3568 | **3249** | 1684 | 3116 | **2414** |

* WritePixel is now well ahead of P96 at every depth.
* The 24-bit blitter numbers (fill, ClipBlit, scroll, friend) dropped about
  20% since the 10-03 table. That table was measured BEFORE the 19:30 fix
  that runs 24-bit at three times the pixel clock (the monitor had called
  the old timing wrong): the display now takes more of the chip's memory
  cycles and the blitter gets fewer. So the old 24-bit Prism column was
  flattered by a wrong video mode; against P96 the honest 24-bit picture is
  ahead on ClipBlit, lines, friend blits and pixels, behind on large fills
  (786 vs 1001), scrolling (188 vs 213), text and small fills.
* Friend blits at 8 and 16-bit are about 10% down on the old table (still
  1.6-1.7x P96). Not explained yet; not a clock effect.
* At 800x600 the same build gives lower blitter figures than at 640x480
  (16-bit fill 2490, ClipBlit 624), for the same display-bandwidth reason.

### True colour on the GD5434: 32-bit with its blitter (2026-10-04)

The 5434's BLT can fill and colour-expand 4-byte pixels (GR30 bits 5:4 =
11; colour bytes 2 and 3 in GR13/GR15 and GR12/GR14), which it cannot do at
24 bits. `p2_CalibrateExpand` now tries, on the real chip at the first mode
set, a 32-bit fill, opaque expansion and transparent expansion (the colour
key is 16 bits wide; it is given the pixel's first two bytes) and reads
them back. On the A2000 all three pass, so `PBF_BLIT_32` is set and JAM1
and JAM2 text and fills run on the blitter. `PBF_PREFER_32` makes the
true-colour slot choose BGRA32 before BGR24 on this chip: the modes are
named "32bit", run at the plain pixel clock (24-bit needs 3x), and 800x600
true colour exists at all. Planar blits into 32-bit write one long a pixel
straight to the bitmap. WinUAE has no 5434, so none of this runs there.

A2000, 640x480, calls/s: P96 "24" (10-03 table) / Prism 24-bit / Prism 32-bit

| | P96 24 | Prism 24 | **Prism 32** |
|---|---|---|---|
| RectFill 100x100 | 1001 | 786 | **1485** |
| RectFill 8x8 | 4787 | 4209 | **6129** |
| Text JAM1 40 chars | 416 | 279 | **1307** |
| Text JAM2 40 chars | 214 | 201 | **1296** |
| Text JAM2 4 chars | 1354 | 1062 | **3241** |
| ClipBlit 200x100 | 406 | 430 | 304 |
| ScrollRaster 400x100 | 213 | 188 | 162 |
| Line ~390 px | 1087 | 1245 | 1055 |
| WritePixel (+SetAPen) | 13718 | 19158 | **18730** |
| Blit planar 64x32 | 108 | 103 | **120** |
| Blit friend 64x32 | 1684 | 2414 | **1946** |

Copies and scrolling move a third more bytes at 32 bits and the chip's copy
rate is the limit (about 24 MB/s in either format), so those two are behind
P96's figures - which were taken in P96's "24" mode; P96 has not been
measured at 32 bits on this machine. All checks 0 wrong at 640x480 and
800x600.

The A2000's harness REBOOT hung twice in a row this night (no ping; power
cycle needed), once with the older build running: it is the machine's
reboot lock-up, not Prism. After each power cycle Prism started normally.

### 24-bit and 32-bit are separate choices (2026-10-04)

`prefs_depths` is now 8, 16, 24, 32: a fourth depth slot, ModeIDs
$7A0003xx. Slot 24 offers only 3-byte formats, slot 32 only 4-byte ones
(`board_format`), and `PBF_PREFER_32` is gone. So PrismPrefs and ScreenMode
list "24bit" and "32bit" modes side by side where a card has both (GD5434:
24-bit at 640x400/640x480, 32-bit up to 800x600), and the user picks fast
copies (24) or blitter text and fills (32). Picasso II: 24 only. ZZ9000: 32
only - its true-colour ModeIDs moved from $7A0002xx to $7A0003xx. The prefs
file gains MODE=...x32 lines; older files load (missing lines = on).

### One unexplained crash on the A2000 (2026-10-04, ~01:50)

After a power cycle with the first 24/32-slot build, the A2000 answered the
harness (VERSION, prismdiag on the Prism Workbench), then stopped during
`PrismBench LIST` and came back by itself on a native Workbench with
`ENVARC:Prism.booting` consumed: the failsafe had skipped Prism, so the
previous start died inside its first minute. Not reproduced: PrismD started
by hand ran LIST, the benchmarks and mode tests at 24 and 32 bits cleanly;
the emulator ran LIST four times straight after a Prism boot; a warm reboot
of the A2000 into the same build was stable (marker cleared, LIST twice).
The stale `DEVS:Monitors/Prism` launcher (built before the prefs struct
grew) is not the cause - it only reads the file - but was updated anyway.
A soak test (`SYS:soak.log` on the A2000: PrismBench checks at 8/16/24/32
bits plus a 1024x768 mode test, 40 rounds) was left running overnight.

Second time, 2026-10-04 ~12:26: again the first boot after a new PrismD was
copied in and the machine reset by the harness. It answered VERSION and
SCREENS on the Prism Workbench, then reset during the next EXEC and came
back native (marker consumed). `lastalert` (tools/) showed exec's LastAlert
empty: a reset, not a guru. Common to both: a fresh binary, a harness
reboot seconds after the copy, and the death inside PrismD's first minute.
Still open.

### cybergraphics mode requester

`CModeRequestTagList` was a stub that returned 0; SysSpeed's screen mode
buttons did nothing. It is now asl.library's screen mode requester with a
filter hook that lets through only Prism modes inside the caller's
CYBRMREQ depth / size / colour-model limits (`tools/cmreq.c` tests it).

### More Cirrus boards, emulation only (2026-10-04)

`drv_picasso2.c` now has a board table: Picasso II (2167/11+12), GBAPII++
(2167/16+17), Piccolo SD64 (2195/10+11, GD5434), Piccolo (2195/5+6, GD5426)
and Spectrum 28/24 (2193/1+2, GD5428), all Zorro II. Facts from NetBSD's
grf_cl and Linux's cirrusfb (read for facts; no code taken): registers at
port offsets on the register board; DAC data at $3C9 direct (only the
Picasso II needs the +$FFF alias); the monitor switch is a control byte at
+$8000 ($6F card / $4F Amiga / $1F wake on the SD64 and Spectrum; bit 5,
wake bit 4 on the Piccolo); aperture nibble found by test as before.

Red and blue are exchanged on these three boards, so their direct-colour
formats are new `PF_BGR565LE` / `PF_BGR555LE`, `PF_RGB24` and, on the SD64,
`PF_ARGB32` (bytes x,R,G,B - found from WinUAE's picture; RGBA32 came out
wrong). For the 256-colour palette the sources disagree (NetBSD: B,G,R on
all three; Linux: B,G,R on Piccolo and Spectrum, R,G,B on the SD64; WinUAE
shows right colours with R,G,B on all three), so R,G,B is the default and
`PALETTE=BGR` in Prism.prefs switches it.

WinUAE configs `prism sd64`, `prism piccolo`, `prism spectrum` (copies of
`prism p2` with another gfxcard_type, same DH0). On each: PrismBench checks
0 wrong at every depth the board has; test pictures right at 8, 16, 24 and
(SD64) 32 bits; Workbench on a 16-bit Prism screen with icons and pointer
on the SD64. The emulated GD5434 passes the 32-bit blitter self-test, so
that code now runs in emulation too. Open: `PrismTest BLIT` at 15/16 bits
on the emulated SD64 gets one overlapping right-shift copy wrong (rows
after the first); the same test passes on the real GD5434 (GBAPII++) and on
the emulated GD5428, so it is taken to be the emulation. Zorro III
versions of these boards, 4 MB on the SD64, and every real card: untested.

### 8-bit JAM1 text after 16-bit text (found by the soak test)

The A2000 soak (`SYS:soak.log`) showed `text check: 304 wrong` in the 8-bit
benchmark of every round after the first: always the benchmark that
followed the 1024x768 16-bit mode test. Reproduced by hand: after any
16-bit blitter text, 8-bit transparent expansion dropped foreground pixels
(native pen 5, screen 3). The 16-bit text leaves the colours' second bytes
in GR11/GR10; the 8-bit path only wrote GR1/GR0. `blt_fg`/`blt_bg` now
zero the second byte at 8 bits (the state the calibration proved). WinUAE
does not show this.

Verified on the A2000 (2026-10-04 morning, after a power cycle): with the
fixed PrismD the 16-bit mode test followed by the 8-bit benchmark gives
text check 0 wrong (it was 304), twice, with a 16-bit benchmark between.

### Real applications on the A2000; low-resolution modes (2026-10-04)

`tools/prismgrab.c` saves a Prism screen's real framebuffer as a PPM (the
harness screenshot reads pens, so true-colour content looks like grey mush
in it).

* MultiView on the 16-bit Prism Workbench shows a PNG in true colour
  (`a2000-multiview-16bit.png`). A JPEG made with PIL gave "Unknown data
  type" - not looked into.
* ADoom 1.4.1 (`-rtg -directcgx`, shareware doom1.wad) runs: its ASL
  screen-mode requester lists the Prism modes. At first Prism had nothing
  under 640x400; Doom got 640x400, drew as if the screen were 320 bytes
  wide (it ignores the bitmap's pitch) and the picture came out twice, half
  height. So `prefs_sizes` gained 320x200 and 320x240 (size slots 6 and 7;
  8 sizes x 4 depths = 32 slots, ModeIDs unchanged for the old sizes).
  Cirrus: the 640x400/640x480 timing at half the pixel clock (12.588 MHz;
  the VCO runs below its documented minimum, as Linux's cirrusfb lets it)
  with CR9 bit 7 doubling the lines. SR1 bit 3 (dot clock / 2) was tried
  first: WinUAE ignores it in packed modes. ZZ9000: its driver already maps
  sizes under 640x480 to the firmware's 2x scaling (untested there).
  With the 320x200 mode Doom's framebuffer is right
  (`a2000-doom-320x200.png`). The timed demo runs at about 4 frames/s on
  the 68030 (2146 frames in ~9.5 min; LockBitMap 318 us a frame): the CPU,
  not Prism.

### IBrowse: what bitmaps say about themselves (2026-10-04)

IBrowse 3.0 (MUI) on a 16-bit Prism Workbench, first on the A2000, then
reproduced in WinUAE: toolbar images shredded with magenta, the photo
dithered to pens. Native AGA showed the toolbar clean, so it was Prism.
Two answers from `GetBitMapAttr` were wrong for programs:

* `BMA_DEPTH` said 8 for a 16/24/32-bit bitmap (its pen count). MUI and
  IBrowse choose their palette or true-colour path on it. It now says 15,
  16, 24 or 32, like a Picasso96/CyberGraphX bitmap. `bm->Depth` and the
  screen's pens are unchanged. With that the photo is drawn in true colour.
* `BMA_WIDTH` said the exact width. graphics.library says BytesPerRow * 8
  (the width rounded up to 16). IBrowse sizes the mask plane of its 168
  pixel toolbar strip from it: 21 bytes a row instead of the 22 that
  BltMaskBitMapRastPort steps by (found by logging the mask rows: the
  pattern slid one byte a row). Now BytesPerRow * 8.

After both: toolbar and page are right in WinUAE (`emu-ibrowse-16bit.png`),
PrismBench checks still 0. The debug log (`PrismD LOG`) now also lists
BltBitMap / BltBitMapRastPort / BltMaskBitMapRastPort calls and friend
AllocBitMap calls. On the A2000 the page text also had lighter boxes
behind the glyphs, which WinUAE does not show - to recheck there with this
build.

### JAM1 text on the real GD5434: the chip's transparency depends on the colour (2026-10-04)

IBrowse's page text on the A2000 (16-bit) had stripes of a second colour
behind the letters; WinUAE did not. `PrismTest XTEST` (new: transparent
expansion of a small template into scratch VRAM, read back, for 13 colours)
on the real chip:

* 8-bit: right only for colours below $80 (any pen from 128 up fails).
* 16-bit: right only when both bytes of the pixel are below $80.
* 32-bit: right for every colour.
* Nine ways of choosing the key colour at 16 bits changed which pixels were
  wrong, none made the failing colours right. GR34/GR35 read back 0.

The calibration's test colours ($11, $1234) and PrismBench's pens were all
in the range that works. Whatever the chip really compares, an opaque
expansion is right for every colour, and so are raster operations on it:
`p2_ExpandRect` now draws JAM1 in two opaque passes when a colour byte is
$80 or more at 8/16 bits (or when the chip's way failed its self-test):
all-ones foreground with ROP $50 (dst AND NOT src), then the colour with
ROP $6D (OR). XTEST: all 13 colours right at 8, 16 and 32 bits. PrismBench's
text check now draws JAM1 at four x alignments in pens 2, 200, 255 and 31:
0 wrong at 8, 16 and 32 bits on the A2000, and IBrowse's page is clean
(`a2000-ibrowse-16bit.png`). The two-pass text costs a second template
stream; the 40-character benchmark (a pen that takes one pass) is unchanged.

## SysSpeed 2.6 on the A2000 (2026-10-04)

The third application test (after MultiView and ADoom, IBrowse). What it
turned up, in order:

* **Mode requesters.** SysSpeed picks its test modes with reqtools.library
  (not installed on the A2000; a copy went into its own `libs` drawer).
  reqtools filters on `DimensionInfo.MaxDepth`, and Prism said 8 for every
  mode, so the Hi-colour and True-colour lists were empty. The display
  database now reports the real depth (16/24/32, as P96 does); screens
  still open with at most 8 planes of pens (`maxDepth` vs `dimDepth` in
  `struct ModeRec`). cybergraphics' own `CModeRequestTagList` was a stub
  and is now an ASL screen mode requester filtered to Prism modes.
* **8-bit JAM1 text striped after 32-bit use (GD5434).** After a 32-bit
  screen had drawn text, 8-bit transparent expansion showed the key colour
  in every fourth pixel column: colour bytes 2 and 3 (GR12-GR15) still held
  the 32-bit pixel. 8- and 16-bit operations now zero them (`blt_hi0`).
  PrismBench's text check caught it (560 wrong of 16000 -> 0).
* **Area fills at a fifth of the native chipset's speed.** AreaEnd sends
  BltPattern a mask; Prism drew it pixel by pixel. A mask with no area
  pattern (JAM1/JAM2) now goes through the template path. PrismBench
  "Area ellipse + triangle" on the A2000: 2.3/s -> 33.6/s, exact against
  graphics.library's own fill (new area check).
* **The "hang" in the Intuition test.** On a true-colour screen the
  machine stopped answering the network during the window tests. Not a
  crash and not a deadlock: layers clears what a closing window uncovers
  with BltBitMap minterm 0, and into 24/32-bit that took the general
  path - both bitmaps read back as pens with a palette search per pixel,
  seconds per window - at SysSpeed's priority 1, which starved the TCP
  stack and the harness. Clear (0x00) and set (0xF0) are now a fill
  (blitter where there is one), and the colour-to-pen lookup remembers its
  last answer. `tools/wintest.c` (LOOP=n) reproduces it: 1000 open/close
  at 24-bit never finished before, takes seconds now.
* **Alerts.** Looking for the hang showed a real gap: a guru is drawn on
  the native display, which nobody sees while the card's video switch is
  on Prism. DisplayAlert and TimedDisplayAlert are patched: the switch
  goes to the Amiga's video for the alert and back after. With LOG=ON the
  alert is logged and skipped instead (unattended test machines).

Debugging tools added: `lockwatch` (who holds Prism's lock, which tasks
wait or are ready - written to a file every 2 s), `wintest` (window
operations step by step, last line of its log = the step that died),
`lastalert`, `cmreq`. The emulated box now carries NetHarness on port
7810 (`Prism/m9ss`), so GUI programs can be clicked there too.

### SysSpeed results on the A2000 (GBAPII++, 68030, after the fixes)

Modes: 8-bit and Hi-colour 640x480, True-colour 640x480 32-bit; Workbench
on 800x600 16-bit. Full list in `docs/sysspeed-a2000.png`. Both test
groups run to the end at all four depths and PrismBench's checks are 0
wrong afterwards.

| Test | 16 col | 256 col | Hi | True |
|---|---|---|---|---|
| RectFill | 4239 | 4260 | 3284 | 1913 |
| PrintTxt | 1512 | 1511 | 1487 | 1325 |
| WrtPixel | 21139 | 20964 | 20185 | 17670 |
| HorLines | 5648 | 5655 | 5323 | 4654 |
| ScrollX | 127 | 127 | 56 | 21 |
| AreaFill | 276 | 276 | 205 | 274 |
| AreaCir | 136 | 136 | 122 | 135 |
| OpenWin | 17 | 17 | 15 | 12 |
| MoveWin | 354 | 295 | 160 | 131 |
| OpenScr | 6 | 1 | 1 | 0 |
| SwapScr | 60 | 15 | 15 | 1 |

Before the area-fill change AreaFill was 17 (the native ECS screen: 95).
Weak spots left: opening a screen (OpenScr) and swapping true-colour
screens (SwapScr 1/s) - a 640x480 32-bit screen is 1.2 MB and Workbench
960 KB, so on a 2 MB card every swap pages a screen out to fast RAM and
the other back in over Zorro II. No Picasso96 numbers to compare with yet
(P96 is parked on this machine).

## More RTG software on the emulated box (2026-10-04)

Run on `prism p2` (emulated Picasso II+, now with a 68882) through the
network harness, Workbench on PRISM:800x600 16-bit. Programs came from
the AmiKit tree already on the PC; their libraries (minus anything
Picasso96/CyberGraphX) sit in `DH0:Apps/AKLibs`.

| Program | Result |
|---|---|
| MysticView 1.07 (guigfx/render) | true-colour PNG, scaled, correct (`docs/app-mysticview.png`) |
| Personal Paint 7 | lists Prism modes in its own requester; 640x480 256 colours: toolbox, palette, requesters, drawing all work - after the SetRast fix below (`docs/app-personalpaint.png`) |
| WBsteroids | true-colour intro and scroller in a Workbench window (`docs/app-wbsteroids.png`) |
| Directory Opus 4 | own 800x600 16-bit screen next to Workbench (paging), listers, icon requester, clean quit (`docs/app-dopus4.png`) |
| NetSurf 68k | own 16-bit screen, anti-aliased UI, local page with a PNG (`docs/app-netsurf.png`) |
| PerfectPaint 2.93 | does NOT run - also not with Picasso96 (see "PerfectPaint: not PrismRTG") |
| ChaosPro, ScummVM RTG, AmiJeweled | not testable here (font from AmiKit's prefs missing; 68060-only build; Workbench start only) |

**SetRast bug (fixed).** `SetRast` on a window's RastPort filled the whole
bitmap; graphics.library clears only that layer. Personal Paint clears its
title strip that way, which wiped its toolbox, canvas and every requester
(the screen was one pen, 182, edge to edge). Layered RastPorts now go
through the clipper.

**PerfectPaint (open).** Two things. (1) It refuses to start unless a file
`LIBS:cgxsystem.library` or `LIBS:Picasso96API.library` exists ("Can't
find Cybergraphics or Picasso96") - a file check, not an OpenLibrary.
SysSpeed looks at the same names to label the system. Prism ships neither.
(2) With a placeholder file it shows its launcher and mode requester
(only PRISM:800x600 16bit offered), then after "Go" allocates four
bitmaps with BMF_SPECIALFMT (48x48 RGB24, LUT8 48x48, 480x48, 48x48) and
the machine stops dead - no alert, nothing further in the log even with
LOG=SYNC. Pointing those bitmaps' Planes[0]/BytesPerRow at the pixels (as
P96 memory bitmaps do) did not change it; reverted. Next step would be a
Picasso96API.library of our own, or tracing what it calls after the
allocations.

Debugging aids added on the way: `LOG=SYNC` (PrismD writes each log line
at once, at priority 10 - survives a freeze), `prismlog ON|OFF` (switch
the log on a running PrismD), `prismgrab PENS` (pen numbers and palette
instead of colours), a `PRISM_TRACE` build flag (every drawing call).

## Prism's own Picasso96API.library (2026-10-04)

Laine's call after PerfectPaint: write our own. `src/p96.c` (inside PrismD,
added to the system in memory like cybergraphics.library), `src/p96api.h`
(the interface's published numbers, written out for this implementation -
function order from the public FD file, tag and format values checked
against a published header), `tools/p96stub.S` (the file the installer
puts in LIBS: - a resident tag whose init returns 0, for programs that
only look for the file and for a machine where PrismD isn't running),
`tools/p96test.c` (22 checks: every call, results read back).

All 28 calls of the public table plus GetRTGData / GetBoardData /
EncodeColor: bitmaps (AllocBitMap in any format through Prism's
pixel-format bitmaps, GetBitMapAttr, Lock/Unlock), modes (BestModeID,
ASL-based RequestModeID, mode list, GetModeIDAttr), OpenScreen (P96SA_
tags mapped onto Intuition's), Write/ReadPixelArray for every direct
colour format and CLUT, WritePixel/ReadPixel/RectFill (on the blitter
where the board has one), Write/ReadTrueColorData, board data. PIP windows
answer PIPERR_NOTAVAILABLE. p96test: 0 of 22 failed at 8, 16 and 24 bits
on the emulated Picasso II+.

The installer puts a larger file of that name (Picasso96's own) aside as
`Picasso96API.library.before-prism` and brings it back on Remove.

**Pixel-format bitmaps expose their pixels.** A bitmap allocated with a
pixel format (CyberGraphX's BMF_SPECIALFMT, or p96AllocBitMap) now has
Planes[0] = the pixel memory and BytesPerRow = its row size, as programs
written for the other two systems expect (`PBitMap.direct`); such bitmaps
never go into VRAM. PerfectPaint builds its own BitMap/RastPort around
them.

**exec Alert patched.** Gurus are drawn by exec itself, not through
Intuition's DisplayAlert, so the earlier alert patch never saw them. Alert
(-108) now logs the number and task and switches the card to native video.

**PerfectPaint, still open.** With the library file present it starts,
shows its launcher and requester. After "Go": four pixel-format bitmaps,
InitBitMap/InitRastPort, a 48x48 pattern RectFill (correct, bytes read
back c8 c8...), then no further graphics call at all - the dumped code
after the call sets AreaPtrn back, dimensions two arrays and runs a
256-step loop of maths library calls - and the whole machine stops (even
a priority-20 exec/dos-only task). No Alert() call is logged. Not found
yet whether that is Prism's doing.

More debug aids: `prismlog SYNC`, a PRISM_TRACE build that logs every
graphics.library vector ("g-<offset>") and can dump a caller's code.

On the A2000 (GBAPII++, real GD5434), same day: `p96test` 0 of 22 failed at
8, 16 and 24 bits, PrismBench checks 0 wrong. Picasso96's own library file
is still in that machine's LIBS: (2.490); Prism's in-memory 2.500 is what
OpenLibrary returns. `tools/trapwatch.c` (catches a task's CPU exception
into a file) caught nothing from PerfectPaint: its stop is not a trap in a
task.

## Loose ends, 2026-10-04 evening

* **JPEG in MultiView on the A2000** ("Unknown data type"): not Prism. A
  `DEVS:DataTypes/JFIF` descriptor (class `jfif.datatype`, not installed)
  claimed JPEG files ahead of OS 3.2's own JPEG descriptor. Moved to
  `SYS:Storage/DataTypes`; after a reboot MultiView shows the JPEG in true
  colour on the 16-bit Workbench.
* **p96test on a 32-bit screen** (`DEPTH=32 FORMAT=9`, new FORMAT option
  -> P96SA_RGBFormat): 0 of 22 failed on the A2000.
* **Slow screen opening.** `wintest SCREENS=n` (open, set 256 colours one
  at a time, close; phases timed). Every single-colour change did the
  whole palette: tables for 256 pens, 256 DAC entries, the pointer. Three
  paths, all now one entry at a time (`palette_one`, `cm_one`):
  SetRGB32/SetRGB4, the ColorMap calls (SetRGB32CM, ObtainPen,
  ObtainBestPen - picture viewers allocating pens on 256-colour screens),
  and LoadRGB32 with up to 16 colours - which is what graphics.library's
  own SetRGB32 calls internally (found with the PRISM_TRACE vector log:
  each SetRGB32 = one LoadRGB32 + GetRGB32s). Emulator, 3 screens: 11.3 s
  -> 2.5 s (the 256 colours: 481 -> 39 ticks). A2000 before: 5 screens in
  29.9 s (8-bit), 24.4 s (16-bit), 32.6 s (32-bit); with only the first
  path fixed 16.5 / 14.4 / 23.1 s. Final A2000 figure still to measure -
  the harness reboot after installing this build hung (the 7th reboot
  with the 20 s wait; the first to hang).

A2000 after a power cycle (the build was fine: booted Prism, settled, no
alert recorded), `wintest SCREENS=5`, before -> after the palette work:
8-bit 29.9 s -> 2.3 s, 16-bit 24.4 s -> 2.4 s, 32-bit 32.6 s -> 12.8 s.
PrismBench checks 0 wrong, p96test 0 of 22 at 8 and 32 bits.

What is left at 32 bits is VRAM paging, not colours: per screen 1.3 s to
open and 1.0 s to close (8/16-bit: 0.2 s and 0.03 s). A 640x480 32-bit
screen (1.2 MB) and the 800x600 16-bit Workbench (960 KB) don't fit in
2 MB together, so opening copies Workbench out to fast RAM and closing
copies it back - about 1 MB each way over Zorro II, which is the bus's
speed, not something the code can shorten. The same screen in 24-bit
(921 KB) fits beside Workbench and opens like the 8/16-bit ones.

## Planar bitmaps in fast RAM (2026-10-04, found by Laine opening Prefs)

Laine, with a real mouse on the A2000: the Prefs drawer showed labels but
no icons, and changing the pointer in Pointer prefs ("Use") crashed the
machine. The A2000 runs PeterK's icon.library 51.4 (true-colour build).
It keeps its work bitmaps - planar, depth 8 - in fast RAM when it finds
cybergraphics.library, and builds a classic icon's mask there with
Move/Draw/ReadPixel, BltPattern and BltBitMap. Prism passed every call on a
non-Prism bitmap to the ROM, whose blitter cannot reach fast RAM: the mask
stayed empty (BltMaskBitMapRastPort then drew nothing - the PRISM_TRACE
dump showed mask rows of zeros) and the blitter wrote into chip RAM at the
same address modulo 2 MB instead. That is memory corruption on every
classic icon drawn, and the likely cause of the pointer crash and of the
unexplained resets soon after a boot (Workbench draws its icons then);
both never left an alert. Colour icons were fine: those go through
cybergraphics' pixel array calls.

Prism now draws into such bitmaps itself (`render.c`, "planar bitmaps
outside chip RAM"): the stubs send a RastPort whose bitmap's first plane
lies above `SysBase->MaxLocMem` to the handlers, which run on a chunky
copy (rows converted in as the clipper reaches them, written back to the
planes afterwards); BltBitMap between two bitmaps takes the CPU path when
either side is out of the blitter's reach. Reproduced in the emulator by
putting the A2000's icon.library into the test box (kept as
`IconTest/icon.library-51.4-from-A2000`; the box runs the OS one again),
fixed there, then on the A2000: `docs/a2000-prefs-icons.png`.

Not covered: Flood, DrawEllipse, BitMapScale and BltClear on such
bitmaps (unpatched ROM calls), and BltBitMapRastPort *from* one onto a
native screen.

## The A4000 with today's build: Picasso96 was still running (2026-10-04 night)

Updating the A4000 from the 00:36 build to today's made it freeze the
moment Workbench drew its first icon (grey Prism screen, no network later
than that; once an 8100000F and once an 80000003 alert after the reset).
What it was, and how it was found:

* The machine still loaded Picasso96 at every boot: `DEVS:Monitors/Native`
  and `Generic` (BOARDTYPE=Native / Generic) start `rtg.library` - a
  'Picasso96' task was in every boot's task list - although the ZZ9000
  monitor itself had been parked by the installer. With it,
  `ENVARC:Picasso96/PlanesToFast`: Picasso96 puts planar bitmaps in fast
  RAM and draws into them with its own patches. Until today Prism passed
  such calls down the chain, to Picasso96, and the two coexisted.
* Today's "planar bitmaps outside chip RAM" handling takes those calls
  itself. With Picasso96 underneath, that is Prism writing into
  Picasso96's bitmaps as if they were plain planar ones.
* PrismD now looks for `rtg.library` (at start and while running) and, if
  it is there, leaves every such bitmap alone and does not add its own
  Picasso96API.library. **That was not enough** - the A4000 still froze
  with that build. Not understood; not pursued, because Laine had every
  Picasso96 piece moved out (`SYS:Storage/Picasso96-parked`: the two
  monitors, LIBS:Picasso96, Picasso96API.library, P96Prefs, the settings,
  ENVARC:Picasso96, the drawer; the assign in User-Startup commented out).
  Without Picasso96 the same build boots: Workbench 800x600 16-bit with
  every icon (`docs/a4000-workbench-today.png`), p96test 0 of 22 at 16 and
  32 bits, Prism's own Picasso96API.library 2.500.
* **So: Prism and Picasso96 must not both be installed.** The installer
  only looks for a Picasso96 monitor for the card it found; it needs to
  find *any* Picasso96 monitor (Native, Generic, uaegfx ...) and
  `LIBS:Picasso96/rtg.library`, and say so. To do.

How it was found, for next time: `LOG=SYNC` writes PrismD's log to
`SYS:PrismD.log` (appended, closed and the disk flushed per line, start-up
stages included) and never marks the start as good, so after a frozen boot
one plain reset comes back without Prism and with the log; `lockwatch`
and `trapwatch` started from the Startup-Sequence (both flush to disk;
lockwatch names each Shell's command); a PRISM_TRACE build names every
handler entered. A stale picture fooled us for a while: the ZZ9000 keeps
showing its last frame after a reset, so a native boot looks like the
frozen Prism screen on that monitor. exec's Alert patch showed its first
real guru on the native display.

Open on the ZZ9000: PrismBench's new area check is 12 wrong of 16000 (the
first rows under a JAM2 triangle's apex stay unpainted) - the masked
BltPattern -> template path on the firmware's RECT_TEMPLATE.

### The ZZ9000 "area bug" was a blitter race (fixed)

`tools/tmpltest.c` (sparse templates: one bit, two bits, runs of 1-12
bits per row, aligned and not, JAM1 and JAM2; then a filled triangle
against graphics.library's own) showed the firmware's template blit to be
exact - 0 wrong in six passes - while the triangle lost its left edge on
the first rows, a different 4 to 7 pixels each run. AreaEnd fills the
TmpRas mask with the Amiga's blitter and calls BltPattern at once; the
ROM's BltPattern queues behind that blit, Prism's read the mask straight
away, and the 68060 got there before the fill had reached the top rows
(the 68030 and the emulator never did). Prism now calls WaitBlit() before
it reads any planar source the blitter may still be writing: BltPattern's
mask, BltTemplate's template, a planar source or mask in
BltBitMapRastPort / BltMaskBitMapRastPort, a planar side in BltBitMap.
A4000 after: triangle 0 wrong three runs running, area check 0 of 16000,
area benchmark 62 /s; planar blit 1,010 /s (1,028 before - no cost).
Bitmap-to-screen blit read 24,243 /s against 28,868 last night: not
looked into.

## Application sweep on the emulated box (2026-10-04/05)

Every program in an AmiKit install (58: games, utilities, Internet
programs), 17 RTG titles from Aminet and 11 game ports, started one after
another on the emulated Picasso II+ box with Workbench on Prism 800x600
16-bit, each screen grabbed with `prismgrab` and looked at. The machine is
reset between programs (`sweep.py` in the session scratchpad drives it
over NetHarness and restarts the emulator by itself after a hang).

Drew correctly: ArTKanoid, MagicNumbers, WBsteroids, dynAMIte, iGame,
AMPlifier, AmiNetRadio, AmigaAMP, Apdf, CharMap, Directory Opus 4,
Eagleplayer, EvenMore, FileX, FlashPlayer, FroggerNG, FryingPan, Heddley,
HippoPlayer, HomeBank, MUIbase, MakeCD, Meridian, MultiRen, MysticView,
AWeb, AmIRC, AmiComSys, AmiTradeCenter, AmigAIM, AmigaJournal, CManager,
IBrowse, Jabberwocky, NetSurf, NewsCoaster, RDesktop, WallGet,
WookieChat, YAM, amrss, P96Speed (its window), CyberAVI, Ballfield (SDL,
in a window), Amiga Chess RTG, AmiArcadia, ArtEffect 4 demo, DoomAttack
(in a window), and through their screen mode requesters ADoom (320x200
8-bit), AHeretic (320x200 16-bit) and the Joyride demo (320x240 16-bit).
Every mode requester listed the Prism modes.

Found and fixed:

- **BitMapScale was not handled.** Lupe (a screen magnifier) copies a
  piece of the screen into a friend bitmap, scales that up with
  BitMapScale and blits the result into its window: black, because the
  ROM scaled the dummy plane. `h_BitMapScale` (render.c) does it for any
  pair of bitmaps where one is Prism's or planar outside chip RAM: the
  same chunky format pixel for pixel, anything into direct colour through
  RGB, the rest as pens. `tmpltest` checks a 16x16 -> 64x48 scale pixel
  by pixel (0 wrong at 8, 16 and 24 bits).
- **Pixel-format bitmaps: every plane pointer aims at the pixels now.**
  Only Planes[0] did; with BytesPerRow the pixel row size, a planar
  operation that got past Prism would have walked 960 KB of a 60 KB
  dummy plane. Nothing in the sweep did that - it came out of chasing
  Sudoku, below - but it costs nothing.
- **ScalePixelArray made a library call per pixel** (CopyMem). It now
  works out each destination column's source column once and scales a
  source line once however many destination lines it fills.
- The trace build's every-vector thunks declared the caller's registers
  as C arguments; the compiler is free to scribble on its argument slots,
  and those slots are what gets restored into the registers. Read through
  a pointer now. dbg() uses Disable, not Forbid (calls from interrupts).

Not Prism:

- **Sudoku (SDL + AHI + ttengine) "hangs the machine".** It took a
  sampling profiler to see it: lockwatch now has a vertical blank server
  that notes the running task fifty times a second, and Sudoku's own
  `Sudoku_soundtask` was the running task in 254 of 256 samples - it
  spins at a priority above everything else. No Prism call involved.
- WorldOfAscii, Leon, Nexus7, The Morning Trip, PhaseOne are chipset
  demos (they take the machine over or reset it).
- Amijeweled, Wet (Workbench start only), Freeciv, ScummVM, AmiTwitter,
  SimpleMail, HivelyTracker, Abuse, OpenTTD, MilkyTracker (an OS4 file):
  missing data, fonts or a network on the test box.
- ChaosPro, ASp, Eagleplayer: their own requesters (stored mode, ROM
  check, key file) - drawn correctly.

Open:

- **KillerCGX** (CNCD) opens PRISM:320x240 8-bit and locks the screen
  bitmap about 90 times a second, 33,000 times without damage to the
  memory lists (trace build), but the one frame grabbed was plain grey,
  and it starves the test harness. Needs a look on a real machine.
- **CgxBenchmark** runs its direct-access test at 15/16/24/32/8 bits,
  then its WritePixelArray test with multitasking off; on the cycle
  exact emulated 68030 that did not finish in 15 minutes. Run it on the
  A4000.
- NewVox draws flat grey bands (frame and palette arrive; whether the
  landscape should have hills there was not established).
- Hexen, ROTT, ZDoom, AmiQuake, AmiWolf, AmiSpear, Orbit, Polanie, Visage
  and CyberView with real pictures, P96Speed's test run: not done yet.

### Debugging kit added on the way

- `sh tools/mktrace.sh [ALL]` builds `out/PrismD.trace`: every handler
  entered goes to the log, exec's free memory lists are walked on each
  one ("MEMORY LIST DAMAGED ... seen entering X" names the first call
  made after the damage), and with ALL every graphics.library vector is
  logged with a0-a2/d0-d1. cgx calls are logged without the 40-line cap.
- `lockwatch`: the profiler above ("running, of N samples") and each
  ready task's saved context.
- An AmiKit drive mounted read-only in WinUAE is fine; AHI's "UAE" audio
  mode is not (use a Paula mode in the test box's ahi.prefs).

### The same sweep at 256 colours

The 52 AmiKit programs that are not chipset demos (and not Sudoku), again
with Workbench on PRISM:640x480 8-bit: no hang, no alert, and every
window that drew at 16 bits drew the same here (Lupe magnifies; SimpleMail
opened this time). One thing looks odd at both depths and was not checked
against a native screen: CManager's directory requester has a black list
area.

### The rest of the list (2026-10-05 morning)

- **Hexen** on PRISM:320x200 8-bit (its requester only lists 8-bit
  modes): demo plays, picture right.
- **Visage** shows a JPEG on a 24-bit Prism screen (640x400, the picture
  at the top left: it asked for 320x256 and there is no such 24-bit
  mode); an ILBM goes to a native screen.
- **P96Speed 1.2** runs all 21 tests on PRISM:640x480 8-bit
  (docs/p96speed-emulated.png; emulated Picasso II+, cycle exact 68030).
  One number stood out: RectFill with an area pattern, 5 a second against
  4,503 plain. That path wrote VRAM a pixel at a time; an area pattern
  without a mask is now handed to the template code as a 1-bit template
  (the pattern's rows repeated, in bands of up to 32 lines): 81 a second.
  `tmpltest` compares patterned fills with graphics.library's own in
  JAM1, JAM2, complement and inverse video: 0 wrong at 8, 16 and 24 bits,
  before and after. Left as they are: ScreenToFront 5 /s, DrawEllipse
  204 /s, BitMapScale 87 /s.
- Not Prism: ZDoom and AmiQuake need a 68060 (the emulated 68030 dies
  before their first graphics call - A4000 material); AmiWolf and
  AmiSpear are AGA ports and fail the same way without Prism; ROTT opens
  a native screen; Orbit wants lucyplay.library; Polanie wants the
  original game's data; CyberView leaves with return code 20 before any
  graphics call.

### On the A4000 (ZZ9000, 68060), 2026-10-05

Deployed over NetHarness (backup C:PrismD.pre-1005), one reboot each
time, no trouble. tmpltest 15/19/19 lines "0 wrong" at 8/16/32 bits (it
now also writes a colour ramp with cybergraphics WritePixelArray in RGB,
RGBA, ARGB and LUT8-with-table and reads it back), p96test 0 of 22
failed, PrismBench checks all 0 wrong.

CgxBenchmark 1.1 runs to the end here. Raw VRAM 6.2 / 5.5 MB/s. Its
WritePixelArray numbers were poor where colours are converted, so
write_cb got one loop per destination format (row_to_16, row_to_deep)
instead of looking at the formats for every pixel:

    320x240 frames/s       before   after
    LUT8 -> 8-bit           68.7     68.7
    LUT8 -> 16-bit          28.1     28.0
    LUT8 -> 32-bit           8.1     15.0
    ARGB -> 16-bit           9.0     13.5
    ARGB -> 32-bit           8.2     13.1
    ScalePixelArray x2
      ARGB -> 16-bit         2.2      3.2

KillerCGX, resolved: not Prism. The archive's `Killer.exe` is a three-line
script - `Assign Killer: ""`, then MAIN.BIN - and every run so far had
started main.bin without that assign, so the demo found none of its data:
it opens PRISM:320x240 8-bit, locks the bitmap each frame, draws nothing
(grey) and waits for a mouse button at a priority that starves the test
harness. (On the A4000 it first stopped behind a requester for
ixemul.library 48, now installed there.) Laine's click ended it each time.
Started with the assign on the A4000 it plays: Laine photographed the
title picture, palette right, with the particle effect moving over it.

ZDoom (the "RTGF" 68060 build from Laine's ports drawer) resets the A4000
a moment after it starts - with Prism loaded and, started again on the
failsafe boot that followed, without Prism loaded. Not Prism; this is
also what it did to the emulated 68030. A 320x240 (pixel-doubled) Prism
screen opened and closed on the ZZ9000 leaves its Ethernet as fast as
before (0 of 12 pings lost, 334 KB in 0.4 s): the slow network after
KillerCGX was the demo's doing, cured by a power cycle.

DevilutionX (Diablo) from Laine's A4000: resets the machine the moment it
starts, before any call into Prism (LOG=SYNC log empty) - and, booted
once without Prism, it ended at the insert-disk screen all the same. Not
Prism. Two things learned for testing on this machine: the A4000 goes on
answering pings while it sits at the insert-disk screen (ping says
nothing about AmigaOS being up; only the harness does), and
`Echo >ENVARC:Prism.booting` followed by a reboot gives one boot without
Prism for a comparison run.

OpenDUNE 0.9 (NovaCoder's SDL port) on the A4000: opens PRISM:320x200
8-bit and shows its intro picture with the right palette
(docs/a4000-opendune.png). The picture did not change in a minute and
Escape did nothing - whether it normally moves on at that point on this
machine is a question for Laine. Started with `<NIL:` as input it leaves
at once without a word; with a console it runs.

### The two slow P96Speed lines, on the real machine (2026-10-05)

`stftest` (two screens, flipped 40 times; then 200 ellipses) on the A4000:
ScreenToFront between two PrismRTG screens 25-33 a second, between two
native screens 50 a second - one extra frame per flip, not the 5 a second
P96Speed measured on the cycle-exact emulated 68030. DrawEllipse: 477 a
second on a PrismRTG screen against 124 on a native one. Neither needs
work; the emulator's numbers were the emulator's speed.

## Small machines (2026-10-06/07)

Ten WinUAE configurations made from the Picasso II+ test box (`prism low
<tag>.uae`): 68030 at 28 MHz with 20, 4, 2, 1 and no MB of fast RAM, 1 MB of
chip RAM, a 68020 and a 68EC020 at 14 MHz with 4 MB and with none, and a
68020 without an FPU. Workbench 3.2 at 800x600 16-bit on the card, MUI and a
TCP/IP stack loaded. On each: boot time, `Avail`, `tmpltest`, PrismBench, then
eleven programs each on a freshly booted machine (MysticView, Personal Paint,
Directory Opus 4, AmigaAMP, YAM, IBrowse, AWeb, NetSurf, ADoom, ScummVM,
OpenTTD) with `Avail`, screens, windows, the last alert and a picture.

First run, with 1.0.1:

- Workbench came up on the card on every machine down to 2 MB chip and no
  fast RAM. With 1 MB of chip RAM and no fast RAM, it opened on a native
  screen instead: the Prism screen could not be had, and Intuition fell back.
- 4 MB of fast RAM: every test passed, light programs ran, big ones said
  there was too little memory. 2 MB: nothing beside Workbench. With 1 MB of
  fast RAM more ran than with 2 MB: the Workbench backdrop (an 800x600
  16-bit bitmap) went to chip RAM rather than taking the last of fast RAM.
- 68020 / 68EC020 at 14 MHz: same programs as the 68030, fills and copies
  about half as fast.
- 68020 with no fast RAM, ~120 KB left: two "Software Failure" requesters
  (error 80000004) and two hangs. `trapwatch` caught one: in IPrefs, running
  in its own data, while NetSurf had closed Workbench and RAM: was full. Not
  seen again in 13 more tries. `trapwatch` now also lists where each
  process's code and each library's functions are, so the next one can be
  put to a program.
- No FPU: AmigaAMP, MysticView, NetSurf (ixemul) and OpenTTD need one.
  PrismRTG doesn't.

PrismD took 555 KB, all of it fast RAM: 310 KB loaded (138 KB code, 171 KB of
buffers) and 245 KB once running. The 245 KB was libnix: it gives every
stdio stream a 64 KB buffer (`__BUFSIZE`), and PrismD has stdin, stdout and
stderr, and opened the prefs with `fopen` - whose buffer stayed with
malloc after `fclose`. A program that only waits takes 21 KB; with one
`printf`, 180 KB. 1.0.2: `__BUFSIZE` points at 1 KB, `prefs_load` reads
with `Open`/`FGets`, and the cgx and Picasso96 row callbacks borrow
render.c's `rgbRow` (every `rect_cb` runs with `lock` held) instead of a
16 KB buffer each. PrismD now takes 314 KB.

Second run, with that PrismD: 2 MB fast now runs MysticView and Directory
Opus with 440 KB left after booting (205 KB before); no fast RAM leaves
365 KB of chip RAM (146 KB); the 68020 with no fast RAM had no Software
Failure, one hang (IBrowse, as before). A second Prism screen next to an
800x600 16-bit Workbench on a 2 MB card still needs ~1 MB in one piece to
move a screen off the card; the README says so.

## Other emulators (2026-10-08)

Amiberry 8.3.0 (Ubuntu 24.04 build, run under WSL) and FS-UAE 3.1.66 were set
up next to WinUAE on the same test disk; WinUAE's UAE RTG card was also tried
as Zorro II.

- **WinUAE 6.0.3, UAE RTG Zorro II (4 MB) and Zorro III (16 MB)**: `BOARD=UAEGFX`
  and `BOARD=P96 P96CARD=uaegfx.card` pass tmpltest at 8, 16 and 32 bits and
  p96test.
- **Amiberry 8.3.0, UAE RTG Zorro III**: 8 and 16 bits pass everything. At 32
  bits nearly every tmpltest line fails, with the card's blitter on or off.
  Narrowed down with small programs on a PrismRTG 640x480 32-bit screen
  (BGRA32, the same format WinUAE gives): plain long, word and byte accesses to
  the card's memory read back right, and all 16 MB are there; but data written
  into the screen's memory - by CopyMem or a plain CPU loop, 256 to 4096 bytes -
  is now and then replaced afterwards with older contents (the screen's
  background colour, or zeros), 2-4 copies in 40, at any pointer position. The
  same program in WinUAE: 0 in 100. Nothing PrismRTG does is involved (the test
  locks the bitmap and writes the memory itself); it looks like the emulator
  writing an older copy of the frame back. The test program is
  `tools/vramcheck` (`vramcheck $7A000301 8`, with PrismD running).
- **Found on the way, a PrismRTG bug**: PrismRTG waited for the blitter with
  WaitBlit() before reading planar memory the blitter may still be drawing (the
  mask of AreaEnd and Flood, templates). WaitBlit() only waits for the running
  blit; a fill queued behind others was read half-drawn - in Amiberry the top
  rows of every area fill. blit_settle() waits for the queue (OwnBlitter,
  WaitBlit, DisownBlitter; or only WaitBlit when the task owns the blitter).
  The A4000's earlier "a few pixels missing under a polygon's top corner" was
  probably the same.
- **FS-UAE 3.1.66**: the test disk stopped at "Please insert a volume
  containing LIBS/workbench.library" (the drive mounts; probably how FS-UAE 3.1
  reads this disk's read-only files). Not followed up.
- **libretro (RetroArch) Amiberry** keeps the RTG hardware pointer off, so the
  UAE driver shows no pointer there; a software pointer (part of PR #6) would
  fix it, as it would on the MiSTer (issue #2).

## PerfectPaint: not PrismRTG (2026-10-08)

The freeze after "Go" (see 2026-10-04) happens without PrismRTG too. On the
same emulated machine (68030 + 68882, OS 3.2.3) with WinUAE's UAE RTG card
and real Picasso96 (the free 2.0 that comes with AmiKit, rtg.library
40.3994) instead of PrismRTG, PerfectPaint 2.93 shows its launcher, takes
the "UAE: 800x600 16bit" mode, and after "Go" the emulated CPU halts
(WinUAE: HALT) at the same point. Without an RTG system it cannot be
started at all (its mode requester is the RTG system's). The blitter-queue
fix of 1.1 beta 2 does not change it with PrismRTG either. So it is
PerfectPaint on this machine, not PrismRTG; closed. Not tried: the current
Picasso96 release.
