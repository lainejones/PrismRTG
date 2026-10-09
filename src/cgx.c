/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laine Jones */
/*
 * cgx.c - cybergraphics.library for programs written for CyberGraphX /
 * Picasso96 (IBrowse, AWeb, Amelinium, datatypes...).
 *
 * PrismD builds the library in memory and adds it to exec's list, so
 * OpenLibrary("cybergraphics.library") finds it without a disk file. The
 * functions work on Prism bitmaps: direct access (LockBitMap,
 * GetCyberMapAttr), pixel arrays in RGB/ARGB/LUT8/grey through a
 * RastPort's clipping, and mode queries over Prism's mode table.
 *
 * Every call is counted and the first few of each are logged, so a run of
 * a browser shows what it really uses.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/libraries.h>
#include <exec/execbase.h>
#include <graphics/gfx.h>
#include <graphics/rastport.h>
#include <graphics/clip.h>
#include <graphics/layers.h>
#include <utility/tagitem.h>
#include <utility/hooks.h>
#include <cybergraphx/cybergraphics.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/utility.h>
#include <libraries/asl.h>
#include <proto/asl.h>
#include <string.h>
#include "prism.h"
#include "prismint.h"

extern struct ExecBase *SysBase;
extern struct Library *UtilityBase;

static struct Library *cgxBase;
static char libName[] = "cybergraphics.library";
static char libId[] = "cybergraphics.library 41.99 (3.10.2026) Prism";

/* ---- call log --------------------------------------------------------- */

enum { F_IsCyberModeID, F_BestCModeID, F_CModeRequest, F_AllocCModeList, F_FreeCModeList,
       F_ScalePixelArray, F_GetCyberMapAttr, F_GetCyberIDAttr, F_ReadRGBPixel,
       F_WriteRGBPixel, F_ReadPixelArray, F_WritePixelArray, F_MovePixelArray,
       F_InvertPixelArray, F_FillPixelArray, F_DoCDrawMethod, F_CVideoCtrl,
       F_LockBitMap, F_UnLockBitMap, F_UnLockBitMapTags, F_ExtractColor,
       F_WriteLUTPixelArray, F_Private, F_COUNT };
static UWORD calls[F_COUNT];

#ifdef PRISM_TRACE
#define LOG(f, ...) do { memchk("cgx " #f); dbg("cgx: " __VA_ARGS__); } while (0)
#else
#define LOG(f, ...) do { if (calls[f]++ < 40) dbg("cgx: " __VA_ARGS__); } while (0)
#endif

/* ---- formats -------------------------------------------------------- */

#define pixfmt(f) pf_to_pixfmt(f)

static UBYTE bits_of(UBYTE fmt)
{
    switch (pf_bpp(fmt)) {
    case 1: return 8;
    case 3: return 24;
    case 4: return 32;
    }
    return (fmt == PF_RGB555LE || fmt == PF_RGB555BE || fmt == PF_BGR555LE) ? 15 : 16;
}

static BOOL rp_ours(struct RastPort *rp)
{
    return rp && rp->BitMap && pbm_get(rp->BitMap);
}

/* Nearest pen to an RGB colour on an 8-bit bitmap, with a 12-bit cache
 * (rebuilt per call: palettes change). Caller holds the lock. */
static UWORD penCache[4096];
static ULONG *cacheTab;

static UBYTE nearest(struct PBitMap *p, ULONG rgb)
{
    UWORD key = ((rgb >> 12) & 0xf00) | ((rgb >> 8) & 0xf0) | ((rgb >> 4) & 0xf), i, best = 0;
    LONG bd = 0x7fffffff;
    if (!p->rgbTab)
        return 0;
    if (cacheTab != p->rgbTab) {
        memset(penCache, 0xff, sizeof(penCache));
        cacheTab = p->rgbTab;
    }
    if (penCache[key] != 0xffff)
        return penCache[key];
    for (i = 0; i < (1 << p->depth) && i < 256; i++) {
        ULONG c = p->rgbTab[i];
        LONG dr = (LONG)((c >> 16) & 255) - ((rgb >> 16) & 255);
        LONG dg = (LONG)((c >> 8) & 255) - ((rgb >> 8) & 255);
        LONG db = (LONG)(c & 255) - (rgb & 255);
        LONG d = dr * dr * 3 + dg * dg * 4 + db * db * 2;
        if (d < bd) { bd = d; best = i; }
    }
    penCache[key] = best;
    return best;
}

/* One source pixel to 0x00RRGGBB. */
static inline ULONG src_rgb(const UBYTE *s, UBYTE rf, const ULONG *ctab, struct PBitMap *p)
{
    switch (rf) {
    case RECTFMT_RGB:   return ((ULONG)s[0] << 16) | ((ULONG)s[1] << 8) | s[2];
    case RECTFMT_RGBA:  return ((ULONG)s[0] << 16) | ((ULONG)s[1] << 8) | s[2];
    case RECTFMT_ARGB:  return ((ULONG)s[1] << 16) | ((ULONG)s[2] << 8) | s[3];
    case RECTFMT_GREY8: return ((ULONG)s[0] << 16) | ((ULONG)s[0] << 8) | s[0];
    }
    /* LUT8: through the caller's colour table or the bitmap's palette */
    if (ctab) return ctab[s[0]] & 0xffffff;
    return p->rgbTab ? p->rgbTab[s[0]] : 0;
}

