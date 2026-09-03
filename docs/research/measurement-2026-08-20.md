# S0 Measurement — CPU-bound vs GPU-bound verdict (Azahar proxy, 2026-08-20)

**Agent:** MEASUREMENT (owns the Azahar emulator this session).
**Build:** `feat/3ds-m1-night` consolidated night build,
`night/build-3ds/port/3ds/G-Diffuser-3DS.3dsx` (gpuprof telemetry + S12 columns + perf
changes, built green 2026-08-20 05:10).
**Harness:** scripted race via staged `gdx-autoinput.txt`, Azahar New3DS profile,
`log_filter=*:Debug`, `[debug] gputrace=1`. Telemetry gate restored to `gputrace=0` after
the run; SD left as found; emulator lock released cleanly.

## Verdict: the F-Zero X race is decisively **CPU-BOUND** (proxy, pending hardware).

The GPU is essentially idle. Every in-race frame spends its milliseconds on the CPU
building the display list (interpreter + n64 bridge + repack + citro3d submit); the PICA
does its drawing in a fraction of a millisecond and the frame-loop *never* blocks waiting
for the GPU.

Two independent signals from the `gdx3ds_gpuprof` telemetry say the same thing:

1. **`gpu` (C3D_GetDrawingTime — real PICA fill/raster/transfer) = 0.4 ms flat** while
   **`build` (CPU frame-build) = 26–58 ms.** GPU draw cost is ~0.8% of CPU build cost.
2. **`wP3D` (the documented "GPU-bound signal" — time blocked in
   `C3D_FrameBegin`'s `gxCmdQueueWait` because the GPU is still busy) = 0.0 ms in every
   single sample.** The CPU never once waits on the GPU. If we were GPU-bound this column
   would be the tall pole; it is a flat zero.

## Per-frame table — stable in-race driving window

Window: `[c3d] frame` 3393 → 10689 (established driving, `[race-dl]` present throughout),
115 aggregated `[gpu]` samples / 124 `[fill]` samples (each = a 64-frame window average).
All times in ms; counts are per-frame averages.

### GPU line (`[gpu]`) — the CPU-vs-GPU split

| metric | meaning | min | median | p95 | max |
|---|---|---:|---:|---:|---:|
| `wall` | whole frame (logic + build + waits) | 33.9 | 50.1 | 70.2 | 87.7 |
| `build` | **CPU** frame-build (interpreter+bridge+submit) | 25.1 | 26.1 | 53.3 | 57.9 |
| `proc` | C3D CPU FrameBegin→End window (≈build) | 25.1 | 26.2 | 53.3 | 57.9 |
| `gpu` | **GPU** drawing time (P3D + transfer) | 0.40 | 0.40 | 0.50 | 0.50 |
| `wVbl` | vblank pacing wait (slack to the divisor) | 0.5 | 11.5 | 16.3 | 16.4 |
| `wP3D` | **GPU-bound signal** (wait for GPU) | 0.00 | 0.00 | 0.00 | 0.00 |
| `draws` | draw calls / frame | 44 | 66 | 164 | 255 |
| `tris` | triangles / frame | 270 | 318 | 730 | 3726 |

### Fill / S12 line (`[fill]`)

| metric | min | median | p95 | max |
|---|---:|---:|---:|---:|
| `passes` (render-target binds) | 2.97 | 3.00 | 3.00 | 3.00 |
| `estMpix` (fill est., Mpix) | 0.48 | 0.48 | 0.48 | 0.48 |
| `texBytes` (upload/swizzle bytes) | 0 | 8 192 | 8 768 | 28 112 |
| `uniqueTex` (distinct tex / frame) | 38.4 | 41.9 | 155.7 | 156.0 |
| `vtx` (verts through repack) | 812 | 957 | 2 194 | 11 173 |

### How to read the split

- **`build` ≈ `proc`** because on this backend virtually all of `C3D_GetProcessingTime`'s
  CPU window IS the display-list build/submit — it is CPU time, not GPU time. Both are the
  CPU pole.
- **`gpu` is the only genuine GPU-side number**, and it is a flat 0.4 ms — the PICA
  finishes a race frame's draw + transfer in well under half a millisecond.
- The frame budget is consumed by **build (CPU, median 26 ms) + wVbl (vblank slack, median
  11.5 ms)**. The `wVbl` is not GPU work — it is the pacer idling against Azahar's vblank
  divisor once the CPU already finished. Even the p95 heavy frames (build 53 ms) are heavy
  because of CPU list-building spikes (draws jump to 164–255, verts to 2k–11k), never GPU.

## What this means for the campaign

**Prioritize (the CPU-side interpreter/DL attack — this is where the milliseconds are):**

- **S4 — audio-HLE producer → core 2.** Still the top-confidence CPU win; the producer
  tick is on core 0 today, directly contending with the 26 ms build. Unaffected by this
  finding, reinforced by it.
- **S7 — interpreter / bridge / DL-path reduction.** This is *the* pole. `build` is the
  frame. texcache was the proof-of-concept; S7 is where native-60 is won or lost. The p95
  build spikes (heavy-draw frames) are the tail to attack.
- **S2/S3 — malloc-crawl, -O3/LTO/fast-math.** Cheap CPU wins, all land against `build`.
- **S5 — native-60 vs 30Hz+interp gate.** The measurement that decides it is *logic/build*
  ms on hardware, exactly the pole this run isolates. Median build 26 ms > 16.67 ms budget
  even as an inflated proxy — the gate is real and interpreter reduction (S7) is the lever
  that can flip it. Keep `gdx_interp` warm as the fallback.

**De-prioritize / likely MOOT:**

- **S11 — fill-rate.** Confirmed the campaign-plan suspicion: **MOOT.** GPU draw = 0.4 ms,
  `wP3D` = 0, AA already off, ~3 passes at 0.48 Mpix. There is no fill wall to knock down;
  the PICA is idle ~99% of the frame. Do not spend shifts here.
- **S10 — stereo.** The GPU has enormous headroom (0.4 ms of a 16.67 ms GPU budget), so the
  "~2× GPU cost" of stereo is affordable *from the GPU's side* — BUT stereo's second per-eye
  pass roughly doubles the **CPU** build/submit (draws, verts, list-walk), which is the pole
  we cannot afford. Stereo stays gated behind "mono holds 60 + slack" and that gate is a
  **CPU** gate, not a GPU one. This run reframes the stereo cost model as CPU-limited.

## S12 asset read — ETC1 / decimation

The frame is **cheap on assets**; S12 is **not justified by this data.**

- **`texBytes` (upload/swizzle): median 8.2 KB/frame, p95 8.8 KB, max 28 KB.** Trivial.
  Texture *upload* cost is near-zero per frame in steady driving. **ETC1's upload/swizzle
  argument does not hold here** — there is almost nothing to shrink on the per-frame upload
  path. ETC1's only remaining case would be the *resident memory* argument (VRAM/RAM
  footprint), which is a memory-budget question, not a frame-time one, and must be
  re-argued against `3ds-memory-budget.md` — not against S0.
