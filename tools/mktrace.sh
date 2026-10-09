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
#
# The source list is PrismD's line in build.sh (board drivers are loadable
# modules, not linked in); keep the two in step.
set -e
export PATH=/opt/amiga/bin:$PATH
mkdir -p out/trace
# The traced copy of prismd.c lives under out/, never in src/.
if [ "${1:-}" = ALL ]; then
  sed "s/^static BOOL traceAll;/static BOOL traceAll = 1;/" src/prismd.c > out/trace/prismd_trace.c
else
  cp src/prismd.c out/trace/prismd_trace.c
fi
m68k-amigaos-gcc -DPRISM_TRACE -DPRISM_DEBUG -O2 -m68020-60 -noixemul -Wall -Wno-pointer-sign \
  -fomit-frame-pointer -ffunction-sections -fdata-sections -Wl,--gc-sections \
  -Isrc -Isrc/p96sdk -s -o out/PrismD.trace out/trace/prismd_trace.c src/prefs.c src/bitmap.c \
  src/present.c src/render.c src/pointer.c src/cgx.c src/p96.c src/stubs.S src/p2c.S \
  src/boardops.c src/driver_loader.c
rm -f out/trace/prismd_trace.c
ls -l out/PrismD.trace
