/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Jusii */
/*
 * drv_z3660.c - Z3660 accelerator RTG driver.
 *
 * The Z3660 (a 68060 accelerator for the A4000) shows an RTG framebuffer on
 * its HDMI output, drawn by the ARM firmware of its Zynq SoC. Written from
 * docs/z3660-hw.md, which cites the Z3660's own Picasso96 driver
 * (Z3660.card, z3660-drivers/rtg/gfx.c) and firmware in shanshe/Z3660
 * 25587f5, GPL-3.0-or-later; that driver is "based on MNT ZZ9000 rev 1.13"
 * (ZZ9000.card (C) 2016-2023 Lukas F. Hartmann / MNT Research GmbH,
 * (C) 2021 Bjorn Astrom). No code is copied from it.
 *
 * The firmware keeps the ZZ9000's meanings - a mode is one write to MODE,
 * the GFXData mailbox, the colour modes - over a different bus ABI:
 * registers are 32-bit words from 0x100, written as whole aligned longs;
 * VRAM starts at board + 0x200000; the mailbox is at board + 0x3200000.
 * Display start, palette and the hardware sprite go through the mailbox,
 * as Z3660.card does them on Zorro III.
 *
 * Works: the board with autoconfig_rtg YES (Zorro III, 0x144b/1, firmware
 * 1.03 or newer), the firmware's fixed modes at 8, 15, 16 and 32 bits,
 * 320-wide modes pixel-doubled, the palette, the hardware pointer, the
 * monitor switch on the CIA-B lines the firmware's monitor_switch names
 * (or ENV:Z3660-MONSWITCH, for a preset that leaves the switch to the OS).
 * Fills, copies, text, solid lines and planar blits (plain copies) on the
 * firmware's blitter (the mailbox ops), small ones through PrismD's direct
 * hooks.
 * Not yet: the board with autoconfig_rtg NO (no ConfigDev: not probed).
 */
#include <exec/types.h>
#include <libraries/configvars.h>
#include <proto/exec.h>
#include <proto/expansion.h>
#include <proto/dos.h>
#include "boardops.h"

extern struct ExpansionBase *ExpansionBase;

#define Z3_MFR          0x144b
#define Z3_PROD         1

/* registers: 32-bit words, offsets from the board base */
#define REG_MODE        0x100         /* id | colour << 8 | scale << 12     */
#define REG_SPRITE_SHOW 0x174         /* 1 = show, 2 = hide                 */
#define REG_VBLANK      0x17c         /* nonzero in vertical blank          */
#define REG_DMA_OP      0x180         /* run the mailbox op                 */
#define REG_ORIG_RES    0x18c         /* w << 16 | h of the output          */
#define REG_FW_VERSION  0x1a0         /* major << 8 | minor                 */
#define REG_MONSWITCH   0x318         /* the firmware's monitor_switch      */

/* monitor_switch, a sum (z3660cfg.txt): which CIA-B port A lines switch
 * the monitor, and for each whether its bit is 0 (level set) or 1 when
 * the RTG picture is wanted. 0 = no switch. */
#define MS_CTS          0x01
#define MS_SEL          0x02
#define MS_CTS_LEVEL    0x10
#define MS_SEL_LEVEL    0x20
#define CIAB            0xbfd000      /* CIA-B: port A, its DDR at +0x200   */
#define CIA_CTS         0x10          /* port A bit 4                       */
#define CIA_SEL         0x04          /* port A bit 2                       */

#define VRAM_OFFSET     0x200000
#define VRAM_END        0x3000000     /* as Z3660.card: 46 MB               */
#define GFXDATA_OFFSET  0x3200000     /* the mailbox, from the board base   */
#define STAGE_OFFSET    0x3210000     /* sprite image and template staging  */
#define STAGE_MAX       0x10000

/* firmware colour modes */
#define CM_8BIT     0
#define CM_565      1
#define CM_32BIT    2
#define CM_555      3

/* mailbox ops */
#define OP_DRAWLINE     1
#define OP_FILLRECT     2
#define OP_COPYRECT     3             /* inside one bitmap, overlap allowed  */
#define OP_COPY_NOMASK  4             /* between two bitmaps, with a minterm */
#define OP_TEMPLATE     5             /* 1-bit template to colour            */
#define OP_P2C          7             /* planar to 8-bit chunky              */
#define OP_P2D          8             /* planar to direct colour, via a table */
#define OP_PAN          10
#define OP_SPRITE_XY    11
#define OP_SPRITE_COLOR 12
#define OP_SPRITE_BMP   13
#define OP_SET_PALETTE  17

#define MINTERM_SRC     12            /* firmware enum value: plain copy     */

#define Z3_CURSOR_W     32            /* the firmware sprite is 32x48       */
#define Z3_CURSOR_H     48

