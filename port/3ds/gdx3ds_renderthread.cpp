/* port/3ds/gdx3ds_renderthread.cpp -- render thread on core 2. Contract: the header and
 * docs/research/renderthread-audit.md (sections 5-8).
 *
 * Command ring: main is the only producer, the render thread the only consumer. A slot is
 * fully written before the producer's release-store of sHead; the consumer's acquire-load of
 * sHead orders the slot reads. The LightSemaphore only counts (its own atomics do not need to
 * carry the payload ordering). Completion: sDone (release) + a sticky LightEvent; the single
 * waiter (main) clears the event BEFORE re-checking sDone, so a signal landing between the
 * check and the wait is never lost (see home-crash-audit.md for the shared-event spin bug this
 * shape avoids: one waiter, one event, the condition only changes by the event's signaler).
 *
 * Modes (gdx3ds_rt_mode): 1 sync, 2 pipe (DP-done when render(N) completes, at the host join),
 * 3 ahead (DP-done acknowledged as soon as the game parks on it; the pool-half protection moves
 * to the NEXT submit, which waits for the previous task; game-side RDRAM writers -- DMA loads,
 * segment reloads, captures -- fence against the in-flight task instead of relying on the N64's
 * DP-done ordering).
 *
 * BRIDGE ON MAIN (`[debug] bridgemain`, default 1; docs/research/balance-progress.md): the
 * bridge stage of a task (adapter construction, ConvertRoot/ProcessList, per-task memos, the
 * segment snapshot) runs on the SUBMITTING thread inside osSpTaskStartGo -- while the render
 * thread is still drawing the previous task -- and the TASK slot carries an already-converted
 * job; core 2 then runs interpreter + draws only. The job is released (ConvertedList recycle,
 * persistent-copy frees, native-RGBA16 retirements) on main once its render completed, so every
 * walk-side container has exactly one owner thread. 0 = the TASK carries the raw DL and the
 * render thread runs the whole gdx_gfx_run, byte-identical to before. */
#include "gdx3ds_renderthread.h"

#include <3ds.h>
#include <atomic>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "gdx3ds_config.h"
#include "gdx3ds_dynlod.h"
#include "gdx3ds_filelog.h"
#include "gfx/gdx3ds_gpu_prof.h"
#include "n64_gfx_bridge.h" /* gdx_gfx_run + GdxTaskUcode + segment view API */

__thread int gdx_port_log_console_muted = 0;

