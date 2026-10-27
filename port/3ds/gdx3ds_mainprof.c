/* port/3ds/gdx3ds_mainprof.c — G-MAINPROF implementation. The contract, the bucket
 * semantics and the receipt format are all in gdx3ds_mainprof.h; read that first. */
#include "gdx3ds_mainprof.h"
#include "gdx3ds_cpucut.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <3ds.h>

#include "gdx3ds_config.h"
#include "gdx3ds_filelog.h"
#include "gdx3ds_hwprof.h"
#include "gfx/gdx3ds_ships.h"
#include "gfx/gdx3ds_prewrite.h"

/* Logical-main test (host thread or any game fiber it created). port/n64_sched.c. */
extern int gdx_sched_on_host_thread(void);
/* draws/frame of the last emitted [prof] window (port/3ds/gfx/gdx3ds_gpu_prof.c). 0 when
 * `[debug] gputrace=0`, in which case the dl= column is the load proxy instead. Read
 * racily from main; it is a scene descriptor, never a control input. */
extern unsigned gdx3ds_prof_last_nd;

/* N60LOG: the window is [debug] receipt_every iterations (default 64): every receipt of this family
 * is normalised by its own n= / r=, so a longer window only lowers the log volume. */
#define GDX3DS_MPROF_WINDOW ((unsigned)gdx3ds_filelog_receipt_every())

int gdx3ds_mprof_active = 0;
int gdx3ds_mprof_cur = GDX3DS_MPROF_OTH;
uint64_t gdx3ds_mprof_t0 = 0;
uint64_t gdx3ds_mprof_ticks[GDX3DS_MPROF_COUNT];
uint32_t gdx3ds_mprof_reads = 0;

static int sChecked = 0;
static int sEnabled = 0; /* the clock runs: `[debug] mainprof=1` OR `[debug] gputrace=1` */
static int sEmit = 0;    /* the [mprof] receipt is emitted: `[debug] mainprof=1` only */
/* C4I: host iterations folded since the clock armed. gdx3ds_mprof_snapshot hands it out so
 * the [oth] split can tell a C3D window that stayed inside one iteration (both snapshots
 * see the same generation, the per-frame tick array was not reset between them) from one
 * that did not. */
static unsigned long sGen = 1;
static uint64_t sWinTicks[GDX3DS_MPROF_COUNT];
static unsigned sWinFrames = 0;
static unsigned long sWinDlBytes = 0;
static unsigned long sWinTasks = 0;
/* C5 STAGE 0 [c5cat] window state (see the header). */
static unsigned sWinRendered = 0;
static uint64_t sWinT0 = 0;       /* first iteration's clock start */
static uint64_t sWinT1 = 0;       /* last iteration's clock end */
static uint64_t sWinReads = 0;
static int sRtOff = 1;            /* 0 when some render sub-bucket was refused (renderthread) */
static float sTickReadUs = 0.0f;  /* one svcGetSystemTick, µs, calibrated at arming */
/* [debug] c5steal: the core-0 steal probe (below), DEFAULT 0. */
static int sStealProbe = 0;
typedef struct C5StealResult {
    float stolenPct; /* of the probe's wall */
    unsigned gaps;   /* preemptions seen */
    float maxGapUs;
} C5StealResult;

long long gdx3ds_mprof_now(void) {
    return (long long)svcGetSystemTick();
}

int gdx3ds_mprof_on_main(void) {
    return gdx_sched_on_host_thread();
}

int gdx3ds_mprof_enabled(void) {
    return sEmit;
}

/* C4I: flush the open bucket into gdx3ds_mprof_ticks (re-entering the same bucket closes
 * and reopens it at `now`, so the array is exact as of this call and the timeline is not
 * disturbed), copy the per-bucket ticks out, and return this iteration's generation. 0 when
 * the clock is off or the caller is not the logical main thread (render-thread C3D frames,
 * renderthread != 0): the [oth] split then reports the frame as unsplit. */
