/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laine Jones */
/*
 * PrismProbe - list the expansion boards and say which ones Prism can drive.
 *
 *   PrismProbe                   the boards, one line each
 *   PrismProbe REPORT [TO=file]  a full report for a card PrismRTG doesn't find:
 *                                the system, every board's autoconfig data and
 *                                driver, PrismD's state and prefs, the other RTG
 *                                software. Default file RAM:PrismRTG-report.txt.
 *
 * Started from Workbench (double-click its icon) it makes the report and
 * shows where it went in a window.
 *
 * Read-only: it never writes to a board, so it is safe to run while
 * Picasso96 owns the display.
 */
#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/memory.h>
#include <libraries/configvars.h>
#include <graphics/gfxbase.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/expansion.h>
#include <stdio.h>
#include <string.h>
#include <exec/semaphores.h>
#include "../src/prism.h"

static const char version[] __attribute__((used)) = "$VER: PrismProbe " PRISM_RELEASE " (" PRISM_RELDATE ")";

/* libnix: the window stdout gets when PrismProbe is started from Workbench */
char *__stdiowin = "CON:20/30/620/180/PrismProbe/AUTO/CLOSE/WAIT";

struct ExpansionBase *ExpansionBase;
extern struct ExecBase *SysBase;
extern struct GfxBase *GfxBase;

#define ISSUE_URL "https://github.com/lainejones/PrismRTG/issues/new?template=card-report.yml"

static const char *known(UWORD mfr, UBYTE prod)
{
    if (mfr == 0x6d6e && prod == 3)  return "ZZ9000 (Zorro II)  -> zz9000 driver";
    if (mfr == 0x6d6e && prod == 4)  return "ZZ9000 (Zorro III) -> zz9000 driver";
    if (mfr == 0x144b && prod == 1)  return "Z3660 RTG -> z3660 driver";
    if (mfr == 2167 && prod == 11)   return "Picasso II/II+ VRAM -> picasso2 driver";
    if (mfr == 2167 && prod == 12)   return "Picasso II/II+ registers -> picasso2 driver";
    if (mfr == 2167 && prod == 16)   return "GBAPII++ VRAM -> picasso2 driver";
    if (mfr == 2167 && prod == 17)   return "GBAPII++ registers -> picasso2 driver";
    if (mfr == 2195 && prod == 10)   return "Piccolo SD64 VRAM -> picasso2 driver (emulation-tested)";
    if (mfr == 2195 && prod == 11)   return "Piccolo SD64 registers -> picasso2 driver (emulation-tested)";
    if (mfr == 2195 && prod == 5)    return "Piccolo VRAM -> picasso2 driver (emulation-tested)";
    if (mfr == 2195 && prod == 6)    return "Piccolo registers -> picasso2 driver (emulation-tested)";
    if (mfr == 2193 && prod == 1)    return "Spectrum 28/24 VRAM -> picasso2 driver (emulation-tested)";
    if (mfr == 2193 && prod == 2)    return "Spectrum 28/24 registers -> picasso2 driver (emulation-tested)";
    if (mfr == 2092 && prod == 34)   return "Graffity VRAM -> picasso2 driver (emulation-tested)";
    if (mfr == 2092 && prod == 33)   return "Graffity registers -> picasso2 driver (emulation-tested)";
    if (mfr == 0x1398 && prod == 5)  return "SAGA (Vampire V4) -> saga driver (test, BOARD=SAGA)";
    if (mfr == 0x1398 && prod == 8)  return "SAGA (Apollo A6000) -> saga driver (test, BOARD=SAGA)";
    return NULL;
}

/* is PrismD driving this board now (its PrismSem names its boards)? */
static BOOL prism_drives(const struct ConfigDev *cd)
{
    struct PrismSem *ps;
    BOOL r = FALSE;
    Forbid();
    ps = (struct PrismSem *)FindSemaphore((STRPTR)PRISM_SEMNAME);
    if (ps && ps->version >= 6)
        r = (ULONG)cd->cd_BoardAddr == ps->boardAddr[0] ||
            (ps->numBoards > 1 && (ULONG)cd->cd_BoardAddr == ps->boardAddr[1]);
    Permit();
    return r;
}

static const char *state_of(const struct ConfigDev *cd)
{
    return prism_drives(cd) ? "[PrismD] " : (cd->cd_Flags & CDF_CONFIGME) ? "[free] " : "[claimed] ";
}

/* ---- the report ---- */

static void cpu_line(FILE *f)
{
    UWORD a = SysBase->AttnFlags;
    const char *cpu = (a & 0x400) ? "68080" : (a & AFF_68060) ? "68060" : (a & AFF_68040) ? "68040" :
                      (a & AFF_68030) ? "68030" : (a & AFF_68020) ? "68020" : (a & AFF_68010) ? "68010" : "68000";
    const char *fpu = (a & AFF_FPU40) ? "on-chip FPU" : (a & AFF_68882) ? "68882" : (a & AFF_68881) ? "68881" : "no FPU";
    fprintf(f, "CPU: %s, %s (AttnFlags $%04x)\n", cpu, fpu, a);
}

