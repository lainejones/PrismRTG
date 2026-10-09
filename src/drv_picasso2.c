/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laine Jones */
/*
 * drv_picasso2.c - Village Tronic Picasso II / II+ and GBAPII++ driver.
 *
 * The board is two Zorro II autoconfig devices: 2 MB of linear VRAM and a
 * 64 KB register window whose offsets are the chip's ISA I/O port numbers.
 * Picasso II / II+ (GD5426 / GD5428) answer as 2167/11 + 2167/12; the
 * GBAPII++ remake (GD5434) as 2167/16 + 2167/17. The chip is told apart by
 * CR27 and the few registers that differ are branched on.
 *
 * Mode setting is classic VGA plus the Cirrus extensions: SR7 depth and
 * linear-aperture base, SR0E/SR1E pixel clock, SRF DRAM control, CR1A/CR1B
 * extension bits and the hidden DAC register for direct colour.
 *
 * Written from docs/cirrus-picasso2-hw.md (section numbers below refer to it).
 * Verified in WinUAE's Picasso II+ (GD5428); the GD5434 paths are untested
 * until the A2000 is unpacked.
 */
#include <exec/types.h>
#include <stdio.h>
#include <libraries/configvars.h>
#include <proto/exec.h>
#include <proto/expansion.h>
#include <string.h>
#include "boardops.h"

extern struct ExpansionBase *ExpansionBase;


/* VGA ports (= offsets in the register window, §1.2) */
#define ATTR_W     0x3c0
#define ATTR_R     0x3c1
#define MISC_W     0x3c2
#define SEQ_I      0x3c4
#define SEQ_D      0x3c5
#define DAC_MASK   0x3c6
#define DAC_WI     0x3c8
#define MISC_R     0x3cc
#define GRC_I      0x3ce
#define GRC_D      0x3cf
#define CRT_I      0x3d4
#define CRT_D      0x3d5
#define STATUS1    0x3da
/* 0x3C7 and 0x3C9 must go through the +0xFFF odd-port alias (§1.2) */
/* DAC read index and data: the Picasso II reaches these odd ports through
 * its +$FFF alias (§1.2); the Piccolo and Spectrum boards at the plain
 * port numbers. Set per board in the probe. */
#define DAC_RI     (((struct P2Priv *)b->priv)->dacRI)
#define DAC_D      (((struct P2Priv *)b->priv)->dacD)

/* how a board switches the monitor between the Amiga and the card */
#define PASS_PICASSO 0        /* byte writes at +$A000 / +$B000           */
#define PASS_4F6F    1        /* +$8000: $6F card, $4F Amiga, $1F wake    */
#define PASS_BIT5    2        /* +$8000 bit 5: 1 card, 0 Amiga; bit 4 wake */

/* chip ids (CR27, §2.2) */
#define CL_GD5426  0x90
#define CL_GD5428  0x98
#define CL_GD5434  0xa8

#define XTAL_KHZ   14318
#define VCO_MIN    28636            /* VCO stability window (§2.3)        */
#define VCO_MAX    111000

struct P2Priv {
    struct ConfigDev *memDev;
    UBYTE  chip;              /* CR27                                     */
    BOOL   is5434;
    BOOL   failed;
    UBYTE  aperture;          /* SR7 bits 7-4: linear window base (§1.3)  */
    UWORD  dacRI, dacD;       /* DAC read-index and data port offsets     */
    UBYTE  pass;              /* monitor switch: PASS_* below             */
    BOOL   swapRB;            /* red and blue DAC lines exchanged         */
    char   name[40];
    UBYTE  sr7;               /* SR7 depth bits for the current mode      */
    UBYTE  hdr;               /* hidden DAC value for the current mode    */
    UWORD  width;
    ULONG  bpr;
    UBYTE  bpp;               /* bytes per pixel of the current mode      */
    ULONG  scratch;           /* VRAM offset of the driver scratch area   */
    UBYTE  xpad;              /* text expansion: template rows padded to
                                 1 or 4 bytes; 0 = not tested, 0xff = off  */
    BOOL   xtransp;           /* transparent (JAM1) expansion works       */
    BOOL   x24;               /* opaque expansion works at 24 bits        */
    BOOL   x32;               /* 5434: fills + opaque expansion at 32 bits */
    BOOL   x32t;              /* ... and transparent expansion            */
    /* found at the first mode set (p2_Calibrate*) */
    BOOL   wordio;            /* index+data go out as one 16-bit write    */
    BOOL   srOff;             /* GR1/GR11 left set don't touch CPU writes */
    BOOL   fillPitch;         /* pattern fills need the source pitch set  */
    ULONG  xdiag;             /* why transparency failed (see the dump)    */
    /* saved state of whoever had the card before us */
    BOOL   saved;
    UBYTE  sMisc, sSeq[0x20], sCrt[0x40], sGrc[0x40], sAttr[0x15];
    UBYTE  sHdr, sDacMask, sPal[256 * 3];
};

static struct P2Priv p2priv;

/* Load the palette (and the pointer's colours) blue, green, red.
 *
 * On the Piccolo, Piccolo SD64 and Spectrum the red and blue outputs are
 * exchanged, which is why their 15/16/24/32-bit formats are the BGR ones.
 * For the 256-colour palette the sources disagree: NetBSD's grf_cl loads
 * B,G,R on all three, Linux's cirrusfb on the Piccolo and Spectrum but not
 * the SD64, and in WinUAE (the only place this driver has run on these
 * boards) all three show the right colours with plain R,G,B. So R,G,B is
 * the default, and PALETTE=BGR in Prism.prefs turns this on for a real
 * board whose 256-colour screens come out with red and blue exchanged. */
UBYTE Picasso2_ClutBGR;

static void p2_CalibrateExpand(struct PrismBoard *b);
static void gr_forget(void);

/* ---- register access (bytes only: no long writes to the window) ----- */

static inline void outb(struct PrismBoard *b, UWORD port, UBYTE v)
{
    *(volatile UBYTE *)(b->regs + port) = v;
}

static inline UBYTE inb(struct PrismBoard *b, UWORD port)
{
    return *(volatile UBYTE *)(b->regs + port);
}

static void wseq(struct PrismBoard *b, UBYTE i, UBYTE v) { outb(b, SEQ_I, i); outb(b, SEQ_D, v); }
static UBYTE rseq(struct PrismBoard *b, UBYTE i)         { outb(b, SEQ_I, i); return inb(b, SEQ_D); }
static void wcrt(struct PrismBoard *b, UBYTE i, UBYTE v) { outb(b, CRT_I, i); outb(b, CRT_D, v); }
static UBYTE rcrt(struct PrismBoard *b, UBYTE i)         { outb(b, CRT_I, i); return inb(b, CRT_D); }
static void wgrc(struct PrismBoard *b, UBYTE i, UBYTE v) { outb(b, GRC_I, i); outb(b, GRC_D, v); }
static UBYTE rgrc(struct PrismBoard *b, UBYTE i)         { outb(b, GRC_I, i); return inb(b, GRC_D); }

/* The attribute controller shares one port for index and data; reading
 * STATUS1 resets its flip-flop to "index". Bit 5 of the index (PAS) must be
 * set again afterwards or the screen stays blank. */
static void wattr(struct PrismBoard *b, UBYTE i, UBYTE v)
{
    (void)inb(b, STATUS1);
    outb(b, ATTR_W, i);
    outb(b, ATTR_W, v);
}

static UBYTE rattr(struct PrismBoard *b, UBYTE i)
{
    (void)inb(b, STATUS1);
    outb(b, ATTR_W, i);
    return inb(b, ATTR_R);
}

static void attr_on(struct PrismBoard *b)
{
    (void)inb(b, STATUS1);
    outb(b, ATTR_W, 0x20);
}

/* Hidden DAC register (§2.2): 0x3C6 = 0, read 0x3C8 to reset the counter,
 * four reads of 0x3C6, then the next 0x3C6 access is the HDR. Reading
 * 0x3C8 closes it again; the pixel mask goes back to 0xFF. */
static void open_hdr(struct PrismBoard *b)
{
    outb(b, DAC_MASK, 0x00);
    (void)inb(b, DAC_WI);
    (void)inb(b, DAC_MASK); (void)inb(b, DAC_MASK);
    (void)inb(b, DAC_MASK); (void)inb(b, DAC_MASK);
}

static void whdr(struct PrismBoard *b, UBYTE v)
{
    open_hdr(b);
    outb(b, DAC_MASK, v);
    (void)inb(b, DAC_WI);
    outb(b, DAC_MASK, 0xff);
}

static UBYTE rhdr(struct PrismBoard *b)
{
    UBYTE v;
    open_hdr(b);
    v = inb(b, DAC_MASK);
    (void)inb(b, DAC_WI);
    outb(b, DAC_MASK, 0xff);
    return v;
}

/* ---- timings -------------------------------------------------------- */

struct P2Timing {
    UWORD w, h;
    UBYTE hz;                        /* vertical refresh                 */
    ULONG khz;                       /* pixel clock                      */
    UWORD hss, hse, ht;              /* h sync start/end, total (pixels)  */
    UWORD vss, vse, vt;              /* v sync start/end, total (lines)   */
    UBYTE hpos, vpos;                /* sync polarity, 1 = positive       */
    UBYTE dbl;                       /* low resolution: every pixel and line
                                        shown twice (see write_crtc)      */
};

/* VESA / VGA standard timings. The first entry for a size is its default
 * (refresh 0); the others are picked by PrismPrefs' refresh setting. */
