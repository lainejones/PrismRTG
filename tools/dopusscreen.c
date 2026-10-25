/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laine Jones */
/*
 * dopusscreen [SECS=60] : a screen opened the way Directory Opus 5 opens
 * its own - like the Workbench (SA_LikeWorkbench), a public screen, auto
 * scrolling, and on Intuition 45+ with off-screen dragging - with stripes
 * (16 rows each, pens 1-7) and a title, kept open SECS seconds so the host
 * can drag it (issue #1: "dragging from DOpus to see WB makes the mouse
 * behave weird"). Prints the screen's position every second it moved.
 */
#include <exec/types.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <intuition/intuitionbase.h>
#include <graphics/gfx.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/dos.h>
#include <stdio.h>

#ifndef SA_OffScreenDragging
#define SA_OffScreenDragging (SA_Dummy + 0x002a)
#endif

struct IntuitionBase *IntuitionBase;
struct GfxBase *GfxBase;

int main(void)
{
    LONG args[2] = { 0 };
    struct RDArgs *rd;
    ULONG secs = 60, i;
    struct Screen *s;
    struct RastPort *rp;
    WORD y, lx = 0, ly = 0;
    BOOL v45;

    IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 39);
    GfxBase = (struct GfxBase *)OpenLibrary("graphics.library", 39);
    if (!IntuitionBase || !GfxBase)
        return 20;
    if ((rd = ReadArgs((STRPTR)"SECS/K/N,NODRAG/S", args, NULL))) {
        if (args[0]) secs = *(LONG *)args[0];
        FreeArgs(rd);
    }
    v45 = ((struct Library *)IntuitionBase)->lib_Version >= 45;
    /* NODRAG: SA_Draggable FALSE - what Intuition does with the pointer on
     * a drag of a screen that can't be dragged (the control for PrismRTG
     * with dragging off, whose modes aren't marked draggable) */
    s = OpenScreenTags(NULL, SA_LikeWorkbench, TRUE, args[1] ? SA_Draggable : TAG_IGNORE, FALSE,
                       v45 ? SA_OffScreenDragging : TAG_IGNORE, TRUE,
                       SA_Overscan, OSCAN_TEXT, SA_Title, (ULONG)"dopusscreen (like Directory Opus 5)",
                       SA_PubName, (ULONG)"DOPUSSCREEN", SA_AutoScroll, TRUE, SA_Interleaved, TRUE,
                       SA_SharePens, TRUE, TAG_DONE);
    if (!s) {
        printf("dopusscreen: can't open the screen\n");
        return 10;
    }
    rp = &s->RastPort;
    for (y = s->BarHeight + 1; y < s->Height; y += 16) {
        SetAPen(rp, 1 + ((y >> 4) % 7));
        RectFill(rp, 0, y, s->Width - 1, y + 15 < s->Height ? y + 15 : s->Height - 1);
    }
    PubScreenStatus(s, 0);
    printf("dopusscreen: %ux%u mode $%08lx, off-screen dragging %s, %lu s\n", s->Width, s->Height,
           (unsigned long)GetVPModeID(&s->ViewPort), v45 ? "asked for" : "not available", (unsigned long)secs);
    for (i = 0; i < secs; i++) {
        Delay(50);
        if (s->LeftEdge != lx || s->TopEdge != ly) {
            lx = s->LeftEdge; ly = s->TopEdge;
            printf("dopusscreen: at %d,%d  mouse %d,%d\n", lx, ly, IntuitionBase->MouseX, IntuitionBase->MouseY);
        }
    }
    CloseScreen(s);
    CloseLibrary((struct Library *)GfxBase);
    CloseLibrary((struct Library *)IntuitionBase);
    return 0;
}
