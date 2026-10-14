/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Stefan Reinauer
 * Copyright (C) 2026 Laine Jones */
#ifndef PRISM_DRIVER_MODULE_H
#define PRISM_DRIVER_MODULE_H
#include <exec/ports.h>
#include <stddef.h>
#include "prismboard.h"
#include "boardops.h"
#ifndef PRISM_OPS_ABI
#error Surface operations require an explicit PRISM_OPS_ABI
#endif
#ifndef PRISM_OPS_SIZE
#define PRISM_OPS_SIZE ((ULONG)sizeof(struct PrismOps))
#endif
#define PRISM_DRIVER_MAGIC 0x50524456UL
#ifndef PRISM_DRIVER_ABI
#define PRISM_DRIVER_ABI 4UL
#endif
#define PRISM_DRIVER_STACK 16384UL

enum PrismDriverStatus {
    PRD_ABI_MISMATCH, PRD_STARTING, PRD_READY, PRD_NOT_FOUND,
    PRD_NO_RESOURCES, PRD_CANCELLED
};
struct PrismDriverConfig {
    UBYTE clutBGR;
    char card[256], monitor[256];
    /* boards PrismD already drives (their ConfigDevs): a probe leaves them
     * alone, so a second copy of a driver takes the second board of a kind
     * (multi-board.md). Private to one PrismD start - no ConfigDev field is
     * touched, other software's own claims (cd_Driver, CDF_CONFIGME) are
     * neither read nor changed. */
    struct ConfigDev *taken[4];
};
/* Shared memory remains live until the module's final acknowledgement.
 * A module exports pointers by remaining alive with its runtime initialized.
 *
 * Protocol 4 (1.1 beta 8) is the frozen one: a driver built against it keeps
 * loading in every later PrismD with protocol 4 and board ABI 3. The request
 * only grows at its end; the board and the settings travel by pointer with
 * their sizes, so either can grow too (docs/driver-modules.md):
 *  - struct PrismBoard, PrismMode, PrismOps and PrismDriverConfig only ever
 *    get new members at their END, optional ones: 0 / NULL means "the
 *    driver doesn't do this", which is what an older driver leaves there;
 *  - PrismD clears the board it hands over, so the members a driver does not
 *    know read as 0; a member PrismD calls is checked against the sizes the
 *    driver reports (opsSize for the operations: BOARD_CAN);
 *  - the layout signature covers the frozen members only, never a size;
 *  - a driver newer than PrismD (bigger structures) is refused with a
 *    message, because PrismD would not know what it fills in.
 * A change no older driver can live with bumps PRISM_BOARD_ABI. */
struct PrismDriverRequest {
    struct Message message;
    ULONG magic, abi, size;                 /* fixed in every protocol      */
    ULONG boardABI, boardSize, boardLayout, opsABI, opsSize;
    ULONG modeSize, configSize;             /* PrismD's structure sizes     */
    volatile BOOL cancelled;
    ULONG status;
    struct MsgPort *control;
    struct PrismDriverConfig *config;       /* PrismD's, configSize bytes   */
    struct PrismBoard *board;               /* PrismD's, boardSize bytes, cleared */
};

/* The frozen members (protocol 4 / board ABI 3): their offsets and sizes,
 * and how scalars and the mode are laid out. Members added later at the
 * end of a structure are deliberately NOT here - they are what the sizes
 * are for. Same-size reordering of a frozen member still shows. */
static inline ULONG driver_board_layout(void)
{
    ULONG h=2166136261UL;
#define MIX(v) h=(h^(ULONG)(v))*16777619UL
#define FIELD(f) MIX(offsetof(struct PrismBoard,f)); MIX(sizeof(((struct PrismBoard *)0)->f))
#define MFIELD(f) MIX(offsetof(struct PrismMode,f)); MIX(sizeof(((struct PrismMode *)0)->f))
    MIX(sizeof(BOOL)); MIX(sizeof(enum PrismFormat));
    FIELD(name); FIELD(configDev); FIELD(regs); FIELD(vram);
    FIELD(vramSize); FIELD(formats); FIELD(flags);
    FIELD(maxWidth); FIELD(maxHeight); FIELD(priv);
    FIELD(setMode); FIELD(checkMode); FIELD(setDisplayStart);
    FIELD(setPalette); FIELD(setSwitch); FIELD(waitVBlank); FIELD(shutdown);
    FIELD(saveState); FIELD(restoreState); FIELD(fillRect); FIELD(copyRect);
    FIELD(copyBetween); FIELD(expandRect); FIELD(waitBlit); FIELD(drawLine);
    FIELD(cursorImage); FIELD(cursorShow); FIELD(cursorMove);
    FIELD(cursorW); FIELD(cursorH);
    FIELD(bytesPerRow);
    FIELD(ops); FIELD(modeReady); FIELD(faults);
    FIELD(blitMaxBytes); FIELD(blitMaxRows); FIELD(blitMaxPitch);
    FIELD(overlaySet); FIELD(overlayOff); FIELD(setPower); FIELD(opsSize);
    MFIELD(width); MFIELD(height); MFIELD(format); MFIELD(refresh);
    MFIELD(bytesPerRow); MFIELD(pitchPixels);
#undef MFIELD
#undef FIELD
#undef MIX
    return h;
}
/* the driver's side: PrismD's structures at least as big as its own */
static inline BOOL driver_compatible(const struct PrismDriverRequest *r)
{
    return r->abi==PRISM_DRIVER_ABI && r->size>=sizeof(*r) &&
        r->boardABI==PRISM_BOARD_ABI && r->boardLayout==driver_board_layout() &&
        r->boardSize>=sizeof(struct PrismBoard) && r->modeSize>=sizeof(struct PrismMode) &&
        r->configSize>=sizeof(struct PrismDriverConfig) && r->board && r->config &&
        r->opsABI==PRISM_OPS_ABI && r->opsSize>=PRISM_OPS_SIZE;
}
BOOL driver_open(struct PrismBoard *, const char *, const struct PrismDriverConfig *);
/* Try every other .driver in PROGDIR:Drivers and LIBS:Prism (not the
 * supplied four) until one finds its board. */
BOOL driver_scan(struct PrismBoard *, const struct PrismDriverConfig *);
void driver_close(void);
/* stop the driver of board n (0 or 1) - a second board PrismD drops */
void driver_close_slot(int n);
void driver_swap_slots(void);                /* PRIMARY=SECOND */
/* TRUE while probing for a second board: no message when nothing is found */
extern BOOL driverQuiet;
/* Used by a claimed P96/UAE context after its final diagnostic is flushed. */
void driver_module_done(void);
#endif
