# tools/prebake — PC-side asset pre-bake for the 3DS port

The Nintendo 3DS build never runs Torch, Python, or first-boot extraction on
device (no CPU/RAM budget for it, and no portlibs). Instead, you bake the
asset archives **once on your PC** with this tool and copy them to the SD
card. The 3DS then mounts them read-only from
`sdmc:/3ds/gdiffuser/` (see `port/3ds/assets/gdx3ds_fs_sd.c`).

This tool does not reimplement extraction: it drives the **existing desktop
flow** — the deterministic `gdx-extract` (trimmed Torch) that the desktop
port's first boot spawns — with the exact same arguments, then applies the
same validation gates first boot applies (ROM SHA-1 in, archive golden
SHA-256/entry-count out).

## What you need

1. **This repository**, with submodules:
   `git submodule update --init --recursive`
2. **A desktop build that produced `gdx-extract`** (any platform the desktop
   port builds on):

   ```
   cmake -S . -B build
   cmake --build build --target gdx-extract
   ```

   Only the `gdx-extract` target is required — you do not need to build the
   full game to pre-bake. (If you already have a full desktop build, that
   works too.)
3. **Your F-Zero X US rev0 cartridge dump**, big-endian (`.z64`).
   - SHA-1 must be `5f658e88ffa9de23cba6986a8fd3d3a90d7b4340`.
   - JP, PAL, rev1, and byte-swapped (`.v64`/`.n64`) dumps are rejected with
     a diagnosis; byte-swapped dumps can be converted to `.z64` first
     (tool64, or `dd conv=swab`), other regions/revisions cannot be used.
4. Python 3.8+ (standard library only).

## Run it

```
python3 tools/prebake/prebake.py --rom /path/to/fzerox-us-rev0.z64 --build-dir build
```

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

**Why some entries are stored.** The 3DS preloads `audio_blob/audio_table`
(10.7 MB, deflate saves only 9%) and `segment_blob/common_assets_compressed`
(2.5 MB of MIO0 data, 26%) at every boot; inflating them is CPU-bound (~4 s
on a New 3DS spare core, longer on the syscore) and the title music's sample
load can block on it, which is audible as a boot hiccup
(`docs/research/bootaudio2-progress.md`). Stored, the preload is a plain SD
read. The archive grows ~1.65 MB (15.5 -> 17.2 MB). The golden gate applies
to the extractor's output before this step; the installed archive keeps the
same entry order, names, sizes and CRCs, only the two entries' method changes.

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

## Copy to the SD card

The staging dir mirrors the SD root:

```
dist/sdmc/
└── 3ds/
    └── gdiffuser/
        ├── fzerox.o2r      (~game archive, tens of MB)
        └── gdiffuser.o2r   (engine archive, small)
```

Copy the `3ds/` folder onto the **root** of your SD card, merging with the
existing `3ds/` homebrew folder. Then install/launch `G-Diffuser-3DS.3dsx`
via the Homebrew Launcher as usual.

Saves are created by the game at `sdmc:/3ds/gdiffuser/saves/` — back that
folder up if you reformat the card. The ROM itself is **not** copied to the
SD card and is not needed on device.

## Mount semantics on device (for porters)

`gdx3ds_fs_sd.c` mirrors the desktop mount behavior
(`port/main.cpp findArchivePaths` + libultraship `ArchiveManager`):
archives mount in the order `gdiffuser.o2r`, `fzerox.o2r`, and the **last
mounted archive wins** duplicate record keys (ArchiveManager overwrites its
hash→archive map per file), so `fzerox.o2r` shadows `gdiffuser.o2r` on
collisions — identical to the PC.

## Not yet covered (post-MVP)

- **Expansion Kit / 64DD (`.ndd`, `n64ddipl.o2r`, `fzerox-disk.o2r`)**: the
  3DS MVP is cartridge content only (`docs/research/3ds-port-plan.md`,
  stream D). The prebake tool will grow `--ndd` / IPL steps when the device
  side mounts them.
- **JP profile**: experimental on desktop, no validated golden; excluded
  here until one exists.
