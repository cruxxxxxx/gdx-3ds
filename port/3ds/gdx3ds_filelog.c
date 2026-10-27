/* port/3ds/gdx3ds_filelog.c — see include/gdx3ds_filelog.h for the contract.
 *
 * Lives beside main_3ds.cpp (integration level, not a stream directory) because it is
 * the tracers' sink, not an OS backend: the hooks are one-line calls from main_3ds.cpp's
 * logStep/logFatal/portLogSvcTap/watchdog paths, kept out of that file so a parallel
 * main_3ds.cpp shift (perf stream) merges cleanly.
 *
 * LEAK-HARDENING (first hardware OOM, [fatal] operator new failed size=504): every byte
 * this sink touches is STATIC.
 *
 * MENU (v1 bottom-screen menu): the 64 KiB ring is now a RETAINED circular history, not
 * a drain-and-reset batch buffer — the menu's LOG tab reads its tail through
 * gdx3ds_filelog_tail_lines(). Retention is ALWAYS on (debug.filelog only gates the SD
 * mirror), so the LOG tab works with zero SD traffic. SD semantics are unchanged from
 * the batch design: bytes between sFlushedTotal and sTotal are the pending batch,
 * flushed when the pending span passes the high-water mark, every
 * GDX3DS_FILELOG_FLUSH_MS, on any "[fatal"-prefixed line, or explicitly; a write that
 * would overwrite unflushed bytes flushes first, so the SD file never loses a line the
 * old design kept. A line that cannot fit the ring at all is dropped and counted
 * ("[filelog] dropped=N" on the next flush).
 *
 * Compiled only in the NINTENDO_3DS build (registered in port/3ds/CMakeLists.txt);
 * uses libctru's RecursiveLock so a fatal tracer firing while the lock is held on the
 * SAME thread (e.g. abort inside a write) cannot self-deadlock.
 */
#include "gdx3ds_filelog.h"
#include "gdx3ds_config.h"
/* HWLINK: the one sink hook that mirrors every tracer line to the host over WiFi. The
 * call is a memcpy into a static SPSC ring and returns -- it never blocks, never
 * allocates, and takes no lock of its own (this sink's recursive lock, already held here,
 * is what serialises the single producer). Inert unless [net] enabled = 1. */
#include "gdx3ds_net.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h> /* fsync: archive_dev's fsync is FSFILE_Flush; write: the raw span write */

#include <3ds.h>

#include "gdx3ds_hitch.h" /* gdx3ds_hitch_aud_inflight: the writer yields to audio-stream reads */
#include "gdx3ds_cpucut.h" /* o3ds-cpucuts: [debug] filelog_flush_defer = 2 is a block A/B */

/* REL1CRASH: a bisect title (-DGDX_3DS_TITLE_TAG=X) logs to log-X.txt so several installed
 * titles sharing one ini never overwrite each other's evidence. Untagged: the historic names. */
#ifndef GDX3DS_LOG_TAG
#define GDX3DS_LOG_TAG ""
#endif
#define GDX3DS_FILELOG_PATH "sdmc:/3ds/gdiffuser/log" GDX3DS_LOG_TAG ".txt"
#define GDX3DS_FILELOG_PREV_PATH "sdmc:/3ds/gdiffuser/log" GDX3DS_LOG_TAG "-prev.txt"
#define GDX3DS_FILELOG_PREV2_PATH "sdmc:/3ds/gdiffuser/log" GDX3DS_LOG_TAG "-prev2.txt"
#define GDX3DS_FILELOG_DEFAULT_MAX_KB 1024

/* Retained ring: 64 KiB holds ~400 typical tracer lines. */
#define GDX3DS_FILELOG_RING_CAP 65536u
#define GDX3DS_FILELOG_RING_HIWAT ((GDX3DS_FILELOG_RING_CAP / 4u) * 3u)
#define GDX3DS_FILELOG_FLUSH_MS 250u

static FILE* sLogFile = NULL;
static RecursiveLock sLock;
static int sLockInited = 0; /* first writes are main-thread boot logSteps, so the lazy
                             * init below cannot race (threads start much later). */
static size_t sBytesWritten = 0;
static size_t sByteCap = 0;
static size_t sMpReserve = 0;      /* [debug] filelog_mp_reserve_kb: the cap's tail kept for MP receipts */
static int sMpBudgetNoted = 0;
static volatile int sMpSession = 0; /* the reserve applies only while an MP room / race is up */
static unsigned sMpBudgetDrops = 0;
/* Static stream buffer: newlib otherwise mallocs one on first write, which the
 * operator-new failure tracer must never do. */
static char sStreamBuf[1024];
/* Retained circular history (guarded by sLock). Static: the sink must stay usable from
 * the operator-new failure path, so it can never allocate. */
static char sRing[GDX3DS_FILELOG_RING_CAP];
static unsigned sTotal = 0;        /* monotonic bytes ever appended (mod 2^32) */
static unsigned sFlushedTotal = 0; /* bytes already mirrored to SD */
static unsigned long sDroppedLines = 0; /* since the last dropped= report */
static unsigned long sDroppedTotal = 0; /* whole-run, reported alongside */
static u64 sLastFlushTick = 0;
/* REL1CRASH [debug] filelog_sync_ms (default 0): every line is flushed until this tick, so a
 * boot-time death leaves its last line on the card instead of up to 250 ms of batch. */
static u64 sSyncUntilTick = 0;
/* n60hwdiag: per-line flush + fsync (gdx3ds_filelog_set_durable), and the per-boot session file
 * (gdx3ds_filelog_set_session_path). fflush alone only reaches FSFILE_Write without the flush flag
 * (libctru archive_dev passes FS_WRITE_FLUSH only for O_SYNC files): fsync is the FSFILE_Flush. */
static volatile int sDurable = 0;
static char sSessionPath[96];
/* N60LAUNCH: the launch variant's log name (gdx3ds_filelog_set_tag) and the deferred-init history
 * mirror (gdx3ds_filelog_set_mirror_history). Both are read once, by gdx3ds_filelog_init. */
static char sTagPath[96];
static char sTagPrevPath[96];
static int sMirrorHistory = 0;

/* C4I logging-cost ledger ([othx] lg columns, port/3ds/gfx/gdx3ds_gpu_prof.c). Armed by
 * gdx3ds_filelog_cost_arm (gputrace latch); unarmed, write() pays one int test. Timed
 * lines are split by caller: the logical main thread (host + game fibers, i.e. the frame
 * path under renderthread=0) versus every other thread. The ledger is guarded by sLock
 * like the ring, and the timed span INCLUDES the lock wait -- contention is real cost. */
