# DYNLOD — automatic rival-detail tier (LOCKED-60 round 3, Task J)

Branch `feat/3ds-dynlod`, worktree `~/code/gdx-3ds/dynlod`, off mainline ac2fed2.
Brief: docs/research/locked60-round3.md "Task J"; common rules: locked60-campaign.md.

## Design (commit 016aefb)
- Signal: the render task's wall per frame — one `svcGetSystemTick` pair around
  `gdx_gfx_run` on the render thread (`port/3ds/gdx3ds_renderthread.cpp`, kCmdTask) or the
  inline path (`port/n64_sched.c`, 3DS only). No gputrace needed; both svc calls are skipped
  with the killswitch off (exact old cost).
- Controller `port/3ds/gdx3ds_dynlod.c`: tier in [floor, MINIMAL]; floor = the user's manual
  `[perf] rival_detail`. RAISE one tier when the last 4 frames average > hi (15.0 ms) and at
  least 8 frames passed since the last change (dwell — a change must land before it is
  judged). LOWER one tier after 30 consecutive frames < lo (12.0 ms). Floor raised by the user
  above the tier snaps the tier up; killswitch off collapses to the floor at once.
- Hook: `gdx_rival_detail_level()` (gdx3ds_menu.c) now returns
  `gdx3ds_dynlod_effective_level(floor)`; `gdx_rival_detail_floor()` is the raw setting. The
  decomp patch is untouched (Racer_Draw already reads the level once per frame) — NO decomp
  change, no new patch.
- Ini (`[perf]`, all read at init; auto is live via the menu): `rival_detail_auto` (default 1,
  KILLSWITCH), `rival_auto_hi_x10` (150), `rival_auto_lo_x10` (120), `rival_auto_lower_frames`
  (30). Thresholds are ini-tunable for hardware without a rebuild.
- Menu: DISP tab row 24 `AUTO LOD: ON  now <tier>` / `AUTO LOD: OFF (tap toggles)` under the
  RIVAL DETAIL row; live tier repaints at ~1 Hz; toggling saves `[perf] rival_detail_auto`.
- Receipts (verbose/gputrace gate, `[race-dl]` cadence, drained by the bridge on the task
  thread): `[dynlod] auto= floor= tier= raises=win/total lowers=win/total ms=<avg task wall>
  max= n= hi= lo=`; transitions `[dynlod] raise 0->1 ms=.. floor=..` / `lower ..`;
  `[dynlod] init ...` at boot.
- Threads: tier is one aligned int, single writer (task thread), read by Racer_Draw on main
  once per frame (one-frame lag in pipe mode, harmless).

## Azahar verification
(filled in per run below)

### Run A — auto=1, shipped thresholds (hi 15.0 / lo 12.0), 400 s, 30-machine GP (build 016aefb)
Artifacts /tmp/dynlod-art/A-*. 202 `[gpu]` windows to frame 12859, ZERO error lines
(gfxfail/gdl-bad/gdl-miss/datafail/fatal/bad_alloc/texdiag), heap plateau 44.90 MB
(`[watchdog] heapUsed=44904200` x8 at the end; control comparison in run C).
Controller: `[dynlod] init auto=1 floor=0 hi=15.0 lo=12.0 lowerN=30 raiseN=4 dwell=8`; 3 raises
on boot/load frames (852 ms load frame -> 1->2), 1 lower on the title screen; in the race the
task wall is 19-23 ms in Azahar (= `[gpu] build` 16.5-18.5 + the ~3 ms bridge pre-pass, which
sits outside `build`), never < 12.0, so the tier pins at MINIMAL for the whole race (race-start
storm windows: task 42 ms, nD=88; steady race: task 19-23 ms, nD=31). Load frames between
courses show as max=~1000 ms in the window receipt (one frame; the 4-frame average absorbs it
but the tier was already at the ceiling). Azahar is ~1.2x slower than hardware on this path,
so the SHIPPED defaults are hardware-scaled; run B uses emulator-scaled thresholds via the ini
keys only to exercise both directions of the hysteresis.

### Run B — auto=1, EMULATOR thresholds hi 21.0 / lo 19.5 (ini keys only), 400 s (build 016aefb)
Artifacts /tmp/dynlod-art/B-*. 197 windows, ZERO error lines, heap plateau 44.76 MB. 10
transitions, all outside the race (boot 0->1->0, load 0->1->2, title 2->1->0, course load
0->1->2). In-race the tier stayed at MINIMAL: windows average 19.3-19.5 ms with max 21-22, i.e.
the per-frame jitter never produced 30 CONSECUTIVE frames under 19.5. That is a hardware risk
too (lo=12.0 sits near the inter-crowd task wall), so commit dd8e3a7 changes the LOWER test to a
30-frame BLOCK AVERAGE < lo (same cadence, same hysteresis, jitter-proof). Run B2 re-tests.

