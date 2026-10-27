/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laine Jones
 * Copyright (C) 2026 Stefan Reinauer */
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
 *                            5 Piccolo SD64, 6 Piccolo, 7 Spectrum 28/24,
 *                            8 Graffity, 9 Z3660, 10 Picasso IV,
 *                            11 UAE's RTG card (an emulator: its
 *                               uaegfx.card is in memory),
 *                            12 a card through its Picasso96 driver (a
 *                               monitor names a BOARDTYPE whose
 *                               LIBS:Picasso96/<it>.card is there: a
 *                               PiStorm's VideoCore, a Vampire)
 *                            - 11 and 12 only when none of ours is there
 *         ENV:PrismCardName  the card's name, or ""
 *         ENV:PrismP96Mon    the Picasso96 monitor file's name, or ""
 *         ENV:PrismP96Card   for 12: the card driver, for P96CARD=
 *         ENV:PrismP96MonDir for 12: the monitor's drawer (DEVS:Monitors,
 *                            or the parked one), for P96MONITOR=
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
 *   PrismSetup WBID=<id>
 *       Write ENVARC:Sys/screenmode.prefs (used from the next boot) for the Prism mode
 *       with ModeID $7A00<id> (the low word: depth slot << 8 | size slot, e.g. $0102 =
 *       800x600 16-bit), 256 pens. Returns 0, or 10. The installer uses this: a ModeID
 *       never changes when sizes are added. $80 on the size slot is the span mode
 *       across both monitors (SECONDMONITOR=SPAN), e.g. $0182 = 1600x600 16-bit.
 *
 *       Where the system keeps a screen mode per machine (Caffeine OS:
 *       screenmode.prefs.UAE and .PI next to it, one picked at boot), those
 *       get the mode too; each is kept first as <name>.before-prism.
 *
 *   PrismSetup P96CARD=<card driver> [P96MONITOR=<monitor>]
 *       Set PrismD's Picasso96 adapter in ENVARC:Prism.prefs (and ENV:):
 *       the installer does this for a card it found through its Picasso96
 *       driver (DETECT's card 12).
 *
 *   PrismSetup STARTUP=ADD|REMOVE|CHECK
 *       A Startup-Sequence whose LoadMonDrvs is given the monitors to load
 *       by name (Caffeine OS) never starts DEVS:Monitors/Prism. ADD puts a
 *       block that does after those lines (after "LAB ENDMONITORS" when
 *       there is one), keeping the old file as
 *       S:Startup-Sequence.before-prism; REMOVE takes the block out; CHECK
 *       only answers. Returns 0 = done (or the block is there), 5 = this
 *       Startup-Sequence needs no block, 10 = error (CHECK: one is missing).
 *
 *   PrismSetup WBMODE=<slot>
 *       The same by prefs slot number (prefs.h: depth * PREFS_NSIZES + size). Slot
 *       numbers move when sizes are added; WBID is the one to use.
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
#include <stdlib.h>
#include "../src/prism.h"
#include "../src/prefs.h"
#include "../src/catalog.h"

static const char version[] __attribute__((used)) = "$VER: PrismSetup " PRISM_RELEASE " (" PRISM_RELDATE ")";

struct ExpansionBase *ExpansionBase;
struct Library *IconBase;

#define TEMPLATE "DETECT/S,WBMODE/K/N,P96PARK/S,P96RESTORE/S,WBID/K,P96CARD/K,P96MONITOR/K,STARTUP/K"

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

static char p96type[64];                    /* card 12's BOARDTYPE */

/* Is this Picasso96 BOARDTYPE the driver for card `card`? */
static BOOL boardtype_matches(const char *bt, int card)
{
    if (card == 11)
        return same_nocase(bt, "uaegfx");
    if (card == 12)
        return same_nocase(bt, p96type);
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
    if (card == 9)
        return same_nocase(bt, "Z3660");
    if (card == 10)
        return same_nocase(bt, "PicassoIV");
    return FALSE;
}

#define PARK_DIR "SYS:Storage/Picasso96-parked"

/* UAE's RTG card: the emulator puts its uaegfx.card in the library list.
 * The same test as PrismD's UAEGFX driver (src/drv_uaegfx.c, UAEGFX_Probe)
 * - a uaegfx.card file on a disk is not the emulator's, and a card PrismD
 * would refuse must not make the installer park Picasso96 */
static BOOL uae_rtg(void)
{
    struct Library *lib;
    BOOL ok;
    Forbid();
    lib = (struct Library *)FindName(&SysBase->LibList, "uaegfx.card");
    ok = lib && lib->lib_Version >= 3 && lib->lib_NegSize >= 36 && lib->lib_IdString &&
         !strncmp((char *)lib->lib_IdString, "UAE Graphics Card", 17);
    Permit();
    return ok;
}

/* A monitor whose icon names a BOARDTYPE (not UAE's) that has a driver in
 * LIBS:Picasso96: a card Prism can drive through that driver. Looks in
 * DEVS:Monitors, then where a earlier install parked Picasso96's monitors. */
static BOOL find_p96_card(const char *dirName, char *card, int cardLen)
{
    BPTR lock;
    struct FileInfoBlock *fib;
    BOOL found = FALSE;

    if (!(IconBase = OpenLibrary("icon.library", 37)))
        return FALSE;
    fib = AllocDosObject(DOS_FIB, NULL);
    lock = Lock((STRPTR)dirName, ACCESS_READ);
    if (fib && lock && Examine(lock, fib)) {
        while (!found && ExNext(lock, fib)) {
            int n = strlen(fib->fib_FileName);
            char path[180], cpath[100];
            struct DiskObject *dob;
            STRPTR bt;
            BPTR cl;

            if (fib->fib_DirEntryType > 0 || n < 6 || n > 100 ||
                !same_nocase(fib->fib_FileName + n - 5, ".info"))
                continue;
            sprintf(path, "%s/%s", dirName, fib->fib_FileName);
            path[strlen(path) - 5] = 0;
            if (!(dob = GetDiskObject(path)))
                continue;
            if (dob->do_ToolTypes && (bt = FindToolType((void *)dob->do_ToolTypes, "BOARDTYPE")) &&
                *bt && strlen(bt) < sizeof(p96type) && !same_nocase(bt, "uaegfx")) {
                sprintf(cpath, "LIBS:Picasso96/%s.card", bt);
                if ((int)strlen(cpath) < cardLen && (cl = Lock(cpath, ACCESS_READ))) {
                    UnLock(cl);
                    strcpy(card, cpath);
                    strcpy(p96type, bt);
                    found = TRUE;
                }
            }
            FreeDiskObject(dob);
        }
    }
    if (lock) UnLock(lock);
    if (fib) FreeDosObject(DOS_FIB, fib);
    CloseLibrary(IconBase);
    return found;
}

/* The monitor in DEVS:Monitors (or `dirName`) whose icon says
 * BOARDTYPE=<this card>. */
static void find_p96_monitor(int card, const char *dirName, char *out, int outLen)
{
    BPTR lock;
    struct FileInfoBlock *fib;

    out[0] = 0;
    if (!card || !(IconBase = OpenLibrary("icon.library", 37)))
        return;
    fib = AllocDosObject(DOS_FIB, NULL);
    lock = Lock((STRPTR)dirName, ACCESS_READ);
    if (fib && lock && Examine(lock, fib)) {
        while (ExNext(lock, fib) && !out[0]) {
            int n = strlen(fib->fib_FileName);
            char path[160];
            struct DiskObject *dob;
            STRPTR bt;

            if (fib->fib_DirEntryType > 0 || n < 6 || n > 100 ||
                !same_nocase(fib->fib_FileName + n - 5, ".info"))
                continue;
            sprintf(path, "%s/%s", dirName, fib->fib_FileName);
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
        printf(S(ID_PRISMSETUP_NO_PICASSO96_FOUND, "PrismSetup: no Picasso96 found\n"));
        return 0;
    }
    make_dir("SYS:Storage");
    make_dir(PARK_DIR);
    make_dir(PARK_DIR "/Monitors");
    make_dir(PARK_DIR "/Libs");
    if (!(mf = Open(MANIFEST, MODE_READWRITE))) {
        printf(S(ID_PRISMSETUP_CAN_T_WRITE, "PrismSetup: can't write %s\n"), MANIFEST);
        return 5;
    }
    Seek(mf, 0, OFFSET_END);
    for (i = 0; i < npieces; i++) {
        if (there(pieces[i].to)) {                /* parked before and installed again */
            printf(S(ID_STAYS_IS_IN_THE_WAY, "  %s stays: %s is in the way\n"), pieces[i].from, pieces[i].to);
            failed++;
        } else if (Rename(pieces[i].from, pieces[i].to)) {
            FPrintf(mf, "%s\t%s\n", (LONG)pieces[i].to, (LONG)pieces[i].from);
            printf(S(ID_FROM_TO, "  %s -> %s\n"), pieces[i].from, pieces[i].to);
            moved++;
        } else {
            printf(S(ID_COULD_NOT_BE_MOVED_ERROR_D, "  %s could not be moved (error %ld)\n"), pieces[i].from, (long)IoErr());
            failed++;
        }
    }
    Close(mf);
    printf(S(ID_PRISMSETUP_MOVED_TO_NOT, "PrismSetup: %d moved to %s, %d not\n"), moved, PARK_DIR, failed);
    return failed ? 5 : 0;
}

static int p96_restore(void)
{
    BPTR mf;
    char line[260];
    int back = 0, left = 0;

    if (!(mf = Open(MANIFEST, MODE_OLDFILE))) {
        printf(S(ID_PRISMSETUP_NOTHING_PARKED_NOT_FOUND, "PrismSetup: nothing parked (%s not found)\n"), MANIFEST);
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
            printf(S(ID_FROM_TO, "  %s -> %s\n"), line, tab);
            back++;
        } else {
            printf(S(ID_STAYS_PARKED_IS_TAKEN, "  %s stays parked: %s is taken\n"), line, tab);
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
    printf(S(ID_PRISMSETUP_PUT_BACK_STILL_IN, "PrismSetup: %d put back, %d still in %s\n"), back, left, PARK_DIR);
    return left ? 5 : 0;
}

static int detect(void)
{
    static const char *names[] = { "", "Picasso II", "GBAPII++", "ZZ9000 (Zorro III)",
                                   "ZZ9000 (Zorro II)", "Piccolo SD64", "Piccolo",
                                   "Spectrum 28/24", "Graffity",
                                   "Z3660", "Picasso IV", "UAE RTG card", "" };
    static char p96name[100];
    char p96card[100], monDir[40];
    int card = 0;
    char num[4], mon[108];
    struct ConfigDev *p4;

    if ((ExpansionBase = (struct ExpansionBase *)OpenLibrary("expansion.library", 37))) {
        if (FindConfigDev(NULL, 0x6d6e, 4))                                card = 3;
        else if (FindConfigDev(NULL, 0x6d6e, 3))                           card = 4;
        else if (FindConfigDev(NULL, 2167, 17) && FindConfigDev(NULL, 2167, 16)) card = 2;
        else if (FindConfigDev(NULL, 2167, 12) && FindConfigDev(NULL, 2167, 11)) card = 1;
        else if (FindConfigDev(NULL, 2195, 11) && FindConfigDev(NULL, 2195, 10)) card = 5;
        else if (FindConfigDev(NULL, 2195, 6) && FindConfigDev(NULL, 2195, 5))   card = 6;
        else if (FindConfigDev(NULL, 2193, 2) && FindConfigDev(NULL, 2193, 1))   card = 7;
        else if (FindConfigDev(NULL, 2092, 33))                            card = 8;   /* Z2: 33 + 34, Z3: 33 */
        else if (FindConfigDev(NULL, 0x144b, 1))                           card = 9;
        else if ((p4 = FindConfigDev(NULL, 2167, 24)) && p4->cd_BoardSize >= 0x1000000) card = 10;   /* Z3 mode */
        else if (FindConfigDev(NULL, 2167, 23) && FindConfigDev(NULL, 2167, 21))  card = 10;   /* Z2 mode */
        CloseLibrary((struct Library *)ExpansionBase);
    }
    {
        /* test hook for the installer script: ENV:PrismFakeCard = 1..10
         * stands in for a card this machine doesn't have (11 and 12 are
         * found by what is installed, not faked) */
        char fake[8];
        int fc;
        if (GetVar("PrismFakeCard", fake, sizeof(fake), GVF_GLOBAL_ONLY) > 0 &&
            sscanf(fake, "%d", &fc) == 1 && fc >= 1 && fc <= 10)
            card = fc;
    }
    p96card[0] = 0;
    strcpy(monDir, "DEVS:Monitors");
    if (!card && uae_rtg())
        card = 11;
    if (!card) {
        if (find_p96_card("DEVS:Monitors", p96card, sizeof(p96card)) ||
            (find_p96_card(PARK_DIR "/Monitors", p96card, sizeof(p96card)) &&
             strcpy(monDir, PARK_DIR "/Monitors"))) {
            card = 12;
            sprintf(p96name, "%s (through its Picasso96 driver)", p96type);
            names[12] = p96name;
        }
    }
    find_p96_monitor(card, monDir, mon, sizeof(mon));
    if (card == 11 && !mon[0])
        find_p96_monitor(card, PARK_DIR "/Monitors", mon, sizeof(mon));
    setenv_str("PrismP96Card", p96card);
    setenv_str("PrismP96MonDir", card == 12 ? monDir : "");
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
    printf(S(ID_PRISMSETUP_CARD_PICASSO96_MONITOR, "PrismSetup: card %d (%s), Picasso96 monitor \"%s\"\n"), card,
           card ? names[card] : "none", mon);
    return card;
}

/* A whole small file into a buffer (NUL-ended); NULL if it can't be read */
static char *read_file(const char *path, LONG *len)
{
    LONG n = size_of(path);
    char *buf;
    BPTR fh;

    if (n < 0 || n > 200000 || !(buf = AllocVec(n + 1, MEMF_ANY)))
        return NULL;
    if (!(fh = Open((STRPTR)path, MODE_OLDFILE)) || Read(fh, buf, n) != n) {
        if (fh) Close(fh);
        FreeVec(buf);
        return NULL;
    }
    Close(fh);
    buf[n] = 0;
    *len = n;
    return buf;
}

static BOOL write_file(const char *path, const char *buf, LONG len)
{
    struct FileInfoBlock *fib = AllocDosObject(DOS_FIB, NULL);
    BPTR fh, l = Lock((STRPTR)path, ACCESS_READ);
    LONG prot = -1;
    BOOL ok;
    if (fib && l && Examine(l, fib))
        prot = fib->fib_Protection;               /* a new file loses them: put them back */
    if (l) UnLock(l);
    if (fib) FreeDosObject(DOS_FIB, fib);
    fh = Open((STRPTR)path, MODE_NEWFILE);
    ok = fh && Write(fh, (APTR)buf, len) == len;
    if (fh) Close(fh);
    if (ok && prot != -1)
        SetProtection((STRPTR)path, prot);
    return ok;
}

/* `from` kept as `to`, unless `to` is there already (the first copy is the
 * one from before PrismRTG) */
static BOOL keep_copy(const char *from, const char *to)
{
    char *buf;
    LONG len;
    BOOL ok;
    if (there(to))
        return TRUE;
    if (!(buf = read_file(from, &len)))
        return FALSE;
    ok = write_file(to, buf, len);
    FreeVec(buf);
    if (ok) {
        /* with the original's protection bits, so a copy back is the same */
        struct FileInfoBlock *fib = AllocDosObject(DOS_FIB, NULL);
        BPTR l = Lock((STRPTR)from, ACCESS_READ);
        if (fib && l && Examine(l, fib))
            SetProtection((STRPTR)to, fib->fib_Protection);
        if (l) UnLock(l);
        if (fib) FreeDosObject(DOS_FIB, fib);
    }
    return ok;
}

/* ---- a Startup-Sequence that names its monitors ------------------------- */
#define SS       "S:Startup-Sequence"
#define SS_KEEP  "S:Startup-Sequence.before-prism"
#define SS_BEGIN ";BEGIN PrismRTG"
#define SS_END   ";END PrismRTG"
static const char ssBlock[] =
    SS_BEGIN " - LoadMonDrvs here starts only the monitors it names\n"
    "IF EXISTS DEVS:Monitors/Prism\n"
    " DEVS:Monitors/Prism >NIL:\n"
    "EndIF\n"
    SS_END "\n";

/* the line at p starts (after blanks) with the word `word`, any case */
static BOOL line_word(const char *p, const char *word)
{
    while (*p == ' ' || *p == 9) p++;
    for (; *word; p++, word++)
        if (lower(*p) != lower(*word))
            return FALSE;
    return *p == ' ' || *p == 9 || *p == 10 || *p == 0;
}

static const char *next_line(const char *p)
{
    while (*p && *p != 10) p++;
    return *p ? p + 1 : p;
}

static int startup(const char *how)
{
    char *buf, *out;
    const char *p, *after = NULL, *b, *e;
    BOOL named = FALSE, lab = FALSE, pending = FALSE;
    int depth = 0;
    LONG len, blen = sizeof(ssBlock) - 1;
    int rc;

    if (!(buf = read_file(SS, &len))) {
        printf("PrismSetup: can't read %s\n", SS);
        return 10;
    }
    /* LoadMonDrvs given names ("A|B|C"): the stock one takes none */
    for (p = buf; *p; p = next_line(p)) {
        if (line_word(p, "IF"))
            depth++;
        else if (line_word(p, "EndIF") && depth > 0 && !--depth && pending && !lab) {
            after = next_line(p);                  /* the IF around the last one */
            pending = FALSE;
        }
        if (line_word(p, "LoadMonDrvs") || line_word(p, "C:LoadMonDrvs")) {
            const char *q;
            for (q = p; *q && *q != 10; q++)
                if (*q == '"') named = TRUE;
            if (!lab) {
                if (depth) pending = TRUE;
                else after = next_line(p), pending = FALSE;
            }
        }
        if (line_word(p, "LAB")) {
            const char *q = p;
            while (*q == ' ' || *q == 9) q++;
            q += 3;
            if (line_word(q, "ENDMONITORS")) {
                after = next_line(p);
                lab = TRUE;
            }
        }
    }
    if (pending)
        after = NULL;                              /* an IF never closed: don't guess */
    b = strstr(buf, SS_BEGIN);
    e = b ? strstr(b, SS_END) : NULL;
    if (same_nocase(how, "CHECK")) {
        rc = b ? 0 : named ? 10 : 5;
        printf("PrismSetup: %s\n", b ? "the PrismRTG block is in " SS :
               named ? "the PrismRTG block is NOT in " SS : SS " needs no PrismRTG block");
    } else if (same_nocase(how, "REMOVE")) {
        rc = 0;
        if (b && e) {
            e = next_line(e);
            memmove((char *)b, e, buf + len - e + 1);
            len -= e - b;
            if (!write_file(SS, buf, len)) {
                printf("PrismSetup: can't write %s\n", SS);
                rc = 10;
            } else {
                /* the copy from before the block: stale now (the file may
                 * have been edited since), and the next ADD keeps a new one */
                DeleteFile(SS_KEEP);
                printf("PrismSetup: the PrismRTG block is out of %s\n", SS);
            }
        }
    } else if (same_nocase(how, "ADD")) {
        if (b)
            rc = 0;                                /* there already */
        else if (!named || !after)
            rc = 5;
        else if (!keep_copy(SS, SS_KEEP) || !(out = AllocVec(len + blen + 2, MEMF_ANY))) {
            printf("PrismSetup: can't keep %s as %s\n", SS, SS_KEEP);
            rc = 10;
        } else {
            LONG at = after - buf, rest = len - at;
            memcpy(out, buf, at);
            if (at && out[at - 1] != 10)
                out[at++] = 10;                    /* a last line without its newline */
            memcpy(out + at, ssBlock, blen);
            memcpy(out + at + blen, after, rest);
            rc = write_file(SS, out, at + blen + rest) ? 0 : 10;
            printf(rc ? "PrismSetup: can't write %s\n" : "PrismSetup: PrismRTG block added to %s\n", SS);
            FreeVec(out);
        }
    } else {
        printf("PrismSetup: STARTUP=ADD, REMOVE or CHECK\n");
        rc = 10;
    }
    FreeVec(buf);
    return rc;
}

/* P96CARD= and P96MONITOR= into the settings */
static int set_p96(const char *card, const char *monitor)
{
    static struct PrismPrefs prefs;
    if (!prefs_load(&prefs, PREFS_ENVARC) && !prefs_load(&prefs, PREFS_ENV))
        prefs_default(&prefs);
    if (strlen(card) >= sizeof(prefs.p96card) ||
        (monitor && strlen(monitor) >= sizeof(prefs.p96monitor)))
        return 10;
    strcpy(prefs.p96card, card);
    strcpy(prefs.p96monitor, monitor ? monitor : "");
    if (!prefs_save(&prefs, PREFS_ENVARC)) {
        printf(S(ID_PRISMSETUP_CAN_T_WRITE, "PrismSetup: can't write %s\n"), PREFS_ENVARC);
        return 10;
    }
    prefs_save(&prefs, PREFS_ENV);
    printf("PrismSetup: P96CARD=%s P96MONITOR=%s\n", prefs.p96card, prefs.p96monitor);
    return 0;
}

/* IFF PREF with one SCRM chunk (prefs/screenmode.h), 62 bytes */
#define SPAN_ID 0x80                         /* in the size slot: a span mode (prismint.h) */

static int write_wbmode(LONG slot, BOOL span)
{
    /* ENVARC: only - it takes effect at the next boot. Writing ENV: as
     * well makes IPrefs try to reset the Workbench screen there and then
     * ("please close all windows"), under the installer's own window. */
    static const char *files[3] = { "ENVARC:Sys/screenmode.prefs",
                                     "ENVARC:Sys/screenmode.prefs.UAE",
                                     "ENVARC:Sys/screenmode.prefs.PI" };
    static struct PrismPrefs prefs;  /* for the custom sizes' slots */
    UBYTE f[62];
    ULONG id;
    UWORD w, h;
    int i, rc = 0;

    if (!prefs_load(&prefs, PREFS_ENVARC))
        prefs_load(&prefs, PREFS_ENV);

    if (slot < 0 || slot >= PREFS_NMODES) {
        printf(S(ID_PRISMSETUP_WBMODE_TAKES_A_MODE_SLOT_0_TO, "PrismSetup: WBMODE takes a mode slot, 0 to %d\n"), PREFS_NMODES - 1);
        return 10;
    }
    id = PRISM_MONITOR_ID | PREFS_IDLOW(slot);
    if (!prefs_size(&prefs, slot, &w, &h)) {
        printf(S(ID_PRISMSETUP_SLOT_D_IS_A_CUSTOM_SIZE_THAT, "PrismSetup: slot %ld is a custom size that is not set\n"), (long)slot);
        return 10;
    }
    if (span) {
        id |= SPAN_ID;
        w *= 2;
    }
    memset(f, 0, sizeof(f));
    memcpy(f, "FORM", 4);          f[7] = 54;
    memcpy(f + 8, "PREFPRHD", 8);  f[19] = 6;
    memcpy(f + 26, "SCRM", 4);     f[33] = 28;
    f[50] = id >> 24; f[51] = id >> 16; f[52] = id >> 8; f[53] = id;
    /* width and height "Default" (~0), as the ScreenMode editor writes it:
     * the screen is the mode's size, and stays so when ScreenMode picks
     * another mode later (with 800x600 written here it kept 800x600) */
    f[54] = f[55] = f[56] = f[57] = 0xff;
    f[59] = 8;                     /* depth: 256 pens at every Prism depth */
    f[61] = 1;                     /* control: autoscroll                   */
    for (i = 0; i < 3; i++) {
        BPTR fh;
        if (i) {
            /* Caffeine OS's per-machine copies: only where they are, and
             * each one kept first */
            char keep[60];
            if (!there(files[i]))
                continue;
            sprintf(keep, "%s.before-prism", files[i]);
            if (!keep_copy(files[i], keep)) {
                printf(S(ID_PRISMSETUP_CAN_T_WRITE, "PrismSetup: can't write %s\n"), keep);
                rc = 10;
                continue;
            }
        }
        fh = Open((STRPTR)files[i], MODE_NEWFILE);
        if (!fh || Write(fh, f, sizeof(f)) != sizeof(f)) {
            printf(S(ID_PRISMSETUP_CAN_T_WRITE, "PrismSetup: can't write %s\n"), files[i]);
            rc = 10;
        }
        if (fh) Close(fh);
    }
    if (!rc)
        printf(S(ID_PRISMSETUP_WORKBENCH_MODE_SET_TO_X_X, "PrismSetup: Workbench mode set to $%08lx (%ux%u)\n"), (unsigned long)id, w, h);
    return rc;
}

int main(void)
{
    LONG args[8] = { 0 };
    struct RDArgs *rda;
    int rc = 0;

    if (!(rda = ReadArgs(TEMPLATE, args, NULL))) {
        PrintFault(IoErr(), "PrismSetup");
        return 20;
    }
    cat_open();                              /* the texts: a translation, or the English */
    if (args[7])
        rc = startup((const char *)args[7]);
    else if (args[5])
        rc = set_p96((const char *)args[5], (const char *)args[6]);
    else if (args[4]) {
        /* $0102, 0x0102, 0102 or the whole $7A000102: depth slot, size slot */
        const char *a = (const char *)args[4];
        ULONG id, depth, size;
        if (*a == '$') a++;
        else if (a[0] == '0' && (a[1] == 'x' || a[1] == 'X')) a += 2;
        id = strtoul(a, NULL, 16) & 0xffff;
        depth = id >> 8;
        size = id & 0xff & ~SPAN_ID;
        if (depth >= PREFS_NDEPTHS || size >= PREFS_NSIZES || ((id & SPAN_ID) && depth > 1)) {
            printf(S(ID_PRISMSETUP_WBID_IS_NOT_A_PRISMRTG_MODE, "PrismSetup: WBID %s is not a PrismRTG mode\n"), (const char *)args[4]);
            rc = 10;
        } else
            rc = write_wbmode((LONG)(depth * PREFS_NSIZES + size), (id & SPAN_ID) != 0);
    } else if (args[1])
        rc = write_wbmode(*(LONG *)args[1], FALSE);
    else if (args[2])
        rc = p96_park();
    else if (args[3])
        rc = p96_restore();
    else
        rc = detect();
    FreeArgs(rda);
    cat_close();
    return rc;
}
