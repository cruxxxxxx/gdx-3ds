/* port/3ds/gdx3ds_bottomhud.h — nibottom Phase 2: the DASH bottom-screen race HUD.
 *
 * Sibling of gdx3ds_racemap.h/.c (the MAP tab), same cost model and the same software-
 * composite technique: a direct CPU store into the bottom-screen libctru console's
 * PrintConsole::frameBuffer (GSP_RGB565_OES, portrait/column-major,
 * index = x*height + (height-1-y)), zero GPU cost.
 *
 * FULL-SCREEN TAKEOVER, NOT A MENU TAB. The real exported layout (manifest.json, from the
 * user's edited Photoshop template) places widgets across the WHOLE 320x240 bottom screen —
 * a header strip at y=0 and the energy bar at y=216..239 — which is the MENU's own tab-bar
 * and scroll-park rows. A real race HUD and a debug tab bar cannot share those rows, so this
 * is not TAB_MAP's pattern: whenever [debug] bottom_race=1 AND a race is live
 * (gdx3ds_bottomhud_race_live), gdx3ds_menu_tick_body hands the ENTIRE tick to
 * gdx3ds_bottomhud_tick (paint + its own touch) and returns, exactly as if the MENU were not
 * there. Outside a live race (title, practice menu, results) the ordinary MENU UI is back,
 * unchanged, lever or no lever — there is nothing for the DASH to show without a race.
 *
 * ASSETS: port/3ds/gdx3ds_hudbake.{h,c} -- the backdrop, chrome widgets and glyph atlases are
 * baked ONCE at boot (gdx3ds_bottomhud_bake_assets) from the user's own fzerox.o2r, by recipes
 * that reproduce the user's template layers pixel for pixel (no game pixels are compiled in).
 * Every widget except "map_box" (the live per-race minimap replaces it; layout.json's own note
 * on that widget says so).
 *
 * LIVE VALUES: the baked lap/timer/gap widgets show only their LABEL rows ("LAP"/"TIME"/"BEST"/
 * "GAP"); their VALUE rows are skipped at composite time (gdx3ds_bottomhud.c's DashBakeSkipCell)
 * and repainted every tick with the real timer/lap glyph atlases.
 */
#ifndef GDX3DS_BOTTOMHUD_H
#define GDX3DS_BOTTOMHUD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* [debug] bottom_race, read once at init. Default ON (n60lead: the DASH is the accepted
 * default bottom-screen HUD now); an explicit `bottom_race = 0` in the ini still wins. */
void gdx3ds_bottomhud_init(void);
int gdx3ds_bottomhud_enabled(void);

/* Bakes the DASH's art from the mounted archives (gdx3ds_hudbake.h). Call once, after the
 * resource manager has mounted fzerox.o2r and before the first bottom-screen paint; a missing or
 * wrong archive leaves the affected elements blank and logs [hudbake] lines. The result is kept
 * in <base>/hudbake.cache and reused on later boots while every source record's CRC-32 and size
 * are unchanged (and the build is the same); [debug] hudbake_cache=0 bakes every boot instead. */
void gdx3ds_bottomhud_bake_assets(void);

/* 1 iff the lever is on AND a race is currently live — the takeover gate gdx3ds_menu.c checks
 * first thing every tick. Cheap (one snapshot read); safe to call every frame. */
int gdx3ds_bottomhud_race_live(void);

/* The raw [hud] placement lever: one of the GDX_HUD_PLACEMENT_* ints in port/gdx_hud_placement.h
 * (that header is plain-int/decomp-type-free, so gdx3ds_bottomhud.c including it directly costs
 * nothing). This is the function port/input_bridge.c's gdx_hud_placement_mode() calls on 3DS. */
int gdx3ds_bottomhud_placement_mode(void);

/* Advances the mode by one (wrapping ALL_TOP -> SPLIT -> ALL_BOTTOM -> ALL_TOP) and updates the
 * in-memory config immediately (so top-screen suppression reacts the same frame); the actual ini
 * WRITE is deferred to gdx3ds_bottomhud_flush_pending_save if called mid-race (an SD write would
 * stall a frame — see that function and gdx3ds_bottomhud_tick's own race-end flush). */
void gdx3ds_bottomhud_cycle_mode(void);

/* Writes the ini if (and only if) gdx3ds_bottomhud_cycle_mode deferred the save. Called from
 * gdx3ds_bottomhud_tick on the race's own valid 1->0 transition, and from gdx3ds_menu.c's
 * MenuEnterOff (screen off / "menu close") as a second safety net. Idempotent and cheap (a flag
 * check) when nothing is pending. */
void gdx3ds_bottomhud_flush_pending_save(void);

/* One call per tick while gdx3ds_bottomhud_race_live() is 1: reads touch/held itself (same latched
 * hid state gdx3ds_menu.c's own tick already uses — this does not call hidScanInput), hit-tests
 * the MODE chip at its real layout.json rect, runs gdx3ds_bottomhud_cycle_mode on a touch-down or
 * a fresh KEY_SELECT, then paints the whole screen (dirty-rect discipline per element — see the
 * .c file). `fb` is PrintConsole::frameBuffer, exactly as gdx3ds_racemap_paint takes it. */
