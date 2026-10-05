/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Laine Jones */
/*
 * p96stub.S - the file LIBS:Picasso96API.library that Prism installs.
 *
 * The working library lives inside PrismD, which adds it to the system in
 * memory when it starts; OpenLibrary() finds that one and never loads this
 * file. The file is here for two cases: programs that only check that it
 * exists (PerfectPaint), and a machine where PrismD isn't running - then
 * this is what gets loaded, and its init routine returns 0, so
 * OpenLibrary() fails cleanly ("no RTG system").
 *
 * Nothing in it comes from Picasso96: it is a resident tag and three
 * instructions.
 */
        .text

        moveq   #-1,d0                  /* run from a Shell: nothing to do */
        rts

romtag:
        .word   0x4afc                  /* RTC_MATCHWORD                  */
        .long   romtag
        .long   endskip
        .byte   0                       /* flags: no auto-init            */
        .byte   2                       /* version                        */
        .byte   9                       /* NT_LIBRARY                     */
        .byte   0                       /* priority                       */
        .long   name
        .long   idstring
        .long   init

name:
        .asciz  "Picasso96API.library"
idstring:
        .asciz  "Picasso96API.library 2.500 (4.10.2026) Prism (stub: PrismD is not running)\r\n"
        .even

/* init(d0 = library base (none), a0 = segment list, a6 = exec):
 * no library - LoadSeg'd code is unloaded again by ramlib */
init:
        moveq   #0,d0
        rts
endskip:
