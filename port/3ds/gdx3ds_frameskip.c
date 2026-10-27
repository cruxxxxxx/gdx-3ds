/* port/3ds/gdx3ds_frameskip.c — FRAMESKIP 3DS glue: lever, clock, task drop, receipts.
 * Contract in gdx3ds_frameskip.h; the decision itself is gdx3ds_frameskip_gov.c. */
#include "gdx3ds_frameskip.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <3ds.h>
#include <citro3d.h>

#include "gdx3ds_config.h"
#include "gdx3ds_filelog.h"
#include "gdx3ds_hitch.h"
#include "gdx3ds_mainprof.h"
#include "gdx3ds_oracle.h"
#include "gfx/gdx3ds_replay.h" /* C6: the skipped tick is presented at the next slot's first vblank */
#include "gdx3ds_c5tick.h"
#include "gdx3ds_pipe.h" /* o3ds60: the pipeline levers' A/B clock and lockstep edge */
#include "gdx3ds_cpucut.h" /* o3ds-mp16: steady_d1_drift rides the [cpucut] A/B block clock */
#include "gfx/gdx3ds_prewrite.h"          /* o3ds3d: [debug] prewrite / gpumem race scope */
#include "vendor/citro3d/gdx/c3d_gdx.h"   /* o3ds3d: C3Dgdx_FrameQueueIdle ([debug] ceiling_probe) */

/* o3ds60 [steadyd1]: the bottom dash's latest tick cost (weak: a build without the dash reads 0). */
extern uint64_t gdx3ds_bottomhud_take_tick_ticks(void) __attribute__((weak));
/* o3ds3d [lock30] / [ceiling]: the right eye is on (gfx/gdx3ds_stereo_frame.cpp; weak: host builds). */
extern int gdx3ds_stereo_active(void) __attribute__((weak));

#define GDX_FRAMESKIP_FIXED_MAX 9
/* 5: the hardware GP race renders at 67.8 ms and a skipped tick costs ~4.3 ms ([mprof] logic +
 * dlb + seg + host on the 0929 log), so reaching 59.94 needs (67.8 + 4.3k)/(k+1) <= 16.68, i.e.
 * k >= 4.4. The governor only uses as many as the scene needs. */
#define GDX_FRAMESKIP_AUTO_MAX_DEFAULT 5
#define GDX_FRAMESKIP_WINDOW 256u

/* N64 NTSC field rate, 60/1.001 Hz, in ARM11 system ticks (~4,472,806). */
#define GDX_FRAMESKIP_INTERVAL_TICKS (((uint64_t)SYSCLOCK_ARM11 * 1001u) / 60000u)

/* One top-LCD refresh (59.8337 Hz) in ARM11 system ticks (~4,480,915). */
#define GDX_STEADY_PERIOD_TICKS ((uint64_t)((double)SYSCLOCK_ARM11 / 59.8337))
/* A vblank target this close (or closer) counts as reached: waiting would cost a whole period. */
#define GDX_STEADY_GUARD_TICKS ((uint64_t)CPU_TICKS_PER_MSEC)
/* A present issued later than this before the slot's display vblank cannot land on it: the
 * GPU's draw + display transfer run ~3-4 ms after C3D_FrameEnd ([gpu] gpu= on hardware). */
#define GDX_STEADY_PRESENT_MARGIN_TICKS ((uint64_t)CPU_TICKS_PER_MSEC * 4u)

extern void gdx3ds_gpuprof_set_external_pacing(int on); /* gfx/gdx3ds_gpu_prof.h */

static GdxFrameskipGov sGov;
static int sMode = GDX_FRAMESKIP_OFF;

/* STEADY state (main thread only). */
static GdxSteadyCtl sSteady;
static GdxFrameskipSlot sSlot;
static unsigned long sWinCatchup = 0;
static int sSteadyAlign = 1;       /* [debug] steady_align: wait for the slot's start vblank */
static int sSteadyHold = 1;        /* [debug] steady_hold: hold the present to S+D-1 */
static unsigned long sWinMissed = 0;
static unsigned long sWinDChanges = 0;
static uint64_t sWinRwMax = 0;
static int sLastSlotMissed = 0; /* HITCH: the latest scored slot missed its vblank */
static uint64_t sSlotSkipMax = 0; /* HITCH: the worst skip tick since the last paced render */
static unsigned long sWinHardMissed = 0;
static unsigned long sWinNetLate = 0;    /* NETTICK: late slot starts the pad wait explains (not misses) */
static unsigned long sWinSpikes = 0;
static int sSteadyLastHard = 0;
static int sEnabled = 0;
static volatile int sDropTasks = 0; /* armed for exactly one SKIP iteration, main thread */
static int sRtMode = 0;
static int sFixedSkip = 0;          /* FIXED's N, latched from the ini (FIXED is ini-only) */
static int sPendingMode = -1;       /* DBG-tab request, applied at the next iteration (-1 = none) */
static unsigned long sSkippedTotal = 0;
static unsigned long sHollowForced = 0; /* C5TICK: renders turned into skips for a hollow list */

/* Window counters (main thread only). */
static uint64_t sWinStartTick = 0;
static unsigned long sWinFrames = 0;
static unsigned long sWinRendered = 0;
static unsigned long sWinSkipped = 0;
static unsigned long sWinVetoed = 0;
static unsigned long sWinDroppedTasks = 0;
static int sWinMaxRun = 0;
static uint64_t sWinMaxLag = 0;
static int sRun = 0;
/* QUICKFIX [overlap] cadence: skip iterations since the latest rendered one (never reset by the
 * receipt window, unlike sRun), and that count as it stood when the latest render was decided. */
static int sSkipsSinceRender = 0;
static int sRenderGap = 0;

/* RACE0 (n60fs, [debug] frameskip = race0; contract in gdx3ds_frameskip.h). Main thread only. */
static int sRace0 = 0;                     /* the meta mode is on */
static int sRace0Other = GDX_FRAMESKIP_AUTO; /* [debug] race0_other: the mode outside races */
static int sRace0Heavy = GDX_FRAMESKIP_AUTO; /* [debug] race0_heavy_mode: a tripped race (default race0_other) */
static int sRace0Arm = 1; /* [debug] race0_arm: the cost guard samples only once the race proper runs */
static int sRace0PostFinish = 1; /* n60results [debug] race0_post_finish: results screens out of scope */
static GdxRace0Ctl sRace0Ctl;
static uint64_t sRace0PrevBegin = 0;       /* the previous iteration's begin tick */
static int sRace0PrevRendered = 0;         /* the previous iteration rendered */
static int sRace0PrevScope = 0;            /* the previous iteration was in raw race scope */
static int sRace0PrevCapture = 0;          /* the previous iteration was an oracle capture (readback) */
static uint64_t sRace0SteadyRw = 0;        /* the latest steady slot's render work (hold excluded) */
static unsigned long sRace0Switches = 0;
static unsigned long sRace0Deferred = 0;   /* switches held one tick for a pending hollow build */
static unsigned long sRace0LastSwitchFrame = 0;

/* NETTICK: lockstep awareness (port/3ds/net/gdx3ds_mp_online.c calls in). In a lockstep race a
 * tick can block in the pad exchange (gdx_controller_poll, before begin_iteration) waiting for a
 * slower peer. That time is the PEER's, not this console's render cost or lateness, but every
 * cost sample here measured begin-to-begin and so took it in: on hardware (90049af, New 3DS
 * host + Old 3DS) the host's race0 cost guard read 35 ms "heavy" frames that were 3 ms of render
 * and 30 ms of waiting, dropped to steady D=2 (19 fps on a console that can draw every tick),
 * then read the steady render alone (3.6 ms, "light") and flipped back every 600 ticks; the Old
 * 3DS's steady skip-cost average read 13 ms for ~2.5 ms ticks. And the steady present hold
 * (present at vblank S+D-1, then run the slot's skips) parks the Old 3DS ~24 ms per slot doing
 * nothing while the peer waits for its pads: the skips then overrun the slot, every slot misses,
 * and the pair ran at 38 ticks/s. With a lockstep race on:
 *   - the wait reported for this iteration (note_net_wait: WaitReady's pad wait, on the socket
 *     service thread's rx event or the inline 0.5 ms poll, plus the mp-timesync pacing sleep
 *     before it) comes out of the race0 sample, the steady skip-cost sample and the AUTO
 *     schedule (the clock did not run late; it was held);
 *   - the steady present hold is off ([debug] steady_lockstep_hold = 1 keeps it): the render
 *     presents as soon as it is drawn and the skips run at once; the slot START still waits for
 *     its vblank, so the 60 Hz schedule and the slot cadence are unchanged. */
static int sLockstep = 0;
static int sLockstepHold = -1;          /* [debug] steady_lockstep_hold (default 0), latched */
static uint64_t sNetWaitPending = 0;    /* reported since the latest begin_iteration */
static uint64_t sWinNetWait = 0;        /* ... summed over the receipt window */
static uint64_t sIterNetWait = 0;       /* ... for the iteration being decided */
/* [debug] steady_lockstep_netmiss (default 0), latched: a steady slot whose START was late only
 * because the lockstep pad wait held it (gdx_steady_late_by_net) is not a missed slot and repays
 * no catch-up ticks (the governor's schedule already moved by that wait). hw 158a13e: the New
 * 3DS host waits 2-20 ms a frame on the Old 3DS's pads; each wait past half a vblank made the
 * slot "missed", and missed slots push steady to a higher D (30 fps) for the dwell. 1 = count
 * them as misses (the earlier rule). */
static int sLockstepNetMiss = -1;

/* o3ds60 [debug] steady_net_excuse (default 1 on an Old 3DS, 0 on a New 3DS), latched with the
 * lockstep levers. hw 5c24be0 (Old 3DS guest, 2-machine race): D = 1 was tried and lost within
 * 2-8 slots although the render fit (D = 1 window rw50 8.1 ms), and one stint ran 250 ticks at
 * 24 fps with netwaitms=2745 of 4318. The peer's pads arrive in 30 Hz pairs ([net]
 * mp_uds_tx_min_ms = 33), so this console sits at the lockstep horizon and waits 16-55 ms on
 * some ticks -- most of them the slot's SKIP ticks ([hitch] r=0 D=1 tl=16-55). Before:
 *   - a wait on a skip tick was not "this iteration's" wait when the next render's start came
 *     late, so that late start was a hard miss and repaid catch-up skips;
 *   - a start late by under half a vblank (pad wait) kept the slot's vblank and judged the
 *     present against it: a net-only miss counted as hard.
 * With the lever on: the pad wait summed over the whole previous slot (its render and skips)
 * explains a late start; a slot that started late by no more than that wait and whose present
 * fits measured from its start is not missed (counted netlate), and the slot re-anchors on the
 * vblank its present can make, so the next slot does not inherit the lateness. 0 = as before. */
static int sNetExcuse = -1;
/* o3ds60 [debug] steady_net_catchup (default 0 on an Old 3DS, 1 on a New 3DS): 0 = no catch-up
 * skips for a late slot start while a pad wait happened in the last 16 ticks. Such a console is
 * AHEAD of its peers (it waits for their pads): catch-up skips only run into the horizon and wait
 * there (hw 5c24be0: D = 1 window catchup=24, then 140, its skips waiting 16-55 ms each), turning
 * D = 1 into one render per pad pair. The lost vblank comes back from the next pad wait. */
static int sNetCatchup = -1;
#define GDX_STEADY_NET_AHEAD_TICKS 16u
/* o3ds60 [debug] steady_net_lead (default 2 on an Old 3DS, 0 on a New 3DS): the most vblanks per
 * lockstep race this console may start a slot LATER than the peers' pads allow, to build a lead
 * over the horizon. hw 5c24be0: the New 3DS host never waited (wait_us=0, stalls=0 every 600
 * ticks) while the Old 3DS guest waited 164-6221 us a tick: the guest runs at the horizon (its
 * tick N starts when pad N arrives), so it alone absorbs every jitter of the 30 Hz pad pairs and
 * the host keeps the whole 6-tick input delay as slack. A slot start that waited for pads
 * (>= 1 ms) takes one more vblank, at most this many times per race and 120 ticks apart: one
 * repeated frame each, then every later tick has that much more time before its pad is due (the
 * host's slack shrinks by the same amount: ~100 ms -> ~67 ms at 2). 0 = never. */
static int sNetLead = -1;
static int sNetLeadShifts = 0;
static unsigned sItersSinceLead = 0;
#define GDX_STEADY_NET_LEAD_GAP_TICKS 120u
static uint64_t sNetSinceSlot = 0;  /* pad wait since the latest slot render began */
static uint64_t sSlotNetWait = 0;   /* ... as it stood when this slot began (its own wait included) */
static unsigned sItersSinceNetWait = 255u;
static uint64_t sSlotStartLate = 0; /* this slot's render began this long after its vblank */
static uint32_t sSlotGsp0 = 0;      /* GSP-wait counter at the slot's start */
static uint64_t sLastPresentTick = 0; /* the latest slot render reached before_present (+hold) */
static uint64_t sSlotPost = 0;      /* own time from the previous render's present to this start */
static int sSlotPostValid = 0;
static uint64_t sSlotEndFrame = 0;   /* o3ds60: the previous render's EndFrame (part of post) */
static uint64_t sPendingEndFrame = 0;
static uint64_t sSlotDrain = 0;      /* o3ds60: the previous render's [overlap] drain (part of fe) */
static uint64_t sPendingDrain = 0;
static int sSlotKicks = 0;
static int sPendingKicks = 0;
/* gfx/gdx3ds_overlap.c: the latest frame end's drain wait and its kicks (weak: host builds). */
extern unsigned long long gdx3ds_overlap_last_drain_ticks(int* kicks) __attribute__((weak));

