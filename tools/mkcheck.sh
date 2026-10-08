#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Laine Jones
# Stage the PrismCheck drawer (out/pkg-check/PrismCheck + PrismCheck.info) from
# out/ and dist/check/. ./build.sh runs it after mkpkg.sh.
set -e
O=out/pkg-check
P=$O/PrismCheck
rm -rf $O
mkdir -p $P/Tools
cp dist/check/PrismCheck dist/check/ReadMe LICENSE $P/
cp dist/check/PrismCheck.info dist/check/ReadMe.info $P/
# the drawer's own icon goes BESIDE it, or Workbench doesn't show the drawer
cp dist/check/PrismCheck-drawer.info $O/PrismCheck.info
for t in tmpltest p96test vramcheck PrismBench PrismProbe prismdiag lastalert prismstate; do
    cp out/$t $P/Tools/
done
ls -l $P $P/Tools
