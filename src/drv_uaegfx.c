/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * Native Prism UAE RTG candidate. Modern UAE exposes its host traps through
 * the emulator-resident uaegfx.card library. This driver binds that firmware
 * interface directly; it never loads a card file or calls the P96 adapter.
 * The shared P96 headers describe UAE's guest/host wire layout here.
 *
 * UAE retains BoardInfo and installs interrupt servers during InitCard.
 * The context and this task therefore stay resident until a machine reset.
 */
#include <exec/execbase.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#pragma pack(push, 2)
#include "p96sdk/boardinfo.h"
#pragma pack(pop)
#include "prismboard.h"
#include "cardcall.h"

/* GCC 13 otherwise overwrites the a0 argument in indirect tail calls. */
#pragma GCC optimize ("no-optimize-sibling-calls")
#include "rtg_ops.h"
#ifdef PRISM_DRIVER_MODULE
#include "driver_module.h"
#endif
_Static_assert(offsetof(struct BoardInfo, SetGC) == 294, "UAE BoardInfo ABI");
_Static_assert(offsetof(struct BoardInfo, MouseImage) == 1390, "UAE sprite ABI");
_Static_assert(sizeof(struct CLUTEntry) == 3, "UAE palette ABI");
_Static_assert(offsetof(struct ModeInfo, PixelClock) == 44, "UAE mode ABI");
/* WinUAE and Amiberry take the shown bitmap's size from here in SetPanning */
_Static_assert(offsetof(struct BitMapExtra, Width) == 40, "UAE panning ABI");
_Static_assert(offsetof(struct BitMapExtra, Height) == 42, "UAE panning ABI");

/* Shared with the generic adapter: either path may claim UAE, never both. */
#define UAE_OWNER "prism.p96.card"

struct UAEPriv {
    struct BoardInfo bi;
    struct ModeInfo mode;
    struct BitMapExtra shown;   /* bi.BitMapExtra: the bitmap on display */
    struct Library *firmware;
    struct SignalSemaphore owner;
    STRPTR tooltypes[1];
    UBYTE pf[5];
    UWORD sprite[4 + 48 * 4];
    BOOL initialized, modeSet;
};
static struct UAEPriv *resident;

static const RGBFTYPE wireformat[PF_COUNT] = {
    RGBFB_CLUT, RGBFB_R5G6B5, RGBFB_R5G6B5PC, RGBFB_R5G5B5,
    RGBFB_R5G5B5PC, RGBFB_R8G8B8, RGBFB_B8G8R8, RGBFB_A8R8G8B8,
    RGBFB_B8G8R8A8, RGBFB_R8G8B8A8, RGBFB_B5G6R5PC, RGBFB_B5G5R5PC
};
static UBYTE bytes(UBYTE pf)
{
    return pf == PF_CLUT8 ? 1 : pf == PF_RGB24 || pf == PF_BGR24 ? 3 :
           pf == PF_ARGB32 || pf == PF_BGRA32 || pf == PF_RGBA32 ? 4 : 2;
}

static ULONG uae_pitch(struct PrismBoard *b, UWORD w, UWORD h, UBYTE bpp)
{
    struct UAEPriv *p = b->priv;
    ULONG pitch = (ULONG)w * bpp;
    if (!w || !h || !bpp || bpp > 4 || p->pf[bpp] == PF_COUNT ||
        w > b->maxWidth || h > b->maxHeight || pitch > 32767)
        return 0;
    return pitch;
}

static BOOL uae_check(struct PrismBoard *b, struct PrismMode *m)
{
    struct UAEPriv *p = b->priv;
    UBYTE bpp, type;
    if (m->format >= PF_COUNT || (m->refresh && m->refresh != 60)) return FALSE;
    bpp = bytes(m->format);
    if (p->pf[bpp] != m->format) return FALSE;
    type = bpp == 1 ? CHUNKY : bpp == 2 ? HICOLOR : bpp == 3 ? TRUECOLOR : TRUEALPHA;
    if (m->width > p->bi.MaxHorResolution[type] ||
        m->height > p->bi.MaxVerResolution[type]) return FALSE;
    m->bytesPerRow = uae_pitch(b, m->width, m->height, bpp);
    if (!m->bytesPerRow || m->bytesPerRow * m->height > b->vramSize) return FALSE;
    m->refresh = 60;
    return TRUE;
}