/* o3ds60 [steadyd1] / [steadymiss]: what every D = 1 slot of the receipt window cost and why the
 * missed ones missed, so a hardware log can tell a pad wait from render work from the post-
 * present tail (EndFrame + dash + telemetry) without a second run. Causes:
 *   begin = the slot's start vblank had passed by over half a vblank (not by a pad wait);
 *   net   = started late by no more than the slot's pad wait (excused with steady_net_excuse);
 *   late  = started late by over 1 ms on its own (the previous slot's post-present tail overran);
 *   work  = started on time; the render itself did not leave the present margin. */
typedef struct SteadyD1Win {
    unsigned long slots, missed, hard, excused, begin, net, late, work, catchupCut;
    unsigned long latePresent; /* o3ds60 steady_d1_rate: presents past the margin, on rate (not misses) */
    uint64_t lateSum, lateMax, netSum, netMax, rwSum, rwMax, postSum, postMax, gspSum, gspMax, dashSum, dashMax;
    uint64_t feSum, feMax;
    uint64_t drainSum, drainMax; /* o3ds60: the [overlap] drain inside EndFrame (fe's GPU wait) */
    unsigned long kickFrames;    /* o3ds60: presents whose frame kicked the GPU mid-frame */
    unsigned long postN;
} SteadyD1Win;
static SteadyD1Win sD1;
static unsigned long sD1CatchupCutTotal = 0;
static int sMissLines = 0;
#define GDX_STEADY_MISS_LINES_PER_RACE 40

/* [debug] steady_lockstep_max (default 3 on an Old 3DS, 0 = no cap on a New 3DS): the steady D
 * ceiling while a lockstep race is on, so the Old 3DS settles at a render every 3rd tick
 * (~20 fps) instead of every 4th (mp_tickmodel near_term_d3: 57.9 ticks/s vs 58.6 at D=4, the
 * same stall count; render 19 ms + 3 x 2.5 ms logic + MP fits the 50 ms slot). 1..4; the race's
 * end restores the configured steady_max. */
static void FrameskipLog(const char* line);
static int ClampInt(int v, int lo, int hi);
static int sLockstepDMax = -1;
static int sSteadyDMaxSaved = 0;

static int LockstepDMaxDefault(void) {
    bool isNew = false;
    if (R_FAILED(APT_CheckNew3DS(&isNew))) {
        return 0;
    }
    return isNew ? 0 : 3;
}

/* In a lockstep race, lower an initialised steady controller's ceiling to the lever (once; the
 * steady mode may start mid-race, when race0 trips into it, so InitSteady calls this too). */
static void CapSteadyForLockstep(void) {
    char line[160];
    int cap;
    if (sLockstepDMax < 0) {
        sLockstepDMax = ClampInt(gdx3ds_config_get_int("debug", "steady_lockstep_max", LockstepDMaxDefault()), 0, 4);
    }
    if (!sLockstep || sLockstepDMax == 0 || sSteady.dMax == 0 || sSteadyDMaxSaved != 0) {
        return;
    }
    cap = sLockstepDMax < sSteady.dMin ? sSteady.dMin : sLockstepDMax;
    if (cap >= sSteady.dMax) {
        return;
    }
    sSteadyDMaxSaved = sSteady.dMax;
    sSteady.dMax = cap;
    if (sSteady.d > cap) {
        sSteady.d = cap;
    }
    snprintf(line, sizeof(line), "[frameskip] lockstep: steady D capped at %d (debug.steady_lockstep_max=%d, was %d)",
             cap, sLockstepDMax, sSteadyDMaxSaved);
    FrameskipLog(line);
}

static void UncapSteadyAfterLockstep(void) {
    char line[96];
    if (sSteadyDMaxSaved == 0) {
        return;
    }
    sSteady.dMax = sSteadyDMaxSaved;
    sSteadyDMaxSaved = 0;
    snprintf(line, sizeof(line), "[frameskip] lockstep: steady D ceiling back to %d", sSteady.dMax);
    FrameskipLog(line);
}

/* o3dsperf [debug] steady_lockstep_up_pct (default 90 on an Old 3DS, 80 = the stock rule on a
 * New 3DS): while a lockstep race is on, a steady step up (D -> D-1) needs the spike-free p90
 * render to fit the smaller slot at this percentage. hw 158a13e: once a heavy stretch pushed the
 * Old 3DS to D=3 it never came back to D=2 (p90 + skip tail had to fit 26.7 of the 33.4 ms slot);
 * the step down still needs the p75 to miss the slot at 100%, so 90 keeps a hysteresis. */
static int sLockstepUpPct = -1;
static unsigned long sD1HoldsSeen = 0; /* o3ds60 [steadyd1] holds=: d1Holds at the last receipt */
/* o3ds60 [debug] steady_d1_rate (default 1 on an Old 3DS, 0 on a New 3DS; 2 = also off lockstep):
 * at D = 1 in a lockstep race, a slot is missed only when its START lost a vblank (the previous
 * slot's work overran its period by more than half a vblank: a repeated frame). A present later
 * than GDX_STEADY_PRESENT_MARGIN_TICKS before the slot's display vblank but on time at its start
 * is counted as late= in [steadyd1] (one more vblank of display latency, not a lost frame: the
 * next slot still starts on its vblank, and the GPU tail runs beside its logic). hw a3b4531: the
 * D = 1 renders took 12.5-14.5 ms, so nearly every slot "missed" the 4 ms margin rule and every
 * D = 1 stint stepped down within 4 slots although its starts were mostly on time. */
static int sD1Rate = -1;

/* o3ds-mp16 [debug] steady_d1_drift (default 0; 1 = on; 2 = a same-run A/B on the [cpucut] block clock,
 * bit GDX3DS_CPUCUT_BIT_D1DRIFT: on in even 2-block pairs, off in odd ones) and steady_d1_drift_hold_pct
 * (default 75, 50..100): in a 2D lockstep race at D = 1, a slot is missed only when its frame REPEATED
 * a vblank (gdx_steady_d1_repeats), not when its start was half a vblank late, and the D = 1 hold
 * uses drift_hold_pct over the 64-slot window. hw card-3 3c0c7b7 (Old 3DS, 2D, VS 2P + 14 CPU): the
 * D = 1 slots cost 15-17 ms of render + ~2 ms of post-present tail, so the starts drifted ~1.2 ms a
 * slot; the start rule called 8-10 slots in a row missed ([steadymiss] cause=begin late=8.8 .. 16.2)
 * and stepped down to 30 Hz every few seconds ("steady D 1->2 (down: missed slots) missBits=0x7f"),
 * though only one vblank in ~14 showed a frame twice (~56 fps). Render cadence only: every tick
 * runs the same logic (lockstep hash, replay parity). Receipt: [d1drift] per A/B stretch. */
static int sD1Drift = 0;
static unsigned sD1DriftHoldPct = 75u;
static uint64_t sD1PrevDisplay = 0; /* the previous D = 1 slot's display vblank (0 = none) */
typedef struct D1DriftArm {
    unsigned long slots, d1Slots, repeats, oldMissed, forgiven, downs, ups, d2Slots;
    uint64_t t0;
} D1DriftArm;
static D1DriftArm sDrift;
static int sDriftArm = -1; /* the arm of the open stretch (-1 = none open) */
static void D1DriftCloseStretch(const char* why);

static int D1RateDefault(void) {
    bool isNew = false;
    if (R_FAILED(APT_CheckNew3DS(&isNew))) {
        return 0;
    }
    return isNew ? 0 : 1;
}

/* o3ds60 [debug] steady_d1_hold_slots / steady_d1_retry_ms / steady_d1_retry_pct (see
 * gdx3ds_frameskip.h): Old 3DS 64 / 2000 / 85, New 3DS 16 / 0 (off) / 85. Old 3DS hold_pct 85
 * with them (80 over 16 slots before). */
static int IsOld3dsModel(void) {
    bool isNew = false;
    if (R_FAILED(APT_CheckNew3DS(&isNew))) {
        return 0;
    }
    return isNew ? 0 : 1;
}

/* o3ds60 [debug] steady_ab (default 0): 1 = lockstep race 1 of the launch runs the D = 1 rules as
 * configured ("new"), race 2 the a3b4531..9088d3a rules ("old": hold 80% over 16 slots, the stock
 * step up and backoff), race 3 and later as configured. [steadyab] race N rule=new|old. */
static int sSteadyAb = -1;
static unsigned sLockstepRaces = 0;
static int sSteadyRuleOld = 0;
static unsigned sCfgHoldPct = 0;
static unsigned sCfgHoldSlots = 16;
static unsigned long sCfgRetryTicks = 0;
static unsigned sCfgRetryPct = 85;
static int sCfgD1Latched = 0;
static unsigned long sD1RetriesSeen = 0;

static void LatchD1Rules(void) {
    const int isOld = IsOld3dsModel();
    int retryMs;
    if (sCfgD1Latched) {
        return;
    }
    sCfgD1Latched = 1;
    sCfgHoldPct = (unsigned) ClampInt(gdx3ds_config_get_int("debug", "steady_d1_hold_pct", isOld ? 85 : 0), 0, 100);
    sCfgHoldSlots = (unsigned) ClampInt(gdx3ds_config_get_int("debug", "steady_d1_hold_slots", isOld ? 64 : 16), 16, 64);
    retryMs = ClampInt(gdx3ds_config_get_int("debug", "steady_d1_retry_ms", isOld ? 2000 : 0), 0, 60000);
    sCfgRetryTicks = (unsigned long) (((uint64_t) retryMs * 60u) / 1000u); /* logic ticks (60 Hz) */
    sCfgRetryPct = (unsigned) ClampInt(gdx3ds_config_get_int("debug", "steady_d1_retry_pct", 85), 50, 100);
    sSteadyAb = gdx3ds_config_get_int("debug", "steady_ab", 0) != 0;
    sD1Drift = gdx3ds_cpucut_clamp(gdx3ds_config_get_int("debug", "steady_d1_drift", 0), 0);
    sD1DriftHoldPct = (unsigned) ClampInt(gdx3ds_config_get_int("debug", "steady_d1_drift_hold_pct", 75), 50, 100);
    gdx3ds_cpucut_lever("d1drift", &sD1Drift, GDX3DS_CPUCUT_BIT_D1DRIFT);
}

/* The D = 1 rules of the current race (the A/B's "old" race 2 or as configured). */
static void ApplyD1Rules(void) {
    LatchD1Rules();
    if (sSteadyRuleOld) {
        gdx_steady_set_d1_hold_pct(&sSteady, 80u);
        gdx_steady_set_d1_hold_slots(&sSteady, 16u);
        gdx_steady_set_d1_retry(&sSteady, 0u, sCfgRetryPct);
    } else {
        gdx_steady_set_d1_hold_pct(&sSteady, sCfgHoldPct);
        gdx_steady_set_d1_hold_slots(&sSteady, sCfgHoldSlots);
        gdx_steady_set_d1_retry(&sSteady, sCfgRetryTicks, sCfgRetryPct);
    }
}

static int LockstepUpPctDefault(void) {
    bool isNew = false;
    if (R_FAILED(APT_CheckNew3DS(&isNew))) {
        return 80;
    }
    return isNew ? 80 : 90;
}

static void ApplyLockstepUpPct(void) {
    if (sLockstepUpPct < 0) {
        sLockstepUpPct = ClampInt(gdx3ds_config_get_int("debug", "steady_lockstep_up_pct", LockstepUpPctDefault()), 50, 100);
    }
    gdx_steady_set_up_pct(&sSteady, sLockstep ? (unsigned)sLockstepUpPct : 80u);
}

static int sLock30Lever = -1; /* o3ds3d [debug] stereo_mp_lock30 (see [lock30] below), latched */
static int sLock30 = 0;       /* ... the open race is pinned */

/* NETTICK: the present hold is off in a lockstep race unless the lever keeps it (or o3ds3d's
 * stereo_mp_lock30 = 1 holds the locked 3D slots). */
static int SteadyHoldOn(void) {
    return sSteadyHold && (!sLockstep || sLockstepHold > 0 || (sLock30 && sLock30Lever == 1));
}

/* NETTICK: a begin-to-begin span with this iteration's lockstep wait taken out. */
static uint64_t LessNetWait(uint64_t span, uint64_t netWait) {
    return span > netWait ? span - netWait : 0;
}


static void FrameskipLog(const char* line) {
    svcOutputDebugString(line, strlen(line));
    gdx3ds_filelog_write(line, strlen(line));
}

static int ClampInt(int v, int lo, int hi) {
    if (v < lo) {
        return lo;
    } else if (v > hi) {
        return hi;
    } else {
        return v;
    }
}

/* "auto" -> AUTO; an integer N >= 1 -> FIXED N; anything else (absent, 0, garbage) -> OFF. */
static int ParseMode(const char* value, int* fixedSkip) {
    char* end = NULL;
    long n;
    if (value == NULL || value[0] == '\0') {
        return GDX_FRAMESKIP_OFF;
    }
    if (strcmp(value, "auto") == 0) {
        return GDX_FRAMESKIP_AUTO;
    }
    if (strcmp(value, "steady") == 0) {
        return GDX_FRAMESKIP_STEADY;
    }
    n = strtol(value, &end, 10);
    if (end == value || n <= 0) {
        return GDX_FRAMESKIP_OFF;
    }
    *fixedSkip = ClampInt((int)n, 1, GDX_FRAMESKIP_FIXED_MAX);
    return GDX_FRAMESKIP_FIXED;
}

static const char* ModeName(int mode) {
    if (mode == GDX_FRAMESKIP_FIXED) {
        return "fixed";
    } else if (mode == GDX_FRAMESKIP_AUTO) {
        return "auto";
    } else if (mode == GDX_FRAMESKIP_STEADY) {
        return "steady";
    } else {
        return "off";
    }
}

/* [debug] steady_min / steady_max (defaults 1 / 4), steady_start (default 2). Returns the
 * gov's maxSkip (D-1). */
static int InitSteady(char* line, size_t cap, const char* source) {
    const int dMax = ClampInt(gdx3ds_config_get_int("debug", "steady_max", 4), 1, 4);
    const int dMin = ClampInt(gdx3ds_config_get_int("debug", "steady_min", 1), 1, dMax);
    const int dStart = ClampInt(gdx3ds_config_get_int("debug", "steady_start", 2), dMin, dMax);
    gdx_steady_init(&sSteady, dMin, dMax, dStart, GDX_STEADY_PERIOD_TICKS, GDX_STEADY_PRESENT_MARGIN_TICKS);
    sSteadyDMaxSaved = 0;
    sLock30 = 0; /* o3ds3d: a fresh controller; the next 3D lockstep slot pins it again */
    CapSteadyForLockstep();
    ApplyLockstepUpPct();
    sSteadyAlign = gdx3ds_config_get_int("debug", "steady_align", 1) != 0;
    sSteadyHold = gdx3ds_config_get_int("debug", "steady_hold", 1) != 0;
    gdx_steady_set_robust(&sSteady, gdx3ds_config_get_int("debug", "steady_robust", 1) != 0);
    ApplyD1Rules();
    sD1HoldsSeen = 0;
    sD1RetriesSeen = 0;
    sD1Rate = ClampInt(gdx3ds_config_get_int("debug", "steady_d1_rate", D1RateDefault()), 0, 2);
    sSlotSkipMax = 0;
    snprintf(line, cap,
             "[frameskip] mode=steady D=%d range=%d..%d period=%lu ticks window=%u align=%d hold=%d robust=%d "
             "d1_hold_pct=%u d1_hold_slots=%u d1_retry_ticks=%lu d1_retry_pct=%u d1_rate=%d rule=%s (%s): one "
             "render per D vblanks, present held to the slot's vblank",
             sSteady.d, dMin, dMax, (unsigned long)GDX_STEADY_PERIOD_TICKS, GDX_FRAMESKIP_WINDOW, sSteadyAlign,
             sSteadyHold, sSteady.robust, sSteady.d1HoldPct, sSteady.d1HoldSlots, sSteady.d1RetryTicks,
             sSteady.d1RetryPct, sD1Rate, sSteadyRuleOld ? "old" : "new", source);
    return sSteady.d - 1;
}

static int MaxSkipFor(int mode) {
    if (mode == GDX_FRAMESKIP_AUTO) {
        return ClampInt(gdx3ds_config_get_int("debug", "frameskip_max", GDX_FRAMESKIP_AUTO_MAX_DEFAULT), 1,
                        GDX_FRAMESKIP_FIXED_MAX);
    } else if (mode == GDX_FRAMESKIP_FIXED) {
        return sFixedSkip;
    } else {
        return 0;
    }
}

static void ResetWindow(uint64_t now) {
    sWinStartTick = now;
    sWinFrames = 0;
    sWinRendered = 0;
    sWinSkipped = 0;
    sWinVetoed = 0;
    sWinDroppedTasks = 0;
    sWinMaxRun = 0;
    sWinMaxLag = 0;
    sRun = 0;
    sWinMissed = 0;
    sWinCatchup = 0;
    sWinDChanges = 0;
    sWinRwMax = 0;
    sWinHardMissed = 0;
    sWinNetLate = 0;
    sWinSpikes = 0;
}

/* The previous mode's tick ran rendered (OFF) or its count is stale: the first render of the new
 * mode follows a rendered tick as far as the [overlap] cadence can know. */
static void ResetRenderGap(void) {
    sSkipsSinceRender = 0;
    sRenderGap = 0;
}

/* Latch `mode` (the refusal is the caller's) and emit its `[frameskip]` receipt. `live` = a
 * DBG-tab switch at the between-iterations safe point: the governor and the slot restart
 * through gdx_frameskip_live_switch (no debt, no held present, first decision renders) and the
 * window receipt restarts. At boot the path is the original one, byte for byte. STEADY owns
 * the vblank alignment, so citro3d's FrameBegin must not also sync (gpuprof external pacing);
 * every other rtMode-0 mode gives the pacing back to FrameBegin. */
static void ApplyMode(int mode, const char* source, int live) {
    char line[320];
    int maxSkip = MaxSkipFor(mode);
    if (mode == GDX_FRAMESKIP_STEADY) {
        maxSkip = InitSteady(line, sizeof(line), source);
    } else if (mode != GDX_FRAMESKIP_OFF) {
        snprintf(line, sizeof(line), "[frameskip] mode=%s max=%d target=59.94Hz interval=%lu ticks window=%u (%s)",
                 ModeName(mode), maxSkip, (unsigned long)GDX_FRAMESKIP_INTERVAL_TICKS, GDX_FRAMESKIP_WINDOW, source);
    } else {
        snprintf(line, sizeof(line), "[frameskip] off (%s)", source);
    }
    if (live) {
        gdx_frameskip_live_switch(&sGov, &sSlot, mode, maxSkip);
        ResetWindow(0);
        ResetRenderGap();
    } else {
        gdx_frameskip_gov_init(&sGov, mode, maxSkip, GDX_FRAMESKIP_INTERVAL_TICKS);
        gdx_frameskip_slot_reset(&sSlot, maxSkip + 1);
    }
    if (sRtMode == 0 && (live || mode == GDX_FRAMESKIP_STEADY)) {
        gdx3ds_gpuprof_set_external_pacing(mode == GDX_FRAMESKIP_STEADY);
    }
    sDropTasks = 0;
    sEnabled = mode != GDX_FRAMESKIP_OFF;
    sMode = mode;
    FrameskipLog(line);
}

static int InitRace0(void);

void gdx3ds_frameskip_init(int rtMode) {
    char line[192];
    char source[48];
    const char* raw = gdx3ds_config_get_string("debug", "frameskip", "0");
    int mode = ParseMode(raw, &sFixedSkip);
    sRtMode = rtMode;
    if (strcmp(raw, "race0") == 0) {
        mode = rtMode == 0 ? InitRace0() : GDX_FRAMESKIP_AUTO; /* rtMode != 0: refused below */
    }
    if (mode != GDX_FRAMESKIP_OFF && rtMode != 0) {
        snprintf(line, sizeof(line),
                 "[frameskip] REFUSED (debug.frameskip=%s): render thread on (rtMode=%d); frame skip is an "
                 "rtMode 0 feature -- running every frame", raw, rtMode);
        FrameskipLog(line);
        mode = GDX_FRAMESKIP_OFF;
    }
    snprintf(source, sizeof(source), "debug.frameskip=%s", raw);
    ApplyMode(mode, source, 0);
}

/* [debug] race0_other (auto | steady, default auto), race0_scope_ticks (8), race0_heavy_ms (33,
 * 0 = cost guard off), race0_light_ms (25). Boot is never in a race: starts in the other mode. */
static int InitRace0(void) {
    char line[224];
    const char* other = gdx3ds_config_get_string("debug", "race0_other", "auto");
    const int scopeTicks = ClampInt(gdx3ds_config_get_int("debug", "race0_scope_ticks", 8), 1, 600);
    const int heavyMs = ClampInt(gdx3ds_config_get_int("debug", "race0_heavy_ms", 33), 0, 1000);
    const int lightMs = ClampInt(gdx3ds_config_get_int("debug", "race0_light_ms", 25), 0, heavyMs);
    sRace0Other = strcmp(other, "steady") == 0 ? GDX_FRAMESKIP_STEADY : GDX_FRAMESKIP_AUTO;
    gdx_race0_init(&sRace0Ctl, scopeTicks, (uint64_t)heavyMs * (uint64_t)CPU_TICKS_PER_MSEC,
                   (uint64_t)lightMs * (uint64_t)CPU_TICKS_PER_MSEC);
    gdx_race0_set_dwell(&sRace0Ctl, ClampInt(gdx3ds_config_get_int("debug", "race0_dwell", 0), 0, 36000));
    sRace0Arm = gdx3ds_config_get_int("debug", "race0_arm", 1) != 0;
    sRace0PostFinish = gdx3ds_config_get_int("debug", "race0_post_finish", 1) != 0;
    {
        const char* heavy = gdx3ds_config_get_string("debug", "race0_heavy_mode", other);
        sRace0Heavy = strcmp(heavy, "steady") == 0 ? GDX_FRAMESKIP_STEADY : GDX_FRAMESKIP_AUTO;
    }
    sRace0 = 1;
    snprintf(line, sizeof(line),
             "[frameskip] mode=race0 other=%s heavy_mode=%s scope_ticks=%d heavy_ms=%d light_ms=%d dwell=%d arm=%d "
             "post_finish=%d ring=%d: every tick renders in races (frameskip 0), %s elsewhere",
             ModeName(sRace0Other), ModeName(sRace0Heavy), scopeTicks, heavyMs, lightMs, sRace0Ctl.dwellTicks, sRace0Arm,
             sRace0PostFinish, GDX_RACE0_RING, ModeName(sRace0Other));
    FrameskipLog(line);
    return sRace0Other;
}

int gdx3ds_frameskip_available(void) {
    return sRtMode == 0;
}

int gdx3ds_frameskip_request_mode(int mode) {
    if (sRtMode != 0) {
        return 0;
    }
    if (mode != GDX_FRAMESKIP_OFF && mode != GDX_FRAMESKIP_AUTO && mode != GDX_FRAMESKIP_STEADY) {
        return 0;
    }
    if (sRace0) {
        sRace0 = 0;
        FrameskipLog("[frameskip] race0 off: DBG-tab mode request overrides it");
    }
    sPendingMode = mode;
    return 1;
}

int gdx3ds_frameskip_mode(void) {
    return sPendingMode >= 0 ? sPendingMode : sMode;
}

/* The safe point: top of an iteration, before its decision. The previous iteration has
 * ended (end_iteration disarmed the task drop) and, if it rendered, presented (the STEADY
 * hold ran in before_present), so nothing is half-done. */
static void ApplyPendingMode(void) {
    const int mode = sPendingMode;
    if (gdx3ds_c5tick_hollow_pending()) {
        /* C5TICK: the previous build was made hollow on the prediction that this iteration skips.
         * Keep the old mode one more iteration (ForceHollowSkip honours the prediction); with the
         * change pending, next_skips refuses, so the build running now is full. */
        sRace0Deferred++;
        return;
    }
    sPendingMode = -1;
    if (mode == sMode) {
        return;
    }
    ApplyMode(mode, sRace0 ? "race0: live" : "menu: live", 1);
}

void gdx3ds_frameskip_mode_label(char* buf, unsigned cap) {
    const int mode = gdx3ds_frameskip_mode();
    const char* prefix = sRace0 ? "r0 " : "";
    if (mode == GDX_FRAMESKIP_STEADY && mode == sMode) {
        snprintf(buf, cap, "%ssteady D%d", prefix, sSteady.d);
    } else if (mode == GDX_FRAMESKIP_FIXED) {
        snprintf(buf, cap, "fixed %d", sFixedSkip);
    } else {
        snprintf(buf, cap, "%s%s", prefix, ModeName(mode));
    }
}

unsigned long gdx3ds_frameskip_skipped_total(void) {
    return sSkippedTotal;
}

int gdx3ds_frameskip_enabled(void) {
    return sEnabled;
}

int gdx3ds_frameskip_render_gap(void) {
    if (!sEnabled) {
        return 0;
    }
    return sRenderGap;
}

static void NoteDecision(const GdxFrameskipDecision* d) {
    sWinFrames++;
    if (d->skip) {
        sWinSkipped++;
        sSkipsSinceRender++;
        sRun++;
        if (sRun > sWinMaxRun) {
            sWinMaxRun = sRun;
        }
    } else {
        sWinRendered++;
        sRun = 0;
        sRenderGap = sSkipsSinceRender;
        sSkipsSinceRender = 0;
    }
    if (d->vetoed) {
        sWinVetoed++;
    }
    if (d->lag > sWinMaxLag) {
        sWinMaxLag = d->lag;
    }
}

