/* port/3ds/gfx/gdx3ds_stereo.cpp — stream S: stereoscopic-3D foundation.
 * See gdx3ds_stereo.h for the design contract and STEREO.md for what remains.
 *
 * Verified references (docs/research/stereo3d-research.md):
 *  - devkitPro composite_scene: two 240x400 RGBA8+D24S8 targets, GFX_LEFT /
 *    GFX_RIGHT, gfxSet3D(true), right pass skipped at slider 0.
 *  - sm64-3ds (mkst / Wyatt-James): per-draw dual emission with a raw
 *    C3D_SetFrameBuf target switch (stock C3D_FrameDrawOn resets the viewport),
 *    identity-shear "stereoTilt" placeholder with iodZ=8 / iodW=16 — reproduced
 *    here as the clearly-marked placeholder, WITHOUT its CPU-side second vertex
 *    buffer copy (we re-issue the draw from the already-repacked VBO).
 */
#include "gdx3ds_stereo.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

// Render-thread identity + menu echo gate (both weak: harness builds link neither).
extern "C" int gdx3ds_rt_on_render_thread(void) __attribute__((weak));
extern "C" int gdx3ds_console_echo_enabled __attribute__((weak));

/* Config comes from stream B's OS library; the DL harness links the backend
 * without it, so resolve the getters weakly (same pattern as gGdxRaceActive in
 * gfx_citro3d.cpp). */
extern "C" int gdx3ds_config_get_bool(const char* section, const char* key, int fallback)
    __attribute__((weak));
extern "C" const char* gdx3ds_config_get_string(const char* section, const char* key,
                                                const char* fallback) __attribute__((weak));

namespace {

constexpr int kEyeWidth = 400; // game-space landscape; targets allocate portrait
constexpr int kEyeHeight = 240;

/* Stereo tuning (ini-overridable, see Init):
 *  - kDefaultIodPixels: on-screen parallax, in top-screen pixels, of a point at
 *    the FAR plane with the slider fully up. 12px ≈ 3% of the 400px screen —
 *    inside the comfort budget every 3DS launch title observed (Nintendo's
 *    guidance caps divergence well under the ~10mm physical eye separation the
 *    parallax barrier can address; 12px ≈ 2.3mm on the 77mm-wide panel).
 *  - kDefaultConvergence: NDC depth (z/w in [0,1], 0 = near plane, 1 = far
 *    plane) of the ZERO-PARALLAX plane. 0.25 puts the racing camera's subject
 *    (own craft, near track) at/near screen depth; everything beyond recedes
 *    into the screen, the closest sliver of track pops out slightly. */
constexpr float kDefaultIodPixels = 12.0f;
constexpr float kDefaultConvergence = 0.25f;

bool sEnabled = false;                    // config latch, fixed after Init()
bool sActive = false;                     // this frame: right-eye pass on
float sSlider = 0.0f;                     // osGet3DSliderState(), polled per frame
float sIodPx = kDefaultIodPixels;         // far-plane parallax in px (menu-visible)
float sSepNdc = kDefaultIodPixels / (kEyeWidth * 0.5f); // far-plane parallax, NDC-x units
float sConvergence = kDefaultConvergence; // zero-parallax plane, NDC depth
C3D_RenderTarget* sRightTarget = nullptr; // 240x400 RGBA8+D24S8, GFX_RIGHT
int sDrawClassOverride = -1;              // bridge tag channel; -1 = heuristic
bool sForceEnable = false;                // harness hook (no config lib linked)
bool sActivationLogged = false;

void StereoLog(const char* msg) {
    /* Console echo only from the main thread and only while the touch menu does not own the
     * console: the libctru console is not thread-safe and a newline on the bottom row scrolls
     * the menu's tab bar away. svc + (via the port logger) filelog always. */
    {
        const bool muted = (&gdx3ds_rt_on_render_thread != nullptr) && gdx3ds_rt_on_render_thread();
        const bool echoOff = (&gdx3ds_console_echo_enabled != nullptr) && !gdx3ds_console_echo_enabled;
        if (!muted && !echoOff) {
            std::fprintf(stderr, "[gdx3ds_stereo] %s\n", msg);
        }
    }
    svcOutputDebugString(msg, std::strlen(msg));
}

float ClampF(float v, float lo, float hi) {
    if (v < lo) {
        return lo;
    } else if (v > hi) {
        return hi;
    } else {
        return v;
    }
}

/* Float config through the string getter (the config lib has no float API);
 * falls back on missing lib, missing key, or unparsable value. */
float ConfigGetFloat(const char* section, const char* key, float fallback) {
    if (gdx3ds_config_get_string == nullptr) {
        return fallback;
    }
    const char* v = gdx3ds_config_get_string(section, key, nullptr);
    if (v == nullptr || *v == '\0') {
        return fallback;
    }
    char* end = nullptr;
    const float parsed = std::strtof(v, &end);
    if (end == v) {
        return fallback;
    }
    return parsed;
}

} // namespace

