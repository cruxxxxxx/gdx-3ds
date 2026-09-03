# 3DS Port — Multi-Agent Work Plan

Companion to `3ds-port-research.md` (read that first — all claims there are cited and
verified). This document segments the port into parallel workstreams designed for
multiple agents working in separate git worktrees with minimal file overlap.

Primary target: **New3DS** (804 MHz quad ARM11, 256 MB). Old3DS: stretch goal, not planned
against. Output: `.3dsx` (Homebrew Launcher) first, `.cia` later.

---

## 0. Strategy decision (made, revisit only if Phase 0 spike fails)

**Keep libultraship's Fast3D interpreter; replace everything around it.**

Rationale: this repo's `port/n64_gfx_bridge.cpp` (~10k lines) drives the LUS interpreter
directly (`GetInterpreterWeak()` + `Interpreter::Run()`). Rewriting a DL interpreter
sm64-style throws away the most battle-tested part of the port. The interpreter is
platform-neutral C++; only its *renderer backends* (D3D11/GL) and the surrounding
window/audio/ImGui/resource layers are desktop-bound. So:

- **Keep**: fzerox decomp core, gfx bridge, LUS Fast3D interpreter, HLE audio path,
  O2R archive *reading* (libzip/zlib compile fine on 3DS).
- **Replace**: renderer backend (→ citro3d), window/input (→ libctru), fibers
  (→ libctru threads), audio output (→ ndsp), first-boot Torch extraction (→ PC-side
  pre-bake).
- **Drop on 3DS**: ImGui enhancement menu (config file instead; bottom-screen menu is a
  post-MVP nicety), GLEW, discord-rpc, Crash Handler, spdlog (or stub to stderr).

**Escape hatch**: if the Phase 0 spike shows the pinned LUS fork cannot be carved down to
compile under devkitARM in reasonable time, fall back to sm64-3ds-style: hand DL
interpreter, no LUS. That decision gate is explicit in Phase 0.

---

## 1. Phase structure

```
Phase 0 (serial, ~1 agent)      Phase 1 (parallel, 5-6 agents)        Phase 2 (serial-ish)
┌─────────────────────┐   ┌──────────────────────────────────┐   ┌──────────────────┐
│ F0 Foundation:      │   │ A Renderer (citro3d)             │   │ Integration:     │
│  - toolchain CMake  │──▶│ B Platform/OS (libctru)          │──▶│  merge, boot,    │
│  - contract headers │   │ C Audio                          │   │  in-race, perf   │
│  - LUS carve spike  │   │ D Assets (PC pre-bake)           │   │  tuning          │
│  - stub .3dsx boots │   │ E 32-bit correctness (PC-side!)  │   │                  │
└─────────────────────┘   │ F Perf profiling (no repo edits) │   └──────────────────┘
                          └──────────────────────────────────┘
```

Phase 1 streams are independent because Phase 0 freezes the **contracts** (headers) each
stream implements. Streams build against stubs; integration swaps stubs for real
implementations.

---

## 2. Phase 0 — Foundation (blocking; one agent, on `feat/3ds-foundation`)

Everything else rebases on this. Ordered deliverables (order matters — see step 3):

1. **Toolchain integration**: `cmake -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/3DS.cmake`
   path through root + `port/CMakeLists.txt`; new `GDX_PLATFORM_3DS` option; verify
   devkitARM GCC accepts the C++20/C11 codebase (fix `-std` / feature fallout only, no
   logic changes).
2. **Directory + ownership skeleton** (this is what makes worktrees mergeable):
   ```
   port/3ds/
     gfx/        ← stream A only   (has own CMakeLists.txt)
     os/         ← stream B only   (has own CMakeLists.txt)
     audio/      ← stream C only   (has own CMakeLists.txt)
     assets/     ← stream D only   (has own CMakeLists.txt)
     include/    ← contract headers (freeze rules in step 3)
     main_3ds.cpp ← 3DS entry point, owned by Phase 0 then stream B
   tools/prebake/ ← stream D only  (has own CMakeLists.txt)
   ```
   **Build wiring rule**: `port/CMakeLists.txt` gains ONE `add_subdirectory(3ds)` block in
   Phase 0 and is not touched again in Phase 1. Each stream registers sources only via the
   `CMakeLists.txt` inside its own directory. This is what makes the "streams only add
   files" guarantee actually true.