- **`uniqueTex`: median 42, p95/max ~156.** A moderate resident set, spiking on transition
  frames. Not a per-frame cost driver here (bind is cheap; `texBytes` proves re-upload is
  rare). Relevant only to the memory argument, if at all.
- **`vtx`: median 957, p95 2194, max 11 173.** The p95/max spikes ride *with* the `build`
  and `tris` spikes — i.e. vertex volume is a **CPU repack/transform** cost (it lands in
  `build`, not `gpu`), which loops back to S7, not to S12(b) decimation as a GPU win.
  Decimation *could* shave a little CPU repack on the heavy frames, but S7's interpreter/
  bridge reduction is the far larger and more direct lever; **decimation stays low-ROI /
  deferred.**

**S12 kill-criterion check:** the plan says "ETC1 struck if upload+fill are both trivial;
decimation struck if the GPU is idle or the bottleneck is not vertex-bound on the GPU."
Both criteria are **met**: upload (`texBytes`) is trivial, fill (`gpu`/`estMpix`) is
trivial, the GPU is idle (`wP3D`=0), and the vertex cost lands on the CPU. Per its own
gate, **S12 is struck for frame-time purposes** — revisit only under the memory budget, and
re-derive first as the plan already flags.

## Proxy caveat (read before acting)

**Every number here is an AZAHAR PROXY and is directional only.**

- `log_filter=*:Debug` + per-frame `svcOutputDebugString` inflate CPU cost; the real
  device build is instrumented differently and Azahar's ARM11 JIT cost profile is unrelated
  to a real 268/804 MHz ARM11 MPCore. Absolute ms (the 26 ms build, the 50 ms wall) are
  **not** trustworthy as hardware numbers.
- Azahar quantizes to vblank divisors, so `wall`/`wVbl` reflect the emulator's pacer, not a
  console's.
- What this run *does* establish robustly is the **shape**: GPU draw time and the GPU-wait
  are near-zero and flat while CPU build dominates and carries all the variance. That
  CPU-vs-GPU *ratio* is a counter/structural signal (the class the campaign plan trusts
  Azahar for), not a fill-rate absolute (the class it does not).
- **The verdict "CPU-bound" is directional, pending a New3DS + Luma hardware baseline
  (S1).** Every perf shift re-verifies on hardware before it is banked. But the direction is
  strong enough to steer now: **spend the campaign on the CPU/interpreter front (S4, S7,
  S2/S3), drop S11 fill-rate, and hold S12 unless the memory budget re-opens it.**

---

*Raw window data: 115 `[gpu]` + 124 `[fill]` 64-frame aggregates, in-race window
`[c3d] frame` 3393–10689, captured from `azahar_log.txt` this session. Emulator lock held
as singleton `/tmp/azahar.lock`, released cleanly.*