/* The vblank wait is citro3d's own C3D_FrameSync (wait until the frame counters its GSP event
 * handler advances on every VBlank0/1 move), NOT gspWaitForVBlank: a main-thread
 * gspWaitForEvent(VBlank0) beside citro3d's handler crashed the emulator's host process within
 * seconds in 5 of 5 lane runs (align=1 or hold=1), and 0 of 1 with both off. C3D_FrameSync is
 * the exact wait the lever-off loop makes every frame (gpuprof SYNCDRAW). */
static void SteadyVblankWait(void) {
    /* C5 STAGE 0: a pacing wait is slack, not work -- [mprof] pace / [c5cat] pace. */
    const int mpPrev = gdx3ds_mprof_enter(GDX3DS_MPROF_PACE);
    const uint64_t t0 = svcGetSystemTick();
    C3D_FrameSync();
    gdx3ds_hitch_add(GDX_HITCH_EV_PACE_TICKS, (uint32_t)(svcGetSystemTick() - t0)); /* HITCH: slack */
    gdx3ds_mprof_leave(mpPrev);
}

static void SleepTicks(uint64_t ticks) {
    /* C5 STAGE 0: the schedule sleep is slack, not work -- [mprof] pace / [c5cat] pace. */
    const int mpPrev = gdx3ds_mprof_enter(GDX3DS_MPROF_PACE);
    const uint64_t t0 = svcGetSystemTick();
    svcSleepThread((s64)(ticks * 1000000000ull / SYSCLOCK_ARM11));
    gdx3ds_hitch_add(GDX_HITCH_EV_PACE_TICKS, (uint32_t)(svcGetSystemTick() - t0)); /* HITCH: slack */
    gdx3ds_mprof_leave(mpPrev);
}

/* Wait for the real vblank nearest `target` (an estimate kept fresh by the returns of earlier
 * waits). Sleeps to half a period before it, then blocks on the GSP vblank event, so a phase
 * error under half a period cannot make it wait a whole extra refresh. Returns the tick after
 * the vblank, or 0 when the target was already reached (no wait). */
static uint64_t WaitVblankNear(uint64_t target, int enabled) {
    const uint64_t now = svcGetSystemTick();
    if (!enabled) {
        return 0;
    }
    if (now + GDX_STEADY_GUARD_TICKS >= target) {
        return 0;
    }
    if (target - now > GDX_STEADY_PERIOD_TICKS / 2u) {
        SleepTicks(target - now - GDX_STEADY_PERIOD_TICKS / 2u);
    }
    SteadyVblankWait();
    return svcGetSystemTick();
}

/* Slot start: the render iteration begins on the vblank D periods after the previous slot's. */
/* A late slot start lost `lost` vblanks of logic. Repay them as extra skip ticks in this slot,
 * but only as many as fit beside the slot's own D-1 in half a vblank after the present (the
 * hold leaves one): the repayment must not make the NEXT slot late too. What does not fit is
 * lost (a hitch then costs game time instead of cascading into more misses). */
static int CatchupTicks(int lost) {
    const uint64_t budget = GDX_STEADY_PERIOD_TICKS / 2u;
    const uint64_t skip = sSteady.skipAvg != 0 ? sSteady.skipAvg : (uint64_t)CPU_TICKS_PER_MSEC;
    const int room = (int)(budget / skip) - (sSteady.d - 1);
    int c = lost < room ? lost : room;
    if (c > 4) {
        c = 4;
    }
    return c > 0 ? c : 0;
}

/* NETTICK: this iteration's pad wait alone made the slot start `late` (see sLockstepNetMiss). */
static int SlotLateByNetWait(uint64_t late) {
    const uint64_t netWait = sNetExcuse > 0 ? sSlotNetWait : sIterNetWait; /* o3ds60: the whole slot's */
    return sLockstep && !sLockstepNetMiss && gdx_steady_late_by_net(late, netWait, GDX_STEADY_PERIOD_TICKS);
}

/* o3ds60 steady_net_catchup: catch-up skips are allowed (see sNetCatchup). */
static int SteadyCatchupAllowed(void) {
    return !(sLockstep && sNetCatchup == 0 && sItersSinceNetWait < GDX_STEADY_NET_AHEAD_TICKS);
}

static unsigned long TicksToMsX10(uint64_t ticks);

/* o3ds60 steady_net_lead: this slot start waited for the peers' pads; take one more vblank. */
static int SteadyNetLeadShift(void) {
    char line[160];
    if (!sLockstep || sNetLead <= 0 || sNetLeadShifts >= sNetLead || sSlot.beginMissed) {
        return 0;
    }
    if (sSlotNetWait < GDX_STEADY_GUARD_TICKS || sItersSinceLead < GDX_STEADY_NET_LEAD_GAP_TICKS) {
        return 0;
    }
    SteadyVblankWait();
    sNetLeadShifts++;
    sItersSinceLead = 0;
    snprintf(line, sizeof(line),
             "[frameskip] lockstep lead +1 vblank (%d of %d, debug.steady_net_lead): the slot start waited "
             "%lu.%lu ms for pads",
             sNetLeadShifts, sNetLead, TicksToMsX10(sSlotNetWait) / 10, TicksToMsX10(sSlotNetWait) % 10);
    FrameskipLog(line);
    return 1;
}

static void SteadyBeginSlot(void) {
    const uint64_t period = GDX_STEADY_PERIOD_TICKS;
    uint64_t target;
    uint64_t woke;
    uint64_t now;
    sSlot.beginMissed = 0;
    sSlot.catchup = 0;
    sSlotNetWait = sNetSinceSlot;
    sNetSinceSlot = 0;
    sSlotStartLate = 0;
    sSlotGsp0 = gdx3ds_hitch_ev[GDX_HITCH_EV_GSP_WAIT_TICKS];
    if (sSlot.vblank == 0) {
        if (sSteadyAlign) {
            SteadyVblankWait();
        }
        sSlot.vblank = svcGetSystemTick();
        sSlot.renderBegin = sSlot.vblank;
        sSlot.d = sSteady.d;
        return;
    }
    target = sSlot.vblank + (uint64_t)sSlot.d * period;
    woke = WaitVblankNear(target, sSteadyAlign);
    now = svcGetSystemTick();
    if (woke != 0) {
        sSlot.vblank = woke;
    } else if (now > target + period / 2u) {
        /* the slot's vblank is gone: start on the latest one that passed */
        sSlot.vblank = target + ((now - target) / period) * period;
        if (SlotLateByNetWait(now - target)) {
            sWinNetLate++; /* NETTICK: held by the peers; on time by our own work */
        } else {
            sSlot.beginMissed = 1;
            if (SteadyCatchupAllowed()) {
                sSlot.catchup = CatchupTicks((int)((now - target + period / 2u) / period));
            } else if (CatchupTicks((int)((now - target + period / 2u) / period)) > 0) {
                sD1.catchupCut++; /* o3ds60: ahead of the peers; the next pad wait repays it */
                sD1CatchupCutTotal++;
            }
        }
    } else {
        sSlot.vblank = target;
    }
    sSlotStartLate = now > sSlot.vblank ? now - sSlot.vblank : 0;
    if (SteadyNetLeadShift()) {
        now = svcGetSystemTick();
        sSlot.vblank = now;
        sSlotStartLate = 0;
    }
    sSlot.renderBegin = now;
    sSlot.d = sSteady.d;
    sWinCatchup += (unsigned long)sSlot.catchup;
}

static void Lock30SlotBegin(void);
static void ProbeSlotBegin(void);
static void Lock30RaceEnd(void);
static void ProbeRaceEnd(void);
static void ProbeTick(unsigned long frame);

static void SteadyNoteBegin(const GdxFrameskipDecision* d, uint64_t now) {
    if (sSlot.lastWasSkip && sSlot.lastBeginTick != 0) {
        const uint64_t skipCost = LessNetWait(now - sSlot.lastBeginTick, sIterNetWait); /* NETTICK */
        gdx_steady_note_skip(&sSteady, skipCost);
        if (skipCost > sSlotSkipMax) {
            sSlotSkipMax = skipCost;
        }
    }
    sSlot.lastWasSkip = d->skip;
    sSlot.lastBeginTick = now;
    sSlot.pacedRender = !d->skip && !d->vetoed;
    if (!d->skip) {
        sSlot.slotPaced = sSlot.pacedRender; /* C6: the slot this render opens, kept through its skips */
    }
    if (sSlot.pacedRender) {
        Lock30SlotBegin(); /* o3ds3d [lock30]: pin D >= 2 for a 3D lockstep slot */
        SteadyBeginSlot();
        ProbeSlotBegin(); /* o3ds3d [ceiling]: a probe slot renders at D = 1 */
        /* C6 [replay]: at D = 2 the previous slot's skipped tick is shown for this vblank while
         * this slot's tick runs (its GPU work overlaps the logic; FrameBegin waits for it). */
        gdx3ds_replay_slot_begin(sSlot.d, sSlot.beginMissed, sSlot.catchup);
    }
}

/* C5TICK: the prediction above held by construction; only an unpredictable capture request (a
 * net CAPTURE) can make the governor want to render the task of a hollow build. It must not:
 * skip anyway (the capture is lost and counted; the hollow list is never walked). */
static void ForceHollowSkip(GdxFrameskipDecision* d) {
    if (d->skip || !gdx3ds_c5tick_hollow_pending()) {
        return;
    }
    d->skip = 1;
    sGov.skipRun++;
    sHollowForced++;
}

static unsigned long TicksToMsX10(uint64_t ticks);

/* The previous iteration's work, when it rendered in race scope: begin-to-begin, or for a paced
 * steady render its slot work with the present hold left out (a hold is pacing, not cost). */
static uint64_t Race0Sample(uint64_t now) {
    if (sRace0PrevBegin == 0 || !sRace0PrevRendered || !sRace0PrevScope || sRace0PrevCapture) {
        return 0; /* a SHOT/[fh] capture's readback is harness work, not render cost */
    }
    if (sMode == GDX_FRAMESKIP_STEADY && sRace0SteadyRw != 0) {
        return sRace0SteadyRw;
    }
    return LessNetWait(now - sRace0PrevBegin, sIterNetWait); /* NETTICK: the peer's wait is not ours */
}

static void LogRace0Switch(unsigned long frame, int target, int why, int gameMode, int changeState) {
    char line[256];
    const int guard = why == GDX_RACE0_WHY_HEAVY || why == GDX_RACE0_WHY_LIGHT;
    const unsigned long pct = guard ? TicksToMsX10(sRace0Ctl.guardPct) : 0;
    snprintf(line, sizeof(line),
             "[frameskip] race0 -> %s at frame %lu reason=%s game=%d chg=%d from=%s after=%lu ticks %s=%lu.%lu ms "
             "switches=%lu deferred=%lu trips=%d dwell=%d",
             target == GDX_FRAMESKIP_OFF ? "0" : ModeName(target), frame, gdx_race0_why_name(why), gameMode,
             changeState, sMode == GDX_FRAMESKIP_OFF ? "0" : ModeName(sMode), frame - sRace0LastSwitchFrame,
             why == GDX_RACE0_WHY_LIGHT ? "p75" : "p25", pct / 10, pct % 10, sRace0Switches, sRace0Deferred,
             sRace0Ctl.trips, gdx_race0_dwell_now(&sRace0Ctl));
    FrameskipLog(line);
}

/* o3dsperf [debug] race0_lockstep_steady (default 1 on an Old 3DS, 0 on a New 3DS): in a
 * lockstep race the race scope runs race0's heavy mode (steady) from its first tick to the
 * finish, whatever the cost guard reads. hw 158a13e (Old 3DS guest, 16 machines): the guard
 * read the steady slot's ~24 ms render as "light" (race0_light_ms 25) and went back to
 * frameskip 0 every 600 ticks -- a render on EVERY tick at ~26 ms, ~38 ticks/s, which dragged the
 * New 3DS host to 30 fps (race 1 lt 0-599 and 1800-2999 were exactly those stretches) until the
 * guard tripped again. The race also started in 0 for ~470 ticks (the guard needs a full ring of
 * racing samples). Steady picks D itself (D=1 when every tick fits), so nothing is lost. 0 = the
 * cost guard decides, as before. */
static int sRace0LockstepSteady = -1;
static int sRace0Pinned = 0;

static int Race0LockstepPin(int postFinish) {
    if (sRace0LockstepSteady < 0) {
        sRace0LockstepSteady =
            gdx3ds_config_get_int("debug", "race0_lockstep_steady", LockstepDMaxDefault() != 0 ? 1 : 0) != 0;
    }
    return sRace0LockstepSteady && sLockstep && sRace0Ctl.scope && !postFinish &&
           sRace0Heavy == GDX_FRAMESKIP_STEADY;
}

/* The mode the cost guard's own state asks for (what race0 would be running unpinned). */
static int Race0ImpliedMode(void) {
    if (sRace0Ctl.racing) {
        return GDX_FRAMESKIP_OFF;
    } else if (sRace0Ctl.scope && sRace0Ctl.heavy) {
        return sRace0Heavy;
    } else {
        return sRace0Other;
    }
}

