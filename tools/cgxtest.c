/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laine Jones */
/* cgxtest: the CyberGraphX 43 calls in Prism's cybergraphics.library -
 * WritePixelArrayAlpha, BltTemplateAlpha and ProcessPixelArray - on a
 * screen in the given mode, each result read back and checked. Prints one
 * line per check and the number that failed. On an 8-bit screen the colours
 * go through the palette, so only the calls' counts are checked there.
 *
 *   cgxtest [MODEID=$7a000101]
 */
#include <exec/types.h>
#include <exec/libraries.h>
#include <dos/dos.h>
#include <graphics/gfx.h>
#include <graphics/rastport.h>
#include <intuition/screens.h>
#include <utility/tagitem.h>
#include <cybergraphx/cybergraphics.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <proto/cybergraphics.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct Library *CyberGfxBase;
static int fails, checks, deep;
static ULONG tol;

#define RA(t, n, r, v) register t n __asm(r) = (v)

static ULONG WPAAlpha(APTR src, UWORD sx, UWORD sy, UWORD smod, struct RastPort *rp,
                      UWORD dx, UWORD dy, UWORD w, UWORD h, ULONG ga)
{
    RA(APTR, a0, "a0", src); RA(ULONG, d0, "d0", sx); RA(ULONG, d1, "d1", sy); RA(ULONG, d2, "d2", smod);
    RA(struct RastPort *, a1, "a1", rp); RA(ULONG, d3, "d3", dx); RA(ULONG, d4, "d4", dy);
    RA(ULONG, d5, "d5", w); RA(ULONG, d6, "d6", h); RA(ULONG, d7, "d7", ga);
    RA(struct Library *, a6, "a6", CyberGfxBase);
    __asm volatile ("jsr -216(%%a6)" : "+r"(d0), "+r"(d1), "+r"(a0), "+r"(a1)
                    : "r"(d2), "r"(d3), "r"(d4), "r"(d5), "r"(d6), "r"(d7), "r"(a6) : "cc", "memory");
    return d0;
}

static void BTAlpha(APTR src, LONG sx, LONG smod, struct RastPort *rp, LONG dx, LONG dy, LONG w, LONG h)
{
    RA(APTR, a0, "a0", src); RA(LONG, d0, "d0", sx); RA(LONG, d1, "d1", smod);
    RA(struct RastPort *, a1, "a1", rp); RA(LONG, d2, "d2", dx); RA(LONG, d3, "d3", dy);
    RA(LONG, d4, "d4", w); RA(LONG, d5, "d5", h); RA(struct Library *, a6, "a6", CyberGfxBase);
    __asm volatile ("jsr -222(%%a6)" : "+r"(d0), "+r"(d1), "+r"(a0), "+r"(a1)
                    : "r"(d2), "r"(d3), "r"(d4), "r"(d5), "r"(a6) : "cc", "memory");
}

static ULONG PPArray(struct RastPort *rp, ULONG x, ULONG y, ULONG w, ULONG h, ULONG op, LONG v,
                     struct TagItem *tags)
{
    RA(struct RastPort *, a1, "a1", rp); RA(ULONG, d0, "d0", x); RA(ULONG, d1, "d1", y);
    RA(ULONG, d2, "d2", w); RA(ULONG, d3, "d3", h); RA(ULONG, d4, "d4", op); RA(LONG, d5, "d5", v);
    RA(struct TagItem *, a2, "a2", tags); RA(struct Library *, a6, "a6", CyberGfxBase);
    __asm volatile ("jsr -228(%%a6)" : "+r"(d0), "+r"(d1), "+r"(a1)
                    : "r"(d2), "r"(d3), "r"(d4), "r"(d5), "r"(a2), "r"(a6) : "a0", "cc", "memory");
    return d0;
}

#define POP_BRIGHTEN 0
#define POP_DARKEN 1
#define POP_SETALPHA 2
#define POP_TINT 3
#define POP_BLUR 4
#define POP_COLOR2GREY 5
#define POP_NEGATIVE 6
#define POP_NEGFADE 7
#define POP_TINTFADE 8
#define POP_GRADIENT 9
#define POP_SHIFTRGB 10
#define PPAOPTAG_GRADIENTTYPE 0x85231022
#define PPAOPTAG_GRADCOLOR1 0x85231023
#define PPAOPTAG_GRADCOLOR2 0x85231024

static void check(const char *what, BOOL ok)
{
    checks++;
    if (!ok) fails++;
    printf("  %-44s %s\n", what, ok ? "ok" : "FAILED");
}

