/* port/3ds/game/gdx3ds_disk_async.c -- o3dsekperf: the 64DD audio loader's I/O worker.
 * Contract, measurements and the lever: port/3ds/include/gdx3ds_disk_async.h.
 *
 * One thread, one job slot, two events. The producer is the only submitter (the async disk
 * state machine runs on it alone), so the slot needs no queue: submit waits for idle, fills the
 * slot, clears `idle` and kicks the worker; the worker runs the job, stamps its wall time and
 * signals `idle`. The worker spends almost all of a job blocked in the FS service call, so it
 * sits ABOVE the producer's priority on the producer's core: it costs the producer only the
 * block-cache copy, and it is never starved by the producer's synthesis while a read is due. */
#include "gdx3ds_disk_async.h"

#include <stdint.h>

#include <3ds.h>

#include "gdx3ds_audio.h" /* gdx3ds_is_old3ds */
#include "port_log.h"

extern int gdx3ds_config_get_int(const char* section, const char* key, int fallback); /* port/3ds/os */

#define DISK_ASYNC_STACK (32u * 1024u)
#define DISK_ASYNC_PRIO 0x18 /* the drain thread's priority, one above the producer's 0x19 */
#define DISK_ASYNC_RECEIPT_EVERY 64u

static int sResolved = 0;
static int sEnabled = 0;
static int sCore = -1;
static Thread sThread = NULL;
static LightEvent sKick;
static LightEvent sIdle;
static volatile int sBusy = 0;
static GdxDiskAsyncJob sJob = NULL;
static void* sJobArg = NULL;

/* Worker-side statistics (written by the worker only, read by its own receipt). */
static uint32_t sJobs = 0;
static uint64_t sJobTicks = 0;
static uint64_t sJobMaxTicks = 0;
/* Submitter-side: submits that found the previous job still running (should stay 0). */
static uint32_t sStraggles = 0;

static void DiskAsyncReceipt(void) {
    const double perMs = (double)SYSCLOCK_ARM11 / 1000.0;
    gdx_port_logf("[audiodisk] async jobs=%lu sd=%.1f ms avg=%.2f ms max=%.2f ms straggles=%lu core=%d\n",
                  (unsigned long)sJobs, (double)sJobTicks / perMs,
                  sJobs != 0 ? (double)sJobTicks / perMs / (double)sJobs : 0.0, (double)sJobMaxTicks / perMs,
                  (unsigned long)sStraggles, sCore);
}

static void DiskAsyncEntry(void* unused) {
    (void)unused;
    for (;;) {
        LightEvent_Wait(&sKick);
        const uint64_t t0 = svcGetSystemTick();
        sJob(sJobArg);
        const uint64_t dt = svcGetSystemTick() - t0;
        sJobs++;
        sJobTicks += dt;
        if (dt > sJobMaxTicks) {
            sJobMaxTicks = dt;
        }
        if (sJobs == 1u || (sJobs % DISK_ASYNC_RECEIPT_EVERY) == 0u) {
            DiskAsyncReceipt();
        }
        __dmb();
        sBusy = 0;
        LightEvent_Signal(&sIdle);
    }
}

static int DiskAsyncStart(void) {
    static const int kCores[2] = { 1, 0 };
    int i;
    LightEvent_Init(&sKick, RESET_ONESHOT);
    LightEvent_Init(&sIdle, RESET_STICKY);
    LightEvent_Signal(&sIdle);
    for (i = 0; i < 2; i++) {
        sThread = threadCreate(DiskAsyncEntry, NULL, DISK_ASYNC_STACK, DISK_ASYNC_PRIO, kCores[i], true);
        if (sThread != NULL) {
            sCore = kCores[i];
            return 1;
        }
    }
    return 0;
}

/* ekaudio2 levers (port/3ds/include/gdx3ds_disk_async.h, COALESCED READS). Latched on first
 * use, one receipt each. Clamped to 0..256 KB; the mechanism caps a run further (set count,
 * half the cache), so a large ask cannot turn into an eviction storm. */
