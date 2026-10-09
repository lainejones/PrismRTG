/* SPDX-License-Identifier: GPL-3.0-only */
/* Launch C:ModuleCheck with output redirected to RAM:module.log, then
 * inspect that file and report over serial from the dedicated boot task. */
#include <proto/exec.h>
#include <proto/dos.h>
#include <dos/dostags.h>
#include <stdio.h>
#include <string.h>
#include "driver_module.h"
#ifndef MODULE_CHECK
static void emit(const char *s)
{
    *(volatile UWORD *)0xdff032=30;
    while(*s) {
        while(!(*(volatile UWORD *)0xdff018&0x2000)) {}
        *(volatile UWORD *)0xdff030=0x100|(UBYTE)*s++;
    }
}
#endif
#ifdef MODULE_CHECK
int main(void)
{
    struct PrismBoard board;
    struct PrismDriverConfig config;
    memset(&config,0,sizeof(config));
    if(driver_open(&board,"MissingFixture",&config))return 5;
    if(driver_open(&board,"OldFixture",&config))return 5;
    if(driver_open(&board,"FailedFixture",&config))return 5;
    if(!driver_open(&board,"Fixture",&config))return 5;
    if(strcmp(board.name,"module fixture") || board.vramSize<16384 || board.vramSize>18000)return 5;
    driver_close();
    Delay(5);
    puts("parent output still open");fflush(stdout);
    if(!driver_open(&board,"Fixture",&config))return 5;
    driver_close();
    /* the driver template, built on a fake board: the minimal contract */
    if(!driver_open(&board,"TEMPLATE",&config))return 5;
    board_defaults(&board);
    if(strcmp(board.name,"Template board (fake)") || board.vramSize!=1024*1024 ||
       !board.setMode || !board.setDisplayStart || !board.checkMode)return 5;
    puts("template driver loaded");fflush(stdout);
    driver_close();
    return 0;
}
#else
int main(void)
{
    FILE *file;static char text[4096];size_t n;LONG result;
    emit("MODULE GUEST START\n");
    AssignLock("T",Lock("RAM:",ACCESS_READ));
    result=SystemTags("C:ModuleCheck >RAM:module.log",NP_StackSize,(ULONG)32768,TAG_END);
    file=fopen("RAM:module.log","r");
    if(!file) {emit("FAIL no log\n");return 20;}
    n=fread(text,1,sizeof(text)-1,file);text[n]=0;fclose(file);emit(text);
    if(result || !strstr(text,"cannot load MissingFixture") ||
       !strstr(text,"OldFixture driver ABI/version or layout mismatch") ||
       !strstr(text,"module fixture: probe failed") ||
       !strstr(text,"module fixture: final output") ||
       !strstr(text,"parent output still open") ||
       !strstr(text,"template driver loaded")) {emit("FAIL module lifecycle\n");return 20;}
    emit("PASS\n");return 0;
}
#endif
