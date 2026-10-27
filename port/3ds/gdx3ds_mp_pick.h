/* port/3ds/gdx3ds_mp_pick.h — multiplayer: every player picks with the game's own 1P menus.
 *
 * WHY. A multiplayer room used to pick machines and the course on the bottom screen. The stock
 * screens are the game's own and players know them, so a room now drives them instead: the host
 * opens MACHINE SELECT on every console at once (a round), each player picks a machine and its
 * engine setting on its own top screen, and the host then picks the course in the stock COURSE
 * SELECT. The menus run locally on each console -- they are not in lockstep; only what they
 * produce is shared, as network data in the lobby's HELLO, and frozen into the START packet. The
 * race pins every racer from the START alone (gdx3ds_mp_game.c PinRace).
 *
 * HOW. The 1P flow's own screens are entered in PRACTICE mode with one player, seeded with the
 * lobby's picks. decomp-3ds-mp-stockpick.patch passes each mode update's result through
 * gdx_mp_pick_filter: MACHINE SETTINGS' final OK records the pick and the screen stays (nothing
 * else follows it); COURSE SELECT's OK the same; MACHINE SELECT's B is ignored (a round always
 * ends in a pick). While a pick runs nothing is read from or written to the save
 * (Save_UpdateCharacterSave / Save_UpdateCourseCharacterSave are skipped), the X cup (generated
 * per console) is hidden, and a course multiplayer cannot race (the EDIT cup) is refused with the
 * error sound. Every global the screens change -- mode, players, course, cup, the 1P picks, the
 * per-machine engine memory -- is put back when the pick ends (or, when a race launches, right
 * before the race saves the user's settings). On the Expansion Kit the custom / super machines are
 * swapped out for the stock ones while a pick or an online race runs (they are per-console save
 * data: two consoles would race different machines under the same index).
 *
 * Main thread only. Compiled into gdx3ds_game (game/gdx3ds_mp_pick_game.c, decomp types). */
#ifndef GDX3DS_MP_PICK_H
#define GDX3DS_MP_PICK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    GDX_MPP_NONE = 0,
    GDX_MPP_MACHINE = 1, /* MACHINE SELECT + MACHINE SETTINGS */
    GDX_MPP_COURSE = 2,  /* COURSE SELECT (the host) */
};

/* Open a stock picker on the top screen, seeded with the lobby's current pick (machine 0..29,
 * skin 0..3, engine 0..1; course 0..41: the course cursor). Works from the title, the main menu
 * or another picker; the screen change waits for any transition in flight. 0 = refused (no game
 * booted yet, or a race is being launched). */
int gdx3ds_mpp_begin(int kind, int machine, int skin, float engine, int course);
/* GDX_MPP_* the top screen is picking with right now (0 when idle, on its way, or confirmed). */
int gdx3ds_mpp_active(void);
/* 1 while the pick holds the top screen at all (on its way in, picking, confirmed, leaving). */
int gdx3ds_mpp_holding(void);
/* A confirmed pick, once each (1 = *out filled). */
int gdx3ds_mpp_take_machine(int* machine, int* skin, float* engine);
int gdx3ds_mpp_take_course(int* course);
/* The machine step's time is up: if the player has not confirmed, its cursor's machine is the
 * pick, with that machine's first colour and the slider's centre, and the machine screen stays
 * (as after a confirm) with the game's buttons held back (gdx3ds_mpp_input_locked) until the next
 * screen. A confirmed pick is kept. */
void gdx3ds_mpp_timeout(void);
/* mp-menus: the host's course step is over ([net] mp_course_ms): the highlighted course (when
 * this console can race it; else the one the picker opened on) is the pick, as if confirmed. */
void gdx3ds_mpp_course_timeout(void);
/* mp-menus KEEP: this machine is the pick (as a confirm would make it), the machine screen stays
 * frozen like a timeout's. Only while the machine step's picker is up or on its way. */
void gdx3ds_mpp_keep(int machine, int skin, float engine);
/* 1 while a timed-out machine screen is frozen: the game gets no buttons or stick (any thread). */
int gdx3ds_mpp_input_locked(void);
/* Leave whatever picker is open for the main menu (the room closed). */
void gdx3ds_mpp_close(void);
/* A race is being launched: the user's own settings come back now (the race saves them next);
 * the save stays guarded until the screen has left the pickers. */
void gdx3ds_mpp_release(void);
/* Every frame (after the game update). */
void gdx3ds_mpp_frame(void);
/* gdx3ds_mp_mode_init_hook, at the mode-init checkpoint (gGameMode is the new mode). */
void gdx3ds_mpp_mode_init(void);
/* 1 for a few seconds after the course picker refused a course (the bottom screen says why). */
int gdx3ds_mpp_course_refused_recently(void);
const char* gdx3ds_mpp_stage_name(void);

/* This console's key for a course (gdx_mp_online.h GDX_MPO_NO_COURSE_KEY = it cannot race it):
 * the stock courses' fixed key; a DD-cup course's from the inserted disk's ID. */
uint32_t gdx3ds_mpp_course_key(int course);
/* The race's course globals for course (cup per the Expansion Kit's cup layout, track name). */
void gdx3ds_mpp_pin_course(int course);
/* An online race runs with the stock machines (Expansion Kit: custom / super machines off). */
void gdx3ds_mpp_race_machines(int on);
/* A digest of what the race reads from the picks: racers 0..n-1's machine / colour / engine
 * factor and their machines' stats. Folded into the per-tick lockstep digest. n is the whole
 * field (MP30: the humans' picks plus the VS COM CPU fill, up to 30), so every console also
 * proves it filled the same CPU pilots around the same picks. */
uint32_t gdx3ds_mpp_picks_hash(int fieldRacers);

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_MP_PICK_H */
