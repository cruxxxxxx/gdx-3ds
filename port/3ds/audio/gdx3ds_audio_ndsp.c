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
#include "gdx3ds_audio_crk.h"
#include "gdx3ds_audio_pcmdump.h"
#include "gdx3ds_crash.h"
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
extern int gGdxRaceActive; /* n64_gfx_bridge.cpp / decomp_port.c: a race-mode latch, set once --
                              same symbol gdx3ds_audio_pcmdump.c reads to gate its own capture
                              window. audio_stallsim's race-period repeat (below) uses it so a
                              [debug] pcm_dump_s capture (which only starts once this is set) can
                              actually contain a forced-contention event; see AUDIOFIX round 5. */

/* n60-3d12 [audiocost]: port/gdx_audio_hle_cost.c's per-op-class CPU/wall ledger, same module
 * [synx]/[synop] already use under debug.gputrace (port/3ds/gfx/gdx3ds_gpu_prof.c). Armed here
 * directly off diag_audio so a hardware run gets per-op-class cost telemetry WITHOUT paying for
 * gputrace's much wider GPU-side tracing. gdx3ds_prof_now (svcGetSystemTick, ~0.6 us/read on
 * hardware) is defined in gdx3ds_gpu_prof.c, always linked into the same G-Diffuser-3DS binary
 * as this file (port/3ds/CMakeLists.txt); extern-declared rather than pulling that header's
 * unrelated GPU-trace declarations in, same pattern as the config/race-latch externs above. */
extern long long gdx3ds_prof_now(void);
extern int gdx_hle_cost_armed; /* port/gdx_audio_hle_cost.h */
extern void gdx_hle_cost_arm(int on, long long (*now)(void), double tickHz);
extern void gdx_hle_cost_request_publish(unsigned long frameNo);
extern int gdx_hle_cost_format_audiocost(int index, char* out, size_t cap, double windowSeconds);
extern double gdx_hle_cost_published_span_seconds(void);

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
/* The HLE producer sits ONE RUNG BELOW the drain, and that is the [audiosched] fix.
 *
 * It used to be the drain's own 0x18, on the drain's own core. Two threads at the same
 * priority on one core do not preempt each other: the 3DS kernel runs the current one until
 * it blocks or yields. The drain's work is a 2 KB memcpy on a HARD 16 ms deadline (one
 * queued 512-frame chunk, see [audioprime]'s QUEUE cap); the producer's is unbounded
 * synthesis and loading, measured at 310 ms in ONE tick on the user's own console. Any
 * CPU-bound producer tick past ~16 ms therefore froze the drain for its whole duration, the
 * ndsp channel ran out of wave buffers, and NOTHING counted it -- sil/dry/part/under= are all
 * measured on the ring, which is full at the time. Deadline-monotonic ordering fixes it: the
 * short-deadline thread outranks the long one, so the drain always preempts. Lower number =
 * higher priority on the 3DS; 0x19 is still far above the 0x30 game and preload threads.
 *
 * [debug] audio_producer_prio = 24 (0x18) is the killswitch back to the old rung. */
#define GDX3DS_AUDIO_PRODUCER_PRIO (GDX3DS_AUDIO_DRAIN_PRIO + 1)
#define GDX3DS_AUDIO_PRIO_MIN 0x18 /* nothing an hbmenu app can raise past */
#define GDX3DS_AUDIO_PRIO_MAX 0x3F
#define GDX3DS_AUDIO_SYSCORE_APP_PERCENT 30u /* New 3DS default (it never actually takes this
                                                 rung -- core 2 is spare and tried first); also
                                                 the CIA exheader-safe fallback percent on a
                                                 refused request, see TakeSyscoreShare(). */
/* N60SYSCORE60 (audiofix-progress.md, hardware card n60-3d13): Old 3DS DEFAULT core-1 share.
 * A hardware A/B of the [audiocrk] silence-pad rate found 30% (the old default) far worse than
 * higher shares: 30%=44 ms/s padded, 40%=6.4 ms/s, 60%=3.6 ms/s, with no measured regression in
 * frame time at 60% vs 30% -- and the user reported 60% "worked best". Overridable by
 * [debug] syscore_pct like any other platform/value. */
#define GDX3DS_AUDIO_SYSCORE_PCT_O3DS_DEFAULT 60
#define GDX3DS_AUDIO_SYSCORE_PCT_MIN 5   /* APT's floor (the suspend drop uses it) */
#define GDX3DS_AUDIO_SYSCORE_PCT_MAX 89  /* Luma hbldr's .3dsx cap; a CIA is capped by its rsf MaxCpu */
/* AUDIOFIX: [debug] syscore_pct_safe (default 0 = old behaviour: a requested syscore_pct
 * above the ceiling below is honoured verbatim, clamped only to [5, 89] below, exactly as it
 * always was).
 * Hardware evidence this lever reacts to (audiofix-progress.md, card n60-3d7 variants T/S,
 * both `syscore_pct=80`, both reported "unplayable, essentially no music or sounds"):
 * APT_SetAppCpuTimeLimit(80) was GRANTED on that console (rc=0, readback=80%) -- not refused
 * by an exheader cap -- and raising the APP's own share of core 1 to 80% starves the 20% left
 * for the OS-side services the audio/loading pipeline depends on (FS/IPC, the DSP service
 * process, SD streaming): [core1] watch showed a 1086 ms single-loop max in the FIRST window
 * alone and [audiocrk] showed ~10 s of spliced silence over ~150-190 s (5-7% of the run).
 * N60SYSCORE60 (card n60-3d13) re-ran this A/B up through 60% and found it strictly BETTER than
 * the old 30% default (44 -> 3.6 ms/s of pad, frame time unaffected), which is why 60% is now the
 * Old 3DS DEFAULT above (GDX3DS_AUDIO_SYSCORE_PCT_O3DS_DEFAULT), not merely a safe ceiling. 80%
 * remains proven harmful, so this lever's ceiling stays AT the new default, not above it. New
 * 3DS was never implicated (it has the spare core 2 and never takes a core-1 rung at all unless
 * it is denied core 2), so the clamp below applies ONLY on Old 3DS and only when this lever is 1. */
#define GDX3DS_AUDIO_SYSCORE_PCT_O3DS_SAFE_MAX 60
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
 * conversion, first-touch sample reads) that Azahar's host CPU never reproduces. AUDIOFIX
 * round 5 (o3dscrackle hardware-PCM round) added a SECOND, recurring trigger, gated on
 * gGdxRaceActive: those 3 one-shots land during boot/menu, well before gGdxRaceActive goes
 * true, so a [debug] pcm_dump_s capture (which only starts staging once race-active) never
 * actually contained one -- confirmed empirically (an emulator capture with stallsim=200 and
 * pcm_dump_s=20 together showed 3 genuine [audiocrk] under-edges, all before t=3047 ms, and
 * an UNCHANGED pcm-main.wav discontinuity profile, because none of the 3 fell inside the
 * 20 s capture window). Every GDX3DS_AUDIO_STALLSIM_RACE_PERIOD pushes once race-active, the
 * producer stalls again, so a capture taken during racing can actually see (and prove a fix
 * against) a forced underrun. */
#define GDX3DS_AUDIO_STALLSIM_RACE_PERIOD 64u /* pushes between stalls once gGdxRaceActive */
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

/* AUDIOFIX round 4 continued (o3dscrackle hardware-PCM round, card n60-3d9): [debug]
 * audio_declick (default 0). FillAndSubmitSlot's underrun path has always memset the
 * unavailable tail of a partial chunk straight to 0 -- a hard splice, no crossfade, at BOTH
 * edges of every dropout: the real content's last sample is very rarely already at (or near)
 * zero, so the drop TO silence is itself a step, and the chunk that finally resumes real
 * content after one or more all-silence chunks jumps straight back from 0 to full amplitude,
 * a second step. [audiocrk]'s own `pad` counter (frames spliced in as silence) was already
 * nonzero on every hardware capture from this card (MAIN 124816 frames / 94 events over a
 * 215 s session, Q 104992/129 -- Q's own deeper ring raises latency without reducing underrun
 * count, matching the user's "worst on Q" report) -- a real, if far less frequent (~0.4-1
 * Hz) than the ~5 Hz headline from this round's hardware-PCM analysis, source of audible
 * clicks, since the OTHER ~5 Hz signal across every captured WAV (hw MAIN/Q/C/G and the
 * emulator) turned out to be entirely explained by [debug] pcm_dump_s's OWN diagnostic ring
 * silently dropping chunks under SD-write contention and splicing the two non-adjacent real
 * chunks on either side of the drop together with no gap at all (every one of the 103-107
 * discontinuities above |second difference| 8000 in every hardware WAV landed exactly on a
 * `chunk_index` gap in that WAV's own CSV sidecar; zero landed on a continuous boundary --
 * see gdx3ds_audio_pcmdump.c's own fix for this round). Default 0 (the historical hard splice,
 * byte-identical to every build before this round) on New 3DS; n60-3d12 makes it default ON on
 * Old 3DS (card n60-3d11 "ALL", hardware-confirmed: "audio is much better, crackle mostly
 * gone") -- `audio_declick = 0` in the ini still forces the old splice on either model. On:
 * FillAndSubmitSlot linear-ramps
 * the LAST (min(real, AUDIO_DECLICK_RAMP) samples of a partial chunk's real content down to 0
 * before the memset (fade-OUT into a dropout), and linear-ramps the FIRST
 * min(real, AUDIO_DECLICK_RAMP) samples of the chunk that ends a dropout up from 0 (fade-IN
 * out of it) -- the same declick idiom [FILTER]/reverb already use elsewhere in this port,
 * applied at the one splice point they do not cover. */
#define GDX3DS_AUDIO_DECLICK_RAMP_FRAMES 32u /* 1.0 ms at 32 kHz: short enough to stay inaudible
                                                 as its own event, long enough that the step it
                                                 replaces no longer reads as a transient */
