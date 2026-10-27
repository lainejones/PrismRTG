/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Stefan Reinauer
 * Copyright (C) 2026 Laine Jones */
/* The compositor: screen dragging and picture-in-picture. A frame of the
 * card's mode is composed from the front screen at its dragged position,
 * the screen behind it, their PIPs and the pointer, and shown instead of
 * the front screen's own bitmap.
 *
 * It does as little as it can per tick:
 *  - nothing at all while nothing changed: the bitmaps' modification
 *    stamps (render.c stamps a bitmap every time it draws into it), the
 *    screen positions, the pointer, the palettes and the PIPs are
 *    compared with the last composed frame. A bitmap a program writes
 *    into through a retained pointer (a LockBitMap, a pixel-format
 *    bitmap) counts as changed every tick, and every 25th tick the whole
 *    frame is redone as a safety net;
 *  - only the rows that depend on what changed are composed again: the
 *    screen behind when it changed, the front's band when it did, the
 *    rows under the pointer when it moved; when the front only moved
 *    (dragging), its band and the rows it uncovered - rows of the screen
 *    behind that were already showing it stay as they are;
 *  - the front's band is copied by the card's blitter from its own VRAM
 *    into the frame's, never through the CPU, where the board has
 *    copyBetween and both are in VRAM in the same format;
 *  - only the rows the CPU composed are compared with the last upload
 *    and written back. */
#include <exec/memory.h>
#include <intuition/screens.h>
#include <proto/exec.h>
#include <string.h>
#include "prismint.h"
#include "composite.h"
#include "damage.h"

/* per row: how it was made, this tick and the last uploaded one */
#define ROW_CPU   0              /* composed in frame.pix, uploaded by changed span */
#define ROW_HW    1              /* the front's band, copied by the blitter          */
#define ROW_HWCPU 2              /* in the band, but with a PIP or the pointer on it:
                                    read back, composed, written in full            */
#define SAFETY_TICKS 25          /* a full frame every half second regardless */

/* One compositor per card: each board composes its own front screen
 * (multi-board.md). Everything below works on P, the card's, set by the
 * entry points (all under the lock). */
static struct Pres {
    struct PBitMap x_frame;
    UBYTE *x_previous;
    BOOL x_allocated, x_valid, x_owned;
    struct PBitMap *x_input;
    struct PBitMap *x_failedInput;
    UWORD x_failedWidth, x_failedHeight, x_retryTicks;
    UBYTE *x_rowMode, *x_prevMode, *x_rowDirty;
    /* what the last composed frame was made from */
    ULONG x_seenFrontMod, x_seenBackMod, x_seenPal, x_seenPip;
    WORD x_seenTop, x_seenBackTop, x_seenPX, x_seenPY, x_seenOv0, x_seenOv1, x_seenBand0, x_seenBand1;
    WORD x_seenAny0, x_seenAny1;           /* rows the pointer or a PIP covered */
    const void *x_seenScreen, *x_seenFront, *x_seenBack;
    BOOL x_seenHwBand;
    UWORD x_idleTicks;
    /* the drag shown by the card's split instead (split_try) */
    struct PrismBoard *x_splitBd;
    struct PBitMap *x_splitFailed;         /* this drag's front couldn't be laid out */
    /* the screen behind's pens in the frame's palette (8-bit), per card:
     * two boards would otherwise rebuild it for each other every tick */
    UBYTE backMap[256];
    ULONG mapPal;
    const struct PBitMap *mapFor;
} pres[MAX_CARDS], *P = &pres[0];
static struct PrismCard *PC = &cards[0];
#define frame        (P->x_frame)
#define previous     (P->x_previous)
#define allocated    (P->x_allocated)
#define valid        (P->x_valid)
#define owned        (P->x_owned)
#define input        (P->x_input)
#define failedInput  (P->x_failedInput)
#define failedWidth  (P->x_failedWidth)
#define failedHeight (P->x_failedHeight)
#define retryTicks   (P->x_retryTicks)
#define rowMode      (P->x_rowMode)
#define prevMode     (P->x_prevMode)
#define rowDirty     (P->x_rowDirty)
#define seenFrontMod (P->x_seenFrontMod)
#define seenBackMod  (P->x_seenBackMod)
#define seenPal      (P->x_seenPal)
#define seenPip      (P->x_seenPip)
#define seenTop      (P->x_seenTop)
#define seenBackTop  (P->x_seenBackTop)
#define seenPX       (P->x_seenPX)
#define seenPY       (P->x_seenPY)
#define seenOv0      (P->x_seenOv0)
#define seenOv1      (P->x_seenOv1)
#define seenBand0    (P->x_seenBand0)
#define seenBand1    (P->x_seenBand1)
#define seenAny0     (P->x_seenAny0)
#define seenAny1     (P->x_seenAny1)
#define seenScreen   (P->x_seenScreen)
#define seenFront    (P->x_seenFront)
#define seenBack     (P->x_seenBack)
#define seenHwBand   (P->x_seenHwBand)
#define idleTicks    (P->x_idleTicks)
#define splitBd      (P->x_splitBd)

