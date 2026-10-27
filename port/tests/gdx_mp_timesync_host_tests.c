/* port/tests/gdx_mp_timesync_host_tests.c — mp-timesync (port/gdx_mp_timesync.c) over the real
 * lockstep core (port/gdx_mp_lockstep.c), in a TWO-CLOCK MODEL of a race:
 *
 *   - every console has its own clock (offset, rate skew, one of them about to wrap the 32-bit ms
 *     counter) and runs the 3DS race loop: Exchange (push our pad, send, drain, pace, wait for the
 *     tick's pads polling every 0.5 ms with 16 ms resends, as gdx3ds_mp_online.c WaitReady) ->
 *     `pre` of work -> the frame begin (citro3d's adaptive vblank sync: wait for the next vblank
 *     unless one vblank already passed since the previous begin, gdx3ds_gpu_prof.c) -> `post`;
 *   - the network delivers each datagram after base + exponential jitter, or loses it;
 *   - a console only sees datagrams when it drains.
 * Scenarios: a vblank-bound fast console + one 10% slower (the headline), equal speeds, the
 * hardware-like gap (60 vs ~32 ticks/s, online1), three consoles, mixed levers, heavy loss.
 * Checks: every console consumes the identical pad sequence with time sync on or off (pacing never
 * changes which pad lands on which tick); on the 10%-slower model the fast console's stall count and
 * stall time drop by most of their size and the race takes no longer; equal consoles are not slowed;
 * the slower console never paces. The model table is printed (the gain receipt for the report). */
#include "gdx_mp_lockstep.h"
#include "gdx_mp_timesync.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int sFailures = 0;
static int sChecks = 0;

#define CHECK(cond)                                                                   \
    do {                                                                              \
        sChecks++;                                                                    \
        if (!(cond)) {                                                                \
            sFailures++;                                                              \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
        }                                                                             \
    } while (0)

/* ---- deterministic randomness ------------------------------------------------------------------ */
static uint64_t sRng = 88172645463325252ull;

static uint32_t Rand32(void) {
    sRng ^= sRng << 13;
    sRng ^= sRng >> 7;
    sRng ^= sRng << 17;
    return (uint32_t) (sRng >> 16);
}

static double RandUnit(void) {
    return (double) (Rand32() & 0xFFFFFFu) / (double) 0x1000000u;
}

static double RandExp(double mean) {
    return mean > 0.0 ? -mean * log(1.0 - RandUnit()) : 0.0;
}

/* ---- the network ------------------------------------------------------------------------------- */
#define MAX_CONSOLES 4
#define MAX_INFLIGHT 8192

typedef struct Datagram {
    int to;
    int len;
    uint64_t deliverAt; /* global us */
    uint8_t data[GDX_MP_MAX_PACKET];
} Datagram;

typedef struct NetModel {
    double baseUs;
    double jitterUs;
    int lossPermille;
} NetModel;

static Datagram sInflight[MAX_INFLIGHT];
static int sInflightN = 0;

static void NetSend(const NetModel* net, int to, const uint8_t* buf, int len, uint64_t nowUs) {
    Datagram* d;
    if ((int) (Rand32() % 1000u) < net->lossPermille || sInflightN >= MAX_INFLIGHT) {
        return;
    }
    d = &sInflight[sInflightN++];
    d->to = to;
    d->len = len;
    d->deliverAt = nowUs + (uint64_t) (net->baseUs + RandExp(net->jitterUs));
    memcpy(d->data, buf, (size_t) len);
}

/* ---- a console --------------------------------------------------------------------------------- */
enum { ST_EXCHANGE, ST_PACED, ST_WAIT, ST_DONE };

typedef struct ConsoleModel {
    double preUs;       /* exchange -> frame begin (the game tick, the DL build) */
    double postUs;      /* frame begin -> the next exchange (the render, the tail) */
    double jitterUs;    /* uniform +- on post */
    double vblankPhaseUs;
    int64_t clockOffsetUs; /* this console's clock = global * rate + offset */
    double clockRate;
    int timesync;       /* [net] mp_timesync */
    uint32_t capUs;
} ConsoleModel;

