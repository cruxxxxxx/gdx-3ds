/* port/3ds/gfx/gdx3ds_overlap.c -- C5 Stage 4 "[overlap]" (see the header for the contract).
 *
 * Why this is safe against citro3d 1.7.1 + libctru's gxqueue.c (read from the sources the shipped
 * libraries were built from):
 *   - After C3D_FrameBegin the queue is stopped (curQueue = NULL, isActive = false), so every
 *     C3D_FrameSplit of the frame only appends. gxCmdQueueRun makes it active: it submits what
 *     is queued, and from then on every append submits at once (gxCmdQueueAdd: queue == curQueue
 *     && isActive && !isRunning). Append order is submission order, so the GPU sees the frame's
 *     command lists, texture copies and transfers in exactly the order it would have.
 *   - The queue's finish callback (citro3d onQueueFinish) is harmless mid-frame: needSwapTop and
 *     needSwapBot are only set inside C3D_FrameEnd, and inSafeTransfer only outside a frame.
 *   - Before C3D_FrameEnd we wait for the queue to go idle and stop it (gxCmdQueueStop on an idle
 *     queue deactivates it at once). C3D_FrameEnd then flushes, queues the display transfers and
 *     sets needSwap* BEFORE its own gxCmdQueueRun, exactly as stock: without the stop, its final
 *     split and transfers would be submitted before its linear-heap flush and before needSwapTop
 *     is set.
 *   - A HOME suspend mid-frame runs citro3d's WaitAndClear (wait, stop, clear): the chunks that
 *     were already kicked have executed, and the wait/stop below is a no-op on a stopped queue.
 *   - Coherence: the Old 3DS has no L2 and the GPU reads RAM. A kick flushes the VBO tail
 *     (the backend's watermark), the whole linear heap (the same flush C3D_FrameEnd does; lever
 *     overlap_heapflush) and the command list itself (GX_CMDLIST_FLUSH).
 *   - Resources: within a frame every CPU-written GPU buffer is append-only (the stock queue ran
 *     all draws after C3D_FrameEnd, so any in-frame reuse would already be a bug), so executing
 *     a prefix early reads the same bytes. The emulator runs a kicked list at submit time, which
 *     is what makes the [fh] parity gate a real test of that assumption. */
#include "gdx3ds_overlap.h"
#include "gdx3ds_gputl.h" /* n60gpu: [debug] gputl submit stamps */
#include "gdx3ds_hwprof.h" /* hwprof [hwsite]: kick / heapflush ([debug] hwsite, default 0) */
#include "gdx3ds_cpucut.h" /* o3ds-cpucuts: the kick diet levers' A/B blocks */
#include "nrr/gdx3ds_nrr_crumb.h" /* n60hwdiag */
#include "../vendor/citro3d/gdx/c3d_gdx.h" /* n60hwdiag: C3Dgdx_QueueWait ([debug] gxq_watch_ms) */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>

#include <3ds.h>
#include <citro3d.h>

extern int gdx3ds_config_get_int(const char* section, const char* key, int fallback)
    __attribute__((weak));
extern int gdx3ds_config_loaded(void) __attribute__((weak));
extern void gdx3ds_filelog_write(const char* msg, size_t len) __attribute__((weak));
extern int gdx3ds_rt_mode(void) __attribute__((weak));
extern int gdx3ds_tickahead_mode(void) __attribute__((weak)); /* N60TICK4 */
/* gdx3ds_frameskip.c: skip iterations right before the latest rendered one (0 = consecutive). */
extern int gdx3ds_frameskip_render_gap(void) __attribute__((weak));
extern int gdx3ds_prof_active; /* gdx3ds_gpu_prof.c: gputrace latched at the frame boundary */
/* o3ds60: the right-eye pass latched at the latest StartFrame (gdx3ds_stereo_frame.cpp) and the
 * pipe's 3D kick lever ([debug] kick_3d, gdx3ds_pipe.c). Weak: host / DL builds link neither. */
