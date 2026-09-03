# Stream A — gfx (citro3d backend)

## Shift T-TEXCACHE (texture re-upload thrash, feat/3ds-texcache)

- **Root cause found in the interpreter, not the backend**: the backend never
  evicts (mTextures is append-only; UploadTexture only runs on an interpreter
  cache miss). The interpreter's `tmem_content_hash` hashed
  `min(remaining TMEM, tile_line_bytes*64)` — up to ALL of TMEM — so a stable
  texture's key folded in every other texture streaming through TMEM and every
  import minted a fresh key: miss → re-decode → Morton re-swizzle → re-upload,
  each frame, in any scene with moving content. Proven by the new `[texmiss]`
  telemetry: 400/400 miss lines with zero repeated hashes per address, texDel
  flat (invalidation innocent).
- **Fix (lus-texcache-content-hash-span.patch)**: hash exactly the bytes the
  decode reads (RGBA16 TMEM path mirrors ImportTextureRgba16's extent incl.
  mask/CLAMP; font I/IA use the recorded load extent) + 4-byte-folded FNV.
  Strict superset of decoded bytes → staleness safety unchanged; animated
  sources (transition strips) still re-upload as they must.
- **Backend telemetry**: [c3d] line now carries `texImp/texMiss/texDel`
  cumulatives via `gdx3ds_texcache_note_*` hooks; bounded race-gated
  `[texmiss]` key dumps (400 lines).
- **Numbers (frame-exact scripted A/B)**: title fade 48.7 → 0.7 miss/frame;
  cumulative misses at frame 4033: 4800 → 978; static-title fps 19.9 → 29.7
  (hash-cost win: zero misses in both builds); menu windows 14.6-15.0 →
  19.0-19.9 fps; attract-class sustained thrash (4.4-10 miss/f) eliminated.
  Driving-race residual 1-3 miss/f is genuine content change (staging buffer,
  repeating hashes). Harness 36/36 pixel checks pass. Full evidence:
  docs/research/m1-boot-debug.md "T-TEXCACHE".

## Shift V-VISUALS (in-race visual bug hunt, feat/3ds-vfix)

### Backend changes

- **Texture-backed framebuffer orientation fix**: tex-backed targets now
  allocate in the screen's portrait convention — fb-x = pow2(game height),
  fb-y = pow2(game width). The previous landscape allocation gave a 320-wide
  game viewport only a 256px fb-y axis: `SetViewport`'s `fbY = fbH - (x+width)`
  clamped at 0 and 64px of game-x were clipped off every render into (and CPU
  readback out of) a texture framebuffer — the frame mirror, autotest shots,
  and any future mGameFb rendering were all cropped and mis-laid-out.
- **CopyFramebuffer screen↔texture implemented** (was log-and-skip): all 3DS
  surfaces now share one rotated tiled-RGBA8 layout (content anchored fb-x 0,
  end of fb-y), so every copy is a GX texture copy; differing fb-x strides
  (screen 240 vs pow2 texture) use the copy engine's per-line gap (16-byte
  units), differing fb-y extents use a start offset on the larger surface.
  Unblocks GdxUpdateFrameMirror (screen→mirror each task frame) and the
  vifallback hold tick (mirror→screen) on 3DS — both previously no-ops.

### Harness changes (port/3ds/harness/)

- Three new scenes: **TEXEL1** (the interpreter.cpp:3182-3188 adjacent-tile
  pattern — one 32x64 RGBA16 TMEM load, tile 0 = rows 0-31, tile 1 = a 32x24
  window at TMEM word 256, quads for TEXEL0 / TEXEL1 / shade-lerp, with a
  magenta out-of-window sentinel), **MACHINE** (census #16 3-stage 2-cycle
  chain incl. the constant-spill prefix stage), **FOG** (depth-tilted quad
  through G_FOG + G_RM_FOG_SHADE_A against the native fog LUT).
- Headless self-verification: scenes auto-advance (~4 s each), each dumps a
  320x240 BMP to `sdmc:/gdx-harness/sceneNN.bmp` via ReadFramebufferToCPU,
  the app exits after a full cycle, and `check_scene_bmps.py` asserts the
  EXPECTED.md pass conditions per scene from the host.

### Verification results (Azahar headless, BMP pixel checks)

- **36/36 checks pass across all 9 scenes**; zero `unmapped combiner` lines;
  `[c3d]` bindMiss=0 / texUpFail=0 throughout.
