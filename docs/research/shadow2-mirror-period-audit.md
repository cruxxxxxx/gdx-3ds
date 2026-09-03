# SHADOW-2 — mirror periodicity vs pow2 padding: the definitive audit

**Charter:** answer, with code cites, whether the 3DS backend's hardware MIRROR wrap
period equals the LOGICAL texture size or the pow2-PADDED size, end-to-end for the
machine drop-shadow (I4 32x64, `G_TX_MIRROR|G_TX_CLAMP` both axes), and fix the
"wrong shape" if the padded-mirror displacement is the cause. Emulator was CONTENDED
for this pass (lock held); every claim below is static, but grounded in committed
receipts from earlier on-device runs and in the shipped asset bytes.

## Verdict up front

1. **General answer: the hardware mirror/repeat period is the PADDED size, not the
   logical size.** For any non-pow2 extent with REPEAT or MIRRORED_REPEAT the seam is
   displaced by `padW/width` — a real, latent hazard the backend already acknowledges.
2. **For the machine shadow it is provably NOT the bug.** 32 and 64 are powers of two:
   `NextPow2` is the identity, `uScale = vScale = 1.0`, and the hardware seam sits
   exactly at the logical edge. On top of that, the shipped quad UVs are strictly
   INTERIOR to the first period, and on real N64 the CLAMP bit neuters the MIRROR bit
   for this tile anyway (clamp-before-mask, window == one period).
3. **The clamp override (f1247a9) does not misclassify mirror-only axes**, and for the
   shadow's MIRROR|CLAMP axes it makes the 3DS sampling N64-exact where the previous
   code (unclamped MIRRORED_REPEAT) had a small mirrored fringe divergence. The livery
   fix plausibly *improved* the shadow; the "wrong shape" report predates it.