static int sCostArmed = 0;
static u64 sCostTicks[2];            /* [0] main, [1] other threads */
static unsigned long sCostLines[2];
static unsigned long sCostFlushes = 0; /* SD batch flushes (any thread) */
static u64 sCostFlushTicks = 0;        /* ... and their fwrite+fflush time */
extern int gdx_sched_on_host_thread(void) __attribute__((weak));

/* HITCH: [debug] filelog_async (DEFAULT 0). The hardware log hw-rel2 (native/main@dbedfb5)
 * put the synchronous batch flush at 26-270 ms PER FLUSH on the Old 3DS card ([othx] lgF x n
 * / flushes, every gputrace window), 8-11 flushes per 256-tick window, all of it on MAIN
 * (lgM == lgF) -- the steady windows missed 8-15 slots each, one per flush. With the lever
 * on, the SD write moves to a writer thread: producers only append to the ring (and kick
 * the writer at the high-water mark); the writer snapshots the pending span under the ring
 * lock, writes it with NO ring lock held, and publishes sFlushedTotal. Order is the ring's
 * order (one writer at a time, serialised by sFileLock). Crash safety is unchanged: a
 * fatal-class line, an explicit flush() and a ring that would overwrite unflushed bytes all
 * still flush synchronously (they wait for an in-flight write first). Lock order: sLock ->
 * sFileLock only; the writer never holds both. */
#define GDX3DS_FILELOG_ASYNC_MS_DEFAULT 1000u
#define GDX3DS_FILELOG_ASYNC_KICK ((GDX3DS_FILELOG_RING_CAP / 2u))
#define GDX3DS_FILELOG_WRITER_STACK (8u * 1024u)
#define GDX3DS_FILELOG_WRITER_PRIO 0x2F /* just above main (0x30): it only runs between SD IPCs */
static int sAsync = 0;
static unsigned sAsyncMs = GDX3DS_FILELOG_ASYNC_MS_DEFAULT;
static LightLock sFileLock;
static LightEvent sWriterWake;
static Thread sWriterThread = NULL;
static volatile u32 sWriterThreadId = 0;
static volatile int sWriterBusy = 0;
static GdxFilelogStats sStats; /* cumulative; async fields written by the writer only */
/* QUICKFIX [debug] filelog_race_quiet (default 0, async writer only): while a race is on screen
 * the writer's timed and kicked passes are deferred until the ring is 3/4 unflushed
 * (GDX3DS_FILELOG_RING_HIWAT), so its SD writes stop competing with the audio stream's sample
 * reads (hwprof-progress.md §10.3: every first-lap `au` hitch had an audio SD read queued behind
 * a writer SD write). Fatal-class lines, explicit flushes and a full ring still flush at once
 * (those paths are synchronous and never consult this). */
static int sRaceQuiet = 0;
extern int gdx_input_in_gameplay(void) __attribute__((weak)); /* port/input_bridge.c */

/* N60LOG (docs/research/native/n60log-progress.md): the file log never blocks the logging thread.
 * Hardware card n60-card2-C (Old 3DS, async + race_quiet): the writer deferred to 48 KiB, its SD
 * write of that span took 2.6-2.9 s, the race's receipts (~5.2 KB/s) filled the remaining 16 KiB
 * during it, and the next line on MAIN waited for the writer and then flushed 15 KiB itself
 * ([hitch] ms=687-2068, sync=1/15K/526-856 ms, ringfull=1-2 per 1024 iterations: the "harsh hitch
 * every fifth of the track").
 *   [debug] filelog_drop_full (default 1, async only): a line that would overwrite unflushed bytes
 *     is DROPPED and counted (written as "[filelog] dropped=N total=M" by the writer's next pass,
 *     dropped= in [hitch] sum) instead of flushing on the caller. Fatal-class lines, the
 *     filelog_sync_ms window and n60hwdiag's durable mode still flush synchronously.
 *   The size cap in async mode is closed by the writer (no synchronous flush on the caller).
 *   race_quiet now defers to HALF the ring (was 3/4), leaving 32 KiB of headroom for the
 *     slowest SD write seen (2.9 s x 5.2 KB/s = 15 KiB at the full receipt cadence).
 *   The writer writes raw, in 8 KiB chunks, and before each chunk yields (up to 50 ms) to an
 *     in-flight audio-stream SD read (hwprof §10.3: audio block reads queued behind log writes).
 *   [debug] receipt_every (default 64): window receipts at a lower cadence (see ReceiptThinned).
 *   [debug] filelog_stall_ms (default 0, TEST ONLY): the writer sleeps this long inside each write,
 *     a slow SD card on the emulator (the drop gate). */
#define GDX3DS_FILELOG_RACE_QUIET_MARK (GDX3DS_FILELOG_RING_CAP / 2u)
#define GDX3DS_FILELOG_WRITE_CHUNK 8192u
#define GDX3DS_FILELOG_AUDIO_YIELD_MAX_MS 50
#define GDX3DS_FILELOG_RECEIPT_BASE 64
static int sDropFull = 1;
static unsigned sStallMs = 0;
static int sCapPending = 0;   /* async size cap: the writer closes the file at sCapAt */
static unsigned sCapAt = 0;
static int sReceiptEvery = GDX3DS_FILELOG_RECEIPT_BASE;
/* o3ds-cpucuts [debug] filelog_flush_defer (include/gdx3ds_filelog.h). Latched at init. */
static int sFlushDefer = 1;

static void gdx3ds_filelog_ensure_lock(void) {
    if (!sLockInited) {
        RecursiveLock_Init(&sLock);
        sLockInited = 1;
    }
}

static int ClampInt(int v, int lo, int hi) {
    if (v < lo) {
        return lo;
    } else if (v > hi) {
        return hi;
    }
    return v;
}

static unsigned FlushedTotal(void) {
    return __atomic_load_n(&sFlushedTotal, __ATOMIC_ACQUIRE);
}

