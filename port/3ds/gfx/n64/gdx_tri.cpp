/* port/3ds/gfx/n64/gdx_tri.cpp -- per-triangle path: reject-variant screen, cull, state diff
 * -> flush, combiner key, tile import + extents, sampler, shader, packed-VBO emission with the
 * aux record, texrect tris. Mirrors Interpreter::GfxSpTri1 (interpreter.cpp:3904-5605) on its
 * default 3DS configuration ([trifast] fast emission, [triloop] packed batches, [trectbatch]
 * atlas protocol) and Interpreter::Flush (:663). Plan §3.9/§3.10, invariants (19)-(26). */
#include "gdx_interp.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#include "gdx3ds_verify.h"
#include "gdx_dlsrc_bridge.h"
#include "gdx_log.h"
#include "gdx3ds_cov.h" // COVERAGE: [cov] per-frame feature census ([debug] cov)
#include "gdx3ds_replay.h" // [native] replay (C6): per-draw matrix-state tags
#include "gdx3ds_hotcold.h" // hotcold: GDX_COLD / GDX_UNLIKELY (per-function attributes only)

/* [native] ncensus tick source (gdx_rt_glue.cpp -> svcGetSystemTick; 0 in the host harness). */
extern "C" long long gdx_ninterp_now(void);
/* [trix] the running [prof] child total, so a phase reports SELF time (gdx_rt_glue.cpp). */
extern "C" unsigned long long gdx_ninterp_child_ticks(void);

/* [nverify] source-identity fields for the tri side channel: FNV of the first 64 source bytes,
 * FNV of 64 TMEM bytes at the render tile, bridge address class. Harness-safe (seam + inline). */
static uint32_t VfyFnv64(const void* p, size_t n) { /* same word-then-byte FNV as gfx_citro3d VfyFnv */
    uint32_t h = 2166136261u;
    const uint8_t* b = static_cast<const uint8_t*>(p);
    size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        uint32_t w;
        memcpy(&w, b + i, 4);
        h = (h ^ w) * 16777619u;
    }
    for (; i < n; i++) h = (h ^ b[i]) * 16777619u;
    return h;
}
/* [nverify] whole-load-extent TMEM identity (see gfx_citro3d.cpp): a 64-byte prefix agrees on
 * any texture whose first rows are static and hides the divergence further in. The source hash
 * stays at 64 bytes -- size_bytes is the requested extent, not the readable one. */
static constexpr uint32_t kVfySrcHashCap = 4096u;
/* MUST equal gfx_citro3d.cpp's kVfyTmemHashCap: the LUS twin caps the TMEM hash at 256 bytes
 * and this side capped it at kVfySrcHashCap (4096), so for any load over 256 bytes the two
 * `tm=` fields hashed different lengths and could never agree. The field is receipt-only, but
 * it is the field a texture-identity mismatch is read against -- it said "the two TMEMs
 * differ" on every large texture, including ones that were identical. */
static constexpr uint32_t kVfyTmemHashCap = 4096u;
/* The TMEM bytes a load actually occupies. size_bytes counts SOURCE bytes; LOADTILE writes each
 * row at an 8-byte-aligned destination stride, so for a 74-byte row the footprint is 80 per row
 * and the last rows of the tile sit BEYOND size_bytes -- which is exactly where the decode reads
 * and where a hash over size_bytes could not see a divergence. */
static inline uint32_t VfyTmemFootprint(uint32_t sizeBytes, uint32_t lineBytes) {
    if (lineBytes == 0 || sizeBytes == 0) return sizeBytes;
    const uint32_t stride = (lineBytes + 7u) & ~7u;
    if (stride == lineBytes) return sizeBytes;
    const uint32_t rows = sizeBytes / lineBytes;
    if (rows == 0) return sizeBytes;
    return (rows - 1u) * stride + lineBytes;
}
static inline void VfyBytes8(unsigned* out, const uint8_t* p, uint32_t n) {
    out[0] = out[1] = 0u;
    if (p == nullptr) return;
    const uint32_t take = n < 8u ? n : 8u;
    uint8_t b[8] = {};
    memcpy(b, p, take);
    memcpy(out, b, 8);
}
static void VfyTriSource(unsigned long* addrOut, unsigned* clsOut, unsigned* srcOut, unsigned* tmemOut,
                         unsigned* srcB, unsigned* tmemB,
                         const uint8_t* addr, uint32_t sizeBytes, const uint8_t* tmem, uint32_t tmemBytes) {
    *addrOut = reinterpret_cast<unsigned long>(addr);
    *clsOut = addr ? gdx_gfx_classify_host_ptr(*addrOut) : 0u;
    *srcOut = addr ? VfyFnv64(addr, sizeBytes < 64u ? sizeBytes : 64u) : 0u;
    *tmemOut = tmem ? VfyFnv64(tmem, tmemBytes < kVfyTmemHashCap ? tmemBytes : kVfyTmemHashCap) : 0u;
    if (srcB != nullptr) {
        VfyBytes8(srcB, addr, sizeBytes);
        VfyBytes8(srcB + 2, addr && sizeBytes >= 8u ? addr + (sizeBytes - 8u) : nullptr, 8u);
    }
    if (tmemB != nullptr) {
        VfyBytes8(tmemB, tmem, tmemBytes);
        VfyBytes8(tmemB + 2, tmem && tmemBytes >= 8u ? tmem + (tmemBytes - 8u) : nullptr, 8u);
    }
}

namespace gdxn {

static constexpr float kMinClipW = 1.0e-5f;
static constexpr float kPrimDepthMax = 32767.0f;

static inline float FminExact(float x, float y) {
    if (x != x) return y;
    if (y != y) return x;
    return x < y ? x : y;
}

/* [native] ncensus: batch-size buckets 1, 2, 3-4, 5-8, 9-16, 17-32, 33-64, 65+. */
static inline unsigned BatchBucket(size_t tris) {
    if (tris <= 2) return static_cast<unsigned>(tris) - 1u;
    unsigned b = 2;
    size_t hi = 4;
    while (b + 1u < kBatchHistBuckets && tris > hi) {
        b++;
        hi <<= 1;
    }
    return b;
}

void Interp::Flush(FlushReason why) {
    if (GDX_UNLIKELY(mNrrOpen) && (mBufNumTris > 0 || NrrFlushTouchesBackend(why))) {
        NrrHudHotClose(); /* [native] nrr_hud: the interpreter is about to touch the backend */
    }
    mStats.flush[why]++;
    if (mBufNumTris > 0) {
        mStats.batches++;
        /* Census timing wraps the whole drain (state application + submit): the ms a class is
         * charged is the submit cost of the batches it ended. Two tick reads per drained batch,
         * only with [native] ncensus=1. */
        const uint64_t t0 = mCensus ? static_cast<uint64_t>(gdx_ninterp_now()) : 0;
        mBackend->SetCurrentPrimDepth(static_cast<float>(mRdp.prim_depth) / kPrimDepthMax);
        mBackend->SetCurrentAlphaCompareThreshold(static_cast<float>(mRdp.blend_color.a) / 255.0f);
        if (mPackBatch) {
            if (gdx3ds_replay_interp != nullptr) {
                gdx3ds_replay_interp->setDrawTag(mReplayBatchTag);
            }
            if (mHsxLean) {
                mBackend->DrawPackedLean(mBufNumTris, &mPackAux); /* [native] hudsky_emit: HUD / sky runs */
            } else {
                mBackend->DrawPackedBatch(mBufNumTris, &mPackAux);
            }
        }
        if (GDX_UNLIKELY(mCensus)) {
            mStats.flushDrain[why]++;
            mStats.flushTicks[why] += static_cast<uint64_t>(gdx_ninterp_now()) - t0;
            mStats.batchHist[BatchBucket(mBufNumTris)]++;
            mStats.batchTris += static_cast<uint32_t>(mBufNumTris);
        }
        mPackBatch = false;
        mPackDst = nullptr;
        mPackCapTris = static_cast<size_t>(kMaxTriBuffer); /* GEOMLOSS: re-asked at the next begin */
        mBufNumTris = 0;
        mPackComb = nullptr;
        mPackFolded = false;
        mTriMemoValid = false; /* the memo described the batch just drawn */
    }
}

/* [native] replay (C6): drain the open batch when the next triangle's matrix-state tag differs,
 * so each draw the backend sees carries one pose. Only with the lever on (the caller tests the
 * hook table): the split adds draws, never changes a pixel. */
void Interp::ReplayBatchTag(uint16_t tag) {
    if (mBufNumTris > 0 && tag != mReplayBatchTag) {
        Flush(FLUSH_POSE);
    }
    mReplayBatchTag = tag;
}

/* A triangle's tag: its three vertices' (a triangle mixing two matrix states cannot be patched
 * with one pose: UNKNOWN); the clipper's fan inherits the clipped triangle's; a rectangle is
 * screen space, except inside the sky where it carries the camera's state for the scroll patch. */
uint16_t Interp::ReplayTriTag(uint8_t a, uint8_t b, uint8_t c, bool isRect) const {
    if (isRect) {
        if (gdx3ds_replay_interp->inSky()) {
            return static_cast<uint16_t>(gdx3ds_replay_interp->curTag(ReplayRectXscale()) | GDX_REPLAY_TAG_SKYBIT);
        }
        return GDX_REPLAY_TAG_SCREEN;
    }
    if (mClipFanDepth > 0) {
        return mReplayClipTag;
    }
    const uint16_t ta = gdx3ds_replay_vtx_tag[a];
    if (gdx3ds_replay_vtx_tag[b] != ta || gdx3ds_replay_vtx_tag[c] != ta) {
        return GDX_REPLAY_TAG_UNKNOWN;
    }
    return ta;
}

/* == LUS GdxHudRegFoldable ([native] fx_hud), over gdxn::Backend::BatchConstInputs. */
static inline bool MuxIsReg(uint8_t mux, bool isPrim, bool alphaChannel) {
    if (isPrim) {
        if (alphaChannel) {
            return mux == ACMUX_PRIMITIVE || mux == ACMUX_PRIM_LOD_FRAC;
        }
        return mux == CCMUX_PRIMITIVE || mux == CCMUX_PRIMITIVE_ALPHA || mux == CCMUX_PRIM_LOD_FRAC;
    }
    if (alphaChannel) {
        return mux == ACMUX_ENVIRONMENT;
    }
    return mux == CCMUX_ENVIRONMENT || mux == CCMUX_ENV_ALPHA;
}

bool Interp::RegFoldable(bool isPrim) const {
    if (!mPackBatch || mPackComb == nullptr) {
        return false;
    }
    uint32_t rgbMask = 0, alphaMask = 0;
    if (!mBackend->BatchConstInputs(&rgbMask, &alphaMask)) {
        return false;
    }
    for (int j = 0; j < 7; j++) {
        if (((rgbMask >> j) & 1u) && MuxIsReg(mPackComb->shader_input_mapping[0][j], isPrim, false)) {
            return false;
        }
        if (((alphaMask >> j) & 1u) && MuxIsReg(mPackComb->shader_input_mapping[1][j], isPrim, true)) {
            return false;
        }
    }
    /* The alpha channel's per-vertex slot only really rides per vertex when the batch wants a
     * vertex alpha at all and the fog factor has not claimed out[9]. */
    const int ai = mPackParams.vtxAlphaInput - 1;
    if (ai >= 0 && MuxIsReg(mPackComb->shader_input_mapping[1][ai], isPrim, true) &&
        (!mPackParams.wantVtxAlpha || mPackParams.fogFactorToAlpha)) {
        return false;
    }
    return true;
}

/* A prim/env VALUE change on an open batch: census it, then either absorb it ([native]
 * ownfold) or split as before. Behaviour with ownfold=0 is byte-for-byte the old path. */
void Interp::NoteRegChange(bool isPrim, FlushReason why) {
    if (mBufNumTris == 0) {
        mStats.flush[why]++; /* a no-op flush; keep the call count comparable */
        return;
    }
    const int reg = isPrim ? 0 : 1;
    const bool foldable = (mCensus || mOwnFold) ? RegFoldable(isPrim) : false;
    if (mCensus && foldable) {
        mStats.foldable[reg]++;
    }
    if (mOwnFold && foldable) {
        mStats.folded[reg]++;
        mPackFolded = true;
        return;
    }
    Flush(why);
}

void* Interp::LookupOrCreateShader(uint64_t id0, uint64_t id1) {
    void* prg = mBackend->LookupShader(id0, id1);
    if (prg == nullptr) {
        mBackend->UnloadShader(mRs.shader_program);
        prg = mBackend->CreateAndLoadNewShader(id0, id1);
        mRs.shader_program = prg;
    }
    return prg;
}

static inline LoadedVertex Interpolate(const LoadedVertex& a, const LoadedVertex& b, float t) {
    LoadedVertex r;
    memset(&r, 0, sizeof(r));
    r.x = a.x + (b.x - a.x) * t;
    r.y = a.y + (b.y - a.y) * t;
    r.z = a.z + (b.z - a.z) * t;
    r.w = a.w + (b.w - a.w) * t;
    r.u = a.u + (b.u - a.u) * t;
    r.v = a.v + (b.v - a.v) * t;
    const auto cl = [](float v) { return static_cast<uint8_t>(v < 0.0f ? 0.0f : (v > 255.0f ? 255.0f : v)); };
    r.color.r = cl(a.color.r + (b.color.r - a.color.r) * t);
    r.color.g = cl(a.color.g + (b.color.g - a.color.g) * t);
    r.color.b = cl(a.color.b + (b.color.b - a.color.b) * t);
    r.color.a = cl(a.color.a + (b.color.a - a.color.a) * t);
    return r;
}

static inline void UpdateClipFlags(LoadedVertex& v) {
    uint8_t m = 0;
    if (v.w - kMinClipW < 0.0f) m |= 1u << 0;
    if (v.x + v.w < 0.0f) m |= 1u << 1;
    if (v.w - v.x < 0.0f) m |= 1u << 2;
    if (v.y + v.w < 0.0f) m |= 1u << 3;
    if (v.w - v.y < 0.0f) m |= 1u << 4;
    if (v.z + v.w < 0.0f) m |= 1u << 5;
    if (v.w - v.z < 0.0f) m |= 1u << 6;
    v.clip_out = m;
    v.clip_rej = 0;
    if (v.x < -v.w) v.clip_rej |= 1;
    if (v.x > v.w) v.clip_rej |= 2;
    if (v.y < -v.w) v.clip_rej |= 4;
    if (v.y > v.w) v.clip_rej |= 8;
    if (v.z < -v.w) v.clip_rej |= 16;
    if (v.z > v.w) v.clip_rej |= 32;
}

static inline float ClipDistance(const LoadedVertex& v, uint32_t plane) {
    switch (plane) {
        case 0: return v.w - kMinClipW;
        case 1: return v.x + v.w;
        case 2: return v.w - v.x;
        case 3: return v.y + v.w;
        case 4: return v.w - v.y;
        case 5: return v.z + v.w;
        default: return v.w - v.z;
    }
}

/* [native] o3dsfast: one Sutherland-Hodgman plane, the SAME arithmetic as the historic loop in
 * SpTri with two instruction-level changes and no numeric one:
 *   - `(i + 1) % inputCount` becomes a compare: ARMv6 has no divide instruction, so the modulo
 *     was a __aeabi_uidivmod call per vertex per plane (up to ~70 per clipped triangle).
 *   - each vertex's plane distance is computed once and carried as the next iteration's
 *     `currentDistance`. It is the same expression on the same, unmodified input slot (the
 *     loop writes only `output`, a different buffer), so the value is bit-identical. */
static void ClipPlaneFast(const LoadedVertex* input, size_t inputCount, LoadedVertex* output, size_t clipCap,
                          uint32_t plane, size_t* outCount, bool* outTruncated) {
    size_t outputCount = 0;
    bool truncated = false;
    float currentDistance = ClipDistance(input[0], plane);
    for (size_t i = 0; i < inputCount; ++i) {
        const LoadedVertex& current = input[i];
        const size_t ni = (i + 1u == inputCount) ? 0u : i + 1u;
        const LoadedVertex& next = input[ni];
        const float nextDistance = ClipDistance(next, plane);
        const bool currentInside = currentDistance >= 0.0f;
        const bool nextInside = nextDistance >= 0.0f;
        if (currentInside) {
            if (outputCount < clipCap) {
                output[outputCount++] = current;
            } else {
                truncated = true;
            }
        }
        if (currentInside != nextInside && outputCount >= clipCap) {
            truncated = true;
        }
        if (currentInside != nextInside && outputCount < clipCap) {
            const float denominator = currentDistance - nextDistance;
            float t = denominator != 0.0f ? currentDistance / denominator : 0.0f;
            if (t < 0.0f) t = 0.0f;
            if (t > 1.0f) t = 1.0f;
            output[outputCount++] = Interpolate(current, next, t);
        }
        currentDistance = nextDistance;
    }
    *outCount = outputCount;
    *outTruncated = truncated;
}

/* [native] o3dsfast: the exact PREFIX of the 7-plane loop for an unclipped triangle. On a plane
 * all three vertices are inside, the historic loop copies them through verbatim, in order (no
 * interpolation, no truncation: 3 <= every legal cap); on a plane all three are outside it
 * emits nothing and the loop ends on `inputCount >= 3`. So those planes can be decided on the
 * ORIGINAL vertices with the same ClipDistance expressions, and only the first STRADDLED plane
 * needs the real clipper -- which then starts from exactly the input it would have had. A NaN
 * distance is "outside" here exactly as `>= 0.0f` makes it outside there. The Old 3DS race
 * sends ~250 clipped triangles a task through the clipper and ~90% come out all-out, i.e. most
 * of them are decided on their first all-outside plane without a single vertex copy.
 * Returns the plane to resume from; *count = 0 when a plane emptied the triangle; *passed says
 * whether any plane passed it through (the historic loop's clip-polygon high-water saw a 3). */
static uint32_t ClipPrepass(const LoadedVertex& a, const LoadedVertex& b, const LoadedVertex& c, size_t* count,
                            bool* passed) {
    *passed = false;
    for (uint32_t plane = 0; plane < 7; ++plane) {
        const bool ia = ClipDistance(a, plane) >= 0.0f;
        const bool ib = ClipDistance(b, plane) >= 0.0f;
        const bool ic = ClipDistance(c, plane) >= 0.0f;
        if (ia && ib && ic) {
            *passed = true;
            continue;
        }
        if (!ia && !ib && !ic) {
            *count = 0;
            return plane + 1u;
        }
        return plane;
    }
    return 7u;
}

/* [trix] sampled phase clock (TriPhase in gdx_interp.h). Enter() closes the running phase --
 * wall minus the [prof] child ticks that moved inside it -- and opens the next; the destructor
 * closes whatever is open, so every early return in SpTri is attributed to its phase. */
class TriClock {
  public:
    TriClock(Counters& s, bool on) : mS(s), mOn(on) {
        if (mOn) {
            mS.triPhSamples++;
            mT = static_cast<uint64_t>(gdx_ninterp_now());
            mC = gdx_ninterp_child_ticks();
        }
    }
    ~TriClock() {
        Enter(TRI_PH_COUNT);
    }
    void Enter(int next) {
        if (!mOn) {
            return;
        }
        const uint64_t t = static_cast<uint64_t>(gdx_ninterp_now());
        const uint64_t c = gdx_ninterp_child_ticks();
        const uint64_t gross = t - mT;
        const uint64_t child = c - mC;
        mS.triPhTicks[mPh] += (child < gross) ? (gross - child) : 0;
        mS.triPhN[mPh]++;
        mT = t;
        mC = c;
        mPh = next;
        if (next == TRI_PH_COUNT) {
            mOn = false;
        }
    }