namespace {

enum RtMode { kOff = 0, kSync = 1, kPipe = 2, kAhead = 3 };
enum RtCmd { kCmdBegin = 1, kCmdTask = 2, kCmdEnd = 3, kCmdQuit = 4 };

struct RtSlot {
    int cmd;
    int ucode;
    void* dl;
    size_t dlSize;
    uint64_t submitTick;
    uintptr_t segs[16]; /* TASK: gSegments snapshot at submit (audit section 4) */
    GdxGfxJob* job;     /* TASK, bridgemain: the converted job (owns its own segment view) */
};

constexpr int kRingSize = 8; /* BEGIN + tasks + END per iteration; never more than a few live */
constexpr int kRtPrio = 0x24;
constexpr int kRtCore = 2;
constexpr size_t kRtStack = 192u * 1024u;

RtSlot sRing[kRingSize];
std::atomic<uint32_t> sHead{0}; /* commands produced */
std::atomic<uint32_t> sDone{0}; /* commands completed */
std::atomic<uint32_t> sTaskSeq{0};       /* sequence number of the last submitted TASK (0 = none) */
std::atomic<uint32_t> sTaskDoneSeq{0};   /* sequence number of the last completed TASK */
uintptr_t sTaskSegsOut[16];              /* the walk's final segment view (render -> main, published by sTaskDoneSeq) */
uint32_t sTaskMerged = 0;                /* last TASK seq whose claims were merged (main only) */
uint32_t sTaskAcked = 0;                 /* last TASK seq whose DP-done was posted (main only) */
uint64_t sTaskSubmitTick = 0;            /* main: tick of the last submit (overlap accounting) */
int sTaskOverlapCounted = 0;
LightSemaphore sCmdSem;
LightEvent sDoneEvent;
Thread sThread = nullptr;
int sMode = kOff;
int sInited = 0;
int sBridgeMain = 0;             /* [debug] bridgemain: the bridge stage runs on the submitter */
GdxGfxJob* sJob = nullptr;       /* main only: the job of the last submitted TASK (released once its render is observed complete) */
__thread int tOnRenderThread = 0;

/* Telemetry (window accumulators; main writes its own words, the render thread its own). */
uint64_t sWaitMainTicks = 0;   /* all main waits */
uint64_t sWaitTopTicks = 0;    /* main: loop-top join (END) */
uint64_t sWaitDpTicks = 0;     /* main: DP join / submit backpressure */
uint64_t sOverlapTicks = 0;    /* main: submit -> first wait for that task (logic overlapped) */
uint64_t sBridgeMainTicks = 0; /* main: the bridge stage (prepare) wall, bridgemain only */
uint32_t sBridgeMainTasks = 0; /* main: tasks whose bridge ran on the submitter */
uint32_t sBridgeReleaseWaits = 0; /* main: release-before-walk waits (native-RGBA16 retirements) */
uint64_t sWaitRenderTicks = 0; /* render: idle inside an open frame */
uint64_t sBeginTicks = 0, sTaskTicks = 0, sEndTicks = 0, sQueueLatTicks = 0; /* render exec */
uint32_t sTasksWindow = 0;
uint32_t sFencesWindow = 0;
uint32_t sDmaFencesWindow = 0;
uint32_t sSegMergeWindow = 0;
std::atomic<uint32_t> sTexCacheMainMut{0}; /* texture-cache calls from a non-render thread (must be 0) */
uint32_t sFramesWindow = 0;
uint32_t sPaceWaits = 0;
uint32_t sPaceSkips = 0;
uint32_t sTaskBeforeBegin = 0; /* TASK produced while no BEGIN was queued this iteration (ordering receipt) */
int sBeginQueued = 0;
uint64_t sPrevTopTick = 0;
uint64_t sVblankPeriodTicks = 0;
GdxRtCallbacks sCb = {};

void RtLog(const char* msg) {
    svcOutputDebugString(msg, strlen(msg));
    gdx3ds_filelog_write(msg, strlen(msg));
}

void RtLogf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void RtLogf(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
        RtLog(buf);
    }
}

/* "Main" = the host loop thread OR any game fiber it schedules: the fibers are real libctru
 * threads (gdx_fiber_3ds.c), so a raw svcGetThreadId compare would make every fence from
 * game code a no-op. n64_sched.c's logical-id test is the one that knows about fibers. */
extern "C" int gdx_sched_on_host_thread(void);
bool OnMainThread() {
    return gdx_sched_on_host_thread() != 0;
}

/* Wait until sDone >= target. Single waiter (main). Returns the ticks spent waiting. */
uint64_t WaitDone(uint32_t target) {
    const uint64_t t0 = svcGetSystemTick();
    for (;;) {
        LightEvent_Clear(&sDoneEvent); /* clear FIRST, then check: a signal after the clear is kept */
        if ((int32_t)(sDone.load(std::memory_order_acquire) - target) >= 0) {
            break;
        }
        LightEvent_Wait(&sDoneEvent);
    }
    const uint64_t dt = svcGetSystemTick() - t0;
    sWaitMainTicks += dt;
    return dt;
}

extern "C" uintptr_t gSegments[16];

uint32_t Produce(int cmd, void* dl, size_t dlSize, int ucode, GdxGfxJob* job = nullptr) {
    const uint32_t seq = sHead.load(std::memory_order_relaxed);
    /* Backpressure: the ring can hold kRingSize outstanding commands; the loop protocol never
     * exceeds BEGIN + a few TASKs + END, but a pathological transition tick could -- wait for
     * room rather than overwrite. */
    while (seq - sDone.load(std::memory_order_acquire) >= (uint32_t)kRingSize) {
        WaitDone(seq - (uint32_t)kRingSize + 1u);
    }
    RtSlot& s = sRing[seq % kRingSize];
    s.cmd = cmd;
    s.dl = dl;
    s.dlSize = dlSize;
    s.ucode = ucode;
    s.submitTick = svcGetSystemTick();
    s.job = job;
    if (cmd == kCmdTask && job == nullptr) {
        memcpy(s.segs, gSegments, sizeof(s.segs)); /* game fiber context: the table is quiescent */
    }
    sHead.store(seq + 1u, std::memory_order_release);
    LightSemaphore_Release(&sCmdSem, 1);
    return seq + 1u; /* the sDone value that marks this command complete */
}