/* ---- N60LOG [debug] receipt_every ---------------------------------------------------------------
 * Two mechanisms, one knob (N = receipt_every, default 64 = today's cadence, 64..4096, a multiple
 * of 64):
 *   - the [c5cat] window family ([c5cat]/[c5cnt]/[c5sub]/[c5pmu]/[hwsite]/[ships]/[lsplit]/[ab],
 *     joined by frame= in hwprof_agg.py / ships_agg.py) LENGTHENS its window to N iterations
 *     (gdx3ds_mainprof.c, gdx3ds_ships.c read gdx3ds_filelog_receipt_every()); every column is
 *     normalised by the window's own n= / r=, and the c5pmu pair still rotates per window.
 *   - every other steady-state window receipt below (64- or 128-iteration windows owned by a dozen
 *     modules) is THINNED here: per key, a line passes if N - 32 host iterations have passed since
 *     that key's last passed line (gdx3ds_filelog_frame_end is the clock; a passed line is still
 *     one whole native window, the slack absorbs a window landing a few frames early). Event lines (bakes, dynlod
 *     raise/lower, [hitch], [fatal], init/refusal lines, [gstate]/[fh]) never match a key.
 * Thinned lines skip the ring (and so the LOG tab and HWLINK); svcOutputDebugString is the
 * emitter's and is untouched. */
static const char* const kThinnedReceipts[] = {
    "[nrr] frame=",      "[nrr] mach frames=", "[nrrdx] frame=",   "[nrrfx] tasks=",     "[nrrsky] tasks=",
    "[nrrhud] tasks=",   "[nrrtrk] frame=",    "[ninterp] tasks=", "[ndrops] ",          "[timgmemo] hit=",
    "[tmemskip] lvl=",   "[hudd] tasks=",      "[hudop] tasks=",   "[hud] frames=",      "[hsx] tasks=",
    "[dressd] tasks=",   "[dlmemo] tasks=",    "[deadset] tasks=", "[dynlod] auto=",     "[texskip] level=",
    "[ldmemo] level=",   "[native] machines=", "[native] meshlife", "[gpuxf] draws",     "[rr] machines",
    "[speck] machines_speck=", "[mattab] level=", "[mattab] keymiss", "[mattab] vfy", "[mattab] order",
};
#define GDX3DS_FILELOG_THIN_KEYS ((int)(sizeof(kThinnedReceipts) / sizeof(kThinnedReceipts[0])))
static unsigned long sThinLastPass[GDX3DS_FILELOG_THIN_KEYS]; /* host frame + 1 (0 = never); sLock */
static unsigned long sThinPeriod = 0;  /* host iterations; 0 = no thinning (receipt_every 64) */
static volatile unsigned long sHostFrame = 0;

static void ConfigureReceiptCadence(void) {
    const int every = ClampInt(gdx3ds_config_get_int("debug", "receipt_every", GDX3DS_FILELOG_RECEIPT_BASE),
                               GDX3DS_FILELOG_RECEIPT_BASE, 4096);
    sReceiptEvery = every - every % GDX3DS_FILELOG_RECEIPT_BASE;
    sThinPeriod = 0;
    if (sReceiptEvery > GDX3DS_FILELOG_RECEIPT_BASE) {
        sThinPeriod = (unsigned long)(sReceiptEvery - GDX3DS_FILELOG_RECEIPT_BASE / 2);
    }
}

void gdx3ds_filelog_frame_end(unsigned long frame) {
    sHostFrame = frame;
}

int gdx3ds_filelog_receipt_every(void) {
    return sReceiptEvery;
}

static int ThinnedKeyOf(const char* msg, size_t len) {
    if (len < 2 || msg[0] != '[') {
        return -1;
    }
    for (int i = 0; i < GDX3DS_FILELOG_THIN_KEYS; i++) {
        const size_t keyLen = strlen(kThinnedReceipts[i]);
        if (len >= keyLen && memcmp(msg, kThinnedReceipts[i], keyLen) == 0) {
            return i;
        }
    }
    return -1;
}

/* Caller holds sLock. 1 = this receipt line is skipped (counted). */
static int ReceiptThinnedLocked(const char* msg, size_t len) {
    if (sThinPeriod == 0) {
        return 0;
    }
    const int key = ThinnedKeyOf(msg, len);
    if (key < 0) {
        return 0;
    }
    const unsigned long now = sHostFrame;
    if (sThinLastPass[key] != 0 && now - (sThinLastPass[key] - 1u) < sThinPeriod) {
        sStats.receiptsThinned++;
        return 1;
    }
    sThinLastPass[key] = now + 1u;
    return 0;
}

static void PublishFlushed(unsigned total) {
    __atomic_store_n(&sFlushedTotal, total, __ATOMIC_RELEASE);
}

/* N60LOG: the writer lets an in-flight audio-stream SD read go first (bounded). Returns 1 if it
 * waited. Only the writer thread calls this: a synchronous flush never sleeps. */
static int YieldToAudioRead(void) {
    int waitedMs = 0;
    while (gdx3ds_hitch_aud_inflight && waitedMs < GDX3DS_FILELOG_AUDIO_YIELD_MAX_MS) {
        svcSleepThread(1000000LL);
        waitedMs++;
    }
    return waitedMs > 0;
}

/* Raw write of n contiguous bytes to the file's descriptor (the stdio buffer is empty: every pass
 * ends in fflush, and WriteRingSpan flushes first). One FSFILE_Write per call instead of stdio's
 * buffer-sized pieces. */
static void WriteRaw(const char* p, unsigned n) {
    const int fd = fileno(sLogFile);
    while (n > 0) {
        const ssize_t w = write(fd, p, n);
        if (w <= 0) {
            return;
        }
        p += w;
        n -= (unsigned)w;
    }
}

/* Contiguous bytes, chunked; the writer yields to audio reads between chunks. */
static void WriteContiguous(const char* p, unsigned n, int yieldToAudio) {
    while (n > 0) {
        const unsigned chunk = n < GDX3DS_FILELOG_WRITE_CHUNK ? n : GDX3DS_FILELOG_WRITE_CHUNK;
        if (yieldToAudio && YieldToAudioRead()) {
            sStats.audioYields++;
        }
        WriteRaw(p, chunk);
        p += chunk;
        n -= chunk;
    }
}

/* Write ring bytes [from, to) to the SD file. Caller owns the file (sLock in sync mode,
 * sFileLock in async mode) and has checked sLogFile. */
static void WriteRingSpan(unsigned from, unsigned to, int yieldToAudio) {
    unsigned pending = to - from;
    unsigned pos = from % GDX3DS_FILELOG_RING_CAP;
    unsigned first = GDX3DS_FILELOG_RING_CAP - pos;
    if (first > pending) {
        first = pending;
    }
    fflush(sLogFile); /* keep the descriptor's offset where stdio left it */
    WriteContiguous(sRing + pos, first, yieldToAudio);
    if (pending > first) {
        WriteContiguous(sRing, pending - first, yieldToAudio);
    }
}

