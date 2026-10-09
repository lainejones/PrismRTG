#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Laine Jones
# Fetch the public sources PrismRTG's hardware knowledge came from (and the
# closest other implementations of the same interfaces) into a folder of
# their own, for tools/provenance/compare.py. Nothing fetched is built or run.
#   sh tools/provenance/fetch.sh [DIR]      (default: out/provenance-src)
set -e
D=${1:-out/provenance-src}
rm -rf "$D" && mkdir -p "$D" && cd "$D"
get() { mkdir -p "$1"; curl -sSL --fail --max-time 120 -o "$1/$(basename "$2")" "$2"; echo "$1/$(basename "$2")"; }
get netbsd https://raw.githubusercontent.com/NetBSD/src/trunk/sys/arch/amiga/dev/grf_cl.c
get netbsd https://raw.githubusercontent.com/NetBSD/src/trunk/sys/arch/amiga/dev/grf_clreg.h
get linux  https://raw.githubusercontent.com/torvalds/linux/master/drivers/video/fbdev/cirrusfb.c
get linux  https://raw.githubusercontent.com/torvalds/linux/master/include/video/cirrus.h
get winuae https://raw.githubusercontent.com/tonioni/WinUAE/master/gfxboard.cpp
get winuae https://raw.githubusercontent.com/tonioni/WinUAE/master/od-win32/picasso96_win.cpp
get winuae https://raw.githubusercontent.com/tonioni/WinUAE/master/pcem/vid_cl5429.cpp
get qemu   https://raw.githubusercontent.com/qemu/qemu/master/hw/display/cirrus_vga.c
get 86box  https://raw.githubusercontent.com/86Box/86Box/master/src/video/vid_cl54xx.c
X=https://gitlab.freedesktop.org/xorg/driver/xf86-video-cirrus/-/raw
get xf86   $X/xf86-video-cirrus-1.5.3/src/alp_xaa.c     # removed after 1.5.3
for f in CirrusClk.c alp_driver.c alp_hwcurs.c cir_driver.c; do get xf86 $X/master/src/$f; done
get z3660  https://raw.githubusercontent.com/shanshe/Z3660/main/z3660-drivers/rtg/gfx.c
# MNT's ZZ9000 drivers; the P96 driver development files with their example
# Cirrus GD5434 / Piccolo SD64 driver, and open P96 card drivers
git clone -q --depth 1 https://source.mnt.re/amiga/zz9000-drivers.git zz9000-drivers
git clone -q --depth 1 https://github.com/mheyer32/p96drivers p96drivers
echo "fetched into $D"
