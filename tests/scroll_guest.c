/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laine Jones */
/* scroll_guest : ScrollRaster in a smart-refresh window that another window
 * partly covers. Content has to move between visible pieces and backing
 * store. After the cover goes away every pixel is checked. */
#include <exec/types.h>
#include <intuition/intuition.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/dos.h>
#include <stdio.h>

struct IntuitionBase *IntuitionBase;
struct GfxBase *GfxBase;

static UBYTE pat(LONG x, LONG y) { return 1 + ((x / 5 + y / 3) % 3); }

static LONG run(WORD dx, WORD dy)
{
    struct Window *a, *b;
    struct RastPort *rp;
    LONG x, y, w, h, x0, y0, bad = 0, first = -1;
    a = OpenWindowTags(NULL, WA_Left, 10, WA_Top, 20, WA_Width, 300, WA_Height, 200,
                       WA_SmartRefresh, TRUE, WA_Title, (ULONG)"scrolltest", TAG_END);
    if (!a) return -1;
    rp = a->RPort;
    x0 = a->BorderLeft; y0 = a->BorderTop;
    w = a->Width - a->BorderLeft - a->BorderRight;
    h = a->Height - a->BorderTop - a->BorderBottom;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            SetAPen(rp, pat(x, y));
            WritePixel(rp, x0 + x, y0 + y);
        }
    b = OpenWindowTags(NULL, WA_Left, 90, WA_Top, 70, WA_Width, 120, WA_Height, 80,
                       WA_SmartRefresh, TRUE, WA_Title, (ULONG)"cover", TAG_END);
    SetBPen(rp, 0);
    ScrollRaster(rp, dx, dy, x0, y0, x0 + w - 1, y0 + h - 1);
    if (b) CloseWindow(b);
    Delay(10);
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            LONG sx = x + dx, sy = y + dy, want, got;
            want = (sx >= 0 && sx < w && sy >= 0 && sy < h) ? pat(sx, sy) : 0;
            got = ReadPixel(rp, x0 + x, y0 + y);
            if (got != want) {
                if (first < 0) first = y * 10000 + x;
                bad++;
            }
        }
    CloseWindow(a);
    printf("scroll %d,%d: %ld wrong", (int)dx, (int)dy, (long)bad);
    if (first >= 0) printf(" (first at %ld,%ld)", (long)(first % 10000), (long)(first / 10000));
    printf("\n");
    return bad;
}

int main(void)
{
    LONG bad = 0;
    IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 39);
    GfxBase = (struct GfxBase *)OpenLibrary("graphics.library", 39);
    if (!IntuitionBase || !GfxBase) return 20;
    bad += run(0, 7);
    bad += run(0, -9);
    bad += run(11, 0);
    bad += run(-6, 5);
    printf("scroll_guest: %s\n", bad ? "FAIL" : "ok");
    CloseLibrary((struct Library *)GfxBase);
    CloseLibrary((struct Library *)IntuitionBase);
    return bad ? 5 : 0;
}
