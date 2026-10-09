/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Stefan Reinauer */
/* A loadable fixture for driver_module_guest, never installed as a driver. */
#include <proto/exec.h>
#include <proto/dos.h>
#include <stdio.h>
#include <stdlib.h>
#include "driver_module.h"
int main(void)
{
    struct PrismDriverRequest *r=(APTR)strtoul(GetArgStr(),NULL,16);
    struct MsgPort *control=NULL;
    struct Message *ack=&r->message;

    setvbuf(stdout,NULL,_IONBF,0);
    puts("module fixture: startup output");
    if(!driver_compatible(r)) goto done;
#ifdef FIXTURE_NOT_FOUND
    puts("module fixture: probe failed");
    r->status=PRD_NOT_FOUND;
#else
    struct Task *me=FindTask(NULL);
    control=CreateMsgPort();
    if(!control) {r->status=PRD_NO_RESOURCES;goto done;}
    r->board.name="module fixture";
    r->board.vramSize=(ULONG)me->tc_SPUpper-(ULONG)me->tc_SPLower;
    r->status=PRD_READY;r->control=control;
    ReplyMsg(ack);WaitPort(control);ack=GetMsg(control);
    puts("module fixture: final output");
#endif
done:
    if(control)DeleteMsgPort(control);
    fflush(stdout);SelectOutput(0);ReplyMsg(ack);
    return 0;
}
