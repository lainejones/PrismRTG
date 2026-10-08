/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laine Jones */
/*
 * p96.c - Prism's own "Picasso96API.library".
 *
 * Programs written for the Picasso96 API open a library of that name; a
 * few (PerfectPaint) only look for the file in LIBS:. Prism provides both:
 * PrismD adds this library to the system in memory, as it does for
 * cybergraphics.library, and the package carries a small library file
 * (tools/p96stub.S) that refuses to open when PrismD isn't running.
 *
 * Everything is implemented on Prism's own bitmaps, modes and clipper.
 * Not available: picture-in-picture windows (no board Prism drives has a
 * video overlay) - p96PIP_OpenTagList fails with PIPERR_NOTAVAILABLE.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/libraries.h>
#include <exec/execbase.h>
#include <graphics/gfx.h>
#include <graphics/rastport.h>
#include <graphics/layers.h>
#include <graphics/modeid.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <libraries/asl.h>
#include <utility/tagitem.h>
#include <utility/hooks.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <proto/utility.h>
#include <proto/asl.h>
#include <string.h>
#include "prism.h"
#include "prismint.h"
#include "p96api.h"

extern struct ExecBase *SysBase;
extern struct Library *UtilityBase;
extern struct IntuitionBase *IntuitionBase;

static struct Library *p96Base;
static char libName[] = "Picasso96API.library";
static char libId[] = "Picasso96API.library 2.500 (4.10.2026) Prism";

static UWORD told;
#define LOG(...) do { if (told < 200) { told++; dbg("p96: " __VA_ARGS__); } } while (0)

/* ---- formats -------------------------------------------------------- */

static const UBYTE rgbf2pf[RGBFB_MaxFormats] = {
    PF_COUNT, PF_CLUT8, PF_RGB24, PF_BGR24, PF_RGB565LE, PF_RGB555LE, PF_ARGB32,
    PF_COUNT /* ABGR: converted here */, PF_RGBA32, PF_BGRA32, PF_RGB565BE, PF_RGB555BE,
    PF_BGR565LE, PF_BGR555LE, PF_COUNT, PF_COUNT
};

static ULONG pf2rgbf(UBYTE fmt)
{
    ULONG i;
    for (i = 1; i < RGBFB_MaxFormats; i++)
        if (rgbf2pf[i] == fmt)
            return i;
    return RGBFB_NONE;
}

static UBYTE rgbf_bpp(ULONG f)
{
    switch (f) {
    case RGBFB_CLUT: return 1;
    case RGBFB_R8G8B8: case RGBFB_B8G8R8: return 3;
    case RGBFB_A8R8G8B8: case RGBFB_A8B8G8R8: case RGBFB_R8G8B8A8: case RGBFB_B8G8R8A8: return 4;
    case RGBFB_R5G6B5PC: case RGBFB_R5G5B5PC: case RGBFB_R5G6B5: case RGBFB_R5G5B5:
    case RGBFB_B5G6R5PC: case RGBFB_B5G5R5PC: return 2;
    }
    return 0;
}

static UBYTE bits_of(UBYTE fmt)
{
    switch (pf_bpp(fmt)) {
    case 1: return 8;
    case 3: return 24;
    case 4: return 32;
    }
    return (fmt == PF_RGB555LE || fmt == PF_RGB555BE || fmt == PF_BGR555LE) ? 15 : 16;
}

/* one pixel of a caller's buffer (direct colour formats) to 0x00RRGGBB */
static inline ULONG get_rgbf(ULONG f, const UBYTE *s)
{
    if (f == RGBFB_A8B8G8R8)
        return ((ULONG)s[3] << 16) | ((ULONG)s[2] << 8) | s[1];
    return pf_get(rgbf2pf[f], s);
}

static inline void put_rgbf(ULONG f, ULONG c, UBYTE *d)
{
    if (f == RGBFB_A8B8G8R8) {
        d[0] = 0; d[1] = c; d[2] = c >> 8; d[3] = c >> 16;
        return;
    }
    pf_put(rgbf2pf[f], c, d);
}

/* nearest pen of an 8-bit bitmap to a colour, remembering the last answer */
static UBYTE near_pen(struct PBitMap *p, ULONG rgb)
{
    static ULONG *lastTab, lastC;
    static UBYTE lastPen;
    UWORD i, best = 0;
    LONG bd = 0x7fffffff;

    rgb &= 0xffffff;
    if (!p->rgbTab)
        return 0;
    if (p->rgbTab == lastTab && rgb == lastC)
        return lastPen;
    for (i = 0; i < 256; i++) {
        ULONG c = p->rgbTab[i];
        LONG dr = (LONG)((c >> 16) & 255) - ((rgb >> 16) & 255);
        LONG dg = (LONG)((c >> 8) & 255) - ((rgb >> 8) & 255);
        LONG db = (LONG)(c & 255) - (rgb & 255);
        LONG d = dr * dr * 3 + dg * dg * 4 + db * db * 2;
        if (d < bd) { bd = d; best = i; }
    }
    lastTab = p->rgbTab; lastC = rgb; lastPen = best;
    return best;
}

