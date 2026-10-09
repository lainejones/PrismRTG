/* SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include <stdio.h>
#include <intuition/screens.h>
#include "../src/present.c"
#include "../src/pointer.c"

struct PrismBoard board;
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
static WORD hardwareY;
static BOOL overlay,backOverlay,failWrite;
static BOOL dragEnabled=TRUE,forceSoftware;
BOOL prism_dragging(void) { return dragEnabled; }
BOOL prism_software_pointer(void) { return forceSoftware; }
void dbg(const char *fmt,...) { (void)fmt; }
LONG call_regs(APTR fn,struct Regs *r) { (void)fn;(void)r;return 0; }
struct PBitMap *prism_display_bitmap(void) { return &front; }
struct Screen *prism_display_layers(struct PBitMap **f,struct PBitMap **b,
    WORD *top,WORD *bt,UWORD *w,UWORD *h)
{ *f=&front;*b=nativeBehind ? NULL : &back;*top=screenTop;*bt=backTop;*w=*h=2;return &screen; }
BOOL p96_pip_active(struct Screen *s) { (void)s;return overlay; }
void p96_pip_compose(struct PBitMap *p,struct Screen *s,WORD top)
{
    (void)top;
    if(s==&screen && overlay)pf_put(p->fmt,0x654321,p->pix+p->bpr+p->bpp);
    if(s==&behindScreen && backOverlay) {
        pf_put(p->fmt,0x2468ac,p->pix);
        pf_put(p->fmt,0x2468ac,p->pix+p->bpr);
    }
}
LONG vram_alloc(ULONG size,UBYTE fmt,ULONG pitch,UWORD w,UWORD h)
{ (void)fmt;(void)pitch;(void)w;(void)h;assert(size==16);allocations++;return failAllocate ? -1 : 128; }
void vram_free(ULONG off) { assert(off==128);frees++; }
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
LONG vram_get_size(ULONG size,UBYTE fmt,ULONG pitch,UWORD w,UWORD h,struct PBitMap *keep)
{ (void)keep;return vram_alloc(size,fmt,pitch,w,h); }
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
{ (void)b;(void)x;hardwareY=y; }
static void cursor_image(struct PrismBoard *b,const UBYTE *pixels,const UBYTE *rgb)
{ (void)b;(void)pixels;(void)rgb; }
int main(void)
{
    struct PrismOps ops={.write=write_pixels};
    UBYTE original[16];ULONG n;
    InitSemaphore(&lock);
    screen.NextScreen=&behindScreen;
    board.vram=video;board.vramSize=sizeof(video);board.ops=&ops;board.setDisplayStart=pan;
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
    on=haveImage=TRUE;posX=posY=0;screenTop=1;failAllocate=TRUE;
    n=allocations;present_tick();pointer_tick();
    assert(!present_ready() && hwVisible && hardwareY==1);
    present_tick();assert(allocations==n+1);
    failAllocate=FALSE;assert(present_reserve(&screen));
    failWrite=TRUE;present_tick();pointer_tick();
    assert(!present_ready() && hwVisible);
    failWrite=FALSE;present_tick();pointer_tick();
    assert(present_ready() && !hwVisible);
    pointer_off();screenTop=0;on=TRUE;dragEnabled=FALSE;
    screenTop=1;present_tick();assert(!allocated && !pointer_software());
    forceSoftware=TRUE;assert(pointer_software());
    pointer_off();
    puts("presentation: cursor, split, damage and scanout lifetime passed");
    return 0;
}
