// port/gdx_hud_placement.c -- the HUD placement lever's predicates (nibottom Phase 2 / nihudtop
// Phase 3). Split out of port/input_bridge.c on purpose: this file has no LUS/SDL/controller
// dependency, so it compiles standalone for the host unit test
// (port/tests/gdx_hud_placement_host_tests.c, tools/ci-3ds.sh), the same way
// port/3ds/gfx/gdx3ds_native_cfg.c and port/3ds/gdx3ds_dynlod.c already do for their own tests.
//
// See port/gdx_hud_placement.h for what each function answers and why; this file is the ONE
// place that implements all of them, so the all-or-nothing/per-element invariants (modes 0-2
// reproduce today's answers exactly; numPlayers>1 always forces every element on top) can't drift
// apart across two definitions.
#include "gdx_hud_placement.h"

// nibottom Phase 2: the raw configured mode. 3DS-only (port/3ds/gdx3ds_bottomhud.c owns the
// ini-backed value: [debug] bottom_race gates whether the DASH draws at all, [hud] placement is
// this mode); everywhere else there is no bottom screen, so the mode always reads ALL_TOP and
// every predicate below always answers "not suppressed" / "every element on top" -- a non-3DS
// build is therefore bit-identical to one with this lever compiled out.
//
// GDX_HUD_PLACEMENT_HOST_TEST (host test only, never defined by a real build target): lets
// port/tests/gdx_hud_placement_host_tests.c force the mode number directly, since the test has
// no gdx3ds_bottomhud.c and no ini to drive it through. Compiled out entirely otherwise -- zero
// cost in every shipped target.
#if defined(GDX_HUD_PLACEMENT_HOST_TEST)
static int sGdxHudPlacementTestModeOverride = -1; // < 0: no override, defer to the #if below

void gdx_hud_placement_test_set_mode(int mode) {
    sGdxHudPlacementTestModeOverride = mode;
}
#endif

int gdx_hud_placement_mode(void) {
#if defined(GDX_HUD_PLACEMENT_HOST_TEST)
    if (sGdxHudPlacementTestModeOverride >= 0) {
        return sGdxHudPlacementTestModeOverride;
    }
#endif
#if defined(GDX_PLATFORM_3DS)
    extern int gdx3ds_bottomhud_placement_mode(void); // port/3ds/gdx3ds_bottomhud.c
    return gdx3ds_bottomhud_placement_mode();
#else
    return GDX_HUD_PLACEMENT_ALL_TOP;
#endif
}

// Shared by every predicate below: the EFFECTIVE mode once the numPlayers>1 force-ALL_TOP rule
// (gdx_hud_placement.h's own top comment) is applied. Not exposed itself -- callers want a
// suppression/mask/element answer, not the mode number, so there is exactly one place (here) that
// has to remember 2-4P always forces ALL_TOP.
static int gdx_hud_placement_effective_mode(int numPlayers) {
    return (numPlayers > 1) ? GDX_HUD_PLACEMENT_ALL_TOP : gdx_hud_placement_mode();
}

// nihudtop Phase 3: per-element top-screen gating, built on the same effective-mode rule above.
// -----------------------------------------------------------------------------------------------
// Everything below answers in terms of a MASK (bit i == GDX_HUD_ELEM_i may draw on top this
// frame) rather than re-deriving a yes/no per element, because the real call sites (hud.c's
// Hud_DrawHud, menus.c) are on the race hot path and want one cheap `(mask >> elem) & 1u` each,
// not a function call each -- gdx_hud_top_element_mask is the one call per frame that produces
// the mask; gdx_hud_element_on_top below is the convenience wrapper host tests and the two
// legacy predicates use.
//
// CUSTOM override: gGdxHudCustomActive is set only by gdx_hud_set_custom_top_mask, which nothing
// in the shipped MODE-chip cycle calls yet (see gdx_hud_placement.h's GDX_HUD_PLACEMENT_CUSTOM
// comment) -- today its only caller is the dev ini lever in port/3ds/gdx3ds_hudtop_custom.c. It
// is deliberately checked BEFORE gdx_hud_placement_mode() so the override works regardless of
// what mode number the chip is parked on, and deliberately excluded for numPlayers>1 so the
// FORCED-ALL-TOP-IN-2-4P rule still holds no matter what a stray dev-ini value says.
static unsigned gGdxHudCustomTopMask = 0;
static int gGdxHudCustomActive = 0;

void gdx_hud_set_custom_top_mask(unsigned mask, int customActive) {
    gGdxHudCustomTopMask = mask;
    gGdxHudCustomActive = customActive;
}

