# PrismRTG: how it is put together

A technical overview for someone who wants to read, review or extend the
code. The README says what PrismRTG does for a user; design.md is the
development diary in date order; the driver documents cover one layer. This
page is the map of the whole: what runs, what it patches, and how a drawing
call travels from a program to the card.

## 1. The shape of it

PrismRTG is one background program, `PrismD`, started by the
`DEVS:Monitors/Prism` entry before Workbench opens. It

* loads one driver module per board (`LIBS:Prism/<board>.driver`), each a
  separate process that talks to the hardware through a fixed interface
  (`src/prismboard.h`);
* registers its modes with graphics.library's display database as the
  monitors `PRISM:` (and `PRISM2:` with a second board), so Prefs/ScreenMode
  and every ASL requester see them;
* patches a small set of graphics.library and intuition.library functions,
  so screens on those modes get their bitmaps in the card's memory and the
  drawing calls on them are rendered by PrismD;
* builds three libraries for programs that ask for them directly:
  `cybergraphics.library` (CyberGraphX), `Picasso96API.library` and
  `cgxvideo.library`;
* publishes the `prism` semaphore (`src/prism.h`) for tools, and the ARexx
  port `PRISM` for scripts (`src/arexx.c`, docs/arexx.md).

Everything else - the compositor, the pointer, the prefs - hangs off that.
There is no prism.library; the daemon and its semaphore are the interface.

## 2. Boot

`DEVS:Monitors/Prism` (a tiny program) creates the boot flag
`ENVARC:Prism.booting`, starts `C:PrismD` and waits for its semaphore.
PrismD reads `ENV:Prism.prefs` (`src/prefs.c`), probes for boards, programs
each board's first mode (with the monitor still showing the Amiga), runs
the text self-test against a known answer (`src/selftest.c`: a board whose
blitter gets text wrong draws text on the CPU from then on), builds the
monitors and the libraries, installs the patches, and enters its loop.
A boot that never reaches that point leaves the flag behind, and the next
boot skips PrismRTG once.

## 3. The files

| File | What it is |
|---|---|
| `src/prismd.c` | the daemon: start-up, the display database monitors, screens (`struct PScreen`), the patch table, the per-tick loop, screen mode promotion, autoscroll, the semaphore |
| `src/bitmap.c` | `struct PBitMap`: a chunky bitmap PrismD owns, in VRAM or fast RAM, wrapped in the `struct BitMap` graphics.library hands around; allocation, the VRAM pool, shadow uploads for boards whose memory the CPU cannot write directly |
| `src/render.c` | the drawing handlers (`h_RectFill`, `h_Text`, `h_BltBitMap`, ... 23 of them): clipping through the layer's ClipRects, the pixel formats, the fast paths, when the board's blitter is used and when the CPU draws |
| `src/gels.c` | Bobs (`DrawGList`) on PrismRTG screens |
| `src/boardops.c`, `src/rtg_ops.c` | the surface-aware operation layer every driver answers to (fill, copy, expand, line, planar), with the CPU fallbacks and the limits |
| `src/present.c` | the compositor: when something must be shown that the card cannot show by itself - two screens during a drag, a PIP or video layer, the software pointer - it composes a frame in VRAM band by band and shows that instead |
| `src/composite.h`, `src/yuv.h`, `src/damage.h` | pixel conversion between formats, YUV 4:2:2 to RGB, change detection for rows |
| `src/pointer.c` | the mouse pointer: the board's sprite where it fits (size and the three colours), PrismD's software pointer where it does not or when a composed frame is shown |
| `src/cgx.c` | cybergraphics.library (V43): the CGX calls, chunky bitmaps of any format, LockBitMap, the alpha calls and ProcessPixelArray |
| `src/p96.c` | Picasso96API.library: the P96 calls, picture-in-picture windows, and the record of every PIP and video layer (`struct PrismPIP`), composed on the CPU or handed to a board that shows overlays itself |
| `src/cgxvideo.c` | cgxvideo.library (V42): video layers for players, on top of the PIP records |
| `src/driver_loader.c`, `src/driver_module.c`, `src/driver_module.h` | loading a `.driver` as its own process, the request block with the ABI check, the two board slots |
| `src/drv_picasso2.c`, `src/drv_zz9000.c`, `src/drv_uaegfx.c`, `src/drv_p96.c`, `src/drv_z3660.c` | the drivers: the Cirrus GD542x/5446 family (Picasso II, GBAPII++, Piccolo, SD64, Spectrum, Graffity, Picasso IV in Zorro III mode), the MNT ZZ9000, UAE's RTG card, the adapter that drives any Picasso96 `.card`, and the Z3660's RTG output |
| `src/arexx.c` | the ARexx port `PRISM`: boards, modes, the Test picture, power saving, the log |
| `src/drv_template.c` | the starting point for a new driver (writing-a-driver.md) |
| `src/prefs.c`, `src/prefs.h` | `ENV:Prism.prefs`: read, write, the defaults, shared with the tools |
| `src/catalog.c` | the tools' texts through locale.library catalogs |
| `src/stubs.S`, `src/p2c.S` | the assembly: the patch entry stubs that save the registers and hand a call to C (with the short cuts for WritePixel and ReadPixel), and the chunky conversions |
| `src/prism.h`, `src/prismint.h`, `src/prismboard.h` | the public semaphore, PrismD's internal declarations, the driver interface |
| `tools/` | PrismPrefs, PrismSetup, PrismProbe, PrismUpdate, PrismImport, PrismMon, PrismScreen and the test programs; `mkdriver.sh`, `mkpkg.sh`, `mkcatalog.py`, `mkguide.py` |
| `tests/` | host tests (the renderer replayed against recorded output, the presentation, the drivers against a fake board, the stack audit, the P96 adapter, the catalog and guide pipelines) |

