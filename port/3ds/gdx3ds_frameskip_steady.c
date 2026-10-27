/* port/3ds/gdx3ds_frameskip_steady.c — FRAMESKIP `steady`: the even-cadence divisor
 * controller. Contract in gdx3ds_frameskip.h. Pure (no 3DS dependency): host-tested. */
#include "gdx3ds_frameskip.h"

#include <string.h>

#define GDX_STEADY_UP_PCT_DEFAULT 80u /* a step up needs the p90 render to fit D-1 at 80% */
#define GDX_STEADY_UP_DWELL_TICKS 120u /* ~2 s of logic at the current D before stepping up */
#define GDX_STEADY_UP_DWELL_MAX_TICKS 960u /* the backoff cap: ~16 s */
#define GDX_STEADY_STABLE_TICKS 1200u /* ~20 s at one D resets the backoff */
#define GDX_STEADY_RECENT 8 /* the step-down test looks at the last 8 renders only */
#define GDX_STEADY_UP_CLEAN_MASK 0xFFFFu /* no miss in the last 16 slots */
#define GDX_STEADY_DOWN_WINDOW_MASK 0xFFu /* misses counted over the last 8 slots */
#define GDX_STEADY_DOWN_MISSES 2
#define GDX_STEADY_UP_MIN_SAMPLES 16
#define GDX_STEADY_DOWN_MIN_SAMPLES 4 /* a boot/load burst of 2 frames is not a measurement */
/* HITCH (robust mode, the default): what an isolated spike is, and how many the rules forgive. */
#define GDX_STEADY_SPIKE_MIN_SAMPLES 8  /* a median needs this many renders before anything is a spike */
#define GDX_STEADY_SPIKE_ISOLATION 0xFFu /* a render over 2x median is a spike while the last 8 hold < 2 more */
#define GDX_STEADY_SPIKE_MAX_RECENT 2
#define GDX_STEADY_FLOOD_MISSES 6       /* 6 of the last 8 slots missed, whatever the cause: that is load */
#define GDX_STEADY_UP_MAX_SPIKE_MISSES 4 /* spike-explained misses tolerated in the 16-slot up window */
#define GDX_STEADY_UP_MAX_SPIKES (GDX_STEADY_RING / 4) /* more spikes than this in the ring: not isolated */

void gdx_steady_init(GdxSteadyCtl* ctl, int dMin, int dMax, int dStart, uint64_t period, uint64_t margin) {
    memset(ctl, 0, sizeof(*ctl));
    ctl->dMin = dMin;
    ctl->dMax = dMax;
    ctl->d = dStart < dMin ? dMin : (dStart > dMax ? dMax : dStart);
    ctl->period = period;
    ctl->margin = margin;
    ctl->upDwell = GDX_STEADY_UP_DWELL_TICKS;
    ctl->robust = 1;
    ctl->upPct = GDX_STEADY_UP_PCT_DEFAULT;
    ctl->d1HoldSlots = 16u;
}

void gdx_steady_set_robust(GdxSteadyCtl* ctl, int on) {
    ctl->robust = on ? 1 : 0;
}

void gdx_steady_set_up_pct(GdxSteadyCtl* ctl, unsigned pct) {
    if (pct < 50u) {
        pct = 50u;
    } else if (pct > 100u) {
        pct = 100u;
    }
    ctl->upPct = pct;
}

void gdx_steady_set_d1_hold_pct(GdxSteadyCtl* ctl, unsigned pct) {
    ctl->d1HoldPct = pct > 100u ? 100u : pct;
}

void gdx_steady_set_d1_hold_slots(GdxSteadyCtl* ctl, unsigned slots) {
    if (slots < 16u) {
        slots = 16u;
    } else if (slots > 64u) {
        slots = 64u;
    }
    ctl->d1HoldSlots = slots;
}

void gdx_steady_set_d1_retry(GdxSteadyCtl* ctl, unsigned long ticks, unsigned pct) {
    if (pct < 50u) {
        pct = 50u;
    } else if (pct > 100u) {
        pct = 100u;
    }
    ctl->d1RetryTicks = ticks;
    ctl->d1RetryPct = pct;
}

static void ChangeD(GdxSteadyCtl* ctl, int d);

void gdx_steady_pin_min(GdxSteadyCtl* ctl, int dMin) {
    if (dMin < 1) {
        dMin = 1;
    } else if (dMin > ctl->dMax) {
        dMin = ctl->dMax;
    }
    ctl->dMin = dMin;
    if (ctl->d < dMin) {
        ChangeD(ctl, dMin);
        ctl->lastWasUp = 0; /* a pin is not a failed step up: no backoff */
    }
}

