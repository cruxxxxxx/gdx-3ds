/* ndsp backend for gdx3ds_audio.h (stream C). 3DS-only translation unit; the host build
 * keeps gdx3ds_audio_stub.c (see the if(NINTENDO_3DS) switch in this dir's CMakeLists.txt).
 *
 * PIPELINE
 *   game audio production --gdx3ds_audio_push()--> PCM ring (heap, LightLock-guarded)
 *   drain thread (spare core) --fixed 512-frame chunks--> ndspWaveBuf triple-buffer
 *   (linearAlloc, DSP-visible) --> ndsp channel 0, 32000 Hz stereo PCM16.
 *
 * RING. One contiguous interleaved-s16 ring of `capacityFrames` frames (default 4096 when
 * gdx3ds_audio_init(0)). A single LightLock guards readPos/countFrames/inFlightFrames;
 * "lock-free" was rejected because the contract's drop-oldest overrun policy makes the
 * PRODUCER advance the read cursor, which breaks the SPSC single-writer-per-index invariant.
 * Every critical section is a bounded memcpy (<= one chunk on the drain side, <= one push on
 * the producer side, worst case the full 16 KB ring), so gdx3ds_audio_push never waits on the
 * DSP or on playback -- only on a microseconds-scale memcpy -- which satisfies the contract's
 * "never blocks" intent. On overrun push drops the OLDEST frames and still queues everything
 * it was given (clamped to ring capacity), returning the frames queued.
 *
 * WAVE BUFFERS. Three 512-frame (16 ms) ndspWaveBufs in linearAlloc memory -- the DSP DMAs
 * straight from linear memory, so each chunk gets DSP_FlushDataCache before
 * ndspChnWaveBufAdd. The drain thread refills any DONE/FREE slot from the ring, padding the
 * tail with silence on underrun so the channel never starves (deterministic latency; partial
 * fills are counted in sUnderrunChunks). gdx3ds_audio_buffered() = ring frames + real
 * (non-padding) frames still queued on the DSP; it over-reports by at most the already-played
 * portion of the currently PLAYING chunk (< 512 frames), which is well inside the pacing
 * loop's tolerance.
 *
 * CORE LADDER (documented in STATUS.md/AUDIO_NOTES.md):
 *   1. core 2 -- New3DS only (APT_CheckNew3DS). Fully preemptive spare core; the target.
 *   2. core 1 -- syscore. Needs APT_SetAppCpuTimeLimit(30) first; for homebrew that call
 *      only succeeds under Luma3DS >= 10.1.1. Cooperative-ish (OS owns it), still off core 0.
 *   3. core 0 -- appcore fallback; always works, shares the core with the game.
 * threadCreate returns NULL when a core is not grantable, which is what walks the ladder.
 * On New3DS osSetSpeedupEnable(true) is requested once (804 MHz + L2 for the whole app).
 *
 * DSP FIRMWARE CAVEAT. ndspInit fails unless the console's DSP component exists at
 * sdmc:/3ds/dspfirm.cdc (libctru's ndspFindAndLoadComponent tries exactly that path, then
 * the hb:ndsp env handle; nothing else). This applies to real hardware (dump once with the
 * DSP1 homebrew) AND to Citra-family emulators (Citra/Lime3DS/Azahar), which need the same
 * file on their EMULATED SD root — the earlier note here claiming "Citra does not need the
 * dump" was wrong, and hid weeks of silent-emulator runs. On failure this backend logs the
 * Result LOUDLY through the [audio-out] channel (svc debug + sdmc filelog — the original
 * printf-only line reached neither sink in the field) and degrades to a NULL SINK: the ring
 * and a real-time-paced drain thread still run, so gdx3ds_audio_push/gdx3ds_audio_buffered
 * keep their exact pacing semantics and the game plays silent instead of dying. init still
 * returns 0 (degraded success); hosts that want to surface a warning can poll
 * gdx3ds_audio_output_active() below (extra diagnostic export, not part of the frozen
 * contract).
 *
 * [audio-out] RECEIPTS (all one-shots unconditional; the periodic line needs INI
 * [debug] diag_audio=1):
 *   - ndspInit result, channel format/rate/mix, wave-buffer vaddr + osConvertVirtToPhys
 *     (paddr=0 would mean non-linear memory the DSP cannot DMA — the classic silent-output
 *     bug; ours is linearAlloc so a nonzero paddr is the expected receipt).
 *   - periodic: chunks submitted / DONE-transitions seen / nonzero-content chunks,
 *     first-16-sample checksum of the last submitted chunk, ring/in-flight/drop/underrun
 *     counters, producer-push totals and a bitwise-OR accumulator over pushed samples
 *     (stays 0x0000 <=> the producer only ever pushed digital silence).
 *   - [debug] audio_testtone=1 replaces every submitted chunk's CONTENT with a 440 Hz sine
 *     (ring pacing untouched): tone audible = output plumbing good, blame producer content;
 *     still silent = ndsp/DSP path broken regardless of what the game produces.
 */
#include "gdx3ds_audio.h"
#include "gdx3ds_filelog.h"

#include <3ds.h>

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Stream B INI (port/3ds/os/gdx3ds_config.h), loaded in main() before gdx3ds_audio_init.
 * Extern-declared like the other cross-stream diagnostics instead of linking gdx3ds_os
 * into this leaf library. */
extern int gdx3ds_config_get_bool(const char* section, const char* key, int fallback);
extern int gdx3ds_config_get_int(const char* section, const char* key, int fallback);

/* MENU AUD tab: master volume percent (0-100), applied via ndspSetMasterVol. Music/sfx
 * split deliberately NOT offered in v1: the split lives inside the guest ROM audio lib
 * (decomp/src/audio/rom seqplayer state), not at the ndsp boundary — no cheap hook. */
static int sMasterVolPct = 100;

#define GDX3DS_AUDIO_DEFAULT_RING_FRAMES 4096u
#define GDX3DS_AUDIO_CHUNK_FRAMES 512u /* 16 ms per wave buffer at 32 kHz */
#define GDX3DS_AUDIO_NUM_WAVEBUFS 3u   /* one playing, one queued, one being refilled */
#define GDX3DS_AUDIO_BYTES_PER_FRAME (GDX3DS_AUDIO_CHANNELS * sizeof(int16_t))
#define GDX3DS_AUDIO_CHANNEL 0
#define GDX3DS_AUDIO_DRAIN_STACK (16u * 1024u)
#define GDX3DS_AUDIO_DRAIN_PRIO 0x18 /* above the 0x30 main thread, below OS services */
#define GDX3DS_AUDIO_SYSCORE_APP_PERCENT 30u
#define GDX3DS_AUDIO_DRAIN_POLL_NS 2000000LL /* 2 ms; chunk period is 16 ms */
#define GDX3DS_AUDIO_CHUNK_PERIOD_NS \
    ((s64)(GDX3DS_AUDIO_CHUNK_FRAMES * 1000000000ull / GDX3DS_AUDIO_SAMPLE_RATE))

typedef struct {
    ndspWaveBuf waveBuf;
    uint32_t realFrames; /* non-padding frames submitted in this slot, not yet retired */
} Gdx3dsAudioSlot;

static struct {
    int initialized;
    int ndspOk;    /* 0 => null-sink mode (ndspInit failed, e.g. no dspfirm.cdc) */
    int drainCore; /* rung of the core ladder actually granted */
    uint32_t capacityFrames;
    int16_t* ring; /* heap; interleaved L/R s16, capacityFrames frames */
    uint32_t readPos;
    uint32_t countFrames;    /* frames in the ring; writePos == readPos + countFrames */
    uint32_t inFlightFrames; /* real frames inside QUEUED/PLAYING wave buffers */
    uint32_t droppedFrames;  /* overrun drop-oldest total (diagnostic) */
    uint32_t underrunChunks; /* chunks submitted with a silence tail (diagnostic) */
    LightLock lock;          /* guards readPos/countFrames/inFlightFrames/droppedFrames */
    Thread drainThread;
    volatile int stopRequested;
    int16_t* waveMem; /* linearAlloc; NUM_WAVEBUFS * CHUNK_FRAMES frames */
    Gdx3dsAudioSlot slots[GDX3DS_AUDIO_NUM_WAVEBUFS];
} s;

/* ---- [audio-out] diagnostics (benign unsynchronized counters, watchdog idiom) ---- */
static int sDiagAudio;             /* INI [debug] diag_audio=1: periodic drain receipts */
static int sTestTone;              /* INI [debug] audio_testtone=1: 440 Hz sine output */
static uint32_t sChunksSubmitted;  /* ndspChnWaveBufAdd calls */
static uint32_t sChunksDoneSeen;   /* NDSP_WBUF_DONE transitions observed (proves the DSP
                                    * actually consumed buffers; stuck at 0 with submitted>0
                                    * means the DSP never played anything) */
