/* port/3ds/gfx/gdx3ds_gpu_prof.c — G-GPUPROF implementation. See the header for the
 * column contract and gating.
 *
 * citro3d timing semantics were verified against the shipped libcitro3d.a
 * (devkitPro, renderqueue.o disassembly), not assumed from the header:
 *
 *   - C3D_FrameBegin(flags): `if (inFrame) return false;` comes FIRST — a
 *     re-entrant call never re-syncs, so routing the backend's re-entrant
 *     StartFrame through the plain SYNCDRAW call is behavior-identical.
 *     With SYNCDRAW it runs the C3D_FrameSync loop (gspWaitForAnyEvent until
 *     frameCounter[0] advances — the vblank0 callback), THEN gxCmdQueueWait
 *     (blocks until the previously submitted GX command queue fully executed).
 *     Splitting the two calls (timed C3D_FrameSync, then timed
 *     C3D_FrameBegin(0)) therefore measures waitVBlank and waitP3D separately
 *     while performing the exact same sequence.
 *   - cpuTime ("processing"): tick counter started on FrameBegin success,
 *     stopped in FrameEnd right before gxCmdQueueRun — the CPU-side frame window.
 *   - gpuTime ("drawing"): tick counter started in FrameEnd at gxCmdQueueRun,
 *     stopped in onQueueFinish — wall time the GX queue (P3D draw lists + PPF
 *     display transfers) took for the frame submitted LAST, read one frame later.
 *   - C3D_GetProcessingTime/C3D_GetDrawingTime return osTickCounterRead(), i.e.
 *     milliseconds as float.
 *
 * All entry points run on the render thread (the main thread — same thread that
 * runs the game fibers); no locking anywhere.
 */
#include "gdx3ds_gpu_prof.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <3ds.h>
#include <citro3d.h>

/* Stream B's config surface (port/3ds/os/gdx3ds_config.h) — weak so the DL-test
 * harness, which links gdx3ds_gfx without gdx3ds_os, still links; the gate then
 * stays off, matching the harness's pre-existing frame behavior. */
extern int gdx3ds_config_get_int(const char* section, const char* key, int fallback)
    __attribute__((weak));
/* Config-ready signal (same TU as get_int). The gputrace latch below runs once and
 * forever at the first fresh frame_begin; if that ever precedes gdx3ds_config_load
 * completing (boot-order drift between .cia/.3dsx/emu), latching then would freeze
 * gputrace=0 for the whole run. Deferring until this reads 1 makes the latch
 * boot-order-proof. Weak for the same DL-harness reason. */
extern int gdx3ds_config_loaded(void) __attribute__((weak));
/* HW-visible sink (port/3ds/gdx3ds_filelog.c, armed by [debug] filelog=1). Retail
 * hardware DISCARDS svcOutputDebugString — only Azahar's logger sees it — so every
 * [gpu]/[fill]/[prof]/[profop] line (and the arming ack) must also reach the filelog
 * or a hardware gputrace run shows nothing at all. Weak: absent in the DL harness. */
extern void gdx3ds_filelog_write(const char* msg, size_t len) __attribute__((weak));

/* SELECT-PERF: cumulative texture-import / SETTIMG-resolution counters
 * (gfx_citro3d.cpp) for the per-window imp=/rl=/rm= columns, and the game's mode
 * word (decomp src/game/game.c) for the md= scene tag. All weak: the DL harness
 * links this TU without the decomp, and md then reads -2 (unavailable). */
extern void gdx3ds_texcache_prof_totals(unsigned long* imports, unsigned long* settimgLoads,
                                        unsigned long* settimgMemoHits) __attribute__((weak));
extern int32_t gGameMode __attribute__((weak));

static int GpuProfGameMode(void) {
    if (&gGameMode == NULL) {
        return -2; /* symbol absent (harness link) */
    }
    if (gGameMode == -1) {
        return -1; /* game not yet in a mode */
    }
    return (int)(gGameMode & 0x1F); /* decomp GET_MODE(): low 5 bits are the screen id */
}

#define GPUPROF_SHOT_LABEL_MAX 24
#define GPUPROF_SCREEN_PIXELS 96000u /* fb0: 240x400 portrait top-screen target */

