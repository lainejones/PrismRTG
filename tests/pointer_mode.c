/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Stefan Reinauer
 * Copyright (C) 2026 Laine Jones */
#include <assert.h>
#include <stdio.h>
#include "../src/pointer.c"
struct PrismBoard board;
struct PrismCard cards[MAX_CARDS] = { { &board, 0, 0 } };
struct SignalSemaphore lock;
struct GfxBase *GfxBase=NULL; /* not common: keeps libnix from auto-opening it */
static ULONG uploads, moves, shows, hides;
BOOL present_ready(struct PrismCard *c) { (void)c; return FALSE; }
void present_stop(struct PrismCard *c) { (void)c; }
void present_invalidate(struct PrismCard *c) { (void)c; }
BOOL prism_dragging(void) { return FALSE; }
BOOL prism_display_scroll(struct PrismCard *c, WORD *rx, WORD *ry) { *rx = *ry = 0; return FALSE; }
BOOL prism_autoscroll(struct PrismCard *c, WORD mx, WORD my) { return FALSE; }
WORD prism_span_origin(const struct PrismCard *c) { (void)c; return -1; }
struct PrismCard *prism_span_card(WORD x, WORD *ox) { (void)x; *ox = 0; return NULL; }
BOOL prism_software_pointer(void) { return FALSE; }
/* No shown Prism bitmap: the software sprite has nowhere to draw. */
struct PBitMap *prism_display_bitmap(struct PrismCard *c) { (void)c; return NULL; }
UBYTE prism_ncards(void) { return 1; }
struct Screen *prism_display_layers(struct PrismCard *c,struct PBitMap **f,struct PBitMap **b,
    WORD *top,WORD *bt,UWORD *w,UWORD *h)
{ (void)f;(void)b;(void)bt;(void)w;(void)h;*top=0;return NULL; }
static void cursor_image(struct PrismBoard *b,const UBYTE *pixels,const UBYTE *rgb)
{ (void)b;(void)pixels;(void)rgb;uploads++; }
static void cursor_move(struct PrismBoard *b,WORD x,WORD y)
{ (void)b;(void)x;(void)y;moves++; }
static void cursor_show(struct PrismBoard *b,BOOL visible)
{ (void)b;if(visible)shows++;else hides++; }
int main(void)
{
    board.cursorImage=cursor_image;
    board.cursorMove=cursor_move;
    board.cursorShow=cursor_show;
    haveImage=TRUE;
    board.flags=PBF_HW_CURSOR;
    pointer_on(NULL, &cards[0]);
    assert(on && uploads==1 && moves==1 && shows==1);
    board.flags=0; /* The next format requires a software pointer. */
    pointer_on(NULL, &cards[0]);
    assert(on && pointer_software() && hides==1 && uploads==1 && moves==1 && shows==1);
    pointer_tick();
    assert(uploads==1 && moves==1 && shows==1);
    board.flags=PBF_HW_CURSOR;
    pointer_on(NULL, &cards[0]);
    assert(on && uploads==2 && moves==2 && shows==2);
    puts("Per-mode hardware cursor availability passed");
    return 0;
}
