/* port/3ds/test/gdx3ds_frameskip_host_tests.c — the FRAMESKIP governor (gdx3ds_frameskip_gov.c)
 * against a simulated console: each host iteration costs `logic` of clock, a RENDERED one
 * costs `render` on top (rounded up to the vblank grid when vblank-paced), and a skip's
 * sleep advances the clock by what the governor asked for. The gates are the two numbers the
 * lever exists for -- logic ticks per wall second (must reach ~59.94 when the console can
 * afford it, and must never exceed it) and the skip cap -- plus the fixed mode's phase and
 * the oracle veto. Clock unit: microseconds. */
#include <stdio.h>
#include <string.h>

#include "gdx3ds_frameskip.h"

#define INTERVAL_US 16683u /* 1001/60000 s */
#define VBLANK_US 16714u   /* the 3DS LCD, ~59.83 Hz: a hair SLOWER than the logic target */

static int sFailures = 0;

static void Check(int ok, const char* what) {
    if (!ok) {
        sFailures++;
        printf("FAIL: %s\n", what);
    }
}

typedef struct Plant {
    unsigned logicUs;
    unsigned renderUs;   /* 0 with vblankPaced: the render is exactly one vblank */
    int vblankPaced;     /* rendered frames end on the next vblank edge */
    unsigned jitterUs;   /* alternating +/- on the render cost */
} Plant;

typedef struct Result {
    unsigned long ticks;
    unsigned long rendered;
    unsigned long skipped;
    unsigned long vetoed;
    int maxRun;
    uint64_t wallUs;
    int phaseOk; /* FIXED: every rendered frame had frame % (N+1) == 0 or was vetoed */
} Result;

static uint64_t NextVblank(uint64_t t) {
    return ((t / VBLANK_US) + 1u) * VBLANK_US;
}

static uint64_t RenderEnd(const Plant* p, uint64_t t, unsigned long frame) {
    if (p->vblankPaced) {
        return NextVblank(t);
    }
    {
        long cost = (long)p->renderUs;
        if (p->jitterUs != 0) {
            cost += (frame & 1u) ? (long)p->jitterUs : -(long)p->jitterUs;
        }
        return t + (uint64_t)cost;
    }
}

/* veto(frame) -> 1 forces a render (the oracle's fh_every sample / pending SHOT). */
static Result Simulate(int mode, int maxSkip, const Plant* p, unsigned long frames, int vetoEvery) {
    GdxFrameskipGov gov;
    Result r;
    uint64_t t = 1000000u;
    const uint64_t t0 = t;
    int run = 0;
    memset(&r, 0, sizeof(r));
    r.phaseOk = 1;
    gdx_frameskip_gov_init(&gov, mode, maxSkip, INTERVAL_US);
    for (unsigned long frame = 1; frame <= frames; frame++) {
        const int veto = vetoEvery > 0 && (frame % (unsigned long)vetoEvery) == 0;
        const GdxFrameskipDecision d = gdx_frameskip_gov_decide(&gov, t, frame, veto);
        t += d.sleepTicks;
        t += p->logicUs;
        r.ticks++;
        if (d.vetoed) {
            r.vetoed++;
        }
        if (d.skip) {
            r.skipped++;
            run++;
            if (run > r.maxRun) {
                r.maxRun = run;
            }
        } else {
            r.rendered++;
            run = 0;
            t = RenderEnd(p, t, frame);
            if (mode == GDX_FRAMESKIP_FIXED && (frame % (unsigned long)(maxSkip + 1)) != 0 && !veto) {
                r.phaseOk = 0;
            }
        }
    }
    r.wallUs = t - t0;
    return r;
}

static double Tps(const Result* r) {
    return (double)r->ticks * 1e6 / (double)r->wallUs;
}

static double Fps(const Result* r) {
    return (double)r->rendered * 1e6 / (double)r->wallUs;
}

static void Report(const char* name, const Result* r) {
    printf("  %-28s ticks=%lu rendered=%lu skipped=%lu veto=%lu maxrun=%d tps=%.2f fps=%.2f\n", name, r->ticks,
           r->rendered, r->skipped, r->vetoed, r->maxRun, Tps(r), Fps(r));
}


/* ---- STEADY: the divisor controller against a slot-timed plant ----
 * Each slot: the render iteration starts on vblank S, works rw, presents at
 * max(S + (D-1)P, S + rw) (the hold), is displayed at S + D*P unless the present came within the
 * GPU margin of it (a miss), then runs D-1 skip ticks; the next slot starts on the first vblank
 * at or after both S + D*P and the end of the skips. Same rules as the glue. */
#define STEADY_MARGIN_US 4000u

typedef uint64_t (*RwFn)(unsigned long slot);

typedef struct SteadyResult {
    unsigned long slots;
    unsigned long ticks;
    unsigned long misses;
    unsigned long dChanges;
    int finalD;
    unsigned long unevenAfterSettle; /* display intervals != D*P after the first 3 s */
    uint64_t wallUs;
} SteadyResult;

static SteadyResult SimulateSteady(RwFn rwOf, unsigned skipUs, unsigned long slots, int dStart) {
    GdxSteadyCtl ctl;
    SteadyResult r;
    uint64_t s = 0;
    uint64_t lastDisplay = 0;
    int lastD = 0;
    memset(&r, 0, sizeof(r));
    gdx_steady_init(&ctl, 1, 4, dStart, VBLANK_US, STEADY_MARGIN_US);
    for (unsigned long i = 0; i < slots; i++) {
        const int d = ctl.d;
        const uint64_t rw = rwOf(i);
        const uint64_t hold = s + (uint64_t)(d - 1) * VBLANK_US;
        const uint64_t present = (s + rw > hold) ? s + rw : hold;
        uint64_t display = s + (uint64_t)d * VBLANK_US;
        const int missed = present + STEADY_MARGIN_US > display;
        uint64_t end = present + (uint64_t)(d - 1) * skipUs;
        uint64_t next = display;
        if (missed) {
            display = ((present + STEADY_MARGIN_US) / VBLANK_US + 1u) * VBLANK_US;
            r.misses++;
        }
        for (int k = 0; k < d - 1; k++) {
            gdx_steady_note_skip(&ctl, skipUs);
        }
        if (gdx_steady_note_slot(&ctl, rw, missed) != GDX_STEADY_HOLD) {
            r.dChanges++;
        }
        if (lastDisplay != 0 && s > 3000000u && lastD == d && display - lastDisplay != (uint64_t)d * VBLANK_US) {
            r.unevenAfterSettle++;
        }
        lastDisplay = display;
        lastD = d;
        if (end > next) {
            next = ((end + VBLANK_US - 1u) / VBLANK_US) * VBLANK_US;
        }
        r.ticks += (unsigned long)d;
        s = next;
        r.slots++;
    }
    r.finalD = ctl.d;
    r.wallUs = s;
    return r;
}

static void ReportSteady(const char* name, const SteadyResult* r) {
    printf("  %-28s slots=%lu D=%d dchg=%lu miss=%lu uneven=%lu tps=%.2f fps=%.2f\n", name, r->slots, r->finalD,
           r->dChanges, r->misses, r->unevenAfterSettle, (double)r->ticks * 1e6 / (double)r->wallUs,
           (double)r->slots * 1e6 / (double)r->wallUs);
}

static uint32_t sLcg = 1u;
static uint32_t Lcg(void) {
    sLcg = sLcg * 1664525u + 1013904223u;
    return sLcg >> 8;
}

