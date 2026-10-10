/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laine Jones */
/*
 * Bobs on PrismRTG screens (DrawGList).
 *
 * graphics.library draws GELs straight into a bitmap's planes with the
 * blitter. A PrismRTG bitmap's Planes[] point at a hidden placeholder, so
 * Workbench's dragged icons (Bobs, drawn with DrawGList) went there and
 * nothing moved on the screen (issue #1, 2026-10-09). Here a Bob on one
 * of our bitmaps goes through the ordinary drawing calls instead, which
 * PrismRTG handles: its background is saved with ClipBlit, its image is
 * drawn with BltMaskBitMapRastPort (BltBitMapRastPort without OVERLAY),
 * and the background is put back at the next DrawGList, as the ROM does.
 * Sprites (VSprites) can't be shown on an RTG screen and are left alone.
 */
#include <exec/memory.h>
#include <graphics/gels.h>
#include <graphics/rastport.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include "prismint.h"

#define SAVES 16                     /* Bobs on screen at once (Workbench: a few) */

/* A Bob's saved background: where it came from, and a bitmap of the
 * screen's kind holding it (kept between calls, grown when needed) */
struct Saved {
    struct RastPort *rp;             /* NULL: free slot                  */
    WORD x, y, w, h;
    BOOL valid;                      /* holds a background to put back   */
    struct BitMap *bm;
    UWORD bmW, bmH;
    struct RastPort srp;
};
static struct Saved saved[SAVES];
static UBYTE savedN;                 /* slots in use, in drawing order   */
static struct SignalSemaphore gelLock;
static BOOL gelReady;

void gels_init(void)
{
    InitSemaphore(&gelLock);
    gelReady = TRUE;
}

/* PrismD is quitting: drop the saved backgrounds. */
void gels_quit(void)
{
    UBYTE i;
    if (!gelReady)
        return;
    ObtainSemaphore(&gelLock);
    for (i = 0; i < SAVES; i++) {
        if (saved[i].bm)
            FreeBitMap(saved[i].bm);
        saved[i].bm = NULL;
        saved[i].rp = NULL;
    }
    savedN = 0;
    ReleaseSemaphore(&gelLock);
}

/* Room for a w x h background in slot s, like rp's bitmap. */
static BOOL save_room(struct Saved *s, struct RastPort *rp, WORD w, WORD h)
{
    if (s->bm && s->bmW >= w && s->bmH >= h)
        return TRUE;
    if (s->bm)
        FreeBitMap(s->bm);
    s->bm = AllocBitMap(w, h, GetBitMapAttr(rp->BitMap, BMA_DEPTH), BMF_MINPLANES, rp->BitMap);
    if (!s->bm)
        return FALSE;
    s->bmW = w;
    s->bmH = h;
    InitRastPort(&s->srp);
    s->srp.BitMap = s->bm;
    return TRUE;
}

/* The Bob's image as a planar bitmap: PlanePick says which planes the
 * image's planes go to, PlaneOnOff what the others are (a NULL plane reads
 * as all zeros, -1 as all ones). */
static void bob_bitmap(const struct VSprite *vs, struct BitMap *bm)
{
    UWORD plane = (vs->Width * 2) * vs->Height / 2;   /* words per image plane */
    WORD *img = vs->ImageData;
    UBYTE i;
    InitBitMap(bm, 8, vs->Width * 16, vs->Height);
    for (i = 0; i < 8; i++) {
        if (vs->PlanePick & (1 << i)) {
            bm->Planes[i] = (PLANEPTR)img;
            img += plane;
        } else
            bm->Planes[i] = (vs->PlaneOnOff & (1 << i)) ? (PLANEPTR)-1 : NULL;
    }
}

/* The blits go straight to PrismRTG's own handlers (render.c), not
 * through graphics.library: a second trip through the patch stub would
 * put the deepest blit chain past the stack budget on the caller's stack
 * (tests/stack_audit.sh). Static, like the image bitmap: all under
 * gelLock. Registers as the library takes them. */
LONG h_ClipBlit(struct Regs *r);
LONG h_BltBitMapRastPort(struct Regs *r);
LONG h_BltMaskBitMapRastPort(struct Regs *r);
static struct Regs gr;
static struct BitMap img;

