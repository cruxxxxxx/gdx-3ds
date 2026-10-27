/* port/3ds/game/gdx3ds_ek_audio_bulk.c -- ekaudio2 `[debug] audio_sync_bulk_kb`: the disk audio
 * library's SYNCHRONOUS cartridge loads, read off the card in large requests.
 *
 * WHY. The first audio tick of an EK boot loads the persistent samples synchronously
 * (AudioLoad_SyncDma, decomp/src/audio/disk/lib/load.c), inside the producer's tick. SyncDma walks
 * the range in 0x400-byte DMAs; with the audio_table stream armed (`[debug] audio_stream`, the Old
 * 3DS preset) each of those is a gdx_o2r_stream read, and every 16th one misses and fills one
 * 16 KB block with one SD request. The rc2 hardware log (Old 3DS, JP disk) has 41 such fills by
 * t=10 s and one 16 KB fill costing 18-21 ms ([actx] io_unlock), against a first tick of
 * max=1054914us that let the boot prime time out and under-run (under edge #1). Emulator, same
 * build: that tick is 210-239 ms, of which dma=130 ms over 6 SyncDma calls.
 *
 * WHAT. A SyncDma of cart medium entirely inside the audio_table window is served with ONE fread
 * per `audio_sync_bulk_kb` straight from fzerox.o2r into the destination -- the file and payload
 * offset the stream itself resolved (gdx_o2r_stream_source / _data_offset), so the bytes are the
 * bytes the 0x400 walk would have copied out of the stream's cache. The stream's cache is not
 * touched: these samples move to MEDIUM_RAM the moment the load returns and are never DMA'd again.
 * sAudioCtxMutex is dropped across the card reads exactly as the stream's own fills drop it
 * (`[debug] audio_io_unlock`), so the game thread waits on this no more than it did before.
 *
 * DECLINES (the caller then runs the unchanged 0x400 walk): lever 0, the stream not armed, the
 * audio_stream_verify mode (its byte comparison must see every read), a range outside the window,
 * a range of one 16 KB stream block or less (no request to save), or any failed/short read -- the
 * walk then rewrites the whole destination, so a partial bulk read is never what the game sees.
 *
 * Model default 256 on the Old 3DS, 0 on the New 3DS. Only the EK build compiles this
 * (port/3ds/game/CMakeLists.txt, EK glue sources); the hook is
 * port/3ds/patches/decomp-3ds-ek-audio-sync-bulk.patch.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <3ds.h>

#include "gdx3ds_audio.h" /* gdx3ds_is_old3ds */
#include "gdx_audio_diet.h"
#include "gdx_o2r_stream.h"
#include "port_log.h"

extern int gdx3ds_config_get_int(const char* section, const char* key, int fallback); /* port/3ds/os */
extern int gdx_audio_ctx_io_unlock_enabled(void); /* port/gdx_audio_thread.cpp */
extern int gdx_audio_ctx_io_release(void);
extern void gdx_audio_ctx_io_reacquire(void);

#define AUDIO_BULK_MAX_KB 1024
#define AUDIO_BULK_MIN_BYTES (16u * 1024u + 1u) /* spans two stream blocks: below that nothing is saved */
#define AUDIO_BULK_RECEIPT_FIRST 8u
#define AUDIO_BULK_RECEIPT_EVERY 64u

static int sResolved = 0;
static unsigned int sChunkBytes = 0;
static FILE* sFile = NULL;
static int sFileFailed = 0;
static LightLock sLock;

static uint32_t sCalls = 0;
static uint64_t sBytes = 0;
static uint32_t sRequests = 0;
static uint64_t sTicks = 0;
static uint64_t sMaxTicks = 0;
static uint32_t sDeclinedReads = 0;
/* Declines by reason, so a log says why the walk still ran: under two blocks, outside the
 * audio_table window, or the stream not armed. */