/* 1 when the lockstep pin owns this tick's decision (it may latch a switch). */
static int Race0PinTick(unsigned long frame, int postFinish) {
    char line[200];
    const int pinned = Race0LockstepPin(postFinish);
    int target;
    if (!pinned && !sRace0Pinned) {
        return 0;
    }
    target = pinned ? sRace0Heavy : Race0ImpliedMode();
    if (pinned != sRace0Pinned) {
        snprintf(line, sizeof(line), "[frameskip] race0 lockstep pin %s at frame %lu (debug.race0_lockstep_steady=%d)",
                 pinned ? "on: the race runs steady" : "off: the cost guard decides again", frame,
                 sRace0LockstepSteady);
        FrameskipLog(line);
        sRace0Pinned = pinned;
    }
    if (target != gdx3ds_frameskip_mode()) {
        sRace0Switches++;
        snprintf(line, sizeof(line), "[frameskip] race0 -> %s at frame %lu reason=%s from=%s after=%lu ticks switches=%lu",
                 target == GDX_FRAMESKIP_OFF ? "0" : ModeName(target), frame,
                 pinned ? "lockstep-steady" : "lockstep-unpin", sMode == GDX_FRAMESKIP_OFF ? "0" : ModeName(sMode),
                 frame - sRace0LastSwitchFrame, sRace0Switches);
        FrameskipLog(line);
        sRace0LastSwitchFrame = frame;
        sPendingMode = target;
    }
    return 1;
}

/* RACE0: read the race scope, feed the controller, and latch a mode change for the safe point
 * right after (ApplyPendingMode, which holds it one tick behind a hollow build). */
static void Race0Tick(unsigned long frame) {
    const uint64_t now = svcGetSystemTick();
    int gameMode = 0;
    int changeState = 0;
    int why = GDX_RACE0_WHY_NONE;
    const int scope = gdx3ds_frameskip_game_race_scope(&gameMode, &changeState);
    const uint64_t sample = (sRace0Arm && !gdx3ds_frameskip_game_racing()) ? 0 : Race0Sample(now);
    const int postFinish = sRace0PostFinish && scope && gdx3ds_frameskip_game_post_finish();
    const int result = gdx_race0_step_post(&sRace0Ctl, scope, postFinish, sample, &why);
    sRace0PrevBegin = now;
    sRace0PrevScope = scope;
    sRace0SteadyRw = 0;
    if (Race0PinTick(frame, postFinish)) {
        return;
    }
    if (result == GDX_RACE0_HOLD) {
        return;
    }
    {
        const int target = result == GDX_RACE0_TO_EVERY ? GDX_FRAMESKIP_OFF
                           : why == GDX_RACE0_WHY_HEAVY   ? sRace0Heavy
                                                          : sRace0Other;
        sRace0Switches++;
        LogRace0Switch(frame, target, why, gameMode, changeState);
        sRace0LastSwitchFrame = frame;
        sPendingMode = target;
    }
}

static int DecideIteration(unsigned long frame);

void gdx3ds_frameskip_set_lockstep(int on) {
    char line[256];
    on = on ? 1 : 0;
    if (sLockstepHold < 0) {
        sLockstepHold = gdx3ds_config_get_int("debug", "steady_lockstep_hold", 0) != 0;
    }
    if (sLockstepNetMiss < 0) {
        sLockstepNetMiss = gdx3ds_config_get_int("debug", "steady_lockstep_netmiss", 0) != 0;
    }
    if (sNetExcuse < 0) {
        const int isOld = LockstepDMaxDefault() != 0;
        sNetExcuse = gdx3ds_config_get_int("debug", "steady_net_excuse", isOld ? 1 : 0) != 0;
        sNetCatchup = gdx3ds_config_get_int("debug", "steady_net_catchup", isOld ? 0 : 1) != 0;
        sNetLead = ClampInt(gdx3ds_config_get_int("debug", "steady_net_lead", isOld ? 2 : 0), 0, 8);
    }
    if (on == sLockstep) {
        return;
    }
    sLockstep = on;
    gdx3ds_pipe_set_lockstep(on);
    LatchD1Rules();
    if (on) {
        sLockstepRaces++;
        sSteadyRuleOld = sSteadyAb > 0 && sLockstepRaces == 2u;
        if (sSteadyAb > 0) {
            char ab[160];
            snprintf(ab, sizeof(ab), "[steadyab] race %u rule=%s (steady_ab: race 1 new, race 2 old, then new)",
                     sLockstepRaces, sSteadyRuleOld ? "old" : "new");
            FrameskipLog(ab);
        }
    } else {
        sSteadyRuleOld = 0;
        D1DriftCloseStretch("race"); /* o3ds-mp16: the race's last [d1drift] stretch */
        sD1PrevDisplay = 0;
        Lock30RaceEnd(); /* o3ds3d: the race's [lock30] summary; the pin lets go */
        ProbeRaceEnd();
    }
    ApplyD1Rules();
    if (on) {
        CapSteadyForLockstep();
    } else {
        UncapSteadyAfterLockstep();
    }
    ApplyLockstepUpPct();
    sNetWaitPending = 0;
    sWinNetWait = 0; /* a race without steady windows (race0 "0") must not leak into the next one */
    sNetSinceSlot = 0;
    sItersSinceNetWait = 255u;
    sMissLines = 0; /* o3ds60: the first [steadymiss] lines of every race */
    sNetLeadShifts = 0;
    sItersSinceLead = 0;
    sD1CatchupCutTotal = 0;
    snprintf(line, sizeof(line),
             "[frameskip] lockstep %s: cost samples exclude the pad wait; steady present hold %s "
             "(debug.steady_lockstep_hold=%d); pad-wait late starts %s (debug.steady_lockstep_netmiss=%d); "
             "up_pct=%u net_excuse=%d net_catchup=%d net_lead=%d",
             on ? "on" : "off", (on && !sLockstepHold) ? "off" : "as configured", sLockstepHold,
             sLockstepNetMiss ? "are misses" : "are not misses", sLockstepNetMiss, sSteady.upPct, sNetExcuse,
             sNetCatchup, sNetLead);
    FrameskipLog(line);
}

void gdx3ds_frameskip_note_net_wait(uint64_t ticks) {
    if (sLockstep) {
        sNetWaitPending += ticks;
        sWinNetWait += ticks;
    }
}

/* o3ds60 [steadyd1]: the own time from the latest slot render's present to the next iteration
 * (EndFrame, the bottom dash, telemetry, the next poll), pad wait left out. Only right after a
 * rendered iteration: at D = 1 this plus the render is the whole tick. */
static void NotePostPresent(uint64_t netWait) {
    sSlotPostValid = 0;
    if (sLastPresentTick == 0) {
        return;
    }
    sSlotPost = LessNetWait(svcGetSystemTick() - sLastPresentTick, netWait);
    sSlotPostValid = 1;
    sSlotEndFrame = sPendingEndFrame;
    sPendingEndFrame = 0;
    sSlotDrain = sPendingDrain;
    sSlotKicks = sPendingKicks;
    sPendingDrain = 0;
    sPendingKicks = 0;
    sLastPresentTick = 0;
}

void gdx3ds_frameskip_after_present(void) {
    if (sLastPresentTick != 0) {
        sPendingEndFrame = svcGetSystemTick() - sLastPresentTick; /* o3ds60 [steadyd1] fe= */
        sPendingKicks = 0;
        sPendingDrain = &gdx3ds_overlap_last_drain_ticks != NULL ? gdx3ds_overlap_last_drain_ticks(&sPendingKicks) : 0;
    }
}

int gdx3ds_frameskip_begin_iteration(unsigned long frame) {
    int skip;
    const uint64_t netWait = sNetWaitPending;
    sNetWaitPending = 0;
    sIterNetWait = netWait;
    sNetSinceSlot += netWait; /* o3ds60 steady_net_excuse: the slot-wide pad wait */
    if (netWait != 0) {
        sItersSinceNetWait = 0;
    } else if (sItersSinceNetWait < 255u) {
        sItersSinceNetWait++;
    }
    if (sItersSinceLead < 0xFFFFu) {
        sItersSinceLead++;
    }
    NotePostPresent(netWait);
    ProbeTick(frame); /* o3ds3d [ceiling] */
    if (gdx3ds_pw_enabled()) {
        int gameMode = 0;
        int changeState = 0;
        gdx3ds_pw_set_race(gdx3ds_frameskip_game_race_scope(&gameMode, &changeState), sLockstep);
    }
    if (sRace0) {
        Race0Tick(frame);
        sRace0PrevCapture = gdx3ds_oracle_wants_next_frame();
    }
    if (sPendingMode >= 0) {
        ApplyPendingMode();
    }
    skip = sEnabled ? DecideIteration(frame) : 0;
    sRace0PrevRendered = !skip;
    return skip;
}

static int DecideIteration(unsigned long frame) {
    GdxFrameskipDecision d;
    uint64_t now;
    now = svcGetSystemTick();
    if (sWinStartTick == 0) {
        sWinStartTick = now;
    }
    if (sMode == GDX_FRAMESKIP_STEADY) {
        sGov.maxSkip = sSlot.d - 1 + sSlot.catchup;
    }
    if (sLockstep && sGov.armed && sIterNetWait != 0) {
        sGov.due += sIterNetWait; /* NETTICK: held by the peers, not late: no catch-up skips for it */
    }
    d = gdx_frameskip_gov_decide(&sGov, now, frame, gdx3ds_oracle_wants_next_frame());
    if (d.skip && gdx3ds_replay_verify_take()) {
        d.skip = 0; /* C6 [debug] replay_verify: replay this tick AND render it, then compare */
    }
    if (d.sleepTicks != 0) {
        SleepTicks(d.sleepTicks);
    }
    if (sMode == GDX_FRAMESKIP_STEADY) {
        SteadyNoteBegin(&d, now);
    }
    ForceHollowSkip(&d);
    NoteDecision(&d);
    if (d.skip) {
        sSkippedTotal++;
    }
    sDropTasks = d.skip;
    return d.skip;
}

/* C5TICK ([debug] dl_hollow, port/3ds/gdx3ds_c5tick.h): the display list built in iteration N
 * is walked in iteration N+1 (the game's framebuffer spin yields between the build and
 * Gfx_SetTask). A hollow build is only allowed when N+1 is certain to skip; this is that
 * prediction, taken at N's build, from the same state the governor will decide from:
 *   FIXED  (N+1)'s frame number is off-phase;
 *   STEADY the slot still has skip ticks left (skipRun < D-1+catchup, both fixed until the
 *          next render);
 *   AUTO   the clock is already a whole tick behind N+1's due time (lag only grows) and the
 *          cap leaves room;
 * and in every mode the oracle will not veto N+1 (fh_every, a pending or next-line SHOT) and no
 * mode change is pending. */
int gdx3ds_frameskip_replay_plan(int* skipRun) {
    *skipRun = sGov.skipRun;
    if (!sEnabled || sMode != GDX_FRAMESKIP_STEADY || sPendingMode >= 0 || !sSlot.slotPaced) {
        return 0;
    }
    return sSlot.d - 1 + sSlot.catchup;
}

int gdx3ds_frameskip_next_skips(unsigned long nextFrame) {
    uint64_t now;
    if (!sEnabled || sPendingMode >= 0 || !sGov.armed) {
        return 0;
    }
    if (gdx3ds_oracle_wants_frame_after_next()) {
        return 0;
    }
    if (sMode == GDX_FRAMESKIP_FIXED) {
        return (nextFrame % (unsigned long)(sGov.maxSkip + 1)) != 0;
    } else if (sMode == GDX_FRAMESKIP_STEADY) {
        return sGov.skipRun < sSlot.d - 1 + sSlot.catchup;
    } else if (sMode == GDX_FRAMESKIP_AUTO) {
        if (sGov.skipRun >= sGov.maxSkip) {
            return 0;
        }
        now = svcGetSystemTick();
        return now > sGov.due && (now - sGov.due) >= sGov.interval;
    }
    return 0;
}

static unsigned long TicksToMsX10(uint64_t ticks) {
    return (unsigned long)((ticks * 10u) / (uint64_t)CPU_TICKS_PER_MSEC);
}

/* missBits/hardBits: the controller's history as it decided (ChangeD clears them after). */
static void LogDChange(int from, int result, uint64_t rw, uint32_t missBits, uint32_t hardBits) {
    char line[224];
    snprintf(line, sizeof(line),
             "[frameskip] steady D %d->%d (%s) rw=%lu.%lu rw90=%lu.%lu cln90=%lu.%lu skip=%lu.%lu ms "
             "missBits=0x%02lx hardBits=0x%02lx",
             from, sSteady.d, result == GDX_STEADY_DOWN ? "down: missed slots" : "up: headroom",
             TicksToMsX10(rw) / 10, TicksToMsX10(rw) % 10,
             TicksToMsX10(gdx_steady_rw_pct(&sSteady, 90)) / 10, TicksToMsX10(gdx_steady_rw_pct(&sSteady, 90)) % 10,
             TicksToMsX10(gdx_steady_rw_pct_clean(&sSteady, 90)) / 10,
             TicksToMsX10(gdx_steady_rw_pct_clean(&sSteady, 90)) % 10,
             TicksToMsX10(sSteady.skipAvg) / 10, TicksToMsX10(sSteady.skipAvg) % 10,
             (unsigned long)(missBits & 0xFFu), (unsigned long)(hardBits & 0xFFu));
    FrameskipLog(line);
}

