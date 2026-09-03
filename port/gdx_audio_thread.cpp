// Dedicated audio production thread.
//
// Audio production used to run as a cooperative "Audio" fiber (decomp/src/sys/sys_audio.c's
// Audio_ThreadEntry) woken once per VI tick by port/n64_sched.c's single-OS-thread scheduler, with
// PCM synthesis on yet a third fiber. All of them share ONE real OS thread with no preemption, so
// a long synchronous, non-yielding game-thread stretch (Segment_LoadAssets during a course
// transition, measured up to ~131 ms) stopped audio production dead for its whole duration -- 725
// holes of ~10ms silence in a 113s capture, far past what os.cpp's 2048-frame osAiGetLength
// cushion could absorb. A real, independent OS thread cannot be wedged that way; this mirrors
// Shipwright's OTRAudio_Thread.
//
// CROSS-THREAD TOUCHPOINTS. The only game-thread code that mutates gAudioCtx's command-queue state
// goes through decomp/src/audio/disk/lib/thread.c's public API: AudioThread_QueueCmd and its typed
// variants (thread.c:440-521), AudioThread_ScheduleProcessCmds (thread.c:523-548, called from ~20
// sites in external.c -- the real game-thread entry point, since QueueCmd alone never crosses
// threads), and AudioThread_ResetAudioHeap / AudioThread_PreNMIInternal. The consumer side is
// AudioThread_CreateTask -> CreateTaskImpl (thread.c:18-269), which this file calls once per tick.
// Everything else CreateTaskImpl reaches -- AudioLoad_DecreaseSampleDmaTtls / ProcessLoads /
// ProcessScriptLoads, osAiSetNextBuffer, AudioSynth_Update, gdx_audio_hle_run -- touches only
// gAudioCtx-owned buffers and gAudioCtx's own DMA-completion queues. None of them touch the game's
// segment/asset arena (Arena_Allocate, Segment_LoadAssets), which the game thread owns
// exclusively, so ProcessLoads is safe to keep running on ticks driven from this thread.
//
// FIBER AFFINITY -- a crash risk, not a data race. CreateTaskImpl (thread.c:117) and several
// AudioLoad_Sync* functions reachable through AudioThread_ProcessGlobalCmd call
// osRecvMesg(..., OS_MESG_BLOCK). A blocking wait on an empty queue reaches __osEnqueueAndYield ->
// __osDispatchThread -> SwitchToFiber, and Win32 fibers are only valid on the OS thread that
// called ConvertThreadToFiber (the host thread). In this port those waits are expected never to
// block -- every DMA handler completes synchronously and posts its completion inline -- but that
// is an assumption about glue this file does not own. port/n64_sched.c's __osEnqueueAndYield
// therefore checks the calling OS thread against the host thread ID and spin-yields instead of
// touching the fiber scheduler, turning a would-be crash into a bounded, diagnosable stall.
//
// MUTEX BOUNDARY. Both sides of the threadCmdBuf ring sit inside sAudioCtxMutex: this file holds
// it across its ENTIRE per-tick production call (gdx_audio_produce_one_tick, which itself runs
// AudioThread_CreateTask + gdx_audio_hle_run), and thread.c's AudioThread_QueueCmd takes
// gdx_audio_ctx_lock/unlock around the ring write under #ifdef PORT. That one function is the
// chokepoint every QueueCmd* variant funnels through, so locking there covers every producer write
// without touching each variant. The mutex is RECURSIVE because a command HANDLER inside the
// mutexed drain may re-enter QueueCmd (ScheduleProcessCmds re-schedules when a STOP left the ring
// "finished"), which a plain mutex would self-deadlock on. ScheduleProcessCmds itself needs no
// lock: under PORT it pushes onto the lock-free CmdRing below, whose release-store/acquire-load
// pair publishes the producer's preceding threadCmdBuf writes to the consumer.
//
// KILL SWITCH. GDX_AUDIO_THREAD (default ON unless set to "0"), overridden by --audio-thread /
// --no-audio-thread, CLI last. OFF reverts completely to the legacy fiber path -- Audio_ThreadEntry
// producing every VI tick, and os.cpp's original 2048-frame osAiGetLength cushion -- i.e. the
// pre-thread behavior byte for byte, for a clean A/B.

