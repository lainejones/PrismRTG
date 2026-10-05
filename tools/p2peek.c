/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laine Jones */
/*
 * p2peek - look at a Picasso II / GBAPII++ register window before and
 * after waking the Cirrus chip.
 *
 * Reads Misc, unlocks the Cirrus extensions (SR6 = 0x12, reads back 0x12
 * when the chip answers) and reads the chip id (CR27), first as the board
 * comes out of reset, then after the ISA wake-up sequence Linux's cirrusfb
 * uses on Zorro boards (0x46E8 = 0x10, 0x102 = 0x01, 0x46E8 = 0x08,
 * 0x3C3 = 0x01). Only index, wake-up and unlock registers are written.
 */
#include <exec/types.h>
#include <libraries/configvars.h>
#include <proto/exec.h>
#include <proto/expansion.h>
#include <stdio.h>

struct ExpansionBase *ExpansionBase;

static volatile UBYTE *base;

static UBYTE rd(ULONG o)          { return base[o]; }
static void  wr(ULONG o, UBYTE v) { base[o] = v; }

static void look(const char *when)
{
    UBYTE misc, sr6, id, id2, sr7, st;

    Disable();
    misc = rd(0x3cc);
    if (!(misc & 1))
        wr(0x3c2, misc | 1);                 /* colour I/O: CRTC at 0x3D4 */
    wr(0x3c4, 0x06); wr(0x3c5, 0x12);
    wr(0x3c4, 0x06); sr6 = rd(0x3c5);
    wr(0x3c4, 0x07); sr7 = rd(0x3c5);
    wr(0x3d4, 0x27); id = rd(0x3d5);
    wr(0x3d4, 0x27); id2 = rd(0x1000 + 0x3d4); /* through the odd-port alias */
    st = rd(0x3da);
    Enable();
    printf("%-14s Misc=%02x SR6=%02x%s SR7=%02x CR27=%02x (alias %02x) ST1=%02x\n", when,
           misc, sr6, sr6 == 0x12 ? " (unlocked)" : "          ", sr7, id, id2, st);
}

int main(void)
{
    struct ConfigDev *cd;

    ExpansionBase = (struct ExpansionBase *)OpenLibrary("expansion.library", 37);
    if (!ExpansionBase) return 20;
    if (!(cd = FindConfigDev(NULL, 2167, 12)) && !(cd = FindConfigDev(NULL, 2167, 17))) {
        printf("no Picasso II / GBAPII++ register board\n");
        CloseLibrary((struct Library *)ExpansionBase);
        return 5;
    }
    base = (volatile UBYTE *)cd->cd_BoardAddr;
    printf("2167/%u regs @ $%08lx size $%lx\n", cd->cd_Rom.er_Product,
           (unsigned long)base, (unsigned long)cd->cd_BoardSize);

    look("as found:");
    Disable();
    wr(0x46e8, 0x10);                         /* setup mode               */
    wr(0x102, 0x01);                          /* POS: enable              */
    wr(0x46e8, 0x08);                         /* enable, leave setup      */
    wr(0x3c3, 0x01);                          /* video subsystem enable   */
    Enable();
    look("after wake-up:");
    CloseLibrary((struct Library *)ExpansionBase);
    return 0;
}