/* ROBUST: a skip tick more than half a vblank over the average is a stall (the hardware's SD
 * flush), not the cost of a skip: it stays out of the average, unless 4 in a row say the skip
 * tick really got that expensive. */
#define GDX_STEADY_SKIP_STALL_RUN 4

static int SkipIsStall(GdxSteadyCtl* ctl, uint64_t skipCost) {
    if (!ctl->robust || ctl->skipAvg == 0 || skipCost <= ctl->skipAvg + ctl->period / 2u) {
        ctl->skipStallRun = 0;
        return 0;
    }
    ctl->skipStallRun++;
    return ctl->skipStallRun < GDX_STEADY_SKIP_STALL_RUN;
}

void gdx_steady_note_skip(GdxSteadyCtl* ctl, uint64_t skipCost) {
    if (SkipIsStall(ctl, skipCost)) {
        ctl->ticksAtD++;
        return;
    }
    /* A skip tick that hit a load stall is not the cost of a skip: clamp it so one hitch
     * cannot make every larger D look unaffordable. */
    if (skipCost > ctl->period) {
        skipCost = ctl->period;
    }
    if (ctl->skipAvg == 0) {
        ctl->skipAvg = skipCost;
    } else {
        ctl->skipAvg = (ctl->skipAvg * 7u + skipCost) / 8u;
    }
    ctl->ticksAtD++;
}

static int PopCount(uint32_t v) {
    int n = 0;
    while (v != 0) {
        v &= v - 1u;
        n++;
    }
    return n;
}

static int PopCount64(uint64_t v) {
    int n = 0;
    while (v != 0) {
        v &= v - 1u;
        n++;
    }
    return n;
}

/* The slot of D vblanks fits a render of work `rw` when the present -- at max(rw, (D-1)P), the
 * hold -- leaves room for BOTH the GPU's draw + display transfer (`margin`) and the D-1 skip
 * ticks before the display vblank at D*P:  present + max(margin, (D-1)L) <= D*P. */
static int SlotFits(const GdxSteadyCtl* ctl, int d, uint64_t rw, unsigned num, unsigned den) {
    const uint64_t skips = (uint64_t)(d - 1) * ctl->skipAvg;
    const uint64_t tail = skips > ctl->margin ? skips : ctl->margin;
    const uint64_t budget = ((uint64_t)d * ctl->period * num) / den;
    if (tail > ctl->period) {
        return 0; /* the hold presents at (D-1)P: the tail has one vblank */
    }
    return rw + tail <= budget;
}

int gdx_steady_pick(const GdxSteadyCtl* ctl, uint64_t rw, unsigned num, unsigned den) {
    for (int d = ctl->dMin; d <= ctl->dMax; d++) {
        if (SlotFits(ctl, d, rw, num, den)) {
            return d;
        }
    }
    return ctl->dMax;
}

/* Percentile over the newest `last` samples (all of them when last >= rwN). */
static uint64_t RwPctRecent(const GdxSteadyCtl* ctl, int pct, int last) {
    uint64_t sorted[GDX_STEADY_RING];
    int n = ctl->rwN < last ? ctl->rwN : last;
    if (n == 0) {
        return 0;
    }
    for (int i = 0; i < n; i++) {
        sorted[i] = ctl->rw[(ctl->rwPos - 1 - i + 2 * GDX_STEADY_RING) % GDX_STEADY_RING];
    }
    for (int i = 1; i < n; i++) {
        const uint64_t v = sorted[i];
        int j = i - 1;
        while (j >= 0 && sorted[j] > v) {
            sorted[j + 1] = sorted[j];
            j--;
        }
        sorted[j + 1] = v;
    }
    {
        int idx = (pct * (n - 1) + 50) / 100;
        if (idx > n - 1) {
            idx = n - 1;
        }
        return sorted[idx];
    }
}

uint64_t gdx_steady_rw_pct(const GdxSteadyCtl* ctl, int pct) {
    return RwPctRecent(ctl, pct, GDX_STEADY_RING);
}

static int RingIndexBack(const GdxSteadyCtl* ctl, int i) {
    return (ctl->rwPos - 1 - i + 2 * GDX_STEADY_RING) % GDX_STEADY_RING;
}

