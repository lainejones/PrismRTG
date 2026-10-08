/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laine Jones */
/*
 * prefs.h - Prism's settings (ENV:Prism.prefs), shared by PrismD and the
 * PrismPrefs editor.
 *
 * The file is plain text, one KEY=VALUE per line:
 *
 *   BOARD=AUTO|PICASSO2|ZZ9000|P96|UAEGFX
 *   P96CARD=LIBS:Picasso96/uaegfx.card
 *   P96MONITOR=SYS:Storage/Monitors/UAEgfx  (optional icon tooltypes)
 *   SOFTWAREPOINTER=ON|OFF      (force the software sprite path)
 *   DRAGGING=ON|OFF             (experimental, defaults to OFF)
 *   BLITTER=ON|OFF
 *   LOG=ON|OFF
 *   PALETTE=RGB|BGR             (Piccolo/Spectrum only, see drv_picasso2.c)
 *   MODE=640x480x8 ON 72        (size x depth, ON/OFF, refresh Hz or 0)
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

#define PREFS_NSIZES  8
#define PREFS_NDEPTHS 4
#define PREFS_NMODES  (PREFS_NSIZES * PREFS_NDEPTHS)
#define PREFS_NRATES  5

extern const UWORD prefs_sizes[PREFS_NSIZES][2];
extern const UBYTE prefs_depths[PREFS_NDEPTHS];  /* bits per pixel: 8, 16, 24, 32 */
extern const UBYTE prefs_rates[PREFS_NRATES];    /* 0 = driver's default   */

/* slot i: size i % PREFS_NSIZES, depth i / PREFS_NSIZES. The low ModeID
 * bits are depth << 8 | size (8-bit modes keep the IDs they always had). */
#define PREFS_SIZE(i)   ((i) % PREFS_NSIZES)
#define PREFS_DEPTH(i)  ((i) / PREFS_NSIZES)
#define PREFS_IDLOW(i)  ((ULONG)PREFS_DEPTH(i) << 8 | PREFS_SIZE(i))

enum { PB_AUTO, PB_PICASSO2, PB_ZZ9000, PB_P96, PB_UAEGFX, PB_COUNT };

struct PrismPrefs {
    UBYTE board;                     /* PB_*                               */
    UBYTE blitter;                   /* use the card's blitter             */
    UBYTE log;                       /* PrismD prints its debug log        */
    UBYTE softwarePointer;          /* override advertised hardware sprite */
    UBYTE dragging;                 /* opt in to software screen splits   */
    UBYTE clutBGR;                   /* PALETTE=BGR: load palettes blue first */
    UBYTE on[PREFS_NMODES];          /* mode offered                       */
    UBYTE hz[PREFS_NMODES];          /* refresh wanted, 0 = default        */
    char p96card[256];
    char p96monitor[256];
};

void prefs_default(struct PrismPrefs *p);
/* FALSE if the file is missing (p then holds the defaults) */
BOOL prefs_load(struct PrismPrefs *p, const char *path);
BOOL prefs_save(const struct PrismPrefs *p, const char *path);

#endif
