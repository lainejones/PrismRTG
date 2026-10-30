/*
 * PrismNet GUIs: a row of tabs for GadTools windows (OS 2.04+, no ClickTab).
 * The tabs are drawn with the screen's pens; each has an invisible Intuition
 * gadget for the click. GPL-3.0-only, see LICENSE.
 *
 * PrismRTG: copied unchanged from PrismNet (tools/tabs.h, same author) for
 * PrismPrefs' tabbed window.
 */
#ifndef PRISMNET_TABS_H
#define PRISMNET_TABS_H

#include <exec/types.h>
#include <intuition/intuition.h>

#define TABS_MAX 8

struct Tabs {
    struct Gadget  gad[TABS_MAX];
    const char   **labels;
    int            n, active;
    UWORD          firstid;         /* GadgetID of tab 0; tab i is firstid + i */
    WORD           x, y, w, h;      /* the tab row; the page starts at y + h */
    WORD           tx[TABS_MAX + 1];
};

/* Lay the tabs out across x..x+w at y (height from the font). */
void  tabs_init(struct Tabs *t, struct RastPort *rp, const char **labels, int n,
                UWORD firstid, WORD x, WORD y, WORD w);
/* Add the click gadgets to the window, remove them again. */
void  tabs_add(struct Tabs *t, struct Window *win);
void  tabs_remove(struct Tabs *t, struct Window *win);
/* Draw the row (and the top edge of the page under it). */
void  tabs_draw(struct Tabs *t, struct Window *win);
/* GadgetID -> tab index, or -1 if it is not one of ours */
int   tabs_hit(struct Tabs *t, UWORD id);

#endif