static uint64_t RwTrial(unsigned long i) { (void)i; return 22000u; }
static uint64_t RwRace(unsigned long i) { (void)i; return 30000u; }
static uint64_t RwHeavy(unsigned long i) { (void)i; return 45000u; }
/* The hardware GP race windows (hw-wave3 [gpu] build 26-35 ms + ~4 ms logic): 30-39 ms. */
static uint64_t RwRaceNoisy(unsigned long i) { (void)i; return 30000u + (Lcg() % 9000u); }
static uint64_t RwSpike(unsigned long i) { return (i == 400u) ? 150000u : 22000u; }
static uint64_t RwLighten(unsigned long i) { return i < 300u ? 35000u : 8000u; }
/* The lane's menus: 12 ms renders + 4 ms GPU fit 60 Hz with no headroom -- from D=2 the
 * controller must NOT step up (80% rule); from D=1 it must hold without a miss. */
static uint64_t RwMenu12(unsigned long i) { (void)i; return 12000u; }
/* A menu: light renders with a burst of load frames every ~3 s (the lane's menu log). */
static uint64_t RwMenuHitches(unsigned long i) { return ((i % 90u) == 45u || (i % 90u) == 46u) ? 120000u : 8000u; }
/* Borderline: 60% of renders fit D=2, 40% need D=3 -- must not flap. */
static uint64_t RwBorder(unsigned long i) { (void)i; return (Lcg() % 10u) < 6u ? 24000u : 31000u; }

static void SteadyTests(void) {
    {
        const SteadyResult r = SimulateSteady(RwMenu12, 800, 6000, 2);
        ReportSteady("steady / menu 12 ms from D=2", &r);
        Check(r.finalD == 2 && r.dChanges == 0 && r.misses == 0, "no step up without 20% headroom");
        const SteadyResult r1 = SimulateSteady(RwMenu12, 800, 6000, 1);
        ReportSteady("steady / menu 12 ms from D=1", &r1);
        Check(r1.finalD == 1 && r1.misses == 0, "12 ms + 4 ms GPU lands every vblank at D=1");
    }
    {
        const SteadyResult r = SimulateSteady(RwMenuHitches, 800, 9000, 2);
        ReportSteady("steady / menu + load hitches", &r);
        Check(r.dChanges <= 1 && r.finalD == 1, "periodic load hitches must not flap D (one step up, then hold)");
    }
    {
        sLcg = 11u;
        const SteadyResult r = SimulateSteady(RwBorder, 4300, 12000, 2);
        ReportSteady("steady / borderline 24|31 ms", &r);
        Check(r.dChanges <= 6, "a borderline scene settles instead of flapping (backoff)");
    }
    {
        const SteadyResult r = SimulateSteady(RwTrial, 4300, 18000, 2);
        ReportSteady("steady / time trial 22 ms", &r);
        Check(r.finalD == 2 && r.dChanges == 0 && r.misses == 0, "22 ms renders hold D=2 (30 Hz) with no miss");
        Check(r.unevenAfterSettle == 0, "steady cadence is exactly D vblanks");
    }
    {
        const SteadyResult r = SimulateSteady(RwRace, 4300, 12000, 1);
        ReportSteady("steady / race 30 ms from D=1", &r);
        Check(r.finalD == 3, "30 ms renders settle on D=3 (20 Hz)");
        Check(r.dChanges <= 2 && r.misses <= 8, "settling from D=1 is fast: a handful of misses, not a crawl");
        Check(r.unevenAfterSettle == 0, "no uneven display interval once settled");
    }
    {
        const SteadyResult r = SimulateSteady(RwHeavy, 4300, 9000, 2);
        ReportSteady("steady / heavy 45 ms", &r);
        Check(r.finalD == 4, "45 ms renders need D=4 (15 Hz)");
    }
    {
        sLcg = 7u;
        const SteadyResult r = SimulateSteady(RwRaceNoisy, 4300, 12000, 2);
        ReportSteady("steady / race 30-39 ms noisy", &r);
        Check(r.finalD == 3, "the noisy hardware race holds D=3");
        Check(r.dChanges <= 2, "no D oscillation over 10 minutes of noisy race frames");
        Check(r.unevenAfterSettle == 0, "noisy race: every frame on screen for exactly 3 vblanks");
    }
    {
        const SteadyResult r = SimulateSteady(RwSpike, 4300, 3000, 2);
        ReportSteady("steady / one 150 ms hitch", &r);
        Check(r.misses <= 2 && r.dChanges == 0, "a single hitch is a miss, not a cadence change");
    }
    {
        const SteadyResult r = SimulateSteady(RwLighten, 4300, 3000, 3);
        ReportSteady("steady / 35 -> 11 ms", &r);
        Check(r.finalD == 1, "real headroom steps back up to D=1");
        Check(r.dChanges <= 3, "stepping up is one step at a time");
    }
}

/* ---- HITCH: the glue-faithful plant with skip-tick stalls (the hardware's SD flush) ----
 * Like SimulateSteady, plus what the round-2 plant did not model: a slot whose previous slot
 * ran late STARTS late (the glue's beginMissed: more than half a vblank past its vblank, the
 * render then begins at once on the latest vblank that passed), and any skip tick can stall.
 * The controller gets the slot's worst skip tick (gdx_steady_note_slot_ex), exactly as the
 * glue passes it. skipOf(slot, k) is the cost of skip tick k after slot's present. */
typedef uint64_t (*SkipFn)(unsigned long slot, int k);

typedef struct SteadyExResult {
    SteadyResult base;
    unsigned long hardMisses;
    unsigned long upSteps;
    unsigned long downSteps;
    unsigned long slotsAtD[5];
} SteadyExResult;

static SteadyExResult SimulateSteadyEx(RwFn rwOf, SkipFn skipOf, unsigned long slots, int dStart, int robust) {
    GdxSteadyCtl ctl;
    SteadyExResult r;
    uint64_t anchor = VBLANK_US;   /* the slot's vblank */
    uint64_t prevEnd = 0;          /* when the previous slot's last skip tick finished */
    uint64_t prevSkipMax = 0;      /* the previous slot's worst skip tick (reported at this slot) */
    uint64_t lastDisplay = 0;
    int lastD = 0;
    memset(&r, 0, sizeof(r));
    gdx_steady_init(&ctl, 1, 4, dStart, VBLANK_US, STEADY_MARGIN_US);
    gdx_steady_set_robust(&ctl, robust);
    for (unsigned long i = 0; i < slots; i++) {
        const int d = ctl.d;
        const uint64_t rw = rwOf(i);
        uint64_t begin = anchor;
        int beginMissed = 0;
        if (prevEnd > anchor + VBLANK_US / 2u) {
            beginMissed = 1;
            anchor += ((prevEnd - anchor) / VBLANK_US) * VBLANK_US;
            begin = prevEnd;
        } else if (prevEnd > anchor) {
            begin = prevEnd;
        }
        const uint64_t hold = anchor + (uint64_t)(d - 1) * VBLANK_US;
        const uint64_t present = (begin + rw > hold) ? begin + rw : hold;
        uint64_t display = anchor + (uint64_t)d * VBLANK_US;
        const int missed = beginMissed || present + STEADY_MARGIN_US > display;
        uint64_t end = present;
        uint64_t skipMax = 0;
        if (missed) {
            display = ((present + STEADY_MARGIN_US) / VBLANK_US + 1u) * VBLANK_US;
            r.base.misses++;
        }
        for (int k = 0; k < d - 1; k++) {
            const uint64_t c = skipOf(i, k);
            gdx_steady_note_skip(&ctl, c);
            end += c;
            skipMax = c > skipMax ? c : skipMax;
        }
        {
            const int res = gdx_steady_note_slot_ex(&ctl, rw, missed, prevSkipMax);
            if (res != GDX_STEADY_HOLD) {
                r.base.dChanges++;
                r.upSteps += res == GDX_STEADY_UP;
                r.downSteps += res == GDX_STEADY_DOWN;
            }
            r.hardMisses += (missed && !ctl.lastExplained) ? 1u : 0u;
        }
        if (lastDisplay != 0 && anchor > 3000000u && lastD == d && !missed &&
            display - lastDisplay != (uint64_t)d * VBLANK_US) {
            r.base.unevenAfterSettle++;
        }
        lastDisplay = display;
        lastD = d;
        r.slotsAtD[d]++;
        r.base.ticks += (unsigned long)d;
        prevSkipMax = skipMax;
        prevEnd = end;
        anchor += (uint64_t)d * VBLANK_US;
        r.base.slots++;
    }
    r.base.finalD = ctl.d;
    r.base.wallUs = anchor;
    return r;
}

