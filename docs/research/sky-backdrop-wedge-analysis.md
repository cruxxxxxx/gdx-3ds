# Distant-sky black wedge — backdrop-coverage analysis (NOT a fog regression)

**Branch:** `feat/3ds-sky` (off `feat/3ds-m1`, HEAD 5758075)
**Symptom (human-observed, Mute City in-race):** a hard **black triangular wedge** in the
distant sky, top-center/right, between the (correct) pink haze and the purple Mute City
building skyline. Appeared right after the per-vertex fog fix landed on `feat/3ds-fog`.

**Verdict:** the black wedge is the **framebuffer clear colour** (`0x000000FF`, opaque black —
`gfx_citro3d.cpp` `C3D_RenderTargetClear`) showing through **above the backdrop skybox quad**,
because the skybox quad is built at 4:3 logical proportions but the 3DS top screen is 400×240
(~16:10) after the portrait fixup, so the quad's top edge falls short of the screen edge.

It is **NOT a fog regression.** The fog fix did not create the black region — it **unmasked**
it. Ruled the three task hypotheses as:

- **H1 (fog over-blends sky to black): RULED OUT as the cause of the black.** The sky IS being
  fog-blended, but it saturates to the **fog colour (pink `fdc0fc`)** at far depth, not black —
  and that pink far-sky is **correct / N64-faithful** (the RSP fogs the skybox too). Touching the
  fog blend to "fix" the sky would regress the road fog that now works. Do not.
- **H2 (clip / black clear-colour shows through): CONFIRMED (coverage flavour, not clip).** The
  quad under-covers the top; clear colour bleeds above it. It is *not* a near/far-plane triangle
  clip (the skybox sits at far depth, `skyboxDepth` 5300–7000) — it is vertical **coverage**.
- **H3 (draw order / depth-write): RULED OUT.** The black is where nothing draws at all, so no
  ordering or z-write interaction is involved.

---

## 1. Decisive evidence — the black exists with fog OFF too

Captured scanouts (Azahar, in-race Mute City), column x=300, y top→horizon:

| y (px) | `autotest-fogfix` (per-vertex blend ON) | `autotest-nofog` (PICA fog unit OFF) |
|---|---|---|
| 8–14 (top) | `(0,0,0)` **black** | `(0,0,0)` **black** |
| 56+ (below skybox top edge) | flat pink `(252,192,253)` | dark-teal gradient `(34,49,49)` |

The **black at the top is identical in both** — present with fog fully off. So no fog draw
produces it. What the fog fix changed is only the region *below* the skybox top edge: dark-teal
skybox gradient → flat pink (the fog blend to `fogColor`). Because bright pink now borders the
black-above-skybox, the previously-camouflaged black gutter reads as a stark "wedge."

The pink is a **perfectly flat** `(252,192,253)` = fog colour `fdc0fc`, not a textured gradient
(a 64×1 CLAMP strip would vary/dither). That flatness is the fog blend saturating to `fogColor`
at far depth (`f→1`), which is the intended, N64-faithful behaviour.

Full pixel maps: `/tmp/fogfix_drive{1,2}_scan.png`, `/tmp/nofog_drive2_scan.png` (regenerate with
`sips -s format png <sdmc>/autotest-fogfix/drive2_scan.bmp --out …`).

## 2. Why the quad under-covers the top (root cause, in decomp geometry)

`decomp/src/overlays/ovl_i3/background.c`:
- `Background_Draw` (1069): loads 28 verts, draws the skybox as **2 triangles**
  `gSP2Triangles(gfx++, 8, 11, 9, 0, 8, 10, 11, 0)`, textured with `sSkyboxTexture` (64×1 RGBA16).
- `Background_InitBackgroundInfo` (568): `aspectRatio = camera->fovScaleY / camera->fovScaleX`.
  On both N64 and 3DS `fovScaleX/Y = SCREEN_WIDTH/HEIGHT = 320/240` (`camera.c` 2325+), so
  `aspectRatio = 0.75` (4:3) — **unchanged on 3DS**.
- `Background_Update` (626): `verticalRange = horizontalRange * aspectRatio`. There is already a
  `PORT`/widescreen `hor+` compensation (600–644) driven by `gdx_get_widescreen_geometry_xscale()`
  that widens `horizontalRange` and caps `verticalRange` for ultrawide — but that path returns
  **1.0 unless desktop widescreen is explicitly enabled** (`interpreter.cpp:2408`).
