/* SPDX-License-Identifier: GPL-3.0-only */
#include <exec/memory.h>
#include <dos/dostags.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <stdio.h>
#include <string.h>
#include "driver_module.h"
static struct PrismDriverRequest *active;
static struct MsgPort *replies;
static BOOL stalled;

BOOL driver_open(struct PrismBoard *b,const char *name,const struct PrismDriverConfig *config)
{
    char path[128],args[32];
    struct PrismDriverRequest *r;
    struct MsgPort *port;
    struct Process *child;
    BPTR segment,input;
    ULONG ticks;
    if(active || stalled || !name || strlen(name)>32 || strpbrk(name,"/:\\")) return FALSE;
    snprintf(path,sizeof(path),"LIBS:Prism/%s.driver",name);
    segment=LoadSeg(path);
    if(!segment) { snprintf(path,sizeof(path),"PROGDIR:Drivers/%s.driver",name);segment=LoadSeg(path); }
    if(!segment) { printf("PrismD: cannot load %s driver\n",name);return FALSE; }
    r=AllocVec(sizeof(*r),MEMF_PUBLIC|MEMF_CLEAR);
    port=CreateMsgPort();input=Open("NIL:",MODE_OLDFILE);
    if(!r || !port || !input) {
        if(r)FreeVec(r);
        if(port)DeleteMsgPort(port);
        if(input)Close(input);
        UnLoadSeg(segment);return FALSE;
    }
    r->message.mn_ReplyPort=port;r->message.mn_Length=sizeof(*r);
    r->magic=PRISM_DRIVER_MAGIC;r->abi=PRISM_DRIVER_ABI;r->size=sizeof(*r);
    r->boardABI=PRISM_BOARD_ABI;r->boardSize=sizeof(*b);
    r->boardLayout=driver_board_layout();
    r->opsABI=PRISM_OPS_ABI;r->opsSize=PRISM_OPS_SIZE;r->config=*config;
    r->status=PRD_ABI_MISMATCH;
    snprintf(args,sizeof(args),"%lx\n",(unsigned long)r);
    /* The child borrows Output(), including redirected diagnostic logs.
     * It owns only its NIL: input and must never close the parent's output.
     * Flush the parent's C buffer before the child begins writing to it. */
    fflush(stdout);
    child=CreateNewProcTags(NP_Seglist,(ULONG)segment,NP_FreeSeglist,(ULONG)TRUE,
        NP_Arguments,(ULONG)args,NP_Name,(ULONG)"Prism board driver",
        NP_Cli,(ULONG)TRUE,NP_Input,(ULONG)input,NP_Output,(ULONG)Output(),
        NP_CloseInput,(ULONG)TRUE,NP_CloseOutput,(ULONG)FALSE,
        NP_StackSize,(ULONG)PRISM_DRIVER_STACK,TAG_END);
    if(!child) {
        Close(input);DeleteMsgPort(port);FreeVec(r);UnLoadSeg(segment);return FALSE;
    }
    for(ticks=0;ticks<1500;ticks++) {
        if(GetMsg(port)) break;
        Delay(1);
    }
    if(ticks==1500) {
        /* Do not exit while a tardy probe can still write diagnostics to
         * our borrowed output. driver_close waits for its final reply. */
        stalled=TRUE;r->cancelled=TRUE;active=r;replies=port;
        puts("PrismD: driver startup timed out; waiting for safe release");
        return FALSE;
    }
    if(r->status!=PRD_READY || !r->control) {
        if(r->status==PRD_ABI_MISMATCH)
            printf("PrismD: %s driver ABI/version or layout mismatch; reinstall matching modules\n",name);
        else if(r->status==PRD_NO_RESOURCES)
            printf("PrismD: %s driver could not open its libraries or message port\n",name);
        else if(r->status==PRD_NOT_FOUND)
            printf("PrismD: %s driver found no supported board\n",name);
        else printf("PrismD: %s driver startup was cancelled\n",name);
        DeleteMsgPort(port);FreeVec(r);return FALSE;
    }
    *b=r->board;active=r;replies=port;return TRUE;
}
void driver_close(void)
{
    struct Message stop;
    if(!active) return;
    fflush(stdout);
    if(stalled) {
        /* A READY reply can race the timeout. In that case the child is
         * waiting for STOP, rather than having acknowledged cancellation. */
        WaitPort(replies);GetMsg(replies);
    }
    if(!stalled || (active->status==PRD_READY && active->control)) {
        memset(&stop,0,sizeof(stop));stop.mn_Length=sizeof(stop);stop.mn_ReplyPort=replies;
        PutMsg(active->control,&stop);
        WaitPort(replies);GetMsg(replies);
    }
    DeleteMsgPort(replies);FreeVec(active);active=NULL;replies=NULL;stalled=FALSE;
}