/* a colour into one pixel of a Prism bitmap */
static inline void put_pbm(struct PBitMap *p, UBYTE *d, ULONG rgb)
{
    if (p->bpp == 1)
        *d = near_pen(p, rgb);
    else
        pf_put(p->fmt, rgb, d);
}

static inline ULONG get_pbm(struct PBitMap *p, const UBYTE *s)
{
    if (p->bpp == 1)
        return p->rgbTab ? p->rgbTab[*s] : 0;
    return pf_get(p->fmt, s);
}

static inline ULONG be32(const UBYTE *b)
{
    return ((ULONG)b[0] << 24) | ((ULONG)b[1] << 16) | ((ULONG)b[2] << 8) | b[3];
}

static BOOL rp_ours(struct RastPort *rp)
{
    return rp && rp->BitMap && pbm_get(rp->BitMap);
}

/* ---- library housekeeping ------------------------------------------- */

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
    return 0;
}

/* ---- bitmaps -------------------------------------------------------- */

static struct BitMap *P_AllocBitMap(ULONG w __asm("d0"), ULONG h __asm("d1"),
                                    ULONG depth __asm("d2"), ULONG flags __asm("d3"),
                                    struct BitMap *friend __asm("a0"), ULONG fmt __asm("d7"))
{
    struct BitMap *bm;
    UBYTE pf = fmt < RGBFB_MaxFormats ? rgbf2pf[fmt] : PF_COUNT;

    flags &= ~(ULONG)P96_BMF_USERPRIVATE & 0xffff;
    if (friend && pbm_get(friend)) {
        /* like its friend: same format, on the board if there is room */
        bm = AllocBitMap(w, h, depth, flags, friend);
    } else if (pf != PF_COUNT) {
        /* a format of the caller's choosing: Prism's AllocBitMap patch
         * takes the request in CyberGraphX form (bit 7 + format in the top
         * byte) */
        bm = AllocBitMap(w, h, depth > 8 ? 8 : depth,
                         (flags & (BMF_CLEAR | BMF_DISPLAYABLE)) | 0x80 |
                         (pf_to_pixfmt(pf) << 24), NULL);
    } else {
        bm = AllocBitMap(w, h, depth > 8 ? 8 : depth, flags, friend);
    }
    LOG("AllocBitMap %lux%lux%lu flags %lx fmt %lu -> %lx\n", w, h, depth, flags, fmt, (ULONG)bm);
    return bm;
}

static void P_FreeBitMap(struct BitMap *bm __asm("a0"))
{
    if (bm)
        FreeBitMap(bm);
}

static ULONG P_GetBitMapAttr(struct BitMap *bm __asm("a0"), ULONG attr __asm("d0"))
{
    struct PBitMap *p = pbm_get(bm);
    ULONG r = 0;

    if (!bm)
        return 0;
    if (!p) {
        /* not one of ours: what graphics.library knows */
        switch (attr) {
        case P96BMA_WIDTH:  r = GetBitMapAttr(bm, BMA_WIDTH); break;
        case P96BMA_HEIGHT: r = GetBitMapAttr(bm, BMA_HEIGHT); break;
        case P96BMA_DEPTH: case P96BMA_BITSPERPIXEL: r = GetBitMapAttr(bm, BMA_DEPTH); break;
        case P96BMA_BYTESPERROW: r = bm->BytesPerRow; break;
        }
        return r;
    }
    switch (attr) {
    case P96BMA_WIDTH:         r = p->w; break;
    case P96BMA_HEIGHT:        r = p->h; break;
    case P96BMA_DEPTH:
    case P96BMA_BITSPERPIXEL:  r = bits_of(p->fmt); break;
    case P96BMA_MEMORY:        r = (ULONG)p->pix; break;
    case P96BMA_BYTESPERROW:   r = p->bpr; break;
    case P96BMA_BYTESPERPIXEL: r = p->bpp; break;
    case P96BMA_RGBFORMAT:     r = pf2rgbf(p->fmt); break;
    case P96BMA_ISP96:         r = TRUE; break;
    case P96BMA_ISONBOARD:     r = p->inVram ? TRUE : FALSE; break;
    case P96BMA_BOARDMEMBASE:  r = (ULONG)board.vram; break;
    case P96BMA_BOARDIOBASE:
    case P96BMA_BOARDMEMIOBASE: r = (ULONG)board.regs; break;
    }
    LOG("GetBitMapAttr %lx %lu -> %lx\n", (ULONG)bm, attr, r);
    return r;
}

/* The caller gets the pixels' address: they must not move (VRAM paging)
 * and no blit may be running until p96UnlockBitMap. */
static LONG P_LockBitMap(struct BitMap *bm __asm("a0"), UBYTE *buf __asm("a1"),
                         ULONG size __asm("d0"))
{
    struct PBitMap *p = pbm_get(bm);
    struct P96RenderInfo ri;

    LOG("LockBitMap %lx -> %s\n", (ULONG)bm, p ? "ours" : "not ours");
    if (!p)
        return 0;
    ObtainSemaphore(&lock);
    p->locks++;
    if (p->inVram && board.waitBlit)
        board.waitBlit(&board);
    ReleaseSemaphore(&lock);
    ri.Memory = p->pix;
    ri.BytesPerRow = p->bpr;
    ri.pad = 0;
    ri.RGBFormat = pf2rgbf(p->fmt);
    if (buf)
        CopyMem(&ri, buf, size < sizeof(ri) ? size : sizeof(ri));
    return (LONG)p;
}