- `Background_UpdateSkyboxVtx` (700): builds the quad at ±`verticalRange` / ±`horizontalRange`
  around `skyboxDepth`. Verts carry `cn = {0,0,0,255}` (black), but the combiner is TEXEL0-driven
  so vertex colour is not the black source.

The quad is therefore sized for a **4:3 logical frame**. The 3DS renders **portrait** (240×400)
then rotates via the vertex-shader fixup (`x'=y, y'=-x`, `gfx_citro3d.cpp:526-533`) onto the
400×240 top screen (~16:10, wider than 4:3). Nothing in the backdrop sizing knows about the 3DS's
non-4:3 final display, so the quad's top/bottom edges land inside the screen → clear colour above.
The edge sweeps with camera pitch (basis vectors in `UpdateSkyboxVtx`), which is why the wedge
looks triangular at one camera angle and a curved band at another across the drive captures.

## 3. Ownership / coordination

- **This is decomp backdrop *geometry*, not the fog TexEnv layer.** The correct fix lives in the
  skybox vertical-coverage math (`Background_Update` `verticalRange` / `Background_UpdateSkyboxVtx`)
  — extend the existing `PORT` widescreen compensation to also cover the **3DS non-4:3 rotated
  display** so the quad overscans the top screen edge (the strip samples CLAMP, so vertical
  overscan is free, exactly as the existing horizontal `1.02f` margin comment notes at 604–606).
  Not landed here: it re-frames the whole backdrop and must be validated on the emulator, and it
  overlaps the projection/portrait-fixup domain owned by the geometry/roof effort.
- **Distinct from the tunnel roof** (`tunnel-roof-depth-clip-analysis.md`, `GDX_DIAG_ROOF`). The
  roof is *interior overhead* geometry dropped by the near-plane clip at the tunnel mouth; the sky
  wedge is the *backdrop* skybox under-covering the top. Different geometry, different mechanism,
  different draw. No overlap in the diagnostics (`[sky]` vs `[roof]`).
- **Do NOT change the fog blend** (`MapCombiner` `wantFogBlend` / `DrawTriangles` fog gate). The
  pink far-sky is correct; the road fog depends on that exact path.

## 4. Scoped diagnostic landed (this branch)

`port/3ds/gfx/gfx_citro3d.cpp` `DrawTriangles`, `__3DS__`-gated + `GDX_DIAG_SKY=1` env + race-gated.
For every far-depth 2-triangle draw (the skybox/floor signature: `zw>0.80`) it logs the post-fixup
**screen NDC** bounding box and the gap to each screen edge:

```
[sky] tris=2 tex=1 zw=[0.990,0.998] ndcX=[-0.83,0.83] ndcY=[-0.71,0.71] topGap=0.290 botGap=0.290 fogBlend=1
```

Fixup maps screen_y' = −clip_x/w, screen_x' = clip_y/w (`mFixupMatrix` r0.y=1, r1.x=−1).
`topGap = 1 − ndcYmax`, `botGap = 1 + ndcYmin`. Cost when off: one `getenv` + a `bufVboNumTris==2`
branch/draw. Desktop untouched (whole block `#ifdef __3DS__`).

## What the next emulator pass should look for

Run a scripted Mute City race with `GDX_DIAG_SKY=1` and grep the log for `[sky]`:

- **Confirms this diagnosis** if the skybox line shows `fogBlend=1`, `zw≈[0.99,1.0]`, and a
  **positive `topGap`** (e.g. ~0.2–0.3) — the quad's top NDC edge stops short of +1, leaving that
  fraction of the screen as clear-colour black. `botGap` will likely mirror it (symmetric quad).
- The `ndcX` range should reach ~±1 (the existing horizontal `hor+`/1.02f margin), confirming the
  gutter is **vertical only** — i.e. the fix is to widen `verticalRange` for the 3DS display, not
  the fog.
- Cross-check: the pink region below the top edge should carry `fogBlend=1` and the wedge above it
  should be exactly the `topGap` band. If instead `topGap≈0` (quad reaches the edge) the wedge is a
  *different* defect and this diagnosis is wrong — but the with-fog-off capture (§1) already makes
  that unlikely.

---

# 2026-08-27 SKY-WEDGE-3 — SUPERSEDING VERDICT: texture-padding sampling, not coverage

