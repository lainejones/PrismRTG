/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PRISM_DRIVER_MODULE_H
#define PRISM_DRIVER_MODULE_H
#include <exec/ports.h>
#include <stddef.h>
#include "prismboard.h"
#ifdef PRISM_BOARD_HAS_OPS
#include "boardops.h"
#ifndef PRISM_OPS_ABI
#error Surface operations require an explicit PRISM_OPS_ABI
#endif
#ifndef PRISM_OPS_SIZE
#define PRISM_OPS_SIZE ((ULONG)sizeof(struct PrismOps))
#endif
#else
#define PRISM_OPS_ABI 0UL
#define PRISM_OPS_SIZE 0UL
#endif
#define PRISM_DRIVER_MAGIC 0x50524456UL
#ifndef PRISM_DRIVER_ABI
#define PRISM_DRIVER_ABI 3UL
#endif
#define PRISM_DRIVER_STACK 16384UL

enum PrismDriverStatus {
    PRD_ABI_MISMATCH, PRD_STARTING, PRD_READY, PRD_NOT_FOUND,
    PRD_NO_RESOURCES, PRD_CANCELLED
};
struct PrismDriverConfig {
    UBYTE clutBGR;
    char card[256], monitor[256];
};
/* Shared memory remains live until the module's final acknowledgement.
 * A module exports pointers by remaining alive with its runtime initialized.
 * The message/magic/abi/size prefix stays fixed across protocol revisions. */
struct PrismDriverRequest {
    struct Message message;
    ULONG magic, abi, size;
    ULONG boardABI, boardSize, boardLayout, opsABI, opsSize;
    volatile BOOL cancelled;
    ULONG status;
    struct MsgPort *control;
    struct PrismDriverConfig config;
    struct PrismBoard board;
};

/* Sizes alone miss same-size field reordering. Include offsets of all board
 * fields and mode fields, plus scalar and pixel-format representation. */
static inline ULONG driver_board_layout(void)
{
    ULONG h=2166136261UL;
#define MIX(v) h=(h^(ULONG)(v))*16777619UL
#define FIELD(f) MIX(offsetof(struct PrismBoard,f)); MIX(sizeof(((struct PrismBoard *)0)->f))
    MIX(sizeof(struct PrismBoard)); MIX(sizeof(struct PrismMode));
    MIX(sizeof(BOOL)); MIX(sizeof(enum PrismFormat)); MIX(PF_COUNT);
    FIELD(name); FIELD(configDev); FIELD(regs); FIELD(vram);
    FIELD(vramSize); FIELD(formats); FIELD(flags);
    FIELD(maxWidth); FIELD(maxHeight); FIELD(priv);
    FIELD(setMode); FIELD(checkMode); FIELD(setDisplayStart);
    FIELD(setPalette); FIELD(setSwitch); FIELD(waitVBlank); FIELD(shutdown);
    FIELD(saveState); FIELD(restoreState); FIELD(fillRect); FIELD(copyRect);
    FIELD(copyBetween); FIELD(expandRect); FIELD(waitBlit); FIELD(drawLine);
    FIELD(cursorImage); FIELD(cursorShow); FIELD(cursorMove);
    FIELD(bytesPerRow); FIELD(textExpand);
#ifdef PRISM_BOARD_HAS_OPS
    FIELD(ops); FIELD(modeReady); FIELD(faults);
#endif
    MIX(offsetof(struct PrismMode,width)); MIX(offsetof(struct PrismMode,height));
    MIX(offsetof(struct PrismMode,format)); MIX(offsetof(struct PrismMode,refresh));
    MIX(offsetof(struct PrismMode,bytesPerRow));
#undef FIELD
#undef MIX
    return h;
}
static inline BOOL driver_compatible(const struct PrismDriverRequest *r)
{
    return r->abi==PRISM_DRIVER_ABI && r->size==sizeof(*r) &&
        r->boardABI==PRISM_BOARD_ABI && r->boardSize==sizeof(struct PrismBoard) &&
        r->boardLayout==driver_board_layout() &&
        r->opsABI==PRISM_OPS_ABI && r->opsSize==PRISM_OPS_SIZE;
}
BOOL driver_open(struct PrismBoard *, const char *, const struct PrismDriverConfig *);
void driver_close(void);
/* Used by a claimed P96/UAE context after its final diagnostic is flushed. */
void driver_module_done(void);
#endif
