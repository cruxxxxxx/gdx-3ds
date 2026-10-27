/* port/3ds/gdx3ds_bottomhud.c — nibottom Phase 2: the DASH bottom-screen race HUD.
 * Contract, takeover model and the digit-overlay scope gap: gdx3ds_bottomhud.h.
 *
 * REPAINT-RATE DISCIPLINE (the brief's "dirty-rect repaint only"), per element:
 *   baked backdrop+chrome : once per race (sWasValid's 0->1 edge) — gdx3ds_hudbake.h (baked at boot).
 *   map box               : throttled to [debug] bottom_race_hz (default 10), own timestamp,
 *                           independent of the MAP tab's always-60Hz box.
 *   top-5 colour tags      : only when top5Character[] differs from the last paint.
 *   energy bar fill + MODE dot : every tick — a few hundred pixels, cheaper to redraw than to
 *                           dirty-track.
 *   spare machines        : only when the GP stock count changes (DashPaintLives).
 *   FPS text              : sampled at 2 Hz, drawn as the map box's last layer (map's rate).
 *   live numeric overlay  : every tick while valid (this is the "timer digits per frame"
 *                           element; the others piggyback on the same console-text call for
 *                           simplicity since one extra PaintBegin/PaintRow pair per tick is
 *                           cheap and they must not drift out of sync with the pixel backdrop).
 */
#include "gdx3ds_bottomhud.h"
#include "gdx3ds_hudbake.h"         /* the DASH art: baked at boot from fzerox.o2r */
#include "gdx3ds_bottomhud_idle_assets.h" /* tools/gen_nibottom_idle_assets.py -- the idle view's logo */
#include "gdx3ds_bui_glyph_table.h"  /* bui_glyph_table(): the bottom-menus kit reads these atlases */
#include "gdx3ds_bui_race.h"         /* the multiplayer race dash (bottom-menus M3) */
#include "gdx3ds_bui_kit.h"          /* bui_kit_dirty: the columns the race column painted (o3dsperf) */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <3ds.h>

#include "gdx3ds_config.h"
#include "gdx3ds_cpucut.h"     /* o3ds-cpucuts: dash_race0_d1 */
#include "gdx3ds_filelog.h"
#include "gdx3ds_fs.h"         /* gdx3ds_fs_base_path: the bake cache sits beside the archives */
#include "gdx3ds_frameskip.h"   /* gdx3ds_frameskip_skipped_total: the FPS chip's drawn-frame count */
#include "gdx_racemap.h"        /* GdxRaceHudSnapshot/gdx_racehud_snapshot + the MAP bitmap accessors */
#include "gdx_hud_placement.h" /* GDX_HUD_PLACEMENT_*, GDX_HUD_ELEM_*, gdx_hud_placement_name,
                                * gdx_hud_element_on_top, gdx_hud_set_custom_top_mask (native/
                                * nihudtop's real CUSTOM-mode API -- port/3ds/gdx3ds_hudcustom.h's
                                * local #ifndef stand-ins and its weak gdx3ds_hudcustom_stub.c are
                                * both gone as of the nihudtop merge; this is the one real header) */
#include "gdx3ds_hudcustom_layout.h"  /* pure clamp/snap/hit-test/ini-line logic, host-tested */

/* port/input_bridge.c. Weak: see the matching comment on the KEY_SELECT mode-cycle logic. */
extern int gdx_input_in_gameplay(void) __attribute__((weak));
extern int gdx_input_game_mode(void) __attribute__((weak)); /* port/input_bridge.c: GET_MODE(gGameMode) */

/* ---- geometry: one data table, not scattered #defines -------------------------------- */
#define DASH_FB_W 320
#define DASH_FB_H 240

/* COORDINATOR FIX (custom-HUD groundwork, hardware feedback round): the coordinator wants a
 * FUTURE on-device "custom" HUD mode where each widget can be dragged, resized or hidden. That
 * needs widget geometry to be RUNTIME data, not a compile-time #define -- a #define can never be
 * written by a future drag gesture or a saved layout file. sDashWidgets below is that single
 * source of truth: one row per widget (id, rect, enabled). The DASH_<widget>_X0/Y0/W/H names
 * every paint function already used are kept (so none of this file's per-widget pixel math has
 * to change) but are now THIN ACCESSORS over this table instead of independent literals --
 * relocating a widget is one row edit here, not a hunt through the file. `paint` records which
 * function owns each widget's per-tick draw, for a future generic "for each enabled widget, call
 * its paint fn" dispatch loop; NOT wired to one yet (see the comment on sDashWidgets itself,
 * below the forward declarations, for exactly why that last step is deferred this round, not
 * done partway). `enabled` IS wired up today (every Dash*Paint* function below bails out first
 * thing when its own row is disabled) -- cheap, safe, and it is the one piece of "hide a widget"
 * a future mode would actually need working now rather than only scaffolded. */
typedef struct DashWidgetRect {
    const char* id;
    int x, y, w, h;
    int enabled;
    void (*paint)(uint16_t* fb, const GdxRaceHudSnapshot* snap);
} DashWidgetRect;

typedef enum DashWidgetId {
    DASHW_ENERGY_BAR = 0,
    DASHW_HEADER,
    DASHW_MODE_CHIP,
    DASHW_MENU_BUTTON,
    DASHW_PORTRAIT,
    DASHW_STANDINGS,
    DASHW_KO_STARS,
    DASHW_LAP,
    DASHW_TIMER,
    DASHW_GAP,
    DASHW_SPEED,
    DASHW_MAP_BOX,
    DASHW_COUNT
} DashWidgetId;

/* Forward declarations purely so sDashWidgets' own `paint` column can reference them -- every
 * function here already exists further down this file with this exact signature, OR (map_box,
 * portrait, standings) is wrapped by a same-signature shim right above the table so the column
 * has no holes. gdx3ds_bottomhud_tick still calls the real per-tick sequence directly, in its own
 * carefully ordered/throttled/dirty-checked shape -- see the note on sDashWidgets for why. */
static void DashPaintEnergyFooter(uint16_t* fb, const GdxRaceHudSnapshot* snap);
static void DashPaintHeaderLive(uint16_t* fb, const GdxRaceHudSnapshot* snap);
static void DashPaintModeLabel(uint16_t* fb, const GdxRaceHudSnapshot* snap);
static void DashPaintBackButton(uint16_t* fb, const GdxRaceHudSnapshot* snap);
static void DashPaintOurPosition(uint16_t* fb, const GdxRaceHudSnapshot* snap);
static void DashPaintStandingsLive(uint16_t* fb, const GdxRaceHudSnapshot* snap);
static void DashPaintKOStars(uint16_t* fb, const GdxRaceHudSnapshot* snap);
static void DashPaintRightColumnLap(uint16_t* fb, const GdxRaceHudSnapshot* snap);
static void DashPaintRightColumnTimer(uint16_t* fb, const GdxRaceHudSnapshot* snap);
static void DashPaintRightColumnGap(uint16_t* fb, const GdxRaceHudSnapshot* snap);
static void DashPaintSpeed(uint16_t* fb, const GdxRaceHudSnapshot* snap);
static void DashPaintMapBoxLive(uint16_t* fb, const GdxRaceHudSnapshot* snap);

/* COORDINATOR NOTE (why `paint` is populated but gdx3ds_bottomhud_tick does not loop over it
 * yet): several widgets here are not independent per-tick draws -- the map box is cost-throttled
 * to [debug] bottom_race_hz against its OWN timestamp; the standings portraits/rank tags/colour
 * swatches are dirty-checked against Top5Changed and must repaint together, in a fixed order,
 * relative to the baked backdrop; the MODE dot must be painted AFTER DashPaintModeLabel repaints
 * the chip's own interior; and the once-per-race baked backdrop (DashPaintBaked) is not a
 * per-tick call at all. Collapsing that into one uniform "call every enabled widget's paint fn"
 * loop is a real, measurable rewrite of the tick's control flow, not a rename -- and Gate 5 (the
 * nibottom track's own measured main-thread cost budget) and the DASH's documented REPAINT-RATE
 * DISCIPLINE table at the top of this file both depend on exactly this ordering/throttling being
 * right. Rather than risk that under this round's "same visual result, keep the gates green"
 * constraint, the geometry/enabled table above is wired up FULLY (every paint fn already reads
 * its own rect from here, and bails out on `enabled == 0`); the `paint` column is real data (not
 * a placeholder) for a future generic dispatch pass to consume once it re-derives the
 * ordering/throttle rules as per-widget metadata instead of tick()'s hand-written sequence --
 * that re-derivation, not the table itself, is the work left undone this round. */
static DashWidgetRect sDashWidgets[DASHW_COUNT] = {
    { "energy_bar",  0,   0,   320, 24,  1, DashPaintEnergyFooter },
    { "header",      0,   218, 320, 22,  1, DashPaintHeaderLive },
    { "mode_chip",   168, 221, 62,  16,  1, DashPaintModeLabel },
    { "menu_button", 82,  221, 78,  16,  1, DashPaintBackButton },
    { "portrait",    0,   26,  63,  42,  1, DashPaintOurPosition },
    { "standings",   0,   67,  64,  148, 1, DashPaintStandingsLive },
    /* card-4b (hw: "the km/h is so small at bottom... the KO stars should switch spot and make
     * the km/h bigger"): the speedometer heads the right column in the top screen's own digits
     * (DashPaintSpeed) and the KO stars take the bottom slot.
     * uipolish (hw: "the speedometer numbers slightly clip under the map and the time/best/gap
     * text overdraw on the KO stars"): one right column, x 241..316 (3 px from the screen edge,
     * 3 px from the map's border), in three boxes with the same 3 px gaps: SPEED 26..69, the
     * stats box 73..192 (LAP / TIME / BEST / GAP: the lap, timer and gap rows are its three
     * sections, DashPaintStatsBox) and KO 196..215 (2 px above the header, as the column starts
     * 2 px under the energy bar). The map shrinks 168 -> 160 px for a 76 px column: three 2x
     * speed digits (70 px of ink) and an 8-character clock fit with room on both sides. */
    { "ko_stars",    241, 196, 76,  20,  1, DashPaintKOStars },
    { "lap",         241, 73,  76,  30,  1, DashPaintRightColumnLap },
    { "timer",       241, 103, 76,  62,  1, DashPaintRightColumnTimer },
    { "gap",         241, 165, 76,  28,  1, DashPaintRightColumnGap },
    { "speed",       241, 26,  76,  44,  1, DashPaintSpeed },
    { "map_box",     77,  41,  160, 160, 1, DashPaintMapBoxLive },
};

#define DASH_ENERGY_X0 (sDashWidgets[DASHW_ENERGY_BAR].x)
#define DASH_ENERGY_Y0 (sDashWidgets[DASHW_ENERGY_BAR].y)
#define DASH_ENERGY_W  (sDashWidgets[DASHW_ENERGY_BAR].w)
#define DASH_ENERGY_H  (sDashWidgets[DASHW_ENERGY_BAR].h)

#define DASH_HEADER_X0 (sDashWidgets[DASHW_HEADER].x)
#define DASH_HEADER_Y0 (sDashWidgets[DASHW_HEADER].y)
#define DASH_HEADER_W  (sDashWidgets[DASHW_HEADER].w)
#define DASH_HEADER_H  (sDashWidgets[DASHW_HEADER].h)

#define DASH_MODE_X0 (sDashWidgets[DASHW_MODE_CHIP].x)
#define DASH_MODE_Y0 (sDashWidgets[DASHW_MODE_CHIP].y)
#define DASH_MODE_W  (sDashWidgets[DASHW_MODE_CHIP].w)
#define DASH_MODE_H  (sDashWidgets[DASHW_MODE_CHIP].h)

#define DASH_BACK_X0 (sDashWidgets[DASHW_MENU_BUTTON].x)
#define DASH_BACK_Y0 (sDashWidgets[DASHW_MENU_BUTTON].y)
#define DASH_BACK_W  (sDashWidgets[DASHW_MENU_BUTTON].w)
#define DASH_BACK_H  (sDashWidgets[DASHW_MENU_BUTTON].h)

#define DASH_PORTRAIT_X0 (sDashWidgets[DASHW_PORTRAIT].x)
#define DASH_PORTRAIT_Y0 (sDashWidgets[DASHW_PORTRAIT].y)
#define DASH_PORTRAIT_W  (sDashWidgets[DASHW_PORTRAIT].w)
#define DASH_PORTRAIT_H  (sDashWidgets[DASHW_PORTRAIT].h)

#define DASH_STANDINGS_X0 (sDashWidgets[DASHW_STANDINGS].x)
#define DASH_STANDINGS_Y0 (sDashWidgets[DASHW_STANDINGS].y)
#define DASH_STANDINGS_W  (sDashWidgets[DASHW_STANDINGS].w)
#define DASH_STANDINGS_H  (sDashWidgets[DASHW_STANDINGS].h)
#define DASH_STANDINGS_ROW_H (DASH_STANDINGS_H / GDX_RACEHUD_TOP5_MAX) /* 29 */

#define DASH_KO_X0 (sDashWidgets[DASHW_KO_STARS].x)
#define DASH_KO_Y0 (sDashWidgets[DASHW_KO_STARS].y)
#define DASH_KO_W  (sDashWidgets[DASHW_KO_STARS].w)
#define DASH_KO_H  (sDashWidgets[DASHW_KO_STARS].h)

#define DASH_LAP_X0 (sDashWidgets[DASHW_LAP].x)
#define DASH_LAP_Y0 (sDashWidgets[DASHW_LAP].y)

#define DASH_TIMER_X0 (sDashWidgets[DASHW_TIMER].x)
#define DASH_TIMER_Y0 (sDashWidgets[DASHW_TIMER].y)

#define DASH_GAP_X0 (sDashWidgets[DASHW_GAP].x)
#define DASH_GAP_Y0 (sDashWidgets[DASHW_GAP].y)

/* uipolish: the stats box spans the lap, timer and gap rows (one frame, three sections). */
#define DASH_STATS_X0 DASH_LAP_X0
#define DASH_STATS_Y0 DASH_LAP_Y0
#define DASH_STATS_W (sDashWidgets[DASHW_LAP].w)
#define DASH_STATS_H (DASH_GAP_Y0 + sDashWidgets[DASHW_GAP].h - DASH_LAP_Y0)

/* The speedometer (hw bug n60-3d7, item 4): fills the gap below GAP now that BOOST is gone (item
 * 6, hardware feedback round removed the BOOST box outright -- see sDashWidgets' own "ko_stars"
 * row for where its space went instead). No baked backdrop; chrome and value are both repainted
 * live, same style as the MODE/MENU/KO-star chips. Position itself is NOT duplicated here --
 * round 3 item 3 already put the big ordinal numeral on the portrait widget. */
#define DASH_SPEED_X0 (sDashWidgets[DASHW_SPEED].x)
#define DASH_SPEED_Y0 (sDashWidgets[DASHW_SPEED].y)
#define DASH_SPEED_W  (sDashWidgets[DASHW_SPEED].w)
#define DASH_SPEED_H  (sDashWidgets[DASHW_SPEED].h)

/* COORDINATOR FIX (hardware feedback round, item 4): "the minimap could afford to be a bit
 * smaller, uses a lot of prime real estate." Shrunk from the debug MAP tab's own 192x192 (scale
 * 3 on the 64x64 source bitmap) to 168x168 and re-centred in the middle column's free space
 * (x64..258, y24..216) instead of pinned to its old top-left corner, trading the freed ~14-24px
 * margin on every side for breathing room around it rather than growing any one neighbour (no
 * neighbour widget needed more width/height to read correctly; the ask was "give everything else
 * some room", not "make X bigger"). DashPaintMapBoxLive/DashPaintMapMarker below are written as a
 * destination-pixel-driven nearest-neighbour scale (not the old fixed-integer-replication one)
 * so this box's size is just DASH_MAP_PX (square) -- any future custom-HUD resize is a table edit,
 * not a rewrite of the blit loop. */
#define DASH_MAP_X0 (sDashWidgets[DASHW_MAP_BOX].x)
#define DASH_MAP_Y0 (sDashWidgets[DASHW_MAP_BOX].y)
#define DASH_MAP_PX (sDashWidgets[DASHW_MAP_BOX].w) /* square: w == h */
#define DASH_MAP_SRC_DIM 64

/* ---- the fixed per-tick paint ORDER for modes ALL_TOP/SPLIT/ALL_BOTTOM ----------------
 *
 * COORDINATOR FIX (custom-HUD groundwork, follow-up round): "convert tick() to loop the widget
 * table in a defined paint order, re-encoding the ordering/throttle couplings explicitly (keep
 * output identical in modes 1-3)." sDashWidgets' own `paint` column (above) already holds the
 * right function pointer per widget; the only thing tick() used to do BY HAND that a bare loop
 * over that table in DASHW_* storage order would get wrong is (a) the paint CALL order, which is
 * NOT the same as the table's own storage order (storage is grouped by widget "kind" for
 * readability; paint order follows the backdrop's own layer order -- header, then standings,
 * then the map box, etc.), and (b) the MODE dot, which has no row of its own in sDashWidgets at
 * all and must paint immediately after DashPaintModeLabel repaints the chip's interior (see that
 * function's own call-site comment, further down). kDashPaintOrder is that order, written out
 * explicitly instead of left implicit in a hand-written call sequence; HEADER and MENU_BUTTON are
 * deliberately absent (HEADER's own live half is this list's first entry via DASHW_HEADER --
 * wait, HEADER *is* in the list, see below; MENU_BUTTON is not: it is only ever painted once, by
 * DashPaintBaked, at backdrop-bake time, never per-tick -- matching tick()'s pre-refactor
 * behaviour exactly, where DashPaintBackButton was never one of the hand-written per-tick calls
 * either). The throttle/dirty-check couplings themselves (map_box's own [debug] bottom_race_hz
 * timestamp, standings' Top5Changed dirty check) already live INSIDE their respective paint
 * functions (DashPaintMapBoxLive/DashPaintStandingsLive) from the widget-geometry-table round, so
 * looping this list unconditionally every tick is exactly as cheap as the old hand-written
 * sequence was -- each function still decides for itself whether it actually has anything new to
 * draw this tick. */
static const DashWidgetId kDashPaintOrder[] = {
    DASHW_HEADER,     /* course name + "<n> RACERS", dirty-checked internally */
    DASHW_STANDINGS,  /* portraits + rank badges + colour tags, dirty-checked internally */
    DASHW_MAP_BOX,    /* throttled internally to [debug] bottom_race_hz */
    DASHW_ENERGY_BAR, /* every tick: a few hundred pixels, cheaper to redraw than to dirty-track */
    DASHW_PORTRAIT,   /* the big OUR-position ordinal numeral */
    DASHW_MODE_CHIP,  /* repaints the chip's own interior -- the MODE DOT (no table row) must
                        * follow it; see the special-case right after this loop runs, below */
    DASHW_KO_STARS,
    DASHW_LAP,
    DASHW_TIMER,
    DASHW_GAP,
    DASHW_SPEED,
};
#define DASH_PAINT_ORDER_COUNT ((int) (sizeof(kDashPaintOrder) / sizeof(kDashPaintOrder[0])))

/* ---- CUSTOM HUD mode: a 4th placement mode, draggable/resizable/closeable widgets ------
 *
 * GDX_HUD_PLACEMENT_CUSTOM (port/gdx_hud_placement.h) sits one past the 3 fixed modes. Unlike
 * ALL_TOP/SPLIT/ALL_BOTTOM, CUSTOM does not reuse the baked pixel art at all (a baked asset can't
 * stretch to an arbitrary user-chosen size) -- every editable widget draws as a simple flat
 * panel + border + short label + live value instead (DashPaintCustomWidget, further down), at
 * whatever rect sCustomWidgets currently holds for it. sCustomWidgets starts as a plain copy of
 * sDashWidgets' own default rects (DashCustomDefaults) and is then possibly overridden, per
 * widget, by a saved sdmc:/3ds/gdiffuser/hud.ini [custom] section (DashHudIniLoad); RESET and
 * DONE (DashCustomReset / the EDIT<->DONE toggle) are the only two things that ever write that
 * section back out -- never every frame, per the brief.
 *
 * HEADER/MODE_CHIP/MENU_BUTTON are NOT part of the editable set: they are DASH-only chrome (a
 * course-name header, the mode-cycle chip, the "back to debug tabs" button), not race-HUD
 * elements with a top-screen counterpart, so dragging/closing them would not mean anything.
 *
 * SHELVED (nibottom-progress.md §12): the user's own real-hardware pass on CUSTOM found it not
 * worth further investment right now ("the default HUDS are good enough") and reported concrete
 * bugs along the way -- resizing a widget does not clear the framebuffer (stale pixels from the
 * old size/position linger until something else repaints over them) and some widgets simply
 * never display at all. None of that code is deleted: it is gated, off by default, behind the
 * new [debug] hud_custom lever (HudCustomLeverEnabled, right below) so modes 0-2 (ALL_TOP/SPLIT/
 * ALL_BOTTOM -- already proven byte-identical across this whole track) are unaffected and CUSTOM
 * simply becomes unreachable through the normal MODE chip/SELECT cycle or the DBG tab until
 * someone opts back in to keep working on it. */
static DashWidgetRect sCustomWidgets[DASHW_COUNT];

/* SHELF: [debug] hud_custom (default 0). 0 = CUSTOM ("MODE Z") is unreachable: the MODE chip/
 * SELECT cycle only visits the 3 fixed modes, the DBG tab's "HUD EDIT" row is hidden, and a
 * saved hud.ini with a stale [hud] placement=3 from before this lever existed falls back to
 * ALL_BOTTOM at load (gdx3ds_bottomhud_init) instead of resurrecting CUSTOM silently. 1 restores
 * the exact pre-shelving behaviour (4-mode cycle, visible HUD EDIT row) for whoever picks this
 * back up -- no code path below is removed, only gated. */
static int HudCustomLeverEnabled(void) {
    return gdx3ds_config_get_bool("debug", "hud_custom", 0) ? 1 : 0;
}

/* Public wrapper (gdx3ds_bottomhud.h): gdx3ds_menu.c's DBG tab reads this to decide whether to
 * paint/accept taps on the "HUD EDIT" row at all, instead of duplicating the ini read. */
int gdx3ds_bottomhud_custom_enabled(void) {
    return HudCustomLeverEnabled();
}

static int DashWidgetEditable(int id) {
    return id != DASHW_HEADER && id != DASHW_MODE_CHIP && id != DASHW_MENU_BUTTON;
}

/* "Picture widgets like portraits/star icon may resize in fixed steps or not at all." PORTRAIT
 * (the big single OUR-position numeral) is a fixed-size element -- drag-only, never resized;
 * MAP_BOX and KO_STARS (icon-based: the minimap bitmap, the star glyph) resize in coarse 16px
 * steps so their contents stay legible at a handful of sizes rather than an arbitrary one;
 * everything else (plain text/number boxes) resizes freely in 8px steps, same grid the drag
 * snap below uses. */
static int DashWidgetResizable(int id) {
    return id != DASHW_PORTRAIT;
}

static int DashWidgetResizeStep(int id) {
    if (id == DASHW_MAP_BOX || id == DASHW_KO_STARS) {
        return 16;
    }
    return 8;
}

static int DashWidgetMinW(int id) {
    return (id == DASHW_MAP_BOX) ? 48 : 32;
}

static int DashWidgetMinH(int id) {
    return (id == DASHW_MAP_BOX) ? 48 : 16;
}

/* Which GDX_HUD_ELEM_* bit (port/gdx_hud_placement.h) each editable widget stands in for on the TOP
 * screen, in DashWidgetId order; -1 for a widget with no top-screen counterpart at all (the
 * brief's own examples: "GAP/BEST/standings list, just hide" -- BEST lives inside the TIMER
 * widget here, not as its own row, so TIMER maps to GDX_HUD_ELEM_TIME on the strength of its
 * current-lap-time half; its BEST sub-field has no top equivalent either, but the whole widget
 * is one close/open unit, same as every other row in this table). */
static const int kDashWidgetTopElem[DASHW_COUNT] = {
    GDX_HUD_ELEM_ENERGY,  /* DASHW_ENERGY_BAR */
    -1,                    /* DASHW_HEADER      -- fixed chrome, not editable */
    -1,                    /* DASHW_MODE_CHIP   -- fixed chrome, not editable */
    -1,                    /* DASHW_MENU_BUTTON -- fixed chrome, not editable */
    GDX_HUD_ELEM_RANK,     /* DASHW_PORTRAIT    -- the big position ordinal */
    -1,                    /* DASHW_STANDINGS   -- no top-screen counterpart: hide only */
    GDX_HUD_ELEM_KO_STARS, /* DASHW_KO_STARS */
    GDX_HUD_ELEM_LAP,      /* DASHW_LAP */
    GDX_HUD_ELEM_TIME,     /* DASHW_TIMER */
    -1,                    /* DASHW_GAP         -- no top-screen counterpart: hide only */
    GDX_HUD_ELEM_SPEED,    /* DASHW_SPEED */
    GDX_HUD_ELEM_MINIMAP,  /* DASHW_MAP_BOX */
};

/* ---- state -------------------------------------------------------------------------- */
static int sEnabled = 0;       /* [debug] bottom_race */
static int sPlacementMode = GDX_HUD_PLACEMENT_ALL_TOP; /* [hud] placement, RAM cache of the ini */
static int sHz = 10;           /* [debug] bottom_race_hz — map-box throttle */
static int sWasValid = 0;      /* backdrop once-per-race edge */
static int sLastTop5Count = -1;
static int sLastTop5[GDX_RACEHUD_TOP5_MAX];
static u64 sLastMapTick = 0;
/* COORDINATOR FIX (hardware feedback round, item 5): "<n> RACERS" was frozen baked-template text;
 * make it live. Dirty-checked (like top5), not every tick -- it only ever changes when a racer
 * retires/finishes, same cadence gCpuRacersRetired itself changes at. sLastHeaderTrackName is the
 * matching cache for the course-name half of the header (replaces the old sWasValid-edge-only
 * DashPaintCourseName call with a self-contained dirty check, so DashPaintHeaderLive can be one
 * widget-table paint fn instead of two separately-scheduled pieces). */
static int sLastHeaderRacersRemaining = -1;
static char sLastHeaderTrackName[24] = "";
static uint16_t sDashHeaderFill = 0x2043u; /* header interior, sampled from the art at bake time */
/* Spare machines under the ENERGY label (DashPaintLives): the count last painted, so it repaints
 * only when the stock changes. DASH_LIVES_UNPAINTED forces the first paint after every backdrop
 * bake (the bake paints the checkerboard over that strip). */
#define DASH_LIVES_UNPAINTED (-2)
static int sLastSpareMachines = DASH_LIVES_UNPAINTED;
/* FPS text (DashPaintMapFpsText): [debug] bottom_fps, plus the sampling window and the latest
 * sample. Same "drawn" definition as gdx3ds_fps_hud.c: host iterations minus frameskip skips. */
static int sFpsEnabled = 1;
static u64 sFpsWindowStartTick = 0;
static unsigned long sFpsWindowTicks = 0;
static unsigned long sFpsWindowStartSkipped = 0;
static int sFpsShown = -1;        /* latest whole-fps sample; -1 = "--" (no sample yet) */
static char sPendingShot[40];
static int sShotArmed = 0;
static int sPlacementSaveDirty = 0; /* [hud] placement changed mid-race; ini write deferred */
static int sPrevChipTouch = 0;      /* edge-detect for the MODE chip's own touch-down */
static int sPrevBackTouch = 0;      /* edge-detect for the DEBUG button's touch-down (nee "MENU") */
static int sDismissed = 0;          /* 1 once the DEBUG button hands the screen back to gdx3ds_menu.c */
/* COORDINATOR FIX (hw bug n60-3d7, item 5): "let the MENU/RACE toggle work from a key (SELECT
 * long-press or similar)" -- the debug menu's RACE tab (the only prior way back once dismissed)
 * could overflow off its 40-column tab bar; a key-based toggle is the fallback that never
 * depends on tab-bar geometry. Ticks (not wall time) so it's frame-rate independent like every
 * other DASH timer here; ~1.5s at 60Hz. Counts while KEY_SELECT is held, independent of the
 * short-tap mode-cycle below (selectDown fires once on press regardless -- a long hold therefore
 * both cycles the MODE chip once AND, after the threshold, dismisses; a minor, harmless overlap
 * rather than two conflicting edge-detectors). */
#define DASH_SELECT_LONGPRESS_TICKS 90
static int sSelectHoldTicks = 0;

/* IDLE VIEW state -- see gdx3ds_bottomhud.h's own "IDLE VIEW" doc comment for the full picture.
 * Deliberately separate from the race DASH's own sDismissed/sSelectHoldTicks/sPrevBackTouch/
 * sWasValid (not reused): the two takeovers are mutually exclusive in time (gdx3ds_menu.c never
 * runs both in the same tick) but conceptually distinct, and keeping their latches apart means
 * neither one's reset logic has to reason about the other's state. */
static int sIdleEnabled = 1;     /* [debug] bottom_idle, read once at init */
static int sIdleDismissed = 0;   /* 1 once the idle view's own DEBUG button hands off to the console */
static int sIdleBaked = 0;       /* idle backdrop painted this "session" (reset on every fresh entry) */
static int sIdlePrevBackTouch = 0;
static int sIdleSelectHoldTicks = 0;

/* Bottom-screen product menus ([ui] bottom_menus, gdx3ds_bui.c): while on, the developer console
 * is reached with L+R+SELECT held (gdx3ds_menu.c runs that combo for every owner of the screen),
 * so neither takeover runs its own SELECT long-press, and the DEBUG button is hidden unless
 * [debug] dev_console=1. Set once at init by gdx3ds_bottomhud_set_bui. */
static int sBuiOn = 0;
static int sDebugHidden = 0;
/* The multiplayer race dash (gdx3ds_bui_race.h): 1 on a DASH tick of a multiplayer race with the
 * product menus on. Every hook below is behind it, so a single-player race runs exactly the code
 * it ran before. sLiveFb: the last live buffer the DASH presented to (the stall banner paints
 * there while the main loop is blocked). */
static int sMpDash = 0;
static uint16_t* sLiveFb = NULL;

/* CUSTOM HUD mode state. sCustomBackdropPainted mirrors sWasValid's own "once per race" latch,
 * but separately: switching between a baked mode (1-3) and CUSTOM mid-race must repaint the
 * right kind of backdrop for whichever one is now active, not assume the other one already ran.
 * sEditMode is the drag/resize/close/tray UI; sEditSelected is -1 (nothing) or a DashWidgetId.
 *
 * The brief offers two alternative gestures for entering edit mode ("long-press the MODE chip
 * while in CUSTOM, or an EDIT button"); this build uses the EDIT button alone (DashPaintCustomChrome/
 * DashCustomToggleEdit, below), not a chip long-press, specifically so the MODE chip's existing
 * touch-DOWN-fires-immediately short-tap-cycle behaviour (gdx3ds_bottomhud_tick's own MODE-chip
 * block, unchanged from before this round) never has to change for ANY mode, CUSTOM included --
 * a long-press gesture sharing that same rect would need the short tap to fire on RELEASE
 * instead, which is exactly the kind of touch-timing change step 1's "keep output identical in
 * modes 1-3" was written to rule out. */
static int sCustomBackdropPainted = 0;
static int sEditMode = 0;
static int sEditSelected = -1;
static int sEditDragging = 0;
static int sEditResizing = 0;
static int sEditDragOffX = 0;
static int sEditDragOffY = 0;

/* The CUSTOM-mode fixed chrome buttons: RESET and the EDIT<->DONE toggle, both in the header's
 * only remaining free gap (x0..81, left of the SETTINGS button at DASH_BACK_X0=82 -- see that
 * macro's own widget-table row). Not part of sDashWidgets/sCustomWidgets: these are meta-UI for
 * the custom-layout editor itself, not race-HUD widgets a layout could ever hide or move. */
#define DASH_RESET_X0 2
#define DASH_RESET_Y0 (DASH_HEADER_Y0 + 3)
#define DASH_RESET_W 38
#define DASH_RESET_H 16
#define DASH_EDIT_X0 42
#define DASH_EDIT_Y0 (DASH_HEADER_Y0 + 3)
#define DASH_EDIT_W 38
#define DASH_EDIT_H 16

/* The hidden-widget tray: one 32x14 chip per currently-disabled editable widget, packed
 * left-to-right starting at x=2, drawn (and only drawn) along this fixed strip while sEditMode
 * is on. Tapping a chip restores that widget (DashCustomRestoreHidden, below) immediately -- no
 * drag needed to bring something back from the tray, unlike everything already on the canvas. */
#define DASH_TRAY_Y0 24
#define DASH_TRAY_CHIP_W 32
#define DASH_TRAY_CHIP_H 14
#define DASH_TRAY_CHIP_GAP 2

/* Self-timing, same convention as gdx3ds_racemap.c's [bmap] receipt (see its own comment on
   why the window closes before the SHOT write): Gate 5 (nibottom Phase 2) asks for a
   main-thread cost delta in ms, and this is the honest number to quote it from — run with
   [debug] shots=0 so a SHOT's ~50ms SD write never gets folded into max_us. */
static unsigned long sTicks = 0;
static u64 sTickTicksTotal = 0;
static u64 sTickTicksMax = 0;

/* o3dsperf: the race DASH's cost levers (hardware 4bfb302: [dash] avg_us=3900-4000 on an Old 3DS
 * and ~1800 on a New 3DS, on EVERY host iteration, rendered or skipped).
 *   [ui] dash_every N (default 4 on an Old 3DS, 1 on a New 3DS): the widgets repaint (and the
 *        live buffer is updated) at most every N host iterations; a due repaint waits up to N
 *        more iterations for a frameskip skip iteration, so it lands where the frame has room.
 *        Touch input and the fps sample still run every iteration. 1 = every iteration (as before).
 *   [ui] dash_memo 1: a widget whose shown value is unchanged since its last paint is not
 *        repainted (its pixels are already in the shadow). 0 = repaint every widget (as before).
 *   [debug] dash_prof 1: [dashprof] per-widget cost every 256 DASH ticks. */
static int sDashEvery = 1;
/* o3ds60 [ui] dash_spread (default 1 on an Old 3DS, 0 on a New 3DS): while the race draws every
 * tick (no frameskip skip iteration to land on), a due repaint no longer paints every widget on one
 * iteration every dash_every: iteration i paints the widgets of phase i % dash_every
 * (kDashPaintOrder's index modulo dash_every), so the cost is spread evenly instead of a spike every
 * Nth frame (a 1-in-4 spike sits above the frameskip's p90 and blocks the step up to 60 fps). Each
 * widget still repaints at 60 / dash_every Hz. 1 also spreads from the first tick steady
 * frameskip runs at D = 1; 2 = only after 8 ticks without a skip (the 5c24be0 rule); 0 = the
 * d9deb37 rule. */
static int sDashSpread = 0;
/* o3ds60 [ui] dash_budget (default 1 on an Old 3DS, 0 on a New 3DS). hw 52acd44 (Old 3DS, MP race
 * at steady D = 1): [steadyd1] post-present 4.1 ms avg / 7.6 max, dash tick 1.1 ms avg / 4.5-5.6
 * max: the dash alone overran the next slot. With the lever on:
 *   - while steady frameskip runs at D = 1, a tick stops painting widgets once it has used
 *     [ui] dash_budget_us (default 1000); the rest carry to the next tick, first in line;
 *   - the map box's scaled bitmap (168 x 168 px, one palette lookup per pixel) is cached and
 *     blitted a column at a time while the minimap bitmap and its palette are unchanged;
 *   - the present copies each run of painted columns, not the whole span between the first and
 *     the last painted column (the map and the timer on one tick copied ~180 columns);
 *   - the self-heal full present (every 120 presents) becomes a 40-column slice every 15.
 * Same pixels: every widget paints what it painted before, at most a few ticks later. 0 = the
 * 52acd44 rules. */
static int sDashBudget = 0;
static int sDashBudgetUs = 1000;   /* [ui] dash_budget_us */
/* o3ds60 [ui] dash_every_d1 (default 6 on an Old 3DS, 0 = dash_every): the spread's widget
 * period while steady runs at D = 1 (6: every widget 10 times a second). */
static int sDashEveryD1 = 0;
/* o3ds-cpucuts [ui] dash_race0_d1 (0 off, 1 on, 2 A/B on [cpucut] bit 1; DEFAULT 0): a race whose
 * frameskip is off (the 1P race0 rule: every tick renders) counts as steady D = 1 for the D = 1
 * levers above -- dash_every_d1's period, dash_spread from the first tick, dash_budget's per-tick
 * budget, dash_map_inc's D = 1 rule. hw 06c5264 (Old 3DS, 1P race0): those levers never engaged
 * there, the dash painting at dash_every with no budget. Not default: the bottom screen's widgets
 * repaint on other ticks (same pixels, other moments). */
static int sDashRace0D1 = 0;
static unsigned sDashPhaseN = 1;   /* the spread period this tick (dash_every or dash_every_d1) */
static unsigned sDashCarry = 0;    /* dash_budget: paint-order bits (+ DASH_CARRY_BUI) left for the next tick */
#define DASH_CARRY_BUI (1u << 30)
static unsigned sHealSlice = 0;
/* o3ds60 [ui] dash_map_inc (default 1 on an Old 3DS, 0 on a New 3DS; needs dash_budget). hw
 * a3b4531 ([dashworst], Old 3DS guest, MP race): every map tick cost map_us 1.6-2.0 ms (the
 * cached 168 x 168 box blit, 56 KB) plus pres_us 1.5-2.3 ms (106 KB: the box's 168 whole columns),
 * and the D = 1 misses with dash_us 2.9-3.1 ms were map ticks (one widget: the budget cannot split
 * it). With the lever, while the cached box is on screen unchanged a map tick only puts the cache
 * back under the previous tick's dots / NET chip / FPS text and paints them anew, and the present
 * copies only those pixels' rows of their columns. Any whole-frame present (a bake, an overlay, a
 * new live buffer) without a whole map paint in the same tick paints the whole box again on the
 * next map tick; so does a map tick off D = 1 after DASH_MAP_INC_HEAL map ticks. Same pixels. */
static int sDashMapInc = 0;
static int sMapIncValid = 0;       /* the shadow's box = the cache + the overlay recorded in sOvY0/1 */
static int sMapFullThisTick = 0;   /* a whole map paint (cache + overlay) ran this tick */
static unsigned sMapIncSince = 0;  /* incremental map ticks since the last whole one */
#define DASH_MAP_INC_HEAL 50u
static unsigned long sMapIncPaints = 0;  /* receipts, per 256 dash ticks */
static unsigned long sMapFullPaints = 0;
static unsigned long sMapCacheBuilds = 0;
static int16_t sOvY0[DASH_FB_W];   /* per column: rows the box overlay covered at the last map paint */
static int16_t sOvY1[DASH_FB_W];
static int16_t sTrkY0[DASH_FB_W];  /* per column: rows the overlay paint wrote this map tick */
static int16_t sTrkY1[DASH_FB_W];
static int16_t sRowY0[DASH_FB_W];  /* per column: rows to present this tick (columns not presented whole) */
static int16_t sRowY1[DASH_FB_W];
static int sRowAny = 0;
/* [dashworst]: the costliest tick of each 256 (what it painted, carried, presented). */
typedef struct DashTickCost {
    u64 total;
    u64 map;
    u64 bui;
    u64 pres;
    unsigned long kb;
    unsigned widgets;
    unsigned carried;
    int full;
    int d1;
    int mapInc; /* o3ds60 dash_map_inc: 1 = the map tick restored and repainted only the overlay */
} DashTickCost;
static DashTickCost sTickCost;
static DashTickCost sWorstTick;
static unsigned sDashPhaseNext = 0;
static u64 sDashLastTickTicks = 0; /* o3ds60: the latest race tick's self-timed cost ([steadyd1]) */
static int sMpHudPlacement = 0; /* o3ds60 [ui] mp_hud_placement, latched at init */
static int sDashMemo = 1;
static int sDashProf = 0;
static unsigned sDashSince = 0;          /* iterations since the last repaint */
static unsigned sDashSinceSkip = 255;    /* iterations since the last frameskip skip iteration */
static unsigned long sDashPrevSkipped = 0;
static unsigned long sDashPaints = 0;    /* repaints in the receipt window */
static unsigned long sDashPresentKb = 0; /* KiB copied to the live buffer in the window */
static uint16_t* sDashLastLive = NULL;
static u64 sProfWidget[DASHW_COUNT];
static u64 sProfBui = 0;
static u64 sProfPresent = 0;
#define DASH_MEMO_KEY 28
static char sMemoKey[DASHW_COUNT][DASH_MEMO_KEY];
static unsigned char sMemoValid[DASHW_COUNT];
static int sMemoMode = -1;
#define DASH_FULL_PRESENT_EVERY 120u /* a whole-frame present this often, as a self-heal */
static unsigned sSinceFullPresent = 0;
/* The shadow's painted columns this tick (DashMarkCols, below). */
static int sDirtyX0 = DASH_FB_W;
static int sDirtyX1 = -1;
static int sDirtyFull = 1;
static uint32_t sDirtyColBits[(DASH_FB_W + 31) / 32]; /* o3ds60 dash_budget: the painted columns themselves */

static void DashLog(const char* fmt, ...) {
    char buf[320];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
        if (n > (int) sizeof(buf) - 1) {
            n = (int) sizeof(buf) - 1;
        }
        svcOutputDebugString(buf, (size_t) n);
        gdx3ds_filelog_write(buf, (size_t) n);
    }
}

/* ---- boot bake: the DASH art from the user's own fzerox.o2r (gdx3ds_hudbake.h) ----------
 *
 * MEMORY (heapfix): every byte of it -- the 344 KB arena the painters read, the bake's transient
 * images, the prefetched records, the cache file in and out -- comes from the LINEAR heap
 * (gdx3ds_hudbake_set_allocator below). The Old 3DS's regular heap is the one the game runs out
 * of (card-5: 4-6 KB allocation failures mid-race, then a 73 KB one in the exit chain); the
 * linear heap is pinned at 16 MB and never drops below ~5 MB free. Moving the arena out of .bss
 * hands its 344 KB back to the regular heap (libctru sizes it as what the image leaves), and
 * nothing the bake does at boot touches, or fragments, the regular free list.
 *
 * SPEED (heapfix): card-5 measured 820 ms of archive time for 138 small records on an Old 3DS
 * (~6 ms each: several SD requests per record at ~2 ms each) and 768 ms to write a 100 KB cache
 * (newlib splits an unbuffered fwrite into 1 KB FS writes, each one extending the file). The
 * records are now prefetched in ARCHIVE order (the zipshim's read-ahead window then serves runs
 * of neighbours from one SD read) and the cache goes out in one write() call. */

extern int GDiffuser_LoadArchiveFileBytes(const char* key, void* out, size_t outSize, size_t* copiedSize);
extern int gdx3ds_zipshim_entry_fingerprint(const char* name, unsigned int* fingerprint);
extern int gdx3ds_zipshim_entry_locate(const char* name, unsigned long long* offset, unsigned long long* size);
extern void gdx3ds_zipshim_io_stats(unsigned long* reads, unsigned long long* bytes);

static unsigned long TicksToMicros(u64 ticks) {
    return (unsigned long) (ticks * 1000000ull / SYSCLOCK_ARM11);
}

static void* DashBakeAlloc(size_t bytes) {
    return linearAlloc(bytes);
}

static void DashBakeFree(void* p) {
    linearFree(p);
}

static void DashBakeLog(const char* line, void* user) {
    (void) user;
    DashLog("%s", line);
}

static int DashBakeFingerprint(const char* key, unsigned int* fingerprint, void* user) {
    (void) user;
    return gdx3ds_zipshim_entry_fingerprint(key, fingerprint);
}

/* The prefetch: every record the bake reads, loaded once in archive order into one linear block,
 * then handed to the recipes from memory. A record it could not prefetch (no slot, a read miss)
 * is read through the archive when the bake asks for it, exactly as before. */
#define DASH_PREFETCH_MAX 160

typedef struct DashPrefetchSlot {
    char key[GDX3DS_HUDBAKE_KEY_LEN];
    unsigned long long offset; /* in its archive: the sort key */
    size_t cap;                /* bytes reserved (the entry's size) */
    size_t got;                /* bytes loaded; 0 = not prefetched */
    unsigned char* data;
} DashPrefetchSlot;

typedef struct DashPrefetch {
    DashPrefetchSlot* slots;
    int count;
    int next; /* the bake reads in list order: where to look first */
    unsigned char* store;
    u64 readTicks;
    unsigned long missesAfter; /* reads the prefetch did not cover */
} DashPrefetch;

static int DashPrefetchCompare(const void* a, const void* b) {
    const DashPrefetchSlot* x = *(const DashPrefetchSlot* const*) a;
    const DashPrefetchSlot* y = *(const DashPrefetchSlot* const*) b;
    if (x->offset != y->offset) {
        return x->offset < y->offset ? -1 : 1;
    }
    return 0;
}

static void DashPrefetchFree(DashPrefetch* pf) {
    if (pf->store != NULL) {
        linearFree(pf->store);
    }
    if (pf->slots != NULL) {
        linearFree(pf->slots);
    }
    memset(pf, 0, sizeof(*pf));
}

/* Returns the bytes prefetched (0: nothing, the bake reads through the archive). */
static size_t DashPrefetchLoad(DashPrefetch* pf) {
    char (*keys)[GDX3DS_HUDBAKE_KEY_LEN] = NULL;
    DashPrefetchSlot** order = NULL;
    size_t total = 0;
    size_t used = 0;
    int n = gdx3ds_hudbake_list_keys(NULL, 0);
    int i;
    const u64 t0 = svcGetSystemTick();
    memset(pf, 0, sizeof(*pf));
    if (n <= 0 || n > DASH_PREFETCH_MAX) {
        return 0;
    }
    pf->slots = (DashPrefetchSlot*) linearAlloc(sizeof(DashPrefetchSlot) * (size_t) n);
    keys = (char (*)[GDX3DS_HUDBAKE_KEY_LEN]) linearAlloc(sizeof(*keys) * (size_t) n);
    order = (DashPrefetchSlot**) linearAlloc(sizeof(*order) * (size_t) n);
    if (pf->slots == NULL || keys == NULL || order == NULL) {
        if (keys != NULL) {
            linearFree(keys);
        }
        if (order != NULL) {
            linearFree(order);
        }
        DashPrefetchFree(pf);
        return 0;
    }
    gdx3ds_hudbake_list_keys(keys, n);
    pf->count = n;
    for (i = 0; i < n; i++) {
        DashPrefetchSlot* s = &pf->slots[i];
        unsigned long long size = 0;
        memset(s, 0, sizeof(*s));
        memcpy(s->key, keys[i], sizeof(s->key));
        if (gdx3ds_zipshim_entry_locate(s->key, &s->offset, &size) > 0 && size > 0 && size < (1u << 20)) {
            s->cap = (size_t) ((size + 3u) & ~3ull);
            total += s->cap;
        }
        order[i] = s;
    }
    linearFree(keys);
    pf->store = total != 0 ? (unsigned char*) linearAlloc(total) : NULL;
    if (pf->store == NULL) {
        linearFree(order);
        DashPrefetchFree(pf);
        return 0;
    }
    qsort(order, (size_t) n, sizeof(*order), DashPrefetchCompare);
    for (i = 0; i < n; i++) {
        DashPrefetchSlot* s = order[i];
        size_t copied = 0;
        if (s->cap == 0) {
            continue;
        }
        s->data = pf->store + used;
        used += s->cap;
        if (GDiffuser_LoadArchiveFileBytes(s->key, s->data, s->cap, &copied) && copied > 0) {
            s->got = copied;
        }
    }
    linearFree(order);
    pf->readTicks = svcGetSystemTick() - t0;
    return used;
}

static DashPrefetchSlot* DashPrefetchFind(DashPrefetch* pf, const char* key) {
    int k;
    for (k = 0; k < pf->count; k++) {
        DashPrefetchSlot* s = &pf->slots[(pf->next + k) % pf->count];
        if (strcmp(s->key, key) == 0) {
            pf->next = (int) (s - pf->slots + 1) % pf->count;
            return s;
        }
    }
    return NULL;
}

static size_t DashBakeRead(const char* key, void* buf, size_t cap, void* user) {
    DashPrefetch* pf = (DashPrefetch*) user;
    DashPrefetchSlot* s = (pf != NULL && pf->count > 0) ? DashPrefetchFind(pf, key) : NULL;
    size_t copied = 0;
    u64 t0;
    if (s != NULL && s->got > 0) {
        copied = s->got < cap ? s->got : cap;
        memcpy(buf, s->data, copied);
        return copied;
    }
    t0 = svcGetSystemTick();
    if (pf != NULL) {
        pf->missesAfter++;
    }
    if (!GDiffuser_LoadArchiveFileBytes(key, buf, cap, &copied)) {
        copied = 0;
    }
    if (pf != NULL) {
        pf->readTicks += svcGetSystemTick() - t0;
    }
    return copied;
}

/* The bake's SD cache (gdx3ds_hudbake.h): beside the archives, rewritten whenever it misses.
 * tools/prebake (tools/hudpack) writes the same file on the PC from the user's own ROM, so a
 * first boot with it on the card reads it like any cache. */
static void DashBakeCachePath(char* out, size_t cap, const char* suffix) {
    snprintf(out, cap, "%shudbake.cache%s", gdx3ds_fs_base_path(), suffix);
}

/* The whole file into one linear buffer, one read() call. NULL: none / unreadable (*why says). */
static unsigned char* DashBakeReadFile(const char* path, size_t* size, const char** why) {
    struct stat st;
    unsigned char* data;
    ssize_t got;
    const int fd = open(path, O_RDONLY);
    *why = "none";
    if (fd < 0) {
        return NULL;
    }
    if (fstat(fd, &st) != 0 || st.st_size <= 0 || st.st_size > (8 << 20)) {
        close(fd);
        *why = "unreadable";
        return NULL;
    }
    data = (unsigned char*) linearAlloc((size_t) st.st_size);
    if (data == NULL) {
        close(fd);
        *why = "out of memory";
        return NULL;
    }
    got = read(fd, data, (size_t) st.st_size);
    close(fd);
    if (got != (ssize_t) st.st_size) {
        linearFree(data);
        *why = "short read";
        return NULL;
    }
    *size = (size_t) st.st_size;
    return data;
}

static int DashBakeFromCache(void) {
    char path[192];
    const char* why = NULL;
    size_t size = 0;
    unsigned char* data;
    int ok;
    const u64 t0 = svcGetSystemTick();
    u64 c0;
    DashBakeCachePath(path, sizeof(path), "");
    data = DashBakeReadFile(path, &size, &why);
    if (data == NULL) {
        DashLog("[hudbake] cache not used: %s%s%s", why, strcmp(why, "none") == 0 ? " at " : "",
                strcmp(why, "none") == 0 ? path : "");
        return 0;
    }
    c0 = svcGetSystemTick();
    ok = gdx3ds_hudbake_cache_read(data, size, DashBakeFingerprint, NULL, DashBakeLog, NULL);
    DashLog("[hudbake] cache: %u bytes read in %lu us, checked + unpacked in %lu us (recipe id 0x%08X)",
            (unsigned) size, TicksToMicros(c0 - t0), TicksToMicros(svcGetSystemTick() - c0),
            gdx3ds_hudbake_recipe_id());
    linearFree(data);
    return ok;
}

/* One write() of the whole file to .tmp, then the .tmp replaces the cache (never a torn cache:
 * a reader rejects a short or damaged one anyway, and a missing one is just a miss). */
static int DashBakeWriteFile(const char* tmp, const char* path, const void* data, size_t len, const char** why) {
    ssize_t wrote;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        *why = "cannot create it";
        return 0;
    }
    wrote = write(fd, data, len);
    if (close(fd) != 0 || wrote != (ssize_t) len) {
        remove(tmp);
        *why = "short write";
        return 0;
    }
    remove(path);
    if (rename(tmp, path) != 0) {
        remove(tmp);
        *why = "rename failed";
        return 0;
    }
    return 1;
}

static void DashBakeWriteCache(void) {
    char path[192];
    char tmp[200];
    const char* why = NULL;
    const size_t need = gdx3ds_hudbake_cache_write(NULL, 0, DashBakeFingerprint, NULL);
    void* data;
    size_t len;
    if (need == 0) {
        DashLog("[hudbake] cache not written: the bake was incomplete");
        return;
    }
    data = linearAlloc(need);
    if (data == NULL) {
        DashLog("[hudbake] cache not written: out of memory");
        return;
    }
    len = gdx3ds_hudbake_cache_write(data, need, DashBakeFingerprint, NULL);
    DashBakeCachePath(path, sizeof(path), "");
    DashBakeCachePath(tmp, sizeof(tmp), ".tmp");
    if (len == 0) {
        DashLog("[hudbake] cache not written: a record has no fingerprint");
    } else if (!DashBakeWriteFile(tmp, path, data, len, &why)) {
        DashLog("[hudbake] cache not written: %s", why);
    } else {
        DashLog("[hudbake] cache written: %s (%u bytes)", path, (unsigned) len);
    }
    linearFree(data);
}

static void DashBakeDigestReceipt(void) {
    if (gdx3ds_config_get_bool("debug", "hudbake_digest", 0)) {
        const unsigned digest = gdx3ds_hudbake_digest();
        DashLog("[hudbake] digest 0x%08X (%s)", digest,
                digest == GDX3DS_HUDBAKE_DIGEST_RETAIL ? "matches the retail archive's" : "NOT the retail archive's");
    }
}

static void DashBakeFromArchive(int useCache) {
    GdxHudBakeStats st;
    DashPrefetch pf;
    unsigned long reads0 = 0;
    unsigned long reads1 = 0;
    unsigned long long bytes0 = 0;
    unsigned long long bytes1 = 0;
    size_t prefetched;
    const u64 t0 = svcGetSystemTick();
    u64 dt;
    gdx3ds_zipshim_io_stats(&reads0, &bytes0);
    prefetched = DashPrefetchLoad(&pf);
    gdx3ds_hudbake_run(DashBakeRead, &pf, DashBakeLog, NULL, &st);
    dt = svcGetSystemTick() - t0;
    gdx3ds_zipshim_io_stats(&reads1, &bytes1);
    DashLog("[hudbake] baked from fzerox.o2r: textures=%d missing=%d bytes=%lu in %lu us (archive %lu us, "
            "recipes %lu us) | prefetch %u bytes, %lu late reads | sd reads=%lu bytes=%llu | arena %u bytes linear",
            st.textures, st.missing, st.bytesRead, TicksToMicros(dt), TicksToMicros(pf.readTicks),
            TicksToMicros(dt - pf.readTicks), (unsigned) prefetched, pf.missesAfter, reads1 - reads0,
            bytes1 - bytes0, (unsigned) gdx3ds_hudbake_arena_bytes());
    DashPrefetchFree(&pf);
    DashBakeDigestReceipt();
    if (useCache) {
        const u64 w0 = svcGetSystemTick();
        DashBakeWriteCache();
        DashLog("[hudbake] cache write took %lu us", TicksToMicros(svcGetSystemTick() - w0));
    }
}

/* [debug] hudbake_cache (default 1): 0 bakes from the archive every boot and leaves the SD alone. */
void gdx3ds_bottomhud_bake_assets(void) {
    const int useCache = gdx3ds_config_get_bool("debug", "hudbake_cache", 1);
    const u64 t0 = svcGetSystemTick();
    sDirtyFull = 1;
    gdx3ds_hudbake_set_allocator(DashBakeAlloc, DashBakeFree);
    if (useCache && DashBakeFromCache()) {
        DashLog("[hudbake] loaded from the SD cache in %lu us", TicksToMicros(svcGetSystemTick() - t0));
        DashBakeDigestReceipt();
        return;
    }
    DashBakeFromArchive(useCache);
}

/* DEBUG LEVER (coordinator hw bug n60-3d7, item 2: "boost-ready did NOT turn green on hardware,
 * in a real GP race on lap 2+"). Round 5 only ever proved boostReady's read path agrees with
 * hud.c's via the INIT-path grant (practice mode); it never actually exercised the
 * lap-COMPLETION set-site (racer.c's `racer->stateFlags |= RACER_STATE_CAN_BOOST` at the
 * finish-line wrap check), because no replay in the corpus ever reached lap>=2. [debug]
 * force_lap2=1 plus racer.c's matching lever (see its own comment) fast-forwards a GP race
 * through lap 1 so that exact code path can be exercised in seconds. Investigation with this
 * lever (plus temporary edge-triggered logging, since reverted) found hud.c's read and DASH's
 * read agreeing at the exact same raceTimeMs through the real lap-completion path too, and the
 * bottom-screen energy bar visibly switching to the green family at that moment -- no bug found
 * in the read/paint logic. Kept as a permanent lever (default off) so the real-hardware boost
 * state can be reached and re-verified in seconds without driving a full GP lap by hand; see
 * docs/research/native/nibottom-progress.md for the full writeup. */
extern int gdx3ds_mp_session_levers_forced(void) __attribute__((weak)); /* gdx3ds_autodrive.c (MP30) */

int gdx_debug_force_lap2_enabled(void) {
    static int cached = -1;
    if (cached < 0) {
        cached = gdx3ds_config_get_bool("debug", "force_lap2", 0) ? 1 : 0;
    }
    if (cached && (&gdx3ds_mp_session_levers_forced != 0) && gdx3ds_mp_session_levers_forced()) {
        return 0; /* a lockstep session: never fast-forward one console's race */
    }
    return cached;
}

/* ---- init / levers -------------------------------------------------------------------- */

/* COORDINATOR FIX (hw bug n60-3d7, item 7): "when the HUD mode was saved, the game REWROTE the
 * user's gdiffuser.ini. Comments were dropped, the [variant.T/P/S/V] section names were
 * lowercased... keys moved, and a [hud] section was added. Never rewrite the user's ini
 * wholesale. Persist the HUD mode in its own small file."
 *
 * Root cause: gdx3ds_config_save (gdx3ds_config.c) is the WHOLE config table's writer -- by
 * design, not a bug specific to this lever: it self-documents "rewritten by the in-game menu
 * (comments are not preserved)" and is the same function gdx3ds_menu.c's MenuSaveCfg uses for
 * deliberate settings edits from the paused debug menu, where a full rewrite is an accepted
 * trade-off. The HUD placement lever is different: gdx3ds_bottomhud_cycle_mode can fire from a
 * SELECT press or a chip tap mid-race (see its own comment below), so every tap was rewriting
 * the user's entire ini just to persist one int. Fix: route ONLY this one key through its own
 * tiny dedicated file instead, never touching gdiffuser.ini at all.
 *
 * Variant-loader note (the coordinator also asked to "confirm the variant loader still works
 * with lowercased names, or document that it doesn't"): it does. gdx3ds_config.c's loader
 * lowercases every section AND key name on parse (gdx3ds_cfg_copy_lower, ~line 119/140) --
 * this happens for ALL entries, unconditionally, even from a pristine original-cased file --
 * and every lookup (gdx3ds_config_get_*, gdx3ds_config_section_entry) lowercases its own query
 * the same way before comparing. gdx3ds_launch_core.c's VariantSection builds "variant.%s" from
 * the launch-held button's name as-is (e.g. "variant.T") and hands it straight to
 * gdx3ds_config_section_entry, which lowercases it to "variant.t" before matching -- the exact
 * same normalisation the stored entries already went through on load, whatever case the file on
 * disk used. So a prior accidental lowercase rewrite of [variant.T] -> [variant.t] does NOT
 * break variant selection; the whole config module is case-insensitive end to end by
 * construction, not only for this key. */
#define GDX3DS_HUD_MODE_PATH "sdmc:/3ds/gdiffuser/hud.ini"

/* Fills every row of `out` with sDashWidgets' own hard-coded rects (the exact ALL_BOTTOM
 * geometry), enabled=1 -- CUSTOM mode's "everything on the bottom" starting point, and what
 * RESET always restores. */
static void DashCustomDefaults(DashWidgetRect* out) {
    int i;
    for (i = 0; i < DASHW_COUNT; i++) {
        out[i] = sDashWidgets[i];
        out[i].enabled = 1;
    }
}

/* Tells the real top-screen gating (port/gdx_hud_placement.c's gdx_hud_set_custom_top_mask, as
 * of the native/nihudtop merge) which bottom-screen elements are currently hidden on the bottom
 * and so belong on top instead, and whether CUSTOM mode is the active placement mode at all.
 * Called on every CUSTOM layout/mode change (RESET, DONE, and the mode-cycle transition into/out
 * of CUSTOM), never every frame. */
static void DashSyncCustomTopMask(void) {
    unsigned mask = 0;
    int i;
    for (i = 0; i < DASHW_COUNT; i++) {
        if (kDashWidgetTopElem[i] < 0) {
            continue;
        }
        if (!sCustomWidgets[i].enabled) {
            mask |= (1u << (unsigned) kDashWidgetTopElem[i]);
        }
    }
    gdx_hud_set_custom_top_mask(mask, sPlacementMode == GDX_HUD_PLACEMENT_CUSTOM);
}

/* Single-purpose writer for the WHOLE small hud.ini file -- both the [hud] placement int (hw
 * bug n60-3d7, item 7's own fix: never gdiffuser.ini) and, now, the [custom] section (one line
 * per editable widget: "id = x y w h visible"). Both sections are written together so neither
 * save path can ever silently drop the other one's last-known state. Called from the placement
 * lever's existing safe points (race-end / menu-close, unchanged) AND from DashCustomReset/the
 * EDIT->DONE transition -- per the brief, the custom section is written ONLY on DONE or RESET,
 * never every frame. */