static int sDeclickEnabled;            /* INI [debug] audio_declick */
static int sInDropout;                 /* drain-thread-only: true once a chunk padded any
                                           silence, cleared once real content resumes */

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
static int sSyscoreLever = -1; /* INI [debug] audio_syscore (1/0), -1 = not read yet */

/* O3DSHOME (docs/research/native/o3dshome-progress.md): [debug] audio_syscore (default 1)
 * lets the core ladders take the core-1 (syscore) rung. 0 = never call
 * APT_SetAppCpuTimeLimit and never place an audio/preload thread on core 1 -- the
 * configuration every New 3DS run has (`syscore=0%` in its restore receipt) and the one
 * whose HOME transition is known to work. It is the A/B lever for the Old 3DS HOME freeze
 * (C4j): the syscore grant is the one HOME-path difference between the two models that is
 * ours. Read lazily because the drain and producer ladders run at different boot points. */
/* O3DSHOME2 (docs/research/native/o3dshome2-progress.md): [debug] audio_ndsp (default 1).
 * 0 = never call ndspInit, i.e. the null sink with NO DSP component loaded -- the same state
 * as a missing sdmc:/3ds/dspfirm.cdc, without asking the user to rename files. With no
 * component, libctru's aptDspSleep()/aptDspWakeup() return at once (dsp.c: dspComponentLoaded
 * is false), so a HOME press hands the HOME menu a DSP we never touched. The A/B lever for
 * "our DSP state wedges the HOME menu" (NEXT.md C4j). */
static const Result kNdspDisabledByLever = (Result)0xC0DE0D50u;

static int SyscoreRungAllowed(void) {
    if (sSyscoreLever < 0) {
        sSyscoreLever = gdx3ds_config_get_bool("debug", "audio_syscore", 1) ? 1 : 0;
        if (!sSyscoreLever) {
            AudioOutLogf("[audio-out] syscore rung disabled (debug.audio_syscore=0): no "
                         "APT_SetAppCpuTimeLimit, audio threads stay off core 1");
        }
    }
    return sSyscoreLever;
}

/* N60CORE1: [debug] syscore_pct (default: the per-model constant below -- 30 on New 3DS, byte
 * for byte the historical value; 60 on Old 3DS, see N60SYSCORE60). The app's core-1 share for
 * the whole run, clamped to [5, 89]. A CIA is capped by its exheader (rsf `MaxCpu` & 0x7F; ours
 * is 0x9E = 30 unless built with -DGDX_3DS_CIA_MAXCPU), so a refused request falls back to 30
 * and says so once (that exheader-safe fallback is unaffected by the Old 3DS default change --
 * it is the CIA's own hardware cap, not this lever's default). The value granted is what the
 * HOME restore re-applies (sSyscoreLimitPct), so a raised share survives suspend/resume like
 * the default one. */
static int sSyscoreRequestPct = -1;
static int sSyscoreReceiptDone = 0;

/* The default `[debug] syscore_pct` value for the running model, before any ini override or
 * the syscore_pct_safe clamp. Exported (gdx3ds_audio_syscore_default_pct) so other [core1]
 * log sites (gdx3ds_core1probe.c's watch arm-check) don't have to duplicate this literal. */
static int SyscoreDefaultPct(void) {
    return gdx3ds_is_old3ds() ? GDX3DS_AUDIO_SYSCORE_PCT_O3DS_DEFAULT
                               : (int)GDX3DS_AUDIO_SYSCORE_APP_PERCENT;
}

static int SyscoreRequestPct(void) {
    if (sSyscoreRequestPct < 0) {
        int pct = gdx3ds_config_get_int("debug", "syscore_pct", SyscoreDefaultPct());
        if (pct < GDX3DS_AUDIO_SYSCORE_PCT_MIN) {
            pct = GDX3DS_AUDIO_SYSCORE_PCT_MIN;
        } else if (pct > GDX3DS_AUDIO_SYSCORE_PCT_MAX) {
            pct = GDX3DS_AUDIO_SYSCORE_PCT_MAX;
        }
        /* AUDIOFIX: default 0 keeps the line above as the whole story (old behaviour, byte
         * for byte). 1 additionally clamps to the OS's own 60% Old-3DS ceiling -- see the
         * macro's comment for the hardware evidence that anything higher (80%) starves the
         * services the audio/loading pipeline itself depends on. */
        if (gdx3ds_config_get_bool("debug", "syscore_pct_safe", 0) && gdx3ds_is_old3ds() &&
            pct > GDX3DS_AUDIO_SYSCORE_PCT_O3DS_SAFE_MAX) {
            AudioOutLogf("[core1] syscore_pct_safe=1: requested %d clamped to %d on Old 3DS",
                         pct, GDX3DS_AUDIO_SYSCORE_PCT_O3DS_SAFE_MAX);
            pct = GDX3DS_AUDIO_SYSCORE_PCT_O3DS_SAFE_MAX;
        }
        sSyscoreRequestPct = pct;
    }
    return sSyscoreRequestPct;
}

/* Public accessor: this model's default [debug] syscore_pct, before any ini override. */
int gdx3ds_audio_syscore_default_pct(void) {
    return SyscoreDefaultPct();
}

static void SyscoreReceiptOnce(int requested, Result rc, int granted) {
    if (sSyscoreReceiptDone || requested == (int)GDX3DS_AUDIO_SYSCORE_APP_PERCENT) {
        return;
    }
    sSyscoreReceiptDone = 1;
    u32 got = 0;
    APT_GetAppCpuTimeLimit(&got);
    AudioOutLogf("[core1] syscore_pct=%d rc=0x%08lX granted=%d%% readback=%lu%%%s", requested,
                 (unsigned long)rc, granted, (unsigned long)got,
                 granted == requested ? "" : " (REFUSED above the exheader cap -> fell back to 30)");
}

/* Take the app's syscore share for a core-1 rung: 1 when granted (and recorded so the
 * ONRESTORE/ONWAKEUP hook re-applies it), 0 when the lever forbids it or APT refuses. */
static int TakeSyscoreShare(void) {
    if (!SyscoreRungAllowed()) {
        return 0;
    }
    const int requested = SyscoreRequestPct();
    const Result rc = APT_SetAppCpuTimeLimit((u32)requested);
    if (R_SUCCEEDED(rc)) {
        sSyscoreLimitPct = requested;
        SyscoreReceiptOnce(requested, rc, requested);
        return 1;
    }
    if (requested == (int)GDX3DS_AUDIO_SYSCORE_APP_PERCENT ||
        R_FAILED(APT_SetAppCpuTimeLimit(GDX3DS_AUDIO_SYSCORE_APP_PERCENT))) {
        return 0;
    }
    sSyscoreLimitPct = (int)GDX3DS_AUDIO_SYSCORE_APP_PERCENT;
    SyscoreReceiptOnce(requested, rc, sSyscoreLimitPct);
    return 1;
}

/* N60CORE1: [debug] ndsp_core1 (default 0). libctru's ndspInit creates its DSP service
 * thread (ndspThreadMain: prio 0x18, core -2 = the process default = core 0) and gives no
 * way to choose the core. That thread wakes on every DSP frame IRQ (160 samples, ~4.9 ms,
 * ~3.4 times per 16.7 ms game frame), runs 24 channel status reads + updates against DSP
 * shared memory, and PREEMPTS the game on core 0 each time (0x18 vs main 0x30), evicting
 * part of the game's L1-I working set. 1 = create it on core 1 (with the drain 0x18 and the
 * producer 0x19 already there) through the --wrap=threadCreate redirect: Old 3DS only,
 * only when the syscore share is granted first (a core-1 create needs it), and a refused
 * core-1 create falls back to the default core inside the wrap, so ndspInit cannot fail
 * because of the lever. Audio-only: no game state is touched, so [gstate] is unchanged by
 * construction. Receipt `[core1] ndsp_core1=...`. */
static int sNdspCoreReq = 0;
static int sNdspCreatedCore = -1;
static int sNdspFellBack = 0;

static int NdspCoreRequest(void) {
    const int lever = gdx3ds_config_get_int("debug", "ndsp_core1", 0);
    if (lever == 0) {
        return -1;
    }
    if (!gdx3ds_is_old3ds()) {
        AudioOutLogf("[core1] ndsp_core1=%d REFUSED: New 3DS (core 1 has no app share there; "
                     "audio is on core 2) -> ndsp thread on the default core", lever);
        return -1;
    }
    if (!TakeSyscoreShare()) {
        AudioOutLogf("[core1] ndsp_core1=%d REFUSED: no syscore share (audio_syscore=0 or APT "
                     "refused) -> ndsp thread on the default core", lever);
        return -1;
    }
    return 1;
}

static Result StartNdspUnlessDisabled(void) {
    if (!gdx3ds_config_get_bool("debug", "audio_ndsp", 1)) {
        return kNdspDisabledByLever;
    }
    const int core = NdspCoreRequest();
    if (core < 0) {
        return ndspInit();
    }
    sNdspCoreReq = core;
    gdx3ds_thread_redirect_begin(core);
    const Result rc = ndspInit();
    sNdspCreatedCore = gdx3ds_thread_redirect_end(&sNdspFellBack);
    int ranOn = -1;
    for (int i = 0; i < 20 && ranOn < 0 && R_SUCCEEDED(rc); i++) {
        svcSleepThread(1000000LL); /* the thread's first instruction records its core */
        ranOn = gdx3ds_thread_redirect_ran_on();
    }
    AudioOutLogf("[core1] ndsp_core1=1 ndspInit rc=0x%08lX thread created on core %d ran on core %d "
                 "(requested %d, fallback=%d) syscore=%d%%",
                 (unsigned long)rc, sNdspCreatedCore, ranOn, core, sNdspFellBack, sSyscoreLimitPct);
    return rc;
}

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
    __dmb(); /* HOMEFIX2: the ack must be visible before we block -- plain volatile stores are
              * not ordered against the suspender's polling load on ARM11 MPCore (the Dekker
              * store-buffer shape the 2026-09-23 audit flagged). */
    while (sSuspended && !s.stopRequested) {
        LightEvent_Wait(&sDrainParkEvent);
    }
    sDrainParked = 0;
    __dmb();
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

