/* port/3ds/gfx/n64/gdx_texcache.cpp -- content-keyed texture cache (1024, LRU) and the
 * N64 -> RGBA32 decoders lifted from Interpreter::ImportTexture* (interpreter.cpp 1771-3226).
 * Plan §3.7. Uploads go through the backend's SelectTexture/UploadTexture/SetSamplerParameters,
 * unchanged. Decode-path selection mirrors LUS: an `immutable` (resource-backed) source takes
 * the legacy decode from the source bytes; raw RGBA16 decodes from the TMEM mirror. */
#include "gdx_interp.h"

#include <new>
#include <string.h>

#include "gdx_log.h"
#include "gdx3ds_cov.h" // COVERAGE: [cov] per-frame feature census ([debug] cov)
#include "gdx3ds_hitch.h" // HITCH: per-frame event counters
#include "gdx3ds_hwprof.h" // hwprof [hwsite]: GDX3DS_HWS_* site ids
#include "gdx3ds_hotcold.h" // hotcold: GDX_COLD / GDX_UNLIKELY (per-function attributes only)

/* hwprof [hwsite] (port/3ds/include/gdx3ds_hwprof.h): per-call timers behind [debug] hwsite. The
 * seam returns 0 when the lever is off (one call per site); the host test stubs it. */
extern "C" long long gdx_ninterp_hws_begin(void);
extern "C" void gdx_ninterp_hws_end(int site, long long t0);
namespace {
struct InterpHwsScope {
    int site;
    long long t0;
    explicit InterpHwsScope(int s) : site(s), t0(gdx_ninterp_hws_begin()) {}
    ~InterpHwsScope() {
        if (t0 != 0) {
            gdx_ninterp_hws_end(site, t0);
        }
    }
    InterpHwsScope(const InterpHwsScope&) = delete;
    InterpHwsScope& operator=(const InterpHwsScope&) = delete;
};
} // namespace

extern "C" int gdx_ninterp_palhash(void);

