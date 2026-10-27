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
// [debug] ndsphash_lockstep: the audio-determinism pin, see port/gdx_ndsp_hash.h.
#include "gdx_ndsp_hash.h"
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
// [audioring]: once per producer loop iteration, before the catch-up loop. Records the wake,
// the buffered level it is about to pace against, and whether the PREVIOUS iteration left on
// the kMaxTicksPerWake cap -- the difference between "the producer had nothing to do" and
// "the producer could not catch up". A no-op (one int compare) unless [debug] audioring is on.
extern "C" void gdx3ds_audio_producer_wake(int32_t target, int32_t maxTicksPerWake);
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
// C4I: the same tick in its two phases (port/n64_sched.c) -- SETUP needs sAudioCtxMutex, SYNTH
// (the HLE run over the list SETUP built) does not touch anything the mutex guards.
extern "C" void* gdx_audio_produce_tick_setup(void);
extern "C" void gdx_audio_produce_tick_synth(void* task);
#ifdef GDX_PLATFORM_3DS
extern "C" long long gdx3ds_prof_now(void); // port/3ds/gfx/gdx3ds_gpu_prof.c (svcGetSystemTick)
extern "C" int gdx3ds_config_get_int(const char* section, const char* key, int fallback);
// C4K [debug] audio_simd: the bit-exact fast HLE kernels (port/n64_audio_hle_fast.h).
extern "C" void gdx_audio_hle_set_fast(int on);
extern "C" void gdx_audio_hle_set_verify(int everyN);
extern "C" void gdx_audio_hle_set_rs_align(int on);
extern "C" void gdx_audio_hle_set_acmd_dump(int ticks); // AUDIOFIX round 3 Task 2: ground-truth capture
extern "C" void gdx_audio_hle_set_envmix_xbits(int on); // AUDIOFIX round 4: A_ENVMIXER x0-x3 xor fix
extern "C" void gdx_audio_hle_set_adpcm_scale_clamp(int on); // AUDIOFIX round 4: A_ADPCM scale>12 clamp
// O3DSHOMEFIX: model-gated defaults (1 on Old 3DS, 0 on New 3DS; port/3ds/audio).
extern "C" int gdx3ds_is_old3ds(void);
#endif