/* GFXData field offsets (#pragma pack(4) on the ARM side) */
#define GD_OFFSET0   0
#define GD_OFFSET1   4
#define GD_RGB0      8
#define GD_RGB1      12
#define GD_PITCH0    40
#define GD_PITCH1    42
#define GD_MASK      57
#define GD_MINTERM   58
#define GD_X0        16
#define GD_X1        18
#define GD_X2        20
#define GD_Y0        24
#define GD_Y1        26
#define GD_Y2        28
#define GD_USER0     32
#define GD_USER1     34
#define GD_USER2     36
#define GD_USER3     38
#define GD_U8USER0   48
#define GD_U8USER1   49               /* draw mode                           */
#define GD_U8USER2   50               /* line pattern offset                 */
#define GD_U8USER3   51               /* line padding                        */
#define GD_U8OFFSET  59
#define GD_CLUT1     92

struct Z3Priv {
    UBYTE *gfxdata;
    volatile UBYTE *stage; /* staging above VRAM: templates, the pointer image */
    ULONG mode;            /* last MODE value written                    */
    ULONG origRes;         /* last ORIG_RES value                        */
    UWORD width;           /* bitmap width in pixels (pan x[1])          */
    UBYTE colormode;       /* CM_*                                       */
    ULONG panOffset;
    UBYTE monSwitch;       /* MS_* from the firmware                     */
    volatile UBYTE *ciab;  /* CIA-B port A (a test fixture points it elsewhere) */
};

static struct Z3Priv z3priv;

#define W32(b, off, v) (*(volatile ULONG *)((b)->regs + (off)) = (ULONG)(v))
#define R32(b, off)    (*(volatile ULONG *)((b)->regs + (off)))

#define GD8(p, off, v)  (*(volatile UBYTE *)((p)->gfxdata + (off)) = (UBYTE)(v))
#define GD16(p, off, v) (*(volatile UWORD *)((p)->gfxdata + (off)) = (UWORD)(v))
#define GD32(p, off, v) (*(volatile ULONG *)((p)->gfxdata + (off)) = (ULONG)(v))

/* The mailbox is ordinary card memory: push a copyback cache before the
 * firmware is told to read it. Callers hold Forbid(), as every task that
 * draws and the pointer share the one mailbox. */
static void z3_kick(struct PrismBoard *b, ULONG op)
{
    CacheClearU();
    W32(b, REG_DMA_OP, op);
}

/* ---- blitter ---------------------------------------------------------
 * One command = fill the mailbox, write the opcode to DMA_OP. The firmware
 * runs it inside that register write and acknowledges the bus cycle only
 * afterwards, so a call returns with the blit done (Z3660.card's
 * WaitBlitter is empty). The mailbox is shared by every task that draws
 * and by the pointer, so each command is built and sent under Forbid().
 * No cache push on this, the drawing path, as in drv_zz9000.c; pan,
 * palette and the sprite are rare and keep z3_kick's CacheClearU().
 * The commands are the ZZ9000 firmware's, unchanged (drv_zz9000.c):
 * pitches in 32-bit words, a colour placed so that the mailbox bytes are
 * the pixel bytes in VRAM order, 8-bit taken from the last byte. */
static ULONG z3_colour(UBYTE bpp, ULONG c)
{
    return bpp == 2 ? c << 16 : c;
}

static UBYTE z3_cm(UBYTE bpp)
{
    return bpp == 4 ? CM_32BIT : bpp == 2 ? CM_565 : CM_8BIT;
}

/* Rows on 32-bit words that the firmware can count, inside our VRAM. */
static BOOL z3_storage(struct PrismBoard *b, const struct PrismSurface *s)
{
    return (s->flags & PSF_VRAM) && !(s->pitch & 3) && s->pitch / 4 <= 65535 &&
           s->offset <= b->vramSize && s->allocation <= b->vramSize - s->offset;
}

/* ... in a layout the firmware draws: 15-bit fills use the 565 command
 * (the colour arrives as the raw 16-bit pixel, so the bytes are the same) */
static BOOL z3_surface(struct PrismBoard *b, const struct PrismSurface *s)
{
    return z3_storage(b, s) && (s->format == PF_CLUT8 || s->format == PF_RGB565BE ||
                                s->format == PF_RGB555BE || s->format == PF_BGRA32);
}

/* The commands, shared by PrismOps and the direct hooks (fillRect ...),
 * which PrismD calls without the surface layer for blits within the
 * blitMax* limits: those have no result, so the limits are set to what
 * the commands encode (pitches in whole longs, under 64 KB). */