/* the pixel at x,y is c, give or take the screen depth's rounding */
static BOOL near(struct RastPort *rp, WORD x, WORD y, ULONG c)
{
    ULONG p = ReadRGBPixel(rp, x, y);
    int i;
    if (!deep) return TRUE;
    for (i = 0; i < 24; i += 8) {
        LONG d = (LONG)((p >> i) & 255) - (LONG)((c >> i) & 255);
        if (d < 0) d = -d;
        if ((ULONG)d > tol) {
            printf("    pixel %d,%d is %06lx, want %06lx\n", x, y, (unsigned long)p, (unsigned long)c);
            return FALSE;
        }
    }
    return TRUE;
}

static void fill(struct RastPort *rp, ULONG c)
{
    FillPixelArray(rp, 0, 0, 320, 120, c);
}

int main(void)
{
    static ULONG argb[8 * 8];
    static UBYTE tmpl[8 * 8];
    struct RDArgs *ra;
    LONG args[1] = { 0 };
    ULONG id = 0x7a000101, n, depth;
    struct Screen *scr;
    struct RastPort *rp;
    int i;

    if ((ra = ReadArgs("MODEID/K", args, NULL))) {
        if (args[0]) id = strtoul((char *)args[0] + (*(char *)args[0] == '$'), NULL, 16);
        FreeArgs(ra);
    }
    if (!(CyberGfxBase = OpenLibrary("cybergraphics.library", 43))) {
        puts("cgxtest: no cybergraphics.library 43");
        return 20;
    }
    printf("cgxtest: %s\n", (char *)CyberGfxBase->lib_IdString);
    if (!(scr = OpenScreenTags(NULL, SA_DisplayID, id, SA_Width, 640, SA_Height, 480,
                               SA_Depth, 8, SA_Quiet, TRUE, SA_ShowTitle, FALSE, TAG_DONE))) {
        printf("cgxtest: can't open mode $%08lx\n", (unsigned long)id);
        CloseLibrary(CyberGfxBase);
        return 20;
    }
    rp = &scr->RastPort;
    depth = GetCyberMapAttr(rp->BitMap, CYBRMATTR_DEPTH);
    deep = depth > 8;
    tol = depth <= 16 ? 9 : 1;
    printf("  screen mode $%08lx, %lu bits\n", (unsigned long)id, (unsigned long)depth);

    /* WritePixelArrayAlpha */
    for (i = 0; i < 64; i++) argb[i] = 0xffff0000;
    fill(rp, 0x404040);
    n = WPAAlpha(argb, 0, 0, 32, rp, 10, 10, 8, 8, 0xffffffff);
    check("WritePixelArrayAlpha opaque: count", n == 64);
    check("WritePixelArrayAlpha opaque: colour", near(rp, 13, 13, 0xff0000));
    for (i = 0; i < 64; i++) argb[i] = 0x80ff0000;
    WPAAlpha(argb, 0, 0, 32, rp, 30, 10, 8, 8, 0xffffffff);
    check("WritePixelArrayAlpha half alpha", near(rp, 33, 13, 0x9f2020));
    for (i = 0; i < 64; i++) argb[i] = 0xffff0000;
    WPAAlpha(argb, 0, 0, 32, rp, 50, 10, 8, 8, 0x80000000);
    check("WritePixelArrayAlpha half global alpha", near(rp, 53, 13, 0x9f2020));
    for (i = 0; i < 64; i++) argb[i] = 0x00ff0000;
    WPAAlpha(argb, 0, 0, 32, rp, 70, 10, 8, 8, 0xffffffff);
    check("WritePixelArrayAlpha transparent", near(rp, 73, 13, 0x404040));
    n = WPAAlpha(argb, 0, 0, 32, rp, 90, 10, 8, 8, 0);
    check("WritePixelArrayAlpha global alpha 0 draws nothing", n == 0 && near(rp, 93, 13, 0x404040));
    for (i = 0; i < 64; i++) argb[i] = (i & 7) < 4 ? 0xff0000ff : 0x00000000;
    WPAAlpha(argb, 0, 0, 32, rp, 110, 10, 8, 8, 0xffffffff);
    check("WritePixelArrayAlpha per-pixel alpha", near(rp, 111, 13, 0x0000ff) && near(rp, 116, 13, 0x404040));
    n = WPAAlpha(argb, 0, 0, 32, rp, 636, 476, 8, 8, 0xffffffff);
    check("WritePixelArrayAlpha clipped at the edge", n == 16);

    /* BltTemplateAlpha: the foreground pen's colour */
    SetRGB32(&scr->ViewPort, 1, 0, 0xffffffff, 0);
    SetAPen(rp, 1);
    for (i = 0; i < 64; i++) tmpl[i] = 255;
    BTAlpha(tmpl, 0, 8, rp, 10, 40, 8, 8);
    check("BltTemplateAlpha opaque", near(rp, 13, 43, 0x00ff00));
    for (i = 0; i < 64; i++) tmpl[i] = 128;
    BTAlpha(tmpl, 0, 8, rp, 30, 40, 8, 8);
    check("BltTemplateAlpha half", near(rp, 33, 43, 0x209f20));
    for (i = 0; i < 64; i++) tmpl[i] = 0;
    BTAlpha(tmpl, 0, 8, rp, 50, 40, 8, 8);
    check("BltTemplateAlpha transparent", near(rp, 53, 43, 0x404040));

    /* ProcessPixelArray */
    fill(rp, 0x404040);
    n = PPArray(rp, 0, 0, 16, 16, POP_BRIGHTEN, 0x20, NULL);
    check("ProcessPixelArray brighten", n == 256 && near(rp, 5, 5, 0x606060));
    PPArray(rp, 20, 0, 16, 16, POP_DARKEN, 0x20, NULL);
    check("ProcessPixelArray darken", near(rp, 25, 5, 0x202020));
    PPArray(rp, 40, 0, 16, 16, POP_NEGATIVE, 0, NULL);
    check("ProcessPixelArray negative", near(rp, 45, 5, 0xbfbfbf));
    FillPixelArray(rp, 60, 0, 16, 16, 0xff0000);
    PPArray(rp, 60, 0, 16, 16, POP_COLOR2GREY, 0, NULL);
    check("ProcessPixelArray grey", near(rp, 65, 5, 0x4c4c4c));
    FillPixelArray(rp, 80, 0, 16, 16, 0x000000);
    PPArray(rp, 80, 0, 16, 16, POP_TINT, 0x80ff0000, NULL);
    check("ProcessPixelArray tint", near(rp, 85, 5, 0x800000));
    FillPixelArray(rp, 100, 0, 16, 16, 0x102030);
    PPArray(rp, 100, 0, 16, 16, POP_SHIFTRGB, 0, NULL);
    check("ProcessPixelArray shift RGB", near(rp, 105, 5, 0x301020));
    {
        struct TagItem g[] = { { PPAOPTAG_GRADIENTTYPE, 0 }, { PPAOPTAG_GRADCOLOR1, 0 },
                               { PPAOPTAG_GRADCOLOR2, 0xffffff }, { TAG_DONE, 0 } };
        PPArray(rp, 0, 20, 256, 8, POP_GRADIENT, 0, g);
        check("ProcessPixelArray horizontal gradient",
              near(rp, 0, 22, 0x000000) && near(rp, 255, 22, 0xffffff) && near(rp, 128, 22, 0x808080));
        g[0].ti_Data = 1;
        PPArray(rp, 0, 40, 8, 64, POP_GRADIENT, 0, g);
        check("ProcessPixelArray vertical gradient", near(rp, 3, 40, 0x000000) && near(rp, 3, 103, 0xffffff));
    }
    FillPixelArray(rp, 0, 110, 9, 9, 0x000000);
    FillPixelArray(rp, 4, 114, 1, 1, 0xffffff);
    PPArray(rp, 0, 110, 9, 9, POP_BLUR, 0, NULL);
    {
        ULONG c = ReadRGBPixel(rp, 4, 114), e = ReadRGBPixel(rp, 3, 114);
        check("ProcessPixelArray blur", !deep || ((c & 255) < 200 && (e & 255) > 0));
    }
    FillPixelArray(rp, 20, 110, 64, 4, 0x000000);
    PPArray(rp, 20, 110, 64, 4, POP_NEGFADE, 0, NULL);
    check("ProcessPixelArray negative fade", near(rp, 20, 111, 0x000000) && near(rp, 83, 111, 0xffffff));
    n = PPArray(rp, 0, 0, 16, 16, POP_SETALPHA, 0x80, NULL);
    check("ProcessPixelArray set alpha: count", n == 256);
    n = PPArray(rp, 0, 0, 16, 16, 99, 0, NULL);
    check("ProcessPixelArray unknown operation does nothing", n == 0);

    Delay(25);
    CloseScreen(scr);
    printf("cgxtest: %d of %d checks failed\n", fails, checks);
    CloseLibrary(CyberGfxBase);
    return fails ? 5 : 0;
}