/* o3ds-cpucuts: who asked for the next synchronous flush ([hitch] src=). Set under sLock right
 * before a synchronous path flushes; published into sStats by the flush itself. */
static const char* sNextSyncSrc = NULL;
static uint32_t sNextSyncRa = 0;

static void NoteSyncSrc(const char* src, uint32_t ra) {
    sNextSyncSrc = src;
    sNextSyncRa = ra;
}

/* Mirror the pending span [sFlushedTotal, sTotal) to the SD file. Caller holds sLock
 * and has checked sLogFile. */
static void gdx3ds_filelog_flush_locked(void) {
    const u64 t0 = svcGetSystemTick();
    unsigned pending = sTotal - FlushedTotal();
    if (pending > 0) {
        WriteRingSpan(FlushedTotal(), sTotal, 0);
        PublishFlushed(sTotal);
    }
    if (sDroppedLines > 0) {
        char note[96];
        int n = snprintf(note, sizeof(note), "[filelog] dropped=%lu total=%lu\n",
                         sDroppedLines, sDroppedTotal);
        if (n > 0) {
            fwrite(note, 1, (size_t)n, sLogFile);
            sBytesWritten += (size_t)n; /* report lines count against the cap too */
        }
        sDroppedLines = 0;
    }
    fflush(sLogFile); /* one SD flush per batch instead of per line */
    if (sDurable) {
        fsync(fileno(sLogFile));
    }
    sLastFlushTick = svcGetSystemTick();
    sStats.syncFlushes++;
    sStats.syncBytes += pending;
    sStats.syncTicks += sLastFlushTick - t0;
    sStats.lastSyncSrc = sNextSyncSrc != NULL ? sNextSyncSrc : "other";
    sStats.lastSyncRa = sNextSyncRa;
    sNextSyncSrc = NULL;
    sNextSyncRa = 0;
}

static int OnWriterThread(void) {
    u32 id = 0;
    if (sWriterThread == NULL) {
        return 0;
    }
    svcGetThreadId(&id, CUR_THREAD_HANDLE);
    return id == sWriterThreadId;
}

/* Synchronous flush that is safe beside the async writer: wait for an in-flight write
 * (sFileLock), then write everything pending. Caller holds sLock. A fatal line raised ON
 * the writer thread (it holds sFileLock) must not self-deadlock: it writes directly. */
static void FlushSyncLocked(void) {
    const int lockFile = sAsync && !OnWriterThread();
    if (lockFile) {
        sStats.syncWaits += sWriterBusy ? 1u : 0u;
        LightLock_Lock(&sFileLock);
    }
    if (sLogFile != NULL) {
        gdx3ds_filelog_flush_locked();
    }
    if (lockFile) {
        LightLock_Unlock(&sFileLock);
    }
}

/* Flush + ledger: the batch flush is the SD-card half of the logging cost. */
static void gdx3ds_filelog_flush_counted_locked(void) {
    const u64 t0 = sCostArmed ? svcGetSystemTick() : 0;
    FlushSyncLocked();
    if (sCostArmed) {
        sCostFlushTicks += svcGetSystemTick() - t0;
        sCostFlushes++;
    }
}

void gdx3ds_filelog_set_session_path(const char* path) {
    snprintf(sSessionPath, sizeof(sSessionPath), "%s", path != NULL ? path : "");
}

void gdx3ds_filelog_set_tag(const char* tag) {
    if (tag == NULL || tag[0] == '\0') {
        sTagPath[0] = '\0';
        sTagPrevPath[0] = '\0';
        return;
    }
    snprintf(sTagPath, sizeof(sTagPath), "sdmc:/3ds/gdiffuser/log-%s.txt", tag);
    snprintf(sTagPrevPath, sizeof(sTagPrevPath), "sdmc:/3ds/gdiffuser/log-%s-prev.txt", tag);
}

void gdx3ds_filelog_set_mirror_history(int on) {
    sMirrorHistory = on ? 1 : 0;
}

/* The oldest ring byte still held: everything when the ring never wrapped. Caller holds sLock. */
static unsigned OldestRetainedLocked(void) {
    return sTotal > GDX3DS_FILELOG_RING_CAP ? sTotal - GDX3DS_FILELOG_RING_CAP : 0u;
}

void gdx3ds_filelog_set_durable(int on) {
    sDurable = on ? 1 : 0;
    if (on) {
        gdx3ds_filelog_flush(); /* what the ring holds now reaches the card first */
    }
}

void gdx3ds_filelog_sync_for_ms(unsigned ms) {
    if (ms == 0) {
        return;
    }
    const u64 until = svcGetSystemTick() + (u64)ms * (u64)CPU_TICKS_PER_MSEC;
    if (until > sSyncUntilTick) {
        sSyncUntilTick = until;
    }
    gdx3ds_filelog_flush(); /* what the ring holds now reaches the card first */
}

void gdx3ds_filelog_sync_end(void) {
    sSyncUntilTick = 0;
}

static FILE* gdx3ds_filelog_open_session(void) {
    if (sSessionPath[0] != '\0') {
        return fopen(sSessionPath, "a"); /* one file per boot: no rotation, never truncated */
    }
    if (sTagPath[0] != '\0') {
        remove(sTagPrevPath); /* a launch variant: its own file, one previous run kept */
        rename(sTagPath, sTagPrevPath);
        return fopen(sTagPath, "w");
    }
    /* Keep the previous TWO sessions: an A/B across a reboot must not lose its first half,
     * and a bisect card runs one title up to three times per card pull (REL1CRASH). */
    remove(GDX3DS_FILELOG_PREV2_PATH);
    rename(GDX3DS_FILELOG_PREV_PATH, GDX3DS_FILELOG_PREV2_PATH);
    rename(GDX3DS_FILELOG_PATH, GDX3DS_FILELOG_PREV_PATH);
    return fopen(GDX3DS_FILELOG_PATH, "w");
}

/* The writer's one pass: snapshot the pending span (and the drop report) under the ring
 * lock, then write it holding only sFileLock. A synchronous flush that ran in between may
 * already have written part or all of it: only what is still pending is written. */
/* N60LOG: the async size cap. The writer mirrors up to sCapAt, then the marker, then closes; the
 * caller that hit the cap never touches the card. Caller (the writer) holds sFileLock. */
