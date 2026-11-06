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
#include <proto/exec.h>
#include <stdio.h>

struct GfxBase *GfxBase;

int main(void)
{
    if (!(GfxBase = (struct GfxBase *)OpenLibrary("graphics.library", 37)))
        return 20;
    printf("LOFlist $%08lx SHFlist $%08lx\n", (unsigned long)GfxBase->LOFlist,
           (unsigned long)GfxBase->SHFlist);
    CloseLibrary((struct Library *)GfxBase);
    return 0;
}
