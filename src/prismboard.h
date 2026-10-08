/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laine Jones */
/*
 * prismboard.h - the interface between Prism and a board driver.
 *
 * A driver is deliberately dumb: it knows its own hardware and nothing about
 * screens, bitmaps or the OS display database. Everything a driver does not
 * accelerate, prism.library does on the CPU.
 *
 * PrismD loads native drivers and the P96 adapter as separate modules.
 * The adapter loads external P96 card libraries behind the same interface.
 */
#ifndef PRISMBOARD_H
#define PRISMBOARD_H
#define PRISM_BOARD_HAS_OPS 1

#include <exec/types.h>
#include <libraries/configvars.h>

/* Increment when a callback contract or board layout changes. */
#define PRISM_BOARD_ABI 1UL

/* Pixel formats, as the 68k sees them in VRAM (byte order in memory). */
enum PrismFormat {
    PF_CLUT8 = 0,      /* 1 byte, palette index                      */
    PF_RGB565BE,       /* 2 bytes, RRRRRGGG GGGBBBBB                  */
    PF_RGB565LE,       /* 2 bytes, GGGBBBBB RRRRRGGG (PC order)       */
    PF_RGB555BE,       /* 2 bytes, xRRRRRGG GGGBBBBB                  */
    PF_RGB555LE,
    PF_RGB24,          /* 3 bytes R,G,B                               */
    PF_BGR24,          /* 3 bytes B,G,R                               */
    PF_ARGB32,         /* 4 bytes A,R,G,B                             */
    PF_BGRA32,         /* 4 bytes B,G,R,A                             */
    PF_RGBA32,         /* 4 bytes R,G,B,A                              */
    PF_BGR565LE,       /* 2 bytes, GGGRRRRR BBBBBGGG (PC order, red and
                          blue exchanged: Piccolo, Spectrum)          */
    PF_BGR555LE,
    PF_COUNT
};
#define PF_BIT(f) (1UL << (f))

/* One display mode request. Timings are filled in by the driver's own mode
 * table; Prism only asks for a size, depth and refresh. */
struct PrismMode {
    UWORD width, height;     /* visible pixels                          */
    UBYTE format;            /* enum PrismFormat                        */
    UBYTE refresh;           /* Hz, 0 = driver's default                */
    ULONG bytesPerRow;       /* set by the driver in SetMode            */
};

/* Capability flags */
#define PBF_BLIT_FILL     (1UL << 0)  /* FillRect                        */
#define PBF_BLIT_COPY     (1UL << 1)  /* CopyRect (screen to screen)     */
#define PBF_BLIT_EXPAND   (1UL << 2)  /* 1-bit template -> colour        */
#define PBF_HW_CURSOR     (1UL << 3)
#define PBF_VBLANK_IRQ    (1UL << 4)
#define PBF_BLIT_32       (1UL << 5)  /* fill + expand take 4-byte pixels */

struct PrismOps;
struct PrismBoard {
    const char       *name;          /* "ZZ9000", "Picasso II"           */
    struct ConfigDev *configDev;     /* the board's autoconfig node      */
    volatile UBYTE   *regs;          /* register window                  */
    UBYTE            *vram;          /* 68k address of VRAM              */
    ULONG             vramSize;      /* usable bytes                     */
    ULONG             formats;       /* PF_BIT() set the board can show  */
    ULONG             flags;         /* PBF_*                            */
    UWORD             maxWidth, maxHeight;
    void             *priv;          /* driver-private state             */

    /* Mandatory */
    BOOL (*setMode)(struct PrismBoard *b, struct PrismMode *m);
    BOOL (*checkMode)(struct PrismBoard *b, struct PrismMode *m); /* dry run:
                                         validates and fills bytesPerRow */
    void (*setDisplayStart)(struct PrismBoard *b, ULONG vramOffset);
    void (*setPalette)(struct PrismBoard *b, UWORD first, UWORD count,
                       const UBYTE *rgb);           /* 8-bit R,G,B triples */
    void (*setSwitch)(struct PrismBoard *b, BOOL rtg); /* monitor: RTG or native */
    void (*waitVBlank)(struct PrismBoard *b);
    void (*shutdown)(struct PrismBoard *b);         /* back to power-on state */

    /* Optional: snapshot and restore whatever mode another RTG system left
     * on the card (M0 test tools borrow the board from Picasso96). */
    void (*saveState)(struct PrismBoard *b);
    void (*restoreState)(struct PrismBoard *b);