typedef struct Console {
    ConsoleModel m;
    GdxMpLockstep ls;
    GdxMpTimesync ts;
    int state;
    uint64_t next;      /* global us of its next step */
    uint32_t lt;
    uint64_t waitStart;
    uint64_t prevBegin;
    uint64_t lastRun;
    uint32_t lastSendMs;
    /* results */
    uint32_t stalls;
    uint64_t stallUs;
    uint64_t maxStallUs;
    uint32_t hitches;   /* run-to-run gaps over 2.5 vblanks */
    double gapSum;
    double gapSq;
    uint32_t gaps;
    uint64_t finishedAt;
    uint32_t padLog;
} Console;

typedef struct Scenario {
    const char* name;
    int players;
    int delay;
    uint32_t ticks;
    NetModel net;
    ConsoleModel model[MAX_CONSOLES];
} Scenario;

typedef struct Result {
    uint32_t stalls[MAX_CONSOLES];
    uint64_t stallUs[MAX_CONSOLES];
    uint64_t maxStallUs[MAX_CONSOLES];
    uint32_t hitches[MAX_CONSOLES];
    double gapSdUs[MAX_CONSOLES];
    uint32_t paced[MAX_CONSOLES];
    uint64_t sleptUs[MAX_CONSOLES];
    uint64_t stretchUs[MAX_CONSOLES];
    uint32_t absorbed[MAX_CONSOLES];
    int32_t meanLead[MAX_CONSOLES];
    uint64_t raceUs;
    uint32_t padLog[MAX_CONSOLES];
    int completed;
} Result;

static uint64_t LocalUs(const Console* c, uint64_t globalUs) {
    return (uint64_t) ((int64_t) ((double) globalUs * c->m.clockRate) + c->m.clockOffsetUs);
}

static uint32_t LocalMs(const Console* c, uint64_t globalUs) {
    return (uint32_t) (LocalUs(c, globalUs) / 1000u);
}

static uint32_t Mix(uint32_t h, uint32_t v) {
    h ^= v;
    h *= 16777619u;
    return h;
}

static GdxMpPad ScriptPad(int player, uint32_t tick) {
    GdxMpPad pad;
    pad.buttons = (uint16_t) (((tick / 7u) * 31u + (uint32_t) player * 977u) & GDX_MP_BUTTON_MASK);
    pad.stickX = (int8_t) ((int) ((tick * 13u + (uint32_t) player * 5u) % 161u) - 80);
    pad.stickY = (int8_t) ((int) ((tick * 7u + (uint32_t) player * 3u) % 161u) - 80);
    return pad;
}

static void SendOurs(Console* cs, int players, int me, const NetModel* net, uint64_t nowUs) {
    uint8_t pkt[GDX_MP_MAX_PACKET];
    Console* c = &cs[me];
    const int n = gdx_mp_ls_build_packet(&c->ls, LocalMs(c, nowUs), pkt, sizeof(pkt));
    int p;
    if (n <= 0) {
        return;
    }
    for (p = 0; p < players; p++) {
        if (p != me) {
            NetSend(net, p, pkt, n, nowUs);
        }
    }
    c->lastSendMs = LocalMs(c, nowUs);
}

static void Drain(Console* c, int me, uint64_t nowUs) {
    int i = 0;
    while (i < sInflightN) {
        Datagram* d = &sInflight[i];
        if (d->to == me && d->deliverAt <= nowUs) {
            gdx_mp_ls_ingest(&c->ls, d->data, d->len, LocalMs(c, nowUs));
            sInflight[i] = sInflight[--sInflightN];
        } else {
            i++;
        }
    }
}

/* citro3d's adaptive sync: begin at once when a vblank passed since the previous begin, else at
 * the next vblank. */
