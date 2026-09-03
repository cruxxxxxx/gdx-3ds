/* Single byte-source shim. See gdx_segment_source.h.
 *
 * Resolves an absolute ROM read archive-first via the generated segment_blob
 * table (port/gen/AssetBindings.c), falling back to the raw ROM image
 * (gdx_rom_buffer). Blob payloads are loaded once per family and cached for the
 * process lifetime (ROM data is read-only) -- there is NO allocation on the
 * per-frame read path, only on the first load of each family.
 *
 * Thread safety: the graphics thread, the game thread, and the dedicated audio
 * std::thread (via the osEPiStartDma DMA sink) all call the shim.
 * First-load of a family (the only mutating operation) is serialized under one
 * lock, mirroring n64_sched.c's gdx_mq_lock (SRWLOCK on Windows, pthread_mutex
 * elsewhere). Once a family's payload pointer is published it is immutable, so
 * the payload memcpy is done AFTER releasing the lock.
 */
#include "gdx_segment_source.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "port_log.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <intrin.h>
static SRWLOCK sSegLock = SRWLOCK_INIT;
static void seg_lock(void) { AcquireSRWLockExclusive(&sSegLock); }
static void seg_unlock(void) { ReleaseSRWLockExclusive(&sSegLock); }
/* Acquire/release ordering for the lock-free read fast path (below). The only
 * Windows target here is x86-64: aligned word loads/stores are already atomic and
 * the hardware memory model (TSO) never reorders load-load or store-store, so a
 * compiler barrier is all that is needed to realize acquire/release. This mirrors
 * n64_sched.c's Interlocked-based cross-thread style. */
#pragma intrinsic(_ReadWriteBarrier)
#define SEG_ACQUIRE_FENCE() _ReadWriteBarrier()
#define SEG_RELEASE_FENCE() _ReadWriteBarrier()
#elif defined(GDX_PLATFORM_3DS)
/* devkitARM has no pthread; libctru's LightLock is the same-shaped drop-in
 * (zero-init == unlocked, non-recursive), mirroring n64_sched.c's gdx_mq_lock. */
#include <3ds.h>
static LightLock sSegLock;
static void seg_lock(void) { LightLock_Lock(&sSegLock); }
static void seg_unlock(void) { LightLock_Unlock(&sSegLock); }
#define SEG_ACQUIRE_FENCE() __atomic_thread_fence(__ATOMIC_ACQUIRE)
#define SEG_RELEASE_FENCE() __atomic_thread_fence(__ATOMIC_RELEASE)
#else
#include <pthread.h>
#include <time.h>
#include <unistd.h>
static pthread_mutex_t sSegLock = PTHREAD_MUTEX_INITIALIZER;
static void seg_lock(void) { pthread_mutex_lock(&sSegLock); }
static void seg_unlock(void) { pthread_mutex_unlock(&sSegLock); }
/* GCC/Clang stand-alone fences give real acquire/release on every POSIX target
 * (including weakly-ordered ARM), keeping the fast path correct there too. */
#define SEG_ACQUIRE_FENCE() __atomic_thread_fence(__ATOMIC_ACQUIRE)
#define SEG_RELEASE_FENCE() __atomic_thread_fence(__ATOMIC_RELEASE)
#endif

/* 1 ms sleep for the "another thread is mid-load" wait in seg_ensure_loaded. Only ever
 * taken while a family's first (boot-window) load is in flight on another thread. */
static void seg_sleep_brief(void) {
#ifdef _WIN32
    Sleep(1);
#elif defined(GDX_PLATFORM_3DS)
    svcSleepThread(1000000LL);
#else
    usleep(1000);
#endif
}

/* Monotonic-ish millisecond clock for the load receipts. Also exported (see
 * gdx_segment_source.h) so the decomp audio driver's diag-gated [audio-load]
 * receipts can time loads without pulling a platform header into decomp code. */
double gdx_host_ms_now(void) {
#ifdef _WIN32
    return (double)GetTickCount64();
#elif defined(GDX_PLATFORM_3DS)
    return (double)svcGetSystemTick() / (double)CPU_TICKS_PER_MSEC;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
#endif
}

/* Generated blob table lookup (port/gen/AssetBindings.c, READ-only interface).
 * Containment semantics: returns the entry whose [rom_base, rom_base+size)
 * fully contains [rom_base_query, +size_needed), or NULL. */
