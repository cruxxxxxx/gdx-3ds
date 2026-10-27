/* port/gdx_disk_stream.h -- SD/file-backed 64DD disk reader with a bounded block cache.
 *
 * WHY. `port/disk_buffer.cpp` mounts a 64DD image as ONE 61.9 MB malloc
 * (GDX_DISK_EXACT_BYTES = 64,931,840) that stays resident for the session, and
 * `port/n64_leo.c` random-accesses it. On the 3DS that does not fit and cannot be made to
 * fit: `docs/research/native/o3dsbudget-progress.md` measures the New 3DS malloc grant at
 * 85,311,488 B with 44,674,144 B already in use (~36 MB spare, before the EK audio content
 * a mounted disk brings back), and projects the DEFAULT Old 3DS layout at ~23.9 MB of grant
 * against a 44.4 MB requirement -- short before any disk exists. This is a streaming problem.
 *
 * WHAT THIS IS. A read-only, seekable byte source over the image file plus a fixed-size,
 * set-associative block cache. It allocates exactly once, at open, and never grows. It has
 * no decomp headers, no 3DS headers and no libultraship: it compiles for the host, which is
 * what lets `port/tests/gdx_disk_stream_tests.cpp` gate it in `tools/ci-3ds.sh` against a
 * synthetic image from `tools/dev/mkdisk64.py` -- no ROM, no dump, nothing copyrighted.
 *
 * WHAT IT IS NOT. It is not a writable image. 64DD writes go to the durable dirty-range
 * journal (`port/disk_savefile.cpp`), never to the user's .ndd, and the reader composes them
 * back in through the OVERLAY hook below -- so a streamed read returns exactly what the
 * resident image would have returned after gdx_disk_save_apply().
 *
 * CACHE SIZING was an argument that named its assumption; B11d MEASURED it against a real
 * 64DD image streamed off SD (docs/research/native/ekdrive-progress.md). A boot -> main menu ->
 * COURSE EDIT session makes 785 LEO reads for 1,078 KB and touches a WORKING SET of 89 distinct
 * 16 KiB blocks = 1,424 KB; menus-longtail produces the identical figures, i.e. essentially all
 * 64DD traffic on these paths is boot traffic. Sweeping the lever over one identical session:
 *
 *   cacheKB   128    256    512   1024   2048   4096
 *   refetch    13      5      5      4      2      0
 *   hostKB   1808   1520   1504   1488   1456   1424
 *
 * The whole 128 KiB -> 4 MiB span is 384 KB of card reads, once, because the traffic is 854
 * lookups over 89 distinct blocks -- almost all first-touch. Capacity buys almost nothing, so
 * the default came DOWN to 512 KiB: 80 KB more card reads per boot than an infinite cache, and
 * 1.5 MB of heap back on the projected Old 3DS layout where 2 MiB was 8.4% of a short grant.
 * NOT covered by that measurement, and stated rather than assumed: no course was loaded or
 * saved and no MEDIUM_LBA audio streamed (the 31 MB block at user LBA 1004 was never read).
 * The MFS management area is 6 of the 89 blocks (96 KB), file blocks 3117..3122.
 *
 * NOTE on the shape: the set count is rounded DOWN to a power of two (the index is a mask, not
 * a division, because this runs on a 268 MHz ARM11), so a cacheBlocks that is not
 * ways * 2^n yields fewer blocks than asked. gdx_disk_stream_cache_bytes() reports the truth.
 */
#ifndef GDX_DISK_STREAM_H
#define GDX_DISK_STREAM_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GdxDiskStreamStats {
    unsigned long long reads;          /* gdx_disk_stream_read() calls that were served */
    unsigned long long readBytes;      /* bytes those calls asked for */
    unsigned long long blockHits;      /* block lookups served from cache */
    unsigned long long blockMisses;    /* block lookups that went to the file */
    unsigned long long blockEvictions; /* a valid block replaced by another */
    unsigned long long hostReads;      /* fseek/fread pairs issued */
    unsigned long long hostBytes;      /* bytes pulled off the file */
    unsigned long long overlayBytes;   /* bytes rewritten by the dirty-range overlay */
    unsigned long long refusals;       /* out-of-range or short-read refusals */
} GdxDiskStreamStats;

/* Dirty-range overlay: called AFTER the base bytes are in `dst`, with the same
 * (offset, length), so writes recorded since boot win over the file's pristine bytes.
 * Installed by port/disk_buffer.cpp; the host test installs its own. */
typedef void (*GdxDiskOverlayFn)(unsigned int offset, unsigned int length, void* dst);

/* Opens `path` and arms the cache. `expectedSize` is the exact size the image must have
 * (0 = accept any non-zero size). `blockLog2` is the cache block size (12..18 inclusive);
 * `cacheBlocks` is rounded UP to a multiple of GDX_DISK_STREAM_WAYS and clamped to
 * [GDX_DISK_STREAM_WAYS, 4096].
 *
 * Returns 1 on success. Returns 0 -- with nothing allocated and the stream left inactive --
 * when the file is missing, is the wrong size, or the cache cannot be allocated. A file that
 * is short is REFUSED here rather than read through: the resident path's own guard
 * (disk_buffer.cpp: "a truncated .ndd OOB-reads immediately") applies with more force to a
 * reader that would otherwise serve a short final block silently. */
