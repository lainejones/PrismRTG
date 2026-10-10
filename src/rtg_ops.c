/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Stefan Reinauer
 * Copyright (C) 2026 Laine Jones */
#include <exec/memory.h>
#include <graphics/rastport.h>
#include <proto/exec.h>
#include <string.h>
#include "rtg_ops.h"

/* Preserve BoardInfo in a0 across indirect register-ABI calls. */
#pragma GCC optimize ("no-optimize-sibling-calls")

static const RGBFTYPE rtg_formats[PF_COUNT] = {
    RGBFB_CLUT,RGBFB_R5G6B5,RGBFB_R5G6B5PC,RGBFB_R5G5B5,RGBFB_R5G5B5PC,
    RGBFB_R8G8B8,RGBFB_B8G8R8,RGBFB_A8R8G8B8,RGBFB_B8G8R8A8,RGBFB_R8G8B8A8,
    RGBFB_B5G6R5PC,RGBFB_B5G5R5PC
};
static void rtg_wait(struct BoardInfo *bi) { if (bi->WaitBlitter) bi->WaitBlitter(bi); }
static ULONG rtg_pen(ULONG c,UBYTE bpp)
{
    return bpp==3 ? ((c&255)<<16)|(c&0xff00)|((c>>16)&255) : c;
}
static UBYTE rtg_bpp(RGBFTYPE f)
{
    UBYTE i;
    for(i=0;i<PF_COUNT;i++) if(rtg_formats[i]==f) return board_bpp(i);
    return 0;
}
static ULONG rtg_colour(UBYTE f,ULONG rgb)
{
    UBYTE r=rgb>>16,g=rgb>>8,b=rgb;
    UWORD v;
    switch(f) {
    case PF_RGB24: return ((ULONG)b<<16)|((ULONG)g<<8)|r;
    case PF_BGR24: return rgb & 0xffffff;
    case PF_ARGB32: return rgb & 0xffffff;
    case PF_BGRA32: return ((ULONG)b<<24)|((ULONG)g<<16)|((ULONG)r<<8);
    case PF_RGBA32: return rgb<<8;
    }
    if(f==PF_BGR565LE || f==PF_BGR555LE) { UBYTE t=r;r=b;b=t; }
    v=(f==PF_RGB555BE || f==PF_RGB555LE || f==PF_BGR555LE) ?
        ((r>>3)<<10)|((g>>3)<<5)|(b>>3) : ((r>>3)<<11)|((g>>2)<<5)|(b>>3);
    return f==PF_RGB565LE || f==PF_RGB555LE || f==PF_BGR565LE || f==PF_BGR555LE ?
        (UWORD)((v<<8)|(v>>8)) : v;
}
/* One pixel in memory order (P96 hands 24-bit pens first byte low). */
static void rtg_put(UBYTE *dst,ULONG pen,UBYTE bpp,UBYTE mask)
{
    UBYTE i;
    if(bpp==1) { *dst=(*dst&~mask)|((UBYTE)pen&mask);return; }
    pen=rtg_pen(pen,bpp);
    for(i=0;i<bpp;i++) dst[i]=pen>>(8*(bpp-i-1));
}
/* BlitTemplateDefault: what a card driver calls for a template it cannot
 * expand itself. Also what the adapters install before InitCard, so a
 * card that leaves BlitTemplate alone still has one (P96 register ABI). */