/* ---- [audioring] ring-occupancy census + producer stall map (audioring track) -------------
 * WHY THIS EXISTS. `covfinish` refused an audio census family because a SHOT is a framebuffer
 * hash and cannot see audio; `audiodiet` proved byte-equality of the driver's READS but stated
 * plainly that nothing ever listened and that the audio thread's read multiset is not
 * deterministic even under det=1. So no frame-hash gate can ever cover "does it crackle".
 * This is the substitute: crackle as a NUMBER, taken on the two threads that own the problem.
 *
 * A splice is silence the drain spliced into the stream because the ring was short when a wave
 * slot came free. Two of them were previously invisible:
 *   - `s.underrunChunks` counts only PARTIAL chunks (0 < real < 512). A chunk that found the
 *     ring EMPTY (real == 0) was deliberately not counted, on the reasoning that pure silence
 *     "is the steady state whenever production is legitimately idle". In THIS port that
 *     reasoning does not hold: AudioThread_CreateTaskImpl calls osAiSetNextBuffer with a full
 *     AI buffer on EVERY tick whatever the content is, so the producer never stops pushing
 *     while the game runs. A dry chunk is therefore an underrun, and the worst kind -- 16 ms
 *     of hard silence, not a tail. `under=` undercounts the damage; `dry=` is the rest of it.
 *   - Nothing recorded the DISTRIBUTION of ring occupancy, only `rmin` since the last receipt,
 *     which a single dip pins to 0 and then says nothing about how often.
 *
 * WHAT IT ANSWERS, and it is the discrimination this track was asked for: the producer's own
 * histograms separate "the producer was not scheduled" (gap between ticks) from "the producer
 * was stuck inside one tick" (tick duration). A 3DS EK build's producer tick can perform a
 * synchronous 64DD read -- AudioLoad_SyncLoadSample / AudioLoad_RelocateFontAndPreloadSamples
 * / AudioLoad_PreloadSamplesForFont all call AudioLoad_SyncDiskDrive, which blocks on a whole
 * LBA through gdx_disk_stream, on this thread, inside sAudioCtxMutex. That shows up in
 * tick_us; CPU starvation shows up in gap_us. They cannot be confused.
 *
 * COST WHEN OFF. Every hook early-returns on one int compare. No allocation, no lock: the
 * counters are the port's benign aligned-int cross-thread diag idiom, same as [audio-out]'s.
 * [debug] audioring = 0 (off) | 1 (periodic + FINAL) | 2 (+ per-receipt reset of the
 * histograms so each line is a window rather than a running total).
 * [debug] audioring_report = drain polls between receipts (default 2500 ~= 5 s; ekdrive's
 * lesson -- a cadence you cannot lower can hide the thing you are measuring). */
#define GDX3DS_AUDIO_RC_BUCKETS 8u
/* One 512-frame chunk at 32 kHz. With [audioprime]'s QUEUE cap of 2, this is the WHOLE
 * runway the drain has between a slot going DONE and the ndsp channel having nothing
 * left to play, so a poll interval at or above it is a missed refill opportunity. */
#define GDX3DS_AUDIO_RC_POLL_DEADLINE_US 16000u
#define GDX3DS_AUDIO_RC_PUSH_GRACE 64u /* chunks (~1 s) after the last push that still count as
                                        * "the producer is live", so boot/prime/suspend quiet
                                        * is not booked as starvation */
static int sRcEnabled;                 /* INI [debug] audioring */
static uint32_t sRcReportPolls;        /* INI [debug] audioring_report */
/* drain side */
static uint32_t sRcChunks;             /* chunks submitted since the census armed */
static uint32_t sRcSilFrames;          /* padding frames spliced into those chunks */
static uint32_t sRcDry;                /* chunks that found the ring EMPTY */
static uint32_t sRcPart;               /* chunks that found 0 < ring < 512 */
static uint32_t sRcStarveChunks;       /* short chunks with the producer live (see grace) */
static uint32_t sRcStarveFrames;       /* padding frames in those */
static uint32_t sRcDryRun;             /* current consecutive-dry run */
static uint32_t sRcDryRunMax;          /* longest consecutive-dry run (= worst single gap) */
static uint32_t sRcOcc[GDX3DS_AUDIO_RC_BUCKETS]; /* ring level at each pull */
static uint32_t sRcRingMax;
static uint32_t sRcChunksSincePush;
/* producer side */
static uint32_t sRcWakes;              /* producer loop iterations */
static uint32_t sRcWakesIdle;          /* wakes that produced no tick (already at target) */
static uint32_t sRcWakesCapped;        /* wakes that left on kMaxTicksPerWake, still short */
static uint32_t sRcTicks;
static uint32_t sRcTicksThisWake;
static uint32_t sRcTickUs[GDX3DS_AUDIO_RC_BUCKETS];
static uint32_t sRcGapUs[GDX3DS_AUDIO_RC_BUCKETS];
static uint32_t sRcBufAtWake[GDX3DS_AUDIO_RC_BUCKETS];
/* CONTENT, not just cadence. `audiodiet` 6.6 and `audioring` 6 both end on the same
 * sentence: nothing in this project has ever listened. A sample-exact hash cannot close that
 * here -- audioring measured the audio thread's own read multiset as non-deterministic under
 * det=1, so two identical runs would not agree and the check would be worse than useless.
 * What IS stable run to run, and what a broken sample load would visibly move, is the LEVEL:
 * the mean absolute sample value over every REAL (non-padded) frame the drain submitted, and
 * the session peak. Silence reads 0/0; a load that fell back to garbage reads far higher; the
 * same music at the same volume reads the same to within a few percent. It is a weaker claim
 * than a frame hash and it is stated as one -- but it is a MEASUREMENT, where before there was
 * an inference. Costs one pass over the chunk, only while [debug] audioring is on. */
static uint64_t sRcMagSum;             /* sum of |sample| over real frames, both channels */
static uint32_t sRcMagFrames;          /* real frames counted into sRcMagSum */
static uint32_t sRcPeak;               /* session max |sample| */
static uint32_t sRcTickUsMax;
static uint32_t sRcGapUsMax;
static uint64_t sRcLastTickEndTick;
/* SCHEDULING, the axis `gap_ms` does NOT measure (audiosched track).
 *
 * `gap_ms` is tick-END to tick-START, and the producer DELIBERATELY idles whenever
 * buffered() >= target, so in steady state a gap is exactly the audio cadence -- one
 * 528-frame tick's worth of drain, ~16.5 ms -- and says nothing about whether the thread
 * could have run. It cannot distinguish "descheduled" from "nothing to do", and reading its
 * 8-16 ms mode as starvation is the mistake this track was opened on. The honest scheduling
 * axis is WAKE-to-WAKE: AudioThreadMain's loop is a 5 ms timed condvar wait, so the interval
 * between consecutive gdx3ds_audio_producer_wake() calls is ~5 ms whenever the thread gets
 * the core, WHATEVER it decides to do with it. A long wake interval IS starvation.
 *
 * And the deadline nobody had measured is the DRAIN's, not the producer's. [audioprime]'s
 * QUEUE cap keeps at most 2 wave buffers in flight, so from the moment one goes DONE there
 * is exactly ONE 512-frame chunk = 16 ms of audio left queued on the DSP. The drain has to
 * retire and refill inside that window or the ndsp channel runs out of buffers entirely --
 * a hard discontinuity that `sil` / `dry` / `part` / `under=` CANNOT see, because they are
 * all measured on the RING and the ring may well be full at the time. The drain runs at the
 * same priority (0x18) on the same core (2) as the producer, whose ticks the user's own
 * console log measured at up to 310 ms. `poll_ms`, `late`, `q[]` and `dspdry` are that hop. */
static uint32_t sRcPolls;              /* drain loop iterations */
static uint32_t sRcPollUs[GDX3DS_AUDIO_RC_BUCKETS]; /* poll-to-poll interval (nominal 2 ms) */
static uint32_t sRcPollUsMax;
static uint32_t sRcPollLate;           /* polls whose interval reached one 16 ms chunk period:
                                        * a refill opportunity the DSP queue could not afford */
static uint64_t sRcLastPollTick;       /* 0 = interval suppressed (first poll / after a park) */
static uint32_t sRcQueued[GDX3DS_AUDIO_NUM_WAVEBUFS + 1u]; /* wave bufs QUEUED|PLAYING at the
                                        * top of a poll: q[0] is the DSP running dry */
static uint32_t sRcDspDry;             /* polls that found ZERO queued */
static uint32_t sRcDspDryUs;           /* interval accumulated across those polls */
static uint32_t sRcWakeUs[GDX3DS_AUDIO_RC_BUCKETS]; /* producer wake-to-wake (nominal 5 ms) */
static uint32_t sRcWakeUsMax;
static uint64_t sRcLastWakeTick;       /* 0 = interval suppressed (first wake / after a park) */
/* Emulator stand-in for a CPU-bound hardware tick, so the scheduling question above can be
 * asked without a console: the producer BUSY-holds the core (no syscall, so no yield) inside
 * its tick bracket. Default 0 = not compiled out, just never entered. */
static int sProducerPrio = GDX3DS_AUDIO_PRODUCER_PRIO; /* INI [debug] audio_producer_prio */
static int sProdHoldMs;                /* INI [debug] audio_prodhold_ms */
static uint32_t sProdHoldEvery;        /* INI [debug] audio_prodhold_every (ticks) */
/* levers. audio_ring_frames always defaults 0 = unchanged pacing. audio_target_frames
 * defaults 0 (unchanged) on New 3DS but 2560 on Old 3DS as of n60-3d12 -- see the longer
 * comment on gdx3ds_audio_init's sRcTargetOverride assignment and the [audiocrk] block below. */
