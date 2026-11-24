/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laine Jones */
/*
 * drv_minimig.c - the RTG frame buffer of the MiSTer Minimig-AGA core.
 *
 * Written from docs/minimig-hw.md: the core's own source
 * (MiSTer-devel/Minimig-AGA_MiSTer, rtl/rtg.v and sys/ascal.vhd) and the
 * register use of its MiSTer.card P96 driver - facts only, no code taken
 * over. BOARD=MINIMIG; Auto finds it too once MINIMIG.driver is in
 * LIBS:Prism.
 *
 * The hardware:
 *   - 8 MB of the DE10-Nano's DDR as the frame buffer, shown by the MiSTer
 *     scaler instead of the Amiga picture, scaled to the HDMI mode set in
 *     MiSTer.ini: a size, a format, a row length and a start address are
 *     all there is to a mode (no timing, no pixel clock)
 *   - a Zorro III board (0x139C/0x30) with the frame buffer in its low
 *     8 MB and the registers at +$800100; older cores have them at fixed
 *     addresses ($02000000, $B80100)
 *   - a 256-entry palette, 8/15/16/24/32-bit pixels in PC byte order or
 *     with red and blue exchanged
 *   - no blitter, no sprite, no vertical blank status: the start address
 *     is taken at the scaler's own vertical sync, so it never tears
 *
 * Built with -DMINIMIG_FAKE (tools/mkdriver.sh MINIMIGFAKE ...), the
 * registers are an array and 4 MB of fast RAM is the frame buffer: PrismD
 * runs on it in an emulator, which checks everything but the picture.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/execbase.h>
#include <libraries/configvars.h>
#include <proto/exec.h>
#include <proto/expansion.h>
#include <stdio.h>
#include <string.h>
#include "boardops.h"
#ifdef PRISM_DRIVER_MODULE
#include "driver_module.h"
#endif
#ifndef MINIMIG_FAKE
#include "minimig_mmu.h"
#endif

#define MM_MFR          0x139C
#define MM_PROD_RTG     0x30
#define MM_REGS_OFF     0x800100UL  /* registers, from the board's base   */
#define MM_VRAM_SIZE    0x800000UL  /* the low 8 MB of the board          */
#define MM_LEGACY_VRAM  0x02000000UL /* older cores' fixed addresses      */
#define MM_LEGACY_REGS  0x00B80100UL
#define MM_FB_PHYS      0x27000000UL /* the frame buffer in the scaler's DDR map */

/* registers (16-bit), offsets from the register base */
#define R_ADDR_HI   0x00
#define R_ADDR_LO   0x02
#define R_FORMAT    0x04
#define R_ENABLE    0x06
#define R_HSIZE     0x08
#define R_VSIZE     0x0A
#define R_STRIDE    0x0C
#define R_ID        0x0E
#define R_CLUT      0x300       /* 256 longwords $00RRGGBB              */
#define MM_ID       0x5001

/* FORMAT: bits 2:0 depth, bit 3 1-5-5-5, bit 4 red and blue exchanged */
#define FMT_8       0x03
#define FMT_16      0x04
#define FMT_24      0x05
#define FMT_32      0x06
#define FMT_555     0x08
#define FMT_SWAPRB  0x10

#define MM_MAX_W    2048        /* what the P96 driver offers; the scaler holds 2304 */
#define MM_MAX_H    2048
#define MM_MAX_STRIDE 16383     /* STRIDE is 14 bits                    */
#define MM_ALIGN    16          /* the scaler's word: rows and starts on it */

struct MinimigPriv {
    volatile UBYTE *regs;
    UWORD format;               /* FORMAT of the mode on display, 0 = none */
    UWORD addrHi;               /* the start address's high word as written */
    UBYTE down;                 /* shut down: the Amiga picture is shown */
#ifdef MINIMIG_FAKE
    UBYTE *fakeVram;
#endif
};
static struct MinimigPriv priv;

#ifdef MINIMIG_FAKE
#ifndef MM_FAKE_SIZE
#define MM_FAKE_SIZE (4UL << 20)
#endif
static UWORD fakeRegs[0x400];
#endif

#define W16(p, off, v) (*(volatile UWORD *)((p)->regs + (off)) = (UWORD)(v))
#define R16(p, off)    (*(volatile UWORD *)((p)->regs + (off)))
#define W32(p, off, v) (*(volatile ULONG *)((p)->regs + (off)) = (ULONG)(v))