static uint32_t sNonzeroChunks;    /* submitted chunks containing any nonzero sample */
static uint32_t sLastChunkCk16;    /* checksum over the first 16 samples of the last chunk */
static uint32_t sPushCalls;        /* gdx3ds_audio_push invocations */
static uint32_t sPushFrames;       /* frames accepted from the producer */
static uint16_t sPushOrAccum;      /* bitwise OR of every pushed sample (diag_audio only);
                                    * 0x0000 forever <=> producer pushed pure silence */
static float sTonePhase;           /* test-tone oscillator state (drain thread only) */

#define GDX3DS_AUDIO_TESTTONE_HZ 440.0f
#define GDX3DS_AUDIO_TESTTONE_AMP 8000 /* ~25% full scale; clearly audible, no clipping */

/* ---- [audioprime] boot priming + ring headroom (LOCKED-60 Task D) ------------------------
 * Symptom on hardware: `under=2` within the first ~10 s of boot, audible as two hiccups.
 * An underrun edge is a PARTIAL chunk: the drain found 0 < ring < 512 frames when a slot
 * came DONE, so the chunk got a silence tail spliced into the stream. Two things make that
 * easy at boot: (1) the DSP is fed from an EMPTY ring the moment the drain starts, so the
 * first real pushes race the 16 ms slot cadence at tick granularity (528-frame ticks against
 * 512-frame chunks); (2) the producer paces on buffered() = ring + in-flight, targeting
 * 2048 frames, and with THREE 512-frame slots in flight the steady-state ring holds only
 * ~512-1040 frames -- a producer stall of a few ms past the next DONE already splices.
 *
 * [debug] audioprime=1 (default; =0 restores the old path byte for byte):
 *   PRIME  -- the drain feeds pure silence (ring untouched, not counted as underrun) until
 *             the ring holds >= GDX3DS_AUDIO_PRIME_FRAMES, bounded by PRIME_TIMEOUT_MS after
 *             the first push so a short-of-target producer can never hold audio hostage.
 *             Re-armed by gdx3ds_audio_resume() (HOME/sleep restarts from an empty ring).
 *   QUEUE  -- at most GDX3DS_AUDIO_PRIME_QUEUE slots (2) are in flight instead of 3. The
 *             producer's 2048-frame target is unchanged (same latency, same race-start
 *             sync), so the cushion MOVES from the DSP queue into the ring: steady-state
 *             ring ~1024-1552 frames, i.e. a stall must exceed ~16-32 ms (was ~0-16 ms)
 *             before it can splice. The drain still refills a DONE slot within one 2 ms poll,
 *             well inside the 16 ms the remaining queued chunk buys.
 * Receipts (diag_audio=1): `[audioprime]` line on the [audio-out] cadence plus a 500 ms
 * burst for the first 5 s of boot (rmin = lowest ring level seen at a pull since the last
 * line = the live headroom margin), one-shot `under edge` lines for the first 8 partial
 * chunks (time since init, real frames, push count) so a hardware log pins WHEN the
 * producer fell behind, and prime completion/timeout one-shots.
 * [debug] audio_stallsim=<ms> (default 0, diagnostic): the producer sleeps <ms> before
 * pushes #20, #60 and #100 -- an emulator stand-in for the slow hardware ticks (font
 * conversion, first-touch sample reads) that Azahar's host CPU never reproduces. */
#define GDX3DS_AUDIO_PRIME_FRAMES (2u * GDX3DS_AUDIO_CHUNK_FRAMES)
#define GDX3DS_AUDIO_PRIME_TIMEOUT_MS 1000u
#define GDX3DS_AUDIO_PRIME_QUEUE 2u
#define GDX3DS_AUDIO_PRIME_BOOT_BURST_MS 5000u
#define GDX3DS_AUDIO_PRIME_BURST_POLLS 250u /* 2 ms drain poll * 250 = one line per 500 ms */
#define GDX3DS_AUDIO_UNDER_EDGE_RECEIPTS 8u
static int sPrimeEnabled;              /* INI [debug] audioprime (default 1) */
static volatile int sPrimeActive;      /* 1 while the drain feeds silence and holds the ring */
static uint32_t sPrimeSilenceChunks;   /* chunks fed during the current prime */
static uint32_t sPrimeCompletions;     /* primes that reached PRIME_FRAMES */
static uint32_t sPrimeTimeouts;        /* primes ended by the 1 s bound instead */
static uint64_t sPrimeArmedMs;         /* osGetTime() when the current prime started */
static uint64_t sFirstPushMs;          /* osGetTime() of the first push since (re)arm; 0 = none */
static uint64_t sInitMs;               /* osGetTime() at gdx3ds_audio_init */
static uint32_t sRingMinSincePrint;    /* lowest ring level seen by PullFromRing since the last line */
static uint32_t sQueueSkips;           /* drain iterations that left a slot empty by the queue cap */
static int sStallSimMs;                /* INI [debug] audio_stallsim */

/* ---- [audioprime2] boot cushion, second attempt (LOCKED-60 round 2, Task G) ---------------
 * Hardware still showed `under=2` at ~10.7 s after boot WITH audioprime: `ring=1184 rmin=320`,
 * i.e. the ring dipped below one chunk during boot although it had been primed to ~2144. The
 * emulator never stalls there (Azahar's host CPU has no slow tick, its SD is a host file), so
 * the round-2 levers are reasoned from the receipts and made provable on hardware:
 *   (a) PRELOAD CORE KNOB. main_3ds.cpp's asset preload worker (prio 0x30) shares core 2
 *       with the 0x18 drain + producer threads. [debug] audioprime2_preload_core selects its
 *       first rung: 2 (default) = the round-1 ladder (core 2 on New3DS -> syscore -> appcore),
 *       1 = skip core 2 (syscore if the share is granted, gdx3ds_audio_grant_syscore records
 *       the percent for the HOME-restore re-apply; else appcore 0x3D), 0 = appcore 0x3D only.
 *       Emulator verdict (bootaudio2-progress.md): the preload is an INFLATE of the 10.7 MB
 *       Deflate-stored audio_table, i.e. CPU-bound, and on the 30% syscore it took 8.3 s
 *       instead of 4.3 s -- long enough that the title font's sample load (t~4.7 s) blocked
 *       the producer for 2.1 s inside the in-flight wait: the receipt below pinned it. So
 *       the default keeps core 2, where the worker is alone with the (preempting) audio
 *       threads; the knob exists for a hardware A/B.
 *   (b) BOOT-ONLY PRODUCER TARGET. The HLE producer paces on buffered() < target; the target
 *       is PRIME2_BOOT_TARGET_FRAMES (4096 = 128 ms) until the first race-active frame
 *       (gGdxRaceActive, set by the first course load, well before the countdown) or
 *       PRIME2_BOOT_WINDOW_MS after init, then PRIME2_RUN_TARGET_FRAMES (2048, the unchanged
 *       race value: race-start audio sync is byte-identical). The ring is 8192 frames on the
 *       audioprime2 path so a 4096-frame target plus one 528-frame tick can never wrap the
 *       ring into drop-oldest (with 2048 and the 4096 ring that was never possible either).
 *   (c) UNDER EDGE DETAIL. Each `under edge` one-shot gets an [audioprime2] companion line
 *       with the producer's tick state at the edge (mid-tick for how long, last tick duration,
 *       max tick since the previous receipt, tick count), the main thread's boot phase
 *       (preload/warm/bootproc/frames), whether the preload worker is still running and on
 *       which core, and the producer/drain cores -- the 3DS kernel exposes no "which thread is
 *       on core N" query, so this is the thread map a hardware log can pin the stall to.
 * [debug] audioprime2=0 restores the round-1 (audioprime) path byte for byte: 4096 ring,
 * constant 2048 target, core-2 preload rung, no extra lines. */
#define GDX3DS_AUDIO_PRIME2_RING_FRAMES 8192u
#define GDX3DS_AUDIO_PRIME2_BOOT_TARGET_FRAMES 4096
#define GDX3DS_AUDIO_PRIME2_RUN_TARGET_FRAMES 2048
#define GDX3DS_AUDIO_PRIME2_BOOT_WINDOW_MS 15000u
#define GDX3DS_AUDIO_TICKS_PER_US (SYSCLOCK_ARM11 / 1000000u)
static int sPrime2Enabled;                  /* INI [debug] audioprime2 (default 1) */
static volatile int sBootTargetActive;      /* 1 while the producer paces to the boot target */
static uint32_t sBootTargetReleaseMs;       /* MsSinceInit() when the boot target was released */
static volatile int sProdInTick;            /* producer is inside gdx_audio_produce_one_tick */
static uint64_t sProdTickStartTick;         /* svcGetSystemTick at the current/last tick start */
static uint32_t sProdLastTickUs;            /* duration of the last completed producer tick */
static uint32_t sProdMaxTickUs;             /* longest tick since the previous [audioprime2] line */
static uint32_t sProdTickCount;             /* completed producer ticks since init */
static const char* volatile sMainPhase = "init"; /* main thread's boot phase tag */
static int sPreloadCorePolicy;              /* INI [debug] audioprime2_preload_core (2/1/0) */
static volatile int sPreloadRunning;        /* asset preload worker between start and end */
static volatile int sPreloadCore = -1;      /* core main_3ds.cpp's ladder placed the worker on */

