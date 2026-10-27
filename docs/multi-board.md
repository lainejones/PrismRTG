# Two boards at once (design, October 2026)

Goal: PrismRTG driving two graphics boards in one Amiga at the same time - each with its own
screens on its own monitor (step 1) - and later one screen stretched across both monitors
(step 2). Until 1.1 beta 6 PrismD drove one board: with a ZZ9000 and a GBAPII++ fitted it took
the first its drivers found and left the other alone.

**State (October 2026):** steps 1-5 below are done and pass in WinUAE-Prism's `dual-060.uae`
(every drawing check on both boards, each board showing its own front screen, the pointer
following the front screen, dragging on either board); not yet tried on real hardware with
two cards in one machine.

Developed and tested on the emulated boards in WinUAE-Prism (`dual-060.uae`: a 68060 with a
ZZ9000 and a GBAPII++ on a second monitor), then on real hardware.

## What is per board today

Everything below is one global now and becomes one per board, collected in `struct PrismCard`:

| State | Where | Notes |
|---|---|---|
| `board` (the driver's `PrismBoard`: hooks, VRAM, flags, limits) | prismd.c, used in 9 files (~170 places) | the drawing hot paths read it on every call |
| mode table (`modes`, `nmodes`) and the display-database monitor (`prismMonitor`) | prismd.c | each board gets its own monitor, so its modes have their own ModeIDs |
| VRAM allocator (`used`, `nused`) | prismd.c | per board: a bitmap lives in one board's memory |
| what the board shows: `shown`, `shownVP`, `cardMode`, `shownPbm`, `showStamp`, `rtgOn`, palette shadow | prismd.c | each board shows its own frontmost screen, or the native display passes through |
| test picture (`testPbm` ...) | prismd.c | PrismPrefs' Test button targets one board |
| pointer state | pointer.c | only the board showing the front screen shows the pointer |
| compositor (`frame`, row state) | present.c | dragging and PIPs, per board |
| `textBad` | selftest.c | the text self-test result, per board |
| driver process | driver_loader.c | one module per board (two different drivers at first) |

Stays shared: the drawing lock (one lock for all boards keeps the lock order as it is), the
preferences, the list of PrismRTG screens, the patches, cybergraphics.library and
Picasso96API.library.

## How things find their board

* **A bitmap** records its board: `PBitMap.card`. Drawing into it uses that board's hooks and
  limits; its VRAM comes from that board's allocator. A bitmap made with a friend lands on the
  friend's board. Bitmaps not yet in VRAM (fast RAM) still belong to a board, the one they would
  be shown on.
* **A screen** belongs to the board whose monitor its ModeID is on. Board 0 keeps the
  `PRISM:` monitor and ModeIDs `$7A00xxxx` (nothing changes for existing setups and prefs);
  board 1 registers `PRISM2:` with `$7A01xxxx`. ScreenMode and every program pick a board by
  picking a mode, as with any multi-monitor Amiga system.
* **What a board shows** is the frontmost screen on that board's monitor: on screen depth
  changes PrismD looks at each board separately. A board whose screens are all closed or behind
  native screens switches its monitor back to the Amiga (as one board does now).
* **The pointer** follows Intuition: it is on the front screen. The board showing the front
  screen shows the pointer; the other board hides its own.

## Speed

The hot paths read `board.fillRect` etc. from a global today. With two boards they read them
through the bitmap's board: one more load per call. That must not cost measurable speed on a
68030 - each step is benchmarked on the cycle-exact 030 box against the step before (three code
alignments averaged, as in the beta 5 speed work), and on the A4000 when it is available.

## Steps

1. **`struct PrismCard`, one board** - move the per-board state into `cards[0]`, with the old
   names pointing there. No change in behaviour or speed; all seven emulated boxes must pass.
2. **Bitmaps and screens carry their board** - `PBitMap.card`, `PScreen.card`; drawing, VRAM
   allocation and display go through them. Still one board loaded. Speed checked.
   In detail:
   * `PBitMap` gets `card` (its `struct PrismCard`) and `bd` (that card's `PrismBoard`, kept
     beside it so the drawing fast paths load the board in one step). Set where a bitmap is
     made: a screen's from its mode, a friend's from the friend, anything else card 0.
   * The fast paths in `prismint.h` (`HW_OK`, `hw_fits`, `pbm_fill`, `pbm_copy`, `pbm_line`,
     `pbm_expand`, `BOARD_CAN`) and `pbm_fits` take the board from the bitmap. A copy between
     bitmaps of two boards goes through the CPU.
   * The VRAM allocator takes the card: `vram_alloc`/`vram_free`/`vram_get_size`/
     `vram_free_bytes` work on that card's block list (`cst[n].blk`).
   * `ModeRec` gets `card`; `PScreen` takes its card from its mode.
   * The compositor, pointer, Test picture and text self-test use the card of the screen they
     work on (still always card 0 here; per card in step 4).
   * The global `board` stays as card 0's storage until step 3.
3. **Load two boards** - Auto loads every board its drivers find (or `BOARD=` / a new `BOARD2=`
   says which); the second registers `PRISM2:`. Screens on board 1 open and draw; all checks
   pass on both boards in `dual-060.uae`.
4. **Show both** - each board shows its own front screen and switches on its own; pointer on the
   front screen's board; compositor and Test button per board.
5. **PrismPrefs and docs** - the second board's modes in PrismPrefs, PrismCheck reports both
   boards, README.

Then step 2 of the goal (one screen across both monitors) is its own design.

## Two of a kind, and which board is the primary

Two boards of the same kind work: every `driver_open` loads its own copy of the driver module,
so the drivers' statics are per board, and a driver takes only a board no other driver process
has claimed (`driver_find` / `driver_claim`: `cd_Driver` of its ConfigDevs, given back when the
driver process ends). On Auto the second board is the other supplied driver's, else a second
copy of the first one's driver.

Either board can be the primary - the one with the `PRISM:` modes, so Workbench: `PRIMARY=SECOND`
(PrismPrefs: Board "Auto, swapped") swaps the two after loading them (their `PrismBoard`
records and driver slots; the drivers keep no pointer to those). `PrismSem.boardAddr[]` gives
each board's expansion address, so two of one kind can be told apart.

## The second monitor (SECONDMONITOR=, from 1.3 beta 1)

Until 1.3 a second board with no screen of its own went back to the Amiga's video, and with
Workbench on the primary that is an empty chipset display: a black monitor. Two monitors
should both show something, so the second board has a setting of its own (PrismPrefs' "2nd
monitor" cycle, under Board):

- `SCREEN` (the default): PrismD opens a public screen, `PRISM2`, on the second board behind
  every other screen, in its 256-colour mode of the primary's size where it has one, so the
  monitor is never black and windows and programs can be put there (`PUBSCREEN=PRISM2`, or a
  program's own screen on a `PRISM2:` mode, which then comes in front of it as usual). Closed
  when PrismD quits, not while windows are on it.
- `MIRROR`: the second board shows a copy of what the first one shows, in its mode of the
  same size and depth (none: nothing is mirrored). The first board's picture is never read
  back to find what changed: drawing into the bitmap on display marks rectangles through the
  hooks every drawing path already passes (`SW_CLEAR`, `LOCK_FOR`), and every tick copies up
  to 64 KB of them across, converting the pixels where the boards store them differently. A
  256-colour copy gets the first board's palette. Not copied yet: the pointer, and the
  compositor's frames (dragging, PIPs).
- `SPAN` (from 1.3 beta 2; PrismPrefs "Span right"/"Span left"): one Workbench across both
  monitors. A span mode on the primary is twice the width of a mode both boards have; the
  primary shows its half of the bitmap, the second board a copy of the other half
  (`SECONDSIDE` picks which), and the pointer crosses to the board it is over. 8 and 16 bits
  only.
- `OFF`: as before - the second monitor shows the Amiga's own video while nothing is on it.

How `SPAN` is built (prismd.c, pointer.c):

- Modes: after both boards are up, `build_span_modes()` adds, for every primary mode of 1 or 2
  bytes a pixel that the second board also has, a mode of twice the width (`checkMode` with
  that pitch) whose ModeID is the base one with `$80` in the size byte (`SPAN_ID`), so
  `$7A000102` (800x600 16-bit) gives `$7A000182`. `ModeRec.spanHalf` is the half width.
  PrismSetup `WBID=$0182` writes such a mode for Workbench.
- The primary: the screen is shown like an oversized one (`span_place`: autoscroll on, the
  display start `rx` = the half when the second monitor is on the left, else 0) in its mode
  of the half width; `prism_autoscroll` leaves span screens alone, so the view never scrolls.
- The second board: the mirror's copy with a source offset (`mon2X`). Drawing marks up to 16
  rectangles (merged when that copies at most 1024 pixels more, or when all 16 are used): a
  window frame dragged in outline is eight thin lines a move, and one rectangle around them
  would be the whole window. Each tick copies up to 64 KB of them; where the boards differ
  only in byte order (a ZZ9000's 565 big-endian, a Cirrus's 565 little-endian) two pixels a
  longword are swapped, otherwise `pf_get`/`pf_put` per pixel.
- The pointer: `prism_span_card(x)` names the board whose half `x` is in, `pointer_tick`
  moves the pointer to it, and `prism_span_origin` places the card's cursor in that half.
  Edge switching between the boards is off while a span screen is in front.

The pointer pushed past the edge facing the other monitor (24 mouse counts, no button held,
so a dragged window or icon stays) brings the other board's front screen forward and puts the
pointer on its near edge at the same height; back the same way. `SECONDSIDE=LEFT` when the
second monitor stands left of the first. PrismD's input handler asks; the main loop does it
(`ScreenToFront` and a `NEWPOINTERPOS` event).

## Not in this first version

Moving a screen from one board to the other while it is open.
