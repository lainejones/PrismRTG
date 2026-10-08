/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laine Jones */
/*
 * PrismProbe - list the expansion boards and say which ones Prism can drive.
 *
 * Read-only: it never writes to a board, so it is safe to run while
 * Picasso96 owns the display.
 */
#include <exec/types.h>
#include <libraries/configvars.h>
#include <proto/exec.h>
#include <proto/expansion.h>
#include <stdio.h>

static const char version[] __attribute__((used)) = "$VER: PrismProbe 1.1b2 (08.10.2026)";

struct ExpansionBase *ExpansionBase;

static const char *known(UWORD mfr, UBYTE prod)
{
    if (mfr == 0x6d6e && prod == 3)  return "ZZ9000 (Zorro II)  -> zz9000 driver";
    if (mfr == 0x6d6e && prod == 4)  return "ZZ9000 (Zorro III) -> zz9000 driver";
    if (mfr == 2167 && prod == 11)   return "Picasso II/II+ VRAM -> picasso2 driver";
    if (mfr == 2167 && prod == 12)   return "Picasso II/II+ registers -> picasso2 driver";
    if (mfr == 2167 && prod == 16)   return "GBAPII++ VRAM -> picasso2 driver";
    if (mfr == 2167 && prod == 17)   return "GBAPII++ registers -> picasso2 driver";
    if (mfr == 2195 && prod == 10)   return "Piccolo SD64 VRAM -> picasso2 driver (emulation-tested)";
    if (mfr == 2195 && prod == 11)   return "Piccolo SD64 registers -> picasso2 driver (emulation-tested)";
    if (mfr == 2195 && prod == 5)    return "Piccolo VRAM -> picasso2 driver (emulation-tested)";
    if (mfr == 2195 && prod == 6)    return "Piccolo registers -> picasso2 driver (emulation-tested)";
    if (mfr == 2193 && prod == 1)    return "Spectrum 28/24 VRAM -> picasso2 driver (emulation-tested)";
    if (mfr == 2193 && prod == 2)    return "Spectrum 28/24 registers -> picasso2 driver (emulation-tested)";
    return NULL;
}

int main(void)
{
    struct ConfigDev *cd = NULL;
    int n = 0, ours = 0;

    if (!(ExpansionBase = (struct ExpansionBase *)OpenLibrary("expansion.library", 37))) {
        printf("PrismProbe: no expansion.library\n");
        return 20;
    }
    printf("PrismProbe - expansion boards\n");
    while ((cd = FindConfigDev(cd, -1, -1))) {
        const char *k = known(cd->cd_Rom.er_Manufacturer, cd->cd_Rom.er_Product);
        printf(" %5u/%-3u @ $%08lx size $%08lx %s%s\n",
               cd->cd_Rom.er_Manufacturer, cd->cd_Rom.er_Product,
               (unsigned long)cd->cd_BoardAddr, (unsigned long)cd->cd_BoardSize,
               (cd->cd_Flags & CDF_CONFIGME) ? "[free] " : "[claimed] ",
               k ? k : "");
        n++;
        if (k) ours++;
    }
    printf("%d board(s), %d usable by Prism\n", n, ours);
    CloseLibrary((struct Library *)ExpansionBase);
    return ours ? 0 : 5;
}
