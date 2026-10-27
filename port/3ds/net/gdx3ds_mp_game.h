/* port/3ds/net/gdx3ds_mp_game.h — mp-online: the game side of an online VS race.
 *
 * WHY. The spike's consoles booted the same replay and stayed in lockstep from the first poll. An
 * online session starts from wherever each console happens to be (a menu, the title demo, a
 * single-player race), so the race itself has to be brought to the same state everywhere:
 *
 *   HOP      every console goes to the main menu first, even from the main menu (the title, its attract
 *            demo's GP race and a 1P race each leave different leftovers that the VS init does
 *            not reset; the main menu's init does): every console reaches the race by the same
 *            MAIN MENU -> VS path, whatever it was doing.
 *   LAUNCH   at the first poll with the game in GAMEMODE_UPDATE, the VS mode is queued
 *            (gQueuedGameMode + GAMEMODE_CHANGE_START, which re-inits even a VS mode already
 *            running). Until the race, every port is fed a neutral pad.
 *   PIN      at the mode-init checkpoint (n64_sched.c gdx_transition_step_mark "GMI_B..GMI_C",
 *            right before sGamemodeInitFuncs[] runs Race_Init), everything a VS race reads that
 *            a console may hold differently is set from the START packet: players, course, cup,
 *            laps, VS settings, machines, skins, engine (the player globals AND gRacers[0..n-1],
 *            which is what the VS init really reads), the RNG seeds, gGameFrameCount, the VS
 *            points, camera distance, the player->port map, the title-demo and pause flags.
 *   TICK 0   the first poll after that INIT, with the game back in GAMEMODE_UPDATE in the VS
 *            mode. How long each console's transition took is irrelevant: nothing before it
 *            survives into the race except the pinned globals. The per-tick [gstate] digest
 *            proves it (and reports what the pins missed, should anything).
 *   END      deterministically, at the same lockstep tick everywhere: every human racer done
 *            (finished / retired / crashed out) plus GDX_MPG_END_HOLD ticks, or any of the
 *            race's own menu choices (pause -> quit/retry/...), which is rewritten to QUIT.
 *            The main menu is queued; the user's own settings come back at its mode init.
 * Main thread only. */
#ifndef GDX3DS_MP_GAME_H
#define GDX3DS_MP_GAME_H

#include "gdx_mp_lobby.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GDX_MPG_END_HOLD 300 /* ticks after the last human crossed the line: results on screen */

enum {
    GDX_MPG_IDLE = 0,
    GDX_MPG_QUEUE = 1,     /* armed: queue the VS mode at the next GAMEMODE_UPDATE poll */
    GDX_MPG_WAIT_INIT = 2, /* queued: waiting for the mode-init checkpoint */
    GDX_MPG_PINNED = 3,    /* pinned: tick 0 is the first poll back in GAMEMODE_UPDATE */
    GDX_MPG_RACING = 4,
    GDX_MPG_LEAVING = 5,   /* the main menu is (to be) queued; settings restored at its init */
    GDX_MPG_HOP = 6,       /* armed off the main menu: the main menu is queued, the race next */
};

/* Arm a launch from a START packet (saves the user's settings first). */
void gdx3ds_mpg_arm(const GdxMpLbStart* start, int endAfterTicks);
/* At every poll while launching: 1 when THIS poll is lockstep tick 0 (stage -> RACING). */
int gdx3ds_mpg_launch_poll(void);
/* At every lockstep poll while racing, before the exchange of tick `lt`: 1 when the race is over
 * at this tick (deterministic: the same lt on every console). */
int gdx3ds_mpg_race_poll(unsigned int lt);
/* Leave the race / launch for the main menu (an abort or the end): queued as soon as the game
 * is in GAMEMODE_UPDATE; call every frame until gdx3ds_mpg_stage() is IDLE. */
void gdx3ds_mpg_leave(void);
void gdx3ds_mpg_frame(void);
/* A player was removed from the race at lockstep tick `tick` (gdx_mp_remove.h): from then on it
 * counts as done for the race's end (its machine only coasts). */
void gdx3ds_mpg_set_removed(int player, unsigned int tick);
int gdx3ds_mpg_stage(void);
/* mp-menus #10, the shared pause. The pads are exchanged, so a pause is every console's at the
 * same tick; the stock VS pause menu then follows the pauser's pad alone (any player may pause;
 * only the pauser moves the cursor, resumes or ends the race). Every choice but CONTINUE ends
 * the race for everyone. gdx3ds_mpg_pause: 1 while the race is paused, with who paused and the
 * highlighted row. */
typedef struct GdxMpgPause {
    int paused;
    int player;           /* the pauser's player index */
    int option;           /* the highlighted row (0 = CONTINUE) */
    int ready;            /* the menu takes input now (opened, not busy) */
    int endsRace;         /* the highlighted row ends the race */
    const char* optionName; /* "QUIT" */
} GdxMpgPause;
int gdx3ds_mpg_pause(GdxMpgPause* out);
/* The player whose pause-menu choice ended the last race (-1 = it was not ended from the pause
 * menu), and the game's menu change mode it chose. Kept until the next race is armed. */
int gdx3ds_mpg_ended_by(int* choice);
/* The last race's RESULTS for player p: finishing position (1-based, 0 = did not finish) and race
 * time (ms). Kept until the next race is armed. */
int gdx3ds_mpg_result(int player, int* raceTimeMs);
/* The last race's whole field in finishing order (port/gdx_racemap.h), noted at every race poll
 * and kept until the next race is armed: the results screen's places and CPU rows. */
struct GdxRaceFieldSnapshot;
const struct GdxRaceFieldSnapshot* gdx3ds_mpg_last_field(void);
/* Ports the session drives (= START numPlayers), 0 when idle. */
int gdx3ds_mpg_ports(void);
/* MP30: the pinned race's VS COM machine count (0 = stock 4 / no race). */
int gdx3ds_mpg_vs_racers(void);
/* MP30: 1 when racer `racerId` is a ram-mode test bot (START.ramMask, [net] mp_bot = 2). */
int gdx3ds_mpo_ram_racer(int racerId);
/* "VS 3P / Big Blue" style receipt of the armed race. */
const char* gdx3ds_mpg_describe(void);
/* The user's current picks, for the lobby defaults. */
void gdx3ds_mpg_current_picks(int* machine, int* skin, int* engine);
void gdx3ds_mpg_current_settings(GdxMpLobbySettings* out);

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_MP_GAME_H */
