/* port/tests/gdx_hud_placement_host_tests.c -- truth table for gdx_hud_element_on_top /
 * gdx_hud_top_element_mask / gdx_hud_top_suppressed / gdx_hud_minimap_suppressed
 * (port/gdx_hud_placement.c, nihudtop Phase 3).
 *
 * WHY THIS TEST EXISTS. Phase 3 turns the top-screen HUD's ALL_TOP/SPLIT/ALL_BOTTOM suppression
 * from one all-or-nothing flag into a per-element mask, with a 4th CUSTOM position reserved for
 * a later per-element UI. The one hard requirement carried over from Phase 2 is that modes 0-2
 * must still answer EXACTLY as they did before this file existed -- a regression there would be
 * silent on the top screen (same two bools, now derived from more code) until a coordinator
 * caught a visual diff. This test enumerates every (mode, element, numPlayers) combination modes
 * 0-2 can reach and checks it against the pre-Phase-3 rule directly (not against the new code's
 * own mask, which would just be testing the code against itself), then separately exercises
 * CUSTOM's mask override and the numPlayers>1 force.
 *
 * Compiled standalone (tools/ci-3ds.sh, -DGDX_HUD_PLACEMENT_HOST_TEST) against
 * port/gdx_hud_placement.c alone -- no LUS/controller/ini dependency, which is also why that
 * logic was split out of port/input_bridge.c into its own TU (see that file's new top comment).
 */
#include <stdio.h>
#include <string.h>

#include "gdx_hud_placement.h"

/* port/gdx_hud_placement.c, compiled under GDX_HUD_PLACEMENT_HOST_TEST only. */
void gdx_hud_placement_test_set_mode(int mode);
void gdx_hud_placement_test_set_fullview(int on);

static int sFailures = 0;

static void expect_eq(const char* what, int got, int want) {
    if (got != want) {
        printf("FAIL: %s -- got %d want %d\n", what, got, want);
        sFailures++;
    }
}

static const int kElements[] = {
    GDX_HUD_ELEM_TIME,   GDX_HUD_ELEM_LAP,     GDX_HUD_ELEM_INTERVAL,  GDX_HUD_ELEM_ENERGY,
    GDX_HUD_ELEM_RANK,   GDX_HUD_ELEM_SPEED,   GDX_HUD_ELEM_KO_STARS,  GDX_HUD_ELEM_LIVES,
    GDX_HUD_ELEM_REVERSE, GDX_HUD_ELEM_PORTRAITS, GDX_HUD_ELEM_MINIMAP,
};
#define N_ELEMENTS ((int) (sizeof(kElements) / sizeof(kElements[0])))

/* The pre-Phase-3 rule, reproduced independently of port/gdx_hud_placement.c so the test is not
 * just checking the implementation against itself: every element except MINIMAP follows
 * gdx_hud_top_suppressed's old SPLIT||ALL_BOTTOM rule; MINIMAP alone follows the old
 * ALL_BOTTOM-only rule. numPlayers>1 forces ALL_TOP regardless of the configured mode. */
static int legacy_on_top(int mode, int element, int numPlayers) {
    int effectiveMode = (numPlayers > 1) ? GDX_HUD_PLACEMENT_ALL_TOP : mode;
    if (element == GDX_HUD_ELEM_MINIMAP) {
        return effectiveMode != GDX_HUD_PLACEMENT_ALL_BOTTOM;
    }
    return (effectiveMode != GDX_HUD_PLACEMENT_SPLIT) && (effectiveMode != GDX_HUD_PLACEMENT_ALL_BOTTOM);
}

static void test_modes_0_to_2_match_legacy(void) {
    int mode, numPlayers, i, element, got, want;
    char label[96];

    for (mode = GDX_HUD_PLACEMENT_ALL_TOP; mode <= GDX_HUD_PLACEMENT_ALL_BOTTOM; mode++) {
        gdx_hud_placement_test_set_mode(mode);
        for (numPlayers = 1; numPlayers <= 2; numPlayers++) {
            for (i = 0; i < N_ELEMENTS; i++) {
                element = kElements[i];
                got = gdx_hud_element_on_top(element, numPlayers);
                want = legacy_on_top(mode, element, numPlayers);
                snprintf(label, sizeof(label), "mode=%d elem=%d numPlayers=%d", mode, element,
                         numPlayers);
                expect_eq(label, got, want);
            }
        }
    }
}

static void test_legacy_predicates_match_element_predicate(void) {
    int mode, numPlayers;
    char label[96];

    for (mode = GDX_HUD_PLACEMENT_ALL_TOP; mode <= GDX_HUD_PLACEMENT_ALL_BOTTOM; mode++) {
        gdx_hud_placement_test_set_mode(mode);
        for (numPlayers = 1; numPlayers <= 2; numPlayers++) {
            snprintf(label, sizeof(label), "top_suppressed mode=%d numPlayers=%d", mode, numPlayers);
            expect_eq(label, gdx_hud_top_suppressed(numPlayers),
                      !gdx_hud_element_on_top(GDX_HUD_ELEM_TIME, numPlayers));
            snprintf(label, sizeof(label), "minimap_suppressed mode=%d numPlayers=%d", mode, numPlayers);
            expect_eq(label, gdx_hud_minimap_suppressed(numPlayers),
                      !gdx_hud_element_on_top(GDX_HUD_ELEM_MINIMAP, numPlayers));
        }
    }
}

