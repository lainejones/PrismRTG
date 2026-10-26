/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Stefan Reinauer
 * Copyright (C) 2026 Laine Jones */
#include <assert.h>
#include <stdio.h>
#include "../src/render.c"
struct PrismBoard board;
struct SignalSemaphore lock;
struct GfxBase *GfxBase=NULL; /* not common: keeps libnix from auto-opening it */
struct PrismCard cards[MAX_CARDS] = { { &board, 0, 0 } }; /* textBad 0: no depth failed */
static ULONG hw_calls;
static enum PrismResult outcome=PR_FAILED;
void dbg(const char *fmt,...) { (void)fmt; } /* PrismD's debug log */
enum PrismResult pbm_hw_fill(struct PBitMap *p,UBYTE b,UWORD x,UWORD y,UWORD w,UWORD h,ULONG c)
{ (void)b;(void)w;(void)h;(void)c;p->pix[y*p->bpr+x*p->bpp]=0xee;hw_calls++;return outcome; }
enum PrismResult pbm_hw_copy(struct PBitMap *s,struct PBitMap *d,UWORD sx,UWORD sy,UWORD dx,UWORD dy,UWORD w,UWORD h)
{ (void)s;(void)sx;(void)sy;(void)w;(void)h;d->pix[dy*d->bpr+dx*d->bpp]=0xee;hw_calls++;return outcome; }
enum PrismResult pbm_hw_rop(struct PBitMap *s,struct PBitMap *d,UWORD sx,UWORD sy,UWORD dx,UWORD dy,UWORD w,UWORD h,UBYTE mt)
{ (void)s;(void)d;(void)sx;(void)sy;(void)dx;(void)dy;(void)w;(void)h;(void)mt;return PR_DECLINED; } /* the CPU path is what this test replays */
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
    p.card=&cards[0];p.bd=&board;
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
    {
        /* BltMaskBitMapRastPort's "invert through mask" minterm 0x20:
         * icon.library clears the area under the mask to pen 0 first, so
         * combined with the destination it came out an empty box */
        UBYTE spix[8]={0,1,2,3,0,1,2,3},mbits[2]={0xf0,0};
        UWORD spix16[8]={0x0000,0x1234,0xf800,0xffff,0,0,0,0},*d16=(UWORD *)pixels;
        UWORD ptab[256];
        struct PBitMap icon={0};
        struct Surf s={spix,8,1,NULL,NULL,PF_CLUT8,NULL,8,1,&icon};
        struct Surf d={pixels,64,1,NULL,NULL,PF_CLUT8,NULL,64,8,&p};
        struct BlitCtx b={0};
        for(i=0;i<256;i++)ptab[i]=(UWORD)(i*0x0101);
        icon.depth=2; /* a 2-plane icon: pens 0-3 become 3-0, as natively */
        b.src=s;b.mt=0x20;b.mask=255;b.amask=mbits;
        memset(pixels,0,sizeof(pixels));memset(pixels+4,0x53,4);
        blit_masked(&b,&p,&d,0,0,0,0,8,1);
        for(i=0;i<4;i++)assert(pixels[i]==(spix[i]^3));
        for(i=4;i<8;i++)assert(pixels[i]==0x53);
        icon.depth=8;
        blit_masked(&b,&p,&d,0,0,0,0,8,1);
        for(i=0;i<4;i++)assert(pixels[i]==(UBYTE)~spix[i]);
        b.mask=0x0f;memset(pixels,0,8); /* the RastPort's write mask still applies */
        blit_masked(&b,&p,&d,0,0,0,0,8,1);
        for(i=0;i<4;i++)assert(pixels[i]==(UBYTE)(~spix[i]&0x0f));
        icon.depth=2;b.mask=255; /* pens on 16 bit: inverted before the pen table */
        d.bpr=128;d.bpp=2;d.fmt=PF_RGB565BE;d.penTab=ptab;
        memset(pixels,0,sizeof(pixels));
        blit_masked(&b,&p,&d,0,0,0,0,8,1);
        for(i=0;i<4;i++)assert(d16[i]==ptab[spix[i]^3]);
        {
            /* a selected icon without its own image: icon.library blits a
             * 2-plane bitmap with NOT source (0x33) over the image, and on
             * the Amiga only those two planes change - pens 0-3 become
             * 3-0, the destination's other planes stay (native 3.1.4, 16
             * colours). The same through the mask (0x20). */
            UBYTE pl0[2]={0x50,0},pl1[2]={0x30,0}; /* pens 0,1,2,3 in x 0-3 */
            struct BitMap pbm={0};struct Surf ps=s,dd=d;struct BlitCtx pb=b;
            pbm.BytesPerRow=2;pbm.Rows=1;pbm.Depth=2;pbm.Planes[0]=pl0;pbm.Planes[1]=pl1;
            ps.pix=NULL;ps.bpr=2;ps.bpp=0;ps.bm=&pbm;ps.p=NULL;ps.w=16;ps.h=1;
            dd.bpr=64;dd.bpp=1;dd.fmt=PF_CLUT8;dd.penTab=NULL;
            memset(pixels,0,sizeof(pixels));pixels[3]=0x40;
            blit(&ps,0,0,&dd,0,0,4,1,0x33,255);
            assert(pixels[0]==3 && pixels[1]==2 && pixels[2]==1 && pixels[3]==0x40);
            pb.src=ps;pb.mask=255;
            memset(pixels,0,sizeof(pixels));pixels[3]=0x40;
            blit_masked(&pb,&p,&dd,0,0,0,0,8,1);
            assert(pixels[0]==3 && pixels[1]==2 && pixels[2]==1 && pixels[3]==0x40);
        }
        b.src.p=NULL;b.src.pix=(UBYTE *)spix16;b.src.bpr=16;b.src.bpp=2;b.src.fmt=PF_RGB565BE;b.mask=255;
        memset(pixels,0,sizeof(pixels));d16[4]=0x5353;
        blit_masked(&b,&p,&d,0,0,0,0,8,1);
        for(i=0;i<4;i++)assert(d16[i]==(UWORD)~spix16[i]);
        assert(d16[4]==0x5353);
    }
    puts("Overwrite replay and RMW guards passed");return 0;
}
