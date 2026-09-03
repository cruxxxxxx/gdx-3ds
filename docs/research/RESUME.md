# 3DS Port — Resume State (paused 2026-08-14, Fable credits exhausted; resume ~2026-08-19)

Single source of truth for picking the endeavor back up. Read this first, then
`m1-boot-debug.md` for the full debug history and `60fps-campaign-plan.md` for the road ahead.

## Where the game is

Playable in Azahar (New3DS profile). Boots, menus navigate, races run, HUD fully correct,
audio via HLE. Verified milestones: title/menu/race render, save path works, ~15-20fps menus
/ ~10fps race **in Azahar with debug logging** (proxy numbers, not hardware; instrumented
builds are slower still).

## Active worktree & branch

- **Integration branch = `feat/3ds-m1`**, worktree `~/code/gdx-3ds/m1`. This is HEAD of the
  work. `feat/3ds` (integration/) is stale — do not use.
- Build: `export DEVKITPRO=/opt/devkitpro; cmake --build build-3ds -j8` — **currently GREEN**,
  produces `build-3ds/port/3ds/G-Diffuser-3DS.3dsx`.
- Fresh worktree setup: `tools/ci-3ds.sh` reapplies all 11 patches + builds + host tests.
- 11 patches in `port/3ds/patches/` (README = apply order). All committed as files AND applied
  to the m1 submodule working trees (WIP fully captured — verified via clean reverse-apply;
  texcache "not reversible alone" is the known interpreter.cpp triple-patch overlap, not loss).

## THE ONE BLOCKER TO CLOSE FIRST: invisible road (fog)

**STATUS 2026-08-20: SOLVED and emulator-verified on branch `feat/3ds-fog` @ `c695e31` (per-vertex
fog factor → primary-alpha → one `GPU_INTERPOLATE` TexEnv stage, bypassing the depth-LUT). In-race
scanouts show a textured road with near-clear/far-hazed distance fade. Pending consolidation into m1
(branch `feat/3ds-m1-next`). History below kept for the record.**

**STATUS 2026-08-19 (Opus verification run): batch-split fix `45d3294` DISPROVEN. Root cause
re-diagnosed with high confidence — see below. The real fix is a code change, not yet written.**

- **Superseded hypothesis (batch-mixing):** two fog lines (track `(990,1000)` vs backdrop
  `(980,1000)`) sharing one batch, whole batch binding the last-latched line. Fix `45d3294`
  (`__3DS__`-gated `Flush()` in both G_MW_FOG moveword handlers) addressed this. **It does NOT fix
  the invisible road** — a scripted-race Azahar run on the current build (fix compiled in, verified
  live in `libultraship/src/fast/interpreter.cpp` + rebuild no-op) still shows the track full pink
  in-race (evidence: `sdmc/autotest-fogfail-2026-08-19/drive{1,2,3}_scan.bmp` — identical to the
  pre-fix build). Leave the commit in place (harmless, arguably correct hygiene), but it is NOT the
  cure.
- **ACTUAL root cause (high confidence, proven by the fogprobe experiment):** the citro3d backend
  feeds fog through the **PICA fog unit, whose LUT is indexed by fragment DEPTH**
  (`UpdateFogState`, gfx_citro3d.cpp:1208). F-Zero X compresses the ENTIRE track into clip-depth
  band **d ∈ [0.95, 1.0]** (`[fogdraw]` d-ranges), which all maps to the LUT's far end (≈full fog).
  So every fogged race draw gets ~full fog colour (`col=fdc0fc` pink) regardless of the fog line.
  The batch-split and the "exact fog line" fit were both fixing the wrong layer; the baked
  per-vertex factor (`bufVbo[fogOffset+3]`, correctly varied `f=[0.000..0.510]` in `[fogdraw]`) is
  **ignored** by the depth-LUT approach.
  - **Decisive proof:** the built-in `gdx-fogprobe.txt` discriminator (forces a steep line
    `a=25 b=-24`, `f=25d-24`) produced a scanout **identical** to the real line `a=100.39 b=-99.39`
    (`autotest-fogprobe-2026-08-19/drive{1,2}_scan.bmp`). Changing the fog LINE changed nothing →
    the LUT is not discriminating race fragments → all race depths hit the same (far/full-fog) LUT
    entry. The line/batch are irrelevant.
  - Human earlier confirmed `gdx-nofog.txt` (disables the PICA fog unit) → full correct textured
    scene. So the PICA fog unit IS the thing painting the road pink; disabling it proves the geometry
    and textures are all there.