/* Percentile over the newest `last` renders with the isolated spikes left out (robust mode).
 * Falls back to every sample when the window holds nothing but spikes. */
static uint64_t RwPctClean(const GdxSteadyCtl* ctl, int pct, int last) {
    uint64_t sorted[GDX_STEADY_RING];
    int n = 0;
    const int span = ctl->rwN < last ? ctl->rwN : last;
    for (int i = 0; i < span; i++) {
        const int idx = RingIndexBack(ctl, i);
        if ((ctl->rwSpikeMask & (1u << idx)) == 0) {
            sorted[n++] = ctl->rw[idx];
        }
    }
    if (n == 0) {
        return RwPctRecent(ctl, pct, last);
    }
    for (int i = 1; i < n; i++) {
        const uint64_t v = sorted[i];
        int j = i - 1;
        while (j >= 0 && sorted[j] > v) {
            sorted[j + 1] = sorted[j];
            j--;
        }
        sorted[j + 1] = v;
    }
    {
        int idx = (pct * (n - 1) + 50) / 100;
        if (idx > n - 1) {
            idx = n - 1;
        }
        return sorted[idx];
    }
}

uint64_t gdx_steady_rw_pct_clean(const GdxSteadyCtl* ctl, int pct) {
    return RwPctClean(ctl, pct, GDX_STEADY_RING);
}

static void PushRenderWork(GdxSteadyCtl* ctl, uint64_t rw, int spike) {
    if (spike) {
        ctl->rwSpikeMask |= 1u << ctl->rwPos;
    } else {
        ctl->rwSpikeMask &= ~(1u << ctl->rwPos);
    }
    ctl->rw[ctl->rwPos] = rw;
    ctl->rwPos = (ctl->rwPos + 1) % GDX_STEADY_RING;
    if (ctl->rwN < GDX_STEADY_RING) {
        ctl->rwN++;
    }
}

static void ChangeD(GdxSteadyCtl* ctl, int d) {
    if (d > ctl->d && ctl->lastWasUp && ctl->ticksAtD < 2u * ctl->upDwell) {
        /* the last step up did not hold: back off before trying again */
        ctl->upDwell = ctl->upDwell * 2u > GDX_STEADY_UP_DWELL_MAX_TICKS ? GDX_STEADY_UP_DWELL_MAX_TICKS
                                                                          : ctl->upDwell * 2u;
    }
    ctl->lastWasUp = d < ctl->d;
    ctl->d = d;
    ctl->ticksAtD = 0;
    ctl->missBits = 0;
    ctl->hardMissBits = 0;
    ctl->d1MissBits = 0;
}

static int D1Holds(GdxSteadyCtl* ctl);

static int TryStepDown(GdxSteadyCtl* ctl) {
    int want;
    if (PopCount(ctl->missBits & GDX_STEADY_DOWN_WINDOW_MASK) < GDX_STEADY_DOWN_MISSES) {
        return 0;
    }
    if (ctl->d >= ctl->dMax || ctl->rwN < GDX_STEADY_DOWN_MIN_SAMPLES) {
        return 0;
    }
    if (D1Holds(ctl)) {
        return 0;
    }
    /* Misses from isolated hitches (a load frame, a SHOT write) are not a slot that is too
     * small: step down only when the recent renders themselves do not fit. */
    if (SlotFits(ctl, ctl->d, RwPctRecent(ctl, 75, GDX_STEADY_RECENT), 1, 1)) {
        return 0;
    }
    want = gdx_steady_pick(ctl, RwPctRecent(ctl, 90, GDX_STEADY_RECENT), 95, 100);
    ChangeD(ctl, want > ctl->d ? want : ctl->d + 1);
    return 1;
}

static int TryStepUp(GdxSteadyCtl* ctl) {
    if (ctl->ticksAtD >= GDX_STEADY_STABLE_TICKS) {
        ctl->upDwell = GDX_STEADY_UP_DWELL_TICKS;
    }
    if (ctl->d <= ctl->dMin || ctl->ticksAtD < ctl->upDwell) {
        return 0;
    }
    if ((ctl->missBits & GDX_STEADY_UP_CLEAN_MASK) != 0 || ctl->rwN < GDX_STEADY_UP_MIN_SAMPLES) {
        return 0;
    }
    if (!SlotFits(ctl, ctl->d - 1, gdx_steady_rw_pct(ctl, 90), ctl->upPct, 100)) {
        return 0;
    }
    ChangeD(ctl, ctl->d - 1);
    return 1;
}

