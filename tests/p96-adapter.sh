#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Stefan Reinauer
# Copyright (C) 2026 Laine Jones
# P96 adapter fixture: drv_p96.c's 68k register ABI, software fallbacks and
# mode setup, run as an Amiga program under vamos. Run inside WSL from the
# repository root (tests/architecture.sh runs it too).
set -eu
export PATH=/opt/amiga/bin:$PATH
CC=${CC:-m68k-amigaos-gcc}
VAMOS=${VAMOS:-$(command -v vamos || echo "$HOME/amitools-venv/bin/vamos")}
mkdir -p out/tests
$CC -O2 -m68020-60 -noixemul -Wall -Wextra \
    -Wno-pointer-sign -Isrc -Isrc/p96sdk \
    -o out/tests/p96_adapter tests/p96_adapter.c src/boardops.c src/rtg_ops.c
echo "== p96_adapter"
"$VAMOS" --vols-base-dir out/tests/volumes -C 68020 -s 128 out/tests/p96_adapter
