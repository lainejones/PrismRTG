/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laine Jones */
/*
 * drv_saga.c - the Apollo Vampire V4's SAGA video, driven natively.
 *
 * TEST DRIVER, not yet run on a Vampire: written from the register
 * descriptions in the Apollo team's SAGA.card source (flype44/Vampire,
 * VIDEO/sagacard, MPL-2.0 - facts only, no code taken over). Selected
 * with BOARD=SAGA; Auto does not pick it yet.
 *
 * The hardware, as that source describes it:
 *   - no VRAM of its own: the frame buffer is ordinary fast RAM, so the
 *     "VRAM" here is a block of it and every drawing is the CPU's
 *   - a fixed table of display sizes; the size's number and the pixel
 *     format go into one register (FORMAT, $DFF1F4: size << 8 | format)
 *   - the display start ($DFF1EC, 16-byte aligned) and a row modulo
 *     ($DFF1E6, bytes skipped after each row)
 *   - a 256-entry palette written one entry at a time ($DFF388)
 *   - a 16x16 three-colour hardware sprite ($DFF1D0 position, $DFF3A2
 *     colours, $DFF800 image)
 *   - no vertical blank interrupt of its own (the chipset's is used)
 *
 * Built with -DSAGA_FAKE (tools/mkdriver.sh SAGAFAKE ...), the registers
 * are an array in RAM and no board is needed: PrismD runs on it in an
 * emulator, which checks everything but the picture.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <libraries/configvars.h>
#include <proto/exec.h>
#include <proto/expansion.h>
#include <proto/graphics.h>
#include <stdio.h>
#include <string.h>
#include "boardops.h"
#ifdef PRISM_DRIVER_MODULE
#include "driver_module.h"
#endif

#define SAGA_MFR    0x1398      /* Apollo team                         */
#define SAGA_PROD   5           /* Vampire V4 (Standalone, V4+)         */

/* registers, offsets from $DFF000 */
#define R_SPRITE_X  0x1D0       /* W: sprite left + 16                  */
#define R_SPRITE_Y  0x1D2       /* W: sprite top + 8                    */
#define R_MODULO    0x1E6       /* W: bytes after each displayed row    */
#define R_BPLPTR    0x1EC       /* L: display start, 16-byte aligned    */
#define R_FORMAT    0x1F4       /* W: size << 8 | pixel format          */
#define R_CLUT      0x388       /* L: index << 24 | R << 16 | G << 8 | B */
#define R_SPRCLUT   0x3A2       /* W x3: sprite colours 1-3, 0RGB       */
#define R_SPRDATA   0x800       /* L x16: rows, plane 0 << 16 | plane 1 */

#define FMT_OFF     0           /* SAGA pixel formats                   */
#define FMT_CLUT8   1
#define FMT_RGB16   2           /* R5G6B5, big-endian                   */
#define FMT_RGB24   4           /* R,G,B bytes                          */
#define FMT_RGB32   5           /* A,R,G,B bytes                        */

#define SPRITE_DX   16
#define SPRITE_DY   8
#define SPRITE_OFF  0x3FFF      /* a position outside every mode        */

/* the display sizes the hardware knows, by number, and their refresh
 * (the 256- and 576-line sizes are PAL timings) */
static const struct { UBYTE id; UWORD w, h; UBYTE hz; } sizes[] = {
    { 0x01,  320, 200, 60 }, { 0x02,  320, 240, 60 }, { 0x03,  320, 256, 50 },
    { 0x04,  640, 400, 60 }, { 0x05,  640, 480, 60 }, { 0x06,  640, 512, 50 },
    { 0x07,  960, 540, 60 }, { 0x08,  480, 270, 60 }, { 0x09,  304, 224, 60 },
    { 0x0A, 1280, 720, 60 }, { 0x0B,  640, 360, 60 }, { 0x0C,  800, 600, 60 },
    { 0x0D, 1024, 768, 60 }, { 0x0E,  720, 576, 50 }, { 0x0F,  848, 480, 60 },
};
#define NSIZES (sizeof(sizes) / sizeof(sizes[0]))

struct SagaPriv {
    volatile UBYTE *regs;       /* $DFF000 (or the fake array)          */
    UBYTE *mem;                 /* the allocation; vram is it aligned   */
    ULONG memSize;
    UWORD format;               /* the FORMAT value of the mode shown   */
    UWORD width;                /* the mode's width and bytes per pixel */
    UBYTE bpp;
    UBYTE spriteOn;
    UBYTE down;                 /* shut down: the display reads no memory */
    WORD sx, sy;                /* sprite position as written           */
    ULONG start;                /* VRAM offset on display               */
};
static struct SagaPriv priv;

#ifdef SAGA_FAKE
static UBYTE fakeRegs[0x1000];
#endif

