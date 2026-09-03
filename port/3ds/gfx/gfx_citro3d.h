/* port/3ds/gfx/gfx_citro3d.h — stream A: citro3d backend for the LUS Fast3D
 * interpreter (contract: port/3ds/include/gdx3ds_gfx.h, frozen).
 *
 * Subclasses Fast::GfxRenderingAPI (libultraship/include/fast/backends/
 * gfx_rendering_api.h). The interpreter hands us fully transformed clip-space
 * triangles in a variable-stride float VBO; we repack them into a fixed
 * pos4/uv2/rgba4 layout in a linearAlloc pool and draw with a single PICA
 * pass-through vertex shader (shader.v.pica). The N64 combiner is mapped onto
 * PICA TexEnv stage 0; this shift covers the basic single-cycle cases
 * (REPLACE / MODULATE / INTERPOLATE / MULTIPLY_ADD) and logs every shader-ID
 * pair it cannot map (full mapping waits on stream F's combiner census).
 *
 * Reference architecture: mkst/sm64-port `3ds-port` gfx_citro3d.c — but this
 * interface is richer (framebuffer objects, pixel-depth queries, 2x64-bit
 * shader IDs).
 *
 * Prereq at runtime: the OS layer (stream B) has already called gfxInitDefault().
 * Stereoscopic 3D: foundation plumbed (gdx3ds_stereo.cpp — dual targets, slider
 * gate, per-eye draw loop with a placeholder eye transform); default off via
 * config, see port/3ds/gfx/STEREO.md for the remaining stereo-shift work.
 */
#pragma once

#include <cstdint>

#include <map>
#include <set>
#include <utility>
#include <unordered_map>
#include <vector>

#include <3ds.h>
#include <citro3d.h>

#include "fast/interpreter.h" // CCFeatures, gfx_cc_get_features(), SHADER_* enums
#include "fast/backends/gfx_rendering_api.h"

#include "gdx3ds_gfx.h" // the frozen factory contract this class fulfils
#include "gdx3ds_vbopack.h" // [triloop] direct PICA-layout emission contract

namespace Fast {

/* One PICA TexEnv stage of a mapped combiner. Each stage owns its own constant
 * register: the rgb part comes from constRgbInput (a 1-based SHADER_INPUT_n,
 * refreshed from vertex 0 per draw — valid for draw-constant prim/env) or
 * constRgbFixed (a formula-demanded literal 0/1); the alpha part likewise. */
struct TexEnvStageC3D {
    GPU_COMBINEFUNC rgbFunc = GPU_REPLACE;
    GPU_COMBINEFUNC alphaFunc = GPU_REPLACE;
    GPU_TEVSRC rgbSrc[3] = { GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR };
    GPU_TEVOP_RGB rgbOp[3] = { GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR };
    GPU_TEVSRC alphaSrc[3] = { GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR };
    GPU_TEVOP_A alphaOp[3] = { GPU_TEVOP_A_SRC_ALPHA, GPU_TEVOP_A_SRC_ALPHA, GPU_TEVOP_A_SRC_ALPHA };
    int constRgbInput = 0;
    int constAlphaInput = 0;
    int constRgbFixed = -1;
    int constAlphaFixed = -1;
    /* Baked from the fixed parts at map time; recomposed per draw when either
     * const*Input is set. */
    uint32_t constColor = 0xFFFFFFFF;
};

constexpr int kMaxTexEnvStages = 4; // census worst case is 3; PICA has 6

/* Backend-private ShaderProgram payload; the interpreter only holds opaque
 * ShaderProgram* handles, each backend defines its own (see gfx_direct3d11.cpp). */
struct ShaderProgramC3D {
    uint64_t id0 = 0;
    uint64_t id1 = 0;
    CCFeatures cc = {};

    /* Interpreter-side vertex layout (floats), derived from cc. */
    uint8_t inputStride = 4;
    int uvOffset[2] = { -1, -1 };
    int fogOffset = -1;
    int grayscaleOffset = -1;
    int inputOffset[7] = { -1, -1, -1, -1, -1, -1, -1 };
    int inputSize = 3; // 3 (rgb) or 4 (rgba) floats per combiner input

    /* Which combiner inputs (1-based SHADER_INPUT_n) ride the vertex colour
     * attribute (rgb and alpha routed independently — the interpreter packs each
     * input stream as merged RGBA). Everything else rides per-stage constants. */
    int vtxColorInput = 0;
    int vtxAlphaInput = 0;

    /* TexEnv stage chain (valid when mapped; fallback fills stage 0 otherwise). */
    bool mapped = false;
    int numStages = 1;
    TexEnvStageC3D stages[kMaxTexEnvStages];

