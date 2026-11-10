/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Stefan Reinauer */
#include <exec/types.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "damage.h"
int main(void)
{
    UBYTE a[256]={0}, b[256]={0}; ULONG first,count;
    assert(!changed_span(a,b,256,&first,&count));
    a[30]=1;a[80]=2;
    assert(changed_span(a,b,256,&first,&count) && first==30 && count==51);
    memcpy(b,a,256);
    assert(!changed_span(a,b,256,&first,&count));
    a[0]=3;a[255]=4;
    assert(changed_span(a,b,256,&first,&count) && first==0 && count==256);
    assert(!changed_span(a,b,0,&first,&count));
    /* compared a long at a time: every offset and length, odd sizes, and
     * rows that start off a long boundary give the exact byte span */
    {
        ULONG s, e, n, base;
        for (base = 0; base < 4; base++)
            for (n = 1; n <= 23; n++)
                for (s = 0; s < n; s++)
                    for (e = s; e < n; e++) {
                        memset(a, 0, sizeof(a)); memset(b, 0, sizeof(b));
                        a[base + s] = 5; a[base + e] = 6;
                        assert(changed_span(a + base, b + base, n, &first, &count) &&
                               first == s && count == e - s + 1);
                        assert(!changed_span(a + base, b + base, s, &first, &count));
                    }
    }
    puts("shadow damage spans passed"); return 0;
}
