/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Laine Jones */
/* prismlog ON|SYNC|OFF: switch a running PrismD's debug log (it goes wherever
 * PrismD's output goes). */
#include <exec/semaphores.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <stdio.h>
#include "../src/prism.h"

int main(int argc, char **argv)
{
    struct PrismSem *ps;
    /* ON = 1, SYNC = 2 (each line written at once), anything else = off */
    int on = argc > 1 ? ((argv[1][0] == 'S' || argv[1][0] == 's') ? 2 :
                         (argv[1][1] == 'N' || argv[1][1] == 'n') ? 1 : 0) : 0;

    Forbid();
    ps = (struct PrismSem *)FindSemaphore((STRPTR)PRISM_SEMNAME);
    if (ps && ps->version >= 3 && ps->logLevel)
    {
        *ps->logLevel = on;
        Signal(ps->task, SIGBREAKF_CTRL_F);      /* out of its SYNC wait */
    }
    else
        ps = NULL;
    Permit();
    puts(ps ? (on == 2 ? "prismlog: sync" : on ? "prismlog: on" : "prismlog: off") : "prismlog: PrismD (version 3) isn't running");
    return ps ? 0 : 5;
}
