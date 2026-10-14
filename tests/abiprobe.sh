#!/bin/sh
# tests/abiprobe.sh : the PrismD a later release might be - PRISM_ABI_PROBE appends a member and a hook to
# struct PrismBoard and an operation to struct PrismOps - built as out/PrismD.abiprobe, and a ZZ9000 driver
# built the same way as out/DriversProbe/ZZ9000.driver (a driver newer than PrismD). The gate's abicheck.py
# runs this release's drivers under the first (they must load) and the second under this PrismD (refused).
set -e
cd "$(dirname "$0")/.."
export PATH=/opt/amiga/bin:$PATH
F="-DPRISM_ABI_PROBE -DPRISM_DEBUG -O2 -fno-tree-loop-distribute-patterns -m68020-60 -noixemul -Wall -Wno-pointer-sign -fomit-frame-pointer -Isrc -Isrc/p96sdk"
m68k-amigaos-gcc $F -ffunction-sections -fdata-sections -Wl,--gc-sections -s -o out/PrismD.abiprobe src/prismd.c src/prefs.c \
  src/bitmap.c src/present.c src/render.c src/gels.c src/selftest.c src/pointer.c src/cgx.c src/cgxvideo.c src/p96.c \
  src/stubs.S src/p2c.S src/boardops.c src/driver_loader.c src/arexx.c -lm
CFLAGS="$F" OUT=out/DriversProbe sh tools/mkdriver.sh ZZ9000 src/drv_zz9000.c > /dev/null
echo "abiprobe builds: out/PrismD.abiprobe, out/DriversProbe/ZZ9000.driver"