static const struct P2Timing p2_timings[] = {
    {  640, 480, 60, 25175,  656,  752,  800,  490,  492,  525, 0, 0 },
    {  640, 480, 72, 31500,  664,  704,  832,  489,  492,  520, 0, 0 },
    {  640, 480, 75, 31500,  656,  720,  840,  481,  484,  500, 0, 0 },
    {  640, 400, 70, 25175,  656,  752,  800,  412,  414,  449, 0, 1 },
    /* 320x200 and 320x240: the 640x400 / 640x480 timings at half the pixel
     * clock (so half the pixels a line) with every line scanned twice (CR9
     * bit 7; vertical values stay in monitor lines). The monitor sees the
     * ordinary 31 kHz 640x400 / 640x480 signal. (Linux's cirrusfb makes its
     * low modes the same way. SR1 bit 3, "dot clock / 2", was tried first:
     * WinUAE's chip ignores it in packed-pixel modes.) */
    {  320, 200, 70, 12588,  328,  376,  400,  412,  414,  449, 0, 1, 1 },
    {  320, 240, 60, 12588,  328,  376,  400,  490,  492,  525, 0, 0, 1 },
    {  800, 600, 60, 40000,  840,  968, 1056,  601,  605,  628, 1, 1 },
    {  800, 600, 72, 50000,  856,  976, 1040,  637,  643,  666, 1, 1 },
    {  800, 600, 75, 49500,  816,  896, 1056,  601,  604,  625, 1, 1 },
    { 1024, 768, 60, 65000, 1048, 1184, 1344,  771,  777,  806, 0, 0 },
    { 1024, 768, 70, 75000, 1048, 1184, 1328,  771,  777,  806, 0, 0 },
    { 1024, 768, 75, 78750, 1040, 1136, 1312,  769,  772,  800, 1, 1 },
};

/* Pixel clock limits per bytes/pixel (§2.3): the 5426/28 manage 86 MHz at
 * 8-bit and 57.3 MHz at 15/16-bit. The 5434 is faster; stay at 86 MHz for
 * it until measured. At 24-bit EVERY chip runs VCLK at 3x the pixel clock
 * (SR7 = x5): the real GD5434 does too - with VCLK = the pixel clock a
 * monitor on the GBAPII++ reported "timing off" (2026-10-03), and Linux's
 * cirrusfb triples it for the 543x as well. So the limit there is the
 * synthesiser's: VCO_MAX / 3 on the 5434, 86 / 3 on the 5426/28. */
static ULONG max_khz(struct P2Priv *p, UBYTE bpp)
{
    if (bpp == 1)
        return 86000;
    if (bpp == 3)
        return p->is5434 ? VCO_MAX / 3 : 86000 / 3;
    if (bpp == 4)
        return 57300;                /* 5434 only; conservative */
    return p->is5434 ? 86000 : 57300;
}

/* VCLK per pixel clock */
static UBYTE clk_mul(struct P2Priv *p, UBYTE bpp)
{
    return bpp == 3 ? 3 : 1;
}

/* Closest VCLK = XTAL * N / D / (1 + P) with the VCO = XTAL * N / D kept in
 * its stable window. Returns SR0E (N) in *n and SR1E (D << 1 | P) in *d. */
static void p2_clock(ULONG khz, UBYTE *n, UBYTE *d)
{
    ULONG best = 0xffffffff;
    /* below half the VCO's documented minimum (the low-resolution modes)
     * the VCO has to run slower than that; Linux's cirrusfb does the same */
    ULONG vmin = (khz * 2 < VCO_MIN) ? khz : VCO_MIN;
    UWORD nn, dd, pp;

    *n = 0x4a; *d = 0x2b;            /* 25.227 MHz fallback             */
    for (pp = 0; pp < 2; pp++)
        for (dd = 1; dd <= 0x1f; dd++)
            for (nn = 0x10; nn <= 0x7f; nn++) {
                ULONG vco = (XTAL_KHZ * nn) / dd;
                ULONG f = vco >> pp;
                ULONG err = f > khz ? f - khz : khz - f;
                if (vco < vmin || vco > VCO_MAX)
                    continue;
                if (err < best) {
                    best = err;
                    *n = nn;
                    *d = (dd << 1) | pp;
                }
            }
}

/* Does a CPU write through the Zorro window reach VRAM? Used to find the
 * SR7 aperture nibble, which on the GBAPII++ depends on its ISA wiring. */
static BOOL vram_works(struct PrismBoard *b)
{
    volatile UBYTE *t = b->vram + ((struct P2Priv *)b->priv)->scratch + 0x100;
    t[0] = 0x5a; t[1] = 0xa5;
    return t[0] == 0x5a && t[1] == 0xa5;
}

/* ---- driver entry points ------------------------------------------ */

static void write_crtc(struct PrismBoard *b, const struct P2Timing *t, ULONG pitch8)
{
    UWORD hd, hs, he, ht, hbs, hbe, vd, vs, ve, vtot, vbs, vbe;

    /* §2.4: horizontal values in character clocks (8 pixels) for every
     * depth; vertical in lines */
    ht  = t->ht / 8;   hd = t->w / 8;   hs = t->hss / 8;  he = t->hse / 8;
    hbs = hd;          hbe = ht - 1;
    vtot = t->vt;      vd = t->dbl ? t->h * 2 : t->h;
    vs = t->vss;       ve = t->vse;
    vbs = vd;          vbe = vtot - 1;

    wcrt(b, 0x11, 0x20);                          /* unprotect CR0-7      */
    wcrt(b, 0x00, ht - 5);
    wcrt(b, 0x01, hd - 1);
    wcrt(b, 0x02, hbs);
    wcrt(b, 0x03, 0x80 | (hbe & 0x1f));
    wcrt(b, 0x04, hs);
    wcrt(b, 0x05, ((hbe & 0x20) << 2) | (he & 0x1f));
    wcrt(b, 0x06, (vtot - 2) & 0xff);
    wcrt(b, 0x07, (((vtot - 2) >> 8) & 0x01) | (((vd - 1) >> 7) & 0x02) |
                  ((vs >> 6) & 0x04) | ((vbs >> 5) & 0x08) | 0x10 |
                  (((vtot - 2) >> 4) & 0x20) | (((vd - 1) >> 3) & 0x40) |
                  ((vs >> 2) & 0x80));
    wcrt(b, 0x08, 0x00);
    wcrt(b, 0x09, 0x40 | ((vbs >> 4) & 0x20) | (t->dbl ? 0x80 : 0));
    wcrt(b, 0x0a, 0x20);                          /* text cursor off      */
    wcrt(b, 0x0b, 0x00);
    wcrt(b, 0x0e, 0x00);
    wcrt(b, 0x0f, 0x00);
    wcrt(b, 0x10, vs & 0xff);
    wcrt(b, 0x11, 0x20 | (ve & 0x0f));            /* vint off, unprotected */
    wcrt(b, 0x12, (vd - 1) & 0xff);
    wcrt(b, 0x13, pitch8 & 0xff);
    wcrt(b, 0x14, 0x00);
    wcrt(b, 0x15, vbs & 0xff);
    wcrt(b, 0x16, vbe & 0xff);
    wcrt(b, 0x17, 0xe3);
    wcrt(b, 0x18, 0xff);
    wcrt(b, 0x19, 0x00);
    wcrt(b, 0x1a, (((hbe >> 6) & 3) << 4) | (((vbe >> 8) & 3) << 6));
    /* CR1B: ext. address wrap + blank from display enable, pitch bit 8;
     * start-address bits are kept (SetDisplayStart owns them) */
    wcrt(b, 0x1b, 0x22 | ((pitch8 >> 4) & 0x10) | (rcrt(b, 0x1b) & 0x0d));
}

/* Validate a mode without touching the hardware. Fills m->bytesPerRow and,
 * for SetMode, the timing, SR7 depth bits, HDR value and bytes per pixel. */
static const struct P2Timing *p2_check(struct PrismBoard *b, struct PrismMode *m,
                                       UBYTE *sr7, UBYTE *hdr, UBYTE *bppOut)
{
    struct P2Priv *p = b->priv;
    const struct P2Timing *t = NULL;
    UWORD i;
    UBYTE bpp;

    for (i = 0; i < sizeof(p2_timings) / sizeof(p2_timings[0]); i++)
        if (p2_timings[i].w == m->width && p2_timings[i].h == m->height &&
            (!m->refresh || p2_timings[i].hz == m->refresh)) {
            t = &p2_timings[i];
            break;
        }
    if (!t)
        return NULL;

    /* SR7 bit 0 = packed pixel; bits 2-1 = 00 8-bit, 11 16-bit at one
     * VCLK per pixel. HDR 0xC1 = 565, 0xD0 = 555 (§2.2). VRAM keeps the
     * 68k byte order, so 16-bit pixels are the PC (little-endian) formats. */
    switch (m->format) {
    case PF_CLUT8:    bpp = 1; *sr7 = 0x01; *hdr = 0x00; break;
    /* the BGR / RGB24 / RGBA names are the same chip modes on a board
     * whose red and blue lines are exchanged (b->formats has only the
     * names that are true for the board) */
    case PF_BGR565LE:
    case PF_RGB565LE: bpp = 2; *sr7 = 0x07; *hdr = 0xc1; break;
    case PF_BGR555LE:
    case PF_RGB555LE: bpp = 2; *sr7 = 0x07; *hdr = 0xd0; break;
    case PF_RGB24:
    case PF_BGR24:    bpp = 3; *sr7 = 0x05; *hdr = 0xc5; break; /* B,G,R bytes */
    case PF_ARGB32:
    case PF_BGRA32:   /* GD5434 only: 4 bytes B,G,R,x at VCLK = pixel clock
                       * (SR7 = x9, as Linux's cirrusfb and Picasso96 use) */
        if (!p->is5434)
            return NULL;
        bpp = 4; *sr7 = 0x09; *hdr = 0xc5; break;
    default:          return NULL;
    }
    if (t->khz > max_khz(p, bpp) || (t->dbl && bpp == 3))
        return NULL;                 /* (low resolution at 24 bits: untried) */
    m->bytesPerRow = (ULONG)m->width * bpp;
    if ((ULONG)m->bytesPerRow * m->height > b->vramSize)
        return NULL;
    m->refresh = t->hz;
    *bppOut = bpp;
    return t;
}