static void z3_FillRect(struct PrismBoard *b, ULONG dst, ULONG pitch, UBYTE bpp,
                        UWORD x, UWORD y, UWORD w, UWORD h, ULONG colour)
{
    struct Z3Priv *p = b->priv;
    Forbid();
    GD32(p, GD_OFFSET0, dst);
    GD16(p, GD_PITCH0, pitch >> 2);
    GD8(p, GD_U8USER0, z3_cm(bpp));
    GD8(p, GD_MASK, 0xff);
    GD32(p, GD_RGB0, z3_colour(bpp, colour));
    GD16(p, GD_X0, x);
    GD16(p, GD_X1, w);
    GD16(p, GD_Y0, y);
    GD16(p, GD_Y1, h);
    W32(b, REG_DMA_OP, OP_FILLRECT);
    Permit();
}

/* inside one bitmap, in bytes; the firmware handles overlap */
static void z3_CopyBytes(struct PrismBoard *b, ULONG base, ULONG pitch,
                         UWORD sx, UWORD sy, UWORD dx, UWORD dy, UWORD wb, UWORD h)
{
    struct Z3Priv *p = b->priv;
    Forbid();
    GD32(p, GD_OFFSET0, base);
    GD32(p, GD_OFFSET1, base);
    GD16(p, GD_PITCH0, pitch >> 2);
    GD16(p, GD_X2, sx);
    GD16(p, GD_Y2, sy);
    GD16(p, GD_X0, dx);
    GD16(p, GD_Y0, dy);
    GD16(p, GD_X1, wb);
    GD16(p, GD_Y1, h);
    GD8(p, GD_U8USER0, CM_8BIT);
    GD8(p, GD_MASK, 0xff);
    W32(b, REG_DMA_OP, OP_COPYRECT);
    Permit();
}

static void z3_CopyRect(struct PrismBoard *b, ULONG base, ULONG pitch, UBYTE bpp,
                        UWORD sx, UWORD sy, UWORD dx, UWORD dy, UWORD w, UWORD h)
{
    z3_CopyBytes(b, base, pitch, sx * bpp, sy, dx * bpp, dy, w * bpp, h);
}

/* between two areas that don't overlap: rows from 32-bit words, the odd
 * bytes as x */
static void z3_CopyBetween(struct PrismBoard *b, ULONG src, ULONG spitch, ULONG dst,
                           ULONG dpitch, UWORD wbytes, UWORD h)
{
    struct Z3Priv *p = b->priv;
    Forbid();
    GD32(p, GD_OFFSET1, src & ~3UL);
    GD16(p, GD_PITCH1, spitch >> 2);
    GD16(p, GD_X2, src & 3);
    GD16(p, GD_Y2, 0);
    GD32(p, GD_OFFSET0, dst & ~3UL);
    GD16(p, GD_PITCH0, dpitch >> 2);
    GD16(p, GD_X0, dst & 3);
    GD16(p, GD_Y0, 0);
    GD16(p, GD_X1, wbytes);
    GD16(p, GD_Y1, h);
    GD8(p, GD_MINTERM, MINTERM_SRC);
    GD8(p, GD_U8USER0, CM_8BIT);
    W32(b, REG_DMA_OP, OP_COPY_NOMASK);
    Permit();
}

static enum PrismResult z3_fill(struct PrismBoard *b, const struct PrismSurface *d,
                                UWORD x, UWORD y, UWORD w, UWORD h, ULONG c)
{
    if (!z3_surface(b, d) || (ULONG)x + w > 65535)
        return PR_DECLINED;
    z3_FillRect(b, d->offset, d->pitch, d->bpp, x, y, w, h, c);
    return PR_DONE;
}

/* Copies use byte geometry (an 8-bit copy of w * bpp bytes), so they work
 * for every pixel layout. Inside one bitmap the firmware handles overlap;
 * between two bitmaps the rows start on 32-bit words and the odd bytes
 * become x. Overlapping copies between two surfaces are declined. */
static enum PrismResult z3_copy(struct PrismBoard *b, const struct PrismSurface *s,
                                const struct PrismSurface *d, UWORD sx, UWORD sy,
                                UWORD dx, UWORD dy, UWORD w, UWORD h)
{
    if (!z3_storage(b, s) || !z3_storage(b, d) || s->bpp != d->bpp ||
        ((ULONG)sx + w) * s->bpp > 65535 || ((ULONG)dx + w) * d->bpp > 65535)
        return PR_DECLINED;
    if (s->offset == d->offset && s->pitch == d->pitch) {
        z3_CopyBytes(b, d->offset, d->pitch, sx * s->bpp, sy, dx * d->bpp, dy, w * d->bpp, h);
        return PR_DONE;
    }
    if (s->offset + s->allocation > d->offset && d->offset + d->allocation > s->offset)
        return PR_DECLINED;
    z3_CopyBetween(b, s->offset + (ULONG)sy * s->pitch + (ULONG)sx * s->bpp, s->pitch,
                   d->offset + (ULONG)dy * d->pitch + (ULONG)dx * d->bpp, d->pitch,
                   w * d->bpp, h);
    return PR_DONE;
}

