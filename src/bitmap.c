/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laine Jones */
/*
 * bitmap.c - Prism's chunky bitmaps (see prismint.h) and the AllocBitMap,
 * FreeBitMap and GetBitMapAttr patches.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <graphics/gfx.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <string.h>
#include "prismint.h"

static struct PBitMap *table[PBM_MAX];
static ULONG used;

struct PBitMap *pbm_get(const struct BitMap *bm)
{
    struct PBitMap *p;
    UWORD i;

    if (!bm || !(bm->Flags & BMF_PRISM) || (bm->pad & 0xf000) != PBM_PAD_MAGIC)
        return NULL;
    i = bm->pad & 0x0fff;
    if (i >= PBM_MAX || !(p = table[i]))
        return NULL;
    /* a copy of the struct still points at the same dummy plane - or, for
     * a bitmap made in a pixel format (see h_AllocBitMap), at its pixels */
    return (p->dummy->Planes[0] == bm->Planes[0] || (p->direct && bm->Planes[0] == p->pix))
           ? p : NULL;
}

ULONG pbm_count(void)
{
    return used;
}

/* ---- VRAM paging -----------------------------------------------------
 *
 * 2 MB of VRAM holds a few screens at most. Any Prism bitmap can live in
 * VRAM or fast RAM, and moves under `lock` (all drawing holds it). A
 * bitmap the card shows, or one a cgx client has locked, stays put. */

static ULONG pbm_bytes(struct PBitMap *p)
{
    return p->bpr * p->h;
}

/* VRAM for `size` bytes, evicting least recently shown bitmaps (never
 * `keep`) until it fits. Caller holds lock. */
static LONG vram_get(ULONG size, struct PBitMap *keep)
{
    LONG off;

    while ((off = vram_alloc(size)) < 0) {
        struct PBitMap *v = NULL;
        UWORD i;
        for (i = 0; i < PBM_MAX; i++) {
            struct PBitMap *q = table[i];
            if (q && q != keep && q->inVram && !q->locks && !pbm_is_shown(q) &&
                (!v || q->lastShown < v->lastShown))
                v = q;
        }
        if (!v || !pbm_to_fast(v))
            return -1;
    }
    return off;
}

BOOL pbm_to_fast(struct PBitMap *p)
{
    ULONG size = pbm_bytes(p);
    APTR mem;

    if (!p->inVram)
        return TRUE;
    if (p->locks || !(mem = AllocVec(size, MEMF_ANY)))
        return FALSE;
    if (board.waitBlit)
        board.waitBlit(&board);
    CopyMem(p->pix, mem, size);
    vram_free(p->vramOff);
    p->mem = mem;
    p->pix = mem;
    p->inVram = FALSE;
    dbg("vram: %ux%u x%u out to fast RAM\n", p->w, p->h, p->bpp);
    return TRUE;
}

BOOL pbm_to_vram(struct PBitMap *p)
{
    ULONG size = pbm_bytes(p);
    LONG off;

    if (p->inVram)
        return TRUE;
    if (p->locks || p->direct || (off = vram_get(size, p)) < 0)
        return FALSE;
    if (board.waitBlit)
        board.waitBlit(&board);
    CopyMem(p->mem, board.vram + off, size);
    FreeVec(p->mem);
    p->mem = NULL;
    p->inVram = TRUE;
    p->vramOff = off;
    p->pix = board.vram + off;
    dbg("vram: %ux%u x%u into VRAM +%lx\n", p->w, p->h, p->bpp, (ULONG)off);
    return TRUE;
}

