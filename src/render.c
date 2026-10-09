/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laine Jones */
/*
 * render.c - graphics.library drawing into Prism bitmaps.
 *
 * Prism bitmaps are chunky: 1 byte per pixel (pens, the screen's palette
 * on the DAC), 2 bytes (16-bit RGB in the board's byte order) or 3/4 bytes
 * (true colour). Callers still draw with pens; a 16-bit bitmap converts each
 * pen through its screen's pen table (penTab), a 24/32-bit one through the
 * screen's colours (rgbTab) and the pixel format, when the pixel is written.
 *
 * Each h_<Function> handler checks whether its target is a Prism bitmap;
 * if not it returns 0 and the original graphics.library function runs.
 * Rendering is done on the CPU, with the board's blitter for solid fills
 * and copies inside VRAM. Drawing holds `lock`, which also serialises the
 * board's registers and the static row buffers.
 *
 * Coordinates: "rp" coordinates are what the caller passes (relative to
 * its layer), "bitmap" coordinates index the BitMap actually drawn into.
 * clip_rp() walks a layer's ClipRects and calls back once per visible or
 * backed-up piece with the bitmap-coordinate rectangle and the offset
 * bitmap = rp + (ox, oy).
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/execbase.h>
#include <graphics/gfx.h>
#include <graphics/rastport.h>
#include <graphics/clip.h>
#include <graphics/layers.h>
#include <graphics/text.h>
#include <graphics/scale.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <string.h>
#include "prismint.h"

/* WaitBlit() only waits for the blit that is running. AreaEnd and Flood
 * can leave their fill queued behind other blits, and then a WaitBlit()
 * returns while the mask is still being drawn: in Amiberry 8.3 every area
 * fill on a 32-bit screen lost its top rows (2026-10-07). OwnBlitter()
 * returns only when everything queued ahead has gone; a task that owns the
 * blitter already must not call it again (it would wait for itself). */
void blit_settle(void)
{
    if (GfxBase->BlitOwner == FindTask(NULL)) {
        WaitBlit();
        return;
    }
    OwnBlitter();
    WaitBlit();
    DisownBlitter();
}

#define MAXW 4096
/* rowA is long-aligned (text expansion writes it a long at a time) and
 * has room for a whole last group of 8 */
static ULONG rowAbuf[MAXW / 4 + 2];
#define rowA ((UBYTE *)rowAbuf)
static UBYTE rowB[MAXW], rowC[MAXW], rowT[MAXW];
static UWORD wA[MAXW], wB[MAXW];
/* also the row buffer of the cgx and Picasso96 callbacks (prismint.h) */
ULONG rgbRow[MAXW + 8];              /* blits' RGB row; text's row buffer too */
/* rowW is long-aligned with room for a whole last group of 8 pixels */
#define rowWbuf rgbRow                /* text runs under the lock like a blit:
                                         never while rgbRow is in use */
UBYTE dbgOn;
#define rowW ((UBYTE *)rowWbuf)

/* Nearest pen to an RGB colour (format conversions into pen bitmaps, the
 * software pointer, composition): green weighs most, blue least. The last
 * answer is kept - conversions ask for the same colour many times. */
volatile ULONG paletteGen;

UBYTE pen_nearest(const ULONG *tab, ULONG c)
{
    static const ULONG *lastTab;
    static ULONG lastC, lastGen;
    static UBYTE lastPen;
    UWORD i, best = 0;
    LONG bd = 0x7fffffff;
    if (!tab)
        return 0;
    c &= 0xffffff;
    if (tab == lastTab && c == lastC && lastGen == paletteGen)
        return lastPen;
    for (i = 0; i < 256; i++) {
        LONG dr = (LONG)((tab[i] >> 16) & 255) - ((c >> 16) & 255);
        LONG dg = (LONG)((tab[i] >> 8) & 255) - ((c >> 8) & 255);
        LONG db = (LONG)(tab[i] & 255) - (c & 255);
        LONG dd = dr * dr * 3 + dg * dg * 4 + db * db * 2;
        if (dd < bd) {
            bd = dd; best = i;
            if (!dd)
                break;
        }
    }
    lastTab = tab; lastC = c; lastPen = best; lastGen = paletteGen;
    return best;
}

/* ---- surfaces: planar, 8-bit chunky or 16-bit chunky ---------------- */

struct Surf {
    UBYTE          *pix;        /* chunky pixels, NULL = planar          */
    ULONG           bpr;        /* bytes per row (planar: one plane's)   */
    UBYTE           bpp;        /* 0 = planar, 1 = pens, 2/3/4 = RGB     */
    UWORD          *penTab;     /* 16-bit: pen -> pixel                  */
    ULONG          *rgbTab;     /* pen -> 0x00RRGGBB, if known           */
    UBYTE           fmt;        /* chunky: enum PrismFormat              */
    struct BitMap  *bm;
    WORD            w, h;
    struct PBitMap *p;          /* Prism bitmap behind pix, if any       */
};

static void surf_of(struct BitMap *bm, struct Surf *s)
{
    struct PBitMap *p = pbm_get(bm);
    s->bm = bm;
    s->p = p;
    if (p) {
        s->pix = p->pix;
        s->bpr = p->bpr;
        s->bpp = p->bpp;
        s->penTab = p->penTab;
        s->rgbTab = p->rgbTab;
        s->fmt = p->fmt;
        s->w = p->w;
        s->h = p->h;
    } else {
        /* an interleaved bitmap's BytesPerRow spans all its planes */
        UWORD planeBytes = bm->BytesPerRow;
        if ((bm->Flags & BMF_INTERLEAVED) && bm->Depth)
            planeBytes /= bm->Depth;
        s->pix = NULL;
        s->bpr = planeBytes;
        s->bpp = 0;
        s->penTab = NULL;
        s->rgbTab = NULL;
        s->fmt = PF_CLUT8;
        s->w = planeBytes * 8;
        s->h = bm->Rows;
    }
}

/* 16-bit pixel back to a pen: exact match in the table, else pen 0. */
static UBYTE pen_of(const UWORD *tab, UWORD v)
{
    UWORD i;
    if (tab)
        for (i = 0; i < 256; i++)
            if (tab[i] == v)
                return i;
    return 0;
}

/* 24/32-bit: a pen's colour, and a colour back to a pen (exact match,
 * else pen 0). */
static inline ULONG pen_rgb(const ULONG *tab, UBYTE pen)
{
    return tab ? tab[pen] : pen * 0x010101UL;
}

static UBYTE pen_of_rgb(const ULONG *tab, ULONG c)
{
    /* neighbouring pixels are mostly the same colour: remember the last
     * answer (checked against the table, which may have changed) */
    static const ULONG *lastTab;
    static ULONG lastC;
    static UBYTE lastPen;
    UWORD i;
    c &= 0xffffff;
    if (!tab)
        return 0;
    if (tab == lastTab && c == lastC && tab[lastPen] == c)
        return lastPen;
    for (i = 0; i < 256; i++)
        if (tab[i] == c) {
            lastTab = tab; lastC = c; lastPen = i;
            return i;
        }
    return 0;
}

/* Planar to chunky: p2c[b] spreads a plane byte's 8 bits (MSB = leftmost
 * pixel) into 8 bytes of 0/1, as two big-endian longs. A row converts a
 * plane byte at a time - two ORs per plane per 8 pixels. */
static ULONG p2c[256][2];
static ULONG p2cRow[(MAXW / 8 + 8) * 2];

static void p2c_init(void)
{
    UWORD b;
    for (b = 0; b < 256; b++) {
        p2c[b][0] = ((ULONG)(b >> 7 & 1) << 24) | ((ULONG)(b >> 6 & 1) << 16) |
                    ((ULONG)(b >> 5 & 1) << 8) | (b >> 4 & 1);
        p2c[b][1] = ((ULONG)(b >> 3 & 1) << 24) | ((ULONG)(b >> 2 & 1) << 16) |
                    ((ULONG)(b >> 1 & 1) << 8) | (b & 1);
    }
}

/* 5 or more planes, 32 pixels at a time, in assembly (p2c.S, generated
 * and self-tested by tools/gen_p2c.py): one long per plane through an 8x8
 * bit transpose and a byte transpose, ~5.5 instructions a pixel where the
 * compiler's version of the same thing took 14. Missing planes read from a
 * row of zeros (or ones). Reads up to 3 bytes past the row's end. */
void p2c8(const UBYTE **planes, ULONG *out, ULONG groups);

static UBYTE p2cZero[MAXW / 8 + 8], p2cOnes[MAXW / 8 + 8];

static void planar_read32(struct BitMap *bm, WORD x, WORD y, WORD n, UBYTE *out)
{
    const UBYTE *pp[8];
    WORD bx0 = x >> 3, nb = ((x + n - 1) >> 3) - bx0 + 1;
    ULONG off = (ULONG)y * bm->BytesPerRow + bx0;
    int i;

    for (i = 0; i < 8; i++) {
        const UBYTE *pl = i < bm->Depth ? bm->Planes[i] : NULL;
        if (!pl) {
            pp[i] = p2cZero;
        } else if (pl == (UBYTE *)-1) {
            if (!p2cOnes[0])
                memset(p2cOnes, 0xff, sizeof(p2cOnes));
            pp[i] = p2cOnes;
        } else {
            pp[i] = pl + off;
        }
    }
    if (!(x & 7) && !(n & 31) && !((ULONG)out & 1)) {
        p2c8(pp, (ULONG *)out, n >> 5);         /* aligned: straight out */
        return;
    }
    p2c8(pp, p2cRow, (nb + 3) >> 2);
    CopyMem((UBYTE *)p2cRow + (x & 7), out, n);
}

/* h rows of whole 32-pixel groups from a byte-aligned x, straight into
 * 8-bit rows `dbpr` apart: the plane pointers are set up once. FALSE = not
 * that shape, nothing done. */
static BOOL planar_block(struct BitMap *bm, WORD x, WORD y, WORD w, WORD h, UBYTE *dst,
                         ULONG dbpr)
{
    const UBYTE *pp[8];
    ULONG step[8];
    int i;

    if (bm->Depth < 5 || (x & 7) || (w & 31) || ((ULONG)dst & 1) || (dbpr & 1))
        return FALSE;
    for (i = 0; i < 8; i++) {
        const UBYTE *pl = i < bm->Depth ? bm->Planes[i] : NULL;
        step[i] = 0;
        if (!pl) {
            pp[i] = p2cZero;
        } else if (pl == (UBYTE *)-1) {
            if (!p2cOnes[0])
                memset(p2cOnes, 0xff, sizeof(p2cOnes));
            pp[i] = p2cOnes;
        } else {
            pp[i] = pl + (ULONG)y * bm->BytesPerRow + (x >> 3);
            step[i] = bm->BytesPerRow;
        }
    }
    for (; h > 0; h--, dst += dbpr) {
        p2c8(pp, (ULONG *)dst, w >> 5);
        for (i = 0; i < 8; i++)
            pp[i] += step[i];
    }
    return TRUE;
}

static void planar_read(struct BitMap *bm, WORD x, WORD y, WORD n, UBYTE *out)
{
    WORD bx0 = x >> 3, nb = ((x + n - 1) >> 3) - bx0 + 1, k;
    ULONG *acc = p2cRow;
    UBYTE pl;

    if (bm->Depth >= 5) {
        planar_read32(bm, x, y, n, out);
        return;
    }
    if (!p2c[1][1])
        p2c_init();
    memset(acc, 0, (ULONG)nb * 8);
    for (pl = 0; pl < bm->Depth && pl < 8; pl++) {
        const UBYTE *row = bm->Planes[pl];
        if (!row)
            continue;                         /* NULL plane = all zero  */
        if (row == (UBYTE *)-1) {             /* -1 plane = all ones    */
            ULONG m = 0x01010101UL << pl;
            for (k = 0; k < nb * 2; k++) acc[k] |= m;
            continue;
        }
        row += (ULONG)y * bm->BytesPerRow + bx0;
        for (k = 0; k < nb; k++) {
            const ULONG *t = p2c[row[k]];
            acc[k * 2] |= t[0] << pl;
            acc[k * 2 + 1] |= t[1] << pl;
        }
    }
    CopyMem((UBYTE *)acc + (x & 7), out, n);
}

/* Read n pixels at (x,y) as pens. */
static void surf_read(struct Surf *s, WORD x, WORD y, WORD n, UBYTE *out)
{
    WORD i;

    if (s->bpp == 1) {
        CopyMem(s->pix + (ULONG)y * s->bpr + x, out, n);
        return;
    }
    if (s->bpp == 2) {
        const UWORD *row = (const UWORD *)(s->pix + (ULONG)y * s->bpr) + x;
        for (i = 0; i < n; i++)
            out[i] = pen_of(s->penTab, row[i]);
        return;
    }
    if (s->bpp >= 3) {
        const UBYTE *sp = s->pix + (ULONG)y * s->bpr + (ULONG)x * s->bpp;
        for (i = 0; i < n; i++, sp += s->bpp)
            out[i] = pen_of_rgb(s->rgbTab, pf_get(s->fmt, sp));
        return;
    }
    planar_read(s->bm, x, y, n, out);
}

/* Read n pixels as 16-bit values, pens converted through tab. */
static void surf_read16(struct Surf *s, WORD x, WORD y, WORD n, UWORD *out, const UWORD *tab,
                        UBYTE dfmt)
{
    WORD i;
    if (s->bpp == 2 && s->fmt == dfmt) {
        CopyMem(s->pix + (ULONG)y * s->bpr + x * 2, out, n * 2);
        return;
    }
    if (s->bpp >= 2) {
        const UBYTE *sp = s->pix + (ULONG)y * s->bpr + (ULONG)x * s->bpp;
        for (i = 0; i < n; i++, sp += s->bpp) {
            ULONG c = pf_get(s->fmt, sp);
            out[i] = rgb16(dfmt, c >> 16, c >> 8, c);
        }
        return;
    }
    surf_read(s, x, y, n, rowT);
    for (i = 0; i < n; i++)
        out[i] = tab ? tab[rowT[i]] : rowT[i];
}

/* Read n pixels as 0x00RRGGBB; pens through ptab. */
static void surf_rgb(struct Surf *s, WORD x, WORD y, WORD n, ULONG *out, const ULONG *ptab)
{
    WORD i;
    if (s->bpp >= 2) {
        const UBYTE *sp = s->pix + (ULONG)y * s->bpr + (ULONG)x * s->bpp;
        for (i = 0; i < n; i++, sp += s->bpp)
            out[i] = pf_get(s->fmt, sp);
        return;
    }
    surf_read(s, x, y, n, rowT);
    for (i = 0; i < n; i++)
        out[i] = pen_rgb(ptab, rowT[i]);
}

/* Write n pens at (x,y); only the bits in mask change (pen surfaces). */
static void surf_write(struct Surf *s, WORD x, WORD y, WORD n, const UBYTE *in, UBYTE mask)
{
    struct BitMap *bm;
    UBYTE pl;
    WORD i;

    if (s->bpp == 1) {
        UBYTE *row = s->pix + (ULONG)y * s->bpr + x;
        if (mask == 0xff) {
            CopyMem((APTR)in, row, n);
        } else {
            for (i = 0; i < n; i++)
                row[i] = (row[i] & ~mask) | (in[i] & mask);
        }
        return;
    }
    if (s->bpp == 2) {
        UWORD *row = (UWORD *)(s->pix + (ULONG)y * s->bpr) + x;
        if (mask)
            for (i = 0; i < n; i++)
                row[i] = s->penTab ? s->penTab[in[i]] : in[i];
        return;
    }
    if (s->bpp >= 3) {
        UBYTE *dp = s->pix + (ULONG)y * s->bpr + (ULONG)x * s->bpp;
        if (mask)
            for (i = 0; i < n; i++, dp += s->bpp)
                pf_put(s->fmt, pen_rgb(s->rgbTab, in[i]), dp);
        return;
    }
    bm = s->bm;
    for (pl = 0; pl < bm->Depth && pl < 8; pl++) {
        UBYTE *row = bm->Planes[pl];
        UBYTE bit = 1 << pl;
        if (!(mask & bit) || !row || row == (UBYTE *)-1)
            continue;
        row += (ULONG)y * bm->BytesPerRow;
        for (i = 0; i < n; i++) {
            WORD xx = x + i;
            UBYTE m = 0x80 >> (xx & 7);
            if (in[i] & bit) row[xx >> 3] |= m;
            else             row[xx >> 3] &= ~m;
        }
    }
}

/* Blitter minterm with A = all ones: f(B = source, C = destination). */
static void combine2(const UBYTE *b, const UBYTE *c, UBYTE *out, WORD n, UBYTE mt)
{
    UBYTE m80 = (mt & 0x80) ? 0xff : 0, m40 = (mt & 0x40) ? 0xff : 0;
    UBYTE m20 = (mt & 0x20) ? 0xff : 0, m10 = (mt & 0x10) ? 0xff : 0;
    WORD i;
    for (i = 0; i < n; i++) {
        UBYTE B = b[i], C = c[i];
        out[i] = (B & C & m80) | (B & ~C & m40) | (~B & C & m20) | (~B & ~C & m10);
    }
}