/* o3ds60 steady_net_excuse: this slot started late by no more than the pad wait of the slot
 * before it, and its present fits when measured from its own start: the peers' lateness, not
 * ours. Re-anchors the slot on the vblank the present can still make (the next slot's start
 * waits for the vblank after it instead of starting late against the missed one). */
static int NetExcusedPresent(uint64_t display) {
    const uint64_t period = GDX_STEADY_PERIOD_TICKS;
    const uint64_t now = svcGetSystemTick();
    uint64_t over;
    if (sNetExcuse <= 0 || !sLockstep || sLockstepNetMiss || sSlotStartLate == 0) {
        return 0;
    }
    if (sSlotStartLate > sSlotNetWait + GDX_STEADY_GUARD_TICKS) {
        return 0;
    }
    if (now + GDX_STEADY_PRESENT_MARGIN_TICKS > display + sSlotStartLate) {
        return 0; /* it would have missed starting on time too: our own work */
    }
    over = now + GDX_STEADY_PRESENT_MARGIN_TICKS - display;
    sSlot.vblank += ((over + period - 1u) / period) * period;
    return 1;
}

static unsigned long TicksToUs(uint64_t ticks) {
    return (unsigned long)((ticks * 1000u) / (uint64_t)CPU_TICKS_PER_MSEC);
}

static void MsField(char* buf, size_t cap, uint64_t ticks) {
    const unsigned long x10 = TicksToMsX10(ticks);
    snprintf(buf, cap, "%lu.%lu", x10 / 10, x10 % 10);
}

static int sD1Pending = 0;

static const char* SlotMissCause(int missed, int excused) {
    if (excused) {
        return "net";
    } else if (!missed) {
        return "ok";
    } else if (sSlot.beginMissed) {
        return "begin";
    } else if (sSlotStartLate > sSlotNetWait + GDX_STEADY_GUARD_TICKS && sSlotStartLate > GDX_STEADY_GUARD_TICKS) {
        return "late";
    } else if (sSlotStartLate > GDX_STEADY_GUARD_TICKS) {
        return "net";
    } else {
        return "work";
    }
}

static void AddStat(uint64_t* sum, uint64_t* mx, uint64_t v) {
    *sum += v;
    if (v > *mx) {
        *mx = v;
    }
}

/* One D = 1 slot into the window; the first few misses of a race as their own line. */
static void NoteSlotD1(uint64_t rw, int missed, int excused, uint64_t display) {
    const uint64_t gsp = (uint64_t)(gdx3ds_hitch_ev[GDX_HITCH_EV_GSP_WAIT_TICKS] - sSlotGsp0);
    const uint64_t dash = gdx3ds_bottomhud_take_tick_ticks != NULL ? gdx3ds_bottomhud_take_tick_ticks() : 0;
    const char* cause = SlotMissCause(missed, excused);
    sD1Pending = 0;
    if (sSlot.d != 1) {
        return;
    }
    sD1.slots++;
    sD1.missed += missed ? 1u : 0u;
    sD1.excused += excused ? 1u : 0u;
    if (cause[0] == 'b') {
        sD1.begin++;
    } else if (cause[0] == 'n') {
        sD1.net++;
    } else if (cause[0] == 'l') {
        sD1.late++;
    } else if (cause[0] == 'w') {
        sD1.work++;
    }
    AddStat(&sD1.lateSum, &sD1.lateMax, sSlotStartLate);
    AddStat(&sD1.netSum, &sD1.netMax, sSlotNetWait);
    AddStat(&sD1.rwSum, &sD1.rwMax, rw);
    AddStat(&sD1.gspSum, &sD1.gspMax, gsp);
    AddStat(&sD1.dashSum, &sD1.dashMax, dash);
    if (sSlotPostValid) {
        AddStat(&sD1.postSum, &sD1.postMax, sSlotPost);
        AddStat(&sD1.feSum, &sD1.feMax, sSlotEndFrame);
        AddStat(&sD1.drainSum, &sD1.drainMax, sSlotDrain);
        sD1.kickFrames += sSlotKicks > 0 ? 1u : 0u;
        sD1.postN++;
    }
    if ((missed || excused) && sMissLines < GDX_STEADY_MISS_LINES_PER_RACE) {
        char line[320];
        char late[16], net[16], rws[16], post[16], gs[16], lead[16], fe[16];
        const uint64_t now = svcGetSystemTick();
        sMissLines++;
        MsField(late, sizeof(late), sSlotStartLate);
        MsField(net, sizeof(net), sSlotNetWait);
        MsField(rws, sizeof(rws), rw);
        MsField(post, sizeof(post), sSlotPostValid ? sSlotPost : 0);
        MsField(gs, sizeof(gs), gsp);
        MsField(fe, sizeof(fe), sSlotPostValid ? sSlotEndFrame : 0);
        MsField(lead, sizeof(lead), now + GDX_STEADY_PRESENT_MARGIN_TICKS > display
                                        ? now + GDX_STEADY_PRESENT_MARGIN_TICKS - display : 0);
        snprintf(line, sizeof(line),
                 "[steadymiss] #%d cause=%s late=%s net=%s rw=%s post=%s fe=%s gsp=%s dash_us=%lu over=%s ms "
                 "ahead=%u catchupcut=%lu",
                 sMissLines, cause, late, net, rws, post, fe, gs, TicksToUs(dash), lead, sItersSinceNetWait,
                 sD1CatchupCutTotal);
        FrameskipLog(line);
    }
    sD1Pending = 1;
}

static void NoteSlotD1Hard(int hard) {
    sD1Pending = 0;
    sD1.hard += hard ? 1u : 0u;
}

static uint64_t AvgTicks(uint64_t sum, unsigned long n) {
    return n != 0 ? sum / (uint64_t)n : 0;
}

/* [steadyd1] (only for a receipt window with D = 1 slots). */
static void EmitSteadyD1Window(unsigned long frame) {
    char line[640];
    char f[12][16];
    char fe[4][16];
    const unsigned long n = sD1.slots;
    if (n == 0 && sD1.catchupCut == 0) {
        return;
    }
    MsField(f[0], sizeof(f[0]), AvgTicks(sD1.lateSum, n));
    MsField(f[1], sizeof(f[1]), sD1.lateMax);
    MsField(f[2], sizeof(f[2]), AvgTicks(sD1.netSum, n));
    MsField(f[3], sizeof(f[3]), sD1.netMax);
    MsField(f[4], sizeof(f[4]), AvgTicks(sD1.rwSum, n));
    MsField(f[5], sizeof(f[5]), sD1.rwMax);
    MsField(f[6], sizeof(f[6]), AvgTicks(sD1.postSum, sD1.postN));
    MsField(f[7], sizeof(f[7]), sD1.postMax);
    MsField(f[8], sizeof(f[8]), AvgTicks(sD1.gspSum, n));
    MsField(f[9], sizeof(f[9]), sD1.gspMax);
    MsField(fe[0], sizeof(fe[0]), AvgTicks(sD1.feSum, sD1.postN));
    MsField(fe[1], sizeof(fe[1]), sD1.feMax);
    MsField(fe[2], sizeof(fe[2]), AvgTicks(sD1.drainSum, sD1.postN));
    MsField(fe[3], sizeof(fe[3]), sD1.drainMax);
    snprintf(line, sizeof(line),
             "[steadyd1] frame=%lu slots=%lu missed=%lu hard=%lu excused=%lu | cause begin=%lu net=%lu late=%lu "
             "work=%lu | late=%s/%s net=%s/%s rw=%s/%s post=%s/%s fe=%s/%s gsp=%s/%s ms (avg/max) dash_us=%lu/%lu "
             "catchupcut=%lu excuse=%d catchup=%d holds=%lu hold_pct=%u hit=%lu%% latepres=%lu d1_rate=%d pipe=%d "
             "| drain=%s/%s kickf=%lu/%lu hold_slots=%u retries=%lu rule=%s",
             frame, n, sD1.missed, sD1.hard, sD1.excused, sD1.begin, sD1.net, sD1.late, sD1.work, f[0], f[1], f[2],
             f[3], f[4], f[5], f[6], f[7], fe[0], fe[1], f[8], f[9], TicksToUs(AvgTicks(sD1.dashSum, n)), TicksToUs(sD1.dashMax),
             sD1.catchupCut, sNetExcuse, sNetCatchup, sSteady.d1Holds - sD1HoldsSeen, sSteady.d1HoldPct,
             n != 0 ? ((n - sD1.missed) * 100ul) / n : 0ul, sD1.latePresent, sD1Rate, gdx3ds_pipe_receipt_state(),
             fe[2], fe[3], sD1.kickFrames, sD1.postN, sSteady.d1HoldSlots, sSteady.d1Retries - sD1RetriesSeen,
             sSteadyRuleOld ? "old" : "new");
    sD1HoldsSeen = sSteady.d1Holds;
    sD1RetriesSeen = sSteady.d1Retries;
    FrameskipLog(line);
    memset(&sD1, 0, sizeof(sD1));
}

/* ---- o3ds3d: [lock30] and [ceiling] ---------------------------------------------------------
 *
 * [debug] stereo_mp_lock30 (default 1 on an Old 3DS, 0 on a New 3DS): while a lockstep race renders
 * in 3D (the right eye on, gdx3ds_stereo_active), steady D is pinned at 2 or above (dMin = 2: no
 * D = 1 retry). hw b52ac08 (Old 3DS guest, 3D): a D = 1 slot costs ~16.3 ms of the 16.7 ms period
 * (render 13 + post 1.5 + the GPU tail's queue wait 1-3), so D = 1 held for a few slots and fell back
 * (37 D flips in 92 s), and at D = 2 with the lockstep hold off a frame showed for 1 or 3 vblanks
 * ([pace] hold=20/213/23). 1 = the pin AND the present hold (every frame presents at vblank S+1 and
 * shows 2 vblanks; the hold is short at D = 2: render ~10 ms of the 16.7 before S+1); 2 = the pin
 * alone (the hold as configured for lockstep, off by default). 0 = as before. A D = 3 step down on
 * missed slots is still allowed (dchg= in the receipt). The 2D races and single player never pin.
 *   [lock30] on|off ...                       at every edge
 *   [lock30] frame= slots= missed= begin= latepres= dchg= catchup= | rw= hold= ms (avg/max) |
 *            pp=<1/1/2/3/4+                   per steady window with locked slots (pp = vblanks
 *                                             from one present to the next, rounded)
 *   [lock30] race=N end locked= unlocked= edges= ...   at the race's end
 *
 * [debug] ceiling_probe = N (default 0 = off): lockstep race N of the launch runs probe blocks of
 * D = 1 (the slot's D only: the controller does not see these slots and keeps its pinned D). Each
 * block is ceiling_probe_ticks (240) racing ticks of plain D = 1 ("normal") followed by as many of
 * D = 1 with the previous frame's GPU work waited out FIRST ("idle": C3Dgdx_FrameQueueIdle at the
 * slot start, before any of the frame's CPU work), from racing tick ceiling_probe_start (1200),
 * every ceiling_probe_every (2400) ticks, ceiling_probe_blocks (2) times. Skipping the GPU wait
 * outright is not safe: citro3d has ONE command buffer (C3D_FrameBegin resets it to offset 0) and
 * the VBO pool restarts at 0 every frame, so the new frame would overwrite what the GPU is still
 * reading. The idle half instead measures the frame with no GPU work beside it: its rwcpu
 * (render minus the idle and queue waits) + post is the best-case CPU-only D = 1 3D frame. Only the
 * render cadence changes (skip ticks run the same logic): the lockstep inputs and the game state
 * are untouched. A block whose slots render 2D is skipped (no3d=).
 *   [ceiling] race= block= half=normal|idle slots= no3d= begin= latepres= idlefail= | rw= rwcpu= idle=
 *             gsp= post= frame= ms (avg/p90/max) | fit=N/M (frame <= one period) */
static int sLock30DMinSaved = 1;
static uint64_t sPresentPrev = 0; /* the latest locked present (0 = the previous slot was not locked) */
typedef struct Lock30Win {
    unsigned long slots, missed, begin, latePres, dchg, catchup;
    unsigned long pp[5]; /* present-to-present: <1, 1, 2, 3, 4+ vblanks */
    uint64_t rwSum, rwMax, holdSum, holdMax;
} Lock30Win;
static Lock30Win sL30Win;
static Lock30Win sL30Race;
static unsigned long sL30Unlocked = 0; /* lockstep steady slots of the race with the lock off */
static unsigned long sL30Edges = 0;

static int Lock30Lever(void) {
    if (sLock30Lever < 0) {
        sLock30Lever = ClampInt(gdx3ds_config_get_int("debug", "stereo_mp_lock30", IsOld3dsModel() ? 1 : 0), 0, 2);
    }
    return sLock30Lever;
}

static int StereoOn(void) {
    return &gdx3ds_stereo_active != NULL && gdx3ds_stereo_active();
}

static int Lock30Wanted(void) {
    return Lock30Lever() > 0 && sLockstep && sMode == GDX_FRAMESKIP_STEADY && StereoOn();
}

