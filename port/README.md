# port/ — the host layer

Everything that makes F-Zero X run on a PC lives here: the graphics bridge, the thread scheduler,
audio, the 64DD disk emulation, first-boot setup, and the in-game menu. `decomp/` is the game;
`libultraship/` is the engine; `port/` is the layer that makes those two into one program.

It is kept out of `libultraship/` so the engine submodule stays clean and upstream-mergeable, and
out of `decomp/` so the matching decompilation stays matching.

Build instructions are in the [root README](../README.md). This file is a map: it tells you which
file to open.

## Start here

| If you are | Open |
| --- | --- |
| New to the tree | `CMakeLists.txt` — heavily commented, and the authority for what compiles and why |
| Following a frame | `main.cpp` (host loop) → `n64_sched.c` (tick) → `n64_gfx_bridge.cpp` (draw) |
| Chasing a bug | The subsystem table below, then turn on the matching gate in `gdx_dev_gates.c` |

`CMakeLists.txt` carries the reasoning that is not visible in the source — which decomp files are
excluded and what replaced them, why the RSP interpreter is a separate static library, why
`gdx-extract` is an out-of-tree `ExternalProject`. When a comment there disagrees with this file,
believe the comment.

## Two build targets

| Target | What it is |
| --- | --- |
| `gdiffuser_game` | Object library: the decomp's C compiled for the host, plus the OS-level replacements it cannot link without (`n64_sched.c`, `n64_vi.c`, `decomp_port.c`, the generated binding tables). |
| `G-Diffuser` | The executable: `gdiffuser_game` plus every host subsystem below, linked against libultraship. |

The split matters because `gdiffuser_game` compiles with the decomp's include paths and quiet
warning flags, while port-authored files get `-Wall`. A file's target tells you which world it
lives in.

`G-Diffuser-JP` is a second, experimental executable built from the same tree, guarded behind
`-DGDX_BUILD_JP=ON` (default OFF).

## Subsystems

### Graphics

| File | Open it when |
| --- | --- |
| `n64_gfx_bridge.cpp` | Anything renders wrong. This is the display-list interpreter — the largest file in the port, and where most rendering bugs live. |
| `n64_gfx_convert.cpp` | A binary (8-byte) N64 display list needs converting to the wide 16-byte format. Standalone; unit-tested. |
| `gdx_interp.cpp` | Matrix frame interpolation (default OFF, `GDX_INTERP_P1`). |
| `gdx_vi_convert.c` | RGBA5551 → RGBA8888 for the VI-scanout fallback. Standalone; unit-tested. |
| `n64_vi.c` | VI bridge: framebuffer and retrace. |

The port packs display-list commands at a 16-byte stride so a 64-bit host pointer survives in
`Gfx.w1` without truncation. That ABI is the root of a whole class of bugs; `gdx_gfx_pack_tests`
exists to prove it holds.

### Scheduler and OS

| File | Open it when |
| --- | --- |
| `n64_sched.c` | Threading, messaging, task dispatch, or the crash handler. This is the decomp's own cooperative scheduler running on host fibers. |
| `gdx_fiber.h` + `gdx_fiber_win32.c` / `gdx_fiber_ucontext.c` | Context switching. Win32 fibers on Windows, POSIX `ucontext` elsewhere; exactly one is compiled. |
| `decomp_port.c` | Segment, save, and pool subsystems the decomp expects from the OS. |
| `shims.c` | A libultra symbol is missing at link time. |

The port supplies `osCreateThread`, `osInitialize` and `osGetMemSize` itself because the decomp's
versions truncate 64-bit pointers or touch N64 hardware registers. `CMakeLists.txt` lists each
exclusion with its reason.

### Audio

| File | Open it when |
| --- | --- |
| `n64_audio_hle.c` | Audio is wrong in the default configuration. Software interpreter for the Acmd lists the game builds. |
| `gdx_audio_lle.c` | Routing audio to the real microcode instead, on the vendored cxd4 RSP interpreter (`rsp/cxd4/`). |
| `gdx_audio_thread.cpp` | Dropouts, silence holes, or cross-thread questions. Owns per-tick audio production on its own thread. |
| `gdx_audio_capture.c` | Capturing bit-exact PCM for comparison (`GDX_PCM_CAPTURE`). |

### 64DD / Expansion Kit

Compiled only when `GDX_EXPANSION_KIT=ON`, which is the default and the supported configuration.