static int sChecked = 0;
static int sEnabled = 0;

/* Per-frame scratch (reset on each fresh frame_begin). */
static u64 sFrameStartTick = 0;
static u64 sLastEndTick = 0;
static float sWaitVblMs = 0.0f;
static float sWaitP3dMs = 0.0f;
static float sBuildMs = 0.0f;
static unsigned sFramePassScr = 0;
static unsigned sFramePassTex = 0;
static unsigned sFrameCopies = 0;
static unsigned sFrameRdbk = 0;
static float sFrameMpix = 0.0f;

/* S12 asset-cost per-frame scratch (reset on each fresh frame_begin). */
static unsigned sFrameTexBytes = 0;   /* Σ swizzled upload payload this frame */
static unsigned sFrameVerts = 0;      /* Σ vertices repacked/transformed this frame */
/* Distinct texIds bound this frame, deduped by linear scan. Bounded by binds/frame
 * (≤2 per draw, race telemetry ~200 distinct/frame); overflow degrades to a plain
 * count, never a crash. Only touched when tracing is enabled. */
#define GPUPROF_UNIQUE_TEX_MAX 512
static unsigned sFrameTexIds[GPUPROF_UNIQUE_TEX_MAX];
static unsigned sFrameUniqueTex = 0;

/* Window accumulators, emitted every 64th frame ([c3d] cadence). */
static unsigned long sFrameCount = 0;
static unsigned sWinFrames = 0;
static float sSumWall = 0.0f, sSumBuild = 0.0f, sSumProc = 0.0f, sSumGpu = 0.0f;
static float sSumWVbl = 0.0f, sSumWP3d = 0.0f;
static float sMaxCmdUsage = 0.0f;
static unsigned sWinPassScr = 0, sWinPassTex = 0, sWinCopies = 0, sWinRdbk = 0;
static float sWinMpix = 0.0f;
/* S12 asset-cost window accumulators (u64: texBytes over 64 frames can exceed u32). */
static u64 sWinTexBytes = 0;
static u64 sWinVerts = 0;
static u64 sWinUniqueTex = 0;

/* SELECT-PERF: per-frame deltas of the cumulative import / SETTIMG-resolution
 * counters (gfx_citro3d.cpp), accumulated per window. sPrev* snapshot the previous
 * frame's totals so the tagged single-frame shots report that frame's own rates. */
static unsigned long sPrevImpTotal = 0, sPrevRlTotal = 0, sPrevRmTotal = 0;
static unsigned long sFrameImp = 0, sFrameRl = 0, sFrameRm = 0;
static unsigned long sWinImp = 0, sWinRl = 0, sWinRm = 0;

static char sShotLabel[GPUPROF_SHOT_LABEL_MAX];
static int sShotPending = 0;

static float TicksToMs(u64 dt) {
    return (float)((double)dt / CPU_TICKS_PER_MSEC);
}

static void GpuProfLog(const char* fmt, ...) {
    char buf[224];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
        size_t len = (size_t)n < sizeof(buf) - 1 ? (size_t)n : sizeof(buf) - 1;
        svcOutputDebugString(buf, len); /* Azahar's log channel; a no-op on retail HW */
        if (&gdx3ds_filelog_write != NULL) {
            gdx3ds_filelog_write(buf, len); /* the only sink real hardware can show */
        }
    }
}

/* --------------------------------------------------------------------------------
 * [prof] sectioned CPU-build profiler (see the header block for the contract).
 * The per-frame accumulators are the extern arrays the hook sites write directly;
 * this TU owns folding them into per-window sums, split into steady-state vs
 * "storm" frames (draws > GPUPROF_STORM_DRAWS ≈ the 30-machine race-start burst),
 * and emitting the [prof] / [prof!] companion lines on the [gpu] cadence. */
#define GPUPROF_STORM_DRAWS 90