static void P_UnlockBitMap(struct BitMap *bm __asm("a0"), LONG handle __asm("d0"))
{
    struct PBitMap *p = (struct PBitMap *)handle;
    ObtainSemaphore(&lock);
    if (p && pbm_get(bm) == p && p->locks)
        p->locks--;
    ReleaseSemaphore(&lock);
}

/* ---- modes ---------------------------------------------------------- */

struct ModeFilter {
    ULONG minW, maxW, minH, maxH, minD, maxD, allowed, forbidden;
};

static void filter_tags(struct ModeFilter *f, struct TagItem *tags)
{
    f->minW = GetTagData(P96MA_MinWidth, 0, tags);
    f->maxW = GetTagData(P96MA_MaxWidth, 0xffff, tags);
    f->minH = GetTagData(P96MA_MinHeight, 0, tags);
    f->maxH = GetTagData(P96MA_MaxHeight, 0xffff, tags);
    f->minD = GetTagData(P96MA_MinDepth, 0, tags);
    f->maxD = GetTagData(P96MA_MaxDepth, 32, tags);
    f->allowed = GetTagData(P96MA_FormatsAllowed, ~0UL, tags);
    f->forbidden = GetTagData(P96MA_FormatsForbidden, 0, tags);
}

static BOOL mode_fits(const struct ModeFilter *f, const struct PrismModeInfo *mi)
{
    ULONG bits = bits_of(mi->fmt), ff = RGBFF(pf2rgbf(mi->fmt));
    return mi->w >= f->minW && mi->w <= f->maxW && mi->h >= f->minH && mi->h <= f->maxH &&
           bits >= f->minD && bits <= f->maxD && (ff & f->allowed) && !(ff & f->forbidden);
}

static ULONG P_BestModeIDTagList(struct TagItem *tags __asm("a0"))
{
    ULONG nw = GetTagData(P96BIDTAG_NominalWidth, 640, tags);
    ULONG nh = GetTagData(P96BIDTAG_NominalHeight, 480, tags);
    ULONG depth = GetTagData(P96BIDTAG_Depth, 8, tags);
    ULONG allowed = GetTagData(P96BIDTAG_FormatsAllowed, ~0UL, tags);
    ULONG forbidden = GetTagData(P96BIDTAG_FormatsForbidden, 0, tags);
    /* depth class: 1 = 8-bit, 2 = 15/16-bit, 3 = 24/32-bit */
    UBYTE want = depth > 16 ? 3 : depth > 8 ? 2 : 1, cls;
    ULONG i, best = INVALID_ID, bestArea, big = INVALID_ID, bigArea;
    struct PrismModeInfo mi;

    /* the smallest mode of that class at least the nominal size, else the
     * biggest of the class; true colour may fall back to 16-bit */
    for (cls = want; cls >= (want > 1 ? 2 : 1) && best == INVALID_ID; cls--) {
        bestArea = 0xffffffff;
        bigArea = 0;
        for (i = 0; prism_mode(i, &mi); i++) {
            ULONG area = (ULONG)mi.w * mi.h, ff = RGBFF(pf2rgbf(mi.fmt));
            if ((mi.bpp > 3 ? 3 : mi.bpp) != cls || !(ff & allowed) || (ff & forbidden))
                continue;
            if (mi.w >= nw && mi.h >= nh && area < bestArea) { best = mi.id; bestArea = area; }
            if (area > bigArea) { big = mi.id; bigArea = area; }
        }
        if (best == INVALID_ID)
            best = big;
    }
    LOG("BestModeID %lux%lu depth %lu -> %lx\n", nw, nh, depth, best);
    return best;
}

static ULONG req_filter(struct Hook *h __asm("a0"), APTR rq __asm("a2"), ULONG id __asm("a1"))
{
    struct PrismModeInfo mi;
    return prism_mode_by_id(id, &mi) && mode_fits(h->h_Data, &mi);
}