static void on_card(struct PrismCard *c)
{
    PC = c;
    P = &pres[c->index];
}

void present_invalidate(struct PrismCard *c)
{
    pres[c->index].x_valid = FALSE;
}

BOOL present_ready(struct PrismCard *c)
{
    return pres[c->index].x_allocated && pres[c->index].x_valid;
}

/* something is shown composed or split on any card (PrismD ticks faster) */
BOOL present_active(void)
{
    UBYTE k;
    for (k = 0; k < MAX_CARDS; k++)
        if (pres[k].x_allocated || pres[k].x_splitBd) return TRUE;
    return FALSE;
}

/* the card shows a drag by its split: the front at its TopEdge, as in a
 * composed frame (the pointer goes at display coordinates) */
BOOL present_split(struct PrismCard *c)
{
    return pres[c->index].x_splitBd != NULL;
}

/* Caller holds the board lock. Switch away before releasing scanout VRAM. */
void present_stop(struct PrismCard *c)
{
    struct PBitMap *front;
    on_card(c);
    front=prism_display_bitmap(c);
    retryTicks=0;failedInput=NULL;
    if (!allocated) return;
    if (front && front->inVram) frame.bd->setDisplayStart(frame.bd,front->vramOff);
    frame.bd->flags &= ~PBF_PRESENT;
    if (!owned && input && input->inVram) {
        input->uploadValid=FALSE;pbm_upload(input);
    }
    if (owned) vram_free(frame.card, frame.vramOff);
    FreeVec(frame.pix); FreeVec(previous); FreeVec(rowMode); FreeVec(prevMode); FreeVec(rowDirty);
    memset(&frame,0,sizeof(frame));previous=NULL;rowMode=prevMode=rowDirty=NULL;
    allocated=valid=owned=FALSE;input=NULL;
}

static BOOL prepare(struct PBitMap *front,UWORD width,UWORD height)
{
    ULONG size,pitch; LONG off;
    if (allocated && frame.w==width && frame.h==height && frame.fmt==front->fmt && input==front) return TRUE;
    if (front==failedInput && width==failedWidth && height==failedHeight &&
        retryTicks) { retryTicks--;return FALSE; }
    present_stop(PC);
    pitch=front->bd->ops && front->bd->ops->pitch ? front->bd->ops->pitch(front->bd,width,height,front->fmt) :
        front->bd->bytesPerRow ? front->bd->bytesPerRow(front->bd,width,height,front->bpp) :
        (((ULONG)width*front->bpp+7)&~7UL);
    if (!pitch || pitch<width*front->bpp || pitch>0x7fffffffUL/height) return FALSE;
    if ((front->bd->flags & PBF_SHADOW) && front->inVram && front->w>=width && front->h>=height)
        pitch=front->bpr;
    size=pitch*height;
    frame.pix=AllocVec(size,MEMF_PUBLIC|MEMF_CLEAR);
    previous=AllocVec(size,MEMF_ANY);
    rowMode=AllocVec(height,MEMF_ANY|MEMF_CLEAR);
    prevMode=AllocVec(height,MEMF_ANY);
    rowDirty=AllocVec(height,MEMF_ANY);
    if (!frame.pix || !previous || !rowMode || !prevMode || !rowDirty) goto fail;
    memset(prevMode,ROW_HW,height);          /* nothing uploaded yet: no row comparable */
    owned=!(front->bd->flags & PBF_SHADOW) || !front->inVram || front->w<width || front->h<height;
    /* hidden screens move to fast RAM to make room (a 2 MB card holds
     * the shown screen, its composed copy and little else) */
    off=owned ? vram_get_size(front->card,size,front->fmt,pitch,width,height,front) : (LONG)front->vramOff;
    if (off<0) goto fail;
    frame.w=width;frame.h=height;frame.bpr=pitch;frame.bpp=front->bpp;
    frame.fmt=front->fmt;frame.vramOff=off;frame.inVram=TRUE;
    frame.card=front->card;frame.bd=front->bd;pbm_fits(&frame);
    allocated=TRUE;input=front;valid=FALSE;
    if (!owned) {
        front->bd->flags|=PBF_PRESENT;front->uploadValid=FALSE;
        if(front->uploaded) { FreeVec(front->uploaded);front->uploaded=NULL; }
    }
    return TRUE;
fail:
    if(frame.pix)FreeVec(frame.pix);
    if(previous)FreeVec(previous);
    if(rowMode)FreeVec(rowMode);
    if(prevMode)FreeVec(prevMode);
    if(rowDirty)FreeVec(rowDirty);
    memset(&frame,0,sizeof(frame));previous=NULL;rowMode=prevMode=rowDirty=NULL;
    failedInput=front;failedWidth=width;failedHeight=height;retryTicks=50;
    return FALSE;
}

