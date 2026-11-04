/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laine Jones */
/*
 * cgxvideo.c - Prism's own cgxvideo.library: the video layers (overlays)
 * video players use. None of the boards PrismRTG drives has an overlay
 * engine, so a video layer is a Picasso96-style PIP over the player's
 * window: the player writes its YUV (or 15/16-bit RGB) frames into a
 * bitmap in fast RAM, and the compositor converts and scales them into
 * the screen every tick while the layer is attached. Colour keying is
 * taken as a request and answered with a pen, but the layer's area is
 * drawn over whatever the player painted there, so the key never shows.
 *
 * The API and its tag values are those of cybergraphx/cgxvideo.h
 * (CyberGraphX V4; the MorphOS SDK publishes them). Function order:
 * CreateVLayerHandleTagList -30, DeleteVLayerHandle -36,
 * AttachVLayerTagList -42, DetachVLayer -48, GetVLayerAttr -54,
 * LockVLayer -60, UnlockVLayer -66, SetVLayerAttrTagList -72,
 * (-78..-90 reserved), SwapVLayerBuffer -96, WriteSPLine -102,
 * QueryVLayerAttr -108.
 */
#include <exec/types.h>
#include <exec/libraries.h>
#include <exec/memory.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <graphics/view.h>
#include <utility/tagitem.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/utility.h>
#include <string.h>
#include "prismboard.h"
#include "prism.h"
#include "prismint.h"

#define VOA_LeftIndent     0x88000001
#define VOA_RightIndent    0x88000002
#define VOA_TopIndent      0x88000003
#define VOA_BottomIndent   0x88000004
#define VOA_SrcType        0x88000005
#define VOA_SrcWidth       0x88000006
#define VOA_SrcHeight      0x88000007
#define VOA_Error          0x88000008
#define VOA_UseColorKey    0x88000009
#define VOA_UseBackfill    0x8800000a
#define VOA_Identifier     0x8800000b
#define VOA_UseFilter      0x8800000c
#define VOA_DoubleBuffer   0x8800000d
#define VOA_BaseAddress    0x88000030
#define VOA_ColorKeyPen    0x88000031
#define VOA_ColorKey       0x88000032
#define VOA_FrameBase0     0x88000033
#define VOA_FrameBase1     0x88000034
#define VOA_FrameType      0x88000035
#define VOA_Width          0x88000036
#define VOA_Height         0x88000037
#define VOA_Modulo         0x88000038
#define VOA_BaseOffset     0x88000039

#define VOERR_OK           0
#define VOERR_INVSCRMODE   1
#define VOERR_NOOVLMEMORY  2
#define VOERR_INVSRCFMT    3
#define VOERR_NOMEMORY     4

#define SRCFMT_YUV16       0
#define SRCFMT_YCbCr16     1
#define SRCFMT_RGB15       2
#define SRCFMT_RGB16       3

#define VSQ_Dummy              (TAG_USER + 0xA5000)
#define VSQ_SupportedFeatures  (VSQ_Dummy + 1)
#define VSQ_SupportedFormats   (VSQ_Dummy + 2)
#define VSQ_MaxWidth           (VSQ_Dummy + 3)
#define VSQ_FEAT_OVERLAY       (1 << 0)
#define VSQ_FEAT_COLORKEYING   (1 << 3)
#define VSQ_FMT_YUYV           (1 << 0)
#define VSQ_FMT_R5G5B5_LE      (1 << 1)
#define VSQ_FMT_R5G6B5_LE      (1 << 2)

struct VLayer {
    struct Screen  *screen;
    struct Window  *window;      /* attached to, or NULL                 */
    struct PBitMap *source;      /* the frames, in fast RAM              */
    APTR            pip;         /* p96.c's PIP while attached           */
    LONG            indent[4];   /* left, top, right, bottom             */
    LONG            keyPen;      /* -1: no colour key                    */
    BOOL            sharedKey;   /* the key is the shared black: no board overlay */
    ULONG           keyRGB;
    UWORD           w, h;
    UBYTE           srcType;
};

static char libName[] = "cgxvideo.library";
static char libId[] = "$VER: cgxvideo.library 42.1 (PrismRTG, software video layers)";
static struct Library *vidBase;

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
    return 0;                            /* PrismD removes it (cgxvideo_remove) */
}

static ULONG L_Null(void)
{
    return 0;
}

static void vl_detach(struct VLayer *v)
{
    if (v->pip) {
        p96_pip_detach(v->pip);
        v->pip = NULL;
    }
    v->window = NULL;
}

static struct VLayer *V_CreateVLayerHandleTagList(struct Screen *screen __asm("a0"),
                                                  struct TagItem *tags __asm("a1"))
{
    ULONG *err = (ULONG *)GetTagData(VOA_Error, 0, tags);
    ULONG type = GetTagData(VOA_SrcType, SRCFMT_YCbCr16, tags);
    ULONG w = GetTagData(VOA_SrcWidth, 0, tags), h = GetTagData(VOA_SrcHeight, 0, tags);
    struct PBitMap *sp;
    struct VLayer *v;
    UBYTE fmt;
    ULONG e = VOERR_INVSCRMODE;