namespace gdxn {

#define SCALE_5_8(v) (((v) * 0xFF) / 0x1F)
#define SCALE_4_8(v) ((v) * 0x11)
#define SCALE_3_8(v) ((v) * 0x24)

static inline bool KeyEq(const TexKey& a, const TexKey& b) {
    return memcmp(&a, &b, sizeof(TexKey)) == 0;
}
static inline uint32_t KeyHash(const TexKey& k) {
    const uintptr_t addr = reinterpret_cast<uintptr_t>(k.addr);
    uint32_t h = static_cast<uint32_t>(addr ^ (addr >> 5));
    h ^= k.content_hash * 0x9E3779B1u;
    h ^= k.palette_content_hash * 0x85EBCA6Bu;
    h ^= (static_cast<uint32_t>(k.fmt) << 24) ^ (static_cast<uint32_t>(k.siz) << 20) ^ k.size_bytes;
    h ^= h >> 15;
    return h & (kTexCacheHash - 1);
}

/* == ApplyTileMaskExtent */
static void ApplyTileMaskExtent(const Rdp& rdp, int tile, uint32_t& width, uint32_t& height, bool maskAuthoritative) {
    const Tile& t = rdp.texture_tile[tile];
    if (t.masks != TX_NOMASK && t.masks < 31) {
        if (maskAuthoritative && (t.cms & TX_CLAMP) == 0) {
            width = 1u << t.masks;
        } else if (width > (1u << t.masks)) {
            width = 1u << t.masks;
        }
    }
    if (t.maskt != TX_NOMASK && t.maskt < 31) {
        if (maskAuthoritative && (t.cmt & TX_CLAMP) == 0) {
            height = 1u << t.maskt;
        } else if (height > (1u << t.maskt)) {
            height = 1u << t.maskt;
        }
    }
}

static inline uint32_t GetEffectiveLineSize(uint32_t lineSizeBytes, uint32_t fullImageLineSizeBytes,
                                            uint32_t sizeBytes, uint32_t tileLineSizeBytes) {
    if ((lineSizeBytes != sizeBytes || fullImageLineSizeBytes != sizeBytes) && lineSizeBytes > 0) {
        return lineSizeBytes;
    }
    return tileLineSizeBytes;
}

/* ---- cache ----------------------------------------------------------------------------- */

static void LruUnlink(TexEntry* tex, int16_t& head, int16_t& tail, int16_t i) {
    TexEntry& e = tex[i];
    if (e.lruPrev >= 0) tex[e.lruPrev].lruNext = e.lruNext; else head = e.lruNext;
    if (e.lruNext >= 0) tex[e.lruNext].lruPrev = e.lruPrev; else tail = e.lruPrev;
    e.lruPrev = e.lruNext = -1;
}
static void LruPushBack(TexEntry* tex, int16_t& head, int16_t& tail, int16_t i) {
    TexEntry& e = tex[i];
    e.lruPrev = tail;
    e.lruNext = -1;
    if (tail >= 0) tex[tail].lruNext = i; else head = i;
    tail = i;
}

/* [native] hud_retain: a retained HUD run's cache hit, replayed -- the LRU refresh
 * TexCacheLookup's hit makes (the binding and the mRs slot ride the recorded run). */
void Interp::TexCacheTouch(int16_t slot) {
    LruUnlink(mTex, mTexLruHead, mTexLruTail, slot);
    LruPushBack(mTex, mTexLruHead, mTexLruTail, slot);
}

/* == TextureCacheLookup: true = hit (bound, LRU refreshed); false = decode into the bound id. */
bool Interp::TexCacheLookup(int unit, const TexKey& key) {
    const uint32_t h = KeyHash(key);
    for (int16_t i = mTexHash[h]; i >= 0; i = mTex[i].hashNext) {
        if (KeyEq(mTex[i].key, key)) {
            if (mTex[i].gen != mTexGen) {
                mTex[i].gen = mTexGen; /* N60TEX tex_keep: a kept entry served this generation */
                mTexKeep.keptHits++;
            }
            mBackend->SelectTexture(unit, mTex[i].texture_id);
            mRs.textures[unit] = i;
            LruUnlink(mTex, mTexLruHead, mTexLruTail, i);
            LruPushBack(mTex, mTexLruHead, mTexLruTail, i);
            if (!mTex[i].uploaded) {
                mRs.sampler_valid[unit] = true;
                mRs.sampler_linear[unit] = false;
                mRs.sampler_cms[unit] = 0;
                mRs.sampler_cmt[unit] = 0;
                return false;
            }
            mStats.texHits++;
            return true;
        }
    }
    mStats.texMisses++;
    /* COVERAGE: a miss frame is a frame that runs the decode+upload path; an evict frame is
     * one that runs the LRU retire path. Both were invisible to the old hand-placed SHOT
     * schedules, and `dropsites` had to FORCE all four of its sites because none had ever
     * executed in a normal run (dropsites-progress.md). */
    gdx3ds_cov_mark(GDX3DS_COV_KEY(GDX3DS_COV_FAM_TC, GDX3DS_COV_TC_MISS));
    int16_t slot = -1;
    if (mTexCount >= kTexCacheSize) {
        /* Evict the least recently used; nulling a live binding re-arms the import pipeline. */
        slot = mTexLruHead;
        TexCacheRetireSlot(slot);
        mStats.texEvicts++;
        gdx3ds_hitch_add(GDX_HITCH_EV_TEX_EVICT, 1u); /* HITCH */
        gdx3ds_cov_mark(GDX3DS_COV_KEY(GDX3DS_COV_FAM_TC, GDX3DS_COV_TC_EVICT));
    } else {
        for (unsigned i = 0; i < kTexCacheSize; i++) {
            if (!mTex[i].used) {
                slot = static_cast<int16_t>(i);
                break;
            }
        }
    }
    uint32_t texture_id;
    if (!mTexKeptNone && mTexFreeIdCount == 0 && mBackend->LinearFreeBytes() < mTexKeepFloorBytes) {
        TexKeepRelieve(); /* N60TEX tex_keep: recycle a kept id before growing the backend */
    }
    if (mTexFreeIdCount > 0) {
        texture_id = mTexFreeIds[--mTexFreeIdCount];
    } else {
        texture_id = mBackend->NewTexture();
    }
    TexEntry& e = mTex[slot];
    e.key = key;
    e.gen = mTexGen;
    e.texture_id = texture_id;
    e.uploaded = false;
    e.used = true;
    e.hashNext = mTexHash[h];
    mTexHash[h] = slot;
    e.lruPrev = e.lruNext = -1;
    LruPushBack(mTex, mTexLruHead, mTexLruTail, slot);
    mTexCount++;
    if (mTexCount > mTexCountHw) {
        mTexCountHw = mTexCount; /* B23: how close content-keyed CI textures push the 1024 LRU */
    }
    mBackend->SelectTexture(unit, texture_id);
    mBackend->SetSamplerParameters(unit, false, 0, 0);
    mRs.sampler_valid[unit] = true;
    mRs.sampler_linear[unit] = false;
    mRs.sampler_cms[unit] = 0;
    mRs.sampler_cmt[unit] = 0;
    mRs.textures[unit] = slot;
    return false;
}

/* ---- [native] machines_texskip taps (the rest lives in gdx_texskip.cpp) ------------------ */

/* Inside a skip window: a tile the skipped list set must be an immutable (source-keyed,
 * source-decoded) texture, or its import would read TMEM the list never wrote. */
void Interp::TexSkipCheckImport(int tile) {
    const int t = tile & 7;
    if (((mTexOvMask >> t) & 1u) == 0) {
        return; /* a tile the list never sets (the race setup's tile 0): live, stock state */
    }
    mTexSkipStats.ovImports++;
    if (!mTexOv->lt[t].immutable || mTexOv->lt[t].addr == nullptr) {
        TexSkipTrip(2, static_cast<uint32_t>(t));
    }
}

/* The tripwire: something the skip's scan said could not happen inside a window did. kind 1 =
 * a texture-state writer (detail = opcode), 2 = an import of a set tile whose source is not
 * immutable (detail = tile), 3 = an unexpected G_DL, 4 = the task ended with the window open. The window closes on
 * 3/4; the frame may differ from the stock stream from here on, which is what a nonzero trips=
 * on the [texskip] receipt means. */
GDX_COLD void Interp::TexSkipTrip(int kind, uint32_t detail) { /* hotcold: a tripwire, cold */
    mTexSkipStats.trips++;
    switch (kind) {
        case 1: mTexSkipStats.tripWriter++; break;
        case 2: mTexSkipStats.tripImport++; break;
        case 3: mTexSkipStats.tripDl++; break;
        default: mTexSkipStats.tripTaskEnd++; break;
    }
    if (mTexSkipTripLogs < 16) {
        mTexSkipTripLogs++;
        GDXN_LOG("[texskip] TRIP kind=%d detail=%08lx body=%p (1 writer, 2 non-immutable import, 3 dl, 4 task end)",
                 kind, static_cast<unsigned long>(detail), mTexOvBody);
    }
}

/* == TexCacheRetireSlot: unbind, unhash, unlink, hand the backend texture id back to the
 * free list. The one place an entry leaves the cache (LRU eviction, clear, address delete),
 * so every exit re-arms the import pipeline the same way. */
void Interp::TexCacheRetireSlot(int16_t slot) {
    if (slot < 0) return;
    TexEntry& v = mTex[slot];
    if (!v.used) return;
    TexCacheSignalRetire(slot);
    if (mTexFreeIdCount < kTexCacheSize) {
        mTexFreeIds[mTexFreeIdCount++] = v.texture_id;
    }
    const uint32_t vh = KeyHash(v.key);
    int16_t* pp = &mTexHash[vh];
    while (*pp >= 0 && *pp != slot) pp = &mTex[*pp].hashNext;
    if (*pp == slot) *pp = v.hashNext;
    LruUnlink(mTex, mTexLruHead, mTexLruTail, slot);
    v.used = false;
    v.uploaded = false;
    v.hashNext = -1;
    mTexCount--;
}

/* == TexCacheClear: LUS Interpreter::TextureCacheClear's counterpart. Raised by
 * gdx_rdram_mode_reset (decomp_port.c) at every mode transition, because the bump arena
 * rewinds and re-hands the same host addresses to the next mode's content. */
void Interp::TexCacheClear() {
    if (mTexKeepBytes != 0) {
        TexCacheClearKeeping();
        return;
    }
    mStats.texClears++;
    for (unsigned i = 0; i < kTexCacheSize; i++) {
        TexCacheRetireSlot(static_cast<int16_t>(i));
    }
}

/* native-all S1 [native] atlas_race_reset: the race starts on an empty atlas. Every atlas-backed
 * entry is retired here, before the clear, so tex_keep cannot keep a view the backend is about to
 * drop (a kept view's next hit would bind a released cell). Standalone textures keep their
 * tex_keep treatment. The views are small HUD-sized textures; the menus re-import theirs once,
 * after the race. */
GDX_COLD unsigned Interp::TexCacheAtlasReset() {
    unsigned retired = 0;
    for (unsigned i = 0; i < kTexCacheSize; i++) {
        if (mTex[i].used && mBackend->TexPage(mTex[i].texture_id) >= 0) {
            TexCacheRetireSlot(static_cast<int16_t>(i));
            retired++;
        }
    }
    mBackend->AtlasResetAll();
    return retired;
}

/* What a retire tells the rest of the interpreter: the nrr state blocks naming the slot are
 * stale, and a live binding of it is dropped (re-arming the import pipeline). */
void Interp::TexCacheSignalRetire(int16_t slot) {
    mNrrTexEpoch++; /* [native] nrr_hud: a state block naming this slot is stale */
    mNrrSlotGen[slot]++;
    for (int j = 0; j < 2; j++) {
        if (mRs.textures[j] == slot) {
            mRs.textures[j] = -1;
            ArmTexturesChanged();
        }
    }
}

/* ---- N60TEX [debug] tex_keep ------------------------------------------------------------------ */

/* An entry whose key names its content: a hit on it after the clear is the decode the clear
 * would have redone (the same bytes under the same tile/format/palette key). */
bool Interp::TexKeepEligible(const TexEntry& e) const {
    return e.used && e.uploaded && e.key.content_hash != 0;
}

/* The clear, keeping the most recently used content-keyed entries within the byte budget. Each
 * kept slot still signals a retire (dependents re-resolve exactly as after a stock clear); it
 * keeps its key, hash chain, LRU place and backend id. */
GDX_COLD void Interp::TexCacheClearKeeping() {
    mStats.texClears++;
    mTexGen++;
    const size_t linFree = mBackend->LinearFreeBytes();
    const uint32_t budget = linFree < mTexKeepFloorBytes ? 0u : mTexKeepBytes;
    uint32_t keptBytes = 0;
    uint32_t kept = 0;
    uint32_t retired = 0;
    for (int16_t i = mTexLruTail; i >= 0;) {
        const int16_t older = mTex[i].lruPrev;
        const uint32_t bytes = TexKeepEligible(mTex[i]) ? mBackend->TexBytes(mTex[i].texture_id) : 0u;
        if (bytes != 0 && keptBytes + bytes <= budget) {
            TexCacheSignalRetire(i);
            keptBytes += bytes;
            kept++;
        } else {
            TexCacheRetireSlot(i);
            retired++;
        }
        i = older;
    }
    mTexKeptNone = kept == 0;
    mTexKeep.clears++;
    mTexKeep.kept = kept;
    mTexKeep.keptKiB = keptBytes / 1024u;
    mTexKeep.retired = retired;
    mTexKeep.linFreeKiB = linFree > 0xFFFFFFFFu ? 0xFFFFFFFFu / 1024u : static_cast<uint32_t>(linFree / 1024u);
    TexKeepLog();
}

GDX_COLD void Interp::TexKeepLog() const {
    GDXN_LOG("[tex-keep] clear#%u kept=%u (%u KiB, budget %u KiB) retired=%u linfree=%u KiB | since last: "
             "kept hits=%u pressure retires=%u",
             static_cast<unsigned>(mTexKeep.clears), static_cast<unsigned>(mTexKeep.kept),
             static_cast<unsigned>(mTexKeep.keptKiB), static_cast<unsigned>(mTexKeepBytes / 1024u),
             static_cast<unsigned>(mTexKeep.retired), static_cast<unsigned>(mTexKeep.linFreeKiB),
             static_cast<unsigned>(mTexKeep.keptHits), static_cast<unsigned>(mTexKeep.pressureRetires));
}

/* A miss with no free id while the linear heap is under the floor: hand back the oldest entry a
 * previous generation left, so this upload recycles its id the way it would after a stock clear. */
GDX_COLD void Interp::TexKeepRelieve() {
    for (int16_t i = mTexLruHead; i >= 0; i = mTex[i].lruNext) {
        if (mTex[i].gen != mTexGen) {
            TexCacheRetireSlot(i);
            mTexKeep.pressureRetires++;
            return;
        }
    }
    mTexKeptNone = true;
}

/* == TexCacheDeleteAddress: LUS Interpreter::TextureCacheDelete's counterpart. Exact-address
 * match, the same semantic minimap.c:196-206 was written against (it invalidates each CI8
 * half separately because an exact match will not cover the second one). */
void Interp::TexCacheDeleteAddress(const uint8_t* addr) {
    mStats.texInvalidates++;
    if (addr == nullptr) return;
    for (unsigned i = 0; i < kTexCacheSize; i++) {
        if (mTex[i].used && mTex[i].key.addr == addr) {
            TexCacheRetireSlot(static_cast<int16_t>(i));
            mStats.texInvalidated++;
        }
    }
}

/* == UploadPrepare -- DROPSITE 1.
 *
 * Pre-fix this was a hard 256 KiB ceiling: a decode that needed more bytes was refused, the
 * decoder returned, and ImportTexture then marked the cache entry `uploaded` regardless. The
 * backend texture id had just been handed out of the free list, so it still held the PREVIOUS
 * texture's pixels -- and because the entry was marked uploaded, every later lookup of that
 * key was a cache HIT serving those pixels. Not lost geometry: wrong pixels, permanently, and
 * `texDecodeOverflow` was printed by nothing.
 *
 * The scratch now GROWS on demand, up to the largest upload the backend could accept anyway
 * (kTexUploadMaxBytes == 1024*1024 RGBA8; GfxRenderingAPIC3D::UploadTexture refuses padW or
 * padH > 1024). Steady-state footprint is unchanged -- nothing grows unless a texture actually
 * needs it -- and a decode that used to be dropped now simply decodes. Only a request above
 * that ceiling, which no upload could ever have displayed, still refuses; ImportTexture then
 * takes the neutral fallback rather than leaving a stale sample behind.
 *
 * mTexDecodeHwBytes is the session high-water: the number that says how much margin a scene
 * really has, the way geomloss's vboHW does for the VBO pool. */
bool Interp::UploadPrepare(uint32_t w, uint32_t h) {
    const uint64_t need64 = static_cast<uint64_t>(w) * h * 4u;
    if (need64 > 0xFFFFFFFFull) {
        mStats.texDecodeOverflow++;
        return false;
    }
    const uint32_t need = static_cast<uint32_t>(need64);
    if (need > mTexDecodeHwBytes) {
        mTexDecodeHwBytes = need;
    }
    if (need <= mTexUploadCap) {
        return true;
    }
    const uint32_t ceiling = (mTexUploadMax != 0 && mTexUploadMax < kTexUploadMaxBytes) ? mTexUploadMax
                                                                                       : kTexUploadMaxBytes;
    if (need > ceiling) {
        mStats.texDecodeOverflow++;
        if (mDropsiteTexLogged == 0) { /* one-shot; ses tex= on [ndrops] is the recurring signal */
            mDropsiteTexLogged = 1;
            GDXN_LOG("DROPSITE: texture decode %ux%u needs %u B, above the %u B ceiling -- this "
                     "texture CANNOT be uploaded (PICA caps a side at 1024); the import takes the "
                     "neutral fallback instead of the id's previous contents",
                     w, h, need, ceiling);
        }
        return false;
    }
    /* Grow to the next power of two at or above `need`, never past the ceiling. */
    uint32_t cap = mTexUploadCap != 0 ? mTexUploadCap : kTexUploadBytes;
    while (cap < need) {
        cap = (cap > ceiling / 2u) ? ceiling : cap * 2u;
    }
    uint8_t* grown = new (std::nothrow) uint8_t[cap];
    if (grown == nullptr) {
        mStats.texDecodeOverflow++;
        if (mDropsiteTexLogged == 0) {
            mDropsiteTexLogged = 1;
            GDXN_LOG("DROPSITE: texture decode %ux%u needs %u B and the scratch could not grow "
                     "from %u B (out of memory); the import takes the neutral fallback",
                     w, h, need, mTexUploadCap);
        }
        return false;
    }
    delete[] mTexUpload;
    mTexUpload = grown;
    mTexUploadCap = cap;
    mStats.texDecodeGrow++;
    GDXN_LOG("DROPSITE: texture decode scratch grown to %u B for a %ux%u decode (%u B) -- pre-fix "
             "this texture was SKIPPED and its cache id kept the previous texture's pixels",
             cap, w, h, need);
    return true;
}

void Interp::Upload(uint32_t w, uint32_t h) {
    mUploadDone = true;
    mBackend->UploadTexture(mTexUpload, w, h);
}

/* == UploadNeutralFallback -- DROPSITE 1's only remaining arm.
 *
 * Reached when a decode genuinely cannot happen: above the PICA ceiling, out of memory, a null
 * source, or an fmt/siz pair no decoder handles. Uploading a small opaque-white block makes the
 * primitive draw UNTEXTURED (TEXEL0 * SHADE with TEXEL0 = white is the shade colour) instead of
 * sampling whatever the recycled backend id last held. The entry is deliberately left
 * `uploaded == false`, so the next lookup misses and retries the decode: the fallback is one
 * frame's worth of defined pixels, not a cached answer.
 *
 * Alternatives considered and rejected:
 *  - evict the id: mRs.textures[unit] is already selected and this draw is still going, so
 *    retiring the slot here leaves the draw with no binding at all (a bindMiss, which does not
 *    rebind the PICA texunit either -- the same stale sample by a longer road) and churns the
 *    LRU once per frame for as long as the condition holds.
 *  - decode in tiles: the backend uploads one whole texture per id, so a tiled decode would
 *    need UV surgery the reference path does not do, and would diverge from LUS by construction.
 *  - do nothing but count it: that is the bug. */
void Interp::UploadNeutralFallback(int unit) {
    (void)unit;
    /* Its own buffer, not the decode scratch: the fallback must still work in exactly the case
     * where the scratch is the thing that could not be made big enough. */
    memset(mFallbackTexels, 0xFF, sizeof(mFallbackTexels));
    mStats.texFallback++;
    mBackend->UploadTexture(mFallbackTexels, kFallbackTexDim, kFallbackTexDim);
}

static inline uint16_t Texel16(const uint8_t* p, bool native) {
    return native ? static_cast<uint16_t>((p[1] << 8) | p[0]) : static_cast<uint16_t>((p[0] << 8) | p[1]);
}



/* ---- N60TEX [debug] tex_fast: the decoders' table forms ----------------------------------------
 * Each writes, for every texel, the SAME four bytes (R, G, B, A in memory order) the reference
 * loop below it stores one at a time -- as one little-endian word, from a table instead of a
 * multiply/divide per channel. Byte-for-byte identical output; port/tests/gdx_interp_host_tests
 * and the parity gates compare. */
extern "C" int gdx3ds_tex_fast_enabled(void);

static const uint8_t kScale5To8[32] = { 0,   8,   16,  24,  32,  41,  49,  57,  65,  74,  82,
                                        90,  98,  106, 115, 123, 131, 139, 148, 156, 164, 172,
                                        180, 189, 197, 205, 213, 222, 230, 238, 246, 255 };

static inline uint32_t PackRgbaWord(uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
    return r | (g << 8) | (b << 16) | (a << 24);
}

static inline void PutTexelWord(uint8_t* dst, uint32_t i, uint32_t word) {
    memcpy(dst + 4u * i, &word, 4);
}

/* == the reference's SCALE_5_8 channels and `a ? 255 : 0` of one RGBA5551 texel */
static inline uint32_t Rgba5551Word(uint32_t col16) {
    return PackRgbaWord(kScale5To8[col16 >> 11], kScale5To8[(col16 >> 6) & 0x1f], kScale5To8[(col16 >> 1) & 0x1f],
                        (col16 & 1u) ? 255u : 0u);
}

/* == SCALE_4_8 intensity in R, G, B and SCALE_4_8 alpha (IA8: high nibble I, low nibble A) */
static inline uint32_t Ia8Word(uint32_t byte) {
    const uint32_t i = SCALE_4_8(byte >> 4);
    return PackRgbaWord(i, i, i, SCALE_4_8(byte & 0xfu));
}

static void FastRgba16Tmem(uint8_t* dst, const uint8_t* tmemSrc, uint32_t lineBytes, uint32_t width,
                           uint32_t height) {
    uint32_t px = 0;
    for (uint32_t y = 0; y < height; y++) {
        const uint8_t* row = tmemSrc + static_cast<size_t>(y) * lineBytes;
        for (uint32_t x = 0; x < width; x++) {
            PutTexelWord(dst, px++, Rgba5551Word((static_cast<uint32_t>(row[2 * x]) << 8) | row[2 * x + 1]));
        }
    }
}

static void FastRgba32(uint8_t* dst, const uint8_t* addr, uint32_t stridePx, uint32_t width, uint32_t height) {
    for (uint32_t y = 0; y < height; y++) {
        memcpy(dst + static_cast<size_t>(y) * width * 4u, addr + static_cast<size_t>(y) * stridePx * 4u,
               static_cast<size_t>(width) * 4u);
    }
}

static void FastIA8(uint8_t* dst, const uint8_t* addr, uint32_t full, uint32_t width, uint32_t height) {
    uint32_t i = 0;
    for (uint32_t y = 0; y < height; y++) {
        const uint8_t* row = addr + static_cast<size_t>(y) * full;
        for (uint32_t x = 0; x < width; x++) {
            PutTexelWord(dst, i++, Ia8Word(row[x]));
        }
    }
}

static void FastIA16(uint8_t* dst, const uint8_t* addr, uint32_t full, uint32_t width, uint32_t height) {
    uint32_t i = 0;
    for (uint32_t y = 0; y < height; y++) {
        const uint8_t* row = addr + static_cast<size_t>(y) * (full / 2) * 2;
        for (uint32_t x = 0; x < width; x++) {
            const uint32_t intensity = row[2 * x];
            PutTexelWord(dst, i++, PackRgbaWord(intensity, intensity, intensity, row[2 * x + 1]));
        }
    }
}

static void FastI8(uint8_t* dst, const uint8_t* addr, uint32_t full, uint32_t width, uint32_t height) {
    uint32_t i = 0;
    for (uint32_t y = 0; y < height; y++) {
        const uint8_t* row = addr + static_cast<size_t>(y) * full;
        for (uint32_t x = 0; x < width; x++) {
            PutTexelWord(dst, i++, static_cast<uint32_t>(row[x]) * 0x01010101u);
        }
    }
}

/* The 4-bit formats: texel `idx` of the row-major nibble stream, high nibble first. */
static inline uint32_t NibbleAt(const uint8_t* addr, uint32_t idx) {
    return (addr[idx / 2] >> (4 - (idx % 2) * 4)) & 0xfu;
}

static void FastI4(uint8_t* dst, const uint8_t* addr, uint32_t full, uint32_t width, uint32_t height) {
    uint32_t i = 0;
    for (uint32_t y = 0; y < height; y++) {
        const uint32_t rowIdx = y * (full * 2);
        for (uint32_t x = 0; x < width; x++) {
            PutTexelWord(dst, i++, SCALE_4_8(NibbleAt(addr, rowIdx + x)) * 0x01010101u);
        }
    }
}

static void FastIA4(uint8_t* dst, const uint8_t* addr, uint32_t full, uint32_t width, uint32_t height) {
    uint32_t i = 0;
    for (uint32_t y = 0; y < height; y++) {
        const uint32_t rowIdx = y * (full * 2);
        for (uint32_t x = 0; x < width; x++) {
            const uint32_t part = NibbleAt(addr, rowIdx + x);
            const uint32_t intensity = SCALE_3_8(part >> 1);
            PutTexelWord(dst, i++, PackRgbaWord(intensity, intensity, intensity, (part & 1u) ? 255u : 0u));
        }
    }
}

/* A palette's decoded words, filled on first use of each index (the reference reads only the
 * entries the texels use; so does this). */
struct PaletteWords {
    uint32_t word[256];
    uint32_t valid[8];
};

static inline uint32_t PaletteWordAt(PaletteWords& pw, uint32_t idx, const uint8_t* entry) {
    const uint32_t bit = 1u << (idx & 31u);
    if ((pw.valid[idx >> 5] & bit) == 0) {
        pw.word[idx] = Rgba5551Word((static_cast<uint32_t>(entry[0]) << 8) | entry[1]);
        pw.valid[idx >> 5] |= bit;
    }
    return pw.word[idx];
}

static void FastCi4(uint8_t* dst, const uint8_t* addr, const uint8_t* palette, uint32_t full, uint32_t width,
                    uint32_t height) {
    PaletteWords pw;
    memset(pw.valid, 0, sizeof(pw.valid));
    uint32_t i = 0;
    for (uint32_t y = 0; y < height; y++) {
        const uint32_t rowIdx = y * (full * 2);
        for (uint32_t x = 0; x < width; x++) {
            const uint32_t idx = NibbleAt(addr, rowIdx + x);
            PutTexelWord(dst, i++, PaletteWordAt(pw, idx, palette + idx * 2));
        }
    }
}

static void FastCi8(uint8_t* dst, const uint8_t* addr, const uint8_t* const palettes[2], uint32_t full,
                    uint32_t width, uint32_t height) {
    PaletteWords pw;
    memset(pw.valid, 0, sizeof(pw.valid));
    uint32_t i = 0;
    for (uint32_t y = 0; y < height; y++) {
        const uint8_t* row = addr + static_cast<size_t>(y) * full;
        for (uint32_t x = 0; x < width; x++) {
            const uint32_t idx = row[x];
            PutTexelWord(dst, i++, PaletteWordAt(pw, idx, palettes[idx / 128] + (idx % 128) * 2));
        }
    }
}

/* N60TEX tex_fast: the legacy (source-bytes) RGBA16 loop, a texel past `maxTexel` reading 0. */
static void FastRgba16Source(uint8_t* dst, const uint8_t* addr, uint32_t strideTexels, uint32_t maxTexel, bool native,
                             uint32_t width, uint32_t height) {
    uint32_t i = 0;
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            const uint32_t clrIdx = (y * strideTexels) + x;
            const uint32_t col16 = (clrIdx < maxTexel) ? Texel16(addr + 2 * clrIdx, native) : 0u;
            PutTexelWord(dst, i++, Rgba5551Word(col16));
        }
    }
}

