#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Laine Jones
# Build Prism with amiga-gcc. Run inside WSL:
#   cd /mnt/c/projects/PrismRTG && ./build.sh
# Targets 68020+ (A2000 = 68030, A4000 = 68060); -m68020-60 avoids 64-bit
# mul/div instructions the 060 traps on.
set -e
export PATH=/opt/amiga/bin:$PATH

CC=m68k-amigaos-gcc
CFLAGS="-DPRISM_DEBUG -O2 -m68020-60 -noixemul -Wall -Wno-pointer-sign -fomit-frame-pointer -Isrc"

mkdir -p out

# Strip at LINK time with -s. NEVER m68k-amigaos-strip: it corrupts the hunk
# reloc table on this toolchain.
echo "== M0 tools =="
$CC $CFLAGS -s -o out/PrismProbe tools/prismprobe.c
if [ -f tools/prismtest.c ]; then
  $CC $CFLAGS -s -o out/PrismTest tools/prismtest.c src/drv_picasso2.c src/drv_zz9000.c
fi

echo "== M1 =="
$CC $CFLAGS -s -o out/PrismD src/prismd.c src/prefs.c src/bitmap.c src/render.c src/pointer.c src/cgx.c src/p96.c src/stubs.S src/p2c.S src/drv_picasso2.c src/drv_zz9000.c
$CC $CFLAGS -s -o out/PrismScreen tools/prismscreen.c tools/m8tests.c
$CC $CFLAGS -s -o out/PrismMouse tools/prismmouse.c
$CC $CFLAGS -s -o out/PrismBench tools/prismbench.c -lm

echo "== M5 =="
$CC $CFLAGS -s -o out/PrismPrefs tools/prismprefs.c src/prefs.c

echo "== M6 =="
$CC $CFLAGS -s -o out/PrismMon tools/prismmon.c src/prefs.c
$CC $CFLAGS -s -o out/PrismSetup tools/prismsetup.c src/prefs.c
$CC $CFLAGS -s -o out/prismgrab tools/prismgrab.c
$CC $CFLAGS -s -o out/PrismShow tools/prismshow.c
$CC $CFLAGS -s -o out/prismdiag tools/prismdiag.c
$CC $CFLAGS -s -o out/iotime tools/iotime.c
$CC $CFLAGS -s -o out/p2peek tools/p2peek.c
sh tools/mkpkg.sh

ls -l out
echo "Done."
$CC $CFLAGS -s -o out/lockwatch tools/lockwatch.c
$CC $CFLAGS -s -o out/cmreq tools/cmreq.c
$CC $CFLAGS -s -o out/lastalert tools/lastalert.c
$CC $CFLAGS -s -o out/wintest tools/wintest.c
$CC $CFLAGS -s -o out/prismlog tools/prismlog.c

echo "== Picasso96API.library (Prism's own) =="
$CC -nostartfiles -nostdlib -s -o out/Picasso96API.library tools/p96stub.S
$CC $CFLAGS -s -o out/p96test tools/p96test.c
$CC $CFLAGS -s -o out/trapwatch tools/trapwatch.c
$CC $CFLAGS -s -o out/tmpltest tools/tmpltest.c
