/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laine Jones
 * Copyright (C) 2026 Stefan Reinauer */
/*
 * drv_zz9000.c - MNT ZZ9000 board driver.
 *
 * Written from docs/zz9000-hw.md, checked against the v2.8.1 driver and
 * firmware sources (ZZ9000.card 2.13). The firmware owns all video timing: a
 * mode is one write to MODE (0x02), the displayed framebuffer is set with
 * the pan registers, and native Amiga video is shown by pointing the pan at
 * the capture buffer and turning capture on.
 *
 * Every register is 16 bits wide. Addresses handed to the card are byte
 * offsets from VRAM = board + 0x10000. Zorro III boards take blitter-style
 * commands through a "GFXData" mailbox in card memory plus a DMA_OP write;
 * Zorro II boards use the register file.
 */
#include <exec/types.h>
#include <libraries/configvars.h>
#include <proto/exec.h>
#include <proto/expansion.h>
#include "boardops.h"

extern struct ExpansionBase *ExpansionBase;

#define ZZ_MFR          0x6d6e
#define ZZ_PROD_Z2      3
#define ZZ_PROD_Z3      4

/* registers (offsets from the board base) */
#define REG_MODE        0x02
#define REG_PAN_HI      0x0a
#define REG_PAN_LO      0x0c
#define REG_X1          0x10
#define REG_Y1          0x12
#define REG_X2          0x14
#define REG_COLORMODE   0x30
#define REG_SPRITE_SHOW 0x48          /* 1 = cursor on, 2 = off              */
#define REG_VBLANK      0x4c
#define REG_USER1       0x40          /* the operand of SET_FEATURE         */
#define REG_DMA_OP      0x5a
#define REG_SET_FEATURE 0x60
#define FEATURE_DPMS    4             /* BlitterStudio firmware 2.7+         */
#define REG_FW_VERSION  0xc0
#define REG_FW_CAPS     0xe6
#define REG_CFG_KEY     0xe8          /* ZZ9000.CFG query (firmware 2.3+)    */
#define REG_CFG_PRESENT 0xea
#define REG_CX_DATA_HI  0x1000
#define REG_CX_DATA_LO  0x1002
#define REG_CX_OP       0x1004
#define REG_CAPTURE     0x1006

#define VRAM_OFFSET     0x10000
#define CAPTURE_OFFSET  0xe00000      /* native video capture buffer         */
#define CAPTURE_OFF_OLD 0xdff2f8      /* ... the older "tuned" origin        */
#define VRAM_LIMIT      0xdf0000      /* Prism's bitmaps stay below both     */

/* capture output modes (ZZ9000.CFG videocap_mode) */
#define VMODE_800x600   1
#define VMODE_1080P_60  5
#define VMODE_720x576   6
#define VMODE_1080P_50  7
#define VMODE_1080P_SYNC 0x100
#define CFG_KEY_VCAP_MODE 1
#define CAP_SCANOUT_ORIGIN (1 << 6)   /* firmware places the capture itself  */
#define GFXDATA_OFFSET  0x3200000     /* Z3 mailbox, from the board base     */
#define TEMPLATE_OFFSET 0x3210000     /* Z3 staging area (templates, cursor) */
#define TEMPLATE_MAX    0x10000

/* firmware colour modes */
#define CM_8BIT     0
#define CM_565      1
#define CM_32BIT    2
#define CM_555      3

/* Z3 DMA_OP opcodes */
#define OP_DRAWLINE     1
#define OP_FILLRECT     2
#define OP_COPYRECT     3             /* inside one bitmap, overlap allowed  */
#define OP_COPY_NOMASK  4             /* between two bitmaps, with a minterm */
#define OP_TEMPLATE     5             /* 1-bit template to colour            */
#define OP_PAN          10
#define OP_SPRITE_XY    11
#define OP_SPRITE_CLUT  14            /* cursor image, one byte per pixel    */
#define OP_SET_PALETTE  17
#define OP_VIDEO_OVERLAY 19           /* BlitterStudio firmware 2.8+: see zz_OverlaySet */

#define MINTERM_SRC     12            /* firmware enum value: plain copy     */
#define ZZ_CURSOR_W     32            /* the firmware cursor is 32x48        */
#define ZZ_CURSOR_H     48

/* GFXData field offsets (struct is #pragma pack(4) on the ARM side) */
#define GD_OFFSET0   0
#define GD_OFFSET1   4
#define GD_RGB0      8
#define GD_RGB1      12
#define GD_X2        20
#define GD_Y1        26
#define GD_Y2        28
#define GD_USER2     36
#define GD_USER3     38
#define GD_PITCH0    40
#define GD_PITCH1    42
#define GD_U8USER1   49               /* draw mode                           */
#define GD_U8USER2   50               /* line pattern offset                 */
#define GD_U8USER3   51               /* line padding                        */
#define GD_MASK      57
#define GD_MINTERM   58
#define GD_U8OFFSET  59
#define GD_X0        16
#define GD_X1        18
#define GD_Y0        24
#define GD_USER0     32
#define GD_USER1     34
#define GD_U8USER0   48
#define GD_CLUT1     92

