#!/bin/sh
# mkadf.sh - the install floppy: out/PrismRTG-<release>.adf, an 880 KB (DD)
# FFS disk named PrismRTG with the install drawer's contents at its root
# (insert it, open it, double-click Install_PrismRTG). The whole drawer fits.
# Run in WSL after mkpkg.sh; needs amitools' xdftool (~/amitools-venv).
#
# SPDX-License-Identifier: GPL-3.0-only
set -e
cd "$(dirname "$0")/.."
X=${XDFTOOL:-$HOME/amitools-venv/bin/xdftool}
VER=$(sed -n 's/^#define PRISM_RELEASE *"\(.*\)"/\1/p' src/prism.h)
D=out/pkg/PrismRTG
ADF=$(pwd)/out/PrismRTG-$VER.adf
[ -d "$D" ] || { echo "mkadf.sh: no $D - run tools/mkpkg.sh first" >&2; exit 1; }

S=$(mktemp -d)
cp -r "$D/." "$S/"
cp dist/Disk.info "$S/Disk.info"

rm -f "$ADF"
"$X" "$ADF" create + format PrismRTG ffs >/dev/null
cd "$S"
for d in $(find . -mindepth 1 -type d | sort); do
    "$X" "$ADF" makedir "${d#./}" >/dev/null
done
for f in $(find . -type f | sort); do
    if ! "$X" "$ADF" write "$f" "${f#./}" >/dev/null 2>&1; then
        echo "mkadf.sh: $f does not fit on the floppy" >&2
        rm -f "$ADF"
        exit 1
    fi
done
cd - >/dev/null
rm -rf "$S"
"$X" "$ADF" info | sed -n 's/^\(used\|free\):/  &/p'
echo "mkadf.sh: $ADF"
