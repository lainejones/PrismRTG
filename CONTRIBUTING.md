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

A driver fills in a `struct PrismBoard` (`src/prismboard.h`): mode set,
display start, palette and monitor switch are required; blitter, line and
cursor hooks are optional and PrismRTG draws on the CPU without them. See
`src/drv_picasso2.c` and `src/drv_zz9000.c`.

## Credit

Everyone whose code, review or findings go into PrismRTG is named in the
README's Credits section and in the release notes of the version it ships in.
Your commits keep your name as their author.