  private:
    Counters& mS;
    bool mOn;
    int mPh = TRI_PH_SCREEN;
    uint64_t mT = 0, mC = 0;
};

/* [native] trimemo -- is every input ResolveDrawState reads identical to the last FULL
 * resolve's? The inputs, read off the function itself (keep this list in step with it):
 *   registers    geometry_mode, other_mode_l/h, combine_mode, grayscale, first_tile_index
 *                (the combiner key, depth/decal/blend/filter bits, the tile selection)
 *   flags        viewport_or_scissor_changed and textures_changed[used unit] -- a hit requires
 *                both FALSE, so the import / viewport arms that would run are never skipped
 *   tiles        the texel0/texel1 Tile of every USED unit, whole (extents, masks, clamp)
 *   span store   LoadedTexAt(tile.tmem_index) -> mSpanEpoch, bumped by every store mutation
 *   backend      mRs (what the backend was left holding: depth, decal, viewport, scissor,
 *                shader, bound slots, samplers), whole
 *   is_rect      (only reachable through textures_changed, kept anyway)
 * With all of those equal the full path takes no Flush and calls no setter (every diff it
 * makes compares equal), the pool lookup returns the same combiner (the pool never evicts), and
 * comb->prg[tm], ShaderGetInfo and GetClipParameters are pure -- so it would produce exactly the
 * DrawState banked here. The compare is a snapshot, not a dirty flag, because the native replay
 * hooks write these registers through GdxSeamRegs pointers without passing a handler. */
bool Interp::TriMemoHit(bool isRect) const {
    const TriMemoKey& k = mTmKey;
    if (!k.valid || mVerifyActive || k.isRect != isRect || mRdp.viewport_or_scissor_changed) {
        return false;
    }
    if (k.otherModeL != mRdp.other_mode_l || k.otherModeH != mRdp.other_mode_h ||
        k.geometryMode != mRsp.geometry_mode || k.combineMode != mRdp.combine_mode ||
        k.firstTile != mRdp.first_tile_index || k.grayscale != mRdp.grayscale || k.spanEpoch != mSpanEpoch) {
        return false;
    }
    const Combiner* comb = mTmState.comb;
    for (int i = 0; i < 2; i++) {
        if (!comb->usedTextures[i]) {
            continue;
        }
        if (mRdp.textures_changed[i]) {
            return false;
        }
        const uint32_t tile = (i == 1) ? ((k.firstTile + 1u) & 7u) : k.firstTile;
        if (memcmp(&k.tiles[i], &mRdp.texture_tile[tile], sizeof(Tile)) != 0) {
            return false;
        }
    }
    return memcmp(&k.rs, &mRs, sizeof(RenderState)) == 0;
}

void Interp::TriMemoBank(bool isRect, const DrawState& ds) {
    TriMemoKey& k = mTmKey;
    k.valid = false;
    if (mTriMemoOn == 0 || mVerifyActive || ds.comb == nullptr || mRdp.viewport_or_scissor_changed) {
        return;
    }
    for (int i = 0; i < 2; i++) {
        if (ds.comb->usedTextures[i] && mRdp.textures_changed[i]) {
            return; /* a used unit still owes an import: never bank a half-applied state */
        }
    }
    k.isRect = isRect;
    k.firstTile = mRdp.first_tile_index;
    k.grayscale = mRdp.grayscale;
    k.otherModeL = mRdp.other_mode_l;
    k.otherModeH = mRdp.other_mode_h;
    k.geometryMode = mRsp.geometry_mode;
    k.combineMode = mRdp.combine_mode;
    k.spanEpoch = mSpanEpoch;
    memcpy(&k.tiles[0], &mRdp.texture_tile[k.firstTile & 7u], sizeof(Tile));
    memcpy(&k.tiles[1], &mRdp.texture_tile[(k.firstTile + 1u) & 7u], sizeof(Tile));
    memcpy(&k.rs, &mRs, sizeof(RenderState));
    mTmState = ds;
    k.valid = true;
}

/* SpTri's state block: depth/decal/viewport/scissor diff, combiner key, texture import +
 * extents + sampler, shader program, blend. Binds the backend and banks the tri memo; no
 * emission. Mirrors GfxSpTri1 (interpreter.cpp) expression for expression; the machines
 * binder (seam BindMaterial) runs it against the run's registers. */
bool Interp::ResolveDrawState(bool is_rect, DrawState& out) {
    if (GDX_UNLIKELY(mNrrOpen)) {
        NrrHudHotClose(); /* [native] nrr_hud: the state block binds the backend */
    }
    if (mTriMemoOn != 0 && TriMemoHit(is_rect)) {
        mStats.triResolveMemo++;
        out = mTmState;
        mTriMemo = out;
        mTriMemoValid = true;
        return true;
    }
    mStats.triResolveFull++;
    const bool zbuffer_enabled = (mRsp.geometry_mode & GM_ZBUFFER) == GM_ZBUFFER;
    const bool prim_depth_enabled = (mRdp.other_mode_l & ZS_PRIM) != 0;
    const bool depth_test = (zbuffer_enabled || prim_depth_enabled) && (mRdp.other_mode_l & RM_Z_CMP) == RM_Z_CMP;
    const bool depth_mask = (mRdp.other_mode_l & RM_Z_UPD) == RM_Z_UPD;
    const uint8_t depth_test_and_mask = (depth_test ? 1 : 0) | (depth_mask ? 2 : 0);
    if (depth_test_and_mask != mRs.depth_test_and_mask) {
        Flush(FLUSH_DEPTH);
        mBackend->SetDepthTestAndMask(depth_test, depth_mask);
        mRs.depth_test_and_mask = depth_test_and_mask;
    }
    const bool zmode_decal = (mRdp.other_mode_l & RM_ZMODE_DEC) == RM_ZMODE_DEC;
    if ((zmode_decal ? 1u : 0u) != mRs.decal_mode) {
        Flush(FLUSH_DECAL);
        mBackend->SetZmodeDecal(zmode_decal);
        mRs.decal_mode = zmode_decal ? 1 : 0;
    }
    if (mRdp.viewport_or_scissor_changed) {
        if (memcmp(&mRdp.viewport, &mRs.viewport, sizeof(Rect)) != 0) {
            Flush(FLUSH_VIEWPORT);
            mBackend->SetViewport(mRdp.viewport.x, mRdp.viewport.y, mRdp.viewport.width, mRdp.viewport.height);
            mRs.viewport = mRdp.viewport;
        }
        if (memcmp(&mRdp.scissor, &mRs.scissor, sizeof(Rect)) != 0) {
            Flush(FLUSH_SCISSOR);
            mBackend->SetScissor(mRdp.scissor.x, mRdp.scissor.y, mRdp.scissor.width, mRdp.scissor.height);
            mRs.scissor = mRdp.scissor;
        }
        mRdp.viewport_or_scissor_changed = false;
    }

    const uint64_t cc_id = mRdp.combine_mode;
    uint64_t cc_options = 0;
    bool use_alpha = ((mRdp.other_mode_l & (3 << 20)) == (BL_CLR_MEM << 20) &&
                      (mRdp.other_mode_l & (3 << 16)) == (BL_1MA << 16)) ||
                     ((mRdp.other_mode_l & (3 << 22)) == (BL_CLR_MEM << 22) &&
                      (mRdp.other_mode_l & (3 << 18)) == (BL_1MA << 18));
    const uint8_t blend_src = static_cast<uint8_t>(mRdp.other_mode_l >> 30);
    const bool use_blend_color = blend_src == BL_CLR_BL;
    const bool use_fog = blend_src == BL_CLR_FOG || use_blend_color;
    bool texture_edge = (mRdp.other_mode_l & RM_CVG_X_ALPHA) == RM_CVG_X_ALPHA;
    const bool use_noise = (mRdp.other_mode_l & (3u << MDSFT_ALPHACOMPARE)) == (AC_DITHER << MDSFT_ALPHACOMPARE);
    const bool use_2cyc = (mRdp.other_mode_h & (3u << MDSFT_CYCLETYPE)) == CYC_2CYCLE;
    bool alpha_threshold = (mRdp.other_mode_l & (3u << MDSFT_ALPHACOMPARE)) == (AC_THRESHOLD << MDSFT_ALPHACOMPARE);
    const bool invisible = (mRdp.other_mode_l & (3 << 24)) == (BL_0 << 24) &&
                           (mRdp.other_mode_l & (3 << 20)) == (BL_CLR_MEM << 20);
    const bool use_grayscale = mRdp.grayscale;
    const bool use_prim_depth = (mRdp.other_mode_l & ZS_PRIM) != 0;

    if (texture_edge) {
        if (use_alpha) {
            alpha_threshold = true;
            texture_edge = false;
        }
        use_alpha = true;
    }
    if (alpha_threshold || use_noise) use_alpha = true;
    if (use_alpha) cc_options |= SHOPT_ALPHA;
    if (use_fog) cc_options |= SHOPT_FOG;
    if (texture_edge) cc_options |= SHOPT_TEXTURE_EDGE;
    if (use_noise) cc_options |= SHOPT_NOISE;
    if (use_2cyc) cc_options |= SHOPT_2CYC;
    if (alpha_threshold) cc_options |= SHOPT_ALPHA_THRESHOLD;
    if (invisible) cc_options |= SHOPT_INVISIBLE;
    if (use_grayscale) cc_options |= SHOPT_GRAYSCALE;
    if (use_prim_depth) cc_options |= SHOPT_PRIM_DEPTH;
    cc_options |= static_cast<uint64_t>(static_cast<int64_t>(-1) << SHADER_ID_SHIFT); /* empty shader stack */

    const uint32_t texel0Tile = mRdp.first_tile_index;
    const uint32_t texel1Tile = (mRdp.first_tile_index + 1u) & 7u;
    /* masked/blended are never set on this port (no replacement packs). */

    CombinerKey key;
    key.combine_mode = cc_id;
    key.options = cc_options;
    bool created = false;
    Combiner* comb = mCcPool.Lookup(key, &created);
    if (comb == nullptr) {
        return false;
    }
    if (created) {
        Flush(FLUSH_COMBINER);
    }

    uint32_t tm = 0;
    uint32_t tex_width[2] = { 0, 0 }, tex_height[2] = { 0, 0 }, tex_width2[2] = { 0, 0 }, tex_height2[2] = { 0, 0 };
    uint32_t effective_tile[2] = { 0, 0 };
    for (int i = 0; i < 2; i++) {
        const uint32_t tile = (i == 1) ? texel1Tile : texel0Tile;
        effective_tile[i] = tile;
        if (!comb->usedTextures[i]) continue;
        if (GDX_UNLIKELY(mNrrTmemLive)) {
            NrrTmemRead(static_cast<int>(tile)); /* [native] nrr_hud: LoadedTexAt below reads the span store */
        }
        const Tile& tt = mRdp.texture_tile[tile];
        if (mRdp.textures_changed[i]) {
            /* [trectbatch] atlas protocol, verbatim order (plan §3.10). */
            const int16_t prevNode = mRs.textures[i];
            const bool trectRect = mTrectBatchOn && is_rect;
            const bool trectDefer = trectRect && mBufNumTris > 0 && mPackBatch && !mPackFolded && prevNode >= 0 &&
                                    mTexCount + 1 < kTexCacheSize;
            const uint32_t prevId = trectDefer ? mTex[prevNode].texture_id : 0u;
            const int prevPage = trectDefer ? mBackend->TexPage(prevId) : -1;
            if (!trectDefer) {
                Flush(FLUSH_TEXTURE);
            }
            const bool trectArm = trectRect && i == 0 && mTrectRectAtlasOk;
            if (trectArm) mBackend->AtlasArm(true);
            ImportTexture(i, static_cast<int>(tile));
            if (trectArm) mBackend->AtlasArm(false);
            if (trectDefer) {
                const int16_t newNode = mRs.textures[i];
                const uint32_t newId = newNode >= 0 ? mTex[newNode].texture_id : 0u;
                const int newPage = newNode >= 0 ? mBackend->TexPage(newId) : -1;
                if (newNode == prevNode || (prevPage >= 0 && newPage == prevPage)) {
                    if (newNode != prevNode) {
                        mBackend->SetSamplerParameters(i, mRs.sampler_linear[i], mRs.sampler_cms[i], mRs.sampler_cmt[i]);
                        mBackend->VboPackRefresh(&mPackParams);
                    }
                } else {
                    mBackend->SelectTexture(i, prevId);
                    Flush(FLUSH_TEXTURE);
                    if (newNode >= 0) mBackend->SelectTexture(i, newId);
                }
            }
            mRdp.textures_changed[i] = false;
        }

        uint8_t cms = tt.cms;
        uint8_t cmt = tt.cmt;
        const LoadedTexture lt = LoadedTexAt(tt.tmem_index);
        const uint32_t loaded_line_size = lt.line_size_bytes;
        const uint32_t loaded_size = lt.size_bytes;
        const uint32_t loaded_full_line = lt.full_image_line_size_bytes;
        uint32_t tex_size_bytes;
        uint32_t line_size;
        if ((loaded_line_size != loaded_size || loaded_full_line != loaded_size) && loaded_line_size > 0) {
            line_size = loaded_line_size;
            tex_size_bytes = loaded_size;
        } else {
            line_size = tt.line_size_bytes;
            tex_size_bytes = lt.orig_size_bytes;
            if (tt.siz == IM_SIZ_32b) line_size *= 2;
        }
        if (line_size == 0) line_size = 1;
        tex_height[i] = tex_size_bytes / line_size;
        switch (tt.siz) {
            case IM_SIZ_4b: line_size <<= 1; break;
            case IM_SIZ_8b: break;
            case IM_SIZ_16b: line_size /= 2; break;
            case IM_SIZ_32b: line_size /= 4; break;
        }
        tex_width[i] = line_size;
        tex_width2[i] = static_cast<uint32_t>(static_cast<int32_t>((tt.lrs - tt.uls + 4) / 4));
        tex_height2[i] = static_cast<uint32_t>(static_cast<int32_t>((tt.lrt - tt.ult + 4) / 4));
        const uint32_t loadedPx = tex_width[i] * tex_height[i];
        const uint32_t renderedPx = tex_width2[i] * tex_height2[i];
        const bool pyrLike = renderedPx > 0 && loadedPx > renderedPx && loadedPx * 8 < renderedPx * 13;
        if ((pyrLike || (cms & TX_CLAMP)) && tex_width2[i] > 0 && tex_width2[i] < tex_width[i]) tex_width[i] = tex_width2[i];
        if ((pyrLike || (cmt & TX_CLAMP)) && tex_height2[i] > 0 && tex_height2[i] < tex_height[i]) tex_height[i] = tex_height2[i];
        /* ApplyTileMaskExtent(maskAuthoritative=true) */
        if (tt.masks != TX_NOMASK && tt.masks < 31) {
            if ((tt.cms & TX_CLAMP) == 0) tex_width[i] = 1u << tt.masks;
            else if (tex_width[i] > (1u << tt.masks)) tex_width[i] = 1u << tt.masks;
        }
        if (tt.maskt != TX_NOMASK && tt.maskt < 31) {
            if ((tt.cmt & TX_CLAMP) == 0) tex_height[i] = 1u << tt.maskt;
            else if (tex_height[i] > (1u << tt.maskt)) tex_height[i] = 1u << tt.maskt;
        }
        if (tex_width[i] == 0) tex_width[i] = (tt.masks != TX_NOMASK && tt.masks < 31) ? (1u << tt.masks) : 1u;
        if (tex_height[i] == 0) tex_height[i] = (tt.maskt != TX_NOMASK && tt.maskt < 31) ? (1u << tt.maskt) : 1u;

        const uint32_t tex_width1 = tex_width[i] << (cms & TX_MIRROR);
        const uint32_t tex_height1 = tex_height[i] << (cmt & TX_MIRROR);
        if ((cms & TX_CLAMP) && ((cms & TX_MIRROR) || tex_width1 != tex_width2[i])) {
            tm |= 1u << (2 * i);
            cms &= static_cast<uint8_t>(~TX_CLAMP);
        }
        if ((cmt & TX_CLAMP) && ((cmt & TX_MIRROR) || tex_height1 != tex_height2[i])) {
            tm |= 1u << (2 * i + 1);
            cmt &= static_cast<uint8_t>(~TX_CLAMP);
        }
        if (mRs.textures[i] < 0) continue;
        const bool linear_filter = (mRdp.other_mode_h & (3u << MDSFT_TEXTFILT)) == TF_BILERP;
        if (!mRs.sampler_valid[i] || linear_filter != mRs.sampler_linear[i] || cms != mRs.sampler_cms[i] ||
            cmt != mRs.sampler_cmt[i]) {
            Flush(FLUSH_SAMPLER);
            mBackend->SetSamplerParameters(i, linear_filter, cms, cmt);
            mRs.sampler_valid[i] = true;
            mRs.sampler_linear[i] = linear_filter;
            mRs.sampler_cms[i] = cms;
            mRs.sampler_cmt[i] = cmt;
        }
    }

    void* prg = comb->prg[tm];
    if (prg == nullptr) {
        comb->prg[tm] = prg = LookupOrCreateShader(comb->shader_id0, comb->shader_id1 | tm * SHOPT_TEXEL0_CLAMP_S);
    }
    if (prg != mRs.shader_program) {
        Flush(FLUSH_SHADER);
        mBackend->UnloadShader(mRs.shader_program);
        mBackend->LoadShader(prg);
        mRs.shader_program = prg;
    }
    if ((use_alpha ? 1u : 0u) != mRs.alpha_blend) {
        Flush(FLUSH_ALPHA);
        mBackend->SetUseAlpha(use_alpha);
        mRs.alpha_blend = use_alpha ? 1 : 0;
    }
    uint8_t numInputs;
    bool usedTextures[2];
    mBackend->ShaderGetInfo(prg, &numInputs, usedTextures);
    const ClipParams clip_parameters = mBackend->GetClipParameters();

    out.comb = comb;
    out.prg = prg;
    out.tm = tm;
    for (int i = 0; i < 2; i++) {
        out.tex_width[i] = tex_width[i];
        out.tex_height[i] = tex_height[i];
        out.tex_width2[i] = tex_width2[i];
        out.tex_height2[i] = tex_height2[i];
        out.effective_tile[i] = effective_tile[i];
        out.usedTextures[i] = usedTextures[i];
    }
    out.numInputs = numInputs;
    out.clip = clip_parameters;
    out.use_alpha = use_alpha;
    out.use_fog = use_fog;
    out.use_blend_color = use_blend_color;
    mTriMemo = out;
    mTriMemoValid = true;
    if (mTriMemoOn != 0) {
        TriMemoBank(is_rect, out);
    }
    return true;
}

extern "C" int gdx_ninterp_cpuclip(void);

void Interp::SpTri(uint8_t vtx1_idx, uint8_t vtx2_idx, uint8_t vtx3_idx, bool is_rect) {
    ProfScope prof(mBackend, 3);
    /* [trix] exit-path counters (always) and the sampled phase clock (gputrace, 1 in 32
       top-level calls; the clipper's fan calls are inside their parent's CLIP phase). */
    mStats.triCalls++;
    const bool topLevel = mClipFanDepth == 0;
    if (topLevel) {
        mStats.triTopCalls++;
    }
    if (is_rect) {
        mStats.triRectCalls++;
    }
    TriClock clk(mStats, mDxOn && topLevel && ((++mTriSampleSeq & 31u) == 0u));
    LoadedVertex* v1 = &mRsp.loaded_vertices[vtx1_idx];
    LoadedVertex* v2 = &mRsp.loaded_vertices[vtx2_idx];
    LoadedVertex* v3 = &mRsp.loaded_vertices[vtx3_idx];
    LoadedVertex* v_arr[3] = { v1, v2, v3 };
    if (!is_rect) mStats.tris++;
    const uint16_t replayTag =
        gdx3ds_replay_interp != nullptr ? ReplayTriTag(vtx1_idx, vtx2_idx, vtx3_idx, is_rect) : 0;

    /* Reject variants drop any triangle with a vertex outside the 2x box or behind the eye. */
    if (!is_rect && mClipFanDepth == 0 && (mVariant == 1 || mVariant == 2)) {
        for (const LoadedVertex* vertex : v_arr) {
            const float w2 = 2.0f * vertex->w;
            if (vertex->w <= 0.0f || vertex->x < -w2 || vertex->x > w2 || vertex->y < -w2 || vertex->y > w2 ||
                vertex->z < -vertex->w) {
                mStats.trisRejected++;
                mStats.triVariantRej++;
                /* COVERAGE: the reject-variant screen fired on this frame. */
                gdx3ds_cov_mark(GDX3DS_COV_KEY(GDX3DS_COV_FAM_CLIP, GDX3DS_COV_CLIP_REJECT));
                return;
            }
        }
    }

    /* CPU clipping: LUS default; the plan's no-clip mode is [native] cpuclip=0 (default 1 until
     * the M2 tunnel A/B proves the PICA path). */
    bool requiresClipping = false;
    if (!is_rect && mClipFanDepth == 0 && gdx_ninterp_cpuclip() && !mSkyNoClip) { /* sky_retain=3: PICA clips the sky */
        requiresClipping = ((v1->clip_out | v2->clip_out | v3->clip_out) != 0);
    }
    if (GDX_UNLIKELY(requiresClipping)) {
        mStats.triClipIn++;
        clk.Enter(TRI_PH_CLIP);
        /* hotcold: the clipper + fan runs out of line (noinline, not cold: it is a real per-frame
         * path), so SpTri's unclipped straight-line code stays compact. Every exit below returned
         * from SpTri before; each now returns from the lambda, and SpTri returns right after. */
        [this, v1, v2, v3, replayTag]() __attribute__((noinline)) {
        /* COVERAGE: this frame ran the CPU clipper. `tunnel-silence-records` exists because
         * the clipper is where open failure #1 lives, and no SHOT schedule ever knew which
         * frames reach it. */
        gdx3ds_cov_mark(GDX3DS_COV_KEY(GDX3DS_COV_FAM_CLIP, GDX3DS_COV_CLIP_NEEDED));
        /* DROPSITE 3. Sutherland-Hodgman against 7 half-spaces, expression-for-expression the
         * LUS clipper (interpreter.cpp GfxSpTri1), 16-vertex cap included. Clipping a CONVEX
         * polygon by a half-space adds at most one vertex (two crossings cost at least one
         * vertex, and the result of each step is convex again), so from a triangle the output
         * is bounded by 3 + 7 = 10 and the cap has six slots of slack it can never use. The
         * cap is therefore counted, not raised: raising it would diverge from the reference
         * path this code exists to match, and clipPolyHw on the receipt is what proves the
         * bound empirically instead of by argument. */
        /* NEITHER buffer is zero-filled, and that is deliberate: both zero-fills were dead
         * stores, 1024 B of memset per clipped triangle that the shipped build really ran
         * (objdump: two `bl <memset>` with r2=512, both gone). The clipper only ever reads
         * the prefix it has written on this call -- `outputCount` is incremented on exactly
         * the two arms that write the slot they consume, the truncation arms increment
         * nothing, and the ping-pong makes the next plane's `input` precisely that prefix --
         * so `input[0 .. inputCount-1]` is fully assigned at every read, including the fan
         * emit below. `= { *v1, *v2, *v3 }` would zero all 512 bytes of polygonA before
         * writing [0..2] over the top, hence the explicit assignment instead.
         * Full derivation, and the four differences from the gdx3ds_vtx_math.h sibling that
         * had to be re-checked rather than inherited (the mClipPolyMax lever, the recursive
         * SpTri between the fan's reads, LoadedVertex's unnamed tail padding, and the absence
         * of a caller): docs/research/native/tooling3-progress.md §3. NEXT.md B26's sibling. */
        LoadedVertex polygonA[16];
        polygonA[0] = *v1;
        polygonA[1] = *v2;
        polygonA[2] = *v3;
        LoadedVertex polygonB[16];
        LoadedVertex* input = polygonA;
        LoadedVertex* output = polygonB;
        size_t inputCount = 3;
        /* [debug] clippolymax shrinks the cap below 16 so the truncation arm is reachable on
         * demand; the storage is unchanged, so nothing can overrun. 0 = shipped (16). */
        const size_t clipCap = mClipPolyMax;
        uint32_t firstPlane = 0;
        if (mO3dsFast != 0) {
            bool passed = false;
            firstPlane = ClipPrepass(*v1, *v2, *v3, &inputCount, &passed);
            if (passed && mClipPolyHw < 3u) {
                mClipPolyHw = 3u;
            }
        }
        for (uint32_t plane = firstPlane; plane < 7 && inputCount >= 3; ++plane) {
            size_t outputCount = 0;
            bool truncated = false;
            if (mO3dsFast != 0) {
                ClipPlaneFast(input, inputCount, output, clipCap, plane, &outputCount, &truncated);
            }
            for (size_t i = 0; mO3dsFast == 0 && i < inputCount; ++i) {
                const LoadedVertex& current = input[i];
                const LoadedVertex& next = input[(i + 1) % inputCount];
                const float currentDistance = ClipDistance(current, plane);
                const float nextDistance = ClipDistance(next, plane);
                const bool currentInside = currentDistance >= 0.0f;
                const bool nextInside = nextDistance >= 0.0f;
                if (currentInside) {
                    if (outputCount < clipCap) {
                        output[outputCount++] = current;
                    } else {
                        truncated = true;
                    }
                }
                if (currentInside != nextInside && outputCount >= clipCap) truncated = true;
                if (currentInside != nextInside && outputCount < clipCap) {
                    const float denominator = currentDistance - nextDistance;
                    float t = denominator != 0.0f ? currentDistance / denominator : 0.0f;
                    if (t < 0.0f) t = 0.0f;
                    if (t > 1.0f) t = 1.0f;
                    output[outputCount++] = Interpolate(current, next, t);
                }
            }
            if (outputCount > mClipPolyHw) {
                mClipPolyHw = static_cast<uint32_t>(outputCount);
            }
            if (truncated) {
                mStats.clipTrunc++;
                gdx3ds_cov_mark(GDX3DS_COV_KEY(GDX3DS_COV_FAM_CLIP, GDX3DS_COV_CLIP_TRUNC));
                if (mDropsiteClipLogged == 0) {
                    mDropsiteClipLogged = 1;
                    GDXN_LOG("DROPSITE: clip polygon TRUNCATED at %u vertices on plane %u (input "
                             "%u) -- the fan below draws a malformed polygon. A convex input "
                             "cannot exceed 3+7=10 against the shipped cap of 16, so at 16 this "
                             "would mean the polygon lost convexity",
                             static_cast<unsigned>(clipCap), static_cast<unsigned>(plane),
                             static_cast<unsigned>(inputCount));
                }
            }
            inputCount = outputCount;
            LoadedVertex* tmp = input;
            input = output;
            output = tmp;
        }
        if (inputCount < 3) {
            mStats.trisRejected++;
            mStats.triClipAllOut++;
            gdx3ds_cov_mark(GDX3DS_COV_KEY(GDX3DS_COV_FAM_CLIP, GDX3DS_COV_CLIP_ALLOUT));
            return;
        }
        mStats.triClipFan += static_cast<uint32_t>(inputCount - 2u);
        /* COVERAGE: the fan's width is the feature — a 3-vertex fan is one triangle back, a
         * wider one is geometry the unclipped path would never have drawn. */
        gdx3ds_cov_mark(GDX3DS_COV_KEY(GDX3DS_COV_FAM_CLIP,
                                       GDX3DS_COV_CLIP_FAN | ((uint32_t)inputCount << 8)));
        LoadedVertex saved[3];
        for (size_t i = 0; i < 3; ++i) saved[i] = mRsp.loaded_vertices[kMaxVertices + i];
        mStats.tris--;
        mReplayClipTag = replayTag;
        ++mClipFanDepth;
        for (size_t i = 1; i + 1 < inputCount; ++i) {
            mRsp.loaded_vertices[kMaxVertices] = input[0];
            mRsp.loaded_vertices[kMaxVertices + 1] = input[i];
            mRsp.loaded_vertices[kMaxVertices + 2] = input[i + 1];
            for (size_t j = 0; j < 3; ++j) {
                LoadedVertex& cv = mRsp.loaded_vertices[kMaxVertices + j];
                cv.w = cv.w > kMinClipW ? cv.w : kMinClipW;
                UpdateClipFlags(cv);
            }
            SpTri(kMaxVertices, kMaxVertices + 1, kMaxVertices + 2, false);
        }
        --mClipFanDepth;
        for (size_t i = 0; i < 3; ++i) mRsp.loaded_vertices[kMaxVertices + i] = saved[i];
        }();
        return;
    }

    if (v1->clip_rej & v2->clip_rej & v3->clip_rej) {
        if (!is_rect) mStats.trisRejected++;
        mStats.triTrivialRej++;
        return;
    }

    if ((mRsp.geometry_mode & GM_CULL_BOTH) != 0) {
        float dx1 = v1->x / (v1->w) - v2->x / (v2->w);
        float dy1 = v1->y / (v1->w) - v2->y / (v2->w);
        float dx2 = v3->x / (v3->w) - v2->x / (v2->w);
        float dy2 = v3->y / (v3->w) - v2->y / (v2->w);
        float cross = dx1 * dy2 - dy1 * dx2;
        if ((v1->w < 0) ^ (v2->w < 0) ^ (v3->w < 0)) cross = -cross;
        if ((mRsp.extra_geometry_mode & 0x00000001u) != 0) cross = -cross; /* G_EX_INVERT_CULLING */
        const uint32_t cull_type = mRsp.geometry_mode & GM_CULL_BOTH;
        if (cull_type == GM_CULL_FRONT) {
            if (cross <= 0) {
                if (!is_rect) mStats.trisCulled++;
                mStats.triCull++;
                return;
            }
        } else if (cull_type == GM_CULL_BACK) {
            if (cross >= 0) {
                if (!is_rect) mStats.trisCulled++;
                mStats.triCull++;
                return;
            }
        } else {
            if (!is_rect) mStats.trisCulled++;
            mStats.triCull++;
            return;
        }
    }

    DrawState ds;
    clk.Enter(TRI_PH_RESOLVE);
    /* [native] hud_batch/shadow_batch/fx_batch: a draw whose state provably equals the open
     * batch's (the replay armed mHudRectFast, gdx_hud_replay.cpp HudRectState) takes the memo. */
    if (mSkyPending) {
        SkyMaterialize(); /* [native] sky_retain: the run's deferred texture state (gdx_sky_retain.cpp) */
    }
    if (!(mHudRectFast && HudRectState(is_rect, ds)) && !(mSkyRectFast && SkyRectState(is_rect, ds)) &&
        !ResolveDrawState(is_rect, ds)) {
        return;
    }
    clk.Enter(TRI_PH_EMIT);
    Combiner* comb = ds.comb;
    const uint32_t tm = ds.tm;
    const uint32_t(&tex_width)[2] = ds.tex_width;
    const uint32_t(&tex_height)[2] = ds.tex_height;
    const uint32_t(&tex_width2)[2] = ds.tex_width2;
    const uint32_t(&tex_height2)[2] = ds.tex_height2;
    const uint32_t(&effective_tile)[2] = ds.effective_tile;
    const bool(&usedTextures)[2] = ds.usedTextures;
    const uint8_t numInputs = ds.numInputs;
    const ClipParams clip_parameters = ds.clip;
    const bool use_alpha = ds.use_alpha;
    const bool use_fog = ds.use_fog;
    const bool use_blend_color = ds.use_blend_color;

    /* [nverify] diagnostic block: ~250-byte memset, ~35 field loads, two LoadedTexAt copies
     * and four FNV hashes per triangle. It was UNGATED on this path while the LUS twin has
     * always been guarded (interpreter.cpp:5134) — two agents found it independently.
     * The per-task latch means interp_verify=0 pays one branch (audit finding 31). */
    if (GDX_UNLIKELY(mVerifyActive)) {
        /* hotcold: the [nverify] side channel, out of line and cold. */
        [this, is_rect, comb, tm, v1, &tex_width, &tex_height, &tex_width2, &tex_height2, &effective_tile]() GDX_COLD {
        Gdx3dsVerifyTri vt;
        memset(&vt, 0, sizeof(vt));
        const Tile& t0 = mRdp.texture_tile[effective_tile[0]];
        const LoadedTexture l0 = LoadedTexAt(t0.tmem_index);
        vt.tile0 = effective_tile[0]; vt.tile1 = effective_tile[1]; vt.firstTile = mRdp.first_tile_index; vt.isRect = is_rect;
        vt.uls0 = t0.uls; vt.ult0 = t0.ult; vt.lrs0 = t0.lrs; vt.lrt0 = t0.lrt;
        vt.texW0 = tex_width[0]; vt.texH0 = tex_height[0]; vt.texW20 = tex_width2[0]; vt.texH20 = tex_height2[0];
        vt.texW1 = tex_width[1]; vt.texH1 = tex_height[1]; vt.tm = tm; vt.vu = v1->u; vt.vv = v1->v;
        vt.shifts0 = t0.shifts; vt.shiftt0 = t0.shiftt; vt.line0 = l0.line_size_bytes; vt.size0 = l0.size_bytes;
        vt.full0 = l0.full_image_line_size_bytes; vt.orig0 = l0.orig_size_bytes;
        vt.scaleS = mRsp.texture_scaling_factor.s; vt.scaleT = mRsp.texture_scaling_factor.t;
        vt.omh = mRdp.other_mode_h; vt.oml = mRdp.other_mode_l; vt.geo = mRsp.geometry_mode; vt.variant = mVariant;
        vt.vpx = mRdp.viewport.x; vt.vpy = mRdp.viewport.y; vt.vpw = mRdp.viewport.width; vt.vph = mRdp.viewport.height;
        vt.lists = mStats.lists; vt.cmds = mStats.cmds; vt.branchZ = mStats.branchZTaken; vt.noop = mStats.noopLists;
        vt.blendA = mRdp.blend_color.a;
        {
            const uint32_t tmemOff = static_cast<uint32_t>(t0.tmem_index) * 8u;
            vt.used0 = comb->usedTextures[0] ? 1u : 0u; vt.used1 = comb->usedTextures[1] ? 1u : 0u;
            vt.tmem0 = t0.tmem_index;
            vt.rtLine0 = t0.line_size_bytes;
            if (t0.tmem_index < 512 && mRdp.tmem_span_base[t0.tmem_index] != 0) {
                vt.base0 = mRdp.tmem_span_base[t0.tmem_index] - 1u;
                const LoadedTexture& br = mRdp.loaded_texture[vt.base0];
                vt.baseAddr0 = reinterpret_cast<unsigned long>(br.addr); vt.baseSize0 = br.size_bytes; vt.baseOrig0 = br.orig_size_bytes;
            }
            /* fmt/siz/palette of the render tile + the live palette staging (see the LUS twin). */
            vt.tc1 = (static_cast<unsigned>(t0.fmt) << 8) | static_cast<unsigned>(t0.siz) |
                     (static_cast<unsigned>(t0.palette) << 16) | (l0.immutable ? 0x1000000u : 0u);
            /* CI only, and only the palette half the decode reads: this is per-triangle. */
            vt.tc0 = 0;
            if (t0.fmt == IM_FMT_CI && (t0.siz == IM_SIZ_4b || t0.siz == IM_SIZ_8b)) {
                uint32_t ph = 2166136261u;
                if (t0.siz == IM_SIZ_4b) {
                    const uint8_t* pal = mRdp.palette_staging[(t0.palette / 8) & 1] + (t0.palette % 8) * 32u;
                    for (unsigned bi = 0; bi < 32u; bi++) ph = (ph ^ pal[bi]) * 16777619u;
                } else {
                    for (unsigned hi = 0; hi < 2u; hi++) {
                        for (unsigned bi = 0; bi < 256u; bi++) ph = (ph ^ mRdp.palette_staging[hi][bi]) * 16777619u;
                    }
                }
                vt.tc0 = ph;
            }
            uint32_t tmemSpan0 = 0;
            if (tmemOff < sizeof(mRdp.tmem)) {
                const uint32_t remaining = static_cast<uint32_t>(sizeof(mRdp.tmem)) - tmemOff;
                tmemSpan0 = VfyTmemFootprint(l0.size_bytes, l0.line_size_bytes);
                if (tmemSpan0 < 8u) tmemSpan0 = 8u;
                if (tmemSpan0 > remaining) tmemSpan0 = remaining;
            }
            VfyTriSource(&vt.addr0, &vt.cls0, &vt.srcHash0, &vt.tmemHash0, vt.srcB0, vt.tmemB0,
                         vt.used0 ? l0.addr : nullptr, l0.size_bytes,
                         tmemSpan0 != 0 ? mRdp.tmem + tmemOff : nullptr, tmemSpan0);
            if (vt.used1) {
                const Tile& t1 = mRdp.texture_tile[effective_tile[1]];
                const LoadedTexture l1 = LoadedTexAt(t1.tmem_index);
                const uint32_t tmemOff1 = static_cast<uint32_t>(t1.tmem_index) * 8u;
                uint32_t tmemSpan1 = 0;
                if (tmemOff1 < sizeof(mRdp.tmem)) {
                    const uint32_t remaining1 = static_cast<uint32_t>(sizeof(mRdp.tmem)) - tmemOff1;
                    tmemSpan1 = l1.size_bytes > 8u ? l1.size_bytes : 8u;
                    if (tmemSpan1 > remaining1) tmemSpan1 = remaining1;
                }
                VfyTriSource(&vt.addr1, &vt.cls1, &vt.srcHash1, &vt.tmemHash1, nullptr, nullptr, l1.addr,
                             l1.size_bytes, tmemSpan1 != 0 ? mRdp.tmem + tmemOff1 : nullptr, tmemSpan1);
                vt.tmem1 = t1.tmem_index; vt.line1 = l1.line_size_bytes; vt.size1 = l1.size_bytes; vt.full1 = l1.full_image_line_size_bytes;
                vt.orig1 = l1.orig_size_bytes;
            }
        }
        mBackend->VerifyNoteTri(&vt);
        }();
    }

    /* ---- packed emission ([triloop] + [trifast]) ---- */
    if (gdx3ds_replay_interp != nullptr) {
        ReplayBatchTag(replayTag);
    }
    if (mBufNumTris == 0) {
        /* GEOMLOSS: ask the backend how many triangles still fit rather than demanding room
         * for a whole kMaxTriBuffer batch. LUS answers a refusal by running its legacy
         * variable-stride loop, whose reservation is the size it actually needs — so the old
         * all-or-nothing begin made the owned path DROP triangles that interp=0 still draws,
         * for the last ~36 KiB of every frame's pool. Splitting the batch at the cap emits
         * exactly the same vertices in exactly the same order; only the draw count rises. */
        uint32_t capTris = 0;
        clk.Enter(TRI_PH_BEGIN);
        mStats.triBegin++;
        mPackDst = mBackend->VboPackBegin(&mPackParams, &capTris);
        clk.Enter(TRI_PH_EMIT);
        mPackBatch = (mPackDst != nullptr);
        mPackComb = comb; /* the fold key for this batch (== LUS sGdxPackComb) */
        if (!mPackBatch) {
            /* Genuine exhaustion (or no shader / no frame / [debug] triloop=0): nothing can be
             * emitted here and the LUS path would drop it too. The backend logs the reason
             * once; packRefused on the [ninterp] line is the recurring signal. */
            mStats.packRefused++;
            mPackCapTris = static_cast<size_t>(kMaxTriBuffer);
        } else {
            mPackCapTris = capTris < static_cast<uint32_t>(kMaxTriBuffer) ? static_cast<size_t>(capTris)
                                                                         : static_cast<size_t>(kMaxTriBuffer);
            if (mPackCapTris < static_cast<size_t>(kMaxTriBuffer)) {
                mStats.packSplit++;
            }
        }
    }
    if (!mPackBatch) {
        return;
    }

    struct PackInput {
        Rgba c;
        bool isShade;
    };
    const auto gdxInput = [&](int j, int k, int i) -> PackInput {
        Rgba tmp = { 0, 0, 0, 0 };
        bool isShade = false;
        switch (comb->shader_input_mapping[k][j]) {
            case CCMUX_PRIMITIVE: tmp = mRdp.prim_color; break;
            case CCMUX_SHADE: tmp = v_arr[i]->color; isShade = true; break;
            case CCMUX_SHADE_ALPHA: tmp.r = tmp.g = tmp.b = tmp.a = v_arr[i]->color.a; break;
            case CCMUX_ENVIRONMENT: tmp = mRdp.env_color; break;
            case CCMUX_PRIMITIVE_ALPHA: tmp.r = tmp.g = tmp.b = mRdp.prim_color.a; break;
            case CCMUX_ENV_ALPHA: tmp.r = tmp.g = tmp.b = mRdp.env_color.a; break;
            case CCMUX_PRIM_LOD_FRAC: tmp.r = tmp.g = tmp.b = mRdp.prim_lod_fraction; break;
            case CCMUX_LOD_FRACTION: {
                if (mRdp.other_mode_l & (1u << MDSFT_TEXTLOD)) {
                    float distance_frac = (v1->w - 3000.0f) / 3000.0f;
                    if (distance_frac < 0.0f) distance_frac = 0.0f;
                    if (distance_frac > 1.0f) distance_frac = 1.0f;
                    tmp.r = tmp.g = tmp.b = tmp.a = static_cast<uint8_t>(distance_frac * 255.0f);
                } else {
                    tmp.r = tmp.g = tmp.b = tmp.a = 255;
                }
                break;
            }
            case CCMUX_KEY_CENTER: tmp = mRdp.key_center; break;
            case CCMUX_KEY_SCALE: tmp = mRdp.key_scale; break;
            case CCMUX_CONVERT_K4: tmp.r = tmp.g = tmp.b = static_cast<uint8_t>(mRdp.convert_k[4]); break;
            case CCMUX_CONVERT_K5: tmp.r = tmp.g = tmp.b = static_cast<uint8_t>(mRdp.convert_k[5]); break;
            default: break;
        }
        /* ACMUX_PRIM_LOD_FRAC (6) aliases CCMUX_1/CENTER in the alpha mapping: LUS's switch hits
           the G_ACMUX_PRIM_LOD_FRAC arm (tmp.a = prim_lod_fraction) because 6 has no CCMUX case
           listed before it. Reproduce: */
        if (k == 1 && comb->shader_input_mapping[k][j] == ACMUX_PRIM_LOD_FRAC) {
            tmp = Rgba{ 0, 0, 0, mRdp.prim_lod_fraction };
            isShade = false;
        }
        return PackInput{ tmp, isShade };
    };

    float clampU[2] = { 1e30f, 1e30f };
    float clampV[2] = { 1e30f, 1e30f };
    for (int t = 0; t < 2; t++) {
        if (!usedTextures[t]) continue;
        clampU[t] = mPackParams.wrapClampU[t];
        clampV[t] = mPackParams.wrapClampV[t];
        if (mPackParams.shaderClampGate) {
            if (tm & (1u << (2 * t))) clampU[t] = ((tex_width2[t] - 0.5f) / tex_width[t]) * mPackParams.uScale[t];
            if (tm & (1u << (2 * t + 1))) clampV[t] = ((tex_height2[t] - 0.5f) / tex_height[t]) * mPackParams.vScale[t];
        }
    }

    if (mBufNumTris == 0) {
        mPackAux.numInputs = static_cast<uint8_t>(numInputs < 7 ? numInputs : 7);
        for (int j = 0; j < mPackAux.numInputs; j++) {
            const PackInput cv = gdxInput(j, 0, 0);
            mPackAux.inputRgba[j][0] = cv.c.r / 255.0f;
            mPackAux.inputRgba[j][1] = cv.c.g / 255.0f;
            mPackAux.inputRgba[j][2] = cv.c.b / 255.0f;
            if (use_alpha) {
                const PackInput av = gdxInput(j, 1, 0);
                mPackAux.inputRgba[j][3] = (use_fog && !use_blend_color && av.isShade) ? 1.0f : av.c.a / 255.0f;
            } else {
                mPackAux.inputRgba[j][3] = 1.0f;
            }
        }
        mPackAux.hasFog = use_fog ? 1 : 0;
        if (use_fog) {
            const Rgba& fogSrc = use_blend_color ? mRdp.blend_color : mRdp.fog_color;
            mPackAux.fogRgb[0] = fogSrc.r / 255.0f;
            mPackAux.fogRgb[1] = fogSrc.g / 255.0f;
            mPackAux.fogRgb[2] = fogSrc.b / 255.0f;
            mPackAux.fogFactor0 = use_blend_color ? mRdp.fog_color.a / 255.0f : v_arr[0]->color.a / 255.0f;
        } else {
            mPackAux.fogRgb[0] = mPackAux.fogRgb[1] = mPackAux.fogRgb[2] = 0.0f;
            mPackAux.fogFactor0 = 0.0f;
        }
    }

    float* gdxOut = mPackDst + mBufNumTris * (3 * GDX3DS_VBOPACK_STRIDE);
    const bool bilerp = (mRdp.other_mode_h & (3u << MDSFT_TEXTFILT)) == TF_BILERP;
    const bool halfTexel = bilerp && !is_rect; /* GDX_RECT_HALF_TEXEL default off */
    struct UvPrep {
        float uOff, vOff, uPre, vPre, uNormMul, vNormMul, uNormDiv, vNormDiv;
        bool uMul, vMul;
    } prep[2];
    for (int t = 0; t < 2; t++) {
        if (!usedTextures[t]) continue;
        const Tile& ut = mRdp.texture_tile[effective_tile[t]];
        const int shifts = ut.shifts, shiftt = ut.shiftt;
        prep[t].uPre = shifts == 0 ? 1.0f : (shifts <= 10 ? 1.0f / static_cast<float>(1 << shifts) : static_cast<float>(1 << (16 - shifts)));
        prep[t].vPre = shiftt == 0 ? 1.0f : (shiftt <= 10 ? 1.0f / static_cast<float>(1 << shiftt) : static_cast<float>(1 << (16 - shiftt)));
        prep[t].uOff = ut.uls / 4.0f;
        prep[t].vOff = ut.ult / 4.0f;
        const uint32_t tw = tex_width[t], th = tex_height[t];
        prep[t].uMul = (tw & (tw - 1u)) == 0u;
        prep[t].vMul = (th & (th - 1u)) == 0u;
        prep[t].uNormDiv = static_cast<float>(tw);
        prep[t].vNormDiv = static_cast<float>(th);
        prep[t].uNormMul = prep[t].uMul ? 1.0f / static_cast<float>(tw) : 1.0f;
        prep[t].vNormMul = prep[t].vMul ? 1.0f / static_cast<float>(th) : 1.0f;
    }
    const int ci = mPackParams.vtxColorInput - 1;
    const bool colorIsShade = ci >= 0 && comb->shader_input_mapping[0][ci] == CCMUX_SHADE;
    const int ai = mPackParams.vtxAlphaInput - 1;
    const bool alphaIsShade = ai >= 0 && comb->shader_input_mapping[1][ai] == CCMUX_SHADE;
    const bool fogFactorConst = mPackParams.fogFactorToAlpha && use_blend_color;
    const float fogFactorConstVal = fogFactorConst ? mByteToUnit[mRdp.fog_color.a] : 0.0f;
    const bool shadeAlphaIsOne = use_fog && !use_blend_color;
    for (int i = 0; i < 3; i++) {
        float* out = gdxOut + i * GDX3DS_VBOPACK_STRIDE;
        const LoadedVertex* vv = v_arr[i];
        if (gdx3ds_replay_interp != nullptr) {
            /* [native] replay (C6): which loaded slot this output vertex is (the clipper's are not) */
            const ptrdiff_t slot = vv - &mRsp.loaded_vertices[0];
            gdx3ds_replay_interp->outVtx(out, (slot >= 0 && slot < kMaxVertices) ? static_cast<int>(slot) : -1);
        }
        float z = vv->z, w = vv->w;
        if (clip_parameters.zIsFrom0To1) z = (z + w) / 2.0f;
        out[0] = vv->x;
        out[1] = clip_parameters.invertY ? -vv->y : vv->y;
        out[2] = z;
        out[3] = w;
        for (int t = 0; t < 2; t++) {
            const int slot = (t == 0) ? 4 : 10;
            if (!usedTextures[t]) {
                out[slot] = 0.0f;
                out[slot + 1] = 0.0f;
                continue;
            }
            float u = vv->u / 32.0f;
            float v = vv->v / 32.0f;
            u *= prep[t].uPre;
            v *= prep[t].vPre;
            u -= prep[t].uOff;
            v -= prep[t].vOff;
            if (halfTexel) {
                u += 0.5f;
                v += 0.5f;
            }
            const float nu = prep[t].uMul ? u * prep[t].uNormMul : u / prep[t].uNormDiv;
            const float nv = prep[t].vMul ? v * prep[t].vNormMul : v / prep[t].vNormDiv;
            const float pu = FminExact(nu * mPackParams.uScale[t], clampU[t]);
            const float pv = FminExact(nv * mPackParams.vScale[t], clampV[t]);
            if (mPackParams.hasOff) {
                out[slot] = pu + mPackParams.uOff[t];
                out[slot + 1] = pv + mPackParams.vOff[t];
            } else {
                out[slot] = pu;
                out[slot + 1] = pv;
            }
        }
        if (ci >= 0) {
            if (colorIsShade) {
                out[6] = mByteToUnit[vv->color.r];
                out[7] = mByteToUnit[vv->color.g];
                out[8] = mByteToUnit[vv->color.b];
            } else {
                const PackInput cv = gdxInput(ci, 0, i);
                out[6] = mByteToUnit[cv.c.r];
                out[7] = mByteToUnit[cv.c.g];
                out[8] = mByteToUnit[cv.c.b];
            }
        } else {
            out[6] = out[7] = out[8] = 1.0f;
        }
        if (mPackParams.fogFactorToAlpha) {
            out[9] = fogFactorConst ? fogFactorConstVal : mByteToUnit[vv->color.a];
        } else if (mPackParams.wantVtxAlpha) {
            if (alphaIsShade) {
                out[9] = shadeAlphaIsOne ? 1.0f : mByteToUnit[vv->color.a];
            } else {
                const PackInput av = gdxInput(ai, 1, i);
                out[9] = (shadeAlphaIsOne && av.isShade) ? 1.0f : mByteToUnit[av.c.a];
            }
        } else {
            out[9] = 1.0f;
        }
    }
#ifdef EXPANSION_KIT
    if (GDX_UNLIKELY(mEkCap != nullptr)) {
        EkPvCaptureTri(gdxOut, v_arr[0], ds); /* [native] ek_editmesh: record this element's output */
    }
#endif
    if (mHsxSkyVfy && !is_rect) {
        HsxSkyVerifyEmit(ds, v_arr, gdxOut, halfTexel); /* [native] hudsky_emit=2: the sky's compact packing */
    }
    if (use_fog) {
        if (use_blend_color) {
            mBackend->FogNoteParams(0, 0, 0);
        } else {
            mBackend->FogNoteParams(mRsp.fog_mul, mRsp.fog_offset, 1);
        }
    }
    if (!is_rect) mStats.trisEmitted++;
    mStats.triEmitAll++;
    /* GEOMLOSS: >= not ==, and against the pool-derived cap: under pool pressure the cap is
     * below kMaxTriBuffer and the batch is flushed early instead of overrunning the pool. */
    if (++mBufNumTris >= mPackCapTris) {
        Flush(FLUSH_FULL);
    }
}


/* ---- [native] machines_mattab: the per-run machine material record (C5 Stage 1b) ----------
 *
 * A retained machine mesh binds 5-8 material runs per draw, and each run's ResolveDrawState
 * recomputes the same answer every frame: the combiner lookup, the tile extents, the sampler
 * modes and -- because every run selects a different tile, which re-arms textures_changed --
 * a texture IMPORT whose key hashes the whole source span (FNV over 0.5-2 KiB). On an Old 3DS
 * that material path, not the vertices, is most of the ~215 us per run the hardware measured.
 *
 * The record splits ResolveDrawState into what it COMPUTES and what it APPLIES:
 *   computed  every target (depth/decal bits, the combiner and shader, per-unit sampler
 *             modes, use_alpha, the tile extents, the clip parameters) is a pure function of
 *             the registers in the key: combine_mode, other_mode_l/h, geometry_mode,
 *             grayscale, first_tile_index, the texel0/1 tiles and LoadedTexAt(their
 *             tmem_index). The combiner pool never evicts and a combiner's prg[tm] is
 *             filled once, so the same key yields the same pointers.
 *   applied   each target is DIFFED against mRs (the backend state the last batch was drawn
 *             with) and only a difference calls the backend -- behind the same Flush the full
 *             path takes. A hit runs exactly that diff against the LIVE mRs, in the same
 *             order, so it issues exactly the calls the full path would.
 *   imported  a unit whose textures_changed is armed imports. The record keeps the cache slot
 *             that unit's import HIT and that entry's key. The replay (select, LRU refresh,
 *             the import/hit counters) is valid while the slot still holds that exact key and
 *             the source is `immutable`: TexKey is a function of the tile, the span view (in
 *             the record's key) and content_hash = FNV(bytes at addr, min(size, readable)).
 *             An immutable source lives in an o2r asset-segment image, which is never evicted
 *             or rewritten once published (gLoadedAssetSegments; the kTmemSrcStable argument
 *             the TMEM same-content skip already ships on), so the bytes -- and the hash --
 *             are the recorded ones; `readable` moves only with the range tables, which the
 *             classification epoch (impEpoch, per unit, taken at the recorded import) folds. A retired or reused slot fails the key
 *             compare (TexCacheRetireSlot clears `used`; a new occupant has another key).
 *             CI sources (palette-dependent keys) are never recorded.
 * Verify mode (2) never serves a record: it runs the full resolve and compares it with what the
 * record would have done (targets, the DrawState, the post-resolve mRs, the bound slots). */
static int sUnrecordableLogs = 0; /* first few imports a record could not keep (receipt) */

struct MachMatOps {
    static bool TileEq(const Tile& a, const Tile& b) {
        return a.fmt == b.fmt && a.siz == b.siz && a.cms == b.cms && a.cmt == b.cmt && a.masks == b.masks &&
               a.maskt == b.maskt && a.shifts == b.shifts && a.shiftt == b.shiftt &&
               memcmp(&a.uls, &b.uls, sizeof(float)) == 0 && memcmp(&a.ult, &b.ult, sizeof(float)) == 0 &&
               memcmp(&a.lrs, &b.lrs, sizeof(float)) == 0 && memcmp(&a.lrt, &b.lrt, sizeof(float)) == 0 &&
               a.tmem == b.tmem && a.line_size_bytes == b.line_size_bytes && a.palette == b.palette &&
               a.tmem_index == b.tmem_index;
    }