static uint32_t sDeclinedSmall = 0;
static uint32_t sDeclinedWindow = 0;
static uint32_t sDeclinedInactive = 0;
static uint32_t sOffers = 0;

static unsigned int ResolveLever(void) {
    const int modelDefault = gdx3ds_is_old3ds() ? 256 : 0;
    int kb = gdx3ds_config_get_int("debug", "audio_sync_bulk_kb", modelDefault);
    const int verify = gdx3ds_config_get_int("debug", "audio_stream_verify", 0);
    if (kb < 0) {
        kb = 0;
    }
    if (kb > AUDIO_BULK_MAX_KB) {
        kb = AUDIO_BULK_MAX_KB;
    }
    if (verify != 0) {
        kb = 0;
    }
    gdx_port_logf("[audiobulk] audio_sync_bulk_kb=%d (model default %d%s): synchronous audio_table"
                  " loads %s\n",
                  kb, modelDefault, verify != 0 ? ", forced 0 by audio_stream_verify" : "",
                  kb != 0 ? "read straight off fzerox.o2r in large requests (ekaudio2)"
                          : "walk the stream in 0x400 DMAs (pre-ekaudio2)");
    LightLock_Init(&sLock);
    return (unsigned int) kb * 1024u;
}

static void BulkReceipt(uint32_t size, const char* outcome) {
    const double perMs = (double) SYSCLOCK_ARM11 / 1000.0;
    gdx_port_logf("[audiobulk] calls=%lu KB=%llu sd_requests=%lu sd=%.1f ms max=%.1f ms"
                  " read_fail=%lu declined small=%lu window=%lu inactive=%lu | last %lu B %s\n",
                  (unsigned long) sCalls, (unsigned long long) (sBytes / 1024u),
                  (unsigned long) sRequests, (double) sTicks / perMs, (double) sMaxTicks / perMs,
                  (unsigned long) sDeclinedReads, (unsigned long) sDeclinedSmall,
                  (unsigned long) sDeclinedWindow, (unsigned long) sDeclinedInactive,
                  (unsigned long) size, outcome);
}

/* Receipts for the first few offers (the boot's persistent-sample load is all of them), then
 * every 64th. Called with sLock held or before it is needed; never under sAudioCtxMutex's drop. */
static void BulkNote(uint32_t size, const char* outcome) {
    sOffers++;
    if (sOffers <= AUDIO_BULK_RECEIPT_FIRST || (sOffers % AUDIO_BULK_RECEIPT_EVERY) == 0u) {
        BulkReceipt(size, outcome);
    }
}

static FILE* BulkFile(void) {
    if (sFile != NULL || sFileFailed) {
        return sFile;
    }
    sFile = fopen(gdx_o2r_stream_source(), "rb");
    if (sFile == NULL) {
        sFileFailed = 1;
        gdx_port_logf("[audiobulk] cannot open %s: bulk reads off\n", gdx_o2r_stream_source());
        return NULL;
    }
    /* The reads are already large; stdio's buffer would only add a copy. */
    setvbuf(sFile, NULL, _IONBF, 0);
    return sFile;
}

/* One or more SD requests, each at most sChunkBytes. Returns 1 when all `size` bytes landed. */
static int BulkReadFile(FILE* f, uint32_t fileOffset, uint8_t* dst, uint32_t size) {
    if (fseek(f, (long) fileOffset, SEEK_SET) != 0) {
        return 0;
    }
    while (size > 0u) {
        const uint32_t want = size < sChunkBytes ? size : sChunkBytes;
        const uint64_t t0 = svcGetSystemTick();
        const size_t got = fread(dst, 1, want, f);
        const uint64_t dt = svcGetSystemTick() - t0;
        sRequests++;
        sTicks += dt;
        if (dt > sMaxTicks) {
            sMaxTicks = dt;
        }
        if (got != want) {
            return 0;
        }
        dst += want;
        size -= want;
    }
    return 1;
}

