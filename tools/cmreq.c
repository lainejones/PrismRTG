/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Laine Jones */
/* cmreq: open cybergraphics.library's mode requester and print the answer.
 * cmreq [mindepth [maxdepth]] */
#include <exec/libraries.h>
#include <utility/tagitem.h>
#include <cybergraphx/cybergraphics.h>
#include <proto/exec.h>
#include <proto/cybergraphics.h>
#include <stdio.h>
#include <stdlib.h>

struct Library *CyberGfxBase;

int main(int argc, char **argv)
{
    struct TagItem tags[] = {
        { CYBRMREQ_MinDepth, argc > 1 ? atoi(argv[1]) : 8 },
        { CYBRMREQ_MaxDepth, argc > 2 ? atoi(argv[2]) : 32 },
        { CYBRMREQ_WinTitle, (ULONG)"cmreq: pick a mode" },
        { TAG_DONE, 0 }
    };
    ULONG id;

    if (!(CyberGfxBase = OpenLibrary("cybergraphics.library", 40))) {
        puts("cmreq: no cybergraphics.library");
        return 20;
    }
    id = CModeRequestTagList(NULL, tags);
    printf("cmreq: mode %08lx\n", (unsigned long)id);
    CloseLibrary(CyberGfxBase);
    return 0;
}