static void WriterCloseAtCap(void) {
    static const char kTrunc[] = "[filelog] size cap reached; logging stopped\n";
    if (!sCapPending || sLogFile == NULL || (int)(FlushedTotal() - sCapAt) < 0) {
        return;
    }
    fwrite(kTrunc, 1, sizeof(kTrunc) - 1, sLogFile);
    fclose(sLogFile);
    sLogFile = NULL;
}

/* TEST ONLY ([debug] filelog_stall_ms): a slow SD card, inside the in-flight write. */
static void WriterStallForTest(void) {
    if (sStallMs > 0) {
        svcSleepThread((s64)sStallMs * 1000000LL);
    }
}

/* The span the writer may mirror: everything, or up to the async size cap. Caller holds sLock. */
static unsigned MirrorEndLocked(void) {
    return sCapPending ? sCapAt : sTotal;
}

static void WriterFlushOnce(void) {
    unsigned end;
    unsigned long dropped;
    unsigned long droppedTotal;
    u64 t0;
    u64 dt;
    unsigned from;
    RecursiveLock_Lock(&sLock);
    end = MirrorEndLocked();
    dropped = sDroppedLines;
    droppedTotal = sDroppedTotal;
    sDroppedLines = 0;
    RecursiveLock_Unlock(&sLock);
    LightLock_Lock(&sFileLock);
    from = FlushedTotal();
    if (sLogFile == NULL || ((int)(end - from) <= 0 && dropped == 0)) {
        WriterCloseAtCap();
        LightLock_Unlock(&sFileLock);
        return;
    }
    sWriterBusy = 1;
    t0 = svcGetSystemTick();
    WriterStallForTest();
    if ((int)(end - from) > 0) {
        WriteRingSpan(from, end, 1);
        PublishFlushed(end);
    }
    if (dropped > 0) {
        char note[96];
        int n = snprintf(note, sizeof(note), "[filelog] dropped=%lu total=%lu\n", dropped, droppedTotal);
        if (n > 0) {
            fwrite(note, 1, (size_t)n, sLogFile);
        }
    }
    fflush(sLogFile);
    dt = svcGetSystemTick() - t0;
    sWriterBusy = 0;
    sStats.asyncWrites++;
    sStats.asyncBytes += (int)(end - from) > 0 ? (end - from) : 0u;
    sStats.asyncTicks += dt;
    if (dt > sStats.asyncMaxTicks) {
        sStats.asyncMaxTicks = dt;
    }
    WriterCloseAtCap();
    LightLock_Unlock(&sFileLock);
}

static int RaceOnScreen(void) {
    return &gdx_input_in_gameplay != NULL && gdx_input_in_gameplay() != 0;
}

/* Racy reads of sTotal / sFlushedTotal are fine: a stale value only moves one deferral by a pass. */
static int WriterShouldDefer(void) {
    if (!sRaceQuiet || sCapPending || !RaceOnScreen()) {
        return 0;
    }
    return (sTotal - FlushedTotal()) < GDX3DS_FILELOG_RACE_QUIET_MARK;
}

static void WriterThreadMain(void* arg) {
    (void)arg;
    svcGetThreadId((u32*)&sWriterThreadId, CUR_THREAD_HANDLE);
    for (;;) {
        LightEvent_WaitTimeout(&sWriterWake, (s64)sAsyncMs * 1000000LL);
        if (WriterShouldDefer()) {
            sStats.asyncDeferred++;
            continue;
        }
        WriterFlushOnce();
    }
}

/* [debug] filelog_async=1: start the writer. Any failure leaves the synchronous batch path. */
static void StartAsyncWriter(void) {
    int ms = gdx3ds_config_get_int("debug", "filelog_async_ms", (int)GDX3DS_FILELOG_ASYNC_MS_DEFAULT);
    if (ms < 50) {
        ms = 50;
    } else if (ms > 5000) {
        ms = 5000;
    }
    sAsyncMs = (unsigned)ms;
    LightLock_Init(&sFileLock);
    LightEvent_Init(&sWriterWake, RESET_ONESHOT);
    sWriterThread = threadCreate(WriterThreadMain, NULL, GDX3DS_FILELOG_WRITER_STACK, GDX3DS_FILELOG_WRITER_PRIO,
                                 0, true);
    sAsync = sWriterThread != NULL;
    sRaceQuiet = sAsync && gdx3ds_config_get_bool("debug", "filelog_race_quiet", 0);
    sDropFull = gdx3ds_config_get_bool("debug", "filelog_drop_full", 1);
    sStallMs = (unsigned)ClampInt(gdx3ds_config_get_int("debug", "filelog_stall_ms", 0), 0, 30000);
}