/* ---- ROBUST (HITCH): isolated spikes neither step D down nor block a step up ----
 *
 * A slot is a SPIKE slot when its render was over 2x the ring median with at most one such
 * render in the 8 slots before it (an isolated outlier, not a heavier scene), or when one of the skip
 * ticks before it ran more than half a vblank over the skip average (a stalled skip: the
 * hardware's SD flush). A miss in a spike slot, or in the slot right after one (the spike
 * delayed its start), is EXPLAINED; every other miss is HARD.
 *   DOWN: 2 hard misses in the last 8 slots and the spike-free p75 of the last 8 renders does
 *         not fit D (jump to what the spike-free p90 fits at 95%), or a FLOOD -- 6 of the last
 *         8 slots missed whatever the cause -- with the raw p75 not fitting (one step).
 *   UP:   the dwell, no hard miss and at most 4 explained misses in the last 16 slots, at most
 *         a quarter of the ring spikes, and the spike-free p90 fits D-1 at 80%.
 * A heavier scene is not a spike: its third over-2x render in 8 slots is no longer isolated,
 * so it counts as a hard miss and steps D down within four slots. */
/* o3ds60 d1HoldPct: D = 1 with at most (100 - pct)% of the last d1HoldSlots slots missed (a stint
 * younger than the window counts its unseen slots as hits) stays at D = 1. */
static uint64_t D1HoldMask(unsigned slots) {
    return slots >= 64u ? ~(uint64_t)0 : (((uint64_t)1 << slots) - 1u);
}

static int D1Holds(GdxSteadyCtl* ctl) {
    const unsigned slots = ctl->d1HoldSlots < 16u ? 16u : ctl->d1HoldSlots;
    const unsigned misses = (unsigned) PopCount64(ctl->d1MissBits & D1HoldMask(slots));
    if (ctl->d1HoldPct == 0u || ctl->d != 1) {
        return 0;
    }
    if (misses * 100u > (100u - ctl->d1HoldPct) * slots) {
        return 0;
    }
    ctl->d1Holds++;
    return 1;
}

static int TryStepDownRobust(GdxSteadyCtl* ctl) {
    const int hard = PopCount(ctl->hardMissBits & GDX_STEADY_DOWN_WINDOW_MASK) >= GDX_STEADY_DOWN_MISSES;
    const int flood = PopCount(ctl->missBits & GDX_STEADY_DOWN_WINDOW_MASK) >= GDX_STEADY_FLOOD_MISSES;
    int want;
    if ((!hard && !flood) || ctl->d >= ctl->dMax || ctl->rwN < GDX_STEADY_DOWN_MIN_SAMPLES) {
        return 0;
    }
    if (D1Holds(ctl)) {
        return 0;
    }
    if (hard) {
        if (SlotFits(ctl, ctl->d, RwPctClean(ctl, 75, GDX_STEADY_RECENT), 1, 1)) {
            return 0;
        }
        want = gdx_steady_pick(ctl, RwPctClean(ctl, 90, GDX_STEADY_RECENT), 95, 100);
        ChangeD(ctl, want > ctl->d ? want : ctl->d + 1);
        return 1;
    }
    if (SlotFits(ctl, ctl->d, RwPctRecent(ctl, 75, GDX_STEADY_RECENT), 1, 1)) {
        return 0;
    }
    ChangeD(ctl, ctl->d + 1);
    return 1;
}

/* o3ds60 d1RetryTicks: the D = 2 -> 1 step (see gdx_steady_set_d1_retry). */
static int D1RetryApplies(const GdxSteadyCtl* ctl) {
    return ctl->d1RetryTicks != 0u && ctl->d == 2 && ctl->dMin == 1;
}

static unsigned long UpDwellNow(const GdxSteadyCtl* ctl) {
    if (D1RetryApplies(ctl)) {
        const unsigned long cap = 2u * ctl->d1RetryTicks;
        const unsigned long dwell = ctl->upDwell < ctl->d1RetryTicks ? ctl->d1RetryTicks : ctl->upDwell;
        return dwell > cap ? cap : dwell;
    }
    return ctl->upDwell;
}

static int UpFits(const GdxSteadyCtl* ctl) {
    const uint64_t p90 = RwPctClean(ctl, 90, GDX_STEADY_RING);
    if (D1RetryApplies(ctl)) {
        return p90 * 100u <= ctl->period * ctl->d1RetryPct;
    }
    return SlotFits(ctl, ctl->d - 1, p90, ctl->upPct, 100);
}