static uint32_t TicksToUs(uint64_t ticks) {
    return (uint32_t)(ticks / GDX3DS_AUDIO_TICKS_PER_US);
}

/* svc debug channel (Azahar log / Luma) + sdmc:/3ds/gdiffuser/log.txt (debug.filelog=1).
 * The pre-fix printf-only logging reached ONLY the console, which is disabled by default — a
 * failed ndspInit was invisible on both emulator and hardware. No console printf here on
 * purpose: this runs on the drain thread too (periodic receipts, first-chunk line) and the
 * libctru console is not thread-safe; the MENU LOG tab reads the filelog ring instead. */
int gdx3ds_audio_producer_core(void); /* defined with the producer hooks below */

static void AudioOutLogf(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) {
        return;
    }
    if ((size_t)n >= sizeof(buf)) {
        n = (int)sizeof(buf) - 1;
    }
    svcOutputDebugString(buf, n);
    gdx3ds_filelog_write(buf, (size_t)n);
}

/* ---- APT suspend gate (HOME menu / lid sleep) --------------------------------------------
 *
 * libctru's HOME/sleep sequence runs ndspFinalize (DSP pipe shutdown, irq event + semaphore
 * closed) right after the app's APT hook returns, and only ndsp itself may touch the DSP
 * across that window. Both our audio threads keep running through it, so
 * gdx3ds_audio_suspend() (called from main's ONSUSPEND/ONSLEEP hook) raises sSuspended and
 * WAITS until the drain thread and the HLE producer have each acknowledged by parking on
 * their park event -- no DSP_FlushDataCache / ndspChnWaveBufAdd, no ring traffic, no logging
 * from either thread until gdx3ds_audio_resume() (ONRESTORE/ONWAKEUP) resets the channel
 * queue + slots + ring and signals both events. The events are sticky: a thread that checks
 * the flag late still passes once resumed. Flag/park counters are the port's benign
 * aligned-int cross-thread idiom; the hook side polls them with a bounded sleep.
 *
 * ONE EVENT PER THREAD (close-from-HOME hang fix, docs/research/home-crash-audit.md): a
 * Close order from the HOME menu wakes main with the app still suspended (no ONRESTORE),
 * so main's teardown joins the producer FIRST while sSuspended is still 1. With a single
 * shared event that join signal also woke the drain thread, whose park loop then found
 * "suspended && !stopRequested" still true and re-waited -- but LightEvent_Wait on a
 * SIGNALED sticky event returns immediately without any svc, so the drain became a hot
 * 0x18 spinner on the core it shares with the 0x18 producer (core 2) and the producer never
 * ran again; threadJoin never returned and the app wedged with the HOME animation running.
 * Each thread now waits on an event that only its own two legitimate wakers signal (resume,
 * and its own stop/release order), so a wake always ends the park: no spin, no re-wait. */
static volatile int sSuspended;      /* 1 between suspend() and resume() */
static volatile int sDrainParked;    /* drain / null-sink thread is blocked on its event */
static volatile int sProducerParked; /* HLE producer thread is blocked on its event */
static volatile int sProducerRelease; /* join() requested: let a parked producer leave */
static LightEvent sDrainParkEvent;    /* signalers: resume(), shutdown() */
static LightEvent sProducerParkEvent; /* signalers: resume(), producer_thread_join() */
static int sSuspendGateInited;
static int sSyscoreLimitPct; /* APT_SetAppCpuTimeLimit percent granted at init, else 0 */

#define GDX3DS_AUDIO_SUSPEND_ACK_POLL_NS 1000000LL /* 1 ms */
#define GDX3DS_AUDIO_SUSPEND_ACK_POLLS 50u          /* ~50 ms bound on the park wait */

static void EnsureSuspendGate(void) {
    if (!sSuspendGateInited) {
        LightEvent_Init(&sDrainParkEvent, RESET_STICKY);
        LightEvent_Init(&sProducerParkEvent, RESET_STICKY);
        LightEvent_Signal(&sDrainParkEvent); /* not suspended: waiters never block */
        LightEvent_Signal(&sProducerParkEvent);
        sSuspendGateInited = 1;
    }
}

/* Drain-side park: called at the top of every drain-loop iteration while suspended. Returns
 * with sSuspended cleared (resumed) OR s.stopRequested set (shutdown while still suspended:
 * the caller's loop condition then exits WITHOUT touching ndsp -- the DSP is asleep). */
static void ParkDrainWhileSuspended(void) {
    sDrainParked = 1;
    while (sSuspended && !s.stopRequested) {
        LightEvent_Wait(&sDrainParkEvent);
    }
    sDrainParked = 0;
}

/* Pull up to maxFrames from the ring into dst (NULL = discard, null-sink mode). Returns the
 * frames taken; in copy mode they are accounted as in-flight until RetireSlot. */
static uint32_t PullFromRing(int16_t* dst, uint32_t maxFrames) {
    LightLock_Lock(&s.lock);
    uint32_t take = (s.countFrames < maxFrames) ? s.countFrames : maxFrames;
    if (s.countFrames < sRingMinSincePrint) {
        sRingMinSincePrint = s.countFrames; /* headroom receipt; the lock is already held */
    }
    if (dst != NULL && take > 0) {
        uint32_t first = s.capacityFrames - s.readPos;
        if (first > take) {
            first = take;
        }
        memcpy(dst, &s.ring[s.readPos * GDX3DS_AUDIO_CHANNELS],
               (size_t)first * GDX3DS_AUDIO_BYTES_PER_FRAME);
        if (take > first) {
            memcpy(dst + (size_t)first * GDX3DS_AUDIO_CHANNELS, &s.ring[0],
                   (size_t)(take - first) * GDX3DS_AUDIO_BYTES_PER_FRAME);
        }
        s.inFlightFrames += take;
    }
    s.readPos = (s.readPos + take) % s.capacityFrames;
    s.countFrames -= take;
    LightLock_Unlock(&s.lock);
    return take;
}

static void RetireSlot(Gdx3dsAudioSlot* slot) {
    if (slot->realFrames > 0) {
        LightLock_Lock(&s.lock);
        s.inFlightFrames -= slot->realFrames;
        LightLock_Unlock(&s.lock);
        slot->realFrames = 0;
    }
}

static uint64_t MsSinceInit(void) {
    return osGetTime() - sInitMs;
}

static void ArmPrime(void) {
    sPrimeArmedMs = osGetTime();
    sFirstPushMs = 0;
    sPrimeSilenceChunks = 0;
    sPrimeActive = 1;
}

/* Drain thread only. 1 = keep feeding silence and leave the ring alone this chunk. Ends the
 * prime once the ring reaches PRIME_FRAMES, or PRIME_TIMEOUT_MS after the first push. The
 * unlocked countFrames read is the benign aligned-int diag race used throughout. */
static int PrimeHoldsRing(void) {
    if (!sPrimeActive) {
        return 0;
    }
    uint32_t ringNow = s.countFrames;
    if (ringNow >= GDX3DS_AUDIO_PRIME_FRAMES) {
        sPrimeActive = 0;
        sPrimeCompletions++;
        AudioOutLogf("[audioprime] primed: ring=%lu frames at t=%lums, %lu ms after arm "
                     "(%lu silence chunks, first push at t=%lums)",
                     (unsigned long)ringNow, (unsigned long)MsSinceInit(),
                     (unsigned long)(osGetTime() - sPrimeArmedMs),
                     (unsigned long)sPrimeSilenceChunks,
                     (unsigned long)(sFirstPushMs ? sFirstPushMs - sInitMs : 0));
        return 0;
    }
    if (sFirstPushMs != 0 && osGetTime() - sFirstPushMs >= GDX3DS_AUDIO_PRIME_TIMEOUT_MS) {
        sPrimeActive = 0;
        sPrimeTimeouts++;
        AudioOutLogf("[audioprime] prime TIMEOUT: ring=%lu < %u frames %u ms after the first "
                     "push (t=%lums) -- releasing the ring",
                     (unsigned long)ringNow, (unsigned)GDX3DS_AUDIO_PRIME_FRAMES,
                     (unsigned)GDX3DS_AUDIO_PRIME_TIMEOUT_MS, (unsigned long)MsSinceInit());
        return 0;
    }
    sPrimeSilenceChunks++;
    return 1;
}

