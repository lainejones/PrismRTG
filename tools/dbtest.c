/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laine Jones */
/*
 * dbtest - experiment: can graphics.library's private AddDisplayInfoData()
 * create a display-database record for a brand new ModeID?
 *
 *   dbtest          add records for 0x7A000000 and read them back
 */
#include <exec/types.h>
#include <graphics/displayinfo.h>
#include <graphics/gfxbase.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <string.h>
#include <stdio.h>

struct GfxBase *GfxBase;

#define TEST_ID 0x7A000000UL

/* VOID AddDisplayInfoData(handle a0, buf a1, size d0, tagID d1, id d2), LVO -744 */
static void AddDIData(DisplayInfoHandle h, APTR buf, ULONG size, ULONG tag, ULONG id)
{
    register DisplayInfoHandle a0 __asm("a0") = h;
    register APTR a1 __asm("a1") = buf;
    register ULONG d0 __asm("d0") = size;
    register ULONG d1 __asm("d1") = tag;
    register ULONG d2 __asm("d2") = id;
    register struct GfxBase *a6 __asm("a6") = GfxBase;
    __asm volatile ("jsr -744(%%a6)"
                    : "+r"(a0), "+r"(a1), "+r"(d0), "+r"(d1)
                    : "r"(d2), "r"(a6)
                    : "cc", "memory");
}

static void hdr(struct QueryHeader *q, ULONG tag, ULONG size)
{
    q->StructID = tag;
    q->DisplayID = TEST_ID;
    q->SkipID = TAG_SKIP;
    q->Length = (size + 7) / 8;
}

int main(void)
{
    struct DisplayInfo di;
    struct DimensionInfo dm;
    struct NameInfo ni;
    DisplayInfoHandle h;
    ULONG id, n;

    GfxBase = (struct GfxBase *)OpenLibrary("graphics.library", 39);
    if (!GfxBase) return 20;

    printf("before: FindDisplayInfo($%08lx) = %08lx\n", TEST_ID,
           (unsigned long)FindDisplayInfo(TEST_ID));

    memset(&di, 0, sizeof(di));
    hdr(&di.Header, DTAG_DISP, sizeof(di));
    di.PropertyFlags = DIPF_IS_WB | DIPF_IS_FOREIGN;
    di.Resolution.x = 22; di.Resolution.y = 22;
    di.PixelSpeed = 35;
    di.PaletteRange = 4096;
    di.RedBits = di.GreenBits = di.BlueBits = 8;
    AddDIData(NULL, &di, sizeof(di), DTAG_DISP, TEST_ID);

    h = FindDisplayInfo(TEST_ID);
    printf("after DISP add: handle = %08lx\n", (unsigned long)h);
    if (!h) goto out;

    memset(&dm, 0, sizeof(dm));
    hdr(&dm.Header, DTAG_DIMS, sizeof(dm));
    dm.MaxDepth = 8;
    dm.MinRasterWidth = 16; dm.MinRasterHeight = 16;
    dm.MaxRasterWidth = 640; dm.MaxRasterHeight = 480;
    dm.Nominal.MaxX = 639; dm.Nominal.MaxY = 479;
    dm.MaxOScan = dm.VideoOScan = dm.TxtOScan = dm.StdOScan = dm.Nominal;
    AddDIData(h, &dm, sizeof(dm), DTAG_DIMS, TEST_ID);

    memset(&ni, 0, sizeof(ni));
    hdr(&ni.Header, DTAG_NAME, sizeof(ni));
    strcpy((char *)ni.Name, "PRISM:640x480 8bit");
    AddDIData(h, &ni, sizeof(ni), DTAG_NAME, TEST_ID);

    memset(&dm, 0, sizeof(dm));
    n = GetDisplayInfoData(h, &dm, sizeof(dm), DTAG_DIMS, 0);
    printf("readback DIMS: %lu bytes, depth %u, nominal %d,%d-%d,%d\n", (unsigned long)n,
           dm.MaxDepth, dm.Nominal.MinX, dm.Nominal.MinY, dm.Nominal.MaxX, dm.Nominal.MaxY);
    memset(&ni, 0, sizeof(ni));
    n = GetDisplayInfoData(NULL, &ni, sizeof(ni), DTAG_NAME, TEST_ID);
    printf("readback NAME by id: %lu bytes, '%s'\n", (unsigned long)n, ni.Name);
    memset(&di, 0, sizeof(di));
    n = GetDisplayInfoData(h, &di, sizeof(di), DTAG_DISP, 0);
    printf("readback DISP: %lu bytes, flags %08lx, notavail %u\n", (unsigned long)n,
           (unsigned long)di.PropertyFlags, di.NotAvailable);
    printf("ModeNotAvailable = %ld\n", (long)ModeNotAvailable(TEST_ID));

    printf("enumeration tail:");
    for (id = INVALID_ID, n = 0; (id = NextDisplayInfo(id)) != INVALID_ID; n++)
        if ((id & 0xff000000) == 0x7a000000) printf(" $%08lx", (unsigned long)id);
    printf(" (%lu ids total)\n", (unsigned long)n);
out:
    CloseLibrary((struct Library *)GfxBase);
    return 0;
}