/* asl.library's screen mode requester, showing the Prism modes that fit */
static ULONG P_RequestModeIDTagList(struct TagItem *tags __asm("a0"))
{
    struct Library *AslBase;
    struct ScreenModeRequester *sm;
    struct ModeFilter f;
    struct Hook hook;
    ULONG id = INVALID_ID;
    ULONG initial = GetTagData(P96MA_DisplayID, INVALID_ID, tags);
    struct Screen *scr = (struct Screen *)GetTagData(P96MA_Screen, 0, tags);
    struct Window *win = (struct Window *)GetTagData(P96MA_Window, 0, tags);
    STRPTR pub = (STRPTR)GetTagData(P96MA_PubScreenName, 0, tags);

    filter_tags(&f, tags);
    memset(&hook, 0, sizeof(hook));
    hook.h_Entry = (ULONG (*)())req_filter;
    hook.h_Data = &f;
    if ((AslBase = OpenLibrary("asl.library", 38))) {
        struct TagItem at[] = {
            { ASLSM_TitleText, GetTagData(P96MA_WindowTitle, (ULONG)"Select a screen mode", tags) },
            { ASLSM_PositiveText, GetTagData(P96MA_OKText, (ULONG)"OK", tags) },
            { ASLSM_NegativeText, GetTagData(P96MA_CancelText, (ULONG)"Cancel", tags) },
            { ASLSM_FilterFunc, (ULONG)&hook },
            { initial != INVALID_ID ? ASLSM_InitialDisplayID : TAG_IGNORE, initial },
            { win ? ASLSM_Window : TAG_IGNORE, (ULONG)win },
            { scr ? ASLSM_Screen : TAG_IGNORE, (ULONG)scr },
            { pub ? ASLSM_PubScreenName : TAG_IGNORE, (ULONG)pub },
            { TAG_DONE, 0 }
        };
        if ((sm = AllocAslRequest(ASL_ScreenModeRequest, at))) {
            if (AslRequest(sm, NULL))
                id = sm->sm_DisplayID;
            FreeAslRequest(sm);
        }
        CloseLibrary(AslBase);
    }
    LOG("RequestModeID -> %lx\n", id);
    return id == INVALID_ID ? 0 : id;      /* 0 = cancelled, as callers test */
}

static struct List *P_AllocModeListTagList(struct TagItem *tags __asm("a0"))
{
    struct ModeFilter f;
    struct PrismModeInfo mi;
    struct List *l;
    ULONG i;

    filter_tags(&f, tags);
    if (!(l = AllocVec(sizeof(*l), MEMF_PUBLIC | MEMF_CLEAR)))
        return NULL;
    l->lh_Head = (struct Node *)&l->lh_Tail;
    l->lh_TailPred = (struct Node *)&l->lh_Head;
    for (i = 0; prism_mode(i, &mi); i++) {
        struct P96ModeNode *n;
        if (!mode_fits(&f, &mi) || !(n = AllocVec(sizeof(*n), MEMF_PUBLIC | MEMF_CLEAR)))
            continue;
        strncpy(n->Description, mi.name, P96_MODENAMELENGTH - 1);
        n->Node.ln_Name = n->Description;
        n->Width = mi.w;
        n->Height = mi.h;
        n->Depth = bits_of(mi.fmt);
        n->DisplayID = mi.id;
        AddTail(l, &n->Node);
    }
    return l;
}

static void P_FreeModeList(struct List *l __asm("a0"))
{
    struct Node *n;
    if (!l)
        return;
    while ((n = RemHead(l)))
        FreeVec(n);
    FreeVec(l);
}

static ULONG P_GetModeIDAttr(ULONG id __asm("d0"), ULONG attr __asm("d1"))
{
    struct PrismModeInfo mi;
    ULONG r = 0;

    if (prism_mode_by_id(id, &mi)) {
        switch (attr) {
        case P96IDA_WIDTH:          r = mi.w; break;
        case P96IDA_HEIGHT:         r = mi.h; break;
        case P96IDA_DEPTH:
        case P96IDA_BITSPERPIXEL:   r = bits_of(mi.fmt); break;
        case P96IDA_BYTESPERPIXEL:  r = mi.bpp; break;
        case P96IDA_RGBFORMAT:      r = pf2rgbf(mi.fmt); break;
        case P96IDA_ISP96:          r = TRUE; break;
        case P96IDA_BOARDNUMBER:    r = 0; break;
        case P96IDA_STDBYTESPERROW: r = ((ULONG)mi.w * mi.bpp + 7) & ~7UL; break;
        case P96IDA_BOARDNAME:      r = (ULONG)board.name; break;
        case P96IDA_COMPATIBLEFORMATS: r = RGBFF(pf2rgbf(mi.fmt)); break;
        }
    }
    LOG("GetModeIDAttr %lx %lu -> %lx\n", id, attr, r);
    return r;
}

/* ---- screens -------------------------------------------------------- */

/* P96SA_Left .. P96SA_VideoControl, in order */
static const Tag saMap[] = {
    SA_Left, SA_Top, SA_Width, SA_Height, SA_Depth, SA_DetailPen, SA_BlockPen, SA_Title,
    SA_Colors, SA_ErrorCode, SA_Font, SA_SysFont, SA_Type, SA_BitMap, SA_PubName, SA_PubSig,
    SA_PubTask, SA_DisplayID, SA_DClip, SA_ShowTitle, SA_Behind, SA_Quiet, SA_AutoScroll,
    SA_Pens, SA_SharePens, SA_BackFill, SA_Colors32, SA_VideoControl
};
#define NSA (sizeof(saMap) / sizeof(saMap[0]))

static struct Screen *P_OpenScreenTagList(struct TagItem *tags __asm("a0"))
{
    struct TagItem out[NSA + 8], *ti, *tl = tags;
    ULONG n = 0, id = INVALID_ID, w = 640, h = 480, depth = 8, fmt = RGBFB_NONE;
    BOOL haveW = FALSE, haveH = FALSE;
    struct Screen *s;

