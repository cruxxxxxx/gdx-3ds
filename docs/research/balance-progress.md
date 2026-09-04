# Balance the cores -- bridge pre-pass on main (feat/3ds-balance) -- progress log

Brief: docs/research/locked60-round3.md Task I. Base: feat/3ds-renderthread @ 5e1672a (render
thread, modes 1 pipe / 2 ahead). Worktree ~/code/gdx-3ds/balance. Build: `export
DEVKITPRO=/opt/devkitpro; cmake --build build-3ds -j8` (CIA: `--target G-Diffuser-3DS-cia`).
Killswitch `[debug] bridgemain` (default 1; only meaningful with renderthread>=1; 0 = the TASK
carries the raw DL and the render thread runs the whole gdx_gfx_run, byte-identical to before).
DBG tab row 18 "BRIDGE MAIN" toggles it (reboot). Receipt: `[rt] ... brMain=<ms/frame> bm=<tasks
prepared on main> relWait=<release-before-walk waits> | ... tk=` and `[rt] mode=... bridgemain=1`
at init.

## M1 -- design + code (2026-09-02)

Where the bridge goes: osSpTaskStartGo -> gdx3ds_rt_submit_task now runs `gdx_gfx_job_prepare`
(SETUP + XLATE = adapter construction, ConvertRoot/ProcessList, matrix fixups, walk receipts)
on the SUBMITTING thread (the game fiber, 128 KiB stack -- the same stack the sequential path
walked on) BEFORE the backpressure wait for the previous render, so bridge(N+1) overlaps the
tail of render(N) -- the `jdp` wait was exactly main's slack. Expected: main = logic + br
(~8 + 3), render = interpreter + draws only (~12-15 - 3); frame = max(11, 9-12) instead of
max(8, 12-15). Doing it AFTER the wait would not pay (frame = br + max(logic, render - br)).

gdx_gfx_run (port/n64_gfx_bridge.cpp) is split into three stages around a heap job
(`struct GdxGfxJob`: dl/size/ucode, the private segment view, interpSegs, ConversionStats, the
adapter (unique_ptr), converted root, job-owned persistent allocations, job-owned deferred
native-RGBA16 clears):
- `GdxGfxJobPrepare` (submitter): everything up to and including the [prof] BR bracket + the
  [venueload] probe, plus taking the pending native-RGBA16 clears and the walk-side receipts.
- `GdxGfxJobRun` (render thread): arms the interpreter (renderer ucode, F3DEX2 variant,
  mSegmentPointers from job.interpSegs -- the table after the Ensure* claims, exactly the values
  the old code set before the walk), drains the texture-cache deletes, [race-dl]/interp
  receipts, Run + replays, frame mirror.
- `GdxGfxJobRelease` (submitter, after the render completed): native-RGBA16 retirement (was
  post-Run), persistent-copy frees (was post-Run), adapter destruction (ConvertedList recycle).
- `gdx_gfx_run` = prepare + run + release on one thread (renderthread=0, bridgemain=0, desktop).

Ownership made explicit (the hazards of the brief):
- gSegments view: prepare installs the job's snapshot as the submitter's thread-local view; the
  render thread installs the SAME array for run; the join merges it (unchanged merge). The walk's
  MOVEWORD writes and zero-slot claims therefore land in the job view as before.
- ListFacts / resolve memos / range-class memo / placeholder memo / wide cache / [brop]
  accumulators: walk-only -> owned by whichever thread walks (one walker at a time by
  construction: the submitter in bridgemain, the render thread otherwise). No sharing.
- 3DS memory-region cache (svcQueryMemory memo): per-thread slots (the render thread's
  [race-seg]/capture probes vs the walk).
- ConvertedList recycle pool: touched only by adapter ctor/dtor = prepare/release, both on the
  submitter (release happens where main first observes the render complete: WaitTaskDone).
- Persistent copies (MakePersistentVtx/MtxCopy, raw texture-copy resize retirees): pushed into
  the current job's vector (thread-local pointer), freed at that job's release. The old global
  post-Run clear would have freed N+1's copies while N rendered.
- Raw texture-copy in-place refresh (content changed): GdxRtFence() first -- render(N) may still
  import from that buffer. Rare (DMA-changed content; the DMA itself fenced in ahead mode).
- gPendingTextureCacheDeletes: LightLock around push (walk / fenced game thread) and the
  render-thread drain (swap under the lock).
- gDmaDirtyRanges: existing LightLock; in bridgemain both writer and scanner are main.
- Deferred native-RGBA16 clears: taken into the job at prepare (pusher = game thread = same
  thread), retired at release. If the in-flight job carries clears, the next submit waits for
  it and releases BEFORE walking (`relWait=`), so the next walk classifies textures against the
  same tables the sequential path would. Transition frames only.
- [prof] BR bracket: in bridgemain the split walk does NOT touch the render thread's [prof]
  accumulators (`[prof] br` reads ~0 there); the submitter times the whole prepare and reports
  `[rt] brMain=` (per frame). `[gpu] build` shrinks by the same amount.