void gdx3ds_bottomhud_tick(unsigned short* fb, unsigned int held, unsigned int prevHeld, int touching,
                            int prevTouching, int touchPx, int touchPy);

/* o3ds60: the self-timed cost (ticks) of the latest gdx3ds_bottomhud_tick since the previous
 * call, then 0 until the next tick (the frameskip's [steadyd1] receipt reads it per slot). */
uint64_t gdx3ds_bottomhud_take_tick_ticks(void);

/* Resets the once-per-race backdrop/standings latches and flushes any deferred mode-cycle ini
 * save. Call this exactly once, from gdx3ds_menu.c's tick, on the frame gdx3ds_bottomhud_race_live
 * transitions from 1 to 0 (the race just ended) — the MENU is about to repaint its own UI from
 * scratch over the same screen either way, and the DASH needs a fresh backdrop next race. */
void gdx3ds_bottomhud_on_race_ended(void);

/* COORDINATOR FIX (nibottom Phase 2, second visual-feedback round): the DASH is a full-screen
 * takeover with no way back to the debug tab UI mid-race. 1 once the DASH's own on-screen
 * "DEBUG" button (top header gap, left of the MODE chip; labelled "MENU" before the idle-view
 * round) has been tapped; gdx3ds_menu_tick_body checks this alongside gdx3ds_bottomhud_race_live()
 * in its takeover gate, so a dismissed DASH lets the ordinary tab UI run even while the race is
 * still live underneath. A new TAB_RACE entry in gdx3ds_menu.c's own tab bar (visible only while
 * dismissed and the race is still live) calls gdx3ds_bottomhud_resume() to hand the screen back.
 * Reset to 0 on the race's own 1->0 edge (gdx3ds_bottomhud_on_race_ended) so the NEXT race always
 * starts in the DASH, undismissed. */
int gdx3ds_bottomhud_dismissed(void);
void gdx3ds_bottomhud_resume(void);

/* IDLE VIEW (coordinator ask: "we shouldn't just have the bottom screen only show a polished
 * experience during race"): outside a live race (title, menus, attract, results, loading) the
 * bottom screen shows this -- the same checkerboard backdrop + header chrome the race DASH uses,
 * a centred G-DIFFUSER logo, and the same DEBUG button, instead of jumping straight to the
 * text-console debug UI. [debug] bottom_idle (default 1, independent of [debug] bottom_race --
 * see gdx3ds_bottomhud_idle_enabled) turns it off; with it off, or with bottom_race=0 (which
 * disables DASH-style rendering everywhere, not just mid-race), gdx3ds_menu.c falls back to its
 * pre-existing behaviour untouched: the text-console debug UI, full stop. bottom_idle=0 applies
 * at the idle MENUS only (title / attract, main menu, machine select / settings, course select,
 * the GP cards, records course select, options): it never puts the console over the EK editors,
 * credits or any other non-menu screen, which keep the idle view (hwfix-combined).
 *
 * gdx3ds_bottomhud_idle_active() is gdx3ds_menu_tick_body's own takeover-gate predicate (same
 * shape as gdx3ds_bottomhud_race_live()/gdx3ds_bottomhud_edit_active(), checked only once the
 * race/edit-mode takeover itself does not apply): lever on, no race live, not mid-CUSTOM-edit,
 * and not dismissed. gdx3ds_bottomhud_idle_tick is the one-call-per-tick paint+touch entry point,
 * same calling convention as gdx3ds_bottomhud_tick; it draws the idle backdrop exactly ONCE per
 * entry (a genuinely static screen, unlike the race DASH's live fields) and only re-copies the
 * shadow buffer to the screen on that first tick, never every tick, so steady-state idle cost is
 * a few touch-rect comparisons, nothing more.
 *
 * The way back to the idle view from the debug console mirrors TAB_RACE exactly: the SAME tab
 * slot (gdx3ds_menu.c's TAB_RACE) becomes visible, painted "DASH" instead of "RACE", whenever
 * gdx3ds_bottomhud_idle_dismissed() is 1 and no race is live; tapping it calls
 * gdx3ds_bottomhud_idle_resume(). The two cases (mid-race-dismissed vs idle-dismissed) are
 * mutually exclusive (one requires race_live(), the other requires !race_live()), so one tab
 * slot unambiguously covers both -- no new tab-bar column, so no risk of reopening the
 * OFF-button-overlap class of bug the tab bar's own budget comment warns about. */
int gdx3ds_bottomhud_idle_enabled(void);
int gdx3ds_bottomhud_idle_active(void);
int gdx3ds_bottomhud_idle_dismissed(void);
void gdx3ds_bottomhud_idle_resume(void);
void gdx3ds_bottomhud_idle_tick(unsigned short* fb, unsigned int held, unsigned int prevHeld, int touching,
                                 int prevTouching, int touchPx, int touchPy);

