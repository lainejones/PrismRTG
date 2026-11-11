/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Ferenc Boldog */
/* shadow_guest - the P96 adapter's shadow-mode rules on a stock emulator,
 * with the test cards tests/shim_card.c makes of UAE's uaegfx.card:
 *
 *   shadow_guest CARD=LIBS:Picasso96/softsprite.card SHADOW=0
 *   shadow_guest CARD=LIBS:Picasso96/ownfree.card SHADOW=1
 *
 * Starts C:PrismD BOARD=P96 P96CARD=<card> (PrismD must not be running;
 * ENV: and ENVARC: become RAM:, so it reads no saved prefs),
 * checks that the board is in shadow mode or not as SHADOW says, then runs
 * C:p96test and C:tmpltest MODEID=<mode> (default $7A000301, 640x480
 * 32-bit: uaegfx offers no more in a headless Amiberry), each of which must
 * pass, and stops PrismD. Prints and sends to the serial port PASS or
 * FAIL ..., with the seconds each tool took. On a 68060 there tmpltest
 * took about 750 s in shadow mode before the shadow upload changes
 * (b0a8b04), about 55 s after them and 30 s without shadow mode; the
 * times are reported, not checked. A dedicated boot (UAE Zorro III RTG). */
#include <exec/types.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <dos/dostags.h>
#include <stdio.h>
#include <string.h>
#include "prism.h"
#include "prismboard.h"

static void emit(const char *s)
{
    fputs(s, stdout);
    fflush(stdout);
    *(volatile UWORD *)0xdff032 = 30;
    while (*s) {
        while (!(*(volatile UWORD *)0xdff018 & 0x2000)) {}
        *(volatile UWORD *)0xdff030 = 0x100 | (UBYTE)*s++;
    }
}

static void report(const char *path)
{
    FILE *f = fopen(path, "r");
    char line[256];
    if (f) {
        while (fgets(line, sizeof(line), f))
            emit(line);
        fclose(f);
    }
}

static struct PrismSem *find_sem(void)
{
    struct PrismSem *sem;
    Forbid();
    sem = (struct PrismSem *)FindSemaphore((STRPTR)PRISM_SEMNAME);
    Permit();
    return sem;
}

static LONG seconds(void)
{
    struct DateStamp d;
    DateStamp(&d);
    return (d.ds_Days * 1440 + d.ds_Minute) * 60 + d.ds_Tick / TICKS_PER_SECOND;
}

/* runs cmd with its output in log; fails unless rc is 0 and the log has
 * want (NULL: rc alone) */
static int run(const char *cmd, const char *log, const char *want)
{
    char buf[300], line[256];
    LONG t = seconds(), rc;
    int found = !want;
    FILE *f;

    sprintf(buf, "%s >%s", cmd, log);
    rc = SystemTags((STRPTR)buf, NP_StackSize, (ULONG)65536, TAG_END);
    if ((f = fopen(log, "r"))) {
        while (fgets(line, sizeof(line), f))
            if (want && strstr(line, want))
                found = 1;
        fclose(f);
    }
    sprintf(buf, "%s: rc %ld, %ld s\n", cmd, (long)rc, (long)(seconds() - t));
    emit(buf);
    if (rc || !found) {
        report(log);
        return 0;
    }
    return 1;
}

int main(void)
{
    LONG args[3] = { 0, 0, 0 };
    struct RDArgs *rda = ReadArgs((STRPTR)"CARD/A,SHADOW/N/A,MODEID", args, NULL);
    const char *card, *mode;
    BPTR in, out;
    struct PrismSem *sem = NULL;
    char buf[300];
    int i, ok, shadow, want;

    if (!rda) {
        PrintFault(IoErr(), (STRPTR)"shadow_guest");
        return 20;
    }
    card = (const char *)args[0];
    want = *(LONG *)args[1] != 0;
    mode = args[2] ? (const char *)args[2] : "$7A000301";
    emit("SHADOW GUEST START\n");
    if (find_sem()) {
        emit("FAIL PrismD is already running: use a boot without it\n");
        FreeArgs(rda);
        return 20;
    }
    /* no saved prefs: PrismD runs on its arguments alone, and doesn't move
     * Workbench to a Prism mode in the middle of the test (as
     * driver_guest) */
    AssignLock((STRPTR)"ENV", Lock((STRPTR)"RAM:", ACCESS_READ));
    AssignLock((STRPTR)"ENVARC", Lock((STRPTR)"RAM:", ACCESS_READ));
    in = Open((STRPTR)"NIL:", MODE_OLDFILE);
    out = Open((STRPTR)"RAM:shadow_prismd.log", MODE_NEWFILE);
    sprintf(buf, "C:PrismD BOARD=P96 P96CARD=%s", card);
    if (SystemTags((STRPTR)buf, SYS_Input, (ULONG)in, SYS_Output, (ULONG)out, SYS_Asynch, (ULONG)TRUE,
                   NP_StackSize, (ULONG)32768, TAG_END) == -1) {
        emit("FAIL launch\n");
        FreeArgs(rda);
        return 20;
    }
    for (i = 0; i < 200 && !(sem = find_sem()); i++)
        Delay(5);
    if (!sem) {
        report("RAM:shadow_prismd.log");
        emit("FAIL PrismD did not start\n");
        FreeArgs(rda);
        return 20;
    }
    if (sem->version < 4 || !sem->boardFlags) {
        emit("FAIL this PrismD does not report its board flags\n");
        FreeArgs(rda);
        return 20;
    }
    shadow = (*sem->boardFlags & PBF_SHADOW) != 0;
    sprintf(buf, "%s: board flags $%04lx, %s (expected %s)\n", card, (unsigned long)*sem->boardFlags,
            shadow ? "shadow mode" : "no shadow mode", want ? "shadow mode" : "no shadow mode");
    emit(buf);
    ok = shadow == want;
    ok &= run("C:p96test", "RAM:shadow_p96test.log", "p96test: 0 of ");
    sprintf(buf, "C:tmpltest MODEID=%s", mode);
    ok &= run(buf, "RAM:shadow_tmpltest.log", " 0 wrong");
    {
        /* every compared result must be "0 wrong" */
        FILE *f = fopen("RAM:shadow_tmpltest.log", "r");
        char line[256];
        int results = 0, wrong = 0;
        if (f) {
            while (fgets(line, sizeof(line), f)) {
                char *w = strstr(line, " wrong");
                if (w) {
                    results++;
                    if (!strstr(line, " 0 wrong"))
                        wrong++;
                }
            }
            fclose(f);
        }
        sprintf(buf, "tmpltest: %d results, %d with wrong pixels\n", results, wrong);
        emit(buf);
        ok &= results > 0 && !wrong;
    }
    Signal(sem->task, SIGBREAKF_CTRL_C);
    for (i = 0; i < 200 && find_sem(); i++)    /* up to 20 s */
        Delay(5);
    if (find_sem()) {
        emit("FAIL PrismD did not stop\n");
        ok = 0;
    }
    emit(ok ? "PASS\n" : "FAIL\n");
    FreeArgs(rda);
    return ok ? 0 : 20;
}
