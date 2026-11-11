/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Ferenc Boldog */
/* shim_card.c - a test P96 card for shadow_guest: passes every call to
 * UAE's resident uaegfx.card, then changes one thing after InitCard, so
 * the P96 adapter's shadow-mode rules (drv_p96.c attach_board) can be run
 * on a stock emulator. tests/architecture.sh builds both:
 *   -DSHIM_SOFTSPRITE: SoftSpriteFlags = CLUT | R5G6B5   (softsprite.card):
 *                      a software pointer, no shadow mode (1.4b1: shadow)
 *   -DSHIM_OWNFREE:    a FreeCardMem of its own that only jumps on to the
 *                      one it replaced (ownfree.card): shadow mode
 * The romtag stays near the start: 3.1.4's ramlib did not find it at the
 * end of the file (OpenLibrary failed, InitResident on it worked). Built
 * with -nostartfiles -nostdlib -fno-toplevel-reorder; the first code returns. */
#include <exec/types.h>
#include <exec/libraries.h>
#include <exec/resident.h>
#include <exec/initializers.h>
#include <proto/exec.h>
#include "p96sdk/boardinfo.h"
#include "cardcall.h"

asm(".text\n\tmoveq #-1,d0\n\trts\n");

#ifdef SHIM_SOFTSPRITE
#define NAME "softsprite.card"
#else
#define NAME "ownfree.card"
#endif
extern const ULONG inittab[4];
extern char shim_end[];
const struct Resident __attribute__((section(".text"))) romtag = { RTC_MATCHWORD, (struct Resident *)&romtag, (APTR)&shim_end,
                                 RTF_AUTOINIT, 1, NT_LIBRARY, 0, (char *)NAME,
                                 (char *)"shim card 1.0\r\n", (APTR)inittab };


struct ExecBase *SysBase;
static struct Library *real;
APTR shim_orig_free;

/* jumps to the FreeCardMem it replaced with every register as it came */
asm(".text\n.globl _shim_free\n_shim_free:\n\tmove.l _shim_orig_free,-(sp)\n\trts\n");
extern char shim_free[];

static struct Library *l_open(struct Library *b __asm("a6"))
{
    b->lib_OpenCnt++;
    return b;
}
static ULONG l_close(struct Library *b __asm("a6"))
{
    b->lib_OpenCnt--;
    return 0;
}
static ULONG l_null(void) { return 0; }

static LONG l_find(struct BoardInfo *bi __asm("a0"), APTR a1 __asm("a1"))
{
    if (!real && !(real = OpenLibrary((STRPTR)"uaegfx.card", 0)))
        return 0;
    return card_lvo(real, -30, bi, a1);
}

static LONG l_init(struct BoardInfo *bi __asm("a0"), APTR tt __asm("a1"))
{
    BOOL ok = card_lvo(real, -36, bi, tt);
    if (ok) {
#ifdef SHIM_SOFTSPRITE
        bi->SoftSpriteFlags = RGBFF_CLUT | RGBFF_R5G6B5;
#else
        shim_orig_free = (APTR)bi->FreeCardMem;
        bi->FreeCardMem = (APTR)shim_free;
#endif
    }
    return ok;
}

static struct Library *l_initlib(struct Library *b __asm("d0"), BPTR seg __asm("a0"),
                                 struct ExecBase *sys __asm("a6"))
{
    SysBase = sys;
    b->lib_Node.ln_Type = NT_LIBRARY;
    b->lib_Node.ln_Name = (char *)NAME;
    b->lib_Flags = LIBF_SUMUSED | LIBF_CHANGED;
    b->lib_Version = 1;
    b->lib_IdString = (APTR)"UAE Graphics Card shim (" NAME ", PrismRTG test)\r\n";
    return b;
}

static const APTR funcs[] = { (APTR)l_open, (APTR)l_close, (APTR)l_null, (APTR)l_null,
                              (APTR)l_find, (APTR)l_init, (APTR)-1 };
const ULONG inittab[4] = { sizeof(struct Library), (ULONG)funcs, 0, (ULONG)l_initlib };

asm(".text\n.globl _shim_end\n_shim_end:\n");
