/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laine Jones
 * Copyright (C) 2026 Stefan Reinauer */
/*
 * prismint.h - shared between PrismD's modules (not for clients).
 */
#ifndef PRISMINT_H
#define PRISMINT_H

#include <exec/types.h>
#include <exec/semaphores.h>
#include <graphics/gfx.h>
#include <graphics/rastport.h>
#include "boardops.h"

/* Before the CPU reads or writes planar memory the Amiga's blitter may be
 * working on: wait until the blitter has finished everything queued, not
 * only the blit that is running (render.c). */
void blit_settle(void);

extern struct PrismBoard board;

/* The drawing lock. Drawing code takes it with LOCK(), which also takes the
 * software pointer out of the shown bitmap (pointer.c puts it back once a
 * tick has passed without drawing), so nothing draws over it, reads it or
 * moves it. PrismD's own per-tick paths use ObtainSemaphore directly. */
struct PBitMap;
extern struct PBitMap *swOn;           /* bitmap the software pointer is drawn into */
extern volatile ULONG prismActivity;
void sw_hide(void);
UBYTE pen_nearest(const ULONG *rgbTab, ULONG rgb);   /* render.c */
/* Drawing into `p` (bitmap coordinates, inclusive): take the pointer out
 * if it is in the way; p == NULL: always. Caller holds the lock or Forbid. */
void sw_clear(struct PBitMap *p, WORD x0, WORD y0, WORD x1, WORD y1);
#define LOCK() do { ObtainSemaphore(&lock); prismActivity++; \
                    if (swOn) sw_hide(); } while (0)
/* The lock for work on one known bitmap: the pointer leaves only if that is
 * the bitmap it is drawn into (a game locking its back buffer every frame
 * must not keep it away). */
#define LOCK_FOR(p) do { ObtainSemaphore(&lock); \
                         if (swOn && (p) == swOn) { prismActivity++; sw_hide(); } } while (0)
extern volatile ULONG paletteGen;
void render_quit(void);
/* gels.c: Bobs (DrawGList) on Prism bitmaps */
void gels_init(void);
void gels_quit(void);
/* is p one of the live Prism bitmaps? (a handle from a program; the
 * pointer is compared, never read) */
BOOL pbm_live(const struct PBitMap *p);
extern WORD swX, swY, swW, swH;        /* where it is drawn (pointer.c) */
/* Take the software pointer out if the rectangle touches it; p NULL means
 * "whatever is being drawn": always. The test is inline so the hot paths
 * (WritePixel, every clipped rectangle) make no call while the pointer
 * is elsewhere. */
#define SW_CLEAR(p, x0, y0, x1, y1) do { \
        if (swOn && (!(p) || ((p) == swOn && (x1) >= swX && (x0) < swX + swW && \
                                 (y1) >= swY && (y0) < swY + swH))) \
            sw_clear((p), (x0), (y0), (x1), (y1)); } while (0)      /* bumped when a palette changes in place */
extern struct SignalSemaphore lock;     /* card registers, VRAM allocator,
                                           render buffers, bitmap table */

void dbg(const char *fmt, ...);
extern UBYTE dbgOn;                     /* prefs.log: the hot paths test this before calling dbg */

/* call trace for the debug log: each patched call's name, with the
 * RastPort's bitmap (TRACE=... build only: it is a lot of output) */
#ifdef PRISM_TRACE
void memchk(const char *where);
#define TRACE(n) do { memchk(n); dbg("> " n " a0=%lx a1=%lx\n", (ULONG)r->a[0], (ULONG)r->a[1]); } while (0)
#else
#define TRACE(n)
#endif

LONG vram_alloc(ULONG size, UBYTE format, ULONG pitch, UWORD width, UWORD height);
struct PBitMap;
LONG vram_get_size(ULONG size, UBYTE format, ULONG pitch, UWORD width, UWORD height,
                   struct PBitMap *keep);   /* evicts hidden bitmaps to fast RAM */
void vram_free(ULONG off);