struct PBitMap *pbm_new(UWORD w, UWORD h, UBYTE depth, UBYTE bpp, UWORD *penTab,
                        BOOL vram, BOOL clear)
{
    struct PBitMap *p;
    ULONG size;
    UWORD i, slot = PBM_MAX;

    if (!w || !h || !depth || depth > 8)
        return NULL;
    if (!(p = AllocVec(sizeof(*p), MEMF_PUBLIC | MEMF_CLEAR)))
        return NULL;
    p->w = w;
    p->h = h;
    p->depth = depth;
    p->bpp = bpp ? bpp : 1;
    p->penTab = penTab;
    /* callers set the real format; this is only a sane default */
    p->fmt = (p->bpp == 1) ? PF_CLUT8 : (p->bpp == 3) ? PF_BGR24 : (p->bpp == 4) ? PF_ARGB32 :
             board.formats & PF_BIT(PF_RGB565BE) ? PF_RGB565BE :
             board.formats & PF_BIT(PF_BGR565LE) ? PF_BGR565LE : PF_RGB565LE;
    p->bpr = ((ULONG)w * p->bpp + 7) & ~7UL; /* 8-byte rows suit both blitters */
    if (board.bytesPerRow && vram) {
        p->bpr = board.bytesPerRow(&board, w, h, p->bpp);
        if (p->bpr < (ULONG)w * p->bpp) {
            FreeVec(p);
            return NULL;
        }
    }
    size = p->bpr * h;

    ObtainSemaphore(&lock);
    for (i = 0; i < PBM_MAX; i++)
        if (!table[i]) { slot = i; break; }
    if (slot < PBM_MAX) {
        table[slot] = p;               /* reserve the slot */
        used++;
    }
    ReleaseSemaphore(&lock);
    if (slot == PBM_MAX)
        goto fail;
    p->index = slot;

    if (vram) {
        /* VRAM if it fits (evicting hidden bitmaps), else fast RAM until
         * the bitmap is shown */
        LONG off;
        ObtainSemaphore(&lock);
        /* vram 2 = free VRAM only (off-screen bitmaps never evict) */
        off = (vram == 2) ? vram_alloc(size) : vram_get(size, NULL);
        ReleaseSemaphore(&lock);
        if (off >= 0) {
            p->inVram = TRUE;
            p->vramOff = off;
            p->pix = board.vram + off;
        }
    }
    if (!p->inVram) {
        if (!(p->mem = AllocVec(size, MEMF_ANY | (clear ? MEMF_CLEAR : 0))))
            goto fail;
        p->pix = p->mem;
    }

    /* the planar face: all planes alias one cleared chip plane */
    if (!(p->dummy = AllocBitMap(w, h, 1, BMF_CLEAR, NULL)))
        goto fail;
    if (!(p->bm = AllocVec(sizeof(struct BitMap), MEMF_PUBLIC | MEMF_CLEAR)))
        goto fail;
    InitBitMap(p->bm, depth, w, h);
    p->bm->BytesPerRow = p->dummy->BytesPerRow;
    p->bm->Rows = p->dummy->Rows;
    for (i = 0; i < depth; i++)
        p->bm->Planes[i] = p->dummy->Planes[0];
    p->bm->Flags = BMF_PRISM;
    p->bm->pad = PBM_PAD_MAGIC | slot;

    if (clear && p->inVram) {
        ObtainSemaphore(&lock);
        if (board.fillRect) {
            /* zero is zero at any depth: clear the rows as bytes */
            board.fillRect(&board, p->vramOff, p->bpr, 1, 0, 0, p->bpr, h, 0);
        } else {
            memset(p->pix, 0, size);
        }
        ReleaseSemaphore(&lock);
    }
    return p;

fail:
    pbm_free(p);
    return NULL;
}

void pbm_free(struct PBitMap *p)
{
    if (!p)
        return;
    ObtainSemaphore(&lock);
    if (p->index < PBM_MAX && table[p->index] == p) {
        pbm_gone(p);
        table[p->index] = NULL;
        used--;
    }
    if (p->inVram)
        vram_free(p->vramOff);
    ReleaseSemaphore(&lock);
    if (p->dummy) {
        WaitBlit();
        FreeBitMap(p->dummy);
    }
    if (p->bm) FreeVec(p->bm);
    if (p->mem) FreeVec(p->mem);
    FreeVec(p);
}

/* ---- patches -------------------------------------------------------- */

/* AllocBitMap(sizex d0, sizey d1, depth d2, flags d3, friend a0)
 * A bitmap whose friend is a Prism bitmap is made chunky too, so that
 * off-screen buffers (layers' backing store, double buffers) stay in the
 * same format as the screen. */