/* the FORMAT value for a PrismRTG pixel format; 0 = not shown (docs/minimig-hw.md 3) */
static UWORD mm_format(UBYTE pf)
{
    switch (pf) {
    case PF_CLUT8:    return FMT_8;
    case PF_RGB565LE: return FMT_16 | FMT_SWAPRB;
    case PF_BGR565LE: return FMT_16;
    case PF_RGB555LE: return FMT_16 | FMT_555 | FMT_SWAPRB;
    case PF_BGR555LE: return FMT_16 | FMT_555;
    case PF_RGB24:    return FMT_24;
    case PF_BGR24:    return FMT_24 | FMT_SWAPRB;
    case PF_RGBA32:   return FMT_32;
    case PF_BGRA32:   return FMT_32 | FMT_SWAPRB;
    }
    return 0;
}

/* rows on the scaler's 16-byte word, so that every row of a bitmap on
 * display starts on one (a start off it can lose a row's last pixels) */
static ULONG mm_bpr(struct PrismBoard *b, UWORD w, UWORD h, UBYTE bpp)
{
    (void)b; (void)h;
    return ((ULONG)w * bpp + MM_ALIGN - 1) & ~(ULONG)(MM_ALIGN - 1);
}

/* Any size up to the limits: the scaler takes it as it is and scales it
 * to the HDMI mode, whose refresh is MiSTer.ini's. */
static BOOL mm_CheckMode(struct PrismBoard *b, struct PrismMode *m)
{
    UBYTE bpp = board_bpp(m->format);
    if (!bpp || !(b->formats & PF_BIT(m->format)) || !mm_format(m->format))
        return FALSE;
    if (m->width < 320 || m->height < 200 || m->width > b->maxWidth || m->height > b->maxHeight)
        return FALSE;
    if (m->refresh)
        return FALSE;
    /* no oversized (autoscroll) screens: a sideways pan would move the
     * start off the 16-byte word */
    if (m->pitchPixels && m->pitchPixels != m->width)
        return FALSE;
    m->bytesPerRow = mm_bpr(b, m->width, m->height, bpp);
    if (m->bytesPerRow > MM_MAX_STRIDE)
        return FALSE;
    return m->bytesPerRow * m->height <= b->vramSize;
}

/* The scaler copies the address at its vertical sync. A start whose
 * high word stays the same is one write, so it is never seen half done. */
static void mm_SetDisplayStart(struct PrismBoard *b, ULONG off)
{
    struct MinimigPriv *p = b->priv;
    ULONG a = MM_FB_PHYS + (off & ~(ULONG)(MM_ALIGN - 1));
    if ((UWORD)(a >> 16) != p->addrHi) {
        p->addrHi = (UWORD)(a >> 16);
        W16(p, R_ADDR_HI, p->addrHi);
    }
    W16(p, R_ADDR_LO, (UWORD)a);
}

static BOOL mm_SetMode(struct PrismBoard *b, struct PrismMode *m)
{
    struct MinimigPriv *p = b->priv;
    if (!mm_CheckMode(b, m))
        return FALSE;
    p->format = mm_format(m->format);
    W16(p, R_FORMAT, p->format);
    W16(p, R_HSIZE, m->width);
    W16(p, R_VSIZE, m->height);
    W16(p, R_STRIDE, m->bytesPerRow);
    if (!p->down)
        W16(p, R_ENABLE, 1);
    printf("PrismD: Minimig RTG %ux%u, FORMAT $%02x, %lu bytes per row\n",
           m->width, m->height, p->format, (unsigned long)m->bytesPerRow);
    return TRUE;
}

/* the high word first: its low byte (red) is held until the low word */
static void mm_SetPalette(struct PrismBoard *b, UWORD first, UWORD count, const UBYTE *rgb)
{
    struct MinimigPriv *p = b->priv;
    UWORD i;
    for (i = 0; i < count && first + i < 256; i++, rgb += 3)
        W32(p, R_CLUT + 4 * (first + i),
            ((ULONG)rgb[0] << 16) | ((ULONG)rgb[1] << 8) | rgb[2]);
}

/* ENABLE: the scaler shows the frame buffer, or the Amiga's own picture */
static void mm_SetSwitch(struct PrismBoard *b, BOOL rtg)
{
    struct MinimigPriv *p = b->priv;
    p->down = !rtg;
    W16(p, R_ENABLE, rtg && p->format ? 1 : 0);
}

static void mm_Shutdown(struct PrismBoard *b)
{
    struct MinimigPriv *p = b->priv;
    W16(p, R_ENABLE, 0);
    p->down = TRUE;
}