    static bool LtEq(const LoadedTexture& a, const LoadedTexture& b) {
        return a.addr == b.addr && a.orig_size_bytes == b.orig_size_bytes && a.size_bytes == b.size_bytes &&
               a.full_image_line_size_bytes == b.full_image_line_size_bytes &&
               a.line_size_bytes == b.line_size_bytes && a.tmem_start == b.tmem_start &&
               a.tmem_word_count == b.tmem_word_count && a.immutable == b.immutable &&
               a.native_rgba16 == b.native_rgba16 && a.masked == b.masked && a.blended == b.blended;
    }

    static bool DsEq(const DrawState& a, const DrawState& b) {
        if (a.comb != b.comb || a.prg != b.prg || a.tm != b.tm || a.numInputs != b.numInputs ||
            a.clip.zIsFrom0To1 != b.clip.zIsFrom0To1 || a.clip.invertY != b.clip.invertY ||
            a.use_alpha != b.use_alpha || a.use_fog != b.use_fog || a.use_blend_color != b.use_blend_color) {
            return false;
        }
        for (int i = 0; i < 2; i++) {
            if (a.tex_width[i] != b.tex_width[i] || a.tex_height[i] != b.tex_height[i] ||
                a.tex_width2[i] != b.tex_width2[i] || a.tex_height2[i] != b.tex_height2[i] ||
                a.effective_tile[i] != b.effective_tile[i] || a.usedTextures[i] != b.usedTextures[i]) {
                return false;
            }
        }
        return true;
    }