unsigned long gdx3ds_mprof_snapshot(uint64_t* out, int count) {
    int i;
    if (!gdx3ds_mprof_active || count != GDX3DS_MPROF_COUNT || !gdx3ds_mprof_on_main()) {
        return 0;
    }
    (void)gdx3ds_mprof_enter(gdx3ds_mprof_cur);
    for (i = 0; i < GDX3DS_MPROF_COUNT; i++) {
        out[i] = gdx3ds_mprof_ticks[i];
    }
    return sGen;
}

/* ------------------------------------------------------------------------------------------
 * [c5cat] entry points. The category -> bucket map is a dense table; 0 and out-of-range ids
 * (a marker from a newer decomp patch) land in R_MISC rather than being dropped. */
static const int kC5CatBucket[GDX3DS_C5CAT_COUNT] = {
    GDX3DS_MPROF_R_MISC,  /* 0: never emitted */
    GDX3DS_MPROF_R_MISC,  GDX3DS_MPROF_R_SKY, GDX3DS_MPROF_R_TRK, GDX3DS_MPROF_R_DRESS,
    GDX3DS_MPROF_R_MACH,  GDX3DS_MPROF_R_MESH, GDX3DS_MPROF_R_SHD, GDX3DS_MPROF_R_FX,
    GDX3DS_MPROF_R_HUD,   GDX3DS_MPROF_R_FBEG, GDX3DS_MPROF_R_MIRR, GDX3DS_MPROF_R_SWAP,
};

static int C5CatBucket(int cat) {
    if (cat <= 0 || cat >= GDX3DS_C5CAT_COUNT) {
        return GDX3DS_MPROF_R_MISC;
    }
    return kC5CatBucket[cat];
}

int gdx3ds_c5cat_armed(void) {
    return gdx3ds_mprof_active;
}

/* [debug] c5sub=1 (C5 Stage 3b): the decomp writes its p=0x62 sub-category markers only on top
 * of an armed [c5cat] clock. DEFAULT 0. */
static int sC5Sub = 0;

int gdx3ds_c5sub_armed(void) {
    return gdx3ds_mprof_active && sC5Sub;
}

int gdx3ds_c5cat_enter(int cat) {
    int prev;
    GDX3DS_PW_POLL(); /* o3ds3d [prewrite]: a raw command-buffer write since the last poll */
    if (!gdx3ds_mprof_active) {
        return -1;
    }
    prev = gdx3ds_mprof_enter_safe(C5CatBucket(cat));
    if (prev < 0) {
        sRtOff = 0; /* armed but off main: the walk runs on the render thread */
    }
    return prev;
}

void gdx3ds_c5cat_leave(int prev) {
    gdx3ds_mprof_leave(prev);
}

void gdx3ds_c5cat_marker(uint32_t w0) {
    (void)gdx3ds_c5cat_enter((int)(w0 & 0xFFu));
}

/* svc + filelog, exactly like GpuProfLog: retail hardware DISCARDS
 * svcOutputDebugString, so the filelog is the only sink a hardware run can show. */
static void MProfLog(const char* fmt, ...) {
    char buf[224];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
        size_t len = (size_t)n < sizeof(buf) - 1 ? (size_t)n : sizeof(buf) - 1;
        svcOutputDebugString(buf, len);
        gdx3ds_filelog_write(buf, len);
    }
}

static float TicksToMs(uint64_t dt) {
    return (float)((double)dt / CPU_TICKS_PER_MSEC);
}

/* Decomp frame-function checkpoint. One switch over a small dense id; the gate is tested
 * inside gdx3ds_mprof_enter, so an unarmed run pays a call, a load and a branch. */
