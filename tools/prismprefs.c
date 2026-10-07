/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laine Jones */
/*
 * PrismPrefs - GadTools editor for ENV:Prism.prefs (see src/prefs.h).
 *
 *   PrismPrefs [FROM=<file>] [USE] [SAVE]
 *
 * Like the system prefs editors: FROM loads another file, USE / SAVE write
 * it out (ENV: / ENV: + ENVARC:) without opening the window. PrismD reads
 * the settings when it starts.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/semaphores.h>
#include <intuition/intuition.h>
#include <intuition/gadgetclass.h>
#include <libraries/gadtools.h>
#include <graphics/displayinfo.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/gadtools.h>
#include <stdio.h>
#include <string.h>
#include "prism.h"
#include "prefs.h"

struct IntuitionBase *IntuitionBase;
struct GfxBase *GfxBase;
struct Library *GadToolsBase;

static const char version[] __attribute__((used)) = "$VER: PrismPrefs 1.0.1 (05.10.2026)";

enum { GAD_LIST = 1, GAD_ON, GAD_HZ, GAD_BOARD, GAD_BLIT, GAD_LOG,
       GAD_SAVE, GAD_USE, GAD_DEFAULTS, GAD_CANCEL, GAD_STATUS, GAD_TEST };

static STRPTR hzLabels[] = { "Default", "60 Hz", "70 Hz", "72 Hz", "75 Hz", NULL };
static STRPTR boardLabels[] = { "Auto", "Picasso II", "ZZ9000", "UAE (native)", NULL };

struct Gui {
    struct Screen *scr;
    APTR vi;
    struct Window *win;
    struct Gadget *glist, *gList, *gOn, *gHz, *gBoard, *gBlit, *gLog, *gStatus;
    struct List modes;
    struct Node node[PREFS_NMODES];
    char text[PREFS_NMODES][48];
    struct PrismPrefs prefs;
    LONG sel;
    BOOL running;                    /* PrismD is up: show which modes are live */
    char status[80];
};

static struct Gui g;

/* ---- helpers -------------------------------------------------------- */

static UWORD rate_index(UBYTE hz)
{
    UWORD i;
    for (i = 0; i < PREFS_NRATES; i++)
        if (prefs_rates[i] == hz)
            return i;
    return 0;
}

static void entry_text(struct Gui *g, int i)
{
    char hz[12];
    BOOL live = g->running &&
                !ModeNotAvailable(PRISM_MONITOR_ID | PREFS_IDLOW(i));

    if (g->prefs.hz[i])
        sprintf(hz, "%u Hz", g->prefs.hz[i]);
    else
        strcpy(hz, "default");
    sprintf(g->text[i], "%-3s %4ux%-4u %2u-bit %-7s %s",
            g->prefs.on[i] ? "on" : "--",
            prefs_sizes[PREFS_SIZE(i)][0], prefs_sizes[PREFS_SIZE(i)][1],
            prefs_depths[PREFS_DEPTH(i)], hz, live ? "live" : "");
}

static void build_list(struct Gui *g)
{
    int i;
    /* no NewList() without amiga.lib: initialise by hand */
    g->modes.lh_Head = (struct Node *)&g->modes.lh_Tail;
    g->modes.lh_Tail = NULL;
    g->modes.lh_TailPred = (struct Node *)&g->modes.lh_Head;
    for (i = 0; i < PREFS_NMODES; i++) {
        entry_text(g, i);
        g->node[i].ln_Name = g->text[i];
        AddTail(&g->modes, &g->node[i]);
    }
}

static void set_status(struct Gui *g, const char *s)
{
    strncpy(g->status, s, sizeof(g->status) - 1);
    if (g->win && g->gStatus)
        GT_SetGadgetAttrs(g->gStatus, g->win, NULL, GTTX_Text, (ULONG)g->status, TAG_END);
}

static void refresh_list(struct Gui *g)
{
    int i;
    GT_SetGadgetAttrs(g->gList, g->win, NULL, GTLV_Labels, ~0UL, TAG_END);
    for (i = 0; i < PREFS_NMODES; i++)
        entry_text(g, i);
    GT_SetGadgetAttrs(g->gList, g->win, NULL, GTLV_Labels, (ULONG)&g->modes,
                      GTLV_Selected, (ULONG)g->sel, TAG_END);
}