/* ---- the probe --------------------------------------------------------- */
#ifndef MINIMIG_FAKE
/* Older cores (every release before November 2026): fixed addresses, no
 * autoconfig node. $B80100 is read only on a machine that is a Minimig -
 * one of the core's own boards is there: its Zorro III fast RAM (0x139C),
 * its Zorro II fast RAM (2011/$51) or its Toccata (18260/12, absent only
 * in CDTV mode) - with a 68020 or better, as the P96 driver does it, and
 * must say $5001. A 68EC020 sees the registers too but not $02000000,
 * which it takes for chip RAM at 0: the exception vectors showing up there
 * say so. Under mmu.library (a 68060's) both windows are blank until
 * mm_mmu_map() maps them. */
static BOOL mm_legacy(void)
{
    const volatile ULONG *fb = (const volatile ULONG *)MM_LEGACY_VRAM;
    const volatile ULONG *chip = (const volatile ULONG *)0;
    UWORD i;
    if (!(SysBase->AttnFlags & AFF_68020))
        return FALSE;
    /* a core with the Zorro board (taken already, if driver_find passed
     * it by) has no fixed addresses to fall back on */
    if (FindConfigDev(NULL, MM_MFR, MM_PROD_RTG))
        return FALSE;
    if (!FindConfigDev(NULL, MM_MFR, (ULONG)-1) && !FindConfigDev(NULL, 2011, 0x51) &&
        !FindConfigDev(NULL, 18260, 12))
        return FALSE;
    mm_mmu_map(NULL);
    if (*(volatile UWORD *)(MM_LEGACY_REGS + R_ID) != MM_ID)
        return FALSE;
    for (i = 2; i < 64; i++)
        if (fb[i] != chip[i])
            return TRUE;
    return FALSE;
}
#endif

static BOOL mm_probe(struct PrismBoard *b)
{
    struct MinimigPriv *p = &priv;
#ifdef MINIMIG_FAKE
    if (!(p->fakeVram = AllocVec(MM_FAKE_SIZE, MEMF_ANY | MEMF_CLEAR)))
        return FALSE;
    memset(fakeRegs, 0, sizeof(fakeRegs));
    fakeRegs[R_ID / 2] = MM_ID;
    p->regs      = (volatile UBYTE *)fakeRegs;
    b->name      = "Minimig RTG (fake)";
    b->configDev = NULL;
    b->vram      = p->fakeVram;
    b->vramSize  = MM_FAKE_SIZE;
#else
    struct ConfigDev *cd = driver_find(NULL, MM_MFR, MM_PROD_RTG);
    if (cd) {
        if (cd->cd_BoardSize < MM_REGS_OFF + 0x800)
            return FALSE;
        p->regs  = (volatile UBYTE *)cd->cd_BoardAddr + MM_REGS_OFF;
        b->vram  = (UBYTE *)cd->cd_BoardAddr;
        b->name  = "MiSTer Minimig RTG";
    } else if (mm_legacy()) {
        p->regs  = (volatile UBYTE *)MM_LEGACY_REGS;
        b->vram  = (UBYTE *)MM_LEGACY_VRAM;
        b->name  = "MiSTer Minimig RTG (fixed addresses)";
    } else
        return FALSE;
    if (R16(p, R_ID) != MM_ID)
        return FALSE;
    b->configDev = cd;
    b->vramSize  = MM_VRAM_SIZE;
#endif
    b->regs      = p->regs;
    /* one format per depth, the ones the core's P96 driver offers (known
     * to work) and 15-bit in the same byte order */
    b->formats   = PF_BIT(PF_CLUT8) | PF_BIT(PF_RGB565LE) | PF_BIT(PF_RGB555LE) |
                   PF_BIT(PF_BGR24) | PF_BIT(PF_BGRA32);
    b->flags     = 0;
    b->maxWidth  = MM_MAX_W;
    b->maxHeight = MM_MAX_H;
    b->priv      = p;
    p->format    = 0;
    p->down      = FALSE;
    p->addrHi    = R16(p, R_ADDR_HI);

    b->setMode         = mm_SetMode;
    b->checkMode       = mm_CheckMode;
    b->bytesPerRow     = mm_bpr;
    b->setDisplayStart = mm_SetDisplayStart;
    b->setPalette      = mm_SetPalette;
    b->setSwitch       = mm_SetSwitch;
    b->shutdown        = mm_Shutdown;
    return TRUE;
}

#ifdef PRISM_DRIVER_MODULE
BOOL driver_probe(struct PrismBoard *b, const struct PrismDriverConfig *config)
{
    (void)config;
    return mm_probe(b);
}

/* The fake frame buffer is plain fast RAM no display reads: it goes back
 * when the driver ends, shut down or not. */
void driver_retain(void)
{
#ifdef MINIMIG_FAKE
    struct MinimigPriv *p = &priv;
    if (p->fakeVram) {
        FreeVec(p->fakeVram);
        p->fakeVram = NULL;
    }
#endif
}
#endif
