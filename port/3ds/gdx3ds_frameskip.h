/* port/3ds/gdx3ds_frameskip.h — FRAMESKIP (NEXT.md C4l): 60 Hz game logic on a console that
 * cannot render 60 frames a second.
 *
 * THE FACT THIS EXISTS FOR. The port has no catch-up: one host-loop iteration = one
 * gdx_vi_tick = one VI retrace message = one logic tick of the game (D_800CCFBC == 1 in play;
 * func_80067D64 / func_80067E98 wait for exactly one 0x29 per tick), and one GFX task rendered
 * inline (renderthread=0). The only pacing owner is citro3d's vblank sync. So when a frame
 * takes 67 ms, the GAME takes 67 ms per tick: the Old 3DS race at ~15 fps runs at ~1/4 speed
 * (slow motion), while the audio producer, paced by its ring and not by frames, plays music at
 * wall speed. Hardware receipt: `[watchdog]` beats every 5 s show +72..+84 host frames per beat
 * in the race (14-17 ticks/s) against aud=+~300 audio ticks per beat (60/s), and `[mprof]
 * tasks=64` per 64 frames (one task per tick). docs/research/native/o3dsskip-progress.md §1.
 *
 * WHAT IT DOES. Per host iteration (the decision is taken on MAIN, after the input poll and
 * before gdx_vi_tick), the governor says RENDER or SKIP. A SKIP iteration still runs the whole
 * logic tick -- VI post, the game fibers, Audio_Update, the display-list BUILD, the save tick,
 * every counter -- and drops only the expensive part: osSpTaskStartGo acks the GFX task's SP and
 * DP done without running gdx_gfx_run (the bridge prepare + the owned interpreter's DL walk +
 * citro3d submission), and the host skips StartFrame / present fallback / EndFrame. Nothing is
 * presented; the screen keeps the last rendered frame, exactly as a VI re-scanning an old
 * framebuffer would. The game cannot tell: on the N64 the DP-done is the only thing its frame
 * protocol waits for (sys_gfx.c), and it arrives.
 *
 * Modes, [debug] frameskip (DEFAULT 0 = off, byte-identical host loop):
 *   N (1..9)  FIXED: render host frames with frame % (N+1) == 0, skip the others. Phase-locked
 *             to the frame number, so two runs render the SAME frames -- the parity mode.
 *             Iterations ahead of the 59.94 Hz schedule sleep to it (skips never wait on a
 *             vblank, and a fast scene must not run the game at (N+1)x).
 *   steady    EVEN CADENCE: one render per D vblanks, D in 1..4 adapted with hysteresis, the
 *             present held to the slot's vblank (see the STEADY section below).
 *   auto      ADAPTIVE: skip while the loop is at least one logic tick behind the 59.94 Hz wall
 *             schedule, at most [debug] frameskip_max (default 5) in a row, render otherwise.
 *             A scene the console renders at 60 never skips.
 * Either mode NEVER skips a frame the oracle will capture (pending SHOT/CAPTURE, or an
 * [debug] fh_every sample): the capture would hash a frame that was not drawn.
 * Only with the render thread off (rtMode 0 -- always the case on the Old 3DS, which has no
 * core 2); with it on the lever is refused and the receipt says so.
 *
 * Receipts: `[frameskip] mode=... ` once at init; with the lever on, every 256 host frames
 * `[frameskip] frame= n= dtMs= rendered= skipped= veto= tps= fps= maxrun= lagmaxms=` --
 * tps = logic ticks per wall second, fps = rendered (presented) frames per wall second. */
#ifndef GDX3DS_FRAMESKIP_H
#define GDX3DS_FRAMESKIP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    GDX_FRAMESKIP_OFF = 0,
    GDX_FRAMESKIP_FIXED = 1,
    GDX_FRAMESKIP_AUTO = 2,
    GDX_FRAMESKIP_STEADY = 3,
};

/* ---- the pure governor (no 3DS dependency; host-tested by gdx3ds_frameskip_host_tests) ---- */

typedef struct GdxFrameskipGov {
    int mode;          /* GDX_FRAMESKIP_* */
    int maxSkip;       /* FIXED: skipped frames between renders; AUTO: cap on consecutive skips */
    uint64_t interval; /* one logic tick, in clock units */
    uint64_t due;      /* when the tick being decided was scheduled to start */
    int armed;         /* due is valid */
    int skipRun;       /* consecutive skips so far */
} GdxFrameskipGov;

