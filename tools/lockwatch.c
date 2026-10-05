/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Laine Jones */
/* lockwatch: a debugging aid for deadlocks. Every two seconds it appends to
 * a file who owns and who waits for Prism's drawing lock, each screen's
 * LayerInfo lock and each layer's lock, and which tasks sit in a semaphore
 * wait. It only uses exec and dos, so it keeps writing when Intuition and
 * graphics are stuck; the last report in the file is the stuck state.
 *
 *   Run >NIL: lockwatch DH0:lw.log
 */
#include <exec/execbase.h>
#include <exec/semaphores.h>
#include <exec/tasks.h>
#include <exec/interrupts.h>
#include <hardware/intbits.h>
#include <graphics/layers.h>
#include <graphics/clip.h>
#include <intuition/intuitionbase.h>
#include <intuition/screens.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <stdio.h>
#include <string.h>
#include "../src/prism.h"

static char buf[6000];
static int n;

static void add(const char *fmt, ...)
{
    va_list ap;
    if (n > (int)sizeof(buf) - 200)
        return;
    va_start(ap, fmt);
    n += vsprintf(buf + n, fmt, ap);
    va_end(ap);
}

static const char *tname(struct Task *t)
{
    return t ? (t->tc_Node.ln_Name ? t->tc_Node.ln_Name : "?") : "-";
}

/* " [command]" for a Shell process that is running one (call under Forbid) */
static void cmd(struct Task *t)
{
    struct Process *pr = (struct Process *)t;
    struct CommandLineInterface *cli;
    UBYTE *name;

    if (t->tc_Node.ln_Type != NT_PROCESS || !pr->pr_CLI)
        return;
    cli = (struct CommandLineInterface *)((ULONG)pr->pr_CLI << 2);
    name = (UBYTE *)((ULONG)cli->cli_CommandName << 2);
    if (name && name[0] && n < (int)sizeof(buf) - 300) {
        buf[n++] = ' ';
        buf[n++] = '[';
        memcpy(buf + n, name + 1, name[0]);
        n += name[0];
        buf[n++] = ']';
    }
}

/* one semaphore: owner and waiters (call under Forbid) */
static void sem(const char *what, ULONG id, struct SignalSemaphore *s)
{
    struct SemaphoreRequest *r;

    if (!s->ss_Owner && s->ss_QueueCount < 0)
        return;
    add("%s %lx: owner '%s' (%lx) nest %d queue %d", what, id, tname(s->ss_Owner),
        (ULONG)s->ss_Owner, s->ss_NestCount, s->ss_QueueCount);
    for (r = (struct SemaphoreRequest *)s->ss_WaitQueue.mlh_Head; r->sr_Link.mln_Succ;
         r = (struct SemaphoreRequest *)r->sr_Link.mln_Succ)
        add(" <- '%s'", tname((struct Task *)((ULONG)r->sr_Waiter & ~1UL)));
    add("\n");
}

/* A sampling profiler: a vertical blank interrupt server notes which task
 * was running, and whether it had task switching forbidden, fifty times a
 * second. A machine that looks hung with every task "ready" has one task
 * (or one Forbid) that never lets go: this names it. */
#define NSAMP 256
static struct Task *volatile samp[NSAMP];
static volatile BYTE sampTd[NSAMP];
static volatile UWORD sampAt;

static ULONG vblank(void)
{
    UWORD i = sampAt & (NSAMP - 1);
    samp[i] = SysBase->ThisTask;
    sampTd[i] = SysBase->TDNestCnt;
    sampAt++;
    return 0;                       /* Z set: the other servers still run */
}

static void profile(UWORD from, UWORD to)
{
    static struct Task *who[24];
    static UWORD cnt[24], forb[24];
    UWORD i, k, nw = 0, total = to - from;

    if (total > NSAMP) { from = to - NSAMP; total = NSAMP; }
    for (i = from; i != to; i++) {
        struct Task *t = samp[i & (NSAMP - 1)];
        for (k = 0; k < nw && who[k] != t; k++) ;
        if (k == nw) {
            if (nw == 24) continue;
            who[nw] = t; cnt[nw] = forb[nw] = 0; nw++;
        }
        cnt[k]++;
        if (sampTd[i & (NSAMP - 1)] >= 0) forb[k]++;
    }
    add("running, of %u samples (idle %lu, switches %lu):\n", total, SysBase->IdleCount,
        SysBase->DispCount);
    for (k = 0; k < nw; k++) {
        add("   %3u (forbidden in %u) '%s' (%lx)", cnt[k], forb[k], tname(who[k]), (ULONG)who[k]);
        cmd(who[k]);
        add("\n");
    }
}

int main(int argc, char **argv)
{
    static struct Interrupt is;
    UWORD lastAt = 0;
    struct IntuitionBase *ib;
    struct PrismSem *ps;
    ULONG round = 0;

    if (argc < 2) {
        puts("usage: lockwatch <file>");
        return 20;
    }
    if (!(ib = (struct IntuitionBase *)OpenLibrary((STRPTR)"intuition.library", 37)))
        return 20;
    SetTaskPri(FindTask(NULL), 20);
    is.is_Node.ln_Type = NT_INTERRUPT;
    is.is_Node.ln_Pri = -20;
    is.is_Node.ln_Name = (char *)"lockwatch";
    is.is_Code = (void (*)())vblank;
    AddIntServer(INTB_VERTB, &is);
    while (!(SetSignal(0, 0) & SIGBREAKF_CTRL_C)) {
        struct Node *t;
        BPTR f;

        n = 0;
        add("--- %lu\n", round++);
        Forbid();
        ps = (struct PrismSem *)FindSemaphore((STRPTR)PRISM_SEMNAME);
        if (ps && ps->version >= 3 && ps->drawLock)
            sem("prism lock", 0, ps->drawLock);
        /* (no walk of Intuition's screens and layers: without their locks
         * the lists can be half-changed, and with them we could hang too) */
        for (t = SysBase->TaskWait.lh_Head; t->ln_Succ; t = t->ln_Succ)
        {
            add("waiting: '%s' (%lx) for %08lx", tname((struct Task *)t), (ULONG)t,
                ((struct Task *)t)->tc_SigWait);
            cmd((struct Task *)t);
            add("\n");
        }
        {
            UWORD now = sampAt;
            profile(lastAt, now);
            lastAt = now;
        }
        for (t = SysBase->TaskReady.lh_Head; t->ln_Succ; t = t->ln_Succ) {
            ULONG *sp = (ULONG *)((struct Task *)t)->tc_SPReg;
            add("ready: '%s' (%lx) pri %d", tname((struct Task *)t), (ULONG)t, t->ln_Pri);
            cmd((struct Task *)t);
            /* where it was stopped: its saved context (the program counter
             * is among the first longs; a task that is ready report after
             * report with this changing is the one using the processor) */
            add("\n   sp %lx: %08lx %08lx %08lx %08lx %08lx %08lx\n", (ULONG)sp,
                sp[0], sp[1], sp[2], sp[3], sp[4], sp[5]);
        }
        Permit();
        if ((f = Open((STRPTR)argv[1], MODE_READWRITE))) {
            Seek(f, 0, OFFSET_END);
            Write(f, buf, n);
            Close(f);
            {
                /* to the disk now: the next thing may be a freeze */
                struct MsgPort *fs = DeviceProc((STRPTR)argv[1]);
                if (fs)
                    DoPkt(fs, ACTION_FLUSH, 0, 0, 0, 0, 0);
            }
        }
        Delay(100);
    }
    RemIntServer(INTB_VERTB, &is);
    CloseLibrary((struct Library *)ib);
    return 0;
}