static void combine16(const UWORD *b, const UWORD *c, UWORD *out, WORD n, UBYTE mt)
{
    UWORD m80 = (mt & 0x80) ? 0xffff : 0, m40 = (mt & 0x40) ? 0xffff : 0;
    UWORD m20 = (mt & 0x20) ? 0xffff : 0, m10 = (mt & 0x10) ? 0xffff : 0;
    WORD i;
    for (i = 0; i < n; i++) {
        UWORD B = b[i], C = c[i];
        out[i] = (B & C & m80) | (B & ~C & m40) | (~B & C & m20) | (~B & ~C & m10);
    }
}

/* Clip a source/destination rectangle pair to both surfaces. */
static BOOL clip2(struct Surf *s, WORD *sx, WORD *sy, struct Surf *d, WORD *dx, WORD *dy,
                  WORD *w, WORD *h)
{
    WORD t;
    if (*sx < 0) { *dx -= *sx; *w += *sx; *sx = 0; }
    if (*sy < 0) { *dy -= *sy; *h += *sy; *sy = 0; }
    if (*dx < 0) { *sx -= *dx; *w += *dx; *dx = 0; }
    if (*dy < 0) { *sy -= *dy; *h += *dy; *dy = 0; }
    if ((t = s->w - *sx) < *w) *w = t;
    if ((t = s->h - *sy) < *h) *h = t;
    if ((t = d->w - *dx) < *w) *w = t;
    if ((t = d->h - *dy) < *h) *h = t;
    if (*w > MAXW) *w = MAXW;
    return *w > 0 && *h > 0;
}

/* BltBitMap semantics between any two surfaces. Caller holds lock. */
static void fill(struct PBitMap *p, WORD x0, WORD y0, WORD x1, WORD y1,
                 UBYTE pen, UBYTE mask, BOOL xor);

static void blit(struct Surf *s, WORD sx, WORD sy, struct Surf *d, WORD dx, WORD dy,
                 WORD w, WORD h, UBYTE mt, UBYTE mask)
{
    WORD y, y0, y1, step;
    BOOL same, copy = (mt & 0xf0) == 0xc0;

    if (!clip2(s, &sx, &sy, d, &dx, &dy, &w, &h))
        return;
    same = (s->pix && s->pix == d->pix) || (!s->pix && s->bm == d->bm);

    /* "Clear" (0x00) and "set" (0xF0) don't look at the source: a fill with
     * pen 0 or the all-ones pen, on the blitter where there is one. Layers
     * clears every area a closing or moving window uncovers this way; in
     * true colour the general path below (both bitmaps read back as pens,
     * a palette search per pixel) took seconds per window and starved the
     * rest of the system - SysSpeed's Intuition test looked like a hang. */
    if (d->p && ((mt & 0xf0) == 0x00 || (mt & 0xf0) == 0xf0)) {
        fill(d->p, dx, dy, dx + w - 1, dy + h - 1, (mt & 0xf0) ? 0xff : 0, mask, FALSE);
        return;
    }

    if (!s->pix && d->p && (d->bpp!=2 || d->penTab)) {
        struct PrismPlanar source = { s->bm, s->rgbTab ? s->rgbTab : d->rgbTab, d->penTab };
        if (pbm_hw_planar(&source,d->p,sx,sy,dx,dy,w,h,mt,mask) != PR_DECLINED)
            return;
    }
    if (s->pix && d->pix && s->bpp == d->bpp && s->fmt == d->fmt && copy &&
        (mask == 0xff || d->bpp >= 2)) {
        /* plain copy, same chunky format */
        ULONG bytes = (ULONG)w * d->bpp;
        if (s->p && d->p && w * h > 64) {
            enum PrismResult res = pbm_copy(s->p,d->p,sx,sy,dx,dy,w,h);
            /* a copy that failed part way can be done again on the CPU
             * unless it overlaps itself (then the source is half moved) */
            if (res == PR_DONE ||
                (res != PR_DECLINED && same && sx < dx + w && dx < sx + w && sy < dy + h && dy < sy + h))
                return;
        }
        if (same && dy > sy) {
            for (y = h - 1; y >= 0; y--)
                memmove(d->pix + (ULONG)(dy + y) * d->bpr + dx * d->bpp,
                        s->pix + (ULONG)(sy + y) * s->bpr + sx * s->bpp, bytes);
        } else {
            for (y = 0; y < h; y++)
                memmove(d->pix + (ULONG)(dy + y) * d->bpr + dx * d->bpp,
                        s->pix + (ULONG)(sy + y) * s->bpr + sx * s->bpp, bytes);
        }
        return;
    }

    if (same && dy > sy) { y0 = h - 1; y1 = -1; step = -1; }
    else                 { y0 = 0;     y1 = h;  step = 1;  }

    /* Copies between direct-colour formats that differ or involve 24/32-bit,
     * and direct colour into pens: convert through 0x00RRGGBB (pens via the
     * palette, the nearest pen going back). Other minterms into 24/32-bit
     * work on pens, below. */
    if (((d->bpp >= 3 || s->bpp >= 3) && copy) ||
        (s->bpp == 2 && d->bpp == 2 && s->fmt != d->fmt) || (s->bpp >= 2 && d->bpp <= 1)) {
        const ULONG *ptab = s->rgbTab ? s->rgbTab : d->rgbTab;
        if (s->bpp <= 1 && d->bpp >= 3) {
            /* pens (planar or chunky) into true colour: each pen's pixel
             * bytes worked out once per blit */
            static UBYTE penPx[256][4], penOk[256];
            static ULONG penL[256];
            UBYTE b = d->bpp;
            memset(penOk, 0, sizeof(penOk));
            if (b == 4) {
                /* 32-bit: one long per pixel, straight into the bitmap (no
                 * staging row, no second pass over the bus) */
                for (y = y0; y != y1; y += step) {
                    ULONG *dl = (ULONG *)(d->pix + (ULONG)(dy + y) * d->bpr) + dx;
                    WORD x;
                    surf_read(s, sx, sy + y, w, rowA);
                    for (x = 0; x < w; x++) {
                        UBYTE pen = rowA[x];
                        if (!penOk[pen]) {
                            pf_put(d->fmt, pen_rgb(ptab, pen), (UBYTE *)&penL[pen]);
                            penOk[pen] = 1;
                        }
                        *dl++ = penL[pen];
                    }
                }
                return;
            }
            for (y = y0; y != y1; y += step) {
                UBYTE *dp = rowW;
                WORD x;
                surf_read(s, sx, sy + y, w, rowA);
                for (x = 0; x < w; x++, dp += b) {
                    UBYTE pen = rowA[x], *e = penPx[pen];
                    if (!penOk[pen]) {
                        pf_put(d->fmt, pen_rgb(ptab, pen), e);
                        penOk[pen] = 1;
                    }
                    dp[0] = e[0]; dp[1] = e[1]; dp[2] = e[2];
                    if (b == 4) dp[3] = e[3];
                }
                CopyMem(rowW, d->pix + (ULONG)(dy + y) * d->bpr + (ULONG)dx * b, (ULONG)w * b);
            }
            return;
        }
        for (y = y0; y != y1; y += step) {
            WORD x;
            surf_rgb(s, sx, sy + y, w, rgbRow, ptab);
            if (d->bpp >= 2) {
                UBYTE *dp = rowW;
                for (x = 0; x < w; x++, dp += d->bpp)
                    pf_put(d->fmt, rgbRow[x], dp);
                CopyMem(rowW, d->pix + (ULONG)(dy + y) * d->bpr + (ULONG)dx * d->bpp,
                        (ULONG)w * d->bpp);
            } else {
                for (x = 0; x < w; x++)
                    rowA[x] = pen_nearest(d->rgbTab, rgbRow[x]);
                surf_write(d, dx, dy + y, w, rowA, mask);
            }
        }
        return;
    }

    if (d->bpp == 2) {
        /* Into 16-bit. Minterms mean pens: "clear" (0x00) is pen 0, "set"
         * (0xF0) the all-ones pen; with a pen source the other minterms
         * combine pens (the destination read back as pens) and the result
         * goes through the pen table. 16-bit to 16-bit copies raw. */
        UBYTE hi = mt & 0xf0;
        for (y = y0; y != y1; y += step) {
            UWORD *drow = (UWORD *)(d->pix + (ULONG)(dy + y) * d->bpr) + dx;
            WORD x;
            if (hi == 0x00 || hi == 0xf0) {
                UWORD v = d->penTab ? d->penTab[hi ? mask : 0] : 0;
                for (x = 0; x < w; x++) drow[x] = v;
            } else if (copy) {
                surf_read16(s, sx, sy + y, w, drow, d->penTab, d->fmt);
            } else if (s->bpp != 2) {
                surf_read(s, sx, sy + y, w, rowA);
                surf_read(d, dx, dy + y, w, rowB);
                combine2(rowA, rowB, rowC, w, mt);
                for (x = 0; x < w; x++)
                    drow[x] = d->penTab ? d->penTab[(rowC[x] & mask) | (rowB[x] & ~mask)] : 0;
            } else {
                surf_read16(s, sx, sy + y, w, wA, d->penTab, d->fmt);
                CopyMem(drow, wB, w * 2);
                combine16(wA, wB, drow, w, mt);
            }
        }
        return;
    }
    if (copy && !s->pix && d->bpp == 1 && mask == 0xff) {
        /* planar image into pens (icons, gadget imagery): convert straight
         * into the destination rows */
        if (planar_block(s->bm, sx, sy, w, h, d->pix + (ULONG)dy * d->bpr + dx, d->bpr))
            return;
        for (y = y0; y != y1; y += step)
            planar_read(s->bm, sx, sy + y, w, d->pix + (ULONG)(dy + y) * d->bpr + dx);
        return;
    }
    for (y = y0; y != y1; y += step) {
        surf_read(s, sx, sy + y, w, rowA);
        if (copy) {
            surf_write(d, dx, dy + y, w, rowA, mask);
        } else {
            surf_read(d, dx, dy + y, w, rowB);
            combine2(rowA, rowB, rowC, w, mt);
            surf_write(d, dx, dy + y, w, rowC, mask);
        }
    }
}

/* ---- pixels --------------------------------------------------------- */

/* What a pen becomes in this bitmap. */
static inline ULONG pixval(struct PBitMap *p, UBYTE pen)
{
    return (p->bpp == 2 && p->penTab) ? p->penTab[pen] : pen;
}

/* Pixel rule for a "template" bit under the RastPort's draw mode.
 * Returns 0 = leave, 1 = set to pen, 2 = complement. */
static UBYTE rule(UBYTE dm, BOOL bit, UBYTE fg, UBYTE bg, UBYTE *pen)
{
    if (dm & INVERSVID)
        bit = !bit;
    if (dm & COMPLEMENT)
        return bit ? 2 : 0;
    if (bit) { *pen = fg; return 1; }
    if (dm & JAM2) { *pen = bg; return 1; }
    return 0;
}

/* Apply a rule to pixel x of a row. Pens respect the write mask; RGB
 * pixels are all or nothing and complement inverts the colour. */
static inline void put_px(struct PBitMap *p, UBYTE *row, WORD x, UBYTE op, UBYTE pen,
                          UBYTE mask)
{
    if (!op || !mask)
        return;
    if (p->bpp == 2) {
        UWORD *w = (UWORD *)row + x;
        if (op == 1) *w = pixval(p, pen);
        else         *w ^= 0xffff;
    } else if (p->bpp >= 3) {
        UBYTE *d = row + (ULONG)x * p->bpp;
        pf_put(p->fmt, op == 1 ? pen_rgb(p->rgbTab, pen) : pf_get(p->fmt, d) ^ 0xffffff, d);
    } else if (op == 1) {
        /* a full mask needs no read of the old pixel (slow over Zorro) */
        row[x] = (mask == 0xff) ? pen : (UBYTE)((row[x] & ~mask) | (pen & mask));
    } else {
        row[x] ^= mask;
    }
}

/* ---- VRAM writers ---------------------------------------------------
 * Zorro II is 16 bits wide and every bus cycle costs about the same
 * (~0.8 us on the A2000), so words and longs beat bytes. */

static inline void vset8(UBYTE *q, UBYTE c, ULONG c4, LONG n)
{
    if (n >= 3) {
        if ((ULONG)q & 1) { *q++ = c; n--; }
        for (; n >= 4; n -= 4, q += 4) *(ULONG *)q = c4;
        if (n >= 2) { *(UWORD *)q = (UWORD)c4; q += 2; n -= 2; }
    }
    while (n-- > 0)
        *q++ = c;
}

static inline void vset16(UWORD *q, UWORD c, ULONG c2, LONG n)
{
    for (; n >= 2; n -= 2, q += 2) *(ULONG *)q = c2;
    if (n) *q = c;
}

/* n bytes from fast RAM into VRAM */
static inline void vcopy(UBYTE *d, const UBYTE *src, LONG n)
{
    if (((ULONG)d & 1) && n) { *d++ = *src++; n--; }
    for (; n >= 4; n -= 4, d += 4, src += 4) *(ULONG *)d = *(const ULONG *)src;
    if (n >= 2) { *(UWORD *)d = *(const UWORD *)src; d += 2; src += 2; n -= 2; }
    if (n) *d = *src;
}

/* ---- solid fills ---------------------------------------------------- */

/* Fill a bitmap-coordinate rectangle: dst = pen (xor = FALSE) or
 * complement (xor = TRUE). Caller holds lock. */
static void fill(struct PBitMap *p, WORD x0, WORD y0, WORD x1, WORD y1,
                 UBYTE pen, UBYTE mask, BOOL xor)
{
    WORD x, y, w;

    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= p->w) x1 = p->w - 1;
    if (y1 >= p->h) y1 = p->h - 1;
    if (x1 < x0 || y1 < y0 || !mask)
        return;
    w = x1 - x0 + 1;
    if (p->bpp >= 3) {
        UBYTE px[4] = { 0, 0, 0, 0 }, b = p->bpp;
        UBYTE *r;
        pf_put(p->fmt, pen_rgb(p->rgbTab, pen), px);
        if (xor) {
            for (y = y0; y <= y1; y++)
                for (x = 0, r = p->pix + (ULONG)y * p->bpr + (ULONG)x0 * b; x < w; x++, r += b)
                    pf_put(p->fmt, pf_get(p->fmt, r) ^ 0xffffff, r);
            return;
        }
        /* These fills overwrite their pixels, so a stopped partial blit
         * can finish on the CPU. The same applies to the solid row below. */
        /* a blitter that takes 4-byte pixels (ZZ9000) */
        if (b == 4 && p->inVram && BOARD_CAN(fill) && (board.flags & PBF_BLIT_32) &&
            w * (y1 - y0 + 1) >= 12) {
            if (pbm_fill(p, 4, x0, y0, w, y1 - y0 + 1,
                           ((ULONG)px[0] << 24) | ((ULONG)px[1] << 16) | ((ULONG)px[2] << 8) |
                           px[3]) == PR_DONE) return;
        }
        /* a grey (all bytes equal) fills as bytes on the blitter */
        if (px[0] == px[1] && px[1] == px[2] && (b == 3 || px[2] == px[3]) &&
            p->inVram && BOARD_CAN(fill) && w * (y1 - y0 + 1) > 64) {
            if (pbm_fill(p, 1, x0 * b, y0, w * b, y1 - y0 + 1, px[0]) == PR_DONE) return;
        }
        for (x = 0, r = rowW; x < w; x++, r += b) {
            r[0] = px[0]; r[1] = px[1]; r[2] = px[2];
            if (b == 4) r[3] = px[3];
        }
        if (p->inVram && BOARD_CAN(copy) && w * (y1 - y0 + 1) > 256) {
            /* no colour expansion at 24 bits on the 5426/28: write the
             * first row, then let the blitter double it down (1, 2, 4...
             * rows per copy) */
            WORD h = y1 - y0 + 1, done = 1, k;
            vcopy(p->pix + (ULONG)y0 * p->bpr + (ULONG)x0 * b, rowW, (LONG)w * b);
            while (done < h) {
                k = (done < h - done) ? done : h - done;
                enum PrismResult result = pbm_copy(p,p,x0,y0,x0,y0+done,w,k);
                if (result != PR_DONE) {
                    WORD row;
                    for (row=0; row<k; row++)
                        vcopy(p->pix+(ULONG)(y0+done+row)*p->bpr+(ULONG)x0*b,
                              rowW,(LONG)w*b);
                }
                done += k;
            }
            return;
        }
        for (y = y0; y <= y1; y++)
            vcopy(p->pix + (ULONG)y * p->bpr + (ULONG)x0 * b, rowW, (LONG)w * b);
        return;
    }
    if (!xor && (mask == 0xff || p->bpp == 2) && p->inVram && BOARD_CAN(fill) &&
        w * (y1 - y0 + 1) >= 12) {
        if (pbm_fill(p, p->bpp, x0, y0, w, y1 - y0 + 1,
                       pixval(p, pen)) == PR_DONE) return;
    }
    if (p->bpp == 2) {
        UWORD v = pixval(p, pen);
        for (y = y0; y <= y1; y++) {
            UWORD *row = (UWORD *)(p->pix + (ULONG)y * p->bpr) + x0;
            if (xor) for (x = 0; x < w; x++) row[x] ^= 0xffff;
            else     for (x = 0; x < w; x++) row[x] = v;
        }
        return;
    }
    for (y = y0; y <= y1; y++) {
        UBYTE *row = p->pix + (ULONG)y * p->bpr + x0;
        if (xor)
            for (x = 0; x < w; x++) row[x] ^= mask;
        else if (mask == 0xff)
            vset8(row, pen, pen * 0x01010101UL, w);
        else
            for (x = 0; x < w; x++) row[x] = (row[x] & ~mask) | (pen & mask);
    }
}

