/*
 * PrismNet GUIs: a row of tabs for GadTools windows (see tabs.h).
 *
 *    ____________  __________  ___________
 *   / Interfaces \/ Network  \/ Startup   \
 *  |              -------------------------  <- the page edge
 *
 * The active tab is raised and open at the bottom, joined to the page; the
 * others sit two pixels lower with the page edge under them. Plain
 * Intuition drawing and boolean gadgets: works from OS 2.04.
 * GPL-3.0-only, see LICENSE.
 *
 * PrismRTG: copied unchanged from PrismNet (tools/tabs.c, same author) for
 * PrismPrefs' tabbed window.
 */
#include <string.h>
#include <exec/types.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <graphics/rastport.h>
#include <proto/intuition.h>
#include <proto/graphics.h>

#include "tabs.h"

void tabs_init(struct Tabs *t, struct RastPort *rp, const char **labels, int n,
               UWORD firstid, WORD x, WORD y, WORD w)
{
    WORD fh = rp->TxHeight, need = 0, extra, i;

    memset(t->gad, 0, sizeof(t->gad));
    if (n > TABS_MAX) n = TABS_MAX;
    t->labels = labels;
    t->n = n;
    t->firstid = firstid;
    t->x = x;
    t->y = y;
    t->w = w;
    t->h = fh + 8;
    if (t->active >= n) t->active = 0;
    for (i = 0; i < n; i++)
        need += TextLength(rp, (STRPTR)labels[i], strlen(labels[i])) + 20;
    extra = need < w ? (w - need) / n : 0;  /* share the spare width out */
    t->tx[0] = x;
    for (i = 0; i < n; i++) {
        WORD tw = TextLength(rp, (STRPTR)labels[i], strlen(labels[i])) + 20 + extra;
        struct Gadget *g = &t->gad[i];
        t->tx[i + 1] = t->tx[i] + tw;
        g->LeftEdge = t->tx[i];
        g->TopEdge = y;
        g->Width = tw - 2;
        g->Height = t->h;
        g->Flags = GFLG_GADGHNONE;
        g->Activation = GACT_RELVERIFY;
        g->GadgetType = GTYP_BOOLGADGET;
        g->GadgetID = firstid + i;
        g->NextGadget = i + 1 < n ? &t->gad[i + 1] : NULL;
    }
}

void tabs_add(struct Tabs *t, struct Window *win)
{
    if (t->n)
        AddGList(win, &t->gad[0], ~0, t->n, NULL);
}

void tabs_remove(struct Tabs *t, struct Window *win)
{
    if (t->n)
        RemoveGList(win, &t->gad[0], t->n);
}

int tabs_hit(struct Tabs *t, UWORD id)
{
    return id >= t->firstid && id < t->firstid + t->n ? id - t->firstid : -1;
}

void tabs_draw(struct Tabs *t, struct Window *win)
{
    struct RastPort *rp = win->RPort;
    struct DrawInfo *dri = GetScreenDrawInfo(win->WScreen);
    UWORD shine = 2, shadow = 1, text = 1, hitext = 2, back = 0;
    WORD base = t->y + t->h - 1, i;

    if (dri) {
        shine  = dri->dri_Pens[SHINEPEN];
        shadow = dri->dri_Pens[SHADOWPEN];
        text   = dri->dri_Pens[TEXTPEN];
        hitext = dri->dri_Pens[HIGHLIGHTTEXTPEN];
        back   = dri->dri_Pens[BACKGROUNDPEN];
    }
    SetDrMd(rp, JAM1);
    SetAPen(rp, back);
    RectFill(rp, t->x, t->y, t->x + t->w - 1, base);
    /* the page edge, from the window's left to right, broken by the active tab */
    SetAPen(rp, shine);
    Move(rp, t->x, base);
    Draw(rp, t->x + t->w - 1, base);

    for (i = 0; i < t->n; i++) {
        BOOL on = i == t->active;
        WORD l = t->tx[i], r = t->tx[i + 1] - 3, top = t->y + (on ? 0 : 2);
        const char *s = t->labels[i];
        WORD tw = TextLength(rp, (STRPTR)s, strlen(s));

        if (on) {                       /* open at the bottom: join the page */
            SetAPen(rp, back);
            Move(rp, l + 1, base);
            Draw(rp, r - 1, base);
        }
        SetAPen(rp, shine);             /* left edge, top with rounded corners */
        Move(rp, l, base - (on ? 0 : 1));
        Draw(rp, l, top + 2);
        Draw(rp, l + 2, top);
        Draw(rp, r - 2, top);
        SetAPen(rp, shadow);            /* right edge */
        Move(rp, r - 1, top + 1);
        Draw(rp, r, top + 2);
        Draw(rp, r, base - (on ? 0 : 1));
        SetAPen(rp, on ? hitext : text);
        Move(rp, l + (r - l - tw) / 2, top + 4 + rp->TxBaseline);
        Text(rp, (STRPTR)s, strlen(s));
    }
    if (dri)
        FreeScreenDrawInfo(win->WScreen, dri);
}