static int32_t sRcTargetOverride;      /* INI [debug] audio_target_frames */
static uint32_t sRcRingOverride;       /* INI [debug] audio_ring_frames */

/* ---- [audiocrk] + the o3dscrackle levers (docs/research/native/o3dscrackle-progress.md) ----
 * [debug] audiocrk = 0 (off: every hook is one int compare) | 1 (a window line pair every
 *   audiocrk_ms when the window holds anything audible-suspect, plus a heartbeat pair and a
 *   running total every audiocrk_every windows) | 2 (every window).
 * [debug] audiocrk_ms (1000), audiocrk_every (10), audiocrk_step (8192 = the |delta| that
 *   counts as a step). The census itself is gdx3ds_audio_crk.c (pure, host-tested).
 * LEVERS. ndsp_clip/ndsp_gain_pct default 0 = the shipped path byte for byte on every model.
 * audio_queue/audio_target_frames (below) default to the n60-3d11 "ALL" hardware-confirmed
 * settings ON THE OLD 3DS ONLY (`gdx3ds_is_old3ds()`); the New 3DS default stays 0/off
 * unchanged. An explicit `= 0` in the ini still turns either one off on EITHER model -- the
 * ini key always wins over the model default, same pattern as audio_simd/audio_ctx_narrow.
 * [debug] ndsp_clip = 1     -> ndspSetClippingMode(NDSP_CLIP_NORMAL) instead of the libctru
 *   default SOFT. Our one channel carries s16 at mix 1.0 into a stereo bus, so a HARD clipper
 *   is the identity on it; whatever SOFT does near the rails is the only thing that changes.
 * [debug] ndsp_gain_pct = N -> channel mix N/100 (1..100) instead of 1.0: headroom A/B for
 *   a level-dependent stage after the ring (DSP clipper, codec, speaker). PCM is untouched.
 * [debug] audio_queue = 3   -> three wave buffers in flight instead of [audioprime]'s two:
 *   the DSP-side runway after a slot goes DONE grows 16 -> 32 ms. Pair it with
 *   audio_target_frames = 2560 to keep the ring's own cushion where it was. Old 3DS default
 *   is now 3 (card n60-3d11 "ALL" variant, user-confirmed on hardware: "audio is much better,
 *   crackle mostly gone"); set `audio_queue = 0` to go back to the pre-n60-3d12 two-buffer path. */
static int sCrkEnabled;                /* INI [debug] audiocrk */
static uint32_t sCrkWindowUs;          /* INI [debug] audiocrk_ms, in us */
static uint32_t sCrkEvery;             /* INI [debug] audiocrk_every */
static uint32_t sCrkWindows;
static uint64_t sCrkWindowStartTick;
static uint64_t sCrkLastPollTick;      /* 0 = interval suppressed (first poll / after a park) */
static GdxCrk sCrk;
#define GDX3DS_AUDIO_CRK_DETAIL_MAX 16u
static uint32_t sCrkPushDetail;        /* pstep detail lines printed */
static int sNdspClipLever;             /* INI [debug] ndsp_clip */
static int sNdspGainPct;               /* INI [debug] ndsp_gain_pct (0 = 100) */
static uint32_t sQueueCap;             /* wave buffers allowed in flight under audioprime */

static uint32_t RcOccBucket(uint32_t frames) {
    if (frames == 0u) {
        return 0u;
    }
    if (frames < 128u) {
        return 1u;
    }
    if (frames < 256u) {
        return 2u;
    }
    if (frames < 512u) {
        return 3u;
    }
    if (frames < 1024u) {
        return 4u;
    }
    if (frames < 2048u) {
        return 5u;
    }
    if (frames < 4096u) {
        return 6u;
    }
    return 7u;
}

/* 1 ms, 2, 4, 8, 16, 32, 64, >=64 -- one octave per bucket from the 2 ms drain poll up past
 * the 64 ms the two queued chunks buy. */
static uint32_t RcUsBucket(uint32_t us) {
    uint32_t b = 0u;
    uint32_t lim = 1000u;
    while (b < GDX3DS_AUDIO_RC_BUCKETS - 1u && us >= lim) {
        lim <<= 1;
        b++;
    }
    return b;
}

static void RcResetWindow(void) {
    uint32_t i;
    sRcChunks = 0u;
    sRcSilFrames = 0u;
    sRcDry = 0u;
    sRcPart = 0u;
    sRcStarveChunks = 0u;
    sRcStarveFrames = 0u;
    sRcDryRunMax = 0u;
    sRcRingMax = 0u;
    sRcWakes = 0u;
    sRcWakesIdle = 0u;
    sRcWakesCapped = 0u;
    sRcTicks = 0u;
    sRcTickUsMax = 0u;
    sRcGapUsMax = 0u;
    sRcMagSum = 0u;
    sRcMagFrames = 0u;
    sRcPeak = 0u;
    sRcPolls = 0u;
    sRcPollUsMax = 0u;
    sRcPollLate = 0u;
    sRcDspDry = 0u;
    sRcDspDryUs = 0u;
    sRcWakeUsMax = 0u;
    for (i = 0u; i < GDX3DS_AUDIO_RC_BUCKETS; i++) {
        sRcOcc[i] = 0u;
        sRcTickUs[i] = 0u;
        sRcGapUs[i] = 0u;
        sRcBufAtWake[i] = 0u;
        sRcPollUs[i] = 0u;
        sRcWakeUs[i] = 0u;
    }
    for (i = 0u; i <= GDX3DS_AUDIO_NUM_WAVEBUFS; i++) {
        sRcQueued[i] = 0u;
    }
}

/* Drain thread only, once per loop iteration, BEFORE the retire/refill sweep -- so `queued`
 * is what the DSP had left to play when this poll began, not what the poll left behind.
 * `sRcLastPollTick == 0` suppresses one interval (first poll, and after a HOME/sleep park,
 * where the gap is policy rather than scheduling). Off = one int compare. */
static void RcNotePoll(void) {
    uint64_t now;
    uint32_t queued = 0u;
    uint32_t i;
    if (!sRcEnabled) {
        return;
    }
    now = svcGetSystemTick();
    sRcPolls++;
    for (i = 0u; i < GDX3DS_AUDIO_NUM_WAVEBUFS; i++) {
        u8 st = s.slots[i].waveBuf.status;
        if (st == NDSP_WBUF_QUEUED || st == NDSP_WBUF_PLAYING) {
            queued++;
        }
    }
    sRcQueued[queued]++;
    if (sRcLastPollTick != 0u) {
        uint32_t us = TicksToUs(now - sRcLastPollTick);
        sRcPollUs[RcUsBucket(us)]++;
        if (us > sRcPollUsMax) {
            sRcPollUsMax = us;
        }
        if (us >= GDX3DS_AUDIO_RC_POLL_DEADLINE_US) {
            sRcPollLate++;
        }
        if (queued == 0u) {
            sRcDspDryUs += us;
        }
    }
    if (queued == 0u) {
        sRcDspDry++;
    }
    sRcLastPollTick = now;
}

/* Drain thread only, once per submitted chunk. `ringBefore` is the ring level the pull saw
 * (count after the pull plus what it took); the unlocked re-read is the benign diag race. */
static void RcNoteChunk(uint32_t ringBefore, uint32_t real) {
    uint32_t pad;
    if (!sRcEnabled) {
        return;
    }
    sRcChunks++;
    sRcChunksSincePush++;
    sRcOcc[RcOccBucket(ringBefore)]++;
    if (ringBefore > sRcRingMax) {
        sRcRingMax = ringBefore;
    }
    if (real >= GDX3DS_AUDIO_CHUNK_FRAMES) {
        sRcDryRun = 0u;
        return;
    }
    pad = GDX3DS_AUDIO_CHUNK_FRAMES - real;
    sRcSilFrames += pad;
    if (real == 0u) {
        sRcDry++;
        sRcDryRun++;
        if (sRcDryRun > sRcDryRunMax) {
            sRcDryRunMax = sRcDryRun;
        }
    } else {
        sRcPart++;
        sRcDryRun = 0u;
    }
    if (sRcChunksSincePush <= GDX3DS_AUDIO_RC_PUSH_GRACE) {
        sRcStarveChunks++;
        sRcStarveFrames += pad;
    }
}

/* Drain thread (periodic) and main thread (FINAL, after both audio threads have joined).
 * `sil_ppk` is the headline: spliced silence frames per 1000 submitted frames. It is the
 * closest thing this port has to a measurement of "crackle", and unlike `under=` it counts
 * the dry chunks too. */
/* The EFFECTIVE producer target, with no side effects: gdx3ds_audio_producer_target_frames
 * also RELEASES the boot target and logs a one-shot when it does, and that release is the
 * producer thread's to make -- a receipt printed from the drain thread must never move it.
 * The clamp is shared with that function so the two can never disagree. */
static int32_t RcClampTarget(int32_t want) {
    int32_t cap = (int32_t)s.capacityFrames - 1024; /* target + one 528-frame tick, with room */
    if (cap < GDX3DS_AUDIO_PRIME2_RUN_TARGET_FRAMES) {
        cap = GDX3DS_AUDIO_PRIME2_RUN_TARGET_FRAMES;
    }
    return (want > cap) ? cap : want;
}

static int32_t RcEffectiveTarget(void) {
    if (sRcTargetOverride > 0 && s.initialized) {
        return RcClampTarget(sRcTargetOverride);
    }
    return sBootTargetActive ? GDX3DS_AUDIO_PRIME2_BOOT_TARGET_FRAMES
                             : GDX3DS_AUDIO_PRIME2_RUN_TARGET_FRAMES;
}

