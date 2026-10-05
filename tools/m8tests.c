/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laine Jones */
/*
 * m8tests.c - PrismScreen DBUF and MANY (M8: double buffering, VRAM paging).
 *
 *   PrismScreen DBUF [MODEID=] [SECS=]  bounce a box with ChangeScreenBuffer
 *   PrismScreen MANY [MODEID=] [SECS=]  open more screens than VRAM holds,
 *                                        bring each to the front, check pixels
 */
#include <exec/types.h>
#include <dos/dos.h>
#include <graphics/gfx.h>
#include <graphics/rastport.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <stdio.h>
#include <string.h>
#include "prism.h"

static ULONG ticks(void)
{
    struct DateStamp d;
    DateStamp(&d);
    return (ULONG)d.ds_Minute * 3000 + d.ds_Tick;
}

/* wait for a message on port, at most `max` ticks; TRUE if one came */
static BOOL wait_msg(struct MsgPort *port, ULONG max)
{
    ULONG t;
    for (t = 0; t <= max; t++) {
        if (GetMsg(port)) {
            while (GetMsg(port)) ;
            return TRUE;
        }
        if (t < max) Delay(1);
    }
    return FALSE;
}

static void colours(struct Screen *s)
{
    SetRGB32(&s->ViewPort, 0, 0x10101010, 0x20202020, 0x40404040);
    SetRGB32(&s->ViewPort, 1, 0xffffffff, 0xffffffff, 0xffffffff);
    SetRGB32(&s->ViewPort, 2, 0xffffffff, 0x40404040, 0x20202020);
    SetRGB32(&s->ViewPort, 3, 0x30303030, 0xe0e0e0e0, 0x30303030);
    SetRGB32(&s->ViewPort, 4, 0x30303030, 0x60606060, 0xffffffff);
    SetRGB32(&s->ViewPort, 5, 0xffffffff, 0xe0e0e0e0, 0x20202020);
}

int dbuf_test(ULONG id, ULONG secs)
{
    struct Screen *s;
    struct ScreenBuffer *sb[2] = { NULL, NULL };
    struct MsgPort *safe = NULL, *disp = NULL;
    struct RastPort rp;
    ULONG frames = secs * 25, f, t0, t1, flips = 0, safeMiss = 0, dispMiss = 0;
    WORD w, h, bw = 120, bh = 90, x, y, dx = 6, dy = 4, px[2] = { -1, -1 }, py[2];
    int cur = 1, i, rc = 20;
    char buf[40];

    s = OpenScreenTags(NULL, SA_DisplayID, id, SA_Depth, 8, SA_ShowTitle, FALSE,
                       SA_Quiet, TRUE, SA_Title, (ULONG)"Prism M8", TAG_DONE);
    if (!s) {
        printf("DBUF: OpenScreen failed\n");
        return 20;
    }
    colours(s);
    w = s->Width; h = s->Height;
    sb[0] = AllocScreenBuffer(s, NULL, SB_SCREEN_BITMAP);
    sb[1] = AllocScreenBuffer(s, NULL, SB_COPY_BITMAP);
    safe = CreateMsgPort();
    disp = CreateMsgPort();
    if (!sb[0] || !sb[1] || !safe || !disp) {
        printf("DBUF: no screen buffers\n");
        goto out;
    }
    for (i = 0; i < 2; i++) {
        sb[i]->sb_DBufInfo->dbi_SafeMessage.mn_ReplyPort = safe;
        sb[i]->sb_DBufInfo->dbi_DispMessage.mn_ReplyPort = disp;
    }
    printf("DBUF: %dx%d, buffers %lx %lx\n", w, h, (unsigned long)sb[0]->sb_BitMap, (unsigned long)sb[1]->sb_BitMap);

    x = 10; y = 10;
    t0 = ticks();
    for (f = 0; f < frames; f++) {
        InitRastPort(&rp);
        rp.BitMap = sb[cur]->sb_BitMap;
        /* erase where this buffer had the box two frames ago, draw it anew */
        SetAPen(&rp, 0);
        if (px[cur] >= 0)
            RectFill(&rp, px[cur], py[cur], px[cur] + bw - 1, py[cur] + bh + 11);
        else
            RectFill(&rp, 0, 0, w - 1, h - 1);
        SetAPen(&rp, 2 + (f / 25) % 4);
        RectFill(&rp, x, y, x + bw - 1, y + bh - 1);
        SetAPen(&rp, 1);
        SetDrMd(&rp, JAM1);
        sprintf(buf, "frame %lu", (unsigned long)f);
        Move(&rp, x + 4, y + bh + 9);
        Text(&rp, buf, strlen(buf));
        px[cur] = x; py[cur] = y;

        if (!ChangeScreenBuffer(s, sb[cur])) {
            printf("DBUF: ChangeScreenBuffer failed at frame %lu\n", (unsigned long)f);
            break;
        }
        flips++;
        if (!wait_msg(disp, 10)) dispMiss++;
        if (!wait_msg(safe, 10)) safeMiss++;
        cur ^= 1;

        x += dx; y += dy;
        if (x < 0 || x + bw >= w) { dx = -dx; x += 2 * dx; }
        if (y < 0 || y + bh + 12 >= h) { dy = -dy; y += 2 * dy; }
    }
    t1 = ticks();
    printf("DBUF: %lu flips in %lu ticks (%lu.%lu fps), missing messages: disp %lu, safe %lu\n",
           (unsigned long)flips, (unsigned long)(t1 - t0),
           (unsigned long)(flips * 50 / (t1 - t0 ? t1 - t0 : 1)),
           (unsigned long)((flips * 500 / (t1 - t0 ? t1 - t0 : 1)) % 10),
           (unsigned long)dispMiss, (unsigned long)safeMiss);
    /* the buffer on display has the last box: check a pixel inside it */
    InitRastPort(&rp);
    rp.BitMap = sb[cur ^ 1]->sb_BitMap;
    printf("DBUF: shown buffer pixel in the box = %ld (expect %lu)\n",
           (long)ReadPixel(&rp, px[cur ^ 1] + 5, py[cur ^ 1] + 5),
           (unsigned long)(2 + ((flips - 1) / 25) % 4));
    Delay(secs * 25);
    rc = 0;
out:
    if (sb[1] && sb[0]) {
        ChangeScreenBuffer(s, sb[0]);          /* back to the screen's own */
        wait_msg(safe, 10);
        wait_msg(disp, 10);
    }
    if (sb[1]) FreeScreenBuffer(s, sb[1]);
    if (sb[0]) FreeScreenBuffer(s, sb[0]);
    if (safe) DeleteMsgPort(safe);
    if (disp) DeleteMsgPort(disp);
    CloseScreen(s);
    printf("DBUF: closed\n");
    return rc;
}

