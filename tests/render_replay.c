/* SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include <stdio.h>
#include "../src/render.c"
struct PrismBoard board;
struct SignalSemaphore lock;
struct GfxBase *GfxBase;
static ULONG hw_calls;
static enum PrismResult outcome=PR_FAILED;
enum PrismResult pbm_hw_fill(struct PBitMap *p,UBYTE b,UWORD x,UWORD y,UWORD w,UWORD h,ULONG c)
{ (void)b;(void)w;(void)h;(void)c;p->pix[y*p->bpr+x*p->bpp]=0xee;hw_calls++;return outcome; }
enum PrismResult pbm_hw_copy(struct PBitMap *s,struct PBitMap *d,UWORD sx,UWORD sy,UWORD dx,UWORD dy,UWORD w,UWORD h)
{ (void)s;(void)sx;(void)sy;(void)w;(void)h;d->pix[dy*d->bpr+dx*d->bpp]=0xee;hw_calls++;return outcome; }
enum PrismResult pbm_hw_line(struct PBitMap *p,WORD x,WORD y,WORD dx,WORD dy,ULONG c)
{ (void)dx;(void)dy;(void)c;p->pix[y*p->bpr+x*p->bpp]=0xee;hw_calls++;return outcome; }
enum PrismResult pbm_hw_expand(struct PBitMap *p,UWORD x,UWORD y,UWORD w,UWORD h,const UBYTE *src,ULONG mod,ULONG fg,ULONG bg,BOOL tr)
{ (void)w;(void)h;(void)src;(void)mod;(void)fg;(void)bg;(void)tr;p->pix[y*p->bpr+x*p->bpp]=0xee;hw_calls++;return outcome; }
enum PrismResult pbm_hw_planar(const struct PrismPlanar *s,struct PBitMap *p,UWORD sx,UWORD sy,UWORD dx,UWORD dy,UWORD w,UWORD h,UBYTE mt,UBYTE mask)
{ (void)s;(void)p;(void)sx;(void)sy;(void)dx;(void)dy;(void)w;(void)h;(void)mt;(void)mask;return PR_DECLINED; }
int main(void)
{
    UBYTE pixels[64*8*4],bits[8]={0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80};
    struct PBitMap p={0};struct RastPort rp={0};
    struct LineCtx line={&rp,0,0,63,0,0xffff};
    struct TmplCtx text={&rp,bits,0,1,0,0};
    struct PrismOps ops={0};ULONG i,before;
    /* The mocks replace hardware submission after the same render guards. */
    ops.fill=(void *)1;ops.copy=(void *)1;ops.line=(void *)1;ops.expand=(void *)1;
    board.ops=&ops;board.flags=PBF_BLIT_32;
    p.pix=pixels;p.w=64;p.h=8;p.bpr=64;p.bpp=1;p.fmt=PF_CLUT8;p.inVram=TRUE;
    memset(pixels,0x53,sizeof(pixels));
    fill(&p,0,0,63,7,7,255,FALSE);
    for(i=0;i<64*8;i++)assert(pixels[i]==7);
    memset(pixels,0x53,sizeof(pixels));
    line_cb(&p,0,0,63,7,0,0,&line); /* mask zero: no hardware */
    rp.Mask=255;rp.FgPen=9;
    line_cb(&p,0,0,63,7,0,0,&line);
    for(i=0;i<64;i++)assert(pixels[i]==9);
    for(outcome=PR_FAILED;outcome<=PR_RETRY;outcome++) {
        rp.FgPen=12;rp.BgPen=3;
        for(rp.DrawMode=JAM1;rp.DrawMode<=JAM2;rp.DrawMode++) {
            memset(pixels,0x53,sizeof(pixels));
            tmpl_cb(&p,0,0,7,7,0,0,&text);
            for(i=0;i<8;i++) {
                assert(pixels[i*64]==12);
                assert(pixels[i*64+1]==(rp.DrawMode==JAM2 ? 3 : 0x53));
            }
        }
    }
    outcome=PR_FAILED;p.bpp=3;p.fmt=PF_RGB24;p.bpr=64*3;
    memset(pixels,0x53,sizeof(pixels));
    fill(&p,0,0,63,7,17,255,FALSE);
    for(i=0;i<64*8*3;i++)assert(pixels[i]==17);
    p.bpp=1;p.fmt=PF_CLUT8;p.bpr=64;
    before=hw_calls;rp.DrawMode=COMPLEMENT;
    memset(pixels,0x53,sizeof(pixels));
    tmpl_cb(&p,0,0,7,7,0,0,&text);
    assert(hw_calls==before && pixels[0]==(UBYTE)(0x53^255) && pixels[1]==0x53);
    line_cb(&p,0,0,63,7,0,0,&line);
    assert(hw_calls==before);
    {
        struct Surf s={pixels,64,1,NULL,NULL,PF_CLUT8,NULL,64,8,&p};
        memset(pixels,0x53,sizeof(pixels));
        outcome=PR_FAILED;before=hw_calls;
        blit(&s,0,0,&s,1,0,63,2,0xc0,255);
        assert(hw_calls==before+1 && pixels[1]==0xee && pixels[2]==0x53);
    }
    puts("Overwrite replay and RMW guards passed");return 0;
}