    while ((ti = NextTagItem(&tl))) {
        ULONG k = ti->ti_Tag - P96SA_Left;
        if (ti->ti_Tag < P96SA_Left || ti->ti_Tag > P96SA_ConstantByteSwapping) {
            /* an Intuition tag among them passes through */
            if (n < NSA + 4) { out[n].ti_Tag = ti->ti_Tag; out[n++].ti_Data = ti->ti_Data; }
            continue;
        }
        switch (ti->ti_Tag) {
        case P96SA_Width:     w = ti->ti_Data; haveW = TRUE; break;
        case P96SA_Height:    h = ti->ti_Data; haveH = TRUE; break;
        case P96SA_Depth:     depth = ti->ti_Data; break;
        case P96SA_DisplayID: id = ti->ti_Data; break;
        case P96SA_RGBFormat: fmt = ti->ti_Data; break;
        case P96SA_Exclusive:
            out[n].ti_Tag = SA_Exclusive; out[n++].ti_Data = ti->ti_Data;
            break;
        default:
            if (k < NSA) { out[n].ti_Tag = saMap[k]; out[n++].ti_Data = ti->ti_Data; }
            /* NoSprite, NoMemory, RenderFunc, SaveFunc, UserData, Alignment,
             * FixedScreen, ConstantBytesPerRow, ConstantByteSwapping: Prism
             * screens have fixed rows and byte order anyway */
        }
    }
    if (id == INVALID_ID) {
        struct TagItem bt[] = {
            { P96BIDTAG_NominalWidth, w }, { P96BIDTAG_NominalHeight, h },
            { P96BIDTAG_Depth, depth },
            { fmt != RGBFB_NONE ? P96BIDTAG_FormatsAllowed : TAG_IGNORE, RGBFF(fmt) },
            { TAG_DONE, 0 }
        };
        id = P_BestModeIDTagList(bt);
        if (id == INVALID_ID && fmt != RGBFB_NONE) {
            bt[3].ti_Tag = TAG_IGNORE;       /* no mode in that format: any */
            id = P_BestModeIDTagList(bt);
        }
    }
    if (id == INVALID_ID)
        return NULL;
    out[n].ti_Tag = SA_DisplayID; out[n++].ti_Data = id;
    out[n].ti_Tag = SA_Depth;     out[n++].ti_Data = depth > 8 ? 8 : depth;
    if (haveW) { out[n].ti_Tag = SA_Width;  out[n++].ti_Data = w; }
    if (haveH) { out[n].ti_Tag = SA_Height; out[n++].ti_Data = h; }
    out[n].ti_Tag = TAG_DONE;
    s = OpenScreenTagList(NULL, out);
    LOG("OpenScreen %lux%lu depth %lu fmt %lu id %lx -> %lx\n", w, h, depth, fmt, id, (ULONG)s);
    return s;
}

static BOOL P_CloseScreen(struct Screen *s __asm("a0"))
{
    return s ? CloseScreen(s) : FALSE;
}

/* ---- pixel arrays --------------------------------------------------- */

struct ArrCtx {
    UBYTE *mem;                  /* caller's buffer                        */
    LONG   mod;
    ULONG  fmt;                  /* its RGBFB_ format                      */
    UBYTE  bpp;
    WORD   sx, sy;               /* buffer pixel of rp (dx, dy)            */
    WORD   dx, dy;
    /* true colour planes (p96Write/ReadTrueColorData) */
    const struct P96TrueColorInfo *tci;
};

#define rowbuf PRISM_ROWBUF        /* 4096 * 4 bytes, see prismint.h */

static void wr_cb(struct PBitMap *p, WORD bx0, WORD by0, WORD bx1, WORD by1,
                  WORD ox, WORD oy, void *ctx)
{
    struct ArrCtx *a = ctx;
    WORD y, x, n = bx1 - bx0 + 1;

    if (n > 4096) n = 4096;
    for (y = by0; y <= by1; y++) {
        LONG srow = a->sy + (y - oy - a->dy), scol = a->sx + (bx0 - ox - a->dx);
        UBYTE *row = p->pix + (ULONG)y * p->bpr + (ULONG)bx0 * p->bpp, *d = rowbuf;

        if (a->tci) {
            const struct P96TrueColorInfo *t = a->tci;
            ULONG off = (ULONG)srow * t->BytesPerRow + (ULONG)scol * t->PixelDistance;
            const UBYTE *r = t->RedData + off, *g = t->GreenData + off, *b = t->BlueData + off;
            for (x = 0; x < n; x++, d += p->bpp, r += t->PixelDistance,
                 g += t->PixelDistance, b += t->PixelDistance)
                put_pbm(p, d, ((ULONG)*r << 16) | ((ULONG)*g << 8) | *b);
        } else {
            const UBYTE *s = a->mem + srow * a->mod + scol * a->bpp;
            if (a->fmt < RGBFB_MaxFormats && rgbf2pf[a->fmt] == p->fmt) {
                /* same format: the bytes as they are */
                CopyMem((APTR)s, row, (ULONG)n * p->bpp);
                continue;
            }
            if (a->fmt == RGBFB_CLUT) {
                /* pens, through the bitmap's palette */
                if (p->bpp == 2 && p->penTab)
                    for (x = 0; x < n; x++) ((UWORD *)d)[x] = p->penTab[s[x]];
                else
                    for (x = 0; x < n; x++, d += p->bpp)
                        pf_put(p->fmt, p->rgbTab ? p->rgbTab[s[x]] : 0, d);
            } else {
                for (x = 0; x < n; x++, s += a->bpp, d += p->bpp)
                    put_pbm(p, d, get_rgbf(a->fmt, s));
            }
        }
        CopyMem(rowbuf, row, (ULONG)n * p->bpp);
    }
}

