/* port/3ds/gdx3ds_bui.h — the bottom-screen product menus (artifacts/bottom-menus-design, M1-M2):
 * the Home screen at TITLE / MAIN MENU and the MULTIPLAYER flow (choose, NEARBY, host, ONLINE,
 * room code, name, joining, lobby, machine, race settings, course, launch, MP dialogs).
 *
 * LEVER. gdiffuser.ini [ui] bottom_menus, DEFAULT 1 on every build (EK and shipped alike: the
 * menus need nothing the Expansion Kit adds). 0 = exactly the old bottom screen: the idle logo
 * view with its DEBUG button and SELECT long-press, the DASH as before, nothing of this file runs
 * (no paint, no key mask, no allocation). [debug] bottom_idle=0 or bottom_race=0 also turn the
 * menus off where they turn the idle view off (the menus live where the idle view lives).
 *
 * WHO OWNS WHAT (the design's ownership states):
 *   AMBIENT  top screen at TITLE (0x00) or MAIN MENU (0x07), no attract demo, no flow open: the
 *            Home screen. Touch belongs to the menus, every button to the game (stock menu).
 *            MULTIPLAYER opens from the top screen's VS icon (gdx_mp_vs_menu_open).
 *            SETTINGS (the footer's button, or SELECT on its own -- the game has no SELECT) opens
 *            the player settings: the console UI with only its player tabs (gdx3ds_menu.h
 *            gdx3ds_menu_open_settings), its HOME tab coming back here.
 *   EDITOR   (mp-menus, EK with [ek] course_share) the course editor: its bottom menu imports
 *            courses (SD files, QR scan). Touch only; the scan takes the buttons (B cancels).
 *   FLOW     a flow is open (MULTIPLAYER ...) while the top screen sits at TITLE / MAIN MENU: the
 *            buttons drive the flow and are hidden from the game (gdx3ds_bui_game_key_mask, read
 *            by gdx3ds_os_poll_input); a key still held when the flow lets go stays hidden until
 *            released. Never while an MP session drives the race (gdx3ds_mpo_in_session) and never
 *            if the menus stopped ticking (a watchdog), so the game can not be left without keys.
 *            A flow open while the game is elsewhere (an MP race launching) is shown, touch-only,
 *            its key glyphs hidden -- except a room screen while a stock pick only parks the top
 *            screen (confirmed / frozen): its keys are the room's again (START RACE, SETTINGS).
 *   RACE     a live race: the DASH, as before.
 *   DEV      the developer console (every tab): L+R+SELECT held 1.5 s toggles it from any of
 *            the above ([debug] dev_console=1 also shows a DEBUG button on Home and the DASH).
 *   Elsewhere (machine select, options, records ...) the idle logo view, as before.
 *
 * ini keys:
 *   [ui] bottom_menus  1   the lever above
 *   [debug] dev_console 0  1 = DEBUG buttons on Home / the DASH / the idle view (the combo works
 *                          either way; card-4: once the combo opened the console, the DASH and
 *                          the idle view show DEBUG for the rest of the launch, and a 1P race
 *                          that starts with the console open keeps it open). card-4b: test
 *                          cards set it to 1 (the DEBUG button is the discoverable way in; the
 *                          chord is wall-clock 1.5 s and logs "[menu] chord L+R+SELECT ..." on
 *                          down / early release / fire). Players' cards leave it out.
 *   [debug] bui_3d_button 0  1 = Home shows the 3D ON / OFF button (card-4: a developer lever,
 *                          was [ui] home_3d_button default 1; players use the 3D slider)
 *   [debug] bui_gallery 0  1 = at the first chance (TITLE), render every screen of the gallery
 *                          (gdx3ds_bui_gallery.h) and write autotest/bui_<id>_R.bmp for each
 *   [debug] bui_input  ""  a scripted-input file (gdx3ds_bui_script.c): buttons, circle pad and
 *                          touch fed to both the game and the menus, plus SHOT / WAIT lines
 *   [debug] bui_log    0   1 = a log line per repaint (screen, cost, dirty span)
 *   [ui] hold_confirm_ms 1500  mp-menus: START RACE and LEAVE / CLOSE the room are held this long
 *                          (the button or a touch; a meter fills, letting go early cancels);
 *                          0 = a press, as before; clamped to 0..10000
 * Receipts: `[bui] ...` lines; every 600 ticks `[bui] ticks= idle_us= paints= paint_us= present_us=`. */
#ifndef GDX3DS_BUI_H
#define GDX3DS_BUI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* From gdx3ds_menu_init, after gdx3ds_bottomhud_init. Reads the levers. */
void gdx3ds_bui_init(void);
int gdx3ds_bui_enabled(void);

/* Every menu tick, before any screen owner runs: 1 the tick L+R+SELECT reaches 1.5 s
 * (wall time, card-4b). */
int gdx3ds_bui_dev_combo(uint32_t held);

/* One call per menu tick while the idle view would own the screen (gdx3ds_bottomhud_idle_active)
 * -- the menus decide between Home, a flow and the idle logo (which they tick themselves). */
void gdx3ds_bui_tick(uint16_t* fb, uint32_t held, uint32_t prevHeld, int touching, int touchPx, int touchPy);

/* The console took the screen (or gave it back): the next bui tick repaints from scratch. */
void gdx3ds_bui_invalidate(void);

/* gdx3ds_os_poll_input (weak): the keys to hide from the game this poll, and whether the circle
 * pad is hidden too. */
uint32_t gdx3ds_bui_game_key_mask(uint32_t held);
int gdx3ds_bui_game_stick_hidden(void);

/* The scripted input (gdx3ds_bui_script.c, [debug] bui_input). The menu tick steps it once per
 * tick; the hid readers in gdx3ds_os_poll_input and gdx3ds_menu.c pass their values through it. */
void gdx3ds_bui_script_init(void);
void gdx3ds_bui_script_step(void);
int gdx3ds_bui_script_active(void);
uint32_t gdx3ds_bui_script_keys(uint32_t held);
/* 1 = the script owns the touch screen this tick: *px/*py set (touching = KEY_TOUCH in keys). */
int gdx3ds_bui_script_touch(int* px, int* py);
/* 1 = the script owns the circle pad this tick: *dx/*dy set (-156..156). */
int gdx3ds_bui_script_circle(int* dx, int* dy);

/* mp-menus, decomp-3ds-mp-vs-menu.patch (main_menu.c): with one pad the top screen's VS icon opens
 * MULTIPLAYER here. available = 1 while the bottom-screen menus run; open = A on the icon (the
 * flow opens on the next menu tick, the top screen stays on the grid). */
int gdx_mp_vs_menu_available(void);
void gdx_mp_vs_menu_open(void);

/* For the script's WAIT lines: the screen the menus show ("home", "lobby", ... "logo", "off"). */
const char* gdx3ds_bui_screen_name(void);
/* WAIT conditions beyond a screen name ("rooms", "startable", "race", "ready"); "!x" = not x. */
int gdx3ds_bui_condition(const char* what);

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_BUI_H */
