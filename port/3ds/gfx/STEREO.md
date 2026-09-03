# Stereoscopic 3D — foundation status and the remaining stereo-shift work

Stream S (feat/3ds-stereo). Verified design inputs:
docs/research/stereo3d-research.md; backend architecture:
docs/research/m1-boot-debug.md (V-VISUALS, T-TEXCACHE), STATUS.md.

## What is plumbed (this foundation)

All in `gdx3ds_stereo.{h,cpp}` plus minimal hooks in `gfx_citro3d.cpp`
(Init / StartFrame / DrawTriangles / ClearFramebuffer / [c3d] telemetry):

- **Dual render targets**: a second 240x400 RGBA8+D24S8 target bound
  GFX_TOP/GFX_RIGHT, `gfxSet3D(true)` — created only when enabled (config
  `[stereo] enabled=1` in gdiffuser.ini via gdx3ds_config, resolved weakly so
  the config-less harness links; harness opt-in = `sdmc:/gdx-harness/stereo.on`
  marker file). Default OFF: no target, no gfxSet3D, no slider poll, the draw
  path takes one extra `bool` branch per draw — bit-identical output
  (verified, see §S-STEREO in m1-boot-debug.md).
- **Slider gate**: `osGet3DSliderState()` polled every frame in StartFrame (enabled
  builds only); slider == 0 skips the right-eye pass entirely (official
  composite_scene pattern) — the right target is not even queued into the frame.
- **Per-eye draw loop**: per-draw dual emission in DrawTriangles — left eye
  draws on the main target, then a RAW `C3D_SetFrameBuf` switch (sm64-3ds
  precedent: stock `C3D_FrameDrawOn` would reset viewport/scissor) re-issues
  `C3D_DrawArrays` over the SAME repacked VBO range for the right eye. Only the
  target and the projection uniform differ: no second CPU vertex copy (the
  sm64-3ds lineage's double-copy is deliberately not reproduced).
  Texture-backed (offscreen) passes render once, mono.
- **Eye-transform injection point**: `Gdx3dsStereo::ComputeEyeMatrix(base, eye,
  out, stereoClass)` — the eye-transform. Body (stream S2, feat/3ds-stereo2):
  off-axis (asymmetric-frustum) stereo applied in clip space,
  `x' = x ± k·(z − dc·w)` with `k = sep·slider/(1 − dc)` folded into the fixup
  matrix's x-coefficients (row 1 after the portrait rotation — the same row
  Mtx_PerspStereoTilt shears). Mathematically EXACT for the game's perspective
  projections without knowing near/far: NDC depth z/w is affine in 1/z_view,
  so a shift affine in z/w is precisely the camera-translation + frustum-skew
  family (full derivation in the ComputeEyeMatrix comment). Zero parallax at
  NDC depth `dc` (convergence/screen plane), `sep` parallax at the far plane,
  linear in the slider. z and w untouched → depth test, PICA fog and TexEnv
  fog blend are bit-identical per eye. Tuning via `[stereo] iod` (far-plane
  parallax in px at full slider, default 12, clamp 0..40) and
  `[stereo] convergence` (NDC depth of the screen plane, default 0.25,
  clamp 0..0.90).
- **Stereo-class channel** (s2DMode precedent): per-draw class SCENE /
  UI_ZERO_PARALLAX / SKY_DEEP. Bridge tag hook `gdx3ds_stereo_set_draw_class()`
  (sticky, -1 = auto) for later game-side call sites; foundation default is the
  heuristic "clip w == 1 (ortho) → UI_ZERO_PARALLAX, else SCENE".
  UI_ZERO_PARALLAX draws render with the center matrix in both eyes (screen
  depth, single uniform upload — HUD/2D can never double); SKY_DEEP pins the
  draw to far-plane parallax (`x' = x ± sep·w`), behind all scene geometry.
