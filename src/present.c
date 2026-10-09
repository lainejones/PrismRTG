/* SPDX-License-Identifier: GPL-3.0-only */
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
 *    rows under the pointer when it moved;
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

static struct PBitMap frame;
static UBYTE *previous;
static BOOL allocated, valid, owned;
static struct PBitMap *input;
static struct PBitMap *failedInput;
static UWORD failedWidth,failedHeight,retryTicks;

/* per row: how it was made, this tick and the last uploaded one */
#define ROW_CPU   0              /* composed in frame.pix, uploaded by changed span */
#define ROW_HW    1              /* the front's band, copied by the blitter          */
#define ROW_HWCPU 2              /* in the band, but with a PIP or the pointer on it:
                                    read back, composed, written in full            */
static UBYTE *rowMode, *prevMode, *rowDirty;

/* what the last composed frame was made from */
static ULONG seenFrontMod, seenBackMod, seenPal, seenPip;
static WORD seenTop, seenBackTop, seenPX, seenPY, seenOv0, seenOv1;
static const void *seenScreen, *seenFront, *seenBack;
static BOOL seenHwBand;
static UWORD idleTicks;
#define SAFETY_TICKS 25          /* a full frame every half second regardless */

BOOL present_ready(void) { return allocated && valid; }

/* Caller holds the board lock. Switch away before releasing scanout VRAM. */
void present_stop(void)
{
    struct PBitMap *front=prism_display_bitmap();
    retryTicks=0;failedInput=NULL;
    if (!allocated) return;
    if (front && front->inVram) board.setDisplayStart(&board,front->vramOff);
    board.flags &= ~PBF_PRESENT;
    if (!owned && input && input->inVram) {
        input->uploadValid=FALSE;pbm_upload(input);
    }
    if (owned) vram_free(frame.vramOff);
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
    present_stop();
    pitch=board.ops && board.ops->pitch ? board.ops->pitch(&board,width,height,front->fmt) :
        board.bytesPerRow ? board.bytesPerRow(&board,width,height,front->bpp) :
        (((ULONG)width*front->bpp+7)&~7UL);
    if (!pitch || pitch<width*front->bpp || pitch>0x7fffffffUL/height) return FALSE;
    if ((board.flags & PBF_SHADOW) && front->inVram && front->w>=width && front->h>=height)
        pitch=front->bpr;
    size=pitch*height;
    frame.pix=AllocVec(size,MEMF_PUBLIC|MEMF_CLEAR);
    previous=AllocVec(size,MEMF_ANY);
    rowMode=AllocVec(height,MEMF_ANY|MEMF_CLEAR);
    prevMode=AllocVec(height,MEMF_ANY);
    rowDirty=AllocVec(height,MEMF_ANY);
    if (!frame.pix || !previous || !rowMode || !prevMode || !rowDirty) goto fail;
    memset(prevMode,ROW_HW,height);          /* nothing uploaded yet: no row comparable */
    owned=!(board.flags & PBF_SHADOW) || !front->inVram || front->w<width || front->h<height;
    /* hidden screens move to fast RAM to make room (a 2 MB card holds
     * the shown screen, its composed copy and little else) */
    off=owned ? vram_get_size(size,front->fmt,pitch,width,height,front) : (LONG)front->vramOff;
    if (off<0) goto fail;
    frame.w=width;frame.h=height;frame.bpr=pitch;frame.bpp=front->bpp;
    frame.fmt=front->fmt;frame.vramOff=off;frame.inVram=TRUE;
    allocated=TRUE;input=front;valid=FALSE;
    if (!owned) {
        board.flags|=PBF_PRESENT;front->uploadValid=FALSE;
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
    if (screen!=prism_display_layers(&front,&back,&top,&backTop,&width,&height)) return TRUE;
    retryTicks=0; /* An explicit new PIP request may retry immediately. */
    return prepare(front,width,height);
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
        if (board.blitMaxRows && n>board.blitMaxRows) n=board.blitMaxRows;
        board.copyBetween(&board,front->vramOff+(ULONG)sy0*front->bpr,front->bpr,
                          frame.vramOff+(ULONG)y0*frame.bpr,frame.bpr,wbytes,n);
        y0+=n;sy0+=n;
    }
    if (board.waitBlit) board.waitBlit(&board);
}

static inline WORD clampw(LONG v,WORD lo,WORD hi) { return v<lo ? lo : v>hi ? hi : (WORD)v; }

