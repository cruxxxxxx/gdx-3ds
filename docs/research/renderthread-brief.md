# TASK H — render thread on core 2 (the locked-60 architecture) — brief (2026-09-03)

Branch feat/3ds-renderthread, worktree ~/code/gdx-3ds/renderthread (off mainline
feat/3ds-hwaudio @ d980f78, 47-patch stack applied, build-3ds built). Common rules: see
docs/research/locked60-campaign.md (copy from <scratch>/locked60-campaign.md,
commit first). Killswitch `[debug] renderthread` (default 1 on this branch; 0 = today's
sequential path byte-identical). Progress file docs/research/renderthread-progress.md — update
at every milestone; commit early and often. Branch only; the user's hardware verdict gates merge.

## Why (hardware evidence, New 3DS, crowd frames)
`[gpu] wall=17-22 build=10.5-12.8` => game logic ~7-9 ms + render ~11-13 ms, SEQUENTIAL on
core 0. Pipelined across cores the frame costs max(logic, render) ~ 12-13 ms < 16.7 ms even in
crowds: a locked 60 with margin. Deep research (docs/research/*research*): core 2 is usable
(Luma grants the 0x2000 kernel capability to every 3dsx; hbldr exflags 0xFF002109), idle apart
from our two audio threads (prio 0x18), core 3 is unusable (QTM/GSP starvation), core 1 only
fractional. The kernel scheduler is strict-priority with NO timeslicing: a busy render thread on
core 2 must sit at a LOWER priority than the audio threads (e.g. 0x20-0x28) so audio always wins;
never spin-wait, always block on LightEvent/LightSemaphore. No N64 port has ever done this —
sm64-3ds offloads only audio — so there is no precedent to copy; the audit is the design.

## Today's frame (port/3ds/main_3ds.cpp frame loop ~L860-1050, all core 0)
aptMainLoop -> HandleEvents/input -> gdx_vi_tick (runs the game fibers inline: game logic until
it hands over a display list / waits VI) -> gdx_audio_thread_notify_frame -> w->StartFrame
(C3D_FrameBegin) -> gdx_dispatch (bridge pre-pass + interpreter + citro3d draws; port/
n64_gfx_bridge.cpp gdx_gfx_run, libultraship interpreter, port/3ds/gfx/gfx_citro3d.cpp) ->
gdx_disk_save_tick -> gdx_vi_present_fallback -> w->EndFrame (C3D_FrameEnd, present) -> menu
tick -> frame pacer. The game fibers are real threads parked on LightEvents, switched only by
the main thread (port/3ds/os/gdx_fiber_3ds.c). The game double-buffers its GfxPool per frame.

## Design (adapt with evidence; document every deviation)
1. AUDIT first (milestone 1, commit the doc): every GPU/GSP/GX touch outside gdx_dispatch
   (transition LCD capture "[transition] capture source=lcd-scanout", stereo target binds,
   texture uploads GX_TextureCopy, C3D_FrameBegin/End, gfxFlushBuffers, the bottom-screen console
   and FPS HUD writes, the menu's GSPLCD calls, gdx_vi_present_fallback, interp P0/P1); every
   RDRAM range a frame's display list references that the NEXT frame's game logic may write in
   place (GfxPool half = safe by double-buffering; matrix pools? dynamic vertex buffers? HUD
   scratch? segment reloads via DMA — the bridge's gDmaDirtyRanges/gGdxSegmentEpoch guards exist,
   verify they hold cross-core with acquire/release); the audio coupling
   (gdx_audio_thread_notify_frame, gdx_vi_tick VI retrace posting, osViGet*Framebuffer waits);
   APT hooks (suspend/sleep/close must park/join the render thread at a frame boundary, like the
   audio park gate in gdx3ds_audio_ndsp.c); the frame pacer and watchdog stages.
2. Milestone 2 — off-main rendering, NOT yet pipelined: a render thread on core 2 (prio ~0x24,
   64-128 KiB stack; check the interpreter's stack depth: the bridge/interpreter recursion and
   the clip fan) executes StartFrame+gdx_dispatch+EndFrame for the SAME frame while the main
   thread blocks on its completion event. Proves GPU submission from core 2 works on hardware
   (C3D/GX/gsp handles are process-wide; only one thread may drive citro3d) with zero behavior
   change. Receipt `[rt] mode=sync`.
3. Milestone 3 — pipelining by one frame: main hands frame N's display list (root, segment
   snapshot, GfxPool half id, any copied dynamic ranges) to a 1-deep mailbox and immediately
   proceeds to logic(N+1); the render thread renders N; BACKPRESSURE: main must not start
   logic(N+2) until render(N) completed (the pool half N is about to be rewritten). VI retrace /
   framebuffer-swap semantics: the game's wait for "frame presented" must be satisfied by the
   pipeline without stalling logic — study gdx_vi_tick + gdx_interp; the game must see a steady
   60 Hz VI. Receipt `[rt] mode=pipe waitMain=ms waitRender=ms latency=1`.
4. APT: ONSUSPEND/ONSLEEP park the render thread between frames (bounded wait for the ack,
   same shape as gdx3ds_audio_suspend), ONRESTORE resume, close order joins it before teardown
   (see the [exit] receipts in main_3ds.cpp; the close-from-HOME hang history is in
   docs/research/home-crash-audit.md — same bug class, avoid the shared-event spin).
5. Everything off-thread that touches the console/HUD stays on main (printf is not
   thread-safe); the FPS row keeps working; the menu keeps working; transitions keep working
   (route the LCD capture through the render thread or fence it).
6. Cache coherence: ARM11 MPCore keeps L1 data coherent via the SCU for normal cacheable
   memory; linearAlloc buffers for GX need the existing flushes. Use std::atomic with
   acquire/release for the mailbox; no volatile-only handshakes.

## Verification
Azahar (multi-core emulation; not a perf oracle): renderthread=1 vs 0 — boot, menus, machine
select, race, the 3-course + machine-swap menu-navigation script (docs/research/
bridgecache-progress.md M5 has the grammar), transitions, zero error lines, heap flat, SHOT +
`screencapture -x` race parity, `[rt]` receipts showing the pipeline engaged and both waits
small. Then .3dsx + .cia. Hardware A/B is the user's (killswitch in the DBG tab if cheap: a
"RENDER THREAD" row following the LEVERS pattern in port/3ds/gdx3ds_menu.c; note it needs a
relaunch). Report under 50 lines: audit findings, design as built, receipts, emulator parity,
risks, exact commits, hardware test plan.