/* CUSTOM HUD mode (a 4th placement mode, GDX_HUD_PLACEMENT_CUSTOM, beyond the 3 fixed
 * ALL_TOP/SPLIT/ALL_BOTTOM ones -- see port/gdx_hud_placement.h): every bottom-screen DASH widget
 * becomes draggable/resizable/closeable, directly on the DS, saved to its own hud.ini [custom]
 * section. gdx3ds_bottomhud_placement_mode() (the function port/input_bridge.c actually reads)
 * reports GDX_HUD_PLACEMENT_ALL_BOTTOM whenever the real mode is CUSTOM -- "the top screen
 * follows ALL_BOTTOM behaviour" while native/nihudtop's real per-element top-screen gating isn't
 * merged yet; gdx_hud_set_custom_top_mask() is still called on every layout/mode change so that
 * plumbing is exercised and ready the moment the real gating lands.
 *
 * gdx3ds_bottomhud_edit_active() is OR'd into gdx3ds_menu.c's DASH-takeover gate alongside the
 * existing race-live check: entering edit mode (long-press the MODE chip while already in
 * CUSTOM, or gdx3ds_menu.c's own "HUDX" debug tab) must keep working even with NO live race
 * (sample values are substituted — see gdx3ds_bottomhud.c's DashBuildSampleSnapshot) and must
 * not itself force the takeover while a non-CUSTOM mode is active. */
int gdx3ds_bottomhud_edit_active(void);

/* Switches to CUSTOM mode (if not already there) and enters edit mode in one call — the entry
 * point gdx3ds_menu.c's new "HUDX" debug tab uses, independent of the MODE chip's own
 * long-press gesture (the other way in, available only once the DASH already owns the screen
 * mid-race). Safe to call whether or not a race is live. Refuses (no-op) when [debug]
 * hud_custom=0 -- see gdx3ds_bottomhud_custom_enabled below. */
void gdx3ds_bottomhud_enter_custom_edit(void);

/* SHELVED (nibottom-progress.md §12): 1 iff [debug] hud_custom=1. CUSTOM ("MODE Z") is off by
 * default pending further hardware work (resize doesn't clear the framebuffer, some widgets
 * never display); gdx3ds_menu.c's DBG tab calls this to decide whether to paint/accept taps on
 * the "HUD EDIT" row at all, and gdx3ds_bottomhud_cycle_mode/_init/_enter_custom_edit all gate
 * the same way internally. No CUSTOM code is removed -- only reachable behind this lever. */
int gdx3ds_bottomhud_custom_enabled(void);

/* Arms a one-shot BMP dump of the bottom screen on the next paint, written to
 * autotest/<label>_R.bmp — same mechanism and naming convention as
 * gdx3ds_racemap_note_shot(label) (autotest/<label>_B.bmp), for Gate 3. Weak-linked from
 * port/n64_gfx_bridge.cpp's gdx_request_frame_dump, same call site that arms the MAP tab's
 * shot; a no-op unless [debug] bottom_race is on. */
void gdx3ds_bottomhud_note_shot(const char* label);

/* Bottom-screen product menus ([ui] bottom_menus, gdx3ds_bui.c). set_bui(1, debugButton): the
 * DASH and idle view stop running their own SELECT long-press (gdx3ds_menu.c runs L+R+SELECT for
 * every owner instead), SELECT with L/R held no longer cycles the HUD mode, and the DEBUG button
 * is hidden (painted nowhere, touches ignored) unless debugButton. The kit paints into the same
 * off-screen buffer the DASH uses (shadow_fb) -- never in a tick where the DASH or idle view
 * paints -- and services an armed SHOT through service_shot. idle_invalidate makes the idle view
 * repaint on its next tick (the menus painted over it); idle_dismiss / dismiss hand the screen to
 * the console as the DEBUG button does. */
void gdx3ds_bottomhud_set_bui(int on, int debugButton);
uint16_t* gdx3ds_bottomhud_shadow_fb(void);
/* The live bottom buffer the last DASH tick presented to (NULL before the first): the multiplayer
 * dash's stall banner paints there from inside a stalled race tick (gdx3ds_bui_race.c). */
uint16_t* gdx3ds_bottomhud_live_fb(void);
void gdx3ds_bottomhud_service_shot(const uint16_t* fb);
void gdx3ds_bottomhud_idle_invalidate(void);
void gdx3ds_bottomhud_idle_dismiss(void);
void gdx3ds_bottomhud_dismiss(void);
/* card-4: the developer console was opened this launch (L+R+SELECT): the DASH and the idle view
 * show their DEBUG button from now on, as [debug] dev_console=1 does from boot. Repaints the DASH's
 * backdrop on its next tick so a race in progress gets the button too. */
void gdx3ds_bottomhud_debug_button_unlock(void);

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_BOTTOMHUD_H */