static void LogRingCensus(const char* tag) {
    uint32_t frames;
    uint32_t silPpk;
    uint32_t dryPct;
    if (!sRcEnabled) {
        return;
    }
    frames = sRcChunks * GDX3DS_AUDIO_CHUNK_FRAMES;
    silPpk = (frames != 0u) ? (uint32_t)(((uint64_t)sRcSilFrames * 1000ull) / frames) : 0u;
    dryPct = (sRcChunks != 0u) ? ((sRcDry * 100u) / sRcChunks) : 0u;
    AudioOutLogf("[audioring] %s t=%lums chunks=%lu sil=%lu/%lu sil_ppk=%lu dry=%lu(%lu%%) "
                 "part=%lu dryrun=%lu starve=%lu/%lu drop=%lu ringmax=%lu target=%d",
                 tag, (unsigned long)MsSinceInit(), (unsigned long)sRcChunks,
                 (unsigned long)sRcSilFrames, (unsigned long)frames, (unsigned long)silPpk,
                 (unsigned long)sRcDry, (unsigned long)dryPct, (unsigned long)sRcPart,
                 (unsigned long)sRcDryRunMax, (unsigned long)sRcStarveChunks,
                 (unsigned long)sRcStarveFrames, (unsigned long)s.droppedFrames,
                 (unsigned long)sRcRingMax,
                 /* the EFFECTIVE target, override included -- [audioprime2]'s own lines print
                  * the unoverridden constant and would disagree with the ring if quoted here */
                 (int)RcEffectiveTarget());
    AudioOutLogf("[audioring] %s level mag=%lu peak=%lu magframes=%lu",
                 tag,
                 (unsigned long)(sRcMagFrames != 0u
                                     ? (uint32_t)(sRcMagSum /
                                                  ((uint64_t)sRcMagFrames * GDX3DS_AUDIO_CHANNELS))
                                     : 0u),
                 (unsigned long)sRcPeak, (unsigned long)sRcMagFrames);
    AudioOutLogf("[audioring] %s occ[0|<128|<256|<512|<1k|<2k|<4k|4k+]=%lu/%lu/%lu/%lu/%lu/%lu/%lu/%lu",
                 tag, (unsigned long)sRcOcc[0], (unsigned long)sRcOcc[1],
                 (unsigned long)sRcOcc[2], (unsigned long)sRcOcc[3], (unsigned long)sRcOcc[4],
                 (unsigned long)sRcOcc[5], (unsigned long)sRcOcc[6], (unsigned long)sRcOcc[7]);
    AudioOutLogf("[audioring] %s prod wakes=%lu idle=%lu capped=%lu ticks=%lu "
                 "tick_ms[<1|<2|<4|<8|<16|<32|<64|64+]=%lu/%lu/%lu/%lu/%lu/%lu/%lu/%lu max=%luus",
                 tag, (unsigned long)sRcWakes, (unsigned long)sRcWakesIdle,
                 (unsigned long)sRcWakesCapped, (unsigned long)sRcTicks,
                 (unsigned long)sRcTickUs[0], (unsigned long)sRcTickUs[1],
                 (unsigned long)sRcTickUs[2], (unsigned long)sRcTickUs[3],
                 (unsigned long)sRcTickUs[4], (unsigned long)sRcTickUs[5],
                 (unsigned long)sRcTickUs[6], (unsigned long)sRcTickUs[7],
                 (unsigned long)sRcTickUsMax);
    AudioOutLogf("[audioring] %s prod gap_ms[<1|<2|<4|<8|<16|<32|<64|64+]=%lu/%lu/%lu/%lu/%lu/%lu/%lu/%lu "
                 "max=%luus buf[0|<128|<256|<512|<1k|<2k|<4k|4k+]=%lu/%lu/%lu/%lu/%lu/%lu/%lu/%lu",
                 tag, (unsigned long)sRcGapUs[0], (unsigned long)sRcGapUs[1],
                 (unsigned long)sRcGapUs[2], (unsigned long)sRcGapUs[3],
                 (unsigned long)sRcGapUs[4], (unsigned long)sRcGapUs[5],
                 (unsigned long)sRcGapUs[6], (unsigned long)sRcGapUs[7],
                 (unsigned long)sRcGapUsMax, (unsigned long)sRcBufAtWake[0],
                 (unsigned long)sRcBufAtWake[1], (unsigned long)sRcBufAtWake[2],
                 (unsigned long)sRcBufAtWake[3], (unsigned long)sRcBufAtWake[4],
                 (unsigned long)sRcBufAtWake[5], (unsigned long)sRcBufAtWake[6],
                 (unsigned long)sRcBufAtWake[7]);
    /* SCHEDULING. `wake_ms` is the producer's real "did I get the core" axis (`gap_ms` is
     * not -- see the sRcWakeUs comment). `poll_ms` / `late` / `dspdry` are the drain's, and
     * the DSP hop they cover is the only part of the chain the ring counters cannot see. */
    AudioOutLogf("[audioring] %s prod wake_ms[<1|<2|<4|<8|<16|<32|<64|64+]="
                 "%lu/%lu/%lu/%lu/%lu/%lu/%lu/%lu max=%luus",
                 tag, (unsigned long)sRcWakeUs[0], (unsigned long)sRcWakeUs[1],
                 (unsigned long)sRcWakeUs[2], (unsigned long)sRcWakeUs[3],
                 (unsigned long)sRcWakeUs[4], (unsigned long)sRcWakeUs[5],
                 (unsigned long)sRcWakeUs[6], (unsigned long)sRcWakeUs[7],
                 (unsigned long)sRcWakeUsMax);
    AudioOutLogf("[audioring] %s drain polls=%lu late=%lu dspdry=%lu/%luus q[0|1|2|3]="
                 "%lu/%lu/%lu/%lu poll_ms[<1|<2|<4|<8|<16|<32|<64|64+]="
                 "%lu/%lu/%lu/%lu/%lu/%lu/%lu/%lu max=%luus prio=0x%02X/0x%02X cores=%d/%d",
                 tag, (unsigned long)sRcPolls, (unsigned long)sRcPollLate,
                 (unsigned long)sRcDspDry, (unsigned long)sRcDspDryUs,
                 (unsigned long)sRcQueued[0], (unsigned long)sRcQueued[1],
                 (unsigned long)sRcQueued[2], (unsigned long)sRcQueued[3],
                 (unsigned long)sRcPollUs[0], (unsigned long)sRcPollUs[1],
                 (unsigned long)sRcPollUs[2], (unsigned long)sRcPollUs[3],
                 (unsigned long)sRcPollUs[4], (unsigned long)sRcPollUs[5],
                 (unsigned long)sRcPollUs[6], (unsigned long)sRcPollUs[7],
                 (unsigned long)sRcPollUsMax, (unsigned)sProducerPrio,
                 (unsigned)GDX3DS_AUDIO_DRAIN_PRIO,
                 gdx3ds_audio_producer_core(), s.drainCore);
    if (sRcEnabled >= 2) {
        RcResetWindow(); /* level 2: each line is a window, not a running total */
    }
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

/* ---- [audiocrk] hooks (drain unless stated). Off = one int compare each. ---------------- */
static void CrkNoteChunk(const int16_t* pcm, uint32_t real) {
    if (!sCrkEnabled) {
        return;
    }
    /* Unlocked countFrames re-read: the benign aligned-int diag race used throughout. */
    gdx_crk_note_chunk(&sCrk, pcm, real, GDX3DS_AUDIO_CHUNK_FRAMES, s.countFrames + real);
}

static void CrkLogWindow(const char* tag, const GdxCrkWindow* w) {
    char line[256];
    gdx_crk_format_sched(w, tag, (unsigned long)MsSinceInit(), line, sizeof(line));
    AudioOutLogf("%s", line);
    gdx_crk_format_content(w, tag, line, sizeof(line));
    AudioOutLogf("%s", line);
    gdx_crk_format_mean(w, tag, line, sizeof(line));
    AudioOutLogf("%s", line);
}

/* Top of every drain poll, BEFORE the sweep (same contract as RcNotePoll). Rolls the window
 * over on the drain's own clock and prints per the [debug] audiocrk level. */
static void CrkNotePoll(void) {
    uint64_t now;
    uint32_t queued = 0u;
    uint32_t intervalUs = 0u;
    if (!sCrkEnabled) {
        return;
    }
    now = svcGetSystemTick();
    for (uint32_t i = 0; i < GDX3DS_AUDIO_NUM_WAVEBUFS; i++) {
        u8 st = s.slots[i].waveBuf.status;
        if (st == NDSP_WBUF_QUEUED || st == NDSP_WBUF_PLAYING) {
            queued++;
        }
    }
    if (sCrkLastPollTick != 0u) {
        intervalUs = TicksToUs(now - sCrkLastPollTick);
    }
    sCrkLastPollTick = now;
    if (s.ndspOk) {
        gdx_crk_note_poll(&sCrk, queued, intervalUs,
                          ndspChnIsPlaying(GDX3DS_AUDIO_CHANNEL) ? 1 : 0);
        gdx_crk_note_drop_total(&sCrk, ndspGetDroppedFrames());
    }
    gdx_crk_note_ring_drops(&sCrk, s.droppedFrames); /* unlocked diag read */
    if (sCrkWindowStartTick == 0u) {
        sCrkWindowStartTick = now;
        return;
    }
    if (TicksToUs(now - sCrkWindowStartTick) < sCrkWindowUs) {
        return;
    }
    sCrkWindowStartTick = now;
    sCrkWindows++;
    gdx_crk_fold(&sCrk.total, &sCrk.win);
    {
        const int heartbeat = (sCrkEvery != 0u) && (sCrkWindows % sCrkEvery) == 0u;
        if (sCrkEnabled >= 2 || heartbeat || gdx_crk_window_bad(&sCrk.win)) {
            CrkLogWindow("w", &sCrk.win);
        }
        if (heartbeat) {
            CrkLogWindow("T", &sCrk.total);
        }
    }
    gdx_crk_window_reset(&sCrk.win);
}

/* [debug] audio_declick -- see the lever's own comment above sDeclickEnabled. Ramps `real`
 * frames of real content in `pcm` in place: a fade-IN over its first samples if `fadeIn` (this
 * chunk resumes after a dropout), a fade-OUT over its last samples if `fadeOut` (this chunk
 * itself ends in one). Each ramp is independently at most GDX3DS_AUDIO_DECLICK_RAMP_FRAMES
 * long, clamped to `real` so a very short partial chunk never ramps past its own content; where
 * the two windows overlap (real < 2x the ramp length) every sample gets the LOWER of the two
 * multipliers, so the result still only ever falls or rises monotonically within each window
 * -- never a point discontinuity, whichever edge (or both) applies. */
static void DeclickRamp(int16_t* pcm, uint32_t real, int fadeIn, int fadeOut) {
    uint32_t rampIn = fadeIn ? ((real < GDX3DS_AUDIO_DECLICK_RAMP_FRAMES) ? real
                                                                          : GDX3DS_AUDIO_DECLICK_RAMP_FRAMES)
                              : 0u;
    uint32_t rampOut = fadeOut ? ((real < GDX3DS_AUDIO_DECLICK_RAMP_FRAMES) ? real
                                                                            : GDX3DS_AUDIO_DECLICK_RAMP_FRAMES)
                               : 0u;
    uint32_t i;
    if (rampIn == 0u && rampOut == 0u) {
        return;
    }
    for (i = 0; i < real; i++) {
        float g = 1.0f;
        if (rampIn > 0u && i < rampIn) {
            float gi = (float)(i + 1u) / (float)(rampIn + 1u);
            if (gi < g) {
                g = gi;
            }
        }
        if (rampOut > 0u && i >= real - rampOut) {
            float go = (float)(real - i) / (float)(rampOut + 1u);
            if (go < g) {
                g = go;
            }
        }
        if (g < 1.0f) {
            pcm[i * GDX3DS_AUDIO_CHANNELS + 0] = (int16_t)((float)pcm[i * GDX3DS_AUDIO_CHANNELS + 0] * g);
            pcm[i * GDX3DS_AUDIO_CHANNELS + 1] = (int16_t)((float)pcm[i * GDX3DS_AUDIO_CHANNELS + 1] * g);
        }
    }
}

static void FillAndSubmitSlot(uint32_t slotIndex) {
    Gdx3dsAudioSlot* slot = &s.slots[slotIndex];
    int16_t* pcm = &s.waveMem[(size_t)slotIndex * GDX3DS_AUDIO_CHUNK_FRAMES * GDX3DS_AUDIO_CHANNELS];

    uint32_t real = 0;
    if (!PrimeHoldsRing()) {
        real = PullFromRing(pcm, GDX3DS_AUDIO_CHUNK_FRAMES);
        /* [audioring]: the level the pull SAW = what it left plus what it took. The unlocked
         * re-read is the benign aligned-int diag race used throughout this file. Chunks the
         * prime deliberately holds are not censused -- they are policy, not starvation. */
        RcNoteChunk(s.countFrames + real, real);
        CrkNoteChunk(pcm, real);
    }
    if (sDeclickEnabled && real > 0u) {
        DeclickRamp(pcm, real, sInDropout /* fade-IN: resuming after a dropout */,
                   real < GDX3DS_AUDIO_CHUNK_FRAMES /* fade-OUT: this chunk ends in one */);
    }
    if (real < GDX3DS_AUDIO_CHUNK_FRAMES) {
        memset(pcm + (size_t)real * GDX3DS_AUDIO_CHANNELS, 0,
               (size_t)(GDX3DS_AUDIO_CHUNK_FRAMES - real) * GDX3DS_AUDIO_BYTES_PER_FRAME);
        sInDropout = 1; /* cleared below once a chunk's real content again reaches CHUNK_FRAMES */
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
    } else {
        sInDropout = 0; /* a full real chunk: any dropout this fade-in just handled is over */
    }
    slot->realFrames = real;

    /* [audioring] LEVEL census: mean |sample| and peak over the REAL frames only, so the
     * silence the drain splices in cannot dilute the figure it is meant to cross-check. */
    if (sRcEnabled && real != 0u) {
        const uint32_t n = real * GDX3DS_AUDIO_CHANNELS;
        uint64_t sum = 0;
        uint32_t peak = 0;
        uint32_t k;
        for (k = 0; k < n; k++) {
            int32_t v = pcm[k];
            uint32_t a;
            if (v < 0) {
                v = -v;
            }
            a = (uint32_t)v;
            sum += a;
            if (a > peak) {
                peak = a;
            }
        }
        sRcMagSum += sum;
        sRcMagFrames += real;
        if (peak > sRcPeak) {
            sRcPeak = peak;
        }
    }

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

    /* AUDIOFIX round 3: [debug] pcm_dump_s -- capture the EXACT bytes about to reach the DSP
     * (post-ring, post-padding, post-testtone), so a listener can tell which side of this line
     * the user's reported crackle is on. Inert (one int compare) unless armed. */
    gdx_pcmdump_submit(pcm, GDX3DS_AUDIO_CHUNK_FRAMES, sChunksSubmitted, (uint32_t)MsSinceInit());

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
                 (unsigned)(sPrimeEnabled ? sQueueCap : GDX3DS_AUDIO_NUM_WAVEBUFS),
                 (unsigned long)sQueueSkips, (unsigned long)s.underrunChunks,
                 (unsigned long)sPushCalls, (unsigned long)sPushFrames, sStallSimMs);
    sRingMinSincePrint = UINT32_MAX;
    LogPrime2Receipt();
}

