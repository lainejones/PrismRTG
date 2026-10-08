/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laine Jones */
/*
 * PrismD - the M1 Prism core, run as a background program:
 *
 *   Run >NIL: PrismD [BOARD=ZZ9000|PICASSO2]
 *
 * It finds the board, builds a list of 8-bit modes the board can show and
 * patches graphics.library + intuition.library so that:
 *
 *  - the modes appear in the display database (NextDisplayInfo,
 *    FindDisplayInfo, GetDisplayInfoData, ModeNotAvailable, Open/
 *    CloseMonitor) under ModeIDs 0x7A00xxxx;
 *  - OpenScreen on one of them gets a framebuffer in VRAM. Intuition is
 *    handed a harmless 1-plane chip-RAM "shadow" bitmap (every plane
 *    pointer aliases it) so its own planar rendering lands somewhere safe;
 *    M2 replaces this with real chunky rendering;
 *  - MakeVPort/MrgCop leave Prism viewports out of the native copper list,
 *    and LoadView programs the card and flips the monitor switch so the
 *    display follows the front screen;
 *  - palette changes on a Prism screen reach the card's DAC.
 *
 * Ctrl-C removes the patches again (once no Prism screen is open).
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/semaphores.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <graphics/gfxbase.h>
#include <graphics/displayinfo.h>
#include <graphics/monitor.h>
#include <graphics/view.h>
#include <intuition/intuitionbase.h>
#include <intuition/screens.h>
#include <utility/tagitem.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <proto/utility.h>
#include <proto/expansion.h>
#include <proto/icon.h>
#include <workbench/workbench.h>
#include <string.h>
#include <stddef.h>
#include <stdio.h>
#include <stdarg.h>
#include "prismboard.h"
#include "prism.h"
#include "prismint.h"
#include "prefs.h"

struct GfxBase       *GfxBase;
struct IntuitionBase *IntuitionBase;
struct Library       *UtilityBase;
struct ExpansionBase *ExpansionBase;

struct PrismBoard board;

/* libnix's write() calls __chkabort(), which exits the program on Ctrl-C.
 * PrismD must see Ctrl-C itself and take its patches out first. */
void __chkabort(void) { }

/* libnix gives every stdio stream a 64 KB buffer: stdin, stdout, stderr and
 * the prefs file came to 190 KB of a daemon that stays in memory for good,
 * a third of what PrismD took on a 68030 (2026-10-07). It prints a few
 * lines at start and the debug log; 1 KB buffers are plenty. */
static unsigned long stdioBufSize = 1024;
unsigned long *__BUFSIZE = &stdioBufSize;

/* Patches run in other tasks and must not call dos.library: they log into
 * this ring and the main loop prints it. */
static struct PrismPrefs prefs;
static char dbgRing[8192];
static volatile UWORD dbgHead, dbgTail;
static struct Task *dbgMain;

void dbg(const char *fmt, ...)
{
    char tmp[96];
    va_list ap;
    int i;

    if (!prefs.log)
        return;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    /* Disable, not Forbid: a trace build logs calls made from interrupts
     * too, and Permit() there may try to switch tasks */
    Disable();
    for (i = 0; tmp[i]; i++) {
        dbgRing[dbgHead] = tmp[i];
        dbgHead = (dbgHead + 1) % sizeof(dbgRing);
    }
    Enable();
    /* LOG=SYNC: wake the main loop (running at priority 10 then) to write
     * the line out now. Whatever the caller does next - loop for ever,
     * crash - the log already has this line, unless the caller sits in
     * Forbid(). The pointer is not serviced in this mode. */
    if (prefs.log == 2 && dbgMain)
        Signal(dbgMain, SIGBREAKF_CTRL_F);
}

/* The filesystem's buffers to the disk, now (ACTION_FLUSH). */
static void disk_flush(const char *volume)
{
    struct MsgPort *fs = DeviceProc((STRPTR)volume);
    if (fs)
        DoPkt(fs, ACTION_FLUSH, 0, 0, 0, 0, 0);
}

/* LOG=SYNC: the lines go to SYS:PrismD.log - appended, the file closed
 * again and the disk flushed each time, so that a machine that freezes a
 * moment later still has them (an open file, or a write still in the
 * filesystem's buffers, is lost with the power). */
static void dbg_flush_file(void)
{
    static char chunk[1024];
    BPTR f;
    int n = 0;

    while (dbgTail != dbgHead && n < (int)sizeof(chunk)) {
        chunk[n++] = dbgRing[dbgTail];
        dbgTail = (dbgTail + 1) % sizeof(dbgRing);
    }
    if (!n)
        return;
    if ((f = Open((STRPTR)"SYS:PrismD.log", MODE_READWRITE))) {
        Seek(f, 0, OFFSET_END);
        Write(f, chunk, n);
        Close(f);
        disk_flush("SYS:");
    }
}

static void dbg_flush(void);
#define STAGE(what) do { if (prefs.log == 2) { dbg("stage: " what "\n"); dbg_flush(); } } while (0)

static void dbg_flush(void)
{
    if (prefs.log == 2) {
        while (dbgTail != dbgHead)
            dbg_flush_file();
        return;
    }
    while (dbgTail != dbgHead) {
        putchar(dbgRing[dbgTail]);
        dbgTail = (dbgTail + 1) % sizeof(dbgRing);
    }
    fflush(stdout);
}

/* ---- modes ---------------------------------------------------------- */

struct ModeRec {
    ULONG id;
    struct PrismMode m;
    UBYTE maxDepth;           /* depth Intuition opens its screens with      */
    UBYTE dimDepth;           /* depth the display database reports: the real
                                 bits per pixel, as programs expect of an RTG
                                 mode (reqtools' mode requester filters on it) */
    UBYTE bpp;                   /* bytes per pixel: 1, 2, 3 or 4 */
    char  name[DISPLAYNAMELEN];
    /* A DisplayInfoHandle normally points at a graphics.library record;
     * keep some zeroes around ours in case anything peeks inside. */
    UBYTE pad[128];
};

static const char version[] __attribute__((used)) = "$VER: PrismD 1.0.2 (07.10.2026)";

#define MAX_MODES 32
static struct ModeRec modes[MAX_MODES];
static ULONG nmodes;

static struct MonitorSpec prismMonitor;
static char monName[] = "prism.monitor";

static struct ModeRec *mode_by_id(ULONG id)
{
    ULONG i;
    if (!IS_PRISM_ID(id))
        return NULL;
    for (i = 0; i < nmodes; i++)
        if (modes[i].id == id)
            return &modes[i];
    return NULL;
}

static struct ModeRec *mode_by_handle(APTR h)
{
    ULONG d = (ULONG)h - (ULONG)modes;
    if ((ULONG)h < (ULONG)modes || d >= nmodes * sizeof(struct ModeRec) ||
        d % sizeof(struct ModeRec))
        return NULL;
    return (struct ModeRec *)h;
}

/* One ModeRec per prefs slot the user left on and the board can show.
 * The ModeID comes from the slot (prefs.h), so it doesn't move when other
 * modes are switched off. 16- and 24/32-bit screens still have 256 pens;
 * their pixels are RGB in the board's format (pen -> colour when drawn). */
/* The nth format (from 0) the board has for a depth slot, or PF_COUNT.
 * 24 = three bytes a pixel, 32 = four; they are separate slots because on
 * the GD5434 they behave differently (32-bit has blitter text and fills,
 * 24-bit copies faster) and the user chooses. */
static UBYTE board_format(UBYTE bits, UBYTE nth)
{
    static const UBYTE f16[] = { PF_RGB565BE, PF_RGB565LE, PF_BGR565LE };
    static const UBYTE f24[] = { PF_BGR24, PF_RGB24 };
    static const UBYTE f32[] = { PF_BGRA32, PF_ARGB32, PF_RGBA32 };
    /* (a board lists only the formats it can really show, so the order
     * here only matters for a board that has more than one per depth) */
    const UBYTE *f = bits == 16 ? f16 : bits == 32 ? f32 : f24;
    UBYTE i, n = bits == 16 ? sizeof(f16) : bits == 32 ? sizeof(f32) : sizeof(f24);

    if (bits == 8)
        return nth ? PF_COUNT : PF_CLUT8;
    for (i = 0; i < n; i++)
        if ((board.formats & PF_BIT(f[i])) && !nth--)
            return f[i];
    return PF_COUNT;
}

/* fill r->m for slot i in the first format the board accepts */
static BOOL mode_fits(struct ModeRec *r, ULONG i, UBYTE bits, UBYTE hz)
{
    UBYTE nth, fmt;
    for (nth = 0; (fmt = board_format(bits, nth)) != PF_COUNT; nth++) {
        memset(&r->m, 0, sizeof(r->m));
        r->m.width = prefs_sizes[PREFS_SIZE(i)][0];
        r->m.height = prefs_sizes[PREFS_SIZE(i)][1];
        r->m.format = fmt;
        r->m.refresh = hz;
        if (board.checkMode(&board, &r->m))
            return TRUE;
    }
    return FALSE;
}

static void build_modes(void)
{
    ULONG i;

    for (i = 0; i < PREFS_NMODES && nmodes < MAX_MODES; i++) {
        struct ModeRec *r = &modes[nmodes];
        UBYTE bits = prefs_depths[PREFS_DEPTH(i)];

        if (!prefs.on[i])
            continue;
        memset(r, 0, sizeof(*r));
        if (!mode_fits(r, i, bits, prefs.hz[i])) {
            if (!prefs.hz[i])
                continue;
            printf("PrismD: %ux%u %u-bit can't do %u Hz, using its default\n",
                   r->m.width, r->m.height, bits, prefs.hz[i]);
            if (!mode_fits(r, i, bits, 0))
                continue;
        }
        r->id = PRISM_MONITOR_ID | PREFS_IDLOW(i);
        r->maxDepth = 8;
        r->bpp = pf_bpp(r->m.format);
        r->dimDepth = r->bpp == 4 ? 32 : bits;
        sprintf(r->name, "PRISM:%ux%u %ubit", r->m.width, r->m.height, r->bpp == 4 ? 32 : bits);
        nmodes++;
    }
}

/* ---- VRAM allocator: first fit over a sorted list of used blocks --- */

struct Blk { ULONG off, size; };
#define MAX_BLKS 256
static struct Blk used[MAX_BLKS];
static int nused;

LONG vram_alloc(ULONG size)
{
    ULONG pos = 0;
    int i, j;

    size = (size + 255) & ~255UL;
    if (nused == MAX_BLKS)
        return -1;
    for (i = 0; i <= nused; i++) {
        ULONG end = (i < nused) ? used[i].off : board.vramSize;
        if (end - pos >= size) {
            for (j = nused; j > i; j--)
                used[j] = used[j - 1];
            used[i].off = pos;
            used[i].size = size;
            nused++;
            return pos;
        }
        if (i < nused)
            pos = used[i].off + used[i].size;
    }
    return -1;
}

/* Free VRAM in bytes and the biggest free piece (lock held). */
ULONG vram_free_bytes(ULONG *largest)
{
    ULONG pos = 0, total = 0, big = 0;
    int i;
    for (i = 0; i <= nused; i++) {
        ULONG end = (i < nused) ? used[i].off : board.vramSize;
        ULONG gap = end > pos ? end - pos : 0;
        total += gap;
        if (gap > big) big = gap;
        if (i < nused)
            pos = used[i].off + used[i].size;
    }
    if (largest) *largest = big;
    return total;
}

