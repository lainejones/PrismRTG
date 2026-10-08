/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laine Jones */
/*
 * PrismSetup - the installer's helper. The Installer script can't look at
 * expansion boards, read icon tooltypes or write a ScreenMode prefs file,
 * so this does it and answers through ENV: variables and its return code.
 *
 *   PrismSetup DETECT
 *       Which card Prism can drive, and which Picasso96 monitor in
 *       DEVS:Monitors is set up for it. Read-only. Sets
 *         ENV:PrismCard      0 none, 1 Picasso II, 2 GBAPII++,
 *                            3 ZZ9000 (Zorro III), 4 ZZ9000 (Zorro II),
 *                            5 Piccolo SD64, 6 Piccolo, 7 Spectrum 28/24
 *         ENV:PrismCardName  the card's name, or ""
 *         ENV:PrismP96Mon    the Picasso96 monitor file's name, or ""
 *       and returns the same number as PrismCard.
 *
 *       It also looks for an installed Picasso96 (see p96_scan) and sets
 *         ENV:PrismP96       how many pieces of it were found (0 = none)
 *         ENV:PrismP96List   their names, for the installer to show
 *
 *   PrismSetup P96PARK
 *       Move what starts Picasso96 (its monitor files, its API library)
 *       into SYS:Storage/Picasso96-parked
 *       and note where each came from (Prism.manifest there). Nothing is
 *       deleted. Returns 0, or 5 if a piece could not be moved.
 *
 *   PrismSetup P96RESTORE
 *       Move them back (pieces whose old place is taken stay parked).
 *
 *   PrismSetup WBMODE=<slot>
 *       Write ENVARC:Sys/screenmode.prefs (used from the next boot) for Prism mode slot
 *       <slot> (prefs.h: depth * 8 + size), 256 pens. Returns 0, or 10.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/var.h>
#include <dos/exall.h>
#include <libraries/configvars.h>
#include <workbench/workbench.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/expansion.h>
#include <proto/icon.h>
#include <string.h>
#include <stdio.h>
#include "../src/prism.h"
#include "../src/prefs.h"

static const char version[] __attribute__((used)) = "$VER: PrismSetup 1.1b3 (08.10.2026)";

struct ExpansionBase *ExpansionBase;
struct Library *IconBase;

#define TEMPLATE "DETECT/S,WBMODE/K/N,P96PARK/S,P96RESTORE/S"

static void setenv_str(const char *name, const char *val)
{
    SetVar((STRPTR)name, (STRPTR)val, -1, GVF_GLOBAL_ONLY);
}

static int lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static BOOL same_nocase(const char *a, const char *b)
{
    for (; *a && lower(*a) == lower(*b); a++, b++) ;
    return !*a && !*b;
}

/* Is this Picasso96 BOARDTYPE the driver for card `card`? */
static BOOL boardtype_matches(const char *bt, int card)
{
    if (card == 3 || card == 4)
        return same_nocase(bt, "ZZ9000");
    if (card == 1 || card == 2)
        return same_nocase(bt, "PicassoII") || same_nocase(bt, "PicassoII+") ||
               same_nocase(bt, "GBA-PII++") || same_nocase(bt, "GBAPII++");
    if (card == 5)
        return same_nocase(bt, "PiccoloSD64");
    if (card == 6)
        return same_nocase(bt, "Piccolo");
    if (card == 7)
        return same_nocase(bt, "Spectrum");
    return FALSE;
}

/* The monitor in DEVS:Monitors whose icon says BOARDTYPE=<this card>. */
static void find_p96_monitor(int card, char *out, int outLen)
{
    BPTR lock;
    struct FileInfoBlock *fib;

    out[0] = 0;
    if (!card || !(IconBase = OpenLibrary("icon.library", 37)))
        return;
    fib = AllocDosObject(DOS_FIB, NULL);
    lock = Lock("DEVS:Monitors", ACCESS_READ);
    if (fib && lock && Examine(lock, fib)) {
        while (ExNext(lock, fib) && !out[0]) {
            int n = strlen(fib->fib_FileName);
            char path[160];
            struct DiskObject *dob;
            STRPTR bt;

            if (fib->fib_DirEntryType > 0 || n < 6 || n > 100 ||
                !same_nocase(fib->fib_FileName + n - 5, ".info"))
                continue;
            sprintf(path, "DEVS:Monitors/%s", fib->fib_FileName);
            path[strlen(path) - 5] = 0;              /* GetDiskObject adds .info */
            if (!(dob = GetDiskObject(path)))
                continue;
            if (dob->do_ToolTypes && (bt = FindToolType((void *)dob->do_ToolTypes, "BOARDTYPE")) &&
                boardtype_matches(bt, card)) {
                n -= 5;                           /* omit .info */
                if (n >= outLen)
                    n = outLen - 1;
                memcpy(out, fib->fib_FileName, n);
                out[n] = 0;
            }
            FreeDiskObject(dob);
        }
    }
    if (lock) UnLock(lock);
    if (fib) FreeDosObject(DOS_FIB, fib);
    CloseLibrary(IconBase);
}

