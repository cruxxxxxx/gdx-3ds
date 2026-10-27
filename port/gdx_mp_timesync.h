/* port/gdx_mp_timesync.h — mp-timesync: GGPO-style time sync for the lockstep race, no rollback.
 *
 * THE PROBLEM. In lockstep the faster console runs until it lacks a slower peer's pad, then
 * freezes until it arrives (hardware, online1: the New 3DS host stalled 892 times, ~50 ms each,
 * waiting on the Old 3DS guest). The race's pace is the slow console's either way; the stalls
 * only decide how unevenly the fast one spends the difference.
 *
 * THE FIX (GGPO timesync.cpp, recommend_frame_wait_duration, adapted). Each console estimates its
 * FRAME ADVANTAGE (lead) over the slowest live peer from data it already has -- no wire change:
 *   - the newest pad held from peer p is tick known[p]-1, which p pushed when it was about to run
 *     tick known[p]-1-delay, and sent at once;
 *   - it arrived at knownAtMs[p] (gdx_mp_ls_ingest) and spent about rtt/2 in flight, so p is now
 *     about (rtt/2 + age) / period ticks further on (capped at `delay`: no peer runs past its wall);
 *   - lead = lt - min over live peers of that estimate, smoothed by an EMA (1/16 per tick).
 * A console whose smoothed lead reaches `onMilli` ENGAGES (hysteresis: it lets go only once the lead
 * falls to `offMilli`) and STRETCHES its ticks by a PI controller on e = lead - setMilli:
 *   s = clamp(I + gainUs * e, 0, capUs),   I = clamp(I + integUs * e, 0, capUs) per tick,
 * slewed at most slewUs per tick; capUs defaults to 3 ms. The integral finds the rate mismatch, so
 * the fast console settles about setMilli ticks ahead instead of at the wall (`delay` ticks), and
 * every tick costs it about the mismatch instead of a freeze every few ticks. Disengaged, I bleeds
 * away (1/32 per tick). The slower console sees a negative lead and never stretches.
 *
 * HOW A STRETCH IS APPLIED. A tick may not run before max(E, P) + s after the previous one ran
 * (E = the time the console took to come back to the exchange, P = one vblank): the pacing sleep is
 * max(s, last + P + s - now). On a console that waits on the vblank (New 3DS in a race, frameskip
 * off), a plain s-ms sleep would vanish into the vblank wait; the schedule form makes the sleep
 * grow until the frame no longer catches the vblank it was waiting for, after which the console
 * free-runs at P + s per tick (citro3d's adaptive sync skips a passed vblank: gdx3ds_gpu_prof.c).
 * On a CPU-bound console (E > P) it is a plain s-ms sleep.
 *
 * DETERMINISM. Wall clock only. The module never touches a pad, the tick -> pad mapping, the hash,
 * or anything the simulation reads; a console with it on races a console with it off.
 *
 * Pure: no clock, no socket, no global. Host-tested by port/tests/gdx_mp_timesync_host_tests.c
 * (a two-clock model of a fast + a 10%-slower console over the real lockstep core). */
#ifndef GDX_MP_TIMESYNC_H
#define GDX_MP_TIMESYNC_H

#include <stdint.h>

#include "gdx_mp_lockstep.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GDX_MP_TS_VBLANK_US 16713u      /* one top-LCD refresh, 59.8337 Hz */
#define GDX_MP_TS_DEFAULT_CAP_US 3000u  /* [net] mp_timesync_max_ms = 3 */
#define GDX_MP_TS_MAX_CAP_US 16000u
#define GDX_MP_TS_GAIN_US 2000u         /* P: stretch per tick of lead error */
#define GDX_MP_TS_INTEG_US 30u          /* I: stretch change per tick, per tick of lead error */
#define GDX_MP_TS_SLEW_US 250u          /* stretch change per tick, at most */
#define GDX_MP_TS_EMA_SHIFT 4           /* lead EMA weight 1/16 (~30-tick window) */
#define GDX_MP_TS_RTT_CAP_MS 1000u      /* RTT samples above this are clamped */

typedef struct GdxMpTsConfig {
    int enabled;          /* 0 = measure only, never sleep */
    uint32_t capUs;       /* stretch ceiling */
    uint32_t periodUs;    /* the nominal tick (one vblank) */
    uint32_t gainUs;      /* P: stretch per tick of lead error */
    uint32_t integUs;     /* I: stretch change per tick, per tick of lead error */
    uint32_t slewUs;      /* stretch change per tick, at most */
    int32_t onMilli;      /* engage at this smoothed lead (milli-ticks) */
    int32_t offMilli;     /* release at this smoothed lead */
    int32_t setMilli;     /* the lead the controller holds while engaged */
} GdxMpTsConfig;

