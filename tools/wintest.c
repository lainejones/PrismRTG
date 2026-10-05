/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Laine Jones */
/* wintest: Intuition window operations on a Prism screen, one step at a
 * time, each step appended to a log file before it runs - so a step that
 * hangs or crashes the machine is the last line of the file.
 *
 *   wintest MODEID=$7a000201 LOG=DH0:Prism/wt.log
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/rdargs.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <graphics/gfx.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *logName;

static void step(const char *what)
{
    BPTR f;

    if ((f = Open((STRPTR)logName, MODE_READWRITE))) {
        Seek(f, 0, OFFSET_END);
        Write(f, (APTR)what, strlen(what));
        Write(f, "\n", 1);
        Close(f);
    }
}

int main(void)
{
    LONG args[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    struct RDArgs *rda;
    struct Screen *s;
    struct Window *w, *w2;
    ULONG id;
    ULONG flags = WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_SIZEGADGET | WFLG_CLOSEGADGET;
    int pass;

    if (!(rda = ReadArgs((STRPTR)"MODEID/A,LOG/A,SIMPLE/S,LOOP/K/N,IDLE/K/N,DRAW/K/N,VAR/K/N,SCREENS/K/N", args, NULL)))
        return 20;
    id = strtoul((char *)args[0] + (*(char *)args[0] == '$'), NULL, 16);
    logName = (const char *)args[1];

    step("open screen");
    s = OpenScreenTags(NULL, SA_DisplayID, id, SA_Depth, 8, SA_Title, (ULONG)"wintest",
                       SA_Pens, (ULONG)"\xff\xff", TAG_DONE);
    if (!s) { step("  no screen"); FreeArgs(rda); return 10; }

    if (args[7]) {
        /* open and close a screen n times and time it (SysSpeed's OpenScr),
         * each with a 256-colour palette set one entry at a time */
        LONG cnt = *(LONG *)args[7], i, k, tOpen = 0, tPal = 0, tClose = 0;
        struct DateStamp t0, t1;
        char line[80];
        CloseScreen(s);
        DateStamp(&t0);
        for (i = 0; i < cnt; i++) {
            struct DateStamp a, b, c, d;
            DateStamp(&a);
            s = OpenScreenTags(NULL, SA_DisplayID, id, SA_Depth, 8, SA_Title, (ULONG)"wintest",
                               TAG_DONE);
            if (!s) break;
            DateStamp(&b);
            for (k = 0; k < 256; k++)
                SetRGB32(&s->ViewPort, k, (ULONG)k << 24, (ULONG)(255 - k) << 24, 0x80000000);
            DateStamp(&c);
            CloseScreen(s);
            DateStamp(&d);
            tOpen += b.ds_Tick - a.ds_Tick; tPal += c.ds_Tick - b.ds_Tick; tClose += d.ds_Tick - c.ds_Tick;
        }
        DateStamp(&t1);
        sprintf(line, " ticks (1/50 s): open %ld, 256 colours %ld, close %ld", (long)tOpen, (long)tPal, (long)tClose);
        step(line);
        puts(line);
        k = (t1.ds_Minute - t0.ds_Minute) * 3000 + (t1.ds_Tick - t0.ds_Tick);
        sprintf(line, " %ld screens opened and closed in %ld.%02ld s", (long)i, (long)(k / 50),
                (long)(k % 50) * 2);
        step(line);
        puts(line);
        FreeArgs(rda);
        return 0;
    }
    if (args[4]) {
        /* just sit on the mode: a heartbeat line every second */
        LONG n = *(LONG *)args[4], i;
        char line[40];
        for (i = 0; i < n; i++) {
            sprintf(line, " idle %ld", (long)i);
            step(line);
            Delay(50);
        }
    }
    if (args[5]) {
        /* draw on the screen itself, no windows: fills, text, lines */
        LONG n = *(LONG *)args[5], i;
        char line[40];
        struct RastPort *rp = &s->RastPort;
        for (i = 0; i < n; i++) {
            if (i % 100 == 0) {
                sprintf(line, " draw %ld", (long)i);
                step(line);
            }
            SetAPen(rp, 1 + (i & 3));
            RectFill(rp, 27, 27, 191, 126);
            SetAPen(rp, 2);
            Move(rp, 40, 60);
            Text(rp, (STRPTR)"SysSpeed", 8);
            Move(rp, 27, 27);
            Draw(rp, 191, 126);
        }
        step(" draw done");
    }
    if (args[3]) {
        /* open and close a window over and over (SysSpeed's OpenWin test),
         * logging free memory: a leak or a hang shows in the file */
        LONG n = *(LONG *)args[3], i;
        LONG var = args[6] ? *(LONG *)args[6] : 0;
        char line[80];
        for (i = 0; i < n; i++) {
            if (i % 25 == 0) {
                sprintf(line, " open/close %ld: chip %lu fast %lu", (long)i,
                        (unsigned long)AvailMem(MEMF_CHIP), (unsigned long)AvailMem(MEMF_FAST));
                step(line);
            }
            w = OpenWindowTags(NULL, WA_CustomScreen, (ULONG)s, WA_Left, 27, WA_Top, 27,
                               WA_Width, 165, WA_Height, 100,
                               (var == 1 || var == 3) ? TAG_IGNORE : WA_Title, (ULONG)"SysSpeed",
                               WA_Flags, (var == 1 ? WFLG_BORDERLESS :
                                          var == 2 ? WFLG_DRAGBAR :
                                          var == 3 ? WFLG_DEPTHGADGET | WFLG_CLOSEGADGET :
                                          var == 4 ? 0 :
                                          WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_CLOSEGADGET) |
                               ((i & 1) ? WFLG_SIMPLE_REFRESH : WFLG_SMART_REFRESH), TAG_DONE);
            if (!w) { step("  no window"); break; }
            CloseWindow(w);
        }
        step(" loop done");
    }
    for (pass = 0; pass < 2 && !args[3] && !args[4] && !args[5]; pass++) {
        ULONG refresh = (pass == 0 && !args[2]) ? WFLG_SMART_REFRESH : WFLG_SIMPLE_REFRESH;
        step(pass == 0 && !args[2] ? "smart refresh" : "simple refresh");
        step(" open window");
        w = OpenWindowTags(NULL, WA_CustomScreen, (ULONG)s, WA_Left, 27, WA_Top, 27,
                           WA_Width, 165, WA_Height, 100, WA_Title, (ULONG)"one",
                           WA_MinWidth, 50, WA_MinHeight, 30, WA_MaxWidth, 640, WA_MaxHeight, 480,
                           WA_Flags, flags | refresh, TAG_DONE);
        if (!w) { step("  no window"); break; }
        Delay(10);
        step(" draw in it");
        SetAPen(w->RPort, 3);
        RectFill(w->RPort, 10, 20, 100, 60);
        step(" open second window over it");
        w2 = OpenWindowTags(NULL, WA_CustomScreen, (ULONG)s, WA_Left, 80, WA_Top, 60,
                            WA_Width, 200, WA_Height, 120, WA_Title, (ULONG)"two",
                            WA_Flags, flags | refresh, TAG_DONE);
        Delay(10);
        step(" first window to front");
        WindowToFront(w);
        Delay(15);
        step(" first window to back");
        WindowToBack(w);
        Delay(15);
        if (w2) {
            step(" close second window");
            CloseWindow(w2);
            Delay(10);
        }
        step(" SizeWindow +100,+80");
        SizeWindow(w, 100, 80);
        Delay(20);
        step(" SizeWindow -60,-40");
        SizeWindow(w, -60, -40);
        Delay(20);
        step(" MoveWindow +200,+150");
        MoveWindow(w, 200, 150);
        Delay(20);
        step(" MoveWindow -100,-50");
        MoveWindow(w, -100, -50);
        Delay(20);
        step(" ChangeWindowBox");
        ChangeWindowBox(w, 5, 15, 300, 200);
        Delay(20);
        step(" ScrollRaster");
        ScrollRaster(w->RPort, 5, 3, 10, 20, 150, 90);
        Delay(5);
        step(" close window");
        CloseWindow(w);
        Delay(10);
    }
    step("close screen");
    CloseScreen(s);
    step("done");
    FreeArgs(rda);
    return 0;
}
