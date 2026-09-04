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

## M6 — ahead mode (renderthread=2), 7c8363a + docs (audit §8)
- Hardware mode=pipe log (/tmp/hw-art-1788396432): waitMain 14-15 ms = the whole render; cause
  = the game step runs inside gdx_vi_tick (host-context osSendMesg dispatches the fibers) and
  pipe posts DP-done only at render completion, so the iteration tail (swap/yield/END/HUD/
  pace/next vi_tick) serializes behind the render; TASK was queued before BEGIN (tb).
- M6: BEGIN before gdx_vi_tick; ahead mode = early DP ack + backpressure at the next submit +
  RDRAM-writer fences (segment reload, GdxSegmentSourceRead, MIO0, asset copies, capture).
  Receipt now carries ovl/jdp/jtop (main) and bg/tk/en/ql (render) ms per frame, tb count.
- aheadA (Azahar, renderthread=2, A-mash GP, 330 s, frames to 11909): ZERO error lines, race
  reached, tb=0, heap 44.66 MB; crowd windows e.g.
  `[rt] mode=ahead frame=6661 tasks=64 waitMain=21.01 ... ovl=1.69 jdp=20.69 jtop=0.32 |
  bg=0.01 tk=21.69 en=0.39 ql=0.20 tb=0` -> waitMain == tk - ovl (main waits exactly the
  render time it could not cover; the emulator's main-side work is ~1.5 ms/frame so the
  ratio, not the magnitude, is the evidence). Menus: waitMain=0.00 pace=64/0.
  Screencaptures /tmp/rt-art/aheadA/shot-t{240,300}.png.
- Fix d2622c5: the M6 fences compared raw thread ids, but game fibers are real libctru threads
  -> every fence from game code was a no-op (aheadA/pipeB windows showed fence=0/0). Now uses
  n64_sched.c's logical-id test. Fix b432036: gdx_load_venue_texture_segment runs every race
  frame; its fence moved onto the actual 0x0A rewrite (it had made ahead mode fence per frame).
- aheadB/aheadC (renderthread=2, final code): zero error lines, race reached, walk fences 17 /
  dma fences 7 per run (mode changes only), jdp engaged, tb=0. Crowd window:
  `[rt] mode=ahead frame=6661 tasks=64 waitMain=20.71 ... ovl=1.91 jdp=20.38 jtop=0.33 |
  bg=0.01 tk=21.64 en=0.40 ql=0.23 tb=0`; pipe on the same window (pipeB):
  `waitMain=22.30 ... ovl=1.10 jdp=21.34 jtop=0.95 | tk=21.39` -> in both, waitMain == tk - ovl
  (+jtop for pipe); ahead moves the loop-top join (jtop) and the iteration tail into the
  overlapped work (ovl up). The emulator's main-side work is only ~1-2 ms/frame, so the
  magnitudes are render-bound there; the RATIO is what transfers.
- stormAhead2 (ahead, storm2.txt): race -> pause -> Change Course -> course select -> race 2
  -> Change Machine; 96 transition-task lines, 19 dma fences, ZERO errors; SHOTs r1/c2a/c2b
  valid images (c2a = pillarboxed pause screen, c2b = SELECT COURSE). stormPipe2 (pipe, final
  build): zero errors, r1 byte-identical to ahead.
- SHOT parity caveat (Azahar only): every mirror SHOT logs `ReadFramebufferToCPU: display
  transfer never landed` in ALL modes including renderthread=0 (the sentinel poll's 400 ms
  bound expires; the SHOT frames cost ~900-1000 ms in every run), so a SHOT taken on a black
  fade (settings.bmp) reads whatever the freshly linearAlloc'd buffer held: black in ctrlA,
  noise in two ahead runs, black in aheadB. Shots with real content (menu/machsel/drive1/r1)
  are byte-identical between modes. Backend diagnostics now also go to the SD filelog.
- Screencaptures: /tmp/rt-art/{aheadA,pipeB}/shot-t{240,300}.png (both show the race).

## M7 — texture cache is render-thread-owned (hardware crash fix, 2026-09-05)
Round-4 hardware dump: core 2 data abort in TextureCacheLookup's LRU splice while the game
thread ran `gfx_texture_cache_clear()` from `gdx_rdram_mode_reset` (post-GP podium/venue
transition). Fix: the clear becomes a request (`gdx_texcache_request_clear`) that the render
thread drains before any lookup (job run / fallback present); address invalidations queue
through `gPendingTextureCacheDeletes` (LightLock) and drain there too; with the render thread
off (or when already on it) both happen inline as before. `lus-renderthread-texcache-owner.patch`
hooks every interpreter cache entry point with `gdx3ds_texcache_note_thread`, counted in the
`[rt]` window line as `texcacheMainMut=` (cumulative; must read 0 on hardware — the first eight
violations are logged with their kind). Remaining direct deletes: SeedFramebufferQuad (render
thread, fallback present) and the inline non-deferred path only. Clean-stack roundtrip OK.
