/* port/3ds/gdx3ds_autodrive.c — N60TEX: [debug] autodrive, the game's own CPU driver for player 1.
 *
 * WHY. No corpus replay finishes a race: an open-loop pad script leaves the course within ~15 s
 * (replays/README.md, the time-attack section), so the time trial's FINISH text, the post-finish
 * HUD and the results screen were never seen on the emulator lane. The hardware log was the only
 * evidence for them.
 *
 * WHAT. With `[debug] autodrive = 1`, Racer_UpdateMovement (decomp-3ds-autodrive.patch) drives a
 * player racer with Cpu_GenerateInputs -- the same routine the game already uses for a player
 * after the finish line (cpu.c: `racer->id < gNumPlayers` branch) -- instead of the pad. The pad
 * still drives the menus, so a replay reaches the race and the CPU completes the laps. A
 * MEASUREMENT lever: it changes the game, so both legs of an A/B must carry it.
 *
 * DEFAULT 0 = the pad drives, exactly as before (the hook is one cached int test).
 */

#include "gdx3ds_config.h"
#include "gdx_snapshot.h" /* mp-snapshot: GDX_SNAP_STATE_DATA, gdx_snap_tick_input */

extern void gdx_dbg_logf(const char* fmt, ...);

/* A lockstep session is live ([net] mp, or an online / NEARBY launch or race). Weak: the net
 * library is always linked into the game, but the DL-test harness links this TU without it. */
extern int gdx3ds_mp_active(void) __attribute__((weak));

static int SessionLive(void) {
    return (&gdx3ds_mp_active != 0) && gdx3ds_mp_active();
}

/* ---- MP30: the simulation levers a session forces -----------------------------------------------
 *
 * A measurement lever that changes the race would fork two consoles of one build. While a session
 * is live these read as their neutral value, whatever the ini says, so a console left with one on
 * cannot desync a room. Each getter asks gdx3ds_mp_session_levers_forced() (weak where its TU can
 * be linked without this one). The table is also the sim-ABI word's lever part
 * (gdx3ds_mp_sim_levers_hash, port/3ds/net/gdx3ds_mp_online.c): two builds that force a different
 * set, or to different values, refuse each other in the lobby instead of desyncing in the race.
 *
 * NOT forced: [net] mp_desync_at and mp_xform. They are deliberate lockstep test levers (a
 * planted RNG flip on ONE console, a negated local stick that only enters through the exchanged
 * pad) and must differ between the consoles of a test. */
static const char kForcedLevers[] = "debug.autodrive=0;"                /* gdx_autodrive_racer, player 1 */
                                    "debug.course_force=-1;"           /* gdx3ds_course_force_hook */
                                    "debug.force_lap2=0;"              /* gdx3ds_bottomhud.c, racer.c */
                                    "debug.xition_force=0;"            /* gfx/gdx3ds_hud_capture.c */
                                    "gEnhancements.Practice.PhotoMode=0;"; /* port/input_bridge.c */

int gdx3ds_mp_session_levers_forced(void) {
    static int sNoted = 0;
    const int live = SessionLive();
    if (live && !sNoted) {
        sNoted = 1;
        gdx_dbg_logf("[mp] session: sim levers forced neutral (%s)\n", kForcedLevers);
    }
    return live;
}

/* mppolish [net] mp_fullview_cam (default 1): a live 2P session's race cameras take the 1P
 * framing (port/3ds/game/gdx3ds_fullview_game.c, gdx_fv_cam_reframe). Render-side, but every
 * view's machineLod -- the lockstep digest's "draw" mask -- follows it, so it must match across a
 * room: its value is part of the sim-ABI word below. Read once. */
static int sFullviewCam = -1;

int gdx3ds_mp_fullview_cam_lever(void) {
    if (sFullviewCam < 0) {
        sFullviewCam = gdx3ds_config_get_int("net", "mp_fullview_cam", 1) != 0 ? 1 : 0;
    }
    return sFullviewCam;
}

static unsigned int SimLeversMix(unsigned int h, const char* s) {
    const char* c;
    for (c = s; *c != '\0'; c++) {
        h ^= (unsigned char) *c;
        h *= 16777619u;
    }
    return h;
}

unsigned int gdx3ds_mp_sim_levers_hash(void) {
    unsigned int h = SimLeversMix(2166136261u, kForcedLevers);
    if (gdx3ds_mp_fullview_cam_lever() == 0) {
        h = SimLeversMix(h, "net.mp_fullview_cam=0;"); /* the default (1) leaves the word as before */
    }
    return h;
}

static int sAutodrive = -1;

static int AutodriveLevel(void) {
    if (sAutodrive < 0) {
        sAutodrive = gdx3ds_config_get_int("debug", "autodrive", 0);
        if (sAutodrive != 0) {
            gdx_dbg_logf("[autodrive] armed: player 1 is driven by Cpu_GenerateInputs (measurement lever)\n");
        }
    }
    return sAutodrive;
}

/* mp-online (port/3ds/net/gdx3ds_mp_game.c): racers an online session's START marked as test
 * bots ([net] mp_bot). Weak: 0 when the net library is not linked. */
