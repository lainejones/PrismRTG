/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laine Jones */
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
extern struct SignalSemaphore lock;     /* card registers, VRAM allocator,
                                           render buffers, bitmap table */

void dbg(const char *fmt, ...);

/* call trace for the debug log: each patched call's name, with the
 * RastPort's bitmap (TRACE=... build only: it is a lot of output) */
#ifdef PRISM_TRACE
void memchk(const char *where);
#define TRACE(n) do { memchk(n); dbg("> " n " a0=%lx a1=%lx\n", (ULONG)r->a[0], (ULONG)r->a[1]); } while (0)
#else
#define TRACE(n)
#endif

LONG vram_alloc(ULONG size, UBYTE format, ULONG pitch, UWORD width, UWORD height);
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
};

struct PBitMap *pbm_get(const struct BitMap *bm);
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
extern ULONG rgbRow[4096];
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