static void rd_cb(struct PBitMap *p, WORD bx0, WORD by0, WORD bx1, WORD by1,
                  WORD ox, WORD oy, void *ctx)
{
    struct ArrCtx *a = ctx;
    WORD y, x, n = bx1 - bx0 + 1;

    for (y = by0; y <= by1; y++) {
        LONG drow = a->sy + (y - oy - a->dy), dcol = a->sx + (bx0 - ox - a->dx);
        const UBYTE *s = p->pix + (ULONG)y * p->bpr + (ULONG)bx0 * p->bpp;

        if (a->tci) {
            const struct P96TrueColorInfo *t = a->tci;
            ULONG off = (ULONG)drow * t->BytesPerRow + (ULONG)dcol * t->PixelDistance;
            UBYTE *r = t->RedData + off, *g = t->GreenData + off, *b = t->BlueData + off;
            for (x = 0; x < n; x++, s += p->bpp, r += t->PixelDistance,
                 g += t->PixelDistance, b += t->PixelDistance) {
                ULONG c = get_pbm(p, s);
                *r = c >> 16; *g = c >> 8; *b = c;
            }
        } else {
            UBYTE *d = a->mem + drow * a->mod + dcol * a->bpp;
            if (a->fmt < RGBFB_MaxFormats && rgbf2pf[a->fmt] == p->fmt)
                CopyMem((APTR)s, d, (ULONG)n * p->bpp);
            else if (a->fmt == RGBFB_CLUT)
                for (x = 0; x < n; x++, s += p->bpp)
                    d[x] = p->bpp == 1 ? *s : near_pen(p, pf_get(p->fmt, s));
            else
                for (x = 0; x < n; x++, s += p->bpp, d += a->bpp)
                    put_rgbf(a->fmt, get_pbm(p, s), d);
        }
    }
}

static BOOL arr_ok(const struct P96RenderInfo *ri, struct ArrCtx *a)
{
    if (!ri || !ri->Memory || ri->RGBFormat >= RGBFB_MaxFormats || !(a->bpp = rgbf_bpp(ri->RGBFormat)))
        return FALSE;
    a->mem = ri->Memory;
    a->mod = ri->BytesPerRow;
    a->fmt = ri->RGBFormat;
    a->tci = NULL;
    return TRUE;
}

static void P_WritePixelArray(struct P96RenderInfo *ri __asm("a0"), UWORD sx __asm("d0"),
                              UWORD sy __asm("d1"), struct RastPort *rp __asm("a1"),
                              UWORD dx __asm("d2"), UWORD dy __asm("d3"),
                              UWORD w __asm("d4"), UWORD h __asm("d5"))
{
    struct ArrCtx a;

    LOG("WritePixelArray %ux%u fmt %lu\n", w, h, ri ? ri->RGBFormat : 0);
    if (!w || !h || !arr_ok(ri, &a))
        return;
    if (!rp_ours(rp)) {
        /* a native screen: pens only, the system's way */
        if (rp && a.fmt == RGBFB_CLUT)
            WriteChunkyPixels(rp, dx, dy, dx + w - 1, dy + h - 1,
                              a.mem + (LONG)sy * a.mod + sx, a.mod);
        return;
    }
    a.sx = sx; a.sy = sy; a.dx = dx; a.dy = dy;
    clip_rp(rp, dx, dy, dx + w - 1, dy + h - 1, wr_cb, &a);
}

static void P_ReadPixelArray(struct P96RenderInfo *ri __asm("a0"), UWORD dx __asm("d0"),
                             UWORD dy __asm("d1"), struct RastPort *rp __asm("a1"),
                             UWORD sx __asm("d2"), UWORD sy __asm("d3"),
                             UWORD w __asm("d4"), UWORD h __asm("d5"))
{
    struct ArrCtx a;

    LOG("ReadPixelArray %ux%u fmt %lu\n", w, h, ri ? ri->RGBFormat : 0);
    if (!w || !h || !arr_ok(ri, &a) || !rp_ours(rp))
        return;
    a.sx = dx; a.sy = dy; a.dx = sx; a.dy = sy;
    clip_rp(rp, sx, sy, sx + w - 1, sy + h - 1, rd_cb, &a);
}

static void P_WriteTrueColorData(struct P96TrueColorInfo *tci __asm("a0"), UWORD sx __asm("d0"),
                                 UWORD sy __asm("d1"), struct RastPort *rp __asm("a1"),
                                 UWORD dx __asm("d2"), UWORD dy __asm("d3"),
                                 UWORD w __asm("d4"), UWORD h __asm("d5"))
{
    struct ArrCtx a;

    if (!tci || !w || !h || !rp_ours(rp))
        return;
    a.tci = tci; a.mem = NULL; a.mod = 0; a.fmt = RGBFB_NONE; a.bpp = 0;
    a.sx = sx; a.sy = sy; a.dx = dx; a.dy = dy;
    clip_rp(rp, dx, dy, dx + w - 1, dy + h - 1, wr_cb, &a);
}