    /* Per-vertex fog blend (see UpdateFogState). F-Zero X compresses the whole
     * track into clip-depth [0.95, 1.0], so the depth-indexed PICA fog unit cannot
     * resolve the fade. Instead the interpreter's baked per-vertex fog factor
     * (bufVbo[fogOffset+3], N64 RSP-computed) rides GPU_PRIMARY_COLOR's alpha and a
     * dedicated final TexEnv stage lerps fogColor into the fragment by it:
     *   out.rgb = INTERPOLATE(fogColor, prevColor, primaryColor.a).
     * Enabled only for fog draws whose combiner alpha channel is opaque (so the
     * primary-colour alpha slot is genuinely free); flagged false otherwise, where
     * the code keeps the legacy depth-LUT fog unit as a fallback. */
    bool fogBlendStage = false;
    int fogBlendStageIndex = -1; // which stages[] slot carries the blend
};

/* PICA GPU_INTERPOLATE evaluates src0*src2 + src1*(1 - src2) (3dbrew GPU/Internal
 * Registers). Fog blend out = fogColor*f + fragColor*(1-f) maps to
 * src0=fogColor(const), src1=fragColor(previous), src2=primaryColor.alpha=f. */

class GfxRenderingAPIC3D final : public GfxRenderingAPI {
  public:
    GfxRenderingAPIC3D() = default;
    ~GfxRenderingAPIC3D() override = default;

    const char* GetName() override;
    int GetMaxTextureSize() override;
    GfxClipParameters GetClipParameters() override;

    void UnloadShader(ShaderProgram* oldPrg) override;
    void LoadShader(ShaderProgram* newPrg) override;
    void ClearShaderCache() override;
    ShaderProgram* CreateAndLoadNewShader(uint64_t shaderId0, uint64_t shaderId1) override;
    ShaderProgram* LookupShader(uint64_t shaderId0, uint64_t shaderId1) override;
    void ShaderGetInfo(ShaderProgram* prg, uint8_t* numInputs, bool usedTextures[2]) override;

    uint32_t NewTexture() override;
    void SelectTexture(int tile, uint32_t textureId) override;
    void UploadTexture(const uint8_t* rgba32Buf, uint32_t width, uint32_t height) override;
    void SetSamplerParameters(int sampler, bool linearFilter, uint32_t cms, uint32_t cmt) override;
    void DeleteTexture(uint32_t texId) override;
    void SetTextureFilter(FilteringMode mode) override;
    FilteringMode GetTextureFilter() override;

    void SetDepthTestAndMask(bool depthTest, bool zUpd) override;
    void SetZmodeDecal(bool decal) override;
    void SetViewport(int x, int y, int width, int height) override;
    void SetScissor(int x, int y, int width, int height) override;
    void SetUseAlpha(bool useAlpha) override;

    void DrawTriangles(float bufVbo[], size_t bufVboLen, size_t bufVboNumTris) override;

    void Init() override;
    void OnResize() override;
    void StartFrame() override;
    void EndFrame() override;
    void FinishRender() override;

    int CreateFramebuffer() override;
    void UpdateFramebufferParameters(int fbId, uint32_t width, uint32_t height, uint32_t msaaLevel,
                                     bool openglInvertY, bool renderTarget, bool hasDepthBuffer,
                                     bool canExtractDepth) override;
    void StartDrawToFramebuffer(int fbId, float noiseScale) override;
    void CopyFramebuffer(int fbDstId, int fbSrcId, int srcX0, int srcY0, int srcX1, int srcY1, int dstX0, int dstY0,
                         int dstX1, int dstY1) override;
    void ClearFramebuffer(bool color, bool depth) override;
    void ReadFramebufferToCPU(int fbId, uint32_t width, uint32_t height, uint16_t* rgba16Buf) override;
    void ResolveMSAAColorBuffer(int fbIdTarget, int fbIdSrc) override;
    std::unordered_map<std::pair<float, float>, uint16_t, hash_pair_ff>
    GetPixelDepth(int fbId, const std::set<std::pair<float, float>>& coordinates) override;
    void* GetFramebufferTextureId(int fbId) override;
    void SelectTextureFb(int fbId) override;

    void SetSrgbMode() override;
    ImTextureID GetTextureById(int id) override;
    void SetCurrentPrimDepth(float depth) override;
    void SetCurrentAlphaCompareThreshold(float threshold) override;

    /* [triloop] direct PICA-layout emission (gdx3ds_vbopack.h). VboPackBegin
     * hands the interpreter the batch's pack parameters + a linear-pool write
     * cursor (NULL = refuse, legacy path); DrawPackedBatch submits the packed
     * range through DrawTriangles with the repack skipped. */
    float* VboPackBegin(Gdx3dsVboPackParams* params);
    void DrawPackedBatch(size_t numTris, const Gdx3dsVboPackAux* aux);

