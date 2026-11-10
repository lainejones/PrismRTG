/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Stefan Reinauer
 * Copyright (C) 2026 Laine Jones */
/* Exercise bitmap.c itself, with a device that can schedule a direct writer
 * during an upload and can fail after transferring only part of a span. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <exec/types.h>
#include <exec/memory.h>
#include <proto/exec.h>

static BOOL no_memory;
static ULONG freed;
static APTR mock_alloc(ULONG size, ULONG flags)
{
    (void)flags;                         /* vamos: one kind of memory */
    return no_memory ? NULL : AllocVec(size,MEMF_ANY);
}
static void mock_free(APTR p)
{
    freed++;
    FreeVec(p);
}
static ULONG avail_cap;              /* nonzero: AvailMem says this much */
static ULONG mock_avail(ULONG flags)
{
    (void)flags;
    return avail_cap ? avail_cap : AvailMem(MEMF_ANY);
}
#undef AllocVec
#undef FreeVec
#undef AvailMem
#define AllocVec mock_alloc
#define FreeVec mock_free
#define AvailMem mock_avail
#include "../src/bitmap.c"

struct PrismBoard board;
struct PrismCard cards[MAX_CARDS] = { { &board, 0, 0 } };
struct SignalSemaphore lock;
struct GfxBase *GfxBase=NULL; /* not common: keeps libnix from auto-opening it */
static struct PBitMap bitmap;
static UBYTE pixels[512], device[512];
static ULONG writes, released;
static LONG change=-1;
static BOOL fail_write;

void dbg(const char *fmt, ...) { (void)fmt; }
void blit_settle(void) {}
BOOL pbm_is_shown(struct PBitMap *p) { (void)p; return FALSE; }
void pbm_gone(struct PBitMap *p) { (void)p; }
struct PBitMap *screen_bitmap_hook(ULONG w,ULONG h,ULONG d,ULONG f)
{ (void)w; (void)h; (void)d; (void)f; return NULL; }
LONG vram_alloc(struct PrismCard *c, ULONG size, UBYTE format, ULONG pitch, UWORD w, UWORD h)
{ (void)size; (void)format; (void)pitch; (void)w; (void)h; return 0; }
void vram_free(struct PrismCard *c, ULONG off) { (void)c; assert(off==0); released++; }

static BOOL write_device(struct PrismBoard *b, const struct PrismSurface *s,
    ULONG off, const void *data, ULONG count)
{
    (void)b; (void)s;
    assert(off+count<=sizeof(device));
    assert((ULONG)data<(ULONG)pixels ||
           (ULONG)data>=(ULONG)pixels+sizeof(pixels));
    writes++;
    if (fail_write) count=count ? 1 : 0;
    memcpy(device+off,data,count);
    if (change>=0) { pixels[change]++; change=-1; }
    return !fail_write;
}
static BOOL read_device(struct PrismBoard *b, const struct PrismSurface *s,
    ULONG off, APTR data, ULONG count)
{
    (void)b; (void)s;
    memcpy(data,device+off,count);
    if (change>=0) { pixels[change]++; change=-1; }
    return TRUE;
}
static ULONG reads;
static enum PrismResult fill_device(struct PrismBoard *b, const struct PrismSurface *s,
    UWORD x, UWORD y, UWORD w, UWORD h, ULONG c)
{
    UWORD r;
    (void)b;
    for (r=0; r<h; r++) memset(device+(ULONG)(y+r)*s->pitch+x,(UBYTE)c,w);
    return PR_DONE;
}
static enum PrismResult copy_device(struct PrismBoard *b, const struct PrismSurface *s,
    const struct PrismSurface *d, UWORD sx, UWORD sy, UWORD dx, UWORD dy, UWORD w, UWORD h)
{
    UWORD r;
    (void)b; (void)d;
    for (r=0; r<h; r++) {
        UWORD row = dy>sy ? h-1-r : r;
        memmove(device+(ULONG)(dy+row)*s->pitch+dx,device+(ULONG)(sy+row)*s->pitch+sx,w);
    }
    return PR_DONE;
}
static BOOL counting_read(struct PrismBoard *b, const struct PrismSurface *s,
    ULONG off, APTR data, ULONG count)
{
    reads++;
    return read_device(b,s,off,data,count);
}
static const struct PrismOps ops = { .read=counting_read, .write=write_device,
    .fill=fill_device, .copy=copy_device };

