/* port/3ds/gdx3ds_hwprof.c -- hwprof instruments: [c5pmu] (CP15 counters per [c5cat] bucket) and
 * [hwsite] (per-call micro-timers at the hardware suspects). The contract, the receipts and the
 * pair table are in gdx3ds_hwprof.h; the evidence they were built for is in
 * docs/research/native/hwprof-progress.md. Zero heap: every table below is static. */
#include "gdx3ds_hwprof.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <3ds.h>

#include "gdx3ds_config.h"
#include "gdx3ds_filelog.h"
#include "gdx3ds_mainprof.h"

/* One extra slot past the [mprof] buckets: a site reached off the logical main thread (the render
 * thread under renderthread=1; never on the Old 3DS). */
#define HWP_OFFMAIN GDX3DS_MPROF_COUNT
#define HWP_SLOTS (GDX3DS_MPROF_COUNT + 1)

static void HwpLog(const char* fmt, ...) {
    char buf[480];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) {
        return;
    }
    const size_t len = (size_t)n < sizeof(buf) - 1 ? (size_t)n : sizeof(buf) - 1;
    svcOutputDebugString(buf, len);
    gdx3ds_filelog_write(buf, len);
}

static float TicksToUs(uint64_t t) {
    return (float)((double)t / CPU_TICKS_PER_USEC);
}

/* Short names of every [mprof] bucket (gdx3ds_mainprof.h order) plus the off-main slot. */
static const char* const kBucketName[HWP_SLOTS] = {
    "logic", "dlb", "brm", "aud", "lseg", "sla", "seg", "dma", "stask", "sub",
    "fbeg", "misc", "sky", "trk", "dress", "mach", "mesh", "shd", "fx", "hud", "mir",
    "apt", "pace", "inp", "beg", "vi", "disp", "jdp", "jtop", "end", "tail", "swp",
    "oth", "offmain",
};
_Static_assert(GDX3DS_MPROF_COUNT == 33, "kBucketName follows the [mprof] bucket enum");

static float sTickReadUs = 0.0f;

static void CalibrateTickRead(void) {
    enum { kReads = 256 };
    const uint64_t t0 = svcGetSystemTick();
    for (int i = 0; i < kReads; i++) {
        (void)svcGetSystemTick();
    }
    sTickReadUs = TicksToUs(svcGetSystemTick() - t0) / (float)kReads;
}

/* ============================================================================================
 * [c5pmu]
 * ============================================================================================ */

int gdx3ds_c5pmu_on = 0;

enum { kPmuOff = 0, kPmuCounters = 1, kPmuDry = 2, kPmuPairs = 6 };

typedef struct PmuPair {
    PerfCounterEvent ev[2];
    const char* name[2];
} PmuPair;

/* Cycle counts are not in the table: the [c5cat] clock (svcGetSystemTick, 268 MHz = the core
 * clock) already gives each bucket's cycles, and CCNT belongs to the kernel. */
static const PmuPair kPmuPair[kPmuPairs] = {
    { { PERFCOUNTEREVT_CORE_INST_EXECUTED, PERFCOUNTEREVT_CORE_INST_CACHE_MISS }, { "inst", "imiss" } },
    { { PERFCOUNTEREVT_CORE_STALL_BY_LACK_OF_INST, PERFCOUNTEREVT_CORE_STALL_BY_DATA_HAZARD },
      { "stall_if", "stall_dh" } },
    { { PERFCOUNTEREVT_CORE_DATA_CACHE_READ_MISS, PERFCOUNTEREVT_CORE_DATA_CACHE_WRITE_MISS },
      { "drmiss", "dwmiss" } },
    { { PERFCOUNTEREVT_CORE_EXTERNAL_REQUEST, PERFCOUNTEREVT_CORE_STALL_BY_LSU_FULL }, { "ext", "stall_lsu" } },
    { { PERFCOUNTEREVT_CORE_MAIN_TLB_MISS, PERFCOUNTEREVT_CORE_DATA_CACHE_LINE_EVICTION }, { "tlbmiss", "evict" } },
    { { PERFCOUNTEREVT_CORE_BRANCH_MISS_PREDICTED, PERFCOUNTEREVT_CORE_STORE_BUFFER_DRAIN },
      { "brmiss", "sbdrain" } },
};

