/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laine Jones */
/*
 * PrismBench - time the graphics.library calls Workbench leans on, on a
 * screen in a given mode (Prism or native), for before/after comparisons.
 *
 *   PrismBench [MODEID=$7A000001] [DEPTH=8]
 *
 * Opens a screen with a window, runs each test for about a second and
 * prints operations per second (EClock-timed).
 */
#include <exec/types.h>
#include <devices/timer.h>
#include <graphics/gfx.h>
#include <graphics/displayinfo.h>
#include <intuition/intuition.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/graphics.h>
#include <graphics/gfxmacros.h>
#include <proto/intuition.h>
#include <proto/timer.h>
#include <stdio.h>
#include <stdlib.h>

struct GfxBase *GfxBase;
struct IntuitionBase *IntuitionBase;
struct Device *TimerBase;

static const char version[] __attribute__((used)) = "$VER: PrismBench 1.1b3 (08.10.2026)";

#define TEMPLATE "MODEID/K,DEPTH/K/N,LIST/S"

static struct timerequest tr;

typedef void (*testfn)(struct RastPort *rp, LONG i);

static void t_fill(struct RastPort *rp, LONG i)
{
    SetAPen(rp, (i & 7) + 1);
    RectFill(rp, 10, 20, 109, 119);
}

static const char line[] = "The quick brown fox jumps over the lazy dog";

static void t_text1(struct RastPort *rp, LONG i)
{
    SetDrMd(rp, JAM1); SetAPen(rp, 1);
    Move(rp, 10, 140 + (i & 7) * 8);
    Text(rp, line, 40);
}

static void t_text2(struct RastPort *rp, LONG i)
{
    SetDrMd(rp, JAM2); SetAPen(rp, 2); SetBPen(rp, 3);
    Move(rp, 10, 140 + (i & 7) * 8);
    Text(rp, line, 40);
}

static void t_copy(struct RastPort *rp, LONG i)
{
    ClipBlit(rp, 10, 20, rp, 150 + (i & 3), 20, 200, 100, 0xc0);
}

static void t_scroll(struct RastPort *rp, LONG i)
{
    ScrollRaster(rp, 0, 8, 10, 130, 409, 229);
}

static void t_lines(struct RastPort *rp, LONG i)
{
    SetAPen(rp, (i & 3) + 1);
    Move(rp, 10, 240);
    Draw(rp, 400, 240 + (i & 63));
}

static void t_pixels(struct RastPort *rp, LONG i)
{
    SetAPen(rp, i & 3);
    WritePixel(rp, 10 + (i & 255), 300 + ((i >> 8) & 31));
}

/* micro-tests: what the pieces of a small call cost */
static void t_setapen(struct RastPort *rp, LONG i)
{
    SetAPen(rp, i & 3);
}

static void t_pixel_only(struct RastPort *rp, LONG i)
{
    WritePixel(rp, 10 + (i & 255), 300 + ((i >> 8) & 31));
}

static void t_locklayer(struct RastPort *rp, LONG i)
{
    if (rp->Layer) {
        LockLayerRom(rp->Layer);
        UnlockLayerRom(rp->Layer);
    }
}

static void t_readpixel(struct RastPort *rp, LONG i)
{
    ReadPixel(rp, 10 + (i & 255), 300 + ((i >> 8) & 31));
}

static void t_line_flat(struct RastPort *rp, LONG i)
{
    Move(rp, 10, 240);
    Draw(rp, 400, 242);
}

static void t_line_steep(struct RastPort *rp, LONG i)
{
    Move(rp, 10, 240);
    Draw(rp, 400, 300);
}

static void t_line_vert(struct RastPort *rp, LONG i)
{
    Move(rp, 10, 240);
    Draw(rp, 40, 340);
}

static void t_move(struct RastPort *rp, LONG i)
{
    Move(rp, 10, 240);
}

static void t_fill_small(struct RastPort *rp, LONG i)
{
    SetAPen(rp, (i & 7) + 1);
    RectFill(rp, 420 + (i & 15) * 8, 20, 427 + (i & 15) * 8, 27);
}

static void t_text_short(struct RastPort *rp, LONG i)
{
    SetDrMd(rp, JAM2); SetAPen(rp, 1); SetBPen(rp, 0);
    Move(rp, 420, 50 + (i & 7) * 8);
    Text(rp, line, 4);
}

