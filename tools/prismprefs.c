/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laine Jones
 * Copyright (C) 2026 Stefan Reinauer */
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
#include <dos/dostags.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/gadtools.h>
#include <stdio.h>
#include <string.h>
#include "prism.h"
#include "prefs.h"
#include "catalog.h"

struct IntuitionBase *IntuitionBase;
struct GfxBase *GfxBase;
struct Library *GadToolsBase;

static const char version[] __attribute__((used)) = "$VER: PrismPrefs " PRISM_RELEASE " (" PRISM_RELDATE ")";

enum { GAD_LIST = 1, GAD_ON, GAD_HZ, GAD_BOARD, GAD_BLIT, GAD_LOG,
       GAD_SAVE, GAD_USE, GAD_DEFAULTS, GAD_CANCEL, GAD_STATUS, GAD_TEST, GAD_BGR,
       GAD_DRAG, GAD_SOFTPTR, GAD_HWOVL, GAD_PROMOTE, GAD_PLIST, GAD_PROG, GAD_PADD, GAD_PKEEP, GAD_PDEL,
       GAD_UPDATES, GAD_CHECK, GAD_W, GAD_H, GAD_SETSIZE, GAD_GAMMA, GAD_POWER, GAD_MIRROR };

static STRPTR hzLabels[] = { "Default", "60 Hz", "70 Hz", "72 Hz", "75 Hz", NULL };
/* the Board cycle: Auto three ways - every board PrismD finds (two at
 * most), the same with the two swapped (PRIMARY=SECOND: the second board
 * found is the primary, with the PRISM: modes and Workbench), or only the
 * first board (SECONDBOARD=OFF) - then the PB_* boards after Auto */
#define BOARD_AUTOS 3
static STRPTR boardLabels[] = { "Auto", "Auto, swapped", "Auto, 1 board", "Picasso II", "ZZ9000",
                                "P96 driver", "UAE (native)", "Other (BOARD= in the prefs file)", NULL };
/* LOG=OFF / ON / SYNC, in that order (prefs.log 0, 1, 2) */
static STRPTR logLabels[] = { "Off", "On", "Sync", NULL };
/* UPDATES= (UPD_OFF, UPD_GITHUB, UPD_MIRROR, UPD_BOTH) */
static STRPTR updLabels[] = { "Off", "GitHub", "Mirror", "GitHub, then mirror", NULL };
static const char *const updSource[] = { "OFF", "GITHUB", "MIRROR", "BOTH" };
/* GAMMA= in tenths */
static STRPTR gammaLabels[] = { "1.0 (off)", "1.2", "1.4", "1.6", "1.8", "2.0", "0.8", NULL };
static const UBYTE gammaValues[] = { 10, 12, 14, 16, 18, 20, 8 };

static UWORD gamma_index(UBYTE g)
{
    UWORD i;
    for (i = 0; i < sizeof(gammaValues); i++)
        if (gammaValues[i] == g)
            return i;
    return 0;
}

struct Gui {
    struct Screen *scr;
    BOOL ownScr;                     /* scr is our own tall screen, not Workbench */
    APTR vi;
    struct Window *win;
    struct Gadget *glist, *gList, *gOn, *gHz, *gBoard, *gBlit, *gLog, *gBgr, *gDrag, *gSoft, *gHwOvl, *gStatus;
    struct Gadget *gPromote, *gPList, *gProg, *gUpdates, *gW, *gH, *gSet, *gGamma, *gPower, *gMirror;
    struct List modes;
    struct Node node[PREFS_NMODES];
    char text[PREFS_NMODES][48];
    /* the promotion list: programs promoted or kept whatever the switch says */
    struct List progs;
    struct Node pnode[PREFS_NPROGS];
    char ptext[PREFS_NPROGS][48];
    LONG psel;                       /* selected program, -1 none              */
    struct PrismPrefs prefs;
    LONG sel;
    BOOL running;                    /* PrismD is up: show which modes are live */
    UBYTE boards;                    /* the boards it drives (1 or 2)          */
    UBYTE testOn;                    /* the board the Test button uses (0, 1)  */
    char status[80];
};

static struct Gui g;

/* ---- helpers -------------------------------------------------------- */

static void set_status(struct Gui *g, const char *s);
static void init_texts(void)
{
    hzLabels[0] = (STRPTR)S(ID_DEFAULT, "Default");
    hzLabels[1] = (STRPTR)S(ID_60_HZ, "60 Hz");
    hzLabels[2] = (STRPTR)S(ID_70_HZ, "70 Hz");
    hzLabels[3] = (STRPTR)S(ID_72_HZ, "72 Hz");
    hzLabels[4] = (STRPTR)S(ID_75_HZ, "75 Hz");
    boardLabels[0] = (STRPTR)S(ID_AUTO, "Auto");
    boardLabels[1] = (STRPTR)S(ID_AUTO_SWAPPED, "Auto, swapped");
    boardLabels[2] = (STRPTR)S(ID_AUTO_1_BOARD, "Auto, 1 board");
    boardLabels[3] = (STRPTR)S(ID_PICASSO_II, "Picasso II");
    boardLabels[4] = (STRPTR)S(ID_ZZ9000, "ZZ9000");
    boardLabels[5] = (STRPTR)S(ID_P96_DRIVER, "P96 driver");
    boardLabels[6] = (STRPTR)S(ID_UAE_NATIVE, "UAE (native)");
    boardLabels[7] = (STRPTR)S(ID_OTHER_BOARD_IN_THE_PREFS_FILE, "Other (BOARD= in the prefs file)");
    logLabels[0] = (STRPTR)S(ID_OFF, "Off");
    logLabels[1] = (STRPTR)S(ID_ON, "On");
    logLabels[2] = (STRPTR)S(ID_SYNC, "Sync");
    updLabels[0] = (STRPTR)S(ID_OFF, "Off");
    updLabels[1] = (STRPTR)S(ID_GITHUB, "GitHub");
    updLabels[2] = (STRPTR)S(ID_MIRROR, "Mirror");
    updLabels[3] = (STRPTR)S(ID_GITHUB_THEN_MIRROR, "GitHub, then mirror");
    gammaLabels[0] = (STRPTR)S(ID_1_0_OFF, "1.0 (off)");
    gammaLabels[1] = (STRPTR)S(ID_1_2, "1.2");
    gammaLabels[2] = (STRPTR)S(ID_1_4, "1.4");
    gammaLabels[3] = (STRPTR)S(ID_1_6, "1.6");
    gammaLabels[4] = (STRPTR)S(ID_1_8, "1.8");
    gammaLabels[5] = (STRPTR)S(ID_2_0, "2.0");
    gammaLabels[6] = (STRPTR)S(ID_0_8, "0.8");
}


