# 3DS Port Research — G-Diffuser (F-Zero X)

Research dossier for planning a Nintendo 3DS port. Combines a local codebase audit with
web research (21 sources fetched, 105 claims extracted, 25 adversarially verified — 22
confirmed, 3 refuted). Intended as input for a follow-up planning agent.

Date: 2026-08-10.

---

## 1. Bottom line

A native 3DS port is feasible in principle but **cannot reuse the existing rendering and
platform stack**. libultraship officially supports only Windows / Linux / macOS / iOS /
Android; the 3DS PICA200 GPU has no desktop OpenGL, no GLSL, and no programmable fragment
shaders; and official SDL2 on 3DS is software-rendering-only with cooperative single-core
threading. The proven path — established by the SM64 3DS port lineage — is a **hand-port**:
implement the fast3d `GfxRenderingAPI` backend slot with a citro3d renderer (one hand-written
PICA vertex shader + N64 color-combiner emulation via fixed-function TexEnv stages), fixed
memory pools, and the N64 audio engine offloaded to a spare ARM11 core.

F-Zero X is a substantially harder workload than SM64 (60 fps target, 30 machines of CPU
physics, LLE RSP audio), so **New3DS (804 MHz) should be treated as the primary target** and
a 30 fps cap or reduced machine count may be necessary. No F-Zero X-on-3DS performance data
exists anywhere; that part is inference.

---

## 2. What this codebase actually is (local audit)

- **Game core**: inspectredc/fzerox matching decomp, compiled as an object library with
  `PORT` gating (`port/CMakeLists.txt:149-183`). C++20 / C11 required
  (root `CMakeLists.txt:10-12`).
- **Graphics**: libultraship (Zorkats fork, submodule) Fast3D interpreter driven directly by
  a custom ~10k-line bridge, `port/n64_gfx_bridge.cpp` — uses `GetInterpreterWeak()` +
  `Interpreter::Run()`, not the stock `DrawAndRunGraphicsCommands` path. Microcodes: stock
  **F3DEX2** plus F3DLX2_REJ / F3DFLX2_REJ reject variants (`port/n64_gfx_bridge.h:10-14`).
- **Renderers present**: D3D11 (Windows) and OpenGL (Linux) only. No console backend
  anywhere in the tree; no `__SWITCH__` / `__WIIU__` / `__3DS__` ifdefs.
- **Windowing/input**: SDL2 + ImGui (`port/main.cpp:41`, gamecontroller init at 687-695,
  multi-pad routing at 319-380).
- **Memory model**: hardcoded 16 MB RDRAM arena (`port/n64_rdram.h:11`,
  `GDX_RDRAM_SIZE = 0x1000000`) — Expansion Pak is mandatory, no 8 MB fallback; overlay
  textures reach ~12-13 MB physical. 1 MB staging carve for decompression. Mode-owned
  segment carves (4/5/7/8/9) swap content at mode transitions
  (`docs/ARCHITECTURE.md:282-288`); segment 8 (`course_track_gfx`) is the largest, with a
  measured 133.95 ms single-hit decode, prewarmed at boot.
- **64-bit assumptions**: display-list packets are widened to 16-byte stride on 64-bit hosts
  (`port/n64_gfx_bridge.cpp:248`, `kHostBuiltGfxStride`); segment address resolution and
  audio command words carry full host pointers (`port/gdx_audio_lle.c:14`). Little-endian
  assumed throughout; DL byte-order auto-detection at `port/n64_gfx_bridge.cpp:281-299`.
- **Threading**: game scheduler runs on fibers — Win32 fibers on Windows, POSIX `ucontext`
  elsewhere (`port/CMakeLists.txt:155-161`). Audio runs on a dedicated `std::thread` with
  `std::mutex` / `condition_variable` and a lock-free ring buffer for Acmd queues
  (`port/gdx_audio_thread.cpp`).
- **Audio**: **LLE** RSP audio via the vendored cxd4 RSP interpreter, with microcode blobs
  extracted from the user's ROM (`port/gdx_audio_lle.c:77-78`); HLE fallback exists
  (`gdx_audio_hle_run`). 32 kHz stereo 16-bit output through libultraship's SDL audio layer;
  buffer CVar 1024–8192 frames, default 4096 (`port/main.cpp:871`).
