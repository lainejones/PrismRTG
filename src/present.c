/* SPDX-License-Identifier: GPL-3.0-only */
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
    FreeVec(frame.pix); FreeVec(previous);
    memset(&frame,0,sizeof(frame));previous=NULL;allocated=valid=owned=FALSE;input=NULL;
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
    if (!frame.pix || !previous) goto fail;
    owned=!(board.flags & PBF_SHADOW) || !front->inVram || front->w<width || front->h<height;
    /* hidden screens move to fast RAM to make room (a 2 MB card holds
     * the shown screen, its composed copy and little else) */
    off=owned ? vram_get_size(size,front->fmt,pitch,width,height,front) : (LONG)front->vramOff;
    if (off<0) goto fail;
    frame.w=width;frame.h=height;frame.bpr=pitch;frame.bpp=front->bpp;
    frame.fmt=front->fmt;frame.vramOff=off;frame.inVram=TRUE;
    allocated=TRUE;input=front;
    if (!owned) {
        board.flags|=PBF_PRESENT;front->uploadValid=FALSE;
        if(front->uploaded) { FreeVec(front->uploaded);front->uploaded=NULL; }
    }
    return TRUE;
fail:
    if(frame.pix)FreeVec(frame.pix);
    if(previous)FreeVec(previous);
    memset(&frame,0,sizeof(frame));previous=NULL;
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

void present_tick(void)
{
    struct PBitMap *front,*back;
    struct Screen *screen;
    struct PrismSurface surface;
    UWORD width,height,y; WORD top,backTop;
    BOOL showBack;
    static UBYTE backMap[256];
    const UBYTE *map=NULL;
    ObtainSemaphore(&lock);
    screen=prism_display_layers(&front,&back,&top,&backTop,&width,&height);
    if (!prism_dragging()) top=0;
    if (!screen || (!top && !pointer_software() && !p96_pip_active(screen))) {
        present_stop(); ReleaseSemaphore(&lock); return;
    }
    if (!prepare(front,width,height)) { ReleaseSemaphore(&lock);return; }
    frame.rgbTab=front->rgbTab;
    showBack=back && (top>0 || (LONG)top+front->h<height);
    if(showBack && back->fmt==PF_CLUT8 && frame.fmt==PF_CLUT8 &&
       back->rgbTab!=frame.rgbTab) {
        composite_clut_map(&frame,back,backMap);map=backMap;
    }
    if (board.waitBlit) board.waitBlit(&board);
    for (y=0;y<height;y++) {
        LONG by=(LONG)y-backTop,sy=(LONG)y-top;
        memset(frame.pix+(ULONG)y*frame.bpr,0,frame.bpr);
        if (showBack && (sy<0 || sy>=front->h) && by>=0 && by<back->h)
            composite_row_mapped(&frame,y,back,by,map);
    }
    if(showBack && screen->NextScreen)
        p96_pip_compose(&frame,screen->NextScreen,backTop);
    for (y=0;y<height;y++) {
        LONG sy=(LONG)y-top;
        if (sy>=0 && sy<front->h) {
            memset(frame.pix+(ULONG)y*frame.bpr,0,frame.bpr);
            composite_row(&frame,y,front,sy);
        }
    }
    p96_pip_compose(&frame,screen,top);
    pointer_compose(&frame,top);
    pbm_surface(&frame,&surface);
    for (y=0;y<height;y++) {
        ULONG first=0,count=frame.bpr,off=(ULONG)y*frame.bpr;
        if (valid && !changed_span(frame.pix+off,previous+off,frame.bpr,&first,&count)) continue;
        off+=first;
        if (board.ops && board.ops->write) {
            if (!board.ops->write(&board,&surface,off,frame.pix+off,count)) break;
        } else CopyMem(frame.pix+off,board.vram+frame.vramOff+off,count);
        CopyMem(frame.pix+off,previous+off,count);
    }
    if (y==height) { valid=TRUE;board.setDisplayStart(&board,frame.vramOff); }
    ReleaseSemaphore(&lock);
}
