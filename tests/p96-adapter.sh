#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
set -eu
mkdir -p out/tests
${CC:-m68k-amigaos-gcc} -O2 -m68020-60 -noixemul -Wall -Wextra \
    -Wno-pointer-sign -Isrc -Isrc/p96sdk \
    -o out/tests/p96_adapter tests/p96_adapter.c src/boardops.c src/rtg_ops.c
${VAMOS:-vamos} --vols-base-dir out/tests/volumes -C 68020 -s 128 out/tests/p96_adapter