int gdx3ds_prof_active = 0;
uint64_t gdx3ds_prof_sec_ticks[GDX3DS_PROF_SEC_COUNT];
uint32_t gdx3ds_prof_sec_calls[GDX3DS_PROF_SEC_COUNT];
uint32_t gdx3ds_prof_vtx_loaded = 0;
uint64_t gdx3ds_prof_child_ticks = 0;
/* [profop] per-opcode accumulators — DEFINED in the interpreter (single dispatch call site). */
extern uint64_t gdx3ds_prof_op_ticks[256];
extern uint32_t gdx3ds_prof_op_calls[256];
/* [trect] texrect-run census drain (lus-trect-census.patch); weak so a tree without the
 * patch still links. Formats two lines for the [prof] window and resets. */
extern int gdx_trect_census_format(char* line1, char* line2, int cap, unsigned frames) __attribute__((weak));
/* [trect-tex] inventory sink for the census's one-shot texture listing. */
void gdx_trect_census_dump(const char* line) {
    GpuProfLog("%s", line);
}

long long gdx3ds_prof_now(void) {
    return (long long)svcGetSystemTick();
}

/* Window sums, [0] = steady frames, [1] = storm frames (draws > threshold). */
static u64 sProfWinTicks[2][GDX3DS_PROF_SEC_COUNT];
static unsigned long sProfWinCalls[2][GDX3DS_PROF_SEC_COUNT];
static u64 sProfWinVtx[2];
static float sProfWinProc[2]; /* Σ C3D processing ms, for the oth= column */
static unsigned sProfWinFrames[2];

/* Fold this frame's section scratch into the window bucket keyed by storm-ness,
 * then reset the scratch. Runs from frame_end_post — resetting here (and not in
 * frame_begin) keeps the bridge pre-pass ticks, recorded BEFORE the fresh
 * C3D_FrameBegin, attributed to the frame they build. */
static void ProfFoldFrame(unsigned frameDraws, float procMs) {
    const int storm = frameDraws > GPUPROF_STORM_DRAWS ? 1 : 0;
    for (int s = 0; s < GDX3DS_PROF_SEC_COUNT; s++) {
        sProfWinTicks[storm][s] += gdx3ds_prof_sec_ticks[s];
        sProfWinCalls[storm][s] += gdx3ds_prof_sec_calls[s];
        gdx3ds_prof_sec_ticks[s] = 0;
        gdx3ds_prof_sec_calls[s] = 0;
    }
    sProfWinVtx[storm] += gdx3ds_prof_vtx_loaded;
    gdx3ds_prof_vtx_loaded = 0;
    sProfWinProc[storm] += procMs;
    sProfWinFrames[storm]++;
}

static void ProfEmitLine(const char* tag, const u64 ticks[GDX3DS_PROF_SEC_COUNT],
                         const unsigned long calls[GDX3DS_PROF_SEC_COUNT], u64 vtxN, float procSum,
                         unsigned n) {
    const float inv = 1.0f / (float)n;
    const float br = TicksToMs(ticks[GDX3DS_PROF_BR]) * inv;
    const float dsp = TicksToMs(ticks[GDX3DS_PROF_RUN]) * inv;
    const float vtx = TicksToMs(ticks[GDX3DS_PROF_VTX]) * inv;
    const float tri = TicksToMs(ticks[GDX3DS_PROF_TRI]) * inv;
    const float imp = TicksToMs(ticks[GDX3DS_PROF_IMP]) * inv;
    const float drw = TicksToMs(ticks[GDX3DS_PROF_DRW]) * inv;
    const float mtx = TicksToMs(ticks[GDX3DS_PROF_MTX]) * inv;
    /* oth = the C3D CPU window minus every interpreter-side bucket: ImGui, present
     * glue, post-Run bridge work, FrameEnd flush. br is OUTSIDE proc (pre-pass runs
     * before the fresh FrameBegin) so it is deliberately absent here; a small
     * negative oth just means bracket slop (Run's pre-StartFrame prologue). */
    const float oth = procSum * inv - (dsp + vtx + tri + imp + drw + mtx);
    GpuProfLog("[prof%s] frame=%lu n=%u br=%.2f dsp=%.2f vtx=%.2f tri=%.2f imp=%.2f drw=%.2f "
               "mtx=%.2f oth=%.2f | vtxN=%lu nV=%lu nT=%lu nI=%lu nD=%lu nM=%lu nRun=%lu",
               tag, sFrameCount, n, br, dsp, vtx, tri, imp, drw, mtx, oth,
               (unsigned long)(vtxN / n), calls[GDX3DS_PROF_VTX] / n, calls[GDX3DS_PROF_TRI] / n,
               calls[GDX3DS_PROF_IMP] / n, calls[GDX3DS_PROF_DRW] / n, calls[GDX3DS_PROF_MTX] / n,
               calls[GDX3DS_PROF_RUN] / n);
}

