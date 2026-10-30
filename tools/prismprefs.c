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
#include <dos/dosextens.h>
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
#include "tabs.h"

struct IntuitionBase *IntuitionBase;
struct GfxBase *GfxBase;
struct Library *GadToolsBase;

static const char version[] __attribute__((used)) = "$VER: PrismPrefs " PRISM_RELEASE " (" PRISM_RELDATE ")";

enum { GAD_LIST = 1, GAD_ON, GAD_HZ, GAD_BOARD, GAD_BLIT, GAD_LOG,
       GAD_SAVE, GAD_USE, GAD_DEFAULTS, GAD_CANCEL, GAD_STATUS, GAD_TEST, GAD_BGR,
       GAD_DRAG, GAD_SOFTPTR, GAD_HWOVL, GAD_PROMOTE, GAD_PLIST, GAD_PROG, GAD_PADD, GAD_PKEEP, GAD_PDEL,
       GAD_UPDATES, GAD_CHECK, GAD_W, GAD_H, GAD_SETSIZE, GAD_GAMMA, GAD_POWER, GAD_MIRROR, GAD_SECMON };

/* The window is tabbed (as PrismNet Prefs) so that it fits a 640x200 screen
 * with topaz 8: each tab's page has only its own gadgets, the status line
 * and the Save / Use / Test / Defaults / Cancel row are always there */
enum { PG_MODES, PG_SCREENS, PG_BOARD, PG_OTHER, PG_COUNT };
static const char *pageLabels[PG_COUNT] = { "Modes", "Screens", "Board", "Other" };
#define GAD_TAB0 100                     /* the tabs' GadgetIDs: GAD_TAB0 + page */
/* the 2nd monitor cycle (SECONDMONITOR=): what a second board shows while
 * no screen is opened on it; its entries in the order of secValues. Span
 * twice, for the side the second monitor stands on (SECONDSIDE=) */
static STRPTR secLabels[] = { "Own screen", "Mirror", "Span right", "Span left", "Off", NULL };
static const UBYTE secValues[] = { SM_SCREEN, SM_MIRROR, SM_SPAN, SM_SPAN, SM_OFF };
#define SEC_SPAN_LEFT 3

static UWORD sec_index(const struct PrismPrefs *p)
{
    UWORD i;
    if (p->secondMon == SM_SPAN)
        return p->secondLeft ? SEC_SPAN_LEFT : SEC_SPAN_LEFT - 1;
    for (i = 0; i < sizeof(secValues); i++)
        if (secValues[i] == p->secondMon)
            return i;
    return 0;
}

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

/* the menus: Project and Edit as in the system prefs editors (Edit's three
 * like theirs: the defaults, ENVARC:, or what the window opened with). The
 * texts are put in by init_texts; the user data is the action */
enum { MN_ABOUT = 1, MN_TEST, MN_CHECK, MN_SAVE, MN_USE, MN_QUIT, MN_DEFAULTS, MN_LASTSAVED, MN_RESTORE };

static struct NewMenu menuDef[] = {
    { NM_TITLE, (STRPTR)"Project", NULL, 0, 0, NULL },
    { NM_ITEM,  (STRPTR)"About...", (STRPTR)"?", 0, 0, (APTR)MN_ABOUT },
    { NM_ITEM,  NM_BARLABEL, NULL, 0, 0, NULL },
    { NM_ITEM,  (STRPTR)"Test mode", (STRPTR)"T", 0, 0, (APTR)MN_TEST },
    { NM_ITEM,  (STRPTR)"Check for updates", NULL, 0, 0, (APTR)MN_CHECK },
    { NM_ITEM,  NM_BARLABEL, NULL, 0, 0, NULL },
    { NM_ITEM,  (STRPTR)"Save", (STRPTR)"S", 0, 0, (APTR)MN_SAVE },
    { NM_ITEM,  (STRPTR)"Use", (STRPTR)"U", 0, 0, (APTR)MN_USE },
    { NM_ITEM,  NM_BARLABEL, NULL, 0, 0, NULL },
    { NM_ITEM,  (STRPTR)"Quit", (STRPTR)"Q", 0, 0, (APTR)MN_QUIT },
    { NM_TITLE, (STRPTR)"Edit", NULL, 0, 0, NULL },
    { NM_ITEM,  (STRPTR)"Reset to defaults", (STRPTR)"D", 0, 0, (APTR)MN_DEFAULTS },
    { NM_ITEM,  (STRPTR)"Last saved", (STRPTR)"L", 0, 0, (APTR)MN_LASTSAVED },
    { NM_ITEM,  (STRPTR)"Restore", (STRPTR)"R", 0, 0, (APTR)MN_RESTORE },
    { NM_END,   NULL, NULL, 0, 0, NULL }
};

static UWORD gamma_index(UBYTE g)
{
    UWORD i;
    for (i = 0; i < sizeof(gammaValues); i++)
        if (gammaValues[i] == g)
            return i;
    return 0;
}

/* the window's measures, from the gadget font (make_layout) */
struct Layout {
    WORD fw, fh, gh, row, cbw, cbh;  /* font, gadget height, row pitch, checkbox */
    WORD w;                          /* inner width                            */
    WORD pageTop, pageH;             /* the page under the tabs (from inner top) */
    WORD minPage, maxPage;
    WORD listW;                      /* the Modes and the Programs lists       */
    WORD mLab, mCol;                 /* Modes: right column labels, gadgets    */
    WORD sBtn, sLab, sGap;           /* Screens: buttons, "Program", 2nd box x */
    WORD bLab, bCol;                 /* Board                                  */
    WORD oLab, oCol, oBtn;           /* Other                                  */
};