extern int gdx3ds_stereo_active(void) __attribute__((weak));
extern int gdx3ds_pipe_kick_3d(void) __attribute__((weak));
/* onesubmit (gdx3ds_stereo_frame.cpp): a 3D frame's kick policy (0 as configured, 1 no mid-frame body
 * kick, 2 no kick nor head run) and its narrow-flush arm. Weak: host / DL builds link neither. */
extern int gdx3ds_stereo_onesub_kicks(void) __attribute__((weak));
extern int gdx3ds_stereo_onesub_narrow(void) __attribute__((weak));

/* gfx_citro3d.cpp: 1 if a C3D frame is open (the VBO tail was flushed), 0 otherwise. */
extern int gdx3ds_c3d_overlap_prepare(void);

/* citro3d's context object; its first member is the frame's gxCmdQueue_s (citro3d 1.7.1
 * source/internal.h: `typedef struct { gxCmdQueue_s gxQueue; u32* cmdBuf; ...} C3D_Context`). */
extern char __C3D_Context[];
void gdx3ds_stereo_kick_chunk(void) __attribute__((weak)); /* n60stereo (gdx3ds_stereo_frame.cpp) */
void gdx3ds_stereo_kick_pre_split(void) __attribute__((weak)); /* n60tail [native] stereo_fold */
extern u32 __ctru_linear_heap;
extern u32 __ctru_linear_heap_size;

#define OVERLAP_CAT_MACH 5u
#define OVERLAP_CAT_HUD 9u
#define OVERLAP_DEFAULT_MASK ((1u << OVERLAP_CAT_MACH) | (1u << OVERLAP_CAT_HUD))
#define OVERLAP_MAX_KICKS 4
#define OVERLAP_WINDOW 256u
#define OVERLAP_CADENCE_WINDOW 1024u

/* `[debug] overlap_kick` values. hwprof2 (hwprof-progress.md §10.2): on the console the kick
 * hides ~2.4 ms of GPU wait per frame when consecutive ticks render (D=1, frameskip off) and
 * costs ~0.77 ms of whole-heap flushes per frame when they do not (steady D>=2: the previous
 * frame's GPU work finished during the skipped ticks, there is no wait to hide). */
enum {
    OVERLAP_KICK_OFF = 0,
    OVERLAP_KICK_CADENCE = 1, /* kick only in frames that follow a rendered tick (D=1) */
    OVERLAP_KICK_ALWAYS = 2,  /* the pre-quickfix behaviour, kept for A/B */
};

static int sLatched = 0;
static int sArmed = 0;
static int sKickMode = OVERLAP_KICK_OFF;
static unsigned sMask = OVERLAP_DEFAULT_MASK;
static int sHeapFlush = 1;
static int sEndFlushCmdlist = 0;
/* o3ds-cpucuts kick diet (agent-D-cpu-cuts.md §3.1), each 0 / 1 / 2 = A/B (gdx3ds_cpucut.h):
 *   [debug] overlap_hud_kick (DEFAULT 1 = the shipped mask): 0 drops the HUD category from the kick
 *     mask, so chunk c1 (mach..fx) joins the frame-end submission and that kick's split, queue run,
 *     right-eye chunk replay and resume re-sync are not paid (~0.6-0.7 ms per D=1 3D frame est.).
 *   [debug] overlap_flush_ranges (DEFAULT 0): a kick flushes only the linear ranges noted dirty since
 *     the previous submission (gdx3ds_overlap_note_dirty) through svcFlushProcessDataCache, instead
 *     of the whole 16 MiB heap through the GSP IPC (~110 us per kick on the Old 3DS, hwprof §10.4).
 *     Rests on c5overlap's flush audit (every CPU writer of GPU-read linear memory flushes its own
 *     range: the VBO pool by FlushPendingVbo right before the kick, texture uploads, HUD atlas rows,
 *     retained meshes; LUTs and uniforms ride the command list); a writer that does not is noted
 *     here. An overflowing note list falls back to the whole heap. HARDWARE-ONLY failure mode (the
 *     emulator runs a kicked list at submit, with no cache): a missed writer shows as flicker. */