/* Drain thread only, audioprime2 on: the companion line for an `under edge` one-shot (see the
 * [audioprime2] header, item c). Unlocked reads of the producer's tick state: benign
 * aligned-int diag race. */
static void LogUnderEdgeDetail(uint32_t realFrames) {
    if (!sPrime2Enabled) {
        return;
    }
    uint32_t inTickUs = sProdInTick ? TicksToUs(svcGetSystemTick() - sProdTickStartTick) : 0;
    AudioOutLogf("[audioprime2] under edge #%lu: t=%lums real=%lu target=%d boot=%d tick{in=%d "
                 "for=%luus last=%luus max=%luus n=%lu} main=%s preload=%s/core%d cores=prod%d/drain%d",
                 (unsigned long)s.underrunChunks, (unsigned long)MsSinceInit(),
                 (unsigned long)realFrames,
                 sBootTargetActive ? GDX3DS_AUDIO_PRIME2_BOOT_TARGET_FRAMES
                                   : GDX3DS_AUDIO_PRIME2_RUN_TARGET_FRAMES,
                 sBootTargetActive, sProdInTick, (unsigned long)inTickUs,
                 (unsigned long)sProdLastTickUs, (unsigned long)sProdMaxTickUs,
                 (unsigned long)sProdTickCount, sMainPhase,
                 sPreloadRunning ? "running" : "done", sPreloadCore,
                 gdx3ds_audio_producer_core(), s.drainCore);
}

static void FillAndSubmitSlot(uint32_t slotIndex) {
    Gdx3dsAudioSlot* slot = &s.slots[slotIndex];
    int16_t* pcm = &s.waveMem[(size_t)slotIndex * GDX3DS_AUDIO_CHUNK_FRAMES * GDX3DS_AUDIO_CHANNELS];

    uint32_t real = 0;
    if (!PrimeHoldsRing()) {
        real = PullFromRing(pcm, GDX3DS_AUDIO_CHUNK_FRAMES);
    }
    if (real < GDX3DS_AUDIO_CHUNK_FRAMES) {
        memset(pcm + (size_t)real * GDX3DS_AUDIO_CHANNELS, 0,
               (size_t)(GDX3DS_AUDIO_CHUNK_FRAMES - real) * GDX3DS_AUDIO_BYTES_PER_FRAME);
        if (real > 0) {
            /* Partial fill = genuine underrun edge. Pure-silence chunks are not counted:
             * they are the steady state whenever production is legitimately idle. */
            s.underrunChunks++;
            if (s.underrunChunks <= GDX3DS_AUDIO_UNDER_EDGE_RECEIPTS) {
                AudioOutLogf("[audioprime] under edge #%lu: t=%lums sub=%lu real=%lu ring=%lu "
                             "inflight=%lu push=%lu/%lu prime=%d",
                             (unsigned long)s.underrunChunks, (unsigned long)MsSinceInit(),
                             (unsigned long)sChunksSubmitted, (unsigned long)real,
                             (unsigned long)s.countFrames, (unsigned long)s.inFlightFrames,
                             (unsigned long)sPushCalls, (unsigned long)sPushFrames,
                             sPrimeEnabled);
                LogUnderEdgeDetail(real);
            }
        }
    }
    slot->realFrames = real;

    if (sTestTone) {
        /* Output-plumbing bisector: the ring was still drained above (pacing identical),
         * but the DSP hears a clean 440 Hz sine instead of game content. */
        const float step = 2.0f * (float)M_PI * GDX3DS_AUDIO_TESTTONE_HZ / (float)GDX3DS_AUDIO_SAMPLE_RATE;
        for (uint32_t f = 0; f < GDX3DS_AUDIO_CHUNK_FRAMES; f++) {
            int16_t v = (int16_t)(sinf(sTonePhase) * (float)GDX3DS_AUDIO_TESTTONE_AMP);
            pcm[f * GDX3DS_AUDIO_CHANNELS] = v;
            pcm[f * GDX3DS_AUDIO_CHANNELS + 1] = v;
            sTonePhase += step;
            if (sTonePhase > 2.0f * (float)M_PI) {
                sTonePhase -= 2.0f * (float)M_PI;
            }
        }
    }

    /* Content receipts: cheap full-chunk nonzero scan + a first-16-sample checksum that a
     * log reader can compare across runs (identical silence checksums = 0). */
    {
        uint32_t ck = 0;
        uint32_t k;
        for (k = 0; k < 16; k++) {
            ck = ck * 31u + (uint16_t)pcm[k];
        }
        sLastChunkCk16 = ck;
        for (k = 0; k < GDX3DS_AUDIO_CHUNK_FRAMES * GDX3DS_AUDIO_CHANNELS; k++) {
            if (pcm[k] != 0) {
                sNonzeroChunks++;
                break;
            }
        }
    }

    DSP_FlushDataCache(pcm, GDX3DS_AUDIO_CHUNK_FRAMES * GDX3DS_AUDIO_BYTES_PER_FRAME);
    slot->waveBuf.data_pcm16 = pcm;
    slot->waveBuf.nsamples = GDX3DS_AUDIO_CHUNK_FRAMES; /* per-channel frames, stereo PCM16 */
    slot->waveBuf.looping = false;
    ndspChnWaveBufAdd(GDX3DS_AUDIO_CHANNEL, &slot->waveBuf);

    sChunksSubmitted++;
    if (sChunksSubmitted == 1u) {
        /* paddr=0 here would be THE classic 3DS silence bug (wave buffer outside linear
         * memory, invisible to the DSP's DMA). linearAlloc makes it nonzero by construction;
         * the receipt exists so a regression can never hide again. */
        AudioOutLogf("[audio-out] first chunk: vaddr=%p paddr=0x%08lX nsamples=%u looping=0",
                     (void*)pcm, (unsigned long)osConvertVirtToPhys(pcm),
                     (unsigned)GDX3DS_AUDIO_CHUNK_FRAMES);
    }
}

#define GDX3DS_AUDIO_DIAG_POLLS 2500u /* 2 ms drain poll * 2500 = one receipt per ~5 s */

/* [audioprime2] receipt, drain thread only, on the [audioprime] cadence (diag_audio=1):
 * the live producer target, the boot-window state, the longest producer tick since the
 * previous line (reset here), the tick count, and the thread map (main phase, preload). */
static void LogPrime2Receipt(void) {
    if (!sPrime2Enabled) {
        return;
    }
    AudioOutLogf("[audioprime2] on=1 t=%lums target=%d boot=%d rel=%lums cap=%lu tick{last=%luus "
                 "max=%luus n=%lu in=%d} main=%s preload=%s/core%d",
                 (unsigned long)MsSinceInit(),
                 sBootTargetActive ? GDX3DS_AUDIO_PRIME2_BOOT_TARGET_FRAMES
                                   : GDX3DS_AUDIO_PRIME2_RUN_TARGET_FRAMES,
                 sBootTargetActive, (unsigned long)sBootTargetReleaseMs,
                 (unsigned long)s.capacityFrames, (unsigned long)sProdLastTickUs,
                 (unsigned long)sProdMaxTickUs, (unsigned long)sProdTickCount, sProdInTick,
                 sMainPhase, sPreloadRunning ? "running" : "done", sPreloadCore);
    sProdMaxTickUs = 0;
}

/* [audioprime] receipt, drain thread only (diag_audio=1). Same reader contract as the
 * [audio-out] line; `rmin` is the lowest ring level a pull found since the previous line
 * (the live margin before a splice), reset here. */
static void LogPrimeReceipt(void) {
    uint32_t queued = 0;
    for (uint32_t i = 0; i < GDX3DS_AUDIO_NUM_WAVEBUFS; i++) {
        u8 st = s.slots[i].waveBuf.status;
        if (st == NDSP_WBUF_QUEUED || st == NDSP_WBUF_PLAYING) {
            queued++;
        }
    }
    AudioOutLogf("[audioprime] on=%d t=%lums state=%s primes=%lu/%lu ring=%lu rmin=%lu "
                 "queued=%lu/%u skips=%lu under=%lu push=%lu/%lu stallsim=%d",
                 sPrimeEnabled, (unsigned long)MsSinceInit(), sPrimeActive ? "prime" : "run",
                 (unsigned long)sPrimeCompletions, (unsigned long)sPrimeTimeouts,
                 (unsigned long)s.countFrames, (unsigned long)sRingMinSincePrint,
                 (unsigned long)queued,
                 (unsigned)(sPrimeEnabled ? GDX3DS_AUDIO_PRIME_QUEUE : GDX3DS_AUDIO_NUM_WAVEBUFS),
                 (unsigned long)sQueueSkips, (unsigned long)s.underrunChunks,
                 (unsigned long)sPushCalls, (unsigned long)sPushFrames, sStallSimMs);
    sRingMinSincePrint = UINT32_MAX;
    LogPrime2Receipt();
}