/* ---- an installed Picasso96 ---------------------------------------------
 *
 * Prism and Picasso96 cannot both be active: with Picasso96's monitors
 * loading next to Prism's an A4000 froze at the first Workbench icon. What
 * makes Picasso96 start is in a handful of places, and those are the pieces
 * the installer offers to put aside:
 *   - every monitor file in DEVS:Monitors whose icon has a BOARDTYPE
 *     tooltype (that is how Picasso96 monitors name their driver) - the one
 *     for this card and the others ("Native", "Generic", a second card).
 *     These are what load Picasso96 at boot.
 *   - LIBS:Picasso96API.library, if it is Picasso96's (ours is tiny and
 *     takes that name).
 * Everything else of Picasso96 - LIBS:Picasso96 with rtg.library and the
 * card drivers, its preferences program, its settings, its drawer - does
 * nothing without a monitor file and stays where it is. Nothing is ever
 * deleted. */
#define PARK_DIR "SYS:Storage/Picasso96-parked"
#define MANIFEST PARK_DIR "/Prism.manifest"
#define MAXPIECES 40

struct Piece { char from[120]; char to[120]; };
static struct Piece pieces[MAXPIECES];
static int npieces;

static BOOL there(const char *path)
{
    BPTR l = Lock((STRPTR)path, ACCESS_READ);
    if (l) UnLock(l);
    return l != 0;
}

static LONG size_of(const char *path)
{
    struct FileInfoBlock *fib = AllocDosObject(DOS_FIB, NULL);
    BPTR l = Lock((STRPTR)path, ACCESS_READ);
    LONG n = -1;
    if (fib && l && Examine(l, fib))
        n = fib->fib_Size;
    if (l) UnLock(l);
    if (fib) FreeDosObject(DOS_FIB, fib);
    return n;
}

static void piece(const char *from, const char *sub, const char *name)
{
    if (npieces < MAXPIECES && there(from)) {
        strcpy(pieces[npieces].from, from);
        sprintf(pieces[npieces].to, PARK_DIR "/%s/%s", sub, name);
        npieces++;
    }
}

static void p96_scan(void)
{
    BPTR lock;
    struct FileInfoBlock *fib;

    npieces = 0;
    if ((IconBase = OpenLibrary("icon.library", 37))) {
        fib = AllocDosObject(DOS_FIB, NULL);
        lock = Lock("DEVS:Monitors", ACCESS_READ);
        if (fib && lock && Examine(lock, fib)) {
            while (ExNext(lock, fib)) {
                int n = strlen(fib->fib_FileName);
                char path[160], name[110];
                struct DiskObject *dob;

                if (fib->fib_DirEntryType > 0 || n < 6 || n > 100 ||
                    !same_nocase(fib->fib_FileName + n - 5, ".info"))
                    continue;
                strcpy(name, fib->fib_FileName);
                name[n - 5] = 0;
                sprintf(path, "DEVS:Monitors/%s", name);
                if (!(dob = GetDiskObject(path)))
                    continue;
                if (dob->do_ToolTypes && FindToolType((void *)dob->do_ToolTypes, "BOARDTYPE")) {
                    char info[170], iname[120];
                    piece(path, "Monitors", name);
                    sprintf(info, "%s.info", path);
                    sprintf(iname, "%s.info", name);
                    piece(info, "Monitors", iname);
                }
                FreeDiskObject(dob);
            }
        }
        if (lock) UnLock(lock);
        if (fib) FreeDosObject(DOS_FIB, fib);
        CloseLibrary(IconBase);
    }
    /* Picasso96's API library, where ours is going to be copied: the FIRST
     * drawer of LIBS:. LIBS: is often several drawers (MUI adds its own); a
     * copy further down the list is never reached once ours is in front, and
     * may be on a volume it can't be moved from. */
    {
        BPTR first = Lock("LIBS:", ACCESS_READ), lib = Lock("LIBS:Picasso96API.library", ACCESS_READ);
        if (first && lib) {
            BPTR dir = ParentDir(lib);
            if (dir && SameLock(first, dir) == LOCK_SAME && size_of("LIBS:Picasso96API.library") > 2000)
                piece("LIBS:Picasso96API.library", "Libs", "Picasso96API.library");
            if (dir) UnLock(dir);
        }
        if (lib) UnLock(lib);
        if (first) UnLock(first);
    }
}

