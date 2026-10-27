/* port/gdx_disk_stream.cpp -- the 64DD arm of the block reader.
 * See gdx_disk_stream.h for the why, the contract and the cache-sizing argument.
 *
 * The cache itself now lives in port/gdx_block_cache.{h,cpp}: `audiodiet` needed the same
 * machinery for a second file (the STORED audio_blob/audio_table entry inside fzerox.o2r),
 * and two copies of an LRU cache is how two subtly different LRU caches happen. The state
 * moved into a struct and this file became a ONE-INSTANCE wrapper over it; every semantic
 * below -- the refusal-leaves-dst-untouched contract, the two-pass read, the prove/copy
 * hit accounting, the round-down set count, the wrong-size refusal, the overlay surviving a
 * close -- is unchanged, which is exactly what the 523 checks in
 * port/tests/gdx_disk_stream_tests.cpp re-gate.
 *
 * What stayed here because it is 64DD-specific: the CRC-64/XZ whole-image sweep (same
 * polynomial and reflection as port/disk_savefile.cpp, so a value produced here is
 * interchangeable with one produced there).
 *
 * What moved DOWN into the shared cache with everything else: `ekdrive`'s working-set map
 * (B11d) -- the per-block bitmap behind `uniqBlk`/`wsKB`/`refetch`. It is instrumentation of a
 * cache, not of a 64DD image, and the audio arm needs the same question answered about a file
 * nobody had measured either. `gdx_disk_stream_unique_blocks()` keeps its exact contract and
 * the three checks `ekdrive` added to the disk-stream suite (526 now, up from 523) gate it
 * from here.
 */
#include "gdx_disk_stream.h"

#include "gdx_block_cache.h"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#if defined(GDX_PLATFORM_3DS)
#include "3ds/include/gdx3ds_hitch.h" // HITCH: the coalesced fill is an SD read like any other
#define GDX_DS_HITCH_BEGIN() (gdx3ds_hitch_aud_inflight = 1)
#define GDX_DS_HITCH_END(bytes)                                   \
    do {                                                          \
        gdx3ds_hitch_aud_inflight = 0;                            \
        gdx3ds_hitch_add(GDX_HITCH_EV_AUD_SD_READ, 1u);           \
        gdx3ds_hitch_add(GDX_HITCH_EV_AUD_SD_BYTES, (uint32_t) (bytes)); \
    } while (0)
#else
#define GDX_DS_HITCH_BEGIN() ((void) 0)
#define GDX_DS_HITCH_END(bytes) ((void) (bytes))
#endif

/* The wrapper casts between the two stats structs and the two overlay typedefs rather than
 * copying field by field, so make the compiler prove they are the same shape. If a field is
 * ever added to one and not the other, this fails at build time instead of silently
 * mis-reporting a receipt. */
static_assert(sizeof(GdxDiskStreamStats) == sizeof(GdxBlockCacheStats),
              "GdxDiskStreamStats and GdxBlockCacheStats must stay layout-identical");
static_assert(offsetof(GdxDiskStreamStats, refusals) == offsetof(GdxBlockCacheStats, refusals),
              "GdxDiskStreamStats and GdxBlockCacheStats must stay layout-identical");
static_assert(GDX_DISK_STREAM_WAYS == GDX_BLOCK_CACHE_WAYS,
              "the 64DD arm's associativity is the shared cache's");