#ifndef RECTFMT_RAW
#define RECTFMT_RAW 5                   /* the destination's own pixel layout */
#endif

static UBYTE src_bpp(UBYTE rf, const struct PBitMap *p)
{
    if (rf == RECTFMT_RAW) return p->bpp;
    return (rf == RECTFMT_RGB) ? 3 : (rf == RECTFMT_RGBA || rf == RECTFMT_ARGB) ? 4 : 1;
}

/* ---- pixel arrays through a RastPort -------------------------------- */

struct ArrayCtx {
    const UBYTE *src;
    LONG  srcMod;
    UBYTE rf;                    /* RECTFMT_*                              */
    const ULONG *ctab;           /* WriteLUTPixelArray colour table        */
    WORD  sx, sy;                /* source pixel of rp (dx, dy)            */
    WORD  dx, dy;
    LONG  count;
    /* reading */
    UBYTE *dst;
};

#define rowbuf PRISM_ROWBUF        /* 4096 * 4 bytes, see prismint.h */

/* One row of source pixels into a row of the bitmap's format. The source
 * is RGB bytes (red at s[ro], green at s[go], blue at s[bo], sb bytes a
 * pixel) or, with a table, one byte a pixel looked up as 0x00RRGGBB. One
 * loop per destination format, so that the format is a constant inside it:
 * with the format looked at for every pixel, CgxBenchmark's 320x240 ARGB
 * frames went to a 16-bit ZZ9000 screen at 9 a second on a 68060. */
static void row_to_16(UBYTE fmt, const UBYTE *s, UBYTE sb, UBYTE ro, UBYTE go, UBYTE bo,
                      const ULONG *tab, UWORD *w, WORD n)
{
    WORD x;
#define L16(F) case F: \
        if (tab) for (x = 0; x < n; x++) { ULONG c = tab[s[x]]; w[x] = rgb16(F, c >> 16, c >> 8, c); } \
        else for (x = 0; x < n; x++, s += sb) w[x] = rgb16(F, s[ro], s[go], s[bo]); \
        break;
    switch (fmt) {
    L16(PF_RGB565BE) L16(PF_RGB565LE) L16(PF_BGR565LE)
    L16(PF_RGB555BE) L16(PF_RGB555LE) L16(PF_BGR555LE)
    }
#undef L16
}

static void row_to_deep(UBYTE fmt, const UBYTE *s, UBYTE sb, UBYTE ro, UBYTE go, UBYTE bo,
                        const ULONG *tab, UBYTE *d, WORD n)
{
    WORD x;
#define LD(F, B) case F: \
        if (tab) for (x = 0; x < n; x++, d += B) pf_put(F, tab[s[x]], d); \
        else for (x = 0; x < n; x++, s += sb, d += B) \
            pf_put(F, ((ULONG)s[ro] << 16) | ((ULONG)s[go] << 8) | s[bo], d); \
        break;
    switch (fmt) {
    LD(PF_RGB24, 3) LD(PF_BGR24, 3) LD(PF_ARGB32, 4) LD(PF_BGRA32, 4) LD(PF_RGBA32, 4)
    }
#undef LD
}

static void write_cb(struct PBitMap *p, WORD bx0, WORD by0, WORD bx1, WORD by1,
                     WORD ox, WORD oy, void *ctx)
{
    struct ArrayCtx *a = ctx;
    UBYTE sb = src_bpp(a->rf, p), ro = 0, go = 0, bo = 0;
    const ULONG *tab = NULL;
    WORD y, x, n = bx1 - bx0 + 1;

    if (n > 4096) n = 4096;
    switch (a->rf) {
    case RECTFMT_RGB: case RECTFMT_RGBA: go = 1; bo = 2; break;
    case RECTFMT_ARGB: ro = 1; go = 2; bo = 3; break;
    case RECTFMT_GREY8: case RECTFMT_RAW: break;
    default: tab = a->ctab ? a->ctab : p->rgbTab; break;       /* LUT8 */
    }
    for (y = by0; y <= by1; y++) {
        const UBYTE *s = a->src + (LONG)(a->sy + (y - oy - a->dy)) * a->srcMod +
                         (LONG)(a->sx + (bx0 - ox - a->dx)) * sb;
        UBYTE *row = p->pix + (ULONG)y * p->bpr + bx0 * p->bpp;
        if (a->rf == RECTFMT_RAW) {
            CopyMem((APTR)s, row, (ULONG)n * p->bpp);     /* already our layout */
        } else if (p->bpp == 2) {
            UWORD *w = (UWORD *)rowbuf;
            if (a->rf == RECTFMT_LUT8 && !a->ctab && p->penTab)
                for (x = 0; x < n; x++) w[x] = p->penTab[s[x]];
            else if (a->rf == RECTFMT_LUT8 && !tab)
                memset(w, 0, n * 2);
            else
                row_to_16(p->fmt, s, sb, ro, go, bo, tab, w, n);
            CopyMem(rowbuf, row, n * 2);
        } else if (p->bpp >= 3) {
            if (a->rf == RECTFMT_LUT8 && !tab)
                memset(rowbuf, 0, (ULONG)n * p->bpp);
            else
                row_to_deep(p->fmt, s, sb, ro, go, bo, tab, rowbuf, n);
            CopyMem(rowbuf, row, (ULONG)n * p->bpp);
        } else {
            if (a->rf == RECTFMT_LUT8 && !a->ctab)
                CopyMem((APTR)s, rowbuf, n);
            else
                for (x = 0; x < n; x++, s += sb)
                    rowbuf[x] = nearest(p, src_rgb(s, a->rf, a->ctab, p));
            CopyMem(rowbuf, row, n);
        }
        a->count += n;
    }
}