#define DISK_FILL_MAX_KB 256

static unsigned int ResolveKbLever(const char* key, int modelDefault) {
    int kb = gdx3ds_config_get_int("debug", key, modelDefault);
    if (kb < 0) {
        kb = 0;
    }
    if (kb > DISK_FILL_MAX_KB) {
        kb = DISK_FILL_MAX_KB;
    }
    return (unsigned int) kb * 1024u;
}

unsigned int gdx3ds_disk_coalesce_bytes(void) {
    static int sResolvedCoalesce = 0;
    static unsigned int sCoalesceBytes = 0;
    if (sResolvedCoalesce) {
        return sCoalesceBytes;
    }
    sResolvedCoalesce = 1;
    const int modelDefault = gdx3ds_is_old3ds() ? 64 : 0;
    sCoalesceBytes = ResolveKbLever("disk_stream_coalesce_kb", modelDefault);
    gdx_port_logf("[diskfill] disk_stream_coalesce_kb=%u (model default %d): a 64DD read's missing"
                  " cache blocks are fetched %s\n",
                  sCoalesceBytes / 1024u, modelDefault,
                  sCoalesceBytes != 0 ? "in runs, one SD request per run (ekaudio2)"
                                      : "one SD request per 16 KB block (pre-ekaudio2)");
    return sCoalesceBytes;
}

unsigned int gdx3ds_disk_readahead_bytes(void) {
    static int sResolvedReadahead = 0;
    static unsigned int sReadaheadBytes = 0;
    if (sResolvedReadahead) {
        return sReadaheadBytes;
    }
    sResolvedReadahead = 1;
    const int modelDefault = gdx3ds_is_old3ds() ? 64 : 0;
    sReadaheadBytes = ResolveKbLever("audio_disk_readahead_kb", modelDefault);
    if (gdx3ds_disk_coalesce_bytes() == 0) {
        sReadaheadBytes = 0; /* read-ahead is a longer run; with runs off there is none */
    }
    gdx_port_logf("[diskfill] audio_disk_readahead_kb=%u (model default %d): an async audio-load"
                  " read that misses %s\n",
                  sReadaheadBytes / 1024u, modelDefault,
                  sReadaheadBytes != 0 ? "also fetches the load's next bytes, up to this window"
                                       : "fetches only its own bytes");
    return sReadaheadBytes;
}

int gdx3ds_disk_async_enabled(void) {
    if (sResolved) {
        return sEnabled;
    }
    sResolved = 1;
    const int modelDefault = gdx3ds_is_old3ds();
    const int asked = gdx3ds_config_get_int("debug", "audio_disk_async", modelDefault) != 0 ? 1 : 0;
    sEnabled = asked ? DiskAsyncStart() : 0;
    gdx_port_logf("[audiodisk] audio_disk_async=%d (model default %d, asked %d): 64DD audio-load reads %s"
                  " (worker core %d prio 0x%x)\n",
                  sEnabled, modelDefault, asked,
                  sEnabled ? "run on the I/O worker (the producer tick never waits on the card)"
                           : (asked ? "run INLINE on the producer (worker thread failed to start)"
                                    : "run inline on the producer (pre-o3dsekperf)"),
                  sCore, DISK_ASYNC_PRIO);
    return sEnabled;
}

void gdx3ds_disk_async_wait_idle(void) {
    while (sBusy) {
        LightEvent_Wait(&sIdle);
    }
    __dmb();
}

int gdx3ds_disk_async_submit(GdxDiskAsyncJob job, void* arg) {
    if (!gdx3ds_disk_async_enabled()) {
        return 0;
    }
    if (sBusy) {
        sStraggles++;
        gdx3ds_disk_async_wait_idle();
    }
    sJob = job;
    sJobArg = arg;
    LightEvent_Clear(&sIdle);
    sBusy = 1;
    __dmb();
    LightEvent_Signal(&sKick);
    return 1;
}