void pbm_fill_pen(struct PBitMap *p, UBYTE pen)
{
    fill(p, 0, 0, p->w - 1, p->h - 1, pen, 0xff, FALSE);
}

/* ---- RastPort clipping --------------------------------------------- */

/* pixels an operation may cover and still count as "quick" (draw_lock):
 * the blitter does this many in well under a millisecond */
#define QUICK_AREA 8192

/* rect_cb and clip_rp are declared in prismint.h (cgx.c uses them) */

/* Is this RastPort's drawing ours? */
static BOOL rp_is_prism(struct RastPort *rp)
{
    struct PBitMap *p;
    /* pen drawing goes to 8- and 16-bit bitmaps, and to 24/32-bit ones that
     * have a screen's colours; a 24/32-bit image buffer without them
     * (cybergraphics.library) is left to graphics.library's dummy */
    return rp && rp->BitMap && (p = pbm_get(rp->BitMap)) && (p->bpp <= 2 || p->rgbTab);
}

/* ---- planar bitmaps outside chip RAM --------------------------------
 *
 * The Amiga's blitter only reaches chip RAM, and graphics.library draws
 * into planar bitmaps with it. Programs that know an RTG system is running
 * (they find cybergraphics.library) keep work bitmaps in fast RAM anyway,
 * because the RTG systems they were written for draw into those with the
 * CPU. Left to the ROM, such a call draws nothing where it should and
 * scribbles over chip RAM at the same address modulo 2 MB instead - found
 * with PeterK's icon.library 51.4 on the A2000: classic icons came out
 * blank (their masks, built with Draw/BltPattern/BltBitMap in fast RAM,
 * stayed empty) and the machine crashed some time later.
 *
 * So Prism takes those calls. The stubs send any RastPort whose bitmap's
 * first plane lies above chip RAM to the handlers (chipTop), and the
 * handlers run on a chunky copy: the rows a call touches are converted in
 * as the clipper reaches them and written back to the planes when the
 * call is done. One copy exists at a time, under Prism's lock.
 */
ULONG chipTop;                           /* SysBase->MaxLocMem (prismd.c)  */
static struct PBitMap *shadow;           /* the chunky copy                */
static struct BitMap *shadowOf;          /* planar bitmap it stands in for */
static UBYTE shadowRows[MAXW / 8];       /* rows converted in so far       */

BOOL bm_foreign(const struct BitMap *bm)
{
    return bm && !(bm->Flags & (BMF_PRISM | BMF_INTERLEAVED)) && bm->Planes[0] &&
           bm->Planes[0] != (PLANEPTR)-1 && (ULONG)bm->Planes[0] >= chipTop &&
           bm->Depth >= 1 && bm->Depth <= 8 && bm->BytesPerRow && bm->Rows &&
           (ULONG)bm->BytesPerRow * 8 <= MAXW && bm->Rows <= MAXW;
}

/* Rows y0..y1 of the copy are about to be used: bring them in. */
static void shadow_load(struct PBitMap *p, WORD y0, WORD y1)
{
    WORD y;
    if (p != shadow || !shadowOf)
        return;
    if (y0 < 0) y0 = 0;
    if (y1 >= p->h) y1 = p->h - 1;
    for (y = y0; y <= y1; y++)
        if (!(shadowRows[y >> 3] & (1 << (y & 7)))) {
            planar_read(shadowOf, 0, y, p->w, p->pix + (ULONG)y * p->bpr);
            shadowRows[y >> 3] |= 1 << (y & 7);
        }
}

/* Run a handler on a RastPort whose bitmap is planar and not in chip RAM. */
static LONG foreign(LONG (*h)(struct Regs *), struct Regs *r, struct RastPort *rp)
{
    struct BitMap *bm = rp->BitMap;
    struct Layer *L = rp->Layer;
    UWORD w = bm->BytesPerRow * 8, hgt = bm->Rows;
    struct Surf d;
    LONG res;
    WORD y;

    /* layer first, then the lock: the order every drawing path uses
     * (the handler below locks the layer again inside the lock) */
    if (L) LockLayerRom(L);
    LOCK();
    if (shadowOf) {                      /* a handler calling a handler */
        ReleaseSemaphore(&lock);
        if (L) UnlockLayerRom(L);
        return 0;
    }
    if (!shadow || shadow->w != w || shadow->h != hgt) {
        if (shadow)
            pbm_free(shadow);
        shadow = pbm_new(w, hgt, 8, PF_CLUT8, NULL, FALSE, FALSE);
    }
    if (!shadow) {
        ReleaseSemaphore(&lock);
        if (L) UnlockLayerRom(L);
        return 0;
    }
    shadow->depth = bm->Depth;
    shadow->bm->Depth = bm->Depth;
    memset(shadowRows, 0, (hgt + 7) >> 3);
    shadowOf = bm;
    rp->BitMap = shadow->bm;
    res = h(r);
    rp->BitMap = bm;
    shadowOf = NULL;
    surf_of(bm, &d);
    for (y = 0; y < hgt; y++)
        if (shadowRows[y >> 3] & (1 << (y & 7)))
            surf_write(&d, 0, y, w, shadow->pix + (ULONG)y * shadow->bpr, 0xff);
    ReleaseSemaphore(&lock);
    if (L) UnlockLayerRom(L);
    return res;
}

/* PrismD is quitting: let go of the shadow (nothing draws any more). */
void render_quit(void)
{
    LOCK();
    if (shadow) {
        pbm_free(shadow);
        shadow = NULL;
    }
    ReleaseSemaphore(&lock);
}

#define FOREIGN(fn, reg) do { \
        struct RastPort *frp_ = (struct RastPort *)r->a[reg]; \
        if (frp_ && bm_foreign(frp_->BitMap)) return foreign(fn, r, frp_); \
    } while (0)

/* A semaphore nobody else holds: free, or owned by this task. */
static inline BOOL sem_ours(const struct SignalSemaphore *s, const struct Task *me)
{
    return s->ss_QueueCount == -1 || s->ss_Owner == me;
}

/* Locking for a drawing call. The full way is the layer's semaphore, then
 * Prism's (always in that order: callers such as Intuition often hold the
 * layer already) - 24 us on a 50 MHz 68030. A short operation (`quick`)
 * instead stays in Forbid() when neither is held by another task: nothing
 * can change the layer or use the card until Permit(), for a tenth of the
 * cost. Callbacks never wait, so the Forbid() holds. Returns TRUE if the
 * quick way was taken; hand that to draw_unlock(). */
/* Forbid() and Permit() without the calls (half a microsecond each on the
 * A4000's 68060, most of a pixel): Forbid is one increment, and Permit only
 * has work to do when the count drops below zero with a task switch
 * pending (SysFlags bit 15) - then the real pair does it. */
#define QUICK_FORBID()  (SysBase->TDNestCnt++)
#define QUICK_PERMIT()  do { \
        if (--SysBase->TDNestCnt < 0 && SysBase->IDNestCnt < 0 && \
            (SysBase->SysFlags & 0x8000)) { Forbid(); Permit(); } } while (0)

/* The drawing will touch p (bitmap coordinates, inclusive; p NULL: who
 * knows): the software pointer leaves only if it is in the way, so drawing
 * elsewhere on the screen does not make it disappear. */
static inline BOOL draw_lock(struct Layer *L, BOOL quick, struct PBitMap *p,
                             WORD x0, WORD y0, WORD x1, WORD y1)
{
    if (quick) {
        const struct Task *me = SysBase->ThisTask;
        QUICK_FORBID();
        if ((!L || sem_ours(&L->Lock, me)) && sem_ours(&lock, me)) {
            prismActivity++;             /* the compositor and the pointer watch it */
            if (p) p->modified = prismActivity;
            SW_CLEAR(p, x0, y0, x1, y1);
            return TRUE;
        }
        QUICK_PERMIT();
    }
    if (L) LockLayerRom(L);
    ObtainSemaphore(&lock);
    prismActivity++;
    if (p) p->modified = prismActivity;
    SW_CLEAR(p, x0, y0, x1, y1);
    return FALSE;
}

static inline void draw_unlock(struct Layer *L, BOOL forbidden)
{
    if (forbidden) {
        QUICK_PERMIT();
    } else {
        ReleaseSemaphore(&lock);
        if (L) UnlockLayerRom(L);
    }
}

void clip_rp(struct RastPort *rp, WORD x0, WORD y0, WORD x1, WORD y1,
                    rect_cb cb, void *ctx)
{
    clip_rp_q(rp, x0, y0, x1, y1, cb, ctx, FALSE);
}

void clip_rp_q(struct RastPort *rp, WORD x0, WORD y0, WORD x1, WORD y1,
               rect_cb cb, void *ctx, BOOL quick)
{
    struct Layer *L = rp->Layer;
    struct PBitMap *p;
    struct ClipRect *cr;
    WORD lx, ly;
    BOOL fb;

    if (x1 < x0 || y1 < y0)
        return;
    if (!L) {
        if (!(p = pbm_get(rp->BitMap)))
            return;
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 >= p->w) x1 = p->w - 1;
        if (y1 >= p->h) y1 = p->h - 1;
        if (x0 <= x1 && y0 <= y1) {
            fb = draw_lock(NULL, quick, p, x0, y0, x1, y1);
            shadow_load(p, y0, y1);
            cb(p, x0, y0, x1, y1, 0, 0, ctx);
            draw_unlock(NULL, fb);
        }
        return;
    }

    lx = L->bounds.MinX - L->Scroll_X;
    ly = L->bounds.MinY - L->Scroll_Y;
    p = pbm_get(rp->BitMap);             /* once: the ClipRects share it */
    fb = draw_lock(L, quick, p, x0 + lx, y0 + ly, x1 + lx, y1 + ly);
    for (cr = L->ClipRect; cr; cr = cr->Next) {
        WORD sx0 = x0 + lx, sy0 = y0 + ly, sx1 = x1 + lx, sy1 = y1 + ly;
        WORD ox, oy;

        if (sx0 < cr->bounds.MinX) sx0 = cr->bounds.MinX;
        if (sy0 < cr->bounds.MinY) sy0 = cr->bounds.MinY;
        if (sx1 > cr->bounds.MaxX) sx1 = cr->bounds.MaxX;
        if (sy1 > cr->bounds.MaxY) sy1 = cr->bounds.MaxY;
        if (sx0 > sx1 || sy0 > sy1)
            continue;
        if (!cr->obscured) {
            if (p) {
                shadow_load(p, sy0, sy1);
                cb(p, sx0, sy0, sx1, sy1, lx, ly, ctx);
            }
        } else if (cr->BitMap) {
            struct PBitMap *bp;
            /* backing store: its x origin is the ClipRect's 16-pixel
             * aligned left edge */
            WORD bx = cr->bounds.MinX & ~15;
            ox = lx - bx;
            oy = ly - cr->bounds.MinY;
            if ((bp = pbm_get(cr->BitMap))) {
                bp->modified = prismActivity;
                cb(bp, sx0 - bx, sy0 - cr->bounds.MinY, sx1 - bx, sy1 - cr->bounds.MinY,
                   ox, oy, ctx);
            } else {
                static BOOL told;
                if (!told) { told = TRUE; dbg("render: planar backing store skipped\n"); }
            }
        }
        /* else: simple refresh, hidden - nothing to draw */
    }
    draw_unlock(L, fb);
}

/* ---- RectFill / EraseRect / SetRast / BltPattern -------------------- */

struct FillCtx {
    struct RastPort *rp;
    UBYTE mask, dm;
    BOOL  solid;                 /* no area pattern                       */
    const UBYTE *tmask;          /* BltPattern mask plane, or NULL        */
    UWORD tmaskBPR;
    WORD  tx, ty;                /* rp coords of tmask (0,0)              */
};

static void fill_cb(struct PBitMap *p, WORD bx0, WORD by0, WORD bx1, WORD by1,
                    WORD ox, WORD oy, void *ctx)
{
    struct FillCtx *f = ctx;
    struct RastPort *rp = f->rp;
    UBYTE fg = rp->FgPen, bg = rp->BgPen, pen = 0, op;
    WORD x, y;

    if (f->solid && !f->tmask) {
        op = rule(f->dm, TRUE, fg, bg, &pen);
        if (op == 1)      fill(p, bx0, by0, bx1, by1, pen, f->mask, FALSE);
        else if (op == 2) fill(p, bx0, by0, bx1, by1, 0, f->mask, TRUE);
        return;
    }
    for (y = by0; y <= by1; y++) {
        UBYTE *row = p->pix + (ULONG)y * p->bpr;
        WORD ry = y - oy;
        UWORD pat = 0xffff;
        if (!f->solid) {
            WORD rows = 1 << (rp->AreaPtSz < 0 ? 0 : rp->AreaPtSz);
            pat = rp->AreaPtrn[ry & (rows - 1)];
        }
        for (x = bx0; x <= bx1; x++) {
            WORD rx = x - ox;
            BOOL bit = (pat >> (15 - (rx & 15))) & 1;
            if (f->tmask) {
                WORD mx = rx - f->tx, my = ry - f->ty;
                if (!(f->tmask[(ULONG)my * f->tmaskBPR + (mx >> 3)] & (0x80 >> (mx & 7))))
                    continue;
            }
            op = rule(f->dm, bit, fg, bg, &pen);
            put_px(p, row, x, op, pen, f->mask);
        }
    }
}

void blt_template(struct RastPort *rp, const UBYTE *src, WORD srcBit, WORD srcMod,
                  WORD x, WORD y, WORD w, WORD h);

static void do_rectfill(struct RastPort *rp, WORD x0, WORD y0, WORD x1, WORD y1,
                        const UBYTE *tmask, UWORD tbpr)
{
    struct FillCtx f;
    f.rp = rp;
    f.mask = rp->Mask;
    f.dm = rp->DrawMode;
    f.solid = (rp->AreaPtrn == NULL);
    f.tmask = tmask;
    f.tmaskBPR = tbpr;
    f.tx = x0;
    f.ty = y0;
    /* An area pattern with no mask is a 1-bit template: the pattern's
     * rows, repeated across and down. It goes the way text does - whole
     * rows, on the blitter where the board has one - in bands of a few
     * pattern heights. Pixel by pixel into VRAM (below) P96Speed measured
     * 5 patterned fills a second against 4,500 plain ones. The pattern is
     * anchored at the RastPort's origin, 16 pixels wide. */
    if (!f.solid && !tmask && rp->AreaPtSz >= 0 && rp->AreaPtSz <= 8 && x1 >= x0 && y1 >= y0) {
        WORD rows = 1 << rp->AreaPtSz, w = x1 - x0 + 1, h = y1 - y0 + 1;
        WORD words = ((x0 & 15) + w + 15) >> 4, band = rows, j, k, y;
        UWORD *t;
        while (band < 32 && band < h)
            band <<= 1;
        if ((t = AllocVec((ULONG)words * 2 * band, MEMF_ANY))) {
            for (j = 0; j < band; j++) {
                UWORD pat = rp->AreaPtrn[(y0 + j) & (rows - 1)];
                for (k = 0; k < words; k++)
                    t[(LONG)j * words + k] = pat;
            }
            for (y = y0; y <= y1; y += band)
                blt_template(rp, (const UBYTE *)t, x0 & 15, words * 2, x0, y, w,
                             (y1 - y + 1 < band) ? y1 - y + 1 : band);
            FreeVec(t);
            return;
        }
    }
    clip_rp_q(rp, x0, y0, x1, y1, fill_cb, &f,
              (LONG)(x1 - x0 + 1) * (y1 - y0 + 1) <= QUICK_AREA);
}