static BOOL p2_CheckMode(struct PrismBoard *b, struct PrismMode *m)
{
    UBYTE sr7, hdr, bpp;
    return p2_check(b, m, &sr7, &hdr, &bpp) != NULL;
}

static BOOL p2_SetMode(struct PrismBoard *b, struct PrismMode *m)
{
    struct P2Priv *p = b->priv;
    const struct P2Timing *t;
    UWORD i;
    ULONG pitch8;
    UBYTE n, d, bpp;

    if (!(t = p2_check(b, m, &p->sr7, &p->hdr, &bpp)))
        return FALSE;
    p->bpp = bpp;
    p->width = m->width;
    p->bpr = m->bytesPerRow;
    pitch8 = m->bytesPerRow >> 3;
    /* At 32 bits the GD5434 counts the CRTC offset in 16-byte units: with
     * pitch / 8 the real card showed every other row (picture at half
     * height, then the scratch area, then VRAM wrapping round - photo,
     * 2026-10-03). Nothing in the 86Box/QEMU/Linux sources says so. */
    if (bpp == 4)
        pitch8 >>= 1;

    p2_clock(t->khz * clk_mul(p, bpp), &n, &d);

    /* §2.5 */
    outb(b, 0x3c3, 0x01);                         /* wake a cold board    */
    wseq(b, 0x06, 0x12);                          /* unlock extensions    */
    wseq(b, 0x01, 0x21);                          /* screen off           */
    wseq(b, 0x00, 0x01);                          /* synchronous reset    */
    outb(b, MISC_W, 0x2f | (t->hpos ? 0 : 0x40) | (t->vpos ? 0 : 0x80));
                                                  /* colour I/O, RAM on,
                                                     VCLK3, VGA polarity
                                                     (1 = negative)       */
    wseq(b, 0x02, 0xff);
    wseq(b, 0x03, 0x00);
    wseq(b, 0x04, 0x0e);                          /* chain-4, ext memory  */
    if (p->is5434) {
        wseq(b, 0x0f, b->vramSize > 0x100000 ? 0x38 : 0x30); /* 64-bit DRAM */
        wseq(b, 0x16, 0x5a);
        wseq(b, 0x1f, 0x1c);                      /* MCLK 50.1 MHz        */
    } else {
        wseq(b, 0x0f, b->vramSize > 0x100000 ? 0xb0 : 0x30); /* 2nd bank  */
        wseq(b, 0x16, 0x0a);
        wseq(b, 0x1f, 0x22);                      /* MCLK 60.85 MHz       */
    }
    wseq(b, 0x18, 0x02);
    wseq(b, 0x12, 0x04);                          /* hw cursor off        */
    wseq(b, 0x13, 0x3c);
    wseq(b, 0x07, p->aperture | p->sr7);          /* depth + linear base  */
    wseq(b, 0x0e, n);                             /* VCLK3 numerator      */
    wseq(b, 0x1e, d);                             /* denominator/postdiv  */
    wseq(b, 0x00, 0x03);                          /* run                  */

    /* "registers sometimes need writing twice for the settings to take" */
    write_crtc(b, t, pitch8);
    write_crtc(b, t, pitch8);

    wgrc(b, 0x00, 0x00);
    wgrc(b, 0x01, 0x00);
    wgrc(b, 0x02, 0x00);
    wgrc(b, 0x03, 0x00);
    wgrc(b, 0x04, 0x00);
    wgrc(b, 0x05, 0x40);                          /* 256-colour shift     */
    wgrc(b, 0x06, 0x01);                          /* graphics             */
    wgrc(b, 0x07, 0x0f);
    wgrc(b, 0x08, 0xff);
    wgrc(b, 0x09, 0x00);                          /* no banking           */
    wgrc(b, 0x0a, 0x00);
    wgrc(b, 0x0b, p->is5434 ? 0x20 : 0x28);       /* 5434: bit 3 breaks BLT */

    for (i = 0; i < 16; i++)
        wattr(b, i, i);
    wattr(b, 0x10, 0x01);                         /* graphics             */
    wattr(b, 0x11, 0x00);
    wattr(b, 0x12, 0x0f);
    wattr(b, 0x13, 0x00);
    wattr(b, 0x14, 0x00);
    attr_on(b);

    whdr(b, p->hdr);

    ((struct P2Priv *)b->priv)->failed = TRUE;
    wgrc(b, 0x31, 0x04);                          /* BLT reset            */
    wgrc(b, 0x31, 0x00);

    /* Find the aperture nibble once: 0x2_ on the Picasso II; the GBAPII++
     * may be wired like the Piccolo SD64 (0x8_) instead (§8.5). */
    if (!vram_works(b) && p->aperture == 0x20) {
        p->aperture = 0x80;
        wseq(b, 0x07, p->aperture | p->sr7);
        if (!vram_works(b)) {
            p->aperture = 0x20;
            wseq(b, 0x07, p->aperture | p->sr7);
        }
    }

    /* solid 8x8 mono pattern for blitter fills */
    for (i = 0; i < 8; i++)
        b->vram[p->scratch + i] = 0xff;

    gr_forget();
    /* the first mode set finds out how the blitter behaves here */
    if (!p->xpad)
        p2_CalibrateExpand(b);

    wseq(b, 0x01, 0x01);                          /* screen on            */
    return TRUE;
}

/* ---- BitBLT engine (GR20-GR32) --------------------------------------
 *
 * GR20/21 width-1 in bytes, GR22/23 height-1, GR24/25 dest pitch,
 * GR26/27 source pitch, GR28-2A dest address, GR2C-2E source address,
 * GR30 mode, GR31 start (bit 1) / busy (bit 0), GR32 raster op.
 * GR0/GR1 (+ GR10/GR11 high bytes) double as the colour-expansion
 * background/foreground colours; they are also VGA set/reset registers,
 * so they go back to 0 after every blit or CPU writes get mangled.
 */
#define BLT_BACKWARDS   0x01
#define BLT_16BPP       0x10
#define BLT_32BPP       0x30              /* GD5434: 4-byte pixels          */
#define BLT_PATTERN     0x40
#define BLT_EXPAND      0x80
#define BLT_TRANSP      0x08              /* expansion: 0 bits left alone */
#define BLT_SYSSRC      0x04              /* source data written by the CPU */
#define ROP_SRC         0x0d

/* A register access costs ~0.8 us on a 50 MHz 68030 A2000 (measured,
 * tools/iotime), and a blit used to make ~50 of them. So:
 *  - index and data go out as one 16-bit write (index in the high byte)
 *    where the board passes that on - tested at the first mode set;
 *  - a register that already holds the value isn't written (grShadow);
 *  - the start write leaves the index on GR31, so waiting is reads only;
 *  - every operation returns with the blitter idle, so none waits first.
 */
static UWORD grShadow[0x40];

static void gr_forget(void)
{
    UWORD i;
    for (i = 0; i < 0x40; i++)
        grShadow[i] = 0xffff;
}

static inline void gr_out(struct PrismBoard *b, struct P2Priv *p, UBYTE i, UBYTE v)
{
    if (p->wordio) {
        *(volatile UWORD *)(b->regs + GRC_I) = ((UWORD)i << 8) | v;
    } else {
        outb(b, GRC_I, i);
        outb(b, GRC_D, v);
    }
}

static inline void gw(struct PrismBoard *b, struct P2Priv *p, UBYTE i, UBYTE v)
{
    if (grShadow[i] != v) {
        gr_out(b, p, i, v);
        grShadow[i] = v;
    }
}

static BOOL p2_WaitBlitFor(struct PrismBoard *b, ULONG n);

static void p2_WaitBlit(struct PrismBoard *b)
{
    if (!p2_WaitBlitFor(b, 1000000))
        b->flags |= PBF_ACCEL_BROKEN;
}

/* start the blit and wait for it */
static inline void blt_run(struct PrismBoard *b, struct P2Priv *p)
{
    volatile UBYTE *d = b->regs + GRC_D;
    ULONG n;
    gr_out(b, p, 0x31, 0x02);
    /* poll here (GR31 is selected already); p2_WaitBlitFor only for the
     * reset when it never finishes */
    for (n = 0; n < 1000000 && (*d & 0x01); n++) ;
    if ((*d & 0x01) && !p2_WaitBlitFor(b, 0)) {
        /* a fill or copy that never finished: the chip is wedged. The
         * core's direct path sees the flag and redraws on the CPU. */
        p->failed = TRUE;
        b->flags |= PBF_ACCEL_BROKEN;
    }
}