- TEXEL1 adjacent-tile: VERIFIED end to end (unit-1 bind per TMEM load, tile+1
  view selection, non-pow2 tile-1 window UV rescale, per-vertex T0/T1 lerp).
- Census #16 3-stage chain (constant-spill prefix + cycle-2 shade modulate):
  VERIFIED.
- Fog: run 1 exposed an INVERTED gradient — the PICA fog unit indexes the LUT
  with z/w (0 = near), not the depth-buffer value 1 - z/w. Fixed in
  UpdateFogState; FOG scene now passes all gradient checks.
- ReadFramebufferToCPU: run 1 showed every capture horizontally mirrored —
  content actually runs ALONG fb-y from 0 (dead band = fb-y tail). Fixed
  (fbY = gx); this is the in-game transition-capture path.
- Scenes 1-6 (shift-2 verdicts incl. DECAL): all still pass.
- See docs/research/m1-boot-debug.md "V-VISUALS" for the full evidence chain.

## Shift 2 (harness-verdict fixes + census combiner work)

### Root causes fixed (await human visual re-verification)

- **Scene 3/6 texture V-flip**: `UploadTexture` wrote rows top-to-bottom, but the
  PICA samples T=0 from the LAST row of tiled memory (bottom-up storage). N64 row
  y now lands at padded row `padH-1-y`, putting V=0 at the texture top and the
  pow2 padding rows outside the sampled `[0, vScale]` range. Fixed in the upload
  (not the UVs) — the interpreter's N64-convention rows are untouched.
- **Scene 4 scissor**: `SetScissor` used the identity axis swap (`fb_y = x`),
  which mirrors game-x on the physically 90°-rotated target; game-x runs along
  fb-y NEGATED (`fb_y = fbHeight - x`, matching the fixup matrix `y' = -x`).
  `SetViewport` had the same latent offset bug — it was masked at fb-y origin 0
  (why scene 1 passed); both now share `CurrentTargetFbHeight()`. Side effect:
  the 80px dead band moves to the RIGHT edge, the intended framing per
  EXPECTED.md.
- Scene 2 rotation code untouched (verdict unconfirmed, per shift instructions).

### Census-driven combiner work (docs/research/combiner-census.md)

- **Multi-stage TexEnv (2-cycle)**: `MapCombiner` now builds up to 4 stages
  (census worst case 3). Cycle 2 maps with `COMBINED → GPU_PREVIOUS`; per-stage
  constant registers carry PRIM/ENV (refreshed from vertex 0 per draw); the
  single vertex-colour attribute is pre-assigned to the *latest* cycle's input
  (census: cycle 2 multiplies by per-vertex SHADE). When one channel needs two
  distinct constants in a stage (#16/#19 `INTERPOLATE(PRIM, ENV, T0)`), the
  first is materialized by a REPLACE prefix stage and consumed as PREVIOUS. All
  20 statically-censused modes map: #1-15 in 1 stage, #16/#19 in 3, #17/#18/#20
  in 2.
- **TEXEL0+TEXEL1 adjacent-tile**: texture unit 1 wired end to end — second UV
  set repacked at out[10..11] (fixed layout now 12 floats: pos4/uv0/rgba4/uv1,
  attr v3, permutation 0x3210), `shader.v.pica` passes texcoord1 through, and
  DrawTriangles binds both tiles' texture objects (the interpreter already
  ImportTextures per tile, so "two texture objects per TMEM load" arrives as two
  SelectTexture/UploadTexture pairs — interpreter.cpp:3182-3188).
- **Fog**: PICA native fog unit, no TexEnv stage spent. The interpreter bakes
  RSP fog into the vertex stream (fog rgb draw-constant + per-vertex factor,
  both linear in z/w); the backend fits `factor = a·depth + b` per draw and
  binds a cached `C3D_FogLut` keyed on quantized (a, b). Cache cleared at frame
  boundaries on overflow (>64).
- **Shader variant cache**: unordered_map (node-based, handle-stable) with 64
  buckets reserved per the census population estimate (~40/session).
- **NOISE / grayscale / invisible**: still log-and-fallback (MVP per census —
  non-gameplay usage).

### LUS resource-cache cap (port/3ds/patches/lus-resource-cache-cap.patch)

- ResourceManager gains byte-accounted LRU eviction: payload bytes
  (`GetPointerSize()`) tracked per entry with a monotonic access tick; inserts
  over the 24 MiB default budget (3ds-memory-budget.md:42) evict
  least-recently-used entries whose only reference is the cache itself
  (externally referenced resources are pinned). `SetCacheByteBudget(0)` restores
  desktop behaviour. Applied to the submodule working tree on top of
  lus-newlib-portability.patch (apply order matters); verified
  `git apply --check` clean from the newlib baseline.

