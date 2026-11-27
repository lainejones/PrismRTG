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
#include <exec/libraries.h>
#include <utility/tagitem.h>
#include <proto/exec.h>

/* mmu.library's interface, declared here so that the build needs no MMULib
 * SDK: the seven calls used (library vector offsets from its mmu.fd) and
 * the page properties (MAPP_* in its mmu/context.h), under names of our
 * own so that an installed SDK cannot clash with them. */
struct MMUContext;

#define MM_MAPP_WRITEPROTECTED  (1UL << 2)
#define MM_MAPP_CACHEINHIBIT    (1UL << 6)
#define MM_MAPP_SUPERVISORONLY  (1UL << 7)
#define MM_MAPP_BLANK           (1UL << 11)
#define MM_MAPP_COPYBACK        (1UL << 13)
#define MM_MAPP_INVALID         (1UL << 14)
#define MM_MAPP_REMAPPED        (1UL << 15)
#define MM_MAPP_SWAPPED         (1UL << 16)
#define MM_MAPP_ROM             (1UL << 17)
#define MM_MAPP_IMPRECISE       (1UL << 21)
#define MM_MAPP_INDIRECT        (1UL << 22)
#define MM_MAPP_BUNDLED         (1UL << 23)
#define MM_MAPP_NONSERIALIZED   (1UL << 29)
#define MM_MAPP_IO              (1UL << 30)

#define MM_MMU_REGS      0x00B80000UL
#define MM_MMU_REGS_SIZE 0x00010000UL
#define MM_MMU_VRAM      0x02000000UL
#define MM_MMU_VRAM_SIZE 0x00800000UL

enum { MM_MMU_NONE, MM_MMU_OK, MM_MMU_MAPPED, MM_MMU_FAILED };

static struct Library *mm_MMUBase;

/* d0, d1, a0, a1 (and fp0, fp1 with an FPU) are scratch across a library
 * call; plain jsr, so the same on the 68020 to the 68060 */
#ifdef __HAVE_68881__
#define MM_MMU_CLOBBER "fp0", "fp1", "cc", "memory"
#else
#define MM_MMU_CLOBBER "cc", "memory"
#endif

static struct MMUContext *mm_DefaultContext(void)
{
    register ULONG d0 __asm("d0");
    register ULONG d1 __asm("d1");
    register APTR a0 __asm("a0");
    register APTR a1 __asm("a1");
    register struct Library *a6 __asm("a6") = mm_MMUBase;
    __asm volatile ("jsr %%a6@(-150:W)" : "=r"(d0), "=r"(d1), "=r"(a0), "=r"(a1)
                    : "r"(a6) : MM_MMU_CLOBBER);
    return (struct MMUContext *)d0;
}

static struct MMUContext *mm_SuperContext(struct MMUContext *ctx)
{
    register ULONG d0 __asm("d0");
    register ULONG d1 __asm("d1");
    register APTR a0 __asm("a0") = ctx;
    register APTR a1 __asm("a1");
    register struct Library *a6 __asm("a6") = mm_MMUBase;
    __asm volatile ("jsr %%a6@(-144:W)" : "=r"(d0), "=r"(d1), "+r"(a0), "=r"(a1)
                    : "r"(a6) : MM_MMU_CLOBBER);
    return (struct MMUContext *)d0;
}

static void mm_LockMMUContext(struct MMUContext *ctx)
{
    register ULONG d0 __asm("d0");
    register ULONG d1 __asm("d1");
    register APTR a0 __asm("a0") = ctx;
    register APTR a1 __asm("a1");
    register struct Library *a6 __asm("a6") = mm_MMUBase;
    __asm volatile ("jsr %%a6@(-72:W)" : "=r"(d0), "=r"(d1), "+r"(a0), "=r"(a1)
                    : "r"(a6) : MM_MMU_CLOBBER);
}

static void mm_UnlockMMUContext(struct MMUContext *ctx)
{
    register ULONG d0 __asm("d0");
    register ULONG d1 __asm("d1");
    register APTR a0 __asm("a0") = ctx;
    register APTR a1 __asm("a1");
    register struct Library *a6 __asm("a6") = mm_MMUBase;
    __asm volatile ("jsr %%a6@(-78:W)" : "=r"(d0), "=r"(d1), "+r"(a0), "=r"(a1)
                    : "r"(a6) : MM_MMU_CLOBBER);
}

static ULONG mm_GetPropertiesA(struct MMUContext *ctx, ULONG lower, struct TagItem *tags)
{
    register ULONG d0 __asm("d0");
    register ULONG d1 __asm("d1");
    register APTR a0 __asm("a0") = ctx;
    register ULONG a1 __asm("a1") = lower;
    register struct TagItem *a2 __asm("a2") = tags;
    register struct Library *a6 __asm("a6") = mm_MMUBase;
    __asm volatile ("jsr %%a6@(-90:W)" : "=r"(d0), "=r"(d1), "+r"(a0), "+r"(a1)
                    : "r"(a2), "r"(a6) : MM_MMU_CLOBBER);
    return d0;
}