void gdx3ds_filelog_init(void) {
    gdx3ds_filelog_ensure_lock();
    ConfigureReceiptCadence(); /* the ring (LOG tab) is thinned too, so before the SD check */
    if (!gdx3ds_config_get_bool("debug", "filelog", 0)) {
        return; /* retention stays on; only the SD mirror is disabled */
    }
    sFlushDefer = gdx3ds_cpucut_clamp(gdx3ds_config_get_int("debug", "filelog_flush_defer", 1), 1);
    gdx3ds_cpucut_lever("logdefer", &sFlushDefer, GDX3DS_CPUCUT_BIT_LOGDEFER);
    int maxKb = gdx3ds_config_get_int("debug", "filelog_max_kb", GDX3DS_FILELOG_DEFAULT_MAX_KB);
    if (maxKb < 1) {
        maxKb = 1;
    } else if (maxKb > 8192) {
        maxKb = 8192;
    }
    RecursiveLock_Lock(&sLock);
    sLogFile = gdx3ds_filelog_open_session();
    if (sLogFile == NULL) {
        RecursiveLock_Unlock(&sLock);
        return; /* no SD / write-protected: stay disabled, tracers keep their svc path */
    }
    setvbuf(sLogFile, sStreamBuf, _IOFBF, sizeof(sStreamBuf));
    sByteCap = (size_t)maxKb * 1024u;
    sBytesWritten = 0;
    {
        int reserveKb = gdx3ds_config_get_int("debug", "filelog_mp_reserve_kb", maxKb / 4);
        if (reserveKb < 0) {
            reserveKb = 0;
        } else if (reserveKb > maxKb / 2) {
            reserveKb = maxKb / 2;
        }
        sMpReserve = (size_t)reserveKb * 1024u;
    }
    /* Boot lines appended before init (config load, window init) are already in the
     * retained ring: start the SD mirror from here, not from history — the old batch
     * design also only captured post-init lines. */
    sFlushedTotal = sMirrorHistory ? OldestRetainedLocked() : sTotal;
    sDroppedLines = 0;
    sDroppedTotal = 0;
    sLastFlushTick = svcGetSystemTick();
    int syncMs = gdx3ds_config_get_int("debug", "filelog_sync_ms", 0);
    if (syncMs > 0) {
        sSyncUntilTick = sLastFlushTick + (u64)syncMs * (u64)CPU_TICKS_PER_MSEC;
    }
    RecursiveLock_Unlock(&sLock);
    if (gdx3ds_config_get_bool("debug", "filelog_async", 0)) {
        StartAsyncWriter();
        char note[320];
        int n = snprintf(note, sizeof(note),
                         "[filelog] async=%d writer prio=0x%X core=0 every=%u ms kick=%u KiB race_quiet=%d "
                         "(hiwat %u KiB) drop_full=%d chunk=%u KiB audio_yield<=%d ms stall=%u ms receipt_every=%d "
                         "(debug.filelog_async=1)%s",
                         sAsync, GDX3DS_FILELOG_WRITER_PRIO, sAsyncMs, GDX3DS_FILELOG_ASYNC_KICK / 1024u, sRaceQuiet,
                         GDX3DS_FILELOG_RACE_QUIET_MARK / 1024u, sDropFull, GDX3DS_FILELOG_WRITE_CHUNK / 1024u,
                         GDX3DS_FILELOG_AUDIO_YIELD_MAX_MS, sStallMs, sReceiptEvery,
                         sAsync ? "" : ": threadCreate failed, synchronous batch flush");
        if (n > 0) {
            gdx3ds_filelog_write(note, (size_t)n);
        }
    }
    if (syncMs > 0) {
        char note[96];
        int n = snprintf(note, sizeof(note), "[filelog] sync every line for the first %d ms "
                         "(debug.filelog_sync_ms)", syncMs);
        if (n > 0) {
            gdx3ds_filelog_write(note, (size_t)n);
        }
    }
}

/* Append len bytes + '\n' into the circular ring. Caller holds sLock and has verified
 * len + 1 <= GDX3DS_FILELOG_RING_CAP. */
static void gdx3ds_filelog_ring_append_locked(const char* msg, size_t len) {
    unsigned pos = sTotal % GDX3DS_FILELOG_RING_CAP;
    unsigned first = GDX3DS_FILELOG_RING_CAP - pos;
    if (first > len) {
        first = (unsigned)len;
    }
    memcpy(sRing + pos, msg, first);
    if (len > first) {
        memcpy(sRing, msg + first, len - first);
    }
    sRing[(pos + len) % GDX3DS_FILELOG_RING_CAP] = '\n';
    __atomic_store_n(&sTotal, sTotal + (unsigned)len + 1u, __ATOMIC_RELEASE);
    sStats.lines++;
    sStats.bytes += (unsigned)len + 1u;
}

static void gdx3ds_filelog_write_locked(const char* msg, size_t len);

/* The size cap: pending batch first, in order, then the marker, then close. Async: the
 * writer is excluded for the whole sequence and sees sLogFile == NULL afterwards. */
static void gdx3ds_filelog_close_capped_locked(void) {
    static const char kTrunc[] = "[filelog] size cap reached; logging stopped\n";
    const int lockFile = sAsync && !OnWriterThread();
    gdx3ds_filelog_flush_counted_locked();
    if (lockFile) {
        LightLock_Lock(&sFileLock);
    }
    if (sLogFile != NULL) {
        fwrite(kTrunc, 1, sizeof(kTrunc) - 1, sLogFile);
        fclose(sLogFile); /* flushes; frees the fd for the rest of the run */
        sLogFile = NULL;
    }
    if (lockFile) {
        LightLock_Unlock(&sFileLock);
    }
}

static int gdx3ds_filelog_caller_slot(void) {
    if (&gdx_sched_on_host_thread != NULL && gdx_sched_on_host_thread()) {
        return 0;
    }
    return 1;
}

void gdx3ds_filelog_write(const char* msg, size_t len) {
    if (msg == NULL) {
        return;
    }
    gdx3ds_filelog_ensure_lock();
    if (!sCostArmed) {
        RecursiveLock_Lock(&sLock);
        gdx3ds_filelog_write_locked(msg, len);
        RecursiveLock_Unlock(&sLock);
        return;
    }
    const int slot = gdx3ds_filelog_caller_slot();
    const u64 t0 = svcGetSystemTick();
    RecursiveLock_Lock(&sLock);
    gdx3ds_filelog_write_locked(msg, len);
    sCostTicks[slot] += svcGetSystemTick() - t0;
    sCostLines[slot]++;
    RecursiveLock_Unlock(&sLock);
}

/* Async: wake the writer at the kick mark; the writer's own timeout covers the trickle. Under
 * filelog_race_quiet in a race the mark is the one the writer would defer to anyway, so it is not
 * woken once per line just to defer again. */
static void KickWriterIfDue(void) {
    const unsigned mark = (sRaceQuiet && RaceOnScreen()) ? GDX3DS_FILELOG_RACE_QUIET_MARK : GDX3DS_FILELOG_ASYNC_KICK;
    if ((sTotal - FlushedTotal()) >= mark && !sWriterBusy) {
        LightEvent_Signal(&sWriterWake);
    }
}

/* "[alloc-fail" and "[crash" are fatal-class too: both usually precede a death by microseconds,
 * and a batched line dies with the process (REL1CRASH). */
static int IsFatalClassLine(const char* msg, size_t len) {
    return (len >= 6 && memcmp(msg, "[fatal", 6) == 0) || (len >= 11 && memcmp(msg, "[alloc-fail", 11) == 0) ||
           (len >= 6 && memcmp(msg, "[crash", 6) == 0);
}

/* Fatal-class lines must be on the card before svcBreak; the filelog_sync_ms window and the
 * n60hwdiag durable mode ask for every line on the card. These stay synchronous. */
static int MustReachCardNow(int fatalLine) {
    return fatalLine || sDurable || svcGetSystemTick() < sSyncUntilTick;
}

static int RingWouldOverwriteUnflushed(size_t len) {
    return (sTotal - FlushedTotal()) + len + 1 > GDX3DS_FILELOG_RING_CAP;
}

static void CountDroppedLineLocked(void) {
    sDroppedLines++;
    sDroppedTotal++;
}

