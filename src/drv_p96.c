/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * P96 card-driver adapter. The P96 core is not loaded: Prism owns screens,
 * bitmaps and software rendering, and calls the card ABI directly.
 *
 * Contract: contiguous CPU apertures with explicit format switching.
 * Mapped formats use stable CPU shadows. Special allocators are rejected.
 * P96 has no card teardown vector. Once FindCard succeeds, this task and
 * its libraries must remain resident, even on a subsequent startup error.
 *
 * ABI reference: https://wiki.icomp.de/wiki/P96_Driver_Development
 */
#include <exec/memory.h>
#include <exec/execbase.h>
#include <graphics/rastport.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/icon.h>
#include <proto/dos.h>
#include <workbench/workbench.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#pragma pack(push, 2)
#include "p96sdk/boardinfo.h"
#pragma pack(pop)
#include "prismboard.h"
#include "cardcall.h"

/* amiga-gcc 13 can put an indirect tail-call target in a0, overwriting
 * the BoardInfo argument. Keep register-ABI calls as ordinary calls. */
#pragma GCC optimize ("no-optimize-sibling-calls")
#include "rtg_ops.h"
#ifdef PRISM_DRIVER_MODULE
#include "driver_module.h"
#endif

/* These are binary ABI offsets, not host C structure sizes. */
_Static_assert(sizeof(BOOL) == 2, "P96 requires 16-bit BOOL");
_Static_assert(offsetof(struct BoardInfo, MemoryBase) == 4, "P96 MemoryBase ABI");
_Static_assert(offsetof(struct BoardInfo, SetGC) == 294, "P96 SetGC ABI");
_Static_assert(offsetof(struct BoardInfo, FillRect) == 382, "P96 FillRect ABI");
_Static_assert(offsetof(struct BoardInfo, ModeInfo) == 558, "P96 ModeInfo ABI");
_Static_assert(sizeof(struct CLUTEntry) == 3, "P96 palette entries have no padding");
_Static_assert(offsetof(struct BoardInfo, MouseImage) == 1390, "P96 sprite ABI");
_Static_assert(offsetof(struct ModeInfo, PixelClock) == 44, "P96 ModeInfo layout");

#define P96_OWNER "prism.p96.card"
#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

struct P96Priv {
    struct BoardInfo bi;                  /* first: callbacks receive this */
    struct SignalSemaphore owner;
    struct Library *card;
    struct Library *icon;
    struct DiskObject *disk;
    STRPTR emptyToolTypes[1];
    struct ModeInfo mode;
    ULONG formats;
    UBYTE pf[5];                          /* selected Prism format by bpp */
    BOOL initialized, modeSet;
    struct { ULONG offset, size; } allocations[256];
    UWORD nallocations;
    UWORD sprite[4 + CURSOR_SIZE * 4];    /* 32-pixel P96 sprite + controls */
    char name[96];
};

static struct P96Priv *resident;

static const RGBFTYPE rgbformat[PF_COUNT] = {
    RGBFB_CLUT, RGBFB_R5G6B5, RGBFB_R5G6B5PC, RGBFB_R5G5B5,
    RGBFB_R5G5B5PC, RGBFB_R8G8B8, RGBFB_B8G8R8, RGBFB_A8R8G8B8,
    RGBFB_B8G8R8A8, RGBFB_R8G8B8A8, RGBFB_B5G6R5PC, RGBFB_B5G5R5PC
};

static UBYTE rgb_bpp(RGBFTYPE f)
{
    switch (f) {
    case RGBFB_CLUT: return 1;
    case RGBFB_R5G6B5: case RGBFB_R5G6B5PC: case RGBFB_B5G6R5PC:
    case RGBFB_R5G5B5: case RGBFB_R5G5B5PC: case RGBFB_B5G5R5PC: return 2;
    case RGBFB_R8G8B8: case RGBFB_B8G8R8: return 3;
    case RGBFB_A8R8G8B8: case RGBFB_A8B8G8R8:
    case RGBFB_R8G8B8A8: case RGBFB_B8G8R8A8: return 4;
    default: return 0;
    }
}

static void wait_blitter(struct BoardInfo *bi)
{
    if (bi->WaitBlitter) bi->WaitBlitter(bi);
}

/* P96 passes 16/32-bit pens in memory order as a 68k integer, but 24-bit
 * pens have the first byte in bits 0..7. Prism always uses memory order. */
static ULONG p96_pen(ULONG colour, UBYTE bpp)
{
    if (bpp == 3)
        return ((colour & 0xff) << 16) | (colour & 0xff00) | ((colour >> 16) & 0xff);
    return colour;
}

static void put_pen(UBYTE *dst, ULONG pen, UBYTE bpp, UBYTE mask)
{
    UBYTE i;
    if (bpp == 1) {
        *dst = (*dst & ~mask) | ((UBYTE)pen & mask);
        return;
    }
    pen = p96_pen(pen, bpp);
    for (i = 0; i < bpp; i++)
        dst[i] = pen >> (8 * (bpp - i - 1));
}