void RenderThreadMain(void*) {
    tOnRenderThread = 1;
    gdx_port_log_console_muted = 1; /* console is main-only */
    uint32_t tail = 0;
    bool frameOpen = false;
    for (;;) {
        const uint64_t w0 = svcGetSystemTick();
        LightSemaphore_Acquire(&sCmdSem, 1);
        const uint64_t x0 = svcGetSystemTick();
        if (frameOpen) {
            sWaitRenderTicks += x0 - w0; /* starved inside an open frame */
        }
        /* The semaphore count guarantees sHead > tail; the acquire load orders the slot. */
        (void)sHead.load(std::memory_order_acquire);
        const RtSlot s = sRing[tail % kRingSize];
        tail++;
        bool quit = false;
        switch (s.cmd) {
            case kCmdBegin:
                if (sCb.startFrame != nullptr) {
                    sCb.startFrame();
                }
                frameOpen = true;
                sBeginTicks += svcGetSystemTick() - x0;
                break;
            case kCmdTask: {
                sQueueLatTicks += x0 - s.submitTick;
                if (s.job != nullptr) {
                    /* bridgemain: already converted on the submitter; interpreter + draws only.
                       The job's segment view (walk-time claims included) is the run's table and
                       what the join merges. The job stays alive until main observes this
                       command's completion (sDone / sTaskDoneSeq) and releases it. */
                    uintptr_t* const segs = gdx_gfx_job_segments(s.job);
                    gdx_gfx_segment_view_set(segs);
                    gdx3ds_dynlod_task_begin(); /* DYNLOD: per-frame render-time signal */
                    gdx_gfx_job_run(s.job);
                    gdx3ds_dynlod_task_end();
                    gdx_gfx_segment_view_set(nullptr);
                    memcpy(sTaskSegsOut, segs, sizeof(sTaskSegsOut));
                } else {
                    uintptr_t segs[16];
                    memcpy(segs, s.segs, sizeof(segs));
                    gdx_gfx_segment_view_set(segs); /* job-private table for the walk */
                    gdx3ds_dynlod_task_begin();
                    gdx_gfx_run(s.dl, s.dlSize, (GdxTaskUcode)s.ucode);
                    gdx3ds_dynlod_task_end();
                    gdx_gfx_segment_view_set(nullptr);
                    memcpy(sTaskSegsOut, segs, sizeof(sTaskSegsOut));
                }
                frameOpen = true; /* Run opens the C3D frame itself when BEGIN has not */
                sTaskTicks += svcGetSystemTick() - x0;
                sTaskDoneSeq.store(tail, std::memory_order_release);
                break;
            }
            case kCmdEnd:
                if (sCb.presentFallback != nullptr) {
                    sCb.presentFallback();
                }
                if (sCb.endFrame != nullptr) {
                    sCb.endFrame();
                }
                frameOpen = false;
                sEndTicks += svcGetSystemTick() - x0;
                break;
            case kCmdQuit:
            default:
                quit = true;
                break;
        }
        sDone.store(tail, std::memory_order_release);
        LightEvent_Signal(&sDoneEvent);
        if (quit) {
            break;
        }
    }
}

const char* ModeName(int m) {
    switch (m) {
        case kSync: return "sync";
        case kPipe: return "pipe";
        case kAhead: return "ahead";
        default: return "off";
    }
}

/* Main only: block until the in-flight TASK completed and persist its segment claims once.
 * Does NOT post DP-done: that is the join's job (gdx3ds_rt_wait_task) so a fence from a game
 * fiber cannot double-post or strand the game's DP wait. */
void WaitTaskDone(uint64_t* bucket) {
    const uint32_t t = sTaskSeq.load(std::memory_order_acquire);
    if (t != 0) {
        if (!sTaskOverlapCounted) {
            sTaskOverlapCounted = 1;
            sOverlapTicks += svcGetSystemTick() - sTaskSubmitTick; /* main's work while the task ran */
        }
        const uint64_t dt = WaitDone(t);
        if (bucket != nullptr) {
            *bucket += dt;
        }
        if (sTaskMerged != t) {
            sTaskMerged = t;
            sSegMergeWindow += (uint32_t)gdx_gfx_segment_claims_merge(sTaskSegsOut);
            /* bridgemain: the render is done and main observed it -- release the job here, on
               main (pool recycle, persistent-copy frees, native-RGBA16 retirements). */
            if (sJob != nullptr) {
                gdx_gfx_job_release(sJob);
                sJob = nullptr;
            }
        }
    }
}

