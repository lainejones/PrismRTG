/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Laine Jones */
/* lastalert: print exec's record of the last alert (it survives a reset). */
#include <exec/execbase.h>
#include <proto/exec.h>
#include <stdio.h>

int main(void)
{
    struct ExecBase *e = SysBase;
    printf("LastAlert %08lx %08lx %08lx %08lx\n", (unsigned long)e->LastAlert[0],
           (unsigned long)e->LastAlert[1], (unsigned long)e->LastAlert[2],
           (unsigned long)e->LastAlert[3]);
    return 0;
}