static int sPmuMode = kPmuOff;
static int sPmuPinned = -1;
static int sPmuPair = 0;
static int sPmuKernelLocked = 0;
static uint32_t sPmuLast[2];
static uint64_t sPmuEv[HWP_SLOTS][2];
static uint32_t sPmuReads[HWP_SLOTS];
static float sPmuCal[2];
static float sPmuCalUs = 0.0f;

static uint32_t PmuRead(int counter) {
    if (sPmuMode == kPmuDry) {
        const uint64_t t = svcGetSystemTick();
        return counter == 0 ? (uint32_t)t : (uint32_t)(t >> 3);
    }
    u64 value = 0;
    svcControlPerformanceCounter(&value, PERFCOUNTEROP_GET_VALUE,
                                 counter == 0 ? PERFCOUNTERREG_CORE_COUNT_REG_0 : PERFCOUNTERREG_CORE_COUNT_REG_1, 0);
    return (uint32_t)value;
}

void gdx3ds_c5pmu_charge(int bucket) {
    const uint32_t a = PmuRead(0);
    const uint32_t b = PmuRead(1);
    if (bucket < 0 || bucket >= GDX3DS_MPROF_COUNT) {
        bucket = GDX3DS_MPROF_OTH;
    }
    sPmuEv[bucket][0] += (uint32_t)(a - sPmuLast[0]);
    sPmuEv[bucket][1] += (uint32_t)(b - sPmuLast[1]);
    sPmuReads[bucket]++;
    sPmuLast[0] = a;
    sPmuLast[1] = b;
}

void gdx3ds_c5pmu_rebase(void) {
    if (!gdx3ds_c5pmu_on) {
        return;
    }
    sPmuLast[0] = PmuRead(0);
    sPmuLast[1] = PmuRead(1);
}

static int PmuSelect(int pair) {
    if (sPmuMode == kPmuDry) {
        return 1;
    }
    u64 out = 0;
    const Result r0 = svcControlPerformanceCounter(&out, PERFCOUNTEROP_SET_EVENT, PERFCOUNTERREG_CORE_COUNT_REG_0,
                                                   kPmuPair[pair].ev[0]);
    const Result r1 = svcControlPerformanceCounter(&out, PERFCOUNTEROP_SET_EVENT, PERFCOUNTERREG_CORE_COUNT_REG_1,
                                                   kPmuPair[pair].ev[1]);
    return R_SUCCEEDED(r0) && R_SUCCEEDED(r1);
}

/* What one bucket switch adds to each counter by itself: two counter reads and the clock read,
 * back to back. Measured at every pair switch, because the cost differs per event. */
static void PmuCalibrate(void) {
    enum { kReps = 32 };
    const uint32_t a0 = PmuRead(0);
    const uint32_t a1 = PmuRead(1);
    const uint64_t t0 = svcGetSystemTick();
    for (int i = 0; i < kReps; i++) {
        (void)PmuRead(0);
        (void)PmuRead(1);
        (void)svcGetSystemTick();
    }
    const uint64_t t1 = svcGetSystemTick();
    const uint32_t b0 = PmuRead(0);
    const uint32_t b1 = PmuRead(1);
    sPmuCal[0] = (float)(uint32_t)(b0 - a0) / (float)(kReps + 1);
    sPmuCal[1] = (float)(uint32_t)(b1 - a1) / (float)(kReps + 1);
    sPmuCalUs = TicksToUs(t1 - t0) / (float)kReps;
}

static void PmuAtExit(void) {
    if (sPmuKernelLocked) {
        u64 out = 0;
        svcControlPerformanceCounter(&out, PERFCOUNTEROP_DISABLE, 0, 0);
        sPmuKernelLocked = 0;
    }
}

/* A spin with a known instruction count: the counters must see it. */
static uint32_t PmuSanitySpin(void) {
    volatile uint32_t sink = 0;
    for (uint32_t i = 0; i < 20000u; i++) {
        sink += i;
    }
    return sink;
}