/* WinUAE's and Amiberry's SetPanning take the virtual width and height -
 * and from them the row length - from bi->BitMapExtra, not from d0. Left
 * NULL, they read exception vectors at 0x28 and the display comes out with
 * the wrong stride (found by Dimitris Panokostas, PR #5). */
static void uae_pan(struct PrismBoard *b, ULONG offset)
{
    struct UAEPriv *p = b->priv;
    struct BitMapExtra *x = &p->shown;
    p->bi.XOffset = p->bi.YOffset = 0;
    memset(x, 0, sizeof(*x));
    x->BoardInfo = &p->bi;
    x->Width = p->mode.Width;
    x->Height = p->mode.Height;
    x->RenderInfo.Memory = b->vram + offset;
    x->RenderInfo.BytesPerRow = uae_pitch(b, p->mode.Width, p->mode.Height,
                                          (p->mode.Depth + 7) / 8);
    x->RenderInfo.RGBFormat = p->bi.RGBFormat;
    p->bi.BitMapExtra = x;
    p->bi.SetPanning(&p->bi, b->vram + offset, p->mode.Width, p->mode.Height,
                    0, 0, p->bi.RGBFormat);
}

static BOOL uae_mode(struct PrismBoard *b, struct PrismMode *m)
{
    struct UAEPriv *p = b->priv;
    struct BoardInfo *bi = &p->bi;
    struct ModeInfo *mi = &p->mode;
    if (!uae_check(b, m)) return FALSE;
    if (bi->SetInterrupt) bi->SetInterrupt(bi, FALSE);
    if (p->modeSet && bi->SetSprite) bi->SetSprite(bi, FALSE, bi->RGBFormat);
    bi->SetDisplay(bi, FALSE);
    memset(mi, 0, sizeof(*mi));
    mi->Active = 1; mi->Width = m->width; mi->Height = m->height;
    mi->Depth = bytes(m->format) * 8;
    /* UAE uses virtual timings. No physical PLL or monitor is programmed. */
    mi->HorTotal = m->width + 8; mi->VerTotal = m->height + 8;
    mi->HorBlankSize = mi->VerBlankSize = 8;
    mi->HorSyncStart = mi->VerSyncStart = 2;
    mi->HorSyncSize = mi->VerSyncSize = 2;
    mi->PixelClock = (ULONG)mi->HorTotal * mi->VerTotal * 60;
    bi->ModeInfo = mi; bi->Depth = mi->Depth;
    bi->RGBFormat = wireformat[m->format]; bi->Border = TRUE;
    bi->SetGC(bi, mi, TRUE);
    bi->SetDAC(bi, 0, bi->RGBFormat);
    if (bi->SetSprite) bi->SetSprite(bi, FALSE, bi->RGBFormat);
    uae_pan(b, 0);
    bi->SetDisplay(bi, TRUE);
    p->modeSet = TRUE;
    return TRUE;
}

static void uae_palette(struct PrismBoard *b, UWORD first, UWORD count, const UBYTE *rgb)
{
    struct BoardInfo *bi = &((struct UAEPriv *)b->priv)->bi;
    if (first >= 256) return;
    if (count > 256 - first) count = 256 - first;
    memcpy(&bi->CLUT[first], rgb, count * 3);
    bi->SetColorArray(bi, first, count);
}

static void uae_switch(struct PrismBoard *b, BOOL on)
{
    struct BoardInfo *bi = &((struct UAEPriv *)b->priv)->bi;
    bi->SetSwitch(bi, on);
}
static void uae_vblank(struct PrismBoard *b)
{
    (void)b;
    /* UAE's WaitVerticalSync vector is an RTS, not a real wait. */
    WaitTOF();
}

/* UAE traps are synchronous. When an accelerated operation is declined,
 * its ROM stub calls these defaults with the original register arguments. */