void gdx3ds_mprof_ck(int step) {
    int bucket;
    if (!gdx3ds_mprof_active) {
        return;
    }
    switch (step) {
    case GDX3DS_MPROF_CK_AUDIO:   bucket = GDX3DS_MPROF_AUD; break;
    case GDX3DS_MPROF_CK_LOGIC:   bucket = GDX3DS_MPROF_LOGIC; break;
    case GDX3DS_MPROF_CK_LSEG:    bucket = GDX3DS_MPROF_LSEG; break;
    case GDX3DS_MPROF_CK_DLBUILD: bucket = GDX3DS_MPROF_DLB; break;
    /* Gfx_FullSync + the DP-done park: the park returns to the host, which re-opens its
     * own bucket at once, so this only ever charges the FullSync itself. Folded into
     * stask rather than given a column nobody would read. */
    case GDX3DS_MPROF_CK_DPWAIT:  bucket = GDX3DS_MPROF_STASK; break;
    case GDX3DS_MPROF_CK_SEGLOAD: bucket = GDX3DS_MPROF_SLA; break;
    /* The framebuffer spin yields to the host on its first miss (n64_sched.c gdx_yield),
     * so like the DP park it charges only the first re-check. */
    case GDX3DS_MPROF_CK_FBSPIN:  bucket = GDX3DS_MPROF_SLA; break;
    case GDX3DS_MPROF_CK_SETTASK: bucket = GDX3DS_MPROF_STASK; break;
    case GDX3DS_MPROF_CK_IDLE:
    default:                      bucket = GDX3DS_MPROF_OTH; break;
    }
    (void)gdx3ds_mprof_enter(bucket);
}

void gdx3ds_mprof_note_task(unsigned long dlBytes) {
    if (!gdx3ds_mprof_active) {
        return;
    }
    sWinDlBytes += dlBytes;
    sWinTasks++;
}

/* Latch the ini gate once, after the config is loaded (same boot-order-proof shape as
 * GpuProfEnsureInit). mainprof is its own key: a main-thread profile must be runnable
 * with gputrace off. C4I: gputrace=1 ALSO runs the clock (never the receipt), because the
 * [oth] split in gdx3ds_gpu_prof.c names the C3D window's non-interpreter time in these
 * buckets. What a gputrace run pays for it: ~25 tick reads per host iteration. */
/* One svcGetSystemTick in µs (the [othx] tick= method): the unit of the instrument's own cost. */
static void CalibrateTickRead(void) {
    enum { kReads = 256 };
    const uint64_t t0 = svcGetSystemTick();
    int i;
    for (i = 0; i < kReads; i++) {
        (void)gdx3ds_mprof_now();
    }
    sTickReadUs = TicksToMs(svcGetSystemTick() - t0) * 1000.0f / (float)kReads;
}

/* [debug] c5steal=1: how much of core 0 another thread takes from main. Main spins on the clock
 * for kStealProbeMs between two windows (outside every window's wall, so no column moves) and
 * counts every read-to-read gap longer than a preemption threshold: while main spins, a gap is
 * time the kernel gave to a higher-priority core-0 thread (libctru's GSP event and ndsp threads,
 * the watchdog, the drain thread if it landed on core 0) or to interrupt handling. It samples
 * the between-frames phase only, which is when the GPU finishes the frame just submitted (its
 * P3D/PPF completions wake the GSP event thread), so it is a lower bound on in-frame steal. */
enum { kStealProbeMs = 8 };

static C5StealResult C5StealProbe(void) {
    C5StealResult r = { 0.0f, 0, 0.0f };
    const float thrUs = sTickReadUs * 8.0f > 10.0f ? sTickReadUs * 8.0f : 10.0f;
    const uint64_t thr = (uint64_t)(thrUs * (float)CPU_TICKS_PER_USEC);
    const uint64_t t0 = svcGetSystemTick();
    const uint64_t end = t0 + (uint64_t)(kStealProbeMs * CPU_TICKS_PER_MSEC);
    uint64_t prev = t0;
    uint64_t stolen = 0;
    uint64_t maxGap = 0;
    while (prev < end) {
        const uint64_t now = svcGetSystemTick();
        const uint64_t gap = now - prev;
        if (gap > thr) {
            stolen += gap;
            r.gaps++;
            if (gap > maxGap) {
                maxGap = gap;
            }
        }
        prev = now;
    }
    r.stolenPct = 100.0f * (float)stolen / (float)(prev - t0);
    r.maxGapUs = (float)maxGap / (float)CPU_TICKS_PER_USEC;
    return r;
}

