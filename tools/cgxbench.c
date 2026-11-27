/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laine Jones */
/* cgxbench: times cybergraphics.library's pixel-array calls (Fill, Invert,
 * Write, Read and ScalePixelArray) at 8, 16, 24 and 32 bits, and prints a
 * checksum of a fixed sequence of the same calls drawn into a window that
 * another window partly covers, so two builds can be compared for speed
 * and for identical results.
 *
 *   cgxbench [DEPTH=n] [NOVERIFY] [NOBENCH]
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <devices/timer.h>
#include <graphics/gfx.h>
#include <intuition/intuition.h>
#include <cybergraphx/cybergraphics.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <proto/timer.h>
#include <proto/cybergraphics.h>
#include <stdio.h>
#include <string.h>

#ifndef RECTFMT_RAW
#define RECTFMT_RAW 5
#endif

struct Library *CyberGfxBase;
struct Device *TimerBase;

#define IW 200
#define IH 200
#define SW 160
#define SH 120

static struct timerequest tr;
static UBYTE *img;          /* IW x IH, 4 bytes a pixel (big enough for every format) */
static UBYTE *buf;
static struct RastPort *rp;

static void make_image(UBYTE rf, UBYTE *d, LONG w, LONG h)
{
    LONG x, y;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            UBYTE r = x * 255 / (w - 1), g = y * 255 / (h - 1), b = (x ^ y) * 7;
            switch (rf) {
            case RECTFMT_RGB:  *d++ = r; *d++ = g; *d++ = b; break;
            case RECTFMT_RGBA: *d++ = r; *d++ = g; *d++ = b; *d++ = 0xff; break;
            case RECTFMT_ARGB: *d++ = 0xff; *d++ = r; *d++ = g; *d++ = b; break;
            default:           *d++ = (x * 3 + y * 5) & 255; break;   /* LUT8, GREY8 */
            }
        }
}

static ULONG fnv(ULONG h, const UBYTE *p, ULONG n)
{
    while (n--)
        h = (h ^ *p++) * 16777619UL;
    return h;
}

typedef void (*testfn)(LONG i);

static void t_fill(LONG i)   { FillPixelArray(rp, 10, 10, IW, IH, (i & 1) ? 0x00336699 : 0x00996633); }
static void t_invert(LONG i) { InvertPixelArray(rp, 10, 10, IW, IH); }
static UBYTE wfmt, sfmt;
static ULONG wmod;
static void t_write(LONG i)  { WritePixelArray(img, 0, 0, wmod, rp, 10 + (i & 3), 10, IW, IH, wfmt); }
static void t_read(LONG i)   { ReadPixelArray(buf, 0, 0, wmod, rp, 10 + (i & 3), 10, IW, IH, wfmt); }
static void t_scale(LONG i)  { ScalePixelArray(img, SW, SH, SW * (sfmt == RECTFMT_RGB ? 3 : 4), rp, 10, 10, SW * 2, SH * 2, sfmt); }

static void run(const char *name, testfn fn, LONG kpix)
{
    struct EClockVal ev;
    ULONG f = ReadEClock(&ev), t0 = ev.ev_lo, el;
    LONG n = 0;
    do {
        fn(n); fn(n + 1); fn(n + 2); fn(n + 3);
        n += 4;
        WaitBlit();
        ReadEClock(&ev);
        el = ev.ev_lo - t0;
    } while (el < f);
    {
        ULONG r10 = (ULONG)((double)n * f * 10 / el);
        printf("  %-24s %6lu.%lu /s  (%lu Kpix/s)\n", name, r10 / 10, r10 % 10,
               (ULONG)((double)n * f / el * kpix / 1000));
    }
}

static const UBYTE fmts[] = { RECTFMT_RGB, RECTFMT_RGBA, RECTFMT_ARGB, RECTFMT_LUT8, RECTFMT_GREY8 };
static const char *const fnames[] = { "RGB", "RGBA", "ARGB", "LUT8", "GREY8" };