    static uint32_t UnitTile(const Interp& I, int unit) {
        return unit == 1 ? ((I.mRdp.first_tile_index + 1u) & 7u) : I.mRdp.first_tile_index;
    }

    /* The key half: every register the resolve reads to compute its targets. Returns 0 on a
     * match, else which parts differ (MachMatOutcome::keyBits, for the receipt). */
    static uint16_t KeyDiff(const Interp& I, const MachMatRec& r) {
        uint16_t bits = 0;
        bits |= r.combineMode != I.mRdp.combine_mode ? 0x001u : 0u;
        bits |= r.otherModeL != I.mRdp.other_mode_l ? 0x002u : 0u;
        bits |= r.otherModeH != I.mRdp.other_mode_h ? 0x004u : 0u;
        bits |= r.geometryMode != I.mRsp.geometry_mode ? 0x008u : 0u;
        bits |= r.firstTile != I.mRdp.first_tile_index ? 0x010u : 0u;
        bits |= r.grayscale != (I.mRdp.grayscale ? 1u : 0u) ? 0x020u : 0u;
        if (bits != 0) {
            return bits; /* the tiles below are addressed through first_tile_index */
        }
        for (int i = 0; i < 2; i++) {
            if (!r.ds.comb->usedTextures[i]) {
                continue;
            }
            const Tile& live = I.mRdp.texture_tile[UnitTile(I, i)];
            bits |= !TileEq(r.tiles[i], live) ? static_cast<uint16_t>(0x040u << i) : 0u;
            bits |= !LtEq(r.lt[i], I.LoadedTexAt(live.tmem_index)) ? static_cast<uint16_t>(0x100u << i) : 0u;
        }
        return bits;
    }