/* Emit [prof] (whole window) + [prof!] (storm frames only, if any), then reset. */
static void ProfEmitWindow(void) {
    u64 allTicks[GDX3DS_PROF_SEC_COUNT];
    unsigned long allCalls[GDX3DS_PROF_SEC_COUNT];
    const unsigned n = sProfWinFrames[0] + sProfWinFrames[1];
    if (n == 0) {
        return;
    }
    for (int s = 0; s < GDX3DS_PROF_SEC_COUNT; s++) {
        allTicks[s] = sProfWinTicks[0][s] + sProfWinTicks[1][s];
        allCalls[s] = sProfWinCalls[0][s] + sProfWinCalls[1][s];
    }
    ProfEmitLine("", allTicks, allCalls, sProfWinVtx[0] + sProfWinVtx[1],
                 sProfWinProc[0] + sProfWinProc[1], n);
    if (sProfWinFrames[1] > 0) {
        ProfEmitLine("!", sProfWinTicks[1], sProfWinCalls[1], sProfWinVtx[1], sProfWinProc[1],
                     sProfWinFrames[1]);
    }
    memset(sProfWinTicks, 0, sizeof(sProfWinTicks));
    memset(sProfWinCalls, 0, sizeof(sProfWinCalls));
    sProfWinVtx[0] = sProfWinVtx[1] = 0;
    sProfWinProc[0] = sProfWinProc[1] = 0.0f;
    sProfWinFrames[0] = sProfWinFrames[1] = 0;

    /* [profop] top opcode handlers by accumulated ticks over the same window — names WHICH
     * handler bodies own the dispatch-remainder ("dsp") milliseconds. Top-6 selection sort,
     * per-window reset. ms values are per-frame (divided by n). */
    {
        char line[224];
        int len = 0;
        len += snprintf(line + len, (size_t)(sizeof(line) - len), "[profop] n=%u top:", n);
        for (int rank = 0; rank < 6; rank++) {
            int best = -1;
            uint64_t bestTicks = 0;
            for (int op = 0; op < 256; op++) {
                if (gdx3ds_prof_op_ticks[op] > bestTicks) {
                    bestTicks = gdx3ds_prof_op_ticks[op];
                    best = op;
                }
            }
            if (best < 0 || bestTicks == 0) {
                break;
            }
            len += snprintf(line + len, (size_t)(sizeof(line) - len), " %02X=%.2f/%lu",
                            (unsigned)best, (double)(TicksToMs(bestTicks) / (float)n),
                            (unsigned long)(gdx3ds_prof_op_calls[best] / n));
            gdx3ds_prof_op_ticks[best] = 0;
            gdx3ds_prof_op_calls[best] = 0;
        }
        memset(gdx3ds_prof_op_ticks, 0, sizeof(gdx3ds_prof_op_ticks));
        memset(gdx3ds_prof_op_calls, 0, sizeof(gdx3ds_prof_op_calls));
        GpuProfLog(line);
    }
    /* [trect] texrect-run census, same window (menus and race alike). */
    if (&gdx_trect_census_format != NULL) {
        char l1[224];
        char l2[224];
        if (gdx_trect_census_format(l1, l2, (int)sizeof(l1), n) != 0) {
            GpuProfLog("%s", l1);
            GpuProfLog("%s", l2);
        }
    }
}

/* MENU DBG tab: live gputrace flip. Render/main thread only (the menu tick runs
 * between frames, so gdx3ds_prof_active never flips mid-frame). Marks the config
 * latch as decided so a later GpuProfEnsureInit cannot overwrite the user's choice. */
