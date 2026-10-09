#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Host test suite: builds each fixture with amiga-gcc and runs it as an
# AmigaOS CLI program under vamos (amitools). Guest exercisers that need a
# real Amiga boot are built, so they keep compiling, and reported as SKIP.
# Run inside WSL from the repository root:  sh tests/architecture.sh
# See tests/README.md.
set -eu
export PATH=/opt/amiga/bin:$PATH
CC=${CC:-m68k-amigaos-gcc}
VAMOS=${VAMOS:-$(command -v vamos || echo "$HOME/amitools-venv/bin/vamos")}
export CC VAMOS
OUT=out/tests
mkdir -p $OUT

# Fixtures include driver sources directly and define the library bases
# themselves, so nothing is opened beyond what vamos provides.
UNIT_FLAGS="-O2 -ffunction-sections -fdata-sections -Wl,--gc-sections -m68020-60 -noixemul \
 -Wall -Wextra -Wno-pointer-sign -Wno-missing-field-initializers -Wno-unused-parameter \
 -Wno-sign-compare -Isrc -Isrc/p96sdk"
GUEST_FLAGS="-O2 -m68020-60 -noixemul -Wall -Wno-pointer-sign -Isrc -Isrc/p96sdk"

run() {
    echo "== $1"
    "$VAMOS" --vols-base-dir $OUT/volumes -C 68020 -s 128 $OUT/$1
}

# ---- unit fixtures (vamos) ------------------------------------------
for name in boardops cirrus_limits zz_surfaces render_replay damage shadow_upload \
            pointer_mode presentation driver_module; do
    extra=
    if [ "$name" = render_replay ]; then extra=src/p2c.S; fi
    $CC $UNIT_FLAGS -o $OUT/$name tests/$name.c src/boardops.c $extra
    run $name
done
sh tests/p96-adapter.sh

# ---- guest exercisers (build only) ----------------------------------
# Dedicated-boot tools use the same shared RTG implementation as PrismD.
G=$OUT/guest
mkdir -p $G/Drivers
for name in p96_guest uaegfx_guest; do
    $CC $GUEST_FLAGS -o $G/$name tests/$name.c src/boardops.c src/rtg_ops.c
done
$CC $GUEST_FLAGS -o $G/driver_guest tests/driver_guest.c
$CC $GUEST_FLAGS -o $G/driver_module_guest tests/driver_module_guest.c
$CC $GUEST_FLAGS -DMODULE_CHECK -o $G/ModuleCheck tests/driver_module_guest.c src/driver_loader.c
$CC $GUEST_FLAGS -o $G/Drivers/Fixture.driver tests/module_fixture.c
$CC $GUEST_FLAGS -DFIXTURE_NOT_FOUND -o $G/Drivers/FailedFixture.driver tests/module_fixture.c
$CC $GUEST_FLAGS -DPRISM_DRIVER_ABI=2 -o $G/Drivers/OldFixture.driver tests/module_fixture.c
echo "== SKIP p96_guest, uaegfx_guest: need uaegfx.card / UAE RTG in a dedicated WinUAE boot (built in $G)"
echo "== SKIP driver_guest: needs C:PrismD, C:p96test and a board in a dedicated boot (built in $G)"
echo "== SKIP driver_module_guest + ModuleCheck: vamos has no CreateNewProc, so a driver"
echo "        process cannot be started on the host (built in $G with Drivers/*Fixture.driver)"
echo "All host tests passed."