4. Residual suspects for any still-wrong shape are OUTSIDE the texture domain
   (geometry/billboard first — see the ranking at the end). The sampling stack is
   exonerated three independent ways (static trace 56579ac, measured [shadow] dump
   receipts, and this pass's asset-byte ground truth).

## The chain, with cites

### 1. What the game asks for (decomp, ground truth)

`src/game/racer.c:5880-5885` (rivals + player), `:5909` (crashed), `:7145`
(func_8009CD60, per-machine):

```c
gDPLoadTextureBlock_4b(gfx++, D_800CDC54[racer->shadowType], G_IM_FMT_I, 32, 64, 0,
                       G_TX_MIRROR | G_TX_CLAMP, G_TX_MIRROR | G_TX_CLAMP, 5, 6,
                       G_TX_NOLOD, G_TX_NOLOD);
```

- I4 32x64, LOADBLOCK (transferred as 16b, width-1 sentinel; dxt for 2-word lines).
- BOTH axes `G_TX_MIRROR|G_TX_CLAMP`, masks 5/6 → mask periods exactly 32 and 64.
- The macro sets the tile window to exactly the texture:
  `lrs = (32-1)<<2 = 124`, `lrt = (64-1)<<2 = 252` → window = one mask period.
- Combiner MODULATEIA_PRIM, rendermode `G_RM_ZB_XLU_DECAL`, prim alpha 200.
- 4-vertex quad `D_800CDBA4[shadowType]`, 2 tris.

**Quad UV ground truth** (this pass; `machine_custom_gfx/D_3004F18` extracted from the
prebaked `fzerox.o2r`, LUS Vtx records, S10.5 texcoords):

| vtx | x    | z    | s (10.5) | t (10.5) | s texels | t texels |
|-----|------|------|----------|----------|----------|----------|
| 0   |  170 | -250 | 970      | 249      | 30.31    | 7.78     |
| 1   | -180 |  310 | 27       | 1799     | 0.84     | 56.22    |
| 2   |  170 |  310 | 970      | 1799     | 30.31    | 56.22    |
| 3   | -180 | -250 | 27       | 249      | 0.84     | 7.78     |

Normalized span: **S ∈ [0.026, 0.947], T ∈ [0.122, 0.878] — strictly inside one
period.** For this machine neither MIRROR nor CLAMP ever engages; the wrap mode is
irrelevant to the rendered shape. (The earlier on-device `[shadow]` dump measured a
~1.075 overhang on one axis for a different `shadowType`'s quad — see §4 for why that
is also N64-exact now.)

### 2. Importer decoded extent (libultraship, patched stack)

`src/fast/interpreter.cpp` `ImportTextureI4` (≈1585-1706):

- `width = widthBytes * 2` with `widthBytes` from `GetEffectiveLineSize(...)` (≈1014):
  LOADBLOCK records `line_size_bytes == full_image_line_size_bytes == size_bytes`
  (1024) on the first-ever load, so the function falls back to the RENDER tile's
  `line_size_bytes` = 2 words = 16 bytes → **width 32**; `height = 1024/16 = 64`.
- Tile-window crop (≈1616-1627): `tileWidth = (124-0+4)/4 = 32`,
  `tileHeight = (252-0+4)/4 = 64` — equal, crops nothing.
- `ApplyTileMaskExtent` (≈1046-1069): CLAMP set on both axes → `min(extent, 1<<mask)`
  = min(32,32) x min(64,64) — no change.
- **Decoded extent = 32x64, alpha = intensity** (a = SCALE_4_8(nibble), ≈1666-1672).
  Confirmed by host test `gdx_gfx_i4_extent_tests` (feat/3ds-shadow @ 56579ac).

### 3. pow2 padding, uScale/vScale, PICA coords (port backend)

`port/3ds/gfx/gfx_citro3d.cpp` `UploadTexture` / `DrawTriangles`:

- Padding + rescale (1425-1426): `t.uScale = width/padW`, `t.vScale = height/padH`.
- UV repack (1723-1724, 1738-1739): `out = src * uScale` — the interpreter's
  logical-normalized UV (`u / tex_width`, interpreter.cpp:4056-4057, where
  `tex_width`/`tex_height` are the LOGICAL extents above) is mapped into
  `[0, width/padW]` of the padded texture.
- Wrap modes are applied to the PADDED `C3D_Tex` (`WrapFromN64` 539-546; upload-time
  1454-1456; per-draw override 1690-1695). **PICA wraps/mirrors at padded-normalized
  1.0** — i.e. at `u_logical = padW/width`.

**Hence the general answer: mirror period = PADDED size.** The backend knows
(1452-1453): "REPEAT/MIRROR on pow2-padded non-pow2 textures wraps over the padding".
Additionally the V axis stores rows bottom-up with logical row 0 at padded row
`padH-1` (1399-1417), so for a non-pow2 HEIGHT the mirror band at T=1 reflects into
the (unwritten) padding rows, not into the logical image — a second face of the same
displacement.

**For the shadow: `NextPow2(32)=32`, `NextPow2(64)=64` → padW==width, padH==height,
uScale=vScale=1.0 → the seam sits exactly at the logical edge.** No displacement
exists to distort anything, independent of the interior-UV argument.

### 4. Wrap-mode selection and the f1247a9 clamp override

- The interpreter strips `G_TX_CLAMP` and requests SHADER-side clamping whenever the
  clamp cannot ride the plain wrap mode — for MIRROR|CLAMP axes unconditionally
  (interpreter.cpp:3837-3843: `(cms & G_TX_CLAMP) && ((cms & G_TX_MIRROR) || ...)`),
  appending per-vertex clamp limits `(tex_width2 - 0.5f)/tex_width` (4142-4147).
- **Mirror-only axes never take that branch** (the guard requires the CLAMP bit), so
  f1247a9 cannot misclassify them: they still resolve `GPU_MIRRORED_REPEAT` via
  `WrapFromN64`. No misclassification exists.
- The 3DS backend cannot consume the per-vertex limits (no fragment shader); f1247a9
  forces `GPU_CLAMP_TO_EDGE` on exactly the shader-clamped axes at bind time
  (gfx_citro3d.cpp:1664-1695).
- **N64 semantics for the shadow tile:** the RDP clamps to the tile window BEFORE the
  mask engages. Window = `lrs/4 = 31.0` texels ≤ mask period 32 → every coordinate is
  clamped into `[0, 31.0]` and **the MIRROR bit never fires on real hardware**. The
  observed ~1.075 overhang on some shadowTypes' quads therefore samples the edge
  texel on N64 — and `GPU_CLAMP_TO_EDGE` does exactly the same. Post-f1247a9 the 3DS
  is N64-exact for this draw; pre-f1247a9 (unclamped MIRRORED_REPEAT) the overhang
  band mirrored texels 29.6..32 instead — a small fringe divergence, now gone.
- **One documented nuance** (comment amended in gfx_citro3d.cpp): the livery-fix
  rationale "the mirror we drop never fires inside the window" holds only while the
  tile window ≤ one mask period. A MIRROR|CLAMP tile whose window EXCEEDS its mask
  period would mirror inside the window on N64 but edge-smear on the 3DS. No such
  tile is currently known in F-Zero X (the shadow's window == period; the livery
  decals' windows are narrower), so no code change is made blind; if one surfaces,
  the fix is a mirror-unfolded upload (e.g. 32→64, then CLAMP at the unfolded edge —
  note a plain unfold does NOT fix non-pow2 wrap in general, since the unfolded size
  must itself be pow2 for the PICA).

## Current-state assessment (no emulator this pass)

- Worktree: patch stack applied and roundtrip-consistent; the dead spawn left no
  shadow-specific edits (submodule diffs == the 26-patch stack).
- Scanouts: `docs/research/livery2-fixed-drive{1,2}.png` (post-livery-fix build) show
  no obviously distorted shadow; drive1 has no discernible blob under the craft from
  its high camera (the dark diagonals are the track's X bands; the black rectangle
  right of the ship is the km/h HUD box), drive2 shows plausible soft darkening under
  the machine. The `autotest-final1` drive BMPs are fog-bug-era (Aug 14) and useless
  as a baseline. A fresh eyeball (user display, not `_scan` oracle) on the current
  build should precede any further shadow work.

## Residual ranking if the shape is still wrong on-screen

1. **Geometry** — the locked look-at billboard modelview
   (`Matrix_SetLockedLookAtFromVectors`, decomp src/sys/math.c:698;
   scaleY=0.1 flattener; per-racer `gGfxPool->unk_20A88`). SHADOW-1's endpoint
   (e4d7dc3 on feat/3ds-shadowgeo): static pack/unpack is ILP32-clean; the
   `[shadowgeo]`/`[shadowmtx]` probes are authored there but WERE NEVER RUN — run
   them first next emulator window.
2. **Decal depth partial-fail** — a cut-up blob would read as "wrong shape"; the
   0.004 bias was emulator-verified only at two camera angles.
3. Combiner alpha path is considered exonerated: I4 body textures ride the same
   TEXEL0×PRIM/ENV modulate stages and render correctly post-primenv-flush.

## Regression guard added this pass

`port/tests/gfx_shadow_uv_tests.cpp` (host exe `gdx_gfx_shadow_uv_tests`) pins the
whole contract with the real asset numbers: decoded extent 32x64, identity pow2
padding (seam at the logical edge), machine-0 quad UV interiority, the N64
clamp-before-mask window bound (mirror inert), and the clamp-strip/tm-bit routing
that f1247a9 consumes. Any future change that shifts uScale/vScale, the decode
extent, or the wrap classification for this tile trips the test.