static void DashHudIniSave(void) {
    FILE* fp = fopen(GDX3DS_HUD_MODE_PATH, "w");
    int i;
    if (fp == NULL) {
        DashLog("[dash] WARNING: hud.ini save failed (%s)", GDX3DS_HUD_MODE_PATH);
        return;
    }
    fprintf(fp, "; sdmc:/3ds/gdiffuser/hud.ini -- just the DASH placement lever and the CUSTOM\n");
    fprintf(fp, "; layout (hw bug n60-3d7, item 7): this file is rewritten whole every time\n");
    fprintf(fp, "; either one changes, but it never touches the main gdiffuser.ini, so nothing\n");
    fprintf(fp, "; else is ever at risk.\n");
    fprintf(fp, "[hud]\n");
    fprintf(fp, "placement = %d\n", sPlacementMode);
    fprintf(fp, "[custom]\n");
    for (i = 0; i < DASHW_COUNT; i++) {
        char line[64];
        GdxHudCustomRect rc;
        if (!DashWidgetEditable(i)) {
            continue;
        }
        /* An explicit field-by-field copy, NOT a pointer cast: DashWidgetRect's own layout
         * leads with `id` (a pointer), so x/y/w/h/enabled are NOT at the same offsets
         * gdx_hudcustom_layout.h's GdxHudCustomRect expects -- see that header's own comment on
         * the field-order assumption this copy satisfies honestly instead. */
        rc.x = sCustomWidgets[i].x;
        rc.y = sCustomWidgets[i].y;
        rc.w = sCustomWidgets[i].w;
        rc.h = sCustomWidgets[i].h;
        rc.enabled = sCustomWidgets[i].enabled;
        if (gdx_hudcustom_format_line(line, (int) sizeof(line), &rc) < 0) {
            continue; /* defensive only: DASHW_COUNT rects always fit comfortably in `line` */
        }
        fprintf(fp, "%s = %s\n", sCustomWidgets[i].id, line);
    }
    fclose(fp);
}

/* Trims leading/trailing whitespace from `s` in place, same spirit as the pre-existing
 * placement-only parser this replaces. Returns s for chaining. */
static char* DashTrim(char* s) {
    char* end;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') {
        s++;
    }
    end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) {
        *--end = '\0';
    }
    return s;
}

/* Single-purpose reader for the whole file: tracks the current `[section]` and, inside it,
 * parses one "key = value" pair per line. [hud]'s only key is "placement"; [custom]'s keys are
 * the DASHW_* id strings (sDashWidgets[i].id), each matched against gdx_hudcustom_parse_line's
 * robust "x y w h v" parser. A garbage/short [custom] line, or a line for an id this build
 * doesn't recognise, leaves that widget's entry exactly as the caller already defaulted it
 * (DashCustomDefaults) -- the whole point of routing every field through
 * gdx_hudcustom_parse_line's own validation instead of a bare atoi(). Independent of the shared
 * gdx3ds_config_* table for the same reason the old placement-only reader was (see that
 * function's prior comment, now folded into this one): that table is a process-wide singleton
 * and cannot be safely reloaded here. */
static void DashHudIniLoad(int* outMode, DashWidgetRect* customInOut) {
    FILE* fp = fopen(GDX3DS_HUD_MODE_PATH, "r");
    char line[128];
    char section[16] = "";
    if (fp == NULL) {
        return; /* no file yet: first boot after this fix, or a fresh SD card */
    }
    while (fgets(line, sizeof(line), fp) != NULL) {
        char* trimmed = DashTrim(line);
        char* eq;
        char* key;
        char* value;
        if (trimmed[0] == '\0' || trimmed[0] == ';' || trimmed[0] == '#') {
            continue;
        }
        if (trimmed[0] == '[') {
            char* close = strchr(trimmed, ']');
            size_t n;
            if (close == NULL) {
                continue;
            }
            n = (size_t) (close - (trimmed + 1));
            if (n >= sizeof(section)) {
                n = sizeof(section) - 1;
            }
            memcpy(section, trimmed + 1, n);
            section[n] = '\0';
            continue;
        }
        eq = strchr(trimmed, '=');
        if (eq == NULL) {
            continue;
        }
        *eq = '\0';
        key = DashTrim(trimmed);
        value = DashTrim(eq + 1);
        if (strcmp(section, "hud") == 0) {
            if (strcmp(key, "placement") == 0) {
                *outMode = atoi(value);
            }
            continue;
        }
        if (strcmp(section, "custom") == 0 && customInOut != NULL) {
            int i;
            for (i = 0; i < DASHW_COUNT; i++) {
                GdxHudCustomRect parsed;
                if (!DashWidgetEditable(i) || strcmp(key, customInOut[i].id) != 0) {
                    continue;
                }
                if (gdx_hudcustom_parse_line(value, DashWidgetMinW(i), DashWidgetMinH(i), DASH_HEADER_Y0, &parsed)) {
                    customInOut[i].x = parsed.x;
                    customInOut[i].y = parsed.y;
                    customInOut[i].w = parsed.w;
                    customInOut[i].h = parsed.h;
                    customInOut[i].enabled = parsed.enabled;
                }
                break;
            }
            continue;
        }
    }
    fclose(fp);
}

void gdx3ds_bottomhud_init(void) {
    int loadedMode = -1;
    /* DEFAULT ON (n60lead): the DASH is now the accepted default bottom-screen HUD. An
     * explicit `bottom_race = 0` in the ini still wins -- gdx3ds_config_get_bool only ever
     * hands back the fallback (1, here) when the key is absent or unparsable. */
    sEnabled = gdx3ds_config_get_bool("debug", "bottom_race", 1) ? 1 : 0;
    /* IDLE VIEW (default ON, independent lever from bottom_race -- see gdx3ds_bottomhud.h's own
     * "IDLE VIEW" doc comment): [debug] bottom_idle=0 turns off just the idle-view polish and
     * falls back to the text-console debug UI outside races, same as it always was. */
    sIdleEnabled = gdx3ds_config_get_bool("debug", "bottom_idle", 1) ? 1 : 0;
    sIdleDismissed = 0;
    sIdleBaked = 0;
    sIdlePrevBackTouch = 0;
    sIdleSelectHoldTicks = 0;
    DashCustomDefaults(sCustomWidgets);
    /* hud.ini (if present) is the canonical source from here on; [hud] placement in the main
     * gdiffuser.ini is only a one-time migration fallback for a save made before this fix (or a
     * hand-edit) -- see the fix note above GDX3DS_HUD_MODE_PATH. DashHudIniLoad also fills in
     * any saved [custom] rows over the just-set defaults above. */
    DashHudIniLoad(&loadedMode, sCustomWidgets);
    if (loadedMode >= 0) {
        sPlacementMode = loadedMode;
    } else {
        sPlacementMode = gdx3ds_config_get_int("hud", "placement", GDX_HUD_PLACEMENT_ALL_TOP);
    }
    if (sPlacementMode < 0 || sPlacementMode > GDX_HUD_PLACEMENT_CUSTOM) {
        sPlacementMode = GDX_HUD_PLACEMENT_ALL_TOP;
    }
    /* SHELF: a hud.ini written before CUSTOM was shelved (or hand-edited) can still say
     * placement=3 with the lever off today -- fall back to ALL_BOTTOM rather than silently
     * resurrecting CUSTOM behind the user's back. */
    if (sPlacementMode == GDX_HUD_PLACEMENT_CUSTOM && !HudCustomLeverEnabled()) {
        DashLog("[dash] hud_custom=0: saved placement=CUSTOM unavailable, falling back to ALL_BOTTOM");
        sPlacementMode = GDX_HUD_PLACEMENT_ALL_BOTTOM;
    }
    sHz = gdx3ds_config_get_int("debug", "bottom_race_hz", 10);
    if (sHz < 1) {
        sHz = 1;
    } else if (sHz > 60) {
        sHz = 60;
    }
    {
        bool isNew = true;
        if (R_FAILED(APT_CheckNew3DS(&isNew))) {
            isNew = true;
        }
        sDashEvery = gdx3ds_config_get_int("ui", "dash_every", isNew ? 1 : 4);
        /* o3ds60 [ui] mp_hud_placement (default ON on both models; mp-polish: the user saw the
         * top HUD in a placement=3 MP race on a New 3DS too): an MP
         * fullview race's one full-screen view follows [hud] placement like the 1P race does
         * (gdx3ds_bottomhud_fullview_placement below). Off = the 2-4P force-ALL_TOP rule as
         * shipped. Render-only: menus.c still runs Hud_DrawHud's logic and rewinds the picture
         * (decomp-3ds-o3ds60-mp-hud.patch), so no game state depends on it. */
        sMpHudPlacement = gdx3ds_config_get_bool("ui", "mp_hud_placement", 1) ? 1 : 0;
        sDashSpread = gdx3ds_config_get_int("ui", "dash_spread", isNew ? 0 : 1);
        if (sDashSpread < 0 || sDashSpread > 2) {
            sDashSpread = 1;
        }
        sDashBudget = gdx3ds_config_get_bool("ui", "dash_budget", isNew ? 0 : 1) ? 1 : 0;
        sDashBudgetUs = gdx3ds_config_get_int("ui", "dash_budget_us", 1000);
        if (sDashBudgetUs < 100) {
            sDashBudgetUs = 100;
        } else if (sDashBudgetUs > 16000) {
            sDashBudgetUs = 16000;
        }
        sDashMapInc = (sDashBudget && gdx3ds_config_get_bool("ui", "dash_map_inc", isNew ? 0 : 1)) ? 1 : 0;
        sMapIncValid = 0;
        sDashRace0D1 = gdx3ds_cpucut_clamp(gdx3ds_config_get_int("ui", "dash_race0_d1", 0), 0);
        gdx3ds_cpucut_lever("dashr0", &sDashRace0D1, GDX3DS_CPUCUT_BIT_DASH);
        sDashEveryD1 = gdx3ds_config_get_int("ui", "dash_every_d1", isNew ? 0 : 6);
        if (sDashEveryD1 < 0 || sDashEveryD1 > 30) {
            sDashEveryD1 = 0;
        }
        if (sDashEvery < 1) {
            sDashEvery = 1;
        } else if (sDashEvery > 30) {
            sDashEvery = 30;
        }
    }
    sDashMemo = gdx3ds_config_get_bool("ui", "dash_memo", 1) ? 1 : 0;
    sDashProf = gdx3ds_config_get_bool("debug", "dash_prof", 0) ? 1 : 0;
    sDirtyFull = 1;
    sWasValid = 0;
    sCustomBackdropPainted = 0;
    sEditMode = 0;
    sEditSelected = -1;
    sLastTop5Count = -1;
    sLastHeaderRacersRemaining = -1;
    sLastHeaderTrackName[0] = '\0';
    sLastSpareMachines = DASH_LIVES_UNPAINTED;
    /* [debug] bottom_fps (default ON -- user ask): the DASH's own drawn-fps text, top-right inside
     * the map box. Independent of [debug] fps (the debug console's top-row counter). */
    sFpsEnabled = gdx3ds_config_get_bool("debug", "bottom_fps", 1) ? 1 : 0;
    /* VERIFICATION AID, not a user-facing feature: [debug] hud_edit_start=1 forces CUSTOM mode
     * with edit mode already active from the very first tick, so an emulator/hardware capture
     * can show the edit overlay (selection, close/resize handles, the hidden-widget tray)
     * without first driving a scripted touch sequence to reach it -- the brief's own named
     * fallback ("a debug ini lever that pre-loads a moved/hidden layout") for exactly this case.
     * Combine with a hand-staged hud.ini [custom] section (one widget moved, one hidden) to also
     * demonstrate a non-default saved layout rendering correctly. */
    if (gdx3ds_config_get_bool("debug", "hud_edit_start", 0)) {
        if (HudCustomLeverEnabled()) {
            sPlacementMode = GDX_HUD_PLACEMENT_CUSTOM;
            sEditMode = 1;
            DashLog("[dash] debug hud_edit_start=1: forcing CUSTOM+edit at boot");
        } else {
            DashLog("[dash] debug hud_edit_start=1 ignored: [debug] hud_custom=0 (CUSTOM shelved)");
        }
    }
    /* VERIFICATION AID, not a user-facing feature, same idea as hud_edit_start above: [debug]
     * idle_dismiss_start=1 forces the idle view's own "dismissed" state from the very first tick,
     * so a capture can show the console UI with its "DASH" return tab (TAB_RACE, repainted per
     * MenuTabRaceLabel) without needing a touch sequence to reach it -- there is no scriptable
     * touch/key-hold path into this harness (the autoinput/.gdxrec replay system drives the N64
     * virtual pad, not real 3DS HID, which is what gdx3ds_menu.c's touch/SELECT handling reads),
     * same reason hud_edit_start exists. */
    if (gdx3ds_config_get_bool("debug", "idle_dismiss_start", 0)) {
        sIdleDismissed = 1;
        DashLog("[dash] debug idle_dismiss_start=1: forcing idle-dismissed (console+DASH tab) at boot");
    }
    DashSyncCustomTopMask();
    /* hud_custom/modes is a direct, greppable receipt for the SHELF gate (nibottom-progress.md
     * §12): modes=3 with no "CUSTOM" in the cycle whenever hud_custom=0 (the default), modes=4
     * when someone has explicitly opted back in. */
    DashLog("[dash] init enabled=%d idle=%d placement=%s hz=%d hud_custom=%d modes=%d fps=%d every=%d memo=%d prof=%d mp_hud=%d spread=%d "
            "budget=%d budget_us=%d every_d1=%d map_inc=%d race0_d1=%d",
            sEnabled, sIdleEnabled,
            sPlacementMode == GDX_HUD_PLACEMENT_CUSTOM ? "CUSTOM" : gdx_hud_placement_name(sPlacementMode), sHz,
            HudCustomLeverEnabled(),
            HudCustomLeverEnabled() ? (GDX_HUD_PLACEMENT_CUSTOM + 1) : GDX_HUD_PLACEMENT_MODE_COUNT, sFpsEnabled,
            sDashEvery, sDashMemo, sDashProf, sMpHudPlacement, sDashSpread, sDashBudget, sDashBudgetUs,
            sDashEveryD1, sDashMapInc, sDashRace0D1);
}

int gdx3ds_bottomhud_enabled(void) {
    return sEnabled;
}

int gdx3ds_bottomhud_race_live(void) {
    GdxRaceHudSnapshot snap;
    if (!sEnabled) {
        return 0;
    }
    if (gdx_title_demo_active()) {
        return 0; /* attract demo: the idle view keeps the screen, not the race DASH */
    }
    gdx_racehud_snapshot(&snap);
    return snap.valid;
}

/* This is the function port/gdx_hud_placement.c's gdx_hud_placement_mode() actually calls
 * (gdx3ds_bottomhud.h's own comment on this function says so). GDX_HUD_PLACEMENT_CUSTOM is not
 * one of the 3 values gdx_hud_placement_mode()'s callers originally knew about, so this reports
 * ALL_BOTTOM whenever the real mode is CUSTOM -- a safety fallback now, not the primary CUSTOM
 * path: gdx_hud_top_element_mask() (port/gdx_hud_placement.c) checks the real
 * gdx_hud_set_custom_top_mask() override BEFORE it ever calls gdx_hud_placement_mode() at all
 * (DashSyncCustomTopMask below keeps that override's mask/customActive flag in sync on every
 * CUSTOM layout/mode change), so in the normal 1P-CUSTOM case this function's ALL_BOTTOM answer
 * is never even consulted. It only matters as the effective fallback for numPlayers>1, where the
 * override is deliberately ignored and FORCED ALL_TOP IN 2-4P already overrides any mode number
 * regardless, so ALL_BOTTOM-vs-anything-else makes no difference there either.
 * gdx3ds_bottomhud_tick/_cycle_mode/_placement_mode's own internal logic (the MODE chip's label,
 * CUSTOM's own paint/edit path) all read sPlacementMode directly instead, so they still see the
 * real value. */
/* port/gdx_hud_placement.c asks this before applying the 2-4P force-ALL_TOP rule: nonzero
 * means "this frame draws one MP fullview view, answer as the 1P race". Only inside Race_Draw's
 * fullview latch (gdx_fv_k), never during a demo, never when the DASH itself is off. */
int gdx3ds_bottomhud_fullview_placement(void) {
    extern int gdx_fv_k(void) __attribute__((weak)); /* game/gdx3ds_fullview_game.c */
    if (!sMpHudPlacement || !sEnabled || gdx_fv_k == NULL || gdx_fv_k() < 0) {
        return 0;
    }
    if (gdx_title_demo_active()) {
        return 0;
    }
    return 1;
}

int gdx3ds_bottomhud_placement_mode(void) {
    if (gdx_title_demo_active()) {
        return GDX_HUD_PLACEMENT_ALL_TOP; /* no DASH during a demo, so nothing may leave the top screen */
    }
    if (sPlacementMode == GDX_HUD_PLACEMENT_CUSTOM) {
        return GDX_HUD_PLACEMENT_ALL_BOTTOM;
    }
    return sPlacementMode;
}

/* ADVISOR (nibottom Phase 2 check-in): an SD write is fine from the MENU's other setters (always
 * a deliberate, paused-ish user edit), but this one can fire from a KEY_SELECT press or a chip
 * tap WHILE A RACE IS RUNNING, and a stall there is a dropped frame on Old 3DS.
 * gdx3ds_config_set_int (the in-memory table update that makes the mode take effect immediately,
 * and keeps the shared table's own "[hud] placement" mirror consistent for anything else that
 * might read it this session) stays unconditional; only the actual file write is deferred, to
 * the next safe point: the race's own valid 1->0 transition (gdx3ds_bottomhud_tick, below) or a
 * non-race "menu close" (gdx3ds_bottomhud_flush_pending_save, called from gdx3ds_menu.c's
 * MenuEnterOff). DashHudModeSave, not gdx3ds_config_save -- see that function's own fix note
 * (hw bug n60-3d7, item 7): this lever gets its own small file, never gdiffuser.ini. */
void gdx3ds_bottomhud_cycle_mode(void) {
    const int inRace = (&gdx_input_in_gameplay != NULL) && gdx_input_in_gameplay();
    /* SHELF: with [debug] hud_custom=0 (default) the chip/SELECT only cycles the 3 fixed modes
     * (ALL_TOP -> SPLIT -> ALL_BOTTOM -> ALL_TOP), exactly as before CUSTOM existed. hud_custom=1
     * restores the +1-beyond-CUSTOM cycle (ALL_TOP -> SPLIT -> ALL_BOTTOM -> CUSTOM -> ALL_TOP). */
    const int modeCount = HudCustomLeverEnabled() ? (GDX_HUD_PLACEMENT_CUSTOM + 1) : GDX_HUD_PLACEMENT_MODE_COUNT;
    sPlacementMode = (sPlacementMode + 1) % modeCount;
    sCustomBackdropPainted = 0; /* repaint whichever kind of backdrop this new mode needs */
    if (sPlacementMode != GDX_HUD_PLACEMENT_CUSTOM) {
        sEditMode = 0; /* leaving CUSTOM always leaves edit mode too -- see DONE's own comment */
    }
    DashSyncCustomTopMask();
    /* RAM-only; safe mid-race -- and, since hw bug n60-3d8 item 2, safe for gdiffuser.ini too:
     * this DOES mark "hud"/"placement" dirty in the shared gdx3ds_config table (that mirror is
     * the whole point -- see the comment above), and a LATER, unrelated MenuSaveCfg() in the
     * same session will see it dirty, but gdx3ds_config_save() now refuses to ever let a [hud]
     * key reach gdiffuser.ini, dirty or not, pre-existing or new. That save-side filter is the
     * actual fix; this call did not change. NOTE: gdx3ds_config_set_int clamps/accepts any int,
     * including CUSTOM's 3 -- nothing else in the shared table interprets this key, so there is
     * no cross-module risk in it momentarily holding a value port/input_bridge.c's own
     * GDX_HUD_PLACEMENT_* enum doesn't define; gdx3ds_bottomhud_placement_mode() is the only
     * thing that value is read back through on this build, and it never forwards a raw 3 (see
     * its own comment). */
    gdx3ds_config_set_int("hud", "placement", sPlacementMode);
    if (inRace) {
        sPlacementSaveDirty = 1;
    } else {
        DashHudIniSave();
    }
    DashLog("[dash] mode -> %s%s",
            sPlacementMode == GDX_HUD_PLACEMENT_CUSTOM ? "CUSTOM" : gdx_hud_placement_name(sPlacementMode),
            inRace ? " (save deferred)" : "");
}

void gdx3ds_bottomhud_flush_pending_save(void) {
    if (!sPlacementSaveDirty) {
        return;
    }
    DashHudIniSave();
    sPlacementSaveDirty = 0;
    DashLog("[dash] mode save flushed");
}

void gdx3ds_bottomhud_note_shot(const char* label) {
    if (!sEnabled || label == NULL || label[0] == '\0') {
        return;
    }
    snprintf(sPendingShot, sizeof(sPendingShot), "%s", label);
    __atomic_store_n(&sShotArmed, 1, __ATOMIC_RELEASE);
}

/* COORDINATOR FIX (hardware bug report n60-3d7, item 1): "scattered glitchy black lines on some
 * widgets (energy, position)." Root cause: the bottom screen is single-buffered (consoleInit's
 * GFX_BOTTOM console disables double buffering internally so a plain console can write straight
 * into the displayed buffer) and gdx3ds_bottomhud_tick painted widgets directly into that live,
 * on-screen buffer with many separate DashFillRect/DashPixel/DashBlit* calls spread over the
 * tick's whole duration (the "REPAINT-RATE DISCIPLINE" dirty-rect scheme above) -- exactly the
 * "clear-then-draw into the front buffer" pattern the bug report names: the LCD's scanout can
 * (and on real hardware, demonstrably does) catch the screen mid-paint, between a widget's
 * clear and its redraw, and show the cleared (black) rect for one or more frames.
 *
 * Fix: every Dash* paint call below now targets this off-screen shadow buffer instead of the
 * live framebuffer; gdx3ds_bottomhud_tick does ONE memcpy of the complete shadow buffer into
 * the live buffer at the very end of each tick that painted anything. The live buffer therefore
 * only ever holds a FULLY complete frame (last tick's, or this tick's) -- never a half-drawn
 * widget -- and the window in which the scanout could possibly catch an inconsistent image
 * shrinks from "the whole tick's worth of scattered draw calls" down to the duration of one
 * contiguous 150 KiB memcpy (sub-millisecond on this hardware), which can only ever expose two
 * adjacent rows of two DIFFERENT complete frames, never a cleared one. Deliberately NOT
 * gfxSetDoubleBuffering(GFX_BOTTOM, true) + gfxSwapBuffers(): see gdx3ds_os_ctru.c's own
 * window_swap commentary for why a second swap path on top of the existing
 * C3D_FrameBegin(SYNCDRAW) top-screen pacing has already caused a real fps regression and a
 * black-screen regression on this exact hardware target; this fix stays entirely inside DASH's
 * own (cold, non-NRR-hot) code and touches no gfx/GSP swap semantics at all. */
static uint16_t sShadowFb[DASH_FB_W * DASH_FB_H];

/* ---- pixel helpers (same portrait/column-major convention as gdx3ds_racemap.c) -------- */

static inline void DashPixel(uint16_t* fb, int x, int y, uint16_t colour) {
    if (x < 0 || x >= DASH_FB_W || y < 0 || y >= DASH_FB_H) {
        return;
    }
    if (x < sDirtyX0) {
        sDirtyX0 = x; /* o3dsperf: the partial present copies the columns written this tick */
    }
    if (x > sDirtyX1) {
        sDirtyX1 = x;
    }
    sDirtyColBits[x >> 5] |= 1u << (x & 31);
    fb[(size_t) x * DASH_FB_H + (DASH_FB_H - 1 - y)] = colour;
}

/* o3dsperf: the columns of the shadow this tick painted (the fb is column-major, so a run of
 * columns is one contiguous block). The present copies only that block; every paint primitive
 * below marks the columns it can touch. sDirtyFull = the whole screen (a bake, an overlay, a
 * new live buffer). Inclusive bounds, clamped to the screen. */

static void DashMarkCols(int x0, int w) {
    int x1 = x0 + w - 1;
    if (w <= 0) {
        return;
    }
    if (x0 < 0) {
        x0 = 0;
    }
    if (x1 >= DASH_FB_W) {
        x1 = DASH_FB_W - 1;
    }
    if (x1 < x0) {
        return;
    }
    if (x0 < sDirtyX0) {
        sDirtyX0 = x0;
    }
    if (x1 > sDirtyX1) {
        sDirtyX1 = x1;
    }
    {
        int x;
        for (x = x0; x <= x1; x++) {
            sDirtyColBits[x >> 5] |= 1u << (x & 31);
        }
    }
}

/* o3ds60 dash_map_inc: row ranges per column (screen y, inclusive). */
static void DashRowsReset(int16_t* y0, int16_t* y1) {
    int x;
    for (x = 0; x < DASH_FB_W; x++) {
        y0[x] = INT16_MAX;
        y1[x] = -1;
    }
}

static void DashRowsWiden(int16_t* y0, int16_t* y1, int x, int ya, int yb) {
    if (x < 0 || x >= DASH_FB_W) {
        return;
    }
    if (ya < 0) {
        ya = 0;
    }
    if (yb >= DASH_FB_H) {
        yb = DASH_FB_H - 1;
    }
    if (yb < ya) {
        return;
    }
    if (ya < y0[x]) {
        y0[x] = (int16_t) ya;
    }
    if (yb > y1[x]) {
        y1[x] = (int16_t) yb;
    }
}

/* These rows of column x go to the live buffer at this tick's present. */
static void DashMarkRows(int x, int ya, int yb) {
    if (!sRowAny) {
        DashRowsReset(sRowY0, sRowY1);
        sRowAny = 1;
    }
    DashRowsWiden(sRowY0, sRowY1, x, ya, yb);
}

/* o3dsperf [ui] dash_memo: 1 when widget `id` already shows `key` (the text / value it would
 * paint) since its last paint, so the paint can be skipped; otherwise records `key` and returns 0.
 * Never in CUSTOM / edit mode (the generic widgets move); reset by every backdrop bake. */
static void DashMemoReset(void) {
    memset(sMemoValid, 0, sizeof(sMemoValid));
}

static int DashMemoSame(int id, const char* key) {
    if (!sDashMemo || sPlacementMode == GDX_HUD_PLACEMENT_CUSTOM || sEditMode) {
        sMemoValid[id] = 0;
        return 0;
    }
    if (sMemoMode != sPlacementMode) {
        DashMemoReset();
        sMemoMode = sPlacementMode;
    }
    if (sMemoValid[id] && strncmp(sMemoKey[id], key, DASH_MEMO_KEY) == 0) {
        return 1;
    }
    strncpy(sMemoKey[id], key, DASH_MEMO_KEY - 1);
    sMemoKey[id][DASH_MEMO_KEY - 1] = '\0';
    sMemoValid[id] = 1;
    return 0;
}

/* Same pixels as a DashPixel per texel, one clipped column run at a time (no per-pixel bounds
 * test, no per-pixel index math). */
static void DashFillRect(uint16_t* fb, int x0, int y0, int w, int h, uint16_t colour) {
    int x1 = x0 + w;
    int y1 = y0 + h;
    int x;
    if (x0 < 0) {
        x0 = 0;
    }
    if (y0 < 0) {
        y0 = 0;
    }
    if (x1 > DASH_FB_W) {
        x1 = DASH_FB_W;
    }
    if (y1 > DASH_FB_H) {
        y1 = DASH_FB_H;
    }
    if (x1 <= x0 || y1 <= y0) {
        return;
    }
    DashMarkCols(x0, x1 - x0);
    for (x = x0; x < x1; x++) {
        uint16_t* p = fb + (size_t) x * DASH_FB_H + (DASH_FB_H - y1);
        int n;
        for (n = y1 - y0; n > 0; n--) {
            *p++ = colour;
        }
    }
}

static void DashStrokeRect(uint16_t* fb, int x0, int y0, int w, int h, uint16_t colour) {
    int x;
    int y;
    DashMarkCols(x0, w);
    for (x = x0; x < x0 + w; x++) {
        DashPixel(fb, x, y0, colour);
        DashPixel(fb, x, y0 + h - 1, colour);
    }
    for (y = y0; y < y0 + h; y++) {
        DashPixel(fb, x0, y, colour);
        DashPixel(fb, x0 + w - 1, y, colour);
    }
}

/* RGBA5551 (r5 g5 b5 a1, MSB-first) -> RGB565, same conversion gdx3ds_racemap.c's marker
 * palette uses. a=0 is transparent (the caller skips the write, not this function). */
static inline uint16_t Rgba5551To565(unsigned short v) {
    const unsigned r = (v >> 11) & 31u;
    const unsigned g = (v >> 6) & 31u;
    const unsigned b = (v >> 1) & 31u;
    return (uint16_t) ((r << 11) | (((g << 1) | (g >> 4)) << 5) | b);
}

/* Darkens + lightly desaturates a baked RGB565 background texel in place, toward its own luma.
 * COORDINATOR FIX (second visual-feedback round, item 3): "the background is too bright... tone
 * down the pink checker: darker or more desaturated, lower contrast... keep it pink, but make it
 * a backdrop, not the focus." ~35% darker, blended 20% toward grey -- enough to read as a muted
 * backdrop instead of a foreground pattern, without losing the pink hue entirely. */
static inline uint16_t DashDimBackground(uint16_t c) {
    unsigned r = ((c >> 11) & 31u) * 255u / 31u;
    unsigned g = ((c >> 5) & 63u) * 255u / 63u;
    unsigned b = (c & 31u) * 255u / 31u;
    const unsigned luma = (r * 30u + g * 59u + b * 11u) / 100u;
    r = (r * 65u) / 100u;
    g = (g * 65u) / 100u;
    b = (b * 65u) / 100u;
    r = (r * 80u + luma * 20u) / 100u;
    g = (g * 80u + luma * 20u) / 100u;
    b = (b * 80u + luma * 20u) / 100u;
    return (uint16_t) (((r * 31u / 255u) << 11) | ((g * 63u / 255u) << 5) | (b * 31u / 255u));
}

/* COORDINATOR FIX (third visual-feedback round, item 1): "the hand-drawn 3x5 font breaks an
 * explicit user requirement... Replace it with the real glyph atlases, baked into the C headers
 * from the o2r... Scale them to size with integer nearest-neighbour." gdx3ds_hudbake.c is that
 * bake (run once at boot, nothing compiled in): real RGBA5551 pixels sliced straight from the
 * user's fzerox.o2r's own aTimerSymbolsTex/aLapCounterSymbolsTex/aPosition<N>Tex/
 * aPositionOrdinalSuffix<X>Tex/aFont1<X>Tex/aFont2<X>Tex. This file's own hand font is gone --
 * every blit below goes through DashBlitGlyphRatio (a single nearest-neighbour scaler, num/den
 * both >= 1, so it covers upscaling a tiny icon-sized texture AND downscaling a big one, e.g.
 * the 28x32 position digits at num=1,den=2 to fit two of them in the 64px-wide OUR-position box)
 * or the DashBlitGlyphTable* helpers that walk a whole string against one of the tables above. */

