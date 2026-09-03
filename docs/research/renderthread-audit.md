# Render thread on core 2 — M1 audit (feat/3ds-renderthread, 2026-09-02)

Brief: docs/research/renderthread-brief.md. This document IS the design: every claim below was
read from the code on this branch (line numbers are for the 47-patch tree at 2bdde85).

## 1. What actually runs where today (the brief's picture is one level too coarse)

`gdx_dispatch` (port/n64_sched.c:839) does NOT run "bridge + interpreter + draws" as a stage:
it runs the game's cooperative fibers until they all block. Rendering happens INSIDE a game
fiber: the game's scheduler thread (decomp/src/sys/sys_main.c:384-440) receives
EVENT_MESG_GFX_TASK_SET -> Sched_SpTaskClearStartGfx -> `osSpTaskStartGo` (n64_sched.c:1120)
-> `gdx_gfx_run` (bridge pre-pass + LUS interpreter + citro3d draws, synchronous) -> posts
SP-done (gMainThreadMesgQueue) and DP-done (D_800DCAC8) itself.

The game's own frame function (sys_gfx.c:203-231 `func_80067D64`, per VI):
```
wait VI msg (D_800DCAB0) -> Audio_Update -> Gfx_InitBuffer (flip GfxPool half, gSegments[1]=pool)
-> game logic (func_800690FC) -> build DL (func_80069698) -> Gfx_FullSync
-> WAIT DP-DONE (D_800DCAC8)  <== the game's own "previous frame rendered" fence
-> Segment_LoadAssets -> Transition_SetBackgroundBuffer (LCD capture) -> osViSwapBuffer
-> spin osViGetCurrentFramebuffer (gdx_yield -> host iteration ends)
-> Gfx_SetTask (-> osSpTaskStartGo next iteration)
```
So, per HOST iteration (main_3ds.cpp:878-1046) in steady state the game executes
`SetTask(N+1)` FIRST (the fiber resumes in the fb spin after gdx_vi_tick advanced the
framebuffer, and the VI message posted at this iteration's gdx_vi_tick is already queued), then
`logic(N+2)` into the other pool half, then blocks on DP-done(N+1). The N64 hardware contract the
game is written against is exactly "logic(N+2) overlaps RSP/RDP rendering of N+1; anything the
DL references is stable until DP-done": every RDRAM mutation the game makes outside the pool
(Segment_LoadAssets, Transition_SetBackgroundBuffer, capture reads) sits AFTER the DP wait.

DESIGN CONSEQUENCE: the pipeline is intra-iteration fork/join, not a cross-iteration mailbox.
Fork = osSpTaskStartGo hands the task to the render thread and posts SP-done only; join = the
host loop, after dispatch returns with a task in flight, waits (LightEvent, no spin) for the
render thread and posts DP-done, then re-dispatches so the game finishes swap+SetTask. The
GfxPool half N+1 is rewritten only at Gfx_InitBuffer(N+3), after DP-done(N+1) was consumed, so
the game's double buffer + DP-done IS the backpressure; no dynamic-range copies are needed.
Expected per-iteration cost = max(logic, render) + SetTask/swap overheads. The
`[cadence] task~64 hold~0` line on hardware confirms one task per host present in races.

## 2. GPU/GSP/GX touches outside gdx_gfx_run (all must move to, or be fenced against, the render thread)

| site | today's thread | rt disposition |
|---|---|---|
| `w->StartFrame()` main_3ds.cpp:907 -> Interpreter::StartFrame -> GfxRenderingAPIC3D::StartFrame (gfx_citro3d.cpp:1104): gdx3ds_gpuprof_frame_begin owns C3D_FrameBegin (SYNCDRAW vblank wait, adaptive skip gdx3ds_gpu_prof.c:341-390), C3D_FrameDrawOn, stereo FrameBegin (osGet3DSliderState + right-target queue) | main | render thread, BEGIN command; vblank pacing moves to main (FrameBegin(0) on the render thread) |
| `gdx_vi_present_fallback` main_3ds.cpp:913 (bridge:8880): fb0 bind, hold recomposite, VI-scanout quad upload + draw, mirror refresh | main | render thread, inside END command (after any tasks) |
| `w->EndFrame()` main_3ds.cpp:915: FlushPendingVbo (GSPGPU_FlushDataCache), C3D_FrameEnd(0), gdx3ds_os_window_swap (gfxFlushBuffers + aptShouldClose latch) | main | render thread, END command |
| transition LCD capture `gdx_read_current_framebuffer` (transition.c:713 -> bridge:8470 3DS scanout read / ReadFramebufferToCPU:3145 with C3D_FrameSplit + C3D_SyncDisplayTransfer + sentinel poll) | game fiber (main), after the DP wait | fence: wait for the in-flight task before reading (idle by game order; the fence is insurance) |
| SHOT `gdx_request_frame_dump` (input_bridge.c:371 / gdx_input_script.c:443) arms a label; the readback runs in GdxUpdateFrameMirror at the end of gdx_gfx_run | arm: main; read: inside gdx_gfx_run | arm on main (plain flag), consumed on the render thread: same as today |
| `gdx_gfx_post_run_capture` (interp shot) called from Fast3dWindow::DrawAndRunGraphicsCommands only | n/a on the 3DS loop | unchanged |
| GX_TextureCopy frame mirror (gfx_citro3d.cpp:3114, async) / C3D_FrameSplit / stereo | inside gdx_gfx_run | render thread |
| `[present]` scanout oracle main_3ds.cpp:924 (CPU read of gfxGetFramebuffer) | main, verbose only | main; racy pixels are diagnostic-only |
| bottom-screen console: fps HUD (gdx3ds_fps_hud.c:44 fwrite stdout), menu (gdx3ds_menu.c:162), logStep printf, `gdx_port_logf` stderr echo (port_log.h:157), GFX_C3D_LOG stderr (gfx_citro3d.cpp:57) | main + whichever thread logs | console stays main-only; render-thread logs must skip the stderr echo (thread-local suppression); svc + filelog sinks are thread-safe (filelog has a recursive lock) |
| menu GSPLCD backlight (gdx3ds_menu.c:373-394), APT hooks | main | unchanged (render thread is idle at the loop top, see section 6) |
| `gdx3ds_gpu_prof.c` statics ("all entry points run on the render thread, no locking") | main | move wholesale with StartFrame/EndFrame; GpuProfLog is svc + filelog only |
| interp P0/P1/P2 (bridge:4569-4571) | n/a: never armed on 3DS (dev gate / CVar off; `gdx_gfx_interp_tick_config` is never called from main_3ds.cpp) | unchanged; rt mode refuses to engage if interp is active |

## 3. RDRAM / host data the DL references and who writes it during logic(N+2)

- GfxPool half (DL, matrices, vertices — the bulk; bcache-progress.md: track/machine geometry is
  host-built into the pool every frame): double-buffered by the game (sys_gfx.c:118-126), safe.
- EK segment 6 (`D_80128C90[D_800DCCFC]`, sys_gfx.c:131): double-buffered the same way.
- Static assets / segment carves: rewritten only by Segment_LoadAssets (after the DP wait) and the
  mode-reload path decomp_port.c:1280-1345 (bracketed by the `gGdxSegmentEpoch` seqlock, bridge:78,
  already `std::atomic` acq/rel; the walk drops SETTIMG on an unstable epoch) — N64-legal timing.
- `gdx_fixup_asset_segment_image` in-place rewrites (minimap.c, sys_gfx.c, decomp_port.c): same
  class as the N64 RDP reading a texture the CPU rewrites; the game orders them itself.
- N64 framebuffers (gFrameBuffers, transition source): read by the game only after the DP wait.
- Native RGBA16 capture ranges (`gNativeRgba16Ranges`, transition.c registrations): host vector
  mutated from the game thread — see section 4.

## 4. Host-side shared state (the real hazard; the N64 contract does not cover it)

Writers on the GAME thread (main) that the bridge walk / interpreter read on the render thread:

| state | game-thread writers | render-thread readers | M3 mechanism |
|---|---|---|---|
| `gSegments[16]` (decomp_port.c:502; 93 refs in the bridge) | Segment_SetPhysicalAddress/SetAddress (decomp_port.c:686-720) at Gfx_InitBuffer EVERY frame (segment 1 = the NEW pool half!) and Segment_SetTableAddresses | every segmented resolve; in-walk G_MOVEWORD writes (bridge:7568/7849); zero-slot first-load claims (:1146/:1351/:2089) | job-private snapshot taken at submit (thread-local table view in the bridge); zero-slot claims merged back at the join (`segMerge` receipt). Without this, DL(N+1) would resolve segment 1 into pool N+2. |
| `gDmaDirtyRanges` / `gDmaGeneration` (bridge:741-790) | gdx_record_dma_load (dma.c:126/166, object.c, mio0_wrap.c), RecordHostWrite | HostRangeChanged reverse scan (wide-cache revalidation), G2StampFor | LightLock around the vector push/erase and the scan (uncontended = no svc) |
| `gPendingTextureCacheDeletes` | gdx_invalidate_texture_address (minimap.c:201-204) | drained at gdx_gfx_run start | fence (wait for the in-flight task) — rare (minimap rebuild) |
| `gNativeRgba16Ranges`, `gPendingNativeRgba16RangeClears`, gDiagTransitionCapture* | gdx_set_native_rgba16_texture_range / gdx_defer_native_rgba16_texture_range_clear / gdx_diag_note_transition_capture (transition.c, decomp_port.c) | texture classification during the walk and Run; post-Run retirement | fence — transition-time only |
| `gHostRanges`, `gRawN64Ranges`, `gLoadedAssetSegments`, `gHostN64CommandRanges`, `gN64AddressRanges`, `gN64Framebuffers` (append-only vectors: a realloc during a concurrent scan is UB) | gdx_register_* (boot only), gdx_ensure_asset_segment_for_symbol / gdx_resolve_wide_asset_pointer / gdx_resolve_mode_segment9 / gdx_lookup_asset_segment* (decomp_port.c), gdx_load_venue_texture_segment / gdx_load_venue_building_texture (course_gadgets.c, Segment_LoadAssets), gdx_fixup_asset_segment_image, gdx_register_asset_segment_command_ranges | resolvers on every command | fence in each entry point (no lock on the hot path); the bridge already forbids `gdx_load_venue_texture_segment` on "the graphics thread" (:9395) |
| `gdx_segment_source` tables | game thread, audio thread, preload thread (core 2) | walk | already locked (gdx_segment_source.c:40-57 LightLock + fences) |
| `gdx_mq_*` message queues | game fibers (main) | render thread posts NOTHING into the scheduler: DP-done is posted by main at the join | existing cross-thread guard (n64_sched.c:66-135) unused by rt |
| `gGdxRaceActive`, `gGameMode`, dev gates, verbose latches, fixed-aspect flags | main | walk/Run | plain ints, benign staleness |
| LUS ResourceManager cache | render (SETTIMG o2r loads), game (asset key lookups) | | LUS mutex-protected by contract; verified by soak, not by code |
| gWideCache, gRawTextureCopies, gPersistentAllocations, texture cache, TMEM mirror, [prof]/[profop] accumulators | render thread only in rt mode (today: the game fiber that calls osSpTaskStartGo) | | no change; `gdx_gfx_mem_census` (main, verbose) runs only after the loop-top join |
| `gHostFrameGfxTaskRan`, `sGpuContentLive`, frame mirror | gdx_gfx_run + gdx_vi_present_fallback | | both on the render thread in rt mode |
| `gdx_cadence_*`, `gdx_yield_count`, watchdog counters | volatile counters | main reads for [cadence] | benign |

Fence = the calling thread (a game fiber on main) waits on the task-done event of the in-flight
render job, then proceeds. It is a no-op when nothing is in flight, when rt is off, or when the
caller IS the render thread (thread-local identity), so a walk-internal call to the same helper
cannot deadlock. No fence site yields to the host (the bridge has no gdx_yield call; grep) so a
fence can never be held across a fiber switch.

## 5. Audio coupling, VI, pacing, watchdog

- `gdx_audio_thread_notify_frame` (main_3ds.cpp:904) is a condvar kick; audio production runs on
  its own threads (core 2, prio 0x18) and never touches the bridge. Unchanged.
- VI: `gdx_vi_tick` (n64_vi.c:77) posts the retrace on main once per iteration — the game keeps
  seeing exactly one VI per host iteration. The render thread never posts into the scheduler.
- Pacing: today C3D_FrameBegin(SYNCDRAW) inside StartFrame paces the whole loop (adaptive skip
  when the previous fresh begin is already > 1 LCD period old, gdx3ds_gpu_prof.c:341). In rt
  mode the render thread must not own the vblank wait (it would serialize logic behind it), so
  main paces at the loop top with the same adaptive policy on `gspWaitForVBlank()`, and the
  render thread's FrameBegin uses flags 0 (gxCmdQueueWait only = GPU backpressure). Only ONE
  thread ever waits on GSPGPU_EVENT_VBlank0 (libctru clears the sticky event on wait).
- gdx_frame_pacer_tick is a no-op on 3DS (CVar default 0). Watchdog stages: a new stage 9
  ("rt join") marks the host waiting for the render thread; the [watchdog] fiber id still names
  the parked game fiber.
- Interpreter stack: gdx_gfx_run already runs on a 128 KiB fiber stack (n64_sched.c:243), so
  128 KiB is proven; the render thread gets 192 KiB.

## 6. APT / lifecycle

The APT pump is the first statement of the iteration (main_3ds.cpp:878) and in rt mode the loop
top first JOINS the previous iteration's END (render thread idle, no C3D frame open, nothing
queued). ONSUSPEND/ONSLEEP therefore always find the render thread parked on its command
semaphore — the park is structural, no ack protocol needed; the hook still records
`[apt] render thread idle=<0|1>` and, if a job were somehow in flight, waits for it (bounded,
same shape as gdx3ds_audio_suspend) before libctru's GSPGPU_SaveVramSysArea. Restore needs
nothing (no citro3d state lives on the thread). Close order: teardown sends QUIT and
threadJoins the render thread BEFORE gdx3ds_os_window_shutdown (`[exit] render_thread_join`
receipts), on its own event (no shared-event spin, per home-crash-audit.md).

