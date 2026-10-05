/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laine Jones */
/* memwhere - list the memory regions and say where a few things live
 * (is the code we time running from slow Zorro RAM?). */
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/execbase.h>
#include <exec/tasks.h>
#include <graphics/gfxbase.h>
#include <proto/exec.h>
#include <stdio.h>

extern struct ExecBase *SysBase;
struct GfxBase *GfxBase;

int main(void)
{
    struct MemHeader *mh;
    struct Task *t;
    int local;

    Forbid();
    for (mh = (struct MemHeader *)SysBase->MemList.lh_Head; mh->mh_Node.ln_Succ;
         mh = (struct MemHeader *)mh->mh_Node.ln_Succ)
        printf("region %08lx-%08lx pri %4d attr %04x free %8lu  %s\n",
               (unsigned long)mh->mh_Lower, (unsigned long)mh->mh_Upper, mh->mh_Node.ln_Pri,
               mh->mh_Attributes, (unsigned long)mh->mh_Free,
               mh->mh_Node.ln_Name ? mh->mh_Node.ln_Name : "");
    t = FindTask("PrismD");
    if (!t) t = FindTask("C:PrismD");
    if (t) printf("PrismD task %08lx stack %08lx-%08lx\n", (unsigned long)t,
                  (unsigned long)t->tc_SPLower, (unsigned long)t->tc_SPUpper);
    Permit();
    GfxBase = (struct GfxBase *)OpenLibrary("graphics.library", 39);
    printf("this code %08lx, stack %08lx, GfxBase %08lx, SysBase %08lx\n", (unsigned long)main,
           (unsigned long)&local, (unsigned long)GfxBase, (unsigned long)SysBase);
    if (GfxBase) {
        ULONG *vec = (ULONG *)((UBYTE *)GfxBase - 0x144 + 2);   /* WritePixel LVO target */
        printf("WritePixel vector -> %08lx, Move -> %08lx\n", (unsigned long)*vec,
               (unsigned long)*(ULONG *)((UBYTE *)GfxBase - 0xf0 + 2));
        CloseLibrary((struct Library *)GfxBase);
    }
    return 0;
}
