/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laine Jones
 * Copyright (C) 2026 Stefan Reinauer */
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

#include <exec/types.h>
#include <libraries/configvars.h>

/* Increment when a callback contract or board layout changes. */
#define PRISM_BOARD_ABI 2UL

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
    /* YUV 4:2:2, two pixels in a longword: video sources (Picasso96 PIPs,
     * cgxvideo.library video layers), never a screen's format. Boards never
     * see them; the compositor converts them (composite.h). */
    PF_YUV_CGX,        /* Y0 Cb Y1 Cr, CCIR 601 ranges (Y 16..235): CGX
                          SRCFMT_YCbCr16, Picasso96 RGBFB_YUV422CGX       */
    PF_YUV_YUYV,       /* Y0 U Y1 V, full range: CGX SRCFMT_YUV16        */
    PF_YUV_P96,        /* Y1 U Y0 V (RGBFB_YUV422)                       */
    PF_YUV_P96PC,      /* V Y0 U Y1 (RGBFB_YUV422PC)                     */
    PF_YUV_P96PA,      /* Y0 Y1 V U (RGBFB_YUV422PA)                     */
    PF_YUV_P96PAPC,    /* U V Y1 Y0 (RGBFB_YUV422PAPC)                   */
    PF_COUNT
};
#define PF_BIT(f) (1UL << (f))
#define PF_IS_YUV(f) ((f) >= PF_YUV_CGX && (f) < PF_COUNT)

/* One display mode request. Timings are filled in by the driver's own mode
 * table; Prism only asks for a size, depth and refresh. */
struct PrismMode {
    UWORD width, height;     /* visible pixels                          */
    UBYTE format;            /* enum PrismFormat                        */
    UBYTE refresh;           /* Hz, 0 = driver's default                */
    ULONG bytesPerRow;       /* set by the driver in SetMode            */
    UWORD pitchPixels;       /* 0, or the pixels a row of the displayed
                                bitmap has: an oversized (autoscroll)
                                screen is wider than the mode, and the
                                display must step rows by its pitch. A
                                driver that cannot refuses in checkMode */
};

/* Board flags (bits 0-2 and 4 are free; what a board can draw is
 * whatever its PrismOps provide) */
#define PBF_HW_CURSOR     (1UL << 3)  /* cursorImage/Show/Move work      */
#define PBF_BLIT_32       (1UL << 5)  /* fill + expand take 4-byte pixels */
#define PBF_SHADOW        (1UL << 6)  /* CPU access goes through read/write */
#define PBF_ACCEL_BROKEN  (1UL << 7)  /* an operation failed: CPU from now on */
#define PBF_SOFTWARE      (1UL << 8)  /* blitter off in PrismPrefs        */
#define PBF_BANKED        (1UL << 9)  /* CalculateMemory may remap every byte */
#define PBF_PRESENT       (1UL << 10) /* device holds composed, not source pixels */
#define PBF_REINIT        (1UL << 11) /* release shadow VRAM before setMode */

struct PrismOps;
struct PrismOverlay;                /* defined after the board: its overlay hooks take it */
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

    /* Mandatory: setMode and setDisplayStart (with name, vram, vramSize,
     * formats, maxWidth and maxHeight above). The five after them get a
     * default from board_defaults() when left NULL: checkMode accepts
     * any size up to maxWidth x maxHeight in one of `formats` with
     * unpadded rows; the others do nothing. src/drv_template.c is the
     * worked example, docs/writing-a-driver.md the guide. */
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
    UWORD cursorW, cursorH;          /* the sprite's size (0: the full 64x64);
                                        a bigger pointer image is drawn by
                                        PrismD's software pointer instead */

    /* Optional bitmap row layout; NULL keeps the default 8-byte alignment.
     * (bytes per pixel only: drivers whose row layout depends on the exact
     * format provide PrismOps.pitch instead). */
    ULONG (*bytesPerRow)(struct PrismBoard *b, UWORD w, UWORD h, UBYTE bpp);

    const struct PrismOps *ops; /* surface-aware operations; prefer to legacy hooks */
    void (*modeReady)(struct PrismBoard *); /* optional first-mode setup */
    ULONG faults;              /* operations that failed after starting */
    /* Largest blit the direct fillRect/copyRect hooks can encode: bytes
     * per row, rows and pitch (0 = no limit). Larger ones go through ops. */
    UWORD blitMaxBytes, blitMaxRows;
    ULONG blitMaxPitch;

    /* Hardware video overlay (optional): a packed YUV 4:2:2 bitmap in VRAM
     * shown over the screen at scan-out, scaled into the destination
     * rectangle, and only where the screen holds the colour key. The
     * screen is 15, 16 or 32-bit. overlaySet returns FALSE when the board
     * refuses (PrismD then composes the layer on the CPU); overlayOff
     * takes it down. One overlay per board. */
    BOOL (*overlaySet)(struct PrismBoard *b, const struct PrismOverlay *o);
    void (*overlayOff)(struct PrismBoard *b);

    /* Monitor power (optional, for POWERSAVE): 0 = on, 1 = blank (picture
     * off, syncs running), 2 = standby (HSYNC off), 3 = suspend (VSYNC
     * off), 4 = off (both syncs off). A board that can only blank treats
     * 2-4 as 1. NULL: PrismD blanks by itself (a black palette, or a black
     * bitmap in VRAM on display). Called with the lock held, no blit running. */
    void (*setPower)(struct PrismBoard *b, UBYTE level);
};

struct PrismOverlay {
    ULONG srcOff;               /* VRAM offset of the source bitmap        */
    UWORD srcPitch;             /* its bytes per row                        */
    UWORD srcW, srcH;           /* pixels (an odd width still holds whole
                                   four-byte pairs in the row)              */
    UBYTE srcFormat;            /* PF_YUV_CGX, _P96, _P96PC, _P96PA, _P96PAPC */
    UBYTE keyed;                /* 1: only where the screen shows `key`     */
    UBYTE screenBpp;            /* 2 or 4                                   */
    UBYTE key[4];               /* the key pixel as it lies in VRAM         */
    WORD  dstX, dstY, dstW, dstH; /* on the screen; may reach past its edges */
};

#define CURSOR_SIZE 64

/* What every loadable driver supplies (driver_module.c calls them; the
 * request's config carries the PrismPrefs settings the adapters use):
 * probe the board and fill *b, and park if something cannot be released. */
struct PrismDriverConfig;
BOOL driver_probe(struct PrismBoard *b, const struct PrismDriverConfig *config);
void driver_retain(void);
/* A driver's probe takes only a board PrismD does not drive yet: it looks
 * for one with driver_find - the next ConfigDev of mfr/prod after `after`
 * (NULL: from the start) that is configured (not CDF_SHUTUP) and not in
 * the config's `taken` list - so two copies of one driver drive two
 * boards of a kind (multi-board.md). driver_taken says whether a given
 * node is in that list. */
struct ConfigDev *driver_find(struct ConfigDev *after, ULONG mfr, ULONG prod);
BOOL driver_taken(const struct ConfigDev *cd);
/* Fill the optional callbacks a driver left NULL (boardops.c). PrismD and
 * PrismTest call it on the board a driver returned. */
void board_defaults(struct PrismBoard *b);

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
