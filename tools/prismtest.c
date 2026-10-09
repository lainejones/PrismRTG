/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laine Jones */
/*
 * PrismTest - a board driver on its own, without PrismD.
 *
 * Loads a driver module (PROGDIR:Drivers/NAME.driver, then
 * LIBS:Prism/NAME.driver), sets a mode, draws a test pattern, shows it
 * for a while, then gives the display back:
 *
 *   PrismTest [BOARD=NAME] [WIDTH=640] [HEIGHT=480] [DEPTH=8]
 *             [SECS=10] [OFFSET=<vram offset>] [BLIT] [DUMP]
 *
 * Without BOARD it tries PICASSO2, ZZ9000 and then every other driver
 * installed. BLIT checks the driver's fills and copies against the CPU.
 * This is the first thing to run on a new driver (docs/writing-a-driver.md).
 * Ctrl-C ends the display early. The VRAM it draws into is saved first and
 * put back afterwards, and the card's own state is restored, so Picasso96
 * (if running) finds its screen the way it left it. PrismD must not be
 * running: two owners of one card.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <intuition/intuition.h>
#include <graphics/modeid.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/expansion.h>
#include <string.h>
#include <stdio.h>
#include "prismboard.h"
#include "driver_module.h"

struct ExpansionBase *ExpansionBase;
struct IntuitionBase *IntuitionBase;

static const char version[] __attribute__((used)) = "$VER: PrismTest 1.1b3 (08.10.2026)";

#define TEMPLATE "BOARD/K,WIDTH/K/N,HEIGHT/K/N,DEPTH/K/N,SECS/K/N,OFFSET/K,DUMP/S,BLIT/S"
enum { A_BOARD, A_WIDTH, A_HEIGHT, A_DEPTH, A_SECS, A_OFFSET, A_DUMP, A_BLIT, A_COUNT };

static struct PrismBoard board;

/* ---- pixel helpers -------------------------------------------------- */

static ULONG pack_rgb(UBYTE f, UBYTE r, UBYTE g, UBYTE b)
{
    switch (f) {
    case PF_RGB565BE: return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
    case PF_RGB565LE: { UWORD v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
                        return ((v & 0xff) << 8) | (v >> 8); }
    case PF_RGB555BE: return ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3);
    case PF_RGB555LE: { UWORD v = ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3);
                        return ((v & 0xff) << 8) | (v >> 8); }
    case PF_BGR565LE: { UWORD v = ((b >> 3) << 11) | ((g >> 2) << 5) | (r >> 3);
                        return ((v & 0xff) << 8) | (v >> 8); }
    case PF_BGR555LE: { UWORD v = ((b >> 3) << 10) | ((g >> 3) << 5) | (r >> 3);
                        return ((v & 0xff) << 8) | (v >> 8); }
    case PF_RGBA32:   return ((ULONG)r << 24) | ((ULONG)g << 16) | ((ULONG)b << 8);
    case PF_RGB24:    return ((ULONG)r << 16) | ((ULONG)g << 8) | b;
    case PF_BGR24:    return ((ULONG)b << 16) | ((ULONG)g << 8) | r;  /* bytes B,G,R */
    case PF_ARGB32:   return ((ULONG)r << 16) | ((ULONG)g << 8) | b;
    case PF_BGRA32:   return ((ULONG)b << 24) | ((ULONG)g << 16) | ((ULONG)r << 8);
    }
    return 0;
}

static int bytes_pp(UBYTE f)
{
    if (f == PF_CLUT8) return 1;
    if (f == PF_RGB24 || f == PF_BGR24) return 3;
    if (f >= PF_ARGB32) return 4;
    return 2;
}

static void put(UBYTE *row, int x, int bpp, ULONG c)
{
    switch (bpp) {
    case 1: row[x] = c; break;
    case 2: ((UWORD *)row)[x] = c; break;
    case 3: row[x * 3] = c >> 16; row[x * 3 + 1] = c >> 8; row[x * 3 + 2] = c; break;
    case 4: ((ULONG *)row)[x] = c; break;
    }
}

/* Colour of a "hue" 0..255 around the colour wheel. */
static void hue(int h, UBYTE *r, UBYTE *g, UBYTE *b)
{
    int s = (h % 43) * 6, seg = h / 43;
    UBYTE up = s > 255 ? 255 : s, dn = 255 - up;
    switch (seg) {
    case 0:  *r = 255; *g = up;  *b = 0;   break;
    case 1:  *r = dn;  *g = 255; *b = 0;   break;
    case 2:  *r = 0;   *g = 255; *b = up;  break;
    case 3:  *r = 0;   *g = dn;  *b = 255; break;
    case 4:  *r = up;  *g = 0;   *b = 255; break;
    default: *r = 255; *g = 0;   *b = dn;  break;
    }
}