- **THE REAL FIX (charter for the next fog agent, e.g. V / §V-VISUALS):** stop routing race fog
  through the depth-indexed PICA fog unit. Instead blend `fogColor` into the fragment using the
  **baked per-vertex fog factor** that already rides the vertex stream at `fogOffset+3` (N64-faithful:
  the RSP already computed it per vertex). On PICA that means a TexEnv combiner stage weighted by the
  interpolated vertex fog factor (combiner census had headroom — ≤3 of 6 stages used). This decouples
  fog from the unusable z-precision in the [0.95,1.0] band. Alt/cheaper probes to try first: index the
  fog LUT by **w (linear)** instead of z if citro3d exposes it, or remap the depth→LUT coordinate to
  expand [0.95,1.0]→[0,1]. Verify with the same scripted-race scanout harness (see OPS note below).

## In-flight agents when paused (all died at credit/session/API limit — RESPAWN needed)

| Agent | Branch/worktree | State | Resume action |
|---|---|---|---|
| C4 (fog) | m1 | Committed candidate `45d3294`, **unverified** | Verify fog fix in Azahar (above) |
| F2 (verify G+S) | gpuprof + stereo | Never got emulator lock; no verification runs | Run G's GPU table + S's stereo off/on proof |
| P2 (perf) | perf (`aa2cef3`) | **No commits** — never really started | Respawn from scratch: pacer double-throttle, audio-HLE-producer→core2, malloc histogram |

## Landed & waiting to merge into m1 (verified, on their own branches)

- `feat/3ds-gpuprof` (`d37a8f3`) — GPU/fill-rate telemetry (`[gpu]`/`[fill]` lines). Built, needs
  F2's first CPU-vs-GPU table run, then merge. **Steers the whole 60fps campaign** (is race
  CPU- or GPU-bound? — campaign plan S0).
- `feat/3ds-stereo` (`27adcc7`) — stereo foundation: dual targets, per-eye loop, HUD-class channel,
  behind `stereo.enabled=0`. Built, needs F2's off/on proof, then merge. Foundation only; proper
  per-eye projection math is a later shift (see stereo3d-research.md + planned STEREO.md).

## Roadmap (from 60fps-campaign-plan.md — the executable order)

1. **Close fog blocker** — `45d3294` disproven (see above); write the REAL fix: per-vertex fog
   factor via TexEnv combiner instead of the depth-indexed PICA fog unit. Then merge gpuprof + stereo.
2. **S0/S1**: run GPU telemetry table; **get a real New3DS** — all Azahar fps are proxies. Every
   perf shift re-verifies on hardware.
3. Quick wins: malloc-crawl (S2), compiler pass -O3/LTO/fast-math (S3), **audio-HLE producer →
   core 2** (S4, top-confidence win — only the ndsp drain thread offloads today, producer still
   on core 0).
4. **S5 GATE**: native-60 vs 30Hz+interpolation — decided by measured p95 logic ms. Port already
   ships `gdx_interp` so the retrofit is cheap (~2-4 shifts) if native-60 fails.
