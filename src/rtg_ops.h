/* SPDX-License-Identifier: GPL-3.0-only */
/* P96 wire ABI shared by the card adapter and UAE firmware backend.
 * BoardInfo is the first field in their private state. The board lock
 * serializes operations, including the shared planar colour table. */
#ifndef PRISM_RTG_OPS_H
#define PRISM_RTG_OPS_H
#include "boardops.h"
#pragma pack(push, 2)
#include "p96sdk/boardinfo.h"
#pragma pack(pop)

void rtg_planar_chunky(struct BoardInfo *bi __asm("a0"),struct BitMap *bm __asm("a1"),
    struct RenderInfo *ri __asm("a2"),WORD sx __asm("d0"),WORD sy __asm("d1"),
    WORD dx __asm("d2"),WORD dy __asm("d3"),WORD w __asm("d4"),WORD h __asm("d5"),
    UBYTE mt __asm("d6"),UBYTE mask __asm("d7"));
void rtg_planar_direct(struct BoardInfo *bi __asm("a0"),struct BitMap *bm __asm("a1"),
    struct RenderInfo *ri __asm("a2"),struct ColorIndexMapping *cim __asm("a3"),
    WORD sx __asm("d0"),WORD sy __asm("d1"),WORD dx __asm("d2"),WORD dy __asm("d3"),
    WORD w __asm("d4"),WORD h __asm("d5"),UBYTE mt __asm("d6"),UBYTE mask __asm("d7"));
void rtg_template_default(struct BoardInfo *bi __asm("a0"),struct RenderInfo *ri __asm("a1"),
    struct Template *t __asm("a2"),WORD x __asm("d0"),WORD y __asm("d1"),WORD w __asm("d2"),
    WORD h __asm("d3"),UBYTE mask __asm("d4"),RGBFTYPE fmt __asm("d7"));
enum PrismResult rtg_fill(struct PrismBoard *b,const struct PrismSurface *d,
    UWORD x,UWORD y,UWORD w,UWORD h,ULONG c);
enum PrismResult rtg_copy(struct PrismBoard *b,const struct PrismSurface *s,
    const struct PrismSurface *d,UWORD sx,UWORD sy,UWORD dx,UWORD dy,UWORD w,UWORD h);
enum PrismResult rtg_expand(struct PrismBoard *b,const struct PrismSurface *d,
    UWORD x,UWORD y,UWORD w,UWORD h,const UBYTE *src,ULONG mod,ULONG fg,ULONG bg,BOOL tr);
enum PrismResult rtg_planar(struct PrismBoard *b,const struct PrismPlanar *s,
    const struct PrismSurface *d,UWORD sx,UWORD sy,UWORD dx,UWORD dy,UWORD w,UWORD h,
    UBYTE mt,UBYTE mask);
ULONG rtg_pitch(struct PrismBoard *b,UWORD w,UWORD h,UBYTE f);
BOOL rtg_read(struct PrismBoard *b,const struct PrismSurface *s,ULONG off,APTR mem,ULONG size);
BOOL rtg_write(struct PrismBoard *b,const struct PrismSurface *s,ULONG off,const void *mem,ULONG size);
BOOL rtg_probe_planar(struct BoardInfo *bi, ULONG formats);
extern const struct PrismOps rtg_ops;
#endif
