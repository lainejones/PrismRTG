#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Laine Jones
# Build out/PrismD.trace: PrismD that writes every call it handles to the
# debug log and checks exec's memory lists on each one. Use with
# "prismlog SYNC": the log then ends at the last call a program made
# before it took the machine down, and names the call after which memory
# was found damaged. "sh tools/mktrace.sh ALL" traces every
# graphics.library vector as well (slow, and heavy on small stacks).
# Run inside WSL.
set -e
export PATH=/opt/amiga/bin:$PATH
if [ "$1" = ALL ]; then
  sed "s/^static BOOL traceAll;/static BOOL traceAll = 1;/" src/prismd.c > src/prismd_trace.c
else
  cp src/prismd.c src/prismd_trace.c
fi
trap 'rm -f src/prismd_trace.c' EXIT
m68k-amigaos-gcc -DPRISM_TRACE -DPRISM_DEBUG -O2 -m68020-60 -noixemul -Wall -Wno-pointer-sign \
  -fomit-frame-pointer -Isrc -Isrc/p96sdk -s -o out/PrismD.trace src/prismd_trace.c src/prefs.c src/bitmap.c \
  src/render.c src/pointer.c src/cgx.c src/p96.c src/stubs.S src/p2c.S src/drv_picasso2.c src/drv_zz9000.c src/drv_p96.c src/drv_uaegfx.c
ls -l out/PrismD.trace