struct ZZPriv {
    BOOL  z3;
    UBYTE *gfxdata;
    UWORD mode;            /* last MODE value written                    */
    UWORD width;           /* bitmap width in pixels (pan X2)            */
    UBYTE colormode;       /* CM_*                                       */
    ULONG panOffset;
    ULONG capOffset;       /* pan that shows the native capture          */
    UBYTE saved[GD_CLUT1]; /* mailbox header, while the card is borrowed */
};

static struct ZZPriv zzpriv;

#define W16(b, off, v) (*(volatile UWORD *)((b)->regs + (off)) = (UWORD)(v))
#define R16(b, off)    (*(volatile UWORD *)((b)->regs + (off)))

#define GD8(p, off, v)  (*(volatile UBYTE *)((p)->gfxdata + (off)) = (UBYTE)(v))
#define GD16(p, off, v) (*(volatile UWORD *)((p)->gfxdata + (off)) = (UWORD)(v))
#define GD32(p, off, v) (*(volatile ULONG *)((p)->gfxdata + (off)) = (ULONG)(v))

/* Every write into the mailbox is a Zorro III cycle, about 0.6 us on the
 * A4000's 68060 (profiled 2026-10-14: the nine field writes of an 8x8 fill
 * were a fifth of its time, the fifteen of a 4-character Text a quarter).
 * So the fields an op sets together go in one 32-bit write where they are
 * neighbours: x[0..1], y[0..1], pitch[0..1], user[0..1], user[2..3],
 * u8_user[0..1] (PAIR puts the lower offset in the high half).
 *
 * Every field an op uses is written every time. Writing only the fields
 * that changed since the last op (as the Cirrus driver does with its
 * registers) took the A4000 down: the firmware converts the header's
 * fields in place as it reads them, so after an op the card holds x[],
 * y[], user[], pitch[] and offset[1] byte-swapped and offset[0] zero (read
 * back on the A4000, 2026-10-14); only rgb[], u8_user[], mask and minterm
 * stay as written. A field left over from the last op is garbage to the
 * next, and a template op with a swapped source offset hung the card and
 * the bus with it. */
#define PAIR(hi, lo)    (((ULONG)(UWORD)(hi) << 16) | (UWORD)(lo))

/* Z3: the mailbox is ordinary card memory; make sure a copyback 060 cache
 * has pushed it before the firmware is told to read it. */
static void z3_kick(struct PrismBoard *b, UWORD op)
{
    CacheClearU();
    W16(b, REG_DMA_OP, op);
}

/* ---- blitter (Zorro III) --------------------------------------------
 * One command = fill the mailbox, write the opcode to DMA_OP. The firmware
 * runs it before the write cycle ends, so a call returns with the blit
 * done. The mailbox is shared by every task that draws and by the cursor,
 * so each command is built and sent under Forbid().
 *
 * Pitches for fills and copies are in 32-bit words (Prism rows are
 * multiples of 4 bytes); the template op takes bytes. A colour goes in so
 * that the mailbox bytes are the pixel bytes in VRAM order - except
 * 8-bit, where the firmware takes the last byte. */
static ULONG zz_colour(UBYTE bpp, ULONG c)
{
    return bpp == 2 ? c << 16 : c;
}

static UBYTE zz_cm(UBYTE bpp)
{
    return bpp == 4 ? CM_32BIT : bpp == 2 ? CM_565 : CM_8BIT;
}

static void zz_FillRectMode(struct PrismBoard *b, ULONG dst, ULONG pitch, UBYTE bpp,
                        UWORD x, UWORD y, UWORD w, UWORD h, ULONG colour, UBYTE cm)
{
    struct ZZPriv *p = b->priv;
    Forbid();
    GD32(p, GD_OFFSET0, dst);
    GD16(p, GD_PITCH0, pitch >> 2);
    GD8(p, GD_U8USER0, cm);
    GD8(p, GD_MASK, 0xff);
    GD32(p, GD_RGB0, zz_colour(bpp, colour));
    GD32(p, GD_X0, PAIR(x, w));
    GD32(p, GD_Y0, PAIR(y, h));
    W16(b, REG_DMA_OP, OP_FILLRECT);
    Permit();
}

static void zz_FillRect(struct PrismBoard *b, ULONG dst, ULONG pitch, UBYTE bpp,
                        UWORD x, UWORD y, UWORD w, UWORD h, ULONG colour)
{
    zz_FillRectMode(b,dst,pitch,bpp,x,y,w,h,colour,zz_cm(bpp));
}