void present_tick(void)
{
    struct PBitMap *front,*back;
    struct Screen *screen;
    struct PrismSurface surface;
    UWORD width,height,y; WORD top,backTop;
    BOOL showBack,hwBand,full,frontChanged,backChanged,ptrChanged;
    static UBYTE backMap[256];
    static ULONG mapPal; static const struct PBitMap *mapFor;
    const UBYTE *map=NULL;
    WORD bandY0=0,bandY1=0,ovY0=0,ovY1=0,pipY0,pipY1,ptrY0,ptrY1;
    ULONG wrow;
    ObtainSemaphore(&lock);
    screen=prism_display_layers(&front,&back,&top,&backTop,&width,&height);
    if (!prism_dragging()) top=0;
    /* The pointer alone is a software sprite (pointer.c), not a reason
     * to compose; while composing, pointer_compose draws it. */
    if (!screen || (!top && !p96_pip_active(screen))) {
        present_stop(); ReleaseSemaphore(&lock); return;
    }
    if (!prepare(front,width,height)) { ReleaseSemaphore(&lock);return; }

    frame.rgbTab=front->rgbTab;
    showBack=back && (top>0 || (LONG)top+front->h<height);
    /* the front's band: rows of the frame it covers */
    if (top<height && (LONG)top+front->h>0) {
        bandY0=top<0 ? 0 : top;
        bandY1=(LONG)top+front->h>height ? height : top+front->h;
    }
    hwBand = bandY1>bandY0 && owned && front->inVram && front->w>=width &&
        front->fmt==frame.fmt && board.copyBetween &&
        !(board.flags & (PBF_SHADOW|PBF_SOFTWARE|PBF_ACCEL_BROKEN|PBF_PRESENT)) &&
        hw_fits((UWORD)(width*frame.bpp),1,front->bpr>frame.bpr ? front->bpr : frame.bpr);
    /* rows in the band that carry something on top of the front: read
     * back from the front and composed by the CPU */
    if (hwBand) {
        if (p96_pip_rows(screen,top,&pipY0,&pipY1)) { ovY0=pipY0;ovY1=pipY1; }
        if (pointer_compose_rows(top,&ptrY0,&ptrY1)) {
            if (ovY1<=ovY0) { ovY0=ptrY0;ovY1=ptrY1; }
            else { if (ptrY0<ovY0) ovY0=ptrY0; if (ptrY1>ovY1) ovY1=ptrY1; }
        }
        ovY0=clampw(ovY0,bandY0,bandY1);ovY1=clampw(ovY1,bandY0,bandY1);
    }

    /* what changed since the last frame? */
    full = !valid || screen!=seenScreen || front!=seenFront || back!=seenBack ||
        top!=seenTop || backTop!=seenBackTop || paletteGen!=seenPal || pipGen!=seenPip ||
        hwBand!=seenHwBand || ++idleTicks>=SAFETY_TICKS;
    frontChanged = full || front->modified!=seenFrontMod || volatile_bitmap(front) ||
        p96_pip_volatile(screen);
    backChanged = full || (back && (back->modified!=seenBackMod || volatile_bitmap(back))) ||
        (back && screen->NextScreen && p96_pip_volatile(screen->NextScreen));
    ptrChanged = full || posX!=seenPX || posY!=seenPY || ovY0!=seenOv0 || ovY1!=seenOv1;
    if (!frontChanged && !backChanged && !ptrChanged) { ReleaseSemaphore(&lock); return; }
    if (full) idleTicks=0;

    if(showBack && back->fmt==PF_CLUT8 && frame.fmt==PF_CLUT8 &&
       back->rgbTab!=frame.rgbTab) {
        /* 65,536 nearest-pen searches: once per palette, not per tick */
        if (mapFor!=back || mapPal!=paletteGen) {
            composite_clut_map(&frame,back,backMap);mapFor=back;mapPal=paletteGen;
        }
        map=backMap;
    }
    if (board.waitBlit) board.waitBlit(&board);

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
            redo = frontChanged || ptrChanged;
            if (!redo) continue;
            rowDirty[y]=TRUE;
            if (wrow<frame.bpr) memset(row+wrow,0,frame.bpr-wrow);
            composite_row(&frame,y,front,sy);
            continue;
        }
        redo = backChanged || (ptrChanged && ((y>=ovY0 && y<ovY1) || (y>=seenOv0 && y<seenOv1)));
        if (!redo) continue;
        rowDirty[y]=TRUE;
        memset(row,0,frame.bpr);
        if (showBack && by>=0 && by<back->h)
            composite_row_mapped(&frame,y,back,by,map);
    }
    if(showBack && screen->NextScreen && backChanged)
        p96_pip_compose(&frame,screen->NextScreen,backTop);
    if (hwBand) {
        if (frontChanged) band_blit(front,bandY0,bandY1,top);
        else if (ptrChanged && seenOv1>seenOv0)
            band_blit(front,clampw(seenOv0,bandY0,bandY1),clampw(seenOv1,bandY0,bandY1),top);
    }
    p96_pip_compose(&frame,screen,top);
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
        if (board.ops && board.ops->write) {
            if (!board.ops->write(&board,&surface,off,frame.pix+off,count)) break;
        } else CopyMem(frame.pix+off,board.vram+frame.vramOff+off,count);
        CopyMem(frame.pix+off,previous+off,count);
        prevMode[y]=mode;
    }
    if (y==height) {
        valid=TRUE;board.setDisplayStart(&board,frame.vramOff);
        seenScreen=screen;seenFront=front;seenBack=back;seenTop=top;seenBackTop=backTop;
        seenPX=posX;seenPY=posY;seenPal=paletteGen;seenPip=pipGen;seenHwBand=hwBand;
        seenOv0=ovY0;seenOv1=ovY1;seenFrontMod=front->modified;
        seenBackMod=back ? back->modified : 0;
    } else valid=FALSE;                  /* a failed write: compose the whole next tick */
    ReleaseSemaphore(&lock);
}