struct Gui {
    struct Screen *scr;
    APTR vi;
    struct Window *win;
    struct Tabs tabs;
    int page;
    struct Layout lay;
    struct TextAttr ta;              /* the gadgets' font: the screen's or topaz 8 */
    struct TextFont *font;
    struct RastPort mrp;             /* measures texts in it                   */
    struct Gadget *bglist;           /* status line + buttons: always there    */
    struct Gadget *gStatus;
    /* the page's gadgets (glist); those of the other pages are NULL */
    struct Gadget *glist, *gList, *gOn, *gHz, *gBoard, *gBlit, *gLog, *gBgr, *gDrag, *gSoft, *gHwOvl;
    struct Gadget *gPromote, *gPList, *gProg, *gUpdates, *gW, *gH, *gSet, *gGamma, *gPower, *gMirror;
    struct Gadget *gSecMon;
    char progName[PREFS_PROGNAME + 1]; /* the Program field, kept across tabs */
    struct Menu *menu;
    struct List modes;
    struct Node node[PREFS_NMODES];
    char text[PREFS_NMODES][48];
    /* the promotion list: programs promoted or kept whatever the switch says */
    struct List progs;
    struct Node pnode[PREFS_NPROGS];
    char ptext[PREFS_NPROGS][48];
    LONG psel;                       /* selected program, -1 none              */
    struct PrismPrefs prefs;
    struct PrismPrefs opened;        /* as the window opened with them: Restore */
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
    secLabels[0] = (STRPTR)S(ID_OWN_SCREEN, "Own screen");
    secLabels[1] = (STRPTR)S(ID_MIRROR_FIRST, "Mirror");
    secLabels[2] = (STRPTR)S(ID_SPAN_RIGHT, "Span right");
    secLabels[3] = (STRPTR)S(ID_SPAN_LEFT, "Span left");
    secLabels[4] = (STRPTR)S(ID_OFF, "Off");
    menuDef[0].nm_Label = (STRPTR)S(ID_MENU_PROJECT, "Project");
    menuDef[1].nm_Label = (STRPTR)S(ID_MENU_ABOUT, "About...");
    menuDef[3].nm_Label = (STRPTR)S(ID_MENU_TEST_MODE, "Test mode");
    menuDef[4].nm_Label = (STRPTR)S(ID_MENU_CHECK_FOR_UPDATES, "Check for updates");
    menuDef[6].nm_Label = (STRPTR)S(ID_SAVE, "Save");
    menuDef[7].nm_Label = (STRPTR)S(ID_USE, "Use");
    menuDef[9].nm_Label = (STRPTR)S(ID_MENU_QUIT, "Quit");
    menuDef[10].nm_Label = (STRPTR)S(ID_MENU_EDIT, "Edit");
    menuDef[11].nm_Label = (STRPTR)S(ID_MENU_RESET_TO_DEFAULTS, "Reset to defaults");
    menuDef[12].nm_Label = (STRPTR)S(ID_MENU_LAST_SAVED, "Last saved");
    menuDef[13].nm_Label = (STRPTR)S(ID_MENU_RESTORE, "Restore");
    pageLabels[PG_MODES] = S(ID_MODES, "Modes");
    pageLabels[PG_SCREENS] = S(ID_TAB_SCREENS, "Screens");
    pageLabels[PG_BOARD] = S(ID_TAB_BOARD, "Board");
    pageLabels[PG_OTHER] = S(ID_TAB_OTHER, "Other");
}

/* a page gadget's attribute; nothing when its page isn't the one shown */
static void gset(struct Gui *g, struct Gadget *gad, ULONG tag, ULONG val)
{
    if (gad)
        GT_SetGadgetAttrs(gad, g->win, NULL, tag, val, TAG_END);
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
    gset(g, g->gPList, GTLV_Labels, ~0UL);
    build_progs(g);
    if (g->psel >= g->prefs.nprog)
        g->psel = g->prefs.nprog - 1;
    if (g->gPList)
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
    gset(g, g->gList, GTLV_Labels, ~0UL);
    for (i = 0; i < PREFS_NMODES; i++)
        entry_text(g, i);
    if (g->gList)
        GT_SetGadgetAttrs(g->gList, g->win, NULL, GTLV_Labels, (ULONG)&g->modes,
                          GTLV_Selected, (ULONG)g->sel, TAG_END);
}