/* The checked sequence: the area under test is 300x220 at 0,0 of rp; the
 * caller covers part of it with another window. Each step's checksum is of
 * the area read back RAW (whose path these changes leave alone), the read
 * steps' of the buffer read. */
static ULONG verify(struct RastPort *r, BOOL show)
{
    ULONG all = 2166136261UL, h;
    ULONG raw = 300UL * GetCyberMapAttr(r->BitMap, CYBRMATTR_BPPIX);
    LONG k;
    char name[32];
#define STEP(label) do { \
        ReadPixelArray(buf, 0, 0, raw, r, 0, 0, 300, 220, RECTFMT_RAW); \
        h = fnv(2166136261UL, buf, raw * 220); all = fnv(all, (UBYTE *)&h, 4); \
        if (show) printf("    %-20s %08lx\n", label, h); } while (0)

    FillPixelArray(r, 0, 0, 300, 220, 0x00123456);
    STEP("fill");
    for (k = 0; k < 5; k++) {
        make_image(fmts[k], img, 64, 48);
        WritePixelArray(img, 3, 2, 64 * (k < 3 ? (k ? 4 : 3) : 1), r, 7 + k * 57, 5 + k * 41,
                        58, 43, fmts[k]);
        sprintf(name, "write %s", fnames[k]);
        STEP(name);
    }
    InvertPixelArray(r, 5, 5, 290, 200);
    STEP("invert");
    FillPixelArray(r, 100, 50, 150, 100, 0x00ff8020);
    InvertPixelArray(r, 120, 60, 37, 41);
    STEP("fill+invert");
    for (k = 0; k < 3; k++) {
        make_image(fmts[k], img, SW, SH);
        ScalePixelArray(img, 64 + k * 9, 48 + k * 5, SW * (k ? 4 : 3), r, 11 + k * 61, 9 + k * 37,
                        150 - k * 47, 100 + k * 13, fmts[k]);
        sprintf(name, "scale %s", fnames[k]);
        STEP(name);
    }
    for (k = 0; k < 5; k++) {
        ULONG bpp = k < 3 ? (k ? 4 : 3) : 1;
        memset(buf, 0xa5, 300 * 4 * 220);
        ReadPixelArray(buf, 1, 1, 300 * bpp, r, 3, 2, 290, 210, fmts[k]);
        h = fnv(2166136261UL, buf, 300 * bpp * 220);
        all = fnv(all, (UBYTE *)&h, 4);
        if (show) printf("    read %-15s %08lx\n", fnames[k], h);
    }
    return all;
#undef STEP
}

