/* port/3ds/gdx3ds_bui_layout.h — the bottom-screen UI's pure logic: widget hit tests, D-pad focus
 * navigation, the developer-console combo and the game-key mask's release rule. Header-only and
 * free of <3ds.h> so gdx3ds_bui.c (the 3DS shell), gdx3ds_bui_screens.c (the painters, which
 * register the widgets) and the host tests (port/3ds/test/gdx3ds_bui_host_tests.c) share it. */
#ifndef GDX3DS_BUI_LAYOUT_H
#define GDX3DS_BUI_LAYOUT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- widgets: what a painted screen can be touched / focused on ---------------------------- */

#define BUI_MAX_WIDGETS 48

enum {
    BUI_WF_FOCUS = 1,    /* reachable with the D-pad */
    BUI_WF_DISABLED = 2, /* drawn disabled: a touch or A does nothing (the screen may explain) */
};

typedef struct BuiWidget {
    int16_t x, y, w, h;
    int16_t id;    /* screen-defined, stable across repaints (focus is kept by id) */
    int16_t flags; /* BUI_WF_* */
} BuiWidget;

typedef struct BuiWidgets {
    int count;
    BuiWidget w[BUI_MAX_WIDGETS];
} BuiWidgets;

static inline void bui_widgets_clear(BuiWidgets* ws) {
    ws->count = 0;
}

static inline void bui_widgets_add(BuiWidgets* ws, int x, int y, int w, int h, int id, int flags) {
    BuiWidget* b;
    if (ws == 0 || ws->count >= BUI_MAX_WIDGETS) {
        return;
    }
    b = &ws->w[ws->count++];
    b->x = (int16_t) x;
    b->y = (int16_t) y;
    b->w = (int16_t) w;
    b->h = (int16_t) h;
    b->id = (int16_t) id;
    b->flags = (int16_t) flags;
}

static inline const BuiWidget* bui_widgets_find(const BuiWidgets* ws, int id) {
    int i;
    for (i = 0; i < ws->count; i++) {
        if (ws->w[i].id == id) {
            return &ws->w[i];
        }
    }
    return 0;
}

/* The widget under (x, y): the LAST one added wins, so a dialog registered after the screen
 * below it takes the touch. -1 = none. */
static inline int bui_widgets_hit(const BuiWidgets* ws, int x, int y) {
    int i;
    for (i = ws->count - 1; i >= 0; i--) {
        const BuiWidget* b = &ws->w[i];
        if (x >= b->x && x < b->x + b->w && y >= b->y && y < b->y + b->h) {
            return b->id;
        }
    }
    return -1;
}

/* The first focusable, enabled widget (the default focus of a fresh screen). -1 = none. */
static inline int bui_widgets_first_focus(const BuiWidgets* ws) {
    int i;
    for (i = 0; i < ws->count; i++) {
        if ((ws->w[i].flags & BUI_WF_FOCUS) && !(ws->w[i].flags & BUI_WF_DISABLED)) {
            return ws->w[i].id;
        }
    }
    return -1;
}

static inline int BuiAbs(int v) {
    return v < 0 ? -v : v;
}

/* Spatial D-pad navigation: from widget `cur`, the nearest focusable widget whose centre lies in
 * direction (dx, dy) (one of them +-1, the other 0). Distance along the direction counts once and
 * the sideways offset twice, so a row/column neighbour beats a diagonal one. Disabled widgets are
 * reachable (focus can rest on a disabled START so its reason is read) only when `allowDisabled`.
 * Returns `cur` when nothing lies that way. */
static inline int bui_widgets_nav(const BuiWidgets* ws, int cur, int dx, int dy, int allowDisabled) {
    const BuiWidget* from = bui_widgets_find(ws, cur);
    int best = cur;
    long bestScore = 0x7FFFFFFFL;
    int cx, cy, i;
    if (from == 0) {
        return bui_widgets_first_focus(ws);
    }
    cx = from->x * 2 + from->w;
    cy = from->y * 2 + from->h;
    for (i = 0; i < ws->count; i++) {
        const BuiWidget* b = &ws->w[i];
        const int bx = b->x * 2 + b->w;
        const int by = b->y * 2 + b->h;
        const int along = dx != 0 ? (bx - cx) * dx : (by - cy) * dy;
        const int side = dx != 0 ? BuiAbs(by - cy) : BuiAbs(bx - cx);
        long score;
        if (b->id == cur || !(b->flags & BUI_WF_FOCUS)) {
            continue;
        }
        if ((b->flags & BUI_WF_DISABLED) && !allowDisabled) {
            continue;
        }
        if (along <= 0) {
            continue;
        }
        score = (long) along + 2L * side;
        if (score < bestScore) {
            bestScore = score;
            best = b->id;
        }
    }
    return best;
}

/* ---- the developer-console combo ---------------------------------------------------------- */

/* Holding every key of `combo` (L+R+SELECT) for `thresholdMs` of wall-clock time fires ONCE; it
 * re-arms only after the combo is let go. card-4b: wall time, not menu ticks (90 ticks were 3 s
 * whenever the main loop ran at 30 Hz), and every edge is an event so the caller can log a
 * receipt: hardware logs then show a chord that was let go too early, not just one that fired. */
typedef struct BuiCombo {
    int down;         /* every key of the combo is held */
    int fired;        /* ... and this hold already fired */
    uint32_t startMs; /* when the hold started */
} BuiCombo;

enum {
    BUI_COMBO_NONE = 0,     /* nothing changed */
    BUI_COMBO_DOWN,         /* the last key of the combo went down: the hold starts */
    BUI_COMBO_FIRED,        /* held for thresholdMs: fire */
    BUI_COMBO_RELEASED_EARLY /* let go before thresholdMs: no fire */
};

static inline int bui_combo_step(BuiCombo* c, uint32_t held, uint32_t combo, uint32_t nowMs, uint32_t thresholdMs,
                                 uint32_t* heldMs) {
    const int all = (held & combo) == combo;
    *heldMs = c->down ? nowMs - c->startMs : 0;
    if (!all) {
        const int early = c->down && !c->fired;
        c->down = 0;
        c->fired = 0;
        return early ? BUI_COMBO_RELEASED_EARLY : BUI_COMBO_NONE;
    }
    if (!c->down) {
        c->down = 1;
        c->fired = 0;
        c->startMs = nowMs;
        *heldMs = 0;
        return BUI_COMBO_DOWN;
    }
    if (!c->fired && *heldMs >= thresholdMs) {
        c->fired = 1;
        return BUI_COMBO_FIRED;
    }
    return BUI_COMBO_NONE;
}

/* ---- the game-key mask --------------------------------------------------------------------- */

/* While a flow owns the buttons every key in `maskable` is hidden from the game. When the flow
 * lets go, a key still held stays hidden until it is released, so the press that closed the flow
 * (B on the last screen) never reaches the game as a fresh press. Returns the keys to hide. */
typedef struct BuiKeyMask {
    uint32_t sticky;
} BuiKeyMask;

static inline uint32_t bui_keymask_step(BuiKeyMask* m, int flowOwnsButtons, uint32_t held, uint32_t maskable) {
    if (flowOwnsButtons) {
        m->sticky = held & maskable;
        return maskable;
    }
    m->sticky &= held;
    return m->sticky;
}

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_BUI_LAYOUT_H */
