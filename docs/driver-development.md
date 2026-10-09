# Driver development and submission: the technical reference

This is the contract between PrismD and a board driver, and the process
for getting a driver into PrismRTG. [writing-a-driver.md](writing-a-driver.md)
is the short, step-by-step version; read that first. This document is
the one to come back to for *why* and *exactly what*.

## 1. What a driver is, and is not

PrismD owns screens, bitmaps, drawing, the pointer, the display database,
cybergraphics and the Picasso96 API. A driver owns one kind of board:
where it is, how to set a mode and point the scan-out at VRAM, and -
optionally - its blitter, sprite and memory quirks. A driver never sees a
screen, a layer, a RastPort or an application.

Two kinds exist:

* **Native drivers** (`src/drv_picasso2.c`, `src/drv_zz9000.c`, yours)
  talk to registers. This document is about these.
* **Adapters** (`src/drv_p96.c`, `src/drv_uaegfx.c`) put an existing
  Picasso96 `.card` or UAE's RTG behind the same interface. They are
  selected by hand (`BOARD=P96`, `BOARD=UAEGFX`) and are not a model for
  a new native driver.

## 2. The pieces

| Piece | Where | Role |
|---|---|---|
| The interface | `src/prismboard.h` | `struct PrismBoard`, `struct PrismMode`, `enum PrismFormat`, the `PBF_*` flags, `driver_probe()` / `driver_retain()` |
| Operations | `src/boardops.h`, `src/boardops.c` | `struct PrismOps`, `struct PrismSurface`, `enum PrismResult`, `board_bpp()`, `board_defaults()` |
| Module glue | `src/driver_module.c`, `src/driver_module.h` | the executable's `main()`: handshake with PrismD, probe, wait, stop |
| Loader | `src/driver_loader.c` | `driver_open()`, `driver_scan()`, `driver_close()` in PrismD and PrismTest |
| Template | `src/drv_template.c` | a complete driver with every TODO marked; `-DTEMPLATE_FAKE` makes it a board of fast RAM |
| Build | `tools/mkdriver.sh NAME sources...` | one module: `driver_module.c` + your sources + `boardops.c`, `-DDRIVER_NAME="NAME"` |
| Tester | `tools/prismtest.c` (`PrismTest`) | a driver on its own: mode, pattern, blitter check |
| Kit | `out/pkg-check/PrismCheck` | `tmpltest`, `p96test`, `PrismBench`, `vramcheck`, `prismdiag`, `prismstate` on a running PrismD |

A driver is one source file, `src/drv_<name>.c`, and (if it needs them)
headers of its own. Its public symbols are `driver_probe`,
`driver_retain` and, for PrismTest-style static use, nothing else: keep
everything `static`.

## 3. Naming and loading

