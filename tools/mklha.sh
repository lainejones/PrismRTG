#!/bin/sh
# mklha.sh - the release archive: out/PrismRTG-<release>.lha, the install
# drawer and its icon (PrismRTG.info, PrismRTG/...) as LhA unpacks them
# beside each other. PrismUpdate GETs this archive and unpacks it with C:LhA.
# Run in WSL after mkpkg.sh; needs a creating lha (LHa for UNIX, which
# amiga-gcc installs in /opt/amiga/bin; Lhasa only unpacks).
#
# SPDX-License-Identifier: GPL-3.0-only
set -e
cd "$(dirname "$0")/.."
PATH=/opt/amiga/bin:$PATH
L=${LHA:-lha}
VER=$(sed -n 's/^#define PRISM_RELEASE *"\(.*\)"/\1/p' src/prism.h)
P=out/pkg
LHA_OUT=$(pwd)/out/PrismRTG-$VER.lha
[ -d "$P/PrismRTG" ] || { echo "mklha.sh: no $P/PrismRTG - run tools/mkpkg.sh first" >&2; exit 1; }
"$L" --help 2>&1 | grep -q "^ *a  *Add" || { echo "mklha.sh: $L cannot create archives (Lhasa?); set LHA=" >&2; exit 1; }

rm -f "$LHA_OUT"
cd "$P"
# files only, so LhA makes the drawers as it unpacks; -lh5- like the releases
"$L" -aqo5 "$LHA_OUT" PrismRTG.info $(find PrismRTG -type f | sort)
cd - >/dev/null
echo "mklha.sh: $LHA_OUT ($(wc -c < "$LHA_OUT") bytes)"