typedef struct GdxFrameskipDecision {
    int skip;            /* 1: run the logic tick, drop its GFX task and its present */
    int vetoed;          /* 1: the governor wanted to skip and the veto forced a render */
    uint64_t sleepTicks; /* FIXED, ahead of schedule: wait this long before the tick */
    uint64_t lag;        /* how far behind schedule this tick started (0 when ahead) */
} GdxFrameskipDecision;

void gdx_frameskip_gov_init(GdxFrameskipGov* gov, int mode, int maxSkip, uint64_t interval);

/* One call per host iteration, in order. `now` is the clock, `frame` the host frameNum,
 * `veto` nonzero forces a render. Advances the schedule by one tick. */
GdxFrameskipDecision gdx_frameskip_gov_decide(GdxFrameskipGov* gov, uint64_t now, unsigned long frame, int veto);

/* STEADY's per-slot glue state (the vblank anchor and the held present). Lives here, not as
 * glue statics, so the live switch below is host-tested against the same fields. */
typedef struct GdxFrameskipSlot {
    uint64_t vblank;        /* the vblank this slot started on (0 = not anchored yet) */
    uint64_t renderBegin;   /* paced render iteration: work starts here */
    uint64_t lastBeginTick; /* the previous iteration's decision tick (skip-cost sample) */
    int d;                  /* D of the slot in progress: a D change applies from the NEXT slot */
    int catchup;            /* extra skip ticks this slot owes for vblanks a late start lost */
    int beginMissed;        /* the slot's start vblank had already passed */
    int pacedRender;        /* 1 between a slot render's start and its held present */
    int slotPaced;          /* C6: the slot in progress opened with a paced render (kept through its skips) */
    int lastWasSkip;
} GdxFrameskipSlot;

void gdx_frameskip_slot_reset(GdxFrameskipSlot* slot, int d);

/* LIVE MODE SWITCH (the DBG-tab FRAMESKIP entry). Called only at the safe point between two
 * host iterations -- the top of the next iteration, before its decision -- so no GFX task is
 * armed for a drop and no present is held. It still clears both defensively: the schedule is
 * re-armed at the next decision (no debt, no credit carried across modes), the slot is
 * un-anchored with no hold pending, and the first decision after the switch RENDERS (skipRun
 * starts at the cap) so every mode starts from a freshly presented frame. `maxSkip` as for
 * gdx_frameskip_gov_init (STEADY: D-1). */
void gdx_frameskip_live_switch(GdxFrameskipGov* gov, GdxFrameskipSlot* slot, int mode, int maxSkip);