bool TaskRendering() {
    const uint32_t t = sTaskSeq.load(std::memory_order_acquire);
    return t != 0 && (int32_t)(sTaskDoneSeq.load(std::memory_order_acquire) - t) < 0;
}

} // namespace

extern "C" int gdx3ds_rt_init(const GdxRtCallbacks* cb) {
    if (sInited) {
        return sMode;
    }
    sInited = 1;
    sMode = kOff;
    const int key = gdx3ds_config_get_int("debug", "renderthread", 1);
    if (cb == nullptr || key <= 0) {
        RtLog("[rt] off (debug.renderthread=0): sequential path");
        return sMode;
    }
    bool isNew3ds = false;
    APT_CheckNew3DS(&isNew3ds);
    if (!isNew3ds) {
        RtLog("[rt] unavailable: not a New 3DS (core 2 is New3DS-only); sequential path");
        return sMode;
    }
    sCb = *cb;
    LightSemaphore_Init(&sCmdSem, 0, 0x7FFF);
    LightEvent_Init(&sDoneEvent, RESET_STICKY);
    sVblankPeriodTicks = (uint64_t)(CPU_TICKS_PER_MSEC * (1000.0 / 59.8337));
    sThread = threadCreate(RenderThreadMain, nullptr, kRtStack, kRtPrio, kRtCore, false);
    if (sThread == nullptr) {
        RtLog("[rt] unavailable: threadCreate(core 2) failed; sequential path");
        return sMode;
    }
    const int forceSync = gdx3ds_config_get_bool("debug", "renderthread_sync", 0);
    sMode = forceSync ? kSync : (key >= 2 ? kAhead : kPipe);
    sBridgeMain = gdx3ds_config_get_bool("debug", "bridgemain", 1) ? 1 : 0;
    gdx3ds_gpuprof_set_external_pacing(1); /* render-thread FrameBegin never vblank-syncs */
    RtLogf("[rt] mode=%s core=%d prio=0x%02X stack=%uK bridgemain=%d", ModeName(sMode), kRtCore,
           kRtPrio, (unsigned)(kRtStack / 1024u), sBridgeMain);
    return sMode;
}

extern "C" int gdx3ds_rt_mode(void) {
    return sMode;
}

extern "C" int gdx3ds_rt_on_render_thread(void) {
    return tOnRenderThread;
}

extern "C" int gdx3ds_rt_idle(void) {
    if (sMode == kOff) {
        return 1;
    }
    return sDone.load(std::memory_order_acquire) == sHead.load(std::memory_order_relaxed);
}

extern "C" void gdx3ds_rt_join_idle(void) {
    if (sMode == kOff || tOnRenderThread) {
        return;
    }
    sWaitTopTicks += WaitDone(sHead.load(std::memory_order_relaxed));
}

extern "C" void gdx3ds_rt_pace_vblank(void) {
    if (sMode == kOff) {
        return;
    }
    const uint64_t now = svcGetSystemTick();
    if (sPrevTopTick != 0 && (now - sPrevTopTick) < sVblankPeriodTicks) {
        gspWaitForVBlank(); /* the LCD boundary is still ahead: align to it */
        sPaceWaits++;
    } else {
        sPaceSkips++; /* already past the boundary: waiting buys nothing (gpuprof policy) */
    }
    sPrevTopTick = svcGetSystemTick();
}

extern "C" void gdx3ds_rt_frame_begin(void) {
    if (sMode == kOff) {
        return;
    }
    sFramesWindow++;
    sBeginQueued = 1;
    const uint32_t t = Produce(kCmdBegin, nullptr, 0, 0);
    if (sMode == kSync) {
        WaitDone(t);
    }
}