int gdx_disk_stream_open(const char* path, unsigned int expectedSize,
                         unsigned int blockLog2, unsigned int cacheBlocks);

void gdx_disk_stream_close(void);
int gdx_disk_stream_active(void);
unsigned int gdx_disk_stream_size(void);
unsigned int gdx_disk_stream_cache_bytes(void);
unsigned int gdx_disk_stream_block_bytes(void);
const char* gdx_disk_stream_path(void);

/* Reads [offset, offset+length) into `dst`, crossing block boundaries as needed, then
 * applies the overlay. Returns 1 on full success.
 *
 * Returns 0 on refusal: out of range, or a short/failed host read. ON REFUSAL `dst` IS NOT
 * WRITTEN AT ALL -- not zeroed, not partially filled. That rule is deliberate and it is
 * ekdeadlock's lesson restated: never write through a pointer derived from data you have
 * just declared unloadable (docs/research/native/ekdeadlock-progress.md section 6, where a
 * bzero of a declined destination overwrote its own receipt string inside the program image).
 * A caller that ignores the return value gets its old bytes, not new wrong ones. */
int gdx_disk_stream_read(unsigned int offset, unsigned int length, void* dst);

/* Drops any cached blocks covering [offset, offset+length). Called after a 64DD write so the
 * next read re-composes that range through the overlay. */
void gdx_disk_stream_invalidate(unsigned int offset, unsigned int length);

/* Reads straight off the file, bypassing the cache AND the write overlay -- the same source
 * gdx_disk_stream_crc64 sweeps. For reads whose bytes are never revisited and whose subject
 * is the PRISTINE image: the .dcrc sample fingerprint (B11m) is both. Going through the cache
 * cost it a whole 16 KiB block per 4 KiB window -- 1,312 KB off the card for a 256 KiB
 * sample, measured -- and then threw every one of them away again, because a cache warmed
 * with 64 scattered windows is a cache full of the wrong blocks. Same refusal contract:
 * on 0, `dst` is not written. */
int gdx_disk_stream_read_uncached(unsigned int offset, unsigned int length, void* dst);

void gdx_disk_stream_set_overlay(GdxDiskOverlayFn fn);

/* The overlay reports how many bytes it actually rewrote, so `ovlKB` in the receipt is a
 * measurement rather than a restatement of the read size. */
void gdx_disk_stream_note_overlay(unsigned int bytes);

/* CRC-64/XZ over the whole image, streamed in block-sized chunks. Bypasses the cache (a
 * whole-image sweep would evict everything useful and gain nothing), so it does not disturb
 * a warm cache. Returns 0 and sets *ok to 0 if any chunk fails to read. */
unsigned long long gdx_disk_stream_crc64(int* ok);

void gdx_disk_stream_get_stats(GdxDiskStreamStats* out);
void gdx_disk_stream_reset_stats(void);

/* The number of DISTINCT image blocks fetched since the last reset -- the working set, which
 * is the quantity that actually sizes the cache. `blockMisses - this` is the re-fetch count:
 * the blocks the card had to serve twice because the cache could not hold the set. A cache at
 * or above the working set cannot re-fetch at all, so the sweep that chooses
 * `[debug] disk_stream_cache_kb` reads these two numbers and nothing else. */
unsigned long long gdx_disk_stream_unique_blocks(void);

/* ekaudio2 COALESCED FILL. Makes [offset, offset+length) resident, reading each run of
 * consecutive MISSING blocks with ONE fseek/fread of up to `maxRunBytes` instead of one per
 * block. `extra` (READ-AHEAD) extends the range by that many bytes, but only when the range
 * itself has a missing block -- so a sequential walk pays one large request every few calls
 * instead of topping the window up by a block on every call. The whole fill is capped at half
 * the cache. Resident blocks split runs and are left alone; a lone missing block is skipped (the
 * demand read fetches it at the same one-request cost). A run is capped at the set count and
 * at half the cache, so it can neither evict itself nor flush the working set. Blocks it
 * installs count as fills and as speculative fills (`pf=` in the receipt), and as working
 * set, exactly as a demand fill would.
 *
 * Purely an optimisation: it copies nothing to the caller, applies no overlay, and a failed
 * or short read installs nothing (the demand read then refuses on its own terms). Returns the
 * number of SD requests issued, 0 when there was nothing to do, -1 on a failed read.
 * `[debug] disk_stream_coalesce_kb` (port/n64_leo.c) is the lever; this is the mechanism. */
int gdx_disk_stream_fill_range(unsigned int offset, unsigned int length, unsigned int extra,
                               unsigned int maxRunBytes);

/* SD requests the coalesced fill has issued, and the blocks they installed, since boot. */
void gdx_disk_stream_fill_stats(unsigned long long* runs, unsigned long long* blocks);

/* Formats the stats into `buf` as the one-line `[diskstream]` receipt body. Returns `buf`. */
const char* gdx_disk_stream_format_stats(char* buf, unsigned int bufSize);

#define GDX_DISK_STREAM_WAYS 4
#define GDX_DISK_STREAM_DEFAULT_BLOCK_LOG2 14u /* 16 KiB */
#define GDX_DISK_STREAM_DEFAULT_BLOCKS 128u    /* 32 sets x 4 ways x 16 KiB = 2 MiB */

#ifdef __cplusplus
}
#endif

#endif /* GDX_DISK_STREAM_H */