static void MProfEnsureInit(void) {
    if (sChecked) {
        return;
    }
    if (gdx3ds_config_loaded() == 0) {
        return; /* config not in yet — latch at a later frame, never as 0 by accident */
    }
    sChecked = 1;
    sEmit = gdx3ds_config_get_int("debug", "mainprof", 0) != 0;
    sEnabled = sEmit || gdx3ds_config_get_int("debug", "gputrace", 0) != 0;
    /* hwprof: [debug] c5pmu / hwsite ride this clock and arm it by themselves (no gputrace). */
    if (gdx3ds_hwprof_latch()) {
        sEnabled = 1;
    }
    if (sEnabled) {
        sStealProbe = gdx3ds_config_get_int("debug", "c5steal", 0) != 0;
        sC5Sub = gdx3ds_config_get_int("debug", "c5sub", 0) != 0;
        if (sC5Sub) {
            MProfLog("[c5sub] sub-category census armed (debug.c5sub=1): decomp p=0x%02X markers split "
                     "dress= and fx= by piece; one [c5sub] line per [c5cat] window", GDX3DS_C5SUB_MARKER_P);
        }
        CalibrateTickRead();
        MProfLog("[c5cat] per-category timers armed (window=%u iterations, tick read %.2f us; "
                 "decomp p=0x%02X markers written; core-0 steal probe %s)", GDX3DS_MPROF_WINDOW,
                 (double)sTickReadUs, GDX3DS_C5CAT_MARKER_P,
                 sStealProbe ? "ON, 8 ms/window (debug.c5steal=1)" : "off");
    }
    if (sEmit) {
        MProfLog("[mprof] main-thread profiling enabled (window=%u frames; diagnostic only)",
                 GDX3DS_MPROF_WINDOW);
    } else if (sEnabled) {
        MProfLog("[mprof] clock armed for the [oth] split (debug.gputrace=1); no [mprof] "
                 "receipt without debug.mainprof=1");
    }
}

void gdx3ds_mprof_frame_begin(void) {
    MProfEnsureInit();
    if (!sEnabled) {
        return;
    }
    /* Armed between frames only, so gdx3ds_mprof_cur can never flip mid-bucket. */
    if (!gdx3ds_mprof_active) {
        gdx3ds_mprof_active = 1;
        memset(gdx3ds_mprof_ticks, 0, sizeof(gdx3ds_mprof_ticks));
        gdx3ds_mprof_cur = GDX3DS_MPROF_OTH;
    }
    gdx3ds_c5pmu_rebase(); /* hwprof [c5pmu]: the gap between frames is charged to nobody */
    gdx3ds_mprof_t0 = (uint64_t)gdx3ds_mprof_now();
    gdx3ds_mprof_cur = GDX3DS_MPROF_APT; /* the loop's first real section */
    if (sWinFrames == 0) {
        sWinT0 = gdx3ds_mprof_t0;
    }
}

static void MProfEmit(unsigned long frame, unsigned n) {
    const float inv = 1.0f / (float)n;
    float ms[GDX3DS_MPROF_COUNT];
    float fib = 0.0f;
    float host = 0.0f;
    int i;
    for (i = 0; i < GDX3DS_MPROF_COUNT; i++) {
        ms[i] = TicksToMs(sWinTicks[i]) * inv;
        if (i < GDX3DS_MPROF_FIB_END) {
            fib += ms[i];
        } else if (i != GDX3DS_MPROF_OTH) {
            host += ms[i];
        }
    }
    MProfLog("[mprof] frame=%lu n=%u part=1 nD=%u dl=%.1f tot=%.2f fib=%.2f host=%.2f | "
             "logic=%.2f dlb=%.2f brm=%.2f aud=%.2f lseg=%.2f sla=%.2f seg=%.2f dma=%.2f "
             "stask=%.2f sub=%.2f",
             frame, n, gdx3ds_prof_last_nd,
             (double)((float)sWinDlBytes * inv / 1024.0f),
             (double)(fib + host + ms[GDX3DS_MPROF_OTH]), (double)fib, (double)host,
             (double)ms[GDX3DS_MPROF_LOGIC], (double)ms[GDX3DS_MPROF_DLB],
             (double)ms[GDX3DS_MPROF_BRM], (double)ms[GDX3DS_MPROF_AUD],
             (double)ms[GDX3DS_MPROF_LSEG], (double)ms[GDX3DS_MPROF_SLA],
             (double)ms[GDX3DS_MPROF_SEG], (double)ms[GDX3DS_MPROF_DMA],
             (double)(TicksToMs(gdx3ds_mprof_stask_folded(sWinTicks)) * inv),
             (double)ms[GDX3DS_MPROF_SUB]);
    MProfLog("[mprof] frame=%lu n=%u part=2 tasks=%lu | apt=%.2f pace=%.2f inp=%.2f "
             "beg=%.2f vi=%.2f disp=%.2f jdp=%.2f jtop=%.2f end=%.2f tail=%.2f oth=%.2f",
             frame, n, sWinTasks,
             (double)ms[GDX3DS_MPROF_APT], (double)ms[GDX3DS_MPROF_PACE],
             (double)ms[GDX3DS_MPROF_INP], (double)ms[GDX3DS_MPROF_BEG],
             (double)ms[GDX3DS_MPROF_VI], (double)ms[GDX3DS_MPROF_DISP],
             (double)ms[GDX3DS_MPROF_JDP], (double)ms[GDX3DS_MPROF_JTOP],
             (double)(TicksToMs(gdx3ds_mprof_end_folded(sWinTicks)) * inv),
             (double)ms[GDX3DS_MPROF_TAIL],
             (double)ms[GDX3DS_MPROF_OTH]);
}

static float WinMs(int b, float inv) {
    return TicksToMs(sWinTicks[b]) * inv;
}

/* A rendered iteration opened a C3D frame (beg/R_FBEG) or presented (end); a frameskip SKIP
 * iteration does neither. */
static int IterationRendered(void) {
    return gdx3ds_mprof_ticks[GDX3DS_MPROF_BEG] != 0 || gdx3ds_mprof_ticks[GDX3DS_MPROF_END] != 0 ||
           gdx3ds_mprof_ticks[GDX3DS_MPROF_R_FBEG] != 0;
}

typedef struct C5CatTickCols {
    float logic, dlb, aud, ld, vi, inp, apt, pace, tail, task, idle;
} C5CatTickCols;

typedef struct C5CatRenderCols {
    float fbeg, misc, sky, trk, dress, mach, mesh, shd, fx, hud, mir, swp, fend;
} C5CatRenderCols;

static C5CatTickCols C5CatTickColumns(float inv) {
    C5CatTickCols c;
    c.logic = WinMs(GDX3DS_MPROF_LOGIC, inv);
    c.dlb = WinMs(GDX3DS_MPROF_DLB, inv);
    c.aud = WinMs(GDX3DS_MPROF_AUD, inv);
    c.ld = WinMs(GDX3DS_MPROF_LSEG, inv) + WinMs(GDX3DS_MPROF_SLA, inv) +
           WinMs(GDX3DS_MPROF_SEG, inv) + WinMs(GDX3DS_MPROF_DMA, inv);
    c.vi = WinMs(GDX3DS_MPROF_VI, inv) + WinMs(GDX3DS_MPROF_DISP, inv);
    c.inp = WinMs(GDX3DS_MPROF_INP, inv);
    c.apt = WinMs(GDX3DS_MPROF_APT, inv) + WinMs(GDX3DS_MPROF_JTOP, inv) +
            WinMs(GDX3DS_MPROF_JDP, inv);
    c.pace = WinMs(GDX3DS_MPROF_PACE, inv);
    c.tail = WinMs(GDX3DS_MPROF_TAIL, inv);
    c.task = WinMs(GDX3DS_MPROF_STASK, inv) + WinMs(GDX3DS_MPROF_BRM, inv) +
             WinMs(GDX3DS_MPROF_SUB, inv);
    c.idle = WinMs(GDX3DS_MPROF_OTH, inv);
    return c;
}