static void PmuLatch(int mode, int pinned) {
    sPmuMode = mode;
    sPmuPinned = (pinned >= 0 && pinned < kPmuPairs) ? pinned : -1;
    sPmuPair = sPmuPinned >= 0 ? sPmuPinned : 0;
    if (mode == kPmuCounters) {
        u64 out = 0;
        const Result rc = svcControlPerformanceCounter(&out, PERFCOUNTEROP_ENABLE, 0, 0);
        if (R_FAILED(rc)) {
            HwpLog("[c5pmu] unavailable: svcControlPerformanceCounter(ENABLE) rc=0x%08lX (kernel or exheader "
                   "refuses the perfmon); [c5pmu] off", (unsigned long)rc);
            sPmuMode = kPmuOff;
            return;
        }
        sPmuKernelLocked = 1;
        atexit(PmuAtExit);
        if (!PmuSelect(0)) {
            HwpLog("[c5pmu] unavailable: SET_EVENT refused; [c5pmu] off");
            PmuAtExit();
            sPmuMode = kPmuOff;
            return;
        }
        const uint32_t a0 = PmuRead(0);
        const uint32_t a1 = PmuRead(1);
        (void)PmuSanitySpin();
        const uint32_t d0 = PmuRead(0) - a0;
        const uint32_t d1 = PmuRead(1) - a1;
        /* ~20000 loop iterations of 5-6 instructions each: inst must land in [40k, 4M]. An
         * unimplemented svc (the emulator) returns its inputs and reads a constant: d0 = 0. */
        if (d0 < 40000u || d0 > 4000000u) {
            HwpLog("[c5pmu] unavailable: counters do not count (inst=%lu imiss=%lu over a 20000-iteration spin); "
                   "[c5pmu] off", (unsigned long)d0, (unsigned long)d1);
            PmuAtExit();
            sPmuMode = kPmuOff;
            return;
        }
        HwpLog("[c5pmu] sanity spin: inst=%lu imiss=%lu (20000 iterations)", (unsigned long)d0, (unsigned long)d1);
    }
    if (!PmuSelect(sPmuPair)) {
        HwpLog("[c5pmu] unavailable: SET_EVENT pair %d refused; [c5pmu] off", sPmuPair);
        PmuAtExit();
        sPmuMode = kPmuOff;
        return;
    }
    PmuCalibrate();
    memset(sPmuEv, 0, sizeof(sPmuEv));
    memset(sPmuReads, 0, sizeof(sPmuReads));
    gdx3ds_c5pmu_on = 1;
    gdx3ds_c5pmu_rebase();
    HwpLog("[c5pmu] init mode=%d%s pairs=%s cal=%.1f,%.1f/read read=%.2fus (debug.c5pmu; one event pair per "
           "[c5cat] window, k-events per tick / per rendered frame)",
           mode, mode == kPmuDry ? " (DRY RUN: fake counters from the tick)" : "",
           sPmuPinned >= 0 ? kPmuPair[sPmuPinned].name[0] : "rotate", (double)sPmuCal[0], (double)sPmuCal[1],
           (double)sPmuCalUs);
}

static float PmuCorr(int slot, int k) {
    const float raw = (float)sPmuEv[slot][k];
    const float c = raw - (float)sPmuReads[slot] * sPmuCal[k];
    return c > 0.0f ? c : 0.0f;
}

static float PmuSum(const int* slots, int count, int k) {
    float s = 0.0f;
    for (int i = 0; i < count; i++) {
        s += PmuCorr(slots[i], k);
    }
    return s;
}

static int PmuAppend(char* buf, int at, int cap, const char* name, float a, float b, float inv) {
    if (at >= cap) {
        return at;
    }
    const int n = snprintf(buf + at, (size_t)(cap - at), " %s=%.1f,%.1f", name, (double)(a * inv / 1000.0f),
                           (double)(b * inv / 1000.0f));
    return n > 0 ? at + n : at;
}

