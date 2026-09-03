# gdx3ds-dl-tests — F3DEX2 display-list replay tests for the citro3d backend

Standalone `.3dsx` that feeds hand-built F3DEX2 display lists through
libultraship's Fast3D interpreter into stream A's citro3d backend
(`port/3ds/gfx/`). One synthetic scene per TODO(citra-verify) item in
`port/3ds/gfx/STATUS.md`; per-scene ground truth lives in
[EXPECTED.md](EXPECTED.md). This is the standalone flavour of the plan §2.6 DL
replay rig — it needs no game assets, no SD card contents, and none of the
other Phase 1 streams at runtime.

## Building

Same invocation as the main port (from the repo root):

```sh
export DEVKITPRO=/opt/devkitpro
cmake -S . -B build-3ds -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/3DS.cmake -DGDX_PLATFORM_3DS=ON
cmake --build build-3ds -j8
```

Output: `build-3ds/port/3ds/harness/gdx3ds-dl-tests.3dsx` (alongside the main
`build-3ds/port/3ds/G-Diffuser-3DS.3dsx`).

Prereq (shared with `port/3ds/gfx/`): the LUS newlib patch must be applied to
the `libultraship/` submodule working tree:

```sh
git -C libultraship apply ../port/3ds/patches/lus-newlib-portability.patch
```

## Running

### Azahar (emulator)

Open the `.3dsx` directly — no CIA install needed:

```sh
# macOS app bundle:
/Applications/Azahar.app/Contents/MacOS/azahar \
    "$(pwd)/build-3ds/port/3ds/harness/gdx3ds-dl-tests.3dsx"
# or equivalently:
open -a Azahar build-3ds/port/3ds/harness/gdx3ds-dl-tests.3dsx
```

The test scenes render on the top screen; the bottom screen is a text console
showing the current scene's name and expected result. Keep Azahar's log
visible: the backend prints `[gfx_citro3d] ...` diagnostics (including the
`unmapped combiner` lines scene 6 asserts must NOT appear) to stderr.

Note Azahar is still an emulator: PICA depth/TexEnv corner cases can differ
from hardware (plan §6 tripwire). Anything that looks wrong here should be
reproduced on a real New3DS before changing backend signs.

### Real hardware (Homebrew Launcher)

1. Copy `gdx3ds-dl-tests.3dsx` to `sdmc:/3ds/` on the console's SD card.
2. Launch it from the Homebrew Launcher.

### Controls

| Input  | Action |
|--------|--------|
| START  | next scene (wraps around) |
| SELECT | quit |

### Stereo runs (stream S foundation)

The harness links no config library, so stereo is opted into by a marker file:
create `sdmc:/gdx-harness/stereo.on` (host-side: `gdx-harness/stereo.on` in
Azahar's virtual SD) before launch, and give the emulated 3D slider a nonzero
value (Azahar `qt-config.ini` → `factor_3d=100`). Each scene then dumps a
right-eye readback `sceneNN_r.bmp` next to the usual left-eye `sceneNN.bmp`
(all scenes are ortho → zero-parallax class → the eyes must match; see
`port/3ds/gfx/STEREO.md`).

## Scenes

| # | Name    | Verifies (STATUS.md TODO item) |
|---|---------|--------------------------------|
| 1 | STRIP   | vertex order/winding + viewport origin |
| 2 | ROTATE  | fixup-matrix rotation sign |
| 3 | TEXTURE | texture row order + UV orientation |
| 4 | SCISSOR | viewport/scissor corner origin |
| 5 | DECAL   | decal depth-offset magnitude/sign |
| 6 | COMBINE | REPLACE/MODULATE/INTERPOLATE/MULTIPLY_ADD TexEnv mapping |

## Architecture (for maintainers)

```
dl_tests_main.cpp   app entry: libctru init, bottom-screen console,
                    Fast::Interpreter setup (GfxSetInstance + GfxDebugger +
                    null GfxWindowBackend), StartFrame/Run/EndFrame loop
scenes.{h,cpp}      the test scenes: vertices, matrices, the 32x32 arrow
                    texture, and per-scene DL builders (decomp PR/gbi.h g*()
                    writer macros)
link_stubs.cpp      link stubs for the non-libzip families from the STATUS.md
                    unresolved-symbol audit (Ship::Context, ConsoleVariable,
                    CVar C API, gdx_dbg/workshop helpers)
```

Key constraints, learned the hard way — keep them when extending:

- **Two gbi headers, never in one TU.** `scenes.cpp` uses the decomp's
  `PR/gbi.h` (F3DEX2 encodings, 8-byte non-`PORT` packets that match
  `Fast::F3DGfx` on ARM32); `dl_tests_main.cpp` uses LUS headers. Their `G_*`
  macros collide, so DLs cross `scenes.h` as `void*` and
  `kSceneGfxPacketSize` is checked at runtime.
- **Texture data must live in `linearAlloc` memory.** The interpreter's
  `G_SETTIMG` handler drops image pointers `<= 0x0FFFFFFF` as unresolved N64
  segment addresses, and the regular 3DS heap sits at `0x08000000+`.
  Vertices/matrices/DLs are exempt (no such filter) and use `malloc`.
- **The interpreter runs at 320x240** with `mGameWindowViewport` set to match,
  so `Run()` draws straight to framebuffer 0 (the screen) and DL coordinates
  map 1:1 — see EXPECTED.md's "dead band" note for the 400px-wide screen.

### Adding a scene

1. In `scenes.cpp`: add vertices/assets in `ScenesInit()`, write a
   `BuildYourScene(Gfx* g)` (start with `Prologue()` — full state reset), add
   a `case` in `SceneBuildDl()` and an entry in `kScenes[]`.
2. Bump `kSceneCount` in `scenes.h`.
3. Document the expected result (text + ASCII sketch + failure signatures) in
   EXPECTED.md.

Keep each scene single-purpose: one backend behaviour per scene, chosen so its
failure mode is unambiguous on sight.
