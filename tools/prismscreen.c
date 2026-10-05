/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laine Jones */
/*
 * PrismScreen - M1 client test. Needs PrismD running.
 *
 *   PrismScreen LIST                list the display database
 *   PrismScreen [MODEID=$7A000001] [SECS=10]
 *
 * Opens an Intuition screen on a Prism mode, sets its palette with the
 * normal LoadRGB32() call, draws a test pattern straight into the screen's
 * VRAM (M1 has no graphics.library rendering yet), sends the screen to the
 * back and brings it forward again, then closes it.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <graphics/displayinfo.h>
#include <graphics/gfxmacros.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <proto/asl.h>
#include <libraries/asl.h>
#include <stdio.h>
#include <stdlib.h>
#include "prismboard.h"
#include "prism.h"

struct GfxBase *GfxBase;
struct IntuitionBase *IntuitionBase;
struct Library *AslBase;

#define TEMPLATE "LIST/S,MODEID/K,SECS/K/N,REQ/S,QUIT/S,GFX/S,CGX/S,DBUF/S,MANY/S"
enum { A_LIST, A_MODEID, A_SECS, A_REQ, A_QUIT, A_GFX, A_CGX, A_DBUF, A_MANY, A_COUNT };

int dbuf_test(ULONG id, ULONG secs);     /* m8tests.c */
int many_test(ULONG id, ULONG secs);

static int cgx_test(ULONG secs);

static int gfx_test(ULONG id, ULONG secs);

static void list_modes(void)
{
    ULONG id = INVALID_ID, n = 0, ours = 0;
    struct NameInfo ni;
    struct DimensionInfo dm;

    while ((id = NextDisplayInfo(id)) != INVALID_ID) {
        n++;
        if (!IS_PRISM_ID(id))
            continue;
        ours++;
        ni.Name[0] = 0;
        GetDisplayInfoData(NULL, &ni, sizeof(ni), DTAG_NAME, id);
        GetDisplayInfoData(NULL, &dm, sizeof(dm), DTAG_DIMS, id);
        printf("  $%08lx %-24s %dx%d depth %u, available=%s\n", (unsigned long)id,
               ni.Name, dm.Nominal.MaxX + 1, dm.Nominal.MaxY + 1, dm.MaxDepth,
               ModeNotAvailable(id) ? "no" : "yes");
    }
    printf("%lu display ids, %lu Prism\n", (unsigned long)n, (unsigned long)ours);
}

static void pattern8(struct PrismScreenInfo *si)
{
    int x, y, w = si->width, h = si->height;
    for (y = 0; y < h; y++) {
        UBYTE *row = si->vram + y * si->bytesPerRow;
        for (x = 0; x < w; x++) {
            UBYTE c;
            if (y < h / 2)
                c = 16 + x * 112 / w;              /* hue ramp, pens 16-127  */
            else
                c = (((x >> 5) ^ (y >> 5)) & 1) ? 1 : 0;
            if (x == 0 || y == 0 || x == w - 1 || y == h - 1 || x * h == y * w)
                c = 1;
            row[x] = c;
        }
    }
}

static ULONG palette[1 + 128 * 3 + 1];

static void make_palette(void)
{
    int i;
    palette[0] = (128UL << 16) | 0;            /* 128 colours from pen 0 */
    for (i = 0; i < 128; i++) {
        ULONG r, g, b;
        if (i == 0)       { r = g = b = 0; }
        else if (i == 1)  { r = g = b = 255; }
        else if (i < 16)  { r = g = b = i * 16; }
        else {
            int hh = (i - 16) * 6 * 256 / 112, s = hh % 256, seg = hh / 256;
            switch (seg) {
            case 0:  r = 255;     g = s;       b = 0;       break;
            case 1:  r = 255 - s; g = 255;     b = 0;       break;
            case 2:  r = 0;       g = 255;     b = s;       break;
            case 3:  r = 0;       g = 255 - s; b = 255;     break;
            case 4:  r = s;       g = 0;       b = 255;     break;
            default: r = 255;     g = 0;       b = 255 - s; break;
            }
        }
        palette[1 + i * 3]     = r * 0x01010101UL;
        palette[1 + i * 3 + 1] = g * 0x01010101UL;
        palette[1 + i * 3 + 2] = b * 0x01010101UL;
    }
    palette[1 + 128 * 3] = 0;
}

