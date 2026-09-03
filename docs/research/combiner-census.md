# Combiner Census — F-Zero X → PICA200 TexEnv Feasibility (Stream F)

**Scope**: static census of every N64 color-combiner configuration the game can emit,
classified for mappability onto the 3DS PICA200's ~6 fixed-function TexEnv stages.
Feeds stream A's TexEnv mapper.

**⚠ STATIC ANALYSIS ONLY.** This census covers combiner state set from C code in
`decomp/src` and the port. Combiner commands embedded in **binary ROM display lists**
(track surface materials in segment 8 `course_track_gfx`, stock machine models in
`machine_models`, per-course setup DLs like `D_80140F0` referenced from
`decomp/src/game/course.c:3628`) are **not visible to this census** — no ROM/O2R is
present in the tree (verified: no `*.o2r`/`*.z64` anywhere under either worktree).
The runtime census via an instrumented PC build (dev CVar dumping unique
`shader_id0/shader_id1` pairs per track — plan §3 stream F item 1) remains a
**required follow-up**. One strong runtime data point already exists: the
shader-compile-stall investigation logged **~40 shader-program compiles in one
session** (`docs/STATUS.md:188-192`, main repo), so the real variant population is
on the order of dozens, not hundreds.

---

## 1. How libultraship derives its two 64-bit shader IDs (what stream A keys on)

Source: `libultraship/src/fast/interpreter.cpp` (pinned Zorkats fork, submodule).

- `GfxDpSetCombineMode(rgb, alpha, rgb_cyc2, alpha_cyc2)` packs the raw N64 mux fields
  into a single `uint64_t combine_mode`:
  `rgb | (alpha << 16) | (rgb_cyc2 << 28) | (alpha_cyc2 << 44)` (interpreter.cpp:4586-4587).
- At draw time (interpreter.cpp:3058-3207) a `ColorCombinerKey{combine_mode, options}` is
  built. `options` (= future `shader_id1`) is a bitfield of `ShaderOpts`
  (`include/fast/interpreter.h:67-89`):
  `ALPHA, FOG, TEXTURE_EDGE, NOISE, _2CYC, ALPHA_THRESHOLD, INVISIBLE, GRAYSCALE,
  TEXEL0/1_CLAMP_S/T, TEXEL0/1_MASK, TEXEL0/1_BLEND, PRIM_DEPTH, PRISM_SHADER`,
  plus a shader-stack id in the high bits (`SHADER_ID_SHIFT`, interpreter.cpp:3175-3179).
  The option bits are derived from `other_mode_l/h`: blender selects → `ALPHA`/`FOG`/
  `INVISIBLE`, `CVG_X_ALPHA` → `TEXTURE_EDGE`, `G_AC_DITHER` → `NOISE`,
  `G_CYC_2CYCLE` → `_2CYC`, `G_AC_THRESHOLD` → `ALPHA_THRESHOLD`, `G_ZS_PRIM` →
  `PRIM_DEPTH` (interpreter.cpp:3060-3078).
- `GenerateCC` (interpreter.cpp:285-520) normalizes the muxes (degenerate A==B or C==0
  collapse to 0; 1-cycle remaps TEXEL1→TEXEL0; cycle-2-only clears unused cycle-1 state)
  and packs `shader_id0`: **per cycle, 4 RGB muxes + 4 alpha muxes, 4 bits each →
  32 bits/cycle, cycle 0 in bits 0-31, cycle 1 in bits 32-63.** Named inputs
  (PRIM/ENV/SHADE/LOD/K4/K5/CENTER/SCALE) are renumbered into up to 7 generic
  `SHADER_INPUT_n` slots with a `shader_input_mapping` table — so stream A's TexEnv
  mapper receives *slot indices + a mapping back to which N64 register feeds each slot*.
- `CreateAndLoadNewShader(id0, id1)` / `LookupShader(id0, id1)` are called from
  `LookupOrCreateShaderProgram` (interpreter.cpp:236-239; call site at 3482, which may
  also OR in `TEXEL0_CLAMP_S` per tile state).

**Consequence for stream A**: the variant cache must key on the full `(id0, id1)` pair,
but the *combiner structure* lives in `id0` + the `_2CYC/ALPHA/FOG/NOISE/…` bits of
`id1`; clamp/mask/blend bits only alter sampling, not stage topology.

---

## 2. Census: unique combiner configurations from C code