/* o3dsperf: floor(i * den / num) for i in [0, n), by accumulation (the ARM11 has no divide
 * instruction; the old per-texel `/` was a library call per pixel). Exact, same values. */
#define DASH_SCALE_TAB_MAX 256
static void DashScaleTable(int16_t* out, int n, int num, int den) {
    int q = 0;
    int r = 0;
    int i;
    for (i = 0; i < n; i++) {
        out[i] = (int16_t) q;
        r += den;
        while (r >= num) {
            r -= num;
            q++;
        }
    }
}

static void DashBlitGlyphRatioSlow(uint16_t* fb, int x, int y, const DashGlyph* g, int num, int den) {
    int dy;
    const int dw = (g->w * num) / den;
    const int dh = (g->h * num) / den;
    for (dy = 0; dy < dh; dy++) {
        const int sy = (dy * den) / num;
        int dx;
        for (dx = 0; dx < dw; dx++) {
            const int sx = (dx * den) / num;
            const unsigned short p = g->pixels[sy * g->w + sx];
            if ((p & 1u) == 0u) {
                continue; /* alpha=0 */
            }
            DashPixel(fb, x + dx, y + dy, Rgba5551To565(p));
        }
    }
}

/* Nearest-neighbour glyph blit: the same texel -> pixel mapping (and so the same pixels) as the
 * plain per-texel loop above, with the scale steps tabulated once per blit and each destination
 * column written as one run. */
static void DashBlitGlyphRatio(uint16_t* fb, int x, int y, const DashGlyph* g, int num, int den) {
    int16_t sxTab[DASH_SCALE_TAB_MAX];
    int16_t syTab[DASH_SCALE_TAB_MAX];
    const int dw = (g->w * num) / den;
    const int dh = (g->h * num) / den;
    int dx;
    if (dw <= 0 || dh <= 0) {
        return;
    }
    if (dw > DASH_SCALE_TAB_MAX || dh > DASH_SCALE_TAB_MAX) {
        DashMarkCols(x, dw);
        DashBlitGlyphRatioSlow(fb, x, y, g, num, den);
        return;
    }
    DashMarkCols(x, dw);
    DashScaleTable(sxTab, dw, num, den);
    DashScaleTable(syTab, dh, num, den);
    for (dx = 0; dx < dw; dx++) {
        const int px = x + dx;
        const unsigned short* src = g->pixels + sxTab[dx];
        uint16_t* col;
        int dy;
        if (px < 0 || px >= DASH_FB_W) {
            continue;
        }
        col = fb + (size_t) px * DASH_FB_H + (DASH_FB_H - 1);
        for (dy = 0; dy < dh; dy++) {
            const int py = y + dy;
            const unsigned short p = src[syTab[dy] * g->w];
            if ((p & 1u) == 0u || py < 0 || py >= DASH_FB_H) {
                continue; /* alpha=0, or off screen */
            }
            col[-py] = Rgba5551To565(p);
        }
    }
}

static const DashGlyph* DashFindGlyph(const DashGlyph* table, int count, const char* key) {
    int i;
    for (i = 0; i < count; i++) {
        if (strcmp(table[i].key, key) == 0) {
            return &table[i];
        }
    }
    return NULL;
}

/* Width of one glyph (its real, proportional `adv`, not its raw texture width) at num/den. */
static int DashGlyphAdvanceRatio(const DashGlyph* g, int num, int den) {
    return (g->adv * num) / den;
}

/* Font1/Font2 are the only tables with multi-glyph TEXT walked via DashFindGlyph per character
 * (one-char keys, plus a no-glyph space advance); the other tables (timer/lap/position digits,
 * suffixes) are always looked up a single glyph at a time by their own call sites below, since
 * each one is a distinct symbol/digit/suffix composited at its own hand-placed position, not a
 * run of prose. `space` is DASH_FONT1_SPACE or DASH_FONT2_SPACE, `gap` a small extra kerning
 * pixel between glyphs (at num/den, like everything else here) so characters do not touch. */
static int DashFontTextWidthRatio(const DashGlyph* table, int count, const char* s, int num, int den,
                                   int space, int gap) {
    int w = 0;
    int n = 0;
    for (; *s != '\0'; s++) {
        if (*s == ' ') {
            w += (space * num) / den;
        } else {
            char key[2] = {*s, '\0'};
            const DashGlyph* g = DashFindGlyph(table, count, key);
            if (g != NULL) {
                w += DashGlyphAdvanceRatio(g, num, den);
            }
        }
        if (n > 0) {
            w += (gap * num) / den;
        }
        n++;
    }
    return w;
}

static void DashBlitFontText(uint16_t* fb, int x, int y, const DashGlyph* table, int count, const char* s,
                              int num, int den, int space, int gap) {
    int cx = x;
    int n = 0;
    for (; *s != '\0'; s++) {
        if (n > 0) {
            cx += (gap * num) / den;
        }
        if (*s == ' ') {
            cx += (space * num) / den;
        } else {
            char key[2] = {*s, '\0'};
            const DashGlyph* g = DashFindGlyph(table, count, key);
            if (g != NULL) {
                DashBlitGlyphRatio(fb, cx, y, g, num, den);
                cx += DashGlyphAdvanceRatio(g, num, den);
            }
        }
        n++;
    }
}

/* Largest num in {2,1} (den fixed at 1) whose rendered Font2 string fits maxW -- the MODE chip
 * and the right column's boxes are narrow and their content length varies tick to tick (a
 * >=1-minute clock value runs longer; GAP can show a sign). */
static int DashFitFont2Num(const char* s, int maxW, int gap) {
    int num;
    for (num = 2; num >= 1; num--) {
        if (DashFontTextWidthRatio(kDashGlyphFont2, KDASHGLYPHFONT2_COUNT, s, num, 1, DASH_FONT2_SPACE, gap) <= maxW) {
            return num;
        }
    }
    return 1;
}

/* The baked backdrop+chrome composite (gdx3ds_hudbake.h): the opaque background first, then
 * every other widget alpha-composited over it, in the template manifest's own order. Drawn ONCE PER RACE — see sWasValid's 0->1 edge in gdx3ds_bottomhud_tick.
 *
 * COORDINATOR FIX (nibottom Phase 2, post-mockup-shot review): "portrait" and "mode_button"
 * were baked FLATTENED PNGs of the template's own frozen example content -- "4TH"/"04th" baked
 * into the portrait ribbon, "MODE 1" baked across the whole mode_button box (a flattened export,
 * not layered). They are no longer baked at all (gdx3ds_hudbake.c has no recipe for them).
 * Both are also fields this file draws LIVE every tick (the player's real position, the real
 * placement mode name) immediately on top, so baking them too meant drawing the live value
 * literally on top of a different, frozen value in the same cells -- the "stray '04'/garbled
 * text over the our-position block" and "MODE chip over ALL TOP" the coordinator flagged from
 * nibottom-shots/mutecity-start_m01_3297_bottom.png. Skipping them here (the checker background
 * still shows through, same as any other unbaked area) and drawing only the live text is the
 * fix -- not a width tweak, since the baked content fills each box edge to edge. LAP/TIME/BEST/
 * GAP/BOOST keep their bake (label + frozen example number, e.g. "TIME 0'42"18""): the matching
 * fix there was the opposite one at the time -- stop drawing live text over them, since "per
 * frame" live digits was deferred past that pass in favour of a single consistent game-font
 * look. (Later rounds then DID wire up live digits -- DashPaintRightColumnLap/Timer/Gap, below --
 * and BOOST itself is gone outright as of the hardware feedback round; see DashPaintKOStars.) */
/* Per-cell skip test for the widgets that carry a LIVE field in among otherwise-static baked
 * content, so DashPaintBaked leaves those cells alone (checker shows through) and a live paint
 * draws the real value there every tick / once per race. Row ranges for lap/timer/gap found by
 * scanning the baked lap/timer/gap widgets directly for their actual label-vs-value content bands
 * (label always starts at row 0).
 *
 * COORDINATOR FIX (third visual-feedback round, item 2): "the header says 'MUTE CITY !' on the
 * gp2-sector replay. It should be SECTOR... read the actual course, not a constant." The baked
 * "header" widget's course-name text occupies cols ~6-76 of its 320px width (confirmed by direct
 * decode, same scan that found the free MENU-button gap in the second round) -- col < 150 skips
 * that whole zone with a comfortable margin short of the MODE chip's own already-unbaked rect
 * (168+). DashPaintHeaderLive (below) draws the real name there with real Font1 glyphs.
 *
 * COORDINATOR FIX (hardware feedback round, item 5): "'30 RACERS' [is] currently baked/frozen
 * text; make it live." Direct decode of the baked header widget found its "<N> RACERS" glyph pixels at
 * cols 255-313 on a flat cols-230-319 panel (nothing else lives right of the MODE chip's own 230
 * right edge) -- col >= 230 skips that whole panel, DashPaintHeaderLive's racer-count half draws
 * the live "<N> RACERS" there.
 *
 * COORDINATOR FIX (hardware feedback round, item 1): "the energy bar... has 'overflow' cut off
 * on top." Root cause: the baked energy_bar widget is a FLATTENED EXAMPLE export (same "frozen PNG of
 * whatever was on screen" issue "portrait"/"mode_button" already had) -- its baked pixels past
 * the "ENERGY" label (cols 7-53) are a frozen fill-bar screenshot, not a reusable empty frame, and
 * DashPaintEnergyFooter's own live fill only ever repaints rows 4..19 of the 24-tall box, leaving
 * a few rows of that frozen fill peeking out above/below it every tick -- the "overflow". col >=
 * 54 skips the whole frame/fill zone (keeping just the "ENERGY" label baked); DashPaintEnergyFrame
 * (below) draws a flat procedural panel + border there instead, same DASH-chrome style as the
 * MODE/MENU/KO-star/speed chips, with no frozen content left to bleed through.
 *
 * COORDINATOR FIX (hardware feedback round, item 2): "standings portraits... don't update based
 * on the racers/position. They're static." The standings widget's own baked art only fills cols
 * 0..28 of its 64px width (portrait + its own tiny baked ordinal ribbon, confirmed by direct
 * decode) -- col <= 28 skips that whole zone (all 5 rows at once: they share the same column
 * range), DashPaintStandingsPortraits (below) draws the real, live, per-row character portrait. */
static int DashBakeSkipCell(const char* id, int row, int col) {
    if (strcmp(id, "lap") == 0) {
        return row >= 12;
    }
    if (strcmp(id, "timer") == 0) {
        return (row >= 18 && row < 38) || row >= 52; /* TIME value, then BEST value */
    }
    if (strcmp(id, "gap") == 0) {
        return row >= 12;
    }
    if (strcmp(id, "header") == 0) {
        /* uipolish: the frozen text only -- the panel's top and left edge lines stay, and the
         * skipped interior is painted in the panel's own colour (DashCompositeAssetWidget), so the
         * bar runs the whole width under the course name and the SETTINGS button (hw: "the mode
         * button has a background bar that doesn't stretch all the way across the screen") */
        return row >= 1 && col >= 1 && (col < 150 || col >= 230);
    }
    if (strcmp(id, "energy_bar") == 0) {
        return col >= 54;
    }
    if (strcmp(id, "standings") == 0) {
        return col <= 28;
    }
    return 0;
}

/* COORDINATOR FIX (second round item 1 / third round item 1): "the rank text in the top-5
 * column... is too small" -> "replace [the hand font] with the real glyph atlases... the
 * position digits and ST/ND/RD/TH textures for the ranks." The standings widget's own baked art
 * only fills cols 0..28 of its 64px width (portrait + its own tiny baked ordinal ribbon,
 * confirmed by direct decode) -- cols 29..63 are blank in every row band, so the real
 * aPosition<N>Tex digit + aPositionOrdinalSuffix<X>Tex suffix go there (num=1,den=2: the 28x32 /
 * 20x20 native textures halved, 14x16 + 10x20 side by side, comfortably inside the 34x29 free
 * cell) without touching the baked art. Static per race (row->rank never changes), bakes once. */
static void DashPaintStandingsRanks(uint16_t* fb) {
    static const char* const kSuffixKey[GDX_RACEHUD_TOP5_MAX] = {"ST", "ND", "RD", "TH", "TH"};
    int i;
    if (!sDashWidgets[DASHW_STANDINGS].enabled) {
        return;
    }
    for (i = 0; i < GDX_RACEHUD_TOP5_MAX; i++) {
        char digitKey[2] = {(char) ('1' + i), '\0'};
        const DashGlyph* digit = DashFindGlyph(kDashGlyphPosDigit, KDASHGLYPHPOSDIGIT_COUNT, digitKey);
        const DashGlyph* suffix = DashFindGlyph(kDashGlyphPosSuffix, KDASHGLYPHPOSSUFFIX_COUNT, kSuffixKey[i]);
        const int y0 = DASH_STANDINGS_Y0 + i * DASH_STANDINGS_ROW_H;
        const int freeX0 = DASH_STANDINGS_X0 + 29;
        int x = freeX0 + 1;
        if (digit == NULL || suffix == NULL) {
            continue;
        }
        DashBlitGlyphRatio(fb, x, y0 + (DASH_STANDINGS_ROW_H - digit->h / 2) / 2, digit, 1, 2);
        x += digit->w / 2 + 1;
        DashBlitGlyphRatio(fb, x, y0 + (DASH_STANDINGS_ROW_H - suffix->h / 2) / 2, suffix, 1, 2);
    }
}

/* COORDINATOR FIX (second round item 4 / third round item 1): the "return to the debug menu"
 * button, styled like the MODE chip (same panel fill, same pink border), labelled with the real
 * Font2 glyphs (not libctru console text, not the retired hand font). Static chrome, baked once;
 * gdx3ds_bottomhud_tick hit-tests the same DASH_BACK_* rect every tick independently of this.
 * Takes `snap` (unused) only to match sDashWidgets' uniform paint-fn signature -- see the
 * geometry-table comment at the top of this file for why gdx3ds_bottomhud_tick still calls this
 * directly (once, at backdrop-bake time) rather than through that table's own `paint` column.
 *
 * COORDINATOR FIX (idle-view round): renamed "MENU" -> "DEBUG" -- a clearer name for a button
 * that opens the text-console DEBUG UI, not a game menu. Shared, unchanged, by the race DASH
 * (gdx3ds_bottomhud_tick's own dismiss handling), CUSTOM mode (gdx3ds_bottomhud_tick's
 * once-per-entry CUSTOM backdrop), and the new idle view (DashPaintIdleBaked) -- the label change
 * and every behaviour below apply identically to all three; only the one literal changed.
 *
 * uipolish: "SETTINGS" now (Home's DEBUG button is gone; the console it opens is the SETTINGS
 * pages plus the developer tabs, left with the yellow EXIT tab), 8 px wider so the label clears
 * the border. */
static void DashPaintBackButton(uint16_t* fb, const GdxRaceHudSnapshot* snap) {
    static const char kLabel[] = "SETTINGS";
    const int num = 1;
    int tw;
    int x;
    int y;
    (void) snap;
    if (!sDashWidgets[DASHW_MENU_BUTTON].enabled || sDebugHidden) {
        return;
    }
    tw = DashFontTextWidthRatio(kDashGlyphFont2, KDASHGLYPHFONT2_COUNT, kLabel, num, 1, DASH_FONT2_SPACE, 1);
    x = DASH_BACK_X0 + (DASH_BACK_W - tw) / 2;
    y = DASH_BACK_Y0 + (DASH_BACK_H - (8 * num)) / 2;
    DashFillRect(fb, DASH_BACK_X0, DASH_BACK_Y0, DASH_BACK_W, DASH_BACK_H, 0x3885u);
    DashStrokeRect(fb, DASH_BACK_X0, DASH_BACK_Y0, DASH_BACK_W, DASH_BACK_H, 0xFB76u);
    DashBlitFontText(fb, x, y, kDashGlyphFont2, KDASHGLYPHFONT2_COUNT, kLabel, num, 1, DASH_FONT2_SPACE, 1);
}

/* COORDINATOR FIX (hardware feedback round, item 1): the energy bar's own flat procedural panel
 * + border, replacing the frozen/overflowing baked fill-bar art DashBakeSkipCell's "energy_bar"
 * case (col >= 54) now skips entirely -- see that function's own comment for the root cause. Same
 * DASH-chrome look every other procedural widget here uses (fill 0x3885u, border 0xFB76u), baked
 * once per race; DashPaintEnergyFooter's live fill (every tick, unchanged) draws on top of this. */
static void DashPaintEnergyFrame(uint16_t* fb) {
    if (!sDashWidgets[DASHW_ENERGY_BAR].enabled) {
        return;
    }
    DashFillRect(fb, DASH_ENERGY_X0 + 54, DASH_ENERGY_Y0 + 1, DASH_ENERGY_W - 54 - 1, DASH_ENERGY_H - 2, 0x3885u);
    DashStrokeRect(fb, DASH_ENERGY_X0 + 54, DASH_ENERGY_Y0, DASH_ENERGY_W - 54, DASH_ENERGY_H, 0xFB76u);
}

/* COORDINATOR FIX (third round item 2 / hardware feedback round item 5): the header's two live
 * fields -- the course name (was baked, now reads the real track) and the "<N> RACERS" readout
 * (was frozen baked-template text, item 5: "make it live... current racers remaining... if
 * cheap") -- as ONE self-contained, dirty-checked widget function, so it can sit in sDashWidgets'
 * table as a single per-tick paint fn instead of a once-per-race side call. Cheap on every tick
 * that changes nothing: two cache compares, no drawing. num=1,den=2 for the course name (Font1 is
 * a 16x16 native cell; the original baked art's own text sat at roughly this scale); Font2 at the
 * best-fit num (2 or 1) for the racer count, right-aligned in the now-unbaked cols 230+. */
/* uipolish: the header's live track name must stay left of whatever sits next in the bar (the
 * SETTINGS button, MP's STANDINGS button, or the MODE chip when neither is shown). Font1 at half
 * scale first; a long name ("DEVIL'S FOREST") drops to Font2, and anything still too wide is cut
 * at the last whole character that fits rather than running under the button. */
static int DashHeaderNameRight(void) {
    const int buttonShown = sDashWidgets[DASHW_MENU_BUTTON].enabled && (!sDebugHidden || sMpDash);
    return (buttonShown ? DASH_BACK_X0 : DASH_MODE_X0) - 4;
}

static void DashPaintHeaderName(uint16_t* fb, const char* name) {
    const int x0 = 6;
    const int maxW = DashHeaderNameRight() - x0;
    char fit[sizeof(sLastHeaderTrackName)];
    size_t len;
    DashFillRect(fb, 1, DASH_HEADER_Y0 + 1, DashHeaderNameRight() + 2, DASH_HEADER_H - 2, sDashHeaderFill);
    if (name[0] == '\0') {
        return;
    }
    if (DashFontTextWidthRatio(kDashGlyphFont1, KDASHGLYPHFONT1_COUNT, name, 1, 2, DASH_FONT1_SPACE, 1) <= maxW) {
        DashBlitFontText(fb, x0, DASH_HEADER_Y0 + (DASH_HEADER_H - 8) / 2, kDashGlyphFont1, KDASHGLYPHFONT1_COUNT,
                         name, 1, 2, DASH_FONT1_SPACE, 1);
        return;
    }
    snprintf(fit, sizeof(fit), "%s", name);
    len = strlen(fit);
    while (len > 1 && DashFontTextWidthRatio(kDashGlyphFont2, KDASHGLYPHFONT2_COUNT, fit, 1, 1, DASH_FONT2_SPACE, 1) > maxW) {
        fit[--len] = '\0';
    }
    if (len < strlen(name) && strchr(fit, ' ') != NULL) {
        *strrchr(fit, ' ') = '\0'; /* cut at a word boundary: "DEVIL'S", never "DEVIL'S FO" */
    }
    DashBlitFontText(fb, x0, DASH_HEADER_Y0 + (DASH_HEADER_H - 8) / 2, kDashGlyphFont2, KDASHGLYPHFONT2_COUNT, fit,
                     1, 1, DASH_FONT2_SPACE, 1);
}

static void DashPaintHeaderLive(uint16_t* fb, const GdxRaceHudSnapshot* snap) {
    if (!sDashWidgets[DASHW_HEADER].enabled) {
        return;
    }
    if (strcmp(sLastHeaderTrackName, snap->trackName) != 0) {
        snprintf(sLastHeaderTrackName, sizeof(sLastHeaderTrackName), "%s", snap->trackName);
        DashPaintHeaderName(fb, sLastHeaderTrackName);
    }
    if (snap->racersRemaining != sLastHeaderRacersRemaining && !sMpDash) {
        char buf[16];
        const int maxW = DASH_HEADER_X0 + DASH_HEADER_W - 234; /* cols 230..316: DashBakeSkipCell's skip zone */
        int num;
        int tw;
        int x;
        int y;
        sLastHeaderRacersRemaining = snap->racersRemaining;
        snprintf(buf, sizeof(buf), snap->racersRemaining == 1 ? "%d RACER" : "%d RACERS", snap->racersRemaining);
        num = DashFitFont2Num(buf, maxW, 1);
        tw = DashFontTextWidthRatio(kDashGlyphFont2, KDASHGLYPHFONT2_COUNT, buf, num, 1, DASH_FONT2_SPACE, 1);
        x = DASH_HEADER_X0 + DASH_HEADER_W - 4 - tw;
        y = DASH_HEADER_Y0 + (DASH_HEADER_H - (8 * num)) / 2;
        DashFillRect(fb, 230, DASH_HEADER_Y0 + 1, DASH_HEADER_W - 230 - 1, DASH_HEADER_H - 2, sDashHeaderFill);
        DashBlitFontText(fb, x, y, kDashGlyphFont2, KDASHGLYPHFONT2_COUNT, buf, num, 1, DASH_FONT2_SPACE, 1);
    }
}

/* The dimmed checkerboard, alone -- factored out of DashPaintBaked so the idle view
 * (DashPaintIdleBaked, below) can reuse the SAME 150 KB background bake instead of baking a
 * second full-320x240 canvas of its own (nibottom-progress.md's own Old-3DS memory note: a
 * second full-screen bake would cost another 150 KB RAM, which this reuse avoids entirely). Pure
 * extraction, no behaviour change -- DashPaintBaked's own call site below is byte-identical to
 * its pre-refactor body. */
static void DashPaintBackdropBackground(uint16_t* fb) {
    int i;
    DashMarkCols(0, DASH_FB_W);
    for (i = 0; i < 76800; i++) {
        const int x = i % DASH_FB_W;
        const int y = i / DASH_FB_W;
        DashPixel(fb, x, y, DashDimBackground(gDashAssetBackground != NULL ? gDashAssetBackground[i] : 0u));
    }
}

/* Composites one kDashAssetWidgets entry over whatever is already in `fb` (background or an
 * earlier widget), skipping its own live-text cells (DashBakeSkipCell) and transparent texels --
 * factored out of DashPaintBaked's own per-widget loop so DashPaintIdleHeaderChrome (below) can
 * composite just the "header" widget alone for the idle view, with the exact same skip-cell and
 * alpha rules the race DASH's full bake already uses. */
/* Where a baked chrome piece goes: its sDashWidgets row's origin when it has one (card-4b moved
 * LAP/TIME/GAP down; the others sit exactly where the asset table says), else the asset's own. */
static void DashAssetOrigin(const DashAssetWidget* w, int* x, int* y) {
    int i;
    *x = w->x;
    *y = w->y;
    for (i = 0; i < DASHW_COUNT; i++) {
        if (strcmp(sDashWidgets[i].id, w->id) == 0) {
            *x = sDashWidgets[i].x;
            *y = sDashWidgets[i].y;
            return;
        }
    }
}

/* The header panel's own interior colour (its art is a flat panel with a top and left edge line),
 * for the frozen-text cells DashBakeSkipCell leaves to the live paint. */
#define DASH_HEADER_FILL 0x2043u

static uint16_t DashHeaderFill(const DashAssetWidget* w) {
    const unsigned short p = (w->w > 160 && w->h > 1) ? w->pixels[w->w + 160] : 0u;
    return (p & 1u) ? Rgba5551To565(p) : DASH_HEADER_FILL;
}

static void DashCompositeAssetWidget(uint16_t* fb, const DashAssetWidget* w) {
    const int isHeader = strcmp(w->id, "header") == 0;
    uint16_t headerFill;
    int row;
    int ox;
    int oy;
    if (w->pixels == NULL) {
        return; /* not baked (no memory for the arena): nothing to composite */
    }
    headerFill = isHeader ? DashHeaderFill(w) : 0u;
    if (isHeader) {
        sDashHeaderFill = headerFill;
    }
    DashAssetOrigin(w, &ox, &oy);
    DashMarkCols(ox, w->w);
    for (row = 0; row < w->h; row++) {
        int col;
        for (col = 0; col < w->w; col++) {
            const unsigned short p = w->pixels[row * w->w + col];
            if (DashBakeSkipCell(w->id, row, col)) {
                if (isHeader) {
                    DashPixel(fb, ox + col, oy + row, headerFill); /* the bar, without its frozen text */
                }
                continue; /* live-text-only cell within an otherwise-baked widget */
            }
            if ((p & 1u) == 0u) {
                continue; /* alpha=0: transparent, leave the layer below as-is */
            }
            DashPixel(fb, ox + col, oy + row, Rgba5551To565(p));
        }
    }
}

/* ---- uipolish: the stats box (LAP / TIME / BEST / GAP) --------------------------------------
 *
 * One framed box over the lap, timer and gap rows. Its labels are the baked LAP/TIME/GAP art's
 * label bands alone (the frozen example values under them are never drawn); each live value goes
 * in its own row under its label. Rows, from the box's top (frame at 0): 3 px pad, label, 2 px,
 * value, 5 px between sections, 2 px pad at the bottom. Values start 4 px in from the frame. */
#define DASH_STATS_FILL 0x1041u
#define DASH_STATS_BORDER 0xFB76u
#define DASH_STATS_TEXT_X (DASH_STATS_X0 + 4)
#define DASH_STATS_LAP_LABEL_Y (DASH_STATS_Y0 + 4)    /* art rows 2..11 -> ink 77..86 */
#define DASH_STATS_LAP_VALUE_Y (DASH_STATS_Y0 + 16)   /* lap glyphs, ink rows 0..10 */
#define DASH_STATS_TIME_LABEL_Y (DASH_STATS_Y0 + 32)  /* art rows 4..14 */
#define DASH_STATS_TIME_VALUE_Y (DASH_STATS_Y0 + 44)  /* timer glyphs, ink rows 1..14 */
#define DASH_STATS_BEST_LABEL_Y (DASH_STATS_Y0 + 64)  /* art rows 40..49 */
#define DASH_STATS_BEST_VALUE_Y (DASH_STATS_Y0 + 75)
#define DASH_STATS_GAP_LABEL_Y (DASH_STATS_Y0 + 95)   /* Font2, ink rows -1..5 */
#define DASH_STATS_GAP_VALUE_Y (DASH_STATS_Y0 + 102)

static const DashAssetWidget* DashAssetFind(const char* id) {
    int i;
    for (i = 0; i < DASH_ASSET_WIDGET_COUNT; i++) {
        if (strcmp(kDashAssetWidgets[i].id, id) == 0) {
            return &kDashAssetWidgets[i];
        }
    }
    return NULL;
}

/* Art rows [r0, r1) of a baked widget, opaque texels only, with art row r0 / col 0 at (x, y). */
static void DashCompositeAssetRows(uint16_t* fb, const char* id, int r0, int r1, int x, int y) {
    const DashAssetWidget* w = DashAssetFind(id);
    int row;
    if (w == NULL || w->pixels == NULL) {
        return;
    }
    DashMarkCols(x, w->w);
    for (row = r0; row < r1 && row < w->h; row++) {
        int col;
        for (col = 0; col < w->w; col++) {
            const unsigned short p = w->pixels[row * w->w + col];
            if (p & 1u) {
                DashPixel(fb, x + col, y + row - r0, Rgba5551To565(p));
            }
        }
    }
}

/* A value row of the stats box: cleared to the box's fill, frame untouched. */
static void DashStatsClearRow(uint16_t* fb, int y, int h) {
    DashFillRect(fb, DASH_STATS_X0 + 2, y, DASH_STATS_W - 4, h, DASH_STATS_FILL);
}

static void DashPaintStatsBox(uint16_t* fb) {
    const int artX = DASH_STATS_TEXT_X - 2; /* every label's ink starts at art col 2 */
    if (!sDashWidgets[DASHW_LAP].enabled || !sDashWidgets[DASHW_TIMER].enabled || !sDashWidgets[DASHW_GAP].enabled) {
        return;
    }
    DashFillRect(fb, DASH_STATS_X0 + 1, DASH_STATS_Y0 + 1, DASH_STATS_W - 2, DASH_STATS_H - 2, DASH_STATS_FILL);
    DashStrokeRect(fb, DASH_STATS_X0, DASH_STATS_Y0, DASH_STATS_W, DASH_STATS_H, DASH_STATS_BORDER);
    DashCompositeAssetRows(fb, "lap", 2, 12, artX, DASH_STATS_LAP_LABEL_Y);
    DashCompositeAssetRows(fb, "timer", 4, 15, artX, DASH_STATS_TIME_LABEL_Y);
    DashCompositeAssetRows(fb, "timer", 40, 50, artX, DASH_STATS_BEST_LABEL_Y);
    /* GAP: the baked label's tiny font reads as "CAP" (hw: "time/best/cap"), so Font2 spells it,
     * its 7 ink rows ending on the art label's last row */
    DashBlitFontText(fb, DASH_STATS_TEXT_X - 1, DASH_STATS_GAP_LABEL_Y - 1, kDashGlyphFont2, KDASHGLYPHFONT2_COUNT,
                      "GAP", 1, 1, DASH_FONT2_SPACE, 1);
}

/* The once-per-race backdrop+chrome composite -- full rationale in the big comment block above
 * DashBakeSkipCell (this function used to sit directly below it; DashPaintBackdropBackground/
 * DashCompositeAssetWidget, extracted just above for the idle view to share, now sit between). */
static void DashPaintBaked(uint16_t* fb) {
    int i;
    DashMemoReset();
    sDirtyFull = 1;
    DashPaintBackdropBackground(fb);
    for (i = 0; i < DASH_ASSET_WIDGET_COUNT; i++) {
        const DashAssetWidget* w = &kDashAssetWidgets[i];
        if (strcmp(w->id, "portrait") == 0 || strcmp(w->id, "mode_button") == 0) {
            continue; /* live-text-only boxes -- see the comment above */
        }
        if (strcmp(w->id, "lap") == 0 || strcmp(w->id, "timer") == 0 || strcmp(w->id, "gap") == 0) {
            continue; /* their labels go in the stats box (DashPaintStatsBox) */
        }
        DashCompositeAssetWidget(fb, w);
    }
    DashPaintStatsBox(fb);
    DashPaintStandingsRanks(fb);
    DashPaintBackButton(fb, NULL);
    DashPaintEnergyFrame(fb);
    DashStrokeRect(fb, DASH_MODE_X0, DASH_MODE_Y0, DASH_MODE_W, DASH_MODE_H, 0xFB76u);
}

