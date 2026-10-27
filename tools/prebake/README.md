# tools/prebake — PC-side asset pre-bake for the 3DS port

This entire page runs on your PC (Windows/macOS/Linux) — nothing here runs
on the 3DS.

The Nintendo 3DS build never runs Torch, Python, or first-boot extraction on
device (no CPU/RAM budget for it, and no portlibs). Instead, you bake the
asset archives **once on your PC** with this tool and copy them to the SD
card. The 3DS then mounts them read-only from
`sdmc:/3ds/gdiffuser/` (see `port/3ds/assets/gdx3ds_fs_sd.c`).

This tool does not reimplement extraction: it drives the **existing desktop
flow** — the deterministic `gdx-extract` (trimmed Torch) — with the same
arguments and validation gates first boot uses (ROM SHA-1 in, archive golden
SHA-256/entry-count out).

## What you need

1. **This repository**, cloned with `git` (not GitHub's "Download ZIP" button
   — a zip download omits submodule content, so `decomp/` lands empty and
   the extractor step below fails). With the `decomp` submodule initialized:

   ```
   git clone https://github.com/cruxxxxxx/gdx-3ds.git
   cd gdx-3ds
   git submodule update --init decomp
   ```

   (`torch` is only needed if you're building `gdx-extract` yourself — see
   the "build it yourself" steps below. Skip it if you're using the
   downloaded extractor.)
