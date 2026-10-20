/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laine Jones */
/*
 * upd_prismrtg.c - the update settings are UPDATES= and MIRROR= in
 * ENV:Prism.prefs (PrismPrefs writes them), read with the usual prefs code.
 */
#include <string.h>
#include "upd_cfg.h"

UBYTE upd_load_prefs(char *mirror, int n)
{
    static struct PrismPrefs prefs;
    prefs_load(&prefs, PREFS_ENV);
    strncpy(mirror, prefs.mirror, n - 1);
    mirror[n - 1] = 0;
    return prefs.updates;
}