/* Reserve visible-window storage before returning a successful PIP open.
 * Hidden windows acquire scanout when their screen reaches the front. */
BOOL present_reserve(struct Screen *screen)
{
    struct PBitMap *front,*back;WORD top,backTop;UWORD width,height;
    UBYTE k;
    for (k = 0; k < prism_ncards(); k++)
        if (screen==prism_display_layers(&cards[k],&front,&back,&top,&backTop,&width,&height)) {
            on_card(&cards[k]);
            retryTicks=0; /* An explicit new PIP request may retry immediately. */
            return prepare(front,width,height);
        }
    return TRUE;
}

/* A bitmap whose pixels a program can change without PrismD seeing a
 * call: composed every tick. */
static BOOL volatile_bitmap(const struct PBitMap *p)
{
    return p && (p->locks || p->direct);
}

/* The front's band, VRAM to VRAM on the blitter (the caller checked it
 * can be done). Rows y0..y1-1 of the frame from the matching front rows. */
static void band_blit(const struct PBitMap *front,WORD y0,WORD y1,WORD top)
{
    UWORD wbytes=(UWORD)(frame.w*frame.bpp);
    WORD sy0=y0-top;
    while (y0<y1) {
        UWORD n=y1-y0;
        if (frame.bd->blitMaxRows && n>frame.bd->blitMaxRows) n=frame.bd->blitMaxRows;
        frame.bd->copyBetween(frame.bd,front->vramOff+(ULONG)sy0*front->bpr,front->bpr,
                          frame.vramOff+(ULONG)y0*frame.bpr,frame.bpr,wbytes,n);
        y0+=n;sy0+=n;
    }
    if (frame.bd->waitBlit) frame.bd->waitBlit(frame.bd);
}

static inline WORD clampw(LONG v,WORD lo,WORD hi) { return v<lo ? lo : v>hi ? hi : (WORD)v; }

/* A drag shown by the card itself (PrismBoard.setSplit): the lines above
 * the front screen scanned out of the screen behind's own bitmap, the
 * rest out of the front's - nothing composed or copied, a step costs a
 * few register writes. Only when both screens lie in VRAM the way one mode can
 * show them, the screen behind at row 0, and nothing is drawn over them
 * (a PIP, the software pointer); otherwise FALSE and the frame is composed.
 * Leaving the split puts the display back on the front. Lock held. */
static BOOL split_try(struct Screen *screen,struct PBitMap *front,struct PBitMap *back,
                      WORD top,WORD backTop,UWORD width,UWORD height)
{
    struct PrismBoard *bd=front ? front->bd : NULL;
    struct Screen *bs=screen ? prism_back_screen(PC,screen) : NULL;
    WORD py0,py1;
    /* everything but where the two bitmaps lie */
    BOOL ok = screen && bd && bd->setSplit && top>0 && top<height &&
        front->inVram && front->w>=width && front->h>=height && !(bd->flags & PBF_SHADOW) &&
        back && back->bd==bd && back->fmt==front->fmt && back->bpr==front->bpr &&
        backTop==0 && back->h>=top && bs &&
        bs->Width==screen->Width && bs->Height==screen->Height &&
        !p96_pip_active(screen) && !p96_pip_active(bs) &&
        !pointer_compose_rows(PC,top,&py0,&py1);
    if (top<=0) P->x_splitFailed=NULL;            /* a new drag tries again */
    /* the screen behind into VRAM if it was paged out - once per drag: a
     * refusal (a locked bitmap) leaves the drag to the compositor */
    if (ok && !back->inVram && P->x_splitFailed!=front) {
        if (allocated) present_stop(PC);
        if (!pbm_to_vram(back)) P->x_splitFailed=front;
    }
    if (ok && back->inVram) {
        if (allocated) present_stop(PC);          /* the split shows the screens' own pixels */
        if (bd->setSplit(bd,back->vramOff,front->vramOff,(UWORD)top)) {
            splitBd=bd; return TRUE;
        }
        /* a board that starts the lower part at VRAM 0 only (a VGA line
         * compare): the front moves there on the drag's first step (a screen
         * opened after Workbench lies further up), once per drag */
        if (front->vramOff && P->x_splitFailed!=front) {
            if (pbm_to_vram0(front) && (back->inVram || pbm_to_vram(back)) && back->vramOff &&
                bd->setSplit(bd,back->vramOff,0,(UWORD)top)) {
                splitBd=bd; return TRUE;
            }
            P->x_splitFailed=front;
            if (!front->vramOff) bd->setDisplayStart(bd,0);   /* moved, but no split */
        }
    }
    if (splitBd) {
        splitBd->setSplit(splitBd,0,0,0);
        if (front && front->inVram && front->bd==splitBd) splitBd->setDisplayStart(splitBd,front->vramOff);
        splitBd=NULL;
    }
    return FALSE;
}

