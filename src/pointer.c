/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laine Jones
 * Copyright (C) 2026 Stefan Reinauer */
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
#include <exec/memory.h>
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
volatile WORD posX, posY;     /* sprite position on the display (prismint.h) */
/* a mirrored chipset screen (MIRRORCHIPSET): the pointer follows that screen's
 * own mouse position, times the mirror's scale (prismd.c sets both) */
struct Screen *volatile ptrMirror;
UBYTE ptrMirrorScale = 1;
static WORD loadedDX = -1, loadedDY = -1;        /* clip shift now loaded  */
static struct ViewPort *vpOn;
static WORD  imgW, imgH;                         /* extent of the image    */

/* Software sprite: drawn into the shown bitmap, with what was under it. */
struct PBitMap *swOn;
/* the card showing the front screen (pointer_on): its cursor is the pointer */
static struct PrismCard *ptrCard = &cards[0];
#define PB (ptrCard->b)
static void shown_origin(WORD *ox, WORD *oy);
volatile ULONG prismActivity;
WORD swX, swY, swW, swH;                        /* drawn rectangle (prismint.h) */
static ULONG swSeen;
static volatile ULONG swHits;                    /* drawing that took it out */
static BOOL  swDirty;
static UBYTE *swSave;                   /* allocated at the first draw */

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

/* does the image fit the board's sprite? (a 64x64 pointer on a ZZ9000,
 * whose sprite is 32x48, is drawn by the software pointer instead) */
static BOOL image_fits(void)
{
    return (!PB->cursorW || imgW <= PB->cursorW) && (!PB->cursorH || imgH <= PB->cursorH);
}

static BOOL pointer_software(void)
{
    struct PBitMap *front,*back; WORD top,backTop; UWORD w,h;
    if (!on) return FALSE;
    prism_display_layers(ptrCard,&front,&back,&top,&backTop,&w,&h);
    return prism_software_pointer() || (prism_dragging() && top) || !(PB->flags & PBF_HW_CURSOR) ||
        !PB->cursorImage || !PB->cursorShow || !PB->cursorMove || !image_fits();
}
/* The board's cursor, unless PrismPrefs asks for the software pointer, the
 * image is bigger than the sprite, or a composed frame (dragging, PIPs)
 * carries the pointer. */
static BOOL hardware_pointer(void)
{
    return (PB->flags & PBF_HW_CURSOR) && PB->cursorImage &&
        PB->cursorShow && PB->cursorMove && !prism_software_pointer() && image_fits() &&
        (!pointer_software() || !present_ready(ptrCard));
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
    if (p->bd->waitBlit)
        p->bd->waitBlit(p->bd);
    n = (ULONG)swW * p->bpp;
    for (r = 0; r < swH; r++)
        CopyMem(swSave + r * n, p->pix + (ULONG)(swY + r) * p->bpr + (ULONG)swX * p->bpp, n);
}

void sw_hit(void)
{
    swHits++;
    sw_hide();
}

void sw_clear(struct PBitMap *p, WORD x0, WORD y0, WORD x1, WORD y1)
{
    if (!swOn)
        return;
    if (p && (p != swOn || x1 < swX || x0 >= swX + swW || y1 < swY || y0 >= swY + swH))
        return;
    prismActivity++;
    sw_hit();
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
    {
        WORD ox, oy;
        shown_origin(&ox, &oy);              /* sw_draw: nothing is composed */
        px = posX - ox; py = posY - oy;
    }
    x0 = px; y0 = py; x1 = px + imgW; y1 = py + imgH;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (WORD)p->w) x1 = p->w;
    if (y1 > (WORD)p->h) y1 = p->h;
    if (x0 >= x1 || y0 >= y1)
        return;
    if (!swSave && !(swSave = AllocVec(CURSOR_SIZE * CURSOR_SIZE * 4, MEMF_PUBLIC)))
        return;                          /* no save-under, no pointer */
    for (pen = 1; pen < 4; pen++)
        composite_put(p, pixels[pen], ((ULONG)colours[(pen - 1) * 3] << 16) |
                      ((ULONG)colours[(pen - 1) * 3 + 1] << 8) | colours[(pen - 1) * 3 + 2]);
    if (p->bd->waitBlit)
        p->bd->waitBlit(p->bd);
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
    swPX = posX; swPY = posY;                 /* as read: sw_tick compares with posX/posY */
}