static int sHudKick = 1;
static int sFlushRanges = 0;
#define OVERLAP_DIRTY_MAX 16
static uint32_t sDirtyLo[OVERLAP_DIRTY_MAX];
static uint32_t sDirtyHi[OVERLAP_DIRTY_MAX];
static int sDirtyN = 0;
static int sDirtyOverflow = 0;
/* `[overlap] diet` per cadence window: kicks the HUD lever refused, range vs whole-heap flushes. */
static unsigned sDietHudRefused = 0;
static unsigned sDietRangeKicks = 0;
static unsigned sDietHeapKicks = 0;
static unsigned sDietRanges = 0;
static uint32_t sDietRangeBytes = 0;

static unsigned sFrameKickedCats = 0;
static int sFrameKicks = 0;
static int sFrameHeadRun = 0; /* stereo-race-fps: gdx3ds_overlap_head_run started this frame's queue */
static int sFrameCadenceOk = 0; /* this frame may kick (mode ALWAYS, or it follows a rendered tick) */
static int sFrameGap = 0;
static int sFrameBodyKicksOff = 0; /* onesubmit_kicks = 1: this frame's markers kick no more (head run kept) */
static int sFrameNarrow = 0;       /* onesubmit_narrow: kicks + frame end flush only the noted ranges */
static unsigned sOsKicksRefused = 0;
static unsigned sOsNarrowFrames = 0;
static unsigned sOsNarrowEnds = 0;

/* `[overlap] cadence` receipt, every OVERLAP_CADENCE_WINDOW armed frames, gputrace or not. */
static unsigned sCadFrames = 0;
static unsigned sCadConsec = 0;   /* frames that followed a rendered tick (gap 0) */
static unsigned sCadSkipped = 0;  /* frames refused a kick by the cadence (gap > 0, mode CADENCE) */
static unsigned sCadKickFrames = 0;
static unsigned sCadKicks = 0;
static int sCadGapMax = 0;

static unsigned long sFrames = 0;
static unsigned sWinFrames = 0;
static unsigned sWinKicks = 0;
static unsigned sWinKickFrames = 0;
static u64 sWinKickTicks = 0;
static u64 sWinDrainTicks = 0;
static u64 sLastDrainTicks = 0; /* n60st3: the latest frame end's drain wait (0 = no kick) */
static int sLastKicks = 0;      /* n60st3: ...and that frame's kicks */

static void ResetDirty(void);

static void OverlapLog(const char* fmt, ...) {
    char buf[224];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) {
        return;
    }
    size_t len = (size_t)n < sizeof(buf) - 1 ? (size_t)n : sizeof(buf) - 1;
    svcOutputDebugString(buf, len);
    if (&gdx3ds_filelog_write != NULL) {
        gdx3ds_filelog_write(buf, len);
    }
}

static float TicksToMs(u64 ticks) {
    return (float)((double)ticks / (double)CPU_TICKS_PER_MSEC);
}

static gxCmdQueue_s* FrameQueue(void) {
    return (gxCmdQueue_s*)(void*)__C3D_Context;
}

static int ConfigInt(const char* key, int fallback) {
    if (&gdx3ds_config_get_int == NULL) {
        return fallback;
    }
    return gdx3ds_config_get_int("debug", key, fallback);
}

/* The queue must look like citro3d 1.7.1's (32 entries, allocated): a different citro3d layout
 * disarms the lever instead of starting a random pointer. */
static int QueueLayoutMatches(void) {
    const gxCmdQueue_s* q = FrameQueue();
    return q->entries != NULL && q->maxEntries == 32;
}

/* Why a requested lever is not armed. (Stereo is fine: the right-eye readback in
 * gdx3ds_stereo.cpp runs from the oracle after C3D_FrameEnd, outside any frame.) */
static const char* DisarmReason(int want, int rtMode, int layoutOk) {
    if (!want) {
        return "off";
    }
    if (rtMode != 0) {
        return "renderthread";
    }
    if (!layoutOk) {
        return "c3d-layout";
    }
    return "on";
}

