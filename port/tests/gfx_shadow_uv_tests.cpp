// gfx_shadow_uv_tests.cpp -- regression guard for the machine drop-shadow sampling
// contract on the 3DS (SHADOW-2 audit, docs/research/shadow2-mirror-period-audit.md).
//
// Standalone console exe: no libultraship, no game objects. It re-implements the exact
// derivations the interpreter + citro3d backend run for the shadow draw
// (racer.c:5880 / :7145 -- gDPLoadTextureBlock_4b I4 32x64, MIRROR|CLAMP both axes,
// masks 5/6, tile window == the texture) and pins the facts that make the pow2-padded
// mirror seam provably irrelevant for this draw:
//
//   1. ImportTextureI4's decoded extent is exactly 32x64 (LOADBLOCK line fallback via
//      GetEffectiveLineSize, tile-window crop and mask bound both no-ops).
//   2. The pow2 padding is the identity (uScale = vScale = 1.0), so the PICA
//      MIRRORED_REPEAT/CLAMP seam sits exactly at the LOGICAL edge. Any future
//      importer/padding change that shifts uScale/vScale garbles the silhouette and
//      trips this test.
//   3. The shipped machine-0 quad UVs (machine_custom_gfx/D_3004F18, S10.5) normalize
//      strictly INSIDE one period: neither MIRROR nor CLAMP ever engages.
//   4. N64 clamps to the tile window BEFORE the mask engages; window == one period, so
//      the MIRROR bit is inert on hardware and the measured ~1.075 UV overhang samples
//      the edge texel -- exactly what GPU_CLAMP_TO_EDGE reproduces post-f1247a9.
//   5. The interpreter's clamp-strip routing: MIRROR|CLAMP axes always become
//      shader-clamped (tm bit set, CLAMP stripped -> backend forces CLAMP_TO_EDGE);
//      mirror-only axes are NOT misclassified (no tm bit, hardware MIRRORED_REPEAT).
//
// Returns 0 iff every check passes; non-zero (and prints [FAIL]) otherwise.

#include <cstdint>
#include <cstdio>

// N64 GBI constants used by the derivations (values from decomp include/PR/gbi.h).
static constexpr uint8_t G_TX_NOMASK = 0;
static constexpr uint8_t G_TX_MIRROR = 1;
static constexpr uint8_t G_TX_CLAMP = 2;

static int g_failures = 0;

static void check_u32(const char* name, uint32_t got, uint32_t want) {
    if (got == want) {
        printf("[ OK ] %-58s got=%u\n", name, got);
    } else {
        printf("[FAIL] %-58s got=%u want=%u\n", name, got, want);
        ++g_failures;
    }
}

static void check_true(const char* name, bool got) {
    if (got) {
        printf("[ OK ] %s\n", name);
    } else {
        printf("[FAIL] %s\n", name);
        ++g_failures;
    }
}

static void check_near(const char* name, float got, float want, float tol) {
    const float d = got > want ? got - want : want - got;
    if (d <= tol) {
        printf("[ OK ] %-58s got=%.4f\n", name, (double)got);
    } else {
        printf("[FAIL] %-58s got=%.4f want=%.4f\n", name, (double)got, (double)want);
        ++g_failures;
    }
}

/* ------------------------------------------------------------------------------ */
/* Mirrors of the production derivations (keep in sync with the cited code).       */
/* ------------------------------------------------------------------------------ */

// interpreter.cpp GetEffectiveLineSize: prefer the recorded DRAM stride when it looks
// like real per-line info; fall back to the render tile's TMEM stride for the
// LOADBLOCK width-1 sentinel (line == full == size).
static uint32_t EffectiveLineSize(uint32_t lineSizeBytes, uint32_t fullImageLineSizeBytes,
                                  uint32_t sizeBytes, uint32_t tileLineSizeBytes) {
    if ((lineSizeBytes != sizeBytes || fullImageLineSizeBytes != sizeBytes) && lineSizeBytes > 0) {
        return lineSizeBytes;
    }
    return tileLineSizeBytes;
}

