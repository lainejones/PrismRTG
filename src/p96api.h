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

/* P96 memory-window tags and errors. */
#define P96PIP_Dummy				(TAG_USER + 0x30000 + 96)
#define P96PIP_SourceFormat	(P96PIP_Dummy+1)	/* RGBFTYPE (I) */
#define P96PIP_SourceBitMap	(P96PIP_Dummy+2)	/* struct BitMap * (G) */
#define P96PIP_SourceRPort		(P96PIP_Dummy+3)	/* struct RastPort * (G) */
#define P96PIP_SourceWidth		(P96PIP_Dummy+4)	/* ULONG (I) */
#define P96PIP_SourceHeight	(P96PIP_Dummy+5)	/* ULONG (I) */
#define P96PIP_Type				(P96PIP_Dummy+6)	/* ULONG (I) default: PIPT_MemoryWindow */
#define P96PIP_ErrorCode		(P96PIP_Dummy+7)	/* LONG* (I) */
#define P96PIP_Brightness		(P96PIP_Dummy+8)	/* ULONG (IGS) default: 0 */
#define P96PIP_Left				(P96PIP_Dummy+9)	/* ULONG (I) default: 0 */
#define P96PIP_Top				(P96PIP_Dummy+10)	/* ULONG (I) default: 0 */
#define P96PIP_Width				(P96PIP_Dummy+11)	/* ULONG (I) default: inner width of window */
#define P96PIP_Height			(P96PIP_Dummy+12)	/* ULONG (I) default: inner height of window */
#define P96PIP_Relativity		(P96PIP_Dummy+13)	/* ULONG (I) default: PIPRel_Width|PIPRel_Height */
#define P96PIP_Colors			(P96PIP_Dummy+14)	/* struct ColorSpec * (IS)
																 * ti_Data is an array of struct ColorSpec,
																 * terminated by ColorIndex = -1.  Specifies
																 * initial screen palette colors.
																 * Also see P96PIP_Colors32.
																 * This only works with CLUT PIPs on non-CLUT
																 * screens. For CLUT PIPs on CLUT screens the
																 * PIP colors share the screen palette.
																 */
#define P96PIP_Colors32			(P96PIP_Dummy+15)	/* ULONG* (IS)
																 * Tag to set the palette colors at 32 bits-per-gun.
																 * ti_Data is a pointer * to a table to be passed to
																 * the graphics.library/LoadRGB32() function.
																 * This format supports both runs of color
																 * registers and sparse registers.  See the
																 * autodoc for that function for full details.
																 * Any color set here has precedence over
																 * the same register set by P96PIP_Colors.
																 * This only works with CLUT PIPs on non-CLUT
																 * screens. For CLUT PIPs on CLUT screens the
																 * PIP colors share the screen palette.
																 */
#define P96PIP_NoMemory						(P96PIP_Dummy+16)
#define P96PIP_RenderFunc					(P96PIP_Dummy+17)
#define P96PIP_SaveFunc						(P96PIP_Dummy+18)
#define P96PIP_UserData						(P96PIP_Dummy+19)
#define P96PIP_Alignment					(P96PIP_Dummy+20)
#define P96PIP_ConstantBytesPerRow		(P96PIP_Dummy+21)
#define P96PIP_AllowCropping				(P96PIP_Dummy+22)
#define P96PIP_InitialIntScaling			(P96PIP_Dummy+23)
#define P96PIP_ClipLeft						(P96PIP_Dummy+24)	/* ULONG (IS) */
#define P96PIP_ClipTop						(P96PIP_Dummy+25)	/* ULONG (IS) */
#define P96PIP_ClipWidth					(P96PIP_Dummy+26)	/* ULONG (IS) */
#define P96PIP_ClipHeight					(P96PIP_Dummy+27)	/* ULONG (IS) */
#define P96PIP_ConstantByteSwapping		(P96PIP_Dummy+28)

enum {
	PIPT_MemoryWindow,		/* default */
	PIPT_VideoWindow,
	PIPT_NUMTYPES
};

#define	P96PIPT_MemoryWindow	PIPT_MemoryWindow
#define	P96PIPT_VideoWindow	PIPT_VideoWindow

#define	PIPRel_Right		1	/* P96PIP_Left is relative to the right side (negative value) */
#define	PIPRel_Bottom		2	/* P96PIP_Top is relative to the bottom (negative value) */
#define	PIPRel_Width		4	/* P96PIP_Width is amount of pixels not used by PIP at the
										   right side of the window (negative value) */
#define	PIPRel_Height		8	/* P96PIP_Height is amount of pixels not used by PIP at the
										   window bottom (negative value) */

#define	PIPERR_NOMEMORY		(1)	/* couldn't get normal memory */
#define	PIPERR_ATTACHFAIL		(2)	/* Failed to attach to a screen */
#define	PIPERR_NOTAVAILABLE	(3)	/* PIP not available for other reason	*/
#define	PIPERR_OUTOFPENS		(4)	/* couldn't get a free pen for occlusion */
#define	PIPERR_BADDIMENSIONS	(5)	/* type, width, height or format invalid */
#define	PIPERR_NOWINDOW		(6)	/* couldn't open window */
#define	PIPERR_BADALIGNMENT	(7)	/* specified alignment is not ok */
#define	PIPERR_CROPPED			(8)	/* pip would be cropped, but isn't allowed to */

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