static const char* KickModeName(int mode) {
    if (mode == OVERLAP_KICK_CADENCE) {
        return "cadence";
    } else if (mode == OVERLAP_KICK_ALWAYS) {
        return "always";
    }
    return "off";
}

static void LatchConfig(void) {
    if (&gdx3ds_config_loaded != NULL && !gdx3ds_config_loaded()) {
        return; /* not parsed yet: re-check at the next fresh frame */
    }
    sLatched = 1;
    int mode = ConfigInt("overlap_kick", OVERLAP_KICK_OFF);
    if (mode < OVERLAP_KICK_OFF || mode > OVERLAP_KICK_ALWAYS) {
        mode = OVERLAP_KICK_CADENCE;
    }
    const int want = mode != OVERLAP_KICK_OFF;
    const int wantEndFlush = ConfigInt("endflush_cmdlist", 0) != 0;
    sMask = (unsigned)ConfigInt("overlap_kick_mask", (int)OVERLAP_DEFAULT_MASK);
    sHeapFlush = ConfigInt("overlap_heapflush", 1) != 0;
    sHudKick = gdx3ds_cpucut_clamp(ConfigInt("overlap_hud_kick", 1), 1);
    sFlushRanges = gdx3ds_cpucut_clamp(ConfigInt("overlap_flush_ranges", 0), 0);
    gdx3ds_cpucut_lever("hudkick", &sHudKick, GDX3DS_CPUCUT_BIT_HUDKICK);
    gdx3ds_cpucut_lever("flushr", &sFlushRanges, GDX3DS_CPUCUT_BIT_FLUSHRANGE);
    int rtMode = (&gdx3ds_rt_mode != NULL) ? gdx3ds_rt_mode() : 0;
    /* N60TICK4: under the Old 3DS tick-ahead pipe every citro3d call (FrameBegin, the kicks, the
     * drain, FrameEnd) runs on the one render thread, exactly as the sequential path runs them on
     * main: the kick protocol is single-threaded either way. */
    const int tickAhead = &gdx3ds_tickahead_mode != NULL && gdx3ds_tickahead_mode() != 0;
    if (tickAhead && rtMode <= 2) {
        rtMode = 0;
    }
    const int layoutOk = QueueLayoutMatches();
    sArmed = want && rtMode == 0 && layoutOk;
    sKickMode = sArmed ? mode : OVERLAP_KICK_OFF;
    sEndFlushCmdlist = wantEndFlush && rtMode == 0;
    OverlapLog("[overlap] init kick=%d mode=%s mask=0x%x heapflush=%d rt=%d (%s) endflush_cmdlist=%d (%s) "
               "hud_kick=%d flush_ranges=%d", sArmed, KickModeName(mode), sMask, sHeapFlush, rtMode,
               DisarmReason(want, rtMode, layoutOk), sEndFlushCmdlist, DisarmReason(wantEndFlush, rtMode, 1), sHudKick,
               sFlushRanges);
}

const void* gdx3ds_overlap_frame_queue(void) {
    return FrameQueue();
}

int gdx3ds_overlap_markers_armed(void) {
    return sArmed;
}

int gdx3ds_overlap_frame_kicks(void) {
    return sFrameKicks + sFrameHeadRun;
}

static void EmitWindow(void) {
    if (gdx3ds_prof_active != 0) {
        const float inv = 1.0f / (float)sWinFrames;
        OverlapLog("[overlap] frame=%lu n=%u kicks=%.2f kf=%.2f kick=%.3f drain=%.3f", sFrames, sWinFrames,
                   (float)sWinKicks * inv, (float)sWinKickFrames * inv, TicksToMs(sWinKickTicks) * inv,
                   TicksToMs(sWinDrainTicks) * inv);
    }
    sWinFrames = 0;
    sWinKicks = 0;
    sWinKickFrames = 0;
    sWinKickTicks = 0;
    sWinDrainTicks = 0;
}

