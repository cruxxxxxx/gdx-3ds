# G-Diffuser 3DS — Architecture

How the Nintendo 3DS port is put together, with source paths, for a new contributor
or the maintainer coming back to this in six months.

G-Diffuser 3DS runs the **F-Zero X decompilation** on console hardware. The porting
model is the same tension the PC port started from — *the game logic must stay a
byte-matching N64 decompilation and also run correctly on a different CPU* — plus
everything a closed, memory-constrained handheld adds on top: two console models
with different memory budgets, no desktop windowing or file dialogs, a GPU that is
nothing like OpenGL/D3D, and no process environment to toggle anything from.

Read [§6](#6-where-to-look-when-something-renders-wrong) first if you are here because
something is on fire.

---

## 1. Repository layout and ownership

| Path | Kind | Owns |
|---|---|---|
| `decomp/` | submodule | The matching F-Zero X decompilation: `src/`, `include/`, its own build and expected ROM checksums. **The game logic.** Shared with the PC port; nothing here is 3DS-specific. |
| `port/` | in-tree | The **shared host layer**: `decomp_port.c`, `n64_sched.c`, `n64_gfx_bridge.cpp`, `disk_buffer.cpp`, `rom_buffer.cpp`, the fiber scheduler contract, generated asset bindings. The 3DS build does not use `port/CMakeLists.txt` (that wires the desktop executable); `port/3ds/game/CMakeLists.txt` pulls the specific `port/*` translation units the console needs and compiles them into the console game library instead. |
| `port/3ds/` | in-tree | Everything 3DS-specific. See [§4](#4-the-3ds-platform-layer-port3ds) below for what is in each subdirectory. |
| `libultraship/` | submodule | The desktop runtime (SDL window/input/audio, ImGui, ship resource loader). **Not linked into the 3DS build at all** — see §4.2. |
| `torch/` | submodule | PC-side asset extraction, built into the standalone `gdx-extract` tool. Used by `tools/prebake` before anything reaches the console; see §3. |
| `fzerox-expansion-kit/` | submodule | The 64DD Expansion Kit's own asset source (`assets/yaml/jp/`, `tools/patch.py`). Only needed to configure an EK target (`-DGDX_3DS_EXPANSION_KIT=ON`) — `tools/gen_ek_assets.py` reads it at CMake configure time, with no prebaked fallback. The shipped O3DS/N3DS targets never evaluate it. |
| `tools/` | in-tree | Python build/asset/dev-loop tooling: `tools/build-3ds.sh` (the one-command 3DS build, §4.6), `tools/prebake/` (PC-side ROM→archive conversion), `tools/dev/` (the WiFi and emulator dev loops — see `docs/HW-DEV-LOOP.md`), the asset-binding and link-stub generators. |
| `docs/` | in-tree, tracked | Ships with the release. |

### Dependency direction is one-way

```
                 ┌──────────────┐
                 │   port/      │──────┐
                 └──────┬───────┘      │
                        │              │
                        ▼              ▼
                 ┌──────────────┐  ┌───────────────────────┐
                 │   decomp/    │  │ libultraship (desktop) │
                 └──────────────┘  │ port/3ds/vendor/lus    │
                                    │ (3DS)                  │
                                    └───────────────────────┘
```

Decomp translation units never `#include` a port header — a decomp source that needs
a host helper forward-declares it (`extern`), resolved at link time. The convention is
stated in the code itself, e.g. `decomp/src/sys/dma.c`.

---

## 2. The `PORT` gating convention

Every host-side behavioural change to decomp code is wrapped in `#ifdef PORT`, because
the non-`PORT` build must stay byte-identical to retail. This is a hard invariant, not
a preference, and it is shared by every platform that compiles `decomp/`. The 3DS build
defines `PORT=1` on its game library exactly as the desktop build does, so the same
hazards apply:

- A `PORT`-only construct referenced from ungated code fails to compile outside `PORT`.
- A decomp edit that compiles clean under `PORT` can still break the retail
  (non-`PORT`) build.
- The superproject has no CI that catches that; only `decomp/`'s own CI does, against
  its committed submodule pointer.

Two macros that matter specifically for porting to a new target live in
`decomp/include/macros.h`: `ROM_READ(addr)` becomes `gdx_rom_read32()` under `PORT`
(every platform shares this), and segment/ROM addressing gating lives in
`decomp/include/segment_symbols.h`.

---

## 3. The asset pipeline — why the 3DS step is different

On desktop, `gdx-extract` (Torch) runs **on the PC the game launches from**, as a
child process, the first time you boot with a ROM configured. The 3DS cannot do that:
no ROM ever reaches the console, and extraction is too slow and too memory-hungry for
the hardware. Instead:

```
your ROM + your 64DD disk (PC)
        │
        ▼
  tools/prebake  (runs gdx-extract / Torch on your PC)
        │
        ▼
  fzerox.o2r + gdiffuser.o2r
        │
        ▼
  copied to sdmc:/3ds/gdiffuser/   (console reads only the finished archives)
```

The console-side code (`port/3ds/assets/`) only needs to **read** `.o2r` archives off
the SD card — it never parses a ROM. `gdx3ds_fs_sd.c` is the SD-card filesystem
backend for the vendored resource loader (the desktop equivalent reads from a regular
filesystem or a packed zip; see §4.2).

The binding-table mechanics (`AssetBindings.c`, placeholder symbols, the three fixup
`kind`s, the "never regenerate in place" hazard) are unchanged from the PC port and
are not repeated here — same generator, same archive format, same caution applies if
you touch `tools/gen_asset_bindings.py`.

---

## 4. The 3DS platform layer (`port/3ds/`)

### 4.1 Build wiring

`port/3ds/CMakeLists.txt` is the **only** place 3DS sources get registered — the shared
`port/CMakeLists.txt` is never edited for 3DS-specific sources. Cross-compiling needs devkitPro's
toolchain file plus `-DGDX_PLATFORM_3DS=ON`; `tools/build-3ds.sh <target>` wraps this
(§4.6). The four buildable console configurations are two independent choices multiplied
together — all four are released, `o3ds-ek` as experimental because 64DD courses, disk music,
and the editors run poorly on real Old 3DS hardware (see RELEASE-NOTES.md Known issues):

| Flag | Default | Effect |
|---|---|---|
| `GDX_3DS_O3DS` | `OFF` | Old 3DS / 2DS profile: 8 MiB RDRAM arena (vs. 16), a pinned linear-heap size, size-optimised codegen in cold paths. The Old-3DS-specific option is **one flag**; the memory split it chooses is sized to the console's measured real heap/linear-heap budget. |
| `GDX_3DS_EXPANSION_KIT` | `OFF` | Compiles the 64DD Expansion Kit — see `NATIVE-RENDERER.md` §2.6 for what it swaps and the runtime disk-streaming levers it needs. |

Combined: `o3ds`, `n3ds` (default, no flags), `o3ds-ek`, `n3ds-ek`. The CIA packaging
(`port/3ds/packaging/`) picks a distinct title ID per vanilla/EK pair so both can be
installed side by side on the HOME menu, sharing one SD folder.

Two more build options matter beyond this table — `GDX_3DS_RDRAM_MB` and
`GDX_3DS_SIM_O3DS` — see `NATIVE-RENDERER.md` §2.6 for their defaults and the
bisect hazard. `GDX_3DS_HEAPATTR` adds an allocator-interposition census and is a
measurement build, not a shipping one.

### 4.2 The vendored libultraship carve (`port/3ds/vendor/lus/`)

The 3DS build does **not** link the `libultraship/` submodule — it would pull in SDL2,
ImGui, and desktop-only resource-loading code that has no 3DS equivalent. Instead,
`port/3ds/vendor/lus/` is a frozen, in-tree carve: the subset of libultraship's Fast3D
display-list interpreter and ship resource (O2R) system the 3DS actually needs, with
its own `lus-*.patch` set already folded in (`port/3ds/vendor/lus/patches/`). This is a
separate patch set from the `decomp-*.patch` stack `NATIVE-RENDERER.md` §5 verifies,
which patches `decomp/` instead. It was produced by `tools/vendor-lus.sh` from
the pinned submodule commit and is now the source of truth for that code on 3DS — the
script's `--verify` mode re-derives it and diffs, as a reproducibility check, but the
vendored tree itself is what ships. `port/3ds/lus_stubs/` supplies shadow headers
(`SDL.h`, `spdlog.h`, a `ship/window` stub) so the carved sources compile without
pulling in the real desktop libraries they still `#include`.

This carved interpreter is what draws a frame when the native renderer's `interp`
lever is off (the historical, libultraship-parity path). `port/3ds/gfx/gfx_citro3d.cpp`
implements that interpreter's `GfxRenderingAPI` against citro3d — the console
equivalent of the desktop OpenGL/D3D11 backends — and the same backend also receives
the native renderer's batches when `interp` is on. Either way, citro3d
(`port/3ds/vendor/citro3d/`) is what actually talks to the PICA200 GPU.

### 4.3 The native renderer

`port/3ds/gfx/n64/` is the project's own F3DEX2→citro3d interpreter and the retained
track/machine/effect/HUD fast paths that bypass display-list interpretation
altogether for the three things a race spends its frame on. This is the headline
change since the last public release and has its own reference:
**[`docs/NATIVE-RENDERER.md`](NATIVE-RENDERER.md)** — every `[native]` ini lever, how
each is verified, and what has been measured on hardware.

### 4.4 Audio (`port/3ds/audio/`)

Output goes through `gdx3ds_audio_ndsp.c`: one ndsp channel, stereo PCM16 at 32000 Hz,
fed by a lock-guarded push ring and three 16 ms wave buffers in DSP-visible
(`linearAlloc`) memory. Synthesis
itself runs in **HLE** (software mixing) rather than the cycle-accurate RSP
interpreter the desktop port also supports — an ARM11 core that tops out at 804 MHz
on New 3DS (less on Old 3DS) and is already carrying the game logic has no budget
left for RSP interpretation, and the HLE path is already the
correctness fallback the whole audio stack is checked against on every platform, so
making it primary on 3DS removes a component rather than a guarantee.
`[debug] audio_stream=1` streams the sample bank from the `.o2r` through a block
cache instead of holding it resident — see `docs/NATIVE-RENDERER.md` §2.6 for that and
the matching cache-size/verify keys.

### 4.5 Disk streaming (Expansion Kit)

The 64DD disk image (~64.9 MB) doesn't fit in a resident buffer on either console model, so it
streams off the SD card by default (`[debug] disk_stream = 2`). See `docs/NATIVE-RENDERER.md`
§2.6 for the mode table, cache keys, and the measured sweep behind the defaults.

### 4.6 Build and tool entry points

- `tools/build-3ds.sh <o3ds|n3ds|o3ds-ek|n3ds-ek|all>` — the one-command build; produces
  the `.3dsx`/`.cia` pair under `dist/3ds/` plus a `SHA256SUMS` file. If this script isn't
  present in your checkout, use the manual walkthrough instead: **[`docs/BUILDING.md`](BUILDING.md)**,
  which also covers full toolchain setup.

  | Flag | Effect |
  |---|---|
  | `--list` | Print the available targets |
  | `--jobs N` | Control build parallelism |
  | `--out DIR` | Redirect the output directory |
  | `--no-cia` | Skip CIA packaging (needs `makerom`/`bannertool`) and produce only the `.3dsx` |
- `tools/dev/hw.py` and `tools/dev/gdx-dev` — the hardware (WiFi) and emulator dev
  loops; see **[`docs/HW-DEV-LOOP.md`](HW-DEV-LOOP.md)**.
- `tools/ci-3ds.sh` — builds and runs the host-side test binaries that check the
  3DS-only code (vertex packer, block cache/disk streamer, config reader, LOD
  controller, the MAME↔`.ndd` disk converter, and others) without needing hardware or
  an emulator.

### 4.7 Memory profiles and runtime model detection

Which performance preset actually runs is decided **at runtime**, by asking the
hardware (`APT_CheckNew3DS`) — not by which build you installed. An `o3ds` build on a
New 3DS still selects New-3DS settings; the reverse does not work, because the New-3DS
build's RDRAM arena and memory layout do not fit an Old 3DS's real grant. Use the
build that matches your own console. The same model probe also picks this release's
`[perf] preset = auto` default — see `NATIVE-RENDERER.md` §4 — so a stock ini already
runs the native renderer tuned for whichever console it boots on.

---

## 5. Segments and live carves

Unchanged from the PC port, and shared code: the N64 game addresses assets through
segmented addresses, several segments are **mode-owned** (the same segment number
holds different content depending on the game mode), and `decomp_port.c`'s
`gdx_load_seg4_if_needed()` / `gdx_load_seg7_if_needed()` skip a reload — and the
byte-order fixup that comes with it — when the requested variant is already resident.
**The hazard: a carve mutated out of band survives into a later mode that requests the
same variant**, and the resulting defect has no obvious connection to the code that
wrote it. This logic is identical on 3DS; if a visual defect depends on which mode you
visited first, suspect this before anything console-specific.

---

## 6. Where to look when something renders wrong

| Symptom | Look at | How to check it |
|---|---|---|
| Wrong/missing texture on one object | Asset resolution, binding/fixup rows (§3) | Compare the dumped texture key against `AssetBindings.c` |
| Garbage geometry, stray huge triangles | Display-list conversion, vertex fixups | `docs/DIAGNOSTICS.md` |
| Whole screen or one layer missing | Viewport/scissor, citro3d submission | `docs/DIAGNOSTICS.md`; try `interp=0` to isolate the owned interpreter |
| Breaks only after visiting another mode first | **Sticky segment residency (§5)** | Reproduce from a cold boot, not mid-session |
| Native renderer disagrees with the old path | A `[native]` lever | `docs/NATIVE-RENDERER.md` §2.3 (verify/bisect levers) |
| Stutter at a mode change, race start, or EK disk access | Scheduler, DMA yield, disk streaming | `docs/NATIVE-RENDERER.md` §2.6; `docs/DIAGNOSTICS.md` |
| Audio pops, gaps, or stays silent | ndsp channel, `dspfirm.cdc`, streaming cache | §4.4 above; `docs/DIAGNOSTICS.md` |
| Crash on boot or during a race | Boot trail, crash dump | `docs/DIAGNOSTICS.md` |

Two habits that shorten most investigations:

1. **Confirm the code path you are editing actually runs.** Decomp has duplicated
   near-identical functions before, where a fix landed on the copy that is not called.
2. **Reproduce from a cold start**, not after wandering through menus — sticky
   residency (§5) makes mode order significant.

---

## 7. Document status

Supersedes the PC-port architecture doc for 3DS work — that doc doesn't cover the 3DS
platform layer, citro3d, or the native renderer.
