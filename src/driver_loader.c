/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Stefan Reinauer
 * Copyright (C) 2026 Laine Jones */
#include <exec/memory.h>
#include <exec/execbase.h>
#include <dos/dostags.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <stdio.h>
#include <string.h>
#include "driver_module.h"
extern struct ExecBase *SysBase;
/* One driver process per board PrismD drives (multi-board.md): each
 * slot is a running driver, or the one that timed out at start-up. */
#define DRIVER_SLOTS 2
static struct Slot {
    struct PrismDriverRequest *active;
    struct MsgPort *replies;
    BOOL stalled;
    struct Task *childTask;
} slots[DRIVER_SLOTS];
BOOL driverQuiet;

/* Is the driver process still there? One that ends without replying (its
 * startup failed, or BOARD= named some other program) must not be waited
 * for. */
static BOOL child_alive(struct Task *childTask)
{
    struct Node *n;
    BOOL alive = FALSE;
    Forbid();
    for (n = SysBase->TaskReady.lh_Head; n->ln_Succ && !alive; n = n->ln_Succ)
        alive = (struct Task *)n == childTask;
    for (n = SysBase->TaskWait.lh_Head; n->ln_Succ && !alive; n = n->ln_Succ)
        alive = (struct Task *)n == childTask;
    Permit();
    return alive;
}

BOOL driver_open(struct PrismBoard *b,const char *name,const struct PrismDriverConfig *config)
{
    char path[128],args[32];
    struct PrismDriverRequest *r;
    struct MsgPort *port;
    struct Process *child;
    struct Task *childTask;
    struct Slot *sl=NULL;
    BPTR segment,input;
    ULONG ticks;
    int k;
    /* a free slot; none while a timed-out driver may still answer */
    for(k=0;k<DRIVER_SLOTS;k++) {
        if(slots[k].stalled) return FALSE;
        if(!sl && !slots[k].active) sl=&slots[k];
    }
    if(!sl || !name || strlen(name)>32 || strpbrk(name,"/:\\")) return FALSE;
    /* Drivers next to the program first: a build run from its own drawer
     * must not pick up the installed (older) modules. At boot PrismD is
     * C:PrismD, so this finds nothing and LIBS:Prism is used. */
    snprintf(path,sizeof(path),"PROGDIR:Drivers/%s.driver",name);
    segment=LoadSeg(path);
    if(!segment) { snprintf(path,sizeof(path),"LIBS:Prism/%s.driver",name);segment=LoadSeg(path); }
    if(!segment) { if(!driverQuiet) printf("PrismD: cannot load %s driver\n",name);return FALSE; }
    /* the request, then the board and the settings it points to (protocol 4) */
    r=AllocVec(sizeof(*r)+sizeof(struct PrismBoard)+sizeof(struct PrismDriverConfig),MEMF_PUBLIC|MEMF_CLEAR);
    port=CreateMsgPort();input=Open("NIL:",MODE_OLDFILE);
    if(!r || !port || !input) {
        if(r)FreeVec(r);
        if(port)DeleteMsgPort(port);
        if(input)Close(input);
        UnLoadSeg(segment);return FALSE;
    }
    r->message.mn_ReplyPort=port;r->message.mn_Length=sizeof(*r);
    r->magic=PRISM_DRIVER_MAGIC;r->abi=PRISM_DRIVER_ABI;r->size=sizeof(*r);
    r->board=(struct PrismBoard *)(r+1);
    r->config=(struct PrismDriverConfig *)(r->board+1);
    *r->config=*config;
    r->boardABI=PRISM_BOARD_ABI;r->boardSize=sizeof(struct PrismBoard);
    r->modeSize=sizeof(struct PrismMode);r->configSize=sizeof(struct PrismDriverConfig);
    r->boardLayout=driver_board_layout();
    r->opsABI=PRISM_OPS_ABI;r->opsSize=PRISM_OPS_SIZE;
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
    childTask=&child->pr_Task;
    for(ticks=0;ticks<1500;ticks++) {
        if(GetMsg(port)) break;
        if(!child_alive(childTask)) {
            if(GetMsg(port)) break;
            printf("PrismD: %s driver ended without answering\n",name);
            DeleteMsgPort(port);FreeVec(r);return FALSE;
        }
        Delay(1);
    }
    if(ticks==1500) {
        /* Do not exit while a tardy probe can still write diagnostics to
         * our borrowed output. driver_close waits for its final reply. */
        sl->stalled=TRUE;r->cancelled=TRUE;sl->active=r;sl->replies=port;sl->childTask=childTask;
        puts("PrismD: driver startup timed out; waiting for safe release");
        return FALSE;
    }
    if(r->status!=PRD_READY || !r->control) {
        if(r->status==PRD_ABI_MISMATCH)
            printf("PrismD: %s driver doesn't fit this PrismD (driver protocol %lu, board ABI %lu): use the one from this release, or any from 1.1 beta 8 up to it\n",name,(unsigned long)PRISM_DRIVER_ABI,(unsigned long)PRISM_BOARD_ABI);
        else if(r->status==PRD_NO_RESOURCES)
            printf("PrismD: %s driver could not open its libraries or message port\n",name);
        else if(r->status==PRD_NOT_FOUND) {
            if(!driverQuiet) printf("PrismD: %s driver found no supported board\n",name);
        }
        else printf("PrismD: %s driver startup was cancelled\n",name);
        DeleteMsgPort(port);FreeVec(r);return FALSE;
    }
    *b=*r->board;sl->active=r;sl->replies=port;sl->childTask=childTask;return TRUE;
}
static void slot_close(struct Slot *sl)
{
    struct Message stop;
    if(!sl->active) return;
    fflush(stdout);
    if(sl->stalled) {
        /* A READY reply can race the timeout. In that case the child is
         * waiting for STOP, rather than having acknowledged cancellation. */
        while(!GetMsg(sl->replies)) {
            if(!child_alive(sl->childTask) && !GetMsg(sl->replies)) {
                /* it ended without a reply: nothing more will come */
                DeleteMsgPort(sl->replies);FreeVec(sl->active);
                sl->active=NULL;sl->replies=NULL;sl->stalled=FALSE;
                return;
            }
            Delay(5);
        }
    }
    if(!sl->stalled || (sl->active->status==PRD_READY && sl->active->control)) {
        memset(&stop,0,sizeof(stop));stop.mn_Length=sizeof(stop);stop.mn_ReplyPort=sl->replies;
        PutMsg(sl->active->control,&stop);
        WaitPort(sl->replies);GetMsg(sl->replies);
    }
    DeleteMsgPort(sl->replies);FreeVec(sl->active);sl->active=NULL;sl->replies=NULL;sl->stalled=FALSE;
}
/* the two boards swapped places (PRIMARY=SECOND): their drivers too */
void driver_swap_slots(void)
{
    struct Slot t=slots[0];
    slots[0]=slots[1];
    slots[1]=t;
}
void driver_close_slot(int n)
{
    if(n>=0 && n<DRIVER_SLOTS) slot_close(&slots[n]);
}
/* every driver, the last started first */
void driver_close(void)
{
    int k;
    for(k=DRIVER_SLOTS-1;k>=0;k--) slot_close(&slots[k]);
}

