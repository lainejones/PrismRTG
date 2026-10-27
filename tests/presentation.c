/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Stefan Reinauer
 * Copyright (C) 2026 Laine Jones */
#include <assert.h>
#include <stdio.h>
#include <intuition/screens.h>
#include "../src/present.c"
#include "../src/pointer.c"

struct PrismBoard board;
struct PrismCard cards[MAX_CARDS] = { { &board, 0, 0 } };
struct SignalSemaphore lock;
struct GfxBase *GfxBase=NULL; /* not common: keeps libnix from auto-opening it */
struct IntuitionBase *IntuitionBase;
APTR o_MoveSprite,o_ChangeSprite,o_ChangeExtSpriteA;
static struct Screen screen,behindScreen;
static struct PBitMap front,back;
static UBYTE video[256],source[16],background[16];
static WORD screenTop,backTop;
static BOOL nativeBehind;
static ULONG display,writes,frees,allocations;
static BOOL failAllocate,hwVisible;
static WORD hardwareX,hardwareY;
static BOOL overlay,backOverlay,failWrite;
static BOOL dragEnabled=TRUE,forceSoftware;
BOOL prism_dragging(void) { return dragEnabled; }
BOOL prism_display_scroll(struct PrismCard *c, WORD *rx, WORD *ry) { *rx = *ry = 0; return FALSE; }
BOOL prism_autoscroll(struct PrismCard *c, WORD mx, WORD my) { return FALSE; }
BOOL prism_software_pointer(void) { return forceSoftware; }
void dbg(const char *fmt,...) { (void)fmt; }
LONG call_regs(APTR fn,struct Regs *r) { (void)fn;(void)r;return 0; }
struct PBitMap *prism_display_bitmap(struct PrismCard *c) { (void)c; return &front; }
UBYTE prism_ncards(void) { return 1; }
struct Screen *prism_display_layers(struct PrismCard *c,struct PBitMap **f,struct PBitMap **b,
    WORD *top,WORD *bt,UWORD *w,UWORD *h)
{ *f=&front;*b=nativeBehind ? NULL : &back;*top=screenTop;*bt=backTop;*w=*h=2;return &screen; }
BOOL p96_pip_active(struct Screen *s) { (void)s;return overlay; }
/* one card here: what a drag uncovers is simply the next screen */
struct Screen *prism_back_screen(struct PrismCard *c,struct Screen *s) { (void)c;return s ? s->NextScreen : NULL; }
/* a span screen: where this board's half starts (-1: none); one board here,
 * so the pointer never changes board */
static WORD spanOrigin=-1;
WORD prism_span_origin(const struct PrismCard *c) { (void)c;return spanOrigin; }
struct PrismCard *prism_span_card(WORD x,WORD *ox) { (void)x;*ox=spanOrigin;return spanOrigin>=0 ? &cards[0] : NULL; }
/* the split: what the board was asked, and a board (a VGA line compare)
 * that only restarts the lower part at VRAM 0 */