int main(void)
{
    LONG args[A_COUNT] = { 0 };
    struct RDArgs *rda;
    struct PrismSem *ps;
    struct PrismScreenInfo si;
    struct Screen *s;
    ULONG id = PRISM_MONITOR_ID | 1, secs = 10;
    int rc = 20;

    setvbuf(stdout, NULL, _IONBF, 0);
    if (!(rda = ReadArgs(TEMPLATE, args, NULL))) {
        PrintFault(IoErr(), "PrismScreen");
        return 20;
    }
    GfxBase = (struct GfxBase *)OpenLibrary("graphics.library", 39);
    IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 39);
    if (!GfxBase || !IntuitionBase) goto out;

    Forbid();
    ps = (struct PrismSem *)FindSemaphore(PRISM_SEMNAME);
    Permit();
    if (!ps) {
        printf("PrismScreen: PrismD is not running\n");
        rc = 5;
        goto out;
    }
    printf("PrismScreen: Prism v%u on %s, %lu modes\n", ps->version, ps->boardName,
           (unsigned long)ps->numModes);

    if (args[A_QUIT]) {
        Signal(ps->task, SIGBREAKF_CTRL_C);
        printf("PrismScreen: asked PrismD to quit\n");
        rc = 0;
        goto out;
    }
    if (args[A_LIST]) {
        list_modes();
        rc = 0;
        goto out;
    }
    if (args[A_MODEID]) {
        const char *p = (const char *)args[A_MODEID];
        if (*p == '$') p++;
        id = strtoul(p, NULL, 16);
    }
    if (args[A_SECS]) secs = *(LONG *)args[A_SECS];

    if (args[A_REQ]) {
        /* the standard ASL screen-mode requester, preselected on a Prism
         * mode so its list scrolls there */
        struct ScreenModeRequester *rq;
        if ((AslBase = OpenLibrary("asl.library", 39)) &&
            (rq = AllocAslRequestTags(ASL_ScreenModeRequest,
                                      ASLSM_InitialDisplayID, id,
                                      ASLSM_DoWidth, TRUE, ASLSM_DoHeight, TRUE,
                                      ASLSM_DoDepth, TRUE, TAG_DONE))) {
            if (AslRequest(rq, NULL))
                printf("PrismScreen: picked $%08lx %lux%lu x%u\n",
                       (unsigned long)rq->sm_DisplayID, (unsigned long)rq->sm_DisplayWidth,
                       (unsigned long)rq->sm_DisplayHeight, rq->sm_DisplayDepth);
            else
                printf("PrismScreen: requester cancelled\n");
            FreeAslRequest(rq);
        }
        if (AslBase) CloseLibrary(AslBase);
        rc = 0;
        goto out;
    }

    if (args[A_CGX]) {
        rc = cgx_test(secs);
        goto out;
    }
    if (args[A_DBUF]) {
        rc = dbuf_test(id, secs);
        goto out;
    }
    if (args[A_MANY]) {
        rc = many_test(id, secs);
        goto out;
    }
    if (args[A_GFX]) {
        rc = gfx_test(id, secs);
        goto out;
    }
    printf("PrismScreen: opening $%08lx\n", (unsigned long)id);
    s = OpenScreenTags(NULL, SA_DisplayID, id, SA_Depth, 8,
                       SA_Title, (ULONG)"Prism M1", SA_ShowTitle, FALSE,
                       SA_Quiet, TRUE, TAG_DONE);
    if (!s) {
        printf("PrismScreen: OpenScreen failed\n");
        goto out;
    }
    if (!ps->screenInfo(s, &si)) {
        printf("PrismScreen: not a Prism screen?\n");
        CloseScreen(s);
        goto out;
    }
    printf("PrismScreen: %dx%d, VRAM $%08lx, %lu bytes/row\n", si.width, si.height,
           (unsigned long)si.vram, (unsigned long)si.bytesPerRow);

    make_palette();
    LoadRGB32(&s->ViewPort, palette);
    pattern8(&si);
    printf("PrismScreen: pattern drawn, screen in front\n");
    Delay(secs * 25);

    ScreenToBack(s);
    printf("PrismScreen: screen to back (native display)\n");
    Delay(secs * 25);
    ScreenToFront(s);
    printf("PrismScreen: screen to front again\n");
    Delay(secs * 25);

    CloseScreen(s);
    printf("PrismScreen: closed\n");
    rc = 0;
out:
    if (IntuitionBase) CloseLibrary((struct Library *)IntuitionBase);
    if (GfxBase) CloseLibrary((struct Library *)GfxBase);
    FreeArgs(rda);
    return rc;
}

/* ---- GFX: graphics.library rendering on a Prism screen (M2) ------------ */

static UWORD ditherPat[] = { 0xaaaa, 0x5555 };

