/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Stefan Reinauer */
/* Dedicated-boot module lifecycle and API exerciser; reports over serial. */
#ifndef DRIVER_ARGUMENTS
#define DRIVER_ARGUMENTS "BOARD=UAEGFX"
#endif
#include <exec/types.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <dos/dostags.h>
#include <stdio.h>
#include "prism.h"
static void emit(const char *s) {
    *(volatile UWORD *)0xdff032=30;
    while (*s) {
        while (!(*(volatile UWORD *)0xdff018 & 0x2000)) {}
        *(volatile UWORD *)0xdff030=0x100|(UBYTE)*s++;
    }
}
static void report(const char *path) {
    FILE *f=fopen(path,"r"); char line[256];
    if(f) { while(fgets(line,sizeof(line),f)) emit(line); fclose(f); }
}
int main(void) {
    BPTR in=Open("NIL:",MODE_OLDFILE),out=Open("RAM:prism.log",MODE_NEWFILE);
    int i; struct PrismSem *sem=NULL;
    emit("PRISM INTEGRATION START\n");
    AssignLock("ENV",Lock("RAM:",ACCESS_READ));
    AssignLock("ENVARC",Lock("RAM:",ACCESS_READ));
    AssignLock("T",Lock("RAM:",ACCESS_READ));
    AssignLock("DEVS",Lock("RAM:",ACCESS_READ));
    if(SystemTags("C:PrismD " DRIVER_ARGUMENTS,SYS_Input,(ULONG)in,SYS_Output,(ULONG)out,
                  SYS_Asynch,(ULONG)TRUE,NP_StackSize,(ULONG)32768,TAG_END)==-1) {
        emit("FAIL launch\n"); return 20;
    }
    for(i=0;i<100;i++) {
        Forbid(); sem=(struct PrismSem *)FindSemaphore(PRISM_SEMNAME); Permit();
        if(sem)break;
        Delay(5);
    }
    if(!sem) { report("RAM:prism.log"); emit("FAIL startup\n"); return 20; }
    for(i=0;i<4;i++) {
        char cmd[100]; LONG rc; int depths[]={8,16,24,32};
        sprintf(cmd,"C:p96test DEPTH=%d%s >RAM:api.log",depths[i],
                depths[i]==32 ? " FORMAT=9" : "");
        rc=SystemTags(cmd,NP_StackSize,(ULONG)65536,TAG_END);
        report("RAM:api.log");
        if(rc) { emit("FAIL API\n"); return 20; }
    }
    Signal(sem->task,SIGBREAKF_CTRL_C);
    Delay(50);
    Forbid(); sem=(struct PrismSem *)FindSemaphore(PRISM_SEMNAME); Permit();
    if(sem) { emit("FAIL stop\n"); return 20; }
    report("RAM:prism.log");
    emit("PASS\n"); return 0;
}
