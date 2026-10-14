/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laine Jones */
/* mirrortest: a chipset screen the way old programs make one - its own
 * bitmap (CUSTOMBITMAP), drawn by writing the bitplanes directly - for
 * MIRRORCHIPSET. Vertical colour bars, one per pen, then a white block
 * that moves across them for a while (the copy must follow), then it
 * stays still until SECS are up.
 *
 *   mirrortest [DEPTH=5] [EHB] [SECS=20] [MOVE=4]
 *
 * Pen n is (n*8, 255-n*8, n*4) in the first 32; with EHB (6 planes) pens
 * 32-63 are the first 32 at half brightness and the bars show both. */
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <graphics/gfx.h>
#include <graphics/view.h>
#include <graphics/modeid.h>
#include <intuition/screens.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <stdio.h>
#include <string.h>

#define W 320
#define H 256

static void put(struct BitMap *bm, int depth, int x, int y, int pen)
{
    int k;
    for (k = 0; k < depth; k++) {
        UBYTE *b = bm->Planes[k] + y * bm->BytesPerRow + (x >> 3);
        if (pen & (1 << k)) *b |= 0x80 >> (x & 7);
        else                *b &= ~(0x80 >> (x & 7));
    }
}

int main(void)
{
    struct RDArgs *ra;
    LONG args[4] = { 0, 0, 0, 0 };
    int depth = 5, secs = 20, moves = 4, pens, x, y, i, k, bx;
    BOOL ehb;
    struct BitMap bm;
    struct Screen *s;
    ULONG modeid;

    if ((ra = ReadArgs("DEPTH/K/N,EHB/S,SECS/K/N,MOVE/K/N", args, NULL))) {
        if (args[0]) depth = *(LONG *)args[0];
        if (args[2]) secs = *(LONG *)args[2];
        if (args[3]) moves = *(LONG *)args[3];
        FreeArgs(ra);
    }
    ehb = args[1] != 0;
    if (ehb) depth = 6;
    if (depth < 1 || depth > 6) depth = 5;
    pens = ehb ? 64 : 1 << depth;
    InitBitMap(&bm, depth, W, H);
    for (k = 0; k < depth; k++)
        if (!(bm.Planes[k] = AllocRaster(W, H))) {
            puts("mirrortest: no chip memory");
            return 20;
        } else
            memset(bm.Planes[k], 0, RASSIZE(W, H));
    modeid = PAL_MONITOR_ID | LORES_KEY | (ehb ? EXTRAHALFBRITE_KEY : 0);
    s = OpenScreenTags(NULL, SA_DisplayID, modeid, SA_Width, W, SA_Height, H, SA_Depth, depth,
                       SA_BitMap, (ULONG)&bm, SA_Quiet, TRUE, SA_ShowTitle, FALSE, TAG_DONE);
    if (!s) {
        puts("mirrortest: can't open the screen");
        return 20;
    }
    for (i = 0; i < 32 && i < pens; i++)
        SetRGB32(&s->ViewPort, i, (ULONG)(i * 8) << 24, (ULONG)(255 - i * 8) << 24, (ULONG)(i * 4) << 24);
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++)
            put(&bm, depth, x, y, (x * pens / W) % pens);
    printf("mirrortest: %dx%d, %d planes%s, %d bars, screen %lx\n", W, H, depth, ehb ? " EHB" : "",
           pens, (unsigned long)s);
    fflush(stdout);
    /* a 16x16 block of pen 31 (or the last) moving right, one step a second */
    for (i = 0, bx = 16; i < moves; i++, bx += 32) {
        Delay(50);
        for (y = 100; y < 116; y++)
            for (x = bx - 32; x < bx + 16; x++)
                if (x >= 0 && x < W)
                    put(&bm, depth, x, y, x >= bx ? (pens > 31 ? 31 : pens - 1) : (x * pens / W) % pens);
    }
    printf("mirrortest: block at x %d, y 100-115\n", bx - 32);
    fflush(stdout);
    Delay(50 * (secs > moves ? secs - moves : 1));
    CloseScreen(s);
    for (k = 0; k < depth; k++)
        FreeRaster(bm.Planes[k], W, H);
    return 0;
}