/* ---- STEADY: the even-cadence divisor controller (pure; host-tested) ----
 *
 * `frameskip = steady` renders one frame in D (D in [steady_min, steady_max], default 1..4) and
 * DISPLAYS it on a fixed vblank grid: the render iteration starts on the slot's first vblank S,
 * the present is held until vblank S+D-1 so the swap latches at exactly S+D, and the D-1 skip
 * ticks run in the remaining time. Every frame is on screen for exactly D vblanks, so the
 * picture moves at 60/D Hz with no 1..5 run-length jitter (the hardware `auto` verdict:
 * "playable but nausea inducing"). Logic stays one tick per vblank.
 *
 * The slot fits when   rw + T <= D*P   and   T <= P,   T = max(M, (D-1)*L)
 * (rw = render-iteration work: its logic tick + the DL walk + citro3d build, measured from the
 * slot start to the present, hold excluded; L = one skip tick; P = one vblank; M = the GPU's
 * draw + display transfer after C3D_FrameEnd, 4 ms from the hardware [gpu] gpu= column). The
 * second term is the hold's own cost: after the present at S+D-1 both the GPU and the skips
 * must finish by S+D.
 *
 * Hysteresis: DOWN (bigger D, lower fps) is fast -- two missed slots within the last 8, when
 * the p75 of the last 8 renders does not fit D either (isolated load hitches miss a slot but do
 * not change D), jump straight to the smallest D the recent p90 fits at 95% of the slot. UP
 * (smaller D) is slow -- only after ~2 s at the current D with no miss in the last 16 slots, and
 * only if the p90 of the last 32 renders fits D-1 at 80% of the slot (real headroom, not a lucky
 * second). A step up that is undone within twice its dwell doubles the dwell (cap ~16 s); ~20 s
 * at one D resets it. Skip-tick samples are clamped to one period (a stalled skip is not the
 * cost of a skip).
 *
 * ROBUST (HITCH, [debug] steady_robust, DEFAULT 1; 0 = the rules above). hw-rel2 sat at D=4 for
 * the whole race although rw50/rw90 fit D=3: an SD flush every ~6 slots missed a slot (so no
 * 16 clean slots ever) and its 80-515 ms renders sat in the p90. The robust rules classify each
 * slot: a SPIKE is a render over 2x the ring median with at most one more in the last 8 slots,
 * or a skip tick more than half a vblank over the skip average; a miss in a spike slot or the
 * slot after one is EXPLAINED, every other miss is HARD. DOWN needs 2 hard misses in 8 (spike
 * renders left out of the p75/p90), or 6 misses in 8 of any kind (a flood is load). UP needs no
 * hard miss and at most 4 explained ones in 16, at most 8 spikes in the ring, and the spike-free
 * p90 fitting D-1 at 80%. Stalled skip ticks stay out of the skip average (unless 4 in a row).
 * The cadence contract is unchanged: every drawn frame is on screen for exactly D vblanks.
 *
 * A slot that STARTS late (its vblank already passed: a load hitch) repays the vblanks it lost
 * as extra skip ticks in that slot, as many as fit in half a vblank beside its own D-1 (cap 4),
 * so a hitch costs game time only when it cannot be repaid without making the next slot late.
 *
 * The vblank wait is C3D_FrameSync (citro3d's own), not gspWaitForVBlank; the lever sets gpuprof
 * external pacing so FrameBegin does not sync a second time. [debug] steady_align=0 /
 * steady_hold=0 disable the slot-start wait / the present hold (bisect levers; default 1).
 * Receipts: `[frameskip] steady D a->b (down|up) rw= rw90= cln90= skip= missBits= hardBits=` on
 * every change, and per 256 frames `[frameskip] steady frame= D= hz= dchg= missed= catchup= rw50=
 * rw90= rwmax= skip= hard= spikes= cln90=` (cln90 = the spike-free p90 the robust rules use). */

#define GDX_STEADY_RING 32

typedef struct GdxSteadyCtl {
    int d;               /* current divisor */
    int dMin;
    int dMax;
    uint64_t period;     /* one vblank, clock units */
    uint64_t margin;     /* a present later than this before its vblank misses it (GPU time) */
    uint64_t rw[GDX_STEADY_RING];
    int rwN;
    int rwPos;
    uint64_t skipAvg;    /* EMA of one skip tick's cost (0 = unknown) */
    uint32_t missBits;   /* bit0 = the latest slot */
    unsigned long ticksAtD; /* logic ticks since the last D change */
    unsigned long upDwell;  /* ticks at D required before a step up (doubles when one fails) */
    int lastWasUp;          /* the last change was a step up */
    /* HITCH robust mode ([debug] steady_robust, default 1; gdx_steady_init turns it on). */
    int robust;
    uint32_t hardMissBits;  /* misses no spike explains (bit0 = the latest slot) */
    uint32_t overBits;      /* renders over 2x the ring median (bit0 = the latest slot) */
    uint32_t rwSpikeMask;   /* ring positions holding an isolated-spike render (excluded from p75/p90) */
    int prevSlotSpike;      /* the previous slot was a spike slot: it may have delayed this one */
    int lastSpike;          /* the latest slot was a spike slot (receipts) */
    int lastExplained;      /* the latest slot missed and a spike explains it (receipts) */
    int skipStallRun;       /* consecutive skip samples kept out of skipAvg as stalls */
    unsigned upPct;         /* a step up needs the p90 render to fit D-1 at this % (default 80) */
    unsigned d1HoldPct;     /* o3ds60: D = 1 holds while this % of the last d1HoldSlots slots hit (0 = off) */
    unsigned long d1Holds;  /* o3ds60: slots the D = 1 hold kept D = 1 the old rules would have left */
    unsigned d1HoldSlots;   /* o3ds60: the hold's window, 16..64 slots (init = 16) */
    uint64_t d1MissBits;    /* o3ds60: missBits over 64 slots (bit0 = the latest slot) */
    unsigned long d1RetryTicks; /* o3ds60: D = 2 -> 1 dwell (0 = the stock up rule and backoff) */
    unsigned d1RetryPct;    /* o3ds60: ...and the spike-free p90 render must fit this % of a vblank */
    unsigned long d1Retries; /* o3ds60: steps up to D = 1 taken by the retry rule */
} GdxSteadyCtl;