/* RectFill(rp a1, xmin d0, ymin d1, xmax d2, ymax d3) */
LONG h_RectFill(struct Regs *r)
{
    TRACE("RectFill");
    FOREIGN(h_RectFill, 1);
    struct RastPort *rp = (struct RastPort *)r->a[1];
#ifdef PRISM_TRACE
    {
        struct PBitMap *q = rp ? pbm_get(rp->BitMap) : NULL;
        dbg("  %d,%d-%d,%d bm %lx layer %lx ptrn %lx dm %u ", RW(0), RW(1), RW(2), RW(3),
            rp ? (ULONG)rp->BitMap : 0, rp ? (ULONG)rp->Layer : 0, rp ? (ULONG)rp->AreaPtrn : 0,
            rp ? rp->DrawMode : 0);
        if (q) dbg("prism %ux%u bpp %u vram %u rgbTab %lx\n", q->w, q->h, q->bpp, q->inVram, (ULONG)q->rgbTab);
        else   dbg("not prism: depth %u bpr %u rows %u\n", rp && rp->BitMap ? rp->BitMap->Depth : 0,
                   rp && rp->BitMap ? rp->BitMap->BytesPerRow : 0, rp && rp->BitMap ? rp->BitMap->Rows : 0);
    }
#endif
    if (!rp_is_prism(rp))
        return 0;
    do_rectfill(rp, RW(0), RW(1), RW(2), RW(3), NULL, 0);
#ifdef PRISM_TRACE
    dbg("  < RectFill\n");
    if (!rp->Layer && RW(2) == 47 && RW(3) == 47) {
        /* the caller's code after the call, for a disassembler */
        const UWORD *pc = (const UWORD *)((ULONG *)r)[15];
        const UBYTE *px = pbm_get(rp->BitMap)->pix;
        int i;
        dbg("  bm: depth %u bpr %u rows %u flags %02x planes %lx %lx pix %lx\n", rp->BitMap->Depth,
            rp->BitMap->BytesPerRow, rp->BitMap->Rows, rp->BitMap->Flags,
            (ULONG)rp->BitMap->Planes[0], (ULONG)rp->BitMap->Planes[1], (ULONG)px);
        dbg("  pix: %02x %02x %02x %02x %02x %02x %02x %02x\n", px[0], px[1], px[2], px[3], px[4],
            px[5], px[6], px[7]);
        dbg("  regs d0-7 %lx %lx %lx %lx %lx %lx %lx %lx\n", r->d[0], r->d[1], r->d[2], r->d[3], r->d[4],
            r->d[5], r->d[6], r->d[7]);
        dbg("  regs a0-6 %lx %lx %lx %lx %lx %lx %lx\n", r->a[0], r->a[1], r->a[2], r->a[3], r->a[4],
            r->a[5], r->a[6]);
        dbg("  caller %lx:", (ULONG)pc);
        for (i = -40; i < 200; i++) {
            if ((i & 15) == 8) dbg("\n ");
            dbg(" %04x", pc[i]);
        }
        dbg("\n");
    }
#endif
    return 1;
}

/* BltPattern(rp a1, mask a0, xmin d0, ymin d1, xmax d2, ymax d3, maskBPR d4) */
LONG h_BltPattern(struct Regs *r)
{
    TRACE("BltPattern");
    FOREIGN(h_BltPattern, 1);
    struct RastPort *rp = (struct RastPort *)r->a[1];
    if (!rp_is_prism(rp))
        return 0;
    /* A mask and no area pattern is what AreaEnd and Flood send: the mask
     * is a 1-bit template drawn in the foreground pen (JAM1 and JAM2 alike:
     * pixels outside the mask stay), so it goes the way text does - a row
     * at a time, on the blitter where the board has one - not pixel by
     * pixel (SysSpeed's area tests ran at a fifth of the native chipset's
     * speed). */
    /* The mask is what the Amiga's blitter has just drawn (AreaEnd fills
     * the TmpRas with it and calls us straight away); the ROM's BltPattern
     * queues behind that blit, so we have to wait for it. Without this a
     * 68060 read the first rows before they were filled: a few pixels
     * missing under every polygon's top corner, a different few each time
     * (A4000, PrismBench area check). The same goes for every planar
     * source below. */
    if (r->a[0])
        blit_settle();
    if (r->a[0] && !rp->AreaPtrn && rp->DrawMode <= JAM2 && RW(2) >= RW(0) && RW(3) >= RW(1)) {
        UBYTE dm = rp->DrawMode;
        rp->DrawMode = JAM1;
        blt_template(rp, (const UBYTE *)r->a[0], 0, (WORD)r->d[4], RW(0), RW(1),
                     RW(2) - RW(0) + 1, RW(3) - RW(1) + 1);
        rp->DrawMode = dm;
        return 1;
    }
    do_rectfill(rp, RW(0), RW(1), RW(2), RW(3), (const UBYTE *)r->a[0], (UWORD)r->d[4]);
    return 1;
}

struct PenCtx { UBYTE pen, mask; };

static void pen_cb(struct PBitMap *p, WORD bx0, WORD by0, WORD bx1, WORD by1,
                   WORD ox, WORD oy, void *ctx)
{
    struct PenCtx *c = ctx;
    fill(p, bx0, by0, bx1, by1, c->pen, c->mask, FALSE);
}

/* EraseRect(rp a1, xmin d0, ymin d1, xmax d2, ymax d3): we do the default
 * backfill (pen 0) and "no backfill"; a custom backfill hook goes to the
 * original, whose hook then draws through our other patches. */
LONG h_EraseRect(struct Regs *r)
{
    TRACE("EraseRect");
    FOREIGN(h_EraseRect, 1);
    struct RastPort *rp = (struct RastPort *)r->a[1];
    struct PenCtx c;

    if (!rp_is_prism(rp))
        return 0;
    if (rp->Layer && rp->Layer->BackFill != LAYERS_BACKFILL) {
        if (rp->Layer->BackFill == LAYERS_NOBACKFILL)
            return 1;
        return 0;
    }
    c.pen = 0;
    c.mask = 0xff;
    clip_rp_q(rp, RW(0), RW(1), RW(2), RW(3), pen_cb, &c,
              (LONG)(RW(2) - RW(0) + 1) * (RW(3) - RW(1) + 1) <= QUICK_AREA);
    return 1;
}

/* SetRast(rp a1, pen d0): the whole bitmap, ignoring layers */
LONG h_SetRast(struct Regs *r)
{
    TRACE("SetRast");
    FOREIGN(h_SetRast, 1);
    struct RastPort *rp = (struct RastPort *)r->a[1];
    struct PBitMap *p;
    if (!rp || !(p = pbm_get(rp->BitMap)))
        return 0;
    if (rp->Layer) {
        /* a window's RastPort: only its layer, clipped like any drawing.
         * (Personal Paint clears its title strip this way; filling the
         * whole bitmap wiped its toolbox, its canvas and every requester.) */
        struct PenCtx c;
        struct Layer *L = rp->Layer;
        c.pen = (UBYTE)r->d[0];
        c.mask = 0xff;
        clip_rp(rp, L->Scroll_X, L->Scroll_Y,
                L->Scroll_X + (L->bounds.MaxX - L->bounds.MinX),
                L->Scroll_Y + (L->bounds.MaxY - L->bounds.MinY), pen_cb, &c);
        return 1;
    }
    LOCK();
    shadow_load(p, 0, p->h - 1);
    fill(p, 0, 0, p->w - 1, p->h - 1, (UBYTE)r->d[0], 0xff, FALSE);
    ReleaseSemaphore(&lock);
    return 1;
}

/* ---- single pixels -------------------------------------------------- */

struct PixCtx { struct RastPort *rp; LONG result; };

static void wpix_cb(struct PBitMap *p, WORD bx0, WORD by0, WORD bx1, WORD by1,
                    WORD ox, WORD oy, void *ctx)
{
    struct PixCtx *c = ctx;
    UBYTE pen = 0, op = rule(c->rp->DrawMode, TRUE, c->rp->FgPen, c->rp->BgPen, &pen);
    put_px(p, p->pix + (ULONG)by0 * p->bpr, bx0, op, pen, c->rp->Mask);
    c->result = 0;
}

static void rpix_cb(struct PBitMap *p, WORD bx0, WORD by0, WORD bx1, WORD by1,
                    WORD ox, WORD oy, void *ctx)
{
    struct PixCtx *c = ctx;
    UBYTE *row = p->pix + (ULONG)by0 * p->bpr;
    c->result = (p->bpp == 2) ? pen_of(p->penTab, ((UWORD *)row)[bx0]) :
                (p->bpp >= 3) ? pen_of_rgb(p->rgbTab, pf_get(p->fmt, row + (ULONG)bx0 * p->bpp)) :
                row[bx0];
}

/* One pixel without the general ClipRect walk: find the ClipRect holding
 * it; a visible one (or no layer) is done here, under the quick lock.
 * FALSE = it sits in backing store: take the general path. */
static BOOL pixel_op(struct RastPort *rp, WORD x, WORD y, rect_cb cb, struct PixCtx *c)
{
    struct Layer *L = rp->Layer;
    struct PBitMap *p = pbm_get(rp->BitMap);
    BOOL fb, done = TRUE;

    if (swOn) {
        WORD px = x + (L ? L->bounds.MinX - L->Scroll_X : 0);
        WORD py = y + (L ? L->bounds.MinY - L->Scroll_Y : 0);
        fb = draw_lock(L, TRUE, p, px, py, px, py);
    } else
        fb = draw_lock(L, TRUE, NULL, 0, 0, 0, 0);
    if (L) {
        struct ClipRect *cr;
        WORD lx = L->bounds.MinX - L->Scroll_X, ly = L->bounds.MinY - L->Scroll_Y;
        x += lx;
        y += ly;
        for (cr = L->ClipRect; cr; cr = cr->Next)
            if (x >= cr->bounds.MinX && x <= cr->bounds.MaxX &&
                y >= cr->bounds.MinY && y <= cr->bounds.MaxY)
                break;
        if (cr) {
            if (cr->obscured)
                done = FALSE;
            else {
                shadow_load(p, y, y);
                cb(p, x, y, x, y, lx, ly, c);
            }
        }
    } else if (x >= 0 && y >= 0 && x < p->w && y < p->h) {
        shadow_load(p, y, y);
        cb(p, x, y, x, y, 0, 0, c);
    }
    draw_unlock(L, fb);
    return done;
}

/* WritePixel's short cut (see stubs.S): pen drawing with a full mask, into
 * a visible part of the layer, with nobody else holding the locks. Returns
 * the result, or 2 to hand the call to h_WritePixel. */
LONG f_WritePixel(struct RastPort *rp, LONG ax, LONG ay)
{
    struct PBitMap *p;
    struct Layer *L;
    const struct Task *me;
    WORD x = ax, y = ay;
    LONG res = -1;

    if ((rp->DrawMode & (COMPLEMENT | INVERSVID)) || rp->Mask != 0xff ||
        !(p = pbm_get(rp->BitMap)) || (p->bpp > 2 && !p->rgbTab))
        return 2;
    L = rp->Layer;
    me = SysBase->ThisTask;
    QUICK_FORBID();
    if ((L && !sem_ours(&L->Lock, me)) || !sem_ours(&lock, me))
        goto pass;
    if (L) {
        struct ClipRect *cr;
        x += L->bounds.MinX - L->Scroll_X;
        y += L->bounds.MinY - L->Scroll_Y;
        for (cr = L->ClipRect; cr; cr = cr->Next)
            if (x >= cr->bounds.MinX && x <= cr->bounds.MaxX &&
                y >= cr->bounds.MinY && y <= cr->bounds.MaxY)
                break;
        if (!cr)
            goto out;
        if (cr->obscured)
            goto pass;
    } else if (x < 0 || y < 0 || x >= p->w || y >= p->h) {
        goto out;
    }
    prismActivity++;
    p->modified = prismActivity;
    SW_CLEAR(p, x, y, x, y);
    {
        UBYTE *row = p->pix + (ULONG)y * p->bpr;
        if (p->bpp == 1)
            row[x] = rp->FgPen;
        else if (p->bpp == 2)
            ((UWORD *)row)[x] = pixval(p, rp->FgPen);
        else if (p->bpp == 4) {
            /* one long, not four bytes: each card access is 0.5 us */
            ULONG v;
            pf_put(p->fmt, pen_rgb(p->rgbTab, rp->FgPen), (UBYTE *)&v);
            ((ULONG *)row)[x] = v;
        } else
            pf_put(p->fmt, pen_rgb(p->rgbTab, rp->FgPen), row + (ULONG)x * p->bpp);
        res = 0;
    }
out:
    QUICK_PERMIT();
    return res;
pass:
    QUICK_PERMIT();
    return 2;
}

/* WritePixel(rp a1, x d0, y d1) -> 0 or -1 */
LONG h_WritePixel(struct Regs *r)
{
    TRACE("WritePixel");
    FOREIGN(h_WritePixel, 1);
    struct PixCtx c;
    c.rp = (struct RastPort *)r->a[1];
    if (!rp_is_prism(c.rp))
        return 0;
    c.result = -1;
    if (!pixel_op(c.rp, RW(0), RW(1), wpix_cb, &c))
        clip_rp(c.rp, RW(0), RW(1), RW(0), RW(1), wpix_cb, &c);
    r->d[0] = c.result;
    return 1;
}

/* ReadPixel(rp a1, x d0, y d1) -> pen or -1 */
LONG h_ReadPixel(struct Regs *r)
{
    TRACE("ReadPixel");
    FOREIGN(h_ReadPixel, 1);
    struct PixCtx c;
    c.rp = (struct RastPort *)r->a[1];
    if (!rp_is_prism(c.rp))
        return 0;
    c.result = -1;
    if (!pixel_op(c.rp, RW(0), RW(1), rpix_cb, &c))
        clip_rp(c.rp, RW(0), RW(1), RW(0), RW(1), rpix_cb, &c);
    r->d[0] = c.result;
    return 1;
}

/* ---- lines ---------------------------------------------------------- */

struct LineCtx {
    struct RastPort *rp;
    WORD x0, y0, x1, y1;         /* rp coordinates                         */
    UWORD ptrn;
};

/* A solid line that lies wholly inside one piece (the usual case): no
 * clipping, one loop per pixel size, a single pointer walking VRAM. Same
 * pixels as line_solid's loops (brute-forced against them, as those were
 * against Bresenham). */
static void line_fast(struct PBitMap *p, struct LineCtx *l, WORD ox, WORD oy, UBYTE pen)
{
    LONG x = l->x0 + ox, y = l->y0 + oy, x1 = l->x1 + ox, y1 = l->y1 + oy;
    LONG dx = x1 > x ? x1 - x : x - x1, dy = y1 > y ? y1 - y : y - y1;
    LONG rstep = y < y1 ? (LONG)p->bpr : -(LONG)p->bpr;
    BOOL right = x < x1;
    UBYTE b = p->bpp, px[4] = { 0, 0, 0, 0 };
    UBYTE *q = p->pix + y * p->bpr + x * b;
    UWORD w16 = 0;
    ULONG c4 = 0;

    if (b == 1) {
        c4 = pen * 0x01010101UL;
    } else if (b == 2) {
        w16 = pixval(p, pen);
        c4 = ((ULONG)w16 << 16) | w16;
    } else {
        pf_put(p->fmt, pen_rgb(p->rgbTab, pen), px);
    }

    if (dx >= dy) {
        /* run-slice (see line_solid): one run of pixels per row */
        LONG left = dx + 1, m, twody = 4 * dy, qq = 0, r2 = 0, rem = 0;
        if (!dy) {
            m = left;
            twody = 1;
        } else {
            /* doubled deltas, error half a unit down: see line_solid */
            LONG D = 2 * dx - twody - 2;
            qq = dx / dy;
            r2 = 4 * (dx - qq * dy);
            if (D < 0) { m = 1; rem = D + twody; }
            else       { m = D / twody + 2; rem = D - (m - 2) * twody; }
        }
        if (b == 1) {
            for (;;) {
                if (m > left) m = left;
                if (right) { vset8(q, pen, c4, m); q += m; }
                else       { q -= m; vset8(q + 1, pen, c4, m); }
                if ((left -= m) <= 0) break;
                q += rstep;
                rem += r2; m = qq;
                if (rem >= twody) { rem -= twody; m++; }
            }
        } else if (b == 2) {
            for (;;) {
                if (m > left) m = left;
                if (right) { vset16((UWORD *)q, w16, c4, m); q += 2 * m; }
                else       { q -= 2 * m; vset16((UWORD *)(q + 2), w16, c4, m); }
                if ((left -= m) <= 0) break;
                q += rstep;
                rem += r2; m = qq;
                if (rem >= twody) { rem -= twody; m++; }
            }
        } else {
            /* 3/4 bytes: the longest run's worth of pixels composed once
             * in fast RAM, each run copied out */
            LONG n = dy ? qq + 2 : left, k;
            UBYTE *o = rowW;
            if (n > left) n = left;
            for (k = 0; k < n; k++, o += b) {
                o[0] = px[0]; o[1] = px[1]; o[2] = px[2];
                if (b == 4) o[3] = px[3];
            }
            for (;;) {
                if (m > left) m = left;
                if (right) { vcopy(q, rowW, m * b); q += m * b; }
                else       { q -= m * b; vcopy(q + b, rowW, m * b); }
                if ((left -= m) <= 0) break;
                q += rstep;
                rem += r2; m = qq;
                if (rem >= twody) { rem -= twody; m++; }
            }
        }
    } else {
        /* mostly vertical: one pixel per row; y always steps */
        LONG dx2 = 2 * dx, dy2 = 2 * dy, err = dx2 - dy2 + 1, n, xs = right ? b : -(LONG)b;
        if (b == 1) {
            for (n = dy + 1; n; n--) {
                *q = pen;
                if (err > -dy) { err -= dy2; q += xs; }
                err += dx2; q += rstep;
            }
        } else if (b == 2) {
            for (n = dy + 1; n; n--) {
                *(UWORD *)q = w16;
                if (err > -dy) { err -= dy2; q += xs; }
                err += dx2; q += rstep;
            }
        } else if (b == 4) {
            ULONG v = ((ULONG)px[0] << 24) | ((ULONG)px[1] << 16) | ((ULONG)px[2] << 8) | px[3];
            for (n = dy + 1; n; n--) {
                *(ULONG *)q = v;
                if (err > -dy) { err -= dy2; q += xs; }
                err += dx2; q += rstep;
            }
        } else {
            /* 3 bytes in two bus cycles: byte + word or word + byte */
            UWORD w01 = ((UWORD)px[0] << 8) | px[1], w12 = ((UWORD)px[1] << 8) | px[2];
            for (n = dy + 1; n; n--) {
                if ((ULONG)q & 1) { q[0] = px[0]; *(UWORD *)(q + 1) = w12; }
                else              { *(UWORD *)q = w01; q[2] = px[2]; }
                if (err > -dy) { err -= dy2; q += xs; }
                err += dx2; q += rstep;
            }
        }
    }
}

