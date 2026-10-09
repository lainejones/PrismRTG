/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laine Jones */
/*
 * prismgrab - save what a Prism screen really holds as a PPM picture.
 *
 *   prismgrab FILE=RAM:shot.ppm
 *
 * Reads the front screen's framebuffer through PrismD (the public "prism"
 * semaphore) and converts each pixel from the board's format, so a 16-,
 * 24- or 32-bit screen comes out in its true colours. (A grab made with
 * ReadPixel only sees pens: on a deep screen every true-colour pixel turns
 * into the nearest of the screen's few pen colours.) 8-bit screens are
 * converted through the screen's palette.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <graphics/view.h>
#include <intuition/intuitionbase.h>
#include <intuition/screens.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <stdio.h>
#include <string.h>
#include "../src/prism.h"
#include "../src/prismboard.h"

struct IntuitionBase *IntuitionBase;
struct GfxBase *GfxBase;

static const char version[] __attribute__((used)) = "$VER: prismgrab 1.1b4 (09.10.2026)";

#define TEMPLATE "FILE/A,PENS/S"

int main(void)
{
    LONG args[2] = { 0, 0 };
    struct RDArgs *rda;
    struct PrismSem *sem;
    struct PrismScreenInfo si;
    struct Screen *s;
    BOOL (*screenInfo)(struct Screen *, struct PrismScreenInfo *) = NULL;
    static ULONG pal[256 * 3];
    UBYTE *row = NULL;
    BPTR fh = 0;
    char hdr[40];
    ULONG lock;
    UWORD x, y;
    int rc = 20;

    if (!(rda = ReadArgs(TEMPLATE, args, NULL))) {
        PrintFault(IoErr(), "prismgrab");
        return 20;
    }
    IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 39);
    GfxBase = (struct GfxBase *)OpenLibrary("graphics.library", 39);
    if (!IntuitionBase || !GfxBase) goto out;

    Forbid();
    if ((sem = (struct PrismSem *)FindSemaphore(PRISM_SEMNAME)))
        screenInfo = sem->screenInfo;
    Permit();
    if (!screenInfo) { printf("prismgrab: PrismD isn't running\n"); goto out; }

    lock = LockIBase(0);
    s = IntuitionBase->FirstScreen;
    UnlockIBase(lock);
    if (!s || !screenInfo(s, &si)) { printf("prismgrab: the front screen isn't a Prism screen\n"); rc = 5; goto out; }
    if (s->ViewPort.ColorMap)
        GetRGB32(s->ViewPort.ColorMap, 0, 256, pal);

    if (!(row = AllocVec((ULONG)si.width * 3, MEMF_ANY))) goto out;
    if (!(fh = Open((STRPTR)args[0], MODE_NEWFILE))) { PrintFault(IoErr(), "prismgrab"); goto out; }
    sprintf(hdr, "P6\n%u %u\n255\n", si.width, si.height);
    Write(fh, hdr, strlen(hdr));
    for (y = 0; y < si.height; y++) {
        const UBYTE *p = si.vram + (ULONG)y * si.bytesPerRow;
        UBYTE *o = row;
        for (x = 0; x < si.width; x++, o += 3) {
            UBYTE r, g, b;
            UWORD v;
            switch (si.format) {
            case PF_CLUT8:
                if (args[1]) { r = g = b = *p++; break; }   /* PENS: grey level = pen number */
                r = pal[*p * 3] >> 24; g = pal[*p * 3 + 1] >> 24; b = pal[*p * 3 + 2] >> 24; p++; break;
            case PF_RGB24:  r = p[0]; g = p[1]; b = p[2]; p += 3; break;
            case PF_BGR24:  b = p[0]; g = p[1]; r = p[2]; p += 3; break;
            case PF_ARGB32: r = p[1]; g = p[2]; b = p[3]; p += 4; break;
            case PF_BGRA32: b = p[0]; g = p[1]; r = p[2]; p += 4; break;
            case PF_RGBA32: r = p[0]; g = p[1]; b = p[2]; p += 4; break;
            default:
                v = ((UWORD)p[0] << 8) | p[1];
                if (si.format == PF_RGB565LE || si.format == PF_RGB555LE ||
                    si.format == PF_BGR565LE || si.format == PF_BGR555LE)
                    v = (v << 8) | (v >> 8);
                if (si.format == PF_RGB555LE || si.format == PF_RGB555BE ||
                    si.format == PF_BGR555LE) {
                    r = (v >> 7) & 0xf8; g = (v >> 2) & 0xf8; b = (v << 3) & 0xf8;
                } else {
                    r = (v >> 8) & 0xf8; g = (v >> 3) & 0xfc; b = (v << 3) & 0xf8;
                }
                if (si.format == PF_BGR565LE || si.format == PF_BGR555LE) { UBYTE t = r; r = b; b = t; }
                p += 2;
            }
            o[0] = r; o[1] = g; o[2] = b;
        }
        if (Write(fh, row, (LONG)si.width * 3) != (LONG)si.width * 3) { printf("prismgrab: write failed\n"); goto out; }
    }
    if (args[1]) {
        /* the colours behind the pen numbers (first and last 16) */
        UWORD i;
        for (i = 0; i < 256; i++)
            if (i < 16 || i >= 240)
                printf("pen %3u = %02lx %02lx %02lx\n", i, (unsigned long)(pal[i * 3] >> 24),
                       (unsigned long)(pal[i * 3 + 1] >> 24), (unsigned long)(pal[i * 3 + 2] >> 24));
    }
    printf("prismgrab: %ux%u, format %u, mode $%08lx\n", si.width, si.height, si.format,
           (unsigned long)si.modeID);
    rc = 0;
out:
    if (fh) Close(fh);
    if (row) FreeVec(row);
    if (GfxBase) CloseLibrary((struct Library *)GfxBase);
    if (IntuitionBase) CloseLibrary((struct Library *)IntuitionBase);
    FreeArgs(rda);
    return rc;
}
