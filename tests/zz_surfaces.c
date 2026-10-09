/* SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/drv_zz9000.c"
struct ExpansionBase *ExpansionBase=NULL; /* not common: keeps libnix from auto-opening it */
int main(void)
{
    struct PrismBoard b={0};struct ZZPriv p={0};
    static UBYTE regs[0x2000],mailbox[GD_CLUT1];
    struct PrismSurface s={NULL,0,2048,128,64,16,PF_RGB555BE,2,PSF_VRAM},d=s;
    b.regs=regs;b.priv=&p;b.vramSize=16384;b.ops=&zz_ops;
    p.z3=TRUE;p.gfxdata=mailbox;
    /* 15-bit fills and lines go through the 565 commands (see zz_surface_cm):
     * the colour arrives as the raw 16-bit pixel, so the bytes are the same. */
    assert(board_fill(&b,&s,1,2,8,4,0x7c1f)==PR_DONE);
    assert(R16(&b,REG_DMA_OP)==OP_FILLRECT && mailbox[GD_U8USER0]==CM_565);
    assert(*(ULONG *)(mailbox+GD_RGB0)==0x7c1f0000UL);
    assert(board_line(&b,&s,1,2,20,4,0x7c1f)==PR_DONE);
    assert(R16(&b,REG_DMA_OP)==OP_DRAWLINE && mailbox[GD_U8USER0]==CM_565);
    assert(board_copy(&b,&s,&s,1,0,2,0,20,4)==PR_DONE);
    assert(R16(&b,REG_DMA_OP)==OP_COPYRECT && mailbox[GD_U8USER0]==CM_8BIT);
    assert(*(UWORD *)(mailbox+GD_X2)==2 && *(UWORD *)(mailbox+GD_X0)==4);
    assert(*(UWORD *)(mailbox+GD_X1)==40);
    d.offset=4096;
    assert(board_copy(&b,&s,&d,1,0,2,0,20,4)==PR_DONE);
    assert(R16(&b,REG_DMA_OP)==OP_COPY_NOMASK && mailbox[GD_U8USER0]==CM_8BIT);
    s.format=PF_RGB24;s.bpp=3;s.width=40;d=s;
    assert(board_copy(&b,&s,&d,1,0,2,0,20,4)==PR_DONE);
    assert(*(UWORD *)(mailbox+GD_X2)==3 && *(UWORD *)(mailbox+GD_X0)==6);
    assert(*(UWORD *)(mailbox+GD_X1)==60);
    assert(board_fill(&b,&s,0,0,20,4,0)==PR_DECLINED);
    s.pitch=130;assert(board_copy(&b,&s,&s,0,0,0,1,20,4)==PR_DECLINED);
    puts("ZZ9000 15-bit command modes and byte copies passed");return 0;
}