/* SPLIT must leave exactly MINIMAP on top and nothing else -- the one mode where the mask isn't
 * all-bits or no-bits, so it is worth pinning the exact bit pattern, not just per-element yes/no. */
static void test_split_mask_is_minimap_only(void) {
    unsigned mask;
    gdx_hud_placement_test_set_mode(GDX_HUD_PLACEMENT_SPLIT);
    mask = gdx_hud_top_element_mask(1);
    expect_eq("SPLIT mask == MINIMAP bit only", (int) mask, (int) (1u << GDX_HUD_ELEM_MINIMAP));
}

static void test_all_bottom_mask_is_zero(void) {
    gdx_hud_placement_test_set_mode(GDX_HUD_PLACEMENT_ALL_BOTTOM);
    expect_eq("ALL_BOTTOM mask == 0", (int) gdx_hud_top_element_mask(1), 0);
}

static void test_all_top_mask_is_every_bit(void) {
    unsigned mask;
    gdx_hud_placement_test_set_mode(GDX_HUD_PLACEMENT_ALL_TOP);
    mask = gdx_hud_top_element_mask(1);
    expect_eq("ALL_TOP mask has GDX_HUD_ELEM_MINIMAP set", (int) ((mask >> GDX_HUD_ELEM_MINIMAP) & 1u), 1);
    expect_eq("ALL_TOP mask has GDX_HUD_ELEM_TIME set", (int) ((mask >> GDX_HUD_ELEM_TIME) & 1u), 1);
    expect_eq("ALL_TOP mask has GDX_HUD_ELEM_RANK set", (int) ((mask >> GDX_HUD_ELEM_RANK) & 1u), 1);
}

/* CUSTOM: the whole point of Phase 3. An arbitrary mask, 1P only, must come back bit-for-bit
 * regardless of what gdx_hud_placement_mode() itself would say (ALL_TOP, from the host stub). */
static void test_custom_mask_1p_is_exact(void) {
    unsigned mask = (1u << GDX_HUD_ELEM_ENERGY) | (1u << GDX_HUD_ELEM_MINIMAP);
    int i, element, want, got;
    char label[96];

    gdx_hud_placement_test_set_mode(GDX_HUD_PLACEMENT_ALL_TOP); /* mode is irrelevant once active */
    gdx_hud_set_custom_top_mask(mask, 1);

    expect_eq("CUSTOM mask verbatim (1P)", (int) gdx_hud_top_element_mask(1), (int) mask);
    for (i = 0; i < N_ELEMENTS; i++) {
        element = kElements[i];
        want = (int) ((mask >> element) & 1u);
        got = gdx_hud_element_on_top(element, 1);
        snprintf(label, sizeof(label), "CUSTOM elem=%d (1P)", element);
        expect_eq(label, got, want);
    }

    /* top_suppressed: ENERGY is a Hud_DrawHud element and its bit is set, so Hud_DrawHud must
     * still be called even though TIME/LAP/etc. are off. */
    expect_eq("CUSTOM top_suppressed (ENERGY only, 1P)", gdx_hud_top_suppressed(1), 0);
    /* minimap_suppressed should track the MINIMAP bit, which is set here. */
    expect_eq("CUSTOM minimap_suppressed (MINIMAP set, 1P)", gdx_hud_minimap_suppressed(1), 0);

    gdx_hud_set_custom_top_mask(0, 1);
    expect_eq("CUSTOM top_suppressed (empty mask, 1P)", gdx_hud_top_suppressed(1), 1);
    expect_eq("CUSTOM minimap_suppressed (empty mask, 1P)", gdx_hud_minimap_suppressed(1), 1);

    gdx_hud_set_custom_top_mask(0, 0); /* leave CUSTOM deactivated for later tests */
}

/* FORCED ALL_TOP IN 2-4P must hold even with CUSTOM active -- a stray dev-ini mask must never
 * touch multiplayer. */
static void test_custom_mask_ignored_above_1p(void) {
    unsigned mask = 1u << GDX_HUD_ELEM_ENERGY; /* deliberately sparse */
    int numPlayers;

    gdx_hud_placement_test_set_mode(GDX_HUD_PLACEMENT_ALL_TOP);
    gdx_hud_set_custom_top_mask(mask, 1);

    for (numPlayers = 2; numPlayers <= 4; numPlayers++) {
        char label[64];
        snprintf(label, sizeof(label), "CUSTOM ignored at numPlayers=%d", numPlayers);
        expect_eq(label, (int) gdx_hud_top_element_mask(numPlayers), (int) ~0u);
    }

    gdx_hud_set_custom_top_mask(0, 0);
}