static void cpu_fill(struct BoardInfo *bi __asm("a0"),
                     struct RenderInfo *ri __asm("a1"),
                     WORD x __asm("d0"), WORD y __asm("d1"),
                     WORD w __asm("d2"), WORD h __asm("d3"),
                     ULONG pen __asm("d4"), UBYTE mask __asm("d5"),
                     RGBFTYPE fmt __asm("d7"))
{
    UBYTE bpp = rgb_bpp(fmt);
    WORD xx, yy;
    wait_blitter(bi);
    if (!bpp || w <= 0 || h <= 0) return;
    for (yy = 0; yy < h; yy++) {
        UBYTE *d = (UBYTE *)ri->Memory + (LONG)(y + yy) * ri->BytesPerRow + (LONG)x * bpp;
        for (xx = 0; xx < w; xx++, d += bpp)
            put_pen(d, pen, bpp, mask);
    }
}

static void cpu_copy(struct BoardInfo *bi __asm("a0"),
                     struct RenderInfo *ri __asm("a1"),
                     WORD sx __asm("d0"), WORD sy __asm("d1"),
                     WORD dx __asm("d2"), WORD dy __asm("d3"),
                     WORD w __asm("d4"), WORD h __asm("d5"),
                     UBYTE mask __asm("d6"), RGBFTYPE fmt __asm("d7"))
{
    UBYTE bpp = rgb_bpp(fmt);
    LONG row, step = 1, n, bytes = (LONG)w * bpp;
    wait_blitter(bi);
    if (!bpp || w <= 0 || h <= 0) return;
    row = 0;
    if (dy > sy) { row = h - 1; step = -1; }
    for (n = 0; n < h; n++, row += step) {
        UBYTE *s = (UBYTE *)ri->Memory + (sy + row) * ri->BytesPerRow + (LONG)sx * bpp;
        UBYTE *d = (UBYTE *)ri->Memory + (dy + row) * ri->BytesPerRow + (LONG)dx * bpp;
        if (bpp != 1 || mask == 0xff) {
            memmove(d, s, bytes);
        } else {
            LONG i;
            if (d > s && d < s + bytes) {
                for (i = bytes; i-- > 0;)
                    d[i] = (d[i] & ~mask) | (s[i] & mask);
            } else {
                for (i = 0; i < bytes; i++)
                    d[i] = (d[i] & ~mask) | (s[i] & mask);
            }
        }
    }
}

static void soft_interrupt(void)
{
    /* Prism polls; no P96 WaitTOF queue. Interrupt code returns zero in d0. */
    __asm__ volatile ("moveq #0,%%d0" : : : "d0");
}

static BOOL soft_sprite(struct BoardInfo *bi __asm("a0"),
                        ULONG formats __asm("d0"), struct ModeInfo *mi __asm("a1"))
{
    (void)mi;
    return !(bi->Flags & BIF_HARDWARESPRITE) || (formats & bi->SoftSpriteFlags) != 0;
}

static BOOL vsync_state(struct BoardInfo *bi __asm("a0"), BOOL expected __asm("d0"))
{
    (void)bi;
    return expected;
}

/* Drivers can wrap these defaults to impose stricter alignment. They also
 * serve private driver allocations, so all P96 allocations share this pool.
 * Migration is owned by Prism; force/system never evict behind its back. */
static APTR card_alloc_at(struct BoardInfo *bi,ULONG size,ULONG offset,BOOL exact)
{
    struct P96Priv *p=(struct P96Priv *)bi;
    ULONG pos=0,end;UWORD i,j;
    if(!size || size>0xfffffff0UL || p->nallocations==256)return NULL;
    size=(size+15)&~15UL;
    for(i=0;i<=p->nallocations;i++) {
        end=i<p->nallocations ? p->allocations[i].offset : bi->MemorySize;
        if(exact && pos<offset)pos=offset;
        if(pos<=end && size<=end-pos) {
            for(j=p->nallocations;j>i;j--)p->allocations[j]=p->allocations[j-1];
            p->allocations[i].offset=pos;p->allocations[i].size=size;
            p->nallocations++;
            return bi->MemoryBase+pos;
        }
        if(i<p->nallocations)pos=p->allocations[i].offset+p->allocations[i].size;
        if(exact && pos>offset)break;
    }
    return NULL;
}
static APTR card_alloc(struct BoardInfo *bi __asm("a0"), ULONG size __asm("d0"),
    BOOL force __asm("d1"), BOOL system __asm("d2"), ULONG pitch __asm("d3"),
    struct ModeInfo *mi __asm("a1"), RGBFTYPE format __asm("d7"))
{
    (void)force;(void)system;(void)pitch;(void)mi;(void)format;
    return card_alloc_at(bi,size,0,FALSE);
}
static APTR card_alloc_abs(struct BoardInfo *bi __asm("a0"),ULONG size __asm("d0"),
    char *target __asm("a1"))
{
    ULONG offset=(ULONG)target-(ULONG)bi->MemoryBase;
    if(offset>bi->MemorySize || (offset&15))return NULL;
    return card_alloc_at(bi,size,offset,TRUE);
}
static BOOL card_free(struct BoardInfo *bi __asm("a0"),APTR mem __asm("a1"))
{
    struct P96Priv *p=(struct P96Priv *)bi;
    ULONG offset=(ULONG)mem-(ULONG)bi->MemoryBase;UWORD i;
    for(i=0;i<p->nallocations;i++)if(p->allocations[i].offset==offset) {
        for(;i+1<p->nallocations;i++)p->allocations[i]=p->allocations[i+1];
        p->nallocations--;return TRUE;
    }
    return FALSE;
}
static void card_reinit(struct BoardInfo *bi __asm("a0"),RGBFTYPE format __asm("d0"))
{
    (void)format;
    ((struct P96Priv *)bi)->nallocations=0;
}