static void zz_CopyRect(struct PrismBoard *b, ULONG base, ULONG pitch, UBYTE bpp,
                        UWORD sx, UWORD sy, UWORD dx, UWORD dy, UWORD w, UWORD h)
{
    struct ZZPriv *p = b->priv;
    Forbid();
    GD32(p, GD_OFFSET0, base);
    GD32(p, GD_OFFSET1, base);
    GD16(p, GD_PITCH0, pitch >> 2);
    GD16(p, GD_X2, sx);
    GD16(p, GD_Y2, sy);
    GD32(p, GD_X0, PAIR(dx, w));
    GD32(p, GD_Y0, PAIR(dy, h));
    GD8(p, GD_U8USER0, zz_cm(bpp));
    GD8(p, GD_MASK, 0xff);
    W16(b, REG_DMA_OP, OP_COPYRECT);
    Permit();
}

static void zz_CopyBetweenOp(struct PrismBoard *b, ULONG src, ULONG spitch, ULONG dst,
                             ULONG dpitch, UWORD wbytes, UWORD h, UBYTE minterm)
{
    struct ZZPriv *p = b->priv;
    /* byte geometry: an 8-bit copy whose rows start at src and dst. The
     * firmware wants row starts on 32-bit words; the odd bytes become x. */
    Forbid();
    GD32(p, GD_OFFSET1, src & ~3UL);
    GD32(p, GD_PITCH0, PAIR(dpitch >> 2, spitch >> 2));
    GD16(p, GD_X2, src & 3);
    GD16(p, GD_Y2, 0);
    GD32(p, GD_OFFSET0, dst & ~3UL);
    GD32(p, GD_X0, PAIR(dst & 3, wbytes));
    GD32(p, GD_Y0, PAIR(0, h));
    GD8(p, GD_MINTERM, minterm);
    GD8(p, GD_U8USER0, CM_8BIT);
    W16(b, REG_DMA_OP, OP_COPY_NOMASK);
    Permit();
}

static void zz_CopyBetween(struct PrismBoard *b, ULONG src, ULONG spitch, ULONG dst,
                           ULONG dpitch, UWORD wbytes, UWORD h)
{
    struct ZZPriv *p = b->priv;
    /* the plain copy as before the rop (the friend-blit path the speed gate times) */
    Forbid();
    GD32(p, GD_OFFSET1, src & ~3UL);
    GD32(p, GD_PITCH0, PAIR(dpitch >> 2, spitch >> 2));
    GD16(p, GD_X2, src & 3);
    GD16(p, GD_Y2, 0);
    GD32(p, GD_OFFSET0, dst & ~3UL);
    GD32(p, GD_X0, PAIR(dst & 3, wbytes));
    GD32(p, GD_Y0, PAIR(0, h));
    GD8(p, GD_MINTERM, MINTERM_SRC);
    GD8(p, GD_U8USER0, CM_8BIT);
    W16(b, REG_DMA_OP, OP_COPY_NOMASK);
    Permit();
}

static BOOL zz_ExpandRectMode(struct PrismBoard *b, ULONG dst, ULONG pitch, UBYTE bpp,
                          UWORD x, UWORD y, UWORD w, UWORD h, const UBYTE *tmpl, ULONG mod,
                          ULONG fg, ULONG bg, BOOL transparent, UBYTE cm)
{
    struct ZZPriv *p = b->priv;
    ULONG rb = ((ULONG)w + 7) >> 3, n;
    volatile UBYTE *o = b->regs + TEMPLATE_OFFSET;
    UWORD r;

    if (!w || !h || bpp == 3 || rb * h > TEMPLATE_MAX)
        return FALSE;
    Forbid();
    for (r = 0; r < h; r++, tmpl += mod) {
        /* longs where they fit: each card access costs the same */
        for (n = 0; n + 4 <= rb; n += 4, o += 4)
            *(volatile ULONG *)o = *(const ULONG *)(tmpl + n);
        for (; n < rb; n++)
            *o++ = tmpl[n];
    }
    GD32(p, GD_X0, PAIR(x, w));
    GD16(p, GD_X2, 0);
    GD32(p, GD_Y0, PAIR(y, h));
    GD16(p, GD_Y2, 0);
    GD32(p, GD_OFFSET0, dst);
    GD32(p, GD_OFFSET1, TEMPLATE_OFFSET - VRAM_OFFSET);
    GD32(p, GD_PITCH0, PAIR(pitch, rb));          /* this op: pitches in bytes */
    GD32(p, GD_RGB0, zz_colour(bpp, fg));
    GD32(p, GD_RGB1, zz_colour(bpp, bg));
    GD16(p, GD_U8USER0, (cm << 8) | (transparent ? 0 : 1));   /* cm; JAM1 : JAM2 */
    GD8(p, GD_MASK, 0xff);
    W16(b, REG_DMA_OP, OP_TEMPLATE);
    Permit();
    return TRUE;
}

static BOOL zz_ExpandRect(struct PrismBoard *b, ULONG dst, ULONG pitch, UBYTE bpp,
                          UWORD x, UWORD y, UWORD w, UWORD h, const UBYTE *tmpl, ULONG mod,
                          ULONG fg, ULONG bg, BOOL transparent)
{
    return zz_ExpandRectMode(b,dst,pitch,bpp,x,y,w,h,tmpl,mod,fg,bg,transparent,zz_cm(bpp));
}