/* Text: the template is copied to the staging area above VRAM, then one
 * RECT_TEMPLATE command colours it in. Its destination pitch is in bytes.
 * FALSE (nothing written): too big for the staging area or the pitch. */
static BOOL z3_ExpandRect(struct PrismBoard *b, ULONG dst, ULONG pitch, UBYTE bpp,
                          UWORD x, UWORD y, UWORD w, UWORD h, const UBYTE *tmpl,
                          ULONG mod, ULONG fg, ULONG bg, BOOL transparent)
{
    struct Z3Priv *p = b->priv;
    ULONG rb = ((ULONG)w + 7) >> 3, n;
    volatile UBYTE *o = p->stage;
    UWORD r;

    if (!w || !h || bpp == 3 || pitch > 65535 || rb * h > STAGE_MAX)
        return FALSE;
    Forbid();
    for (r = 0; r < h; r++, tmpl += mod) {
        /* longs where they fit: each card access costs the same */
        for (n = 0; n + 4 <= rb; n += 4, o += 4)
            *(volatile ULONG *)o = *(const ULONG *)(tmpl + n);
        for (; n < rb; n++)
            *o++ = tmpl[n];
    }
    GD16(p, GD_X0, x);
    GD16(p, GD_X1, w);
    GD16(p, GD_X2, 0);
    GD16(p, GD_Y0, y);
    GD16(p, GD_Y1, h);
    GD16(p, GD_Y2, 0);
    GD32(p, GD_OFFSET0, dst);
    GD32(p, GD_OFFSET1, STAGE_OFFSET - VRAM_OFFSET);
    GD16(p, GD_PITCH0, pitch);
    GD16(p, GD_PITCH1, rb);
    GD32(p, GD_RGB0, z3_colour(bpp, fg));
    GD32(p, GD_RGB1, z3_colour(bpp, bg));
    GD8(p, GD_U8USER0, z3_cm(bpp));
    GD8(p, GD_U8USER1, transparent ? 0 : 1);      /* JAM1 : JAM2 */
    GD8(p, GD_MASK, 0xff);
    W32(b, REG_DMA_OP, OP_TEMPLATE);
    Permit();
    return TRUE;
}

static enum PrismResult z3_expand(struct PrismBoard *b, const struct PrismSurface *d,
                                  UWORD x, UWORD y, UWORD w, UWORD h, const UBYTE *tmpl,
                                  ULONG mod, ULONG fg, ULONG bg, BOOL transparent)
{
    if (!z3_surface(b, d))
        return PR_DECLINED;
    return z3_ExpandRect(b, d->offset, d->pitch, d->bpp, x, y, w, h, tmpl, mod,
                         fg, bg, transparent) ? PR_DONE : PR_DECLINED;
}

/* Solid lines. The firmware seeds its error term with L/2 and steps the
 * shorter axis when it reaches L: round(i * S / L) with halves rounded
 * up, PrismD's own rule (line_solid in render.c), for odd and even L. */
static enum PrismResult z3_line(struct PrismBoard *b, const struct PrismSurface *d,
                                WORD x, WORD y, WORD dx, WORD dy, ULONG c)
{
    struct Z3Priv *p = b->priv;
    UWORD ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
    UWORD len = ax >= ay ? ax : ay;

    if (!z3_surface(b, d))
        return PR_DECLINED;
    Forbid();
    GD32(p, GD_OFFSET0, d->offset);
    GD16(p, GD_PITCH0, d->pitch >> 2);
    GD8(p, GD_U8USER0, z3_cm(d->bpp));
    GD8(p, GD_U8USER1, 0);                        /* JAM1 */
    GD8(p, GD_U8USER2, 0);
    GD8(p, GD_U8USER3, 0);
    GD8(p, GD_MASK, 0xff);
    GD32(p, GD_RGB0, z3_colour(d->bpp, c));
    GD16(p, GD_X0, x);
    GD16(p, GD_X1, dx);
    GD16(p, GD_Y0, y);
    GD16(p, GD_Y1, dy);
    GD16(p, GD_USER0, len);
    GD16(p, GD_USER1, 0xffff);                    /* solid */
    GD16(p, GD_USER2, 0);
    GD16(p, GD_USER3, len >> 1);
    W32(b, REG_DMA_OP, OP_DRAWLINE);
    Permit();
    return PR_DONE;
}

/* Commands finish inside their register write; a read of a register
 * orders it before the CPU's next access to VRAM. */