/* QUEUE lever: with audioprime on, refuse to put a third chunk in flight so the cushion
 * stays in the ring (see the [audioprime] header). Off = the old unconditional refill. */
static int QueueCapReached(void) {
    if (!sPrimeEnabled) {
        return 0;
    }
    uint32_t queued = 0;
    for (uint32_t i = 0; i < GDX3DS_AUDIO_NUM_WAVEBUFS; i++) {
        u8 st = s.slots[i].waveBuf.status;
        if (st == NDSP_WBUF_QUEUED || st == NDSP_WBUF_PLAYING) {
            queued++;
        }
    }
    return queued >= GDX3DS_AUDIO_PRIME_QUEUE;
}

static void DrainThreadMain(void* arg) {
    (void)arg;
    uint32_t polls = 0;
    while (!s.stopRequested) {
        if (sSuspended) {
            ParkDrainWhileSuspended(); /* HOME/sleep: nothing below may run (see gate) */
            continue;
        }
        for (uint32_t i = 0; i < GDX3DS_AUDIO_NUM_WAVEBUFS; i++) {
            u8 status = s.slots[i].waveBuf.status;
            if (status == NDSP_WBUF_DONE || status == NDSP_WBUF_FREE) {
                if (status == NDSP_WBUF_DONE) {
                    sChunksDoneSeen++; /* the DSP consumed a buffer — playback is really running */
                }
                RetireSlot(&s.slots[i]);
                if (QueueCapReached()) {
                    if (status == NDSP_WBUF_DONE) {
                        /* ndsp has released this buffer; park it as FREE ourselves so the next
                         * poll neither re-counts the DONE transition nor re-retires it. */
                        sQueueSkips++;
                        s.slots[i].waveBuf.status = NDSP_WBUF_FREE;
                    }
                    continue; /* refilled on a later poll, once a queued chunk finishes */
                }
                FillAndSubmitSlot(i);
            }
        }
        svcSleepThread(GDX3DS_AUDIO_DRAIN_POLL_NS);
        if (sDiagAudio) {
            uint32_t burstPolls = polls + 1u;
            if (burstPolls % GDX3DS_AUDIO_DIAG_POLLS == 0u ||
                (burstPolls % GDX3DS_AUDIO_PRIME_BURST_POLLS == 0u &&
                 MsSinceInit() < GDX3DS_AUDIO_PRIME_BOOT_BURST_MS)) {
                LogPrimeReceipt();
            }
        }
        if (sDiagAudio && (++polls % GDX3DS_AUDIO_DIAG_POLLS) == 0u) {
            /* Unlocked reads: benign aligned-int diag race, same as the watchdog counters.
             * Reading the line: done==0 while sub grows => DSP never plays (ndsp/firmware
             * problem); nz==0 while push OR==0x0000 => producer ships pure silence (content
             * problem, not output plumbing); everything moving + nz>0 => audible content is
             * reaching the DSP, so listen (volume slider / headphone jack territory). */
            AudioOutLogf("[audio-out] sub=%lu done=%lu nz=%lu ck16=0x%08lX ring=%lu inflight=%lu "
                         "drop=%lu under=%lu push=%lu/%lu or=0x%04X st=%u/%u/%u tone=%d "
                         "play=%d paused=%d pos=%lu mvol=%.2f",
                         (unsigned long)sChunksSubmitted, (unsigned long)sChunksDoneSeen,
                         (unsigned long)sNonzeroChunks, (unsigned long)sLastChunkCk16,
                         (unsigned long)s.countFrames, (unsigned long)s.inFlightFrames,
                         (unsigned long)s.droppedFrames, (unsigned long)s.underrunChunks,
                         (unsigned long)sPushCalls, (unsigned long)sPushFrames,
                         (unsigned)sPushOrAccum, (unsigned)s.slots[0].waveBuf.status,
                         (unsigned)s.slots[1].waveBuf.status, (unsigned)s.slots[2].waveBuf.status,
                         sTestTone, ndspChnIsPlaying(GDX3DS_AUDIO_CHANNEL) ? 1 : 0,
                         ndspChnIsPaused(GDX3DS_AUDIO_CHANNEL) ? 1 : 0,
                         (unsigned long)ndspChnGetSamplePos(GDX3DS_AUDIO_CHANNEL),
                         (double)ndspGetMasterVol());
        }
    }
}

/* No-DSP fallback: consume the ring at real-time rate so gdx3ds_audio_buffered() paces the
 * producer exactly as it would with output; the game runs silent instead of dying. Sleep
 * overhead makes this drain marginally slow; the ring's drop-oldest policy absorbs the creep. */
static void NullSinkThreadMain(void* arg) {
    (void)arg;
    uint32_t iters = 0;
    while (!s.stopRequested) {
        if (sSuspended) {
            ParkDrainWhileSuspended(); /* same gate as the ndsp drain, for the receipts */
            continue;
        }
        PullFromRing(NULL, GDX3DS_AUDIO_CHUNK_FRAMES);
        svcSleepThread(GDX3DS_AUDIO_CHUNK_PERIOD_NS);
        if (sDiagAudio && (++iters % 313u) == 0u) { /* 16 ms * 313 ~= every 5 s */
            AudioOutLogf("[audio-out] NULL SINK draining (no DSP output) push=%lu/%lu ring=%lu "
                         "drop=%lu — sdmc:/3ds/dspfirm.cdc missing?",
                         (unsigned long)sPushCalls, (unsigned long)sPushFrames,
                         (unsigned long)s.countFrames, (unsigned long)s.droppedFrames);
        }
    }
}

/* Walk the core ladder (see file header). Returns 0 once a thread is running. */
static int StartDrainThread(void) {
    bool isNew3ds = false;
    APT_CheckNew3DS(&isNew3ds);
    if (isNew3ds) {
        osSetSpeedupEnable(true); /* New3DS 804 MHz + L2; a no-op elsewhere */
    }

    ThreadFunc entry = s.ndspOk ? DrainThreadMain : NullSinkThreadMain;

    int candidates[3];
    int candidateCount = 0;
    if (isNew3ds) {
        candidates[candidateCount++] = 2;
    }
    candidates[candidateCount++] = 1;
    candidates[candidateCount++] = 0;

    for (int i = 0; i < candidateCount; i++) {
        int core = candidates[i];
        if (core == 1) {
            /* Syscore threads need the app's syscore share granted first; for homebrew this
             * call only succeeds under Luma3DS >= 10.1.1 (document for users). */
            if (R_FAILED(APT_SetAppCpuTimeLimit(GDX3DS_AUDIO_SYSCORE_APP_PERCENT))) {
                continue;
            }
            sSyscoreLimitPct = (int)GDX3DS_AUDIO_SYSCORE_APP_PERCENT; /* re-applied on restore */
        }
        s.drainThread = threadCreate(entry, NULL, GDX3DS_AUDIO_DRAIN_STACK,
                                     GDX3DS_AUDIO_DRAIN_PRIO, core, false);
        if (s.drainThread != NULL) {
            s.drainCore = core;
            AudioOutLogf("[audio-out] drain thread on core %d (%s) diag_audio=%d",
                         core, s.ndspOk ? "ndsp" : "NULL SINK — SILENT", sDiagAudio);
            return 0;
        }
    }
    return -1;
}

