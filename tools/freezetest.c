/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laine Jones */
/*
 * freezetest A=<modeid> B=<modeid> [LOG=file] [WAIT=secs]
 *
 * Issue #1 (HANSolo-0007, 2026-11-01): Workbench and Amelinium each on a
 * 1440x900x24 screen, DOpus on a third; DOpus switched to 960x540x8; back to
 * Workbench = instant freeze. This does the same with plain screens: opens
 * screen A (the browser) and screen B (DOpus) in mode A, draws into both,
 * then changes B to mode B the way DOpus does (close, reopen in the new
 * mode), draws again and brings Workbench to the front. Every step is
 * appended to LOG and the file closed again, so after a freeze its last line
 * names the step. Modes are full IDs in hex, e.g. 7A000309.
 */
#include <exec/types.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <graphics/gfx.h>
#include <graphics/displayinfo.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/dos.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct IntuitionBase *IntuitionBase;
struct GfxBase *GfxBase;
static char logname[128] = "RAM:freezetest.log";

static void step(const char *fmt, ULONG a)
{
    char line[160];
    BPTR fh;

    sprintf(line, fmt, a);
    printf("%s\n", line);
    if ((fh = Open((STRPTR)logname, MODE_READWRITE))) {
        Seek(fh, 0, OFFSET_END);
        FPuts(fh, (STRPTR)line);
        FPutC(fh, '\n');
        Close(fh);
    }
}

static struct Screen *open_in(ULONG id, const char *title)
{
    struct DimensionInfo dim;
    ULONG depth = 8;

    if (GetDisplayInfoData(NULL, (UBYTE *)&dim, sizeof(dim), DTAG_DIMS, id))
        depth = dim.MaxDepth;
    step("  depth %lu", depth);
    return OpenScreenTags(NULL, SA_DisplayID, id, SA_Depth, depth, SA_Title, (ULONG)title,
                          SA_ShowTitle, TRUE, SA_Draggable, TRUE, SA_AutoScroll, TRUE,
                          SA_Interleaved, FALSE, TAG_END);
}

static void draw(struct Screen *s, UBYTE pens)
{
    struct RastPort *rp = &s->RastPort;
    WORD y;

    for (y = s->BarHeight + 1; y < s->Height; y += 8) {
        SetAPen(rp, 1 + (y / 8) % pens);
        RectFill(rp, 0, y, s->Width - 1, y + 7 < s->Height ? y + 7 : s->Height - 1);
    }
    SetAPen(rp, 1);
    Move(rp, 20, s->BarHeight + 20);
    Text(rp, (STRPTR)"freezetest", 10);
}

int main(void)
{
    LONG args[4] = { 0 };
    struct RDArgs *rd;
    ULONG ida, idb, wait = 5;
    struct Screen *a, *b;

    IntuitionBase = (struct IntuitionBase *)OpenLibrary((STRPTR)"intuition.library", 39);
    GfxBase = (struct GfxBase *)OpenLibrary((STRPTR)"graphics.library", 39);
    if (!IntuitionBase || !GfxBase)
        return 20;
    if (!(rd = ReadArgs((STRPTR)"A/A,B/A,LOG/K,WAIT/K/N", args, NULL))) {
        PrintFault(IoErr(), (STRPTR)"freezetest");
        return 20;
    }
    ida = strtoul((char *)args[0], NULL, 16);
    idb = strtoul((char *)args[1], NULL, 16);
    if (args[2])
        strncpy(logname, (char *)args[2], sizeof(logname) - 1);
    if (args[3])
        wait = *(ULONG *)args[3];
    DeleteFile((STRPTR)logname);
    step("mode A $%08lx", ida);
    step("  available: %lu (0 = yes)", ModeNotAvailable(ida));
    step("mode B $%08lx", idb);
    step("  available: %lu (0 = yes)", ModeNotAvailable(idb));

    step("1 open screen A (the browser) in mode A", 0);
    if (!(a = open_in(ida, "freezetest A"))) { step("  FAILED", 0); goto out; }
    draw(a, 7);
    step("  open, drawn", 0);
    Delay(wait * 50);
    step("2 open screen B (DOpus) in mode A", 0);
    if (!(b = open_in(ida, "freezetest B"))) { step("  FAILED", 0); CloseScreen(a); goto out; }
    draw(b, 5);
    step("  open, drawn", 0);
    Delay(wait * 50);
    step("3 B to mode B: close B", 0);
    CloseScreen(b);
    step("  closed; reopen B in mode B", 0);
    if (!(b = open_in(idb, "freezetest B2"))) { step("  FAILED", 0); CloseScreen(a); goto out; }
    draw(b, 3);
    step("  open, drawn", 0);
    Delay(wait * 50);
    step("4 Workbench to the front", 0);
    WBenchToFront();
    step("  done", 0);
    Delay(wait * 50);
    step("5 screen A to the front", 0);
    ScreenToFront(a);
    step("  done", 0);
    Delay(wait * 50);
    step("6 Workbench to the front again", 0);
    WBenchToFront();
    step("  done", 0);
    Delay(wait * 50);
    step("7 close B and A", 0);
    CloseScreen(b);
    CloseScreen(a);
    step("PASS: no freeze", 0);
out:
    FreeArgs(rd);
    CloseLibrary((struct Library *)GfxBase);
    CloseLibrary((struct Library *)IntuitionBase);
    return 0;
}
