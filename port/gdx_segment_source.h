#pragma once
/* Single byte-source shim for venue/geometry blob migration.
 *
 * Every chokepoint that reads the cartridge image routes through here rather than
 * doing `memcpy(dst, gdx_rom_buffer + romBase, size)` itself, so one place decides
 * where the bytes come from:
 *   1. archive-first  -- the generated segment_blob table (containment lookup),
 *   2. raw-ROM fallback -- gdx_rom_buffer, byte-identical to the old direct read.
 *
 * Blob entries are verbatim ROM slices: MIO0 families stay compressed,
 * uncompressed families stay big-endian. Every consumer keeps its existing
 * decode/swap step, so the result is provably byte-equal to the pre-shim path.
 */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Copy `size` bytes of the cartridge image starting at absolute ROM offset
 * `romBase` into `dst`. Resolves archive-first (blob whose span fully contains
 * [romBase, romBase+size)), else copies from the raw ROM image. Returns 1 on
 * success (either source), 0 only when no source can satisfy the read (ROM image
 * absent or the range is out of bounds) -- in which case `dst` is left untouched
 * and the caller applies its own miss policy (matching the old bounds check). */
int GdxSegmentSourceRead(uint32_t romBase, uint32_t size, void* dst);

/* Span of the archive blob (if any) whose verbatim ROM slice contains `romBase`,
 * measured from `romBase` to the blob's end. A caller that must stage a whole
 * family span whose byte length is not known a priori (an MIO0 stream: the
 * compressed length is not carried in the header) uses this to size its staging
 * read to the family blob, so the staged `GdxSegmentSourceRead` stays
 * archive-first (fully contained) instead of resolving to the raw-ROM fallback.
 * Writes `*outSpan` and returns 1 when a blob contains `romBase`; returns 0
 * otherwise (no matching blob -- the caller bounds its stage by other means).
 * Pure table lookup: no lock, no allocation, no ROM read. */
int GdxSegmentSourceContainingSpan(uint32_t romBase, uint32_t* outSpan);

/* Telemetry: total raw-ROM fallback reads across every family plus the unmapped
 * bucket. Target 0 once the archive is complete. */
unsigned int gdx_segment_source_fallback_total(void);

/* Per-family telemetry iterator: call with index 0,1,2,... until it returns 0 (one past
 * the last populated family). On a hit it writes the family's archive key to *outKey and
 * its raw-ROM fallback count to *outFallbackReads (either may be NULL) and returns 1. The
 * unmapped bucket is NOT a family and shows up only in gdx_segment_source_fallback_total().
 * Each index is a snapshot; counts may advance between calls under concurrent reads. */
int GdxSegmentSourceFamilyStats(unsigned int index, const char** outKey,
                                unsigned int* outFallbackReads);

/* Boot-time preload: forces the lazy archive load of the blob family whose verbatim ROM
 * slice contains `romBase`, so the payload is resident before the first audio DMA rather
 * than loading on a game/audio tick. Returns 1 when the payload is resident from the
 * archive, 0 when the family is absent or the load failed -- reads then degrade silently
 * to the raw-ROM fallback. Idempotent: a second call returns the cached state. */
int GdxSegmentSourcePreload(uint32_t romBase);

/* Boot-time split of GdxSegmentSourcePreload for hosts that run the (long, SD-bound)
 * archive read on a background worker: pre-allocates the family's process-lifetime
 * staging buffer WITHOUT touching the archive and hands back the payload view + its
 * capacity, so the address can be gdx_register_host_range()'d on the MAIN thread (the
 * host-range vector is not thread-safe) before the worker calls GdxSegmentSourcePreload.
 * Idempotent; an already-loaded family returns its live payload/size. Returns 0 when the
 * family is unmapped, already failed, or the allocation failed. */
int GdxSegmentSourcePreallocPayload(uint32_t romBase, void** outPayload, uint32_t* outCap);

/* Millisecond monotonic clock backing the [seg-src]/[audio-load] load receipts; exported
 * so decomp-side diag code can time loads without platform headers. */
double gdx_host_ms_now(void);

/* Companion getter to GdxSegmentSourcePreload: hands back the resident,
 * immutable (process-lifetime) payload view + its byte length for the blob
 * family containing `romBase`, so a caller can register it with the host-range
 * marshaller (truncated-low32 tokens of blob-served buffers must resolve exactly
 * like gAudioHeap/gdx_rom_buffer). Writes outPayload and outSize, and
 * returns 1 only when the family payload is resident; returns 0 (out-params
 * untouched) when the family is unmapped or not loaded from the archive. */
int GdxSegmentSourcePayload(uint32_t romBase, void** outPayload, uint32_t* outSize);

#ifdef __cplusplus
}
#endif
