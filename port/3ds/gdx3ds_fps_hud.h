/* port/3ds/gdx3ds_fps_hud.h — FPS-HUD: bottom-screen performance counter.
 *
 * A 3-row text HUD pinned to the TOP of the bottom-screen console, refreshed
 * ~once per second from the main/render thread (libctru's console is not
 * thread-safe; every call here must come from the thread that runs the frame
 * loop). Integration-level TU like gdx3ds_filelog.c, not a stream dir.
 *
 * Rows:
 *   1  host fps (frame-loop presents/sec) + game fps (decomp gGameFrameCount rate)
 *   2  when [debug] gputrace = 1: window-avg CPU build ms + the top [profop]
 *      opcode (read from gdx3ds_gpu_prof's live window accumulators)
 *   3  static build id (branch@hash, baked at configure time) so on-device
 *      reports can name the exact build
 *
 * Gate: gdiffuser.ini `[debug] fps` — DEFAULT ON. `fps = 0` (with `console = 0`)
 * restores the untouched black bottom screen. When enabled the HUD owns the
 * bottom-screen console init and reserves its top rows via consoleSetWindow, so
 * scrolling stdout/stderr logs (debug.console behavior) can never eat the HUD.
 */
#ifndef GDX3DS_FPS_HUD_H
#define GDX3DS_FPS_HUD_H

#ifdef __cplusplus
extern "C" {
#endif

/* Read the [debug] fps gate (config must already be loaded), init the bottom
 * console when enabled, paint the static rows. Call once, after
 * gdx3ds_os_window_init (gfx must be up). Safe no-op when the gate is off. */
void gdx3ds_fps_hud_init(void);

/* Once per host frame from the frame loop. Cheap when off (one int test) and
 * between refreshes (tick math only); repaints at ~1 Hz. No heap allocation on
 * any path (integer-only formatting — newlib's %f can malloc via dtoa). */
void gdx3ds_fps_hud_tick(unsigned gameFrameCount);

/* MENU integration (gdx3ds_menu.c). When the menu owns the bottom screen the HUD
 * stops painting and instead hands its ~1 Hz fps line to the menu, which pins it
 * as row 1 of every tab. */
void* gdx3ds_fps_hud_console(void);          /* PrintConsole* (NULL when HUD off) */
const char* gdx3ds_fps_hud_build_id(void);   /* configure-time branch@hash */
void gdx3ds_fps_hud_set_enabled(int on);     /* DBG-tab live toggle */
int gdx3ds_fps_hud_get_enabled(void);

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_FPS_HUD_H */