static void read_cb(struct PBitMap *p, WORD bx0, WORD by0, WORD bx1, WORD by1,
                    WORD ox, WORD oy, void *ctx)
{
    struct ArrayCtx *a = ctx;
    UBYTE db = src_bpp(a->rf, p);
    WORD y, x, n = bx1 - bx0 + 1;

    for (y = by0; y <= by1; y++) {
        UBYTE *d = a->dst + (LONG)(a->sy + (y - oy - a->dy)) * a->srcMod +
                   (LONG)(a->sx + (bx0 - ox - a->dx)) * db;
        const UBYTE *row = p->pix + (ULONG)y * p->bpr + bx0 * p->bpp;
        if (a->rf == RECTFMT_RAW) {
            CopyMem((APTR)row, d, (ULONG)n * p->bpp);
            a->count += n;
            continue;
        }
        for (x = 0; x < n; x++, d += db) {
            ULONG c;
            UBYTE pen = 0;
            if (p->bpp >= 2) {
                c = pf_get(p->fmt, row + x * p->bpp);
            } else {
                pen = row[x];
                c = p->rgbTab ? p->rgbTab[pen] : 0;
            }
            switch (a->rf) {
            case RECTFMT_RGB:  d[0] = c >> 16; d[1] = c >> 8; d[2] = c; break;
            case RECTFMT_RGBA: d[0] = c >> 16; d[1] = c >> 8; d[2] = c; d[3] = 0; break;
            case RECTFMT_ARGB: d[0] = 0; d[1] = c >> 16; d[2] = c >> 8; d[3] = c; break;
            case RECTFMT_LUT8: d[0] = pen; break;
            default:           d[0] = (((c >> 16) & 255) * 77 + ((c >> 8) & 255) * 151 +
                                       (c & 255) * 28) >> 8;
            }
        }
        a->count += n;
    }
}

static LONG write_array(const UBYTE *src, WORD sx, WORD sy, LONG mod, struct RastPort *rp,
                        WORD dx, WORD dy, WORD w, WORD h, UBYTE rf, const ULONG *ctab)
{
    struct ArrayCtx a;
    if (!rp_ours(rp) || w <= 0 || h <= 0 || !src)
        return 0;
    a.src = src; a.srcMod = mod; a.rf = rf; a.ctab = ctab;
    a.sx = sx; a.sy = sy; a.dx = dx; a.dy = dy; a.count = 0;
    clip_rp(rp, dx, dy, dx + w - 1, dy + h - 1, write_cb, &a);
    return a.count;
}

/* ---- solid fill / invert -------------------------------------------- */

struct FillCtx { ULONG rgb; BOOL invert; };

static void fill_cb(struct PBitMap *p, WORD bx0, WORD by0, WORD bx1, WORD by1,
                    WORD ox, WORD oy, void *ctx)
{
    struct FillCtx *f = ctx;
    WORD y, x, n = bx1 - bx0 + 1;
    UWORD v16 = rgb16(p->fmt, f->rgb >> 16, f->rgb >> 8, f->rgb);
    UBYTE v8 = f->invert ? 0 : nearest(p, f->rgb);

    for (y = by0; y <= by1; y++) {
        UBYTE *row = p->pix + (ULONG)y * p->bpr + bx0 * p->bpp;
        if (p->bpp == 2) {
            UWORD *w = (UWORD *)row;
            if (f->invert) for (x = 0; x < n; x++) w[x] ^= 0xffff;
            else           for (x = 0; x < n; x++) w[x] = v16;
        } else if (p->bpp >= 3) {
            UBYTE *dp = row;
            for (x = 0; x < n; x++, dp += p->bpp)
                pf_put(p->fmt, f->invert ? pf_get(p->fmt, dp) ^ 0xffffff : f->rgb, dp);
        } else {
            if (f->invert) for (x = 0; x < n; x++) row[x] ^= 0xff;
            else           memset(row, v8, n);
        }
    }
}

/* ---- library functions ---------------------------------------------- */

static struct Library *L_Open(struct Library *base __asm("a6"))
{
    base->lib_OpenCnt++;
    base->lib_Flags &= ~LIBF_DELEXP;
    return base;
}

static BPTR L_Close(struct Library *base __asm("a6"))
{
    if (base->lib_OpenCnt)
        base->lib_OpenCnt--;
    return 0;
}

static BPTR L_Expunge(struct Library *base __asm("a6"))
{
    return 0;                   /* PrismD removes it when it quits */
}

static ULONG L_Null(void)
{
    calls[F_Private]++;
    return 0;
}

static BOOL C_IsCyberModeID(ULONG id __asm("d0"))
{
    struct PrismModeInfo mi;
    BOOL r = prism_mode_by_id(id, &mi);
    LOG(F_IsCyberModeID, "IsCyberModeID %lx -> %d\n", id, r);
    return r;
}

