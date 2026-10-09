/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laine Jones
 * Copyright (C) 2026 Stefan Reinauer */
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
#include "damage.h"

struct PBitMap *pbmTable[PBM_MAX];     /* pbm_get (prismint.h) reads it inline */
#define table pbmTable
static ULONG used;

ULONG pbm_count(void)
{
    return used;
}

BOOL pbm_live(const struct PBitMap *p)
{
    UWORD i;
    for (i = 0; i < PBM_MAX; i++)
        if (table[i] == p)
            return TRUE;
    return FALSE;
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
 * `keep`, never one the card shows) until it fits. Caller holds lock. */
LONG vram_get_size(ULONG size, UBYTE fmt, ULONG pitch, UWORD w, UWORD h, struct PBitMap *keep)
{
    LONG off;

    while ((off = vram_alloc(size,fmt,pitch,w,h)) < 0) {
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

static LONG vram_get(struct PBitMap *p, struct PBitMap *keep)
{
    return vram_get_size(pbm_bytes(p), p->fmt, p->bpr, p->w, p->h, keep);
}

BOOL pbm_to_fast(struct PBitMap *p)
{
    ULONG size = pbm_bytes(p);
    APTR mem;

    if (!p->inVram)
        return TRUE;
    if (p->locks && !(board.flags & PBF_SHADOW)) return FALSE;
    if ((board.flags & PBF_SHADOW) && p->mem) {
        vram_free(p->vramOff);
        p->inVram = FALSE;
        if (p->uploaded) FreeVec(p->uploaded);
        p->uploaded = NULL;
        p->uploadValid = FALSE;
        return TRUE;
    }
    if (!(mem = AllocVec(size, MEMF_ANY)))
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
    if (((p->locks || p->direct) && !(board.flags & PBF_SHADOW)) ||
        (off = vram_get(p, p)) < 0)
        return FALSE;
    if (board.waitBlit)
        board.waitBlit(&board);
    if (board.flags & PBF_SHADOW) {
        p->inVram = TRUE; p->vramOff = off; p->uploadValid = FALSE;
        if (pbm_upload(p)) return TRUE;
        pbm_to_fast(p);
        return FALSE;
    }
    CopyMem(p->mem, board.vram + off, size);
    FreeVec(p->mem);
    p->mem = NULL;
    p->inVram = TRUE;
    p->vramOff = off;
    p->pix = board.vram + off;
    dbg("vram: %ux%u x%u into VRAM +%lx\n", p->w, p->h, p->bpp, (ULONG)off);
    return TRUE;
}

/* A driver's format change may rebuild its VRAM pool. Shadow pointers stay
 * valid even for locked/direct bitmaps while their device storage is freed. */
BOOL pbm_prepare_mode(void)
{
    UWORD i;
    if (!(board.flags & PBF_REINIT)) return TRUE;
    if (!(board.flags & PBF_SHADOW)) return FALSE;
    for (i=0;i<PBM_MAX;i++)
        if (table[i] && table[i]->inVram && !pbm_to_fast(table[i])) return FALSE;
    return TRUE;
}

struct PBitMap *pbm_new(UWORD w, UWORD h, UBYTE depth, UBYTE format, UWORD *penTab,
                        BOOL vram, BOOL clear)
{
    struct PBitMap *p;
    ULONG size;
    UWORD i, slot = PBM_MAX;

    if (!w || !h || !depth || depth > 8 || format >= PF_COUNT)
        return NULL;
    if (!(p = AllocVec(sizeof(*p), MEMF_PUBLIC | MEMF_CLEAR)))
        return NULL;
    p->w = w;
    p->h = h;
    p->depth = depth;
    p->bpp = board_bpp(format);
    p->fmt = format;
    p->penTab = penTab;
    p->bpr = ((ULONG)w * p->bpp + 7) & ~7UL; /* 8-byte rows suit both blitters */
    if (vram && ((board.ops && board.ops->pitch) || board.bytesPerRow)) {
        p->bpr = board.ops && board.ops->pitch ? board.ops->pitch(&board,w,h,format) :
                 board.bytesPerRow(&board, w, h, p->bpp);
        if (p->bpr < (ULONG)w * p->bpp) {
            FreeVec(p);
            return NULL;
        }
    }
    size = p->bpr * h;

    LOCK_FOR(p);
    for (i = 0; i < PBM_MAX; i++)
        if (!table[i]) { slot = i; break; }
    if (slot < PBM_MAX) {
        table[slot] = p;               /* reserve the slot */
        used++;
        p->index = slot;
    }
    ReleaseSemaphore(&lock);
    if (slot == PBM_MAX)
        goto fail;

    if (vram) {
        /* VRAM if it fits (evicting hidden bitmaps), else fast RAM until
         * the bitmap is shown */
        LONG off;
        LOCK_FOR(p);
        /* vram 2 = free VRAM only (off-screen bitmaps never evict) */
        off = (vram == 2) ? vram_alloc(pbm_bytes(p),p->fmt,p->bpr,p->w,p->h) : vram_get(p, NULL);
        ReleaseSemaphore(&lock);
        if (off >= 0) {
            p->inVram = TRUE;
            p->vramOff = off;
            p->pix = board.vram + off;
        }
    }
    if ((board.flags & PBF_PRESENT) || !p->inVram || (board.flags & PBF_SHADOW)) {
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
        LOCK_FOR(p);
        if (pbm_hw_fill(p, 1, 0, 0, p->bpr, h, 0) != PR_DONE) {
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
    LOCK_FOR(p);
    if (p->index < PBM_MAX && table[p->index] == p) {
        pbm_gone(p);
        table[p->index] = NULL;
        used--;
    }
    if (p->inVram)
        vram_free(p->vramOff);
    ReleaseSemaphore(&lock);
    if (p->dummy) {
        blit_settle();
        FreeBitMap(p->dummy);
    }
    if (p->bm) FreeVec(p->bm);
    if (p->mem) FreeVec(p->mem);
    if (p->uploaded) FreeVec(p->uploaded);
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
        UBYTE fmt = pf_from_pixfmt(pf);
        dbg("AllocBitMap special: %ldx%ld pixfmt %lu -> %s\n", r->d[0], r->d[1], pf,
            fmt != PF_COUNT ? "Prism" : "not ours");
        if (fmt == PF_COUNT)
            return 0;
        /* off-screen image buffers in any of these formats; blits to the
         * screen convert */
        p = pbm_new(r->d[0], r->d[1], depth, fmt,
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
        p = pbm_new(r->d[0], r->d[1], depth, f->fmt, f->penTab, disp ? 1 : 2, TRUE);
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

/* Stable CPU views make application locks independent of aperture changes.
 * Linear boards retain their direct VRAM pointers and pay no copy cost. */
void pbm_surface(const struct PBitMap *p, struct PrismSurface *s)
{
    s->memory=p->pix; s->offset=p->vramOff; s->allocation=p->bpr*p->h;
    s->pitch=p->bpr; s->width=p->w; s->height=p->h;
    s->format=p->fmt; s->bpp=p->bpp; s->flags=p->inVram ? PSF_VRAM : 0;
}
static BOOL pbm_write(struct PBitMap *p,struct PrismSurface *s,ULONG off,
    const UBYTE *data,ULONG count)
{
    if (board.ops && board.ops->write) return board.ops->write(&board,s,off,data,count);
    if (board.flags & PBF_BANKED) return FALSE;
    if (board.waitBlit) board.waitBlit(&board);
    CopyMem((APTR)data,board.vram+p->vramOff+off,count);
    return TRUE;
}
BOOL pbm_upload(struct PBitMap *p)
{
    struct PrismSurface s;
    ULONG y;
    if (!p->inVram || !(board.flags & PBF_SHADOW)) return TRUE;
    pbm_surface(p,&s);
    if (!p->uploaded) p->uploaded = AllocVec(s.allocation, MEMF_ANY);
    if (!p->uploaded) {
        /* static: the board lock is held (this runs on any caller's stack) */
        static UBYTE snapshot[256];
        ULONG off;
        /* Low memory still permits presentation, without damage history.
         * Never pass a buffer that clients can change during the transfer. */
        p->uploadValid = FALSE;
        for (off=0; off<s.allocation; off+=sizeof(snapshot)) {
            ULONG count=s.allocation-off;
            if (count>sizeof(snapshot)) count=sizeof(snapshot);
            CopyMem(p->pix+off,snapshot,count);
            if (!pbm_write(p,&s,off,snapshot,count)) return FALSE;
        }
        return TRUE;
    }
    if (!p->uploadValid) {
        CopyMem(p->pix,p->uploaded,s.allocation);
        if (!pbm_write(p,&s,0,p->uploaded,s.allocation)) return FALSE;
        p->uploadValid = TRUE;
        return TRUE;
    }
    for (y=0; y<p->h; y++) {
        ULONG first, count, off=y*p->bpr;
        if (changed_span(p->pix+off,p->uploaded+off,p->bpr,&first,&count)) {
            off += first;
            /* The history is also the transfer snapshot: a direct store
             * into pix while the driver runs remains dirty next time. */
            CopyMem(p->pix+off,p->uploaded+off,count);
            if (!pbm_write(p,&s,off,p->uploaded+off,count)) {
                p->uploadValid = FALSE;
                return FALSE;
            }
        }
    }
    return TRUE;
}
static enum PrismResult pbm_result(struct PBitMap *p,enum PrismResult result,
    ULONG xbytes,UWORD y,ULONG rowbytes,UWORD h)
{
    struct PrismSurface s;
    if (result != PR_DECLINED && (board.flags & PBF_SHADOW)) {
        UWORD row;
        pbm_surface(p,&s);
        for (row=0; row<h; row++) {
            ULONG off=(ULONG)(y+row)*p->bpr+xbytes;
            UBYTE *data=p->uploaded ? p->uploaded+off : p->pix+off;
            if (board.ops && board.ops->read ?
                !board.ops->read(&board,&s,off,data,rowbytes) :
                (board.flags & PBF_BANKED) != 0) {
                board.flags |= PBF_ACCEL_BROKEN; board.faults++; result=PR_FAILED;
                p->uploadValid=FALSE;
                break;
            }
            if (!board.ops || !board.ops->read)
                CopyMem(board.vram+p->vramOff+off,data,rowbytes);
            /* As with uploads, history comes from the transfer itself,
             * never a CPU view that an unlocked client may have changed. */
            if (p->uploaded) CopyMem(data,p->pix+off,rowbytes);
        }
    }
    if (result == PR_FAILED) dbg("driver: operation failed; acceleration disabled\n");
    return result;
}
void pbm_flush_shown(void)
{
    UWORD i;
    if (!(board.flags & PBF_SHADOW) || (board.flags & PBF_PRESENT)) return;
    ObtainSemaphore(&lock);
    for (i=0;i<PBM_MAX;i++) if (table[i] && pbm_is_shown(table[i]))
        if (!pbm_upload(table[i])) dbg("driver: aperture upload failed\n");
    ReleaseSemaphore(&lock);
}
/* Surface-operation fast path (boards whose drivers only have ops): build
 * the surface here and call the driver directly. The direct-hook fast
 * paths are inline in prismint.h (pbm_fill, pbm_copy). */
#define HW_FAST(p) (HW_OK(p) && board.ops)

static inline BOOL hw_rect(const struct PBitMap *p, UWORD x, UWORD y, UWORD w, UWORD h)
{
    return w && h && (ULONG)x + w <= p->w && (ULONG)y + h <= p->h;
}

static inline void hw_surface(const struct PBitMap *p, struct PrismSurface *s)
{
    s->memory = p->pix; s->offset = p->vramOff; s->allocation = p->bpr * p->h;
    s->pitch = p->bpr; s->width = p->w; s->height = p->h;
    s->format = p->fmt; s->bpp = p->bpp; s->flags = PSF_VRAM;
}

static inline enum PrismResult hw_finish(enum PrismResult r)
{
    if (r == PR_DONE || r == PR_DECLINED)
        return r;
    board.faults++;
    if (r == PR_RETRY)
        return r;
    board.flags |= PBF_ACCEL_BROKEN;
    dbg("driver: operation failed; acceleration disabled\n");
    return PR_FAILED;
}

enum PrismResult pbm_hw_fill(struct PBitMap *p,UBYTE bpp,UWORD x,UWORD y,UWORD w,UWORD h,ULONG c)
{
    struct PrismSurface s;
    if (HW_FAST(p) && board.ops->fill && bpp == p->bpp && bpp != 1 && hw_rect(p,x,y,w,h)) {
        hw_surface(p,&s);
        return hw_finish(board.ops->fill(&board,&s,x,y,w,h,c));
    }
    if ((board.flags & PBF_PRESENT) || !p->inVram || !pbm_upload(p)) return PR_DECLINED;
    pbm_surface(p,&s);
    if (bpp==1 && p->bpp!=1) {
        if (board.flags & PBF_SHADOW) return PR_DECLINED;
        s.format=PF_CLUT8; s.bpp=1; s.width=s.pitch;
    }
    else if (bpp!=p->bpp) return PR_DECLINED;
    /* Byte clears include row padding. */
    if (bpp==1) s.width=s.pitch;
    return pbm_result(p,board_fill(&board,&s,x,y,w,h,c),(ULONG)x*bpp,y,(ULONG)w*bpp,h);
}
enum PrismResult pbm_hw_copy(struct PBitMap *s,struct PBitMap *d,UWORD sx,UWORD sy,
    UWORD dx,UWORD dy,UWORD w,UWORD h)
{
    struct PrismSurface a,b;
    if (HW_FAST(s) && d->inVram && board.ops->copy && s->fmt == d->fmt &&
        hw_rect(s,sx,sy,w,h) && hw_rect(d,dx,dy,w,h)) {
        hw_surface(s,&a);
        if (s == d)
            return hw_finish(board.ops->copy(&board,&a,&a,sx,sy,dx,dy,w,h));
        hw_surface(d,&b);
        return hw_finish(board.ops->copy(&board,&a,&b,sx,sy,dx,dy,w,h));
    }
    if ((board.flags & PBF_PRESENT) || !s->inVram || !d->inVram || !pbm_upload(s) || (s!=d && !pbm_upload(d))) return PR_DECLINED;
    pbm_surface(s,&a); pbm_surface(d,&b);
    return pbm_result(d,board_copy(&board,&a,&b,sx,sy,dx,dy,w,h),(ULONG)dx*d->bpp,dy,(ULONG)w*d->bpp,h);
}
enum PrismResult pbm_hw_expand(struct PBitMap *p,UWORD x,UWORD y,UWORD w,UWORD h,
    const UBYTE *src,ULONG mod,ULONG fg,ULONG bg,BOOL tr)
{
    struct PrismSurface s;
    if (HW_FAST(p) && board.ops->expand && src && mod >= ((ULONG)w + 7) / 8 &&
        hw_rect(p,x,y,w,h)) {
        hw_surface(p,&s);
        return hw_finish(board.ops->expand(&board,&s,x,y,w,h,src,mod,fg,bg,tr));
    }
    if ((board.flags & PBF_PRESENT) || !p->inVram || !pbm_upload(p)) return PR_DECLINED;
    pbm_surface(p,&s);
    return pbm_result(p,board_expand(&board,&s,x,y,w,h,src,mod,fg,bg,tr),(ULONG)x*p->bpp,y,(ULONG)w*p->bpp,h);
}
enum PrismResult pbm_hw_line(struct PBitMap *p,WORD x,WORD y,WORD dx,WORD dy,ULONG c)
{
    struct PrismSurface s;
    if ((board.flags & PBF_PRESENT) || !p->inVram || !pbm_upload(p)) return PR_DECLINED;
    pbm_surface(p,&s);
    return pbm_result(p,board_line(&board,&s,x,y,dx,dy,c),
        (ULONG)(dx<0 ? x+dx : x)*p->bpp,dy<0 ? y+dy : y,
        (ULONG)(dx<0 ? 1-dx : dx+1)*p->bpp,dy<0 ? 1-dy : dy+1);
}
enum PrismResult pbm_hw_planar(const struct PrismPlanar *src,struct PBitMap *p,
    UWORD sx,UWORD sy,UWORD dx,UWORD dy,UWORD w,UWORD h,UBYTE mt,UBYTE mask)
{
    struct PrismSurface d;
    if ((board.flags & PBF_PRESENT) || !p->inVram || !board.ops || !board.ops->planar || !pbm_upload(p)) return PR_DECLINED;
    pbm_surface(p,&d);
    return pbm_result(p,board_planar(&board,src,&d,sx,sy,dx,dy,w,h,mt,mask),
        (ULONG)dx*p->bpp,dy,(ULONG)w*p->bpp,h);
}
