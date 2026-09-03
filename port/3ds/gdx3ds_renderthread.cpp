/* port/3ds/gdx3ds_renderthread.cpp -- render thread on core 2. Contract: the header and
 * docs/research/renderthread-audit.md (sections 5-7).
 *
 * Command ring: main is the only producer, the render thread the only consumer. A slot is
 * fully written before the producer's release-store of sHead; the consumer's acquire-load of
 * sHead orders the slot reads. The LightSemaphore only counts (its own atomics do not need to
 * carry the payload ordering). Completion: sDone (release) + a sticky LightEvent; the single
 * waiter (main) clears the event BEFORE re-checking sDone, so a signal landing between the
 * check and the wait is never lost (see home-crash-audit.md for the shared-event spin bug this
 * shape avoids: one waiter, one event, the condition only changes by the event's signaler). */
#include "gdx3ds_renderthread.h"

#include <3ds.h>
#include <atomic>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "gdx3ds_config.h"
#include "gdx3ds_filelog.h"
#include "gfx/gdx3ds_gpu_prof.h"

#include "n64_gfx_bridge.h" /* gdx_gfx_run + GdxTaskUcode */

__thread int gdx_port_log_console_muted = 0;

namespace {

enum RtMode { kOff = 0, kSync = 1, kPipe = 2 };
enum RtCmd { kCmdBegin = 1, kCmdTask = 2, kCmdEnd = 3, kCmdQuit = 4 };

struct RtSlot {
    int cmd;
    int ucode;
    void* dl;
    size_t dlSize;
    uintptr_t segs[16]; /* TASK: gSegments snapshot at submit (audit section 4) */
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
uint32_t sSegMergeWindow = 0;
uint32_t sTaskMerged = 0;                /* last TASK seq whose claims were merged (main only) */
uint32_t sTaskAcked = 0;                 /* last TASK seq whose DP-done was posted (main only) */
LightSemaphore sCmdSem;
LightEvent sDoneEvent;
Thread sThread = nullptr;
int sMode = kOff;
int sInited = 0;
u32 sRenderThreadId = 0;
__thread int tOnRenderThread = 0;

/* Telemetry (window accumulators; main writes waitMain/fence, the render thread writes
 * waitRender -- distinct words, each single-writer). */
uint64_t sWaitMainTicks = 0;
uint64_t sWaitRenderTicks = 0;
uint32_t sTasksWindow = 0;
uint32_t sFencesWindow = 0;
uint32_t sFramesWindow = 0;
uint32_t sPaceWaits = 0;
uint32_t sPaceSkips = 0;
uint64_t sPrevTopTick = 0;
uint64_t sVblankPeriodTicks = 0;
GdxRtCallbacks sCb = {};

void RtLog(const char* msg) {
    svcOutputDebugString(msg, strlen(msg));
    gdx3ds_filelog_write(msg, strlen(msg));
}

void RtLogf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void RtLogf(const char* fmt, ...) {
    char buf[224];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
        RtLog(buf);
    }
}

/* Wait until sDone >= target. Single waiter (main). */
void WaitDone(uint32_t target) {
    const uint64_t t0 = svcGetSystemTick();
    for (;;) {
        LightEvent_Clear(&sDoneEvent); /* clear FIRST, then check: a signal after the clear is kept */
        if ((int32_t)(sDone.load(std::memory_order_acquire) - target) >= 0) {
            break;
        }
        LightEvent_Wait(&sDoneEvent);
    }
    sWaitMainTicks += svcGetSystemTick() - t0;
}

extern "C" uintptr_t gSegments[16];

uint32_t Produce(int cmd, void* dl, size_t dlSize, int ucode) {
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
    if (cmd == kCmdTask) {
        memcpy(s.segs, gSegments, sizeof(s.segs)); /* game fiber context: the table is quiescent */
    }
    sHead.store(seq + 1u, std::memory_order_release);
    LightSemaphore_Release(&sCmdSem, 1);
    return seq + 1u; /* the sDone value that marks this command complete */
}

void RenderThreadMain(void*) {
    tOnRenderThread = 1;
    gdx_port_log_console_muted = 1; /* console is main-only */
    svcGetThreadId(&sRenderThreadId, CUR_THREAD_HANDLE);
    uint32_t tail = 0;
    bool frameOpen = false;
    for (;;) {
        const uint64_t w0 = svcGetSystemTick();
        LightSemaphore_Acquire(&sCmdSem, 1);
        if (frameOpen) {
            sWaitRenderTicks += svcGetSystemTick() - w0; /* starved inside an open frame */
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
                break;
            case kCmdTask: {
                uintptr_t segs[16];
                memcpy(segs, s.segs, sizeof(segs));
                gdx_gfx_segment_view_set(segs); /* job-private table for the walk */
                gdx_gfx_run(s.dl, s.dlSize, (GdxTaskUcode)s.ucode);
                gdx_gfx_segment_view_set(nullptr);
                memcpy(sTaskSegsOut, segs, sizeof(sTaskSegsOut));
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

} // namespace

static void WaitTaskDone(void);

extern "C" int gdx3ds_rt_init(const GdxRtCallbacks* cb) {
    if (sInited) {
        return sMode;
    }
    sInited = 1;
    sMode = kOff;
    if (cb == nullptr || !gdx3ds_config_get_bool("debug", "renderthread", 1)) {
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
    const int forceSync = gdx3ds_config_get_bool("debug", "renderthread_sync", 0); /* M3: pipe default */
    sMode = forceSync ? kSync : kPipe;
    gdx3ds_gpuprof_set_external_pacing(1); /* render-thread FrameBegin never vblank-syncs */
    RtLogf("[rt] mode=%s core=%d prio=0x%02X stack=%uK", sMode == kSync ? "sync" : "pipe", kRtCore,
           kRtPrio, (unsigned)(kRtStack / 1024u));
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
    WaitDone(sHead.load(std::memory_order_relaxed));
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
    /* One TASK in flight at a time: a second task in the same iteration (transition ticks)
     * waits for the previous one and hands its DP-done to the game first. */
    if (gdx3ds_rt_task_pending()) {
        gdx3ds_rt_wait_task(); /* waits + posts the previous task's DP-done */
    }
    sTasksWindow++;
    const uint32_t t = Produce(kCmdTask, dl, dlSize, ucode);
    sTaskSeq.store(t, std::memory_order_release);
    if (sMode == kSync) {
        WaitTaskDone();  /* waits + merges the segment claims */
        sTaskAcked = t;  /* the caller (osSpTaskStartGo) posts SP + DP itself, as before */
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

/* Main only: block until the in-flight TASK completed and persist its segment claims once.
 * Does NOT post DP-done: that is the join's job (gdx3ds_rt_wait_task) so a fence from a game
 * fiber cannot double-post or strand the game's DP wait. */
static void WaitTaskDone(void) {
    const uint32_t t = sTaskSeq.load(std::memory_order_acquire);
    if (t != 0) {
        WaitDone(t);
        if (sTaskMerged != t) {
            sTaskMerged = t;
            sSegMergeWindow += (uint32_t)gdx_gfx_segment_claims_merge(sTaskSegsOut);
        }
    }
}

extern "C" void gdx3ds_rt_wait_task(void) {
    if (sMode == kOff || tOnRenderThread) {
        return;
    }
    WaitTaskDone();
    const uint32_t t = sTaskSeq.load(std::memory_order_acquire);
    if (t != 0 && sTaskAcked != t) {
        sTaskAcked = t; /* the game's DP-done for this task: exactly once */
        gdx3ds_rt_post_dp_done();
    }
}

extern "C" void gdx3ds_rt_frame_end(void) {
    if (sMode == kOff) {
        return;
    }
    const uint32_t t = Produce(kCmdEnd, nullptr, 0, 0);
    if (sMode == kSync) {
        WaitDone(t);
    }
}

extern "C" void gdx3ds_rt_fence(void) {
    if (sMode == kOff || tOnRenderThread) {
        return;
    }
    const uint32_t t = sTaskSeq.load(std::memory_order_acquire);
    if (t != 0 && (int32_t)(sTaskDoneSeq.load(std::memory_order_acquire) - t) < 0) {
        sFencesWindow++;
        WaitTaskDone(); /* DP-done is still posted by the host's join */
    }
}

extern "C" void gdx3ds_rt_emit_receipt(unsigned long frame) {
    if (sMode == kOff) {
        return;
    }
    const float n = sFramesWindow != 0 ? (float)sFramesWindow : 1.0f;
    RtLogf("[rt] mode=%s frame=%lu n=%u tasks=%u waitMain=%.2f waitRender=%.2f fence=%u "
           "segMerge=%u pace=%u/%u latency=1",
           sMode == kSync ? "sync" : "pipe", frame, (unsigned)sFramesWindow, (unsigned)sTasksWindow,
           (float)((double)sWaitMainTicks / CPU_TICKS_PER_MSEC) / n,
           (float)((double)sWaitRenderTicks / CPU_TICKS_PER_MSEC) / n, (unsigned)sFencesWindow,
           (unsigned)sSegMergeWindow, (unsigned)sPaceWaits, (unsigned)sPaceSkips);
    sSegMergeWindow = 0;
    sWaitMainTicks = 0;
    sWaitRenderTicks = 0;
    sTasksWindow = 0;
    sFencesWindow = 0;
    sFramesWindow = 0;
    sPaceWaits = 0;
    sPaceSkips = 0;
}

extern "C" void gdx3ds_rt_shutdown(void) {
    if (sMode == kOff || sThread == nullptr) {
        return;
    }
    gdx3ds_rt_join_idle();
    const uint32_t t = Produce(kCmdQuit, nullptr, 0, 0);
    WaitDone(t);
    threadJoin(sThread, U64_MAX);
    threadFree(sThread);
    sThread = nullptr;
    sMode = kOff;
}
