/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Laine Jones */
/* tmpltest: BltTemplate with sparse templates (one bit, then two bits, per
 * row, walking along a diagonal) on a Prism screen, read back pixel by
 * pixel. Finds a card blitter that drops isolated template bits.
 *
 *   tmpltest [MODEID=$7a000001]
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/rdargs.h>
#include <graphics/gfx.h>
#include <graphics/rastport.h>
#include <graphics/scale.h>
#include <graphics/gfxmacros.h>
#include <cybergraphx/cybergraphics.h>
#include <proto/cybergraphics.h>

struct Library *CyberGfxBase;
#include <intuition/screens.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TW 96
#define TH 96

int main(void)
{
    LONG args[1] = { 0 };
    struct RDArgs *rda;
    struct Screen *s;
    struct RastPort *rp;
    UBYTE *t;
    ULONG id = 0x7a000001;
    int pass, x, y, bad, shown;

    if (!(rda = ReadArgs((STRPTR)"MODEID/K", args, NULL)))
        return 20;
    if (args[0])
        id = strtoul((char *)args[0] + (*(char *)args[0] == '$'), NULL, 16);
    if (!(t = AllocVec(TW / 8 * TH + 8, MEMF_CHIP | MEMF_CLEAR))) {
        FreeArgs(rda);
        return 20;
    }
    s = OpenScreenTags(NULL, SA_DisplayID, id, SA_Depth, 8, SA_Quiet, TRUE, TAG_DONE);
    if (!s) {
        printf("tmpltest: can't open mode $%08lx\n", (unsigned long)id);
        FreeVec(t);
        FreeArgs(rda);
        return 10;
    }
    rp = &s->RastPort;
    /* pass 0: one bit per row; 1: two adjacent bits; 2: the same at an odd
     * destination and a template bit offset; 3: JAM2 */
    for (pass = 0; pass < 6; pass++) {
        int dx = (pass >= 2 && pass != 4) ? 301 : 304, sbit = (pass == 2 || pass == 3) ? 3 : 0;
        memset(t, 0, TW / 8 * TH);
        for (y = 0; y < TH; y++) {
            int c = sbit + (y * 5) % (TW - 8);
            t[y * (TW / 8) + (c >> 3)] |= 0x80 >> (c & 7);
            if (pass >= 1 && pass < 4) {
                c++;
                t[y * (TW / 8) + (c >> 3)] |= 0x80 >> (c & 7);
            }
            if (pass >= 4) {
                /* runs of 1..12 bits from every column */
                int len = 1 + y % 12, k;
                for (k = 1; k < len && c + k < TW - 8; k++)
                    t[y * (TW / 8) + ((c + k) >> 3)] |= 0x80 >> ((c + k) & 7);
            }
        }
        SetRast(rp, 0);
        SetAPen(rp, 5);
        SetBPen(rp, 9);
        SetDrMd(rp, pass == 3 ? JAM2 : JAM1);
        BltTemplate(t, sbit, TW / 8, rp, dx, 100, TW - 8, TH);
        WaitBlit();
        for (bad = shown = 0, y = 0; y < TH; y++)
            for (x = 0; x < TW - 8; x++) {
                int c = sbit + x;
                int bit = (t[y * (TW / 8) + (c >> 3)] >> (7 - (c & 7))) & 1;
                LONG want = bit ? 5 : (pass == 3 ? 9 : 0), got = ReadPixel(rp, dx + x, 100 + y);
                if (got != want) {
                    bad++;
                    if (shown++ < 6)
                        printf("  pass %d: row %d col %d: want %ld got %ld\n", pass, y, x,
                               (long)want, (long)got);
                }
            }
        printf("pass %d (%s, dest x %d, template bit %d): %d wrong of %d\n", pass,
               pass == 0 ? "1 bit/row" : pass == 3 ? "2 bits/row JAM2" : pass >= 4 ? "runs of 1-12" : "2 bits/row", dx, sbit, bad,
               (TW - 8) * TH);
    }
    /* a filled triangle (AreaEnd -> BltPattern with a mask) against
     * graphics.library's own in a planar bitmap: every wrong pixel, and
     * the same triangle at a few x offsets */
    {
        static UBYTE abuf[2][5 * 8];
        static struct AreaInfo ai[2];
        static struct TmpRas tr[2];
        struct BitMap *nb = AllocBitMap(160, 100, 8, BMF_CLEAR, NULL);
        PLANEPTR r0 = AllocRaster(640, 480), r1 = AllocRaster(160, 100);
        struct RastPort nrp, *q;
        int off, k;

        if (nb && r0 && r1) {
            InitRastPort(&nrp);
            nrp.BitMap = nb;
            InitArea(&ai[0], abuf[0], 8); InitTmpRas(&tr[0], r0, RASSIZE(640, 480));
            InitArea(&ai[1], abuf[1], 8); InitTmpRas(&tr[1], r1, RASSIZE(160, 100));
            rp->AreaInfo = &ai[0]; rp->TmpRas = &tr[0];
            nrp.AreaInfo = &ai[1]; nrp.TmpRas = &tr[1];
            for (off = 300; off < 304; off++) {
                SetRast(rp, 0);
                SetRast(&nrp, 0);
                for (k = 0; k < 2; k++) {
                    int ox = k ? off : 0, oy = k ? 200 : 0;
                    q = k ? rp : &nrp;
                    SetDrMd(q, JAM2); SetAPen(q, 200); SetBPen(q, 7);
                    AreaMove(q, ox + 90, oy + 5);
                    AreaDraw(q, ox + 155, oy + 60);
                    AreaDraw(q, ox + 70, oy + 95);
                    AreaEnd(q);
                }
                WaitBlit();
                for (bad = shown = 0, y = 0; y < 100; y++)
                    for (x = 0; x < 160; x++) {
                        LONG a = ReadPixel(&nrp, x, y), b = ReadPixel(rp, off + x, 200 + y);
                        if (a != b) {
                            bad++;
                            if (shown++ < 16)
                                printf("  %d,%d: native %ld screen %ld\n", x, y, (long)a, (long)b);
                        }
                    }
                printf("triangle at x offset %d: %d wrong\n", off, bad);
            }
            rp->AreaInfo = NULL; rp->TmpRas = NULL;
        }
        if (r0) FreeRaster(r0, 640, 480);
        if (r1) FreeRaster(r1, 160, 100);
        if (nb) FreeBitMap(nb);
    }
    /* RectFill with an area pattern against graphics.library's own in a
     * planar bitmap: JAM1, JAM2, complement and inverse video, at x
     * positions that are and aren't multiples of 16, taller than one band */
    {
        static UWORD ptn[4] = { 0xf0f0, 0x8421, 0x0ff0, 0x5a5a };
        struct BitMap *nb = AllocBitMap(160, 100, 8, BMF_CLEAR, NULL);
        struct RastPort nrp, *q;
        static const UBYTE dms[] = { JAM1, JAM2, COMPLEMENT, JAM2 | INVERSVID };
        int m, k, off;

        if (nb) {
            InitRastPort(&nrp);
            nrp.BitMap = nb;
            for (m = 0; m < 4; m++)
                for (off = 320; off < 323; off += 2) {
                    SetRast(rp, 3);
                    SetRast(&nrp, 3);
                    for (k = 0; k < 2; k++) {
                        int ox = k ? off : 0, oy = k ? 200 : 0;
                        q = k ? rp : &nrp;
                        SetAfPt(q, ptn, 2);
                        SetAPen(q, 5); SetBPen(q, 9); SetDrMd(q, dms[m]);
                        RectFill(q, ox + 7, oy + 3, ox + 150, oy + 90);
                        SetAfPt(q, NULL, 0);
                    }
                    WaitBlit();
                    for (bad = shown = 0, y = 0; y < 100; y++)
                        for (x = 0; x < 160; x++) {
                            /* the pattern is anchored at the RastPort's
                             * origin: compare like against like */
                            LONG a, b = ReadPixel(rp, off + x, 200 + y);
                            if (off & 15)
                                continue;
                            a = ReadPixel(&nrp, x, y);
                            /* complement on a direct colour screen inverts
                             * the colour, not the pen: there only "changed
                             * or not" can be compared */
                            if (dms[m] == COMPLEMENT ? ((a != 3) != (b != 3)) : (a != b)) {
                                bad++;
                                if (shown++ < 6)
                                    printf("  pattern %d,%d: native %ld screen %ld\n", x, y, (long)a, (long)b);
                            }
                        }
                    if (!(off & 15))
                        printf("patterned RectFill, draw mode %d: %d wrong\n", dms[m], bad);
                }
            FreeBitMap(nb);
            SetDrMd(rp, JAM1);
        }
    }
    /* BitMapScale between two friends of the screen (a magnifier: Lupe):
     * a 16x16 piece of the screen, 4 times as wide and 3 times as high */
    {
        struct BitMap *a = AllocBitMap(16, 16, 8, 0, rp->BitMap);
        struct BitMap *b = AllocBitMap(64, 48, 8, 0, rp->BitMap);
        struct BitScaleArgs bsa;

        if (a && b) {
            /* every pen its own colour: on a 16-bit screen ReadPixel finds
             * the pen from the colour */
            for (x = 0; x < 256; x++)
                SetRGB32(&s->ViewPort, x, (ULONG)((x & 31) << 3) << 24,
                         (ULONG)((x >> 5) << 5) << 24, 0x80000000);
            SetRast(rp, 0);
            for (y = 0; y < 16; y++)
                for (x = 0; x < 16; x++) {
                    SetAPen(rp, 1 + ((x * 7 + y * 13) % 250));
                    WritePixel(rp, 40 + x, 40 + y);
                }
            BltBitMap(rp->BitMap, 40, 40, a, 0, 0, 16, 16, 0xc0, 0xff, NULL);
            memset(&bsa, 0, sizeof(bsa));
            bsa.bsa_SrcWidth = 16; bsa.bsa_SrcHeight = 16;
            bsa.bsa_XSrcFactor = 1; bsa.bsa_XDestFactor = 4;
            bsa.bsa_YSrcFactor = 1; bsa.bsa_YDestFactor = 3;
            bsa.bsa_SrcBitMap = a; bsa.bsa_DestBitMap = b;
            BitMapScale(&bsa);
            BltBitMapRastPort(b, 0, 0, rp, 300, 300, 64, 48, 0xc0);
            WaitBlit();
            for (bad = shown = 0, y = 0; y < 48; y++)
                for (x = 0; x < 64; x++) {
                    LONG want = 1 + (((x / 4) * 7 + (y / 3) * 13) % 250);
                    LONG got = ReadPixel(rp, 300 + x, 300 + y);
                    if (got != want) {
                        bad++;
                        if (shown++ < 6)
                            printf("  scale %d,%d: want %ld got %ld\n", x, y, (long)want, (long)got);
                    }
                }
            printf("BitMapScale 16x16 -> %ux%u (want 64x48): %d wrong of %d\n",
                   bsa.bsa_DestWidth, bsa.bsa_DestHeight, bad, 64 * 48);
        }
        if (a) FreeBitMap(a);
        if (b) FreeBitMap(b);
    }
    /* cybergraphics WritePixelArray in each source format, read back with
     * ReadPixelArray: 64x8 pixels of a colour ramp, a little off the left
     * edge of a long. 16-bit screens keep 5 or 6 bits a gun. */
    if ((CyberGfxBase = OpenLibrary((STRPTR)"cybergraphics.library", 40))) {
        static UBYTE src[64 * 8 * 4], back[64 * 8 * 4];
        static ULONG ctab[256];
        static const UBYTE fmts[] = { RECTFMT_RGB, RECTFMT_RGBA, RECTFMT_ARGB, RECTFMT_LUT8 };
        static const char *const fn[] = { "RGB", "RGBA", "ARGB", "LUT8 + table" };
        int f, i, tol = GetCyberMapAttr(rp->BitMap, CYBRMATTR_DEPTH) <= 8 ? 255 :
                        GetCyberMapAttr(rp->BitMap, CYBRMATTR_DEPTH) <= 16 ? 8 : 0;

        for (i = 0; i < 256; i++)
            ctab[i] = ((ULONG)i << 16) | ((ULONG)(255 - i) << 8) | ((i * 5) & 255);
        for (f = 0; f < 4 && tol < 255; f++) {
            int sb = f == 0 ? 3 : f == 3 ? 1 : 4, ro = f == 2 ? 1 : 0;
            for (i = 0; i < 64 * 8; i++) {
                UBYTE r = i & 255, g = 255 - (i & 255), b = (i * 5) & 255;
                if (f == 3) { src[i] = i & 255; continue; }
                if (sb == 4) src[i * 4] = src[i * 4 + 3] = 0;
                src[i * sb + ro] = r; src[i * sb + ro + 1] = g; src[i * sb + ro + 2] = b;
            }
            SetRast(rp, 0);
            if (f == 3)
                WriteLUTPixelArray(src, 0, 0, 64, rp, ctab, 101, 50, 64, 8, CTABFMT_XRGB8);
            else
                WritePixelArray(src, 0, 0, 64 * sb, rp, 101, 50, 64, 8, fmts[f]);
            memset(back, 0, sizeof(back));
            ReadPixelArray(back, 0, 0, 64 * 4, rp, 101, 50, 64, 8, RECTFMT_ARGB);
            for (bad = shown = 0, i = 0; i < 64 * 8; i++) {
                int r = i & 255, g = 255 - (i & 255), b = (i * 5) & 255;
                int dr = back[i * 4 + 1] - r, dg = back[i * 4 + 2] - g, db = back[i * 4 + 3] - b;
                if (dr < -tol || dr > tol || dg < -tol || dg > tol || db < -tol || db > tol) {
                    bad++;
                    if (shown++ < 4)
                        printf("  pixel %d: wrote %d,%d,%d read %d,%d,%d\n", i, r, g, b,
                               back[i * 4 + 1], back[i * 4 + 2], back[i * 4 + 3]);
                }
            }
            printf("WritePixelArray %s: %d wrong of %d\n", fn[f], bad, 64 * 8);
        }
        CloseLibrary(CyberGfxBase);
    }
    CloseScreen(s);
    FreeVec(t);
    FreeArgs(rda);
    return 0;
}