/* ---- chunky bitmaps -------------------------------------------------
 *
 * A Prism bitmap is an ordinary struct BitMap, so code that knows nothing
 * about Prism can't crash on it: every Planes[] pointer aims at one
 * harmless chip-RAM "dummy" plane (planar rendering we didn't intercept
 * lands there, invisibly). Flags bit 7 marks it and pad carries its index
 * in Prism's table; both survive the struct being copied (Intuition keeps
 * its own copy in Screen->BitMap). The real pixels are 8-bit chunky, in
 * VRAM (screens) or fast RAM (off-screen friends).
 */
#define BMF_PRISM       0x80
#define PBM_PAD_MAGIC   0xA000
#define PBM_MAX         1024

struct PBitMap {
    struct BitMap *bm;          /* the BitMap handed out                 */
    UBYTE         *pix;         /* chunky pixels                         */
    ULONG          bpr;         /* bytes per row of pix                  */
    UWORD          w, h;
    UBYTE          depth;       /* pens the owner may use (<= 8)         */
    UBYTE          bpp;         /* bytes per pixel: 1 = pens, 2/3/4 = RGB */
    UWORD         *penTab;      /* 16-bit: pen -> pixel (screen palette);
                                   24/32-bit pens go through rgbTab   */
    ULONG         *rgbTab;      /* pen -> 0x00RRGGBB (screen palette)    */
    UBYTE          fmt;         /* enum PrismFormat of the pixels        */
    UBYTE          inVram;
    ULONG          vramOff;
    struct BitMap *dummy;       /* 1-plane chip bitmap behind Planes[]   */
    APTR           mem;         /* fast RAM block (off-screen bitmaps)   */
    UBYTE         *uploaded;    /* last successful device upload, shadows only */
    BOOL           uploadValid;
    UWORD          index;
    UWORD          locks;       /* cgx LockBitMap: pixels must not move  */
    UBYTE          direct;      /* Planes[0] = pix (pixel-format bitmaps) */
    APTR           owner;       /* struct PScreen whose display buffer
                                   this is (screen + screen buffers)     */
    ULONG          lastShown;   /* LRU stamp for VRAM eviction           */
    ULONG          modified;    /* prismActivity when last drawn into (the
                                   compositor composes what changed)     */
    UBYTE          blitFits;    /* the whole bitmap is within the board's
                                   direct blit limits (pbm_fits), so any
                                   rectangle in it is too               */
};

/* The Prism bitmap behind a BitMap, or NULL (bitmap.c keeps the table).
 * Inline: two to four of these per drawing call. */
extern struct PBitMap *pbmTable[PBM_MAX];
static inline struct PBitMap *pbm_get(const struct BitMap *bm)
{
    struct PBitMap *p;
    UWORD i;

    if (!bm || !(bm->Flags & BMF_PRISM) || (bm->pad & 0xf000) != PBM_PAD_MAGIC)
        return NULL;
    i = bm->pad & 0x0fff;
    /* (no dummy yet: pbm_new still building it in this slot, and a stale
     * copy of an earlier bitmap's struct carries the slot number) */
    if (i >= PBM_MAX || !(p = pbmTable[i]) || !p->dummy)
        return NULL;
    /* a copy of the struct still points at the same dummy plane - or, for
     * a bitmap made in a pixel format (see h_AllocBitMap), at its pixels */
    return (p->dummy->Planes[0] == bm->Planes[0] || (p->direct && bm->Planes[0] == p->pix))
           ? p : NULL;
}
/* prismd.c: the displayable bitmap of a screen Intuition is opening on a
 * Prism mode, or NULL */
struct PBitMap *screen_bitmap_hook(ULONG w, ULONG h, ULONG depth, ULONG flags);
struct PBitMap *pbm_new(UWORD w, UWORD h, UBYTE depth, UBYTE format, UWORD *penTab,
                        BOOL vram, BOOL clear);
void            pbm_free(struct PBitMap *p);
ULONG           pbm_count(void);
/* VRAM paging (bitmap.c, caller holds lock): move pixels in or out of
 * VRAM. Into VRAM evicts least recently shown bitmaps that aren't on
 * display or locked. */