/* a text file into the report, as it is */
static void file_into(FILE *f, const char *name)
{
    char line[256];
    FILE *in = fopen(name, "r");
    if (!in) { fprintf(f, "%s: not there\n", name); return; }
    fprintf(f, "%s:\n", name);
    while (fgets(line, sizeof(line), in)) {
        fputs("   ", f);
        fputs(line, f);
        if (!strchr(line, '\n')) fputc('\n', f);
    }
    fclose(in);
}

/* two text files hold the same */
static BOOL same_file(const char *a, const char *b)
{
    FILE *fa = fopen(a, "r"), *fb = fopen(b, "r");
    BOOL same = fa && fb;
    int ca, cb;
    while (same) {
        ca = fgetc(fa); cb = fgetc(fb);
        if (ca != cb) same = FALSE;
        else if (ca == EOF) break;
    }
    if (fa) fclose(fa);
    if (fb) fclose(fb);
    return same;
}

/* a drawer's files, one line each */
static void dir_into(FILE *f, const char *name)
{
    BPTR lock = Lock((STRPTR)name, ACCESS_READ);
    struct FileInfoBlock *fib = AllocDosObject(DOS_FIB, NULL);
    int n = 0;
    if (!lock || !fib) {
        fprintf(f, "%s: not there\n", name);
        if (lock) UnLock(lock);
        if (fib) FreeDosObject(DOS_FIB, fib);
        return;
    }
    fprintf(f, "%s:", name);
    if (Examine(lock, fib))
        while (ExNext(lock, fib)) {
            fprintf(f, "%s%s (%ld)", n++ % 3 ? ", " : "\n   ", fib->fib_FileName, (long)fib->fib_Size);
        }
    fprintf(f, n ? "\n" : " empty\n");
    UnLock(lock);
    FreeDosObject(DOS_FIB, fib);
}

static void report(FILE *f)
{
    struct ConfigDev *cd = NULL;
    struct PrismSem *ps;
    struct Node *nd;
    struct Library *vl;
    int n = 0;

    fprintf(f, "PrismProbe REPORT - PrismRTG %s (%s)\n", PRISM_RELEASE, PRISM_RELDATE);
    fprintf(f, "Send this with a report at %s\n\n", ISSUE_URL);

    fprintf(f, "== system\n");
    fprintf(f, "Kickstart: exec %d.%d\n", SysBase->LibNode.lib_Version, SysBase->SoftVer);
    if ((vl = OpenLibrary((STRPTR)"version.library", 0))) {
        fprintf(f, "Workbench: version.library %d.%d\n", vl->lib_Version, vl->lib_Revision);
        CloseLibrary(vl);
    }
    cpu_line(f);
    fprintf(f, "Memory: chip %lu KB, other %lu KB\n",
            (unsigned long)(AvailMem(MEMF_CHIP | MEMF_TOTAL) / 1024),
            (unsigned long)(AvailMem(MEMF_FAST | MEMF_TOTAL) / 1024));
    fprintf(f, "Chipset: graphics.library %d.%d, ChipRevBits0 $%02x (%s)\n",
            GfxBase->LibNode.lib_Version, GfxBase->LibNode.lib_Revision, GfxBase->ChipRevBits0,
            (GfxBase->ChipRevBits0 & GFXF_AA_ALICE) ? "AGA" : (GfxBase->ChipRevBits0 & GFXF_HR_DENISE) ? "ECS" : "OCS");

    fprintf(f, "\n== expansion boards (manufacturer/product, address, size, type, serial, driver)\n");
    while ((cd = FindConfigDev(cd, -1, -1))) {
        const struct ExpansionRom *er = &cd->cd_Rom;
        const char *k = known(er->er_Manufacturer, er->er_Product);
        const char *drv = NULL;
        UBYTE t = er->er_Type;
        if (cd->cd_Driver && ((struct Node *)cd->cd_Driver)->ln_Name)
            drv = ((struct Node *)cd->cd_Driver)->ln_Name;
        fprintf(f, "%5u/%-3u  $%04x/$%02x  @ $%08lx  size $%08lx  %s%s%s\n",
                er->er_Manufacturer, er->er_Product, er->er_Manufacturer, er->er_Product,
                (unsigned long)cd->cd_BoardAddr, (unsigned long)cd->cd_BoardSize,
                (t & ERT_TYPEMASK) == ERT_ZORROIII ? "Zorro III" : "Zorro II",
                (t & ERTF_MEMLIST) ? ", memory" : "", (t & ERTF_DIAGVALID) ? ", boot ROM" : "");
        fprintf(f, "            er_Type $%02x  er_Flags $%02x  serial $%08lx  cd_Flags $%02x  %s\n",
                t, er->er_Flags, (unsigned long)er->er_SerialNumber, cd->cd_Flags, state_of(cd));
        fprintf(f, "            driver: %s\n", drv ? drv : "-");
        fprintf(f, "            PrismRTG: %s\n", k ? k : "not a card PrismRTG knows");
        n++;
    }
    fprintf(f, "%d board(s)\n", n);

    fprintf(f, "\n== PrismRTG\n");
    Forbid();
    ps = (struct PrismSem *)FindSemaphore((STRPTR)PRISM_SEMNAME);
    if (!ps) {
        Permit();
        fprintf(f, "PrismD is not running\n");
    } else {
        char name1[60] = "", name2[60] = "", rel[24] = "";
        UWORD v = ps->version;
        ULONG modes = ps->numModes, a0 = 0, a1 = 0;
        UBYTE nb = 1;
        if (ps->boardName) strncpy(name1, ps->boardName, sizeof(name1) - 1);
        if (v >= 6) {
            nb = ps->numBoards;
            if (ps->board2Name) strncpy(name2, ps->board2Name, sizeof(name2) - 1);
            a0 = ps->boardAddr[0]; a1 = ps->boardAddr[1];
        }
        if (v >= 7 && ps->release) strncpy(rel, ps->release, sizeof(rel) - 1);
        Permit();
        fprintf(f, "PrismD is running: release %s, interface version %u, %lu modes\n",
                rel[0] ? rel : "?", v, (unsigned long)modes);
        fprintf(f, "board 1: %s @ $%08lx\n", name1[0] ? name1 : "?", (unsigned long)a0);
        if (nb > 1) fprintf(f, "board 2: %s @ $%08lx\n", name2[0] ? name2 : "?", (unsigned long)a1);
    }
    file_into(f, "ENV:Prism.prefs");
    if (same_file("ENV:Prism.prefs", "ENVARC:Prism.prefs"))
        fprintf(f, "ENVARC:Prism.prefs: the same\n");
    else
        file_into(f, "ENVARC:Prism.prefs");
    dir_into(f, "LIBS:Prism");
    dir_into(f, "DEVS:Monitors");

    fprintf(f, "\n== other graphics software in memory\n");
    Forbid();
    for (nd = SysBase->LibList.lh_Head; nd->ln_Succ; nd = nd->ln_Succ) {
        const char *s = nd->ln_Name;
        if (s && (strstr(s, "Picasso") || strstr(s, "rtg") || strstr(s, "cybergraphics") ||
                  strstr(s, "Prism") || strstr(s, ".card")))
            fprintf(f, "%s %d.%d\n", s, ((struct Library *)nd)->lib_Version, ((struct Library *)nd)->lib_Revision);
    }
    Permit();
    dir_into(f, "LIBS:Picasso96");
    fprintf(f, "\n== end\n");
}