static inline void blt_dst(struct PrismBoard *b, struct P2Priv *p, UWORD wbytes, UWORD h,
                           ULONG dpitch, ULONG dst)
{
    gw(b, p, 0x20, (wbytes - 1) & 0xff);
    gw(b, p, 0x21, (wbytes - 1) >> 8);
    gw(b, p, 0x22, (h - 1) & 0xff);
    gw(b, p, 0x23, (h - 1) >> 8);
    gw(b, p, 0x24, dpitch & 0xff);
    gw(b, p, 0x25, dpitch >> 8);
    /* addresses are always written: the chip (WinUAE's at least) counts
     * in these registers during a blit */
    gr_out(b, p, 0x28, dst & 0xff);
    gr_out(b, p, 0x29, (dst >> 8) & 0xff);
    gr_out(b, p, 0x2a, (dst >> 16) & 0x1f);
}

static inline void blt_srcaddr(struct PrismBoard *b, struct P2Priv *p, ULONG src)
{
    gr_out(b, p, 0x2c, src & 0xff);
    gr_out(b, p, 0x2d, (src >> 8) & 0xff);
    gr_out(b, p, 0x2e, (src >> 16) & 0x1f);
}

static inline void blt_srcpitch(struct PrismBoard *b, struct P2Priv *p, ULONG spitch)
{
    gw(b, p, 0x26, spitch & 0xff);
    gw(b, p, 0x27, spitch >> 8);
}

/* GR1/GR11 are also VGA "enable set/reset": left non-zero they mangle CPU
 * writes to VRAM on the 5426/28. The 5429+ switch that off in packed-pixel
 * modes (srOff, tested), so there the colours can stay for the next blit. */
static inline void blt_done(struct PrismBoard *b, struct P2Priv *p)
{
    if (!p->srOff) {
        gw(b, p, 0x01, 0);
        gw(b, p, 0x11, 0);
    }
}

/* BLT foreground (and background) colour, as the 68k stores the pixel:
 * GR1/GR0 take the byte at the lowest VRAM address, then GR11/GR10, and
 * on the 5434 GR13/GR12 and GR15/GR14 for bytes 2 and 3. */
/* GD5434: colour bytes 2 and 3 (GR12/GR14 background, GR13/GR15
 * foreground) back to 0 for an 8- or 16-bit operation. Left at a 32-bit
 * pixel's bytes they got into 8-bit transparent expansion: after a 32-bit
 * screen had drawn text, 8-bit JAM1 text showed the key colour in every
 * fourth pixel column (A2000, 2026-10-04, PrismBench after SysSpeed's
 * true colour tests). */
static inline void blt_hi0(struct PrismBoard *b, struct P2Priv *p, UBYTE reg)
{
    if (p->is5434) {
        gw(b, p, reg, 0);
        gw(b, p, reg + 2, 0);
    }
}

static inline void blt_fg(struct PrismBoard *b, struct P2Priv *p, UBYTE bpp, ULONG c)
{
    if (bpp == 4) {
        gw(b, p, 0x01, c >> 24);         gw(b, p, 0x11, (c >> 16) & 0xff);
        gw(b, p, 0x13, (c >> 8) & 0xff); gw(b, p, 0x15, c & 0xff);
    } else if (bpp == 2) {
        gw(b, p, 0x01, (c >> 8) & 0xff); gw(b, p, 0x11, c & 0xff);
        blt_hi0(b, p, 0x13);
    } else {
        /* 8-bit: the second colour byte must be 0 as well. Left at a 16-bit
         * pixel's second byte, transparent (JAM1) expansion drops some of
         * the foreground pixels - found on the A2000 by drawing 8-bit text
         * after a 16-bit screen had drawn any (2026-10-04). */
        gw(b, p, 0x01, c & 0xff);
        gw(b, p, 0x11, 0);
        blt_hi0(b, p, 0x13);
    }
}

static inline void blt_bg(struct PrismBoard *b, struct P2Priv *p, UBYTE bpp, ULONG c)
{
    if (bpp == 4) {
        gw(b, p, 0x00, c >> 24);         gw(b, p, 0x10, (c >> 16) & 0xff);
        gw(b, p, 0x12, (c >> 8) & 0xff); gw(b, p, 0x14, c & 0xff);
    } else if (bpp == 2) {
        gw(b, p, 0x00, (c >> 8) & 0xff); gw(b, p, 0x10, c & 0xff);
        blt_hi0(b, p, 0x12);
    } else {
        gw(b, p, 0x00, c & 0xff);
        gw(b, p, 0x10, 0);
        blt_hi0(b, p, 0x12);
    }
}

static inline UBYTE blt_depth(UBYTE bpp)
{
    return bpp == 4 ? BLT_32BPP : bpp == 2 ? BLT_16BPP : 0;
}

static void p2_FillRect(struct PrismBoard *b, ULONG base, ULONG pitch, UBYTE bpp,
                        UWORD x, UWORD y, UWORD w, UWORD h, ULONG colour)
{
    struct P2Priv *p = b->priv;
    ULONG dst = base + (ULONG)y * pitch + (ULONG)x * bpp;

    if (!w || !h)
        return;
    /* colour-expand the all-ones pattern in the foreground colour. GR1 is
     * the byte at the lower VRAM address; the colour comes in as the 68k
     * sees it, so a 16-bit pixel's first byte is bits 15-8. */
    blt_fg(b, p, bpp, colour);
    blt_dst(b, p, w * bpp, h, pitch, dst);
    blt_srcaddr(b, p, p->scratch);
    if (p->fillPitch)
        blt_srcpitch(b, p, pitch);
    gw(b, p, 0x30, BLT_PATTERN | BLT_EXPAND | blt_depth(bpp));
    gw(b, p, 0x32, ROP_SRC);
    blt_run(b, p);
    blt_done(b, p);
}

static void p2_CopyRect(struct PrismBoard *b, ULONG base, ULONG pitch, UBYTE bpp,
                        UWORD sx, UWORD sy, UWORD dx, UWORD dy, UWORD w, UWORD h)
{
    struct P2Priv *p = b->priv;
    ULONG wb = (ULONG)w * bpp;
    ULONG src = base + (ULONG)sy * pitch + (ULONG)sx * bpp;
    ULONG dst = base + (ULONG)dy * pitch + (ULONG)dx * bpp;
    UBYTE mode = 0;                  /* plain byte copy: depth only
                                        matters for colour expansion */

    if (!w || !h)
        return;
    /* overlapping, destination after source: run backwards from the
     * last byte of each rectangle */
    if (dst > src) {
        src += (ULONG)(h - 1) * pitch + wb - 1;
        dst += (ULONG)(h - 1) * pitch + wb - 1;
        mode |= BLT_BACKWARDS;
    }
    blt_dst(b, p, wb, h, pitch, dst);
    blt_srcaddr(b, p, src);
    blt_srcpitch(b, p, pitch);
    gw(b, p, 0x30, mode);
    gw(b, p, 0x32, ROP_SRC);
    blt_run(b, p);
}

static void p2_CopyBetween(struct PrismBoard *b, ULONG src, ULONG spitch, ULONG dst,
                           ULONG dpitch, UWORD wbytes, UWORD h)
{
    struct P2Priv *p = b->priv;
    if (!wbytes || !h)
        return;
    blt_dst(b, p, wbytes, h, dpitch, dst);
    blt_srcaddr(b, p, src);
    blt_srcpitch(b, p, spitch);
    gw(b, p, 0x30, 0);
    gw(b, p, 0x32, ROP_SRC);
    blt_run(b, p);
}

/* ---- text: CPU-fed colour expansion --------------------------------- */

/* Wait for the blitter at most `n` polls; if it is still busy (left
 * waiting for more source data) reset it. TRUE = finished normally. */
static BOOL p2_WaitBlitFor(struct PrismBoard *b, ULONG n)
{
    volatile UBYTE *d = b->regs + GRC_D;
    outb(b, GRC_I, 0x31);
    while (n-- && (*d & 0x01)) ;
    if (!(*d & 0x01))
        return TRUE;
    ((struct P2Priv *)b->priv)->failed = TRUE;
    b->faults++;                                  /* the core's direct paths look */
    wgrc(b, 0x31, 0x04);                          /* BLT reset            */
    wgrc(b, 0x31, 0x00);
    gr_forget();
    return FALSE;
}

/* System-to-screen colour expansion: the BLT takes its 1-bit source from
 * CPU writes into the VRAM window. The stream is written into the scratch
 * area, so any surplus lands somewhere harmless. */
#define ROP_CLEAR_SRC   0x50              /* dst = dst AND NOT src          */
#define ROP_OR          0x6d              /* dst = dst OR src               */
static UBYTE xrop = ROP_SRC;              /* expand()'s raster operation    */