int gdx3ds_audio_init(uint32_t bufferFrames) {
    if (s.initialized) {
        return 0;
    }
    memset(&s, 0, sizeof(s));
    EnsureSuspendGate();
    s.capacityFrames = (bufferFrames != 0) ? bufferFrames : GDX3DS_AUDIO_DEFAULT_RING_FRAMES;
    sPrime2Enabled = gdx3ds_config_get_bool("debug", "audioprime2", 1);
    sPreloadCorePolicy = gdx3ds_config_get_int("debug", "audioprime2_preload_core", 2);
    if (sPreloadCorePolicy < 0 || sPreloadCorePolicy > 2) {
        sPreloadCorePolicy = 2;
    }
    if (sPrime2Enabled && bufferFrames == 0) {
        s.capacityFrames = GDX3DS_AUDIO_PRIME2_RING_FRAMES; /* room for the boot target + a tick */
    }
    sBootTargetActive = sPrime2Enabled;
    sBootTargetReleaseMs = 0;
    sProdInTick = 0;
    sProdLastTickUs = 0;
    sProdMaxTickUs = 0;
    sProdTickCount = 0;
    LightLock_Init(&s.lock);

    s.ring = (int16_t*)malloc((size_t)s.capacityFrames * GDX3DS_AUDIO_BYTES_PER_FRAME);
    if (s.ring == NULL) {
        return -1;
    }

    sDiagAudio = gdx3ds_config_get_bool("debug", "diag_audio", 0);
    sTestTone = gdx3ds_config_get_bool("debug", "audio_testtone", 0);
    sPrimeEnabled = gdx3ds_config_get_bool("debug", "audioprime", 1);
    sStallSimMs = gdx3ds_config_get_int("debug", "audio_stallsim", 0);
    sInitMs = osGetTime();
    sRingMinSincePrint = UINT32_MAX;
    sPrimeActive = 0;
    if (sPrimeEnabled) {
        ArmPrime();
    }

    Result rc = ndspInit();
    if (R_FAILED(rc)) {
        /* No DSP component. libctru looks ONLY at sdmc:/3ds/dspfirm.cdc (or an hb:ndsp
         * handle) — on real hardware AND on Citra/Lime3DS/Azahar, whose emulated SD needs
         * the same dump. Degrade to the null sink; see the file header. */
        AudioOutLogf("[audio-out] ndspInit FAILED rc=0x%08lX -> NULL SINK, game runs SILENT. "
                     "Fix: put a DSP dump at sdmc:/3ds/dspfirm.cdc (DSP1 homebrew; Azahar: "
                     "same path on its emulated SD).",
                     (unsigned long)rc);
        s.ndspOk = 0;
    } else {
        s.ndspOk = 1;
        s.waveMem = (int16_t*)linearAlloc((size_t)GDX3DS_AUDIO_NUM_WAVEBUFS *
                                          GDX3DS_AUDIO_CHUNK_FRAMES *
                                          GDX3DS_AUDIO_BYTES_PER_FRAME);
        if (s.waveMem == NULL) {
            ndspExit();
            free(s.ring);
            memset(&s, 0, sizeof(s));
            return -1;
        }
        memset(s.waveMem, 0, (size_t)GDX3DS_AUDIO_NUM_WAVEBUFS * GDX3DS_AUDIO_CHUNK_FRAMES *
                                 GDX3DS_AUDIO_BYTES_PER_FRAME);

        /* HW-AUDIO prong A: real-DSP-vs-Azahar-HLE bisect. State EVERYTHING the devkitPro
         * audio/streaming example states (its exact call order), plus explicit master-output
         * config libctru claims as defaults. Azahar's HLE mixer forgives missing/implicit
         * master state that the real DSP firmware may not. Each call is cheap and idempotent. */
        ndspSetOutputMode(NDSP_OUTPUT_STEREO);
        ndspSetClippingMode(NDSP_CLIP_SOFT); /* libctru default; stated, not assumed */
        ndspSetOutputCount(2);               /* libctru default; stated, not assumed */
        ndspChnReset(GDX3DS_AUDIO_CHANNEL);
        ndspChnWaveBufClear(GDX3DS_AUDIO_CHANNEL); /* belt+braces: no stale queue entries */
        /* LINEAR interp is the HARDWARE FIX (HW-AUDIO verdict): POLYPHASE silenced the real
         * DSP while Azahar's HLE (which implements all interp modes identically) played it
         * fine. Keep LINEAR; do not "upgrade" back to POLYPHASE without a hardware re-test. */
        ndspChnSetInterp(GDX3DS_AUDIO_CHANNEL, NDSP_INTERP_LINEAR);
        ndspChnSetRate(GDX3DS_AUDIO_CHANNEL, (float)GDX3DS_AUDIO_SAMPLE_RATE);
        ndspChnSetFormat(GDX3DS_AUDIO_CHANNEL, NDSP_FORMAT_STEREO_PCM16);
        float mix[12];
        memset(mix, 0, sizeof(mix));
        mix[0] = 1.0f; /* front left */
        mix[1] = 1.0f; /* front right */
        ndspChnSetMix(GDX3DS_AUDIO_CHANNEL, mix);
        ndspChnSetPaused(GDX3DS_AUDIO_CHANNEL, false); /* explicit unpause receipt below */
        /* libctru already defaults master volume to 1.0; set it explicitly so the receipt
         * below is a statement about OUR configuration, not about library defaults.
         * MENU: [audio] master_volume (0-100, default 100) persists the AUD-tab slider. */
        sMasterVolPct = gdx3ds_config_get_int("audio", "master_volume", 100);
        if (sMasterVolPct < 0) {
            sMasterVolPct = 0;
        } else if (sMasterVolPct > 100) {
            sMasterVolPct = 100;
        }
        ndspSetMasterVol((float)sMasterVolPct / 100.0f);

        AudioOutLogf("[audio-out] ndsp UP: ch=%d fmt=STEREO_PCM16 rate=%u interp=linear "
                     "clip=soft outcount=2 mix[FL]=1.0 mix[FR]=1.0 mastervol=%.2f paused=%d "
                     "wavemem=%p paddr=0x%08lX ring=%lu frames testtone=%d",
                     GDX3DS_AUDIO_CHANNEL, (unsigned)GDX3DS_AUDIO_SAMPLE_RATE,
                     (double)ndspGetMasterVol(), ndspChnIsPaused(GDX3DS_AUDIO_CHANNEL) ? 1 : 0,
                     (void*)s.waveMem, (unsigned long)osConvertVirtToPhys(s.waveMem),
                     (unsigned long)s.capacityFrames, sTestTone);
        if (sTestTone) {
            AudioOutLogf("[audio-out] TEST TONE active: 440 Hz sine replaces all game audio "
                         "([debug] audio_testtone=1). Tone audible => output plumbing OK, "
                         "investigate producer content. Silent => ndsp/DSP path broken.");
        }
    }

    if (StartDrainThread() != 0) {
        if (s.ndspOk) {
            ndspChnReset(GDX3DS_AUDIO_CHANNEL);
            ndspExit();
        }
        if (s.waveMem != NULL) {
            linearFree(s.waveMem);
        }
        free(s.ring);
        memset(&s, 0, sizeof(s));
        return -1;
    }

    s.initialized = 1;
    AudioOutLogf("[audioprime] %s: prime=%u frames timeout=%u ms queue=%u/%u stallsim=%d ms",
                 sPrimeEnabled ? "ON" : "OFF (old path)", (unsigned)GDX3DS_AUDIO_PRIME_FRAMES,
                 (unsigned)GDX3DS_AUDIO_PRIME_TIMEOUT_MS,
                 (unsigned)(sPrimeEnabled ? GDX3DS_AUDIO_PRIME_QUEUE : GDX3DS_AUDIO_NUM_WAVEBUFS),
                 (unsigned)GDX3DS_AUDIO_NUM_WAVEBUFS, sStallSimMs);
    if (sPrime2Enabled) {
        AudioOutLogf("[audioprime2] ON: ring=%lu frames, producer target %d until race-active or "
                     "%u ms then %d, preload_core=%d (2=round-1 ladder), under-edge tick detail",
                     (unsigned long)s.capacityFrames, GDX3DS_AUDIO_PRIME2_BOOT_TARGET_FRAMES,
                     (unsigned)GDX3DS_AUDIO_PRIME2_BOOT_WINDOW_MS,
                     GDX3DS_AUDIO_PRIME2_RUN_TARGET_FRAMES, sPreloadCorePolicy);
    } else {
        AudioOutLogf("[audioprime2] OFF (round-1 audioprime path): ring=%lu frames, target %d",
                     (unsigned long)s.capacityFrames, GDX3DS_AUDIO_PRIME2_RUN_TARGET_FRAMES);
    }
    return 0;
}

void gdx3ds_audio_shutdown(void) {
    if (!s.initialized) {
        return;
    }
    s.stopRequested = 1;         /* set BEFORE the signal: the park loop re-checks it */
    LightEvent_Signal(&sDrainParkEvent); /* a drain parked by the suspend gate must see stop */
    if (s.drainThread != NULL) {
        threadJoin(s.drainThread, U64_MAX); /* drain loop wakes every <= 16 ms */
        threadFree(s.drainThread);
    }
    if (s.ndspOk) {
        /* Also correct when closing from the HOME menu with the DSP asleep (sSuspended still
         * 1, no ONRESTORE): libctru's aptWaitForWakeUp ran aptDspCancel() before aptMainLoop
         * returned false, so ndsp's bCancelReceived is set and ndspExit() joins its (now
         * idle-polling) sync thread and SKIPS ndspFinalize -- the only DSP-pipe wait in that
         * path. ndspChnWaveBufClear is CPU-side list surgery under the channel lock. */
        ndspChnWaveBufClear(GDX3DS_AUDIO_CHANNEL);
        ndspExit();
    }
    if (s.waveMem != NULL) {
        linearFree(s.waveMem);
    }
    free(s.ring);
    memset(&s, 0, sizeof(s));
}

