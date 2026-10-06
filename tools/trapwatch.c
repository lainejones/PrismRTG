/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Laine Jones */
/* trapwatch: catch a program's CPU exception (the cause of a guru) and
 * write it to a file instead of letting the machine stop.
 *
 *   Run >NIL: trapwatch DH0:trap.log
 *
 * It puts its own trap handler into every task that exists and into
 * exec's default for new tasks. When a task traps, the handler notes the
 * exception number, the program counter, the registers and the code around
 * the PC, parks that task for good and wakes trapwatch, which writes the
 * report. A debugging aid: it never takes its handler out again.
 */
#include <exec/execbase.h>
#include <exec/tasks.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <stdio.h>
#include <string.h>

ULONG trapNum, trapPC, trapRegs[15], trapUSP;
UWORD trapSR, trapFmt;
struct Task *trapTask, *watcher;

void trap_entry(void);
void park(void);

/* Supervisor mode. Stack: trap number (long), then the exception frame:
 * SR (word), PC (long), format/vector (word), ... The frame is thrown away
 * (its size depends on the format) and a plain one built that "returns"
 * to park() in the task's own context. */
__asm__(
"       .text\n"
"       .globl _trap_entry\n"
"_trap_entry:\n"
"       move.l  (sp)+,_trapNum\n"
"       movem.l d0-d7/a0-a6,_trapRegs\n"
"       move.w  (sp),_trapSR\n"
"       move.l  2(sp),_trapPC\n"
"       move.w  6(sp),d0\n"
"       move.w  d0,_trapFmt\n"
"       move.l  usp,a0\n"
"       move.l  a0,_trapUSP\n"
"       lsr.w   #8,d0\n"
"       lsr.w   #4,d0\n"                 /* frame format 0..15            */
"       moveq   #8,d1\n"                 /* format 0, 1: 8 bytes          */
"       cmp.w   #2,d0\n"
"       bne.s   1f\n"
"       moveq   #12,d1\n"
"1:     cmp.w   #9,d0\n"
"       bne.s   2f\n"
"       moveq   #20,d1\n"
"2:     cmp.w   #10,d0\n"
"       bne.s   3f\n"
"       moveq   #32,d1\n"
"3:     cmp.w   #11,d0\n"
"       bne.s   4f\n"
"       moveq   #92,d1\n"
"4:     cmp.w   #7,d0\n"
"       bne.s   5f\n"
"       moveq   #60,d1\n"
"5:     cmp.w   #4,d0\n"
"       bne.s   6f\n"
"       moveq   #16,d1\n"
"6:     add.l   d1,sp\n"
"       clr.w   -(sp)\n"                 /* format 0                      */
"       pea     _park\n"
"       clr.w   -(sp)\n"                 /* SR: user mode, interrupts on  */
"       rte\n"
);

void park(void)
{
    trapTask = FindTask(NULL);
    Signal(watcher, SIGBREAKF_CTRL_E);
    for (;;)
        Wait(0);
}

static char buf[16384];

/* Where code lives, to put names to the PC and the return addresses on
 * the stack: each process's loaded segments, and for each library the
 * span its jump table points into. */
static int segs(int len, const char *name, BPTR sl)
{
    int k = 0;

    while (sl && k < 24 && len < (int)sizeof(buf) - 200) {
        ULONG *s = (ULONG *)BADDR(sl);
        len += sprintf(buf + len, "%s %s %08lx-%08lx", k ? "" : "\n  seg", k ? "" : name,
                       (unsigned long)(s + 1), (unsigned long)s + s[-1] - 4);
        sl = (BPTR)*s;
        k++;
    }
    return len;
}

