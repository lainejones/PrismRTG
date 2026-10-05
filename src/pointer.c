/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laine Jones */
/*
 * pointer.c - the mouse pointer on the card.
 *
 * Intuition shows its pointer with sprite 0: ChangeSprite/ChangeExtSpriteA
 * give the image, MoveSprite the position. The patches let the original
 * run (the native display keeps its pointer) and keep a copy here; while a
 * Prism screen is in front the board's hardware cursor shows it.
 *
 * Position updates come from input.device's task, so they only touch the
 * board when Prism's lock is free right then; otherwise PrismD's main loop
 * applies the latest one on its next tick.
 */
#include <exec/types.h>
#include <graphics/sprite.h>
#include <graphics/view.h>
#include <intuition/intuitionbase.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <string.h>
#include "prismint.h"

extern struct IntuitionBase *IntuitionBase;
LONG call_regs(APTR fn, struct Regs *r);
extern APTR o_MoveSprite, o_ChangeSprite, o_ChangeExtSpriteA;

static UBYTE image[CURSOR_SIZE * CURSOR_SIZE];   /* unshifted, pens 0..3   */
static UBYTE shifted[CURSOR_SIZE * CURSOR_SIZE];
static UBYTE colours[9];
static BOOL  haveImage, on;
static volatile BOOL dirtyImage, dirtyPos;
static volatile WORD posX, posY;                 /* sprite position        */
static WORD loadedDX = -1, loadedDY = -1;        /* clip shift now loaded  */
static struct ViewPort *vpOn;

/* Decode Amiga sprite data: control words, then per line ww words of
 * plane 0 and ww words of plane 1 (ww = 1, 2 or 4 for 16/32/64 wide). */
static void decode(const UWORD *posctl, UWORD height, UWORD ww)
{
    const UWORD *d = posctl + 2 * ww;
    UWORD y, x, w;

    if (!posctl || !height)
        return;
    if (ww < 1) ww = 1;
    if (ww > 4) ww = 4;
    if (height > CURSOR_SIZE) height = CURSOR_SIZE;
    memset(image, 0, sizeof(image));
    for (y = 0; y < height; y++, d += 2 * ww)
        for (w = 0; w < ww; w++)
            for (x = 0; x < 16; x++) {
                UBYTE v = ((d[w] >> (15 - x)) & 1) | (((d[ww + w] >> (15 - x)) & 1) << 1);
                image[y * CURSOR_SIZE + w * 16 + x] = v;
            }
    haveImage = TRUE;
    dirtyImage = TRUE;
}

/* Sprite colours 17-19 of the screen on the card. */
static void load_colours(struct ViewPort *vp)
{
    ULONG t[9];
    UBYTE i;
    if (!vp || !vp->ColorMap)
        return;
    GetRGB32(vp->ColorMap, 17, 3, t);
    for (i = 0; i < 9; i++)
        colours[i] = t[i] >> 24;
}

/* Push image (shifted for a negative position) and position. Caller holds
 * the lock and the cursor is on. */
static void apply(void)
{
    WORD x = posX, y = posY, dx = 0, dy = 0;

    if (x < 0) { dx = -x; x = 0; }
    if (y < 0) { dy = -y; y = 0; }
    if (dx >= CURSOR_SIZE) dx = CURSOR_SIZE - 1;
    if (dy >= CURSOR_SIZE) dy = CURSOR_SIZE - 1;
    if (dirtyImage || dx != loadedDX || dy != loadedDY) {
        WORD r;
        memset(shifted, 0, sizeof(shifted));
        for (r = dy; r < CURSOR_SIZE; r++)
            memcpy(shifted + (r - dy) * CURSOR_SIZE, image + r * CURSOR_SIZE + dx,
                   CURSOR_SIZE - dx);
        board.cursorImage(&board, shifted, colours);
        loadedDX = dx;
        loadedDY = dy;
        dirtyImage = FALSE;
    }
    board.cursorMove(&board, x, y);
    dirtyPos = FALSE;
}

/* A Prism screen came to the front (lock held). */
void pointer_on(struct ViewPort *vp)
{
    if (!board.cursorImage)
        return;
    vpOn = vp;
    load_colours(vp);
    dirtyImage = TRUE;
    on = TRUE;
    if (haveImage) {
        apply();
        board.cursorShow(&board, TRUE);
    }
}

/* The native display took over (lock held). */
void pointer_off(void)
{
    if (on && board.cursorShow)
        board.cursorShow(&board, FALSE);
    on = FALSE;
    vpOn = NULL;
}

/* Palette of the screen on the card changed (lock held). */
void pointer_colours(struct ViewPort *vp)
{
    if (on && vp == vpOn) {
        load_colours(vp);
        dirtyImage = TRUE;
        apply();
    }
}

/* PrismD's main loop, every tick. */
void pointer_tick(void)
{
    if (on && (dirtyPos || dirtyImage)) {
        ObtainSemaphore(&lock);
        if (on && haveImage) {
            apply();
            board.cursorShow(&board, TRUE);
        }
        ReleaseSemaphore(&lock);
    }
}

static void try_apply(void)
{
    if (on && haveImage && AttemptSemaphore(&lock)) {
        apply();
        board.cursorShow(&board, TRUE);
        ReleaseSemaphore(&lock);
    }
}

/* ---- patches -------------------------------------------------------- */

static WORD logged;

/* MoveSprite(vp a0, sprite a1, x d0, y d1) */
LONG h_MoveSprite(struct Regs *r)
{
    struct SimpleSprite *s = (struct SimpleSprite *)r->a[1];
    call_regs(o_MoveSprite, r);
    if (s && s->num == 0) {
        struct Screen *fs = IntuitionBase->FirstScreen;
        WORD x = RW(0), y = RW(1);
        /* On a Prism screen Intuition's sprite X comes out as 0 whatever
         * the mode reports, while Y is right (mouse Y minus the hot spot).
         * Take X from the screen's mouse position with the same hot-spot
         * offset as Y. */
        if (fs && r->a[0] == (LONG)&fs->ViewPort) {
            WORD hot = y - fs->MouseY;
            x = fs->MouseX + hot;
        }
        if (logged < 20 && (x != posX || y != posY)) {
            logged++;
            dbg("MoveSprite -> %d,%d\n", x, y);
        }
        posX = x;
        posY = y;
        dirtyPos = TRUE;
        try_apply();
    }
    return 1;
}

/* ChangeSprite(vp a0, sprite a1, newData a2) */
LONG h_ChangeSprite(struct Regs *r)
{
    struct SimpleSprite *s = (struct SimpleSprite *)r->a[1];
    call_regs(o_ChangeSprite, r);
    if (s && s->num == 0) {
        dbg("ChangeSprite h=%u\n", s->height);
        decode((const UWORD *)r->a[2], s->height, 1);
        try_apply();
    }
    return 1;
}

/* ChangeExtSpriteA(vp a0, old a1, new a2, tags a3) */
LONG h_ChangeExtSpriteA(struct Regs *r)
{
    struct ExtSprite *n = (struct ExtSprite *)r->a[2];
    call_regs(o_ChangeExtSpriteA, r);
    if (n && n->es_SimpleSprite.num == 0) {
        dbg("ChangeExtSprite h=%u ww=%u x=%d y=%d\n", n->es_SimpleSprite.height,
            n->es_wordwidth, n->es_SimpleSprite.x, n->es_SimpleSprite.y);
        decode(n->es_SimpleSprite.posctldata, n->es_SimpleSprite.height, n->es_wordwidth);
        try_apply();
    }
    return 1;
}
