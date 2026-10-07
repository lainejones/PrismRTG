/* SPDX-License-Identifier: GPL-3.0-only */
/* Run as an Amiga executable (or with vamos) to exercise the actual 68k
 * register ABI, rather than compiling register callbacks away on a host. */
#include <assert.h>
#include "../src/drv_p96.c"

struct Library *UtilityBase;
struct GfxBase *GfxBase;
static struct P96Priv p;
static struct PrismBoard b;
static UBYTE pixels[4096], expected[4096];
static ULONG waits;
static int events[32], nevents;

static void mock_wait(struct BoardInfo *bi __asm("a0"))
{
    assert(bi == &p.bi);
    waits++;
}

static ULONG mock_compatible(struct BoardInfo *bi __asm("a0"), RGBFTYPE f __asm("d7"))
{
    (void)f;
    return bi->RGBFormats;
}

static APTR mock_memory(struct BoardInfo *bi __asm("a0"), APTR mem __asm("a1"),
                        struct RenderInfo *ri __asm("d0"), RGBFTYPE f __asm("d7"))
{
    (void)bi; (void)ri;
    /* This aperture cannot be represented by Prism's single base. */
    if (f == RGBFB_R5G6B5) return (UBYTE *)mem + 16;
    return mem;
}

static UWORD mock_pitch(struct BoardInfo *bi __asm("a0"), UWORD w __asm("d0"),
                        UWORD h __asm("d1"), struct ModeInfo *mi __asm("a1"),
                        RGBFTYPE f __asm("d7"))
{
    (void)bi; (void)h; (void)mi;
    return ((ULONG)w * rgb_bpp(f) + 63) & ~63UL;
}

static LONG mock_clock(struct BoardInfo *bi __asm("a0"), struct ModeInfo *mi __asm("a1"),
                        ULONG clock __asm("d0"), RGBFTYPE f __asm("d7"))
{
    assert(bi == &p.bi && f == RGBFB_CLUT);
    if (clock == 25175000)
        assert(mi->HorSyncStart == 16 && mi->VerSyncStart == 10);
    mi->PixelClock = clock;
    return 0;
}

static void mock_gc(struct BoardInfo *bi __asm("a0"), struct ModeInfo *mi __asm("a1"),
                    BOOL border __asm("d0"))
{
    assert(bi->ModeInfo == mi && mi->Width == 640 && border);
    events[nevents++] = 2;
}

static void mock_dac(struct BoardInfo *bi __asm("a0"), UWORD region __asm("d0"),
                     RGBFTYPE f __asm("d7"))
{
    assert(bi == &p.bi && region == 0 && f == RGBFB_CLUT);
    events[nevents++] = 3;
}

static BOOL mock_display(struct BoardInfo *bi __asm("a0"), BOOL on __asm("d0"))
{
    assert(bi == &p.bi);
    events[nevents++] = on ? 5 : 1;
    return !on;
}

static BOOL mock_switch(struct BoardInfo *bi __asm("a0"), BOOL on __asm("d0"))
{
    assert(bi == &p.bi);
    return !on;
}

static void mock_pan(struct BoardInfo *bi __asm("a0"), UBYTE *mem __asm("a1"),
                     UWORD w __asm("d0"), UWORD h __asm("d3"), WORD x __asm("d1"),
                     WORD y __asm("d2"), RGBFTYPE f __asm("d7"))
{
    assert(bi == &p.bi && mem == pixels && w == 640 && h == 480);
    assert(x == 0 && y == 0 && f == RGBFB_CLUT);
    events[nevents++] = 4;
}

static void mock_palette(struct BoardInfo *bi __asm("a0"), UWORD start __asm("d0"),
                         UWORD count __asm("d1"))
{
    assert(start == 255 && count == 1);
    assert(bi->CLUT[255].Red == 10 && bi->CLUT[255].Green == 20 && bi->CLUT[255].Blue == 30);
}

static void setup(void)
{
    memset(&p, 0, sizeof(p));
    memset(&b, 0, sizeof(b));
    init_boardinfo(&p);
    p.bi.WaitBlitter = mock_wait;
    p.bi.MemoryBase = pixels;
    p.bi.MemorySize = 4 * 1024 * 1024;
    p.bi.RGBFormats = RGBFF_CLUT | RGBFF_R5G6B5 | RGBFF_R5G6B5PC |
                      RGBFF_B8G8R8 | RGBFF_A8R8G8B8;
    p.bi.GetCompatibleFormats = mock_compatible;
    p.bi.CalculateMemory = mock_memory;
    p.bi.CalculateBytesPerRow = mock_pitch;
    p.bi.ResolvePixelClock = mock_clock;
    p.bi.SetGC = mock_gc; p.bi.SetDAC = mock_dac;
    p.bi.SetDisplay = mock_display; p.bi.SetSwitch = mock_switch;
    p.bi.SetPanning = mock_pan; p.bi.SetColorArray = mock_palette;
    assert(attach_board(&p, &b));
    assert(p.pf[2] == PF_RGB565LE); /* big-endian aperture excluded */
}