### Verification state

- devkitARM build green: all targets incl. `gdx3ds-dl-tests.3dsx`.
- Azahar headless run (~20s): boots via SelfNCCH, main loop runs, **no crash /
  svcBreak / GPU errors** in the Azahar log. Caveats: (1) the harness advances
  scenes only via START and headless input injection is unavailable (macOS
  accessibility denied), so only scene 1 executed; (2) the harness's
  `[gfx_citro3d]` stderr goes to the emulated bottom-screen console
  (consoleInit hooks stderr), so host-side stderr cannot carry the
  unmapped-combiner lines — scene 6's four modes verified by static trace
  through the new mapper instead (all map; zero unmapped lines expected).
- **Awaiting human visual re-verification**: scene 3/6 arrows point UP, red
  stripe top; scene 4 yellow top-LEFT; scenes 1/5 unchanged; dead band now on
  the right; bottom-screen console shows no `unmapped combiner` lines through
  all six scenes.

## Done

- **LUS newlib patch applied** to the `libultraship/` submodule working tree
  (`git apply port/3ds/patches/lus-newlib-portability.patch`; per plan it is NOT
  committed inside the submodule — it lands in the fork at integration).
- **`gdx3ds_lus_carve`**: all 36 spike-list sources compile to a static lib with
  devkitARM (include order `lus_stubs` → `third_party` → LUS include/src; cvars.cmake
  included for the CVar name macros; `-DF3DEX_GBI_2`; exceptions/RTTI on; no
  `ENABLE_OPENGL`). Third-party headers vendored under `third_party/`:
  nlohmann/json.hpp v3.11.3, BS_thread_pool.hpp v4.1.0, tinyxml2 10.0.0.
  **Deliberate addition beyond the 36-source spike list**: `third_party/tinyxml2.cpp`
  is compiled into the carve lib (third-party dep of the XML factories, not a LUS
  carve widening) — without it tinyxml2 symbols are unresolved at link.
- **`gdx3ds_gfx`**: `Fast::GfxRenderingAPIC3D` (gfx_citro3d.cpp/.h) implements ALL
  GfxRenderingAPI virtuals; `Gdx3ds_GetCitro3dRenderer()` factory per the frozen
  contract. Real citro3d paths: Init (C3D context, main 240x400 RGBA8+D24S8 target,
  DVLB shader load, attr layout, clip fixup matrix), StartFrame/EndFrame,
  clear/viewport/scissor/depth-test/depth-mask/decal/blend state, NewTexture/
  UploadTexture (RGBA32 → PICA 8x8-tile Morton swizzle, pow2 padding + UV rescale,
  GSPGPU_FlushDataCache), DrawTriangles (variable-stride interpreter VBO repacked
  into a fixed pos4/uv2/rgba4 layout inside a 1 MiB linearAlloc pool, exhaustion
  logged once per frame, C3D_DrawArrays), SetCurrentAlphaCompareThreshold →
  C3D_AlphaTest (G_AC_THRESHOLD path).
- **PICA vertex shader** `shader.v.pica` (picasso via `ctr_add_shader_library`,
  embedded via `dkp_add_embedded_binary_library`): clip-space pass-through with a
  fixup-matrix uniform (landscape→portrait rotation + `[0,w]`→`[-w,0]` depth remap;
  reversed depth, near = 1, test GEQUAL, clear-to-0).
- **Combiner mapping (basic set, per shift scope)**: shader pool keyed on the
  2×64-bit ID pair; `gfx_cc_get_features()` decode; cycle-0 formulas classified to
  TexEnv stage 0 as REPLACE / MODULATE / INTERPOLATE / MULTIPLY_ADD with sources
  TEXTURE0 / PRIMARY_COLOR (first vertex-colour input) / CONSTANT (second input,
  refreshed from vertex 0 per draw — valid for draw-constant prim/env) / fixed 0/1.
  2-cycle (non-passthrough cycle 1), TEXEL1, NOISE, COMBINED-in-cycle-0, 3+ distinct
  inputs, over-subscribed constants → **unmapped**: logged once per unique ID pair as
  a single `[gfx_citro3d] unmapped combiner id0=… id1=… rgb0=… a0=… rgb1=… a1=…
  2cyc/fog/noise/gray/texedge/inputs/tex …: <reason>` stderr line (cross-references
  stream F's census format), then drawn with a visible fallback (texture×vertex-colour
  or vertex colour). Fog/grayscale/noise/invisible options log but still draw.