/* the right-hand gadgets follow the selected mode / the global settings */
static void show_settings(struct Gui *g)
{
    struct PrismPrefs *p = &g->prefs;
    GT_SetGadgetAttrs(g->gOn, g->win, NULL, GTCB_Checked, (ULONG)p->on[g->sel], TAG_END);
    GT_SetGadgetAttrs(g->gHz, g->win, NULL, GTCY_Active, (ULONG)rate_index(p->hz[g->sel]), TAG_END);
    GT_SetGadgetAttrs(g->gBoard, g->win, NULL, GTCY_Active, (ULONG)p->board, TAG_END);
    GT_SetGadgetAttrs(g->gBlit, g->win, NULL, GTCB_Checked, (ULONG)p->blitter, TAG_END);
    GT_SetGadgetAttrs(g->gLog, g->win, NULL, GTCB_Checked, (ULONG)p->log, TAG_END);
}

static void running_status(struct Gui *g)
{
    struct PrismSem *s;
    char buf[80];

    Forbid();
    s = (struct PrismSem *)FindSemaphore(PRISM_SEMNAME);
    if (s)
        sprintf(buf, "PrismD: %.24s, %lu modes. New settings: restart it.",
                s->boardName ? s->boardName : "?", (unsigned long)s->numModes);
    Permit();
    g->running = s != NULL;
    if (!s)
        strcpy(buf, "PrismD isn't running. It reads these when it starts.");
    strcpy(g->status, buf);
}

static BOOL write_prefs(struct Gui *g, BOOL envarc)
{
    if (!prefs_save(&g->prefs, PREFS_ENV)) {
        set_status(g, "Can't write ENV:Prism.prefs");
        return FALSE;
    }
    if (envarc && !prefs_save(&g->prefs, PREFS_ENVARC)) {
        set_status(g, "Can't write ENVARC:Prism.prefs");
        return FALSE;
    }
    return TRUE;
}

/* ---- Test: show the selected mode for a few seconds ------------------ */

#define TEST_SECS 10

/* Shows mode slot i as it is set in the window (not as saved). With `ask`,
 * a requester afterwards offers to switch off a mode that didn't work.
 * Returns the PRISM_TEST_* result, or -1 when PrismD isn't there. */
static LONG test_mode(struct Gui *g, LONG i, BOOL ask)
{
    struct PrismSem *s;
    LONG (*begin)(ULONG, ULONG, ULONG, ULONG, const char *) = NULL;
    void (*end)(void) = NULL;
    UWORD w = prefs_sizes[PREFS_SIZE(i)][0], h = prefs_sizes[PREFS_SIZE(i)][1];
    UBYTE bits = prefs_depths[PREFS_DEPTH(i)], hz = g->prefs.hz[i];
    char label[96], rate[12];
    LONG rc, t;
    BOOL stop = FALSE;

    Forbid();
    if ((s = (struct PrismSem *)FindSemaphore(PRISM_SEMNAME)) && s->version >= 2) {
        begin = s->testBegin;
        end = s->testEnd;
    }
    Permit();
    if (!begin || !end) {
        set_status(g, "Test needs Prism running (restart with Prism installed).");
        return -1;
    }
    if (hz) sprintf(rate, "%u Hz", hz); else strcpy(rate, "default rate");
    sprintf(label, "PRISM test  %ux%u %u-bit %s  -  back in %d s (any key or click)", w, h,
            bits, rate, TEST_SECS);
    rc = begin(w, h, bits, hz, label);
    if (rc == PRISM_TEST_NOMODE) {
        set_status(g, "The card can't show that mode at that rate.");
        return rc;
    }
    if (rc != PRISM_TEST_OK) {
        set_status(g, rc == PRISM_TEST_BUSY ? "A test is already showing."
                                            : "Not enough video memory free for the test.");
        return rc;
    }
    for (t = 0; t < TEST_SECS * 10 && !stop; t++) {
        Delay(5);
        if (g->win) {
            struct IntuiMessage *im;
            while ((im = GT_GetIMsg(g->win->UserPort))) {
                ULONG cls = im->Class;
                GT_ReplyIMsg(im);
                if (cls == IDCMP_VANILLAKEY || cls == IDCMP_MOUSEBUTTONS || cls == IDCMP_GADGETUP)
                    stop = TRUE;
            }
        } else if (SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) {
            stop = TRUE;
        }
    }
    end();
    if (ask && g->win) {
        struct EasyStruct es = { sizeof(struct EasyStruct), 0, "Prism mode test",
                                 "Did the test picture for\n%s\nlook right on your monitor?",
                                 "Yes|No, switch it off" };
        char what[40];
        sprintf(what, "%ux%u %u-bit %s", w, h, bits, rate);
        if (EasyRequest(g->win, &es, NULL, (ULONG)what)) {
            set_status(g, "Mode tested.");
        } else {
            g->prefs.on[i] = 0;
            refresh_list(g);
            show_settings(g);
            set_status(g, "Mode switched off. Save or Use to keep that.");
        }
    }
    return rc;
}

