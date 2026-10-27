/* port/gdx_mp_timesync.c — mp-timesync: frame-advantage pacing. Contract: gdx_mp_timesync.h. */
#include "gdx_mp_timesync.h"

#include <string.h>

#define TS_MAX_RUN_GAP_US 200000u /* a run-to-run gap past this is a stall, not the race pace */

static int PeerIsLive(const GdxMpLockstep* ls, int p) {
    return p != ls->localPlayer && ls->removedAt[p] == GDX_MP_NO_TICK && !ls->frozen[p];
}

static void StatsReset(GdxMpTsStats* st) {
    memset(st, 0, sizeof(*st));
}

void gdx_mp_ts_default_config(GdxMpTsConfig* cfg, int delay, int enabled, uint32_t capUs) {
    const int32_t on = 500 * (delay > 0 ? delay : 1);
    cfg->enabled = enabled ? 1 : 0;
    cfg->capUs = capUs > GDX_MP_TS_MAX_CAP_US ? GDX_MP_TS_MAX_CAP_US : capUs;
    cfg->periodUs = GDX_MP_TS_VBLANK_US;
    cfg->gainUs = GDX_MP_TS_GAIN_US;
    cfg->integUs = GDX_MP_TS_INTEG_US;
    cfg->slewUs = GDX_MP_TS_SLEW_US;
    cfg->onMilli = on < 1500 ? on : 1500;
    cfg->setMilli = cfg->onMilli * 2 / 3;
    cfg->offMilli = cfg->onMilli / 3;
}

void gdx_mp_ts_init(GdxMpTimesync* ts, const GdxMpTsConfig* cfg, int delay) {
    memset(ts, 0, sizeof(*ts));
    ts->cfg = *cfg;
    ts->delay = delay > 0 ? delay : 1;
    ts->periodEmaUs = cfg->periodUs;
    ts->lastWorstPeer = -1;
    StatsReset(&ts->win);
    StatsReset(&ts->total);
}

void gdx_mp_ts_window_reset(GdxMpTimesync* ts) {
    StatsReset(&ts->win);
}

int32_t gdx_mp_ts_mean_lead(const GdxMpTsStats* st) {
    if (st->leadTicks == 0) {
        return 0;
    }
    return (int32_t) (st->leadSumMilli / (int64_t) st->leadTicks);
}

/* ---- peer progress --------------------------------------------------------------------------- */
static void FoldRtt(GdxMpTimesync* ts, int p, uint32_t rttMs) {
    const uint32_t sampleUs = (rttMs > GDX_MP_TS_RTT_CAP_MS ? GDX_MP_TS_RTT_CAP_MS : rttMs) * 1000u;
    if (ts->rttEmaUs[p] == 0) {
        ts->rttEmaUs[p] = sampleUs;
    } else {
        const int64_t delta = (int64_t) sampleUs - (int64_t) ts->rttEmaUs[p];
        ts->rttEmaUs[p] = (uint32_t) ((int64_t) ts->rttEmaUs[p] + delta / 8);
    }
}

void gdx_mp_ts_observe(GdxMpTimesync* ts, const GdxMpLockstep* ls) {
    int p;
    for (p = 0; p < ls->numPlayers; p++) {
        if (PeerIsLive(ls, p) && ls->peer[p].rttSamples != ts->rttSeen[p]) {
            FoldRtt(ts, p, ls->peer[p].rttLastMs);
            ts->rttSeen[p] = ls->peer[p].rttSamples;
        }
    }
}

static uint32_t PacePeriodUs(const GdxMpTimesync* ts) {
    return ts->periodEmaUs > ts->cfg.periodUs ? ts->periodEmaUs : ts->cfg.periodUs;
}

/* Where peer p is now, in milli-ticks: the tick it was about to run when it sent its newest pad,
 * plus the flight and the age of that pad at the shared pace, capped at `delay` ticks of travel and
 * at our own wall (it cannot run a tick whose pad from us it lacks). */
