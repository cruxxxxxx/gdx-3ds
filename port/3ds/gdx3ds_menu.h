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
 * down; force_backlight re-applies a GSPLCD power state through a SHORT-LIVED gsp::Lcd
 * session (open, one call, close -- C4j: a session held across the HOME jump froze the
 * Old 3DS HOME menu) WITHOUT touching the menu's own screen-off state or logging (safe from
 * the hook: no console). Returns the GSPLCD result, 0 when nothing was needed. ONSUSPEND
 * powers the panel on for the HOME menu, ONRESTORE/ONWAKEUP re-apply the saved state.
 * lcd_session_open: 1 only while one of those calls is in flight (receipt; must read 0). */
int gdx3ds_menu_backlight_off(void);
int gdx3ds_menu_force_backlight(int on);
int gdx3ds_menu_lcd_session_open(void);

/* Arms a one-shot BMP dump of the bottom screen on the next tick this file's own plain
 * console UI is the one painting it (not the race DASH or idle-view takeover, which arm
 * gdx3ds_bottomhud_note_shot instead), written to autotest/<label>_R.bmp -- same mechanism
 * and naming convention as gdx3ds_bottomhud_note_shot/gdx3ds_racemap_note_shot. Weak-linked
 * from port/n64_gfx_bridge.cpp's gdx_request_frame_dump, same call site that arms the other
 * two; a no-op unless [menu] enabled is on. */
void gdx3ds_menu_note_shot(const char* label);

/* release: the PLAYER SETTINGS (Home's SETTINGS button, gdx3ds_bui.c). Opens this console UI with
 * only the player tabs -- DSP (display, widescreen, detail), 3D, AUD (volume), IN (controls,
 * remapping) -- and a HOME tab back; no developer tab (STAT, LOG, DBG, MAP, MP, AB, CRS). The
 * developer console stays behind L+R+SELECT / [debug] dev_console. Returns 0 when the console UI
 * is off ([menu] enabled = 0), so Home hides the button. */
int gdx3ds_menu_settings_available(void);
void gdx3ds_menu_open_settings(void);
/* 1 while the player settings (not the developer console) are open. */
int gdx3ds_menu_settings_open(void);
/* uipolish: 1 while B leaves the console (SETTINGS / the developer pages, outside a race): the
 * product menus hide that B from the game (gdx3ds_bui_game_key_mask). */
int gdx3ds_menu_b_exits(void);

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_MENU_H */

/* Console filter passthrough (main thread painters only). */
void gdx3ds_console_passthrough(int on);