static BOOL expand(struct PrismBoard *b, UBYTE pad, ULONG dst, ULONG pitch, UBYTE bpp,
                   UWORD w, UWORD h, const UBYTE *tmpl, ULONG mod, ULONG fg, ULONG bg,
                   BOOL transparent, ULONG wait)
{
    struct P2Priv *p = b->priv;
    ULONG rowBytes = (w + 7) >> 3, padded = (rowBytes + pad - 1) / pad * pad;
    ULONG total = ((padded * h) + 3) & ~3UL, i, y, acc = 0;
    UBYTE n = 0;
    volatile ULONG *port = (volatile ULONG *)(b->vram + p->scratch + 0x8000);

    /* the stream goes into the scratch area: at most 28 KB of it */
    if (!w || !h || total > 0x7000 || bpp == 3)
        return FALSE;

    /* Transparent: the chip still expands 0 bits to the background colour
     * and then drops pixels equal to the key in GR34/35. Use a background
     * that can't be the foreground (its inverse) as the key too, as the
     * XFree86 Cirrus driver does. (WinUAE just skips 0 bits.) */
    if (transparent) {
        /* the key is 16 bits wide: at 32 bits it is tried against the
         * pixel's first two bytes (the calibration says if that works) */
        bg = bpp == 4 ? ~fg : bpp == 2 ? (~fg & 0xffff) : (~fg & 0xff);
        if (bpp == 4)      { gw(b, p, 0x34, bg >> 24);         gw(b, p, 0x35, (bg >> 16) & 0xff); }
        else if (bpp == 2) { gw(b, p, 0x34, (bg >> 8) & 0xff); gw(b, p, 0x35, bg & 0xff); }
        else               { gw(b, p, 0x34, bg & 0xff);        gw(b, p, 0x35, 0); }
    }
    blt_fg(b, p, bpp, fg);
    blt_bg(b, p, bpp, bg);
    blt_dst(b, p, w * bpp, h, pitch, dst);
    blt_srcaddr(b, p, p->scratch);                /* unused; as fills leave it */
    blt_srcpitch(b, p, 0);
    gw(b, p, 0x30, BLT_EXPAND | BLT_SYSSRC | blt_depth(bpp) |
                   (transparent ? BLT_TRANSP : 0));
    gw(b, p, 0x32, xrop);
    gr_out(b, p, 0x31, 0x02);
    /* the template's rows straight out of the caller's buffer, gathered
     * into longs on the way (each row padded to `pad` bytes, the whole to
     * a long) */
    if (pad == 1 && mod == rowBytes) {
        /* contiguous rows (topaz text): a straight long copy (68020+:
         * the source may be unaligned; the template has 3 spare bytes) */
        const ULONG *src = (const ULONG *)tmpl;
        for (i = total / 4; i; i--)
            *port++ = *src++;
        h = 0;
    }
    for (y = 0; y < h; y++, tmpl += mod) {
        for (i = 0; i < padded; i++) {
            acc = (acc << 8) | (i < rowBytes ? tmpl[i] : 0);
            if (++n == 4) {
                *port++ = acc;
                n = 0;
            }
        }
    }
    if (n)
        *port = acc << (8 * (4 - n));
    {
        /* done as soon as the last long is in; the index is still GR31 */
        volatile UBYTE *d = b->regs + GRC_D;
        i = (*d & 0x01) ? p2_WaitBlitFor(b, wait) : TRUE;
    }
    blt_done(b, p);
    return i;
}

static BOOL p2_ExpandRect(struct PrismBoard *b, ULONG base, ULONG pitch, UBYTE bpp,
                          UWORD x, UWORD y, UWORD w, UWORD h, const UBYTE *tmpl, ULONG mod,
                          ULONG fg, ULONG bg, BOOL transparent)
{
    struct P2Priv *p = b->priv;
    ULONG dst = base + (ULONG)y * pitch + (ULONG)x * bpp;
    BOOL ok;

    if (p->xpad == 0 || p->xpad == 0xff || (bpp == 3 && !p->x24) || (bpp == 4 && !p->x32))
        return FALSE;
    /* Transparent (JAM1). The chip's own way - expand with a background
     * that is also the colour key - is right on the real GD5434 only when
     * every byte of the foreground colour is below $80 at 8 and 16 bits
     * (measured with PrismTest XTEST on the A2000: white, bright red and
     * every pen from 128 up left stripes of the key colour); at 32 bits it
     * is right for every colour. Any other colour is drawn in two opaque
     * passes, which work for all of them: clear the set bits (dst AND NOT
     * src, with an all-ones foreground), then OR the colour in. */
    if (transparent &&
        (bpp == 4 ? !p->x32t
                  : (!p->xtransp || (fg & (bpp == 2 ? 0x8080UL : 0x80UL))))) {
        ULONG ones = bpp == 4 ? 0xffffffffUL : bpp == 2 ? 0xffffUL : 0xffUL;
        xrop = ROP_CLEAR_SRC;
        ok = expand(b, p->xpad, dst, pitch, bpp, w, h, tmpl, mod, ones, 0, FALSE, 1000000);
        xrop = ROP_OR;
        ok = ok && expand(b, p->xpad, dst, pitch, bpp, w, h, tmpl, mod, fg, 0, FALSE, 1000000);
        xrop = ROP_SRC;
    } else {
        ok = expand(b, p->xpad, dst, pitch, bpp, w, h, tmpl, mod, fg, bg, transparent, 1000000);
    }
    if (!ok) {
        p->xpad = 0xff;                           /* stuck: never again   */
        return FALSE;
    }
    return TRUE;
}

/* Which row padding does this chip want, and does transparency work? Try
 * each on a small template in the scratch area and read it back. */
static void p2_CalibrateExpand(struct PrismBoard *b)
{
    static const UBYTE t[4][3] = {
        { 0xa5, 0x3c, 0xff }, { 0x01, 0x80, 0x5a }, { 0xff, 0x00, 0x81 }, { 0x6d, 0xb6, 0xdb },
    };
    struct P2Priv *p = b->priv;
    ULONG d = p->scratch + 0x1000, pitch = 64;
    UBYTE pads[2] = { 1, 4 }, k, bpp;
    WORD x, y;

    /* 1. do 16-bit index+data writes arrive? (GR0 = BLT background) */
    p->wordio = FALSE;
    wgrc(b, 0x00, 0x11);
    *(volatile UWORD *)(b->regs + GRC_I) = 0x00a5;
    if (rgrc(b, 0x00) == 0xa5) {
        *(volatile UWORD *)(b->regs + GRC_I) = 0x005a;
        p->wordio = (rgrc(b, 0x00) == 0x5a);
    }
    wgrc(b, 0x00, 0x00);

    /* 2. do GR1/GR11 (BLT foreground = VGA enable set/reset) left set
     *    change what the CPU writes to VRAM? */
    wgrc(b, 0x01, 0xff);
    wgrc(b, 0x11, 0xff);
    b->vram[d] = 0x5a;
    b->vram[d + 1] = 0xa5;
    *(volatile UWORD *)(b->vram + d + 2) = 0x1234;
    p->srOff = (b->vram[d] == 0x5a && b->vram[d + 1] == 0xa5 &&
                *(volatile UWORD *)(b->vram + d + 2) == 0x1234);
    wgrc(b, 0x01, 0x00);
    wgrc(b, 0x11, 0x00);
    gr_forget();

    p->fillPitch = TRUE;
    p->xpad = 0xff;
    p->xtransp = FALSE;
    for (k = 0; k < 2 && p->xpad == 0xff; k++) {
        BOOL ok = TRUE;
        for (bpp = 1; bpp <= 2 && ok; bpp++) {
            ULONG fg = bpp == 2 ? 0x1234 : 0x11, bg = bpp == 2 ? 0x5678 : 0x22;
            memset(b->vram + d, 0x33, pitch * 4);
            ok = expand(b, pads[k], d, pitch, bpp, 21, 4, &t[0][0], 3, fg, bg, FALSE, 20000);
            for (y = 0; y < 4 && ok; y++)
                for (x = 0; x < 21 && ok; x++) {
                    BOOL bit = (t[y][x >> 3] >> (7 - (x & 7))) & 1;
                    ULONG v = bpp == 2 ? *(UWORD *)(b->vram + d + y * pitch + x * 2)
                                       : b->vram[d + y * pitch + x];
                    if (v != (bit ? fg : bg))
                        ok = FALSE;
                }
        }
        if (ok)
            p->xpad = pads[k];
    }
    if (p->xpad != 0xff) {
        /* transparency: clear bits must leave the 0x33 underneath */
        p->xtransp = TRUE;
        for (bpp = 1; bpp <= 2 && p->xtransp; bpp++) {
            ULONG fg = bpp == 2 ? 0x1234 : 0x11, under = bpp == 2 ? 0x3333 : 0x33;
            memset(b->vram + d, 0x33, pitch * 4);
            if (!expand(b, p->xpad, d, pitch, bpp, 21, 4, &t[0][0], 3, fg, 0, TRUE, 20000)) {
                p->xtransp = FALSE;
                p->xdiag = 0xff000000UL | ((ULONG)bpp << 16);    /* blit stuck */
            }
            for (y = 0; y < 4 && p->xtransp; y++)
                for (x = 0; x < 21 && p->xtransp; x++) {
                    BOOL bit = (t[y][x >> 3] >> (7 - (x & 7))) & 1;
                    ULONG v = bpp == 2 ? *(UWORD *)(b->vram + d + y * pitch + x * 2)
                                       : b->vram[d + y * pitch + x];
                    if (v != (bit ? fg : under)) {
                        p->xtransp = FALSE;
                        /* bpp, x, y and what was there, for the dump */
                        p->xdiag = ((ULONG)bpp << 28) | ((ULONG)x << 20) | ((ULONG)y << 16) |
                                   (v & 0xffff);
                    }
                }
        }
    }
    /* No 24-bit attempt: Linux's cirrusfb says the 543x can't expand at
     * 24 bits, and in WinUAE a refused 24-bit expansion left the chip
     * swallowing ordinary VRAM writes as blitter data. */
    p->x24 = FALSE;
    wgrc(b, 0x31, 0x04);                          /* BLT reset            */
    wgrc(b, 0x31, 0x00);
    gr_forget();

    /* 3. does a pattern fill come out right with the source pitch left at
     *    0 (as text expansion leaves it)? Then fills needn't set it. */
    {
        BOOL ok = TRUE;
        p->fillPitch = FALSE;
        blt_srcpitch(b, p, 0);
        memset(b->vram + d, 0x33, pitch * 6);
        p2_FillRect(b, d, pitch, 1, 3, 1, 21, 4, 0x77);
        for (y = 0; y < 6 && ok; y++)
            for (x = 0; x < 32 && ok; x++) {
                UBYTE want = (y >= 1 && y < 5 && x >= 3 && x < 24) ? 0x77 : 0x33;
                if (b->vram[d + y * pitch + x] != want)
                    ok = FALSE;
            }
        if (!ok) {
            wgrc(b, 0x31, 0x04);
            wgrc(b, 0x31, 0x00);
            gr_forget();
            p->fillPitch = TRUE;
        }
    }

    /* 4. GD5434: the same three things at 32 bits a pixel - a fill, opaque
     *    expansion, transparent expansion (the colour key is only 16 bits
     *    wide there; whether that still works is simply tried). */
    p->x32 = p->x32t = FALSE;
    if (p->is5434 && p->xpad != 0xff) {
        ULONG fg = 0x12345678UL, bg = 0x9abcdef0UL, under = 0x33333333UL, v;
        ULONG pitch4 = 128;
        BOOL ok = TRUE;

        memset(b->vram + d, 0x33, pitch4 * 6);
        p2_FillRect(b, d, pitch4, 4, 3, 1, 21, 4, fg);
        for (y = 0; y < 6 && ok; y++)
            for (x = 0; x < 32 && ok; x++) {
                v = *(ULONG *)(b->vram + d + y * pitch4 + x * 4);
                if (v != ((y >= 1 && y < 5 && x >= 3 && x < 24) ? fg : under))
                    ok = FALSE;
            }
        if (ok) {
            memset(b->vram + d, 0x33, pitch4 * 4);
            ok = expand(b, p->xpad, d, pitch4, 4, 21, 4, &t[0][0], 3, fg, bg, FALSE, 20000);
            for (y = 0; y < 4 && ok; y++)
                for (x = 0; x < 21 && ok; x++) {
                    BOOL bit = (t[y][x >> 3] >> (7 - (x & 7))) & 1;
                    if (*(ULONG *)(b->vram + d + y * pitch4 + x * 4) != (bit ? fg : bg))
                        ok = FALSE;
                }
        }
        p->x32 = ok;
        if (ok) {
            memset(b->vram + d, 0x33, pitch4 * 4);
            ok = expand(b, p->xpad, d, pitch4, 4, 21, 4, &t[0][0], 3, fg, 0, TRUE, 20000);
            for (y = 0; y < 4 && ok; y++)
                for (x = 0; x < 21 && ok; x++) {
                    BOOL bit = (t[y][x >> 3] >> (7 - (x & 7))) & 1;
                    if (*(ULONG *)(b->vram + d + y * pitch4 + x * 4) != (bit ? fg : under))
                        ok = FALSE;
                }
            p->x32t = ok;
        }
        wgrc(b, 0x31, 0x04);                      /* BLT reset, whatever happened */
        wgrc(b, 0x31, 0x00);
        gr_forget();
        if (p->x32)
            b->flags |= PBF_BLIT_32;
    }
}

