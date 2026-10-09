# Contributing to PrismRTG

Changes are welcome: bug fixes, speed work, and above all drivers and test
reports for graphics cards.

## Licence of contributions

PrismRTG is licensed under the GNU General Public License, version 3
(`LICENSE`, `SPDX-License-Identifier: GPL-3.0-only`). By sending a change
you agree that it is your own work (or that you have the right to submit
it) and that it is licensed under the same terms. Keep the SPDX line at
the top of each source file and add one to new files.

## Where code may come from

Drivers are written from hardware facts: data sheets, register
descriptions, behaviour measured on the card. Notes on each card live in
`docs/`.

* Do not paste code from Picasso96, CyberGraphX or any other closed
  software, and do not include their files.
* GPL-3-compatible sources (for example the ZZ9000 driver and firmware,
  GPL-3.0-or-later) may be used with attribution.
* Linux (GPL-2.0-only) and NetBSD code must not be copied in.

## Testing

* `./build.sh` under WSL with amiga-gcc builds everything and stages the
  package in `out/pkg/Prism`.
* `PrismBench MODEID=$7A00xxxx` on a PrismRTG screen must report 0 wrong for
  every exactness check (planar blit, line, text, fill) at 8, 16 and
  24/32-bit.
* `PrismTest BLIT` checks a card's blitter against the CPU.
* Say what the change was tested on: a real card (which one, which CPU) or
  an emulator. A driver that has only run in an emulator is marked
  "emulation only" in the ReadMe until someone confirms it on hardware.

## New card drivers

A driver is one C file that fills in a `struct PrismBoard`
(`src/prismboard.h`); only a mode set and a display start are required,
everything else (palette, monitor switch, blitter, cursor) is optional and
PrismRTG draws on the CPU without it. Start from `src/drv_template.c`,
build with `sh tools/mkdriver.sh NAME src/drv_name.c`, and follow
[docs/writing-a-driver.md](docs/writing-a-driver.md): it tests the driver
with `PrismTest` before PrismD ever runs on it. `src/drv_zz9000.c` is the
complete example with a blitter and sprite.

## Credit

Everyone whose code, review or findings go into PrismRTG is named in the
README's Credits section and in the release notes of the version it ships in.
Your commits keep your name as their author.
