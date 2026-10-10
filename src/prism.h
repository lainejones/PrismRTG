/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laine Jones */
/*
 * prism.h - what other programs can see of a running Prism.
 *
 * M1: Prism runs as the PrismD daemon and publishes a named semaphore,
 * "prism", holding this structure. Clients find it with FindSemaphore()
 * under Forbid(). (From M2 on this becomes prism.library.)
 */
#ifndef PRISM_H
#define PRISM_H

#include <exec/semaphores.h>
#include <intuition/screens.h>

#define PRISM_SEMNAME     "prism"
#define PRISM_VERSION     4

/* Boot failsafe: DEVS:Monitors/Prism creates this before starting PrismD
 * and PrismD deletes it after a minute. Found at boot, it means the last
 * start never got that far: Prism is skipped once. */
#define PRISM_BOOTFLAG    "ENVARC:Prism.booting"

/* Prism ModeIDs: one "monitor", 0x7A00xxxx; low bits = depth slot << 8 |
 * size slot (prefs.h), so an ID never changes when other modes are off. */
#define PRISM_MONITOR_ID  0x7A000000UL
#define PRISM_ID_MASK     0xFFFF0000UL
#define IS_PRISM_ID(id)   (((id) & PRISM_ID_MASK) == PRISM_MONITOR_ID)

struct PrismScreenInfo {
    UBYTE  *vram;            /* 68k address of the screen's framebuffer  */
    ULONG   bytesPerRow;
    UWORD   width, height;
    UBYTE   format;          /* enum PrismFormat (prismboard.h)          */
    UBYTE   depth;           /* bits per pixel the screen was opened at  */
    ULONG   modeID;
};

#define PRISM_TEST_OK      0
#define PRISM_TEST_NOMODE  1     /* the card can't show that size/depth/rate */
#define PRISM_TEST_NOMEM   2     /* no video memory free for the picture     */
#define PRISM_TEST_BUSY    3     /* a test is already showing                */

struct PrismSem {
    struct SignalSemaphore ss;   /* ln_Name = PRISM_SEMNAME              */
    UWORD   version;
    const char *boardName;
    struct Task *task;           /* PrismD itself: Ctrl-C asks it to quit */
    ULONG   numModes;
    /* Fill *info for an open Prism screen; FALSE if it isn't one. Plain C
     * (stack) calling convention: amiga-gcc clients only. */
    BOOL  (*screenInfo)(struct Screen *s, struct PrismScreenInfo *info);
    /* version 2: show a test picture in any mode the card can do, whether
     * or not it is switched on in the prefs (PrismPrefs' Test button).
     * bits = 8, 16 or 24 (the true-colour slot); hz 0 = the card's default;
     * label is drawn along the bottom. Returns PRISM_TEST_*. The picture
     * stays until testEnd(), or until another screen comes to the front. */
    LONG  (*testBegin)(ULONG width, ULONG height, ULONG bits, ULONG hz, const char *label);
    void  (*testEnd)(void);
    /* version 3: PrismD's drawing lock, for debugging tools (lockwatch)
     * that report who holds it. Never obtain it. */
    struct SignalSemaphore *drawLock;
    /* the debug log switch (0 off, 1 on): tools/prismlog flips it on a
     * running PrismD, so a log can cover just the step under test */
    UBYTE *logLevel;
    /* version 4: what the board does, for reports (PrismCheck): its
     * PBF_* flags (prismboard.h; read, never write) and which drawing
     * calls its driver does in hardware (PRISM_HW_*) */
    const ULONG *boardFlags;
    ULONG hwOps;
};
#define PRISM_HW_FILL     (1UL << 0)
#define PRISM_HW_COPY     (1UL << 1)    /* within one bitmap            */
#define PRISM_HW_BETWEEN  (1UL << 2)    /* between two bitmaps in VRAM  */
#define PRISM_HW_TEXT     (1UL << 3)
#define PRISM_HW_LINE     (1UL << 4)
#define PRISM_HW_PLANAR   (1UL << 5)    /* planar sources (icons)       */

#endif
