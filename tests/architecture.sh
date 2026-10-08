#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
set -eu
mkdir -p out/tests
for name in boardops cirrus_limits zz_surfaces render_replay damage shadow_upload pointer_mode presentation; do
    extra=
    if [ "$name" = render_replay ]; then extra=src/p2c.S; fi
    ${CC:-m68k-amigaos-gcc} -O2 -ffunction-sections -fdata-sections -Wl,--gc-sections -m68020-60 -noixemul -Wall -Wextra -Wno-pointer-sign -Wno-missing-field-initializers -Wno-unused-parameter -Wno-sign-compare \
        -Isrc -o out/tests/$name tests/$name.c src/boardops.c $extra
    ${VAMOS:-vamos} --vols-base-dir out/tests/volumes -C 68020 -s 128 out/tests/$name
done
sh tests/p96-adapter.sh

# Dedicated-boot tools use the same shared RTG implementation as PrismD.
for name in p96_guest uaegfx_guest; do
    ${CC:-m68k-amigaos-gcc} -O2 -m68020-60 -noixemul -Wall -Wno-pointer-sign \
        -Isrc -Isrc/p96sdk -o out/tests/$name tests/$name.c src/boardops.c src/rtg_ops.c
done