enum {
    GDX_STEADY_HOLD = 0,
    GDX_STEADY_DOWN = 1, /* D increased after misses */
    GDX_STEADY_UP = 2,   /* D decreased on headroom */
};

void gdx_steady_init(GdxSteadyCtl* ctl, int dMin, int dMax, int dStart, uint64_t period, uint64_t margin);
void gdx_steady_note_skip(GdxSteadyCtl* ctl, uint64_t skipCost);
/* One call per completed slot (render iteration). Returns GDX_STEADY_* and may change ctl->d. */
int gdx_steady_note_slot(GdxSteadyCtl* ctl, uint64_t renderWork, int missed);
/* The same with the skip ticks' worst cost since the previous render: a skip tick more than half
 * a vblank over the skip average is a stalled skip (the hardware's SD flush), and a miss it
 * explains is forgiven by the robust rules. gdx_steady_note_slot = skipMax 0. */
int gdx_steady_note_slot_ex(GdxSteadyCtl* ctl, uint64_t renderWork, int missed, uint64_t skipMax);
/* 1 (default) = the robust rules above; 0 = the round-2 rules (bisect lever). */
void gdx_steady_set_robust(GdxSteadyCtl* ctl, int on);
/* o3dsperf: the step-up fit percentage (50..100; init = 80). [debug] steady_lockstep_up_pct sets
 * it while a lockstep race is on. */
void gdx_steady_set_up_pct(GdxSteadyCtl* ctl, unsigned pct);
/* o3ds60 [debug] steady_d1_hold_pct (default 80 on an Old 3DS, 0 = off on a New 3DS): at D = 1
 * the step-down rules wait until fewer than this % of the last 16 slots hit (a missed slot repeats
 * one frame; a step down halves the rate for the whole dwell). 0 = the rules as before. */
void gdx_steady_set_d1_hold_pct(GdxSteadyCtl* ctl, unsigned pct);
/* o3ds60 [debug] steady_d1_hold_slots (default 64 on an Old 3DS, 16 on a New 3DS; 16..64): the
 * window of the D = 1 hold above. A stint younger than the window counts its unseen slots as hits,
 * so a short burst of misses (one repeated frame each) does not leave D = 1; only a hit rate under
 * steady_d1_hold_pct over the window does. hw 9088d3a (3D, 2 machines): stints at 93-97% hit left
 * D = 1 on 4 misses in a row (the 16-slot window at 80% allows 3). */
void gdx_steady_set_d1_hold_slots(GdxSteadyCtl* ctl, unsigned slots);
/* o3ds60 [debug] steady_d1_retry_ms / steady_d1_retry_pct (default 2000 / 85 on an Old 3DS, 0 = off
 * on a New 3DS): at D = 2 with D = 1 allowed, step up after this long (in ticks here; a failed try
 * doubles it once, to 2x at most) when no hard miss is in the last 16 slots and the spike-free p90
 * render fits pct% of a vblank. The stock up rule needs p90 + the 4 ms present margin to fit 80%
 * (p90 <= ~9.3 ms) and backs off to 16 s: hw 9088d3a sat at D = 2 for whole windows with p90
 * 8-12 ms. A miss at D = 1 repeats one frame, so a try that fails costs ~nothing against D = 2. */
void gdx_steady_set_d1_retry(GdxSteadyCtl* ctl, unsigned long ticks, unsigned pct);
/* o3ds3d [debug] stereo_mp_lock30: set the floor of D (clamped to 1..dMax). A D under the new floor
 * moves up to it at once (its miss history cleared, no backoff); nothing steps below it until the
 * floor is lowered again, so with dMin = 2 neither the stock up rule nor the D = 1 retry runs. */
