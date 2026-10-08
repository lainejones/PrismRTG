/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laine Jones */
/*
 * prismstate - is PrismD running, on which board, and which PrismRTG modes
 * can programs open right now (for PrismCheck's report).
 *
 * Read-only: it looks at PrismD's public semaphore and the display
 * database, and opens nothing.
 */
#include <exec/types.h>
#include <exec/execbase.h>
#include <graphics/displayinfo.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <stdio.h>
#include "../src/prism.h"

static const char version[] __attribute__((used)) = "$VER: prismstate 1.0 (08.10.2026)";

struct GfxBase *GfxBase;

int main(void)
{
    struct PrismSem *ps;
    ULONG id = INVALID_ID;
    int n = 0, off = 0;

    if (!(GfxBase = (struct GfxBase *)OpenLibrary("graphics.library", 39))) {
        puts("prismstate: needs OS 3.0 or newer");
        return 20;
    }
    Forbid();
    ps = (struct PrismSem *)FindSemaphore((STRPTR)PRISM_SEMNAME);
    if (ps)
        printf("PrismD is running: board \"%s\", interface version %u, %lu modes\n",
               ps->boardName ? ps->boardName : "?", ps->version, (unsigned long)ps->numModes);
    Permit();
    if (!ps)
        puts("PrismD is NOT running");

    puts("PrismRTG modes in the display database:");
    while ((id = NextDisplayInfo(id)) != INVALID_ID) {
        struct NameInfo ni;
        struct DimensionInfo dim;
        ULONG why;
        if (!IS_PRISM_ID(id))
            continue;
        ni.Name[0] = 0;
        GetDisplayInfoData(NULL, (UBYTE *)&ni, sizeof(ni), DTAG_NAME, id);
        dim.MaxDepth = 0;
        GetDisplayInfoData(NULL, (UBYTE *)&dim, sizeof(dim), DTAG_DIMS, id);
        why = ModeNotAvailable(id);
        printf("  $%08lx  %-28s %s\n", (unsigned long)id, ni.Name[0] ? (char *)ni.Name : "(no name)",
               why ? "not available now" : "available");
        n++;
        if (why)
            off++;
    }
    printf("%d PrismRTG modes, %d of them not available now\n", n, off);
    CloseLibrary((struct Library *)GfxBase);
    return ps ? 0 : 5;
}