#include "gdx_audio_thread.h"
#include "gdx_perf.h"
#include "port_log.h"
#ifdef GDX_PLATFORM_3DS
// R1 (port/3ds/audio/STATUS.md): the carved 3DS build has no LUS AudioPlayer/audiobridge;
// the ndsp backend's buffered() query stands in for AudioPlayerBuffered below.
#include "gdx3ds_audio.h"
// S4: the HLE audio PRODUCER runs on a spare ARM11 core, not core 0. std::thread on libctru's
// pthread shim would place it on the application core (core 0) in contention with the game; these
// hooks (port/3ds/audio/gdx3ds_audio_ndsp.c) host AudioThreadMain on a libctru thread walking the
// spare-core ladder (core 2 New3DS -> core 1 syscore -> core 0 fallback). Declared here rather
// than in the FROZEN gdx3ds_audio.h contract, matching that backend's diagnostic-export idiom.
extern "C" int gdx3ds_audio_producer_thread_start(void (*entry)(void*), void* arg);
extern "C" void gdx3ds_audio_producer_thread_join(void);
extern "C" int gdx3ds_audio_producer_core(void);
// HOME/sleep gate: blocks while the APT suspend hook holds the audio backend parked (see
// gdx3ds_audio_suspend in gdx3ds_audio.h); returns immediately otherwise or on join.
extern "C" int gdx3ds_audio_producer_park_if_suspended(void); // 1 = released for exit
// [audioprime2] (LOCKED-60 round 2, Task G): the boot-only producer target and the tick
// bracket the under-edge receipt reads. With [debug] audioprime2=0 the target is the constant
// 2048 below and the bracket only feeds receipts -- pacing byte-identical to round 1.
extern "C" int32_t gdx3ds_audio_producer_target_frames(int raceActive);
extern "C" void gdx3ds_audio_producer_tick_begin(void);
extern "C" void gdx3ds_audio_producer_tick_end(void);
extern "C" int gGdxRaceActive; // set by the first course load (port/decomp_port.c)
#else
#include "libultraship/bridge/audiobridge.h"
#endif

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

// Defined in port/n64_sched.c, which already includes the decomp audio headers this needs. This
// TU deliberately stays decomp-header-free, like port/n64_audio_hle.c.
extern "C" int gdx_audio_produce_one_tick(void);

namespace {

// Guards decomp's gAudioCtx thread-cmd queue handoff; see the MUTEX BOUNDARY comment above.
// Recursive because a command handler inside the mutexed drain may re-queue a command.
std::recursive_mutex sAudioCtxMutex;

std::mutex sWakeMutex;
std::condition_variable sWakeCv;
#ifndef GDX_PLATFORM_3DS
std::thread sAudioThread;
#endif
std::atomic<bool> sStopRequested{ false };
std::atomic<bool> sThreadActive{ false }; // resolved kill-switch value, cached for this run

// AudioThread_CreateTaskImpl can legitimately return without producing a task (its own
// totalTaskCount % specUnk4 gating), which means "not yet", not "stop" -- so the catch-up loop
// must not break early on it. This cap only bounds worst-case per-wake CPU work if something
// upstream keeps reporting Buffered() below DesiredBuffered indefinitely, e.g. before the SDL
// device is warmed up.
constexpr int kMaxTicksPerWake = 64;

// Lock-free ring replacing gAudioCtx.threadCmdProcQueue, the ONE message queue that genuinely
// crosses the game/host OS thread and this one. Its (readPos<<8 | writePos) token tells the
// consumer which slice of threadCmdBuf to apply; it has two producers (the game thread via
// AudioThread_ScheduleProcessCmds, and this thread when CreateTaskImpl re-schedules after a STOP)
// and one consumer (CreateTaskImpl's per-tick drain).
//
// Routing that token through decomp's libultra osSendMesg/osRecvMesg meant BOTH OS threads
// executed the kernel message-queue path concurrently. n64_sched.c's gdx_mq_lock serializes the
// queue DATA, but the host thread still mutates the run queue / waiter lists / fibers OUTSIDE that
// lock (only the deferred-wake drain takes it), so the audio thread still collided with it --
// glibc-detected heap/list corruption (SIGABRT) during 64DD boot. This ring touches ZERO libultra
// scheduler state, so that path is simply never reached from here.
//
// Bounded MPMC (Vyukov) rather than strict SPSC, because of the two producers above. Each cell's
// release-store / acquire-load also publishes the producer's preceding threadCmdBuf writes, so the
// command payload is visible after a successful pop. Initialized once at static-construction time
// and never reset at runtime, so a concurrent AudioThread_InitMesgQueues (audio-heap reset) cannot
// race it; stale tokens are harmless because ProcessCmds NOOPs each AudioCmd slot as it applies it.
class CmdRing {
public:
    CmdRing() {
        for (size_t i = 0; i < kSize; i++) {
            cells_[i].seq.store(i, std::memory_order_relaxed);
        }
        enqueuePos_.store(0, std::memory_order_relaxed);
        dequeuePos_.store(0, std::memory_order_relaxed);
    }

