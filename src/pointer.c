/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laine Jones */
/*
 * pointer.c - the mouse pointer on the card.
 *
 * Intuition shows its pointer with sprite 0: ChangeSprite/ChangeExtSpriteA
 * give the image, MoveSprite the position. The patches let the original
 * run (the native display keeps its pointer) and keep a copy here; while a
 * Prism screen is in front the board's hardware cursor shows it - or, on a
 * board without a working one (or SOFTWAREPOINTER=ON), a software sprite
 * drawn into the shown bitmap with what was under it saved (sw_*), and
 * while the compositor runs (dragging, PIPs), pointer_compose.
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
#include "composite.h"

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
static WORD  imgW, imgH;                         /* extent of the image    */

/* Software sprite: drawn into the shown bitmap, with what was under it. */
struct PBitMap *swOn;
volatile ULONG prismActivity;
static WORD  swX, swY, swW, swH;                 /* drawn rectangle        */
static ULONG swSeen;
static BOOL  swDirty;
static UBYTE swSave[CURSOR_SIZE * CURSOR_SIZE * 4];

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
    /* the part of the 64x64 box the image uses: all the save-under needs */
    imgW = imgH = 0;
    for (y = 0; y < CURSOR_SIZE; y++)
        for (x = 0; x < CURSOR_SIZE; x++)
            if (image[y * CURSOR_SIZE + x]) {
                if (x >= imgW) imgW = x + 1;
                if (y >= imgH) imgH = y + 1;
            }
    haveImage = TRUE;
    dirtyImage = TRUE;
    swDirty = TRUE;
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

BOOL pointer_software(void)
{
    struct PBitMap *front,*back; WORD top,backTop; UWORD w,h;
    if (!on) return FALSE;
    prism_display_layers(&front,&back,&top,&backTop,&w,&h);
    return prism_software_pointer() || (prism_dragging() && top) || !(board.flags & PBF_HW_CURSOR) ||
        !board.cursorImage || !board.cursorShow || !board.cursorMove;
}
/* The board's cursor, unless PrismPrefs asks for the software pointer or
 * a composed frame (dragging, PIPs) carries the pointer. */
static BOOL hardware_pointer(void)
{
    return (board.flags & PBF_HW_CURSOR) && board.cursorImage &&
        board.cursorShow && board.cursorMove && !prism_software_pointer() &&
        (!pointer_software() || !present_ready());
}

/* Take the software pointer out again (lock held). */
void sw_hide(void)
{
    struct PBitMap *p = swOn;
    WORD r;
    ULONG n;
    if (!p)
        return;
    swOn = NULL;
    if (board.waitBlit)
        board.waitBlit(&board);
    n = (ULONG)swW * p->bpp;
    for (r = 0; r < swH; r++)
        CopyMem(swSave + r * n, p->pix + (ULONG)(swY + r) * p->bpr + (ULONG)swX * p->bpp, n);
}

void sw_clear(struct PBitMap *p, WORD x0, WORD y0, WORD x1, WORD y1)
{
    if (!swOn)
        return;
    if (p && (p != swOn || x1 < swX || x0 >= swX + swW || y1 < swY || y0 >= swY + swH))
        return;
    prismActivity++;
    sw_hide();
}

/* Draw the software pointer into the shown bitmap at the sprite position,
 * keeping what was under it (lock held, not drawn now). */
static WORD swPX, swPY;                         /* position it was drawn for */

static void sw_draw(struct PBitMap *p)
{
    UBYTE pixels[4][4];
    WORD px, py, x0, y0, x1, y1, x, y;
    UBYTE pen, k, bpp = p->bpp;
    ULONG n;
    /* MoveSprite and the image decode run in input.device without the
     * lock: take the position once, and clear the flag first so a change
     * made meanwhile draws again on the next tick */
    swDirty = FALSE;
    px = posX; py = posY;
    x0 = px; y0 = py; x1 = px + imgW; y1 = py + imgH;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (WORD)p->w) x1 = p->w;
    if (y1 > (WORD)p->h) y1 = p->h;
    if (x0 >= x1 || y0 >= y1)
        return;
    for (pen = 1; pen < 4; pen++)
        composite_put(p, pixels[pen], ((ULONG)colours[(pen - 1) * 3] << 16) |
                      ((ULONG)colours[(pen - 1) * 3 + 1] << 8) | colours[(pen - 1) * 3 + 2]);
    if (board.waitBlit)
        board.waitBlit(&board);
    swX = x0; swY = y0; swW = x1 - x0; swH = y1 - y0;
    n = (ULONG)swW * bpp;
    for (y = y0; y < y1; y++) {
        UBYTE *row = p->pix + (ULONG)y * p->bpr + (ULONG)x0 * bpp;
        const UBYTE *img = image + (y - py) * CURSOR_SIZE + (x0 - px);
        CopyMem(row, swSave + (y - y0) * n, n);
        for (x = 0; x < swW; x++)
            if ((pen = img[x]))
                for (k = 0; k < bpp; k++)
                    row[x * bpp + k] = pixels[pen][k];
    }
    swOn = p;
    swPX = px; swPY = py;
}