  private:
    struct TextureC3D {
        C3D_Tex tex = {};
        bool inited = false;
        uint32_t width = 0;   // original N64 dimensions
        uint32_t height = 0;
        uint32_t padWidth = 0; // pow2-padded PICA dimensions
        uint32_t padHeight = 0;
        float uScale = 1.0f; // width / padWidth — applied to UVs at repack time
        float vScale = 1.0f;
        bool linearFilter = true;
        uint32_t cms = 0;
        uint32_t cmt = 0;
        /* [trectbatch] VIEW into a shared atlas page (page >= 0): this id owns no C3D_Tex;
         * its content sits at (ax, ay) of mAtlasPages[page] with a 1-texel replicated gutter.
         * uScale/vScale = content / page extent, uOff/vOff = origin / page extent. */
        int16_t page = -1;
        uint16_t ax = 0;
        uint16_t ay = 0;
        float uOff = 0.0f;
        float vOff = 0.0f;
    };
    static bool TexReady(const TextureC3D& t) {
        return t.inited || t.page >= 0;
    }

    /* [trectbatch] HUD atlas page: shelf-packed RGBA8 page shared by many small clamp-addressed
     * rect textures so consecutive same-page rects draw as ONE batch. Cells are never freed
     * individually; a page resets when its last view id is recycled (refs == 0). */
    struct AtlasPageC3D {
        C3D_Tex tex = {};
        bool inited = false;
        uint32_t refs = 0;
        uint16_t shelfY = 0;
        uint16_t shelfH = 0;
        uint16_t cursorX = 0;
    };

    struct FramebufferC3D {
        C3D_RenderTarget* target = nullptr;
        C3D_Tex tex = {};
        bool texBacked = false; // true: render-to-texture; false: main screen target
        uint32_t width = 0;
        uint32_t height = 0;
    };

    void MapCombiner(ShaderProgramC3D& prg);

    /* [trectbatch] atlas internals */
    bool AtlasTryPlace(TextureC3D& t, const uint8_t* rgba32, uint32_t w, uint32_t h);
    void AtlasRelease(TextureC3D& t);
    void AtlasFillTexParams(int unit, Gdx3dsVboPackParams* params, bool skyClampFix) const;
    std::vector<AtlasPageC3D> mAtlasPages;
    bool mAtlasArm = false;
    unsigned long mAtlasPlaced = 0;
    unsigned long mAtlasFull = 0;
    unsigned long mAtlasResets = 0;

  public:
    /* [trectbatch] public atlas contract (C shims in gfx_citro3d.cpp, weak-referenced by the
     * patched interpreter): arm the NEXT UploadTexture as atlas-eligible, query a texture's
     * page (-1 = standalone), refresh the pack params' texture-dependent fields for the
     * currently bound ids (a same-page view merge mid-batch), and drain receipts. */
    void AtlasArm(bool on) {
        mAtlasArm = on;
    }
    int TexPage(uint32_t id) const {
        return id < mTextures.size() ? mTextures[id].page : -1;
    }
    void VboPackRefreshTextures(Gdx3dsVboPackParams* params);
    void AtlasStats(unsigned long out[5]) const;
    void ComputeVertexLayout(ShaderProgramC3D& prg);
    void ApplyShaderState(const ShaderProgramC3D& prg);
    void ApplyAlphaTest();
    void LogUnmappedCombiner(const ShaderProgramC3D& prg, const char* reason);
    /* Re-applies the per-stage TexEnv constant registers from vertex 0 of the batch,
     * value-dirty-tracked against mAppliedStageConst (a redundant C3D_TexEnvColor
     * write re-dirties the TexEnv and re-emits its GPU words every draw). Returns a
     * bitmask of the stages whose register actually moved this draw, and records the
     * first input-driven constant's packed value for the [prim] diag. */
    uint32_t RefreshStageConstants(const ShaderProgramC3D& prg, const float* vertex0);
    /* [triloop] aux-record variant for packed batches: identical value-dirty
     * logic, but the vertex-0 input values come from the interpreter's batch
     * record instead of the (nonexistent) variable-stride buffer. */
    uint32_t RefreshStageConstantsPacked(const ShaderProgramC3D& prg, const Gdx3dsVboPackAux& aux);
    void UpdateFogState(const ShaderProgramC3D& prg, const float* bufVbo, size_t numVerts, size_t inStride,
                        const Gdx3dsVboPackAux* packedAux);
    void DisableFog();
    void FlushPendingVbo();
    uint32_t CurrentTargetFbHeight() const;

    /* PICA state */
    DVLB_s* mVshDvlb = nullptr;
    shaderProgram_s mShaderProgram = {};
    int mProjectionUniformLoc = -1;
    C3D_Mtx mFixupMatrix = {};