/* `[debug] audio_sync_bulk_verify` (default 0): re-read every bulk range through the stream path
 * the walk uses (GdxSegmentSourceRead -> the audio_table window -> gdx_o2r_stream) and compare. A
 * proof lever for a new device or a new .o2r, not a mode to ship: it pays every SD request the
 * bulk read saved. Frame hashes cannot see a wrong sample, so this is the byte-level check. */
extern int GdxSegmentSourceRead(unsigned int romBase, unsigned int size, void* dst);
static int sVerifyLever = -1;
static uint32_t sVerifyOk = 0;
static uint32_t sVerifyBad = 0;

static const char* BulkVerify(uint32_t romOffset, const uint8_t* got, uint32_t size) {
    if (sVerifyLever < 0) {
        sVerifyLever = gdx3ds_config_get_int("debug", "audio_sync_bulk_verify", 0) != 0 ? 1 : 0;
    }
    if (!sVerifyLever) {
        return "bulk";
    }
    uint8_t* ref = (uint8_t*) malloc(size);
    if (ref == NULL || !GdxSegmentSourceRead(romOffset, size, ref)) {
        free(ref);
        return "bulk (verify: reference read failed)";
    }
    const int same = memcmp(ref, got, size) == 0;
    free(ref);
    if (same) {
        sVerifyOk++;
    } else {
        sVerifyBad++;
    }
    gdx_port_logf("[audiobulk] verify ok=%lu bad=%lu\n", (unsigned long) sVerifyOk, (unsigned long) sVerifyBad);
    return same ? "bulk (verify: identical)" : "bulk (verify: MISMATCH)";
}

int gdx3ds_ek_audio_sync_bulk(uint32_t devAddr, void* ramAddr, uint32_t size) {
    if (!sResolved) {
        sResolved = 1;
        sChunkBytes = ResolveLever();
    }
    if (sChunkBytes == 0u || ramAddr == NULL) {
        return 0;
    }
    LightLock_Lock(&sLock);
    if (!gdx_o2r_stream_active()) {
        sDeclinedInactive++;
        BulkNote(size, "declined: stream not armed");
        LightLock_Unlock(&sLock);
        return 0;
    }
    if (size < AUDIO_BULK_MIN_BYTES) {
        sDeclinedSmall++;
        BulkNote(size, "declined: within one block");
        LightLock_Unlock(&sLock);
        return 0;
    }
    /* osEPiStartDma's own mapping (port/3ds/lus_glue/gdx3ds_libultra.cpp), then the window's. */
    const uint32_t romOffset = devAddr & 0x0FFFFFFFu;
    if (romOffset < GDX_AUDIO_TABLE_ROM_BASE ||
        (uint64_t) romOffset + size > (uint64_t) GDX_AUDIO_TABLE_ROM_BASE + gdx_o2r_stream_payload_bytes()) {
        sDeclinedWindow++;
        BulkNote(size, "declined: outside audio_table");
        LightLock_Unlock(&sLock);
        return 0;
    }
    const uint32_t fileOffset = gdx_o2r_stream_data_offset() + (romOffset - GDX_AUDIO_TABLE_ROM_BASE);

    FILE* f = BulkFile();
    int ok = 0;
    if (f != NULL) {
        const int dropped = gdx_audio_ctx_io_unlock_enabled() ? gdx_audio_ctx_io_release() : 0;
        ok = BulkReadFile(f, fileOffset, (uint8_t*) ramAddr, size);
        if (dropped) {
            gdx_audio_ctx_io_reacquire();
        }
    }
    if (ok) {
        sCalls++;
        sBytes += size;
        BulkNote(size, BulkVerify(romOffset, (const uint8_t*) ramAddr, size));
    } else if (f != NULL) {
        sDeclinedReads++;
        BulkNote(size, "declined: read failed, walking");
    }
    LightLock_Unlock(&sLock);
    return ok;
}