    static bool KeyMatches(const Interp& I, const MachMatRec& r) {
        return KeyDiff(I, r) == 0;
    }

    /* The recorded import of `unit` would be exactly the stock import's cache hit. */
    static bool SlotValid(const Interp& I, const MachMatRec& r, int unit) {
        const int16_t s = r.slot[unit];
        if (s < 0 || s >= static_cast<int16_t>(kTexCacheSize)) {
            return false;
        }
        const TexEntry& e = I.mTex[s];
        return e.used && e.uploaded && memcmp(&e.key, &r.texKey[unit], sizeof(TexKey)) == 0;
    }

    /* Everything a hit needs, checked BEFORE anything is applied (a miss leaves no trace). */
    /* The way whose key matches the live registers, else -1 (keyBits: way 0's difference). */
    static int FindWay(const Interp& I, MachMatRec* const* recs, int ways, uint16_t* keyBits) {
        for (int w = 0; w < ways; w++) {
            if (!recs[w]->valid) {
                continue;
            }
            const uint16_t bits = KeyDiff(I, *recs[w]);
            if (bits == 0) {
                return w;
            }
            if (*keyBits == 0) {
                *keyBits = bits;
            }
        }
        return -1;
    }

    /* The way a full resolve banks into when no way's key matched: an empty one, else the
     * least recently used. */
    static int VictimWay(MachMatRec* const* recs, int ways) {
        int victim = 0;
        for (int w = 0; w < ways; w++) {
            if (!recs[w]->valid) {
                return w;
            }
            if (recs[w]->useSeq < recs[victim]->useSeq) {
                victim = w;
            }
        }
        return victim;
    }

