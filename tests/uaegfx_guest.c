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
        {
            /* Nonzero source/destination origins, interleaved plane rows,
             * and every pixel size through the selected and default callbacks. */
            UBYTE planes[16] = {0xaa,0xaa,0xcc,0xcc,0x55,0x55,0x33,0x33,
                                0xaa,0xaa,0xcc,0xcc,0x55,0x55,0x33,0x33};
            ULONG rgb[256]={0,0xff0000,0x00ff00,0xffffff};
            struct BitMap bm;
            struct PrismSurface dst={b->vram,0,size,m.bytesPerRow,640,480,m.format,bpp,PSF_VRAM};
            struct PrismPlanar src={&bm,rgb,NULL};
            __typeof__(p->bi.BlitPlanar2Chunky) host_chunky=p->bi.BlitPlanar2Chunky;
            __typeof__(p->bi.BlitPlanar2Direct) host_direct=p->bi.BlitPlanar2Direct;
            int fallback;
            memset(&bm,0,sizeof(bm));bm.BytesPerRow=4;bm.Rows=4;
            bm.Depth=2;bm.Flags=BMF_INTERLEAVED;bm.Planes[0]=planes;bm.Planes[1]=planes+2;
            for(fallback=1;fallback>=0;fallback--) {
                if(fallback) {
                    p->bi.BlitPlanar2Chunky=p->bi.BlitPlanar2ChunkyDefault;
                    p->bi.BlitPlanar2Direct=p->bi.BlitPlanar2DirectDefault;
                } else {
                    p->bi.BlitPlanar2Chunky=host_chunky;
                    p->bi.BlitPlanar2Direct=host_direct;
                }
                memset(b->vram+(12*m.bytesPerRow),0xcd,3*m.bytesPerRow);
                memset(reference+(12*m.bytesPerRow),0xcd,3*m.bytesPerRow);
                if(board_planar(b,&src,&dst,3,1,21,12,11,3,0xc0,255)!=PR_DONE) {
                    emit("FAIL planar dispatch\n");FreeVec(reference);return;
                }
                for(y=0;y<3;y++) for(x=0;x<11;x++) {
                    UBYTE bit=0x80>>((x+3)&7);
                    ULONG off=(y+1)*4+(x+3)/8;
                    UBYTE index=(!!(planes[off]&bit)) | (!!(planes[off+2]&bit)<<1);
                    ULONG c;
                    switch(m.format) {
                    case PF_CLUT8: c=index;break;
                    case PF_RGB565BE: c=(index&1?0xf800:0)|(index&2?0x07e0:0)|(index==3?31:0);break;
                    case PF_RGB565LE: c=(index&1?0x00f8:0)|(index&2?0xe007:0)|(index==3?0x1f00:0);break;
                    case PF_RGB24: case PF_ARGB32: c=rgb[index];break;
                    case PF_BGR24: c=index==1?0xff:index==2?0xff00:rgb[index];break;
                    case PF_BGRA32: c=index==1?0xff00:index==2?0xff0000:rgb[index]<<8;break;
                    case PF_RGBA32: c=rgb[index]<<8;break;
                    default: emit("FAIL planar reference format\n");FreeVec(reference);return;
                    }
                    for(k=0;k<bpp;k++) reference[(y+12)*m.bytesPerRow+(x+21)*bpp+k]=c>>(8*(bpp-k-1));
                }
                if(memcmp(reference,b->vram,size)) {
                    for(i=0;i<size;i++) if(reference[i]!=b->vram[i]) {
                        sprintf(message,"DIFF offset=%lu expected=%02x actual=%02x format=%u\n",i,reference[i],b->vram[i],m.format);emit(message);break;
                    }
                    sprintf(message,"FAIL planar %u fallback=%d\n",bpp,fallback);emit(message);
                    FreeVec(reference);return;
                }
            }
            p->bi.BlitPlanar2Chunky=host_chunky;
            p->bi.BlitPlanar2Direct=host_direct;
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