static ULONG C_BestCModeIDTagList(struct TagItem *tags __asm("a0"))
{
    ULONG depth = GetTagData(CYBRBIDTG_Depth, 8, tags);
    ULONG nw = GetTagData(CYBRBIDTG_NominalWidth, 640, tags);
    ULONG nh = GetTagData(CYBRBIDTG_NominalHeight, 480, tags);
    /* class: 1 = 8-bit, 2 = 15/16-bit, 3 = 24/32-bit */
    UBYTE want = (depth > 16) ? 3 : (depth > 8) ? 2 : 1, cls;
    ULONG i, best = INVALID_ID, bestArea, bigId = INVALID_ID, bigArea;
    struct PrismModeInfo mi;

    /* the smallest mode of that class at least the nominal size, else its
     * biggest; no true-colour mode on (too big for the card, or switched
     * off in PrismPrefs) falls back to 16-bit */
    for (cls = want; cls >= (want > 1 ? 2 : 1) && best == INVALID_ID; cls--) {
        bestArea = 0xffffffff;
        bigArea = 0;
        for (i = 0; prism_mode(i, &mi); i++) {
            ULONG area = (ULONG)mi.w * mi.h;
            if ((mi.bpp > 3 ? 3 : mi.bpp) != cls)
                continue;
            if (mi.w >= nw && mi.h >= nh && area < bestArea) { best = mi.id; bestArea = area; }
            if (area > bigArea) { bigId = mi.id; bigArea = area; }
        }
        if (best == INVALID_ID)
            best = bigId;
    }
    LOG(F_BestCModeID, "BestCModeID depth %lu %lux%lu -> %lx\n", depth, nw, nh, best);
    return best;
}

/* The mode requester: asl.library's screen mode requester, showing only the
 * Prism modes that fit the caller's limits. Runs on the caller's task. */
struct ModeFilter {
    ULONG minD, maxD, minW, maxW, minH, maxH;
    UWORD *models;
};

static ULONG mode_filter(struct Hook *h __asm("a0"), APTR rq __asm("a2"), ULONG id __asm("a1"))
{
    struct ModeFilter *f = h->h_Data;
    struct PrismModeInfo mi;
    UBYTE bits;

    if (!prism_mode_by_id(id, &mi))
        return FALSE;
    bits = bits_of(mi.fmt);
    if (bits < f->minD || bits > f->maxD || mi.w < f->minW || mi.w > f->maxW ||
        mi.h < f->minH || mi.h > f->maxH)
        return FALSE;
    if (f->models) {
        UWORD *m;
        for (m = f->models; *m != (UWORD)~0 && *m != pixfmt(mi.fmt); m++) ;
        if (*m == (UWORD)~0)
            return FALSE;
    }
    return TRUE;
}

static ULONG C_CModeRequestTagList(APTR req __asm("a0"), struct TagItem *tags __asm("a1"))
{
    struct Library *AslBase;
    struct ScreenModeRequester *sm;
    struct ModeFilter f;
    struct Hook hook;
    ULONG id = INVALID_ID;
    STRPTR title = (STRPTR)GetTagData(CYBRMREQ_WinTitle, (ULONG)"Select a screen mode", tags);
    STRPTR ok = (STRPTR)GetTagData(CYBRMREQ_OKText, (ULONG)"OK", tags);
    STRPTR cancel = (STRPTR)GetTagData(CYBRMREQ_CancelText, (ULONG)"Cancel", tags);
    struct Screen *scr = (struct Screen *)GetTagData(CYBRMREQ_Screen, 0, tags);

    f.minD = GetTagData(CYBRMREQ_MinDepth, 0, tags);
    f.maxD = GetTagData(CYBRMREQ_MaxDepth, 32, tags);
    f.minW = GetTagData(CYBRMREQ_MinWidth, 0, tags);
    f.maxW = GetTagData(CYBRMREQ_MaxWidth, 0xffff, tags);
    f.minH = GetTagData(CYBRMREQ_MinHeight, 0, tags);
    f.maxH = GetTagData(CYBRMREQ_MaxHeight, 0xffff, tags);
    f.models = (UWORD *)GetTagData(CYBRMREQ_CModelArray, 0, tags);
    memset(&hook, 0, sizeof(hook));
    hook.h_Entry = (ULONG (*)())mode_filter;
    hook.h_Data = &f;

    if ((AslBase = OpenLibrary("asl.library", 38))) {
        struct TagItem at[] = {
            { ASLSM_TitleText, (ULONG)title },
            { ASLSM_PositiveText, (ULONG)ok },
            { ASLSM_NegativeText, (ULONG)cancel },
            { ASLSM_FilterFunc, (ULONG)&hook },
            { scr ? ASLSM_Screen : TAG_IGNORE, (ULONG)scr },
            { TAG_DONE, 0 }
        };
        if ((sm = AllocAslRequest(ASL_ScreenModeRequest, at))) {
            if (AslRequest(sm, NULL))
                id = sm->sm_DisplayID;
            FreeAslRequest(sm);
        }
        CloseLibrary(AslBase);
    }
    LOG(F_CModeRequest, "CModeRequest depth %lu-%lu -> %lx\n", f.minD, f.maxD, id);
    return id;
}

