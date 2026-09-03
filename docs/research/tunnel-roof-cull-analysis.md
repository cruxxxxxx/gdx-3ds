# Tunnel-roof missing — face-cull hypothesis ruled out

**Branch:** `feat/3ds-cull` (off `feat/3ds-m1`, HEAD 2024af3)
**Symptom (human-observed):** "opening tunnel doesn't have a roof" on the 3DS build.
**Suspected root cause under test:** the N64 `G_CULL_FRONT`/`G_CULL_BACK` geometry-mode
bits are mapped with the wrong winding sense into the citro3d/PICA200 face-cull state, so
roof triangles that should be visible get culled — possibly because the portrait fixup
matrix inverts triangle winding relative to the cull sense the backend assumes.

**Verdict: the face-cull hypothesis is RULED OUT.** There is no `G_CULL_*` → PICA
cull-face mapping to be wrong, and the portrait fixup matrix does not invert winding.
The missing roof is a different defect (see "Next suspect").

---

## 1. Culling on the 3DS runs on the CPU, not the GPU

`port/3ds/gfx/gfx_citro3d.cpp` sets face culling to **off** at init and never changes it:

```
port/3ds/gfx/gfx_citro3d.cpp:533
    C3D_CullFace(GPU_CULL_NONE); // Fast3D interpreter culls on the CPU
```

There is no `C3D_CullFace(GPU_CULL_FRONT_CCW/BACK_CCW)` mapping anywhere in the backend.
The PICA200 rasterizer is never asked to cull, so a wrong-winding face-cull mapping
cannot exist. The task's assumed mechanism (an inverted `C3D_CullFace(GPU_CULL_...)`
mapping) is not present in the code.

The real backface cull is the CPU cross-product test in the Fast3D interpreter:

```
libultraship/src/fast/interpreter.cpp:2937-2985  (Interpreter::GfxSpTri1)
    cull_both  = get_attr(CULL_BOTH);
    cull_front = get_attr(CULL_FRONT);
    cull_back  = get_attr(CULL_BACK);
    if (geometry_mode & cull_both) {
        cross = (dx1*dy2 - dy1*dx2)              // screen-space signed area, x/w,y/w
        if ((v1->w<0) ^ (v2->w<0) ^ (v3->w<0)) cross = -cross;     // one vtx behind eye
        if (extra_geometry_mode & G_EX_INVERT_CULLING) cross = -cross;
        cull_front:  reject if cross <= 0
        cull_back:   reject if cross >= 0
        cull_both:   reject
    }
```

This block is **stock libultraship**, byte-for-byte identical to the desktop path:

- No `#ifdef __3DS__` / `_3DS` anywhere in `src/fast/interpreter.cpp`.
- `git -C libultraship log/diff` shows the cull region unmodified across recent history;
  the only recent interpreter change on this fork is an unrelated ImGui touch-input fix.
- None of the 6 libultraship patches under `port/3ds/patches/` touches this cull block
  (`lus-3ds-fog-exact-params` extends a *different* `#ifdef __3DS__` hook in the same
  file — the fog LUT bind — not the cull).

So whatever the CPU cull decides on the 3DS, it decides identically on desktop, where the
tunnel roof is present. A cull-sense bug would have to be desktop-visible too. It is not.

## 2. The portrait fixup matrix does NOT invert winding

The fixup matrix is applied on the **GPU, in the vertex shader**, via the `projection`
uniform — *after* the CPU cull has already run on the raw N64 clip coords:

```
port/3ds/gfx/gfx_citro3d.cpp:5      "v0: position x,y,z,w — interpreter clip space, fixed up by the projection [shader]"
port/3ds/gfx/gfx_citro3d.cpp:524-530  mFixupMatrix:  x'=y, y'=-x, z'=z-w, w'=w
port/3ds/gfx/gfx_citro3d.cpp:530/587  C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, projection, &mFixupMatrix)
```

Two independent reasons this cannot be the cull bug:

1. **Ordering.** The interpreter's cull cross-product (§1) reads `v->x/v->w`, `v->y/v->w`
   — the game's own MVP clip output, *before* the fixup matrix. The fixup is a GPU
   uniform consumed later in the vertex shader. It literally cannot affect a decision that
   was already made on the CPU. GPU cull is `NONE`, so the post-fixup winding is never
   tested against anything.

