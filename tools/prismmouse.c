/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laine Jones */
/*
 * PrismMouse - move the mouse pointer from software (for testing the
 * pointer on the card without a real mouse).
 *
 *   PrismMouse X=<n> Y=<n> [STEPS=n] [WAIT=ticks]
 *
 * Feeds IECLASS_NEWPOINTERPOS events (pixel position on the front screen)
 * into input.device, gliding from the current position to (X, Y).
 */
#include <exec/types.h>
#include <exec/io.h>
#include <devices/input.h>
#include <devices/inputevent.h>
#include <intuition/intuitionbase.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <stdio.h>
#include <string.h>

struct IntuitionBase *IntuitionBase;

#define TEMPLATE "X/A/N,Y/A/N,STEPS/K/N,WAIT/K/N,CLICK/S"

int main(void)
{
    LONG args[5] = { 0 };
    struct RDArgs *rda;
    struct MsgPort *port;
    struct IOStdReq *io;
    struct InputEvent ie;
    struct IEPointerPixel pp;
    struct Screen *s;
    WORD x0, y0, x1, y1, steps = 20, i;
    LONG wait = 1;
    int rc = 20;

    if (!(rda = ReadArgs(TEMPLATE, args, NULL))) {
        PrintFault(IoErr(), "PrismMouse");
        return 20;
    }
    if (!(IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 39)))
        goto out;
    x1 = *(LONG *)args[0];
    y1 = *(LONG *)args[1];
    if (args[2]) steps = *(LONG *)args[2];
    if (args[3]) wait = *(LONG *)args[3];
    if (steps < 1) steps = 1;

    if (!(port = CreateMsgPort()))
        goto out;
    if (!(io = (struct IOStdReq *)CreateIORequest(port, sizeof(*io)))) {
        DeleteMsgPort(port);
        goto out;
    }
    if (OpenDevice("input.device", 0, (struct IORequest *)io, 0)) {
        DeleteIORequest((struct IORequest *)io);
        DeleteMsgPort(port);
        goto out;
    }

    s = IntuitionBase->FirstScreen;
    x0 = s->MouseX;
    y0 = s->MouseY;
    for (i = 1; i <= steps; i++) {
        memset(&ie, 0, sizeof(ie));
        pp.iepp_Screen = s;
        pp.iepp_Position.X = x0 + (LONG)(x1 - x0) * i / steps;
        pp.iepp_Position.Y = y0 + (LONG)(y1 - y0) * i / steps;
        ie.ie_Class = IECLASS_NEWPOINTERPOS;
        ie.ie_SubClass = IESUBCLASS_PIXEL;
        ie.ie_EventAddress = &pp;
        io->io_Command = IND_WRITEEVENT;
        io->io_Data = &ie;
        io->io_Length = sizeof(ie);
        DoIO((struct IORequest *)io);
        if (wait) Delay(wait);
    }
    if (args[4]) {
        /* left button down, then up, where the pointer is */
        UWORD codes[2] = { IECODE_LBUTTON, IECODE_LBUTTON | IECODE_UP_PREFIX };
        for (i = 0; i < 2; i++) {
            memset(&ie, 0, sizeof(ie));
            ie.ie_Class = IECLASS_RAWMOUSE;
            ie.ie_Code = codes[i];
            ie.ie_Qualifier = IEQUALIFIER_RELATIVEMOUSE | (i == 0 ? IEQUALIFIER_LEFTBUTTON : 0);
            io->io_Command = IND_WRITEEVENT;
            io->io_Data = &ie;
            io->io_Length = sizeof(ie);
            DoIO((struct IORequest *)io);
            Delay(5);
        }
    }
    printf("PrismMouse: (%d,%d) -> (%d,%d)%s; screen says %d,%d\n", x0, y0, x1, y1,
           args[4] ? " + click" : "", s->MouseX, s->MouseY);
    CloseDevice((struct IORequest *)io);
    DeleteIORequest((struct IORequest *)io);
    DeleteMsgPort(port);
    rc = 0;
out:
    if (IntuitionBase) CloseLibrary((struct Library *)IntuitionBase);
    FreeArgs(rda);
    return rc;
}