/* the page's gadgets follow the selected mode / the global settings */
static void show_settings(struct Gui *g)
{
    struct PrismPrefs *p = &g->prefs;
    gset(g, g->gOn, GTCB_Checked, (ULONG)p->on[g->sel]);
    gset(g, g->gHz, GTCY_Active, (ULONG)rate_index(p->hz[g->sel]));
    gset(g, g->gBoard, GTCY_Active, (ULONG)board_entry(p));
    gset(g, g->gSecMon, GTCY_Active, (ULONG)sec_index(p));
    gset(g, g->gBlit, GTCB_Checked, (ULONG)p->blitter);
    gset(g, g->gLog, GTCY_Active, (ULONG)(p->log <= 2 ? p->log : 1));
    gset(g, g->gBgr, GTCB_Checked, (ULONG)p->clutBGR);
    gset(g, g->gDrag, GTCB_Checked, (ULONG)p->dragging);
    gset(g, g->gSoft, GTCB_Checked, (ULONG)p->softwarePointer);
    gset(g, g->gHwOvl, GTCB_Checked, (ULONG)p->hwOverlay);
    gset(g, g->gPromote, GTCB_Checked, (ULONG)p->promote);
    gset(g, g->gUpdates, GTCY_Active, (ULONG)(p->updates < UPD_COUNT ? p->updates : 0));
    gset(g, g->gGamma, GTCY_Active, (ULONG)gamma_index(p->gamma));
    gset(g, g->gPower, GTIN_Number, (ULONG)p->powerSave);
    gset(g, g->gMirror, GTCB_Checked, (ULONG)p->mirrorChip);
    if (g->gW && g->gH && g->gSet) {
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
    LONG w, h;
    if (!g->gW || !g->gH)                /* not the Modes page */
        return;
    w = ((struct StringInfo *)g->gW->SpecialInfo)->LongInt;
    h = ((struct StringInfo *)g->gH->SpecialInfo)->LongInt;
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

/* Why the last start at boot ended: the last PrismD or Prism line in
 * T:PrismD.log (prismmon.c writes it), or FALSE when it just stopped. A
 * closing "no supported board" gives way to the line before it when that
 * one says more (a card still held by an earlier start) */
static BOOL last_start(char *why, int len)
{
    char line[120], prev[60];
    BPTR f = Open((STRPTR)"T:PrismD.log", MODE_OLDFILE);
    why[0] = prev[0] = 0;
    if (!f)
        return FALSE;
    while (FGets(f, (STRPTR)line, sizeof(line))) {
        char *t = line;
        int n;
        if (!strncmp(t, "PrismD: ", 8))
            t += 8;
        else if (!strncmp(t, "Prism: ", 7))
            t += 7;
        else
            continue;
        if (!strncmp(t, "modes", 5) || t[0] == ' ' || t[0] == '$')
            continue;                         /* the mode list, not news */
        n = strlen(t);
        while (n && (t[n - 1] == '\n' || t[n - 1] == '\r'))
            t[--n] = 0;
        strcpy(prev, why);
        strncpy(why, t, len - 1);
        why[len - 1] = 0;
    }
    Close(f);
    if (!strcmp(why, "no supported board") && prev[0] && !strstr(prev, "no supported board"))
        strcpy(why, prev);
    return why[0] && strcmp(why, "stopped");
}

static void running_status(struct Gui *g)
{
    char why[60];
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
    if (!s && last_start(why, sizeof(why)))
        snprintf(buf, sizeof(buf), S(ID_PRISMD_DIDN_T_START, "Not running: %s"), why);
    else if (!s)
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

/* The window is tabbed so that it fits a 640x200 NTSC Workbench with topaz 8
 * (screen title bar and window borders included) and stays a normal size on
 * a big RTG screen: the page height is what the screen leaves, between the
 * smallest every page fits in (the mode list LIST_MIN lines) and the mode
 * list at LIST_MAX lines. A screen font too big for that gives way to
 * topaz 8, as WHDUpdate does. The window is at most WIN_MAXW wide. */
static struct TextAttr topaz8 = { (STRPTR)"topaz.font", 8, 0, FPF_ROMFONT };

#define WIN_MAXW 600
#define LIST_MIN 6
#define LIST_MAX 14

/* the labels make_layout measures and make_page shows */
#define T_OFFERED   S(ID_OFFERED, "Offered")
#define T_REFRESH   S(ID_REFRESH, "Refresh")
#define T_WIDTH     S(ID_WIDTH, "Width")
#define T_HEIGHT    S(ID_HEIGHT, "Height")
#define T_SETSIZE   S(ID_SET_SIZE, "Set size")
#define T_PROMOTESC S(ID_PROMOTE_SCREENS, "Promote screens")
#define T_PROGRAMS  S(ID_PROGRAMS, "Programs")
#define T_PROMOTE   S(ID_PROMOTE, "Promote")
#define T_KEEP      S(ID_KEEP, "Keep")
#define T_REMOVE    S(ID_REMOVE, "Remove")
#define T_PROGRAM   S(ID_PROGRAM, "Program")
#define T_DRAG      S(ID_DRAG_SCREENS, "Drag screens")
#define T_SOFT      S(ID_SOFT_POINTER, "Soft pointer")
#define T_BOARD     S(ID_BOARD, "Board")
#define T_SECMON    S(ID_2ND_MONITOR, "2nd monitor")
#define T_BLIT      S(ID_USE_BLITTER, "Use blitter")
#define T_BGR       S(ID_SWAP_RED_BLUE, "Swap red/blue")
#define T_HWOVL     S(ID_VIDEO_ON_BOARD, "Video on board")
#define T_MIRROR    S(ID_MIRROR_CHIPSET, "Mirror chipset")
#define T_GAMMA     S(ID_GAMMA, "Gamma")
#define T_SLEEP     S(ID_SLEEP, "Sleep")
#define T_LOG       S(ID_LOG, "Log")
#define T_UPDATES   S(ID_UPDATES, "Updates")
#define T_CHECK     S(ID_CHECK_NOW, "Check now")
#define T_SAVE      S(ID_SAVE, "Save")
#define T_USE       S(ID_USE, "Use")
#define T_TEST      S(ID_TEST, "Test")
#define T_DEFAULTS  S(ID_DEFAULTS, "Defaults")
#define T_CANCEL    S(ID_CANCEL, "Cancel")

static WORD max2(WORD a, WORD b)
{
    return a > b ? a : b;
}

static WORD tlen(struct Gui *g, const char *s)
{
    ULONG n = strlen(s);                 /* not inside the library macro's arguments */
    return TextLength(&g->mrp, (STRPTR)s, n);
}

static WORD tmax(struct Gui *g, STRPTR *l)
{
    WORD m = 0, w;
    for (; *l; l++)
        if ((w = tlen(g, (const char *)*l)) > m)
            m = w;
    return m;
}

/* everything but the page: the tab row, the status line, the buttons */
#define FIXED_H(l) ((l)->pageTop + 4 + (l)->gh + 3 + (l)->gh + 4)

/* The measures of every page in the font g->mrp has: the inner width (the
 * widest page), the smallest page height every page fits in, the tallest
 * worth having. */
static void make_layout(struct Gui *g)
{
    struct Layout *l = &g->lay;
    WORD w, cb, i, t;

    memset(l, 0, sizeof(*l));
    l->fw = g->font->tf_XSize;
    l->fh = g->font->tf_YSize;
    l->gh = l->fh + 6;
    l->row = l->gh + 3;
    l->cbw = 26;
    l->cbh = l->fh + 3;
    cb = l->cbw + 8;                     /* a checkbox and the gap to its text */
    l->listW = 38 * l->fw + 24;          /* a mode / program line, the scroller */

    /* Modes: the list, beside it Offered, Refresh, Width, Height, Set size */
    l->mLab = max2(max2(tlen(g, T_REFRESH), tlen(g, T_WIDTH)), tlen(g, T_HEIGHT)) + 8;
    l->mCol = max2(max2(tmax(g, hzLabels) + 34, 6 * l->fw + 12),
                   max2(tlen(g, T_SETSIZE) + 16, cb + tlen(g, T_OFFERED)));
    w = 8 + l->listW + 12 + l->mLab + l->mCol + 8;

    /* Screens: Promote screens, the Programs list with Promote / Keep /
     * Remove beside it, Program, then Drag screens and Soft pointer */
    l->sBtn = max2(max2(tlen(g, T_PROMOTE), tlen(g, T_KEEP)), tlen(g, T_REMOVE)) + 24;
    l->sLab = tlen(g, T_PROGRAM) + 8;
    l->sGap = cb + tlen(g, T_DRAG) + 24;
    w = max2(w, 8 + l->listW + 8 + l->sBtn + 8);
    w = max2(w, 8 + l->sGap + cb + tlen(g, T_SOFT) + 8);
    w = max2(w, 8 + cb + tlen(g, T_PROMOTESC) + 8);

    /* Board: Board and 2nd monitor, four checkboxes under them */
    l->bLab = max2(tlen(g, T_BOARD), tlen(g, T_SECMON)) + 8;
    l->bCol = max2(tmax(g, boardLabels), tmax(g, secLabels)) + 34;
    t = max2(max2(tlen(g, T_BLIT), tlen(g, T_BGR)), max2(tlen(g, T_HWOVL), tlen(g, T_MIRROR)));
    l->bCol = max2(l->bCol, cb + t);
    w = max2(w, 8 + l->bLab + l->bCol + 8);

    /* Other: Gamma, Sleep, Log, Updates with Check now beside it */
    l->oLab = max2(max2(tlen(g, T_GAMMA), tlen(g, T_SLEEP)), max2(tlen(g, T_LOG), tlen(g, T_UPDATES))) + 8;
    l->oCol = max2(max2(tmax(g, updLabels), tmax(g, gammaLabels)), tmax(g, logLabels)) + 34;
    l->oBtn = tlen(g, T_CHECK) + 24;
    w = max2(w, 8 + l->oLab + l->oCol + 12 + l->oBtn + 8);

    /* the five buttons, the tabs (tabs.c: text + 20 each) */
    t = max2(max2(tlen(g, T_SAVE), tlen(g, T_USE)), max2(tlen(g, T_TEST), max2(tlen(g, T_DEFAULTS), tlen(g, T_CANCEL))));
    w = max2(w, 16 + 5 * (t + 16) + 4 * 8);
    for (i = 0, t = 0; i < PG_COUNT; i++)
        t += tlen(g, pageLabels[i]) + 20;
    l->w = max2(w, 8 + t + 8);

    /* the heights: 2 + the tab row (font + 8) + 4, then the page */
    l->pageTop = 2 + l->fh + 8 + 4;
    l->minPage = max2(LIST_MIN * l->fh + 4, 4 * l->row + 6 + l->gh);           /* Modes */
    l->minPage = max2(l->minPage, 4 * l->row + 2 * l->gh + l->fh + 6);          /* Screens */
    l->minPage = max2(l->minPage, 5 * l->row + l->gh);                          /* Board */
    l->maxPage = max2(l->minPage, LIST_MAX * l->fh + 4);
}

/* The gadget font: the screen's when the window fits the screen with it,
 * else topaz 8; then the page height the screen leaves. */
static BOOL choose_font(struct Gui *g)
{
    struct Screen *s = g->scr;
    struct Layout *l = &g->lay;
    WORD aw = (s->Width < WIN_MAXW ? s->Width : WIN_MAXW) - s->WBorLeft - s->WBorRight;
    /* the window opens under the screen's title bar; its own title bar is
     * in the screen's font whatever the gadgets use */
    WORD ah = s->Height - (s->BarHeight + 1) - (s->WBorTop + s->Font->ta_YSize + 1) - s->WBorBottom;

    g->ta = *s->Font;
    if (!(g->font = OpenFont(&g->ta))) {
        g->ta = topaz8;
        if (!(g->font = OpenFont(&g->ta)))
            return FALSE;
    }
    InitRastPort(&g->mrp);
    SetFont(&g->mrp, g->font);
    make_layout(g);
    if (l->w > aw || FIXED_H(l) + l->minPage > ah) {
        CloseFont(g->font);
        g->ta = topaz8;
        if (!(g->font = OpenFont(&g->ta)))
            return FALSE;
        SetFont(&g->mrp, g->font);
        make_layout(g);
    }
    /* with topaz 8 every page fits a 640x200 screen; a still smaller one
     * gets the smallest page and WA_AutoAdjust */
    l->pageH = ah - FIXED_H(l);
    if (l->pageH > l->maxPage)
        l->pageH = l->maxPage;
    if (l->pageH < l->minPage)
        l->pageH = l->minPage;
    return TRUE;
}

/* one gadget into the list being built (ng, gad); var takes it */
#define MK(var, kind, id, x_, y_, w_, h_, txt, fl, ...) do {                 \
        ng.ng_LeftEdge = (x_); ng.ng_TopEdge = (y_);                          \
        ng.ng_Width = (w_); ng.ng_Height = (h_);                              \
        ng.ng_GadgetText = (STRPTR)(txt); ng.ng_Flags = (fl);                 \
        ng.ng_GadgetID = (id);                                                \
        gad = (var) = CreateGadget(kind, gad, &ng, __VA_ARGS__, TAG_END);     \
    } while (0)

/* the status line and Save / Use / Test / Defaults / Cancel, under the page */
static BOOL make_base(struct Gui *g)
{
    struct Layout *l = &g->lay;
    struct NewGadget ng;
    struct Gadget *gad, *b;
    WORD x0 = g->win->BorderLeft + 8, cw = l->w - 16, bw = (cw - 4 * 8) / 5;
    WORD y = g->win->BorderTop + l->pageTop + l->pageH + 4;

    if (!(gad = CreateContext(&g->bglist)))
        return FALSE;
    memset(&ng, 0, sizeof(ng));
    ng.ng_TextAttr = &g->ta;
    ng.ng_VisualInfo = g->vi;
    MK(g->gStatus, TEXT_KIND, GAD_STATUS, x0, y, cw, l->gh, NULL, 0,
       GTTX_Text, (ULONG)g->status, GTTX_Border, TRUE, GTTX_Clipped, TRUE);
    y += l->gh + 3;
    MK(b, BUTTON_KIND, GAD_SAVE, x0, y, bw, l->gh, T_SAVE, PLACETEXT_IN, TAG_IGNORE, 0);
    MK(b, BUTTON_KIND, GAD_USE, x0 + (bw + 8), y, bw, l->gh, T_USE, PLACETEXT_IN, TAG_IGNORE, 0);
    MK(b, BUTTON_KIND, GAD_TEST, x0 + 2 * (bw + 8), y, bw, l->gh, T_TEST, PLACETEXT_IN, TAG_IGNORE, 0);
    MK(b, BUTTON_KIND, GAD_DEFAULTS, x0 + 3 * (bw + 8), y, bw, l->gh, T_DEFAULTS, PLACETEXT_IN, TAG_IGNORE, 0);
    MK(b, BUTTON_KIND, GAD_CANCEL, x0 + cw - bw, y, bw, l->gh, T_CANCEL, PLACETEXT_IN, TAG_IGNORE, 0);
    (void)b;
    return gad != NULL;
}

static void clear_page(struct Gui *g)
{
    g->gList = g->gOn = g->gHz = g->gBoard = g->gBlit = g->gLog = g->gBgr = g->gDrag = NULL;
    g->gSoft = g->gHwOvl = g->gPromote = g->gPList = g->gProg = g->gUpdates = NULL;
    g->gW = g->gH = g->gSet = g->gGamma = g->gPower = g->gMirror = g->gSecMon = NULL;
}

/* The shown tab's gadgets (glist). Created with their kinds only:
 * show_settings puts the values in. */
static BOOL make_page(struct Gui *g)
{
    struct Layout *l = &g->lay;
    struct NewGadget ng;
    struct Gadget *gad, *b;
    WORD x0 = g->win->BorderLeft + 8, y0 = g->win->BorderTop + l->pageTop;
    WORD gh = l->gh, row = l->row, x, y;

    clear_page(g);
    if (!(gad = CreateContext(&g->glist)))
        return FALSE;
    memset(&ng, 0, sizeof(ng));
    ng.ng_TextAttr = &g->ta;
    ng.ng_VisualInfo = g->vi;

    switch (g->page) {
    case PG_MODES:
        /* the selected mode's Offered and Refresh; a custom slot's size */
        x = x0 + l->listW + 12 + l->mLab;
        MK(g->gOn, CHECKBOX_KIND, GAD_ON, x, y0 + 1, l->cbw, l->cbh, T_OFFERED, PLACETEXT_RIGHT,
           GTCB_Scaled, TRUE);
        MK(g->gHz, CYCLE_KIND, GAD_HZ, x, y0 + row, l->mCol, gh, T_REFRESH, PLACETEXT_LEFT,
           GTCY_Labels, (ULONG)hzLabels);
        y = y0 + 2 * row + 6;
        MK(g->gW, INTEGER_KIND, GAD_W, x, y, l->mCol, gh, T_WIDTH, PLACETEXT_LEFT,
           GTIN_MaxChars, 4, GA_Disabled, TRUE);
        MK(g->gH, INTEGER_KIND, GAD_H, x, y + row, l->mCol, gh, T_HEIGHT, PLACETEXT_LEFT,
           GTIN_MaxChars, 4, GA_Disabled, TRUE);
        MK(g->gSet, BUTTON_KIND, GAD_SETSIZE, x, y + 2 * row, l->mCol, gh, T_SETSIZE, PLACETEXT_IN,
           GA_Disabled, TRUE);
        /* listviews last: their scroller sub-gadgets end the chain */
        MK(g->gList, LISTVIEW_KIND, GAD_LIST, x0, y0, l->listW, l->pageH, NULL, 0,
           GTLV_Labels, (ULONG)&g->modes, GTLV_ShowSelected, 0, GTLV_Selected, (ULONG)g->sel,
           GTLV_MakeVisible, (ULONG)g->sel);
        break;

    case PG_SCREENS:
        /* screen mode promotion (PROMOTE=), its exceptions in the list */
        MK(g->gPromote, CHECKBOX_KIND, GAD_PROMOTE, x0, y0 + 1, l->cbw, l->cbh, T_PROMOTESC, PLACETEXT_RIGHT,
           GTCB_Scaled, TRUE);
        y = y0 + row + l->fh + 3;
        x = x0 + l->listW + 8;
        MK(b, BUTTON_KIND, GAD_PADD, x, y, l->sBtn, gh, T_PROMOTE, PLACETEXT_IN, TAG_IGNORE, 0);
        MK(b, BUTTON_KIND, GAD_PKEEP, x, y + row, l->sBtn, gh, T_KEEP, PLACETEXT_IN, TAG_IGNORE, 0);
        MK(b, BUTTON_KIND, GAD_PDEL, x, y + 2 * row, l->sBtn, gh, T_REMOVE, PLACETEXT_IN, TAG_IGNORE, 0);
        (void)b;
        MK(g->gProg, STRING_KIND, GAD_PROG, x0 + l->sLab, y + 2 * row + gh + 3,
           l->listW + 8 + l->sBtn - l->sLab, gh, T_PROGRAM, PLACETEXT_LEFT,
           GTST_MaxChars, PREFS_PROGNAME, GTST_String, (ULONG)g->progName);
        /* the two experimental switches (DRAGGING=, SOFTWAREPOINTER=) */
        MK(g->gDrag, CHECKBOX_KIND, GAD_DRAG, x0, y + 3 * row + gh + 4, l->cbw, l->cbh, T_DRAG, PLACETEXT_RIGHT,
           GTCB_Scaled, TRUE);
        MK(g->gSoft, CHECKBOX_KIND, GAD_SOFTPTR, x0 + l->sGap, y + 3 * row + gh + 4, l->cbw, l->cbh, T_SOFT,
           PLACETEXT_RIGHT, GTCB_Scaled, TRUE);
        MK(g->gPList, LISTVIEW_KIND, GAD_PLIST, x0, y, l->listW, 2 * row + gh, T_PROGRAMS,
           PLACETEXT_ABOVE | NG_HIGHLABEL, GTLV_Labels, (ULONG)&g->progs, GTLV_ShowSelected, 0);
        break;

    case PG_BOARD:
        x = x0 + l->bLab;
        MK(g->gBoard, CYCLE_KIND, GAD_BOARD, x, y0, l->bCol, gh, T_BOARD, PLACETEXT_LEFT,
           GTCY_Labels, (ULONG)boardLabels);
        /* SECONDMONITOR=: the second board's monitor with nothing of its own on it */
        MK(g->gSecMon, CYCLE_KIND, GAD_SECMON, x, y0 + row, l->bCol, gh, T_SECMON, PLACETEXT_LEFT,
           GTCY_Labels, (ULONG)secLabels);
        MK(g->gBlit, CHECKBOX_KIND, GAD_BLIT, x, y0 + 2 * row + 1, l->cbw, l->cbh, T_BLIT, PLACETEXT_RIGHT,
           GTCB_Scaled, TRUE);
        MK(g->gBgr, CHECKBOX_KIND, GAD_BGR, x, y0 + 3 * row + 1, l->cbw, l->cbh, T_BGR, PLACETEXT_RIGHT,
           GTCB_Scaled, TRUE);
        /* video layers on the board's own overlay (HWOVERLAY=; the ZZ9000) */
        MK(g->gHwOvl, CHECKBOX_KIND, GAD_HWOVL, x, y0 + 4 * row + 1, l->cbw, l->cbh, T_HWOVL, PLACETEXT_RIGHT,
           GTCB_Scaled, TRUE);
        /* MIRROR=: chipset screens copied to the board's monitor */
        MK(g->gMirror, CHECKBOX_KIND, GAD_MIRROR, x, y0 + 5 * row + 1, l->cbw, l->cbh, T_MIRROR, PLACETEXT_RIGHT,
           GTCB_Scaled, TRUE);
        break;

    case PG_OTHER:
        x = x0 + l->oLab;
        /* GAMMA=: the palette's curve (256-colour screens) */
        MK(g->gGamma, CYCLE_KIND, GAD_GAMMA, x, y0, l->oCol, gh, T_GAMMA, PLACETEXT_LEFT,
           GTCY_Labels, (ULONG)gammaLabels);
        /* POWERSAVE=: minutes without input before the monitors go dark */
        MK(g->gPower, INTEGER_KIND, GAD_POWER, x, y0 + row, 5 * l->fw + 12, gh, T_SLEEP, PLACETEXT_LEFT,
           GTIN_MaxChars, 3);
        MK(g->gLog, CYCLE_KIND, GAD_LOG, x, y0 + 2 * row, l->oCol, gh, T_LOG, PLACETEXT_LEFT,
           GTCY_Labels, (ULONG)logLabels);
        /* update checks (UPDATES=, PrismUpdate): where to look, and a check now */
        MK(g->gUpdates, CYCLE_KIND, GAD_UPDATES, x, y0 + 3 * row, l->oCol, gh, T_UPDATES, PLACETEXT_LEFT,
           GTCY_Labels, (ULONG)updLabels);
        MK(b, BUTTON_KIND, GAD_CHECK, x + l->oCol + 12, y0 + 3 * row, l->oBtn, gh, T_CHECK, PLACETEXT_IN,
           TAG_IGNORE, 0);
        (void)b;
        break;
    }
    return gad != NULL;
}

/* what the page's fields hold that isn't in the prefs yet (typed in
 * without Return): taken before the page goes */
static void store_page(struct Gui *g)
{
    if (g->gPower) {
        LONG m = ((struct StringInfo *)g->gPower->SpecialInfo)->LongInt;
        if (m >= 0 && m <= 240)
            g->prefs.powerSave = m;
    }
    if (g->gProg) {
        strncpy(g->progName, (const char *)((struct StringInfo *)g->gProg->SpecialInfo)->Buffer, PREFS_PROGNAME);
        g->progName[PREFS_PROGNAME] = 0;
    }
}

/* the shown tab's gadgets into the window */
static BOOL add_page(struct Gui *g)
{
    if (!make_page(g)) {
        FreeGadgets(g->glist);           /* (NULL is fine) */
        g->glist = NULL;
        clear_page(g);
        return FALSE;
    }
    AddGList(g->win, g->glist, ~0, -1, NULL);
    RefreshGList(g->glist, g->win, NULL, -1);
    GT_RefreshWindow(g->win, NULL);
    show_settings(g);
    return TRUE;
}

/* another tab: the old page's gadgets go, the new one's come */
static void switch_page(struct Gui *g, int page)
{
    struct Window *w = g->win;
    struct Layout *l = &g->lay;

    store_page(g);
    if (g->glist) {
        RemoveGList(w, g->glist, -1);
        FreeGadgets(g->glist);
        g->glist = NULL;
    }
    clear_page(g);
    EraseRect(w->RPort, w->BorderLeft, w->BorderTop + l->pageTop - 3,
              w->Width - w->BorderRight - 1, w->BorderTop + l->pageTop + l->pageH + 3);
    g->page = g->tabs.active = page;
    if (!add_page(g))
        set_status(g, S(ID_NOT_ENOUGH_MEMORY_FOR_THIS_PAGE, "Not enough memory for this page"));
    tabs_draw(&g->tabs, w);
}

static BOOL open_window(struct Gui *g)
{
    struct Layout *l;

    if (!(g->scr = LockPubScreen(NULL)) || !(g->vi = GetVisualInfo(g->scr, TAG_END)))
        return FALSE;
    if (!choose_font(g))
        return FALSE;
    l = &g->lay;
    g->win = OpenWindowTags(NULL,
        WA_Title, (ULONG)S(ID_PRISMRTG_PREFERENCES, "PrismRTG Preferences"),
        WA_Left, 20, WA_Top, g->scr->BarHeight + 1,
        WA_InnerWidth, l->w, WA_InnerHeight, FIXED_H(l) + l->pageH,
        WA_AutoAdjust, TRUE,
        WA_NewLookMenus, TRUE,
        WA_Flags, WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_CLOSEGADGET | WFLG_ACTIVATE |
                  WFLG_SMART_REFRESH,
        WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW | IDCMP_VANILLAKEY |
                  IDCMP_MOUSEBUTTONS | IDCMP_MENUPICK |
                  BUTTONIDCMP | CHECKBOXIDCMP | CYCLEIDCMP | LISTVIEWIDCMP | STRINGIDCMP,
        WA_PubScreen, (ULONG)g->scr,
        TAG_END);
    if (!g->win)
        return FALSE;
    /* the tab row under the title bar, in the gadgets' font */
    SetFont(g->win->RPort, g->font);
    tabs_init(&g->tabs, g->win->RPort, pageLabels, PG_COUNT, GAD_TAB0,
              g->win->BorderLeft + 4, g->win->BorderTop + 2, l->w - 8);
    tabs_add(&g->tabs, g->win);
    if (!make_base(g))
        return FALSE;
    AddGList(g->win, g->bglist, ~0, -1, NULL);
    RefreshGList(g->bglist, g->win, NULL, -1);
    if (!add_page(g))
        return FALSE;
    tabs_draw(&g->tabs, g->win);
    /* the menus; without them the window still works by its buttons */
    if ((g->menu = CreateMenus(menuDef, TAG_END))) {
        if (LayoutMenus(g->menu, g->vi, GTMN_NewLookMenus, TRUE, TAG_END))
            SetMenuStrip(g->win, g->menu);
        else {
            FreeMenus(g->menu);
            g->menu = NULL;
        }
    }
    return TRUE;
}

/* ---- the actions the buttons and the menus share --------------------- */

/* new settings as a whole (Defaults, Last saved, Restore): every gadget follows */
static void show_prefs(struct Gui *g, const char *msg)
{
    refresh_list(g);
    g->psel = -1;
    refresh_progs(g);
    show_settings(g);
    set_status(g, msg);
}

/* Test: with two boards, ask which one shows it */
static void test_selected(struct Gui *g)
{
    if (g->boards > 1) {
        struct EasyStruct es = { sizeof(struct EasyStruct), 0, (STRPTR)S(ID_PRISM_MODE_TEST, "Prism mode test"),
                                 (STRPTR)S(ID_SHOW_THE_TEST_PICTURE_ON_WHICH_BOARD, "Show the test picture on which board?"),
                                 (STRPTR)S(ID_FIRST_SECOND_CANCEL, "First|Second|Cancel") };
        LONG b = EasyRequest(g->win, &es, NULL);   /* 1 First, 2 Second, 0 Cancel */
        if (!b)
            return;
        g->testOn = b == 2;
    }
    test_mode(g, g->sel, TRUE);
}

static void about(struct Gui *g)
{
    struct EasyStruct es = { sizeof(struct EasyStruct), 0, (STRPTR)S(ID_ABOUT_PRISMPREFS, "About PrismPrefs"),
                             (STRPTR)"PrismPrefs %s\n\n%s\n\n%s", (STRPTR)S(ID_OK, "OK") };
    EasyRequest(g->win, &es, NULL, (ULONG)PRISM_RELEASE,
                (ULONG)S(ID_THE_SETTINGS_EDITOR_OF_PRISMRTG, "The settings editor of PrismRTG, the RTG system for AmigaOS 3.x"),
                (ULONG)S(ID_LICENCE_GPL_3_0, "Licence: GPL-3.0"));
}

/* a menu item; TRUE when the window should close */
static BOOL menu_pick(struct Gui *g, ULONG id)
{
    switch (id) {
    case MN_ABOUT:
        about(g);
        break;
    case MN_TEST:
        test_selected(g);
        break;
    case MN_CHECK:
        check_update(g);
        break;
    case MN_SAVE:
        return write_prefs(g, TRUE);
    case MN_USE:
        return write_prefs(g, FALSE);
    case MN_QUIT:                        /* as Cancel: nothing written */
        return TRUE;
    case MN_DEFAULTS:
        prefs_default(&g->prefs);
        show_prefs(g, S(ID_DEFAULTS_EVERY_MODE_THE_BOARD_CAN_SHOW, "Defaults - every mode the board can show"));
        break;
    case MN_LASTSAVED:
        /* prefs_load starts from the defaults: no ENVARC: file is the defaults */
        show_prefs(g, prefs_load(&g->prefs, PREFS_ENVARC)
                      ? S(ID_THE_SETTINGS_LAST_SAVED_ENVARC, "The settings last saved (ENVARC:Prism.prefs)")
                      : S(ID_NOTHING_SAVED_YET_THE_DEFAULTS, "Nothing saved yet: the defaults"));
        break;
    case MN_RESTORE:
        g->prefs = g->opened;
        show_prefs(g, S(ID_THE_SETTINGS_THE_WINDOW_OPENED_WITH, "The settings the window opened with"));
        break;
    }
    return FALSE;
}

/* returns when the window should close */
static void event_loop(struct Gui *g)
{
    BOOL done = FALSE;

    while (!done) {
        struct IntuiMessage *im;
        int newpage = -1;                /* a tab clicked: switched once the queue is empty */
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
                tabs_draw(&g->tabs, g->win);
                GT_EndRefresh(g->win, TRUE);
                break;
            case IDCMP_MENUPICK:
                /* every item picked in one go (NextSelect), until one closes */
                while (code != MENUNULL && !done) {
                    struct MenuItem *it = ItemAddress(g->menu, code);
                    if (!it)
                        break;
                    done = menu_pick(g, (ULONG)GTMENUITEM_USERDATA(it));
                    code = it->NextSelect;
                }
                break;
            case IDCMP_GADGETUP:
                if (tabs_hit(&g->tabs, gad->GadgetID) >= 0) {
                    newpage = tabs_hit(&g->tabs, gad->GadgetID);
                    break;
                }
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
                case GAD_SECMON:
                    g->prefs.secondMon = code < sizeof(secValues) ? secValues[code] : SM_SCREEN;
                    if (g->prefs.secondMon == SM_SPAN)
                        g->prefs.secondLeft = code == SEC_SPAN_LEFT;
                    set_status(g, g->prefs.secondMon == SM_SCREEN ? S(ID_SECOND_BOARD_SHOWS_A_SCREEN_OF_ITS_OWN, "Second board: a screen of its own, PRISM2 (restart PrismD)")
                                  : g->prefs.secondMon == SM_MIRROR ? S(ID_SECOND_BOARD_SHOWS_A_COPY_OF_THE_FIRST, "Second board: a copy of the first one's picture (restart PrismD)")
                                  : g->prefs.secondMon == SM_SPAN ? S(ID_SPAN_MODES_ONE_SCREEN_ACROSS_BOTH_MONITO, "Span modes: one screen across both monitors (restart PrismD, then pick one in ScreenMode)")
                                  : S(ID_SECOND_BOARD_SHOWS_THE_AMIGA_S_VIDEO, "Second board: the Amiga's own video while nothing is on it (restart PrismD)"));
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
                    if (code < g->prefs.nprog) {
                        strcpy(g->progName, g->prefs.prog[code].name);
                        gset(g, g->gProg, GTST_String, (ULONG)g->progName);
                    }
                    break;
                case GAD_PADD:
                case GAD_PKEEP: {
                    const char *nm;
                    store_page(g);               /* the Program field into progName */
                    nm = g->progName;
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
                    test_selected(g);
                    break;
                case GAD_SAVE:
                    done = menu_pick(g, MN_SAVE);
                    break;
                case GAD_USE:
                    done = menu_pick(g, MN_USE);
                    break;
                case GAD_DEFAULTS:
                    menu_pick(g, MN_DEFAULTS);
                    break;
                case GAD_CANCEL:
                    done = TRUE;
                    break;
                }
                break;
            }
        }
        /* after the queue: no message left points at the old page's gadgets */
        if (!done && newpage >= 0 && newpage != g->page)
            switch_page(g, newpage);
    }
}

