/* port/3ds/gdx3ds_dynlod.c — DYNLOD controller. Contract in the header. */
#include "gdx3ds_dynlod.h"

#include <stdio.h>
#include <string.h>

#include <3ds.h>

#include "gdx3ds_config.h"
#include "gdx3ds_filelog.h"
#include "gfx/gdx3ds_gpu_prof.h" /* gdx3ds_prof_active: transition-log gate (gputrace) */

extern int gdx_rival_detail_floor(void); /* gdx3ds_menu.c: the user's [perf] rival_detail */

#define GDX_DYNLOD_TIER_MAX 2
#define GDX_DYNLOD_RAISE_FRAMES 4
#define GDX_DYNLOD_DWELL_FRAMES 8

static int sInited = 0;
static int sAuto = 1;             /* [perf] rival_detail_auto (menu writes, task thread reads) */
static int sVerbose = 0;          /* [debug] verbose: transition lines are census-gated */
static int sHiX10 = 150;          /* raise when the 4-frame average exceeds this (tenths of ms) */
static int sLoX10 = 120;          /* lower after sLowerFrames consecutive frames below this */
static int sLowerFrames = 30;

static volatile int sTier = 0;    /* controller tier; effective = max(floor, sTier) */
static u64 sTaskT0 = 0;
static int sTaskOpen = 0;
static u32 sHistX10[GDX_DYNLOD_RAISE_FRAMES]; /* last N task walls, tenths of ms */
static unsigned sHistN = 0;
static unsigned sHistAt = 0;
static u32 sLowSumX10 = 0;        /* block average over sLowerFrames frames for the LOWER test */
static unsigned sLowCount = 0;
static unsigned sSinceChange = GDX_DYNLOD_DWELL_FRAMES;

/* Receipt counters: window (reset on drain) + cumulative. */
static unsigned sWinFrames = 0;
static u64 sWinTicks = 0;
static u64 sWinMaxTicks = 0;
static unsigned sWinRaises = 0, sWinLowers = 0;
static unsigned sTotRaises = 0, sTotLowers = 0;

static void DynLodLog(const char* msg) {
    svcOutputDebugString(msg, strlen(msg));
    gdx3ds_filelog_write(msg, strlen(msg));
}