/* 64x32 icon-sized images: a planar 8-plane one (icons, gadget imagery)
 * and one made with the screen as friend (chunky on a Prism screen) */
/* a filled ellipse and a filled triangle: AreaEnd -> BltPattern with a mask */
static void t_area(struct RastPort *rp, LONG i)
{
    SetAPen(rp, 1 + (i & 7));
    AreaEllipse(rp, 150, 150, 100, 70);
    AreaEnd(rp);
    AreaMove(rp, 300, 100);
    AreaDraw(rp, 500, 180);
    AreaDraw(rp, 340, 260);
    AreaEnd(rp);
}

static struct BitMap *planarBM, *friendBM;

static void t_blit_planar(struct RastPort *rp, LONG i)
{
    BltBitMapRastPort(planarBM, 0, 0, rp, 420 + (i & 3) * 4, 120, 64, 32, 0xc0);
}

static void t_blit_friend(struct RastPort *rp, LONG i)
{
    BltBitMapRastPort(friendBM, 0, 0, rp, 420 + (i & 3) * 4, 170, 64, 32, 0xc0);
}

/* Time in EClock ticks, integer only: the clock is read once per 8 calls
 * and no floating point runs inside the loop. (The first version did
 * soft-float maths after every call, which on an FPU-less 68030 cost more
 * than most of the calls it timed and flattened every comparison.) */
static void run(const char *name, testfn fn, struct RastPort *rp, const char *unit, LONG per)
{
    struct EClockVal ev;
    ULONG f = ReadEClock(&ev), t0 = ev.ev_lo, el;
    LONG n = 0;
    double rate;
    do {
        fn(rp, n); fn(rp, n + 1); fn(rp, n + 2); fn(rp, n + 3);
        fn(rp, n + 4); fn(rp, n + 5); fn(rp, n + 6); fn(rp, n + 7);
        n += 8;
        WaitBlit();
        ReadEClock(&ev);
        el = ev.ev_lo - t0;
    } while (el < f);
    rate = (double)n * f / el;
    printf("  %-22s %8.1f /s  (%.0f %s/s)\n", name, rate, rate * per, unit);
}

