/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laine Jones */
/* muitime: how long MUI takes to open (lay out and draw) and close a busy
 * window - framed groups, buttons, strings, cycles, a 40-line list, sliders,
 * gauges - on the screen in front. Opens it N times (after one warm-up) and
 * prints the average milliseconds, timed with the E-clock.
 *
 *   muitime [N=10]
 *
 * No MUI SDK in the toolchain: the class names and tags are MUI's public
 * values (TAG_USER based), as AROS's Zune headers list them. */
#include <exec/types.h>
#include <exec/libraries.h>
#include <devices/timer.h>
#include <utility/tagitem.h>
#include <intuition/classusr.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/timer.h>
#include <stdio.h>
#include <stdlib.h>

#define MUIA(x) (TAG_USER | (x))
#define MUIA_Application_Title  MUIA(0x004281b8)
#define MUIA_Application_Window MUIA(0x0042bfe0)
#define MUIA_Window_Title       MUIA(0x0042ad3d)
#define MUIA_Window_RootObject  MUIA(0x0042cba5)
#define MUIA_Window_Open        MUIA(0x00428aa0)
#define MUIA_Group_Child        MUIA(0x004226e6)
#define MUIA_Group_Horiz        MUIA(0x0042536b)
#define MUIA_Group_Columns      MUIA(0x0042f416)
#define MUIA_Frame              MUIA(0x0042ac64)
#define MUIA_FrameTitle         MUIA(0x0042d1c7)
#define MUIA_InputMode          MUIA(0x0042fb04)
#define MUIA_Text_Contents      MUIA(0x0042f8dc)
#define MUIA_String_Contents    MUIA(0x00428ffd)
#define MUIA_Cycle_Entries      MUIA(0x00420629)
#define MUIA_Listview_List      MUIA(0x0042bcce)
#define MUIA_List_SourceArray   MUIA(0x0042c0a0)
#define MUIA_Gauge_Current      MUIA(0x0042f0dd)
#define MUIA_Gauge_Horiz        MUIA(0x004232dd)
#define MUIA_Numeric_Max        MUIA(0x0042d78a)
#define MUIA_Numeric_Value      MUIA(0x0042ae3a)
#define FRAME_BUTTON 1
#define FRAME_TEXT   3
#define FRAME_STRING 4
#define FRAME_READLIST 5
#define FRAME_GROUP  9

struct Library *MUIMasterBase;
struct Device *TimerBase;

static Object *mui_new(const char *cls, ULONG *tags)
{
    register Object *d0 __asm("d0");
    register const char *a0 __asm("a0") = cls;
    register ULONG *a1 __asm("a1") = tags;
    register struct Library *a6 __asm("a6") = MUIMasterBase;
    __asm volatile ("jsr -30(%%a6)" : "=r"(d0), "+r"(a0), "+r"(a1) : "r"(a6) : "d1", "cc", "memory");
    return d0;
}
static void mui_dispose(Object *o)
{
    register Object *a0 __asm("a0") = o;
    register struct Library *a6 __asm("a6") = MUIMasterBase;
    __asm volatile ("jsr -36(%%a6)" : "+r"(a0) : "r"(a6) : "d0", "d1", "a1", "cc", "memory");
}

#define T(...) ((ULONG []){ __VA_ARGS__, TAG_DONE })

static Object *button(const char *s)
{
    return mui_new("Text.mui", T(MUIA_Frame, FRAME_BUTTON, MUIA_InputMode, 1,
                                 MUIA_Text_Contents, (ULONG)s));
}
static Object *label(const char *s)
{
    return mui_new("Text.mui", T(MUIA_Text_Contents, (ULONG)s));
}

static const char *cyc[] = { "First choice", "Second choice", "Third choice", NULL };
static const char *lines[41];
static char linebuf[40][32];