    /* The rest of a hit's preconditions, for the way whose key matched. */
    static MachMatResult Validate(Interp& I, const MachMatRec& r, bool live, uint8_t* liveMask) {
        uint32_t epoch = 0;
        *liveMask = 0;
        for (int i = 0; i < 2; i++) {
            if (!r.ds.comb->usedTextures[i] || !I.mRdp.textures_changed[i]) {
                continue;
            }
            if (!r.importOk[i] && live) {
                /* [native] machines_mattab_live: this unit imports LIVE (the stock ImportTexture,
                 * at the stock point of the apply order), so only its sampler target is served
                 * and that must have been recorded. */
                if (!r.smpKnown[i]) {
                    return MM_MISS_SMP;
                }
                *liveMask = static_cast<uint8_t>(*liveMask | (1u << i));
                continue;
            }
            if (!r.importOk[i]) {
                return MM_MISS_NOIMP;
            }
            if (!SlotValid(I, r, i)) {
                return MM_MISS_TEX;
            }
            if (epoch == 0) {
                epoch = I.mSrc.TimgClassEpoch(); /* never 0 */
            }
            if (r.impEpoch[i] != epoch) {
                return MM_MISS_EPOCH;
            }
        }
        for (int i = 0; i < 2; i++) {
            if (!r.ds.comb->usedTextures[i] || ((*liveMask >> i) & 1u) != 0) {
                continue;
            }
            const int16_t bound = I.mRdp.textures_changed[i] ? r.slot[i] : I.mRs.textures[i];
            if (bound >= 0 && !r.smpKnown[i]) {
                return MM_MISS_SMP;
            }
        }
        return MM_HIT;
    }

    /* TexCacheLookup's hit branch for a validated slot (gdx_texcache.cpp; keep in step). */
    static void ReplayImportHit(Interp& I, int unit, int16_t s) {
        I.mStats.texImports++;
        I.mBackend->TexCacheNoteImport();
        if (I.mTexOv != nullptr) {
            I.TexSkipCheckImport(static_cast<int>(UnitTile(I, unit)));
        }
        TexEntry* tex = I.mTex;
        I.mBackend->SelectTexture(unit, tex[s].texture_id);
        I.mRs.textures[unit] = s;
        TexEntry& e = tex[s];
        if (e.lruPrev >= 0) tex[e.lruPrev].lruNext = e.lruNext; else I.mTexLruHead = e.lruNext;
        if (e.lruNext >= 0) tex[e.lruNext].lruPrev = e.lruPrev; else I.mTexLruTail = e.lruPrev;
        e.lruPrev = I.mTexLruTail;
        e.lruNext = -1;
        if (I.mTexLruTail >= 0) tex[I.mTexLruTail].lruNext = s; else I.mTexLruHead = s;
        I.mTexLruTail = s;
        I.mStats.texHits++;
    }

    /* ResolveDrawState's viewport/scissor block, verbatim: it reads only live registers and
     * mRs (no key input), so a hit runs it as the full path does. */
    static uint8_t ApplyViewportScissor(Interp& I) {
        if (!I.mRdp.viewport_or_scissor_changed) {
            return 0;
        }
        uint8_t binds = 0;
        if (memcmp(&I.mRdp.viewport, &I.mRs.viewport, sizeof(Rect)) != 0) {
            I.Flush(FLUSH_VIEWPORT);
            I.mBackend->SetViewport(I.mRdp.viewport.x, I.mRdp.viewport.y, I.mRdp.viewport.width,
                                    I.mRdp.viewport.height);
            I.mRs.viewport = I.mRdp.viewport;
            binds++;
        }
        if (memcmp(&I.mRdp.scissor, &I.mRs.scissor, sizeof(Rect)) != 0) {
            I.Flush(FLUSH_SCISSOR);
            I.mBackend->SetScissor(I.mRdp.scissor.x, I.mRdp.scissor.y, I.mRdp.scissor.width, I.mRdp.scissor.height);
            I.mRs.scissor = I.mRdp.scissor;
            binds++;
        }
        I.mRdp.viewport_or_scissor_changed = false;
        return binds;
    }

    /* The hit: ResolveDrawState's apply half against the live mRs, in its order. */
    /* ResolveDrawState's import of one unit, verbatim for a triangle (never a rect, so the
     * [trectbatch] deferral and atlas arm are off): the flush, the stock import (key, lookup,
     * decode on a miss), the disarm. */
    static void LiveImport(Interp& I, int unit) {
        I.Flush(FLUSH_TEXTURE);
        I.ImportTexture(unit, static_cast<int>(UnitTile(I, unit)));
    }

    static uint8_t Apply(Interp& I, const MachMatRec& r, uint8_t liveMask) {
        uint8_t binds = 0;
        if (r.depthTM != I.mRs.depth_test_and_mask) {
            I.Flush(FLUSH_DEPTH);
            I.mBackend->SetDepthTestAndMask((r.depthTM & 1u) != 0, (r.depthTM & 2u) != 0);
            I.mRs.depth_test_and_mask = r.depthTM;
            binds++;
        }
        if (r.decal != I.mRs.decal_mode) {
            I.Flush(FLUSH_DECAL);
            I.mBackend->SetZmodeDecal(r.decal != 0);
            I.mRs.decal_mode = r.decal;
            binds++;
        }
        binds += ApplyViewportScissor(I);
        for (int i = 0; i < 2; i++) {
            if (!r.ds.comb->usedTextures[i]) {
                continue;
            }
            if (I.mRdp.textures_changed[i] && ((liveMask >> i) & 1u) != 0) {
                LiveImport(I, i);
                I.mRdp.textures_changed[i] = false;
                binds++;
            } else if (I.mRdp.textures_changed[i]) {
                I.Flush(FLUSH_TEXTURE);
                ReplayImportHit(I, i, r.slot[i]);
                I.mRdp.textures_changed[i] = false;
                binds++;
            }
            if (I.mRs.textures[i] < 0) {
                continue;
            }
            const bool linear = r.smpLinear[i] != 0;
            if (!I.mRs.sampler_valid[i] || linear != I.mRs.sampler_linear[i] || r.smpCms[i] != I.mRs.sampler_cms[i] ||
                r.smpCmt[i] != I.mRs.sampler_cmt[i]) {
                I.Flush(FLUSH_SAMPLER);
                I.mBackend->SetSamplerParameters(i, linear, r.smpCms[i], r.smpCmt[i]);
                I.mRs.sampler_valid[i] = true;
                I.mRs.sampler_linear[i] = linear;
                I.mRs.sampler_cms[i] = r.smpCms[i];
                I.mRs.sampler_cmt[i] = r.smpCmt[i];
                binds++;
            }
        }
        if (r.ds.prg != I.mRs.shader_program) {
            I.Flush(FLUSH_SHADER);
            I.mBackend->UnloadShader(I.mRs.shader_program);
            I.mBackend->LoadShader(r.ds.prg);
            I.mRs.shader_program = r.ds.prg;
            binds++;
        }
        if (r.useAlpha != I.mRs.alpha_blend) {
            I.Flush(FLUSH_ALPHA);
            I.mBackend->SetUseAlpha(r.useAlpha != 0);
            I.mRs.alpha_blend = r.useAlpha;
            binds++;
        }
        return binds;
    }

    /* What Apply would leave in mRs, without touching the backend (verify mode). */
    static void Predict(const Interp& I, const MachMatRec& r, uint8_t liveMask, RenderState& rs) {
        rs = I.mRs;
        rs.depth_test_and_mask = r.depthTM;
        rs.decal_mode = r.decal;
        if (I.mRdp.viewport_or_scissor_changed) {
            rs.viewport = I.mRdp.viewport;
            rs.scissor = I.mRdp.scissor;
        }
        for (int i = 0; i < 2; i++) {
            if (!r.ds.comb->usedTextures[i]) {
                continue;
            }
            if (I.mRdp.textures_changed[i] && ((liveMask >> i) & 1u) != 0) {
                continue; /* a live import: its slot is the full resolve's (PredictLive) */
            }
            if (I.mRdp.textures_changed[i]) {
                rs.textures[i] = r.slot[i];
            }
            if (rs.textures[i] < 0) {
                continue;
            }
            rs.sampler_valid[i] = true;
            rs.sampler_linear[i] = r.smpLinear[i] != 0;
            rs.sampler_cms[i] = r.smpCms[i];
            rs.sampler_cmt[i] = r.smpCmt[i];
        }
        rs.shader_program = r.ds.prg;
        rs.alpha_blend = r.useAlpha;
    }

    /* Verify mode, after the full resolve: a live unit's slot is whatever the stock import
     * bound (the hit would have run that same import), then the recorded sampler over it. */
    static void PredictLive(const Interp& I, const MachMatRec& r, uint8_t liveMask, RenderState& rs) {
        for (int i = 0; i < 2; i++) {
            if (((liveMask >> i) & 1u) == 0) {
                continue;
            }
            rs.textures[i] = I.mRs.textures[i];
            if (rs.textures[i] < 0) {
                continue;
            }
            rs.sampler_valid[i] = true;
            rs.sampler_linear[i] = r.smpLinear[i] != 0;
            rs.sampler_cms[i] = r.smpCms[i];
            rs.sampler_cmt[i] = r.smpCmt[i];
        }
    }