    /* Fixed-size linearAlloc vertex pool, reset every StartFrame. */
    float* mVboPool = nullptr;
    size_t mVboPoolFloats = 0;
    size_t mVboOffsetFloats = 0;
    size_t mVboFlushedFloats = 0; // flushed-to-GPU watermark (FlushPendingVbo)
    bool mVboExhaustionLogged = false;

    std::vector<TextureC3D> mTextures;
    std::vector<FramebufferC3D> mFramebuffers;
    uint32_t mBoundTextureIds[2] = { 0, 0 };
    int mLastSelectedTile = 0;
    int mCurrentFramebuffer = 0;

    /* Shader variant cache. unordered_map is node-based, so ShaderProgramC3D*
     * handles held by the interpreter stay valid across growth; buckets are
     * pre-reserved for the census-predicted 64-variant population (~40 observed
     * per session, docs/research/combiner-census.md §5.2). */
    struct ShaderIdHash {
        size_t operator()(const std::pair<uint64_t, uint64_t>& k) const {
            return std::hash<uint64_t>()(k.first) ^ (std::hash<uint64_t>()(k.second) * 0x9E3779B97F4A7C15ull);
        }
    };
    std::unordered_map<std::pair<uint64_t, uint64_t>, ShaderProgramC3D, ShaderIdHash> mShaderPool;
    ShaderProgramC3D* mCurrentShader = nullptr;
    std::set<std::pair<uint64_t, uint64_t>> mLoggedUnmappedIds;
    std::set<std::pair<uint64_t, uint64_t>> mLoggedFogPathIds;

    /* PRIM-COLOR: mirror of each TexEnv stage's constant-colour register, kept in
     * lockstep by ApplyShaderState (unconditional writes) and RefreshStageConstants
     * (value-compared writes). Nothing else writes stage constants, so a matching
     * mirror value means the register already holds the batch's colour and the
     * write — and the TexEnv dirty re-emit it would queue — can be skipped.
     * 0xFFFFFFFF matches the C3D_TexEnvInit default. */
    uint32_t mAppliedStageConst[kMaxTexEnvStages] = { 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF };
    /* [prim] diag capture: the packed value of the first input-driven (prim/env)
     * stage constant of the last RefreshStageConstants call, if any. */
    uint32_t mLastInputConstPacked = 0;
    bool mLastDrawHadInputConst = false;

    /* PICA native fog: per-draw linear fit of the interpreter's per-vertex fog
     * factor against fragment depth, cached as fog LUTs keyed on the quantized
     * (slope, offset) pair. LUT storage must outlive the frame's command list. */
    struct FogKey {
        int32_t a = 0;
        int32_t b = 0;
        bool operator==(const FogKey& o) const {
            return a == o.a && b == o.b;
        }
    };
    struct FogKeyHash {
        size_t operator()(const FogKey& k) const {
            return std::hash<int64_t>()(((int64_t)k.a << 32) | (uint32_t)k.b);
        }
    };
    std::unordered_map<FogKey, C3D_FogLut, FogKeyHash> mFogLutCache;
    FogKey mBoundFogKey = { INT32_MIN, INT32_MIN };
    uint32_t mBoundFogColor = 0xFFFFFFFF;
    bool mFogEnabled = false;
    bool mFogLutCacheOverflowPending = false;

    FilteringMode mTextureFilter = FILTER_THREE_POINT;
    bool mUseAlpha = false;
    bool mDepthTest = false;
    bool mDepthMask = false;
    bool mZmodeDecal = false;
    bool mFrameActive = false;
    bool mInitialized = false;
    /* Stereo foundation (stream S, gdx3ds_stereo.cpp): latched from config at
     * Init(); the per-eye draw loop additionally gates on the slider per frame. */
    bool mStereoEnabled = false;
    uint32_t mFrameDrawsRightEye = 0; // right-eye re-emissions this frame

    /* Present-path telemetry (m1-boot-debug): per-frame draw/tri counters,
     * emitted on the svc debug channel every 64th EndFrame (~1 line/sec). */
    uint32_t mFrameIndex = 0;
    uint32_t mFrameDrawCalls = 0;
    uint32_t mFrameTris = 0;
    uint32_t mFrameFbBinds = 0;
    uint32_t mFrameDrawsScreenFb = 0;
    uint32_t mFrameDrawsTexFb = 0;
    uint32_t mFrameBindMisses = 0; // draws wanting a texel whose texture never uploaded
    uint32_t mTexUploads = 0;      // cumulative successful UploadTexture calls
    uint32_t mTexUploadFails = 0;  // cumulative UploadTexture early-outs

    /* [triloop] batch aux record of the packed draw currently threading through
     * DrawTriangles (non-null exactly inside DrawPackedBatch). */
    const Gdx3dsVboPackAux* mPackedAux = nullptr;
};

} // namespace Fast