void vram_free(ULONG off)
{
    int i;
    for (i = 0; i < nused; i++)
        if (used[i].off == off) {
            for (; i < nused - 1; i++)
                used[i] = used[i + 1];
            nused--;
            return;
        }
}

/* ---- Prism screens + card state ------------------------------------- */

struct PScreen {
    struct PScreen  *next;
    struct Screen   *screen;     /* NULL until OpenScreen returns        */
    struct PBitMap  *pbm;        /* the screen's chunky bitmap           */
    struct PBitMap  *front;      /* buffer ChangeVPBitMap shows, or NULL
                                    for pbm (double buffering)           */
    struct ModeRec  *mode;
    UBYTE            depth;
    BOOL             intuitionOwns; /* bitmap came from Intuition's own
                                       AllocBitMap: it frees it         */
    UWORD            penTab[256];   /* 16-bit screens: pen -> pixel     */
    ULONG            rgbTab[256];   /* pen -> 0x00RRGGBB, for cgx       */
    BOOL             painted;       /* 16-bit: background set to pen 0  */
    ULONG            pal32[256][3]; /* colours at full precision (ECS)  */
    UBYTE            palKnown[256];
};

/* A screen opened without an explicit SA_DisplayID (Workbench!) gets its
 * mode from Intuition's private prefs. While a task is inside the original
 * OpenScreen(TagList) we watch it look up a Prism mode, then hand it a Prism
 * bitmap when it allocates the displayable screen bitmap. */
static struct {
    struct Task     *task;
    struct ModeRec  *mr;
    struct PScreen  *ps;
} pending;

static void pending_saw(struct ModeRec *mr)
{
    if (mr && pending.task && pending.task == FindTask(NULL))
        pending.mr = mr;
}

struct SignalSemaphore lock;             /* see prismint.h                */
static struct PScreen *pscreens;
static struct PScreen *shown;            /* screen the card is showing    */
static struct ViewPort *shownVP;
static struct ModeRec *cardMode;         /* mode the card is set to       */
static struct PBitMap *shownPbm;         /* bitmap the card scans out     */
static ULONG showStamp;
static BOOL rtgOn;

BOOL pbm_is_shown(struct PBitMap *p)
{
    return p && p == shownPbm;
}

/* Point the card at a bitmap, paging it into VRAM first. Lock held. */
static BOOL show_pbm(struct PBitMap *p)
{
    shownPbm = NULL;                     /* the old one may be evicted now */
    if (!pbm_to_vram(p)) {
        dbg("show: %ux%u doesn't fit in VRAM\n", p->w, p->h);
        return FALSE;
    }
    board.setDisplayStart(&board, p->vramOff);
    shownPbm = p;
    p->lastShown = ++showStamp;
    return TRUE;
}

/* pbm_free: a screen buffer is going; show its screen's own bitmap
 * instead if it was on display. Lock held. */
void pbm_gone(struct PBitMap *p)
{
    struct PScreen *ps;
    for (ps = pscreens; ps; ps = ps->next)
        if (ps->front == p)
            ps->front = NULL;
    if (shownPbm == p) {
        shownPbm = NULL;
        if (shown && shown->pbm && shown->pbm != p)
            show_pbm(shown->pbm);
    }
}

/* Intuition renders through its own copy of the bitmap we hand it
 * (&Screen->BitMap); pbm_get() recognises copies too. */
static struct PScreen *ps_by_screen(struct Screen *s)
{
    struct PScreen *ps;
    struct PBitMap *p;
    if (!s)
        return NULL;
    p = pbm_get(s->RastPort.BitMap);
    for (ps = pscreens; ps; ps = ps->next)
        if (ps->screen == s || (p && ps->pbm == p))
            return ps;
    return NULL;
}

static ULONG palTab[256 * 3];
static UBYTE palRGB[256 * 3];

/* A screen's colours at full precision into palTab. On ECS a ColorMap
 * keeps only 4 bits a gun, so GetRGB32() alone turns a 256-colour picture
 * into 4096-colour posterisation (seen on the A2000). The 32-bit values
 * seen going into SetRGB32 / LoadRGB32 / SetRGB32CM / ObtainPen are kept
 * per screen (ps->pal32) and win while the ColorMap still agrees with them
 * in its top 4 bits; a colour changed some way we didn't see falls back to
 * the ColorMap. On AGA the ColorMap holds 8 bits and this changes nothing. */
static struct PScreen *ps_by_vp(struct ViewPort *vp);

static void ps_colours(struct PScreen *ps, struct ColorMap *cm, ULONG n)
{
    ULONG i, k;
    GetRGB32(cm, 0, n, palTab);
    if (!ps)
        return;
    for (i = 0; i < n; i++) {
        if (!ps->palKnown[i])
            continue;
        for (k = 0; k < 3; k++)
            if ((ps->pal32[i][k] ^ palTab[i * 3 + k]) & 0xf0000000UL)
                break;
        if (k < 3) {
            ps->palKnown[i] = 0;        /* changed behind our back */
            continue;
        }
        for (k = 0; k < 3; k++)
            palTab[i * 3 + k] = ps->pal32[i][k];
    }
}

/* Remember a colour at full precision. Lock held. */
static void ps_record(struct PScreen *ps, ULONG n, ULONG r, ULONG g, ULONG b)
{
    if (!ps || n > 255)
        return;
    ps->pal32[n][0] = r;
    ps->pal32[n][1] = g;
    ps->pal32[n][2] = b;
    ps->palKnown[n] = 1;
}

/* Copy a screen's colours to the card. Caller holds the lock. */
static void push_palette(struct ViewPort *vp)
{
    ULONG n, i;
    if (!vp || !vp->ColorMap)
        return;
    n = vp->ColorMap->Count;
    if (n > 256) n = 256;
    /* the DAC only matters to an 8-bit screen; deeper screens keep their
     * colours in their pen tables */
    if (!shown || shown->mode->bpp == 1) {
        ps_colours(ps_by_vp(vp), vp->ColorMap, n);
        for (i = 0; i < n * 3; i++)
            palRGB[i] = palTab[i] >> 24;
        board.setPalette(&board, 0, n, palRGB);
    }
    pointer_colours(vp);
}

/* The Prism screen a ViewPort belongs to: its Screen, or (while Intuition
 * is still opening it) the Screen the ViewPort is embedded in. */
static struct PScreen *ps_by_vp(struct ViewPort *vp)
{
    struct PScreen *ps;
    struct Screen *s;
    struct PBitMap *p;

    if (!vp)
        return NULL;
    for (ps = pscreens; ps; ps = ps->next)
        if (ps->screen && &ps->screen->ViewPort == vp)
            return ps;
    s = (struct Screen *)((UBYTE *)vp - offsetof(struct Screen, ViewPort));
    if ((p = pbm_get(s->RastPort.BitMap)))
        for (ps = pscreens; ps; ps = ps->next)
            if (ps->pbm == p)
                return ps;
    return NULL;
}

/* A screen's colours changed: rebuild its pen table (16-bit) and, if it
 * is the screen on the card, reload the DAC and pointer. Lock held. */
static void palette_update(struct ViewPort *vp)
{
    struct PScreen *ps = ps_by_vp(vp);
    ULONG n, i;

    if (!ps || !vp->ColorMap)
        return;
    n = vp->ColorMap->Count;
    if (n > 256) n = 256;
    ps_colours(ps, vp->ColorMap, n);
    for (i = 0; i < n; i++)
        ps->rgbTab[i] = ((palTab[i * 3] >> 8) & 0xff0000) | ((palTab[i * 3 + 1] >> 16) & 0xff00) |
                        (palTab[i * 3 + 2] >> 24);
    if (ps->mode->bpp == 2)
        for (i = 0; i < n; i++)
            ps->penTab[i] = rgb16(ps->mode->m.format, palTab[i * 3] >> 24,
                                  palTab[i * 3 + 1] >> 24, palTab[i * 3 + 2] >> 24);
    /* A cleared 8-bit bitmap is pen 0, which Intuition relies on and never
     * paints. A 16/24/32-bit one is black, so the first time the colours
     * are known (early in OpenScreen) paint it pen 0. */
    if (ps->mode->bpp >= 2 && !ps->painted && ps->pbm) {
        ps->painted = TRUE;
        pbm_fill_pen(ps->pbm, 0);
    }
    if (vp == shownVP)
        push_palette(vp);
}

/* Make the card follow the front screen. */
static void update_display(void)
{
    struct Screen *s = IntuitionBase->FirstScreen;
    struct PScreen *ps;

    ObtainSemaphore(&lock);
    ps = ps_by_screen(s);
    dbg("LoadView: front %lx %s\n", (ULONG)s, ps ? "PRISM" : "native");
    if (ps) {
        struct PBitMap *dp = ps->front ? ps->front : ps->pbm;
        if (cardMode != ps->mode) {
            struct PrismMode m = ps->mode->m;
            board.setMode(&board, &m);
            cardMode = ps->mode;
            shown = NULL;
        }
        if (shown != ps || shownPbm != dp)
            show_pbm(dp);
        if (shown != ps) {
            shown = ps;
            shownVP = &s->ViewPort;
            push_palette(shownVP);
            pointer_on(shownVP);          /* a mode set turns it off too */
        }
        if (!rtgOn) {
            board.setSwitch(&board, TRUE);
            rtgOn = TRUE;
        }
    } else {
        pointer_off();
        if (rtgOn) {
            board.setSwitch(&board, FALSE);
            rtgOn = FALSE;
        }
        /* program the mode again on the way back: WinUAE only re-enables
         * its RTG view on a mode change, and it costs nothing on hardware */
        cardMode = NULL;
        shown = NULL;
        shownVP = NULL;
        shownPbm = NULL;
    }
    ReleaseSemaphore(&lock);
}

/* ---- calling the original functions -------------------------------- */

static APTR o_NextDisplayInfo, o_FindDisplayInfo, o_GetDisplayInfoData,
            o_ModeNotAvailable, o_OpenMonitor, o_CloseMonitor, o_MakeVPort,
            o_MrgCop, o_LoadView, o_SetRGB32, o_LoadRGB32, o_SetRGB4,
            o_LoadRGB4, o_OpenScreenTagList, o_OpenScreen, o_CloseScreen,
            o_DisplayAlert, o_TimedDisplayAlert, o_Alert;

#define BASE(b) register APTR _a6 __asm("a6") = (b)

static ULONG c_d0(APTR fn, APTR base, ULONG x)
{
    register ULONG d0 __asm("d0") = x;
    BASE(base);
    __asm volatile ("jsr (%[f])" : "+r"(d0) : "r"(_a6), [f]"a"(fn)
                    : "d1", "a0", "a1", "cc", "memory");
    return d0;
}

static ULONG c_a0(APTR fn, APTR base, APTR x)
{
    register APTR a0 __asm("a0") = x;
    register ULONG d0 __asm("d0");
    BASE(base);
    __asm volatile ("jsr (%[f])" : "=r"(d0), "+r"(a0) : "r"(_a6), [f]"a"(fn)
                    : "d1", "a1", "cc", "memory");
    return d0;
}

static ULONG c_a1(APTR fn, APTR base, APTR x)
{
    register APTR a1 __asm("a1") = x;
    register ULONG d0 __asm("d0");
    BASE(base);
    __asm volatile ("jsr (%[f])" : "=r"(d0), "+r"(a1) : "r"(_a6), [f]"a"(fn)
                    : "d1", "a0", "cc", "memory");
    return d0;
}