typedef struct {
    unsigned int rom_base;
    unsigned int size;
    const char* o2r_key;
} GdxSegmentBlobEntry;
extern const GdxSegmentBlobEntry* gdx_lookup_segment_blob(unsigned int rom_base,
                                                          unsigned int size_needed);

/* Raw-ROM fallback source (port/rom_buffer.cpp). */
extern unsigned char* gdx_rom_buffer;
extern size_t gdx_rom_size;

/* Raw archive-file reader (port/AssetLoader.cpp): copies min(fileSize, outSize) bytes
 * of the o2r file INCLUDING its 0x40 Torch resource header, returns 1 on success. The
 * size_t parameters must stay spelled exactly as in the C++ definition, or this C
 * prototype disagrees with it across the extern "C" boundary. */
extern int GDiffuser_LoadArchiveFileBytes(const char* key, void* out, size_t outSize,
                                          size_t* copiedSize);

/* Archive entry framing: 0x40-byte Torch header, u32 little-endian
 * payload size at 0x40, verbatim ROM-slice payload begins at 0x44. */
#define GDX_ARCHIVE_HEADER_BYTES 0x40u
#define GDX_ARCHIVE_SIZE_FIELD_BYTES 0x04u
#define GDX_ARCHIVE_PAYLOAD_OFFSET (GDX_ARCHIVE_HEADER_BYTES + GDX_ARCHIVE_SIZE_FIELD_BYTES) /* 0x44 */

/* Per-family cache + telemetry slots, discovered lazily by blob-entry identity -- the
 * generated table is static, so entry pointers are stable identities. 128 leaves ample
 * headroom over the 22 venue/geometry families plus the audio_blob families this shim
 * also serves. A family beyond the cap still serves correct bytes through the raw-ROM
 * fallback; it is just not cached or tracked. */
#define GDX_SEG_FAMILY_MAX 128

typedef struct {
    const GdxSegmentBlobEntry* entry;  /* identity (stable table pointer) */
    unsigned char* volatile payload;   /* cached blob payload view (process lifetime);
                                        * lock-free read fast path publishes this last */
    volatile unsigned int payloadSize; /* usable payload bytes at `payload` */
    int loadState;                     /* 0 = unattempted, 1 = loaded, 2 = failed,
                                        * 3 = load in flight on some thread (the archive
                                        *     read runs OFF the lock; racing readers of the
                                        *     same family wait, other families proceed) */
    unsigned char* stagedBuf;          /* whole-file buffer pre-allocated by
                                        * GdxSegmentSourcePreallocPayload so a boot host can
                                        * host-range-register the payload view on the MAIN
                                        * thread before a background worker fills it; NULL =>
                                        * the loader mallocs on demand as before */
    unsigned int fallbackReads;        /* per-family raw-ROM fallbacks */
    int loggedFallback;                /* rate-limit: first fallback per family logs once */
} GdxSegFamilySlot;

static GdxSegFamilySlot sFamilies[GDX_SEG_FAMILY_MAX];
/* Monotonically increasing count of populated slots. Published with a release
 * fence AFTER a new slot is fully initialized so the lock-free read path can
 * iterate [0, sFamilyCount) without the lock and never observe a torn slot. */
static volatile unsigned int sFamilyCount;
static unsigned int sUnmappedFallback; /* fallbacks where no blob contains the read */
static int sUnmappedLogged;

/* Strict archive mode. GDX_STRICT_ARCHIVE (any non-empty, non-"0" value)
 * turns EVERY raw-ROM fallback into a logged defect -- the soak evidence collector.
 * It NEVER aborts (logged-defect semantics): a fallback still serves byte-identical
 * raw ROM. The per-family "log once" rate-limit is lifted to one line per 100
 * fallbacks, each carrying the running count. Read from the environment exactly
 * once (getenv is idempotent, so the first-read race is benign: every racer
 * computes the same value). */
static volatile int sStrictMode = -1; /* -1 = unread, 0 = off, 1 = on */
static int seg_strict_mode(void) {
    int m = sStrictMode;
    if (m < 0) {
        const char* e = getenv("GDX_STRICT_ARCHIVE");
        m = (e != NULL && e[0] != '\0' && e[0] != '0') ? 1 : 0;
        sStrictMode = m;
    }
    return m;
}

