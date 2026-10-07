/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PRISM_BOARDOPS_H
#define PRISM_BOARDOPS_H
#include "prismboard.h"
#include <graphics/gfx.h>

/* DECLINED guarantees no writes. FAILED may have modified the destination;
 * the driver must stop its engine before returning. Never replay FAILED. */
enum PrismResult { PR_DECLINED = 0, PR_DONE = 1, PR_FAILED = 2 };
#define PSF_VRAM 1
struct PrismSurface {
    UBYTE *memory;               /* stable CPU view, possibly a shadow */
    ULONG offset, allocation;    /* logical VRAM offset and allocation size */
    ULONG pitch;
    UWORD width, height;
    UBYTE format, bpp;
    UWORD flags;
};
struct PrismPlanar {
    const struct BitMap *bitmap;
    const ULONG *colours;         /* 256 entries of 0x00RRGGBB, or NULL */
    const UWORD *pens16;          /* optional packed 16-bit pen overrides */
};
struct PrismOps {
    enum PrismResult (*fill)(struct PrismBoard *, const struct PrismSurface *,
        UWORD, UWORD, UWORD, UWORD, ULONG);
    enum PrismResult (*copy)(struct PrismBoard *, const struct PrismSurface *,
        const struct PrismSurface *, UWORD, UWORD, UWORD, UWORD, UWORD, UWORD);
    enum PrismResult (*expand)(struct PrismBoard *, const struct PrismSurface *,
        UWORD, UWORD, UWORD, UWORD, const UBYTE *, ULONG, ULONG, ULONG, BOOL);
    enum PrismResult (*line)(struct PrismBoard *, const struct PrismSurface *,
        WORD, WORD, WORD, WORD, ULONG);
    enum PrismResult (*planar)(struct PrismBoard *, const struct PrismPlanar *,
        const struct PrismSurface *, UWORD, UWORD, UWORD, UWORD, UWORD, UWORD,
        UBYTE, UBYTE);            /* minterm and plane mask */
    ULONG (*pitch)(struct PrismBoard *, UWORD, UWORD, UBYTE); /* full format */
    /* Transfer a range between stable CPU storage and device memory. The
     * driver controls aperture/bank changes. Caller holds the board lock. */
    BOOL (*read)(struct PrismBoard *, const struct PrismSurface *, ULONG, APTR, ULONG);
    BOOL (*write)(struct PrismBoard *, const struct PrismSurface *, ULONG, const void *, ULONG);
};
#define PBF_SHADOW (1UL << 6)     /* CPU access goes through read/write */
#define PBF_SOFTWARE (1UL << 8)
#define PBF_ACCEL_BROKEN (1UL << 7)

UBYTE board_bpp(UBYTE format);
BOOL board_rect(const struct PrismSurface *, UWORD, UWORD, UWORD, UWORD);
enum PrismResult board_fill(struct PrismBoard *, const struct PrismSurface *,
    UWORD, UWORD, UWORD, UWORD, ULONG);
enum PrismResult board_copy(struct PrismBoard *, const struct PrismSurface *,
    const struct PrismSurface *, UWORD, UWORD, UWORD, UWORD, UWORD, UWORD);
enum PrismResult board_expand(struct PrismBoard *, const struct PrismSurface *,
    UWORD, UWORD, UWORD, UWORD, const UBYTE *, ULONG, ULONG, ULONG, BOOL);
enum PrismResult board_line(struct PrismBoard *, const struct PrismSurface *,
    WORD, WORD, WORD, WORD, ULONG);
enum PrismResult board_planar(struct PrismBoard *, const struct PrismPlanar *,
    const struct PrismSurface *, UWORD, UWORD, UWORD, UWORD, UWORD, UWORD, UBYTE, UBYTE);
#endif