static ULONG c_a0a1(APTR fn, APTR base, APTR x, APTR y)
{
    register APTR a0 __asm("a0") = x;
    register APTR a1 __asm("a1") = y;
    register ULONG d0 __asm("d0");
    BASE(base);
    __asm volatile ("jsr (%[f])" : "=r"(d0), "+r"(a0), "+r"(a1) : "r"(_a6), [f]"a"(fn)
                    : "d1", "cc", "memory");
    return d0;
}

static ULONG c_a1d0(APTR fn, APTR base, APTR x, ULONG y)
{
    register APTR a1 __asm("a1") = x;
    register ULONG d0 __asm("d0") = y;
    BASE(base);
    __asm volatile ("jsr (%[f])" : "+r"(d0), "+r"(a1) : "r"(_a6), [f]"a"(fn)
                    : "d1", "a0", "cc", "memory");
    return d0;
}

static void c_a0a1d0(APTR fn, APTR base, APTR x, APTR y, ULONG z)
{
    register APTR a0 __asm("a0") = x;
    register APTR a1 __asm("a1") = y;
    register ULONG d0 __asm("d0") = z;
    BASE(base);
    __asm volatile ("jsr (%[f])" : "+r"(d0), "+r"(a0), "+r"(a1) : "r"(_a6), [f]"a"(fn)
                    : "d1", "cc", "memory");
}

static void c_a0d0d1d2d3(APTR fn, APTR base, APTR x, ULONG a, ULONG b, ULONG c, ULONG d)
{
    register APTR a0 __asm("a0") = x;
    register ULONG d0 __asm("d0") = a;
    register ULONG d1 __asm("d1") = b;
    register ULONG d2 __asm("d2") = c;
    register ULONG d3 __asm("d3") = d;
    BASE(base);
    __asm volatile ("jsr (%[f])" : "+r"(d0), "+r"(d1), "+r"(a0)
                    : "r"(d2), "r"(d3), "r"(_a6), [f]"a"(fn)
                    : "a1", "cc", "memory");
}

static ULONG c_a0a1d0d1d2(APTR fn, APTR base, APTR x, APTR y, ULONG a, ULONG b, ULONG c)
{
    register APTR a0 __asm("a0") = x;
    register APTR a1 __asm("a1") = y;
    register ULONG d0 __asm("d0") = a;
    register ULONG d1 __asm("d1") = b;
    register ULONG d2 __asm("d2") = c;
    BASE(base);
    __asm volatile ("jsr (%[f])" : "+r"(d0), "+r"(d1), "+r"(a0), "+r"(a1)
                    : "r"(d2), "r"(_a6), [f]"a"(fn)
                    : "cc", "memory");
    return d0;
}

/* Patches can be running in other tasks when PrismD wants to quit. */
static volatile LONG inPatch;
/* one instruction each, so a task switch can't split the update */
#define ENTER() __asm volatile ("addq.l #1,_inPatch" ::: "cc", "memory")
#define LEAVE() __asm volatile ("subq.l #1,_inPatch" ::: "cc", "memory")

static BOOL vp_is_prism(struct ViewPort *vp)
{
    return vp && vp->ColorMap && IS_PRISM_ID(GetVPModeID(vp));
}

/* ---- display database patches -------------------------------------- */

static ULONG P_NextDisplayInfo(ULONG id __asm("d0"))
{
    ULONG r;
    ENTER();
    if (IS_PRISM_ID(id)) {
        struct ModeRec *m = mode_by_id(id);
        ULONG i = m ? (ULONG)(m - modes) + 1 : nmodes;
        r = (i < nmodes) ? modes[i].id : INVALID_ID;
    } else {
        r = c_d0(o_NextDisplayInfo, GfxBase, id);
        if (r == INVALID_ID && nmodes)
            r = modes[0].id;
    }
    LEAVE();
    return r;
}

static APTR P_FindDisplayInfo(ULONG id __asm("d0"))
{
    APTR r;
    ENTER();
    r = mode_by_id(id);
    pending_saw(r);
    if (IS_PRISM_ID(id)) dbg("FindDisplayInfo %lx -> %lx\n", id, (ULONG)r);
    if (!r)
        r = (APTR)c_d0(o_FindDisplayInfo, GfxBase, id);
    LEAVE();
    return r;
}

static LONG P_ModeNotAvailable(ULONG id __asm("d0"))
{
    LONG r;
    ENTER();
    r = mode_by_id(id) ? 0 : (LONG)c_d0(o_ModeNotAvailable, GfxBase, id);
    if (IS_PRISM_ID(id)) dbg("ModeNotAvailable %lx -> %ld\n", id, r);
    LEAVE();
    return r;
}

static void qhdr(struct QueryHeader *q, ULONG tag, ULONG id, ULONG size)
{
    q->StructID = tag;
    q->DisplayID = id;
    q->SkipID = TAG_SKIP;
    q->Length = (size + 7) / 8;
}

static ULONG P_GetDisplayInfoData(APTR h __asm("a0"), APTR buf __asm("a1"),
                                  ULONG size __asm("d0"), ULONG tag __asm("d1"),
                                  ULONG id __asm("d2"))
{
    union {
        struct DisplayInfo   di;
        struct DimensionInfo dm;
        struct MonitorInfo   mi;
        struct NameInfo      ni;
    } u;
    struct ModeRec *r;
    ULONG len = 0;

    ENTER();
    r = h ? mode_by_handle(h) : mode_by_id(id);
    if (!r) {
        len = c_a0a1d0d1d2(o_GetDisplayInfoData, GfxBase, h, buf, size, tag, id);
        LEAVE();
        return len;
    }
    pending_saw(r);
    dbg("GetDisplayInfoData %lx tag %lx size %lu\n", r->id, tag, size);
    memset(&u, 0, sizeof(u));
    switch (tag) {
    case DTAG_DISP:
        len = sizeof(u.di);
        qhdr(&u.di.Header, tag, r->id, len);
        u.di.PropertyFlags = DIPF_IS_WB | DIPF_IS_FOREIGN;
        u.di.Resolution.x = 22;
        u.di.Resolution.y = 22;
        u.di.PixelSpeed = 35;
        u.di.PaletteRange = 4096;
        u.di.NumStdSprites = 1;           /* the hardware cursor          */
        u.di.SpriteResolution.x = 22;     /* sprite pixels = screen pixels */
        u.di.SpriteResolution.y = 22;
        u.di.RedBits = u.di.GreenBits = u.di.BlueBits = 8;
        break;
    case DTAG_DIMS:
        len = sizeof(u.dm);
        qhdr(&u.dm.Header, tag, r->id, len);
        u.dm.MaxDepth = r->dimDepth;
        u.dm.MinRasterWidth = 16;
        u.dm.MinRasterHeight = 16;
        u.dm.MaxRasterWidth = r->m.width;
        u.dm.MaxRasterHeight = r->m.height;
        u.dm.Nominal.MaxX = r->m.width - 1;
        u.dm.Nominal.MaxY = r->m.height - 1;
        u.dm.MaxOScan = u.dm.VideoOScan = u.dm.TxtOScan = u.dm.StdOScan = u.dm.Nominal;
        break;
    case DTAG_MNTR:
        len = sizeof(u.mi);
        qhdr(&u.mi.Header, tag, r->id, len);
        u.mi.Mspc = &prismMonitor;
        u.mi.ViewResolution.x = 22;
        u.mi.ViewResolution.y = 22;
        u.mi.ViewPositionRange.MaxX = r->m.width - 1;
        u.mi.ViewPositionRange.MaxY = r->m.height - 1;
        u.mi.TotalRows = r->m.height;
        u.mi.TotalColorClocks = r->m.width;
        u.mi.Compatibility = MCOMPAT_NOBODY;
        /* ticks the pointer moves per mouse count: one pixel, as P96
         * reports it. With 1,1 here Intuition moved the pointer at half
         * speed (measured on the A4000 with relative mouse events). */
        u.mi.MouseTicks.x = 22;
        u.mi.MouseTicks.y = 22;
        u.mi.PreferredModeID = r->id;
        break;
    case DTAG_NAME:
        len = sizeof(u.ni);
        qhdr(&u.ni.Header, tag, r->id, len);
        strncpy((char *)u.ni.Name, r->name, DISPLAYNAMELEN - 1);
        break;
    default:
        LEAVE();
        return 0;
    }
    if (len > size)
        len = size;
    CopyMem(&u, buf, len);
    LEAVE();
    return len;
}

static struct MonitorSpec *P_OpenMonitor(CONST_STRPTR name __asm("a1"), ULONG id __asm("d0"))
{
    struct MonitorSpec *r;
    ENTER();
    if ((name && !strcmp((const char *)name, monName)) || (!name && mode_by_id(id))) {
        Forbid();
        prismMonitor.ms_OpenCount++;
        Permit();
        r = &prismMonitor;
    } else {
        r = (struct MonitorSpec *)c_a1d0(o_OpenMonitor, GfxBase, (APTR)name, id);
    }
    if (IS_PRISM_ID(id) || name)
        dbg("OpenMonitor '%s' %lx -> %lx\n", name ? (const char *)name : "-", id, (ULONG)r);
    LEAVE();
    return r;
}

static BOOL P_CloseMonitor(struct MonitorSpec *ms __asm("a0"))
{
    BOOL r;
    ENTER();
    if (ms == &prismMonitor) {
        Forbid();
        if (prismMonitor.ms_OpenCount)
            prismMonitor.ms_OpenCount--;
        Permit();
        r = TRUE;
    } else {
        r = c_a0(o_CloseMonitor, GfxBase, ms);
    }
    LEAVE();
    return r;
}

/* ---- view patches --------------------------------------------------- */

static ULONG P_MakeVPort(struct View *v __asm("a0"), struct ViewPort *vp __asm("a1"))
{
    ULONG r;
    ENTER();
    /* Prism viewports have no copper list: the card does the display */
    r = vp_is_prism(vp) ? MVP_OK : c_a0a1(o_MakeVPort, GfxBase, v, vp);
    LEAVE();
    return r;
}

static ULONG P_MrgCop(struct View *v __asm("a1"))
{
    struct ViewPort *all[32], *head = NULL, *tail = NULL, *vp, *lastNext;
    int n = 0, i;
    ULONG r;

    ENTER();
    if (!v) {
        r = c_a1(o_MrgCop, GfxBase, v);
        LEAVE();
        return r;
    }
    /* take Prism viewports out of the chain while the copper list is
     * merged, then put the chain back exactly as it was */
    for (vp = v->ViewPort; vp && n < 32; vp = vp->Next)
        all[n++] = vp;
    lastNext = n ? all[n - 1]->Next : NULL;
    for (i = 0; i < n; i++)
        if (!vp_is_prism(all[i])) {
            if (tail) tail->Next = all[i]; else head = all[i];
            tail = all[i];
        }
    if (tail)
        tail->Next = NULL;
    if (n)
        v->ViewPort = head;

    r = c_a1(o_MrgCop, GfxBase, v);

    if (n) {
        for (i = 0; i < n - 1; i++)
            all[i]->Next = all[i + 1];
        all[n - 1]->Next = lastNext;
        v->ViewPort = all[0];
    }
    LEAVE();
    return r;
}

static void P_LoadView(struct View *v __asm("a1"))
{
    ENTER();
    c_a1(o_LoadView, GfxBase, v);
    update_display();
    LEAVE();
}

/* ---- palette patches ------------------------------------------------ */