/* Solid line (every pixel the same colour, full write mask): the same
 * Bresenham steps as line_cb, but the pixel's bytes are worked out once,
 * the row pointer moves with y, and nothing is read back from VRAM - a
 * read across Zorro II costs far more than the write. */
/* The blitter attempt, out of line: with it inlined next to line_fast
 * the compiler spilled the colour and the run count to the stack inside
 * the 16-bit per-run loop (-12%). */
static __attribute__((noinline)) BOOL line_hw(struct PBitMap *p, WORD x, WORD y, WORD x1, WORD y1,
                                              UBYTE pen)
{
    UBYTE b = p->bpp, px[4] = { 0, 0, 0, 0 };
    ULONG c;
    if (b == 1)      c = pen;
    else if (b == 2) c = pixval(p, pen);
    else {
        pf_put(p->fmt, pen_rgb(p->rgbTab, pen), px);
        c = ((ULONG)px[0] << 24) | ((ULONG)px[1] << 16) | ((ULONG)px[2] << 8) | px[3];
    }
    return pbm_hw_line(p, x, y, x1 - x, y1 - y, c) == PR_DONE;
}

static void line_solid(struct PBitMap *p, struct LineCtx *l, WORD bx0, WORD by0,
                       WORD bx1, WORD by1, WORD ox, WORD oy, UBYTE pen)
{
    WORD x = l->x0 + ox, y = l->y0 + oy, x1 = l->x1 + ox, y1 = l->y1 + oy;
    WORD dx = x1 > x ? x1 - x : x - x1, dy = y1 > y ? y1 - y : y - y1;
    WORD sx = x < x1 ? 1 : -1, sy = y < y1 ? 1 : -1;
    /* The native rule (graphics.library, checked on the A4000 with lines
     * whose long delta is exactly twice the short one): after i steps along
     * the longer axis the line has moved round(i * S / L) along the shorter,
     * halves rounded UP, from the starting end, in every direction. Plain
     * Bresenham rounds a half down; working in doubled deltas with the
     * error moved half a unit gives the native pixels (brute-forced
     * against the formula for every delta up to 90). */
    LONG dx2 = 2 * (LONG)dx, dy2 = 2 * (LONG)dy;
    LONG err = dx2 - dy2 + (dx > dy ? -1 : dx < dy ? 1 : 0), e2;
    LONG rstep = sy > 0 ? (LONG)p->bpr : -(LONG)p->bpr;
    UBYTE b = p->bpp, px[4] = { 0, 0, 0, 0 };
    UWORD w16 = 0;
    UBYTE *row = p->pix + (LONG)y * p->bpr;

    /* nothing to clip? */
    if ((x < x1 ? x : x1) >= bx0 && (x < x1 ? x1 : x) <= bx1 &&
        (y < y1 ? y : y1) >= by0 && (y < y1 ? y1 : y) <= by1) {
        /* long enough to be worth a blitter command */
        if (BOARD_CAN(line) && p->inVram && b != 3 && (dx > 40 || dy > 40) &&
            (b != 4 || (board.flags & PBF_BLIT_32)) && line_hw(p, x, y, x1, y1, pen))
            return;
        line_fast(p, l, ox, oy, pen);
        return;
    }

    if (b == 1)      px[0] = pen;
    else if (b == 2) w16 = pixval(p, pen);
    else             pf_put(p->fmt, pen_rgb(p->rgbTab, pen), px);

    if (dx >= dy && dy > 0) {
        /* Mostly horizontal, run-slice: the same pixels as the Bresenham
         * steps below (checked by brute force over every slope up to 200),
         * but a row at a time. With error e at a run's first pixel and
         * D = 2e - dx, the run is 1 pixel if D < 0, else D / 2dy + 2; after
         * that every run is dx / dy pixels, plus one whenever the running
         * remainder passes 2dy - so the divisions happen once per line. */
        LONG ldx = dx2, ldy = dy2, left = dx + 1, cx = x, cy = y, m;
        LONG twody = 2 * ldy, q = ldx / ldy, r2 = 2 * (ldx - q * ldy), rem;
        LONG D = ldx - twody - 2;
        if (D < 0) { m = 1; rem = D + twody; }
        else       { m = D / twody + 2; rem = D - (m - 2) * twody; }
        for (;;) {
            if (m > left)
                m = left;
            {
                LONG lo = sx > 0 ? cx : cx - m + 1, hi = lo + m - 1, k;
                if (lo < bx0) lo = bx0;
                if (hi > bx1) hi = bx1;
                if (cy >= by0 && cy <= by1 && lo <= hi) {
                    switch (b) {
                    case 1: {
                        /* Zorro II is 16 bits wide: a byte write costs a
                         * whole bus cycle, so write words and longs */
                        UBYTE *q = row + lo, c = px[0];
                        LONG cnt = hi - lo + 1;
                        if (((ULONG)q & 1) && cnt) { *q++ = c; cnt--; }
                        if (cnt >= 4) {
                            ULONG c4 = c * 0x01010101UL;
                            if (((ULONG)q & 2) && cnt >= 2) { *(UWORD *)q = c4; q += 2; cnt -= 2; }
                            for (; cnt >= 4; cnt -= 4, q += 4) *(ULONG *)q = c4;
                        }
                        if (cnt >= 2) { *(UWORD *)q = c * 0x0101; q += 2; cnt -= 2; }
                        if (cnt) *q = c;
                        break;
                    }
                    case 2:  { UWORD *q = (UWORD *)row + lo;
                               for (k = lo; k <= hi; k++) *q++ = w16; } break;
                    default:
                        if (hi - lo < 24) {
                            for (k = lo; k <= hi; k++) {
                                UBYTE *q = row + k * b;
                                q[0] = px[0]; q[1] = px[1]; q[2] = px[2];
                                if (b == 4) q[3] = px[3];
                            }
                        } else {
                            /* compose in fast RAM, out with long writes */
                            UBYTE *q = rowW;
                            for (k = lo; k <= hi; k++, q += b) {
                                q[0] = px[0]; q[1] = px[1]; q[2] = px[2];
                                if (b == 4) q[3] = px[3];
                            }
                            CopyMem(rowW, row + lo * b, (hi - lo + 1) * b);
                        }
                    }
                }
            }
            left -= m;
            if (left <= 0)
                break;
            cx += m * sx;
            cy += sy;
            row += rstep;
            rem += r2;
            m = q;
            if (rem >= twody) { rem -= twody; m++; }
        }
        return;
    }
    for (;;) {
        if (x >= bx0 && x <= bx1 && y >= by0 && y <= by1) {
            switch (b) {
            case 1:  row[x] = px[0]; break;
            case 2:  ((UWORD *)row)[x] = w16; break;
            default: {
                UBYTE *q = row + (LONG)x * b;
                q[0] = px[0]; q[1] = px[1]; q[2] = px[2];
                if (b == 4) q[3] = px[3];
            }
            }
        }
        if (x == x1 && y == y1)
            break;
        e2 = 2 * err;
        if (e2 > -dy2) { err -= dy2; x += sx; }
        if (e2 < dx2)  { err += dx2; y += sy; row += rstep; }
    }
}

/* Plot the part of the line that falls in this piece. */
static void line_cb(struct PBitMap *p, WORD bx0, WORD by0, WORD bx1, WORD by1,
                    WORD ox, WORD oy, void *ctx)
{
    struct LineCtx *l = ctx;
    struct RastPort *rp = l->rp;
    WORD x = l->x0, y = l->y0, dx, dy, sx, sy, n = 0;
    LONG dx2, dy2, err, e2;
    UBYTE fg = rp->FgPen, bg = rp->BgPen;

    /* solid pattern, plain JAM1/JAM2 drawing in the foreground pen */
    if (l->ptrn == 0xffff && rp->Mask == 0xff && !(rp->DrawMode & (COMPLEMENT | INVERSVID))) {
        line_solid(p, l, bx0, by0, bx1, by1, ox, oy, fg);
        return;
    }

    dx = l->x1 > x ? l->x1 - x : x - l->x1;
    dy = l->y1 > y ? l->y1 - y : y - l->y1;
    sx = x < l->x1 ? 1 : -1;
    sy = y < l->y1 ? 1 : -1;
    dx2 = 2 * (LONG)dx;
    dy2 = 2 * (LONG)dy;
    err = dx2 - dy2 + (dx > dy ? -1 : dx < dy ? 1 : 0);   /* see line_solid */
    for (;;) {
        WORD px = x + ox, py = y + oy;
        if (px >= bx0 && px <= bx1 && py >= by0 && py <= by1) {
            UBYTE pen = 0;
            BOOL bit = (l->ptrn >> (15 - (n & 15))) & 1;
            UBYTE op = rule(rp->DrawMode, bit, fg, bg, &pen);
            put_px(p, p->pix + (ULONG)py * p->bpr, px, op, pen, rp->Mask);
        }
        if (x == l->x1 && y == l->y1)
            break;
        e2 = 2 * err;
        if (e2 > -dy2) { err -= dy2; x += sx; }
        if (e2 < dx2)  { err += dx2; y += sy; }
        n++;
    }
}

static void do_draw(struct RastPort *rp, WORD x1, WORD y1)
{
    struct LineCtx l;
    WORD x0 = rp->cp_x, y0 = rp->cp_y;

    l.rp = rp;
    l.x0 = x0; l.y0 = y0; l.x1 = x1; l.y1 = y1;
    l.ptrn = rp->LinePtrn;
    if (l.ptrn == 0xffff && (x0 == x1 || y0 == y1) && !(rp->DrawMode & (COMPLEMENT | INVERSVID))) {
        /* solid horizontal/vertical: a 1-pixel fill */
        struct PenCtx c;
        c.pen = rp->FgPen;
        c.mask = rp->Mask;
        clip_rp_q(rp, x0 < x1 ? x0 : x1, y0 < y1 ? y0 : y1,
                      x0 < x1 ? x1 : x0, y0 < y1 ? y1 : y0, pen_cb, &c, TRUE);
    } else {
        clip_rp_q(rp, x0 < x1 ? x0 : x1, y0 < y1 ? y0 : y1,
                      x0 < x1 ? x1 : x0, y0 < y1 ? y1 : y0, line_cb, &l, TRUE);
    }
    rp->cp_x = x1;
    rp->cp_y = y1;
}

/* Draw(rp a1, x d0, y d1) */
LONG h_Draw(struct Regs *r)
{
    TRACE("Draw");
    FOREIGN(h_Draw, 1);
    struct RastPort *rp = (struct RastPort *)r->a[1];
    if (!rp_is_prism(rp))
        return 0;
    do_draw(rp, RW(0), RW(1));
    return 1;
}

/* PolyDraw(rp a1, count d0, array a0) */
LONG h_PolyDraw(struct Regs *r)
{
    TRACE("PolyDraw");
    FOREIGN(h_PolyDraw, 1);
    struct RastPort *rp = (struct RastPort *)r->a[1];
    const WORD *a = (const WORD *)r->a[0];
    WORD n = RW(0), i;
    if (!rp_is_prism(rp))
        return 0;
    for (i = 0; i < n; i++)
        do_draw(rp, a[i * 2], a[i * 2 + 1]);
    return 1;
}

/* ---- templates and text ---------------------------------------------
 *
 * Text and BltTemplate both end up as a 1-bit template expanded through
 * the draw mode. Text first builds the whole string's template in fast
 * RAM (glyphs, bold smear, underline), so the bitmap is written once per
 * pixel - and for JAM2 a row at a time, composed in fast RAM and copied
 * with long writes, which is what the Zorro bus likes.
 */

/* 8 template bits starting at bit position pos (MSB first). Reads one
 * byte past the bits it needs; callers' templates allow for that. */
static inline UBYTE get8(const UBYTE *src, LONG pos)
{
    const UBYTE *s = src + (pos >> 3);
    UBYTE sh = pos & 7;
    return sh ? (UBYTE)((s[0] << sh) | (s[1] >> (8 - sh))) : s[0];
}

/* OR n bits from src (starting at bit sbit) into dst at bit dbit. */
static void or_bits(UBYTE *dst, LONG dbit, const UBYTE *src, LONG sbit, WORD n)
{
    WORD k;
    for (k = 0; k < n; k += 8) {
        UBYTE v = get8(src, sbit + k);
        LONG d = dbit + k;
        UBYTE sh = d & 7;
        if (n - k < 8)
            v &= (UBYTE)(0xff << (8 - (n - k)));
        if (!v)
            continue;
        dst[d >> 3] |= v >> sh;
        if (sh)
            dst[(d >> 3) + 1] |= (UBYTE)(v << (8 - sh));
    }
}

/* JAM2 for 2/3/4-byte pixels: a template nibble picks 4 pixels' worth of
 * bytes (b longs) from a table built for the fg/bg pair, cached across
 * rows and calls; the row goes out with one CopyMem. */
static ULONG jamTab[16][4];
static UBYTE jamKey[9];

static void jam2_row(UBYTE b, const UBYTE *f, const UBYTE *g, const UBYTE *src, LONG bp,
                     UBYTE invb, WORD n, UBYTE *dst)
{
    ULONG *o = rowWbuf;
    WORD i;
    UBYTE j, k, q;

    if (jamKey[0] != b || memcmp(jamKey + 1, f, b) || memcmp(jamKey + 5, g, b)) {
        for (j = 0; j < 16; j++) {
            UBYTE *e = (UBYTE *)jamTab[j];
            for (k = 0; k < 4; k++) {
                const UBYTE *c = (j & (8 >> k)) ? f : g;
                for (q = 0; q < b; q++)
                    *e++ = c[q];
            }
        }
        jamKey[0] = b;
        memcpy(jamKey + 1, f, b);
        memcpy(jamKey + 5, g, b);
    }
    for (i = 0; i < n; i += 8, bp += 8) {
        UBYTE v = get8(src, bp) ^ invb;
        const ULONG *h = jamTab[v >> 4], *l = jamTab[v & 15];
        switch (b) {
        case 2: o[0] = h[0]; o[1] = h[1]; o[2] = l[0]; o[3] = l[1]; o += 4; break;
        case 3: o[0] = h[0]; o[1] = h[1]; o[2] = h[2];
                o[3] = l[0]; o[4] = l[1]; o[5] = l[2]; o += 6; break;
        default: o[0] = h[0]; o[1] = h[1]; o[2] = h[2]; o[3] = h[3];
                 o[4] = l[0]; o[5] = l[1]; o[6] = l[2]; o[7] = l[3]; o += 8; break;
        }
    }
    vcopy(dst, rowW, (LONG)n * b);
}

/* expand_row for 24/32-bit: JAM2 rows composed in fast RAM, JAM1 and
 * complement per set bit. */
static void expand_deep(struct PBitMap *p, UBYTE *d, WORD n, const UBYTE *src, LONG bp,
                        UBYTE dm, UBYTE invb, UBYTE fg, UBYTE bg)
{
    UBYTE f[4], g[4], b = p->bpp, v, k, cnt;
    WORD i;

    pf_put(p->fmt, pen_rgb(p->rgbTab, fg), f);
    pf_put(p->fmt, pen_rgb(p->rgbTab, bg), g);
    if ((dm & JAM2) && !(dm & COMPLEMENT)) {
        jam2_row(b, f, g, src, bp, invb, n, d);
        return;
    }
    if (!(dm & COMPLEMENT)) {
        /* JAM1: a pixel is one long (32-bit) or a word and a byte
         * (24-bit) on the bus, not three or four byte writes. (Copying
         * whole runs of set bits was tried: slower, even on the A2000.) */
        ULONG f32 = ((ULONG)f[0] << 24) | ((ULONG)f[1] << 16) | ((ULONG)f[2] << 8) | f[3];
        UWORD w01 = ((UWORD)f[0] << 8) | f[1], w12 = ((UWORD)f[1] << 8) | f[2];
        for (i = 0; i < n; i += 8, bp += 8) {
            if (!(v = get8(src, bp) ^ invb))
                continue;
            cnt = (n - i < 8) ? n - i : 8;
            for (k = 0; k < cnt; k++)
                if (v & (0x80 >> k)) {
                    UBYTE *q = d + (ULONG)(i + k) * b;
                    if (b == 4)              *(ULONG *)q = f32;
                    else if ((ULONG)q & 1) { q[0] = f[0]; *(UWORD *)(q + 1) = w12; }
                    else                   { *(UWORD *)q = w01; q[2] = f[2]; }
                }
        }
        return;
    }
    for (i = 0; i < n; i += 8, bp += 8) {
        if (!(v = get8(src, bp) ^ invb))
            continue;
        cnt = (n - i < 8) ? n - i : 8;
        for (k = 0; k < cnt; k++)
            if (v & (0x80 >> k)) {
                UBYTE *q = d + (ULONG)(i + k) * b;
                pf_put(p->fmt, pf_get(p->fmt, q) ^ 0xffffff, q);
            }
    }
}

