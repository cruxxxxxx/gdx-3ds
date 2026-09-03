# Is per-frame display-list translation the only way? (deep-research, 2026-09-01)

Question: for an N64 decomp on New 3DS, is the sm64-port / libultraship "walk the Gfx
stream every frame" model really the most performant architecture, and what else could
reach a locked 60?

Method: 5 search angles, 15 sources fetched, every claim adversarially verified by 3 voters
against primary sources (code, PR bodies, commit messages). 99 agents. Only 3-0 / 2-1
survivors are reported. One claim ("painfully slow divides in gfx_pc.c") was refuted 0-3.

## What the evidence says

1. **Every shipped N64-decomp renderer re-walks the Gfx stream on the CPU every frame.**
   n64-fast3d-engine, sm64-port, the mkst/Gericom/Wyatt-James sm64 3DS port, libultraship's
   gfx_pc lineage, and RT64 all do. None retain translated display lists or vertex buffers
   across frames. The only cross-frame caches are shader-program and texture pools.
   (Verified in source: `gfx_run_dl` opcode switch to `G_ENDDL`; 1 MB linearAlloc VBO rewound
   to 0 every frame in `gfx_citro3d.c`.) [1][2][3]
2. **No verified precedent for a compile-time GBI shim** (redefining gSP*/gDP* macros to emit
   native backend calls) in sm64ex-coop, Perfect Dark, Ship of Harkinian, Turok, or the 3DS
   ports. Absence of evidence, not proof it was never tried. Same for asset-time baking of
   static geometry and for cross-frame mesh caching.
3. **In the Fast3D/3DS lineage, RSP transform + lighting runs on the CPU** (`gfx_sp_vertex`:
   per-vertex MP-matrix multiply, per-light dot products, texgen, fog). The 3DS PICA200
   vertex shader is pass-through: it only applies a fixed screen rotation and the stereo
   shear. [2]
4. **The sm64 3DS port's own optimizer measured vertex/triangle processing at ~21 ms/frame
   (old 3DS)** and called it "the last barrier keeping o3DS from maintaining 60fps", with
   "moving vertex processing to a vertex shader" as the best remedy. As of 2026-09 nobody has
   shipped that on 3DS. [4]
5. **Interpreter restructuring gives modest, measured gains:** Wyatt-James's 2026-08 commit
   (zero-copy Vtx pointers into the backend, dedicated vtx/tri loop, flush sites 31 -> 3)
   reports 5.6 -> 4.8 ms on the RSP bucket (~14%). [5]
6. **RT64 (N64Recomp) moves all RSP vertex work to a GPU compute shader and defers the whole
   frame's draws**, which its authors credit for removing the CPU bottleneck. It still parses
   the display list on the CPU every frame. It needs D3D12 SM6 / Vulkan 1.2 / Metal, compute
   shaders and bindless textures. **Does not transfer to PICA200** (no compute, fixed-function
   fragment stage). [6][7]
7. RT64's author independently corroborates CPU vertex transform as the scaling wall: "every
   vertex you saw on the screen was transformed by the CPU ... up to 20 or 30 milliseconds". [8]
8. The sm64-port 3DS "60fps" patch **doubles** interpreter work per game tick (renders the
   same SPTask twice with patched matrices). Not a cost reducer. [9]

## Ranked assessment from the research (for a New 3DS port)

1. GPU vertex T&L in the PICA200 vertex shader — highest expected value, moderate risk,
   unshipped by anyone.
2. Interpreter micro-architecture (tri loop, flush elimination, zero-copy vertices) — low
   risk, ~10-15% of the RSP bucket.
3. Retained-mode / baked static geometry — plausible, no precedent, validity model is the
   whole risk.
4. RT64-style compute/bindless — not feasible on this GPU.

## How G-Diffuser 3DS already compares (our state, not from the research)

- Item 1 was built here first: `feat/3ds-gputransform` (unlit phase 1) works, emu vtx -62%,
  but HW-imperceptible at phase-1 scope and a floor-texture precision bug; shelved with a
  documented phase-2 path (lit machine geometry). See gputransform-report.md.
- Item 2 is already on mainline: lus-3ds-triloop-packed-vbo, lus-traffic-pipesync-noop,
  lus-s7-tri-state-memo, lus-vtx-mtx-hoist, lus-flat-dispatch.
- Our 2026-09-01 census (bridgecache-progress.md): 87% of walked commands per crowd frame are
  host-built by the game's C code each frame; only 13% come from static ROM lists. So a
  cross-frame translated-list cache (item 3) has a ~1.5 ms ceiling here and was dropped.
  A GBI shim would attack exactly that 87%, but it removes only the bridge pre-pass
  (~11.5 ms emu, being cut by micro-opt now), not the interpreter/dispatch (~40 ms emu).
- The interpreter bucket on our profile is dominated by per-draw and per-tri cost
  (~150 draws, ~1100 tris per crowd frame; stereo doubles draws), i.e. batching, not
  translation. Texture atlasing (137 machine-texture switches/frame) is the unexplored
  lever there.

## Sources

[1] https://github.com/AloXado320/n64-fast3d-engine
[2] https://github.com/mkst/sm64-port (branch 3ds-port: gfx_pc.c, gfx_citro3d.c, shader.v.pica)
[3] https://github.com/Wyatt-James/sm64-3ds-port
[4] https://github.com/mkst/sm64-port/pull/85
[5] https://github.com/Wyatt-James/sm64-3ds-port/commit/a12d55a8654c95b756485ca9f10169caae25f684
[6] https://github.com/rt64/rt64
[7] https://github.com/Zelda64Recomp/Zelda64Recomp
[8] https://softwareengineeringdaily.com/2024/10/02/n64-recompiled-with-dario-and-wiseguy/
[9] https://github.com/mkst/sm64-port/blob/3ds-port/enhancements/60fps.patch

Caveats: all performance figures are single-developer self-reports, mostly old 3DS, for
SM64 not F-Zero X. The PICA200 T&L magnitude is inferred from bottleneck attribution, not
measured by anyone.