/* What a Board entry means, with the boards PrismD drives now by name and
 * expansion address (two of one kind tell apart by it). */
static void board_status(struct Gui *g, UWORD code)
{
    struct PrismSem *s;
    char buf[80];
    Forbid();
    s = (struct PrismSem *)FindSemaphore(PRISM_SEMNAME);
    if (s && s->version >= 6 && s->numBoards > 1 && code < 2) {
        /* the primary as running now, and which one this entry makes it
         * (from the next start): running swapped or not, "swapped" means
         * the other one of the two */
        BOOL nowSwapped = s->boardsSwapped;
        BOOL other = (code == 1) != nowSwapped;
        snprintf(buf, sizeof(buf), S(ID_PRIMARY_FROM_THE_NEXT_START, "Primary from the next start: %.20s @%08lx"),
                other ? (s->board2Name ? s->board2Name : "?") : (s->boardName ? s->boardName : "?"),
                (unsigned long)s->boardAddr[other ? 1 : 0]);
        Permit();
        set_status(g, buf);
        return;
    }
    Permit();
    set_status(g, code == 2 ? S(ID_AUTO_ONLY_THE_FIRST_BOARD_PRISMD_FINDS, "Auto: only the first board PrismD finds") :
                  code == 1 ? S(ID_AUTO_SWAPPED_THE_SECOND_BOARD_FOUND_IS_T, "Auto, swapped: the second board found is the primary") :
                              S(ID_AUTO_EVERY_BOARD_PRISMD_FINDS_TWO_AT_MOS, "Auto: every board PrismD finds (two at most)"));
}

/* the Board cycle's entry for the prefs, and back */
static UWORD board_entry(const struct PrismPrefs *p)
{
    if (p->board != PB_AUTO)
        return p->board + BOARD_AUTOS - 1;
    return !p->second ? 2 : p->swapBoards ? 1 : 0;
}

static void board_from_entry(struct PrismPrefs *p, UWORD code)
{
    if (code < BOARD_AUTOS) {
        p->board = PB_AUTO;
        p->second = code != 2;
        p->swapBoards = code == 1;
    } else {
        p->board = code - (BOARD_AUTOS - 1) < PB_COUNT ? code - (BOARD_AUTOS - 1) : PB_AUTO;
    }
}

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
    /* which boards show it now: the first's ModeID and the second's */
    BOOL live1 = g->running && !ModeNotAvailable(PRISM_CARD_ID(0) | PREFS_IDLOW(i));
    BOOL live2 = g->running && g->boards > 1 && !ModeNotAvailable(PRISM_CARD_ID(1) | PREFS_IDLOW(i));

    UWORD w, h;

    if (g->prefs.hz[i])
        sprintf(hz, "%u Hz", g->prefs.hz[i]);
    else
        strcpy(hz, "default");
    if (!prefs_size(&g->prefs, i, &w, &h)) {
        /* a custom size not set yet: pick it and give it a size below */
        snprintf(g->text[i], sizeof(g->text[i]), "    custom %d  %2u-bit  (no size yet)",
                PREFS_SIZE(i) - PREFS_NFIXED + 1, prefs_depths[PREFS_DEPTH(i)]);
        return;
    }
    snprintf(g->text[i], sizeof(g->text[i]), "%-3.3s %4ux%-4u %2u-bit %-7s %s",
            g->prefs.on[i] ? S(ID_ON_LOWER, "on") : "--", w, h,
            prefs_depths[PREFS_DEPTH(i)], hz,
            live1 && live2 ? S(ID_LIVE_1_2, "live 1+2") : live1 ? (g->boards > 1 ? S(ID_LIVE_1, "live 1") : S(ID_LIVE, "live")) : live2 ? S(ID_LIVE_2, "live 2") : "");
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

static void prog_text(struct Gui *g, int i)
{
    snprintf(g->ptext[i], sizeof(g->ptext[i]), "%-28.28s %.12s", g->prefs.prog[i].name,
            g->prefs.prog[i].keep ? S(ID_KEPT, "kept") : S(ID_PROMOTED, "promoted"));
}

static void build_progs(struct Gui *g)
{
    int i;
    g->progs.lh_Head = (struct Node *)&g->progs.lh_Tail;
    g->progs.lh_Tail = NULL;
    g->progs.lh_TailPred = (struct Node *)&g->progs.lh_Head;
    for (i = 0; i < g->prefs.nprog; i++) {
        prog_text(g, i);
        g->pnode[i].ln_Name = g->ptext[i];
        AddTail(&g->progs, &g->pnode[i]);
    }
}

static void refresh_progs(struct Gui *g)
{
    GT_SetGadgetAttrs(g->gPList, g->win, NULL, GTLV_Labels, ~0UL, TAG_END);
    build_progs(g);
    if (g->psel >= g->prefs.nprog)
        g->psel = g->prefs.nprog - 1;
    GT_SetGadgetAttrs(g->gPList, g->win, NULL, GTLV_Labels, (ULONG)&g->progs,
                      GTLV_Selected, g->psel < 0 ? ~0UL : (ULONG)g->psel, TAG_END);
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
    GT_SetGadgetAttrs(g->gBoard, g->win, NULL, GTCY_Active, (ULONG)board_entry(p), TAG_END);
    GT_SetGadgetAttrs(g->gBlit, g->win, NULL, GTCB_Checked, (ULONG)p->blitter, TAG_END);
    GT_SetGadgetAttrs(g->gLog, g->win, NULL, GTCY_Active, (ULONG)(p->log <= 2 ? p->log : 1), TAG_END);
    GT_SetGadgetAttrs(g->gBgr, g->win, NULL, GTCB_Checked, (ULONG)p->clutBGR, TAG_END);
    GT_SetGadgetAttrs(g->gDrag, g->win, NULL, GTCB_Checked, (ULONG)p->dragging, TAG_END);
    GT_SetGadgetAttrs(g->gSoft, g->win, NULL, GTCB_Checked, (ULONG)p->softwarePointer, TAG_END);
    GT_SetGadgetAttrs(g->gHwOvl, g->win, NULL, GTCB_Checked, (ULONG)p->hwOverlay, TAG_END);
    GT_SetGadgetAttrs(g->gPromote, g->win, NULL, GTCB_Checked, (ULONG)p->promote, TAG_END);
    GT_SetGadgetAttrs(g->gUpdates, g->win, NULL, GTCY_Active, (ULONG)(p->updates < UPD_COUNT ? p->updates : 0), TAG_END);
    GT_SetGadgetAttrs(g->gGamma, g->win, NULL, GTCY_Active, (ULONG)gamma_index(p->gamma), TAG_END);
    GT_SetGadgetAttrs(g->gPower, g->win, NULL, GTIN_Number, (ULONG)p->powerSave, TAG_END);
    GT_SetGadgetAttrs(g->gMirror, g->win, NULL, GTCB_Checked, (ULONG)p->mirrorChip, TAG_END);
    {
        /* the size fields follow a custom slot and are ghosted for a fixed one */
        int k = PREFS_SIZE(g->sel) - PREFS_NFIXED;
        BOOL custom = PREFS_IS_CUSTOM(PREFS_SIZE(g->sel));
        GT_SetGadgetAttrs(g->gW, g->win, NULL, GA_Disabled, !custom,
                          GTIN_Number, custom ? (ULONG)p->custom[k][0] : 0, TAG_END);
        GT_SetGadgetAttrs(g->gH, g->win, NULL, GA_Disabled, !custom,
                          GTIN_Number, custom ? (ULONG)p->custom[k][1] : 0, TAG_END);
        GT_SetGadgetAttrs(g->gSet, g->win, NULL, GA_Disabled, !custom, TAG_END);
    }
}