namespace {

GdxBlockCache s_disk = {};

/* o3dsekperf: THE STREAM'S LOCK. gdx_block_cache is single-consumer by contract, and this
 * wrapper used to inherit that. It was never true on the Expansion Kit: the game thread reads
 * the image (course records, EK asset fills, LeoReadWrite from the game's fibers) while the audio
 * producer -- a real thread on core 1 -- reads it for the disk audio library, and
 * `[debug] audio_disk_async` adds the I/O worker as a third reader. Two threads in one
 * fseek/fread pair, or in one LRU update, is a wrong read with no error. Recursive because a
 * read's overlay (gdx_disk_save_overlay) re-enters gdx_disk_stream_note_overlay. Held across the
 * card read, so a second reader waits it out -- what the producer's sAudioCtxMutex already made
 * the game thread do whenever it queued audio behind an inline disk read. */
std::recursive_mutex s_diskLock;

/* ekaudio2 COALESCED FILL (gdx_disk_stream_fill_range). The shared cache fills one block per
 * fseek/fread; on an Old 3DS one 16 KiB SD request costs 18-21 ms (rc2 hardware log, [actx]
 * io_unlock), and almost none of that is the bytes -- it is the per-request cost. A 64DD LBA is
 * 19,720 B, so every LBA the audio loader asks for is two (often three) of those requests. This
 * reads a RUN of missing blocks with one request into a staging buffer and installs them.
 *
 * It lives here and not in gdx_block_cache.cpp on purpose: that TU is also linked into the
 * shipped non-EK binaries (the audio_table stream), and they must stay byte-identical. So the
 * install below repeats the cache's own victim rule (first invalid way, else the oldest) and
 * LRU touch -- the struct is public for exactly this kind of wrapper -- rather than reaching
 * into the cache's anonymous namespace. */
unsigned char* s_fillStage = nullptr;
unsigned int s_fillStageBytes = 0;
unsigned long long s_fillRuns = 0;   /* SD requests the coalesced fill issued */
unsigned long long s_fillBlocks = 0; /* blocks those requests installed */

bool fillResident(const GdxBlockCache* c, unsigned int block) {
    const unsigned int base = (block & c->setMask) * GDX_BLOCK_CACHE_WAYS;
    for (unsigned int w = 0; w < GDX_BLOCK_CACHE_WAYS; w++) {
        if (c->slots[base + w].valid && c->slots[base + w].block == block) {
            return true;
        }
    }
    return false;
}

unsigned int fillVictim(const GdxBlockCache* c, unsigned int set) {
    const unsigned int base = set * GDX_BLOCK_CACHE_WAYS;
    for (unsigned int w = 0; w < GDX_BLOCK_CACHE_WAYS; w++) {
        if (!c->slots[base + w].valid) {
            return w;
        }
    }
    unsigned int victim = 0;
    unsigned char bestAge = 0;
    for (unsigned int w = 0; w < GDX_BLOCK_CACHE_WAYS; w++) {
        if (c->slots[base + w].age >= bestAge) {
            bestAge = c->slots[base + w].age;
            victim = w;
        }
    }
    return victim;
}

void fillTouch(GdxBlockCache* c, unsigned int set, unsigned int way) {
    const unsigned int base = set * GDX_BLOCK_CACHE_WAYS;
    const unsigned char was = c->slots[base + way].age;
    for (unsigned int w = 0; w < GDX_BLOCK_CACHE_WAYS; w++) {
        if (c->slots[base + w].age < was) {
            c->slots[base + w].age = (unsigned char) (c->slots[base + w].age + 1);
        }
    }
    c->slots[base + way].age = 0;
}

void fillMarkTouched(GdxBlockCache* c, unsigned int block) {
    if (c->touched == nullptr || (block >> 3) >= c->touchedBytes) {
        return;
    }
    const unsigned char bit = (unsigned char) (1u << (block & 7u));
    if ((c->touched[block >> 3] & bit) == 0) {
        c->touched[block >> 3] |= bit;
        c->uniqueBlocks++;
    }
}

/* Installs `count` blocks starting at `first` out of the staging buffer. `bytes` is what the
 * read returned; a block past it (the file's tail) is zero-padded like fillSlot's tail. */
void fillInstall(GdxBlockCache* c, unsigned int first, unsigned int count, unsigned int bytes) {
    for (unsigned int i = 0; i < count; i++) {
        const unsigned int block = first + i;
        const unsigned int set = block & c->setMask;
        const unsigned int way = fillVictim(c, set);
        const unsigned int idx = set * GDX_BLOCK_CACHE_WAYS + way;
        unsigned char* dst = c->data + (size_t) idx * c->blockBytes;
        const unsigned int at = i * c->blockBytes;
        const unsigned int have = (bytes > at) ? (bytes - at) : 0u;
        const unsigned int take = (have < c->blockBytes) ? have : c->blockBytes;
        if (c->slots[idx].valid) {
            c->stats.blockEvictions++;
        }
        memcpy(dst, s_fillStage + at, take);
        if (take < c->blockBytes) {
            memset(dst + take, 0, c->blockBytes - take);
        }
        c->slots[idx].block = block;
        c->slots[idx].valid = 1;
        fillTouch(c, set, way);
        fillMarkTouched(c, block);
        c->blockFills++;
        c->prefetchFills++;
    }
}

/* One SD request for blocks [first, first+count). Returns false on a short or failed read,
 * having installed NOTHING: the demand read that follows then fills those blocks itself, one
 * at a time, through the cache's own refusal path. */
bool fillRun(GdxBlockCache* c, unsigned int first, unsigned int count) {
    const unsigned long long off = (unsigned long long) first << c->blockLog2;
    unsigned long long want = (unsigned long long) count << c->blockLog2;
    if (off + want > (unsigned long long) c->fileSize) {
        want = (unsigned long long) c->fileSize - off;
    }
    FILE* f = (FILE*) c->file;
    if (fseek(f, (long) off, SEEK_SET) != 0) {
        return false;
    }
    GDX_DS_HITCH_BEGIN();
    const size_t got = fread(s_fillStage, 1, (size_t) want, f);
    GDX_DS_HITCH_END(got);
    c->stats.hostReads++;
    c->stats.hostBytes += got;
    if (got != (size_t) want) {
        return false;
    }
    fillInstall(c, first, count, (unsigned int) got);
    s_fillRuns++;
    s_fillBlocks += count;
    return true;
}

unsigned long long s_crcTable[256];
bool s_crcReady = false;

void crcInit() {
    if (s_crcReady) {
        return;
    }
    for (unsigned n = 0; n < 256; n++) {
        unsigned long long c = n;
        for (int k = 0; k < 8; k++) {
            c = (c & 1ull) ? (0xC96C5795D7870F42ull ^ (c >> 1)) : (c >> 1);
        }
        s_crcTable[n] = c;
    }
    s_crcReady = true;
}

} // namespace