## 7. Design as built (milestones)

- Killswitch `[debug] renderthread` (default 1 on this branch): 0 = byte-identical sequential path
  (no thread created, no hooks taken). If threadCreate on core 2 fails, `[rt] unavailable` and the
  sequential path runs.
- port/3ds/gdx3ds_renderthread.cpp: one thread, core 2, prio 0x24 (below audio 0x18), 192 KiB
  stack, a 4-slot command ring {BEGIN, TASK(dl,size,ucode,segs[16]), END, QUIT}; commands are
  handed over with std::atomic acquire/release sequence numbers, the render thread blocks on a
  LightSemaphore, waiters block on a sticky LightEvent re-armed per command. No spin anywhere.
- Hooks: osSpTaskStartGo (n64_sched.c) -> gdx3ds_rt_submit_task; DP-done posted by n64_sched.c's
  `gdx3ds_rt_post_dp_done` from main only. main_3ds.cpp loop: join-at-top, vblank pace, BEGIN,
  dispatch/join loop, END.
- M2 `mode=sync`: every command waited immediately (DP-done posted inside osSpTaskStartGo as
  today). Proves core-2 GPU submission with zero concurrency.
- M3 `mode=pipe`: TASK and END are asynchronous; the join after dispatch posts DP-done; segment
  snapshot + merge; DMA-range lock; fences on the game-thread mutators; console suppression.
- Receipt on the [gpu]/[race-dl] cadence: `[rt] mode=pipe n=64 tasks= waitMain=ms waitRender=ms
  fence= segMerge= latency=1`.