/* Expand one template row into bitmap row y, pixels bx0..bx1; template
 * bit `bit0` (MSB-first) lands on bx0. Works 8 template bits at a time. */
static void expand_row(struct PBitMap *p, WORD y, WORD bx0, WORD bx1,
                       const UBYTE *src, LONG bit0, struct RastPort *rp)
{
    UBYTE dm = rp->DrawMode, mask = rp->Mask, fg = rp->FgPen, bg = rp->BgPen;
    UBYTE invb = (dm & INVERSVID) ? 0xff : 0, v, k, cnt;
    WORD n = bx1 - bx0 + 1, i;
    UBYTE *row = p->pix + (ULONG)y * p->bpr;
    LONG bp = bit0;

    if (!mask || n <= 0)
        return;
    if (n > MAXW) n = MAXW;

    if (p->bpp >= 3) {
        expand_deep(p, row + (ULONG)bx0 * p->bpp, n, src, bp, dm, invb, fg, bg);
        return;
    }
    if (dm & COMPLEMENT) {
        for (i = 0; i < n; i += 8, bp += 8) {
            if (!(v = get8(src, bp) ^ invb))
                continue;
            cnt = (n - i < 8) ? n - i : 8;
            for (k = 0; k < cnt; k++)
                if (v & (0x80 >> k)) {
                    if (p->bpp == 2) ((UWORD *)row)[bx0 + i + k] ^= 0xffff;
                    else             row[bx0 + i + k] ^= mask;
                }
        }
        return;
    }
    if (dm & JAM2) {
        if (p->bpp == 2) {
            UWORD f = pixval(p, fg), g = pixval(p, bg);
            jam2_row(2, (const UBYTE *)&f, (const UBYTE *)&g, src, bp, invb, n,
                     (UBYTE *)((UWORD *)row + bx0));
        } else if (mask == 0xff) {
            /* nibble -> four pixels, one long write each */
            ULONG tab[16];
            UBYTE j;
            for (j = 0; j < 16; j++)
                tab[j] = ((ULONG)((j & 8) ? fg : bg) << 24) | ((ULONG)((j & 4) ? fg : bg) << 16) |
                         ((ULONG)((j & 2) ? fg : bg) << 8) | ((j & 1) ? fg : bg);
            for (i = 0; i < n; i += 8, bp += 8) {
                v = get8(src, bp) ^ invb;
                ((ULONG *)(rowA + i))[0] = tab[v >> 4];
                ((ULONG *)(rowA + i))[1] = tab[v & 15];
            }
            CopyMem(rowA, row + bx0, n);
        } else {
            for (i = 0; i < n; i += 8, bp += 8) {
                v = get8(src, bp) ^ invb;
                cnt = (n - i < 8) ? n - i : 8;
                for (k = 0; k < cnt; k++)
                    row[bx0 + i + k] = (row[bx0 + i + k] & ~mask) |
                                       (((v & (0x80 >> k)) ? fg : bg) & mask);
            }
        }
        return;
    }
    /* JAM1: only the set bits */
    for (i = 0; i < n; i += 8, bp += 8) {
        if (!(v = get8(src, bp) ^ invb))
            continue;
        cnt = (n - i < 8) ? n - i : 8;
        if (p->bpp == 2) {
            UWORD f = pixval(p, fg), *w = (UWORD *)row + bx0 + i;
            for (k = 0; k < cnt; k++)
                if (v & (0x80 >> k)) w[k] = f;
        } else if (mask == 0xff) {
            UBYTE *b = row + bx0 + i;
            for (k = 0; k < cnt; k++)
                if (v & (0x80 >> k)) b[k] = fg;
        } else {
            UBYTE *b = row + bx0 + i;
            for (k = 0; k < cnt; k++)
                if (v & (0x80 >> k)) b[k] = (b[k] & ~mask) | (fg & mask);
        }
    }
}

struct TmplCtx {
    struct RastPort *rp;
    const UBYTE *src;            /* 1-bit template                         */
    LONG  srcBit;                /* bit offset of rp x = tx in each row    */
    WORD  srcMod;                /* bytes per template row                 */
    WORD  tx, ty;                /* rp coords of the template's (0,0)      */
};

/* the piece's template, byte aligned, for the board's colour expansion */
static UBYTE xtmpl[8192];

static void tmpl_cb(struct PBitMap *p, WORD bx0, WORD by0, WORD bx1, WORD by1,
                    WORD ox, WORD oy, void *ctx)
{
    struct TmplCtx *t = ctx;
    struct RastPort *rp = t->rp;
    WORD y, w = bx1 - bx0 + 1, h = by1 - by0 + 1;
    UBYTE dm = rp->DrawMode;

    /* On the card's blitter (Cirrus colour expansion): JAM1 = transparent,
     * JAM2 = opaque, INVERSVID = the template inverted. */
    if (BOARD_CAN(expand) && p->inVram && rp->Mask == 0xff && !(dm & COMPLEMENT) &&
        (LONG)w * h >= 64 &&
        (p->bpp <= 2 || (p->bpp == 4 && (board.flags & PBF_BLIT_32)))) {
        ULONG rb = ((ULONG)w + 7) >> 3, k, fgv, bgv;
        UBYTE invb = (dm & INVERSVID) ? 0xff : 0;
        LONG sbit0 = t->srcBit + (bx0 - ox - t->tx);
        if (p->bpp == 4) {
            /* 32-bit: the four VRAM bytes, as the 68k stores the pixel */
            UBYTE e[4] = { 0, 0, 0, 0 };
            pf_put(p->fmt, pen_rgb(p->rgbTab, rp->FgPen), e);
            fgv = ((ULONG)e[0] << 24) | ((ULONG)e[1] << 16) | ((ULONG)e[2] << 8) | e[3];
            pf_put(p->fmt, pen_rgb(p->rgbTab, rp->BgPen), e);
            bgv = ((ULONG)e[0] << 24) | ((ULONG)e[1] << 16) | ((ULONG)e[2] << 8) | e[3];
        } else if (p->bpp == 3) {
            /* 24-bit: the three VRAM bytes as $00b0b1b2 */
            UBYTE e[4] = { 0, 0, 0, 0 };
            pf_put(p->fmt, pen_rgb(p->rgbTab, rp->FgPen), e);
            fgv = ((ULONG)e[0] << 16) | ((ULONG)e[1] << 8) | e[2];
            pf_put(p->fmt, pen_rgb(p->rgbTab, rp->BgPen), e);
            bgv = ((ULONG)e[0] << 16) | ((ULONG)e[1] << 8) | e[2];
        } else {
            fgv = pixval(p, rp->FgPen);
            bgv = pixval(p, rp->BgPen);
        }
        if (!invb && !(sbit0 & 7) && sbit0 >= 0) {
            /* byte-aligned (unclipped text always is): the template rows
             * go to the chip as they are */
            const UBYTE *src = t->src + (LONG)(by0 - oy - t->ty) * t->srcMod + (sbit0 >> 3);
            if (pbm_expand(p, bx0, by0, w, h, src,
                                 t->srcMod, fgv, bgv,
                                 !(dm & JAM2)) == PR_DONE)
                return;
        } else if (rb * h <= sizeof(xtmpl)) {
            UBYTE *o = xtmpl;
            LONG sbit = t->srcBit + (bx0 - ox - t->tx);
            for (y = by0; y <= by1; y++) {
                const UBYTE *src = t->src + (LONG)(y - oy - t->ty) * t->srcMod;
                for (k = 0; k < rb; k++)
                    *o++ = get8(src, sbit + (LONG)k * 8) ^ invb;
            }
            if (pbm_expand(p, bx0, by0, w, h, xtmpl, rb,
                                 fgv, bgv, !(dm & JAM2)) == PR_DONE)
                return;
        }
    }
    for (y = by0; y <= by1; y++)
        expand_row(p, y, bx0, bx1, t->src + (LONG)(y - oy - t->ty) * t->srcMod,
                   t->srcBit + (bx0 - ox - t->tx), t->rp);
}

void blt_template(struct RastPort *rp, const UBYTE *src, WORD srcBit, WORD srcMod,
                  WORD x, WORD y, WORD w, WORD h)
{
    struct TmplCtx t;

    t.rp = rp;
    t.src = src;
    t.srcBit = srcBit;
    t.srcMod = srcMod;
    t.tx = x;
    t.ty = y;
    clip_rp_q(rp, x, y, x + w - 1, y + h - 1, tmpl_cb, &t, (LONG)w * h <= QUICK_AREA);
}

/* BltTemplate(src a0, xSrc d0, srcMod d1, rp a1, xDest d2, yDest d3,
 *             xSize d4, ySize d5) */
LONG h_BltTemplate(struct Regs *r)
{
    TRACE("BltTemplate");
    FOREIGN(h_BltTemplate, 1);
    struct RastPort *rp = (struct RastPort *)r->a[1];

    if (!rp_is_prism(rp))
        return 0;
    if (RW(4) > 0 && RW(5) > 0) {
        blit_settle();                       /* the template may still be drawn */
        blt_template(rp, (const UBYTE *)r->a[0], RW(0), RW(1), RW(2), RW(3), RW(4), RW(5));
    }
    return 1;
}

static inline UWORD glyph_index(struct TextFont *tf, UBYTE c)
{
    return (c < tf->tf_LoChar || c > tf->tf_HiChar) ? tf->tf_HiChar - tf->tf_LoChar + 1
                                                    : c - tf->tf_LoChar;
}

/* Text(rp a1, string a0, count d0) */
/* Text runs on the CALLER's stack: Workbench, IPrefs, input.device - often
 * 4 KB, already deep in Intuition and layers. A 1 KB buffer and two
 * 256-entry tables in h_Text (1.9 KB in all) overflowed it at boot on
 * Kickstart 3.1 once anything else grew; they now come from this pool
 * (tests/stack_audit.sh checks every chain). */
#define TEXT_POOL     4                 /* strings drawn at once without AllocVec */
#define TEXT_TMPL     1024              /* the template part of a pool buffer */
#define TEXT_POOLBUF  (TEXT_TMPL + 512 + 256)   /* + glyph offsets and masks (256 each) */

/* Text's template and glyph-table buffers: static (not on the caller's stack),
 * claimed under a Forbid of a few instructions and never waited for - no
 * lock is held while the template is built, so there is no lock order to
 * get wrong. More than TEXT_POOL tasks drawing at once fall back to
 * AllocVec. */
static ULONG textPool[TEXT_POOL][TEXT_POOLBUF / 4];
static UBYTE textPoolUsed[TEXT_POOL];

static UBYTE *text_buf_get(ULONG size)
{
    WORD i;
    if (size <= TEXT_POOLBUF) {
        Forbid();
        for (i = 0; i < TEXT_POOL; i++)
            if (!textPoolUsed[i]) {
                textPoolUsed[i] = 1;
                Permit();
                return (UBYTE *)textPool[i];
            }
        Permit();
    }
    return AllocVec(size, MEMF_ANY);
}

static void text_buf_put(UBYTE *b)
{
    WORD i;
    for (i = 0; i < TEXT_POOL; i++)
        if (b == (UBYTE *)textPool[i]) {
            textPoolUsed[i] = 0;
            return;
        }
    FreeVec(b);
}

/* Fixed 8-pixel fonts (topaz 8: Workbench, Shells, most windows) with no
 * style or extra spacing: every glyph is one byte per row at a byte offset
 * in the font data, so the template is a byte per character per row - no
 * extent pass, no shifting, rows contiguous for the chip. FALSE: not this
 * kind of string (nothing drawn). */
LONG h_Text(struct Regs *r)
{
    TRACE("Text");
    FOREIGN(h_Text, 1);
    struct RastPort *rp = (struct RastPort *)r->a[1];
    const UBYTE *str = (const UBYTE *)r->a[0];
    WORD count = (WORD)r->d[0], i, smear, x0, x1, width, cx, top;
    struct TextFont *tf;
    const ULONG *loc;
    const WORD *space, *kern;
    /* The caller's stack (Workbench, IPrefs, input.device: often 4 KB,
     * already deep in Intuition and layers) is no place for the 1.8 KB of
     * template and glyph tables this used to keep there - it overflowed at
     * boot on Kickstart 3.1. They come from the static pool (text_buf_get:
     * claimed without waiting, AllocVec when all are taken). */
    UBYTE style, *pool, *stackbuf, *tmpl;
    ULONG tbpr, size;
    struct TmplCtx t;

    if (!rp_is_prism(rp))
        return 0;
    if (!(tf = rp->Font) || count <= 0)
        return 1;
    if (!(pool = text_buf_get(TEXT_POOLBUF)))
        return 1;
    stackbuf = pool;
    loc = tf->tf_CharLoc;
    space = tf->tf_CharSpace;
    kern = tf->tf_CharKern;
    style = rp->AlgoStyle & ~tf->tf_Style;
    smear = (style & FSF_BOLD) ? (tf->tf_BoldSmear ? tf->tf_BoldSmear : 1) : 0;

    /* Fixed 8-pixel fonts (topaz 8: Workbench, Shells, most windows) with
     * no style or extra spacing: every glyph is one byte per row at a byte
     * offset in the font data, so the template is a byte per character per
     * row - no extent pass, no shifting, rows contiguous for the chip. */
    if (tf->tf_XSize == 8 && !(tf->tf_Flags & FPF_PROPORTIONAL) && !kern && !space &&
        !style && !rp->TxSpacing && count <= 256 && (ULONG)count * tf->tf_YSize + 4 <= TEXT_TMPL) {
        UWORD *off = (UWORD *)(pool + TEXT_TMPL);
        UBYTE *msk = pool + TEXT_TMPL + 512, gy;
        for (i = 0; i < count; i++) {
            ULONG l = loc[glyph_index(tf, str[i])];
            UWORD bo = l >> 16, bw = l & 0xffff;
            if ((bo & 7) || bw > 8)
                break;
            off[i] = bo >> 3;
            msk[i] = (UBYTE)(0xff00 >> bw);
        }
        if (i == count) {
            const UBYTE *cd = (const UBYTE *)tf->tf_CharData;
            UBYTE *o = stackbuf;
            for (gy = 0; gy < tf->tf_YSize; gy++, cd += tf->tf_Modulo)
                for (i = 0; i < count; i++)
                    *o++ = cd[off[i]] & msk[i];
            top = rp->cp_y - tf->tf_Baseline;
            t.rp = rp;
            t.src = stackbuf;
            t.srcBit = 0;
            t.srcMod = count;
            t.tx = rp->cp_x;
            t.ty = top;
            clip_rp_q(rp, t.tx, top, rp->cp_x + count * 8 - 1, top + tf->tf_YSize - 1, tmpl_cb,
                      &t, (LONG)count * 8 * tf->tf_YSize <= QUICK_AREA);
            rp->cp_x += count * 8;
            text_buf_put(pool);
            return 1;
        }
    }

    /* extent: the advance [0, width) plus any glyph overhang (kerning) */
    x0 = 0;
    x1 = 0;
    cx = 0;
    for (i = 0; i < count; i++) {
        UWORD idx = glyph_index(tf, str[i]);
        WORD gx = cx + (kern ? kern[idx] : 0), gw = (loc[idx] & 0xffff) + smear;
        if (gx < x0) x0 = gx;
        if (gx + gw > x1) x1 = gx + gw;
        cx += (space ? space[idx] : tf->tf_XSize) + rp->TxSpacing + smear;
    }
    width = cx;
    if (width > x1) x1 = width;
    if (x1 <= x0) {
        rp->cp_x += width;
        text_buf_put(pool);
        return 1;
    }

    /* the string as one 1-bit template, origin at x0 */
    tbpr = ((x1 - x0 + 7) >> 3) + 4;          /* fast glyph path writes 4 bytes */
    size = tbpr * tf->tf_YSize;
    if (size <= TEXT_TMPL)
        tmpl = stackbuf;
    else if (!(tmpl = AllocVec(size, MEMF_ANY))) {
        text_buf_put(pool);
        return 1;
    }
    memset(tmpl, 0, size);
    cx = 0;
    for (i = 0; i < count; i++) {
        UWORD idx = glyph_index(tf, str[i]), bitoff = loc[idx] >> 16, bitw = loc[idx] & 0xffff, gy;
        WORD gx = cx + (kern ? kern[idx] : 0) - x0;
        if (!bitw)
            ;
        else if (bitw <= 24 && !smear) {
            /* the common case: a glyph row is one unaligned long read,
             * masked, shifted into place and ORed as four bytes */
            const UBYTE *grow = (const UBYTE *)tf->tf_CharData + (bitoff >> 3);
            UBYTE *trow = tmpl + (gx >> 3);
            ULONG keep = 0xffffffffUL << (32 - bitw);
            UBYTE ss = bitoff & 7, ds = gx & 7;
            for (gy = 0; gy < tf->tf_YSize; gy++, grow += tf->tf_Modulo, trow += tbpr) {
                ULONG v = ((*(const ULONG *)grow << ss) & keep) >> ds;
                if (!v)
                    continue;
                trow[0] |= v >> 24; trow[1] |= v >> 16; trow[2] |= v >> 8; trow[3] |= v;
            }
        } else {
            for (gy = 0; gy < tf->tf_YSize; gy++) {
                const UBYTE *grow = (const UBYTE *)tf->tf_CharData + (ULONG)gy * tf->tf_Modulo;
                UBYTE *trow = tmpl + gy * tbpr;
                WORD s;
                for (s = 0; s <= smear; s++)
                    or_bits(trow, gx + s, grow, bitoff, bitw);
            }
        }
        cx += (space ? space[idx] : tf->tf_XSize) + rp->TxSpacing + smear;
    }
    if ((style & FSF_UNDERLINED) && tf->tf_Baseline + 1 < tf->tf_YSize) {
        UBYTE *trow = tmpl + (tf->tf_Baseline + 1) * tbpr;
        WORD tx;
        for (tx = -x0; tx < width - x0; tx++)
            trow[tx >> 3] |= 0x80 >> (tx & 7);
    }

    top = rp->cp_y - tf->tf_Baseline;
    t.rp = rp;
    t.src = tmpl;
    t.srcBit = 0;
    t.srcMod = tbpr;
    t.tx = rp->cp_x + x0;
    t.ty = top;
    clip_rp_q(rp, t.tx, top, rp->cp_x + x1 - 1, top + tf->tf_YSize - 1, tmpl_cb, &t,
              (LONG)(x1 - x0) * tf->tf_YSize <= QUICK_AREA);
    if (tmpl != stackbuf)
        FreeVec(tmpl);
    text_buf_put(pool);
    rp->cp_x += width;
    return 1;
}