static void PmuEmit(unsigned long frame, unsigned n, unsigned r) {
    static const int kOthTick[] = {
        GDX3DS_MPROF_BRM,  GDX3DS_MPROF_AUD, GDX3DS_MPROF_LSEG, GDX3DS_MPROF_SLA,  GDX3DS_MPROF_SEG,
        GDX3DS_MPROF_DMA,  GDX3DS_MPROF_STASK, GDX3DS_MPROF_SUB, GDX3DS_MPROF_APT, GDX3DS_MPROF_INP,
        GDX3DS_MPROF_VI,   GDX3DS_MPROF_DISP, GDX3DS_MPROF_JDP, GDX3DS_MPROF_JTOP, GDX3DS_MPROF_TAIL,
    };
    static const int kFbeg[] = { GDX3DS_MPROF_BEG, GDX3DS_MPROF_R_FBEG };
    static const struct {
        const char* name;
        int slot;
    } kRender[] = {
        { "misc", GDX3DS_MPROF_R_MISC }, { "sky", GDX3DS_MPROF_R_SKY },   { "trk", GDX3DS_MPROF_R_TRK },
        { "dress", GDX3DS_MPROF_R_DRESS }, { "mach", GDX3DS_MPROF_R_MACH }, { "mesh", GDX3DS_MPROF_R_MESH },
        { "shd", GDX3DS_MPROF_R_SHD },   { "fx", GDX3DS_MPROF_R_FX },     { "hud", GDX3DS_MPROF_R_HUD },
        { "mir", GDX3DS_MPROF_R_MIRR },  { "swp", GDX3DS_MPROF_R_SWAP },  { "fend", GDX3DS_MPROF_END },
    };
    const float invN = n != 0 ? 1.0f / (float)n : 0.0f;
    const float invR = r != 0 ? 1.0f / (float)r : 0.0f;
    unsigned long reads = 0;
    for (int i = 0; i < HWP_SLOTS; i++) {
        reads += sPmuReads[i];
    }
    const PmuPair* p = &kPmuPair[sPmuPair];
    char buf[480];
    const int cap = (int)sizeof(buf);
    int at = snprintf(buf, sizeof(buf), "[c5pmu] frame=%lu n=%u r=%u pair=%d ev=%s,%s cal=%.1f,%.1f rd/t=%.1f |",
                      frame, n, r, sPmuPair, p->name[0], p->name[1], (double)sPmuCal[0], (double)sPmuCal[1],
                      (double)((float)reads * invN));
    at = PmuAppend(buf, at, cap, "logic", PmuCorr(GDX3DS_MPROF_LOGIC, 0), PmuCorr(GDX3DS_MPROF_LOGIC, 1), invN);
    at = PmuAppend(buf, at, cap, "dlb", PmuCorr(GDX3DS_MPROF_DLB, 0), PmuCorr(GDX3DS_MPROF_DLB, 1), invN);
    const int nOth = (int)(sizeof(kOthTick) / sizeof(kOthTick[0]));
    at = PmuAppend(buf, at, cap, "oth", PmuSum(kOthTick, nOth, 0), PmuSum(kOthTick, nOth, 1), invN);
    if (at < cap) {
        at += snprintf(buf + at, (size_t)(cap - at), " |");
    }
    at = PmuAppend(buf, at, cap, "fbeg", PmuSum(kFbeg, 2, 0), PmuSum(kFbeg, 2, 1), invR);
    for (size_t i = 0; i < sizeof(kRender) / sizeof(kRender[0]); i++) {
        at = PmuAppend(buf, at, cap, kRender[i].name, PmuCorr(kRender[i].slot, 0), PmuCorr(kRender[i].slot, 1), invR);
    }
    if (at > 0) {
        const size_t len = at < cap ? (size_t)at : (size_t)cap - 1;
        svcOutputDebugString(buf, len);
        gdx3ds_filelog_write(buf, len);
    }
}

static void PmuWindowEnd(unsigned long frame, unsigned n, unsigned r) {
    PmuEmit(frame, n, r);
    memset(sPmuEv, 0, sizeof(sPmuEv));
    memset(sPmuReads, 0, sizeof(sPmuReads));
    if (sPmuPinned < 0) {
        const int next = (sPmuPair + 1) % kPmuPairs;
        if (PmuSelect(next)) {
            sPmuPair = next;
        }
        PmuCalibrate();
    }
    /* frame_begin rebases before the next window's first bucket opens. */
}

/* ============================================================================================
 * [hwsite]
 * ============================================================================================ */

int gdx3ds_hws_on = 0;
/* onesubmit [debug] hwsite_every = N (default 1): the sites are timed in one [c5cat] window of
 * every N (the others run the untimed call), so a card can carry [hwsite] at 1/N of its cost. */
static int sHwsArmed = 0;
static unsigned sHwsEvery = 1;
static unsigned sHwsWindow = 0;

static const char* const kSiteName[GDX3DS_HWS_COUNT] = {
    "draw", "texup", "import", "flush", "heapflush", "kick", "rplrec", "hudret", "hudrec",
};