2. **`gdx-extract`**, the extractor binary. Easiest: download
   `gdx-extract-<your OS>.zip` from the
   [latest release](https://github.com/cruxxxxxx/gdx-3ds/releases/latest)
   (Windows x64, macOS arm64 / x86_64, Linux x64), unzip it anywhere, and pass
   it with `--extractor`. No compiler needed.

   To build it yourself instead, only the Torch submodule is required:

   ```
   git submodule update --init torch
   cmake -S torch -B build-extract -DUSE_STANDALONE=ON -DBUILD_STORMLIB=OFF \
         -DGDX_DETERMINISTIC=ON -DGDX_OUTPUT_NAME=gdx-extract -DCMAKE_BUILD_TYPE=Release
   cmake --build build-extract --config Release
   cmake --install build-extract --config Release --prefix build-extract/install
   ```

   The binary lands in `build-extract/install/bin/`. (A full desktop build's
   `--build-dir build` also works, as before.)

   The same zip also carries **`gdx-hudpack`**, which bakes the 3DS
   bottom-screen HUD (fonts, digits, portraits, chrome) from the archives
   this tool produces — see step 6 below. Keep it next to `gdx-extract` and
   the script finds it. To build it yourself (any C compiler, no submodules):

   ```
   cmake -S tools/hudpack -B build-hudpack -DCMAKE_BUILD_TYPE=Release
   cmake --build build-hudpack --config Release
   ```

   (`prebake.py` looks in `build-hudpack/` too, or pass `--hudpack <path>`.)
   Without it everything still works: the console bakes the HUD itself on
   its first boot (about a second on an Old 3DS) and caches the result.
3. **Your F-Zero X US rev0 cartridge dump**, big-endian (`.z64`) — the
   cartridge ROM, a separate file from any 64DD Expansion Kit disk image.
   This tool does not process the disk image at all; see "Expansion Kit"
   below.
   - SHA-1 must be `5f658e88ffa9de23cba6986a8fd3d3a90d7b4340`.
   - JP, PAL, rev1, and byte-swapped (`.v64`/`.n64`) dumps are rejected with
     a diagnosis; byte-swapped dumps can be converted to `.z64` first
     (tool64, or `dd conv=swab`), other regions/revisions cannot be used.
4. Python 3.8+ (standard library only).

## Run it

```
python3 tools/prebake/prebake.py --rom /path/to/fzerox-us-rev0.z64 --extractor /path/to/gdx-extract
```

### Windows, step by step

1. Install Python 3 from python.org (tick "Add python.exe to PATH").
2. Install [Git for Windows](https://git-scm.com/download/win) and clone the
   repository with git (not the "Download ZIP" button — see "What you need"
   above): `git clone https://github.com/cruxxxxxx/gdx-3ds.git`, then
   `cd gdx-3ds` and `git submodule update --init decomp`.
3. Download `gdx-extract-windows-x64.zip` from the latest release and unzip it,
   for example to `C:\gdx\gdx-extract.exe`.
4. In PowerShell, from the repository folder:

   ```
   python tools\prebake\prebake.py --rom C:\path\to\fzerox-us-rev0.z64 --extractor C:\gdx\gdx-extract.exe
   ```

5. Copy `dist\sdmc\` onto the SD card — see "Copy to the SD card" below.
   Nothing in this page needs Visual Studio, MSYS2, or devkitPro.

Options:

| Flag | Meaning |
|---|---|
| `--rom PATH` | (required) US rev0 big-endian ROM |
| `--build-dir DIR` | desktop CMake build dir; used to find `gdx-extract` (`<dir>/gdx-extract/install/bin/`) and, as a fallback, a prebuilt `gdiffuser.o2r` |
| `--extractor PATH` | explicit `gdx-extract` binary (overrides `--build-dir` discovery) |
| `--out DIR` | staging dir, default `dist/sdmc` |
| `--skip-golden` | development only: accept an archive that fails the golden SHA-256/entry-count gate |
| `--store-entry NAME` | keep this entry STORED (uncompressed) in `fzerox.o2r`; repeatable. Default: `audio_blob/audio_table`, `segment_blob/common_assets_compressed` |
| `--no-store` | install the extractor's archive as-is (every entry deflated) |
| `--hudpack PATH` | explicit `gdx-hudpack` binary (default: beside the extractor, then `build-hudpack/`, then `PATH`) |
| `--no-hudpack` | skip `hudbake.cache` (the console bakes the HUD on first boot) |

**Why some entries are stored.** The 3DS preloads `audio_blob/audio_table`
(10.7 MB, deflate saves only 9%) and `segment_blob/common_assets_compressed`
(2.5 MB of MIO0 data, 26%) at every boot. Inflating them is CPU-bound (~4 s
on a New 3DS's spare core, longer on the syscore) and the title music's
sample load can block on it, which is audible as a boot hiccup. Stored, the
preload is a plain SD read instead.

The archive grows ~1.65 MB (15.5 -> 17.2 MB). The golden gate still applies
to the extractor's raw output, before this step; the installed archive keeps
the same entry order, names, sizes and CRCs — only the two entries'
compression method changes.

**Multiplayer content check.** Rooms compare a hash of each archive's entry
names, CRCs and uncompressed sizes (`gdx_mpo_zip_cd_hash`), not its compressed
bytes, so archives prebaked on different PCs (another zlib, `--no-store`)
from the same ROM still match. `test_content_hash.py` (run by
`tools/ci-3ds.sh`) re-packs an archive at several levels and checks this.

The script:

1. **Validates the ROM** exactly like `port/gdx_extract_launch.cpp`:
   whole-file SHA-1 against the US rev0 hash resolved from
   `decomp/config.yml`. Clear failure text for JP/PAL/rev1/byte-swapped/junk.
2. **Runs the extractor**: `gdx-extract o2r <rom> -s decomp -d <tmp> -u
   2027490995` (the `-u` value is the US rev0 ROM CRC `0x78D90EB3` in
   decimal; without it Torch omits the `portVersion` record and the archive
   can never match the golden — see `tools/o2r_harness/README.md`).
3. **Validates the archive** against `port/gen/gdx_o2r_expected.h`
   (SHA-256 `1b95e895…`, 3610 central-directory records) — the same golden
   gate desktop first boot enforces, so a bad bake fails on your PC, not on
   the console.
4. **Installs** it as `fzerox.o2r` (atomic copy+rename), the runtime archive
   name — `generic.o2r` is only the extractor's output name.
5. **Produces `gdiffuser.o2r`** (the engine archive: Fast3D shaders +
   branding, zero game-derived content) via the existing deterministic
   `tools/gen_f3d_o2r.py`, falling back to a desktop build's copy.
6. **Bakes the bottom-screen HUD** into `hudbake.cache` with `gdx-hudpack`
   (`tools/hudpack`): the console's own recipes (`port/3ds/gdx3ds_hudbake.c`,
   compiled for your PC) run over the two archives above, and the result is
   written in the console's SD-cache format — byte for byte the file the
   console would write after baking from the same archives. It is keyed on
   the CRC-32 and size of every archive record it was baked from plus the
   recipes' build id, so the console uses it only while both still match;
   any other archive or game build rejects it, bakes, and overwrites it.
   Like the archives it is made from your own ROM on your PC and is never
   shipped. Optional: without it the console bakes on first boot.

## Copy to the SD card

The staging dir mirrors the SD root:

```
dist/sdmc/
└── 3ds/
    └── gdiffuser/
        ├── fzerox.o2r      (~game archive, tens of MB)
        ├── gdiffuser.o2r   (engine archive, small)
        └── hudbake.cache   (bottom-screen HUD, ~100 KB; optional, see step 6)
```

Copy the `3ds/` folder onto the **root** of your SD card, merging with the
existing `3ds/` homebrew folder. Then get the `G-Diffuser-3DS-*.3dsx` or
`.cia` that matches your console and content — see the table in the main
[README](../../README.md#download) (e.g. an Old 3DS with the Expansion Kit
disk wants the O3DS-EK build):

- **`.3dsx`**: copy it to `sdmc:/3ds/` and launch it from the Homebrew Launcher.
- **`.cia`**: install it with [FBI](https://github.com/Steveice10/FBI/releases)
  from the HOME menu (needs Luma3DS and sigpatches), then launch it from there.

First time doing either? See `docs/3DS-HARDWARE.md` for the full walkthrough.

Saves are created by the game at `sdmc:/3ds/gdiffuser/saves/` — back that
folder up if you reformat the card. The ROM itself is **not** copied to the
SD card and is not needed on device.

(Porters: how the two archives mount and resolve collisions on device is
architecture-level detail, not a prebake concern — see `docs/ARCHITECTURE.md`.)

## Expansion Kit: this tool doesn't bake your disk image

If you're using an Expansion Kit build (`o3ds-ek` / `n3ds-ek` — whether you
downloaded it or built it yourself), this script's job doesn't change, and
that's the point: you still only need it for the ROM above. The 64DD disk
image is handled completely differently from the ROM: the console **streams
it directly from `sdmc:/3ds/gdiffuser/media/`** at runtime, reading it in
small blocks as needed rather than loading it whole. There is no pre-bake,
archive, or golden check for it. Just copy your own disk dump there yourself
— see `docs/3DS-HARDWARE.md` §4 ("SD card layout") for the exact filename;
this tool never touches it.

## Not yet covered

- **JP ROM profile**: experimental on desktop, no validated golden; excluded
  here until one exists. This is separate from Japanese *disk* content —
  the only retail Expansion Kit disk is Japanese, and 3DS builds accept it
  by default (see `docs/BUILDING.md`, `GDX_ALLOW_JP_INPUTS`).
