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

#include <stdio.h>
#include <string.h>

#include <3ds.h>

#define GDX3DS_FILELOG_PATH "sdmc:/3ds/gdiffuser/log.txt"
#define GDX3DS_FILELOG_PREV_PATH "sdmc:/3ds/gdiffuser/log-prev.txt"
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

static void gdx3ds_filelog_ensure_lock(void) {
    if (!sLockInited) {
        RecursiveLock_Init(&sLock);
        sLockInited = 1;
    }
}

/* Mirror the pending span [sFlushedTotal, sTotal) to the SD file. Caller holds sLock
 * and has checked sLogFile. */
static void gdx3ds_filelog_flush_locked(void) {
    unsigned pending = sTotal - sFlushedTotal;
    if (pending > 0) {
        unsigned pos = sFlushedTotal % GDX3DS_FILELOG_RING_CAP;
        unsigned first = GDX3DS_FILELOG_RING_CAP - pos;
        if (first > pending) {
            first = pending;
        }
        fwrite(sRing + pos, 1, first, sLogFile);
        if (pending > first) {
            fwrite(sRing, 1, pending - first, sLogFile);
        }
        sFlushedTotal = sTotal;
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
    sLastFlushTick = svcGetSystemTick();
}

void gdx3ds_filelog_init(void) {
    gdx3ds_filelog_ensure_lock();
    if (!gdx3ds_config_get_bool("debug", "filelog", 0)) {
        return; /* retention stays on; only the SD mirror is disabled */
    }
    int maxKb = gdx3ds_config_get_int("debug", "filelog_max_kb", GDX3DS_FILELOG_DEFAULT_MAX_KB);
    if (maxKb < 1) {
        maxKb = 1;
    } else if (maxKb > 8192) {
        maxKb = 8192;
    }
    RecursiveLock_Lock(&sLock);
    /* Keep the previous session: an A/B across a reboot must not lose its first half. */
    remove(GDX3DS_FILELOG_PREV_PATH);
    rename(GDX3DS_FILELOG_PATH, GDX3DS_FILELOG_PREV_PATH);
    sLogFile = fopen(GDX3DS_FILELOG_PATH, "w");
    if (sLogFile == NULL) {
        RecursiveLock_Unlock(&sLock);
        return; /* no SD / write-protected: stay disabled, tracers keep their svc path */
    }
    setvbuf(sLogFile, sStreamBuf, _IOFBF, sizeof(sStreamBuf));
    sByteCap = (size_t)maxKb * 1024u;
    sBytesWritten = 0;
    /* Boot lines appended before init (config load, window init) are already in the
     * retained ring: start the SD mirror from here, not from history — the old batch
     * design also only captured post-init lines. */
    sFlushedTotal = sTotal;
    sDroppedLines = 0;
    sDroppedTotal = 0;
    sLastFlushTick = svcGetSystemTick();
    RecursiveLock_Unlock(&sLock);
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
    sTotal += (unsigned)len + 1u;
}

void gdx3ds_filelog_write(const char* msg, size_t len) {
    if (msg == NULL) {
        return;
    }
    gdx3ds_filelog_ensure_lock();
    RecursiveLock_Lock(&sLock);
    if (len + 1 > GDX3DS_FILELOG_RING_CAP) {
        /* A single line larger than the whole ring: bounded drop, counted — the hard
         * cap is the ring; nothing is ever queued beyond it. */
        sDroppedLines++;
        sDroppedTotal++;
    } else {
        if (sLogFile != NULL) {
            if (sBytesWritten + len + 1 > sByteCap) {
                static const char kTrunc[] = "[filelog] size cap reached; logging stopped\n";
                gdx3ds_filelog_flush_locked(); /* pending batch first, in order */
                fwrite(kTrunc, 1, sizeof(kTrunc) - 1, sLogFile);
                fclose(sLogFile); /* flushes; frees the fd for the rest of the run */
                sLogFile = NULL;
            } else if ((sTotal - sFlushedTotal) + len + 1 > GDX3DS_FILELOG_RING_CAP) {
                /* The append would overwrite unflushed bytes: mirror them first. */
                gdx3ds_filelog_flush_locked();
            }
        }
        gdx3ds_filelog_ring_append_locked(msg, len);
        if (sLogFile != NULL) {
            sBytesWritten += len + 1;
            /* Fatal-class lines must be on the card before svcBreak: flush now.
             * Otherwise batch — flush at the high-water mark or on the time cadence.
             * (CPU_TICKS_PER_MSEC is a double in libctru; bake an integer budget.) */
            static const u64 kFlushTicks =
                (u64)(GDX3DS_FILELOG_FLUSH_MS * CPU_TICKS_PER_MSEC);
            int fatalLine = (len >= 6 && memcmp(msg, "[fatal", 6) == 0);
            if (fatalLine || (sTotal - sFlushedTotal) >= GDX3DS_FILELOG_RING_HIWAT ||
                (svcGetSystemTick() - sLastFlushTick) >= kFlushTicks) {
                gdx3ds_filelog_flush_locked();
            }
        }
    }
    RecursiveLock_Unlock(&sLock);
}

void gdx3ds_filelog_flush(void) {
    if (sLogFile == NULL) {
        return;
    }
    gdx3ds_filelog_ensure_lock();
    RecursiveLock_Lock(&sLock);
    if (sLogFile != NULL) {
        gdx3ds_filelog_flush_locked();
    }
    RecursiveLock_Unlock(&sLock);
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