/* ---- main ----------------------------------------------------------- */

#define TEMPLATE "FROM/K,USE/S,SAVE/S,TEST/K/N,TESTON/K/N"

int main(void)
{
    LONG args[5] = { 0 };
    struct RDArgs *rda = NULL;
    int rc = 20;

    /* Shell arguments only from a Shell: started from its icon there is no
     * argument line, and ReadArgs then waited for one on an empty console
     * window that the C library opened for it */
    if (((struct Process *)FindTask(NULL))->pr_CLI &&
        !(rda = ReadArgs(TEMPLATE, args, NULL))) {
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

    g.opened = g.prefs;
    running_status(&g);
    build_list(&g);
    build_progs(&g);
    g.psel = -1;
    if (open_window(&g)) {
        event_loop(&g);
        rc = 0;
    }

out:
    if (g.win && g.menu) ClearMenuStrip(g.win);
    if (g.win) {
        /* AddGList chained each list onto the one before it: take them off
         * (the page first, it is last) so each ends on its own before
         * FreeGadgets walks it */
        if (g.glist) RemoveGList(g.win, g.glist, -1);
        if (g.bglist) RemoveGList(g.win, g.bglist, -1);
        tabs_remove(&g.tabs, g.win);
        CloseWindow(g.win);
    }
    if (g.menu) FreeMenus(g.menu);
    if (g.glist) FreeGadgets(g.glist);
    if (g.bglist) FreeGadgets(g.bglist);
    if (g.font) CloseFont(g.font);
    if (g.vi) FreeVisualInfo(g.vi);
    if (g.scr) UnlockPubScreen(NULL, g.scr);
    if (GadToolsBase) CloseLibrary(GadToolsBase);
    if (GfxBase) CloseLibrary((struct Library *)GfxBase);
    if (IntuitionBase) CloseLibrary((struct Library *)IntuitionBase);
    cat_close();
    if (rda) FreeArgs(rda);
    return rc;
}