    // Returns true on success, false if the ring is full (mirrors osSendMesg OS_MESG_NOBLOCK).
    bool Enqueue(uint32_t value) {
        Cell* cell;
        size_t pos = enqueuePos_.load(std::memory_order_relaxed);
        for (;;) {
            cell = &cells_[pos & kMask];
            size_t seq = cell->seq.load(std::memory_order_acquire);
            intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);
            if (diff == 0) {
                if (enqueuePos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    break;
                }
            } else if (diff < 0) {
                return false; // full
            } else {
                pos = enqueuePos_.load(std::memory_order_relaxed);
            }
        }
        cell->data = value;
        cell->seq.store(pos + 1, std::memory_order_release);
        return true;
    }

    // Returns true and writes *out on success, false if empty (mirrors osRecvMesg OS_MESG_NOBLOCK).
    bool Dequeue(uint32_t* out) {
        Cell* cell;
        size_t pos = dequeuePos_.load(std::memory_order_relaxed);
        for (;;) {
            cell = &cells_[pos & kMask];
            size_t seq = cell->seq.load(std::memory_order_acquire);
            intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);
            if (diff == 0) {
                if (dequeuePos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    break;
                }
            } else if (diff < 0) {
                return false; // empty
            } else {
                pos = dequeuePos_.load(std::memory_order_relaxed);
            }
        }
        if (out != nullptr) {
            *out = cell->data;
        }
        cell->seq.store(pos + kMask + 1, std::memory_order_release);
        return true;
    }

private:
    static constexpr size_t kSize = 256; // power of two; >> the 4-slot decomp threadCmdProcMsgBuf
    static constexpr size_t kMask = kSize - 1;
    struct Cell {
        std::atomic<size_t> seq;
        uint32_t data;
    };
    Cell cells_[kSize];
    alignas(64) std::atomic<size_t> enqueuePos_;
    alignas(64) std::atomic<size_t> dequeuePos_;
};

CmdRing sCmdRing;

// AudioThread_CreateTaskImpl posted gAudioCtx.totalTaskCount to gAudioCtx.taskStartQueue via
// osSendMesg on EVERY tick -- the last decomp osSendMesg this thread still executed, and the one a
// crash core caught running concurrently with the boot fiber's SLMFSLoad->LeoSpdlMotor osSendMesg
// during a 64DD mount. taskStartQueue has NO consumer in this port (AudioThread_WaitForAudioTask,
// its only reader, is never called), so the send never needed the kernel at all: it is now a plain
// atomic counter. If a host-side consumer ever needs the count or token, read these rather than
// reintroducing a cross-thread message queue.
//
// The other audio-thread-reachable os-queue calls stay on the existing path deliberately, and none
// of them enters the libultra run-queue/waiter/fiber machinery from this thread.
// curAudioFrameDmaQueue is audio-thread-local (its producer is the inline, synchronous host-DMA
// completion on this same thread, so CreateTaskImpl's NOBLOCK drain always empties it first).
// audioResetQueue is written only when gAudioCtx.resetStatus != 0, i.e. never during the 64DD boot
// window, and cannot be ring-converted the way taskStartQueue was: its consumer does an
// OS_MESG_BLOCK recv that, under the legacy-fiber kill-switch path, relies on the cooperative
// __osEnqueueAndYield fiber switch, which a lock-free ring spin would deadlock.
std::atomic<uint32_t> sTaskStartCount{ 0 };
std::atomic<uint32_t> sTaskStartLastToken{ 0 };

bool ResolveKillSwitch(int argc, char** argv) {
    bool enabled = true;
    if (const char* env = std::getenv("GDX_AUDIO_THREAD")) {
        enabled = (env[0] != '0');
    }
    for (int i = 1; i < argc; i++) {
        if (argv[i] == nullptr) {
            continue;
        }
        if (std::strcmp(argv[i], "--no-audio-thread") == 0) {
            enabled = false;
        } else if (std::strcmp(argv[i], "--audio-thread") == 0) {
            enabled = true;
        }
    }
    return enabled;
}