- **Framebuffer ops (minimal-correct)**: fb 0 = screen target; CreateFramebuffer +
  UpdateFramebufferParameters → render-to-texture via `C3D_TexInitVRAM` +
  `C3D_RenderTargetCreateFromTex`; StartDrawToFramebuffer → C3D_FrameDrawOn;
  ClearFramebuffer → C3D_RenderTargetClear; SelectTextureFb/GetFramebufferTextureId
  wired. ResolveMSAAColorBuffer = no-op (PICA has no MSAA; we never allocate MS
  buffers). GetPixelDepth returns empty + logged TODO; ReadFramebufferToCPU zero-fills
  + logged TODO; CopyFramebuffer no-op + logged TODO (needs a GX display-transfer
  path).
- **Stereo**: plumbed (`mStereoEnabled`), off — post-MVP.
- **Everything compiles with devkitARM**; default `cmake --build build-3ds` builds all
  targets including the `.3dsx` (backend is not yet linked into the executable —
  that's integration's Fast3dWindow wiring, per port/3ds/CMakeLists.txt ownership).

## Unresolved-symbol audit (gdx3ds_gfx_linktest)

`cmake --build build-3ds --target gdx3ds_gfx_linktest` links carve + backend with
`--whole-archive` **and `--no-gc-sections`** (3dsx.specs' `--gc-sections` otherwise
hides the gaps by discarding unreferenced code). Result: **25 undefined symbols, 5
families** — far smaller than feared; Window/Gui/spdlog/StormLib do NOT appear
(header stubs are self-sufficient, and this fork gates OtrArchive construction behind
`INCLUDE_MPQ_SUPPORT`, so the spike's ArchiveManager.cpp:270 concern is already
closed).

| Family | Symbols | Recommendation |
|---|---|---|
| libzip | `zip_open/zip_close/zip_fread/…` (15) | Stream D owns the O2R read path: cross-compile libzip (portable C over zlib, `dkp-pacman -S 3ds-zlib`). For A's standalone replay harness a stub TU is fine. Do NOT widen the carve. |
| `Ship::Context` | `GetInstance`, `GetResourceManager`, `GetConsoleVariables` (3) | Stub at link: a small shim TU owning the singleton `shared_ptr<Context>` + the two accessors. Adding `src/ship/Context.cpp` to the carve would drag Window/Console/CrashHandler — reject. Integration/E decision. |
| `Ship::ConsoleVariable` | `GetFloat`, `GetInteger` (2) | Stub returning the caller-provided defaults (3DS has no ImGui CVar editor; config comes from stream B's INI). Alternative: carve in `src/ship/config/ConsoleVariable.cpp` + Config deps — heavier, only if INI-backed CVars are wanted inside LUS itself. |
| CVar C API | `CVarGetFloat`, `CVarGetInteger` (2) | Same stub TU as above (thin wrappers). |
| G-Diffuser port | `gdx_dbg_logf` (port/n64_sched.c:496), `gdx_workshop_dump_texture` / `gdx_workshop_texture_dump_enabled` (port/gdx_workshop.cpp) (3) | This fork's interpreter.cpp calls repo-side helpers; the full 3DS app links these existing port sources anyway (stream E's 32-bit sweep covers them). For the standalone harness: 3-line stub. Not a carve issue. |

## Blocked on

- **Human visual re-verification** of the shift-2 fixes (texture V-flip, scissor
  mapping, dead-band side) and first visual confirmation of the multi-stage /
  TEXEL1 / fog paths (no harness scene exercises them yet).
- **Stream F runtime census** (instrumented PC build) → confirms the binary-DL
  track/stock-machine combiner population before the mapper is declared final.

## Next

1. Re-run the six harness scenes with a human at the screen; confirm scene 2's
   rotation direction while there (still unconfirmed — code untouched).
2. Harness scenes for the new paths: a 2-cycle machine-material mode (#16 shape),
   a TEXEL0+TEXEL1 adjacent-tile draw, and a fogged draw.
3. Grayscale second-stage lerp + noise approximation (scrolling dither texture or
   per-frame alpha-threshold constant, sm64-3ds precedent) — post-MVP per census.
4. CopyFramebuffer via GX display transfer; GetPixelDepth via depth-buffer transfer +
   untile (needed if the game reads depth for lens flares etc. — check with F).
5. Link stubs for the audit families (with orchestrator/E sign-off) so the linktest
   target closes and the replay harness can run standalone.