/* PrismTest XTEST: JAM1 text on the real chip, read back, at this mode's
 * pixel size: the chip's own transparency (key = NOT fg), and the two-pass
 * way (clear the set bits, then OR the colour in) built from two opaque
 * expansions. Prints what fails. */
static ULONG xt_get(const UBYTE *q, UBYTE bpp)
{
    return bpp == 1 ? q[0] : bpp == 2 ? *(const UWORD *)q : *(const ULONG *)q;
}

void Picasso2_TranspTest(struct PrismBoard *b, UBYTE bpp)
{
    static const UBYTE t[4][3] = {
        { 0xa5, 0x3c, 0xff }, { 0x01, 0x80, 0x5a }, { 0xff, 0x00, 0x81 }, { 0x6d, 0xb6, 0xdb },
    };
    static const ULONG fgs[] = { 0x08f808f8, 0xffffffff, 0xe847e847, 0x08280828, 0x1f641f64,
                                 0x00000000, 0x80008000, 0x00800080, 0x7f7f7f7f, 0x80808080,
                                 0x12345678, 0x9abcdef0, 0x55aa55aa };
    struct P2Priv *p = b->priv;
    ULONG d = p->scratch + 0x1000, pitch = 128, mask, under, ones;
    UWORD i, k;
    WORD x, y;

    if (bpp == 3)
        return;
    mask = bpp == 1 ? 0xff : bpp == 2 ? 0xffff : 0xffffffffUL;
    under = 0x18181818UL & mask;
    ones = mask;
    for (k = 0; k < 2; k++) {
        printf("%u-bit %s:", bpp * 8, k ? "two-pass" : "chip key");
        for (i = 0; i < sizeof(fgs) / sizeof(fgs[0]); i++) {
            ULONG fg = fgs[i] & mask, got = 0, want = 0;
            UWORD bad = 0;
            WORD fx = -1, fy = -1;
            BOOL ok;
            memset(b->vram + d, 0x18, pitch * 4);
            if (k == 0) {
                ok = expand(b, p->xpad, d, pitch, bpp, 21, 4, &t[0][0], 3, fg, 0, TRUE, 20000);
            } else {
                xrop = ROP_CLEAR_SRC;
                ok = expand(b, p->xpad, d, pitch, bpp, 21, 4, &t[0][0], 3, ones, 0, FALSE, 20000);
                xrop = ROP_OR;
                ok = ok && expand(b, p->xpad, d, pitch, bpp, 21, 4, &t[0][0], 3, fg, 0, FALSE, 20000);
                xrop = ROP_SRC;
            }
            if (!ok) {
                printf(" %08lx:STUCK", (unsigned long)fg);
                continue;
            }
            for (y = 0; y < 4; y++)
                for (x = 0; x < 21; x++) {
                    BOOL bit = (t[y][x >> 3] >> (7 - (x & 7))) & 1;
                    ULONG v = xt_get(b->vram + d + y * pitch + x * bpp, bpp);
                    ULONG w = bit ? fg : under;
                    if (v != w && !bad++) { fx = x; fy = y; got = v; want = w; }
                }
            if (bad)
                printf(" %lx:%u(@%d,%d %lx/%lx)", (unsigned long)fg, bad, fx, fy,
                       (unsigned long)got, (unsigned long)want);
            else
                printf(" %lx:ok", (unsigned long)fg);
        }
        printf("\n");
    }
}

UBYTE Picasso2_TextExpand(struct PrismBoard *b, BOOL *transparent)
{
    struct P2Priv *p = b->priv;
    *transparent = p->xtransp;
    return p->xpad;
}

static void p2_SetDisplayStart(struct PrismBoard *b, ULONG off)
{
    /* §4: start address in 4-byte units. CR0C/CR0D = bits 15-0, CR1B
     * bits 0/2/3 = bits 16/17/18, CR1D bit 7 = bit 19 (5434 only) */
    struct P2Priv *p = b->priv;
    ULONG a = off >> 2;
    UBYTE x = rcrt(b, 0x1b) & ~0x0d;

    wcrt(b, 0x0c, (a >> 8) & 0xff);
    wcrt(b, 0x0d, a & 0xff);
    wcrt(b, 0x1b, x | ((a >> 16) & 0x01) | ((a >> 15) & 0x0c));
    if (p->is5434)
        wcrt(b, 0x1d, (rcrt(b, 0x1d) & 0x7f) | ((a >> 12) & 0x80));
}

static void p2_SetPalette(struct PrismBoard *b, UWORD first, UWORD count,
                          const UBYTE *rgb)
{
    UWORD i;
    outb(b, DAC_WI, first);
    if (Picasso2_ClutBGR) {
        /* the palette loaded blue first: see Picasso2_ClutBGR */
        for (i = 0; i < count; i++, rgb += 3) {
            outb(b, DAC_D, rgb[2] >> 2);
            outb(b, DAC_D, rgb[1] >> 2);
            outb(b, DAC_D, rgb[0] >> 2);
        }
        return;
    }
    for (i = 0; i < count * 3; i++)
        outb(b, DAC_D, rgb[i] >> 2);              /* 6-bit DAC, R,G,B     */
}

static void p2_SetSwitch(struct PrismBoard *b, BOOL rtg)
{
    struct P2Priv *p = b->priv;
    volatile UBYTE *sw = b->regs + 0x8000;

    if (p->pass == PASS_4F6F) {
        /* Spectrum, Piccolo SD64: a control byte at +$8000 */
        *sw = rtg ? 0x6f : 0x4f;
        return;
    }
    if (p->pass == PASS_BIT5) {
        /* Piccolo: bit 5 of the byte at +$8000 */
        *sw = rtg ? (*sw | 0x20) : (*sw & 0xdf);
        return;
    }
    /* §1.6: a byte write at +$A000 routes the card's output to the
     * monitor, +$B000 gives it back to the Amiga. (+$8000/+$9000 on boards
     * that ignore A13; the GBAPII++ aliases both.) */
    *(volatile UBYTE *)(b->regs + (rtg ? 0xa000 : 0xb000)) = 0;
}

static void p2_WaitVBlank(struct PrismBoard *b)
{
    ULONG n;
    for (n = 0; n < 200000 && (inb(b, STATUS1) & 0x08); n++) ;
    for (n = 0; n < 200000 && !(inb(b, STATUS1) & 0x08); n++) ;
}