extern "C" void gdx3ds_stereo_set_draw_class(int stereoClass) {
    sDrawClassOverride = (stereoClass >= GDX3DS_STEREO_SCENE && stereoClass <= GDX3DS_STEREO_SKY_DEEP)
                             ? stereoClass
                             : -1;
}

extern "C" void gdx3ds_stereo_request_enable(void) {
    sForceEnable = true;
}

/* ---- MENU live tuning (3D tab) --------------------------------------------------
 * Main/render thread only (the menu tick shares the render thread). The setters are
 * meaningful only while stereo was enabled at Init; harmless otherwise. */
extern "C" int gdx3ds_stereo_runtime_enabled(void) {
    return sEnabled ? 1 : 0;
}

extern "C" int gdx3ds_stereo_get_iod_px(void) {
    return (int)(sIodPx + 0.5f);
}

extern "C" void gdx3ds_stereo_set_iod_px(int px) {
    sIodPx = ClampF((float)px, 0.0f, 40.0f);
    sSepNdc = sIodPx / (kEyeWidth * 0.5f);
}

extern "C" int gdx3ds_stereo_get_conv_x100(void) {
    return (int)(sConvergence * 100.0f + 0.5f);
}

extern "C" void gdx3ds_stereo_set_conv_x100(int centi) {
    sConvergence = ClampF((float)centi / 100.0f, 0.0f, 0.90f);
}

namespace Gdx3dsStereo {

bool Init() {
    int enabled = sForceEnable ? 1 : 0;
    if (!enabled && gdx3ds_config_get_bool != nullptr) {
        enabled = gdx3ds_config_get_bool("stereo", "enabled", 0);
    }
    if (!enabled) {
        sEnabled = false;
        return false; // zero further cost: no target, no gfxSet3D, no slider poll
    }

    sRightTarget = C3D_RenderTargetCreate(kEyeHeight, kEyeWidth, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    if (sRightTarget == nullptr) {
        StereoLog("[stereo] right-eye target allocation FAILED - staying mono");
        sEnabled = false;
        return false;
    }
    /* Same transfer flags as the main target (gfx_citro3d.cpp Init). */
    const uint32_t transferFlags =
        GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
        GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) |
        GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO);
    C3D_RenderTargetSetOutput(sRightTarget, GFX_TOP, GFX_RIGHT, transferFlags);
    gfxSet3D(true); // mutually exclusive with 800px wide mode (libctru contract)

    /* Tuning: [stereo] iod = far-plane parallax in top-screen pixels at full
     * slider (0..40, clamped for comfort); [stereo] convergence = NDC depth of
     * the zero-parallax plane (clamped away from 1.0 — the shift scales with
     * 1/(1-convergence)). */
    const float iodPx = ClampF(ConfigGetFloat("stereo", "iod", kDefaultIodPixels), 0.0f, 40.0f);
    sConvergence = ClampF(ConfigGetFloat("stereo", "convergence", kDefaultConvergence), 0.0f, 0.90f);
    sIodPx = iodPx;
    sSepNdc = iodPx / (kEyeWidth * 0.5f); // px -> NDC-x: 400px spans [-1,1]

    sEnabled = true;
    char msg[128];
    std::snprintf(msg, sizeof(msg),
                  "[stereo] enabled: GFX_RIGHT target bound, iod=%.1fpx convergence=%.2f",
                  (double)iodPx, (double)sConvergence);
    StereoLog(msg);
    return true;
}

bool Enabled() {
    return sEnabled;
}