* The module file is `NAME.driver`; `NAME` is upper-case letters and
  digits, 32 at most, no `/ : \`. It is what `BOARD=NAME` uses.
* PrismD looks in `PROGDIR:Drivers/NAME.driver` first (a build run from
  its own drawer), then `LIBS:Prism/NAME.driver` (installed). At boot
  PrismD is `C:PrismD`, so installed modules are used.
* With the board on Auto, PrismD tries `PICASSO2`, then `ZZ9000`, then
  every other `.driver` in those two drawers in directory order
  (`driver_scan`), except `P96` and `UAEGFX`. A probe that finds no board
  must return FALSE quickly and quietly: on Auto, every installed driver
  runs its probe at each boot.
* `$VER: NAME.driver <version> (<date>)` is in every module
  (`driver_module.c` writes it from `DRIVER_NAME`), so `Version` reports
  it.

## 4. The runtime model

A module is an AmigaDOS process (`CreateNewProc`, 16 KB stack) started by
PrismD with the request address as its argument. Its own C runtime and
library bases (`GfxBase`, `ExpansionBase`, `UtilityBase`) are opened by
`driver_module.c` before `driver_probe()` runs. The process then waits
on a message port until PrismD stops it, so every function pointer the
probe put in the `PrismBoard` stays valid.

**Callbacks do not run in the driver's process.** They run in whatever
task called into PrismD: an application drawing, Intuition opening a
screen, PrismD's own tick task. Therefore, inside any callback:

* No `Wait()`, no DOS calls, no `printf` (the driver's stdio belongs to
  its own process; `driver_probe` and `modeReady` are the exceptions,
  they run in the driver process and may print diagnostics).
* No allocation that can fail silently mid-operation; allocate in the
  probe.
* The PrismD lock (a SignalSemaphore) is held by the caller for every
  callback except `checkMode` and `bytesPerRow`/`ops->pitch`, which are
  pure functions of their arguments. The lock serialises drawing, VRAM
  paging and mode changes, so a callback never runs concurrently with
  another callback. Shared command structures (a mailbox) still need
  `Forbid()`/`Permit()` if an interrupt or another driver path touches
  them.
* A callback may take as long as the hardware needs, but it is in the
  caller's time: a `waitBlit` that spins for a second freezes the
  application. Bound every hardware wait (`for (n = 0; n < LIMIT && busy; n++)`)
  and report a timeout as a failure rather than spinning forever.

Who calls what:

| Callback | Called from | When |
|---|---|---|
| `driver_probe` | the driver process | once, at load |
| `checkMode` | PrismD at start (mode list), PrismPrefs' Test | no lock, no hardware |
| `setMode` | the task opening/showing a screen; PrismD at start | lock held; the board may be showing another mode |
| `modeReady` | PrismD's task, after the first `setMode` | lock not held; the only place after the probe that may print |
| `setDisplayStart` | screen show, double-buffer swap, compositor | lock held; may precede the first `setMode` |
| `setPalette` | `SetRGB32`/`LoadRGB` callers, screen show | lock held |
| `setSwitch` | screen show/hide, PrismD start and quit | lock held |
| `waitVBlank` | `ChangeVPBitMap` (double buffering) | lock held |
| `saveState`/`restoreState` | PrismTest only | before/after borrowing the board |
| `fillRect` ... `drawLine`, `ops->*` | any drawing task | lock held (quick paths hold `Forbid` instead - the same exclusivity) |
| `waitBlit` | any task about to touch VRAM with the CPU | lock held |
| `cursorImage/Show/Move` | PrismD's tick task | lock held; `Move` up to 50 times a second |
| `shutdown` | PrismD's task at quit | after the last screen closed |
| `driver_retain` | the driver process | after PrismD said stop; return, or park forever |

`driver_retain` exists for adapters that claimed something they cannot
give back (UAE's card context). A native driver returns from it; its
process then ends and PrismD can be started again.

## 5. The board

Filled in by `driver_probe`:

| Field | Required | Meaning |
|---|---|---|
| `name` | yes | shown everywhere a board is named |
| `configDev` | no | the autoconfig node (PrismTest uses it to recognise a ZZ9000; NULL is fine) |
| `regs` | no | register window, for the driver's own use |
| `vram` | yes | 68k address of VRAM offset 0 |
| `vramSize` | yes | bytes PrismD may use; stop short of anything the firmware keeps there |
| `formats` | yes | `PF_BIT()`s of the layouts the board scans out |
| `flags` | - | `PBF_*`, below |
| `maxWidth`, `maxHeight` | yes | the largest mode |
| `priv` | - | the driver's state |
| `setMode`, `setDisplayStart` | yes | |
| `checkMode`, `setPalette`, `setSwitch`, `waitVBlank`, `shutdown` | no | defaulted by `board_defaults()` |
| `ops` | no | the blitter (section 7) |
| `fillRect` ... `drawLine`, `waitBlit`, `blitMax*` | no | direct hooks (section 7) |
| `cursorImage`, `cursorShow`, `cursorMove` | no | with `PBF_HW_CURSOR` |
| `bytesPerRow` | no | row padding by bytes per pixel; `ops->pitch` when it depends on the exact format |
| `saveState`, `restoreState` | no | |
| `modeReady` | no | |

Flags:

| Flag | Set by | Meaning |
|---|---|---|
| `PBF_HW_CURSOR` | driver | the three cursor hooks work; without it PrismD draws a software pointer |
| `PBF_BLIT_32` | driver | fills and text take 4-byte pixels (32-bit modes accelerated) |
| `PBF_SHADOW` | driver | VRAM is not CPU-addressable in the normal way: PrismD keeps a fast-RAM copy of every bitmap and uploads changed spans through `ops->write` (section 8) |
| `PBF_BANKED` | driver | the CPU window is banked; byte-range transfers only |
| `PBF_REINIT` | driver | the driver's memory pool must be rebuilt for a new mode: PrismD releases VRAM before `setMode` |
| `PBF_ACCEL_BROKEN` | PrismD | an operation failed: CPU from now on |
| `PBF_SOFTWARE` | PrismD | blitter off in PrismPrefs |
| `PBF_PRESENT` | PrismD | the compositor reuses scan-out; direct hooks stand down |

**VRAM is a byte array that PrismD manages.** Every bitmap PrismD puts on
the card lives at some offset; offsets are what the driver gets in every
call (`dst`, `src`, `offset`, `setDisplayStart`). PrismD pages bitmaps
in and out of VRAM to make room (a 2 MB card holds a few screens), so the
same bitmap can be at a different offset later - but never while it is on
display, while a program holds a `LockBitMap` on it, or in the middle of
an operation. A driver must not remember offsets between calls, except
the display start it was last given.

Pixel formats (`enum PrismFormat`) name the bytes as the 68k sees them in
VRAM: `PF_RGB565BE` is `RRRRRGGG GGGBBBBB`, `PF_RGB565LE` the same two
bytes swapped (PC order), `PF_BGRA32` is B,G,R,A and so on. Offer the
formats the board scans out natively; PrismD never converts on the way
to VRAM. Colours passed to fills, lines and text are the pixel bytes in
that order held in a `ULONG` (an 8-bit pen is the pen; a 16-bit pixel is
its 16-bit value; a 32-bit pixel its four bytes).

## 6. Modes

PrismD builds its mode list at start from PrismPrefs' slots: eight sizes
(640x400 ... 1920x1080, 320x200, 320x240) times four depths (8, 16, 24,
32), each with a refresh rate or 0 for the driver's default. For each
slot it picks the board's format for that depth (16-bit prefers 565 over
555; 24 and 32 are distinct: a board with only 32-bit formats gets no
24-bit modes) and calls `checkMode`. What `checkMode` accepts is offered
as a `PRISM:` screen mode; what it refuses is simply absent. The mode's
ID is fixed by its slot, so refusing one never renumbers the others.

`checkMode(b, m)`: pure. Return FALSE for anything the board cannot show
(size, format, refresh). Fill `m->bytesPerRow`: the pitch PrismD will lay
the screen's bitmap out with. Pad it as the hardware needs; PrismD
handles any pitch >= width * bytes per pixel. The default accepts any
size up to `maxWidth` x `maxHeight` in one of `formats` with unpadded
rows, which suits a board whose firmware takes a size and times it
itself.

`setMode(b, m)`: program the board. It may be called while another mode
is on display and while VRAM holds live bitmaps; nothing in VRAM may be
disturbed (a board that must clear or reorganise memory on a mode change
is a `PBF_REINIT` + `PBF_SHADOW` board). Return FALSE and PrismD shows
nothing rather than a broken mode. The display start last given applies
to the new mode.

Modes below 640x400 (320x200, 320x240) are usually shown pixel-doubled;
that is the driver's business, PrismD only asks for the size.

## 7. Drawing

PrismD draws on the CPU unless the driver offers better. Two layers:

**`PrismOps`** (`boardops.h`), the general path. `fill`, `copy`,
`expand` (1-bit template to colour, i.e. text), `line`, `planar`
(planar source to chunky destination), `pitch`, and the memory hooks of
section 8. Each gets `PrismSurface`s: `offset`, `pitch`, `width`,
`height`, `format`, `bpp`, `allocation` and `flags` (`PSF_VRAM`), with
rectangles already clipped and a non-zero size. Results:

| Result | Meaning | What PrismD does |
|---|---|---|
| `PR_DECLINED` | nothing written | draws it on the CPU |
| `PR_DONE` | finished, pixels in VRAM | nothing |
| `PR_RETRY` | failed part way, engine stopped, the operation disabled itself | redraws on the CPU (when safe) and keeps the other operations |
| `PR_FAILED` | engine wedged | redraws on the CPU (when safe), `PBF_ACCEL_BROKEN`, CPU from now on |

"When safe": a fill, text or line can be replayed; a copy that overlaps
itself cannot (the source is half moved), so decline anything the engine
might fail on rather than failing it. Decline is cheap and exact: use it
for every pitch, size, alignment or format the engine cannot encode.

Return only when the pixels are in VRAM, *or* provide `waitBlit`, which
PrismD calls before the CPU reads or writes VRAM and before the
software pointer draws. A driver with an asynchronous engine must
provide it.

**Direct hooks** (`fillRect`, `copyRect`, `copyBetween`, `expandRect`,
`drawLine`), the fast path. PrismD's inline paths call them for small
blits that fit `blitMaxBytes` (bytes per row), `blitMaxRows` and
`blitMaxPitch` (0 = unlimited), without the surface layer: on a 68030
the extra call levels cost about a third of a small blit. They have no
result code - they must succeed for anything within the limits, so set
the limits to what the engine encodes. Everything else goes through
`ops`. Add these after `ops` works, with the same checks.

`drawLine` must produce PrismD's own pixels (see `line_solid` in
`render.c`): one step along the longer axis per pixel, the shorter axis
at `round(i * S / L)` with halves rounded up. PrismBench's line check
compares every pixel with the CPU; a blitter with a different rounding
rule should decline lines.

`expand` / `expandRect`: rows of `(w + 7) / 8` bytes, `mod` bytes apart,
bit 7 of a row's first byte is pixel x. Set bits get `fg`; clear bits
get `bg`, or stay as they are when `transparent`.

## 8. Memory the driver owns

Linear boards need none of this: `vram` is the frame buffer, PrismD
reads and writes it directly. For boards where that is not true:

* `ops->read(b, s, offset, buf, count)` / `ops->write(b, s, offset, data, count)`:
  byte-range transfers between CPU memory and device memory, with the
  driver doing bank switching or aperture moves. With `PBF_SHADOW`,
  PrismD keeps every bitmap in fast RAM, draws there, and uploads only
  the spans that changed; accelerated operations are followed by a
  read-back of the destination rectangle so the shadow stays exact.
* `ops->allocate(b, surface)` / `ops->release(b, offset)`: the driver's
  own VRAM pool, when alignment or placement rules are stricter than
  PrismD's first-fit. PrismD checks the returned offsets and tracks
  every live allocation; it owns eviction.
* `PBF_REINIT`: with `PBF_SHADOW`, PrismD releases every device
  allocation before `setMode` so the pool can be rebuilt for the new
  format; application pointers stay stable because they point at the
  shadows.

The compositor (screen dragging, picture-in-picture) sets `PBF_PRESENT`
while it reuses a screen's VRAM for the composed frame; the direct hooks
stand down and nothing a driver does changes.

## 9. Pointer, switch, blanking, state

* **Cursor:** `cursorImage(b, img, rgb)` gets a 64x64 byte image (0
  transparent, 1-3 the colours in `rgb`, three R,G,B triples) and is
  called when the image or its clipping changes; `cursorMove(b, x, y)`
  with `x, y >= 0` (PrismD shifts the image for negative positions);
  `cursorShow(b, on)`. Set `PBF_HW_CURSOR` only when all three work in
  every mode the board offers; a cursor that is wrong in one mode is
  worse than the software pointer, which costs a few percent.
* **Switch:** `setSwitch(b, TRUE)` shows the board's picture,
  `FALSE` the Amiga's own, for boards with a pass-through. PrismD calls
  it with FALSE at start (so the native display stays up until a
  PrismRTG screen opens), on every change of the front screen, and at
  quit. A board without a switch leaves it NULL. A board that drives an
  external switch through other hardware (CIA lines, a serial port) does
  that here.
* **Vertical blank:** `waitVBlank` returns when the next blank starts.
  Used for tear-free double-buffer swaps; NULL means swap at once.
* **State:** `saveState`/`restoreState` snapshot whatever another RTG
  system left on the card, so `PrismTest` can borrow a board from a
  running Picasso96 and hand it back.
* **Shutdown:** power-on state, or at least the Amiga's video. Called
  once, after the last screen closed, before the module is stopped.

## 10. ABI and versions

Three numbers guard the module boundary:

* `PRISM_BOARD_ABI` (`prismboard.h`): the `PrismBoard` contract. Bumped
  when a callback's meaning or a field changes.
* `PRISM_OPS_ABI` (`boardops.h`): the operation table.
* `PRISM_DRIVER_ABI` (`driver_module.h`): the request/handshake protocol.

Plus a layout hash over every field's offset and size, so a module built
against different headers is refused before it touches hardware
("ABI/version or layout mismatch"). The practical rule: **a module is
valid for the PrismD it was built with.** A driver distributed on its
own works only with that exact release, which is why drivers belong in
the repository: each release then builds and ships them together.

## 11. The testing ladder

Each rung is a reason to stop and fix before the next:

1. `sh tools/mkdriver.sh NAME src/drv_name.c` builds without warnings.
   (`-Wall` is on; the host suite also builds everything with `-Wextra`.)
2. **Optional but valuable:** a host fixture under `tests/` that
   `#include`s the driver and checks its `ops` decline/accept rules and
   address arithmetic with fake registers (`cirrus_limits.c`,
   `zz_surfaces.c` are the models); `sh tests/architecture.sh` runs it
   under vamos with no Amiga.
