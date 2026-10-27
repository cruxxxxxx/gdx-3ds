/* port/3ds/gdx3ds_c5tick.c — C5TICK implementation. Contract, levers and receipts:
 * gdx3ds_c5tick.h. Section ids: gfx/gdx3ds_dlb_shim.h. */
#include "gdx3ds_c5tick.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <3ds.h>

#include "gdx3ds_config.h"
#include "gdx3ds_filelog.h"
#include "gdx3ds_frameskip.h"
#include "gdx_snapshot.h" /* mp-snapshot: gdx_snap_tick_input (the build-drop prediction) */
#include "gdx3ds_mainprof.h"
#include "gfx/gdx3ds_dlb_shim.h"
#include "gfx/nrr/gdx3ds_nrr.h"

#define C5TICK_WINDOW 64
#define C5TICK_HOLLOW_WINDOW 256
#define DLW_MAX_RANGES 8
#define DLW_MAX_LINES 30000
#define DLW_SEC_NOISE GDX_DLB_SEC_COUNT
#define DLW_SEC_SLOTS (GDX_DLB_SEC_COUNT + 1)

enum {
    KIND_REND = 0,
    KIND_SKIP = 1,
    KIND_COUNT = 2,
};

typedef struct GdxDlwRange {
    unsigned long lo;
    unsigned long hi;
} GdxDlwRange;

/* port/3ds/game/gdx3ds_c5tick_game.c (decomp types for the sizes). */
extern int gdx3ds_dlw_scratch_ranges(GdxDlwRange* out, int max);
/* port/3ds/os/gdx_fiber_3ds.c: fiber switches so far */
extern unsigned long gdx_fiber_switch_count;
/* port/gdx_audio_thread.cpp: the audio context mutex (recursive) */
extern void gdx_audio_ctx_lock(void);
extern void gdx_audio_ctx_unlock(void);
/* port/input_bridge.c */
extern int gdx_input_game_mode(void);
/* 3dsx.ld: .data starts on the page after .ARM.exidx; the image ends at __bss_end__. */
extern char __exidx_end[];
extern char __bss_end__[];

static const char* const kSecNames[DLW_SEC_SLOTS] = {
    "other", "rhead", "sky", "trk", "dress", "rmid", "cam", "vis", "shd",
    "body", "boost", "efx", "rhud", "p1hud", "menus", "trans", "end", "noise",
};

/* The sections GDX_DL_SKIP may ever skip: they write only the display list, the GfxPool and
 * per-frame draw scratch (the proof: port/3ds/patches/README.md, decomp-3ds-c5tick-dlb.patch).
 * A section not listed here always builds, whatever a stage or the hollow lever asks. */
static const uint8_t kSkippable[GDX_DLB_SEC_COUNT] = {
    [GDX_DLB_SEC_SKY] = 1,
    [GDX_DLB_SEC_TRK] = 1,
    [GDX_DLB_SEC_DRESS] = 1,
    [GDX_DLB_SEC_CAM] = 1,
    [GDX_DLB_SEC_SHD] = 1,
    [GDX_DLB_SEC_BODY] = 1,
    [GDX_DLB_SEC_BOOST] = 1,
    [GDX_DLB_SEC_EFX] = 1,
};

/* The subset a HOLLOW tick skips. The booster and effects sections are side-effect free too, but
 * they keep running: they fill the GfxPool's effects vertex buffer through a running cursor, and
 * the game's own debris / spark triangles later READ vertex fields no build writes (colour and
 * texture coordinates of vertices 1-2), i.e. whatever the previous build on that pool half left.
 * Skipping them on a hollow tick changes those stale bytes and so the pixels of the next frame
 * rendered from that half (canonical SHOT m01_4342, debris shards). They cost ~0.03 ms/tick in
 * the emulator. The consume path (rendered ticks) may still take them: whoever replaces them
 * owns what their triangles read. */
/* C6 [native] replay presents exactly the tick a hollow build is for (the one whose task the next
 * iteration drops) with THAT tick's matrices and sky/effect vertices: when it will (D = 2), a hollow
 * tick still runs the sections that write them -- sky tiles, the camera, the shadow matrices --
 * and always builds the machine poses. Track, dressing and bodies stay hollow (the replay keeps
 * A's world-space course and re-poses A's bodies). */