extern "C" {

int gdx_disk_stream_open(const char* path, unsigned int expectedSize,
                         unsigned int blockLog2, unsigned int cacheBlocks) {
    /* base 0 / span 0: the window is the whole image, which is what every 64DD consumer
     * addresses. `expectedSize` keeps its exact meaning (0 = accept any non-zero size). */
    return gdx_block_cache_open(&s_disk, path, 0u, 0u, expectedSize, blockLog2, cacheBlocks);
}

void gdx_disk_stream_close(void) {
    gdx_block_cache_close(&s_disk);
}

int gdx_disk_stream_active(void) {
    return gdx_block_cache_active(&s_disk);
}

unsigned int gdx_disk_stream_size(void) {
    return s_disk.fileSize;
}

unsigned int gdx_disk_stream_cache_bytes(void) {
    return gdx_block_cache_bytes(&s_disk);
}

unsigned int gdx_disk_stream_block_bytes(void) {
    return gdx_block_cache_block_bytes(&s_disk);
}

const char* gdx_disk_stream_path(void) {
    return gdx_block_cache_path(&s_disk);
}

void gdx_disk_stream_set_overlay(GdxDiskOverlayFn fn) {
    gdx_block_cache_set_overlay(&s_disk, (GdxBlockOverlayFn) fn);
}

int gdx_disk_stream_read(unsigned int offset, unsigned int length, void* dst) {
    std::lock_guard<std::recursive_mutex> lock(s_diskLock);
    return gdx_block_cache_read(&s_disk, offset, length, dst);
}

void gdx_disk_stream_invalidate(unsigned int offset, unsigned int length) {
    std::lock_guard<std::recursive_mutex> lock(s_diskLock);
    gdx_block_cache_invalidate(&s_disk, offset, length);
}

int gdx_disk_stream_read_uncached(unsigned int offset, unsigned int length, void* dst) {
    std::lock_guard<std::recursive_mutex> lock(s_diskLock);
    return gdx_block_cache_read_uncached(&s_disk, offset, length, dst);
}

unsigned long long gdx_disk_stream_crc64(int* ok) {
    if (ok != nullptr) {
        *ok = 0;
    }
    if (!gdx_block_cache_active(&s_disk)) {
        return 0ull;
    }
    crcInit();
    /* Cache-bypassing on purpose: a whole-image sweep would evict every useful block and
     * gain nothing, since no byte is revisited. Reads straight into a scratch chunk. */
    const unsigned int chunkBytes = gdx_block_cache_block_bytes(&s_disk);
    unsigned char* chunk = (unsigned char*) malloc(chunkBytes);
    if (chunk == nullptr) {
        return 0ull;
    }
    const unsigned int size = s_disk.fileSize;
    unsigned long long crc = 0xFFFFFFFFFFFFFFFFull;
    unsigned int done = 0;
    while (done < size) {
        unsigned int want = chunkBytes;
        if (size - done < want) {
            want = size - done;
        }
        if (!gdx_disk_stream_read_uncached(done, want, chunk)) {
            free(chunk);
            return 0ull;
        }
        for (unsigned int i = 0; i < want; i++) {
            crc = s_crcTable[(crc ^ chunk[i]) & 0xFFull] ^ (crc >> 8);
        }
        done += want;
    }
    free(chunk);
    crc ^= 0xFFFFFFFFFFFFFFFFull;
    if (ok != nullptr) {
        *ok = 1;
    }
    return crc;
}

void gdx_disk_stream_note_overlay(unsigned int bytes) {
    std::lock_guard<std::recursive_mutex> lock(s_diskLock);
    gdx_block_cache_note_overlay(&s_disk, bytes);
}

void gdx_disk_stream_get_stats(GdxDiskStreamStats* out) {
    std::lock_guard<std::recursive_mutex> lock(s_diskLock);
    gdx_block_cache_get_stats(&s_disk, (GdxBlockCacheStats*) out);
}

void gdx_disk_stream_reset_stats(void) {
    std::lock_guard<std::recursive_mutex> lock(s_diskLock);
    gdx_block_cache_reset_stats(&s_disk);
}

unsigned long long gdx_disk_stream_unique_blocks(void) {
    std::lock_guard<std::recursive_mutex> lock(s_diskLock);
    return gdx_block_cache_unique_blocks(&s_disk);
}

const char* gdx_disk_stream_format_stats(char* buf, unsigned int bufSize) {
    std::lock_guard<std::recursive_mutex> lock(s_diskLock);
    return gdx_block_cache_format_stats(&s_disk, buf, bufSize);
}

int gdx_disk_stream_fill_range(unsigned int offset, unsigned int length, unsigned int extra,
                               unsigned int maxRunBytes) {
    std::lock_guard<std::recursive_mutex> lock(s_diskLock);
    GdxBlockCache* c = &s_disk;
    if (c->file == nullptr || length == 0u || maxRunBytes == 0u) {
        return 0;
    }
    if (offset >= c->fileSize) {
        return 0;
    }
    if ((unsigned long long) offset + length > (unsigned long long) c->fileSize) {
        length = c->fileSize - offset;
    }
    if (extra != 0u) {
        /* The read-ahead tail rides only on a request that misses: a request already
         * resident was paid for by the previous run, and topping the window up by a block or
         * two per request would turn one large SD request into many small ones again. */
        bool missing = false;
        for (unsigned int b = offset >> c->blockLog2; b <= (offset + length - 1u) >> c->blockLog2; b++) {
            if (!fillResident(c, b)) {
                missing = true;
                break;
            }
        }
        if (!missing) {
            return 0;
        }
        unsigned long long end = (unsigned long long) offset + length + extra;
        if (end > (unsigned long long) c->fileSize) {
            end = c->fileSize;
        }
        length = (unsigned int) (end - offset);
    }
    if (length > (c->blocks << c->blockLog2) / 2u) {
        length = (c->blocks << c->blockLog2) / 2u;
    }
    /* A run never holds two blocks of one set (the second install could evict the first),
     * and never more than half the cache (it must not flush the working set it serves). */
    unsigned int runBlocks = maxRunBytes >> c->blockLog2;
    if (runBlocks > c->sets) {
        runBlocks = c->sets;
    }
    if (runBlocks > c->blocks / 2u) {
        runBlocks = c->blocks / 2u;
    }
    if (runBlocks < 2u) {
        return 0; /* a one-block run is what the demand path already does */
    }
    const unsigned int stageBytes = runBlocks << c->blockLog2;
    if (s_fillStageBytes < stageBytes) {
        unsigned char* grown = (unsigned char*) realloc(s_fillStage, stageBytes);
        if (grown == nullptr) {
            return -1;
        }
        s_fillStage = grown;
        s_fillStageBytes = stageBytes;
    }
    const unsigned int firstBlock = offset >> c->blockLog2;
    const unsigned int lastBlock = (offset + length - 1u) >> c->blockLog2;
    int requests = 0;
    unsigned int b = firstBlock;
    while (b <= lastBlock) {
        if (fillResident(c, b)) {
            b++;
            continue;
        }
        unsigned int count = 1;
        while (b + count <= lastBlock && count < runBlocks && !fillResident(c, b + count)) {
            count++;
        }
        if (count < 2u) {
            /* A lone missing block: leave it to the demand read, which costs the same one
             * request and keeps the miss accounted where it happens. */
            b++;
            continue;
        }
        if (!fillRun(c, b, count)) {
            return -1;
        }
        requests++;
        b += count;
    }
    return requests;
}

void gdx_disk_stream_fill_stats(unsigned long long* runs, unsigned long long* blocks) {
    std::lock_guard<std::recursive_mutex> lock(s_diskLock);
    if (runs != nullptr) {
        *runs = s_fillRuns;
    }
    if (blocks != nullptr) {
        *blocks = s_fillBlocks;
    }
}

} // extern "C"
