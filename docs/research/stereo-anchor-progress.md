# Stereo anchor — race position markers at their ship's depth (feat/3ds-stereo-anchor)

## Symptom
In stereo, the 1P/2P/3P position markers (and the GP RIVAL marker) sat at screen depth while
the machine they label is far into the scene. Two conflicting depth cues in one spot; jarring.

## Cause
The markers are ortho texture rects (clip w == 1), so the stereo classifier treated them as HUD
(`GDX3DS_STEREO_UI_ZERO_PARALLAX`). The game already tells us where they belong: `racer.c`
writes `gDPSetPrimDepth((sp564 * 16352) + 16352, 0)` = the labeled machine's projected NDC
depth before every marker rect, and enables `G_ZS_PRIM`. No decomp change needed.

## Fix (port side only, no LUS patch)
- `gdx3ds_stereo.{h,cpp}`: new class `GDX3DS_STEREO_ANCHORED` and `SetAnchorDepth(d)`.
  `ComputeEyeMatrix` gives the draw the SAME shift a scene vertex at NDC depth d receives, as a
  constant per draw: `x' = x + k·(d − dc)·w`, `k = sep/(1 − dc)`. z/w untouched.
- `gfx_citro3d.cpp` draw path: `anchoredDraw = stereo_anchor && !texBacked &&
  prg.cc.opt_prim_depth && bufVbo[3] == 1.0f` (an ortho draw whose combiner carries prim depth).
  It takes the UI projection base (`sDispProjUi`, no scene x-compression) and class ANCHORED with
  `d = mCurrentPrimDepth` (interpreter: `prim_depth / 0x7FFF`, sampled at Flush — prim-depth
  changes already split batches, so each marker is its own draw with its own depth).
- Killswitch `[debug] stereo_anchor = 1` (0 = markers back at screen depth).
- Receipt on the `[c3d]` line (stereo only, only when it fired in the window):
  `anchor=<draws>/<dmin>/<dmax>`.

## Depth convention check
Scene depth in this port is the interpreter's clip z/w in [0,1] (0 near, 1 far; the fixup does
the [0,w] → [0,−w] remap), i.e. N64 NDC depth, which is what the game's prim depth encodes
(0..0x7FFF near..far). Far machines sit at ~0.99 in that space, and the receipt shows exactly
that: `anchor=38/0.99/0.99`.

## Verification (Azahar, side-by-side stereo, Mute City GP start)
Deterministic tick script (START, A presses, then 200-tick A holds), 1 s window captures.
Frame at race time 00:01.15 (`stereo-anchor-t92-eyes.png`, left | right eye, 3x): the cyan
marker above the leading pack moves with the far machines, not with the HUD.

| element              | eye shift (game px) |
|----------------------|---------------------|
| HUD "29" (position)  | 0.5                 |
| far red machine      | 20.7 (5-px blob)    |
| position marker      | 26.3                |

Log: `anchor=19/0.99/0.99`, `anchor=26/0.99/0.99` on the two windows that had markers; zero
error lines. Markers only exist while a top-3 machine is ≥ 800 units ahead and on screen, so
the window is short at a start; the user's own play (lapped, or in GP with the RIVAL marker) is
where it shows.

## What to watch on hardware
- Markers should sit ON the ship they point at (slider up), not in front of the screen.
- HUD unchanged; no change with stereo off. `[debug] stereo_anchor=0` A/Bs it live (relaunch).
- Anything else that pops into the scene unexpectedly: it would be another prim-depth ortho draw
  (the receipt's draw count would jump); report the screen.