static struct List *C_AllocCModeListTagList(struct TagItem *tags __asm("a1"))
{
    ULONG minD = GetTagData(CYBRMREQ_MinDepth, 0, tags), maxD = GetTagData(CYBRMREQ_MaxDepth, 32, tags);
    ULONG minW = GetTagData(CYBRMREQ_MinWidth, 0, tags), maxW = GetTagData(CYBRMREQ_MaxWidth, 0xffff, tags);
    ULONG minH = GetTagData(CYBRMREQ_MinHeight, 0, tags), maxH = GetTagData(CYBRMREQ_MaxHeight, 0xffff, tags);
    UWORD *models = (UWORD *)GetTagData(CYBRMREQ_CModelArray, 0, tags);
    struct List *l;
    struct PrismModeInfo mi;
    ULONG i;

    if (!(l = AllocVec(sizeof(*l), MEMF_PUBLIC | MEMF_CLEAR)))
        return NULL;
    l->lh_Head = (struct Node *)&l->lh_Tail;
    l->lh_TailPred = (struct Node *)&l->lh_Head;
    for (i = 0; prism_mode(i, &mi); i++) {
        struct CyberModeNode *n;
        UBYTE bits = bits_of(mi.fmt);
        if (bits < minD || bits > maxD || mi.w < minW || mi.w > maxW || mi.h < minH || mi.h > maxH)
            continue;
        if (models) {
            UWORD *m;
            for (m = models; *m != (UWORD)~0 && *m != pixfmt(mi.fmt); m++) ;
            if (*m == (UWORD)~0)
                continue;
        }
        if (!(n = AllocVec(sizeof(*n), MEMF_PUBLIC | MEMF_CLEAR)))
            break;
        strncpy(n->ModeText, mi.name, DISPLAYNAMELEN - 1);
        n->Node.ln_Name = n->ModeText;
        n->DisplayID = mi.id;
        n->Width = mi.w;
        n->Height = mi.h;
        n->Depth = bits;
        AddTail(l, &n->Node);
    }
    LOG(F_AllocCModeList, "AllocCModeList\n");
    return l;
}

static void C_FreeCModeList(struct List *l __asm("a0"))
{
    struct Node *n;
    if (!l)
        return;
    while ((n = RemHead(l)))
        FreeVec(n);
    FreeVec(l);
}

static ULONG C_GetCyberMapAttr(struct BitMap *bm __asm("a0"), ULONG attr __asm("d0"))
{
    struct PBitMap *p = pbm_get(bm);
    ULONG r = 0;
    if (p) {
        switch (attr) {
        case CYBRMATTR_XMOD:        r = p->bpr; break;
        case CYBRMATTR_BPPIX:       r = p->bpp; break;
        case CYBRMATTR_DISPADR:     r = (ULONG)p->pix; break;
        case CYBRMATTR_PIXFMT:      r = pixfmt(p->fmt); break;
        case CYBRMATTR_WIDTH:       r = p->w; break;
        case CYBRMATTR_HEIGHT:      r = p->h; break;
        case CYBRMATTR_DEPTH:       r = bits_of(p->fmt); break;
        case CYBRMATTR_ISCYBERGFX:  r = (ULONG)-1; break;
        case CYBRMATTR_ISLINEARMEM: r = (ULONG)-1; break;
        }
    }
    LOG(F_GetCyberMapAttr, "GetCyberMapAttr %lx %lx -> %lx\n", (ULONG)bm, attr, r);
    return r;
}

static ULONG C_GetCyberIDAttr(ULONG attr __asm("d0"), ULONG id __asm("d1"))
{
    struct PrismModeInfo mi;
    ULONG r = 0;
    if (prism_mode_by_id(id, &mi)) {
        switch (attr) {
        case CYBRIDATTR_PIXFMT: r = pixfmt(mi.fmt); break;
        case CYBRIDATTR_WIDTH:  r = mi.w; break;
        case CYBRIDATTR_HEIGHT: r = mi.h; break;
        case CYBRIDATTR_DEPTH:  r = bits_of(mi.fmt); break;
        case CYBRIDATTR_BPPIX:  r = mi.bpp; break;
        }
    }
    LOG(F_GetCyberIDAttr, "GetCyberIDAttr %lx %lx -> %lu\n", attr, id, r);
    return r;
}

static ULONG C_ReadRGBPixel(struct RastPort *rp __asm("a1"), UWORD x __asm("d0"), UWORD y __asm("d1"))
{
    struct ArrayCtx a;
    UBYTE px[4] = { 0, 0, 0, 0 };
    LOG(F_ReadRGBPixel, "ReadRGBPixel %d,%d\n", x, y);
    if (!rp_ours(rp))
        return 0;
    a.dst = px; a.srcMod = 4; a.rf = RECTFMT_ARGB; a.sx = 0; a.sy = 0; a.dx = x; a.dy = y;
    a.count = 0;
    clip_rp(rp, x, y, x, y, read_cb, &a);
    return ((ULONG)px[1] << 16) | ((ULONG)px[2] << 8) | px[3];
}

static LONG C_WriteRGBPixel(struct RastPort *rp __asm("a1"), UWORD x __asm("d0"),
                            UWORD y __asm("d1"), ULONG argb __asm("d2"))
{
    UBYTE px[4];
    LOG(F_WriteRGBPixel, "WriteRGBPixel %d,%d %lx\n", x, y, argb);
    px[0] = argb >> 24; px[1] = argb >> 16; px[2] = argb >> 8; px[3] = argb;
    return write_array(px, 0, 0, 4, rp, x, y, 1, 1, RECTFMT_ARGB, NULL) ? 0 : -1;
}

