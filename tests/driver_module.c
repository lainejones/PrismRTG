/* SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "driver_module.h"
int main(void)
{
    struct PrismDriverRequest r;
    memset(&r,0,sizeof(r));
    r.abi=PRISM_DRIVER_ABI;r.size=sizeof(r);
    r.boardABI=PRISM_BOARD_ABI;r.boardSize=sizeof(struct PrismBoard);
    r.boardLayout=driver_board_layout();r.opsABI=PRISM_OPS_ABI;r.opsSize=PRISM_OPS_SIZE;
    assert(driver_compatible(&r));
#define REJECT(field) r.field++; assert(!driver_compatible(&r)); r.field--
    REJECT(abi); REJECT(size); REJECT(boardABI); REJECT(boardSize);
    REJECT(boardLayout); REJECT(opsABI); REJECT(opsSize);
#undef REJECT
    assert(PRISM_DRIVER_STACK==16384UL);
    puts("driver module request contracts passed");
    return 0;
}
