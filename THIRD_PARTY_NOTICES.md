# Third-party notices

G-Diffuser combines original port code with independently licensed decompilation, engine, tooling,
font, and support components. The corresponding license texts are distributed in `LICENSES/` and
must remain with binary packages.

## Core components

| Component | Use | License file |
|---|---|---|
| F-Zero X decompilation | Recreated game code and extraction recipes | `LICENSES/CC0-1.0.txt` |
| F-Zero X Expansion Kit decompilation | Source/reference submodule | `LICENSES/CC0-1.0.txt` |
| libultraship | Runtime, platform abstraction, and resource system | `LICENSES/MIT-libultraship.txt` |
| libultraship (vendored 3DS carve) | The 36 sources + 83 headers the 3DS build compiles, patched and vendored at `port/3ds/vendor/lus/` — same MIT terms, verbatim copy beside the code and provenance in `port/3ds/vendor/lus/PROVENANCE.md` | `LICENSES/MIT-libultraship.txt` |
| Fast3D | Graphics interpreter and shaders | `LICENSES/MIT-Fast3D.txt` |
| Torch | `gdx-extract` asset processor | `LICENSES/MIT-Torch.txt` |
| cxd4 RSP core | Low-level audio RSP interpreter | `LICENSES/CC0-1.0.txt` |
| SSE2NEON | ARM SIMD compatibility header used by cxd4 | `LICENSES/MIT-SSE2NEON.txt` |
| discord-rpc | Discord Rich Presence (optional, off by default) | `LICENSES/MIT-discord-rpc.txt` |
| RapidJSON | JSON serialization inside discord-rpc | `LICENSES/MIT-rapidjson.txt` |
| QR Code generator (Project Nayuki) | Expansion Kit 3DS build only: draws `[ek] course_share` QR codes; vendored at `port/3ds/vendor/qrcodegen/` | `LICENSES/MIT-qrcodegen.txt` |
| quirc | QR decoder; host unit test only today (reads back the course-share QR frames); vendored at `port/3ds/vendor/quirc/` | `LICENSES/ISC-quirc.txt` |

## Runtime and extraction dependencies

The package also includes or statically links Dear ImGui, prism, BS::thread_pool, Monocypher, stb,
libgfxd, yaml-cpp, spdlog, tinyxml2, SDL2, fmt, libzip, zlib, bzip2, GLEW, nlohmann/json, and
miniz-cpp/miniz. Their complete notices are the correspondingly named files under `LICENSES/`.

Montserrat and Inconsolata are distributed under the SIL Open Font License 1.1. Their license texts
are included both under `LICENSES/` and beside the fonts in release packages.

## Asset boundary

- G-Diffuser distributions do not include a Nintendo ROM, disk image, or IPL image, or game
  assets (textures, models, audio, or other game payloads). The binaries do contain code compiled
  from the community decompilations listed above, including the N64 SDK library code those
  decompilations carry. Users must dump their own cartridge (and, for the Expansion Kit, their own
  64DD disk).
- The `fzerox-expansion-kit` submodule is pinned to a copy of the upstream decompilation's history
  with an accidentally committed retail disk image (`baserom.jp.disk`) purged from every commit;
  see `.gitmodules`.
- Binary packages (release zips) carry `LICENSE`, this file, and `LICENSES/`.
- The `decomp-recipes/` folder that desktop builds place beside the executable is not tracked in
  this repository: the build copies it from the `decomp` submodule. It contains CC0-licensed
  extraction metadata and no embedded game payload.
- `fzerox.o2r` is generated locally from files supplied by the user and must not be redistributed
  with G-Diffuser.
- RSP microcode (aspMain) is not shipped in the source tree or in binaries; the low-level audio
  path reads it from `fzerox.o2r`, where extraction places it from the user's own ROM.
- `gdiffuser.o2r` contains only the MIT-licensed Fast3D shaders required to initialize the renderer.
- The Input Viewer is drawn with ImGui primitives. No Ship of Harkinian controller PNGs are included.
- No game-derived application icon is included. Builds use the platform default until original,
  separately cleared artwork is provided.

F-Zero and Nintendo are trademarks of Nintendo. Their names are used only to identify compatibility;
no affiliation, endorsement, or sponsorship is claimed.