#ifdef GDX_PLATFORM_3DS
// R2 (port/3ds/audio/STATUS.md): no AudioPlayerGetDesiredBuffered on 3DS. 2048 frames
// matches the desktop os.cpp osAiGetLength cushion and leaves half the ndsp backend's
// 4096-frame default ring as headroom. [audioprime2] raises it to 4096 during boot only
// (gdx3ds_audio_producer_target_frames), back to this value before any race starts.
constexpr int32_t kGdx3dsDesiredBufferedFrames = 2048;
#endif

void AudioThreadMain() {
    using namespace std::chrono_literals;
#ifdef GDX_PLATFORM_3DS
    gdx_port_logf("[audio-thread] dedicated audio thread started (DesiredBuffered=%d)\n",
                  (int)kGdx3dsDesiredBufferedFrames);
#else
    gdx_port_logf("[audio-thread] dedicated audio thread started (DesiredBuffered=%d)\n",
                  AudioPlayerGetDesiredBuffered());
#endif

    while (!sStopRequested.load(std::memory_order_relaxed)) {
        {
            // 5ms self-pump: this thread never depends on gdx_audio_thread_notify_frame()
            // arriving in a timely fashion, only on it eventually arriving OR this timeout
            // firing, so a stalled main thread cannot stop production.
            std::unique_lock<std::mutex> wakeLock(sWakeMutex);
            sWakeCv.wait_for(wakeLock, 5ms);
        }
        if (sStopRequested.load(std::memory_order_relaxed)) {
            break;
        }

#ifdef GDX_PLATFORM_3DS
        // HOME menu / lid sleep: park here (no buffered() polling, no production) until the
        // ONRESTORE/ONWAKEUP hook resumes the backend. A nonzero return is the join's release
        // order while STILL suspended (close from the HOME menu, no ONRESTORE): leave at
        // once -- the DSP is asleep, so not even gdx3ds_audio_buffered() may run below.
        if (gdx3ds_audio_producer_park_if_suspended() != 0 ||
            sStopRequested.load(std::memory_order_relaxed)) {
            break;
        }
        const int32_t desired = gdx3ds_audio_producer_target_frames(gGdxRaceActive); // R2 + audioprime2
        int iterations = 0;
        while ((int32_t)gdx3ds_audio_buffered() < desired && iterations < kMaxTicksPerWake) { // R3
            gdx3ds_audio_producer_tick_begin();
#else
        const int32_t desired = AudioPlayerGetDesiredBuffered();
        int iterations = 0;
        while (AudioPlayerBuffered() < desired && iterations < kMaxTicksPerWake) {
#endif
            const auto tickStart = std::chrono::steady_clock::now();
            {
                std::lock_guard<std::recursive_mutex> ctxLock(sAudioCtxMutex);
                gdx_audio_produce_one_tick();
            }
#ifdef GDX_PLATFORM_3DS
            gdx3ds_audio_producer_tick_end();
#endif
            if (gdx::PerfEnabled()) {
                gdx::PerfAudioTick(std::chrono::duration<double, std::milli>(
                                       std::chrono::steady_clock::now() - tickStart)
                                       .count());
            }
            iterations++;
        }
    }

    gdx_port_logf("[audio-thread] dedicated audio thread stopping\n");
}

#ifdef GDX_PLATFORM_3DS
// libctru ThreadFunc entry (void(*)(void*)) wrapping the no-arg loop, so the spare-core producer
// hook can host it.
void AudioThreadTrampoline(void* /*arg*/) { AudioThreadMain(); }
#endif

} // namespace