/* Find or create the slot for a blob entry. Caller holds sSegLock. */
static GdxSegFamilySlot* seg_slot_for(const GdxSegmentBlobEntry* entry) {
    unsigned int i;
    unsigned int count = sFamilyCount; /* holding the lock: plain read is fine */
    GdxSegFamilySlot* slot;
    for (i = 0; i < count; i++) {
        if (sFamilies[i].entry == entry) {
            return &sFamilies[i];
        }
    }
    if (count >= GDX_SEG_FAMILY_MAX) {
        return NULL;
    }
    /* Fully initialize the new slot BEFORE it becomes reachable to the lock-free
     * reader. The release fence + the subsequent count publication guarantee that
     * a reader which observes the incremented count also observes these stores, so
     * every slot in [0, sFamilyCount) is fully constructed (payload NULL until
     * loaded, which the fast path treats as "take the lock"). */
    slot = &sFamilies[count];
    slot->entry = entry;
    slot->payload = NULL;
    slot->payloadSize = 0;
    slot->loadState = 0;
    slot->stagedBuf = NULL;
    slot->fallbackReads = 0;
    slot->loggedFallback = 0;
    SEG_RELEASE_FENCE();
    sFamilyCount = count + 1;
    return slot;
}

/* Lazily load and cache a family's blob payload. Caller holds sSegLock; the lock is
 * DROPPED for the archive read itself (SD-bound, up to ~10.7 MB for audio_table on the
 * 3DS) and re-taken to publish, so one family's first load never serializes reads of
 * other families or the per-frame fast path. Returns 1 (holding the lock again) if the
 * payload is available, 0 to force a raw-ROM fallback. A failed load is sticky
 * (loadState 2) so a missing archive entry falls back once and then cheaply forever,
 * without re-hitting the resource manager per read. A caller racing an in-flight load of
 * the SAME family waits off-lock in 1 ms slices until the loader publishes. */
static int seg_ensure_loaded(GdxSegFamilySlot* slot) {
    const GdxSegmentBlobEntry* e;
    size_t cap;
    size_t copied;
    unsigned char* buf;
    unsigned int payloadSize;
    int staged;
    int ok;
    double t0;

    for (;;) {
        if (slot->loadState == 1) {
            return 1;
        }
        if (slot->loadState == 2) {
            return 0;
        }
        if (slot->loadState != 3) {
            break; /* unattempted: this thread becomes the loader */
        }
        /* Another thread is mid-load (boot preload worker vs first audio read, or two
         * lazy readers). Wait off-lock; the loader publishes 1 or 2 before clearing 3. */
        seg_unlock();
        seg_sleep_brief();
        seg_lock();
    }
    slot->loadState = 3;

    e = slot->entry;
    /* Whole-file buffer: header + size field + the full family payload span. If
     * the archive's real payload is larger than the table promises, the copy is
     * truncated below and we fall back (conservative, still byte-correct). A
     * pre-allocated staging buffer (GdxSegmentSourcePreallocPayload) is reused so its
     * already-registered host-range address stays live. */
    cap = (size_t)GDX_ARCHIVE_PAYLOAD_OFFSET + (size_t)e->size;
    staged = (slot->stagedBuf != NULL);
    buf = staged ? slot->stagedBuf : (unsigned char*)malloc(cap);
    if (buf == NULL) {
        slot->loadState = 2;
        return 0;
    }

    /* -- off the lock for the long read: other families keep loading/serving -- */
    seg_unlock();
    t0 = gdx_host_ms_now();
    copied = 0;
    ok = GDiffuser_LoadArchiveFileBytes(e->o2r_key, buf, cap, &copied) &&
         copied >= (size_t)GDX_ARCHIVE_PAYLOAD_OFFSET;

    payloadSize = 0;
    if (ok) {
        /* u32 little-endian payload size at 0x40. */
        payloadSize = (unsigned int)buf[GDX_ARCHIVE_HEADER_BYTES] |
                      ((unsigned int)buf[GDX_ARCHIVE_HEADER_BYTES + 1] << 8) |
                      ((unsigned int)buf[GDX_ARCHIVE_HEADER_BYTES + 2] << 16) |
                      ((unsigned int)buf[GDX_ARCHIVE_HEADER_BYTES + 3] << 24);

        /* The payload must be fully present and cover at least the span the table
         * promises (containment guarantees every served read fits inside e->size). */
        if (payloadSize < e->size ||
            (size_t)GDX_ARCHIVE_PAYLOAD_OFFSET + (size_t)payloadSize > copied) {
            ok = 0;
        }
    }
    seg_lock();

    if (!ok) {
        if (!staged) {
            free(buf);
        } /* a staged buffer may already be host-range-registered: keep it alive */
        slot->loadState = 2;
        return 0;
    }

    /* The whole file buffer stays alive (process-lifetime cache, never freed); the payload
     * view starts at +0x44. Publication order matters for the lock-free read fast path:
     * payloadSize and the copied bytes must settle BEFORE the release fence, with `payload`
     * published last, so a reader that acquire-loads a non-NULL payload also sees the
     * matching size and buffer contents. */
    slot->payloadSize = payloadSize;
    slot->loadState = 1;
    SEG_RELEASE_FENCE();
    slot->payload = buf + GDX_ARCHIVE_PAYLOAD_OFFSET;
    gdx_port_logf("[seg-src] blob loaded %s bytes=%u ms=%d", e->o2r_key, payloadSize,
                  (int)(gdx_host_ms_now() - t0));
    return 1;
}