BOOL            pbm_to_vram(struct PBitMap *p);
BOOL            pbm_to_fast(struct PBitMap *p);
void pbm_surface(const struct PBitMap *, struct PrismSurface *);
BOOL pbm_upload(struct PBitMap *);
BOOL pbm_prepare_mode(void);
void pbm_flush_shown(void);
enum PrismResult pbm_hw_fill(struct PBitMap *, UBYTE, UWORD, UWORD, UWORD, UWORD, ULONG);
enum PrismResult pbm_hw_copy(struct PBitMap *, struct PBitMap *, UWORD, UWORD, UWORD, UWORD, UWORD, UWORD);
enum PrismResult pbm_hw_expand(struct PBitMap *, UWORD, UWORD, UWORD, UWORD, const UBYTE *, ULONG, ULONG, ULONG, BOOL);
enum PrismResult pbm_hw_line(struct PBitMap *, WORD, WORD, WORD, WORD, ULONG);
enum PrismResult pbm_hw_planar(const struct PrismPlanar *, struct PBitMap *, UWORD, UWORD, UWORD, UWORD, UWORD, UWORD, UBYTE, UBYTE);

/* Fast paths for the common case, one of our own VRAM bitmaps on a linear
 * board: fills and copies the board's direct hooks can encode go straight
 * to them, inline in the caller, as before the surface layer - on a 68030
 * every extra call level (registers saved, arguments copied, a surface
 * built) costs a noticeable part of a small blit. A PBitMap already
 * guarantees what board_rect() checks besides the rectangle. A driver that
 * finds its blitter wedged sets PBF_ACCEL_BROKEN, which comes back as
 * PR_FAILED so the caller redraws on the CPU. Anything else goes through
 * pbm_hw_fill / pbm_hw_copy and the surface operations. */
/* Can the board do this operation at all (render.c's first choice)? */
#define BOARD_CAN(op) (board.ops && board.ops->op)

#define HW_OK(p) ((p)->inVram && \
    !(board.flags & (PBF_SHADOW | PBF_ACCEL_BROKEN | PBF_SOFTWARE | PBF_PRESENT)))

static inline BOOL hw_fits(ULONG bytes, UWORD h, ULONG pitch)
{
    return (!board.blitMaxBytes || bytes <= board.blitMaxBytes) &&
        (!board.blitMaxRows || h <= board.blitMaxRows) &&
        (!board.blitMaxPitch || pitch <= board.blitMaxPitch);
}

/* Checking the limits for every blit cost 1.5-2.5% of a small fill or copy
 * on the 68030; a bitmap that fits as a whole is checked once, when it is
 * made (the board's limits are set before the first bitmap). */
static inline void pbm_fits(struct PBitMap *p)
{
    p->blitFits = hw_fits((ULONG)p->w * p->bpp, p->h, p->bpr);
}
#define FITS(p, bytes, h) ((p)->blitFits || hw_fits(bytes, h, (p)->bpr))

static inline enum PrismResult hw_direct(void)
{
    if (!(board.flags & PBF_ACCEL_BROKEN))
        return PR_DONE;
    dbg("driver: operation failed; acceleration disabled\n");
    return PR_FAILED;
}

/* The callers (render.c, p96.c) pass rectangles already clipped to the
 * bitmap, and copies only between bitmaps of one format; the general
 * path behind these still checks everything. */
static inline enum PrismResult pbm_fill(struct PBitMap *p, UBYTE bpp, UWORD x, UWORD y,
                                        UWORD w, UWORD h, ULONG c)
{
    if (HW_OK(p) && board.fillRect && (bpp == p->bpp || bpp == 1) &&
        FITS(p, (UWORD)w * (UWORD)bpp, h)) {
        board.fillRect(&board, p->vramOff, p->bpr, bpp, x, y, w, h, c);
        return hw_direct();
    }
    return pbm_hw_fill(p, bpp, x, y, w, h, c);
}

