/* port/3ds/net/gdx3ds_mp_timesync.c — mp-timesync 3DS glue. Contract: gdx3ds_mp_timesync.h. */
#include "gdx3ds_mp_timesync.h"

#include "gdx3ds_mp_netab.h"
#include "gdx_mp_netab.h"
#include "gdx_mp_timesync.h"
#include "gdx3ds_config.h"
#include "gdx3ds_filelog.h"

#include <3ds.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

extern void gdx3ds_frameskip_note_net_wait(uint64_t ticks) __attribute__((weak));

#define MPTS_TICKS_PER_US (SYSCLOCK_ARM11 / 1000000ull)

static GdxMpTimesync sTs;
static int sBegun = 0;
static int sLever = 1;
static int sCapMs = 3;
static int sCapLever = 3; /* card-3: the ini value as a lever (GDX_MP_NETAB_TIMESYNC_AB = A/B) */
static uint32_t sWinStallsAtStart = 0;
static uint64_t sWinStallUsAtStart = 0;

static void MptsLog(const char* fmt, ...) {
    char line[384];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n <= 0) {
        return;
    }
    if (n >= (int) sizeof(line)) {
        n = (int) sizeof(line) - 1;
    }
    svcOutputDebugString(line, n);
    gdx3ds_filelog_write(line, (size_t) n);
}

static uint64_t NowUs(void) {
    return svcGetSystemTick() / MPTS_TICKS_PER_US;
}

static uint32_t NowMs(void) {
    return (uint32_t) osGetTime(); /* the clock gdx_mp_ls_ingest stamps knownAtMs with */
}

static void LoadLevers(void) {
    sLever = gdx3ds_config_get_bool("net", "mp_timesync", 1) ? 1 : 0;
    sCapLever = gdx3ds_mpab_timesync_lever();
    sCapMs = gdx3ds_mpab_timesync_cap_ms(sCapLever); /* card-3: "ab" = this race's arm, 3 or 16 */
}

static const char* SignOf(int32_t milli) {
    return milli < 0 ? "-" : "+";
}

static uint32_t AbsMilli(int32_t milli) {
    return (uint32_t) (milli < 0 ? -milli : milli);
}

void gdx3ds_mpts_race_begin(const GdxMpLockstep* ls) {
    GdxMpTsConfig cfg;
    LoadLevers();
    gdx_mp_ts_default_config(&cfg, ls->delay, sLever && sCapMs > 0, (uint32_t) sCapMs * 1000u);
    gdx_mp_ts_init(&sTs, &cfg, ls->delay);
    sBegun = 1;
    sWinStallsAtStart = 0;
    sWinStallUsAtStart = 0;
    MptsLog("[mpts] race start on=%d cap_ms=%d%s delay=%d on/set/off=%lu.%02lu/%lu.%02lu/%lu.%02lu ticks "
            "(debug: net.mp_timesync, net.mp_timesync_max_ms)",
            cfg.enabled, sCapMs, sCapLever == GDX_MP_NETAB_TIMESYNC_AB ? " (A/B)" : "", ls->delay, (unsigned long) (cfg.onMilli / 1000), (unsigned long) (cfg.onMilli % 1000 / 10),
            (unsigned long) (cfg.setMilli / 1000), (unsigned long) (cfg.setMilli % 1000 / 10),
            (unsigned long) (cfg.offMilli / 1000), (unsigned long) (cfg.offMilli % 1000 / 10));
}

static void NoteFrameskipWait(uint32_t sleepUs) {
    if (gdx3ds_frameskip_note_net_wait != NULL) {
        gdx3ds_frameskip_note_net_wait((uint64_t) sleepUs * MPTS_TICKS_PER_US); /* not this console's cost */
    }
}

void gdx3ds_mpts_pace(const GdxMpLockstep* ls, uint32_t lt) {
    uint32_t sleepUs;
    if (!sBegun) {
        gdx3ds_mpts_race_begin(ls);
    }
    sleepUs = gdx_mp_ts_pace(&sTs, ls, lt, NowMs(), NowUs(), !gdx_mp_ls_ready(ls, lt));
    if (sleepUs == 0) {
        return;
    }
    svcSleepThread((s64) sleepUs * 1000LL);
    NoteFrameskipWait(sleepUs);
}

void gdx3ds_mpts_ran(int stalled) {
    if (sBegun) {
        gdx_mp_ts_ran(&sTs, NowUs(), stalled);
    }
}

static void Receipt(const char* what, const GdxMpTsStats* st, uint32_t stalls, uint64_t stallUs) {
    const int32_t lead = gdx_mp_ts_mean_lead(st);
    const uint32_t stretchUs = st->pacedTicks ? (uint32_t) (st->stretchSumUs / st->pacedTicks) : 0u;
    const int worst = sTs.lastWorstPeer;
    MptsLog("[mpts] %s on=%d lead=%s%lu.%02lu ema=%s%lu.%02lu..%s%lu.%02lu vs=p%d engaged=%d stretch_ms=%lu.%02lu "
            "paced=%lu/%lu applied_ms=%lu sleep_ms=%lu max_sleep_ms=%lu.%lu absorbed=%lu engages=%lu stalls=%lu "
            "stall_ms=%lu rtt=%lu pace_ms=%lu.%lu",
            what, sTs.cfg.enabled, SignOf(lead), (unsigned long) (AbsMilli(lead) / 1000),
            (unsigned long) (AbsMilli(lead) % 1000 / 10), SignOf(st->leadMinMilli),
            (unsigned long) (AbsMilli(st->leadMinMilli) / 1000), (unsigned long) (AbsMilli(st->leadMinMilli) % 1000 / 10),
            SignOf(st->leadMaxMilli), (unsigned long) (AbsMilli(st->leadMaxMilli) / 1000),
            (unsigned long) (AbsMilli(st->leadMaxMilli) % 1000 / 10), worst, sTs.active,
            (unsigned long) (stretchUs / 1000), (unsigned long) (stretchUs % 1000 / 10), (unsigned long) st->pacedTicks,
            (unsigned long) st->ticks, (unsigned long) (st->stretchSumUs / 1000u), (unsigned long) (st->sleptUs / 1000u),
            (unsigned long) (st->maxSleepUs / 1000u), (unsigned long) (st->maxSleepUs % 1000u / 100u),
            (unsigned long) st->absorbed, (unsigned long) st->engages, (unsigned long) stalls,
            (unsigned long) (stallUs / 1000u), (unsigned long) (worst >= 0 ? sTs.rttEmaUs[worst] / 1000u : 0u),
            (unsigned long) (sTs.periodEmaUs / 1000u), (unsigned long) (sTs.periodEmaUs % 1000u / 100u));
}

void gdx3ds_mpts_window(uint32_t lt, uint32_t stalls, uint64_t stallUs) {
    char what[24];
    if (!sBegun) {
        return;
    }
    snprintf(what, sizeof(what), "lt=%lu", (unsigned long) lt);
    Receipt(what, &sTs.win, stalls - sWinStallsAtStart, stallUs - sWinStallUsAtStart);
    sWinStallsAtStart = stalls;
    sWinStallUsAtStart = stallUs;
    gdx_mp_ts_window_reset(&sTs);
}

void gdx3ds_mpts_race_end(uint32_t lt, uint32_t stalls, uint64_t stallUs) {
    char what[32];
    if (!sBegun) {
        return;
    }
    snprintf(what, sizeof(what), "race end lt=%lu", (unsigned long) lt);
    Receipt(what, &sTs.total, stalls, stallUs);
    sBegun = 0;
}