/* The firmware's line: e starts at the seed, each step along the longer
 * axis adds S, and when e reaches L it takes L off and steps the shorter
 * axis. Seeded with L/2 that is round(i * S / L) with halves rounded up -
 * the native rule, and Prism's (see line_solid in render.c). */
static void zz_DrawLineMode(struct PrismBoard *b, ULONG dst, ULONG pitch, UBYTE bpp,
                        WORD x, WORD y, WORD dx, WORD dy, ULONG colour, UBYTE cm)
{
    struct ZZPriv *p = b->priv;
    UWORD ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
    UWORD len = ax >= ay ? ax : ay;

    Forbid();
    GD32(p, GD_OFFSET0, dst);
    GD16(p, GD_PITCH0, pitch >> 2);
    GD32(p, GD_U8USER0, (ULONG)cm << 24);         /* cm; JAM1; pattern offset 0; pad 0 */
    GD8(p, GD_MASK, 0xff);
    GD32(p, GD_RGB0, zz_colour(bpp, colour));
    GD32(p, GD_X0, PAIR(x, dx));
    GD32(p, GD_Y0, PAIR(y, dy));
    GD32(p, GD_USER0, PAIR(len, 0xffff));         /* length; solid */
    GD32(p, GD_USER2, PAIR(0, len >> 1));         /* halves round up */
    W16(b, REG_DMA_OP, OP_DRAWLINE);
    Permit();
}

static void zz_DrawLine(struct PrismBoard *b, ULONG dst, ULONG pitch, UBYTE bpp,
                        WORD x, WORD y, WORD dx, WORD dy, ULONG colour)
{
    zz_DrawLineMode(b,dst,pitch,bpp,x,y,dx,dy,colour,zz_cm(bpp));
}

static void zz_WaitBlit(struct PrismBoard *b)
{
    (void)R16(b, REG_DMA_OP);         /* commands are synchronous: a fence */
}

/* Storage limits apply to byte copies regardless of the pixel layout. */
static BOOL zz_storage(struct PrismBoard *b,const struct PrismSurface *s)
{
    return ((struct ZZPriv *)b->priv)->z3 &&
        !(s->pitch & 3) && s->pitch/4<=65535 &&
        s->offset<=b->vramSize && s->allocation<=b->vramSize-s->offset;
}
static BOOL zz_surface(struct PrismBoard *b,const struct PrismSurface *s)
{
    return zz_storage(b,s) && (s->format==PF_CLUT8 ||
        s->format==PF_RGB565BE || s->format==PF_RGB555BE || s->format==PF_BGRA32);
}
/* Fill, line and text colours arrive as raw 16-bit values, so 15-bit
 * surfaces use the 565 commands too: the bytes written are the same, and
 * that is the path tested on the hardware (zz_pan maps 555 to 565 alike). */
static UBYTE zz_surface_cm(const struct PrismSurface *s)
{
    return zz_cm(s->bpp);
}
static enum PrismResult zz_fill(struct PrismBoard *b,const struct PrismSurface *d,
    UWORD x,UWORD y,UWORD w,UWORD h,ULONG c)
{
    if(!zz_surface(b,d)) return PR_DECLINED;
    zz_FillRectMode(b,d->offset,d->pitch,d->bpp,x,y,w,h,c,zz_surface_cm(d));
    zz_WaitBlit(b);return PR_DONE;
}
static enum PrismResult zz_copy(struct PrismBoard *b,const struct PrismSurface *s,
    const struct PrismSurface *d,UWORD sx,UWORD sy,UWORD dx,UWORD dy,UWORD w,UWORD h)
{
    if(!zz_storage(b,s) || !zz_storage(b,d) ||
        ((ULONG)sx+w)*s->bpp>65535 || ((ULONG)dx+w)*d->bpp>65535)
        return PR_DECLINED;
    if(s->offset==d->offset && s->pitch==d->pitch)
        zz_CopyRect(b,s->offset,s->pitch,1,sx*s->bpp,sy,dx*d->bpp,dy,w*d->bpp,h);
    else if((ULONG)w*d->bpp<=65535 && (s->offset+s->allocation<=d->offset ||
                                                    d->offset+d->allocation<=s->offset))
        zz_CopyBetween(b,s->offset+(ULONG)sy*s->pitch+(ULONG)sx*s->bpp,s->pitch,
            d->offset+(ULONG)dy*d->pitch+(ULONG)dx*d->bpp,d->pitch,w*d->bpp,h);
    else return PR_DECLINED;
    zz_WaitBlit(b);return PR_DONE;
}
static enum PrismResult zz_expand(struct PrismBoard *b,const struct PrismSurface *d,
    UWORD x,UWORD y,UWORD w,UWORD h,const UBYTE *src,ULONG mod,ULONG fg,ULONG bg,BOOL tr)
{
    if(!zz_surface(b,d) || d->pitch>65535) return PR_DECLINED;
    if(!zz_ExpandRectMode(b,d->offset,d->pitch,d->bpp,x,y,w,h,src,mod,fg,bg,tr,zz_surface_cm(d)))
        return PR_DECLINED;
    zz_WaitBlit(b);return PR_DONE;
}
static enum PrismResult zz_line(struct PrismBoard *b,const struct PrismSurface *d,
    WORD x,WORD y,WORD dx,WORD dy,ULONG c)
{
    if(!zz_surface(b,d)) return PR_DECLINED;
    zz_DrawLineMode(b,d->offset,d->pitch,d->bpp,x,y,dx,dy,c,zz_surface_cm(d));
    zz_WaitBlit(b);return PR_DONE;
}
/* A minterm blit between two bitmaps (the core's rop): the firmware's
 * copy_rect_nomask with the minterm as Picasso96 numbers it (opcode =
 * BltBitMap minterm >> 4). As bytes, like zz_CopyBetween - every minterm is
 * bitwise, so 16-bit pixels come out the same. Only the minterms firmware
 * 2.8.1 computes as named: ONLYDST, INVERT, EOR, NEOR, checked with ropcheck
 * on a real ZZ9000 (A4000, 2026-10-23). Its NOR, NOTSRC, NAND, NOTONLYSRC,
 * NOTONLYDST and OR are wrong in gfx.h already, and ONLYSRC and AND, which
 * read right there, came out wrong on nearly every byte on the card - so all
 * of those stay on the CPU. EOR (6) is the one that matters: layers'
 * window-drag swap. */