- Walk-side census lines ([wide]/[bcache-census]/[brfast]/[brop]) are emitted by the walking
  thread (`GdxEmitWalkReceipts`, own 64-task cadence) so the render thread never reads/resets
  the walk's counters; [race-dl] + the interpreter receipts stay on the render thread.
- Segment-claim skew: prepare(N+1) snapshots the live table before render(N)'s claims are
  merged (when N is still running); a slot N claimed is then re-derived by N+1's walk (same
  value; the first-load slow path fences). When render(N) is already over at submit time the
  submitter observes it (merge + release) first, so the common case is identical to today.
- Audio notify / menu tick: unchanged (audio threads are core-2 prio 0x18 condvar-driven); the
  bridge adds ~br to main's iteration, which the receipts show as ovl growing.

Build green (`cmake --build build-3ds -j8`), commit below. Emulator verification: M2.

## M2 -- emulator verification (Azahar, 2026-09-02; multi-core emulation is time-sliced on one
## host thread, so the magnitudes are NOT hardware predictions -- engagement, correctness,
## parity, heap, and the RATIOS are the evidence)
Runner /tmp/balance-art/run.sh <tag> <renderthread> <bridgemain> <secs> [script]; artifacts
/tmp/balance-art/<tag>/{log.txt,autotest/,gdiffuser.ini,shot-t240.png,shot-t300.png}; analyze.sh /
compare.sh alongside. Build ba54912 (label shows 2c83a2c: configure-time string).
- bmA (renderthread=2 bridgemain=1, A-mash 30-machine GP, 330 s, frames to 14081): ZERO error
  lines (gfxfail/gdl-bad/gdl-miss/datafail/fatal/bad_alloc/texdiag/sched WARNING/[rt] ERROR);
  `[rt] mode=ahead ... bridgemain=1`; every race window `bm=64` (13752 tasks prepared on main),
  `brMain=3.6` ms/frame, `[prof] br=0.00` on the render thread (the bracket moved), `tb=0`,
  relWait=6 over the run (boot + race-start transitions), walk fences 15. Heap plateau 45.78 MB.
- ctrlB (renderthread=2 bridgemain=0, same build/script, frames to 11777): zero error lines,
  `[prof] br=4.0-4.3` back on the render thread, `bm=0 brMain=0.00`, heap plateau 44.73 MB.
- Frame-aligned crowd windows (nD=169, frames 2049-2305): `br 0.00 vs 9.1`, `dsp 28.3 vs 28.2`
  (interpreter cost untouched), `[gpu] wall 66.6 vs 76.0` (the emulator serialises the cores, so
  wall drops by the whole br there); 108 aligned windows nD>=40: mean wall 26.9 vs 31.7.
  Late race windows: `tk 18.7-19.6 vs 21.5` (tk shrank by ~br), `ovl 6.2 vs 1.9` (the bridge is
  now overlapped main work), `waitMain 13.6-14.4 vs 20.5-20.7`; waitMain == tk - ovl in both.
- Heap: +1.05 MB plateau with bridgemain (two jobs alive: N rendering, N+1 converted), flat over
  the last 40 windows in both runs.
- Parity: screencaptures /tmp/balance-art/{bmA,ctrlB}/shot-t{240,300}.png (race with 30 machines,
  HUD, minimap; machine-select) visually identical between modes; mirror SHOT .bmp files are the
  documented Azahar caveat (renderthread-progress.md M6: the display transfer never lands, so a
  SHOT reads the freshly allocated buffer -- drive4/menu are the same blank md5 in both runs).
- stormBM (bridgemain=1, storm2.txt: race -> pause -> Change Course -> course select -> race 2
  -> Change Machine, 360 s, frames to 11525): ZERO error lines, 96 [transition-task] lines, 22
  dma fences, relWait=7, no seg-epoch drops, heap plateau 45.06 MB; screencaptures show race 2
  and SELECT MACHINE rendering correctly.
- No libultraship/decomp changes (port-side only): no new patch, README apply list unchanged.
- Base: feat/3ds-renderthread is still at 5e1672a (M6 included) -- no rebase needed.

## Hardware expectations (what to watch in log.txt, ini: renderthread=2 bridgemain=1 vs 0)
- `[rt] ... brMain=~3-4 bm=64` on race windows with bridgemain=1 (0.00/0 with 0); `[prof] br`
  ~0 with 1 (3.2-3.8 with 0); `[gpu] build` and `tk` down by ~br (12-15 -> ~9-12 ms).
- `waitMain`/`jdp` down by ~br while main stays under the render (logic ~8 + br ~3.5 = ~11.5 ms
  vs render ~9-12): expect waitMain ~0-1 ms on lighter crowd windows, `ovl` up by ~br. If
  waitMain hits 0 and `waitRender` grows, main became the bottleneck (frame = logic + br) and
  the remaining lever is the walk itself (brfast/bcache).
- relWait should be 0 on race windows (transitions only); tb=0; zero error lines; heap plateau
  ~+1 MB vs bridgemain=0.