/* The size cap. Async (off the writer): the writer mirrors up to here, then closes -- the caller
 * never waits on the card. Synchronous mode: the historic flush + marker + close. */
static void ApplySizeCapLocked(void) {
    if (sAsync && !OnWriterThread()) {
        sCapPending = 1;
        sCapAt = sTotal;
        LightEvent_Signal(&sWriterWake);
        return;
    }
    gdx3ds_filelog_close_capped_locked();
}

/* Room in the ring for len + 1 bytes without losing an unflushed byte. Returns 0 when the line
 * is dropped instead (async + filelog_drop_full: the caller never waits for the writer). */
static int MakeRoomLocked(size_t len, int fatalLine) {
    if (sLogFile == NULL) {
        return 1;
    }
    if (!sCapPending && sBytesWritten + len + 1 > sByteCap) {
        ApplySizeCapLocked();
    }
    if (sLogFile == NULL || !RingWouldOverwriteUnflushed(len)) {
        return 1;
    }
    if (sAsync && sDropFull && !MustReachCardNow(fatalLine)) {
        sStats.ringFullDrops++;
        CountDroppedLineLocked();
        if (!sWriterBusy) {
            LightEvent_Signal(&sWriterWake);
        }
        return 0;
    }
    /* Synchronous mode, or a line that must reach the card: mirror the backlog first (async: wait
     * for the writer, then write the rest). */
    sStats.ringFullFlushes++;
    NoteSyncSrc("ringfull", 0);
    gdx3ds_filelog_flush_counted_locked();
    return 1;
}

/* The SD batch policy for the line just appended. */
static void MirrorPolicyLocked(size_t len, int fatalLine) {
    /* (CPU_TICKS_PER_MSEC is a double in libctru; bake an integer budget.) */
    static const u64 kFlushTicks = (u64)(GDX3DS_FILELOG_FLUSH_MS * CPU_TICKS_PER_MSEC);
    if (sLogFile == NULL) {
        return;
    }
    if (!sCapPending) {
        sBytesWritten += len + 1;
    }
    if (MustReachCardNow(fatalLine)) {
        NoteSyncSrc(fatalLine ? "fatal" : "durable", 0);
        gdx3ds_filelog_flush_counted_locked();
    } else if (sAsync) {
        if (!sCapPending) {
            KickWriterIfDue();
        }
    } else if ((sTotal - FlushedTotal()) >= GDX3DS_FILELOG_RING_HIWAT ||
               (svcGetSystemTick() - sLastFlushTick) >= kFlushTicks) {
        NoteSyncSrc("batch", 0);
        gdx3ds_filelog_flush_counted_locked();
    }
}

/* One line into the ring (+ the SD batch policy). Caller holds sLock. */
/* The lines the MP reserve keeps: the online / LAN receipts a race diagnosis needs. */
static int IsMpReceiptLine(const char* msg, size_t len) {
    static const char* const kKeep[] = { "[mpo]", "[mpipc]", "[mpcost]", "[mprof]", "[mprate]", "[mpradio]",
                                         "[socthr]", "[net]", "[watchdog]", "[apt]",
                                         "[trkres] arena" /* ship6: a skipped course bake */ };
    size_t i;
    for (i = 0; i < sizeof(kKeep) / sizeof(kKeep[0]); i++) {
        const size_t n = strlen(kKeep[i]);
        if (len >= n && memcmp(msg, kKeep[i], n) == 0) {
            return 1;
        }
    }
    return 0;
}

/* Past (cap - reserve) only MP receipt and fatal-class lines go to the card, so a long online
 * race keeps its [mpo] stats to the end (hardware online1: the New 3DS host log stopped at race
 * tick ~3000, before the dropout). 1 = drop this line from the SD mirror. */
static int OverMpBudgetLocked(const char* msg, size_t len, int fatalLine) {
    static const char kNote[] = "[filelog] general budget reached; only MP receipts and fatal lines from here "
                                "(debug.filelog_mp_reserve_kb)";
    if (!sMpSession || sMpReserve == 0 || sLogFile == NULL || sCapPending || fatalLine) {
        return 0;
    }
    if (sBytesWritten + len + 1 + sMpReserve <= sByteCap || IsMpReceiptLine(msg, len)) {
        return 0;
    }
    if (!sMpBudgetNoted) {
        sMpBudgetNoted = 1;
        if (MakeRoomLocked(sizeof(kNote) - 1, 0)) {
            gdx3ds_filelog_ring_append_locked(kNote, sizeof(kNote) - 1);
            MirrorPolicyLocked(sizeof(kNote) - 1, 0);
        }
    }
    sMpBudgetDrops++;
    return 1;
}

void gdx3ds_filelog_set_mp_session(int on) {
    sMpSession = on ? 1 : 0;
}

static void gdx3ds_filelog_write_locked(const char* msg, size_t len) {
    if (len + 1 > GDX3DS_FILELOG_RING_CAP) {
        /* A single line larger than the whole ring: bounded drop, counted — the hard
         * cap is the ring; nothing is ever queued beyond it. */
        CountDroppedLineLocked();
        return;
    }
    if (ReceiptThinnedLocked(msg, len)) {
        return;
    }
    const int fatalLine = IsFatalClassLine(msg, len);
    if (OverMpBudgetLocked(msg, len, fatalLine)) {
        gdx3ds_net_log_write(msg, len); /* the live link still sees it */
        return;
    }
    if (!MakeRoomLocked(len, fatalLine)) {
        return;
    }
    gdx3ds_filelog_ring_append_locked(msg, len);
    gdx3ds_net_log_write(msg, len); /* HWLINK mirror; no-op when [net] enabled = 0 */
    MirrorPolicyLocked(len, fatalLine);
}

void gdx3ds_filelog_cost_arm(int on) {
    sCostArmed = on ? 1 : 0;
}

void gdx3ds_filelog_cost_totals(GdxFilelogCost* out) {
    if (out == NULL) {
        return;
    }
    gdx3ds_filelog_ensure_lock();
    RecursiveLock_Lock(&sLock);
    out->mainTicks = sCostTicks[0];
    out->otherTicks = sCostTicks[1];
    out->mainLines = sCostLines[0];
    out->otherLines = sCostLines[1];
    out->flushes = sCostFlushes;
    out->flushTicks = sCostFlushTicks;
    RecursiveLock_Unlock(&sLock);
}