static int64_t PeerNowMilli(const GdxMpTimesync* ts, const GdxMpLockstep* ls, int p, uint32_t nowMs) {
    const int64_t sentTick = (int64_t) ls->known[p] - 1 - ls->delay;
    const uint64_t ageUs = (uint64_t) (nowMs - ls->knownAtMs[p]) * 1000u + ts->rttEmaUs[p] / 2u;
    const int64_t capMilli = (int64_t) ts->delay * 1000;
    const int64_t wallMilli = ((int64_t) ls->known[ls->localPlayer] - 1) * 1000;
    int64_t travelMilli = (int64_t) (ageUs * 1000u / PacePeriodUs(ts));
    int64_t milli;
    if (travelMilli > capMilli) {
        travelMilli = capMilli;
    }
    milli = sentTick * 1000 + travelMilli;
    return milli < wallMilli ? milli : wallMilli;
}

int gdx_mp_ts_lead(const GdxMpTimesync* ts, const GdxMpLockstep* ls, uint32_t lt, uint32_t nowMs,
                   int32_t* leadMilli, int* worstPeer) {
    int p;
    int have = 0;
    int64_t worst = 0;
    *worstPeer = -1;
    *leadMilli = 0;
    for (p = 0; p < ls->numPlayers; p++) {
        int64_t lead;
        if (!PeerIsLive(ls, p)) {
            continue;
        }
        if (ls->known[p] <= (uint32_t) ls->delay) {
            return 0; /* no real pad from this peer yet: the race start, no estimate */
        }
        lead = (int64_t) lt * 1000 - PeerNowMilli(ts, ls, p, nowMs);
        if (!have || lead > worst) {
            worst = lead;
            *worstPeer = p;
            have = 1;
        }
    }
    if (!have) {
        return 0;
    }
    *leadMilli = (int32_t) worst;
    return 1;
}

/* ---- the stretch ------------------------------------------------------------------------------- */
static void NoteLead(GdxMpTsStats* st, int32_t rawMilli, int32_t emaMilli) {
    if (st->leadTicks == 0 || emaMilli < st->leadMinMilli) {
        st->leadMinMilli = emaMilli;
    }
    if (st->leadTicks == 0 || emaMilli > st->leadMaxMilli) {
        st->leadMaxMilli = emaMilli;
    }
    st->leadTicks++;
    st->leadSumMilli += rawMilli;
}

static void SmoothLead(GdxMpTimesync* ts, int32_t leadMilli) {
    if (!ts->haveLead) {
        ts->leadEmaMilli = leadMilli;
        ts->haveLead = 1;
    } else {
        ts->leadEmaMilli += (leadMilli - ts->leadEmaMilli) / (1 << GDX_MP_TS_EMA_SHIFT);
    }
    ts->lastLeadMilli = leadMilli;
    NoteLead(&ts->win, leadMilli, ts->leadEmaMilli);
    NoteLead(&ts->total, leadMilli, ts->leadEmaMilli);
}

static void UpdateHysteresis(GdxMpTimesync* ts) {
    if (!ts->active && ts->leadEmaMilli >= ts->cfg.onMilli) {
        ts->active = 1;
        ts->win.engages++;
        ts->total.engages++;
    } else if (ts->active && ts->leadEmaMilli <= ts->cfg.offMilli) {
        ts->active = 0;
    }
}