/* Decided once per frame, from the frameskip state as it stood when this render was decided. */
static void DecideFrameCadence(void) {
    sFrameGap = (&gdx3ds_frameskip_render_gap != NULL) ? gdx3ds_frameskip_render_gap() : 0;
    sFrameCadenceOk = sKickMode == OVERLAP_KICK_ALWAYS || sFrameGap == 0;
    /* o3ds60 [debug] kick_3d = 0: no mid-frame kick (and so no head run) while the right eye runs.
     * The frame end then has no kicked queue to drain: the GPU starts at C3D_FrameEnd, as at D >= 2. */
    if (sFrameCadenceOk && &gdx3ds_pipe_kick_3d != NULL && &gdx3ds_stereo_active != NULL &&
        gdx3ds_stereo_active() && !gdx3ds_pipe_kick_3d()) {
        sFrameCadenceOk = 0;
    }
    /* onesubmit_kicks: with both eyes in one entry per chunk, fewer (1) or no (2) GPU starts mid-frame */
    const int policy = &gdx3ds_stereo_onesub_kicks != NULL ? gdx3ds_stereo_onesub_kicks() : 0;
    if (policy == 2) {
        sFrameCadenceOk = 0;
    } else if (policy == 1) {
        sFrameBodyKicksOff = 1;
    }
}

void gdx3ds_overlap_frame_begin(void) {
    if (!sLatched) {
        LatchConfig();
    }
    sFrameKickedCats = 0;
    sFrameKicks = 0;
    sFrameHeadRun = 0;
    sFrameBodyKicksOff = 0;
    ResetDirty(); /* the previous frame's C3D_FrameEnd flushed the whole heap (or its noted ranges) */
    sFrameNarrow = sArmed && &gdx3ds_stereo_onesub_narrow != NULL && gdx3ds_stereo_onesub_narrow() != 0;
    sOsNarrowFrames += (unsigned)sFrameNarrow;
    if (sArmed) {
        DecideFrameCadence();
    }
}

unsigned gdx3ds_overlap_split_flags(void) {
    if (sFrameKicks > 0 || sFrameHeadRun || sEndFlushCmdlist || sFrameNarrow) {
        return GX_CMDLIST_FLUSH;
    }
    return 0;
}

/* onesubmit_narrow: the frame end flushes the noted ranges (gdx3ds_overlap_before_frame_end) and every
 * list carries GSP's flush, as under [debug] endflush_cmdlist, instead of the whole-heap flush. */
unsigned gdx3ds_overlap_frame_end_flags(void) {
    return (sEndFlushCmdlist || sFrameNarrow) ? GX_CMDLIST_FLUSH : 0;
}

/* o3ds-cpucuts [debug] overlap_hud_kick = 0: the HUD category kicks no more (this block). */
static int HudKickRefused(unsigned category) {
    return category == OVERLAP_CAT_HUD && !gdx3ds_cpucut_on(sHudKick, GDX3DS_CPUCUT_BIT_HUDKICK);
}

static int KickWanted(unsigned category) {
    if (!sArmed || category >= 32u) {
        return 0;
    }
    if ((sMask & (1u << category)) == 0 || (sFrameKickedCats & (1u << category)) != 0) {
        return 0;
    }
    if (!sFrameCadenceOk) {
        return 0;
    }
    if (sFrameBodyKicksOff) {
        sFrameKickedCats |= 1u << category; /* counted once per frame */
        sOsKicksRefused++;
        return 0;
    }
    if (HudKickRefused(category)) {
        sFrameKickedCats |= 1u << category; /* counted once per frame */
        sDietHudRefused++;
        return 0;
    }
    return sFrameKicks < OVERLAP_MAX_KICKS;
}

void gdx3ds_overlap_note_dirty(const void* addr, unsigned bytes) {
    const uint32_t lo = (uint32_t)(uintptr_t)addr & ~31u;
    const uint32_t hi = ((uint32_t)(uintptr_t)addr + bytes + 31u) & ~31u;
    int i;
    if (bytes == 0 || sDirtyOverflow) {
        return;
    }
    for (i = 0; i < sDirtyN; i++) {
        if (lo <= sDirtyHi[i] && hi >= sDirtyLo[i]) { /* overlapping or adjacent: merge */
            sDirtyLo[i] = lo < sDirtyLo[i] ? lo : sDirtyLo[i];
            sDirtyHi[i] = hi > sDirtyHi[i] ? hi : sDirtyHi[i];
            return;
        }
    }
    if (sDirtyN >= OVERLAP_DIRTY_MAX) {
        sDirtyOverflow = 1;
        return;
    }
    sDirtyLo[sDirtyN] = lo;
    sDirtyHi[sDirtyN] = hi;
    sDirtyN++;
}

