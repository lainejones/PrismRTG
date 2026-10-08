/* SPDX-License-Identifier: GPL-3.0-only */
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
    puts("shadow damage spans passed"); return 0;
}
