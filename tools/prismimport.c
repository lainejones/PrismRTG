/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laine Jones */
/*
 * PrismImport - the screen sizes a Picasso96 user had switched on, taken
 * into PrismRTG's settings, so that switching keeps them.
 *
 *   PrismImport [FROM=DEVS:Picasso96Settings] [BOARD=name] [ONLY] [TEST]
 *
 * Picasso96Settings is an IFF FORM P96S: per board an STHD (the settings'
 * name) and a BDNM (the board), then per resolution an RSHD (its size and
 * whether it is switched on) and per depth an MIHD (likewise). Every size
 * and depth switched on there is switched on in ENVARC:Prism.prefs (and
 * ENV:): one of PrismRTG's own sizes where it has it, else a free custom
 * size slot. Timings are not taken - the boards' drivers time their modes
 * themselves. ONLY switches every other mode off; TEST only says what it
 * would do. BOARD picks the block for a board by name (part of it is
 * enough); without it, the block for the board PrismD drives, else the
 * first.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/semaphores.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <stdio.h>
#include <string.h>
#include "../src/prism.h"
#include "../src/prefs.h"

static const char version[] __attribute__((used)) = "$VER: PrismImport " PRISM_RELEASE " (" PRISM_RELDATE ")";

#define MAXBLOCKS 8
#define MAXRES    64

struct Res { UWORD w, h; UBYTE depths; };   /* depths: 1 = 8, 2 = 16, 4 = 24, 8 = 32 bits */
struct Block {
    char name[32], board[32];
    struct Res res[MAXRES];
    int nres;
};
static struct Block blocks[MAXBLOCKS];
static int nblocks;

static ULONG be32(const UBYTE *p) { return ((ULONG)p[0] << 24) | ((ULONG)p[1] << 16) | ((ULONG)p[2] << 8) | p[3]; }
static UWORD be16(const UBYTE *p) { return (UWORD)((p[0] << 8) | p[1]); }

static void copyname(char *d, const UBYTE *s, ULONG max, ULONG have)
{
    ULONG i;
    for (i = 0; i + 1 < max && i < have && s[i]; i++)
        d[i] = s[i];
    d[i] = 0;
}

/* the file's blocks; FALSE if it is no Picasso96Settings */
static BOOL parse(const UBYTE *d, ULONG len)
{
    ULONG p = 12;
    struct Block *b = NULL;
    struct Res *r = NULL;
    BOOL resOn = FALSE;
    if (len < 12 || memcmp(d, "FORM", 4) || memcmp(d + 8, "P96S", 4))
        return FALSE;
    while (p + 8 <= len) {
        ULONG size = be32(d + p + 4);
        const UBYTE *c = d + p + 8;
        if (p + 8 + size > len)
            break;
        if (!memcmp(d + p, "STHD", 4) && nblocks < MAXBLOCKS) {
            b = &blocks[nblocks++];
            memset(b, 0, sizeof(*b));
            if (size > 8)
                copyname(b->name, c + 8, sizeof(b->name), size - 8);
            r = NULL;
        } else if (!memcmp(d + p, "BDNM", 4) && b) {
            copyname(b->board, c, sizeof(b->board), size);
        } else if (!memcmp(d + p, "RSHD", 4) && b && size >= 14) {
            /* DisplayID, Width, Height, Active, LastSelected, Flags, Name */
            resOn = be16(c + 8) != 0;
            r = NULL;
            if (resOn && b->nres < MAXRES) {
                r = &b->res[b->nres++];
                r->w = be16(c + 4);
                r->h = be16(c + 6);
                r->depths = 0;
            }
        } else if (!memcmp(d + p, "MIHD", 4) && r && size >= 9) {
            /* OpenCount, Active, Width, Height, Depth, ... */
            UBYTE depth = c[8];
            if (be16(c + 2))
                r->depths |= depth <= 8 ? 1 : depth <= 16 ? 2 : depth <= 24 ? 4 : 8;
        }
        p += 8 + size + (size & 1);
    }
    return nblocks > 0;
}

/* "ZZ9000" in "ZZ9000 (Zorro III)", "PicassoII" in "Picasso II (GD5426)":
 * letters and digits compared, case and the rest left out */
static BOOL like(const char *part, const char *whole)
{
    char a[40], b[48];
    int i, j;
    for (i = j = 0; part[i] && j < 39; i++)
        if ((part[i] >= '0' && part[i] <= '9') || ((part[i] | 32) >= 'a' && (part[i] | 32) <= 'z'))
            a[j++] = part[i] | 32;
    a[j] = 0;
    for (i = j = 0; whole[i] && j < 47; i++)
        if ((whole[i] >= '0' && whole[i] <= '9') || ((whole[i] | 32) >= 'a' && (whole[i] | 32) <= 'z'))
            b[j++] = whole[i] | 32;
    b[j] = 0;
    return a[0] && strstr(b, a) != NULL;
}