extern int gdx3ds_mpo_bot_racer(int racerId) __attribute__((weak));

static int MpBot(int racerId) {
    return (&gdx3ds_mpo_bot_racer != 0) && gdx3ds_mpo_bot_racer(racerId);
}

/* MP30 test lever ([net] mp_bot = 2, synced as START.ramMask): an aggressive bot for the 30-machine
 * lockstep tests. It keeps the CPU driver's own boosts, spin and side attacks (the plain bot keeps
 * throttle and steering only), its jittered pad adds R, Z and Z+R on top, and its energy is not
 * pinned: machines bump, attack, boost themselves dry and get destroyed, the paths a calm race
 * never takes. Weak: no net library, no ram bots. */
extern int gdx3ds_mpo_ram_racer(int racerId) __attribute__((weak));

static int MpRamBot(int racerId) {
    return (&gdx3ds_mpo_ram_racer != 0) && gdx3ds_mpo_ram_racer(racerId);
}

int gdx_autodrive_full_buttons(int racerId) {
    return MpRamBot(racerId);
}

/* MP30: a human removed from a session ([net] mp_drop = cpu) whose machine the CPU driver drives
 * from the agreed removal tick T (net/gdx3ds_mp.c; every console asks inside the same tick's
 * update with the same removal tick, so the takeover is lockstep). Throttle and steering like
 * the other autodriven racers, but no pad share and no energy pin: it races (and can be
 * destroyed) like a CPU. Weak: no net library, no takeover. */
extern int gdx3ds_mp_drop_cpu_racer(int racerId) __attribute__((weak));

static int MpDroppedCpu(int racerId) {
    return (&gdx3ds_mp_drop_cpu_racer != 0) && gdx3ds_mp_drop_cpu_racer(racerId);
}

/* Non-zero when racer `racerId` should take the CPU driver's inputs instead of its pad. */
int gdx_autodrive_racer(int racerId) {
    if (MpBot(racerId) || MpDroppedCpu(racerId)) {
        return 1;
    }
    if (racerId != 0) {
        return 0;
    }
    if (gdx3ds_mp_session_levers_forced()) {
        return 0;
    }
    return AutodriveLevel() != 0;
}

/* decomp-3ds-mp-sim.patch + decomp-3ds-mp-vis.patch: EVERY simulation read of the draw-written
 * machineLod (racer.c: the damage floor, the spin-out debris and the crash explosion, the wall,
 * pair and random sparks -- four of them draw Math_Rand1/2 only when "visible"; cpu.c: Billy's
 * boost and the AI's sp70 branch). In a lockstep session every machine counts as visible, so no
 * console's view, skipped frame, stereo eye or LOD lever can change the race.
 *
 * WHY CONSTANT 1 AND NOT A SIM-SIDE FRUSTUM. It is what stock VS already does: Racer_Update sets
 * machineLod = 1 for every racer at the top of every tick whenever gNumPlayers != 1
 * (racer.c "if (gNumPlayers != 1)", before the collision pass), and every one of these reads
 * runs after that line inside the same update. In a 2-4 player race the sim therefore never saw
 * Racer_Draw's value in the first place; this function makes that explicit and keeps it true
 * should a session ever run with gNumPlayers == 1 or a read move outside Racer_Update. So a
 * session spawns the same effects (and draws the same RNG) as a stock VS race with the same
 * racer count -- no gameplay change. The cost scales with the racers: at 30 machines every
 * spin-out and crash spawns its debris (bounded by the effects pool), as stock VS would with 30.
 * A frustum test against the humans' cameras would be cheaper but would change stock VS
 * behaviour and need a camera state every console agrees on; not worth it. Weak: no net
 * library, no session. */
int gdx_sim_machine_visible(int machineLod) {
    if (SessionLive()) {
        return 1;
    }
    return machineLod != 0;
}

/* ---- MP30: the Expansion Kit countdown hold as a lockstep event ---------------------------------
 *
 * Stock EK holds the race countdown at gRaceIntroTimer == 40 for as long as THIS console's disk
 * BGM load is in flight (racer.c, func_807424CC). Load times differ per console, so in a session
 * the race would start on different ticks. Here the decision comes from data every console holds
 * identically: the lockstep pads of the tick being simulated. Each console sets the "BGM ready"
 * bit (GDX_MP_PAD_BGM_READY) in the pads it sends while its own load is done
 * (gdx_mp_local_bgm_ready); the hold releases on the first tick whose pads ALL carry it (a
 * removed player counts as ready), or after GDX_MP_EK_HOLD_CAP held ticks, counted from the
 * simulation's own frame counter. A console whose BGM is still loading at the release just starts
 * its music late. Off a session: the stock local answer. */
#define GDX_MP_EK_HOLD_CAP 180 /* 3 s at 60 ticks/s */

extern int gdx3ds_mp_pads_bgm_ready(void) __attribute__((weak)); /* net: this tick's pads, all ready */

/* mp-snapshot: simulation state (the hold's length decides the release tick), so both live in the
 * snapshot block and a restore rewinds them with the decomp image. */