extern "C" int gdx3ds_rt_submit_task(void* dl, size_t dlSize, int ucode) {
    if (sMode == kOff) {
        return 0;
    }
    if (tOnRenderThread) {
        /* A task submitted from inside a render job cannot happen (osSpTaskStartGo is a game
         * fiber); refuse loudly rather than deadlock. */
        RtLog("[rt] ERROR: submit from the render thread; running inline");
        return 0;
    }
    /* BRIDGE ON MAIN: convert this task here, BEFORE the backpressure wait, so the bridge of
     * N+1 overlaps the tail of render(N) (that wait is exactly the slack main had). The walk
     * reads pool half N+1 (logic done) and the static tables; everything it mutates is
     * job-owned or walk-owned (n64_gfx_bridge.cpp GdxGfxJob), and the rare writers that
     * render(N) could still be reading (raw texture-copy refreshes, first-load claims) fence
     * against it. Two exceptions keep the sequential semantics exact: (1) if render(N) is
     * already over, observe it first (segment-claim merge + release) so this walk sees the
     * same tables the sequential path would; (2) if job N carries deferred native-RGBA16
     * retirements, its release mutates the classification tables this walk reads -- wait for
     * it and release it first (transition frames only, counted as relWait=). */
    GdxGfxJob* job = nullptr;
    if (sBridgeMain) {
        if (sJob != nullptr && (!TaskRendering() || gdx_gfx_job_release_before_walk(sJob))) {
            if (TaskRendering()) {
                sBridgeReleaseWaits++;
            }
            WaitTaskDone(&sWaitDpTicks); /* merges + releases sJob */
        }
        const uint64_t b0 = svcGetSystemTick();
        job = gdx_gfx_job_prepare(dl, dlSize, (GdxTaskUcode)ucode);
        sBridgeMainTicks += svcGetSystemTick() - b0;
        sBridgeMainTasks++;
    }
    /* One TASK in flight at a time. pipe: a second task in the same iteration (transition
     * ticks) waits for the previous one and hands its DP-done to the game first. ahead: this
     * is THE backpressure -- the game is about to write the pool half the previous task reads
     * (audit section 8), so wait for it here; its DP-done was acknowledged early. */
    if (gdx3ds_rt_task_pending() || TaskRendering()) {
        WaitTaskDone(&sWaitDpTicks);
        const uint32_t prev = sTaskSeq.load(std::memory_order_acquire);
        if (sTaskAcked != prev) {
            sTaskAcked = prev;
            gdx3ds_rt_post_dp_done();
        }
    }
    if (sJob != nullptr) {
        /* Unreachable by construction (the previous task was observed above or before the
           prepare); defensive: observe it properly (merge + release), never free blind. */
        WaitTaskDone(&sWaitDpTicks);
    }
    sJob = job;
    sTasksWindow++;
    if (!sBeginQueued) {
        sTaskBeforeBegin++; /* Run's re-entrant StartFrame opens the frame; harmless but logged */
    }
    const uint32_t t = Produce(kCmdTask, dl, dlSize, ucode, job);
    sTaskSeq.store(t, std::memory_order_release);
    sTaskSubmitTick = svcGetSystemTick();
    sTaskOverlapCounted = 0;
    if (sMode == kSync) {
        WaitTaskDone(&sWaitDpTicks); /* waits + merges the segment claims */
        sTaskAcked = t;              /* the caller (osSpTaskStartGo) posts SP + DP itself, as before */
        return 1;
    }
    return 2;
}

/* "A task whose DP-done the game has not received yet" -- NOT "still rendering": a fence may
 * have waited for the render without acking, and the join must still post the DP-done. */
extern "C" int gdx3ds_rt_task_pending(void) {
    if (sMode == kOff) {
        return 0;
    }
    const uint32_t t = sTaskSeq.load(std::memory_order_acquire);
    return t != 0 && sTaskAcked != t;
}

extern "C" void gdx3ds_rt_wait_task(void) {
    if (sMode == kOff || tOnRenderThread) {
        return;
    }
    const uint32_t t = sTaskSeq.load(std::memory_order_acquire);
    if (sMode == kAhead) {
        /* Early acknowledgement: the game parks on DP-done(N) after logic(N+1); render(N) may
         * still be running. Let the game swap + submit N+1; that submit waits for render(N)
         * before the pool half N is rewritten by logic(N+2). */
        if (t != 0 && sTaskAcked != t) {
            sTaskAcked = t;
            gdx3ds_rt_post_dp_done();
        }
        return;
    }
    WaitTaskDone(&sWaitDpTicks);
    if (t != 0 && sTaskAcked != t) {
        sTaskAcked = t; /* the game's DP-done for this task: exactly once */
        gdx3ds_rt_post_dp_done();
    }
}

extern "C" void gdx3ds_rt_frame_end(void) {
    if (sMode == kOff) {
        return;
    }
    sBeginQueued = 0;
    const uint32_t t = Produce(kCmdEnd, nullptr, 0, 0);
    if (sMode == kSync) {
        WaitDone(t);
    }
}

