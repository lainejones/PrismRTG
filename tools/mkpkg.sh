#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Laine Jones
# Stage the release drawer out/pkg/PrismRTG (+ PrismRTG.info) from out/ and
# dist/. Run inside WSL after ./build.sh. The icon tools are in a separate
# directory (TOOLS=...); without them the drawer is staged without icons.
set -e
T=${TOOLS:-/mnt/c/projects/tools}
P=out/pkg/PrismRTG
rm -rf out/pkg
mkdir -p $P
cp out/PrismD out/PrismPrefs out/PrismSetup out/PrismProbe out/Picasso96API.library $P/
cp out/PrismMon $P/Prism
cp dist/Install_PrismRTG dist/ReadMe LICENSE $P/
if [ -f "$T/iconlib.py" ]; then
AMIGA_TOOLS="$T" python3 tools/mkicons.py prefs $P/PrismPrefs.info
AMIGA_TOOLS="$T" python3 tools/mkicons.py prism $P/Prism.info
python3 "$T/makeicon_doc.py" $P/ReadMe.info >/dev/null
python3 "$T/makeicon_drawer.py" out/pkg/PrismRTG.info >/dev/null
# Install_PrismRTG.info: a project icon for the standard Installer
python3 - "$T" "$P/Install_PrismRTG.info" <<'PY'
import sys
sys.path.insert(0, sys.argv[1])
import iconlib, makeicon_install as mi
open(sys.argv[2], 'wb').write(
    iconlib.build_info(mi.build_cidx(), mi.PALETTE, mi.PLANAR_MAP, mi.W, mi.H, mi.TRANSPARENT,
                       icon_type=4, default_tool='Installer',
                       tool_types=['APPNAME=PrismRTG', 'SCRIPT=Install_PrismRTG', 'DEFUSER=AVERAGE',
                                   'MINUSER=AVERAGE', 'LOG=FALSE']))
PY
else
echo "mkpkg: no icon tools in $T - all icons left out"
fi
ls -l $P
