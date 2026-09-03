# TRILOOP — packed PICA-layout vertex emission (feat/3ds-triloop)

2026-08-27. Precedent: Wyatt-James/sm64-3ds-port tri-loop optimization (measured
~0.8 ms on their tri path, 5.6 → 4.8 ms: fold the gfx_pc→gfx_citro3d vertex
repack into the first write). This branch implements the same shape for our
LUS-interpreter tri path.

## What changed

- `lus-3ds-triloop-packed-vbo.patch` (closes the lus series): GfxSpTri1's vertex
  append, `__3DS__`-gated. At each batch's first tri it latches backend pack
  parameters + a linear-VBO-pool write cursor (`gdx3ds_vbopack_begin`) and emits
  every vertex ONCE in the final 12-float PICA layout (UV pow2 scale + shader-
  clamp and content-edge clamp applied at append; only `vtxColorInput` evaluated
  per vertex). A per-batch aux record carries what the fixed layout cannot:
  vertex-0 RGBA of every combiner input, fog rgb, vertex-0 fog factor.
- Port side (committed directly): `gdx3ds_vbopack.h` contract,
  `VboPackBegin`/`DrawPackedBatch` + `RefreshStageConstantsPacked` +
  `UpdateFogState` aux path in gfx_citro3d.cpp; `dPk=` receipt on the [c3d]
  line; optmask bit4.
- Fallback: begin refuses ([debug] triloop=0 / GDX_TRILOOP_OFF, no shader or
  inactive frame, pool < one 256-tri batch of headroom, geo diagnostics armed)
  → the whole batch takes the fully-compiled legacy variable-stride + repack
  path. Verbose UV/geometry diagnostics therefore keep legacy fidelity.

## Why the emitted VBO is bit-identical

Same expressions in the same float op order as legacy append-then-repack
(`fminf(normUV * scale, clamp)` etc.), and every value the aux record hoists to
batch scope was ALREADY batch-constant by the renderer's flush invariant
(prim/env value-change flush, fog moveword flush, sampler/shader/texture flush):
the backend only ever read vertex 0 of the batch for them.

## Reasoned delta (emulator lock contended — /tmp/azahar.lock held by a
## concurrent agent for the whole session; measure on the next free window)

Per-vertex cost removed, typical race material (1 texture, fog, 2 combiner
inputs, alpha; legacy stride 18 floats):

- DrawTriangles repack loop gone: ~12 loads + 12 stores + 2 fminf + 2 muls +
  per-field branches per vertex ([prof] drw bucket).
- Append side: ~6 fewer float stores per vertex (18 → 12) and the discarded
  per-vertex evaluation of each constant-spill input (mux switch + 3-4 uint8→
  float divides per input per vertex) drops to once per batch ([prof] tri
  bucket).
- UpdateFogState's exact=0 per-vertex secant scan replaced by a=0, b=factor
  (shroud draws only; small).

At the crowd profile's ~860 tris/frame (2580 verts, ~150 draws): ~65-70k
eliminated memory ops + ~5k fminf + ~5k discarded-input divides per frame. On
the 268 MHz ARM11 (in-order, VFPv2 fdiv ~19cy non-pipelined) that reasons to
~0.5-1.2 ms off drw and ~0.3-0.8 ms off tri per crowd frame — consistent with
the sm64-3ds-port precedent's measured 0.8 ms on a lighter tri format. Steady
menus (texrect-dominated, all through the same append) shrink proportionally.

## Verification state

- 3DS build green (interpreter.o + G-Diffuser-3DS.3dsx relinked), patch
  roundtrip-verified against the full 36-patch stack.
- NOT emulator-verified: singleton lock held by another agent throughout.
  Next session: [prof] tri/drw + [profop] 06 A/B on a race-reaching run
  (gputrace=1), triloop=1 vs [debug] triloop=0 same build — the killswitch
  makes the A/B one INI flip; `dPk` on [c3d] must track the draw volume when
  the packed path is live. Visual sweep: machine-select (constant-spill
  inputs), race fog fade, shroud mode, sky wedge (clamp family), HUD rects.