/* PrismD's tick: is the software sprite wanted, and where? (lock held) */
static void sw_tick(void)
{
    struct PBitMap *p = prism_display_bitmap(ptrCard);
    BOOL want = on && haveImage && !hardware_pointer() && !present_ready(ptrCard) &&
        p && !p->locks && !(p->bd->flags & PBF_PRESENT);
    if (!want) {
        sw_hide();
        return;
    }
    /* drawing hit it lately: wait for a tick in which nothing does (drawing
     * elsewhere - a clock, a shell scrolling in another window - used to
     * keep it away too, and colour changes took it out every time) */
    if (swHits != swSeen) {
        swSeen = swHits;
        return;
    }
    if (swOn && !swDirty && swOn == p && swPX == posX && swPY == posY)
        return;
    sw_hide();
    sw_draw(p);
}

/* Where a composed frame shows the pointer: its display position, kept on
 * the frame. Below a dragged screen Intuition lets the pointer go as far
 * as the screen behind reaches; when that screen is bigger than the
 * frame the pointer would leave the picture (issue #1, beta 5). */
static void frame_pos(UWORD w, UWORD h, WORD *x, WORD *y)
{
    *x = posX; *y = posY;
    if (*x > (WORD)w - 1) *x = w - 1;
    if (*y > (WORD)h - 1) *y = h - 1;
}

BOOL pointer_compose_rows(struct PrismCard *c,WORD top,WORD *y0,WORD *y1)
{
    struct PBitMap *front,*back; WORD t,backTop,x; UWORD w=32767,h=32767;
    (void)top;
    /* only the board showing the front screen has the pointer */
    if (!haveImage || c != ptrCard || !pointer_software()) return FALSE;
    prism_display_layers(c,&front,&back,&t,&backTop,&w,&h);
    frame_pos(w,h,&x,y0);*y1=*y0+CURSOR_SIZE;
    return TRUE;
}

void pointer_compose(struct PBitMap *dst,WORD top)
{
    UBYTE pixels[4][4];
    WORD x,y,px,py;UBYTE pen,k;
    (void)top;
    if (!haveImage || dst->card != ptrCard || !pointer_software()) return;
    frame_pos(dst->w,dst->h,&px,&py);           /* position taken once (see sw_draw) */
    /* Quantize the three sprite colours once, rather than once per pixel. */
    for(pen=1;pen<4;pen++) {
        ULONG rgb=((ULONG)colours[(pen-1)*3]<<16)|
            ((ULONG)colours[(pen-1)*3+1]<<8)|colours[(pen-1)*3+2];
        composite_put(dst,pixels[pen],rgb);
    }
    for(y=0;y<CURSOR_SIZE;y++) for(x=0;x<CURSOR_SIZE;x++) {
        LONG dx=(LONG)px+x,dy=(LONG)py+y;
        UBYTE *d;
        pen=image[y*CURSOR_SIZE+x];
        if(!pen || dx<0 || dy<0 || dx>=dst->w || dy>=dst->h) continue;
        d=dst->pix+dy*dst->bpr+dx*dst->bpp;
        for(k=0;k<dst->bpp;k++)d[k]=pixels[pen][k];
    }
}
/* Where the shown screen's origin is on the display. With nothing composed
 * the board scans the front screen out from its own top left, whatever
 * its LeftEdge and TopEdge (a screen moved while dragging is off, or
 * before the compositor has its frame): the pointer, in display
 * coordinates, then goes at its place on that screen. Lock held. */
static void shown_origin(WORD *ox, WORD *oy)
{
    struct PBitMap *f, *b; WORD top, bt; UWORD w, h;
    struct Screen *s;
    *ox = *oy = 0;
    if ((s = prism_display_layers(ptrCard, &f, &b, &top, &bt, &w, &h))) {
        WORD rx, ry;
        *ox = s->LeftEdge;
        /* a composed frame with dragging on has the screen at its TopEdge
         * already; with dragging off (a PIP) it is composed at row 0 */
        if (!(present_ready(ptrCard) && prism_dragging()))
            *oy = s->TopEdge;
        /* an oversized screen: the display starts rx,ry into it */
        if (prism_display_scroll(ptrCard, &rx, &ry)) {
            *ox += rx;
            *oy += ry;
        }
    }
}

/* Push image (shifted for a negative position) and position. Caller holds
 * the lock and the cursor is on. */
static void apply(void)
{
    WORD x = posX, y = posY, dx = 0, dy = 0, ox, oy;
    if (!hardware_pointer()) {
        if (PB->cursorShow) PB->cursorShow(PB,FALSE);
        dirtyPos=FALSE;dirtyImage=TRUE;
        return;
    }
    shown_origin(&ox, &oy);
    x -= ox;
    y -= oy;

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
        PB->cursorImage(PB, shifted, colours);
        loadedDX = dx;
        loadedDY = dy;
        dirtyImage = FALSE;
    }
    PB->cursorMove(PB, x, y);
    dirtyPos = FALSE;
}

