/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laine Jones */
/*
 * drv_template.c - the starting point for a new board driver.
 *
 * Copy this file to drv_<yourboard>.c, work through the TODO marks in
 * order, and build it with
 *
 *     sh tools/mkdriver.sh MYBOARD src/drv_myboard.c
 *
 * which makes out/Drivers/MYBOARD.driver. Copy that to LIBS:Prism/ and
 * start PrismD with BOARD=MYBOARD (or put BOARD=MYBOARD in Prism.prefs).
 * PrismD also tries every .driver in LIBS:Prism when the board is left on
 * Auto. docs/writing-a-driver.md walks through the whole job, including
 * how to test each step with PrismTest before PrismD ever runs on it.
 *
 * What a driver is: the smallest piece that knows the hardware. PrismD
 * does everything else - screens, bitmaps, drawing, the pointer, the OS
 * display database. A driver that only sets a mode and says where the
 * screen starts in VRAM gives a working Workbench; the blitter, hardware
 * cursor and the rest are optional extras added one at a time.
 *
 * Mandatory (PrismD refuses the driver without them):
 *   name, vram, vramSize, formats, maxWidth, maxHeight, setMode,
 *   setDisplayStart
 * Optional, with a default when left NULL (board_defaults in boardops.c):
 *   checkMode    - accepts any size up to maxWidth x maxHeight in one of
 *                  `formats`, with bytesPerRow = width * bytes per pixel
 *   setPalette   - ignored (a board with no 8-bit modes)
 *   setSwitch    - ignored (no pass-through of the Amiga's own video)
 *   waitVBlank   - returns at once
 *   shutdown     - does nothing
 * Optional, no default: saveState/restoreState, the blitter (ops), the
 * hardware cursor, bytesPerRow, modeReady, the direct fill/copy/expand
 * hooks (fast paths for small blits; ops alone is fine to begin with).
 *
 * Built with -DTEMPLATE_FAKE, this file is a complete driver for a board
 * that does not exist: 1 MB of fast RAM stands in for VRAM and nothing is
 * displayed. The test suite loads it, and PrismD runs on it in an
 * emulator (every drawing test passes without a card), which is how the
 * minimal contract above is kept honest.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <libraries/configvars.h>
#include <proto/exec.h>
#include <proto/expansion.h>
#include <string.h>
#include "boardops.h"
#ifdef PRISM_DRIVER_MODULE
#include "driver_module.h"
#endif

/* ---- 1. identify the board ------------------------------------------
 * TODO: the autoconfig manufacturer and product numbers of the board
 * (PrismProbe lists every board in the machine with its numbers).
 * Manufacturer 0 matches nothing, so the template finds no board. */
#define TEMPLATE_MFR   0
#define TEMPLATE_PROD  0

/* ---- 2. the board's own state ---------------------------------------
 * Everything the callbacks need between calls. One static instance: a
 * driver process serves one board. */
struct TemplatePriv {
    UWORD width, height;      /* the mode on display                       */
    UBYTE format;             /* enum PrismFormat                          */
    ULONG panOffset;          /* VRAM offset of the first displayed pixel  */
#ifdef TEMPLATE_FAKE
    UBYTE *fakeVram;
#endif
};
static struct TemplatePriv priv;

/* Register access. TODO: replace with the board's register layout.
 * `regs` is the register window the probe filled in. */
#define WREG8(b, off, v)  (*(volatile UBYTE *)((b)->regs + (off)) = (UBYTE)(v))
#define RREG8(b, off)     (*(volatile UBYTE *)((b)->regs + (off)))

/* ---- 3. modes ---------------------------------------------------------
 * checkMode is a dry run: say whether the mode can be shown and fill in
 * m->bytesPerRow (rows may be padded; PrismD lays bitmaps out with it).
 * setMode programs the board for it. PrismD asks for sizes from
 * PrismPrefs' list (640x400 up to 1920x1080) in each format in `formats`;
 * refuse what the board cannot do and those modes are simply not offered.
 *
 * TODO: a timing table for the sizes the board supports, and the CRTC
 * programming for them. The template accepts every size up to maxWidth x
 * maxHeight with unpadded rows, which is right for a board whose
 * firmware or FPGA takes a size and does the timing itself. */
static BOOL tpl_CheckMode(struct PrismBoard *b, struct PrismMode *m)
{
    UBYTE bpp = board_bpp(m->format);
    if (!bpp || !(b->formats & PF_BIT(m->format)))
        return FALSE;
    if (m->width < 320 || m->height < 200 || m->width > b->maxWidth || m->height > b->maxHeight)
        return FALSE;
    /* TODO: refresh (m->refresh is Hz, 0 = the board's default) */
    m->bytesPerRow = (ULONG)m->width * bpp;
    return TRUE;
}

