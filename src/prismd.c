/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laine Jones
 * Copyright (C) 2026 Stefan Reinauer */
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
#include <graphics/modeid.h>
#include <graphics/view.h>
#include <intuition/intuitionbase.h>
#include <intuition/screens.h>
#include <exec/interrupts.h>
#include <devices/input.h>
#include <devices/inputevent.h>
#include <devices/timer.h>
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
#include <math.h>
#include "prismboard.h"
#include "prism.h"
#include "prismint.h"
#include "driver_module.h"
#include "prefs.h"

struct GfxBase       *GfxBase;
struct IntuitionBase *IntuitionBase;
/* Use libnix's UtilityBase: its startup code needs utility.library before
 * main. Our explicit OpenLibrary below holds an additional reference. */
struct ExpansionBase *ExpansionBase;

struct PrismBoard board;                 /* card 0's */
static struct PrismBoard board2;         /* card 1's, when there is a second board */
struct PrismCard cards[MAX_CARDS] = { { &board, 0, 0 }, { &board2, 0, 1 } };
static UBYTE ncards = 1;
static BOOL boardsSwapped;               /* PRIMARY=SECOND took effect */

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

BOOL prism_dragging(void) { return prefs.dragging; }
BOOL prism_hw_overlay(void) { return prefs.hwOverlay; }
/* does any board go through a shadow copy (PBF_SHADOW)? */
BOOL prism_any_shadow(void)
{
    ULONG i;
    for (i = 0; i < ncards; i++)
        if (cards[i].b && (cards[i].b->flags & PBF_SHADOW))
            return TRUE;
    return FALSE;
}
BOOL prism_software_pointer(void) { return prefs.softwarePointer; }
#define DBG_RING 8192
static char *dbgRing;                   /* allocated when LOG is on */
static volatile UWORD dbgHead, dbgTail;
static struct Task *dbgMain;

