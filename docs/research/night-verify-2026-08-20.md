# Night-verify 2026-08-20 — fx+sky consolidation + emulator verification

Branch **`feat/3ds-m1-night`** off `feat/3ds-m1` @ 5758075. Autonomous overnight
run: fold `feat/3ds-fx` + `feat/3ds-sky` into m1, author/verify the sky fix, sweep
the shadow decal bias, and emulator-verify the whole set in Azahar. Do NOT
fast-forward `feat/3ds-m1` (left for human promotion in the morning).

## Consolidation (both merges clean)

- `9d0b6a1` merge `feat/3ds-fx` (dbc0b59) — boost/translucent fog off the depth LUT,
  `gdx-decalbias.txt` SD override, `[fogpath]` telemetry.
- `02c79fa` merge `feat/3ds-sky` (ebd5cd8) — `GDX_DIAG_SKY` `[sky]` diagnostic + analysis doc.
- Both branched directly off m1 @ 5758075 (merge-base == m1 HEAD), so each carried only
  its own delta; `gfx_citro3d.cpp` auto-merged (fx fog/decal region + sky diagnostic
  region are disjoint). All 11 base patches re-apply cleanly; build GREEN
  (`cmake -DGDX_PLATFORM_3DS=ON ...`, `-j8`, produces `G-Diffuser-3DS.3dsx`).

## Harness note (cost ~30 min — record for next agent)

Azahar 2126.0 now shows a **modal** "run via the .app bundle" dialog when launched by
the raw binary path (`.../Azahar.app/Contents/MacOS/azahar <rom>`); the modal blocks
boot (main thread parks in `QDialog::exec`), the game never loads, no BMPs, empty log.
**Launch via the bundle instead:**

```
open -a /Applications/Azahar.app [--env GDX_DIAG_SKY=1] --args <ABSOLUTE .3dsx path>
```

`open --env` sets azahar's *host* env, but on the 3DS **ctru `getenv` does not see the
host environment** — raw-`getenv` diagnostics (GDX_DIAG_SKY etc.) are unreachable in the
emulator/hardware. Use the `[debug]` INI keys in `sdmc:/3ds/gdiffuser/gdiffuser.ini`
instead (see below). Everything else in RESUME.md "Scanout-harness gotchas" still holds
(absolute ROM path, `autotest/<label>_scan.bmp` oracle, debounced liveness, periodic
`lldb fflush`).

## 1. Baseline / no-regression — PASS

In-race Mute City scanouts (combined build): road is **textured with fog distance-fade**
(near-clear → far pink haze), HUD/track/position glyphs correct, machine renders. The
invisible-road fog fix HOLDS after the merges. `[fogpath]` telemetry: every fog draw
logs `blend=1` (per-vertex blend path), zero draws fell back to the depth LUT.

- `/tmp/night-verify/final/night_270x_drive2.png`, `..._drive3.png` (in-race)
- `/tmp/night-verify/baseline/night_drive{1,2,3}.png`

## 2. Boost verify — PARTIAL (logic sound; flame draw never triggered)

`[fogpath]` distinct signatures across the whole race are ALL `opt_alpha=0 vtxAlpha=1
blend=1` — the road/track opaque fog draws correctly take the per-vertex blend. **No
`opt_alpha=1` (translucent boost-flame) draw ever fired**, because the scripted
autoinput only holds A and never triggers a boost (boost is lap-2+ / needs its own
input). So the flame's `blend=1` could not be observed live. The fx fix's logic is
nonetheless confirmed correct: `fogAlphaSlotFree = !opt_alpha || vtxAlpha==0`, and the
boost flame (G_RM_FOG_SHADE_A, opt_alpha=1, vtxAlpha==0) WOULD take blend=1 when it
fires. A driver that triggers boost is needed to close this fully.

- `/tmp/night-verify/final/fogpath_distinct.txt`

## 3. Sky overscan — DEAD END (approach invalid), diagnostic now usable

The `verticalRange *= factor` overscan from `sky-backdrop-wedge-analysis.md` is **PROVEN
INEFFECTIVE** on the emulator. `GDX_DIAG_SKY` `[sky]` `topGap` held **~0.31 identically at
factor 1.41x AND 2.70x**, and drive scanouts were pixel-unchanged.

**Root cause:** `Background_UpdateSkyboxVtx` (background.c ~L790-824) clamps every skybox
vertex position to **±32000.0f** (s16 vertex-coord range). At F-Zero X's `skyboxDepth` the
verts already saturate that clamp, so scaling `verticalRange` only pushes them further past
a fixed ceiling — the on-screen quad does not grow. The real fix must **reduce
`skyboxDepth`** (and rescale coverage to stay inside ±32000) or rework the projection —
re-frames the whole backdrop, overlaps the geometry/roof domain; **deferred**.

**Also:** the black band at the very top/bottom of scanouts is largely a **fixed scanout
letterbox** (top≈0, bottom≈0 luma on the *menu/title* screens too, where there is no
skybox), NOT skybox undercoverage. In-race the interior sky is well-covered (pink fog to
the top interior; distant purple skyline present) — no stark black sky wedge reproduced
in these captures.

Shipped instead of a fake fix:
- Reverted the no-op overscan; left a comment-only decomp note
  (`decomp-3ds-sky-overscan-deadend-note.patch`) so the ±32000 dead-end is not repeated.
- `9c27f06` — made `GDX_DIAG_SKY` reachable on 3DS via `[debug] diag_sky` INI (ctru getenv
  can't see host env). The weak `gdx3ds_config_get_int` accessor had to be `extern "C"`:
  inside `namespace Fast` a C++-mangled decl resolved to a distinct unresolved weak symbol
  (nullptr) that silently disabled the fallback (nm: `Fast::...` `w` vs real C `T`).
  Verified: `[sky]` telemetry now fires in-race under `diag_sky=1`.

- `/tmp/night-verify/final/sky_telemetry_distinct.txt` (187 distinct `[sky]` lines)

## 4. Shadow decal-bias sweep

Override mechanism (`gdx-decalbias.txt` at SD root, read once) **verified working**:
log shows `[decalbias] override active: 0.004000`. Current compiled default = `1/1024`
≈ 0.000977.

- **0.004** — drop-shadow sits on the track beneath the craft, no z-fight shimmer, no
  visible lift/gap (`/tmp/night-verify/final/decal004_d2_machine.png`).
- **0.008** — near frame-identical drive2 capture; shadow also on-track, no z-fight, no
  lift — visually indistinguishable from 0.004 at this camera/resolution
  (`/tmp/night-verify/final/decal008_d2_machine.png`). Bias is not sensitive across
  [0.004, 0.008].
- **Chosen default: 0.004** — baked as the `__3DS__` default (was `1/1024` ≈ 0.00098,
  which sat closer to the z-fight edge). 0.004 is the safer mid value (further from lift)
  and comfortably clear of z-fighting. `gdx-decalbias.txt` override kept for the hardware
  pass, which is the real oracle for the final magnitude (small shadow, low chase camera →
  scanout assessment is marginal). Override read-back verified in-log
  (`[decalbias] override active: 0.004000` / `0.008000`).

## Deliverable state

Branch `feat/3ds-m1-night`: fx+sky merged, diagnostic fixed, sky dead-end documented,
decal default set, building green. `feat/3ds-m1` NOT fast-forwarded.