static void P_ReadTrueColorData(struct P96TrueColorInfo *tci __asm("a0"), UWORD dx __asm("d0"),
                                UWORD dy __asm("d1"), struct RastPort *rp __asm("a1"),
                                UWORD sx __asm("d2"), UWORD sy __asm("d3"),
                                UWORD w __asm("d4"), UWORD h __asm("d5"))
{
    struct ArrCtx a;

    if (!tci || !w || !h || !rp_ours(rp))
        return;
    a.tci = tci; a.mem = NULL; a.mod = 0; a.fmt = RGBFB_NONE; a.bpp = 0;
    a.sx = dx; a.sy = dy; a.dx = sx; a.dy = sy;
    clip_rp(rp, sx, sy, sx + w - 1, sy + h - 1, rd_cb, &a);
}

/* ---- single colours ------------------------------------------------- */

struct ColCtx { ULONG rgb; ULONG got; };

static void fill_cb(struct PBitMap *p, WORD bx0, WORD by0, WORD bx1, WORD by1,
                    WORD ox, WORD oy, void *ctx)
{
    struct ColCtx *c = ctx;
    WORD y, x, n = bx1 - bx0 + 1;
    UBYTE px[4] = { 0, 0, 0, 0 };

    /* on the card where it can: 8- and 16-bit fills, 32-bit on a blitter
     * that takes 4-byte pixels */
    put_pbm(p, px, c->rgb);
    if (p->inVram && (board.fillRect || (board.ops && board.ops->fill)) && (LONG)n * (by1 - by0 + 1) >= 12 &&
        (p->bpp <= 2 || (p->bpp == 4 && (board.flags & PBF_BLIT_32)))) {
        ULONG v = p->bpp == 1 ? px[0] : p->bpp == 2 ? (((ULONG)px[0] << 8) | px[1]) : be32(px);
        if (pbm_fill(p,p->bpp,bx0,by0,n,by1-by0+1,v) == PR_DONE) return;
    }
    if (n > 4096) n = 4096;
    for (x = 0; x < n; x++)
        memcpy(rowbuf + (ULONG)x * p->bpp, px, p->bpp);
    for (y = by0; y <= by1; y++)
        CopyMem(rowbuf, p->pix + (ULONG)y * p->bpr + (ULONG)bx0 * p->bpp, (ULONG)n * p->bpp);
}

static void get_cb(struct PBitMap *p, WORD bx0, WORD by0, WORD bx1, WORD by1,
                   WORD ox, WORD oy, void *ctx)
{
    struct ColCtx *c = ctx;
    c->got = get_pbm(p, p->pix + (ULONG)by0 * p->bpr + (ULONG)bx0 * p->bpp);
}

static ULONG P_WritePixel(struct RastPort *rp __asm("a1"), UWORD x __asm("d0"),
                          UWORD y __asm("d1"), ULONG colour __asm("d2"))
{
    struct ColCtx c;
    if (!rp_ours(rp))
        return (ULONG)-1;
    c.rgb = colour & 0xffffff;
    clip_rp_q(rp, x, y, x, y, fill_cb, &c, TRUE);
    return 0;
}

static ULONG P_ReadPixel(struct RastPort *rp __asm("a1"), UWORD x __asm("d0"), UWORD y __asm("d1"))
{
    struct ColCtx c;
    c.got = 0;
    if (rp_ours(rp))
        clip_rp_q(rp, x, y, x, y, get_cb, &c, TRUE);
    return c.got;
}

static void P_RectFill(struct RastPort *rp __asm("a1"), UWORD x0 __asm("d0"), UWORD y0 __asm("d1"),
                       UWORD x1 __asm("d2"), UWORD y1 __asm("d3"), ULONG colour __asm("d4"))
{
    struct ColCtx c;
    if (!rp_ours(rp) || x1 < x0 || y1 < y0)
        return;
    c.rgb = colour & 0xffffff;
    clip_rp(rp, x0, y0, x1, y1, fill_cb, &c);
}

/* a colour as one pixel of a format, the way the 68k would store it */
static ULONG P_EncodeColor(ULONG fmt __asm("d0"), ULONG colour __asm("d1"))
{
    UBYTE px[4] = { 0, 0, 0, 0 };
    if (fmt >= RGBFB_MaxFormats || rgbf_bpp(fmt) < 2)
        return colour;
    put_rgbf(fmt, colour & 0xffffff, px);
    switch (rgbf_bpp(fmt)) {
    case 2: return ((ULONG)px[0] << 8) | px[1];
    case 3: return ((ULONG)px[0] << 16) | ((ULONG)px[1] << 8) | px[2];
    }
    return be32(px);
}

/* ---- picture-in-picture: not on these boards ------------------------- */