static ULONG C_ReadPixelArray(UBYTE *dst __asm("a0"), UWORD dx __asm("d0"), UWORD dy __asm("d1"),
                              UWORD dmod __asm("d2"), struct RastPort *rp __asm("a1"),
                              UWORD sx __asm("d3"), UWORD sy __asm("d4"), UWORD w __asm("d5"),
                              UWORD h __asm("d6"), UBYTE fmt __asm("d7"))
{
    struct ArrayCtx a;
    LOG(F_ReadPixelArray, "ReadPixelArray %dx%d fmt %u\n", w, h, fmt);
    if (!rp_ours(rp) || !dst)
        return 0;
    a.dst = dst; a.srcMod = dmod; a.rf = fmt; a.sx = dx; a.sy = dy; a.dx = sx; a.dy = sy;
    a.count = 0;
    clip_rp(rp, sx, sy, sx + w - 1, sy + h - 1, read_cb, &a);
    return a.count;
}

static ULONG C_WritePixelArray(UBYTE *src __asm("a0"), UWORD sx __asm("d0"), UWORD sy __asm("d1"),
                               UWORD smod __asm("d2"), struct RastPort *rp __asm("a1"),
                               UWORD dx __asm("d3"), UWORD dy __asm("d4"), UWORD w __asm("d5"),
                               UWORD h __asm("d6"), UBYTE fmt __asm("d7"))
{
    LOG(F_WritePixelArray, "WritePixelArray %dx%d at %d,%d fmt %u mod %u\n", w, h, dx, dy, fmt, smod);
    return write_array(src, sx, sy, smod, rp, dx, dy, w, h, fmt, NULL);
}

static ULONG C_WriteLUTPixelArray(UBYTE *src __asm("a0"), UWORD sx __asm("d0"), UWORD sy __asm("d1"),
                                  UWORD smod __asm("d2"), struct RastPort *rp __asm("a1"),
                                  ULONG *ctab __asm("a2"), UWORD dx __asm("d3"),
                                  UWORD dy __asm("d4"), UWORD w __asm("d5"), UWORD h __asm("d6"),
                                  UBYTE ctf __asm("d7"))
{
    LOG(F_WriteLUTPixelArray, "WriteLUTPixelArray %dx%d\n", w, h);
    return write_array(src, sx, sy, smod, rp, dx, dy, w, h, RECTFMT_LUT8, ctab);
}

static LONG C_ScalePixelArray(UBYTE *src __asm("a0"), UWORD sw __asm("d0"), UWORD sh __asm("d1"),
                              UWORD smod __asm("d2"), struct RastPort *rp __asm("a1"),
                              UWORD dx __asm("d3"), UWORD dy __asm("d4"), UWORD dw __asm("d5"),
                              UWORD dh __asm("d6"), UBYTE fmt __asm("d7"))
{
    UBYTE sb, *line;
    UWORD x, y, *col;
    LONG n = 0, last = -1;

    LOG(F_ScalePixelArray, "ScalePixelArray %dx%d -> %dx%d fmt %u\n", sw, sh, dw, dh, fmt);
    if (!rp_ours(rp) || !src || !sw || !sh || !dw || !dh)
        return 0;
    sb = src_bpp(fmt, pbm_get(rp->BitMap));
    /* one scaled line at a time, drawn with WritePixelArray's code. The
     * source column of each destination column is worked out once, and a
     * source line that fills several destination lines is scaled once
     * (CgxBenchmark's 320x240 to full screen test: it was a library call
     * per pixel before, and minutes per frame on a 68030). */
    if (!(line = AllocVec((ULONG)dw * sb + (ULONG)dw * sizeof(UWORD), MEMF_ANY)))
        return 0;
    col = (UWORD *)(line + (((ULONG)dw * sb + 1) & ~1UL));
    for (x = 0; x < dw; x++)
        col[x] = (ULONG)x * sw / dw;
    for (y = 0; y < dh; y++) {
        LONG srow = (ULONG)y * sh / dh;
        if (srow != last) {
            const UBYTE *s = src + (ULONG)srow * smod;
            last = srow;
            if (sb == 1) {
                for (x = 0; x < dw; x++) line[x] = s[col[x]];
            } else if (sb == 2) {
                for (x = 0; x < dw; x++) ((UWORD *)line)[x] = ((const UWORD *)s)[col[x]];
            } else if (sb == 4) {
                for (x = 0; x < dw; x++) ((ULONG *)line)[x] = ((const ULONG *)s)[col[x]];
            } else {
                UBYTE *d = line;
                for (x = 0; x < dw; x++, d += 3) {
                    const UBYTE *q = s + (ULONG)col[x] * 3;
                    d[0] = q[0]; d[1] = q[1]; d[2] = q[2];
                }
            }
        }
        n += write_array(line, 0, 0, dw * sb, rp, dx, dy + y, dw, 1, fmt, NULL);
    }
    FreeVec(line);
    return n;
}