/* The board is on Auto and the supplied native drivers found nothing: try
 * every other .driver installed, in directory order. The P96 and UAE
 * adapters need settings (or claim the emulator's card for good), so they
 * stay explicit choices. */
static BOOL scan_dir(struct PrismBoard *b,const struct PrismDriverConfig *config,
                     const char *dir,char tried[][33],int *ntried)
{
    struct FileInfoBlock *fib;
    BPTR lock;
    BOOL found=FALSE;
    if(!(lock=Lock(dir,ACCESS_READ))) return FALSE;
    if((fib=AllocDosObject(DOS_FIB,NULL))) {
        if(Examine(lock,fib))
            while(!found && ExNext(lock,fib)) {
                char name[33];
                size_t n=strlen(fib->fib_FileName);
                int i;
                if(fib->fib_DirEntryType>0 || n<8 || n-7>32 ||
                   stricmp(fib->fib_FileName+n-7,".driver")) continue;
                memcpy(name,fib->fib_FileName,n-7);name[n-7]=0;
                if(!stricmp(name,"PICASSO2") || !stricmp(name,"ZZ9000") ||
                   !stricmp(name,"P96") || !stricmp(name,"UAEGFX")) continue;
                for(i=0;i<*ntried;i++) if(!stricmp(tried[i],name)) break;
                if(i<*ntried) continue;
                if(*ntried<16) strcpy(tried[(*ntried)++],name);
                found=driver_open(b,name,config);
            }
        FreeDosObject(DOS_FIB,fib);
    }
    UnLock(lock);
    return found;
}
BOOL driver_scan(struct PrismBoard *b,const struct PrismDriverConfig *config)
{
    char tried[16][33];
    int ntried=0;
    return scan_dir(b,config,"PROGDIR:Drivers",tried,&ntried) ||
           scan_dir(b,config,"LIBS:Prism",tried,&ntried);
}