static enum PrismResult zz_rop(struct PrismBoard *b,const struct PrismSurface *s,
    const struct PrismSurface *d,UWORD sx,UWORD sy,UWORD dx,UWORD dy,UWORD w,UWORD h,UBYTE mt)
{
    struct ZZPriv *p=b->priv;
    UBYTE op=(UBYTE)((mt>>4)&15);
    if(!(op==2 || op==5 || op==6 || op==9) || s->bpp!=d->bpp || d->bpp>2 ||
        !zz_storage(b,s) || !zz_storage(b,d) || (ULONG)w*d->bpp>65535 ||
        ((ULONG)sx+w)*s->bpp>65535 || ((ULONG)dx+w)*d->bpp>65535)
        return PR_DECLINED;
    zz_CopyBetweenOp(b,s->offset+(ULONG)sy*s->pitch+(ULONG)sx*s->bpp,s->pitch,
        d->offset+(ULONG)dy*d->pitch+(ULONG)dx*d->bpp,d->pitch,(UWORD)(w*d->bpp),h,op);
    zz_WaitBlit(b);
    GD8(p, GD_MINTERM, MINTERM_SRC);   /* the field stays as written: plain copies after */
    return PR_DONE;
}
static const struct PrismOps zz_ops={.fill=zz_fill,.copy=zz_copy,
    .expand=zz_expand,.line=zz_line,.rop=zz_rop};

/* ---- hardware cursor (Zorro III) ------------------------------------
 * The firmware cursor is 32x48 true-colour pixels; the image Prism hands
 * over is 64x64 with pens 0-3, so its top-left 32x48 is what shows. */
static void zz_CursorImage(struct PrismBoard *b, const UBYTE *img, const UBYTE *rgb)
{
    struct ZZPriv *p = b->priv;
    volatile UBYTE *o = b->regs + TEMPLATE_OFFSET;
    UWORD x, y;

    Forbid();
    for (y = 0; y < ZZ_CURSOR_H; y++)
        for (x = 0; x < ZZ_CURSOR_W; x++)
            *o++ = img[y * CURSOR_SIZE + x];
    for (x = 0; x < 9; x++)
        GD8(p, GD_CLUT1 + 3 + x, rgb[x]);         /* pens 1-3 */
    GD32(p, GD_OFFSET1, TEMPLATE_OFFSET);         /* this op: from the board base */
    GD32(p, GD_X0, PAIR(0, ZZ_CURSOR_W));
    GD32(p, GD_Y0, PAIR(0, ZZ_CURSOR_H));
    GD8(p, GD_U8OFFSET, 0);                       /* pen 0 = transparent */
    W16(b, REG_DMA_OP, OP_SPRITE_CLUT);
    Permit();
}

static void zz_CursorShow(struct PrismBoard *b, BOOL on)
{
    W16(b, REG_SPRITE_SHOW, on ? 1 : 2);
}

/* ---- video overlay (firmware OP_VIDEO_OVERLAY, BlitterStudio 2.8+) ------
 * The mailbox carries: offset[1] the YUV source in VRAM, pitch[1] its row,
 * x[2]/y[2] its size, x[0],y[0],x[1],y[1] the destination rectangle,
 * user[0] flags (1 colour key on, 2 active), u8_user[4] the YUV variant,
 * u8_user[5] the sub-command (0 set, 1 off) and u32_user[1] the key,
 * which the firmware reads raw (little-endian) and compares with the
 * screen's pixels as they lie in memory: the key bytes go in as stored,
 * right-aligned in the field. The firmware answers in u32_user[0]
 * (0 = ok); a firmware without the op leaves our sentinel there. */