3. **Submodule init + LUS carve spike (decision gate) — BEFORE gfx contract freeze**:
   init all submodules (`libultraship/` is currently uninitialized — the renderer
   interface has not been read in-tree); read the pinned fork's actual renderer
   virtual-class surface; then attempt to compile LUS core (interpreter + resource
   manager, minus SDL/GL/D3D/ImGui/spdlog) with devkitARM. Time-boxed.
   *Pass* → write `gdx3ds_gfx.h` against the verified interface and freeze it.
   *Fail* → stream A's scope becomes a hand DL interpreter (sm64-3ds model) and
   `gdx3ds_gfx.h` is written for that instead. Either way the gfx contract freezes only
   AFTER this verdict; the os/audio/fs contracts (step 4) can be written and frozen
   before it, since they don't depend on the outcome.
4. **Contract headers** in `port/3ds/include/` — small C ABI boundaries:
   - `gdx3ds_gfx.h` — renderer backend entry points matching whatever step 3 verified
     (expected: C++ virtual-class surface with two-part 64-bit shader ID,
     `CreateAndLoadNewShader(uint64_t, uint64_t)` — NOT the old 20-pointer C struct; but
     confirm against the initialized submodule, do not trust this doc).
   - `gdx3ds_os.h` — window init/swap, HID poll → existing controller abstraction, plus
     fibers matching the REAL `port/gdx_fiber.h` surface exactly: the four functions
     `gdx_fiber_convert_thread` / `gdx_fiber_create` / `gdx_fiber_switch` /
     `gdx_fiber_current_thread_id` (there is NO destroy), with that header's invariants
     (entry never returns; all calls on scheduler thread; thread-id feeds the
     audio-affinity guard).
   - `gdx3ds_audio.h` — `osAiSetNextBuffer`-shaped push interface, 32 kHz stereo s16.
   - `gdx3ds_fs.h` — asset read (`GDiffuser_LoadArchiveFileBytes` equivalent), paths on SD
     (`sdmc:/3ds/gdiffuser/`), **and save-data persistence**: SRAM + 64DD save surfaces
     (`port/sram_buffer.cpp`, `port/disk_savefile.cpp`) need an SD-backed path.
5. **3DS entry point**: `port/3ds/main_3ds.cpp` selected by CMake instead of
   `port/main.cpp` (which is SDL/ImGui/Discord-saturated and stays desktop-only —
   nobody ifdefs it). Phase 0 enumerates main.cpp's init responsibilities (window, audio
   device, controller hotplug, CVar/config load, LUS Context, asset archive mount, crash
   handler, Discord) and maps each to a contract call, a 3DS equivalent, or an explicit
   "dropped on 3DS" note in the file header.
6. **DL capture/replay harness**: record the interpreter's command + vertex stream on the
   PC build for a fixed demo segment, serialize to file; a standalone driver feeds it to a
   `gdx3ds_gfx.h` implementation. This is stream A's Phase 1 test rig (golden-image
   compare in Citra WITHOUT needing streams B/D). Style precedent: `port/tests/`.
7. **Stub build**: all contract headers implemented as no-op stubs; produces a `.3dsx`
   that boots to a cleared screen on Citra emulator. CI job (optional): 3DS build on
   every push, mirroring existing Windows release job.