/* == ImportTextureRgba16 (both the TMEM path and the legacy path). */
void Interp::DecodeRgba16(int unit, int tile, const LoadedTexture& lt) {
    (void)unit;
    const uint8_t* addr = lt.addr;
    if (addr == nullptr) return;
    const Tile& tt = mRdp.texture_tile[tile];
    if (!lt.immutable && tt.siz == IM_SIZ_16b) {
        const uint32_t tmemByteOffset = static_cast<uint32_t>(tt.tmem_index) * 8u;
        const uint32_t lineBytes = tt.line_size_bytes;
        if (tmemByteOffset < sizeof(mRdp.tmem) && lineBytes >= 2) {
            uint32_t width = lineBytes / 2;
            uint32_t height = (sizeof(mRdp.tmem) - tmemByteOffset) / lineBytes;
            ApplyTileMaskExtent(mRdp, tile, width, height, true);
            const uint32_t tileW = static_cast<uint32_t>((tt.lrs - tt.uls + 4) / 4);
            const uint32_t tileH = static_cast<uint32_t>((tt.lrt - tt.ult + 4) / 4);
            const bool boundW = (tt.cms & TX_CLAMP) != 0 || tt.masks == TX_NOMASK;
            const bool boundH = (tt.cmt & TX_CLAMP) != 0 || tt.maskt == TX_NOMASK;
            if (boundW && tileW > 0 && tileW < width) width = tileW;
            if (boundH && tileH > 0 && tileH < height) height = tileH;
            if (width == 0) width = 1;
            if (height == 0) height = 1;
            if (lineBytes * height > sizeof(mRdp.tmem) - tmemByteOffset) {
                height = (sizeof(mRdp.tmem) - tmemByteOffset) / lineBytes;
            }
            if (width > 0 && height > 0) {
                if (!UploadPrepare(width, height)) return;
                const uint8_t* tmemSrc = mRdp.tmem + tmemByteOffset;
                if (gdx3ds_tex_fast_enabled()) {
                    FastRgba16Tmem(mTexUpload, tmemSrc, lineBytes, width, height);
                    Upload(width, height);
                    return;
                }
                uint32_t px = 0;
                for (uint32_t y = 0; y < height; y++) {
                    const uint8_t* row = tmemSrc + static_cast<size_t>(y) * lineBytes;
                    for (uint32_t x = 0; x < width; x++) {
                        const uint16_t col16 = static_cast<uint16_t>((row[2 * x] << 8) | row[2 * x + 1]);
                        const uint8_t a = col16 & 1;
                        mTexUpload[4 * px + 0] = static_cast<uint8_t>(SCALE_5_8(col16 >> 11));
                        mTexUpload[4 * px + 1] = static_cast<uint8_t>(SCALE_5_8((col16 >> 6) & 0x1f));
                        mTexUpload[4 * px + 2] = static_cast<uint8_t>(SCALE_5_8((col16 >> 1) & 0x1f));
                        mTexUpload[4 * px + 3] = a ? 255 : 0;
                        px++;
                    }
                }
                Upload(width, height);
                return;
            }
        }
    }
    /* Legacy path (resource-backed): decode from the source bytes. */
    const uint32_t sizeBytes = lt.size_bytes;
    uint32_t fullImageLineSizeBytes = lt.full_image_line_size_bytes;
    const uint32_t line_size_bytes = lt.line_size_bytes;
    const uint32_t widthBytes = GetEffectiveLineSize(line_size_bytes, fullImageLineSizeBytes, sizeBytes, tt.line_size_bytes);
    uint32_t width = widthBytes / 2;
    uint32_t height = widthBytes > 0 ? sizeBytes / widthBytes : 0;
    if (fullImageLineSizeBytes == 0 || fullImageLineSizeBytes == sizeBytes) fullImageLineSizeBytes = width * 2;
    const uint32_t tile_w = static_cast<uint32_t>((tt.lrs - tt.uls + 4) / 4);
    const uint32_t tile_h = static_cast<uint32_t>((tt.lrt - tt.ult + 4) / 4);
    const uint32_t loadedPixels = width * height;
    const uint32_t renderedPixels = tile_w * tile_h;
    const bool pyramidLike = renderedPixels > 0 && loadedPixels > renderedPixels && loadedPixels * 8 < renderedPixels * 13;
    const bool clampS = (tt.cms & TX_CLAMP) != 0;
    const bool clampT = (tt.cmt & TX_CLAMP) != 0;
    if ((pyramidLike || clampS) && tile_w > 0 && tile_w < width) width = tile_w;
    if ((pyramidLike || clampT) && tile_h > 0 && tile_h < height) height = tile_h;
    ApplyTileMaskExtent(mRdp, tile, width, height, true);
    const uint32_t rowStrideBytes = (fullImageLineSizeBytes > 0) ? fullImageLineSizeBytes : (width * 2);
    const uint32_t readableBytes = (rowStrideBytes > 0 && height > 0) ? rowStrideBytes * height : sizeBytes;
    const uint32_t maxTexel = readableBytes / 2;
    if (!UploadPrepare(width, height)) return;
    const bool native = lt.native_rgba16 != 0;
    if (gdx3ds_tex_fast_enabled()) {
        FastRgba16Source(mTexUpload, addr, fullImageLineSizeBytes / 2, maxTexel, native, width, height);
        Upload(width, height);
        return;
    }
    uint32_t i = 0;
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            const uint32_t clrIdx = (y * (fullImageLineSizeBytes / 2)) + x;
            const uint16_t col16 = (clrIdx < maxTexel) ? Texel16(addr + 2 * clrIdx, native) : 0;
            const uint8_t a = col16 & 1;
            mTexUpload[4 * i + 0] = static_cast<uint8_t>(SCALE_5_8(col16 >> 11));
            mTexUpload[4 * i + 1] = static_cast<uint8_t>(SCALE_5_8((col16 >> 6) & 0x1f));
            mTexUpload[4 * i + 2] = static_cast<uint8_t>(SCALE_5_8((col16 >> 1) & 0x1f));
            mTexUpload[4 * i + 3] = a ? 255 : 0;
            i++;
        }
    }
    Upload(width, height);
}

