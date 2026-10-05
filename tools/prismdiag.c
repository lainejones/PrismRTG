/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laine Jones */
/*
 * prismdiag - print what Intuition and graphics.library think of the
 * front screen and the mouse (for pointer-position problems).
 */
#include <exec/types.h>
#include <graphics/gfxbase.h>
#include <graphics/displayinfo.h>
#include <graphics/view.h>
#include <intuition/intuitionbase.h>
#include <intuition/screens.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <stdio.h>

struct GfxBase *GfxBase;
struct IntuitionBase *IntuitionBase;

int main(void)
{
    struct Screen *s;
    struct ViewPort *vp;
    struct DisplayInfo di;
    struct DimensionInfo dm;
    struct MonitorInfo mi;
    ULONG id;

    GfxBase = (struct GfxBase *)OpenLibrary("graphics.library", 39);
    IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 39);
    if (!GfxBase || !IntuitionBase) return 20;

    for (s = IntuitionBase->FirstScreen; s; s = s->NextScreen) {
        vp = &s->ViewPort;
        id = GetVPModeID(vp);
        printf("screen %08lx \"%.30s\"\n", (unsigned long)s, s->Title ? (char *)s->Title : "");
        printf("  mode $%08lx  %dx%d at %d,%d  Mouse %d,%d  Flags %04x\n", (unsigned long)id,
               s->Width, s->Height, s->LeftEdge, s->TopEdge, s->MouseX, s->MouseY, s->Flags);
        printf("  ViewPort D %dx%d offset %d,%d Modes %04x  RasInfo Rx/Ry %d,%d\n", vp->DWidth,
               vp->DHeight, vp->DxOffset, vp->DyOffset, vp->Modes,
               vp->RasInfo ? vp->RasInfo->RxOffset : -1, vp->RasInfo ? vp->RasInfo->RyOffset : -1);
        if (GetDisplayInfoData(NULL, (UBYTE *)&di, sizeof(di), DTAG_DISP, id))
            printf("  DISP  Resolution %d,%d PixelSpeed %u Sprite res %d,%d NotAvail %u Props %08lx\n",
                   di.Resolution.x, di.Resolution.y, di.PixelSpeed, di.SpriteResolution.x,
                   di.SpriteResolution.y, di.NotAvailable, (unsigned long)di.PropertyFlags);
        if (GetDisplayInfoData(NULL, (UBYTE *)&dm, sizeof(dm), DTAG_DIMS, id))
            printf("  DIMS  Nominal %d,%d-%d,%d MaxOScan %d,%d-%d,%d TxtOScan %d,%d-%d,%d Raster %u..%u x %u..%u\n",
                   dm.Nominal.MinX, dm.Nominal.MinY, dm.Nominal.MaxX, dm.Nominal.MaxY,
                   dm.MaxOScan.MinX, dm.MaxOScan.MinY, dm.MaxOScan.MaxX, dm.MaxOScan.MaxY,
                   dm.TxtOScan.MinX, dm.TxtOScan.MinY, dm.TxtOScan.MaxX, dm.TxtOScan.MaxY,
                   dm.MinRasterWidth, dm.MaxRasterWidth, dm.MinRasterHeight, dm.MaxRasterHeight);
        if (GetDisplayInfoData(NULL, (UBYTE *)&mi, sizeof(mi), DTAG_MNTR, id))
            printf("  MNTR  ViewPosition %d,%d ViewRes %d,%d PosRange %d,%d-%d,%d Total %u x %u MouseTicks %d,%d DefViewPos %d,%d\n",
                   mi.ViewPosition.x, mi.ViewPosition.y, mi.ViewResolution.x, mi.ViewResolution.y,
                   mi.ViewPositionRange.MinX, mi.ViewPositionRange.MinY,
                   mi.ViewPositionRange.MaxX, mi.ViewPositionRange.MaxY, mi.TotalColorClocks,
                   mi.TotalRows, mi.MouseTicks.x, mi.MouseTicks.y, mi.DefaultViewPosition.x,
                   mi.DefaultViewPosition.y);
    }
    printf("IntuitionBase Mouse %d,%d  ViewLord D offset %d,%d Modes %04x\n",
           IntuitionBase->MouseX, IntuitionBase->MouseY, IntuitionBase->ViewLord.DxOffset,
           IntuitionBase->ViewLord.DyOffset, IntuitionBase->ViewLord.Modes);
    printf("GfxBase NormalDisplayRows %u Columns %u DisplayFlags %04x\n",
           GfxBase->NormalDisplayRows, GfxBase->NormalDisplayColumns, GfxBase->DisplayFlags);
    CloseLibrary((struct Library *)IntuitionBase);
    CloseLibrary((struct Library *)GfxBase);
    return 0;
}