bool FrameBegin(C3D_RenderTarget* mainTarget) {
    if (!sEnabled) {
        sActive = false;
        return false;
    }
    sSlider = osGet3DSliderState();
    const bool active = sSlider > 0.0f && sRightTarget != nullptr;
    if (active && !sActive) {
        if (!sActivationLogged) {
            sActivationLogged = true;
            char msg[96];
            std::snprintf(msg, sizeof(msg), "[stereo] right-eye pass ACTIVE (slider=%.2f)", (double)sSlider);
            StereoLog(msg);
        }
    }
    sActive = active;
    if (sActive) {
        /* Queue the right target into this frame so C3D_FrameEnd runs its
         * display transfer, then re-bind the main (left) target. Both binds go
         * through stock C3D_FrameDrawOn: at frame start the viewport reset it
         * performs matches StartFrame's existing state exactly. */
        C3D_FrameDrawOn(sRightTarget);
        C3D_FrameDrawOn(mainTarget);
    }
    return sActive;
}

bool Active() {
    return sActive;
}

C3D_RenderTarget* RightTarget() {
    return sRightTarget;
}

void BindTargetRaw(C3D_RenderTarget* target) {
    /* sm64-3ds precedent: C3D_SetFrameBuf alone — no viewport/scissor reset, so
     * the interpreter's current rects stay bound for the second eye. */
    C3D_SetFrameBuf(&target->frameBuf);
}

void ClearRight(C3D_ClearBits flags, uint32_t color, uint32_t depth) {
    if (sActive && sRightTarget != nullptr) {
        C3D_RenderTargetClear(sRightTarget, flags, color, depth);
    }
}

void ComputeEyeMatrix(const C3D_Mtx* base, int eye, C3D_Mtx* out, int stereoClass) {
    *out = *base;
    if (eye == 0 || !sActive || stereoClass == GDX3DS_STEREO_UI_ZERO_PARALLAX) {
        return; // center matrix: draw lands at screen depth in both eyes
    }
    /* ------------------- Off-axis stereo, clip-space form -------------------
     * DERIVATION. Vertices arrive in the game's clip space (x,y,z,w), with
     * z ∈ [0,w] (interpreter contract: z_is_from_0_to_1), so NDC depth
     * d = z/w ∈ [0,1], 0 = near plane, 1 = far plane. For any perspective
     * projection, d is an AFFINE function of 1/z_view (d = A + B/z_view; the
     * constants come from the game's near/far, which we never need to know).
     *
     * Physically correct stereo = translate the camera by ±s/2 along view-x
     * and skew the frustum so both view axes cross at the convergence depth C
     * (the screen plane). The image shift that pair produces, in NDC-x:
     *     shift(z_view) = ±(f·s/2)·(1/C − 1/z_view)
     * — affine in 1/z_view, zero at z_view = C, saturating toward the eye
     * separation term at infinity. Because d is ALSO affine in 1/z_view, that
     * family is exactly the set of shifts affine in d:
     *     shift(d) = ±sep · (d − dc) / (1 − dc)
     * where dc = d(C) is the convergence plane in NDC depth and sep the
     * parallax at the far plane (d = 1). No approximation: this clip-space
     * skew IS an asymmetric-frustum stereo projection — the same family
     * citro3d's Mtx_PerspStereoTilt generates — applied post-hoc because the
     * game owns the projection matrix (vertices are CPU-pre-transformed).
     *
     * Multiply by w to stay in clip space:  x' = x ± k·(z − dc·w), with
     * k = sep·slider/(1 − dc). As a matrix S on the incoming clip vector,
     * S = I except S[x][z] = ±k, S[x][w] = ∓k·dc; we fold base·S directly:
     * every row picks up (±k, ∓k·dc) times its x-coefficient. (For the fixup
     * base only row 1 — the portrait-rotated horizontal axis, out.y = −x —
     * has a nonzero x-coefficient, the same row Mtx_PerspStereoTilt shears.)
     *
     * Sign: the game's clip +x is screen-right (the mono image is not
     * mirrored, so the fixup→viewport→scanout chain is net-identity on x).
     * For the RIGHT eye (+1), points beyond convergence (d > dc) shift
     * screen-right → uncrossed (positive) parallax, perceived behind the
     * screen; nearer points shift left and pop out. z and w are untouched, so
     * depth testing, the PICA fog unit (indexed on z/w), and the TexEnv fog
     * blend are bit-identical per eye by construction.
     * ------------------------------------------------------------------------ */
    const float sep = sSepNdc * sSlider * (eye < 0 ? -1.0f : 1.0f);
    if (stereoClass == GDX3DS_STEREO_SKY_DEEP) {
        /* Backgrounds/skybox class: pin to far-plane parallax regardless of
         * the (often near/ortho) depth they are drawn at, so they sit behind
         * all scene geometry: x' = x + sep·w (constant sep in NDC). */
        for (int i = 0; i < 4; i++) {
            out->r[i].w += sep * base->r[i].x;
        }
        return;
    }
    const float k = sep / (1.0f - sConvergence);
    for (int i = 0; i < 4; i++) {
        const float bx = base->r[i].x;
        out->r[i].z += k * bx;
        out->r[i].w -= k * sConvergence * bx;
    }
}

