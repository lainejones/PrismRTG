#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Laine Jones
# Copyright (C) 2026 Stefan Reinauer
# Stage the release drawer out/pkg/PrismRTG (+ PrismRTG.info) from out/ and
# dist/. ./build.sh runs it last, after every program is built.
#
# The icons are finished .info files in dist/. They were drawn with
# tools/mkicons.py (the prism and PrismPrefs icons) and the author's icon
# writer; to change one, regenerate it there and commit the new .info.
set -e
P=out/pkg/PrismRTG
rm -rf out/pkg
mkdir -p $P/Drivers
for driver in PICASSO2 ZZ9000 Z3660 P96 UAEGFX; do
    cp out/Drivers/$driver.driver $P/Drivers/
done
cp out/PrismD out/PrismPrefs out/PrismSetup out/PrismProbe out/Picasso96API.library $P/
# PrismUpdate is built only with the AmiSSL SDK headers (build.sh); a package
# without it has no update check
[ -f out/PrismUpdate ] && cp out/PrismUpdate $P/ || echo "mkpkg: no PrismUpdate in this package"
cp out/PrismMon $P/Prism
# translations: every catalogs/<language>.ct becomes Catalogs/<language>/PrismRTG.catalog
for ct in catalogs/*.ct; do
    [ -f "$ct" ] || continue
    lang=$(basename "$ct" .ct)
    mkdir -p "$P/Catalogs/$lang"
    python3 tools/mkcatalog.py --build "$ct" "$P/Catalogs/$lang/PrismRTG.catalog"
done
cp dist/Install_PrismRTG dist/ReadMe LICENSE $P/
# the ReadMe as an AmigaGuide document too (MultiView), with its committed icon
python3 tools/mkguide.py dist/ReadMe $P/PrismRTG.guide
cp dist/PrismRTG.guide.info $P/
cp dist/Install_PrismRTG.info dist/Prism.info dist/PrismPrefs.info dist/ReadMe.info $P/
# the drawer's own icon goes BESIDE it, or Workbench doesn't show the drawer
cp dist/PrismRTG.info out/pkg/
ls -l $P