static uint64_t FrameBegin(Console* c, uint64_t arriveUs) {
    const double period = (double) GDX_MP_TS_VBLANK_US;
    uint64_t begin = arriveUs;
    if (c->prevBegin != 0 && arriveUs - c->prevBegin < (uint64_t) period) {
        const double k = ceil(((double) arriveUs - c->m.vblankPhaseUs) / period);
        begin = (uint64_t) (k * period + c->m.vblankPhaseUs);
        if (begin < arriveUs) {
            begin = arriveUs;
        }
    }
    c->prevBegin = begin;
    return begin;
}

static double PostWork(const Console* c) {
    return c->m.postUs + (c->m.jitterUs > 0.0 ? (RandUnit() * 2.0 - 1.0) * c->m.jitterUs : 0.0);
}

static void NoteRun(Console* c, uint64_t nowUs, int me) {
    const uint64_t waited = nowUs - c->waitStart;
    GdxMpPad pads[GDX_MP_MAX_PLAYERS];
    int p;
    if (waited > 200u) {
        c->stalls++;
        c->stallUs += waited;
        if (waited > c->maxStallUs) {
            c->maxStallUs = waited;
        }
    }
    if (c->lastRun != 0) {
        const double gap = (double) (nowUs - c->lastRun);
        c->gapSum += gap;
        c->gapSq += gap * gap;
        c->gaps++;
        if (gap > 2.5 * GDX_MP_TS_VBLANK_US) {
            c->hitches++;
        }
    }
    c->lastRun = nowUs;
    gdx_mp_ts_ran(&c->ts, LocalUs(c, nowUs), waited > 200u);
    gdx_mp_ls_get(&c->ls, c->lt, pads);
    for (p = 0; p < c->ls.numPlayers; p++) {
        c->padLog = Mix(c->padLog, (uint32_t) pads[p].buttons | ((uint32_t) (uint8_t) pads[p].stickX << 16) |
                                       ((uint32_t) (uint8_t) pads[p].stickY << 24));
    }
    (void) me;
}

static void StepExchange(Console* cs, int players, int me, const Scenario* sc, uint64_t t) {
    Console* c = &cs[me];
    uint32_t sleepUs;
    gdx_mp_ls_push_local(&c->ls, c->lt, ScriptPad(me, c->lt));
    SendOurs(cs, players, me, &sc->net, t);
    Drain(c, me, t);
    sleepUs = gdx_mp_ts_pace(&c->ts, &c->ls, c->lt, LocalMs(c, t), LocalUs(c, t), !gdx_mp_ls_ready(&c->ls, c->lt));
    c->state = sleepUs > 0 ? ST_PACED : ST_WAIT;
    c->next = t + sleepUs;
    c->waitStart = c->next;
}

static void ResendWhileWaiting(Console* cs, int players, int me, const Scenario* sc, uint64_t t) {
    Console* c = &cs[me];
    const uint32_t since = LocalMs(c, t) - c->lastSendMs;
    if (since >= 100u || (since >= 16u && gdx_mp_ls_unacked(&c->ls))) {
        SendOurs(cs, players, me, &sc->net, t);
    }
}

static void StepWait(Console* cs, int players, int me, const Scenario* sc, uint64_t t) {
    Console* c = &cs[me];
    Drain(c, me, t);
    if (!gdx_mp_ls_ready(&c->ls, c->lt)) {
        ResendWhileWaiting(cs, players, me, sc, t);
        c->next = t + 500u;
        return;
    }
    NoteRun(c, t, me);
    c->lt++;
    if (c->lt >= sc->ticks) {
        c->state = ST_DONE;
        c->finishedAt = t;
        c->next = t + 500u;
        return;
    }
    c->next = FrameBegin(c, t + (uint64_t) c->m.preUs) + (uint64_t) PostWork(c);
    c->state = ST_EXCHANGE;
}