2. **The fixup preserves orientation anyway.** Its xy block is `[[0,1],[-1,0]]` — a pure
   +90° rotation (landscape→portrait). `det = 0·0 − 1·(−1) = +1`. A positive determinant
   means screen-space triangle winding is preserved, not flipped. Even if the GPU *were*
   culling, CPU-cull and rasterizer would agree. (The `z'=z−w` row only remaps depth for
   the reversed-depth `C3D_DepthMap(true,-1,0)` setup and does not affect xy winding.)

So the "fixup inverts winding, so the cull sense must be opposite" premise is false on both
counts.

## 3. The existing `gdx-nocull` kill-switch is a different layer (and already exonerated)

`gdx_nocull_test_enabled()` (`port/n64_sched.c:580`, patch
`decomp-race-cull-diagnostics.patch`) is **not** the RSP triangle backface cull. It
force-passes the decomp game-logic *course-chunk NDC frustum test* in `course.c`. Prior
debugging already used it to prove the course cull is correct end-to-end and that the
invisible-road defect was the fog LUT misbind — fixed by `lus-3ds-fog-exact-params.patch`
(see `port/3ds/patches/README.md` and `docs/research/m1-boot-debug.md`). It says nothing
about, and does not touch, the per-triangle backface cull that this task suspected.

## Risk analysis (had a cull-sense flip been applied)

Recorded for completeness, since the deliverable asks whether flipping the cull sense
could hide *other* surfaces: yes, and dangerously so. The interpreter cull is global —
`cull_front`/`cull_back`/`G_EX_INVERT_CULLING` apply to every triangle in every display
list, not per-mesh. Negating `cross` (or flipping the `<=0`/`>=0` comparisons, or setting
`G_EX_INVERT_CULLING`) would reveal the tunnel roof only by simultaneously culling every
surface that is currently correct — every road face, wall, and machine body drawn with
`G_CULL_BACK` would invert and vanish, trading one hole for the whole rest of the scene.
This is exactly why the correct sense must match the RSP, and why "make the roof appear by
flipping cull" is not a viable fix even if cull were the cause. It is not the cause, so no
such change was made.

## Next suspect (cull excluded)

The roof is present on desktop and absent on 3DS through an identical CPU cull, so the
divergence is downstream of cull. In priority order:

1. **Near-plane / homogeneous clip against the reversed-depth setup.** The 3DS uses
   `C3D_DepthMap(true, -1, 0)` + `GetClipParameters() z_is_from_0_to_1 = true`
   (`gfx_citro3d.cpp:13-16, 532`). The interpreter clips `z < -w` (near) but the roof is
   geometry directly overhead and very close to the eye at the tunnel mouth — a candidate
   for near-plane rejection or a z-range/w-sign mismatch that only bites the 3DS clip
   path. Inspect `clipDistance` plane 5 (`z+w`) and the `w <= 0` whole-triangle drop
   (`interpreter.cpp:2419, 2822, 2937`) with the roof's actual clip coords.
2. **Depth test / decal bias.** `SetDepthTestAndMask` maps N64 LEQUAL→GEQUAL for reversed
   depth (`gfx_citro3d.cpp:983-996`). A roof coplanar/behind the sky or a decal-bias sign
   error could z-fail the roof specifically.
3. **Material A/B isolation.** Use the in-tree `GDX_DIAG_SKIP_COMBINE`
   (`interpreter.cpp:~2825`) to identify which material owns the roof, then confirm whether
   it is culled (`trianglesCullRejected`), clipped (`trianglesClipRejected`), or drawn but
   z/blend-failed — this discriminates the three suspects above directly.

## Verification performed this pass

- Applied all 11 submodule patches cleanly (6 libultraship + 5 decomp).
- Configured + built the full 3DS target with the devkitARM toolchain:
  `cmake -S . -B build-3ds -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/3DS.cmake
  -DGDX_PLATFORM_3DS=ON` then `cmake --build build-3ds -j8` →
  `G-Diffuser-3DS.3dsx` + `gdx3ds-dl-tests.elf` built green (warnings only, no errors).
  This exercises `gfx_citro3d.cpp` and the patched `interpreter.cpp`.
- **No source change was made** (cull hypothesis disproven, not fixed). Submodule working
  trees hold only the standard patch set and were NOT committed.
- Visual confirmation of the roof is **pending an emulator pass** (emulator is a shared
  singleton owned by another agent this session; not run here).