/* ---- IDLE VIEW: the bottom screen's default look outside a live race -------------------
 *
 * The user's own ask: "we shouldn't just have the bottom screen only show a polished experience
 * during race... have the checkerboard always there, with the bottom 'menu' option there and
 * maybe just a g-diffuser logo." Reuses the SAME checkerboard bake the race DASH uses
 * (DashPaintBackdropBackground -- no second 150 KB full-screen image), the SAME baked "header"
 * chrome panel (DashCompositeAssetWidget, header only -- no race-only lap/timer/gap/standings/
 * energy_bar chrome, which would bake frozen EXAMPLE race numbers that have no meaning outside a
 * race), the real G-DIFFUSER wordmark (tools/gen_nibottom_idle_assets.py's small bake, the same
 * logo file the desktop port's own title menu already uses), and the SAME DEBUG button chrome
 * (DashPaintBackButton -- see DashPaintBackButton's own kLabel for why "DEBUG" now serves both
 * this view and the race DASH). Composited ONCE per idle-view entry (gdx3ds_bottomhud_idle_tick's
 * own sIdleBaked latch) -- genuinely static, unlike the race DASH's own per-tick live fields. */

/* Just the "header" widget's baked chrome, skipping its own live-text zones exactly as the race
 * DASH does (DashBakeSkipCell) -- those zones are left as checkerboard here too, since the idle
 * view has no course name or racer count to show. */
static void DashPaintIdleHeaderChrome(uint16_t* fb) {
    int i;
    for (i = 0; i < DASH_ASSET_WIDGET_COUNT; i++) {
        const DashAssetWidget* w = &kDashAssetWidgets[i];
        if (strcmp(w->id, "header") != 0) {
            continue;
        }
        DashCompositeAssetWidget(fb, w);
        return;
    }
}

/* Centres the baked G-DIFFUSER wordmark (gdx3ds_bottomhud_idle_assets.h, DASH_IDLE_LOGO_W/H)
 * over the free area above the header band -- num=1,den=1 since the generator already bakes it
 * at its final on-screen size, so DashBlitGlyphRatio does a plain alpha-composited copy here. */
static void DashPaintIdleLogo(uint16_t* fb) {
    static const DashGlyph kIdleLogo = {"LOGO", DASH_IDLE_LOGO_W, DASH_IDLE_LOGO_H, 0, kDashIdleLogoPixels};
    const int x = (DASH_FB_W - DASH_IDLE_LOGO_W) / 2;
    const int y = (DASH_HEADER_Y0 - DASH_IDLE_LOGO_H) / 2;
    DashBlitGlyphRatio(fb, x, y, &kIdleLogo, 1, 1);
}

/* The idle view's own once-per-entry backdrop: background, header chrome, logo, DEBUG button --
 * in that paint order (background first, everything else composited on top), mirroring
 * DashPaintBaked's own layering. */
static void DashPaintIdleBaked(uint16_t* fb) {
    DashPaintBackdropBackground(fb);
    DashPaintIdleHeaderChrome(fb);
    DashPaintIdleLogo(fb);
    DashPaintBackButton(fb, NULL);
}

/* Small colour tag for a standings row, tinted by character id -- kept alongside the real
 * portrait (DashPaintStandingsPortraits, below) as a cheap at-a-glance colour cue; harmless and
 * already working, so there was no reason to remove it when the portrait itself went live. */
static uint16_t DashCharacterColour(int character) {
    static const uint16_t kPalette[8] = {
        0xF800u, 0x07E0u, 0x001Fu, 0xFFE0u, 0xF81Fu, 0x07FFu, 0xFD20u, 0xFFFFu,
    };
    if (character < 0) {
        return 0x4208u;
    }
    return kPalette[(unsigned) character % 8u];
}

static void DashPaintTop5Tags(uint16_t* fb, const GdxRaceHudSnapshot* snap) {
    int i;
    for (i = 0; i < GDX_RACEHUD_TOP5_MAX; i++) {
        const int y0 = DASH_STANDINGS_Y0 + i * DASH_STANDINGS_ROW_H + 2;
        const int haveEntry = i < snap->top5Count;
        const uint16_t colour = haveEntry ? DashCharacterColour(snap->top5Character[i]) : 0x0000u;
        DashFillRect(fb, DASH_STANDINGS_X0 + DASH_STANDINGS_W - 8, y0, 6, 6, colour);
        if (haveEntry && snap->position == (i + 1)) {
            DashStrokeRect(fb, DASH_STANDINGS_X0 + 1, DASH_STANDINGS_Y0 + i * DASH_STANDINGS_ROW_H + 1,
                           DASH_STANDINGS_W - 2, DASH_STANDINGS_ROW_H - 2, 0xFFFFu);
        }
    }
    sLastTop5Count = snap->top5Count;
    for (i = 0; i < GDX_RACEHUD_TOP5_MAX; i++) {
        sLastTop5[i] = (i < snap->top5Count) ? snap->top5Character[i] : -1;
    }
}

static int Top5Changed(const GdxRaceHudSnapshot* snap) {
    int i;
    if (snap->top5Count != sLastTop5Count) {
        return 1;
    }
    for (i = 0; i < snap->top5Count; i++) {
        if (snap->top5Character[i] != sLastTop5[i]) {
            return 1;
        }
    }
    return 0;
}

/* COORDINATOR FIX (hardware feedback round, item 2): "these portrait elements don't update based
 * on the racers/position... they're static and essentially useless... they don't follow the
 * sizing of the original HUD, where the top 2 racers have bigger portraits." Source of the
 * ORIGINAL per-racer portrait and its sizing rule: decomp/src/overlays/ovl_i3/hud.c's
 * gCharacterPortraitTextures[] (one real 32x32 texture per character) and Hud_DrawRacePortraits
 * (i<3 draws at scale 1.0/32x32, i>=3 at 0.75/24x24 on the source's own 6-row readout). Baked at boot
 * into gdx3ds_hudbake.h's kDashGlyphPortrait table (same real archive
 * textures, keyed by the Racer::character index as a decimal string, looked up with the same
 * DashFindGlyph every other glyph table here already uses) -- not a swatch, not frozen art, the
 * REAL portrait for whichever racer actually holds that position this tick. This box has 5 rows,
 * not 6; the split is 2 big / 3 small, the closest analogue a 5-row readout has to the source's
 * 3-big/3-small split, and matches the user's own framing ("the top 2 racers have bigger
 * portraits"). Drawn BEFORE DashPaintTop5Tags (above) so that function's own white
 * highlight-border stroke -- which runs the full row width, including this portrait's own column
 * -- stays on top and visible, not erased by a later portrait repaint. */
static void DashPaintStandingsPortraits(uint16_t* fb, const GdxRaceHudSnapshot* snap) {
    const int cellW = 29; /* DashBakeSkipCell's "standings" col <= 28 skip zone */
    int i;
    for (i = 0; i < GDX_RACEHUD_TOP5_MAX; i++) {
        const int y0 = DASH_STANDINGS_Y0 + i * DASH_STANDINGS_ROW_H;
        DashFillRect(fb, DASH_STANDINGS_X0, y0, cellW, DASH_STANDINGS_ROW_H, 0x1041u);
        if (i < snap->top5Count) {
            char key[4];
            const DashGlyph* portrait;
            const int num = (i < 2) ? 7 : 5; /* top 2 rows: 28x28; rows 3-5: 20x20 */
            const int den = 8;
            snprintf(key, sizeof(key), "%d", snap->top5Character[i]);
            portrait = DashFindGlyph(kDashGlyphPortrait, KDASHGLYPHPORTRAIT_COUNT, key);
            if (portrait != NULL) {
                const int w = (portrait->w * num) / den;
                const int h = (portrait->h * num) / den;
                const int x = DASH_STANDINGS_X0 + (cellW - w) / 2;
                const int y = y0 + (DASH_STANDINGS_ROW_H - h) / 2;
                DashBlitGlyphRatio(fb, x, y, portrait, num, den);
            }
        }
    }
}

/* The standings box's own per-tick paint fn (sDashWidgets' "standings" row): dirty-checked
 * against Top5Changed, same cadence the rank badges/colour swatches already used, now also
 * covering the live portraits above -- all three stay in sync since they only ever repaint
 * together, on the same edge. */
static void DashPaintStandingsLive(uint16_t* fb, const GdxRaceHudSnapshot* snap) {
    if (!sDashWidgets[DASHW_STANDINGS].enabled) {
        return;
    }
    if (!Top5Changed(snap)) {
        return;
    }
    DashPaintStandingsPortraits(fb, snap);
    DashPaintTop5Tags(fb, snap); /* also refreshes sLastTop5/sLastTop5Count */
    /* [ui] dash_memo: the standings' top row (y 67) is the OUR-position box's bottom row, which
     * the position numeral (painted next) owns; it must repaint after this one. */
    sMemoValid[DASHW_PORTRAIT] = 0;
}

/* COORDINATOR FIX (hw bug n60-3d7, item 6): "the player and rival markers are missing, or show
 * as black dots." Root cause: DashPaintMapBox already called gdx_racemap_snapshot() (for its
 * `.valid` gate) but never drew `.markers[]` -- the snapshot's marker list was sitting right
 * there, unused, exactly the "future work" gap this comment used to name. gdx3ds_racemap.c's own
 * debug MAP tab (BmapMarker + kMarkerRgba5551) already proves the marker DATA and COLOURS are
 * correct (kMarkerRgba5551 below is byte-identical to decomp's own MINIMAP_*_COLOR constants,
 * confirmed against decomp/src/overlays/ovl_i3/minimap.h) -- this was never a wrong-colour bug,
 * it was markers never being drawn on the DASH's own map_box at all, which is exactly what reads
 * as "missing". (MINIMAP_GHOST_COLOR is legitimately black in the real game -- a ghost racer's
 * own marker -- not a bug; it only ever appears in time-attack/time-trial mode.) */
static const unsigned short kDashMarkerRgba5551[6] = {
    (unsigned short) ((0u << 11) | (31u << 6) | (31u << 1) | 1u),  /* PLAYER1 cyan    */
    (unsigned short) ((31u << 11) | (31u << 6) | (0u << 1) | 1u),  /* PLAYER2 yellow  */
    (unsigned short) ((0u << 11) | (31u << 6) | (0u << 1) | 1u),   /* PLAYER3 green   */
    (unsigned short) ((31u << 11) | (15u << 6) | (31u << 1) | 1u), /* PLAYER4 magenta */
    (unsigned short) ((0u << 11) | (0u << 6) | (31u << 1) | 1u),   /* LEADER blue     */
    (unsigned short) ((0u << 11) | (0u << 6) | (0u << 1) | 1u),    /* GHOST black     */
};

/* Clamps v into [lo,hi] -- small single-purpose helper, used below to keep the map's
 * destination-pixel-driven scale (DashPaintMapMarker/DashPaintMapBox) safe at the box edges now
 * that the scale is a plain ratio (DASH_MAP_PX/DASH_MAP_SRC_DIM) rather than a fixed integer. */
static int DashClampInt(int v, int lo, int hi) {
    if (v < lo) {
        return lo;
    } else if (v > hi) {
        return hi;
    } else {
        return v;
    }
}

/* One 3x3-source-pixel marker, mapped to its own destination-pixel rect at the box's current
 * DASH_MAP_PX/DASH_MAP_SRC_DIM ratio -- same shape as gdx3ds_racemap.c's own BmapMarker, just
 * ratio-driven instead of a fixed-integer replication, so the hardware-feedback-round map shrink
 * (item 4, DASH_MAP_PX now 168, not an integer multiple of 64) scales markers correctly too; any
 * FUTURE custom-HUD resize of this box needs no change here either. Draw order: called AFTER the
 * track bitmap blit below, so markers sit on top of the track/background, matching the real
 * game's own draw order (Minimap_DrawCourseMinimap draws the track first, then stamps marker
 * fill-rects over it). */
static void DashPaintMapMarker(uint16_t* fb, int srcCol, int srcRow, uint16_t colour565) {
    const int c0 = DashClampInt(srcCol - 1, 0, DASH_MAP_SRC_DIM);
    const int c1 = DashClampInt(srcCol + 2, 0, DASH_MAP_SRC_DIM);
    const int r0 = DashClampInt(srcRow - 1, 0, DASH_MAP_SRC_DIM);
    const int r1 = DashClampInt(srcRow + 2, 0, DASH_MAP_SRC_DIM);
    const int x0 = DASH_MAP_X0 + (c0 * DASH_MAP_PX) / DASH_MAP_SRC_DIM;
    const int x1 = DASH_MAP_X0 + (c1 * DASH_MAP_PX) / DASH_MAP_SRC_DIM;
    const int y0 = DASH_MAP_Y0 + (r0 * DASH_MAP_PX) / DASH_MAP_SRC_DIM;
    const int y1 = DASH_MAP_Y0 + (r1 * DASH_MAP_PX) / DASH_MAP_SRC_DIM;
    DashFillRect(fb, x0, y0, x1 - x0, y1 - y0, colour565);
}

/* The map box: nearest-neighbour scale of the shared 64x64 CI8 minimap bitmap into DASH_MAP_PX
 * square screen pixels, destination-pixel-driven (iterate the SCREEN pixel, divide back to the
 * source index) rather than the old fixed-integer-replication loop -- see DASH_MAP_PX's own
 * comment (hardware feedback round, item 4: "could probably afford to be a bit smaller") for why
 * the box is no longer an exact integer multiple of 64. Racer markers (hw bug n60-3d7 item 6) are
 * drawn on top of it, reusing the same snapshot this function already fetched for its `.valid`
 * gate. */
static void DashPaintMapFpsText(uint16_t* fb);

static void DashPaintMapMarkers(uint16_t* fb, const GdxRaceMapSnapshot* mapSnap) {
    int i;
    for (i = 0; i < mapSnap->markerCount; i++) {
        const GdxRaceMapMarker* m = &mapSnap->markers[i];
        if (m->blink && !mapSnap->blinkOn) {
            continue; /* the game's own (gGameFrameCount % 16) < 8 blink */
        }
        if ((unsigned) m->kind < 6u) {
            DashPaintMapMarker(fb, m->col, m->row, Rgba5551To565(kDashMarkerRgba5551[m->kind]));
        }
    }
}

/* The map bitmap scaled into the box: the same nearest-neighbour texel per pixel as a
 * `(d * DASH_MAP_SRC_DIM) / DASH_MAP_PX` per pixel, with the steps tabulated once (o3dsperf: that
 * divide ran ~20000 times per repaint on a CPU without a divide instruction). */
/* o3ds60 dash_budget: the map box's scaled bitmap, column-major exactly as DashPaintMapBitmap
 * writes it (each cached column top to bottom in fb order), keyed on the minimap bitmap's bytes,
 * the palette and the box size. Only for a box fully on screen. */
#define DASH_MAP_CACHE_MAX 176
static uint16_t sMapCache[DASH_MAP_CACHE_MAX * DASH_MAP_CACHE_MAX];
static uint32_t sMapCacheKey = 0;
static int sMapCachePx = 0;

static uint32_t DashMapKey(const unsigned char* tex, const uint16_t* pal565, int px) {
    uint32_t h = 2166136261u;
    int i;
    for (i = 0; i < DASH_MAP_SRC_DIM * DASH_MAP_SRC_DIM; i++) {
        h = (h ^ (uint32_t) (tex[i] & 3u)) * 16777619u;
    }
    for (i = 0; i < 4; i++) {
        h = (h ^ pal565[i]) * 16777619u;
    }
    return (h ^ (uint32_t) px) * 16777619u;
}

static void DashMapCacheBuild(const unsigned char* tex, const uint16_t* pal565, int px) {
    int16_t srcTab[DASH_SCALE_TAB_MAX];
    int dx;
    DashScaleTable(srcTab, px, px, DASH_MAP_SRC_DIM);
    for (dx = 0; dx < px; dx++) {
        const unsigned char* src = tex + srcTab[dx];
        uint16_t* col = sMapCache + (size_t) dx * px;
        int dy;
        for (dy = 0; dy < px; dy++) {
            /* fb order: row DASH_MAP_Y0 + dy sits at col[-(y)], so the cache runs bottom-up */
            col[px - 1 - dy] = pal565[src[(size_t) srcTab[dy] * DASH_MAP_SRC_DIM] & 3u];
        }
    }
}

static int DashMapCacheBlit(uint16_t* fb, const unsigned char* tex, const uint16_t* pal565, int px) {
    uint32_t key;
    int dx;
    if (!sDashBudget || px > DASH_MAP_CACHE_MAX || px > DASH_SCALE_TAB_MAX) {
        return 0;
    }
    if (DASH_MAP_X0 < 0 || DASH_MAP_X0 + px > DASH_FB_W || DASH_MAP_Y0 < 0 || DASH_MAP_Y0 + px > DASH_FB_H) {
        return 0;
    }
    key = DashMapKey(tex, pal565, px);
    if (key != sMapCacheKey || px != sMapCachePx) {
        sMapCacheBuilds++;
        DashMapCacheBuild(tex, pal565, px);
        sMapCacheKey = key;
        sMapCachePx = px;
    }
    DashMarkCols(DASH_MAP_X0, px);
    for (dx = 0; dx < px; dx++) {
        const int x = DASH_MAP_X0 + dx;
        /* rows DASH_MAP_Y0 .. +px-1 land at fb indices (H-1-y): lowest index is y = Y0+px-1 */
        uint16_t* dst = fb + (size_t) x * DASH_FB_H + (DASH_FB_H - DASH_MAP_Y0 - px);
        memcpy(dst, sMapCache + (size_t) dx * px, (size_t) px * sizeof(uint16_t));
    }
    return 1;
}

static int DashPaintMapBitmap(uint16_t* fb, const unsigned char* tex, const uint16_t* pal565) {
    int16_t srcTab[DASH_SCALE_TAB_MAX];
    const int px = DASH_MAP_PX;
    int dx;
    if (px <= 0) {
        return 0;
    }
    if (px > DASH_SCALE_TAB_MAX) {
        DashMarkCols(DASH_MAP_X0, px);
        for (dx = 0; dx < px; dx++) {
            const int srcCol = (dx * DASH_MAP_SRC_DIM) / px;
            int dy;
            for (dy = 0; dy < px; dy++) {
                const int srcRow = (dy * DASH_MAP_SRC_DIM) / px;
                const uint16_t colour = pal565[tex[(size_t) srcRow * DASH_MAP_SRC_DIM + srcCol] & 3u];
                DashPixel(fb, DASH_MAP_X0 + dx, DASH_MAP_Y0 + dy, colour);
            }
        }
        return 0;
    }
    if (DashMapCacheBlit(fb, tex, pal565, px)) {
        return 1; /* o3ds60 dash_budget: the same pixels, from the cache */
    }
    DashMarkCols(DASH_MAP_X0, px);
    DashScaleTable(srcTab, px, px, DASH_MAP_SRC_DIM);
    for (dx = 0; dx < px; dx++) {
        const int x = DASH_MAP_X0 + dx;
        const unsigned char* src = tex + srcTab[dx];
        uint16_t* col;
        int dy;
        if (x < 0 || x >= DASH_FB_W) {
            continue;
        }
        col = fb + (size_t) x * DASH_FB_H + (DASH_FB_H - 1);
        for (dy = 0; dy < px; dy++) {
            const int y = DASH_MAP_Y0 + dy;
            if (y < 0 || y >= DASH_FB_H) {
                continue;
            }
            col[-y] = pal565[src[(size_t) srcTab[dy] * DASH_MAP_SRC_DIM] & 3u];
        }
    }
    return 0;
}

/* ---- o3ds60 dash_map_inc: the box overlay over the cached bitmap ---- */
static int DashFrameskipAtD1(void);
static int DashRace0EveryTick(void) {
    if (!gdx3ds_cpucut_on(sDashRace0D1, GDX3DS_CPUCUT_BIT_DASH)) {
        return 0;
    }
    return gdx3ds_frameskip_enabled() && gdx3ds_frameskip_mode() == GDX_FRAMESKIP_OFF;
}

/* The cache's rows of the box's column dx back into the shadow, over the rows the overlay had. */
static void DashMapOverlayRestore(uint16_t* fb, int px) {
    int dx;
    for (dx = 0; dx < px; dx++) {
        const int x = DASH_MAP_X0 + dx;
        int y0 = sOvY0[x];
        int y1 = sOvY1[x];
        if (y1 < y0) {
            continue;
        }
        if (y0 < DASH_MAP_Y0) {
            y0 = DASH_MAP_Y0;
        }
        if (y1 > DASH_MAP_Y0 + px - 1) {
            y1 = DASH_MAP_Y0 + px - 1;
        }
        if (y1 >= y0) {
            /* fb index H-1-y <-> cache index px-1-(y-Y0): one ascending run in both */
            memcpy(fb + (size_t) x * DASH_FB_H + (DASH_FB_H - 1 - y1),
                   sMapCache + (size_t) dx * px + (px - 1 - (y1 - DASH_MAP_Y0)), (size_t) (y1 - y0 + 1) * sizeof(uint16_t));
            DashMarkRows(x, y0, y1);
        }
    }
    DashRowsReset(sOvY0, sOvY1);
}

/* Runs a DashPixel / DashMarkCols painter whose rows are known (y0..y1) without its whole-column
 * marks: the painted columns go to sTrk instead. */
typedef struct DashColMarks {
    uint32_t bits[(DASH_FB_W + 31) / 32];
    int x0;
    int x1;
} DashColMarks;

static void DashColMarksSave(DashColMarks* m) {
    memcpy(m->bits, sDirtyColBits, sizeof(m->bits));
    m->x0 = sDirtyX0;
    m->x1 = sDirtyX1;
}

/* The painter's whole-column marks undone; its rect [xa, xb] x [ya, yb] tracked instead. */
static void DashColMarksToTrack(const DashColMarks* m, int xa, int xb, int ya, int yb) {
    int x;
    for (x = xa; x <= xb; x++) {
        DashRowsWiden(sTrkY0, sTrkY1, x, ya, yb);
    }
    memcpy(sDirtyColBits, m->bits, sizeof(m->bits));
    sDirtyX0 = m->x0;
    sDirtyX1 = m->x1;
}

static int DashFont2Height(void) {
    static int sH = -1;
    if (sH < 0) {
        int i;
        sH = 0;
        for (i = 0; i < KDASHGLYPHFONT2_COUNT; i++) {
            if (kDashGlyphFont2[i].h > sH) {
                sH = kDashGlyphFont2[i].h;
            }
        }
    }
    return sH;
}

static void DashPaintMapMarker(uint16_t* fb, int srcCol, int srcRow, uint16_t colour565);
static void DashPaintMapFpsTextTracked(uint16_t* fb);

/* The dots (or the 1P markers), the NET chip and the FPS text over the box, their rows tracked:
 * recorded as the overlay (the next restore) and marked for a rows-only present. */
static void DashMapOverlayPaint(uint16_t* fb, const GdxRaceMapSnapshot* mapSnap, int px) {
    DashColMarks marks;
    int x;
    DashRowsReset(sTrkY0, sTrkY1);
    if (sMpDash) {
        bui_kit_dirty_clear();
        bui_kit_row_track(sTrkY0, sTrkY1);
        gdx3ds_buirace_paint_map(fb, DASH_MAP_X0, DASH_MAP_Y0, px); /* every machine + NET */
        bui_kit_row_track(NULL, NULL);
    } else {
        int i;
        for (i = 0; i < mapSnap->markerCount; i++) {
            const GdxRaceMapMarker* m = &mapSnap->markers[i];
            if (m->blink && !mapSnap->blinkOn) {
                continue;
            }
            if ((unsigned) m->kind < 6u) {
                /* DashPaintMapMarker's own rect */
                const int c0 = DashClampInt(m->col - 1, 0, DASH_MAP_SRC_DIM);
                const int c1 = DashClampInt(m->col + 2, 0, DASH_MAP_SRC_DIM);
                const int r0 = DashClampInt(m->row - 1, 0, DASH_MAP_SRC_DIM);
                const int r1 = DashClampInt(m->row + 2, 0, DASH_MAP_SRC_DIM);
                DashColMarksSave(&marks);
                DashPaintMapMarker(fb, m->col, m->row, Rgba5551To565(kDashMarkerRgba5551[m->kind]));
                DashColMarksToTrack(&marks, DASH_MAP_X0 + (c0 * DASH_MAP_PX) / DASH_MAP_SRC_DIM,
                                    DASH_MAP_X0 + (c1 * DASH_MAP_PX) / DASH_MAP_SRC_DIM - 1,
                                    DASH_MAP_Y0 + (r0 * DASH_MAP_PX) / DASH_MAP_SRC_DIM,
                                    DASH_MAP_Y0 + (r1 * DASH_MAP_PX) / DASH_MAP_SRC_DIM - 1);
            }
        }
    }
    DashPaintMapFpsTextTracked(fb);
    for (x = 0; x < DASH_FB_W; x++) {
        int y0 = sTrkY0[x];
        int y1 = sTrkY1[x];
        if (y1 < y0) {
            continue;
        }
        if (x < DASH_MAP_X0 || x >= DASH_MAP_X0 + px || y0 < DASH_MAP_Y0 || y1 > DASH_MAP_Y0 + px - 1) {
            DashMarkCols(x, 1); /* outside the cached box: not restorable, present it whole */
        }
        if (y0 < DASH_MAP_Y0) {
            y0 = DASH_MAP_Y0;
        }
        if (y1 > DASH_MAP_Y0 + px - 1) {
            y1 = DASH_MAP_Y0 + px - 1;
        }
        if (x >= DASH_MAP_X0 && x < DASH_MAP_X0 + px && y1 >= y0) {
            DashRowsWiden(sOvY0, sOvY1, x, y0, y1);
            DashMarkRows(x, y0, y1);
        }
    }
}

/* The incremental map tick: 1 = done (the shadow's box is the cache plus this tick's overlay). */
static int DashPaintMapBoxInc(uint16_t* fb, const unsigned char* tex, const uint16_t* pal565,
                              const GdxRaceMapSnapshot* mapSnap) {
    const int px = DASH_MAP_PX;
    if (!sDashMapInc || !sMapIncValid || sDirtyFull || px != sMapCachePx) {
        return 0;
    }
    if (sMapIncSince >= DASH_MAP_INC_HEAL && !DashFrameskipAtD1()) {
        return 0; /* the self-heal: a whole box off D = 1 now and then */
    }
    if (DashMapKey(tex, pal565, px) != sMapCacheKey) {
        return 0;
    }
    DashMapOverlayRestore(fb, px);
    DashMapOverlayPaint(fb, mapSnap, px);
    sMapIncSince++;
    sMapIncPaints++;
    sTickCost.mapInc = 1;
    return 1;
}

static void DashPaintMapBox(uint16_t* fb) {
    GdxRaceMapSnapshot mapSnap;
    const unsigned char* tex = NULL;
    unsigned short pal5551[4];
    uint16_t pal565[4];
    int i;

    gdx_racemap_snapshot(&mapSnap);
    if (!mapSnap.valid || !gdx_racemap_bitmap(&tex, pal5551)) {
        sMapIncValid = 0;
        DashFillRect(fb, DASH_MAP_X0, DASH_MAP_Y0, DASH_MAP_PX, DASH_MAP_PX, 0x0841u);
        DashStrokeRect(fb, DASH_MAP_X0 - 1, DASH_MAP_Y0 - 1, DASH_MAP_PX + 2, DASH_MAP_PX + 2, 0xFFFFu);
        DashPaintMapFpsText(fb);
        return;
    }
    pal565[0] = 0x0841u;
    for (i = 1; i < 4; i++) {
        pal565[i] = Rgba5551To565(pal5551[i]);
    }
    if (DashPaintMapBoxInc(fb, tex, pal565, &mapSnap)) {
        return; /* o3ds60 dash_map_inc */
    }
    if (DashPaintMapBitmap(fb, tex, pal565) && sDashMapInc) {
        /* o3ds60 dash_map_inc: the whole box from the cache, the overlay recorded for next time */
        DashRowsReset(sOvY0, sOvY1);
        DashMapOverlayPaint(fb, &mapSnap, DASH_MAP_PX);
        DashStrokeRect(fb, DASH_MAP_X0 - 1, DASH_MAP_Y0 - 1, DASH_MAP_PX + 2, DASH_MAP_PX + 2, 0xFFFFu);
        sMapIncValid = 1;
        sMapFullThisTick = 1;
        sMapIncSince = 0;
        sMapFullPaints++;
        return;
    }
    sMapIncValid = 0;
    if (sMpDash) {
        int mx0;
        int mx1;
        bui_kit_dirty_clear();
        gdx3ds_buirace_paint_map(fb, DASH_MAP_X0, DASH_MAP_Y0, DASH_MAP_PX); /* every machine + NET */
        if (bui_kit_dirty(&mx0, &mx1)) {
            DashMarkCols(mx0, mx1 - mx0 + 1);
        }
    } else {
        DashPaintMapMarkers(fb, &mapSnap);
    }
    DashStrokeRect(fb, DASH_MAP_X0 - 1, DASH_MAP_Y0 - 1, DASH_MAP_PX + 2, DASH_MAP_PX + 2, 0xFFFFu);
    DashPaintMapFpsText(fb);
}

/* sDashWidgets' "map_box" per-tick paint fn: owns its own [debug] bottom_race_hz throttle
 * internally (moved here from gdx3ds_bottomhud_tick, which used to track sLastMapTick itself) so
 * this widget is fully self-contained, matching the custom-HUD-groundwork table's "each row is
 * one independent per-tick paint call" shape. `snap` is unused (DashPaintMapBox fetches its own
 * GdxRaceMapSnapshot, a deliberately separate contract -- see gdx_racemap.h's own module doc for
 * why map state is a sibling struct, not a growth of GdxRaceHudSnapshot). */
static void DashPaintMapBoxLive(uint16_t* fb, const GdxRaceHudSnapshot* snap) {
    const u64 now = svcGetSystemTick();
    const u64 interval = (u64) (1000.0 * CPU_TICKS_PER_MSEC) / (u64) sHz;
    (void) snap;
    if (!sDashWidgets[DASHW_MAP_BOX].enabled) {
        return;
    }
    if ((now - sLastMapTick) < interval) {
        return;
    }
    DashPaintMapBox(fb);
    sLastMapTick = now;
}

/* COORDINATOR FIX (third round item 5): the baked energy_bar widget (320x24) has its
 * "ENERGY" label at cols ~7-53 and the decorative frame at cols 54-314; filling from x=4 (the
 * old flat 4px bezel on both edges) painted straight over the label and made the frame's own
 * left cap look clipped. Start the fill inside the frame instead (x=57) and stop 4px short of
 * its right edge (314-4=310), so the fill sits inside the frame art instead of under the label. */
