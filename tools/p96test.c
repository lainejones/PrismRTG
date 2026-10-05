/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Laine Jones */
/* p96test: exercise Prism's Picasso96API.library - every function, with
 * the results read back and checked. Prints one line per check and the
 * number that failed.
 *
 *   p96test [DEPTH=8|16|24|32] [FORMAT=n] [SECS=n]
 *   FORMAT: RGBFB_ number of the screen format wanted (9 = B8G8R8A8),
 *   SECS: keep the picture up
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/libraries.h>
#include <dos/dos.h>
#include <dos/rdargs.h>
#include <graphics/gfx.h>
#include <graphics/rastport.h>
#include <intuition/screens.h>
#include <utility/tagitem.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <stdio.h>
#include <string.h>
#include "../src/p96api.h"

static struct Library *P96Base;

/* ---- the library's calls (registers as its function table says) ----- */

#define P96CALL(off) "jsr -" #off "(%%a6)"
#define RA(t, n, r, v) register t n __asm(r) = (v)

static struct BitMap *p96AllocBitMap(ULONG w, ULONG h, ULONG d, ULONG fl, struct BitMap *fr, ULONG fmt)
{
    RA(ULONG, d0, "d0", w); RA(ULONG, d1, "d1", h); RA(ULONG, d2, "d2", d); RA(ULONG, d3, "d3", fl);
    RA(struct BitMap *, a0, "a0", fr); RA(ULONG, d7, "d7", fmt); RA(struct Library *, a6, "a6", P96Base);
    __asm volatile (P96CALL(30) : "+r"(d0), "+r"(d1), "+r"(a0) : "r"(d2), "r"(d3), "r"(d7), "r"(a6) : "a1", "cc", "memory");
    return (struct BitMap *)d0;
}
static void p96FreeBitMap(struct BitMap *bm)
{
    RA(struct BitMap *, a0, "a0", bm); RA(struct Library *, a6, "a6", P96Base);
    __asm volatile (P96CALL(36) : "+r"(a0) : "r"(a6) : "d0", "d1", "a1", "cc", "memory");
}
static ULONG p96GetBitMapAttr(struct BitMap *bm, ULONG attr)
{
    RA(ULONG, d0, "d0", attr); RA(struct BitMap *, a0, "a0", bm); RA(struct Library *, a6, "a6", P96Base);
    __asm volatile (P96CALL(42) : "+r"(d0), "+r"(a0) : "r"(a6) : "d1", "a1", "cc", "memory");
    return d0;
}
static LONG p96LockBitMap(struct BitMap *bm, UBYTE *buf, ULONG size)
{
    RA(ULONG, d0, "d0", size); RA(struct BitMap *, a0, "a0", bm); RA(UBYTE *, a1, "a1", buf);
    RA(struct Library *, a6, "a6", P96Base);
    __asm volatile (P96CALL(48) : "+r"(d0), "+r"(a0), "+r"(a1) : "r"(a6) : "d1", "cc", "memory");
    return d0;
}
static void p96UnlockBitMap(struct BitMap *bm, LONG lock)
{
    RA(ULONG, d0, "d0", lock); RA(struct BitMap *, a0, "a0", bm); RA(struct Library *, a6, "a6", P96Base);
    __asm volatile (P96CALL(54) : "+r"(d0), "+r"(a0) : "r"(a6) : "d1", "a1", "cc", "memory");
}
static ULONG tagcall(struct TagItem *tags, ULONG d0v, int which)
{
    RA(ULONG, d0, "d0", d0v); RA(struct TagItem *, a0, "a0", tags); RA(struct Library *, a6, "a6", P96Base);
    switch (which) {
    case 60:  __asm volatile (P96CALL(60)  : "+r"(d0), "+r"(a0) : "r"(a6) : "d1", "a1", "cc", "memory"); break;
    case 72:  __asm volatile (P96CALL(72)  : "+r"(d0), "+r"(a0) : "r"(a6) : "d1", "a1", "cc", "memory"); break;
    case 78:  __asm volatile (P96CALL(78)  : "+r"(d0), "+r"(a0) : "r"(a6) : "d1", "a1", "cc", "memory"); break;
    case 90:  __asm volatile (P96CALL(90)  : "+r"(d0), "+r"(a0) : "r"(a6) : "d1", "a1", "cc", "memory"); break;
    case 96:  __asm volatile (P96CALL(96)  : "+r"(d0), "+r"(a0) : "r"(a6) : "d1", "a1", "cc", "memory"); break;
    case 144: __asm volatile (P96CALL(144) : "+r"(d0), "+r"(a0) : "r"(a6) : "d1", "a1", "cc", "memory"); break;
    case 180: __asm volatile (P96CALL(180) : "+r"(d0), "+r"(a0) : "r"(a6) : "d1", "a1", "cc", "memory"); break;
    case 186: __asm volatile (P96CALL(186) : "+r"(d0), "+r"(a0) : "r"(a6) : "d1", "a1", "cc", "memory"); break;
    }
    return d0;
}
static ULONG p96GetModeIDAttr(ULONG id, ULONG attr)
{
    RA(ULONG, d0, "d0", id); RA(ULONG, d1, "d1", attr); RA(struct Library *, a6, "a6", P96Base);
    __asm volatile (P96CALL(84) : "+r"(d0), "+r"(d1) : "r"(a6) : "a0", "a1", "cc", "memory");
    return d0;
}
/* the four array calls share one register layout */
static void arr(int which, APTR info, UWORD x0, UWORD y0, struct RastPort *rp, UWORD x1, UWORD y1, UWORD w, UWORD h)
{
    RA(ULONG, d0, "d0", x0); RA(ULONG, d1, "d1", y0); RA(ULONG, d2, "d2", x1); RA(ULONG, d3, "d3", y1);
    RA(ULONG, d4, "d4", w); RA(ULONG, d5, "d5", h); RA(APTR, a0, "a0", info); RA(struct RastPort *, a1, "a1", rp);
    RA(struct Library *, a6, "a6", P96Base);
    switch (which) {
    case 102: __asm volatile (P96CALL(102) : "+r"(d0), "+r"(d1), "+r"(a0), "+r"(a1) : "r"(d2), "r"(d3), "r"(d4), "r"(d5), "r"(a6) : "cc", "memory"); break;
    case 108: __asm volatile (P96CALL(108) : "+r"(d0), "+r"(d1), "+r"(a0), "+r"(a1) : "r"(d2), "r"(d3), "r"(d4), "r"(d5), "r"(a6) : "cc", "memory"); break;
    case 132: __asm volatile (P96CALL(132) : "+r"(d0), "+r"(d1), "+r"(a0), "+r"(a1) : "r"(d2), "r"(d3), "r"(d4), "r"(d5), "r"(a6) : "cc", "memory"); break;
    case 138: __asm volatile (P96CALL(138) : "+r"(d0), "+r"(d1), "+r"(a0), "+r"(a1) : "r"(d2), "r"(d3), "r"(d4), "r"(d5), "r"(a6) : "cc", "memory"); break;
    }
}
static ULONG p96WritePixel(struct RastPort *rp, UWORD x, UWORD y, ULONG c)
{
    RA(ULONG, d0, "d0", x); RA(ULONG, d1, "d1", y); RA(ULONG, d2, "d2", c); RA(struct RastPort *, a1, "a1", rp);
    RA(struct Library *, a6, "a6", P96Base);
    __asm volatile (P96CALL(114) : "+r"(d0), "+r"(d1), "+r"(a1) : "r"(d2), "r"(a6) : "a0", "cc", "memory");
    return d0;
}
static ULONG p96ReadPixel(struct RastPort *rp, UWORD x, UWORD y)
{
    RA(ULONG, d0, "d0", x); RA(ULONG, d1, "d1", y); RA(struct RastPort *, a1, "a1", rp);
    RA(struct Library *, a6, "a6", P96Base);
    __asm volatile (P96CALL(120) : "+r"(d0), "+r"(d1), "+r"(a1) : "r"(a6) : "a0", "cc", "memory");
    return d0;
}
static void p96RectFill(struct RastPort *rp, UWORD x0, UWORD y0, UWORD x1, UWORD y1, ULONG c)
{
    RA(ULONG, d0, "d0", x0); RA(ULONG, d1, "d1", y0); RA(ULONG, d2, "d2", x1); RA(ULONG, d3, "d3", y1);
    RA(ULONG, d4, "d4", c); RA(struct RastPort *, a1, "a1", rp); RA(struct Library *, a6, "a6", P96Base);
    __asm volatile (P96CALL(126) : "+r"(d0), "+r"(d1), "+r"(a1) : "r"(d2), "r"(d3), "r"(d4), "r"(a6) : "a0", "cc", "memory");
}
static ULONG p96EncodeColor(ULONG fmt, ULONG c)
{
    RA(ULONG, d0, "d0", fmt); RA(ULONG, d1, "d1", c); RA(struct Library *, a6, "a6", P96Base);
    __asm volatile (P96CALL(192) : "+r"(d0), "+r"(d1) : "r"(a6) : "a0", "a1", "cc", "memory");
    return d0;
}

