# gdx3ds-dl-tests — per-scene expected results

Ground truth for eyeballing (or later screenshot-automating) the citro3d
backend against `port/3ds/gfx/STATUS.md`'s TODO(citra-verify) items. Scenes
advance with START; the bottom-screen console shows the same expectations in
short form. The authoritative cross-check is the desktop G-Diffuser build
rendering the same logical content — every scene below is described in *screen*
terms (what a person looking at the 3DS/Azahar top screen must see).

## Global framing (applies to every scene)

The interpreter runs at the N64-native 320x240 with a 1:1 viewport, while the
backend's screen target is the full 400x240 top screen. Correct behaviour is
therefore:

```
 top screen (400x240, landscape)
+--------------------------------+------+
|                                |      |
|      320x240 rendered area     | dead |
|      (scene content)           | band |
|                                | 80px |
+--------------------------------+------+
```

- The dead band is whatever the previous contents of the target were (black
  after clear). With the intended fixup-matrix signs it sits on the **right**
  edge.
- Band on the LEFT instead: viewport axis mapping is offset (viewport/scissor
  corner-origin item) — note it, then judge the scenes by their own geometry.
- Content rotated 90/180/270 degrees or mirrored: fixup-matrix rotation signs
  wrong (`gfx_citro3d.cpp` `mFixupMatrix`).
- All scenes clear to opaque black; scene content is drawn fresh every frame.

## Scene 1 — STRIP (vertex order/winding + viewport origin)

160x120 quad centred in the rendered area, drawn as two triangles from a
tri-strip-ordered vertex set, smooth (gouraud) interpolation:

```
+--------------------------+
|                          |
|   R============G         |   R = red      G = green
|   |  gradient  |         |   B = blue     W = white
|   B============W         |
|                          |
+--------------------------+
```

- **PASS**: red corner top-left, green top-right, blue bottom-left, white
  bottom-right; smooth colour blend; no missing triangle half.
- Red at bottom-left = Y axis flipped (viewport origin item).
- Red at top-right = X mirrored.
- One solid diagonal half missing = triangle index/winding decode fault.

## Scene 2 — ROTATE (fixup-matrix rotation sign)

One gouraud triangle (red apex, green bottom-left, blue bottom-right) drawn
with a precomputed model matrix that steps 45 degrees every 16 vsync frames
(8 matrices, ~2s per revolution):

```
        R                 R moves left...        ...ends up left of centre
       / \        ->       ...                    R
      /   \                                        \__
     G-----B                                       ...
```

- **PASS**: at step 0 the red apex points UP; the apex then sweeps
  **counterclockwise** (red moves toward the LEFT edge first).
- Clockwise sweep = rotation sign flipped somewhere between the interpreter's
  matrix path and the fixup matrix.
- Triangle wobbling/shearing instead of rigid rotation = fixed-point matrix
  unpack fault (not a backend item; would implicate `MtxFromFloat`/GfxSpMatrix).

## Scene 3 — TEXTURE (texture row order + UV orientation)

128x128 quad centred, textured with a code-built 32x32 RGBA32 pattern,
point-sampled, texel row 0 mapped to the quad's top edge:

```
+----------------+
|RRRRRRRRRRRRRRRR|   2px solid RED stripe   = texture ROW 0..1 (top)
|GG      /\      |   2px solid GREEN stripe = texture COL 0..1 (left)
|GG     /  \     |
|GG    /____\    |   white UP-pointing arrow (apex near the top)
|GG      ||      |   on dark grey background
|GG      ||      |
+----------------+
```

- **PASS**: red stripe on TOP, green stripe on LEFT, arrow pointing UP.
- Red stripe at the BOTTOM (arrow pointing down) = texture rows uploaded in
  flipped order (Morton-swizzle row order item in `UploadTexture`).
- Green stripe on the RIGHT = S axis mirrored.
- Blocky diagonal garbage = swizzle tile math wrong, not just an orientation
  flip.

## Scene 4 — SCISSOR (scissor rectangle origin)