static void init_list(struct MinList *l)
{
    l->mlh_Head = (struct MinNode *)&l->mlh_Tail;
    l->mlh_Tail = NULL;
    l->mlh_TailPred = (struct MinNode *)&l->mlh_Head;
}

static void init_boardinfo(struct P96Priv *p)
{
    struct BoardInfo *bi = &p->bi;
    extern struct Library *UtilityBase;
    bi->ExecBase = SysBase;
    bi->UtilBase = UtilityBase;
    bi->CardBase = (struct CardBase *)p->card;
    bi->BoardName = p->name;
    bi->BitsPerCannon = 8;
    bi->Flags = BIF_GRANTDIRECTACCESS;
    bi->AllocCardMem = card_alloc;
    bi->FreeCardMem = card_free;
    bi->AllocCardMemAbs = card_alloc_abs;
    bi->ReInitMemory = card_reinit;
    bi->MaxBMWidth = bi->MaxBMHeight = 4096;
    InitSemaphore(&bi->BoardLock);
    init_list(&bi->ResolutionsList);
    init_list(&bi->SpecialFeatures);
    init_list(&bi->BitMapList);
    init_list(&bi->MemList);
    init_list(&bi->WaitQ);
    bi->SoftInterrupt.is_Node.ln_Type = NT_INTERRUPT;
    bi->SoftInterrupt.is_Node.ln_Name = (STRPTR)P96_OWNER;
    bi->SoftInterrupt.is_Data = bi;
    bi->SoftInterrupt.is_Code = soft_interrupt;
    bi->HardInterrupt.is_Node.ln_Type = NT_INTERRUPT;
    bi->HardInterrupt.is_Node.ln_Name = (STRPTR)P96_OWNER;
    bi->HardInterrupt.is_Data = bi;
    bi->FillRect = bi->FillRectDefault = cpu_fill;
    bi->BlitRect = bi->BlitRectDefault = cpu_copy;
    bi->BlitTemplate = bi->BlitTemplateDefault = rtg_template_default;
    bi->GetVSyncState = vsync_state;
    bi->EnableSoftSprite = bi->EnableSoftSpriteDefault = soft_sprite;
    bi->BlitPlanar2ChunkyDefault = rtg_planar_chunky;
    bi->BlitPlanar2DirectDefault = rtg_planar_direct;
    bi->MouseImage = p->sprite;
    bi->MouseWidth = 32;
    bi->MouseHeight = 48;
    memset(p->pf, PF_COUNT, sizeof(p->pf));
}

/* Standard timings already used by Prism's Cirrus driver. P96 sync starts
 * are measured from the end of the visible area, not the start of a line.
 * Internal-mode drivers (notably uaegfx) supply their own ModeInfos. */
struct Timing {
    UWORD w, h, hz;
    ULONG clock;
    UWORD hs, he, ht, vs, ve, vt;
    UBYTE flags;
};
static const struct Timing timings[] = {
    {640,480,60,25175000,656,752,800,490,492,525,0},
    {640,480,72,31500000,664,704,832,489,492,520,0},
    {640,480,75,31500000,656,720,840,481,484,500,0},
    {640,400,70,25175000,656,752,800,412,414,449,GMF_VPOLARITY},
    {800,600,60,40000000,840,968,1056,601,605,628,GMF_HPOLARITY|GMF_VPOLARITY},
    {800,600,72,50000000,856,976,1040,637,643,666,GMF_HPOLARITY|GMF_VPOLARITY},
    {800,600,75,49500000,816,896,1056,601,604,625,GMF_HPOLARITY|GMF_VPOLARITY},
    {1024,768,60,65000000,1048,1184,1344,771,777,806,0},
    {1024,768,70,75000000,1048,1184,1328,771,777,806,0},
    {1024,768,75,78750000,1040,1136,1312,769,772,800,GMF_HPOLARITY|GMF_VPOLARITY},
    {1280,720,60,74250000,1390,1430,1650,725,730,750,GMF_HPOLARITY|GMF_VPOLARITY},
    {1280,1024,60,108000000,1328,1440,1688,1025,1028,1066,GMF_HPOLARITY|GMF_VPOLARITY},
    {1280,1024,75,135000000,1296,1440,1688,1025,1028,1066,GMF_HPOLARITY|GMF_VPOLARITY}
};

