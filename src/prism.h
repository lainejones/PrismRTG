/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laine Jones */
/*
 * prism.h - what other programs can see of a running Prism.
 *
 * PrismD publishes a named semaphore, "prism", holding this structure.
 * Clients find it with FindSemaphore() under Forbid(). That is the stable
 * way to tell that PrismRTG is running and which release it is (issue
 * #12): the semaphore stays whatever else is added later, so a client
 * checks FindSemaphore(PRISM_SEMNAME) and reads `release` (version 7+) -
 * no prism.library replaces it. PrismD's own cybergraphics.library and
 * Picasso96API.library are compatibility faces, not the identity.
 * `version` counts revisions of this structure; a field exists from the
 * version noted at it on.
 */
#ifndef PRISM_H
#define PRISM_H

#include <exec/semaphores.h>
#include <intuition/screens.h>

#define PRISM_SEMNAME     "prism"
#define PRISM_VERSION     8
/* the release this build belongs to, as the release tag names it without
 * its "v" and "-beta" (v1.1-beta5 = 1.1b5): PrismUpdate compares it with
 * GitHub's */
#ifndef PRISM_RELEASE
#define PRISM_RELEASE     "1.3b2"
#endif
#define PRISM_RELDATE     "27.10.26"      /* the $VER date, dd.mm.yy */

/* Boot failsafe: DEVS:Monitors/Prism creates this before starting PrismD
 * and PrismD deletes it after a minute. Found at boot, it means the last
 * start never got that far: Prism is skipped once. */
#define PRISM_BOOTFLAG    "ENVARC:Prism.booting"

/* Prism ModeIDs: one "monitor" per board, 0x7A00xxxx for the first and
 * 0x7A01xxxx for a second; low bits = depth slot << 8 | size slot
 * (prefs.h), so an ID never changes when other modes are off. */
#define PRISM_MONITOR_ID  0x7A000000UL
#define PRISM_CARD_ID(n)  (PRISM_MONITOR_ID | ((ULONG)(n) << 16))
#define PRISM_ID_CARD(id) (((ULONG)(id) >> 16) & 1)
#define IS_PRISM_ID(id)   (((id) & 0xFFFE0000UL) == PRISM_MONITOR_ID)

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
    /* version 6 (5 was never released): a second board (multi-board.md). numBoards is 1 or 2;
     * the second board's modes are PRISM_CARD_ID(1) | ..., on the monitor
     * "prism2.monitor", and numModes counts both boards' modes. vramKB is
     * each board's video memory. testBeginOn is testBegin on board 0 or
     * 1; testEnd ends whichever is showing. */
    UBYTE numBoards;
    const char *board2Name;
    const ULONG *board2Flags;
    ULONG board2HwOps;
    ULONG vramKB[2];
    LONG  (*testBeginOn)(UBYTE board, ULONG width, ULONG height, ULONG bits, ULONG hz,
                         const char *label);
    /* each board's expansion address (its ConfigDev's cd_BoardAddr): tells
     * two boards of one kind apart */
    ULONG boardAddr[2];
    UBYTE boardsSwapped;         /* PRIMARY=SECOND took effect at start */
    /* version 7: the release this PrismD belongs to, for tools that show it (xSysInfo,
     * issue #12): PRISM_RELEASE ("1.1b7") and the $VER date, dd.mm.yy */
    const char *release;
    const char *releaseDate;
    /* version 8: POWERSAVE, the minutes without input before the boards'
     * monitors go dark (0 = never). PrismPrefs writes it on a running PrismD,
     * which takes it up at once. */
    UBYTE *powerSave;
};
#define PRISM_HW_FILL     (1UL << 0)
#define PRISM_HW_COPY     (1UL << 1)    /* within one bitmap            */
#define PRISM_HW_BETWEEN  (1UL << 2)    /* between two bitmaps in VRAM  */
#define PRISM_HW_TEXT     (1UL << 3)
#define PRISM_HW_LINE     (1UL << 4)
#define PRISM_HW_PLANAR   (1UL << 5)    /* planar sources (icons)       */

#endif