Method: `grep`/`perl` extraction of all `g(s)DPSetCombineMode` and `g(s)DPSetCombineLERP`
across `decomp/src` (multi-line-aware), macro expansions verified against
`decomp/include/PR/gbi.h:491-510`.

### 2a. Preset modes (`gDPSetCombineMode`, both cycles identical → 1-cycle semantics)

| # | Mode (count) | Cycle-1 RGB / Alpha | Inputs | Usage context | PICA verdict |
|---|---|---|---|---|---|
| 1 | `G_CC_SHADE` ×7 | 0,0,0,SHADE / 0,0,0,SHADE | shade | fill-rect setup DLs (`game/194E0.c:35`), race viewport clears (`ovl_i2/race.c:151`), records | **EASY** — 1 stage, REPLACE(primary color) |
| 2 | `G_CC_PRIMITIVE` ×14 | 0,0,0,PRIM / 0,0,0,PRIM | prim | menu fills, minimap lines (`ovl_i3/minimap.c:273`, `menus.c`) | **EASY** — 1 stage, REPLACE(constant) |
| 3 | `G_CC_DECALRGBA` ×28 | 0,0,0,TEXEL0 / 0,0,0,TEXEL0 | texel0 | HUD sprites, course-edit UI, gadgets | **EASY** — 1 stage, REPLACE(texture0) |
| 4 | `G_CC_DECALRGB` ×1 | 0,0,0,TEXEL0 / 0,0,0,SHADE | texel0, shade | one-off | **EASY** — 1 stage (RGB=tex, A=primary) |
| 5 | `G_CC_MODULATEIA` ×1 | T0,0,SHADE,0 / T0,0,SHADE,0 | texel0, shade | one-off | **EASY** — 1 stage, MODULATE(tex, primary) |
| 6 | `G_CC_MODULATEIA_PRIM` ×5 | T0,0,PRIM,0 / T0,0,PRIM,0 | texel0, prim | sprite tints (`game/racer.c`, `texture_utils2.c`) | **EASY** — 1 stage, MODULATE(tex, constant) |
| 7 | `G_CC_MODULATEI_PRIM` ×1 | T0,0,PRIM,0 / 0,0,0,PRIM | texel0, prim | course-edit | **EASY** — 1 stage |
| 8 | `G_CC_MODULATEIDECALA_PRIM` ×19 | T0,0,PRIM,0 / 0,0,0,TEXEL0 | texel0, prim | menu/HUD text tinting (`ovl_i3/menus.c`, `hud.c`) | **EASY** — 1 stage (RGB MODULATE, A REPLACE tex) |
| 9 | `G_CC_BLENDRGBA` ×5 | (T0−SHADE)·T0_A+SHADE / 0,0,0,SHADE | texel0, texel0-alpha, shade | course gadgets (`game/course_gadgets.c`) — drawn with `G_RM_FOG_SHADE_A` (2-cycle fog blender, `course_gadgets.c:551`) | **EASY** — 1 stage INTERPOLATE(tex, primary, tex-alpha); + hardware fog |

### 2b. Explicit `gDPSetCombineLERP` — cycle1 == cycle2 (1-cycle semantics)

| # | Config (count) | RGB / Alpha | Usage context | PICA verdict |
|---|---|---|---|---|
| 10 | `0,0,0,TEXEL0 / TEXEL0,0,SHADE,0` ×2 | RGB=tex; A=tex·shade | records fade (`records/records.c:940`), sky fade (`ovl_i3/background.c:1407`) | **EASY** — 1 stage |
| 11 | `0,0,0,TEXEL0 / 0,0,0,PRIMITIVE` ×2 | RGB=tex; A=prim | screen transitions (`ovl_i2/transition.c:1300`), sky (`background.c:1084`) | **EASY** — 1 stage |
| 12 | `PRIMITIVE,0,TEXEL0,0` both channels ×12 | prim·tex (RGB+A) | HUD gauges (`ovl_i3/hud.c:1540…`), menus, EK UI | **EASY** — 1 stage MODULATE(constant, tex) |
| 13 | `0,0,0,PRIMITIVE / 0,0,0,TEXEL0` ×13 | RGB=prim; A=tex | course-edit + EK UI (`course_edit/*`, `expansion_kit/A8140.c`) | **EASY** — 1 stage |
| 14 | `TEXEL0,0,PRIMITIVE,0 / TEXEL0,0,SHADE,0` ×1 | RGB=tex·prim; A=tex·shade | sky (`background.c:1409`) | **EASY** — 1 stage |
| 15 | `PRIMITIVE,0,TEXEL0,0 / (0−TEXEL0)·SHADE+TEXEL0` ×1 | RGB=prim·tex; A=lerp(tex→0 by shade) | sky (`background.c:1130`) | **MULTI-STAGE** — alpha needs INTERPOLATE(0, tex, primary-alpha): still 1 stage using GPU_INTERPOLATE on the alpha side; call it 1-2 stages |

