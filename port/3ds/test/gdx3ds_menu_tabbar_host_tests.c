/* port/3ds/test/gdx3ds_menu_tabbar_host_tests.c -- the MENU tab bar's column layout
 * (gdx3ds_menu_tabbar_layout.h), against every combination of the two tabs whose presence is
 * dynamic: MAP ([debug] bottom_map) and RACE (reachable only mid-race, once the DASH takeover
 * has been dismissed -- see gdx3ds_menu.c's TAB_RACE enum comment).
 *
 * hw bug n60-3d8, item 1: "it had the race tab on debug but was unclickable because OFF was
 * cover[ing] it". The algorithm under test here was never actually the bug (every combination
 * below already comes out overlap-free -- that is exactly what this file proves, and keeps
 * proving on every future label change). The real bug was gdx3ds_menu.c calling this layout
 * exactly once, at init, and caching the result while RACE's actual visibility changed live
 * underneath it -- see MenuLayoutTabs()'s own comment in that file for the fix. This test
 * exists so the one thing that WOULD still be a real bug -- the layout math itself producing
 * an overlap for some future label set -- gets caught here, on a host, instead of on
 * hardware.
 *
 * kLabels below is a hand copy of gdx3ds_menu.c's kTabLabels (TAB_RACE..TAB_ABOUT, i.e.
 * TAB_COUNT == 11) -- keep the two in sync if that array ever changes. kLabelsEk is the
 * Expansion Kit build's set (GDX3DS_COURSE_SHARE adds CRS after MAP), with its rule that CRS
 * steps aside when MAP and RACE are both shown (gdx3ds_menu.c MENU_TAB_CRS_HIDDEN) -- a
 * combination that cannot fit 40 columns, which the overflow probe below demonstrates.
 *
 * mp-online: a further dynamic tab, MP ([net] rendezvous / mp_lan). When the visible labels no
 * longer fit before OFF, gdx3ds_menu_tabs_crowd() hides AB (and only then): MAP + RACE + MP, and
 * in the EK set CRS + MP with MAP or RACE. */
#include <stdio.h>
#include <string.h>

#include "gdx3ds_menu_tabbar_layout.h"

#define MENU_COLS 40
#define TAB_COUNT 11
#define TAB_COUNT_EK 12
#define TAB_COUNT_MAX TAB_COUNT_EK
/* uipolish: TAB_RACE (EXIT) is first, so it sits at column 1 whenever it is shown. */
#define TAB_RACE 0
#define TAB_MAP 8
/* shipped set */
#define TAB_MP 9
#define TAB_ABOUT 10
/* EK set */
#define TAB_CRS_EK 9
#define TAB_MP_EK 10
#define TAB_ABOUT_EK 11

static const char* const kLabels[TAB_COUNT] = {
    "EXIT", "STA", "DSP", "3D", "AUD", "IN", "LG", "DBG", "MAP", "MP", "AB",
};

static const char* const kLabelsEk[TAB_COUNT_EK] = {
    "EXIT", "STA", "DSP", "3D", "AUD", "IN", "LG", "DBG", "MAP", "CRS", "MP", "AB",
};

static int sFailures = 0;

static void Check(int ok, const char* what) {
    if (ok) {
        printf("ok   %s\n", what);
    } else {
        printf("FAIL %s\n", what);
        sFailures++;
    }
}

/* 1 if [a0,a1] and [b0,b1] (both inclusive, 1-based) share a column. An entry with c1 < c0
 * (the "hidden" sentinel, e.g. {0,-1}) can never overlap anything -- the range is empty. */
static int RangesOverlap(int a0, int a1, int b0, int b1) {
    if (a1 < a0 || b1 < b0) {
        return 0;
    }
    return a0 <= b1 && b0 <= a1;
}

/* One label set and one combination. crsIdx < 0: the set has no CRS tab (the shipped build).
 * crowd: apply gdx3ds_menu_tabs_crowd() as gdx3ds_menu.c does (expectDrop = whether AB must go).
 * Returns the failures. */