    dbg("cgxvideo: CreateVLayerHandle screen %lx type %lu %lux%lu\n", (ULONG)screen, type, w, h);
    if (!screen || !(sp = pbm_get(screen->RastPort.BitMap)))
        goto fail;
    e = VOERR_INVSRCFMT;
    switch (type) {
    case SRCFMT_YUV16:   fmt = PF_YUV_YUYV;  break;
    case SRCFMT_YCbCr16: fmt = PF_YUV_CGX;   break;
    case SRCFMT_RGB15:   fmt = PF_RGB555LE;  break;
    case SRCFMT_RGB16:   fmt = PF_RGB565LE;  break;
    default:             goto fail;
    }
    if (!w || !h || w > 2048 || h > 2048)
        goto fail;
    e = VOERR_NOMEMORY;
    if (!(v = AllocVec(sizeof(*v), MEMF_PUBLIC | MEMF_CLEAR)))
        goto fail;
    /* two YUV pixels share a longword: an odd width gets a spare column.
     * Where the board can show the layer itself (overlaySet, a YUV format
     * it takes, a 16- or 32-bit screen) the frames go into its VRAM; else,
     * or when VRAM is short, fast RAM and the compositor */
    v->source = NULL;
    /* (free VRAM only - a video layer never evicts other bitmaps; with
     * screen dragging on the board shows it too, except while the screen
     * is dragged down, when the compositor reads the frames back) */
    if (prism_hw_overlay() && sp->bd->overlaySet && (sp->bpp == 2 || sp->bpp == 4) &&
        PF_IS_YUV(fmt) && fmt != PF_YUV_YUYV)
        v->source = pbm_new(sp->card, (w + 1) & ~1, h, 8, fmt, NULL, 2, TRUE);
    if (!v->source && !(v->source = pbm_new(sp->card, (w + 1) & ~1, h, 8, fmt, NULL, FALSE, TRUE))) {
        FreeVec(v);
        goto fail;
    }
    v->source->locks = 1;                /* "being written": the compositor redoes it every tick */
    v->screen = screen;
    v->w = w;
    v->h = h;
    v->srcType = type;
    v->keyPen = -1;
    if (GetTagData(VOA_UseColorKey, FALSE, tags) && screen->ViewPort.ColorMap) {
        /* a pen for the player to paint its window with. A board that shows
         * the layer itself (the ZZ9000's overlay) shows the video wherever
         * the screen holds this colour, so it is an exclusive pen of an
         * unusual near-black (#080408, distinct after 15/16-bit rounding),
         * not the shared black every window draws with; the compositor
         * draws the layer over the area anyway */
        v->keyPen = ObtainPen(screen->ViewPort.ColorMap, -1, 0x08080808UL, 0x04040404UL, 0x08080808UL,
                              PEN_EXCLUSIVE);
        v->keyRGB = 0x080408;
        if (v->keyPen < 0) {             /* no free pen: black, and the board path stays off */
            v->keyPen = ObtainBestPenA(screen->ViewPort.ColorMap, 0, 0, 0, NULL);
            v->keyRGB = 0;
            v->sharedKey = TRUE;
        }
    }
    if (err) *err = VOERR_OK;
    return v;
fail:
    if (err) *err = e;
    return NULL;
}

static void V_DeleteVLayerHandle(struct VLayer *v __asm("a0"))
{
    if (!v)
        return;
    dbg("cgxvideo: DeleteVLayerHandle %lx\n", (ULONG)v);
    vl_detach(v);
    if (v->keyPen >= 0 && v->screen->ViewPort.ColorMap)
        ReleasePen(v->screen->ViewPort.ColorMap, v->keyPen);
    v->source->locks = 0;
    pbm_free(v->source);
    FreeVec(v);
}

static void vl_indents(struct VLayer *v, struct TagItem *tags)
{
    v->indent[0] = GetTagData(VOA_LeftIndent, v->indent[0], tags);
    v->indent[1] = GetTagData(VOA_TopIndent, v->indent[1], tags);
    v->indent[2] = GetTagData(VOA_RightIndent, v->indent[2], tags);
    v->indent[3] = GetTagData(VOA_BottomIndent, v->indent[3], tags);
}

static ULONG V_AttachVLayerTagList(struct VLayer *v __asm("a0"), struct Window *w __asm("a1"),
                                   struct TagItem *tags __asm("a2"))
{
    if (!v || !w)
        return 1;
    dbg("cgxvideo: AttachVLayer %lx to window %lx\n", (ULONG)v, (ULONG)w);
    vl_detach(v);
    memset(v->indent, 0, sizeof(v->indent));
    vl_indents(v, tags);
    if (!(v->pip = p96_pip_attach(w, v->source, v->w, v->h, v->indent[0], v->indent[1], v->indent[2], v->indent[3],
                                  v->sharedKey ? -2 : v->keyPen)))
        return 1;
    v->window = w;
    return 0;
}