static int sEkHoldFrom GDX_SNAP_STATE_DATA = -1; /* race frame of the first held tick of the current hold */
static int sEkHoldLast GDX_SNAP_STATE_DATA = -1;

/* mp-snapshot: both inputs of the decision live OUTSIDE the snapshot -- this console's disk BGM load
 * (the audio thread; the caller's func_807424CC) and every pad's "BGM ready" bit (the session) -- so
 * each is a per-tick INPUT: the real tick records it and a resimulated tick replays it
 * (gdx_snap_tick_input is the identity while no snapshot window is open). */
int gdx_mp_ek_bgm_hold(int localLoading, int raceFrame) {
    int allReady;
    int held;
    localLoading = gdx_snap_tick_input(GDX_SNAP_IN_EK_BGM_LOADING, localLoading);
    if (!SessionLive() || &gdx3ds_mp_pads_bgm_ready == 0) {
        return localLoading;
    }
    if (sEkHoldFrom < 0 || raceFrame < sEkHoldFrom || raceFrame != sEkHoldLast + 1) {
        sEkHoldFrom = raceFrame; /* a new hold (a new race restarts the frame counter) */
    }
    sEkHoldLast = raceFrame;
    held = raceFrame - sEkHoldFrom;
    allReady = gdx_snap_tick_input(GDX_SNAP_IN_EK_BGM_READY_ALL, gdx3ds_mp_pads_bgm_ready());
    if (allReady || held >= GDX_MP_EK_HOLD_CAP) {
        if (!gdx_snap_resim_active()) { /* the receipt belongs to the real tick (emu_matrix compares it) */
            gdx_dbg_logf("[mp] ek hold: countdown released at race frame %d after %d held ticks (%s; local load %s)\n",
                         raceFrame, held, allReady ? "every pad says BGM ready" : "cap",
                         localLoading ? "pending" : "done");
        }
        sEkHoldFrom = -1;
        sEkHoldLast = -1;
        return 0;
    }
    return 1;
}

/* ---- MP30: the VS COM machine count ------------------------------------------------------------
 * Race_Init (decomp-3ds-mp-racers.patch) asks this for gTotalRacers when VS COM is on. A session's
 * START carries the count the host picked (4, 8, 16 or 30; gdx3ds_mp_game.c); off a session it is
 * the stock 4. */
extern int gdx3ds_mpg_vs_racers(void) __attribute__((weak)); /* net: 0 = no session / stock */

int gdx_mp_vs_total_racers(int stockCount) {
    const int n = (&gdx3ds_mpg_vs_racers != 0) ? gdx3ds_mpg_vs_racers() : 0;
    if (n < stockCount || n > 30) {
        return stockCount;
    }
    gdx_dbg_logf("[mp] vs com: %d machines (stock %d)\n", n, stockCount);
    return n;
}

/* Non-zero when the autodriven racer should also take a share of its (exchanged) pad. */
int gdx_autodrive_mix(int racerId) {
    return MpBot(racerId) && !MpDroppedCpu(racerId);
}

/* Non-zero when the autodriven racer's energy is kept full (the measurement lever and the test
 * bots, which must reach the finish line); 0 for a dropped human the CPU took over and for a
 * ram-mode bot. */
int gdx_autodrive_pin_energy(int racerId) {
    return !MpDroppedCpu(racerId) && !MpRamBot(racerId);
}

/* ---- n60atlas: [debug] course_force (census lever, default -1 = off) --------------------------------
 *
 * The course a replay races is whatever its menu walk selected. For a per-course census (every
 * course's track materials, docs/research/native/n60atlas-progress.md) one replay is run per course
 * with `[debug] course_force = N` (+ `autodrive = 1`): at the mode-init checkpoint of a race mode
 * (n64_sched.c gdx_transition_step_mark "GMI_B..GMI_C", right before sGamemodeInitFuncs runs
 * Course_Init) gCourseIndex is set to N. 48..53 run the X cup generator (Course_GenerateRandomCourse).
 * A MEASUREMENT lever: it changes the game. */
extern int gCourseIndex;
extern int gGameMode;

static int sCourseForce = -2;

void gdx3ds_course_force_hook(const char* label) {
    if (sCourseForce == -2) {
        sCourseForce = gdx3ds_config_get_int("debug", "course_force", -1);
        if (sCourseForce >= 0) {
            gdx_dbg_logf("[courseforce] armed course=%d (census lever)\n", sCourseForce);
        }
    }
    if (sCourseForce >= 0 && gdx3ds_mp_session_levers_forced()) {
        return;
    }
    if (sCourseForce < 0 || label == 0 || label[0] != 'G' || label[4] != 'B' || label[7] != 'G' || label[11] != 'C') {
        return;
    }
    const int mode = gGameMode & 0x1F;
    if (mode != 1 && mode != 2 && mode != 0xE) { /* GP_RACE, PRACTICE, TIME_ATTACK */
        return;
    }
    gdx_dbg_logf("[courseforce] mode=%d course %d -> %d\n", mode, gCourseIndex, sCourseForce);
    gCourseIndex = sCourseForce;
}