#define DASH_ENERGY_FILL_X0 (DASH_ENERGY_X0 + 57)
#define DASH_ENERGY_FILL_W ((DASH_ENERGY_X0 + 314 - 4) - DASH_ENERGY_FILL_X0)

/* COORDINATOR FIX (fourth round): "the energy bar is flat yellow, but it should follow the real
 * game's colour scheme exactly." Read hud.c's own Hud_DrawEnergyBar (decomp/src/overlays/
 * ovl_i3/hud.c:617-665) and sEnergyBarFillColors (:134-145): there is NO continuous energy-level
 * gradient and NO low-energy flashing anywhere in that function or file (grepped) -- the only
 * state it reads is RACER_STATE_CAN_BOOST (sEnergyBarFillColors[i+5] vs [i], i=0..4), the same
 * flag DASH's own `boostReady` already mirrors exactly (gdx_racemap.h:122, decomp_port.c:2063).
 * What the game DOES draw is a fixed 5-row vertical "glossy" shading, one solid colour per
 * scanline, switched as a whole from a red family (boost not yet available) to a green family
 * (boost available -- set once per completed lap, cleared on use) by that single flag. Row 3 of
 * the red family packs two different RGBA5551 values into one 32-bit N64 FillColor
 * (GPACK_RGBA5551(220,0,0,1)<<16 | GPACK_RGBA5551(230,0,0,1)), which the hardware fill path
 * renders as alternating pixels -- reproduced here as an x-parity dither. Values below are
 * hud.c's own sEnergyBarFillColors rows, each converted once (offline, by hand) through this
 * file's own Rgba5551To565 (the same 5->6 bit green widen every other baked-art pixel here goes
 * through). The bar is scaled to our 16px-tall fill (hud.c's 1-player bar is 5px tall) by
 * nearest-neighbour row mapping, same integer-ratio style as DashBlitGlyphRatio. */
static const uint16_t kEnergyRowRed[5] = {0x9800u, 0xd800u, 0xf800u, 0xd800u, 0x9800u};
static const uint16_t kEnergyRowRedDitherB = 0xe000u; /* row 3's alternate (odd-x) pixel colour */
static const uint16_t kEnergyRowGreen[5] = {0x07e0u, 0x07e2u, 0x0624u, 0x0526u, 0x0428u};

static void DashPaintEnergyFooter(uint16_t* fb, const GdxRaceHudSnapshot* snap) {
    const int fillW = (int) (((long) DASH_ENERGY_FILL_W * snap->energyMilli) / 1000);
    const int top = DASH_ENERGY_Y0 + 4;
    const int fillH = DASH_ENERGY_H - 8;
    int dy;
    char key[DASH_MEMO_KEY];
    if (!sDashWidgets[DASHW_ENERGY_BAR].enabled) {
        return;
    }
    snprintf(key, sizeof(key), "%d|%d", fillW, snap->boostReady ? 1 : 0);
    if (DashMemoSame(DASHW_ENERGY_BAR, key)) {
        return;
    }
    DashFillRect(fb, DASH_ENERGY_FILL_X0, top, DASH_ENERGY_FILL_W, fillH, 0x0000u);
    DashMarkCols(DASH_ENERGY_FILL_X0, fillW);
    for (dy = 0; dy < fillH; dy++) {
        const int row = (dy * 5) / fillH;
        int dx;
        for (dx = 0; dx < fillW; dx++) {
            const int x = DASH_ENERGY_FILL_X0 + dx;
            uint16_t colour;
            if (snap->boostReady) {
                colour = kEnergyRowGreen[row];
            } else if (row == 3 && (x & 1)) {
                colour = kEnergyRowRedDitherB;
            } else {
                colour = kEnergyRowRed[row];
            }
            DashPixel(fb, x, top + dy, colour);
        }
    }
}

/* Repaints a rect of the dimmed checkerboard (DashPaintBackdropBackground's own texels) -- for
 * live widgets with no panel of their own that sit straight on the backdrop and must erase what
 * they drew last time. Only valid where no baked widget art overlaps the rect. */
static void DashRestoreBackground(uint16_t* fb, int x0, int y0, int w, int h) {
    int x;
    DashMarkCols(x0, w);
    for (x = x0; x < x0 + w; x++) {
        int y;
        for (y = y0; y < y0 + h; y++) {
            DashPixel(fb, x, y,
                      DashDimBackground(gDashAssetBackground != NULL ? gDashAssetBackground[y * DASH_FB_W + x] : 0u));
        }
    }
}

/* ---- spare machines (life stock), under the energy bar's baked "ENERGY" label -------------
 *
 * decomp/src/overlays/ovl_i3/hud.c's Hud_DrawPlayerLives: one 8x8 aHudLivesLeftIconTex per
 * gPlayerLives[0], 10px pitch, wrapping every 5 onto a new row 9px down, GP races only
 * (snapshot spareMachines == -1 everywhere else, so nothing is drawn). The baked label covers
 * rows 2-7 of the 24-tall energy widget at cols 8-50; rows 8-23 of cols 0-53 are transparent in
 * the baked energy_bar widget (plain checkerboard), and rows 24-25 are the gutter above the portrait
 * widget (y=26). So the stock gets x 5..52 (5 icons = 48px, left of the frame at col 54):
 *   1-5 spare  : one row at y 12..19, centred in the free band below the label;
 *   6-10 spare : two rows, y 9..16 and 18..25 (the game's own 9px row pitch);
 *   11+        : one icon + "X<n>" in Font2, which cannot outgrow the strip.
 * The game's 90-frame flash of a just-gained/lost icon is not reproduced: it would need a
 * per-tick repaint and its counter (gLivesChangeCounter) only advances while the top HUD draws. */
#define DASH_LIVES_X0 (DASH_ENERGY_X0 + 5)
#define DASH_LIVES_Y0 (DASH_ENERGY_Y0 + 9)
#define DASH_LIVES_W 48
#define DASH_LIVES_H 17
#define DASH_LIVES_PITCH_X 10
#define DASH_LIVES_PITCH_Y 9
#define DASH_LIVES_PER_ROW 5
#define DASH_LIVES_MAX_ICONS (2 * DASH_LIVES_PER_ROW)

static void DashPaintLivesIcons(uint16_t* fb, const DashGlyph* icon, int count) {
    const int y0 = (count <= DASH_LIVES_PER_ROW) ? (DASH_ENERGY_Y0 + 12) : DASH_LIVES_Y0;
    int i;
    for (i = 0; i < count; i++) {
        const int x = DASH_LIVES_X0 + (i % DASH_LIVES_PER_ROW) * DASH_LIVES_PITCH_X;
        const int y = y0 + (i / DASH_LIVES_PER_ROW) * DASH_LIVES_PITCH_Y;
        DashBlitGlyphRatio(fb, x, y, icon, 1, 1);
    }
}

static void DashPaintLivesCount(uint16_t* fb, const DashGlyph* icon, int count) {
    char buf[8];
    const int y = DASH_ENERGY_Y0 + 12;
    snprintf(buf, sizeof(buf), "X%d", count); /* Font2 has no lowercase -- see DashPaintKOStars */
    DashBlitGlyphRatio(fb, DASH_LIVES_X0, y, icon, 1, 1);
    DashBlitFontText(fb, DASH_LIVES_X0 + icon->w + 3, y, kDashGlyphFont2, KDASHGLYPHFONT2_COUNT, buf, 1, 1,
                      DASH_FONT2_SPACE, 1);
}

static void DashPaintLives(uint16_t* fb, const GdxRaceHudSnapshot* snap) {
    const DashGlyph* icon;
    if (!sDashWidgets[DASHW_ENERGY_BAR].enabled || snap->spareMachines == sLastSpareMachines) {
        return;
    }
    sLastSpareMachines = snap->spareMachines;
    DashRestoreBackground(fb, DASH_LIVES_X0, DASH_LIVES_Y0, DASH_LIVES_W, DASH_LIVES_H);
    icon = DashFindGlyph(kDashGlyphLives, KDASHGLYPHLIVES_COUNT, "machine");
    if (icon == NULL || snap->spareMachines <= 0) {
        return; /* hidden (not a GP race) or no machines left */
    }
    if (snap->spareMachines <= DASH_LIVES_MAX_ICONS) {
        DashPaintLivesIcons(fb, icon, snap->spareMachines);
    } else {
        DashPaintLivesCount(fb, icon, snap->spareMachines);
    }
}

/* ---- FPS text: inside the map box, top-right corner ----------------------------------------
 *
 * Plain Font2 text ("60 FPS"), right-aligned 3px inside the map's white border, with a 1px dark
 * outline so it stays legible where the track line or a racer dot runs through that corner. It
 * is drawn as the LAST layer of DashPaintMapBox, which repaints the whole map interior first, so
 * it composites on top of every map repaint and a narrower value ("60" -> "9") leaves no residue;
 * a new sample therefore shows on the next map repaint ([debug] bottom_race_hz). The rate is
 * gdx3ds_fps_hud.c's "drawn" figure measured here directly (this tick runs once per host
 * iteration while the DASH owns the screen, and a frameskip skip iteration presents nothing), so
 * it works with [debug] fps=0 too. Sampled every DASH_FPS_WINDOW_MS (2 Hz). */
#define DASH_FPS_INSET 3
#define DASH_FPS_RIGHT (DASH_MAP_X0 + DASH_MAP_PX - DASH_FPS_INSET) /* one past the text's last column */
#define DASH_FPS_TOP (DASH_MAP_Y0 + DASH_FPS_INSET)
#define DASH_FPS_OUTLINE 0x0000u
#define DASH_FPS_WINDOW_MS 500u

static void DashFpsResetWindow(u64 now) {
    sFpsWindowStartTick = now;
    sFpsWindowTicks = 0;
    sFpsWindowStartSkipped = gdx3ds_frameskip_skipped_total();
}

/* Advances the sampling window by one host iteration, updating sFpsShown once per window. */
static void DashFpsSample(u64 now) {
    unsigned long skipped;
    unsigned long drawn;
    u32 elapsedMs;
    if (sFpsWindowStartTick == 0) {
        DashFpsResetWindow(now); /* first DASH tick of a race: baseline only */
        return;
    }
    sFpsWindowTicks++;
    elapsedMs = (u32) ((now - sFpsWindowStartTick) / (u64) CPU_TICKS_PER_MSEC);
    if (elapsedMs < DASH_FPS_WINDOW_MS) {
        return;
    }
    if (elapsedMs > 4u * DASH_FPS_WINDOW_MS) {
        DashFpsResetWindow(now); /* the DASH was off screen (dismissed / screen off): re-baseline */
        return;
    }
    skipped = gdx3ds_frameskip_skipped_total() - sFpsWindowStartSkipped;
    drawn = (skipped < sFpsWindowTicks) ? sFpsWindowTicks - skipped : 0;
    sFpsShown = (int) ((drawn * 1000ul + elapsedMs / 2u) / elapsedMs);
    if (sFpsShown > 60) {
        sFpsShown = 60; /* presents are vsync-bound: 61 is only window-edge jitter (31 frames in 508ms) */
    }
    DashFpsResetWindow(now);
}

/* One glyph's opaque texels in a single flat colour, at native size: the outline pass below. */
static void DashBlitGlyphSolid(uint16_t* fb, int x, int y, const DashGlyph* g, uint16_t colour) {
    int gy;
    DashMarkCols(x, g->w);
    for (gy = 0; gy < g->h; gy++) {
        int gx;
        for (gx = 0; gx < g->w; gx++) {
            if (g->pixels[gy * g->w + gx] & 1u) {
                DashPixel(fb, x + gx, y + gy, colour);
            }
        }
    }
}

/* Font2 text at native size with a 1px outline: every glyph stamped in `outline` at the 8
 * neighbouring offsets, then the real glyph on top. Same advance/space/gap walk as
 * DashBlitFontText (gap 1). */
static void DashBlitFont2Outlined(uint16_t* fb, int x, int y, const char* s, uint16_t outline) {
    int cx = x;
    int n = 0;
    for (; *s != '\0'; s++) {
        if (n > 0) {
            cx += 1;
        }
        if (*s == ' ') {
            cx += DASH_FONT2_SPACE;
        } else {
            char key[2] = {*s, '\0'};
            const DashGlyph* g = DashFindGlyph(kDashGlyphFont2, KDASHGLYPHFONT2_COUNT, key);
            if (g != NULL) {
                int oy;
                for (oy = -1; oy <= 1; oy++) {
                    int ox;
                    for (ox = -1; ox <= 1; ox++) {
                        if (ox != 0 || oy != 0) {
                            DashBlitGlyphSolid(fb, cx + ox, y + oy, g, outline);
                        }
                    }
                }
                DashBlitGlyphRatio(fb, cx, y, g, 1, 1);
                cx += g->adv;
            }
        }
        n++;
    }
}

static void DashPaintMapFpsText(uint16_t* fb) {
    char buf[12];
    int tw;
    if (!sFpsEnabled) {
        return;
    }
    if (sFpsShown < 0) {
        snprintf(buf, sizeof(buf), "-- FPS");
    } else {
        snprintf(buf, sizeof(buf), "%d FPS", sFpsShown);
    }
    tw = DashFontTextWidthRatio(kDashGlyphFont2, KDASHGLYPHFONT2_COUNT, buf, 1, 1, DASH_FONT2_SPACE, 1);
    DashBlitFont2Outlined(fb, DASH_FPS_RIGHT - tw, DASH_FPS_TOP, buf, DASH_FPS_OUTLINE);
}

/* o3ds60 dash_map_inc: the FPS text with its rect tracked (DashMapOverlayPaint). */
static void DashPaintMapFpsTextTracked(uint16_t* fb) {
    DashColMarks marks;
    int tw;
    if (!sFpsEnabled) {
        return;
    }
    /* the widest text it paints ("999 FPS"), its 1 px outline included */
    tw = DashFontTextWidthRatio(kDashGlyphFont2, KDASHGLYPHFONT2_COUNT, "999 FPS", 1, 1, DASH_FONT2_SPACE, 1);
    DashColMarksSave(&marks);
    DashPaintMapFpsText(fb);
    DashColMarksToTrack(&marks, DASH_FPS_RIGHT - tw - 2, DASH_FPS_RIGHT + 1, DASH_FPS_TOP - 1,
                        DASH_FPS_TOP + DashFont2Height());
}

static void DashPaintModeDot(uint16_t* fb) {
    /* Sized GDX_HUD_PLACEMENT_CUSTOM+1 (4), not GDX_HUD_PLACEMENT_MODE_COUNT (3): CUSTOM is a
     * real, reachable value of sPlacementMode now, and this array is indexed by it directly. */
    static const uint16_t kModeColour[GDX_HUD_PLACEMENT_CUSTOM + 1] = {0x07E0u, 0xFFE0u, 0xF81Fu, 0x07FFu};
    if (!sDashWidgets[DASHW_MODE_CHIP].enabled) {
        return;
    }
    DashFillRect(fb, DASH_MODE_X0 + DASH_MODE_W - 10, DASH_MODE_Y0 + 2, 6, 6, kModeColour[sPlacementMode]);
}

/* ---- the live numeric overlay: real glyphs only, one paint fn per right-column widget ---- */

/* "0'42"18"" -- same separators hud.c's own Hud_DrawTimerScisThousandths draws (prime/double-
 * prime, not colon/dot; see tools/gen_nibottom_mockup_assets.py's own citation of that). ms is
 * assumed >= 0 (lapTimeMs/bestTimeMs never go negative); gapMs is signed and goes through
 * DashFormatInterval instead, which adds the sign itself. */
static void DashFormatClock(char* out, size_t cap, int ms) {
    const int totalCs = ms / 10;
    const int cs = totalCs % 100;
    const int totalSec = totalCs / 100;
    const int sec = totalSec % 60;
    const int min = totalSec / 60;
    snprintf(out, cap, "%d'%02d\"%02d", min, sec, cs);
}

static void DashFormatInterval(char* out, size_t cap, int ms) {
    char body[16];
    const int absMs = (ms < 0) ? -ms : ms;
    DashFormatClock(body, sizeof(body), absMs);
    snprintf(out, cap, "%s%s", (ms < 0) ? "-" : "", body);
}

/* COORDINATOR FIX (second round item 2 / third round item 1): "the right-column text... is too
 * small" -> "replace [the hand font]... aTimerSymbolsTex (8x16 digits plus ' " for times)."
 * Each field clears its own unbaked value row (DashBakeSkipCell) and redraws with the real
 * digit-strip textures every tick, at their native num=1,den=1 scale (gap=0: a dense digit strip
 * reads fine edge-to-edge, same way the game's own HUD draws it, and it is the only scale that
 * fits LAP/TIME/BEST/GAP's narrow ~56-58px boxes without clipping -- confirmed by width
 * arithmetic against kFontCharWidths-less-but-uniform 8px timer/lap cells).
 *
 * Split into one function per sDashWidgets row (Lap/Timer/Gap) instead of one monolithic sweep,
 * matching the "one paint fn per widget" table shape; BOOST's own badge is gone outright (item 6,
 * hardware feedback round: "basically useless... I'd put the [KO] stars at the top of this right
 * side column instead" -- see DashPaintKOStars, below, and sDashWidgets' own "ko_stars" row for
 * where that box now lives). */
/* uipolish: the clocks are clamped to what the 76 px stats box shows in full (8 cells of 8 px):
 * a lap or best time to 99'59"99, a gap to -9'59"99 .. 9'59"99. */
#define DASH_CLOCK_MAX_MS 5999990
#define DASH_GAP_MAX_MS 599990

static int DashClampMs(int ms, int maxMs) {
    if (ms > maxMs) {
        return maxMs;
    }
    if (ms < -maxMs) {
        return -maxMs;
    }
    return ms;
}

/* 1 when every character of s has a glyph in the lap set (it has '/' and 1..5 only). */
static int DashLapGlyphsCover(const char* s) {
    for (; *s != '\0'; s++) {
        const char key[2] = {*s, '\0'};
        if (DashFindGlyph(kDashGlyphLap, KDASHGLYPHLAP_COUNT, key) == NULL) {
            return 0;
        }
    }
    return 1;
}

/* "n/m" in the lap digits; with a lap past 5 (MP races run up to 9 laps) the lap set has no
 * glyph for it, so every digit comes from the timer set (8x16, its ink 2 rows taller) and only
 * the '/' from the lap set, centred on the same line. */
static void DashPaintLapValue(uint16_t* fb, const char* s, int x, int y) {
    if (DashLapGlyphsCover(s)) {
        DashBlitFontText(fb, x, y, kDashGlyphLap, KDASHGLYPHLAP_COUNT, s, 1, 1, 0, 0);
        return;
    }
    for (; *s != '\0'; s++, x += 8) {
        const char key[2] = {*s, '\0'};
        const DashGlyph* g = DashFindGlyph(kDashGlyphTimer, KDASHGLYPHTIMER_COUNT, key);
        if (*s == '/' || g == NULL) {
            g = DashFindGlyph(kDashGlyphLap, KDASHGLYPHLAP_COUNT, key);
            if (g != NULL) {
                DashBlitGlyphRatio(fb, x, y + 1, g, 1, 1);
            }
        } else {
            DashBlitGlyphRatio(fb, x, y - 2, g, 1, 1);
        }
    }
}

static void DashPaintRightColumnLap(uint16_t* fb, const GdxRaceHudSnapshot* snap) {
    char buf[16];
    int lapDisp;
    if (!sDashWidgets[DASHW_LAP].enabled) {
        return;
    }
    /* COORDINATOR FIX (hardware feedback round, item 7): "when the race ends, this goes up to
     * 4/3. Should be clamped." gRacers[0].lap keeps counting past gTotalLapCount once the race
     * finishes (the decomp's own post-finish bookkeeping, left alone here -- this is a DASH
     * display-only clamp, not a game-state change). */
    lapDisp = snap->lap;
    if (snap->totalLaps > 0 && lapDisp > snap->totalLaps) {
        lapDisp = snap->totalLaps;
    }
    snprintf(buf, sizeof(buf), "%d/%d", lapDisp, snap->totalLaps);
    if (DashMemoSame(DASHW_LAP, buf)) {
        return;
    }
    DashStatsClearRow(fb, DASH_STATS_LAP_VALUE_Y - 2, 15);
    DashPaintLapValue(fb, buf, DASH_STATS_TEXT_X, DASH_STATS_LAP_VALUE_Y);
}

static void DashPaintStatsClock(uint16_t* fb, const char* s, int y) {
    DashStatsClearRow(fb, y, 16);
    DashBlitFontText(fb, DASH_STATS_TEXT_X, y, kDashGlyphTimer, KDASHGLYPHTIMER_COUNT, s, 1, 1, 0, 0);
}

static void DashPaintRightColumnTimer(uint16_t* fb, const GdxRaceHudSnapshot* snap) {
    char buf[16];
    char best[16];
    char key[DASH_MEMO_KEY];
    if (!sDashWidgets[DASHW_TIMER].enabled) {
        return;
    }
    DashFormatClock(buf, sizeof(buf), DashClampMs(snap->lapTimeMs, DASH_CLOCK_MAX_MS));
    DashFormatClock(best, sizeof(best), DashClampMs(snap->bestTimeMs, DASH_CLOCK_MAX_MS));
    snprintf(key, sizeof(key), "%s|%s", buf, best);
    if (DashMemoSame(DASHW_TIMER, key)) {
        return;
    }
    DashPaintStatsClock(fb, buf, DASH_STATS_TIME_VALUE_Y);
    DashPaintStatsClock(fb, best, DASH_STATS_BEST_VALUE_Y);
}

static void DashPaintRightColumnGap(uint16_t* fb, const GdxRaceHudSnapshot* snap) {
    char buf[16];
    if (!sDashWidgets[DASHW_GAP].enabled) {
        return;
    }
    DashFormatInterval(buf, sizeof(buf), DashClampMs(snap->gapMs, DASH_GAP_MAX_MS));
    if (DashMemoSame(DASHW_GAP, buf)) {
        return;
    }
    DashPaintStatsClock(fb, buf, DASH_STATS_GAP_VALUE_Y);
}

/* COORDINATOR FIX (hardware feedback round, item 6): "[BOOST is] basically useless... more
 * useful would be the 'knockout' stars which are not visible anywhere on the second screen
 * HUD. I'd put the stars at the top of this right side column." Source: decomp/src/overlays/
 * ovl_i3/hud.c's Hud_DrawKOStars (hud_gfx/aHudRacersKOdTex, one 8x8 star per gRacersKOd, a
 * global race-wide knockout counter -- gdx_racehud_snapshot's own `koCount` field, see
 * gdx_racemap.h). No baked backdrop (same reasoning DASH_SPEED_* used when it was added): chrome,
 * the real star icon and the count are all repainted live, same chip style as MODE/MENU/SPEED. A
 * single icon + "x<N>" numeral reads correctly at any count from 0 to the full 30-racer field,
 * unlike trying to fit a literal growing star GRID (what the N64 HUD draws, with much more
 * screen edge to grow into) inside this 62px-wide column. */
static void DashPaintKOStars(uint16_t* fb, const GdxRaceHudSnapshot* snap) {
    char buf[8];
    const DashGlyph* star;
    int tw;
    int iconW;
    int x;
    int y;
    if (!sDashWidgets[DASHW_KO_STARS].enabled) {
        return;
    }
    /* "X%d", not "x%d": kDashGlyphFont2 only has uppercase A-Z (FONT_CHAR_SPEC, the generator's
     * own 48-slot font.c scheme) -- a lowercase 'x' silently drops (DashFindGlyph finds nothing,
     * DashBlitFontText skips it with zero advance), which first-pass testing against a live
     * emulator capture caught: the box rendered a bare "0", no "x". */
    snprintf(buf, sizeof(buf), "X%d", snap->koCount);
    if (DashMemoSame(DASHW_KO_STARS, buf)) {
        return;
    }
    tw = DashFontTextWidthRatio(kDashGlyphFont2, KDASHGLYPHFONT2_COUNT, buf, 1, 1, DASH_FONT2_SPACE, 1);
    star = DashFindGlyph(kDashGlyphKoStar, KDASHGLYPHKOSTAR_COUNT, "star");
    iconW = (star != NULL) ? star->w * 2 : 0; /* num=2,den=1: the native 8x8 icon shown at 16x16 */

    DashFillRect(fb, DASH_KO_X0 + 1, DASH_KO_Y0 + 1, DASH_KO_W - 2, DASH_KO_H - 2, 0x3885u);
    DashStrokeRect(fb, DASH_KO_X0, DASH_KO_Y0, DASH_KO_W, DASH_KO_H, 0xFB76u);
    x = DASH_KO_X0 + (DASH_KO_W - (iconW + 4 + tw)) / 2;
    y = DASH_KO_Y0 + (DASH_KO_H - 16) / 2;
    if (star != NULL) {
        DashBlitGlyphRatio(fb, x, y, star, 2, 1);
    }
    DashBlitFontText(fb, x + iconW + 4, DASH_KO_Y0 + (DASH_KO_H - 8) / 2, kDashGlyphFont2, KDASHGLYPHFONT2_COUNT,
                      buf, 1, 1, DASH_FONT2_SPACE, 1);
}

/* card-4b (hw: "the km/h is so small at bottom. hard to see at a glance... make it look more like
 * it does on the top screen"): the top screen's own speedometer textures (Hud_DrawPlayerSpeed's
 * aSpeedDigitsTex 12x16 cells at a 12 px advance, then aKmhTex) on its black box, the km/h unit
 * at 1x under the digits' right edge. Repaints only when the number changes (DashMemoSame), like
 * every other right-column value.
 * uipolish (hw: "the speedometer numbers slightly clip under the map"): the digits' ink, not
 * their cells, is placed: up to 999 at 2x (three digits are 70 px of ink, 2 px inside the 76 px
 * box on each side), 1000+ at 3/2 (four digits, the same 70 px) instead of the old 1x fallback,
 * bottom-aligned with the 2x row; fewer digits keep the same right edge. */
#define DASH_SPEED_DIGIT_ADV 12
#define DASH_SPEED_DIGIT_INK_W 11 /* aSpeedDigitsTex's ink is cols 0..10 of each 12 px cell */
#define DASH_SPEED_KMH_INK_X 1    /* aKmhTex's ink is cols 1..18 of its 20 */
#define DASH_SPEED_KMH_INK_W 18
#define DASH_SPEED_INK_RIGHT (DASH_SPEED_X0 + DASH_SPEED_W - 4) /* the digits' and unit's last ink column */

typedef struct DashSpeedRow {
    int num, den; /* glyph scale */
    int adv;      /* cell advance at that scale */
    int x;        /* the first cell's left edge */
    int y;        /* the cells' top */
    int h;        /* the cells' height */
} DashSpeedRow;

static DashSpeedRow DashSpeedLayout(int digits) {
    DashSpeedRow r;
    int inkW;
    if (digits <= 3) {
        r.num = 2;
        r.den = 1;
        r.y = DASH_SPEED_Y0 + 1; /* ink rows 2..29 of the 32 px cell: box rows 3..30 */
    } else {
        r.num = 3;
        r.den = 2;
        r.y = DASH_SPEED_Y0 + 8; /* ink rows 2..22 of the 24 px cell: bottom-aligned at row 30 */
    }
    r.adv = (DASH_SPEED_DIGIT_ADV * r.num) / r.den;
    r.h = (16 * r.num) / r.den;
    inkW = (DASH_SPEED_DIGIT_INK_W * r.num + r.den - 1) / r.den;
    r.x = DASH_SPEED_INK_RIGHT + 1 - ((digits - 1) * r.adv + inkW);
    return r;
}

static void DashPaintSpeedDigits(uint16_t* fb, const char* digits, const DashSpeedRow* r, int x) {
    for (; *digits != '\0'; digits++) {
        const char key[2] = {*digits, '\0'};
        const DashGlyph* g = DashFindGlyph(kDashGlyphSpeedDigit, KDASHGLYPHSPEEDDIGIT_COUNT, key);
        if (g != NULL) {
            DashBlitGlyphRatio(fb, x, r->y, g, r->num, r->den);
        }
        x += r->adv;
    }
}

static void DashPaintSpeedRow(uint16_t* fb, const char* digits) {
    const DashGlyph* kmh = DashFindGlyph(kDashGlyphKmh, KDASHGLYPHKMH_COUNT, "kmh");
    const DashSpeedRow r = DashSpeedLayout((int) strlen(digits));
    DashPaintSpeedDigits(fb, digits, &r, r.x);
    if (kmh != NULL) {
        /* the unit's ink (cell rows 5..15) starts 1 px under the digits' ink (box rows 3..30) */
        DashBlitGlyphRatio(fb, DASH_SPEED_INK_RIGHT + 1 - DASH_SPEED_KMH_INK_X - DASH_SPEED_KMH_INK_W,
                           DASH_SPEED_Y0 + 27, kmh, 1, 1);
    }
}

/* The common case, keeping the paint cost of the big digits low: the box is already painted with
 * the same number of digits, so only the digit cells that changed are cleared and redrawn
 * (usually the last one or two). A cell is cleared over its own rows only (box rows 1..31 at 2x):
 * the digits' ink ends at row 30 and the km/h unit's starts at row 32, so the unit and the border
 * stay. 0 = the caller paints the whole box. */
static int DashPaintSpeedChangedDigits(uint16_t* fb, const char* prev, const char* digits) {
    const int n = (int) strlen(digits);
    const DashSpeedRow r = DashSpeedLayout(n);
    int i;
    if (n > 4 || (int) strlen(prev) != n) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        if (prev[i] != digits[i]) {
            const char one[2] = {digits[i], '\0'};
            const int x = r.x + i * r.adv;
            DashFillRect(fb, x, r.y, r.adv, r.h - 1, 0x0000u);
            DashPaintSpeedDigits(fb, one, &r, x);
        }
    }
    return 1;
}

/* COORDINATOR FIX (hw bug n60-3d7, item 4): the speed readout (see sDashWidgets' own "speed"
 * row). Unlike lap/timer/gap, this box has no baked archive texture to redraw a value row
 * into -- it didn't exist before the header/energy_bar swap opened this gap -- so chrome and
 * value are both repainted here, every tick, same style as DashPaintModeLabel's chip (panel
 * fill 0x3885u, border 0xFB76u) rather than the darker 0x1041u value-row fill the baked boxes
 * use, so it reads as "the same family as MODE/MENU", not a mismatched fifth look. Font2 (not
 * the digit-strip kDashGlyphTimer, which has no letters) so "237 KPH" is one string, sized to
 * fit the 62px box the same way DashPaintModeLabel fits its label.
 *
 * COORDINATOR FIX (hardware feedback round, "also"): "dash-speed-box is x=260 w=62 -> 2px past
 * the right edge (322); fix." Table-driven now (DASH_SPEED_X0/W read sDashWidgets' "speed" row,
 * x=258 w=62 -> right edge 320, the screen's own width) instead of inheriting BOOST's old x0.
 * card-4b: no Font2 "KPH" chip any more -- the top screen's digits and km/h on a black box at the
 * head of the right column (DashPaintSpeedRow, above). */