extern int gdx3ds_replay_wants_tick(void);
static const uint8_t kReplayKeeps[GDX_DLB_SEC_COUNT] = {
    [GDX_DLB_SEC_SKY] = 1,
    [GDX_DLB_SEC_CAM] = 1,
    [GDX_DLB_SEC_SHD] = 1,
};

static const uint8_t kHollowSkippable[GDX_DLB_SEC_COUNT] = {
    [GDX_DLB_SEC_SKY] = 1,
    [GDX_DLB_SEC_TRK] = 1,
    [GDX_DLB_SEC_DRESS] = 1,
    [GDX_DLB_SEC_CAM] = 1,
    [GDX_DLB_SEC_SHD] = 1,
    [GDX_DLB_SEC_BODY] = 1,
};

/* ---- rendered-tick consumers (NEXT.md C5: a native path that replaces a section) ---- */
static GdxDlbStub sConsumeStub[GDX_DLB_SEC_COUNT];
static uint8_t sConsumed[GDX_DLB_SEC_COUNT];
static unsigned long sWinSkipped[KIND_COUNT][GDX_DLB_SEC_COUNT];

/* ---- levers (latched once, at the first tick) ---- */
static int sChecked = 0;
static int sProf = 0;         /* [debug] dlbprof */
static int sWrite = 0;        /* [debug] dlwrite */
static int sWriteEvery = 4;   /* [debug] dlwrite_every */
static int sHollowLever = 0;  /* [debug] dl_hollow */
static int sPoses = 1;        /* [debug] dl_hollow_poses: model matrices still built on skipped body sections */
static int sConsumeMask = 0;  /* [debug] dl_consume_mask: MEASUREMENT ONLY, see LatchConsumeMask */
static int sMarkActive = 0;   /* any of the three: gdx_dlb_mark does work */

/* ---- per tick ---- */
static unsigned long sHostFrame = 0; /* this iteration's host frame number */
static int sTickSkip = 0;          /* frameskip skips THIS iteration (drops the PREVIOUS build's task) */
static int sBuildDrop = 0;         /* the build now running will be dropped: the next iteration skips */
static int sTickHollow = 0;        /* this build may be hollow (sBuildDrop && the lever) */
static int sBuildHollowed = 0;     /* this build actually skipped a section for hollowness */
static int sTickWrite = 0;         /* this tick runs the write-set check */
static int sPendingHollowTask = 0; /* a hollow build finished; its task has not been submitted */
static int sCur = -1;              /* open section, -1 outside a build */
static int sRaceScope = 0;         /* inside Race_Draw (RHEAD .. TRANS): the only hollow scope */
static int sSceneScope = 0;        /* niview: a non-race 3D scene (the records flyby) the NRR takes */
#ifdef EXPANSION_KIT
/* S5b [native] nrr_testdrive (default 0): the course editor's test drive draws its race outside
 * Race_Draw (course_edit/191080.c func_xk2_800DF6FC); with the lever on its draw opens the race
 * scope here (gdx_dl_testdrive_scope, decomp-3ds-ek-testdrive-scope.patch). Never hollow: the test
 * drive is outside race0's frameskip scope, and the hollow proof was made on Race_Draw. */
static int sTestDriveLever = 0;
static int sTestDriveScope = 0;          /* this build is inside the test drive's race scope */
static unsigned long sTestDriveBuilds = 0;
static unsigned long sTestDriveRuns = 0;
static unsigned long sTestDriveLastFrame = 0;
#define TESTDRIVE_SCOPE() sTestDriveScope
#else
#define TESTDRIVE_SCOPE() 0
#endif
static uint64_t sLast = 0;
static uint64_t sTickAcc[GDX_DLB_SEC_COUNT];
static unsigned long sTickCount = 0;
static unsigned long sFrame = 0;

/* ---- windows ---- */
static uint64_t sWinAcc[KIND_COUNT][GDX_DLB_SEC_COUNT];
static unsigned sWinBuilds[KIND_COUNT];
static unsigned sWinFrames = 0;
static unsigned sWinHollow = 0;
static unsigned sHollowFrames = 0;
static unsigned long sHollowTotal = 0;
static unsigned long sHollowWin = 0;
static unsigned long sLeakTotal = 0;