void gdx3ds_filelog_flush_sync(void) {
    const uint32_t ra = (uint32_t)(uintptr_t)__builtin_return_address(0);
    if (sLogFile == NULL) {
        return;
    }
    gdx3ds_filelog_ensure_lock();
    RecursiveLock_Lock(&sLock);
    NoteSyncSrc("flush_sync", ra);
    FlushSyncLocked();
    RecursiveLock_Unlock(&sLock);
}

static volatile int sRaceDefer = 0;

void gdx3ds_filelog_set_race_defer(int on) {
    sRaceDefer = on ? 1 : 0;
}

int gdx3ds_filelog_flush_lever(void) {
    return gdx3ds_cpucut_on(sFlushDefer, GDX3DS_CPUCUT_BIT_LOGDEFER);
}

int gdx3ds_filelog_flush_defer_on(void) {
    return sAsync && gdx3ds_filelog_flush_lever() && RaceOnScreen();
}

/* A flush the race defers: the async writer is up, nothing asks for synchronous lines. */
static int FlushDeferredLocked(void) {
    int byLever;
    if (!sAsync || OnWriterThread() || MustReachCardNow(0)) {
        return 0;
    }
    byLever = !sRaceDefer && gdx3ds_filelog_flush_defer_on();
    if (!sRaceDefer && !byLever) {
        return 0;
    }
    sStats.raceDeferred++;
    if (byLever) {
        sStats.flushDeferred++;
    }
    if (!sWriterBusy) {
        LightEvent_Signal(&sWriterWake);
    }
    return 1;
}

static void FlushFrom(const char* src, uint32_t ra) {
    if (sLogFile == NULL) {
        return;
    }
    gdx3ds_filelog_ensure_lock();
    RecursiveLock_Lock(&sLock);
    if (!FlushDeferredLocked()) {
        NoteSyncSrc(src, ra);
        FlushSyncLocked();
    }
    RecursiveLock_Unlock(&sLock);
}

void gdx3ds_filelog_flush(void) {
    FlushFrom("flush", (uint32_t)(uintptr_t)__builtin_return_address(0));
}

void gdx3ds_filelog_flush_src(const char* src) {
    FlushFrom(src != NULL ? src : "flush", (uint32_t)(uintptr_t)__builtin_return_address(0));
}

void gdx3ds_filelog_stats(GdxFilelogStats* out) {
    if (out == NULL) {
        return;
    }
    *out = sStats; /* torn reads of a counter are acceptable in a diagnostic */
    out->async = sAsync;
    out->writerBusy = sWriterBusy;
    out->pending = sTotal - FlushedTotal();
}

/* ---- MENU LOG-tab reader ---------------------------------------------------------
 * Copies up to maxLines '\n'-terminated lines from the retained tail into out (an
 * array of maxLines records of lineCap bytes each, oldest first), skipping the
 * skipNewest most recent lines (scroll offset). Long lines are truncated to
 * lineCap-1; the oldest reachable line may have a garbage prefix when the ring has
 * wrapped mid-line (accepted). Returns the number of lines written. Any thread. */
int gdx3ds_filelog_tail_lines(unsigned skipNewest, char* out, int maxLines, int lineCap) {
    if (out == NULL || maxLines <= 0 || lineCap < 2) {
        return 0;
    }
    gdx3ds_filelog_ensure_lock();
    RecursiveLock_Lock(&sLock);
    unsigned avail = sTotal < GDX3DS_FILELOG_RING_CAP ? sTotal : GDX3DS_FILELOG_RING_CAP;
    /* Walk backwards over the retained span collecting [start, end) offsets of the
     * last (skipNewest + maxLines) lines. Offsets are monotonic byte positions. */
    unsigned end = sTotal;   /* exclusive; every append is '\n'-terminated */
    int found = 0;           /* complete lines located so far (newest first) */
    static unsigned lineEnd[256];   /* newest-first located line bounds (static: this
                                     * sink must never touch the stack deeply or heap) */
    static unsigned lineStart[256]; /* guarded by sLock like the ring itself */
    int want = (int)skipNewest + maxLines;
    if (want > 256) {
        want = 256; /* bounded scratch; covers the menu's max scroll depth */
    }
    unsigned pos = end;
    unsigned low = sTotal - avail;
    while (pos > low && found < want) {
        /* pos is just past a '\n'; step over it, then scan back to the previous one. */
        unsigned scan = pos - 1; /* the terminating '\n' of this line */
        unsigned start = scan;
        while (start > low && sRing[(start - 1) % GDX3DS_FILELOG_RING_CAP] != '\n') {
            start--;
        }
        lineStart[found] = start;
        lineEnd[found] = scan; /* exclusive of '\n' */
        found++;
        pos = start;
    }
    int emitted = 0;
    /* Emit oldest-first among the window [skipNewest, skipNewest+maxLines);
     * newest-first index i must stay >= skipNewest (scrolled past the top: 0 lines). */
    int firstIdx = found - 1; /* oldest located */
    for (int i = firstIdx; i >= (int)skipNewest; i--) {
        unsigned len = lineEnd[i] - lineStart[i];
        if ((int)len > lineCap - 1) {
            len = (unsigned)(lineCap - 1);
        }
        char* dst = out + (size_t)emitted * (size_t)lineCap;
        for (unsigned b = 0; b < len; b++) {
            dst[b] = sRing[(lineStart[i] + b) % GDX3DS_FILELOG_RING_CAP];
        }
        dst[len] = '\0';
        emitted++;
        if (emitted >= maxLines) {
            break;
        }
    }
    RecursiveLock_Unlock(&sLock);
    return emitted;
}

/* REL1CRASH (gdx3ds_crash.c): copy the retained ring's newest bytes, WITHOUT the lock -- the
 * caller is a CPU exception handler that may have interrupted a writer holding it. A torn
 * line at the start or end is accepted. *pendingOut = bytes not yet mirrored to the SD file,
 * i.e. the lines the crash would otherwise have taken with it. Returns bytes copied. */
size_t gdx3ds_filelog_crash_tail(char* out, size_t cap, unsigned* pendingOut) {
    const unsigned total = sTotal;
    const unsigned avail = total < GDX3DS_FILELOG_RING_CAP ? total : GDX3DS_FILELOG_RING_CAP;
    const unsigned want = (unsigned)cap < avail ? (unsigned)cap : avail;
    const unsigned start = total - want;
    for (unsigned i = 0; i < want; i++) {
        out[i] = sRing[(start + i) % GDX3DS_FILELOG_RING_CAP];
    }
    if (pendingOut != NULL) {
        *pendingOut = total - FlushedTotal();
    }
    return want;
}