5. S7 interpreter/DL-path reduction (texcache is the proof-of-concept), S9 frame-pacing polish
   (needs P2 verdict), **S10 stereo gate** (mono must hold 60 + ≥2ms slack first), S11 fill-rate
   (likely MOOT — AA already off, 250-700 race tris probably leaves GPU idle; G's telemetry decides).

## Known visual polish backlog (post-fog, human-observed)

- Tunnel roof missing → suspect `G_CULL_FRONT/BACK` geometry-mode → citro3d face-cull mapping (inverted/unhandled winding).
- Element jitter/shimmer → z-precision (N64 depth → PICA 24-bit).
- Textures occasionally swap → texcache key aliasing residual (T's hash) or TMEM tile-state bleed.
- Speedometer garbage, HUD sparkle, overlapping position glyphs, black sky wedge.
- Tunnel lighting off → vertex-lighting/shade path on ILP32.
- Frame-mirror readback regression (B2 flag): in-game `CopyFramebuffer`→`ReadFramebufferToCPU`
  returns black while present shows lit scanout (transition captures share it).

## Deferred (need external input)

- **Real hardware run** — needs a New3DS + Luma≥10.1.1 + on-console dspfirm.cdc dump. Runbook:
  `docs/3DS-HARDWARE.md`. `.cia` + `.3dsx` both build (agent H).
- **Patch consolidation** → fork branches — needs GitHub push access. Plan:
  `docs/research/patch-consolidation-plan.md`. Amend inventory with gpuprof/stereo patches before
  executing (K's coupling note). Latent bug flagged: `lus-resource-cache-cap` is unconditional
  (caps desktop too) — fix in the same conversion.
- **`feat/3ds` integration branch merge + worktree cleanup** — orchestrator housekeeping once dust settles.

## Ops gotchas (cost real time — heed)

- Fresh worktree = `git submodule update --init --recursive --depth 1` then apply all 11 patches
  (ci-3ds.sh does it). Never commit inside submodules.
- Azahar: install from GitHub releases (not brew — no cask). `log_filter=*:Debug` AND
  `log_filter\default=false` in qt-config for the svc debug channel; Azahar reverts it — edit only
  while Azahar fully dead. Repeated SIGKILLs can wedge it in IOKit HID → needs logout/reboot.
- Emulator lock `/tmp/azahar.lock`: **touch the dir every 30s while holding** (stale-mtime steals
  killed live runs). ONE Azahar instance ever.
- svcOutputDebugString shows in Azahar log at Debug level; on real HARDWARE it goes nowhere —
  use the file-log sink (`debug.filelog=1` → sdmc:/3ds/gdiffuser/log.txt, agent H).
- Azahar stdio buffering truncates the log mid-line at "hangs" — `lldb -p <pid> -o "expr (int)fflush(0)"`.
- **Recurring bug class**: 3DS low address space (image 0x100000, heap 0x8000000) collides with
  every "small address = N64 segment offset" heuristic. Check that first on any new memory/pointer bug.
- Agents die at credit/session/API limits — **commit early, commit often** (wip() prefix ok).
  Detect dead agents via branch commit mtimes + transcript files; respawn with narrowed charter.

### Scanout-harness gotchas (learned 2026-08-19 — cost an hour)

- **Launch azahar with the ABSOLUTE `.3dsx` path.** A relative path fails silently (`Loader <Error>
  Failed to obtain loader` in `~/Library/Application Support/Azahar/log/azahar_log.txt`) and the
  window just sits there. The game reads `gdx-autoinput.txt` and writes `autotest/*.bmp` relative to
  the **sdmc root** regardless of azahar's cwd, so only the ROM arg needs to be absolute.
- **Azahar 2126.0+ (updated 2026-08-20): launch via the .app bundle, not the raw binary.** The raw
  `.../MacOS/azahar <rom>` path now pops a modal "run via the .app bundle" dialog that blocks boot.
  Use `open -a /Applications/Azahar.app --args <ABSOLUTE .3dsx>` instead. (Earlier this session the
  raw path worked; the app auto-updated mid-session.)
- **3DS `getenv` can't see host env vars.** `GDX_DIAG_*` switches gated purely on `getenv` are
  UNREACHABLE in the emulator — route them through a `[debug] diag_*` key in
  `sdmc:/3ds/gdiffuser/gdiffuser.ini` instead (and make sure the weak config accessor is `extern "C"`,
  or C++ name-mangling silently disables the fallback — cost night-verify ~time on GDX_DIAG_SKY).
- **`gdx-autoinput.txt`: interleave SHOT lines AMONG the button lines, tick-ascending.** A trailing
  block of `SHOT` lines after the last button line is DROPPED at parse (symptom: log says
  `autoinput script loaded: 38 events` when the file has more; the 38 = button events only, all
  SHOTs lost → zero BMPs). Root cause not chased down; interleaving is the reliable workaround.
- `s_tick` (input_bridge VI-tick, one per controller poll) ≈ the `[c3d] frame` counter ~1:1. A
  scripted race reaches the course-intro card by tick ~3300 and actual driving (`[race-dl]` present)
  by ~4600. Budget ~6-7 min wall-clock to get in-race SHOTs; drive SHOTs at ticks 4600+.
- Scanout BMP twin (`autotest/<label>_scan.bmp`) is the ONLY reliable in-game oracle — the plain
  `<label>.bmp` frame-mirror is black in-game (known B2 regression). Convert with
  `sips -s format png x_scan.bmp --out /tmp/x.png` then view.
- Watch liveness with a DEBOUNCED check (azahar drops out of `ps` momentarily during MoltenVK
  swapchain recreates — a single-miss "process gone" test false-trips and kills the run early).
- **STALE-OBJECT TRAP (cost a whole verify run 2026-08-20):** if you reset+re-apply submodule
  patches on an ALREADY-BUILT worktree, `cmake --build` may NOT recompile the changed submodule
  `.cpp` (e.g. `interpreter.cpp`) — it linked a stale `.o` and the "fix" wasn't in the binary, so
  the emulator showed no change. After re-applying patches, `touch` the patched submodule sources
  (or `--clean-first`) before building, and CONFIRM the build log shows the file recompiling
  (`Building CXX object .../interpreter.o`) not just `Built target`. A fresh worktree's first build
  is fine; the trap only bites incremental rebuilds after a patch re-apply.

## Prebaked assets (already on Azahar virtual SD)

`~/Library/Application Support/Azahar/sdmc/3ds/gdiffuser/` has fzerox.o2r + gdiffuser.o2r
(from `tools/prebake/prebake.py --rom "~/Downloads/F-Zero X (USA).z64" --skip-golden`; golden hash
differs on macOS due to zlib deflate bytes — known, unresolved). gdiffuser.ini has debug.console=1.
