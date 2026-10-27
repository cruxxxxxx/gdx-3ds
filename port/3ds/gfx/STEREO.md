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

## Frame replay (n60stereo; `gdx3ds_stereo_frame.cpp`, `[native] stereo_replay`, default 1)

The per-draw loop above costs two raw target switches and up to two projection uploads per draw.
The frame replay makes the right eye with almost no CPU: every draw of the frame's *body* is
emitted ONCE, eye-independent, and the right eye is the same command lists submitted again.

- **Shader** (`shader.v.pica`): the incoming clip vector (the legacy packed vertex, or gpuxf's
  packed clip `r2`) is skewed on x by `steye * dot(stk, clip)` before the projection, i.e. the
  same `base · S` ComputeEyeMatrix folds into the matrix. `stk` is a per-draw, eye-INDEPENDENT row
  (`Gdx3dsStereo::ClassRow`): SCENE `(0,0,k,-k·dc)`, SKY_DEEP `(0,0,0,sep)`, ANCHORED
  `(0,0,0,k·(d-dc))`, UI / mono / offscreen `0` -- `k`, `sep`, `dc` exactly ComputeEyeMatrix's.
  `steye` is `-1` / `+1` / `0` and is written RAW, never through citro3d (citro3d never re-sends a
  register it never sent, so no state re-emission can put a stale eye inside the body). With
  `steye = 0` the skew adds exactly 0: mono output is bit-for-bit unchanged (gated).
- **Enter** (`ReplayEnter`, from `gdx3ds_nrr_bk_frame_active`: the first NRR hand-off of a frame
  with the slider up): the open command list (draws so far took the per-draw loop: both eyes done)
  ends with `steye = -1` + the main target's framebuffer words and is submitted; from here the
  frame is the body. Every citro3d state group and uniform is re-emitted at the body's first word
  (`C3Dgdx_FlushState`), so the body runs the same from any GPU state but the framebuffer / steye.
- **Per draw**: DrawTriangles (legacy draws inside the body) and the NRR core (citro3d path and
  direct words, `NrrCoreC3dStereo` / `NrrCoreDxStereo`; `nrr_direct=2` verifies the register) set
  `stk` from the draw's class: the per-draw loop's classification (bridge override, ortho
  heuristic `w == 1`, [anchor] prim-depth rects incl. the NRR HUD's -- `NrrStereoAnchor` --,
  texture passes mono). One 8-word uniform write when the class row changes.