static void ReportSteadyEx(const char* name, const SteadyExResult* r) {
    printf("  %-34s slots=%lu D=%d dchg=%lu (up %lu down %lu) miss=%lu hard=%lu atD=%lu/%lu/%lu/%lu\n", name,
           r->base.slots, r->base.finalD, r->base.dChanges, r->upSteps, r->downSteps, r->base.misses, r->hardMisses,
           r->slotsAtD[1], r->slotsAtD[2], r->slotsAtD[3], r->slotsAtD[4]);
}

/* hw-rel2 race, the flush moved off main: renders 24-30 ms (fit D=3 at 80%). */
static uint64_t RwRaceRel2(unsigned long i) { (void)i; return 24000u + (Lcg() % 6000u); }
/* hw-rel2 as logged: rw50 ~29, rw90 ~31-33 between spikes, and the synchronous flush landing in
 * 4% of renders (rwmax 80-515 ms in almost every window). */
static uint64_t RwRaceRel2Spiky(unsigned long i) {
    const uint32_t r = Lcg() % 100u;
    (void)i;
    if (r < 4u) {
        return 80000u + (Lcg() % 435000u);
    }
    return 25000u + (Lcg() % 7000u);
}
static uint64_t SkipPlain(unsigned long slot, int k) { (void)slot; (void)k; return 3300u; }
/* hw-rel2: 8-11 SD flushes per 256 ticks at 26-270 ms each, most of them in skip ticks. */
static uint64_t SkipFlushy(unsigned long slot, int k) {
    (void)slot;
    (void)k;
    if ((Lcg() % 100u) < 4u) {
        return 26000u + (Lcg() % 244000u);
    }
    return 3300u;
}
static uint64_t SkipOneFlush(unsigned long slot, int k) { return (slot == 500u && k == 0) ? 180000u : 3300u; }
/* A scene that really gets heavier at slot 600 (27 -> 60 ms): not a spike. */
static uint64_t RwHeavierAt600(unsigned long i) { return i < 600u ? 27000u : 60000u + (Lcg() % 4000u); }
/* Every render stalls (a pathological stream of spikes): the flood rule still steps down. */
static uint64_t RwAlwaysSpiky(unsigned long i) { return (i < 200u) ? 20000u : ((i & 1u) ? 90000u : 20000u); }

static void SteadyRobustTests(void) {
    {
        sLcg = 3u;
        const SteadyExResult old = SimulateSteadyEx(RwRaceRel2Spiky, SkipFlushy, 3000, 4, 0);
        ReportSteadyEx("round-2 rules / hw-rel2 race", &old);
        sLcg = 3u;
        const SteadyExResult r = SimulateSteadyEx(RwRaceRel2Spiky, SkipFlushy, 3000, 4, 1);
        ReportSteadyEx("robust / hw-rel2 race", &r);
        /* The plant is not the console (it has no logging on main and no spike-inflated skip
         * average, which is what pinned hw-rel2 to D=4 outright), but the round-2 rules already
         * flap on it: every flushed slot that lands twice in 8 is a step down. */
        Check(old.downSteps >= 2 && old.slotsAtD[4] > 4u * r.slotsAtD[4],
              "plant check: the round-2 rules step down on flush misses (hw-rel2)");
        Check(r.base.finalD == 3, "robust: the hw-rel2 race steps up to D=3 and holds it");
        Check(r.slotsAtD[3] > r.base.slots * 3 / 4, "robust: most of the race at D=3");
        Check(r.downSteps <= 1, "robust: isolated spikes do not flap D");
    }
    {
        sLcg = 5u;
        const SteadyExResult r = SimulateSteadyEx(RwRaceRel2, SkipFlushy, 3000, 4, 1);
        ReportSteadyEx("robust / race, flushes in skips only", &r);
        Check(r.base.finalD == 3 && r.downSteps == 0, "robust: stalled skips never step D down");
    }
    {
        sLcg = 9u;
        const SteadyExResult r = SimulateSteadyEx(RwRaceRel2, SkipOneFlush, 1500, 3, 1);
        ReportSteadyEx("robust / one 180 ms stalled skip", &r);
        Check(r.base.dChanges == 0 && r.base.misses <= 2, "one stalled skip is a miss, not a cadence change");
        Check(r.hardMisses == 0, "the stall explains its miss");
    }
    {
        sLcg = 13u;
        const SteadyExResult r = SimulateSteadyEx(RwHeavierAt600, SkipPlain, 1200, 3, 1);
        ReportSteadyEx("robust / 27 -> 60 ms scene", &r);
        Check(r.base.finalD == 4, "a heavier scene is not a spike: it steps down");
        Check(r.downSteps >= 1 && r.slotsAtD[3] <= 600u + 8u, "the step down lands within a few slots");
    }
    {
        sLcg = 17u;
        const SteadyExResult r = SimulateSteadyEx(RwAlwaysSpiky, SkipPlain, 800, 2, 1);
        ReportSteadyEx("robust / every other render spikes", &r);
        Check(r.base.finalD >= 3, "a stream of spikes is load: the flood rule steps down");
    }
    {
        sLcg = 7u;
        const SteadyExResult r = SimulateSteadyEx(RwRaceNoisy, SkipPlain, 6000, 2, 1);
        ReportSteadyEx("robust / race 30-39 ms noisy", &r);
        Check(r.base.finalD == 3 && r.base.dChanges <= 2, "robust keeps the round-2 noisy-race behaviour");
        Check(r.base.unevenAfterSettle == 0, "robust: every frame on screen for exactly D vblanks");
    }
}

/* ---- LIVE MODE SWITCH (the DBG-tab FRAMESKIP entry): gdx_frameskip_live_switch ---- */

/* Drive `n` decisions one logic tick apart (on schedule), returning the skips seen. */
static int DecideOnSchedule(GdxFrameskipGov* gov, uint64_t* t, unsigned long* frame, int n, int* firstSkip) {
    int skips = 0;
    for (int i = 0; i < n; i++) {
        const GdxFrameskipDecision d = gdx_frameskip_gov_decide(gov, *t, (*frame)++, 0);
        if (i == 0 && firstSkip != NULL) {
            *firstSkip = d.skip;
        }
        skips += d.skip;
        *t += INTERVAL_US;
    }
    return skips;
}

/* RACE0 (n60fs): run `ticks` steps of one scope with one sample per tick (0 = no render);
 * returns the number of switches and the last non-HOLD result / reason. */
typedef struct Race0Run {
    int switches;
    int lastResult;
    int lastWhy;
} Race0Run;