void gdx_steady_pin_min(GdxSteadyCtl* ctl, int dMin);
/* Percentile of the ring with the isolated-spike renders left out. */
uint64_t gdx_steady_rw_pct_clean(const GdxSteadyCtl* ctl, int pct);
/* Percentile (0..100) of the render-work ring; 0 when empty. */
uint64_t gdx_steady_rw_pct(const GdxSteadyCtl* ctl, int pct);
/* Smallest D in [dMin, dMax] whose slot fits `rw` at margin num/den (dMax when none does). */
int gdx_steady_pick(const GdxSteadyCtl* ctl, uint64_t rw, unsigned num, unsigned den);
/* NETTICK: 1 when a slot start `late` ticks past its vblank would have made that vblank (within
 * half a period, the start's own tolerance) had the lockstep pad wait `netWait` not held it: the
 * peers held this console, its own work was on time ([debug] steady_lockstep_netmiss). */
int gdx_steady_late_by_net(uint64_t late, uint64_t netWait, uint64_t period);
/* o3ds-mp16 [debug] steady_d1_drift: the vblank a D = 1 slot's frame reaches the display -- the
 * first vblank after the slot's own (slotVblank + period) that the present (`now`) still makes with
 * `margin` to spare -- and whether that frame REPEATED the one before (it reached the display 2 or
 * more vblanks after the previous slot's frame: one vblank showed the old frame again). A D = 1
 * console whose slots cost a little over a vblank drifts: every start is later than the last, the
 * start rule ("its vblank passed by half a period") calls ~7 slots in a row missed, yet only one
 * vblank in ~14 repeats a frame. prevDisplay 0 = no previous slot (never a repeat). The margin is the
 * present rule's conservative GPU allowance, so a present inside it counts as reaching the vblank
 * after: repeats are an upper bound, and steady_d1_drift only ever forgives a start-rule miss that
 * is not one (it never adds a miss). */
uint64_t gdx_steady_d1_display(uint64_t slotVblank, uint64_t now, uint64_t margin, uint64_t period);
int gdx_steady_d1_repeats(uint64_t prevDisplay, uint64_t display, uint64_t period);

/* ---- RACE0: every tick in races, the other mode everywhere else (pure; host-tested) ----
 *
 * `[debug] frameskip = race0` (n60fs). On the Old 3DS the NRR race renders at 45-60 fps, so
 * rendering every tick (frameskip 0) gives a smooth race at (nearly) full speed, while the menus,
 * results screens and transitions still run on the legacy interpreter at ~45 ms a frame: with
 * frameskip 0 they ran in slow motion (10-15 fps, ~1/3 speed). race0 switches between OFF and
 * `[debug] race0_other = auto | steady` (default auto) at the between-iterations safe point.
 *
 * RACE SCOPE (read from the game each tick, gdx3ds_frameskip_game_race_scope): GET_MODE(gGameMode)
 * is an on-track race mode (GP, practice, VS 2P/3P/4P, time attack, death race -- the title
 * attract demo races included), no mode change is queued (gQueuedGameMode == gGameMode) and the
 * mode-change machine is idle (gGameModeChangeState == GAMEMODE_UPDATE). So the race intro,
 * countdown, the race, pause, retire, FINISH and the in-race results all belong to the race; the
 * wipes in and out, loads, and every menu mode do not. The raw scope is debounced: it must hold
 * for `scope_ticks` consecutive ticks ([debug] race0_scope_ticks, default 8) to flip.
 *
 * COST GUARD (in scope only): the results/records overlays, the pause/retire menus and the
 * split-screen modes still draw through the legacy path; every-tick rendering is only worth it
 * when a frame fits ~2 vblanks. Over the last GDX_RACE0_RING rendered in-scope iterations: if
 * the p25 is >= heavy ([debug] race0_heavy_ms, default 33; 0 = guard off) the race leaves OFF
 * (12 of 16 frames slower than 30 fps: a hitch or a texture burst never trips it); once tripped,
 * a p75 <= light ([debug] race0_light_ms, default 25) brings it back. Each change clears the
 * ring, so a decision always rests on 16 fresh frames. The guard resets on every scope change.
 *
 * Frameskip never changes game logic in any mode (o3dsskip-progress.md §3); race0 only chooses
 * which mode runs. */

#define GDX_RACE0_RING 16