static inline enum PrismResult pbm_copy(struct PBitMap *s, struct PBitMap *d, UWORD sx, UWORD sy,
                                        UWORD dx, UWORD dy, UWORD w, UWORD h)
{
    if (HW_OK(s) && d->inVram &&
        ((s->blitFits && d->blitFits) ||
         (hw_fits((UWORD)w * (UWORD)s->bpp, h, s->bpr) &&
          (!board.blitMaxPitch || d->bpr <= board.blitMaxPitch)))) {
        if (s == d && board.copyRect) {
            board.copyRect(&board, s->vramOff, s->bpr, s->bpp, sx, sy, dx, dy, w, h);
            return hw_direct();
        }
        /* two of our bitmaps never share VRAM */
        if (s != d && board.copyBetween) {
            board.copyBetween(&board, s->vramOff + (ULONG)sy * s->bpr + (ULONG)sx * s->bpp, s->bpr,
                              d->vramOff + (ULONG)dy * d->bpr + (ULONG)dx * d->bpp, d->bpr,
                              (UWORD)(w * d->bpp), h);
            return hw_direct();
        }
    }
    return pbm_hw_copy(s, d, sx, sy, dx, dy, w, h);
}

/* Lines: the driver's direct drawLine hook (the ZZ9000's), as 1.0 called
 * it - through the surface layer a 390-pixel line took a third longer on
 * the A4000. Everything else goes through the operation table. */
static inline enum PrismResult pbm_line(struct PBitMap *p, WORD x, WORD y, WORD dx, WORD dy, ULONG c)
{
    if (HW_OK(p) && board.drawLine && (p->blitFits || !board.blitMaxPitch || p->bpr <= board.blitMaxPitch)) {
        board.drawLine(&board, p->vramOff, p->bpr, p->bpp, x, y, dx, dy, c);
        return hw_direct();
    }
    return pbm_hw_line(p, x, y, dx, dy, c);
}

/* Text: a timeout while the CPU feeds the template only turns blitter text
 * off (the driver counts it in faults and stops expanding), so it comes
 * back as PR_RETRY and this string is drawn on the CPU. */
static inline enum PrismResult pbm_expand(struct PBitMap *p, UWORD x, UWORD y, UWORD w, UWORD h,
                                          const UBYTE *src, ULONG mod, ULONG fg, ULONG bg, BOOL tr)
{
    if (HW_OK(p) && board.expandRect && FITS(p, (UWORD)w * (UWORD)p->bpp, h)) {
        ULONG f = board.faults;
        BOOL done = board.expandRect(&board, p->vramOff, p->bpr, p->bpp, x, y, w, h,
                                     src, mod, fg, bg, tr);
        if (board.faults != f)
            return PR_RETRY;
        return done ? PR_DONE : PR_DECLINED;
    }
    return pbm_hw_expand(p, x, y, w, h, src, mod, fg, bg, tr);
}
/* prismd.c: is it the bitmap the card shows; a bitmap is going away */
BOOL            pbm_is_shown(struct PBitMap *p);
void            pbm_gone(struct PBitMap *p);
/* render.c: paint a whole bitmap one pen (caller holds lock) */
void            pbm_fill_pen(struct PBitMap *p, UBYTE pen);

/* ---- patch trampolines (stubs.S) -------------------------------------
 *
 * Every M2 patch is an assembly stub that saves d0-d7/a0-a6 and calls
 * h_<name>(struct Regs *). A handler that returns 0 passes the call on to
 * the original function with all registers untouched; one that returns 1
 * has handled it, and r->d[0] is the result.
 */
struct Regs {
    LONG d[8];
    LONG a[7];
};

#define RW(n) ((WORD)r->d[n])               /* WORD argument in dn       */