static UBYTE mode_type(UBYTE bpp)
{
    return bpp == 1 ? CHUNKY : bpp == 2 ? HICOLOR : bpp == 3 ? TRUECOLOR : TRUEALPHA;
}

static UBYTE refresh(const struct ModeInfo *mi)
{
    ULONG total = (ULONG)mi->HorTotal * mi->VerTotal;
    ULONG hz;
    if (!total) return 0;
    hz = (mi->PixelClock + total / 2) / total;
    if (mi->Flags & GMF_DOUBLESCAN) hz /= 2;
    if (mi->Flags & GMF_INTERLACE) hz *= 2;
    return hz < 256 ? hz : 0;
}

static BOOL mode_info(struct P96Priv *p, struct PrismMode *m, struct ModeInfo *mi)
{
    struct BoardInfo *bi = &p->bi;
    UBYTE bpp, type;
    UBYTE nominal = 0;
    ULONG i;
    if (m->format >= PF_COUNT) return FALSE;
    bpp = rgb_bpp(rgbformat[m->format]);
    if (!bpp || !(p->formats & PF_BIT(m->format))) return FALSE;
    type = mode_type(bpp);
    memset(mi, 0, sizeof(*mi));
    if (bi->Flags & BIF_INTERNALMODESONLY) {
        struct LibResolution *r;
        BOOL found = FALSE;
        for (r = (struct LibResolution *)bi->ResolutionsList.mlh_Head;
             r->Node.ln_Succ; r = (struct LibResolution *)r->Node.ln_Succ) {
            const struct ModeInfo *candidate = r->Modes[type];
            if (r->Width == m->width && r->Height == m->height && candidate &&
                candidate->Depth == bpp * 8 &&
                (!m->refresh || refresh(candidate) == m->refresh)) {
                *mi = *candidate;
                found = TRUE;
                break;
            }
        }
        if (!found) return FALSE;
    } else {
        const struct Timing *t = NULL;
        for (i = 0; i < COUNT(timings); i++)
            if (timings[i].w == m->width && timings[i].h == m->height &&
                (!m->refresh || timings[i].hz == m->refresh)) {
                t = &timings[i];
                break;
            }
        if (!t || !bi->ResolvePixelClock) return FALSE;
        nominal = t->hz;
        mi->Width = t->w; mi->Height = t->h; mi->Depth = bpp * 8;
        mi->HorTotal = t->ht; mi->HorBlankSize = t->ht - t->w;
        mi->HorSyncStart = t->hs - t->w; mi->HorSyncSize = t->he - t->hs;
        mi->VerTotal = t->vt; mi->VerBlankSize = t->vt - t->h;
        mi->VerSyncStart = t->vs - t->h; mi->VerSyncSize = t->ve - t->vs;
        mi->Flags = t->flags; mi->PixelClock = t->clock;
        if (bi->ResolvePixelClock(bi, mi, t->clock, rgbformat[m->format]) < 0 ||
            mi->PixelClock < t->clock - t->clock / 20 ||
            mi->PixelClock > t->clock + t->clock / 20)
            return FALSE;
    }
    if ((bi->MaxHorResolution[type] && mi->Width > bi->MaxHorResolution[type]) ||
        (bi->MaxVerResolution[type] && mi->Height > bi->MaxVerResolution[type]) ||
        (bi->MaxHorValue[type] && mi->HorTotal > bi->MaxHorValue[type]) ||
        (bi->MaxVerValue[type] && mi->VerTotal > bi->MaxVerValue[type]) ||
        mi->Width > bi->MaxBMWidth || mi->Height > bi->MaxBMHeight)
        return FALSE;
    m->bytesPerRow = bi->CalculateBytesPerRow(bi, m->width, m->height, mi,
                                             rgbformat[m->format]);
    if (m->bytesPerRow < (ULONG)m->width * bpp || m->bytesPerRow > 32767 ||
        m->bytesPerRow * m->height > bi->MemorySize)
        return FALSE;
    m->refresh = nominal ? nominal : refresh(mi);
    return TRUE;
}

static BOOL p96_check(struct PrismBoard *b, struct PrismMode *m)
{
    struct ModeInfo mi;
    return mode_info(b->priv, m, &mi);
}

