# Contributing to G-Diffuser

Thanks for wanting to help. G-Diffuser is a native source port of F-Zero X built on the
[inspectredc/fzerox](https://github.com/inspectredc/fzerox) matching decompilation and
[libultraship](https://github.com/Kenix3/libultraship), targeting PC (Windows/Linux, see
[`README-G-Diffuser.md`](README-G-Diffuser.md)) and the Nintendo 3DS (see
[`README.md`](README.md)).

Two things here are unusual, and will cost you time if you don't know them going in:

- **`decomp/` is a matching decompilation.** Your change has to work on PC *and* leave the retail
  build byte-identical. The PC build cannot tell you when you have broken the second one.
- **A PR gets a 3DS build check, but nothing checks the PC/decomp side.**
  [`release-3ds.yml`](.github/workflows/release-3ds.yml) builds and host-tests the three released
  3DS console targets on every push and pull request to `main` — it shows green/red per target,
  but it's a status check, not a merge gate, and attaches nothing. For a PC or `decomp/` change,
  your local build is the only check that runs. See [Releases](#releases) for what the same
  workflow does on a tagged release; `decomp/` has its own CI too, and it checks something
  different.

Read [Things that will bite you](#things-that-will-bite-you) before you touch `decomp/`.

## No game files. Ever.

**Never put a ROM, disk image, IPL dump, extracted asset, or generated `fzerox.o2r` in the
repository, in an issue, or in a pull request.** Not as an attachment, not in a test fixture, not
base64'd in a comment. This is not negotiable and there is no exception for "just a small texture".

Contributors supply their own legally obtained dumps. If a bug can only be shown with game data,
describe it, attach a log, and say which file reproduces it — do not attach the file.

## Building

### PC (Windows / Linux)

Full prerequisites and commands are in [`README-G-Diffuser.md`](README-G-Diffuser.md#building).
The short version:

| | |
| --- | --- |
| **Clone** | `git clone`, then `git submodule update --init libultraship decomp torch fzerox-expansion-kit` — the desktop build needs all four submodules; without them it will not configure |
| **Windows** | Visual Studio 2022, CMake 3.24+, Python 3 |
| **Linux** | CMake 3.24+, Ninja, a C++20 compiler, Python 3, and libultraship's dependencies from your distribution |

### Nintendo 3DS

`tools/build-3ds.sh <o3ds|n3ds|o3ds-ek|n3ds-ek|all>` configures and builds with devkitPro, and
collects the result in `dist/3ds/`. Full prerequisites, the four build targets, and the underlying
CMake flags: [`README.md`](README.md#building-from-source) and
[`docs/BUILDING.md`](docs/BUILDING.md). Initialise the submodules by name:
`git submodule update --init decomp torch` for O3DS / N3DS, plus `fzerox-expansion-kit` for the
`-ek` targets. The `libultraship` submodule is not needed for this lane —
the sources the console compiles are vendored in-tree at `port/3ds/vendor/lus/`.

### Python

**PC builds need Python 3, not optionally.** `port/CMakeLists.txt:8` is
`find_package(Python3 COMPONENTS Interpreter REQUIRED)` — configure fails outright without an
interpreter, because build-time generators run during the build. Those generators also import
**PyYAML** and **Pillow** (`python3-yaml` and `python3-pil` on Debian-likes); CMake cannot check
for them, so a missing one surfaces mid-build as a `ModuleNotFoundError`.

**3DS builds need no Python at all** for the plain `o3ds` / `n3ds` targets. `port/CMakeLists.txt`
(the file above) is PC-only — the root `CMakeLists.txt` branches into `port/3ds/` instead and
never configures it for a 3DS build. Only the `o3ds-ek` / `n3ds-ek` targets need Python 3 +
PyYAML, checked inside the `GDX_3DS_EXPANSION_KIT` guard in `port/3ds/game/CMakeLists.txt` —
matching what [`README.md`](README.md#building-from-source) already says.

Two things routinely trip people up on Windows:

- CMake defaults to the **Visual Studio generator**, which is multi-config. It ignores
  `-DCMAKE_BUILD_TYPE`, so you must pass `--config Release` at build time. **Ninja** is
  single-config and works the other way around: `-DCMAKE_BUILD_TYPE` at configure time, and
  `--config` does nothing. Both paths work; the README has the exact commands for each.
- Build from a shell **without** MSYS2/MinGW on `PATH`, or MSVC may pick up MinGW headers.

## Checking your change

### 1. Prove the binary actually contains it

A build that exits 0 is not evidence that your change was compiled. This project has shipped a
stale binary before, from a silently-skipped recompile and from a `build && copy` chain that
copied after a failed build.

The discipline that came out of it:

```sh
# Never this — the copy runs even when the build fails:
cmake --build build/x64 --config Release && cp .../G-Diffuser.exe <testdir>/

# This: build, VERIFY the artifact, then copy.
cmake --build build/x64 --config Release --parallel
python -c "import sys; d=open(r'build/x64/port/Release/G-Diffuser.exe','rb').read(); \
sys.exit(0 if b'SOME_STRING_YOUR_CHANGE_ADDED' in d else 1)" \
  && cp build/x64/port/Release/G-Diffuser.exe <testdir>/
```

If your change adds no new string literal, add one on purpose — a log line, an env-gate name. A
change you cannot find in the binary is a change you cannot prove you tested.

### 2. Run the console tests

They need no ROM, no window and no assets, and each one exits **0** on success. They catch the
failure modes specific to this port — pointer truncation, byte order, DSP maths.

```sh
# Windows (Visual Studio generator)
cmake --build build/x64 --config Release --target gdx_dsp_tests

# Linux (Ninja)
cmake --build build/x64-linux --target gdx_dsp_tests
```

`--target` takes several names at once. `ALL_BUILD` (Visual Studio) or the default target (Ninja)
builds the whole set along with everything else.

| Target | Run it when you touched | Covers |
| --- | --- | --- |
| `gdx_dsp_tests` | Audio | VADPCM, resample, FIR, envelope kernels |
| `gdx_pcm_capture_tests` | Audio capture | Streamed `.pcm` layout and its SHA-256 sidecar |
| `gdx_vi_fallback_tests` | VI / scanout | RGBA5551 → RGBA8888 conversion |
| `gdx_gfx_pack_tests` | Anything touching `Gfx` or the GBI macros | Proves a 64-bit host pointer survives in `Gfx.w1` |
| `gdx_gfx_convert_tests` | Display-list conversion | The binary N64 → wide converter and its cache |
| `gdx_rsp_boot_tests` | LLE audio / the cxd4 RSP | Booting the real audio microcode. Skips itself if the ucode blobs are absent |
| `resource_smoketest` | Resource loading | The `.o2r` pipeline. Only configured when `libultraship` is a target |

Executables land beside `G-Diffuser` — on the Visual Studio generator, `build/x64/port/Release/`.

### 3. Play it, if you touched timing, pacing or rendering

There is no substitute for driving a race. Run with `GDX_PERF=1`, play, and read the `[interp-p2]`
lines in `gdiffuser-run.log`.

The one **hard contract** is `sim_hz`: it must hold ≈ 59.9. That is the simulation clock, and a
change that drags it below ~59 has broken game speed no matter what the frame counter says —
the symptom is the whole game running in slow motion, which is easy to miss on a frame-rate readout
that still looks healthy. `presents/s` may legitimately vary with load; `sim_hz` may not.

Frame-capture and pad-script replay tooling — including a gotcha with `GDX_INPUT_SCRIPT`'s `LOG`
lines firing even when the preceding `WAITMODE` never succeeded — are covered in the field
glossary at [`docs/DIAGNOSTICS.md`](docs/DIAGNOSTICS.md).

### 4. If you touched an asset recipe under `decomp/assets/yaml/`

Run the asset-binding lint. It reads the recipes, reports colliding symbols, and writes nothing:

```sh
python tools/gen_asset_bindings.py --lint-only
```

A clean run ends with `LINT-ONLY [us/rev0]: 3553 asset symbols scanned, nothing written`. Four
duplicate-offset warnings in `create_machine_textures` are present today and are **not** caused by
your change.

## Things that will bite you

### 1. A change that builds under `PORT` can still break the retail build

Every host-side behavioural change in `decomp/` is wrapped in `#ifdef PORT`, because the non-`PORT`
build must stay byte-identical to the retail ROM. **The port build never compiles the non-`PORT`
path**, so it cannot warn you when you have broken it.

The sharp edge is a **`PORT`-only helper called from ungated code**. It does not exist in the retail
configuration, so the file stops compiling entirely.

The rule: *if a `PORT`-only helper is reachable from ungated code, it must have a paired non-`PORT`
definition that reproduces retail behaviour exactly.* The codebase does this consistently — two live
examples:

```c
/* decomp/include/global.h — the checkpoint macro compiles away entirely in retail */
#define GDX_CK(x)                 /* line 18: retail */
#define GDX_CK(x) gdx_ck(#x)      /* line 40: PORT   */

/* decomp/src/overlays/course_edit/191080.c — translated-disk labels fall back to the JP string */
#define GDX_EK_LABEL(bound, jp) ((bound)[0] != 0 ? (u8*) (bound) : (u8*) (jp))  /* line 25: PORT   */
#define GDX_EK_LABEL(bound, jp) ((u8*) (jp))                                     /* line 69: retail */
```

Whatever the helper yields in its inert state must equal what retail sees unconditionally.

Verifying that matching still holds is a **separate workflow inside `decomp/`**, with its own
Makefile and the MIPS toolchain, checked against committed ROM checksums — see `decomp/README.md`.
Only that workflow can tell you that you have broken matching, and it runs against `decomp/`'s own
committed submodule pointer, so a decomp edit made for the port can pass everything you run locally
and surface later.

### 2. Never blind-regenerate `port/gen/AssetBindings.c` (or `LinkStubs.c`)

These files are generator output **plus** hand-maintained corrections: real array sizes measured at
runtime, endian-fixup ranges trimmed to true command boundaries, symbols the generator cannot infer.
A blind regenerate deletes those silently, still compiles, and corrupts every asset it touched.

`tools/gen_asset_bindings.py` refuses to write to the tracked path for exactly this reason, and the
refusal compares real paths, so pointing `--out` at the tracked file is caught too. Generate to a
scratch path and diff:

```sh
python tools/gen_asset_bindings.py --profile us/rev0 --out /tmp/AssetBindings.fresh.c
diff /tmp/AssetBindings.fresh.c port/gen/AssetBindings.c
```

To show what is at stake — segment 8's fixup table: the generator emits one coarse
32-bit-word-swap row per model, but the tracked file **splits** some of those into a word-swap row
for the display-list part plus a *vertex* fixup row (16-byte stride) for the vertex block — same
start offset, same total span, correct treatment for each half. A blind regenerate would
word-swap those vertex blocks as plain 32-bit words and corrupt every vertex they touch. In short:
the generator currently undercounts rows because it can't tell a vertex block from a
display-list block — always diff before replacing, never force-overwrite.

`--force-overwrite` exists and obliges you to re-apply every hand edit yourself.

### 3. Turn the diagnostics on before you guess

**Almost every diagnostic in this port is off by default, including in Release builds.** A log that
looks complete is usually a log with the interesting lines switched off.

| Switch | Effect |
| --- | --- |
| `GDX_LOG=1` | Opens `gdiffuser-run.log` beside the executable. Without it, no run log is created at all. |
| `GDX_TRACE` | Tri-state, and the one gate whose stock value is not off: **on by default in Debug, off in Release.** Set it explicitly. |
| `GDX_DIAG_VERBOSE=1` | Unlocks whole per-frame families at once (`[gfxdiag] [game] [seg] [sched] …`). |

```sh
GDX_LOG=1 GDX_TRACE=1 GDX_DIAG_VERBOSE=1 ./G-Diffuser
```

There is also an **always-on crash sink**: a crash writes `gdiffuser-crash.txt` beside the
executable with no environment variable set, on both Windows and Linux. Attach it if you have one.
Crash addresses are module-relative (`rva=`); the build also emits `G-Diffuser.map`, so an RVA
resolves to a symbol with nothing but the map file — parse the `Preferred load address`, subtract,
and bisect.

The full gate table is `port/gdx_dev_gates.c`, each row with its own description, and it is
surfaced in-game under **F1 → Dev Tools**. For what each switch reveals — and, importantly, which
ones *change* what is rendered rather than merely observing it — read
[`docs/DIAGNOSTICS.md`](docs/DIAGNOSTICS.md). Do not file a log captured with a behaviour-altering
gate on without saying so.

One convention to keep when you add a diagnostic of your own: **every A/B toggle must write its arm
state to the log.** This project once burned a full play-test on an experiment whose toggle left no
trace in the log — the result was uninterpretable and the run was wasted. If the gate isn't in the
log, the experiment didn't happen.

### 4. An intermittent bug here is often an order-dependent bug

Several asset segments are reused across mode changes, and the loader skips the reload — **and the
byte-order fixups with it** — when the content it wants is already resident. Anything that scribbles
into one of those buffers survives into a later mode, and the corruption surfaces somewhere with no
obvious connection to the code that caused it.

So: reproduce from a **cold start**, and record which screens you passed through. "Only happens
sometimes" often means "only happens if you visit Course Edit first".

### 5. There are two copies of the libultra headers, kept in sync by hand

`decomp/include/PR/` and `libultraship/include/libultraship/libultra/` both declare the N64 OS
types. They are duplicates, not one shared header, so a layout change has to be made **twice**.

They have already diverged: `OSContPad` is four fields in the decomp copy and `0x24` bytes in the
libultraship copy, which added gyro and right-stick fields. That is currently harmless — the decomp
function that would consume it is `#ifndef PORT` at every call site, and the port feeds input
through `port/input_bridge.c` instead. Treat it as a warning about the mechanism: **if you change a
shared OS struct, change both trees.**

## Code style

There is no repository-wide formatter for port code. `decomp/`, `libultraship/`, `torch/` and
`fzerox-expansion-kit/` each carry their own `.clang-format`; `port/` does not.

- **Match the file you are editing.** Every file in `port/` is internally consistent; follow it.
- **In `decomp/`, follow the decomp's conventions**, not the port's — it is a submodule with its own
  standards, and its `.clang-format` applies.
- **Explain the non-obvious in a comment.** This codebase leans heavily on comments that record
  *why* — measured values, rejected alternatives, hardware quirks. `port/CMakeLists.txt` is the
  model. If you worked something out the hard way, write it down where the next person will trip.
  One caution that comes with the style: a comment that asserts a mechanism is a claim of evidence.
  Write the mechanism down **after** a measurement confirms it, not before — this tree has had to
  correct comments that confidently explained the wrong cause.

## Where to look

| You want to | Read |
| --- | --- |
| Build it, run it, report a bug (3DS) | [`README.md`](README.md) |
| Build it, run it, report a bug (PC) | [`README-G-Diffuser.md`](README-G-Diffuser.md) |
| Find which file owns a subsystem | [`port/README.md`](port/README.md) |
| Turn on logging, or find the switch that shows your bug | [`docs/DIAGNOSTICS.md`](docs/DIAGNOSTICS.md) |
| Understand the architecture | [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) |

`docs/` is tracked and maintained — read it as current. Where a document records a measurement, it
carries the date and the numbers, so stale claims are checkable rather than quietly trusted.

## The repository is five repos

Changes usually span more than one, and they publish in a fixed order:

| Repo | Branch | What lives there |
| --- | --- | --- |
| this repo | `main` | `port/`, tools, assets, docs — the port itself |
| `decomp/` (submodule) | `g-diffuser` | the matching decompilation + `#ifdef PORT` changes |
| `libultraship/` (submodule) | `g-diffuser` | the Fast3D/engine fork |
| `torch/` (submodule) | `g-diffuser` | the asset extraction tool |
| `fzerox-expansion-kit/` (submodule) | `g-diffuser` | the Expansion Kit's matching decompilation; only needed for the `-ek` 3DS targets |

The submodules' `main`/upstream branches track their upstreams — **never** target those with port
work. A PR that changes a submodule needs a PR against that submodule's `g-diffuser` branch, and
the parent-repo PR then bumps the submodule pointer. Submodule PRs merge first; the parent pointer
bump lands last.

## Submitting a change

**Outside pull requests are welcome.** The practical route:

1. **For anything non-trivial, open an issue first** at
   [github.com/cruxxxxxx/gdx-3ds/issues](https://github.com/cruxxxxxx/gdx-3ds/issues) and say what
   you intend to change. This is a port with sharp invariants (matching, byte order, the
   `PORT`-pairing rule) — two paragraphs of intent can save you a rewritten branch. Typo-level fixes
   can skip straight to a PR.
2. **Target `main`.** PRs are **squash-merged**: your branch history collapses into one commit on
   `main`, so commit locally however you like — the PR title and description are what survive.
3. **Branch naming is free.** Name it something you can find again.
4. **Commit / PR title convention:** `type(scope): summary`, matching the history —
   `feat(interp): …`, `fix(audio): …`, `refactor(firstboot): …`, `chore: …`. Keep the summary in
   the imperative and put the *why* in the body. Read `git log` for the house voice: bodies here
   state the measured problem and the mechanism of the fix, not a list of files touched.
5. **Say how you verified it.** Which tests ran, which script route, what the telemetry showed —
   and if you could not verify something, say that plainly. An honest "untested on Linux" costs
   nothing; a silent one costs the maintainer an evening.
6. **Review** is by the maintainer ([@cruxxxxxx](https://github.com/cruxxxxxx)). This is a
   one-person project reviewed in spare time — expect days, not hours, and expect questions about
   evidence rather than style.

**Discussion venue:** GitHub Issues, for now. There is no Discord or forum yet.

## Releases

**3DS releases are built by CI too** — the same [`release-3ds.yml`](.github/workflows/release-3ds.yml)
workflow mentioned above. On every push or PR to `main` it builds the three released console
targets (`o3ds`, `n3ds`, `n3ds-ek`) as a build-only check (nothing attached, doesn't gate
merging). On a `v*` tag, or a manual dispatch naming an existing release tag, it does the full
job for those same three targets: builds each one, zips it, and attaches the zips, the loose
`.3dsx`/`.cia`/`.elf` files, and one `SHA256SUMS` to the GitHub release, plus a "Which file do I
need?" table in the release notes. `o3ds-ek` is experimental and not released — it's excluded
from `all` and only builds when a manual dispatch's `targets` input names it explicitly.

**Linux releases are built by CI, not by hand.** Push a `v*` tag, or run the
*Release (Linux)* workflow manually, then download the tarball from the workflow artifacts and
attach it to the GitHub release. Publishing stays a deliberate human step.

The workflow builds inside an `ubuntu:22.04` container with pinned dependency versions. That is
not fussiness: an earlier hand-built Linux release (the PC port's own desktop `v1.0.0`, built
before this workflow existed) demanded a newer glibc, an unneeded CPU feature, and libraries not
present on users' systems — three defects invisible on the machine that produced it, each reported
separately by a different user. Pinning the build environment is what prevents all three.

`tools/check_linux_abi.py` enforces that, and `tools/package_linux.sh` refuses to write a tarball
until it passes. Run it against any Linux build before you hand it to anyone:

```sh
python3 tools/check_linux_abi.py build/x64-linux/port/G-Diffuser
```

It reports the glibc floor, the CPU level the loader will demand, and any dependency that would
have to already be installed on the user's machine. Ceilings live in its `--help`; raising one is
a decision about which distributions you are dropping, so change it deliberately.

**Windows releases are built by CI too.** Push a `v*` tag, or run the *Release (Windows)* workflow
manually; it packages from the static `build/x64` tree (the dynamic `build_x64` tree produces an
executable that needs eight DLLs beside it) and runs `tools/check_windows_deps.py` to prove the
zip is DLL-self-contained before anyone can download it — the same discipline as the Linux
workflow above, not a hand-built, hand-packaged process.

## Reporting a bug

Bug reports do not need any of the above. Each platform's README has its own checklist:
[3DS](README.md#reporting-a-bug) (which build, your console model, `log.txt` /
`log-EK.txt`, `crash*.txt`) or [PC](README-G-Diffuser.md#reporting-a-bug) (platform and renderer,
ROM region, repro steps, `gdiffuser-run.log`, and `gdiffuser-crash.txt` if the game crashed).

Reminder: **no game files, ever** — see [No game files. Ever.](#no-game-files-ever).