static void ResetDirty(void) {
    sDirtyN = 0;
    sDirtyOverflow = 0;
}

static void FlushWholeHeap(void) {
    if (gdx3ds_hws_gsp_flush_kind != NULL) {
        gdx3ds_hws_gsp_flush_kind((const void*)__ctru_linear_heap, __ctru_linear_heap_size, GDX3DS_HWFL_HEAPKICK);
    } else {
        GSPGPU_FlushDataCache((const void*)__ctru_linear_heap, __ctru_linear_heap_size);
    }
}

static void FlushDirtyRanges(void) {
    int i;
    for (i = 0; i < sDirtyN; i++) {
        const uint32_t bytes = sDirtyHi[i] - sDirtyLo[i];
        svcFlushProcessDataCache(CUR_PROCESS_HANDLE, sDirtyLo[i], bytes);
        sDietRangeBytes += bytes;
    }
    sDietRanges += (unsigned)sDirtyN;
}

static void FlushLinearHeap(void) {
    if ((sFrameNarrow || gdx3ds_cpucut_on(sFlushRanges, GDX3DS_CPUCUT_BIT_FLUSHRANGE)) && !sDirtyOverflow) {
        FlushDirtyRanges();
        sDietRangeKicks++;
    } else {
        FlushWholeHeap();
        sDietHeapKicks++;
    }
    ResetDirty();
}

void gdx3ds_overlap_scene_marker(unsigned category) {
    if (!KickWanted(category)) {
        return;
    }
    sFrameKickedCats |= 1u << category;
    const u64 t0 = svcGetSystemTick();
    const uint64_t hws = gdx3ds_hws_begin(); /* hwprof [hwsite] kick */
    if (!gdx3ds_c3d_overlap_prepare()) {
        return;
    }
    if (sHeapFlush) {
        FlushLinearHeap();
    }
    NRR_CRUMB_FIRST(NRR_CRUMB_KICK, 6, "overlap kick > cat=%u kicks=%d (FrameSplit + gxCmdQueueRun)", category, sFrameKicks);
    if (&gdx3ds_stereo_kick_pre_split != NULL) {
        gdx3ds_stereo_kick_pre_split(); /* n60tail: the right prologue may close this list (stereo_fold) */
    }
    C3D_FrameSplit(GX_CMDLIST_FLUSH);
    gdx3ds_gputl_note_kick(0); /* n60gpu [debug] gputl: the queued entries start now */
    gxCmdQueueRun(FrameQueue());
    if (&gdx3ds_stereo_kick_chunk != NULL) {
        gdx3ds_stereo_kick_chunk(); /* n60stereo: the chunk's right eye right behind it */
    }
    gdx3ds_gputl_note_kick(1); /* n60gpu [debug] gputl: ...and the right-eye chunk from here */
    sFrameKicks++;
    gdx3ds_hws_end(GDX3DS_HWS_KICK, hws);
    sWinKickTicks += svcGetSystemTick() - t0;
}

int gdx3ds_overlap_head_run(void) {
    if (!sArmed || !sFrameCadenceOk || sFrameKicks != 0 || sFrameHeadRun) {
        return 0;
    }
    if (!gdx3ds_c3d_overlap_prepare()) {
        return 0; /* no C3D frame open */
    }
    gdx3ds_gputl_note_kick(0); /* n60gpu [debug] gputl: a submit point, not a new chunk */
    gxCmdQueueRun(FrameQueue());
    sFrameHeadRun = 1;
    return 1;
}

