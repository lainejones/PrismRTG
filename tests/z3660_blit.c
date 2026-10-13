/* SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include <stdio.h>
#include "../src/drv_z3660.c"
struct ExpansionBase *ExpansionBase=NULL; /* not common: keeps libnix from auto-opening it */
static UBYTE mailbox[GD_CLUT1];
#define U16(o) ((UWORD)(mailbox[o] << 8 | mailbox[(o) + 1]))
#define U32(o) ((ULONG)U16(o) << 16 | U16((o) + 2))
int main(void)
{
    struct PrismBoard b={0};struct Z3Priv p={0};
    static UBYTE regs[0x2000];
    struct PrismSurface s={NULL,0,2048,128,64,16,PF_RGB555BE,2,PSF_VRAM},d=s;
    b.regs=regs;b.priv=&p;b.vramSize=16384;b.ops=&z3_ops;p.gfxdata=mailbox;
    /* a 15-bit fill: the 565 command, the pixel in the high word, pitch in longs */
    assert(board_fill(&b,&s,1,2,8,4,0x7c1f)==PR_DONE);
    assert(R32(&b,REG_DMA_OP)==OP_FILLRECT && mailbox[GD_U8USER0]==CM_565);
    assert(U32(GD_RGB0)==0x7c1f0000UL && U16(GD_PITCH0)==32 && mailbox[GD_MASK]==0xff);
    assert(U16(GD_X0)==1 && U16(GD_Y0)==2 && U16(GD_X1)==8 && U16(GD_Y1)==4);
    /* 8-bit and 32-bit colours go as they are */
    s.format=PF_CLUT8;s.bpp=1;assert(board_fill(&b,&s,0,0,4,4,0x2a)==PR_DONE && U32(GD_RGB0)==0x2a);
    s.format=PF_BGRA32;s.bpp=4;s.pitch=256;s.allocation=4096;assert(board_fill(&b,&s,0,0,4,4,0x11223344UL)==PR_DONE);
    assert(U32(GD_RGB0)==0x11223344UL && mailbox[GD_U8USER0]==CM_32BIT);
    /* inside one bitmap: COPYRECT in bytes */
    s.format=PF_RGB565BE;s.bpp=2;s.pitch=128;s.allocation=2048;d=s;
    assert(board_copy(&b,&s,&s,1,0,2,0,20,4)==PR_DONE);
    assert(R32(&b,REG_DMA_OP)==OP_COPYRECT && mailbox[GD_U8USER0]==CM_8BIT);
    assert(U16(GD_X2)==2 && U16(GD_X0)==4 && U16(GD_X1)==40);
    /* between two bitmaps: COPYRECT_NOMASK, rows from 32-bit words, odd bytes as x */
    d.offset=4096;
    assert(board_copy(&b,&s,&d,1,0,2,0,20,4)==PR_DONE);
    assert(R32(&b,REG_DMA_OP)==OP_COPY_NOMASK && mailbox[GD_MINTERM]==MINTERM_SRC);
    assert(U32(GD_OFFSET1)==0 && U16(GD_X2)==2 && U32(GD_OFFSET0)==4100 && U16(GD_X0)==0);
    assert(U16(GD_X1)==40 && U16(GD_PITCH1)==32);
    /* declined: overlap between two surfaces, a pitch not in longs, not in VRAM,
     * past the end of VRAM, a 24-bit fill */
    d.offset=1024;assert(board_copy(&b,&s,&d,0,0,0,0,8,1)==PR_DECLINED);
    s.pitch=130;assert(board_fill(&b,&s,0,0,8,1,0)==PR_DECLINED);
    s.pitch=128;s.flags=0;assert(board_fill(&b,&s,0,0,8,1,0)==PR_DECLINED);
    s.flags=PSF_VRAM;s.offset=16000;assert(board_fill(&b,&s,0,0,8,1,0)==PR_DECLINED);
    s.offset=0;s.format=PF_RGB24;s.bpp=3;assert(board_fill(&b,&s,0,0,8,1,0)==PR_DECLINED);
    /* text: the template staged, the destination pitch in bytes, JAM2 unless
     * transparent; a solid line with its steps and pitch in longs */
    {
        static UBYTE stage[64];const UBYTE t[4]={0xf0,0x0f,0xaa,0x55};
        p.stage=stage;s.format=PF_RGB565BE;s.bpp=2;s.pitch=128;s.allocation=2048;
        assert(board_expand(&b,&s,3,1,10,2,t,2,0xf800,0x001f,FALSE)==PR_DONE);
        assert(R32(&b,REG_DMA_OP)==OP_TEMPLATE && mailbox[GD_U8USER1]==1);
        assert(U16(GD_PITCH0)==128 && U16(GD_PITCH1)==2 && U32(GD_OFFSET1)==STAGE_OFFSET-VRAM_OFFSET);
        assert(U32(GD_RGB0)==0xf8000000UL && U32(GD_RGB1)==0x001f0000UL);
        assert(stage[0]==0xf0 && stage[1]==0x0f && stage[2]==0xaa && stage[3]==0x55);
        assert(board_expand(&b,&s,3,1,10,2,t,2,0xf800,0,TRUE)==PR_DONE && mailbox[GD_U8USER1]==0);
        assert(board_line(&b,&s,1,9,20,-4,0xffff)==PR_DONE);
        assert(R32(&b,REG_DMA_OP)==OP_DRAWLINE && U16(GD_USER0)==20 && U16(GD_USER1)==0xffff);
        assert(U16(GD_PITCH0)==32 && (WORD)U16(GD_Y1)==-4 && U32(GD_RGB0)==0xffff0000UL);
        s.format=PF_RGB24;s.bpp=3;s.pitch=192;s.allocation=4096;
        assert(board_expand(&b,&s,0,0,8,1,t,1,0,0,FALSE)==PR_DECLINED);
        assert(board_line(&b,&s,0,0,5,5,0)==PR_DECLINED);
    }
    puts("Z3660 fills, copies, text and lines passed");return 0;
}