int ClassifyDraw(bool orthoDraw) {
    if (sDrawClassOverride >= 0) {
        return sDrawClassOverride;
    }
    /* Heuristic default (foundation only; real per-drawcall tagging is bridge
     * work): orthographic draws (clip w == 1) are HUD/menu content and belong
     * at screen depth. */
    return orthoDraw ? GDX3DS_STEREO_UI_ZERO_PARALLAX : GDX3DS_STEREO_SCENE;
}

} // namespace Gdx3dsStereo

/* ---------------------------------------------------------------------------------
 * Right-eye verification readback (harness evidence path). Mirrors the main
 * target branch of GfxRenderingAPIC3D::ReadFramebufferToCPU — same GX display
 * transfer (RGBA8 -> linear RGB5A1) and the same harness-verified un-rotation
 * (game-y along fb-x from 0, game-x along fb-y from 0).
 * --------------------------------------------------------------------------------- */
extern "C" int gdx3ds_stereo_read_right(uint16_t* rgba16Buf, uint32_t width, uint32_t height) {
    if (rgba16Buf == nullptr || width == 0 || height == 0) {
        return 1;
    }
    std::memset(rgba16Buf, 0, (size_t)width * height * sizeof(uint16_t));
    if (!sEnabled || sRightTarget == nullptr) {
        return 2;
    }
    u32* colorBuf = (u32*)sRightTarget->frameBuf.colorBuf;
    if (colorBuf == nullptr) {
        return 3;
    }
    const u32 fbW = (u32)kEyeHeight; // portrait: 240 wide ...
    const u32 fbH = (u32)kEyeWidth;  // ... x 400 tall
    const size_t linearBytes = (size_t)fbW * fbH * sizeof(u16);
    u16* linear = (u16*)linearAlloc(linearBytes);
    if (linear == nullptr) {
        return 4;
    }
    /* Same completion sentinel as ReadFramebufferToCPU: C3D_SyncDisplayTransfer does NOT wait
       while a C3D frame is active (see the TRANSITION-GLITCH note in gfx_citro3d.cpp), so poll
       the transfer's final words before consuming the buffer. */
    volatile u32* sentinel = (volatile u32*)((u8*)linear + linearBytes - 8);
    sentinel[0] = 0xDEADC0DEu;
    sentinel[1] = 0x5EA7BEEFu ^ 0x0F0F0F0Fu;
    GSPGPU_FlushDataCache((void*)((u8*)linear + linearBytes - 8), 8);
    const u32 transferFlags =
        GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
        GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB5A1) |
        GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO);
    C3D_SyncDisplayTransfer(colorBuf, GX_BUFFER_DIM(fbW, fbH), (u32*)linear, GX_BUFFER_DIM(fbW, fbH),
                            transferFlags);
    for (int spin = 0; spin < 4000; ++spin) { // 400 ms bound
        GSPGPU_InvalidateDataCache((void*)((u8*)linear + linearBytes - 8), 8);
        if (sentinel[0] != 0xDEADC0DEu || sentinel[1] != (0x5EA7BEEFu ^ 0x0F0F0F0Fu)) {
            break;
        }
        svcSleepThread(100 * 1000);
    }
    GSPGPU_InvalidateDataCache(linear, linearBytes);

    for (uint32_t yOut = 0; yOut < height; yOut++) {
        const u32 gy = (u32)(((uint64_t)(height - 1 - yOut) * kEyeHeight) / height);
        for (uint32_t xOut = 0; xOut < width; xOut++) {
            const u32 gx = (u32)(((uint64_t)xOut * kEyeWidth) / width);
            const u32 fbX = gy;
            const u32 fbY = gx;
            if (fbX >= fbW || fbY >= fbH) {
                continue;
            }
            const u16 px = linear[(size_t)fbY * fbW + fbX];
            rgba16Buf[(size_t)yOut * width + xOut] = (u16)(px | 0x0001u);
        }
    }
    linearFree(linear);
    return 0;
}