static void DashPaintSpeed(uint16_t* fb, const GdxRaceHudSnapshot* snap) {
    char buf[16];
    char prev[8] = "";
    if (!sDashWidgets[DASHW_SPEED].enabled) {
        return;
    }
    snprintf(buf, sizeof(buf), "%d", snap->speedKph < 0 ? 0 : snap->speedKph > 9999 ? 9999 : snap->speedKph);
    if (sMemoValid[DASHW_SPEED] && sMemoMode == sPlacementMode) {
        memcpy(prev, sMemoKey[DASHW_SPEED], sizeof(prev) - 1);
        prev[sizeof(prev) - 1] = '\0';
    }
    if (DashMemoSame(DASHW_SPEED, buf)) {
        return;
    }
    if (DashPaintSpeedChangedDigits(fb, prev, buf)) {
        return;
    }
    /* the top screen's black box; the chip border goes on last (the 2x row spans the box) */
    DashFillRect(fb, DASH_SPEED_X0 + 1, DASH_SPEED_Y0 + 1, DASH_SPEED_W - 2, DASH_SPEED_H - 2, 0x0000u);
    DashPaintSpeedRow(fb, buf);
    DashStrokeRect(fb, DASH_SPEED_X0, DASH_SPEED_Y0, DASH_SPEED_W, DASH_SPEED_H, 0xFB76u);
}

/* COORDINATOR FIX (third round item 3): "verify the player's position... If it's wrong, fix the
 * source... Then remove that 'P30/30' debug string entirely; the big place numeral already
 * covers it." This IS that big numeral: real aPosition<N>Tex digit(s) + an
 * aPositionOrdinalSuffix<X>Tex, replacing the small P<n>/<n> text in the now-fully-unbaked
 * "portrait" box. Single digit (positions 1-9) draws at native num=1,den=1 (28x32 digit + 20x20
 * suffix side by side = 48px, fits the box with room); two digits (10+) halve to num=1,den=2 so
 * both digits plus the suffix (14+14+10=38px) still fit.
 *
 * COORDINATOR FIX (hardware feedback round, item 3): "has a black background that overflows onto
 * the mini map." Root cause: this box's own fill rect used to be exactly 64px wide (x=0..63),
 * landing its right edge on column 63 -- the SAME column DashPaintMapBox's white border stroke
 * occupies for its full height (DASH_MAP_X0 - 1 == 63 before the hardware-feedback-round map
 * shrink). This widget's fill paints every tick (DashPaintMapBox's own throttled redraw does
 * not), so it was repeatedly overwriting one column of the map's own border with black. Fixed at
 * the source: sDashWidgets' own "portrait" row is now 63px wide, not 64 -- see that table entry's
 * own comment. (The map box no longer sits at that same column after its own resize/recentre
 * either, so this is now doubly clear of it; the width fix stays, since a future custom-HUD drag
 * could move the map box back adjacent to this one.) */
static void DashPaintOurPosition(uint16_t* fb, const GdxRaceHudSnapshot* snap) {
    char digits[4];
    const char* suffixKey;
    int n;
    int den;
    int x;
    const int y0 = DASH_PORTRAIT_Y0;
    int i;
    if (!sDashWidgets[DASHW_PORTRAIT].enabled) {
        return;
    }
    n = snprintf(digits, sizeof(digits), "%d", snap->position);
    if (DashMemoSame(DASHW_PORTRAIT, digits)) {
        return;
    }
    den = (n >= 2) ? 2 : 1;
    x = DASH_PORTRAIT_X0 + 2;

    DashFillRect(fb, DASH_PORTRAIT_X0, DASH_PORTRAIT_Y0, DASH_PORTRAIT_W, DASH_PORTRAIT_H, 0x1041u);
    if (n <= 0) {
        return;
    }
    for (i = 0; i < n; i++) {
        char key[2] = {digits[i], '\0'};
        const DashGlyph* digit = DashFindGlyph(kDashGlyphPosDigit, KDASHGLYPHPOSDIGIT_COUNT, key);
        if (digit == NULL) {
            continue;
        }
        DashBlitGlyphRatio(fb, x, y0 + (DASH_PORTRAIT_H - digit->h / den) / 2, digit, 1, den);
        x += digit->w / den + 1;
    }
    switch (snap->position % 100) {
        case 11: case 12: case 13: suffixKey = "TH"; break;
        default:
            switch (snap->position % 10) {
                case 1: suffixKey = "ST"; break;
                case 2: suffixKey = "ND"; break;
                case 3: suffixKey = "RD"; break;
                default: suffixKey = "TH"; break;
            }
    }
    {
        const DashGlyph* suffix = DashFindGlyph(kDashGlyphPosSuffix, KDASHGLYPHPOSSUFFIX_COUNT, suffixKey);
        if (suffix != NULL) {
            DashBlitGlyphRatio(fb, x, y0 + (DASH_PORTRAIT_H - suffix->h / den) / 2, suffix, 1, den);
        }
    }
}

/* COORDINATOR FIX (third round item 4): "the MODE chip is clipped: only 'TOP' and a green
 * square are visible. Show the full mode name." Real Font2 glyphs, best-fit num (2 or 1) so
 * "MODE 1"/"MODE 2"/"MODE 3" (the same labels docs/research/native/nibottom-mockup.html's own
 * MODE_CHIP_LABELS draws) always reads in full inside the 62x16 chip. */
static const char* DashModeChipLabel(int mode) {
    switch (mode) {
        case GDX_HUD_PLACEMENT_SPLIT: return "MODE 2";
        case GDX_HUD_PLACEMENT_ALL_BOTTOM: return "MODE 3";
        case GDX_HUD_PLACEMENT_CUSTOM: return "MODE Z"; /* the brief's own suggested label */
        default: return "MODE 1";
    }
}

static void DashPaintModeLabel(uint16_t* fb, const GdxRaceHudSnapshot* snap) {
    const char* label;
    int maxW;
    int num;
    int tw;
    int x;
    int y;
    (void) snap; /* unused -- present only to match sDashWidgets' uniform paint-fn signature */
    if (!sDashWidgets[DASHW_MODE_CHIP].enabled) {
        return;
    }
    label = DashModeChipLabel(sPlacementMode);
    if (DashMemoSame(DASHW_MODE_CHIP, label)) {
        return;
    }
    maxW = DASH_MODE_W - 12; /* leave room for the colour dot at the chip's right edge */
    num = DashFitFont2Num(label, maxW, 1);
    tw = DashFontTextWidthRatio(kDashGlyphFont2, KDASHGLYPHFONT2_COUNT, label, num, 1, DASH_FONT2_SPACE, 1);
    x = DASH_MODE_X0 + 4;
    y = DASH_MODE_Y0 + (DASH_MODE_H - (8 * num)) / 2;
    (void) tw;
    DashFillRect(fb, DASH_MODE_X0 + 1, DASH_MODE_Y0 + 1, DASH_MODE_W - 2, DASH_MODE_H - 2, 0x3885u);
    DashBlitFontText(fb, x, y, kDashGlyphFont2, KDASHGLYPHFONT2_COUNT, label, num, 1, DASH_FONT2_SPACE, 1);
}

/* ---- CUSTOM HUD mode: generic widget paint, sample data, reset/restore, touch handling --- */

/* "Show sample values if no race is live": built whenever CUSTOM/edit mode is entered without a
 * live race (gdx3ds_menu.c's "HUDX" debug tab is the only way that happens -- see
 * gdx3ds_bottomhud_enter_custom_edit). Plausible mid-race numbers, nothing more; this is display
 * data only, never fed back into any game state. */
static void DashBuildSampleSnapshot(GdxRaceHudSnapshot* snap) {
    int i;
    memset(snap, 0, sizeof(*snap));
    snap->valid = 1;
    snap->numPlayers = 1;
    snap->position = 3;
    snap->totalRacers = 30;
    snap->racersRemaining = 27;
    snap->speedKph = 184;
    snap->lap = 2;
    snap->totalLaps = 3;
    snap->character = 0;
    snap->energyMilli = 620;
    snap->boostReady = 0;
    snap->koCount = 3;
    snap->lapTimeMs = 42180;
    snap->bestTimeMs = 41530;
    snap->gapMs = 1250;
    snap->top5Count = GDX_RACEHUD_TOP5_MAX;
    for (i = 0; i < GDX_RACEHUD_TOP5_MAX; i++) {
        snap->top5Character[i] = i;
    }
    snprintf(snap->trackName, sizeof(snap->trackName), "SAMPLE TRACK");
}

static const char* DashWidgetShortLabel(int id) {
    switch (id) {
        case DASHW_ENERGY_BAR: return "ENERGY";
        case DASHW_PORTRAIT: return "RANK";
        case DASHW_STANDINGS: return "TOP5";
        case DASHW_KO_STARS: return "KO";
        case DASHW_LAP: return "LAP";
        case DASHW_TIMER: return "TIME";
        case DASHW_GAP: return "GAP";
        case DASHW_SPEED: return "SPEED";
        case DASHW_MAP_BOX: return "MAP";
        default: return "?";
    }
}

/* One short live-value string per editable widget, in the same format each widget's own real
 * (non-CUSTOM) paint function already uses -- DashFormatClock/DashFormatInterval, the lap
 * clamp, etc. -- just rendered through the generic Font2 box instead of that widget's own
 * digit-strip glyphs (which assume a fixed baked box size CUSTOM mode does not have). */
static void DashFormatCustomValue(char* out, size_t cap, int id, const GdxRaceHudSnapshot* snap) {
    switch (id) {
        case DASHW_ENERGY_BAR:
            snprintf(out, cap, "%d%%", snap->energyMilli / 10);
            break;
        case DASHW_PORTRAIT:
            snprintf(out, cap, "%d", snap->position);
            break;
        case DASHW_STANDINGS:
            snprintf(out, cap, "TOP %d", snap->top5Count);
            break;
        case DASHW_KO_STARS:
            snprintf(out, cap, "X%d", snap->koCount);
            break;
        case DASHW_LAP: {
            int lapDisp = snap->lap;
            if (snap->totalLaps > 0 && lapDisp > snap->totalLaps) {
                lapDisp = snap->totalLaps;
            }
            snprintf(out, cap, "%d/%d", lapDisp, snap->totalLaps);
            break;
        }
        case DASHW_TIMER: {
            char c[16];
            DashFormatClock(c, sizeof(c), snap->lapTimeMs);
            snprintf(out, cap, "%s", c);
            break;
        }
        case DASHW_GAP:
            DashFormatInterval(out, cap, snap->gapMs);
            break;
        case DASHW_SPEED:
            snprintf(out, cap, "%d KPH", snap->speedKph);
            break;
        default:
            if (cap > 0) {
                out[0] = '\0';
            }
    }
}

/* The generic box every editable widget draws as in CUSTOM mode: flat fill, border (brighter
 * white when `selected`, the usual chip pink otherwise), a short label centred at the top, and
 * (room permitting) a live value centred at the bottom. Replaces that widget's own fancy baked-
 * art paint function entirely in this mode -- see the block comment above sCustomWidgets. */
static void DashPaintCustomWidget(uint16_t* fb, int id, const GdxRaceHudSnapshot* snap, int selected) {
    const DashWidgetRect* r = &sCustomWidgets[id];
    const char* label = DashWidgetShortLabel(id);
    char valueBuf[16];
    int num;
    int tw;
    /* BUG FIX (found via this round's own verification capture -- see the report): fitting the
     * label and the value text EACH independently by width alone let a wide-but-short widget
     * (e.g. the default full-width ENERGY_BAR, h=24) pick num=2 for both, which are then 16px
     * tall each -- the label's top-anchored row and the value's bottom-anchored row overlapped
     * by more than half their height inside a 24px box, garbling both ("EN[glitch]RGY"). rowNum
     * caps whichever size DashFitFont2Num picks so two 8px-tall rows (label then value) plus a
     * 2px margin on every edge ALWAYS fit vertically before either one is ever asked to draw at
     * num=2; a short widget silently falls back to the small, always-safe num=1 for both. */
    const int rowNum = (r->h >= 2 * 16 + 6) ? 2 : 1;
    if (!r->enabled) {
        return;
    }
    DashFillRect(fb, r->x + 1, r->y + 1, r->w - 2, r->h - 2, 0x3885u);
    DashStrokeRect(fb, r->x, r->y, r->w, r->h, selected ? 0xFFFFu : 0xFB76u);

    num = DashFitFont2Num(label, r->w - 4, 1);
    if (num > rowNum) {
        num = rowNum;
    }
    tw = DashFontTextWidthRatio(kDashGlyphFont2, KDASHGLYPHFONT2_COUNT, label, num, 1, DASH_FONT2_SPACE, 1);
    DashBlitFontText(fb, r->x + (r->w - tw) / 2, r->y + 2, kDashGlyphFont2, KDASHGLYPHFONT2_COUNT, label, num, 1,
                      DASH_FONT2_SPACE, 1);

    DashFormatCustomValue(valueBuf, sizeof(valueBuf), id, snap);
    if (valueBuf[0] != '\0' && r->h >= 24) {
        int vnum = DashFitFont2Num(valueBuf, r->w - 4, 1);
        int vtw;
        if (vnum > rowNum) {
            vnum = rowNum;
        }
        vtw = DashFontTextWidthRatio(kDashGlyphFont2, KDASHGLYPHFONT2_COUNT, valueBuf, vnum, 1, DASH_FONT2_SPACE, 1);
        DashBlitFontText(fb, r->x + (r->w - vtw) / 2, r->y + r->h - 8 * vnum - 2, kDashGlyphFont2,
                          KDASHGLYPHFONT2_COUNT, valueBuf, vnum, 1, DASH_FONT2_SPACE, 1);
    }
}

/* Edit-mode-only overlay, drawn AFTER every widget box so its handles always sit on top: a red
 * close "X" at the top-right corner (every visible editable widget, not just the selected one --
 * closing something should never require selecting it first) and, for resizable widgets only, a
 * cyan resize handle at the bottom-right corner. */
static void DashPaintEditHandles(uint16_t* fb, int id) {
    const DashWidgetRect* r = &sCustomWidgets[id];
    if (!r->enabled) {
        return;
    }
    DashFillRect(fb, r->x + r->w - GDX_HUDCUSTOM_HANDLE - 1, r->y + 1, GDX_HUDCUSTOM_HANDLE, GDX_HUDCUSTOM_HANDLE,
                 0xF800u);
    DashBlitFontText(fb, r->x + r->w - GDX_HUDCUSTOM_HANDLE, r->y + 1, kDashGlyphFont2, KDASHGLYPHFONT2_COUNT, "X", 1,
                      1, 0, 0);
    if (DashWidgetResizable(id)) {
        DashFillRect(fb, r->x + r->w - GDX_HUDCUSTOM_HANDLE, r->y + r->h - GDX_HUDCUSTOM_HANDLE,
                     GDX_HUDCUSTOM_HANDLE, GDX_HUDCUSTOM_HANDLE, 0x07FFu);
    }
}

/* The hidden-widget tray: one chip per currently-disabled editable widget, left to right. Loop
 * shape (which widgets, which order, which x) is duplicated in DashCustomHandleTouch's own tray
 * hit-test on purpose -- both are short, both are commented, and keeping them as two small
 * mirrored loops is clearer here than threading a shared index lookup through both a paint call
 * and a touch call for a <= 9-item list. */
static void DashPaintTray(uint16_t* fb) {
    int i;
    int x = 2;
    for (i = 0; i < DASHW_COUNT; i++) {
        if (!DashWidgetEditable(i) || sCustomWidgets[i].enabled) {
            continue;
        }
        DashFillRect(fb, x, DASH_TRAY_Y0, DASH_TRAY_CHIP_W, DASH_TRAY_CHIP_H, 0x2104u);
        DashStrokeRect(fb, x, DASH_TRAY_Y0, DASH_TRAY_CHIP_W, DASH_TRAY_CHIP_H, 0x07E0u);
        DashBlitFontText(fb, x + 2, DASH_TRAY_Y0 + 3, kDashGlyphFont2, KDASHGLYPHFONT2_COUNT,
                          DashWidgetShortLabel(i), 1, 1, 0, 0);
        x += DASH_TRAY_CHIP_W + DASH_TRAY_CHIP_GAP;
    }
}

/* RESET + the EDIT<->DONE toggle, drawn every tick CUSTOM mode is active (cheap: two small
 * chips), in the header's one remaining free gap. */
static void DashPaintCustomChrome(uint16_t* fb) {
    static const char kReset[] = "RESET";
    const char* editLabel = sEditMode ? "DONE" : "EDIT";
    int tw;

    DashFillRect(fb, DASH_RESET_X0 + 1, DASH_RESET_Y0 + 1, DASH_RESET_W - 2, DASH_RESET_H - 2, 0x3885u);
    DashStrokeRect(fb, DASH_RESET_X0, DASH_RESET_Y0, DASH_RESET_W, DASH_RESET_H, 0xFB76u);
    tw = DashFontTextWidthRatio(kDashGlyphFont2, KDASHGLYPHFONT2_COUNT, kReset, 1, 1, DASH_FONT2_SPACE, 1);
    DashBlitFontText(fb, DASH_RESET_X0 + (DASH_RESET_W - tw) / 2, DASH_RESET_Y0 + 4, kDashGlyphFont2,
                      KDASHGLYPHFONT2_COUNT, kReset, 1, 1, DASH_FONT2_SPACE, 1);

    DashFillRect(fb, DASH_EDIT_X0 + 1, DASH_EDIT_Y0 + 1, DASH_EDIT_W - 2, DASH_EDIT_H - 2, 0x3885u);
    DashStrokeRect(fb, DASH_EDIT_X0, DASH_EDIT_Y0, DASH_EDIT_W, DASH_EDIT_H, sEditMode ? 0xFFFFu : 0xFB76u);
    tw = DashFontTextWidthRatio(kDashGlyphFont2, KDASHGLYPHFONT2_COUNT, editLabel, 1, 1, DASH_FONT2_SPACE, 1);
    DashBlitFontText(fb, DASH_EDIT_X0 + (DASH_EDIT_W - tw) / 2, DASH_EDIT_Y0 + 4, kDashGlyphFont2,
                      KDASHGLYPHFONT2_COUNT, editLabel, 1, 1, DASH_FONT2_SPACE, 1);
}

/* RESET: restores every editable widget to its default (ALL_BOTTOM) rect, all visible, clears
 * any selection/drag/resize in progress, and persists immediately -- the brief's own "RESET
 * restoring the default layout with everything on the bottom". */
static void DashCustomReset(void) {
    DashCustomDefaults(sCustomWidgets);
    sEditSelected = -1;
    sEditDragging = 0;
    sEditResizing = 0;
    DashHudIniSave();
    DashSyncCustomTopMask();
    DashLog("[dash] custom layout reset to defaults");
}

static void DashCustomToggleEdit(void) {
    sEditMode = !sEditMode;
    sEditDragging = 0;
    sEditResizing = 0;
    if (!sEditMode) {
        DashHudIniSave();
        DashSyncCustomTopMask();
        DashLog("[dash] custom edit -> DONE, layout saved");
    } else {
        sEditSelected = -1;
        DashLog("[dash] custom edit -> begin");
    }
}

/* All CUSTOM-mode touch handling for one tick: RESET/EDIT-DONE buttons (always live while in
 * CUSTOM, edit mode or not), then -- edit mode only -- the hidden-widget tray, each visible
 * widget's close "X" / resize handle / body (in that priority order, first match wins, same
 * rule gdx_hudcustom_hit_test_all documents), and finally drag/resize tracking while the touch
 * stays down. Called once per tick, BEFORE painting, so the same tick's paint already reflects
 * whatever just changed. */
static void DashCustomHandleTouch(int touching, int prevTouching, int touchPx, int touchPy) {
    if (touching && !prevTouching) {
        if (touchPx >= DASH_RESET_X0 && touchPx < DASH_RESET_X0 + DASH_RESET_W && touchPy >= DASH_RESET_Y0 &&
            touchPy < DASH_RESET_Y0 + DASH_RESET_H) {
            DashCustomReset();
            return;
        }
        if (touchPx >= DASH_EDIT_X0 && touchPx < DASH_EDIT_X0 + DASH_EDIT_W && touchPy >= DASH_EDIT_Y0 &&
            touchPy < DASH_EDIT_Y0 + DASH_EDIT_H) {
            DashCustomToggleEdit();
            return;
        }
        if (!sEditMode) {
            return; /* nothing else is interactive in CUSTOM's plain view mode */
        }
        {
            int i;
            int x = 2;
            for (i = 0; i < DASHW_COUNT; i++) {
                if (!DashWidgetEditable(i) || sCustomWidgets[i].enabled) {
                    continue;
                }
                if (touchPx >= x && touchPx < x + DASH_TRAY_CHIP_W && touchPy >= DASH_TRAY_Y0 &&
                    touchPy < DASH_TRAY_Y0 + DASH_TRAY_CHIP_H) {
                    sCustomWidgets[i].enabled = 1;
                    sEditSelected = i;
                    return;
                }
                x += DASH_TRAY_CHIP_W + DASH_TRAY_CHIP_GAP;
            }
        }
        {
            GdxHudCustomRect rects[DASHW_COUNT];
            unsigned resizableMask = 0;
            int i;
            int hitIndex = -1;
            GdxHudCustomHit hit = GDX_HUDCUSTOM_HIT_NONE;
            for (i = 0; i < DASHW_COUNT; i++) {
                rects[i].x = sCustomWidgets[i].x;
                rects[i].y = sCustomWidgets[i].y;
                rects[i].w = sCustomWidgets[i].w;
                rects[i].h = sCustomWidgets[i].h;
                rects[i].enabled = DashWidgetEditable(i) ? sCustomWidgets[i].enabled : 0;
                if (DashWidgetResizable(i)) {
                    resizableMask |= (1u << (unsigned) i);
                }
            }
            if (gdx_hudcustom_hit_test_all(rects, DASHW_COUNT, resizableMask, touchPx, touchPy, &hitIndex, &hit)) {
                DashWidgetRect* r = &sCustomWidgets[hitIndex];
                sEditSelected = hitIndex;
                sEditDragging = 0;
                sEditResizing = 0;
                switch (hit) {
                    case GDX_HUDCUSTOM_HIT_CLOSE:
                        r->enabled = 0;
                        sEditSelected = -1;
                        break;
                    case GDX_HUDCUSTOM_HIT_RESIZE:
                        sEditResizing = 1;
                        break;
                    case GDX_HUDCUSTOM_HIT_BODY:
                    default:
                        sEditDragging = 1;
                        sEditDragOffX = touchPx - r->x;
                        sEditDragOffY = touchPy - r->y;
                        break;
                }
                return;
            }
        }
        sEditSelected = -1; /* empty space: deselect */
        return;
    }

    if (!touching) {
        sEditDragging = 0;
        sEditResizing = 0;
        return;
    }

    if (sEditMode && sEditSelected >= 0 && DashWidgetEditable(sEditSelected)) {
        DashWidgetRect* r = &sCustomWidgets[sEditSelected];
        if (sEditDragging) {
            GdxHudCustomRect rc;
            rc.x = gdx_hudcustom_snap(touchPx - sEditDragOffX, 8);
            rc.y = gdx_hudcustom_snap(touchPy - sEditDragOffY, 8);
            rc.w = r->w;
            rc.h = r->h;
            rc.enabled = 1;
            gdx_hudcustom_clamp_rect(&rc, DashWidgetMinW(sEditSelected), DashWidgetMinH(sEditSelected),
                                      DASH_HEADER_Y0);
            r->x = rc.x;
            r->y = rc.y;
        } else if (sEditResizing) {
            GdxHudCustomRect rc;
            const int step = DashWidgetResizeStep(sEditSelected);
            rc.x = r->x;
            rc.y = r->y;
            rc.w = gdx_hudcustom_snap(touchPx - r->x, step);
            rc.h = gdx_hudcustom_snap(touchPy - r->y, step);
            rc.enabled = 1;
            gdx_hudcustom_clamp_rect(&rc, DashWidgetMinW(sEditSelected), DashWidgetMinH(sEditSelected),
                                      DASH_HEADER_Y0);
            r->w = rc.w;
            r->h = rc.h;
        }
    }
}

/* ---- SHOT (same format/writer shape as gdx3ds_racemap.c's own, independent instance) --- */

static long DashWriteShot(const char* path, const uint16_t* fb) {
    const unsigned rowBytes = (DASH_FB_W * 3u + 3u) & ~3u;
    const uint32_t imageBytes = rowBytes * DASH_FB_H;
    const uint32_t fileBytes = 54u + imageBytes;
    static unsigned char sWriteBuf[8u * 1024u];
    unsigned char header[54];
    unsigned char row[DASH_FB_W * 3u + 3u];
    FILE* f;
    unsigned y;

    mkdir("autotest", 0777);
    f = fopen(path, "wb");
    if (f == NULL) {
        return -1;
    }
    setvbuf(f, (char*) sWriteBuf, _IOFBF, sizeof(sWriteBuf));
    memset(header, 0, sizeof(header));
    header[0] = 'B';
    header[1] = 'M';
    header[2] = (unsigned char) (fileBytes);
    header[3] = (unsigned char) (fileBytes >> 8);
    header[4] = (unsigned char) (fileBytes >> 16);
    header[5] = (unsigned char) (fileBytes >> 24);
    header[10] = 54;
    header[14] = 40;
    header[18] = (unsigned char) (DASH_FB_W & 0xFF);
    header[19] = (unsigned char) (DASH_FB_W >> 8);
    header[22] = (unsigned char) (DASH_FB_H & 0xFF);
    header[23] = (unsigned char) (DASH_FB_H >> 8);
    header[26] = 1;
    header[28] = 24;
    fwrite(header, 1, sizeof(header), f);
    for (y = 0; y < DASH_FB_H; y++) {
        const unsigned sy = DASH_FB_H - 1u - y;
        unsigned x;
        memset(row, 0, rowBytes);
        for (x = 0; x < DASH_FB_W; x++) {
            const uint16_t p = fb[(size_t) x * DASH_FB_H + (DASH_FB_H - 1u - sy)];
            const unsigned r = (p >> 11) & 31u;
            const unsigned g = (p >> 5) & 63u;
            const unsigned b = p & 31u;
            row[x * 3u + 0] = (unsigned char) ((b << 3) | (b >> 2));
            row[x * 3u + 1] = (unsigned char) ((g << 2) | (g >> 4));
            row[x * 3u + 2] = (unsigned char) ((r << 3) | (r >> 2));
        }
        fwrite(row, 1, rowBytes, f);
    }
    fclose(f);
    return (long) fileBytes;
}

static void DashServiceShot(const uint16_t* fb) {
    char path[80];
    long bytes;
    if (!__atomic_exchange_n(&sShotArmed, 0, __ATOMIC_ACQ_REL)) {
        return;
    }
    snprintf(path, sizeof(path), "autotest/%s_R.bmp", sPendingShot);
    bytes = DashWriteShot(path, fb);
    if (bytes < 0) {
        DashLog("[dash] SHOT write failed: %s", path);
    } else {
        DashLog("[dash] SHOT captured -> %s bytes=%ld", path, bytes);
    }
}

/* ---- the one call site: touch + paint, every tick the takeover is active -------------- */

/* Repaint everything from the backdrop up on this tick (the multiplayer dash's overlay closed, or
 * a stall banner / toast went away). */
static void DashRebake(void) {
    sWasValid = 0;
    sLastHeaderRacersRemaining = -1;
    sLastHeaderTrackName[0] = '\0';
}

/* The multiplayer standings overlay: 1 = it owned this tick (painted and presented itself). */
/* o3dsperf [ui] dash_every: is this host iteration's repaint due? Always on the first tick after
 * a bake / overlay (a whole-frame present is pending) and while the screen is touched. */
/* o3ds60 dash_spread: steady frameskip runs a render on every tick (D = 1). hw 5c24be0: the
 * "more than 8 ticks without a skip" test alone left the first 8 ticks of every D = 1 stint on
 * the skip-waiting rule, which then paints every widget at once (dash max_us 7125) on a rendered
 * tick -- right when the governor is judging whether D = 1 holds. */
static int DashFrameskipAtD1(void) {
    int d = 0;
    int late = 0;
    gdx3ds_frameskip_hitch_info(&d, &late);
    if (d == 1) {
        return 1;
    }
    return DashRace0EveryTick();
}

static int DashPaintDue(int touching, int* phase) {
    const unsigned long skipped = gdx3ds_frameskip_skipped_total();
    const int thisSkipped = skipped != sDashPrevSkipped;
    sDashPrevSkipped = skipped;
    if (thisSkipped) {
        sDashSinceSkip = 0;
    } else if (sDashSinceSkip < 255u) {
        sDashSinceSkip++;
    }
    sDashSince++;
    *phase = -1;
    if (sDashEvery <= 1 || sDirtyFull || touching || sEditMode) {
        sDashSince = 0;
        return 1;
    }
    if (sDashSpread != 0 && (sDashSinceSkip > 8u || (sDashSpread == 1 && DashFrameskipAtD1()))) {
        /* o3ds60 [ui] dash_spread: no skip iteration lately, or steady already runs at D = 1
         * (the race draws every tick from now on: no full paint at the start of a D = 1 stint) */
        sDashPhaseN = (sDashEveryD1 > 1 && DashFrameskipAtD1()) ? (unsigned) sDashEveryD1 : (unsigned) sDashEvery;
        *phase = (int) (sDashPhaseNext % sDashPhaseN);
        sDashPhaseNext++;
        sDashSince = 0;
        return 1;
    }
    if (sDashSince < (unsigned) sDashEvery) {
        return 0;
    }
    /* Due. On a rendered iteration of a race that is skipping frames, wait (up to another N
     * iterations) for the next skip iteration: its frame has the room. */
    if (!thisSkipped && sDashSinceSkip <= 8u && sDashSince < 2u * (unsigned) sDashEvery) {
        return 0;
    }
    sDashSince = 0;
    return 1;
}

/* dash_spread: does this paint (phase -1 = every widget) include paint-order entry `oi`? */
static int DashPhaseHas(int phase, unsigned oi) {
    if (phase < 0 || sDashEvery <= 1) {
        return 1;
    }
    return (int) (oi % (sDashPhaseN > 0 ? sDashPhaseN : 1u)) == phase;
}

static unsigned DashOrderIndex(DashWidgetId id) {
    unsigned oi;
    for (oi = 0; oi < (unsigned) DASH_PAINT_ORDER_COUNT; oi++) {
        if (kDashPaintOrder[oi] == id) {
            return oi;
        }
    }
    return 0;
}

/* The shadow's painted columns -> the live buffer (one contiguous block: the buffer is
 * column-major), or the whole frame after a bake, a new live buffer, or every
 * DASH_FULL_PRESENT_EVERY presents. */
/* o3ds60 dash_budget: copy each run of painted columns (the column-major buffer makes a run one
 * contiguous block), plus this present's self-heal slice. */