static void z3_WaitBlit(struct PrismBoard *b)
{
    (void)R32(b, REG_FW_VERSION);
}

/* Planar blits (icons, gadget imagery, planar games): the planes of the
 * source rectangle are staged above VRAM - after a 256-entry pixel table
 * for 16/32-bit destinations - and P2C or P2D builds the pixels on the
 * card. Plain copies only (minterm 0xCx, all planes), as the other
 * adapters do. Staging stays under Forbid() like every mailbox user, so
 * it is bounded: a blit that needs more than STAGE_MAX goes to the CPU.
 * Table entries are placed like fill colours; a 32-bit pixel is
 * B,G,R,0 as PrismD writes it, a 16-bit one comes from the pen table. */
static enum PrismResult z3_planar(struct PrismBoard *b, const struct PrismPlanar *src,
                                  const struct PrismSurface *d, UWORD sx, UWORD sy,
                                  UWORD dx, UWORD dy, UWORD w, UWORD h, UBYTE mt, UBYTE mask)
{
    struct Z3Priv *p = b->priv;
    const struct BitMap *bm = src->bitmap;
    ULONG line = (((ULONG)sx & 7) + w + 7) >> 3, plane = line * h;
    ULONG table = d->bpp == 1 ? 0 : 256 * 4;
    volatile UBYTE *o = p->stage;
    UBYTE layers = 0xff, i;
    ULONG n;
    UWORD r;

    if (!z3_surface(b, d) || (mt & 0xf0) != 0xc0 || mask != 0xff ||
        (d->bpp == 2 && !src->pens16) || w > 32767 || h > 32767 ||
        table + plane * bm->Depth > STAGE_MAX)
        return PR_DECLINED;
    Forbid();
    for (n = 0; table && n < 256; n++, o += 4) {
        ULONG c = src->colours ? src->colours[n] : n * 0x010101UL;
        *(volatile ULONG *)o = d->bpp == 2 ? (ULONG)src->pens16[n] << 16 :
            (c & 0xff) << 24 | ((c >> 8) & 0xff) << 16 | ((c >> 16) & 0xff) << 8;
    }
    for (i = 0; i < bm->Depth; i++) {
        const UBYTE *s = (const UBYTE *)bm->Planes[i];
        if (s == (const UBYTE *)-1) {          /* all ones */
            for (n = 0; n < plane; n++)
                *o++ = 0xff;
            continue;
        }
        if (!s) {                              /* no plane: leave it out */
            layers &= ~(1 << i);
            o += plane;
            continue;
        }
        for (s += (ULONG)sy * bm->BytesPerRow + (sx >> 3), r = 0; r < h; r++, s += bm->BytesPerRow) {
            for (n = 0; n + 4 <= line; n += 4, o += 4)
                *(volatile ULONG *)o = *(const ULONG *)(s + n);
            for (; n < line; n++)
                *o++ = s[n];
        }
    }
    GD32(p, GD_OFFSET0, d->offset);
    GD32(p, GD_OFFSET1, STAGE_OFFSET - VRAM_OFFSET);
    GD16(p, GD_PITCH0, d->pitch >> 2);
    GD16(p, GD_PITCH1, line);
    GD16(p, GD_X0, sx & 7);
    GD16(p, GD_X1, dx);
    GD16(p, GD_Y1, dy);
    GD16(p, GD_X2, w);
    GD16(p, GD_Y2, h);
    GD16(p, GD_USER0, layers);
    GD16(p, GD_USER1, bm->Depth);
    GD32(p, GD_RGB0, d->bpp == 2 ? 0xffff : 0xffffffffUL);
    GD8(p, GD_U8USER0, z3_cm(d->bpp));
    GD8(p, GD_MASK, 0xff);
    GD8(p, GD_MINTERM, MINTERM_SRC);
    W32(b, REG_DMA_OP, d->bpp == 1 ? OP_P2C : OP_P2D);
    Permit();
    return PR_DONE;
}

static const struct PrismOps z3_ops = { .fill = z3_fill, .copy = z3_copy,
                                         .expand = z3_expand, .line = z3_line,
                                         .planar = z3_planar };

/* Firmware mode ids by output size and refresh, with the pixel clock in MHz
 * (docs/z3660-hw.md 2). */