/* One colour changed (SetRGB32, SetRGB4): only that entry of the screen's
 * tables and of the card's palette. Going through palette_update for each
 * made a 256-colour palette set one entry at a time - what opening a
 * screen does - cost 256 times 256 entries (SysSpeed's OpenScr: one 8-bit
 * screen a second). r, g, b are 32-bit left-justified. */
static void palette_one(struct ViewPort *vp, ULONG n, ULONG r, ULONG g, ULONG b)
{
    struct PScreen *ps;

    ObtainSemaphore(&lock);
    ps = ps_by_vp(vp);
    if (ps && n < 256 && vp->ColorMap && n < vp->ColorMap->Count) {
        if (ps->mode->bpp >= 2 && !ps->painted) {
            palette_update(vp);                  /* first colours: the full job */
        } else {
            UBYTE c[3];
            c[0] = r >> 24; c[1] = g >> 24; c[2] = b >> 24;
            ps->rgbTab[n] = ((ULONG)c[0] << 16) | ((ULONG)c[1] << 8) | c[2];
            if (ps->mode->bpp == 2)
                ps->penTab[n] = rgb16(ps->mode->m.format, c[0], c[1], c[2]);
            if (vp == shownVP) {
                if (!shown || shown->mode->bpp == 1)
                    board.setPalette(&board, n, 1, c);
                if (n >= 16 && n < 32)           /* the pointer's colours live here */
                    pointer_colours(vp);
            }
        }
    }
    ReleaseSemaphore(&lock);
}

static void palette_changed(struct ViewPort *vp)
{
    ObtainSemaphore(&lock);
    palette_update(vp);
    ReleaseSemaphore(&lock);
}

static void record_vp(struct ViewPort *vp, ULONG n, ULONG r, ULONG g, ULONG b)
{
    ObtainSemaphore(&lock);
    ps_record(ps_by_vp(vp), n, r, g, b);
    ReleaseSemaphore(&lock);
}

static void P_SetRGB32(struct ViewPort *vp __asm("a0"), ULONG n __asm("d0"),
                       ULONG r __asm("d1"), ULONG g __asm("d2"), ULONG b __asm("d3"))
{
    ENTER();
    record_vp(vp, n, r, g, b);
    c_a0d0d1d2d3(o_SetRGB32, GfxBase, vp, n, r, g, b);
    palette_one(vp, n, r, g, b);
    LEAVE();
}

static void P_SetRGB4(struct ViewPort *vp __asm("a0"), ULONG n __asm("d0"),
                      ULONG r __asm("d1"), ULONG g __asm("d2"), ULONG b __asm("d3"))
{
    ENTER();
    c_a0d0d1d2d3(o_SetRGB4, GfxBase, vp, n, r, g, b);
    palette_one(vp, n, (r & 15) * 0x11111111UL, (g & 15) * 0x11111111UL, (b & 15) * 0x11111111UL);
    LEAVE();
}

static void P_LoadRGB32(struct ViewPort *vp __asm("a0"), const ULONG *t __asm("a1"))
{
    ENTER();
    if (t) {
        /* (count << 16 | first), count * 3 ULONGs, ..., 0 */
        const ULONG *q = t;
        ObtainSemaphore(&lock);
        {
            struct PScreen *ps = ps_by_vp(vp);
            while (ps && *q) {
                ULONG cnt = *q >> 16, first = *q & 0xffff, k;
                q++;
                for (k = 0; k < cnt; k++, q += 3)
                    ps_record(ps, first + k, q[0], q[1], q[2]);
            }
        }
        ReleaseSemaphore(&lock);
    }
    c_a0a1(o_LoadRGB32, GfxBase, vp, (APTR)t);
    {
        /* a few colours (graphics' own SetRGB32 loads one this way): just
         * those entries; a whole palette: the full update */
        const ULONG *q = t;
        ULONG total = 0;
        while (q && *q && total <= 16) {
            total += *q >> 16;
            q += 1 + (*q >> 16) * 3;
        }
        if (t && total <= 16) {
            for (q = t; *q; ) {
                ULONG cnt = *q >> 16, first = *q & 0xffff, k;
                q++;
                for (k = 0; k < cnt; k++, q += 3)
                    palette_one(vp, first + k, q[0], q[1], q[2]);
            }
        } else {
            palette_changed(vp);
        }
    }
    LEAVE();
}

static void P_LoadRGB4(struct ViewPort *vp __asm("a0"), const UWORD *t __asm("a1"),
                       ULONG n __asm("d0"))
{
    ENTER();
    c_a0a1d0(o_LoadRGB4, GfxBase, vp, (APTR)t, n);
    palette_changed(vp);
    LEAVE();
}

/* ---- screen patches ------------------------------------------------- */

static Tag filterOut[] = {
    SA_BitMap, SA_Width, SA_Height, SA_Depth, SA_DisplayID, SA_Overscan,
    SA_DClip, SA_AutoScroll, TAG_DONE
};

static ULONG tag_data(struct TagItem *tags, struct TagItem *ext, Tag t, ULONG def)
{
    struct TagItem *ti;
    if (tags && (ti = FindTagItem(t, tags)))
        return ti->ti_Data;
    if (ext && (ti = FindTagItem(t, ext)))
        return ti->ti_Data;
    return def;
}

/* Called from the AllocBitMap patch: is this the displayable bitmap of a
 * screen Intuition is opening on a Prism mode? */
struct PBitMap *screen_bitmap_hook(ULONG w, ULONG h, ULONG depth, ULONG flags)
{
    struct ModeRec *mr = pending.mr;
    struct PScreen *ps;

    if (!pending.task || pending.task != FindTask(NULL) || pending.ps || !mr)
        return NULL;
    dbg("  AllocBitMap %lux%lux%lu flags %lx during open (mode %s)\n", w, h, depth, flags, mr->name);
    if (!(flags & BMF_DISPLAYABLE) || depth > mr->maxDepth ||
        w != mr->m.width || h != mr->m.height)
        return NULL;
    if (!(ps = AllocVec(sizeof(*ps), MEMF_PUBLIC | MEMF_CLEAR)))
        return NULL;
    if (!(ps->pbm = pbm_new(w, h, depth ? depth : 1, mr->bpp,
                            mr->bpp == 2 ? ps->penTab : NULL, TRUE, TRUE)) ||
        ps->pbm->bpr != mr->m.bytesPerRow) {
        pbm_free(ps->pbm);
        FreeVec(ps);
        return NULL;
    }
    ps->pbm->fmt = mr->m.format;
    ps->pbm->rgbTab = ps->rgbTab;
    ps->pbm->owner = ps;
    ps->mode = mr;
    ps->depth = depth;
    ps->intuitionOwns = TRUE;
    ObtainSemaphore(&lock);
    ps->next = pscreens;
    pscreens = ps;
    ReleaseSemaphore(&lock);
    pending.ps = ps;
    return ps->pbm;
}

static void free_ps(struct PScreen *ps);

/* Start watching an OpenScreen that names no mode (one at a time). */
static BOOL pending_begin(void)
{
    BOOL ok = FALSE;
    Forbid();
    if (!pending.task) {
        pending.task = FindTask(NULL);
        pending.mr = NULL;
        pending.ps = NULL;
        ok = TRUE;
    }
    Permit();
    return ok;
}

static void pending_end(struct Screen *s)
{
    struct PScreen *ps = pending.ps;
    if (ps) {
        if (s) {
            ps->screen = s;
            ObtainSemaphore(&lock);
            palette_update(&s->ViewPort);       /* fill a 16-bit pen table */
            ReleaseSemaphore(&lock);
            dbg("  screen %lx opened by Intuition on %s, %s\n", (ULONG)s,
                ps->mode->name, ps->pbm->inVram ? "in VRAM" : "in fast RAM");
        } else {
            /* Intuition already freed the bitmap on the way out */
            ps->pbm = NULL;
            free_ps(ps);
        }
    }
    pending.ps = NULL;
    pending.mr = NULL;
    pending.task = NULL;
}

static void free_ps(struct PScreen *ps)
{
    ObtainSemaphore(&lock);
    if (pscreens == ps) {
        pscreens = ps->next;
    } else {
        struct PScreen *p;
        for (p = pscreens; p && p->next != ps; p = p->next) ;
        if (p) p->next = ps->next;
    }
    if (shown == ps) {
        shown = NULL;
        shownVP = NULL;
        shownPbm = NULL;
    }
    ReleaseSemaphore(&lock);
    if (!ps->intuitionOwns)
        pbm_free(ps->pbm);
    FreeVec(ps);
}

/* OpenScreen(TagList) on a Prism mode. */
static struct Screen *open_prism_screen(struct ModeRec *mr, struct NewScreen *ns,
                                        struct TagItem *tags, struct TagItem *ext)
{
    struct ExtNewScreen ens;
    struct TagItem newTags[7], *clone = NULL, *extClone = NULL;
    struct PScreen *ps;
    struct Screen *s;
    ULONG w = mr->m.width, h = mr->m.height, depth;

    BOOL viaHook;

    depth = tag_data(tags, ext, SA_Depth, ns ? ns->Depth : 8);
    if (depth < 1) depth = 1;
    if (depth > mr->maxDepth) depth = mr->maxDepth;

    /* Preferred: Intuition allocates the screen's bitmap itself and the
     * AllocBitMap hook hands it ours (as for Workbench), so the screen is an
     * ordinary one - not CUSTOMBITMAP, which AllocScreenBuffer() refuses.
     * If another open is being watched, fall back to SA_BitMap. */
    if ((viaHook = pending_begin())) {
        pending.mr = mr;
        ps = NULL;
        goto tags;
    }

    if (!(ps = AllocVec(sizeof(*ps), MEMF_PUBLIC | MEMF_CLEAR)))
        return NULL;
    ps->mode = mr;
    ps->depth = depth;

    /* the screen's chunky bitmap, cleared, in VRAM if it fits (else fast
     * RAM until it is shown); its row size must be the mode's (modes are
     * multiples of 8 pixels wide, so it is) */
    if (!(ps->pbm = pbm_new(w, h, depth, mr->bpp, mr->bpp == 2 ? ps->penTab : NULL,
                            TRUE, TRUE)) || ps->pbm->bpr != mr->m.bytesPerRow) {
        dbg("open: no memory for %lux%lu\n", w, h);
        pbm_free(ps->pbm);
        FreeVec(ps);
        return NULL;
    }
    ps->pbm->fmt = mr->m.format;
    ps->pbm->rgbTab = ps->rgbTab;
    ps->pbm->owner = ps;

    ObtainSemaphore(&lock);
    ps->next = pscreens;
    pscreens = ps;
    ReleaseSemaphore(&lock);

    /* Our size/depth/bitmap win; everything else the caller asked for
     * passes through. */
tags:
    if (tags && (clone = CloneTagItems(tags)))
        FilterTagItems(clone, filterOut, TAGFILTER_NOT);
    if (ns) {
        memset(&ens, 0, sizeof(ens));
        CopyMem(ns, &ens, (ns->Type & NS_EXTENDED) ? sizeof(struct ExtNewScreen)
                                                   : sizeof(struct NewScreen));
        ens.Width = w;
        ens.Height = h;
        ens.Depth = depth;
        ens.Type &= ~CUSTOMBITMAP;
        ens.CustomBitMap = NULL;
        if (ext && (extClone = CloneTagItems(ext))) {
            FilterTagItems(extClone, filterOut, TAGFILTER_NOT);
            ens.Extension = extClone;
        }
    }
    newTags[0].ti_Tag = viaHook ? TAG_IGNORE : SA_BitMap;
    newTags[0].ti_Data = viaHook ? 0 : (ULONG)ps->pbm->bm;
    newTags[1].ti_Tag = SA_Width;     newTags[1].ti_Data = w;
    newTags[2].ti_Tag = SA_Height;    newTags[2].ti_Data = h;
    newTags[3].ti_Tag = SA_Depth;     newTags[3].ti_Data = depth;
    newTags[4].ti_Tag = SA_DisplayID; newTags[4].ti_Data = mr->id;
    newTags[5].ti_Tag = clone ? TAG_MORE : TAG_DONE;
    newTags[5].ti_Data = (ULONG)clone;
    newTags[6].ti_Tag = TAG_DONE;