static ULONG mm_SetPropertiesA(struct MMUContext *ctx, ULONG flags, ULONG mask,
                               ULONG lower, ULONG size, struct TagItem *tags)
{
    register ULONG d0 __asm("d0") = size;
    register ULONG d1 __asm("d1") = flags;
    register ULONG d2 __asm("d2") = mask;
    register APTR a0 __asm("a0") = ctx;
    register ULONG a1 __asm("a1") = lower;
    register struct TagItem *a2 __asm("a2") = tags;
    register struct Library *a6 __asm("a6") = mm_MMUBase;
    __asm volatile ("jsr %%a6@(-84:W)" : "+r"(d0), "+r"(d1), "+r"(a0), "+r"(a1)
                    : "r"(d2), "r"(a2), "r"(a6) : MM_MMU_CLOBBER);
    return d0;
}

/* ctxs: a NULL-terminated array of contexts */
static ULONG mm_RebuildTreesA(struct MMUContext **ctxs)
{
    register ULONG d0 __asm("d0");
    register ULONG d1 __asm("d1");
    register APTR a0 __asm("a0") = ctxs;
    register APTR a1 __asm("a1");
    register struct Library *a6 __asm("a6") = mm_MMUBase;
    __asm volatile ("jsr %%a6@(-360:W)" : "=r"(d0), "=r"(d1), "+r"(a0), "=r"(a1)
                    : "r"(a6) : MM_MMU_CLOBBER);
    return d0;
}

/* what makes a page unusable for the hardware behind it */
#define MM_MMU_BAD (MM_MAPP_BLANK | MM_MAPP_INVALID | MM_MAPP_SWAPPED | \
                    MM_MAPP_SUPERVISORONLY | MM_MAPP_REMAPPED | MM_MAPP_BUNDLED | \
                    MM_MAPP_INDIRECT | MM_MAPP_ROM | MM_MAPP_WRITEPROTECTED | \
                    MM_MAPP_COPYBACK)

/* MM_MMU_NONE: no mmu.library (V43) or no context, nothing to do;
 * MM_MMU_OK: both windows usable already; MM_MMU_MAPPED: made usable now;
 * MM_MMU_FAILED: mmu.library refused. before[0], before[1] (may be NULL):
 * the windows' properties as found. */
static int mm_mmu_map(ULONG *before)
{
    static const struct TagItem done[] = { { TAG_DONE, 0 } };
    static const ULONG base[2] = { MM_MMU_REGS, MM_MMU_VRAM };
    static const ULONG size[2] = { MM_MMU_REGS_SIZE, MM_MMU_VRAM_SIZE };
    static const ULONG want[2] = { MM_MAPP_CACHEINHIBIT | MM_MAPP_IO,
                                   MM_MAPP_CACHEINHIBIT | MM_MAPP_NONSERIALIZED | MM_MAPP_IMPRECISE };
    struct MMUContext *ctx[3];
    int r = MM_MMU_NONE, w, c;
    BOOL change = FALSE, ok = TRUE;

    if (!(mm_MMUBase = OpenLibrary((STRPTR)"mmu.library", 43)))
        return r;
    ctx[0] = mm_DefaultContext();
    ctx[1] = ctx[0] ? mm_SuperContext(ctx[0]) : NULL;
    ctx[2] = NULL;
    if (ctx[0] && ctx[1]) {
        mm_LockMMUContext(ctx[0]);
        mm_LockMMUContext(ctx[1]);
        for (w = 0; w < 2; w++)
            for (c = 0; c < 2; c++) {
                ULONG p = mm_GetPropertiesA(ctx[c], base[w], (struct TagItem *)done);
                if (c == 0 && before)
                    before[w] = p;
                if ((p & MM_MMU_BAD) || !(p & MM_MAPP_CACHEINHIBIT))
                    change = TRUE;
            }
        if (change) {
            for (w = 0; w < 2; w++)
                for (c = 0; c < 2; c++)
                    ok &= mm_SetPropertiesA(ctx[c], want[w], ~0UL, base[w], size[w],
                                            (struct TagItem *)done) != 0;
            ok &= mm_RebuildTreesA(ctx) != 0;
        }
        mm_UnlockMMUContext(ctx[1]);
        mm_UnlockMMUContext(ctx[0]);
        r = !change ? MM_MMU_OK : ok ? MM_MMU_MAPPED : MM_MMU_FAILED;
    }
    CloseLibrary(mm_MMUBase);
    mm_MMUBase = NULL;
    return r;
}

#endif