static int code_map(int len)
{
    struct List *lists[2] = { &SysBase->TaskWait, &SysBase->TaskReady };
    struct Node *n;
    int i;

    for (i = 0; i < 2; i++)
        for (n = lists[i]->lh_Head; n->ln_Succ; n = n->ln_Succ) {
            struct Process *pr = (struct Process *)n;
            struct CommandLineInterface *cli;
            if (n->ln_Type != NT_PROCESS)
                continue;
            cli = (struct CommandLineInterface *)BADDR(pr->pr_CLI);
            if (cli && cli->cli_Module)
                len = segs(len, n->ln_Name ? n->ln_Name : "?", cli->cli_Module);
            else if (pr->pr_SegList && ((LONG *)BADDR(pr->pr_SegList))[0] >= 3)
                len = segs(len, n->ln_Name ? n->ln_Name : "?", ((BPTR *)BADDR(pr->pr_SegList))[3]);
        }
    if (trapTask && trapTask->tc_Node.ln_Type == NT_PROCESS) {
        struct Process *pr = (struct Process *)trapTask;
        struct CommandLineInterface *cli = (struct CommandLineInterface *)BADDR(pr->pr_CLI);
        if (cli && cli->cli_Module)
            len = segs(len, "(trapped)", cli->cli_Module);
        else if (pr->pr_SegList && ((LONG *)BADDR(pr->pr_SegList))[0] >= 3)
            len = segs(len, "(trapped)", ((BPTR *)BADDR(pr->pr_SegList))[3]);
    }
    for (n = SysBase->LibList.lh_Head; n->ln_Succ && len < (int)sizeof(buf) - 200; n = n->ln_Succ) {
        struct Library *lib = (struct Library *)n;
        ULONG lo = 0xffffffff, hi = 0;
        UWORD v, nv = lib->lib_NegSize / 6;
        for (v = 1; v <= nv; v++) {
            UWORD *j = (UWORD *)((UBYTE *)lib - 6 * v);
            ULONG a;
            if (*j != 0x4ef9)
                continue;
            a = *(ULONG *)(j + 1);
            if (a < lo) lo = a;
            if (a > hi) hi = a;
        }
        if (hi)
            len += sprintf(buf + len, "\n  lib %s %08lx-%08lx", n->ln_Name ? n->ln_Name : "?",
                           (unsigned long)lo, (unsigned long)hi);
    }
    return len;
}

int main(int argc, char **argv)
{
    struct Node *n;

    if (argc < 2) {
        puts("usage: trapwatch <file>");
        return 20;
    }
    watcher = FindTask(NULL);
    Forbid();
    SysBase->TaskTrapCode = (APTR)trap_entry;
    for (n = SysBase->TaskWait.lh_Head; n->ln_Succ; n = n->ln_Succ)
        ((struct Task *)n)->tc_TrapCode = (APTR)trap_entry;
    for (n = SysBase->TaskReady.lh_Head; n->ln_Succ; n = n->ln_Succ)
        ((struct Task *)n)->tc_TrapCode = (APTR)trap_entry;
    Permit();
    SetTaskPri(watcher, 25);

    for (;;) {
        ULONG sig;
        int len = 0, i;
        BPTR f;

        /* Ten times a second: tasks started since get the handler too
         * (dos.library gives every new process its own trap code). */
        Delay(5);
        sig = SetSignal(0, SIGBREAKF_CTRL_C | SIGBREAKF_CTRL_E);
        if (sig & SIGBREAKF_CTRL_C)
            break;
        Forbid();
        for (n = SysBase->TaskWait.lh_Head; n->ln_Succ; n = n->ln_Succ)
            ((struct Task *)n)->tc_TrapCode = (APTR)trap_entry;
        for (n = SysBase->TaskReady.lh_Head; n->ln_Succ; n = n->ln_Succ)
            ((struct Task *)n)->tc_TrapCode = (APTR)trap_entry;
        Permit();
        if (!(sig & SIGBREAKF_CTRL_E))
            continue;
        len += sprintf(buf + len, "TRAP %lu in task '%s' (%lx)\n  PC %08lx SR %04x frame %04x USP %08lx\n",
                       (unsigned long)trapNum,
                       trapTask && trapTask->tc_Node.ln_Name ? trapTask->tc_Node.ln_Name : "?",
                       (unsigned long)trapTask, (unsigned long)trapPC, trapSR, trapFmt,
                       (unsigned long)trapUSP);
        len += sprintf(buf + len, "  d0-d7");
        for (i = 0; i < 8; i++)
            len += sprintf(buf + len, " %08lx", (unsigned long)trapRegs[i]);
        len += sprintf(buf + len, "\n  a0-a6");
        for (i = 8; i < 15; i++)
            len += sprintf(buf + len, " %08lx", (unsigned long)trapRegs[i]);
        len += sprintf(buf + len, "\n  stack:");
        if (trapUSP > 0x1000 && !(trapUSP & 1))
            for (i = 0; i < 48; i++)
                len += sprintf(buf + len, " %08lx", (unsigned long)((ULONG *)trapUSP)[i]);
        len += sprintf(buf + len, "\n  code at PC-32:");
        if (trapPC > 0x1000 && !(trapPC & 1))
            for (i = -16; i < 32; i++) {
                if (i == 0) len += sprintf(buf + len, " |");
                len += sprintf(buf + len, " %04x", ((UWORD *)trapPC)[i]);
            }
        Forbid();
        len = code_map(len);
        Permit();
        len += sprintf(buf + len, "\n");
        if ((f = Open((STRPTR)argv[1], MODE_READWRITE))) {
            Seek(f, 0, OFFSET_END);
            Write(f, buf, len);
            Close(f);
            {
                /* onto the disk now: the machine may not last */
                struct MsgPort *fs = DeviceProc((STRPTR)argv[1]);
                if (fs)
                    DoPkt(fs, ACTION_FLUSH, 0, 0, 0, 0, 0);
            }
        }
    }
    return 0;
}
