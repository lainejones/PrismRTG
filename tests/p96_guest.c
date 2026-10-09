/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Stefan Reinauer
 * Copyright (C) 2026 Laine Jones */
/* Dedicated-boot guest exerciser. Uses uaegfx.card and emits results on
 * the Amiga serial port. The P96 driver stays resident until reset. */
#include "../src/drv_p96.c"
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
    static const UBYTE bits[] = { 0xaa, 0x80, 0x55, 0x00 };
    struct P96Priv *p = b->priv;
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
        size = m.bytesPerRow * m.height;
        reference = AllocVec(size, MEMF_PUBLIC);
        if (!reference) { emit("FAIL memory\n"); return; }
        memset(reference, 0xcd, size);
        memset(b->vram, 0xcd, size);
        b->setPalette(b, 0, 256, palette);
        b->setDisplayStart(b, 0);
        b->setSwitch(b, TRUE);
        if (!b->fillRect || !b->copyRect || !b->expandRect) {
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
        if (!b->expandRect(b, 0, m.bytesPerRow, bpp, 30, 20, 9, 2,
                           bits, 2, pen, 0, FALSE)) {
            emit("FAIL template rejected\n"); FreeVec(reference); return;
        }
        for (y = 0; y < 2; y++) for (x = 0; x < 9; x++) {
            ULONG c = (bits[y*2+x/8] & (0x80 >> (x&7))) ? pen : 0;
            for (k = 0; k < bpp; k++)
                reference[(y+20)*m.bytesPerRow+(x+30)*bpp+k] = c >> (8*(bpp-k-1));
        }
        if (memcmp(reference, b->vram, size)) {
            sprintf(message, "FAIL template %u\n", bpp); emit(message); FreeVec(reference); return;
        }
        FreeVec(reference);
        if (b->cursorImage) {
            for (i = 0; i < 16; i++) cursor[i*CURSOR_SIZE+i] = 3;
            b->cursorImage(b, cursor, colors);
            b->cursorMove(b, 100, 100);
            b->cursorShow(b, TRUE);
        }
        b->waitVBlank(b);
        sprintf(message, "OK %u-bit pitch=%lu format=%u\n", bpp*8, (unsigned long)m.bytesPerRow, m.format);
        emit(message);
    }
    b->setSwitch(b, FALSE);
    emit("PASS\n");
}

int main(void)
{
    struct PrismBoard b;
    emit("P96 GUEST START\n");
    UtilityBase = OpenLibrary("utility.library", 39);
    GfxBase = (struct GfxBase *)OpenLibrary("graphics.library", 39);
    if (!UtilityBase || !GfxBase) { emit("FAIL libraries\n"); return 20; }
    if (P96_Probe(&b, "uaegfx.card", NULL)) exercise(&b);
    else emit("FAIL probe\n");
    P96_KeepResident();
    return 20;
}