extern "C" void gdx_audio_thread_start(int argc, char** argv) {
    const bool enabled = ResolveKillSwitch(argc, argv);
    sThreadActive.store(enabled, std::memory_order_relaxed);
    gdx_port_logf(
        "[audio-thread] GDX_AUDIO_THREAD -> dedicated thread %s (legacy fiber path %s)\n",
        enabled ? "ACTIVE" : "inactive",
        enabled ? "disabled (see sys_audio.c's gdx_audio_thread_active() gate)"
                : "ACTIVE (legacy fiber behavior, 2048-frame osAiGetLength cushion intact)");

    if (!enabled) {
        return; // Kill switch OFF: nothing else in this file runs this session.
    }

    sStopRequested.store(false, std::memory_order_relaxed);
#ifdef GDX_PLATFORM_3DS
    // S4: place the HLE producer on a spare ARM11 core (core 2 on New3DS) instead of core 0.
    // The core-0 rung always succeeds, so a nonzero return means only that thread creation itself
    // failed -- fall back to running production inline is not possible here, so log and continue
    // with sThreadActive still set; the 5ms self-pump loop simply never runs, matching a failed
    // std::thread on desktop (which would have thrown). In practice this never fails.
    if (gdx3ds_audio_producer_thread_start(&AudioThreadTrampoline, nullptr) != 0) {
        gdx_port_logf("[audio-thread] WARNING: spare-core producer thread failed to start\n");
    } else {
        gdx_port_logf("[audio-thread] HLE producer pinned to core %d\n",
                      gdx3ds_audio_producer_core());
    }
#else
    sAudioThread = std::thread(AudioThreadMain);

    // libultraship's crash handler installs a SIGTERM/SIGINT/SIGQUIT ShutdownHandler that calls
    // exit() DIRECTLY, so a window close or `kill` bypasses main()'s gdx_audio_thread_stop().
    // exit() then runs static destructors and reaches sAudioThread while it is still joinable,
    // and ~std::thread() on a joinable thread calls std::terminate(). [basic.start.term] fixes it:
    // sAudioThread's construction is sequenced before this registration, so the handler runs
    // BEFORE ~sAudioThread. Idempotent with main()'s explicit stop, which then no-ops.
    static bool sAtexitRegistered = false;
    if (!sAtexitRegistered) {
        sAtexitRegistered = true;
        std::atexit([] { gdx_audio_thread_stop(); });
    }
#endif
}

extern "C" void gdx_audio_thread_stop(void) {
    if (!sThreadActive.load(std::memory_order_relaxed)) {
        return;
    }
    sStopRequested.store(true, std::memory_order_relaxed);
    sWakeCv.notify_all();
#ifdef GDX_PLATFORM_3DS
    gdx3ds_audio_producer_thread_join();
#else
    if (sAudioThread.joinable()) {
        sAudioThread.join();
    }
#endif
}

extern "C" void gdx_audio_thread_notify_frame(void) {
    if (!sThreadActive.load(std::memory_order_relaxed)) {
        return;
    }
    // No predicate flag: a notify lost to a race only means the next production waits for the 5ms
    // self-pump instead of happening immediately.
    sWakeCv.notify_one();
}

extern "C" int gdx_audio_thread_active(void) {
    return sThreadActive.load(std::memory_order_relaxed) ? 1 : 0;
}

extern "C" void gdx_audio_ctx_lock(void) {
    sAudioCtxMutex.lock();
}

extern "C" void gdx_audio_ctx_unlock(void) {
    sAudioCtxMutex.unlock();
}

// Ring bridge for decomp/src/audio/disk/lib/thread.c: push replaces ScheduleProcessCmds'
// osSendMesg, pop replaces CreateTaskImpl's drain-loop osRecvMesg. Both keep the OS_MESG_NOBLOCK
// return convention (0 = ok, -1 = full/empty) so thread.c's control flow is untouched.
extern "C" int gdx_audio_cmdring_push(unsigned int token) {
    return sCmdRing.Enqueue(static_cast<uint32_t>(token)) ? 0 : -1;
}

extern "C" int gdx_audio_cmdring_pop(unsigned int* out) {
    uint32_t value = 0;
    if (!sCmdRing.Dequeue(&value)) {
        return -1;
    }
    if (out != nullptr) {
        *out = value;
    }
    return 0;
}

// Replaces CreateTaskImpl's per-tick osSendMesg to taskStartQueue (see the comment above).
// Relaxed ordering is sufficient: no other data is published through this counter.
extern "C" void gdx_audio_taskstart_post(unsigned int token) {
    sTaskStartLastToken.store(static_cast<uint32_t>(token), std::memory_order_relaxed);
    sTaskStartCount.fetch_add(1, std::memory_order_relaxed);
}

// Diagnostic accessors, not currently consumed; provided so a future host-side reader never needs
// to reintroduce a cross-thread message queue.
extern "C" unsigned int gdx_audio_taskstart_count(void) {
    return sTaskStartCount.load(std::memory_order_relaxed);
}

extern "C" unsigned int gdx_audio_taskstart_last_token(void) {
    return sTaskStartLastToken.load(std::memory_order_relaxed);
}
