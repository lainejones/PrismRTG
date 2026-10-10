/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laine Jones
 * Copyright (C) 2026 Stefan Reinauer */
/*
 * prefs.c - read and write ENV:Prism.prefs (see prefs.h).
 */
#include <exec/types.h>
#include <stdio.h>
#include <dos/dos.h>
#include <proto/dos.h>
#include <string.h>
#include <stdlib.h>
#include "prefs.h"

/* New sizes go at the END: a slot's position is its ModeID. */
const UWORD prefs_sizes[PREFS_NSIZES][2] = {
    { 640, 400 }, { 640, 480 }, { 800, 600 }, { 1024, 768 },
    { 1280, 720 }, { 1280, 1024 },
    /* low resolution, for games that draw 320 pixels wide and assume the
     * screen is exactly that (ADoom, 2026-10-04) */
    { 320, 200 }, { 320, 240 },
    /* 16:9 (issue #1, 2026-10-09): offered where the board can show them -
     * the ZZ9000's firmware has 1920x1080, and P96 drivers with their own
     * mode lists (the Vampire's) may have both */
    { 960, 540 }, { 1920, 1080 },
};
/* New depths go at the END too. 24 = 3 bytes a pixel, 32 = 4: a card may
 * have either or both (see board_format in prismd.c). */
const UBYTE prefs_depths[PREFS_NDEPTHS] = { 8, 16, 24, 32 };
const UBYTE prefs_rates[PREFS_NRATES] = { 0, 60, 70, 72, 75 };

static const char *const boardNames[PB_COUNT] = { "AUTO", "PICASSO2", "ZZ9000", "P96", "UAEGFX", "" };

void prefs_default(struct PrismPrefs *p)
{
    memset(p, 0, sizeof(*p));
    p->board = PB_AUTO;
    p->blitter = 1;
    p->log = 0;
    memset(p->on, 1, sizeof(p->on));
}

static int same(const char *a, const char *b)
{
    while (*a && *b) {
        char x = *a++, y = *b++;
        if (x >= 'a' && x <= 'z') x -= 32;
        if (y >= 'a' && y <= 'z') y -= 32;
        if (x != y)
            return 0;
    }
    return *a == *b;
}

static int onoff(const char *v)
{
    return same(v, "ON") || same(v, "YES") || same(v, "1");
}

static void parse_mode(struct PrismPrefs *p, char *v)
{
    unsigned w, h, d, hz = 0;
    char state[8] = "ON";
    int i;

    if (sscanf(v, "%ux%ux%u %7s %u", &w, &h, &d, state, &hz) < 3)
        return;
    for (i = 0; i < PREFS_NMODES; i++)
        if (prefs_sizes[PREFS_SIZE(i)][0] == w && prefs_sizes[PREFS_SIZE(i)][1] == h &&
            prefs_depths[PREFS_DEPTH(i)] == d) {
            p->on[i] = onoff(state);
            p->hz[i] = hz < 256 ? hz : 0;
            return;
        }
}

/* Read with dos.library, not stdio: libnix keeps what fopen allocated
 * after fclose, 54 KB of a daemon that never quits (PrismD, 2026-10-07). */
BOOL prefs_load(struct PrismPrefs *p, const char *path)
{
    BPTR f;
    char line[288], *v, *e;
    int i;

    prefs_default(p);
    if (!(f = Open((STRPTR)path, MODE_OLDFILE)))
        return FALSE;
    while (FGets(f, (STRPTR)line, sizeof(line))) {
        if ((e = strpbrk(line, "\r\n")))
            *e = 0;
        if (line[0] == ';' || line[0] == '#' || !(v = strchr(line, '=')))
            continue;
        *v++ = 0;
        if (same(line, "BOARD")) {
            /* one of the supplied drivers by its name, else the name of
             * another LIBS:Prism/<name>.driver */
            p->board = PB_AUTO;
            for (i = 0; i < PB_OTHER; i++)
                if (same(v, boardNames[i]))
                    p->board = i;
            if (p->board == PB_AUTO && !same(v, "AUTO") && *v &&
                strlen(v) <= PREFS_BOARDNAME && !strpbrk(v, "/:\\ ")) {
                p->board = PB_OTHER;
                strcpy(p->boardName, v);
            }
        } else if (same(line, "P96CARD") || same(line, "P96MONITOR")) {
            char *dest = same(line, "P96CARD") ? p->p96card : p->p96monitor;
            if (strlen(v) < sizeof(p->p96card))
                strcpy(dest, v);
        } else if (same(line, "SOFTWAREPOINTER")) {
            p->softwarePointer = onoff(v);
        } else if (same(line, "DRAGGING")) {
            p->dragging = onoff(v);
        } else if (same(line, "BLITTER")) {
            p->blitter = onoff(v);
        } else if (same(line, "LOG")) {
            p->log = same(v, "SYNC") ? 2 : onoff(v);
        } else if (same(line, "PALETTE")) {
            p->clutBGR = same(v, "BGR");
        } else if (same(line, "MODE")) {
            parse_mode(p, v);
        }
    }
    Close(f);
    return TRUE;
}

BOOL prefs_save(const struct PrismPrefs *p, const char *path)
{
    FILE *f;
    int i, ok;

    if (!(f = fopen(path, "w")))
        return FALSE;
    fprintf(f, "; Prism RTG settings - written by PrismPrefs\n");
    fprintf(f, "BOARD=%s\n", p->board == PB_OTHER && p->boardName[0] ? p->boardName :
                              boardNames[p->board < PB_OTHER ? p->board : PB_AUTO]);
    if (p->p96card[0]) fprintf(f, "P96CARD=%s\n", p->p96card);
    if (p->p96monitor[0]) fprintf(f, "P96MONITOR=%s\n", p->p96monitor);
    fprintf(f, "SOFTWAREPOINTER=%s\n", p->softwarePointer ? "ON" : "OFF");
    fprintf(f, "DRAGGING=%s\n", p->dragging ? "ON" : "OFF");
    fprintf(f, "BLITTER=%s\n", p->blitter ? "ON" : "OFF");
    fprintf(f, "LOG=%s\n", p->log == 2 ? "SYNC" : p->log ? "ON" : "OFF");
    if (p->clutBGR)
        fprintf(f, "PALETTE=BGR\n");
    for (i = 0; i < PREFS_NMODES; i++)
        fprintf(f, "MODE=%ux%ux%u %s %u\n", prefs_sizes[PREFS_SIZE(i)][0],
                prefs_sizes[PREFS_SIZE(i)][1], prefs_depths[PREFS_DEPTH(i)],
                p->on[i] ? "ON" : "OFF", p->hz[i]);
    ok = !ferror(f);
    return fclose(f) == 0 && ok;
}
