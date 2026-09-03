# Morning status — 2026-08-20 (overnight autonomous session)

Read this first. Summarizes what got fixed, what's verified, what's still broken, and the branch map.

## TL;DR
- **The invisible-road fog blocker is SOLVED and in `feat/3ds-m1`** — the road renders textured with a
  proper distance fade. This was THE blocker. Verified in live play + scanouts.
- A wave of visual-bug fixes were built + partially verified on side branches. The **`feat/3ds-m1`
  integration branch is deliberately left at the rock-solid `5758075`** (fog + perf + gpuprof +
  stereo, all verified). Nothing risky was promoted into it.
- Two visible bugs remain UNFIXED after failed first attempts: the **km/h speedometer scramble** and
  the **black sky wedge**. Both are now correctly root-scoped for the next attempt (see below).

## What is VERIFIED-GOOD and already in `feat/3ds-m1` @ 5758075
- **Fog fix** (invisible road) — per-vertex fog factor → primary-alpha → one GPU_INTERPOLATE TexEnv
  stage, bypassing the depth-LUT that couldn't resolve F-Zero's compressed clip-depth band.
- **perf**: frame-pacer double-throttle removed (vblank is sole pacer); HLE-audio producer → core 2;
  malloc histogram (opt-in).
- **gpuprof** telemetry + S12 asset-cost columns; **stereo** foundation (flag-off no-op).

## Candidate branch `feat/3ds-m1-night` @ 765e238 (a STRICT improvement over m1, no regressions)
Built green, road-fog confirmed intact. Adds on top of m1:
- **Machine shadow — FIXED & verified.** Decal depth-bias baked to `0.004` (was 1/1024, near the
  z-fight edge). Shadow sits cleanly on the track.
- **HUD symbol/lap/racer strips — FIXED** (RGBA16 tall-NOMASK-atlas decode extent + matching
  content-hash span; both host-tested). NOTE: these did NOT fix the km/h scramble (wrong layer — see
  below), but they are real correctness fixes and regress nothing.
- **Boost-jet tint fix — code-complete, UNOBSERVED.** The boost flame is a translucent fog draw that
  the fog fix left on the broken depth-LUT; extended the per-vertex blend to cover it. Couldn't be
  seen live because the scripted driver never triggers a boost. Logic is sound by inspection.
- **Sky diagnostic** (`GDX_DIAG_SKY`, no fix).

To build/run `feat/3ds-m1-night`: worktree `~/code/gdx-3ds/night`; `.3dsx` at
`night/build-3ds/port/3ds/G-Diffuser-3DS.3dsx`. Launch (Azahar 2126+):
`open -a /Applications/Azahar.app --args <abs .3dsx>`.

## STILL BROKEN (visible) — correctly scoped, attempts in progress
1. **km/h speedometer** — black panel, green LED digits render as a **static green scramble**
   (identical across frames → a decode/FORMAT bug, not a texcache race). The RGBA16 HUD fixes are
   confirmed ineffective for it. Strong hypothesis: the digits are a **CI (palette/TLUT) or I/IA
   intensity** format the 3DS converter mishandles. A focused agent (`feat/3ds-kmh`) is
   re-identifying the true texture format + decode path. UNFINISHED.
2. **Black sky wedge** — NOT a fog bug. It's the black framebuffer clear-color showing where the 4:3
   skybox quad under-covers the 3DS's wider rotated display (+ partly a fixed letterbox). The obvious
   fix (scale `verticalRange`) is a proven DEAD END: the skybox verts are s16-clamped at ±32000,
   already saturated, so scaling does nothing. Needs a NEW approach (viewport/letterbox extent, or a
   full-screen sky-color backdrop clear, or addressing the vertex clamp). UNSTARTED.
3. **Boost tint** — see above; needs a boost-triggering driver to confirm.
4. **Tunnel roof** — deprioritized (human: "looks ok"). Both cull AND near-plane-clip ruled out;
   `GDX_DIAG_ROOF` diagnostic exists on `feat/3ds-roof` to bisect (drawn-invisible vs combine) if it
   turns out to matter.

## Performance — MEASURED: the race is CPU-BOUND (Azahar proxy)
Measurement pass done (`measurement-2026-08-20.md` on `feat/3ds-m1-night` @ 2ddcbf4). Over a 115-sample
in-race window: GPU drawing time **0.4 ms** vs CPU build **26–58 ms**; GPU-wait (`wP3D`) **0.0 ms every
frame** — the loop never waits on the PICA. The software DL interpreter (Fast3D, CPU-side) is THE
bottleneck.
- **Path to 60fps = CPU work:** S7 interpreter/DL reduction (the pole), S4 audio→core2 (DONE), S2/S3
  cheap CPU wins. **S11 fill-rate is MOOT** (GPU idle).
- **S12 ETC1/decimation is STRUCK for frame-time** — the frame is trivially cheap on assets (median
  8 KB tex upload, 42 unique textures, 957 verts). Model/texture reduction will NOT help speed; only a
  memory-budget argument could reopen it.
- Absolute ms are directional (debug logging inflates CPU; JIT ≠ ARM11); the CPU-vs-GPU *shape* is
  robust. Real baseline still needs a New3DS.

### km/h speedometer — localized (not yet fixed)
Bisected via the now-3DS-reachable `[speedtex]` diagnostic (`feat/3ds-kmh`): it did NOT fire during a
steady race → the km/h texture cache-HITS → the content key is STABLE (not thrashing). Combined with the
static-across-frames scramble, the bug is **downstream in the gfx backend** — Morton swizzle / UV(vScale)
rescale / bind of the small non-pow2 NOMASK atlas in `gfx_citro3d.cpp`, NOT a decode or texcache-key bug.
That's the next lead. Textures confirmed RGBA16 (CI/IA hypothesis disproven).

## Branch map (nothing merged to m1 except the verified consolidation)
| Branch | Contains | State |
|---|---|---|
| `feat/3ds-m1` @ 5758075 | fog+perf+gpuprof+stereo | VERIFIED, frozen |
| `feat/3ds-m1-night` @ 765e238 | m1 + shadow + hud-strips + boost + sky-diag | green, no-regress, shadow verified |
| `feat/3ds-kmh` | km/h format decode | IN PROGRESS (agent) |
| `feat/3ds-fx`,`-sky`,`-roof`,`-hud` | source branches / diagnostics | folded or reference |
| `feat/3ds-cidocs` @ 7d74244 | ci-3ds.sh 9→11 patches + idempotency fix | ready |

## Recommendation for the morning
1. Eyeball `feat/3ds-m1-night` (shadow better, HUD strips better, road-fog intact) — if it looks
   good, promote `feat/3ds-m1` to it. It's a strict improvement.
2. Check `feat/3ds-kmh` result (km/h) + any follow-ups; verify on emulator, fold in if good.
3. The sky wedge needs a fresh design decision (letterbox vs backdrop clear vs vertex-clamp).

## Gotchas learned tonight (also in RESUME.md)
- Azahar 2126+ needs `open -a /Applications/Azahar.app --args <abs .3dsx>` (raw binary path hits a modal).
- 3DS `getenv` can't see host env → gate diagnostics via `[debug] diag_*` INI keys (+ `extern "C"`).
- STALE-OBJECT TRAP: after re-applying submodule patches on a built worktree, `touch` the patched
  `.cpp` and confirm it recompiles — cmake linked a stale `interpreter.o` and cost a whole verify run.