/* 8-bit palette: 0 black, 1 white, 2..129 hue wheel, 130..255 grey ramp. */
static void make_palette(UBYTE *pal)
{
    int i;
    pal[0] = pal[1] = pal[2] = 0;
    pal[3] = pal[4] = pal[5] = 255;
    for (i = 0; i < 128; i++)
        hue(i * 2, &pal[(2 + i) * 3], &pal[(2 + i) * 3 + 1], &pal[(2 + i) * 3 + 2]);
    for (i = 0; i < 126; i++)
        pal[(130 + i) * 3] = pal[(130 + i) * 3 + 1] = pal[(130 + i) * 3 + 2] = i * 255 / 125;
}

/* Test pattern: hue bars on top, grey ramp in the middle, checkerboard at
 * the bottom, white border and diagonals. Any shear, wrong stride or wrong
 * byte order shows at a glance. */
static void draw_pattern(UBYTE *fb, ULONG bpr, int w, int h, UBYTE f)
{
    int bpp = bytes_pp(f), x, y;
    ULONG white = (f == PF_CLUT8) ? 1 : pack_rgb(f, 255, 255, 255);
    ULONG black = (f == PF_CLUT8) ? 0 : pack_rgb(f, 0, 0, 0);

    for (y = 0; y < h; y++) {
        UBYTE *row = fb + y * bpr;
        for (x = 0; x < w; x++) {
            ULONG c;
            if (y < h / 3) {
                int hh = x * 255 / w;
                if (f == PF_CLUT8) c = 2 + hh / 2;
                else { UBYTE r, g, b; hue(hh, &r, &g, &b); c = pack_rgb(f, r, g, b); }
            } else if (y < 2 * h / 3) {
                int v = x * 255 / w;
                c = (f == PF_CLUT8) ? 130 + v * 125 / 255 : pack_rgb(f, v, v, v);
            } else {
                c = (((x >> 4) ^ (y >> 4)) & 1) ? white : black;
            }
            if (x == 0 || y == 0 || x == w - 1 || y == h - 1 ||
                x * h == y * w || (w - 1 - x) * h == y * w)
                c = white;
            put(row, x, bpp, c);
        }
    }
}

/* Make Picasso96 (if present) reprogram the card: a native screen in front
 * switches the monitor to native, closing it switches back and P96 redoes
 * its mode + display start. Harmless when no RTG system is running. */
static void kick_native_and_back(void)
{
    struct Screen *s;
    if (!IntuitionBase) return;
    s = OpenScreenTags(NULL, SA_DisplayID, LORES_KEY, SA_Depth, 1,
                       SA_Width, 320, SA_Height, 200, SA_Quiet, TRUE,
                       SA_ShowTitle, FALSE, TAG_DONE);
    if (s) {
        Delay(50);
        CloseScreen(s);
    }
}