namespace {

// Guards decomp's gAudioCtx thread-cmd queue handoff; see the MUTEX BOUNDARY comment above.
// Recursive because a command handler inside the mutexed drain may re-queue a command.
std::recursive_mutex sAudioCtxMutex;

#ifdef GDX_PLATFORM_3DS
// C4I [othx] actx ledger (port/3ds/gfx/gdx3ds_gpu_prof.c): what the game fiber pays to take
// sAudioCtxMutex in AudioThread_QueueCmd while the producer holds it across a whole tick, and
// how long those producer ticks hold it. On the Old 3DS the producer runs on the syscore
// (core 1), so a tick there is slow and the game thread -- the frame -- waits it out. Armed by
// the gputrace latch; unarmed, each path pays one int test. Static counters only.
volatile int sCtxLedgerArmed = 0;
uint64_t sCtxWaitTicks = 0;      // QueueCmd side: blocked in lock() (contended locks only)
unsigned long sCtxLocks = 0;     // QueueCmd side: every lock
unsigned long sCtxContended = 0; // QueueCmd side: locks that found it held
uint64_t sCtxHoldTicks = 0;      // producer side: ticks sAudioCtxMutex was held per tick
unsigned long sCtxHolds = 0;
uint64_t sCtxSynthTicks = 0;     // producer side: the SYNTH phase (inside or outside the hold)
// `[debug] audio_ctx_narrow` (default 0), resolved once when the producer starts: 1 releases
// sAudioCtxMutex between SETUP and SYNTH, so the game fiber's QueueCmd waits out only the
// command drain + loads, never the HLE synthesis. 0 = the pre-C4I lock span, byte for byte.
int sCtxNarrow = 0;

// QUICKFIX `[debug] audio_io_unlock` (hwprof-progress.md §10.3): a sample read that may touch the
// SD card (an audio-stream block-cache miss or read-ahead, port/gdx_o2r_stream.cpp) drops
// sAudioCtxMutex for the card I/O and re-takes it after. On hardware the miss read queued behind
// the async log writer's SD write for 76-351 ms while the producer held the mutex, and the game
// thread's Audio_Update -> AudioThread_QueueCmd waited it out (the `au` hitches).
//
// WHY THIS IS SAFE. The mutex guards one thing: the threadCmdBuf ring write in QueueCmd (game
// thread, and the producer's own re-queue from a drain handler). A sample read is never inside
// that write, so while it runs unlocked the producer touches no ring state; a game QueueCmd in
// the window writes at threadCmdWritePos, beyond every token already published, exactly as it
// does between two ticks. If the game also pushes a token, a drain still in progress may apply
// it this tick -- the interleaving the game gets by queueing just before the tick started.
// The bytes read are the same: the same cache, the same reads in the same order, same thread.
// Only the OWNING producer tick, holding the mutex exactly once (tTickHold set, no nested
// QueueCmd lock), ever releases; any other caller gets 0.
int sIoUnlock = 0;
__thread int tTickHold = 0;   // this thread holds sAudioCtxMutex for a producer tick, once
__thread int tTickNested = 0; // recursive gdx_audio_ctx_lock depth on top of the tick's hold
unsigned long sIoReleases = 0;
uint64_t sIoUnlockedTicks = 0;
uint64_t sIoUnlockedMax = 0;
long long sIoReleaseT0 = 0;
unsigned long sIoNextReceipt = 1;

// The tick's hold, as a scope inside the lock_guard (constructed after it, destroyed before).
struct TickHoldScope {
    TickHoldScope() {
        tTickHold = 1;
        tTickNested = 0;
    }
    ~TickHoldScope() { tTickHold = 0; }
};

void AudioCtxLockCounted() {
    sCtxLocks++;
    if (sAudioCtxMutex.try_lock()) {
        return;
    }
    const long long t0 = gdx3ds_prof_now();
    sAudioCtxMutex.lock();
    sCtxWaitTicks += (uint64_t)(gdx3ds_prof_now() - t0);
    sCtxContended++;
}

void AudioCtxLockPlain() {
    if (sCtxLedgerArmed) {
        AudioCtxLockCounted();
    } else {
        sAudioCtxMutex.lock();
    }
}

// Latched with audio_ctx_narrow, on the producer thread before its first tick (or, under the
// ndsphash lockstep pin, on the game thread's first inline tick).
// QUICKFIX: default 0 until the hardware card confirms it; the emulator corpus is bit-exact.
void ResolveIoUnlock() {
    static std::atomic<int> sResolved{ 0 };
    if (sResolved.exchange(1) != 0) {
        return;
    }
    sIoUnlock = gdx3ds_config_get_int("debug", "audio_io_unlock", 0) != 0 ? 1 : 0;
    gdx_port_logf("[actx] audio_io_unlock=%d: audio-stream SD reads %s\n", sIoUnlock,
                  sIoUnlock ? "drop sAudioCtxMutex (the game thread never waits on card I/O)"
                            : "run under sAudioCtxMutex (pre-quickfix)");
}

// C4K `[debug] audio_simd` (default 0): route the HLE ops through the fast kernels where each
// proves it computes the reference's bytes (n64_audio_hle_fast.h). Resolved on the host thread
// BEFORE the producer starts, so the first tick -- on either thread, lockstep included -- already
// runs the chosen path. One receipt either way.
// `[debug] audio_simd_verify=N` (default 0): with audio_simd=1, render one tick in N twice --
// reference on saved inputs, then fast -- and count byte mismatches on the real silicon
// (n64_audio_hle.c). A verification run, not a performance one: that tick's synth costs double.
// Default ON for the Old 3DS only (hardware 2026-09-29: `[audio-simd] verify=391/0/0`, 0
// mismatches, 15.9% of the synth); the New 3DS default stays 0. The ini key wins either way.
void ResolveHleFast() {
    const int modelDefault = gdx3ds_is_old3ds();
    const int on = gdx3ds_config_get_int("debug", "audio_simd", modelDefault) != 0 ? 1 : 0;
    const int verify = gdx3ds_config_get_int("debug", "audio_simd_verify", 0);
    gdx_audio_hle_set_fast(on);
    gdx_audio_hle_set_verify(verify);
    gdx_port_logf("[audio-simd] audio_simd=%d verify=%d (model default %d): HLE kernels = %s\n", on, verify,
                  modelDefault, on ? "fast (bit-exact per-op proof, reference fallback)" : "reference");
    // o3dscrackle `[debug] audio_rs_align`: the A_RESAMPLE tap window, see n64_audio_hle.c's
    // sHleRsAlign. DEFAULT 1 since n3ds (a bug fix: the old window read past the input at every
    // update seam, ~180 clicks/s, on every model); 0 = the old taps. Receipt only when on.
    const int rsAlign = gdx3ds_config_get_int("debug", "audio_rs_align", 1) != 0 ? 1 : 0;
    gdx_audio_hle_set_rs_align(rsAlign);
    if (rsAlign) {
        gdx_port_logf("[audiocrk] audio_rs_align=1: A_RESAMPLE taps x[c-4..c-1] (the RSP's window, "
                      "no lookahead past the consumed input)\n");
    }
    // AUDIOFIX round 3 Task 2: [debug] acmd_dump_n, default 0 (inert) -- capture the first N
    // post-race Acmd lists plus every resolved external address they touch, for a future
    // independent-ucode (mupen64plus-rsp-hle alist_process_nead_fz) ground-truth diff. An
    // investigation tool, not a shipped lever: never recommended in a card ini.
    const int acmdDumpN = gdx3ds_config_get_int("debug", "acmd_dump_n", 0);
    gdx_audio_hle_set_acmd_dump(acmdDumpN);
    if (acmdDumpN > 0) {
        gdx_port_logf("[acmd-dump] armed: %d post-race ticks -> sdmc:/3ds/gdiffuser/acmd-*.bin\n",
                      acmdDumpN);
    }
    // AUDIOFIX round 4: [debug] envmix_xbits, default 0 (old behaviour) -- aEnvMixer's x0-x3 bits
    // (n64_audio_hle.c's GDX_A_ENVMIXER case), xor-mixed per mupen64plus-rsp-hle's
    // alist_process_nead_fz. Round 3's acmd_dump_n capture never saw real F-Zero X content set
    // these bits, so this ships inert until a hardware A/B says the xor path matters.
    const int envmixXbits = gdx3ds_config_get_int("debug", "envmix_xbits", 0) != 0 ? 1 : 0;
    gdx_audio_hle_set_envmix_xbits(envmixXbits);
    if (envmixXbits) {
        gdx_port_logf("[audiocrk] envmix_xbits=1: A_ENVMIXER applies the mupen64plus-rsp-hle x0-x3 "
                      "xor masks (reference path only; the fast kernel always declines)\n");
    }
    // AUDIOFIX round 4: [debug] adpcm_scale_clamp -- caps A_ADPCM's header-scale-derived
    // residual shift at 12 (n64_audio_hle.c's RunAdpcm), matching mupen64plus-rsp-hle's
    // `rshift = (scale < 12) ? 12 - scale : 0` clamp. Found against this ROM's own
    // race-mutecity-start capture (a scale-13 frame, tick 2): unclamped, this file's
    // `nibble << shift` overshoots by 1-3 bits past mupen's reference, exactly 2x on the
    // reported sample. n60-3d12 made it default ON on Old 3DS (card n60-3d11 "ALL",
    // hardware-confirmed). v1.0.1: default ON on New 3DS too -- the overshoot is a decode bug
    // in the ROM's own ADPCM data, not a CPU-budget trade, and v1.0 New 3DS players reported
    // exactly its symptom (crackle, worst on SFX transients). `adpcm_scale_clamp = 0` in the
    // ini still forces the old unclamped shift on either model.
    const int adpcmScaleClamp = gdx3ds_config_get_int("debug", "adpcm_scale_clamp", 1) != 0 ? 1 : 0;
    gdx_audio_hle_set_adpcm_scale_clamp(adpcmScaleClamp);
    if (adpcmScaleClamp) {
        gdx_port_logf("[audiocrk] adpcm_scale_clamp=1: A_ADPCM residual shift capped at 12 for "
                      "any header scale >= 12 (mupen64plus-rsp-hle's rshift clamp)\n");
    }
}

// Latched once, on the producer thread, before its first tick: the lock span never changes
// mid-run. One receipt either way, so a hardware log says which span it measured.
// Default ON for the Old 3DS only (hardware 2026-09-29: race wall -20 ms/frame); the New 3DS
// default stays 0. The ini key wins either way.
void ResolveCtxNarrow() {
    const int modelDefault = gdx3ds_is_old3ds();
    sCtxNarrow = gdx3ds_config_get_int("debug", "audio_ctx_narrow", modelDefault) != 0 ? 1 : 0;
    gdx_port_logf("[actx] audio_ctx_narrow=%d (model default %d): sAudioCtxMutex held across %s\n", sCtxNarrow,
                  modelDefault, sCtxNarrow ? "SETUP only (HLE synth runs unlocked)" : "the whole tick");
    ResolveIoUnlock();
}

void ProduceTickSynthTimed(void* task, bool armed) {
    const long long t0 = armed ? gdx3ds_prof_now() : 0;
    gdx_audio_produce_tick_synth(task);
    if (armed) {
        sCtxSynthTicks += (uint64_t)(gdx3ds_prof_now() - t0);
    }
}

void NoteCtxHold(long long t0, bool armed) {
    if (armed) {
        sCtxHoldTicks += (uint64_t)(gdx3ds_prof_now() - t0);
        sCtxHolds++;
    }
}

// One producer tick. Default: SETUP and SYNTH both under sAudioCtxMutex (the historical span).
// audio_ctx_narrow=1: SYNTH runs after the unlock.
void ProduceOneTickLocked() {
    const bool armed = sCtxLedgerArmed != 0;
    void* task = nullptr;
    {
        std::lock_guard<std::recursive_mutex> ctxLock(sAudioCtxMutex);
        TickHoldScope hold;
        const long long t0 = armed ? gdx3ds_prof_now() : 0;
        task = gdx_audio_produce_tick_setup();
        if (task != nullptr && !sCtxNarrow) {
            ProduceTickSynthTimed(task, armed);
            task = nullptr;
        }
        NoteCtxHold(t0, armed);
    }
    if (task != nullptr) {
        ProduceTickSynthTimed(task, armed);
    }
}
#else
void AudioCtxLockPlain() {
    sAudioCtxMutex.lock();
}

void ProduceOneTickLocked() {
    std::lock_guard<std::recursive_mutex> ctxLock(sAudioCtxMutex);
    gdx_audio_produce_one_tick();
}
#endif

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
    ResolveCtxNarrow();
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
        // Read the pin ONCE per wake, not once per catch-up iteration: this loop runs up to
        // kMaxTicksPerWake times and the shipped path must not pay a call per iteration for a
        // lever that is off. It cannot change mid-wake -- it is resolved at boot.
        const bool ndspHashLockstep = gdx_ndsphash_lockstep() != 0;
        int iterations = 0;
        // Deliberately does NOT pass gdx3ds_audio_buffered(): that takes the ring lock, and the
        // shipped path must not acquire it one extra time per 5 ms wake for a diagnostic. The
        // hook reads the two counters unlocked itself, and only when the census is armed.
        gdx3ds_audio_producer_wake(desired, kMaxTicksPerWake);
        // [debug] ndsphash_lockstep: this thread stands down entirely and production moves
        // to gdx_audio_thread_notify_frame(), inline on the game thread, one tick per frame.
        // Two producers would reintroduce exactly the interleaving the pin exists to remove.
        while (!ndspHashLockstep &&
               (int32_t)gdx3ds_audio_buffered() < desired && iterations < kMaxTicksPerWake) { // R3
            gdx3ds_audio_producer_tick_begin();
#else
        const int32_t desired = AudioPlayerGetDesiredBuffered();
        int iterations = 0;
        while (AudioPlayerBuffered() < desired && iterations < kMaxTicksPerWake) {
#endif
            const auto tickStart = std::chrono::steady_clock::now();
            ProduceOneTickLocked();
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
    ResolveHleFast();
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
#ifdef GDX_PLATFORM_3DS
    // [debug] ndsphash_lockstep (default 0, one int compare when off): produce the tick HERE,
    // on the game thread, once per frame, instead of signalling the free-running producer.
    // Audio tick N then belongs to game frame N by construction, and the game's frames are
    // already deterministic under [debug] det=1. Without this the producer paces itself
    // against the ring's occupancy, so the number of audio ticks between two game events
    // varies run to run, every note-on lands at a different sample position, and two identical
    // runs share only about a quarter of their audio command lists.
    //
    // Running it on THIS thread is also the safer of the two: gdx_audio_produce_one_tick can
    // reach a blocking osRecvMesg, and port/n64_sched.c's __osEnqueueAndYield spin-yields
    // rather than touching the fiber scheduler when the caller is not the host thread. Here it
    // IS the host thread, so the fiber path is the normal one.
    //
    // PARITY MODE, not a shipping option: production no longer tracks playback, so the ring
    // over- or under-runs and no timing, sil_ppk or level from such a run means anything.
    if (gdx_ndsphash_lockstep()) {
        ResolveIoUnlock();
        std::lock_guard<std::recursive_mutex> ctxLock(sAudioCtxMutex);
        TickHoldScope hold;
        gdx_audio_produce_one_tick();
        return;
    }
#endif
    // No predicate flag: a notify lost to a race only means the next production waits for the 5ms
    // self-pump instead of happening immediately.
    sWakeCv.notify_one();
}

extern "C" int gdx_audio_thread_active(void) {
    return sThreadActive.load(std::memory_order_relaxed) ? 1 : 0;
}

// B11 EK-DEADLOCK PROBE (port/n64_sched.c, docs/research/native/ekdeadlock-progress.md):
// sAudioCtxMutex is held across the producer's ENTIRE tick, and the game fiber takes it in
// AudioThread_QueueCmd. When the EK build wedges, the watchdog has to say which side owns it.
// The counters are plain volatiles read racily by the watchdog; nothing reads them in the
// shipped build, where the probe fields are omitted from the line entirely.
extern "C" {
extern volatile uint32_t gdx_watch_ctx_owner;
extern volatile uint32_t gdx_watch_ctx_waiters;
uint32_t gdx_probe_actor(void);
}

#ifdef GDX_PLATFORM_3DS
extern "C" void gdx_audio_ctx_ledger_arm(int on) {
    sCtxLedgerArmed = on ? 1 : 0;
}

extern "C" void gdx_audio_ctx_ledger_totals(uint64_t* waitTicks, unsigned long* locks,
                                            unsigned long* contended, uint64_t* holdTicks,
                                            unsigned long* holds, uint64_t* synthTicks) {
    *waitTicks = sCtxWaitTicks;
    *locks = sCtxLocks;
    *contended = sCtxContended;
    *holdTicks = sCtxHoldTicks;
    *holds = sCtxHolds;
    *synthTicks = sCtxSynthTicks;
}
#endif

extern "C" void gdx_audio_ctx_lock(void) {
#ifdef EXPANSION_KIT
    gdx_watch_ctx_waiters++;
    AudioCtxLockPlain();
    gdx_watch_ctx_waiters--;
    gdx_watch_ctx_owner = gdx_probe_actor();
#else
    AudioCtxLockPlain();
#endif
#ifdef GDX_PLATFORM_3DS
    if (tTickHold) {
        tTickNested++;
    }
#endif
}

extern "C" void gdx_audio_ctx_unlock(void) {
#ifdef GDX_PLATFORM_3DS
    if (tTickHold && tTickNested > 0) {
        tTickNested--;
    }
#endif
#ifdef EXPANSION_KIT
    gdx_watch_ctx_owner = 0u;
#endif
    sAudioCtxMutex.unlock();
}

#ifdef GDX_PLATFORM_3DS
extern "C" int gdx_audio_ctx_io_unlock_enabled(void) {
    return sIoUnlock;
}

// QUICKFIX [debug] audio_io_unlock: see sIoUnlock. Returns 1 when the mutex was dropped; the
// caller then MUST call gdx_audio_ctx_io_reacquire() on the same thread.
extern "C" int gdx_audio_ctx_io_release(void) {
    if (!sIoUnlock || !tTickHold || tTickNested != 0) {
        return 0;
    }
    tTickHold = 0;
    sIoReleaseT0 = gdx3ds_prof_now();
    sAudioCtxMutex.unlock();
    return 1;
}

// Receipt at 1, 2, 4, 8, ... releases: bounded log volume, emitted while still unlocked so its
// log write never runs under the mutex either.
static void IoUnlockReceipt() {
    if (sIoReleases < sIoNextReceipt) {
        return;
    }
    sIoNextReceipt *= 2u;
    const double perMs = 268111.856; // svcGetSystemTick ticks per ms (SYSCLOCK_ARM11 / 1000)
    gdx_port_logf("[actx] io_unlock releases=%lu unlocked=%.1f ms max=%.1f ms\n", sIoReleases,
                  (double)sIoUnlockedTicks / perMs, (double)sIoUnlockedMax / perMs);
}

extern "C" void gdx_audio_ctx_io_reacquire(void) {
    const uint64_t dt = (uint64_t)(gdx3ds_prof_now() - sIoReleaseT0);
    sIoReleases++;
    sIoUnlockedTicks += dt;
    if (dt > sIoUnlockedMax) {
        sIoUnlockedMax = dt;
    }
    IoUnlockReceipt();
    AudioCtxLockPlain();
    tTickHold = 1;
    tTickNested = 0;
}
#endif

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
