/* port/3ds/os/gdx3ds_input_map.h -- MENU: runtime-mutable 3DS->N64 button mapping.
 *
 * NOT a frozen contract header (those live in port/3ds/include/); exported via
 * gdx3ds_os's PUBLIC include dir like gdx3ds_config.h. The compiled kButtonMap in
 * gdx3ds_os_ctru.c is re-expressed as a table of game ACTIONS (accel/boost/...),
 * each holding a mutable 3DS-key mask whose compiled value is the default. The
 * bottom-screen menu's INPUT tab reads/rebinds through this surface and persists
 * changed masks as [input] bind_<action> (hex int) in gdiffuser.ini; the OS backend
 * loads those keys over the defaults at window init.
 *
 * Main thread only (the menu tick and gdx3ds_os_poll_input run on the same thread).
 */
#ifndef GDX3DS_INPUT_MAP_H
#define GDX3DS_INPUT_MAP_H

#ifdef __cplusplus
extern "C" {
#endif

int gdx3ds_input_action_count(void);
/* Lowercase config-stable action name ("accel", "boost", ...). */
const char* gdx3ds_input_action_name(int index);
/* Current / compiled-default 3DS hid key masks (libctru KEY_* bits). */
unsigned gdx3ds_input_action_mask(int index);
unsigned gdx3ds_input_action_default(int index);
void gdx3ds_input_action_set_mask(int index, unsigned hidMask);
void gdx3ds_input_reset_defaults(void);
/* All hid bits an action may legally bind to (face/shoulder/start/select/dpad/cstick). */
unsigned gdx3ds_input_bindable_mask(void);
/* "B+ZL+ZR"-style label for a mask; returns buf. */
const char* gdx3ds_input_key_label(unsigned hidMask, char* buf, int cap);

/* INPUT TUNE: circle-pad deadzone (raw units, 0-80), range where +-80 is reached (60-156),
 * response curve (0 linear, 1 soft, 2 softer), d-pad steering (0 off, 1 full, 2 ramp).
 * Persisted by the menu as [input] deadzone / range / curve / dpad_steer. */
int gdx3ds_input_get_deadzone(void);
void gdx3ds_input_set_deadzone(int v);
int gdx3ds_input_get_range(void);
void gdx3ds_input_set_range(int v);
int gdx3ds_input_get_curve(void);
void gdx3ds_input_set_curve(int v);
int gdx3ds_input_get_dpad_steer(void);
void gdx3ds_input_set_dpad_steer(int v);
/* Last polled raw circle-pad position and the stick value handed to the game. */
void gdx3ds_input_stick_readout(int* rawX, int* rawY, int* stickX, int* stickY);

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_INPUT_MAP_H */