static ULONG p96_surface_pitch(struct PrismBoard *b, UWORD w, UWORD h, UBYTE format)
{
    struct P96Priv *p = b->priv;
    struct ModeInfo mi;
    struct PrismMode m;
    if (format >= PF_COUNT || w > p->bi.MaxBMWidth || h > p->bi.MaxBMHeight ||
        w > 32767 || h > 32767) return 0;
    memset(&m, 0, sizeof(m));
    m.width = w; m.height = h; m.format = format;
    if (mode_info(p, &m, &mi)) return m.bytesPerRow;
    return rtg_pitch(b,w,h,format);
}
static ULONG p96_pitch(struct PrismBoard *b, UWORD w, UWORD h, UBYTE bpp)
{
    struct P96Priv *p = b->priv;
    return bpp && bpp <= 4 ? p96_surface_pitch(b,w,h,p->pf[bpp]) : 0;
}
static BOOL p96_allocate(struct PrismBoard *b,struct PrismSurface *s)
{
    struct P96Priv *p=b->priv;
    struct BoardInfo *bi=&p->bi;
    struct ModeInfo mi;
    struct PrismMode m;
    BOOL displayable;
    APTR memory;
    memset(&m,0,sizeof(m));
    m.width=s->width;m.height=s->height;m.format=s->format;
    displayable=mode_info(p,&m,&mi);
    wait_blitter(bi);
    memory=bi->AllocCardMem(bi,s->allocation,FALSE,FALSE,
        displayable ? s->pitch : 0,displayable ? &mi : NULL,
        rgbformat[s->format]);
    if (!memory) return FALSE;
    s->offset=(ULONG)memory-(ULONG)b->vram;
    return TRUE;
}
static void p96_release(struct PrismBoard *b,ULONG off)
{
    struct BoardInfo *bi=b->priv;
    wait_blitter(bi);
    bi->FreeCardMem(bi,b->vram+off);
}
static const struct PrismOps p96_alloc_ops = {
    .fill=rtg_fill,.copy=rtg_copy,.expand=rtg_expand,.planar=rtg_planar,
    .pitch=p96_surface_pitch,.read=rtg_read,.write=rtg_write,
    .allocate=p96_allocate,.release=p96_release
};

static void p96_pan(struct PrismBoard *b, ULONG offset)
{
    struct P96Priv *p = b->priv;
    struct BoardInfo *bi = &p->bi;
    wait_blitter(bi);
    bi->XOffset = bi->YOffset = 0;
    bi->SetPanning(bi, b->vram + offset, p->mode.Width, p->mode.Height,
                   0, 0, bi->RGBFormat);
}

static BOOL p96_mode(struct PrismBoard *b, struct PrismMode *m)
{
    struct P96Priv *p = b->priv;
    struct BoardInfo *bi = &p->bi;
    struct ModeInfo mi;
    if (!mode_info(p, m, &mi)) return FALSE;
    wait_blitter(bi);
    if (bi->SetInterrupt) bi->SetInterrupt(bi, FALSE);
    if (p->modeSet && bi->SetSprite) bi->SetSprite(bi, FALSE, bi->RGBFormat);
    bi->SetDisplay(bi, FALSE);
    if ((b->flags & PBF_REINIT) && (!p->modeSet ||
        !(bi->GetCompatibleFormats(bi,bi->RGBFormat) & (1UL<<rgbformat[m->format])))) {
        bi->ReInitMemory(bi,rgbformat[m->format]);
        b->vram=bi->MemoryBase;b->vramSize=bi->MemorySize;
    }
    p->mode = mi;
    bi->ModeInfo = &p->mode;
    bi->RGBFormat = rgbformat[m->format];
    bi->Depth = mi.Depth;
    bi->Border = TRUE;
    if (b->cursorImage && (!bi->EnableSoftSprite ||
        !bi->EnableSoftSprite(bi,1UL<<bi->RGBFormat,&p->mode))) b->flags |= PBF_HW_CURSOR;
    else b->flags &= ~PBF_HW_CURSOR;
    if (bi->SetMemoryMode) bi->SetMemoryMode(bi, bi->RGBFormat);
    bi->SetGC(bi, &p->mode, TRUE);
    if (bi->SetClock) bi->SetClock(bi);
    bi->SetDAC(bi, 0, bi->RGBFormat);
    if (bi->SetSprite) bi->SetSprite(bi, FALSE, bi->RGBFormat);
    p96_pan(b, 0);
    bi->SetDisplay(bi, TRUE);
    p->modeSet = TRUE;
    return TRUE;
}

static void p96_palette(struct PrismBoard *b, UWORD first, UWORD count, const UBYTE *rgb)
{
    struct BoardInfo *bi = &((struct P96Priv *)b->priv)->bi;
    if (first >= 256) return;
    if (count > 256 - first) count = 256 - first;
    memcpy(&bi->CLUT[first], rgb, count * 3);
    bi->SetColorArray(bi, first, count);
}

static void p96_switch(struct PrismBoard *b, BOOL on)
{
    struct BoardInfo *bi = &((struct P96Priv *)b->priv)->bi;
    wait_blitter(bi);
    bi->SetSwitch(bi, on);
}

