/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laine Jones */
/*
 * p96api.h - the interface of "Picasso96API.library" as Prism provides it.
 *
 * Prism has its own library of that name so that programs written for the
 * Picasso96 API find one. Nothing here comes from Picasso96 itself: these
 * are the interface's published numbers (function order, tag values, pixel
 * format codes, structure layouts) written down for this implementation,
 * which is Prism code from top to bottom.
 */
#ifndef PRISM_P96API_H
#define PRISM_P96API_H

#include <exec/types.h>
#include <exec/nodes.h>
#include <utility/tagitem.h>

/* pixel formats ("RGBFTYPE") and their bit masks */
enum {
    RGBFB_NONE,         /* planar                                        */
    RGBFB_CLUT,         /* 1 byte, palette                               */
    RGBFB_R8G8B8,       /* 3 bytes R,G,B                                 */
    RGBFB_B8G8R8,       /* 3 bytes B,G,R                                 */
    RGBFB_R5G6B5PC,     /* 2 bytes, low byte first                       */
    RGBFB_R5G5B5PC,
    RGBFB_A8R8G8B8,     /* 4 bytes A,R,G,B                               */
    RGBFB_A8B8G8R8,     /* 4 bytes A,B,G,R                               */
    RGBFB_R8G8B8A8,     /* 4 bytes R,G,B,A                               */
    RGBFB_B8G8R8A8,     /* 4 bytes B,G,R,A                               */
    RGBFB_R5G6B5,       /* 2 bytes, high byte first                      */
    RGBFB_R5G5B5,
    RGBFB_B5G6R5PC,     /* 2 bytes, low byte first, blue on top          */
    RGBFB_B5G5R5PC,
    RGBFB_Y4U2V2,
    RGBFB_Y4U1V1,
    RGBFB_MaxFormats
};
#define RGBFF(f) (1UL << (f))

struct P96RenderInfo {
    APTR  Memory;
    WORD  BytesPerRow;
    WORD  pad;
    ULONG RGBFormat;
};

struct P96TrueColorInfo {
    ULONG  PixelDistance, BytesPerRow;
    UBYTE *RedData, *GreenData, *BlueData;
};

#define P96_MODENAMELENGTH 48
struct P96ModeNode {
    struct Node Node;
    char   Description[P96_MODENAMELENGTH];
    UWORD  Width, Height, Depth;
    ULONG  DisplayID;
};

/* p96GetBitMapAttr */
enum {
    P96BMA_WIDTH, P96BMA_HEIGHT, P96BMA_DEPTH, P96BMA_MEMORY, P96BMA_BYTESPERROW,
    P96BMA_BYTESPERPIXEL, P96BMA_BITSPERPIXEL, P96BMA_RGBFORMAT, P96BMA_ISP96,
    P96BMA_ISONBOARD, P96BMA_BOARDMEMBASE, P96BMA_BOARDIOBASE, P96BMA_BOARDMEMIOBASE
};

/* p96GetModeIDAttr */
enum {
    P96IDA_WIDTH, P96IDA_HEIGHT, P96IDA_DEPTH, P96IDA_BYTESPERPIXEL, P96IDA_BITSPERPIXEL,
    P96IDA_RGBFORMAT, P96IDA_ISP96, P96IDA_BOARDNUMBER, P96IDA_STDBYTESPERROW,
    P96IDA_BOARDNAME, P96IDA_COMPATIBLEFORMATS, P96IDA_VIDEOCOMPATIBLE,
    P96IDA_PABLOIVCOMPATIBLE, P96IDA_PALOMAIVCOMPATIBLE
};

/* p96BestModeIDTagList */
#define P96BIDTAG_Dummy            (TAG_USER + 96)
#define P96BIDTAG_FormatsAllowed   (P96BIDTAG_Dummy + 1)
#define P96BIDTAG_FormatsForbidden (P96BIDTAG_Dummy + 2)
#define P96BIDTAG_NominalWidth     (P96BIDTAG_Dummy + 3)
#define P96BIDTAG_NominalHeight    (P96BIDTAG_Dummy + 4)
#define P96BIDTAG_Depth            (P96BIDTAG_Dummy + 5)