static Race0Run Race0Feed(GdxRace0Ctl* ctl, int scope, unsigned sampleUs, int ticks) {
    Race0Run run = { 0, GDX_RACE0_HOLD, GDX_RACE0_WHY_NONE };
    for (int i = 0; i < ticks; i++) {
        int why = 0;
        const int r = gdx_race0_step(ctl, scope, sampleUs, &why);
        if (r != GDX_RACE0_HOLD) {
            run.switches++;
            run.lastResult = r;
            run.lastWhy = why;
        }
    }
    return run;
}

static void Race0ScopeTests(void) {
    GdxRace0Ctl c;
    Race0Run r;
    gdx_race0_init(&c, 8, 33000u, 25000u);
    r = Race0Feed(&c, 0, 45000u, 600);
    Check(r.switches == 0 && !c.racing, "race0: menus never switch (boot is the other mode)");
    r = Race0Feed(&c, 1, 17000u, 7);
    Check(r.switches == 0, "race0: 7 in-scope ticks do not flip the scope yet");
    r = Race0Feed(&c, 1, 17000u, 1);
    Check(r.switches == 1 && r.lastResult == GDX_RACE0_TO_EVERY && r.lastWhy == GDX_RACE0_WHY_SCOPE_ENTER,
          "race0: the 8th in-scope tick enters every-tick rendering (race-scope-enter)");
    for (int blip = 0; blip < 20; blip++) {
        r = Race0Feed(&c, 0, 17000u, 7);
        Check(r.switches == 0, "race0: a 7-tick scope blip does not flap");
        r = Race0Feed(&c, 1, 17000u, 30);
        Check(r.switches == 0, "race0: back in scope after a blip, no switch");
    }
    r = Race0Feed(&c, 0, 0u, 8);
    Check(r.switches == 1 && r.lastResult == GDX_RACE0_TO_OTHER && r.lastWhy == GDX_RACE0_WHY_SCOPE_EXIT,
          "race0: 8 out-of-scope ticks leave (race-scope-exit)");
}

static void Race0GuardTests(void) {
    GdxRace0Ctl c;
    Race0Run r;
    gdx_race0_init(&c, 8, 33000u, 25000u);
    Race0Feed(&c, 1, 20000u, 8);
    Check(c.racing, "race0 guard: in race");
    /* GP at 45-50 fps plus a hitch train: 3 of every 16 frames at 120-235 ms. */
    for (int i = 0; i < 2000; i++) {
        int why = 0;
        const unsigned us = (i % 16) < 3 ? 150000u : 21000u;
        const int res = gdx_race0_step(&c, 1, us, &why);
        Check(res == GDX_RACE0_HOLD, "race0 guard: hitches never trip the guard");
        if (res != GDX_RACE0_HOLD) {
            break;
        }
    }
    r = Race0Feed(&c, 1, 28000u, 600);
    Check(r.switches == 0 && c.racing, "race0 guard: a 28 ms race (inside the band) stays every-tick");
    r = Race0Feed(&c, 1, 47000u, 11);
    Check(r.switches == 0, "race0 guard: 11 heavy frames of the last 16 are not enough");
    r = Race0Feed(&c, 1, 47000u, 1);
    Check(r.switches == 1 && r.lastResult == GDX_RACE0_TO_OTHER && r.lastWhy == GDX_RACE0_WHY_HEAVY,
          "race0 guard: the 12th heavy frame of 16 (a 47 ms results overlay) leaves every-tick");
    Check(c.guardPct == 47000u, "race0 guard: the receipt keeps the p25 that tripped it");
    r = Race0Feed(&c, 1, 28000u, 600);
    Check(r.switches == 0 && !c.racing, "race0 guard: inside the band the tripped guard holds");
    r = Race0Feed(&c, 1, 18000u, 16);
    Check(r.switches == 1 && r.lastResult == GDX_RACE0_TO_EVERY && r.lastWhy == GDX_RACE0_WHY_LIGHT,
          "race0 guard: 16 light frames come back (light-frames)");
    Race0Feed(&c, 1, 47000u, 16);
    Check(!c.racing, "race0 guard: tripped again");
    r = Race0Feed(&c, 0, 47000u, 8);
    Check(r.switches == 0 && !c.racing, "race0 guard: leaving scope while tripped is no switch");
    r = Race0Feed(&c, 1, 47000u, 8);
    Check(r.switches == 1 && c.racing && r.lastWhy == GDX_RACE0_WHY_SCOPE_ENTER,
          "race0 guard: a new race starts every-tick (the guard resets on scope change)");
    r = Race0Feed(&c, 0, 0u, 8);
    Check(r.switches == 1 && !c.racing, "race0 guard: out");

    gdx_race0_init(&c, 8, 0u, 0u);
    Race0Feed(&c, 1, 20000u, 8);
    r = Race0Feed(&c, 1, 90000u, 2000);
    Check(r.switches == 0 && c.racing, "race0 guard off (heavy_ms=0): pure scope, never trips");
}

/* n60stereo: the 3D card flapped 25 times (heavy 33 ms every-tick <-> light 20 ms under auto). */
static void Race0DwellTests(void) {
    GdxRace0Ctl c;
    Race0Run r;
    gdx_race0_init(&c, 8, 33000u, 25000u);
    Race0Feed(&c, 1, 20000u, 8);
    r = Race0Feed(&c, 1, 33400u, 4000);
    Check(r.switches == 1 && !c.racing, "race0 no dwell: trips once on a 33 ms race");
    r = Race0Feed(&c, 1, 20000u, 16);
    Check(r.switches == 1 && c.racing, "race0 no dwell: comes back after 16 light frames (the flap)");
    Race0Feed(&c, 0, 0u, 8);

    gdx_race0_init(&c, 8, 33000u, 25000u);
    gdx_race0_set_dwell(&c, 600);
    Race0Feed(&c, 1, 20000u, 8);
    int flaps = 0;
    for (int cycle = 0; cycle < 40; cycle++) {
        r = Race0Feed(&c, 1, c.racing ? 33400u : 20000u, 120);
        flaps += r.switches;
    }
    Check(flaps <= 14, "race0 dwell 600: a 4800-tick alternating race switches at most 14 times (was 40)");
    gdx_race0_init(&c, 8, 33000u, 25000u);
    gdx_race0_set_dwell(&c, 600);
    Race0Feed(&c, 1, 20000u, 8);
    Race0Feed(&c, 1, 33400u, 16);
    Check(!c.racing && gdx_race0_dwell_now(&c) == 600, "race0 dwell: first trip holds 600");
    r = Race0Feed(&c, 1, 20000u, 590);
    Check(r.switches == 0 && !c.racing, "race0 dwell: light frames inside the dwell do not release");
    r = Race0Feed(&c, 1, 20000u, 30);
    Check(r.switches == 1 && c.racing, "race0 dwell: released once the dwell passed");
    Race0Feed(&c, 1, 33400u, 16);
    Check(!c.racing && gdx_race0_dwell_now(&c) == 600 && c.trips == 2, "race0 dwell: every trip holds the same");
    Race0Feed(&c, 0, 0u, 8);
    Race0Feed(&c, 1, 20000u, 8);
    Race0Feed(&c, 1, 33400u, 16);
    Check(gdx_race0_dwell_now(&c) == 600 && c.trips == 1, "race0 dwell: a new race counts trips from 1");
    /* race0_arm: no samples during the intro (the caller passes 0) -> a heavy intro never trips. */
    gdx_race0_init(&c, 8, 33000u, 25000u);
    gdx_race0_set_dwell(&c, 600);
    Race0Feed(&c, 1, 0u, 460);
    Check(c.racing && c.rwN == 0, "race0 arm: an unsampled 460-tick intro keeps every-tick rendering");
    r = Race0Feed(&c, 1, 15000u, 600);
    Check(r.switches == 0 && c.racing, "race0 arm: a 15 ms race after the intro never trips");
}

