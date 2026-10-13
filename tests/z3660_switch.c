/* SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include <stdio.h>
#include "../src/drv_z3660.c"
struct ExpansionBase *ExpansionBase=NULL; /* not common: keeps libnix from auto-opening it */
static UBYTE cia[0x201];
static void sw(UBYTE ms,BOOL rtg,UBYTE pra,UBYTE ddra)
{
    struct PrismBoard b={0};struct Z3Priv p={0};
    b.priv=&p;p.ciab=cia;p.monSwitch=ms;cia[0]=pra;cia[0x200]=ddra;
    z3_SetSwitch(&b,rtg);
}
int main(void)
{
    /* SEL, level 32: bit 2 is 0 for RTG and 1 for the Amiga; only bit 2 driven */
    sw(MS_SEL|MS_SEL_LEVEL,TRUE,0xff,0x00);  assert(cia[0]==0xfb && cia[0x200]==0x04);
    sw(MS_SEL|MS_SEL_LEVEL,FALSE,0x00,0xc0); assert(cia[0]==0x04 && cia[0x200]==0xc4);
    /* SEL without the level: the other way round */
    sw(MS_SEL,TRUE,0x00,0x00);  assert(cia[0]==0x04);
    sw(MS_SEL,FALSE,0xff,0x00); assert(cia[0]==0xfb);
    /* CTS, level 16 (the shipped 17): bit 4 */
    sw(MS_CTS|MS_CTS_LEVEL,TRUE,0xff,0x00);  assert(cia[0]==0xef && cia[0x200]==0x10);
    sw(MS_CTS|MS_CTS_LEVEL,FALSE,0x00,0x00); assert(cia[0]==0x10);
    /* both lines; RTS/DTR (bits 6, 7) and the rest are kept */
    sw(MS_CTS|MS_SEL|MS_SEL_LEVEL,TRUE,0xc0,0xc0); assert(cia[0]==0xd0 && cia[0x200]==0xd4);
    /* a level without its line drives nothing */
    sw(MS_SEL_LEVEL,TRUE,0x5a,0x00); assert(cia[0]==0x5a && cia[0x200]==0x00);
    puts("Z3660 monitor switch passed");return 0;
}