**Branch:** `feat/3ds-skywedge3`. The user's new observation ("it looks like a weird
texture almost — maybe the fog or cloud shader?") reframed the hunt, and a padding-fill
discriminator settled it. The 2026-08-20 "clear-colour-above-the-quad" verdict above
explains only the fixed top letterbox band; the MOVING black wedge the user photographs
is something else entirely:

## Root cause

`UploadTexture` (port/3ds/gfx/gfx_citro3d.cpp) pads every texture to PICA pow2/min-8
dimensions and zeroes the padding (black, alpha 0). N64 tile clamping is expressed as
`GPU_CLAMP_TO_EDGE` — which clamps at the **padded** edge, not the content edge. Any
clamp-requested axis whose interpolated UV overshoots the content therefore samples the
zeroed padding instead of the N64's edge texel: black RGB, and alpha 0 (so blended draws
punch through to the black clear colour — the wedge stays black even where the fog blend
saturates the sky to pink).

The in-race backdrop is the extreme case: the sky gradient strip is **64x1**
(`gDPLoadTextureBlock(..., 64, 1, ..., G_TX_CLAMP, ...)` in
`decomp/src/overlays/ovl_i3/background.c:1103`, padded to 64x8, vScale = 1/8), and
`Background_UpdateSkyboxVtx` leaves the T axis mathematically wild —
`textureTCoordinates = ((xPos[i]-xPos[0])/(2*horizontalRange) * 512) - 0.5`, up to
~±600 texels, harmless on N64 where the tile's lrt=0 window clamps every T to the single
row. On the 3DS the wild T walks into the padding. The ±32000 vertex-position clamp
(the same one that killed the verticalRange overscan idea) bends the four corner T
values non-planarly with camera pitch/yaw, so the padding region is a hard-edged
polygon that moves and spreads with the camera and splits along the quad diagonal —
exactly the photographed artifact, and literally "a weird texture" (it IS one).

## Receipts (Azahar, scripted Mute City drive, this directory)

- **Run A** (`[debug] sky_clamp_fix=0 diag_padfill=1` — fix OFF, padding flooded OPAQUE
  MAGENTA): `runA-fixoff-padfill-drive{2,5}_scan.bmp` show a magenta region in the sky at
  (15,16)-(332,88) — top/top-left, over the pink haze, growing across the drive shots
  (55 px → 4957 px). Pure (255,0,255) near the top, fog-tinted magenta lower down —
  matching how the real (zeroed) padding renders black at the top and disappears into
  the saturated fog pink below. This is the user's wedge, made visible.
- **Run B** (fix ON, padfill still magenta): `runB-fixon-padfill-drive{2,5}_scan.bmp` —
  magenta-detector counts collapse to 52-55 px on every drive shot, and sampling those
  pixels shows they are (197,65,197)-class PURPLE CITY-SKYLINE CONTENT, byte-identical
  between runs (detector false positives), not (255,0,255) padding. A/B per shot
  (A -> B): drive2 2963 -> 53, drive3 3741 -> 52, drive4 4377 -> 52, drive5 4957 -> 53.
  Black-pixel counts (letterbox + dark content) match within 0.6% across runs — no
  collateral change to the letterbox, road fog, or scene content.
- Cloud-shader theory: exonerated. The cloud combiner
  (`PRIMITIVE,0,TEXEL0,0 / 0,TEXEL0,SHADE,TEXEL0`) maps cleanly (the run's only
  `unmapped combiner` line is an ignored noise option on an untextured draw), and the
  cloud texture is 64x32 pow2 (no padding).
- Why older scanouts looked clean: the artifact is camera-state dependent (the clamped
  corner spread times 512 must exceed one texel), and against the black letterbox /
  pre-fog dark sky a black-on-dark wedge was invisible to the pixel probes used then.

## Fix (committed on `feat/3ds-skywedge3`)

`DrawTriangles` now pre-clamps repacked UVs to the last content texel's centre —
`(extent - 0.5) / paddedSize`, the interpreter's own clamp-limit formula — for
clamp-requested axes (two branchless `fminf` per UV pair). The shader-clamp (livery)
case reads the interpreter's per-vertex limit so tile-window clamps stay exact; wrap
axes are untouched; the skybox rides the sampler-clamp branch with
vClamp = (1-0.5)/8 = 0.0625, pinning V to the gradient row under both filters.
`[debug] sky_clamp_fix=0` disables for A/B; `[debug] diag_padfill=1` floods padding
magenta; the `[sky]` diag now logs tex0 content/padded extents + the padded-space V
range and applied limit.
