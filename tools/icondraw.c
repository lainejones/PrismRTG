/* SPDX-License-Identifier: GPL-3.0-only */
/* icondraw - draw icons normal and selected through icon.library, as
 * Workbench does, in a window on the Workbench screen, and log the pens
 * read back. An icon without a selected image of its own is complemented
 * (BltMaskBitMapRastPort minterm 0x20): run it on a native and on a Prism
 * screen and compare the logs.
 *   icondraw [TO file] [WAIT] icon [icon ...]   (icon: path without .info) */
#include <exec/types.h>
#include <dos/dos.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <intuition/imageclass.h>
#include <workbench/workbench.h>
#include <workbench/icon.h>
#include <utility/tagitem.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/icon.h>
#include <string.h>

struct IntuitionBase *IntuitionBase;
struct GfxBase *GfxBase;
struct Library *IconBase;

#define MAXICONS 8

int main(int argc, char **argv)
{
    struct Screen *s = NULL;
    struct Window *win = NULL;
    struct DiskObject *dob[MAXICONS];
    struct Rectangle rect[MAXICONS];
    const char *to = NULL;
    BPTR fh = 0;
    BOOL wait = FALSE;
    int n = 0, i, k, rc = 20;
    WORD x, y, left, top, rowh = 0;

    IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 39);
    GfxBase = (struct GfxBase *)OpenLibrary("graphics.library", 39);
    IconBase = OpenLibrary("icon.library", 44);
    if (!IntuitionBase || !GfxBase || !IconBase) {
        Printf("icondraw: needs icon.library 44\n");
        goto out;
    }
    for (i = 1; i < argc; i++)
        if (!stricmp(argv[i], "TO") && i + 1 < argc)
            to = argv[++i];
        else if (!stricmp(argv[i], "WAIT"))
            wait = TRUE;
    if (to && !(fh = Open(to, MODE_NEWFILE)))
        Printf("icondraw: can't write %s\n", (ULONG)to);
    for (i = 1; i < argc; i++) {
        if (!stricmp(argv[i], "TO")) {
            i++;
            continue;
        }
        if (!stricmp(argv[i], "WAIT"))
            continue;
        if (n < MAXICONS && (dob[n] = GetDiskObjectNew(argv[i])))
            n++;
        else
            FPrintf(fh ? fh : Output(), "no icon %s\n", (ULONG)argv[i]);
    }
    if (!n)
        goto out;
    /* started from User-Startup: wait for the Workbench screen */
    for (k = 0; k < (wait ? 120 : 1) && !(s = LockPubScreen("Workbench")); k++)
        Delay(50);
    if (!s) {
        FPrintf(fh ? fh : Output(), "no Workbench screen\n");
        goto out;
    }
    if (wait)
        Delay(250);                    /* let Workbench draw its windows first */
    win = OpenWindowTags(NULL, WA_PubScreen, (ULONG)s, WA_Left, 20, WA_Top, 20,
                         WA_Width, 400, WA_Height, 200, WA_Title, (ULONG)"icondraw",
                         WA_DragBar, TRUE, WA_DepthGadget, TRUE, WA_CloseGadget, TRUE,
                         WA_IDCMP, IDCMP_CLOSEWINDOW, TAG_DONE);
    if (!win) {
        FPrintf(fh ? fh : Output(), "no window\n");
        goto out;
    }
    FPrintf(fh ? fh : Output(), "screen depth %ld\n",
            (LONG)GetBitMapAttr(s->RastPort.BitMap, BMA_DEPTH));
    top = win->BorderTop + 4;
    for (i = 0; i < n; i++) {
        WORD st;
        GetIconRectangle(win->RPort, dob[i], NULL, &rect[i], ICONDRAWA_Borderless, TRUE, TAG_DONE);
        for (st = 0, left = win->BorderLeft + 4; st < 2; st++) {
            WORD l = left - rect[i].MinX, t = top - rect[i].MinY;
            WORD w = rect[i].MaxX - rect[i].MinX + 1, h = rect[i].MaxY - rect[i].MinY + 1;
            DrawIconState(win->RPort, dob[i], NULL, l, t, st ? IDS_SELECTED : IDS_NORMAL,
                          ICONDRAWA_Borderless, TRUE, ICONDRAWA_EraseBackground, TRUE, TAG_DONE);
            FPrintf(fh ? fh : Output(), "icon %ld %s %ldx%ld\n", (LONG)i,
                    (ULONG)(st ? "selected" : "normal"), (LONG)w, (LONG)h);
            if (fh)
                for (y = 0; y < h; y++) {
                    for (x = 0; x < w; x++)
                        FPrintf(fh, "%02lx", (ULONG)ReadPixel(win->RPort, left + x, top + y));
                    FPrintf(fh, "\n");
                }
            left += w + 8;
            if (h > rowh) rowh = h;
        }
        top += rowh + 8;
        rowh = 0;
    }
    if (fh) {
        Close(fh);
        fh = 0;
    }
    rc = 0;
    WaitPort(win->UserPort);
out:
    if (fh) Close(fh);
    if (win) CloseWindow(win);
    if (s) UnlockPubScreen(NULL, s);
    for (i = 0; i < n; i++)
        FreeDiskObject(dob[i]);
    if (IconBase) CloseLibrary(IconBase);
    if (GfxBase) CloseLibrary((struct Library *)GfxBase);
    if (IntuitionBase) CloseLibrary((struct Library *)IntuitionBase);
    return rc;
}