/* A Prism screen came to the front, on card c (lock held). */
void pointer_on(struct ViewPort *vp, struct PrismCard *c)
{
    /* the front screen is now on another board: the pointer leaves the
     * old one (its cursor off, a software sprite taken out) */
    if (c != ptrCard) {
        sw_hide();
        if (on && PB->cursorShow)
            PB->cursorShow(PB, FALSE);
        present_invalidate(ptrCard);         /* its composed frame drops the pointer */
        loadedDX = loadedDY = -1;            /* the new board has no image yet */
    }
    ptrCard = c;
    vpOn = vp;
    load_colours(vp);
    dirtyImage = TRUE;
    swDirty = TRUE;
    on = TRUE;
    if (haveImage) {
        apply();
        if (hardware_pointer()) PB->cursorShow(PB, TRUE);
    }
}

/* The native display took over (lock held). */
void pointer_off(void)
{
    sw_hide();
    if (on && PB->cursorShow)
        PB->cursorShow(PB, FALSE);
    on = FALSE;
    vpOn = NULL;
}

/* is the pointer on card c (the board of the screen in front)? */
BOOL pointer_is_card(const struct PrismCard *c)
{
    return c == ptrCard;
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
    /* the lock first: pointer_software() looks at the screens */
    ObtainSemaphore(&lock);
    if (on && prism_autoscroll(ptrCard, posX, posY))   /* an oversized screen follows the pointer */
        dirtyPos = TRUE;
    if ((on && (dirtyPos || dirtyImage || pointer_software())) || swOn) {
        if (on && haveImage) {
            apply();
            if (hardware_pointer()) PB->cursorShow(PB, TRUE);
        }
        sw_tick();
    }
    ReleaseSemaphore(&lock);
}

/* A move (from input.device) that found the lock taken: whoever releases
 * it next applies the move (pointer_catchup), instead of the pointer
 * waiting for PrismD's next tick - under heavy disk use PrismD's own task
 * gets the CPU late, and the pointer lagged (issue #1, 2026-10-09). */
volatile UBYTE pointerPending;

static void try_apply(void)
{
    if (!on || !haveImage)
        return;
    if (AttemptSemaphore(&lock)) {
        pointerPending = 0;
        apply();
        if (hardware_pointer()) PB->cursorShow(PB, TRUE);
        ReleaseSemaphore(&lock);
    } else
        pointerPending = 1;
}

void pointer_catchup(void)
{
    try_apply();
}

/* ---- patches -------------------------------------------------------- */

static WORD logged;
static WORD hot;                 /* sprite position minus the mouse position */

/* MoveSprite(vp a0, sprite a1, x d0, y d1) */
LONG h_MoveSprite(struct Regs *r)
{
    struct SimpleSprite *s = (struct SimpleSprite *)r->a[1];
    call_regs(o_MoveSprite, r);
    if (s && s->num == 0) {
        struct Screen *fs = IntuitionBase->FirstScreen;
        WORD x, y = RW(1);
        /* On a Prism screen Intuition's sprite X comes out as 0 whatever
         * the mode reports, while Y is right relative to the ViewPort it
         * names (mouse Y minus the hot spot). So only the hot spot is taken
         * from here, and the position is Intuition's own mouse position,
         * which is on the display whatever screen the pointer is over: the
         * sprite position is relative to the screen named, which below a
         * dragged screen is the one behind (the pointer went to x 0 or off
         * the picture), and while a screen is dragged Intuition places the
         * sprite before it moves the screen (the pointer ran a step
         * behind). Issue #1, beta 5. */
        if (fs && r->a[0] == (LONG)&fs->ViewPort) {
            /* a hot spot is a small offset up/left; mid-drag the sprite
             * position is a step stale and gives something else */
            WORD h = y - fs->MouseY;
            if (h > -64 && h <= 0)              /* sprites go up to 64 rows */
                hot = h;
        }
        if (ptrMirror && fs == ptrMirror) {
            x = (fs->MouseX + hot) * ptrMirrorScale;
            y = (fs->MouseY + hot) * ptrMirrorScale;
        } else {
            x = IntuitionBase->MouseX + hot;
            y = IntuitionBase->MouseY + hot;
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
        /* (es_flags: SPRITE_ATTACHED would mean a 16-colour pair; OS 3.2's
         * pointerclass never sets it - a four-plane pointer bitmap arrives
         * as two planes, checked 2026-10-12, so there is nothing more to show) */
        dbg("ChangeExtSprite h=%u ww=%u flags=%x x=%d y=%d\n", n->es_SimpleSprite.height,
            n->es_wordwidth, n->es_flags, n->es_SimpleSprite.x, n->es_SimpleSprite.y);
        decode(n->es_SimpleSprite.posctldata, n->es_SimpleSprite.height, n->es_wordwidth);
        try_apply();
    }
    return 1;
}