void gdx3ds_gpuprof_set_enabled(int on) {
    sChecked = 1;
    sEnabled = on ? 1 : 0;
    if (!sEnabled) {
        gdx3ds_prof_active = 0; /* hook sites stop accumulating immediately */
    }
}

int gdx3ds_gpuprof_get_enabled(void) {
    return sEnabled;
}

static void GpuProfEnsureInit(void) {
    if (sChecked) {
        return;
    }
    if (&gdx3ds_config_get_int != NULL && &gdx3ds_config_loaded != NULL &&
        !gdx3ds_config_loaded()) {
        /* INI not parsed yet: do NOT latch — re-check on the next fresh frame_begin.
         * Frames before config-ready just run untraced (identical to the disabled
         * path), so both .cia and .3dsx boot orders converge on the INI's value. */
        return;
    }
    sChecked = 1;
    if (&gdx3ds_config_get_int != NULL) {
        sEnabled = gdx3ds_config_get_int("debug", "gputrace", 0) != 0;
    }
    if (sEnabled) {
        GpuProfLog("[gpu] tracing enabled (debug.gputrace=1)");
    }
}

/* ---------------------------------------------------------------------------------------------
 * CADENCE: adaptive vblank alignment — the structural fps-ceiling fix.
 *
 * C3D_FRAME_SYNCDRAW performs C3D_FrameSync BEFORE the frame's CPU work: it parks the whole
 * host thread (game fibers included — this runs inside gdx_vi_tick on task frames) until the
 * NEXT vblank0 event. That is correct pacing when a frame's total cost is under one LCD period
 * (~16.7 ms): it caps the loop at 60 Hz with vblank-aligned presents. But every measured menu
 * and race frame on this port costs 24-28 ms of CPU *after* the sync ([gpu] build= column), so
 * the alignment stalls a frame that is already guaranteed to miss the very boundary it waits
 * for: measured wVbl = 11-16 ms of pure idle per frame on top of the build (wall 38-50 ms,
 * i.e. menus at ~26 fps that would be ~38 fps without the stall). The swap itself does not
 * need this wait — C3D_FrameEnd queues the display transfer + swap through the GX queue and
 * GSP latches buffer flips at vblank regardless, so skipping the sync can never tear; the
 * only backpressure a frame needs is gxCmdQueueWait (the C3D_FrameBegin(0) component, the
 * [gpu] wP3D column — measured 0.0 on every sample, the GPU is idle).
 *
 * Policy: skip C3D_FrameSync exactly when the time since the previous fresh frame-begin
 * already exceeds one LCD period — the boundary has passed, the wait buys nothing. When the
 * loop ever gets fast enough to finish inside one period (< 16.7 ms), the elapsed check fails
 * and the SYNCDRAW behavior re-engages, so the 60 fps cap and vblank alignment are preserved
 * for the fast case by construction. `[debug] force_syncdraw = 1` restores the old
 * unconditional alignment for A/B measurement.
 * ------------------------------------------------------------------------------------------ */
static u64 sCadPrevFreshBeginTick = 0;
static int sExternalPacing = 0; /* render thread owns the C3D frame, main paces on vblank */

void gdx3ds_gpuprof_set_external_pacing(int on) {
    sExternalPacing = on ? 1 : 0;
}

static int GpuProfSkipVblankSync(void) {
    static int sForceSyncdraw = -1;
    static u64 sVblankPeriodTicks = 0;
    if (sForceSyncdraw < 0) {
        sForceSyncdraw = (&gdx3ds_config_get_int != NULL)
                             ? (gdx3ds_config_get_int("debug", "force_syncdraw", 0) != 0)
                             : 0;
        /* One top-LCD refresh at 59.8337 Hz, in svcGetSystemTick ticks. */
        sVblankPeriodTicks = (u64)(CPU_TICKS_PER_MSEC * (1000.0 / 59.8337));
        if (sForceSyncdraw) {
            GpuProfLog("[gpu] adaptive vblank sync DISABLED (debug.force_syncdraw=1)");
        }
    }
    if (sExternalPacing) {
        return 1; /* main already aligned this iteration to vblank */
    }
    if (sForceSyncdraw) {
        return 0;
    }
    if (sCadPrevFreshBeginTick == 0) {
        return 0; /* first frame: keep the aligned start */
    }
    return (svcGetSystemTick() - sCadPrevFreshBeginTick) >= sVblankPeriodTicks;
}

