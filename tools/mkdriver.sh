#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Laine Jones
# Build one loadable board driver:
#
#     sh tools/mkdriver.sh NAME source.c [more sources...]
#
# makes out/Drivers/NAME.driver (OUT=dir to put it elsewhere) from
# src/driver_module.c, the driver's own sources and src/boardops.c. The
# driver defines driver_probe() and driver_retain() (see src/drv_template.c
# and docs/writing-a-driver.md). PrismD loads LIBS:Prism/NAME.driver for
# BOARD=NAME. Extra compiler flags go in DRIVER_CFLAGS. Run inside WSL.
set -e
if [ $# -lt 2 ]; then
    echo "usage: sh tools/mkdriver.sh NAME source.c [more sources...]" >&2
    exit 2
fi
NAME=$1; shift
export PATH=/opt/amiga/bin:$PATH
CC=${CC:-m68k-amigaos-gcc}
CFLAGS=${CFLAGS:-"-DPRISM_DEBUG -O2 -m68020-60 -noixemul -Wall -Wno-pointer-sign -fomit-frame-pointer -Isrc -Isrc/p96sdk"}
GC="-ffunction-sections -fdata-sections -Wl,--gc-sections"
OUT=${OUT:-out/Drivers}
mkdir -p "$OUT"
# Strip at link time with -s, never with m68k-amigaos-strip (see build.sh).
$CC $CFLAGS $GC ${DRIVER_CFLAGS:-} -s -DPRISM_DRIVER_MODULE -DDRIVER_NAME="\"$NAME\"" \
    -o "$OUT/$NAME.driver" src/driver_module.c "$@" src/boardops.c