static void test_fill(void)
{
    static const RGBFTYPE formats[] = { RGBFB_CLUT, RGBFB_R5G6B5PC,
                                        RGBFB_B8G8R8, RGBFB_A8R8G8B8 };
    ULONG i, x, y;
    for (i = 0; i < 4; i++) {
        UBYTE bpp = i + 1;
        struct RenderInfo ri = { pixels, 48, 0, formats[i] };
        memset(pixels, 0x5a, sizeof(pixels));
        memcpy(expected, pixels, sizeof(pixels));
        for (y = 2; y < 5; y++) for (x = 1; x < 6; x++) {
            UBYTE *d = expected + y * 48 + x * bpp;
            if (bpp == 1) d[0] = 0x54;
            if (bpp == 2) { d[0] = 0x33; d[1] = 0x44; }
            if (bpp == 3) { d[0] = 0x44; d[1] = 0x33; d[2] = 0x22; }
            if (bpp == 4) { d[0] = 0x11; d[1] = 0x22; d[2] = 0x33; d[3] = 0x44; }
        }
        p.bi.FillRectDefault(&p.bi, &ri, 1, 2, 5, 3, 0x11223344, 0x0f, formats[i]);
        assert(!memcmp(pixels, expected, sizeof(pixels)));
    }
}

static void test_overlap(void)
{
    UBYTE snapshot[512];
    ULONG i, x, y;
    struct RenderInfo ri = { pixels, 32, 0, RGBFB_CLUT };
    for (i = 0; i < sizeof(pixels); i++) pixels[i] = i ^ (i >> 4);
    memcpy(expected, pixels, sizeof(pixels));
    memcpy(snapshot, pixels, sizeof(snapshot));
    for (y = 0; y < 8; y++) for (x = 0; x < 12; x++) {
        UBYTE *d = expected + (y + 2) * 32 + x + 3;
        *d = (*d & 0xf0) | (snapshot[y * 32 + x] & 0x0f);
    }
    p.bi.BlitRectDefault(&p.bi, &ri, 0, 0, 3, 2, 12, 8, 0x0f, RGBFB_CLUT);
    assert(!memcmp(pixels, expected, sizeof(pixels)));
    memcpy(snapshot, pixels, sizeof(snapshot));
    for (y = 0; y < 8; y++) for (x = 0; x < 12; x++)
        expected[y * 32 + x] = snapshot[(y + 2) * 32 + x + 3];
    p.bi.BlitRectDefault(&p.bi, &ri, 3, 2, 0, 0, 12, 8, 0xff, RGBFB_CLUT);
    assert(!memcmp(pixels, expected, sizeof(pixels)));
}

static void test_template(void)
{
    UBYTE src[] = { 0x55, 0x80, 0xaa, 0x00 };
    struct RenderInfo ri = { pixels, 32, 0, RGBFB_CLUT };
    struct Template t = { src, 2, 1, JAM1, 0x12, 0x34 };
    memset(pixels, 0x56, sizeof(pixels));
    p.bi.BlitTemplateDefault(&p.bi, &ri, &t, 0, 0, 8, 2, 0xff, RGBFB_CLUT);
    assert(pixels[0] == 0x12 && pixels[1] == 0x56 && pixels[7] == 0x12);
    assert(pixels[32] == 0x56 && pixels[33] == 0x12 && pixels[39] == 0x56);
    t.DrawMode = JAM2 | INVERSVID;
    p.bi.BlitTemplateDefault(&p.bi, &ri, &t, 0, 0, 8, 2, 0xff, RGBFB_CLUT);
    assert(pixels[0] == 0x34 && pixels[1] == 0x12 && pixels[7] == 0x34);
    t.DrawMode = COMPLEMENT;
    p.bi.BlitTemplateDefault(&p.bi, &ri, &t, 0, 0, 8, 2, 0x0f, RGBFB_CLUT);
    assert(pixels[0] == 0x3b && pixels[1] == 0x12);
}

static void test_modes(void)
{
    struct PrismMode m = { 640, 480, PF_CLUT8, 60, 0 };
    UBYTE rgb[] = { 10, 20, 30, 99, 99, 99 };
    assert(b.checkMode(&b, &m));
    assert(m.bytesPerRow == 640 && m.refresh == 60);
    assert(b.bytesPerRow(&b, 641, 480, 1) == 704);
    assert(b.setMode(&b, &m));
    assert(nevents == 5 && events[0] == 1 && events[1] == 2 &&
           events[2] == 3 && events[3] == 4 && events[4] == 5);
    assert(p.bi.ModeInfo == &p.mode);
    b.setPalette(&b, 255, 2, rgb);
    m.width = 123;
    assert(!b.checkMode(&b, &m));
    m.width = 640; m.refresh = 75;
    p.bi.MemorySize = 640 * 479;
    assert(!b.checkMode(&b, &m));
    p.bi.MemorySize = 4 * 1024 * 1024;
    /* Drivers with internal modes must never be fed invented timings. */
    p.bi.Flags |= BIF_INTERNALMODESONLY;
    assert(!b.checkMode(&b, &m));
    {
        struct LibResolution res;
        struct ModeInfo mode;
        memset(&res, 0, sizeof(res));
        memset(&mode, 0, sizeof(mode));
        res.Width = mode.Width = 640; res.Height = mode.Height = 480;
        mode.Depth = 8; mode.HorTotal = 800; mode.VerTotal = 525;
        mode.PixelClock = 25200000;
        res.Modes[CHUNKY] = &mode;
        AddTail((struct List *)&p.bi.ResolutionsList, &res.Node);
        m.refresh = 60;
        assert(b.checkMode(&b, &m));
        m.refresh = 75;
        assert(!b.checkMode(&b, &m));
        Remove(&res.Node);
    }
}

int main(void)
{
    setup();
    test_fill();
    test_overlap();
    test_template();
    test_modes();
    assert(waits != 0);
    puts("P96 adapter: ABI, formats, drawing fallbacks and mode setup passed");
    return 0;
}