static void StepConsole(Console* cs, int players, int me, const Scenario* sc) {
    Console* c = &cs[me];
    const uint64_t t = c->next;
    switch (c->state) {
        case ST_EXCHANGE:
            StepExchange(cs, players, me, sc, t);
            break;
        case ST_PACED:
            Drain(c, me, t);
            c->state = ST_WAIT;
            break;
        case ST_WAIT:
            StepWait(cs, players, me, sc, t);
            break;
        default:
            Drain(c, me, t); /* finished: keep acking and resending for the others */
            ResendWhileWaiting(cs, players, me, sc, t);
            c->next = t + 500u;
            break;
    }
}

static int AllDone(const Console* cs, int players) {
    int p;
    for (p = 0; p < players; p++) {
        if (cs[p].state != ST_DONE) {
            return 0;
        }
    }
    return 1;
}

static int Earliest(const Console* cs, int players) {
    int best = 0;
    int p;
    for (p = 1; p < players; p++) {
        if (cs[p].next < cs[best].next) {
            best = p;
        }
    }
    return best;
}

static void InitConsole(Console* c, const Scenario* sc, int me, int timesyncOverride) {
    GdxMpTsConfig cfg;
    memset(c, 0, sizeof(*c));
    c->m = sc->model[me];
    if (timesyncOverride >= 0) {
        c->m.timesync = timesyncOverride;
    }
    gdx_mp_ls_init(&c->ls, sc->players, me, sc->delay, 1, 0xC0FFEEu);
    gdx_mp_ts_default_config(&cfg, sc->delay, c->m.timesync, c->m.capUs ? c->m.capUs : GDX_MP_TS_DEFAULT_CAP_US);
    gdx_mp_ts_init(&c->ts, &cfg, sc->delay);
    c->state = ST_EXCHANGE;
    c->next = 1000u + (uint64_t) me * 3000u; /* the consoles reach race tick 0 a little apart */
    c->padLog = 2166136261u;
}

static void Collect(const Console* cs, int players, Result* r) {
    int p;
    for (p = 0; p < players; p++) {
        const Console* c = &cs[p];
        const double mean = c->gaps ? c->gapSum / c->gaps : 0.0;
        r->stalls[p] = c->stalls;
        r->stallUs[p] = c->stallUs;
        r->maxStallUs[p] = c->maxStallUs;
        r->hitches[p] = c->hitches;
        r->gapSdUs[p] = c->gaps ? sqrt(fmax(0.0, c->gapSq / c->gaps - mean * mean)) : 0.0;
        r->paced[p] = c->ts.total.pacedTicks;
        r->sleptUs[p] = c->ts.total.sleptUs;
        r->stretchUs[p] = c->ts.total.stretchSumUs;
        r->absorbed[p] = c->ts.total.absorbed;
        r->meanLead[p] = gdx_mp_ts_mean_lead(&c->ts.total);
        r->padLog[p] = c->padLog;
        if (c->finishedAt > r->raceUs) {
            r->raceUs = c->finishedAt;
        }
    }
}

/* timesyncOverride: -1 = each console's model, 0 / 1 = force every console off / on. */
static Result RunScenario(const Scenario* sc, int timesyncOverride, uint64_t seed) {
    static Console cs[MAX_CONSOLES];
    Result r;
    int p;
    uint64_t steps = 0;
    memset(&r, 0, sizeof(r));
    sRng = seed;
    sInflightN = 0;
    for (p = 0; p < sc->players; p++) {
        InitConsole(&cs[p], sc, p, timesyncOverride);
    }
    while (!AllDone(cs, sc->players) && steps < 400000000ull) {
        StepConsole(cs, sc->players, Earliest(cs, sc->players), sc);
        steps++;
    }
    r.completed = AllDone(cs, sc->players);
    Collect(cs, sc->players, &r);
    return r;
}

/* ---- scenarios ----------------------------------------------------------------------------------- */
#define VBLANK ((double) GDX_MP_TS_VBLANK_US)

static ConsoleModel FastNew3ds(void) {
    ConsoleModel m;
    memset(&m, 0, sizeof(m));
    m.preUs = 4000.0; /* a 9 ms tick: vblank-bound at 59.83 Hz */
    m.postUs = 5000.0;
    m.jitterUs = 800.0;
    m.vblankPhaseUs = 3100.0;
    m.clockRate = 1.0;
    m.timesync = 1;
    return m;
}

