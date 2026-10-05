/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laine Jones */
/*
 * iotime - what do single bus accesses and OS calls cost on this machine?
 *
 * Run with PrismD started and idle (the Cirrus chip awake, no Prism screen
 * being drawn into). Touches only the BLT background-colour register (GR0),
 * a few bytes in the driver's VRAM scratch area, and its own memory.
 * Prints nanoseconds per operation, loop overhead subtracted.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/semaphores.h>
#include <devices/timer.h>
#include <libraries/configvars.h>
#include <proto/exec.h>
#include <proto/expansion.h>
#include <proto/timer.h>
#include <stdio.h>
#include <graphics/gfxbase.h>
#include <graphics/rastport.h>
#include <proto/graphics.h>

struct ExpansionBase *ExpansionBase;
struct Device *TimerBase;
static struct timerequest tr;

#define N 20000UL

static ULONG ticks(void)
{
    struct EClockVal ev;
    ReadEClock(&ev);
    return ev.ev_lo;
}

static ULONG efreq, base;

static void report(const char *what, ULONG t)
{
    /* ns per op = (t - base) / efreq * 1e9 / N, in integers */
    ULONG d = t > base ? t - base : 0;
    ULONG ns = (ULONG)(((unsigned long long)d * 1000000000ULL) / efreq / N);
    printf("  %-34s %5lu ns\n", what, (unsigned long)ns);
}

int main(void)
{
    struct ConfigDev *regs, *mem;
    struct MsgPort *mp;
    struct EClockVal ev;
    struct SignalSemaphore sem;
    volatile UBYTE *r, *v;
    volatile ULONG *chip, *fast;
    volatile ULONG sink = 0;
    ULONG i, t0, t;
    UBYTE a, b;

    ExpansionBase = (struct ExpansionBase *)OpenLibrary("expansion.library", 37);
    if (!ExpansionBase) return 20;
    if (!(regs = mem = FindConfigDev(NULL, 0x6d6e, 4))) {
        puts("no Zorro III ZZ9000");
        return 5;
    }
    if (!(mp = CreateMsgPort())) return 20;
    tr.tr_node.io_Message.mn_ReplyPort = mp;
    if (OpenDevice(TIMERNAME, UNIT_ECLOCK, (struct IORequest *)&tr, 0)) return 20;
    TimerBase = tr.tr_node.io_Device;
    efreq = ReadEClock(&ev);

    r = (volatile UBYTE *)regs->cd_BoardAddr;
    v = (volatile UBYTE *)mem->cd_BoardAddr + 0x10000 + 0x2000000;
    chip = AllocMem(64, MEMF_CHIP | MEMF_CLEAR);
    fast = AllocMem(64, MEMF_FAST | MEMF_CLEAR);
    if (!chip || !fast) return 20;
    InitSemaphore(&sem);

    printf("iotime: regs $%08lx, VRAM scratch $%08lx, EClock %lu Hz, %lu loops\n",
           (unsigned long)r, (unsigned long)v, (unsigned long)efreq, N);

    t0 = ticks(); for (i = 0; i < N; i++) sink += i;                    base = ticks() - t0;
    printf("  (empty loop: %lu ticks)\n", (unsigned long)base);

    t0 = ticks(); for (i = 0; i < N; i++) { sink += i; *fast = i; }      t = ticks() - t0;
    report("fast RAM long write", t);
    t0 = ticks(); for (i = 0; i < N; i++) { sink += i; sink += *fast; }  t = ticks() - t0;
    report("fast RAM long read", t);
    t0 = ticks(); for (i = 0; i < N; i++) { sink += i; sink += *chip; }  t = ticks() - t0;
    report("chip RAM long read", t);
    t0 = ticks(); for (i = 0; i < N; i++) { sink += i; sink += *(volatile UWORD *)chip; } t = ticks() - t0;
    report("chip RAM word read", t);

    t0 = ticks(); for (i = 0; i < N; i++) { sink += i; v[0] = i; }       t = ticks() - t0;
    report("VRAM byte write", t);
    t0 = ticks(); for (i = 0; i < N; i++) { sink += i; *(volatile UWORD *)v = i; } t = ticks() - t0;
    report("VRAM word write", t);
    t0 = ticks(); for (i = 0; i < N; i++) { sink += i; *(volatile ULONG *)v = i; } t = ticks() - t0;
    report("VRAM long write", t);
    t0 = ticks(); for (i = 0; i < N; i++) { sink += i; sink += v[0]; }   t = ticks() - t0;
    report("VRAM byte read", t);
    t0 = ticks(); for (i = 0; i < N; i++) { sink += i; sink += *(volatile ULONG *)v; } t = ticks() - t0;
    report("VRAM long read", t);

    t0 = ticks(); for (i = 0; i < N; i++) { sink += i; ObtainSemaphore(&sem); ReleaseSemaphore(&sem); } t = ticks() - t0;
    report("ObtainSemaphore + Release", t);
    t0 = ticks(); for (i = 0; i < N; i++) { sink += i; Forbid(); Permit(); } t = ticks() - t0;
    report("Forbid + Permit", t);
    t0 = ticks(); for (i = 0; i < N; i++) { sink += i; ReadEClock(&ev); } t = ticks() - t0;
    report("ReadEClock", t);

    {
        struct GfxBase *GfxBase = (struct GfxBase *)OpenLibrary("graphics.library", 39);
        struct RastPort rp;
        InitRastPort(&rp);
        t0 = ticks(); for (i = 0; i < N; i++) { sink += i; Move(&rp, 1, 1); } t = ticks() - t0;
        report("Move() (ROM, 3 instructions)", t);
        t0 = ticks(); for (i = 0; i < N; i++) { sink += i; SetAPen(&rp, i & 3); } t = ticks() - t0;
        report("SetAPen()", t);
        t0 = ticks(); for (i = 0; i < N; i++) { sink += i; sink += (ULONG)FindTask(NULL); } t = ticks() - t0;
        report("FindTask(NULL)", t);
        CloseLibrary((struct Library *)GfxBase);
    }
    FreeMem((APTR)chip, 64);
    FreeMem((APTR)fast, 64);
    CloseDevice((struct IORequest *)&tr);
    DeleteMsgPort(mp);
    CloseLibrary((struct Library *)ExpansionBase);
    return 0;
}
