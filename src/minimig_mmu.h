/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laine Jones */
/*
 * minimig_mmu.h - the MiSTer Minimig's fixed RTG windows under mmu.library.
 *
 * Cores without the Zorro III RTG board have the registers at $B80100 and
 * the frame buffer at $02000000, in no autoconfig node. With the MMU on
 * (68040/68060.library of MMULib, e.g. on the AP060 core's 68060)
 * mmu.library builds its tables from what it knows and leaves these two
 * windows blank: reads give 0, writes go nowhere. mm_mmu_map() makes them
 * valid, cache-inhibited, unmoved pages in the user and supervisor contexts
 * (docs/minimig-hw.md §1). Include after the other headers; it opens and
 * closes mmu.library itself.
 */
#ifndef MINIMIG_MMU_H
#define MINIMIG_MMU_H

#include <exec/types.h>
#include <utility/tagitem.h>
#include <mmu/context.h>
#include <proto/exec.h>
#define __NOLIBBASE__
#define MMU_BASE_NAME mm_MMUBase
#include <proto/mmu.h>
#undef __NOLIBBASE__

#define MM_MMU_REGS      0x00B80000UL
#define MM_MMU_REGS_SIZE 0x00010000UL
#define MM_MMU_VRAM      0x02000000UL
#define MM_MMU_VRAM_SIZE 0x00800000UL

enum { MM_MMU_NONE, MM_MMU_OK, MM_MMU_MAPPED, MM_MMU_FAILED };

static struct Library *mm_MMUBase;

/* what makes a page unusable for the hardware behind it */
#define MM_MMU_BAD (MAPP_BLANK | MAPP_INVALID | MAPP_SWAPPED | MAPP_SUPERVISORONLY | \
                    MAPP_REMAPPED | MAPP_BUNDLED | MAPP_INDIRECT | MAPP_ROM | \
                    MAPP_WRITEPROTECTED | MAPP_COPYBACK)

/* MM_MMU_NONE: no mmu.library (V43) or no context, nothing to do;
 * MM_MMU_OK: both windows usable already; MM_MMU_MAPPED: made usable now;
 * MM_MMU_FAILED: mmu.library refused. before[0], before[1] (may be NULL):
 * the windows' properties as found. */
static int mm_mmu_map(ULONG *before)
{
    static const struct TagItem done[] = { { TAG_DONE, 0 } };
    static const ULONG base[2] = { MM_MMU_REGS, MM_MMU_VRAM };
    static const ULONG size[2] = { MM_MMU_REGS_SIZE, MM_MMU_VRAM_SIZE };
    static const ULONG want[2] = { MAPP_CACHEINHIBIT | MAPP_IO,
                                   MAPP_CACHEINHIBIT | MAPP_NONSERIALIZED | MAPP_IMPRECISE };
    struct MMUContext *ctx[3];
    int r = MM_MMU_NONE, w, c;
    BOOL change = FALSE, ok = TRUE;

    if (!(mm_MMUBase = OpenLibrary((STRPTR)"mmu.library", 43)))
        return r;
    ctx[0] = DefaultContext();
    ctx[1] = ctx[0] ? SuperContext(ctx[0]) : NULL;
    ctx[2] = NULL;
    if (ctx[0] && ctx[1]) {
        LockMMUContext(ctx[0]);
        LockMMUContext(ctx[1]);
        for (w = 0; w < 2; w++)
            for (c = 0; c < 2; c++) {
                ULONG p = GetPropertiesA(ctx[c], base[w], (struct TagItem *)done);
                if (c == 0 && before)
                    before[w] = p;
                if ((p & MM_MMU_BAD) || !(p & MAPP_CACHEINHIBIT))
                    change = TRUE;
            }
        if (change) {
            for (w = 0; w < 2; w++)
                for (c = 0; c < 2; c++)
                    ok &= SetPropertiesA(ctx[c], want[w], ~0UL, base[w], size[w],
                                         (struct TagItem *)done) != 0;
            ok &= RebuildTreesA(ctx) != 0;
        }
        UnlockMMUContext(ctx[1]);
        UnlockMMUContext(ctx[0]);
        r = !change ? MM_MMU_OK : ok ? MM_MMU_MAPPED : MM_MMU_FAILED;
    }
    CloseLibrary(mm_MMUBase);
    mm_MMUBase = NULL;
    return r;
}

#endif