static UBYTE wirebytes(RGBFTYPE f)
{
    return f == RGBFB_CLUT ? 1 : f == RGBFB_R8G8B8 || f == RGBFB_B8G8R8 ? 3 :
           f == RGBFB_A8R8G8B8 || f == RGBFB_B8G8R8A8 || f == RGBFB_R8G8B8A8 ? 4 : 2;
}
static ULONG pen24(ULONG pen, UBYTE bpp)
{
    return bpp == 3 ? ((pen & 255) << 16) | (pen & 0xff00) | ((pen >> 16) & 255) : pen;
}
static void fill_default(struct BoardInfo *bi __asm("a0"), struct RenderInfo *ri __asm("a1"),
                         WORD x __asm("d0"), WORD y __asm("d1"), WORD w __asm("d2"),
                         WORD h __asm("d3"), ULONG pen __asm("d4"), UBYTE mask __asm("d5"),
                         RGBFTYPE f __asm("d7"))
{
    UBYTE bpp = wirebytes(f), k;
    WORD xx, yy;
    (void)bi;
    pen = pen24(pen, bpp);
    for (yy = 0; yy < h; yy++) for (xx = 0; xx < w; xx++) {
        UBYTE *d = (UBYTE *)ri->Memory + (LONG)(y+yy)*ri->BytesPerRow + (LONG)(x+xx)*bpp;
        if (bpp == 1) *d = (*d & ~mask) | (pen & mask);
        else for (k = 0; k < bpp; k++) d[k] = pen >> (8*(bpp-k-1));
    }
}
static void copy_default(struct BoardInfo *bi __asm("a0"), struct RenderInfo *ri __asm("a1"),
                         WORD sx __asm("d0"), WORD sy __asm("d1"), WORD dx __asm("d2"),
                         WORD dy __asm("d3"), WORD w __asm("d4"), WORD h __asm("d5"),
                         UBYTE mask __asm("d6"), RGBFTYPE f __asm("d7"))
{
    UBYTE bpp = wirebytes(f);
    LONG row = dy > sy ? h-1 : 0, step = dy > sy ? -1 : 1, n;
    (void)bi;
    /* Prism only requests unmasked copies. */
    (void)mask;
    if (w <= 0 || h <= 0) return;
    for (n = 0; n < h; n++, row += step)
        memmove((UBYTE *)ri->Memory + (dy+row)*ri->BytesPerRow + (LONG)dx*bpp,
                (UBYTE *)ri->Memory + (sy+row)*ri->BytesPerRow + (LONG)sx*bpp,
                (ULONG)w*bpp);
}
static void uae_fill(struct PrismBoard *b, ULONG off, ULONG pitch, UBYTE bpp,
                     UWORD x, UWORD y, UWORD w, UWORD h, ULONG pen)
{
    struct UAEPriv *p = b->priv;
    RGBFTYPE f = bpp == 1 ? RGBFB_CLUT : wireformat[p->pf[bpp]];
    struct RenderInfo ri = { b->vram + off, pitch, 0, f };
    /* Byte clears remain valid even when UAE's CLUT mode is disabled. */
    p->bi.FillRect(&p->bi, &ri, x, y, w, h, pen24(pen, bpp), 255, f);
}
static void uae_copy(struct PrismBoard *b, ULONG off, ULONG pitch, UBYTE bpp,
                     UWORD sx, UWORD sy, UWORD dx, UWORD dy, UWORD w, UWORD h)
{
    struct UAEPriv *p = b->priv;
    RGBFTYPE f = wireformat[p->pf[bpp]];
    struct RenderInfo ri = { b->vram + off, pitch, 0, f };
    p->bi.BlitRect(&p->bi, &ri, sx, sy, dx, dy, w, h, 255, f);
}

static void uae_cursor_image(struct PrismBoard *b, const UBYTE *img, const UBYTE *rgb)
{
    struct UAEPriv *p = b->priv;
    struct BoardInfo *bi = &p->bi;
    UWORD x, y;
    bi->Flags = (bi->Flags & ~BIF_BIGSPRITE) | BIF_HIRESSPRITE;
    bi->MouseWidth = 32; bi->MouseHeight = 48;
    memset(p->sprite, 0, sizeof(p->sprite));
    for (y = 0; y < 48; y++) for (x = 0; x < 32; x++) {
        UBYTE pen = img[y*CURSOR_SIZE+x];
        if (pen & 1) p->sprite[4+4*y+x/16] |= 0x8000 >> (x & 15);
        if (pen & 2) p->sprite[6+4*y+x/16] |= 0x8000 >> (x & 15);
    }
    for (x = 0; x < 3; x++) {
        bi->CLUT[17+x].Red = rgb[3*x]; bi->CLUT[17+x].Green = rgb[3*x+1];
        bi->CLUT[17+x].Blue = rgb[3*x+2];
        bi->SetSpriteColor(bi, x, rgb[3*x], rgb[3*x+1], rgb[3*x+2], bi->RGBFormat);
    }
    bi->SetSpriteImage(bi, bi->RGBFormat);
}
static void uae_cursor_show(struct PrismBoard *b, BOOL on)
{
    struct BoardInfo *bi = &((struct UAEPriv *)b->priv)->bi;
    bi->SetSprite(bi, on, bi->RGBFormat);
}
static void uae_cursor_move(struct PrismBoard *b, WORD x, WORD y)
{
    struct BoardInfo *bi = &((struct UAEPriv *)b->priv)->bi;
    bi->MouseX = x; bi->MouseY = y;
    bi->SetSpritePosition(bi, x, y, bi->RGBFormat);
}