typedef struct GdxRace0Ctl {
    int racing;        /* 1: every tick renders (OFF); 0: the other mode */
    int scope;         /* debounced race scope */
    int scopeRun;      /* consecutive ticks the raw scope disagreed with `scope` */
    int scopeTicks;    /* ticks of disagreement that flip `scope` */
    int heavy;         /* the cost guard is tripped */
    uint64_t heavyAt;  /* p25 at/above this trips the guard (0 = guard off) */
    uint64_t lightAt;  /* p75 at/below this releases it */
    uint64_t rw[GDX_RACE0_RING];
    int rwN;
    int rwPos;
    uint64_t guardPct;  /* receipts: the p25 (trip) / p75 (release) the latest guard change read */
    int dwellTicks;     /* n60stereo [debug] race0_dwell: in-scope ticks a trip holds at least (0 = off) */
    int trips;          /* guard trips in this scope (each doubles the dwell, up to 8x) */
    int heavyTicks;     /* in-scope ticks since the latest trip */
} GdxRace0Ctl;

enum {
    GDX_RACE0_HOLD = 0,
    GDX_RACE0_TO_EVERY = 1, /* switch to OFF: every tick renders */
    GDX_RACE0_TO_OTHER = 2, /* switch to race0_other */
};

enum {
    GDX_RACE0_WHY_NONE = 0,
    GDX_RACE0_WHY_SCOPE_ENTER = 1,
    GDX_RACE0_WHY_SCOPE_EXIT = 2,
    GDX_RACE0_WHY_HEAVY = 3,
    GDX_RACE0_WHY_LIGHT = 4,
    GDX_RACE0_WHY_POST_FINISH = 5, /* n60results: the player(s) finished (race0_post_finish) */
};

void gdx_race0_init(GdxRace0Ctl* ctl, int scopeTicks, uint64_t heavyAt, uint64_t lightAt);
/* n60stereo: ANTI-FLAP DWELL. Once the cost guard trips, the light release also needs `dwellTicks`
 * in-scope ticks since the trip (every trip the same; a scope change resets the count). Under the
 * fallback the measured work is lighter than the every-tick work that tripped it (the ring then
 * holds rendered iterations of a skipping mode), so without a dwell the guard flaps. 0 = off. */
void gdx_race0_set_dwell(GdxRace0Ctl* ctl, int dwellTicks);
/* The dwell the current trip must hold (0 when not tripped or the dwell is off). */
int gdx_race0_dwell_now(const GdxRace0Ctl* ctl);
/* One call per host iteration. `rawScope` = the game is in race scope this tick; `sample` = the
 * work of the previous iteration when it rendered in scope (0 = none). Returns GDX_RACE0_* and
 * the reason (GDX_RACE0_WHY_*) in *why. */
int gdx_race0_step(GdxRace0Ctl* ctl, int rawScope, uint64_t sample, int* why);
/* n60results [debug] race0_post_finish: as gdx_race0_step, plus `postFinish` = every human player
 * has finished (the FINISH banner on: the lap times, results, standings, name entry and records
 * screens). While it holds the race is out of scope at once -- no debounce, no cost guard, no dwell
 * (reason GDX_RACE0_WHY_POST_FINISH when it leaves) -- and it cannot re-enter. */
int gdx_race0_step_post(GdxRace0Ctl* ctl, int rawScope, int postFinish, uint64_t sample, int* why);
/* Percentile (0..100) of the sample ring; 0 when empty. */
uint64_t gdx_race0_pct(const GdxRace0Ctl* ctl, int pct);
const char* gdx_race0_why_name(int why);

/* port/3ds/game/gdx3ds_frameskip_game.c (decomp types): 1 when the game is in race scope (above);
 * *gameMode = GET_MODE(gGameMode), *changeState = gGameModeChangeState, for the receipts. */
int gdx3ds_frameskip_game_race_scope(int* gameMode, int* changeState);
/* n60stereo: 1 once the race proper runs (intro camera + countdown over). With [debug] race0_arm
 * (default 1) the cost guard samples only then: the intro's heavy frames never trip it. */
int gdx3ds_frameskip_game_racing(void);
/* n60results: 1 when every human racer (i < gNumPlayers) has crossed the line (RACER_STATE_FINISHED,
 * racer.c) in an on-track race mode. */
int gdx3ds_frameskip_game_post_finish(void);

/* ---- 3DS glue (main thread) ---- */

/* After gdx3ds_config_load and gdx3ds_rt_init: latches [debug] frameskip / frameskip_max,
 * refuses when rtMode != 0, emits the init receipt. */
void gdx3ds_frameskip_init(int rtMode);