/* n60results race0_post_finish: the finish leaves the scope at once, never trips, cannot re-enter. */
static void Race0PostFinishTests(void) {
    GdxRace0Ctl c;
    int why = 0;
    int r;
    gdx_race0_init(&c, 8, 33000u, 25000u);
    gdx_race0_set_dwell(&c, 600);
    Race0Feed(&c, 1, 15000u, 64);
    Check(c.racing, "race0 post-finish: racing before the line");
    r = gdx_race0_step_post(&c, 1, 1, 15000u, &why);
    Check(r == GDX_RACE0_TO_OTHER && why == GDX_RACE0_WHY_POST_FINISH && !c.racing && !c.scope,
          "race0 post-finish: the first finished tick leaves (no debounce), reason post-finish");
    for (int i = 0; i < 700; i++) {
        r = gdx_race0_step_post(&c, 1, 1, 47000u, &why);
        Check(r == GDX_RACE0_HOLD, "race0 post-finish: a heavy results screen never switches");
    }
    Check(c.trips == 0 && c.rwN == 0 && !c.heavy, "race0 post-finish: no samples, no trip, no dwell");
    r = gdx_race0_step_post(&c, 0, 0, 0u, &why);
    Check(r == GDX_RACE0_HOLD, "race0 post-finish: the mode change after it is no switch");
    Race0Feed(&c, 1, 15000u, 8);
    Check(c.racing, "race0 post-finish: the next race enters as usual (race-scope-enter)");
    r = gdx_race0_step_post(&c, 0, 1, 0u, &why);
    Check(r == GDX_RACE0_TO_OTHER && why == GDX_RACE0_WHY_POST_FINISH, "race0 post-finish: wins over the debounce");
    Check(strcmp(gdx_race0_why_name(GDX_RACE0_WHY_POST_FINISH), "post-finish") == 0, "race0 post-finish: name");
}

static void LiveSwitchTests(void) {
    GdxFrameskipGov gov;
    GdxFrameskipSlot slot;
    uint64_t t = 1000000u;
    unsigned long frame = 1;
    int firstSkip = -1;

    /* steady D=3 mid-slot: one render + one of its two skips done, a held present pending. */
    gdx_frameskip_gov_init(&gov, GDX_FRAMESKIP_STEADY, 2, INTERVAL_US);
    gdx_frameskip_slot_reset(&slot, 3);
    DecideOnSchedule(&gov, &t, &frame, 4, NULL);
    slot.vblank = t;
    slot.pacedRender = 1;
    slot.catchup = 2;
    slot.lastWasSkip = 1;
    Check(gov.skipRun == 1, "setup: steady is mid-slot (one skip of two done)");
    gdx_frameskip_live_switch(&gov, &slot, GDX_FRAMESKIP_AUTO, 5);
    Check(slot.pacedRender == 0, "switch mid-slot leaves no held present pending");
    Check(slot.vblank == 0 && slot.catchup == 0 && slot.lastWasSkip == 0, "switch un-anchors the slot");
    Check(gov.mode == GDX_FRAMESKIP_AUTO && gov.maxSkip == 5 && gov.interval == INTERVAL_US,
          "switch keeps the tick interval and takes the new mode + cap");
    {
        /* Long after the old schedule (a menu pause): the new mode inherits no debt. */
        const GdxFrameskipDecision d = gdx_frameskip_gov_decide(&gov, t + 5000000u, frame++, 0);
        Check(d.skip == 0 && d.lag == 0 && d.sleepTicks == 0, "first decision after switch renders, no lag");
    }

    /* auto deep in a skip run (behind schedule) -> steady D=2: renders, then exactly D-1 skips. */
    gdx_frameskip_gov_init(&gov, GDX_FRAMESKIP_AUTO, 5, INTERVAL_US);
    t = 1000000u;
    for (int i = 0; i < 20; i++) {
        (void)gdx_frameskip_gov_decide(&gov, t, frame++, 0);
        t += 70000u; /* slow scene: auto is skipping */
    }
    Check(gov.skipRun > 0, "setup: auto is mid skip-run");
    gdx_frameskip_live_switch(&gov, &slot, GDX_FRAMESKIP_STEADY, 1);
    Check(slot.d == 2 && slot.pacedRender == 0 && slot.vblank == 0, "switch into steady: slot at D, unanchored");
    {
        int pattern = 0;
        for (int i = 0; i < 6; i++) {
            const GdxFrameskipDecision d = gdx_frameskip_gov_decide(&gov, t, frame++, 0);
            pattern = (pattern << 1) | d.skip;
            t += INTERVAL_US;
        }
        Check(pattern == 0x15, "steady D=2 after switch: render, skip, render, skip, render, skip");
    }

    /* steady mid-slot -> off: never skips again, whatever the schedule. */
    gdx_frameskip_live_switch(&gov, &slot, GDX_FRAMESKIP_OFF, 0);
    Check(DecideOnSchedule(&gov, &t, &frame, 50, NULL) == 0, "switch to off never skips");

    /* off -> auto in a fast scene: the fresh schedule does not skip. */
    gdx_frameskip_live_switch(&gov, &slot, GDX_FRAMESKIP_AUTO, 5);
    Check(DecideOnSchedule(&gov, &t, &frame, 600, &firstSkip) == 0 && firstSkip == 0,
          "auto after a switch does not skip an on-schedule scene");

    /* the cap binds right after a switch: a hopeless scene runs at most maxSkip in a row. */
    gdx_frameskip_live_switch(&gov, &slot, GDX_FRAMESKIP_AUTO, 3);
    {
        int run = 0;
        int maxRun = 0;
        for (int i = 0; i < 100; i++) {
            const GdxFrameskipDecision d = gdx_frameskip_gov_decide(&gov, t, frame++, 0);
            run = d.skip ? run + 1 : 0;
            maxRun = run > maxRun ? run : maxRun;
            t += 150000u;
        }
        Check(maxRun == 3, "auto cap holds across a switch");
    }
    printf("  live switch                  done\n");
}

/* NETTICK (hw 158a13e): a lockstep host waits on the peer's pads 2-20 ms a frame. A slot start the
 * wait alone made late is not a miss (gdx_steady_late_by_net). Counted as one, it never lets a
 * light render step back up: steady entered at D=2 (a race0 heavy trip) stays at 30 fps.
 * Clock unit: microseconds. */
static void SteadyNetLateTests(void) {
    const uint64_t p = VBLANK_US;
    Check(!gdx_steady_late_by_net(20000u, 0u, p), "net-late: no wait explains nothing");
    Check(gdx_steady_late_by_net(20000u, 18000u, p), "net-late: an 18 ms wait explains a 20 ms late start");
    Check(gdx_steady_late_by_net(15000u, 20000u, p), "net-late: a wait longer than the lateness explains it");
    Check(!gdx_steady_late_by_net(30000u, 5000u, p), "net-late: 25 ms of own lateness is a miss");
    Check(gdx_steady_late_by_net(9000u, 1000u, p), "net-late: within half a vblank once the wait is out");
    {
        GdxSteadyCtl oldRule;
        GdxSteadyCtl newRule;
        int i;
        int oldMinD = 2;
        int newMinD = 2;
        gdx_steady_init(&oldRule, 1, 4, 2, p, STEADY_MARGIN_US);
        gdx_steady_init(&newRule, 1, 4, 2, p, STEADY_MARGIN_US);
        for (i = 0; i < 1200; i++) {
            /* one slot in three starts ~20 ms late: an 18 ms pad wait plus 2 ms of our own */
            const int late = (i % 3) == 0;
            const uint64_t rw = 5000u;
            const int newMissed = late && !gdx_steady_late_by_net(20000u, 18000u, p);
            gdx_steady_note_slot(&oldRule, rw, late);
            gdx_steady_note_slot(&newRule, rw, newMissed);
            if (oldRule.d < oldMinD) {
                oldMinD = oldRule.d;
            }
            if (newRule.d < newMinD) {
                newMinD = newRule.d;
            }
        }
        printf("net-late: light render at D=2, 1 slot in 3 held by pads: old rule final D=%d (min %d), "
               "new rule final D=%d (min %d)\n", oldRule.d, oldMinD, newRule.d, newMinD);
        Check(oldMinD == 2, "plant check: counted as misses, pad-wait late starts pin D=2 (30 fps)");
        Check(newRule.d == 1, "net-late: not counted, a light render steps back up to D=1 (60 fps)");
    }
}