int main(void)
{
    LONG args[3] = { 0 };
    struct RDArgs *rda;
    struct Screen *s = NULL;
    struct Window *w = NULL;
    struct MsgPort *mp = NULL;
    ULONG id = 0x7A000001;
    LONG depth = 8;
    int rc = 20;

    if (!(rda = ReadArgs(TEMPLATE, args, NULL))) {
        PrintFault(IoErr(), "PrismBench");
        return 20;
    }
    if (args[0]) {
        const char *p = (const char *)args[0];
        if (*p == '$') p++;
        id = strtoul(p, NULL, 16);
    }
    if (args[1]) depth = *(LONG *)args[1];

    GfxBase = (struct GfxBase *)OpenLibrary("graphics.library", 39);
    IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 39);
    if (!GfxBase || !IntuitionBase) goto out;
    if (args[2]) {
        /* every display mode 640 wide or more: name, size, deepest depth,
         * so other RTG systems' ModeIDs can be benchmarked too */
        ULONG mid = INVALID_ID;
        struct NameInfo ni;
        struct DimensionInfo dm;
        while ((mid = NextDisplayInfo(mid)) != INVALID_ID) {
            ni.Name[0] = 0;
            GetDisplayInfoData(NULL, (UBYTE *)&ni, sizeof(ni), DTAG_NAME, mid);
            if (!GetDisplayInfoData(NULL, (UBYTE *)&dm, sizeof(dm), DTAG_DIMS, mid) ||
                dm.Nominal.MaxX + 1 < 640 || ModeNotAvailable(mid))
                continue;
            printf("  $%08lx %-32s %dx%d depth %u\n", (unsigned long)mid, (char *)ni.Name,
                   dm.Nominal.MaxX + 1, dm.Nominal.MaxY + 1, dm.MaxDepth);
        }
        rc = 0;
        goto out;
    }
    if (!(mp = CreateMsgPort())) goto out;
    tr.tr_node.io_Message.mn_ReplyPort = mp;
    if (OpenDevice(TIMERNAME, UNIT_ECLOCK, (struct IORequest *)&tr, 0)) goto out;
    TimerBase = tr.tr_node.io_Device;

    s = OpenScreenTags(NULL, SA_DisplayID, id, SA_Depth, depth, SA_Width, 640, SA_Height, 400,
                       SA_Title, (ULONG)"PrismBench", SA_Pens, (ULONG)"\xff\xff", TAG_DONE);
    if (!s) { printf("PrismBench: can't open mode $%08lx\n", (unsigned long)id); goto out; }
    w = OpenWindowTags(NULL, WA_CustomScreen, (ULONG)s, WA_Left, 0, WA_Top, 12,
                       WA_Width, 620, WA_Height, 380, WA_Flags, WFLG_SMART_REFRESH |
                       WFLG_BORDERLESS, TAG_DONE);
    if (!w) goto out;

    printf("PrismBench: mode $%08lx, depth %ld\n", (unsigned long)id, (long)depth);
    run("RectFill 100x100", t_fill, w->RPort, "Kpix", 10);
    run("Text JAM1 40 chars", t_text1, w->RPort, "chars", 40);
    run("Text JAM2 40 chars", t_text2, w->RPort, "chars", 40);
    run("ClipBlit 200x100", t_copy, w->RPort, "Kpix", 20);
    run("ScrollRaster 400x100", t_scroll, w->RPort, "Kpix", 40);
    run("Draw ~390 px line", t_lines, w->RPort, "lines", 1);
    run("WritePixel", t_pixels, w->RPort, "pix", 1);
    run("RectFill 8x8", t_fill_small, w->RPort, "Kpix", 0);
    run("  SetAPen alone", t_setapen, w->RPort, "calls", 1);
    run("  WritePixel, no SetAPen", t_pixel_only, w->RPort, "pix", 1);
    run("  ReadPixel", t_readpixel, w->RPort, "pix", 1);
    run("  LockLayerRom+Unlock", t_locklayer, w->RPort, "pairs", 1);
    run("  Move alone", t_move, w->RPort, "calls", 1);
    run("  Line 390x2 (3 runs)", t_line_flat, w->RPort, "lines", 1);
    run("  Line 390x60 (61 runs)", t_line_steep, w->RPort, "lines", 1);
    run("  Line 30x100 (steep)", t_line_vert, w->RPort, "lines", 1);
    run("Text JAM2 4 chars", t_text_short, w->RPort, "chars", 4);
    {
        static UBYTE abuf[5 * 16];
        static struct AreaInfo ai;
        static struct TmpRas trs;
        PLANEPTR ras = AllocRaster(640, 400);
        if (ras) {
            InitArea(&ai, abuf, 16);
            InitTmpRas(&trs, ras, RASSIZE(640, 400));
            w->RPort->AreaInfo = &ai;
            w->RPort->TmpRas = &trs;
            run("Area ellipse + triangle", t_area, w->RPort, "pairs", 1);
            if (depth == 8) {
                /* exactness against graphics.library's own fill in a planar
                 * bitmap, solid and JAM2, at an odd x offset */
                struct BitMap *ab = AllocBitMap(160, 100, 8, BMF_CLEAR, NULL);
                if (ab) {
                    static UBYTE abuf2[5 * 16];
                    static struct AreaInfo ai2;
                    static struct TmpRas trs2;
                    PLANEPTR ras2 = AllocRaster(160, 100);
                    struct RastPort arp, *both[2];
                    LONG k, x, y, bad = 0;
                    InitRastPort(&arp);
                    arp.BitMap = ab;
                    both[0] = &arp;
                    both[1] = w->RPort;
                    if (ras2) {
                        InitArea(&ai2, abuf2, 16);
                        InitTmpRas(&trs2, ras2, RASSIZE(160, 100));
                        arp.AreaInfo = &ai2;
                        arp.TmpRas = &trs2;
                        SetRast(w->RPort, 0);
                        for (k = 0; k < 2; k++) {
                            struct RastPort *q = both[k];
                            WORD ox = k ? 301 : 0, oy = k ? 200 : 0;
                            SetDrMd(q, JAM1); SetAPen(q, 4); SetOPen(q, 9);
                            AreaEllipse(q, ox + 50, oy + 40, 45, 30);
                            AreaEnd(q);
                            BNDRYOFF(q);
                            SetDrMd(q, JAM2); SetAPen(q, 200); SetBPen(q, 7);
                            AreaMove(q, ox + 90, oy + 5);
                            AreaDraw(q, ox + 155, oy + 60);
                            AreaDraw(q, ox + 70, oy + 95);
                            AreaEnd(q);
                            SetDrMd(q, JAM1);
                        }
                        WaitBlit();
                        for (y = 0; y < 100; y++)
                            for (x = 0; x < 160; x++) {
                                LONG a = ReadPixel(&arp, x, y), b = ReadPixel(w->RPort, 301 + x, 200 + y);
                                if (a != b && bad++ < 3)
                                    printf("  area WRONG at %ld,%ld: native %ld, screen %ld\n",
                                           (long)x, (long)y, (long)a, (long)b);
                            }
                        printf("  area check: %ld wrong of 16000\n", (long)bad);
                        FreeRaster(ras2, 160, 100);
                    }
                    FreeBitMap(ab);
                }
            }
            w->RPort->AreaInfo = NULL;
            w->RPort->TmpRas = NULL;
            SetDrMd(w->RPort, JAM2);              /* as the later checks expect */
            SetBPen(w->RPort, 0);
            FreeRaster(ras, 640, 400);
        }
    }
    planarBM = AllocBitMap(64, 32, 8, BMF_CLEAR, NULL);
    friendBM = AllocBitMap(64, 32, 8, BMF_CLEAR, w->RPort->BitMap);
    if (planarBM && friendBM) {
        struct RastPort brp;
        InitRastPort(&brp);
        brp.BitMap = planarBM;
        SetAPen(&brp, 5); RectFill(&brp, 4, 4, 59, 27);
        brp.BitMap = friendBM;
        SetAPen(&brp, 6); RectFill(&brp, 4, 4, 59, 27);
        WaitBlit();
        run("Blit planar 64x32", t_blit_planar, w->RPort, "Kpix", 2);
        run("Blit friend 64x32", t_blit_friend, w->RPort, "Kpix", 2);
        if (depth == 8 && s->ViewPort.ColorMap && s->ViewPort.ColorMap->Count >= 256) {
            /* exactness: every pen, blitted at odd x offsets, read back
             * (8-bit-deep screens only: a shallower native screen has
             * fewer colour registers than the test sets) */
            LONG x, y, dx, bad = 0, pen;
            /* 256 colours that stay distinct even in RGB565, so a 16/24-bit
             * screen's ReadPixel (colour back to pen) is exact */
            for (pen = 0; pen < 256; pen++)
                SetRGB32(&s->ViewPort, pen, (ULONG)((pen & 31) << 3) << 24,
                         (ULONG)((pen >> 5) << 5) << 24, 0x40000000);
            brp.BitMap = planarBM;
            for (y = 0; y < 32; y++)
                for (x = 0; x < 64; x++) {
                    SetAPen(&brp, (x * 7 + y * 37) & 255);
                    WritePixel(&brp, x, y);
                }
            for (dx = 0; dx < 4; dx++) {
                BltBitMapRastPort(planarBM, dx, 0, w->RPort, 421 + dx * 3, 220, 61, 32, 0xc0);
                WaitBlit();
                for (y = 0; y < 32; y++)
                    for (x = 0; x < 61; x++) {
                        pen = ReadPixel(w->RPort, 421 + dx * 3 + x, 220 + y);
                        if (pen != ((x + dx) * 7 + y * 37) % 256 && bad++ < 3)
                            printf("  planar blit WRONG at %ld,%ld (shift %ld): %ld\n",
                                   (long)x, (long)y, (long)dx, (long)pen);
                    }
            }
            /* and whole 32-pixel groups to an even address (the path that
             * converts straight into the bitmap) */
            BltBitMapRastPort(planarBM, 0, 0, w->RPort, 420, 260, 64, 32, 0xc0);
            WaitBlit();
            for (y = 0; y < 32; y++)
                for (x = 0; x < 64; x++) {
                    pen = ReadPixel(w->RPort, 420 + x, 260 + y);
                    if (pen != (x * 7 + y * 37) % 256 && bad++ < 3)
                        printf("  planar blit WRONG at %ld,%ld (aligned): %ld\n",
                               (long)x, (long)y, (long)pen);
                }
            printf("  planar blit check: %ld wrong of %ld\n", (long)bad, 4L * 61 * 32 + 64 * 32);
        }
    }
    if (depth == 8) {
        /* line exactness: graphics.library's own lines in a planar bitmap
         * against the same lines drawn into the screen window */
        struct BitMap *lb = AllocBitMap(160, 100, 8, BMF_CLEAR, NULL);
        if (lb) {
            struct RastPort lrp;
            LONG k, x, y, bad = 0;
            static const WORD ln[][4] = {
                { 0, 0, 159, 99 }, { 0, 99, 159, 0 }, { 5, 50, 150, 52 }, { 80, 2, 82, 97 },
                { 10, 10, 10, 90 }, { 2, 30, 157, 30 }, { 150, 95, 3, 7 }, { 40, 90, 120, 3 },
                { 0, 0, 159, 20 }, { 159, 0, 0, 60 }, { 30, 0, 31, 99 }, { 70, 70, 71, 71 },
                /* exact ties (2 x short = long), each direction */
                { 0, 0, 100, 50 }, { 120, 90, 20, 40 }, { 0, 99, 40, 19 }, { 150, 0, 110, 80 },
                /* short ties (drawn by the CPU even with a line blitter) */
                { 100, 60, 140, 80 }, { 140, 95, 100, 75 }, { 60, 99, 50, 79 }, { 20, 60, 30, 80 },
            };
            InitRastPort(&lrp);
            lrp.BitMap = lb;
            SetRast(w->RPort, 0);
            for (k = 0; k < (LONG)(sizeof(ln) / sizeof(ln[0])); k++) {
                SetAPen(&lrp, 1 + k); Move(&lrp, ln[k][0], ln[k][1]); Draw(&lrp, ln[k][2], ln[k][3]);
                SetAPen(w->RPort, 1 + k);
                Move(w->RPort, 300 + ln[k][0], 200 + ln[k][1]);
                Draw(w->RPort, 300 + ln[k][2], 200 + ln[k][3]);
            }
            /* patterned ties: the general per-pixel path */
            SetDrPt(&lrp, 0xf0f0); SetDrPt(w->RPort, 0xf0f0);
            SetAPen(&lrp, 30); SetAPen(w->RPort, 30);
            Move(&lrp, 60, 5); Draw(&lrp, 140, 45);
            Move(w->RPort, 360, 205); Draw(w->RPort, 440, 245);
            Move(&lrp, 155, 90); Draw(&lrp, 125, 30);
            Move(w->RPort, 455, 290); Draw(w->RPort, 425, 230);
            SetDrPt(&lrp, 0xffff); SetDrPt(w->RPort, 0xffff);
            WaitBlit();
            for (y = 0; y < 100; y++)
                for (x = 0; x < 160; x++) {
                    LONG a = ReadPixel(&lrp, x, y), b = ReadPixel(w->RPort, 300 + x, 200 + y);
                    if (a != b && bad++ < 3)
                        printf("  line WRONG at %ld,%ld: native %ld, screen %ld\n",
                               (long)x, (long)y, (long)a, (long)b);
                }
            printf("  line check: %ld wrong of 16000\n", (long)bad);

            /* text exactness: JAM1 over a coloured background, JAM2, and
             * JAM2 + INVERSVID, native (planar) against the screen */
            {
                static const UBYTE modes[3] = { JAM1, JAM2, JAM2 | INVERSVID };
                bad = 0;
                SetAPen(&lrp, 3); RectFill(&lrp, 0, 0, 159, 99);
                SetAPen(w->RPort, 3); RectFill(w->RPort, 300, 200, 459, 299);
                SetFont(&lrp, w->RPort->Font);
                for (k = 0; k < 3; k++) {
                    SetDrMd(&lrp, modes[k]); SetAPen(&lrp, 5 + k); SetBPen(&lrp, 9 + k);
                    Move(&lrp, 3 + k, 12 + k * 20 + w->RPort->TxBaseline);
                    Text(&lrp, line, 19);
                    SetDrMd(w->RPort, modes[k]); SetAPen(w->RPort, 5 + k); SetBPen(w->RPort, 9 + k);
                    Move(w->RPort, 303 + k, 212 + k * 20 + w->RPort->TxBaseline);
                    Text(w->RPort, line, 19);
                }
                /* JAM1 at four x alignments and in four pens, white among
                 * them (IBrowse's page text on the A2000 came out striped
                 * at 16 bits: transparent expansion and the destination's
                 * alignment or the colour's bytes) */
                for (k = 0; k < 4; k++) {
                    static const UBYTE tp[4] = { 2, 200, 255, 31 };   /* 200, 255: bytes from $80 up */
                    SetDrMd(&lrp, JAM1); SetAPen(&lrp, tp[k]);
                    Move(&lrp, 4 + k * 13, 62 + k * 9 + w->RPort->TxBaseline);
                    Text(&lrp, line, 8);
                    SetDrMd(w->RPort, JAM1); SetAPen(w->RPort, tp[k]);
                    Move(w->RPort, 304 + k * 13, 262 + k * 9 + w->RPort->TxBaseline);
                    Text(w->RPort, line, 8);
                }
                WaitBlit();
                for (y = 0; y < 100; y++)
                    for (x = 0; x < 160; x++) {
                        LONG a = ReadPixel(&lrp, x, y), b = ReadPixel(w->RPort, 300 + x, 200 + y);
                        if (a != b && bad++ < 3)
                            printf("  text WRONG at %ld,%ld: native %ld, screen %ld\n",
                                   (long)x, (long)y, (long)a, (long)b);
                    }
                printf("  text check: %ld wrong of 16000\n", (long)bad);
                if (bad) {
                    /* what is there: the first text row's area, native then screen */
                    for (k = 0; k < 2; k++)
                        for (y = 11; y < 21; y++) {
                            for (x = 0; x < 40; x++) {
                                LONG v = k ? ReadPixel(w->RPort, 300 + x, 200 + y) : ReadPixel(&lrp, x, y);
                                printf("%02lx", (long)v & 0xff);
                            }
                            printf(k ? " S\n" : " N\n");
                        }
                }
            }

            /* fill exactness, straight after text (the blitter's registers
             * are as a text expansion left them): a blitter-sized and a
             * tiny rectangle, each checked with a 2-pixel ring around it */
            {
                static const WORD fr[2][4] = { { 311, 213, 347, 235 }, { 400, 250, 402, 252 } };
                LONG total = 0;
                bad = 0;
                SetDrMd(w->RPort, JAM1);
                SetAPen(w->RPort, 3); RectFill(w->RPort, 300, 200, 459, 299);
                SetDrMd(w->RPort, JAM2); SetAPen(w->RPort, 6); SetBPen(w->RPort, 9);
                Move(w->RPort, 303, 285 + w->RPort->TxBaseline);
                Text(w->RPort, line, 10);
                for (k = 0; k < 2; k++) {
                    SetAPen(w->RPort, 77 + k);
                    RectFill(w->RPort, fr[k][0], fr[k][1], fr[k][2], fr[k][3]);
                }
                WaitBlit();
                for (k = 0; k < 2; k++)
                    for (y = fr[k][1] - 2; y <= fr[k][3] + 2; y++)
                        for (x = fr[k][0] - 2; x <= fr[k][2] + 2; x++, total++) {
                            LONG in = x >= fr[k][0] && x <= fr[k][2] && y >= fr[k][1] && y <= fr[k][3];
                            LONG got = ReadPixel(w->RPort, x, y);
                            if (got != (in ? 77 + k : 3) && bad++ < 3)
                                printf("  fill WRONG at %ld,%ld: %ld\n", (long)x, (long)y, (long)got);
                        }
                printf("  fill check: %ld wrong of %ld\n", (long)bad, (long)total);
            }
            FreeBitMap(lb);
        }
    }
    if (planarBM) FreeBitMap(planarBM);
    if (friendBM) FreeBitMap(friendBM);
    rc = 0;
out:
    if (w) CloseWindow(w);
    if (s) CloseScreen(s);
    if (TimerBase) CloseDevice((struct IORequest *)&tr);
    if (mp) DeleteMsgPort(mp);
    if (IntuitionBase) CloseLibrary((struct Library *)IntuitionBase);
    if (GfxBase) CloseLibrary((struct Library *)GfxBase);
    FreeArgs(rda);
    return rc;
}