3. `PrismTest BOARD=NAME DEPTH=8`, then 16 (and 24/32 if offered), at
   two or three sizes, with PrismD **not** running. The pattern must be
   right: hue bars, a grey ramp, a checkerboard, a label. Wrong colours
   = wrong format; shear = wrong pitch; a stripe of garbage = the
   display start or a clipping limit.
4. `PrismTest BOARD=NAME BLIT` at each depth: every byte of the fills and
   copies must match the CPU.
5. `Run >RAM:PrismD.log C:PrismD BOARD=NAME LOG`, open a `PRISM:` screen
   from ScreenMode. Then the PrismCheck kit on that screen: `tmpltest`
   at 8, 16, 24/32-bit (every line "0 wrong"), `p96test` (0 failed),
   `vramcheck`, `PrismBench MODEID=...` (its exactness lines "0 wrong",
   and the speed figures to put in the submission).
6. Real programs: Workbench, a Shell, MultiView on a picture, IBrowse or
   AWeb, a game that uses cybergraphics (Doom ports), screen dragging
   with `DRAGGING=ON`. Watch the PrismD log for driver failures.
7. Hardware. A driver that has only run in an emulator is marked
   "emulation only" until someone confirms it on the real board.

Keep the outputs: `PrismTest` and `PrismD.log` texts, the PrismBench
figures, a photo or screenshot of step 3. They go with the submission.