static void quiet(struct UAEPriv *p)
{
    if (!p->initialized) return;
    if (p->bi.SetInterrupt) p->bi.SetInterrupt(&p->bi, FALSE);
    if (p->modeSet) {
        if (p->bi.SetSprite) p->bi.SetSprite(&p->bi, FALSE, p->bi.RGBFormat);
        p->bi.SetSwitch(&p->bi, FALSE);
    }
}
static void uae_shutdown(struct PrismBoard *b) { quiet(b->priv); }
static void soft_interrupt(void) { __asm__ volatile ("moveq #0,%%d0" : : : "d0"); }

/* By hand: NewMinList() is exec V45 (OS 3.2), and Prism runs on 3.1 */
static void init_list(struct MinList *l)
{
    l->mlh_Head = (struct MinNode *)&l->mlh_Tail;
    l->mlh_Tail = NULL;
    l->mlh_TailPred = (struct MinNode *)&l->mlh_Head;
}

BOOL UAEGFX_Probe(struct PrismBoard *b)
{
    static const UBYTE formats[] = { PF_CLUT8, PF_RGB565BE, PF_BGR565LE,
        PF_RGB565LE, PF_BGR24, PF_RGB24, PF_BGRA32, PF_ARGB32, PF_RGBA32 };
    struct UAEPriv *p;
    struct BoardInfo *bi;
    struct Library *lib;
    ULONG i;
    BOOL busy;
    extern struct Library *UtilityBase;
    /* Checking the live library list first prevents a disk driver load. */
    Forbid();
    lib = (struct Library *)FindName(&SysBase->LibList, "uaegfx.card");
    busy = FindSemaphore(UAE_OWNER) != NULL;
    if (lib && !busy && lib->lib_Version >= 3 && lib->lib_NegSize >= 36 &&
        lib->lib_IdString && !strncmp(lib->lib_IdString, "UAE Graphics Card", 17))
        lib = OpenLibrary("uaegfx.card", 0);
    else lib = NULL;
    Permit();
    if (!lib) {
        puts("PrismD: UAEGFX needs unclaimed emulator-resident uaegfx.card firmware");
        return FALSE;
    }
    p = AllocVec(sizeof(*p), MEMF_PUBLIC | MEMF_CLEAR);
    if (!p) { CloseLibrary(lib); return FALSE; }
    p->firmware = lib;
    bi = &p->bi;
    bi->ExecBase = SysBase; bi->UtilBase = UtilityBase;
    bi->CardBase = (struct CardBase *)lib;
    InitSemaphore(&bi->BoardLock);
    init_list(&bi->ResolutionsList);
    init_list(&bi->SpecialFeatures);
    init_list(&bi->BitMapList);
    init_list(&bi->MemList);
    init_list(&bi->WaitQ);
    bi->SoftInterrupt.is_Node.ln_Type = NT_INTERRUPT;
    bi->SoftInterrupt.is_Node.ln_Name = (STRPTR)"Prism UAE RTG";
    bi->SoftInterrupt.is_Data = bi; bi->SoftInterrupt.is_Code = soft_interrupt;
    bi->BlitPlanar2ChunkyDefault = rtg_planar_chunky;
    bi->BlitPlanar2DirectDefault = rtg_planar_direct;
    /* rtg_expand uses the card's BlitTemplate only when a default exists
     * for it to fall back on; without this line every string was drawn
     * on the CPU (five times slower than through the P96 adapter) */
    bi->BlitTemplateDefault = rtg_template_default;
    bi->MouseImage = p->sprite;
    bi->FillRectDefault = fill_default; bi->BlitRectDefault = copy_default;
    InitSemaphore(&p->owner);
    p->owner.ss_Link.ln_Name = (STRPTR)UAE_OWNER;
    Forbid();
    busy = FindSemaphore(UAE_OWNER) != NULL;
    if (!busy) AddSemaphore(&p->owner);
    Permit();
    if (busy) { CloseLibrary(lib); FreeVec(p); return FALSE; }
    /* UAE writes its retained BoardInfo pointer even if FindCard fails.
     * Keep the context from this point onward, including error paths. */
    resident = p;
    if (!card_FindCard(lib, bi, NULL)) {
        puts("PrismD: UAE RTG memory is unavailable or already claimed");
        return FALSE;
    }
    p->initialized = card_InitCard(lib, bi, p->tooltypes);
    if (!p->initialized) { puts("PrismD: UAE RTG initialization failed"); return FALSE; }
    if (bi->SetInterrupt) bi->SetInterrupt(bi, FALSE);
    if (!bi->MemoryBase || bi->MemorySize < 65536 || !bi->SetGC || !bi->SetDAC ||
        !bi->SetPanning || !bi->SetColorArray || !bi->SetSwitch || !bi->SetDisplay) {
        puts("PrismD: incomplete UAE RTG firmware interface"); return FALSE;
    }
    memset(b, 0, sizeof(*b));
    memset(p->pf, PF_COUNT, sizeof(p->pf));
    for (i = 0; i < sizeof(formats); i++) {
        UBYTE pf = formats[i], bpp = bytes(pf);
        if (p->pf[bpp] == PF_COUNT && (bi->RGBFormats & (1UL << wireformat[pf]))) {
            p->pf[bpp] = pf; b->formats |= PF_BIT(pf);
        }
    }
    if (!b->formats) { puts("PrismD: UAE has no enabled Prism pixel formats"); return FALSE; }
    if (!rtg_probe_planar(bi, b->formats)) bi->BlitPlanar2Direct = rtg_planar_direct;
    puts(bi->BlitPlanar2Direct == rtg_planar_direct ?
        "PrismD: UAE planar copies use CPU fallback" : "PrismD: UAE planar copies use host hook");
    b->ops = &rtg_ops;
    b->priv = p; b->name = "UAE (native)";
    b->vram = bi->MemoryBase; b->vramSize = bi->MemorySize;
    b->maxWidth = b->maxHeight = 8192;
    b->checkMode = uae_check; b->setMode = uae_mode;
    b->bytesPerRow = uae_pitch; b->setDisplayStart = uae_pan;
    b->setPalette = uae_palette; b->setSwitch = uae_switch;
    b->waitVBlank = uae_vblank; b->shutdown = uae_shutdown;
    if (!(bi->Flags & BIF_NOBLITTER)) {
        if (bi->FillRect) { b->fillRect = uae_fill; b->flags |= PBF_BLIT_32; }
        if (bi->BlitRect) b->copyRect = uae_copy;
    }
    if ((bi->Flags & BIF_HARDWARESPRITE) && bi->SetSprite && bi->SetSpriteImage &&
        bi->SetSpritePosition && bi->SetSpriteColor) {
        b->cursorImage = uae_cursor_image; b->cursorShow = uae_cursor_show;
        b->cursorMove = uae_cursor_move; b->flags |= PBF_HW_CURSOR;
    } else puts("PrismD: UAE has no hardware cursor; using software pointer");
    return TRUE;
}

void UAEGFX_KeepResident(void)
{
    if (!resident) return;
    quiet(resident);
    puts("PrismD: UAE RTG context remains resident until reboot");
    fflush(stdout);
#ifdef PRISM_DRIVER_MODULE
    /* The loader may release its output only after our last diagnostic. */
    {
        extern void driver_module_done(void);
        driver_module_done();
    }
#endif
    for (;;) Wait(SIGBREAKF_CTRL_C);
}

/* ---- as a loadable module (driver_module.c) --------------------------- */
#ifdef PRISM_DRIVER_MODULE
BOOL driver_probe(struct PrismBoard *b, const struct PrismDriverConfig *config)
{
    return UAEGFX_Probe(b);
}
void driver_retain(void)
{
    UAEGFX_KeepResident();
}
#endif
