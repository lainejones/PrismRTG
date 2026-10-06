/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Laine Jones */
/* stftest: how long does bringing a screen to the front take? Opens two
 * screens and flips between them, then draws ellipses, and prints rates.
 *
 *   stftest [A=$7a000001] [B=$7a000001] [N=40]
 *
 * A and B are the two screens' mode IDs (two native modes give the
 * machine's own figure to compare with).
 */
#include <exec/types.h>
#include <dos/rdargs.h>
#include <devices/timer.h>
#include <graphics/gfx.h>
#include <graphics/gfxmacros.h>
#include <intuition/screens.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <proto/timer.h>
#include <stdio.h>
#include <stdlib.h>

struct Device *TimerBase;

static ULONG now_ms(void)
{
    struct timeval tv;
    GetSysTime(&tv);
    return tv.tv_secs * 1000 + tv.tv_micro / 1000;
}

int main(void)
{
    LONG args[3] = { 0 };
    struct RDArgs *rda;
    struct Screen *a, *b;
    struct timerequest tr;
    ULONG ida = 0x7a000001, idb = 0x7a000001, t;
    LONG n = 40, i;

    if (!(rda = ReadArgs((STRPTR)"A/K,B/K,N/K/N", args, NULL)))
        return 20;
    if (args[0]) ida = strtoul((char *)args[0] + (*(char *)args[0] == '$'), NULL, 16);
    if (args[1]) idb = strtoul((char *)args[1] + (*(char *)args[1] == '$'), NULL, 16);
    if (args[2]) n = *(LONG *)args[2];
    if (OpenDevice((STRPTR)TIMERNAME, UNIT_VBLANK, (struct IORequest *)&tr, 0)) {
        FreeArgs(rda);
        return 20;
    }
    TimerBase = tr.tr_node.io_Device;
    a = OpenScreenTags(NULL, SA_DisplayID, ida, SA_Depth, 8, SA_Quiet, TRUE, TAG_DONE);
    b = OpenScreenTags(NULL, SA_DisplayID, idb, SA_Depth, 8, SA_Quiet, TRUE, TAG_DONE);
    if (a && b) {
        struct RastPort *rp = &a->RastPort;

        t = now_ms();
        for (i = 0; i < n; i++)
            ScreenToFront((i & 1) ? b : a);
        t = now_ms() - t;
        printf("ScreenToFront $%08lx <-> $%08lx: %ld in %lu ms = %lu.%lu /s\n", (unsigned long)ida,
               (unsigned long)idb, (long)n, (unsigned long)t, (unsigned long)(n * 1000UL / (t ? t : 1)),
               (unsigned long)(n * 10000UL / (t ? t : 1) % 10));
        ScreenToFront(a);
        SetAPen(rp, 5);
        t = now_ms();
        for (i = 0; i < 200; i++)
            DrawEllipse(rp, 160, 100, 20 + (i % 100), 10 + (i % 80));
        t = now_ms() - t;
        printf("DrawEllipse (radii 20-120 x 10-90): 200 in %lu ms = %lu /s\n", (unsigned long)t,
               (unsigned long)(200000UL / (t ? t : 1)));
    } else {
        printf("stftest: can't open the screens ($%08lx %s, $%08lx %s)\n", (unsigned long)ida,
               a ? "ok" : "failed", (unsigned long)idb, b ? "ok" : "failed");
    }
    if (b) CloseScreen(b);
    if (a) CloseScreen(a);
    CloseDevice((struct IORequest *)&tr);
    FreeArgs(rda);
    return 0;
}
