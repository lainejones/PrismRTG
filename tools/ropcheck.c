/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Laine Jones */
/* ropcheck MODEID=<id> [W=<n>] [H=<n>] : BltBitMap with every minterm between two
 * bitmaps made with the screen's bitmap as friend (on a card: both in VRAM, so a
 * driver with the rop operation blits them on the card), then the destination is
 * read back and compared with the same minterm done here, bitwise on the pixel
 * data (what the Amiga blitter, Picasso96 and PrismRTG's CPU path all do).
 * Odd offsets and an odd width on purpose: the drivers blit in bytes. Minterms
 * 0x00 and 0xF0 are left out (PrismRTG fills those with pens, not bits).
 * Prints the time of each blit too: the card is far quicker than the CPU path.
 * The two bitmaps go to VRAM only if it has room beside every open screen
 * (off-screen bitmaps never evict): on a 2 MB card at 800x600 16-bit with a
 * 16-bit Workbench there is none, and the blits are done by the CPU - right,
 * but slow. A smaller MODEID (640x480) leaves room.
 * Returns 0 when every pixel of every minterm is right, 10 otherwise. */
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/rdargs.h>
#include <graphics/gfx.h>
#include <cybergraphx/cybergraphics.h>
#include <proto/cybergraphics.h>
#include <intuition/screens.h>
#include <devices/timer.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <proto/timer.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

struct Library *CyberGfxBase;
struct Device *TimerBase;

/* one line to the log file, opened, written and closed at once */
static void logline(const char *name, const char *fmt, ...)
{
    char buf[96]; va_list ap; BPTR f;
    if (!name) return;
    va_start(ap, fmt); vsprintf(buf, fmt, ap); va_end(ap);
    strcat(buf, "\n");
    if ((f = Open((STRPTR)name, MODE_READWRITE))) {
        Seek(f, 0, OFFSET_END);
        Write(f, buf, strlen(buf));
        Close(f);
    }
}

static ULONG seed = 0x12345678;
static UBYTE rnd(void) { seed = seed * 1103515245 + 12345; return (UBYTE)(seed >> 16); }

/* fill a bitmap's w x h pixels with a known pattern through LockBitMapTags */
static BOOL fill(struct BitMap *bm, UWORD w, UWORD h, ULONG s0)
{
    APTR base; ULONG bpr = 0, bpp = 0; UBYTE *mem = NULL; UWORD y, x;
    base = LockBitMapTags(bm, LBMI_BASEADDRESS, (ULONG)&mem, LBMI_BYTESPERROW, (ULONG)&bpr,
                          LBMI_BYTESPERPIX, (ULONG)&bpp, TAG_DONE);
    if (!base || !mem) return FALSE;
    seed = s0;
    for (y = 0; y < h; y++)
        for (x = 0; x < w * bpp; x++)
            mem[(ULONG)y * bpr + x] = rnd();
    UnLockBitMap(base);
    return TRUE;
}

static BOOL grab(struct BitMap *bm, UWORD w, UWORD h, UBYTE *out, ULONG *bppOut)
{
    APTR base; ULONG bpr = 0, bpp = 0; UBYTE *mem = NULL; UWORD y;
    base = LockBitMapTags(bm, LBMI_BASEADDRESS, (ULONG)&mem, LBMI_BYTESPERROW, (ULONG)&bpr,
                          LBMI_BYTESPERPIX, (ULONG)&bpp, TAG_DONE);
    if (!base || !mem) return FALSE;
    for (y = 0; y < h; y++)
        CopyMem(mem + (ULONG)y * bpr, out + (ULONG)y * w * bpp, (ULONG)w * bpp);
    UnLockBitMap(base);
    *bppOut = bpp;
    return TRUE;
}

static UBYTE apply(UBYTE mt, UBYTE s, UBYTE d)
{
    UBYTE r = 0;
    if (mt & 0x80) r |= s & d;
    if (mt & 0x40) r |= s & ~d;
    if (mt & 0x20) r |= ~s & d;
    if (mt & 0x10) r |= ~s & ~d;
    return r;
}

static ULONG ms(void)
{
    struct timeval tv;
    GetSysTime(&tv);
    return tv.tv_secs * 1000 + tv.tv_micro / 1000;
}