static int64_t ClampI64(int64_t v, int64_t lo, int64_t hi) {
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

/* The PI step: the integral (in 1/1000 us, so a 30 us/tick gain moves it on milli-tick errors). */
static void UpdateIntegral(GdxMpTimesync* ts, int64_t errMilli) {
    const int64_t capNano = (int64_t) ts->cfg.capUs * 1000;
    if (ts->active) {
        ts->integNanoUs = ClampI64(ts->integNanoUs + errMilli * (int64_t) ts->cfg.integUs, 0, capNano);
    } else {
        ts->integNanoUs -= ts->integNanoUs / 32;
    }
}

static uint32_t TargetStretchUs(GdxMpTimesync* ts) {
    const int64_t errMilli = (int64_t) ts->leadEmaMilli - ts->cfg.setMilli;
    UpdateIntegral(ts, errMilli);
    if (!ts->active) {
        return 0;
    }
    return (uint32_t) ClampI64(ts->integNanoUs / 1000 + errMilli * (int64_t) ts->cfg.gainUs / 1000, 0,
                               (int64_t) ts->cfg.capUs);
}

static void SlewStretch(GdxMpTimesync* ts, uint32_t targetUs) {
    if (targetUs > ts->stretchUs) {
        const uint32_t up = targetUs - ts->stretchUs;
        ts->stretchUs += up < ts->cfg.slewUs ? up : ts->cfg.slewUs;
    } else {
        const uint32_t down = ts->stretchUs - targetUs;
        ts->stretchUs -= down < ts->cfg.slewUs ? down : ts->cfg.slewUs;
    }
}

/* The tick may not run before max(E, P) + s after the previous one: max(s, last + P + s - now). */
static uint32_t PacingSleepUs(const GdxMpTimesync* ts, uint64_t nowUs) {
    uint64_t sleepUs = ts->stretchUs;
    if (ts->haveRun) {
        const uint64_t due = ts->lastRunUs + ts->cfg.periodUs + ts->stretchUs;
        if (due > nowUs + sleepUs) {
            sleepUs = due - nowUs;
        }
    }
    return (uint32_t) (sleepUs < (uint64_t) ts->cfg.periodUs + ts->cfg.capUs ? sleepUs
                                                                              : ts->cfg.periodUs + ts->cfg.capUs);
}

static void NotePaced(GdxMpTsStats* st, uint32_t sleepUs, uint32_t stretchUs) {
    st->pacedTicks++;
    st->sleptUs += sleepUs;
    st->stretchSumUs += stretchUs;
    if (sleepUs > st->maxSleepUs) {
        st->maxSleepUs = sleepUs;
    }
    if (stretchUs > st->maxStretchUs) {
        st->maxStretchUs = stretchUs;
    }
}

uint32_t gdx_mp_ts_pace(GdxMpTimesync* ts, const GdxMpLockstep* ls, uint32_t lt, uint32_t nowMs, uint64_t nowUs,
                        int padsMissing) {
    int32_t leadMilli;
    int worstPeer;
    uint32_t sleepUs;
    ts->win.ticks++;
    ts->total.ticks++;
    ts->pendingPaced = 0;
    gdx_mp_ts_observe(ts, ls);
    if (gdx_mp_ts_lead(ts, ls, lt, nowMs, &leadMilli, &worstPeer)) {
        SmoothLead(ts, leadMilli);
        ts->lastWorstPeer = worstPeer;
        UpdateHysteresis(ts);
        SlewStretch(ts, TargetStretchUs(ts));
    } else {
        SlewStretch(ts, 0);
    }
    if (!ts->cfg.enabled || ts->stretchUs == 0) {
        return 0;
    }
    sleepUs = PacingSleepUs(ts, nowUs);
    ts->pendingPaced = 1;
    ts->pendingMissing = padsMissing ? 1 : 0;
    NotePaced(&ts->win, sleepUs, ts->stretchUs);
    NotePaced(&ts->total, sleepUs, ts->stretchUs);
    return sleepUs;
}

void gdx_mp_ts_ran(GdxMpTimesync* ts, uint64_t nowUs, int stalled) {
    if (ts->haveRun && nowUs >= ts->lastRunUs) {
        const uint64_t rawGapUs = nowUs - ts->lastRunUs;
        const uint32_t gapUs = rawGapUs > TS_MAX_RUN_GAP_US ? TS_MAX_RUN_GAP_US : (uint32_t) rawGapUs;
        const int64_t delta = (int64_t) gapUs - (int64_t) ts->periodEmaUs;
        ts->periodEmaUs = (uint32_t) ((int64_t) ts->periodEmaUs + delta / (1 << GDX_MP_TS_EMA_SHIFT));
    }
    ts->lastRunUs = nowUs;
    ts->haveRun = 1;
    if (ts->pendingPaced && ts->pendingMissing && !stalled) {
        ts->win.absorbed++;
        ts->total.absorbed++;
    }
    ts->pendingPaced = 0;
    ts->pendingMissing = 0;
}