static void p96_wait(struct PrismBoard *b)
{
    wait_blitter(&((struct P96Priv *)b->priv)->bi);
}

static void quiesce(struct P96Priv *p)
{
    if (!p->initialized) return;
    wait_blitter(&p->bi);
    if (p->bi.SetInterrupt) p->bi.SetInterrupt(&p->bi, FALSE);
    if (p->modeSet) {
        if (p->bi.SetSprite) p->bi.SetSprite(&p->bi, FALSE, p->bi.RGBFormat);
        p->bi.SetSwitch(&p->bi, FALSE);
    }
}

static void p96_shutdown(struct PrismBoard *b)
{
    /* There is no driver release ABI; only quiesce the hardware. */
    quiesce(b->priv);
}

static void p96_vblank(struct PrismBoard *b)
{
    struct BoardInfo *bi = &((struct P96Priv *)b->priv)->bi;
    if (bi->WaitVerticalSync) {
        bi->WaitVerticalSync(bi, TRUE);
        bi->WaitVerticalSync(bi, FALSE);
    } else {
        WaitTOF();
    }
}

static void p96_fill(struct PrismBoard *b, ULONG offset, ULONG pitch, UBYTE bpp,
                     UWORD x, UWORD y, UWORD w, UWORD h, ULONG colour)
{
    struct P96Priv *p = b->priv;
    RGBFTYPE f = bpp <= 4 && p->pf[bpp] < PF_COUNT ? rgbformat[p->pf[bpp]] : RGBFB_CLUT;
    struct RenderInfo ri = { b->vram + offset, pitch, 0, f };
    /* Prism also uses byte fills to clear direct-colour bitmaps. A card
     * without CLUT support can still do these through the CPU default. */
    if (bpp == 1 && p->pf[1] == PF_COUNT)
        cpu_fill(&p->bi, &ri, x, y, w, h, colour, 0xff, RGBFB_CLUT);
    else
        p->bi.FillRect(&p->bi, &ri, x, y, w, h, p96_pen(colour, bpp), 0xff, f);
    wait_blitter(&p->bi);
}

static void p96_copy(struct PrismBoard *b, ULONG offset, ULONG pitch, UBYTE bpp,
                     UWORD sx, UWORD sy, UWORD dx, UWORD dy, UWORD w, UWORD h)
{
    struct P96Priv *p = b->priv;
    RGBFTYPE f = rgbformat[p->pf[bpp]];
    struct RenderInfo ri = { b->vram + offset, pitch, 0, f };
    p->bi.BlitRect(&p->bi, &ri, sx, sy, dx, dy, w, h, 0xff, f);
    wait_blitter(&p->bi);
}

static BOOL p96_expand(struct PrismBoard *b, ULONG offset, ULONG pitch, UBYTE bpp,
                       UWORD x, UWORD y, UWORD w, UWORD h, const UBYTE *src,
                       ULONG mod, ULONG fg, ULONG bg, BOOL transparent)
{
    struct P96Priv *p = b->priv;
    RGBFTYPE f;
    struct RenderInfo ri;
    struct Template t;
    if (!bpp || bpp > 4 || p->pf[bpp] == PF_COUNT || mod > 32767 || pitch > 32767)
        return FALSE;
    f = rgbformat[p->pf[bpp]];
    ri.Memory = b->vram + offset; ri.BytesPerRow = pitch; ri.pad = 0; ri.RGBFormat = f;
    t.Memory = (APTR)src; t.BytesPerRow = mod; t.XOffset = 0;
    t.DrawMode = transparent ? JAM1 : JAM2;
    t.FgPen = p96_pen(fg, bpp); t.BgPen = p96_pen(bg, bpp);
    p->bi.BlitTemplate(&p->bi, &ri, &t, x, y, w, h, 0xff, f);
    wait_blitter(&p->bi);
    return TRUE;
}

static void p96_cursor_image(struct PrismBoard *b, const UBYTE *img, const UBYTE *rgb)
{
    struct P96Priv *p = b->priv;
    struct BoardInfo *bi = &p->bi;
    UWORD x, y;
    /* HIRESSPRITE selects a 32-pixel source (two words per plane).
     * It is a requested image layout, not a hardware capability flag. */
    bi->Flags = (bi->Flags & ~BIF_BIGSPRITE) | BIF_HIRESSPRITE;
    bi->MouseWidth = 32;
    bi->MouseHeight = 48;
    memset(p->sprite, 0, sizeof(p->sprite));
    for (y = 0; y < bi->MouseHeight; y++)
        for (x = 0; x < 32; x++) {
            UBYTE pen = img[y * CURSOR_SIZE + x];
            if (pen & 1) p->sprite[4 + 4 * y + x / 16] |= 0x8000 >> (x & 15);
            if (pen & 2) p->sprite[6 + 4 * y + x / 16] |= 0x8000 >> (x & 15);
        }
    for (x = 0; x < 3; x++) {
        /* Some drivers reload these entries when enabling the sprite. */
        bi->CLUT[17+x].Red = rgb[3*x];
        bi->CLUT[17+x].Green = rgb[3*x+1];
        bi->CLUT[17+x].Blue = rgb[3*x+2];
        bi->SetSpriteColor(bi, x, rgb[3*x], rgb[3*x+1], rgb[3*x+2], bi->RGBFormat);
    }
    bi->SetSpriteImage(bi, bi->RGBFormat);
}