static void one_depth(LONG depth, BOOL doVerify, BOOL doBench)
{
    struct Screen *s;
    struct Window *w, *over;
    ULONG id = BestCModeIDTags(CYBRBIDTG_Depth, depth, CYBRBIDTG_NominalWidth, 640,
                               CYBRBIDTG_NominalHeight, 480, TAG_DONE);
    LONG k;
    if (id == INVALID_ID) {
        printf("cgxbench: no %ld-bit mode\n", (long)depth);
        return;
    }
    s = OpenScreenTags(NULL, SA_DisplayID, id, SA_Depth, depth, SA_Width, 640, SA_Height, 480,
                       SA_Quiet, TRUE, SA_ShowTitle, FALSE, TAG_DONE);
    if (!s) {
        printf("cgxbench: can't open mode $%08lx\n", (unsigned long)id);
        return;
    }
    if (depth == 8) {
        static ULONG pal[2 + 256 * 3];
        pal[0] = 256 << 16;
        for (k = 0; k < 256; k++) {
            pal[1 + k * 3] = (ULONG)((k & 7) * 36) << 24;
            pal[2 + k * 3] = (ULONG)(((k >> 3) & 7) * 36) << 24;
            pal[3 + k * 3] = (ULONG)((k >> 6) * 85) << 24;
        }
        LoadRGB32(&s->ViewPort, pal);
    }
    w = OpenWindowTags(NULL, WA_CustomScreen, (ULONG)s, WA_Left, 0, WA_Top, 0, WA_Width, 640,
                       WA_Height, 480, WA_Borderless, TRUE, WA_SmartRefresh, TRUE, TAG_DONE);
    if (w) {
        rp = w->RPort;
        printf("cgxbench: mode $%08lx, %ld bits, pixfmt %lu\n", (unsigned long)id, (long)depth,
               (unsigned long)GetCyberMapAttr(rp->BitMap, CYBRMATTR_PIXFMT));
        if (doVerify) {
            over = OpenWindowTags(NULL, WA_CustomScreen, (ULONG)s, WA_Left, 170, WA_Top, 90,
                                  WA_Width, 60, WA_Height, 50, WA_Borderless, TRUE,
                                  WA_SimpleRefresh, TRUE, TAG_DONE);
            if (over) {
                printf("  verify %08lx\n", (unsigned long)verify(rp, TRUE));
                CloseWindow(over);
            }
        }
        if (doBench) {
            make_image(RECTFMT_ARGB, img, IW, IH);
            run("FillPixelArray 200x200", t_fill, IW * IH);
            run("InvertPixelArray 200x200", t_invert, IW * IH);
            for (k = 0; k < 5; k++) {
                char n[40];
                wfmt = fmts[k];
                wmod = IW * (k < 3 ? (k ? 4 : 3) : 1);
                make_image(wfmt, img, IW, IH);
                sprintf(n, "WritePixelArray %s", fnames[k]);
                run(n, t_write, IW * IH);
            }
            for (k = 0; k < 5; k++) {
                char n[40];
                wfmt = fmts[k];
                wmod = IW * (k < 3 ? (k ? 4 : 3) : 1);
                sprintf(n, "ReadPixelArray %s", fnames[k]);
                run(n, t_read, IW * IH);
            }
            for (k = 0; k < 3; k++) {
                char n[40];
                sfmt = fmts[k];
                make_image(sfmt, img, SW, SH);
                sprintf(n, "ScalePixelArray %s 2x", fnames[k]);
                run(n, t_scale, SW * SH * 4);
            }
        }
        CloseWindow(w);
    }
    CloseScreen(s);
}

int main(void)
{
    LONG args[3] = { 0 };
    struct RDArgs *ra;
    struct MsgPort *mp = NULL;
    static const LONG depths[] = { 8, 16, 24, 32 };
    LONG k;
    int rc = 20;

    if (!(ra = ReadArgs("DEPTH/K/N,NOVERIFY/S,NOBENCH/S", args, NULL))) {
        PrintFault(IoErr(), "cgxbench");
        return 20;
    }
    if (!(CyberGfxBase = OpenLibrary("cybergraphics.library", 41))) {
        puts("cgxbench: no cybergraphics.library");
        goto out;
    }
    img = AllocVec(IW * IH * 4, MEMF_ANY);
    buf = AllocVec(300 * 4 * 220, MEMF_ANY);
    if (!img || !buf || !(mp = CreateMsgPort()))
        goto out;
    tr.tr_node.io_Message.mn_ReplyPort = mp;
    if (OpenDevice(TIMERNAME, UNIT_ECLOCK, (struct IORequest *)&tr, 0))
        goto out;
    TimerBase = tr.tr_node.io_Device;
    printf("cgxbench: %s\n", (char *)CyberGfxBase->lib_IdString);
    for (k = 0; k < 4; k++)
        if (!args[0] || *(LONG *)args[0] == depths[k])
            one_depth(depths[k], !args[1], !args[2]);
    rc = 0;
    CloseDevice((struct IORequest *)&tr);
out:
    if (mp) DeleteMsgPort(mp);
    FreeVec(buf);
    FreeVec(img);
    if (CyberGfxBase) CloseLibrary(CyberGfxBase);
    FreeArgs(ra);
    return rc;
}