About 15,000 lines of C in `src/`.

## 4. The patches

PrismD patches these library vectors (the `patches[]` table in prismd.c):

* **graphics.library**: `OpenMonitor`, `CloseMonitor`, `FindDisplayInfo`,
  `NextDisplayInfo`, `GetDisplayInfoData`, `ModeNotAvailable` - the
  display database answers for the PRISM modes; `MakeVPort`, `MrgCop`,
  `LoadView`, `ScrollVPort` - a view with a PRISM screen is shown by the
  card instead of the Copper; `LoadRGB4`, `LoadRGB32`, `SetRGB4`,
  `SetRGB32` - palettes go to the board (through the gamma curve).
* **intuition.library**: `OpenScreen`, `OpenScreenTagList`,
  `CloseScreen` - a screen on a PRISM mode gets a bitmap in VRAM, and
  screen mode promotion opens chipset screens on the board; `CloseWindow` -
  a video layer's window going away; `DisplayAlert`, `TimedDisplayAlert`,
  `Alert` - the display is handed back to the Amiga for an alert.
* **The drawing calls** (`RectFill`, `Text`, `BltBitMap`, `Draw`,
  `WritePixel`, `ScrollRaster`, ... the handlers in render.c and gels.c)
  are patched too, through stubs in `stubs.S`. A `struct BitMap` PrismD
  hands out carries a marker (`BMF_PRISM` in its flags, with a magic in
  the pad word), and each stub tests it before anything else: a call on a
  chipset bitmap goes straight on to the original function, so drawing
  on a native screen costs one compare and a jump.

## 5. A drawing call

`RectFill(rp, x0, y0, x1, y1)` from a program:

1. graphics.library's vector reaches the stub in `stubs.S`; it saves the
   registers into a `struct Regs` and calls `h_RectFill`.
2. `FOREIGN()`: if the RastPort's bitmap is not PrismD's, the original
   function runs. Otherwise the drawing lock is taken (`LOCK()`, which also
   lifts the software pointer out of the way).
3. The handler walks the layer's ClipRects (`clip_rp_q`) and, for each
   visible rectangle, calls a per-rectangle callback with the clipped
   coordinates; a plain fill in the foreground pen takes the direct path to
   the fill primitive instead.
4. The primitive decides: the board's blitter (through the driver's
   `fillRect` hook or its `PrismOps.fill`) when the bitmap is in VRAM and
   the board does it in hardware, else the CPU writes the pixels, in the
   bitmap's format (8-bit pens, 15/16-bit, 24-bit, 32-bit).
5. If a composed frame is on show (section 7), the rows touched are marked
   and the compositor refreshes them on the next tick.

Text, lines, blits and the rest follow the same shape with their own
primitives. The renderer never blocks on the board: a blit that fails
after starting is counted (`faults`) and the CPU takes over.

## 6. Bitmaps and VRAM

A PRISM screen's bitmap, and every chunky bitmap CyberGraphX or P96
programs allocate, is a `PBitMap`. VRAM is a pool per board; a bitmap that
does not fit, or that a program will lock and write for long (`locks`),
lives in fast RAM. The shown bitmap is whatever the card scans out
(`setDisplayStart`); a screen's bitmap can be larger than the mode
(autoscroll), and the card then steps rows by the bitmap's pitch.

## 7. The compositor and the pointer

The card shows one bitmap. When Intuition wants two screens at once
(dragging), when a PIP or video layer is attached to a window, or when the
pointer must be drawn by PrismD, `present.c` allocates a frame in VRAM,
composes the visible part of each screen into it (converting formats where
screens differ), draws the PIPs over it and the pointer over those, and
shows the frame. It works in bands of rows and only redoes rows that
changed. When nothing needs composing it stops and the screen is shown
directly again. Per-tick work (the pointer, autoscroll, layers on a
board's own overlay) runs from PrismD's loop every 1/50 s.

A video layer on a board with a hardware overlay (the ZZ9000) is not
composed at all: `p96_pip_hw_tick` hands its rectangle to the board and
takes it back when the screen is not the one scanned out.

Monitor power saving runs from the same loop: an input handler counts key
and mouse events, and after POWERSAVE minutes without one each board goes
dark - through its driver's `setPower` where it has one, else with a black
palette or a cleared bitmap on display. While dark, the board's display
start and palette calls are only noted (PrismD swaps the function pointers),
so flips and colour cycling keep it dark; input, a mode change or the monitor
switch puts everything back.

## 8. Drivers

A driver is a loadable module with one job: describe the board
(`struct PrismBoard`: VRAM, formats, limits, the sprite) and do what the
hardware can do (`setMode`, `setDisplayStart`, palette, the blitter hooks,
the overlay). PrismD never touches a register. The module runs as its own
process and is asked for things by message; the request block carries the
ABI number, the structure sizes and a layout hash, so a driver built for
another PrismD is refused rather than misread. driver-architecture.md,
driver-development.md and writing-a-driver.md go into this layer.

Two boards: each has its own monitor, screens and VRAM pool; a bitmap
carries its card; the pointer is on whichever is in front (multi-board.md).

## 9. The tools and the tests

PrismPrefs edits the prefs and tests modes; PrismSetup is the installer's
helper; PrismProbe lists boards; PrismUpdate checks GitHub over HTTPS;
PrismImport takes Picasso96's switched-on sizes into the prefs;
PrismMon and the smaller tools are for diagnosis. On the host, `tests/`
replays recorded drawing against the renderer, runs the drivers against a
fake board, audits the stack use of every handler, and checks the catalog
and guide pipelines. The emulator gate (in the WinUAE-Prism fork's
`prism-tools/`) boots every emulated board, runs the drawing suites and
the display checks, and benchmarks against the last release.