static int ClampInt(int v, int lo, int hi) {
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

void gdx3ds_dynlod_init(void) {
    if (sInited) {
        return;
    }
    sInited = 1;
    sAuto = gdx3ds_config_get_bool("perf", "rival_detail_auto", 1) ? 1 : 0;
    sVerbose = gdx3ds_config_get_bool("debug", "verbose", 0) ? 1 : 0;
    sHiX10 = ClampInt(gdx3ds_config_get_int("perf", "rival_auto_hi_x10", 150), 10, 1000);
    sLoX10 = ClampInt(gdx3ds_config_get_int("perf", "rival_auto_lo_x10", 120), 5, sHiX10);
    sLowerFrames = ClampInt(gdx3ds_config_get_int("perf", "rival_auto_lower_frames", 30), 1, 600);
    sTier = ClampInt(gdx_rival_detail_floor(), 0, GDX_DYNLOD_TIER_MAX);
    char line[128];
    snprintf(line, sizeof(line), "[dynlod] init auto=%d floor=%d hi=%d.%d lo=%d.%d lowerN=%d raiseN=%d dwell=%d",
             sAuto, sTier, sHiX10 / 10, sHiX10 % 10, sLoX10 / 10, sLoX10 % 10, sLowerFrames,
             GDX_DYNLOD_RAISE_FRAMES, GDX_DYNLOD_DWELL_FRAMES);
    DynLodLog(line);
}

int gdx3ds_dynlod_auto_enabled(void) {
    return sAuto;
}

void gdx3ds_dynlod_set_auto(int on) {
    sAuto = on ? 1 : 0;
    if (!sAuto) {
        sTier = 0; /* effective level collapses to the floor immediately */
        sHistN = 0;
        sLowSumX10 = 0;
        sLowCount = 0;
    }
}

int gdx3ds_dynlod_effective_level(int floorLevel) {
    if (!sAuto) {
        return floorLevel;
    }
    const int tier = sTier;
    return tier > floorLevel ? tier : floorLevel;
}

int gdx3ds_dynlod_tier(void) {
    return sAuto ? sTier : 0;
}

void gdx3ds_dynlod_task_begin(void) {
    if (!sAuto) {
        return;
    }
    if (!sInited) {
        gdx3ds_dynlod_init();
    }
    sTaskT0 = svcGetSystemTick();
    sTaskOpen = 1;
}

static void DynLodLogTransition(const char* dir, int from, int to, unsigned msX10) {
    if (!sVerbose && gdx3ds_prof_active == 0) {
        return;
    }
    char line[96];
    snprintf(line, sizeof(line), "[dynlod] %s %d->%d ms=%u.%u floor=%d", dir, from, to, msX10 / 10,
             msX10 % 10, gdx_rival_detail_floor());
    DynLodLog(line);
}

static unsigned HistAverageX10(void) {
    u32 sum = 0;
    for (unsigned i = 0; i < GDX_DYNLOD_RAISE_FRAMES; i++) {
        sum += sHistX10[i];
    }
    return sum / GDX_DYNLOD_RAISE_FRAMES;
}

static void DynLodStep(unsigned msX10) {
    const int floorLevel = ClampInt(gdx_rival_detail_floor(), 0, GDX_DYNLOD_TIER_MAX);
    int tier = sTier;
    if (tier < floorLevel) {
        tier = floorLevel; /* the user raised the floor above us */
    }
    sHistX10[sHistAt] = msX10;
    sHistAt = (sHistAt + 1u) % GDX_DYNLOD_RAISE_FRAMES;
    if (sHistN < GDX_DYNLOD_RAISE_FRAMES) {
        sHistN++;
    }
    sSinceChange++;

    /* LOWER test: the average of a whole sLowerFrames-frame block below lo (a block, not a
       run of consecutive frames — per-frame jitter of +-2 ms around the threshold would
       otherwise keep the tier stuck at the ceiling forever). */
    sLowSumX10 += msX10;
    sLowCount++;
    int blockLow = 0;
    if (sLowCount >= (unsigned)sLowerFrames) {
        blockLow = (sLowSumX10 / sLowCount) < (unsigned)sLoX10;
        sLowSumX10 = 0;
        sLowCount = 0;
    }

    if (sHistN >= GDX_DYNLOD_RAISE_FRAMES && sSinceChange >= GDX_DYNLOD_DWELL_FRAMES &&
        tier < GDX_DYNLOD_TIER_MAX && HistAverageX10() > (unsigned)sHiX10) {
        DynLodLogTransition("raise", tier, tier + 1, msX10);
        tier++;
        sWinRaises++;
        sTotRaises++;
        sSinceChange = 0;
        sLowSumX10 = 0;
        sLowCount = 0;
    } else if (blockLow && tier > floorLevel) {
        DynLodLogTransition("lower", tier, tier - 1, msX10);
        tier--;
        sWinLowers++;
        sTotLowers++;
        sSinceChange = 0;
    }
    sTier = tier;
}

void gdx3ds_dynlod_task_end(void) {
    if (!sAuto || !sTaskOpen) {
        return;
    }
    sTaskOpen = 0;
    const u64 dt = svcGetSystemTick() - sTaskT0;
    sWinFrames++;
    sWinTicks += dt;
    if (dt > sWinMaxTicks) {
        sWinMaxTicks = dt;
    }
    const unsigned msX10 = (unsigned)((dt * 10u) / (u64)CPU_TICKS_PER_MSEC);
    DynLodStep(msX10);
}

int gdx3ds_dynlod_receipt(char* buf, int cap) {
    if (buf == NULL || cap <= 0) {
        return 0;
    }
    const unsigned n = sWinFrames != 0 ? sWinFrames : 1u;
    const unsigned avgX10 = (unsigned)((sWinTicks * 10u) / ((u64)CPU_TICKS_PER_MSEC * n));
    const unsigned maxX10 = (unsigned)((sWinMaxTicks * 10u) / (u64)CPU_TICKS_PER_MSEC);
    const int len = snprintf(buf, (size_t)cap,
                             "[dynlod] auto=%d floor=%d tier=%d raises=%u/%u lowers=%u/%u ms=%u.%u max=%u.%u n=%u "
                             "hi=%u.%u lo=%u.%u",
                             sAuto, gdx_rival_detail_floor(), gdx3ds_dynlod_tier(), sWinRaises, sTotRaises,
                             sWinLowers, sTotLowers, avgX10 / 10, avgX10 % 10, maxX10 / 10, maxX10 % 10,
                             sWinFrames, (unsigned)sHiX10 / 10, (unsigned)sHiX10 % 10, (unsigned)sLoX10 / 10,
                             (unsigned)sLoX10 % 10);
    sWinFrames = 0;
    sWinTicks = 0;
    sWinMaxTicks = 0;
    sWinRaises = 0;
    sWinLowers = 0;
    return len > 0 ? len : 0;
}