static const struct { UWORD w, h; UBYTE id, hz, mhz; } z3_modes[] = {
    { 1280,  720,  0, 60,  74 }, {  800,  600,  1, 60,  40 }, {  640,  480,  2, 60,  25 },
    { 1024,  768,  3, 60,  65 }, { 1280, 1024,  4, 60, 108 }, { 1920, 1080,  5, 60, 150 },
    {  720,  576,  6, 50,  27 }, { 1920, 1080,  7, 50, 148 }, {  720,  480,  8, 60,  25 },
    {  640,  512,  9, 60,  40 }, { 1600, 1200, 10, 60, 161 }, { 2560, 1440, 11, 30, 146 },
    {  640,  400, 16, 60,  25 }, { 1280,  800, 17, 60,  66 }, { 1920, 1200, 18, 60, 148 },
    { 1600,  900, 19, 60,  93 }, { 1680, 1050, 20, 60, 114 }, { 1366,  768, 21, 60,  72 },
};

/* The firmware shows 32-bit modes right up to a 108 MHz pixel clock (1280x1024);
 * from 1680x1050 (114 MHz) up they come out line-doubled (docs/z3660-hw.md 2). */
#define Z3_MAX32_MHZ    108

static int z3_mode(UWORD w, UWORD h, UBYTE hz)
{
    int i;
    for (i = 0; i < (int)(sizeof(z3_modes) / sizeof(z3_modes[0])); i++)
        if (z3_modes[i].w == w && z3_modes[i].h == h && (!hz || z3_modes[i].hz == hz))
            return i;
    return -1;
}

static void z3_pan(struct PrismBoard *b)
{
    struct Z3Priv *p = b->priv;
    /* 15-bit is panned as 565: same bytes per pixel */
    UBYTE cm = (p->colormode == CM_555) ? CM_565 : p->colormode;

    Forbid();
    GD32(p, GD_OFFSET0, p->panOffset);
    GD16(p, GD_X0, 0);
    GD16(p, GD_Y0, 0);
    GD16(p, GD_X1, p->width);
    GD8(p, GD_U8USER0, cm);
    z3_kick(b, OP_PAN);
    Permit();
}

/* Validate a mode without touching the card: returns the MODE register
 * value (or -1) and fills m->bytesPerRow, the colour mode and ORIG_RES. */
static LONG z3_check(struct PrismMode *m, UBYTE *cmOut, ULONG *origRes)
{
    UWORD ow = m->width, oh = m->height, scale = 0;
    int i;
    UBYTE cm, bpp;

    switch (m->format) {
    case PF_CLUT8:    cm = CM_8BIT;  bpp = 1; break;
    case PF_RGB565BE: cm = CM_565;   bpp = 2; break;
    case PF_RGB555BE: cm = CM_555;   bpp = 2; break;
    case PF_BGRA32:   cm = CM_32BIT; bpp = 4; break;
    default:          return -1;
    }
    if (m->width < 320 || m->height < 200)
        return -1;
    /* an oversized (autoscroll) screen: the scan-out here steps rows by
     * the mode's own width only */
    if (m->pitchPixels && m->pitchPixels != m->width)
        return -1;
    /* below 640x400 the firmware doubles both axes into the 2x mode,
     * as Z3660.card does for 320x200/240/256 */
    if (m->width < 640 && m->height < 400) {
        ow = m->width * 2;
        oh = m->height * 2;
        scale = 3;
    }
    if ((i = z3_mode(ow, oh, m->refresh)) < 0)
        return -1;
    if (bpp == 4 && z3_modes[i].mhz > Z3_MAX32_MHZ)
        return -1;
    /* the blitter counts pitches in longs, and the direct hooks cannot
     * decline: rows that are not whole longs (1366x768 8-bit) are left out */
    if (((ULONG)m->width * bpp) & 3)
        return -1;
    m->bytesPerRow = (ULONG)m->width * bpp;
    *cmOut = cm;
    *origRes = ((ULONG)ow << 16) | oh;
    return z3_modes[i].id | (cm << 8) | (scale << 12);
}

static BOOL z3_CheckMode(struct PrismBoard *b, struct PrismMode *m)
{
    UBYTE cm;
    ULONG o;
    return z3_check(m, &cm, &o) >= 0;
}

static BOOL z3_SetMode(struct PrismBoard *b, struct PrismMode *m)
{
    struct Z3Priv *p = b->priv;
    LONG mode;
    UBYTE cm;
    ULONG o;

    if ((mode = z3_check(m, &cm, &o)) < 0)
        return FALSE;
    p->mode = mode;
    p->origRes = o;
    p->width = m->width;
    p->colormode = cm;

    W32(b, REG_ORIG_RES, p->origRes);
    W32(b, REG_MODE, p->mode);
    z3_pan(b);
    return TRUE;
}

static void z3_SetDisplayStart(struct PrismBoard *b, ULONG off)
{
    struct Z3Priv *p = b->priv;
    p->panOffset = off;
    if (p->width)              /* before the first mode set: just remember it */
        z3_pan(b);
}

