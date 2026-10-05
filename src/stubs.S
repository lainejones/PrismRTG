| SPDX-License-Identifier: GPL-3.0-only
| Copyright (C) 2026 Laine Jones
| stubs.S - register-saving trampolines for PrismD's graphics patches.
|
| stub_X: save d0-d7/a0-a6, call h_X(struct Regs *). If it returns 0, put
| every register back and continue in the original function (o_X); else
| put the registers back from the frame (r->d[0] = the result) and return.

        .text

        .macro BODY name
        movem.l d0-d7/a0-a6,-(sp)
        move.l  sp,-(sp)
        jsr     _h_\name
        addq.l  #4,sp
        tst.l   d0
        bne.s   1f
        movem.l (sp)+,d0-d7/a0-a6
        move.l  _o_\name,-(sp)
        rts
| handled: the C code kept d2-d7/a2-a6, so only d0 (the result), d1, a0
| and a1 are read back - a memory read is the expensive part of a call
1:      move.l  (sp),d0
        move.l  4(sp),d1
        movem.l 32(sp),a0-a1
        lea     60(sp),sp
        rts
        .endm

        .macro PATCH name
        .globl _stub_\name
_stub_\name:
        BODY \name
        .endm

| Most calls draw into bitmaps that aren't Prism's (every native screen).
| These variants look at the bitmap's flag byte first - Prism bitmaps have
| bit 7 (BMF_PRISM) set - and go straight to the original function without
| saving anything when it is clear. A set bit only means "ask the handler".
| (A NULL pointer here reads low memory, harmlessly.)

| RastPort in rp; tmp is any other address register
        .macro PATCH_RP name, rp, tmp
        .globl _stub_\name
_stub_\name:
        move.l  \tmp,-(sp)
        movea.l 4(\rp),\tmp
        btst    #7,4(\tmp)
        bne.s   2f
| ...or a planar bitmap whose first plane is not in chip RAM: the blitter
| can't draw there, the handler does it (render.c, "planar bitmaps outside
| chip RAM")
        movea.l 8(\tmp),\tmp
        cmpa.l  _chipTop,\tmp
        movea.l (sp)+,\tmp
        bcc.s   3f
        move.l  _o_\name,-(sp)
        rts
2:      movea.l (sp)+,\tmp
3:      BODY \name
        .endm

| BitMap in a0
        .macro PATCH_BM name
        .globl _stub_\name
_stub_\name:
        btst    #7,4(a0)
        bne.s   3f
        move.l  _o_\name,-(sp)
        rts
3:      BODY \name
        .endm

| BltBitMap: source BitMap in a0, destination in a1
        .globl _stub_BltBitMap
_stub_BltBitMap:
        btst    #7,4(a0)
        bne.s   3f
        btst    #7,4(a1)
        bne.s   3f
        move.l  d0,-(sp)
        move.l  8(a0),d0                | either first plane above chip RAM?
        cmp.l   _chipTop,d0
        bcc.s   2f
        move.l  8(a1),d0
        cmp.l   _chipTop,d0
        bcc.s   2f
        move.l  (sp)+,d0
        move.l  _o_BltBitMap,-(sp)
        rts
2:      move.l  (sp)+,d0
3:      BODY BltBitMap

        PATCH_RP BltBitMapRastPort, a1, a0
        PATCH_RP ClipBlit, a1, a0
        PATCH_RP BltMaskBitMapRastPort, a1, a0
        PATCH_RP BltTemplate, a1, a0
        PATCH_RP BltPattern, a1, a0
        PATCH_RP RectFill, a1, a0
        PATCH_RP Text, a1, a0
        PATCH_RP Draw, a1, a0
        PATCH_RP PolyDraw, a1, a0
| WritePixel(rp a1, x d0, y d1): the plain case goes to f_WritePixel(rp, x,
| y) with nothing but its arguments saved. It returns the result (0 or -1),
| or 2 for "not one of mine": then the full handler takes over.
        .globl _stub_WritePixel
_stub_WritePixel:
        move.l  a0,-(sp)
        movea.l 4(a1),a0
        btst    #7,4(a0)
        bne.s   3f
        movea.l 8(a0),a0                | planar, not in chip RAM: the handler
        cmpa.l  _chipTop,a0
        bcc.s   3f
        movea.l (sp)+,a0
        move.l  _o_WritePixel,-(sp)
        rts
3:      move.l  d1,-(sp)
        move.l  d0,-(sp)
        move.l  a1,-(sp)
        jsr     _f_WritePixel
        movea.l (sp)+,a1
        subq.l  #2,d0
        beq.s   4f
        addq.l  #2,d0
        addq.l  #4,sp
        move.l  (sp)+,d1
        movea.l (sp)+,a0
        rts
4:      move.l  (sp)+,d0
        move.l  (sp)+,d1
        movea.l (sp)+,a0
        BODY WritePixel
        PATCH_RP ReadPixel, a1, a0
        PATCH_RP SetRast, a1, a0
        PATCH_RP ScrollRaster, a1, a0
        PATCH_RP ScrollRasterBF, a1, a0
        PATCH_RP EraseRect, a1, a0
        PATCH AllocBitMap
        PATCH BitMapScale
        PATCH_BM FreeBitMap
        PATCH_BM GetBitMapAttr
        PATCH_RP WriteChunkyPixels, a0, a1
        PATCH_RP WritePixelArray8, a0, a1
        PATCH_RP ReadPixelArray8, a0, a1
        PATCH_RP WritePixelLine8, a0, a1
        PATCH_RP ReadPixelLine8, a0, a1

        PATCH SetRGB32CM
        PATCH ObtainPen
        PATCH ObtainBestPenA
        PATCH MoveSprite
        PATCH ChangeSprite
        PATCH ChangeExtSpriteA
        PATCH ChangeVPBitMap
        PATCH CoerceMode

| LONG call_regs(APTR fn, struct Regs *r): call fn with every register
| loaded from *r (the original function, from a handler that wants to run
| it first and act afterwards); its d0 goes back into r->d[0] and is
| returned.
        .globl _call_regs
_call_regs:
        movem.l d2-d7/a2-a6,-(sp)
        move.l  48(sp),a1
        move.l  52(sp),a0
        move.l  a0,-(sp)
        pea     1f(pc)
        move.l  a1,-(sp)
        movem.l (a0),d0-d7/a0-a6
        rts
1:      move.l  (sp)+,a1
        move.l  d0,(a1)
        movem.l (sp)+,d2-d7/a2-a6
        rts