static ULONG C_MovePixelArray(UWORD sx __asm("d0"), UWORD sy __asm("d1"), struct RastPort *rp __asm("a1"),
                              UWORD dx __asm("d2"), UWORD dy __asm("d3"), UWORD w __asm("d4"),
                              UWORD h __asm("d5"))
{
    LOG(F_MovePixelArray, "MovePixelArray %dx%d\n", w, h);
    ClipBlit(rp, sx, sy, rp, dx, dy, w, h, 0xc0);
    return (ULONG)w * h;
}

static ULONG C_InvertPixelArray(struct RastPort *rp __asm("a1"), UWORD x __asm("d0"),
                                UWORD y __asm("d1"), UWORD w __asm("d2"), UWORD h __asm("d3"))
{
    struct FillCtx f;
    LOG(F_InvertPixelArray, "InvertPixelArray %dx%d\n", w, h);
    if (!rp_ours(rp) || !w || !h)
        return 0;
    f.rgb = 0; f.invert = TRUE;
    clip_rp(rp, x, y, x + w - 1, y + h - 1, fill_cb, &f);
    return (ULONG)w * h;
}

static ULONG C_FillPixelArray(struct RastPort *rp __asm("a1"), UWORD x __asm("d0"),
                              UWORD y __asm("d1"), UWORD w __asm("d2"), UWORD h __asm("d3"),
                              ULONG argb __asm("d4"))
{
    struct FillCtx f;
    LOG(F_FillPixelArray, "FillPixelArray %dx%d %lx\n", w, h, argb);
    if (!rp_ours(rp) || !w || !h)
        return 0;
    f.rgb = argb & 0xffffff; f.invert = FALSE;
    clip_rp(rp, x, y, x + w - 1, y + h - 1, fill_cb, &f);
    return (ULONG)w * h;
}

/* DoCDrawMethod: the hook gets every visible piece of the RastPort. */
struct DrawCtx { struct Hook *hook; struct RastPort *rp; };

static void draw_cb(struct PBitMap *p, WORD bx0, WORD by0, WORD bx1, WORD by1,
                    WORD ox, WORD oy, void *ctx)
{
    struct DrawCtx *d = ctx;
    struct CDrawMsg m;
    m.cdm_MemPtr = p->pix + (ULONG)by0 * p->bpr + bx0 * p->bpp;
    m.cdm_offx = bx0 - ox;
    m.cdm_offy = by0 - oy;
    m.cdm_xsize = bx1 - bx0 + 1;
    m.cdm_ysize = by1 - by0 + 1;
    m.cdm_BytesPerRow = p->bpr;
    m.cdm_BytesPerPix = p->bpp;
    m.cdm_ColorModel = pixfmt(p->fmt);
    CallHookPkt(d->hook, d->rp, &m);
}

static void C_DoCDrawMethodTagList(struct Hook *hook __asm("a0"), struct RastPort *rp __asm("a1"),
                                   struct TagItem *tags __asm("a2"))
{
    struct DrawCtx d;
    WORD w, h;
    LOG(F_DoCDrawMethod, "DoCDrawMethod\n");
    if (!hook || !rp_ours(rp))
        return;
    if (rp->Layer) {
        w = rp->Layer->bounds.MaxX - rp->Layer->bounds.MinX + 1;
        h = rp->Layer->bounds.MaxY - rp->Layer->bounds.MinY + 1;
    } else {
        struct PBitMap *p = pbm_get(rp->BitMap);
        w = p->w;
        h = p->h;
    }
    d.hook = hook;
    d.rp = rp;
    clip_rp(rp, 0, 0, w - 1, h - 1, draw_cb, &d);
}

static void C_CVideoCtrlTagList(struct ViewPort *vp __asm("a0"), struct TagItem *tags __asm("a1"))
{
    LOG(F_CVideoCtrl, "CVideoCtrl\n");
}

static APTR C_LockBitMapTagList(APTR bm __asm("a0"), struct TagItem *tags __asm("a1"))
{
    struct PBitMap *p = pbm_get(bm);
    struct TagItem *ti, *tl = tags;

    LOG(F_LockBitMap, "LockBitMap %lx -> %s\n", (ULONG)bm, p ? "ours" : "not ours");
    if (!p)
        return NULL;
    /* the caller gets the pixels' address: they must not move (VRAM
     * paging) until UnLockBitMap */
    LOCK_FOR(p);
    p->locks++;
    ReleaseSemaphore(&lock);
    while ((ti = NextTagItem(&tl))) {
        ULONG *v = (ULONG *)ti->ti_Data;
        if (!v)
            continue;
        switch (ti->ti_Tag) {
        case LBMI_WIDTH:       *v = p->w; break;
        case LBMI_HEIGHT:      *v = p->h; break;
        case LBMI_DEPTH:       *v = bits_of(p->fmt); break;
        case LBMI_PIXFMT:      *v = pixfmt(p->fmt); break;
        case LBMI_BYTESPERPIX: *v = p->bpp; break;
        case LBMI_BYTESPERROW: *v = p->bpr; break;
        case LBMI_BASEADDRESS: *v = (ULONG)p->pix; break;
        }
    }
    /* the caller writes the pixels itself: no blit may be running */
    if (p->inVram && board.waitBlit) {
        LOCK_FOR(p);
        board.waitBlit(&board);
        ReleaseSemaphore(&lock);
    }
    return p;
}

static void unlock_pbm(APTR handle)
{
    struct PBitMap *p = handle;
    ObtainSemaphore(&lock);              /* unlocking touches no pixels */
    if (p && pbm_live(p) && p->locks)    /* (a freed handle is never read) */
        p->locks--;
    ReleaseSemaphore(&lock);
}