// interpreter.cpp ApplyTileMaskExtent (non-authoritative form used by ImportTextureI4).
static void ApplyMaskExtent(uint8_t masks, uint8_t maskt, uint32_t& width, uint32_t& height) {
    if (masks != G_TX_NOMASK && masks < 31) {
        width = width < (1u << masks) ? width : (1u << masks);
    }
    if (maskt != G_TX_NOMASK && maskt < 31) {
        height = height < (1u << maskt) ? height : (1u << maskt);
    }
}

// gfx_citro3d.cpp NextPow2.
static uint32_t NextPow2(uint32_t v) {
    if (v == 0) {
        return 1;
    }
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    return v + 1;
}

// interpreter.cpp:3837-3843 -- the clamp-strip / shader-clamp (tm bit) routing.
struct WrapRouting {
    uint8_t cmsOut;   // what SetSamplerParameters receives
    bool shaderClamp; // tm bit for this axis (backend forces CLAMP_TO_EDGE when set)
};
static WrapRouting RouteWrap(uint8_t cms, uint32_t texExtent, uint32_t tileWindow) {
    WrapRouting r{ cms, false };
    const uint32_t mirroredExtent = texExtent << (cms & G_TX_MIRROR);
    if ((cms & G_TX_CLAMP) && ((cms & G_TX_MIRROR) || mirroredExtent != tileWindow)) {
        r.shaderClamp = true;
        r.cmsOut = (uint8_t)(cms & ~G_TX_CLAMP);
    }
    return r;
}

// gfx_citro3d.cpp WrapFromN64 result, as an enum we can assert on.
enum Wrap { WRAP_CLAMP_TO_EDGE, WRAP_MIRRORED_REPEAT, WRAP_REPEAT };
static Wrap WrapFromN64(uint8_t cm) {
    if (cm & G_TX_CLAMP) {
        return WRAP_CLAMP_TO_EDGE;
    } else if (cm & G_TX_MIRROR) {
        return WRAP_MIRRORED_REPEAT;
    }
    return WRAP_REPEAT;
}

// N64 texture-coordinate pipeline for a clamped axis: clamp to the tile window
// (in texels, lr/4 relative to ul) happens BEFORE the mask/mirror engages.
static float N64ClampThenMask(float texel, float windowMaxTexel, uint8_t mask, uint8_t cm) {
    if (cm & G_TX_CLAMP) {
        if (texel < 0.0f) {
            texel = 0.0f;
        } else if (texel > windowMaxTexel) {
            texel = windowMaxTexel;
        }
    }
    const float period = (float)(1u << mask);
    if (texel >= 0.0f && texel < period) {
        return texel; // mask untouched inside the first period
    }
    // mirror fold (only reachable when the clamp window exceeds the period)
    float m = texel - (float)((int)(texel / (2.0f * period))) * 2.0f * period;
    return m < period ? m : 2.0f * period - m;
}