/* o3dsperf (hw 158a13e): an Old 3DS MP race at D=3 whose render dropped to ~24 ms (+3.5 ms skip
 * ticks) fits the 2-tick slot (24 + 4 ms margin <= 33.4) but not at the stock step-up rule's 80%
 * (26.7 ms): it stays at 20 fps. [debug] steady_lockstep_up_pct 90 (30.0 ms) lets it step up, and
 * the step-down rule (p75 must miss the slot at 100%) does not bounce it back. Clock: us. */
static int RunUpPct(unsigned pct, uint64_t rw, int* minD) {
    GdxSteadyCtl ctl;
    const uint64_t p = VBLANK_US;
    int i;
    gdx_steady_init(&ctl, 1, 3, 3, p, STEADY_MARGIN_US);
    gdx_steady_set_up_pct(&ctl, pct);
    *minD = ctl.d;
    for (i = 0; i < 2000; i++) {
        const int d = ctl.d;
        const uint64_t skips = (uint64_t)(d - 1) * 3500u;
        const uint64_t tail = skips > STEADY_MARGIN_US ? skips : STEADY_MARGIN_US;
        const int missed = rw + tail > (uint64_t)d * p;
        int k;
        for (k = 0; k < d - 1; k++) {
            gdx_steady_note_skip(&ctl, 3500u);
        }
        gdx_steady_note_slot(&ctl, rw, missed);
        if (ctl.d < *minD) {
            *minD = ctl.d;
        }
    }
    return ctl.d;
}

static void SteadyUpPctTests(void) {
    int minD80 = 0;
    int minD90 = 0;
    int minDHeavy = 0;
    const int d80 = RunUpPct(80u, 24000u, &minD80);
    const int d90 = RunUpPct(90u, 24000u, &minD90);
    const int dHeavy = RunUpPct(90u, 31000u, &minDHeavy);
    printf("up-pct: 24 ms render from D=3: 80%% -> D=%d (min %d), 90%% -> D=%d (min %d); 31 ms at 90%% -> D=%d\n", d80,
           minD80, d90, minD90, dHeavy);
    Check(d80 == 3, "up-pct: the stock 80% rule keeps a 24 ms render at D=3");
    Check(d90 == 2 && minD90 == 2, "up-pct: 90% steps a 24 ms render up to D=2 and holds it there");
    Check(dHeavy == 3 && minDHeavy == 3, "up-pct: a 31 ms render does not step up at 90%");
}

/* o3ds60 steady_d1_hold_pct (hw a3b4531: every D = 1 stint ended after 3 misses in 4 slots): at
 * D = 1, 3 misses in a row out of every 16 slots (81% hit) step the stock rules down to D = 2;
 * with the hold at 80% D = 1 stays. Every slot missed still steps down. Clock: us. */
static int RunD1Hold(unsigned pct, unsigned missPer16, unsigned long* holds) {
    GdxSteadyCtl ctl;
    const uint64_t p = VBLANK_US;
    unsigned i;
    gdx_steady_init(&ctl, 1, 4, 1, p, STEADY_MARGIN_US);
    gdx_steady_set_d1_hold_pct(&ctl, pct);
    for (i = 0; i < 64u && ctl.d == 1; i++) {
        const int missed = (i % 16u) < missPer16;
        (void) gdx_steady_note_slot(&ctl, missed ? 16000u : 9000u, missed);
    }
    *holds = ctl.d1Holds;
    return ctl.d;
}

static void SteadyD1HoldTests(void) {
    unsigned long h0 = 0;
    unsigned long h80 = 0;
    unsigned long hBad = 0;
    const int dOff = RunD1Hold(0u, 3u, &h0);
    const int dHold = RunD1Hold(80u, 3u, &h80);
    const int dBad = RunD1Hold(80u, 16u, &hBad);
    printf("d1-hold: 3 misses / 16 slots: off -> D=%d, 80%% -> D=%d (holds %lu); every slot missed at 80%% -> D=%d\n",
           dOff, dHold, h80, dBad);
    Check(dOff == 2, "d1-hold: off, 3 misses in a row at D=1 step down (the stock rule)");
    Check(dHold == 1 && h80 > 0, "d1-hold: at 80% an 81% hit rate stays at D=1");
    Check(dBad == 2, "d1-hold: at 80% a 0% hit rate still steps down");
}

/* o3ds60 steady_d1_hold_slots (hw 9088d3a, 3D: stints at 93-97% hit left D = 1 on 4 misses in a
 * row): 4 misses in a row (18 ms renders) out of every 64 slots. 80% over 16 slots steps down;
 * 85% over 64 holds D = 1 for 256 slots. Every slot missed at 85/64 still steps down, within 12. */
static int RunD1Window(unsigned pct, unsigned slots, unsigned missPer64, unsigned total, unsigned* atSlot) {
    GdxSteadyCtl ctl;
    unsigned i;
    gdx_steady_init(&ctl, 1, 4, 1, VBLANK_US, STEADY_MARGIN_US);
    gdx_steady_set_d1_hold_pct(&ctl, pct);
    gdx_steady_set_d1_hold_slots(&ctl, slots);
    *atSlot = total;
    for (i = 0; i < total && ctl.d == 1; i++) {
        const int missed = ((i + 40u) % 64u) < missPer64;
        (void) gdx_steady_note_slot(&ctl, missed ? 18000u : 11000u, missed);
        if (ctl.d != 1) {
            *atSlot = i + 1u;
        }
    }
    return ctl.d;
}

/* o3ds60 steady_d1_retry: at D = 2 with an 11 ms render (stock: p90 + 4 ms margin must fit 80% of
 * D = 1, never), a 120-tick retry at 85% steps up to D = 1 within ~2 s of ticks. */
static int RunD1Retry(unsigned long retryTicks, unsigned long* ticksToUp) {
    GdxSteadyCtl ctl;
    unsigned long ticks = 0;
    gdx_steady_init(&ctl, 1, 4, 2, VBLANK_US, STEADY_MARGIN_US);
    gdx_steady_set_d1_retry(&ctl, retryTicks, 85u);
    *ticksToUp = 0;
    while (ticks < 3600u && ctl.d == 2) {
        gdx_steady_note_skip(&ctl, 3000u);
        (void) gdx_steady_note_slot(&ctl, 11000u, 0);
        ticks += 2u;
    }
    *ticksToUp = ticks;
    return ctl.d;
}