- **Frame end** (`ReplayFrameEnd`, after the `[overlap]` drain, before `C3D_FrameEnd`): close the
  body, add a prologue list (right target's framebuffer words, `steye = +1`), re-add the body's GX
  queue entries (command lists as they are; GX memory fills of the main target retargeted to the
  right target, other fills repeated; texture copies / transfers / DMA -- captures and uploads --
  NOT repeated: they are mono, as in the per-draw loop), an epilogue (`steye = 0`), then
  C3D_FrameEnd queues both display transfers. A main-target rebind after a texture pass inside the
  body gets its framebuffer words in a list of its own, swapped for the right target's in the
  replay (`swaps=`).
- **Receipt**: `[stereo] replay frames= stereo= replayed= refused= full= lists/f= fills/f= skip/f=
  swaps= chunks/f= chunkskip= noflush/f= gx/f= enter_us= (sync ) chunk_us= end_us=` every 256 frames
  with stereo on (`gx/f` = GX queue entries of a replayed frame before its display transfers).
- **No re-flush** (`[native] stereo_noreflush`, default 1, n60st3): a replayed command list is
  re-added with word 7 (GSP's data-cache flush of the list) cleared -- its own entry ran first in the
  same queue and flushed the same, never rewritten, bytes. Exact; `noflush/f=` counts them. Slider 0 never enters (mono).
- **Limits**: the body's texture uploads are not repeated, so a texture slot rewritten inside the
  body (after a draw that sampled its old content) would show the new content in the right eye
  for that frame; the GX queue holds 32 entries (a frame that cannot fit the replay counts
  `full=` and its right eye holds only what was drawn before the body); a frame whose replay could not be entered
  (`refused=`: a taskless hold present, no queue room) keeps the NRR categories off for that frame.

## Race GPU path (stereo-race-fps; `[stereo] head_kick`, `[stereo] resume_fold`, default 0)

With the slider up the race frame's right eye costs little CPU (hardware 3d7: enter ~95 µs, the two
interleave chunks ~240 µs, end ~30 µs on the Old 3DS) but the GPU path after the first `[overlap]`
kick grows: every GX queue entry costs ~0.15-0.3 ms of GPU start-up (gputl `tiny`), each eye's
colour+depth fill ~0.3 ms, and in 3D the frame-end drain waits on that path (`[c5cat] fend`
2.3-4.4 ms in 3D vs ~0.5 ms in 2D, 3d7 V). Both levers shorten that path and are exact (both eyes
bit-for-bit the lever-off frame); `2` = A/B (head_kick 512-frame blocks, resume_fold 4096-frame
blocks), `[gputl] tp=` gets `+8` (head) / `+16` (resume) in the phases they run.

- **head_kick=1**: at `ReplayEnter`, when the frame passes the `[overlap]` cadence (the frames that
  kick) and nothing queued so far draws (no `DrawUsed`, no draw words in the closed list, no command
  list in the queue before it), the GX queue is STARTED right there (`gdx3ds_overlap_head_run`: no
  heap flush, no split, no chunk): both eyes' clears and the pre-body list run while the CPU builds
  the sky/track, so they leave the post-kick path. It counts as a kick for the frame-end drain +
  stop, the split flags and `gdx3ds_overlap_frame_kicks` (sky_late then refuses the frame), not for
  the kick budget or the stereo chunks. A head that would draw is refused (`hkdrew=`).
- **resume_fold=1**: the left eye's resume words after each right-eye chunk (main framebuffer +
  `steye = -1`) no longer get a GX list of their own: they lead the next left list (16-byte
  aligned, an idempotent uniform-config pad keeps the uniform write aligned), and the replay re-adds
  that list from PAST them (`ReplayList` start + size adjusted), so the right eye never sees them.
  -2 GX entries per frame with two chunks. Falls back to the separate list when the open list is
  not empty or not aligned.
- **Receipts**: `[stereo] race init head_kick= resume_fold=` once; `[stereo] race frames= replayed=
  hk/f= hkclean= hkdrew= rfold/f= rskip/f= head= resume=` per replay window, only while a lever is
  set (the `[stereo] replay` line is unchanged). `[gputl] seq` shows one `l` fewer per chunk and the
  fills before the first kick mark. 3D off and the Old 3DS profile: nothing changes (both levers act
  only inside a replayed frame; nrr_size `--direct` identical).
- Rejected: right-eye LOD / HUD-once (the replay runs the SAME lists for both eyes; the HUD blends
  over a per-eye scene), a shifted sky copy (tiled framebuffer shifts in 8 px steps; the sky is zero
  parallax by the ortho heuristic), per-eye uniform uploads (already none but `steye`), an end kick
  before the drain (late frames only move the GPU gap), one fill for both eyes (needs adjacent VRAM).

## One submission (onesubmit; `[stereo] onesubmit` 1, `onesubmit_narrow` 1, `onesubmit_kicks` 0, `onesubmit_ab` 0)

Both eyes of a body chunk in ONE GX queue entry: the list is P (left only: resume words + the
`CMDBUF_SIZE1/ADDR0/ADDR1` words) + B (the body, a render-cache flush, `CMDBUF_JUMP1`); behind it R
(right framebuffer, `steye = +1`, channel 0 = B, channel 1 = E, `JUMP0`) and E (FINALIZE). GSP gets
[P][B]; the GPU runs B for the left eye, jumps to R, runs B again for the right, ends in E: one
submit, one P3D IRQ, no CPU between the eyes. Chunks that are not exactly one open list fall back
to the re-submitted replay (`[stereo] onesub fb=`). `onesubmit_kicks` 1 = no mid-frame body kick, 2 =
no head run either; `onesubmit_narrow` = range flushes instead of the frame end's whole-heap flush;
`onesubmit_ab` cycles baseline / onesubmit / +kicks / +narrow per `[cpucut]` block. Emulator: both
eyes bit-identical to the lever-off frame in every arm, race entries 7 -> 6 per frame.