static ConsoleModel SlowerBy(double factor, double phaseUs) {
    ConsoleModel m;
    memset(&m, 0, sizeof(m));
    m.preUs = 0.45 * VBLANK * factor; /* CPU-bound: factor x one vblank per tick */
    m.postUs = 0.55 * VBLANK * factor;
    m.jitterUs = 1200.0;
    m.vblankPhaseUs = phaseUs;
    /* The second clock: another epoch, 300 ppm fast, its 32-bit ms counter wrapping mid-race. */
    m.clockOffsetUs = (int64_t) (4294967296ull - 30000ull) * 1000;
    m.clockRate = 1.0003;
    m.timesync = 1;
    return m;
}

static Scenario TenPercent(void) {
    Scenario sc;
    memset(&sc, 0, sizeof(sc));
    sc.name = "fast N3DS + 10% slower";
    sc.players = 2;
    sc.delay = 4;
    sc.ticks = 6000;
    sc.net.baseUs = 4000.0;
    sc.net.jitterUs = 3000.0;
    sc.net.lossPermille = 20;
    sc.model[0] = FastNew3ds();
    sc.model[1] = SlowerBy(1.10, 9000.0);
    return sc;
}

static Scenario EqualSpeed(void) {
    Scenario sc = TenPercent();
    sc.name = "equal speeds (both vblank-bound)";
    sc.model[1] = FastNew3ds();
    sc.model[1].vblankPhaseUs = 11000.0;
    sc.model[1].clockOffsetUs = 123456789;
    sc.model[1].clockRate = 0.9997;
    return sc;
}

static Scenario HardwareGap(uint32_t capUs) {
    Scenario sc = TenPercent();
    sc.name = capUs == GDX_MP_TS_DEFAULT_CAP_US ? "hw-like 60 vs 32 tps, cap 3 ms" : "hw-like 60 vs 32 tps, cap 16 ms";
    sc.delay = 6;
    sc.net.baseUs = 10000.0;
    sc.net.jitterUs = 12000.0;
    sc.model[1] = SlowerBy(1.87, 9000.0); /* ~32 ticks/s: online1's guest pace */
    sc.model[1].jitterUs = 6000.0;
    sc.model[0].capUs = capUs;
    sc.model[1].capUs = capUs;
    return sc;
}

static Scenario ThreeConsoles(void) {
    Scenario sc = TenPercent();
    sc.name = "3 consoles: fast, fast, 10% slower";
    sc.players = 3;
    sc.model[1] = FastNew3ds();
    sc.model[1].vblankPhaseUs = 9000.0;
    sc.model[2] = SlowerBy(1.10, 14000.0);
    return sc;
}

static Scenario BothCpuBound(void) {
    Scenario sc = TenPercent();
    sc.name = "both CPU-bound: 20 ms vs 22 ms ticks";
    sc.model[0] = SlowerBy(20000.0 / VBLANK, 3100.0);
    sc.model[0].clockOffsetUs = 0;
    sc.model[0].clockRate = 1.0;
    sc.model[1] = SlowerBy(22000.0 / VBLANK, 9000.0);
    return sc;
}

static Scenario HeavyLoss(void) {
    Scenario sc = TenPercent();
    sc.name = "10% slower, 25% loss, 30 ms jitter";
    sc.ticks = 3000;
    sc.net.jitterUs = 30000.0;
    sc.net.lossPermille = 250;
    return sc;
}

