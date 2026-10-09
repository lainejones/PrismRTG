# Writing a driver for a new board

A PrismRTG driver is one C file that knows one board: how to find it, set
a mode, say where the screen starts, and (if it has one) drive its blitter
and sprite. PrismD does everything else. A driver is built into its own
loadable module, `LIBS:Prism/NAME.driver`, and PrismD picks it up without
any change to PrismD, PrismPrefs or the installer.

This is the whole job, in the order that works:

## 1. Copy the template

```
cp src/drv_template.c src/drv_myboard.c
```

`src/drv_template.c` is a complete driver with `TODO` marks at each
place that depends on the hardware. It builds and loads as it is; built
with `-DTEMPLATE_FAKE` it even runs PrismD on a megabyte of fast RAM, which
is how the contract below is checked by the tests.

## 2. The minimal driver

Fill in, in `driver_probe()`:

| Field | What |
|---|---|
| `name` | shown by PrismD, PrismPrefs and PrismProbe |
| `regs`, `vram`, `vramSize` | where the registers and the frame buffer are (from the autoconfig node; `PrismProbe` lists every board with its manufacturer/product numbers) |
| `formats` | the pixel layouts the board scans out, as `PF_BIT()`s of `enum PrismFormat` (prismboard.h). PrismD offers 8/16/24/32-bit screens from these |
| `maxWidth`, `maxHeight` | the largest mode |
| `setMode` | program a mode; fill `m->bytesPerRow` |
| `setDisplayStart` | point the scan-out at a VRAM offset |

Everything else may stay NULL to begin with. `board_defaults()` supplies
`checkMode` (any size up to the maximum, unpadded rows), and does-nothing
versions of `setPalette`, `setSwitch`, `waitVBlank` and `shutdown`. Fill
them in when the board needs them: a palette for 8-bit modes, the monitor
switch for a board with a pass-through, a vertical-blank wait for
tear-free screen changes.

Pixel formats are the bytes as the 68k sees them in VRAM. If the board's
rows are padded (a pitch of a power of two, say), provide `checkMode` and
set `bytesPerRow` there, or `ops->pitch` when the padding depends on the
exact format.

## 3. Build it

Inside WSL (or wherever amiga-gcc is):

```
sh tools/mkdriver.sh MYBOARD src/drv_myboard.c
```

makes `out/Drivers/MYBOARD.driver`. The name is what `BOARD=` will use;
keep it to letters and digits, 32 at most.

## 4. Test it without PrismD

`PrismTest` loads a driver on its own, sets a mode, draws a test pattern
and hands the display back. Copy `MYBOARD.driver` to `LIBS:Prism/` (or to
a `Drivers` drawer next to PrismTest) and, with PrismD not running:

```
PrismTest BOARD=MYBOARD DEPTH=8
PrismTest BOARD=MYBOARD DEPTH=16 WIDTH=800 HEIGHT=600
```

The pattern is hue bars, a grey ramp and a checkerboard: a wrong pixel
format shows as wrong colours, a wrong pitch as a sheared picture. Once
the picture is right at every depth the board offers, the driver works.

## 5. Run PrismD on it

```
Run >RAM:PrismD.log C:PrismD BOARD=MYBOARD LOG
```

or `BOARD=MYBOARD` in `ENV:Prism.prefs` (PrismPrefs shows it as "Other").
With the board on Auto, PrismD tries its own drivers first and then every
other `.driver` in `LIBS:Prism`, so once the file is installed nothing
needs selecting. Open a screen on a `PRISM:` mode and run the PrismCheck
kit: `tmpltest` and `p96test` must report 0 wrong, `PrismBench` gives the
speed.

The installer copies every file in the package's `Drivers` drawer, so a
driver added there is installed with the rest.

## 6. The blitter (optional)

Add a `PrismOps` table (boardops.h) with `fill`, `copy`, `expand` (text)
and `line` as the engine supports them. The rules:

* Each call gets the destination (and source) as a `PrismSurface`: VRAM
  offset, pitch, format, size. The rectangle is already clipped.
* Return `PR_DECLINED` for anything the engine cannot do (a pitch it
  cannot encode, a format it does not know): nothing was written, and
  PrismD draws it on the CPU. Return `PR_DONE` when the pixels are in VRAM.
* `PR_RETRY` means the operation failed after starting and the engine is
  stopped: PrismD redraws it on the CPU and keeps using the others.
  `PR_FAILED` means the engine is wedged: PrismD stops using it.
* Return only when the blit has finished, or provide `waitBlit`, which
  PrismD calls before the CPU touches VRAM.

`PrismTest BOARD=MYBOARD BLIT` draws fills and copies through the table
and compares every byte with the CPU's result. Then the direct hooks
(`fillRect`, `copyRect`, `copyBetween`, `expandRect` and the `blitMax*`
limits) are a second, faster path for small blits; add them last, with
the same checks.

## 7. The sprite (optional)

`cursorImage`, `cursorShow` and `cursorMove` with `PBF_HW_CURSOR` in
`flags`. The image is 64x64 bytes with pens 0-3 and three R,G,B colours.
Without them PrismD draws a software pointer, which costs a few percent.

## 8. Ship it

* Say in the ReadMe what it was tested on. A driver that has only run in
  an emulator is marked "emulation only" until someone confirms it on
  the real card.
* `PrismProbe` and `PrismSetup DETECT` know the supplied boards by their
  autoconfig numbers; a new one goes into `tools/prismsetup.c`'s table so
  the installer greets it by name (until then "Install anyway" works).
* Everyone whose code or findings go in is named in the Credits.

## Where to look

* `src/prismboard.h` - the whole driver interface, commented.
* `src/boardops.h` - the operation table and result rules.
* `src/drv_zz9000.c` - a complete native driver with blitter and sprite
  (680 lines); `src/drv_picasso2.c` - a Cirrus VGA chip with a mode table.
* `docs/driver-architecture.md` - how PrismD uses the driver: surfaces,
  VRAM paging, shadow bitmaps, the software pointer.
* `docs/driver-modules.md` - the module protocol and ABI checks.
