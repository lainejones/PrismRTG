/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laine Jones */
/*
 * upd_cfg.h - PrismRTG's settings for the shared update tool (update.c, the
 * same file as PrismNet's tools/update/update.c: change it in both).
 */
#ifndef UPD_CFG_H
#define UPD_CFG_H

#include <exec/types.h>
#include "../../src/prism.h"
#include "../../src/prefs.h"        /* UPD_OFF..UPD_BOTH, PREFS_MIRROR */

#define UPD_TOOL        "PrismUpdate"               /* the command, and its messages */
#define UPD_PRODUCT     "PrismRTG"                  /* T:<product>-update.lha, APPNAME */
#define UPD_REPO        "lainejones/PrismRTG"       /* GitHub owner/repository */
#define UPD_ASSET       "PrismRTG-"                 /* the release archive: <asset><version>.lha */
#define UPD_INSTALLER   "Install_PrismRTG"          /* the Installer script in the archive */
#ifndef UPD_VERSION
#define UPD_VERSION     PRISM_RELEASE               /* this release */
#endif
#define UPD_RELDATE     PRISM_RELDATE
#define UPD_PREFS_NAME  PREFS_ENV                   /* named in messages */
#define UPD_MIRROR_MAX  PREFS_MIRROR

/* the product's settings: returns UPDATES= (UPD_*), fills MIRROR= */
UBYTE upd_load_prefs(char *mirror, int n);

#endif