static void draw_stuff(struct Window *w)
{
    struct RastPort *rp = w->RPort;
    WORD x0 = w->BorderLeft, y0 = w->BorderTop;
    WORD x1 = w->Width - w->BorderRight - 1, y1 = w->Height - w->BorderBottom - 1;
    WORD i;

    SetAPen(rp, 3);                         /* blue box                      */
    RectFill(rp, x0 + 10, y0 + 10, x0 + 110, y0 + 60);
    SetAPen(rp, 1);                         /* lines in pen 1                */
    for (i = 0; i < 10; i++) {
        Move(rp, x0 + 120, y0 + 10 + i * 5);
        Draw(rp, x1 - 10, y0 + 10 + i * 9);
    }
    SetAPen(rp, 2);                         /* dithered fill                 */
    SetAfPt(rp, ditherPat, 1);
    RectFill(rp, x0 + 10, y0 + 70, x0 + 110, y0 + 110);
    SetAfPt(rp, NULL, 0);
    SetAPen(rp, 1); SetBPen(rp, 0);         /* JAM1 text                     */
    SetDrMd(rp, JAM1);
    Move(rp, x0 + 10, y0 + 130);
    Text(rp, "Prism M2: JAM1 text", 19);
    SetAPen(rp, 2); SetBPen(rp, 3);         /* JAM2 text                     */
    SetDrMd(rp, JAM2);
    Move(rp, x0 + 10, y0 + 145);
    Text(rp, "JAM2 text on blue", 17);
    SetDrMd(rp, COMPLEMENT);                /* complement box over the lines */
    RectFill(rp, x0 + 200, y0 + 20, x0 + 260, y0 + 50);
    SetDrMd(rp, JAM1);
    (void)y1;
}

static int gfx_test(ULONG id, ULONG secs)
{
    struct Screen *s;
    struct Window *w1, *w2;

    s = OpenScreenTags(NULL, SA_DisplayID, id, SA_Depth, 8,
                       SA_Title, (ULONG)"Prism M2 - graphics.library on the card",
                       SA_ShowTitle, TRUE, SA_Pens, (ULONG)"\xff\xff",
                       SA_FullPalette, TRUE, TAG_DONE);
    if (!s) {
        printf("PrismScreen: OpenScreen failed\n");
        return 20;
    }
    w1 = OpenWindowTags(NULL, WA_CustomScreen, (ULONG)s, WA_Left, 20, WA_Top, 30,
                        WA_Width, 380, WA_Height, 220, WA_Title, (ULONG)"Smart refresh",
                        WA_Flags, WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_CLOSEGADGET |
                                  WFLG_SIZEGADGET | WFLG_SMART_REFRESH | WFLG_ACTIVATE,
                        TAG_DONE);
    w2 = OpenWindowTags(NULL, WA_CustomScreen, (ULONG)s, WA_Left, 260, WA_Top, 120,
                        WA_Width, 300, WA_Height, 160, WA_Title, (ULONG)"Second window",
                        WA_Flags, WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_SIZEGADGET |
                                  WFLG_SMART_REFRESH, TAG_DONE);
    printf("PrismScreen: GFX screen %lx (bar layer %lx), windows %lx %lx\n",
           (unsigned long)s, (unsigned long)s->BarLayer,
           (unsigned long)w1, (unsigned long)w2);
    if (w1) draw_stuff(w1);
    if (w2) draw_stuff(w2);
    printf("PrismScreen: drawn\n");
    {
        /* Anything graphics.library drew that Prism did not intercept went
         * into the screen bitmap's dummy plane: report where. */
        struct BitMap *bm = s->RastPort.BitMap;
        UBYTE *pl = bm->Planes[0];
        WORD y, x, first = -1, last = -1, n = 0;
        for (y = 0; y < bm->Rows; y++) {
            for (x = 0; x < bm->BytesPerRow; x++)
                if (pl[(LONG)y * bm->BytesPerRow + x]) {
                    if (first < 0) first = y;
                    last = y;
                    n++;
                    break;
                }
        }
        printf("PrismScreen: dummy plane has drawing on %d rows (%d..%d)\n", n, first, last);
        if (first >= 0) {
            for (y = first; y <= last && y < first + 12; y++) {
                printf("  row %3d:", y);
                for (x = 0; x < 40 && x < bm->BytesPerRow; x++)
                    printf("%02x", pl[(LONG)y * bm->BytesPerRow + x]);
                printf("\n");
            }
        }
    }
    Delay(secs * 25);
    if (w2) {
        MoveWindow(w2, -200, -60);          /* uncover part of w1            */
        printf("PrismScreen: moved window 2\n");
    }
    Delay(secs * 25);
    if (w2) CloseWindow(w2);
    if (w1) CloseWindow(w1);
    CloseScreen(s);
    printf("PrismScreen: closed\n");
    return 0;
}