static void SteadyD1WindowTests(void) {
    unsigned atOld = 0;
    unsigned atNew = 0;
    unsigned atBad = 0;
    unsigned long upStock = 0;
    unsigned long upRetry = 0;
    const int dOld = RunD1Window(80u, 16u, 4u, 256u, &atOld);
    const int dNew = RunD1Window(85u, 64u, 4u, 256u, &atNew);
    const int dBad = RunD1Window(85u, 64u, 64u, 256u, &atBad);
    const int dStock = RunD1Retry(0u, &upStock);
    const int dRetry = RunD1Retry(120u, &upRetry);
    printf("d1-window: 4-miss bursts per 64 slots: 80/16 -> D=%d (slot %u), 85/64 -> D=%d; all missed at 85/64 -> "
           "D=%d at slot %u\n", dOld, atOld, dNew, dBad, atBad);
    printf("d1-retry: 11 ms renders at D=2: stock -> D=%d after %lu ticks, retry 120 -> D=%d after %lu ticks\n", dStock,
           upStock, dRetry, upRetry);
    Check(dOld == 2, "d1-window: 80% over 16 slots leaves D=1 on a 4-miss burst (the 9088d3a rule)");
    Check(dNew == 1, "d1-window: 85% over 64 slots holds D=1 through 4-miss bursts (94% hit)");
    Check(dBad == 2 && atBad <= 12u, "d1-window: every slot missed still steps down within 12 slots");
    Check(dStock == 2, "d1-retry: the stock up rule keeps an 11 ms render at D=2");
    Check(dRetry == 1 && upRetry <= 260u, "d1-retry: the retry steps an 11 ms render up to D=1 within ~2 s");
}

/* o3ds-mp16 steady_d1_drift (hw card-3, Old 3DS 2D 16-racer VS): D = 1 slots that cost a little over
 * a vblank (15.7 ms render + 2.2 ms post-present tail = 17.9 ms) drift: each start is 1.2 ms later
 * than the last. The start rule (vblank passed by over half a period) calls ~half the slots missed
 * and steps down to 30 Hz; only ~1 vblank in 14 actually repeats a frame. Simulated with the slot
 * clock frameskip.c keeps (a late start keeps its vblank unless a whole period passed). */
typedef struct DriftRun {
    int d;
    unsigned slots;
    unsigned oldMissed;
    unsigned repeats;
} DriftRun;

static DriftRun RunD1Drift(int driftRule, unsigned holdPct, uint64_t rw, uint64_t post, unsigned total) {
    GdxSteadyCtl ctl;
    DriftRun r = { 1, 0, 0, 0 };
    const uint64_t p = VBLANK_US;
    uint64_t vblank = 100000u;
    uint64_t t = vblank;
    uint64_t prevDisplay = 0;
    unsigned i;
    gdx_steady_init(&ctl, 1, 3, 1, p, STEADY_MARGIN_US);
    gdx_steady_set_d1_hold_pct(&ctl, holdPct);
    gdx_steady_set_d1_hold_slots(&ctl, 64u);
    for (i = 0; i < total && ctl.d == 1; i++) {
        const uint64_t target = vblank + p;
        uint64_t start = t < target ? target : t;
        int beginMissed = 0;
        int repeat;
        uint64_t display;
        if (start > target + p / 2u) {
            beginMissed = 1;
            vblank = target + ((start - target) / p) * p;
        } else {
            vblank = target;
        }
        display = gdx_steady_d1_display(vblank, start + rw, STEADY_MARGIN_US, p);
        repeat = gdx_steady_d1_repeats(prevDisplay, display, p);
        prevDisplay = display;
        r.oldMissed += beginMissed ? 1u : 0u;
        r.repeats += repeat ? 1u : 0u;
        (void) gdx_steady_note_slot(&ctl, rw, driftRule ? (beginMissed && repeat) : beginMissed);
        t = start + rw + post;
        r.slots++;
    }
    r.d = ctl.d;
    return r;
}

static void SteadyD1DriftTests(void) {
    const DriftRun oldRule = RunD1Drift(0, 85u, 15700u, 2200u, 640u);
    const DriftRun drift = RunD1Drift(1, 75u, 15700u, 2200u, 640u);
    const DriftRun fits = RunD1Drift(1, 75u, 12000u, 2000u, 640u);
    const DriftRun heavy = RunD1Drift(1, 75u, 24000u, 2500u, 640u);
    uint64_t d0;
    printf("d1-drift: 17.9 ms slots: start rule -> D=%d after %u slots (%u missed); drift rule -> D=%d after %u slots "
           "(%u repeats, start rule would say %u); 14 ms -> D=%d (%u repeats); 26.5 ms -> D=%d after %u slots\n",
           oldRule.d, oldRule.slots, oldRule.oldMissed, drift.d, drift.slots, drift.repeats, drift.oldMissed, fits.d,
           fits.repeats, heavy.d, heavy.slots);
    Check(oldRule.d == 2 && oldRule.slots < 64u, "d1-drift: the start rule steps a drifting 17.9 ms D=1 down to 30 Hz");
    Check(drift.d == 1 && drift.slots == 640u, "d1-drift: the drift rule holds D=1 for 640 slots of 17.9 ms");
    Check(drift.repeats >= 30u && drift.repeats <= 55u, "d1-drift: ~1 repeated frame per 1.2 ms / 16.7 ms of overrun");
    Check(drift.oldMissed > 4u * drift.repeats, "d1-drift: the start rule over-counts a drift several times over");
    Check(fits.d == 1 && fits.repeats == 0u, "d1-drift: slots inside a vblank never repeat");
    Check(heavy.d == 2, "d1-drift: 26.5 ms slots (a repeat every ~2nd vblank) still step down");
    d0 = gdx_steady_d1_display(1000u, 1000u + 5000u, STEADY_MARGIN_US, VBLANK_US);
    Check(d0 == 1000u + VBLANK_US, "d1-drift: a present with margin to spare reaches the next vblank");
    Check(gdx_steady_d1_display(1000u, 1000u + 14000u, STEADY_MARGIN_US, VBLANK_US) == 1000u + 2u * VBLANK_US,
          "d1-drift: a present inside the margin reaches the vblank after");
    Check(!gdx_steady_d1_repeats(0u, d0, VBLANK_US), "d1-drift: no previous slot, no repeat");
    Check(!gdx_steady_d1_repeats(d0, d0 + VBLANK_US, VBLANK_US), "d1-drift: consecutive vblanks are not a repeat");
    Check(gdx_steady_d1_repeats(d0, d0 + 2u * VBLANK_US, VBLANK_US), "d1-drift: a skipped vblank is a repeat");
}

/* o3ds3d stereo_mp_lock30: the Old 3DS rules (85/64 hold, 120-tick retry at 85%) with the floor pinned
 * at 2. A 6 ms render (fits D = 1 by every rule) never steps up while pinned; a pin at D = 1 lifts D
 * to 2 at once; the unpin hands D back to the retry, which steps up within ~2 s. */
static void SteadyLock30Tests(void) {
    GdxSteadyCtl ctl;
    unsigned long ticks = 0;
    unsigned long ticksToUp = 0;
    unsigned long retriesPinned;
    int dPinned;
    int lifted;
    gdx_steady_init(&ctl, 1, 3, 1, VBLANK_US, STEADY_MARGIN_US);
    gdx_steady_set_d1_hold_pct(&ctl, 85u);
    gdx_steady_set_d1_hold_slots(&ctl, 64u);
    gdx_steady_set_d1_retry(&ctl, 120u, 85u);
    gdx_steady_pin_min(&ctl, 2);
    lifted = ctl.d;
    while (ticks < 3600u) {
        gdx_steady_note_skip(&ctl, 3000u);
        (void) gdx_steady_note_slot(&ctl, 6000u, 0);
        ticks += 2u;
    }
    dPinned = ctl.d;
    retriesPinned = ctl.d1Retries;
    gdx_steady_pin_min(&ctl, 1);
    ticks = 0;
    while (ticks < 3600u && ctl.d == 2) {
        gdx_steady_note_skip(&ctl, 3000u);
        (void) gdx_steady_note_slot(&ctl, 6000u, 0);
        ticks += 2u;
    }
    ticksToUp = ticks;
    printf("lock30: pin at D=1 -> D=%d; 6 ms renders pinned 3600 ticks -> D=%d retries=%lu; unpinned -> D=%d after "
           "%lu ticks\n", lifted, dPinned, retriesPinned, ctl.d, ticksToUp);
    Check(lifted == 2, "lock30: the pin lifts D = 1 to 2 at once");
    Check(dPinned == 2 && retriesPinned == 0u, "lock30: pinned, no step up and no D = 1 retry in 3600 ticks");
    Check(ctl.d == 1 && ticksToUp <= 260u, "lock30: unpinned, the retry steps up to D = 1 within ~2 s");
}

