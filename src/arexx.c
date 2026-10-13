/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laine Jones */
/*
 * arexx.c - PrismD's ARexx port, "PRISM", for scripts: what the boards are
 * and what modes they offer, the Test picture, monitor power saving and the
 * debug log. docs/arexx.md lists the commands. PrismD's main loop looks at
 * the port every tick, so a command is answered within a frame.
 *
 * A script that shows 800x600 at 16 bits on the first board for five seconds
 * (after the comment line every ARexx script starts with):
 *
 *   ADDRESS PRISM
 *   OPTIONS RESULTS
 *   TEST 800 600 16
 *   ADDRESS COMMAND 'Wait 5'
 *   ADDRESS PRISM TESTEND
 */
#include <exec/types.h>
#include <exec/ports.h>
#include <exec/memory.h>
#include <rexx/storage.h>
#include <rexx/errors.h>
#include <proto/exec.h>
#include <proto/rexxsyslib.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "prism.h"
#include "prismint.h"
#include "prefs.h"

#define PORTNAME "PRISM"

struct RxsLib *RexxSysBase;
static struct MsgPort *port;
static char result[256];

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

/* the command's words, in place: up to 8 */
static int split(char *s, char **w)
{
    int n = 0;
    while (*s && n < 8) {
        while (*s == ' ' || *s == '\t')
            s++;
        if (!*s)
            break;
        w[n++] = s;
        while (*s && *s != ' ' && *s != '\t')
            s++;
        if (*s)
            *s++ = 0;
    }
    return n;
}

static ULONG num(const char *s, BOOL *ok)
{
    char *e;
    ULONG v = strtoul(s + (*s == '$'), &e, *s == '$' ? 16 : 0);
    if (ok) *ok = *s && !*e;
    return v;
}

/* the board a command names: its last word "2" or "BOARD=2" (default 1), 0-based */
static UBYTE board_arg(char **w, int *n)
{
    if (*n > 1 && (same(w[*n - 1], "2") || same(w[*n - 1], "BOARD=2"))) {
        (*n)--;
        return 1;
    }
    if (*n > 1 && (same(w[*n - 1], "1") || same(w[*n - 1], "BOARD=1")))
        (*n)--;
    return 0;
}

static const char help[] =
    "VERSION BOARDS BOARD VRAM MODES MODE MODEID TEST TESTEND POWERSAVE SLEEP WAKE DARK LOG HELP";