/* decomp/src/overlays/ovl_i3/hud.c and menus.c cannot #include gdx_hud_placement.h (no port/
 * include path from decomp/src), so the two decomp patches (decomp-3ds-hud-placement.patch's
 * follow-on, decomp-native-hud-elem-gate.patch) each carry their own local mirror of the bit
 * positions/element numbers they need. This pins the real enum's numeric values so a future
 * reorder of GDX_HUD_ELEM_* fails HERE, loudly, instead of silently mis-gating a HUD piece in a
 * decomp patch nothing here recompiles. */
static void test_element_enum_numbering_is_pinned(void) {
    expect_eq("GDX_HUD_ELEM_TIME", GDX_HUD_ELEM_TIME, 0);
    expect_eq("GDX_HUD_ELEM_LAP", GDX_HUD_ELEM_LAP, 1);
    expect_eq("GDX_HUD_ELEM_INTERVAL", GDX_HUD_ELEM_INTERVAL, 2);
    expect_eq("GDX_HUD_ELEM_ENERGY", GDX_HUD_ELEM_ENERGY, 3);
    expect_eq("GDX_HUD_ELEM_RANK", GDX_HUD_ELEM_RANK, 4);
    expect_eq("GDX_HUD_ELEM_SPEED", GDX_HUD_ELEM_SPEED, 5);
    expect_eq("GDX_HUD_ELEM_KO_STARS", GDX_HUD_ELEM_KO_STARS, 6);
    expect_eq("GDX_HUD_ELEM_LIVES", GDX_HUD_ELEM_LIVES, 7);
    expect_eq("GDX_HUD_ELEM_REVERSE", GDX_HUD_ELEM_REVERSE, 8);
    expect_eq("GDX_HUD_ELEM_PORTRAITS", GDX_HUD_ELEM_PORTRAITS, 9);
    expect_eq("GDX_HUD_ELEM_MINIMAP", GDX_HUD_ELEM_MINIMAP, 10);
}

static void test_placement_name(void) {
    expect_eq("name(ALL_TOP)", strcmp(gdx_hud_placement_name(GDX_HUD_PLACEMENT_ALL_TOP), "ALL_TOP"), 0);
    expect_eq("name(SPLIT)", strcmp(gdx_hud_placement_name(GDX_HUD_PLACEMENT_SPLIT), "SPLIT"), 0);
    expect_eq("name(ALL_BOTTOM)", strcmp(gdx_hud_placement_name(GDX_HUD_PLACEMENT_ALL_BOTTOM), "ALL_BOTTOM"), 0);
    expect_eq("name(out of range)", strcmp(gdx_hud_placement_name(99), "ALL_TOP"), 0);
}

/* o3ds60 [ui] mp_hud_placement: an MP fullview frame (hook on) answers as 1P for every mode; with
 * the hook off 2P still forces ALL_TOP; a 1P frame is unaffected either way. */
static void test_mp_fullview_follows_placement(void) {
    int mode;
    for (mode = GDX_HUD_PLACEMENT_ALL_TOP; mode <= GDX_HUD_PLACEMENT_ALL_BOTTOM; mode++) {
        unsigned oneP;
        gdx_hud_placement_test_set_mode(mode);
        gdx_hud_placement_test_set_fullview(0);
        oneP = gdx_hud_top_element_mask(1);
        expect_eq("fv off: 2P forced ALL_TOP", (int) (gdx_hud_top_element_mask(2) == ~0u), 1);
        gdx_hud_placement_test_set_fullview(1);
        expect_eq("fv on: 2P mask == 1P mask", (int) (gdx_hud_top_element_mask(2) == oneP), 1);
        expect_eq("fv on: 1P mask unchanged", (int) (gdx_hud_top_element_mask(1) == oneP), 1);
        expect_eq("fv on: 2P suppressed == 1P", gdx_hud_top_suppressed(2), gdx_hud_top_suppressed(1));
    }
    gdx_hud_placement_test_set_fullview(0);
    gdx_hud_placement_test_set_mode(GDX_HUD_PLACEMENT_ALL_TOP);
}

int main(void) {
    test_modes_0_to_2_match_legacy();
    test_legacy_predicates_match_element_predicate();
    test_split_mask_is_minimap_only();
    test_all_bottom_mask_is_zero();
    test_all_top_mask_is_every_bit();
    test_custom_mask_1p_is_exact();
    test_custom_mask_ignored_above_1p();
    test_element_enum_numbering_is_pinned();
    test_placement_name();
    test_mp_fullview_follows_placement();

    if (sFailures != 0) {
        printf("=== %d FAILURE(S) ===\n", sFailures);
        return 1;
    }
    printf("=== ALL PASS (modes 0-2 x %d elements x numPlayers 1-2, plus CUSTOM) ===\n", N_ELEMENTS);
    return 0;
}