static void DrainAndStopQueue(void) {
    const u64 t0 = svcGetSystemTick();
    gxCmdQueue_s* q = FrameQueue();
    NRR_CRUMB_FIRST(NRR_CRUMB_KICK, 12, "overlap drain > gxCmdQueueWait (kicked GPU work must finish) kicks=%d", sFrameKicks);
    C3Dgdx_QueueWait(q, -1, "overlap drain (DrainAndStopQueue)"); /* stock wait unless gxq_watch_ms */
    gxCmdQueueStop(q);
    sLastDrainTicks = svcGetSystemTick() - t0;
    sWinDrainTicks += sLastDrainTicks;
}

unsigned long long gdx3ds_overlap_last_drain_ticks(int* kicks) {
    *kicks = sLastKicks;
    return sLastDrainTicks;
}

static void EmitDietWindow(void) {
    if (sOsKicksRefused + sOsNarrowFrames > 0u) {
        OverlapLog("[overlap] onesub body_kicks_refused=%u narrow_frames=%u narrow_ends=%u", sOsKicksRefused,
                   sOsNarrowFrames, sOsNarrowEnds);
        sOsKicksRefused = 0;
        sOsNarrowFrames = 0;
        sOsNarrowEnds = 0;
    }
    if (sHudKick == 1 && sFlushRanges == 0) {
        return;
    }
    OverlapLog("[overlap] diet hud_kick=%d flush_ranges=%d hud_refused=%u range_kicks=%u heap_kicks=%u ranges=%u "
               "range_kb=%lu", sHudKick, sFlushRanges, sDietHudRefused, sDietRangeKicks, sDietHeapKicks, sDietRanges,
               (unsigned long)(sDietRangeBytes / 1024u));
    sDietHudRefused = 0;
    sDietRangeKicks = 0;
    sDietHeapKicks = 0;
    sDietRanges = 0;
    sDietRangeBytes = 0;
}

static void EmitCadenceWindow(void) {
    OverlapLog("[overlap] cadence mode=%s frames=%u consec=%u kickf=%u kicks=%u cad_skip=%u gapmax=%d",
               KickModeName(sKickMode), sCadFrames, sCadConsec, sCadKickFrames, sCadKicks, sCadSkipped,
               sCadGapMax);
    EmitDietWindow();
    sCadFrames = 0;
    sCadConsec = 0;
    sCadSkipped = 0;
    sCadKickFrames = 0;
    sCadKicks = 0;
    sCadGapMax = 0;
}

static void NoteCadence(int kicks) {
    sCadFrames++;
    if (sFrameGap == 0) {
        sCadConsec++;
    } else if (!sFrameCadenceOk) {
        sCadSkipped++;
    }
    if (sFrameGap > sCadGapMax) {
        sCadGapMax = sFrameGap;
    }
    if (kicks > 0) {
        sCadKickFrames++;
        sCadKicks += (unsigned)kicks;
    }
    if (sCadFrames >= OVERLAP_CADENCE_WINDOW) {
        EmitCadenceWindow();
    }
}

/* onesubmit_narrow: what C3D_FrameEnd's whole-heap flush covered, now that it passes GX_CMDLIST_FLUSH:
 * the ranges noted since the last kick (an overflowed note list: the whole heap). */
static void NarrowFrameEndFlush(void) {
    if (!sFrameNarrow) {
        return;
    }
    if (sDirtyOverflow) {
        FlushWholeHeap();
    } else {
        FlushDirtyRanges();
    }
    ResetDirty();
    sOsNarrowEnds++;
}

void gdx3ds_overlap_before_frame_end(void) {
    const int kicks = sFrameKicks;
    sLastDrainTicks = 0;
    sLastKicks = kicks;
    if (sFrameKicks > 0 || sFrameHeadRun) {
        DrainAndStopQueue();
        sWinKicks += (unsigned)sFrameKicks;
        sWinKickFrames++;
        sFrameKicks = 0;
        sFrameHeadRun = 0;
    }
    NarrowFrameEndFlush();
    if (!sArmed) {
        return;
    }
    NoteCadence(kicks);
    sFrames++;
    if (++sWinFrames >= OVERLAP_WINDOW) {
        EmitWindow();
    }
}