void rtg_template_default(struct BoardInfo *bi __asm("a0"),struct RenderInfo *ri __asm("a1"),
    struct Template *t __asm("a2"),WORD x __asm("d0"),WORD y __asm("d1"),WORD w __asm("d2"),
    WORD h __asm("d3"),UBYTE mask __asm("d4"),RGBFTYPE fmt __asm("d7"))
{
    UBYTE bpp=rtg_bpp(fmt);
    WORD xx,yy;
    rtg_wait(bi);
    if(!bpp || w<=0 || h<=0) return;
    for(yy=0;yy<h;yy++) {
        const UBYTE *s=(const UBYTE *)t->Memory+(LONG)yy*t->BytesPerRow;
        UBYTE *d=(UBYTE *)ri->Memory+(LONG)(y+yy)*ri->BytesPerRow+(LONG)x*bpp;
        for(xx=0;xx<w;xx++,d+=bpp) {
            ULONG bit=xx+t->XOffset;
            BOOL set=(s[bit>>3]&(0x80>>(bit&7)))!=0;
            if(t->DrawMode&INVERSVID) set=!set;
            if(t->DrawMode&COMPLEMENT) {
                UBYTE i;
                if(set) for(i=0;i<bpp;i++) d[i]^=bpp==1 ? mask : 0xff;
            } else if(set || (t->DrawMode&JAM2)) {
                rtg_put(d,set ? t->FgPen : t->BgPen,bpp,mask);
            }
        }
    }
}
static UBYTE rtg_index(const struct BitMap *bm,UWORD x,UWORD y)
{
    UBYTE i,v=0,bit=0x80>>(x&7);
    ULONG off=(ULONG)y*bm->BytesPerRow+x/8;
    for(i=0;i<bm->Depth;i++) {
        const UBYTE *plane=bm->Planes[i];
        if(plane==(UBYTE *)~0UL || (plane && (plane[off]&bit))) v|=1<<i;
    }
    return v;
}
void rtg_planar_chunky(struct BoardInfo *bi __asm("a0"),struct BitMap *bm __asm("a1"),
    struct RenderInfo *ri __asm("a2"),WORD sx __asm("d0"),WORD sy __asm("d1"),
    WORD dx __asm("d2"),WORD dy __asm("d3"),WORD w __asm("d4"),WORD h __asm("d5"),
    UBYTE mt __asm("d6"),UBYTE mask __asm("d7"))
{
    WORD x,y;
    rtg_wait(bi);
    /* The surface hook dispatches only plain copies with a full mask. */
    if(mt!=12) return;
    for(y=0;y<h;y++) for(x=0;x<w;x++) {
        UBYTE *d=(UBYTE *)ri->Memory+(LONG)(dy+y)*ri->BytesPerRow+dx+x;
        *d=(*d & ~mask)|(rtg_index(bm,sx+x,sy+y)&mask);
    }
}
void rtg_planar_direct(struct BoardInfo *bi __asm("a0"),struct BitMap *bm __asm("a1"),
    struct RenderInfo *ri __asm("a2"),struct ColorIndexMapping *cim __asm("a3"),
    WORD sx __asm("d0"),WORD sy __asm("d1"),WORD dx __asm("d2"),WORD dy __asm("d3"),
    WORD w __asm("d4"),WORD h __asm("d5"),UBYTE mt __asm("d6"),UBYTE mask __asm("d7"))
{
    WORD x,y;UBYTE k,bpp=rtg_bpp(ri->RGBFormat);
    rtg_wait(bi);
    if(mt!=12 || mask!=255 || !bpp) return;
    for(y=0;y<h;y++) for(x=0;x<w;x++) {
        ULONG c=rtg_pen(cim->Colors[rtg_index(bm,sx+x,sy+y)],bpp);
        UBYTE *d=(UBYTE *)ri->Memory+(LONG)(dy+y)*ri->BytesPerRow+(LONG)(dx+x)*bpp;
        for(k=0;k<bpp;k++) d[k]=c>>(8*(bpp-k-1));
    }
}
/* The firmware version is shared by fixed and broken UAE hosts. Exercise
 * the actual hook in saved VRAM before enabling scanout. UAE host traps
 * require a VRAM destination, even when their source is in system RAM. */