static ULONG V_DetachVLayer(struct VLayer *v __asm("a0"))
{
    if (!v)
        return 1;
    dbg("cgxvideo: DetachVLayer %lx\n", (ULONG)v);
    vl_detach(v);
    return 0;
}

static ULONG V_GetVLayerAttr(struct VLayer *v __asm("a0"), ULONG attr __asm("d0"))
{
    if (!v)
        return 0;
    switch (attr) {
    case VOA_BaseAddress:
    case VOA_FrameBase0:
    case VOA_FrameBase1:  return (ULONG)v->source->pix;
    case VOA_BaseOffset:  return 0;
    case VOA_Modulo:      return v->source->bpr;
    case VOA_Width:       return v->w;
    case VOA_Height:      return v->h;
    case VOA_SrcType:
    case VOA_FrameType:   return v->srcType;
    case VOA_ColorKeyPen: return (ULONG)v->keyPen;
    case VOA_ColorKey:    return v->keyPen >= 0 ? v->keyRGB : (ULONG)-1;
    case VOA_Identifier:  return 0;
    }
    return 0;
}

static ULONG V_LockVLayer(struct VLayer *v __asm("a0"))
{
    return v != NULL;                    /* fast RAM: nothing moves      */
}

static void V_UnlockVLayer(struct VLayer *v __asm("a0"))
{
    if (v)
        v->source->modified++;           /* a new frame (the layer is composed every tick anyway) */
}

static void V_SetVLayerAttrTagList(struct VLayer *v __asm("a0"), struct TagItem *tags __asm("a1"))
{
    if (!v)
        return;
    vl_indents(v, tags);
    if (v->pip)
        p96_pip_indent(v->pip, v->indent[0], v->indent[1], v->indent[2], v->indent[3]);
}

static ULONG V_SwapVLayerBuffer(struct VLayer *v __asm("a0"))
{
    return 0;                            /* one buffer                   */
}

/* QueryVLayerAttr (V50): what the "hardware" can do, into ULONGs the tags point at */
static ULONG V_QueryVLayerAttr(struct TagItem *tags __asm("a0"))
{
    struct TagItem *ti, *tl = tags;
    ULONG n = 0;
    while ((ti = NextTagItem(&tl))) {
        ULONG *out = (ULONG *)ti->ti_Data;
        if (!out) continue;
        switch (ti->ti_Tag) {
        case VSQ_SupportedFeatures: *out = VSQ_FEAT_OVERLAY | VSQ_FEAT_COLORKEYING; n++; break;
        case VSQ_SupportedFormats:  *out = VSQ_FMT_YUYV | VSQ_FMT_R5G5B5_LE | VSQ_FMT_R5G6B5_LE; n++; break;
        case VSQ_MaxWidth:          *out = 2048; n++; break;
        }
    }
    return n;
}

static APTR funcs[] = {
    (APTR)L_Open, (APTR)L_Close, (APTR)L_Expunge, (APTR)L_Null,
    (APTR)V_CreateVLayerHandleTagList,                                /* -30 */
    (APTR)V_DeleteVLayerHandle,                                       /* -36 */
    (APTR)V_AttachVLayerTagList,                                      /* -42 */
    (APTR)V_DetachVLayer,                                             /* -48 */
    (APTR)V_GetVLayerAttr,                                            /* -54 */
    (APTR)V_LockVLayer,                                               /* -60 */
    (APTR)V_UnlockVLayer,                                             /* -66 */
    (APTR)V_SetVLayerAttrTagList,                                     /* -72 */
    (APTR)L_Null, (APTR)L_Null, (APTR)L_Null,                         /* -78..-90 reserved */
    (APTR)V_SwapVLayerBuffer,                                         /* -96 */
    (APTR)L_Null,                                                     /* -102 WriteSPLine */
    (APTR)V_QueryVLayerAttr,                                          /* -108 */
    (APTR)-1
};

BOOL cgxvideo_init(void)
{
    struct Library *lib;

    Forbid();
    lib = (struct Library *)FindName(&SysBase->LibList, libName);
    Permit();
    if (lib) {
        dbg("cgxvideo: a cgxvideo.library is already running - not adding ours\n");
        return FALSE;
    }
    if (!(vidBase = MakeLibrary(funcs, NULL, NULL, sizeof(struct Library), 0)))
        return FALSE;
    vidBase->lib_Node.ln_Type = NT_LIBRARY;
    vidBase->lib_Node.ln_Name = libName;
    vidBase->lib_Flags = LIBF_SUMUSED | LIBF_CHANGED;
    vidBase->lib_Version = 42;
    vidBase->lib_Revision = 1;
    vidBase->lib_IdString = libId;
    AddLibrary(vidBase);
    return TRUE;
}

BOOL cgxvideo_remove(void)
{
    if (!vidBase)
        return TRUE;
    Forbid();
    if (vidBase->lib_OpenCnt) {
        Permit();
        return FALSE;
    }
    Remove(&vidBase->lib_Node);
    Permit();
    FreeMem((UBYTE *)vidBase - vidBase->lib_NegSize, vidBase->lib_NegSize + vidBase->lib_PosSize);
    vidBase = NULL;
    return TRUE;
}