static void Lock30Release(const char* why) {
    char line[160];
    if (!sLock30) {
        return;
    }
    sSteady.dMin = sLock30DMinSaved;
    sLock30 = 0;
    sPresentPrev = 0;
    sL30Edges++;
    snprintf(line, sizeof(line), "[lock30] off (%s): D=%d dMin=%d", why, sSteady.d, sSteady.dMin);
    FrameskipLog(line);
}

/* Before a paced slot's start: pin or release with the slot's 3D state. */
static void Lock30SlotBegin(void) {
    char line[200];
    const int want = Lock30Wanted();
    if (want == sLock30) {
        return;
    }
    if (!want) {
        Lock30Release(StereoOn() ? "not a lockstep steady race" : "2D");
        return;
    }
    sLock30DMinSaved = sSteady.dMin;
    gdx_steady_pin_min(&sSteady, 2);
    sLock30 = 1;
    sPresentPrev = 0;
    sL30Edges++;
    snprintf(line, sizeof(line),
             "[lock30] on (3D lockstep race %u): D=%d dMin=%d (was %d) hold=%s (debug.stereo_mp_lock30=%d)",
             sLockstepRaces, sSteady.d, sSteady.dMin, sLock30DMinSaved, sLock30Lever == 1 ? "on" : "as configured",
             sLock30Lever);
    FrameskipLog(line);
}

static void Lock30Add(Lock30Win* w, uint64_t rw, uint64_t hold, int missed, int latePres, int dchg,
                      int ppBucket) {
    w->slots++;
    w->missed += missed ? 1u : 0u;
    w->begin += sSlot.beginMissed ? 1u : 0u;
    w->latePres += latePres ? 1u : 0u;
    w->dchg += dchg ? 1u : 0u;
    w->catchup += (unsigned long)sSlot.catchup;
    AddStat(&w->rwSum, &w->rwMax, rw);
    AddStat(&w->holdSum, &w->holdMax, hold);
    if (ppBucket >= 0) {
        w->pp[ppBucket]++;
    }
}

/* One scored slot (not a probe slot): into the window and the race while locked. */
static void Lock30NoteSlot(uint64_t rw, uint64_t hold, int missed, int latePres, int dchg) {
    const uint64_t now = svcGetSystemTick();
    int bucket = -1;
    if (!sLock30) {
        if (sLockstep && Lock30Lever() > 0) {
            sL30Unlocked++;
        }
        sPresentPrev = 0;
        return;
    }
    if (sPresentPrev != 0) {
        const uint64_t vbl = (now - sPresentPrev + GDX_STEADY_PERIOD_TICKS / 2u) / GDX_STEADY_PERIOD_TICKS;
        bucket = vbl >= 4u ? 4 : (int)vbl;
    }
    sPresentPrev = now;
    Lock30Add(&sL30Win, rw, hold, missed, latePres, dchg, bucket);
    Lock30Add(&sL30Race, rw, hold, missed, latePres, dchg, bucket);
}

static void Lock30Format(char* line, size_t cap, const Lock30Win* w) {
    char f[4][16];
    MsField(f[0], sizeof(f[0]), AvgTicks(w->rwSum, w->slots));
    MsField(f[1], sizeof(f[1]), w->rwMax);
    MsField(f[2], sizeof(f[2]), AvgTicks(w->holdSum, w->slots));
    MsField(f[3], sizeof(f[3]), w->holdMax);
    snprintf(line, cap,
             "slots=%lu missed=%lu begin=%lu latepres=%lu dchg=%lu catchup=%lu | rw=%s/%s hold=%s/%s ms (avg/max) | "
             "pp=%lu/%lu/%lu/%lu/%lu (<1/1/2/3/4+ vblanks)",
             w->slots, w->missed, w->begin, w->latePres, w->dchg, w->catchup, f[0], f[1], f[2], f[3], w->pp[0],
             w->pp[1], w->pp[2], w->pp[3], w->pp[4]);
}

static void EmitLock30Window(unsigned long frame) {
    char body[288];
    char line[352];
    if (sL30Win.slots == 0) {
        return;
    }
    Lock30Format(body, sizeof(body), &sL30Win);
    snprintf(line, sizeof(line), "[lock30] frame=%lu D=%d %s", frame, sSteady.d, body);
    FrameskipLog(line);
    memset(&sL30Win, 0, sizeof(sL30Win));
}

/* set_lockstep(0): the race's summary, then the lock lets go. */
static void Lock30RaceEnd(void) {
    char body[288];
    char line[400];
    if (Lock30Lever() == 0) {
        return;
    }
    Lock30Release("race end");
    if (sL30Race.slots != 0 || sL30Unlocked != 0) {
        Lock30Format(body, sizeof(body), &sL30Race);
        snprintf(line, sizeof(line), "[lock30] race=%u end locked=%lu unlocked=%lu edges=%lu %s", sLockstepRaces,
                 sL30Race.slots, sL30Unlocked, sL30Edges, body);
        FrameskipLog(line);
    }
    memset(&sL30Win, 0, sizeof(sL30Win));
    memset(&sL30Race, 0, sizeof(sL30Race));
    sL30Unlocked = 0;
    sL30Edges = 0;
}

#define GDX_PROBE_CAP 256u
#define GDX_PROBE_IDLE_TIMEOUT_NS 50000000LL
static int sProbeRace = -1;
static unsigned long sProbeStart = 1200;
static unsigned long sProbeEvery = 2400;
static unsigned long sProbeTicks = 240;
static int sProbeBlocks = 2;
static unsigned long sProbeRacing = 0; /* racing ticks of the probe race */
static int sProbePhase = 0;            /* 0 none, 1 normal, 2 idle */
static int sProbeBlock = -1;
static int sProbeSlot = 0;             /* the open slot is a probe slot (its phase) */
static uint64_t sProbeIdle = 0;
static int sProbeIdleOk = 1;
static int sProbePostPending = 0;      /* 1 + the sample index waiting for its post */
static unsigned long sProbeFrame = 0;
typedef struct ProbeHalf {
    unsigned n, no3d, begin, latePres, idleFail, postN, fit;
    uint32_t rw[GDX_PROBE_CAP], rwcpu[GDX_PROBE_CAP], idle[GDX_PROBE_CAP], gsp[GDX_PROBE_CAP];
    uint32_t post[GDX_PROBE_CAP], frame[GDX_PROBE_CAP];
} ProbeHalf;
static ProbeHalf sProbeH;

static void LatchProbe(void) {
    if (sProbeRace >= 0) {
        return;
    }
    sProbeRace = ClampInt(gdx3ds_config_get_int("debug", "ceiling_probe", 0), 0, 99);
    sProbeStart = (unsigned long)ClampInt(gdx3ds_config_get_int("debug", "ceiling_probe_start", 1200), 0, 100000);
    sProbeTicks = (unsigned long)ClampInt(gdx3ds_config_get_int("debug", "ceiling_probe_ticks", 240), 16,
                                          (int)GDX_PROBE_CAP);
    sProbeEvery = (unsigned long)ClampInt(gdx3ds_config_get_int("debug", "ceiling_probe_every", 2400),
                                          (int)(2u * sProbeTicks), 100000);
    sProbeBlocks = ClampInt(gdx3ds_config_get_int("debug", "ceiling_probe_blocks", 2), 1, 16);
    if (sProbeRace > 0) {
        char line[200];
        snprintf(line, sizeof(line),
                 "[ceiling] armed: lockstep race %d, %d block(s) of %lu normal + %lu idle D=1 ticks from racing tick "
                 "%lu every %lu (3D only; skipping the GPU wait is unsafe: idle-first instead)",
                 sProbeRace, sProbeBlocks, sProbeTicks, sProbeTicks, sProbeStart, sProbeEvery);
        FrameskipLog(line);
    }
}