static void C_UnLockBitMap(APTR handle __asm("a0"))
{
    LOG(F_UnLockBitMap, "UnLockBitMap\n");
    unlock_pbm(handle);
}

static void C_UnLockBitMapTagList(APTR handle __asm("a0"), struct TagItem *tags __asm("a1"))
{
    LOG(F_UnLockBitMapTags, "UnLockBitMapTagList\n");
    unlock_pbm(handle);
}

/* ExtractColor: a 1-bit mask of the pixels that have a colour. */
struct ExtractCtx { struct BitMap *mask; ULONG colour; WORD x0, y0; };

static void extract_cb(struct PBitMap *p, WORD bx0, WORD by0, WORD bx1, WORD by1,
                       WORD ox, WORD oy, void *ctx)
{
    struct ExtractCtx *e = ctx;
    WORD x, y;
    for (y = by0; y <= by1; y++) {
        const UBYTE *row = p->pix + (ULONG)y * p->bpr;
        UBYTE *mrow = e->mask->Planes[0] + (ULONG)(y - oy - e->y0) * e->mask->BytesPerRow;
        for (x = bx0; x <= bx1; x++) {
            ULONG c = (p->bpp >= 2) ? pf_get(p->fmt, row + x * p->bpp) : row[x];
            WORD mx = x - ox - e->x0;
            if (c == e->colour)
                mrow[mx >> 3] |= 0x80 >> (mx & 7);
        }
    }
}

static ULONG C_ExtractColor(struct RastPort *rp __asm("a0"), struct BitMap *bm __asm("a1"),
                            ULONG colour __asm("d0"), ULONG sx __asm("d1"), ULONG sy __asm("d2"),
                            ULONG w __asm("d3"), ULONG h __asm("d4"))
{
    struct ExtractCtx e;
    LOG(F_ExtractColor, "ExtractColor %lux%lu\n", w, h);
    if (!rp_ours(rp) || !bm || !bm->Planes[0])
        return FALSE;
    e.mask = bm; e.colour = colour & 0xffffff; e.x0 = sx; e.y0 = sy;
    clip_rp(rp, sx, sy, sx + w - 1, sy + h - 1, extract_cb, &e);
    return TRUE;
}

static APTR funcs[] = {
    (APTR)L_Open, (APTR)L_Close, (APTR)L_Expunge, (APTR)L_Null,
    (APTR)L_Null, (APTR)L_Null, (APTR)L_Null, (APTR)L_Null,           /* private 1-4 */
    (APTR)C_IsCyberModeID,
    (APTR)C_BestCModeIDTagList,
    (APTR)C_CModeRequestTagList,
    (APTR)C_AllocCModeListTagList,
    (APTR)C_FreeCModeList,
    (APTR)L_Null,                                                     /* private 5 */
    (APTR)C_ScalePixelArray,
    (APTR)C_GetCyberMapAttr,
    (APTR)C_GetCyberIDAttr,
    (APTR)C_ReadRGBPixel,
    (APTR)C_WriteRGBPixel,
    (APTR)C_ReadPixelArray,
    (APTR)C_WritePixelArray,
    (APTR)C_MovePixelArray,
    (APTR)L_Null,                                                     /* private 6 */
    (APTR)C_InvertPixelArray,
    (APTR)C_FillPixelArray,
    (APTR)C_DoCDrawMethodTagList,
    (APTR)C_CVideoCtrlTagList,
    (APTR)C_LockBitMapTagList,
    (APTR)C_UnLockBitMap,
    (APTR)C_UnLockBitMapTagList,
    (APTR)C_ExtractColor,
    (APTR)L_Null,                                                     /* private 7 */
    (APTR)C_WriteLUTPixelArray,
    (APTR)-1
};

BOOL cgx_init(void)
{
    struct Library *lib;

    Forbid();
    lib = (struct Library *)FindName(&SysBase->LibList, libName);
    Permit();
    if (lib) {
        dbg("cgx: a cybergraphics.library is already running - not adding ours\n");
        return FALSE;
    }
    if (!(cgxBase = MakeLibrary(funcs, NULL, NULL, sizeof(struct Library), 0)))
        return FALSE;
    cgxBase->lib_Node.ln_Type = NT_LIBRARY;
    cgxBase->lib_Node.ln_Name = libName;
    cgxBase->lib_Flags = LIBF_SUMUSED | LIBF_CHANGED;
    cgxBase->lib_Version = 41;
    cgxBase->lib_Revision = 99;
    cgxBase->lib_IdString = libId;
    AddLibrary(cgxBase);
    return TRUE;
}

BOOL cgx_remove(void)
{
    ULONG i;
    if (!cgxBase)
        return TRUE;
    Forbid();
    if (cgxBase->lib_OpenCnt) {
        Permit();
        return FALSE;
    }
    Remove(&cgxBase->lib_Node);
    Permit();
    for (i = 0; i < F_COUNT; i++)
        if (calls[i])
            dbg("cgx: function %lu called %u times\n", i, calls[i]);
    FreeMem((UBYTE *)cgxBase - cgxBase->lib_NegSize, cgxBase->lib_NegSize + cgxBase->lib_PosSize);
    cgxBase = NULL;
    return TRUE;
}