size_t gdx3ds_audio_push(const int16_t* samples, size_t frames) {
    if (!s.initialized || samples == NULL) {
        return frames; /* swallow, mirroring the Phase 0 stub, so callers never spin */
    }
    if (frames == 0) {
        return 0;
    }

    /* More than a whole ring in one push: only the newest capacityFrames can survive
     * drop-oldest anyway, so skip the doomed prefix up front. */
    uint32_t clampedDrop = 0;
    if (frames > s.capacityFrames) {
        clampedDrop = (uint32_t)(frames - s.capacityFrames);
        samples += (size_t)clampedDrop * GDX3DS_AUDIO_CHANNELS;
        frames = s.capacityFrames;
    }

    /* Producer-side receipts, outside the lock (reads only the caller's buffer). The OR
     * accumulator is the "is the game even producing non-silence?" oracle: it can only stay
     * 0x0000 if every sample ever pushed was zero. Scan gated on diag_audio (2 KB/push). */
    sPushCalls++;
    sPushFrames += (uint32_t)frames;
    if (sFirstPushMs == 0) {
        sFirstPushMs = osGetTime(); /* prime timeout base; producer thread only */
    }
    if (sStallSimMs > 0 && (sPushCalls == 20u || sPushCalls == 60u || sPushCalls == 100u)) {
        /* Diagnostic stand-in for a slow hardware tick: stall the PRODUCER here, before the
         * frames land, exactly where a font conversion or first-touch sample read would. */
        AudioOutLogf("[audioprime] stallsim: sleeping %d ms before push #%lu (ring=%lu)",
                     sStallSimMs, (unsigned long)sPushCalls, (unsigned long)s.countFrames);
        svcSleepThread((s64)sStallSimMs * 1000000LL);
    }
    if (sDiagAudio) {
        uint16_t acc = sPushOrAccum;
        const size_t total = (size_t)frames * GDX3DS_AUDIO_CHANNELS;
        for (size_t k = 0; k < total; k++) {
            acc |= (uint16_t)samples[k];
        }
        sPushOrAccum = acc;
    }

    LightLock_Lock(&s.lock);
    uint32_t freeFrames = s.capacityFrames - s.countFrames;
    if ((uint32_t)frames > freeFrames) {
        uint32_t drop = (uint32_t)frames - freeFrames;
        s.readPos = (s.readPos + drop) % s.capacityFrames;
        s.countFrames -= drop;
        s.droppedFrames += drop;
    }
    s.droppedFrames += clampedDrop;

    uint32_t writePos = (s.readPos + s.countFrames) % s.capacityFrames;
    uint32_t first = s.capacityFrames - writePos;
    if (first > (uint32_t)frames) {
        first = (uint32_t)frames;
    }
    memcpy(&s.ring[writePos * GDX3DS_AUDIO_CHANNELS], samples,
           (size_t)first * GDX3DS_AUDIO_BYTES_PER_FRAME);
    if ((uint32_t)frames > first) {
        memcpy(&s.ring[0], samples + (size_t)first * GDX3DS_AUDIO_CHANNELS,
               ((size_t)frames - first) * GDX3DS_AUDIO_BYTES_PER_FRAME);
    }
    s.countFrames += (uint32_t)frames;
    LightLock_Unlock(&s.lock);

    return frames; /* everything survived drop-oldest (clamp handled above) */
}

size_t gdx3ds_audio_buffered(void) {
    if (!s.initialized) {
        return 0;
    }
    LightLock_Lock(&s.lock);
    size_t buffered = (size_t)s.countFrames + s.inFlightFrames;
    LightLock_Unlock(&s.lock);
    return buffered;
}

/* ---- Diagnostic exports (NOT part of the frozen gdx3ds_audio.h contract; callers that
 * want them declare the prototypes themselves). Counter reads are unsynchronized on
 * purpose -- the benign aligned-int race pattern used elsewhere in the port. ---- */

/* 1 while real DSP output is running; 0 in null-sink (silent) mode or before init. */
int gdx3ds_audio_output_active(void) {
    return s.initialized && s.ndspOk;
}

/* Core the drain thread landed on (2 / 1 / 0), or -1 before init. */
int gdx3ds_audio_drain_core(void) {
    return s.initialized ? s.drainCore : -1;
}

uint32_t gdx3ds_audio_dropped_frames(void) {
    return s.droppedFrames;
}

uint32_t gdx3ds_audio_underrun_chunks(void) {
    return s.underrunChunks;
}

/* MENU AUD tab: live master volume (0-100). ndspSetMasterVol only stages a value the
 * ndsp sync thread commits under its own lock, so calling it from the main thread is
 * safe; a no-op in null-sink mode (the percent is still remembered/persisted). */
void gdx3ds_audio_set_master_volume(int pct) {
    if (pct < 0) {
        pct = 0;
    } else if (pct > 100) {
        pct = 100;
    }
    sMasterVolPct = pct;
    if (s.initialized && s.ndspOk) {
        ndspSetMasterVol((float)pct / 100.0f);
    }
}

int gdx3ds_audio_get_master_volume(void) {
    return sMasterVolPct;
}

/* MENU DBG tab: live [audio-out] receipt toggle (same latch diag_audio=1 sets at boot). */
void gdx3ds_audio_set_diag(int on) {
    sDiagAudio = on ? 1 : 0;
}

int gdx3ds_audio_get_diag(void) {
    return sDiagAudio;
}

/* ---- HLE audio PRODUCER core placement (S4) -------------------------------------------------
 *
 * The ndsp DRAIN thread above already walks the spare-core ladder. The HLE audio PRODUCER --
 * the dedicated thread in port/gdx_audio_thread.cpp that runs gdx_audio_produce_one_tick() --
 * was created with a portable std::thread, which libctru's newlib pthread shim places on the
 * APPLICATION core (core 0, exheader default). That put the whole audio-HLE synthesis workload
 * back on core 0 in direct contention with the game thread and its fibers, defeating the point
 * of the spare-core drain.
 *
 * These two hooks let the portable C++ TU host that thread on a libctru thread instead, created
 * via the SAME core ladder threadCreate walks (core 2 New3DS-only -> core 1 syscore -> core 0
 * appcore). threadCreate returns NULL when a core is not grantable, which walks the ladder; the
 * final core-0 rung ALWAYS succeeds, so this can never fail to start the producer (matching the
 * old std::thread's guarantee). The producer's std::condition_variable / std::mutex wake
 * machinery is thread-implementation agnostic and works unchanged on a libctru thread.
 *
 * This mirrors StartDrainThread's ladder exactly but is kept separate: the two threads are sized
 * and prioritized independently, and either may legitimately land on a different rung (e.g. the
 * drain took core 1's single syscore slot, leaving the producer on core 0).
 *
 * The ndspOk / New3DS speedup state is owned by gdx3ds_audio_init(); this only places a thread,
 * so it is safe to call before OR without a successful ndspInit (null-sink mode still produces). */
#define GDX3DS_AUDIO_PRODUCER_STACK (32u * 1024u)
#define GDX3DS_AUDIO_PRODUCER_PRIO 0x18 /* same rung as the drain: above the 0x30 game thread */

static Thread sProducerThread;
static int sProducerCore = -1;

int gdx3ds_audio_producer_thread_start(void (*entry)(void*), void* arg) {
    if (entry == NULL) {
        return -1;
    }

    bool isNew3ds = false;
    APT_CheckNew3DS(&isNew3ds);
    EnsureSuspendGate(); /* the producer parks on it even when gdx3ds_audio_init failed */

    int candidates[3];
    int candidateCount = 0;
    if (isNew3ds) {
        candidates[candidateCount++] = 2;
    }
    candidates[candidateCount++] = 1;
    candidates[candidateCount++] = 0;

    for (int i = 0; i < candidateCount; i++) {
        int core = candidates[i];
        if (core == 1) {
            /* Same homebrew/Luma>=10.1.1 caveat as the drain thread's syscore rung. The drain
             * may already hold the single syscore slot; then this threadCreate simply returns
             * NULL and we fall through to core 0. */
            if (R_FAILED(APT_SetAppCpuTimeLimit(GDX3DS_AUDIO_SYSCORE_APP_PERCENT))) {
                continue;
            }
            sSyscoreLimitPct = (int)GDX3DS_AUDIO_SYSCORE_APP_PERCENT; /* re-applied on restore */
        }
        sProducerThread = threadCreate((ThreadFunc)entry, arg, GDX3DS_AUDIO_PRODUCER_STACK,
                                       GDX3DS_AUDIO_PRODUCER_PRIO, core, false);
        if (sProducerThread != NULL) {
            sProducerCore = core;
            AudioOutLogf("[audio-out] HLE producer thread on core %d", core);
            return 0;
        }
    }
    return -1;
}