static Object *window_root(void)
{
    Object *btns, *form, *lst, *meters, *row;
    int i;
    btns = mui_new("Group.mui", T(MUIA_Group_Horiz, TRUE, MUIA_Frame, FRAME_GROUP,
        MUIA_FrameTitle, (ULONG)"Buttons",
        MUIA_Group_Child, (ULONG)button("Open"), MUIA_Group_Child, (ULONG)button("Save"),
        MUIA_Group_Child, (ULONG)button("Save as"), MUIA_Group_Child, (ULONG)button("Print"),
        MUIA_Group_Child, (ULONG)button("Settings"), MUIA_Group_Child, (ULONG)button("Help")));
    form = mui_new("Group.mui", T(MUIA_Group_Columns, 2, MUIA_Frame, FRAME_GROUP,
        MUIA_FrameTitle, (ULONG)"Form",
        MUIA_Group_Child, (ULONG)label("Name"),
        MUIA_Group_Child, (ULONG)mui_new("String.mui", T(MUIA_Frame, FRAME_STRING, MUIA_String_Contents, (ULONG)"Laine")),
        MUIA_Group_Child, (ULONG)label("Host"),
        MUIA_Group_Child, (ULONG)mui_new("String.mui", T(MUIA_Frame, FRAME_STRING, MUIA_String_Contents, (ULONG)"amiga.example")),
        MUIA_Group_Child, (ULONG)label("Port"),
        MUIA_Group_Child, (ULONG)mui_new("String.mui", T(MUIA_Frame, FRAME_STRING, MUIA_String_Contents, (ULONG)"7800")),
        MUIA_Group_Child, (ULONG)label("Mode"),
        MUIA_Group_Child, (ULONG)mui_new("Cycle.mui", T(MUIA_Cycle_Entries, (ULONG)cyc)),
        MUIA_Group_Child, (ULONG)label("Depth"),
        MUIA_Group_Child, (ULONG)mui_new("Cycle.mui", T(MUIA_Cycle_Entries, (ULONG)cyc)),
        MUIA_Group_Child, (ULONG)label("Speed"),
        MUIA_Group_Child, (ULONG)mui_new("Slider.mui", T(MUIA_Numeric_Max, 100, MUIA_Numeric_Value, 40))));
    for (i = 0; i < 40; i++) {
        sprintf(linebuf[i], "List entry number %d", i + 1);
        lines[i] = linebuf[i];
    }
    lines[40] = NULL;
    lst = mui_new("Listview.mui", T(MUIA_Listview_List,
        (ULONG)mui_new("List.mui", T(MUIA_Frame, FRAME_READLIST, MUIA_List_SourceArray, (ULONG)lines))));
    meters = mui_new("Group.mui", T(MUIA_Frame, FRAME_GROUP, MUIA_FrameTitle, (ULONG)"Progress",
        MUIA_Group_Child, (ULONG)mui_new("Gauge.mui", T(MUIA_Gauge_Horiz, TRUE, MUIA_Frame, FRAME_TEXT, MUIA_Gauge_Current, 30)),
        MUIA_Group_Child, (ULONG)mui_new("Gauge.mui", T(MUIA_Gauge_Horiz, TRUE, MUIA_Frame, FRAME_TEXT, MUIA_Gauge_Current, 60)),
        MUIA_Group_Child, (ULONG)mui_new("Gauge.mui", T(MUIA_Gauge_Horiz, TRUE, MUIA_Frame, FRAME_TEXT, MUIA_Gauge_Current, 90)),
        MUIA_Group_Child, (ULONG)mui_new("Text.mui", T(MUIA_Frame, FRAME_TEXT, MUIA_Text_Contents,
                                                       (ULONG)"A line of status text in a frame"))));
    row = mui_new("Group.mui", T(MUIA_Group_Horiz, TRUE,
        MUIA_Group_Child, (ULONG)form, MUIA_Group_Child, (ULONG)lst));
    return mui_new("Group.mui", T(MUIA_Group_Child, (ULONG)btns, MUIA_Group_Child, (ULONG)row,
                                  MUIA_Group_Child, (ULONG)meters));
}

static double ms(struct EClockVal *a, struct EClockVal *b, ULONG freq)
{
    double t = ((double)b->ev_hi - a->ev_hi) * 4294967296.0 + ((double)b->ev_lo - a->ev_lo);
    return t * 1000.0 / freq;
}

int main(int argc, char **argv)
{
    struct timerequest tr;
    struct EClockVal t0, t1, t2;
    Object *app, *win, *root;
    double open = 0, shut = 0, best = 1e9;
    ULONG freq;
    int n = 10, i;

    if (argc > 1 && sscanf(argv[1] + (argv[1][0] == 'N' && argv[1][1] == '=' ? 2 : 0), "%d", &n) != 1)
        n = 10;
    if (!(MUIMasterBase = OpenLibrary("muimaster.library", 11))) {
        puts("muitime: no muimaster.library");
        return 20;
    }
    if (OpenDevice("timer.device", UNIT_ECLOCK, (struct IORequest *)&tr, 0)) {
        CloseLibrary(MUIMasterBase);
        return 20;
    }
    TimerBase = tr.tr_node.io_Device;
    root = window_root();
    win = mui_new("Window.mui", T(MUIA_Window_Title, (ULONG)"muitime", MUIA_Window_RootObject, (ULONG)root));
    app = win ? mui_new("Application.mui", T(MUIA_Application_Title, (ULONG)"muitime",
                                             MUIA_Application_Window, (ULONG)win)) : NULL;
    if (!app) {
        puts("muitime: can't create the window");
        CloseDevice((struct IORequest *)&tr);
        CloseLibrary(MUIMasterBase);
        return 20;
    }
    printf("muitime: muimaster.library %u.%u\n", MUIMasterBase->lib_Version, MUIMasterBase->lib_Revision);
    for (i = -1; i < n; i++) {               /* -1: the warm-up, not counted */
        freq = ReadEClock(&t0);
        SetAttrs(win, MUIA_Window_Open, TRUE, TAG_DONE);
        ReadEClock(&t1);
        SetAttrs(win, MUIA_Window_Open, FALSE, TAG_DONE);
        ReadEClock(&t2);
        if (i >= 0) {
            double o = ms(&t0, &t1, freq);
            open += o;
            shut += ms(&t1, &t2, freq);
            if (o < best) best = o;
        }
    }
    printf("muitime: open %.1f ms (best %.1f), close %.1f ms, average of %d\n",
           open / n, best, shut / n, n);
    mui_dispose(app);
    CloseDevice((struct IORequest *)&tr);
    CloseLibrary(MUIMasterBase);
    return 0;
}