/* ---- bitmap blits --------------------------------------------------- */

/* BltBitMap(src a0, xSrc d0, ySrc d1, dst a1, xDest d2, yDest d3,
 *           xSize d4, ySize d5, minterm d6, mask d7, tempA a2) -> planes */
LONG h_BltBitMap(struct Regs *r)
{
    TRACE("BltBitMap");
    struct BitMap *sb = (struct BitMap *)r->a[0], *db = (struct BitMap *)r->a[1];
    struct Surf s, d;

    /* ours on either side - or planar out of the blitter's reach on either
     * side: then the CPU does it, planes to planes */
    if (!sb || !db || (!pbm_get(sb) && !pbm_get(db) && !bm_foreign(sb) && !bm_foreign(db)))
        return 0;
    surf_of(sb, &s);
    surf_of(db, &d);
    if (dbgOn) dbg("BltBitMap: src %s bpp %lu depth %lu -> dst %s bpp %lu depth %lu, %ldx%ld mt %02lx mask %02lx\n",
        s.pix ? "chunky" : "planar", (ULONG)s.bpp, (ULONG)sb->Depth,
        d.pix ? "chunky" : "planar", (ULONG)d.bpp, (ULONG)db->Depth,
        (LONG)RW(4), (LONG)RW(5), (ULONG)(UBYTE)r->d[6], (ULONG)(UBYTE)r->d[7]);
    if (!s.pix || !d.pix)
        blit_settle();                       /* a planar side: let the blitter finish with it */
    ObtainSemaphore(&lock);
    prismActivity++;
    /* again under the lock: VRAM paging may have moved either bitmap
     * while this task waited (a screen coming to the front) */
    surf_of(sb, &s);
    surf_of(db, &d);
    SW_CLEAR(s.p, RW(0), RW(1), RW(0) + RW(4) - 1, RW(1) + RW(5) - 1);
    SW_CLEAR(d.p, RW(2), RW(3), RW(2) + RW(4) - 1, RW(3) + RW(5) - 1);
    if (d.p) d.p->modified = prismActivity;
    blit(&s, RW(0), RW(1), &d, RW(2), RW(3), RW(4), RW(5), (UBYTE)r->d[6], (UBYTE)r->d[7]);
    ReleaseSemaphore(&lock);
    r->d[0] = 8;
    return 1;
}

/* BitMapScale(bitScaleArgs a0): a rectangle of one bitmap into another at
 * a different size (nearest pixel, as the ROM does it). The ROM works on
 * the planes with the blitter, which for a Prism bitmap are the dummy
 * plane: Lupe, a screen magnifier, copied a piece of the screen into a
 * friend bitmap, scaled that up five times and showed black. */
LONG h_BitMapScale(struct Regs *r)
{
    TRACE("BitMapScale");
    struct BitScaleArgs *a = (struct BitScaleArgs *)r->a[0];
    struct BitMap *sb, *db;
    struct Surf s, d;
    ULONG xs, ys, xd, yd;
    LONG sw, sh, dw, dh, dx0, dy0, x, y;
    static UWORD col[MAXW];

    if (!a || !(sb = a->bsa_SrcBitMap) || !(db = a->bsa_DestBitMap) ||
        (!pbm_get(sb) && !pbm_get(db) && !bm_foreign(sb) && !bm_foreign(db)))
        return 0;
    xs = a->bsa_XSrcFactor; ys = a->bsa_YSrcFactor;
    xd = a->bsa_XDestFactor; yd = a->bsa_YDestFactor;
    if (!xs || !ys || !xd || !yd)
        return 0;
    /* the sizes it reports back are ScalerDiv()'s: rounded to nearest */
    dw = (a->bsa_SrcWidth * xd + xs / 2) / xs;
    dh = (a->bsa_SrcHeight * yd + ys / 2) / ys;
    a->bsa_DestWidth = dw;
    a->bsa_DestHeight = dh;
    surf_of(sb, &s);
    surf_of(db, &d);
    if (dbgOn) dbg("BitMapScale: %ux%u at %u,%u (%s bpp %lu) -> %ldx%ld at %u,%u (%s bpp %lu)\n",
        a->bsa_SrcWidth, a->bsa_SrcHeight, a->bsa_SrcX, a->bsa_SrcY,
        s.pix ? "chunky" : "planar", (ULONG)s.bpp, dw, dh, a->bsa_DestX, a->bsa_DestY,
        d.pix ? "chunky" : "planar", (ULONG)d.bpp);
    sw = a->bsa_SrcWidth; sh = a->bsa_SrcHeight;
    if (a->bsa_SrcX + sw > s.w) sw = s.w - a->bsa_SrcX;
    if (a->bsa_SrcY + sh > s.h) sh = s.h - a->bsa_SrcY;
    if (sw > MAXW) sw = MAXW;
    dx0 = a->bsa_DestX; dy0 = a->bsa_DestY;
    if (dx0 + dw > d.w) dw = d.w - dx0;
    if (dy0 + dh > d.h) dh = d.h - dy0;
    if (dw > MAXW) dw = MAXW;
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0)
        return 1;
    if (!s.pix || !d.pix)
        blit_settle();                       /* a planar side: let the blitter finish with it */
    ObtainSemaphore(&lock);
    prismActivity++;
    surf_of(sb, &s);                         /* again under the lock (VRAM paging) */
    surf_of(db, &d);
    SW_CLEAR(s.p, a->bsa_SrcX, a->bsa_SrcY, a->bsa_SrcX + sw - 1, a->bsa_SrcY + sh - 1);
    SW_CLEAR(d.p, dx0, dy0, dx0 + dw - 1, dy0 + dh - 1);
    if (d.p) d.p->modified = prismActivity;
    /* which source column each destination column shows (col[] is shared:
     * filled under the lock) */
    for (x = 0; x < dw; x++) {
        ULONG c = (ULONG)x * xs / xd;
        col[x] = c < (ULONG)sw ? c : sw - 1;
    }
    if (s.p && s.p->inVram && board.waitBlit)
        board.waitBlit(&board);
    if (d.p && d.p->inVram && board.waitBlit)
        board.waitBlit(&board);
    for (y = 0; y < dh; y++) {
        ULONG sy = (ULONG)y * ys / yd;
        if (sy >= (ULONG)sh) sy = sh - 1;
        sy += a->bsa_SrcY;
        if (s.pix && d.pix && s.bpp == d.bpp && s.fmt == d.fmt) {
            /* the same chunky format: pixels as they are */
            const UBYTE *sp = s.pix + sy * s.bpr + (ULONG)a->bsa_SrcX * s.bpp;
            UBYTE *dp = d.pix + (ULONG)(dy0 + y) * d.bpr + (ULONG)dx0 * d.bpp;
            if (s.bpp == 1) {
                for (x = 0; x < dw; x++) dp[x] = sp[col[x]];
            } else if (s.bpp == 2) {
                for (x = 0; x < dw; x++) ((UWORD *)dp)[x] = ((const UWORD *)sp)[col[x]];
            } else if (s.bpp == 4) {
                for (x = 0; x < dw; x++) ((ULONG *)dp)[x] = ((const ULONG *)sp)[col[x]];
            } else {
                for (x = 0; x < dw; x++, dp += 3) {
                    const UBYTE *q = sp + (ULONG)col[x] * 3;
                    dp[0] = q[0]; dp[1] = q[1]; dp[2] = q[2];
                }
            }
        } else if (d.bpp >= 2) {
            /* into direct colour: through 0x00RRGGBB */
            UBYTE *dp = d.pix + (ULONG)(dy0 + y) * d.bpr + (ULONG)dx0 * d.bpp;
            surf_rgb(&s, a->bsa_SrcX, sy, sw, rgbRow, s.rgbTab ? s.rgbTab : d.rgbTab);
            for (x = 0; x < dw; x++, dp += d.bpp)
                pf_put(d.fmt, rgbRow[col[x]], dp);
        } else {
            /* pens (planar or chunky) */
            surf_read(&s, a->bsa_SrcX, sy, sw, rowA);
            for (x = 0; x < dw; x++)
                rowB[x] = rowA[col[x]];
            surf_write(&d, dx0, dy0 + y, dw, rowB, 0xff);
        }
    }
    ReleaseSemaphore(&lock);
    return 1;
}

struct BlitCtx {
    struct Surf src;
    struct BitMap *srcbm;        /* read src again under the lock (NULL: a buffer) */
    WORD sx, sy;                 /* source (0,0) of the rp destination ... */
    WORD dx, dy;                 /* ... which is at rp (dx, dy)            */
    UBYTE mt, mask;
    const UBYTE *amask;          /* BltMaskBitMapRastPort mask plane      */
};

static void blit_cb(struct PBitMap *p, WORD bx0, WORD by0, WORD bx1, WORD by1,
                    WORD ox, WORD oy, void *ctx)
{
    struct BlitCtx *b = ctx;
    struct Surf d;
    WORD sx = b->sx + (bx0 - ox - b->dx), sy = b->sy + (by0 - oy - b->dy);
    WORD w = bx1 - bx0 + 1, h = by1 - by0 + 1, y, x;
    ULONG mbpr;

    surf_of(p->bm, &d);
    /* the lock is held now: the source may have moved (VRAM paging)
     * since blit_rp looked, and if it is the shown bitmap the software
     * pointer must not be copied along */
    if (b->srcbm && b->src.p && b->src.pix != b->src.p->pix)
        surf_of(b->srcbm, &b->src);
    SW_CLEAR(b->src.p, sx, sy, sx + w - 1, sy + h - 1);
    if (!b->amask) {
        blit(&b->src, sx, sy, &d, bx0, by0, w, h, b->mt, b->mask);
        return;
    }
    /* Masked: the mask plane has one plane's worth of rows - the source's
     * plane stride if planar, the planar row size of its width if chunky.
     * Where the mask is set the minterm applies as in BltBitMap; elsewhere
     * the destination stays. The documented "copy through mask" minterm
     * (ABC|ABNC|ANBC) = 0xE0 means copy. */
    mbpr = b->src.pix ? (((ULONG)b->src.w + 15) >> 4) << 1 : b->src.bpr;
    if (w > MAXW) w = MAXW;
    for (y = 0; y < h; y++) {
        const UBYTE *mrow = b->amask + (ULONG)(sy + y) * mbpr;
        BOOL copy = (b->mt == 0xe0 || (b->mt & 0xf0) == 0xc0);
        if (sy + y < 0 || sy + y >= b->src.h || by0 + y >= d.h)
            continue;
        for (x = 0; x < w; x++) {
            WORD xx = sx + x;
            rowC[x] = (xx >= 0 && xx < b->src.w && (mrow[xx >> 3] & (0x80 >> (xx & 7))));
        }
        if (d.bpp == 2) {
            UWORD *drow = (UWORD *)(d.pix + (ULONG)(by0 + y) * d.bpr) + bx0;
            surf_read16(&b->src, sx, sy + y, w, wA, d.penTab, d.fmt);
            if (!copy) {
                CopyMem(drow, wB, w * 2);
                combine16(wA, wB, wA, w, b->mt);
            }
            for (x = 0; x < w; x++)
                if (rowC[x])
                    drow[x] = wA[x];
        } else if (d.bpp >= 3) {
            /* true colour: masked copy (other minterms act as copy) */
            UBYTE *dp = d.pix + (ULONG)(by0 + y) * d.bpr + (ULONG)bx0 * d.bpp;
            surf_rgb(&b->src, sx, sy + y, w, rgbRow, b->src.rgbTab ? b->src.rgbTab : d.rgbTab);
            for (x = 0; x < w; x++, dp += d.bpp)
                if (rowC[x])
                    pf_put(d.fmt, rgbRow[x], dp);
        } else {
            surf_read(&b->src, sx, sy + y, w, rowA);
            surf_read(&d, bx0, by0 + y, w, rowB);
            if (!copy)
                combine2(rowA, rowB, rowA, w, b->mt);
            for (x = 0; x < w; x++)
                if (!rowC[x])
                    rowA[x] = rowB[x];
            surf_write(&d, bx0, by0 + y, w, rowA, b->mask);
        }
    }
}

static LONG blit_rp(struct Regs *r, const UBYTE *amask)
{
    struct RastPort *rp = (struct RastPort *)r->a[1];
    struct BlitCtx b;
    WORD w = RW(4), h = RW(5);

    if (!rp_is_prism(rp) || !r->a[0])
        return 0;
    if (w <= 0 || h <= 0)
        return 1;
    surf_of((struct BitMap *)r->a[0], &b.src);
    if (dbgOn) dbg("blit_rp: src %s bpp %lu depth %lu %ldx%ld -> rp, mt %02lx mask %lx rpmask %02lx\n",
        b.src.pix ? "chunky" : "planar", (ULONG)b.src.bpp,
        (ULONG)((struct BitMap *)r->a[0])->Depth, (LONG)w, (LONG)h, (ULONG)(UBYTE)r->d[6],
        (ULONG)amask, (ULONG)rp->Mask);
    b.sx = RW(0); b.sy = RW(1);
    b.dx = RW(2); b.dy = RW(3);
    if (amask && dbgOn)
        dbg("  masked: src bitmap %ldx%ld BytesPerRow %lu, from %ld,%ld to %ld,%ld\n",
            (LONG)b.src.w, (LONG)b.src.h, (ULONG)((struct BitMap *)r->a[0])->BytesPerRow,
            (LONG)b.sx, (LONG)b.sy, (LONG)b.dx, (LONG)b.dy);
#ifdef PRISM_TRACE
    if (amask) {
        struct BitMap *sb = (struct BitMap *)r->a[0];
        const UBYTE *m = amask + 8 * sb->BytesPerRow;
        dbg("  planes %lx %lx %lx %lx %lx %lx %lx %lx flags %02x dm %u fg %u bg %u\n",
            (ULONG)sb->Planes[0], (ULONG)sb->Planes[1], (ULONG)sb->Planes[2], (ULONG)sb->Planes[3],
            (ULONG)sb->Planes[4], (ULONG)sb->Planes[5], (ULONG)sb->Planes[6], (ULONG)sb->Planes[7],
            sb->Flags, rp->DrawMode, rp->FgPen, rp->BgPen);
        dbg("  mask row 8: %02x %02x %02x %02x %02x %02x %02x %02x\n", m[0], m[1], m[2], m[3], m[4],
            m[5], m[6], m[7]);
        if (!b.src.pix && sb->Planes[0] && sb->Planes[0] != (PLANEPTR)-1) {
            const UBYTE *q = sb->Planes[0] + 8 * sb->BytesPerRow;
            dbg("  plane 0 row 8: %02x %02x %02x %02x %02x %02x %02x %02x\n", q[0], q[1], q[2], q[3],
                q[4], q[5], q[6], q[7]);
        }
    }
#endif
    if (!b.src.pix || amask)
        blit_settle();                       /* planar source or mask: let the blitter finish */
    b.mt = (UBYTE)r->d[6];
    b.mask = rp->Mask;
    b.amask = amask;
    b.srcbm = (struct BitMap *)r->a[0];
    clip_rp_q(rp, b.dx, b.dy, b.dx + w - 1, b.dy + h - 1, blit_cb, &b,
              b.src.pix && !amask && (LONG)w * h <= QUICK_AREA);
    return 1;
}

/* BltBitMapRastPort(src a0, xSrc d0, ySrc d1, rp a1, xDest d2, yDest d3,
 *                   xSize d4, ySize d5, minterm d6) */
LONG h_BltBitMapRastPort(struct Regs *r)
{
    TRACE("BltBitMapRastPort");
    FOREIGN(h_BltBitMapRastPort, 1);
    return blit_rp(r, NULL);
}

/* BltMaskBitMapRastPort(src a0, xSrc d0, ySrc d1, rp a1, xDest d2,
 *                       yDest d3, xSize d4, ySize d5, minterm d6, mask a2) */
LONG h_BltMaskBitMapRastPort(struct Regs *r)
{
    TRACE("BltMaskBitMapRastPort");
    FOREIGN(h_BltMaskBitMapRastPort, 1);
    return blit_rp(r, (const UBYTE *)r->a[2]);
}

