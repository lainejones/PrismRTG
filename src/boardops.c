/* SPDX-License-Identifier: GPL-3.0-only */
#include "boardops.h"

UBYTE board_bpp(UBYTE f)
{
    if (f >= PF_COUNT) return 0;
    return f == PF_CLUT8 ? 1 : f == PF_RGB24 || f == PF_BGR24 ? 3 :
        f == PF_ARGB32 || f == PF_BGRA32 || f == PF_RGBA32 ? 4 : 2;
}
BOOL board_rect(const struct PrismSurface *s, UWORD x, UWORD y, UWORD w, UWORD h)
{
    return s && w && h && s->bpp && s->bpp == board_bpp(s->format) &&
        (ULONG)x+w <= s->width && (ULONG)y+h <= s->height &&
        s->pitch >= (ULONG)s->width*s->bpp &&
        s->height && s->pitch <= s->allocation/s->height;
}
static enum PrismResult finish(struct PrismBoard *b, enum PrismResult r)
{
    if (r == PR_RETRY) {
        b->faults++;
        return r;
    }
    if (r != PR_DONE && r != PR_DECLINED) {
        b->flags |= PBF_ACCEL_BROKEN;
        b->faults++;
        return PR_FAILED;
    }
    return r;
}
static BOOL ready(struct PrismBoard *b, const struct PrismSurface *s)
{
    return !(b->flags & (PBF_ACCEL_BROKEN | PBF_SOFTWARE | PBF_PRESENT)) && (s->flags & PSF_VRAM);
}
enum PrismResult board_fill(struct PrismBoard *b, const struct PrismSurface *s,
    UWORD x, UWORD y, UWORD w, UWORD h, ULONG c)
{
    if (!board_rect(s,x,y,w,h) || !ready(b,s)) return PR_DECLINED;
    if (b->ops && b->ops->fill) return finish(b,b->ops->fill(b,s,x,y,w,h,c));
    if (!b->fillRect) return PR_DECLINED;
    b->fillRect(b,s->offset,s->pitch,s->bpp,x,y,w,h,c);
    return PR_DONE;
}
enum PrismResult board_copy(struct PrismBoard *b, const struct PrismSurface *s,
    const struct PrismSurface *d, UWORD sx,UWORD sy,UWORD dx,UWORD dy,UWORD w,UWORD h)
{
    if (!board_rect(s,sx,sy,w,h) || !board_rect(d,dx,dy,w,h) ||
        !ready(b,s) || !ready(b,d) || s->format != d->format) return PR_DECLINED;
    if (b->ops && b->ops->copy) return finish(b,b->ops->copy(b,s,d,sx,sy,dx,dy,w,h));
    if (s->offset == d->offset && s->pitch == d->pitch && b->copyRect)
        b->copyRect(b,s->offset,s->pitch,s->bpp,sx,sy,dx,dy,w,h);
    else if (b->copyBetween && (s->offset+s->allocation <= d->offset ||
                              d->offset+d->allocation <= s->offset))
        b->copyBetween(b,s->offset+(ULONG)sy*s->pitch+(ULONG)sx*s->bpp,s->pitch,
            d->offset+(ULONG)dy*d->pitch+(ULONG)dx*d->bpp,d->pitch,w*d->bpp,h);
    else return PR_DECLINED;
    return PR_DONE;
}
enum PrismResult board_expand(struct PrismBoard *b,const struct PrismSurface *d,
    UWORD x,UWORD y,UWORD w,UWORD h,const UBYTE *src,ULONG mod,ULONG fg,ULONG bg,BOOL tr)
{
    if (!board_rect(d,x,y,w,h) || !ready(b,d) || !src || mod < ((ULONG)w+7)/8)
        return PR_DECLINED;
    if (b->ops && b->ops->expand)
        return finish(b,b->ops->expand(b,d,x,y,w,h,src,mod,fg,bg,tr));
    if (!b->expandRect) return PR_DECLINED;
    return b->expandRect(b,d->offset,d->pitch,d->bpp,x,y,w,h,src,mod,fg,bg,tr) ?
        PR_DONE : PR_DECLINED;
}
enum PrismResult board_line(struct PrismBoard *b,const struct PrismSurface *d,
    WORD x,WORD y,WORD dx,WORD dy,ULONG c)
{
    LONG x1=(LONG)x+dx,y1=(LONG)y+dy;
    if (!board_rect(d,0,0,1,1) || x<0 || y<0 || x1<0 || y1<0 || x>=d->width || y>=d->height ||
        x1>=d->width || y1>=d->height || !ready(b,d)) return PR_DECLINED;
    if (b->ops && b->ops->line) return finish(b,b->ops->line(b,d,x,y,dx,dy,c));
    if (!b->drawLine) return PR_DECLINED;
    b->drawLine(b,d->offset,d->pitch,d->bpp,x,y,dx,dy,c);
    return PR_DONE;
}
enum PrismResult board_planar(struct PrismBoard *b,const struct PrismPlanar *s,
    const struct PrismSurface *d,UWORD sx,UWORD sy,UWORD dx,UWORD dy,UWORD w,UWORD h,
    UBYTE minterm,UBYTE mask)
{
    ULONG row;
    if (!s || !s->bitmap || !s->bitmap->Depth || s->bitmap->Depth>8 ||
        !board_rect(d,dx,dy,w,h) || !ready(b,d) || !b->ops || !b->ops->planar)
        return PR_DECLINED;
    row=s->bitmap->BytesPerRow;
    if (s->bitmap->Flags & BMF_INTERLEAVED) row/=s->bitmap->Depth;
    if ((ULONG)sx+w>row*8 || (ULONG)sy+h>s->bitmap->Rows) return PR_DECLINED;
    return finish(b,b->ops->planar(b,s,d,sx,sy,dx,dy,w,h,minterm,mask));
}
