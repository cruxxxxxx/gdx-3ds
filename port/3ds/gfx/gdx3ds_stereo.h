/* port/3ds/gfx/gdx3ds_stereo.h — stream S: stereoscopic-3D foundation for the
 * citro3d backend (docs/research/stereo3d-research.md).
 *
 * This is PLUMBING, not the finished stereo feature: dual render targets, the
 * per-eye draw loop's injection point, the slider poll, and the per-drawcall
 * stereo-class channel — everything the later "stereo shift" needs so that shift
 * is only projection math + tuning (see port/3ds/gfx/STEREO.md for the
 * remaining-work list).
 *
 * Design constraints (mission + research):
 *  - Default OFF via config ([stereo] enabled, gdx3ds_config — read-only use);
 *    when off the backend behaves bit-identically to a build without this TU.
 *  - Slider-driven per frame (osGet3DSliderState, 0.0-1.0, official linear
 *    pattern); the right-eye pass is skipped entirely at slider == 0.
 *  - Per-eye pass reuses the already-built VBO — only the target binding and the
 *    projection uniform change for the second eye (NO CPU vertex re-copy; the
 *    sm64-3ds lineage re-copied per eye, refuted-as-necessary by the research).
 *  - The eye transform is an abstracted injection point (ComputeEyeMatrix):
 *    off-axis (asymmetric-frustum) stereo in clip space, mathematically exact
 *    for the game's perspective projections without knowing their parameters
 *    (derivation in the .cpp). Slider-scaled live; [stereo] iod / convergence
 *    tune strength and screen plane.
 *
 * Ownership: stream S. Hooks in gfx_citro3d.cpp are deliberately minimal.
 */
#pragma once

#include <cstdint>

#include <3ds.h>
#include <citro3d.h>

/* Per-drawcall stereo class (the s2DMode precedent from sm64-3ds, generalized).
 * Foundation behaviour:
 *   SCENE            — full eye offset (placeholder shear for now).
 *   UI_ZERO_PARALLAX — zero eye offset: the draw lands at screen depth in both
 *                      eyes (HUD/menus). Heuristic default for ortho draws.
 *   SKY_DEEP         — backgrounds that must sit behind all 3D geometry
 *                      (research: SM64 skybox artifact class): pinned to
 *                      far-plane parallax regardless of drawn depth. Bridge
 *                      tagging of the background draws is still pending.
 */
enum Gdx3dsStereoClass {
    GDX3DS_STEREO_SCENE = 0,
    GDX3DS_STEREO_UI_ZERO_PARALLAX = 1,
    GDX3DS_STEREO_SKY_DEEP = 2,
    GDX3DS_STEREO_ANCHORED = 3, /* ortho draw pinned to a world depth (prim-depth rects: race markers) */
};

#ifdef __cplusplus
extern "C" {
#endif

/* Bridge-layer tag channel (later work wires actual game call sites): sticky
 * override for subsequent draws, GDX3DS_STEREO_* or -1 to return to the
 * heuristic default (ortho draw -> UI_ZERO_PARALLAX, else SCENE). Safe to call
 * from any layer; a no-op cost when stereo is off. */
void gdx3ds_stereo_set_draw_class(int stereoClass);

/* Force-enable regardless of config — for the DL harness, which does not link
 * the OS/config library. Must be called before the backend's Init(). */
void gdx3ds_stereo_request_enable(void);

/* Verification readback of the RIGHT eye's color buffer (same output contract
 * as GfxRenderingAPIC3D::ReadFramebufferToCPU: packed RGBA5551, row 0 = scene
 * top, coverage bit forced). Call OUTSIDE an active C3D frame (after EndFrame).
 * Returns 0 on success, nonzero when stereo is inactive/unavailable. */
int gdx3ds_stereo_read_right(uint16_t* rgba16Buf, uint32_t width, uint32_t height);

#ifdef __cplusplus
} /* extern "C" */

namespace Gdx3dsStereo {

/* Read config, and when enabled: create the 240x400 right-eye color+depth
 * target bound to GFX_TOP/GFX_RIGHT and switch the LCD to stereo scanout
 * (gfxSet3D(true) — mutually exclusive with 800px wide mode). Called from the
 * backend's Init() after the main (left) target exists. Returns the enabled
 * state; on any allocation failure stereo stays off and mono is unaffected. */
bool Init();

bool Enabled();

/* Per-frame gate, called from StartFrame() inside the C3D frame (only when
 * Enabled()): polls the slider, and when slider > 0 queues the right target
 * into the frame (C3D_FrameDrawOn — required for its display transfer at
 * C3D_FrameEnd) before re-binding `mainTarget`. Returns this frame's "active"
 * state: true iff the per-eye draw loop should run. */
bool FrameBegin(C3D_RenderTarget* mainTarget);

/* True iff FrameBegin() latched the right-eye pass on for this frame. */
bool Active();

C3D_RenderTarget* RightTarget();

/* Raw mid-frame target switch: C3D_SetFrameBuf without C3D_FrameDrawOn's
 * viewport/scissor reset (research: the sm64-3ds frame_draw_on precedent), so
 * the interpreter's current viewport/scissor state applies to both eyes. */
void BindTargetRaw(C3D_RenderTarget* target);

/* Mirror a main-target clear onto the right eye (only when Active()). */
void ClearRight(C3D_ClearBits flags, uint32_t color, uint32_t depth);

/* THE eye-transform injection point. eye: -1 = left, +1 = right, 0 = center
 * (mono / zero parallax). Writes `base` patched for the eye into `out`.
 *
 * Implementation: off-axis (asymmetric-frustum) stereo applied in CLIP SPACE —
 * x' = x ± k·(z − dc·w), k = sep·slider/(1−dc) — folded into the base matrix's
 * x-coefficients. Mathematically exact (see the derivation in the .cpp): a
 * shift affine in NDC depth is the same family Mtx_PerspStereoTilt generates,
 * applied post-hoc because the game owns the projection and vertices arrive
 * CPU-pre-transformed. Tuning: [stereo] iod (far-plane parallax in pixels at
 * full slider, default 12) and [stereo] convergence (NDC depth of the
 * zero-parallax plane, default 0.25), scaled live by the 3D slider.
 *
 * stereoClass semantics: UI_ZERO_PARALLAX returns the center matrix (screen
 * depth, no doubling of HUD/2D); SKY_DEEP pins the draw to far-plane parallax
 * (behind all scene geometry); SCENE applies the depth-dependent shift. */
void ComputeEyeMatrix(const C3D_Mtx* base, int eye, C3D_Mtx* out,
                      int stereoClass = GDX3DS_STEREO_SCENE);

/* Per-draw stereo class: the bridge override when set, else the heuristic
 * (`orthoDraw` — the backend passes clip-w == 1, i.e. an orthographic
 * projection, the research's HUD signature). */
int ClassifyDraw(bool orthoDraw);
/* [anchor] NDC depth (0 near .. 1 far, the interpreter z/w convention) for the next
   GDX3DS_STEREO_ANCHORED draw: the N64 prim depth of a marker rect, i.e. the machine it labels. */
void SetAnchorDepth(float d);

} // namespace Gdx3dsStereo

#endif /* __cplusplus */