static ULONG splitTop,splitBottom,toVram0;
static UWORD splitLine;
static BOOL splitZeroOnly;
static BOOL set_split(struct PrismBoard *b,ULONG top,ULONG bottom,UWORD line)
{
    (void)b;
    if (line && splitZeroOnly && bottom) return FALSE;
    splitTop=top;splitBottom=bottom;splitLine=line;return TRUE;
}
BOOL pbm_to_vram(struct PBitMap *p) { p->inVram=TRUE;return TRUE; }
BOOL pbm_to_vram0(struct PBitMap *p) { toVram0++;p->vramOff=0;return TRUE; }
volatile ULONG paletteGen; ULONG pipGen;
BOOL p96_pip_volatile(struct Screen *s) { (void)s;return FALSE; }
/* the fixture's PIP sits on row 1 of the frame */
BOOL p96_pip_rows(struct Screen *s,WORD top,WORD *y0,WORD *y1)
{ if(s!=&screen || !overlay)return FALSE;*y0=1+top;*y1=2+top;return TRUE; }
/* the front's VRAM is source[] here: a band copy reads it from there */
static ULONG bandCopies;
static void copy_between(struct PrismBoard *b,ULONG src,ULONG spitch,ULONG dst,ULONG dpitch,UWORD w,UWORD h)
{ UWORD r;(void)b;for(r=0;r<h;r++)memcpy(video+dst+r*dpitch,(src<16 ? source : video)+src+r*spitch,w);bandCopies++; }
void p96_pip_compose(struct PBitMap *p,struct Screen *s,WORD top,WORD sk0,WORD sk1)
{
    (void)sk0;(void)sk1;
    (void)top;
    if(s==&screen && overlay)pf_put(p->fmt,0x654321,p->pix+p->bpr+p->bpp);
    if(s==&behindScreen && backOverlay) {
        pf_put(p->fmt,0x2468ac,p->pix);
        pf_put(p->fmt,0x2468ac,p->pix+p->bpr);
    }
}
LONG vram_alloc(struct PrismCard *c,ULONG size,UBYTE fmt,ULONG pitch,UWORD w,UWORD h)
{ (void)fmt;(void)pitch;(void)w;(void)h;assert(size==16);allocations++;return failAllocate ? -1 : 128; }
void vram_free(struct PrismCard *c,ULONG off) { (void)c; assert(off==128);frees++; }
/* render.c's nearest pen (same weighting), without linking the renderer. */
UBYTE pen_nearest(const ULONG *tab,ULONG c)
{
    UWORD i,best=0;LONG bd=0x7fffffff;
    if(!tab)return 0;
    for(i=0;i<256;i++) {
        LONG dr=(LONG)((tab[i]>>16)&255)-(LONG)((c>>16)&255);
        LONG dg=(LONG)((tab[i]>>8)&255)-(LONG)((c>>8)&255);
        LONG db=(LONG)(tab[i]&255)-(LONG)(c&255);
        LONG dd=dr*dr*3+dg*dg*4+db*db*2;
        if(dd<bd) { bd=dd;best=i;if(!dd)break; }
    }
    return best;
}
/* No other bitmaps to evict here, so this is vram_alloc (as in bitmap.c). */
LONG vram_get_size(struct PrismCard *c,ULONG size,UBYTE fmt,ULONG pitch,UWORD w,UWORD h,struct PBitMap *keep)
{ (void)keep;return vram_alloc(c,size,fmt,pitch,w,h); }
void pbm_surface(const struct PBitMap *p,struct PrismSurface *s)
{ s->offset=p->vramOff;s->allocation=p->bpr*p->h; }
BOOL pbm_upload(struct PBitMap *p)
{ memcpy(video+p->vramOff,p->pix,p->bpr*p->h);return TRUE; }
static void pan(struct PrismBoard *b,ULONG off) { (void)b;display=off; }
static BOOL write_pixels(struct PrismBoard *b,const struct PrismSurface *s,
    ULONG off,const void *data,ULONG count)
{ (void)b;if(failWrite)return FALSE;memcpy(video+s->offset+off,data,count);writes+=count;return TRUE; }
static void cursor_show(struct PrismBoard *b,BOOL visible)
{ (void)b;hwVisible=visible; }
static void cursor_move(struct PrismBoard *b,WORD x,WORD y)
{ (void)b;hardwareX=x;hardwareY=y; }
static void cursor_image(struct PrismBoard *b,const UBYTE *pixels,const UBYTE *rgb)
{ (void)b;(void)pixels;(void)rgb; }
int main(void)
{
    struct PrismOps ops={.write=write_pixels};
    UBYTE original[16];ULONG n;
    InitSemaphore(&lock);
    front.card=back.card=&cards[0];front.bd=back.bd=&board;
    screen.NextScreen=&behindScreen;
    board.vram=video;board.vramSize=sizeof(video);board.ops=&ops;board.setDisplayStart=pan;
    board.copyBetween=copy_between;
    /* the front and back are written into directly here (no PrismD call):
     * what a LockBitMap client does - composed every tick */
    front.direct=back.direct=TRUE;
    front.w=front.h=back.w=back.h=2;front.bpp=back.bpp=3;
    front.fmt=back.fmt=PF_RGB24;front.bpr=back.bpr=8;
    front.pix=source;back.pix=background;front.inVram=TRUE;
    pf_put(PF_RGB24,0xabcdef,source);pf_put(PF_RGB24,0x123456,background);
    memcpy(original,source,sizeof(source));
    board.flags=PBF_SHADOW;on=haveImage=TRUE;image[0]=1;
    colours[0]=0x11;colours[1]=0x22;colours[2]=0x33;
    front.uploaded=AllocVec(16,MEMF_ANY);
    assert(front.uploaded);
    /* The pointer alone is a software sprite in the shown bitmap (pointer.c),
     * not a reason to compose: nothing is presented or written. */
    present_tick();
    assert(front.uploaded && !allocated && !(board.flags&PBF_PRESENT) && !writes);
    /* A PIP is. In shadow mode the shown bitmap's own VRAM is the scanout,
     * and the composed frame carries the pointer (pointer_compose). */
    overlay=TRUE;present_tick();
    assert(!front.uploaded);
    assert(display==0 && (board.flags&PBF_PRESENT));
    assert(pf_get(PF_RGB24,video)==0x112233 && !memcmp(source,original,16));
    assert(pf_get(PF_RGB24,video+11)==0x654321);
    n=writes;present_tick();assert(writes==n);
    posX=1;present_tick();
    assert(pf_get(PF_RGB24,video)==0xabcdef && pf_get(PF_RGB24,video+3)==0x112233);
    on=FALSE;overlay=FALSE;present_tick();
    assert(!(board.flags&PBF_PRESENT) && !memcmp(video,source,16) && !frees);
    board.flags=0;screenTop=1;present_tick();
    assert(display==128 && pf_get(PF_RGB24,video+128)==0x123456);
    assert(pf_get(PF_RGB24,video+136)==0xabcdef);
    assert(bandCopies==1);               /* the front's row came by blitter */
    backTop=1;present_tick();
    assert(pf_get(PF_RGB24,video+128)==0);
    backTop=-1;
    pf_put(PF_RGB24,0x789abc,background+8);present_tick();
    assert(pf_get(PF_RGB24,video+128)==0x789abc);
    backTop=0;nativeBehind=TRUE;present_tick();
    assert(pf_get(PF_RGB24,video+128)==0);
    nativeBehind=FALSE;backOverlay=TRUE;present_tick();
    assert(pf_get(PF_RGB24,video+128)==0x2468ac);
    assert(pf_get(PF_RGB24,video+136)==0xabcdef);
    backOverlay=FALSE;
    screenTop=0;overlay=TRUE;present_tick();
    assert(pf_get(PF_RGB24,video+139)==0x654321 && !memcmp(source,original,16));
    n=writes;failWrite=TRUE;source[0]=0x44;present_tick();assert(writes==n);
    failWrite=FALSE;present_tick();assert(video[128]==0x44);
    overlay=FALSE;present_tick();assert(display==0 && frees==1);
    /* A clipped software sprite uses the visible source column. Without
     * composition it is drawn into the shown bitmap, over a save-under. */
    {
        UBYTE under[16];
        board.flags=PBF_SHADOW;on=TRUE;posX=-1;posY=0;image[1]=1;imgW=2;imgH=1;
        memcpy(under,source,sizeof(under));
        present_tick();assert(!allocated);
        pointer_tick();
        assert(swOn==&front && pf_get(PF_RGB24,source)==0x112233);
        assert(!memcmp(source+3,under+3,sizeof(under)-3));
        pointer_off();
        assert(!swOn && !allocated && !memcmp(source,under,sizeof(under)));
    }
    {
        static ULONG dstPalette[256],srcPalette[256];
        UBYTE srcPixels[2]={1,0},dstPixels[2],map[256];
        struct PBitMap src={0},dst={0};
        src.card=dst.card=&cards[0];src.bd=dst.bd=&board;
        src.w=dst.w=2;src.h=dst.h=1;src.bpr=dst.bpr=2;
        src.bpp=dst.bpp=1;src.fmt=dst.fmt=PF_CLUT8;
        src.pix=srcPixels;dst.pix=dstPixels;
        src.rgbTab=srcPalette;dst.rgbTab=dstPalette;
        srcPalette[1]=0x123456;dstPalette[3]=0x123456;
        composite_clut_map(&dst,&src,map);
        assert(map[1]==3 && map[0]==0);
        composite_row_mapped(&dst,0,&src,0,map);
        assert(dstPixels[0]==3 && dstPixels[1]==0 && srcPixels[0]==1);
    }
    /* A failed allocation or first upload must not hide a working sprite.
     * Retry allocation after a cooldown, or for an explicit new PIP. */
    board.flags=PBF_HW_CURSOR;board.cursorShow=cursor_show;
    board.cursorMove=cursor_move;board.cursorImage=cursor_image;
    on=haveImage=TRUE;posX=0;posY=1;screenTop=1;failAllocate=TRUE; /* display position */
    n=allocations;present_tick();pointer_tick();
    assert(!present_ready(&cards[0]) && hwVisible && hardwareY==1);
    present_tick();assert(allocations==n+1);
    failAllocate=FALSE;assert(present_reserve(&screen));
    failWrite=TRUE;present_tick();pointer_tick();
    assert(!present_ready(&cards[0]) && hwVisible);
    /* the card's cursor stays on over the composed drag, at display rows */
    failWrite=FALSE;present_tick();pointer_tick();
    assert(present_ready(&cards[0]) && hwVisible && hardwareY==1);
    /* on a span screen the card's cursor is placed in its own half */
    spanOrigin=0;posX=1;dirtyPos=TRUE;pointer_tick();assert(hwVisible && hardwareX==1);
    spanOrigin=1;dirtyPos=TRUE;pointer_tick();assert(hardwareX==0);
    spanOrigin=-1;
    pointer_off();screenTop=0;on=TRUE;dragEnabled=FALSE;
    screenTop=1;present_tick();assert(!allocated && !pointer_software());
    forceSoftware=TRUE;assert(pointer_software());
    pointer_off();
    /* A board with a split shows the drag by itself: the lines above from
     * the screen behind's bitmap, the rest from the front's - nothing
     * composed or written. Ending the drag puts the display on the front. */
    on=forceSoftware=FALSE;dragEnabled=TRUE;board.flags=0;board.setSplit=set_split;
    front.direct=back.direct=FALSE;front.inVram=back.inVram=TRUE;
    front.vramOff=64;back.vramOff=32;backTop=0;screenTop=1;n=writes;
    present_tick();
    assert(present_split(&cards[0]) && !allocated && writes==n);
    assert(splitTop==32 && splitBottom==64 && splitLine==1 && !toVram0);
    screenTop=0;present_tick();
    assert(!present_split(&cards[0]) && splitLine==0 && display==64);
    /* One that restarts at VRAM 0 only: the front moves there first. */
    splitZeroOnly=TRUE;screenTop=1;present_tick();
    assert(present_split(&cards[0]) && toVram0==1 && front.vramOff==0 && splitBottom==0 && splitLine==1);
    screenTop=0;present_tick();
    assert(!present_split(&cards[0]) && display==0);
    puts("presentation: cursor, split, span cursor, damage and scanout lifetime passed");
    return 0;
}
