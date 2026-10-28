#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Laine Jones
# Copyright (C) 2026 Stefan Reinauer
# Build Prism with amiga-gcc. Run inside WSL:
#   cd /mnt/c/projects/PrismRTG && ./build.sh
# Targets 68020+ (A2000 = 68030, A4000 = 68060); -m68020-60 avoids 64-bit
# mul/div instructions the 060 traps on.
set -e
export PATH=/opt/amiga/bin:$PATH

CC=m68k-amigaos-gcc
CFLAGS="-DPRISM_DEBUG -O2 -fno-tree-loop-distribute-patterns -m68020-60 -noixemul -Wall -Wno-pointer-sign -fomit-frame-pointer -Isrc -Isrc/p96sdk"
# PrismD and the driver modules: leave out functions nothing calls (debug helpers etc.)
GC="-ffunction-sections -fdata-sections -Wl,--gc-sections"

mkdir -p out

# Strip at LINK time with -s. NEVER m68k-amigaos-strip: it corrupts the hunk
# reloc table on this toolchain.
echo "== M0 tools =="
$CC $CFLAGS -s -o out/PrismProbe tools/prismprobe.c
if [ -f tools/prismtest.c ]; then
  $CC $CFLAGS -s -o out/PrismTest tools/prismtest.c src/boardops.c src/driver_loader.c
fi

echo "== Loadable board drivers (tools/mkdriver.sh NAME sources...) =="
export CC CFLAGS
sh tools/mkdriver.sh PICASSO2 src/drv_picasso2.c
sh tools/mkdriver.sh ZZ9000 src/drv_zz9000.c
sh tools/mkdriver.sh Z3660 src/drv_z3660.c
sh tools/mkdriver.sh P96 src/drv_p96.c src/rtg_ops.c
sh tools/mkdriver.sh UAEGFX src/drv_uaegfx.c src/rtg_ops.c
# the SAGA test driver (Vampire V4): built, not packaged until it has run on one
sh tools/mkdriver.sh SAGA src/drv_saga.c
# the template for new drivers must keep building (it is not packaged)
OUT=out/template sh tools/mkdriver.sh TEMPLATE src/drv_template.c

echo "== M1 =="
$CC $CFLAGS $GC -s -o out/PrismD src/prismd.c src/prefs.c src/bitmap.c src/present.c src/render.c src/gels.c src/selftest.c src/pointer.c src/cgx.c src/cgxvideo.c src/p96.c src/stubs.S src/p2c.S src/boardops.c src/driver_loader.c src/arexx.c -lm
$CC $CFLAGS -s -o out/PrismScreen tools/prismscreen.c tools/m8tests.c
$CC $CFLAGS -s -o out/PrismMouse tools/prismmouse.c
$CC $CFLAGS -s -o out/PrismBench tools/prismbench.c -lm

echo "== M5 =="
# the tools' texts: IDs for the translation catalog (src/catalog_ids.h, catalogs/PrismRTG.cd)
python3 tools/mkcatalog.py --ids tools/prismprefs.c tools/prismsetup.c
$CC $CFLAGS -s -o out/PrismPrefs tools/prismprefs.c src/prefs.c src/catalog.c
# PrismUpdate needs the AmiSSL SDK headers (the include/ directory of
# https://github.com/jens-maus/amissl, Apache 2.0): AMISSL_INC, default
# ~/amissl-sdk/include. Without them the rest still builds.
AMISSL_INC=${AMISSL_INC:-$HOME/amissl-sdk/include}
if [ -d "$AMISSL_INC" ]; then
  $CC $CFLAGS -I"$AMISSL_INC" -s -o out/PrismUpdate tools/update/update.c tools/update/upd_prismrtg.c src/prefs.c
else
  echo "PrismUpdate skipped: no AmiSSL SDK headers at $AMISSL_INC"
fi

echo "== M6 =="
$CC $CFLAGS -s -o out/PrismMon tools/prismmon.c src/prefs.c
$CC $CFLAGS -s -o out/PrismSetup tools/prismsetup.c src/prefs.c src/catalog.c
$CC $CFLAGS -s -o out/PrismImport tools/prismimport.c src/prefs.c
$CC $CFLAGS -s -o out/prismgrab tools/prismgrab.c
$CC $CFLAGS -s -o out/PrismShow tools/prismshow.c
$CC $CFLAGS -s -o out/prismdiag tools/prismdiag.c
$CC $CFLAGS -s -o out/iotime tools/iotime.c
$CC $CFLAGS -s -o out/p2peek tools/p2peek.c
# hardware probes (real boards only); built so they cannot rot unnoticed
$CC $CFLAGS -s -o out/ddcprobe tools/ddcprobe.c
$CC $CFLAGS -s -o out/memwhere tools/memwhere.c
$CC $CFLAGS -s -o out/zztime tools/zztime.c
$CC $CFLAGS -s -o out/lockwatch tools/lockwatch.c
$CC $CFLAGS -s -o out/cmreq tools/cmreq.c
$CC $CFLAGS -s -o out/lastalert tools/lastalert.c
$CC $CFLAGS -s -o out/wintest tools/wintest.c
$CC $CFLAGS -s -o out/prismlog tools/prismlog.c

echo "== Picasso96API.library (Prism's own) =="
$CC -nostartfiles -nostdlib -s -o out/Picasso96API.library tools/p96stub.S
$CC $CFLAGS -s -o out/p96test tools/p96test.c
$CC $CFLAGS -s -o out/cgxtest tools/cgxtest.c
$CC $CFLAGS -s -o out/mirrortest tools/mirrortest.c
$CC $CFLAGS -s -o out/muitime tools/muitime.c -lm
cp tools/arexxtest.rexx out/arexxtest.rexx
$CC $CFLAGS -s -o out/trapwatch tools/trapwatch.c
$CC $CFLAGS -s -o out/tmpltest tools/tmpltest.c
$CC $CFLAGS -s -o out/ropcheck tools/ropcheck.c
$CC $CFLAGS -s -o out/vlayertest tools/vlayertest.c tools/cgxvideo_stubs.S
$CC $CFLAGS -s -o out/vrampeek tools/vrampeek.c
$CC $CFLAGS -s -o out/bigpointer tools/bigpointer.c
$CC $CFLAGS -s -o out/movebench tools/movebench.c
$CC $CFLAGS -s -o out/dragcheck tools/dragcheck.c
$CC $CFLAGS -s -o out/dopusscreen tools/dopusscreen.c
$CC $CFLAGS -DSCRMOVE -s -o out/scrmove tools/dragcheck.c
$CC $CFLAGS -s -o out/busydraw tools/busydraw.c
$CC $CFLAGS -s -o out/stftest tools/stftest.c
$CC $CFLAGS -s -o out/vramcheck tools/vramcheck.c
$CC $CFLAGS -s -o out/prismstate tools/prismstate.c
$CC $CFLAGS -s -o out/pencheck tools/pencheck.c
$CC $CFLAGS -s -o out/icondraw tools/icondraw.c

sh tools/mkpkg.sh
sh tools/mkcheck.sh
ls -l out
echo "Done."