static void make_dir(const char *path)
{
    BPTR l = CreateDir((STRPTR)path);
    if (l) UnLock(l);
}

static int p96_park(void)
{
    BPTR mf;
    int i, failed = 0, moved = 0;

    p96_scan();
    if (!npieces) {
        printf("PrismSetup: no Picasso96 found\n");
        return 0;
    }
    make_dir("SYS:Storage");
    make_dir(PARK_DIR);
    make_dir(PARK_DIR "/Monitors");
    make_dir(PARK_DIR "/Libs");
    if (!(mf = Open(MANIFEST, MODE_READWRITE))) {
        printf("PrismSetup: can't write %s\n", MANIFEST);
        return 5;
    }
    Seek(mf, 0, OFFSET_END);
    for (i = 0; i < npieces; i++) {
        if (there(pieces[i].to)) {                /* parked before and installed again */
            printf("  %s stays: %s is in the way\n", pieces[i].from, pieces[i].to);
            failed++;
        } else if (Rename(pieces[i].from, pieces[i].to)) {
            FPrintf(mf, "%s\t%s\n", (LONG)pieces[i].to, (LONG)pieces[i].from);
            printf("  %s -> %s\n", pieces[i].from, pieces[i].to);
            moved++;
        } else {
            printf("  %s could not be moved (error %ld)\n", pieces[i].from, (long)IoErr());
            failed++;
        }
    }
    Close(mf);
    printf("PrismSetup: %d moved to %s, %d not\n", moved, PARK_DIR, failed);
    return failed ? 5 : 0;
}

static int p96_restore(void)
{
    BPTR mf;
    char line[260];
    int back = 0, left = 0;

    if (!(mf = Open(MANIFEST, MODE_OLDFILE))) {
        printf("PrismSetup: nothing parked (%s not found)\n", MANIFEST);
        return 0;
    }
    while (FGets(mf, line, sizeof(line))) {
        char *tab = strchr(line, 9), *nl = strchr(line, 10);
        if (!tab)
            continue;
        *tab++ = 0;
        if (nl) *nl = 0;
        if (!there(line))
            continue;                              /* put back by hand already */
        /* (no look first: LIBS: is often several drawers, and a file of
         * that name in another of them is not in the way - Rename fails by
         * itself if the place really is taken) */
        if (Rename(line, tab)) {
            printf("  %s -> %s\n", line, tab);
            back++;
        } else {
            printf("  %s stays parked: %s is taken\n", line, tab);
            left++;
        }
    }
    Close(mf);
    if (!left) {
        DeleteFile(MANIFEST);
        DeleteFile(PARK_DIR "/Monitors");          /* only if empty */
        DeleteFile(PARK_DIR "/Libs");
        DeleteFile(PARK_DIR);
    }
    printf("PrismSetup: %d put back, %d still in %s\n", back, left, PARK_DIR);
    return left ? 5 : 0;
}