void gdx3ds_audio_producer_thread_join(void) {
    if (sProducerThread != NULL) {
        sProducerRelease = 1; /* a producer parked by the suspend gate must be able to exit */
        LightEvent_Signal(&sProducerParkEvent); /* producer's event ONLY: never wakes the drain */
        threadJoin(sProducerThread, U64_MAX);
        threadFree(sProducerThread);
        sProducerThread = NULL;
    }
}

/* Core the producer landed on (2 / 1 / 0), or -1 before start. Diagnostic. */
int gdx3ds_audio_producer_core(void) {
    return sProducerCore;
}

/* HLE producer park point: gdx_audio_thread.cpp's AudioThreadMain calls this once per wake;
 * it returns 0 immediately when not suspended and otherwise blocks (no ring traffic, no
 * gdx3ds_audio_buffered polling, no production) until gdx3ds_audio_resume() (returns 0) or
 * a join (returns 1: the app is closing while still suspended -- the caller must leave its
 * loop at once without another backend call, the DSP is asleep). */
int gdx3ds_audio_producer_park_if_suspended(void) {
    if (!sSuspended) {
        return 0;
    }
    sProducerParked = 1;
    while (sSuspended && !sProducerRelease) {
        LightEvent_Wait(&sProducerParkEvent);
    }
    sProducerParked = 0;
    return sProducerRelease ? 1 : 0;
}

/* ---- APT suspend gate entry points (main_3ds.cpp aptLifecycleHook) -----------------------
 * Both run on the main thread inside libctru's APT transition, BEFORE ndspFinalize (suspend)
 * and AFTER ndspInitialize (resume). Nothing here logs: the hook writes the receipts through
 * its own svc + filelog path from the return values. */

/* Raise the flag, then wait (bounded, ~50 ms) for every audio thread that exists to park.
 * Returns 1 when all of them acknowledged in time; drainParked/producerParked receive the
 * per-thread state either way (a thread that does not exist counts as parked). */
int gdx3ds_audio_suspend(int* drainParked, int* producerParked) {
    EnsureSuspendGate();
    LightEvent_Clear(&sDrainParkEvent); /* clear BEFORE the flag: a thread seeing 1 must block */
    LightEvent_Clear(&sProducerParkEvent);
    sSuspended = 1;
    int needDrain = (s.drainThread != NULL);
    int needProducer = (sProducerThread != NULL && !sProducerRelease);
    uint32_t polls = 0;
    for (;;) {
        int drainOk = !needDrain || sDrainParked;
        int producerOk = !needProducer || sProducerParked;
        if ((drainOk && producerOk) || polls >= GDX3DS_AUDIO_SUSPEND_ACK_POLLS) {
            if (drainParked != NULL) {
                *drainParked = drainOk;
            }
            if (producerParked != NULL) {
                *producerParked = producerOk;
            }
            return drainOk && producerOk;
        }
        svcSleepThread(GDX3DS_AUDIO_SUSPEND_ACK_POLL_NS);
        polls++;
    }
}

/* Drop the channel queue and everything staged for it, then release both threads. Both are
 * parked (or absent), so the slot/ring resets below race with nothing. ndspInitialize(true)
 * has already re-dirtied the channel; without the clear the DSP would replay stale QUEUED
 * slots and the drain would trust their pre-sleep statuses. */
void gdx3ds_audio_resume(void) {
    EnsureSuspendGate();
    if (s.initialized && s.ndspOk) {
        ndspChnWaveBufClear(GDX3DS_AUDIO_CHANNEL);
        for (uint32_t i = 0; i < GDX3DS_AUDIO_NUM_WAVEBUFS; i++) {
            s.slots[i].waveBuf.status = NDSP_WBUF_FREE;
            s.slots[i].realFrames = 0;
        }
    }
    if (s.initialized) {
        LightLock_Lock(&s.lock);
        s.readPos = 0;
        s.countFrames = 0;
        s.inFlightFrames = 0;
        LightLock_Unlock(&s.lock);
        if (sPrimeEnabled) {
            ArmPrime(); /* both threads are parked: the restart begins from an empty ring */
        }
    }
    sSuspended = 0;
    LightEvent_Signal(&sDrainParkEvent);
    LightEvent_Signal(&sProducerParkEvent);
}

/* 1 while the gate is up (between suspend() and resume()). At main()'s teardown this means
 * the frame loop was left by a Close order delivered while suspended (HOME menu Close /
 * power menu): no ONRESTORE ran, the DSP is asleep (cancelled) and GSP rights are released. */
int gdx3ds_audio_suspended(void) {
    return sSuspended ? 1 : 0;
}

/* ---- [audioprime2] exports (diagnostic-export idiom, not in the frozen header) --------- */

int gdx3ds_audio_prime2_enabled(void) {
    return sPrime2Enabled;
}

/* HLE producer thread only: the buffered() target it paces to this wake. Off = the constant
 * run target (byte-identical pacing). On = the boot target until the first race-active frame
 * or the boot window elapses; the release is a one-shot receipt. */
int32_t gdx3ds_audio_producer_target_frames(int raceActive) {
    if (!sPrime2Enabled || !s.initialized) {
        return GDX3DS_AUDIO_PRIME2_RUN_TARGET_FRAMES;
    }
    if (sBootTargetActive) {
        const char* reason = NULL;
        if (raceActive) {
            reason = "race-active";
        } else if (MsSinceInit() >= GDX3DS_AUDIO_PRIME2_BOOT_WINDOW_MS) {
            reason = "boot window elapsed";
        }
        if (reason != NULL) {
            sBootTargetActive = 0;
            sBootTargetReleaseMs = (uint32_t)MsSinceInit();
            AudioOutLogf("[audioprime2] boot target released (%s): t=%lums target %d -> %d ring=%lu "
                         "inflight=%lu push=%lu/%lu ticks=%lu under=%lu",
                         reason, (unsigned long)sBootTargetReleaseMs,
                         GDX3DS_AUDIO_PRIME2_BOOT_TARGET_FRAMES, GDX3DS_AUDIO_PRIME2_RUN_TARGET_FRAMES,
                         (unsigned long)s.countFrames, (unsigned long)s.inFlightFrames,
                         (unsigned long)sPushCalls, (unsigned long)sPushFrames,
                         (unsigned long)sProdTickCount, (unsigned long)s.underrunChunks);
        }
    }
    return sBootTargetActive ? GDX3DS_AUDIO_PRIME2_BOOT_TARGET_FRAMES
                             : GDX3DS_AUDIO_PRIME2_RUN_TARGET_FRAMES;
}

/* HLE producer thread only: brackets gdx_audio_produce_one_tick so an under edge can report
 * whether the producer was mid-tick and how long its ticks take (svcGetSystemTick, us). */
void gdx3ds_audio_producer_tick_begin(void) {
    sProdTickStartTick = svcGetSystemTick();
    sProdInTick = 1;
}

void gdx3ds_audio_producer_tick_end(void) {
    uint32_t us = TicksToUs(svcGetSystemTick() - sProdTickStartTick);
    sProdInTick = 0;
    sProdLastTickUs = us;
    if (us > sProdMaxTickUs) {
        sProdMaxTickUs = us;
    }
    sProdTickCount++;
}

/* Main thread: boot phase tag for the under-edge thread map (string literal, never freed). */
void gdx3ds_audio_set_main_phase(const char* tag) {
    sMainPhase = (tag != NULL) ? tag : "?";
}

/* main_3ds.cpp's preload ladder: first rung policy (2 = round-1 ladder, 1 = skip core 2,
 * 0 = appcore low priority only); always 2 with audioprime2 off. */
int gdx3ds_audio_prime2_preload_core(void) {
    return sPrime2Enabled ? sPreloadCorePolicy : 2;
}

/* Main thread: the core the preload ladder placed the worker on (-1 = synchronous). */
void gdx3ds_audio_note_preload_core(int core) {
    sPreloadCore = core;
}

/* Preload worker: running (1) between its first and last statement, else done (0). */
void gdx3ds_audio_note_preload_running(int running) {
    sPreloadRunning = running ? 1 : 0;
}

/* main_3ds.cpp's audioprime2 preload ladder: request the syscore share the way both audio
 * ladders do, recording the percent so the HOME-restore hook re-applies it. Returns 1 when
 * granted. */
int gdx3ds_audio_grant_syscore(void) {
    if (R_FAILED(APT_SetAppCpuTimeLimit(GDX3DS_AUDIO_SYSCORE_APP_PERCENT))) {
        return 0;
    }
    sSyscoreLimitPct = (int)GDX3DS_AUDIO_SYSCORE_APP_PERCENT;
    return 1;
}

/* Restore-side re-application (cause #5 in docs/research/home-crash-audit.md): the percent
 * APT_SetAppCpuTimeLimit was granted at init (0 if no core-1 rung was ever taken). */
int gdx3ds_audio_syscore_limit_percent(void) {
    return sSyscoreLimitPct;
}