/* PrismD's tick: is the software sprite wanted, and where? (lock held) */
static void sw_tick(void)
{
    struct PBitMap *p = prism_display_bitmap();
    BOOL want = on && haveImage && !hardware_pointer() && !present_ready() &&
        p && !p->locks && !(board.flags & PBF_PRESENT);
    if (!want) {
        sw_hide();
        return;
    }
    if (prismActivity != swSeen) {     /* drawn into lately: wait for quiet */
        swSeen = prismActivity;
        return;
    }
    if (swOn && !swDirty && swOn == p && swPX == posX && swPY == posY)
        return;
    sw_hide();
    sw_draw(p);
}

void pointer_compose(struct PBitMap *dst,WORD top)
{
    UBYTE pixels[4][4];
    WORD x,y,px=posX,py=posY;UBYTE pen,k;     /* position taken once (see sw_draw) */
    if (!haveImage || !pointer_software()) return;
    /* Quantize the three sprite colours once, rather than once per pixel. */
    for(pen=1;pen<4;pen++) {
        ULONG rgb=((ULONG)colours[(pen-1)*3]<<16)|
            ((ULONG)colours[(pen-1)*3+1]<<8)|colours[(pen-1)*3+2];
        composite_put(dst,pixels[pen],rgb);
    }
    for(y=0;y<CURSOR_SIZE;y++) for(x=0;x<CURSOR_SIZE;x++) {
        LONG dx=(LONG)px+x,dy=(LONG)py+y+top;
        UBYTE *d;
        pen=image[y*CURSOR_SIZE+x];
        if(!pen || dx<0 || dy<0 || dx>=dst->w || dy>=dst->h) continue;
        d=dst->pix+dy*dst->bpr+dx*dst->bpp;
        for(k=0;k<dst->bpp;k++)d[k]=pixels[pen][k];
    }
}
/* Push image (shifted for a negative position) and position. Caller holds
 * the lock and the cursor is on. */
static void apply(void)
{
    WORD x = posX, y = posY, dx = 0, dy = 0;
    if (!hardware_pointer()) {
        if (board.cursorShow) board.cursorShow(&board,FALSE);
        dirtyPos=FALSE;dirtyImage=TRUE;
        return;
    }

    if (pointer_software() && prism_dragging()) {
        struct PBitMap *front,*back; WORD top,backTop; UWORD w,h;
        prism_display_layers(&front,&back,&top,&backTop,&w,&h);
        y += top;
    }
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
    vpOn = vp;
    load_colours(vp);
    dirtyImage = TRUE;
    swDirty = TRUE;
    on = TRUE;
    if (haveImage) {
        apply();
        if (hardware_pointer()) board.cursorShow(&board, TRUE);
    }
}

/* The native display took over (lock held). */
void pointer_off(void)
{
    sw_hide();
    if (on && board.cursorShow)
        board.cursorShow(&board, FALSE);
    on = FALSE;
    vpOn = NULL;
    present_stop();
}

/* Palette of the screen on the card changed (lock held). */
void pointer_colours(struct ViewPort *vp)
{
    if (on && vp == vpOn) {
        load_colours(vp);
        dirtyImage = TRUE;
        swDirty = TRUE;
        apply();
    }
}

/* PrismD's main loop, every tick. */
void pointer_tick(void)
{
    if ((on && (dirtyPos || dirtyImage || pointer_software())) || swOn) {
        ObtainSemaphore(&lock);
        if (on && haveImage) {
            apply();
            if (hardware_pointer()) board.cursorShow(&board, TRUE);
        }
        sw_tick();
        ReleaseSemaphore(&lock);
    }
}

static void try_apply(void)
{
    if (on && haveImage && AttemptSemaphore(&lock)) {
        apply();
        if (hardware_pointer()) board.cursorShow(&board, TRUE);
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
        swDirty = TRUE;
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
