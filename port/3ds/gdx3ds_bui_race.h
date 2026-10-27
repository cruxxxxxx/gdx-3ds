/* port/3ds/gdx3ds_bui_race.h — the multiplayer race dash's shell (bottom-menus design, M3).
 *
 * The DASH (gdx3ds_bottomhud.c) stays the race's bottom screen; in a multiplayer race with the
 * product menus on ([ui] bottom_menus) it calls in here at a few fixed points, and this file
 * draws what multiplayer adds (gdx3ds_bui_racedash.h) from the game's whole field
 * (gdx_racefield_snapshot) and the session (gdx3ds_mpo_race_view):
 *
 *   the left column      you (big place, "OF N") and every human by place: replaces the DASH's
 *                        own portrait + top-5 boxes; repainted only when a place changes
 *   the header           "2P+14 CPU" instead of "<n> RACERS"; STANDINGS instead of DEBUG
 *   the map box          CPUs as grey dots, humans in their player colours (instead of the
 *                        game's four minimap markers), and the NET chip
 *   STANDINGS            a full-screen 30-row overlay, rows repainted at 4 Hz; tap to close
 *   the drop toast       "<NAME> DROPPED: A CPU DRIVES" for 3 s after a removal
 *   the stall banner     painted from inside a stalled race tick (gdx3ds_bui_race_stall, called
 *                        by the MP layer while the main loop is blocked)
 *
 * gdx3ds_buirace_active() is 0 outside a multiplayer session (or with bottom_menus off), and the
 * DASH then runs exactly its single-player code path.
 *
 * Receipts: `[bui] mpdash ticks=N avg_us= max_us=` (this file's share of the DASH tick) and
 * `[bui] standings ticks=N avg_us= max_us=` (the overlay), every 256 ticks of each. */
#ifndef GDX3DS_BUI_RACE_H
#define GDX3DS_BUI_RACE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 1 = this DASH tick is a multiplayer race's: call the hooks below. Cheap; evaluated once per
 * DASH tick by gdx3ds_bottomhud_tick. */
int gdx3ds_buirace_active(void);

/* After the DASH baked its backdrop (once per race and after every rebake): the column frame,
 * the STANDINGS button and the header's field count. */
void gdx3ds_buirace_on_bake(uint16_t* shadow);

/* Inside the map box's repaint, after the track bitmap: the dots and the NET chip. The box is
 * px square at (x0, y0). */
void gdx3ds_buirace_paint_map(uint16_t* shadow, int x0, int y0, int px);

/* After the DASH's widget loop: the column (what changed), the drop toast. */
void gdx3ds_buirace_paint(uint16_t* shadow);

/* Touch on a DASH tick: 1 = a tap on STANDINGS opened the overlay. */
int gdx3ds_buirace_touch(int touching, int px, int py);

/* While the standings overlay is open it owns the whole tick: paints into shadow, presents what
 * changed into live, closes on a tap. Returns 1 when it owned this tick (the DASH paints
 * nothing), 0 when it is closed (the DASH runs; rebake first if gdx3ds_buirace_take_rebake). */
int gdx3ds_buirace_overlay_tick(uint16_t* shadow, uint16_t* live, int touching);

/* 1 once (then cleared) when the DASH must repaint from scratch: the overlay closed, a stall
 * banner or a toast went away. */
int gdx3ds_buirace_take_rebake(void);
/* mp-menus #10: while the race is paused (shared: every console), the pause card owns the bottom
 * screen: who paused; the pauser's own console has the button (a touch is its A). 1 = it painted
 * (the DASH does nothing this tick), 0 = not paused (rebake first if take_rebake says so). */
int gdx3ds_buirace_pause_tick(uint16_t* shadow, uint16_t* live, int touching, int px, int py);

/* The race's DASH is gone (race over): forget the race. */
void gdx3ds_buirace_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_BUI_RACE_H */