static int TryStepUpRobust(GdxSteadyCtl* ctl) {
    if (ctl->ticksAtD >= GDX_STEADY_STABLE_TICKS) {
        ctl->upDwell = GDX_STEADY_UP_DWELL_TICKS;
    }
    if (ctl->d <= ctl->dMin || ctl->ticksAtD < UpDwellNow(ctl) || ctl->rwN < GDX_STEADY_UP_MIN_SAMPLES) {
        return 0;
    }
    if ((ctl->hardMissBits & GDX_STEADY_UP_CLEAN_MASK) != 0 ||
        PopCount(ctl->missBits & GDX_STEADY_UP_CLEAN_MASK) > GDX_STEADY_UP_MAX_SPIKE_MISSES) {
        return 0;
    }
    if (PopCount(ctl->rwSpikeMask) > GDX_STEADY_UP_MAX_SPIKES) {
        return 0;
    }
    if (!UpFits(ctl)) {
        return 0;
    }
    if (D1RetryApplies(ctl)) {
        ctl->d1Retries++;
    }
    ChangeD(ctl, ctl->d - 1);
    return 1;
}

/* Classify this slot (see above) before its render joins the ring. */
static int IsRenderSpike(const GdxSteadyCtl* ctl, uint64_t rw, int* over) {
    uint64_t median;
    *over = 0;
    if (ctl->rwN < GDX_STEADY_SPIKE_MIN_SAMPLES) {
        return 0;
    }
    median = RwPctRecent(ctl, 50, GDX_STEADY_RING);
    *over = rw > 2u * median;
    return *over && PopCount(ctl->overBits & GDX_STEADY_SPIKE_ISOLATION) < GDX_STEADY_SPIKE_MAX_RECENT;
}

static int IsSkipSpike(const GdxSteadyCtl* ctl, uint64_t skipMax) {
    return ctl->skipAvg != 0 && skipMax > ctl->skipAvg + ctl->period / 2u;
}

int gdx_steady_note_slot_ex(GdxSteadyCtl* ctl, uint64_t renderWork, int missed, uint64_t skipMax) {
    int over = 0;
    const int rwSpike = IsRenderSpike(ctl, renderWork, &over);
    const int spike = rwSpike || IsSkipSpike(ctl, skipMax);
    const int explained = missed && (spike || ctl->prevSlotSpike);
    PushRenderWork(ctl, renderWork, ctl->robust && rwSpike);
    ctl->missBits = (ctl->missBits << 1) | (missed ? 1u : 0u);
    ctl->d1MissBits = (ctl->d1MissBits << 1) | (missed ? 1u : 0u);
    ctl->hardMissBits = (ctl->hardMissBits << 1) | (missed && !explained ? 1u : 0u);
    ctl->overBits = (ctl->overBits << 1) | (over ? 1u : 0u);
    ctl->prevSlotSpike = spike;
    ctl->lastSpike = spike;
    ctl->lastExplained = explained;
    ctl->ticksAtD++;
    if (ctl->robust ? TryStepDownRobust(ctl) : TryStepDown(ctl)) {
        return GDX_STEADY_DOWN;
    } else if (ctl->robust ? TryStepUpRobust(ctl) : TryStepUp(ctl)) {
        return GDX_STEADY_UP;
    } else {
        return GDX_STEADY_HOLD;
    }
}

int gdx_steady_note_slot(GdxSteadyCtl* ctl, uint64_t renderWork, int missed) {
    return gdx_steady_note_slot_ex(ctl, renderWork, missed, 0);
}

int gdx_steady_late_by_net(uint64_t late, uint64_t netWait, uint64_t period) {
    if (netWait == 0) {
        return 0;
    }
    return late <= netWait || late - netWait <= period / 2u;
}

uint64_t gdx_steady_d1_display(uint64_t slotVblank, uint64_t now, uint64_t margin, uint64_t period) {
    uint64_t display = slotVblank + period;
    if (period == 0) {
        return display;
    }
    if (now + margin > display) {
        display += ((now + margin - display + period - 1u) / period) * period;
    }
    return display;
}

int gdx_steady_d1_repeats(uint64_t prevDisplay, uint64_t display, uint64_t period) {
    if (prevDisplay == 0 || display <= prevDisplay) {
        return 0;
    }
    return display - prevDisplay > period + period / 2u;
}
