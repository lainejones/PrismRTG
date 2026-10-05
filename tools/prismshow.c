/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laine Jones */
/*
 * PrismShow - show a true-colour picture on a Prism screen.
 *
 *   PrismShow FILE=<picture.ppm> [MODEID=$7A000201] [SECS=60]
 *
 * The picture is a binary PPM (P6, 8 bits a channel) - convert on another
 * machine, e.g. with Python's PIL: Image.open(f).save("pic.ppm"). It goes
 * to the screen through cybergraphics.library's WritePixelArray, centred,
 * on black. Ctrl-C closes it early.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <intuition/screens.h>
#include <graphics/displayinfo.h>
#include <string.h>
#include <cybergraphx/cybergraphics.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <proto/cybergraphics.h>
#include <stdio.h>
#include <stdlib.h>

struct GfxBase *GfxBase;
struct IntuitionBase *IntuitionBase;
struct Library *CyberGfxBase;

#define TEMPLATE "FILE/A,MODEID/K,SECS/K/N"
#define ROWS 16

/* next whitespace-separated number in a PPM header, skipping # comments */
static long ppm_num(FILE *f)
{
    int c;
    long v = 0;
    for (;;) {
        c = fgetc(f);
        if (c == '#') {
            while ((c = fgetc(f)) != '\n' && c != EOF) ;
        } else if (c == EOF) {
            return -1;
        } else if (c >= '0' && c <= '9') {
            break;
        }
    }
    for (; c >= '0' && c <= '9'; c = fgetc(f))
        v = v * 10 + (c - '0');
    return v;                        /* the one whitespace after it is eaten */
}

int main(void)
{
    LONG args[3] = { 0 };
    struct RDArgs *rda;
    struct Screen *s = NULL;
    FILE *f = NULL;
    UBYTE *buf = NULL;
    ULONG id = 0x7A000201, secs = 60, i;
    long w, h, maxv, y, x0, y0;
    int rc = 20;

    if (!(rda = ReadArgs(TEMPLATE, args, NULL))) {
        PrintFault(IoErr(), "PrismShow");
        return 20;
    }
    if (args[1]) {
        const char *p = (const char *)args[1];
        if (*p == '$') p++;
        id = strtoul(p, NULL, 16);
    }
    if (args[2]) secs = *(LONG *)args[2];

    GfxBase = (struct GfxBase *)OpenLibrary("graphics.library", 39);
    IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 39);
    CyberGfxBase = OpenLibrary("cybergraphics.library", 41);
    if (!GfxBase || !IntuitionBase || !CyberGfxBase) {
        printf("PrismShow: needs cybergraphics.library (is PrismD running?)\n");
        goto out;
    }
    if (!(f = fopen((char *)args[0], "rb"))) {
        printf("PrismShow: can't open %s\n", (char *)args[0]);
        goto out;
    }
    if (fgetc(f) != 'P' || fgetc(f) != '6' || (w = ppm_num(f)) <= 0 || (h = ppm_num(f)) <= 0 ||
        (maxv = ppm_num(f)) != 255) {
        printf("PrismShow: not a binary 8-bit PPM (P6)\n");
        goto out;
    }
    if (!(buf = AllocVec(w * 3 * ROWS, MEMF_ANY)))
        goto out;

    s = OpenScreenTags(NULL, SA_DisplayID, id, SA_Depth, 8, SA_Quiet, TRUE, SA_ShowTitle, FALSE,
                       SA_Title, (ULONG)"PrismShow", TAG_DONE);
    if (!s) {
        printf("PrismShow: can't open mode $%08lx\n", (unsigned long)id);
        goto out;
    }
    SetRGB32(&s->ViewPort, 0, 0, 0, 0);
    SetRast(&s->RastPort, 0);
    printf("PrismShow: %ldx%ld picture on a %dx%d screen, %lu bits a pixel\n", w, h, s->Width,
           s->Height, (unsigned long)GetCyberMapAttr(s->RastPort.BitMap, CYBRMATTR_DEPTH));

    x0 = (s->Width - w) / 2;
    y0 = (s->Height - h) / 2;
    for (y = 0; y < h; y += ROWS) {
        long n = h - y < ROWS ? h - y : ROWS;
        if (fread(buf, w * 3, n, f) != (size_t)n) {
            printf("PrismShow: picture file is short\n");
            break;
        }
        WritePixelArray(buf, 0, 0, w * 3, &s->RastPort, x0, y0 + y, w, n, RECTFMT_RGB);
    }
    printf("PrismShow: shown\n");
    {
        /* the mode's name in the corner, so a photo of the monitor says
         * which mode it was */
        struct NameInfo ni;
        ni.Name[0] = 0;
        GetDisplayInfoData(NULL, (UBYTE *)&ni, sizeof(ni), DTAG_NAME, id);
        SetRGB32(&s->ViewPort, 1, 0xffffffff, 0xffffffff, 0xffffffff);
        SetAPen(&s->RastPort, 1);
        SetBPen(&s->RastPort, 0);
        SetDrMd(&s->RastPort, JAM2);
        Move(&s->RastPort, 8, 6 + s->RastPort.TxBaseline);
        Text(&s->RastPort, (STRPTR)ni.Name, strlen((char *)ni.Name));
    }
    for (i = 0; i < secs * 10; i++) {
        if (SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C)
            break;
        Delay(5);
    }
    rc = 0;
out:
    if (s) CloseScreen(s);
    if (buf) FreeVec(buf);
    if (f) fclose(f);
    if (CyberGfxBase) CloseLibrary(CyberGfxBase);
    if (IntuitionBase) CloseLibrary((struct Library *)IntuitionBase);
    if (GfxBase) CloseLibrary((struct Library *)GfxBase);
    FreeArgs(rda);
    return rc;
}
