/* SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include <stdio.h>
#include "../src/drv_z3660.c"
#include "../src/prefs.c"  /* the slot table */
struct ExpansionBase *ExpansionBase=NULL; /* not common: keeps libnix from auto-opening it */
static LONG chk(UWORD w,UWORD h,UBYTE f,UBYTE hz,ULONG *bpr,ULONG *orig)
{
    struct PrismMode m={w,h,f,hz,0};UBYTE cm;LONG v=z3_check(&m,&cm,orig);
    *bpr=m.bytesPerRow;return v;
}
/* not offered at 32 bits: the firmware shows these line-doubled */
static int no32(UWORD w,UWORD h)
{
    return (w==1920&&(h==1080||h==1200))||(w==1600&&h==1200)||(w==1680&&h==1050)||
           (w==2560&&h==1440);
}
int main(void)
{
    static const UBYTE fmt[]={PF_CLUT8,PF_RGB565BE,PF_RGB555BE,PF_BGRA32},bpp[]={1,2,2,4},cm[]={0,1,3,2};
    ULONG bpr,orig;int i,k;
    /* every PrismPrefs size the firmware has is a mode at every depth the board scans out,
     * unless its rows would not be whole longs */
    for(i=0;i<PREFS_NFIXED;i++)
        for(k=0;k<4;k++){
            UWORD w=prefs_sizes[i][0],h=prefs_sizes[i][1];LONG v=chk(w,h,fmt[k],0,&bpr,&orig);
            if(((ULONG)w*bpp[k])&3){assert(v<0);continue;}   /* rows not whole longs: 1366x768 8-bit */
            if((w==960&&h==540)||(w==1440&&h==900)){assert(v<0);continue;}   /* no firmware mode */
            if(k==3&&no32(w,h)){assert(v<0);continue;}   /* docs/z3660-hw.md 2 */
            assert(v>=0 && bpr==(ULONG)w*bpp[k] && ((v>>8)&0xf)==cm[k]);
            if(w<640) assert((v>>12)==3 && orig==((ULONG)w*2<<16|h*2));
            else assert((v>>12)==0 && orig==((ULONG)w<<16|h));
        }
    /* 320x200 is doubled into 640x400 (16), 320x240 into 640x480 (2) */
    assert((chk(320,200,PF_CLUT8,0,&bpr,&orig)&0xff)==16);
    assert((chk(320,240,PF_CLUT8,0,&bpr,&orig)&0xff)==2);
    /* refresh: 0 = the first match (1080p 60 Hz, 5); 50 Hz has its own id (7) */
    assert((chk(1920,1080,PF_RGB565BE,0,&bpr,&orig)&0xff)==5);
    assert((chk(1920,1080,PF_RGB565BE,50,&bpr,&orig)&0xff)==7);
    assert(chk(1920,1080,PF_RGB565BE,75,&bpr,&orig)<0);
    /* no 24-bit mode, no PC byte order, nothing the firmware has no mode for */
    assert(chk(800,600,PF_RGB24,0,&bpr,&orig)<0);
    assert(chk(800,600,PF_RGB565LE,0,&bpr,&orig)<0);
    assert(chk(1000,700,PF_CLUT8,0,&bpr,&orig)<0);
    assert(chk(300,200,PF_CLUT8,0,&bpr,&orig)<0);
    /* 32-bit up to a 108 MHz pixel clock: 1600x900 and 1280x1024, not 1680x1050 */
    assert(chk(1280,1024,PF_BGRA32,0,&bpr,&orig)>=0);
    assert(chk(1600,900,PF_BGRA32,0,&bpr,&orig)>=0);
    assert(chk(1680,1050,PF_BGRA32,0,&bpr,&orig)<0);
    assert(chk(1920,1080,PF_BGRA32,0,&bpr,&orig)<0);
    assert(chk(1920,1080,PF_RGB565BE,0,&bpr,&orig)>=0);
    assert((chk(2560,1440,PF_RGB565BE,0,&bpr,&orig)&0xff)==11);
    assert(chk(2560,1440,PF_BGRA32,0,&bpr,&orig)<0);
    /* an oversized (autoscroll) pitch is refused: rows step by the mode's width */
    {struct PrismMode m={800,600,PF_RGB565BE,0,0,1024};UBYTE c;assert(z3_check(&m,&c,&orig)<0);
     m.pitchPixels=800;assert(z3_check(&m,&c,&orig)>=0&&m.bytesPerRow==1600);}
    puts("Z3660 modes passed");return 0;
}
