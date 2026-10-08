/* SPDX-License-Identifier: GPL-3.0-only */
/* Each .driver is an executable whose process keeps callbacks and private
 * data alive. It borrows PrismD's output until the final acknowledgement. */
#include <exec/memory.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/graphics.h>
#include <stdio.h>
#include <stdlib.h>
#include "driver_module.h"
struct GfxBase *GfxBase;
struct ExpansionBase *ExpansionBase;
/* UtilityBase is initialized by libnix before main. */
static struct Message *acknowledgement;

/* libnix gives each standard stream a 64 KB buffer at startup (about
 * 190 KB per driver process); PrismD has the same line. */
static unsigned long stdioBufSize = 1024;
unsigned long *__BUFSIZE = &stdioBufSize;

#if defined(DRIVER_P96)
#define DRIVER_NAME "P96"
#elif defined(DRIVER_UAEGFX)
#define DRIVER_NAME "UAEGFX"
#elif defined(DRIVER_PICASSO2)
#define DRIVER_NAME "PICASSO2"
#elif defined(DRIVER_ZZ9000)
#define DRIVER_NAME "ZZ9000"
#endif
static const char version[] __attribute__((used)) = "$VER: " DRIVER_NAME ".driver 1.1b2 (08.10.2026)";

void driver_module_done(void)
{
    /* No module output follows this point, even for a retained context.
     * NP_CloseOutput is FALSE: PrismD owns the borrowed file handle. */
    fflush(stdout);
    SelectOutput(0);
    if(acknowledgement) {
        struct Message *message=acknowledgement;
        acknowledgement=NULL;
        ReplyMsg(message);
    }
}
static void retain(void)
{
#if defined(DRIVER_P96)
    P96_KeepResident();
#elif defined(DRIVER_UAEGFX)
    UAEGFX_KeepResident();
#endif
}
int main(void)
{
    struct PrismDriverRequest *r=(APTR)strtoul(GetArgStr(),NULL,16);
    struct MsgPort *control=NULL;
    BOOL found=FALSE;
    setvbuf(stdout,NULL,_IONBF,0);
    if(!r || !TypeOfMem(r)) return 20;
    acknowledgement=&r->message;
    if(r->magic!=PRISM_DRIVER_MAGIC) {
        /* Another version's request: answer it untouched - the loader
         * preset its status to "ABI mismatch". */
        driver_module_done();return 20;
    }
    if(!driver_compatible(r)) {
        /* The loader initializes status to ABI_MISMATCH. Do not write any
         * version-dependent fields in an incompatible request. */
        driver_module_done();return 20;
    }
    if(r->cancelled) { r->status=PRD_CANCELLED;goto done; }
    r->status=PRD_NO_RESOURCES;
    GfxBase=(struct GfxBase *)OpenLibrary("graphics.library",39);
    ExpansionBase=(struct ExpansionBase *)OpenLibrary("expansion.library",37);
    if(GfxBase && ExpansionBase) control=CreateMsgPort();
    if(control) {
#if defined(DRIVER_P96)
        found=P96_Probe(&r->board,r->config.card,r->config.monitor);
#elif defined(DRIVER_UAEGFX)
        found=UAEGFX_Probe(&r->board);
#elif defined(DRIVER_PICASSO2)
        Picasso2_ClutBGR=r->config.clutBGR;
        found=Picasso2_Probe(&r->board);
#elif defined(DRIVER_ZZ9000)
        found=ZZ9000_Probe(&r->board);
#else
#error Driver selection missing
#endif
        r->status=found ? PRD_READY : PRD_NOT_FOUND;
    }
    if(r->cancelled) { r->status=PRD_CANCELLED;found=FALSE; }
    if(found) {
        r->control=control;
        fflush(stdout);
        acknowledgement=NULL;
        ReplyMsg(&r->message);
        WaitPort(control);acknowledgement=GetMsg(control);
    }
done:
    if(control)DeleteMsgPort(control);
    /* Retainers quiesce their claimed card as they did in linked PrismD,
     * flush their diagnostic, then acknowledge before parking forever.
     * Native drivers never ran shutdown on an uninitialized mode; keep
     * that lifecycle unchanged, including early no-modes failures. */
    retain();
    if(ExpansionBase)CloseLibrary((struct Library *)ExpansionBase);
    if(GfxBase)CloseLibrary((struct Library *)GfxBase);
    driver_module_done();
    return found ? 0 : 5;
}