/* NETTICK: a lockstep race starts / ends (gdx3ds_mp_online.c SetPhase). While on, the steady
 * present hold is off ([debug] steady_lockstep_hold = 1 keeps it) and the cost samples leave out
 * the pad wait reported below. */
void gdx3ds_frameskip_set_lockstep(int on);
/* NETTICK: this iteration's pad exchange waited `ticks` (ARM11 ticks) for a peer. Two callers,
 * summed: WaitReady's wait for the peers' pads (gdx3ds_p2p_wait_rx: the socket service thread's
 * rx event, or the inline / local-wireless 0.5 ms poll) and the mp-timesync pacing sleep before
 * it (gdx3ds_mpts_pace: the console that is ahead stretches the tick). Neither is this console's
 * cost. */
void gdx3ds_frameskip_note_net_wait(uint64_t ticks);

/* 1 when the lever is on (either mode). */
int gdx3ds_frameskip_enabled(void);

/* QUICKFIX [overlap] cadence: how many SKIP iterations came immediately before the latest
 * rendered one (0 = this render follows a rendered tick: the previous frame's GPU work may still
 * be running, the case the overlap kick hides). Steady D=N reads N-1, fixed N reads N, auto reads
 * the run it just skipped. 0 whenever the lever is off (every tick renders). */
int gdx3ds_frameskip_render_gap(void);

/* DBG-tab live control. `available` is 0 when the render thread is on (rtMode != 0: the lever
 * is refused, live as at init). request_mode(OFF|AUTO|STEADY) latches the change; it is applied
 * at the top of the next begin_iteration (gdx_frameskip_live_switch) and emits the `[frameskip]`
 * mode line. Returns 0 when refused. mode() reports the pending mode once one is latched. */
int gdx3ds_frameskip_available(void);
int gdx3ds_frameskip_request_mode(int mode);
int gdx3ds_frameskip_mode(void);

/* FPS row: "off" / "auto" / "steady D3" / "fixed 3" (no allocation). */
void gdx3ds_frameskip_mode_label(char* buf, unsigned cap);

/* Monotonic count of SKIP iterations (nothing presented). Drawn frames over a window =
 * host iterations - the delta of this counter. */
unsigned long gdx3ds_frameskip_skipped_total(void);

/* Host loop, after the input poll and before gdx_vi_tick: decide this iteration, sleep if the
 * FIXED schedule says so, and arm the task drop. Returns 1 for a SKIP iteration. */
int gdx3ds_frameskip_begin_iteration(unsigned long frame);

/* Host loop, after the iteration's dispatch: disarm the task drop, fold the window receipt. */
void gdx3ds_frameskip_end_iteration(unsigned long frame);

/* Host loop, rtMode 0, a RENDERED iteration, after the present fallback and immediately
 * before EndFrame: STEADY holds the present to its slot's vblank and scores the slot. A no-op
 * in every other mode. */
void gdx3ds_frameskip_before_present(void);
/* o3ds60: right after the paced render's EndFrame ([steadyd1] fe= : EndFrame's share of post). */
void gdx3ds_frameskip_after_present(void);

/* HITCH receipt: the steady slot's D (0 when steady is off) and whether the latest scored slot
 * missed its vblank. */
void gdx3ds_frameskip_hitch_info(int* d, int* late);

/* port/n64_sched.c osSpTaskStartGo, GFX tasks only: 1 = ack this task without running it. */
int gdx3ds_frameskip_drop_task(void);
/* C5TICK (gdx3ds_c5tick.h): 1 if the iteration after this one (host frame `nextFrame`) is certain
 * to skip, i.e. the display list being built now will be dropped unwalked. */
int gdx3ds_frameskip_next_skips(unsigned long nextFrame);
/* C6 [replay] (gdx3ds_replay.h), read after this iteration's decision: the number of ticks the
 * current steady slot skips (D - 1 + catch-up; 0 when not steady, or the slot's render was not
 * a paced one), and in *skipRun the skips taken so far in it (0 on its render iteration). The
 * replay records a render, and asks c5tick to keep a tick's build, from these. */
int gdx3ds_frameskip_replay_plan(int* skipRun);
/* C5TICK: iterations the governor wanted to render but a hollow list forced to skip (an
 * unpredictable capture request); expected 0. */
unsigned long gdx3ds_frameskip_hollow_forced(void);

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_FRAMESKIP_H */
