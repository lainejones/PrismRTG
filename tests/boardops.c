/* SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "boardops.h"
static ULONG calls;
static enum PrismResult result;
static enum PrismResult mock_fill(struct PrismBoard *b,const struct PrismSurface *s,
    UWORD x,UWORD y,UWORD w,UWORD h,ULONG colour)
{
    (void)b;(void)x;(void)y;(void)w;(void)h;(void)colour;
    assert(s->format==PF_RGB565BE);calls++;return result;
}
static enum PrismResult mock_copy(struct PrismBoard *b,const struct PrismSurface *s,
    const struct PrismSurface *d,UWORD sx,UWORD sy,UWORD dx,UWORD dy,UWORD w,UWORD h)
{
    (void)b;(void)sx;(void)sy;(void)dx;(void)dy;(void)w;(void)h;
    assert(s->pitch==128 && d->pitch==256);calls++;return result;
}
int main(void)
{
    struct PrismOps ops={.fill=mock_fill,.copy=mock_copy};
    struct PrismBoard b;
    struct PrismSurface s={NULL,0,2048,128,64,16,PF_RGB565BE,2,PSF_VRAM},d=s;
    memset(&b,0,sizeof(b));b.ops=&ops;
    result=PR_DECLINED;
    assert(board_fill(&b,&s,0,0,32,16,0)==PR_DECLINED && calls==1);
    result=PR_DONE;
    assert(board_fill(&b,&s,0,0,32,16,0)==PR_DONE && calls==2);
    assert(board_fill(&b,&s,63,0,2,1,0)==PR_DECLINED && calls==2);
    assert(board_fill(&b,&s,0,0,0,1,0)==PR_DECLINED && calls==2);
    d.pitch=256;d.allocation=4096;d.offset=2048;
    assert(board_copy(&b,&s,&d,0,0,0,0,64,16)==PR_DONE && calls==3);
    d.format=PF_RGB565LE;
    assert(board_copy(&b,&s,&d,0,0,0,0,64,16)==PR_DECLINED && calls==3);
    d=s;d.bpp=1;
    assert(board_fill(&b,&d,0,0,1,1,0)==PR_DECLINED && calls==3);
    d=s;d.allocation=1;
    assert(board_line(&b,&d,0,0,1,1,0)==PR_DECLINED);
    assert(board_line(&b,NULL,0,0,1,1,0)==PR_DECLINED);
    result=PR_FAILED;
    assert(board_fill(&b,&s,0,0,32,16,0)==PR_FAILED && calls==4);
    assert(b.faults==1 && (b.flags & PBF_ACCEL_BROKEN));
    assert(board_fill(&b,&s,0,0,32,16,0)==PR_DECLINED && calls==4);
    puts("board operation contracts passed");
    return 0;
}