/* one command: RC_OK with `result` filled (or empty), else an RC_ */
static LONG command(char *line)
{
    char *w[8];
    int n = split(line, w);
    UBYTE k;
    BOOL ok;

    result[0] = 0;
    if (!n)
        return RC_WARN;
    if (same(w[0], "HELP")) {
        strcpy(result, help);
    } else if (same(w[0], "VERSION")) {
        snprintf(result, sizeof(result), "%s %s %u", PRISM_RELEASE, PRISM_RELDATE, PRISM_VERSION);
    } else if (same(w[0], "BOARDS")) {
        snprintf(result, sizeof(result), "%u", prism_ncards());
    } else if (same(w[0], "BOARD")) {
        k = board_arg(w, &n);
        if (k >= prism_ncards() || !prism_board_name(k))
            return RC_ERROR;
        snprintf(result, sizeof(result), "%s", prism_board_name(k));
    } else if (same(w[0], "VRAM")) {
        ULONG fr, all;
        k = board_arg(w, &n);
        if (k >= prism_ncards())
            return RC_ERROR;
        all = prism_board_vram(k, &fr);
        snprintf(result, sizeof(result), "%lu %lu", (unsigned long)(all >> 10), (unsigned long)(fr >> 10));
    } else if (same(w[0], "MODES")) {
        snprintf(result, sizeof(result), "%lu", (unsigned long)prism_mode_count());
    } else if (same(w[0], "MODE")) {
        /* MODE <n>: the n-th mode (1-based) as "$7A000101 640 480 16 PRISM:640x480 16bit" */
        struct PrismModeInfo mi;
        ULONG i = n > 1 ? num(w[1], &ok) : 0;
        if (n < 2 || !ok || !i || !prism_mode(i - 1, &mi))
            return RC_ERROR;
        snprintf(result, sizeof(result), "$%08lX %u %u %u %s", (unsigned long)mi.id, mi.w, mi.h,
                 mi.bpp == 1 ? 8 : mi.bpp == 3 ? 24 : mi.bpp == 4 ? 32 : 16, mi.name);
    } else if (same(w[0], "MODEID")) {
        /* MODEID <width> <height> <bits> [2]: the ModeID PrismD offers for it */
        struct PrismModeInfo mi;
        ULONG i, ww, hh, bb;
        BOOL o1, o2, o3;
        k = board_arg(w, &n);
        if (n < 4)
            return RC_ERROR;
        ww = num(w[1], &o1); hh = num(w[2], &o2); bb = num(w[3], &o3);
        if (!o1 || !o2 || !o3)
            return RC_ERROR;
        for (i = 0; prism_mode(i, &mi); i++) {
            UBYTE bits = mi.bpp == 1 ? 8 : mi.bpp == 3 ? 24 : mi.bpp == 4 ? 32 : 16;
            if (PRISM_ID_CARD(mi.id) == k && mi.w == ww && mi.h == hh && bits == bb) {
                snprintf(result, sizeof(result), "$%08lX", (unsigned long)mi.id);
                return RC_OK;
            }
        }
        return RC_WARN;                      /* not offered */
    } else if (same(w[0], "TEST")) {
        /* TEST <width> <height> <bits> [hz] [2]: the Test picture, until TESTEND */
        ULONG ww, hh, bb, hz = 0;
        BOOL o1, o2, o3, o4 = TRUE;
        LONG r;
        k = board_arg(w, &n);
        if (n < 4)
            return RC_ERROR;
        ww = num(w[1], &o1); hh = num(w[2], &o2); bb = num(w[3], &o3);
        if (n > 4)
            hz = num(w[4], &o4);
        if (!o1 || !o2 || !o3 || !o4 || k >= prism_ncards())
            return RC_ERROR;
        r = prism_test(k, ww, hh, bb, hz);
        snprintf(result, sizeof(result), "%s", r == PRISM_TEST_OK ? "OK" : r == PRISM_TEST_NOMODE ? "NOMODE" :
                 r == PRISM_TEST_NOMEM ? "NOMEM" : "BUSY");
        return r == PRISM_TEST_OK ? RC_OK : RC_WARN;
    } else if (same(w[0], "TESTEND")) {
        prism_test_stop();
    } else if (same(w[0], "POWERSAVE")) {
        /* POWERSAVE [minutes]: the minutes before the monitors go dark (0 = never) */
        UBYTE *p = prism_setting('P');
        if (n > 1) {
            ULONG m = same(w[1], "OFF") ? 0 : num(w[1], &ok);
            if (!same(w[1], "OFF") && (!ok || m > 240))
                return RC_ERROR;
            *p = m;
        }
        snprintf(result, sizeof(result), "%u", *p);
    } else if (same(w[0], "SLEEP")) {
        prism_sleep(TRUE);                   /* dark now, until an input or WAKE */
    } else if (same(w[0], "WAKE")) {
        prism_sleep(FALSE);
    } else if (same(w[0], "DARK")) {
        strcpy(result, prism_is_dark() ? "1" : "0");
    } else if (same(w[0], "LOG")) {
        UBYTE *p = prism_setting('L');
        if (n > 1) {
            if (same(w[1], "ON")) *p = 1;
            else if (same(w[1], "OFF")) *p = 0;
            else return RC_ERROR;
        }
        strcpy(result, *p ? "ON" : "OFF");
    } else
        return RC_ERROR;                     /* no such command */
    return RC_OK;
}

BOOL arexx_init(void)
{
    struct MsgPort *p;
    if (!(RexxSysBase = (struct RxsLib *)OpenLibrary((STRPTR)"rexxsyslib.library", 36)))
        return FALSE;
    Forbid();
    p = FindPort((STRPTR)PORTNAME);
    Permit();
    if (p || !(port = CreateMsgPort())) {
        CloseLibrary((struct Library *)RexxSysBase);
        RexxSysBase = NULL;
        return FALSE;
    }
    port->mp_Node.ln_Name = (char *)PORTNAME;
    port->mp_Node.ln_Pri = 0;
    AddPort(port);
    return TRUE;
}

void arexx_tick(void)
{
    struct RexxMsg *m;
    if (!port)
        return;
    while ((m = (struct RexxMsg *)GetMsg(port))) {
        char line[256];
        LONG rc;
        if (!IsRexxMsg(m) || !m->rm_Args[0]) {
            m->rm_Result1 = RC_FATAL;
            m->rm_Result2 = 0;
            ReplyMsg((struct Message *)m);
            continue;
        }
        strncpy(line, (const char *)m->rm_Args[0], sizeof(line) - 1);
        line[sizeof(line) - 1] = 0;
        rc = command(line);
        m->rm_Result1 = rc;
        m->rm_Result2 = 0;
        if (rc == RC_OK && (m->rm_Action & RXFF_RESULT) && result[0])
            m->rm_Result2 = (LONG)CreateArgstring((STRPTR)result, strlen(result));
        ReplyMsg((struct Message *)m);
    }
}

void arexx_quit(void)
{
    struct Message *m;
    if (!port)
        return;
    Forbid();
    RemPort(port);
    while ((m = GetMsg(port))) {
        ((struct RexxMsg *)m)->rm_Result1 = RC_FATAL;
        ((struct RexxMsg *)m)->rm_Result2 = 0;
        ReplyMsg(m);
    }
    Permit();
    DeleteMsgPort(port);
    port = NULL;
    CloseLibrary((struct Library *)RexxSysBase);
    RexxSysBase = NULL;
}