/* ---- CGX: cybergraphics.library on a Prism screen (M4) ---------------- */

#include <cybergraphx/cybergraphics.h>
#include <proto/cybergraphics.h>

struct Library *CyberGfxBase;

static int cgx_test(ULONG secs)
{
    struct Screen *s;
    struct Window *w;
    ULONG id, bpr = 0, base = 0, fmt = 0, lw = 0, lh = 0;
    APTR h;
    UBYTE *rgb;
    WORD x, y;

    if (!(CyberGfxBase = OpenLibrary("cybergraphics.library", 41))) {
        printf("PrismScreen: no cybergraphics.library 41\n");
        return 20;
    }
    printf("PrismScreen: %s\n", (char *)CyberGfxBase->lib_IdString);
    id = BestCModeIDTags(CYBRBIDTG_Depth, 24, CYBRBIDTG_NominalWidth, 640,
                         CYBRBIDTG_NominalHeight, 480, TAG_DONE);
    printf("PrismScreen: BestCModeID(24-bit 640x480) = $%08lx, IsCyberModeID %d, %lu-bit fmt %lu\n",
           (unsigned long)id, IsCyberModeID(id), (unsigned long)GetCyberIDAttr(CYBRIDATTR_DEPTH, id),
           (unsigned long)GetCyberIDAttr(CYBRIDATTR_PIXFMT, id));
    s = OpenScreenTags(NULL, SA_DisplayID, id, SA_Depth, 8, SA_Title, (ULONG)"Prism M4 - cybergraphics.library",
                       SA_Pens, (ULONG)"\xff\xff", TAG_DONE);
    if (!s) { CloseLibrary(CyberGfxBase); return 20; }
    w = OpenWindowTags(NULL, WA_CustomScreen, (ULONG)s, WA_Left, 20, WA_Top, 20, WA_Width, 600,
                       WA_Height, 440, WA_Title, (ULONG)"WritePixelArray", WA_Flags,
                       WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_SMART_REFRESH, TAG_DONE);
    printf("PrismScreen: screen bitmap: cybergfx %ld, %lu-bit, %lu bytes/row\n",
           (long)GetCyberMapAttr(s->RastPort.BitMap, CYBRMATTR_ISCYBERGFX),
           (unsigned long)GetCyberMapAttr(s->RastPort.BitMap, CYBRMATTR_DEPTH),
           (unsigned long)GetCyberMapAttr(s->RastPort.BitMap, CYBRMATTR_XMOD));
    if ((h = LockBitMapTags(s->RastPort.BitMap, LBMI_BYTESPERROW, (ULONG)&bpr, LBMI_BASEADDRESS,
                            (ULONG)&base, LBMI_PIXFMT, (ULONG)&fmt, LBMI_WIDTH, (ULONG)&lw,
                            LBMI_HEIGHT, (ULONG)&lh, TAG_DONE))) {
        UnLockBitMap(h);
        printf("PrismScreen: LockBitMap: %lux%lu fmt %lu base $%08lx bpr %lu\n", (unsigned long)lw,
               (unsigned long)lh, (unsigned long)fmt, (unsigned long)base, (unsigned long)bpr);
    }
    /* a 256x256 RGB gradient: red across, green down, blue fixed */
    if (w && (rgb = AllocVec(256 * 256 * 3, MEMF_ANY))) {
        for (y = 0; y < 256; y++)
            for (x = 0; x < 256; x++) {
                UBYTE *p = rgb + (y * 256 + x) * 3;
                p[0] = x; p[1] = y; p[2] = 160;
            }
        WritePixelArray(rgb, 0, 0, 256 * 3, w->RPort, w->BorderLeft + 10, w->BorderTop + 10,
                        256, 256, RECTFMT_RGB);
        FreeVec(rgb);
        FillPixelArray(w->RPort, w->BorderLeft + 280, w->BorderTop + 10, 120, 60, 0x00ff8000);
        FillPixelArray(w->RPort, w->BorderLeft + 280, w->BorderTop + 80, 120, 60, 0x00008080);
        for (x = 0; x < 200; x++)
            WriteRGBPixel(w->RPort, w->BorderLeft + 280 + x, w->BorderTop + 160 + x / 4,
                          0x00ffff00);
        printf("PrismScreen: ReadRGBPixel at the fill = %08lx\n",
               (unsigned long)ReadRGBPixel(w->RPort, w->BorderLeft + 300, w->BorderTop + 20));
    }
    Delay(secs * 50);
    if (w) CloseWindow(w);
    CloseScreen(s);
    CloseLibrary(CyberGfxBase);
    printf("PrismScreen: closed\n");
    return 0;
}