### 2c. True 2-cycle modes — custom machine rendering (`ovl_i9/machine_draw.c`, exclusively)

All five share **cycle 2 = `(COMBINED−0)·SHADE+0` RGB, `COMBINED` alpha** — i.e. cycle 1
computes an unlit material color, cycle 2 multiplies by vertex shade (lighting).

| # | Cycle-1 config (count) | Cycle-1 meaning | Inputs | PICA verdict |
|---|---|---|---|---|
| 16 | `PRIM,ENV,TEXEL0,ENV / …,ENV alpha` ×271 | lerp(ENV→PRIM by T0); A=ENV | prim, env, texel0, shade | **MULTI-STAGE (3)** — s0: REPLACE(const=ENV); s1: INTERPOLATE(const=PRIM, prev, tex0); s2: MODULATE(prev, primary). Needs *two per-stage constants* (PRIM, ENV) — fine, PICA constants are per-stage |
| 17 | `TEXEL0,ENV,TEXEL0_ALPHA,ENV / …,ENV` ×108 | lerp(ENV→T0 by T0.a); A=ENV | env, texel0, texel0-alpha, shade | **MULTI-STAGE (3)** — same shape; source-c = tex0 with `GPU_TEVOP_RGB_SRC_ALPHA` operand |
| 18 | `1,0,TEXEL0,0 / A=1` ×56 | RGB=T0; A=1 | texel0, shade | **MULTI-STAGE (2)** — s0: REPLACE(tex); s1: MODULATE(prev, primary) |
| 19 | `0,ENV,TEXEL0,ENV / …,ENV` ×43 | ENV·(1−T0); A=ENV | env, texel0, shade | **MULTI-STAGE (3)** — INTERPOLATE(const=0/black, const=ENV, tex0) then shade-modulate |
| 20 | `0,0,0,ENV / …,ENV` ×16 | RGB=ENV; A=ENV | env, shade | **MULTI-STAGE (2)** — REPLACE(const) + MODULATE(primary) |

Stock (non-custom) machine models are binary DLs (`machine_models.yaml`) — expected to be
the same material family (the interpreter comment at `interpreter.cpp:3182-3184` says
"track **and vehicle** materials" rely on 2-cycle adjacent-tile TEXEL1), but this is
unverified until the runtime census.

### 2d. Known-but-unseen: binary ROM display lists (the census gap)

- **Track surfaces** (segment 8, `course_track_gfx` + per-course data via
  `D_800F89B8->unk_*` DLs, `game/course.c:3628-3988`): the interpreter carries a
  load-bearing comment — *"In two-cycle mode TEXEL1 uses the next tile descriptor.
  F-Zero X relies on this for track and vehicle materials, where adjacent tiles describe
  different views of one TMEM load"* (`interpreter.cpp:3182-3188`, with a dedicated
  `GDX_DIAG_TEXEL1_FROM_BASE` bisect gate). So expect **2-cycle dual-texture (TEXEL0 +
  TEXEL1) modes with fog** in-race. PICA has 3 texture units; a 2-cycle T0/T1 mode maps
  in ≤3-4 stages → **MULTI-STAGE**, but the "tile+1 = second view of one TMEM load"
  semantics mean stream A must materialize *two texture objects from one upload* (or
  duplicate uploads) — flagged as the main renderer-plumbing risk, not a stage-count risk.
- **In-race fog**: `game/course.c:4771` (`gSPFogPosition`), `course.c:4816` /
  `racer.c:5973` (`gDPSetFogColor`), `G_RM_FOG_SHADE_A` render modes
  (`course_gadgets.c:551,593,708`) → `SHADER_OPT(FOG)` shaders. PICA has a hardware
  per-fragment fog unit (fog LUTs — same approach as sm64-3ds `FOG_LUT`) → **EASY,
  native**, does not consume a TexEnv stage.
- **Cycle-type**: only one explicit `G_CYC_2CYCLE` in C (`course_edit/191080.c:3220`);
  the in-race 2-cycle enable necessarily comes from binary setup DLs. Harmless for the
  mapper — it keys off `other_mode_h` at draw time regardless of who set it.