/* Copy out of a RastPort's visible/backed-up pieces into a surface. */
struct GrabCtx { struct Surf *dst; WORD x0, y0; };

static void grab_cb(struct PBitMap *p, WORD bx0, WORD by0, WORD bx1, WORD by1,
                    WORD ox, WORD oy, void *ctx)
{
    struct GrabCtx *g = ctx;
    struct Surf s;
    surf_of(p->bm, &s);
    blit(&s, bx0, by0, g->dst, bx0 - ox - g->x0, by0 - oy - g->y0,
         bx1 - bx0 + 1, by1 - by0 + 1, 0xc0, 0xff);
}

/* Does an rp rectangle lie wholly in one visible ClipRect of L (or is
 * there no layer)? Then it is plain bitmap at rp + (ox, oy). Layer locked. */
static BOOL in_one_piece(struct Layer *L, WORD x0, WORD y0, WORD x1, WORD y1, WORD *ox, WORD *oy)
{
    struct ClipRect *cr;
    WORD lx, ly;

    if (!L) {
        *ox = *oy = 0;
        return TRUE;
    }
    lx = L->bounds.MinX - L->Scroll_X;
    ly = L->bounds.MinY - L->Scroll_Y;
    x0 += lx; x1 += lx; y0 += ly; y1 += ly;
    for (cr = L->ClipRect; cr; cr = cr->Next)
        if (x0 >= cr->bounds.MinX && x1 <= cr->bounds.MaxX &&
            y0 >= cr->bounds.MinY && y1 <= cr->bounds.MaxY) {
            if (cr->obscured)
                return FALSE;
            *ox = lx;
            *oy = ly;
            return TRUE;
        }
    return FALSE;
}

/* ClipBlit(srcRP a0, xSrc d0, ySrc d1, dstRP a1, xDest d2, yDest d3,
 *          xSize d4, ySize d5, minterm d6): source rectangle through the
 * source's clipping into a buffer in the source's format, then out through
 * the destination's. Within one window, with both rectangles fully
 * visible (scrolling a list or a text view), it is one direct blit - the
 * card's blitter when it is VRAM. */
LONG h_ClipBlit(struct Regs *r)
{
    TRACE("ClipBlit");
    FOREIGN(h_ClipBlit, 1);
    struct RastPort *srp = (struct RastPort *)r->a[0], *drp = (struct RastPort *)r->a[1];
    WORD sx = RW(0), sy = RW(1), dx = RW(2), dy = RW(3), w = RW(4), h = RW(5);
    struct PBitMap *sp;
    struct Surf tmp;
    struct GrabCtx g;
    struct BlitCtx b;

    if (!rp_is_prism(srp) || !rp_is_prism(drp))
        return 0;                /* planar on either side: the original */
    if (w <= 0 || h <= 0)
        return 1;
    if (srp->Layer == drp->Layer && pbm_get(srp->BitMap) == pbm_get(drp->BitMap)) {
        struct Layer *L = srp->Layer;
        WORD ox, oy, ox2, oy2;
        BOOL direct;
        if (L) LockLayerRom(L);
        direct = in_one_piece(L, sx, sy, sx + w - 1, sy + h - 1, &ox, &oy) &&
                 in_one_piece(L, dx, dy, dx + w - 1, dy + h - 1, &ox2, &oy2);
        if (direct) {
            struct Surf s;
            ObtainSemaphore(&lock);
            prismActivity++;
            surf_of(srp->BitMap, &s);        /* under the lock (VRAM paging) */
            SW_CLEAR(s.p, sx + ox, sy + oy, sx + ox + w - 1, sy + oy + h - 1);
            SW_CLEAR(s.p, dx + ox, dy + oy, dx + ox + w - 1, dy + oy + h - 1);
            if (s.p) s.p->modified = prismActivity;
            blit(&s, sx + ox, sy + oy, &s, dx + ox, dy + oy, w, h, (UBYTE)r->d[6], drp->Mask);
            ReleaseSemaphore(&lock);
        }
        if (L) UnlockLayerRom(L);
        if (direct)
            return 1;
    }
    sp = pbm_get(srp->BitMap);
    memset(&tmp, 0, sizeof(tmp));
    tmp.bpp = sp->bpp;
    tmp.penTab = sp->penTab;
    tmp.rgbTab = sp->rgbTab;
    tmp.fmt = sp->fmt;
    tmp.bpr = (ULONG)w * tmp.bpp;
    tmp.w = w;
    tmp.h = h;
    if (!(tmp.pix = AllocVec(tmp.bpr * h, MEMF_ANY | MEMF_CLEAR)))
        return 1;
    g.dst = &tmp; g.x0 = sx; g.y0 = sy;
    clip_rp(srp, sx, sy, sx + w - 1, sy + h - 1, grab_cb, &g);
    b.src = tmp;
    b.sx = 0; b.sy = 0; b.dx = dx; b.dy = dy;
    b.mt = (UBYTE)r->d[6]; b.mask = drp->Mask; b.amask = NULL; b.srcbm = NULL;
    clip_rp(drp, dx, dy, dx + w - 1, dy + h - 1, blit_cb, &b);
    FreeVec(tmp.pix);
    return 1;
}

/* ---- scrolling ------------------------------------------------------ */

struct ScrollCtx { WORD dx, dy; UBYTE bgpen; };

static void scroll_cb(struct PBitMap *p, WORD bx0, WORD by0, WORD bx1, WORD by1,
                      WORD ox, WORD oy, void *ctx)
{
    struct ScrollCtx *c = ctx;
    struct Surf s;
    WORD w = bx1 - bx0 + 1, h = by1 - by0 + 1;
    WORD adx = c->dx < 0 ? -c->dx : c->dx, ady = c->dy < 0 ? -c->dy : c->dy;

    surf_of(p->bm, &s);
    if (adx < w && ady < h) {
        /* content moves by (-dx, -dy) inside the piece */
        WORD sx = bx0 + (c->dx > 0 ? c->dx : 0), sy = by0 + (c->dy > 0 ? c->dy : 0);
        WORD tx = bx0 + (c->dx < 0 ? -c->dx : 0), ty = by0 + (c->dy < 0 ? -c->dy : 0);
        blit(&s, sx, sy, &s, tx, ty, w - adx, h - ady, 0xc0, 0xff);
    }
    /* clear what scrolled in */
    if (c->dy > 0) fill(p, bx0, by1 - (ady < h ? ady : h) + 1, bx1, by1, c->bgpen, 0xff, FALSE);
    if (c->dy < 0) fill(p, bx0, by0, bx1, by0 + (ady < h ? ady : h) - 1, c->bgpen, 0xff, FALSE);
    if (c->dx > 0) fill(p, bx1 - (adx < w ? adx : w) + 1, by0, bx1, by1, c->bgpen, 0xff, FALSE);
    if (c->dx < 0) fill(p, bx0, by0, bx0 + (adx < w ? adx : w) - 1, by1, c->bgpen, 0xff, FALSE);
}

static void bgfill_cb(struct PBitMap *p, WORD bx0, WORD by0, WORD bx1, WORD by1,
                      WORD ox, WORD oy, void *ctx)
{
    fill(p, bx0, by0, bx1, by1, ((struct ScrollCtx *)ctx)->bgpen, 0xff, FALSE);
}

/* A rectangle split over several ClipRects (a window partly covered):
 * content has to move from one piece into another, and out of or into
 * backing store. The whole rectangle is read through the clipping into a
 * buffer, written back shifted, and the strip that scrolled in cleared.
 * Simple-refresh windows get no damage list for the parts that were
 * hidden, as before. */
static BOOL scroll_pieces(struct RastPort *rp, WORD x0, WORD y0, WORD x1, WORD y1,
                          struct ScrollCtx *c)
{
    WORD w = x1 - x0 + 1, h = y1 - y0 + 1;
    WORD adx = c->dx < 0 ? -c->dx : c->dx, ady = c->dy < 0 ? -c->dy : c->dy;
    struct PBitMap *p = pbm_get(rp->BitMap);
    struct Surf tmp;
    struct GrabCtx g;
    struct BlitCtx b;

    if (adx < w && ady < h) {
        memset(&tmp, 0, sizeof(tmp));
        tmp.bpp = p->bpp;
        tmp.penTab = p->penTab;
        tmp.rgbTab = p->rgbTab;
        tmp.fmt = p->fmt;
        tmp.bpr = (ULONG)w * tmp.bpp;
        tmp.w = w;
        tmp.h = h;
        if (!(tmp.pix = AllocVec(tmp.bpr * h, MEMF_ANY | MEMF_CLEAR)))
            return FALSE;
        g.dst = &tmp; g.x0 = x0; g.y0 = y0;
        clip_rp(rp, x0, y0, x1, y1, grab_cb, &g);
        b.src = tmp;
        b.sx = c->dx > 0 ? c->dx : 0;
        b.sy = c->dy > 0 ? c->dy : 0;
        b.dx = x0 + (c->dx < 0 ? adx : 0);
        b.dy = y0 + (c->dy < 0 ? ady : 0);
        b.mt = 0xc0; b.mask = 0xff; b.amask = NULL; b.srcbm = NULL;
        clip_rp(rp, b.dx, b.dy, b.dx + w - adx - 1, b.dy + h - ady - 1, blit_cb, &b);
        FreeVec(tmp.pix);
    }
    if (ady > h) ady = h;
    if (adx > w) adx = w;
    if (c->dy > 0) clip_rp(rp, x0, y1 - ady + 1, x1, y1, bgfill_cb, c);
    if (c->dy < 0) clip_rp(rp, x0, y0, x1, y0 + ady - 1, bgfill_cb, c);
    if (c->dx > 0) clip_rp(rp, x1 - adx + 1, y0, x1, y1, bgfill_cb, c);
    if (c->dx < 0) clip_rp(rp, x0, y0, x0 + adx - 1, y1, bgfill_cb, c);
    return TRUE;
}

static LONG do_scroll(struct Regs *r, BOOL bf)
{
    struct RastPort *rp = (struct RastPort *)r->a[1];
    struct Layer *L = rp->Layer;
    struct ScrollCtx c;
    WORD x0 = RW(2), y0 = RW(3), x1 = RW(4), y1 = RW(5), ox, oy;

    if (!rp_is_prism(rp))
        return 0;
    c.dx = RW(0);
    c.dy = RW(1);
    c.bgpen = bf ? 0 : rp->BgPen;
    /* (a layer that is one unobscured ClipRect is always one piece) */
    if (L && !L->SuperBitMap && x0 <= x1 && y0 <= y1 && (c.dx || c.dy) &&
        !(L->ClipRect && !L->ClipRect->Next && !L->ClipRect->obscured)) {
        BOOL one;
        LockLayerRom(L);
        /* the part inside the layer: outside it there is nothing to move */
        if (x0 < L->Scroll_X) x0 = L->Scroll_X;
        if (y0 < L->Scroll_Y) y0 = L->Scroll_Y;
        if (x1 > L->Scroll_X + L->bounds.MaxX - L->bounds.MinX)
            x1 = L->Scroll_X + L->bounds.MaxX - L->bounds.MinX;
        if (y1 > L->Scroll_Y + L->bounds.MaxY - L->bounds.MinY)
            y1 = L->Scroll_Y + L->bounds.MaxY - L->bounds.MinY;
        if (x0 > x1 || y0 > y1) {
            UnlockLayerRom(L);
            return 1;
        }
        one = in_one_piece(L, x0, y0, x1, y1, &ox, &oy);
        /* the layer stays locked: clip_rp locks it again (nests) */
        if (!one && scroll_pieces(rp, x0, y0, x1, y1, &c)) {
            UnlockLayerRom(L);
            return 1;
        }
        UnlockLayerRom(L);
    }
    clip_rp(rp, x0, y0, x1, y1, scroll_cb, &c);
    return 1;
}

/* ScrollRaster(rp a1, dx d0, dy d1, xMin d2, yMin d3, xMax d4, yMax d5) */
LONG h_ScrollRaster(struct Regs *r)
{
    FOREIGN(h_ScrollRaster, 1);
    return do_scroll(r, FALSE);
}

LONG h_ScrollRasterBF(struct Regs *r)
{
    FOREIGN(h_ScrollRasterBF, 1);
    return do_scroll(r, TRUE);
}

/* ---- chunky pen arrays ---------------------------------------------- */

struct ArrCtx { UBYTE *buf; LONG bpr; WORD x0, y0; LONG count; };

static void read_cb(struct PBitMap *p, WORD bx0, WORD by0, WORD bx1, WORD by1,
                    WORD ox, WORD oy, void *ctx)
{
    struct ArrCtx *c = ctx;
    struct Surf s;
    WORD y, w = bx1 - bx0 + 1;
    surf_of(p->bm, &s);
    for (y = by0; y <= by1; y++) {
        surf_read(&s, bx0, y, w, c->buf + (LONG)(y - oy - c->y0) * c->bpr + (bx0 - ox - c->x0));
        c->count += w;
    }
}

static void write_cb(struct PBitMap *p, WORD bx0, WORD by0, WORD bx1, WORD by1,
                     WORD ox, WORD oy, void *ctx)
{
    struct ArrCtx *c = ctx;
    struct Surf s;
    WORD y, w = bx1 - bx0 + 1;
    surf_of(p->bm, &s);
    for (y = by0; y <= by1; y++) {
        surf_write(&s, bx0, y, w, c->buf + (LONG)(y - oy - c->y0) * c->bpr + (bx0 - ox - c->x0),
                   0xff);
        c->count += w;
    }
}

static LONG rw_array(struct RastPort *rp, WORD x0, WORD y0, WORD x1, WORD y1,
                     UBYTE *array, LONG bpr, BOOL write)
{
    struct ArrCtx c;
    c.buf = array; c.bpr = bpr; c.x0 = x0; c.y0 = y0; c.count = 0;
    clip_rp(rp, x0, y0, x1, y1, write ? write_cb : read_cb, &c);
    return c.count;
}

/* WriteChunkyPixels(rp a0, xstart d0, ystart d1, xstop d2, ystop d3,
 *                   array a2, bytesperrow d4) */
LONG h_WriteChunkyPixels(struct Regs *r)
{
    TRACE("WriteChunkyPixels");
    FOREIGN(h_WriteChunkyPixels, 0);
    struct RastPort *rp = (struct RastPort *)r->a[0];
    if (!rp_is_prism(rp))
        return 0;
    rw_array(rp, (UWORD)r->d[0], (UWORD)r->d[1], (UWORD)r->d[2], (UWORD)r->d[3],
             (UBYTE *)r->a[2], r->d[4], TRUE);
    return 1;
}

/* Write/ReadPixelArray8(rp a0, xstart d0, ystart d1, xstop d2, ystop d3,
 *                       array a2, temprp a1) -> pixel count */
LONG h_WritePixelArray8(struct Regs *r)
{
    TRACE("WritePixelArray8");
    FOREIGN(h_WritePixelArray8, 0);
    struct RastPort *rp = (struct RastPort *)r->a[0];
    WORD x0 = (UWORD)r->d[0], x1 = (UWORD)r->d[2];
    if (!rp_is_prism(rp))
        return 0;
    r->d[0] = rw_array(rp, x0, (UWORD)r->d[1], x1, (UWORD)r->d[3], (UBYTE *)r->a[2],
                       ((x1 - x0 + 1 + 15) >> 4) << 4, TRUE);
    return 1;
}

LONG h_ReadPixelArray8(struct Regs *r)
{
    TRACE("ReadPixelArray8");
    FOREIGN(h_ReadPixelArray8, 0);
    struct RastPort *rp = (struct RastPort *)r->a[0];
    WORD x0 = (UWORD)r->d[0], x1 = (UWORD)r->d[2];
    if (!rp_is_prism(rp))
        return 0;
    r->d[0] = rw_array(rp, x0, (UWORD)r->d[1], x1, (UWORD)r->d[3], (UBYTE *)r->a[2],
                       ((x1 - x0 + 1 + 15) >> 4) << 4, FALSE);
    return 1;
}

/* Write/ReadPixelLine8(rp a0, xstart d0, ystart d1, width d2, array a2,
 *                      temprp a1) -> pixel count */
LONG h_WritePixelLine8(struct Regs *r)
{
    TRACE("WritePixelLine8");
    FOREIGN(h_WritePixelLine8, 0);
    struct RastPort *rp = (struct RastPort *)r->a[0];
    WORD x0 = (UWORD)r->d[0], y = (UWORD)r->d[1], w = (UWORD)r->d[2];
    if (!rp_is_prism(rp))
        return 0;
    r->d[0] = rw_array(rp, x0, y, x0 + w - 1, y, (UBYTE *)r->a[2], w, TRUE);
    return 1;
}

LONG h_ReadPixelLine8(struct Regs *r)
{
    TRACE("ReadPixelLine8");
    FOREIGN(h_ReadPixelLine8, 0);
    struct RastPort *rp = (struct RastPort *)r->a[0];
    WORD x0 = (UWORD)r->d[0], y = (UWORD)r->d[1], w = (UWORD)r->d[2];
    if (!rp_is_prism(rp))
        return 0;
    r->d[0] = rw_array(rp, x0, y, x0 + w - 1, y, (UBYTE *)r->a[2], w, FALSE);
    return 1;
}