/* ---- checks --------------------------------------------------------- */

static int failed, checks;

static void check(const char *what, int ok)
{
    checks++;
    if (!ok)
        failed++;
    printf("  %-44s %s\n", what, ok ? "ok" : "FAILED");
}

/* two colours equal to within what a pixel format keeps */
static int near(ULONG a, ULONG b, int tol)
{
    int dr = (int)((a >> 16) & 255) - (int)((b >> 16) & 255);
    int dg = (int)((a >> 8) & 255) - (int)((b >> 8) & 255);
    int db = (int)(a & 255) - (int)(b & 255);
    return dr >= -tol && dr <= tol && dg >= -tol && dg <= tol && db >= -tol && db <= tol;
}

static ULONG grad(int x, int y)
{
    return ((ULONG)(x * 8 & 255) << 16) | ((ULONG)(y * 8 & 255) << 8) | ((x + y) * 4 & 255);
}

static UBYTE buf[32 * 32 * 4], back[32 * 32 * 4];
static UBYTE pr[32 * 32], pg[32 * 32], pb[32 * 32];

int main(void)
{
    LONG args[3] = { 0, 0, 0 };
    struct RDArgs *rda;
    struct Screen *s = NULL;
    struct RastPort *rp;
    struct BitMap *bm;
    struct P96RenderInfo ri;
    struct P96TrueColorInfo tci;
    struct List *ml;
    struct Node *nd;
    ULONG depth = 16, id, n, v, boards = 99, total = 0, freeMem = 0, formats = 0, err = 0;
    char *bname = NULL;
    int x, y, bad, tol, f;

    if (!(rda = ReadArgs((STRPTR)"DEPTH/K/N,SECS/K/N,FORMAT/K/N", args, NULL)))
        return 20;
    if (args[0]) depth = *(LONG *)args[0];
    tol = depth >= 24 ? 0 : 8;

    if (!(P96Base = OpenLibrary((STRPTR)"Picasso96API.library", 2))) {
        printf("p96test: no Picasso96API.library\n");
        FreeArgs(rda);
        return 10;
    }
    printf("p96test: %s", (char *)P96Base->lib_IdString);
    if (((char *)P96Base->lib_IdString)[strlen((char *)P96Base->lib_IdString) - 1] != '\n')
        printf("\n");

    {
        struct TagItem t1[] = { { P96RD_NumberOfBoards, (ULONG)&boards }, { TAG_DONE, 0 } };
        struct TagItem t2[] = { { P96BD_BoardName, (ULONG)&bname }, { P96BD_TotalMemory, (ULONG)&total },
                                { P96BD_FreeMemory, (ULONG)&freeMem }, { P96BD_RGBFormats, (ULONG)&formats },
                                { TAG_DONE, 0 } };
        tagcall(t1, 0, 180);
        check("p96GetRTGDataTagList: one board", boards == 1);
        n = tagcall(t2, 0, 186);
        printf("    board '%s', %lu KB, %lu KB free, formats %04lx\n", bname ? bname : "?",
               (unsigned long)(total >> 10), (unsigned long)(freeMem >> 10), (unsigned long)formats);
        check("p96GetBoardDataTagList", n == 4 && bname && total && (formats & RGBFF(RGBFB_CLUT)));
    }
    {
        struct TagItem bt[] = { { P96BIDTAG_NominalWidth, 640 }, { P96BIDTAG_NominalHeight, 480 },
                                { P96BIDTAG_Depth, depth }, { TAG_DONE, 0 } };
        id = tagcall(bt, 0, 60);
        printf("    best mode for 640x480 depth %lu: $%08lx, %lux%lu, %lu bits, format %lu\n",
               (unsigned long)depth, (unsigned long)id, (unsigned long)p96GetModeIDAttr(id, P96IDA_WIDTH),
               (unsigned long)p96GetModeIDAttr(id, P96IDA_HEIGHT),
               (unsigned long)p96GetModeIDAttr(id, P96IDA_BITSPERPIXEL),
               (unsigned long)p96GetModeIDAttr(id, P96IDA_RGBFORMAT));
        check("p96BestModeIDTagList + p96GetModeIDAttr",
              id != INVALID_ID && p96GetModeIDAttr(id, P96IDA_WIDTH) >= 640 &&
              p96GetModeIDAttr(id, P96IDA_ISP96));
    }
    {
        struct TagItem mt[] = { { P96MA_MinDepth, 8 }, { TAG_DONE, 0 } };
        n = 0;
        if ((ml = (struct List *)tagcall(mt, 0, 72))) {
            for (nd = ml->lh_Head; nd->ln_Succ; nd = nd->ln_Succ)
                n++;
            tagcall((struct TagItem *)ml, 0, 78);
        }
        printf("    %lu modes in the list\n", (unsigned long)n);
        check("p96AllocModeListTagList / p96FreeModeList", n > 0);
    }
    {
        struct TagItem pt[] = { { P96PIP_ErrorCode, (ULONG)&err }, { TAG_DONE, 0 } };
        check("p96PIP_OpenTagList says not available", !tagcall(pt, 0, 144) && err == PIPERR_NOTAVAILABLE);
    }
    check("p96EncodeColor R5G6B5", p96EncodeColor(RGBFB_R5G6B5, 0xff0000) == 0xf800 &&
                                   p96EncodeColor(RGBFB_R5G6B5PC, 0xff0000) == 0x00f8 &&
                                   p96EncodeColor(RGBFB_A8R8G8B8, 0x123456) == 0x00123456);

    {
        struct TagItem st[] = { { P96SA_Width, 640 }, { P96SA_Height, 480 }, { P96SA_Depth, depth },
                                { P96SA_Title, (ULONG)"p96test" }, { P96SA_Quiet, FALSE },
                                /* FORMAT=n: ask for a pixel format (RGBFB_ number) */
                                { args[2] ? P96SA_RGBFormat : TAG_IGNORE, args[2] ? *(LONG *)args[2] : 0 },
                                { TAG_DONE, 0 } };
        s = (struct Screen *)tagcall(st, 0, 90);
    }
    check("p96OpenScreenTagList", s != NULL);
    if (!s)
        goto out;
    rp = &s->RastPort;
    if (depth <= 8) {
        /* a palette the true-colour calls can find their colours in */
        int i;
        for (i = 0; i < 256; i++)
            SetRGB32(&s->ViewPort, i, (ULONG)((i & 0xe0)) << 24, (ULONG)((i & 0x1c) << 3) << 24,
                     (ULONG)((i & 3) << 6) << 24);
        tol = 64;                /* 3-3-2 palette: blue has four levels */
    }
    printf("    screen bitmap: %lu bits, format %lu, %lu bytes/row, on board %lu\n",
           (unsigned long)p96GetBitMapAttr(rp->BitMap, P96BMA_BITSPERPIXEL),
           (unsigned long)p96GetBitMapAttr(rp->BitMap, P96BMA_RGBFORMAT),
           (unsigned long)p96GetBitMapAttr(rp->BitMap, P96BMA_BYTESPERROW),
           (unsigned long)p96GetBitMapAttr(rp->BitMap, P96BMA_ISONBOARD));
    check("p96GetBitMapAttr on the screen", p96GetBitMapAttr(rp->BitMap, P96BMA_ISP96) &&
          p96GetBitMapAttr(rp->BitMap, P96BMA_WIDTH) == 640 && p96GetBitMapAttr(rp->BitMap, P96BMA_MEMORY));

    p96RectFill(rp, 20, 40, 219, 139, 0x2060c0);
    check("p96RectFill + p96ReadPixel", near(p96ReadPixel(rp, 100, 90), 0x2060c0, tol) &&
          near(p96ReadPixel(rp, 20, 40), 0x2060c0, tol) && near(p96ReadPixel(rp, 219, 139), 0x2060c0, tol) &&
          !near(p96ReadPixel(rp, 220, 140), 0x2060c0, 4));
    p96WritePixel(rp, 300, 60, 0xe02020);
    check("p96WritePixel", near(p96ReadPixel(rp, 300, 60), 0xe02020, tol));

    /* a 32x32 gradient written in each source format, read back as ARGB */
    for (f = 0; f < 5; f++) {
        static const ULONG fmts[5] = { RGBFB_R8G8B8, RGBFB_A8R8G8B8, RGBFB_B8G8R8A8, RGBFB_R5G6B5PC,
                                       RGBFB_R5G6B5 };
        static const char *const names[5] = { "p96WritePixelArray R8G8B8", "p96WritePixelArray A8R8G8B8",
                                              "p96WritePixelArray B8G8R8A8", "p96WritePixelArray R5G6B5PC",
                                              "p96WritePixelArray R5G6B5" };
        int bpp = f == 0 ? 3 : f < 3 ? 4 : 2, t2 = (f >= 3 && tol < 8) ? 8 : tol;
        for (y = 0; y < 32; y++)
            for (x = 0; x < 32; x++) {
                ULONG c = grad(x, y);
                UBYTE *d = buf + (y * 32 + x) * bpp;
                UWORD w16 = ((c >> 8) & 0xf800) | ((c >> 5) & 0x07e0) | ((c >> 3) & 0x1f);
                switch (f) {
                case 0: d[0] = c >> 16; d[1] = c >> 8; d[2] = c; break;
                case 1: d[0] = 0; d[1] = c >> 16; d[2] = c >> 8; d[3] = c; break;
                case 2: d[0] = c; d[1] = c >> 8; d[2] = c >> 16; d[3] = 0; break;
                case 3: d[0] = w16; d[1] = w16 >> 8; break;
                case 4: d[0] = w16 >> 8; d[1] = w16; break;
                }
            }
        ri.Memory = buf; ri.BytesPerRow = 32 * bpp; ri.pad = 0; ri.RGBFormat = fmts[f];
        arr(102, &ri, 0, 0, rp, 240 + f * 40, 200, 32, 32);
        memset(back, 0x55, sizeof(back));
        ri.Memory = back; ri.BytesPerRow = 32 * 4; ri.RGBFormat = RGBFB_A8R8G8B8;
        arr(108, &ri, 0, 0, rp, 240 + f * 40, 200, 32, 32);
        for (bad = 0, y = 0; y < 32; y++)
            for (x = 0; x < 32; x++) {
                UBYTE *q = back + (y * 32 + x) * 4;
                if (!near(((ULONG)q[1] << 16) | ((ULONG)q[2] << 8) | q[3], grad(x, y), t2))
                    bad++;
            }
        if (bad) printf("    %d of 1024 pixels differ\n", bad);
        check(names[f], bad == 0);
    }

    /* part of an array, at an offset inside the source */
    ri.Memory = buf; ri.BytesPerRow = 32 * 2; ri.RGBFormat = RGBFB_R5G6B5;
    p96RectFill(rp, 240, 250, 300, 300, 0);
    arr(102, &ri, 8, 4, rp, 250, 260, 16, 8);
    check("p96WritePixelArray with a source offset",
          near(p96ReadPixel(rp, 250, 260), grad(8, 4), tol < 8 ? 8 : tol) &&
          near(p96ReadPixel(rp, 265, 267), grad(23, 11), tol < 8 ? 8 : tol) &&
          near(p96ReadPixel(rp, 266, 267), 0, 4) && near(p96ReadPixel(rp, 249, 260), 0, 4));

    /* separate red, green and blue planes */
    for (y = 0; y < 32; y++)
        for (x = 0; x < 32; x++) {
            ULONG c = grad(x, y);
            pr[y * 32 + x] = c >> 16; pg[y * 32 + x] = c >> 8; pb[y * 32 + x] = c;
        }
    tci.PixelDistance = 1; tci.BytesPerRow = 32; tci.RedData = pr; tci.GreenData = pg; tci.BlueData = pb;
    arr(132, &tci, 0, 0, rp, 460, 200, 32, 32);
    memset(pr, 0, sizeof(pr)); memset(pg, 0, sizeof(pg)); memset(pb, 0, sizeof(pb));
    arr(138, &tci, 0, 0, rp, 460, 200, 32, 32);
    for (bad = 0, y = 0; y < 32; y++)
        for (x = 0; x < 32; x++)
            if (!near(((ULONG)pr[y * 32 + x] << 16) | ((ULONG)pg[y * 32 + x] << 8) | pb[y * 32 + x],
                      grad(x, y), tol))
                bad++;
    check("p96WriteTrueColorData / p96ReadTrueColorData", bad == 0);

    /* a bitmap in a format of our choosing, written directly, blitted over */
    bm = p96AllocBitMap(48, 48, 24, BMF_CLEAR, NULL, RGBFB_R8G8B8);
    check("p96AllocBitMap 48x48 R8G8B8", bm && p96GetBitMapAttr(bm, P96BMA_RGBFORMAT) == RGBFB_R8G8B8 &&
          p96GetBitMapAttr(bm, P96BMA_BYTESPERPIXEL) == 3 && p96GetBitMapAttr(bm, P96BMA_WIDTH) == 48);
    if (bm) {
        LONG lk;
        memset(&ri, 0, sizeof(ri));
        lk = p96LockBitMap(bm, (UBYTE *)&ri, sizeof(ri));
        check("p96LockBitMap", lk && ri.Memory && ri.BytesPerRow >= 144 && ri.RGBFormat == RGBFB_R8G8B8);
        if (lk) {
            for (y = 0; y < 48; y++)
                for (x = 0; x < 48; x++) {
                    UBYTE *d = (UBYTE *)ri.Memory + y * ri.BytesPerRow + x * 3;
                    ULONG c = grad(x, y);
                    d[0] = c >> 16; d[1] = c >> 8; d[2] = c;
                }
            p96UnlockBitMap(bm, lk);
        }
        BltBitMapRastPort(bm, 0, 0, rp, 520, 200, 48, 48, 0xc0);
        WaitBlit();
        check("locked bitmap blitted to the screen", near(p96ReadPixel(rp, 520, 200), grad(0, 0), tol) &&
              near(p96ReadPixel(rp, 540, 230), grad(20, 30), tol) &&
              near(p96ReadPixel(rp, 567, 247), grad(47, 47), tol));
        p96FreeBitMap(bm);
    }
    /* like a friend: same format as the screen */
    bm = p96AllocBitMap(64, 32, depth, 0, rp->BitMap, 0);
    v = bm ? p96GetBitMapAttr(bm, P96BMA_RGBFORMAT) : 0;
    check("p96AllocBitMap with the screen as friend", bm && v == p96GetBitMapAttr(rp->BitMap, P96BMA_RGBFORMAT));
    if (bm) p96FreeBitMap(bm);

    if (args[1])
        Delay(*(LONG *)args[1] * 50);
out:
    if (s)
        check("p96CloseScreen", tagcall((struct TagItem *)s, 0, 96) != 0);
    printf("p96test: %d of %d checks failed\n", failed, checks);
    CloseLibrary(P96Base);
    FreeArgs(rda);
    return failed ? 5 : 0;
}