    static bool RsEq(const RenderState& a, const RenderState& b) {
        if (a.depth_test_and_mask != b.depth_test_and_mask || a.decal_mode != b.decal_mode ||
            a.alpha_blend != b.alpha_blend || a.shader_program != b.shader_program ||
            memcmp(&a.viewport, &b.viewport, sizeof(Rect)) != 0 || memcmp(&a.scissor, &b.scissor, sizeof(Rect)) != 0) {
            return false;
        }
        for (int i = 0; i < 2; i++) {
            if (a.textures[i] != b.textures[i] || a.sampler_valid[i] != b.sampler_valid[i]) {
                return false;
            }
            if (a.textures[i] >= 0 && (a.sampler_linear[i] != b.sampler_linear[i] ||
                                       a.sampler_cms[i] != b.sampler_cms[i] || a.sampler_cmt[i] != b.sampler_cmt[i])) {
                return false;
            }
        }
        return true;
    }

    /* Import bookkeeping around one full resolve: which armed units imported by a cache hit. */
    struct ImportProbe {
        bool armed[2];
        uint32_t hits0, misses0;
    };

    static ImportProbe ProbeBefore(const Interp& I) {
        ImportProbe p;
        p.armed[0] = I.mRdp.textures_changed[0];
        p.armed[1] = I.mRdp.textures_changed[1];
        p.hits0 = I.mStats.texHits;
        p.misses0 = I.mStats.texMisses;
        return p;
    }

    static bool ImportRecordable(const LoadedTexture& lt, const Tile& t) {
        return lt.immutable != 0 && lt.addr != nullptr && t.fmt != IM_FMT_CI;
    }

    /* Write the record from the full resolve that just ran (the targets are what it left in
     * mRs: each one was either set to its target or already equal to it). */
    static void Bank(Interp& I, MachMatRec& r, const DrawState& ds, const ImportProbe& p) {
        const bool sameKey = r.valid && r.ds.comb == ds.comb && KeyMatches(I, r);
        r.combineMode = I.mRdp.combine_mode;
        r.otherModeL = I.mRdp.other_mode_l;
        r.otherModeH = I.mRdp.other_mode_h;
        r.geometryMode = I.mRsp.geometry_mode;
        r.firstTile = I.mRdp.first_tile_index;
        r.grayscale = I.mRdp.grayscale ? 1u : 0u;
        r.depthTM = I.mRs.depth_test_and_mask;
        r.decal = I.mRs.decal_mode;
        r.useAlpha = I.mRs.alpha_blend;
        r.ds = ds;
        unsigned armedUsed = 0;
        for (int i = 0; i < 2; i++) {
            if (ds.comb->usedTextures[i] && p.armed[i]) {
                armedUsed++;
            }
        }
        const bool allHits = I.mStats.texMisses == p.misses0 && I.mStats.texHits - p.hits0 == armedUsed;
        for (int i = 0; i < 2; i++) {
            const Tile& live = I.mRdp.texture_tile[UnitTile(I, i)];
            r.tiles[i] = live;
            r.lt[i] = I.LoadedTexAt(live.tmem_index);
            const int16_t bound = I.mRs.textures[i];
            r.smpKnown[i] = bound >= 0 ? 1u : 0u;
            r.smpLinear[i] = I.mRs.sampler_linear[i] ? 1u : 0u;
            r.smpCms[i] = I.mRs.sampler_cms[i];
            r.smpCmt[i] = I.mRs.sampler_cmt[i];
            if (!ds.comb->usedTextures[i]) {
                r.importOk[i] = 0;
                continue;
            }
            if (p.armed[i]) {
                const bool ok = allHits && bound >= 0 && ImportRecordable(r.lt[i], live);
                if (!ok && sUnrecordableLogs < 12) {
                    sUnrecordableLogs++;
                    GDXN_LOG("[mattab] import not recordable: unit=%d tile=%u fmt=%u siz=%u immutable=%u addr=%p "
                             "hits=+%lu misses=+%lu armed=%u bound=%d (a record serves only immutable non-CI hits)",
                             i, static_cast<unsigned>(UnitTile(I, i)), static_cast<unsigned>(live.fmt),
                             static_cast<unsigned>(live.siz), static_cast<unsigned>(r.lt[i].immutable), r.lt[i].addr,
                             static_cast<unsigned long>(I.mStats.texHits - p.hits0),
                             static_cast<unsigned long>(I.mStats.texMisses - p.misses0), armedUsed,
                             static_cast<int>(bound));
                }
                r.importOk[i] = ok ? 1u : 0u;
                r.slot[i] = ok ? bound : static_cast<int16_t>(-1);
                if (ok) {
                    memcpy(&r.texKey[i], &I.mTex[bound].key, sizeof(TexKey));
                    r.impEpoch[i] = I.mSrc.TimgClassEpoch();
                }
            } else if (!sameKey) {
                r.importOk[i] = 0; /* a new key: the old import answer is not this key's */
            }
        }
        r.valid = 1;
    }
};

static uint32_t sMachMatSeq = 0; /* drawing thread only */

bool Interp::MachMatResolve(MachMatRec* const* recs, int ways, int modeArg, DrawState& out, MachMatOutcome& o) {
    memset(&o, 0, sizeof(o));
    /* GDXSEAM_MATREC_LIVE (bit 4): [native] machines_mattab_live -- a unit the record holds no
     * import hit for imports live instead of missing (MachMatOps::Validate). */
    const bool live = (modeArg & 0x10) != 0;
    const int mode = modeArg & 0x0F;
    uint8_t liveMask = 0;
    uint16_t keyBits = 0;
    const int way = MachMatOps::FindWay(*this, recs, ways, &keyBits);
    bool anyValid = false;
    for (int w = 0; w < ways; w++) {
        anyValid = anyValid || recs[w]->valid != 0;
    }
    MachMatResult v = anyValid ? MM_MISS_KEY : MM_MISS_EMPTY;
    if (way >= 0) {
        v = MachMatOps::Validate(*this, *recs[way], live, &liveMask);
    } else {
        o.keyBits = keyBits;
    }
    MachMatRec& rec = *recs[way >= 0 ? way : MachMatOps::VictimWay(recs, ways)];
    const bool wasValid = rec.valid != 0;
    rec.useSeq = ++sMachMatSeq;
    if (mode == 1 && v == MM_HIT) {
        o.result = MM_HIT;
        o.binds = MachMatOps::Apply(*this, rec, liveMask);
        o.live = liveMask;
        out = rec.ds;
        mTriMemo = out;
        mTriMemoValid = true;
        if (mTriMemoOn != 0) {
            TriMemoBank(false, out);
        }
        return true;
    }
    o.result = mode == 1 ? static_cast<uint8_t>(v) : static_cast<uint8_t>(MM_FULL);
    /* An invalidation: a record that had served and now cannot, because what it proved moved
     * (a key part, the cache slot, the epoch). A record that never held an import (noimp) is a
     * standing miss, not an invalidation. */
    const bool invalidating = v == MM_MISS_KEY || v == MM_MISS_TEX || v == MM_MISS_EPOCH || v == MM_MISS_SMP;
    o.wasValid = (wasValid && invalidating) ? 1u : 0u;
    RenderState predicted;
    const bool check = mode == 2 && v == MM_HIT;
    if (GDX_UNLIKELY(check)) {
        MachMatOps::Predict(*this, rec, liveMask, predicted);
    }
    const MachMatOps::ImportProbe probe = MachMatOps::ProbeBefore(*this);
    if (!ResolveDrawState(false, out)) {
        return false;
    }
    if (GDX_UNLIKELY(check)) {
        /* hotcold: verify mode (2) only, out of line and cold. */
        [this, &rec, liveMask, &predicted, &o, &out]() GDX_COLD {
        MachMatOps::PredictLive(*this, rec, liveMask, predicted);
        o.live = liveMask;
        o.checked = 1;
        const bool same = MachMatOps::DsEq(out, rec.ds) && MachMatOps::RsEq(mRs, predicted);
        o.mismatch = same ? 0u : 1u;
        }();
    }
    if (out.comb != nullptr) {
        MachMatOps::Bank(*this, rec, out, probe);
        o.banked = 1;
    }
    return true;
}

/* ---- [native] track_mattab: the retained course's probe from a course material table (C5 Stage 3d)
 *
 * The retained course resolves each material segment's state by running SpTri on the segment's
 * first surviving triangle and discarding what it appended (gdx3ds_track_retained.cpp ProbeState).
 * For a triangle the caller has proved reaches SpTri's state block -- no vertex outside a clip
 * plane (so no CPU clipper), not dropped by the reject box, the trivial reject or the backface rule,
 * nothing pending -- that probe does exactly four things:
 *   1. ResolveDrawState: the machines' MachMatRec argument applies verbatim (the same key, the
 *      same apply half, the same import-hit replay and its validity proof, gdx_tri.cpp above);
 *   2. opens the packed batch (VboPackBegin: a pure read of the backend state 1. left, no pool
 *      advance);
 *   3. latches the batch aux from its vertex 0 (TrackMatOps::LatchAux: SpTri's gdxInput for
 *      v_arr[0], verbatim; the only vertex inputs are its colour and w);
 *   4. notes the fog parameters,
 * then writes three packed vertices past the pool cursor that PopAppended forgets. A hit does 1-4
 * from the record and the caller's copy of vertex 0 and skips the rest (and the three vertex
 * T&Ls the probe needed). Verify mode (2) always runs the stock probe and compares: the DrawState,
 * the post-resolve mRs (MachMatOps' check), and the aux the hit would have latched from the
 * caller's vertex against the probe's own (which proves the caller's vertex synthesis too). */
struct TrackMatPend {
    bool armed;
    bool check;
    bool predDrop; /* the geometry says the probe's triangle is dropped before the state block */
    int bucket;
    int way;
    uint8_t v;
    uint8_t liveMask;
    LoadedVertex v0; /* the vertex 0 the geometry says the state block latches from */
    RenderState predicted;
    MachMatOps::ImportProbe probe;
};
static TrackMatPend sTrkPend = {}; /* drawing thread only */

struct TrackMatOps {
    static constexpr int kWaysMax = 8;

    /* SpTri's backface rule, verbatim. */
    static bool Culled(const Interp& I, const LoadedVertex* v1, const LoadedVertex* v2, const LoadedVertex* v3) {
        if ((I.mRsp.geometry_mode & GM_CULL_BOTH) == 0) {
            return false;
        }
        const float dx1 = v1->x / (v1->w) - v2->x / (v2->w);
        const float dy1 = v1->y / (v1->w) - v2->y / (v2->w);
        const float dx2 = v3->x / (v3->w) - v2->x / (v2->w);
        const float dy2 = v3->y / (v3->w) - v2->y / (v2->w);
        float cross = dx1 * dy2 - dy1 * dx2;
        if ((v1->w < 0) ^ (v2->w < 0) ^ (v3->w < 0)) {
            cross = -cross;
        }
        if ((I.mRsp.extra_geometry_mode & 0x00000001u) != 0) {
            cross = -cross;
        }
        const uint32_t cull_type = I.mRsp.geometry_mode & GM_CULL_BOTH;
        if (cull_type == GM_CULL_FRONT) {
            return cross <= 0;
        }
        if (cull_type == GM_CULL_BACK) {
            return cross >= 0;
        }
        return true;
    }

    /* The geometry half of SpTri for a top-level triangle that passed the reject-variant screen:
     * the CPU clipper and its fan (the same functions and expressions, cap and prepass included),
     * the trivial reject and the backface rule. true = some triangle reaches the state block, and
     * *v0 is the vertex 0 it latches the batch aux from; false = SpTri draws nothing and binds
     * nothing. */
    static bool StateVertex(const Interp& I, const LoadedVertex tri[3], LoadedVertex* v0) {
        const LoadedVertex* v1 = &tri[0];
        const LoadedVertex* v2 = &tri[1];
        const LoadedVertex* v3 = &tri[2];
        const bool requiresClipping = gdx_ninterp_cpuclip() && ((v1->clip_out | v2->clip_out | v3->clip_out) != 0);
        if (!requiresClipping) {
            if (v1->clip_rej & v2->clip_rej & v3->clip_rej) {
                return false;
            }
            if (Culled(I, v1, v2, v3)) {
                return false;
            }
            *v0 = *v1;
            return true;
        }
        /* hotcold: the clipper half runs out of line (noinline, not cold: a real per-frame path). */
        return [&I, v1, v2, v3, v0]() __attribute__((noinline)) -> bool {
        LoadedVertex polygonA[16];
        polygonA[0] = *v1;
        polygonA[1] = *v2;
        polygonA[2] = *v3;
        LoadedVertex polygonB[16];
        LoadedVertex* input = polygonA;
        LoadedVertex* output = polygonB;
        size_t inputCount = 3;
        const size_t clipCap = I.mClipPolyMax;
        uint32_t firstPlane = 0;
        if (I.mO3dsFast != 0) {
            bool passed = false;
            firstPlane = ClipPrepass(*v1, *v2, *v3, &inputCount, &passed);
        }
        for (uint32_t plane = firstPlane; plane < 7 && inputCount >= 3; ++plane) {
            size_t outputCount = 0;
            bool truncated = false;
            if (I.mO3dsFast != 0) {
                ClipPlaneFast(input, inputCount, output, clipCap, plane, &outputCount, &truncated);
            }
            for (size_t i = 0; I.mO3dsFast == 0 && i < inputCount; ++i) {
                const LoadedVertex& current = input[i];
                const LoadedVertex& next = input[(i + 1) % inputCount];
                const float currentDistance = ClipDistance(current, plane);
                const float nextDistance = ClipDistance(next, plane);
                const bool currentInside = currentDistance >= 0.0f;
                const bool nextInside = nextDistance >= 0.0f;
                if (currentInside && outputCount < clipCap) {
                    output[outputCount++] = current;
                }
                if (currentInside != nextInside && outputCount < clipCap) {
                    const float denominator = currentDistance - nextDistance;
                    float t = denominator != 0.0f ? currentDistance / denominator : 0.0f;
                    if (t < 0.0f) t = 0.0f;
                    if (t > 1.0f) t = 1.0f;
                    output[outputCount++] = Interpolate(current, next, t);
                }
            }
            inputCount = outputCount;
            LoadedVertex* tmp = input;
            input = output;
            output = tmp;
        }
        if (inputCount < 3) {
            return false;
        }
        for (size_t i = 1; i + 1 < inputCount; ++i) {
            LoadedVertex cv[3] = { input[0], input[i], input[i + 1] };
            for (size_t j = 0; j < 3; ++j) {
                cv[j].w = cv[j].w > kMinClipW ? cv[j].w : kMinClipW;
                UpdateClipFlags(cv[j]);
            }
            /* the fan's SpTri: no reject screen and no clipper at depth 1 */
            if (cv[0].clip_rej & cv[1].clip_rej & cv[2].clip_rej) {
                continue;
            }
            if (Culled(I, &cv[0], &cv[1], &cv[2])) {
                continue;
            }
            *v0 = cv[0];
            return true;
        }
        return false;
        }();
    }