static void z3_SetPalette(struct PrismBoard *b, UWORD first, UWORD count,
                          const UBYTE *rgb)
{
    struct Z3Priv *p = b->priv;
    UWORD i;

    if (first >= 256 || !count)
        return;
    if (first + count > 256)
        count = 256 - first;

    Forbid();
    for (i = 0; i < count * 3; i++)
        GD8(p, GD_CLUT1 + i, rgb[i]);
    GD16(p, GD_USER0, first);
    GD16(p, GD_USER1, count);
    GD8(p, GD_U8USER0, 0);               /* primary palette */
    z3_kick(b, OP_SET_PALETTE);
    Permit();
}

/* ---- the monitor switch ---------------------------------------------
 * The Z3660 has no capture of the Amiga's own picture: an external switch
 * on CIA-B's CTS and/or SEL lines picks the monitor's input. The lines are
 * made outputs and set as monitor_switch says, the other port bits kept;
 * Disable() because serial.device changes RTS/DTR on the same port from
 * its interrupt. Called on screen changes and around alerts: no waiting. */
static void z3_SetSwitch(struct PrismBoard *b, BOOL rtg)
{
    struct Z3Priv *p = b->priv;
    UBYTE lines = 0, set = 0;

    if (p->monSwitch & MS_CTS) {
        lines |= CIA_CTS;
        if (!rtg != !(p->monSwitch & MS_CTS_LEVEL))
            set |= CIA_CTS;
    }
    if (p->monSwitch & MS_SEL) {
        lines |= CIA_SEL;
        if (!rtg != !(p->monSwitch & MS_SEL_LEVEL))
            set |= CIA_SEL;
    }
    Disable();
    p->ciab[0x200] |= lines;
    p->ciab[0] = (p->ciab[0] & ~lines) | set;
    Enable();
}

static void z3_WaitVBlank(struct PrismBoard *b)
{
    ULONG n;
    /* finish any blank we are in, then wait for the next one to start */
    for (n = 0; n < 200000 && R32(b, REG_VBLANK); n++) ;
    for (n = 0; n < 200000 && !R32(b, REG_VBLANK); n++) ;
}

/* ---- hardware cursor -------------------------------------------------
 * The firmware turns a two-plane Amiga sprite image into colours when it
 * loads it, so the colours go first. Rows are 4 bytes of plane 0, then 4
 * of plane 1; the image Prism hands over is 64x64 with pens 0-3, and its
 * top-left 32x48 is what the firmware shows. */
static void z3_CursorImage(struct PrismBoard *b, const UBYTE *img, const UBYTE *rgb)
{
    struct Z3Priv *p = b->priv;
    volatile ULONG *o = (volatile ULONG *)p->stage;
    UWORD x, y;

    Forbid();
    for (x = 0; x < 3; x++, rgb += 3) {
        /* bytes B,G,R,0: the ARM reads them as 0x00RRGGBB */
        GD32(p, GD_RGB0, ((ULONG)rgb[2] << 24) | ((ULONG)rgb[1] << 16) | ((ULONG)rgb[0] << 8));
        GD8(p, GD_U8OFFSET, x + 1);
        z3_kick(b, OP_SPRITE_COLOR);
    }
    for (y = 0; y < Z3_CURSOR_H; y++) {
        ULONG p0 = 0, p1 = 0;
        for (x = 0; x < Z3_CURSOR_W; x++) {
            UBYTE pen = img[y * CURSOR_SIZE + x];
            p0 = (p0 << 1) | (pen & 1);
            p1 = (p1 << 1) | ((pen >> 1) & 1);
        }
        *o++ = p0;
        *o++ = p1;
    }
    GD32(p, GD_OFFSET1, STAGE_OFFSET - VRAM_OFFSET);
    GD16(p, GD_X0, 0);                            /* hot spot: Prism's */
    GD16(p, GD_Y0, 0);
    GD16(p, GD_X1, Z3_CURSOR_W);
    GD16(p, GD_Y1, Z3_CURSOR_H);
    GD16(p, GD_X2, 0);                            /* not doubled */
    GD16(p, GD_Y2, 0);
    z3_kick(b, OP_SPRITE_BMP);
    Permit();
}

static void z3_CursorShow(struct PrismBoard *b, BOOL on)
{
    W32(b, REG_SPRITE_SHOW, on ? 1 : 2);
}

static void z3_CursorMove(struct PrismBoard *b, WORD x, WORD y)
{
    struct Z3Priv *p = b->priv;
    Forbid();
    GD16(p, GD_X0, x);
    GD16(p, GD_Y0, y);
    /* every mouse move: no cache push (two words, as the drawing path and
     * drv_zz9000.c's cursor move do), only the opcode */
    W32(b, REG_DMA_OP, OP_SPRITE_XY);
    Permit();
}

