/* SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include "../src/drv_picasso2.c"
struct ExpansionBase *ExpansionBase=NULL; /* not common: keeps libnix from auto-opening it */
int main(void)
{
    struct PrismBoard b;
    struct P2Priv p;
    struct PrismSurface s={NULL,0,4096,256,128,16,PF_RGB565LE,2,PSF_VRAM};
    static UBYTE regs[65536];
    memset(&b,0,sizeof(b));memset(&p,0,sizeof(p));
    b.priv=&p;b.vramSize=2*1024*1024;b.formats=PF_BIT(PF_RGB565LE);b.regs=regs;
    assert(p2_surface_ok(&b,&s,1024,1024));
    assert(!p2_surface_ok(&b,&s,1025,1));
    assert(!p2_surface_ok(&b,&s,1,1025));
    s.pitch=4096;assert(!p2_surface_ok(&b,&s,1,1));
    p.is5434=TRUE;assert(p2_surface_ok(&b,&s,4096,2048));
    assert(!p2_surface_ok(&b,&s,4097,1));
    s.offset=0x1ffff0;assert(!p2_surface_ok(&b,&s,1,1));
    regs[GRC_D]=1;
    assert(!p2_WaitBlitFor(&b,0));assert(p.failed);
    puts("Cirrus bounds and timeout path passed");return 0;
}