static uint64_t sHwsTicks[GDX3DS_HWS_COUNT][HWP_SLOTS];
static uint32_t sHwsCalls[GDX3DS_HWS_COUNT][HWP_SLOTS];

uint64_t gdx3ds_hws_now(void) {
    return svcGetSystemTick();
}

void gdx3ds_hws_add(int site, uint64_t t0) {
    const uint64_t now = svcGetSystemTick();
    if (site < 0 || site >= GDX3DS_HWS_COUNT) {
        return;
    }
    int slot = GDX3DS_MPROF_OTH;
    if (!gdx3ds_mprof_on_main()) {
        slot = HWP_OFFMAIN;
    } else if (gdx3ds_mprof_active) {
        slot = gdx3ds_mprof_cur;
    }
    sHwsTicks[site][slot] += now - t0;
    sHwsCalls[site][slot]++;
}

/* hwprof2 [hwflush] + [debug] flush_svc (see the header). */
static const char* const kFlushKindName[GDX3DS_HWFL_COUNT] = {
    "vbo", "tex", "res", "swap", "heapend", "heapkick", "other",
};
static uint32_t sFlCalls[GDX3DS_HWFL_COUNT];
static uint32_t sFlSvcCalls[GDX3DS_HWFL_COUNT];
static uint64_t sFlTicks[GDX3DS_HWFL_COUNT];
static uint64_t sFlBytes[GDX3DS_HWFL_COUNT];
static int sFlushSvc = 0;

void gdx3ds_hws_gsp_flush_kind(const void* addr, uint32_t size, int kind) {
    const uint64_t t0 = gdx3ds_hws_begin();
    const int big = size >= (1u << 20);
    const int viaSvc = sFlushSvc && !big;
    if (viaSvc) {
        svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u32)addr, size);
    } else {
        GSPGPU_FlushDataCache(addr, size);
    }
    if (t0 == 0) {
        return;
    }
    if (kind < 0 || kind >= GDX3DS_HWFL_COUNT) {
        kind = GDX3DS_HWFL_OTHER;
    }
    sFlCalls[kind]++;
    sFlSvcCalls[kind] += (uint32_t)viaSvc;
    sFlBytes[kind] += size;
    sFlTicks[kind] += svcGetSystemTick() - t0;
    gdx3ds_hws_end(big ? GDX3DS_HWS_HEAPFLUSH : GDX3DS_HWS_FLUSH, t0);
}

void gdx3ds_hws_gsp_flush(const void* addr, uint32_t size) {
    gdx3ds_hws_gsp_flush_kind(addr, size, size >= (1u << 20) ? GDX3DS_HWFL_HEAPEND : GDX3DS_HWFL_OTHER);
}

static void FlushEmit(unsigned long frame, unsigned n, unsigned r) {
    const unsigned per = r != 0 ? r : n;
    const float inv = per != 0 ? 1.0f / (float)per : 0.0f;
    char buf[480];
    const int cap = (int)sizeof(buf);
    int at = snprintf(buf, sizeof(buf), "[hwflush] frame=%lu r=%u svc=%d |", frame, r, sFlushSvc);
    uint32_t total = 0;
    for (int k = 0; k < GDX3DS_HWFL_COUNT && at < cap; k++) {
        total += sFlCalls[k];
        if (sFlCalls[k] == 0) {
            continue;
        }
        const int w = snprintf(buf + at, (size_t)(cap - at), " %s=%.2f/%.0fus/%.0fKB/svc%.2f", kFlushKindName[k],
                               (double)((float)sFlCalls[k] * inv), (double)(TicksToUs(sFlTicks[k]) * inv),
                               (double)((float)sFlBytes[k] / 1024.0f * inv), (double)((float)sFlSvcCalls[k] * inv));
        if (w > 0) {
            at += w;
        }
    }
    if (total != 0) {
        const size_t len = at < cap ? (size_t)at : (size_t)cap - 1;
        svcOutputDebugString(buf, len);
        gdx3ds_filelog_write(buf, len);
    }
    memset(sFlCalls, 0, sizeof(sFlCalls));
    memset(sFlSvcCalls, 0, sizeof(sFlSvcCalls));
    memset(sFlTicks, 0, sizeof(sFlTicks));
    memset(sFlBytes, 0, sizeof(sFlBytes));
}