static float C5CatTickSum(const C5CatTickCols* c) {
    return c->logic + c->dlb + c->aud + c->ld + c->vi + c->inp + c->apt + c->pace + c->tail +
           c->task + c->idle;
}

static C5CatRenderCols C5CatRenderColumns(float invR) {
    C5CatRenderCols c;
    c.fbeg = WinMs(GDX3DS_MPROF_BEG, invR) + WinMs(GDX3DS_MPROF_R_FBEG, invR);
    c.misc = WinMs(GDX3DS_MPROF_R_MISC, invR);
    c.sky = WinMs(GDX3DS_MPROF_R_SKY, invR);
    c.trk = WinMs(GDX3DS_MPROF_R_TRK, invR);
    c.dress = WinMs(GDX3DS_MPROF_R_DRESS, invR);
    c.mach = WinMs(GDX3DS_MPROF_R_MACH, invR);
    c.mesh = WinMs(GDX3DS_MPROF_R_MESH, invR);
    c.shd = WinMs(GDX3DS_MPROF_R_SHD, invR);
    c.fx = WinMs(GDX3DS_MPROF_R_FX, invR);
    c.hud = WinMs(GDX3DS_MPROF_R_HUD, invR);
    c.mir = WinMs(GDX3DS_MPROF_R_MIRR, invR);
    c.swp = WinMs(GDX3DS_MPROF_R_SWAP, invR);
    c.fend = WinMs(GDX3DS_MPROF_END, invR);
    return c;
}

static float C5CatRenderSum(const C5CatRenderCols* c) {
    return c->fbeg + c->misc + c->sky + c->trk + c->dress + c->mach + c->mesh + c->shd + c->fx +
           c->hud + c->mir + c->swp + c->fend;
}

static void C5CatLog(const char* fmt, ...) {
    char buf[420];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
        size_t len = (size_t)n < sizeof(buf) - 1 ? (size_t)n : sizeof(buf) - 1;
        svcOutputDebugString(buf, len);
        gdx3ds_filelog_write(buf, len);
    }
}

static void C5CatEmit(unsigned long frame, unsigned n) {
    const float inv = 1.0f / (float)n;
    const unsigned r = sWinRendered;
    const float invR = r != 0 ? 1.0f / (float)r : 0.0f;
    const C5CatTickCols t = C5CatTickColumns(inv);
    const C5CatRenderCols c = C5CatRenderColumns(invR);
    const float tickSum = C5CatTickSum(&t);
    const float renderSum = C5CatRenderSum(&c);
    const float wall = TicksToMs(sWinT1 - sWinT0) * inv;
    const float res = wall - (tickSum + renderSum * (float)r * inv);
    const float reads = (float)sWinReads * inv;
    C5CatLog("[c5cat] frame=%lu n=%u r=%u rt=%d wall=%.2f rf=%.2f | logic=%.2f dlb=%.2f aud=%.2f "
             "ld=%.2f vi=%.2f inp=%.2f apt=%.2f pace=%.2f tail=%.2f task=%.2f idle=%.2f | fbeg=%.2f "
             "misc=%.2f sky=%.2f trk=%.2f dress=%.2f mach=%.2f mesh=%.2f shd=%.2f fx=%.2f "
             "hud=%.2f mir=%.2f swp=%.2f fend=%.2f | res=%.2f tk=%.0f ovh=%.2f",
             frame, n, r, sRtOff ? 0 : 1, (double)wall, (double)(tickSum - t.pace + renderSum),
             (double)t.logic, (double)t.dlb, (double)t.aud, (double)t.ld, (double)t.vi,
             (double)t.inp, (double)t.apt, (double)t.pace, (double)t.tail, (double)t.task, (double)t.idle,
             (double)c.fbeg, (double)c.misc, (double)c.sky, (double)c.trk, (double)c.dress,
             (double)c.mach, (double)c.mesh, (double)c.shd, (double)c.fx, (double)c.hud,
             (double)c.mir, (double)c.swp, (double)c.fend, (double)res, (double)reads,
             (double)(reads * sTickReadUs / 1000.0f));
}