#define W16(p, off, v) (*(volatile UWORD *)((p)->regs + (off)) = (UWORD)(v))
#define W32(p, off, v) (*(volatile ULONG *)((p)->regs + (off)) = (ULONG)(v))

static int size_index(UWORD w, UWORD h)
{
    UWORD i;
    for (i = 0; i < NSIZES; i++)
        if (sizes[i].w == w && sizes[i].h == h)
            return i;
    return -1;
}

static UBYTE saga_format(UBYTE pf)
{
    switch (pf) {
    case PF_CLUT8:    return FMT_CLUT8;
    case PF_RGB565BE: return FMT_RGB16;
    case PF_RGB24:    return FMT_RGB24;
    case PF_ARGB32:   return FMT_RGB32;
    }
    return FMT_OFF;
}

/* rows on 8-byte boundaries, as PrismD lays bitmaps out anyway; the
 * modulo skips the padding (none for the sizes in the table) */
static ULONG saga_bpr(struct PrismBoard *b, UWORD w, UWORD h, UBYTE bpp)
{
    (void)b; (void)h;
    return ((ULONG)w * bpp + 7) & ~7UL;
}

static BOOL saga_CheckMode(struct PrismBoard *b, struct PrismMode *m)
{
    int i;
    UBYTE bpp = board_bpp(m->format);
    if (!bpp || !(b->formats & PF_BIT(m->format)) || saga_format(m->format) == FMT_OFF)
        return FALSE;
    if ((i = size_index(m->width, m->height)) < 0)
        return FALSE;
    /* no oversized (autoscroll) screens yet: the display start would have
     * to move in steps the 16-byte alignment can't follow */
    if (m->pitchPixels && m->pitchPixels != m->width)
        return FALSE;
    m->refresh = sizes[i].hz;           /* the hardware's timing, whatever was asked */
    m->bytesPerRow = saga_bpr(b, m->width, m->height, bpp);
    return m->bytesPerRow * m->height <= b->vramSize;
}

static void saga_SetDisplayStart(struct PrismBoard *b, ULONG off)
{
    struct SagaPriv *p = b->priv;
    p->start = off;
    if (p->format != FMT_OFF)
        W32(p, R_BPLPTR, ((ULONG)b->vram + off) & ~15UL);
}

static BOOL saga_SetMode(struct PrismBoard *b, struct PrismMode *m)
{
    struct SagaPriv *p = b->priv;
    int i;
    if (!saga_CheckMode(b, m))
        return FALSE;
    i = size_index(m->width, m->height);
    p->width = m->width;
    p->bpp = board_bpp(m->format);
    p->format = ((UWORD)sizes[i].id << 8) | saga_format(m->format);
    W16(p, R_MODULO, m->bytesPerRow - (ULONG)m->width * p->bpp);
    W32(p, R_BPLPTR, ((ULONG)b->vram + p->start) & ~15UL);
    W16(p, R_FORMAT, p->format);
    p->down = FALSE;
    printf("PrismD: SAGA %ux%u, %u bytes per pixel: FORMAT $%04x, start $%08lx\n",
           m->width, m->height, p->bpp, p->format, (unsigned long)(((ULONG)b->vram + p->start) & ~15UL));
    return TRUE;
}

static void saga_SetPalette(struct PrismBoard *b, UWORD first, UWORD count, const UBYTE *rgb)
{
    struct SagaPriv *p = b->priv;
    UWORD i;
    for (i = 0; i < count && first + i < 256; i++, rgb += 3)
        W32(p, R_CLUT, ((ULONG)(first + i) << 24) | ((ULONG)rgb[0] << 16) |
                       ((ULONG)rgb[1] << 8) | rgb[2]);
}

/* RTG picture on, or off: the SAGA source turns its display off with
 * format 0, which leaves the chipset's own picture */
static void saga_SetSwitch(struct PrismBoard *b, BOOL rtg)
{
    struct SagaPriv *p = b->priv;
    W16(p, R_FORMAT, rtg ? p->format : FMT_OFF);
    p->down = !rtg;
}

static void saga_WaitVBlank(struct PrismBoard *b)
{
    (void)b;
    WaitTOF();                          /* SAGA has no vertical blank interrupt */
}

static void saga_Shutdown(struct PrismBoard *b)
{
    struct SagaPriv *p = b->priv;
    W16(p, R_SPRITE_X, SPRITE_OFF);
    W16(p, R_SPRITE_Y, SPRITE_OFF);
    W16(p, R_FORMAT, FMT_OFF);
    p->down = TRUE;
}