static int CheckLayout(const char* const* labels, int count, int mpIdx, int abIdx, int crsIdx, int mapShown,
                       int raceShown, int crsShown, int mpShown, int crowd, int expectDrop) {
    int hidden[TAB_COUNT_MAX];
    Gdx3dsMenuTabCols cols[TAB_COUNT_MAX];
    int offC0 = -1, offC1 = -1;
    char tag[64];
    int t, u;
    const int failuresBefore = sFailures;

    snprintf(tag, sizeof(tag), "%s MAP=%d RACE=%d CRS=%d MP=%d", crsIdx >= 0 ? "EK" : "ship", mapShown, raceShown,
             crsShown, mpShown);

    for (t = 0; t < count; t++) {
        hidden[t] = 0;
    }
    hidden[TAB_MAP] = !mapShown;
    hidden[TAB_RACE] = !raceShown;
    hidden[mpIdx] = !mpShown;
    if (crsIdx >= 0) {
        hidden[crsIdx] = !crsShown;
    }
    if (crowd) {
        const int dropped = gdx3ds_menu_tabs_crowd(labels, hidden, count, MENU_COLS, abIdx);
        char what[96];
        snprintf(what, sizeof(what), "%s: AB hidden only when crowded (%d)", tag, expectDrop);
        Check(dropped == expectDrop && hidden[abIdx] == expectDrop, what);
        if (mpShown) {
            snprintf(what, sizeof(what), "%s: MP stays visible", tag);
            Check(!hidden[mpIdx], what);
        }
    }

    gdx3ds_menu_layout_tabs(labels, hidden, count, MENU_COLS, cols, &offC0, &offC1);

    /* --- within 320px / 40 columns, and OFF always reaches the right edge exactly. ----- */
    {
        char what[96];
        snprintf(what, sizeof(what), "%s: offC0 >= 1", tag);
        Check(offC0 >= 1, what);
        snprintf(what, sizeof(what), "%s: offC0 <= offC1", tag);
        Check(offC0 <= offC1, what);
        snprintf(what, sizeof(what), "%s: offC1 == MENU_COLS (40)", tag);
        Check(offC1 == MENU_COLS, what);
    }
    for (t = 0; t < count; t++) {
        char what[96];
        if (hidden[t]) {
            snprintf(what, sizeof(what), "%s: tab %d hidden -> unreachable (c1 < c0)", tag, t);
            Check(cols[t].c1 < cols[t].c0, what);
            continue;
        }
        snprintf(what, sizeof(what), "%s: tab %d c0 >= 1", tag, t);
        Check(cols[t].c0 >= 1, what);
        snprintf(what, sizeof(what), "%s: tab %d c1 <= MENU_COLS (40)", tag, t);
        Check(cols[t].c1 <= MENU_COLS, what);
        snprintf(what, sizeof(what), "%s: tab %d c0 <= c1", tag, t);
        Check(cols[t].c0 <= cols[t].c1, what);
    }

    /* --- uipolish: EXIT is in the same place on every page. ---------------------------- */
    if (raceShown) {
        char what[96];
        snprintf(what, sizeof(what), "%s: EXIT at columns 1-4", tag);
        Check(cols[TAB_RACE].c0 == 1 && cols[TAB_RACE].c1 == 4, what);
    }

    /* --- no overlap between any two VISIBLE tabs. -------------------------------------- */
    for (t = 0; t < count; t++) {
        if (hidden[t]) {
            continue;
        }
        for (u = t + 1; u < count; u++) {
            char what[96];
            if (hidden[u]) {
                continue;
            }
            snprintf(what, sizeof(what), "%s: tab %d/%d do not overlap", tag, t, u);
            Check(!RangesOverlap(cols[t].c0, cols[t].c1, cols[u].c0, cols[u].c1), what);
        }
    }

    /* --- the actual n60-3d8 bug: OFF must not overlap any visible tab, MAP/RACE above
     * all -- this is the exact pair the hardware report was about. --------------------- */
    for (t = 0; t < count; t++) {
        char what[96];
        if (hidden[t]) {
            continue;
        }
        snprintf(what, sizeof(what), "%s: tab %d does not overlap OFF", tag, t);
        Check(!RangesOverlap(cols[t].c0, cols[t].c1, offC0, offC1), what);
    }
    return sFailures - failuresBefore;
}

static void CheckCombo(int mapShown, int raceShown, int mpShown) {
    CheckLayout(kLabels, TAB_COUNT, TAB_MP, TAB_ABOUT, -1, mapShown, raceShown, 0, mpShown, 1,
                mapShown && raceShown && mpShown);
}

/* The EK build: CRS shown unless MAP and RACE both are (gdx3ds_menu.c MENU_TAB_CRS_HIDDEN), and
 * with course_share = 0 (CRS never shown). */
static void CheckComboEk(int mapShown, int raceShown, int mpShown) {
    const int crs = !(mapShown && raceShown);
    CheckLayout(kLabelsEk, TAB_COUNT_EK, TAB_MP_EK, TAB_ABOUT_EK, TAB_CRS_EK, mapShown, raceShown, crs, mpShown, 1,
                mpShown && ((crs && (mapShown || raceShown)) || (mapShown && raceShown)));
    CheckLayout(kLabelsEk, TAB_COUNT_EK, TAB_MP_EK, TAB_ABOUT_EK, TAB_CRS_EK, mapShown, raceShown, 0, mpShown, 1,
                mapShown && raceShown && mpShown);
}

/* Why the CRS rule exists: MAP + RACE + CRS does NOT fit, even without MP and with no crowding.
 * Run silently, expect failures. */
static void CheckEkOverflowIsReal(void) {
    const int saved = sFailures;
    int overflowFailures;
    printf("(expected-overflow probe: MAP+RACE+CRS; FAIL lines below are expected)\n");
    overflowFailures = CheckLayout(kLabelsEk, TAB_COUNT_EK, TAB_MP_EK, TAB_ABOUT_EK, TAB_CRS_EK, 1, 1, 1, 0, 0, 0);
    sFailures = saved;
    Check(overflowFailures > 0, "EK: MAP+RACE+CRS together overflow 40 columns (why CRS steps aside)");
}

int main(void) {
    int mp;
    for (mp = 0; mp <= 1; mp++) {
        CheckCombo(0, 0, mp);
        CheckCombo(1, 0, mp);
        CheckCombo(0, 1, mp);
        CheckCombo(1, 1, mp); /* the worst case (MP=0: the one hardware actually hit) */
        CheckComboEk(0, 0, mp);
        CheckComboEk(1, 0, mp);
        CheckComboEk(0, 1, mp);
        CheckComboEk(1, 1, mp);
    }
    CheckEkOverflowIsReal();

    printf(sFailures == 0 ? "=== ALL PASS (0 failures) ===\n" : "=== %d FAILURES ===\n", sFailures);
    return sFailures == 0 ? 0 : 1;
}