static void p96_cursor_show(struct PrismBoard *b, BOOL on)
{
    struct BoardInfo *bi = &((struct P96Priv *)b->priv)->bi;
    if (on && bi->EnableSoftSprite(bi, 1UL << bi->RGBFormat, bi->ModeInfo))
        on = FALSE;
    bi->SetSprite(bi, on, bi->RGBFormat);
}

static void p96_cursor_move(struct PrismBoard *b, WORD x, WORD y)
{
    struct BoardInfo *bi = &((struct P96Priv *)b->priv)->bi;
    bi->MouseX = x; bi->MouseY = y;
    bi->SetSpritePosition(bi, x, y, bi->RGBFormat);
}

static BOOL select_formats(struct P96Priv *p, struct PrismBoard *b)
{
    /* Advertise each accepted format. A format that needs its own aperture,
     * a memory-mode switch or no direct access puts the board into shadow
     * mode (bitmap.c uploads only the changed spans). Retain a first
     * choice per byte depth only for the legacy diagnostic callbacks. */
    static const UBYTE order[] = {
        PF_CLUT8, PF_RGB565BE, PF_BGR565LE, PF_RGB565LE,
        PF_BGR24, PF_RGB24, PF_BGRA32, PF_ARGB32, PF_RGBA32
    };
    struct BoardInfo *bi = &p->bi;
    ULONG accepted = 0, compatible = ~0UL;
    ULONG i;
    for (i = 0; i < COUNT(order); i++) {
        UBYTE pf = order[i], bpp = rgb_bpp(rgbformat[pf]);
        RGBFTYPE f = rgbformat[pf];
        ULONG bit = 1UL << f, mask;
        UBYTE *base,*end;
        if (!(bi->RGBFormats & bit)) continue;
        mask = bi->GetCompatibleFormats(bi, f);
        if (!(mask & bit)) continue;
        base=bi->CalculateMemory(bi,bi->MemoryBase,NULL,f);
        end=bi->CalculateMemory(bi,bi->MemoryBase+bi->MemorySize-1,NULL,f);
        /* A remapped window must be consumed one translation at a time. */
        if (!base || !end) continue;
        if ((ULONG)end-(ULONG)base != bi->MemorySize-1)
            b->flags |= PBF_BANKED | PBF_SHADOW;
        if (((mask & accepted)!=accepted || !(compatible & bit)) && !bi->SetMemoryMode)
            continue;
        if (!(bi->Flags & BIF_GRANTDIRECTACCESS) || base!=bi->MemoryBase ||
            (mask & accepted)!=accepted || !(compatible & bit))
            b->flags |= PBF_SHADOW;
        if (p->pf[bpp]==PF_COUNT) p->pf[bpp] = pf;
        b->formats |= PF_BIT(pf);
        accepted |= bit;
        compatible &= mask;
    }
    p->formats=b->formats;
    return accepted != 0;
}