/* o3ds60 dash_map_inc: the marked rows of each column not presented whole. */
static size_t DashPresentRows(uint16_t* realFb, const uint16_t* fb) {
    size_t bytes = 0;
    int x;
    for (x = 0; x < DASH_FB_W; x++) {
        const int y0 = sRowY0[x];
        const int y1 = sRowY1[x];
        size_t off;
        size_t n;
        if (y1 < y0 || (sDirtyColBits[x >> 5] & (1u << (x & 31))) != 0) {
            continue;
        }
        off = (size_t) x * DASH_FB_H + (size_t) (DASH_FB_H - 1 - y1);
        n = (size_t) (y1 - y0 + 1) * sizeof(uint16_t);
        memcpy(realFb + off, fb + off, n);
        bytes += n;
    }
    return bytes;
}

static size_t DashPresentRuns(uint16_t* realFb, const uint16_t* fb) {
    const int sliceW = DASH_FB_W / 8;
    int healX0 = -1;
    size_t bytes = 0;
    int x;
    if (++sSinceFullPresent >= DASH_FULL_PRESENT_EVERY / 8u) {
        healX0 = (int) (sHealSlice % 8u) * sliceW;
        sHealSlice++;
        sSinceFullPresent = 0;
        DashMarkCols(healX0, sliceW);
    }
    if (sRowAny) {
        bytes += DashPresentRows(realFb, fb);
    }
    if (sDirtyX1 < sDirtyX0) {
        return bytes;
    }
    x = sDirtyX0;
    while (x <= sDirtyX1) {
        int run = x;
        size_t off;
        size_t n;
        if ((sDirtyColBits[x >> 5] & (1u << (x & 31))) == 0) {
            x++;
            continue;
        }
        while (run + 1 <= sDirtyX1 && (sDirtyColBits[(run + 1) >> 5] & (1u << ((run + 1) & 31))) != 0) {
            run++;
        }
        off = (size_t) x * DASH_FB_H;
        n = (size_t) (run - x + 1) * DASH_FB_H * sizeof(uint16_t);
        memcpy(realFb + off, fb + off, n);
        bytes += n;
        x = run + 1;
    }
    return bytes;
}

static void DashPresent(uint16_t* realFb, const uint16_t* fb) {
    const u64 t0 = svcGetSystemTick();
    size_t bytes = 0;
    if (realFb != sDashLastLive) {
        sDashLastLive = realFb;
        sDirtyFull = 1;
    }
    if (!sDashMemo && sDashEvery <= 1) {
        sDirtyFull = 1; /* both levers off: exactly the old whole-frame copy every tick */
    }
    if (!sDashBudget && sDirtyX1 >= sDirtyX0 && ++sSinceFullPresent >= DASH_FULL_PRESENT_EVERY) {
        sDirtyFull = 1;
    }
    if (sDirtyFull && !sMapFullThisTick) {
        sMapIncValid = 0; /* o3ds60 dash_map_inc: the shadow may have changed under the box */
    }
    sMapFullThisTick = 0;
    if (sDirtyFull) {
        bytes = sizeof(sShadowFb);
        memcpy(realFb, fb, bytes);
        sSinceFullPresent = 0;
        sTickCost.full = 1;
    } else if (sDashBudget) {
        bytes = DashPresentRuns(realFb, fb);
    } else if (sDirtyX1 >= sDirtyX0) {
        const size_t off = (size_t) sDirtyX0 * DASH_FB_H;
        bytes = (size_t) (sDirtyX1 - sDirtyX0 + 1) * DASH_FB_H * sizeof(uint16_t);
        memcpy(realFb + off, fb + off, bytes);
    }
    if (bytes != 0) {
        sDashPaints++;
        sDashPresentKb += (unsigned long) (bytes / 1024u);
    }
    sDirtyFull = 0;
    sDirtyX0 = DASH_FB_W;
    sDirtyX1 = -1;
    memset(sDirtyColBits, 0, sizeof(sDirtyColBits));
    sRowAny = 0;
    sTickCost.kb = (unsigned long) (bytes / 1024u);
    sTickCost.pres = svcGetSystemTick() - t0;
    if (sDashProf) {
        sProfPresent += sTickCost.pres;
    }
}

static unsigned long DashTicksToUs(u64 t, unsigned n) {
    if (n == 0) {
        return 0;
    }
    return (unsigned long) (((t / n) * 1000u) / (u64) CPU_TICKS_PER_MSEC);
}

/* [dashprof]: per-widget us per DASH tick over the last `n` ticks (0 = not painted / skipped). */
static void DashProfEmit(unsigned n) {
    if (sDashProf) {
        DashLog("[dashprof] n=%u every=%d memo=%d paints=%lu kb=%lu | hdr=%lu std=%lu map=%lu nrg=%lu pos=%lu "
                "mode=%lu ko=%lu lap=%lu tim=%lu gap=%lu spd=%lu bui=%lu pres=%lu",
                n, sDashEvery, sDashMemo, sDashPaints, sDashPresentKb, DashTicksToUs(sProfWidget[DASHW_HEADER], n),
                DashTicksToUs(sProfWidget[DASHW_STANDINGS], n), DashTicksToUs(sProfWidget[DASHW_MAP_BOX], n),
                DashTicksToUs(sProfWidget[DASHW_ENERGY_BAR], n), DashTicksToUs(sProfWidget[DASHW_PORTRAIT], n),
                DashTicksToUs(sProfWidget[DASHW_MODE_CHIP], n), DashTicksToUs(sProfWidget[DASHW_KO_STARS], n),
                DashTicksToUs(sProfWidget[DASHW_LAP], n), DashTicksToUs(sProfWidget[DASHW_TIMER], n),
                DashTicksToUs(sProfWidget[DASHW_GAP], n), DashTicksToUs(sProfWidget[DASHW_SPEED], n),
                DashTicksToUs(sProfBui, n), DashTicksToUs(sProfPresent, n));
    }
    memset(sProfWidget, 0, sizeof(sProfWidget));
    sProfBui = 0;
    sProfPresent = 0;
    sDashPaints = 0;
    sDashPresentKb = 0;
}

static int DashMpOverlayTick(uint16_t* fb, uint16_t* realFb, int touching, int touchPx, int touchPy) {
    if (gdx3ds_buirace_pause_tick(fb, realFb, touching, touchPx, touchPy)) {
        DashServiceShot(fb);
        sDirtyFull = 1; /* mp-menus #10: the pause card drew on the live buffer */
        return 1;
    }
    if (gdx3ds_buirace_overlay_tick(fb, realFb, touching)) {
        DashServiceShot(fb);
        sDirtyFull = 1; /* the overlay drew on the live buffer: the next present is whole */
        return 1;
    }
    if (gdx3ds_buirace_take_rebake()) {
        DashRebake();
    }
    return 0;
}

void gdx3ds_bottomhud_tick(unsigned short* fbIn, unsigned int held, unsigned int prevHeld, int touching,
                            int prevTouching, int touchPx, int touchPy) {
    uint16_t* const realFb = (uint16_t*) fbIn; /* the live, on-screen, single-buffered console fb */
    uint16_t* fb = sShadowFb;                  /* every Dash* paint call below targets this instead
                                                 * -- see sShadowFb's own comment (hw bug n60-3d7 #1) */
    GdxRaceHudSnapshot snap;
    const u64 now = svcGetSystemTick();

    if (realFb == NULL) {
        return;
    }
    sLiveFb = realFb;
    sMpDash = sBuiOn && sPlacementMode != GDX_HUD_PLACEMENT_CUSTOM && gdx3ds_buirace_active();
    gdx_racehud_snapshot(&snap);
    if (!snap.valid) {
        if (!sEditMode) {
            return; /* the caller already checked race_live (or edit_active); nothing to show */
        }
        /* "Show sample values if no race is live": gdx3ds_menu.c's "HUDX" debug tab is the only
         * way sEditMode can be 1 here without a live race -- see gdx3ds_bottomhud_enter_custom_edit
         * and gdx3ds_bottomhud_edit_active's own takeover-gate OR in gdx3ds_menu.c. */
        DashBuildSampleSnapshot(&snap);
    }

    if (sMpDash && DashMpOverlayTick(fb, realFb, touching, touchPx, touchPy)) {
        return;
    }
    if (sPlacementMode != GDX_HUD_PLACEMENT_CUSTOM) {
        if (!sWasValid) {
            DashPaintBaked(fb);
            if (sMpDash) {
                gdx3ds_buirace_on_bake(fb);
            }
            sWasValid = 1;
            sLastTop5Count = -1;
            sLastMapTick = 0;
            sLastSpareMachines = DASH_LIVES_UNPAINTED;
            sFpsShown = -1;
            sFpsWindowStartTick = 0;
        }
    } else if (!sCustomBackdropPainted) {
        /* CUSTOM mode never reuses the baked pixel art (DashPaintBaked) -- widgets here can be
         * resized to any size, which baked art cannot stretch to -- so it gets its own flat,
         * once-per-entry backdrop plus the one piece of baked-style chrome it still needs: the
         * DEBUG button, painted once here exactly like DashPaintBaked paints it once per race. */
        DashFillRect(fb, 0, 0, DASH_FB_W, DASH_FB_H, 0x18C3u);
        DashPaintBackButton(fb, NULL);
        sCustomBackdropPainted = 1;
    }

    /* MODE chip: a touch-down inside its rect, or a fresh KEY_SELECT, both cycle the mode --
     * EXACTLY as before this round, for every mode including CUSTOM (see sEditMode's own
     * comment for why CUSTOM's edit-mode entry deliberately does NOT share this gesture). The
     * one addition is `&& !sEditMode`: while actively editing a CUSTOM layout, the chip (and
     * SELECT) do nothing, so an accidental tap/press can never strand an in-progress drag/resize
     * unsaved by cycling the mode out from under it -- DONE (DashPaintCustomChrome's EDIT/DONE
     * toggle) is the only way out of edit mode itself. */
    {
        const int insideChip = touching && touchPx >= DASH_MODE_X0 && touchPx < DASH_MODE_X0 + DASH_MODE_W &&
                                touchPy >= DASH_MODE_Y0 && touchPy < DASH_MODE_Y0 + DASH_MODE_H;
        const int chipDown = insideChip && !sPrevChipTouch;
        /* bottom_menus: SELECT pressed with L or R held is the dev-console combo starting, not a
         * mode cycle (the combo would otherwise move the top-screen HUD on its way in). */
        const int comboKeys = sBuiOn && (held & (KEY_L | KEY_R)) != 0;
        const int selectDown = (held & KEY_SELECT) && !(prevHeld & KEY_SELECT) && !comboKeys;
        if ((chipDown || selectDown) && !sEditMode) {
            gdx3ds_bottomhud_cycle_mode();
        }
        sPrevChipTouch = insideChip;
    }
    (void) prevTouching;

    /* SELECT long-press -> dismiss, the key-based twin of the DEBUG button just below. Tracked
     * here (not inside the DEBUG-button block) so it still counts even while the finger is on the
     * chip/elsewhere; reset on release, and pushed past the threshold the instant it fires so
     * holding SELECT through the dismiss can't immediately re-fire. */
    if ((held & KEY_SELECT) && !sBuiOn) {
        if (sSelectHoldTicks <= DASH_SELECT_LONGPRESS_TICKS) {
            sSelectHoldTicks++;
        }
    } else {
        sSelectHoldTicks = 0;
    }

    /* COORDINATOR FIX (item 4): the DEBUG button (originally labelled "MENU"; renamed in the
     * idle-view round -- see DashPaintBackButton) -- a touch-down inside its rect dismisses
     * the DASH takeover back to the ordinary debug tab UI (gdx3ds_menu.c's gate checks
     * gdx3ds_bottomhud_dismissed() every tick); the race keeps running underneath. Nothing else
     * this tick matters once that happens -- the ordinary debug tab UI owns the next tick's whole
     * repaint.
     * COORDINATOR FIX (item 5): a SELECT long-press does the exact same thing -- see
     * sSelectHoldTicks above. */
    {
        const int insideBack = touching && !sDebugHidden && !sMpDash && touchPx >= DASH_BACK_X0 &&
                                touchPx < DASH_BACK_X0 + DASH_BACK_W && touchPy >= DASH_BACK_Y0 &&
                                touchPy < DASH_BACK_Y0 + DASH_BACK_H;
        const int backDown = insideBack && !sPrevBackTouch;
        const int selectLongpress = sSelectHoldTicks == DASH_SELECT_LONGPRESS_TICKS;
        sPrevBackTouch = insideBack;
        if (backDown || selectLongpress) {
            sDismissed = 1;
            sEditMode = 0; /* no autosave on the way out -- see DONE/RESET's own comments */
            DashLog("[dash] dismissed -> debug tabs");
            /* Flush before bailing: the `!sWasValid` block above may have just painted the
             * once-per-race baked backdrop into the shadow buffer on this very tick (race start
             * and a DEBUG-button tap landing on the same tick) -- without this, that backdrop
             * would sit in sShadowFb forever and never reach the live screen, since sWasValid is
             * now 1 and DashPaintBaked never runs again this race. */
            memcpy(realFb, fb, sizeof(sShadowFb));
            return;
        }
    }

    if (sPlacementMode == GDX_HUD_PLACEMENT_CUSTOM) {
        /* CUSTOM mode's own touch handling + paint, entirely separate from the modes-1-3 path
         * below (see the block comment above sCustomWidgets for why: no baked art, generic
         * resizable boxes instead). Touch is handled BEFORE painting so this same tick's paint
         * already reflects whatever a drag/resize/close/tray tap just changed. */
        int i;
        sDirtyFull = 1; /* CUSTOM: the generic widgets move; present the whole frame */
        DashCustomHandleTouch(touching, prevTouching, touchPx, touchPy);
        DashPaintHeaderLive(fb, &snap);
        DashPaintModeLabel(fb, &snap); /* repaints the chip's interior -- the dot must follow it */
        DashPaintModeDot(fb);
        DashPaintCustomChrome(fb);
        for (i = 0; i < DASHW_COUNT; i++) {
            if (!DashWidgetEditable(i)) {
                continue;
            }
            DashPaintCustomWidget(fb, i, &snap, sEditMode && sEditSelected == i);
        }
        if (sEditMode) {
            for (i = 0; i < DASHW_COUNT; i++) {
                if (!DashWidgetEditable(i)) {
                    continue;
                }
                DashPaintEditHandles(fb, i);
            }
            DashPaintTray(fb);
        }
    } else {
        /* COORDINATOR FIX (custom-HUD groundwork, follow-up round): "convert tick() to loop the
         * widget table in a defined paint order, re-encoding the ordering/throttle couplings
         * explicitly." kDashPaintOrder (declared near sDashWidgets, with the full rationale) is
         * that order; each widget's own paint fn already decides internally whether it has
         * anything new to draw (dirty-checking for standings, the [debug] bottom_race_hz
         * throttle for the map box) -- exactly as cheap as the old hand-written call sequence
         * this loop replaces, and calling the exact same functions in the exact same order
         * produces byte-identical output for modes 1-3. The one coupling with no row of its own
         * in the table -- the MODE dot, which must follow DashPaintModeLabel's own repaint of the
         * chip's interior -- is re-encoded explicitly right here, not hidden inside the loop. */
        unsigned oi;
        int paintPhase = -1;
        const int paintNow = DashPaintDue(touching, &paintPhase);
        /* o3ds60 dash_budget: at steady D = 1 a tick paints until the budget is used; the
         * widgets still due carry to the next tick (painted first, whatever its phase). */
        const int budgetOn = sDashBudget && sDashEvery > 1 && !sDirtyFull && DashFrameskipAtD1();
        const u64 budget = (u64) sDashBudgetUs * (u64) CPU_TICKS_PER_MSEC / 1000u;
        const unsigned carry = budgetOn ? sDashCarry : 0u;
        sDashCarry = 0;
        sTickCost.d1 = budgetOn;
        if (paintNow || carry != 0u) {
            for (oi = 0; oi < (unsigned) DASH_PAINT_ORDER_COUNT; oi++) {
                const DashWidgetId id = kDashPaintOrder[oi];
                const unsigned bit = 1u << oi;
                const u64 w0 = svcGetSystemTick();
                if (!((paintNow && DashPhaseHas(paintPhase, oi)) || (carry & bit) != 0u)) {
                    continue; /* o3ds60 [ui] dash_spread: another iteration's widget */
                }
                if (sMpDash && (id == DASHW_STANDINGS || id == DASHW_PORTRAIT)) {
                    continue; /* the multiplayer column replaces both (gdx3ds_buirace_paint) */
                }
                if (budgetOn && w0 - now > budget) {
                    sDashCarry |= bit;
                    sTickCost.carried++;
                    continue;
                }
                sDashWidgets[id].paint(fb, &snap);
                if (id == DASHW_MODE_CHIP) {
                    DashPaintModeDot(fb);
                } else if (id == DASHW_ENERGY_BAR) {
                    DashPaintLives(fb, &snap); /* lives in the energy widget's own label column */
                }
                sTickCost.widgets++;
                if (id == DASHW_MAP_BOX) {
                    sTickCost.map += svcGetSystemTick() - w0;
                }
                if (sDashProf) {
                    sProfWidget[id] += svcGetSystemTick() - w0;
                }
            }
        }
        if (sFpsEnabled) {
            DashFpsSample(now); /* drawn by the map box's own repaint: DashPaintMapFpsText */
        }
        if (sMpDash) {
            gdx3ds_buirace_touch(touching, touchPx, touchPy);
            /* dash_spread: the multiplayer column takes the standings' phase (the column replaces
             * the standings widget, so that phase has the room) */
            const int buiDue = (paintNow && DashPhaseHas(paintPhase, DashOrderIndex(DASHW_STANDINGS))) ||
                               (carry & DASH_CARRY_BUI) != 0u;
            if (buiDue && budgetOn && svcGetSystemTick() - now > budget) {
                sDashCarry |= DASH_CARRY_BUI; /* o3ds60 dash_budget */
                sTickCost.carried++;
            } else if (buiDue) {
                const u64 b0 = svcGetSystemTick();
                int bx0;
                int bx1;
                bui_kit_dirty_clear();
                gdx3ds_buirace_paint(fb);
                if (bui_kit_dirty(&bx0, &bx1)) {
                    DashMarkCols(bx0, bx1 - bx0 + 1);
                }
                sTickCost.bui = svcGetSystemTick() - b0;
                sTickCost.widgets++;
                if (sDashProf) {
                    sProfBui += sTickCost.bui;
                }
            }
        }
    }

    /* The ONE write to the live, on-screen buffer this tick: the shadow buffer is now a
     * complete, consistent frame (every widget above painted into it, never into realFb
     * directly), so this single contiguous copy is the only moment the live buffer changes --
     * see sShadowFb's own comment (hw bug n60-3d7 #1) for why that's what kills the tearing.
     * Deliberately included inside the self-timing window below: it is real per-tick cost this
     * fix adds and the avg/max_us receipt should say so honestly. o3dsperf: only the columns
     * painted this tick (one contiguous block of the column-major buffer); the rest of the live
     * buffer already holds the same pixels. */
    DashPresent(realFb, fb);

    /* Self-timing window CLOSES HERE, before the SHOT write — see the sTicks/sTickTicksTotal
       comment above. */
    {
        const u64 dt = svcGetSystemTick() - now;
        sDashLastTickTicks = dt;
        sTickCost.total = dt;
        if (dt > sWorstTick.total) {
            sWorstTick = sTickCost;
        }
        memset(&sTickCost, 0, sizeof(sTickCost));
        sTicks++;
        sTickTicksTotal += dt;
        if (dt > sTickTicksMax) {
            sTickTicksMax = dt;
        }
        if ((sTicks % 256u) == 0u) {
            const unsigned long ticksPerMs = (unsigned long) CPU_TICKS_PER_MSEC;
            const unsigned long avgUs = (unsigned long) (((sTickTicksTotal / 256u) * 1000u) / ticksPerMs);
            const unsigned long maxUs = (unsigned long) ((sTickTicksMax * 1000u) / ticksPerMs);
            DashLog("[dash] ticks=%lu avg_us=%lu max_us=%lu", sTicks, avgUs, maxUs);
            DashLog("[dashmap] inc=%lu whole=%lu cache_builds=%lu (map_inc=%d: map ticks of the 256)", sMapIncPaints,
                    sMapFullPaints, sMapCacheBuilds, sDashMapInc);
            sMapIncPaints = 0;
            sMapFullPaints = 0;
            sMapCacheBuilds = 0;
            DashLog("[dashworst] us=%lu map_us=%lu bui_us=%lu pres_us=%lu kb=%lu full=%d widgets=%u carried=%u "
                    "budget=%d map_inc=%d (the costliest tick of the 256)",
                    (unsigned long) ((sWorstTick.total * 1000u) / ticksPerMs),
                    (unsigned long) ((sWorstTick.map * 1000u) / ticksPerMs),
                    (unsigned long) ((sWorstTick.bui * 1000u) / ticksPerMs),
                    (unsigned long) ((sWorstTick.pres * 1000u) / ticksPerMs), sWorstTick.kb, sWorstTick.full,
                    sWorstTick.widgets, sWorstTick.carried, sWorstTick.d1, sWorstTick.mapInc);
            memset(&sWorstTick, 0, sizeof(sWorstTick));
            DashProfEmit(256u);
            sTickTicksTotal = 0;
            sTickTicksMax = 0;
        }
    }

    DashServiceShot(fb);
}

/* Called once, from gdx3ds_menu_tick_body, right after a race ends while the DASH owned the
 * screen: clears the once-per-race latch and flushes a deferred mode-cycle save (the race's own
 * valid 1->0 transition is the safe point gdx3ds_bottomhud.h promises). gdx3ds_menu.c detects
 * the transition itself (gdx3ds_bottomhud_race_live going 0) and repaints its own UI from
 * scratch; this function only resets the DASH's latches so the NEXT race gets a fresh backdrop. */
uint64_t gdx3ds_bottomhud_take_tick_ticks(void) {
    const u64 t = sDashLastTickTicks;
    sDashLastTickTicks = 0;
    return t;
}

void gdx3ds_bottomhud_on_race_ended(void) {
    sWasValid = 0;
    gdx3ds_buirace_reset();
    sCustomBackdropPainted = 0;
    sLastTop5Count = -1;
    sLastHeaderRacersRemaining = -1;
    sLastHeaderTrackName[0] = '\0';
    sDismissed = 0; /* the next race always starts in the DASH, not the debug tabs */
    /* The idle view becomes reachable again the instant this race ends (if not itself dismissed)
     * -- its own backdrop latch must repaint fresh rather than trust whatever pixels the just-
     * ended race DASH (or CUSTOM edit mode) left in the live framebuffer. sIdleDismissed is left
     * alone on purpose: a user who was already looking at the debug console before/during this
     * race stays there after it ends too, same as the race DASH's own TAB_RACE return path never
     * force-navigates the console UI either. */
    sIdleBaked = 0;
    gdx3ds_bottomhud_flush_pending_save();
}

int gdx3ds_bottomhud_dismissed(void) {
    return sDismissed;
}

void gdx3ds_bottomhud_resume(void) {
    sDismissed = 0;
}

int gdx3ds_bottomhud_edit_active(void) {
    return sEditMode;
}

/* ---- IDLE VIEW: public API -- see gdx3ds_bottomhud.h's own doc comment for the full picture. */

int gdx3ds_bottomhud_idle_enabled(void) {
    return sIdleEnabled;
}

/* gdx3ds_menu_tick_body's own takeover-gate predicate for the idle view, checked only once the
 * race-live/edit-active takeover does not apply. bottom_race=0 (sEnabled) disables DASH-style
 * rendering everywhere, not just mid-race -- see gdx3ds_bottomhud.h's own note on why idle
 * requires BOTH levers, not bottom_idle alone. */
/* hwfix-combined (hardware report "see the track editor input debug menu"): [debug] bottom_idle=0
 * hands the bottom screen to the settings console only at the idle MENUS -- the screens a player
 * sits on between races (title and its attract demos, main menu, machine select / settings, course
 * select, the GP next-course / next-machine cards, records course select, options), which is where
 * the EK CRS course-share tab is wanted. Everywhere else that is not a live race (the EK course
 * editor and machine creator, credits, the records flyby, a race's intro before its DASH snapshot is
 * valid) keeps the idle view exactly as bottom_idle=1 does. GET_MODE values: decomp/include/
 * fzx_game.h. */
static int IdleConsoleMode(void) {
    if (gdx_title_demo_active()) {
        return 1;
    }
    if (&gdx_input_game_mode == NULL) {
        return 1;
    }
    switch (gdx_input_game_mode()) {
        case 0x00: /* TITLE */
        case 0x07: /* MAIN_MENU */
        case 0x08: /* MACHINE_SELECT */
        case 0x09: /* MACHINE_SETTINGS */
        case 0x0A: /* COURSE_SELECT */
        case 0x0F: /* GP_RACE_NEXT_COURSE */
        case 0x12: /* GP_RACE_NEXT_MACHINE_SETTINGS */
        case 0x13: /* RECORDS_COURSE_SELECT */
        case 0x14: /* OPTIONS_MENU */
            return 1;
        default:
            return 0;
    }
}

int gdx3ds_bottomhud_idle_active(void) {
    const int idleWanted = sIdleEnabled || !IdleConsoleMode();
    return sEnabled && idleWanted && !sEditMode && !sIdleDismissed && !gdx3ds_bottomhud_race_live();
}

int gdx3ds_bottomhud_idle_dismissed(void) {
    return sIdleDismissed;
}

void gdx3ds_bottomhud_idle_resume(void) {
    sIdleDismissed = 0;
    sIdleBaked = 0; /* force a fresh repaint -- the live buffer holds the console UI right now */
}

/* One call per tick while gdx3ds_bottomhud_idle_active() is 1 -- same calling convention as
 * gdx3ds_bottomhud_tick, same off-screen sShadowFb (the two never run in the same tick, so
 * sharing the one buffer costs nothing extra). Unlike the race DASH, the idle view has no live
 * fields at all: DashPaintIdleBaked runs exactly ONCE per entry (the `!sIdleBaked` edge below),
 * and the shadow-to-live copy runs only on that same tick -- every later tick while idle stays up
 * does nothing but the DEBUG-button touch test, no framebuffer writes at all. This is the
 * "redraw rarely (static image; only redraw on change)" requirement taken literally: there is
 * nothing to change until the DEBUG button is tapped, so nothing redraws until then. */
void gdx3ds_bottomhud_idle_tick(unsigned short* fbIn, unsigned int held, unsigned int prevHeld, int touching,
                                 int prevTouching, int touchPx, int touchPy) {
    uint16_t* const realFb = (uint16_t*) fbIn;
    uint16_t* const fb = sShadowFb;
    int insideBack;
    int backDown;
    int selectLongpress;
    (void) prevHeld;
    (void) prevTouching;
    if (realFb == NULL) {
        return;
    }
    if (!sIdleBaked) {
        DashPaintIdleBaked(fb);
        memcpy(realFb, fb, sizeof(sShadowFb));
        sIdleBaked = 1;
    }

    /* SELECT long-press, the idle view's own twin of the race DASH's DEBUG-button dismiss
     * gesture -- same threshold constant, same feel. */
    if ((held & KEY_SELECT) && !sBuiOn) {
        if (sIdleSelectHoldTicks <= DASH_SELECT_LONGPRESS_TICKS) {
            sIdleSelectHoldTicks++;
        }
    } else {
        sIdleSelectHoldTicks = 0;
    }

    insideBack = touching && !sDebugHidden && touchPx >= DASH_BACK_X0 && touchPx < DASH_BACK_X0 + DASH_BACK_W &&
                 touchPy >= DASH_BACK_Y0 && touchPy < DASH_BACK_Y0 + DASH_BACK_H;
    backDown = insideBack && !sIdlePrevBackTouch;
    selectLongpress = sIdleSelectHoldTicks == DASH_SELECT_LONGPRESS_TICKS;
    sIdlePrevBackTouch = insideBack;
    if (backDown || selectLongpress) {
        sIdleDismissed = 1;
        DashLog("[dash] idle dismissed -> debug tabs");
        return; /* nothing left to paint -- the console UI owns the next tick's whole repaint */
    }

    DashServiceShot(fb); /* same armed-shot mechanism the race DASH uses, for idle-view captures */
}

void gdx3ds_bottomhud_enter_custom_edit(void) {
    /* SHELF defense in depth: gdx3ds_menu.c's DBG tab already hides/refuses the "HUD EDIT" row
     * whenever hud_custom=0 (gdx3ds_bottomhud_custom_enabled), so this should be unreachable with
     * the lever off -- but refuse here too rather than trust the caller alone. */
    if (!HudCustomLeverEnabled()) {
        DashLog("[dash] custom edit refused: [debug] hud_custom=0 (CUSTOM shelved)");
        return;
    }
    if (sPlacementMode != GDX_HUD_PLACEMENT_CUSTOM) {
        const int inRace = (&gdx_input_in_gameplay != NULL) && gdx_input_in_gameplay();
        sPlacementMode = GDX_HUD_PLACEMENT_CUSTOM;
        sCustomBackdropPainted = 0;
        gdx3ds_config_set_int("hud", "placement", sPlacementMode);
        DashSyncCustomTopMask();
        /* Same save-now-or-defer rule as gdx3ds_bottomhud_cycle_mode's own switch into CUSTOM --
         * see that function's comment. */
        if (inRace) {
            sPlacementSaveDirty = 1;
        } else {
            DashHudIniSave();
        }
    }
    sDismissed = 0; /* the takeover gate's edit_active() clause needs this 0 to actually run */
    sEditSelected = -1;
    sEditMode = 1;
    DashLog("[dash] custom edit -> begin (via debug menu)");
}

/* ---- bottom-screen product menus (gdx3ds_bui.c) -- see gdx3ds_bottomhud.h ---------------- */

void gdx3ds_bottomhud_set_bui(int on, int debugButton) {
    sBuiOn = on ? 1 : 0;
    sDebugHidden = (on && !debugButton) ? 1 : 0;
    sIdleBaked = 0;
}

uint16_t* gdx3ds_bottomhud_shadow_fb(void) {
    return sShadowFb;
}

uint16_t* gdx3ds_bottomhud_live_fb(void) {
    return sLiveFb;
}

void gdx3ds_bottomhud_service_shot(const uint16_t* fb) {
    DashServiceShot(fb);
}

void gdx3ds_bottomhud_idle_invalidate(void) {
    sIdleBaked = 0;
}

void gdx3ds_bottomhud_idle_dismiss(void) {
    sIdleDismissed = 1;
    DashLog("[dash] idle dismissed -> debug tabs (L+R+SELECT)");
}

void gdx3ds_bottomhud_debug_button_unlock(void) {
    if (!sDebugHidden) {
        return;
    }
    sDebugHidden = 0;
    sWasValid = 0;
    sIdleBaked = 0;
    DashLog("[dash] developer console opened: DEBUG button shown on the DASH from now on");
}

void gdx3ds_bottomhud_dismiss(void) {
    sDismissed = 1;
    sEditMode = 0;
    DashLog("[dash] dismissed -> debug tabs (L+R+SELECT)");
}