---

## 3. Feasibility summary for stream A

| Class | Modes | Notes |
|---|---|---|
| EASY (1 stage) | #1-14 (all HUD/menu/sky/gadget modes) | direct REPLACE/MODULATE/INTERPOLATE |
| MULTI-STAGE (2-4 stages) | #15-20 (machines), expected track T0/T1 modes | worst statically-seen case needs **3 TexEnv stages**; comfortably inside PICA's 6 |
| NEEDS-APPROXIMATION | `NOISE` (`G_AC_DITHER`: `ovl_i2/transition.c:1589`, `ovl_i6/credits.c:783`) | no per-fragment noise on PICA; approximate with a scrolling dither texture or per-frame random alpha-threshold constant (sm64-3ds precedent). Non-gameplay (transitions/credits dissolve) |
| NEEDS-APPROXIMATION | `GRAYSCALE`, `TEXEL0/1_MASK`, `TEXEL0/1_BLEND`, `PRISM_SHADER` opts | LUS enhancement features; recommend **dropping on 3DS MVP** (config-off), saving mapper complexity |
| IMPOSSIBLE-SINGLE-PASS | none found statically | no mode uses >2 textures, chroma key (`G_CK_NONE` everywhere), LOD_FRACTION, or K4/K5 YUV convert |

**No statically-visible mode exceeds 6 TexEnv stages.** The runtime census must confirm
the binary-DL population (track/stock-machine materials) before this is declared final.

## 4. Framebuffer / VI-level effects and PICA implications

| Effect | Evidence | 3DS implication |
|---|---|---|
| Screen-transition snapshot: game reads back the current framebuffer, CPU-manipulates rows, redraws it as RGBA16 texture strips | `ovl_i2/transition.c:714` (`gdx_read_current_framebuffer`, port hook implemented at `port/n64_gfx_bridge.cpp:9899`), redraw via `gDPLoadTextureTile` at `transition.c:1076,1322,1786` | Stream A must implement the readback hook: GX display-transfer/texture-copy from the render target to FCRAM + RGBA8→RGBA5551 convert. Non-gameplay frequency (menu↔race transitions) → cost acceptable. This is the **only** framebuffer-readback effect found |
| Direct `gDPSetColorImage` retargeting to `gFrameBuffers[n]` | `ovl_i2/race.c:158-244`, `records/records.c:866-877`, `ovl_i3/records_entry.c:252`, `course_edit/191080.c:834`, `ovl_i10/187510.c` | Standard double/multi-buffer VI semantics ("VI scans out whatever is in RDRAM") — already abstracted by `port/n64_vi.c` (swap/track only; mode setters are no-ops) + `port/gdx_vi_convert.c` RGBA5551→8888 fallback quad. On 3DS: keep the same model; the RGBA5551 conversion maps to a native RGB5A1 PICA texture (cheaper than on PC — no 8888 expansion needed) |
| Noise dissolve (VI-adjacent) | `G_AC_DITHER` sites above | covered under NEEDS-APPROXIMATION |
| Motion blur | **none found** — no framebuffer-feedback blend anywhere in `decomp/src`; `src/framebuffers/framebuffer{1,2,3}.c` are plain buffer definitions | n/a |

## 5. Recommendations to stream A

1. Implement the EASY set + the 5 machine 2-cycle shapes (#16-20) first — that covers
   every combiner from C code; ~15 TexEnv variant programs total.
2. Key the variant cache on `(shader_id0, shader_id1 & TOPOLOGY_MASK)` where
   TOPOLOGY_MASK = `ALPHA|FOG|TEXTURE_EDGE|NOISE|_2CYC|ALPHA_THRESHOLD|INVISIBLE`;
   clamp/mask bits become sampler state, not new programs. Observed runtime population
   ≈ 40 programs/session (STATUS.md:188-192) → a 64-entry cache is plenty.
3. Use PICA hardware fog (LUT) for `SHADER_OPT(FOG)`; never spend a stage on it.
4. Escalate (per plan §3.A) only if the runtime census surfaces a binary-DL mode needing
   >6 stages — none is expected given the N64's own 2-cycle ceiling (2 lerp units)
   maps to ≤4-5 PICA stages structurally.

---
*Stream F static census, 2026-08-12. Sources: decomp @ pinned submodule, libultraship
(Zorkats fork) `src/fast/interpreter.cpp`, main-repo `docs/STATUS.md`/`docs/DIAGNOSTICS.md`.*
