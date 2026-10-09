/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PRISM_COMPOSITE_H
#define PRISM_COMPOSITE_H
/* Presentation works on a private scanout image, never application pixels. */
static inline ULONG composite_rgb(const struct PBitMap *p, const UBYTE *pixel)
{
    if (p->fmt != PF_CLUT8) return pf_get(p->fmt,pixel);
    return p->rgbTab ? p->rgbTab[*pixel] : *pixel * 0x010101UL;
}
/* A bitmap without a palette is taken as a grey ramp. */
static inline UBYTE composite_pen(const struct PBitMap *p,ULONG rgb)
{
    if (p->rgbTab) return pen_nearest(p->rgbTab,rgb);
    return (((rgb>>16)&255)*3+((rgb>>8)&255)*4+(rgb&255)*2)/9;
}
static inline void composite_put(struct PBitMap *p, UBYTE *pixel, ULONG rgb)
{
    if (p->fmt != PF_CLUT8) pf_put(p->fmt,rgb,pixel);
    else *pixel=composite_pen(p,rgb);
}
/* Both bitmaps are indexed. Build once per frame when palettes differ. */
static inline void composite_clut_map(struct PBitMap *dst,
    const struct PBitMap *src, UBYTE *map)
{
    UWORD i;
    for(i=0;i<256;i++) {
        ULONG rgb=src->rgbTab ? src->rgbTab[i] : i*0x010101UL;
        map[i]=composite_pen(dst,rgb);
    }
}
static inline void composite_row_mapped(struct PBitMap *dst, UWORD dy,
    const struct PBitMap *src, UWORD sy, const UBYTE *map)
{
    UWORD x, width=src->w<dst->w ? src->w : dst->w;
    UBYTE *d=dst->pix+(ULONG)dy*dst->bpr;
    const UBYTE *s=src->pix+(ULONG)sy*src->bpr;
    if (src->fmt==dst->fmt && (src->fmt!=PF_CLUT8 || src->rgbTab==dst->rgbTab))
        CopyMem((APTR)s,d,(ULONG)width*dst->bpp);
    else if(map && src->fmt==PF_CLUT8 && dst->fmt==PF_CLUT8)
        for(x=0;x<width;x++) d[x]=map[s[x]];
    else for (x=0;x<width;x++)
        composite_put(dst,d+(ULONG)x*dst->bpp,composite_rgb(src,s+(ULONG)x*src->bpp));
}
static inline void composite_row(struct PBitMap *dst, UWORD dy,
    const struct PBitMap *src, UWORD sy)
{
    composite_row_mapped(dst,dy,src,sy,NULL);
}
#endif
