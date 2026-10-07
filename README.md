# PrismRTG

Graphics card (RTG) software for AmigaOS 3.x, written from scratch. PrismRTG
puts Workbench and programs on a graphics card in 256 colours, 16-bit and
24/32-bit, drawn by the card's blitter, with a hardware mouse pointer and a
its own built-in `cybergraphics.library` and `Picasso96API.library` for programs
that ask for one (both written for PrismRTG: nothing of CyberGraphX or Picasso96
is used or shipped).

It was written so that its author has an RTG system he can maintain himself:
when something is wrong it can be fixed the same day. It is standalone - it
uses, needs and includes nothing of Picasso96 or CyberGraphX - and it is free
software under the GPL. It takes the place of Picasso96 on the Amiga it runs
on.

![PrismPrefs on a PrismRTG Workbench (A4000, ZZ9000)](docs/a4000-prismprefs.png)

## Supported cards

| Card | Chip | Tested on | Status |
|---|---|---|---|
| MNT ZZ9000, Zorro III | firmware 2.8 | A4000, 68060 | Real hardware. Blitter fills, copies, text, lines; hardware pointer. 8/16/32-bit up to 1280x1024. |
| GBAPII++ (Picasso II remake) | Cirrus GD5434, 2 MB | A2000, 68030 | Real hardware. Blitter fills, copies, text (8/16/32-bit); hardware pointer. 8/16-bit up to 1024x768, 24-bit 640x480, 32-bit 800x600. |
| Village Tronic Picasso II / II+ | Cirrus GD5426/28 | WinUAE only | Emulation only: never run on a real card. 8/16-bit, 24-bit 640x480. |
| Piccolo SD64, Zorro II | Cirrus GD5434 | WinUAE only | Emulation only. 8/16/24/32-bit, blitter as on the GBAPII++. |
| Piccolo, Zorro II | Cirrus GD5426 | WinUAE only | Emulation only. 8/16-bit, 24-bit 640x480. |
| GVP Spectrum 28/24, Zorro II | Cirrus GD5428 | WinUAE only | Emulation only. 8/16-bit, 24-bit 640x480. |
| MNT ZZ9000, Zorro II mode | | none | Untested. No blitter or pointer support in this mode. |

Needs AmigaOS 3.0 or newer and a 68020 or better. No FPU needed.

## How small a machine

PrismD, the driver, takes about 310 KB, in fast RAM when there is any and
none of it in chip RAM. Measured in WinUAE on an emulated Picasso II+ (2 MB
on the card), with Workbench 3.2 on a PrismRTG 800x600 16-bit screen and MUI
and a TCP/IP stack loaded, each program started on a freshly booted machine:

| Machine | Free after booting | What ran |
|---|---|---|
| 68030 28 MHz, 2 MB chip + 20 MB fast | 18 MB fast | Everything tried, apart from ScummVM and OpenTTD (want more memory) |
| 68030, 2 MB chip + 4 MB fast | 2.4 MB fast | MysticView, Personal Paint, Directory Opus, IBrowse, AmigaAMP; YAM, AWeb, NetSurf and ADoom found too little memory and said so |
| 68030, 2 MB chip + 2 MB fast | 440 KB fast | MysticView, Directory Opus |
| 68030, 2 MB chip + 1 MB fast | 1.4 MB chip | MysticView, Directory Opus; drawing half as fast (screens that don't fit on the card are kept in chip RAM) |
| 68030, 2 MB chip, no fast RAM | 365 KB chip | Workbench, nothing beside it |
| 68020 or 68EC020 14 MHz, 2 MB chip + 4 MB fast | 2.4 MB fast | The same as the 68030 with 4 MB; drawing about half as fast |

Every PrismRTG test passed on each machine that had room to run it. Short of
memory, programs refused or stopped with a message. The one exception was a
68020 with no fast RAM, where with about 120 KB left a program hung now and
then; nothing pointed at PrismRTG.

Screens that don't fit on the card are kept in RAM, and moving one there
needs RAM for all of it: about 940 KB for 800x600 at 16 bits. With a 2 MB
card, an 800x600 16-bit Workbench and its backdrop picture fill the card, so
a second screen needs about 1 MB free in one piece; an 8-bit Workbench needs
half of that.

## What works

* PrismRTG modes appear in Prefs/ScreenMode and ASL requesters (`PRISM:800x600 16bit` ...).
* Workbench 3.2 on the card at 8, 16, 24 and 32 bits, with colour icons.
  Both the OS's own `icon.library` and PeterK's `icon.library` 51.4 are
  used daily: the two real machines run PeterK's (which keeps its work
  bitmaps in fast RAM - PrismRTG draws into those itself), the emulated
  test machines the OS's.
* graphics.library drawing into PrismRTG screens, checked pixel-for-pixel
  against the native chipset (`PrismBench`: fills, lines, text, planar blits).
* cybergraphics.library 41 API: MultiView, AWeb and Amelinium show
  true-colour pictures.