Hardware (card-6 `ba0692e`, per-block A/B on both consoles): on the Old 3DS every onesubmit arm
lifts the 3D menu from 40.2 to 44.6 fps (one GX entry and one P3D IRQ fewer per chunk: the
previous frame's queue finishes 1.9 ms sooner) and gives the 1P 3D race ~1.5 ms (onesubmit) /
~1.8 ms (+ narrow) of headroom a frame; the baseline was the only arm to drop below 60. New 3DS:
neutral (60 fps in every arm). `onesubmit_kicks` loses the CPU/GPU overlap (+1.0 / +2.5 ms of
frame-begin GPU wait) and stays off. No visual difference in 3D in any arm (narrow flush
included). **Shipped (ship6): `onesubmit = 1`, `onesubmit_narrow = 1` in the code defaults and both
auto presets; `onesubmit_kicks = 0`, `onesubmit_ab = 0`.** Each is still an ini key (`onesubmit =
0` is the old re-submitted replay). Details, receipts and the A/B card snippet:
`docs/research/native/onesubmit-progress.md`; dispatch cost receipt: `[debug] gxcost` (default 0).

## Menu depth layout (menu-stereo-depth; `[stereo] menu_depth`, default 0)

**What was wrong (measured, `[debug] stereo_census` + `--3d` SHOT pairs, legacy and `ni_menu=1` alike).**
Every menu 2D draw is ortho (clip w == 1), so the heuristic put all of it -- backdrop, panels, text --
at zero parallax: the menus were one flat sheet. Every menu 3D draw is SCENE and took the race's
off-axis shift with the race convergence (0.25), but the menu cameras put their models at NDC depth
0.96 .. 0.999 (N64 near planes are tiny: the transitions' frustum is near 16 / far 8129 with the eye
500 away, `decomp/src/overlays/ovl_i2/transition.c` Matrix_SetFrustrum), i.e. at the far-plane
parallax: the 30 SELECT MACHINE thumbnails +22.7 px R-L, the MACHINE SETTINGS model +23.7 px, the
course previews +12 px (a folded half-size viewport), each with < 0.2 px of internal relief -- flat
cut-outs sunk deep behind the zero-parallax panels they are drawn on top of (occlusion says in front,
disparity says far behind), and in the machine grid 23 px is two thirds of a 35 px cell: every
machine's other-eye image lands on its neighbour. The tiled whirl / spiral transitions draw the
captured (flat) screen as perspective tiles at d ~= 0.97: the whole menu, text included, jumped from
0 to +23 px for the transition and back.

**The layout (lever on, slider up, a menu game mode: title, main menu, machine select / settings,
course select, the GP next-course card, records course select, options).** Disparities R - L in
top-screen px at full slider and iod 12, scaled live by slider x iod / 12:

| layer | what | disparity |
|---|---|---|
| backdrop | screen-space draws whose largest triangle is >= 272 px (the art, drawn in full-width strips) | `menu_back` +4 (behind) |
| panel | 32 .. 160 px (icons, portraits, frames, banners) | 0 (screen plane) |
| text | <= 24 px (glyphs, arrows, cursors) | `menu_front` 2 -> -2 (in front) |
| model | every SCENE draw of the frame (menu 3D, transition tiles) | `menu_model` 0 (flat, on the plane with the panels) |

Linear ramps between the size breakpoints (a growing element slides, never pops). Models sit ON the
plane, not behind it: menus draw 2D over models and models over 2D in the same cell (SELECT MACHINE
draws the pilot portrait and the cursor burst, then the selected machine over both), so any
separation between the model layer and the icon layer is a depth conflict where they overlap -- the
first hardware build had models at +1 and 32..48 px icons ramped to ~-0.7 and the top-left cell read
as a clash. Portrait, burst and machine now share the plane; only text floats in front. A screen-space
draw touching the left / right screen edge stays at 0 (a shifted edge would uncover a strip in one
eye). Models stay >= 0, so a model cut by its cell or the screen edge never violates the window.
Size is per triangle, not per draw, because the legacy interpreter and the NRR menu runner batch the
same rectangles differently. One draw is still one layer, and the batchers merge rectangles across
texture changes (atlas pages): during the title's load-in the top half of the copyright line rode in
the backdrop's batch (+4) while its bottom half was its own draw (0), so the line looked cut in two
for ~1.4 s; and the NRR menu runner (`ni_menu=1`) batches a panel with the glyphs on it (SELECT
MACHINE's "1260kg" sat on the plane there, in front on the legacy path). Every path now cuts a draw
where its rectangles (triangle pairs sharing one box) change layer class -- backdrop (>= +0.5 px),
plane, text (<= -0.5 px; the ramps' first half px counts as the plane, so a 31 px value box is a
panel beside its arrows) -- and draws each run at its own layer (`MenuUiRunEnd`): the per-draw loop
(`StereoMenuUiRuns`: both eyes per run), the frame replay (`StereoReplayMenuRuns`: one projection
upload, an stk row per run; not while a native replay records) and the NRR menu runner
(`NrrStereoMenuRow`, from the HUD's cold stereo hook: it draws the leading runs itself and hands the
hot `NrrHudHotDraw` the last). One exception keeps nine-slice panels whole: between the plane and the
text layer a rectangle that abuts (shares an edge with, without overlapping) one already in the run
stays in it -- frame corners and borders touch their neighbours, glyphs sit inside or apart. A
non-rectangle triangle never starts a run, so meshes stay whole. With `[debug] det=1` the legacy and
`ni_menu=1` paths now give byte-identical menu SHOTs in both eyes. Races, the records flyby, demos, credits and editors are untouched;
`[stereo] convergence` no longer moves menu models (they are a layer, not a depth).

Paths: the per-draw loop and the frame replay classify in DrawTriangles (`StereoMenuUiClass`); the
NRR menu runner's batches take the same row through the HUD's cold stereo hook
(`NrrHudStereoAnchor` -> `NrrStereoMenuRow`), so no NRR hot function changes (`nrr_size --direct`
identical). Census: `[stc]` lines carry `p=D` (per-draw loop), `p=R` (frame replay) or `p=N` (NRR
menu runner; `m=` is GET_MODE there). Receipts: `[stereo] menu_depth=1 back= front= model=` at init, `[stereo] menu_depth
ACTIVE` once, `[stereo] menu frames= ui= back= front= plane= edge= model=` every 256 menu frames.
Census: `[debug] stereo_census=N` prints an `[stc]` line per stereo draw of every N-th frame
(class, NDC depth, screen box, triangle size, disparity). `--3d` SHOTs now write the right eye as
`<tag>_RE.bmp` (`_R` is the bottom screen's and overwrote it).

## Edge coverage (skystereo; `gdx3ds_stereo_edge_shift_ndc()`)

The eye skew moves each eye's image of far geometry sideways by up to `sep` (far-plane parallax,
`iod` x slider px), so near the left / right screen edge each eye needs geometry the mono frame
never had to cover. `gdx3ds_stereo_edge_shift_ndc()` is that largest per-eye shift this frame
(NDC-x, `max(sep, k*dc)`), exactly 0 while the right-eye pass is off. `Background_Update` widens
the skybox / venue floor / clouds `horizontalRange` by it (`decomp-3ds-stereo-sky-margin.patch`):
no extra draw, and slider down or 2D is bit-identical. The course chunk cull is NOT widened by
it: the 1.15 `widescreen_cull_pct` margin already covers the shift (measured: +2.5 % draws, no
pixel changed).

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
