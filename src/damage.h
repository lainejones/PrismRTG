/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Stefan Reinauer */
#ifndef PRISM_DAMAGE_H
#define PRISM_DAMAGE_H
/* Exact byte comparison also catches stores through pointers retained by
 * clients. Return the smallest changed span; equal rows need no bus write. */
static BOOL changed_span(const UBYTE *a, const UBYTE *b, ULONG size,
                         ULONG *first, ULONG *count)
{
    ULONG begin = 0, end = size;
    /* a long at a time, then the bytes of the long that differs: a
     * 1280x720 16-bit shadow is 1.8 MB, a byte loop over it took a 68060
     * tens of milliseconds (68020 and up read longs at any address) */
    while (begin + 4 <= end && *(const ULONG *)(a + begin) == *(const ULONG *)(b + begin))
        begin += 4;
    while (begin < end && a[begin] == b[begin]) begin++;
    if (begin == end) return FALSE;
    while (end >= begin + 4 && *(const ULONG *)(a + end - 4) == *(const ULONG *)(b + end - 4))
        end -= 4;
    while (end > begin && a[end-1] == b[end-1]) end--;
    *first = begin; *count = end - begin;
    return TRUE;
}
#endif