static void HwsEmit(unsigned long frame, unsigned n, unsigned r) {
    const unsigned per = r != 0 ? r : n;
    const float inv = per != 0 ? 1.0f / (float)per : 0.0f;
    for (int s = 0; s < GDX3DS_HWS_COUNT; s++) {
        uint64_t ticks = 0;
        uint32_t calls = 0;
        for (int b = 0; b < HWP_SLOTS; b++) {
            ticks += sHwsTicks[s][b];
            calls += sHwsCalls[s][b];
        }
        if (calls == 0) {
            continue;
        }
        char buf[480];
        const int cap = (int)sizeof(buf);
        int at = snprintf(buf, sizeof(buf), "[hwsite] frame=%lu r=%u site=%s n/f=%.1f us/f=%.0f ovh/f=%.0f |", frame,
                          r, kSiteName[s], (double)((float)calls * inv), (double)(TicksToUs(ticks) * inv),
                          (double)((float)calls * 2.0f * sTickReadUs * inv));
        for (int b = 0; b < HWP_SLOTS && at < cap; b++) {
            if (sHwsCalls[s][b] == 0) {
                continue;
            }
            const int k = snprintf(buf + at, (size_t)(cap - at), " %s=%.1f/%.0f", kBucketName[b],
                                   (double)((float)sHwsCalls[s][b] * inv), (double)(TicksToUs(sHwsTicks[s][b]) * inv));
            if (k > 0) {
                at += k;
            }
        }
        const size_t len = at < cap ? (size_t)at : (size_t)cap - 1;
        svcOutputDebugString(buf, len);
        gdx3ds_filelog_write(buf, len);
    }
    memset(sHwsTicks, 0, sizeof(sHwsTicks));
    memset(sHwsCalls, 0, sizeof(sHwsCalls));
}

/* ============================================================================================
 * mainprof hooks
 * ============================================================================================ */

int gdx3ds_hwprof_latch(void) {
    const int pmu = gdx3ds_config_get_int("debug", "c5pmu", 0);
    const int pinned = gdx3ds_config_get_int("debug", "c5pmu_pair", -1);
    const int hws = gdx3ds_config_get_int("debug", "hwsite", 0) != 0;
    sFlushSvc = gdx3ds_config_get_int("debug", "flush_svc", 0) != 0;
    if (sFlushSvc) {
        HwpLog("[hwflush] flush_svc=1: data-cache flushes under 1 MiB go through svcFlushProcessDataCache "
               "(not the GSPGPU IPC); whole-heap flushes unchanged");
    }
    if (pmu == 0 && !hws) {
        return 0;
    }
    CalibrateTickRead();
    if (pmu == kPmuCounters || pmu == kPmuDry) {
        PmuLatch(pmu, pinned);
    } else if (pmu != 0) {
        HwpLog("[c5pmu] c5pmu=%d unknown (1 = counters, 2 = dry run); off", pmu);
    }
    if (hws) {
        const int every = gdx3ds_config_get_int("debug", "hwsite_every", 1);
        sHwsEvery = every > 1 ? (unsigned)every : 1u;
        memset(sHwsTicks, 0, sizeof(sHwsTicks));
        memset(sHwsCalls, 0, sizeof(sHwsCalls));
        sHwsArmed = 1;
        gdx3ds_hws_on = 1;
        HwpLog("[hwsite] init sites=draw,texup,import,flush,heapflush,kick,rplrec,hudret,hudrec tick read %.2f us "
               "(debug.hwsite; per [c5cat] window, calls/us per rendered frame by open bucket; timed in 1 window "
               "of %u, debug.hwsite_every)",
               (double)sTickReadUs, sHwsEvery);
    }
    return 1;
}

void gdx3ds_hwprof_emit(unsigned long frame, unsigned iterations, unsigned rendered) {
    if (gdx3ds_c5pmu_on) {
        PmuWindowEnd(frame, iterations, rendered);
    }
    if (!sHwsArmed) {
        return;
    }
    if (gdx3ds_hws_on) {
        HwsEmit(frame, iterations, rendered);
        FlushEmit(frame, iterations, rendered);
    }
    sHwsWindow++;
    gdx3ds_hws_on = (sHwsWindow % sHwsEvery) == 0u ? 1 : 0; /* the next window's sampling */
}