static int detect(void)
{
    static const char *names[] = { "", "Picasso II", "GBAPII++", "ZZ9000 (Zorro III)",
                                   "ZZ9000 (Zorro II)", "Piccolo SD64", "Piccolo",
                                   "Spectrum 28/24" };
    int card = 0;
    char num[4], mon[108];

    if ((ExpansionBase = (struct ExpansionBase *)OpenLibrary("expansion.library", 37))) {
        if (FindConfigDev(NULL, 0x6d6e, 4))                                card = 3;
        else if (FindConfigDev(NULL, 0x6d6e, 3))                           card = 4;
        else if (FindConfigDev(NULL, 2167, 17) && FindConfigDev(NULL, 2167, 16)) card = 2;
        else if (FindConfigDev(NULL, 2167, 12) && FindConfigDev(NULL, 2167, 11)) card = 1;
        else if (FindConfigDev(NULL, 2195, 11) && FindConfigDev(NULL, 2195, 10)) card = 5;
        else if (FindConfigDev(NULL, 2195, 6) && FindConfigDev(NULL, 2195, 5))   card = 6;
        else if (FindConfigDev(NULL, 2193, 2) && FindConfigDev(NULL, 2193, 1))   card = 7;
        CloseLibrary((struct Library *)ExpansionBase);
    }
    {
        /* test hook for the installer script: ENV:PrismFakeCard = 1..7
         * stands in for a card this machine doesn't have */
        char fake[8];
        if (GetVar("PrismFakeCard", fake, sizeof(fake), GVF_GLOBAL_ONLY) > 0 &&
            fake[0] >= '1' && fake[0] <= '7')
            card = fake[0] - '0';
    }
    find_p96_monitor(card, mon, sizeof(mon));
    sprintf(num, "%d", card);
    setenv_str("PrismCard", num);
    setenv_str("PrismCardName", names[card]);
    setenv_str("PrismP96Mon", mon);
    {
        /* all of Picasso96, for the installer's question */
        static char list[400];
        char cnt[8];
        int i;
        p96_scan();
        list[0] = 0;
        for (i = 0; i < npieces; i++) {
            const char *n = pieces[i].from;
            int len = strlen(n);
            if (len > 5 && same_nocase(n + len - 5, ".info"))
                continue;                          /* icons go with their files */
            if (strlen(list) + len + 4 < sizeof(list)) {
                strcat(list, "  ");
                strcat(list, n);
                strcat(list, "\n");
            }
        }
        sprintf(cnt, "%d", npieces);
        setenv_str("PrismP96", cnt);
        setenv_str("PrismP96List", list);
    }
    printf("PrismSetup: card %d (%s), Picasso96 monitor \"%s\"\n", card,
           card ? names[card] : "none", mon);
    return card;
}

/* IFF PREF with one SCRM chunk (prefs/screenmode.h), 62 bytes */
static int write_wbmode(LONG slot)
{
    /* ENVARC: only - it takes effect at the next boot. Writing ENV: as
     * well makes IPrefs try to reset the Workbench screen there and then
     * ("please close all windows"), under the installer's own window. */
    static const char *files[1] = { "ENVARC:Sys/screenmode.prefs" };
    UBYTE f[62];
    ULONG id;
    UWORD w, h;
    int i, rc = 0;

    if (slot < 0 || slot >= PREFS_NMODES) {
        printf("PrismSetup: WBMODE takes a mode slot, 0 to %d\n", PREFS_NMODES - 1);
        return 10;
    }
    id = PRISM_MONITOR_ID | PREFS_IDLOW(slot);
    w = prefs_sizes[PREFS_SIZE(slot)][0];
    h = prefs_sizes[PREFS_SIZE(slot)][1];
    memset(f, 0, sizeof(f));
    memcpy(f, "FORM", 4);          f[7] = 54;
    memcpy(f + 8, "PREFPRHD", 8);  f[19] = 6;
    memcpy(f + 26, "SCRM", 4);     f[33] = 28;
    f[50] = id >> 24; f[51] = id >> 16; f[52] = id >> 8; f[53] = id;
    f[54] = w >> 8;   f[55] = w;
    f[56] = h >> 8;   f[57] = h;
    f[59] = 8;                     /* depth: 256 pens at every Prism depth */
    f[61] = 1;                     /* control: autoscroll                   */
    for (i = 0; i < 1; i++) {
        BPTR fh = Open((STRPTR)files[i], MODE_NEWFILE);
        if (!fh || Write(fh, f, sizeof(f)) != sizeof(f)) {
            printf("PrismSetup: can't write %s\n", files[i]);
            rc = 10;
        }
        if (fh) Close(fh);
    }
    if (!rc)
        printf("PrismSetup: Workbench mode set to $%08lx (%ux%u)\n", (unsigned long)id, w, h);
    return rc;
}

int main(void)
{
    LONG args[4] = { 0 };
    struct RDArgs *rda;
    int rc = 0;

    if (!(rda = ReadArgs(TEMPLATE, args, NULL))) {
        PrintFault(IoErr(), "PrismSetup");
        return 20;
    }
    if (args[1])
        rc = write_wbmode(*(LONG *)args[1]);
    else if (args[2])
        rc = p96_park();
    else if (args[3])
        rc = p96_restore();
    else
        rc = detect();
    FreeArgs(rda);
    return rc;
}