BOOL rtg_probe_planar(struct BoardInfo *bi, ULONG formats)
{
    struct ColorIndexMapping *map;
    struct BitMap bm;
    struct RenderInfo ri;
    UBYTE saved[32], *pixels, plane[2] = {0x50, 0};
    UBYTE f, i, k;
    BOOL ok = TRUE;
    if (!bi->BlitPlanar2Direct || !bi->WaitBlitter ||
        !bi->MemoryBase || bi->MemorySize < sizeof(saved)) return FALSE;
    map = AllocVec(sizeof(*map), MEMF_PUBLIC | MEMF_CLEAR);
    if (!map) return FALSE;
    pixels = bi->MemoryBase + bi->MemorySize - sizeof(saved);
    rtg_wait(bi);
    CopyMem(pixels, saved, sizeof(saved));
    memset(&bm, 0, sizeof(bm));
    bm.BytesPerRow = 2; bm.Rows = 1; bm.Depth = 1; bm.Planes[0] = plane;
    for (f = 0; f < PF_COUNT && ok; f++) {
        UBYTE bpp = board_bpp(f);
        if (bpp == 1 || !(formats & PF_BIT(f))) continue;
        memset(pixels, 0xa5, sizeof(saved));
        memset(&ri, 0, sizeof(ri));
        ri.Memory = pixels; ri.BytesPerRow = 4 * bpp; ri.RGBFormat = rtg_formats[f];
        map->ColorMask = bpp == 2 ? 0xffff : 0xffffffff;
        map->Colors[0] = rtg_colour(f, 0x123456);
        map->Colors[1] = rtg_colour(f, 0xa1b2c3);
        bi->BlitPlanar2Direct(bi, &bm, &ri, map, 0, 0, 0, 0, 4, 1, 12, 255);
        rtg_wait(bi);
        for (i = 0; i < 4; i++) {
            ULONG c = rtg_pen(map->Colors[i & 1], bpp);
            for (k = 0; k < bpp; k++)
                if (pixels[i*bpp+k] != (UBYTE)(c >> (8*(bpp-k-1)))) ok = FALSE;
        }
        for (i = 4*bpp; i < sizeof(saved); i++) if (pixels[i] != 0xa5) ok = FALSE;
    }
    CopyMem(saved, pixels, sizeof(saved));
    FreeVec(map);
    return ok;
}
static UBYTE *rtg_address(struct PrismBoard *b,const struct PrismSurface *s,ULONG off)
{
    struct BoardInfo *bi=b->priv;
    RGBFTYPE f=rtg_formats[s->format];
    struct RenderInfo ri={b->vram+s->offset,s->pitch,0,f};
    rtg_wait(bi);
    if(bi->SetMemoryMode) bi->SetMemoryMode(bi,f);
    return bi->CalculateMemory ? bi->CalculateMemory(bi,b->vram+s->offset+off,&ri,f) :
                                b->vram+s->offset+off;
}
static BOOL rtg_render(struct PrismBoard *b,const struct PrismSurface *s,struct RenderInfo *ri)
{
    if(s->format>=PF_COUNT || !(b->formats & PF_BIT(s->format)) || s->pitch>32767 ||
       s->width>32767 || s->height>32767 || s->offset>b->vramSize || s->allocation>b->vramSize-s->offset) return FALSE;
    ri->Memory=rtg_address(b,s,0);ri->BytesPerRow=s->pitch;ri->pad=0;
    ri->RGBFormat=rtg_formats[s->format];
    return ri->Memory!=NULL;
}
enum PrismResult rtg_fill(struct PrismBoard *b,const struct PrismSurface *d,
    UWORD x,UWORD y,UWORD w,UWORD h,ULONG c)
{
    struct BoardInfo *bi=b->priv;struct RenderInfo ri;
    if((b->flags & PBF_BANKED) || (bi->Flags & BIF_NOBLITTER) || !bi->WaitBlitter || !bi->FillRect || !rtg_render(b,d,&ri)) return PR_DECLINED;
    bi->FillRect(bi,&ri,x,y,w,h,rtg_pen(c,d->bpp),255,ri.RGBFormat);rtg_wait(bi);
    return PR_DONE;
}
enum PrismResult rtg_copy(struct PrismBoard *b,const struct PrismSurface *s,
    const struct PrismSurface *d,UWORD sx,UWORD sy,UWORD dx,UWORD dy,UWORD w,UWORD h)
{
    struct BoardInfo *bi=b->priv;struct RenderInfo ri;
    if((b->flags & PBF_BANKED) || (bi->Flags & BIF_NOBLITTER) || !bi->WaitBlitter || s->offset!=d->offset || s->pitch!=d->pitch || !bi->BlitRect || !rtg_render(b,d,&ri))
        return PR_DECLINED;
    bi->BlitRect(bi,&ri,sx,sy,dx,dy,w,h,255,ri.RGBFormat);rtg_wait(bi);
    return PR_DONE;
}
enum PrismResult rtg_expand(struct PrismBoard *b,const struct PrismSurface *d,
    UWORD x,UWORD y,UWORD w,UWORD h,const UBYTE *src,ULONG mod,ULONG fg,ULONG bg,BOOL tr)
{
    struct BoardInfo *bi=b->priv;struct RenderInfo ri;struct Template t;
    if((b->flags & PBF_BANKED) || (bi->Flags & BIF_NOBLITTER) || !bi->WaitBlitter || !bi->BlitTemplate || !bi->BlitTemplateDefault || mod>32767 || !rtg_render(b,d,&ri))
        return PR_DECLINED;
    t.Memory=(APTR)src;t.BytesPerRow=mod;t.XOffset=0;t.DrawMode=tr ? JAM1:JAM2;
    t.FgPen=rtg_pen(fg,d->bpp);t.BgPen=rtg_pen(bg,d->bpp);
    bi->BlitTemplate(bi,&ri,&t,x,y,w,h,255,ri.RGBFormat);rtg_wait(bi);
    return PR_DONE;
}
enum PrismResult rtg_planar(struct PrismBoard *b,const struct PrismPlanar *s,
    const struct PrismSurface *d,UWORD sx,UWORD sy,UWORD dx,UWORD dy,UWORD w,UWORD h,
    UBYTE mt,UBYTE mask)
{
    struct BoardInfo *bi=b->priv;struct RenderInfo ri;
    /* Serialized by the board lock; keep 1 KB off application task stacks. */
    static struct ColorIndexMapping cim;
    UWORD i;
    if((ULONG)sx+w>32768 || (ULONG)sy+h>32768 ||
       (b->flags & PBF_BANKED) || (bi->Flags & BIF_NOBLITTER) || !bi->WaitBlitter || (mt & 0xf0)!=0xc0 || mask!=255 || !rtg_render(b,d,&ri)) return PR_DECLINED;
    if(d->bpp==1) {
        if(!bi->BlitPlanar2Chunky) return PR_DECLINED;
        bi->BlitPlanar2Chunky(bi,(struct BitMap *)s->bitmap,&ri,sx,sy,dx,dy,w,h,12,mask);
    } else {
        /* A card's own planar hook is tried once, on its first use (a mode
         * is set by then), against a known answer: the Vampire's drew
         * nothing at 16 bits (issue #1, 2026-10-09). One that gets it
         * wrong is replaced by the CPU version for the session. */
        static UBYTE planarTested;
        if(!bi->BlitPlanar2Direct) return PR_DECLINED;
        if(!planarTested && bi->BlitPlanar2Direct!=rtg_planar_direct) {
            planarTested=1;
            if(!rtg_probe_planar(bi,b->formats))
                bi->BlitPlanar2Direct=rtg_planar_direct;
        }
        cim.ColorMask=d->bpp==2 ? 0xffff : 0xffffffff;
        for(i=0;i<256;i++) cim.Colors[i]=d->bpp==2 && s->pens16 ? s->pens16[i] :
            rtg_colour(d->format,s->colours ? s->colours[i] : i*0x010101UL);
        bi->BlitPlanar2Direct(bi,(struct BitMap *)s->bitmap,&ri,&cim,sx,sy,dx,dy,w,h,12,mask);
    }
    rtg_wait(bi);return PR_DONE;
}
ULONG rtg_pitch(struct PrismBoard *b,UWORD w,UWORD h,UBYTE f)
{
    struct BoardInfo *bi=b->priv;ULONG pitch;
    if(f>=PF_COUNT || !(b->formats & PF_BIT(f)) || !bi->CalculateBytesPerRow) return 0;
    pitch=bi->CalculateBytesPerRow(bi,w,h,NULL,rtg_formats[f]);
    return pitch>32767 || pitch<(ULONG)w*board_bpp(f) ? 0 : pitch;
}
static BOOL rtg_transfer(struct PrismBoard *b,const struct PrismSurface *s,ULONG off,
    UBYTE *mem,ULONG size,BOOL write)
{
    ULONG n;
    struct BoardInfo *bi=b->priv;
    struct RenderInfo ri;
    if(s->format>=PF_COUNT || off>s->allocation || size>s->allocation-off ||
        s->offset>b->vramSize || s->allocation>b->vramSize-s->offset || !s->pitch) return FALSE;
    ri.Memory=b->vram+s->offset;ri.BytesPerRow=s->pitch;ri.pad=0;ri.RGBFormat=rtg_formats[s->format];
    rtg_wait(bi);
    if(bi->SetMemoryMode)bi->SetMemoryMode(bi,ri.RGBFormat);
    while(size) {
        UBYTE *mapped=bi->CalculateMemory ? bi->CalculateMemory(bi,b->vram+s->offset+off,&ri,ri.RGBFormat) :
            b->vram+s->offset+off;
        if(!mapped) return FALSE;
        /* A banked aperture can change on every translation. Consume the
         * mapping before asking for another address; never infer a window
         * size by probing ahead and invalidating the current mapping. */
        n=(b->flags & PBF_BANKED) ? 1 : s->pitch-off%s->pitch;
        if(n>size)n=size;
        if(n==1) { if(write)*mapped=*mem;else *mem=*mapped; }
        else if(write) CopyMem(mem,mapped,n); else CopyMem(mapped,mem,n);
        mem+=n;off+=n;size-=n;
    }
    return TRUE;
}
BOOL rtg_read(struct PrismBoard *b,const struct PrismSurface *s,ULONG off,APTR mem,ULONG size)
{ return rtg_transfer(b,s,off,mem,size,FALSE); }
BOOL rtg_write(struct PrismBoard *b,const struct PrismSurface *s,ULONG off,const void *mem,ULONG size)
{ return rtg_transfer(b,s,off,(UBYTE *)mem,size,TRUE); }
const struct PrismOps rtg_ops={.fill=rtg_fill,.copy=rtg_copy,.expand=rtg_expand,
    .planar=rtg_planar,.pitch=rtg_pitch,.read=rtg_read,.write=rtg_write};