int main(int argc, char **argv)
{
    struct ConfigDev *cd = NULL;
    LONG args[2] = { 0, 0 };
    struct RDArgs *rda = NULL;
    int n = 0, ours = 0, rep = 0;
    char to[256] = "RAM:PrismRTG-report.txt";
    const char * volatile keepver = version;
    (void)keepver; (void)argv;

    if (!(ExpansionBase = (struct ExpansionBase *)OpenLibrary("expansion.library", 37))) {
        printf("PrismProbe: no expansion.library\n");
        return 20;
    }
    if (argc == 0) rep = 1;                     /* from Workbench: the report */
    else if ((rda = ReadArgs((STRPTR)"REPORT/S,TO/K", args, NULL))) {
        rep = args[0] != 0;
        if (args[1]) { strncpy(to, (char *)args[1], sizeof(to) - 1); rep = 1; }
        FreeArgs(rda);
    } else {
        PrintFault(IoErr(), (STRPTR)"PrismProbe");
        CloseLibrary((struct Library *)ExpansionBase);
        return 20;
    }

    printf("PrismProbe - expansion boards\n");
    while ((cd = FindConfigDev(cd, -1, -1))) {
        const char *k = known(cd->cd_Rom.er_Manufacturer, cd->cd_Rom.er_Product);
        printf(" %5u/%-3u @ $%08lx size $%08lx %s%s\n",
               cd->cd_Rom.er_Manufacturer, cd->cd_Rom.er_Product,
               (unsigned long)cd->cd_BoardAddr, (unsigned long)cd->cd_BoardSize,
               state_of(cd), k ? k : "");
        n++;
        if (k) ours++;
    }
    printf("%d board(s), %d usable by Prism\n", n, ours);

    if (rep) {
        FILE *f = fopen(to, "w");
        if (!f) printf("\nPrismProbe: cannot write %s\n", to);
        else {
            report(f);
            fclose(f);
            printf("\nThe report is in %s\n\n"
                   "If PrismRTG doesn't find your graphics card, please send it with\n"
                   "a report, and say which card it is: at\n  https://github.com/lainejones/PrismRTG/issues\n"
                   "choose New issue, then \"A graphics card PrismRTG doesn't find\".\n", to);
        }
    }
    CloseLibrary((struct Library *)ExpansionBase);
    return ours ? 0 : 5;
}
