/* port/3ds/gdx3ds_menu.h — MENU: v1 bottom-screen touch menu.
 *
 * A tabbed text-console UI on the bottom screen (libctru console + hidTouchRead
 * hit zones), owning the whole console layout when enabled:
 *
 *   row 1      fps line (handed over by gdx3ds_fps_hud at its ~1 Hz cadence)
 *   row 2      tab bar: STAT DISP 3D AUD INP LOG DBG ABT  [OFF]   (tappable)
 *   rows 3-29  active tab's page
 *   row 30     stdout scroll window (stray debug.console prints land here)
 *
 * Repaints only on state change / tab switch, plus ~1 Hz for the live pages
 * (STATUS, LOG in live mode) — no per-frame console traffic (MENU-PERF).
 * All calls are main/render thread only (libctru console + hid access).
 *
 * Gate: gdiffuser.ini [menu] enabled — DEFAULT ON. With enabled=0 the legacy
 * FPS-HUD layout (rows 1-3 + scrolling log) is untouched.
 *
 * [OFF] blanks the console and powers the bottom backlight down (GSPLCD); any
 * touch wakes it. The off state persists ([menu] screen_off) and is restored at
 * boot. Every user action logs a "[menu] ..." receipt (svc + filelog) so
 * headless emulator runs can verify the wiring.
 */
#ifndef GDX3DS_MENU_H
#define GDX3DS_MENU_H

#ifdef __cplusplus
extern "C" {
#endif

/* After gdx3ds_fps_hud_init (shares its console when the HUD is on; owns its own
 * consoleInit otherwise). Reads config, paints the initial page, restores the
 * persisted screen-off state. */
void gdx3ds_menu_init(void);

/* Once per host frame from the frame loop, after input polling (uses the hid
 * state latched by gdx3ds_os_poll_input — never calls hidScanInput itself). */
void gdx3ds_menu_tick(void);

/* 1 when the menu owns the bottom screen ([menu] enabled, default on). Stable
 * after gdx3ds_menu_init; also safe (0) before it. */
int gdx3ds_menu_enabled(void);

/* FPS-HUD handoff: the HUD's ~1 Hz fps line, pinned as row 1 of every tab. */
void gdx3ds_menu_set_fps_line(const char* line);

/* DBG-tab live latch for main_3ds.cpp's recurring telemetry gate: replaces the
 * boot-time `[debug] verbose || gputrace` const so the DBG toggles apply live. */
int gdx3ds_dbg_verbose_active(void);

/* APT lifecycle (main_3ds.cpp aptLifecycleHook): 1 while [OFF] holds the bottom backlight
 * down through the gsp::Lcd session; force_backlight re-applies a GSPLCD power state on
 * that session WITHOUT touching the menu's own screen-off state or logging (safe from the
 * hook: no console). ONSUSPEND powers the panel on for the HOME menu, ONRESTORE/ONWAKEUP
 * re-apply the saved state. */
int gdx3ds_menu_backlight_off(void);
void gdx3ds_menu_force_backlight(int on);

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_MENU_H */

/* Console filter passthrough (main thread painters only). */
void gdx3ds_console_passthrough(int on);
