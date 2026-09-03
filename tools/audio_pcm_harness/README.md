# Audio PCM Parity Harness

Bit-identical PCM gate for the R2 audio delivery migration
(contract **C-R2.3**, `docs/investigation/2026-07-18/o2r-migration/R2_CONTRACTS.md`).

The gate's exit criterion: the captured audio PCM is **byte-identical across the
scenario matrix (legacy fiber path) before vs after the R2-C delivery swap**. This
harness captures that PCM deterministically, hashes it, and compares runs.

Python 3, standard library only. Run every script from inside
`tools/audio_pcm_harness/` (the two orchestration scripts import `run_scenario`
and `compare_pcm` as sibling modules).

## How capture works

A dormant tap already exists at `decomp/src/audio/disk/lib/thread.c:84`, upstream
of ALL host post-processing (low-pass, volume, underrun-fade). The new streaming
module `port/gdx_audio_capture.{h,c}` hooks that tap: when `GDX_PCM_CAPTURE` is
set to an output **prefix**, it streams the raw AI output — **interleaved signed
16-bit little-endian stereo, 32 kHz** — to `<prefix>.pcm`, and on finalize writes
`<prefix>.pcm.sha256` (sha256sum-style `"<hex>  <name>"`).

Two determinism pins make the stream reproducible (the gate is unreachable
without BOTH):

| Pin | Where | Control |
|-----|-------|---------|
| `gRandSeed1/2` (wall-clock seeded) | `sys_gfx.c:713,764` | `GDX_RAND_SEED1` / `GDX_RAND_SEED2` env override |
| `gAudioCtx.audioRandom` (fed by `osGetCount()` every tick) | `thread.c:194` | deterministic counter, **only** while capture is armed |

Golden runs also force the legacy fiber audio path with `GDX_AUDIO_THREAD=0`.

The capture window and stop are defined in **frames**, never wall-clock:
`GDX_PCM_CAPTURE_FRAMES=N` finalizes after N frames, and `main.cpp` then auto-exits
(it polls `gdx_pcm_capture_finished()` and reuses the window-close path). That is
what makes a run headless.

## Scripts

| Script | Purpose | Fails (exit 1) when |
|--------|---------|---------------------|
| `run_scenario.py` | Launch `G-Diffuser.exe` with a scenario's pins + optional tick-scripted autoinput; wait for the bounded capture to finalize; collect `<prefix>.pcm.sha256`. | The run times out, exits without producing a sidecar, or the autoinput file is missing. |
| `compare_pcm.py` | Compare two `.pcm` files byte-for-byte (reports first divergent sample: frame/channel/value) **or** two `.sha256` sidecars (digest equality). | The bytes/digests differ. |
| `verify_determinism.py` | Run one scenario **twice** with identical pins, assert identical SHA (byte-localizes any divergence). | The two runs differ, or a run fails. |

## Scenario matrix (`scenarios/*.json`)

| # | File | id | Status |
|---|------|----|--------|
| 1 | `01_title_bgm.json`    | `title_bgm`    | **ready** — zero input; attract trigger is sample-position-driven (already deterministic) |
| 2 | `02_gp_course.json`    | `gp_course`    | **needs-recording** — tick-scripted autoinput placeholder; open item: verify no `Math_Rand` reachability in AI/audio first |
| 3 | `03_ek_course.json`    | `ek_course`    | **needs-recording** — EK course; last to green |
| 4 | `04_fault_jingle.json` | `fault_jingle` | **blocked** — trace `leo_fault_dd.c` wall-clock retry gating before declaring it deterministic |

Scenario JSON schema is documented at the top of `run_scenario.py`. The
`.autoinput.txt` files use the **tick timebase** (`port/input_bridge.c`): first
line `ticks`, then `<tick> <INPUT> [holdTicks]`. The legacy seconds format still
works (no `ticks` marker) but is non-deterministic and must not be used for
goldens.

## Typical gauntlet

```
# 1. Prove a scenario is deterministic (precondition for a stable golden).
python verify_determinism.py --exe <G-Diffuser.exe> --scenario scenarios/01_title_bgm.json

# 2. Capture the golden into golden/.
python run_scenario.py --exe <G-Diffuser.exe> --scenario scenarios/01_title_bgm.json --out-dir golden

# 3. After the R2-C delivery swap, recapture and compare against the golden.
python run_scenario.py --exe <G-Diffuser.exe> --scenario scenarios/01_title_bgm.json --out-dir captures
python compare_pcm.py captures/title_bgm.pcm.sha256 golden/title_bgm.pcm.sha256
```

See `golden/README.md` for the full owner-run capture procedure, the dedicated-
thread regression canary, and the per-scenario blessing rules.

## Unit test

The capture module has a standalone unit test (`port/tests/pcm_capture_tests.c`,
CMake target `gdx_pcm_capture_tests`) that exercises the byte layout, the frame
cap, the SHA-256 sidecar (cross-checked against an independent SHA-256), the
unconfigured no-op, and run-to-run determinism — no game required.
