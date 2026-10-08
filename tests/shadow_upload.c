/* SPDX-License-Identifier: GPL-3.0-only */
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
    return no_memory ? NULL : AllocVec(size,flags);
}
static void mock_free(APTR p)
{
    freed++;
    FreeVec(p);
}
#undef AllocVec
#undef FreeVec
#define AllocVec mock_alloc
#define FreeVec mock_free
#include "../src/bitmap.c"

struct PrismBoard board;
struct SignalSemaphore lock;
struct GfxBase *GfxBase;
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
LONG vram_alloc(ULONG size, UBYTE format, ULONG pitch, UWORD w, UWORD h)
{ (void)size; (void)format; (void)pitch; (void)w; (void)h; return 0; }
void vram_free(ULONG off) { assert(off==0); released++; }

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
static const struct PrismOps ops = { .read=read_device, .write=write_device };

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
    bitmap.w=bitmap.bpr=256; bitmap.h=2; bitmap.bpp=1;
    bitmap.fmt=PF_CLUT8; bitmap.inVram=TRUE;
    test_upload_races();
    test_readback();
    test_eviction();
    test_low_memory();
    puts("shadow upload snapshots and eviction passed");
    return 0;
}
