/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laine Jones
 * Copyright (C) 2026 Stefan Reinauer */
/*
 * prefs.h - Prism's settings (ENV:Prism.prefs), shared by PrismD and the
 * PrismPrefs editor.
 *
 * The file is plain text, one KEY=VALUE per line:
 *
 *   BOARD=AUTO|PICASSO2|ZZ9000|P96|UAEGFX|<name of any LIBS:Prism/<name>.driver>
 *   P96CARD=LIBS:Picasso96/uaegfx.card
 *   P96MONITOR=SYS:Storage/Monitors/UAEgfx  (optional icon tooltypes)
 *   SOFTWAREPOINTER=ON|OFF      (force the software sprite path)
 *   DRAGGING=ON|OFF             (experimental, defaults to OFF)
 *   BLITTER=ON|OFF
 *   LOG=ON|OFF
 *   PALETTE=RGB|BGR             (Piccolo/Spectrum only, see drv_picasso2.c)
 *   MODE=640x480x8 ON 72        (size x depth, ON/OFF, refresh Hz or 0)
 *   CUSTOM1=1152x864            (a size of the user's own, slots 11-14; up to
 *                                CUSTOM4; a board shows it if its driver can
 *                                time it - the Cirrus family computes a VESA
 *                                GTF timing, the ZZ9000 has its firmware's list)
 *   PROMOTE=ON|OFF              (native-chipset screens open on the board)
 *   PROMOTEPROG=<program>       (promoted even with PROMOTE=OFF)
 *   KEEPPROG=<program>          (left on the chipset even with PROMOTE=ON)
 *   HWOVERLAY=ON|OFF            (video layers on the board's own overlay where
 *                                it has one - the ZZ9000; OFF: composed on the CPU)
 *   GAMMA=1.4                   (0.5 to 2.5, 1.0 = off: the curve the board's
 *                                palette is loaded through; 256-colour
 *                                screens and the Test picture, since the
 *                                DAC is where it applies)
 *   MIRRORCHIPSET=ON|OFF        (a chipset screen in front - one promotion left
 *                                alone - shown on the primary board as a copy)
 *   POWERSAVE=10                (minutes without a key or mouse event before the
 *                                boards' monitors go dark; 0 or OFF = never)
 *   UPDATES=OFF|GITHUB|MIRROR|BOTH  (where PrismUpdate looks for a newer release)
 *   SECONDMONITOR=SCREEN|MIRROR|SPAN|OFF  (what a second board shows with no
 *                                screen of its own; docs/multi-board.md)
 *   SECONDSIDE=RIGHT|LEFT       (where the second monitor stands: the pointer
 *                                pushed off that edge goes over to it)
 *   MIRROR=https://...          (a mirror's directory: latest.txt and the archive)
 *
 * Promotion: a screen a program opens on a PAL, NTSC or other native
 * monitor (or with no mode at all) is opened instead on the primary
 * board's smallest 256-colour mode that holds it, at the depth asked for.
 * The program's name is its command name (a CLI program) or its task name
 * (started from Workbench), compared without regard to case.
 *
 * Every candidate mode has a fixed slot, and its ModeID comes from the
 * slot, so turning one mode off never renumbers the others (a saved
 * Workbench ModeID stays valid).
 */
#ifndef PRISM_PREFS_H
#define PRISM_PREFS_H

#include <exec/types.h>

#define PREFS_ENV     "ENV:Prism.prefs"
#define PREFS_ENVARC  "ENVARC:Prism.prefs"

/* size slots: the fixed sizes (0-10), the user's own (11-14), then the larger
 * fixed ones (15-21). A slot's number is in its ModeID, so new sizes only
 * ever go at the end. */
#define PREFS_NFIXED  11             /* the first fixed sizes                */
#define PREFS_NCUSTOM 4              /* CUSTOM1..4=WxH, the user's own sizes */
#define PREFS_NEXTRA  7              /* the larger fixed sizes (#14), off at first */
#define PREFS_NSIZES  (PREFS_NFIXED + PREFS_NCUSTOM + PREFS_NEXTRA)
#define PREFS_IS_CUSTOM(s) ((s) >= PREFS_NFIXED && (s) < PREFS_NFIXED + PREFS_NCUSTOM)
#define PREFS_NDEPTHS 4
#define PREFS_NMODES  (PREFS_NSIZES * PREFS_NDEPTHS)
#define PREFS_NRATES  5

extern const UWORD prefs_sizes[PREFS_NFIXED][2];
extern const UWORD prefs_extra[PREFS_NEXTRA][2];
extern const UBYTE prefs_depths[PREFS_NDEPTHS];  /* bits per pixel: 8, 16, 24, 32 */
extern const UBYTE prefs_rates[PREFS_NRATES];    /* 0 = driver's default   */

