# M1 integration — link status (feat/3ds-m1)

Shift goal: get the game code compiling for 3DS and drive toward a full link.

**Status: FULL LINK ACHIEVED.** `cmake --build build-3ds` produces
`build-3ds/port/3ds/G-Diffuser-3DS.elf` (+ `.3dsx` via ctr_create_3dsx) with the whole
decomp game, the port bridge layer, the LUS carve, the citro3d renderer, and the
lus_glue link stubs. **Zero unresolved symbols.** All three gates green:

| Gate | Result |
|---|---|
| devkitARM cross build (`build-3ds`, clean from scratch) | green, `.3dsx` produced |
| host `-DGDX_PLATFORM_3DS=ON` stub build | green (main_3ds.cpp keeps the Phase 0 stub loop under `#ifndef __3DS__`) |
| 64-bit host regression (`gfx_pack_tests`, `gfx_convert_tests` against the patched gbi.h) | ALL PASS |

## What compiles/links

- **gdx3ds_game** (port/3ds/game/): the full decomp game-source set — same
  glob/filters as desktop via the new shared `port/cmake/GdxGameSources.cmake` —
  plus the port game core (decomp_port.c, n64_sched.c, n64_vi.c, devmgr.c,
  AssetBindings.c, LinkStubs.c). `GDX_EXPANSION_KIT=OFF` (3DS MVP is cartridge-only;
  `gdx3ds_us_linkstubs.c` supplies the EK-provided symbols `leoBootID`,
  `gspF3DEX2_Rej_fifoTextStart`, `gspL3DEX2_fifoTextStart`).
- **gdx3ds_glue** (port/3ds/game/): the desktop G-Diffuser executable TU list minus
  desktop-only files. Dropped: main.cpp, gdx_discord, gdx_gui/menu/registry/console_log,
  UIWidgets, imgui_nav, ghost_window, input_viewer, fps_overlay, BoostDuration
  (menu-registry dependent), firstboot*/extract_launch/dump_launch (pre-bake replaces
  them), disk_buffer + EK gen TUs (EK OFF). Kept: n64_gfx_bridge, gdx_interp,
  n64_gfx_convert, gdx_vi_convert, n64_audio_hle, gdx_audio_lle (+cxd4), audio
  thread/capture, frame pacer, shims, AssetLoader, ResourceFactories, rom/sram/disk
  buffers, ghost_io, perf, dev_gates, workshop, mio0, segment_source, GameEvents.
- **gdx3ds_lus_glue** (port/3ds/lus_glue/): link-level replacements for the LUS layers
  the carve excludes — see that directory's CMakeLists header. Families closed:
  `Ship::Context` (real ResourceManager over SD archives), `Ship::ConsoleVariable` +
  CVar C bridge (real in-memory, Save/Load no-op), `Ship::Window` base,
  `Fast::Fast3dWindow` (full replacement wiring `Gdx3ds_GetCitro3dRenderer()` + a
  `GfxWindowBackend` over gdx3ds_os), libultra surface (os_cache no-ops, time on the
  N64 46.875 MHz chrono ratio, osEPiStartDma over GdxSegmentSourceRead, controllers
  over gdx3ds_os_poll_input, motor stubs), windowbridge/eventsbridge stubs,
  osAiSetNextBuffer/osAiGetLength → gdx3ds_audio (hook R5).
- **main_3ds.cpp**: real bring-up — config → fs → window → Context/CVars →
  ResourceManager(sdmc:/3ds/gdiffuser/{gdiffuser,fzerox}.o2r) → Fast3dWindow →
  gdx3ds_audio_init(0) → audio thread → factories → LoadAllAssets → sched → ROM
  (synthetic argv `sdmc:/3ds/gdiffuser/baserom.us.rev0.z64`) → RDRAM + range
  registration → audio-blob preload → segment warm → bootproc → desktop-shaped frame
  loop (vi_tick / StartFrame / dispatch / present-fallback / EndFrame / pacer).

## decomp-ilp32.patch highlights (see port/3ds/patches/README.md)

- stdint/stddef/stdlib ILP32 typedefs matched to newlib exactly.
- `_GFXW1_PTR` high32 host-pointer tag (`GDX_GFXW1_HOST_TAG` = 0x47445831 "GDX1");
  bridge counterpart `kGfxW1HostTag32` in ProcessList (32-bit builds only; tagged
  low32==0 stays on the value path).
- **New finding vs stream E's sweep**: a 32-bit toolchain cannot relocate a symbol
  address into a u64 field at all — every static `gs*` DL initializer was an
  "initializer element is not constant" error, not just a warning. Fixed with the
  `GwordsStatic32` word-split union arm + `_GFX_STATIC_PTR_INIT` (9 static macros
  rewritten; 64-bit/N64 expansion semantically unchanged, host tests pass).
- `osGetTime`/`osSetTime` aliased on 3DS (libctru exports a colliding `osGetTime`).

## Boot-blocking TODOs / risks for M1 (emulator boot)

1. **Untested at runtime** — everything above is compile/link-level. First Citra run
   pending; needs a prebaked `fzerox.o2r` + `gdiffuser.o2r` (tools/prebake) staged in
   the emulated SD `3ds/gdiffuser/`.
2. **Memory budget**: ROM buffer (16 MB) + RDRAM (16 MB) + decoded segment warm cache +
   LUS resource cache must fit the app region. `gdx_boot_warm_asset_segments()` and the
   audio-blob preloads are kept for parity but are the first candidates to trim.
3. **BS::thread_pool in ResourceManager**: compiles (gthreads over libctru), but
   runtime behavior of its worker threads on 3DS cores is unproven; loads may need
   `reservedThreadCount` tuning or synchronous loading.
4. **svcQueryMemory probe volume**: the bridge probes readability thousands of times
   per frame; the 3DS backend does one syscall per probe (no cache yet). Add the
   Windows-style sorted region cache if profiles show it.
5. **Static-DL tag coverage**: any decomp site passing a NUMERIC segment token through
   a pointer-carrying gs*/g* macro would be mis-tagged as a host pointer on 32-bit
   (none known; the bridge logs will show it as a bad host deref near 0x0?xxxxxx).
6. **GetPixelDepth / ReadFramebufferToCPU / CopyFramebuffer** are logged TODOs in the
   citro3d backend (stream A STATUS); lens-flare-class effects will misrender.
7. **Audio R1–R5 hooks are in**, but ndsp output needs `sdmc:/3ds/dspfirm.cdc` on real
   hardware (backend degrades to silent pacing without it).
8. `osContGetReadData` writes the DECOMP 6-byte pad layout (deliberate divergence from
   LUS's 0x24-byte shape — the decomp is the only caller on 3DS; note that desktop
   LUS writes its own larger shape through the same call, flagged for stream E).

## Follow-ups (not this shift)

- Wire `debug.console` (stream B INI) to a bottom-screen consoleInit for boot logs.
- Desktop CRC archive gate port into main_3ds once boot is proven.
- CI: add the 3DS cross build as a job mirroring the Windows release gate.