/* n60-3d12 [audiocost]: drain thread, diag_audio=1, same ~5 s cadence as [audioprime] above
 * (GDX3DS_AUDIO_DIAG_POLLS). Formats the window the cost ledger last PUBLISHED (so the very
 * first call before anything has published yet prints nothing -- gdx_hle_cost_format_audiocost
 * returns 0 immediately), then requests the next publish, same order OthEmitSynthCost uses for
 * [synx]/[synop] (gdx3ds_gpu_prof.c) so a report never blocks on the producer thread. Re-arms
 * the ledger if something else (debug.gputrace's own live toggle) turned it off since the last
 * call -- this module's own arm request always wins back within one ~5 s window. */
static void LogAudioCostReceipt(void) {
    char line[224];
    double windowSeconds;
    int i;
    if (!gdx_hle_cost_armed) {
        gdx_hle_cost_arm(1, gdx3ds_prof_now, (double)SYSCLOCK_ARM11);
    }
    windowSeconds = gdx_hle_cost_published_span_seconds();
    for (i = 0; gdx_hle_cost_format_audiocost(i, line, sizeof(line), windowSeconds) > 0; i++) {
        AudioOutLogf("%s", line);
    }
    gdx_hle_cost_request_publish((unsigned long)sProdTickCount);
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
    return queued >= sQueueCap;
}