    s = (struct Screen *)c_a0a1(o_OpenScreenTagList, IntuitionBase,
                                ns ? (APTR)&ens : NULL, newTags);
    if (clone) FreeTagItems(clone);
    if (extClone) FreeTagItems(extClone);
    if (!s) dbg("  original OpenScreenTagList failed\n");
    if (viaHook) {
        if (s && !pending.ps)
            dbg("  screen %lx: Intuition didn't take a Prism bitmap\n", (ULONG)s);
        pending_end(s);
        return s;
    }
    if (!s) {
        free_ps(ps);
        return NULL;
    }
    dbg("  screen %lx: rp.bm=%lx ours=%lx %s\n", (ULONG)s, (ULONG)s->RastPort.BitMap,
        (ULONG)ps->pbm->bm, ps->pbm->inVram ? "in VRAM" : "in fast RAM");
    ps->screen = s;
    ObtainSemaphore(&lock);
    palette_update(&s->ViewPort);               /* fill a 16-bit pen table */
    ReleaseSemaphore(&lock);
    return s;
}

static struct ModeRec *wanted_mode(struct NewScreen *ns, struct TagItem *tags,
                                   struct TagItem **extOut)
{
    struct TagItem *ext = NULL;
    if (ns && (ns->Type & NS_EXTENDED))
        ext = ((struct ExtNewScreen *)ns)->Extension;
    *extOut = ext;
    return mode_by_id(tag_data(tags, ext, SA_DisplayID, INVALID_ID));
}

static struct Screen *P_OpenScreenTagList(struct NewScreen *ns __asm("a0"),
                                          struct TagItem *tags __asm("a1"))
{
    struct TagItem *ext;
    struct ModeRec *mr;
    struct Screen *s;

    ENTER();
    mr = wanted_mode(ns, tags, &ext);
    dbg("OpenScreenTagList ns=%lx tags=%lx id=%lx -> mode %s\n", (ULONG)ns, (ULONG)tags,
        tag_data(tags, ext, SA_DisplayID, INVALID_ID), mr ? mr->name : "-");
    if (mr) {
        s = open_prism_screen(mr, ns, tags, ext);
    } else {
        BOOL watch = pending_begin();
        s = (struct Screen *)c_a0a1(o_OpenScreenTagList, IntuitionBase, ns, tags);
        if (watch)
            pending_end(s);
    }
    LEAVE();
    return s;
}

static struct Screen *P_OpenScreen(struct NewScreen *ns __asm("a0"))
{
    struct TagItem *ext;
    struct ModeRec *mr;
    struct Screen *s;

    ENTER();
    if ((mr = wanted_mode(ns, NULL, &ext))) {
        s = open_prism_screen(mr, ns, NULL, ext);
    } else {
        BOOL watch = pending_begin();
        s = (struct Screen *)c_a0(o_OpenScreen, IntuitionBase, ns);
        if (watch)
            pending_end(s);
    }
    LEAVE();
    return s;
}

static BOOL P_CloseScreen(struct Screen *s __asm("a0"))
{
    struct PScreen *ps;
    BOOL r;

    ENTER();
    ObtainSemaphore(&lock);
    ps = ps_by_screen(s);
    ReleaseSemaphore(&lock);
    r = c_a0(o_CloseScreen, IntuitionBase, s);
    if (r && ps)
        free_ps(ps);
    LEAVE();
    return r;
}

/* ---- double buffering ----------------------------------------------- */

/* ChangeVPBitMap(vp a0, bm a1, dbi a2) - what Intuition's
 * ChangeScreenBuffer() calls. On a Prism screen the card's display start
 * moves to the new buffer (paged into VRAM if needed) and takes effect at
 * the next vertical blank; then the DBufInfo's messages go out: "displayed"
 * and "safe to draw into the old one". */
LONG h_ChangeVPBitMap(struct Regs *r)
{
    struct ViewPort *vp = (struct ViewPort *)r->a[0];
    struct BitMap *bm = (struct BitMap *)r->a[1];
    struct DBufInfo *db = (struct DBufInfo *)r->a[2];
    struct PScreen *ps;
    struct PBitMap *p;

    ObtainSemaphore(&lock);
    ps = ps_by_vp(vp);
    p = pbm_get(bm);
    if (!ps || !p || (p != ps->pbm && p->owner != ps)) {
        ReleaseSemaphore(&lock);
        return 0;
    }
    if (vp->RasInfo)
        vp->RasInfo->BitMap = bm;
    ps->front = (p == ps->pbm) ? NULL : p;
    if (ps == shown && show_pbm(p) && board.waitVBlank)
        board.waitVBlank(&board);
    ReleaseSemaphore(&lock);
    if (db) {
        if (db->dbi_DispMessage.mn_ReplyPort)
            ReplyMsg(&db->dbi_DispMessage);
        if (db->dbi_SafeMessage.mn_ReplyPort)
            ReplyMsg(&db->dbi_SafeMessage);
    }
    return 1;
}

/* ---- the semaphore clients see -------------------------------------- */

static BOOL screen_info(struct Screen *s, struct PrismScreenInfo *info)
{
    struct PScreen *ps;
    BOOL ok = FALSE;

    ObtainSemaphore(&lock);
    if ((ps = ps_by_screen(s))) {
        info->vram = (ps->front ? ps->front : ps->pbm)->pix;
        info->bytesPerRow = ps->mode->m.bytesPerRow;
        info->width = ps->mode->m.width;
        info->height = ps->mode->m.height;
        info->format = ps->mode->m.format;
        info->depth = ps->depth;
        info->modeID = ps->mode->id;
        ok = TRUE;
    }
    ReleaseSemaphore(&lock);
    return ok;
}

static struct PrismSem sem;

/* ---- install / remove ----------------------------------------------- */

struct Patch {
    struct Library **base;
    WORD   lvo;
    APTR   func;
    APTR  *orig;
};

/* M2 rendering patches: assembly trampolines (stubs.S) that call the
 * h_<name> handlers in render.c / bitmap.c and fall through to o_<name>. */
#define TRAMP(n) extern void stub_##n(void); APTR o_##n;
TRAMP(BltBitMap) TRAMP(BltBitMapRastPort) TRAMP(ClipBlit) TRAMP(BltMaskBitMapRastPort)
TRAMP(BltTemplate) TRAMP(BltPattern) TRAMP(RectFill) TRAMP(Text) TRAMP(Draw)
TRAMP(PolyDraw) TRAMP(WritePixel) TRAMP(ReadPixel) TRAMP(SetRast) TRAMP(ScrollRaster)
TRAMP(ScrollRasterBF) TRAMP(EraseRect) TRAMP(AllocBitMap) TRAMP(FreeBitMap)
TRAMP(GetBitMapAttr) TRAMP(WriteChunkyPixels) TRAMP(WritePixelArray8)
TRAMP(ReadPixelArray8) TRAMP(WritePixelLine8) TRAMP(ReadPixelLine8)
TRAMP(SetRGB32CM) TRAMP(ObtainPen) TRAMP(ObtainBestPenA)
TRAMP(MoveSprite) TRAMP(ChangeSprite) TRAMP(ChangeExtSpriteA) TRAMP(ChangeVPBitMap)
TRAMP(CoerceMode) TRAMP(BitMapScale)
#define GFX_T(n, lvo) { (struct Library **)&GfxBase, -(lvo), (APTR)stub_##n, &o_##n }

LONG call_regs(APTR fn, struct Regs *r);
extern ULONG chipTop;                /* render.c */

/* Pens allocated with ObtainPen/ObtainBestPen (Workbench's colour icons)
 * and SetRGB32CM change a ColorMap; graphics.library then loads the colour
 * internally, so the card needs telling. */
/* One pen of a ColorMap changed: just that entry, as the ColorMap now has
 * it (or at the full precision we recorded for it). */
static void cm_one(struct ColorMap *cm, ULONG pen)
{
    ULONG c[3];
    struct PScreen *ps;

    if (!cm || !cm->cm_vp || pen >= cm->Count || pen > 255)
        return;
    GetRGB32(cm, pen, 1, c);
    ObtainSemaphore(&lock);
    if ((ps = ps_by_vp(cm->cm_vp)) && ps->palKnown[pen] &&
        !((ps->pal32[pen][0] ^ c[0]) & 0xf0000000UL) &&
        !((ps->pal32[pen][1] ^ c[1]) & 0xf0000000UL) &&
        !((ps->pal32[pen][2] ^ c[2]) & 0xf0000000UL)) {
        c[0] = ps->pal32[pen][0]; c[1] = ps->pal32[pen][1]; c[2] = ps->pal32[pen][2];
    }
    ReleaseSemaphore(&lock);
    palette_one(cm->cm_vp, pen, c[0], c[1], c[2]);
}

/* SetRGB32CM(cm a0, n d0, r d1, g d2, b d3) */
LONG h_SetRGB32CM(struct Regs *r)
{
    struct ColorMap *cm = (struct ColorMap *)r->a[0];
    if (cm && cm->cm_vp)
        record_vp(cm->cm_vp, r->d[0], r->d[1], r->d[2], r->d[3]);
    {
        ULONG n = r->d[0];
        call_regs(o_SetRGB32CM, r);
        cm_one(cm, n);          /* graphics' own SetRGB32 comes through here too */
    }
    return 1;
}

/* ObtainPen(cm a0, n d0, r d1, g d2, b d3, flags d4) -> pen */
LONG h_ObtainPen(struct Regs *r)
{
    struct ColorMap *cm = (struct ColorMap *)r->a[0];
    ULONG cr = r->d[1], cg = r->d[2], cb = r->d[3];
    call_regs(o_ObtainPen, r);
    if (cm && cm->cm_vp && r->d[0] != -1)
        record_vp(cm->cm_vp, r->d[0], cr, cg, cb);
    if (r->d[0] != -1)
        cm_one(cm, r->d[0]);
    return 1;
}

/* ObtainBestPenA(cm a0, r d1, g d2, b d3, tags a1) -> pen */
LONG h_ObtainBestPenA(struct Regs *r)
{
    struct ColorMap *cm = (struct ColorMap *)r->a[0];
    ULONG cr = r->d[1], cg = r->d[2], cb = r->d[3];
    call_regs(o_ObtainBestPenA, r);
    /* a pen just allocated for this colour holds it exactly; a shared one
     * that only came close keeps what it had */
    if (cm && cm->cm_vp && r->d[0] != -1) {
        struct PScreen *ps;
        ObtainSemaphore(&lock);
        if ((ps = ps_by_vp(cm->cm_vp)) && (ULONG)r->d[0] < 256 && !ps->palKnown[r->d[0]])
            ps_record(ps, r->d[0], cr, cg, cb);
        ReleaseSemaphore(&lock);
        cm_one(cm, r->d[0]);
    }
    return 1;
}