/* ClipBlit(srcRP a0, xSrc d0, ySrc d1, destRP a1, xDest d2, yDest d3, xSize d4, ySize d5, minterm d6) */
static inline void clip_blit(struct RastPort *s, WORD sx, WORD sy, struct RastPort *d, WORD dx, WORD dy,
                      WORD w, WORD h)
{
    gr.a[0] = (LONG)s; gr.d[0] = sx; gr.d[1] = sy;
    gr.a[1] = (LONG)d; gr.d[2] = dx; gr.d[3] = dy;
    gr.d[4] = w; gr.d[5] = h; gr.d[6] = 0xc0;
    if (!h_ClipBlit(&gr))
        ClipBlit(s, sx, sy, d, dx, dy, w, h, 0xc0);
}

/* Bob image into rp: Blt(Mask)BitMapRastPort(srcBM a0, d0, d1, destRP a1, d2..d6[, mask a2]) */
static inline void bob_draw(struct RastPort *rp, WORD x, WORD y, WORD w, WORD h, PLANEPTR mask)
{
    gr.a[0] = (LONG)&img; gr.d[0] = 0; gr.d[1] = 0;
    gr.a[1] = (LONG)rp; gr.d[2] = x; gr.d[3] = y;
    gr.d[4] = w; gr.d[5] = h;
    if (mask) {
        gr.d[6] = 0xe0;
        gr.a[2] = (LONG)mask;
        if (!h_BltMaskBitMapRastPort(&gr))
            BltMaskBitMapRastPort(&img, 0, 0, rp, x, y, w, h, 0xe0, mask);
    } else {
        gr.d[6] = 0xc0;
        if (!h_BltBitMapRastPort(&gr))
            BltBitMapRastPort(&img, 0, 0, rp, x, y, w, h, 0xc0);
    }
}

/* DrawGList(rp a1, vp a0) */
LONG h_DrawGList(struct Regs *r)
{
    struct RastPort *rp = (struct RastPort *)r->a[1];
    struct GelsInfo *gi;
    struct VSprite *vs, *next;
    WORD i;

    if (!rp || !rp->BitMap || !pbm_get(rp->BitMap) || !gelReady)
        return 0;                    /* not ours: the ROM's GELs        */
    if (!(gi = rp->GelsInfo) || !gi->gelHead)
        return 1;
    ObtainSemaphore(&gelLock);

    /* Take the Bobs off: backgrounds back, last drawn first. */
    for (i = savedN - 1; i >= 0; i--)
        if (saved[i].rp == rp && saved[i].valid) {
            clip_blit(&saved[i].srp, 0, 0, rp, saved[i].x, saved[i].y, saved[i].w, saved[i].h);
            saved[i].valid = FALSE;
        }
    /* free this rp's slots for the new frame (the bitmaps stay for reuse) */
    for (i = 0; i < savedN; i++)
        if (saved[i].rp == rp)
            saved[i].rp = NULL;

    for (vs = gi->gelHead->NextVSprite; vs && vs != gi->gelTail; vs = next) {
        struct Bob *b = vs->VSBob;
        WORD w = vs->Width * 16, h = vs->Height;
        next = vs->NextVSprite;
        if ((vs->Flags & VSPRITE) || !b || w <= 0 || h <= 0 || !vs->ImageData)
            continue;
        if (b->Flags & BOBSAWAY) {
            /* RemBob: off the list, and the program told it is gone */
            vs->PrevVSprite->NextVSprite = vs->NextVSprite;
            vs->NextVSprite->PrevVSprite = vs->PrevVSprite;
            b->Flags = (b->Flags & ~(BOBSAWAY | BDRAWN)) | BOBNIX;
            continue;
        }
        if (vs->Flags & SAVEBACK) {
            struct Saved *s = NULL;
            for (i = 0; i < SAVES; i++)
                if (!saved[i].rp) { s = &saved[i]; break; }
            if (s && save_room(s, rp, w, h)) {
                s->rp = rp;
                s->x = vs->X; s->y = vs->Y; s->w = w; s->h = h;
                clip_blit(rp, vs->X, vs->Y, &s->srp, 0, 0, w, h);
                s->valid = TRUE;
                if (s - saved >= savedN)
                    savedN = s - saved + 1;
                vs->Flags |= BACKSAVED;
            }
        }
        bob_bitmap(vs, &img);
        bob_draw(rp, vs->X, vs->Y, w, h,
                 (vs->Flags & OVERLAY) && b->ImageShadow ? (PLANEPTR)b->ImageShadow : NULL);
        vs->OldX = vs->X;
        vs->OldY = vs->Y;
        b->Flags |= BDRAWN;
    }
    ReleaseSemaphore(&gelLock);
    return 1;
}