static void PrintRow(const Scenario* sc, const char* mode, const Result* r) {
    int p;
    printf("  %-36s %-4s race=%7.2f s", sc->name, mode, (double) r->raceUs / 1e6);
    for (p = 0; p < sc->players; p++) {
        printf(" | p%d stalls=%5lu stall=%7.1f ms max=%5.1f hitch=%4lu sd=%5.2f ms paced=%5lu stretch=%4.2f ms "
               "absorbed=%4lu lead=%+5.2f",
               p, (unsigned long) r->stalls[p], (double) r->stallUs[p] / 1000.0, (double) r->maxStallUs[p] / 1000.0,
               (unsigned long) r->hitches[p], r->gapSdUs[p] / 1000.0, (unsigned long) r->paced[p],
               r->paced[p] ? (double) r->stretchUs[p] / 1000.0 / r->paced[p] : 0.0, (unsigned long) r->absorbed[p],
               r->meanLead[p] / 1000.0);
    }
    printf("\n");
}

static void CheckSamePads(const Scenario* sc, const Result* off, const Result* on) {
    int p;
    CHECK(off->completed && on->completed);
    for (p = 0; p < sc->players; p++) {
        CHECK(off->padLog[p] == off->padLog[0]); /* every console consumed the same pads ... */
        CHECK(on->padLog[p] == off->padLog[0]);  /* ... and time sync did not change one of them */
    }
}

static void RunPair(const Scenario* sc, Result* off, Result* on) {
    *off = RunScenario(sc, 0, 0x9E3779B97F4A7C15ull);
    *on = RunScenario(sc, -1, 0x9E3779B97F4A7C15ull);
    PrintRow(sc, "off", off);
    PrintRow(sc, "on", on);
    CheckSamePads(sc, off, on);
}

/* The headline: the fast console's frequent short stalls mostly go, the race is not slower. */
static void TestTenPercentSlower(void) {
    const Scenario sc = TenPercent();
    Result off, on;
    RunPair(&sc, &off, &on);
    CHECK(off.stalls[0] > 500u); /* the model reproduces the problem */
    CHECK(on.stalls[0] * 5u <= off.stalls[0]);
    CHECK(on.stallUs[0] * 4u <= off.stallUs[0]);
    CHECK(on.hitches[0] * 4u <= off.hitches[0] + 4u);
    CHECK(on.raceUs <= off.raceUs + off.raceUs / 50u);
    CHECK(on.paced[1] * 20u <= sc.ticks); /* the slower console (almost) never paces */
    CHECK(on.meanLead[1] < 0);
}

static void TestEqualSpeedNotSlowed(void) {
    const Scenario sc = EqualSpeed();
    Result off, on;
    RunPair(&sc, &off, &on);
    CHECK(on.raceUs <= off.raceUs + off.raceUs / 100u);
    CHECK(on.stalls[0] <= off.stalls[0] + 20u && on.stalls[1] <= off.stalls[1] + 20u);
}

static void TestHardwareGap(void) {
    const Scenario cap3 = HardwareGap(GDX_MP_TS_DEFAULT_CAP_US);
    const Scenario cap16 = HardwareGap(GDX_MP_TS_MAX_CAP_US);
    Result off, on, off16, on16;
    RunPair(&cap3, &off, &on);
    RunPair(&cap16, &off16, &on16);
    CHECK(on.stallUs[0] < off.stallUs[0]);
    CHECK(on.raceUs <= off.raceUs + off.raceUs / 50u);
    CHECK(on16.stalls[0] * 2u <= off16.stalls[0]);
    CHECK(on16.raceUs <= off16.raceUs + off16.raceUs / 50u);
}

static void TestThreeConsoles(void) {
    const Scenario sc = ThreeConsoles();
    Result off, on;
    RunPair(&sc, &off, &on);
    CHECK(on.stalls[0] * 3u <= off.stalls[0] && on.stalls[1] * 3u <= off.stalls[1]);
    CHECK(on.raceUs <= off.raceUs + off.raceUs / 50u);
}

/* No vblank wait to take in: the stretch is a plain sleep on top of the console's own work. */
static void TestBothCpuBound(void) {
    const Scenario sc = BothCpuBound();
    Result off, on;
    RunPair(&sc, &off, &on);
    CHECK(on.stalls[0] * 5u <= off.stalls[0]);
    CHECK(on.raceUs <= off.raceUs + off.raceUs / 50u);
}

