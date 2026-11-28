#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Laine Jones
# Copyright (C) 2026 Stefan Reinauer
# Build Prism with amiga-gcc: a wrapper around the Makefile, kept for scripts
# and CI. Run inside WSL or Linux:
#   cd /mnt/c/projects/PrismRTG && ./build.sh
# Arguments go to make (./build.sh clean, ./build.sh drivers, ...). Builds in
# parallel, each target's output kept together; JOBS=1 for one at a time.
set -e
cd "$(dirname "$0")"
make -j"${JOBS:-$(nproc 2>/dev/null || echo 1)}" --output-sync=target "$@"
if [ $# -eq 0 ]; then echo "Done."; fi