static void present_card(void)
{
    struct PBitMap *front,*back;
    struct Screen *screen,*bs=NULL;
    struct PrismSurface surface;
    UWORD width,height,y; WORD top,backTop;
    BOOL showBack,hwBand,full,moved,frontChanged,backChanged,ptrChanged;
    const UBYTE *map=NULL;
    WORD bandY0=0,bandY1=0,ovY0=0,ovY1=0,anyY0=0,anyY1=0,pipY0,pipY1,ptrY0,ptrY1;
    /* the pointer's position once per compose (input.device moves it
     * without the lock): what was drawn is what is remembered */
    WORD px=posX,py=posY;
    BOOL ptrHere=pointer_is_card(PC);
    ULONG wrow;
    WORD rx,ry;
    ObtainSemaphore(&lock);
    screen=prism_display_layers(PC,&front,&back,&top,&backTop,&width,&height);
    /* an oversized (autoscroll) screen is not composed: the card steps its
     * rows by the big bitmap's pitch, and the frame has the mode's. PIPs
     * are refused on one (p96.c) and dragging leaves it where it is. */
    if (front && prism_display_scroll(PC,&rx,&ry)) {
        split_try(screen,front,back,0,backTop,width,height);   /* leaves a split */
        present_stop(PC); ReleaseSemaphore(&lock); return;
    }
    if (!prism_dragging()) top=0;
    if (split_try(screen,front,back,top,backTop,width,height)) {
        ReleaseSemaphore(&lock); return;
    }
    /* The pointer alone is a software sprite (pointer.c), not a reason
     * to compose; while composing, pointer_compose draws it. */
    if (!screen || (!top && !p96_pip_active(screen))) {
        present_stop(PC); ReleaseSemaphore(&lock); return;
    }
    if (!prepare(front,width,height)) { ReleaseSemaphore(&lock);return; }

    bs=prism_back_screen(PC,screen);
    frame.rgbTab=front->rgbTab;
    showBack=back && (top>0 || (LONG)top+front->h<height);
    /* The card shows one mode: a screen behind of another size would come
     * out cut off or magnified in the front's (issue #1). It stays black
     * until it comes to the front; one of the same size shows through
     * (another depth converted, which looks right). */
    if (showBack && bs &&
        (bs->Width!=screen->Width || bs->Height!=screen->Height))
        showBack=FALSE;
    /* the front's band: rows of the frame it covers */
    if (top<height && (LONG)top+front->h>0) {
        bandY0=top<0 ? 0 : top;
        bandY1=(LONG)top+front->h>height ? height : top+front->h;
    }
    hwBand = bandY1>bandY0 && owned && front->inVram && front->w>=width &&
        front->fmt==frame.fmt && frame.bd->copyBetween &&
        !(frame.bd->flags & (PBF_SHADOW|PBF_SOFTWARE|PBF_ACCEL_BROKEN|PBF_PRESENT)) &&
        hw_fits(frame.bd,(UWORD)(width*frame.bpp),1,front->bpr>frame.bpr ? front->bpr : frame.bpr);
    /* rows anything is drawn on top of: the pointer and PIPs (anywhere in
     * the frame - over the screen behind too); of those, the rows in the
     * band are read back from the front and composed by the CPU */
    if (p96_pip_rows(screen,top,&pipY0,&pipY1)) { anyY0=pipY0;anyY1=pipY1; }
    if (pointer_compose_rows(PC,top,&ptrY0,&ptrY1)) {
        if (anyY1<=anyY0) { anyY0=ptrY0;anyY1=ptrY1; }
        else { if (ptrY0<anyY0) anyY0=ptrY0; if (ptrY1>anyY1) anyY1=ptrY1; }
    }
    anyY0=clampw(anyY0,0,height);anyY1=clampw(anyY1,0,height);
    if (hwBand) { ovY0=clampw(anyY0,bandY0,bandY1);ovY1=clampw(anyY1,bandY0,bandY1); }

    /* what changed since the last frame? */
    full = !valid || screen!=seenScreen || front!=seenFront || back!=seenBack ||
        backTop!=seenBackTop || paletteGen!=seenPal || pipGen!=seenPip ||
        hwBand!=seenHwBand || ++idleTicks>=SAFETY_TICKS;
    /* the front moved (a drag step): its band is redone, and of the rest
     * only the rows that were in the band before (now uncovered) */
    moved = top!=seenTop;
    frontChanged = full || moved || front->modified!=seenFrontMod || volatile_bitmap(front) ||
        p96_pip_volatile(screen);
    backChanged = full || (back && (back->modified!=seenBackMod || volatile_bitmap(back))) ||
        (back && bs && p96_pip_volatile(bs));
    ptrChanged = full || (ptrHere && (px!=seenPX || py!=seenPY)) || ovY0!=seenOv0 || ovY1!=seenOv1 ||
        anyY0!=seenAny0 || anyY1!=seenAny1;
    if (!frontChanged && !backChanged && !ptrChanged) { ReleaseSemaphore(&lock); return; }
    if (full) idleTicks=0;

    if(showBack && back->fmt==PF_CLUT8 && frame.fmt==PF_CLUT8 &&
       back->rgbTab!=frame.rgbTab) {
        /* 65,536 nearest-pen searches: once per palette, not per tick */
        if (P->mapFor!=back || P->mapPal!=paletteGen) {
            composite_clut_map(&frame,back,P->backMap);P->mapFor=back;P->mapPal=paletteGen;
        }
        map=P->backMap;
    }
    if (frame.bd->waitBlit) frame.bd->waitBlit(frame.bd);

    wrow=(ULONG)(front->w<width ? front->w : width)*frame.bpp;
    for (y=0;y<height;y++) {
        LONG by=(LONG)y-backTop,sy=(LONG)y-top;
        UBYTE *row=frame.pix+(ULONG)y*frame.bpr;
        BOOL inBand=sy>=0 && sy<front->h, redo;
        rowDirty[y]=FALSE;
        if (hwBand && inBand) {
            BOOL overlay=y>=ovY0 && y<ovY1, wasOverlay=y>=seenOv0 && y<seenOv1;
            rowMode[y]=overlay ? ROW_HWCPU : ROW_HW;
            /* a row the pointer or a PIP just left goes back to plain
             * front pixels: the band blit below restores it */
            redo = frontChanged || (ptrChanged && (overlay || wasOverlay));
            if (!redo) continue;
            rowDirty[y]=TRUE;
            if (overlay) {                /* the front's pixels, then the overlays */
                CopyMem(front->pix+(ULONG)sy*front->bpr,row,wrow);
                if (wrow<frame.bpr) memset(row+wrow,0,frame.bpr-wrow);
            }
            continue;
        }
        rowMode[y]=ROW_CPU;
        if (inBand) {
            /* the pointer moving redoes only the rows it was or is on */
            redo = frontChanged ||
                (ptrChanged && ((y>=anyY0 && y<anyY1) || (y>=seenAny0 && y<seenAny1)));
            if (!redo) continue;
            rowDirty[y]=TRUE;
            if (wrow<frame.bpr) memset(row+wrow,0,frame.bpr-wrow);
            composite_row(&frame,y,front,sy);
            continue;
        }
        redo = backChanged || (moved && y>=seenBand0 && y<seenBand1) ||
            (ptrChanged && ((y>=anyY0 && y<anyY1) || (y>=seenAny0 && y<seenAny1)));
        if (!redo) continue;
        rowDirty[y]=TRUE;
        memset(row,0,frame.bpr);
        if (showBack && by>=0 && by<back->h)
            composite_row_mapped(&frame,y,back,by,map);
    }
    /* after a drag step too (the rows it uncovered were redone from the
     * screen behind's bitmap), and under the pointer; never on the front's
     * band, which is on top of it */
    if(showBack && bs && (backChanged || moved || ptrChanged))
        p96_pip_compose(&frame,bs,backTop,bandY0,bandY1);
    if (hwBand) {
        if (frontChanged) {
            /* not the overlay rows: those are composed on the CPU and
             * uploaded below, and a blit over them would show the front
             * for the rest of the pass (a video layer flickered black) */
            if (ovY1>ovY0) { band_blit(front,bandY0,ovY0,top); band_blit(front,ovY1,bandY1,top); }
            else band_blit(front,bandY0,bandY1,top);
        } else if (ptrChanged && seenOv1>seenOv0)
            band_blit(front,clampw(seenOv0,bandY0,bandY1),clampw(seenOv1,bandY0,bandY1),top);
    }
    p96_pip_compose(&frame,screen,top,0,0);
    pointer_compose(&frame,top);
    pbm_surface(&frame,&surface);
    for (y=0;y<height;y++) {
        ULONG first=0,count=frame.bpr,off=(ULONG)y*frame.bpr;
        UBYTE mode=rowMode[y];
        if (mode==ROW_HW) { if (rowDirty[y]) prevMode[y]=ROW_HW; continue; }
        if (!rowDirty[y]) continue;
        /* a row the blitter wrote last time holds nothing comparable */
        if (valid && prevMode[y]!=ROW_HW && mode==ROW_CPU &&
            !changed_span(frame.pix+off,previous+off,frame.bpr,&first,&count)) continue;
        off+=first;
        if (frame.bd->ops && frame.bd->ops->write) {
            if (!frame.bd->ops->write(frame.bd,&surface,off,frame.pix+off,count)) break;
        } else CopyMem(frame.pix+off,frame.bd->vram+frame.vramOff+off,count);
        CopyMem(frame.pix+off,previous+off,count);
        prevMode[y]=mode;
    }
    if (y==height) {
        valid=TRUE;frame.bd->setDisplayStart(frame.bd,frame.vramOff);
        seenScreen=screen;seenFront=front;seenBack=back;seenTop=top;seenBackTop=backTop;
        seenPX=px;seenPY=py;seenPal=paletteGen;seenPip=pipGen;seenHwBand=hwBand;
        seenOv0=ovY0;seenOv1=ovY1;seenFrontMod=front->modified;
        seenBand0=bandY0;seenBand1=bandY1;
        seenAny0=anyY0;seenAny1=anyY1;
        seenBackMod=back ? back->modified : 0;
    } else valid=FALSE;                  /* a failed write: compose the whole next tick */
    ReleaseSemaphore(&lock);
}

/* PrismD's tick: every board composes its own front screen. */
void present_tick(void)
{
    UBYTE k;
    /* the lock across the cards: P must not change between on_card and
     * present_card (present_card takes the lock again, which is allowed) */
    ObtainSemaphore(&lock);
    for (k = 0; k < prism_ncards(); k++) {
        on_card(&cards[k]);
        present_card();
    }
    ReleaseSemaphore(&lock);
}
