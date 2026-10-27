/* port/3ds/gdx3ds_menu_tabbar_layout.h — the MENU tab bar's column math, pulled out of
 * gdx3ds_menu.c so it can be host-tested without the rest of that TU (console I/O, 3ds.h,
 * the live gdx3ds_bottomhud_* hooks) — see gdx3ds_menu_tabbar_host_tests.c.
 *
 * Pure, header-only, no includes beyond <string.h>: gdx3ds_menu.c's MenuLayoutTabs() and the
 * host test both call gdx3ds_menu_layout_tabs() directly, so there is exactly one copy of
 * this arithmetic — the production tab bar and the test can never silently drift apart.
 *
 * hw bug n60-3d8, item 1 ("RACE tab unreachable, OFF covers it"): the algorithm below was
 * never the bug — every MAP/RACE combination it computes already leaves OFF clear of every
 * visible tab (gdx3ds_menu_tabbar_host_tests.c checks exactly that, for all 4 combinations).
 * The bug was staleness: gdx3ds_menu.c used to call this layout exactly once, at init, and
 * cache the result for the rest of the process's life — but TAB_RACE's own visibility is a
 * live, mid-race condition (gdx3ds_bottomhud_dismissed()/race_live()), almost always false at
 * init. PaintTabBar() re-checks that live visibility on every paint and draws whatever is
 * actually visible RIGHT NOW, so once RACE became reachable mid-race its label was drawn —
 * but TouchTabBar() was still hit-testing (and OFF was still drawn at) the STALE, init-time
 * columns, which assumed RACE was still hidden. OFF's touch box — checked first — ended up
 * starting inside the columns RACE's label was actually drawn in, so OFF silently ate the
 * tap. The fix (gdx3ds_menu.c) is to call this every tick, not just at init, so the layout
 * paint and touch handling consume in a given tick is never older than that tick's own live
 * visibility. This header only has to guarantee the ALGORITHM is overlap-free for every
 * combination; gdx3ds_menu.c is what guarantees it is never fed a stale combination. */
#ifndef GDX3DS_MENU_TABBAR_LAYOUT_H
#define GDX3DS_MENU_TABBAR_LAYOUT_H

#include <string.h>

typedef struct Gdx3dsMenuTabCols {
    int c0, c1; /* 1-based, inclusive; c1 < c0 (e.g. c0=0,c1=-1) means "not touchable". */
} Gdx3dsMenuTabCols;

/* labels[i]/hidden[i] for i in [0, count). menuCols is the console width (40 for the 3DS
 * bottom screen's 8px font over 320px). Fills outCols[0..count), *outOffC0, *outOffC1 (the
 * OFF button's own column span, always clamped to end exactly at menuCols). */
static inline void gdx3ds_menu_layout_tabs(const char* const* labels, const int* hidden, int count,
                                            int menuCols, Gdx3dsMenuTabCols* outCols, int* outOffC0,
                                            int* outOffC1) {
    int c = 1;
    int t;
    for (t = 0; t < count; t++) {
        if (hidden[t]) {
            outCols[t].c0 = 0; /* never matched by a touch: columns are 1-based */
            outCols[t].c1 = -1;
            continue;
        }
        outCols[t].c0 = c;
        outCols[t].c1 = c + (int) strlen(labels[t]) - 1;
        c = outCols[t].c1 + 2; /* one-column gap before the next visible tab */
    }
    *outOffC0 = c;
    if (*outOffC0 > menuCols - 2) {
        *outOffC0 = menuCols - 2; /* at least the 3-column reverse-video "OFF" always fits */
    }
    *outOffC1 = menuCols;
}

/* mp-online: the MP tab ([net] rendezvous set) makes the worst case (MAP and RACE both shown)
 * one tab too wide for the 40 columns. When the visible tabs would run into the OFF button's
 * minimum 3-column span (menuCols-2..menuCols), the tab `dropIndex` (the least needed one: AB)
 * is hidden instead. Returns 1 if it was. Call before gdx3ds_menu_layout_tabs, with the same
 * hidden[]. Without an MP tab no combination is crowded, so the bar is the old one. */
static inline int gdx3ds_menu_tabs_crowd(const char* const* labels, int* hidden, int count, int menuCols,
                                         int dropIndex) {
    int c = 1;
    int last = 0;
    int t;
    for (t = 0; t < count; t++) {
        if (hidden[t]) {
            continue;
        }
        last = c + (int) strlen(labels[t]) - 1;
        c = last + 2;
    }
    if (last > menuCols - 3 && dropIndex >= 0 && dropIndex < count && !hidden[dropIndex]) {
        hidden[dropIndex] = 1;
        return 1;
    }
    return 0;
}

#endif /* GDX3DS_MENU_TABBAR_LAYOUT_H */