int GdxSegmentSourceRead(uint32_t romBase, uint32_t size, void* dst) {
    const GdxSegmentBlobEntry* entry;
    const unsigned char* src = NULL;

    if (dst == NULL) {
        return 0;
    }

    entry = gdx_lookup_segment_blob((unsigned int)romBase, (unsigned int)size);
    if (entry != NULL) {
        /* Lock-free fast path: a published payload is immutable for the process lifetime,
         * so a read into an already-loaded family serves the copy without the lock and the
         * game and graphics threads never serialize on it. Only first-load and sticky-fail
         * families fall through to the lock below. */
        unsigned int count = sFamilyCount; /* aligned u32 load is atomic on the targets */
        unsigned int i;
        SEG_ACQUIRE_FENCE(); /* order the count load before the slot reads */
        for (i = 0; i < count; i++) {
            if (sFamilies[i].entry == entry) {
                unsigned char* published = sFamilies[i].payload;
                SEG_ACQUIRE_FENCE(); /* order the payload load before payload-derived reads */
                if (published != NULL) {
                    /* Containment (checked at lookup) guarantees
                     * (romBase - rom_base) + size <= payloadSize <= buffer length. */
                    memcpy(dst, published + ((unsigned int)romBase - entry->rom_base),
                           (size_t)size);
                    return 1;
                }
                break; /* slot exists but not yet loaded -> take the lock */
            }
        }

        seg_lock();
        {
            GdxSegFamilySlot* slot = seg_slot_for(entry);
            if (slot != NULL && seg_ensure_loaded(slot)) {
                /* Containment guarantees (romBase - rom_base) + size <= payloadSize. */
                src = slot->payload + ((unsigned int)romBase - entry->rom_base);
            } else if (slot != NULL) {
                slot->fallbackReads++;
                if (seg_strict_mode()) {
                    if ((slot->fallbackReads % 100u) == 1u) {
                        gdx_port_logf("[seg-src] STRICT: fallback family=%s rom=%X count=%u",
                                      entry->o2r_key, (unsigned int)romBase, slot->fallbackReads);
                    }
                } else if (!slot->loggedFallback) {
                    slot->loggedFallback = 1;
                    gdx_port_logf("[seg-src] fallback family=%s rom=%X", entry->o2r_key,
                                  (unsigned int)romBase);
                }
            }
        }
        seg_unlock();

        if (src != NULL) {
            /* Payload is immutable once published -- copy outside the lock so the
             * per-frame graphics path never serializes on a large texture memcpy. */
            memcpy(dst, src, (size_t)size);
            return 1;
        }
        /* Archive miss/short-read: fall through to the byte-identical raw ROM. */
    } else {
        seg_lock();
        sUnmappedFallback++;
        if (seg_strict_mode()) {
            if ((sUnmappedFallback % 100u) == 1u) {
                gdx_port_logf("[seg-src] STRICT: fallback family=<unmapped> rom=%X count=%u",
                              (unsigned int)romBase, sUnmappedFallback);
            }
        } else if (!sUnmappedLogged) {
            sUnmappedLogged = 1;
            gdx_port_logf("[seg-src] fallback family=<unmapped> rom=%X", (unsigned int)romBase);
        }
        seg_unlock();
    }

    /* Raw-ROM fallback -- byte-identical to the pre-shim direct read. */
    if (gdx_rom_buffer == NULL || (size_t)romBase + (size_t)size > gdx_rom_size) {
        return 0;
    }
    memcpy(dst, gdx_rom_buffer + (size_t)romBase, (size_t)size);
    return 1;
}

