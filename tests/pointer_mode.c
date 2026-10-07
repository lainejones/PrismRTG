/* SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include <stdio.h>
#include "../src/pointer.c"
struct PrismBoard board;
struct SignalSemaphore lock;
struct GfxBase *GfxBase;
static ULONG uploads, moves, shows, hides;
BOOL present_ready(void) { return FALSE; }
void present_stop(void) {}
BOOL prism_dragging(void) { return FALSE; }
BOOL prism_software_pointer(void) { return FALSE; }
struct Screen *prism_display_layers(struct PBitMap **f,struct PBitMap **b,
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
    pointer_on(NULL);
    assert(on && uploads==1 && moves==1 && shows==1);
    board.flags=0; /* The next format requires a software pointer. */
    pointer_on(NULL);
    assert(on && pointer_software() && hides==1 && uploads==1 && moves==1 && shows==1);
    pointer_tick();
    assert(uploads==1 && moves==1 && shows==1);
    board.flags=PBF_HW_CURSOR;
    pointer_on(NULL);
    assert(on && uploads==2 && moves==2 && shows==2);
    puts("Per-mode hardware cursor availability passed");
    return 0;
}