static BOOL tpl_SetMode(struct PrismBoard *b, struct PrismMode *m)
{
    struct TemplatePriv *p = b->priv;
    if (!tpl_CheckMode(b, m))
        return FALSE;
    p->width = m->width;
    p->height = m->height;
    p->format = m->format;
    /* TODO: program the mode: timing, pixel format, pitch (= m->bytesPerRow).
     * Pixels are stored as enum PrismFormat says (byte order in VRAM);
     * pick the formats the board scans out natively. The display start
     * set earlier applies to the new mode too (see below). */
    return TRUE;
}

/* ---- 4. where the screen starts -----------------------------------------
 * PrismD keeps several screens in VRAM and shows one by pointing the
 * board at it (also how double buffering works). The offset is in bytes
 * from the start of `vram`. May be called before the first setMode: then
 * just remember it. */
static void tpl_SetDisplayStart(struct PrismBoard *b, ULONG off)
{
    struct TemplatePriv *p = b->priv;
    p->panOffset = off;
    /* TODO: write the start address registers (if the board has a mode) */
}

/* ---- 5. palette (8-bit modes) ---------------------------------------
 * `rgb` is count triples of 8-bit R, G, B for pens first.. Leave NULL for
 * a board with no palette modes (drop PF_CLUT8 from `formats` too). */
static void tpl_SetPalette(struct PrismBoard *b, UWORD first, UWORD count, const UBYTE *rgb)
{
    /* TODO: load the DAC. A 6-bit DAC takes rgb[i] >> 2. */
}

/* ---- 6. monitor switch ------------------------------------------------
 * rtg = TRUE shows the board's picture, FALSE the Amiga's own (a board
 * with a pass-through cable). Leave NULL if the board has no switch. */
static void tpl_SetSwitch(struct PrismBoard *b, BOOL rtg)
{
    /* TODO: flip the switch */
}

/* ---- 7. vertical blank --------------------------------------------------
 * Return when the next vertical blank starts (tear-free screen changes).
 * Leave NULL if the board cannot tell; PrismD then switches at once. */
static void tpl_WaitVBlank(struct PrismBoard *b)
{
    /* TODO: poll the status bit, with a bound so a dead board can't hang */
}

/* ---- 8. shutdown -------------------------------------------------------
 * PrismD is quitting (Ctrl-C): put the board back the way it was at
 * power-on, or at least show the Amiga's video. */
static void tpl_Shutdown(struct PrismBoard *b)
{
    tpl_SetSwitch(b, FALSE);
}

/* ---- 9. the blitter (optional) ----------------------------------------
 * Fill in a PrismOps table once the plain driver works. Each operation
 * gets the destination (and source) as a PrismSurface: `offset` is the
 * VRAM offset, `pitch` the row length in bytes, `format`/`bpp` the pixel
 * layout, `width`/`height` the bitmap size. Rectangles are already
 * clipped to the surface. Colours arrive as the pixel bytes in VRAM
 * order (a 16-bit pixel as a 16-bit value, 8-bit as the pen).
 *
 * Results (boardops.h):
 *   PR_DECLINED - nothing was written: PrismD draws it on the CPU. Say
 *                 this for anything the engine can't do (a pitch or size
 *                 it can't encode, a format it doesn't know).
 *   PR_DONE     - finished, the pixels are in VRAM.
 *   PR_RETRY    - the operation failed and the engine is stopped; PrismD
 *                 redraws it on the CPU and keeps the other operations.
 *   PR_FAILED   - the engine is wedged: PrismD stops using it.
 * Return only when the blit has finished (or provide waitBlit, which
 * PrismD calls before the CPU touches VRAM). */
#if 0   /* TODO: enable when the board has a blitter */
static enum PrismResult tpl_fill(struct PrismBoard *b, const struct PrismSurface *d,
                                 UWORD x, UWORD y, UWORD w, UWORD h, ULONG colour)
{
    return PR_DECLINED;
}
static enum PrismResult tpl_copy(struct PrismBoard *b, const struct PrismSurface *s,
                                 const struct PrismSurface *d, UWORD sx, UWORD sy,
                                 UWORD dx, UWORD dy, UWORD w, UWORD h)
{
    /* s and d may be the same surface with overlapping rectangles: copy
     * in the right direction, or decline those */
    return PR_DECLINED;
}
static enum PrismResult tpl_expand(struct PrismBoard *b, const struct PrismSurface *d,
                                   UWORD x, UWORD y, UWORD w, UWORD h, const UBYTE *tmpl,
                                   ULONG mod, ULONG fg, ULONG bg, BOOL transparent)
{
    /* text: rows of (w+7)/8 bytes `mod` apart, bit 7 first; set bits get
     * fg, clear bits bg or (transparent) stay */
    return PR_DECLINED;
}
static const struct PrismOps tpl_ops = { .fill = tpl_fill, .copy = tpl_copy, .expand = tpl_expand };
#endif