- **Assets**: O2R (zip-based) archives, 3,610 records. `gdiffuser.o2r` ships MIT Fast3D
  shaders; `fzerox.o2r` is generated at first boot from the user's ROM via deterministic
  Torch extraction (`gdx-extract`, `tools/o2r_harness/README.md`). Runtime is
  read-on-demand through `GDiffuser_LoadArchiveFileBytes()` (`port/AssetLoader.cpp`),
  staged through the RDRAM arena. Binding/fixup tables in `port/gen/AssetBindings.c`
  (u32/u16 byteswap and vertex-layout fixups).
- **64DD Expansion Kit**: disk I/O shim `port/n64_leo.c` + `port/disk_buffer.cpp`; separate
  `.ndd` image, ~64.45 MB capacity.
- **VI/framebuffer**: VI scanout emulation (`port/n64_vi.c`) with an RGBA5551→RGBA8888
  textured-quad fallback path (`port/gdx_vi_convert.c`) preserving "VI scans out whatever is
  in RDRAM" semantics.
- **Dependencies**: SDL2, GLEW, zlib, bzip2, libzip, nlohmann-json, tinyxml2, spdlog,
  discord-rpc, cxd4 (vendored). Torch built via isolated ExternalProject to avoid ODR
  clashes.

---

## 3. 3DS platform facts (verified)

### Toolchain (mature, all pieces exist)
- **devkitPro `3ds-dev` metapackage**: devkitARM (GCC for ARM11), libctru (OS/syscall
  library), citro3d/citro2d (GPU), picasso (PICA shader assembler), 3dstools. Installed via
  `dkp-pacman -Syu 3ds-dev`. [3-0 verified]
- **CMake cross-compile is official**: `cmake -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/3DS.cmake`
  — aligns with G-Diffuser's existing CMake build. [3-0]
- **Packaging**: `.3dsx` for the Homebrew Launcher; `makerom` v0.17 (Project_CTR) for
  installable `.cia`. Docker/Linux/WSL/MSYS2 builds all proven by sm64_3ds. [3-0]
- **Caveat**: devkitPro pacman ships only SDL **1.2** as a 3DS portlib (SDL2 for 3DS exists
  upstream in SDL ≥ 2.24 but is software-rendering-only — see below). Do not plan around
  prebuilt SDL2 portlibs the way Switch homebrew does. [3-0]

### Hardware constraints
- CPU: ARM11, 268 MHz (old3DS, 2 cores, core 1 = syscore shared with OS) / 804 MHz
  (New3DS, 4 cores). 32-bit, little-endian.
- RAM: 128 MB (old3DS) / 256 MB (New3DS); GPU-visible memory must come from `linearAlloc`.
- GPU: **PICA200** — programmable vertex shaders (PICA assembly via picasso), **no
  programmable fragment shaders**, no runtime shader compilation, ~6 fixed-function TexEnv
  combiner stages. citro3d "intentionally deviates from OpenGL" to match hardware
  semantics; a GL wrapper (picaGL, GL 1.1 fixed-function) cannot run libultraship's
  runtime-generated GLSL fragment shaders. [3-0]
- SDL2 on 3DS: software rendering only, no GL context, cooperative threading on a single
  core (threads yield only via `SDL_Delay`/blocking waits). G-Diffuser's GL-over-SDL path
  is impossible; desktop preemptive-threading assumptions break. [3-0]

### libultraship platform support
- Current libultraship main documents Windows / Linux / macOS / iOS / Android (plus OpenBSD
  in PORTING.md). **No 3DS, Switch, or Wii U backend exists in current main.** Switch/Wii U
  (`gfx_gx2`) backends existed circa 2022-2023 and were **removed** — usable only as
  historical prior-art references for constrained-platform backends. [3-0 / 2-1]
- No libultraship-based 3DS port exists anywhere (verifiers searched; Ship of Harkinian 3DS
  threads on GBAtemp are requests, not ports). [3-0]

---

## 4. Prior art: the SM64 3DS lineage (the template)

Lineage: **Gericom `sm64_3ds` → mkst/sm64-port `3ds-port` branch (based on sm64-port
"Refresh 11") → Wyatt-James `sm64-3ds-port` optimization fork.** All are hand-ports of the
sm64-port PC codebase — none use libultraship. [3-0]

Key techniques (all code-verified in `mkst/sm64-port/blob/3ds-port/src/pc/gfx/gfx_citro3d.c`):