/* ---- window --------------------------------------------------------- */

static BOOL make_gadgets(struct Gui *g, WORD *innerW, WORD *innerH)
{
    struct NewGadget ng;
    struct Gadget *gad;
    struct TextFont *tf = g->scr->RastPort.Font;
    WORD fw = tf->tf_XSize, fh = tf->tf_YSize, gh = fh + 6, row = gh + 4;
    WORD cl = g->scr->WBorLeft + 8, ct = g->scr->WBorTop + fh + 1 + 6;
    WORD listW = 38 * fw + 24, listH = 12 * fh + 4;
    WORD rx, rw, y, bw, by;

    if (!(gad = CreateContext(&g->glist)))
        return FALSE;
    memset(&ng, 0, sizeof(ng));
    ng.ng_TextAttr = g->scr->Font;
    ng.ng_VisualInfo = g->vi;

    /* right column: label width + gadgets */
    rx = cl + listW + 16 + 8 * fw;
    rw = 12 * fw + 28;

    y = ct + fh + 2;
    ng.ng_LeftEdge = rx; ng.ng_TopEdge = y;
    ng.ng_Width = 26; ng.ng_Height = fh + 3;
    ng.ng_GadgetText = "Offered"; ng.ng_Flags = PLACETEXT_RIGHT; ng.ng_GadgetID = GAD_ON;
    gad = g->gOn = CreateGadget(CHECKBOX_KIND, gad, &ng, GTCB_Scaled, TRUE,
                                GTCB_Checked, (ULONG)g->prefs.on[g->sel], TAG_END);

    y += row;
    ng.ng_TopEdge = y; ng.ng_Width = rw; ng.ng_Height = gh;
    ng.ng_GadgetText = "Refresh"; ng.ng_Flags = PLACETEXT_LEFT; ng.ng_GadgetID = GAD_HZ;
    gad = g->gHz = CreateGadget(CYCLE_KIND, gad, &ng, GTCY_Labels, (ULONG)hzLabels,
                                GTCY_Active, (ULONG)rate_index(g->prefs.hz[g->sel]), TAG_END);

    y += row + fh + 6;
    ng.ng_TopEdge = y;
    ng.ng_GadgetText = "Board"; ng.ng_GadgetID = GAD_BOARD;
    gad = g->gBoard = CreateGadget(CYCLE_KIND, gad, &ng, GTCY_Labels, (ULONG)boardLabels,
                                   GTCY_Active, (ULONG)g->prefs.board, TAG_END);

    y += row;
    ng.ng_TopEdge = y; ng.ng_Width = 26; ng.ng_Height = fh + 3;
    ng.ng_GadgetText = "Use blitter"; ng.ng_Flags = PLACETEXT_RIGHT; ng.ng_GadgetID = GAD_BLIT;
    gad = g->gBlit = CreateGadget(CHECKBOX_KIND, gad, &ng, GTCB_Scaled, TRUE,
                                  GTCB_Checked, (ULONG)g->prefs.blitter, TAG_END);

    y += row;
    ng.ng_TopEdge = y;
    ng.ng_GadgetText = "Debug log"; ng.ng_GadgetID = GAD_LOG;
    gad = g->gLog = CreateGadget(CHECKBOX_KIND, gad, &ng, GTCB_Scaled, TRUE,
                                 GTCB_Checked, (ULONG)g->prefs.log, TAG_END);

    /* status line, then the Save / Use / Defaults / Cancel row */
    by = ct + fh + 2 + listH + 6;
    ng.ng_LeftEdge = cl; ng.ng_TopEdge = by;
    ng.ng_Width = rx + rw - cl; ng.ng_Height = gh;
    ng.ng_GadgetText = NULL; ng.ng_Flags = 0; ng.ng_GadgetID = GAD_STATUS;
    gad = g->gStatus = CreateGadget(TEXT_KIND, gad, &ng, GTTX_Text, (ULONG)g->status,
                                    GTTX_Border, TRUE, TAG_END);

    by += row + 2;
    bw = (rx + rw - cl - 4 * 8) / 5;
    ng.ng_TopEdge = by; ng.ng_Width = bw; ng.ng_Height = gh; ng.ng_Flags = PLACETEXT_IN;
    ng.ng_LeftEdge = cl;                     ng.ng_GadgetText = "Save";     ng.ng_GadgetID = GAD_SAVE;
    gad = CreateGadget(BUTTON_KIND, gad, &ng, TAG_END);
    ng.ng_LeftEdge = cl + (bw + 8);          ng.ng_GadgetText = "Use";      ng.ng_GadgetID = GAD_USE;
    gad = CreateGadget(BUTTON_KIND, gad, &ng, TAG_END);
    ng.ng_LeftEdge = cl + 2 * (bw + 8);      ng.ng_GadgetText = "Test";     ng.ng_GadgetID = GAD_TEST;
    gad = CreateGadget(BUTTON_KIND, gad, &ng, TAG_END);
    ng.ng_LeftEdge = cl + 3 * (bw + 8);      ng.ng_GadgetText = "Defaults"; ng.ng_GadgetID = GAD_DEFAULTS;
    gad = CreateGadget(BUTTON_KIND, gad, &ng, TAG_END);
    ng.ng_LeftEdge = rx + rw - bw;           ng.ng_GadgetText = "Cancel";   ng.ng_GadgetID = GAD_CANCEL;
    gad = CreateGadget(BUTTON_KIND, gad, &ng, TAG_END);

    /* listview last: its scroller sub-gadgets end the chain */
    ng.ng_LeftEdge = cl; ng.ng_TopEdge = ct + fh + 2;
    ng.ng_Width = listW; ng.ng_Height = listH;
    ng.ng_GadgetText = "Modes"; ng.ng_Flags = PLACETEXT_ABOVE | NG_HIGHLABEL; ng.ng_GadgetID = GAD_LIST;
    gad = g->gList = CreateGadget(LISTVIEW_KIND, gad, &ng, GTLV_Labels, (ULONG)&g->modes,
                                  GTLV_ShowSelected, 0, GTLV_Selected, (ULONG)g->sel,
                                  GTLV_MakeVisible, (ULONG)g->sel, TAG_END);
    if (!gad)
        return FALSE;

    *innerW = rx + rw + 8 - g->scr->WBorLeft;
    *innerH = by + gh + 6 - (g->scr->WBorTop + fh + 1);
    return TRUE;
}