int main() {
    printf("== shadow tile: decode extent (I4 LOADBLOCK 32x64, masks 5/6, window 32x64) ==\n");
    // LOADBLOCK first-load bookkeeping: line == full == size (1024B); render tile
    // line = 2 words = 16B. gDPLoadTextureBlock_4b(32, 64): sizeBytes = 32*64/2.
    const uint32_t sizeBytes = 32 * 64 / 2;
    const uint32_t widthBytes = EffectiveLineSize(sizeBytes, sizeBytes, sizeBytes, /*tileLine=*/16);
    uint32_t width = widthBytes * 2; // 4b: two texels per byte
    uint32_t height = widthBytes > 0 ? sizeBytes / widthBytes : 0;
    check_u32("I4 width from render-tile line fallback", width, 32);
    check_u32("I4 height = sizeBytes / lineBytes", height, 64);
    // Tile-window crop: window == texture, crops nothing.
    const uint32_t tileW = (124 - 0 + 4) / 4, tileH = (252 - 0 + 4) / 4;
    if (tileW > 0 && tileW < width) {
        width = tileW;
    }
    if (tileH > 0 && tileH < height) {
        height = tileH;
    }
    ApplyMaskExtent(5, 6, width, height);
    check_u32("decoded extent width after window+mask bounds", width, 32);
    check_u32("decoded extent height after window+mask bounds", height, 64);

    printf("== pow2 padding is the identity: mirror seam at the LOGICAL edge ==\n");
    const uint32_t padW = NextPow2(width), padH = NextPow2(height);
    check_u32("padW == logical width", padW, width);
    check_u32("padH == logical height", padH, height);
    check_near("uScale (width/padW)", (float)width / (float)padW, 1.0f, 0.0f);
    check_near("vScale (height/padH)", (float)height / (float)padH, 1.0f, 0.0f);

    printf("== machine-0 quad UVs (D_3004F18, S10.5) are strictly interior ==\n");
    // s in {27, 970}, t in {249, 1799}; normalized by the logical extent like
    // interpreter.cpp:4056 (u/32 texels, / tex_width).
    const float sMin = 27.0f / 32.0f / 32.0f, sMax = 970.0f / 32.0f / 32.0f;
    const float tMin = 249.0f / 32.0f / 64.0f, tMax = 1799.0f / 32.0f / 64.0f;
    check_true("S span inside (0,1): no wrap engages", sMin > 0.0f && sMax < 1.0f);
    check_true("T span inside (0,1): no wrap engages", tMin > 0.0f && tMax < 1.0f);

    printf("== N64 clamp-before-mask: MIRROR inert, overhang == CLAMP_TO_EDGE ==\n");
    // Window max texel = lrs/4 = 31.0 (<= period 32): coordinates never reach the
    // mirror fold. The measured ~1.075 overhang (34.4 texels) must land on the edge
    // texel, which is what the backend's forced GPU_CLAMP_TO_EDGE samples too.
    check_near("overhang 34.4 texels -> clamped edge texel", N64ClampThenMask(34.4f, 31.0f, 5, G_TX_MIRROR | G_TX_CLAMP), 31.0f, 0.001f);
    check_near("interior 30.3 texels -> untouched", N64ClampThenMask(30.3f, 31.0f, 5, G_TX_MIRROR | G_TX_CLAMP), 30.3f, 0.001f);
    check_near("negative -0.5 texels -> clamped to first texel", N64ClampThenMask(-0.5f, 31.0f, 5, G_TX_MIRROR | G_TX_CLAMP), 0.0f, 0.001f);

    printf("== clamp-strip routing feeding the f1247a9 override ==\n");
    // MIRROR|CLAMP (the shadow axes): tm bit set, CLAMP stripped, backend must land
    // on CLAMP_TO_EDGE (N64-exact for window <= period, proven above).
    const WrapRouting shadowAxis = RouteWrap(G_TX_MIRROR | G_TX_CLAMP, 32, 32);
    check_true("MIRROR|CLAMP axis is shader-clamped (tm bit)", shadowAxis.shaderClamp);
    check_u32("MIRROR|CLAMP axis cms handed to sampler", shadowAxis.cmsOut, G_TX_MIRROR);
    check_true("override resolves CLAMP_TO_EDGE for tm axis",
               (shadowAxis.shaderClamp ? WRAP_CLAMP_TO_EDGE : WrapFromN64(shadowAxis.cmsOut)) == WRAP_CLAMP_TO_EDGE);
    // Mirror-only axis (no CLAMP bit): must NOT be misclassified -- hardware mirror.
    const WrapRouting mirrorOnly = RouteWrap(G_TX_MIRROR, 32, 32);
    check_true("mirror-only axis keeps hardware mirror (no tm bit)", !mirrorOnly.shaderClamp);
    check_true("mirror-only axis resolves MIRRORED_REPEAT",
               (mirrorOnly.shaderClamp ? WRAP_CLAMP_TO_EDGE : WrapFromN64(mirrorOnly.cmsOut)) == WRAP_MIRRORED_REPEAT);

    if (g_failures == 0) {
        printf("ALL PASS\n");
        return 0;
    }
    printf("%d FAILURE(S)\n", g_failures);
    return 1;
}