void Interp::DecodeRgba32(int tile, const LoadedTexture& lt) {
    const uint8_t* addr = lt.addr;
    if (addr == nullptr) return;
    const Tile& tt = mRdp.texture_tile[tile];
    const uint32_t size_bytes = lt.size_bytes;
    uint32_t full = lt.full_image_line_size_bytes;
    const uint32_t widthBytes = GetEffectiveLineSize(lt.line_size_bytes, full, size_bytes, tt.line_size_bytes * 2);
    uint32_t width = widthBytes / 4;
    uint32_t height = widthBytes > 0 ? size_bytes / widthBytes : 0;
    if (full == 0 || full == size_bytes) full = width * 4;
    const uint32_t tile_w = static_cast<uint32_t>((tt.lrs - tt.uls + 4) / 4);
    const uint32_t tile_h = static_cast<uint32_t>((tt.lrt - tt.ult + 4) / 4);
    const uint32_t loadedPixels = width * height, renderedPixels = tile_w * tile_h;
    const bool pyramidLike = renderedPixels > 0 && loadedPixels > renderedPixels && loadedPixels * 8 < renderedPixels * 13;
    if ((pyramidLike || (tt.cms & TX_CLAMP)) && tile_w > 0 && tile_w < width) width = tile_w;
    if ((pyramidLike || (tt.cmt & TX_CLAMP)) && tile_h > 0 && tile_h < height) height = tile_h;
    ApplyTileMaskExtent(mRdp, tile, width, height, false);
    if (!UploadPrepare(width, height)) return;
    const uint32_t stridePx = full / 4;
    if (gdx3ds_tex_fast_enabled()) {
        FastRgba32(mTexUpload, addr, stridePx, width, height);
        Upload(width, height);
        return;
    }
    uint32_t i = 0;
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            const uint32_t s = y * stridePx + x;
            mTexUpload[4 * i + 0] = addr[4 * s + 0];
            mTexUpload[4 * i + 1] = addr[4 * s + 1];
            mTexUpload[4 * i + 2] = addr[4 * s + 2];
            mTexUpload[4 * i + 3] = addr[4 * s + 3];
            i++;
        }
    }
    Upload(width, height);
}