/* p96RequestModeIDTagList / p96AllocModeListTagList */
#define P96MA_Dummy            (TAG_USER + 0x10000 + 96)
#define P96MA_MinWidth         (P96MA_Dummy + 1)
#define P96MA_MinHeight        (P96MA_Dummy + 2)
#define P96MA_MinDepth         (P96MA_Dummy + 3)
#define P96MA_MaxWidth         (P96MA_Dummy + 4)
#define P96MA_MaxHeight        (P96MA_Dummy + 5)
#define P96MA_MaxDepth         (P96MA_Dummy + 6)
#define P96MA_DisplayID        (P96MA_Dummy + 7)
#define P96MA_FormatsAllowed   (P96MA_Dummy + 8)
#define P96MA_FormatsForbidden (P96MA_Dummy + 9)
#define P96MA_WindowTitle      (P96MA_Dummy + 10)
#define P96MA_OKText           (P96MA_Dummy + 11)
#define P96MA_CancelText       (P96MA_Dummy + 12)
#define P96MA_Window           (P96MA_Dummy + 13)
#define P96MA_PubScreenName    (P96MA_Dummy + 14)
#define P96MA_Screen           (P96MA_Dummy + 15)

/* p96OpenScreenTagList: the first 28 follow Intuition's SA_ tags one for
 * one in meaning (not in number); the rest are this API's own */
#define P96SA_Dummy       (TAG_USER + 0x20000 + 96)
enum {
    P96SA_Left = P96SA_Dummy + 1, P96SA_Top, P96SA_Width, P96SA_Height, P96SA_Depth,
    P96SA_DetailPen, P96SA_BlockPen, P96SA_Title, P96SA_Colors, P96SA_ErrorCode,
    P96SA_Font, P96SA_SysFont, P96SA_Type, P96SA_BitMap, P96SA_PubName, P96SA_PubSig,
    P96SA_PubTask, P96SA_DisplayID, P96SA_DClip, P96SA_ShowTitle, P96SA_Behind,
    P96SA_Quiet, P96SA_AutoScroll, P96SA_Pens, P96SA_SharePens, P96SA_BackFill,
    P96SA_Colors32, P96SA_VideoControl, P96SA_RGBFormat, P96SA_NoSprite, P96SA_NoMemory,
    P96SA_RenderFunc, P96SA_SaveFunc, P96SA_UserData, P96SA_Alignment, P96SA_FixedScreen,
    P96SA_Exclusive, P96SA_ConstantBytesPerRow, P96SA_ConstantByteSwapping
};

/* picture-in-picture windows (not available on Prism's boards) */
#define P96PIP_Dummy      (TAG_USER + 0x30000 + 96)
#define P96PIP_ErrorCode  (P96PIP_Dummy + 7)
#define PIPERR_NOTAVAILABLE 3

/* p96GetRTGDataTagList / p96GetBoardDataTagList */
#define P96RD_Dummy              (TAG_USER + 0x40000 + 96)
#define P96RD_NumberOfBoards     (P96RD_Dummy + 1)
#define P96BD_Dummy              (TAG_USER + 0x50000 + 96)
#define P96BD_BoardName          (P96BD_Dummy + 1)
#define P96BD_ChipName           (P96BD_Dummy + 2)
#define P96BD_TotalMemory        (P96BD_Dummy + 4)
#define P96BD_FreeMemory         (P96BD_Dummy + 5)
#define P96BD_LargestFreeMemory  (P96BD_Dummy + 6)
#define P96BD_MonitorSwitch      (P96BD_Dummy + 7)
#define P96BD_RGBFormats         (P96BD_Dummy + 8)
#define P96BD_MemoryClock        (P96BD_Dummy + 9)

#define P96_BMF_USERPRIVATE 0x8000      /* p96AllocBitMap flag: never on the board */

#endif