static BOOL open_window(struct Gui *g)
{
    WORD iw, ih;

    if (!(g->scr = LockPubScreen(NULL)) || !(g->vi = GetVisualInfo(g->scr, TAG_END)))
        return FALSE;
    if (!make_gadgets(g, &iw, &ih))
        return FALSE;
    g->win = OpenWindowTags(NULL,
        WA_Title, (ULONG)"PrismRTG Preferences",
        WA_Left, 20, WA_Top, g->scr->BarHeight + 4,
        WA_InnerWidth, iw, WA_InnerHeight, ih,
        WA_AutoAdjust, TRUE,
        WA_Flags, WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_CLOSEGADGET | WFLG_ACTIVATE |
                  WFLG_SMART_REFRESH,
        WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW | IDCMP_VANILLAKEY |
                  IDCMP_MOUSEBUTTONS |
                  BUTTONIDCMP | CHECKBOXIDCMP | CYCLEIDCMP | LISTVIEWIDCMP,
        WA_Gadgets, (ULONG)g->glist,
        WA_PubScreen, (ULONG)g->scr,
        TAG_END);
    if (!g->win)
        return FALSE;
    GT_RefreshWindow(g->win, NULL);
    return TRUE;
}

/* returns when the window should close */
static void event_loop(struct Gui *g)
{
    BOOL done = FALSE;

    while (!done) {
        struct IntuiMessage *im;
        WaitPort(g->win->UserPort);
        while (!done && (im = GT_GetIMsg(g->win->UserPort))) {
            ULONG cls = im->Class;
            UWORD code = im->Code;
            struct Gadget *gad = (struct Gadget *)im->IAddress;
            GT_ReplyIMsg(im);

            switch (cls) {
            case IDCMP_CLOSEWINDOW:
                done = TRUE;
                break;
            case IDCMP_VANILLAKEY:
                if (code == 27)                  /* Esc = Cancel */
                    done = TRUE;
                break;
            case IDCMP_REFRESHWINDOW:
                GT_BeginRefresh(g->win);
                GT_EndRefresh(g->win, TRUE);
                break;
            case IDCMP_GADGETUP:
                switch (gad->GadgetID) {
                case GAD_LIST:
                    g->sel = code;
                    show_settings(g);
                    break;
                case GAD_ON:
                    g->prefs.on[g->sel] = (gad->Flags & GFLG_SELECTED) ? 1 : 0;
                    refresh_list(g);
                    break;
                case GAD_HZ:
                    g->prefs.hz[g->sel] = prefs_rates[code < PREFS_NRATES ? code : 0];
                    refresh_list(g);
                    break;
                case GAD_BOARD:
                    g->prefs.board = code < PB_COUNT ? code : PB_AUTO;
                    break;
                case GAD_BLIT:
                    g->prefs.blitter = (gad->Flags & GFLG_SELECTED) ? 1 : 0;
                    break;
                case GAD_LOG:
                    g->prefs.log = (gad->Flags & GFLG_SELECTED) ? 1 : 0;
                    break;
                case GAD_TEST:
                    test_mode(g, g->sel, TRUE);
                    break;
                case GAD_SAVE:
                    done = write_prefs(g, TRUE);
                    break;
                case GAD_USE:
                    done = write_prefs(g, FALSE);
                    break;
                case GAD_DEFAULTS:
                    prefs_default(&g->prefs);
                    refresh_list(g);
                    show_settings(g);
                    set_status(g, "Defaults - every mode the board can show");
                    break;
                case GAD_CANCEL:
                    done = TRUE;
                    break;
                }
                break;
            }
        }
    }
}