/* slot i: size i % PREFS_NSIZES, depth i / PREFS_NSIZES. The low ModeID
 * bits are depth << 8 | size (8-bit modes keep the IDs they always had). */
#define PREFS_SIZE(i)   ((i) % PREFS_NSIZES)
#define PREFS_DEPTH(i)  ((i) / PREFS_NSIZES)
#define PREFS_IDLOW(i)  ((ULONG)PREFS_DEPTH(i) << 8 | PREFS_SIZE(i))

enum { PB_AUTO, PB_PICASSO2, PB_ZZ9000, PB_P96, PB_UAEGFX, PB_OTHER, PB_COUNT };
#define PREFS_BOARDNAME 32           /* driver_open's limit on a name       */
#define PREFS_NPROGS    16           /* promotion exceptions                */
#define PREFS_PROGNAME  31
#define PREFS_MIRROR    127
/* UPDATES=: off, GitHub's releases, a mirror, GitHub then the mirror */
enum { UPD_OFF, UPD_GITHUB, UPD_MIRROR, UPD_BOTH, UPD_COUNT };
/* SECONDMONITOR=: the second board's monitor while nothing is opened on it */
enum { SM_OFF, SM_SCREEN, SM_MIRROR, SM_SPAN, SM_COUNT };

struct PrefsProg {
    char  name[PREFS_PROGNAME + 1];
    UBYTE keep;                      /* 1: never promoted, 0: always promoted */
};

struct PrismPrefs {
    UBYTE board;                     /* PB_*                               */
    char boardName[PREFS_BOARDNAME + 1]; /* PB_OTHER: LIBS:Prism/<name>.driver */
    UBYTE blitter;                   /* use the card's blitter             */
    UBYTE log;                       /* PrismD prints its debug log        */
    UBYTE softwarePointer;          /* override advertised hardware sprite */
    UBYTE dragging;                 /* opt in to software screen splits   */
    UBYTE clutBGR;                   /* PALETTE=BGR: load palettes blue first */
    UBYTE swapBoards;                /* PRIMARY=SECOND: of the two boards Auto
                                        finds, the second is the primary (the
                                        PRISM: modes, Workbench) */
    UBYTE second;                    /* SECONDBOARD=: on Auto, also drive a second
                                        board if there is one (multi-board.md) */
    UBYTE on[PREFS_NMODES];          /* mode offered                       */
    UBYTE hz[PREFS_NMODES];          /* refresh wanted, 0 = default        */
    UWORD custom[PREFS_NCUSTOM][2];  /* CUSTOMn= width, height; 0 = unused */
    UBYTE promote;                   /* PROMOTE=ON: native screens on the board */
    UBYTE nprog;
    struct PrefsProg prog[PREFS_NPROGS];
    UBYTE gamma;                     /* GAMMA= in tenths: 10 = 1.0 (off), 5..25 */
    UBYTE hwOverlay;                 /* HWOVERLAY=ON: video layers on the board's
                                        own overlay where it has one (ZZ9000) */
    UBYTE updates;                   /* UPD_*: where PrismUpdate looks         */
    UBYTE mirrorChip;                /* MIRRORCHIPSET=ON: chipset screens in front copied
                                        to the primary board's monitor          */
    UBYTE powerSave;                 /* POWERSAVE= minutes idle before the monitors
                                        go dark, 0 = never (1-240)              */
    UBYTE secondMon;                 /* SM_*: SECONDMONITOR=                   */
    UBYTE secondLeft;                /* SECONDSIDE=LEFT: the second monitor stands
                                        left of the first (the pointer crosses
                                        over at the left edge)                 */
    char mirror[PREFS_MIRROR + 1];   /* MIRROR=: an HTTPS directory            */
    char p96card[256];
    char p96monitor[256];
};

void prefs_default(struct PrismPrefs *p);
/* the size of mode slot i (a fixed one or a CUSTOMn); FALSE for an unset
 * custom slot */
BOOL prefs_size(const struct PrismPrefs *p, int i, UWORD *w, UWORD *h);
/* the program's entry in the promotion list, or -1 */
int  prefs_prog_find(const struct PrismPrefs *p, const char *name);
/* add or change an entry; FALSE when the list is full or the name is no good */
BOOL prefs_prog_set(struct PrismPrefs *p, const char *name, BOOL keep);
void prefs_prog_remove(struct PrismPrefs *p, int i);
/* FALSE if the file is missing (p then holds the defaults) */
BOOL prefs_load(struct PrismPrefs *p, const char *path);
BOOL prefs_save(const struct PrismPrefs *p, const char *path);

#endif
