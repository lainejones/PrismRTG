/* SPDX-License-Identifier: GPL-3.0-only */
/* drv_minimig.c: modes, the FORMAT values, row length, start address,
 * palette and switch, on the fake registers (docs/minimig-hw.md) */
#include <assert.h>
#include <stdio.h>
#define MINIMIG_FAKE
#define MM_FAKE_SIZE 0x10000UL      /* the driver's VRAM; vramSize is set below */
#include "../src/drv_minimig.c"
struct ExpansionBase *ExpansionBase=NULL; /* not common: keeps libnix from auto-opening it */
#define REG(off) fakeRegs[(off)/2]
static struct PrismBoard b;
static BOOL set(UWORD w,UWORD h,UBYTE f,UBYTE hz,UWORD pitch)
{
    struct PrismMode m={w,h,f,hz,0,pitch};
    return b.setMode(&b,&m);
}
int main(void)
{
    static const UBYTE fmt[]={PF_CLUT8,PF_RGB565LE,PF_RGB555LE,PF_BGR24,PF_BGRA32};
    static const UWORD val[]={0x03,0x14,0x1C,0x15,0x16},bpp[]={1,2,2,3,4};
    static const UBYTE rgb[]={0x12,0x34,0x56, 0xab,0xcd,0xef};
    int k;
    assert(mm_probe(&b));
    /* A legacy $5001 core has no blitter. All paths must decline without
     * touching VRAM, leaving the renderer to do the work on the CPU. */
    {
        struct PrismSurface s={b.vram,0,32,16,16,2,PF_CLUT8,1,PSF_VRAM};
        struct BitMap bm={0};
        struct PrismPlanar p={&bm,NULL,NULL};
        static const UBYTE bits[]={0xaa,0x55};
        bm.Depth=1; bm.Rows=2; bm.BytesPerRow=1; bm.Planes[0]=(UBYTE *)bits;
        memset(b.vram,0x5a,32);
        assert(!b.ops && !b.fillRect && !b.copyRect && !b.copyBetween &&
               !b.expandRect && !b.drawLine);
        assert(board_fill(&b,&s,0,0,8,2,1)==PR_DECLINED);
        assert(board_copy(&b,&s,&s,0,0,1,0,8,2)==PR_DECLINED);
        assert(board_rop(&b,&s,&s,0,0,8,0,8,2,0x6a)==PR_DECLINED);
        assert(board_expand(&b,&s,0,0,8,2,bits,1,1,0,FALSE)==PR_DECLINED);
        assert(board_line(&b,&s,0,0,7,1,1)==PR_DECLINED);
        assert(board_planar(&b,&p,&s,0,0,0,0,8,2,0xc0,0xff)==PR_DECLINED);
        for (k=0;k<32;k++) assert(b.vram[k]==0x5a);
    }
    b.vramSize=MM_VRAM_SIZE;
    /* one format per depth, each with its FORMAT; rows on 16 bytes */
    for(k=0;k<5;k++){
        ULONG want=((ULONG)1366*bpp[k]+15)&~15UL;
        assert(set(1366,768,fmt[k],0,0));
        assert(REG(R_FORMAT)==val[k]&&REG(R_HSIZE)==1366&&REG(R_VSIZE)==768&&REG(R_STRIDE)==want);
        assert(b.bytesPerRow(&b,1366,768,bpp[k])==want);
    }
    /* the other byte orders are not offered: one format per depth */
    assert(!set(800,600,PF_BGR565LE,0,0)&&!set(800,600,PF_RGB24,0,0)&&!set(800,600,PF_RGBA32,0,0));
    assert(!set(800,600,PF_RGB565BE,0,0)&&!set(800,600,PF_ARGB32,0,0));
    /* sizes: up to 2048, and only what fits the 8 MB */
    assert(set(1920,1080,PF_BGRA32,0,0)&&REG(R_STRIDE)==7680);
    assert(!set(2048,1080,PF_BGRA32,0,0));
    assert(set(2048,2048,PF_RGB565LE,0,0));
    assert(!set(2304,1080,PF_CLUT8,0,0)&&!set(300,200,PF_CLUT8,0,0));
    /* no refresh of its own (MiSTer.ini's), no autoscroll pitch */
    assert(!set(800,600,PF_CLUT8,60,0));
    assert(!set(800,600,PF_CLUT8,0,1024)&&set(800,600,PF_CLUT8,0,800));
    /* the start: the DDR address, on 16 bytes; the high word written only when it changes */
    b.setDisplayStart(&b,0x12340);
    assert(REG(R_ADDR_HI)==0x2701&&REG(R_ADDR_LO)==0x2340);
    REG(R_ADDR_HI)=0xdead;
    b.setDisplayStart(&b,0x1ff07);
    assert(REG(R_ADDR_HI)==0xdead&&REG(R_ADDR_LO)==0xff00);
    b.setDisplayStart(&b,0x7f0000);
    assert(REG(R_ADDR_HI)==0x277f&&REG(R_ADDR_LO)==0);
    /* palette: longwords $00RRGGBB from $300 */
    b.setPalette(&b,254,3,rgb);
    assert(REG(R_CLUT+4*254)==0x0012&&REG(R_CLUT+4*254+2)==0x3456);
    assert(REG(R_CLUT+4*255)==0x00ab&&REG(R_CLUT+4*255+2)==0xcdef);
    assert(REG(R_CLUT+4*256)==0);   /* the 257th pen is not written */
    /* the switch, and a mode set while the Amiga picture is shown keeps it */
    b.setSwitch(&b,FALSE); assert(REG(R_ENABLE)==0);
    assert(set(640,480,PF_CLUT8,0,0)&&REG(R_ENABLE)==0);
    b.setSwitch(&b,TRUE);  assert(REG(R_ENABLE)==1);
    b.shutdown(&b);        assert(REG(R_ENABLE)==0);
    FreeVec(priv.fakeVram);
    puts("Minimig modes passed");return 0;
}