## 12. Submitting a driver

**Licence and provenance.** PrismRTG is GPL-3.0-only; a contribution is
GPL-3.0 or later, or compatible. A driver is written from hardware
facts: data sheets, register references, the board's own open firmware
or GPL drivers (the ZZ9000 and Z3660 sources are GPL), measurements on
the board. Nothing from a closed driver - not its disassembly, not its
constants copied without a public source. Say where the facts came from
in the file header.

**What a submission contains:**

1. `src/drv_<name>.c` (and headers), with a header comment: the board,
   the chip, what works, what is not implemented, where the register
   facts came from, your name.
2. The build line: `sh tools/mkdriver.sh NAME src/drv_name.c` added to
   `build.sh` under "Loadable board drivers", and `NAME` in the driver
   list in `tools/mkpkg.sh` so the package carries it.
3. Detection: the board's autoconfig manufacturer/product in
   `tools/prismsetup.c` (`DETECT`, the installer's greeting and the
   `PrismCard` number) and `tools/prismprobe.c` (marks the board as
   drivable).
4. Docs: a row in the README's board table and in `dist/ReadMe`'s board
   list, and - for a board with quirks worth recording - `docs/<board>-hw.md`
   in the style of `zz9000-hw.md` / `cirrus-picasso2-hw.md`.