void gdx3ds_gpuprof_frame_begin(int reentry) {
    if (reentry) {
        /* Re-entrant StartFrame: citro3d early-outs on inFrame before SYNCDRAW,
         * so this is the exact pre-existing behavior at the pre-existing cost. */
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        return;
    }
    GpuProfEnsureInit();
    if (!sEnabled) {
        /* CADENCE: same adaptive skip as the profiled path below — the fix must not
         * depend on gputrace being armed. */
        C3D_FrameBegin(GpuProfSkipVblankSync() ? 0 : C3D_FRAME_SYNCDRAW);
        sCadPrevFreshBeginTick = svcGetSystemTick();
        return;
    }
    /* [prof] hot-path gate: latched here (render thread, never mid-frame) so the
     * hook sites see a plain int that flips only at a fresh frame boundary. */
    gdx3ds_prof_active = sEnabled;
    const u64 t0 = svcGetSystemTick();
    if (!GpuProfSkipVblankSync()) {
        C3D_FrameSync();             /* the SYNCDRAW component: vblank pacing wait */
    }
    const u64 t1 = svcGetSystemTick();
    C3D_FrameBegin(0);               /* gxCmdQueueWait: previous frame's GPU work */
    const u64 t2 = svcGetSystemTick();
    sWaitVblMs = TicksToMs(t1 - t0); /* ~0 on frames where the adaptive skip fired */
    sWaitP3dMs = TicksToMs(t2 - t1);
    sFrameStartTick = t2;
    sCadPrevFreshBeginTick = t2;
    /* Fresh frame always opens a pass on the screen target (C3D_FrameDrawOn fb0). */
    sFramePassScr = 1;
    sFramePassTex = 0;
    sFrameCopies = 0;
    sFrameRdbk = 0;
    sFrameMpix = (float)GPUPROF_SCREEN_PIXELS * 1e-6f;
    sFrameTexBytes = 0;
    sFrameVerts = 0;
    sFrameUniqueTex = 0;
}

void gdx3ds_gpuprof_frame_end_pre(void) {
    if (!sEnabled) {
        return;
    }
    sBuildMs = TicksToMs(svcGetSystemTick() - sFrameStartTick);
}

static void GpuProfEmit(const char* tagFmtPrefix, unsigned nFrames, float wall, float build, float proc,
                        float gpu, float wVbl, float wP3d, float cmdPct, unsigned draws, unsigned tris,
                        unsigned passScr, unsigned passTex, unsigned copies, unsigned rdbk, float mpix,
                        u64 texBytes, u64 uniqueTex, u64 verts, unsigned long imp, unsigned long rl,
                        unsigned long rm) {
    /* imp/rl/rm are DELTAS over this emit's n frames (texture imports, full SETTIMG
     * resource loads, SETTIMG-memo hits); md is the game's GET_MODE screen id at emit
     * time (8=machine select, 9=machine settings; -1 pre-mode, -2 harness link). */
    GpuProfLog("[gpu] %sframe=%lu n=%u wall=%.1f build=%.1f proc=%.1f gpu=%.1f wVbl=%.1f wP3D=%.1f "
               "cmd=%.0f%% draws=%u tris=%u imp=%lu rl=%lu rm=%lu md=%d",
               tagFmtPrefix, sFrameCount, nFrames, wall, build, proc, gpu, wVbl, wP3d, cmdPct, draws, tris,
               imp, rl, rm, GpuProfGameMode());
    GpuProfLog("[fill] %sframe=%lu n=%u passes=%.2f (scr=%.2f tex=%.2f) copies=%.2f rdbk=%.2f estMpix=%.2f "
               "texBytes=%.0f uniqueTex=%.1f vtx=%.0f",
               tagFmtPrefix, sFrameCount, nFrames, (float)(passScr + passTex) / (float)nFrames,
               (float)passScr / (float)nFrames, (float)passTex / (float)nFrames,
               (float)copies / (float)nFrames, (float)rdbk / (float)nFrames, mpix / (float)nFrames,
               (double)texBytes / (double)nFrames, (double)uniqueTex / (double)nFrames,
               (double)verts / (double)nFrames);
}