    /* SpTri's gdxInput lambda for vertex 0 (there v_arr[0] and v1 are both vertex 0), verbatim. */
    static Rgba Input(const Interp& I, const Combiner* comb, const LoadedVertex& v0, int j, int k, bool* isShade) {
        Rgba tmp = { 0, 0, 0, 0 };
        *isShade = false;
        switch (comb->shader_input_mapping[k][j]) {
            case CCMUX_PRIMITIVE: tmp = I.mRdp.prim_color; break;
            case CCMUX_SHADE: tmp = v0.color; *isShade = true; break;
            case CCMUX_SHADE_ALPHA: tmp.r = tmp.g = tmp.b = tmp.a = v0.color.a; break;
            case CCMUX_ENVIRONMENT: tmp = I.mRdp.env_color; break;
            case CCMUX_PRIMITIVE_ALPHA: tmp.r = tmp.g = tmp.b = I.mRdp.prim_color.a; break;
            case CCMUX_ENV_ALPHA: tmp.r = tmp.g = tmp.b = I.mRdp.env_color.a; break;
            case CCMUX_PRIM_LOD_FRAC: tmp.r = tmp.g = tmp.b = I.mRdp.prim_lod_fraction; break;
            case CCMUX_LOD_FRACTION: {
                if (I.mRdp.other_mode_l & (1u << MDSFT_TEXTLOD)) {
                    float distance_frac = (v0.w - 3000.0f) / 3000.0f;
                    if (distance_frac < 0.0f) distance_frac = 0.0f;
                    if (distance_frac > 1.0f) distance_frac = 1.0f;
                    tmp.r = tmp.g = tmp.b = tmp.a = static_cast<uint8_t>(distance_frac * 255.0f);
                } else {
                    tmp.r = tmp.g = tmp.b = tmp.a = 255;
                }
                break;
            }
            case CCMUX_KEY_CENTER: tmp = I.mRdp.key_center; break;
            case CCMUX_KEY_SCALE: tmp = I.mRdp.key_scale; break;
            case CCMUX_CONVERT_K4: tmp.r = tmp.g = tmp.b = static_cast<uint8_t>(I.mRdp.convert_k[4]); break;
            case CCMUX_CONVERT_K5: tmp.r = tmp.g = tmp.b = static_cast<uint8_t>(I.mRdp.convert_k[5]); break;
            default: break;
        }
        if (k == 1 && comb->shader_input_mapping[k][j] == ACMUX_PRIM_LOD_FRAC) {
            tmp = Rgba{ 0, 0, 0, I.mRdp.prim_lod_fraction };
            *isShade = false;
        }
        return tmp;
    }

    /* SpTri's batch-open aux latch (mBufNumTris == 0), verbatim, for vertex 0 = v0. */
    static void LatchAux(const Interp& I, const DrawState& ds, const LoadedVertex& v0, Gdx3dsVboPackAux& aux) {
        aux.numInputs = static_cast<uint8_t>(ds.numInputs < 7 ? ds.numInputs : 7);
        for (int j = 0; j < aux.numInputs; j++) {
            bool shade = false;
            const Rgba c = Input(I, ds.comb, v0, j, 0, &shade);
            aux.inputRgba[j][0] = c.r / 255.0f;
            aux.inputRgba[j][1] = c.g / 255.0f;
            aux.inputRgba[j][2] = c.b / 255.0f;
            if (ds.use_alpha) {
                bool alphaShade = false;
                const Rgba a = Input(I, ds.comb, v0, j, 1, &alphaShade);
                aux.inputRgba[j][3] = (ds.use_fog && !ds.use_blend_color && alphaShade) ? 1.0f : a.a / 255.0f;
            } else {
                aux.inputRgba[j][3] = 1.0f;
            }
        }
        aux.hasFog = ds.use_fog ? 1 : 0;
        if (ds.use_fog) {
            const Rgba& fogSrc = ds.use_blend_color ? I.mRdp.blend_color : I.mRdp.fog_color;
            aux.fogRgb[0] = fogSrc.r / 255.0f;
            aux.fogRgb[1] = fogSrc.g / 255.0f;
            aux.fogRgb[2] = fogSrc.b / 255.0f;
            aux.fogFactor0 = ds.use_blend_color ? I.mRdp.fog_color.a / 255.0f : v0.color.a / 255.0f;
        } else {
            aux.fogRgb[0] = aux.fogRgb[1] = aux.fogRgb[2] = 0.0f;
            aux.fogFactor0 = 0.0f;
        }
    }

    static bool AuxEq(const Gdx3dsVboPackAux& a, const Gdx3dsVboPackAux& b) {
        if (a.numInputs != b.numInputs || a.hasFog != b.hasFog ||
            memcmp(a.inputRgba, b.inputRgba, sizeof(a.inputRgba[0]) * a.numInputs) != 0 ||
            memcmp(a.fogRgb, b.fogRgb, sizeof(a.fogRgb)) != 0) {
            return false;
        }
        return memcmp(&a.fogFactor0, &b.fogFactor0, sizeof(float)) == 0;
    }

    /* Which bucket of the table the live key falls in (FNV over the key's leading registers and the
     * texel0 span source; only a spreading choice, the proof is KeyDiff). */
    static int Bucket(const Interp& I, int nBuckets) {
        uint32_t h = 2166136261u;
        const uint32_t words[7] = { static_cast<uint32_t>(I.mRdp.combine_mode),
                                    static_cast<uint32_t>(I.mRdp.combine_mode >> 32),
                                    I.mRdp.other_mode_l,
                                    I.mRdp.other_mode_h,
                                    I.mRsp.geometry_mode,
                                    I.mRdp.first_tile_index,
                                    static_cast<uint32_t>(reinterpret_cast<uintptr_t>(
                                        I.LoadedTexAt(I.mRdp.texture_tile[I.mRdp.first_tile_index & 7u].tmem_index).addr)) };
        for (uint32_t w : words) {
            h = (h ^ w) * 16777619u;
        }
        return static_cast<int>(h % static_cast<uint32_t>(nBuckets));
    }

    static int Gather(MachMatRec* table, int bucket, int ways, MachMatRec** recs) {
        const int n = ways < kWaysMax ? ways : kWaysMax;
        for (int w = 0; w < n; w++) {
            recs[w] = &table[bucket * ways + w];
        }
        return n;
    }

    /* SpTri's batch open for its first triangle (mBufNumTris == 0), then the aux latch and the fog
     * note of its emission. 1 = open (the probe's state is complete), 2 = the backend refused (the
     * probe would have dropped the triangle), 3 = the pool fits under two triangles (the probe's
     * own append would flush: the caller runs it). */
    static int OpenBatch(Interp& I, const DrawState& ds, const LoadedVertex& v0) {
        uint32_t capTris = 0;
        I.mStats.triBegin++;
        I.mPackDst = I.mBackend->VboPackBegin(&I.mPackParams, &capTris);
        I.mPackBatch = I.mPackDst != nullptr;
        I.mPackComb = ds.comb;
        if (!I.mPackBatch) {
            I.mStats.packRefused++;
            I.mPackCapTris = static_cast<size_t>(kMaxTriBuffer);
            return 2;
        }
        I.mPackCapTris = capTris < static_cast<uint32_t>(kMaxTriBuffer) ? static_cast<size_t>(capTris)
                                                                        : static_cast<size_t>(kMaxTriBuffer);
        if (I.mPackCapTris < static_cast<size_t>(kMaxTriBuffer)) {
            I.mStats.packSplit++;
        }
        if (I.mPackCapTris < 2) {
            return 3;
        }
        LatchAux(I, ds, v0, I.mPackAux);
        if (ds.use_fog) {
            if (ds.use_blend_color) {
                I.mBackend->FogNoteParams(0, 0, 0);
            } else {
                I.mBackend->FogNoteParams(I.mRsp.fog_mul, I.mRsp.fog_offset, 1);
            }
        }
        I.mStats.trisEmitted++;
        I.mStats.triEmitAll++;
        return 1;
    }
};

int Interp::TrackMatBefore(MachMatRec* table, int nBuckets, int ways, int modeArg, bool serve, const LoadedVertex* tri,
                           MachMatOutcome& o) {
    memset(&o, 0, sizeof(o));
    sTrkPend.armed = false;
    if (table == nullptr || nBuckets <= 0 || ways <= 0 || tri == nullptr) {
        return 0;
    }
    const bool servable = serve && !mHudRectFast && !mVerifyActive && mBufNumTris == 0;
    LoadedVertex v0;
    memset(&v0, 0, sizeof(v0));
    const bool reaches = TrackMatOps::StateVertex(*this, tri, &v0);
    if ((modeArg & 0x0F) == 1 && servable && !reaches) {
        o.result = MM_HIT;
        return 4; /* SpTri would draw nothing and bind nothing: no record needed */
    }
    const bool live = (modeArg & 0x10) != 0;
    const int mode = modeArg & 0x0F;
    MachMatRec* recs[TrackMatOps::kWaysMax];
    const int bucket = TrackMatOps::Bucket(*this, nBuckets);
    const int n = TrackMatOps::Gather(table, bucket, ways, recs);
    uint16_t keyBits = 0;
    const int way = MachMatOps::FindWay(*this, recs, n, &keyBits);
    bool anyValid = false;
    for (int w = 0; w < n; w++) {
        anyValid = anyValid || recs[w]->valid != 0;
    }
    uint8_t liveMask = 0;
    MachMatResult v = anyValid ? MM_MISS_KEY : MM_MISS_EMPTY;
    if (way >= 0) {
        v = MachMatOps::Validate(*this, *recs[way], live, &liveMask);
    } else {
        o.keyBits = keyBits;
    }
    if (mode == 1 && v == MM_HIT && servable) {
        MachMatRec& rec = *recs[way];
        rec.useSeq = ++sMachMatSeq;
        o.result = MM_HIT;
        o.binds = MachMatOps::Apply(*this, rec, liveMask);
        o.live = liveMask;
        mTriMemo = rec.ds;
        mTriMemoValid = true;
        if (mTriMemoOn != 0) {
            TriMemoBank(false, rec.ds);
        }
        mStats.triCalls++;
        mStats.triTopCalls++;
        mStats.tris++;
        return TrackMatOps::OpenBatch(*this, rec.ds, v0);
    }
    o.result = mode == 1 ? static_cast<uint8_t>(v) : static_cast<uint8_t>(MM_FULL);
    sTrkPend.armed = true;
    sTrkPend.bucket = bucket;
    sTrkPend.way = way;
    sTrkPend.v = static_cast<uint8_t>(v);
    sTrkPend.liveMask = liveMask;
    sTrkPend.predDrop = !reaches;
    sTrkPend.v0 = v0;
    sTrkPend.check = mode == 2 && servable && (v == MM_HIT || !reaches);
    if (GDX_UNLIKELY(sTrkPend.check && reaches)) {
        MachMatOps::Predict(*this, *recs[way], liveMask, sTrkPend.predicted);
    }
    sTrkPend.probe = MachMatOps::ProbeBefore(*this);
    return 0;
}

void Interp::TrackMatAfter(MachMatRec* table, int nBuckets, int ways, int modeArg, int probe, MachMatOutcome& o) {
    (void)nBuckets;
    (void)modeArg;
    if (!sTrkPend.armed) {
        return;
    }
    sTrkPend.armed = false;
    /* probe: 0 = the state resolved (appended, forgotten), 1 = dropped before the state block,
     * 2 = drawn by the interpreter (a refused batch or a flush: not compared) */
    if (sTrkPend.check && probe != 2 && (probe == 1) != sTrkPend.predDrop) {
        o.checked = 1;
        o.mismatch = 16; /* the geometry's drop decision disagreed with SpTri's */
        return;
    }
    if (GDX_UNLIKELY(sTrkPend.check && sTrkPend.predDrop)) {
        o.checked = 1;
        return;
    }
    const bool resolved = probe == 0;
    if (!resolved || !mTriMemoValid || mTriMemo.comb == nullptr || mHudRectFast) {
        return;
    }
    MachMatRec* recs[TrackMatOps::kWaysMax];
    const int n = TrackMatOps::Gather(table, sTrkPend.bucket, ways, recs);
    MachMatRec& rec = *recs[sTrkPend.way >= 0 ? sTrkPend.way : MachMatOps::VictimWay(recs, n)];
    const LoadedVertex* v0 = &sTrkPend.v0;
    if (GDX_UNLIKELY(sTrkPend.check && sTrkPend.v == MM_HIT)) {
        /* hotcold: verify mode (2) only, out of line and cold. */
        [this, &rec, &o, v0]() GDX_COLD {
        MachMatOps::PredictLive(*this, rec, sTrkPend.liveMask, sTrkPend.predicted);
        o.live = sTrkPend.liveMask;
        o.checked = 1;
        uint8_t bits = 0;
        bits |= MachMatOps::DsEq(mTriMemo, rec.ds) ? 0u : 1u;
        bits |= MachMatOps::RsEq(mRs, sTrkPend.predicted) ? 0u : 2u;
        Gdx3dsVboPackAux aux = mPackAux;
        TrackMatOps::LatchAux(*this, rec.ds, *v0, aux);
        bits |= TrackMatOps::AuxEq(aux, mPackAux) ? 0u : 4u;
        bits |= mPackBatch ? 0u : 8u;
        o.mismatch = bits;
        }();
    }
    const uint8_t v = sTrkPend.v;
    const bool invalidating = v == MM_MISS_KEY || v == MM_MISS_TEX || v == MM_MISS_EPOCH || v == MM_MISS_SMP;
    o.wasValid = (rec.valid != 0 && invalidating) ? 1u : 0u;
    rec.useSeq = ++sMachMatSeq;
    MachMatOps::Bank(*this, rec, mTriMemo, sTrkPend.probe);
    o.banked = 1;
}

#ifdef EXPANSION_KIT
/* [native] ek_editmesh (gdx_ekpreview.cpp): a served element opens its batch exactly as SpTri does and
 * then latches the aux from the recorded vertex 0 -- TrackMatOps' verbatim copy of SpTri's latch. */
void Interp::EkLatchAux(const DrawState& ds, const LoadedVertex& v0) {
    TrackMatOps::LatchAux(*this, ds, v0, mPackAux);
}
#endif

/* [native] hudsky_emit: the sky's compact triangle path (hudsky). */
#include "gdx_hudsky_tri.inc"

} // namespace gdxn
