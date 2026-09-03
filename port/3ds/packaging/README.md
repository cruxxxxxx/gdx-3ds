# port/3ds/packaging — SMDH icon, HOME-menu banner, CIA spec

Inputs for the `.3dsx` metadata (always built) and the optional installable `.cia`
(target `G-Diffuser-3DS-cia`, gated on makerom + bannertool being findable).

## Files

| File | What | Source |
|---|---|---|
| `icon.png` | 48x48 SMDH icon (HBL list + HOME menu) | `assets/branding/gdiffuser-icon.png` downscaled (ImageMagick) |
| `banner.png` | 256x128 HOME-menu banner image | `assets/branding/gdiffuser-logo-menu.png` centered on `#101018` |
| `banner.wav` | banner audio: 0.25 s of silence (deliberately quiet) | generated (Python `wave`, 22050 Hz mono s16) |
| `gdiffuser.rsf` | makerom spec; see header comment for the choices | Steveice10 buildtools `template.rsf`, inlined |

Licensing: `icon.png`/`banner.png` are derivatives of this repository's own branding
art (`assets/branding/`), same license as the repo; nothing Nintendo-derived is in
this directory.

## SMDH metadata (baked into the .3dsx)

- Short title: `G-Diffuser`
- Long description: `G-Diffuser — F-Zero X decompilation port`
- Publisher: `G-Diffuser Project`

Wired in `port/3ds/CMakeLists.txt` via devkitPro's `ctr_generate_smdh`/`ctr_create_3dsx`
(smdhtool + 3dsxtool, both ship with 3ds-tools).

## CIA tools (local, never committed)

`makerom` and `bannertool` are not part of devkitPro's pacman set. Drop them in
`tools/3ds-bin/` (gitignored) or anywhere on `PATH`; CMake finds either location.

- **makerom** (Project_CTR, v0.19.0 tested): grab the platform binary from
  <https://github.com/3DSGuy/Project_CTR/releases> (macOS arm64/x86_64, Linux,
  Windows builds provided) and `chmod +x`.
- **bannertool** (v1.2.3, from the maintained carstene1ns fork — the original
  Steveice10 repo is gone): <https://github.com/carstene1ns/3ds-bannertool>.
  Linux/Windows release binaries exist; on macOS build from source
  (`cmake -S . -B build && cmake --build build`). Note: clang rejects a
  zero-initialized VLA in `source/3ds/lz11.cpp:115`; change `u8 pad[padLength]`
  to `u8 pad[4]` (its maximum) to build.

Then:

```sh
cmake --build build-3ds --target G-Diffuser-3DS-cia
```

produces `build-3ds/port/3ds/G-Diffuser-3DS.cia`. Install with FBI on hardware
(Luma3DS) or `File > Install CIA` / `-i` in Azahar. The title installs as
**000400000FF3D500** ("G-Diffuser").