#define GD_X2V       20
#define GD_Y2V       28
#define GD_U8_YUV    52
#define GD_U8_SUBCMD 53
#define GD_U32_0     60
#define GD_U32_1     64
#define GDR32(p, off) (*(volatile ULONG *)((p)->gfxdata + (off)))

static BOOL zz_OverlaySet(struct PrismBoard *b, const struct PrismOverlay *o)
{
    struct ZZPriv *p = b->priv;
    UBYTE variant;
    ULONG status;
    switch (o->srcFormat) {
    case PF_YUV_CGX:    variant = 0; break;
    case PF_YUV_P96:    variant = 1; break;
    case PF_YUV_P96PC:  variant = 2; break;
    case PF_YUV_P96PA:  variant = 3; break;
    case PF_YUV_P96PAPC: variant = 4; break;
    default: return FALSE;
    }
    if (o->screenBpp != 2 && o->screenBpp != 4)
        return FALSE;
    Forbid();
    GD32(p, GD_OFFSET1, o->srcOff);
    GD16(p, GD_PITCH1, o->srcPitch);
    GD16(p, GD_X2V, o->srcW);
    GD16(p, GD_Y2V, o->srcH);
    GD16(p, GD_X0, o->dstX);
    GD16(p, GD_Y0, o->dstY);
    GD16(p, GD_X1, o->dstW);
    GD16(p, GD_Y1, o->dstH);
    GD16(p, GD_USER0, (o->keyed ? 1 : 0) | 2);
    GD8(p, GD_U8_YUV, variant);
    GD8(p, GD_U8_SUBCMD, 0);
    GD32(p, GD_U32_0, 0xFFFFFFFFUL);              /* the sentinel: no answer */
    GD32(p, GD_U32_1, 0);
    if (o->screenBpp == 4) {
        GD8(p, GD_U32_1 + 0, o->key[0]); GD8(p, GD_U32_1 + 1, o->key[1]);
        GD8(p, GD_U32_1 + 2, o->key[2]); GD8(p, GD_U32_1 + 3, o->key[3]);
    } else {
        GD8(p, GD_U32_1 + 2, o->key[0]); GD8(p, GD_U32_1 + 3, o->key[1]);
    }
    z3_kick(b, OP_VIDEO_OVERLAY);
    (void)R16(b, REG_DMA_OP);                      /* synchronous: a fence */
    CacheClearU();
    status = GDR32(p, GD_U32_0);
    Permit();
    return status == 0;                           /* else: refused, or no such op (sentinel) */
}

/* Monitor power (POWERSAVE), firmware 2.7+: the card feature DPMS gates the
 * video formatter's syncs - 0 on, 1 standby, 2 suspend, 3 off. Prism's
 * level 1 (blank) has no ZZ9000 equivalent and becomes standby. */
static void zz_SetPower(struct PrismBoard *b, UBYTE level)
{
    static const UBYTE dpms[5] = { 0, 1, 1, 2, 3 };
    Forbid();
    W16(b, REG_USER1, FEATURE_DPMS);
    W16(b, REG_SET_FEATURE, dpms[level < 5 ? level : 4]);
    Permit();
}

static void zz_OverlayOff(struct PrismBoard *b)
{
    struct ZZPriv *p = b->priv;
    Forbid();
    GD8(p, GD_U8_SUBCMD, 1);
    GD32(p, GD_U32_0, 0xFFFFFFFFUL);
    z3_kick(b, OP_VIDEO_OVERLAY);
    (void)R16(b, REG_DMA_OP);
    Permit();
}

static void zz_CursorMove(struct PrismBoard *b, WORD x, WORD y)
{
    struct ZZPriv *p = b->priv;
    Forbid();
    GD16(p, GD_X0, x);
    GD16(p, GD_Y0, y);
    W16(b, REG_DMA_OP, OP_SPRITE_XY);
    Permit();
}

/* Firmware modeline ids, by output size. */
static const struct { UWORD w, h; UBYTE id; } zz_modes[] = {
    { 1280,  720,  0 }, {  800,  600,  1 }, {  640,  480,  2 },
    { 1024,  768,  3 }, { 1280, 1024,  4 }, { 1920, 1080,  5 },
    {  720,  576,  6 }, {  720,  480,  8 }, {  640,  512,  9 },
    { 1600, 1200, 10 }, { 2560, 1440, 11 }, {  640,  400, 16 },
    { 1920,  800, 17 },
};

static int zz_mode_id(UWORD w, UWORD h)
{
    int i;
    for (i = 0; i < (int)(sizeof(zz_modes) / sizeof(zz_modes[0])); i++)
        if (zz_modes[i].w == w && zz_modes[i].h == h)
            return zz_modes[i].id;
    return -1;
}

