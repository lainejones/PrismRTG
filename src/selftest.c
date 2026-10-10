/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laine Jones */
/*
 * Start-up checks of a board's drawing hooks against a known answer.
 *
 * Text (colour-expanding a 1-bit template) is the blitter operation with
 * the most to get wrong - source bit order, padding, odd widths, JAM1. The
 * MiSTer's ZZ9000 core put every glyph after the second a pixel or more to
 * the left at 8 bits and drew nothing at 16 (issue #2, 2026-10-09), where
 * the card itself is right. So once the first mode is set, each depth the
 * board shows gets a template drawn into a scratch bitmap in VRAM and read
 * back; a depth where the board gets it wrong draws its text on the CPU.
 */
#include <exec/memory.h>
#include <proto/exec.h>
#include <stdio.h>
#include <string.h>
#include "prismint.h"

UBYTE textBad;                       /* bit n: blitter text wrong at n bytes/pixel */

#define TW 37                        /* odd width, past one byte, not a multiple of 8 */
#define TH 5
#define TMOD 6                       /* rows of (37+7)/8 = 5 bytes, 6 apart */

static ULONG px_get(const UBYTE *a, UBYTE bpp)
{
    return bpp == 1 ? a[0] : bpp == 2 ? *(const UWORD *)a : *(const ULONG *)a;
}

/* wrong pixels for one draw mode at one depth; ~0 = could not test */
static ULONG check(struct PBitMap *p, const UBYTE *tmpl, BOOL transparent)
{
    const UBYTE bpp = p->bpp;
    const ULONG fg = bpp == 1 ? 0x05 : bpp == 2 ? 0x1234 : 0x00123456UL;
    const ULONG bg = bpp == 1 ? 0x09 : bpp == 2 ? 0x0f0f : 0x00abcdefUL;
    const ULONG keep = bpp == 1 ? 0x77 : bpp == 2 ? 0x7777 : 0x00777777UL;
    WORD x, y, x0 = 3, y0 = 1;      /* odd x: the template is not byte aligned */
    ULONG bad = 0;

    for (y = 0; y < p->h; y++)
        for (x = 0; x < p->w; x++) {
            UBYTE *a = p->pix + (ULONG)y * p->bpr + (ULONG)x * bpp;
            if (bpp == 1) *a = keep; else if (bpp == 2) *(UWORD *)a = keep; else *(ULONG *)a = keep;
        }
    if (!board.expandRect(&board, p->vramOff, p->bpr, bpp, x0, y0, TW, TH, tmpl, TMOD,
                          fg, bg, transparent))
        return ~0UL;                 /* the board declined: nothing to check */
    if (board.waitBlit)
        board.waitBlit(&board);
    for (y = 0; y < p->h; y++)
        for (x = 0; x < p->w; x++) {
            ULONG want = keep, got = px_get(p->pix + (ULONG)y * p->bpr + (ULONG)x * bpp, bpp);
            if (x >= x0 && x < x0 + TW && y >= y0 && y < y0 + TH) {
                WORD tx = x - x0;
                BOOL set = (tmpl[(y - y0) * TMOD + (tx >> 3)] >> (7 - (tx & 7))) & 1;
                want = set ? fg : transparent ? keep : bg;
            }
            if (got != want)
                bad++;
        }
    return bad;
}

void selftest_text(void)
{
    static const UBYTE tmpl[TH * TMOD] = {
        0x81, 0x42, 0x24, 0x18, 0xf0, 0,  /* every byte different, the last one */
        0x55, 0xaa, 0x0f, 0xf0, 0x80, 0,  /* partly used (37 = 4 bytes + 5 bits) */
        0xff, 0x00, 0xff, 0x00, 0xa8, 0,
        0x01, 0x80, 0x01, 0x80, 0x08, 0,
        0xc3, 0x3c, 0x99, 0x66, 0x70, 0,
    };
    UBYTE bpp, f;

    if (!board.expandRect || (board.flags & (PBF_SHADOW | PBF_SOFTWARE)))
        return;
    for (bpp = 1; bpp <= 4; bpp++) {
        struct PBitMap *p = NULL;
        ULONG a, b;
        if (bpp == 3)
            continue;
        /* the first format the board shows at this depth */
        for (f = 0; f < PF_COUNT && !p; f++)
            if ((board.formats & PF_BIT(f)) && board_bpp(f) == bpp)
                p = pbm_new(48, 8, 8, f, NULL, 2, TRUE);
        if (!p)
            continue;
        if (p->inVram && p->bpp == bpp) {
            a = check(p, tmpl, FALSE);
            b = check(p, tmpl, TRUE);
            if ((a != ~0UL && a) || (b != ~0UL && b)) {
                textBad |= 1 << bpp;
                printf("PrismD: %u-bit text on the CPU (the board's text blit failed its "
                       "self-test: %lu + %lu pixels wrong)\n", bpp == 1 ? 8 : bpp * 8,
                       (unsigned long)(a == ~0UL ? 0 : a), (unsigned long)(b == ~0UL ? 0 : b));
            }
        }
        pbm_free(p);
    }
}