static struct Window *P_PIP_OpenTagList(struct TagItem *tags __asm("a0"))
{
    ULONG *err = (ULONG *)GetTagData(P96PIP_ErrorCode, 0, tags);
    if (err)
        *err = PIPERR_NOTAVAILABLE;
    return NULL;
}

/* ---- the system and the board --------------------------------------- */

ULONG vram_free_bytes(ULONG *largest);       /* prismd.c */

static LONG P_GetRTGDataTagList(struct TagItem *tags __asm("a0"))
{
    struct TagItem *ti, *tl = tags;
    LONG n = 0;
    while ((ti = NextTagItem(&tl)))
        if (ti->ti_Tag == P96RD_NumberOfBoards && ti->ti_Data) {
            *(ULONG *)ti->ti_Data = 1;
            n++;
        }
    return n;
}

static LONG P_GetBoardDataTagList(ULONG boardNo __asm("d0"), struct TagItem *tags __asm("a0"))
{
    struct TagItem *ti, *tl = tags;
    ULONG freeBytes, largest, i, formats = 0;
    LONG n = 0;

    if (boardNo != 0)
        return 0;
    ObtainSemaphore(&lock);
    freeBytes = vram_free_bytes(&largest);
    ReleaseSemaphore(&lock);
    for (i = 1; i < RGBFB_MaxFormats; i++)
        if (rgbf2pf[i] != PF_COUNT && (board.formats & PF_BIT(rgbf2pf[i])))
            formats |= RGBFF(i);
    while ((ti = NextTagItem(&tl))) {
        ULONG *v = (ULONG *)ti->ti_Data;
        if (!v)
            continue;
        n++;
        switch (ti->ti_Tag) {
        case P96BD_BoardName:
        case P96BD_ChipName:          *v = (ULONG)board.name; break;
        case P96BD_TotalMemory:       *v = board.vramSize; break;
        case P96BD_FreeMemory:        *v = freeBytes; break;
        case P96BD_LargestFreeMemory: *v = largest; break;
        case P96BD_MonitorSwitch:     *v = TRUE; break;
        case P96BD_RGBFormats:        *v = formats; break;
        case P96BD_MemoryClock:       *v = 0; break;
        default:                      n--;
        }
    }
    return n;
}

/* ---- the library ---------------------------------------------------- */

static const APTR funcs[] = {
    (APTR)L_Open, (APTR)L_Close, (APTR)L_Expunge, (APTR)L_Null,
    (APTR)P_AllocBitMap,               /*  -30 */
    (APTR)P_FreeBitMap,
    (APTR)P_GetBitMapAttr,
    (APTR)P_LockBitMap,
    (APTR)P_UnlockBitMap,
    (APTR)P_BestModeIDTagList,         /*  -60 */
    (APTR)P_RequestModeIDTagList,
    (APTR)P_AllocModeListTagList,
    (APTR)P_FreeModeList,
    (APTR)P_GetModeIDAttr,
    (APTR)P_OpenScreenTagList,         /*  -90 */
    (APTR)P_CloseScreen,
    (APTR)P_WritePixelArray,
    (APTR)P_ReadPixelArray,
    (APTR)P_WritePixel,
    (APTR)P_ReadPixel,                 /* -120 */
    (APTR)P_RectFill,
    (APTR)P_WriteTrueColorData,
    (APTR)P_ReadTrueColorData,
    (APTR)P_PIP_OpenTagList,
    (APTR)L_Null,                      /* -150 p96PIP_Close       */
    (APTR)L_Null,                      /*      p96PIP_SetTagList  */
    (APTR)L_Null,                      /*      p96PIP_GetTagList  */
    (APTR)L_Null,                      /*      p96PIP_GetIMsg     */
    (APTR)L_Null,                      /*      p96PIP_ReplyIMsg   */
    (APTR)P_GetRTGDataTagList,         /* -180 */
    (APTR)P_GetBoardDataTagList,
    (APTR)P_EncodeColor,
    (APTR)-1
};

BOOL p96_init(void)
{
    struct Library *lib;

    Forbid();
    lib = (struct Library *)FindName(&SysBase->LibList, libName);
    Permit();
    if (lib) {
        dbg("p96: a Picasso96API.library is already running - not adding ours\n");
        return FALSE;
    }
    if (!(p96Base = MakeLibrary((APTR)funcs, NULL, NULL, sizeof(struct Library), 0)))
        return FALSE;
    p96Base->lib_Node.ln_Type = NT_LIBRARY;
    p96Base->lib_Node.ln_Name = libName;
    p96Base->lib_Flags = LIBF_SUMUSED | LIBF_CHANGED;
    p96Base->lib_Version = 2;
    p96Base->lib_Revision = 500;
    p96Base->lib_IdString = libId;
    AddLibrary(p96Base);
    return TRUE;
}

BOOL p96_remove(void)
{
    if (!p96Base)
        return TRUE;
    Forbid();
    if (p96Base->lib_OpenCnt) {
        Permit();
        return FALSE;
    }
    Remove(&p96Base->lib_Node);
    Permit();
    FreeMem((UBYTE *)p96Base - p96Base->lib_NegSize, p96Base->lib_NegSize + p96Base->lib_PosSize);
    p96Base = NULL;
    return TRUE;
}