static int CmpU32(const void* a, const void* b) {
    const uint32_t x = *(const uint32_t*)a;
    const uint32_t y = *(const uint32_t*)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

static void UsField(char* buf, size_t cap, uint64_t us) {
    snprintf(buf, cap, "%lu.%lu", (unsigned long)(us / 1000u), (unsigned long)((us % 1000u) / 100u));
}

/* "avg/p90/max" in ms of n microsecond samples (sorted in place). */
static void ProbeStat(char* buf, size_t cap, uint32_t* v, unsigned n) {
    char a[16], p[16], m[16];
    uint64_t sum = 0;
    unsigned i;
    if (n == 0) {
        snprintf(buf, cap, "-");
        return;
    }
    for (i = 0; i < n; i++) {
        sum += v[i];
    }
    qsort(v, n, sizeof(v[0]), CmpU32);
    UsField(a, sizeof(a), sum / n);
    UsField(p, sizeof(p), v[(n * 9u) / 10u]);
    UsField(m, sizeof(m), v[n - 1u]);
    snprintf(buf, cap, "%s/%s/%s", a, p, m);
}

static void EmitProbeHalf(void) {
    char line[512];
    char f[6][40];
    ProbeHalf* h = &sProbeH;
    const unsigned n = h->n < GDX_PROBE_CAP ? h->n : GDX_PROBE_CAP;
    const unsigned pn = h->postN < GDX_PROBE_CAP ? h->postN : GDX_PROBE_CAP;
    ProbeStat(f[0], sizeof(f[0]), h->rw, n);
    ProbeStat(f[1], sizeof(f[1]), h->rwcpu, n);
    ProbeStat(f[2], sizeof(f[2]), h->idle, n);
    ProbeStat(f[3], sizeof(f[3]), h->gsp, n);
    ProbeStat(f[4], sizeof(f[4]), h->post, pn);
    ProbeStat(f[5], sizeof(f[5]), h->frame, pn);
    snprintf(line, sizeof(line),
             "[ceiling] race=%d block=%d half=%s frame=%lu slots=%u no3d=%u begin=%u latepres=%u idlefail=%u | "
             "rw=%s rwcpu=%s idle=%s gsp=%s post=%s frame=%s ms (avg/p90/max) | fit=%u/%u",
             sProbeRace, sProbeBlock + 1, sProbePhase == 2 ? "idle" : "normal", sProbeFrame, h->n, h->no3d, h->begin,
             h->latePres, h->idleFail, f[0], f[1], f[2], f[3], f[4], f[5], h->fit, pn);
    FrameskipLog(line);
}

/* begin_iteration, after NotePostPresent: the previous probe slot's post, and the tick's phase. */
static void ProbeTick(unsigned long frame) {
    int phase = 0;
    int block = -1;
    LatchProbe();
    if (sProbeRace <= 0) {
        return;
    }
    sProbeFrame = frame;
    if (sProbePostPending != 0) {
        const unsigned i = (unsigned)(sProbePostPending - 1);
        ProbeHalf* h = &sProbeH;
        if (sSlotPostValid && h->postN < GDX_PROBE_CAP) {
            const uint32_t post = (uint32_t)TicksToUs(sSlotPost);
            const uint32_t whole = h->rwcpu[i] + post;
            h->post[h->postN] = post;
            h->frame[h->postN] = whole;
            h->fit += whole <= (uint32_t)TicksToUs(GDX_STEADY_PERIOD_TICKS) ? 1u : 0u;
            h->postN++;
        }
        sProbePostPending = 0;
    }
    if (sLockstep && (int)sLockstepRaces == sProbeRace && gdx3ds_frameskip_game_racing()) {
        sProbeRacing++;
        if (sProbeRacing >= sProbeStart) {
            const unsigned long t = sProbeRacing - sProbeStart;
            const unsigned long b = t / sProbeEvery;
            const unsigned long in = t % sProbeEvery;
            if (b < (unsigned long)sProbeBlocks && in < 2u * sProbeTicks) {
                block = (int)b;
                phase = in < sProbeTicks ? 1 : 2;
            }
        }
    }
    if (phase == sProbePhase && block == sProbeBlock) {
        return;
    }
    if (sProbePhase != 0) {
        EmitProbeHalf();
    }
    sProbePhase = phase;
    sProbeBlock = block;
    memset(&sProbeH, 0, sizeof(sProbeH));
}

/* After SteadyBeginSlot: a probe slot renders at D = 1; the idle half waits the GPU out first. */
static void ProbeSlotBegin(void) {
    sProbeSlot = 0;
    if (sProbePhase == 0 || sMode != GDX_FRAMESKIP_STEADY) {
        return;
    }
    if (!StereoOn()) {
        sProbeH.no3d++;
        return;
    }
    sProbeSlot = sProbePhase;
    sSlot.d = 1;
    sProbeIdle = 0;
    sProbeIdleOk = 1;
    if (sProbeSlot == 2) {
        const uint64_t t0 = svcGetSystemTick();
        sProbeIdleOk = C3Dgdx_FrameQueueIdle(GDX_PROBE_IDLE_TIMEOUT_NS) ? 1 : 0;
        sProbeIdle = svcGetSystemTick() - t0;
    }
}

/* before_present of a probe slot: its sample (the steady controller never sees it). */
static void ProbeNoteSlot(uint64_t rw, int latePres) {
    ProbeHalf* h = &sProbeH;
    const uint64_t gsp = (uint64_t)(gdx3ds_hitch_ev[GDX_HITCH_EV_GSP_WAIT_TICKS] - sSlotGsp0);
    const uint64_t waits = sProbeIdle + gsp;
    const unsigned i = h->n;
    h->n++;
    h->begin += sSlot.beginMissed ? 1u : 0u;
    h->latePres += latePres ? 1u : 0u;
    h->idleFail += sProbeIdleOk ? 0u : 1u;
    sProbeSlot = 0;
    if (i >= GDX_PROBE_CAP) {
        return;
    }
    h->rw[i] = (uint32_t)TicksToUs(rw);
    h->rwcpu[i] = (uint32_t)TicksToUs(rw > waits ? rw - waits : 0);
    h->idle[i] = (uint32_t)TicksToUs(sProbeIdle);
    h->gsp[i] = (uint32_t)TicksToUs(gsp);
    sProbePostPending = (int)i + 1;
}

/* set_lockstep(0): a half cut short by the race's end is still reported. */
static void ProbeRaceEnd(void) {
    if (sProbePhase != 0) {
        EmitProbeHalf();
    }
    sProbePhase = 0;
    sProbeBlock = -1;
    sProbeSlot = 0;
    sProbePostPending = 0;
    sProbeRacing = 0;
}

/* o3ds-mp16 steady_d1_drift: the open A/B stretch as one receipt line. */
static void EmitD1Drift(const char* why) {
    char line[256];
    const unsigned long ms = (unsigned long)((svcGetSystemTick() - sDrift.t0) / (uint64_t)CPU_TICKS_PER_MSEC);
    if (sDriftArm < 0 || sDrift.slots == 0) {
        return;
    }
    snprintf(line, sizeof(line),
             "[d1drift] arm=%s slots=%lu d1=%lu d2+=%lu repeats=%lu startmiss=%lu forgiven=%lu downs=%lu ups=%lu ms=%lu "
             "fps=%lu.%lu hold_pct=%u end=%s (debug.steady_d1_drift=%d)",
             sDriftArm ? "on" : "off", sDrift.slots, sDrift.d1Slots, sDrift.d2Slots, sDrift.repeats, sDrift.oldMissed,
             sDrift.forgiven, sDrift.downs, sDrift.ups, ms, ms != 0 ? (sDrift.slots * 1000ul) / ms : 0ul,
             ms != 0 ? ((sDrift.slots * 10000ul) / ms) % 10ul : 0ul, sD1DriftHoldPct, why, sD1Drift);
    FrameskipLog(line);
}

static void D1DriftCloseStretch(const char* why) {
    EmitD1Drift(why);
    memset(&sDrift, 0, sizeof(sDrift));
    sDriftArm = -1;
}

/* The arm this slot runs under, opening a new stretch when it changed (lever off: -1, no receipt). */
static int D1DriftArmForSlot(void) {
    int arm;
    if (sD1Drift == 0 || !sLockstep || StereoOn()) {
        if (sDriftArm >= 0) {
            D1DriftCloseStretch(sLockstep ? "3d" : "race");
        }
        return -1;
    }
    arm = gdx3ds_cpucut_on(sD1Drift, GDX3DS_CPUCUT_BIT_D1DRIFT) ? 1 : 0;
    if (arm != sDriftArm) {
        D1DriftCloseStretch("block");
        sDriftArm = arm;
        sDrift.t0 = svcGetSystemTick();
    }
    return arm;
}

/* This D = 1 slot's frame repeated a vblank (the display clock is kept for every D = 1 slot, on
 * either arm, so both arms report the same repeats= measure). */
static int D1DriftRepeat(void) {
    uint64_t display;
    int repeat;
    if (sSlot.d != 1) {
        sD1PrevDisplay = 0;
        return 0;
    }
    display = gdx_steady_d1_display(sSlot.vblank, svcGetSystemTick(), GDX_STEADY_PRESENT_MARGIN_TICKS,
                                    GDX_STEADY_PERIOD_TICKS);
    repeat = gdx_steady_d1_repeats(sD1PrevDisplay, display, GDX_STEADY_PERIOD_TICKS);
    sD1PrevDisplay = display;
    return repeat;
}

/* o3ds60 steady_d1_rate: this D = 1 slot is judged on its start only. */
static int SteadyD1RateRule(void) {
    return sSlot.d == 1 && (sD1Rate == 2 || (sD1Rate == 1 && sLockstep));
}

/* Hold: present at vblank S+D-1 so the swap latches at S+D (the next slot's first vblank). A
 * present that could not make S+D (late work) is a missed slot, like a late start. */
void gdx3ds_frameskip_before_present(void) {
    uint64_t rw;
    uint64_t display;
    uint64_t woke;
    int missed;
    int result;
    int from;
    uint32_t missBefore;
    uint32_t hardBefore;
    int excused;
    int latePres;
    int driftArm;
    int repeat;
    unsigned holdPctSaved;
    uint64_t hold = 0;
    if (sMode != GDX_FRAMESKIP_STEADY || !sSlot.pacedRender) {
        return;
    }
    rw = svcGetSystemTick() - sSlot.renderBegin;
    sRace0SteadyRw = rw;
    display = sSlot.vblank + (uint64_t)sSlot.d * GDX_STEADY_PERIOD_TICKS;
    if (sSlot.d > 1) {
        const uint64_t holdStart = svcGetSystemTick();
        woke = WaitVblankNear(display - GDX_STEADY_PERIOD_TICKS, SteadyHoldOn());
        hold = svcGetSystemTick() - holdStart;
        if (woke != 0) {
            sSlot.vblank = woke - (uint64_t)(sSlot.d - 1) * GDX_STEADY_PERIOD_TICKS;
            display = woke + GDX_STEADY_PERIOD_TICKS;
        }
    }
    latePres = !sSlot.beginMissed && svcGetSystemTick() + GDX_STEADY_PRESENT_MARGIN_TICKS > display;
    if (sProbeSlot != 0) {
        /* o3ds3d [ceiling]: a probe slot is measured, never scored (the controller keeps its D). */
        ProbeNoteSlot(rw, latePres);
        sLastSlotMissed = sSlot.beginMissed;
        sPresentPrev = 0;
        sSlot.pacedRender = 0;
        sLastPresentTick = svcGetSystemTick();
        return;
    }
    missed = sSlot.beginMissed || svcGetSystemTick() + GDX_STEADY_PRESENT_MARGIN_TICKS > display;
    if (missed && !sSlot.beginMissed && SteadyD1RateRule()) {
        missed = 0; /* o3ds60 steady_d1_rate: on time at its start, one vblank more latency */
        sD1.latePresent++;
    }
    excused = missed && !sSlot.beginMissed && NetExcusedPresent(display);
    if (excused) {
        missed = 0;
        sWinNetLate++;
    }
    driftArm = D1DriftArmForSlot();
    repeat = D1DriftRepeat();
    if (driftArm >= 0) {
        sDrift.slots++;
        if (sSlot.d == 1) {
            sDrift.d1Slots++;
            sDrift.repeats += repeat ? 1u : 0u;
            sDrift.oldMissed += missed ? 1u : 0u;
        } else {
            sDrift.d2Slots++;
        }
    }
    if (driftArm == 1 && sSlot.d == 1 && missed && !repeat) {
        missed = 0; /* o3ds-mp16 steady_d1_drift: a late start, but every vblank still showed a new frame */
        sDrift.forgiven++;
    }
    NoteSlotD1(rw, missed, excused, display);
    sLastSlotMissed = missed;
    if (missed) {
        sWinMissed++;
    }
    if (rw > sWinRwMax) {
        sWinRwMax = rw;
    }
    from = sSteady.d;
    missBefore = sSteady.missBits;
    hardBefore = sSteady.hardMissBits;
    holdPctSaved = sSteady.d1HoldPct;
    if (driftArm == 1) {
        sSteady.d1HoldPct = sD1DriftHoldPct; /* o3ds-mp16: the hold counts repeated frames now */
    }
    result = gdx_steady_note_slot_ex(&sSteady, rw, missed, sSlotSkipMax);
    sSteady.d1HoldPct = holdPctSaved;
    if (driftArm >= 0 && result == GDX_STEADY_DOWN) {
        sDrift.downs++;
    } else if (driftArm >= 0 && result == GDX_STEADY_UP) {
        sDrift.ups++;
    }
    sSlotSkipMax = 0;
    sSteadyLastHard = missed && !sSteady.lastExplained;
    if (sSteadyLastHard) {
        sWinHardMissed++;
    }
    if (sSteady.lastSpike) {
        sWinSpikes++;
    }
    if (result != GDX_STEADY_HOLD) {
        sWinDChanges++;
        LogDChange(from, result, rw, (missBefore << 1) | (missed ? 1u : 0u),
                   (hardBefore << 1) | (sSteadyLastHard ? 1u : 0u));
    }
    if (sD1Pending) {
        NoteSlotD1Hard(sSteadyLastHard);
    }
    Lock30NoteSlot(rw, hold, missed, latePres, result != GDX_STEADY_HOLD); /* o3ds3d */
    sSlot.pacedRender = 0;
    sLastPresentTick = svcGetSystemTick();
}

/* Rate x10 over the window, as "whole.tenth" without float printf. */
static unsigned long RateX10(unsigned long count, unsigned long dtMs) {
    return dtMs != 0 ? (count * 10000ul) / dtMs : 0;
}

static void EmitSteadyWindow(unsigned long frame) {
    char line[256];
    const unsigned long p50 = TicksToMsX10(gdx_steady_rw_pct(&sSteady, 50));
    const unsigned long p90 = TicksToMsX10(gdx_steady_rw_pct(&sSteady, 90));
    const unsigned long mx = TicksToMsX10(sWinRwMax);
    const unsigned long sk = TicksToMsX10(sSteady.skipAvg);
    const unsigned long c90 = TicksToMsX10(gdx_steady_rw_pct_clean(&sSteady, 90));
    snprintf(line, sizeof(line),
             "[frameskip] steady frame=%lu D=%d hz=%lu.%lu dchg=%lu missed=%lu catchup=%lu rw50=%lu.%lu "
             "rw90=%lu.%lu rwmax=%lu.%lu skip=%lu.%lu ms hard=%lu spikes=%lu cln90=%lu.%lu netlate=%lu",
             frame, sSteady.d, 598u / (unsigned)sSteady.d / 10u, (598u / (unsigned)sSteady.d) % 10u, sWinDChanges,
             sWinMissed, sWinCatchup, p50 / 10, p50 % 10, p90 / 10, p90 % 10, mx / 10, mx % 10, sk / 10, sk % 10,
             sWinHardMissed, sWinSpikes, c90 / 10, c90 % 10, sWinNetLate);
    FrameskipLog(line);
    EmitSteadyD1Window(frame);
    EmitLock30Window(frame); /* o3ds3d */
    sWinNetLate = 0;
    sWinHardMissed = 0;
    sWinSpikes = 0;
    sWinMissed = 0;
    sWinCatchup = 0;
    sWinDChanges = 0;
    sWinRwMax = 0;
}

static void EmitWindow(unsigned long frame) {
    char line[256];
    const uint64_t now = svcGetSystemTick();
    const unsigned long dtMs = (unsigned long)((now - sWinStartTick) / (uint64_t)CPU_TICKS_PER_MSEC);
    const unsigned long tps = RateX10(sWinFrames, dtMs);
    const unsigned long fps = RateX10(sWinRendered, dtMs);
    snprintf(line, sizeof(line),
             "[frameskip] frame=%lu n=%lu dtMs=%lu rendered=%lu skipped=%lu veto=%lu dropped=%lu "
             "tps=%lu.%lu fps=%lu.%lu maxrun=%d lagmaxms=%lu",
             frame, sWinFrames, dtMs, sWinRendered, sWinSkipped, sWinVetoed, sWinDroppedTasks, tps / 10, tps % 10,
             fps / 10, fps % 10, sWinMaxRun, (unsigned long)(sWinMaxLag / (uint64_t)CPU_TICKS_PER_MSEC));
    if (sLockstep || sWinNetWait != 0) {
        /* NETTICK: the window's lockstep pad wait (left out of every cost sample above). */
        const size_t len = strlen(line);
        snprintf(line + len, sizeof(line) - len, " netwaitms=%lu",
                 (unsigned long)(sWinNetWait / (uint64_t)CPU_TICKS_PER_MSEC));
        sWinNetWait = 0;
    }
    FrameskipLog(line);
    if (sMode == GDX_FRAMESKIP_STEADY) {
        EmitSteadyWindow(frame);
    }
    gdx3ds_pipe_window(frame); /* o3ds60: after the receipts of the window it closes */
    sWinStartTick = now;
    sWinFrames = 0;
    sWinRendered = 0;
    sWinSkipped = 0;
    sWinVetoed = 0;
    sWinDroppedTasks = 0;
    sWinMaxRun = 0;
    sWinMaxLag = 0;
}

void gdx3ds_frameskip_end_iteration(unsigned long frame) {
    if (!sEnabled) {
        return;
    }
    sDropTasks = 0;
    if (sWinFrames >= GDX_FRAMESKIP_WINDOW) {
        EmitWindow(frame);
    }
}

void gdx3ds_frameskip_hitch_info(int* d, int* late) {
    *d = sMode == GDX_FRAMESKIP_STEADY ? sSlot.d : 0;
    *late = sLastSlotMissed;
}

int gdx3ds_frameskip_drop_task(void) {
    if (sDropTasks == 0) {
        return 0;
    }
    sWinDroppedTasks++;
    return 1;
}

unsigned long gdx3ds_frameskip_hollow_forced(void) {
    return sHollowForced;
}
