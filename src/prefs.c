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
const UWORD prefs_sizes[PREFS_NFIXED][2] = {
    { 640, 400 }, { 640, 480 }, { 800, 600 }, { 1024, 768 },
    { 1280, 720 }, { 1280, 1024 },
    /* low resolution, for games that draw 320 pixels wide and assume the
     * screen is exactly that (ADoom, 2026-10-04) */
    { 320, 200 }, { 320, 240 },
    /* 16:9 (issue #1, 2026-10-09): offered where the board can show them -
     * the ZZ9000's firmware has 1920x1080, and P96 drivers with their own
     * mode lists (the Vampire's) may have both */
    { 960, 540 }, { 1920, 1080 },
    /* 16:10 (issue #1, beta 5): common on PC monitors */
    { 1440, 900 },
};
/* larger (Jusii, #14): the other fixed modes of the ZZ9000 and Z3660
 * firmwares; a board that can't show one refuses it in checkMode. Their
 * slots come after the four custom ones (prefs.h), so no existing ModeID
 * moves; off until switched on in PrismPrefs. */
const UWORD prefs_extra[PREFS_NEXTRA][2] = {
    { 1280, 800 }, { 1600, 1200 }, { 1920, 1200 }, { 1680, 1050 },
    { 1600, 900 }, { 1366, 768 }, { 2560, 1440 },
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
    p->second = 1;
    p->secondMon = SM_SCREEN;
    p->log = 0;
    p->gamma = 10;
    p->hwOverlay = 1;
    memset(p->on, 1, sizeof(p->on));
    {
        /* the larger sizes start off: a prefs file from before them has no
         * MODE= line for them, and nobody has seen them on every board yet */
        int i;
        for (i = 0; i < PREFS_NMODES; i++)
            if (PREFS_SIZE(i) >= PREFS_NFIXED + PREFS_NCUSTOM)
                p->on[i] = 0;
    }
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

int prefs_prog_find(const struct PrismPrefs *p, const char *name)
{
    int i;
    for (i = 0; i < p->nprog; i++)
        if (same(p->prog[i].name, name))
            return i;
    return -1;
}

BOOL prefs_prog_set(struct PrismPrefs *p, const char *name, BOOL keep)
{
    char t[PREFS_PROGNAME + 1];
    int i, n;
    while (*name == ' ')
        name++;
    n = strlen(name);
    while (n && name[n - 1] == ' ')
        n--;
    if (!n || n > PREFS_PROGNAME)
        return FALSE;
    memcpy(t, name, n);                  /* trimmed: "Doom " is Doom's entry */
    t[n] = 0;
    if ((i = prefs_prog_find(p, t)) < 0) {
        if (p->nprog >= PREFS_NPROGS)
            return FALSE;
        i = p->nprog++;
        strcpy(p->prog[i].name, t);
    }
    p->prog[i].keep = keep ? 1 : 0;
    return TRUE;
}

void prefs_prog_remove(struct PrismPrefs *p, int i)
{
    if (i < 0 || i >= p->nprog)
        return;
    for (; i + 1 < p->nprog; i++)
        p->prog[i] = p->prog[i + 1];
    p->nprog--;
}

BOOL prefs_size(const struct PrismPrefs *p, int i, UWORD *w, UWORD *h)
{
    int s = PREFS_SIZE(i);
    if (s < PREFS_NFIXED) {
        *w = prefs_sizes[s][0];
        *h = prefs_sizes[s][1];
        return TRUE;
    }
    if (PREFS_IS_CUSTOM(s)) {
        *w = p->custom[s - PREFS_NFIXED][0];
        *h = p->custom[s - PREFS_NFIXED][1];
        return *w != 0 && *h != 0;
    }
    *w = prefs_extra[s - PREFS_NFIXED - PREFS_NCUSTOM][0];
    *h = prefs_extra[s - PREFS_NFIXED - PREFS_NCUSTOM][1];
    return TRUE;
}

/* CUSTOM<n>=WxH: slot n's size; widths in multiples of 8 */
static void parse_custom(struct PrismPrefs *p, const char *key, char *v)
{
    unsigned w, h;
    int n = key[6] - '1';
    if (n < 0 || n >= PREFS_NCUSTOM || key[7] || sscanf(v, "%ux%u", &w, &h) != 2)
        return;
    if (w < 320 || w > 4096 || (w & 7) || h < 200 || h > 4096)
        return;
    p->custom[n][0] = w;
    p->custom[n][1] = h;
}

static void parse_mode(struct PrismPrefs *p, char *v)
{
    unsigned w, h, d, hz = 0;
    UWORD sw, sh;
    char state[8] = "ON";
    int i;

    if (sscanf(v, "%ux%ux%u %7s %u", &w, &h, &d, state, &hz) < 3)
        return;
    for (i = 0; i < PREFS_NMODES; i++)
        if (prefs_size(p, i, &sw, &sh) && sw == w && sh == h && prefs_depths[PREFS_DEPTH(i)] == d) {
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
        } else if (same(line, "SECONDBOARD")) {
            p->second = onoff(v);
        } else if (same(line, "PRIMARY")) {
            p->swapBoards = same(v, "SECOND");
        } else if (same(line, "BLITTER")) {
            p->blitter = onoff(v);
        } else if (same(line, "LOG")) {
            p->log = same(v, "SYNC") ? 2 : onoff(v);
        } else if (same(line, "PALETTE")) {
            p->clutBGR = same(v, "BGR");
        } else if (same(line, "MODE")) {
            parse_mode(p, v);
        } else if (!strnicmp(line, "CUSTOM", 6)) {
            parse_custom(p, line, v);
        } else if (same(line, "PROMOTE")) {
            p->promote = onoff(v);
        } else if (same(line, "PROMOTEPROG") || same(line, "KEEPPROG")) {
            prefs_prog_set(p, v, same(line, "KEEPPROG"));
        } else if (same(line, "HWOVERLAY")) {
            p->hwOverlay = onoff(v);
        } else if (same(line, "GAMMA")) {
            /* "1.4" or "14": tenths */
            unsigned a = 0, b = 0;
            int got = sscanf(v, "%u.%u", &a, &b);
            unsigned g = got == 2 ? a * 10 + (b % 10) : a >= 5 ? a : a * 10;
            if (g >= 5 && g <= 25)
                p->gamma = g;
        } else if (same(line, "SECONDMONITOR")) {
            p->secondMon = same(v, "MIRROR") ? SM_MIRROR : same(v, "SPAN") ? SM_SPAN :
                           same(v, "OFF") ? SM_OFF : SM_SCREEN;
        } else if (same(line, "SECONDSIDE")) {
            p->secondLeft = same(v, "LEFT");
        } else if (same(line, "MIRRORCHIPSET")) {
            p->mirrorChip = onoff(v);
        } else if (same(line, "POWERSAVE")) {
            unsigned m = 0;
            if (sscanf(v, "%u", &m) == 1 && m <= 240)
                p->powerSave = m;
            else if (same(v, "OFF"))
                p->powerSave = 0;
        } else if (same(line, "UPDATES")) {
            p->updates = same(v, "GITHUB") ? UPD_GITHUB : same(v, "MIRROR") ? UPD_MIRROR :
                         same(v, "BOTH") ? UPD_BOTH : UPD_OFF;
        } else if (same(line, "MIRROR")) {
            if (strlen(v) <= PREFS_MIRROR)
                strcpy(p->mirror, v);
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
    fprintf(f, "SECONDBOARD=%s\n", p->second ? "ON" : "OFF");
    fprintf(f, "PRIMARY=%s\n", p->swapBoards ? "SECOND" : "FIRST");
    fprintf(f, "SECONDMONITOR=%s\n", p->secondMon == SM_MIRROR ? "MIRROR" : p->secondMon == SM_SPAN ? "SPAN" :
                                     p->secondMon == SM_OFF ? "OFF" : "SCREEN");
    fprintf(f, "SECONDSIDE=%s\n", p->secondLeft ? "LEFT" : "RIGHT");
    fprintf(f, "BLITTER=%s\n", p->blitter ? "ON" : "OFF");
    fprintf(f, "LOG=%s\n", p->log == 2 ? "SYNC" : p->log ? "ON" : "OFF");
    if (p->clutBGR)
        fprintf(f, "PALETTE=BGR\n");
    fprintf(f, "PROMOTE=%s\n", p->promote ? "ON" : "OFF");
    fprintf(f, "GAMMA=%u.%u\n", p->gamma / 10, p->gamma % 10);
    fprintf(f, "HWOVERLAY=%s\n", p->hwOverlay ? "ON" : "OFF");
    fprintf(f, "MIRRORCHIPSET=%s\n", p->mirrorChip ? "ON" : "OFF");
    fprintf(f, "POWERSAVE=%u\n", p->powerSave);
    fprintf(f, "UPDATES=%s\n", p->updates == UPD_GITHUB ? "GITHUB" : p->updates == UPD_MIRROR ? "MIRROR" :
                               p->updates == UPD_BOTH ? "BOTH" : "OFF");
    if (p->mirror[0])
        fprintf(f, "MIRROR=%s\n", p->mirror);
    for (i = 0; i < p->nprog; i++)
        fprintf(f, "%s=%s\n", p->prog[i].keep ? "KEEPPROG" : "PROMOTEPROG", p->prog[i].name);
    /* the custom sizes before the MODE lines that name them */
    for (i = 0; i < PREFS_NCUSTOM; i++)
        if (p->custom[i][0] && p->custom[i][1])
            fprintf(f, "CUSTOM%d=%ux%u\n", i + 1, p->custom[i][0], p->custom[i][1]);
    for (i = 0; i < PREFS_NMODES; i++) {
        UWORD w, h;
        if (prefs_size(p, i, &w, &h))
            fprintf(f, "MODE=%ux%ux%u %s %u\n", w, h, prefs_depths[PREFS_DEPTH(i)],
                    p->on[i] ? "ON" : "OFF", p->hz[i]);
    }
    ok = !ferror(f);
    return fclose(f) == 0 && ok;
}