void gdx3ds_gpuprof_frame_end_post(unsigned frameDraws, unsigned frameTris) {
    if (!sEnabled) {
        return;
    }
    const u64 now = svcGetSystemTick();
    const float wall = sLastEndTick != 0 ? TicksToMs(now - sLastEndTick) : 0.0f;
    sLastEndTick = now;
    const float proc = C3D_GetProcessingTime();
    const float gpu = C3D_GetDrawingTime(); /* previous submitted queue (see header) */
    const float cmdPct = C3D_GetCmdBufUsage() * 100.0f;
    /* C3D_FrameEnd queued this frame's fb0 -> LCD display transfer. */
    sFrameMpix += (float)GPUPROF_SCREEN_PIXELS * 1e-6f;

    sFrameCount++;
    sWinFrames++;
    sSumWall += wall;
    sSumBuild += sBuildMs;
    sSumProc += proc;
    sSumGpu += gpu;
    sSumWVbl += sWaitVblMs;
    sSumWP3d += sWaitP3dMs;
    if (cmdPct > sMaxCmdUsage) {
        sMaxCmdUsage = cmdPct;
    }
    sWinPassScr += sFramePassScr;
    sWinPassTex += sFramePassTex;
    sWinCopies += sFrameCopies;
    sWinRdbk += sFrameRdbk;
    sWinMpix += sFrameMpix;
    sWinTexBytes += sFrameTexBytes;
    sWinVerts += sFrameVerts;
    sWinUniqueTex += sFrameUniqueTex;

    /* [prof] fold this frame's section scratch (storm split keys on this frame's
     * draw-call count — the 30-machine race-start burst runs draws well past the
     * threshold while menus/steady race sit under it). */
    ProfFoldFrame(frameDraws, proc);

    /* SELECT-PERF: fold this frame's import / SETTIMG-resolution deltas into the
     * window. Totals are cumulative since boot, so the first frame's delta absorbs
     * boot-time loads — same convention as the [c3d] d* columns. */
    if (&gdx3ds_texcache_prof_totals != NULL) {
        unsigned long impTotal = 0, rlTotal = 0, rmTotal = 0;
        gdx3ds_texcache_prof_totals(&impTotal, &rlTotal, &rmTotal);
        sFrameImp = impTotal - sPrevImpTotal;
        sFrameRl = rlTotal - sPrevRlTotal;
        sFrameRm = rmTotal - sPrevRmTotal;
        sPrevImpTotal = impTotal;
        sPrevRlTotal = rlTotal;
        sPrevRmTotal = rmTotal;
        sWinImp += sFrameImp;
        sWinRl += sFrameRl;
        sWinRm += sFrameRm;
    }

    if (sShotPending) {
        /* Scene-tagged single-frame snapshot, emitted at SHOT time regardless of
         * the window phase (the label names the scene the BMP twin captured). */
        char prefix[GPUPROF_SHOT_LABEL_MAX + 8];
        snprintf(prefix, sizeof(prefix), "tag=%s ", sShotLabel);
        GpuProfEmit(prefix, 1, wall, sBuildMs, proc, gpu, sWaitVblMs, sWaitP3dMs, cmdPct, frameDraws,
                    frameTris, sFramePassScr, sFramePassTex, sFrameCopies, sFrameRdbk, sFrameMpix,
                    sFrameTexBytes, sFrameUniqueTex, sFrameVerts, sFrameImp, sFrameRl, sFrameRm);
        sShotPending = 0;
    }

    /* Same cadence/phase as the [c3d] line (mFrameIndex & 63) == 1: adjacent in the log. */
    if ((sFrameCount & 63) == 1 && sWinFrames > 1) {
        GpuProfEmit("", sWinFrames, sSumWall / (float)sWinFrames, sSumBuild / (float)sWinFrames,
                    sSumProc / (float)sWinFrames, sSumGpu / (float)sWinFrames,
                    sSumWVbl / (float)sWinFrames, sSumWP3d / (float)sWinFrames, sMaxCmdUsage, frameDraws,
                    frameTris, sWinPassScr, sWinPassTex, sWinCopies, sWinRdbk, sWinMpix,
                    sWinTexBytes, sWinUniqueTex, sWinVerts, sWinImp, sWinRl, sWinRm);
        ProfEmitWindow(); /* [prof]/[prof!] companion lines, same window */
        sWinFrames = 0;
        sSumWall = sSumBuild = sSumProc = sSumGpu = sSumWVbl = sSumWP3d = 0.0f;
        sMaxCmdUsage = 0.0f;
        sWinPassScr = sWinPassTex = sWinCopies = sWinRdbk = 0;
        sWinMpix = 0.0f;
        sWinTexBytes = sWinVerts = sWinUniqueTex = 0;
        sWinImp = sWinRl = sWinRm = 0;
    }
}