#define NMANY 4

int many_test(ULONG id, ULONG secs)
{
    struct Screen *s[NMANY];
    int i, n, round, bad = 0;
    char buf[40];

    for (n = 0; n < NMANY; n++) {
        s[n] = OpenScreenTags(NULL, SA_DisplayID, id, SA_Depth, 8, SA_ShowTitle, FALSE,
                              SA_Quiet, TRUE, SA_Title, (ULONG)"Prism M8", TAG_DONE);
        if (!s[n]) {
            printf("MANY: screen %d didn't open\n", n);
            break;
        }
        colours(s[n]);
        SetRast(&s[n]->RastPort, 2 + n % 4);
        SetAPen(&s[n]->RastPort, 1);
        SetDrMd(&s[n]->RastPort, JAM1);
        sprintf(buf, "Prism M8: screen %d of %d", n + 1, NMANY);
        Move(&s[n]->RastPort, 40, 40);
        Text(&s[n]->RastPort, buf, strlen(buf));
        RectFill(&s[n]->RastPort, 40 + n * 60, 80, 90 + n * 60, 130);
        printf("MANY: opened %d (%dx%d)\n", n + 1, s[n]->Width, s[n]->Height);
    }
    for (round = 0; round < 2; round++)
        for (i = 0; i < n; i++) {
            LONG bg, mark;
            ScreenToFront(s[i]);
            Delay(10);
            bg = ReadPixel(&s[i]->RastPort, s[i]->Width - 20, s[i]->Height - 20);
            mark = ReadPixel(&s[i]->RastPort, 60 + i * 60, 100);
            if (bg != 2 + i % 4 || mark != 1)
                bad++;
            printf("MANY: round %d, screen %d in front: background %ld mark %ld %s\n",
                   round, i + 1, (long)bg, (long)mark, (bg == 2 + i % 4 && mark == 1) ? "ok" : "WRONG");
        }
    if (n > 1)
        ScreenToFront(s[1]);
    printf("MANY: %d screens, %d wrong; screen 2 in front\n", n, bad);
    Delay(secs * 25);
    for (i = 0; i < n; i++)
        CloseScreen(s[i]);
    printf("MANY: closed\n");
    return bad ? 10 : 0;
}
