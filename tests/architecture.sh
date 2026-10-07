#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
set -eu
mkdir -p out/tests
for name in boardops cirrus_limits; do
    ${CC:-m68k-amigaos-gcc} -O2 -m68020-60 -noixemul -Wall -Wextra -Wno-pointer-sign -Wno-missing-field-initializers -Wno-unused-parameter \
        -Isrc -o out/tests/$name tests/$name.c src/boardops.c
    ${VAMOS:-vamos} --vols-base-dir out/tests/volumes -C 68020 -s 128 out/tests/$name
done
sh tests/p96-adapter.sh