/* ---- main ----------------------------------------------------------- */

#define TEMPLATE "FROM/K,USE/S,SAVE/S,TEST/K/N"

int main(void)
{
    LONG args[4] = { 0 };
    struct RDArgs *rda;
    int rc = 20;

    if (!(rda = ReadArgs(TEMPLATE, args, NULL))) {
        PrintFault(IoErr(), "PrismPrefs");
        return 20;
    }
    IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 39);
    GfxBase = (struct GfxBase *)OpenLibrary("graphics.library", 39);
    GadToolsBase = OpenLibrary("gadtools.library", 39);
    if (!IntuitionBase || !GfxBase || !GadToolsBase)
        goto out;

    if (args[0]) {
        if (!prefs_load(&g.prefs, (const char *)args[0])) {
            printf("PrismPrefs: can't read %s\n", (char *)args[0]);
            rc = 10;
            goto out;
        }
    } else if (!prefs_load(&g.prefs, PREFS_ENV)) {
        prefs_load(&g.prefs, PREFS_ENVARC);
    }

    if (args[1] || args[2]) {                /* USE / SAVE: no window */
        rc = write_prefs(&g, args[2] != 0) ? 0 : 10;
        if (rc)
            printf("PrismPrefs: %s\n", g.status);
        goto out;
    }

    if (args[3]) {                           /* TEST=<slot>: no window */
        LONG slot = *(LONG *)args[3];
        if (slot < 0 || slot >= PREFS_NMODES) {
            printf("PrismPrefs: TEST takes a mode slot, 0 to %d\n", PREFS_NMODES - 1);
            goto out;
        }
        rc = test_mode(&g, slot, FALSE);
        printf("PrismPrefs: test of slot %ld: %s\n", (long)slot,
               rc == 0 ? "shown" : g.status);
        rc = rc ? 5 : 0;
        goto out;
    }

    running_status(&g);
    build_list(&g);
    if (open_window(&g)) {
        event_loop(&g);
        rc = 0;
    }

out:
    if (g.win) CloseWindow(g.win);
    if (g.glist) FreeGadgets(g.glist);
    if (g.vi) FreeVisualInfo(g.vi);
    if (g.scr) UnlockPubScreen(NULL, g.scr);
    if (GadToolsBase) CloseLibrary(GadToolsBase);
    if (GfxBase) CloseLibrary((struct Library *)GfxBase);
    if (IntuitionBase) CloseLibrary((struct Library *)IntuitionBase);
    FreeArgs(rda);
    return rc;
}