void dbg(const char *fmt, ...)
{
    char tmp[96];
    va_list ap;
    int i;

    if (!prefs.log || !dbgRing)
        return;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    /* Disable, not Forbid: a trace build logs calls made from interrupts
     * too, and Permit() there may try to switch tasks */
    Disable();
    for (i = 0; tmp[i]; i++) {
        dbgRing[dbgHead] = tmp[i];
        dbgHead = (dbgHead + 1) % DBG_RING;
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
        dbgTail = (dbgTail + 1) % DBG_RING;
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

/* LOG=SYNC's sleep: the main loop waits until dbg() has a line (Ctrl-F) or
 * the next frame is due, whichever comes first, so a line reaches the file
 * at once and the once-a-frame work (ARexx, the mirror, the pointer, power
 * saving, the compositor) goes on. Up to 1.1 beta 8 the loop slept for log
 * lines only, and all of that stood still while the log was on. */
static struct MsgPort *syncPort;
static struct timerequest *syncTr;
static BOOL syncBusy;                     /* a frame request is out */

static BOOL sync_timer(void)
{
    if (syncTr)
        return TRUE;
    if (!(syncPort = CreateMsgPort()))
        return FALSE;
    if (!(syncTr = (struct timerequest *)CreateIORequest(syncPort, sizeof(*syncTr))) ||
        OpenDevice((STRPTR)TIMERNAME, UNIT_VBLANK, (struct IORequest *)syncTr, 0)) {
        if (syncTr)
            DeleteIORequest((struct IORequest *)syncTr);
        DeleteMsgPort(syncPort);
        syncTr = NULL;
        syncPort = NULL;
        return FALSE;
    }
    return TRUE;
}

static void sync_timer_close(void)
{
    if (!syncTr)
        return;
    if (syncBusy) {
        AbortIO((struct IORequest *)syncTr);
        WaitIO((struct IORequest *)syncTr);
        syncBusy = FALSE;
    }
    CloseDevice((struct IORequest *)syncTr);
    DeleteIORequest((struct IORequest *)syncTr);
    DeleteMsgPort(syncPort);
    syncTr = NULL;
    syncPort = NULL;
}

/* The sleep between ticks without the log. Delay(1) slept 40 ms - two DOS
 * ticks, so everything once a frame ran at 25 Hz - and a drag stepped at
 * that rate. timer.device's microsecond unit keeps to what is asked: a
 * card's 60 Hz while the compositor or a split shows a drag, 50 Hz else. */
static struct MsgPort *tickPort;
static struct timerequest *tickTr;

static void tick_sleep(void)
{
    if (!tickTr && (!(tickPort = CreateMsgPort()) ||
                    !(tickTr = (struct timerequest *)CreateIORequest(tickPort, sizeof(*tickTr))) ||
                    OpenDevice((STRPTR)TIMERNAME, UNIT_MICROHZ, (struct IORequest *)tickTr, 0))) {
        if (tickTr) DeleteIORequest((struct IORequest *)tickTr);
        if (tickPort) DeleteMsgPort(tickPort);
        tickTr = NULL; tickPort = NULL;
        Delay(1);                         /* no timer: the old way */
        return;
    }
    tickTr->tr_node.io_Command = TR_ADDREQUEST;
    tickTr->tr_time.tv_secs = 0;
    tickTr->tr_time.tv_micro = present_active() ? 16667 : 20000;
    DoIO((struct IORequest *)tickTr);
}

static void tick_close(void)
{
    if (!tickTr) return;
    CloseDevice((struct IORequest *)tickTr);
    DeleteIORequest((struct IORequest *)tickTr);
    DeleteMsgPort(tickPort);
    tickTr = NULL; tickPort = NULL;
}

/* the signals that came; *frame: a frame is up (the per-frame work is due) */
static ULONG sync_sleep(BOOL *frame)
{
    ULONG sig;
    if (!sync_timer()) {                  /* no timer: poll once a frame */
        Delay(1);
        *frame = TRUE;
        return SetSignal(0, SIGBREAKF_CTRL_F);
    }
    if (!syncBusy) {
        syncTr->tr_node.io_Command = TR_ADDREQUEST;
        syncTr->tr_time.tv_secs = 0;
        syncTr->tr_time.tv_micro = 20000;
        SendIO((struct IORequest *)syncTr);
        syncBusy = TRUE;
    }
    sig = Wait(SIGBREAKF_CTRL_C | SIGBREAKF_CTRL_F | (1UL << syncPort->mp_SigBit));
    *frame = FALSE;
    if (CheckIO((struct IORequest *)syncTr)) {
        WaitIO((struct IORequest *)syncTr);
        syncBusy = FALSE;
        *frame = TRUE;
    }
    return sig;
}

static void dbg_flush(void)
{
    if (prefs.log == 2) {
        while (dbgTail != dbgHead)
            dbg_flush_file();
        return;
    }
    while (dbgTail != dbgHead) {
        putchar(dbgRing[dbgTail]);
        dbgTail = (dbgTail + 1) % DBG_RING;
    }
    fflush(stdout);
}

/* ---- modes ---------------------------------------------------------- */

struct ModeRec {
    /* A DisplayInfoHandle from FindDisplayInfo is ours (a ModeRec), and
     * graphics.library reads it as one of its own records: links first,
     * then the mode ID (major and minor key) at offset 16. With other
     * fields there it read $01505249 ("\1PRI" - bpp and the name), looked
     * that up through the whole display database, several times for
     * every MoveScreen: dragging a screen cost about 120 ms a step. */
    APTR  recLinks[4];        /* no links: nothing is chained to it      */
    ULONG id;
    /* the rest of graphics' record header (tag, control, its get/set data
     * functions): zero, in case graphics looks at those of a handle */
    UBYTE recRest[32];
    struct PrismCard *card;   /* the board the mode is on                */
    struct PrismMode m;
    UBYTE maxDepth;           /* depth Intuition opens its screens with      */
    UBYTE dimDepth;           /* depth the display database reports: the real
                                 bits per pixel, as programs expect of an RTG
                                 mode (reqtools' mode requester filters on it) */
    UBYTE bpp;                   /* bytes per pixel: 1, 2, 3 or 4 */
    UWORD spanHalf;              /* SECONDMONITOR=SPAN: the width each board shows
                                    of this mode's twice as wide screen (0: none) */
    char  name[DISPLAYNAMELEN];
    /* A DisplayInfoHandle normally points at a graphics.library record;
     * keep some zeroes around ours in case anything peeks inside. */
    UBYTE pad[128];
};
/* where graphics.library reads a record's mode ID (see above) */
typedef char modeRecIdAt16[offsetof(struct ModeRec, id) == 16 ? 1 : -1];

static const char version[] __attribute__((used)) = "$VER: PrismD " PRISM_RELEASE " (" PRISM_RELDATE ")";

/* every size at every depth, and on board 0 a span mode per size at 8 and 16 bit */
#define MAX_MODES (PREFS_NMODES + 2 * PREFS_NSIZES)
#define SPAN_ID   0x80                /* in a mode ID's low byte: a span mode */

struct Blk { ULONG off, size; };
#define MAX_BLKS 256
struct PScreen;

/* PrismD's own state for one board (struct PrismCard holds what the other
 * modules need). Only card 0 for now, under the old names below. */
struct CardState {
    struct ModeRec modes[MAX_MODES];
    ULONG nmodes;
    struct MonitorSpec monitor;         /* the board's display-database monitor */
    struct Blk blk[MAX_BLKS];           /* VRAM blocks in use, sorted        */
    int nblk;
    struct PScreen *shown;              /* screen the card is showing        */
    struct ViewPort *shownVP;
    struct ModeRec *curMode;            /* mode the card is set to           */
    UWORD curPitch;                     /* pixels a row of the set mode steps by
                                           (an oversized screen's width)      */
    struct PBitMap *shownPbm;           /* bitmap the card scans out         */
    ULONG showStamp;
    BOOL rtgOn;
    ULONG palTab[256 * 3];
    UBYTE palRGB[256 * 3];
    struct PBitMap *testPbm;            /* PrismPrefs' Test picture          */
    UWORD testPen[256];
    ULONG testRgb[256];
    UBYTE testPal[256 * 3];
};
static struct CardState cst[MAX_CARDS];

#define palTab       (cst[0].palTab)
#define palRGB       (cst[0].palRGB)

static char *const monName[MAX_CARDS] = { "prism.monitor", "prism2.monitor" };

static struct ModeRec *mode_by_id(ULONG id)
{
    ULONG i, c = PRISM_ID_CARD(id);
    if (!IS_PRISM_ID(id) || c >= ncards)
        return NULL;
    for (i = 0; i < cst[c].nmodes; i++)
        if (cst[c].modes[i].id == id)
            return &cst[c].modes[i];
    return NULL;
}

static struct ModeRec *mode_by_handle(APTR h)
{
    UBYTE c;
    for (c = 0; c < ncards; c++) {
        ULONG d = (ULONG)h - (ULONG)cst[c].modes;
        if ((ULONG)h >= (ULONG)cst[c].modes && d < cst[c].nmodes * sizeof(struct ModeRec) &&
            !(d % sizeof(struct ModeRec)))
            return (struct ModeRec *)h;
    }
    return NULL;
}

/* One ModeRec per prefs slot the user left on and the board can show.
 * The ModeID comes from the slot (prefs.h), so it doesn't move when other
 * modes are switched off. 16- and 24/32-bit screens still have 256 pens;
 * their pixels are RGB in the board's format (pen -> colour when drawn). */
/* The nth format (from 0) the board has for a depth slot, or PF_COUNT.
 * 24 = three bytes a pixel, 32 = four; they are separate slots because on
 * the GD5434 they behave differently (32-bit has blitter text and fills,
 * 24-bit copies faster) and the user chooses. */
static UBYTE board_format(struct PrismBoard *b, UBYTE bits, UBYTE nth)
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
        if ((b->formats & PF_BIT(f[i])) && !nth--)
            return f[i];
    return PF_COUNT;
}

/* fill r->m for slot i in the first format the board accepts */
static BOOL mode_fits(struct PrismBoard *b, struct ModeRec *r, ULONG i, UBYTE bits, UBYTE hz)
{
    UBYTE nth, fmt;
    UWORD w, h;
    if (!prefs_size(&prefs, i, &w, &h))
        return FALSE;                        /* an unset custom slot */
    for (nth = 0; (fmt = board_format(b, bits, nth)) != PF_COUNT; nth++) {
        memset(&r->m, 0, sizeof(r->m));
        r->m.width = w;
        r->m.height = h;
        r->m.format = fmt;
        r->m.refresh = hz;
        if (b->checkMode(b, &r->m))
            return TRUE;
    }
    return FALSE;
}

static void build_modes(struct PrismCard *c)
{
    struct CardState *cs = &cst[c->index];
    ULONG i;

    for (i = 0; i < PREFS_NMODES && cs->nmodes < MAX_MODES; i++) {
        struct ModeRec *r = &cs->modes[cs->nmodes];
        UBYTE bits = prefs_depths[PREFS_DEPTH(i)];

        if (!prefs.on[i])
            continue;
        memset(r, 0, sizeof(*r));
        if (!mode_fits(c->b, r, i, bits, prefs.hz[i])) {
            if (!prefs.hz[i])
                continue;
            printf("PrismD: %ux%u %u-bit can't do %u Hz, using its default\n",
                   r->m.width, r->m.height, bits, prefs.hz[i]);
            if (!mode_fits(c->b, r, i, bits, 0))
                continue;
        }
        r->id = PRISM_CARD_ID(c->index) | PREFS_IDLOW(i);
        r->maxDepth = 8;
        r->bpp = pf_bpp(r->m.format);
        r->card = c;
        r->dimDepth = r->bpp == 4 ? 32 : bits;
        sprintf(r->name, "PRISM%s:%ux%u %ubit", c->index ? "2" : "", r->m.width, r->m.height,
                r->bpp == 4 ? 32 : bits);
        cs->nmodes++;
    }
}

/* SECONDMONITOR=SPAN (docs/multi-board.md): one screen across both
 * monitors. For each size the two boards both have at 8 or 16 bit, board 0
 * gets a mode twice as wide. Its bitmap lives on board 0, which shows its
 * half through the oversized-screen path (a row as long as the whole
 * screen); board 1 shows a copy of the other half (mon2_tick). 24 and 32
 * bit are left out: a Picasso II row is under 4096 bytes. */
static void build_span_modes(void)
{
    struct CardState *a = &cst[0], *b = &cst[1];
    ULONG i, j, n = a->nmodes;

    for (i = 0; i < n && a->nmodes < MAX_MODES; i++) {
        struct ModeRec *m0 = &a->modes[i], *r = &a->modes[a->nmodes];
        struct PrismMode t;
        if (m0->bpp > 2 || (m0->id & 0xff) >= SPAN_ID)
            continue;
        for (j = 0; j < b->nmodes; j++)
            if (b->modes[j].m.width == m0->m.width && b->modes[j].m.height == m0->m.height &&
                b->modes[j].bpp == m0->bpp)
                break;
        if (j == b->nmodes)
            continue;
        t = m0->m;
        t.pitchPixels = 2 * m0->m.width;      /* board 0 shows half of a double row */
        if (!cards[0].b->checkMode(cards[0].b, &t))
            continue;
        *r = *m0;
        r->m.width = 2 * m0->m.width;
        r->m.bytesPerRow = t.bytesPerRow;
        r->m.pitchPixels = 0;
        r->spanHalf = m0->m.width;
        r->id = m0->id | SPAN_ID;
        sprintf(r->name, "PRISM:%ux%u %ubit span", r->m.width, r->m.height, r->bpp == 2 ? 16 : 8);
        a->nmodes++;
    }
}

/* ---- VRAM allocator: first fit over a sorted list of used blocks --- */


LONG vram_alloc(struct PrismCard *c, ULONG size, UBYTE format, ULONG pitch, UWORD width, UWORD height)
{
    struct PrismBoard *B = c->b;
    struct CardState *cs = &cst[c->index];
    ULONG pos = 0;
    int i, j;

    if (!size || size > 0xffffff00UL) return -1;
    if (B->ops && B->ops->allocate) {
        struct PrismSurface surface={NULL,0,size,pitch,width,height,format,0,PSF_VRAM};
        surface.bpp=board_bpp(format);
        if (!B->ops->release || cs->nblk == MAX_BLKS || !B->ops->allocate(B,&surface)) return -1;
        pos=surface.offset;
        if (pos>B->vramSize || size>B->vramSize-pos) goto reject;
        for (i=0;i<cs->nblk && cs->blk[i].off<pos;i++);
        if ((i && cs->blk[i-1].off+cs->blk[i-1].size>pos) ||
            (i<cs->nblk && pos+size>cs->blk[i].off)) goto reject;
        for (j=cs->nblk;j>i;j--) cs->blk[j]=cs->blk[j-1];
        cs->blk[i].off=pos;cs->blk[i].size=size;cs->nblk++;
        return pos;
reject:
        B->ops->release(B,pos);
        return -1;
    }
    size = (size + 255) & ~255UL;
    if (cs->nblk == MAX_BLKS)
        return -1;
    for (i = 0; i <= cs->nblk; i++) {
        ULONG end = (i < cs->nblk) ? cs->blk[i].off : B->vramSize;
        if (end - pos >= size) {
            for (j = cs->nblk; j > i; j--)
                cs->blk[j] = cs->blk[j - 1];
            cs->blk[i].off = pos;
            cs->blk[i].size = size;
            cs->nblk++;
            return pos;
        }
        if (i < cs->nblk)
            pos = cs->blk[i].off + cs->blk[i].size;
    }
    return -1;
}

/* Free VRAM in bytes and the biggest free piece (lock held). */
ULONG vram_free_bytes(struct PrismCard *c, ULONG *largest)
{
    struct PrismBoard *B = c->b;
    struct CardState *cs = &cst[c->index];
    ULONG pos = 0, total = 0, big = 0;
    int i;
    for (i = 0; i <= cs->nblk; i++) {
        ULONG end = (i < cs->nblk) ? cs->blk[i].off : B->vramSize;
        ULONG gap = end > pos ? end - pos : 0;
        total += gap;
        if (gap > big) big = gap;
        if (i < cs->nblk)
            pos = cs->blk[i].off + cs->blk[i].size;
    }
    if (largest) *largest = big;
    return total;
}

void vram_free(struct PrismCard *c, ULONG off)
{
    struct PrismBoard *B = c->b;
    struct CardState *cs = &cst[c->index];
    int i;
    for (i = 0; i < cs->nblk; i++)
        if (cs->blk[i].off == off) {
            if (B->ops && B->ops->release) B->ops->release(B,off);
            for (; i < cs->nblk - 1; i++)
                cs->blk[i] = cs->blk[i + 1];
            cs->nblk--;
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
    BOOL             autoscroll;     /* bigger than its mode: the display
                                        shows rx,ry into it (ScrollVPort) */
    WORD             rx, ry;
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

BOOL pbm_is_shown(struct PBitMap *p)
{
    return p && p == cst[p->card->index].shownPbm;
}

UBYTE prism_ncards(void)
{
    return ncards;
}

/* A span screen (build_span_modes): board 0 shows one half of it, the one
 * on its own monitor's side - through the oversized-screen path, scrolled
 * to that half once and never again. */
static void span_place(struct PScreen *ps)
{
    if (!ps->mode->spanHalf)
        return;
    ps->autoscroll = TRUE;
    ps->rx = prefs.secondLeft ? ps->mode->spanHalf : 0;   /* the second monitor on the left: board 0 shows the right half */
    ps->ry = 0;
}

/* A span screen in front: the board the pointer at screen x is on, and
 * where that board's half starts on the screen (*ox). NULL: no span
 * screen in front. Lock held. */
struct PrismCard *prism_span_card(WORD x, WORD *ox)
{
    struct PScreen *ps = cst[0].shown;
    WORD half;
    if (ncards < 2 || !ps || !ps->mode->spanHalf || !IntuitionBase->FirstScreen ||
        ps->screen != IntuitionBase->FirstScreen)
        return NULL;
    half = ps->mode->spanHalf;
    if (x >= ps->rx && x < ps->rx + half) {
        *ox = ps->rx;
        return &cards[0];
    }
    *ox = ps->rx ? 0 : half;              /* board 1: the other half */
    return &cards[1];
}

WORD prism_span_origin(const struct PrismCard *c)
{
    WORD ox;
    struct PScreen *ps = cst[0].shown;
    if (!prism_span_card(0, &ox))
        return -1;
    return c == &cards[0] ? ps->rx : ps->rx ? 0 : ps->mode->spanHalf;
}

/* where the display starts in a bitmap: an oversized screen's scroll */
static ULONG scrolled_start(struct PBitMap *p)
{
    struct PScreen *ps = p->owner;
    if (ps && ps->autoscroll)
        return p->vramOff + (ULONG)ps->ry * p->bpr + (ULONG)ps->rx * p->bpp;
    return p->vramOff;
}

/* An oversized screen follows the pointer: the displayed part moves so the
 * pointer stays inside it, as Intuition's autoscroll would (which never
 * engages here: the ViewPort it opened is the whole bitmap, so it sees
 * nothing to scroll). Lock held. TRUE when the display moved. */
BOOL prism_autoscroll(struct PrismCard *c, WORD mx, WORD my)
{
    struct CardState *cs = &cst[c->index];
    struct PScreen *ps = cs->shown;
    LONG rx, ry, maxx, maxy, w, h;
    if (!ps || !ps->autoscroll || !cs->shownPbm || !cs->curMode || ps->mode->spanHalf)
        return FALSE;                     /* (a span screen's half stays put) */
    w = cs->curMode->m.width;
    h = cs->curMode->m.height;
    rx = ps->rx;
    ry = ps->ry;
    maxx = (LONG)ps->pbm->w - w;
    maxy = (LONG)ps->pbm->h - h;
    if (mx >= rx + w) rx = mx - w + 1;
    if (mx < rx) rx = mx;
    if (my >= ry + h) ry = my - h + 1;
    if (my < ry) ry = my;
    if (rx < 0) rx = 0;
    if (ry < 0) ry = 0;
    if (rx > maxx) rx = maxx < 0 ? 0 : maxx;
    if (ry > maxy) ry = maxy < 0 ? 0 : maxy;
    /* the Picasso II's display start is in longwords: rx a multiple of
     * four pixels, rounded towards the pointer so it stays in view */
    if (rx > ps->rx) rx = (rx + 3) & ~3L; else rx &= ~3L;
    if (rx > maxx) rx = maxx < 0 ? 0 : (maxx & ~3L);
    if (rx == ps->rx && ry == ps->ry)
        return FALSE;
    ps->rx = rx;
    ps->ry = ry;
    if (!present_ready(c))               /* (a composed frame would be switched off) */
        cs->shownPbm->bd->setDisplayStart(cs->shownPbm->bd, scrolled_start(cs->shownPbm));
    present_invalidate(c);
    return TRUE;
}

static struct PScreen *ps_by_screen(struct Screen *s);
/* is the screen an oversized (autoscroll) one? for p96.c: no PIPs on it */
BOOL prism_screen_autoscroll(struct Screen *s)
{
    struct PScreen *ps = ps_by_screen(s);
    return ps && ps->autoscroll;
}

BOOL prism_display_scroll(struct PrismCard *c, WORD *rx, WORD *ry)
{
    struct PScreen *ps = cst[c->index].shown;
    *rx = *ry = 0;
    if (!ps || !ps->autoscroll)
        return FALSE;
    *rx = ps->rx;
    *ry = ps->ry;
    return TRUE;
}

/* Point the card at a bitmap, paging it into VRAM first. Lock held. */
static BOOL show_pbm(struct PBitMap *p)
{
    struct CardState *cs = &cst[p->card->index];
    present_stop(p->card);
    cs->shownPbm = NULL;                     /* the old one may be evicted now */
    if (!pbm_to_vram(p)) {
        dbg("show: %ux%u doesn't fit in VRAM\n", p->w, p->h);
        return FALSE;
    }
    if (!pbm_upload(p)) return FALSE;
    p->bd->setDisplayStart(p->bd, scrolled_start(p));
    cs->shownPbm = p;
    p->lastShown = ++cs->showStamp;
    return TRUE;
}

/* pbm_free: a screen buffer is going; show its screen's own bitmap
 * instead if it was on display. Lock held.
 * A screen whose bitmap Intuition allocated (Workbench) is gone when
 * Intuition frees that bitmap. That is also the only sign of it when the
 * screen closes inside Intuition, past the CloseScreen patch: IPrefs'
 * Workbench reset after a ScreenMode change does. The screen Intuition
 * opens next can get the same address, so its record goes now - left in
 * the list, it would make the new screen look like the old Prism one and
 * the card show a bitmap that has been freed. */
void pbm_gone(struct PBitMap *p)
{
    struct CardState *cs = &cst[p->card->index];
    struct PScreen *ps, **pp;
    for (ps = pscreens; ps; ps = ps->next)
        if (ps->front == p)
            ps->front = NULL;
    for (pp = &pscreens; (ps = *pp); )
        if (ps->intuitionOwns && ps->pbm == p) {
            *pp = ps->next;
            if (cs->shown == ps) {
                cs->shown = NULL;
                cs->shownVP = NULL;
            }
            if (pending.ps == ps)
                pending.ps = NULL;
            FreeVec(ps);
        } else
            pp = &ps->next;
    if (cs->shownPbm == p) {
        present_stop(p->card);
        cs->shownPbm = NULL;
        if (cs->shown && cs->shown->pbm && cs->shown->pbm != p)
            show_pbm(cs->shown->pbm);
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
/* GAMMA=: the curve every palette goes to a board through. lut[v] is v
 * raised to 1/gamma (gamma above 1 brightens the midtones), computed once
 * from the prefs (libm's pow, software floating point: once at start). */
static UBYTE gammaLut[256];

static void gamma_init(void)
{
    ULONG i;
    for (i = 0; i < 256; i++)
        gammaLut[i] = i;
    if (prefs.gamma == 10 || prefs.gamma < 5 || prefs.gamma > 25)
        return;
    for (i = 1; i < 255; i++)
        gammaLut[i] = (UBYTE)(255.0 * pow(i / 255.0, 10.0 / prefs.gamma) + 0.5);
    printf("PrismD: gamma %u.%u: palettes through a curve (128 -> %u)\n", prefs.gamma / 10, prefs.gamma % 10,
           gammaLut[128]);
}

/* a board's palette, through the gamma curve */
static void board_palette(struct PrismCard *c, UWORD first, UWORD count, const UBYTE *rgb)
{
    static UBYTE out[256 * 3];
    ULONG i;
    if (count > 256) count = 256;
    for (i = 0; i < (ULONG)count * 3; i++)
        out[i] = gammaLut[rgb[i]];
    c->b->setPalette(c->b, first, count, out);
}

static void push_palette(struct ViewPort *vp)
{
    ULONG n, i;
    if (!vp || !vp->ColorMap)
        return;
    n = vp->ColorMap->Count;
    if (n > 256) n = 256;
    /* the DAC only matters to an 8-bit screen; deeper screens keep their
     * colours in their pen tables */
    {
        struct PScreen *ps = ps_by_vp(vp);
        struct PrismCard *c = ps ? ps->mode->card : &cards[0];
        struct PScreen *on = cst[c->index].shown;
        if (!on || on->mode->bpp == 1) {
            ps_colours(ps, vp->ColorMap, n);
            for (i = 0; i < n * 3; i++)
                palRGB[i] = palTab[i] >> 24;
            board_palette(c, 0, n, palRGB);
        }
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

struct PBitMap *prism_display_bitmap(struct PrismCard *c) { return cst[c->index].shownPbm; }

/* Intuition supplies the dragged front screen's origin. Different backing
 * formats are converted into the active display mode by the compositor. */
struct Screen *prism_display_layers(struct PrismCard *c,struct PBitMap **front,struct PBitMap **back,
    WORD *top,WORD *backTop,UWORD *width,UWORD *height)
{
    struct CardState *cs = &cst[c->index];
    struct Screen *s;
    *front=cs->shownPbm;*back=NULL;*top=*backTop=0;
    if (!cs->shown || !cs->shown->screen || !cs->shownPbm || !cs->curMode || !cs->rtgOn) return NULL;
    *width=cs->curMode->m.width;*height=cs->curMode->m.height;
    *top=cs->shown->screen->TopEdge;
    s=prism_back_screen(c,cs->shown->screen);
    if(s) {
        /* a native screen behind: nothing shows through, it stays black */
        struct PScreen *ps=ps_by_screen(s);
        if(ps && ps->mode->card==c) { *back=ps->front ? ps->front : ps->pbm;*backTop=s->TopEdge; }
    }
    return cs->shown->screen;
}

/* The screen a drag of s uncovers on card c: the next one behind it,
 * passing over the other board's screens - those are on their own monitor
 * (with two monitors, cycling the screens puts PRISM2's between two of
 * this card's, and the drag showed black). */
struct Screen *prism_back_screen(struct PrismCard *c, struct Screen *s)
{
    struct PScreen *ps;
    for (s = s ? s->NextScreen : NULL; s; s = s->NextScreen)
        if (!(ps = ps_by_screen(s)) || ps->mode->card == c)
            break;
    return s;
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
    paletteGen++;            /* after all the writes: the colour-to-pen caches */
    /* A cleared 8-bit bitmap is pen 0, which Intuition relies on and never
     * paints. A 16/24/32-bit one is black, so the first time the colours
     * are known (early in OpenScreen) paint it pen 0. */
    if (ps->mode->bpp >= 2 && !ps->painted && ps->pbm) {
        ps->painted = TRUE;
        SW_CLEAR(ps->pbm, 0, 0, 32767, 32767);    /* the locks above leave the pointer */
        pbm_fill_pen(ps->pbm, 0);
    }
    if (vp == cst[ps->mode->card->index].shownVP)
        push_palette(vp);
}

static struct PrismCard *ptrOn;            /* the card the pointer was put on */

/* Edge switching (two boards): the pointer pushed off the edge of one
 * monitor towards the other (SECONDSIDE=) brings the other board's front
 * screen forward, the pointer on its near edge at the same height. The
 * input handler (idle_handler) asks, the main loop does it (edge_tick).
 * update_display keeps what the handler reads: it never walks the screens. */
static volatile BYTE edgeFront = -1;      /* the card of the screen in front, -1: none of ours */
static volatile UWORD edgeW, edgeH;       /* that screen's size */
static volatile BOOL edgeHas[MAX_CARDS];  /* card k has a screen to go to */
static volatile BYTE edgeGo;              /* asked for: the card to go to + 1 */
static volatile UWORD edgeYFrac;          /* the pointer's height there, 0..65535 */
/* What card k shows: the first screen, from the front, that is either on
 * that card or native - a native screen in front of a board's screens
 * means the Amiga's video on that monitor, as with one board. Lock held. */
static struct PScreen *card_front(UBYTE k, struct Screen **sp)
{
    struct Screen *s;
    for (s = IntuitionBase->FirstScreen; s; s = s->NextScreen) {
        struct PScreen *ps = ps_by_screen(s);
        if (!ps)
            return NULL;
        if (ps->mode->card->index == k) {
            *sp = s;                     /* (ps->screen is still NULL while it opens) */
            return ps;
        }
    }
    return NULL;
}

/* ---- mirrored chipset screens (MIRRORCHIPSET=ON, PrismPrefs' Mirror chipset) ---
 *
 * A chipset screen in front - one promotion left alone: its program brings
 * its own bitmap, writes the bitplanes itself, or is on the Keep list -
 * stays a real chipset screen, and the primary board shows a chunky copy
 * of it on its monitor, in the smallest 256-colour mode that holds it, at
 * the top left. Every tick PrismD compares a share of the bitplane rows
 * with the copy it keeps and converts the rows that changed (the whole
 * screen within a few ticks); a new bitmap behind the ViewPort (double
 * buffering) is converted whole. The palette follows the screen's
 * ColorMap, with the halfbright colours of an EHB screen worked out. HAM
 * and dual playfield screens are not mirrored, nor are boards whose VRAM
 * the CPU cannot write directly. One screen at a time, on board 0. */
/* (the setting is MIRRORCHIPSET=; MIRROR= is the update mirror's address) */
void p2c8(const UBYTE **planes, ULONG *out, ULONG groups);   /* p2c.S */

static struct Mirror {
    struct Screen *s;
    struct BitMap *bm;            /* the bitmap converted last              */
    struct PBitMap *pbm;          /* the chunky copy on display             */
    UBYTE *shadow;                /* the planes as converted: depth x h rows */
    UWORD w, h, depth, rowBytes, next;
    UBYTE scale;                  /* 2: each pixel shown 2x2 (a lores screen) */
    BOOL ehb, full, started;
    UBYTE rgb[256 * 3];
} mir;
static const UBYTE mirZero[1024];     /* a plane the screen does not have */

static BOOL screen_listed(struct Screen *s)
{
    struct Screen *q;
    for (q = IntuitionBase->FirstScreen; q; q = q->NextScreen)
        if (q == s)
            return TRUE;
    return FALSE;
}

/* lock held */
static void mirror_stop(void)
{
    struct CardState *cs = &cst[0];
    if (!mir.s)
        return;
    dbg("mirror: %lx stops\n", (ULONG)mir.s);
    ptrMirror = NULL;
    ptrMirrorScale = 1;
    if (mir.pbm) {
        mir.pbm->locks--;
        if (cs->shownPbm == mir.pbm)
            cs->shownPbm = NULL;
        pbm_free(mir.pbm);
    }
    if (mir.shadow)
        FreeVec(mir.shadow);
    mir.s = NULL;
    mir.bm = NULL;
    mir.pbm = NULL;
    mir.shadow = NULL;
    cs->curMode = NULL;                  /* the next screen sets its mode again */
}

/* the screen's colours to the board (lock held) */
static void mirror_palette(BOOL force)
{
    static ULONG tab[256 * 3];
    struct PrismBoard *B = cards[0].b;
    UBYTE rgb[256 * 3];
    ULONG n = mir.ehb ? 32 : 1UL << mir.depth, i;
    struct ColorMap *cm = mir.s->ViewPort.ColorMap;
    if (!cm)
        return;
    if (n > 256)
        n = 256;
    if (n > (ULONG)cm->Count)
        n = cm->Count;
    GetRGB32(cm, 0, n, tab);
    for (i = 0; i < n * 3; i++)
        rgb[i] = tab[i] >> 24;
    if (mir.ehb) {                        /* 32-63: the first 32 at half brightness */
        for (i = 0; i < 32 * 3; i++)
            rgb[32 * 3 + i] = rgb[i] >> 1;
        n = 64;
    }
    if (!force && !memcmp(rgb, mir.rgb, n * 3))
        return;
    memcpy(mir.rgb, rgb, n * 3);
    B->setPalette(B, 0, n, rgb);
}

/* start mirroring screen s on board 0, or go on with it; TRUE while it is
 * mirrored (lock held) */
static BOOL mirror_show(struct Screen *s)
{
    struct CardState *cs = &cst[0];
    struct PrismBoard *B = cards[0].b;
    struct ViewPort *vp = &s->ViewPort;
    struct BitMap *bm = vp->RasInfo && vp->RasInfo->BitMap ? vp->RasInfo->BitMap : s->RastPort.BitMap;
    struct ModeRec *best = NULL;
    struct PrismMode m;
    struct PBitMap *p;
    ULONG area = ~0UL, i;
    UWORD w, h;

    mir.started = FALSE;
    if (mir.s == s)
        return TRUE;
    mirror_stop();
    if (!bm || pbm_get(bm) || (vp->Modes & (HAM | DUALPF)) || !bm->Depth || bm->Depth > 8 ||
        (B->flags & (PBF_SHADOW | PBF_PRESENT)) || cs->testPbm)
        return FALSE;
    w = vp->DWidth > 0 && vp->DWidth <= s->Width ? vp->DWidth : s->Width;
    h = vp->DHeight > 0 && vp->DHeight <= s->Height ? vp->DHeight : s->Height;
    if (w > bm->BytesPerRow * 8)
        w = bm->BytesPerRow * 8;
    if (h > bm->Rows)
        h = bm->Rows;
    if (!w || !h || w > 4096)
        return FALSE;
    /* a lores screen at twice the size where a mode holds that, else 1:1 */
    for (mir.scale = w <= 400 ? 2 : 1; mir.scale && !best; mir.scale--)
        for (i = 0; i < cs->nmodes; i++) {
            struct ModeRec *mr = &cs->modes[i];
            if (mr->bpp == 1 && mr->m.width >= w * mir.scale && mr->m.height >= h * mir.scale &&
                (ULONG)mr->m.width * mr->m.height < area) {
                area = (ULONG)mr->m.width * mr->m.height;
                best = mr;
            }
        }
    mir.scale++;                          /* the loop went one past */
    if (!best)
        return FALSE;
    present_stop(&cards[0]);
    m = best->m;
    m.pitchPixels = 0;
    if (!pbm_prepare_mode(&cards[0]) || !B->setMode(B, &m))
        return FALSE;
    cs->curMode = best;
    cs->curPitch = 0;
    cs->shown = NULL;
    cs->shownVP = NULL;
    p = pbm_new(&cards[0], best->m.width, best->m.height, 8, best->m.format, NULL, TRUE, TRUE);
    if (!p || !p->inVram || !show_pbm(p)) {
        if (p)
            pbm_free(p);
        cs->curMode = NULL;
        return FALSE;
    }
    p->locks++;                           /* VRAM paging leaves it where it is */
    mir.rowBytes = (w + 7) / 8;
    /* +4: p2c8 reads whole longs, up to 3 bytes past a row's end */
    if (!(mir.shadow = AllocVec((ULONG)bm->Depth * mir.rowBytes * h + 4, MEMF_ANY | MEMF_CLEAR))) {
        p->locks--;
        cs->shownPbm = NULL;
        pbm_free(p);
        cs->curMode = NULL;
        return FALSE;
    }
    mir.s = s;
    mir.bm = bm;
    mir.pbm = p;
    mir.w = w;
    mir.h = h;
    mir.depth = bm->Depth;
    mir.ehb = (vp->Modes & EXTRA_HALFBRITE) && bm->Depth == 6;
    mir.next = 0;
    mir.full = TRUE;
    mir.started = TRUE;
    mirror_palette(TRUE);
    if (!cs->rtgOn) {
        B->setSwitch(B, TRUE);
        cs->rtgOn = TRUE;
    }
    ptrMirror = s;
    ptrMirrorScale = mir.scale;
    dbg("mirror: %lx %ux%ux%u x%u on %s\n", (ULONG)s, w, h, mir.depth, mir.scale, best->name);
    return TRUE;
}

/* the main loop, every tick: the rows that changed (lock taken here) */
static void mirror_tick(void)
{
    struct PrismBoard *B = cards[0].b;
    struct ViewPort *vp;
    struct BitMap *bm;
    const UBYTE *pp[8];
    UWORD rows, n, y, k, groups, rx = 0, ry = 0;
    BOOL open;

    if (!mir.s)
        return;
    LOCK_QUIET();
    Forbid();
    open = mir.s && screen_listed(mir.s);
    Permit();
    if (!open) {                          /* closed inside Intuition, past the patch */
        mirror_stop();
        ReleaseSemaphore(&lock);
        return;
    }
    vp = &mir.s->ViewPort;
    bm = vp->RasInfo && vp->RasInfo->BitMap ? vp->RasInfo->BitMap : mir.s->RastPort.BitMap;
    if (bm != mir.bm) {                   /* double buffering: a new bitmap, whole */
        mir.bm = bm;
        mir.full = TRUE;
    }
    if (!bm || bm->Depth < mir.depth) {
        ReleaseSemaphore(&lock);
        return;
    }
    if (vp->RasInfo) {
        if (!(vp->RasInfo->RxOffset & 7))
            rx = vp->RasInfo->RxOffset >> 3;
        ry = vp->RasInfo->RyOffset;
    }
    if (ry + mir.h > bm->Rows)
        ry = 0;
    mirror_palette(FALSE);
    rows = mir.full ? mir.h : 32768 / ((ULONG)mir.rowBytes * mir.depth);
    if (rows < 8)
        rows = 8;
    if (rows > mir.h)
        rows = mir.h;
    groups = (mir.w + 31) / 32;
    if ((ULONG)groups * 32 * mir.scale > mir.pbm->bpr)
        groups = mir.pbm->bpr / (32 * mir.scale);
    if (B->waitBlit)
        B->waitBlit(B);
    for (n = 0; n < rows; n++) {
        BOOL changed = mir.full;
        y = mir.next;
        mir.next = mir.next + 1 < mir.h ? mir.next + 1 : 0;
        for (k = 0; k < mir.depth; k++) {
            const UBYTE *src = bm->Planes[k] + (ULONG)(y + ry) * bm->BytesPerRow + rx;
            UBYTE *sh = mir.shadow + ((ULONG)k * mir.h + y) * mir.rowBytes;
            if (changed || memcmp(src, sh, mir.rowBytes)) {
                CopyMem((APTR)src, sh, mir.rowBytes);
                changed = TRUE;
            }
            pp[k] = sh;
        }
        if (!changed)
            continue;
        for (k = mir.depth; k < 8; k++)
            pp[k] = mirZero;
        if (mir.scale == 1) {
            p2c8(pp, (ULONG *)(mir.pbm->pix + (ULONG)y * mir.pbm->bpr), groups);
        } else {
            /* the row once, then every pixel twice on two rows */
            static ULONG one[4096 / 4];
            UBYTE *c = (UBYTE *)one;
            UWORD *d0 = (UWORD *)(mir.pbm->pix + (ULONG)y * 2 * mir.pbm->bpr);
            UWORD *d1 = (UWORD *)((UBYTE *)d0 + mir.pbm->bpr);
            UWORD x, nx = groups * 32;
            p2c8(pp, one, groups);
            for (x = 0; x < nx; x++)
                d0[x] = d1[x] = (UWORD)(c[x] * 0x0101);
        }
    }
    mir.full = FALSE;
    ReleaseSemaphore(&lock);
}

static BOOL mon2_show(void);              /* SECONDMONITOR=MIRROR, further down */
static void mon2_stop(void);
static struct PScreen *span_source(void);

/* Make card k show its front screen. TRUE if it now shows another screen
 * than before (the pointer has to be set up again). Lock held. */
static BOOL show_card(UBYTE k)
{
    struct CardState *cs = &cst[k];
    struct PrismCard *c = &cards[k];
    struct PrismBoard *B = c->b;
    struct Screen *s = NULL;
    struct PScreen *ps = card_front(k, &s);

    if (cs->testPbm)                     /* PrismPrefs' Test picture: left alone */
        return FALSE;
    if (ps) {
        struct PBitMap *dp = ps->front ? ps->front : ps->pbm;
        if (k == 0)
            mirror_stop();
        else
            mon2_stop();                  /* a screen of its own: no copy of board 0 */
        UWORD pitch = ps->autoscroll ? dp->bpr / dp->bpp : 0;
        BOOL changed = FALSE;
        if (cs->curMode != ps->mode || cs->curPitch != pitch) {
            present_stop(c);
            struct PrismMode m = ps->mode->m;
            m.pitchPixels = pitch;
            if (ps->mode->spanHalf)
                m.width = ps->mode->spanHalf;     /* half of the span screen's row */
            /* Rebuilding the pool releases the old scanout allocation. */
            if ((B->flags & PBF_REINIT) && cs->rtgOn) {
                B->setSwitch(B, FALSE);
                cs->rtgOn = FALSE;
            }
            if (!pbm_prepare_mode(c) || !B->setMode(B, &m)) {
                B->setSwitch(B, FALSE);
                cs->rtgOn = FALSE;
                cs->shown = NULL;
                cs->shownPbm = NULL;
                cs->shownVP = NULL;
                cs->curMode = NULL;
                return TRUE;
            }
            cs->curMode = ps->mode;
            cs->curPitch = pitch;
            cs->shown = NULL;
        }
        if (cs->shown != ps || cs->shownPbm != dp)
            show_pbm(dp);
        if (cs->shown != ps) {
            cs->shown = ps;
            cs->shownVP = &s->ViewPort;
            push_palette(cs->shownVP);
            changed = TRUE;               /* a mode set turns the cursor off too */
        }
        if (!cs->rtgOn) {
            B->setSwitch(B, TRUE);
            cs->rtgOn = TRUE;
        }
        return changed;
    }
    present_stop(c);
    /* SECONDMONITOR=MIRROR: nothing of its own on board 1, so a copy of board 0;
     * SPAN: a copy of the half of board 0's span screen board 0 doesn't show */
    if (k == 1 && (prefs.secondMon == SM_MIRROR || span_source()) && mon2_show())
        return FALSE;
    if (k == 1)
        mon2_stop();
    /* MIRRORCHIPSET=ON: a chipset screen in front is shown on board 0 as a copy */
    if (k == 0 && prefs.mirrorChip && IntuitionBase->FirstScreen && mirror_show(IntuitionBase->FirstScreen))
        return mir.started;
    if (k == 0)
        mirror_stop();
    /* No screen at all (Workbench closed for a reset, the next one not
     * open yet) is no reason to flip the monitor: the next screen sets
     * it. A native screen in front is. */
    if (cs->rtgOn && IntuitionBase->FirstScreen) {
        B->setSwitch(B, FALSE);
        cs->rtgOn = FALSE;
    }
    /* program the mode again on the way back: WinUAE only re-enables
     * its RTG view on a mode change, and it costs nothing on hardware */
    cs->curMode = NULL;
    cs->shown = NULL;
    cs->shownVP = NULL;
    cs->shownPbm = NULL;
    return FALSE;
}

/* Make the boards follow the screens: each shows its own front screen,
 * and the pointer is on the board of the screen in front of all. */
static void update_display(void)
{
    struct Screen *s = IntuitionBase->FirstScreen;
    struct PScreen *ps;
    BOOL changed[MAX_CARDS] = { FALSE };
    UBYTE k;

    LOCK();
    ps = ps_by_screen(s);
    dbg("LoadView: front %lx %s\n", (ULONG)s, ps ? "PRISM" : "native");
    for (k = 0; k < ncards; k++)
        changed[k] = show_card(k);
    dbg("display: front card %ld shown %lx ps %lx changed %ld/%ld pointer on card %ld\n",
        ps ? (LONG)ps->mode->card->index : -1L, ps ? (ULONG)cst[ps->mode->card->index].shown : 0UL,
        (ULONG)ps, (LONG)changed[0], (LONG)changed[1], ptrOn ? (LONG)ptrOn->index : -1L);
    if (ps && cst[ps->mode->card->index].shown == ps) {
        struct PrismCard *c = ps->mode->card;
        if (changed[c->index] || ptrOn != c) {
            pointer_on(&s->ViewPort, c);
            ptrOn = c;
        }
    } else if (mir.s && s == mir.s) {
        if (changed[0] || ptrOn != &cards[0]) {
            pointer_on(&s->ViewPort, &cards[0]);
            ptrOn = &cards[0];
        }
    } else {
        pointer_off();
        ptrOn = NULL;
    }
    /* what the edge switch reads (idle_handler) */
    {
        struct Screen *q;
        BOOL has[MAX_CARDS] = { FALSE };
        for (q = IntuitionBase->FirstScreen; q; q = q->NextScreen) {
            struct PScreen *qs = ps_by_screen(q);
            if (qs)
                has[qs->mode->card->index] = TRUE;
        }
        for (k = 0; k < MAX_CARDS; k++)
            edgeHas[k] = has[k];
        if (ps && s && !ps->mode->spanHalf) {
            edgeW = s->Width;
            edgeH = s->Height;
            edgeFront = ps->mode->card->index;
        } else
            edgeFront = -1;               /* (a span screen: the pointer just goes across) */
    }
    ReleaseSemaphore(&lock);
}

/* ---- calling the original functions -------------------------------- */

static APTR o_NextDisplayInfo, o_FindDisplayInfo, o_GetDisplayInfoData,
            o_ModeNotAvailable, o_OpenMonitor, o_CloseMonitor, o_MakeVPort,
            o_MrgCop, o_LoadView, o_SetRGB32, o_LoadRGB32, o_SetRGB4,
            o_LoadRGB4, o_OpenScreenTagList, o_OpenScreen, o_CloseScreen, o_ScrollVPort, o_CloseWindow,
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

/* Patches can be running in other tasks when PrismD wants to quit
 * (stubs.S counts its handler calls here too). */
volatile LONG inPatch;

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
        ULONG c = PRISM_ID_CARD(id);
        ULONG i = m ? (ULONG)(m - cst[c].modes) + 1 : cst[c].nmodes;
        r = INVALID_ID;
        if (c < ncards && i < cst[c].nmodes)
            r = cst[c].modes[i].id;
        else
            for (c++; c < ncards && r == INVALID_ID; c++)
                if (cst[c].nmodes)
                    r = cst[c].modes[0].id;
    } else {
        ULONG c;
        r = c_d0(o_NextDisplayInfo, GfxBase, id);
        for (c = 0; c < ncards && r == INVALID_ID; c++)
            if (cst[c].nmodes)
                r = cst[c].modes[0].id;
    }
    LEAVE();
    return r;
}

#ifdef PRISM_MISSLOG
/* test builds: which IDs go to graphics' own (slow) database search */
static struct { ULONG key, n; char what; } miss[24];
static void miss_count(char what, ULONG key)
{
    int i;
    for (i = 0; i < 24 && miss[i].n && (miss[i].key != key || miss[i].what != what); i++);
    if (i < 24) { miss[i].key = key; miss[i].what = what; miss[i].n++; }
}
static void miss_print(void)
{
    int i;
    for (i = 0; i < 24 && miss[i].n; i++)
        printf("PrismD miss: %c %08lx x%lu\n", miss[i].what, (unsigned long)miss[i].key, (unsigned long)miss[i].n);
    fflush(stdout);
}
#define MISS(w, k) miss_count(w, k)
#else
#define MISS(w, k) ((void)0)
#endif

static APTR P_FindDisplayInfo(ULONG id __asm("d0"))
{
    APTR r;
    ENTER();
    r = mode_by_id(id);
    pending_saw(r);
    if (IS_PRISM_ID(id)) dbg("FindDisplayInfo %lx -> %lx\n", id, (ULONG)r);
    if (!r) {
        MISS('F', id);
        r = (APTR)c_d0(o_FindDisplayInfo, GfxBase, id);
    }
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

/* Ticks per Prism pixel (Resolution, ViewResolution, SpriteResolution,
 * MouseTicks: all the same, so a pixel is square and one mouse count moves
 * the pointer one pixel). Intuition keeps the pointer in ticks in a WORD:
 * at 22 (hires) it stopped near x 1360 on a 1920x1080 screen (issue #1,
 * beta 5). 11 (super hires) reaches 2978 pixels. */
#ifndef PRISM_TICKS
#define PRISM_TICKS 11
#endif

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
        MISS(h ? 'H' : 'I', h ? (ULONG)h : id);
        MISS('T', tag);
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
        if (prefs.dragging) u.di.PropertyFlags |= DIPF_IS_DRAGGABLE;
        u.di.Resolution.x = PRISM_TICKS;
        u.di.Resolution.y = PRISM_TICKS;
        u.di.PixelSpeed = 35;
        u.di.PaletteRange = 4096;
        u.di.NumStdSprites = 1;           /* the hardware cursor          */
        u.di.SpriteResolution.x = PRISM_TICKS;     /* sprite pixels = screen pixels */
        u.di.SpriteResolution.y = PRISM_TICKS;
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
        u.mi.Mspc = &cst[r->card->index].monitor;
        u.mi.ViewResolution.x = PRISM_TICKS;
        u.mi.ViewResolution.y = PRISM_TICKS;
        u.mi.ViewPositionRange.MaxX = r->m.width - 1;
        u.mi.ViewPositionRange.MaxY = r->m.height - 1;
        u.mi.TotalRows = r->m.height;
        u.mi.TotalColorClocks = r->m.width;
        u.mi.Compatibility = MCOMPAT_SELF;
        /* ticks the pointer moves per mouse count: one pixel, as P96
         * reports it. With 1,1 here Intuition moved the pointer at half
         * speed (measured on the A4000 with relative mouse events). */
        u.mi.MouseTicks.x = PRISM_TICKS;
        u.mi.MouseTicks.y = PRISM_TICKS;
        u.mi.PreferredModeID = r->id;
        break;
    case DTAG_NAME:
        len = sizeof(u.ni);
        qhdr(&u.ni.Header, tag, r->id, len);
        memcpy(u.ni.Name, r->name, sizeof(u.ni.Name));
        u.ni.Name[sizeof(u.ni.Name) - 1] = 0;
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
    struct MonitorSpec *r = NULL;
    struct ModeRec *m;
    UBYTE c;
    ENTER();
    for (c = 0; c < ncards && !r; c++)
        if (name && !strcmp((const char *)name, monName[c]))
            r = &cst[c].monitor;
    if (!r && !name && (m = mode_by_id(id)))
        r = &cst[m->card->index].monitor;
    if (r) {
        Forbid();
        r->ms_OpenCount++;
        Permit();
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
    if (ms == &cst[0].monitor || (ncards > 1 && ms == &cst[1].monitor)) {
        Forbid();
        if (ms->ms_OpenCount)
            ms->ms_OpenCount--;
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

    LOCK_QUIET();
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
            paletteGen++;    /* after the writes: the colour-to-pen caches */
            struct CardState *cs = &cst[ps->mode->card->index];
            if (vp == cs->shownVP) {
                if (!cs->shown || cs->shown->mode->bpp == 1)
                    board_palette(ps->mode->card, n, 1, c);
                if (n >= 16 && n < 32)           /* the pointer's colours live here */
                    pointer_colours(vp);
            }
        }
    }
    ReleaseSemaphore(&lock);
}

static void palette_changed(struct ViewPort *vp)
{
    LOCK_QUIET();
    palette_update(vp);
    ReleaseSemaphore(&lock);
}

static void record_vp(struct ViewPort *vp, ULONG n, ULONG r, ULONG g, ULONG b)
{
    LOCK_QUIET();
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
        LOCK_QUIET();
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
    if (!(flags & BMF_DISPLAYABLE))
        return NULL;
    /* The screen's size comes from the ScreenMode prefs, which can keep an
     * old size with a new mode (800x600 on 1280x1024). Smaller than the
     * mode: the bitmap is the mode's size and the screen sits at its top
     * left, as on every other Prism screen the row size is the mode's.
     * Larger (a scrolling screen) or deeper than the mode can show: the
     * open fails here and Intuition falls back as for any mode it can't
     * open. Passing it on would give the screen a planar chip RAM bitmap
     * on a Prism mode, which nothing can show: the card is never told,
     * and the monitor switch goes to the Amiga's own (black) video. */
    if (w > mr->m.width || h > mr->m.height || depth > mr->maxDepth) {
        dbg("  refused: the mode is %ux%u, depth up to %u\n", mr->m.width, mr->m.height,
            mr->maxDepth);
        return PBM_REFUSED;
    }
    w = mr->m.width;
    h = mr->m.height;
    if (!(ps = AllocVec(sizeof(*ps), MEMF_PUBLIC | MEMF_CLEAR)))
        return PBM_REFUSED;
    if (!(ps->pbm = pbm_new(mr->card, w, h, depth ? depth : 1, mr->m.format,
                            mr->bpp == 2 ? ps->penTab : NULL, TRUE, TRUE)) ||
        ps->pbm->bpr != mr->m.bytesPerRow) {
        pbm_free(ps->pbm);
        FreeVec(ps);
        return PBM_REFUSED;
    }
    ps->pbm->fmt = mr->m.format;
    ps->pbm->rgbTab = ps->rgbTab;
    ps->pbm->owner = ps;
    ps->mode = mr;
    ps->depth = depth;
    ps->intuitionOwns = TRUE;
    span_place(ps);
    LOCK();
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
            LOCK();
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
    struct CardState *cs = &cst[ps->mode->card->index];
    LOCK();
    if (pscreens == ps) {
        pscreens = ps->next;
    } else {
        struct PScreen *p;
        for (p = pscreens; p && p->next != ps; p = p->next) ;
        if (p) p->next = ps->next;
    }
    if (cs->shown == ps) {
        present_stop(ps->mode->card);
        cs->shown = NULL;
        cs->shownVP = NULL;
        cs->shownPbm = NULL;
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
    ULONG w = mr->m.width, h = mr->m.height, depth, rw, rh;
    BOOL big = FALSE;

    BOOL viaHook;

    depth = tag_data(tags, ext, SA_Depth, ns ? ns->Depth : 8);
    if (depth < 1) depth = 1;
    if (depth > mr->maxDepth) depth = mr->maxDepth;

    /* An oversized screen (SA_AutoScroll, wider or taller than the mode):
     * the bitmap is the size asked for and the display shows a mode-sized
     * part of it, scrolled by Intuition through ScrollVPort. Only where the
     * driver can step rows by the bitmap's pitch; else the mode's size. */
    rw = tag_data(tags, ext, SA_Width, ns ? (ULONG)(WORD)ns->Width : (ULONG)STDSCREENWIDTH);
    rh = tag_data(tags, ext, SA_Height, ns ? (ULONG)(WORD)ns->Height : (ULONG)STDSCREENHEIGHT);
    if (tag_data(tags, ext, SA_AutoScroll, FALSE) && (LONG)rw > 0 && (LONG)rh > 0 &&
        rw <= 4096 && rh <= 4096 && (rw > w || rh > h)) {
        struct PrismMode m = mr->m;
        if (rw < w) rw = w;
        if (rh < h) rh = h;
        rw = (rw + 7) & ~7UL;
        m.pitchPixels = rw;
        /* the driver sees the mode's own height (a ZZ9000 looks modes up by
         * size); the whole bitmap's video memory is checked here */
        if (mr->card->b->checkMode(mr->card->b, &m) &&
            (ULONG)m.bytesPerRow * rh <= mr->card->b->vramSize) {
            big = TRUE;
            w = rw;
            h = rh;
            dbg("open: oversized %lux%lu on %s (autoscroll)\n", w, h, mr->name);
        }
    }

    /* Preferred: Intuition allocates the screen's bitmap itself and the
     * AllocBitMap hook hands it ours (as for Workbench), so the screen is an
     * ordinary one - not CUSTOMBITMAP, which AllocScreenBuffer() refuses.
     * If another open is being watched, fall back to SA_BitMap. */
    if (!big && (viaHook = pending_begin())) {
        pending.mr = mr;
        ps = NULL;
        goto tags;
    }
    viaHook = FALSE;

    if (!(ps = AllocVec(sizeof(*ps), MEMF_PUBLIC | MEMF_CLEAR)))
        return NULL;
    ps->mode = mr;
    ps->depth = depth;
    ps->autoscroll = big;
    span_place(ps);

    /* the screen's chunky bitmap, cleared, in VRAM if it fits (else fast
     * RAM until it is shown); its row size must be the mode's (modes are
     * multiples of 8 pixels wide, so it is) - an oversized one has its own */
    if (!(ps->pbm = pbm_new(mr->card, w, h, depth, mr->m.format, mr->bpp == 2 ? ps->penTab : NULL,
                            TRUE, TRUE)) || (!big && ps->pbm->bpr != mr->m.bytesPerRow)) {
        dbg("open: no memory for %lux%lu\n", w, h);
        pbm_free(ps->pbm);
        FreeVec(ps);
        return NULL;
    }
    ps->pbm->fmt = mr->m.format;
    ps->pbm->rgbTab = ps->rgbTab;
    ps->pbm->owner = ps;

    LOCK();
    ps->next = pscreens;
    pscreens = ps;
    ReleaseSemaphore(&lock);

    /* Our size/depth/bitmap win; everything else the caller asked for
     * passes through. */
tags:
    if (tags && (clone = CloneTagItems(tags))) {
        FilterTagItems(clone, filterOut, TAGFILTER_NOT);
        if (big) {
            struct TagItem *ti = FindTagItem(TAG_IGNORE, clone);   /* a filtered slot */
            if (ti) { ti->ti_Tag = SA_AutoScroll; ti->ti_Data = TRUE; }
        }
    }
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
    LOCK();
    palette_update(&s->ViewPort);               /* fill a 16-bit pen table */
    ReleaseSemaphore(&lock);
    return s;
}

/* The name a program is known by in the promotion list: its command name
 * (a CLI program) or its task name (started from Workbench). */
static const char *program_name(char *buf, int n)
{
    struct Task *t = FindTask(NULL);
    const char *s = t->tc_Node.ln_Name;
    if (t->tc_Node.ln_Type == NT_PROCESS && ((struct Process *)t)->pr_CLI) {
        struct CommandLineInterface *cli = BADDR(((struct Process *)t)->pr_CLI);
        const UBYTE *b = cli ? BADDR(cli->cli_CommandName) : NULL;
        if (b && b[0]) {
            int len = b[0] < n - 1 ? b[0] : n - 1;
            memcpy(buf, b + 1, len);
            buf[len] = 0;
            s = (const char *)FilePart((STRPTR)buf);
        }
    }
    return s ? s : "?";
}

/* a mode of the chipset: the default monitor or one of the Amiga ones */
static BOOL native_mode_id(ULONG id)
{
    ULONG mon = id & MONITOR_ID_MASK;
    return mon == 0 || (mon >= NTSC_MONITOR_ID && mon <= DBLPAL_MONITOR_ID && (mon & 0x1000));
}

/* Screen mode promotion (PROMOTE= in the prefs): a screen asked for on
 * the chipset opens on the primary board instead, in its smallest
 * 256-colour mode that holds it. NULL: leave it to the chipset. */
static struct ModeRec *promote_mode(struct NewScreen *ns, struct TagItem *tags,
                                    struct TagItem *ext)
{
    struct CardState *cs;
    struct ModeRec *best = NULL;
    ULONG id, w, h, depth, i, area = ~0UL;
    char buf[64];
    const char *prog;
    int k;

    if (!ncards || !cst[0].nmodes)
        return NULL;
    id = tag_data(tags, ext, SA_DisplayID, INVALID_ID);
    if (id != INVALID_ID && !native_mode_id(id))
        return NULL;                             /* another RTG system's mode */
    if ((ns && (ns->Type & CUSTOMBITMAP)) || tag_data(tags, ext, SA_BitMap, 0) ||
        tag_data(tags, ext, SA_LikeWorkbench, FALSE))
        return NULL;
    /* not Workbench itself: its mode is the ScreenMode preferences' choice,
     * and the name promotion would match is whichever task opened it */
    if (tag_data(tags, ext, SA_Type, ns ? (ULONG)(ns->Type & SCREENTYPE) : CUSTOMSCREEN) == WBENCHSCREEN)
        return NULL;
    prog = program_name(buf, sizeof(buf));
    k = prefs_prog_find(&prefs, prog);
    if (k >= 0 ? prefs.prog[k].keep : !prefs.promote)
        return NULL;
    depth = tag_data(tags, ext, SA_Depth, ns ? ns->Depth : 2);
    if (depth > 8)
        return NULL;
    w = tag_data(tags, ext, SA_Width, ns ? (ULONG)(WORD)ns->Width : (ULONG)STDSCREENWIDTH);
    h = tag_data(tags, ext, SA_Height, ns ? (ULONG)(WORD)ns->Height : (ULONG)STDSCREENHEIGHT);
    if ((LONG)w <= 0 || w > 4096 || (LONG)h <= 0 || h > 4096) {
        /* STDSCREENWIDTH/HEIGHT: the chipset mode's nominal size (a lores
         * game is 320 wide, a laced PAL screen 512 high); with no mode at
         * all, the NewScreen's ViewModes bits are a default-monitor ID */
        struct DimensionInfo di;
        ULONG mid = id != INVALID_ID ? id : ns ? (ULONG)(ns->ViewModes & (HIRES | LACE | SUPERHIRES)) : HIRES_KEY;
        if (GetDisplayInfoData(NULL, (UBYTE *)&di, sizeof(di), DTAG_DIMS, mid) > 0) {
            if ((LONG)w <= 0 || w > 4096) w = di.Nominal.MaxX - di.Nominal.MinX + 1;
            if ((LONG)h <= 0 || h > 4096) h = di.Nominal.MaxY - di.Nominal.MinY + 1;
        }
        if ((LONG)w <= 0 || w > 4096) w = 640;
        if ((LONG)h <= 0 || h > 4096) h = 256;
    }
    cs = &cst[0];
    for (i = 0; i < cs->nmodes; i++) {
        struct ModeRec *mr = &cs->modes[i];
        if (mr->bpp == 1 && mr->m.width >= w && mr->m.height >= h &&
            (ULONG)mr->m.width * mr->m.height < area) {
            area = (ULONG)mr->m.width * mr->m.height;
            best = mr;
        }
    }
    dbg("promote: %s asks %lux%lu depth %lu id %lx -> %s\n", prog, w, h, depth, id,
        best ? best->name : "no mode holds it");
    return best;
}

static struct ModeRec *wanted_mode(struct NewScreen *ns, struct TagItem *tags,
                                   struct TagItem **extOut)
{
    struct TagItem *ext = NULL;
    struct ModeRec *mr;
    if (ns && (ns->Type & NS_EXTENDED))
        ext = ((struct ExtNewScreen *)ns)->Extension;
    *extOut = ext;
    if (!(mr = mode_by_id(tag_data(tags, ext, SA_DisplayID, INVALID_ID))))
        mr = promote_mode(ns, tags, ext);
    return mr;
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

/* ScrollVPort(vp a0): Intuition's autoscroll moved an oversized Prism
 * screen's RasInfo offsets - show that part. Other ViewPorts go on to
 * graphics. */
static void P_ScrollVPort(struct ViewPort *vp __asm("a0"))
{
    struct PScreen *ps;
    ENTER();
    LOCK();
    ps = ps_by_vp(vp);
    if (ps && ps->autoscroll) {
        /* PrismD scrolls it from the pointer (prism_autoscroll); graphics
         * would rebuild a native view for it */
        struct CardState *cs = &cst[ps->mode->card->index];
        if (cs->shown == ps && cs->shownPbm && !present_ready(ps->mode->card))
            cs->shownPbm->bd->setDisplayStart(cs->shownPbm->bd, scrolled_start(cs->shownPbm));
        ReleaseSemaphore(&lock);
        LEAVE();
        return;
    }
    ReleaseSemaphore(&lock);
    c_a0(o_ScrollVPort, GfxBase, vp);
    LEAVE();
}

/* CloseWindow(w a0): a video layer (cgxvideo.library) attached to it must
 * not be composed from a freed window: its PIP record leaves the list now
 * (the layer's handle keeps the record; DetachVLayer frees it). */
static void P_CloseWindow(struct Window *w __asm("a0"))
{
    ENTER();
    p96_pip_window_gone(w);
    c_a0(o_CloseWindow, IntuitionBase, w);
    LEAVE();
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
    LOCK();
    ps = ps_by_screen(s);
    if (s == mir.s)                      /* its planes go with it */
        mirror_stop();
    ReleaseSemaphore(&lock);
    r = c_a0(o_CloseScreen, IntuitionBase, s);
    if (r && ps) {
        /* a Workbench-style screen's record went with its bitmap (pbm_gone) */
        struct PScreen *q;
        LOCK_QUIET();
        for (q = pscreens; q && q != ps; q = q->next) ;
        ReleaseSemaphore(&lock);
        if (q)
            free_ps(ps);
    }
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

    LOCK();
    ps = ps_by_vp(vp);
    p = pbm_get(bm);
    if (!ps || !p || (p != ps->pbm && p->owner != ps)) {
        ReleaseSemaphore(&lock);
        return 0;
    }
    if (vp->RasInfo)
        vp->RasInfo->BitMap = bm;
    ps->front = (p == ps->pbm) ? NULL : p;
    if (ps == cst[ps->mode->card->index].shown && show_pbm(p) && p->bd->waitVBlank)
        p->bd->waitVBlank(p->bd);
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

    LOCK();                              /* it hands out the pixels: no sprite in them */
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
TRAMP(CoerceMode) TRAMP(BitMapScale) TRAMP(DrawGList) TRAMP(RemIBob)
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
    LOCK_QUIET();
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
        LOCK_QUIET();
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

static LONG test_begin_on(UBYTE card, ULONG width, ULONG height, ULONG bits, ULONG hz,
                          const char *label)
{
    struct CardState *cs;
    struct PrismCard *c;
    struct PrismMode m;
    struct PBitMap *p;
    struct RastPort rp;
    UBYTE nth, fmt, *buf;
    UWORD y, i;
    int kind = -1;

    if (card >= ncards)
        return PRISM_TEST_NOMODE;
    cs = &cst[card];
    c = &cards[card];

    if (cs->testPbm)
        return PRISM_TEST_BUSY;
    for (nth = 0; (fmt = board_format(c->b, bits, nth)) != PF_COUNT; nth++) {
        memset(&m, 0, sizeof(m));
        m.width = width;
        m.height = height;
        m.format = fmt;
        m.refresh = hz;
        if (c->b->checkMode(c->b, &m))
            break;
    }
    if (fmt == PF_COUNT)
        return PRISM_TEST_NOMODE;

    cs->testPen[0] = 0;
    cs->testPen[1] = rgb16(fmt, 255, 255, 255);
    cs->testRgb[0] = 0;
    cs->testRgb[1] = 0xffffff;
    if (!(p = pbm_new(c, width, height, 8, fmt, cs->testPen, TRUE, FALSE)))
        return PRISM_TEST_NOMEM;
    p->fmt = fmt;
    p->rgbTab = cs->testRgb;
    buf = AllocVec(p->bpr, MEMF_ANY | MEMF_CLEAR);

    LOCK();
    present_stop(c);                     /* before what it composes can go */
    /* The screen on display may have to leave VRAM to make room (a 2 MB
     * card can't hold an 800x600 16-bit Workbench and a 1024x768 16-bit
     * test picture): let it be paged out like any other bitmap. It comes
     * back when test_end() shows the front screen again. */
    cs->shownPbm = NULL;
    if (!buf || p->bpr != m.bytesPerRow || !pbm_to_vram(p)) {
        cs->curMode = NULL;
        cs->shown = NULL;
        cs->shownVP = NULL;
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

    /* the pointer, if it is on this board: the test picture has none (on
     * the other board it stays where it is) */
    dbg("test on card %ld, pointer on card %ld\n", (LONG)card, ptrOn ? (LONG)ptrOn->index : -1L);
    if (ptrOn == c || ncards == 1) {
        pointer_off();
        ptrOn = NULL;
    }
    if (!c->b->setMode(c->b, &m) || !pbm_upload(p)) {
        p->locks--;
        cs->curMode = NULL; cs->shown = NULL; cs->shownVP = NULL;
        ReleaseSemaphore(&lock);
        pbm_free(p); update_display();
        return PRISM_TEST_NOMODE;
    }
    cs->curMode = NULL;                     /* the next real screen sets its own */
    cs->shown = NULL;
    cs->shownVP = NULL;
    p->bd->setDisplayStart(p->bd, p->vramOff);
    cs->shownPbm = p;
    if (fmt == PF_CLUT8) {
        cs->testPal[3] = cs->testPal[4] = cs->testPal[5] = 255;
        for (i = 0; i < 128; i++)
            test_hue(i * 2, &cs->testPal[(2 + i) * 3], &cs->testPal[(2 + i) * 3 + 1],
                     &cs->testPal[(2 + i) * 3 + 2]);
        for (i = 0; i < 126; i++)
            cs->testPal[(130 + i) * 3] = cs->testPal[(130 + i) * 3 + 1] = cs->testPal[(130 + i) * 3 + 2] =
                i * 255 / 125;
        board_palette(c, 0, 256, cs->testPal);
    }
    if (!cs->rtgOn) {
        c->b->setSwitch(c->b, TRUE);
        cs->rtgOn = TRUE;
    }
    cs->testPbm = p;
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

/* version 2's testBegin: the first board */
static LONG test_begin(ULONG width, ULONG height, ULONG bits, ULONG hz, const char *label)
{
    return test_begin_on(0, width, height, bits, hz, label);
}

static void test_end(void)
{
    struct PBitMap *p[MAX_CARDS] = { NULL };
    BOOL any = FALSE;
    UBYTE k;

    LOCK();
    for (k = 0; k < ncards; k++) {
        struct CardState *cs = &cst[k];
        if ((p[k] = cs->testPbm)) {
            cs->testPbm = NULL;
            p[k]->locks--;
            if (cs->shownPbm == p[k])
                cs->shownPbm = NULL;
            cs->curMode = NULL;
            cs->shown = NULL;
            cs->shownVP = NULL;
            any = TRUE;
        }
    }
    ReleaseSemaphore(&lock);
    for (k = 0; k < ncards; k++)
        if (p[k])
            pbm_free(p[k]);
    if (any)
        update_display();                /* back to the front screens */
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
    BOOL was[MAX_CARDS];
    UBYTE k;
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
    for (k = 0; k < ncards; k++)
        if ((was[k] = cst[k].rtgOn))
            cards[k].b->setSwitch(cards[k].b, FALSE);
    __asm volatile ("jsr (%[f])" : "+r"(d0), "+r"(d1), "+r"(a0), "+r"(a1)
                    : "r"(_a6), [f]"a"(fn) : "cc", "memory");
    for (k = 0; k < ncards; k++)
        if (was[k])
            cards[k].b->setSwitch(cards[k].b, TRUE);
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
    {
        UBYTE k;
        for (k = 0; k < ncards; k++)
            if (cst[k].rtgOn) {
                cards[k].b->setSwitch(cards[k].b, FALSE);
                cst[k].rtgOn = FALSE;
                cst[k].curMode = NULL;
            }
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
    GFX_T(DrawGList,             0x072),
    GFX_T(RemIBob,               0x084),
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
    { (struct Library **)&GfxBase,       -0x24c, (APTR)P_ScrollVPort,  &o_ScrollVPort },
    { (struct Library **)&IntuitionBase, -0x048, (APTR)P_CloseWindow,  &o_CloseWindow },
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
    gels_init();
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

/* The second board is not used after all (no mode, or its first mode
 * failed): its monitor back to the Amiga's video, its driver stopped. */
static void drop_second(void)
{
    if (board2.setSwitch)
        board2.setSwitch(&board2, FALSE);
    driver_close_slot(1);
    ncards = 1;
    boardsSwapped = FALSE;
}

/* PRIMARY=SECOND: the two boards change places (their records and their
 * driver slots; the drivers keep no pointer to these records). */
static void swap_boards(void)
{
    struct PrismBoard t = board;
    board = board2;
    board2 = t;
    driver_swap_slots();
    boardsSwapped = !boardsSwapped;
}

/* Card k from its board: its modes, its first mode programmed now (with
 * the monitor still showing the Amiga: a cold Cirrus chip drops every CPU
 * write to VRAM until its linear window is set up, and screens are drawn
 * before they are shown), its text self-test, its display-database
 * monitor. FALSE (with the reason printed) if the board is no use. */
static BOOL init_card(UBYTE k)
{
    struct PrismBoard *B = cards[k].b;
    struct CardState *cs = &cst[k];
    struct PrismMode m;

    memset(cs, 0, sizeof(*cs));
    board_defaults(B);
    if (!prefs.blitter)
        B->flags |= PBF_SOFTWARE;        /* every blitter path checks it */
    build_modes(&cards[k]);
    if (!cs->nmodes) {
        printf("PrismD: %s offers no usable modes\n", B->name);
        return FALSE;
    }
    m = cs->modes[0].m;
    if (!B->setMode(B, &m)) {
        printf("PrismD: %s: initial mode failed\n", B->name);
        return FALSE;
    }
    if (prefs.blitter && B->modeReady) B->modeReady(B);
    selftest_text(&cards[k]);
    /* the board shows the first mode for a moment before the switch goes
     * back to the Amiga: Amiberry 8.4's ZZ9000 corrupts its heap and dies
     * when the RTG display is switched on and off inside one frame (seen
     * at every boot, 2026-10-12); two frames cost nothing on a real card */
    Delay(2);
    B->setSwitch(B, FALSE);
    cs->curMode = &cs->modes[0];
    /* A copy of the system's monitor, but with a list and a semaphore of
     * its own: copied, they would point into graphics' own MonitorSpec. */
    CopyMem(GfxBase->default_monitor, &cs->monitor, sizeof(cs->monitor));
    cs->monitor.ms_Node.xln_Name = monName[k];
    cs->monitor.ms_OpenCount = 0;
    {
        struct List *l = &cs->monitor.DisplayInfoDataBase;
        l->lh_Head = (struct Node *)&l->lh_Tail;
        l->lh_Tail = NULL;
        l->lh_TailPred = (struct Node *)&l->lh_Head;
    }
    InitSemaphore(&cs->monitor.DisplayInfoDataBaseSemaphore);
    return TRUE;
}

/* ---- the second monitor (SECONDMONITOR=, docs/multi-board.md) --------
 *
 * SCREEN: a public screen of PrismD's own, PRISM2, on the second board and
 * behind every other screen. A board shows its own front-most screen, so
 * the second monitor shows it - instead of the Amiga's video, which with
 * Workbench on the primary is an empty, black picture - and windows and
 * programs can be put there. Opened once a screen is up in front (so it
 * never comes up ahead of Workbench), in the second board's 256-colour mode
 * of the primary's size where it has one; closed when PrismD quits. */
static struct Screen *secScreen;
static ULONG secTried;                    /* VBCounter of the last attempt */
static BOOL secStop;                      /* quitting: not opened again    */
static const UWORD secPens[] = { (UWORD)~0 };

static ULONG second_mode(void)
{
    const struct CardState *a = &cst[0], *b = &cst[1];
    ULONG i, first = 0;
    for (i = 0; i < b->nmodes; i++) {
        const struct ModeRec *mr = &b->modes[i];
        if (mr->bpp != 1)
            continue;
        if (!first)
            first = mr->id;
        if (a->curMode && mr->m.width == a->curMode->m.width && mr->m.height == a->curMode->m.height)
            return mr->id;
    }
    return first ? first : b->nmodes ? b->modes[0].id : 0;
}

static void second_tick(void)
{
    ULONG id;
    if (ncards < 2 || prefs.secondMon != SM_SCREEN || secScreen || secStop || !IntuitionBase->FirstScreen)
        return;
    if (secTried && GfxBase->VBCounter - secTried < 250)
        return;                           /* a failed attempt: again in ~5 s */
    secTried = GfxBase->VBCounter | 1;
    if (!(id = second_mode()))
        return;
    secScreen = OpenScreenTags(NULL, SA_DisplayID, id, SA_Depth, 8, SA_Behind, TRUE,
                               SA_Title, (ULONG)"PRISM2 - second monitor", SA_PubName, (ULONG)"PRISM2",
                               SA_Pens, (ULONG)secPens, SA_SharePens, TRUE, SA_SysFont, 1, TAG_DONE);
    dbg("second monitor: PRISM2 on $%08lx %s\n", id, secScreen ? "open" : "FAILED");
    if (secScreen)
        PubScreenStatus(secScreen, 0);
}

/* quitting: PRISM2 closed; FALSE while windows are still on it */
static BOOL second_close(void)
{
    secStop = TRUE;
    if (!secScreen)
        return TRUE;
    if (!(PubScreenStatus(secScreen, PSNF_PRIVATE) & PSNF_PRIVATE) || !CloseScreen(secScreen)) {
        PubScreenStatus(secScreen, 0);
        return FALSE;
    }
    secScreen = NULL;
    return TRUE;
}

/* MIRROR: the second board shows a copy of what the first one shows, in its
 * mode of the same size and depth (no such mode: nothing is mirrored). The
 * first board's picture is never read back to find what changed: drawing
 * into the bitmap on display marks a rectangle (SW_CLEAR, LOCK_FOR in
 * prismint.h), and every tick copies up to MON2_BUDGET bytes of it across,
 * converting the pixels where the two boards store them differently. A
 * 256-colour copy gets the first board's palette. Not copied yet: the
 * pointer, and what the compositor puts together (screen dragging, PIPs). */
#define MON2_BUDGET 65536UL
struct PBitMap *mirSrc;
static struct PBitMap *mon2Dst;
static UWORD mon2X;                      /* SPAN: where in the source board 1's half starts */
static ULONG mon2Gen;                    /* the paletteGen its palette is from */

/* the marked rectangles. Not one rectangle around everything:
 * a window frame dragged in outline is eight thin lines a move (the old
 * frame off, the new one on), and the box around them is the whole window */
#define MON2_RECTS 16
static struct DRect { WORD x0, y0, x1, y1; } dr[MON2_RECTS];
static UWORD ndr;

static LONG dr_area(WORD x0, WORD y0, WORD x1, WORD y1)
{
    return (LONG)(x1 - x0 + 1) * (y1 - y0 + 1);
}

/* lock held or Forbid: kept short */
void mon2_mark(WORD x0, WORD y0, WORD x1, WORD y1)
{
    LONG a = dr_area(x0, y0, x1, y1), bestWaste = 0x7fffffffL;
    UWORD i, best = 0;

    for (i = 0; i < ndr; ) {
        struct DRect *r = &dr[i];
        WORD ux0, uy0, ux1, uy1;
        LONG waste;
        if (x0 >= r->x0 && y0 >= r->y0 && x1 <= r->x1 && y1 <= r->y1)
            return;                       /* already marked */
        if (x0 <= r->x0 && y0 <= r->y0 && x1 >= r->x1 && y1 >= r->y1) {
            dr[i] = dr[--ndr];            /* the new one covers it */
            continue;
        }
        ux0 = x0 < r->x0 ? x0 : r->x0; uy0 = y0 < r->y0 ? y0 : r->y0;
        ux1 = x1 > r->x1 ? x1 : r->x1; uy1 = y1 > r->y1 ? y1 : r->y1;
        waste = dr_area(ux0, uy0, ux1, uy1) - dr_area(r->x0, r->y0, r->x1, r->y1) - a;
        if (waste < bestWaste) {
            bestWaste = waste;
            best = i;
        }
        i++;
    }
    /* joined to another when that copies little more (pieces of one text
     * line, a box and its border), or when there is no room left */
    if (ndr && (bestWaste <= 1024 || ndr == MON2_RECTS)) {
        struct DRect *r = &dr[best];
        if (x0 < r->x0) r->x0 = x0;
        if (y0 < r->y0) r->y0 = y0;
        if (x1 > r->x1) r->x1 = x1;
        if (y1 > r->y1) r->y1 = y1;
        return;
    }
    dr[ndr].x0 = x0; dr[ndr].y0 = y0; dr[ndr].x1 = x1; dr[ndr].y1 = y1;
    ndr++;
}

/* lock held */
static void mon2_stop(void)
{
    struct CardState *cs = &cst[1];
    if (!mon2Dst)
        return;
    dbg("second monitor: mirror stops\n");
    mirSrc = NULL;
    mon2Dst->locks--;
    if (cs->shownPbm == mon2Dst)
        cs->shownPbm = NULL;
    pbm_free(mon2Dst);
    mon2Dst = NULL;
    cs->curMode = NULL;
}

/* start mirroring board 0's picture on board 1, or go on with it; TRUE while
 * it is mirrored (lock held) */
/* SPAN: the span screen board 1 shows the other half of - the one board 0
 * shows, else the frontmost span screen behind whatever is in front there
 * (a test picture, a game's own screen, a chipset screen), so the second
 * monitor keeps its half of Workbench instead of going black. NULL: none. */
static struct PScreen *span_source(void)
{
    struct Screen *s;
    if (prefs.secondMon != SM_SPAN)
        return NULL;
    if (cst[0].shown && cst[0].shown->mode->spanHalf)
        return cst[0].shown;
    for (s = IntuitionBase->FirstScreen; s; s = s->NextScreen) {
        struct PScreen *ps = ps_by_screen(s);
        if (ps && ps->pbm && ps->mode->card->index == 0 && ps->mode->spanHalf)
            return ps;
    }
    return NULL;
}

/* what board 1 copies from: board 0's shown bitmap, or a span screen behind it */
static struct PBitMap *mon2_source(struct ModeRec **mode)
{
    struct PScreen *sp = span_source();
    if (sp && sp != cst[0].shown) {
        if (mode) *mode = sp->mode;
        return sp->front ? sp->front : sp->pbm;
    }
    if (mode) *mode = cst[0].curMode;
    return cst[0].shownPbm;
}

static BOOL mon2_show(void)
{
    struct CardState *cs = &cst[1];
    struct PrismBoard *B = cards[1].b;
    struct ModeRec *am = NULL;
    struct PBitMap *src = mon2_source(&am), *p;
    struct ModeRec *best = NULL;
    struct PrismMode m;
    ULONG i;
    UWORD w, x;

    if (!src || !am || cs->testPbm || (B->flags & (PBF_SHADOW | PBF_PRESENT))) {
        mon2_stop();
        return FALSE;
    }
    /* a span screen: board 1 shows the half board 0 doesn't, in its mode
     * of half the width */
    w = am->spanHalf ? am->spanHalf : am->m.width;
    x = am->spanHalf && !prefs.secondLeft ? am->spanHalf : 0;
    if (mon2Dst && mirSrc == src && mon2X == x && cs->curMode && cs->curMode->m.width == w &&
        cs->curMode->m.height == am->m.height)
        return TRUE;
    mon2_stop();
    mon2X = x;
    for (i = 0; i < cs->nmodes; i++) {
        struct ModeRec *mr = &cs->modes[i];
        if (mr->m.width == w && mr->m.height == am->m.height && mr->bpp == src->bpp) {
            best = mr;
            break;
        }
    }
    if (!best) {
        dbg("second monitor: no %ux%u mode at %u bytes a pixel to mirror into\n",
            w, am->m.height, src->bpp);
        return FALSE;
    }
    present_stop(&cards[1]);
    m = best->m;
    m.pitchPixels = 0;
    if (!pbm_prepare_mode(&cards[1]) || !B->setMode(B, &m))
        return FALSE;
    cs->curMode = best;
    cs->curPitch = 0;
    cs->shown = NULL;
    cs->shownVP = NULL;
    p = pbm_new(&cards[1], best->m.width, best->m.height, 8, best->m.format, NULL, TRUE, TRUE);
    if (!p || !p->inVram || !show_pbm(p)) {
        if (p)
            pbm_free(p);
        cs->curMode = NULL;
        return FALSE;
    }
    p->locks++;                           /* VRAM paging leaves it where it is */
    mon2Dst = p;
    mirSrc = src;
    mon2_mark(0, 0, 32767, 32767);
    mon2Gen = paletteGen - 1;             /* the palette at the next tick */
    if (!cs->rtgOn) {
        B->setSwitch(B, TRUE);
        cs->rtgOn = TRUE;
    }
    dbg("second monitor: mirror %ux%u, %u bytes a pixel, formats %u -> %u\n",
        p->w, p->h, p->bpp, src->fmt, p->fmt);
    return TRUE;
}

/* copy rows y..y1, x0..x1 of board 1's half while the budget lasts; the
 * first row not copied */
static UWORD mon2_rows(struct PBitMap *s, struct PBitMap *d, UWORD x0, UWORD x1, UWORD y, UWORD y1,
                       BOOL swap16, ULONG *budget)
{
    ULONG rowBytes = (ULONG)(x1 - x0 + 1) * s->bpp;

    for (; y <= y1 && *budget; y++) {
        const UBYTE *sp = s->pix + (ULONG)y * s->bpr + (ULONG)(x0 + mon2X) * s->bpp;
        UBYTE *dp = d->pix + (ULONG)y * d->bpr + (ULONG)x0 * d->bpp;
        if (s->fmt == d->fmt) {
            CopyMem((APTR)sp, dp, rowBytes);
        } else if (swap16) {
            UWORD n = x1 - x0 + 1;
            if (!((ULONG)sp & 2) && !((ULONG)dp & 2)) {       /* two pixels a longword */
                const ULONG *sl = (const ULONG *)sp;
                ULONG *dl = (ULONG *)dp;
                for (; n >= 2; n -= 2) {
                    ULONG v = *sl++;
                    *dl++ = ((v >> 8) & 0x00ff00ffUL) | ((v << 8) & 0xff00ff00UL);
                }
                sp = (const UBYTE *)sl;
                dp = (UBYTE *)dl;
            }
            for (; n; n--, sp += 2, dp += 2) {
                UWORD v = *(const UWORD *)sp;
                *(UWORD *)dp = (UWORD)((v >> 8) | (v << 8));
            }
        } else {
            UWORD x;
            for (x = x0; x <= x1; x++, sp += s->bpp, dp += d->bpp)
                pf_put(d->fmt, pf_get(s->fmt, sp), dp);
        }
        *budget = *budget > rowBytes ? *budget - rowBytes : 0;
    }
    return y;
}

/* the main loop, every tick: copy what was drawn (lock taken here) */
static void mon2_tick(void)
{
    struct PBitMap *s, *d;
    UWORD x0, x1, y, y1, w, h;
    ULONG budget = MON2_BUDGET;
    BOOL swap16;

    if (!mon2Dst)
        return;
    LOCK_QUIET();
    if (mirSrc != mon2_source(NULL)) {    /* another bitmap to copy (double buffering, a span screen to front) */
        ReleaseSemaphore(&lock);
        update_display();
        return;
    }
    s = mirSrc;
    d = mon2Dst;
    if (d->fmt == PF_CLUT8 && mon2Gen != paletteGen) {
        UBYTE rgb[256 * 3];
        for (y = 0; y < 256; y++) {
            ULONG c = s->rgbTab ? s->rgbTab[y] : y * 0x010101UL;
            rgb[y * 3] = c >> 16; rgb[y * 3 + 1] = c >> 8; rgb[y * 3 + 2] = c;
        }
        board_palette(&cards[1], 0, 256, rgb);
        mon2Gen = paletteGen;
    }
    if (!ndr) {
        ReleaseSemaphore(&lock);
        return;
    }
    if (cards[0].b->waitBlit) cards[0].b->waitBlit(cards[0].b);
    if (cards[1].b->waitBlit) cards[1].b->waitBlit(cards[1].b);
    /* the same pixels with their two bytes exchanged (a ZZ9000's 565 big-
     * endian and a Cirrus's 565 little-endian): pf_get/pf_put per pixel
     * took about a second for a whole 800x600 half */
    swap16 = s->bpp == 2 && d->bpp == 2 &&
        ((s->fmt == PF_RGB565BE && d->fmt == PF_RGB565LE) || (s->fmt == PF_RGB565LE && d->fmt == PF_RGB565BE) ||
         (s->fmt == PF_RGB555BE && d->fmt == PF_RGB555LE) || (s->fmt == PF_RGB555LE && d->fmt == PF_RGB555BE));
    /* the marked rectangles are in the source's coordinates; board 1's half
     * starts mon2X into it (0 but for a span screen) */
    w = s->w - mon2X < d->w ? s->w - mon2X : d->w;
    h = s->h < d->h ? s->h : d->h;
    while (ndr && budget) {
        struct DRect *r = &dr[0];
        if (r->x1 >= (WORD)mon2X && r->x0 - (WORD)mon2X < (WORD)w && r->y1 >= 0 && r->y0 < (WORD)h) {
            x0 = r->x0 - (WORD)mon2X < 0 ? 0 : r->x0 - mon2X;
            x1 = r->x1 - (WORD)mon2X >= (WORD)w ? w - 1 : r->x1 - mon2X;
            y = r->y0 < 0 ? 0 : r->y0;
            y1 = r->y1 >= (WORD)h ? h - 1 : r->y1;
            y = mon2_rows(s, d, x0, x1, y, y1, swap16, &budget);
            if (y <= y1) {
                r->y0 = y;                /* the rest at the next tick */
                break;
            }
        }                                 /* else all of it on board 0's half */
        dr[0] = dr[--ndr];
    }
    ReleaseSemaphore(&lock);
}

/* ---- main ----------------------------------------------------------- */

#define TEMPLATE "BOARD/K,PREFS/K,LOG/S,P96CARD/K,P96MONITOR/K"

/* ---- monitor power saving (POWERSAVE in PrismPrefs) -------------------
 *
 * After POWERSAVE minutes without a key or mouse event, every board showing
 * an RTG picture goes dark: through its driver's setPower (the Cirrus
 * boards: the picture off, and on the 5426/5428 both syncs off, so that the
 * monitor sleeps) or, without one, by PrismD - a black palette on a
 * 256-colour screen, else a cleared bitmap in VRAM on display. Any input
 * brings the picture back, and so does a mode change or the monitor switch.
 * While dark the board's display start and palette calls only take note:
 * a program flipping its buffers or cycling colours keeps it dark. */
static volatile ULONG inputSeen;        /* key and mouse events, from the handler */
static struct Interrupt idleInt;
static struct MsgPort *idlePort;
static struct IOStdReq *idleReq;
static ULONG idleSecs, idleCount, powerPoll;
static BOOL dark;
static struct PowerState {
    BOOL active, hw, wanted;
    ULONG want;                          /* the display start asked for while dark */
    struct PBitMap *black;
    void (*start)(struct PrismBoard *, ULONG);
    BOOL (*mode)(struct PrismBoard *, struct PrismMode *);
    void (*palette)(struct PrismBoard *, UWORD, UWORD, const UBYTE *);
    void (*sw)(struct PrismBoard *, BOOL);
    void (*cursor)(struct PrismBoard *, BOOL);
} pw[MAX_CARDS];
static void power_wake(void);

#define EDGE_PUSH 24                    /* mouse counts past the edge before it switches */
static WORD edgePush;

static struct InputEvent *idle_handler(struct InputEvent *ev __asm("a0"), APTR data __asm("a1"))
{
    struct InputEvent *e;
    BOOL seen = FALSE;
    (void)data;
    for (e = ev; e; e = e->ie_NextEvent) {
        if (e->ie_Class == IECLASS_RAWKEY || e->ie_Class == IECLASS_RAWMOUSE ||
            e->ie_Class == IECLASS_POINTERPOS || e->ie_Class == IECLASS_NEWPOINTERPOS)
            seen = TRUE;
        /* edge switching: a push off the edge facing the other monitor, no
         * button held (a window or icon being dragged stays where it is) */
        if (e->ie_Class == IECLASS_RAWMOUSE && ncards == 2 && edgeFront >= 0 && !edgeGo &&
            !(e->ie_Qualifier & (IEQUALIFIER_LEFTBUTTON | IEQUALIFIER_RBUTTON | IEQUALIFIER_MIDBUTTON))) {
            BYTE other = 1 - edgeFront;
            BOOL right = (edgeFront == 0) != (prefs.secondLeft != 0);   /* the other monitor is right of this one */
            WORD dx = e->ie_X;
            BOOL edge = right ? IntuitionBase->MouseX >= (WORD)edgeW - 1 : IntuitionBase->MouseX <= 0;
            if (edge && edgeHas[other] && (right ? dx > 0 : dx < 0)) {
                edgePush += dx > 0 ? dx : -dx;
                if (edgePush >= EDGE_PUSH) {
                    WORD y = IntuitionBase->MouseY;
                    edgeYFrac = edgeH > 1 && y > 0 ? (UWORD)((ULONG)(y < (WORD)edgeH ? y : edgeH - 1) * 65535UL / (edgeH - 1)) : 0;
                    edgeGo = other + 1;
                    edgePush = 0;
                }
            } else if (dx)
                edgePush = 0;
        }
    }
    if (seen)
        inputSeen++;
    return ev;
}

/* the main loop, every tick: a switch the handler asked for */
static void edge_tick(void)
{
    BYTE t = edgeGo;
    struct Screen *s, *target = NULL;
    if (!t)
        return;
    t--;
    LOCK_QUIET();
    for (s = IntuitionBase->FirstScreen; s; s = s->NextScreen) {
        struct PScreen *ps = ps_by_screen(s);
        if (ps && ps->mode->card->index == t) {
            target = s;
            break;
        }
    }
    ReleaseSemaphore(&lock);
    if (target && idleReq) {
        /* arriving from the left: on its left edge, else its right */
        BOOL atLeft = (t == 1) != (prefs.secondLeft != 0);
        struct IEPointerPixel pp;
        struct InputEvent ie;
        ScreenToFront(target);
        memset(&ie, 0, sizeof(ie));
        pp.iepp_Screen = target;
        pp.iepp_Position.X = atLeft ? 0 : target->Width - 1;
        pp.iepp_Position.Y = (WORD)((ULONG)edgeYFrac * (target->Height - 1) / 65535UL);
        ie.ie_Class = IECLASS_NEWPOINTERPOS;
        ie.ie_SubClass = IESUBCLASS_PIXEL;
        ie.ie_EventAddress = (APTR)&pp;
        idleReq->io_Command = IND_WRITEEVENT;
        idleReq->io_Data = &ie;
        idleReq->io_Length = sizeof(ie);
        DoIO((struct IORequest *)idleReq);
        dbg("edge: to card %ld, screen %lx at %ld,%ld\n", (LONG)t, (ULONG)target,
            (LONG)pp.iepp_Position.X, (LONG)pp.iepp_Position.Y);
    }
    edgeGo = 0;
}

static int pw_card(struct PrismBoard *b)
{
    UBYTE k;
    for (k = 0; k < ncards; k++)
        if (cards[k].b == b)
            return k;
    return 0;
}

/* the board's calls while it is dark */
static void dark_start(struct PrismBoard *b, ULONG off)
{
    struct PowerState *s = &pw[pw_card(b)];
    s->want = off;
    s->wanted = TRUE;
}
static void dark_palette(struct PrismBoard *b, UWORD first, UWORD count, const UBYTE *rgb)
{
    (void)b; (void)first; (void)count; (void)rgb;   /* push_palette reloads all on waking */
}
static void dark_cursor(struct PrismBoard *b, BOOL on)
{
    (void)b; (void)on;                  /* pointer_on shows it again on waking */
}
static BOOL dark_mode(struct PrismBoard *b, struct PrismMode *m)
{
    power_wake();
    return b->setMode(b, m);
}
static void dark_switch(struct PrismBoard *b, BOOL rtg)
{
    power_wake();
    b->setSwitch(b, rtg);
}

/* every board showing a picture goes dark (lock held) */
static void power_dark(void)
{
    static const UBYTE black[256 * 3];
    UBYTE k;
    for (k = 0; k < ncards; k++) {
        struct CardState *cs = &cst[k];
        struct PrismBoard *B = cards[k].b;
        struct PowerState *s = &pw[k];
        memset(s, 0, sizeof(*s));
        if (!cs->rtgOn || !cs->curMode || !cs->shownPbm || cs->testPbm)
            continue;
        if (B->waitBlit)
            B->waitBlit(B);
        if (B->setPower) {
            B->setPower(B, 4);
            s->hw = TRUE;
        } else if (cs->shownPbm->bpp == 1) {
            B->setPalette(B, 0, 256, black);
        } else if (!(B->flags & (PBF_SHADOW | PBF_PRESENT))) {
            struct PBitMap *p = pbm_new(&cards[k], cs->shownPbm->w, cs->curMode->m.height,
                                        cs->shownPbm->depth, cs->shownPbm->fmt, NULL, TRUE, TRUE);
            if (!p || !p->inVram || p->bpr != cs->shownPbm->bpr) {
                dbg("power: no black bitmap for card %ld - left on\n", (LONG)k);
                if (p)
                    pbm_free(p);
                continue;
            }
            p->locks++;                      /* VRAM paging leaves it where it is */
            s->black = p;
            B->setDisplayStart(B, p->vramOff);
        } else
            continue;                         /* a 16/32-bit shadow board: left on */
        if ((B->flags & PBF_HW_CURSOR) && B->cursorShow)
            B->cursorShow(B, FALSE);
        s->start = B->setDisplayStart;   B->setDisplayStart = dark_start;
        s->palette = B->setPalette;      B->setPalette = dark_palette;
        s->mode = B->setMode;            B->setMode = dark_mode;
        s->sw = B->setSwitch;            B->setSwitch = dark_switch;
        s->cursor = B->cursorShow;
        if (B->cursorShow)
            B->cursorShow = dark_cursor;
        s->active = TRUE;
        dbg("power: card %ld dark (%s)\n", (LONG)k, s->hw ? "board" : s->black ? "black bitmap" : "black palette");
    }
    dark = TRUE;                             /* until the next input, even if no board could */
}

/* the pictures back (lock held) */
static void power_wake(void)
{
    ULONG secs, micros;
    UBYTE k;
    if (!dark)
        return;
    dark = FALSE;
    /* a full POWERSAVE period from now, whatever woke it (an input, or a
     * program's mode change: no going dark again a second later) */
    CurrentTime(&secs, &micros);
    idleSecs = secs;
    for (k = 0; k < ncards; k++) {
        struct CardState *cs = &cst[k];
        struct PrismBoard *B = cards[k].b;
        struct PowerState *s = &pw[k];
        if (!s->active)
            continue;
        s->active = FALSE;
        B->setDisplayStart = s->start;
        B->setPalette = s->palette;
        B->setMode = s->mode;
        B->setSwitch = s->sw;
        B->cursorShow = s->cursor;
        if (s->hw)
            B->setPower(B, 0);
        if (s->wanted)
            B->setDisplayStart(B, s->want);
        else if (s->black && cs->shownPbm && cs->shownPbm->inVram)
            B->setDisplayStart(B, scrolled_start(cs->shownPbm));
        if (s->black) {
            s->black->locks--;
            pbm_free(s->black);
            s->black = NULL;
        }
        if (cs->shownVP)
            push_palette(cs->shownVP);
        if (ptrOn == &cards[k] && cs->shownVP)
            pointer_on(cs->shownVP, &cards[k]);
        dbg("power: card %ld back on\n", (LONG)k);
    }
}

static void power_init(void)
{
    if (!(idlePort = CreateMsgPort()))
        return;
    if (!(idleReq = (struct IOStdReq *)CreateIORequest(idlePort, sizeof(*idleReq)))) {
        DeleteMsgPort(idlePort);
        idlePort = NULL;
        return;
    }
    if (OpenDevice((STRPTR)"input.device", 0, (struct IORequest *)idleReq, 0)) {
        DeleteIORequest((struct IORequest *)idleReq);
        DeleteMsgPort(idlePort);
        idleReq = NULL;
        idlePort = NULL;
        return;
    }
    idleInt.is_Node.ln_Type = NT_INTERRUPT;
    idleInt.is_Node.ln_Pri = 100;            /* ahead of Intuition (50) */
    idleInt.is_Node.ln_Name = (char *)"PrismD power saving";
    idleInt.is_Code = (void (*)(void))idle_handler;
    idleReq->io_Command = IND_ADDHANDLER;
    idleReq->io_Data = &idleInt;
    DoIO((struct IORequest *)idleReq);
}

static void power_quit(void)
{
    LOCK_QUIET();
    power_wake();
    ReleaseSemaphore(&lock);
    if (!idleReq)
        return;
    idleReq->io_Command = IND_REMHANDLER;
    idleReq->io_Data = &idleInt;
    DoIO((struct IORequest *)idleReq);
    CloseDevice((struct IORequest *)idleReq);
    DeleteIORequest((struct IORequest *)idleReq);
    DeleteMsgPort(idlePort);
    idleReq = NULL;
}

/* the main loop, every tick */
static void power_tick(void)
{
    ULONG secs, micros;
    if (inputSeen != idleCount) {
        idleCount = inputSeen;
        CurrentTime(&secs, &micros);
        idleSecs = secs;
        if (dark) {
            LOCK_QUIET();
            power_wake();
            ReleaseSemaphore(&lock);
        }
        return;
    }
    if (dark || !prefs.powerSave || !idleReq || ++powerPoll % 50)
        return;
    CurrentTime(&secs, &micros);
    if (!idleSecs || secs < idleSecs)        /* the first look, or the clock was set back */
        idleSecs = secs;
    if (secs - idleSecs >= (ULONG)prefs.powerSave * 60) {
        LOCK();
        power_dark();
        ReleaseSemaphore(&lock);
    }
}

int main(void)
{
    LONG args[5] = { 0 };
    static const char *const pbName[PB_COUNT] = { NULL, "PICASSO2", "ZZ9000", "P96", "UAEGFX", NULL };
    struct RDArgs *rda;
    const char *want;
    BOOL found = FALSE;
    ULONG i;
    UBYTE k;
    int rc = 20;

    /* the linker's --gc-sections drops an unreferenced $VER string (beta 3
     * shipped without one): keep it reachable */
    __asm__ volatile("" : : "r"(version));
    if (!(rda = ReadArgs(TEMPLATE, args, NULL))) {
        PrintFault(IoErr(), "PrismD");
        return 20;
    }
    /* Kickstart 3.0's graphics drives the display differently (its MrgCop
     * and LoadView call into the monitor), and PrismRTG crashes it there. */
    GfxBase = (struct GfxBase *)OpenLibrary("graphics.library", 40);
    IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 40);
    UtilityBase = OpenLibrary("utility.library", 39);
    ExpansionBase = (struct ExpansionBase *)OpenLibrary("expansion.library", 37);
    if (!GfxBase || !IntuitionBase || !UtilityBase || !ExpansionBase) {
        printf("PrismD: needs Kickstart 3.1 or newer\n");
        goto out;
    }
    if (FindSemaphore(PRISM_SEMNAME)) {
        printf("PrismD: already running\n");
        rc = 5;
        goto out_other;
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
    gamma_init();
    /* LOG=SYNC is for a machine that dies during boot, where PrismD is
     * started with no output: the log goes to SYS:PrismD.log (dbg_flush) */
    if (prefs.log && !dbgRing)
        dbgRing = AllocVec(DBG_RING, MEMF_PUBLIC | MEMF_CLEAR);
    if (prefs.log == 2)
    {
        dbg("PrismD %s starting\n", __DATE__ " " __TIME__);
        dbg_flush();
    }
    if (args[2])
        prefs.log = 1;
    if (prefs.log && !dbgRing && !(dbgRing = AllocVec(DBG_RING, MEMF_PUBLIC | MEMF_CLEAR)))
        prefs.log = 0;
    dbgOn = prefs.log != 0;
    want = args[0] ? (const char *)args[0] :
           prefs.board == PB_OTHER && prefs.boardName[0] ? prefs.boardName :
           pbName[prefs.board < PB_COUNT ? prefs.board : 0];
    {
        struct PrismDriverConfig config;
        const char *card=args[3] ? (const char *)args[3] : prefs.p96card;
        const char *monitor=args[4] ? (const char *)args[4] : prefs.p96monitor;
        memset(&config,0,sizeof(config));
        config.clutBGR=prefs.clutBGR;
        if(strlen(card)>=sizeof(config.card) || strlen(monitor)>=sizeof(config.monitor)) {
            puts("PrismD: driver path too long");goto out;
        }
        strcpy(config.card,card);strcpy(config.monitor,monitor);
        if(want) found=driver_open(&board,want,&config);
        else {
            const char *first=NULL,*second=NULL;
            found=driver_open(&board,"PICASSO2",&config);
            if(found) { first="PICASSO2"; second="ZZ9000"; }
            else if((found=driver_open(&board,"ZZ9000",&config))) { first="ZZ9000"; second="PICASSO2"; }
            /* then any other driver installed (a new board's): the P96
             * and UAE adapters stay explicit choices, they claim contexts */
            if(!found) found=driver_scan(&board,&config);
            /* nothing of our own: the adapters, last - UAE's RTG card (an
             * emulator, Caffeine OS in WinUAE: Auto found nothing there and
             * PrismRTG stayed off), then a card through its Picasso96 driver
             * when the settings name one (P96CARD=, e.g. a PiStorm's
             * VideoCore or a Vampire) */
            if(!found) {
                struct Library *uae;
                driverQuiet=TRUE;
                /* only where the emulator's card is: the UAEGFX driver
                 * says why it can't run, on every Amiga without one */
                Forbid();
                uae=(struct Library *)FindName(&SysBase->LibList,"uaegfx.card");
                Permit();
                if(uae) found=driver_open(&board,"UAEGFX",&config);
                if(!found && config.card[0]) found=driver_open(&board,"P96",&config);
                driverQuiet=FALSE;
            }
            /* Auto drives a second board too (multi-board.md): one the
             * other supplied driver finds, else a second of the first one's
             * kind (a second copy of its driver; drivers skip boards
             * another copy has claimed). Its screens get their own monitor,
             * PRISM2:. The first board is as before. */
            if(found && second && prefs.second) {
                config.taken[0]=board.configDev;   /* the second probe leaves it alone */
                driverQuiet=TRUE;            /* no board there is the usual case */
                if(driver_open(&board2,second,&config) || driver_open(&board2,first,&config))
                    ncards=2;
                driverQuiet=FALSE;
                /* PRIMARY=SECOND: the second board found is the primary -
                 * it gets the PRISM: modes and so Workbench (a bigger
                 * monitor on it, say). The drivers keep no pointer to
                 * these records: swapping them, and their driver slots,
                 * swaps the boards. */
                if(ncards==2 && prefs.swapBoards)
                    swap_boards();
            }
        }
    }
    if (!found) {
        printf("PrismD: no supported board\n");
        rc = 5;
        goto out;
    }
    InitSemaphore(&lock);
    if (!init_card(0)) {
        if (!boardsSwapped)
            goto out;
        /* the board chosen as the primary is no use: the other one alone,
         * as it would have been without PRIMARY=SECOND */
        printf("PrismD: %s cannot be the primary; %s is used alone\n", board.name, board2.name);
        swap_boards();
        drop_second();
        if (!init_card(0))
            goto out;
    }
    if (ncards > 1 && !init_card(1)) {
        printf("PrismD: only %s is used\n", board.name);
        drop_second();
    }
    if (ncards > 1 && prefs.secondMon == SM_SPAN)
        build_span_modes();

    STAGE("board found, modes built");
    for (k = 0; k < ncards; k++) {
        struct CardState *cs = &cst[k];
        printf("PrismD: %s%s, %lu KB VRAM, modes:\n", k ? "second board: " : "", cards[k].b->name,
               (unsigned long)(cards[k].b->vramSize >> 10));
        for (i = 0; i < cs->nmodes; i++)
            if (cs->modes[i].m.refresh)
                printf("  $%08lx %s %u Hz\n", (unsigned long)cs->modes[i].id, cs->modes[i].name,
                       cs->modes[i].m.refresh);
            else            /* the card's driver times it and doesn't say how */
                printf("  $%08lx %s\n", (unsigned long)cs->modes[i].id, cs->modes[i].name);
    }
    if (!prefs.blitter)
        printf("PrismD: blitter off (PrismPrefs)\n");

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
        printf("PrismD: cybergraphics.library 43 added\n");
    if (cgxvideo_init())
        printf("PrismD: cgxvideo.library 42 added (video layers, composed in software)\n");
    if (chipTop != 0xffffffffUL && p96_init())
        printf("PrismD: Picasso96API.library 2 added (Prism's own)\n");
    power_init();                     /* POWERSAVE: the input handler */
    if (arexx_init())
        printf("PrismD: ARexx port PRISM open\n");

    memset(&sem, 0, sizeof(sem));
    sem.ss.ss_Link.ln_Name = PRISM_SEMNAME;
    sem.ss.ss_Link.ln_Pri = 0;
    sem.version = PRISM_VERSION;
    sem.release = PRISM_RELEASE;
    sem.releaseDate = PRISM_RELDATE;
    sem.powerSave = &prefs.powerSave;
    sem.boardName = board.name;
    sem.task = FindTask(NULL);
    sem.numModes = prism_mode_count();
    sem.screenInfo = screen_info;
    sem.testBegin = test_begin;
    sem.testEnd = test_end;
    sem.drawLock = &lock;
    sem.logLevel = &prefs.log;
    sem.boardFlags = &board.flags;
    sem.numBoards = ncards;
    if (ncards > 1) {
        sem.board2Name = board2.name;
        sem.board2Flags = &board2.flags;
        sem.board2HwOps = (board2.fillRect || BOARD_CAN(&board2, fill) ? PRISM_HW_FILL : 0) |
                          (board2.copyRect || BOARD_CAN(&board2, copy) ? PRISM_HW_COPY : 0) |
                          (board2.copyBetween ? PRISM_HW_BETWEEN : 0) |
                          (board2.expandRect || BOARD_CAN(&board2, expand) ? PRISM_HW_TEXT : 0) |
                          (board2.drawLine || BOARD_CAN(&board2, line) ? PRISM_HW_LINE : 0) |
                          (BOARD_CAN(&board2, planar) ? PRISM_HW_PLANAR : 0);
    }
    sem.boardsSwapped = boardsSwapped;
    sem.boardAddr[0] = board.configDev ? (ULONG)board.configDev->cd_BoardAddr : 0;
    sem.boardAddr[1] = ncards > 1 && board2.configDev ? (ULONG)board2.configDev->cd_BoardAddr : 0;
    sem.vramKB[0] = board.vramSize >> 10;
    sem.vramKB[1] = ncards > 1 ? board2.vramSize >> 10 : 0;
    sem.testBeginOn = test_begin_on;
    sem.hwOps = (board.fillRect || BOARD_CAN(&board, fill) ? PRISM_HW_FILL : 0) |
                (board.copyRect || BOARD_CAN(&board, copy) ? PRISM_HW_COPY : 0) |
                (board.copyBetween ? PRISM_HW_BETWEEN : 0) |
                (board.expandRect || BOARD_CAN(&board, expand) ? PRISM_HW_TEXT : 0) |
                (board.drawLine || BOARD_CAN(&board, line) ? PRISM_HW_LINE : 0) |
                (BOARD_CAN(&board, planar) ? PRISM_HW_PLANAR : 0);
    AddSemaphore(&sem.ss);
#ifdef PRISM_ABI_PROBE
    printf("PrismD: ABI probe: board %lu bytes here, the driver's ops table %lu; the later operation is %s\n",
           (unsigned long)sizeof(struct PrismBoard), (unsigned long)board.opsSize,
           BOARD_CAN(&board, future) ? "CALLABLE (wrong)" : "not called (right)");
#endif
    printf("PrismD: running (Ctrl-C to quit)\n");
    STAGE("running");

    for (;;) {
        ULONG tick = 0;
        static ULONG vb0;
        static BOOL settled;
        secStop = FALSE;                      /* a quit that didn't happen: PRISM2 again */
        if (!vb0)
            vb0 = GfxBase->VBCounter | 1;
        dbgMain = FindTask(NULL);
        while (!(SetSignal(0, 0) & SIGBREAKF_CTRL_C)) {
            /* up for a minute (3600 vertical blanks): this start is a good
             * one (see prismmon.c). Checked first: LOG=SYNC below sleeps,
             * and every second boot was skipped while it did (1.1 beta 4) */
            if (!settled && GfxBase->VBCounter - vb0 > 3600) {
                settled = TRUE;
                DeleteFile(PRISM_BOOTFLAG);
            }
            /* the log switched on while running (prismlog ON or SYNC, ARexx
             * LOG ON): its ring comes now - dbg() drops every line until it
             * exists, and before 1.1 beta 9 it only ever came at the start */
            if (prefs.log && !dbgRing && !(dbgRing = AllocVec(DBG_RING, MEMF_PUBLIC | MEMF_CLEAR)))
                prefs.log = 0;
            if (prefs.log == 2) {
                /* LOG=SYNC (or "prismlog SYNC"): sleep until dbg() has a
                 * line or a frame is up (sync_sleep), write the lines at
                 * once, ahead of whoever logged them; the frame's work
                 * below runs when the frame is up */
                ULONG sig;
                BOOL frame;
                SetTaskPri(dbgMain, 10);
                sig = sync_sleep(&frame);
                if (sig & SIGBREAKF_CTRL_C)
                    SetSignal(SIGBREAKF_CTRL_C, SIGBREAKF_CTRL_C);
                dbg_flush();
                if (alertWaiter) {
                    struct Task *w = alertWaiter;
                    alertWaiter = NULL;
                    Signal(w, SIGF_SINGLE);
                }
                if (prefs.log != 2)
                    SetTaskPri(dbgMain, 0);
                if (!frame)
                    continue;                 /* a log line: the frame's work when it is due */
            } else
                tick_sleep();                 /* one frame               */
            if (chipTop != 0xffffffffUL && (tick & 63) == 0 && other_rtg()) {
                chipTop = 0xffffffffUL;       /* Picasso96 came up after us */
                dbg("Picasso96 has started: its bitmaps are left to it from now on\n");
            }
            pbm_flush_shown();
            present_tick();
            pointer_tick();
            p96_pip_hw_tick();            /* layers on a board's overlay follow their windows */
            power_tick();                 /* POWERSAVE */
            mirror_tick();                /* MIRRORCHIPSET */
            second_tick();                /* SECONDMONITOR=SCREEN */
            mon2_tick();                  /* SECONDMONITOR=MIRROR */
            edge_tick();                  /* the pointer pushed over to the other monitor */
            arexx_tick();                 /* the PRISM port */
            if (prefs.log || ++tick % 25 == 0)
                dbg_flush();
        }
        SetSignal(0, SIGBREAKF_CTRL_C);
        sync_timer_close();               /* opened again if the loop goes on */
        tick_close();
#ifdef PRISM_MISSLOG
        miss_print();
#endif
        if (!second_close()) {
            printf("PrismD: windows are still open on PRISM2 - close them first\n");
            continue;
        }
        if (pscreens) {
            printf("PrismD: Prism screens are still open - close them first\n");
            continue;
        }
        if (!p96_remove()) {
            printf("PrismD: programs still have Picasso96API.library open\n");
            continue;
        }
        if (!cgxvideo_remove()) {
            printf("PrismD: programs still have cgxvideo.library open\n");
            continue;
        }
        if (!cgx_remove()) {
            printf("PrismD: programs still have cybergraphics.library open\n");
            continue;
        }
        /* a program still holding one of our bitmaps (an off-screen buffer
         * made with a Prism friend) would free it through the original
         * FreeBitMap later, with every plane aliasing one chip plane */
        render_quit();
        gels_quit();                  /* its saved backgrounds are Prism bitmaps */
        ObtainSemaphore(&lock);
        mon2_stop();                  /* the mirror's copy is one of ours too */
        ReleaseSemaphore(&lock);
        if (pbm_count()) {
            printf("PrismD: programs still hold %lu Prism bitmaps - close them first\n",
                   (unsigned long)pbm_count());
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
    arexx_quit();
    power_quit();
    for (k = 0; k < ncards; k++)
        if (cst[k].rtgOn)
            cards[k].b->setSwitch(cards[k].b, FALSE);
    printf("PrismD: stopped\n");
    rc = 0;

out:
    /* PrismD ended by itself - no board, a setting it can't use, a quit:
     * not the hang the boot flag is for (prismmon.c). Left there, the next
     * boot skipped PrismRTG even after the cause was fixed (1.3 beta 6).
     * Not when another PrismD runs: its start may still be under way. */
    DeleteFile(PRISM_BOOTFLAG);
out_other:
    for (k = 0; k < ncards; k++)
        present_stop(&cards[k]);
    driver_close();
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
    return cst[0].nmodes + (ncards > 1 ? cst[1].nmodes : 0);
}

BOOL prism_mode(ULONG i, struct PrismModeInfo *mi)
{
    struct ModeRec *r;
    if (i < cst[0].nmodes)
        r = &cst[0].modes[i];
    else if (ncards > 1 && i - cst[0].nmodes < cst[1].nmodes)
        r = &cst[1].modes[i - cst[0].nmodes];
    else
        return FALSE;
    mi->id = r->id;
    mi->w = r->m.width;
    mi->h = r->m.height;
    mi->fmt = r->m.format;
    mi->bpp = r->bpp;
    mi->name = r->name;
    return TRUE;
}

BOOL prism_mode_by_id(ULONG id, struct PrismModeInfo *mi)
{
    struct ModeRec *r = mode_by_id(id);
    if (!r)
        return FALSE;
    return prism_mode((ULONG)(r - cst[r->card->index].modes) +
                      (r->card->index ? cst[0].nmodes : 0), mi);
}

/* ---- for arexx.c (PrismD's own task) ---------------------------------- */

const char *prism_board_name(UBYTE k)
{
    return k < ncards && cards[k].b->name ? cards[k].b->name : NULL;
}

ULONG prism_board_vram(UBYTE k, ULONG *freeBytes)
{
    if (k >= ncards)
        return 0;
    if (freeBytes) {
        LOCK_QUIET();
        *freeBytes = vram_free_bytes(&cards[k], NULL);
        ReleaseSemaphore(&lock);
    }
    return cards[k].b->vramSize;
}

ULONG prism_board_flags(UBYTE k)
{
    return k < ncards ? cards[k].b->flags : 0;
}

LONG prism_test(UBYTE k, ULONG w, ULONG h, ULONG bits, ULONG hz)
{
    return test_begin_on(k, w, h, bits, hz, "ARexx");
}

void prism_test_stop(void)
{
    test_end();
}

void prism_sleep(BOOL on)
{
    LOCK();
    if (on)
        power_dark();
    else
        power_wake();
    ReleaseSemaphore(&lock);
}

BOOL prism_is_dark(void)
{
    return dark;
}

UBYTE *prism_setting(char which)
{
    return which == 'P' ? &prefs.powerSave : which == 'L' ? &prefs.log : NULL;
}