void Interp::DecodeIA4(int tile, const LoadedTexture& lt) {
    const uint8_t* addr = lt.addr;
    if (addr == nullptr) return;
    const Tile& tt = mRdp.texture_tile[tile];
    const uint32_t sizeBytes = lt.size_bytes;
    uint32_t full = lt.full_image_line_size_bytes;
    const uint32_t widthBytes = GetEffectiveLineSize(lt.line_size_bytes, full, sizeBytes, tt.line_size_bytes);
    uint32_t width = widthBytes * 2;
    uint32_t height = widthBytes > 0 ? sizeBytes / widthBytes : 0;
    if (full == 0 || full == sizeBytes) full = widthBytes;
    ApplyTileMaskExtent(mRdp, tile, width, height, false);
    if (!UploadPrepare(width, height)) return;
    if (gdx3ds_tex_fast_enabled()) {
        FastIA4(mTexUpload, addr, full, width, height);
        Upload(width, height);
        return;
    }
    uint32_t i = 0;
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            const uint32_t srcPixelIdx = y * (full * 2) + x;
            const uint8_t byte = addr[srcPixelIdx / 2];
            const uint8_t part = (byte >> (4 - (srcPixelIdx % 2) * 4)) & 0xf;
            const uint8_t intensity = part >> 1;
            const uint8_t alpha = part & 1;
            mTexUpload[4 * i + 0] = static_cast<uint8_t>(SCALE_3_8(intensity));
            mTexUpload[4 * i + 1] = static_cast<uint8_t>(SCALE_3_8(intensity));
            mTexUpload[4 * i + 2] = static_cast<uint8_t>(SCALE_3_8(intensity));
            mTexUpload[4 * i + 3] = alpha ? 255 : 0;
            i++;
        }
    }
    Upload(width, height);
}

void Interp::DecodeIA8(int tile, const LoadedTexture& lt) {
    const uint8_t* addr = lt.addr;
    if (addr == nullptr) return;
    const Tile& tt = mRdp.texture_tile[tile];
    const uint32_t sizeBytes = lt.size_bytes;
    uint32_t full = lt.full_image_line_size_bytes;
    uint32_t width = GetEffectiveLineSize(lt.line_size_bytes, full, sizeBytes, tt.line_size_bytes);
    uint32_t height = width > 0 ? sizeBytes / width : 0;
    if (full == 0 || full == sizeBytes) full = width;
    ApplyTileMaskExtent(mRdp, tile, width, height, false);
    if (!UploadPrepare(width, height)) return;
    if (gdx3ds_tex_fast_enabled()) {
        FastIA8(mTexUpload, addr, full, width, height);
        Upload(width, height);
        return;
    }
    uint32_t i = 0;
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            const uint32_t s = y * full + x;
            const uint8_t intensity = addr[s] >> 4;
            const uint8_t alpha = addr[s] & 0xf;
            mTexUpload[4 * i + 0] = static_cast<uint8_t>(SCALE_4_8(intensity));
            mTexUpload[4 * i + 1] = static_cast<uint8_t>(SCALE_4_8(intensity));
            mTexUpload[4 * i + 2] = static_cast<uint8_t>(SCALE_4_8(intensity));
            mTexUpload[4 * i + 3] = static_cast<uint8_t>(SCALE_4_8(alpha));
            i++;
        }
    }
    Upload(width, height);
}

void Interp::DecodeIA16(int tile, const LoadedTexture& lt) {
    const uint8_t* addr = lt.addr;
    if (addr == nullptr) return;
    const Tile& tt = mRdp.texture_tile[tile];
    const uint32_t size_bytes = lt.size_bytes;
    uint32_t full = lt.full_image_line_size_bytes;
    const uint32_t widthBytes = GetEffectiveLineSize(lt.line_size_bytes, full, size_bytes, tt.line_size_bytes);
    uint32_t width = widthBytes / 2;
    uint32_t height = widthBytes > 0 ? size_bytes / widthBytes : 0;
    if (full == 0 || full == size_bytes) full = width * 2;
    ApplyTileMaskExtent(mRdp, tile, width, height, false);
    if (!UploadPrepare(width, height)) return;
    if (gdx3ds_tex_fast_enabled()) {
        FastIA16(mTexUpload, addr, full, width, height);
        Upload(width, height);
        return;
    }
    uint32_t i = 0;
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            const uint32_t clrIdx = (y * (full / 2)) + x;
            const uint8_t intensity = addr[2 * clrIdx];
            const uint8_t alpha = addr[2 * clrIdx + 1];
            mTexUpload[4 * i + 0] = intensity;
            mTexUpload[4 * i + 1] = intensity;
            mTexUpload[4 * i + 2] = intensity;
            mTexUpload[4 * i + 3] = alpha;
            i++;
        }
    }
    Upload(width, height);
}

void Interp::DecodeI4(int tile, const LoadedTexture& lt) {
    const uint8_t* addr = lt.addr;
    if (addr == nullptr) return;
    const Tile& tt = mRdp.texture_tile[tile];
    const uint32_t sizeBytes = lt.size_bytes;
    uint32_t full = lt.full_image_line_size_bytes;
    const uint32_t widthBytes = GetEffectiveLineSize(lt.line_size_bytes, full, sizeBytes, tt.line_size_bytes);
    uint32_t width = widthBytes * 2;
    uint32_t height = widthBytes > 0 ? sizeBytes / widthBytes : 0;
    if (full == 0 || full == sizeBytes) full = width / 2;
    const uint32_t tileWidth = static_cast<uint32_t>((tt.lrs - tt.uls + 4) / 4);
    const uint32_t tileHeight = static_cast<uint32_t>((tt.lrt - tt.ult + 4) / 4);
    if (tileWidth > 0 && tileWidth < width) width = tileWidth;
    if (tileHeight > 0 && tileHeight < height) height = tileHeight;
    ApplyTileMaskExtent(mRdp, tile, width, height, false);
    if (sizeBytes != 0 && width > 0 && height > 0) {
        const uint64_t lastIdx = static_cast<uint64_t>(height - 1) * (full * 2) + (width - 1);
        const uint64_t neededBytes = lastIdx / 2 + 1;
        if (neededBytes > sizeBytes) {
            const uint32_t rowBytes = full != 0 ? full : 1u;
            uint32_t maxRows = static_cast<uint32_t>(sizeBytes / rowBytes);
            if (maxRows == 0) maxRows = 1;
            if (height > maxRows) height = maxRows;
        }
    }
    if (!UploadPrepare(width, height)) return;
    if (gdx3ds_tex_fast_enabled()) {
        FastI4(mTexUpload, addr, full, width, height);
        Upload(width, height);
        return;
    }
    uint32_t i = 0;
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            const uint32_t clrIdx = (y * (full * 2)) + x;
            const uint8_t byte = addr[clrIdx / 2];
            const uint8_t part = (byte >> (4 - (clrIdx % 2) * 4)) & 0xf;
            mTexUpload[4 * i + 0] = static_cast<uint8_t>(SCALE_4_8(part));
            mTexUpload[4 * i + 1] = static_cast<uint8_t>(SCALE_4_8(part));
            mTexUpload[4 * i + 2] = static_cast<uint8_t>(SCALE_4_8(part));
            mTexUpload[4 * i + 3] = static_cast<uint8_t>(SCALE_4_8(part));
            i++;
        }
    }
    Upload(width, height);
}