- **Renderer**: `gfx_citro3d.c` (~1002 lines) fills the same fast3d `GfxRenderingAPI`
  function-pointer contract that gfx_opengl/gfx_direct3d11 fill (`lookup_shader`,
  `create_and_load_new_shader`, `shader_get_info`, `draw_triangles`), drawing via
  `C3D_DrawArrays(GPU_TRIANGLES, ...)`.
- **Shaders**: exactly one hand-written PICA vertex shader (`shader.v.pica`, assembled with
  picasso). N64 color combiner emulated with fixed-function TexEnv stages
  (`GPU_REPLACE` / `GPU_MODULATE` / `GPU_INTERPOLATE`), cached in a 32-variant
  `sShaderProgramPool`.
- **Memory**: fixed pools — 1 MB `linearAlloc` VBO, `TEXTURE_POOL_SIZE 4096`,
  `FOG_LUT_SIZE 32`, with explicit exhaustion messages ("Out of textures!",
  "Fog exhausted!"). No desktop-style dynamic allocation.
- **Audio**: N64 RSP audio engine on a separate ARM11 core — Core 1 (syscore) on old3DS,
  Core 2 on New3DS — **requires Luma3DS ≥ 10.1.1** to unlock syscore CPU time. Ships an
  "Enhanced RSPA" fast-math path, a reference-accuracy fallback (`FORCE_REFERENCE_RSPA=1`),
  a 32-bit SIMD mixer (`mixer_3ds_simd32.c`), and `DISABLE_AUDIO=1`.
- **Performance**: even SM64 (30 fps, one Mario) shipped optional naive frame-skip
  (skip when frame > 33.3 ms) and needed an upstream `GFX_POOL_SIZE` fix for 60 fps on
  32-bit platforms. Wyatt-James notes frame-skip is now legacy — SM64 largely holds rate.
- **Stereoscopic 3D**: supported in the backend (`gTargetRight`, `set_iod`, `stereoTilt`).
- **Assets**: pre-extracted from ROM **at build time** (binutils-mips for extraction) — not
  a runtime archive/resource-manager pipeline.

Note on API drift: the "GfxRenderingAPI is ~20 C function pointers" characterization was
**refuted** for this codebase — the pinned Zorkats libultraship uses a C++ virtual-class
renderer API with a two-part 64-bit shader ID (`CreateAndLoadNewShader(uint64_t, uint64_t)`).
The TexEnv mapping of that richer combiner keyspace onto ≤6 PICA stages is the central
rendering difficulty.

---

## 5. Gap analysis: G-Diffuser vs 3DS

| Subsystem | Today | 3DS reality | Work required |
|---|---|---|---|
| Renderer | LUS Fast3D → D3D11/OpenGL, runtime GLSL | PICA200: no GLSL, no frag shaders, citro3d | New citro3d backend; TexEnv combiner mapping; PICA vertex shader |
| Windowing/UI | SDL2 + ImGui | SDL2 = software-only; no GL | Native libctru `gfxInit`; drop or bottom-screen-ify ImGui menu |
| Input | SDL gamecontroller | libctru HID | Straight remap; simplest subsystem |
| Threading | Win32 fibers / ucontext + std::thread audio | libctru threads, cooperative, appcore+syscore | New fiber backend on libctru threads or restructure scheduler; audio → spare core (Luma ≥ 10.1.1) |
| Audio | LLE cxd4 RSP interpreter, 32 kHz | 268/804 MHz ARM11 | LLE likely too slow on old3DS; HLE fallback (`gdx_audio_hle_run`) becomes primary; SM64-style core offload |
| Memory | 16 MB RDRAM arena + host heap, 64-bit ptrs | 128/256 MB, 32-bit | 16 MB arena fits; 64-bit stride/pointer code must gain a real 32-bit path (`kHostBuiltGfxStride` already branches — verify end-to-end); fixed pools everywhere |
| Assets | O2R zip, Torch first-boot extraction, read-on-demand | SD I/O, no Python, 128 MB | Either port libzip read path (feasible: zlib/libzip build on 3DS) or pre-bake assets PC-side into flat files, SM64-style; first-boot Torch extraction must move to PC |
| 64DD | ~64.45 MB `.ndd` buffered | RAM budget | Stream from SD instead of full-buffer on old3DS |
| Build | CMake, C++20 | devkitARM GCC, official 3DS.cmake | Toolchain file integration; check C++20 support in current devkitARM (recent GCC — likely fine); strip GLEW/discord-rpc |