Full-screen yellow quad, scissored (N64 raster coords, origin top-left) to
`(0,0)-(160,120)`:

```
+------------+-------------+
|YYYYYYYYYYYY|             |
|YYYYYYYYYYYY|   black     |
+------------+             |
|                          |
|          black           |
+--------------------------+
```

- **PASS**: yellow in the TOP-LEFT quadrant of the rendered area only; the
  other three quadrants stay black.
- Yellow bottom-left = scissor Y origin not flipped for the rotated target.
- Yellow top-right = scissor X/Y swap wrong.
- Yellow everywhere = scissor silently ignored; yellow nowhere = degenerate
  scissor rect (check the axis-swapped `C3D_SetScissor` corner encoding).

## Scene 5 — DECAL (decal zmode depth offset)

Z-buffered blue 200x140 quad at z=0, then a red 100x70 quad at the SAME z
drawn with `G_RM_ZB_OPA_DECAL`:

```
+--------------------------+
|   BBBBBBBBBBBBBBBBBBB    |
|   BBBB+---------+BBBB    |
|   BBBB| RRRRRRR |BBBB    |
|   BBBB+---------+BBBB    |
|   BBBBBBBBBBBBBBBBBBB    |
+--------------------------+
```

- **PASS**: solid, stable red rectangle centred on the blue one; edges clean;
  no flicker frame to frame (the scene redraws every vsync — decal failures
  show up as shimmer even in a static image).
- Red completely absent = decal offset pushes the decal BEHIND the base
  (offset sign wrong for the reversed-depth GEQUAL setup).
- Red/blue speckle (z-fighting) = decal offset magnitude too small.
- Red visible but blue base gone = offset far too large / depth test broken.

## Scene 6 — COMBINE (TexEnv combiner mapping)

Four 64x56 arrow-textured quads in a row (left to right), prim colour =
orange (255,128,0), env colour = azure (0,128,255), vertex shade = mid grey:

```
 [1 REPLACE] [2 MODULATE] [3 LERP] [4 MULADD]
```

1. **REPLACE** (`G_CC_DECALRGBA`): the raw arrow texture, exactly like
   scene 3 at smaller size.
2. **MODULATE** (`TEXEL0 * PRIM`): the arrow tinted orange — white arrow
   becomes orange, grey background becomes dark orange-brown, red stripe
   stays red-ish, green stripe goes dark (green x orange kills red/blue).
3. **INTERPOLATE** (`(TEXEL0 - PRIM) * ENV + PRIM`, per-channel lerp by the
   azure env colour): an orange-based quad where the texture shows through
   mostly in the green/blue channels — the arrow appears as a pale
   yellow-to-white shape over orange; NOT a plain copy of either input.
4. **MULTIPLY_ADD** (`TEXEL0 * SHADE + PRIM`): the texture at half brightness
   plus orange — a bright washed-orange quad where the white arrow reads as
   light peach and the background as mid orange.

- **PASS**: four clearly DIFFERENT quads, none black, none identical to a
  neighbour, and **no `[gfx_citro3d] unmapped combiner ...` line on stderr**
  (Azahar: check the console/log window).
- Any quad rendering as plain texture x vertex-colour when it shouldn't =
  that mode fell into the backend's unmapped-combiner fallback: a TexEnv
  mapping regression, and stderr will name the shader-ID pair.

## Scene 7 — TEXEL1 (2-cycle adjacent-tile TEXEL1: unit-1 bind + UV1 scale)

One 32x64 RGBA16 image block-loaded into TMEM (4096 B — all of it), then two
render tiles pointing INTO that single load (the F-Zero X track/vehicle
pattern, `interpreter.cpp:3182-3188`): tile 0 = rows 0-31 ("view A": the
red-stripe/green-stripe/up-arrow design), tile 1 = a 32x24 window at TMEM word
256 ("view B": an 8px blue/yellow checkerboard). Rows 56-63 of the image are
MAGENTA and sit outside tile 1's window. Three 80x80 quads:

```
 [1 TEXEL0: arrow] [2 TEXEL1: checker] [3 lerp arrow->checker]
```

1. **TEXEL0 only** (1-cycle `G_CC_DECALRGBA`): view A — arrow, red stripe on
   top, green stripe left. This is the control quad.
2. **TEXEL1 only** (2-cycle, cycle 1 `RGB = TEXEL1`, cycle 2 passthrough):
   view B — the blue/yellow checker filling the quad, 4 cells across and
   3 cells down (24 rows stretched over the quad height).
3. **`(TEXEL0 - TEXEL1) * SHADE + TEXEL1`** with a white→black horizontal
   shade ramp: arrow on the left edge morphing into checker on the right.

- **PASS**: all three quads distinct as described; no `unmapped combiner`
  or `shader wants texel1` line in the log; `[c3d]` telemetry shows
  `bindMiss=0`.
- Q2 showing the ARROW = TEXEL1 sampling tile 0's texture (adjacent-tile
  selection broken).
- Q2 grey/black or `shader wants texel1 but texture N is not uploaded` in the
  log = unit-1 texture never bound/uploaded.
- MAGENTA anywhere = tile-1 extent/window decode broken (sampling past the
  24-row window into the sentinel rows).
- Black band at the bottom of Q2/Q3's checker = UV1 rescale wrong (unit-0's
  pow2 scale applied to unit 1: samples run into the padding rows).

## Scene 8 — MACHINE (census #16: 3-stage 2-cycle machine material)

The 271-site custom-machine combiner: cycle 1 `(PRIM - ENV) * TEXEL0 + ENV`
(lerp ENV→PRIM by texel), cycle 2 `COMBINED * SHADE`. PRIM = red (255,40,40),
ENV = blue (0,32,255), arrow texture. Two 100x90 quads:

```
 [L: full shade] [R: shade ramp ->dark]
```

- **PASS (left quad)**: BLUE body (dark-grey texels ≈ 24% toward red — still
  clearly blue-dominant) with the arrow reading RED/pink; the 2px texture
  stripes read as slightly different blends. NOT flat blue, NOT flat red,
  NOT the raw grey arrow.
- **PASS (right quad)**: same image, darkening smoothly toward the right
  edge (shade ramp 255→40 multiplies the cycle-1 result).
- Raw grey arrow (texture x white) = combiner fell into the unmapped
  fallback — the log names the shader-ID pair.
- Left quad correct but right quad NOT darkening = cycle-2 shade modulate
  lost (vertex-colour input not riding the attribute).
- Flat blue with no arrow = TEXEL0 lerp factor lost (constant-spill prefix
  stage broken).

## Scene 9 — FOG (PICA native fog unit vs the interpreter's fog vertices)

Full-width red quad (`G_CC_SHADE`) tilted in depth — left edge near
(z/w = -0.78), right edge far (z/w = +0.78) — drawn with `G_FOG` vertex fog,
`gSPFogPosition(400, 900)` and BLUE fog colour (40,60,255) through
`G_RM_FOG_SHADE_A`:

```
+--------------------------+
| RED ... red ... purple .. BLUE |
|   (left ~37% pure red,       |
|    then ramping to ~98% blue |
|    at the right edge)        |
+--------------------------+
```

- **PASS**: pure red from the left edge to roughly x=120 of 320 (fog factor
  0 until `gSPFogPosition`'s min), then a smooth red→blue ramp reaching
  near-full blue at the right edge. The ramp must be stable frame to frame.
- Gradient REVERSED (blue on the left) = fog-LUT depth direction flipped
  (reversed-depth mapping: near = depth 1).
- No blue at all = fog unit never enabled or the fog LUT is all-zero
  (check `UpdateFogState`'s per-draw linear fit).
- Whole quad solid blue = fog factor saturating (LUT offset/slope fit wrong).
- Banding/steps = LUT quantization too coarse (128-entry LUT should look
  continuous).
