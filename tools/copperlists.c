/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laine Jones */
/*
 * copperlists - the chipset copper lists graphics.library is running:
 * GfxBase->LOFlist (long frames) and SHFlist (short frames), for the
 * gate's copperlist check. With only PrismRTG screens up the two must be
 * the same blank list. Read-only.
 */
#include <exec/types.h>
#include <graphics/gfxbase.h>
#include <graphics/view.h>
#include <intuition/intuitionbase.h>
#include <intuition/screens.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <stdio.h>

struct GfxBase *GfxBase;

int main(void)
{
    if (!(GfxBase = (struct GfxBase *)OpenLibrary("graphics.library", 37)))
        return 20;
    struct IntuitionBase *ib = (struct IntuitionBase *)OpenLibrary("intuition.library", 37);
    printf("LOFlist $%08lx SHFlist $%08lx\n", (unsigned long)GfxBase->LOFlist,
           (unsigned long)GfxBase->SHFlist);
    /* what graphics runs the chipset from: the active View's modes (LACE
     * $0004 makes it interlace), and the front screen's ViewPort modes */
    if (GfxBase->ActiView)
        printf("ActiView Modes $%04x LOFCprList $%08lx\n", GfxBase->ActiView->Modes,
               (unsigned long)GfxBase->ActiView->LOFCprList);
    printf("ActiView $%08lx GfxBase Modes $%04x system_bplcon0 $%04x\n", (unsigned long)GfxBase->ActiView,
           GfxBase->Modes, GfxBase->system_bplcon0);
    {
        /* the chipset itself: VPOSR's LOF bit toggles from frame to frame only
         * while it runs interlaced */
        int i;
        printf("LOF over 8 frames:");
        for (i = 0; i < 8; i++) {
            WaitTOF();
            printf(" %d", (*(volatile UWORD *)0xdff004 >> 15) & 1);
        }
        printf("\n");
    }
    if (ib) {
        if (ib->FirstScreen)
            printf("front screen ViewPort Modes $%04x ModeID $%08lx\n", ib->FirstScreen->ViewPort.Modes,
                   (unsigned long)GetVPModeID(&ib->FirstScreen->ViewPort));
        printf("ViewLord $%08lx Modes $%04x LOFCprList $%08lx\n", (unsigned long)&ib->ViewLord, ib->ViewLord.Modes,
               (unsigned long)ib->ViewLord.LOFCprList);
        CloseLibrary((struct Library *)ib);
    }
    CloseLibrary((struct Library *)GfxBase);
    return 0;
}
