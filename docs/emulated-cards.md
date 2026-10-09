# PrismRTG on every RTG board WinUAE emulates

Every graphics board WinUAE (6.x) can emulate that PrismRTG has a driver
for, run through the same tests on the same machine: an emulated 68030
at cycle-exact speed (`cpu_speed=real`), 4 MB fast RAM, AmigaOS 3.2.3,
PrismD started at boot with the board on Auto (the UAE RTG rows start
PrismD by hand, once with the native UAE driver and once through the P96
adapter with `uaegfx.card`). The figures are PrismBench's, on an 800x600
Workbench, so they compare the boards' emulations and PrismRTG's paths,
not real hardware: WinUAE's Cirrus emulation does the blitter at host
speed.

What the columns mean: *Depths* is `tmpltest` at 8, 16, 24 and 32 bits
("no mode" = the board offers no such mode); *Exactness checks* counts
every "0 wrong" line of `tmpltest` and `PrismBench` (planar blits,
lines, text, fills, pixel arrays, templates, scaling...) against any
line with wrong pixels; *p96test* is the Picasso96 API test. Boards
WinUAE emulates that have no PrismRTG driver (CyberVision 64 and 64/3D,
Retina, Merlin, Domino, the PCI cards...) are not in the table; the P96
adapter can drive those that have a Picasso96 `.card`, which this run
did not include.

| Card | Driver / pass | Depths (tmpltest) | Exactness checks | p96test | 8-bit RectFill/s | Text/s | ClipBlit/s | Line/s | WritePixel/s | Planar blit/s | 16-bit RectFill/s | Text/s | ClipBlit/s | WritePixel/s |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| Picasso II (GD5426, Z2) | picasso2 | 8:ok 16:ok 24:ok 32:no mode | 65 ok / 0 wrong | 0/29 failed | 4885 | 1394 | 4634 | 2379 | 17940 | 429 | 4792 | 1392 | 4636 | 17435 |
| Picasso II+ (GD5428, Z2) | picasso2 | 8:ok 16:ok 24:ok 32:no mode | 65 ok / 0 wrong | 0/29 failed | 4895 | 1398 | 4620 | 2379 | 17907 | 429 | 4805 | 1386 | 4628 | 17350 |
| Piccolo (GD5426, Z2) | picasso2 | 8:ok 16:ok 24:ok 32:no mode | 65 ok / 0 wrong | 0/29 failed | 4891 | 1394 | 4639 | 2378 | 18093 | 429 | 4763 | 1392 | 4634 | 17549 |
| Piccolo (GD5426, Z3) | picasso2 | 8:ok 16:ok 24:ok 32:no mode | 65 ok / 0 wrong | 0/29 failed | 4879 | 1399 | 4621 | 2363 | 17934 | 429 | 4801 | 1392 | 4626 | 17373 |
| Piccolo SD64 (GD5434, Z2) | picasso2 | 8:ok 16:ok 24:ok 32:ok | 85 ok / 0 wrong | 0/29 failed | 4904 | 1399 | 4628 | 2367 | 17847 | 429 | 4837 | 1389 | 4624 | 17310 |
| Piccolo SD64 (GD5434, Z3) | picasso2 | 8:ok 16:ok 24:ok 32:ok | 85 ok / 0 wrong | 0/29 failed | 4908 | 1396 | 4609 | 2374 | 17954 | 429 | 4835 | 1387 | 4617 | 17437 |
| Spectrum 28/24 (GD5428, Z2) | picasso2 | 8:ok 16:ok 24:ok 32:no mode | 65 ok / 0 wrong | 0/29 failed | 4897 | 1399 | 4624 | 2367 | 18050 | 429 | 4801 | 1386 | 4621 | 17404 |
| Spectrum 28/24 (GD5428, Z3) | picasso2 | 8:ok 16:ok 24:ok 32:no mode | 65 ok / 0 wrong | 0/29 failed | 4888 | 1396 | 4630 | 2374 | 18186 | 429 | 4788 | 1387 | 4625 | 17575 |
| Graffity (GD5428, Z2) | not detected: no PrismRTG driver for this chip | - | - | - | - | - | - | - | - | - | - | - | - | - |
| Graffity (GD5428, Z3) | not detected: no PrismRTG driver for this chip | - | - | - | - | - | - | - | - | - | - | - | - | - |
| Picasso IV (GD5446, Z2) | not detected: no PrismRTG driver for this chip | - | - | - | - | - | - | - | - | - | - | - | - | - |
| Picasso IV (GD5446, Z3) | not detected: no PrismRTG driver for this chip | - | - | - | - | - | - | - | - | - | - | - | - | - |
| UAE RTG, Zorro II (4 MB) | P96 | 8:no mode 16:no mode 24:no mode 32:no mode | 0 ok / 0 wrong | no library | - | - | - | - | - | - | - | - | - | - |
| UAE RTG, Zorro II (4 MB) | UAEGFX | 8:ok 16:ok 24:no mode 32:ok | 65 ok / 0 wrong | 0/29 failed | 5839 | 305 | 5315 | 2382 | 18005 | 3356 | 5735 | 354 | 5306 | 17475 |
| UAE RTG, Zorro III (16 MB) | P96 | 8:no mode 16:no mode 24:no mode 32:no mode | 0 ok / 0 wrong | no library | - | - | - | - | - | - | - | - | - | - |
| UAE RTG, Zorro III (16 MB) | UAEGFX | 8:ok 16:ok 24:no mode 32:ok | 65 ok / 0 wrong | 0/29 failed | 5763 | 301 | 5373 | 2325 | 18535 | 3347 | 5681 | 339 | 5380 | 18160 |

## Notes

* **Graffity** (Ateo Concepts, autoconfig 2092/33+34, Cirrus GD5428) and
  **Picasso IV** (Village Tronic 2167/21-24, Cirrus GD5446 with a
  flicker fixer) are emulated by WinUAE but have no PrismRTG driver.
  The Graffity is the same chip as the Spectrum 28/24 with a different
  register window layout (Zorro III: VRAM at board + 0xC00000, two
  64 KB register windows at + 0x400000 and + 0x800000), so it is a
  candidate for the Cirrus driver's board table. The Picasso IV is a
  driver of its own.
* **UAE RTG through the P96 adapter** (`BOARD=P96 P96CARD=uaegfx.card`)
  ran in its own boot: the native UAE driver keeps the emulator's card
  context after PrismD stops, so the two cannot follow each other in one
  session.
* The UAE RTG rows' **text figure** (about 300 strings/s against about
  1,400 on the Cirrus boards) is PrismRTG drawing text on the CPU into
  the UAE card's memory: UAE's RTG has no template expansion, and the
  write path to its VRAM is the slow part. Worth a look, not a fault.
* The Cirrus boards all measure the same because WinUAE emulates one
  blitter for all of them at host speed; the differences between the
  real boards (bus width, clock, DAC) are not emulated.

Generated by the bench scripts from the run of 2026-10-08 (PrismRTG main
at the commit this file was added in).