int main(void)
{
    LONG args[A_COUNT] = { 0 };
    struct RDArgs *rda;
    struct PrismMode mode;
    ULONG off = 0, size, secs = 10, i, limit;
    UBYTE *save = NULL, *fb;
    UBYTE pal[256 * 3];
    int depth = 8, rc = 20;
    BOOL found = FALSE;
    const char *want;

    setvbuf(stdout, NULL, _IONBF, 0);
    if (!(rda = ReadArgs(TEMPLATE, args, NULL))) {
        PrintFault(IoErr(), "PrismTest");
        return 20;
    }
    ExpansionBase = (struct ExpansionBase *)OpenLibrary("expansion.library", 37);
    IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 37);
    if (!ExpansionBase) goto out;

    if (FindSemaphore((STRPTR)"prism")) {          /* PRISM_SEMNAME, prism.h */
        printf("PrismTest: PrismD is running and owns the board - stop it first (Ctrl-C)\n");
        goto out;
    }
    want = args[A_BOARD] ? (const char *)args[A_BOARD] : NULL;
    {
        struct PrismDriverConfig config;
        memset(&config, 0, sizeof(config));
        if (want)
            found = driver_open(&board, want, &config);
        else {
            found = driver_open(&board, "PICASSO2", &config);
            if (!found) found = driver_open(&board, "ZZ9000", &config);
            if (!found) found = driver_scan(&board, &config);
        }
    }
    if (!found) {
        printf("PrismTest: no usable board found%s%s\n", want ? " for " : "", want ? want : "");
        rc = 5;
        goto out;
    }
    board_defaults(&board);
    printf("PrismTest: driver %s: formats $%lx flags $%lx, %s%s%s%s%s\n", board.name,
           (unsigned long)board.formats, (unsigned long)board.flags,
           board.ops ? "ops " : "", board.fillRect ? "fillRect " : "",
           board.copyRect ? "copyRect " : "", board.expandRect ? "expandRect " : "",
           board.cursorImage ? "cursor" : "no cursor");

    memset(&mode, 0, sizeof(mode));
    mode.width  = args[A_WIDTH]  ? *(LONG *)args[A_WIDTH]  : 640;
    mode.height = args[A_HEIGHT] ? *(LONG *)args[A_HEIGHT] : 480;
    if (args[A_DEPTH]) depth = *(LONG *)args[A_DEPTH];
    if (args[A_SECS])  secs  = *(LONG *)args[A_SECS];
    switch (depth) {
    case 8:  mode.format = PF_CLUT8; break;
    case 15: mode.format = (board.formats & PF_BIT(PF_RGB555BE)) ? PF_RGB555BE :
                           (board.formats & PF_BIT(PF_BGR555LE)) ? PF_BGR555LE : PF_RGB555LE; break;
    case 16: mode.format = (board.formats & PF_BIT(PF_RGB565BE)) ? PF_RGB565BE :
                           (board.formats & PF_BIT(PF_BGR565LE)) ? PF_BGR565LE : PF_RGB565LE; break;
    case 24: mode.format = (board.formats & PF_BIT(PF_BGR24)) ? PF_BGR24 : PF_RGB24; break;
    case 32: mode.format = (board.formats & PF_BIT(PF_BGRA32)) ? PF_BGRA32 :
                           (board.formats & PF_BIT(PF_RGBA32)) ? PF_RGBA32 : PF_ARGB32; break;
    default: printf("PrismTest: DEPTH must be 8, 15, 16, 24 or 32\n"); goto out;
    }
    if (!(board.formats & PF_BIT(mode.format))) {
        printf("PrismTest: %s can't show %d-bit\n", board.name, depth);
        goto out;
    }

    /* Default test framebuffer: well clear of where P96 puts its screens on
     * the ZZ9000; the start of VRAM on the 2 MB Picasso II. */
    off = 0;
    limit = board.vramSize;
    if (board.configDev && board.configDev->cd_Rom.er_Manufacturer == 0x6d6e &&
        board.configDev->cd_Rom.er_Product == 4) {
        /* Zorro III ZZ9000: the card window runs on past what Prism uses,
         * up to the firmware's SDK heap. $2000000 is above the capture
         * buffer and far above P96's screens (it fills from the bottom). */
        off = 0x2000000;
        limit = 0x2e00000;
    }
    if (args[A_OFFSET]) {
        const char *s = (const char *)args[A_OFFSET];
        if (s[0] == '$') s++;
        else if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
        off = 0;
        for (; *s; s++)
            off = off * 16 + ((*s <= '9') ? *s - '0' : ((*s | 0x20) - 'a' + 10));
    }

    printf("PrismTest: %s regs $%08lx vram $%08lx (%lu KB)\n", board.name,
           (unsigned long)board.regs, (unsigned long)board.vram,
           (unsigned long)(board.vramSize >> 10));
    if (board.setMode == NULL) goto out;

    size = (ULONG)mode.width * mode.height * bytes_pp(mode.format);
    if (off + size > limit) {
        printf("PrismTest: mode needs %lu bytes at $%lx, VRAM is %lu\n",
               (unsigned long)size, (unsigned long)off, (unsigned long)limit);
        goto out;
    }
    if (!(save = AllocVec(size, MEMF_ANY))) {
        printf("PrismTest: no memory to save %lu bytes of VRAM\n", (unsigned long)size);
        goto out;
    }
    fb = board.vram + off;
    CopyMemQuick(fb, save, size & ~3);

    if (board.saveState) board.saveState(&board);

    /* The ZZ9000's memory can be drawn into before the mode is set, so the
     * finished picture is what appears; otherwise the card shows whatever
     * is at VRAM+0 (P96's screen, at the wrong size) while we draw. */
    if (limit != board.vramSize && board.checkMode(&board, &mode)) {
        draw_pattern(fb, mode.bytesPerRow, mode.width, mode.height, mode.format);
        board.setDisplayStart(&board, off);
    }

    if (!board.setMode(&board, &mode)) {
        printf("PrismTest: %s rejected %dx%d %d-bit\n", board.name, mode.width, mode.height, depth);
        if (board.restoreState) board.restoreState(&board);
        goto out;
    }
    printf("PrismTest: %dx%d %d-bit, %lu bytes/row, framebuffer at VRAM+$%lx\n",
           mode.width, mode.height, depth, (unsigned long)mode.bytesPerRow, (unsigned long)off);
    if (mode.format == PF_CLUT8) {
        make_palette(pal);
        board.setPalette(&board, 0, 256, pal);
    }
    draw_pattern(fb, mode.bytesPerRow, mode.width, mode.height, mode.format);
    if (args[A_BLIT] && !((board.fillRect && board.copyRect) ||
                          (board.ops && board.ops->fill && board.ops->copy)))
        printf("PrismTest: the driver has no blitter to check\n");
    if (args[A_BLIT] && ((board.fillRect && board.copyRect) ||
                         (board.ops && board.ops->fill && board.ops->copy))) {
        /* Blitter check, all inside the test pattern:
         *  1. three solid boxes across the hue bars (red, green, blue)
         *  2. copy the left of the grey ramp into the checkerboard
         *  3. overlapping copy: shift a hue strip 24 px right in place */
        int bw = mode.width / 6, bh = mode.height / 8, k;
        UBYTE bpp = bytes_pp(mode.format);
        ULONG red, grn, blu, bad = 0, first = 0, last = 0;
        UBYTE *shadow = AllocVec(size, MEMF_ANY);
        struct { int x, y, w, h; ULONG c; } fills[3];
        struct { int sx, sy, dx, dy, w, h; } copies[2];

        if (mode.format == PF_CLUT8) { red = 2; grn = 2 + 42; blu = 2 + 85; }
        else { red = pack_rgb(mode.format, 255, 0, 0); grn = pack_rgb(mode.format, 0, 255, 0);
               blu = pack_rgb(mode.format, 0, 0, 255); }
        fills[0].x = bw / 2;     fills[1].x = bw * 2;  fills[2].x = bw * 7 / 2;
        fills[0].c = red;        fills[1].c = grn;     fills[2].c = blu;
        for (k = 0; k < 3; k++) { fills[k].y = bh / 2; fills[k].w = bw; fills[k].h = bh; }
        copies[0].sx = 8; copies[0].sy = mode.height / 3 + 8;
        copies[0].dx = mode.width / 2; copies[0].dy = mode.height * 2 / 3 + 16;
        copies[0].w = mode.width / 3; copies[0].h = mode.height / 6;
        copies[1].sx = 0; copies[1].sy = mode.height / 4;
        copies[1].dx = 24; copies[1].dy = mode.height / 4;
        copies[1].w = mode.width - 32; copies[1].h = 16;

        /* the surface the PrismOps path sees: the whole test framebuffer */
        struct PrismSurface sf;
        memset(&sf, 0, sizeof(sf));
        sf.memory = fb; sf.offset = off; sf.allocation = size; sf.pitch = mode.bytesPerRow;
        sf.width = mode.width; sf.height = mode.height; sf.format = mode.format;
        sf.bpp = bpp; sf.flags = PSF_VRAM;

        if (shadow)
            CopyMem(fb, shadow, size);
        /* the 5426/28 can't colour-expand at 24 bits (Prism fills those by
         * row doubling): only copies are tested there */
        for (k = 0; k < 3 && bpp != 3; k++) {
            enum PrismResult r = PR_DONE;
            if (board.fillRect)
                board.fillRect(&board, off, mode.bytesPerRow, bpp, fills[k].x, fills[k].y,
                               fills[k].w, fills[k].h, fills[k].c);
            else
                r = board.ops->fill(&board, &sf, fills[k].x, fills[k].y, fills[k].w, fills[k].h, fills[k].c);
            if (r != PR_DONE)
                printf("PrismTest: fill %d: the driver answered %d (1 = done)\n", k, (int)r);
        }
        for (k = 0; k < 2; k++) {
            enum PrismResult r = PR_DONE;
            if (board.copyRect)
                board.copyRect(&board, off, mode.bytesPerRow, bpp, copies[k].sx, copies[k].sy,
                               copies[k].dx, copies[k].dy, copies[k].w, copies[k].h);
            else
                r = board.ops->copy(&board, &sf, &sf, copies[k].sx, copies[k].sy,
                                    copies[k].dx, copies[k].dy, copies[k].w, copies[k].h);
            if (r != PR_DONE)
                printf("PrismTest: copy %d: the driver answered %d (1 = done)\n", k, (int)r);
        }
        if (board.waitBlit) board.waitBlit(&board);
        printf("PrismTest: blitter fills + copies done\n");

        /* the same operations on the shadow copy by the CPU, then compare */
        if (shadow) {
            int x, y;
            ULONG bpr = mode.bytesPerRow;
            for (k = 0; k < 3 && bpp != 3; k++)
                for (y = fills[k].y; y < fills[k].y + fills[k].h; y++)
                    for (x = fills[k].x; x < fills[k].x + fills[k].w; x++) {
                        UBYTE *q = shadow + y * bpr + x * bpp;
                        if (bpp == 1)      *q = fills[k].c;
                        else if (bpp == 2) *(UWORD *)q = fills[k].c;
                        else               *(ULONG *)q = fills[k].c;
                    }
            for (k = 0; k < 2; k++) {
                int h = copies[k].h, sy = copies[k].sy, dy = copies[k].dy;
                ULONG n = (ULONG)copies[k].w * bpp;
                for (y = 0; y < h; y++) {
                    int yy = (dy > sy) ? h - 1 - y : y;
                    memmove(shadow + (dy + yy) * bpr + copies[k].dx * bpp,
                            shadow + (sy + yy) * bpr + copies[k].sx * bpp, n);
                }
            }
            for (i = 0; i < size; i++)
                if (fb[i] != shadow[i]) {
                    if (!bad++)
                        first = i;
                    last = i;
                }
            if (bad) {
                /* per row: how many bytes differ, and the first one */
                ULONG ry, rx, n, fx;
                for (ry = first / bpr; ry <= last / bpr; ry++) {
                    for (rx = 0, n = 0, fx = 0; rx < bpr; rx++)
                        if (fb[ry * bpr + rx] != shadow[ry * bpr + rx] && !n++)
                            fx = rx;
                    if (n)
                        printf("  row %lu: %lu bytes differ from byte %lu (card %02x %02x, expected %02x %02x)\n",
                               (unsigned long)ry, (unsigned long)n, (unsigned long)fx,
                               fb[ry * bpr + fx], fb[ry * bpr + fx + 1],
                               shadow[ry * bpr + fx], shadow[ry * bpr + fx + 1]);
                }
            }
            if (bad)
                printf("PrismTest: wrong bytes run from x=%lu y=%lu to x=%lu y=%lu\n",
                       (unsigned long)(first % bpr / bpp), (unsigned long)(first / bpr),
                       (unsigned long)(last % bpr / bpp), (unsigned long)(last / bpr));
            if (bad)
                printf("PrismTest: blitter check: %lu bytes WRONG, first at x=%lu y=%lu "
                       "(card %02x, expected %02x)\n", (unsigned long)bad,
                       (unsigned long)(first % bpr / bpp), (unsigned long)(first / bpr),
                       fb[first], shadow[first]);
            else
                printf("PrismTest: blitter check: all %lu bytes match the CPU's result\n",
                       (unsigned long)size);
            FreeVec(shadow);
        }
    }
    board.setDisplayStart(&board, off);
    board.setSwitch(&board, TRUE);
    if (args[A_DUMP]) {
        printf("VRAM readback:");
        for (i = 0; i < 8; i++) printf(" %02x", fb[i]);
        printf(" ... row 200:");
        for (i = 0; i < 8; i++) printf(" %02x", fb[200 * mode.bytesPerRow + i]);
        printf("\n");
    }

    printf("PrismTest: showing for %lu s (Ctrl-C to stop)\n", (unsigned long)secs);
    for (i = 0; i < secs * 10; i++) {
        if (SetSignal(0, 0) & SIGBREAKF_CTRL_C) {
            SetSignal(0, SIGBREAKF_CTRL_C);
            break;
        }
        Delay(5);
    }

    board.setSwitch(&board, FALSE);
    if (board.restoreState) board.restoreState(&board);
    CopyMemQuick(save, fb, size & ~3);
    kick_native_and_back();
    printf("PrismTest: display handed back\n");
    rc = 0;

out:
    driver_close();
    if (save) FreeVec(save);
    if (IntuitionBase) CloseLibrary((struct Library *)IntuitionBase);
    if (ExpansionBase) CloseLibrary((struct Library *)ExpansionBase);
    FreeArgs(rda);
    return rc;
}