static void p2_SaveState(struct PrismBoard *b)
{
    struct P2Priv *p = b->priv;
    UWORD i;

    Disable();
    p->sMisc = inb(b, MISC_R);
    for (i = 0; i < sizeof(p->sSeq); i++)  p->sSeq[i] = rseq(b, i);
    for (i = 0; i < sizeof(p->sCrt); i++)  p->sCrt[i] = rcrt(b, i);
    for (i = 0; i < sizeof(p->sGrc); i++)  p->sGrc[i] = rgrc(b, i);
    for (i = 0; i < sizeof(p->sAttr); i++) p->sAttr[i] = rattr(b, i);
    attr_on(b);
    p->sDacMask = inb(b, DAC_MASK);
    p->sHdr = rhdr(b);
    outb(b, DAC_RI, 0);
    for (i = 0; i < sizeof(p->sPal); i++)  p->sPal[i] = inb(b, DAC_D);
    Enable();
    p->saved = TRUE;
}

static void p2_RestoreState(struct PrismBoard *b)
{
    struct P2Priv *p = b->priv;
    UWORD i;

    if (!p->saved)
        return;
    Disable();
    wseq(b, 0x06, 0x12);
    wseq(b, 0x00, 0x01);
    outb(b, MISC_W, p->sMisc);
    for (i = 1; i < sizeof(p->sSeq); i++)
        if (i != 0x06)
            wseq(b, i, p->sSeq[i]);
    wseq(b, 0x00, p->sSeq[0] | 0x03);
    wcrt(b, 0x11, p->sCrt[0x11] & 0x7f);
    for (i = 0; i < sizeof(p->sCrt); i++)
        if (i != 0x11 && i != 0x27)               /* CR27 = read-only ID  */
            wcrt(b, i, p->sCrt[i]);
    wcrt(b, 0x11, p->sCrt[0x11]);
    for (i = 0; i < sizeof(p->sGrc); i++)
        if (i != 0x31)                            /* don't start a blit   */
            wgrc(b, i, p->sGrc[i]);
    for (i = 0; i < sizeof(p->sAttr); i++)
        wattr(b, i, p->sAttr[i]);
    attr_on(b);
    whdr(b, p->sHdr);
    outb(b, DAC_MASK, p->sDacMask);
    outb(b, DAC_WI, 0);
    for (i = 0; i < sizeof(p->sPal); i++)
        outb(b, DAC_D, p->sPal[i]);
    wseq(b, 0x06, p->sSeq[0x06] == 0x12 ? 0x12 : 0x0f);
    Enable();
    gr_forget();
}

/* ---- hardware cursor (§6) -------------------------------------------
 *
 * 64x64, two 1-bit planes, pattern at VRAM end - 1 KB (SR13 = 0x3C).
 * Per row: 8 bytes plane 0, then 8 bytes plane 1. A pixel (p0,p1) is
 * (0,0) transparent, (0,1) cursor colour 0, (1,1) colour 1, (1,0) invert.
 * Only two real colours: Amiga pens 1 and 2 get them, pen 3 takes the
 * nearer of the two.
 */
static void p2_CursorImage(struct PrismBoard *b, const UBYTE *img, const UBYTE *rgb)
{
    struct P2Priv *p = b->priv;
    UBYTE *pat = b->vram + p->memDev->cd_BoardSize - 0x400;
    UBYTE map3, x, y, i;
    LONG d1 = 0, d2 = 0;

    for (i = 0; i < 3; i++) {
        LONG a = (LONG)rgb[6 + i] - rgb[i], c = (LONG)rgb[6 + i] - rgb[3 + i];
        d1 += a * a;
        d2 += c * c;
    }
    map3 = (d1 <= d2) ? 1 : 2;

    for (y = 0; y < CURSOR_SIZE; y++) {
        UBYTE *row = pat + y * 16;
        for (x = 0; x < 8; x++) {
            UBYTE p0 = 0, p1 = 0, bit;
            for (bit = 0; bit < 8; bit++) {
                UBYTE v = img[y * CURSOR_SIZE + x * 8 + bit];
                if (v == 3) v = map3;
                if (v == 1) p1 |= 0x80 >> bit;                     /* colour 0 */
                else if (v == 2) { p0 |= 0x80 >> bit; p1 |= 0x80 >> bit; } /* colour 1 */
            }
            row[x] = p0;
            row[8 + x] = p1;
        }
    }
    /* cursor colours live in hidden DAC entries reached with SR12 bit 1 */
    wseq(b, 0x12, rseq(b, 0x12) | 0x02);
    {
        UBYTE r0 = Picasso2_ClutBGR ? 2 : 0;                     /* B,G,R or R,G,B */
        outb(b, DAC_WI, 0x00);
        outb(b, DAC_D, rgb[r0] >> 2); outb(b, DAC_D, rgb[1] >> 2);
        outb(b, DAC_D, rgb[2 - r0] >> 2);
        outb(b, DAC_WI, 0x0f);
        outb(b, DAC_D, rgb[3 + r0] >> 2); outb(b, DAC_D, rgb[4] >> 2);
        outb(b, DAC_D, rgb[5 - r0] >> 2);
    }
    wseq(b, 0x12, rseq(b, 0x12) & ~0x02);
}

static void p2_CursorShow(struct PrismBoard *b, BOOL on)
{
    wseq(b, 0x13, 0x3c);
    wseq(b, 0x12, on ? 0x05 : 0x04);          /* 64x64, enable bit       */
}

static void p2_CursorMove(struct PrismBoard *b, WORD x, WORD y)
{
    /* low 3 bits ride in the index byte */
    outb(b, SEQ_I, 0x10 | ((x & 7) << 5));
    outb(b, SEQ_D, (x >> 3) & 0xff);
    outb(b, SEQ_I, 0x11 | ((y & 7) << 5));
    outb(b, SEQ_D, (y >> 3) & 0xff);
}

static void p2_Shutdown(struct PrismBoard *b)
{
    p2_SetSwitch(b, FALSE);
}

UBYTE Picasso2_ChipID(struct PrismBoard *b)
{
    return ((struct P2Priv *)b->priv)->chip;
}

/* A cold chip is in mono I/O mode (Misc bit 0 = 0) and its CRTC is not
 * reachable at 0x3D4. Switch to colour I/O first, as NetBSD does; a card
 * another RTG system set up is in colour mode already. */
static UBYTE read_chip_id(struct PrismBoard *b)
{
    UBYTE misc = inb(b, MISC_R);
    if (!(misc & 0x01))
        outb(b, MISC_W, misc | 0x01);
    return rcrt(b, 0x27);
}

UWORD Picasso2_LastID;      /* 0x100 | CR27 when a probe rejected the chip */

/* The Zorro II Cirrus boards this driver knows: two autoconfig boards
 * each (VRAM + registers), the same chip family, different glue. */

static const struct P2Board {
    UWORD mfr;
    UBYTE memProd, regProd, pass;
    BOOL  alias;              /* DAC data/read-index at port + $FFF       */
    BOOL  swapRB;
    const char *name;
} p2_boards[] = {
    /* Product 13 is the Picasso II's segmented mode (a jumper): refused. */
    { 2167, 11, 12, PASS_PICASSO, TRUE,  FALSE, "Picasso II" },
    { 2167, 16, 17, PASS_PICASSO, TRUE,  FALSE, "GBAPII++" },
    { 2195, 10, 11, PASS_4F6F,    FALSE, TRUE,  "Piccolo SD64" },
    { 2195,  5,  6, PASS_BIT5,    FALSE, TRUE,  "Piccolo" },
    { 2193,  1,  2, PASS_4F6F,    FALSE, TRUE,  "Spectrum 28/24" },
};

/* Reject an operation before writing registers if the chip cannot encode
 * its geometry. CPU rendering remains available for larger bitmaps. */
static BOOL p2_surface_ok(struct PrismBoard *b,const struct PrismSurface *s,
                          UWORD w,UWORD h)
{
    struct P2Priv *p=b->priv;
    ULONG end;
    if (!s->allocation || s->offset>=b->vramSize ||
        s->allocation>b->vramSize-s->offset) return FALSE;
    end=s->offset+s->allocation;
    return s->pitch<=(p->is5434 ? 8191UL : 4095UL) &&
        (ULONG)w*s->bpp<=(p->is5434 ? 8192UL : 2048UL) &&
        h<=(p->is5434 ? 2048 : 1024) && end<=0x200000UL &&
        (s->format==PF_CLUT8 || (b->formats & PF_BIT(s->format)));
}
static enum PrismResult p2_fill_surface(struct PrismBoard *b,const struct PrismSurface *d,
    UWORD x,UWORD y,UWORD w,UWORD h,ULONG c)
{
    struct P2Priv *p=b->priv;
    if (!p2_surface_ok(b,d,w,h) || d->bpp==3 || (d->bpp==4 && !p->x32))
        return PR_DECLINED;
    p->failed=FALSE;
    p2_FillRect(b,d->offset,d->pitch,d->bpp,x,y,w,h,c);
    return p->failed ? PR_FAILED : PR_DONE;
}
static enum PrismResult p2_copy_surface(struct PrismBoard *b,const struct PrismSurface *s,
    const struct PrismSurface *d,UWORD sx,UWORD sy,UWORD dx,UWORD dy,UWORD w,UWORD h)
{
    struct P2Priv *p=b->priv;
    if (!p2_surface_ok(b,s,w,h) || !p2_surface_ok(b,d,w,h)) return PR_DECLINED;
    p->failed=FALSE;
    if (s->offset==d->offset && s->pitch==d->pitch)
        p2_CopyRect(b,s->offset,s->pitch,s->bpp,sx,sy,dx,dy,w,h);
    else {
        if (!(s->offset+s->allocation<=d->offset || d->offset+d->allocation<=s->offset))
            return PR_DECLINED;
        p2_CopyBetween(b,s->offset+(ULONG)sy*s->pitch+(ULONG)sx*s->bpp,s->pitch,
            d->offset+(ULONG)dy*d->pitch+(ULONG)dx*d->bpp,d->pitch,w*d->bpp,h);
    }
    return p->failed ? PR_FAILED : PR_DONE;
}
static enum PrismResult p2_expand_surface(struct PrismBoard *b,const struct PrismSurface *d,
    UWORD x,UWORD y,UWORD w,UWORD h,const UBYTE *src,ULONG mod,ULONG fg,ULONG bg,BOOL tr)
{
    struct P2Priv *p=b->priv;
    BOOL done;
    if (!p2_surface_ok(b,d,w,h)) return PR_DECLINED;
    p->failed=FALSE;
    done=p2_ExpandRect(b,d->offset,d->pitch,d->bpp,x,y,w,h,src,mod,fg,bg,tr);
    /* A reset after a text timeout disables only CPU-fed expansion.
     * Fills and VRAM copies still work; overwrite text can run on the CPU. */
    return p->failed ? PR_RETRY : done ? PR_DONE : PR_DECLINED;
}
static const struct PrismOps p2_ops = {
    .fill=p2_fill_surface, .copy=p2_copy_surface, .expand=p2_expand_surface
};