/* ---- the sprite: 16x16, two planes, three colours ---------------------- */
static void saga_CursorImage(struct PrismBoard *b, const UBYTE *img, const UBYTE *rgb)
{
    struct SagaPriv *p = b->priv;
    UWORD x, y;
    for (y = 0; y < 16; y++) {
        UWORD p0 = 0, p1 = 0;
        for (x = 0; x < 16; x++) {
            UBYTE pen = img[y * CURSOR_SIZE + x];
            if (pen & 1) p0 |= 0x8000 >> x;
            if (pen & 2) p1 |= 0x8000 >> x;
        }
        W32(p, R_SPRDATA + 4 * y, ((ULONG)p0 << 16) | p1);
    }
    for (x = 0; x < 3; x++, rgb += 3)
        W16(p, R_SPRCLUT + 2 * x, ((rgb[0] & 0xf0) << 4) | (rgb[1] & 0xf0) | (rgb[2] >> 4));
}

static void saga_CursorShow(struct PrismBoard *b, BOOL on)
{
    struct SagaPriv *p = b->priv;
    p->spriteOn = on;
    W16(p, R_SPRITE_X, on ? p->sx : SPRITE_OFF);
    W16(p, R_SPRITE_Y, on ? p->sy : SPRITE_OFF);
}

static void saga_CursorMove(struct PrismBoard *b, WORD x, WORD y)
{
    struct SagaPriv *p = b->priv;
    p->sx = x + SPRITE_DX;
    p->sy = y + SPRITE_DY;
    if (p->spriteOn) {
        W16(p, R_SPRITE_X, p->sx);
        W16(p, R_SPRITE_Y, p->sy);
    }
}

/* ---- the probe --------------------------------------------------------- */
static BOOL saga_probe(struct PrismBoard *b)
{
    static const ULONG tryMB[] = { 16, 8, 4 };
    struct SagaPriv *p = &priv;
    UWORD i;
#ifdef SAGA_FAKE
    p->regs = fakeRegs;
    b->configDev = NULL;
    b->name = "SAGA (fake)";
#else
    struct ConfigDev *cd = driver_find(NULL, SAGA_MFR, SAGA_PROD);
    if (!cd)
        return FALSE;
    p->regs = (volatile UBYTE *)0xDFF000;
    b->configDev = cd;
    b->name = "SAGA";
#endif
    /* the frame buffer: fast RAM, as much as can be had up to 16 MB
     * (more bitmaps stay "on the card"), aligned so every 256-byte VRAM
     * block PrismD hands out starts on a 16-byte boundary */
    for (i = 0; i < sizeof(tryMB) / sizeof(tryMB[0]) && !p->mem; i++) {
        p->memSize = tryMB[i] << 20;
        p->mem = AllocMem(p->memSize + 255, MEMF_FAST | MEMF_PUBLIC);
    }
    if (!p->mem) {
        puts("PrismD: SAGA found, but no fast RAM for its frame buffer");
        return FALSE;
    }
    b->regs      = p->regs;
    b->vram      = (UBYTE *)(((ULONG)p->mem + 255) & ~255UL);
    b->vramSize  = p->memSize;
    b->formats   = PF_BIT(PF_CLUT8) | PF_BIT(PF_RGB565BE) | PF_BIT(PF_RGB24) | PF_BIT(PF_ARGB32);
    b->flags     = PBF_HW_CURSOR;
    b->maxWidth  = 1280;
    b->maxHeight = 768;
    b->priv      = p;
    b->cursorW   = 16;
    b->cursorH   = 16;

    b->setMode         = saga_SetMode;
    b->checkMode       = saga_CheckMode;
    b->bytesPerRow     = saga_bpr;
    b->setDisplayStart = saga_SetDisplayStart;
    b->setPalette      = saga_SetPalette;
    b->setSwitch       = saga_SetSwitch;
    b->waitVBlank      = saga_WaitVBlank;
    b->shutdown        = saga_Shutdown;
    b->cursorImage     = saga_CursorImage;
    b->cursorShow      = saga_CursorShow;
    b->cursorMove      = saga_CursorMove;
    W16(p, R_SPRITE_X, SPRITE_OFF);
    W16(p, R_SPRITE_Y, SPRITE_OFF);
    printf("PrismD: SAGA frame buffer %lu MB at $%08lx (test driver)\n",
           (unsigned long)(p->memSize >> 20), (unsigned long)b->vram);
    return TRUE;
}

#ifdef PRISM_DRIVER_MODULE
BOOL driver_probe(struct PrismBoard *b, const struct PrismDriverConfig *config)
{
    (void)config;
    return saga_probe(b);
}

/* The frame buffer goes back only once shutdown has turned the display
 * off (PrismTest, a PrismD that quit): otherwise the display may still be
 * reading it, and it stays allocated. */
void driver_retain(void)
{
    struct SagaPriv *p = &priv;
    if (p->mem && p->down) {
        FreeMem(p->mem, p->memSize + 255);
        p->mem = NULL;
    }
}
#endif
