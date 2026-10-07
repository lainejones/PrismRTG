/* SPDX-License-Identifier: GPL-3.0-only */
/* Dedicated-boot guest exerciser. Binds UAE firmware directly and emits results
 * on the Amiga serial port. The native context stays resident until reset. */
#include "../src/drv_uaegfx.c"
struct Library *UtilityBase;
struct GfxBase *GfxBase;

static void emit(const char *s)
{
    *(volatile UWORD *)0xdff032 = 30;
    while (*s) {
        while (!(*(volatile UWORD *)0xdff018 & 0x2000)) {}
        *(volatile UWORD *)0xdff030 = 0x100 | (UBYTE)*s++;
    }
}

static void exercise(struct PrismBoard *b)
{
    UBYTE bpp;
    UBYTE palette[768];
    static UBYTE cursor[CURSOR_SIZE * CURSOR_SIZE];
    static const UBYTE colors[] = { 255,0,0, 0,255,0, 255,255,255 };
    struct UAEPriv *p = b->priv;
    ULONG i;
    for (i = 0; i < 256; i++) palette[3*i] = palette[3*i+1] = palette[3*i+2] = i;
    for (bpp = 1; bpp <= 4; bpp++) {
        struct PrismMode m = { 640, 480, p->pf[bpp], 0, 0 };
        UBYTE *reference;
        ULONG size, x, y, k;
        const ULONG pen = 0x11223344;
        char message[80];
        if (p->pf[bpp] == PF_COUNT) { emit("FAIL missing format\n"); return; }
        if (!b->checkMode(b, &m) || !b->setMode(b, &m)) {
            emit("FAIL mode\n"); return;
        }
        {
            struct PrismMode invalid = m;
            invalid.refresh = 75;
            if (b->checkMode(b, &invalid) || b->bytesPerRow(b, 8192, 480, 4)) {
                emit("FAIL limits\n"); return;
            }
        }
        size = m.bytesPerRow * m.height;
        reference = AllocVec(size, MEMF_PUBLIC);
        if (!reference) { emit("FAIL memory\n"); return; }
        memset(reference, 0xcd, size);
        memset(b->vram, 0xcd, size);
        b->setPalette(b, 0, 256, palette);
        b->setDisplayStart(b, 0);
        b->setSwitch(b, TRUE);
        if (!b->fillRect || !b->copyRect) {
            emit("FAIL missing acceleration\n"); FreeVec(reference); return;
        }
        b->fillRect(b, 0, m.bytesPerRow, bpp, 2, 3, 13, 7, pen);
        for (y = 3; y < 10; y++) for (x = 2; x < 15; x++)
            for (k = 0; k < bpp; k++)
                reference[y*m.bytesPerRow+x*bpp+k] = pen >> (8*(bpp-k-1));
        if (memcmp(reference, b->vram, size)) {
            sprintf(message, "FAIL fill %u\n", bpp); emit(message); FreeVec(reference); return;
        }
        b->copyRect(b, 0, m.bytesPerRow, bpp, 2, 3, 5, 5, 13, 7);
        for (y = 7; y-- > 0;)
            memmove(reference+(y+5)*m.bytesPerRow+5*bpp,
                    reference+(y+3)*m.bytesPerRow+2*bpp, 13*bpp);
        if (memcmp(reference, b->vram, size)) {
            sprintf(message, "FAIL copy %u\n", bpp); emit(message); FreeVec(reference); return;
        }
        FreeVec(reference);
        if (b->cursorImage) {
            for (i = 0; i < 16; i++) cursor[i*CURSOR_SIZE+i] = 3;
            b->cursorImage(b, cursor, colors);
            b->cursorMove(b, 100, 100);
            b->cursorShow(b, TRUE);
        }
        b->waitVBlank(b);
        sprintf(message, "OK %u-bit pitch=%lu format=%u\n", bpp*8, m.bytesPerRow, m.format);
        emit(message);
    }
    /* Exercise the ROM fallback callbacks even if the host always blits. */
    {
        UBYTE pixels[32], expected[32];
        struct RenderInfo ri = { pixels, 8, 0, RGBFB_CLUT };
        memset(pixels, 0xa0, sizeof(pixels));
        p->bi.FillRectDefault(&p->bi, &ri, 1, 0, 4, 2, 5, 15, RGBFB_CLUT);
        if (pixels[0] != 0xa0 || pixels[1] != 0xa5 || pixels[12] != 0xa5) {
            emit("FAIL fill fallback\n"); return;
        }
        memcpy(expected, pixels, sizeof(pixels));
        memmove(expected+16, expected+8, 8);
        memmove(expected+8, expected, 8);
        p->bi.BlitRectDefault(&p->bi, &ri, 0, 0, 0, 1, 8, 2, 255, RGBFB_CLUT);
        if (memcmp(pixels, expected, sizeof(pixels))) {
            emit("FAIL copy fallback\n"); return;
        }
    }
    b->shutdown(b);
    emit("PASS\n");
}

int main(void)
{
    struct PrismBoard b;
    emit("UAEGFX GUEST START\n");
    UtilityBase = OpenLibrary("utility.library", 39);
    GfxBase = (struct GfxBase *)OpenLibrary("graphics.library", 39);
    if (!UtilityBase || !GfxBase) { emit("FAIL libraries\n"); return 20; }
    if (UAEGFX_Probe(&b)) exercise(&b);
    else emit("FAIL probe\n");
    UAEGFX_KeepResident();
    return 20;
}