int GdxSegmentSourceContainingSpan(uint32_t romBase, uint32_t* outSpan) {
    /* size_needed = 1: find whichever blob's [rom_base, +size) contains romBase.
     * The generated table is static/const, so this needs no lock. */
    const GdxSegmentBlobEntry* entry = gdx_lookup_segment_blob((unsigned int)romBase, 1u);
    if (entry == NULL) {
        return 0;
    }
    if (outSpan != NULL) {
        *outSpan = (entry->rom_base + entry->size) - (unsigned int)romBase;
    }
    return 1;
}

int GdxSegmentSourcePreload(uint32_t romBase) {
    /* size_needed = 1: locate whichever family's [rom_base, +size) contains
     * romBase, then drive it through the same lock + lazy-load machinery the read
     * path uses. Boot-time single-shot; the per-family cache makes it idempotent. */
    const GdxSegmentBlobEntry* entry = gdx_lookup_segment_blob((unsigned int)romBase, 1u);
    int resident = 0;

    if (entry == NULL) {
        return 0;
    }

    seg_lock();
    {
        GdxSegFamilySlot* slot = seg_slot_for(entry);
        if (slot != NULL && seg_ensure_loaded(slot)) {
            resident = 1;
        }
    }
    seg_unlock();
    return resident;
}

int GdxSegmentSourcePreallocPayload(uint32_t romBase, void** outPayload, uint32_t* outCap) {
    /* Pre-allocates (without reading the archive) the whole-file staging buffer of the
     * family containing romBase and hands back its payload view, so a boot host can
     * gdx_register_host_range() the address on the MAIN thread (the host-range vector is
     * not thread-safe) before delegating the long archive read to a background worker.
     * Idempotent: an already-loaded family returns its live payload/size instead. */
    const GdxSegmentBlobEntry* entry = gdx_lookup_segment_blob((unsigned int)romBase, 1u);
    GdxSegFamilySlot* slot;
    int ok = 0;

    if (entry == NULL || outPayload == NULL || outCap == NULL) {
        return 0;
    }

    seg_lock();
    slot = seg_slot_for(entry);
    if (slot != NULL) {
        if (slot->loadState == 1) {
            *outPayload = slot->payload;
            *outCap = slot->payloadSize;
            ok = 1;
        } else if (slot->loadState != 2) {
            if (slot->stagedBuf == NULL) {
                slot->stagedBuf =
                    (unsigned char*)malloc((size_t)GDX_ARCHIVE_PAYLOAD_OFFSET + (size_t)entry->size);
            }
            if (slot->stagedBuf != NULL) {
                *outPayload = slot->stagedBuf + GDX_ARCHIVE_PAYLOAD_OFFSET;
                *outCap = entry->size;
                ok = 1;
            }
        }
    }
    seg_unlock();
    return ok;
}

int GdxSegmentSourcePayload(uint32_t romBase, void** outPayload, uint32_t* outSize) {
    const GdxSegmentBlobEntry* entry = gdx_lookup_segment_blob((unsigned int)romBase, 1u);
    int found = 0;

    if (entry == NULL) {
        return 0;
    }

    seg_lock();
    {
        unsigned int i;
        unsigned int count = sFamilyCount; /* holding the lock: plain read is fine */
        for (i = 0; i < count; i++) {
            if (sFamilies[i].entry == entry) {
                if (sFamilies[i].payload != NULL) {
                    if (outPayload != NULL) {
                        *outPayload = sFamilies[i].payload;
                    }
                    if (outSize != NULL) {
                        *outSize = sFamilies[i].payloadSize;
                    }
                    found = 1;
                }
                break;
            }
        }
    }
    seg_unlock();
    return found;
}

unsigned int gdx_segment_source_fallback_total(void) {
    unsigned int total = 0;
    unsigned int i;
    seg_lock();
    for (i = 0; i < sFamilyCount; i++) {
        total += sFamilies[i].fallbackReads;
    }
    total += sUnmappedFallback;
    seg_unlock();
    return total;
}

int GdxSegmentSourceFamilyStats(unsigned int index, const char** outKey,
                                unsigned int* outFallbackReads) {
    int found = 0;
    seg_lock();
    if (index < sFamilyCount) {
        if (outKey != NULL) {
            *outKey = sFamilies[index].entry->o2r_key;
        }
        if (outFallbackReads != NULL) {
            *outFallbackReads = sFamilies[index].fallbackReads;
        }
        found = 1;
    }
    seg_unlock();
    return found;
}