static void zz_pan(struct PrismBoard *b)
{
    struct ZZPriv *p = b->priv;
    /* 15-bit is panned as 565: same bytes per pixel */
    UBYTE cm = (p->colormode == CM_555) ? CM_565 : p->colormode;

    if (p->z3) {
        Forbid();
        GD32(p, GD_OFFSET0, p->panOffset);
        GD32(p, GD_X0, PAIR(0, p->width));
        GD16(p, GD_Y0, 0);
        GD8(p, GD_U8USER0, cm);
        z3_kick(b, OP_PAN);
        Permit();
    } else {
        W16(b, REG_X1, 0);
        W16(b, REG_Y1, 0);
        W16(b, REG_X2, p->width);
        W16(b, REG_COLORMODE, cm);
        W16(b, REG_PAN_HI, p->panOffset >> 16);
        W16(b, REG_PAN_LO, p->panOffset & 0xffff);
    }
}

/* Validate a mode without touching the card: returns the MODE register
 * value (or -1) and fills m->bytesPerRow and the firmware colour mode. */
static LONG zz_check(struct PrismMode *m, UBYTE *cmOut)
{
    UWORD ow = m->width, oh = m->height, scale = 0;
    int id;
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
    /* small modes are shown pixel-doubled inside the 2x output mode */
    if (m->width < 640 && m->height < 480) {
        ow = m->width * 2;
        oh = m->height * 2;
        scale = 3;
    }
    if ((id = zz_mode_id(ow, oh)) < 0)
        return -1;
    if (m->pitchPixels && (m->pitchPixels < m->width || m->pitchPixels > 8192))
        return -1;
    m->bytesPerRow = (ULONG)(m->pitchPixels ? m->pitchPixels : m->width) * bpp;
    *cmOut = cm;
    return id | (cm << 8) | (scale << 12);
}

static BOOL zz_CheckMode(struct PrismBoard *b, struct PrismMode *m)
{
    UBYTE cm;
    return zz_check(m, &cm) >= 0;
}

static BOOL zz_SetMode(struct PrismBoard *b, struct PrismMode *m)
{
    struct ZZPriv *p = b->priv;
    LONG mode;
    UBYTE cm;

    if ((mode = zz_check(m, &cm)) < 0)
        return FALSE;
    p->mode = mode;
    p->width = m->pitchPixels ? m->pitchPixels : m->width;   /* the pan width: the row's pixels */
    p->colormode = cm;
    W16(b, REG_CAPTURE, 0);
    W16(b, REG_MODE, p->mode);
    zz_pan(b);
    return TRUE;
}

static void zz_SetDisplayStart(struct PrismBoard *b, ULONG off)
{
    struct ZZPriv *p = b->priv;
    p->panOffset = off;
    if (p->width)              /* before the first mode set: just remember it */
        zz_pan(b);
}

static void zz_SetPalette(struct PrismBoard *b, UWORD first, UWORD count,
                          const UBYTE *rgb)
{
    struct ZZPriv *p = b->priv;
    UWORD i;

    if (first >= 256)
        return;
    if (first + count > 256)
        count = 256 - first;

    if (p->z3) {
        Forbid();
        for (i = 0; i < count * 3; i++)
            GD8(p, GD_CLUT1 + i, rgb[i]);
        GD32(p, GD_USER0, PAIR(first, count));
        GD8(p, GD_U8USER0, 0);           /* primary palette */
        z3_kick(b, OP_SET_PALETTE);
        Permit();
    } else {
        for (i = 0; i < count; i++, rgb += 3) {
            W16(b, REG_CX_DATA_HI, ((first + i) << 8) | rgb[0]);
            W16(b, REG_CX_DATA_LO, (rgb[1] << 8) | rgb[2]);
            W16(b, REG_CX_OP, 3);
            W16(b, REG_CX_OP, 0);
        }
    }
}

static void zz_SetSwitch(struct PrismBoard *b, BOOL rtg)
{
    struct ZZPriv *p = b->priv;

    if (rtg) {
        W16(b, REG_CAPTURE, 0);
        W16(b, REG_MODE, p->mode);
        zz_pan(b);
    } else {
        W16(b, REG_PAN_HI, p->capOffset >> 16);
        W16(b, REG_PAN_LO, p->capOffset & 0xffff);
        W16(b, REG_CAPTURE, 1);
    }
}

static void zz_WaitVBlank(struct PrismBoard *b)
{
    ULONG n;
    /* finish any blank we are in, then wait for the next one to start */
    for (n = 0; n < 200000 && R16(b, REG_VBLANK); n++) ;
    for (n = 0; n < 200000 && !R16(b, REG_VBLANK); n++) ;
}

/* Borrowing the card from Picasso96 (PrismTest): ZZ9000.card remembers the
 * bytes it last put in the mailbox header and skips writing them again, so
 * the header has to go back exactly as it was found. */
static void zz_SaveState(struct PrismBoard *b)
{
    struct ZZPriv *p = b->priv;
    UWORD i;
    if (p->z3)
        for (i = 0; i < GD_CLUT1; i++)
            p->saved[i] = *(volatile UBYTE *)(p->gfxdata + i);
}

static void zz_RestoreState(struct PrismBoard *b)
{
    struct ZZPriv *p = b->priv;
    UWORD i;
    if (p->z3)
        for (i = 0; i < GD_CLUT1; i++)
            GD8(p, i, p->saved[i]);
}