Nothing merges to the integration branch until Phase 0 lands. After freeze, contract
changes require orchestrator sign-off (they're the cross-worktree coupling point).

---

## 3. Phase 1 — Parallel streams

Each stream: own branch (`feat/3ds-<stream>`), own worktree, owns ONLY its directories
listed below. Editing outside ownership = coordination request to orchestrator, not a
direct edit. All streams build with stubs for the other streams' contracts.

### Stream A — Renderer (hardest; strongest agent, largest budget)
- **Owns**: `port/3ds/gfx/`
- **Builds**: citro3d backend implementing `gdx3ds_gfx.h`:
  - one PICA vertex shader (`shader.v.pica`, picasso-assembled; reference:
    mkst/sm64-port `3ds-port` branch `src/pc/gfx/shader.v.pica`)
  - N64 combiner → TexEnv stage mapper with variant cache (reference: `gfx_citro3d.c`
    32-variant `sShaderProgramPool`; ours needs the 2×64-bit shader-ID keyspace)
  - fixed pools: linearAlloc VBO, texture pool, fog LUTs — with exhaustion logging
  - RGBA5551→native VI fallback path (`port/gdx_vi_convert.c` consumer)
  - stereoscopic 3D: plumb but off by default (post-MVP)
- **Prereq input**: combiner audit from stream F (list of unique shader IDs F-Zero X
  actually generates). Until it arrives, implement the SM64-proven subset.
- **Testable without hardware and without other streams**: Phase 0's DL capture/replay
  harness drives the backend standalone; golden-image compare vs PC-port renders of the
  same captured stream under Citra. Full in-game compare waits for M1/M2 integration.
- **Risk**: combiner modes exceeding ~6 TexEnv stages → needs multi-pass or approximation.
  Escalate to orchestrator with the specific modes; do not silently approximate.

### Stream B — Platform/OS (libctru)
- **Owns**: `port/3ds/os/`
- **Builds**: implements `gdx3ds_os.h`:
  - `gfxInitDefault`/framebuffer window glue, gsp vsync
  - HID (buttons + circle pad + optional C-stick) → existing multi-pad abstraction
    (`port/main.cpp:319-380` consumer side stays untouched)
  - fiber backend: `gdx_fiber_3ds.c` on libctru threads (cooperative — matches 3DS
    threading model; reference existing `gdx_fiber_win32.c` / `gdx_fiber_ucontext.c`
    for the required semantics)
  - main-loop shim replacing SDL event pump; config from `sdmc:` INI instead of ImGui
- **Testable**: input/fiber unit harness under Citra.

### Stream C — Audio
- **Owns**: `port/3ds/audio/`
- **Builds**: implements `gdx3ds_audio.h`:
  - ndsp output at 32 kHz stereo s16, ring buffer sized from existing CVar defaults
  - audio thread on spare core (New3DS core 2; `APT_SetAppCpuTimeLimit` / syscore notes —
    requires Luma3DS ≥ 10.1.1, document this for users)
  - **HLE path as primary** (`gdx_audio_hle_run` — exists already); benchmark cxd4 LLE on
    ARM11 once, keep LLE only if it fits budget (unlikely; don't burn time)
  - reference: Wyatt-James sm64-3ds-port audio architecture (Enhanced RSPA, SIMD mixer)
- **Coordination note**: any ifdef hooks needed in `port/gdx_audio_thread.cpp`
  (std::thread/mutex) belong to **stream E's charter**, not C — E is the only stream that
  edits shared files. C files a request to E with the exact hook points; C itself only
  adds files under `port/3ds/audio/`.

### Stream D — Assets
- **Owns**: `tools/prebake/`, `port/3ds/assets/`
- **Builds**:
  - PC-side pre-bake: run existing Torch/`gdx-extract` flow on desktop, emit final
    `fzerox.o2r` (+ `gdiffuser.o2r`) for the user to copy to
    `sdmc:/3ds/gdiffuser/`. No Python, no Torch on 3DS. Document user flow.
  - 3DS-side: implement `gdx3ds_fs.h` — libzip/zlib O2R read-on-demand from SD; measure
    worst case (segment 8 `course_track_gfx`, 133.95 ms decode on PC) and add load-screen
    prewarm if needed
  - `.ndd` (64DD, ~64.45 MB): stream from SD rather than full-buffer; Expansion Kit is
    post-MVP — MVP is cartridge content only
  - **save data**: SD-backed persistence for SRAM + 64DD saves (`port/sram_buffer.cpp`,
    `port/disk_savefile.cpp` are the existing surfaces) via `gdx3ds_fs.h`
- **Decision point** (data-driven, stream owns it): if SD+unzip throughput is bad, switch
  to flat pre-extracted files PC-side (sm64 model) — contract header unchanged.

### Stream E — 32-bit correctness + shared-file edits (runs on PC, no 3DS needed)
- **Owns**: edits to existing core files (`port/n64_gfx_bridge.cpp`,
  `port/gdx_audio_lle.c`, `port/gdx_audio_thread.cpp` ifdef hooks requested by stream C,
  segment addressing, `AssetBindings` fixups) — the ONE stream allowed to touch shared
  code, which is why it merges FIRST in Phase 2.
- **Builds**: make the existing 32-bit code paths real and tested:
  - `kHostBuiltGfxStride` 8-byte path (`port/n64_gfx_bridge.cpp:248`) exercised end-to-end
  - full sweep for `uintptr_t`/pointer-width and alignment assumptions (ARM11 faults on
    some unaligned access patterns x86 tolerates)
  - CI-able proof: `-m32` (or i686 container) PC build that boots and plays — this becomes
    the regression gate for everyone
- **Why parallel-safe**: other streams only ADD files under `port/3ds/`; E only EDITS
  existing files. Conflicts ≈ 0.

### Stream F — Performance, memory & combiner audit (report output)
- **Instrumentation lands via orchestrator on day 1 of Phase 1** (not "locally,
  uncommitted" — that's unreproducible): a dev CVar for shader-ID dumping plus a
  `tools/` benchmark harness, both merged to `feat/3ds` as F's only repo footprint.
- **Builds three reports**:
  1. **Combiner census**: dump unique shader IDs + combiner modes across all
     tracks/menus/machine counts → feeds stream A.
  2. **CPU budget**: compile physics/game-loop core headless with devkitARM, run on Citra
     + real New3DS if available; measure ms/frame at 30 machines. Output: 60 fps verdict,
     30 fps contingency recommendation, old3DS verdict.
  3. **Memory budget**: table of 16 MB RDRAM arena + LUS resource-manager cache + libzip
     inflate buffers + linearAlloc pools + code/stack vs the 3DS application-region
     allotment (well below the headline 256 MB). Tripwire: heap high-water mark measured
     under Citra at M2.
- Smallest stream; finishes early; agent then reassigned (likely to A, the long pole).

---

## 4. Phase 2 — Integration (orchestrator + 1-2 agents)

Merge order onto `feat/3ds` integration branch (from Phase 0 base):

1. **E** (edits shared files — everyone rebases after)
2. **B** (boot + input + fibers → can reach menus with stub gfx)
3. **A** (renderer → visible menus/race)
4. **D** (real assets instead of dev fixtures)
5. **C** (audio last; game is playable silent)

Milestones / acceptance gates:
- **M1**: boots on Citra, title screen renders, input navigates menus
- **M2**: in-race, one track, 1 machine, any fps — correctness over speed
- **M3**: 30 machines; measure; apply stream F contingency (30 fps cap etc.)
- **M4**: audio on; no underruns at target fps
- **M5**: real-hardware pass (New3DS + Luma), SD asset flow docs, `.cia` packaging

Reviewer agent runs per-merge: build all targets (Win/Linux/3DS + `-m32` gate), diff review
focused on ifdef leakage into shared code.

---

## 5. Worktree / orchestration mechanics

```bash
git worktree add ../gdx-3ds-foundation feat/3ds-foundation   # Phase 0
# after Phase 0 merges to feat/3ds:
git worktree add ../gdx-3ds-gfx      feat/3ds-gfx
git worktree add ../gdx-3ds-os       feat/3ds-os
git worktree add ../gdx-3ds-audio    feat/3ds-audio
git worktree add ../gdx-3ds-assets   feat/3ds-assets
git worktree add ../gdx-3ds-32bit    feat/3ds-32bit
```

Rules that keep this conflict-free:
1. **Ownership is directory-based** (section 3). Outside your directories: read
   everything, edit nothing — shared-file needs are filed as requests to stream E
   (code) or the orchestrator (contracts/build).
2. **Ownership is checked, not just trusted**: reviewer agent (or a trivial CI step)
   diffs each stream branch against its path allowlist before merge; out-of-bounds
   files block the merge.
3. **Contracts frozen per Phase 0 step 3/4 rules.** Header change = orchestrator
   broadcasts to all affected streams before merge.
4. **Streams rebase on `feat/3ds` after every integration merge** (esp. after E lands).
5. **Stubs stay until integration** — a stream never depends on a sibling's WIP branch.
6. **Each stream leaves a `STATUS.md` in its owned dir**: done / blocked-on / next.
   Worktrees share one repo, so the orchestrator reads
   `../gdx-3ds-*/port/3ds/<dir>/STATUS.md` directly from disk — no fetch/branch dance.

Agent staffing (6 concurrent max in Phase 1):
| Stream | Effort | Notes |
|---|---|---|
| A gfx | XL | long pole; strongest agent; +F's agent when free |
| B os | M | well-trodden libctru territory |
| C audio | M | HLE-first keeps it bounded |
| D assets | M | mostly PC-side tooling |
| E 32-bit | M | pure PC work, merges first |
| F audit | S | report-only, finishes early |

---

## 6. Known risks & tripwires

| Risk | Tripwire | Response |
|---|---|---|
| LUS won't compile under devkitARM | Phase 0 spike fails | Fall back to hand DL interpreter (sm64 model); stream A scope grows, contracts amended |
| Combiner modes exceed TexEnv stages | Stream F census shows >6-stage modes | Multi-pass rendering in stream A, or documented visual approximation per mode |
| Physics doesn't fit 804 MHz @ 60 fps | Stream F CPU report | Ship 30 fps cap; machine-count option; old3DS dropped formally |
| SD I/O too slow for O2R on-demand | Stream D measurement | Pre-extracted flat files, loading screens |
| HLE audio inaccurate for this title | Stream C A/B vs LLE capture (`gdx_audio_capture.h` exists for this) | Targeted HLE fixes; LLE only if budget allows |
| devkitARM C++20 gaps | Phase 0 compile | Local polyfills / `-std=gnu++2a` fallbacks; no core rewrites |
| Memory exceeds 3DS app region | Stream F memory table; heap high-water under Citra at M2 | Shrink LUS resource cache, stream instead of cache, drop prewarm |
| Citra renders ≠ real hardware (PICA/TexEnv accuracy) | Hardware smoke test at M1/M2, not M5 (stream A owns) | Fix against hardware early; keep Citra for iteration only |
| Wrong ROM region breaks pre-bake/audio ucode | Build pins `VERSION_US=1`; LLE ucode offsets are US rev0 (`port/gdx_audio_lle.c:76-79`) | Stream D docs: US rev0 only for MVP; region detect + clear error in prebake tool |

---

## 7. What MVP deliberately excludes

- ImGui enhancement menu (config file; bottom-screen UI later)
- 64DD Expansion Kit content (post-MVP, stream D groundwork laid)
- Old3DS support (formal decision after stream F report)
- Stereoscopic 3D (plumbed in stream A, off)
- Texture packs, Discord RPC, crash handler, widescreen/ultrawide enhancements
  (3DS top screen is 400×240 — 5:3; decide letterbox vs native aspect in stream A)