/* ---- 10. hardware cursor (optional) ------------------------------------
 * 64x64 bytes, one per pixel: 0 transparent, 1..3 the colours in rgb
 * (three R,G,B triples). x, y are never negative (PrismD clips at the
 * top-left). Set PBF_HW_CURSOR when all three work; without them PrismD
 * draws a software pointer. */
#if 0   /* TODO: enable when the board has a sprite */
static void tpl_CursorImage(struct PrismBoard *b, const UBYTE *img, const UBYTE *rgb) {}
static void tpl_CursorShow(struct PrismBoard *b, BOOL on) {}
static void tpl_CursorMove(struct PrismBoard *b, WORD x, WORD y) {}
#endif

/* ---- 11. the probe --------------------------------------------------------
 * Find the board and fill in the PrismBoard. Touch the hardware as little
 * as possible here: PrismD calls setMode before anything is shown. Return
 * FALSE (and print nothing) when the board is not in this machine - with
 * the board on Auto, PrismD tries every driver it has. */
static BOOL tpl_probe(struct PrismBoard *b)
{
    struct TemplatePriv *p = &priv;
#ifdef TEMPLATE_FAKE
    /* no hardware: a megabyte of fast RAM is the "VRAM" */
    if (!(p->fakeVram = AllocVec(1024 * 1024, MEMF_ANY | MEMF_CLEAR)))
        return FALSE;
    b->name      = "Template board (fake)";
    b->configDev = NULL;
    b->regs      = NULL;
    b->vram      = p->fakeVram;
    b->vramSize  = 1024 * 1024;
#else
    struct ConfigDev *cd = FindConfigDev(NULL, TEMPLATE_MFR, TEMPLATE_PROD);
    if (!cd)
        return FALSE;
    b->name      = "Template board";            /* TODO: the board's name */
    b->configDev = cd;
    /* TODO: where the registers and the VRAM are. Many boards have one
     * autoconfig node for registers and another for memory. */
    b->regs      = (volatile UBYTE *)cd->cd_BoardAddr;
    b->vram      = (UBYTE *)cd->cd_BoardAddr + 0x10000;
    b->vramSize  = cd->cd_BoardSize - 0x10000;
#endif
    /* TODO: the pixel formats the board scans out, as the 68k sees them in
     * VRAM. PrismD offers 8/16/24/32-bit modes from these. */
    b->formats   = PF_BIT(PF_CLUT8) | PF_BIT(PF_RGB565BE);
    b->flags     = 0;                           /* PBF_HW_CURSOR with a sprite */
    b->maxWidth  = 1024;                        /* TODO: the board's limits */
    b->maxHeight = 768;
    b->priv      = p;

    b->setMode         = tpl_SetMode;
    b->checkMode       = tpl_CheckMode;         /* NULL = board_defaults' */
    b->setDisplayStart = tpl_SetDisplayStart;
    b->setPalette      = tpl_SetPalette;        /* NULL = no palette */
    b->setSwitch       = tpl_SetSwitch;         /* NULL = no switch */
    b->waitVBlank      = tpl_WaitVBlank;        /* NULL = don't wait */
    b->shutdown        = tpl_Shutdown;          /* NULL = nothing to undo */
#if 0   /* TODO: blitter and cursor, once they work */
    b->ops         = &tpl_ops;
    b->flags      |= PBF_HW_CURSOR;
    b->cursorImage = tpl_CursorImage;
    b->cursorShow  = tpl_CursorShow;
    b->cursorMove  = tpl_CursorMove;
#endif
    return TRUE;
}

/* ---- 12. the module entry points ----------------------------------------
 * driver_module.c calls these. config carries PrismPrefs settings the
 * adapters use (P96CARD paths, PALETTE=BGR); most drivers ignore it. */
#ifdef PRISM_DRIVER_MODULE
BOOL driver_probe(struct PrismBoard *b, const struct PrismDriverConfig *config)
{
    return tpl_probe(b);
}

/* Called when the driver process ends. A driver that claimed something
 * it cannot give back (an interrupt, another system's context) parks
 * here instead of returning; see drv_uaegfx.c. */
void driver_retain(void)
{
}
#else
BOOL Template_Probe(struct PrismBoard *b)
{
    return tpl_probe(b);
}
#endif
