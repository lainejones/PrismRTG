/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laine Jones */
/*
 * Prism - the DEVS:Monitors file. LoadMonDrvs runs it at boot, before
 * IPrefs, so Workbench can open straight onto a Prism mode.
 *
 * It starts C:PrismD in the background, waits until PrismD has published
 * its modes, and returns. Hold the LEFT MOUSE BUTTON during boot to skip
 * Prism (a way out if a board or mode misbehaves). Output goes to NIL:, or
 * to T:PrismD.log when ENV:Prism.prefs has LOG=ON.
 */
#include <exec/types.h>
#include <exec/semaphores.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include "prism.h"
#include "prefs.h"

static const char version[] __attribute__((used)) = "$VER: Prism 1.1b1 (07.10.2026)";

#define PRISMD "C:PrismD"

static BOOL prism_up(void)
{
    BOOL up;
    Forbid();
    up = FindSemaphore(PRISM_SEMNAME) != NULL;
    Permit();
    return up;
}

int main(void)
{
    struct PrismPrefs p;
    BPTR in, out, lock;
    int i;

    /* CIA-A PRA bit 6 is low while the left mouse button is held */
    if (!(*(volatile UBYTE *)0xbfe001 & 0x40)) {
        PutStr("Prism: left mouse button held - not starting\n");
        return 0;
    }
    if (prism_up())
        return 0;
    /* Failsafe for a machine with nobody at the keyboard: if the last
     * start of Prism never reached a minute of running (PrismD deletes the
     * flag then), don't start it this time. Workbench then falls back to a
     * native mode and the rest of the boot - network included - goes on. */
    if ((lock = Lock(PRISM_BOOTFLAG, ACCESS_READ))) {
        UnLock(lock);
        DeleteFile(PRISM_BOOTFLAG);
        PutStr("Prism: the last start didn't finish - skipped this once\n");
        return 5;
    }
    if (!(lock = Lock(PRISMD, ACCESS_READ))) {
        PutStr("Prism: " PRISMD " is missing\n");
        return 10;
    }
    UnLock(lock);

    prefs_load(&p, PREFS_ENV);
    if (!(in = Open("NIL:", MODE_OLDFILE)))
        return 20;
    out = p.log ? Open("T:PrismD.log", MODE_NEWFILE) : 0;
    if (!out && !(out = Open("NIL:", MODE_NEWFILE))) {
        Close(in);
        return 20;
    }
    if ((lock = Open(PRISM_BOOTFLAG, MODE_NEWFILE)))
        Close(lock);
    if (SystemTags(PRISMD, SYS_Input, in, SYS_Output, out, SYS_Asynch, TRUE,
                   NP_StackSize, 16384, NP_Name, (ULONG)"PrismD", TAG_END) == -1) {
        Close(in);
        Close(out);
        PutStr("Prism: can't start " PRISMD "\n");
        return 20;
    }
    /* IPrefs comes next: give PrismD up to 5 s to add its modes */
    for (i = 0; i < 50 && !prism_up(); i++)
        Delay(5);
    return prism_up() ? 0 : 5;
}