/* ---- pointer.c ------------------------------------------------------ */
struct ViewPort;
void pointer_on(struct ViewPort *vp);        /* lock held                 */
void pointer_off(void);                      /* lock held                 */
void pointer_colours(struct ViewPort *vp);   /* lock held                 */
BOOL pointer_software(void);
void pointer_compose(struct PBitMap *, WORD);
void present_tick(void);
BOOL present_ready(void);
BOOL prism_dragging(void);
BOOL prism_software_pointer(void);
void present_stop(void);
struct Screen;
BOOL present_reserve(struct Screen *);
struct PBitMap *prism_display_bitmap(void);
struct Screen *prism_display_layers(struct PBitMap **,struct PBitMap **,WORD *,WORD *,UWORD *,UWORD *);
BOOL p96_pip_active(struct Screen *);
void p96_pip_compose(struct PBitMap *,struct Screen *,WORD);
/* the frame rows (screenTop added) any PIP of the screen covers; FALSE = none */
BOOL p96_pip_rows(struct Screen *,WORD screenTop,WORD *y0,WORD *y1);
/* a PIP source a program can write into without a call PrismD sees */
BOOL p96_pip_volatile(struct Screen *);
extern ULONG pipGen;                    /* bumped when a PIP opens, closes or changes */
/* the frame rows pointer_compose would draw on; FALSE = it draws nothing */
BOOL pointer_compose_rows(WORD top,WORD *y0,WORD *y1);
extern volatile WORD posX, posY;        /* the pointer's position (pointer.c) */
void pointer_tick(void);                     /* PrismD main loop          */


/* ---- render.c, for cgx.c -------------------------------------------- */
typedef void (*rect_cb)(struct PBitMap *p, WORD bx0, WORD by0, WORD bx1, WORD by1,
                        WORD ox, WORD oy, void *ctx);
/* quick = a short operation: may run under Forbid() instead of taking
 * the semaphores (render.c draw_lock) */
void clip_rp_q(struct RastPort *rp, WORD x0, WORD y0, WORD x1, WORD y1,
               rect_cb cb, void *ctx, BOOL quick);
void clip_rp(struct RastPort *rp, WORD x0, WORD y0, WORD x1, WORD y1, rect_cb cb, void *ctx);
/* 4096 longs of row space, render.c's. The cgx.c and p96.c callbacks
 * borrow it: every rect_cb runs with `lock` held, so only one at a time
 * (two private 16 KB copies were a tenth of PrismD's memory). */
extern ULONG rgbRow[4096 + 8];
#define PRISM_ROWBUF ((UBYTE *)rgbRow)

/* 16-bit pixel for an 8-bit R,G,B, as the 68k stores it. */
static inline UWORD rgb16(UBYTE fmt, UBYTE r, UBYTE g, UBYTE b)
{
    UWORD v;
    if (fmt == PF_BGR565LE || fmt == PF_BGR555LE) { UBYTE t = r; r = b; b = t; }
    if (fmt == PF_RGB555LE || fmt == PF_RGB555BE || fmt == PF_BGR555LE)
        v = ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3);
    else
        v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
    return (fmt == PF_RGB565LE || fmt == PF_RGB555LE || fmt == PF_BGR565LE ||
            fmt == PF_BGR555LE) ? (UWORD)((v << 8) | (v >> 8)) : v;
}

/* ...and back to 0x00RRGGBB */
static inline ULONG rgb32(UBYTE fmt, UWORD v)
{
    ULONG r, g, b;
    if (fmt == PF_RGB565LE || fmt == PF_RGB555LE || fmt == PF_BGR565LE || fmt == PF_BGR555LE)
        v = (v << 8) | (v >> 8);
    if (fmt == PF_RGB555LE || fmt == PF_RGB555BE || fmt == PF_BGR555LE) {
        r = (v >> 10) & 31; g = (v >> 5) & 31; b = v & 31;
        r = (r << 3) | (r >> 2); g = (g << 3) | (g >> 2); b = (b << 3) | (b >> 2);
    } else {
        r = (v >> 11) & 31; g = (v >> 5) & 63; b = v & 31;
        r = (r << 3) | (r >> 2); g = (g << 2) | (g >> 4); b = (b << 3) | (b >> 2);
    }
    if (fmt == PF_BGR565LE || fmt == PF_BGR555LE)
        return (b << 16) | (g << 8) | r;
    return (r << 16) | (g << 8) | b;
}

