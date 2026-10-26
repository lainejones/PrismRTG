/* SPDX-License-Identifier: GPL-3.0-only */
/* pencheck - the Workbench screen's depth and the colours of its first and
 * last four pens. A complemented icon (no selected image of its own) goes
 * to pens 2^depth-4 .. 2^depth-1, so they must hold Palette prefs' colours
 * 4-7 for it to look right. Run it on a native and on a Prism screen. */
#include <exec/types.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <graphics/gfx.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <stdio.h>

struct IntuitionBase *IntuitionBase;
struct GfxBase *GfxBase;

static void pen(struct ColorMap *cm, ULONG n)
{
    ULONG rgb[3];
    GetRGB32(cm, n, 1, rgb);
    printf("  pen %3lu: %02lx %02lx %02lx\n", (unsigned long)n, (unsigned long)(rgb[0] >> 24),
           (unsigned long)(rgb[1] >> 24), (unsigned long)(rgb[2] >> 24));
}

int main(void)
{
    struct Screen *s;
    struct ColorMap *cm;
    ULONG depth, n, i;

    IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 39);
    GfxBase = (struct GfxBase *)OpenLibrary("graphics.library", 39);
    if (!IntuitionBase || !GfxBase || !(s = LockPubScreen(NULL))) {
        printf("pencheck: needs OS 3.0 and the Workbench screen\n");
        return 20;
    }
    cm = s->ViewPort.ColorMap;
    depth = GetBitMapAttr(s->RastPort.BitMap, BMA_DEPTH);
    n = depth >= 8 ? 256 : 1UL << depth;
    printf("screen depth %lu (BitMap->Depth %u), ColorMap count %u, mode %08lx\n",
           (unsigned long)depth, s->RastPort.BitMap->Depth, cm->Count,
           (unsigned long)GetVPModeID(&s->ViewPort));
    for (i = 0; i < 4; i++)
        pen(cm, i);
    if (n > 8)
        for (i = 4; i < 8; i++)
            pen(cm, i);
    for (i = n - 4; i < n; i++)
        pen(cm, i);
    UnlockPubScreen(NULL, s);
    CloseLibrary((struct Library *)GfxBase);
    CloseLibrary((struct Library *)IntuitionBase);
    return 0;
}