/* Per stats window (and whole-race totals): what the receipts print. */
typedef struct GdxMpTsStats {
    uint32_t ticks;       /* paced-or-not race ticks seen */
    uint32_t leadTicks;   /* ... with a lead estimate */
    uint32_t pacedTicks;  /* ... that slept */
    uint64_t sleptUs;     /* pacing sleep, total (it includes vblank waits it moved, see above) */
    uint64_t stretchSumUs; /* the stretch s of every paced tick: how much longer than max(E, P) */
    uint32_t maxSleepUs;
    uint32_t maxStretchUs;
    int32_t leadMinMilli; /* smoothed lead range */
    int32_t leadMaxMilli;
    int64_t leadSumMilli; /* raw lead sum (mean = leadSumMilli / leadTicks) */
    uint32_t absorbed;    /* paced ticks whose pads were missing when the sleep began and that then
                           * ran without a stall: a stall the pacing took in (an estimate: pads
                           * may already have sat undrained in the socket) */
    uint32_t engages;     /* off -> on transitions */
} GdxMpTsStats;

typedef struct GdxMpTimesync {
    GdxMpTsConfig cfg;
    int delay;
    int haveRun;
    uint64_t lastRunUs;   /* when the previous tick ran (its pads were all in) */
    uint32_t periodEmaUs; /* run-to-run period, smoothed (the shared race pace) */
    int haveLead;
    int32_t leadEmaMilli;
    int32_t lastLeadMilli;
    int lastWorstPeer;    /* the peer the latest lead was measured against, -1 = none */
    int active;
    uint32_t stretchUs;
    int64_t integNanoUs;  /* the controller's integral, in 1/1000 us */
    uint32_t rttEmaUs[GDX_MP_MAX_PLAYERS];
    uint32_t rttSeen[GDX_MP_MAX_PLAYERS]; /* ls->peer[p].rttSamples already folded */
    int pendingPaced;     /* the current tick slept */
    int pendingMissing;   /* ... and its pads were not all in when it began */
    GdxMpTsStats win;
    GdxMpTsStats total;
} GdxMpTimesync;

/* The defaults above for a race at `delay`: onMilli = min(1500, 500 * delay), setMilli = 2/3 of it,
 * offMilli = 1/3 of it (1.5 / 1.0 / 0.5 ticks from delay 3 up). */
void gdx_mp_ts_default_config(GdxMpTsConfig* cfg, int delay, int enabled, uint32_t capUs);

void gdx_mp_ts_init(GdxMpTimesync* ts, const GdxMpTsConfig* cfg, int delay);

/* Folds any new RTT samples of the live peers into the per-peer EMAs. Once per tick. */
void gdx_mp_ts_observe(GdxMpTimesync* ts, const GdxMpLockstep* ls);

/* The raw lead of this console at race tick `lt` (about to run it) over its slowest live peer, in
 * milli-ticks; *worstPeer = that peer. 0 = no estimate yet (no real pad from some live peer). */
int gdx_mp_ts_lead(const GdxMpTimesync* ts, const GdxMpLockstep* ls, uint32_t lt, uint32_t nowMs,
                   int32_t* leadMilli, int* worstPeer);

/* One call per race tick, after our pad went out and before waiting for the peers' pads: updates
 * the smoothed lead and the stretch and returns the pacing sleep in microseconds (0 = none).
 * `padsMissing` = the tick's pads are not all held yet (receipts). nowUs: a monotonic clock. */
uint32_t gdx_mp_ts_pace(GdxMpTimesync* ts, const GdxMpLockstep* ls, uint32_t lt, uint32_t nowMs, uint64_t nowUs,
                        int padsMissing);

/* The tick's pads are all in and it runs now. `stalled` = the wait for them was a stall. */
void gdx_mp_ts_ran(GdxMpTimesync* ts, uint64_t nowUs, int stalled);

/* Clears the window counters (after its receipt). */
void gdx_mp_ts_window_reset(GdxMpTimesync* ts);

/* Mean raw lead of a stats block in milli-ticks (0 when it has none). */
int32_t gdx_mp_ts_mean_lead(const GdxMpTsStats* st);

#ifdef __cplusplus
}
#endif

#endif /* GDX_MP_TIMESYNC_H */