/* ---- cgx.c ---------------------------------------------------------- */
BOOL cgx_init(void);
BOOL cgx_remove(void);              /* FALSE while programs have it open */

/* ---- p96.c: Prism's own Picasso96API.library ---------------------- */
BOOL p96_init(void);
BOOL p96_remove(void);

/* ---- prismd.c: the mode table, for cgx.c ----------------------------- */
struct PrismModeInfo {
    ULONG       id;
    UWORD       w, h;
    UBYTE       fmt, bpp;
    const char *name;
};
ULONG prism_mode_count(void);
BOOL  prism_mode(ULONG index, struct PrismModeInfo *mi);
BOOL  prism_mode_by_id(ULONG id, struct PrismModeInfo *mi);

/* ---- pixel formats -------------------------------------------------- */

static inline UBYTE pf_bpp(UBYTE fmt)
{
    switch (fmt) {
    case PF_CLUT8:  return 1;
    case PF_RGB24: case PF_BGR24: return 3;
    case PF_ARGB32: case PF_BGRA32: case PF_RGBA32: return 4;
    }
    return 2;
}

/* one pixel of a direct-colour format to 0x00RRGGBB */
static inline ULONG pf_get(UBYTE fmt, const UBYTE *s)
{
    switch (fmt) {
    case PF_RGB24:  return ((ULONG)s[0] << 16) | ((ULONG)s[1] << 8) | s[2];
    case PF_BGR24:  return ((ULONG)s[2] << 16) | ((ULONG)s[1] << 8) | s[0];
    case PF_ARGB32: return ((ULONG)s[1] << 16) | ((ULONG)s[2] << 8) | s[3];
    case PF_BGRA32: return ((ULONG)s[2] << 16) | ((ULONG)s[1] << 8) | s[0];
    case PF_RGBA32: return ((ULONG)s[0] << 16) | ((ULONG)s[1] << 8) | s[2];
    }
    return rgb32(fmt, *(const UWORD *)s);
}

static inline void pf_put(UBYTE fmt, ULONG c, UBYTE *d)
{
    UBYTE r = c >> 16, g = c >> 8, b = c;
    switch (fmt) {
    case PF_RGB24:  d[0] = r; d[1] = g; d[2] = b; return;
    case PF_BGR24:  d[0] = b; d[1] = g; d[2] = r; return;
    case PF_ARGB32: d[0] = 0; d[1] = r; d[2] = g; d[3] = b; return;
    case PF_BGRA32: d[0] = b; d[1] = g; d[2] = r; d[3] = 0; return;
    case PF_RGBA32: d[0] = r; d[1] = g; d[2] = b; d[3] = 0; return;
    }
    *(UWORD *)d = rgb16(fmt, r, g, b);
}

/* CyberGraphX PIXFMT_* <-> Prism formats (PF_COUNT = not supported) */
static inline UBYTE pf_from_pixfmt(ULONG pf)
{
    static const UBYTE map[14] = {
        PF_CLUT8, PF_RGB555BE, PF_COUNT, PF_RGB555LE, PF_BGR555LE, PF_RGB565BE, PF_COUNT,
        PF_RGB565LE, PF_BGR565LE, PF_RGB24, PF_BGR24, PF_ARGB32, PF_BGRA32, PF_RGBA32
    };
    return pf < 14 ? map[pf] : PF_COUNT;
}

static inline ULONG pf_to_pixfmt(UBYTE fmt)
{
    switch (fmt) {
    case PF_RGB555BE: return 1;  case PF_RGB555LE: return 3;
    case PF_RGB565BE: return 5;  case PF_RGB565LE: return 7;
    case PF_BGR555LE: return 4;  case PF_BGR565LE: return 8;
    case PF_RGB24:    return 9;  case PF_BGR24:    return 10;
    case PF_ARGB32:   return 11; case PF_BGRA32:   return 12;
    case PF_RGBA32:   return 13;
    }
    return 0;
}

#endif /* PRISMINT_H */
