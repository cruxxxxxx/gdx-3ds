# Building from source

This is for building the console binary yourself instead of downloading the
release — grab the `.3dsx`/`.cia` from the
[latest release](https://github.com/cruxxxxxx/gdx-3ds/releases/latest) and
see the main [README](../README.md) instead if you just want to play; asset
prebake is a separate step ([`tools/prebake/README.md`](../tools/prebake/README.md)).

## 1. What you need

| Tool | Why | Get it |
|---|---|---|
| **devkitPro**, `3ds-dev` group (devkitARM, libctru, citro3d, 3dstools) | The cross-toolchain and 3DS libraries | `dkp-pacman -S 3ds-dev` after installing [devkitPro](https://devkitpro.org) |
| **CMake 3.20+** | Configures the build | Comes with most devkitPro installs, or your package manager |
| **Python 3** | Runs the host-side test/build scripts | python.org, or your package manager |
| **PyYAML** | Only if you build an **Expansion Kit** target (`GDX_3DS_EXPANSION_KIT=ON`) — CMake runs an asset-generation script at configure time that needs it | `pip install pyyaml` |
| **makerom** + **bannertool** | Only to produce the installable `.cia`; the `.3dsx` builds without them | See [`port/3ds/packaging/README.md`](../port/3ds/packaging/README.md) |

Ninja and ccache are optional but picked up automatically if they're on `PATH` —
they make rebuilds noticeably faster.

You never need a game ROM or disk image — this repository and its build
tools never touch your game data; see `tools/prebake/README.md`.

Pillow is never needed for building. It's only used to hand-regenerate the
Expansion Kit banner art (`port/3ds/packaging/README.md`); the banner itself
ships pre-generated in the repo.

## 2. Clone with submodules

The `o3ds`/`n3ds` targets need only the `decomp` and `torch` submodules. An
Expansion Kit target (`o3ds-ek` / `n3ds-ek`) additionally needs the
`fzerox-expansion-kit` submodule: `tools/gen_ek_assets.py` reads its
`assets/yaml/jp/**/*.yaml` and `tools/patch.py` at CMake configure time to
generate the EK asset sources, and there's no prebaked fallback — an EK
configure fails outright without it. It does **not** need the
`libultraship` submodule for any target — the slice of it the console
compiles is vendored in-tree at `port/3ds/vendor/lus/` with every
3DS-specific patch already folded in.

Get the repository with `git`, not GitHub's "Download ZIP" button — a zip
download omits submodule content, so `decomp/` lands empty and the build
fails later with no indication the clone was the cause.

```sh
git clone --recurse-submodules https://github.com/cruxxxxxx/gdx-3ds.git
cd gdx-3ds
# or, if you already cloned without --recurse-submodules:
git submodule update --init decomp torch                      # o3ds / n3ds
git submodule update --init decomp torch fzerox-expansion-kit # o3ds-ek / n3ds-ek
```

`--recurse-submodules` also pulls in `libultraship`, which you don't need for
this build — to skip it, clone plain (no `--recurse-submodules`) and use the
targeted `git submodule update --init` line above instead.

(`torch` supplies a small source module — `libmio0`, MIO0 decompression —
that the port compiles directly into its asset-loading code; it is not built
or linked as a separate library. It's also used standalone to build
`gdx-extract`, the asset extractor build described on the prebake page.)

## 3. One-command build: `tools/build-3ds.sh`

`build-3ds.sh` configures and builds the chosen console target(s) — it does
**not** apply the `decomp` patch stack. Apply that first, either by running
`tools/ci-3ds.sh` once (§4 — it applies the stack, then also runs the host
tests and a build, which is harmless but slower) or by applying the patches
yourself per `port/3ds/patches/README.md`. Then:

```sh
export DEVKITPRO=/opt/devkitpro     # wherever devkitPro is installed
tools/build-3ds.sh n3ds             # or: o3ds | o3ds-ek | n3ds-ek | all
```

This collects the results — named per target, plus a combined checksum file —
in `dist/3ds/`:

| Target | Console | Content | Files |
|---|---|---|---|
| `o3ds` | Old Nintendo 3DS / 2DS | F-Zero X | `G-Diffuser-3DS-O3DS.3dsx` / `.cia` |
| `n3ds` | New Nintendo 3DS / New 2DS XL | F-Zero X | `G-Diffuser-3DS-N3DS.3dsx` / `.cia` |
| `o3ds-ek` | Old Nintendo 3DS | F-Zero X + Expansion Kit | `G-Diffuser-3DS-O3DS-EK.3dsx` / `.cia` — **experimental**: runs poorly on real Old 3DS hardware (jittery 64DD courses, breaking-up disk music, laggy editors) |
| `n3ds-ek` | New Nintendo 3DS | F-Zero X + Expansion Kit | `G-Diffuser-3DS-N3DS-EK.3dsx` / `.cia` |

`all` builds all four, including the experimental `o3ds-ek`. Useful flags:

| Flag | Does |
|---|---|
| `--list` | Print the available targets and exit |
| `--jobs N` | Parallel build jobs (defaults to your CPU count) |
| `--out DIR` | Write results somewhere other than `dist/3ds/` |
| `--no-cia` | Skip the `.cia` step (handy if you don't have makerom/bannertool installed) |
| `--clean` | Wipe the target's build directory before configuring (otherwise builds are reused across runs) |
| `--zip [VERSION]` | Also package each target built this run into a zip, laid out like an SD card drop (in `<out>`) |

Run the build for the console you actually own — a New 3DS build won't fit
an Old 3DS's memory budget. See `GDX_3DS_O3DS` in §6 for why.

## 4. `tools/ci-3ds.sh` — the full local check

`ci-3ds.sh` is a separate script from `build-3ds.sh` — it does not run
automatically as part of a `build-3ds.sh` invocation. Run it once yourself
(or apply the patches by hand per `port/3ds/patches/README.md`) to prepare
`decomp/` before using `build-3ds.sh`, and it's also the right full check to
run before sending a change anywhere:

```sh
export DEVKITPRO=/opt/devkitpro
tools/ci-3ds.sh --jobs 8
```

From a clean, patch-stack-free checkout it:

1. Applies the full `decomp-*.patch` stack under `port/3ds/patches/` to the
   `decomp/` submodule's working tree (idempotent — safe to re-run; see
   `port/3ds/patches/README.md` for the current list and apply order).
2. Builds and runs the host-side unit tests (the parts of the port that compile
   and run directly on your machine, no 3DS hardware or emulator needed).
3. Configures and builds the 3DS target with the devkitARM toolchain, timing the
   configure, full, and incremental build.

Add `--emu` to also boot the result in an emulator and check it runs past boot
with no fatal errors; `--no-ninja` / `--no-ccache` force the plain generator and
compiler cache off if either is causing trouble; `--no-incremental` skips the
incremental-rebuild timing step. `--help` prints the rest.

## 5. The manual build (what the scripts automate)

If you want to drive CMake yourself instead:

```sh
# 1. Apply the patch stack (see port/3ds/patches/README.md for the exact list/order,
#    or just run tools/ci-3ds.sh once to apply it for you).

# 2. Configure.
cmake -S . -B build-3ds -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/3DS.cmake" \
      -DGDX_PLATFORM_3DS=ON -DCMAKE_BUILD_TYPE=Release
      # add e.g. -DGDX_3DS_O3DS=ON here for an Old 3DS build — see the table below
      # if reusing an existing build-3ds dir created before GDX_ALLOW_JP_INPUTS
      # defaulted on, also pass -DGDX_ALLOW_JP_INPUTS=ON explicitly here --
      # option() never overwrites an already-cached value.

# 3. Build.
cmake --build build-3ds --parallel 8               # G-Diffuser-3DS.3dsx
cmake --build build-3ds --target G-Diffuser-3DS-cia # + G-Diffuser-3DS.cia (needs makerom/bannertool)
```

Artifacts land in `build-3ds/port/3ds/`. The `.elf` alongside them is what you
want for turning a crash address in a log into a source line.

## 6. What the CMake options mean

| Option | Default | Effect |
|---|---|---|
| `GDX_3DS_O3DS` | `OFF` | Builds the Old 3DS memory profile: a smaller RDRAM arena and a pinned linear heap, sized to fit an Old 3DS's actual memory grant. This is a *compile-time* choice about how much memory the build asks for; which performance settings run is decided separately, at runtime, by detecting the console model. |
| `GDX_3DS_EXPANSION_KIT` | `OFF` | Compiles in the 64DD Expansion Kit content (course editor, machine creator, DD cups). This is a different compile of the game, not an add-on: it swaps out several subsystems and installs as its own HOME-menu title alongside the regular game, with its own save file. Needs Python3 + PyYAML at configure time (§1) and the `fzerox-expansion-kit` submodule (§2). |
| `GDX_ALLOW_JP_INPUTS` | **`ON`** on 3DS builds (`OFF` on desktop) | Accepts Japanese-region game data (JP ROM / the retail JP Expansion Kit disk). On by default for 3DS builds; off by default on desktop. |

3DS defaults this on because the console never reads a ROM directly (the
PC-side prebake step keeps its own region check) and the only retail
Expansion Kit disk is Japanese; desktop defaults it off because the JP
raw-ROM path has known-bad audio, which doesn't apply to 3DS Expansion Kit's
streamed-disk path.

Several other build-time knobs exist for porting/debugging work (memory-arena
size overrides, bisect build tagging, and similar) — see the comments above
each `option(...)` in `port/3ds/CMakeLists.txt` and `port/3ds/game/CMakeLists.txt`
if you need them; they aren't part of the normal build.

## 7. Building in a container (Docker / devkitPro)

You don't need to install devkitPro on your machine at all — the official
`devkitpro/devkitarm` container image has the whole toolchain, the same one
this project's CI uses. With Docker installed:

```sh
docker run --rm -v "$PWD":/src -w /src devkitpro/devkitarm:latest bash -lc '
  apt-get update -qq && apt-get install -y -qq python3 python3-yaml cmake ninja-build
  export DEVKITPRO=/opt/devkitpro
  git submodule update --init --depth 1 decomp torch                      # add fzerox-expansion-kit too for an -ek target
  tools/ci-3ds.sh --jobs 4
  tools/build-3ds.sh <target>     # o3ds | n3ds | o3ds-ek | n3ds-ek | all
'
```

Run `ci-3ds.sh` once per fresh checkout to apply the `decomp` patch stack (§4),
then `build-3ds.sh <target>` for the console you actually own — same flags as
§3, inside the same container. This lands results in `dist/3ds/` inside the
bind-mounted repo, same as a local build. The `.cia` needs makerom and
bannertool as well, which aren't in the image; without them `build-3ds.sh`
warns and skips just the `.cia` step (or pass `--no-cia` to skip it without
the warning) — build or copy the tools in as described in
`port/3ds/packaging/README.md` to get a `.cia` from this route too.

## 8. Windows

**Use Docker (§7 above)** — it avoids setting up a native devkitPro/MSYS2
toolchain entirely. Install Docker Desktop with WSL2, then run §7's commands
from a WSL shell (bind-mount the repo from there, not from a Windows path,
for reasonable build speed), substituting the target name for your own
console (`o3ds` for an Old 3DS, etc.) in the `build-3ds.sh` line — a plain
copy-paste builds the New 3DS target, which won't fit an Old 3DS's memory
budget. This is the simplest route and the one past users have had the least
trouble with.

If you'd rather not run Docker, native devkitPro also works:

1. Install devkitPro ([devkitpro.org](https://devkitpro.org)) using its Windows
   installer, and add the `3ds-dev` group when prompted (or run
   `dkp-pacman -S 3ds-dev` afterward from the devkitPro MSYS2 shell).
2. Install Python 3 from python.org and CMake, both available from that same
   shell's `pacman`, or installed separately on Windows with their installers
   added to `PATH`.
3. Open the **devkitPro MSYS2** shell (not a plain Windows terminal — it sets
   up the cross-compiler environment) and follow §2 (clone), §4
   (`tools/ci-3ds.sh`, to apply the `decomp` patch stack — mandatory on every
   platform, including Windows), and §3 (`tools/build-3ds.sh <target>`) above
   from there — §5 is only needed if you want the manual steps.

## Reference

- Patch stack details and apply order: `port/3ds/patches/README.md`
- CIA packaging, title IDs, and the Expansion Kit banner: `port/3ds/packaging/README.md`
- Converting your own game dump (no compiler needed): `tools/prebake/README.md`
- Running the result on real hardware, SD card layout, troubleshooting: `docs/3DS-HARDWARE.md`
