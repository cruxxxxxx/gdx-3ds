# Render thread (feat/3ds-renderthread) — progress log

Brief: docs/research/renderthread-brief.md. Audit/design: docs/research/renderthread-audit.md.
Worktree ~/code/gdx-3ds/renderthread. Build: `export DEVKITPRO=/opt/devkitpro;
cmake --build build-3ds -j8` (CIA: `--target G-Diffuser-3DS-cia`). Killswitch `[debug]
renderthread` (1 default on this branch; 0 = sequential path, byte-identical).

## M1 — audit (2026-09-02) DONE
Key finding: rendering runs INSIDE a game fiber (osSpTaskStartGo -> gdx_gfx_run), not as a host
stage, and the game's own DP-done wait (sys_gfx.c:212) after logic(N+2) is the backpressure the
N64 contract already provides. Design = intra-iteration fork (osSpTaskStartGo hands off, posts
SP-done) / join (host waits for the render thread after dispatch, posts DP-done, re-dispatches).
Host-side hazards for the overlap: gSegments (rewritten every frame by Gfx_InitBuffer), the DMA
dirty-range vector, and ~12 rare game-thread bridge mutators (fenced). See audit sections 4-7.

## M2 — sync off-main rendering (code landed; emulator run pending the Azahar lock)
- port/3ds/gdx3ds_renderthread.{cpp,h}: core 2, prio 0x24, 192 KiB, command ring
  {BEGIN,TASK,END,QUIT}, LightSemaphore consumer / sticky-LightEvent single waiter
  (clear-then-check, no spin), std::atomic acq/rel sequence numbers.
- Hooks: n64_sched.c osSpTaskStartGo -> gdx3ds_rt_submit_task (0 inline / 1 done / 2 in
  flight) + gdx3ds_rt_post_dp_done; main_3ds.cpp loop: join at top, aptMainLoop, adaptive
  vblank pace on main (gdx3ds_gpuprof_set_external_pacing -> FrameBegin(0) on the render
  thread), BEGIN, dispatch, [wait task -> DP-done -> dispatch]*, END; APT hook
  `[apt] render thread parked (idle=1)`; teardown `[exit] render_thread_join enter/done`.
- Console safety: thread-local `gdx_port_log_console_muted` honored by port_log.h's stderr
  echo and GFX_C3D_LOG (svc + filelog sinks unchanged).
- Mode default: `renderthread_sync=1` until M3 flips it; receipt `[rt] mode=sync ...` on
  the verbose cadence (frame & 63 == 5).
- Runner: /tmp/rt-art/run.sh <tag> <renderthread> <sync> <secs> [autoinput]; artifacts
  /tmp/rt-art/<tag>/{log.txt,autotest/,gdiffuser.ini,status.txt}.
## M3 — pipelined fork/join (code landed b3cffc8 + ack fix; emulator runs in progress)
- pipe = default (`renderthread_sync=0`); TASK/END asynchronous; the host joins after
  dispatch: gdx3ds_rt_wait_task = wait render done -> merge segment claims -> post DP-done
  ONCE (sTaskAcked) -> re-dispatch. task_pending() means "DP-done not yet delivered", so a
  game-thread fence (which waits without acking) can never strand the game's DP wait.
- Bridge (port/n64_gfx_bridge.cpp): `gSegments` -> GdxSegTable() (thread-local per-task
  view installed by the render thread via gdx_gfx_segment_view_set; game thread = live
  array); gdx_gfx_segment_claims_merge at the join; LightLock on gDmaDirtyRanges
  (RecordHostWrite / gdx_record_dma_load / HostRangeChanged); reserve() on the append-only
  range tables; GdxRtFence() in gdx_invalidate_texture_address, the native-RGBA16 range
  registrations, the transition capture note/read, venue loads, and the asset first-load
  append path (EnsureAssetSegmentImage slow path only -- the per-frame Segment_SetAddress
  fast path never fences).
- DBG tab row 16 "RENDER THR core-2 pipeline (reboot)" toggles [debug] renderthread.
## M5 — verification (Azahar; multi-core emulation is time-sliced on one host thread, so
## wall/dsp are NOT perf evidence — engagement, correctness, parity and heap only)
- pipeA (renderthread=1 pipe, A-mash 30-machine GP, 330 s, frames to 10881, 121 race windows):
  ZERO error lines (gfxfail/gdl-bad/gdl-miss/datafail/fatal/bad_alloc/texdiag/sched
  WARNING/[rt] ERROR); `[rt] mode=pipe ... tasks=64` on every race window (165/165 windows
  with tasks>=60), waitRender ~1 ms (render thread rarely starved), fences=11 over the run
  (asset first-loads at mode changes), segMerge=10 at boot; heap plateau 44.74 MB.
- ctrlA (renderthread=0, same build/script, frames to 13057): zero error lines, heap 44.71 MB.
- SHOT parity pipeA vs ctrlA: menu/settings/drive1 mirror shots BYTE-IDENTICAL; course
  (animated outline) 4.9 KB, machsel (blink) 2.2 KB, drive2-4 (pack positions differ run to
  run, as in bridgecache M5) — same classes the earlier tasks documented. The *_scan
  (LCD scanout) variants differ by ~640 B on static screens: the scanout is read one
  swap earlier/later relative to the mirror now that EndFrame is asynchronous.
- Emulator timing (frame-aligned, nD>=40, 37 windows): dsp 18.7 vs 16.4 ms, wall 47.5 vs
  41.8 ms with the thread ON — the expected emulator tax (core 2 shares the host thread),
  not a hardware prediction. Hardware A/B is the user's.
- stormP (pipe, storm2.txt menu navigation, 360 s, frames to 10305): race 1 -> pause ->
  Change Course -> course select (SHOTs r1/c2a/c2b) -> race 2 -> pause -> Change Machine ->
  machine select; 96 [transition-task] lines, fences=10 (all at loads/transitions), ZERO
  error lines, no seg-epoch/skip_epoch drops, heap 44.39 MB. (The tick-based script did not
  reach c3/m4 in 360 s at emulator speed.)
- syncA (renderthread_sync=1, 150 s): `[rt] mode=sync` engaged, zero error lines (M2 receipt).
- Not testable in Azahar: HOME/sleep park receipts (`[apt] render thread parked (idle=1)`)
  and the `[exit] render_thread_join` teardown order — hardware items.
- Artifacts: /tmp/rt-art/{pipeA,ctrlA,stormP,syncA}/ (log.txt, autotest/, gdiffuser.ini) and
  /tmp/rt-art/G-Diffuser-3DS.{3dsx,cia} (build da6621e).

## M4 — APT park/join: landed with M2 (structural park at the loop-top join; receipts above).
