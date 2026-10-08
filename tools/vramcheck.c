/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Laine Jones */
/* vramcheck [modeid] [reps]: fill VRAM with 5A, CopyMem from fast RAM, check - many times;
 * tells lost copy writes (bytes still 5A) from other damage. Also the same with a
 * CPU long-word loop, and CopyMem into fast RAM, for comparison. */
#include <exec/types.h>
#include <exec/memory.h>
#include <intuition/screens.h>
#include <cybergraphx/cybergraphics.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/cybergraphics.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct IntuitionBase *IntuitionBase;
struct Library *CyberGfxBase;

static int check(const UBYTE *src, const UBYTE *d, int n, int *still, int *first)
{
    int j, bad = 0;
    *still = 0;
    *first = -1;
    for (j = 0; j < n; j++)
        if (d[j] != src[j]) {
            bad++;
            if (d[j] == 0x5A) (*still)++;
            if (*first < 0) *first = j;
        }
    return bad;
}

static void longcopy(const UBYTE *s, UBYTE *d, int n)
{
    const ULONG *a = (const ULONG *)s;
    ULONG *b = (ULONG *)d;
    int i;
    for (i = 0; i < n / 4; i++)
        b[i] = a[i];
}

int main(int argc, char **argv)
{
    ULONG id = argc > 1 ? strtoul(argv[1][0] == '$' ? argv[1] + 1 : argv[1], NULL, 16) : 0x7A000301;
    int reps = argc > 2 ? atoi(argv[2]) : 8;
    struct Screen *s;
    ULONG base = 0, bpr = 0;
    UBYTE *src, *fast, *d;
    APTR h;
    int i, n, rep, how;

    IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 39);
    CyberGfxBase = OpenLibrary("cybergraphics.library", 40);
    src = AllocVec(8192, MEMF_ANY);
    fast = AllocVec(8192, MEMF_ANY);
    for (i = 0; i < 8192; i++)
        src[i] = (UBYTE)(i * 7 + 3);
    if (!(s = OpenScreenTags(NULL, SA_DisplayID, id, SA_Depth, 8, SA_Quiet, TRUE, TAG_END))) {
        puts("no screen");
        return 10;
    }
    h = LockBitMapTags(s->RastPort.BitMap, LBMI_BASEADDRESS, (ULONG)&base, LBMI_BYTESPERROW, (ULONG)&bpr, TAG_END);
    for (how = 0; how < 3; how++) {
        int tot = 0, runs = 0, shown = 0;
        const char *name = how == 0 ? "CopyMem into VRAM" : how == 1 ? "long loop into VRAM" : "CopyMem into fast RAM";
        d = how == 2 ? fast : (UBYTE *)base + 200 * bpr;
        for (rep = 0; rep < reps; rep++)
            for (n = 256; n <= 4096; n *= 2) {
                int still, first, bad;
                memset(d, 0x5A, n);
                if (how == 1)
                    longcopy(src, d, n);
                else
                    CopyMem(src, d, n);
                runs++;
                if ((bad = check(src, d, n, &still, &first))) {
                    tot++;
                    if (shown++ < 4) {
                        int q = first & ~3, k;
                        printf("  %s, %4d bytes: %4d wrong from offset %d, %d of them still 5A\n",
                               name, n, bad, first, still);
                        for (k = 0; k < 3; k++, q += 4)
                            printf("     +%d wrote %02x %02x %02x %02x  read %02x %02x %02x %02x\n", q,
                                   src[q], src[q+1], src[q+2], src[q+3], d[q], d[q+1], d[q+2], d[q+3]);
                    }
                }
            }
        printf("%-22s %d of %d copies wrong\n", name, tot, runs);
    }
    if (h)
        UnLockBitMap(h);
    CloseScreen(s);
    FreeVec(src);
    FreeVec(fast);
    return 0;
}