/* Set size: the selected custom slot takes the Width and Height fields */
static void set_size(struct Gui *g)
{
    int k = PREFS_SIZE(g->sel) - PREFS_NFIXED;
    LONG w = ((struct StringInfo *)g->gW->SpecialInfo)->LongInt;
    LONG h = ((struct StringInfo *)g->gH->SpecialInfo)->LongInt;
    if (!PREFS_IS_CUSTOM(PREFS_SIZE(g->sel))) {
        set_status(g, S(ID_PICK_ONE_OF_THE_FOUR_CUSTOM_ENTRIES_IN_T, "Pick one of the four custom entries in the list first"));
        return;
    }
    if (w < 320 || w > 4096 || (w & 7) || h < 200 || h > 4096) {
        set_status(g, S(ID_WIDTH_320_TO_4096_IN_STEPS_OF_8_HEIGHT_2, "Width 320 to 4096 in steps of 8, height 200 to 4096"));
        return;
    }
    for (k = 0; k < PREFS_NFIXED + PREFS_NEXTRA; k++)
        if (k < PREFS_NFIXED ? prefs_sizes[k][0] == w && prefs_sizes[k][1] == h
                             : prefs_extra[k - PREFS_NFIXED][0] == w && prefs_extra[k - PREFS_NFIXED][1] == h) {
            set_status(g, S(ID_THAT_SIZE_IS_IN_THE_LIST_ALREADY, "That size is in the list already"));
            return;
        }
    k = PREFS_SIZE(g->sel) - PREFS_NFIXED;
    g->prefs.custom[k][0] = w;
    g->prefs.custom[k][1] = h;
    refresh_list(g);
    set_status(g, S(ID_SET_TEST_SHOWS_IT_NOW_PRISMD_OFFERS_IT_A, "Set. Test shows it now; PrismD offers it after a restart (if the board can)"));
}

/* ---- Check now: PrismUpdate, with the settings as they are in the window ---- */

static void check_update(struct Gui *g)
{
    char cmd[320], line[160];
    BPTR f;
    LONG rc;

    if (g->prefs.updates == UPD_OFF) {
        set_status(g, S(ID_UPDATES_ARE_OFF_CHOOSE_GITHUB_OR_A_MIRRO, "Updates are off: choose GitHub or a mirror first"));
        return;
    }
    set_status(g, S(ID_ASKING_FOR_THE_LATEST_RELEASE, "Asking for the latest release..."));
    sprintf(cmd, "C:PrismUpdate SOURCE=%s%s%s >T:PrismUpdate.out", updSource[g->prefs.updates],
            g->prefs.mirror[0] ? " MIRROR=" : "", g->prefs.mirror);
    rc = SystemTags((STRPTR)cmd, SYS_Input, 0, SYS_Output, 0, NP_StackSize, 65536, TAG_DONE);
    /* its last line is the result (a note about an unverified connection
     * can come before it) */
    line[0] = 0;
    if ((f = Open((STRPTR)"T:PrismUpdate.out", MODE_OLDFILE))) {
        char one[160];
        while (FGets(f, (STRPTR)one, sizeof(one))) {
            char *e = strpbrk(one, "\r\n");
            if (e) *e = 0;
            if (one[0])
                strcpy(line, one);
        }
        Close(f);
    }
    if (rc == -1 || !line[0]) {
        set_status(g, S(ID_C_PRISMUPDATE_IS_NOT_INSTALLED, "C:PrismUpdate is not installed"));
        return;
    }
    /* "PrismUpdate: " off the front: the status line is short */
    set_status(g, strncmp(line, "PrismUpdate: ", 13) ? line : line + 13);
    if (rc == RETURN_WARN) {
        struct EasyStruct es = { sizeof(struct EasyStruct), 0, "PrismRTG update", "%s\n\n"
                                 "Download it and run its installer now?\n"
                                 "The installer asks its usual questions; the update\n"
                                 "takes effect at the next reboot.", "Download and install|Not now" };
        if (EasyRequest(g->win, &es, NULL, (ULONG)(line + (strncmp(line, "PrismUpdate: ", 13) ? 0 : 13)))) {
            sprintf(cmd, "C:PrismUpdate GET SOURCE=%s%s%s", updSource[g->prefs.updates],
                    g->prefs.mirror[0] ? " MIRROR=" : "", g->prefs.mirror);
            SystemTags((STRPTR)cmd, SYS_Asynch, TRUE, SYS_Input, 0,
                       SYS_Output, (ULONG)Open((STRPTR)"CON:0/40/640/200/PrismRTG update/CLOSE/WAIT", MODE_NEWFILE),
                       NP_StackSize, 65536, TAG_DONE);
            set_status(g, S(ID_THE_UPDATE_RUNS_IN_ITS_OWN_WINDOW, "The update runs in its own window"));
        }
    }
}

static void running_status(struct Gui *g)
{
    struct PrismSem *s;
    char buf[80];

    Forbid();
    s = (struct PrismSem *)FindSemaphore(PRISM_SEMNAME);
    g->boards = s && s->version >= 6 && s->numBoards > 1 ? 2 : 1;
    if (s && g->boards > 1)
        snprintf(buf, sizeof(buf), S(ID_PRISMD_TWO_BOARDS_NEW_SETTINGS, "PrismD: %.18s + %.18s. New settings: restart it."),
                s->boardName ? s->boardName : "?", s->board2Name ? s->board2Name : "?");
    else if (s)
        snprintf(buf, sizeof(buf), S(ID_PRISMD_MODES_NEW_SETTINGS, "PrismD: %.24s, %lu modes. New settings: restart it."),
                s->boardName ? s->boardName : "?", (unsigned long)s->numModes);
    Permit();
    g->running = s != NULL;
    if (!s)
        snprintf(buf, sizeof(buf), "%s", S(ID_PRISMD_ISN_T_RUNNING_IT_READS_THESE, "PrismD isn't running. It reads these when it starts."));
    strcpy(g->status, buf);
}