LONG h_AllocBitMap(struct Regs *r)
{
    TRACE("AllocBitMap");
    struct PBitMap *f = pbm_get((struct BitMap *)r->a[0]), *p;
    ULONG depth = r->d[2];

    /* a screen Intuition is opening on a Prism mode? */
    if ((p = screen_bitmap_hook(r->d[0], r->d[1], depth, r->d[3]))) {
        r->d[0] = (LONG)p->bm;
        return 1;
    }
    if (!r->d[0] || !r->d[1])
        return 0;
    if (depth > 8) depth = 8;           /* pens Intuition-style; pixels per fmt */
    if (!depth) depth = 1;

    /* CyberGraphX-style request for a pixel format: BMF_SPECIALFMT (bit 7)
     * with SHIFT_PIXFMT(fmt) in the top byte. Prism stores LUT8 and the
     * board's 16-bit format; anything else is graphics.library's answer. */
    if (r->d[3] & 0x80) {
        ULONG pf = (ULONG)r->d[3] >> 24;
        UBYTE fmt = pf_from_pixfmt(pf), bpp;
        dbg("AllocBitMap special: %ldx%ld pixfmt %lu -> %s\n", r->d[0], r->d[1], pf,
            fmt != PF_COUNT ? "Prism" : "not ours");
        if (fmt == PF_COUNT)
            return 0;
        bpp = pf_bpp(fmt);
        /* off-screen image buffers in any of these formats; blits to the
         * screen convert */
        p = pbm_new(r->d[0], r->d[1], depth, bpp,
                    (f && f->fmt == fmt) ? f->penTab : NULL, FALSE, TRUE);
        if (p) {
            p->fmt = fmt;
            p->rgbTab = f ? f->rgbTab : NULL;
            /* A bitmap asked for in a pixel format lives in fast RAM and
             * never moves, and programs written for CyberGraphX and
             * Picasso96 reach into it: Planes[0] is the pixel memory and
             * BytesPerRow its row size there, and they read and write it
             * without LockBitMap (PerfectPaint draws a pattern with
             * RectFill, then picks the bytes up from Planes[0]). With the
             * dummy plane in Planes[0] that ran off the end of a few bytes
             * of chip RAM and took the machine down. The other planes keep
             * the dummy, and nothing planar is ever done to these. */
            if (!p->inVram) {
                p->direct = TRUE;
                /* Every plane, not just the first: with BytesPerRow the
                 * pixel row size, a planar operation that gets past Prism
                 * walks rows * BytesPerRow bytes of each plane - 960 KB of
                 * a 60 KB dummy plane for 800x600 in 16 bits, and the
                 * memory lists behind it (Sudoku, an SDL game, hung the
                 * machine on its next AllocBitMap). Aimed at the pixels,
                 * the worst it can do is scribble on the image. */
                int i;
                for (i = 0; i < 8; i++)
                    p->bm->Planes[i] = p->pix;
                p->bm->BytesPerRow = p->bpr;
            }
        }
        r->d[0] = p ? (LONG)p->bm : 0;
        return 1;
    }
    dbg("AllocBitMap %ldx%ld depth %lu flags %lx friend %s\n", r->d[0], r->d[1], depth,
        (ULONG)r->d[3], f ? "prism" : "-");
    if (!f)
        return 0;
    /* A displayable bitmap with a screen's bitmap as friend is a buffer
     * for that screen (AllocScreenBuffer): VRAM if it fits, and showable
     * through ChangeVPBitMap. */
    {
        BOOL disp = (r->d[3] & BMF_DISPLAYABLE) && f->owner &&
                    r->d[0] == f->w && r->d[1] == f->h;
        /* other friends go to free VRAM if there is some: blits between
         * them and the screen then run on the card's blitter */
        p = pbm_new(r->d[0], r->d[1], depth, f->bpp, f->penTab, disp ? 1 : 2, TRUE);
        if (p && disp)
            p->owner = f->owner;
    }
    if (p) {
        p->fmt = f->fmt;
        p->rgbTab = f->rgbTab;
    }
    r->d[0] = p ? (LONG)p->bm : 0;
    return 1;
}

/* FreeBitMap(bm a0) */
LONG h_FreeBitMap(struct Regs *r)
{
    TRACE("FreeBitMap");
    struct BitMap *bm = (struct BitMap *)r->a[0];
    struct PBitMap *p = pbm_get(bm);

    /* only the BitMap pbm_new handed out frees it (not a copy) */
    if (!p || p->bm != bm)
        return 0;
    pbm_free(p);
    return 1;
}

/* GetBitMapAttr(bm a0, attr d1) */
LONG h_GetBitMapAttr(struct Regs *r)
{
    TRACE("GetBitMapAttr");
    struct PBitMap *p = pbm_get((struct BitMap *)r->a[0]);

    if (!p)
        return 0;
    switch (r->d[1]) {
    case BMA_HEIGHT: r->d[0] = p->h; break;
    case BMA_DEPTH:
        /* A deep bitmap says how deep it really is, as Picasso96 and
         * CyberGraphX bitmaps do: programs (MUI, IBrowse) decide between
         * their palette and true-colour paths on this. Inside Prism it
         * still has `depth` planes' worth of pens. */
        r->d[0] = p->bpp == 1 ? p->depth : p->bpp == 3 ? 24 : p->bpp == 4 ? 32 :
                  (p->fmt == PF_RGB555LE || p->fmt == PF_RGB555BE || p->fmt == PF_BGR555LE) ? 15 : 16;
        break;
    case BMA_WIDTH:
        /* graphics.library answers BytesPerRow * 8: the width rounded up
         * to 16 pixels. Programs size mask planes from it (IBrowse's
         * toolbar: with the exact width here its mask rows were a byte
         * short of what BltMaskBitMapRastPort steps by, and the images
         * came out shredded). */
        r->d[0] = p->direct ? (((ULONG)p->w + 15) & ~15UL) : (ULONG)p->bm->BytesPerRow * 8;
        break;
    case BMA_FLAGS:  r->d[0] = p->inVram ? BMF_DISPLAYABLE : 0; break;
    default:         r->d[0] = 0; break;
    }
    return 1;
}