static void DrainThreadMain(void* arg) {
    (void)arg;
    uint32_t polls = 0;
    while (!s.stopRequested) {
        if (sSuspended) {
            ParkDrainWhileSuspended(); /* HOME/sleep: nothing below may run (see gate) */
            sRcLastPollTick = 0u;      /* a park is policy, not a scheduling interval */
            sCrkLastPollTick = 0u;
            continue;
        }
        RcNotePoll(); /* [audioring] drain scheduling + DSP queue depth, BEFORE the sweep */
        CrkNotePoll(); /* [audiocrk] DSP hop + window rollover, BEFORE the sweep */
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
        if (sRcEnabled && sRcReportPolls != 0u && ((polls + 1u) % sRcReportPolls) == 0u) {
            LogRingCensus("win");
        }
        if (sDiagAudio && (++polls % GDX3DS_AUDIO_DIAG_POLLS) == 0u) {
            LogAudioCostReceipt(); /* n60-3d12 [audiocost]: same ~5 s cadence as [audio-out] below */
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
        /* Syscore threads need the app's syscore share granted first; for homebrew this
         * call only succeeds under Luma3DS >= 10.1.1 (document for users). */
        if (core == 1 && !TakeSyscoreShare()) {
            continue;
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

/* [audiocrk] + o3dscrackle levers (see the block beside sCrkEnabled). Every default leaves
 * the shipped path byte for byte. */
static void ReadCrackleLevers(void) {
    int q;
    sCrkEnabled = gdx3ds_config_get_int("debug", "audiocrk", 0);
    {
        int ms = gdx3ds_config_get_int("debug", "audiocrk_ms", 1000);
        if (ms < 100) {
            ms = 100;
        } else if (ms > 60000) {
            ms = 60000;
        }
        sCrkWindowUs = (uint32_t)ms * 1000u;
    }
    sCrkEvery = (uint32_t)gdx3ds_config_get_int("debug", "audiocrk_every", 10);
    sCrkWindows = 0u;
    sCrkWindowStartTick = 0u;
    sCrkLastPollTick = 0u;
    sCrkPushDetail = 0u;
    gdx_crk_init(&sCrk, gdx3ds_config_get_int("debug", "audiocrk_step", GDX_CRK_STEP_DEFAULT));
    sNdspClipLever = gdx3ds_config_get_int("debug", "ndsp_clip", 0) != 0 ? 1 : 0;
    sNdspGainPct = gdx3ds_config_get_int("debug", "ndsp_gain_pct", 0);
    if (sNdspGainPct < 0 || sNdspGainPct > 100) {
        sNdspGainPct = 0;
    }
    /* n60-3d12: default 3 on Old 3DS (card n60-3d11 "ALL", hardware-confirmed), 0 (off) on
     * New 3DS, unchanged. An explicit `audio_queue = 0` in the ini still turns it off either way. */
    q = gdx3ds_config_get_int("debug", "audio_queue", gdx3ds_is_old3ds() ? 3 : 0);
    sQueueCap = GDX3DS_AUDIO_PRIME_QUEUE;
    if (q >= 2 && q <= (int)GDX3DS_AUDIO_NUM_WAVEBUFS) {
        sQueueCap = (uint32_t)q;
    }
}

/* The DSP-side half of the levers, once ndsp is up. Receipt only when something moved. */
static void ApplyCrackleNdspLevers(float* mix) {
    if (sNdspClipLever) {
        ndspSetClippingMode(NDSP_CLIP_NORMAL);
    }
    if (sNdspGainPct > 0) {
        mix[0] = (float)sNdspGainPct / 100.0f;
        mix[1] = (float)sNdspGainPct / 100.0f;
    }
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
    /* [audioring] levers. audio_ring_frames always defaults 0 = unchanged path (it only ever
     * GROWS the ring; a smaller one would change drop-oldest behaviour for no benefit).
     * audio_target_frames defaults to 2560 on Old 3DS (card n60-3d11 "ALL", hardware-confirmed)
     * and stays 0 (off) on New 3DS; an explicit `audio_target_frames = 0` in the ini turns it
     * off on either model. It is clamped against the ring in
     * gdx3ds_audio_producer_target_frames so a target can never exceed what the ring plus one
     * 528-frame tick can hold. */
    sRcTargetOverride = (int32_t)gdx3ds_config_get_int("debug", "audio_target_frames",
                                                        gdx3ds_is_old3ds() ? 2560 : 0);
    sRcRingOverride = (uint32_t)gdx3ds_config_get_int("debug", "audio_ring_frames", 0);
    if (bufferFrames == 0 && sRcRingOverride > s.capacityFrames) {
        s.capacityFrames = sRcRingOverride;
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
    sRcEnabled = gdx3ds_config_get_int("debug", "audioring", 0);
    sRcReportPolls = (uint32_t)gdx3ds_config_get_int("debug", "audioring_report", 2500);
    RcResetWindow();
    sRcChunksSincePush = GDX3DS_AUDIO_RC_PUSH_GRACE + 1u; /* nothing pushed yet */
    sRcDryRun = 0u;
    sRcTicksThisWake = 0u;
    sRcLastTickEndTick = 0u;
    sTestTone = gdx3ds_config_get_bool("debug", "audio_testtone", 0);
    sPrimeEnabled = gdx3ds_config_get_bool("debug", "audioprime", 1);
    ReadCrackleLevers();
    sStallSimMs = gdx3ds_config_get_int("debug", "audio_stallsim", 0);
    /* n60-3d12: default ON on Old 3DS (card n60-3d11 "ALL", hardware-confirmed). v1.0.1: default
     * ON on New 3DS too -- it only touches the splice at a dropout edge, which is a hard click on
     * any model. `audio_declick = 0` in the ini still forces the old hard-splice path on either
     * model. */
    sDeclickEnabled = gdx3ds_config_get_bool("debug", "audio_declick", 1);
    sInDropout = 0;
    /* [audiosched] levers. audio_producer_prio = 0 keeps the historical 0x18 (the drain's own
     * rung) byte for byte; any other value is clamped into the app-grantable band and applied
     * at threadCreate. The stress hold is a diagnostic and is never on by default. */
    {
        int prio = gdx3ds_config_get_int("debug", "audio_producer_prio", 0);
        if (prio != 0) {
            if (prio < GDX3DS_AUDIO_PRIO_MIN) {
                prio = GDX3DS_AUDIO_PRIO_MIN;
            } else if (prio > GDX3DS_AUDIO_PRIO_MAX) {
                prio = GDX3DS_AUDIO_PRIO_MAX;
            }
            sProducerPrio = prio;
        }
    }
    sProdHoldMs = gdx3ds_config_get_int("debug", "audio_prodhold_ms", 0);
    sProdHoldEvery = (uint32_t)gdx3ds_config_get_int("debug", "audio_prodhold_every", 64);
    sInitMs = osGetTime();
    sRingMinSincePrint = UINT32_MAX;
    sPrimeActive = 0;
    if (sPrimeEnabled) {
        ArmPrime();
    }

    Result rc = StartNdspUnlessDisabled();
    if (rc == kNdspDisabledByLever) {
        AudioOutLogf("[audio-out] ndsp DISABLED (debug.audio_ndsp=0) -> NULL SINK, game runs "
                     "SILENT; no DSP component is loaded, so the APT suspend skips aptDspSleep");
        s.ndspOk = 0;
    } else if (R_FAILED(rc)) {
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
        ApplyCrackleNdspLevers(mix); /* [debug] ndsp_clip / ndsp_gain_pct, default no-op */
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
                     "clip=%s outcount=2 mix[FL]=%.2f mix[FR]=%.2f mastervol=%.2f paused=%d "
                     "wavemem=%p paddr=0x%08lX ring=%lu frames testtone=%d",
                     GDX3DS_AUDIO_CHANNEL, (unsigned)GDX3DS_AUDIO_SAMPLE_RATE,
                     sNdspClipLever ? "normal" : "soft", (double)mix[0], (double)mix[1],
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
                 (unsigned)(sPrimeEnabled ? sQueueCap : GDX3DS_AUDIO_NUM_WAVEBUFS),
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
    AudioOutLogf("[audiosched] producer prio 0x%02X%s, drain prio 0x%02X, drain deadline %u us "
                 "(queue cap %u); prodhold=%dms/%luticks",
                 (unsigned)sProducerPrio,
                 sProducerPrio == GDX3DS_AUDIO_PRODUCER_PRIO ? " (default)" : " (INI override)",
                 (unsigned)GDX3DS_AUDIO_DRAIN_PRIO, (unsigned)GDX3DS_AUDIO_RC_POLL_DEADLINE_US,
                 (unsigned)(sPrimeEnabled ? sQueueCap : GDX3DS_AUDIO_NUM_WAVEBUFS),
                 sProdHoldMs, (unsigned long)sProdHoldEvery);
    if (sCrkEnabled || sNdspClipLever || sNdspGainPct > 0 || sQueueCap != GDX3DS_AUDIO_PRIME_QUEUE) {
        AudioOutLogf("[audiocrk] on=%d window=%lums every=%lu step=%ld | levers ndsp_clip=%d (%s) "
                     "ndsp_gain_pct=%d audio_queue=%lu%s target_override=%ld",
                     sCrkEnabled, (unsigned long)(sCrkWindowUs / 1000u), (unsigned long)sCrkEvery,
                     (long)sCrk.stepThreshold, sNdspClipLever, sNdspClipLever ? "NORMAL" : "SOFT",
                     sNdspGainPct > 0 ? sNdspGainPct : 100, (unsigned long)sQueueCap,
                     sPrimeEnabled ? "" : " (audioprime=0: 3 always)", (long)sRcTargetOverride);
    }
    gdx_pcmdump_init(); /* AUDIOFIX round 3: [debug] pcm_dump_s, inert (no thread/alloc) unless armed */
    return 0;
}

void gdx3ds_audio_shutdown(void) {
    if (!s.initialized) {
        return;
    }
    LogRingCensus("FINAL"); /* whole-session totals; both threads are still the only writers */
    if (sCrkEnabled) {
        gdx_crk_fold(&sCrk.total, &sCrk.win);
        gdx_crk_window_reset(&sCrk.win);
        CrkLogWindow("FINAL", &sCrk.total);
    }
    /* [ndsphash] whole-session render digest, emitted beside [audioring]'s FINAL because the
       two answer different halves of one question: [audioring] says whether the samples
       ARRIVED, [ndsphash] says whether they were the right ones. */
    {
        extern void gdx_ndsphash_report_final(void);
        gdx_ndsphash_report_final();
    }
    /* [audiomix] mix-bus headroom, the third half of the same question: [audioring] says
       whether the samples ARRIVED, [ndsphash] whether they were the right ones, [audiomix]
       whether the sum that produced them had any room left. No-op unless armed. */
    {
        extern void gdx_audiomix_report_final(void);
        gdx_audiomix_report_final();
    }
    s.stopRequested = 1;         /* set BEFORE the signal: the park loop re-checks it */
    __dmb();
    LightEvent_Signal(&sDrainParkEvent); /* a drain parked by the suspend gate must see stop */
    if (s.drainThread != NULL) {
        /* HOMEFIX2: bounded. This teardown runs on the close-from-HOME path with the DSP
         * asleep; a drain stuck in an ndsp call there would wedge main() one step short of
         * the final flush (the hang home-crash-audit.md's "Close-from-HOME hang fix"
         * documents). Leak the thread deliberately with a receipt instead. */
        if (R_FAILED(threadJoin(s.drainThread, 3000000000LL))) {
            AudioOutLogf("[audio-out] drain join TIMEOUT (3 s) — leaking the thread so teardown "
                         "can finish; suspended=%d stopRequested=%d",
                         sSuspended ? 1 : 0, s.stopRequested);
        } else {
            threadFree(s.drainThread);
        }
        s.drainThread = NULL;
    }
    gdx_pcmdump_shutdown(); /* after the drain stops: no more submit() calls can race the final flush */
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
    if (sCrkEnabled && gdx_crk_note_push(&sCrk, samples, (uint32_t)frames) &&
        sCrkPushDetail < GDX3DS_AUDIO_CRK_DETAIL_MAX) {
        /* producer-side boundary pair stepped: the four frames around it, first few only */
        sCrkPushDetail++;
        const int16_t(*e)[2] = sCrk.evPush;
        AudioOutLogf("[audiocrk] pstep#%lu t=%lums push=%lu frames=%lu tick=%lu "
                     "L=%d,%d,%d,%d|%d,%d,%d,%d R=%d,%d,%d,%d|%d,%d,%d,%d",
                     (unsigned long)sCrkPushDetail, (unsigned long)MsSinceInit(),
                     (unsigned long)sPushCalls, (unsigned long)frames,
                     (unsigned long)sProdTickCount, e[0][0], e[1][0], e[2][0], e[3][0], e[4][0],
                     e[5][0], e[6][0], e[7][0], e[0][1], e[1][1], e[2][1], e[3][1], e[4][1],
                     e[5][1], e[6][1], e[7][1]);
    }
    sRcChunksSincePush = 0u; /* [audioring]: the producer is live as of this chunk */
    if (sFirstPushMs == 0) {
        sFirstPushMs = osGetTime(); /* prime timeout base; producer thread only */
    }
    if (sStallSimMs > 0 &&
        ((sPushCalls == 20u || sPushCalls == 60u || sPushCalls == 100u) ||
         (gGdxRaceActive && (sPushCalls % GDX3DS_AUDIO_STALLSIM_RACE_PERIOD) == 0u))) {
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
        /* Same homebrew/Luma>=10.1.1 caveat as the drain thread's syscore rung. The drain
         * may already hold the single syscore slot; then this threadCreate simply returns
         * NULL and we fall through to core 0. */
        if (core == 1 && !TakeSyscoreShare()) {
            continue;
        }
        sProducerThread = threadCreate((ThreadFunc)entry, arg, GDX3DS_AUDIO_PRODUCER_STACK,
                                       sProducerPrio, core, false);
        if (sProducerThread != NULL) {
            sProducerCore = core;
            AudioOutLogf("[audio-out] HLE producer thread on core %d prio 0x%02X (drain 0x%02X)",
                         core, (unsigned)sProducerPrio, (unsigned)GDX3DS_AUDIO_DRAIN_PRIO);
            return 0;
        }
    }
    return -1;
}

void gdx3ds_audio_producer_thread_join(void) {
    if (sProducerThread != NULL) {
        sProducerRelease = 1; /* a producer parked by the suspend gate must be able to exit */
        __dmb();
        LightEvent_Signal(&sProducerParkEvent); /* producer's event ONLY: never wakes the drain */
        /* HOMEFIX2: bounded, same reason as the drain join in gdx3ds_audio_shutdown. */
        if (R_FAILED(threadJoin(sProducerThread, 3000000000LL))) {
            AudioOutLogf("[audio-out] HLE producer join TIMEOUT (3 s) — leaking the thread so "
                         "teardown can finish; suspended=%d", sSuspended ? 1 : 0);
        } else {
            threadFree(sProducerThread);
        }
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
    sRcLastWakeTick = 0u; /* a park is policy, not a scheduling interval */
    sProducerParked = 1;
    __dmb(); /* HOMEFIX2: see ParkDrainWhileSuspended -- publish the ack before blocking. */
    while (sSuspended && !sProducerRelease) {
        LightEvent_Wait(&sProducerParkEvent);
    }
    sProducerParked = 0;
    __dmb();
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
    gdx_pcmdump_notify_suspend(); /* AUDIOFIX round 6: HOME-menu pause is one of pcm_dump_s's
                                      three flush triggers; cheap flag-set, never blocks here */
    LightEvent_Clear(&sDrainParkEvent); /* clear BEFORE the flag: a thread seeing 1 must block */
    LightEvent_Clear(&sProducerParkEvent);
    sSuspended = 1;
    __dmb(); /* HOMEFIX2: the flag must be visible to both threads before we start polling
              * their acks; without it the poll can read a stale 0 for the whole 50 ms and
              * the receipt blames a thread that did park. */
    int needDrain = (s.drainThread != NULL);
    int needProducer = (sProducerThread != NULL && !sProducerRelease);
    uint32_t polls = 0;
    for (;;) {
        __dmb(); /* HOMEFIX2: pair with the park-side barriers above */
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
        gdx_crk_break_chain(&sCrk); /* restart from silence: no boundary is meaningful */
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
    /* [debug] audio_target_frames overrides the cushion the producer paces to, for BOTH the
     * boot and run phases. 0 (default) leaves every branch below exactly as it was. It is
     * clamped so a target plus one 528-frame producer tick can never wrap the ring into
     * drop-oldest, which would turn a cushion into silence. */
    if (sRcTargetOverride > 0 && s.initialized) {
        return RcClampTarget(sRcTargetOverride);
    }
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
    /* [debug] audio_prodhold_ms: BUSY-hold the core for N ms on every Nth tick. Not a fix and
     * not shipped on -- it is the emulator stand-in for a CPU-bound hardware tick, the one
     * thing Azahar's host CPU never reproduces, so "does an equal-priority drain on the same
     * core still get to refill the DSP queue" can be asked without a console. The hold is a
     * svcGetSystemTick spin with NO syscall in it, so it never yields; it sits OUTSIDE
     * sAudioCtxMutex (AudioThreadMain takes that after this hook returns), so it cannot
     * deadlock the game thread's QueueCmd. Default 0 = never entered. */
    if (sProdHoldMs > 0 && sProdHoldEvery != 0u && (sProdTickCount % sProdHoldEvery) == 0u) {
        uint64_t until = sProdTickStartTick +
                         (uint64_t)sProdHoldMs * 1000ull * (uint64_t)GDX3DS_AUDIO_TICKS_PER_US;
        while (svcGetSystemTick() < until) {
            /* spin */
        }
    }
    /* [audioring]: the GAP between the end of the previous tick and the start of this one is
     * the producer's "was I allowed to run" axis. A long gap with short ticks means the thread
     * did not get the core; a long TICK means it was stuck inside production (on the EK build,
     * typically AudioLoad_SyncDiskDrive blocking on a 64DD LBA). The two cannot be confused. */
    if (sRcEnabled && sRcLastTickEndTick != 0u) {
        uint32_t gapUs = TicksToUs(sProdTickStartTick - sRcLastTickEndTick);
        sRcGapUs[RcUsBucket(gapUs)]++;
        if (gapUs > sRcGapUsMax) {
            sRcGapUsMax = gapUs;
        }
    }
}

void gdx3ds_audio_producer_tick_end(void) {
    uint32_t us = TicksToUs(svcGetSystemTick() - sProdTickStartTick);
    sProdInTick = 0;
    sProdLastTickUs = us;
    if (us > sProdMaxTickUs) {
        sProdMaxTickUs = us;
    }
    sProdTickCount++;
    if (sCrkEnabled) {
        gdx_crk_note_tick(&sCrk, us);
    }
    if (sRcEnabled) {
        sRcTicks++;
        sRcTicksThisWake++;
        sRcTickUs[RcUsBucket(us)]++;
        if (us > sRcTickUsMax) {
            sRcTickUsMax = us;
        }
        sRcLastTickEndTick = svcGetSystemTick();
    }
}

/* HLE producer thread only, once per loop iteration, BEFORE the catch-up loop. Folds in what
 * the previous iteration did (ticks produced, whether it left on the kMaxTicksPerWake cap) and
 * records the buffered level the producer is about to pace against. Off = one int compare. */
void gdx3ds_audio_producer_wake(int32_t target, int32_t maxTicksPerWake) {
    uint32_t buffered;
    if (sCrkEnabled) {
        /* [audiocrk] the producer's deadline slack: buffered() as it wakes (unlocked read). */
        gdx_crk_note_wake(&sCrk, s.countFrames + s.inFlightFrames);
    }
    if (!sRcEnabled) {
        return;
    }
    buffered = s.countFrames + s.inFlightFrames; /* unlocked: benign aligned-int diag race */
    if (sRcWakes != 0u) {
        if (sRcTicksThisWake == 0u) {
            sRcWakesIdle++;
        } else if ((int32_t)sRcTicksThisWake >= maxTicksPerWake) {
            sRcWakesCapped++;
        }
    }
    sRcTicksThisWake = 0u;
    sRcWakes++;
    sRcBufAtWake[RcOccBucket(buffered)]++;
    /* WAKE-to-WAKE, the real "did this thread get the core" axis. AudioThreadMain's loop is a
     * 5 ms timed condvar wait (plus a per-frame notify), so this interval is ~5 ms whenever
     * the thread is scheduled, INDEPENDENT of whether it then decides to produce. `gap_ms`
     * cannot say this: it is tick-to-tick, and an idle-at-target producer books a long gap
     * while being perfectly scheduled. The tick duration is inside this interval by
     * construction, so a long TICK also widens it -- read it beside tick_ms, never alone. */
    {
        uint64_t now = svcGetSystemTick();
        if (sRcLastWakeTick != 0u) {
            uint32_t us = TicksToUs(now - sRcLastWakeTick);
            sRcWakeUs[RcUsBucket(us)]++;
            if (us > sRcWakeUsMax) {
                sRcWakeUsMax = us;
            }
        }
        sRcLastWakeTick = now;
    }
    (void)target;
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
/* O3DSHOMEFIX: 1 on an Old 3DS (APT_CheckNew3DS false), else 0. Model gate for the Old 3DS
 * defaults ([debug] audio_ctx_narrow / audio_simd, port/gdx_audio_thread.cpp). */
int gdx3ds_is_old3ds(void) {
    bool isNew3ds = false;
    APT_CheckNew3DS(&isNew3ds);
    return isNew3ds ? 0 : 1;
}

int gdx3ds_audio_grant_syscore(void) {
    return TakeSyscoreShare();
}

/* O3DSHOME: per-thread park state for the suspended-mode watch receipts. Racy reads of
 * flags the threads publish with __dmb() (diagnostic only). An absent thread reads -1. */
void gdx3ds_audio_park_state(int* drainParked, int* producerParked, int* preloadRunning) {
    if (drainParked != NULL) {
        *drainParked = s.drainThread != NULL ? sDrainParked : -1;
    }
    if (producerParked != NULL) {
        *producerParked = sProducerThread != NULL ? sProducerParked : -1;
    }
    if (preloadRunning != NULL) {
        *preloadRunning = sPreloadRunning;
    }
}

/* Restore-side re-application (cause #5 in docs/research/home-crash-audit.md): the percent
 * APT_SetAppCpuTimeLimit was granted at init (0 if no core-1 rung was ever taken). */
int gdx3ds_audio_syscore_limit_percent(void) {
    return sSyscoreLimitPct;
}

/* N60CORE1: partial-chunk underrun edges counted by the drain (racy read, diagnostic). */
unsigned long gdx3ds_audio_underruns(void) {
    return (unsigned long)s.underrunChunks;
}

/* N60CORE1: the core libctru's ndsp thread was created on under [debug] ndsp_core1
 * (-1 = lever off / refused, i.e. the default core). */
int gdx3ds_audio_ndsp_core(void) {
    return sNdspCreatedCore;
}