5. Test evidence: the ladder's outputs, and one line saying what it was
   tested on (board revision, firmware, CPU, emulator version or real
   machine).
6. Credit: your name as you want it in the README's Credits, the
   package ReadMe and the release notes.

**How:** a pull request on [GitHub](https://github.com/lainejones/PrismRTG)
(or on the Forgejo mirror) with those files, or the driver file plus the
notes by any other channel if a PR is not your thing - it will be
committed under your name either way (`git commit --author`). A partial
driver (mode set working, no blitter yet) is a fine first submission:
say so, and it ships as a CPU-only driver until the rest arrives.

**What the maintainers do with it** (so nothing is forgotten; also the
checklist for drivers written here):

- [ ] review against sections 4-9: no DOS/Wait in callbacks, bounded
      waits, every decline case, offsets never cached, `waitBlit`
      present for an asynchronous engine
- [ ] build under `-Wall -Wextra` (`tests/architecture.sh`), host fixture
      if supplied
- [ ] run the ladder in an emulator where one exists; on hardware where
      it is here (A2000 GBAPII++, A4000 ZZ9000)
- [ ] `build.sh`, `mkpkg.sh`, `prismsetup.c`, `prismprobe.c`, the
      installer's card list, README table, `dist/ReadMe`, this document's
      "two kinds" if it is a new kind
- [ ] version string in the driver's `$VER` is the release's
- [ ] Credits in README, ReadMe, release notes; author kept on the commit
- [ ] "emulation only" or "tested on <hardware> by <name>" in the table

## 13. Where the rest is

* [driver-architecture.md](driver-architecture.md): how PrismD uses all
  this from the inside - surfaces, VRAM paging, shadows, the software
  pointer, the compositor.
* [driver-modules.md](driver-modules.md): the module protocol, handshake
  and lifetime in detail.
* [p96-drivers.md](p96-drivers.md), [uaegfx.md](uaegfx.md): the adapters.
* [zz9000-hw.md](zz9000-hw.md), [cirrus-picasso2-hw.md](cirrus-picasso2-hw.md):
  what two real boards taught us; the shape a `<board>-hw.md` takes.