void Interp::DecodeI8(int tile, const LoadedTexture& lt) {
    const uint8_t* addr = lt.addr;
    if (addr == nullptr) return;
    const Tile& tt = mRdp.texture_tile[tile];
    const uint32_t sizeBytes = lt.size_bytes;
    uint32_t full = lt.full_image_line_size_bytes;
    uint32_t width = GetEffectiveLineSize(lt.line_size_bytes, full, sizeBytes, tt.line_size_bytes);
    uint32_t height = width > 0 ? sizeBytes / width : 0;
    if (full == 0 || full == sizeBytes) full = width;
    ApplyTileMaskExtent(mRdp, tile, width, height, false);
    if (!UploadPrepare(width, height)) return;
    if (gdx3ds_tex_fast_enabled()) {
        FastI8(mTexUpload, addr, full, width, height);
        Upload(width, height);
        return;
    }
    uint32_t i = 0;
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            const uint8_t intensity = addr[y * full + x];
            mTexUpload[4 * i + 0] = intensity;
            mTexUpload[4 * i + 1] = intensity;
            mTexUpload[4 * i + 2] = intensity;
            mTexUpload[4 * i + 3] = intensity;
            i++;
        }
    }
    Upload(width, height);
}

void Interp::DecodeCi4(int tile, const LoadedTexture& lt) {
    const uint8_t* addr = lt.addr;
    if (addr == nullptr) return;
    const Tile& tt = mRdp.texture_tile[tile];
    uint32_t full = lt.full_image_line_size_bytes;
    const uint32_t sizeBytes = lt.size_bytes;
    const uint32_t palIdx = tt.palette;
    if (mRdp.palettes[palIdx / 8] == nullptr) return;
    const uint8_t* palette = mRdp.palettes[palIdx / 8] + (palIdx % 8) * 16 * 2;
    const uint32_t baseLine = GetEffectiveLineSize(lt.line_size_bytes, full, sizeBytes, tt.line_size_bytes);
    const uint32_t resultLine = baseLine;
    uint32_t width = resultLine * 2;
    uint32_t height = resultLine > 0 ? sizeBytes / resultLine : 0;
    if (full == 0 || full == sizeBytes) full = resultLine;
    const uint32_t tile_w = static_cast<uint32_t>((tt.lrs - tt.uls + 4) / 4);
    const uint32_t tile_h = static_cast<uint32_t>((tt.lrt - tt.ult + 4) / 4);
    const uint32_t loadedPixels = width * height, renderedPixels = tile_w * tile_h;
    const bool pyramidLike = renderedPixels > 0 && loadedPixels > renderedPixels && loadedPixels * 8 < renderedPixels * 13;
    if ((pyramidLike || (tt.cms & TX_CLAMP)) && tile_w > 0 && tile_w < width) width = tile_w;
    if ((pyramidLike || (tt.cmt & TX_CLAMP)) && tile_h > 0 && tile_h < height) height = tile_h;
    ApplyTileMaskExtent(mRdp, tile, width, height, false);
    if (!UploadPrepare(width, height)) return;
    if (gdx3ds_tex_fast_enabled()) {
        FastCi4(mTexUpload, addr, palette, full, width, height);
        Upload(width, height);
        return;
    }
    uint32_t i = 0;
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            const uint32_t srcPixelIdx = y * (full * 2) + x;
            const uint8_t byte = addr[srcPixelIdx / 2];
            const uint8_t idx = (byte >> (4 - (srcPixelIdx % 2) * 4)) & 0xf;
            const uint16_t col16 = static_cast<uint16_t>((palette[idx * 2] << 8) | palette[idx * 2 + 1]);
            const uint8_t a = col16 & 1;
            mTexUpload[4 * i + 0] = static_cast<uint8_t>(SCALE_5_8(col16 >> 11));
            mTexUpload[4 * i + 1] = static_cast<uint8_t>(SCALE_5_8((col16 >> 6) & 0x1f));
            mTexUpload[4 * i + 2] = static_cast<uint8_t>(SCALE_5_8((col16 >> 1) & 0x1f));
            mTexUpload[4 * i + 3] = a ? 255 : 0;
            i++;
        }
    }
    Upload(width, height);
}

void Interp::DecodeCi8(int tile, const LoadedTexture& lt) {
    const uint8_t* addr = lt.addr;
    if (addr == nullptr) return;
    const Tile& tt = mRdp.texture_tile[tile];
    const uint32_t sizeBytes = lt.size_bytes;
    uint32_t full = lt.full_image_line_size_bytes;
    uint32_t lineSizeBytes = lt.line_size_bytes;
    if (mRdp.palettes[0] == nullptr || mRdp.palettes[1] == nullptr) return;
    const uint32_t baseLine = GetEffectiveLineSize(lineSizeBytes, full, sizeBytes, tt.line_size_bytes);
    lineSizeBytes = baseLine;
    if (full == 0 || full == sizeBytes || full < lineSizeBytes) full = lineSizeBytes;
    if (lineSizeBytes == 0) return;
    uint32_t width = baseLine;
    uint32_t height = baseLine > 0 ? sizeBytes / baseLine : 0;
    const uint32_t tile_w = static_cast<uint32_t>((tt.lrs - tt.uls + 4) / 4);
    const uint32_t tile_h = static_cast<uint32_t>((tt.lrt - tt.ult + 4) / 4);
    const uint32_t loadedPixels = width * height, renderedPixels = tile_w * tile_h;
    const bool pyramidLike = renderedPixels > 0 && loadedPixels > renderedPixels && loadedPixels * 8 < renderedPixels * 13;
    if ((pyramidLike || (tt.cms & TX_CLAMP)) && tile_w > 0 && tile_w < width) width = tile_w;
    if ((pyramidLike || (tt.cmt & TX_CLAMP)) && tile_h > 0 && tile_h < height) height = tile_h;
    ApplyTileMaskExtent(mRdp, tile, width, height, false);
    if (!UploadPrepare(width, height)) return;
    if (gdx3ds_tex_fast_enabled()) {
        FastCi8(mTexUpload, addr, mRdp.palettes, full, width, height);
        Upload(width, height);
        return;
    }
    uint32_t out = 0;
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const uint8_t idx = addr[y * full + x];
            const uint8_t* pal = mRdp.palettes[idx / 128];
            const uint16_t col16 = static_cast<uint16_t>((pal[(idx % 128) * 2] << 8) | pal[(idx % 128) * 2 + 1]);
            const uint8_t a = col16 & 1;
            mTexUpload[4 * out + 0] = static_cast<uint8_t>(SCALE_5_8(col16 >> 11));
            mTexUpload[4 * out + 1] = static_cast<uint8_t>(SCALE_5_8((col16 >> 6) & 0x1f));
            mTexUpload[4 * out + 2] = static_cast<uint8_t>(SCALE_5_8((col16 >> 1) & 0x1f));
            mTexUpload[4 * out + 3] = a ? 255 : 0;
            ++out;
        }
    }
    Upload(width, height);
}

/* FNV-1a, 4 bytes at a time then the tail -- the spelling every other hash in this file uses. */
static inline uint32_t FoldBytes(uint32_t h, const uint8_t* p, uint32_t n) {
    uint32_t b = 0;
    for (; b + 4 <= n; b += 4) {
        uint32_t word;
        memcpy(&word, p + b, 4);
        h = (h ^ word) * 16777619u;
    }
    for (; b < n; b++) {
        h = (h ^ p[b]) * 16777619u;
    }
    return h;
}

/* True when `p` points INSIDE palette_staging, i.e. at bytes the [native] palhash fold below
 * already covers (palhash folds the whole staging half, a superset of any 32-byte sub-palette
 * inside it). Used to keep texhash_ci from folding the same palette bytes into the same key a
 * second time -- audit-2026-09-24 finding 7. The other DpLoadTlut outcome (tmem < 256:
 * palettes[1] = the raw resolved source, staging NOT updated) is NOT covered by palhash, so
 * texhash_ci must still fold it there or that TLUT is invisible to the key. */
static bool PaletteIsStaged(const Rdp& rdp, const uint8_t* p) {
    const uint8_t* stageBegin = &rdp.palette_staging[0][0];
    return p >= stageBegin && p < stageBegin + sizeof(rdp.palette_staging);
}

/* How many of `want` palette bytes are safe to read at `p`. DpLoadTlut leaves palettes[] either
 * inside palette_staging (the tmem>=256 path) or pointing straight at the resolved source (the
 * tmem<256 path), and a raw source need not carry a full 256-byte half. */
