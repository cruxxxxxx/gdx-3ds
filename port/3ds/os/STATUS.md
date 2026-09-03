# Stream B — os (window/HID/fibers/config)

## Done

- `gdx3ds_os_ctru.c` — real libctru backend for `gdx3ds_os.h` (3DS build only; Phase 0
  stub retained for the host build):
  - `gfxInitDefault` + double-buffered top screen, reported 400x240 per contract
  - swap = cache flush only (citro3d owns presentation/pacing); returns nonzero when an
    APT close order is pending (`aptShouldClose` — a pure flag read). `aptMainLoop()` is
    deliberately NOT called here any more: swap runs inside the interpreter's EndFrame
    (including mid-dispatch), and aptMainLoop performs the whole HOME/POWER suspend
    transition inline at its call site — that crashed hardware power-off. The frame loop
    (main_3ds.cpp) pumps aptMainLoop once per iteration at the loop top instead.
    Behavior change vs stub: START no longer exits — it is game input now; only HOME/close
    unwinds the loop.
  - full HID → N64 mapping (table below), circle pad → −80..80 with deadzone+rescale
    (config-tunable), pads 1+ reported disconnected
  - `gdx3ds_os_time_ns` overflow fix vs the stub: `tick * 1e9` overflows u64 after ~68 s
    of uptime; now split into seconds + remainder (orchestrator FYI — same bug pattern is
    in the frozen stub file, which the host build still uses, but host stub returns 0
    anyway so no action needed).
- `gdx_fiber_3ds.c` — implements `port/gdx_fiber.h` exactly (4 functions, no destroy).
  Design: one libctru thread per fiber, parked on a ONESHOT `LightEvent`; switch =
  signal target, wait self, so exactly one fiber is ever runnable. Same core as the
  converted (scheduler) thread via `svcGetProcessorID`; priority = main + 1 (lower).
  Stack size passed through to `threadCreate` (0 → 1 MB contract default). Entries that
  return hit `abort()`. `gdx_fiber_current_thread_id` returns the LOGICAL scheduler id
  for the host thread and all fiber threads (TLS-tracked), real OS id for foreign threads
  — required because `n64_sched.c`'s affinity guard compares ids from inside fibers, and
  raw per-fiber thread ids would make it spin-yield forever.
- `gdx3ds_config.{h,c}` — minimal INI loader, `sdmc:/3ds/gdiffuser/gdiffuser.ini`, no
  dependencies, fixed 64-entry table, host-testable (parser unit-checked on host).
  Header exported via `gdx3ds_os` PUBLIC include dir.
- `CMakeLists.txt` — 3DS build compiles ctru backend + fibers + config; host build keeps
  stub + config. Both builds verified green (`.3dsx` produced; host links).

## Config keys (all optional; missing file = defaults)

| Section.key        | Default | Meaning                                             |
|--------------------|---------|-----------------------------------------------------|
| input.deadzone     | 16      | circle-pad units ignored around center (raw ~±156)  |
| input.range        | 145     | circle-pad units mapped to full N64 deflection ±80  |
| input.y_maps_to_b  | 1       | 3DS Y doubles as N64 B                              |
| debug.console      | 0       | text console on the bottom screen                   |

Unknown sections/keys are stored not rejected — sibling streams may add keys (e.g.
`[audio]`) via `gdx3ds_config_get_*` without touching stream B files.

## Button mapping (final, documented in gdx3ds_os_ctru.c)

| 3DS        | N64      | Note                                            |
|------------|----------|-------------------------------------------------|
| A          | A        | accelerate                                      |
| B          | B        | brake                                           |
| Y          | B        | positional match (B is left of A on N64); config-disableable |
| X          | Z        | keeps Z reachable on Old3DS (no ZL/ZR there)    |
| L          | L        |                                                 |
| R          | R        | slide/side attack                               |
| ZL, ZR     | Z        | trigger→trigger; mirrored for either hand       |
| D-pad      | D-pad    | menus                                           |
| START      | START    | pause                                           |
| SELECT     | (none)   | reserved: future bottom-screen menu toggle      |
| C-stick    | C-UP/DOWN/LEFT/RIGHT | camera (New3DS only)                |
| Circle pad | stick    | −80..80, deadzone 16, full deflection at 145    |

## Blocked on

- Nothing. Integration items below are requests, not blockers.

## Requests to orchestrator (main_3ds.cpp is frozen for stream B)

1. Add an explicit `gdx3ds_config_load(GDX3DS_CONFIG_DEFAULT_PATH);` (include
   `gdx3ds_config.h`, exported by the gdx3ds_os target) at the top of `main()` before
   `gdx3ds_os_window_init`. Interim behavior: `gdx3ds_os_window_init` self-loads the
   config so nothing is broken today; the explicit call is for when other streams need
   keys before window init (e.g. audio buffer sizing).
2. When the scheduler is wired on 3DS (Phase 2), note `n64_sched.c` uses
   `pthread_mutex`/`sched_yield` on the non-Win32 path — devkitARM has no pthread;
   that shared-file ifdef belongs to stream E's charter (libctru `LightLock` +
   `svcSleepThread(0)` are the drop-ins). Filed here so it lands on E's list.
3. Watch item, no action yet: fiber stacks are eagerly committed on 3DS (no demand
   paging), so `gdx_fiber_create(…, 0)` costs a real 1 MB per decomp thread. If Phase 2
   memory pressure appears, have `n64_sched.c` pass explicit smaller stack sizes
   (contract already supports it).

## Next

- Citra smoke test of input/fiber behavior once a driver exists (fiber unit harness
  needs a caller; can add a dev-gated self-test behind `debug.console` if the
  orchestrator wants one — currently keeping the tree free of test scaffolding).
- Bottom-screen config UI: post-MVP per plan; SELECT is reserved for it.