/* ---- mode test (PrismPrefs' Test button) ------------------------------
 * A picture in a bitmap of its own, shown by setting the card straight to
 * the mode: it needs no entry in the mode table, so a mode that is switched
 * off, or a refresh rate that isn't the saved one, can be tried before it
 * is saved. Runs in the caller's task. */
static struct PBitMap *testPbm;
static UWORD testPen[256];
static ULONG testRgb[256];
static UBYTE testPal[256 * 3];

static void test_hue(int h, UBYTE *r, UBYTE *g, UBYTE *b)
{
    int s = (h % 43) * 6, seg = h / 43;
    UBYTE up = s > 255 ? 255 : s, dn = 255 - up;
    switch (seg) {
    case 0:  *r = 255; *g = up;  *b = 0;   break;
    case 1:  *r = dn;  *g = 255; *b = 0;   break;
    case 2:  *r = 0;   *g = 255; *b = up;  break;
    case 3:  *r = 0;   *g = dn;  *b = 255; break;
    case 4:  *r = up;  *g = 0;   *b = 255; break;
    default: *r = 255; *g = 0;   *b = dn;  break;
    }
}

/* one pixel into a row buffer: 8-bit takes the palette index */
static void test_px(UBYTE fmt, UBYTE *d, UBYTE idx, UBYTE r, UBYTE g, UBYTE b)
{
    if (fmt == PF_CLUT8)
        *d = idx;
    else
        pf_put(fmt, ((ULONG)r << 16) | ((ULONG)g << 8) | b, d);
}

/* kind: 0 hue bar, 1 grey steps, 2/3 checkerboard, 4 black, 5 white */
static void test_row(struct PBitMap *p, UBYTE *buf, int kind)
{
    UWORD x, w = p->w;
    UBYTE bpp = p->bpp;

    for (x = 0; x < w; x++) {
        UBYTE r = 0, g = 0, b = 0, idx = 0;
        if (x == 0 || x == w - 1 || kind == 5) {
            r = g = b = 255; idx = 1;
        } else if (kind == 0) {
            int hh = (int)((ULONG)x * 255 / w);
            test_hue(hh, &r, &g, &b);
            idx = 2 + hh / 2;
        } else if (kind == 1) {
            int v = (int)((ULONG)x * 16 / w) * 17;
            r = g = b = v;
            idx = 130 + v * 125 / 255;
        } else if (kind == 2 || kind == 3) {
            if (((x >> 4) & 1) == (kind == 2)) { r = g = b = 255; idx = 1; }
        }
        test_px(p->fmt, buf + (ULONG)x * bpp, idx, r, g, b);
    }
}

static LONG test_begin(ULONG width, ULONG height, ULONG bits, ULONG hz, const char *label)
{
    struct PrismMode m;
    struct PBitMap *p;
    struct RastPort rp;
    UBYTE nth, fmt, *buf;
    UWORD y, i;
    int kind = -1;

    if (testPbm)
        return PRISM_TEST_BUSY;
    for (nth = 0; (fmt = board_format(bits, nth)) != PF_COUNT; nth++) {
        memset(&m, 0, sizeof(m));
        m.width = width;
        m.height = height;
        m.format = fmt;
        m.refresh = hz;
        if (board.checkMode(&board, &m))
            break;
    }
    if (fmt == PF_COUNT)
        return PRISM_TEST_NOMODE;

    testPen[0] = 0;
    testPen[1] = rgb16(fmt, 255, 255, 255);
    testRgb[0] = 0;
    testRgb[1] = 0xffffff;
    if (!(p = pbm_new(width, height, 8, pf_bpp(fmt), testPen, TRUE, FALSE)))
        return PRISM_TEST_NOMEM;
    p->fmt = fmt;
    p->rgbTab = testRgb;
    buf = AllocVec(p->bpr, MEMF_ANY | MEMF_CLEAR);

    ObtainSemaphore(&lock);
    /* The screen on display may have to leave VRAM to make room (a 2 MB
     * card can't hold an 800x600 16-bit Workbench and a 1024x768 16-bit
     * test picture): let it be paged out like any other bitmap. It comes
     * back when test_end() shows the front screen again. */
    shownPbm = NULL;
    if (!buf || p->bpr != m.bytesPerRow || !pbm_to_vram(p)) {
        cardMode = NULL;
        shown = NULL;
        shownVP = NULL;
        ReleaseSemaphore(&lock);
        if (buf) FreeVec(buf);
        pbm_free(p);
        update_display();                /* whatever was evicted comes back */
        return PRISM_TEST_NOMEM;
    }
    p->locks++;                          /* stays in VRAM */

    for (y = 0; y < height; y++) {
        int k;
        if (y == 0 || y == height - 1)   k = 5;
        else if (y >= height - 28)       k = 4;          /* room for the label */
        else if (y < height * 2 / 5)     k = 0;
        else if (y < height * 3 / 5)     k = 1;
        else                             k = 2 + ((y >> 4) & 1);
        if (k != kind)
            test_row(p, buf, kind = k);
        CopyMem(buf, p->pix + (ULONG)y * p->bpr, p->bpr);
    }
    FreeVec(buf);

    pointer_off();
    board.setMode(&board, &m);
    cardMode = NULL;                     /* the next real screen sets its own */
    shown = NULL;
    shownVP = NULL;
    board.setDisplayStart(&board, p->vramOff);
    shownPbm = p;
    if (fmt == PF_CLUT8) {
        testPal[3] = testPal[4] = testPal[5] = 255;
        for (i = 0; i < 128; i++)
            test_hue(i * 2, &testPal[(2 + i) * 3], &testPal[(2 + i) * 3 + 1],
                     &testPal[(2 + i) * 3 + 2]);
        for (i = 0; i < 126; i++)
            testPal[(130 + i) * 3] = testPal[(130 + i) * 3 + 1] = testPal[(130 + i) * 3 + 2] =
                i * 255 / 125;
        board.setPalette(&board, 0, 256, testPal);
    }
    if (!rtgOn) {
        board.setSwitch(&board, TRUE);
        rtgOn = TRUE;
    }
    testPbm = p;
    ReleaseSemaphore(&lock);

    if (label) {
        /* the bitmap is a Prism bitmap like any other: Text() draws in it */
        InitRastPort(&rp);
        rp.BitMap = p->bm;
        SetAPen(&rp, 1);
        SetBPen(&rp, 0);
        SetDrMd(&rp, JAM2);
        Move(&rp, 12, height - 10);
        Text(&rp, (STRPTR)label, strlen(label));
    }
    return PRISM_TEST_OK;
}

static void test_end(void)
{
    struct PBitMap *p;

    ObtainSemaphore(&lock);
    if ((p = testPbm)) {
        testPbm = NULL;
        p->locks--;
        if (shownPbm == p)
            shownPbm = NULL;
        cardMode = NULL;
        shown = NULL;
        shownVP = NULL;
    }
    ReleaseSemaphore(&lock);
    if (p) {
        pbm_free(p);
        update_display();                /* back to the front screen */
    }
}

/* CoerceMode(vp a0, monitorID d0, flags d1) -> ModeID
 * Intuition asks, for every screen, which mode shows it on the monitor of
 * the frontmost screen. For a Prism screen on Prism's own monitor that is
 * the screen's own mode. graphics.library only says so for modes that fit
 * its idea of the native display: 640x480 passed, but for 800x600 it went
 * looking for a substitute (BestModeIDA), found none and returned
 * INVALID_ID - and Intuition then hides the screen's ViewPort (DHeight 0,
 * VP_HIDE), which empties its mouse limits and throws the pointer to
 * +-32767. Found on the A2000 (no pointer on an 800x600 Workbench);
 * tools/prismdiag shows the symptoms. */
LONG h_CoerceMode(struct Regs *r)
{
    struct ViewPort *vp = (struct ViewPort *)r->a[0];
    ULONG id;

    if (!vp || !vp->ColorMap || !IS_PRISM_ID((ULONG)r->d[0]))
        return 0;
    id = GetVPModeID(vp);
    if (!IS_PRISM_ID(id) || !mode_by_id(id))
        return 0;
    r->d[0] = id;
    return 1;
}

/* ---- alerts ---------------------------------------------------------
 *
 * A guru (or any DisplayAlert) is drawn on the native display and waits
 * for a mouse button with the whole system stopped. With the card's video
 * switch on Prism nobody sees it: the machine just looks frozen, on the
 * last Prism picture. So the switch goes to the Amiga's own video for the
 * alert and back afterwards. Nothing here may wait or take a lock: alerts
 * come from anywhere.
 *
 * With LOG=ON in the prefs the alert is not shown at all - its number,
 * the task and its text go to the log and the call returns at once - so a
 * machine with nobody at the mouse (the emulator under test) carries on
 * and the log says what crashed. */
static ULONG alert(APTR fn, ULONG num, UBYTE *str, ULONG height, ULONG time)
{
    register ULONG d0 __asm("d0") = num;
    register ULONG d1 __asm("d1") = height;
    register APTR a0 __asm("a0") = str;
    register APTR a1 __asm("a1") = (APTR)time;
    BOOL was = rtgOn;
    BASE(IntuitionBase);

    if (prefs.log) {
        struct Task *t = FindTask(NULL);
        UBYTE *q = str;
        dbg("ALERT %08lx in task '%s' (%lx)\n", num,
            t->tc_Node.ln_Name ? t->tc_Node.ln_Name : "?", (ULONG)t);
        if (q)
            for (;;) {
                q += 3;                           /* x (word), y (byte)   */
                dbg("  %s\n", (char *)q);
                while (*q++) ;
                if (!*q++)
                    break;
            }
        return 0;
    }
    if (was)
        board.setSwitch(&board, FALSE);
    __asm volatile ("jsr (%[f])" : "+r"(d0), "+r"(d1), "+r"(a0), "+r"(a1)
                    : "r"(_a6), [f]"a"(fn) : "cc", "memory");
    if (was)
        board.setSwitch(&board, TRUE);
    return d0;
}

/* exec's own Alert() - a guru. Exec draws it itself, straight on the
 * chipset, without Intuition, so it needs the same treatment: the card's
 * switch to the Amiga's video first (it stays there: after a dead-end
 * alert the machine resets, after a recoverable one the next screen that
 * comes to the front switches back). The number and the task go to the
 * log first; with LOG=SYNC they are in the file before the machine stops. */
static struct Task *volatile alertWaiter;     /* a task waiting for the log flush */
static ULONG P_DisplayAlert(ULONG num __asm("d0"), UBYTE *str __asm("a0"), ULONG height __asm("d1"));

static void P_Alert(ULONG num __asm("d7"))
{
    register ULONG d7 __asm("d7") = num;
    register APTR a6 __asm("a6") = SysBase;
    struct Task *t = SysBase->ThisTask;

    dbg("GURU %08lx, task '%s' (%lx)\n", num,
        t && t->tc_Node.ln_Name ? t->tc_Node.ln_Name : "?", (ULONG)t);
    if (prefs.log == 2 && t) {
        /* LOG=SYNC: the stack of whoever raised it (return addresses to
         * look up), and - when this is the task's own stack, not an
         * interrupt's - wait until PrismD has the lines in its log file:
         * after a dead-end alert nothing else gets to run. */
        ULONG *sp = (ULONG *)&sp;
        int i;
        dbg("  PrismD code near %lx; stack at %lx (task's %lx-%lx):\n", (ULONG)P_DisplayAlert,
            (ULONG)sp, (ULONG)t->tc_SPLower, (ULONG)t->tc_SPUpper);
        for (i = 0; i < 48; i += 6)
            dbg("  %08lx %08lx %08lx %08lx %08lx %08lx\n", sp[i], sp[i + 1], sp[i + 2],
                sp[i + 3], sp[i + 4], sp[i + 5]);
        if ((APTR)sp >= t->tc_SPLower && (APTR)sp < t->tc_SPUpper && dbgMain && t != dbgMain) {
            alertWaiter = t;
            SetSignal(0, SIGF_SINGLE);
            Signal(dbgMain, SIGBREAKF_CTRL_F);
            Wait(SIGF_SINGLE);
        }
    }
    if (rtgOn) {
        board.setSwitch(&board, FALSE);
        rtgOn = FALSE;
        cardMode = NULL;
    }
    __asm volatile ("jsr (%[f])" : "+r"(d7) : "r"(a6), [f]"a"(o_Alert)
                    : "d0", "d1", "a0", "a1", "cc", "memory");
}

