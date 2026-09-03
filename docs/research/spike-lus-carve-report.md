# LUS Carve Spike — Verdict: PASS

Phase 0 decision gate (plan section 2, step 3). Question: can libultraship's core —
the Fast3D interpreter plus the resource/archive layer — compile for 3DS with
devkitARM, with desktop layers (SDL, GL/D3D/Metal, ImGui, spdlog) stubbed out?

**Answer: yes.** All 36 carved sources compile to `.o` with devkitARM GCC 16.1.0
(`-march=armv6k -mfloat-abi=hard -D__3DS__ -DF3DEX_GBI_2 -std=gnu++20 -fexceptions
-frtti`). Consequence: **stream A subclasses `Fast::GfxRenderingAPI` with a citro3d
backend; no hand DL interpreter fallback needed.** `gdx3ds_gfx.h` is frozen
accordingly.

## What compiled

- `src/fast/interpreter.cpp` (7,643 lines — the core gate) → 1.7 MB object, zero errors
  after patches below
- Fast3D resource factories/types (DisplayList, Light, Matrix, Texture, Vertex)
- `src/ship/resource/` manager, loader, binary/XML factories
- `src/ship/resource/archive/` Archive, ArchiveManager, **O2rArchive**, FolderArchive
  (OtrArchive/StormLib excluded from the carve)
- binarytools, utils, `glob.c`

## Key findings

1. **devkitARM libstdc++ HAS gthreads**: `<thread>`, `<mutex>`, `<shared_mutex>`,
   `<future>`, `condition_variable` all compile — the resource manager's BS::thread_pool
   model survives as-is at the compile level. (Runtime behavior on 3DS cooperative
   threads is stream B/E territory.)
2. **C++20 fine**: `<compare>`, `<any>`, `<variant>`, `<filesystem>`, `<span>`,
   designated initializers all accepted.
3. **Carve set has zero desktop-API calls** — no SDL/GL/D3D/ImGui usage in interpreter
   or resource code. Only header-level leaks, all shadow-stubbed (see
   `spike-lus-carve/stubs/`): `imconfig.h` (ImTextureID), `ship/window/gui/Gui.h`,
   `spdlog/*`, `SDL2/SDL.h` (types only, leaks via `AudioPlayer.h:178`), decl-only
   `zip.h` / `monocypher.h` / `StormLib.h`. Real headers used for `nlohmann/json.hpp`,
   `BS_thread_pool.hpp`, `tinyxml2.h` (all header-only or portable).
4. **Only real source fixes needed: newlib type quirks.** On arm-none-eabi,
   `int32_t`/`uint32_t` are `long`/`unsigned long`, not `int`/`unsigned int`, so exact
   template matches and virtual-override signatures that pass on desktop fail. Total: 8
   files, 13 insertions, 9 deletions (`spike-lus-carve/lus-newlib-portability.patch`,
   to be committed to the Zorkats LUS fork in Phase 1):
   - `std::min`/`std::max` → explicit `<uint32_t>`/`<int32_t>` (interpreter.cpp ×5,
     ResourceManager.cpp ×1)
   - `int Buffered()` → `int32_t Buffered()` (SDLAudioPlayer.h, NullAudioPlayer.h —
     base declares `int32_t`)
   - missing `<unordered_map>` (DisplayListFactory.cpp), missing ResourceManager.h /
     `<cstring>` includes (Archive.cpp, O2rArchive.cpp) — desktop stdlibs include these
     transitively, newlib's doesn't
   - `int strLen` → `int32_t strLen` (BinaryWriter.cpp — overload ambiguity)
5. **CVar macros** come from `libultraship/cmake/cvars.cmake` as compile definitions —
   the 3DS build must replicate them (or include that cmake file).
6. **Portlibs gap**: `/opt/devkitpro/portlibs/3ds/` is empty. For LINK (not this
   compile gate) stream D needs: `dkp-pacman -S 3ds-zlib 3ds-bzip2 3ds-libpng`; libzip
   has no 3DS portlib and must be cross-compiled (portable C over zlib, low risk);
   monocypher is vendorable portable C.

## Evidence

`spike-lus-carve/` (uncommitted scratch, foundation worktree): `stubs/` tree,
`obj/*.o` (38 objects incl. probes), `err_interp.txt` / `err_carve.txt` /
`err_round2.txt` error logs, `lus-newlib-portability.patch`.

Exceptions/RTTI stay ON (Archive.cpp try/catch, ArchiveManager dynamic_pointer_cast).
Do not define `ENABLE_OPENGL`.

## Follow-ups pushed to Phase 1

- Commit the newlib patch to the LUS fork (orchestrator, before stream fan-out).
- Link-level validation happens naturally at M1 integration; unresolved-symbol audit of
  the carve (Context/Window/ConsoleVariable object files not yet in the carve list) is
  stream A's first task with the DL replay harness.
- `ArchiveManager.cpp:270` constructs `OtrArchive` — 3DS build needs that path compiled
  out or StormLib stubbed at link time (stream A/E decision at integration).