/* ---- write-set check ---- */
static uintptr_t sDlwBase = 0;
static unsigned sDlwBlk = 128;
static unsigned sDlwBlocks = 0;
static uint32_t* sDlwHash = NULL;
static uint8_t* sDlwSeen[DLW_SEC_SLOTS];
static uint8_t* sDlwScratch = NULL; /* 1 bit per block: inside a scratch range, never reported */
static unsigned sDlwLines = 0;
/* New (section, block) hits of the build being checked. They are logged at the end of the build,
 * after the last scan: a log line written mid-build would itself change the file-log ring, and
 * that ring would then show up as a write of every following section. */
#define DLW_PENDING_MAX 512
static uint32_t sDlwPendBlock[DLW_PENDING_MAX];
static uint8_t sDlwPendSec[DLW_PENDING_MAX];
static unsigned sDlwPendCount = 0;
static unsigned long sDlwPendDropped = 0;
static unsigned long sDlwChecked = 0;

static void C5TickLog(const char* fmt, ...) {
    char buf[448];
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

static float TicksToMs(uint64_t ticks) {
    return (float)ticks / (float)CPU_TICKS_PER_MSEC;
}

static int BitTest(const uint8_t* bits, unsigned i) {
    return (bits[i >> 3] >> (i & 7)) & 1;
}

static void BitSet(uint8_t* bits, unsigned i) {
    bits[i >> 3] |= (uint8_t)(1u << (i & 7));
}

/* ---------------------------------------------------------------- write-set check ---- */

static void DlwMarkScratch(void) {
    GdxDlwRange ranges[DLW_MAX_RANGES];
    const int count = gdx3ds_dlw_scratch_ranges(ranges, DLW_MAX_RANGES);
    int r;
    for (r = 0; r < count; r++) {
        unsigned long a;
        for (a = ranges[r].lo; a < ranges[r].hi; a += sDlwBlk) {
            if (a >= sDlwBase && a < sDlwBase + (uintptr_t)sDlwBlocks * sDlwBlk) {
                BitSet(sDlwScratch, (unsigned)((a - sDlwBase) / sDlwBlk));
            }
        }
    }
}

static int DlwAllocate(void) {
    const unsigned bitmapBytes = (sDlwBlocks + 7u) / 8u;
    int s;
    sDlwHash = (uint32_t*)malloc((size_t)sDlwBlocks * sizeof(uint32_t));
    sDlwScratch = (uint8_t*)calloc(bitmapBytes, 1);
    if (sDlwHash == NULL || sDlwScratch == NULL) {
        return 0;
    }
    for (s = 0; s < DLW_SEC_SLOTS; s++) {
        sDlwSeen[s] = (uint8_t*)calloc(bitmapBytes, 1);
        if (sDlwSeen[s] == NULL) {
            return 0;
        }
    }
    return 1;
}

static void DlwInit(void) {
    const uintptr_t dataStart = ((uintptr_t)__exidx_end + 0xFFFu) & ~(uintptr_t)0xFFFu;
    const uintptr_t imageEnd = (uintptr_t)__bss_end__;
    int blk = gdx3ds_config_get_int("debug", "dlwrite_blk", 128);
    if (blk < 32) {
        blk = 32;
    }
    sDlwBlk = (unsigned)blk & ~31u;
    sDlwBase = dataStart;
    sDlwBlocks = (unsigned)((imageEnd - dataStart) / sDlwBlk);
    if (!DlwAllocate()) {
        C5TickLog("[c5tick] dlwrite REFUSED: no memory for %u blocks", sDlwBlocks);
        sWrite = 0;
        return;
    }
    DlwMarkScratch();
    C5TickLog("[c5tick] dlwrite armed: image 0x%08lx..0x%08lx blk=%u blocks=%u every=%d "
              "(scratch: GfxPools, D_1000000, framebuffers, RSP output)",
              (unsigned long)dataStart, (unsigned long)imageEnd, sDlwBlk, sDlwBlocks, sWriteEvery);
}

static uint32_t DlwHashBlock(const uint32_t* p, unsigned words) {
    uint32_t h = 2166136261u;
    unsigned i;
    for (i = 0; i < words; i++) {
        h = (h ^ p[i]) * 16777619u;
    }
    return h;
}

static void DlwReport(int sec, unsigned block) {
    if (sDlwPendCount >= DLW_PENDING_MAX) {
        sDlwPendDropped++;
        return;
    }
    sDlwPendBlock[sDlwPendCount] = block;
    sDlwPendSec[sDlwPendCount] = (uint8_t)sec;
    sDlwPendCount++;
}

static void DlwFlushReports(void) {
    unsigned i;
    for (i = 0; i < sDlwPendCount && sDlwLines < DLW_MAX_LINES; i++) {
        sDlwLines++;
        C5TickLog("[dlw] sec=%s blk=0x%08lx frame=%lu mode=%d", kSecNames[sDlwPendSec[i]],
                  (unsigned long)(sDlwBase + (uintptr_t)sDlwPendBlock[i] * sDlwBlk), sHostFrame,
                  gdx_input_game_mode());
    }
    sDlwPendCount = 0;
}

/* Re-hash the image; every block that changed since the last hash is charged to `sec`. With
 * sec < 0 the hash only re-baselines. */
static void DlwScan(int sec) {
    const unsigned words = sDlwBlk / 4u;
    unsigned b;
    for (b = 0; b < sDlwBlocks; b++) {
        const uint32_t h = DlwHashBlock((const uint32_t*)(sDlwBase + (uintptr_t)b * sDlwBlk), words);
        if (h == sDlwHash[b]) {
            continue;
        }
        sDlwHash[b] = h;
        if (sec < 0 || BitTest(sDlwScratch, b) || BitTest(sDlwSeen[sec], b)) {
            continue;
        }
        if (sec != DLW_SEC_NOISE && BitTest(sDlwSeen[DLW_SEC_NOISE], b)) {
            continue; /* already known to change under another thread */
        }
        BitSet(sDlwSeen[sec], b);
        DlwReport(sec, b);
    }
}

/* ------------------------------------------------------------------------ levers ---- */

static void LatchConsumeMask(void) {
    int sec;
    sConsumeMask = gdx3ds_config_get_int("debug", "dl_consume_mask", 0);
    if (sConsumeMask == 0) {
        return;
    }
    C5TickLog("[c5tick] dl_consume_mask=0x%x: MEASUREMENT ONLY -- these sections are not built on "
              "rendered ticks either and nothing replaces them", sConsumeMask);
    for (sec = 0; sec < GDX_DLB_SEC_COUNT; sec++) {
        if (sConsumeMask & (1 << sec)) {
            (void)gdx3ds_c5tick_consume(sec, NULL);
        }
    }
}

static void LatchConfig(void) {
    sChecked = 1;
    sProf = gdx3ds_config_get_int("debug", "dlbprof", 0) != 0;
    sWrite = gdx3ds_config_get_int("debug", "dlwrite", 0) != 0;
    sWriteEvery = gdx3ds_config_get_int("debug", "dlwrite_every", 4);
    if (sWriteEvery < 1) {
        sWriteEvery = 1;
    }
    sHollowLever = gdx3ds_config_get_int("debug", "dl_hollow", 0) != 0;
    sPoses = gdx3ds_config_get_int("debug", "dl_hollow_poses", 1) != 0;
    if (sWrite) {
        DlwInit();
    }
    LatchConsumeMask();
#ifdef EXPANSION_KIT
    sTestDriveLever = gdx3ds_config_get_int("native", "nrr_testdrive", 0) != 0;
    C5TickLog("[c5tick] nrr_testdrive=%d (EK course editor test drive: its race draw opens the race scope, "
              "so the NRR categories take it; never hollow)", sTestDriveLever);
#endif
    gdx3ds_nrr_boot(); /* [native] nrr (gfx/nrr/gdx3ds_nrr.h): latch + category inits (their consumes) */
    sMarkActive = sMarkActive || sProf || sWrite || sHollowLever;
    C5TickLog("[c5tick] dl_hollow=%d poses=%d dlbprof=%d dlwrite=%d (debug.dl_hollow: skipped ticks "
              "build only the draw code that changes game state)", sHollowLever, sPoses, sProf, sWrite);
}

/* --------------------------------------------------------------------- the ticks ---- */

void gdx3ds_c5tick_begin_tick(unsigned long frame, int skipRender) {
    if (!sChecked) {
        LatchConfig();
    }
    sHostFrame = frame;
    sTickSkip = skipRender ? 1 : 0;
    sTickWrite = sWrite && (sTickCount % (unsigned long)sWriteEvery) == 0;
    sTickCount++;
}

static void FoldTick(void) {
    const int kind = sBuildDrop ? KIND_SKIP : KIND_REND;
    int s;
    for (s = 0; s < GDX_DLB_SEC_COUNT; s++) {
        sWinAcc[kind][s] += sTickAcc[s];
        sTickAcc[s] = 0;
    }
    sWinBuilds[kind]++;
    if (sBuildHollowed) {
        sWinHollow++;
        sHollowWin++;
        sHollowTotal++;
    }
}

static void EmitKind(unsigned long frame, int kind) {
    const unsigned n = sWinBuilds[kind];
    float ms[GDX_DLB_SEC_COUNT];
    float tot = 0.0f;
    unsigned long skips = 0;
    int s;
    if (n == 0) {
        return;
    }
    for (s = 0; s < GDX_DLB_SEC_COUNT; s++) {
        ms[s] = TicksToMs(sWinAcc[kind][s]) / (float)n;
        tot += ms[s];
        skips += sWinSkipped[kind][s];
    }
    C5TickLog("[c5dlb] frame=%lu kind=%s n=%u tot=%.2f hollow=%u skips=%lu | other=%.2f rhead=%.2f sky=%.2f "
              "trk=%.2f dress=%.2f rmid=%.2f cam=%.2f vis=%.2f shd=%.2f body=%.2f boost=%.2f "
              "efx=%.2f rhud=%.2f p1hud=%.2f menus=%.2f trans=%.2f",
              frame, kind == KIND_SKIP ? "skip" : "rend", n, (double)tot,
              kind == KIND_SKIP ? sWinHollow : 0u, skips, (double)ms[0], (double)ms[1], (double)ms[2],
              (double)ms[3], (double)ms[4], (double)ms[5], (double)ms[6], (double)ms[7],
              (double)ms[8], (double)ms[9], (double)ms[10], (double)ms[11], (double)ms[12],
              (double)ms[13], (double)ms[14], (double)ms[15]);
}

static void ResetWindow(void) {
    memset(sWinAcc, 0, sizeof(sWinAcc));
    memset(sWinBuilds, 0, sizeof(sWinBuilds));
    memset(sWinSkipped, 0, sizeof(sWinSkipped));
    sWinFrames = 0;
    sWinHollow = 0;
}

static void EmitHollowWindow(unsigned long frame) {
    static unsigned long sPrevSwitches = 0;
    const unsigned long switches = gdx_fiber_switch_count - sPrevSwitches;
    sPrevSwitches = gdx_fiber_switch_count;
    C5TickLog("[c5tick] frame=%lu n=%u hollow=%lu hollowTotal=%lu leak=%lu forced=%lu sw/tick=%.1f "
              "dlwChecked=%lu dlwLines=%u dlwDropped=%lu", frame, sHollowFrames, sHollowWin, sHollowTotal, sLeakTotal,
              gdx3ds_frameskip_hollow_forced(),
              (double)switches / (double)(sHollowFrames ? sHollowFrames : 1u), sDlwChecked, sDlwLines,
              sDlwPendDropped);
    sHollowFrames = 0;
    sHollowWin = 0;
}

void gdx3ds_c5tick_end_tick(unsigned long frame) {
    if (!sMarkActive) {
        return;
    }
    sFrame = frame;
    if (sProf && ++sWinFrames >= C5TICK_WINDOW) {
        EmitKind(frame, KIND_REND);
        EmitKind(frame, KIND_SKIP);
        ResetWindow();
    }
    if (++sHollowFrames >= C5TICK_HOLLOW_WINDOW) {
        EmitHollowWindow(frame);
    }
}

/* ------------------------------------------------------------ the decomp's hooks ---- */

/* The list this build writes is walked by the NEXT iteration (the game's framebuffer spin yields
 * between the build and Gfx_SetTask), so its fate is the next iteration's skip decision --
 * predicted here, and honoured by the governor (gdx3ds_frameskip_next_skips). */
static void BeginBuild(uint64_t now) {
    sPendingHollowTask = 0;
    sBuildDrop = (sHollowLever || sProf) ? gdx3ds_frameskip_next_skips(sHostFrame + 1) : 0;
    /* mp-snapshot: the prediction reads the governor's clock, which a resimulated tick does not
     * share, and a hollow build leaves different draw-side bytes behind (Racer shadowMatrixInit,
     * course.c's chunk groups and display-list cursors). A resim replays the real tick's value
     * (identity while no snapshot window is open). */
    sBuildDrop = gdx_snap_tick_input(GDX_SNAP_IN_DL_BUILD_DROP, sBuildDrop);
    sTickHollow = sHollowLever && sBuildDrop;
    sBuildHollowed = 0;
    sRaceScope = 0;
    sCur = GDX_DLB_SEC_OTHER;
    if (sTickWrite) {
        /* Hold the audio producer off the static image for the checked build: it is the main
         * concurrent writer (its tick state lives in .bss), and every byte it touches while a
         * section runs would be charged to that section. */
        gdx_audio_ctx_lock();
        DlwScan(-1);
        sDlwChecked++;
    }
    (void)now;
    sLast = svcGetSystemTick(); /* after the prediction (a replay peek) and the baseline hash */
}

static void EndBuild(void) {
    sCur = -1;
    sRaceScope = 0;
    sSceneScope = 0;
#ifdef EXPANSION_KIT
    sTestDriveScope = 0;
#endif
    if (sBuildHollowed) {
        sPendingHollowTask = 1;
    }
    if (sTickWrite) {
        DlwScan(DLW_SEC_NOISE);
        gdx_audio_ctx_unlock();
        DlwFlushReports();
    }
    FoldTick();
}

void gdx_dlb_mark(int sec) {
    uint64_t now;
    if (!sMarkActive || sec < 0 || sec >= GDX_DLB_SEC_COUNT) {
        return;
    }
    now = svcGetSystemTick();
    if (sec == GDX_DLB_SEC_OTHER) {
        BeginBuild(now);
        return;
    }
    if (sCur < 0) {
        return; /* a marker outside func_80069698 (a draw function called from elsewhere) */
    }
    sTickAcc[sCur] += now - sLast;
    if (sTickWrite) {
        DlwScan(sCur);
    }
    if (sec == GDX_DLB_SEC_END) {
        EndBuild();
        return;
    }
    sCur = sec;
    if (sec == GDX_DLB_SEC_RHEAD) {
        sRaceScope = 1;
    } else if (sec == GDX_DLB_SEC_TRANS) {
        sRaceScope = 0;
#ifdef EXPANSION_KIT
        sTestDriveScope = 0;
#endif
    }
    sLast = sTickWrite ? svcGetSystemTick() : now;
}

/* Hollow only inside Race_Draw: every other draw function (menus, the ending cutscene's
 * Racer_Draw, Transition_Draw) builds in full on every tick. */
int gdx_dl_hollow(void) {
    return sTickHollow && sRaceScope && !TESTDRIVE_SCOPE();
}

int gdx3ds_c5tick_hollow_now(void) {
    return sTickHollow && sRaceScope && !TESTDRIVE_SCOPE();
}

int gdx3ds_c5tick_hollow_pending(void) {
    return sPendingHollowTask;
}

int gdx3ds_c5tick_take_hollow_task(void) {
    const int hollow = sPendingHollowTask;
    sPendingHollowTask = 0;
    return hollow;
}

void gdx3ds_c5tick_note_leak(void) {
    sLeakTotal++;
    C5TickLog("[c5tick] LEAK: a hollow display list reached a rendered tick at frame %lu; "
              "dropped (the screen keeps the last frame)", sFrame);
}

/* game/gdx3ds_fullview_game.c: this build's local view, -1 when fullview is off. */
extern int gdx_fv_k(void);

static int FullviewDrops(int view) {
    const int k = gdx_fv_k();
    return k >= 0 && view != k;
}

/* niview: the scene scope's skip -- only a consumed section WITH a stub (the stub builds what replaces
 * it, or declines), never hollow, never the track (the gadget pass reads its cull). */
static int SceneSkip(int sec, int view, void** gfx) {
    void* next;
    if (!sConsumed[sec] || sConsumeStub[sec] == NULL || gfx == NULL || sec == GDX_DLB_SEC_TRK) {
        return 0;
    }
    next = sConsumeStub[sec](*gfx, sec, view);
    if (next == NULL) {
        return 0;
    }
    *gfx = next;
    sWinSkipped[sBuildDrop ? KIND_SKIP : KIND_REND][sec]++;
    return 1;
}

/* o3dsperf: [c5cat] markers for a multiplayer fullview build. Race_Draw's 2..4-player branch
 * carries no sky / track / dressing markers (decomp-3ds-c5cat-markers.patch marks the 1P branch
 * only), so on hardware 158a13e all of the local view's scene landed in misc= (12 ms of a 30 ms
 * rendered tick). At each sky / track / dressing / camera section start of that branch this writes
 * the category the walk is about to run: the section's own for the local view, misc for a
 * dropped view and for the camera. Only while the [c5cat] clock is armed ([debug] mainprof=1 or
 * gputrace=1); otherwise nothing is written and the list is unchanged. */
extern void* gdx_c5cat_gbi_marker(void* g, int cat);

static void C5CatFullviewMark(int sec, int view, void** gfx) {
    const int k = gdx_fv_k();
    int cat;
    if (k < 0 || gfx == NULL || *gfx == NULL || !gdx3ds_c5cat_armed()) {
        return;
    }
    if (sec == GDX_DLB_SEC_SKY) {
        cat = GDX3DS_C5CAT_SKY;
    } else if (sec == GDX_DLB_SEC_TRK) {
        cat = GDX3DS_C5CAT_TRK;
    } else if (sec == GDX_DLB_SEC_DRESS) {
        cat = GDX3DS_C5CAT_DRESS;
    } else if (sec == GDX_DLB_SEC_CAM) {
        cat = GDX3DS_C5CAT_MISC;
    } else {
        return; /* racer.c's sections carry their own markers */
    }
    if (view != k) {
        cat = GDX3DS_C5CAT_MISC;
    }
    *gfx = gdx_c5cat_gbi_marker(*gfx, cat);
}

int gdx_dl_skip(int sec, int view, void** gfx) {
    const int kind = sBuildDrop ? KIND_SKIP : KIND_REND;
    if (sec < 0 || sec >= GDX_DLB_SEC_COUNT || !kSkippable[sec]) {
        return 0;
    }
    C5CatFullviewMark(sec, view, gfx);
    /* MP phase 3 (gfx/gdx3ds_fullview_shim.h): in a fullview build only the local view is drawn;
     * every other view's side-effect-free sections go unbuilt (their visibility pass and HUD logic
     * still run). Latched by Race_Draw itself, so it needs neither the markers nor the race scope. */
    if (FullviewDrops(view)) {
        sWinSkipped[kind][sec]++;
        return 1;
    }
    if (!sRaceScope) {
        return sSceneScope ? SceneSkip(sec, view, gfx) : 0;
    }
    if (sTickHollow && kHollowSkippable[sec] && !TESTDRIVE_SCOPE() &&
        !(kReplayKeeps[sec] && gdx3ds_replay_wants_tick())) {
        sBuildHollowed = 1;
        sWinSkipped[kind][sec]++;
        return 1;
    }
    if (!sConsumed[sec]) {
        return 0;
    }
    /* Course_GadgetsDraw reads the chunk cull (chunk->drawState) Course_Draw just wrote: the
     * track may only go unbuilt when the dressing does too. */
    if (sec == GDX_DLB_SEC_TRK && !sConsumed[GDX_DLB_SEC_DRESS]) {
        return 0;
    }
    if (sConsumeStub[sec] != NULL && gfx != NULL) {
        void* next = sConsumeStub[sec](*gfx, sec, view);
        if (next == NULL) {
            return 0; /* the stub declined this build (a scene its native path does not model) */
        }
        *gfx = next;
    }
    sWinSkipped[kind][sec]++;
    return 1;
}

int gdx3ds_c5tick_consume(int sec, GdxDlbStub stub) {
    if (sec < 0 || sec >= GDX_DLB_SEC_COUNT || !kSkippable[sec]) {
        C5TickLog("[c5tick] consume REFUSED sec=%d (%s): not side-effect free, it always builds", sec,
                  (sec >= 0 && sec < GDX_DLB_SEC_COUNT) ? kSecNames[sec] : "?");
        return 0;
    }
    sConsumed[sec] = 1;
    sConsumeStub[sec] = stub;
    if (sec == GDX_DLB_SEC_TRK && !sConsumed[GDX_DLB_SEC_DRESS]) {
        C5TickLog("[c5tick] consume sec=trk: held until dress is consumed too (the gadget pass "
                  "reads the track's chunk cull)");
    }
    sMarkActive = 1; /* the race scope comes from the markers */
    C5TickLog("[c5tick] consume sec=%s stub=%d", kSecNames[sec], stub != NULL ? 1 : 0);
    return 1;
}

/* n60 NRR (gdx3ds_nrr_trk.h): a native category that keeps its section building (the track keeps
 * its cull) asks for the race scope the markers track. want_scope turns the markers on. */
void gdx3ds_c5tick_want_scope(void) {
    sMarkActive = 1;
}

int gdx3ds_c5tick_race_scope(void) {
    return sRaceScope;
}

#ifdef EXPANSION_KIT
/* S5b: called at the top of the test drive's race draw (func_xk2_800DF6FC's test-run branch), inside
 * func_80069698's build. Opens Race_Draw's scope exactly as its RHEAD marker does (the markers must
 * be on: a consumed section or a want_scope category turned them on); game.c's TRANS marker and the
 * end of the build close it. A new test run (a gap in the builds) gets one receipt line. */
void gdx_dl_testdrive_scope(void) {
    if (!sTestDriveLever || !sMarkActive || sCur < 0 || sRaceScope) {
        return;
    }
    gdx_dlb_mark(GDX_DLB_SEC_RHEAD);
    sTestDriveScope = 1;
    if (sTestDriveBuilds == 0 || sHostFrame > sTestDriveLastFrame + 8u) {
        sTestDriveRuns++;
        C5TickLog("[c5tick] testdrive scope open run=%lu frame=%lu builds=%lu", sTestDriveRuns, sHostFrame,
                  sTestDriveBuilds);
    }
    sTestDriveBuilds++;
    sTestDriveLastFrame = sHostFrame;
}

int gdx3ds_c5tick_testdrive_scope(void) {
    return sTestDriveScope;
}
#endif

/* niview: Records_Draw brackets the flyby's sky / course / dressing (decomp-native-records-scene.patch).
 * The scope opens only when the NRR takes such scenes ([native] ni_view records bit, 1 view). Kept
 * scene_ok-only on purpose: Records_Draw is the ONLY caller of gdx_dl_scene, and any other condition
 * OR'd in here would open its flyby scope too, letting that condition's stub claim sections (sky,
 * dress, body, ...) of a scene it was never meant to touch. gdx_dl_scene_mach below is the mach
 * category's own, separately-named entry for exactly that reason. */
extern int gdx3ds_nrr_scene_ok(void) __attribute__((weak));

void gdx_dl_scene(int on) {
    sSceneScope = (on && !sRaceScope && &gdx3ds_nrr_scene_ok != NULL && gdx3ds_nrr_scene_ok()) ? 1 : 0;
}

/* nimach (advisor correctness fix, native-review-2026-10-15.md follow-up): EndingCutscene_Draw's own
 * Racer_Draw(gfx, 0) call (decomp-native-gp-end-mach.patch) is outside Race_Draw's RHEAD..TRANS
 * bracket and outside Records_Draw's own flyby, so it needs a scope of its own rather than sharing
 * gdx_dl_scene -- an earlier version OR'd gdx3ds_nrr_mach_armed() into gdx_dl_scene itself, which
 * meant ANY scene that calls gdx_dl_scene(1) (i.e. Records_Draw's flyby) would also open once
 * [native] nrr_mach armed, even with ni_view's records bit off, letting the BODY stub claim the
 * flyby's racers too. This entry point is reached only from ending.c, so it cannot leak into
 * Records_Draw no matter what gdx3ds_nrr_mach_armed() returns. */
extern int gdx3ds_nrr_mach_armed(void) __attribute__((weak));

void gdx_dl_scene_mach(int on) {
    sSceneScope = (on && !sRaceScope && &gdx3ds_nrr_mach_armed != NULL && gdx3ds_nrr_mach_armed()) ? 1 : 0;
}

int gdx3ds_c5tick_scene_scope(void) {
    return sSceneScope;
}

int gdx_dl_poses(void) {
    return sPoses || gdx3ds_replay_wants_tick(); /* the replay reads the poses */
}
