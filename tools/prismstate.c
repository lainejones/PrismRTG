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

static const char version[] __attribute__((used)) = "$VER: prismstate " PRISM_RELEASE " (" PRISM_RELDATE ")";

struct GfxBase *GfxBase;

/* how a board is driven: what reports need to tell slow or wrong drawing
 * apart (names as in prismboard.h) */
static void board_lines(const char *what, ULONG f, ULONG h)
{
    static const char *const fn[] = { "", "", "", "hardware-pointer", "", "blitter-32bit",
        "shadow", "ACCEL-BROKEN", "blitter-off", "banked", "composed", "reinit" };
    int i;
    printf("%sBoard flags $%04lx:", what, (unsigned long)f);
    for (i = 0; i < (int)(sizeof(fn) / sizeof(fn[0])); i++)
        if ((f & (1UL << i)) && fn[i][0])
            printf(" %s", fn[i]);
    printf("\n%sIn hardware:%s%s%s%s%s%s%s\n", what, h & PRISM_HW_FILL ? " fills" : "",
           h & PRISM_HW_COPY ? " copies" : "", h & PRISM_HW_BETWEEN ? " bitmap-to-bitmap" : "",
           h & PRISM_HW_TEXT ? " text" : "", h & PRISM_HW_LINE ? " lines" : "",
           h & PRISM_HW_PLANAR ? " planar" : "", h ? "" : " nothing (all on the CPU)");
}

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
        printf("PrismD is running: board \"%s\", interface version %u, %lu modes, release %s (%s)\n",
               ps->boardName ? ps->boardName : "?", ps->version, (unsigned long)ps->numModes,
               ps->version >= 7 && ps->release ? ps->release : "before 1.1b6",
               ps->version >= 7 && ps->releaseDate ? ps->releaseDate : "-");
    if (ps && ps->version >= 4 && ps->boardFlags)
        board_lines("", *ps->boardFlags, ps->hwOps);
    if (ps && ps->version >= 6 && ps->numBoards > 1 && ps->board2Flags) {
        printf("Second board \"%s\" at $%08lx (its modes $7a01xxxx, PRISM2:), %lu KB; "
               "the first at $%08lx, %lu KB\n",
               ps->board2Name ? ps->board2Name : "?", (unsigned long)ps->boardAddr[1],
               (unsigned long)ps->vramKB[1], (unsigned long)ps->boardAddr[0],
               (unsigned long)ps->vramKB[0]);
        board_lines("  second board: ", *ps->board2Flags, ps->board2HwOps);
    }
    if (ps && ps->version >= 8 && ps->powerSave) {
        if (*ps->powerSave)
            printf("Monitor power saving: dark after %u minutes without input\n", *ps->powerSave);
        else
            puts("Monitor power saving: off");
    }
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