static ULONG P_DisplayAlert(ULONG num __asm("d0"), UBYTE *str __asm("a0"), ULONG height __asm("d1"))
{
    return alert(o_DisplayAlert, num, str, height, 0);
}

static ULONG P_TimedDisplayAlert(ULONG num __asm("d0"), UBYTE *str __asm("a0"),
                                 ULONG height __asm("d1"), ULONG time __asm("a1"))
{
    return alert(o_TimedDisplayAlert, num, str, height, time);
}

static struct Patch patches[] = {
    GFX_T(BltBitMap,             0x01e),
    GFX_T(BltBitMapRastPort,     0x25e),
    GFX_T(ClipBlit,              0x228),
    GFX_T(BltMaskBitMapRastPort, 0x27c),
    GFX_T(BltTemplate,           0x024),
    GFX_T(BltPattern,            0x138),
    GFX_T(RectFill,              0x132),
    GFX_T(Text,                  0x03c),
    GFX_T(Draw,                  0x0f6),
    GFX_T(PolyDraw,              0x150),
    GFX_T(WritePixel,            0x144),
    GFX_T(ReadPixel,             0x13e),
    GFX_T(SetRast,               0x0ea),
    GFX_T(ScrollRaster,          0x18c),
    GFX_T(ScrollRasterBF,        0x3ea),
    GFX_T(EraseRect,             0x32a),
    GFX_T(AllocBitMap,           0x396),
    GFX_T(FreeBitMap,            0x39c),
    GFX_T(GetBitMapAttr,         0x3c0),
    GFX_T(BitMapScale,           0x2a6),
    GFX_T(WriteChunkyPixels,     0x420),
    GFX_T(WritePixelArray8,      0x312),
    GFX_T(ReadPixelArray8,       0x30c),
    GFX_T(WritePixelLine8,       0x306),
    GFX_T(ReadPixelLine8,        0x300),
    GFX_T(SetRGB32CM,            0x3e4),
    GFX_T(ObtainPen,             0x3ba),
    GFX_T(ObtainBestPenA,        0x348),
    GFX_T(MoveSprite,            0x1aa),
    GFX_T(ChangeSprite,          0x1a4),
    GFX_T(ChangeExtSpriteA,      0x402),
    GFX_T(ChangeVPBitMap,        0x3ae),
    GFX_T(CoerceMode,            0x3a8),
    { (struct Library **)&GfxBase, -0x2dc, (APTR)P_NextDisplayInfo,    &o_NextDisplayInfo },
    { (struct Library **)&GfxBase, -0x2d6, (APTR)P_FindDisplayInfo,    &o_FindDisplayInfo },
    { (struct Library **)&GfxBase, -0x2f4, (APTR)P_GetDisplayInfoData, &o_GetDisplayInfoData },
    { (struct Library **)&GfxBase, -0x31e, (APTR)P_ModeNotAvailable,   &o_ModeNotAvailable },
    { (struct Library **)&GfxBase, -0x2ca, (APTR)P_OpenMonitor,        &o_OpenMonitor },
    { (struct Library **)&GfxBase, -0x2d0, (APTR)P_CloseMonitor,       &o_CloseMonitor },
    { (struct Library **)&GfxBase, -0x0d8, (APTR)P_MakeVPort,          &o_MakeVPort },
    { (struct Library **)&GfxBase, -0x0d2, (APTR)P_MrgCop,             &o_MrgCop },
    { (struct Library **)&GfxBase, -0x0de, (APTR)P_LoadView,           &o_LoadView },
    { (struct Library **)&GfxBase, -0x354, (APTR)P_SetRGB32,           &o_SetRGB32 },
    { (struct Library **)&GfxBase, -0x372, (APTR)P_LoadRGB32,          &o_LoadRGB32 },
    { (struct Library **)&GfxBase, -0x120, (APTR)P_SetRGB4,            &o_SetRGB4 },
    { (struct Library **)&GfxBase, -0x0c0, (APTR)P_LoadRGB4,           &o_LoadRGB4 },
    { (struct Library **)&IntuitionBase, -0x264, (APTR)P_OpenScreenTagList, &o_OpenScreenTagList },
    { (struct Library **)&IntuitionBase, -0x0c6, (APTR)P_OpenScreen,   &o_OpenScreen },
    { (struct Library **)&IntuitionBase, -0x042, (APTR)P_CloseScreen,  &o_CloseScreen },
    { (struct Library **)&IntuitionBase, -0x05a, (APTR)P_DisplayAlert, &o_DisplayAlert },
    { (struct Library **)&SysBase, -0x06c, (APTR)P_Alert, &o_Alert },
    { (struct Library **)&IntuitionBase, -0x336, (APTR)P_TimedDisplayAlert, &o_TimedDisplayAlert },
};
#define NPATCHES (sizeof(patches) / sizeof(patches[0]))

#ifdef PRISM_TRACE
/* Trace build: every graphics.library call, patched by Prism or not, goes
 * to the debug log as "g-<offset>" before it runs. With "prismlog SYNC"
 * the log then ends at the last call a program made before it took the
 * machine down. Each vector gets a little piece of code:
 *      movem.l d0-d7/a0-a6,-(sp) ; move.l #offset,-(sp) ; jsr trace_lvo
 *      addq.l #4,sp ; movem.l (sp)+,d0-d7/a0-a6 ; jmp original
 * A trace build can't be quit (the thunks stay). */
static BOOL traceAll;       /* every vector, not just Prism's handlers (set by hand) */

/* Walk exec's free memory lists and say so, once, when they stop making
 * sense: the call named is the first one entered with memory already
 * damaged, so the damage was done since the line before it. (A program
 * that writes past a bitmap hangs the machine much later, in whoever
 * allocates next.) */
void memchk(const char *where)
{
    static BOOL bad;
    struct MemHeader *mh;
    const char *why = NULL;
    ULONG at = 0;

    if (bad || !prefs.log)
        return;
    Forbid();
    for (mh = (struct MemHeader *)SysBase->MemList.lh_Head; mh->mh_Node.ln_Succ && !why;
         mh = (struct MemHeader *)mh->mh_Node.ln_Succ) {
        struct MemChunk *mc = mh->mh_First;
        ULONG sum = 0, n = 0;
        for (; mc; mc = mc->mc_Next) {
            if ((ULONG)mc < (ULONG)mh->mh_Lower || (ULONG)mc >= (ULONG)mh->mh_Upper ||
                ((ULONG)mc & 3)) { why = "chunk outside its region"; at = (ULONG)mc; break; }
            if (mc->mc_Next && (ULONG)mc->mc_Next <= (ULONG)mc + mc->mc_Bytes) {
                why = "chunks out of order"; at = (ULONG)mc; break; }
            sum += mc->mc_Bytes;
            if (++n > 100000) { why = "endless list"; at = (ULONG)mc; break; }
        }
        if (!why && sum != mh->mh_Free) { why = "free total wrong"; at = (ULONG)mh; }
    }
    Permit();
    if (why) {
        bad = TRUE;
        dbg("MEMORY LIST DAMAGED (%s at %lx), seen entering %s\n", why, at, where);
    }
}

static void trace_lvo(LONG lvo)
{
    /* The caller's registers are the thunk's movem, above our one
     * argument on the stack (d0-d7, a0-a6). They are read through a
     * pointer, not declared as arguments: the compiler may use its own
     * argument slots as scratch, and these are restored into the
     * registers afterwards. Only calls made by a task on its own stack
     * with room to spare: the formatting needs a few hundred bytes. */
    const ULONG *r = (const ULONG *)(&lvo + 1);
    struct Task *me = SysBase->ThisTask;
    if ((ULONG)&me < (ULONG)me->tc_SPLower + 1200 || (ULONG)&me >= (ULONG)me->tc_SPUpper)
        return;
    if (prefs.log && traceAll)
        dbg("g-%ld a0=%lx a1=%lx a2=%lx d0=%lx d1=%lx\n", lvo, r[8], r[9], r[10], r[0], r[1]);
}

static void trace_install(void)
{
    LONG lvo, n = ((struct Library *)GfxBase)->lib_NegSize / 6;
    UWORD *code = AllocMem(n * 28, MEMF_PUBLIC);

    if (!code)
        return;
    for (lvo = 30; lvo <= n * 6; lvo += 6, code += 14) {
        APTR orig;
        code[0] = 0x48e7; code[1] = 0xfffe;
        code[2] = 0x2f3c; code[3] = lvo >> 16; code[4] = lvo;
        code[5] = 0x4eb9; code[6] = (ULONG)trace_lvo >> 16; code[7] = (ULONG)trace_lvo;
        code[8] = 0x588f;
        code[9] = 0x4cdf; code[10] = 0x7fff;
        code[11] = 0x4ef9;
        orig = SetFunction((struct Library *)GfxBase, -lvo, (APTR)code);
        code[12] = (ULONG)orig >> 16; code[13] = (ULONG)orig;
    }
    CacheClearU();
}
#endif

/* Is Picasso96's core in memory? (rtg.library is its own; Prism has
 * nothing of that name.) */
static BOOL other_rtg(void)
{
    BOOL r;
    Forbid();
    r = FindName(&SysBase->LibList, (STRPTR)"rtg.library") != NULL;
    Permit();
    return r;
}

/* Is a Picasso96 monitor installed? Its monitor files in DEVS:Monitors
 * carry a BOARDTYPE tooltype (the driver's name); ours does not. */
static BOOL p96_monitor_installed(void)
{
    struct Library *IconBase = OpenLibrary((STRPTR)"icon.library", 37);
    struct FileInfoBlock *fib;
    BPTR lock;
    BOOL found = FALSE;

    if (!IconBase)
        return FALSE;
    fib = AllocDosObject(DOS_FIB, NULL);
    lock = Lock((STRPTR)"DEVS:Monitors", ACCESS_READ);
    if (fib && lock && Examine(lock, fib)) {
        while (!found && ExNext(lock, fib)) {
            int n = strlen((char *)fib->fib_FileName);
            char path[160];
            struct DiskObject *dob;

            if (fib->fib_DirEntryType > 0 || n < 6 || n > 100 ||
                stricmp((char *)fib->fib_FileName + n - 5, ".info"))
                continue;
            sprintf(path, "DEVS:Monitors/%s", fib->fib_FileName);
            path[strlen(path) - 5] = 0;
            if ((dob = GetDiskObject((STRPTR)path))) {
                if (dob->do_ToolTypes && FindToolType((void *)dob->do_ToolTypes, (STRPTR)"BOARDTYPE"))
                    found = TRUE;
                FreeDiskObject(dob);
            }
        }
    }
    if (lock) UnLock(lock);
    if (fib) FreeDosObject(DOS_FIB, fib);
    CloseLibrary(IconBase);
    return found;
}