---

## 6. Strategy options

1. **sm64-3ds-style hand-port (proven path).** Drop libultraship; keep the decomp core +
   the existing gfx bridge's display-list interpretation; write `gfx_citro3d` against the
   backend contract; pre-bake assets PC-side. Highest confidence, largest divergence from
   upstream G-Diffuser.
2. **Minimal LUS port.** Keep libultraship but implement a citro3d renderer + libctru
   window/audio/input backends inside it, strip ImGui/O2R-manager down. No prior art —
   nobody has put LUS on 3DS; removed 2022-2023 Switch/Wii U backends are the only
   reference. Higher risk, better upstream alignment.
3. **Emulation (non-goal).** Citra-core on 3DS doesn't exist; N64 emulators on 3DS can't
   run F-Zero X well. Not viable.

Prior-art strength strongly favors option 1, with option 2's feasibility worth a
time-boxed spike only if upstream alignment matters.

---

## 7. Open questions for the planning agent

1. **Performance ceiling**: can F-Zero X's 60 fps / 30-machine physics run on ARM11 at all?
   No data exists. Plan for New3DS-primary, 30 fps cap contingency, possibly reduced
   machine count on old3DS. Early profiling spike required (compile physics core for ARM11,
   benchmark headless).
2. **Combiner coverage**: does F-Zero X use combiner modes / framebuffer effects exceeding
   PICA200's ~6 TexEnv stages? Audit the shader-ID population the gfx bridge actually
   generates (instrument `CreateAndLoadNewShader` calls on PC, dump unique IDs per track).
3. **Asset pipeline**: O2R read-on-demand over SD vs build-time pre-bake — measure segment-8
   (133.95 ms PC decode) on 3DS-class I/O/CPU before choosing.
4. **LLE audio cost**: cxd4 interpreter per-frame cost on 804 MHz ARM11 unknown; HLE path
   correctness for this title needs validation.
5. **32-bit correctness**: the codebase has an 8-byte-stride path for 32-bit hosts but it is
   untested; endianness is fine (3DS is LE) but pointer-width assumptions need a full sweep.

---

## 8. Source list

Primary (code-level verified):
- https://github.com/sm64-port/sm64_3ds — original 3DS hand-port
- https://github.com/mkst/sm64-port/tree/3ds-port — 3ds-port branch; `src/pc/gfx/gfx_citro3d.c`, `shader.v.pica`
- https://github.com/Wyatt-James/sm64-3ds-port — maintained optimization fork (audio cores, frame-skip, RSPA docs)
- https://github.com/Emill/n64-fast3d-engine — original GfxRenderingAPI contract
- https://github.com/Kenix3/libultraship — platform support docs, PORTING.md
- https://github.com/devkitPro/citro3d, https://github.com/devkitPro/libctru, https://github.com/devkitPro/pacman-packages
- https://wiki.libsdl.org/SDL2/README-n3ds — SDL2 3DS limitations
- https://github.com/inspectredc/fzerox, https://decomp.dev/inspectredc/fzerox, https://github.com/inspectredc/fzerox-expansion-kit

Secondary/forum:
- http://3dbrew.org/wiki/Nintendo_OpenGL — PICA200 vs GL
- https://en.wikipedia.org/wiki/PICA200
- https://gbatemp.net/threads/ship-of-harkinian-ocarina-of-time-3ds-port.643815/ — confirms no SoH 3DS port exists
- https://devkitpro.org/viewtopic.php?t=9578 — toolchain discussion

Refuted claims (do not rely on):
- "libultraship main uses SDL3/GLEW/Metal" — 0-3; verify the pinned Zorkats fork instead.
- "GfxRenderingAPI is ~20 C function pointers" — 0-3 for this tree; pinned LUS uses C++
  virtual classes with 2×64-bit shader IDs.
- "All 3DS deps come prebuilt via devkitPro pacman portlibs" — 1-2; some must be built from
  source (notably SDL2, if used at all).

Local references:
- `docs/ARCHITECTURE.md` — 592-line architecture doc, defect taxonomy, segment carve map
- `port/n64_gfx_bridge.cpp`, `port/n64_rdram.h`, `port/gdx_audio_lle.c`,
  `port/gdx_audio_thread.cpp`, `port/AssetLoader.cpp`, `port/gen/AssetBindings.c`,
  `tools/o2r_harness/README.md`, `port/CMakeLists.txt`