extern "C" void gdx3ds_rt_fence(void) {
    if (sMode == kOff || tOnRenderThread || !OnMainThread()) {
        return; /* foreign threads (audio/preload) never wait on main's event */
    }
    if (TaskRendering()) {
        sFencesWindow++;
        WaitTaskDone(nullptr); /* DP-done is still posted by the host's join */
    }
}

/* RDRAM writer fence (DMA loads, MIO0 decodes, asset copies). Only the ahead mode needs it:
 * pipe/sync keep the N64 ordering (the game writes after the DP-done it waited for). */
extern "C" void gdx3ds_rt_fence_dma(void) {
    if (sMode != kAhead || tOnRenderThread || !OnMainThread()) {
        return;
    }
    if (TaskRendering()) {
        sDmaFencesWindow++;
        WaitTaskDone(nullptr);
    }
}

extern "C" void gdx3ds_rt_emit_receipt(unsigned long frame) {
    if (sMode == kOff) {
        return;
    }
    const float n = sFramesWindow != 0 ? (float)sFramesWindow : 1.0f;
    const double k = CPU_TICKS_PER_MSEC;
    RtLogf("[rt] mode=%s frame=%lu n=%u tasks=%u waitMain=%.2f waitRender=%.2f fence=%u/%u "
           "segMerge=%u pace=%u/%u latency=1 | ovl=%.2f jdp=%.2f jtop=%.2f brMain=%.2f bm=%u "
           "relWait=%u | bg=%.2f tk=%.2f en=%.2f ql=%.2f tb=%u texcacheMainMut=%u",
           ModeName(sMode), frame, (unsigned)sFramesWindow, (unsigned)sTasksWindow,
           (float)((double)sWaitMainTicks / k) / n, (float)((double)sWaitRenderTicks / k) / n,
           (unsigned)sFencesWindow, (unsigned)sDmaFencesWindow, (unsigned)sSegMergeWindow,
           (unsigned)sPaceWaits, (unsigned)sPaceSkips, (float)((double)sOverlapTicks / k) / n,
           (float)((double)sWaitDpTicks / k) / n, (float)((double)sWaitTopTicks / k) / n,
           (float)((double)sBridgeMainTicks / k) / n, (unsigned)sBridgeMainTasks,
           (unsigned)sBridgeReleaseWaits,
           (float)((double)sBeginTicks / k) / n, (float)((double)sTaskTicks / k) / n,
           (float)((double)sEndTicks / k) / n, (float)((double)sQueueLatTicks / k) / n,
           (unsigned)sTaskBeforeBegin, (unsigned)sTexCacheMainMut.load(std::memory_order_relaxed));
    sWaitMainTicks = sWaitRenderTicks = sWaitTopTicks = sWaitDpTicks = sOverlapTicks = 0;
    sBridgeMainTicks = 0;
    sBridgeMainTasks = sBridgeReleaseWaits = 0;
    sBeginTicks = sTaskTicks = sEndTicks = sQueueLatTicks = 0;
    sTasksWindow = sFencesWindow = sDmaFencesWindow = sSegMergeWindow = sFramesWindow = 0;
    sPaceWaits = sPaceSkips = sTaskBeforeBegin = 0;
}

extern "C" void gdx3ds_rt_shutdown(void) {
    if (sMode == kOff || sThread == nullptr) {
        return;
    }
    gdx3ds_rt_join_idle();
    if (sJob != nullptr) {
        gdx_gfx_job_release(sJob); /* its render completed at the join above */
        sJob = nullptr;
    }
    const uint32_t t = Produce(kCmdQuit, nullptr, 0, 0);
    WaitDone(t);
    threadJoin(sThread, U64_MAX);
    threadFree(sThread);
    sThread = nullptr;
    sMode = kOff;
}

/* Texture-cache ownership receipt (libultraship interpreter.cpp hook, lus-renderthread-
 * texcache-owner.patch): counts entry points reached from a thread other than the render
 * thread while it owns the cache. Cumulative (never reset) so a single violation stays visible
 * in every later [rt] line; the first eight are also logged with the kind. */
extern "C" void gdx3ds_texcache_note_thread(int kind) {
    if (sMode == kOff || tOnRenderThread) {
        return;
    }
    const uint32_t n = sTexCacheMainMut.fetch_add(1u, std::memory_order_relaxed) + 1u;
    if (n <= 8u) {
        RtLogf("[rt] texcache mutation off the render thread: kind=%d count=%u", kind, (unsigned)n);
    }
}