static void p2_ready(struct PrismBoard *b)
{
    BOOL transparent;
    UBYTE pad = Picasso2_TextExpand(b, &transparent);
    if (pad == 0xff)
        puts("PrismD: text on the CPU (blitter text expansion failed its self-test)");
    else
        printf("PrismD: text on the blitter (rows padded to %u byte%s, %s)\n", pad,
               pad == 1 ? "" : "s", transparent ? "JAM1 + JAM2" : "JAM2 only");
    if (b->formats & PF_BIT(PF_BGRA32))
        puts((b->flags & PBF_BLIT_32) ? "PrismD: 32-bit fills and text on the blitter"
                                     : "PrismD: no 32-bit blitter (self-test failed)");
}

BOOL Picasso2_Probe(struct PrismBoard *b)
{
    struct ConfigDev *regs = NULL, *mem = NULL;
    struct P2Priv *p = &p2priv;
    const struct P2Board *bd = NULL;
    const char *chipName;
    ULONG vsize;
    UWORD i;

    for (i = 0; i < sizeof(p2_boards) / sizeof(p2_boards[0]); i++) {
        bd = &p2_boards[i];
        if ((regs = FindConfigDev(NULL, bd->mfr, bd->regProd)) &&
            (mem = FindConfigDev(NULL, bd->mfr, bd->memProd)))
            break;
        regs = mem = NULL;
    }
    if (!regs || !mem)
        return FALSE;

    b->configDev = regs;
    b->regs      = (volatile UBYTE *)regs->cd_BoardAddr;
    b->vram      = (UBYTE *)mem->cd_BoardAddr;
    b->priv      = p;
    p->memDev    = mem;
    p->pass      = bd->pass;
    p->swapRB    = bd->swapRB;
    p->dacRI     = bd->alias ? 0x13c6 : 0x3c7;
    p->dacD      = bd->alias ? 0x13c8 : 0x3c9;

    /* Piccolo and Spectrum boards have their own wake-up at the control
     * byte, before the chip's (NetBSD waits 0.2 s after it). */
    if (bd->pass != PASS_PICASSO) {
        volatile UBYTE *sw = b->regs + 0x8000;
        volatile ULONG n;
        if (bd->pass == PASS_4F6F) *sw = 0x1f;
        else                       *sw = *sw | 0x10;
        for (n = 0; n < 200000; n++) ;
    }

    /* Wake the chip. After a reset nothing initialises it (no VGA BIOS on
     * the Amiga) and it ignores I/O: reads echo the last index written. The
     * ISA wake-up Linux uses on Zorro Cirrus boards; WinUAE ignores it.
     * Verified on the real GBAPII++ (2026-10-03): asleep -> CR27 = $A8. */
    Disable();
    outb(b, 0x46e8, 0x10);                        /* setup mode           */
    outb(b, 0x0102, 0x01);                        /* POS: enable          */
    outb(b, 0x46e8, 0x08);                        /* enable, leave setup  */
    outb(b, 0x3c3, 0x01);                         /* video subsystem on   */
    wseq(b, 0x06, 0x12);
    p->chip = read_chip_id(b) & 0xfc;
    Enable();
    switch (p->chip) {
    case CL_GD5426: chipName = "GD5426"; break;
    case CL_GD5428: chipName = "GD5428"; break;
    case 0xa4:
    case CL_GD5434: chipName = "GD5434"; p->is5434 = TRUE; break;
    default:                                      /* not a Cirrus we know */
        Picasso2_LastID = p->chip | 0x100;
        return FALSE;
    }
    sprintf(p->name, "%s%s (%s)", bd->name,
            (bd->regProd == 12 && p->chip == CL_GD5428) ? "+" : "", chipName);
    b->name      = p->name;
    p->aperture  = 0x20;       /* p2_SetMode tries 0x80 if this one is dead */

    /* the top 64 KB of VRAM is driver scratch: blitter fill pattern now,
     * hardware cursor image later. More than 2 MB (a 4 MB Piccolo SD64)
     * is not used: the DRAM set-up here is for 1 or 2 MB. */
    vsize = mem->cd_BoardSize;
    if (vsize > 0x200000)
        vsize = 0x200000;
    p->scratch   = vsize - 0x10000;
    b->vramSize  = p->scratch;
    if (p->swapRB)
        b->formats = PF_BIT(PF_CLUT8) | PF_BIT(PF_BGR565LE) | PF_BIT(PF_BGR555LE) |
                     PF_BIT(PF_RGB24) | (p->is5434 ? PF_BIT(PF_ARGB32) : 0);   /* 32-bit: x,R,G,B (WinUAE) */
    else
        b->formats = PF_BIT(PF_CLUT8) | PF_BIT(PF_RGB565LE) | PF_BIT(PF_RGB555LE) |
                     PF_BIT(PF_BGR24) | (p->is5434 ? PF_BIT(PF_BGRA32) : 0);
    b->flags     = PBF_HW_CURSOR;
    b->maxWidth  = 1024;
    b->maxHeight = 768;

    b->setMode         = p2_SetMode;
    b->checkMode       = p2_CheckMode;
    b->setDisplayStart = p2_SetDisplayStart;
    b->setPalette      = p2_SetPalette;
    b->setSwitch       = p2_SetSwitch;
    b->waitVBlank      = p2_WaitVBlank;
    b->shutdown        = p2_Shutdown;
    b->saveState       = p2_SaveState;
    b->restoreState    = p2_RestoreState;
    b->fillRect        = p2_FillRect;
    b->copyRect        = p2_CopyRect;
    b->ops             = &p2_ops;
    /* what the BLT registers can encode (p2_surface_ok): bigger requests
     * take the surface path, which declines them */
    b->blitMaxBytes    = p->is5434 ? 8192 : 2048;
    b->blitMaxRows     = p->is5434 ? 2048 : 1024;
    b->blitMaxPitch    = p->is5434 ? 8191 : 4095;
    b->modeReady       = p2_ready;
    b->copyBetween     = p2_CopyBetween;
    b->expandRect      = p2_ExpandRect;
    b->waitBlit        = p2_WaitBlit;
    b->cursorImage     = p2_CursorImage;
    b->cursorShow      = p2_CursorShow;
    b->cursorMove      = p2_CursorMove;
    return TRUE;
}

#ifdef PRISM_DEBUG
#include <stdio.h>
/* Print the live register state, for comparing against what SetMode wrote. */
void Picasso2_Dump(struct PrismBoard *b)
{
    struct P2Priv *p = b->priv;
    UWORD i;

    printf("text expansion: rows padded to %u byte(s)%s\n", p->xpad,
           p->xpad == 0xff ? " - OFF (failed its self-test)" :
           p->xtransp ? ", transparent too" : ", opaque only");
    printf("32-bit blitter (fill + text): %s%s\n", p->x32 ? "yes" : "no",
           p->x32 ? (p->x32t ? ", transparent too" : ", opaque only") : "");
    printf("  register word writes: %s; colours may stay set: %s; fills need source pitch: %s\n",
           p->wordio ? "yes" : "no", p->srOff ? "yes" : "no", p->fillPitch ? "yes" : "no");
    if (!p->xtransp && p->xdiag)
        printf("  transparency test: %lu-byte pixels, x=%lu y=%lu read %04lx%s\n",
               (unsigned long)((p->xdiag >> 28) & 0xf), (unsigned long)((p->xdiag >> 20) & 0xff),
               (unsigned long)((p->xdiag >> 16) & 0xf),
               (unsigned long)(p->xdiag & 0xffff), (p->xdiag >> 24) == 0xff ? " (blit stuck)" : "");
    printf("chip=%02x aperture=%02x MISC=%02x HDR=%02x ST1=%02x\nSR:", p->chip, p->aperture,
           inb(b, MISC_R), rhdr(b), inb(b, STATUS1));
    for (i = 0; i < 0x20; i++) printf(" %02x", rseq(b, i));
    printf("\nCR:");
    for (i = 0; i < 0x20; i++) printf(" %02x", rcrt(b, i));
    printf("\nGR:");
    for (i = 0; i < 0x10; i++) printf(" %02x", rgrc(b, i));
    printf("\nAR:");
    for (i = 0; i < 0x15; i++) printf(" %02x", rattr(b, i));
    attr_on(b);
    printf("\nDAC 0-3:");
    outb(b, DAC_RI, 0);
    for (i = 0; i < 12; i++) printf(" %02x", inb(b, DAC_D));
    printf("\n");
    fflush(stdout);
}
#endif