int main(void)
{
    LONG args[5] = { 0 };
    struct RDArgs *rda;
    struct Screen *s;
    struct BitMap *a, *b;
    struct timerequest tr;
    ULONG id, bpp = 0, n, bad = 0, total = 0;
    unsigned long idl;
    UWORD W = 400, H = 120, w, h;
    const WORD sx = 3, sy = 2, dx = 5, dy = 1;
    UBYTE *sa, *sb, *got;
    int mt, rc = 0, only = -1;
    unsigned long onlyl;
    char *logname = NULL;

    if (!(rda = ReadArgs((STRPTR)"MODEID/K,W/K/N,H/K/N,LOG/K,MINTERM/K", args, NULL))) { PrintFault(IoErr(), NULL); return 20; }
    /* LOG=<file>: a line appended and the file closed before and after every
     * blit, so after a machine freeze the file says which minterm it was in
     * (issue #23). MINTERM=$xx: that one minterm only. */
    logname = (char *)args[3];
    if (args[4] && sscanf((char *)args[4], "$%lx", &onlyl) == 1) only = (int)(onlyl & 0xf0);
    if (!args[0] || sscanf((char *)args[0], "$%lx", &idl) != 1) { puts("ropcheck: MODEID=$7A000102"); return 20; }
    id = idl;
    if (args[1]) W = *(LONG *)args[1];
    if (args[2]) H = *(LONG *)args[2];
    w = W - 9; h = H - 5;                          /* odd width, away from the edges */
    if (!(CyberGfxBase = OpenLibrary((STRPTR)"cybergraphics.library", 40))) { puts("no cybergraphics"); return 20; }
    if (OpenDevice((STRPTR)"timer.device", UNIT_MICROHZ, (struct IORequest *)&tr, 0)) return 20;
    TimerBase = tr.tr_node.io_Device;
    s = OpenScreenTags(NULL, SA_DisplayID, id, SA_Depth, 8, SA_Quiet, TRUE, SA_ShowTitle, FALSE, TAG_DONE);
    if (!s) { printf("ropcheck: can't open $%08lx\n", (unsigned long)id); return 20; }
    a = AllocBitMap(W, H, 8, 0, s->RastPort.BitMap);
    b = AllocBitMap(W, H, 8, 0, s->RastPort.BitMap);
    n = (ULONG)W * H * 4;
    sa = AllocVec(n, MEMF_ANY); sb = AllocVec(n, MEMF_ANY); got = AllocVec(n, MEMF_ANY);
    if (!a || !b || !sa || !sb || !got || !fill(a, W, H, 1) || !grab(a, W, H, sa, &bpp)) {
        puts("ropcheck: no bitmaps / no LockBitMap");
        rc = 20;
        goto out;
    }
    printf("ropcheck $%08lx: %lu bytes per pixel, %ux%u blits\n", (unsigned long)id, (unsigned long)bpp, w, h);
    for (mt = 0x10; mt <= 0xe0; mt += 0x10) {
        ULONG t0, t1, wrong = 0, x, y;
        if (only >= 0 && mt != only) continue;
        logline(logname, "start minterm $%02x", mt | 0x0a);
        fill(a, W, H, 1);
        fill(b, W, H, 2);
        grab(b, W, H, sb, &bpp);
        t0 = ms();
        BltBitMap(a, sx, sy, b, dx, dy, w, h, (UBYTE)(mt | 0x0a), 0xff, NULL);
        WaitBlit();
        t1 = ms();
        logline(logname, "done  minterm $%02x: %lu ms", mt | 0x0a, (unsigned long)(t1 - t0));
        grab(b, W, H, got, &bpp);
        for (y = 0; y < H; y++)
            for (x = 0; x < (ULONG)W * bpp; x++) {
                ULONG k = y * W * bpp + x, px = x / bpp;
                UBYTE want = sb[k];
                if (px >= (ULONG)dx && px < (ULONG)dx + w && y >= (ULONG)dy && y < (ULONG)dy + h)
                    want = apply((UBYTE)mt, sa[(y - dy + sy) * W * bpp + (px - dx + sx) * bpp + x % bpp], sb[k]);
                if (got[k] != want) wrong++;
            }
        printf("  minterm $%02x: %lu ms, %lu wrong bytes\n", mt | 0x0a, (unsigned long)(t1 - t0), (unsigned long)wrong);
        bad += wrong; total++;
    }
    printf("ropcheck: %s (%lu minterms)\n", bad ? "FAILED" : "all right", (unsigned long)total);
    rc = bad ? 10 : 0;
out:
    if (a) FreeBitMap(a);
    if (b) FreeBitMap(b);
    FreeVec(sa); FreeVec(sb); FreeVec(got);
    CloseScreen(s);
    CloseDevice((struct IORequest *)&tr);
    CloseLibrary(CyberGfxBase);
    FreeArgs(rda);
    return rc;
}
