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
    while (begin < end && a[begin] == b[begin]) begin++;
    if (begin == end) return FALSE;
    while (end > begin && a[end-1] == b[end-1]) end--;
    *first = begin; *count = end - begin;
    return TRUE;
}
#endif
