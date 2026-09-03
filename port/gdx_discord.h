#ifndef GDX_DISCORD_H
#define GDX_DISCORD_H

/* Discord Rich Presence (vendored discord-rpc, port/third_party/discord-rpc).
 *
 * Privacy model (owner decisions): master toggle
 * gEnhancements.Online.DiscordPresence defaults OFF; per-field toggles
 * (ShowCourse/ShowLap/ShowPosition/ShowMode/ShowTimer) default ON once the
 * master is enabled. Course Edit never broadcasts user track names. The
 * timestamp is per-race, never per-session. With the master off, the per-tick
 * cost is one counter increment and (every 30th tick) one CVar read; the
 * library is never initialized and no thread or socket exists.
 *
 * Call order (port/main.cpp): gdx_discord_tick() once per host frame in the
 * PerfTicks window on BOTH loop branches — after gdx_dispatch, so the sampled
 * game state is this frame's post-update truth (gGameMode flips mid-dispatch,
 * see port/input_bridge.c's staleness notes). gdx_discord_shutdown() runs
 * EARLY in the exit path: discord-rpc's IO-thread join can hang on Windows
 * (upstream issue #275), and an early shutdown makes a stall visible instead
 * of leaving a zombie process after the window closed. */

#ifdef __cplusplus
extern "C" {
#endif

/* Game-state sample, filled by gdx_discord_snapshot (port/decomp_port.c),
 * which is the TU with the decomp headers. Plain ints so this header needs no
 * decomp types. */
typedef struct GdxDiscordSnapshot {
    int mode;           /* GET_MODE(gGameMode) */
    int modeChanging;   /* mode transition in flight: suppress updates */
    int titleDemo;      /* attract demo running: never report as racing */
    int courseIndex;    /* COURSE_* */
    int cupType;        /* Cup enum */
    int difficulty;     /* Difficulty enum */
    int numPlayers;
    int totalLaps;
    int paused;
    int playerLap;      /* gRacers[0].lap, 1-based */
    int playerPosition; /* gRacers[0].position, 1-based rank */
    int playerFinished; /* RACER_STATE_FINISHED set on the player */
} GdxDiscordSnapshot;

void gdx_discord_snapshot(GdxDiscordSnapshot* out);

void gdx_discord_tick(void);
void gdx_discord_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* GDX_DISCORD_H */