static void TestMixedLevers(void) {
    Scenario sc = TenPercent();
    Result off, mixed;
    sc.name = "10% slower, lever on fast only";
    sc.model[1].timesync = 0;
    RunPair(&sc, &off, &mixed);
    CHECK(mixed.stalls[0] * 5u <= off.stalls[0]);
    CHECK(mixed.paced[1] == 0u);
}

static void TestHeavyLoss(void) {
    const Scenario sc = HeavyLoss();
    Result off, on;
    RunPair(&sc, &off, &on);
    CHECK(on.raceUs <= off.raceUs + off.raceUs / 20u);
}

/* ---- unit checks of the pure core ---------------------------------------------------------------- */
static void TestNoEstimateBeforeRealPads(void) {
    GdxMpLockstep ls;
    GdxMpTimesync ts;
    GdxMpTsConfig cfg;
    int32_t lead;
    int peer;
    gdx_mp_ls_init(&ls, 2, 0, 4, 1, 1u);
    gdx_mp_ts_default_config(&cfg, 4, 1, GDX_MP_TS_DEFAULT_CAP_US);
    gdx_mp_ts_init(&ts, &cfg, 4);
    CHECK(gdx_mp_ts_lead(&ts, &ls, 0, 0, &lead, &peer) == 0);
    CHECK(gdx_mp_ts_pace(&ts, &ls, 0, 0, 0, 1) == 0);
    CHECK(cfg.onMilli == 1500 && cfg.setMilli == 1000 && cfg.offMilli == 500);
    gdx_mp_ts_default_config(&cfg, 2, 1, 99999u);
    CHECK(cfg.onMilli == 1000 && cfg.setMilli == 666 && cfg.offMilli == 333 && cfg.capUs == GDX_MP_TS_MAX_CAP_US);
}

/* A removed or frozen peer is no longer waited for, so it must not hold the lead estimate either. */
static void TestRemovedPeerIgnored(void) {
    GdxMpLockstep ls;
    GdxMpTimesync ts;
    GdxMpTsConfig cfg;
    int32_t lead;
    int peer;
    gdx_mp_ls_init(&ls, 2, 0, 4, 1, 1u);
    gdx_mp_ts_default_config(&cfg, 4, 1, GDX_MP_TS_DEFAULT_CAP_US);
    gdx_mp_ts_init(&ts, &cfg, 4);
    gdx_mp_ls_freeze(&ls, 1);
    CHECK(gdx_mp_ts_lead(&ts, &ls, 100, 0, &lead, &peer) == 0);
    CHECK(peer == -1);
}

/* The lever off: the estimate still runs (receipts), the console never sleeps. */
static void TestLeverOffNeverSleeps(void) {
    Scenario sc = TenPercent();
    Result r;
    sc.ticks = 1500;
    r = RunScenario(&sc, 0, 7u);
    CHECK(r.completed);
    CHECK(r.paced[0] == 0u && r.paced[1] == 0u && r.sleptUs[0] == 0u);
    CHECK(r.meanLead[0] > 1000); /* the fast console measured itself at the wall */
}

int main(void) {
    printf("mp-timesync two-clock model (stall = a wait over 0.2 ms; hitch = a run-to-run gap over 2.5 "
           "vblanks; sd = run-to-run gap std dev; lead = mean raw frame advantage in ticks):\n");
    TestNoEstimateBeforeRealPads();
    TestRemovedPeerIgnored();
    TestLeverOffNeverSleeps();
    TestTenPercentSlower();
    TestEqualSpeedNotSlowed();
    TestThreeConsoles();
    TestMixedLevers();
    TestBothCpuBound();
    TestHeavyLoss();
    TestHardwareGap();
    if (sFailures != 0) {
        fprintf(stderr, "gdx_mp_timesync_host_tests: %d of %d checks FAILED\n", sFailures, sChecks);
        return 1;
    }
    printf("gdx_mp_timesync_host_tests: all %d checks passed\n", sChecks);
    return 0;
}