int main(void) {
    const Plant fast = {1000, 0, 1, 0};           /* a menu: renders inside one vblank */
    const Plant o3dsRace = {4000, 63800, 0, 0};    /* hardware GP race: ~4 ms logic, 67.8 ms/frame */
    const Plant o3dsTrial = {4000, 36000, 0, 0};   /* hardware time trial: 40 ms/frame */
    const Plant hopeless = {4000, 150000, 0, 0};   /* cannot keep up even at the cap */
    const Plant jittery = {1000, 15000, 0, 1500};  /* 13.5/16.5 ms alternating, under budget */

    printf("gdx3ds_frameskip_host_tests\n");

    {
        const Result r = Simulate(GDX_FRAMESKIP_OFF, 4, &o3dsRace, 600, 0);
        Report("off / race", &r);
        Check(r.skipped == 0, "off must never skip");
    }
    {
        const Result r = Simulate(GDX_FRAMESKIP_AUTO, 4, &fast, 36000, 0);
        Report("auto / fast (10 min)", &r);
        Check(r.skipped == 0, "auto must not skip a scene that renders every vblank (the 59.83 vs 59.94 drift)");
    }
    {
        const Result r = Simulate(GDX_FRAMESKIP_AUTO, 4, &jittery, 36000, 0);
        Report("auto / jittery under budget", &r);
        Check(r.skipped == 0, "auto must not skip for sub-tick jitter");
    }
    {
        const Result r = Simulate(GDX_FRAMESKIP_AUTO, 4, &o3dsRace, 3600, 0);
        Report("auto max4 / O3DS race", &r);
        Check(Tps(&r) > 59.0 && Tps(&r) < 60.0, "auto max4 must bring the O3DS race to ~59.94 ticks/s");
        Check(r.maxRun <= 4, "auto must respect frameskip_max");
    }
    {
        /* The default cap (5) against the race's slow tail: 30 machines on screen, p90 frames. */
        const Plant heavyRace = {4300, 75000, 0, 0};
        const Result r = Simulate(GDX_FRAMESKIP_AUTO, 5, &heavyRace, 3600, 0);
        Report("auto max5 / O3DS race p90", &r);
        Check(Tps(&r) > 59.0 && Tps(&r) < 60.0, "the default cap holds ~59.94 ticks/s on a p90 race frame");
        Check(r.maxRun <= 5, "auto must respect frameskip_max");
    }
    {
        const Result r = Simulate(GDX_FRAMESKIP_AUTO, 4, &o3dsTrial, 3600, 0);
        Report("auto max4 / O3DS time trial", &r);
        Check(Tps(&r) > 59.0 && Tps(&r) < 60.0, "auto must bring the O3DS time trial to ~59.94 ticks/s");
        Check(Fps(&r) > 16.0, "time trial must render more than the race does");
    }
    {
        const Result r = Simulate(GDX_FRAMESKIP_AUTO, 2, &o3dsRace, 3600, 0);
        Report("auto max2 / O3DS race", &r);
        Check(r.maxRun <= 2, "the cap binds");
        Check(Tps(&r) < 59.0, "a cap too low for the scene degrades to slow motion, not a burst");
    }
    {
        const Result r = Simulate(GDX_FRAMESKIP_AUTO, 4, &hopeless, 3600, 0);
        Report("auto max4 / hopeless", &r);
        Check(r.maxRun == 4, "a hopeless scene runs at the cap");
        Check(r.rendered * 5 >= r.ticks, "at the cap one frame in five still renders");
    }
    {
        const Result r = Simulate(GDX_FRAMESKIP_FIXED, 3, &fast, 36000, 0);
        Report("fixed 3 / fast", &r);
        Check(r.phaseOk, "fixed renders exactly the frames with frame % (N+1) == 0");
        Check(r.rendered * 4 == r.ticks, "fixed 3 renders one frame in four");
        Check(Tps(&r) < 60.0, "fixed skips must sleep to the schedule: a fast scene must not run the game fast");
        Check(Tps(&r) > 59.5, "fixed in a fast scene still runs the logic at ~59.94");
    }
    {
        const Result r = Simulate(GDX_FRAMESKIP_FIXED, 3, &o3dsRace, 3600, 0);
        Report("fixed 3 / O3DS race", &r);
        Check(r.phaseOk, "fixed phase holds in a slow scene");
        Check(Tps(&r) > 40.0, "fixed 3 lifts the O3DS race from ~15 to >40 ticks/s");
    }
    {
        const Result r = Simulate(GDX_FRAMESKIP_FIXED, 3, &o3dsRace, 3600, 6);
        Report("fixed 3 / veto every 6", &r);
        Check(r.phaseOk, "fixed renders only its phase or vetoed frames");
        Check(r.vetoed > 0, "a veto on an off-phase frame is counted");
    }
    {
        /* The veto always wins, in either mode: a sampled frame is never skipped. */
        GdxFrameskipGov gov;
        uint64_t t = 1000000u;
        int allRendered = 1;
        gdx_frameskip_gov_init(&gov, GDX_FRAMESKIP_AUTO, 4, INTERVAL_US);
        for (unsigned long frame = 1; frame <= 200; frame++) {
            const GdxFrameskipDecision d = gdx_frameskip_gov_decide(&gov, t, frame, 1);
            t += 70000u;
            if (d.skip) {
                allRendered = 0;
            }
        }
        Check(allRendered, "a vetoed frame is never skipped");
    }

    {
        /* n60fs: the O3DS menus/results under race0's other mode (hw-n60card2-C: legacy
         * interpreter ~44 ms render, ~2 ms skip ticks; frameskip 0 ran them at 20-24 tps). */
        const Plant o3dsMenu = {2000, 44000, 0, 0};
        const Result r = Simulate(GDX_FRAMESKIP_AUTO, 5, &o3dsMenu, 3600, 0);
        Report("auto max5 / O3DS menu (race0 other)", &r);
        Check(Tps(&r) > 59.0 && Tps(&r) < 60.0, "race0's auto brings the O3DS menus to ~59.94 ticks/s");
        Check(Fps(&r) > 18.0, "the O3DS menus still draw ~20 fps under auto");
    }
    SteadyTests();
    SteadyRobustTests();
    LiveSwitchTests();
    Race0ScopeTests();
    Race0GuardTests();
    Race0DwellTests();
    Race0PostFinishTests();
    SteadyNetLateTests();
    SteadyUpPctTests();
    SteadyD1HoldTests();
    SteadyD1WindowTests();
    SteadyD1DriftTests();
    SteadyLock30Tests();

    if (sFailures == 0) {
        printf("=== ALL PASS (0 failures) ===\n");
        return 0;
    }
    printf("=== %d FAILURE(S) ===\n", sFailures);
    return 1;
}