int main(void)
{
    static const UBYTE bitsOf[4] = { 8, 16, 24, 32 };
    struct RDArgs *ra;
    LONG args[4] = { 0, 0, 0, 0 };
    const char *from = "DEVS:Picasso96Settings";
    struct PrismPrefs prefs;
    struct Block *b = NULL;
    BPTR f;
    UBYTE *data;
    LONG len;
    int i, k, on = 0, custom = 0, missed = 0;

    if (!(ra = ReadArgs((STRPTR)"FROM/K,BOARD/K,ONLY/S,TEST/S", args, NULL))) {
        PrintFault(IoErr(), (STRPTR)"PrismImport");
        return 20;
    }
    if (args[0])
        from = (const char *)args[0];
    if (!(f = Open((STRPTR)from, MODE_OLDFILE)) && !args[0])
        f = Open((STRPTR)(from = "ENVARC:Picasso96Settings"), MODE_OLDFILE);
    if (!f) {
        printf("PrismImport: no Picasso96 settings (%s)\n", from);
        FreeArgs(ra);
        return 5;
    }
    Seek(f, 0, OFFSET_END);
    len = Seek(f, 0, OFFSET_BEGINNING);
    if (len <= 0 || len > 1024 * 1024 || !(data = AllocVec(len, MEMF_ANY))) {
        Close(f);
        printf("PrismImport: %s can't be read\n", from);
        FreeArgs(ra);
        return 20;
    }
    len = Read(f, data, len);
    Close(f);
    if (len <= 0 || !parse(data, len)) {
        printf("PrismImport: %s is not a Picasso96 settings file\n", from);
        FreeVec(data);
        FreeArgs(ra);
        return 20;
    }
    FreeVec(data);

    /* which block: BOARD=, else the board PrismD drives, else the first */
    if (args[1]) {
        for (i = 0; i < nblocks && !b; i++)
            if (like((const char *)args[1], blocks[i].board) || like((const char *)args[1], blocks[i].name))
                b = &blocks[i];
        if (!b) {
            printf("PrismImport: no settings for a board \"%s\" in %s\n", (const char *)args[1], from);
            FreeArgs(ra);
            return 10;
        }
    } else {
        struct PrismSem *s;
        Forbid();
        s = (struct PrismSem *)FindSemaphore((STRPTR)PRISM_SEMNAME);
        for (i = 0; s && s->boardName && i < nblocks && !b; i++)
            if (like(blocks[i].board, s->boardName))
                b = &blocks[i];
        Permit();
        if (!b)
            b = &blocks[0];
    }
    printf("PrismImport: %s, \"%s\" for the %s (%d sizes switched on)\n", from, b->name, b->board, b->nres);

    prefs_load(&prefs, PREFS_ENVARC);
    if (args[2])
        memset(prefs.on, 0, sizeof(prefs.on));
    for (k = 0; k < b->nres; k++) {
        struct Res *r = &b->res[k];
        int slot = -1;
        UWORD w, h;
        for (i = 0; i < PREFS_NSIZES && slot < 0; i++)
            if (prefs_size(&prefs, i, &w, &h) && w == r->w && h == r->h)
                slot = i;
        if (slot < 0 && !(r->w & 7) && r->w >= 320 && r->w <= 4096 && r->h >= 200 && r->h <= 4096)
            for (i = 0; i < PREFS_NCUSTOM && slot < 0; i++)
                if (!prefs.custom[i][0]) {
                    prefs.custom[i][0] = r->w;
                    prefs.custom[i][1] = r->h;
                    slot = PREFS_NFIXED + i;
                    custom++;
                }
        printf("  %4ux%-4u", r->w, r->h);
        if (slot < 0) {
            printf(" not taken: no free custom size%s\n", r->w & 7 ? " (and its width is not a multiple of 8)" : "");
            missed++;
            continue;
        }
        if (PREFS_IS_CUSTOM(slot))
            printf(" custom size %d,", slot - PREFS_NFIXED + 1);
        for (i = 0; i < PREFS_NDEPTHS; i++)
            if (r->depths & (1 << i)) {
                prefs.on[i * PREFS_NSIZES + slot] = 1;
                printf(" %u", bitsOf[i]);
                on++;
            }
        printf(" bits\n");
    }
    if (args[3]) {
        printf("PrismImport: TEST - nothing written (%d modes, %d custom sizes)\n", on, custom);
    } else if (!prefs_save(&prefs, PREFS_ENVARC) || !prefs_save(&prefs, PREFS_ENV)) {
        printf("PrismImport: can't write the settings\n");
        FreeArgs(ra);
        return 20;
    } else
        printf("PrismImport: %d modes on%s; PrismD takes them at its next start\n", on,
               args[2] ? ", every other mode off" : "");
    FreeArgs(ra);
    return missed ? 5 : 0;
}