static void equal_history(void)
{
    assert(bitmap.uploadValid);
    assert(!memcmp(device,bitmap.uploaded,sizeof(device)));
}
static void test_upload_races(void)
{
    ULONG calls;
    memset(pixels,1,sizeof(pixels));
    change=42;
    assert(pbm_upload(&bitmap));
    equal_history();
    assert(pixels[42]==2 && device[42]==1);
    assert(pbm_upload(&bitmap));
    equal_history();
    assert(device[42]==2);
    calls=writes;
    assert(pbm_upload(&bitmap) && writes==calls);

    pixels[43]=3;
    change=43;
    assert(pbm_upload(&bitmap));
    equal_history();
    assert(pixels[43]==4 && device[43]==3);
    assert(pbm_upload(&bitmap));
    equal_history();
    assert(device[43]==4);

    pixels[44]=5; pixels[46]=6;
    fail_write=TRUE;
    assert(!pbm_upload(&bitmap) && !bitmap.uploadValid);
    assert(device[44]==5 && device[46]==1);
    fail_write=FALSE;
    assert(pbm_upload(&bitmap));
    equal_history();
    assert(device[46]==6);
}
static void test_readback(void)
{
    device[44]=7;
    change=44;
    assert(pbm_result(&bitmap,PR_DONE,44,0,1,1)==PR_DONE);
    equal_history();
    assert(pixels[44]==7);
}
static void test_eviction(void)
{
    ULONG before=freed;
    bitmap.locks=1;
    assert(pbm_to_fast(&bitmap));
    assert(!bitmap.inVram && bitmap.pix==pixels && bitmap.mem==pixels);
    assert(!bitmap.uploaded && !bitmap.uploadValid);
    assert(freed==before+1 && released==1);
    assert(pbm_to_vram(&bitmap));
    equal_history();
    assert(pbm_to_fast(&bitmap));

    before=freed;
    fail_write=TRUE;
    assert(!pbm_to_vram(&bitmap));
    assert(!bitmap.inVram && !bitmap.uploaded && !bitmap.uploadValid);
    assert(freed==before+1 && released==3);
    fail_write=FALSE;
}
/* Uploads before an operation cover its rows only; the rest waits */
static void test_rows(void)
{
    assert(pbm_upload(&bitmap));
    equal_history();
    pixels[10]=20; pixels[256+10]=21;      /* a change in each row */
    assert(pbm_upload_rows(&bitmap,1,1));
    assert(device[256+10]==21 && device[10]!=20);
    assert(pbm_upload_rows(&bitmap,-5,6));  /* clipped to row 0 */
    assert(device[10]==20);
    assert(pbm_upload_rows(&bitmap,2,5) && pbm_upload_rows(&bitmap,0,0));
    equal_history();
}
/* A fill or a copy on the card is repeated in the shadow and the history,
 * with no readback: the shadow, the history and VRAM agree afterwards */
static void test_mirror(void)
{
    ULONG before;
    assert(pbm_upload(&bitmap));
    equal_history();
    pixels[256+100]=0x5a;                  /* pending in row 1, inside the copy */
    before=reads;
    assert(pbm_hw_fill(&bitmap,1,3,0,40,2,0x77)==PR_DONE);
    assert(reads==before);
    assert(pixels[3]==0x77 && pixels[42]==0x77 && pixels[256+3]==0x77 && pixels[43]!=0x77);
    equal_history();
    assert(!memcmp(device,pixels,sizeof(device)));
    pixels[256+100]=0x5b;
    assert(pbm_hw_copy(&bitmap,&bitmap,90,0,95,1,20,1)==PR_DONE);
    assert(reads==before);
    assert(pixels[256+95]==pixels[90] && pixels[256+114]==pixels[109]);
    equal_history();
    assert(!memcmp(device,pixels,sizeof(device)));
    /* without a history (low memory) the rows are read back as before */
    no_memory=TRUE;
    FreeVec(bitmap.uploaded); bitmap.uploaded=NULL; bitmap.uploadValid=FALSE;
    assert(pbm_hw_fill(&bitmap,1,0,1,8,1,0x11)==PR_DONE);
    assert(reads==before+1 && pixels[256]==0x11 && device[256]==0x11);
    no_memory=FALSE;
}
/* The history is not taken when it would leave less than the reserve */
static void test_reserve(void)
{
    FreeVec(bitmap.uploaded); bitmap.uploaded=NULL; bitmap.uploadValid=FALSE;
    avail_cap=SHADOW_RESERVE+sizeof(pixels)-1;
    pixels[7]=0x33;
    assert(pbm_upload(&bitmap) && !bitmap.uploaded && device[7]==0x33);
    avail_cap=SHADOW_RESERVE+sizeof(pixels);
    assert(pbm_upload(&bitmap) && bitmap.uploaded);
    equal_history();
    avail_cap=0;
    FreeVec(bitmap.uploaded); bitmap.uploaded=NULL; bitmap.uploadValid=FALSE;
}
static void test_low_memory(void)
{
    no_memory=TRUE;
    bitmap.inVram=TRUE;
    pixels[42]=10; change=42;
    assert(pbm_upload(&bitmap));
    assert(!bitmap.uploaded && !bitmap.uploadValid);
    assert(device[42]==10 && pixels[42]==11);
    assert(pbm_upload(&bitmap));
    assert(!memcmp(device,pixels,sizeof(device)));
    no_memory=FALSE;
}
int main(void)
{
    board.flags=PBF_SHADOW; board.ops=&ops;
    bitmap.pix=pixels; bitmap.mem=pixels;
    bitmap.card=&cards[0]; bitmap.bd=&board;
    bitmap.w=bitmap.bpr=256; bitmap.h=2; bitmap.bpp=1;
    bitmap.fmt=PF_CLUT8; bitmap.inVram=TRUE;
    board.opsSize=sizeof(ops);
    test_upload_races();
    test_readback();
    test_eviction();
    assert(pbm_to_vram(&bitmap));
    test_rows();
    test_mirror();
    test_reserve();
    test_low_memory();
    puts("shadow upload snapshots and eviction passed");
    return 0;
}