### Run B2 — block-average LOWER (dd8e3a7), EMULATOR thresholds hi 24.0 / lo 22.0, 400 s
Artifacts /tmp/dynlod-art/B2-*. 195 windows, ZERO error lines, heap plateau 44.77 MB, 70
transitions, ~60 of them in-race. Timeline: race-start storm (windows 3393-3585, task 42 ms,
nD=93-97) at MINIMAL; as the pack thins the tier lowers 2->1 (frame 3649, 20.7 ms) and 1->0
(3713, 19.2 ms); the heavy windows later (4545, 5185, 5505, 5761, 6401, 6657, 6977, 7297 ...
task 25-28 ms at decision) raise 0->1->2 within one window and lower back 2->1->0 one to two
windows (64-128 frames) later at ~21 ms. Both directions of the hysteresis exercised; the
cadence is bounded by dwell (8) and the 30-frame block. Emulator-only thresholds: the shipped
defaults stay 15.0 / 12.0 (hardware-scaled).

### Run C — KILLSWITCH control, [perf] rival_detail_auto=0, 320 s (dd8e3a7)
Artifacts /tmp/dynlod-art/C-*. `[dynlod] init auto=0`; receipts `auto=0 floor=0 tier=0
raises=0/0 lowers=0/0 ms=0.0 n=0` (tick pair skipped), 0 transitions, ZERO error lines, heap
plateau 44.89 MB (auto-on runs 44.77-44.90 -> flat, equal within the run-to-run band). Lever
proof on frame-aligned storm windows 3393/3457/3521/3585: C (tier 0) nD=125/122/123/117,
`[gpu] build` 43.6/40.6/40.2/40.1 vs B2 (tier 2) nD=97/96/96/93, build 38.1/35.3/35.1/35.2
(-5 ms/frame in Azahar; hardware MINIMAL was +8-9 fps hand-set). Steady race nD=42 both
(the A-tap script parks the player, rivals out of view).
SHOT parity B2 vs C at the same ticks: race00 BYTE-IDENTICAL (230 454 B); race01/02/03 differ
12.6-13.3 kB (timer digits + pack positions, the same band the bridgecache control-vs-control
runs showed). Transition screenshots (/tmp/dynlod-art/B2-shot-*-trans*.png, 0 s and 2 s after
each transition): no rival machine is in frame at any in-race transition with this script, so
the nearest-5 popping check is NOT demonstrable in Azahar with the A-tap harness — flagged for
hardware (see report).

### Run D — A HELD through the race (nearest-rival attempt), emulator thresholds, 330 s
Artifacts /tmp/dynlod-art/D-*. Zero error lines, heap 44.67 MB, 16 transitions. The pack was
in frame for two windows (4609/4673: nD=225/215, task 32 ms, tier 2) but the unsteered player
wall-crashed and RETIRED at 13 s (screenshots D-shot-170..178 show the RETIRE screen; the
52 ms / nD=23 plateau after 4801 is that screen). Conclusion: neither headless script (A-tap
parks the player, A-hold crashes it) keeps rivals near the player, so the "no popping on the 5
nearest" check is a HARDWARE item. Structural bound: MINIMAL exempts the 5 nearest rivals by
construction (decomp patch pre-pass); REDUCED biases only beyond 250 units; a 0<->1 or 1<->2
transition can move one LOD step on a neighbour that is > ~300 units away, never inside the
near band.

## Final state
- Commits: 016aefb (controller + menu + receipt), dd8e3a7 (block-average LOWER), docs
  cf1c589 / 603b921 / 9d16db3 / this one. No decomp change, no new patch, README untouched.
- Artifacts at HEAD code (dd8e3a7): build-3ds/port/3ds/G-Diffuser-3DS.3dsx (2 994 696 B) and
  .cia (1 749 952 B); copies in /tmp/dynlod-art/. HUD build id feat/3ds-dynlod@9d16db38.
- Shipped defaults: `[perf] rival_detail_auto=1`, hi 15.0 ms, lo 12.0 ms, 30-frame block,
  4-frame raise average, 8-frame dwell. Emulator test setting ONLY: hi 24.0 / lo 22.0.

## Hardware test (New 3DS, gputrace=1 verbose=1 filelog=1)
1. Default ini (auto on, rival_detail=0). 30-machine GP. In log.txt grep `[dynlod]`:
   expect `init auto=1 floor=0 hi=15.0 lo=12.0`, then per window `tier=2` while `[gpu]
   build` > ~12 ms (crowd; task ms ~= build + br), `lowers` > 0 and `tier=0/1` once the pack
   spreads and `ms` reads < 12.0. If `lowers` stays 0 for a whole race while `ms` sits at
   12-14 between crowds, raise `[perf] rival_auto_lo_x10` to 140 (no rebuild).
2. Watch the 5 nearest machines across a `raise`/`lower` line (transition lines are in the
   log): no LOD pop inside the near band. If a pop is visible, set `rival_detail=2` as the
   floor (auto then does nothing) and report.
3. Killswitch: `[perf] rival_detail_auto=0` -> `[dynlod] auto=0 tier=0 n=0`, old behavior.
4. DISP tab: `AUTO LOD: ON  now <tier>` updates live; tap toggles + saves.
