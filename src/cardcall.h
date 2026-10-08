/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laine Jones */
/*
 * cardcall.h - FindCard (-30) and InitCard (-36) of a P96 card library,
 * for drv_p96.c and drv_uaegfx.c.
 *
 * Written as explicit library calls, not as a C call through
 * ((UBYTE *)lib - 30) cast to a function with register arguments: amiga-gcc
 * 6.5.0b turns that into "jsr (30,a6)" - lib PLUS 30, into the library's
 * data - and PrismD died with error 80000004 the moment it called UAE's
 * uaegfx.card (WinUAE 6.0.3, 2026-10-07).
 */
#ifndef CARDCALL_H
#define CARDCALL_H

#include <exec/types.h>
#include <exec/libraries.h>

static inline BOOL card_lvo(struct Library *lib, WORD lvo, APTR bi, APTR a1arg)
{
    register LONG d0 __asm("d0");
    register APTR a0 __asm("a0") = bi;
    register APTR a1 __asm("a1") = a1arg;
    register struct Library *a6 __asm("a6") = lib;

    if (lvo == -30)
        __asm volatile ("jsr a6@(-30:W)" : "=r"(d0), "+r"(a0), "+r"(a1), "+r"(a6) : : "d1", "cc", "memory");
    else
        __asm volatile ("jsr a6@(-36:W)" : "=r"(d0), "+r"(a0), "+r"(a1), "+r"(a6) : : "d1", "cc", "memory");
    return (BOOL)d0;                   /* the card ABI's BOOL is a WORD */
}

/* FindCard(BoardInfo a0) - a1 is passed along for drivers that read it */
#define card_FindCard(lib, bi, a1arg) card_lvo((lib), -30, (bi), (a1arg))
/* InitCard(BoardInfo a0, ToolTypes a1) */
#define card_InitCard(lib, bi, tt)    card_lvo((lib), -36, (bi), (tt))

#endif