    /* Optional (NULL = Prism does it on the CPU). Offsets are VRAM offsets,
     * pitch in bytes, colour already in the current format. */
    /* Every blitter call returns with the blit finished (waitBlit is for
     * code that touches VRAM without having just called one). */
    /* bpp = bytes per pixel of the bitmap drawn into, which need not be
     * the mode on display; colour is the pixel as the 68k stores it */
    void (*fillRect)(struct PrismBoard *b, ULONG dst, ULONG pitch, UBYTE bpp,
                     UWORD x, UWORD y, UWORD w, UWORD h, ULONG colour);
    void (*copyRect)(struct PrismBoard *b, ULONG base, ULONG pitch, UBYTE bpp,
                     UWORD sx, UWORD sy, UWORD dx, UWORD dy, UWORD w, UWORD h);
    /* copy between two VRAM areas with their own pitches (byte geometry,
     * the areas don't overlap) */
    void (*copyBetween)(struct PrismBoard *b, ULONG src, ULONG spitch, ULONG dst,
                        ULONG dpitch, UWORD wbytes, UWORD h);
    /* colour-expand a 1-bit template (text): rows of (w+7)/8 bytes, mod
     * bytes apart, bit 7 of a row's first byte = pixel x. Set bits get fg,
     * clear bits bg - or stay as they were when transparent. FALSE = not
     * done (too big, or the board refused): draw it on the CPU. */
    BOOL (*expandRect)(struct PrismBoard *b, ULONG dst, ULONG pitch, UBYTE bpp,
                       UWORD x, UWORD y, UWORD w, UWORD h, const UBYTE *tmpl, ULONG mod,
                       ULONG fg, ULONG bg, BOOL transparent);
    void (*waitBlit)(struct PrismBoard *b);
    /* A solid line from (x,y), dx/dy the signed deltas to its last pixel,
     * all of it inside the bitmap. The pixels must be Prism's own (see
     * line_solid in render.c): one step along the longer axis per pixel,
     * and after i of them the line has moved round(i * S / L) along the
     * shorter one, halves rounded up (L, S = longer and shorter delta). */
    void (*drawLine)(struct PrismBoard *b, ULONG dst, ULONG pitch, UBYTE bpp,
                     WORD x, WORD y, WORD dx, WORD dy, ULONG colour);

    /* Hardware cursor (PBF_HW_CURSOR). The image is 64x64 bytes, one per
     * pixel: 0 = transparent, 1..3 = colour 1..3 (Amiga sprite pens 17-19);
     * rgb holds those three colours as 8-bit R,G,B. Prism does hot spots
     * and clipping at the top/left edge, so x and y are never negative. */
    void (*cursorImage)(struct PrismBoard *b, const UBYTE *img, const UBYTE *rgb);
    void (*cursorShow)(struct PrismBoard *b, BOOL on);
    void (*cursorMove)(struct PrismBoard *b, WORD x, WORD y);

    /* Optional bitmap row layout; NULL keeps the default 8-byte alignment.
     * Drivers using this offer one compatible format per byte depth. */
    ULONG (*bytesPerRow)(struct PrismBoard *b, UWORD w, UWORD h, UBYTE bpp);

    /* Optional Cirrus text-expansion diagnostic after the first mode set. */
    UBYTE (*textExpand)(struct PrismBoard *b, BOOL *transparent);
    const struct PrismOps *ops; /* surface-aware operations; prefer to legacy hooks */
    void (*modeReady)(struct PrismBoard *); /* optional first-mode setup */
    ULONG faults;              /* operations that failed after starting */
};

#define CURSOR_SIZE 64

/* Driver probes: return TRUE and fill *b if the board is present. */
BOOL Picasso2_Probe(struct PrismBoard *b);
/* after the first mode set: text expansion's row padding (0xff = off) */
UBYTE Picasso2_TextExpand(struct PrismBoard *b, BOOL *transparent);
BOOL ZZ9000_Probe(struct PrismBoard *b);
BOOL P96_Probe(struct PrismBoard *b, const char *card, const char *monitor);
/* After a P96 card was claimed, keep its callbacks and interrupt data alive.
 * Returns only if no card was claimed during this invocation. */
void P96_KeepResident(void);
BOOL UAEGFX_Probe(struct PrismBoard *b);
void UAEGFX_KeepResident(void);
extern UBYTE Picasso2_ClutBGR;      /* PALETTE=BGR in the prefs (drv_picasso2.c) */

#endif