static BOOL write_prefs(struct Gui *g, BOOL envarc)
{
    struct PrismSem *s;
    if (g->gPower) {                     /* typed in without Return: taken as it stands */
        LONG m = ((struct StringInfo *)g->gPower->SpecialInfo)->LongInt;
        if (m >= 0 && m <= 240)
            g->prefs.powerSave = m;
    }
    /* Sleep takes effect on a running PrismD at once (semaphore version 8) */
    Forbid();
    s = (struct PrismSem *)FindSemaphore(PRISM_SEMNAME);
    if (s && s->version >= 8 && s->powerSave)
        *s->powerSave = g->prefs.powerSave;
    Permit();
    if (!prefs_save(&g->prefs, PREFS_ENV)) {
        set_status(g, S(ID_CAN_T_WRITE_ENV_PRISM_PREFS, "Can't write ENV:Prism.prefs"));
        return FALSE;
    }
    if (envarc && !prefs_save(&g->prefs, PREFS_ENVARC)) {
        set_status(g, S(ID_CAN_T_WRITE_ENVARC_PRISM_PREFS, "Can't write ENVARC:Prism.prefs"));
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
    LONG (*beginOn)(UBYTE, ULONG, ULONG, ULONG, ULONG, const char *) = NULL;
    void (*end)(void) = NULL;
    UWORD w, h;
    UBYTE bits = prefs_depths[PREFS_DEPTH(i)], hz = g->prefs.hz[i];
    char label[96], rate[16];
    LONG rc, t;
    BOOL stop = FALSE;

    if (!prefs_size(&g->prefs, i, &w, &h)) {
        set_status(g, S(ID_GIVE_THIS_CUSTOM_SLOT_A_SIZE_FIRST_WIDTH, "Give this custom slot a size first (Width, Height, Set size)"));
        return -1;
    }

    Forbid();
    if ((s = (struct PrismSem *)FindSemaphore(PRISM_SEMNAME)) && s->version >= 2) {
        begin = s->testBegin;
        end = s->testEnd;
        if (s->version >= 6 && g->testOn && s->numBoards > 1)
            beginOn = s->testBeginOn;
    }
    Permit();
    if (!begin || !end) {
        set_status(g, S(ID_TEST_NEEDS_PRISM_RUNNING_RESTART_WITH_PR, "Test needs Prism running (restart with Prism installed)."));
        return -1;
    }
    if (hz) sprintf(rate, "%u Hz", hz); else strcpy(rate, "default rate");
    sprintf(label, "PRISM test  %ux%u %u-bit %s  -  back in %d s (any key or click)", w, h,
            bits, rate, TEST_SECS);
    rc = beginOn ? beginOn(g->testOn, w, h, bits, hz, label) : begin(w, h, bits, hz, label);
    if (rc == PRISM_TEST_NOMODE) {
        set_status(g, S(ID_THE_CARD_CAN_T_SHOW_THAT_MODE_AT_THAT_RA, "The card can't show that mode at that rate."));
        return rc;
    }
    if (rc != PRISM_TEST_OK) {
        set_status(g, rc == PRISM_TEST_BUSY ? S(ID_A_TEST_IS_ALREADY_SHOWING, "A test is already showing.")
                                            : S(ID_NOT_ENOUGH_VIDEO_MEMORY_FREE_FOR_THE_TES, "Not enough video memory free for the test."));
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
        struct EasyStruct es = { sizeof(struct EasyStruct), 0, (STRPTR)S(ID_PRISM_MODE_TEST, "Prism mode test"),
                                 (STRPTR)S(ID_DID_THE_TEST_PICTURE_LOOK_RIGHT, "Did the test picture for\n%s\nlook right on your monitor?"),
                                 (STRPTR)S(ID_YES_NO_SWITCH_IT_OFF, "Yes|No, switch it off") };
        char what[40];
        sprintf(what, "%ux%u %u-bit %s", w, h, bits, rate);
        if (EasyRequest(g->win, &es, NULL, (ULONG)what)) {
            set_status(g, S(ID_MODE_TESTED, "Mode tested."));
        } else {
            g->prefs.on[i] = 0;
            refresh_list(g);
            show_settings(g);
            set_status(g, S(ID_MODE_SWITCHED_OFF_SAVE_OR_USE_TO_KEEP_TH, "Mode switched off. Save or Use to keep that."));
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
    WORD listW = 38 * fw + 24, listH;
    WORD rx, rw, y, bw, by, py, plH;

    if (!(gad = CreateContext(&g->glist)))
        return FALSE;
    memset(&ng, 0, sizeof(ng));
    ng.ng_TextAttr = g->scr->Font;
    ng.ng_VisualInfo = g->vi;

    /* right column: label width + gadgets */
    rx = cl + listW + 16 + 8 * fw;
    rw = 15 * fw + 30;                  /* "Promote screens" beside its box */

    y = ct + fh + 2;
    ng.ng_LeftEdge = rx; ng.ng_TopEdge = y;
    ng.ng_Width = 26; ng.ng_Height = fh + 3;
    ng.ng_GadgetText = (STRPTR)S(ID_OFFERED, "Offered"); ng.ng_Flags = PLACETEXT_RIGHT; ng.ng_GadgetID = GAD_ON;
    gad = g->gOn = CreateGadget(CHECKBOX_KIND, gad, &ng, GTCB_Scaled, TRUE,
                                GTCB_Checked, (ULONG)g->prefs.on[g->sel], TAG_END);

    y += row;
    ng.ng_TopEdge = y; ng.ng_Width = rw; ng.ng_Height = gh;
    ng.ng_GadgetText = (STRPTR)S(ID_REFRESH, "Refresh"); ng.ng_Flags = PLACETEXT_LEFT; ng.ng_GadgetID = GAD_HZ;
    gad = g->gHz = CreateGadget(CYCLE_KIND, gad, &ng, GTCY_Labels, (ULONG)hzLabels,
                                GTCY_Active, (ULONG)rate_index(g->prefs.hz[g->sel]), TAG_END);

    y += row + fh + 6;
    ng.ng_TopEdge = y;
    ng.ng_GadgetText = (STRPTR)S(ID_BOARD, "Board"); ng.ng_GadgetID = GAD_BOARD;
    gad = g->gBoard = CreateGadget(CYCLE_KIND, gad, &ng, GTCY_Labels, (ULONG)boardLabels,
                                   GTCY_Active, (ULONG)board_entry(&g->prefs), TAG_END);

    y += row;
    ng.ng_TopEdge = y; ng.ng_Width = 26; ng.ng_Height = fh + 3;
    ng.ng_GadgetText = (STRPTR)S(ID_USE_BLITTER, "Use blitter"); ng.ng_Flags = PLACETEXT_RIGHT; ng.ng_GadgetID = GAD_BLIT;
    gad = g->gBlit = CreateGadget(CHECKBOX_KIND, gad, &ng, GTCB_Scaled, TRUE,
                                  GTCB_Checked, (ULONG)g->prefs.blitter, TAG_END);

    y += row;
    ng.ng_TopEdge = y;
    ng.ng_GadgetText = (STRPTR)S(ID_SWAP_RED_BLUE, "Swap red/blue"); ng.ng_GadgetID = GAD_BGR;
    gad = g->gBgr = CreateGadget(CHECKBOX_KIND, gad, &ng, GTCB_Scaled, TRUE,
                                 GTCB_Checked, (ULONG)g->prefs.clutBGR, TAG_END);

    /* the two experimental switches (DRAGGING=, SOFTWAREPOINTER=) */
    y += row;
    ng.ng_TopEdge = y;
    ng.ng_GadgetText = (STRPTR)S(ID_DRAG_SCREENS, "Drag screens"); ng.ng_GadgetID = GAD_DRAG;
    gad = g->gDrag = CreateGadget(CHECKBOX_KIND, gad, &ng, GTCB_Scaled, TRUE,
                                  GTCB_Checked, (ULONG)g->prefs.dragging, TAG_END);

    y += row;
    ng.ng_TopEdge = y;
    ng.ng_GadgetText = (STRPTR)S(ID_SOFT_POINTER, "Soft pointer"); ng.ng_GadgetID = GAD_SOFTPTR;
    gad = g->gSoft = CreateGadget(CHECKBOX_KIND, gad, &ng, GTCB_Scaled, TRUE,
                                  GTCB_Checked, (ULONG)g->prefs.softwarePointer, TAG_END);

    /* video layers on the board's own overlay (HWOVERLAY=; the ZZ9000) */
    y += row;
    ng.ng_TopEdge = y;
    ng.ng_GadgetText = (STRPTR)S(ID_VIDEO_ON_BOARD, "Video on board"); ng.ng_GadgetID = GAD_HWOVL;
    gad = g->gHwOvl = CreateGadget(CHECKBOX_KIND, gad, &ng, GTCB_Scaled, TRUE,
                                   GTCB_Checked, (ULONG)g->prefs.hwOverlay, TAG_END);

    /* screen mode promotion (PROMOTE=), its exceptions are the list below */
    y += row;
    ng.ng_TopEdge = y;
    ng.ng_GadgetText = (STRPTR)S(ID_PROMOTE_SCREENS, "Promote screens"); ng.ng_GadgetID = GAD_PROMOTE;
    gad = g->gPromote = CreateGadget(CHECKBOX_KIND, gad, &ng, GTCB_Scaled, TRUE,
                                     GTCB_Checked, (ULONG)g->prefs.promote, TAG_END);

    /* MIRROR=: chipset screens copied to the board's monitor */
    y += row;
    ng.ng_TopEdge = y;
    ng.ng_GadgetText = (STRPTR)S(ID_MIRROR_CHIPSET, "Mirror chipset"); ng.ng_GadgetID = GAD_MIRROR;
    gad = g->gMirror = CreateGadget(CHECKBOX_KIND, gad, &ng, GTCB_Scaled, TRUE,
                                    GTCB_Checked, (ULONG)g->prefs.mirrorChip, TAG_END);

    y += row;
    ng.ng_TopEdge = y; ng.ng_Width = rw; ng.ng_Height = gh;
    ng.ng_GadgetText = (STRPTR)S(ID_LOG, "Log"); ng.ng_Flags = PLACETEXT_LEFT; ng.ng_GadgetID = GAD_LOG;
    gad = g->gLog = CreateGadget(CYCLE_KIND, gad, &ng, GTCY_Labels, (ULONG)logLabels,
                                 GTCY_Active, (ULONG)(g->prefs.log <= 2 ? g->prefs.log : 1), TAG_END);

    /* update checks (UPDATES=, PrismUpdate): where to look, and a check now */
    y += row;
    ng.ng_TopEdge = y;
    ng.ng_GadgetText = (STRPTR)S(ID_UPDATES, "Updates"); ng.ng_GadgetID = GAD_UPDATES;
    gad = g->gUpdates = CreateGadget(CYCLE_KIND, gad, &ng, GTCY_Labels, (ULONG)updLabels,
                                     GTCY_Active, (ULONG)(g->prefs.updates < UPD_COUNT ? g->prefs.updates : 0), TAG_END);
    y += row;
    ng.ng_TopEdge = y;
    ng.ng_GadgetText = (STRPTR)S(ID_CHECK_NOW, "Check now"); ng.ng_Flags = PLACETEXT_IN; ng.ng_GadgetID = GAD_CHECK;
    gad = CreateGadget(BUTTON_KIND, gad, &ng, TAG_END);

    /* GAMMA=: the palette's curve (256-colour screens) */
    y += row;
    ng.ng_TopEdge = y;
    ng.ng_GadgetText = (STRPTR)S(ID_GAMMA, "Gamma"); ng.ng_Flags = PLACETEXT_LEFT; ng.ng_GadgetID = GAD_GAMMA;
    gad = g->gGamma = CreateGadget(CYCLE_KIND, gad, &ng, GTCY_Labels, (ULONG)gammaLabels,
                                   GTCY_Active, (ULONG)gamma_index(g->prefs.gamma), TAG_END);

    /* POWERSAVE=: minutes without input before the monitors go dark */
    y += row;
    ng.ng_TopEdge = y;
    ng.ng_GadgetText = (STRPTR)S(ID_SLEEP, "Sleep"); ng.ng_Flags = PLACETEXT_LEFT; ng.ng_GadgetID = GAD_POWER;
    gad = g->gPower = CreateGadget(INTEGER_KIND, gad, &ng, GTIN_Number, (ULONG)g->prefs.powerSave,
                                   GTIN_MaxChars, 3, TAG_END);

    /* the mode list at least 12 lines, and as tall as the column beside it */
    listH = y + gh - (ct + fh + 2);
    if (listH < 12 * fh + 4)
        listH = 12 * fh + 4;

    /* the programs list under the modes, Promote / Keep / Remove beside it
     * and the name being added below */
    py = ct + fh + 2 + listH + 6 + fh + 2;
    plH = 3 * row + 2;
    ng.ng_LeftEdge = rx; ng.ng_Width = rw; ng.ng_Height = gh; ng.ng_Flags = PLACETEXT_IN;
    ng.ng_TopEdge = py;           ng.ng_GadgetText = (STRPTR)S(ID_PROMOTE, "Promote"); ng.ng_GadgetID = GAD_PADD;
    gad = CreateGadget(BUTTON_KIND, gad, &ng, TAG_END);
    ng.ng_TopEdge = py + row;     ng.ng_GadgetText = (STRPTR)S(ID_KEEP, "Keep");    ng.ng_GadgetID = GAD_PKEEP;
    gad = CreateGadget(BUTTON_KIND, gad, &ng, TAG_END);
    ng.ng_TopEdge = py + 2 * row; ng.ng_GadgetText = (STRPTR)S(ID_REMOVE, "Remove");  ng.ng_GadgetID = GAD_PDEL;
    gad = CreateGadget(BUTTON_KIND, gad, &ng, TAG_END);
    ng.ng_LeftEdge = cl + 8 * fw; ng.ng_TopEdge = py + plH + 4;
    ng.ng_Width = rx + rw - cl - 8 * fw; ng.ng_Height = gh;
    ng.ng_GadgetText = (STRPTR)S(ID_PROGRAM, "Program"); ng.ng_Flags = PLACETEXT_LEFT; ng.ng_GadgetID = GAD_PROG;
    gad = g->gProg = CreateGadget(STRING_KIND, gad, &ng, GTST_MaxChars, PREFS_PROGNAME, TAG_END);

    /* a custom size for the selected custom slot: Width, Height, Set size */
    y = py + plH + 4 + row;
    ng.ng_LeftEdge = cl + 6 * fw; ng.ng_TopEdge = y; ng.ng_Width = 7 * fw + 8; ng.ng_Height = gh;
    ng.ng_GadgetText = (STRPTR)S(ID_WIDTH, "Width"); ng.ng_Flags = PLACETEXT_LEFT; ng.ng_GadgetID = GAD_W;
    gad = g->gW = CreateGadget(INTEGER_KIND, gad, &ng, GTIN_Number, 0, GTIN_MaxChars, 4,
                               GA_Disabled, TRUE, TAG_END);
    ng.ng_LeftEdge = cl + 21 * fw + 8;
    ng.ng_GadgetText = (STRPTR)S(ID_HEIGHT, "Height"); ng.ng_GadgetID = GAD_H;
    gad = g->gH = CreateGadget(INTEGER_KIND, gad, &ng, GTIN_Number, 0, GTIN_MaxChars, 4,
                               GA_Disabled, TRUE, TAG_END);
    ng.ng_LeftEdge = cl + 30 * fw + 24; ng.ng_Width = 10 * fw + 8;
    ng.ng_GadgetText = (STRPTR)S(ID_SET_SIZE, "Set size"); ng.ng_Flags = PLACETEXT_IN; ng.ng_GadgetID = GAD_SETSIZE;
    gad = g->gSet = CreateGadget(BUTTON_KIND, gad, &ng, GA_Disabled, TRUE, TAG_END);

    /* status line, then the Save / Use / Defaults / Cancel row */
    by = y + row + 2;
    ng.ng_LeftEdge = cl; ng.ng_TopEdge = by;
    ng.ng_Width = rx + rw - cl; ng.ng_Height = gh;
    ng.ng_GadgetText = NULL; ng.ng_Flags = 0; ng.ng_GadgetID = GAD_STATUS;
    gad = g->gStatus = CreateGadget(TEXT_KIND, gad, &ng, GTTX_Text, (ULONG)g->status,
                                    GTTX_Border, TRUE, TAG_END);

    by += row + 2;
    bw = (rx + rw - cl - 4 * 8) / 5;
    ng.ng_TopEdge = by; ng.ng_Width = bw; ng.ng_Height = gh; ng.ng_Flags = PLACETEXT_IN;
    ng.ng_LeftEdge = cl;                     ng.ng_GadgetText = (STRPTR)S(ID_SAVE, "Save");     ng.ng_GadgetID = GAD_SAVE;
    gad = CreateGadget(BUTTON_KIND, gad, &ng, TAG_END);
    ng.ng_LeftEdge = cl + (bw + 8);          ng.ng_GadgetText = (STRPTR)S(ID_USE, "Use");      ng.ng_GadgetID = GAD_USE;
    gad = CreateGadget(BUTTON_KIND, gad, &ng, TAG_END);
    ng.ng_LeftEdge = cl + 2 * (bw + 8);      ng.ng_GadgetText = (STRPTR)S(ID_TEST, "Test");     ng.ng_GadgetID = GAD_TEST;
    gad = CreateGadget(BUTTON_KIND, gad, &ng, TAG_END);
    ng.ng_LeftEdge = cl + 3 * (bw + 8);      ng.ng_GadgetText = (STRPTR)S(ID_DEFAULTS, "Defaults"); ng.ng_GadgetID = GAD_DEFAULTS;
    gad = CreateGadget(BUTTON_KIND, gad, &ng, TAG_END);
    ng.ng_LeftEdge = rx + rw - bw;           ng.ng_GadgetText = (STRPTR)S(ID_CANCEL, "Cancel");   ng.ng_GadgetID = GAD_CANCEL;
    gad = CreateGadget(BUTTON_KIND, gad, &ng, TAG_END);

    /* listviews last: their scroller sub-gadgets end the chain */
    ng.ng_LeftEdge = cl; ng.ng_TopEdge = py;
    ng.ng_Width = listW; ng.ng_Height = plH;
    ng.ng_GadgetText = (STRPTR)S(ID_PROGRAMS, "Programs"); ng.ng_Flags = PLACETEXT_ABOVE | NG_HIGHLABEL; ng.ng_GadgetID = GAD_PLIST;
    gad = g->gPList = CreateGadget(LISTVIEW_KIND, gad, &ng, GTLV_Labels, (ULONG)&g->progs,
                                   GTLV_ShowSelected, 0, TAG_END);
    ng.ng_LeftEdge = cl; ng.ng_TopEdge = ct + fh + 2;
    ng.ng_Width = listW; ng.ng_Height = listH;
    ng.ng_GadgetText = (STRPTR)S(ID_MODES, "Modes"); ng.ng_Flags = PLACETEXT_ABOVE | NG_HIGHLABEL; ng.ng_GadgetID = GAD_LIST;
    gad = g->gList = CreateGadget(LISTVIEW_KIND, gad, &ng, GTLV_Labels, (ULONG)&g->modes,
                                  GTLV_ShowSelected, 0, GTLV_Selected, (ULONG)g->sel,
                                  GTLV_MakeVisible, (ULONG)g->sel, TAG_END);
    if (!gad)
        return FALSE;

    *innerW = rx + rw + 8 - g->scr->WBorLeft;
    *innerH = by + gh + 6 - (g->scr->WBorTop + fh + 1);
    return TRUE;
}

/* The window needs about 460 lines with topaz 8. A Workbench shorter than
 * that (640x256 PAL, 640x200 NTSC: any machine without an RTG Workbench,
 * so most machines PrismRTG isn't installed on yet) left half the gadgets,
 * Save and Cancel among them, outside the window - and a crash on some
 * systems. Then the editor opens its own screen like the Workbench,
 * interlaced where the mode has an interlaced form, as tall as it needs
 * (scrolling if the display is shorter still). */
static struct Screen *tall_screen(struct Screen *wb, WORD h)
{
    ULONG id = GetVPModeID(&wb->ViewPort);
    if (id != INVALID_ID && !(id & LACE) && !ModeNotAvailable(id | LACE))
        id |= LACE;
    return OpenScreenTags(NULL,
        SA_LikeWorkbench, TRUE,
        id != INVALID_ID ? SA_DisplayID : TAG_IGNORE, id,
        SA_Height, (ULONG)h,
        SA_AutoScroll, TRUE,
        SA_Title, (ULONG)S(ID_PRISMRTG_PREFERENCES, "PrismRTG Preferences"),
        TAG_END);
}

static BOOL open_window(struct Gui *g)
{
    WORD iw, ih, need;
    struct Screen *own;

    if (!(g->scr = LockPubScreen(NULL)) || !(g->vi = GetVisualInfo(g->scr, TAG_END)))
        return FALSE;
    if (!make_gadgets(g, &iw, &ih))
        return FALSE;
    /* the window's outer height, from where it opens */
    need = g->scr->BarHeight + 4 + g->scr->WBorTop + g->scr->RastPort.Font->tf_YSize + 1 + ih +
           g->scr->WBorBottom;
    if (need > g->scr->Height && (own = tall_screen(g->scr, need + 2))) {
        FreeGadgets(g->glist);
        g->glist = NULL;
        FreeVisualInfo(g->vi);
        UnlockPubScreen(NULL, g->scr);
        g->scr = own;
        g->ownScr = TRUE;
        if (!(g->vi = GetVisualInfo(g->scr, TAG_END)) || !make_gadgets(g, &iw, &ih))
            return FALSE;
    }
    g->win = OpenWindowTags(NULL,
        WA_Title, (ULONG)S(ID_PRISMRTG_PREFERENCES, "PrismRTG Preferences"),
        WA_Left, 20, WA_Top, g->scr->BarHeight + 4,
        WA_InnerWidth, iw, WA_InnerHeight, ih,
        WA_AutoAdjust, TRUE,
        WA_Flags, WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_CLOSEGADGET | WFLG_ACTIVATE |
                  WFLG_SMART_REFRESH,
        WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW | IDCMP_VANILLAKEY |
                  IDCMP_MOUSEBUTTONS |
                  BUTTONIDCMP | CHECKBOXIDCMP | CYCLEIDCMP | LISTVIEWIDCMP | STRINGIDCMP,
        WA_Gadgets, (ULONG)g->glist,
        g->ownScr ? WA_CustomScreen : WA_PubScreen, (ULONG)g->scr,
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
                    board_from_entry(&g->prefs, code);
                    if (code < BOARD_AUTOS)
                        board_status(g, code);
                    break;
                case GAD_BLIT:
                    g->prefs.blitter = (gad->Flags & GFLG_SELECTED) ? 1 : 0;
                    break;
                case GAD_LOG:
                    g->prefs.log = code <= 2 ? code : 1;
                    set_status(g, code == 2 ? S(ID_SYNC_LOG_WRITTEN_TO_SYS_PRISMD_LOG_AT_ON, "Sync: log written to SYS:PrismD.log at once (slow)")
                                  : code ? S(ID_ON_PRISMD_WRITES_ITS_LOG_TO_T_PRISMD_LOG, "On: PrismD writes its log to T:PrismD.log")
                                         : S(ID_LOG_OFF, "Log off"));
                    break;
                case GAD_BGR:
                    g->prefs.clutBGR = (gad->Flags & GFLG_SELECTED) ? 1 : 0;
                    set_status(g, g->prefs.clutBGR
                                  ? S(ID_FOR_A_BOARD_WHOSE_256_COLOUR_SCREENS_SHO, "For a board whose 256-colour screens show red as blue")
                                  : S(ID_256_COLOUR_PALETTES_LOADED_RED_FIRST_NOR, "256-colour palettes loaded red first (normal)"));
                    break;
                case GAD_DRAG:
                    g->prefs.dragging = (gad->Flags & GFLG_SELECTED) ? 1 : 0;
                    set_status(g, g->prefs.dragging
                                  ? S(ID_SCREEN_DRAGGING_ON_EXPERIMENTAL_SLOWER_D, "Screen dragging on (experimental, slower drawing)")
                                  : S(ID_SCREEN_DRAGGING_OFF, "Screen dragging off"));
                    break;
                case GAD_SOFTPTR:
                    g->prefs.softwarePointer = (gad->Flags & GFLG_SELECTED) ? 1 : 0;
                    set_status(g, g->prefs.softwarePointer
                                  ? S(ID_POINTER_DRAWN_BY_PRISMRTG_FOR_BOARDS_WIT, "Pointer drawn by PrismRTG (for boards without one)")
                                  : S(ID_BOARD_S_OWN_POINTER_WHERE_IT_HAS_ONE, "Board's own pointer where it has one"));
                    break;
                case GAD_HWOVL:
                    g->prefs.hwOverlay = (gad->Flags & GFLG_SELECTED) ? 1 : 0;
                    set_status(g, g->prefs.hwOverlay
                                  ? S(ID_VIDEO_LAYERS_SHOWN_BY_THE_BOARD_WHERE_IT, "Video layers shown by the board where it can (ZZ9000)")
                                  : S(ID_VIDEO_LAYERS_COMPOSED_BY_PRISMRTG_ON_THE, "Video layers composed by PrismRTG on the CPU"));
                    break;
                case GAD_UPDATES:
                    g->prefs.updates = code < UPD_COUNT ? code : UPD_OFF;
                    set_status(g, code == UPD_OFF ? S(ID_NO_UPDATE_CHECKS, "No update checks")
                                  : code == UPD_GITHUB ? S(ID_CHECK_NOW_ASKS_GITHUB_S_RELEASES_NEEDS_A, "Check now asks GitHub's releases (needs AmiSSL 5)")
                                  : code == UPD_MIRROR ? S(ID_CHECK_NOW_ASKS_THE_MIRROR_MIRROR_IN_THE, "Check now asks the mirror (MIRROR= in the prefs file)")
                                  : S(ID_CHECK_NOW_ASKS_GITHUB_THEN_THE_MIRROR, "Check now asks GitHub, then the mirror"));
                    break;
                case GAD_CHECK:
                    check_update(g);
                    break;
                case GAD_GAMMA:
                    g->prefs.gamma = code < sizeof(gammaValues) ? gammaValues[code] : 10;
                    set_status(g, g->prefs.gamma == 10 ? S(ID_PALETTES_LOADED_AS_THEY_ARE, "Palettes loaded as they are")
                                  : S(ID_256_COLOUR_SCREENS_PALETTES_THROUGH_THIS, "256-colour screens' palettes through this curve (restart PrismD)"));
                    break;
                case GAD_POWER: {
                    LONG m = ((struct StringInfo *)gad->SpecialInfo)->LongInt;
                    char msg[80];
                    if (m < 0) m = 0;
                    if (m > 240) m = 240;
                    g->prefs.powerSave = m;
                    GT_SetGadgetAttrs(gad, g->win, NULL, GTIN_Number, (ULONG)m, TAG_END);
                    if (m)
                        snprintf(msg, sizeof(msg), S(ID_MONITORS_GO_DARK_AFTER_MINUTES, "Monitors go dark after %ld minutes without input"), (long)m);
                    else
                        snprintf(msg, sizeof(msg), "%s", S(ID_MONITORS_NEVER_GO_DARK, "Monitors never go dark (Sleep 0)"));
                    set_status(g, msg);
                    break;
                }
                case GAD_SETSIZE:
                case GAD_W:                      /* Return in a field: as Set size */
                case GAD_H:
                    set_size(g);
                    break;
                case GAD_MIRROR:
                    g->prefs.mirrorChip = (gad->Flags & GFLG_SELECTED) ? 1 : 0;
                    set_status(g, g->prefs.mirrorChip
                                  ? S(ID_CHIPSET_SCREENS_SHOWN_ON_THE_BOARD_AS_A, "Chipset screens shown on the board as a copy (restart PrismD)")
                                  : S(ID_CHIPSET_SCREENS_ON_THE_AMIGA_S_OWN_VIDEO, "Chipset screens on the Amiga's own video"));
                    break;
                case GAD_PROMOTE:
                    g->prefs.promote = (gad->Flags & GFLG_SELECTED) ? 1 : 0;
                    set_status(g, g->prefs.promote
                                  ? S(ID_SCREENS_ASKED_FOR_ON_THE_CHIPSET_OPEN_ON, "Screens asked for on the chipset open on the board (kept ones stay)")
                                  : S(ID_SCREENS_STAY_ON_THE_CHIPSET_PROMOTED_PRO, "Screens stay on the chipset (promoted programs still move)"));
                    break;
                case GAD_PLIST:
                    g->psel = code;
                    if (code < g->prefs.nprog)
                        GT_SetGadgetAttrs(g->gProg, g->win, NULL, GTST_String,
                                          (ULONG)g->prefs.prog[code].name, TAG_END);
                    break;
                case GAD_PADD:
                case GAD_PKEEP: {
                    const char *nm = (const char *)((struct StringInfo *)g->gProg->SpecialInfo)->Buffer;
                    if (prefs_prog_set(&g->prefs, nm, gad->GadgetID == GAD_PKEEP)) {
                        g->psel = prefs_prog_find(&g->prefs, nm);
                        refresh_progs(g);
                        set_status(g, gad->GadgetID == GAD_PKEEP
                                      ? S(ID_KEPT_ON_THE_CHIPSET_WHATEVER_THE_SWITCH, "Kept on the chipset whatever the switch says")
                                      : S(ID_PROMOTED_TO_THE_BOARD_WHATEVER_THE_SWITC, "Promoted to the board whatever the switch says"));
                    } else
                        set_status(g, S(ID_GIVE_A_PROGRAM_NAME_31_CHARACTERS_AT_MOS, "Give a program name (31 characters at most); the list holds 16"));
                    break;
                }
                case GAD_PDEL:
                    if (g->psel >= 0 && g->psel < g->prefs.nprog) {
                        prefs_prog_remove(&g->prefs, g->psel);
                        refresh_progs(g);
                    }
                    break;
                case GAD_TEST:
                    /* with two boards: which one shows it */
                    if (g->boards > 1) {
                        struct EasyStruct es = { sizeof(struct EasyStruct), 0, (STRPTR)S(ID_PRISM_MODE_TEST, "Prism mode test"),
                                                 (STRPTR)S(ID_SHOW_THE_TEST_PICTURE_ON_WHICH_BOARD, "Show the test picture on which board?"),
                                                 (STRPTR)S(ID_FIRST_SECOND_CANCEL, "First|Second|Cancel") };
                        LONG b = EasyRequest(g->win, &es, NULL);   /* 1 First, 2 Second, 0 Cancel */
                        if (!b)
                            break;
                        g->testOn = b == 2;
                    }
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
                    g->psel = -1;
                    refresh_progs(g);
                    show_settings(g);
                    set_status(g, S(ID_DEFAULTS_EVERY_MODE_THE_BOARD_CAN_SHOW, "Defaults - every mode the board can show"));
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

#define TEMPLATE "FROM/K,USE/S,SAVE/S,TEST/K/N,TESTON/K/N"

int main(void)
{
    LONG args[5] = { 0 };
    struct RDArgs *rda;
    int rc = 20;

    if (!(rda = ReadArgs(TEMPLATE, args, NULL))) {
        PrintFault(IoErr(), "PrismPrefs");
        return 20;
    }
    cat_open();
    init_texts();
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
        g.testOn = args[4] && *(LONG *)args[4] == 2;   /* TESTON=2: the second board */
        g.boards = 2;                    /* (test_mode asks PrismD whether there is one) */
        rc = test_mode(&g, slot, FALSE);
        printf("PrismPrefs: test of slot %ld: %s (texts: %s)\n", (long)slot,
               rc == 0 ? S(ID_SHOWN, "shown") : g.status,
               cat_language() ? cat_language() : "built in");
        rc = rc ? 5 : 0;
        goto out;
    }

    running_status(&g);
    build_list(&g);
    build_progs(&g);
    g.psel = -1;
    if (open_window(&g)) {
        event_loop(&g);
        rc = 0;
    }

out:
    if (g.win) CloseWindow(g.win);
    if (g.glist) FreeGadgets(g.glist);
    if (g.vi) FreeVisualInfo(g.vi);
    if (g.scr && g.ownScr) CloseScreen(g.scr);
    else if (g.scr) UnlockPubScreen(NULL, g.scr);
    if (GadToolsBase) CloseLibrary(GadToolsBase);
    if (GfxBase) CloseLibrary((struct Library *)GfxBase);
    if (IntuitionBase) CloseLibrary((struct Library *)IntuitionBase);
    cat_close();
    FreeArgs(rda);
    return rc;
}
