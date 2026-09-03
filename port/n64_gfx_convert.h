// port/n64_gfx_convert.h -- narrow-to-wide display-list boundary converters.
//
// Converts a binary N64-format (8-byte) display list into the wide 16-byte layout the bridge's
// wide fast-path consumes, ONCE, and caches the result, so the per-frame path never re-parses the
// narrow list nor consults the guessing resolver for pointers this stage already resolved
// deterministically.
//
// The output packet layout matches decomp's PORT `Gwords` and the bridge's wide reader exactly:
// w0 at byte 0, four bytes of padding, w1 (pointer-width) at byte 8, 16 bytes total (see
// n64_gfx_bridge.cpp kHostBuiltGfxStride / sourceIsWide).
//
// Deliberately self-contained: depends only on the standard library and an injected
// ConvertContext of deterministic lookups, so it builds and unit-tests as a standalone console
// exe (gdx_gfx_convert_tests) with no bridge, libultraship, or decomp headers.
#ifndef GDX_N64_GFX_CONVERT_H
#define GDX_N64_GFX_CONVERT_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace gdx {

// Byte-identical to decomp PORT `Gwords { u32 w0; GfxW1 w1; }` and the bridge wide reader.
struct WideGfx {
    uint32_t w0;
    uint32_t _pad;
    uint64_t w1;
};
static_assert(sizeof(WideGfx) == 16, "wide Gfx packet must be 16 bytes");
static_assert(offsetof(WideGfx, w0) == 0, "w0 must be at byte 0");
static_assert(offsetof(WideGfx, w1) == 8, "w1 must be at byte 8");

// The converter NEVER guesses: it asks the caller's context to resolve a physical pointer token
// against the known load context (RDRAM arena / ROM buffer / asset-segment base). A one-time
// lookup with full information, not the per-frame low32 reconstruction.
struct ConvertContext {
    // `raw` is a NON-segmented pointer token (a KSEG0/KSEG1 or bare physical RDRAM address).
    // Return false to leave the 32-bit token in place (high32 == 0) so the draw-time path still
    // handles it unchanged. Only invoked for pointer-carrying opcodes, never for value words.
    bool (*resolve_physical)(void* user, uint32_t raw, size_t required_bytes, uintptr_t* out_host) = nullptr;
    void* user = nullptr;
};

// Anything not explicitly a pointer is treated as a VALUE and copied verbatim (high32 == 0),
// which is what keeps colors / combine modes / vertex indices from ever being mistaken for a
// pointer and corrupted.
enum class W1Kind : uint8_t { Value, DataPtr, SubDlPtr };

// Mirrors the pointer-carrying cases of the bridge's ProcessList switch; the two must agree.
W1Kind ClassifyW1(uint8_t op, bool isF3d);

// A segment index in the top byte (0x01..0x0F). These are left as 32-bit values so the draw-time
// segment-table lookup resolves them, exactly as the game-emitted wide lists do.
inline bool IsSegmentedToken(uint32_t raw) {
    const uint8_t top = static_cast<uint8_t>(raw >> 24);
    return (top >= 0x01) && (top <= 0x0F);
}

// Walks up to `max_commands` 8-byte packets from `src`, byteswapping when `is_big`, stopping
// after the first G_ENDDL (F3DEX2 0xDF / F3D 0xB8). The output is always terminated.
std::vector<WideGfx> ConvertList(const void* src, size_t max_commands, bool is_big,
                                 bool is_f3d, const ConvertContext& ctx);

// Cross-frame cache keyed by narrow source address, with a caller-driven invalidation stamp.
// Reference stability of std::unordered_map mapped values is what guarantees the returned buffer
// address stays valid — and identical for the same source — until it is invalidated or evicted.
class GfxWideCache {
  public:
    void SetContext(const ConvertContext& ctx) { mCtx = ctx; }
    const ConvertContext& Context() const { return mCtx; }

    // [traffic] Optional stamp-mismatch revalidator. On a stamp mismatch the cache asks this
    // predicate whether the cached conversion is still byte-valid (the bridge answers with a
    // DMA dirty-range overlap test against the narrow source span); a `true` return refreshes
    // the stored stamp WITHOUT the ConvertList rebuild. nCachedCmds is the cached entry's
    // command count, i.e. the narrow span the original walk covered (8 bytes per command).
    using StampRevalidateFn = bool (*)(const void* src, size_t nCachedCmds, uint64_t oldStamp);

    // Per-frame cache traffic counters, drained by the bridge's [wide] census.
    struct Stats {
        uint32_t hits = 0;       // stamp matched, cached conversion returned
        uint32_t revalidated = 0; // stamp differed but the revalidator proved the bytes clean
        uint32_t rebuilds = 0;   // stamp differed and the source really changed -> ConvertList
        uint32_t builds = 0;     // first encounter -> ConvertList
        uint32_t rebuiltCmds = 0; // commands re-walked by rebuilds+builds this frame
    };
    Stats DrainStats() {
        Stats out = mStats;
        mStats = Stats{};
        return out;
    }

    // Rebuilds when no entry exists or the stored stamp differs from `stamp` (unless
    // `revalidate` proves the cached bytes still match, in which case the stamp refreshes).
    const std::vector<WideGfx>& GetOrBuild(const void* src, size_t max_commands, bool is_big,
                                           bool is_f3d, uint64_t stamp,
                                           StampRevalidateFn revalidate = nullptr);

    // Drop a single source (e.g. its backing memory was reloaded).
    void Invalidate(const void* src);
    // Drop everything (e.g. a global asset epoch bump).
    void Clear();
    size_t CachedCount() const { return mCache.size(); }
    bool Contains(const void* src) const { return mCache.find(src) != mCache.end(); }

    // Payload bytes held across frames (capacity, since vectors over-allocate). O(entries);
    // intended for low-frequency memory-census logging, not per-frame use.
    size_t BytesUsed() const {
        size_t total = 0;
        for (const auto& kv : mCache) {
            total += kv.second.cmds.capacity() * sizeof(WideGfx);
        }
        return total;
    }

    // Must be called once per gdx_gfx_run, before any GetOrBuild call for that frame. Advances
    // the frame counter and, only once the cache has grown past kEvictHighWatermark entries,
    // sweeps it in one O(size) pass evicting entries more than kStaleFrameLimit frames stale.
    // Eviction needs no invalidation path of its own: GetOrBuild's stamp-mismatch rebuild already
    // handles a source whose backing memory changed, so an evicted source that is revisited just
    // rebuilds instead of returning a dangling reference. Returns the number evicted.
    size_t BeginFrame();

  private:
    struct Entry {
        std::vector<WideGfx> cmds;
        uint64_t stamp = 0;
        uint64_t lastUseFrame = 0;
    };
    static constexpr size_t kEvictHighWatermark = 512;
    static constexpr uint64_t kStaleFrameLimit = 600;

    ConvertContext mCtx{};
    std::unordered_map<const void*, Entry> mCache;
    uint64_t mCurrentFrame = 0;
    Stats mStats{};
};

}  // namespace gdx

#endif  // GDX_N64_GFX_CONVERT_H