* Picasso96API.library 2 (PrismRTG's own): bitmaps in any pixel format, mode
  lists and requester, screens, pixel arrays in every direct-colour format,
  board data. `p96test` checks every call at 8, 16 and 24 bits. No
  picture-in-picture windows.
* Screens that don't fit in video memory page out to fast RAM; double
  buffering through `AllocScreenBuffer` / `ChangeScreenBuffer`.
* `Prefs/PrismPrefs`: which modes are offered, refresh rates, board,
  blitter on/off, red and blue swapped in 256-colour palettes, the debug
  log (off, on, or written at once for a machine that dies while starting),
  and a **Test** button that shows a mode for ten seconds and comes back by
  itself.
* Boot failsafe: hold the left mouse button while booting and PrismRTG does
  not start; a start that hangs is skipped automatically on the next boot.

Neither card lets software read what the monitor supports (the ZZ9000
firmware has no DDC code; the GBAPII++ does not wire the chip's DDC data
pin), so there is no automatic monitor detection: use the Test button.

## Speed against Picasso96

Same machine, same card, same benchmark (`PrismBench`), operations per
second, higher is better. Picasso96 is rtg.library 43.760 with each card's
own driver. Run-to-run noise is about 5%.

### A4000, 68060, ZZ9000 - 800x600, 8-bit

PrismRTG 1.0 as released (5 October 2026); Picasso96 measured on the same
machine two days earlier.

| Test | Picasso96 | PrismRTG 1.0 |
|---|---|---|
| RectFill 100x100 | 19,933 | **22,046** |
| RectFill 8x8 | 26,148 | **35,484** |
| Text, 40 characters | 4,894 | **5,387** |
| Text, 4 characters | 9,844 | **15,433** |
| ClipBlit 200x100 | 5,493 | **8,695** |
| ScrollRaster 400x100 | 5,520 | **11,149** |
| Line, ~390 pixels | 18,134 | **26,761** |
| WritePixel | 92,941 | **117,717** |
| Planar blit 64x32 (icons) | 241 | **1,007** |
| Bitmap-to-screen blit 64x32 | 11,178 | **24,271** |

16-bit and 32-bit show the same pattern (WritePixel at 32-bit: 92,875 /
99,498). Screen flips (`ScreenToFront` between two PrismRTG screens) run at
25-33 a second against 50 for two native screens: one extra frame per flip.

### A2000, 68030 at 50 MHz, GBAPII++ - 640x480

Measured on 4 October 2026 with the build of that day (the A2000 has not run
the released build yet).

| Test | P96 8-bit | **PrismRTG 8** | P96 16-bit | **PrismRTG 16** |
|---|---|---|---|---|
| RectFill 100x100 | 3,902 | **3,987** | 2,865 | 2,807 |
| RectFill 8x8 | 5,094 | **6,765** | 4,918 | **6,475** |
| Text, 40 characters | 1,343 | **1,516** | 1,280 | **1,478** |
| Text, 4 characters | 2,678 | **3,665** | 2,619 | **3,470** |
| ClipBlit 200x100 | 1,107 | **1,408** | 652 | **730** |
| ScrollRaster 400x100 | 698 | **819** | 383 | **409** |
| Line, ~390 pixels | 1,805 | **2,007** | 1,488 | **1,945** |
| WritePixel | 14,646 | **23,480** | 14,206 | **22,773** |
| Planar blit 64x32 (icons) | 306 | **361** | 160 | **173** |
| Bitmap-to-screen blit 64x32 | 2,378 | **4,125** | 2,025 | **3,249** |

True colour on the GBAPII++ comes in two formats, and they trade off:

| Test | P96 "24" | PrismRTG 24-bit | PrismRTG 32-bit |
|---|---|---|---|
| RectFill 100x100 | 1,001 | 786 | **1,485** |
| Text, 40 characters (JAM1) | 416 | 279 | **1,307** |
| Text, 40 characters (JAM2) | 214 | 201 | **1,296** |
| ClipBlit 200x100 | 406 | **430** | 304 |
| ScrollRaster 400x100 | 213 | 188 | 162 |
| WritePixel | 13,718 | **19,158** | **18,730** |
| Planar blit 64x32 (icons) | 108 | 103 | **120** |

The chip's blitter can fill and draw text at 32 bits but not at 24, and it
copies about the same number of bytes per second in either format. So
32-bit is much faster for text and fills, and 24-bit is about a quarter
faster for copies and scrolling because each pixel is smaller. Both are
offered; choose in Prefs/ScreenMode. Picasso96 was measured in its 24-bit
mode only.

## Software it has been seen running

Started one after another on an emulated Picasso II+ with Workbench on a
PrismRTG screen at 16-bit and again at 256 colours, each screen captured
and looked at (`docs/design.md` has the full list and what was found):
AWeb, IBrowse, NetSurf, Amelinium, AmIRC, YAM, SimpleMail, WookieChat,
Directory Opus 4, MultiView, MysticView, Personal Paint 7, ArtEffect 4,
AmigaAMP, AMPlifier, HippoPlayer, Eagleplayer, FileX, EvenMore, CharMap,
Lupe, HomeBank, MUIbase, MakeCD, FryingPan, Amiga Chess RTG, AmiArcadia,
ArTKanoid, WBsteroids, MagicNumbers, dynAMIte, iGame, Ballfield, ADoom,
DoomAttack, AHeretic, Hexen, the Joyride demo, Visage, CyberAVI, P96Speed.
On the real A4000: CgxBenchmark, OpenDUNE, the KillerCGX demo, Amelinium.

## Install

Unpack the archive, open the `PrismRTG` drawer and double-click
**Install_PrismRTG** (the standard Amiga Installer). It

* finds the card,
* looks for an installed Picasso96 - its monitor files in `DEVS:Monitors`
  and its `Picasso96API.library` - lists what it found and asks:
  **back it up** (the files are moved to `SYS:Storage/Picasso96-parked`
  with a note of where each came from) or **leave it** (nothing is
  touched, and PrismRTG stays off). Nothing of Picasso96 is ever deleted,
* asks which PrismRTG screen Workbench should open on.

![The installer's Picasso96 question](docs/installer-picasso96.png)

Reboot. Run Install_PrismRTG again to update or to remove PrismRTG; Remove
moves Picasso96 back out of the backup drawer and puts the previous screen
mode back.

PrismRTG and Picasso96 cannot both be active on one Amiga: with Picasso96's
monitor files still in `DEVS:Monitors` (or its `rtg.library` in memory),
PrismD prints why and does not start. The product is called PrismRTG; its
files keep their short names (`PrismD`, `PrismPrefs`, the monitor file
`Prism`, the `PRISM:` screen modes).

| Installed file | What it is |
|---|---|
| `C:PrismD` | the driver, started at boot |
| `DEVS:Monitors/Prism` | starts PrismD before Workbench opens |
| `SYS:Prefs/PrismPrefs` | settings editor |
| `C:PrismSetup`, `C:PrismProbe` | installer helper, board lister |
| `ENVARC:Prism.prefs` | the settings (plain text) |

If the screen stays dark: hold the left mouse button during boot, then use
PrismPrefs to switch a mode off, change its refresh rate or turn the
blitter off.

## Known limits

* Tested on two machines by one person. Treat it as early software.
* No screen dragging. No monitor detection (see above).
* The Picasso II, Piccolo, Piccolo SD64 and Spectrum have only run in WinUAE.
  A real card can differ from its emulation (the GBAPII++ did, in three
  places). If 256-colour screens on a real Piccolo or Spectrum show red and
  blue exchanged, tick **Swap red/blue** in PrismPrefs.
* The installer's Remove path has been run in the emulator, not yet on a
  real machine.
* On the A2000 one unexplained crash was seen shortly after a cold boot;
  the boot failsafe recovered it and it has not been reproduced.

## Build

Needs amiga-gcc (Bebbo) under WSL or Linux:

```sh
./build.sh
```

Output goes to `out/`, the release drawer to `out/pkg/PrismRTG`.
Packaging also needs Python 3. Icons require the external `iconlib.py`,
`makeicon_doc.py`, `makeicon_drawer.py` and `makeicon_install.py` tools;
set `TOOLS` to their directory when running `./build.sh`. Without these
tools, the release drawer is staged without icons.

| Tool | What it does |
|---|---|
| `PrismD` | The driver. `PrismD [BOARD=ZZ9000|PICASSO2] [PREFS=file] [LOG]`; Ctrl-C removes its patches when no PrismRTG screen is open. |
| `PrismPrefs` | Settings editor. `FROM= USE SAVE TEST=<slot>` work without the window. |
| `Prism` (`out/PrismMon`) | `DEVS:Monitors` launcher with the boot failsafe. |
| `PrismSetup` | Installer helper: `DETECT`, `WBMODE=<slot>`. |
| `PrismProbe` | Lists expansion boards and marks the ones PrismRTG can drive. Read-only. |
| `PrismBench` | Benchmark and pixel-exactness checks: `MODEID=$7A00xxxx`, `LIST`. |
| `PrismTest` | Bare-metal board test: sets a mode, draws a pattern, checks the blitter against the CPU (`BLIT`). |
| `PrismScreen`, `PrismShow`, `PrismMouse`, `prismdiag` | Test clients: screens, pictures, pointer, display database dump. |
| `ddcprobe`, `zztime`, `iotime`, `memwhere`, `p2peek` | Hardware probes and timing tools. |

[docs/design.md](docs/design.md) has the architecture, how each part was
built, and every measurement; [docs/zz9000-hw.md](docs/zz9000-hw.md) and
[docs/cirrus-picasso2-hw.md](docs/cirrus-picasso2-hw.md) are the hardware
notes the drivers were written from.

## Licence

GNU General Public License, version 3 (GPL-3.0-only). Copyright (c) 2026 Laine Jones.
See [LICENSE](LICENSE) and [CONTRIBUTING.md](CONTRIBUTING.md).
