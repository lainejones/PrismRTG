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
| Picasso II+ (GD5428, Z2) | picasso2 | 8:ok 16:ok 24:ok 32:no mode | 65 ok / 0 wrong | 0/29 failed | 4902 | 1397 | 4641 | 2368 | 18022 | 429 | 4817 | 1387 | 4638 | 17464 |
| Piccolo (GD5426, Z2) | picasso2 | 8:ok 16:ok 24:ok 32:no mode | 65 ok / 0 wrong | 0/29 failed | 4891 | 1394 | 4639 | 2378 | 18093 | 429 | 4763 | 1392 | 4634 | 17549 |
| Piccolo (GD5426, Z3) | picasso2 | 8:ok 16:ok 24:ok 32:no mode | 65 ok / 0 wrong | 0/29 failed | 4879 | 1399 | 4621 | 2363 | 17934 | 429 | 4801 | 1392 | 4626 | 17373 |
| Piccolo SD64 (GD5434, Z2) | picasso2 | 8:ok 16:ok 24:ok 32:ok | 85 ok / 0 wrong | 0/29 failed | 4904 | 1399 | 4628 | 2367 | 17847 | 429 | 4837 | 1389 | 4624 | 17310 |
| Piccolo SD64 (GD5434, Z3) | picasso2 | 8:ok 16:ok 24:ok 32:ok | 85 ok / 0 wrong | 0/29 failed | 4908 | 1396 | 4609 | 2374 | 17954 | 429 | 4835 | 1387 | 4617 | 17437 |
| Spectrum 28/24 (GD5428, Z2) | picasso2 | 8:ok 16:ok 24:ok 32:no mode | 65 ok / 0 wrong | 0/29 failed | 4897 | 1399 | 4624 | 2367 | 18050 | 429 | 4801 | 1386 | 4621 | 17404 |
| Spectrum 28/24 (GD5428, Z3) | picasso2 | 8:ok 16:ok 24:ok 32:no mode | 65 ok / 0 wrong | 0/29 failed | 4888 | 1396 | 4630 | 2374 | 18186 | 429 | 4788 | 1387 | 4625 | 17575 |
| Graffity (GD5428, Z2) | picasso2 | 8:ok 16:ok 24:ok 32:no mode | 65 ok / 0 wrong | 0/29 failed | 4896 | 1393 | 4646 | 2380 | 18066 | 429 | 4797 | 1390 | 4654 | 17566 |
| Graffity (GD5428, Z3) | picasso2 | 8:ok 16:ok 24:ok 32:no mode | 65 ok / 0 wrong | 0/29 failed | 4899 | 1398 | 4640 | 2373 | 17932 | 429 | 4811 | 1390 | 4635 | 17357 |
| Picasso IV (GD5446, Z2) | not detected: no PrismRTG driver for this chip | - | - | - | - | - | - | - | - | - | - | - | - | - |
| Picasso IV (GD5446, Z3) | not detected: no PrismRTG driver for this chip | - | - | - | - | - | - | - | - | - | - | - | - | - |
| UAE RTG, Zorro II (4 MB) | UAEGFX | 8:ok 16:ok 24:no mode 32:ok | 65 ok / 0 wrong | 0/29 failed | 5839 | 305 | 5315 | 2382 | 18005 | 3356 | 5735 | 354 | 5306 | 17475 |
| UAE RTG, Zorro II (4 MB) | P96 | 8:ok 16:ok 24:no mode 32:ok | 65 ok / 0 wrong | 0/29 failed | 5731 | 1589 | 5358 | 2316 | 18349 | 3365 | 5662 | 1583 | 5340 | 18040 |
| UAE RTG, Zorro III (16 MB) | UAEGFX | 8:ok 16:ok 24:no mode 32:ok | 65 ok / 0 wrong | 0/29 failed | 5763 | 301 | 5373 | 2325 | 18535 | 3347 | 5681 | 339 | 5380 | 18160 |
| UAE RTG, Zorro III (16 MB) | P96 | 8:ok 16:ok 24:no mode 32:ok | 65 ok / 0 wrong | 0/29 failed | 5725 | 1593 | 5350 | 2325 | 18287 | 3370 | 5645 | 1587 | 5327 | 17973 |

## Notes

* **Graffity** (Ateo Concepts, autoconfig 2092/33+34, Cirrus GD5428):
  not detected in the first run; added to the Cirrus driver's board
  table the next morning from WinUAE's description (plain ports, the
  monitor switch in address bits 5-6, and for the Zorro III version the
  register window at board + 0x800000 and VRAM at + 0xC00000). Both
  rows are from the rerun.
* **Picasso IV** (Village Tronic 2167/21-24, Cirrus GD5446 with a
  flicker fixer) is emulated by WinUAE but has no PrismRTG driver: a
  different chip generation and a driver of its own.
* **UAE RTG through the P96 adapter** (`BOARD=P96 P96CARD=uaegfx.card`)
  ran in its own boot: the native UAE driver keeps the emulator's card
  context after PrismD stops, so the two cannot follow each other in one
  session.
* **Text on the native UAE driver** is about 300 strings/s, against
  about 1,590 through the P96 adapter on the same emulated card and
  about 1,400 on the Cirrus boards. Same machine, same card: the
  native driver's text path is five times slower than the adapter's.
  Cause, found the next morning: the native driver never installed a
  `BlitTemplateDefault`, and `rtg_expand` declines without one, so every
  string went to the CPU. Fixed (one line): 1,500 strings/s at 8 and
  16 bits, every text check still 0 wrong. The figures in the table are
  from before the fix.
* The Cirrus boards all measure the same because WinUAE emulates one
  blitter for all of them at host speed; the differences between the
  real boards (bus width, clock, DAC) are not emulated.

Generated by the bench scripts from the run of 2026-10-08 (PrismRTG main
at the commit this file was added in).