| File | Open it when |
| --- | --- |
| `n64_leo.c` | Drive-level behaviour. Replaces the decomp's hardware command and interrupt layer against a disk image. |
| `disk_buffer.cpp` | Loading or laying out the disk image. |
| `disk_savefile.cpp` | Saves. Copy-on-write dirty-LBA journal in `saves/<disk>.gdd` — the user's `.ndd` is never written to. |
| `gdx_ek_disk_overrides.c` | A translated-disk asset resolves to the wrong thing. |
| `gdx_ek_strings.c` | Translated-disk *text* is wrong (the same idea as above, applied to strings). |

### Data sources

| File | Open it when |
| --- | --- |
| `gdx_segment_source.c` | Tracing where a ROM read actually came from. Single archive-first shim behind every ROM read, with per-family fallback telemetry. |
| `rom_buffer.cpp` / `sram_buffer.cpp` | Host-backed ROM and SRAM images. |
| `AssetLoader.cpp`, `resource/ResourceFactories.cpp` | The `.o2r` resource pipeline. |
| `mio0_wrap.c` | MIO0 decompression. |

### Generated tables — `gen/`

**Do not blind-regenerate these.** They are generator output *plus* hand-maintained corrections, and
a regenerate silently drops the hand edits. See [CONTRIBUTING.md](../CONTRIBUTING.md) for the safe
generate-to-scratch-and-diff workflow.

| File | Contents |
| --- | --- |
| `AssetBindings.c` | Asset symbol definitions, segment-blob lookup, and the endian-fixup table. |
| `LinkStubs.c` | Placeholder definitions for not-yet-ported symbols. |
| `EkAssetBindings.c`, `EkLinkStubs.c`, `EkTranslatedStrings.c`, `EkTranslatedOverrides.c` | The Expansion Kit equivalents. |

### First boot, extraction, and tooling launchers

| File | Open it when |
| --- | --- |
| `gdx_firstboot.cpp` | Data-directory resolution or setup routing. Runs before libultraship init. |
| `gdx_firstboot_gui.cpp` | The in-window setup flow the user actually sees. |
| `gdx_extract_launch.cpp` | O2R extraction: validates the ROM, runs the `gdx-extract` child process, installs `fzerox.o2r`. |
| `gdx_dump_launch.cpp` | The Workshop menu's offline "Dump All". |

### Menu and overlays

| File | Open it when |
| --- | --- |
| `gdx_menu.cpp` | Adding or changing anything in the F1 enhancement menu. |
| `gdx_gui.cpp` | Font setup for that menu. |
| `gdx_imgui_nav.cpp` | Controller navigation of the menu. |
| `gdx_workshop.cpp` | Texture packs, texture dumping, hot reload. |
| `gdx_ghost_window.cpp` | The Ghost Browser window. |
| `gdx_input_viewer.cpp`, `gdx_fps_overlay.cpp` | The on-screen input and FPS overlays. |

### Input

| File | Open it when |
| --- | --- |
| `input_bridge.c` | Controller or keyboard behaviour. The port feeds input through here rather than the decomp's `Controller_UpdateInputs`. |
| `gdx_input_script.c` | Replaying deterministic tick-level input for unattended testing (`GDX_INPUT_SCRIPT`). No-op unless set. |

### Ghosts

`gdx_ghost_io.c` — `.gdg` import/export and the per-course host ghost library, kept alongside the
game's own SRAM slot.

### Diagnostics

| File | Open it when |
| --- | --- |
| `gdx_dev_gates.c` / `.h` | Adding a diagnostic switch, or finding which one reveals your bug. Every `GDX_*` gate is a row in one table, with a description. Dependency-free C, which is why the test targets can link it unchanged. |
| `port_log.h` | Logging itself, including the always-on crash sink that writes `gdiffuser-crash.txt`. |
| `gdx_perf.cpp` | Frame-time telemetry: spike attribution and p50/p95/p99 summaries (`GDX_PERF=1`). |
| `gdx_frame_pacer.c` | The game runs faster than 60 FPS. Opt-in wall-clock pacer. |

Diagnostics are off by default, including in Release. Start from the diagnostics document under
`docs/` rather than reading the bridge top to bottom.

## Tests

Seven console test executables build from `tests/`, `rsp/tests/`, and `resource_smoketest.cpp`. They
need no ROM, no window and no assets, and each exits 0 on pass.
[CONTRIBUTING.md](../CONTRIBUTING.md) names them and says which to run for which kind of change.