// o3ds60 [ui] mp_hud_placement: an MP fullview race draws ONE local view, full screen, in the 1P
// layout (port/3ds/game/gdx3ds_fullview_game.c), so the 2-4P force-ALL_TOP reason (one HUD per
// viewport quadrant) does not hold for it. With the lever on (Old 3DS default) that view answers as
// the 1P race does; the bottom DASH shows the same race information. 3DS only (the hook lives in
// port/3ds/gdx3ds_bottomhud.c); everywhere else numPlayers is used as given.
#if defined(GDX_HUD_PLACEMENT_HOST_TEST)
static int sGdxHudPlacementTestFullview = 0; // host test: stands in for the 3DS hook below

void gdx_hud_placement_test_set_fullview(int on) {
    sGdxHudPlacementTestFullview = on;
}
#endif

static int gdx_hud_placement_players(int numPlayers) {
#if defined(GDX_HUD_PLACEMENT_HOST_TEST)
    if (numPlayers > 1 && sGdxHudPlacementTestFullview) {
        return 1;
    }
#elif defined(GDX_PLATFORM_3DS)
    extern int gdx3ds_bottomhud_fullview_placement(void); // port/3ds/gdx3ds_bottomhud.c
    if (numPlayers > 1 && gdx3ds_bottomhud_fullview_placement()) {
        return 1;
    }
#endif
    return numPlayers;
}

unsigned gdx_hud_top_element_mask(int numPlayers) {
    int mode;

    numPlayers = gdx_hud_placement_players(numPlayers);

    if (gGdxHudCustomActive && (numPlayers <= 1)) {
        return gGdxHudCustomTopMask;
    }

    mode = gdx_hud_placement_effective_mode(numPlayers);
    if (mode == GDX_HUD_PLACEMENT_ALL_BOTTOM) {
        return 0u; // nothing stays on top, minimap included
    }
    if (mode == GDX_HUD_PLACEMENT_SPLIT) {
        return 1u << GDX_HUD_ELEM_MINIMAP; // only the minimap stays on top
    }
    return ~0u; // ALL_TOP (or an unrecognized mode number): every element stays on top
}

int gdx_hud_element_on_top(int element, int numPlayers) {
    return (int) ((gdx_hud_top_element_mask(numPlayers) >> element) & 1u);
}

// Every element Hud_DrawHud itself draws (hud.c case 1) -- the set menus.c's single "call
// Hud_DrawHud or run Hud_AdvanceLapAnnouncementsOnly instead" branch needs to know about.
// GDX_HUD_ELEM_RANK/SPEED/PORTRAITS/MINIMAP are each gated at their OWN menus.c call site
// instead (Hud_DrawHud never draws them), so they are deliberately not in this set.
#define GDX_HUD_DRAWHUD_ELEM_BITS                                                               \
    ((1u << GDX_HUD_ELEM_TIME) | (1u << GDX_HUD_ELEM_LAP) | (1u << GDX_HUD_ELEM_INTERVAL) |      \
     (1u << GDX_HUD_ELEM_ENERGY) | (1u << GDX_HUD_ELEM_KO_STARS) | (1u << GDX_HUD_ELEM_LIVES) |  \
     (1u << GDX_HUD_ELEM_REVERSE))

int gdx_hud_top_suppressed(int numPlayers) {
    // "Suppressed" = menus.c must skip the Hud_DrawHud call outright and run
    // Hud_AdvanceLapAnnouncementsOnly instead (hud.c's own comment on that function explains why
    // the side effects still have to run). True exactly when NONE of Hud_DrawHud's own elements
    // would draw -- for modes 0-2 that is SPLIT/ALL_BOTTOM (identical to the pre-Phase-3 answer,
    // since those modes never split Hud_DrawHud's elements from one another); for CUSTOM it is
    // "the whole mask bit group is off", so a mask with even one Hud_DrawHud element on still
    // gets Hud_DrawHud called (letting gdx_hud_element_on_top sort the rest out inside it).
    return (gdx_hud_top_element_mask(numPlayers) & GDX_HUD_DRAWHUD_ELEM_BITS) == 0u;
}

int gdx_hud_minimap_suppressed(int numPlayers) {
    return !gdx_hud_element_on_top(GDX_HUD_ELEM_MINIMAP, numPlayers);
}

const char* gdx_hud_placement_name(int mode) {
    switch (mode) {
        case GDX_HUD_PLACEMENT_SPLIT: return "SPLIT";
        case GDX_HUD_PLACEMENT_ALL_BOTTOM: return "ALL_BOTTOM";
        default: return "ALL_TOP";
    }
}