static void install(void)
{
    ULONG i;
    Forbid();
#ifdef PRISM_TRACE
    trace_install();
#endif
    for (i = 0; i < NPATCHES; i++)
        *patches[i].orig = SetFunction(*patches[i].base, patches[i].lvo, patches[i].func);
    Permit();
}

/* The jump table entry is "jmp abs.l": the target sits 2 bytes in. */
static APTR vector(struct Library *lib, WORD lvo)
{
    return *(APTR *)((UBYTE *)lib + lvo + 2);
}

static BOOL uninstall(void)
{
    ULONG i;
    Forbid();
    for (i = 0; i < NPATCHES; i++)
        if (vector(*patches[i].base, patches[i].lvo) != patches[i].func) {
            Permit();
            return FALSE;            /* someone patched on top of us */
        }
    for (i = 0; i < NPATCHES; i++)
        SetFunction(*patches[i].base, patches[i].lvo, *patches[i].orig);
    Permit();
    while (inPatch)
        Delay(5);
    Delay(25);
    return TRUE;
}

/* ---- main ----------------------------------------------------------- */

#define TEMPLATE "BOARD/K,PREFS/K,LOG/S"

int main(void)
{
    LONG args[3] = { 0 };
    static const char *const pbName[PB_COUNT] = { NULL, "PICASSO2", "ZZ9000" };
    struct RDArgs *rda;
    const char *want;
    BOOL found = FALSE;
    ULONG i;
    int rc = 20;

    if (!(rda = ReadArgs(TEMPLATE, args, NULL))) {
        PrintFault(IoErr(), "PrismD");
        return 20;
    }
    GfxBase = (struct GfxBase *)OpenLibrary("graphics.library", 39);
    IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 39);
    UtilityBase = OpenLibrary("utility.library", 39);
    ExpansionBase = (struct ExpansionBase *)OpenLibrary("expansion.library", 37);
    if (!GfxBase || !IntuitionBase || !UtilityBase || !ExpansionBase) {
        printf("PrismD: needs OS 3.0 or newer\n");
        goto out;
    }
    if (FindSemaphore(PRISM_SEMNAME)) {
        printf("PrismD: already running\n");
        rc = 5;
        goto out;
    }
    /* Never next to Picasso96: the two patch the same calls, and an A4000
     * with Picasso96's "Native" monitor still installed froze at the first
     * Workbench icon. Either its core is in memory already, or one of its
     * monitors is waiting in DEVS:Monitors to be loaded after ours. The
     * installer puts those aside (PrismSetup P96PARK); until then Prism
     * stays out and the Amiga starts as it did before. */
    {
        const char *why = other_rtg() ? "is running" : p96_monitor_installed() ? "is installed" : NULL;
        if (why) {
            printf("PrismD: Picasso96 %s - PrismRTG does not start next to it.\n"
                   "        Run Install_PrismRTG again to put Picasso96 aside.\n", why);
            rc = 5;
            goto out;
        }
    }

    /* settings from PrismPrefs; BOARD= and LOG on the command line win */
    prefs_load(&prefs, args[1] ? (const char *)args[1] : PREFS_ENV);
    /* LOG=SYNC is for a machine that dies during boot, where PrismD is
     * started with no output: the log goes to SYS:PrismD.log (dbg_flush) */
    if (prefs.log == 2)
    {
        dbg("PrismD %s starting\n", __DATE__ " " __TIME__);
        dbg_flush();
    }
    if (args[2])
        prefs.log = 1;
    want = args[0] ? (const char *)args[0] : pbName[prefs.board < PB_COUNT ? prefs.board : 0];
    if (!want || !Stricmp(want, "PICASSO2"))
        found = Picasso2_Probe(&board);
    if (!found && (!want || !Stricmp(want, "ZZ9000")))
        found = ZZ9000_Probe(&board);
    if (!found) {
        printf("PrismD: no supported board\n");
        rc = 5;
        goto out;
    }
    Picasso2_ClutBGR = prefs.clutBGR;
    if (!prefs.blitter) {
        board.fillRect = NULL;              /* render.c falls back to the CPU */
        board.copyRect = NULL;
        board.copyBetween = NULL;
        board.expandRect = NULL;
        board.drawLine = NULL;
        board.flags &= ~(PBF_BLIT_FILL | PBF_BLIT_COPY);
    }
    build_modes();
    if (!nmodes) {
        printf("PrismD: %s offers no usable modes\n", board.name);
        goto out;
    }

    InitSemaphore(&lock);

    /* Program a mode now, with the monitor still showing the Amiga: a
     * cold Cirrus chip drops every CPU write to VRAM until its linear
     * window is set up, and screens are drawn before they are shown. */
    {
        struct PrismMode m = modes[0].m;
        board.setMode(&board, &m);
        board.setSwitch(&board, FALSE);
        cardMode = &modes[0];
    }
    CopyMem(GfxBase->default_monitor, &prismMonitor, sizeof(prismMonitor));
    prismMonitor.ms_Node.xln_Name = monName;
    prismMonitor.ms_OpenCount = 0;

    STAGE("board found, modes built");
    printf("PrismD: %s, %lu KB VRAM, modes:\n", board.name,
           (unsigned long)(board.vramSize >> 10));
    for (i = 0; i < nmodes; i++)
        printf("  $%08lx %s %u Hz\n", (unsigned long)modes[i].id, modes[i].name,
               modes[i].m.refresh);
    if (!prefs.blitter)
        printf("PrismD: blitter off (PrismPrefs)\n");
    else if (board.configDev && board.configDev->cd_Rom.er_Manufacturer != 0x6d6e) {   /* a Cirrus board */
        BOOL tr;
        UBYTE pad = Picasso2_TextExpand(&board, &tr);
        if (pad == 0xff)
            printf("PrismD: text on the CPU (blitter text expansion failed its self-test)\n");
        else
            printf("PrismD: text on the blitter (rows padded to %u byte%s, %s)\n", pad,
                   pad == 1 ? "" : "s", tr ? "JAM1 + JAM2" : "JAM2 only");
        if (board.formats & PF_BIT(PF_BGRA32))
            puts((board.flags & PBF_BLIT_32) ? "PrismD: 32-bit fills and text on the blitter"
                                             : "PrismD: no 32-bit blitter (self-test failed)");
    }

    /* Planar bitmaps above chip RAM are Prism's to draw into (render.c) -
     * unless Picasso96 is running too. It can be, on another display: its
     * "Native" monitor drives the Amiga's own chipset (the A4000 has it).
     * Then such bitmaps are Picasso96's - its own, and the ones it already
     * draws into with the CPU - and Prism leaves every one of them alone,
     * as it did before it knew about them. Touching them froze that
     * machine as soon as Workbench drew an icon. */
    chipTop = other_rtg() ? 0xffffffffUL : (ULONG)SysBase->MaxLocMem;
    if (chipTop == 0xffffffffUL)
        printf("PrismD: Picasso96 is running as well: its bitmaps and its API library are left to it\n");
    STAGE("installing patches");
    install();
    STAGE("patches in");
    if (cgx_init())
        printf("PrismD: cybergraphics.library 41 added\n");
    if (chipTop != 0xffffffffUL && p96_init())
        printf("PrismD: Picasso96API.library 2 added (Prism's own)\n");

    memset(&sem, 0, sizeof(sem));
    sem.ss.ss_Link.ln_Name = PRISM_SEMNAME;
    sem.ss.ss_Link.ln_Pri = 0;
    sem.version = PRISM_VERSION;
    sem.boardName = board.name;
    sem.task = FindTask(NULL);
    sem.numModes = nmodes;
    sem.screenInfo = screen_info;
    sem.testBegin = test_begin;
    sem.testEnd = test_end;
    sem.drawLock = &lock;
    sem.logLevel = &prefs.log;
    AddSemaphore(&sem.ss);
    printf("PrismD: running (Ctrl-C to quit)\n");
    STAGE("running");

    for (;;) {
        ULONG tick = 0;
        static ULONG vb0;
        static BOOL settled;
        if (!vb0)
            vb0 = GfxBase->VBCounter | 1;
        dbgMain = FindTask(NULL);
        while (!(SetSignal(0, 0) & SIGBREAKF_CTRL_C)) {
            if (prefs.log == 2) {
                /* LOG=SYNC (or "prismlog SYNC"): sleep until dbg() has a
                 * line, write it at once, ahead of whoever logged it */
                SetTaskPri(dbgMain, 10);
                if (Wait(SIGBREAKF_CTRL_C | SIGBREAKF_CTRL_F) & SIGBREAKF_CTRL_C)
                    SetSignal(SIGBREAKF_CTRL_C, SIGBREAKF_CTRL_C);
                dbg_flush();
                if (alertWaiter) {
                    struct Task *w = alertWaiter;
                    alertWaiter = NULL;
                    Signal(w, SIGF_SINGLE);
                }
                if (prefs.log != 2)
                    SetTaskPri(dbgMain, 0);
                continue;
            }
            Delay(1);                         /* ~one frame              */
            if (chipTop != 0xffffffffUL && (tick & 63) == 0 && other_rtg()) {
                chipTop = 0xffffffffUL;       /* Picasso96 came up after us */
                dbg("Picasso96 has started: its bitmaps are left to it from now on\n");
            }
            pointer_tick();
            if (prefs.log || ++tick % 25 == 0)
                dbg_flush();
            /* up for a minute (3600 vertical blanks): this start is a good
             * one (see prismmon.c) */
            if (!settled && GfxBase->VBCounter - vb0 > 3600) {
                settled = TRUE;
                DeleteFile(PRISM_BOOTFLAG);
            }
        }
        SetSignal(0, SIGBREAKF_CTRL_C);
        if (pscreens) {
            printf("PrismD: Prism screens are still open - close them first\n");
            continue;
        }
        if (!p96_remove()) {
            printf("PrismD: programs still have Picasso96API.library open\n");
            continue;
        }
        if (!cgx_remove()) {
            printf("PrismD: programs still have cybergraphics.library open\n");
            continue;
        }
        Forbid();
        RemSemaphore(&sem.ss);
        Permit();
        if (uninstall())
            break;
        printf("PrismD: another program patched the same functions - can't quit\n");
        AddSemaphore(&sem.ss);
    }
    if (rtgOn)
        board.setSwitch(&board, FALSE);
    printf("PrismD: stopped\n");
    rc = 0;

out:
    if (ExpansionBase) CloseLibrary((struct Library *)ExpansionBase);
    if (UtilityBase) CloseLibrary(UtilityBase);
    if (IntuitionBase) CloseLibrary((struct Library *)IntuitionBase);
    if (GfxBase) CloseLibrary((struct Library *)GfxBase);
    FreeArgs(rda);
    return rc;
}

/* ---- the mode table, for cgx.c --------------------------------------- */

ULONG prism_mode_count(void)
{
    return nmodes;
}

BOOL prism_mode(ULONG i, struct PrismModeInfo *mi)
{
    if (i >= nmodes)
        return FALSE;
    mi->id = modes[i].id;
    mi->w = modes[i].m.width;
    mi->h = modes[i].m.height;
    mi->fmt = modes[i].m.format;
    mi->bpp = modes[i].bpp;
    mi->name = modes[i].name;
    return TRUE;
}

BOOL prism_mode_by_id(ULONG id, struct PrismModeInfo *mi)
{
    struct ModeRec *r = mode_by_id(id);
    return r ? prism_mode(r - modes, mi) : FALSE;
}
