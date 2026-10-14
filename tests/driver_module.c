/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Stefan Reinauer
 * Copyright (C) 2026 Laine Jones */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "driver_module.h"
int main(void)
{
    static struct PrismBoard board;
    static struct PrismDriverConfig config;
    struct PrismDriverRequest r;
    memset(&r,0,sizeof(r));
    r.abi=PRISM_DRIVER_ABI;r.size=sizeof(r);
    r.boardABI=PRISM_BOARD_ABI;r.boardSize=sizeof(struct PrismBoard);
    r.modeSize=sizeof(struct PrismMode);r.configSize=sizeof(struct PrismDriverConfig);
    r.boardLayout=driver_board_layout();r.opsABI=PRISM_OPS_ABI;r.opsSize=PRISM_OPS_SIZE;
    r.board=&board;r.config=&config;
    assert(driver_compatible(&r));
#define REJECT(field) r.field++; assert(!driver_compatible(&r)); r.field--
    REJECT(abi); REJECT(boardABI); REJECT(boardLayout); REJECT(opsABI);
#undef REJECT
    /* the frozen ABI: a later PrismD, with structures grown at their end and
     * a longer request, still takes this driver ... */
#define LATER(field) r.field+=16; assert(driver_compatible(&r)); r.field-=16
    LATER(size); LATER(boardSize); LATER(modeSize); LATER(configSize); LATER(opsSize);
#undef LATER
    /* ... an older one, whose structures are shorter than this driver's, not */
#define OLDER(field) r.field-=4; assert(!driver_compatible(&r)); r.field+=4
    OLDER(size); OLDER(boardSize); OLDER(modeSize); OLDER(configSize); OLDER(opsSize);
#undef OLDER
    r.board=NULL; assert(!driver_compatible(&r)); r.board=&board;
    r.config=NULL; assert(!driver_compatible(&r)); r.config=&config;
    /* the signature covers the frozen members, not the structure's size: a
     * member appended to the board later leaves it alone (the frozen list
     * ends with opsSize, the last member of board ABI 3) */
    assert(offsetof(struct PrismBoard, opsSize) + sizeof(board.opsSize) <= sizeof(struct PrismBoard));
    assert(PRISM_DRIVER_STACK==16384UL);
    puts("driver module request contracts passed (frozen protocol 4, board ABI 3)");
    return 0;
}