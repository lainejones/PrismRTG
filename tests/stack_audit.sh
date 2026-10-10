#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Laine Jones
# Stack audit: PrismD's patches run on the CALLING program's stack (Workbench,
# IPrefs, input.device, any application), often 4 KB and already deep in
# Intuition and layers. This compiles PrismD to assembly with -fstack-usage,
# reads every function's frame (.su) and its calls (.s: jsr/jbsr/bsr to a
# symbol; a function whose address is taken counts as called by whoever takes
# it; a call through a register reaches every render callback and every
# driver function), and reports the deepest chain below every patched entry
# point. It fails if any chain is over the budget (STACK_BUDGET, default 1536).
# Run inside WSL from the repository root: sh tests/stack_audit.sh
set -eu
export PATH=/opt/amiga/bin:$PATH
CC=${CC:-m68k-amigaos-gcc}
OUT=out/stack
rm -rf $OUT && mkdir -p $OUT
F="-DPRISM_DEBUG -O2 -m68020-60 -noixemul -Wall -Wno-pointer-sign -fomit-frame-pointer -fstack-usage -Isrc -Isrc/p96sdk"
for s in prismd prefs bitmap present render gels selftest pointer cgx p96 boardops driver_loader; do
    $CC $F -S -o $OUT/$s.s src/$s.c
done
for s in drv_picasso2 drv_zz9000 drv_p96 drv_uaegfx rtg_ops; do
    $CC $F -DPRISM_DRIVER_MODULE -DDRIVER_NAME='"X"' -S -o $OUT/$s.s src/$s.c
done
mv src/*.su $OUT/ 2>/dev/null || true
mv ./*.su $OUT/ 2>/dev/null || true
mv $OUT/../*.su $OUT/ 2>/dev/null || true
python3 tests/stack_audit.py $OUT "${STACK_BUDGET:-1536}"