static uint32_t PaletteReadable(const Rdp& rdp, const uint8_t* p, uint32_t want) {
    const uint8_t* stageBegin = &rdp.palette_staging[0][0];
    const uint8_t* stageEnd = stageBegin + sizeof(rdp.palette_staging);
    if (p >= stageBegin && p < stageEnd) {
        const uint32_t room = static_cast<uint32_t>(stageEnd - p);
        return want < room ? want : room;
    }
    const size_t readable = DlSrc::ReadableBytes(reinterpret_cast<uintptr_t>(p));
    return static_cast<uint32_t>(want < readable ? want : readable);
}

/* == ImportTexture: key derivation (content hash over exactly the decoded span for the
 * hashed formats; source-byte hash for the immutable class where LUS keyed on the resource
 * identity), lookup, dispatch. */
void Interp::ImportTexture(int unit, int tile) {
    InterpHwsScope hws(GDX3DS_HWS_IMPORT);
    ProfScope prof(mBackend, 4);
    mStats.texImports++;
    mBackend->TexCacheNoteImport();
    /* [native] machines_texskip: inside a skip window the skipped list's TMEM was never
     * written; its tiles' sources are immutable (checked at the skip), whose key and decode
     * below read the source bytes only. The check is the tripwire for that promise. */
    if (mTexOv != nullptr) {
        TexSkipCheckImport(tile);
    }
    if (GDX_UNLIKELY(mNrrTmemLive)) {
        NrrTmemRead(tile); /* [native] nrr_hud: this tile's TMEM may still be a pending NRR load */
    }
    const Tile& tt = mRdp.texture_tile[tile];
    const uint8_t fmt = tt.fmt;
    const uint8_t siz = tt.siz;
    const uint16_t tmemIdx = tt.tmem_index;
    const LoadedTexture lt = LoadedTexAt(tmemIdx);
    const uint8_t* origAddr = lt.addr;
    if (origAddr == nullptr) return;

    if (fmt == IM_FMT_CI && (siz == IM_SIZ_4b || siz == IM_SIZ_8b)) {
        bool paletteMissing;
        if (siz == IM_SIZ_4b) {
            paletteMissing = mRdp.palettes[tt.palette / 8] == nullptr;
        } else {
            paletteMissing = mRdp.palettes[0] == nullptr || mRdp.palettes[1] == nullptr;
        }
        if (paletteMissing) return;
    }

    TexKey key;
    memset(&key, 0, sizeof(key));
    key.addr = origAddr;
    if (fmt == IM_FMT_CI) {
        if (siz == IM_SIZ_4b) {
            const uint8_t palSlot = tt.palette / 8;
            key.pal[0] = palSlot == 0 ? mRdp.palette_dram_addr[0] : nullptr;
            key.pal[1] = palSlot == 1 ? mRdp.palette_dram_addr[1] : nullptr;
        } else {
            key.pal[0] = mRdp.palette_dram_addr[0];
            key.pal[1] = mRdp.palette_dram_addr[1];
        }
    }
    key.fmt = fmt;
    key.siz = siz;
    key.palette_index = tt.palette;
    key.size_bytes = lt.orig_size_bytes;
    key.line_size_bytes = lt.line_size_bytes;
    key.full_image_line_size_bytes = lt.full_image_line_size_bytes;
    const uint32_t cacheTileWidth = tt.lrs >= tt.uls ? static_cast<uint32_t>((tt.lrs - tt.uls + 4u) / 4u) : 0u;
    const uint32_t cacheTileHeight = tt.lrt >= tt.ult ? static_cast<uint32_t>((tt.lrt - tt.ult + 4u) / 4u) : 0u;
    key.tile_width = static_cast<uint16_t>(cacheTileWidth < 0xFFFFu ? cacheTileWidth : 0xFFFFu);
    key.tile_height = static_cast<uint16_t>(cacheTileHeight < 0xFFFFu ? cacheTileHeight : 0xFFFFu);
    key.cms = tt.cms;
    key.cmt = tt.cmt;
    key.masks = tt.masks;
    key.maskt = tt.maskt;

    /* [native] palhash: a CI entry is keyed on the palette's DRAM address, which cannot see a
     * TLUT rewritten IN PLACE at that address. The RDP re-reads the TLUT for every primitive, so
     * the palette's CONTENT is part of the texture's identity; without it an animated palette
     * serves whatever decode the cache entry was born with. Hashes the fixed 256-byte staging
     * halves, never raw DRAM, so it cannot fault. CI4 folds the whole 256-byte staging half its
     * paletteIndex selects and CI8 folds both, which is wider than either decode strictly reads
     * -- deliberately, because it is exactly what the vendored LUS folds
     * (interpreter.cpp:3056-3074): THIS FIELD, key.palette_content_hash, has to agree bit for
     * bit with LUS's or interp=1 stops being comparable with the stock path on CI. The claim is
     * about the palette fold only -- key.content_hash is the owned twin of LUS's
     * tmem_content_hash and is deliberately NOT bit-identical for the classes [native]
     * texhash_ci covers, which is what that lever is for. */
    if (fmt == IM_FMT_CI && (siz == IM_SIZ_4b || siz == IM_SIZ_8b) && gdx_ninterp_palhash()) {
        uint32_t palHash = 2166136261u;
        const auto foldPalette = [&palHash](const uint8_t* pal, uint32_t n) {
            for (uint32_t b = 0; b < n; b++) {
                palHash = (palHash ^ pal[b]) * 16777619u;
            }
        };
        /* [native] sky_retain: inside a sky run the fold of these exact staged bytes is memoised
         * (gdx_sky_retain.cpp); the two halves are contiguous, so CI8's fold is one 512-byte span. */
        const uint8_t* const palBytes =
            siz == IM_SIZ_4b ? mRdp.palette_staging[(tt.palette / 8) & 1] : mRdp.palette_staging[0];
        const uint32_t palBytesN = siz == IM_SIZ_4b ? 256u : 512u;
        if (mSkyActive && SkyPalHashLookup(palBytes, palBytesN, &palHash)) {
            /* the key's palette fold, as the loop below would have computed it */
        } else {
            if (siz == IM_SIZ_4b) {
                foldPalette(mRdp.palette_staging[(tt.palette / 8) & 1], 256u);
            } else {
                foldPalette(mRdp.palette_staging[0], 256u);
                foldPalette(mRdp.palette_staging[1], 256u);
            }
            if (mSkyActive) {
                SkyPalHashStore(palBytes, palBytesN, palHash);
            }
        }
        key.palette_content_hash = palHash;
    }

    const bool isRgba16 = (fmt == IM_FMT_RGBA && siz == IM_SIZ_16b);
    const bool isFontFmt = (fmt == IM_FMT_I && (siz == IM_SIZ_4b || siz == IM_SIZ_8b)) ||
                           (fmt == IM_FMT_IA && (siz == IM_SIZ_4b || siz == IM_SIZ_8b || siz == IM_SIZ_16b));
    const bool isCi = (fmt == IM_FMT_CI && (siz == IM_SIZ_4b || siz == IM_SIZ_8b));
    const bool isRgba32 = (fmt == IM_FMT_RGBA && siz == IM_SIZ_32b);
    /* CI and RGBA32 decode from the SOURCE bytes, not from the TMEM mirror (DecodeCi4/DecodeCi8/
     * DecodeRgba32 below all read lt.addr), so the TMEM hash above is not their content identity
     * -- the source span is, plus the live palette for CI. Without it their key is pure identity
     * and a recycled address serves the previous occupant's decode: the bridge covers that for
     * the LUS path with MakePersistentRawTextureCopy's per-source memcmp + TextureCacheDelete
     * (n64_gfx_bridge.cpp:2884-2925), which the owned walk never runs. Plan 06 §3.7 asked for
     * this key ("plus the palette bytes for CI") and invariant (11); it was not carried. */
    const bool hashSource = lt.immutable || (mHashCiRgba32 && (isCi || isRgba32));
    bool hudHashCovered = false; /* [native] hud_retain: the key's hash is a function of one load's source */
    uint32_t hudHashOff = 0, hudHashSpan = 0;
    if (!lt.immutable && (isRgba16 || isFontFmt)) {
        const uint32_t tmemByteOffset = static_cast<uint32_t>(tmemIdx) * 8u;
        const uint32_t lineBytes = tt.line_size_bytes;
        if (tmemByteOffset < sizeof(mRdp.tmem) && lineBytes >= 2) {
            const uint32_t remaining = sizeof(mRdp.tmem) - tmemByteOffset;
            uint32_t span = lt.size_bytes > 8u ? lt.size_bytes : 8u;
            if (span > remaining) span = remaining;
            if (isRgba16) {
                uint32_t w = lineBytes / 2;
                uint32_t h = remaining / lineBytes;
                ApplyTileMaskExtent(mRdp, tile, w, h, true);
                const bool boundW = (tt.cms & TX_CLAMP) != 0 || tt.masks == TX_NOMASK;
                const bool boundH = (tt.cmt & TX_CLAMP) != 0 || tt.maskt == TX_NOMASK;
                if (boundW && cacheTileWidth > 0 && cacheTileWidth < w) w = cacheTileWidth;
                if (boundH && cacheTileHeight > 0 && cacheTileHeight < h) h = cacheTileHeight;
                if (w == 0) w = 1;
                if (h == 0) h = 1;
                if (lineBytes * h > remaining) h = remaining / lineBytes;
                if (w > 0 && h > 0) {
                    span = (h - 1) * lineBytes + 2 * w;
                    if (span > remaining) span = remaining;
                }
            }
            uint32_t contentHash = 2166136261u;
            /* [native] hud_batch: TMEM mirrored from a STABLE source by the live load at this
             * base hashes the same every time (gdx_hud_replay.cpp HudTmemHashLookup).
             * [native] hud_direct: TMEM still owed by a direct HUD load is hashed from the
             * per-source memo or settled first (gdx_hud_direct.cpp HudDebtHash). */
            if (mHudDebtN != 0 && HudDebtHash(tmemByteOffset, span, &contentHash)) {
                /* the key's hash, as the loop below would have computed it */
                hudHashCovered = HudDirectLastHashCovered(); /* [native] hud_retain */
                hudHashOff = tmemByteOffset;
                hudHashSpan = span;
            } else if (mSkyActive && SkyTmemHashLookup(tmemIdx, span, &contentHash)) {
                /* [native] sky_retain: the span's source is unchanged since it was hashed */
            } else if (mHud == nullptr || !HudTmemHashLookup(tmemIdx, span, &contentHash)) {
                const uint8_t* tmemBytes = mRdp.tmem + tmemByteOffset;
                uint32_t b = 0;
                for (; b + 4 <= span; b += 4) {
                    uint32_t word;
                    memcpy(&word, tmemBytes + b, 4);
                    contentHash = (contentHash ^ word) * 16777619u;
                }
                for (; b < span; b++) {
                    contentHash = (contentHash ^ tmemBytes[b]) * 16777619u;
                }
                if (mHud != nullptr) {
                    HudTmemHashStore(tmemIdx, span, contentHash);
                }
                if (mSkyActive) {
                    SkyTmemHashStore(tmemIdx, span, contentHash);
                }
            }
            key.content_hash = contentHash;
        }
    } else if (hashSource) {
        /* LUS keyed resource-backed textures on the archive path; the source bytes are that
         * identity here (append-only images, but mode-owned carves reuse addresses). The same
         * span is the content identity of the CI/RGBA32 classes, which decode from it. */
        uint32_t n = lt.size_bytes;
        const size_t readable = DlSrc::ReadableBytes(reinterpret_cast<uintptr_t>(origAddr));
        if (n > readable) n = static_cast<uint32_t>(readable);
        /* [native] hud_batch: an immutable (o2r asset) source's bytes never change, so inside a
         * replayed HUD run its hash is memoised (gdx_hud_replay.cpp). CI folds a live palette
         * after this and is never memoised. */
        const bool hudHash = mHud != nullptr && lt.immutable && !isCi;
        uint32_t h = 0;
        if (!hudHash || !HudHashLookup(origAddr, n, &h)) {
            /* [native] sky_retain: a sky run's source hash is memoised per source (gdx_sky_retain.cpp) */
            if (!(mSkyActive && SkySrcHashLookup(origAddr, n, lt.immutable != 0, &h))) {
                h = FoldBytes(2166136261u, origAddr, n);
                if (mSkyActive) {
                    SkySrcHashStore(origAddr, n, lt.immutable != 0, h);
                }
            }
            if (hudHash) {
                HudHashStore(origAddr, n, h);
            }
        }
        if (isCi && mHashCiRgba32) {
            /* The palette is half the decoded pixel: the same index bytes under a different TLUT
             * are a different texture. Night-course CI4 TLUTs are staged into the GfxPool and
             * bound through the segment-1 alias with slot indices assigned from 0 per course
             * (n64_gfx_bridge.cpp:2769-2779), so palette_dram_addr alone cannot separate them. */
            /* ...but only where palhash has not already put them in this same key. palhash
             * folds the whole staging half into key.palette_content_hash, a superset of every
             * sub-palette inside it, so folding a staged palette again here separates nothing
             * and costs 32-512 byte-wise FNV rounds per CI import -- per draw in a
             * palette-animated scene, with both levers on by default (audit-2026-09-24
             * finding 7). A palette that is NOT staged (DpLoadTlut's tmem<256 path leaves
             * palettes[1] pointing at the raw resolved source) is invisible to palhash, so it
             * is still folded here whatever palhash says. */
            const bool palStagedCovered = gdx_ninterp_palhash() != 0;
            if (siz == IM_SIZ_4b) {
                const uint32_t palSlot = tt.palette / 8u;
                const uint8_t* pal = mRdp.palettes[palSlot];
                if (pal != nullptr) {
                    pal += (tt.palette % 8u) * 32u;
                    if (!(palStagedCovered && PaletteIsStaged(mRdp, pal))) {
                        h = FoldBytes(h, pal, PaletteReadable(mRdp, pal, 32u));
                    }
                }
            } else {
                for (int pi = 0; pi < 2; pi++) {
                    const uint8_t* pal = mRdp.palettes[pi];
                    if (pal != nullptr && !(palStagedCovered && PaletteIsStaged(mRdp, pal))) {
                        h = FoldBytes(h, pal, PaletteReadable(mRdp, pal, 256u));
                    }
                }
            }
        }
        key.content_hash = h != 0 ? h : 1u;
    }

    if (tt.line_size_bytes == 0 || lt.size_bytes == 0) return;

    if (mHudRec) {
        /* [native] hud_retain: the retained run may replay this import only when its key is a
         * function of the run's own loads (gdx_hud_retain.cpp). */
        HudRecNoteImportBegin(tmemIdx, hudHashOff, hudHashSpan, !lt.immutable && (isRgba16 || isFontFmt),
                              hudHashCovered, !isCi && (!isRgba32 || lt.immutable));
    }
    if (TexCacheLookup(unit, key)) {
        if (mHudRec) {
            HudRecNoteImportHit(unit, mRs.textures[unit], true);
        }
        return;
    }
    /* hotcold: the cache-miss path (decode + upload), out of line and cold: a steady race imports
     * by cache hit; misses cluster at scene loads, where the decoders (separate -O2 functions)
     * carry the work, not this dispatch. */
    [this, unit, tile, &lt, origAddr, fmt, siz, tmemIdx, &key]() GDX_COLD {
    if (mHudDebtN != 0) {
        HudDebtPayAll(); /* [native] hud_direct: DecodeRgba16 reads TMEM */
    }
    const uint64_t hitchT0 = gdx3ds_hitch_now(); /* HITCH: decode + upload time of a miss */
    mUploadDone = false; /* DROPSITE 1: did any decoder below actually reach Upload()? */
    mBackend->TexCacheNoteMiss(origAddr, fmt, siz, tmemIdx, key.line_size_bytes, lt.size_bytes, key.tile_width,
                               key.tile_height, key.content_hash);
    switch (fmt) {
        case IM_FMT_RGBA:
            if (siz == IM_SIZ_16b) DecodeRgba16(unit, tile, lt);
            else if (siz == IM_SIZ_32b) DecodeRgba32(tile, lt);
            break;
        case IM_FMT_IA:
            if (siz == IM_SIZ_4b) DecodeIA4(tile, lt);
            else if (siz == IM_SIZ_8b) DecodeIA8(tile, lt);
            else if (siz == IM_SIZ_16b) DecodeIA16(tile, lt);
            break;
        case IM_FMT_CI:
            if (siz == IM_SIZ_4b) DecodeCi4(tile, lt);
            else if (siz == IM_SIZ_8b) DecodeCi8(tile, lt);
            else if (siz == IM_SIZ_16b) DecodeRgba16(unit, tile, lt);
            else if (siz == IM_SIZ_32b) DecodeRgba32(tile, lt);
            break;
        case IM_FMT_I:
            if (siz == IM_SIZ_4b) DecodeI4(tile, lt);
            else if (siz == IM_SIZ_8b) DecodeI8(tile, lt);
            break;
        default:
            break;
    }
    gdx3ds_hitch_add(GDX_HITCH_EV_TEX_DECODE_TICKS, (uint32_t)(gdx3ds_hitch_now() - hitchT0));
    /* DROPSITE 1. Pre-fix this marked the entry uploaded unconditionally. Every decoder above
     * has early-outs -- a null source, a zero extent, a decode above the scratch ceiling, and
     * an fmt/siz pair with no case in the switch at all (G_IM_FMT_RGBA at 4b/8b falls straight
     * through) -- and each of them left the entry marked uploaded over a backend id that had
     * just come off the free list still holding the last texture that used it. Later lookups
     * of the key then HIT and served those pixels for the life of the entry.
     *
     * Now an import that uploaded nothing gets a neutral fallback and stays un-uploaded, so the
     * next lookup misses and retries. [native] texstalefix=0 restores the pre-fix arm. */
    if (mRs.textures[unit] >= 0) {
        if (mUploadDone || !mTexStaleFix) {
            mTex[mRs.textures[unit]].uploaded = true;
        } else {
            mStats.texNoUpload++;
            if (mDropsiteStaleLogged == 0) {
                mDropsiteStaleLogged = 1;
                GDXN_LOG("DROPSITE: texture import fmt=%u siz=%u %ux%u decoded NOTHING -- pre-fix "
                         "this entry was marked uploaded and the cache id kept the previous "
                         "texture's pixels; it now draws untextured and retries next frame",
                         static_cast<unsigned>(fmt), static_cast<unsigned>(siz),
                         static_cast<unsigned>(key.tile_width), static_cast<unsigned>(key.tile_height));
            }
            UploadNeutralFallback(unit);
        }
    }
    }();
}

} // namespace gdxn