static void zz_Shutdown(struct PrismBoard *b)
{
    zz_SetSwitch(b, FALSE);
}

BOOL ZZ9000_Probe(struct PrismBoard *b)
{
    struct ConfigDev *cd;
    struct ZZPriv *p = &zzpriv;
    UWORD fw, caps, vmode;

    /* one PrismD does not drive yet (a second copy of this driver drives a
     * second board) */
    p->z3 = TRUE;
    if (!(cd = driver_find(NULL, ZZ_MFR, ZZ_PROD_Z3))) {
        p->z3 = FALSE;
        if (!(cd = driver_find(NULL, ZZ_MFR, ZZ_PROD_Z2)))
            return FALSE;
    }

    b->name      = p->z3 ? "ZZ9000 (Zorro III)" : "ZZ9000 (Zorro II)";
    b->configDev = cd;
    b->regs      = (volatile UBYTE *)cd->cd_BoardAddr;
    b->vram      = (UBYTE *)cd->cd_BoardAddr + VRAM_OFFSET;
    /* The native-video capture lands in card memory at CAPTURE_OFFSET (the
     * stock driver hands P96 everything up to the SDK heap and trusts it
     * to fill from the bottom). Prism pages bitmaps anywhere in its VRAM,
     * so it simply stops short of the capture buffer: 13.9 MB. */
    b->vramSize  = p->z3 ? VRAM_LIMIT : cd->cd_BoardSize - 0x40000;
    if (b->vramSize > VRAM_LIMIT)
        b->vramSize = VRAM_LIMIT;
    b->formats   = PF_BIT(PF_CLUT8) | PF_BIT(PF_RGB565BE) |
                   PF_BIT(PF_RGB555BE) | PF_BIT(PF_BGRA32);
    b->flags     = 0;
    b->maxWidth  = 2560;
    b->maxHeight = 1440;
    b->priv      = p;
    p->gfxdata   = (UBYTE *)cd->cd_BoardAddr + GFXDATA_OFFSET;

    fw = R16(b, REG_FW_VERSION);
    if (fw == 0 || fw == 0xffff || fw < 0x0200)
        return FALSE;            /* bad bus or firmware older than 2.0 */

    /* Where the pan has to point to show native video (as ZZ9000.card
     * 2.13 does it): the centred 1080p profiles, and 800x600 on firmware
     * that doesn't place the capture itself, use the old origin. Reads
     * only: the capture mode itself stays what ZZ9000.CFG made it. */
    caps = R16(b, REG_FW_CAPS);
    W16(b, REG_CFG_KEY, CFG_KEY_VCAP_MODE);
    vmode = R16(b, REG_CFG_KEY);
    if (!R16(b, REG_CFG_PRESENT))
        vmode = VMODE_800x600;
    if (vmode == VMODE_1080P_60 || vmode == VMODE_1080P_50 || vmode == VMODE_1080P_SYNC ||
        (vmode != VMODE_720x576 && !(caps & CAP_SCANOUT_ORIGIN)))
        p->capOffset = CAPTURE_OFF_OLD;
    else
        p->capOffset = CAPTURE_OFFSET;

    b->setMode         = zz_SetMode;
    b->checkMode       = zz_CheckMode;
    b->setDisplayStart = zz_SetDisplayStart;
    b->setPalette      = zz_SetPalette;
    b->overlaySet      = zz_OverlaySet;   /* refused by a firmware without the op */
    b->overlayOff      = zz_OverlayOff;
    if (fw >= 0x0207)                     /* DPMS: the monitor sleeps (else PrismD shows black) */
        b->setPower    = zz_SetPower;
    b->cursorW         = ZZ_CURSOR_W;     /* a bigger pointer goes to the software one */
    b->cursorH         = ZZ_CURSOR_H;
    b->setSwitch       = zz_SetSwitch;
    b->waitVBlank      = zz_WaitVBlank;
    b->shutdown        = zz_Shutdown;
    if (p->z3) {
        b->ops         = &zz_ops;
        b->flags       = PBF_BLIT_32 | PBF_HW_CURSOR;
        b->fillRect    = zz_FillRect;
        b->copyRect    = zz_CopyRect;
        b->copyBetween = zz_CopyBetween;
        b->expandRect  = zz_ExpandRect;
        b->waitBlit    = zz_WaitBlit;
        b->drawLine    = zz_DrawLine;
        b->cursorImage = zz_CursorImage;
        b->cursorShow  = zz_CursorShow;
        b->cursorMove  = zz_CursorMove;
    }
    b->saveState       = zz_SaveState;
    b->restoreState    = zz_RestoreState;
    return TRUE;
}

/* ---- as a loadable module (driver_module.c) --------------------------- */
#ifdef PRISM_DRIVER_MODULE
#include "driver_module.h"
BOOL driver_probe(struct PrismBoard *b, const struct PrismDriverConfig *config)
{
    return ZZ9000_Probe(b);
}
void driver_retain(void)
{
}
#endif