static void z3_Shutdown(struct PrismBoard *b)
{
    struct Z3Priv *p = b->priv;
    z3_CursorShow(b, FALSE);
    if (p->monSwitch & (MS_CTS | MS_SEL))
        z3_SetSwitch(b, FALSE);          /* the Amiga's own picture */
}

/* ENV:Z3660-MONSWITCH = a monitor_switch value (decimal, as in
 * z3660cfg.txt) replaces the firmware's, in the ENV: style of Z3660.card's
 * ENV:Z3660-NS-VSYNC. For a preset shared with another OS that drives the
 * lines itself and so keeps monitor_switch 0. The probe runs in the
 * driver's own process, so DOS is allowed here. */
static void z3_monswitch_env(struct Z3Priv *p)
{
    char v[8];
    LONG n = GetVar("Z3660-MONSWITCH", v, sizeof(v), GVF_GLOBAL_ONLY), i;
    ULONG ms = 0;

    if (n <= 0)
        return;
    for (i = 0; i < n && v[i] >= '0' && v[i] <= '9'; i++)
        ms = ms * 10 + (v[i] - '0');
    if (i == 0 || ms > 255)
        return;                      /* not a number: the firmware's value stands */
    p->monSwitch = ms & (MS_CTS | MS_SEL | MS_CTS_LEVEL | MS_SEL_LEVEL);
}

static BOOL z3_probe(struct PrismBoard *b)
{
    struct ConfigDev *cd;
    struct Z3Priv *p = &z3priv;
    ULONG fw;

    if (!(cd = driver_find(NULL, Z3_MFR, Z3_PROD)))
        return FALSE;
    /* the mailbox and the staging area sit near the top of the board's
     * window: a smaller window is not a board this driver knows */
    if (cd->cd_BoardSize < STAGE_OFFSET + STAGE_MAX)
        return FALSE;
    b->regs = (volatile UBYTE *)cd->cd_BoardAddr;
    fw = R32(b, REG_FW_VERSION);
    if ((fw >> 8) != 1 || (fw & 0xff) < 3)
        return FALSE;            /* not this firmware, or older than 1.03 */

    b->name      = "Z3660";
    b->configDev = cd;
    b->vram      = (UBYTE *)cd->cd_BoardAddr + VRAM_OFFSET;
    b->vramSize  = VRAM_END - VRAM_OFFSET;
    b->formats   = PF_BIT(PF_CLUT8) | PF_BIT(PF_RGB565BE) |
                   PF_BIT(PF_RGB555BE) | PF_BIT(PF_BGRA32);
    b->flags     = PBF_HW_CURSOR | PBF_BLIT_32;
    b->cursorW   = Z3_CURSOR_W;      /* a bigger pointer goes to the software one */
    b->cursorH   = Z3_CURSOR_H;
    b->maxWidth  = 2560;
    b->maxHeight = 1440;
    b->priv      = p;
    p->gfxdata   = (UBYTE *)cd->cd_BoardAddr + GFXDATA_OFFSET;
    p->stage     = (UBYTE *)cd->cd_BoardAddr + STAGE_OFFSET;
    p->ciab      = (volatile UBYTE *)CIAB;
    p->monSwitch = R32(b, REG_MONSWITCH) & (MS_CTS | MS_SEL | MS_CTS_LEVEL | MS_SEL_LEVEL);
    z3_monswitch_env(p);

    b->setMode         = z3_SetMode;
    b->checkMode       = z3_CheckMode;
    b->setDisplayStart = z3_SetDisplayStart;
    b->setPalette      = z3_SetPalette;
    b->waitVBlank      = z3_WaitVBlank;
    if (p->monSwitch & (MS_CTS | MS_SEL))  /* else no switch: NULL */
        b->setSwitch   = z3_SetSwitch;
    b->shutdown        = z3_Shutdown;
    b->ops             = &z3_ops;
    b->waitBlit        = z3_WaitBlit;
    b->fillRect        = z3_FillRect;
    b->copyRect        = z3_CopyRect;
    b->copyBetween     = z3_CopyBetween;
    b->expandRect      = z3_ExpandRect;
    b->blitMaxPitch    = 65532;   /* x + w in bytes then fits 16 bits */
    b->cursorImage     = z3_CursorImage;
    b->cursorShow      = z3_CursorShow;
    b->cursorMove      = z3_CursorMove;
    return TRUE;
}

/* ---- as a loadable module (driver_module.c) --------------------------- */
#ifdef PRISM_DRIVER_MODULE
#include "driver_module.h"
BOOL driver_probe(struct PrismBoard *b, const struct PrismDriverConfig *config)
{
    return z3_probe(b);
}
void driver_retain(void)
{
}
#else
BOOL Z3660_Probe(struct PrismBoard *b)
{
    return z3_probe(b);
}
#endif