static BOOL attach_board(struct P96Priv *p, struct PrismBoard *b)
{
    struct BoardInfo *bi = &p->bi;
    if (!bi->MemoryBase || bi->MemorySize < 65536 || !bi->SetGC ||
        !bi->SetDAC || !bi->SetPanning || !bi->SetSwitch || !bi->SetDisplay ||
        !bi->SetColorArray || !bi->CalculateBytesPerRow ||
        !bi->CalculateMemory || !bi->GetCompatibleFormats) {
        puts("PrismD: P96 driver lacks required framebuffer operations");
        return FALSE;
    }
    if (!bi->AllocCardMem || !bi->FreeCardMem) {
        puts("PrismD: P96 allocator requires an allocation/free pair");
        return FALSE;
    }
    memset(b, 0, sizeof(*b));
    if (!select_formats(p, b)) {
        puts("PrismD: P96 driver has no CPU-mapped pixel formats");
        return FALSE;
    }
    if (p->card && p->card->lib_IdString &&
        !strncmp(p->card->lib_IdString,"UAE Graphics Card",17) &&
        !rtg_probe_planar(bi, b->formats)) bi->BlitPlanar2Direct = rtg_planar_direct;
    b->ops = &p96_alloc_ops;
    if (bi->AllocCardMem != card_alloc || bi->FreeCardMem != card_free ||
        bi->SoftSpriteFlags || bi->EnableSoftSprite != soft_sprite) b->flags |= PBF_SHADOW;
    if (bi->ReInitMemory && bi->ReInitMemory != card_reinit)
        b->flags |= PBF_SHADOW | PBF_REINIT;
    b->priv = p;
    b->name = bi->BoardName ? bi->BoardName : p->name;
    b->vram = bi->MemoryBase; b->vramSize = bi->MemorySize;
    b->regs = bi->RegisterBase;
    b->maxWidth = bi->MaxBMWidth < 65536 ? bi->MaxBMWidth : 65535;
    b->maxHeight = bi->MaxBMHeight < 65536 ? bi->MaxBMHeight : 65535;
    b->checkMode = p96_check; b->setMode = p96_mode;
    b->setDisplayStart = p96_pan; b->setPalette = p96_palette;
    b->setSwitch = p96_switch; b->waitVBlank = p96_vblank;
    b->shutdown = p96_shutdown;
    b->waitBlit = p96_wait; b->bytesPerRow = p96_pitch;
    if (bi->WaitBlitter && !(bi->Flags & BIF_NOBLITTER)) {
        if (bi->FillRect && bi->FillRect != cpu_fill) {
            b->fillRect = p96_fill;
            b->flags |= PBF_BLIT_32;
        }
        if (bi->BlitRect && bi->BlitRect != cpu_copy)
            b->copyRect = p96_copy;
        if (bi->BlitTemplate && bi->BlitTemplate != rtg_template_default) {
            b->expandRect = p96_expand;
            b->flags |= PBF_BLIT_32;
        }
    }
    if ((bi->Flags & BIF_HARDWARESPRITE) && bi->SetSprite && bi->SetSpriteImage &&
        bi->SetSpriteColor && bi->SetSpritePosition) {
        b->cursorImage = p96_cursor_image; b->cursorShow = p96_cursor_show;
        b->cursorMove = p96_cursor_move;
        b->flags |= PBF_HW_CURSOR;
    } else {
        puts("PrismD: P96 driver has no usable hardware cursor");
    }
    return TRUE;
}

BOOL P96_Probe(struct PrismBoard *b, const char *card, const char *monitor)
{
    struct P96Priv *p;
    STRPTR *tt;
    BOOL claimed;
    if (!card || !*card) {
        puts("PrismD: BOARD=P96 requires P96CARD=<card library path>");
        return FALSE;
    }
    Forbid();
    claimed = FindSemaphore(P96_OWNER) != NULL;
    Permit();
    if (claimed) {
        puts("PrismD: a Prism P96 driver is already resident; reboot before restarting");
        return FALSE;
    }
    p = AllocVec(sizeof(*p), MEMF_PUBLIC | MEMF_CLEAR);
    if (!p) return FALSE;
    tt = p->emptyToolTypes;
    snprintf(p->name, sizeof(p->name), "P96: %s", card);
    if (monitor && *monitor) {
        struct Library *IconBase = p->icon = OpenLibrary("icon.library", 37);
        if (!IconBase || !(p->disk = GetDiskObject((STRPTR)monitor))) {
            printf("PrismD: cannot read P96 monitor icon %s.info\n", monitor);
            goto fail;
        }
        if (p->disk->do_ToolTypes) tt = p->disk->do_ToolTypes;
    }
    p->card = OpenLibrary((STRPTR)card, 0);
    if (!p->card || p->card->lib_NegSize < 36) {
        printf("PrismD: cannot open P96 card driver %s\n", card);
        goto fail;
    }
    init_boardinfo(p);
    InitSemaphore(&p->owner);
    p->owner.ss_Link.ln_Name = (STRPTR)P96_OWNER;
    /* Serialize probing too: two Prism processes must never claim cards
     * against the same instance of an external driver. */
    Forbid();
    claimed = FindSemaphore(P96_OWNER) != NULL;
    if (!claimed) AddSemaphore(&p->owner);
    Permit();
    if (claimed) goto fail;
    claimed = card_FindCard(p->card, &p->bi, tt);
    if (!claimed) {
        RemSemaphore(&p->owner);
        puts("PrismD: P96 driver found no unclaimed card");
        goto fail;
    }
    resident = p;
    p->initialized = card_InitCard(p->card, &p->bi, tt);
    if (!p->initialized) {
        puts("PrismD: P96 card initialization failed");
        return FALSE;
    }
    if (p->bi.SetInterrupt) p->bi.SetInterrupt(&p->bi, FALSE);
    return attach_board(p, b);

fail:
    if (p->card) CloseLibrary(p->card);
    if (p->icon) {
        struct Library *IconBase = p->icon;
        if (p->disk) FreeDiskObject(p->disk);
        CloseLibrary(IconBase);
    }
    FreeVec(p);
    return FALSE;
}

void P96_KeepResident(void)
{
    struct P96Priv *p = resident;
    if (!p) return;
    quiesce(p);
    puts("PrismD: P96 driver remains resident; reboot to release or restart it");
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
    return P96_Probe(b, config->card, config->monitor);
}
void driver_retain(void)
{
    P96_KeepResident();
}
#endif