/* C5 STAGE 2 [c5cnt]: the owned walk's per-category work counters (gdx_rt_glue.cpp), on the
 * [c5cat] cadence. Weak: builds without the owned interpreter link nothing. */
extern int gdx3ds_c5cnt_format(char* buf, int cap, unsigned long frame, unsigned rendered) __attribute__((weak));

static void C5CntEmit(unsigned long frame) {
    char buf[640];
    int n;
    if (gdx3ds_c5cnt_format == NULL) {
        return;
    }
    n = gdx3ds_c5cnt_format(buf, (int)sizeof(buf), frame, sWinRendered);
    if (n > 0) {
        svcOutputDebugString(buf, (size_t)n);
        gdx3ds_filelog_write(buf, (size_t)n);
    }
}

void gdx3ds_mprof_frame_end(unsigned long frame) {
    int i;
    gdx3ds_ships_frame_end(frame, gdx3ds_mprof_active); /* N60SHIPS [ships]/[lsplit] (default off) */
    if (!gdx3ds_mprof_active) {
        return;
    }
    /* Close the last open bucket of this frame, then fold. Nothing is timed between this
     * call and the next frame_begin, which is exactly where the receipt is emitted — so
     * the emit's own cost is outside every bucket and cannot inflate the profile. */
    (void)gdx3ds_mprof_enter(GDX3DS_MPROF_OTH);
    sWinT1 = gdx3ds_mprof_t0; /* the close above stamped it */
    sWinRendered += (unsigned)IterationRendered();
    sWinReads += gdx3ds_mprof_reads;
    gdx3ds_mprof_reads = 0;
    for (i = 0; i < GDX3DS_MPROF_COUNT; i++) {
        sWinTicks[i] += gdx3ds_mprof_ticks[i];
        gdx3ds_mprof_ticks[i] = 0;
    }
    sGen++;
    sWinFrames++;
    if (sWinFrames < GDX3DS_MPROF_WINDOW) {
        return;
    }
    if (sEmit) {
        MProfEmit(frame, sWinFrames);
    }
    C5CatEmit(frame, sWinFrames);
    gdx3ds_cpucut_window_end(frame, sWinFrames); /* o3ds-cpucuts: the A/B block ends with the window */
    C5CntEmit(frame);
    gdx3ds_ships_emit(frame); /* N60SHIPS: same window as [c5cat] */
    gdx3ds_hwprof_emit(frame, sWinFrames, sWinRendered); /* hwprof [c5pmu] / [hwsite] */
    if (sStealProbe) {
        const C5StealResult st = C5StealProbe();
        C5CatLog("[c5cat] frame=%lu steal=%.2f%% gaps=%u max=%.0fus (core-0 preemption of a "
                 "spinning main, %d ms between windows)", frame, (double)st.stolenPct, st.gaps,
                 (double)st.maxGapUs, kStealProbeMs);
    }
    memset(sWinTicks, 0, sizeof(sWinTicks));
    sWinFrames = 0;
    sWinDlBytes = 0;
    sWinTasks = 0;
    sWinRendered = 0;
    sWinReads = 0;
    sRtOff = 1;
}
