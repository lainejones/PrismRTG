# PrismRTG

Graphics card (RTG) software for AmigaOS 3.x, written from scratch. PrismRTG
puts Workbench and programs on a graphics card in 256 colours, 16-bit and
24/32-bit, drawn by the card's blitter, with a hardware mouse pointer and
its own built-in `cybergraphics.library` and `Picasso96API.library` for programs
that ask for one (both implemented by PrismRTG).

It was written so that its author has an RTG system he can maintain himself:
when something is wrong it can be fixed the same day. Its native drivers
need no Picasso96 or CyberGraphX installation, and it is free software
under the GPL. It takes the place of Picasso96 on the Amiga it runs
on. An optional P96 card-driver adapter can reuse existing `.card` libraries;
the native drivers do not need them.

![PrismPrefs on a PrismRTG Workbench (A4000, ZZ9000)](docs/a4000-prismprefs.png)

## Supported cards

| Card | Chip | Tested on | Status |
|---|---|---|---|
| MNT ZZ9000, Zorro III | firmware 2.8 | A4000, 68060 | Real hardware. Blitter fills, copies, text, lines; hardware pointer. 8/16/32-bit, tested up to 1280x1024 (1920x1080 is offered too, and up to 2560x1440 with the larger sizes switched on). Monitor sleep from firmware 2.7, video overlay from 2.8. |
| GBAPII++ (Picasso II remake) | Cirrus GD5434, 2 MB | A2000, 68030 | Real hardware. Blitter fills, copies, text (8/16/32-bit); hardware pointer. 8/16-bit up to 1024x768, 24-bit 640x480, 32-bit 800x600. |
| Z3660 accelerator (A4000), RTG output | firmware 1.03+, `autoconfig_rtg YES` | A4000, 68060 | Real hardware. 8/15/16-bit in the firmware's fixed modes, 32-bit up to a 108 MHz pixel clock (1280x1024, 1600x900), 320x200/240 doubled; hardware pointer, the monitor switch (`monitor_switch` in z3660cfg.txt, or `ENV:Z3660-MONSWITCH`). Blitter fills, copies, text, lines, planar blits. |
| Village Tronic Picasso II / II+ | Cirrus GD5426/28 | WinUAE only | Emulation only: never run on a real card. 8/16-bit, 24-bit 640x480. |
| Ateo Concepts Graffity (Zorro II and III) | Cirrus GD5428 | WinUAE only | Emulation only, from WinUAE's description of the board. Same as the Picasso II+. |
| Piccolo SD64, Zorro II | Cirrus GD5434 | WinUAE only | Emulation only. 8/16/24/32-bit, blitter as on the GBAPII++. |
| Piccolo, Zorro II | Cirrus GD5426 | WinUAE only | Emulation only. 8/16-bit, 24-bit 640x480. |
| GVP Spectrum 28/24, Zorro II | Cirrus GD5428 | WinUAE only | Emulation only. 8/16-bit, 24-bit 640x480. |
| Village Tronic Picasso IV, Zorro II and III | Cirrus GD5446, 4 MB (2 MB used) | WinUAE only | Emulation only, with WinUAE's model of the card (it needs the card's flash ROM). Native driver from 1.1 beta 8: blitter fills, copies, text (8/16/32-bit); hardware pointer; 8/16-bit up to 1280x720, 24-bit 640x480, 32-bit 800x600. |
| MNT ZZ9000, Zorro II mode | | none | Untested. No blitter or pointer support in this mode. |
| Apollo Vampire (SAGA), through its P96 driver | `vampiregfx.card` 1.61 | Vampire, reported by a user (#1) | Real hardware, not tested here. 1.1 beta 3 with `BOARD=P96`: every drawing test passes at 8, 16, 24 and 32 bits, `p96test` 0 of 29 failed, screens up to 1280x720 24-bit. One failure, planar blits at 16 bits, is handled from beta 5 (the card's own planar blit is checked first and replaced if wrong). |

Needs Kickstart 3.1 or newer (the Workbench disks may be 3.0) and a 68020 or
better. No FPU needed. On Kickstart 3.0 PrismD does not start.

## How small a machine

PrismD, the driver, takes about 330 KB, in fast RAM when there is any and
none of it in chip RAM; since the board drivers became separate modules
(after 1.1 beta 2) it is about 40 KB more, and the buffers that are only
allocated when used (after beta 3) take 35 KB back. Measured in WinUAE on an emulated Picasso II+ (2 MB
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

### Loadable board drivers

PrismD loads the selected backend from `LIBS:Prism/<name>.driver`, with
`PROGDIR:Drivers` as a fallback for running a build in place. The installer
copies the supplied PICASSO2, ZZ9000, Z3660, P96 and UAEGFX modules. From 1.1
beta 8 the driver interface is frozen: a module built for one release keeps
loading in every later one, and only a module newer than PrismD is refused
("doesn't fit this PrismD"). Earlier modules (protocol 3) need rebuilding once.

A driver for another board is one C file built with
`sh tools/mkdriver.sh NAME src/drv_name.c`; `BOARD=NAME` selects it, and
with the board on Auto PrismD tries every other `.driver` in `LIBS:Prism`
after its own. The minimal driver is a mode set and a display start;
`src/drv_template.c` is the starting point,
[docs/writing-a-driver.md](docs/writing-a-driver.md) the guide, with
`PrismTest` to check a driver before PrismD runs on it, and
[docs/driver-development.md](docs/driver-development.md) the full
contract and how to submit a finished driver.

Each loaded module uses its own 16 KB process stack and keeps its callbacks
alive until PrismD stops. The memory measurements above describe the
previous linked build. P96/UAE contexts still stay resident when their card
requires it, while PrismD itself can exit. See
[the module interface and lifetime](docs/driver-modules.md).

### Existing P96 card drivers

The experimental `BOARD=P96` backend loads a P96 `.card` driver directly,
without loading P96's `rtg.library` or monitor program. For example, UAE's
resident driver can be selected with:

```text
Run >RAM:PrismD.log PrismD BOARD=P96 P96CARD=uaegfx.card
```

For boot-time use, set `BOARD=P96` and `P96CARD=uaegfx.card` in
`ENVARC:Prism.prefs` and `ENV:Prism.prefs`. Physical-card drivers normally
use a path such as `LIBS:Picasso96/CyberVision64.card`. Leave any required
chip driver installed as well. The existing P96 monitor programs must
remain disabled, as with native Prism drivers.

See [P96 driver configuration and limits](docs/p96-drivers.md) before
using this backend. It supports linear framebuffers, not every P96 memory
model, and a claimed driver remains resident until reboot. This provides
uaegfx through its P96 driver. A separate native candidate is available below.
Tested here with UAE's `uaegfx.card` in WinUAE 6.0.3 at 8, 16 and 32 bits
(every PrismRTG test and `p96test`). The first physical card reported is
the Apollo Vampire with `vampiregfx.card` (see the card table).

### Native UAE RTG candidate

`BOARD=UAEGFX` selects the native candidate, which binds UAE's resident
RTG host interface directly without loading a `.card` file or using the
P96 adapter:

```text
Run >RAM:PrismD.log PrismD BOARD=UAEGFX
```

Enable UAE RTG memory; PrismRTG uses its hardware cursor when it is on and draws its own pointer when it is off. This candidate is selected
explicitly; see [native UAE configuration and limits](docs/uaegfx.md).
Tested in WinUAE 6.0.3 at 8, 16 and 32 bits (every PrismRTG test and
`p96test`).

### In emulators

| Emulator, card | Board in PrismPrefs | Status |
|---|---|---|
| WinUAE, Picasso II / II+, Piccolo, Piccolo SD64, Spectrum, Graffity, Picasso IV (Zorro II and III where the card has both) | Auto | Tested: every PrismRTG test passes on each; the figures per board are in [docs/emulated-cards.md](docs/emulated-cards.md) |
| WinUAE 6.0.3, UAE RTG Zorro III or Zorro II | UAE (native), or P96 driver with `P96CARD=uaegfx.card` | Tested at 8, 16 and 32 bits: every test passes |
| Amiberry 8.4.0 (Linux), UAE RTG | UAE (native) | Tested at 8, 16 and 32 bits: every test passes, with `rtg_nocustom=false` too. (8.3.0 lost some of what was drawn on 32-bit screens with `rtg_nocustom=false` and ZeroCopy on, [amiberry#2392](https://github.com/BlitterStudio/amiberry/issues/2392); fixed in 8.4.0) |
| Amiberry 8.4.0 (Linux), ZZ9000 (Zorro III) | Auto | Tested: Workbench and every PrismRTG test pass, from beta 6 on (earlier PrismD builds switched the display on and off inside one frame at start-up, which 8.4.0 does not survive) |
| Amiberry inside RetroArch (libretro) | UAE (native) | Not tested. The libretro build has no RTG hardware pointer, so PrismRTG draws its software pointer (tested with WinUAE's hardware sprite turned off) |
| FS-UAE 3.1.66 (Linux), UAE RTG | UAE (native), or P96 driver with `P96CARD=uaegfx.card` | Tested at 8, 16 and 32 bits: every test passes, both ways. FS-UAE 3.1 cannot open files that are read-only on the host, so a system folder shared from Windows with read-only files will not boot until the attribute is cleared |
| MiSTer (FPGA), ZZ9000 core | Auto (ZZ9000) | Reported working with beta 3 (issue #2). No pointer appears by itself: tick "Soft pointer" in PrismPrefs (`SOFTWAREPOINTER=ON`) and reboot (confirmed on a MiSTer). At 8 bits the core's text blit put glyphs a pixel or more to the left, probably the odd spacing seen in the Shell; from beta 5 on PrismRTG checks each board's text blit at start-up and draws text on the CPU where it comes out wrong (not yet run on a MiSTer). At 16 bits most drawing checks failed in that beta 3 report; no later report yet |

On UAE's RTG card PrismRTG uses the emulator's hardware pointer ("Hardware
sprite emulation" in the RTG settings) when it has one: WinUAE has it on by
default, and so has Amiberry from 8.4.0 on (8.3.0 has it off). Without it,
PrismRTG draws its software pointer instead.

## What works

* PrismRTG modes appear in Prefs/ScreenMode and ASL requesters (`PRISM:800x600 16bit` ...).
* Workbench 3.2 on the card at 8, 16, 24 and 32 bits, with colour icons.
  Both the OS's own `icon.library` and PeterK's `icon.library` 51.4 are
  used daily: the two real machines run PeterK's (which keeps its work
  bitmaps in fast RAM - PrismRTG draws into those itself), the emulated
  test machines the OS's.
* graphics.library drawing into PrismRTG screens, checked pixel-for-pixel
  against the native chipset (`PrismBench`: fills, lines, text, planar blits).
* cybergraphics.library 43 API: MultiView, AWeb and Amelinium show
  true-colour pictures. The newer calls are there too (1.1 beta 8):
  `WritePixelArrayAlpha` (ARGB pixels blended in by their alpha, times a
  global alpha), `BltTemplateAlpha` (the pen's colour through an 8-bit alpha
  mask: anti-aliased text) and `ProcessPixelArray` (brighten, darken, tint,
  blur, grey, negative, the two fades, gradients, set alpha, RGB shift).
  `cgxtest` checks them at 8, 16 and 32 bits; on a 256-colour screen the
  results go to the nearest pens.
* Picasso96API.library 2 (PrismRTG's own): bitmaps in any pixel format, mode
  lists and requester, screens, pixel arrays in every direct-colour format,
  board data, and picture-in-picture windows (through the compositor, see Known limits).
  `p96test` checks every call, at 16 bits unless given `DEPTH=8`, `24` or `32`.
* Screens that don't fit in video memory page out to fast RAM; double
  buffering through `AllocScreenBuffer` / `ChangeScreenBuffer`.
* `Prefs/PrismPrefs`: which modes are offered, refresh rates, board,
  blitter on/off, red and blue swapped in 256-colour palettes, the debug
  log (off, on, or written at once for a machine that dies while starting),
  and a **Test** button that shows a mode for ten seconds and comes back by
  itself; also screen promotion, your own sizes, gamma, the software pointer,
  dragging, video on the board, Sleep, Mirror chipset and the update check
  (described in this section and under Known limits).
* Boot failsafe: hold the left mouse button while booting and PrismRTG does
  not start; a start that hangs is skipped automatically on the next boot.
* **Two boards at once** (1.1 beta 6, emulator-tested so far): with a ZZ9000
  and a GBAPII++ (or Picasso II) in one machine, each on its own monitor,
  PrismRTG drives both. The first board's modes are `PRISM:...` as before,
  the second's `PRISM2:...` (ModeIDs `$7A01xxxx`); a program or ScreenMode
  picks the board by picking a mode. Each board shows its own front screen,
  the pointer is on the board of the screen in front, and dragging works on
  both. Any two of the supported cards work, two of the same kind too. Either
  can be the primary (the `PRISM:` modes, so Workbench): Board `Auto, swapped`
  in PrismPrefs (`PRIMARY=SECOND`) makes the second board found the primary -
  the status line names each board and its expansion address. `Auto, 1 board`
  (`SECONDBOARD=OFF`) keeps to one board; with two, the Test button asks which
  board shows the test picture.
  Design: `docs/multi-board.md`.
* **Monitor power saving** (1.1 beta 8; "Sleep" in PrismPrefs, `POWERSAVE=10`
  in the prefs file, minutes, 0 = never, the default): after that long
  without a key or mouse event every board showing a picture goes dark, and
  the next key or mouse move brings it back. On the Picasso II, GBAPII++,
  Piccolo, SD64 and Spectrum the card turns its picture and both syncs off,
  so a DPMS monitor goes to sleep, and so does the ZZ9000 with firmware 2.7 or
  newer (the firmware's DPMS); on the others (Picasso IV, UAE, P96 cards, older
  ZZ9000 firmware) PrismD shows black (a black palette at 256 colours, a cleared bitmap
  deeper) and the monitor stays on. A program flipping its buffers keeps it
  dark; a mode change wakes it. PrismPrefs' Use or Save takes effect at once.
* **Chipset screens on the board** (1.1 beta 8; "Mirror chipset" in PrismPrefs,
  `MIRRORCHIPSET=ON`, off by default): a chipset screen in front that promotion
  left alone - a program with its own bitmap, one that writes the bitplanes
  itself, or one on the Keep list - stays a real chipset screen, and the primary
  board shows a copy of it, so it can be used on a card-only monitor. A lores
  screen is shown at twice its size where a mode holds that (320x256 on 800x600).
  PrismD converts the bitplane rows that changed every tick, follows the palette
  (EHB too) and double buffering, and puts the pointer on the copy. HAM and dual
  playfield screens stay on the chipset; copper colour changes within a frame and
  sprites other than the pointer are not copied. Unlike Sleep, the switch is read
  when PrismD starts: after Use or Save in PrismPrefs it takes effect at the next
  reboot (PrismPrefs' status line says so).
* **ARexx port `PRISM`** (1.1 beta 8, needs RexxMast): `VERSION`, `BOARDS`,
  `BOARD`, `VRAM`, `MODES`, `MODE <n>`, `MODEID <w> <h> <bits>`,
  `TEST <w> <h> <bits> [hz]` / `TESTEND` (the Test picture), `POWERSAVE
  [minutes]`, `SLEEP`, `WAKE`, `DARK`, `LOG [ON|OFF]`, `HELP`; a trailing `2`
  asks about the second board. [docs/arexx.md](docs/arexx.md) has them all
  with an example.
* **Picasso96's screen sizes** (1.1 beta 8): `C:PrismImport` reads
  `DEVS:Picasso96Settings` and switches on in PrismRTG every size and depth
  that was switched on there; sizes PrismRTG does not have become custom
  sizes (up to four). The installer offers it on a first install. Timings
  are not taken: the drivers make their own.

Neither of the two cards tested here lets software read what the monitor supports (the ZZ9000
firmware has no DDC code; the GBAPII++ does not wire the chip's DDC data
pin), so there is no automatic monitor detection: use the Test button.

## Speed against Picasso96

Same machine, same card, same benchmark (`PrismBench`), operations per
second, higher is better. Picasso96 is the release from Individual
Computers (rtg.library 43.760 of 18 December 2025) with each card's own
driver. Run-to-run noise is about 5%.

These are a handful of drawing operations on two cards, and they say nothing
about everything else Picasso96 does and PrismRTG does not: a long list of
cards, picture-in-picture, years of tested compatibility. Picasso96 may well
make different trade-offs for good reasons. `PrismBench` is included, so
anyone can repeat the measurements on their own machine.

### A4000, 68060, ZZ9000 - 800x600, 8-bit

PrismRTG 1.0 as released (5 October 2026); Picasso96 measured on the same
machine two days earlier. The three right-hand columns were measured on
14 and 15 October, each against its neighbour over four boots (two runs per
boot, mean shown): 1.0.2, 1.1 beta 8, and the main branch after beta 8,
which builds short text's template a glyph column at a time and writes the
ZZ9000's command fields in pairs. Speeds vary by up to 5-10% from one boot
to the next on this machine, so only the differences larger than that mean
much.

Beta 8 was about 9% behind 1.0.2 on 4-character text and a few percent on
lines and scrolling. Profiling on the A4000 put most of the text loss in one
loop that stalled the 68060 (an index loaded just before it was used); the
main branch is level with 1.0.2 there and ahead on long text, small fills
and lines. ScrollRaster is still about 4% behind 1.0.2, and the
bitmap-to-screen blit moves by 5-10% with where the compiler places the
code, so read its column as noise. How the betas before lost speed and won
it back is in [docs/design.md](docs/design.md).

| Test | Picasso96 | PrismRTG 1.0 | 1.0.2 (14 Oct) | 1.1 beta 8 | main (15 Oct) |
|---|---|---|---|---|---|
| RectFill 100x100 | 19,933 | **22,046** | **21,506** | **21,782** | **22,091** |
| RectFill 8x8 | 26,148 | **35,484** | **35,466** | **38,429** | **40,316** |
| Text, 40 characters | 4,894 | **5,387** | **5,498** | **5,251** | **5,928** |
| Text, 4 characters | 9,844 | **15,433** | **16,242** | **14,646** | **16,765** |
| ClipBlit 200x100 | 5,493 | **8,695** | **8,616** | **8,353** | **8,629** |
| ScrollRaster 400x100 | 5,520 | **11,149** | **10,734** | **10,230** | **10,313** |
| Line, ~390 pixels | 18,134 | **26,761** | **26,760** | **25,520** | **28,119** |
| WritePixel | 92,941 | **117,717** | **117,898** | **118,233** | **118,017** |
| Planar blit 64x32 (icons) | 241 | **1,007** | **1,009** | **999** | **993** |
| Bitmap-to-screen blit 64x32 | 11,178 | **24,271** | **23,499** | **25,638** | **23,972** |

16-bit and 32-bit show the same pattern (WritePixel at 32-bit: 92,875 /
99,498). Screen flips (`ScreenToFront` between two PrismRTG screens) run at
25-33 a second against 50 for two native screens: one extra frame per flip.

### A2000, 68030 at 50 MHz, GBAPII++ - 640x480

PrismRTG 1.1 beta 8 measured on 15 October 2026, Picasso96 on 4 October,
same machine and card.

| Test | P96 8-bit | **PrismRTG 8** | P96 16-bit | **PrismRTG 16** |
|---|---|---|---|---|
| RectFill 100x100 | 3,902 | **4,022** | 2,865 | 2,806 |
| RectFill 8x8 | 5,094 | **6,762** | 4,918 | **6,580** |
| Text, 40 characters | 1,343 | **1,491** | 1,280 | **1,473** |
| Text, 4 characters | 2,678 | **3,404** | 2,619 | **3,338** |
| ClipBlit 200x100 | 1,107 | **1,378** | 652 | **717** |
| ScrollRaster 400x100 | 698 | **794** | 383 | **401** |
| Line, ~390 pixels | 1,805 | **2,415** | 1,488 | **1,876** |
| WritePixel | 14,646 | **22,965** | 14,206 | **22,490** |
| Planar blit 64x32 (icons) | 306 | **345** | 160 | **166** |
| Bitmap-to-screen blit 64x32 | 2,378 | **3,774** | 2,025 | **2,974** |

True colour on the GBAPII++ comes in two formats, and they trade off:

| Test | P96 "24" | PrismRTG 24-bit | PrismRTG 32-bit |
|---|---|---|---|
| RectFill 100x100 | 1,001 | 763 | **1,441** |
| Text, 40 characters (JAM1) | 416 | 287 | **1,312** |
| Text, 40 characters (JAM2) | 214 | 200 | **1,304** |
| ClipBlit 200x100 | 406 | **427** | 299 |
| ScrollRaster 400x100 | 213 | 181 | 160 |
| WritePixel | 13,718 | **18,736** | **15,900** |
| Planar blit 64x32 (icons) | 108 | 104 | **124** |

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
* on a first install with Picasso96's settings on the Amiga, offers to
  switch on the same screen sizes and depths in PrismRTG (`PrismImport`;
  Picasso96's settings are only read),
* asks which PrismRTG screen Workbench should open on.

![The installer's Picasso96 question](docs/installer-picasso96.png)

Reboot. Run Install_PrismRTG again on an installed PrismRTG and it asks:
**update** (the programs are replaced, your settings and the Workbench
screen are kept), **install afresh** (the settings start over and the
screen question is asked again) or **remove** (Picasso96 comes back out
of the backup drawer and the previous screen mode is put back). The
Picasso96 question is asked on every install, so a Picasso96 added later
is found too.

PrismRTG and Picasso96 cannot both be active on one Amiga: with Picasso96's
monitor files still in `DEVS:Monitors` (or its `rtg.library` in memory),
PrismD prints why and does not start. The product is called PrismRTG; its
files keep their short names (`PrismD`, `PrismPrefs`, the monitor file
`Prism`, the `PRISM:` screen modes).

| Installed file | What it is |
|---|---|
| `C:PrismD` | the driver, started at boot |
| `LIBS:Prism/*.driver` | separately loaded board backends |
| `DEVS:Monitors/Prism` | starts PrismD before Workbench opens |
| `SYS:Prefs/PrismPrefs` | settings editor |
| `C:PrismSetup`, `C:PrismProbe` | installer helper, board lister |
| `C:PrismImport` | Picasso96's screen sizes into PrismRTG's settings |
| `C:PrismUpdate` | the update check (PrismPrefs' Updates) |
| `LIBS:Picasso96API.library` | a small stub for programs that look for the file; the library itself is built into PrismD |
| `ENVARC:Prism.prefs` | the settings (plain text) |

If the screen stays dark: hold the left mouse button during boot, then use
PrismPrefs to switch a mode off, change its refresh rate or turn the
blitter off.

### Translations

PrismPrefs and PrismSetup take their texts from a locale.library catalog
when one is installed for your language (`LOCALE:Catalogs/<language>/
PrismRTG.catalog`), English otherwise; `SetEnv PRISMLANG <language>`
picks one for PrismRTG's tools alone. There are no translations yet:
[docs/translating.md](docs/translating.md) says how to make one from
`catalogs/PrismRTG.cd`, and a partial one is welcome.

## Known limits

* Tested daily on two machines by one person (the A4000 with a ZZ9000 and the
  A2000 with a GBAPII++); the Z3660 driver on its author's own card, and the
  Vampire and the MiSTer by users (see the card tables). Treat it as early software.
* **Software pointer:** boards without a working hardware pointer get one
  drawn into the screen; "Soft pointer" in PrismPrefs (`SOFTWAREPOINTER=ON`) forces it.
  It costs a few percent of drawing speed (WritePixel about 14%), and while a
  program draws over the pointer itself it is taken away and comes back a
  moment later.
* **Screen mode promotion** ("Promote screens" in PrismPrefs, `PROMOTE=ON`)
  opens a screen a program asks for on PAL, NTSC or another chipset
  monitor on the board instead, in the smallest 256-colour mode that holds
  it (a 320x200 game gets the 320x200 mode, a 640x256 one 640x400), at
  the depth it asked for. The Programs list makes exceptions either way:
  a program kept on the chipset with the switch on, or promoted with it
  off (`KEEPPROG=` / `PROMOTEPROG=`, by command name or task name). The
  screen is the mode's size, so a program that draws a 640x256 screen
  edge to edge has a band left over; programs that bring their own bitmap
  (`CUSTOMBITMAP`) or ask for more than 8 planes are left alone, and so
  is Workbench itself (Prefs/ScreenMode puts it on the board).
* **Video layers (overlay)** for video players: PrismD carries its own
  cgxvideo.library (CreateVLayerHandle, AttachVLayer, LockVLayer and the
  rest, V42), and the Picasso96 PIP calls take the YUV 4:2:2 formats.
  On a ZZ9000 with the BlitterStudio firmware (2.8 and later) a layer with
  a colour key, in one of the firmware's YUV formats, on a 16- or 32-bit
  screen is shown by the board itself, scaled at scan-out, and costs the
  CPU nothing (`HWOVERLAY=OFF` in the prefs, "Video on board" in
  PrismPrefs, turns that off). Everywhere else the player's frames (YCbCr
  4:2:2, YUV 4:2:2, or 15/16-bit RGB) are converted on the CPU and scaled
  into the window by the compositor every tick, ahead of a software
  pointer; there colour keying is answered with a pen but not needed, the
  layer's area is drawn over whatever the player painted. Tested with
  FroggerNG (`DISPLAY=VLAYER` and `DISPLAY=P96`) on
  the emulated ZZ9000 and GBAPII++; expect a 68030 to manage small
  windows and an 060 full ones. The ZZ9000's firmware can convert YUV on
  the card (OP_WRITE_YUV); handing it that work is on the backlog.
* **Pointers up to 64x64.** A pointer image bigger than the board's sprite
  (the ZZ9000's and the Z3660's are 32x48; the Cirrus cursor is 64x64) is drawn by the
  software pointer instead of being cut, so the large pointers of the OS
  3.2 preferences, and programs' own through pointerclass, show whole.
* **Oversized screens with autoscroll.** A screen opened with
  `SA_AutoScroll` and a size beyond its mode (a 1280x960 Workbench on a
  640x480 monitor, say) gets a bitmap of that size, and the card shows a
  mode-sized part of it; Intuition scrolls it when the pointer pushes an
  edge, as on the chipset. The board must be able to step rows by the
  larger pitch: the Cirrus family and the ZZ9000 can; a P96 card driver,
  the Z3660 or UAE's native board gets the mode's size instead.
* **Gamma** ("Gamma" in PrismPrefs, `GAMMA=1.4` in the prefs file, 0.5
  to 2.5): every palette a board is given goes through the curve, so
  256-colour screens and the Test picture brighten (above 1.0) or darken
  (below) in the midtones. The DAC is where it applies: 16-bit and deeper
  screens carry their colours in the pixels and are not touched.
* **Your own sizes** (the four "custom" entries in PrismPrefs' mode list,
  `CUSTOM1=1152x864` .. `CUSTOM4=` in the prefs file): pick one, give it a
  width (a multiple of 8) and a height, Set size, and Test shows it at
  once; PrismD offers it after a restart, at every depth the board
  manages. Whether a board can show a size is its driver's call: the
  Cirrus family (Picasso II, GBAPII++, Piccolo, Spectrum) computes a VESA
  GTF timing for any size up to 2048x1016 within its pixel clock, so an
  unusual monitor's 1152x864 or 1360x768 works; the ZZ9000 has its
  firmware's list (720x576, 720x480, 640x512 and 1920x800 are in it but
  not in PrismRTG's list); a P96 card driver has its own.
* **Update check** ("Updates" in PrismPrefs, `UPDATES=GITHUB|MIRROR|BOTH`,
  then "Check now"): `C:PrismUpdate` asks GitHub's release API, or a mirror
  (`MIRROR=https://host/dir/`, a directory with `latest.txt` holding
  "version archive"), whether a newer release exists, and offers to
  download it to T: and run its installer, which ends with the usual
  reboot. Everything goes over HTTPS, so it needs a running TCP/IP stack
  and AmiSSL 5 (or an SSL offload that answers to the AmiSSL API: the
  tool keeps to the basic calls). The server's certificate is checked
  against AmiSSL's (`AmiSSL:Certs`); without them the connection is made
  unverified and the report says so. A beta sees pre-releases, a final
  release only final ones. From a Shell: `PrismUpdate`, `PrismUpdate GET`.
* **Off unless switched on:** screen dragging (`DRAGGING=ON`) and
  Picasso96 PIP windows go through a compositor that redoes only what
  changed and copies the dragged screen with the card's blitter; a program
  drawing on a dragged screen keeps most of its speed on a 68030. Off,
  they cost nothing. Few programs have used it yet.
* Vertical dragging composes the front and next RTG screen; native-chipset
  screens cannot be mixed into the split. No monitor detection (see above).
* **Monitor power saving** ("Sleep" in PrismPrefs, `POWERSAVE=`): the Picasso II
  family (but not the Picasso IV) and the ZZ9000 with firmware 2.7 or newer put the
  monitor to sleep; every other board only shows black, and the monitor stays on.
  The monitor going to sleep has been checked in the emulator, not yet watched on a real card.
* **Mirror chipset** ("Mirror chipset" in PrismPrefs, `MIRRORCHIPSET=ON`) shows a copy of
  a chipset screen that promotion leaves alone on the first board; the chipset
  screen itself stays. HAM and dual-playfield screens are not mirrored. Switching
  it on or off takes effect at the next reboot. Checked on the real GBAPII++ by
  reading the card's picture back: doubled bars and rows, right.
* The Picasso II, Piccolo, Piccolo SD64, Spectrum, Graffity and Picasso IV (Zorro II and III) have only run in WinUAE.
  A real card can differ from its emulation (the GBAPII++ did, in three
  places). If 256-colour screens on a real Piccolo or Spectrum show red and
  blue exchanged, tick **Swap red/blue** in PrismPrefs.
* The installer's Remove path has been run in the emulator, not yet on a
  real machine.
* On the A2000 one unexplained crash was seen shortly after a cold boot;
  the boot failsafe recovered it and it has not been reproduced.

## Reporting a problem

Download **PrismCheck** from the release page and run it on the Amiga (double-click it
with PrismRTG running). In 2 to 5 minutes it records the system, the card,
PrismRTG's settings and modes, and runs every drawing test, the Picasso96 API test
and `vramcheck` - into one text file. Attach that file to an
[issue](https://github.com/lainejones/PrismRTG/issues) with a few words on what
you saw on the screen.

## Build

Needs amiga-gcc (Bebbo) under WSL or Linux:

```sh
./build.sh
```

Output goes to `out/`, the release drawer to `out/pkg/PrismRTG`.
`./build.sh` packages last, after every program is built. The icons are
finished `.info` files in `dist/`.

| Tool | What it does |
|---|---|
| `PrismD` | The driver. `PrismD [BOARD=ZZ9000|PICASSO2|Z3660|P96|UAEGFX|<name>] [P96CARD=file] [P96MONITOR=icon] [PREFS=file] [LOG]`; Ctrl-C removes its patches when no PrismRTG screen is open and no program holds a PrismRTG bitmap. P96 and UAEGFX contexts remain resident until reboot. |
| `PrismPrefs` | Settings editor. `FROM= USE SAVE TEST=<slot>` work without the window. |
| `Prism` (`out/PrismMon`) | `DEVS:Monitors` launcher with the boot failsafe. |
| `PrismSetup` | Installer helper: `DETECT`, `WBID=<id>` (Workbench mode by ModeID, e.g. `0102` = 800x600 16-bit; `WBMODE=<slot>` still works). |
| `PrismProbe` | Lists expansion boards and marks the ones PrismRTG can drive. Read-only. |
| `PrismUpdate` | Asks GitHub or a mirror whether a newer release exists and offers to fetch and install it: `GET`. |
| `PrismImport` | Picasso96's switched-on sizes and depths into `ENVARC:Prism.prefs`: `[FROM=file] [BOARD=name] [ONLY] [TEST]` (ONLY switches every other mode off, TEST writes nothing). |
| `cgxtest`, `arexxtest.rexx` | Checks of the CyberGraphX 43 calls (`MODEID=`) and of the ARexx port (`rx arexxtest.rexx`). |
| `mirrortest`, `muitime` | A chipset screen with its own bitmap, drawn into the planes (`DEPTH= EHB SECS= MOVE=`), for MIRRORCHIPSET; the time MUI takes to open a busy window (`N=`). |
| `PrismBench` | Benchmark and pixel-exactness checks: `MODEID=$7A00xxxx`, `LIST`. |
| `PrismTest` | A driver on its own, without PrismD: loads `BOARD=NAME` (any `.driver`), sets a mode, draws a pattern, checks the driver's fills and copies against the CPU (`BLIT`). |
| `PrismScreen`, `PrismShow`, `PrismMouse`, `prismdiag` | Test clients: screens, pictures, pointer, display database dump. |
| `ddcprobe`, `zztime`, `iotime`, `memwhere`, `p2peek` | Hardware probes and timing tools. |

[docs/architecture.md](docs/architecture.md) is the technical overview:
what runs, what is patched, and how a drawing call travels from a program
to the card. [docs/design.md](docs/design.md) has how each part was
built, in order, and every measurement; [docs/zz9000-hw.md](docs/zz9000-hw.md) and
[docs/cirrus-picasso2-hw.md](docs/cirrus-picasso2-hw.md) and [docs/z3660-hw.md](docs/z3660-hw.md) are the hardware
notes the drivers were written from.

## Credits

PrismRTG is written by Laine Jones, with Claude (Anthropic) as co-author -
noted in every commit. With thanks to:

* **Stefan Reinauer** - the build fixes (#3), the P96 card-driver adapter
  (#4), the native UAE RTG driver (#5), the format-aware surface operations
  (#6), the loadable board-driver modules (#8), the P96 memory pools,
  shadow transfers and UAE planar probe (#9) and the software pointer,
  screen dragging and PIP composition (#10).
* **Jusii** - the Z3660 driver: the board, its monitor switch, and fills,
  copies, text, lines and planar blits on its blitter (#13, #16-#20),
  written and tested on his own Z3660; the seven larger sizes (#14) and the
  Workbench staying on the card after a ScreenMode change (#15).
* **Dimitris Panokostas** (midwan, author of Amiberry) - reviewed the UAE
  driver and found its `SetPanning` / `BitMapExtra` bug (#5), and made
  Amiberry's RTG hardware pointer the default (8.4.0) so PrismRTG shows a
  pointer there ([amiberry#2388](https://github.com/BlitterStudio/amiberry/pull/2388)),
  and fixed the 32-bit write-back ([amiberry#2392](https://github.com/BlitterStudio/amiberry/issues/2392)).
* **polluks** and **yelworC** - the first test on a MiSTer FPGA (#2); **halle-saale** - passing on yelworC's PrismCheck report from it.
* **HANSolo-0007** - the first real card through the P96 adapter, an Apollo
  Vampire, with PrismCheck reports and the findings behind beta 5's
  Workbench drag images, planar-blit check, 16:9 sizes and PrismPrefs
  switches (#1).
* **Individual Computers**, **Alexander Kneer**, **Tobias Abt** and **Thomas
  Richter** - the P96 driver development headers in `src/p96sdk` (CC-BY),
  which the P96 card-driver adapter is built with.
* **Toni Wilen** - WinUAE, in which every Cirrus board here was tested,
  and whose source is the only public description of the Graffity and of the
  Picasso IV's board logic.

### Where the hardware knowledge came from

PrismRTG's drivers are its own code, written from hardware facts: register
numbers, bit meanings and command sequences. No board's data book was
available, so those facts were read from these public sources (licences in
brackets; the exact files and revisions are listed in
[docs/cirrus-picasso2-hw.md](docs/cirrus-picasso2-hw.md) §10 and
[docs/zz9000-hw.md](docs/zz9000-hw.md)):

* **MNT Research / Lukas F. Hartmann** - the ZZ9000 card driver and
  firmware (GPL-3.0-or-later), for the ZZ9000's registers and mailbox.
* **NetBSD** `grf_cl` (BSD), **Linux** `cirrusfb` (GPL-2.0),
  **xf86-video-cirrus** (MIT), **QEMU** `cirrus_vga` (MIT), **86Box** and
  **PCem** via WinUAE (GPL-2.0) - the Cirrus GD542x/543x registers and
  blitter.
* **Matthias Heinrichs** - the GBAPII++ / A500-GraKa design files, for
  that board's bus and monitor switch.
* **Individual Computers** - the P96 driver development headers (above),
  which also define the Picasso96 API numbers PrismRTG's own
  `Picasso96API.library` implements.

Programs talk to PrismRTG through the cybergraphics and Picasso96
interfaces: their function and tag numbers are reproduced so that existing
programs work; the implementations behind them are PrismRTG's own.

Contributions are welcome; everyone whose code or findings go into PrismRTG
is named here and in the release notes.

## Licence

GNU General Public License, version 3 (GPL-3.0-only). Copyright (c) 2026 Laine Jones and Stefan Reinauer; each source file names its authors.
See [LICENSE](LICENSE) and [CONTRIBUTING.md](CONTRIBUTING.md).
