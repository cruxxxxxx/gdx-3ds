# CROWD-GRIND-2 — fresh post-combiner-fix profile + lever verdicts (2026-08-27)

Branch `feat/3ds-crowd2` (worktree `~/code/gdx-3ds/crowd2`, off `feat/3ds-hwaudio`
@ 5c973d2 + full 38-patch stack). All numbers Azahar/New3DS profile, stereo ON, gputrace on,
scripted GP race (A-mash autoinput, 30 machines), 64-frame [gpu]/[prof]/[profop] windows.
Baseline log: `/tmp/crowd2-baseline-log.txt`; lever-1 log: `/tmp/crowd2-art/lever1-log-final.txt`
(runs are frame-aligned: the same window frames 3393/9985/16577 appear in both).

## Fresh baseline — in-race crowd windows (draws>120, 30-machine pack)

Typical crowd window (frames 3393 / 9985 / 16577), per-frame ms:

| bucket | ms | notes |
|---|---|---|
| build (proc) | 39–42 | worst burst 60.8 (draws=188, nT=3812) |
| br (bridge XLATE pre-pass) | 11.5 | ConvertRoot DL translation — BIGGEST single bucket |
| dsp (dispatch remainder) | 18.2 | |
| tri (per-tri geometry) | 10.2 | ~1100 tris/frame ≈ 9 µs/tri (emu) |
| drw (DrawTriangles) | 7.2 | ~150 draws/frame ≈ 46 µs/draw, stereo = 2×draw + 2×target switch each |
| vtx | 3.1 | imp 1.7 (imports are cheap cache hits: 137/frame ≈ 12 µs) |

[profop] crowd: `06=12.5/424  E4=4.9/79  F3=4.0/256  01=3.6/147  FA=2.6/71  FB=1.5/62`.
Menu/machine-select storm windows (draws=201): identical pre/post lever — untouched.

## Lever 1 (SHIPPED): SETTILE/LOADBLOCK memo-thrash — `lus-crowd2-tilestate-value-gate.patch`

Commit d3a9820. Eviction/LOADTLUT/image-rect sites arm `textures_changed` themselves
(the prerequisite invariant), then SETTILE/SETTILESIZE value-gate their re-arm+dirty,
loaders move to conditional dirtying, and the same-content load skip stops arming.

Measured (same-frame windows, base → lever1):
- crowd 3393: build 42.3 → 41.1 (−1.2 ms); tri 10.28→10.00, drw 7.27→6.94, vtx 3.12→2.97
- crowd 3521: 39.0 → 37.9; 9985: 41.7 → 40.6; 16577: 39.7 → 39.1
- steady windows: ~0 to −0.3 ms; whole-run build mean −0.6 %
- burst window unchanged in magnitude (60.8 → 60.5)

Why small: crowd counts show nI≈137 imports vs nD≈150 draws — the 30-machine cycle's
SETTILE/LOADBLOCKs are overwhelmingly REAL content switches through the same TMEM slots,
so the value gates rarely fire in-race (F3 count unchanged 256→257). The win is the
redundant re-assertion tax only. Verified: GP crowd SHOTs pixel-correct (HUD, liveries,
portraits, minimap, effects), deterministic vs prior run (identical race timer at the same
autoinput tick), menus numerically byte-identical, audio lockstep, zero log errors.

## Backlog verdicts against the fresh profile

1. TRI2/S7 memo thrash — DONE (above). Structural remainder of 06 is per-tri geometry
   (tri section), not state resolution: memoization is exhausted here.
2. F3 content-addressed TMEM slots — DEPRIORITIZE. F3=4.0 ms with ~137 real switches/frame;
   content-addressing only saves the ≤4 KiB mirror memcpy on repeat switches (mirror readers
   are all `resource == nullptr`-gated raw paths, so a metadata-only span restore IS viable),
   estimate <1 ms emu for high bookkeeping risk.
3. Stereo right-eye batching — TOP REMAINING HW-REAL LEVER. Current loop in
   `port/3ds/gfx/gfx_citro3d.cpp` (~line 2176) does per-draw L-draw, BindTargetRaw(right),
   R-draw, BindTargetRaw(back) = ~300 framebuffer switches/frame in crowd. On PICA200 every
   switch is a fragOp flush — likely the real hardware crowd cost, and emu underprices it.
   Recommended design: ONE double-height (400×480) color+depth target, per-eye VIEWPORT
   offset instead of target switches (viewport/scissor are cheap registers; scissor needs a
   per-eye y-offset re-emit), split top/bottom halves into the two scanout buffers at present.
   Needs real-hardware verification — do not trust emulator deltas for this one.
4. dsp remainder — decomposed: F3≈4, E4≈4.9 (79 crowd texrects at 61 µs incl. child draws),
   FA/FB≈3.9 (prim/env value-change flushes; cost is the batch break, counted via child
   DrawTriangles). No single cheap target left; batching (fewer draws) is the theme.

## NEW: br (bridge XLATE) = 11.5 ms is now the biggest single bucket

`port/n64_gfx_bridge.cpp` ~9484-9595: BR brackets exactly the ConvertRoot walk
(~3400 race-DL commands/frame ≈ 3.4 µs/cmd emu). Before attacking, run one diagnostic
pass with the GDX_PERF sub-phase telemetry (`GDX_GATE_PERF`, gdx_perf.h seams
XLATE/SETUP/POST/FBMIRROR) to split translation vs cache sweeps vs mirror; the gate is
CVar/env driven and needs a 3DS wiring check (`gdx_dev_gates_bind_cvars`).

## Session hazards worth keeping

- Multiple agents share the Azahar SD: another worktree's run DELETED this session's
  baseline SHOT BMPs and one run log mid-session. Always copy artifacts to /tmp
  IMMEDIATELY at collection (the redo script `/tmp/crowd2-verify-run2.sh` snapshots the
  log every 25 s during the run), and never trust `sdmc/autotest` or `log-*.txt` to survive.
- The timetrial agent's autoinput leaves the mode-select column moved; an A-mash script
  can enter the wrong mode. Verify mode via the position indicator (30/30 = GP) in SHOTs.
- /tmp/inspect.py (some agent's Blender leftover) shadows Python's stdlib `inspect` when
  cwd=/tmp — run Python elsewhere.