void gdx3ds_gpuprof_note_pass(int texBacked, unsigned targetPixels) {
    if (!sEnabled) {
        return;
    }
    if (texBacked) {
        sFramePassTex++;
    } else {
        sFramePassScr++;
    }
    sFrameMpix += (float)targetPixels * 1e-6f;
}

void gdx3ds_gpuprof_note_copy(unsigned pixels) {
    if (!sEnabled) {
        return;
    }
    sFrameCopies++;
    sFrameMpix += (float)pixels * 1e-6f;
}

void gdx3ds_gpuprof_note_readback(unsigned pixels) {
    if (!sEnabled) {
        return;
    }
    sFrameRdbk++;
    sFrameMpix += (float)pixels * 1e-6f;
}

void gdx3ds_gpuprof_note_tex_upload(unsigned bytes) {
    if (!sEnabled) {
        return;
    }
    sFrameTexBytes += bytes;
}

void gdx3ds_gpuprof_note_tex_bind(unsigned texId) {
    if (!sEnabled) {
        return;
    }
    /* Dedupe within the frame: only distinct texIds count toward uniqueTex. Linear
     * scan — cheap at the per-frame bind volume, and only ever runs when tracing. */
    for (unsigned i = 0; i < sFrameUniqueTex; i++) {
        if (sFrameTexIds[i] == texId) {
            return;
        }
    }
    if (sFrameUniqueTex < GPUPROF_UNIQUE_TEX_MAX) {
        sFrameTexIds[sFrameUniqueTex++] = texId;
    }
    /* On overflow the count saturates at GPUPROF_UNIQUE_TEX_MAX rather than lying
     * upward — a distinct-texture population that large already answers the S12
     * memory question. */
}

void gdx3ds_gpuprof_note_verts(unsigned count) {
    if (!sEnabled) {
        return;
    }
    sFrameVerts += count;
}

void gdx3ds_gpuprof_note_shot(const char* label) {
    if (!sEnabled || label == NULL || label[0] == '\0') {
        return;
    }
    snprintf(sShotLabel, sizeof(sShotLabel), "%s", label);
    sShotPending = 1;
}

int gdx3ds_gpuprof_hud_sample(unsigned* outBuildMsX10, int* outTopOp, unsigned* outTopOpMsX10) {
    /* FPS-HUD read-only sample (header contract). sWinFrames == 0 right after a
     * window emit — the HUD then keeps its previous values for one refresh. */
    if (!sEnabled || sWinFrames == 0) {
        return 0;
    }
    if (outBuildMsX10 != NULL) {
        *outBuildMsX10 = (unsigned)(sSumBuild * 10.0f / (float)sWinFrames);
    }
    int best = -1;
    uint64_t bestTicks = 0;
    for (int op = 0; op < 256; op++) {
        if (gdx3ds_prof_op_ticks[op] > bestTicks) {
            bestTicks = gdx3ds_prof_op_ticks[op];
            best = op;
        }
    }
    if (outTopOp != NULL) {
        *outTopOp = best;
    }
    if (outTopOpMsX10 != NULL) {
        *outTopOpMsX10 = (best >= 0) ? (unsigned)(TicksToMs(bestTicks) * 10.0f / (float)sWinFrames)
                                     : 0u;
    }
    return 1;
}