- **Telemetry**: `[c3d]` line gains ` eyeR=<right-eye draws> stereo=<active>`
  ONLY when stereo is enabled (off-state log stays byte-identical);
  `[stereo]` lines mark enable + first activation. Harness dumps
  `sceneNN_r.bmp` right-eye readbacks (`gdx3ds_stereo_read_right`).

## Remaining work (in dependency order)

1. ~~**Proper off-axis projection math**~~ — DONE (stream S2, route a): the
   backend-only clip-space skew, shown exact (not an approximation) for
   pre-transformed perspective draws — see the ComputeEyeMatrix derivation.
   Route b (interpreter matrix interception) is unnecessary: z/w are untouched,
   so z-buffer/clip/fog concerns that motivated it do not arise. Hardware
   tuning of iod/convergence defaults (12px / 0.25) is still pending — both are
   live ini keys, no rebuild needed.
2. **Per-drawcall game tagging** — wire `gdx3ds_stereo_set_draw_class()` calls
   from the bridge/game layer (transitions, menus, HUD, score screens; the
   sm64-3ds gDPSet2d/G_SPECIAL_1 precedent). Replace the w==1 heuristic as the
   primary source; keep it as fallback. Audit F-Zero X's HUD draw sites.
3. ~~**SKY_DEEP semantics**~~ — DONE (stream S2): SKY_DEEP now pins draws to
   far-plane parallax (`x' = x ± sep·w`, depth-independent). Still needed:
   actually TAG the background draws (part of item 2 — nothing emits SKY_DEEP
   yet, the sky currently rides the w==1 heuristic to zero parallax).
4. **Fog / decal / depth interaction** — the clip-space skew does not touch
   z/w, so the PICA fog LUT input and TexEnv fog factors are identical per eye
   by construction. Remaining: re-run the harness FOG and DECAL scenes with
   stereo active and compare eyes. Decal depth-map nudge (SetZmodeDecal) is
   per-context state — verify it holds across the mid-draw target switch on
   hardware.
5. **Framebuffer effects per eye** — transition captures / frame mirror
   currently capture the LEFT eye only (ReadFramebufferToCPU/CopyFramebuffer
   read fb0), and offscreen passes render mono. Decide per effect: capture per
   eye, or replay the mono capture at a chosen depth class (research open
   question 3).
6. **Performance** — measure the real per-eye cost on New3DS hardware (research
   caveat: no benchmark exists; Azahar numbers below are host-bound). Options
   if needed: skip right-eye for tiny-parallax draws, per-eye scissor trims.
   (Stream S2 folded the two uniform uploads into one for UI_ZERO_PARALLAX
   draws — uniforms survive the raw target switch.) The extra
   ~2x `C3D_SetFrameBuf` + uniform uploads per draw (~380/frame in menus) may
   justify draw-stream recording later; measure first.
7. **800px-wide-mode interlock** — stereo and gfxSetWide are hardware-mutually
   exclusive; if a wide mode ever ships, gate the two config keys against each
   other at the OS layer.
8. ~~**Persisted stereo strength / response curve**~~ — DONE (stream S2):
   `[stereo] iod` and `[stereo] convergence` ini keys (see above); the slider
   response stays linear (official pattern). Defaults still need real-hardware
   tuning with the 3D slider.

## Verification recipe

- Off-state A/B: build, run the harness WITHOUT `sdmc:/gdx-harness/stereo.on`,
  `check_scene_bmps.py` must stay 36/36 and the BMPs byte-identical to a
  pre-stereo build.
- On-state: `touch <sdmc>/gdx-harness/stereo.on`, set Azahar `factor_3d=100`
  (qt-config.ini [Renderer]); expect `[stereo] enabled`/`ACTIVE` lines,
  `eyeR>0` in `[c3d]`, `sceneNN_r.bmp` alongside every left dump, left-eye
  checks still 36/36. Game flavour: `[stereo] enabled=1` in
  `sdmc:/3ds/gdiffuser/gdiffuser.ini`.
