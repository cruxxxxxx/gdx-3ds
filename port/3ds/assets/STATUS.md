# Stream D — assets (SD O2R + saves + PC pre-bake)

- Done:
  - **`gdx3ds_fs.h` implemented** (`gdx3ds_fs_sd.c`, device builds; the Phase 0
    stub stays for host builds). O2R read-on-demand over vendored **miniz 3.0.2**
    (`third_party/miniz/`, MIT) — `mz_zip_reader_init_file` keeps a `FILE*` and
    reads only the central directory up front; records inflate individually
    into `malloc`'d buffers (`GDiffuser_LoadArchiveFileBytes` semantics: caller
    frees, `NULL` + `*outSize=0` on miss). The archive is never buffered whole.
  - **Mount ordering mirrors desktop**: `gdiffuser.o2r` then `fzerox.o2r`
    (`port/main.cpp findArchivePaths` order); lookups probe in reverse because
    libultraship `ArchiveManager::AddArchiveUnlocked` overwrites
    `mFileToArchive[hash]` per file — **last mounted wins**, so `fzerox.o2r`
    shadows `gdiffuser.o2r` on duplicate keys. Lookups are case-sensitive
    (libzip `zip_name_locate(a,n,0)` parity); zero-size records are a miss
    (`O2rArchive::LoadFile` parity).
  - **Saves**: whole-blob read/write under `sdmc:/3ds/gdiffuser/saves/`,
    stage-to-`.tmp` + rename like `port/sram_buffer.cpp`, with an
    unlink+rename fallback for FAT renames that refuse to replace. Save names
    are confined to bare filenames (traversal rejected).
  - **`gdx3ds_zipshim`** (BONUS, for stream A): the libzip surface from
    `port/3ds/lus_stubs/zip.h` implemented over miniz as a separate static lib.
    Read side complete (open/locate/stat/fopen_index/fread/close, streaming
    inflate via miniz's extract iterator — partial reads never inflate the
    whole record). Write side read-only-degraded: `zip_file_add` returns -1 and
    `O2rArchive::WriteFile` fails cleanly; `ZIP_CREATE` on an existing archive
    opens it for reading (what `O2rArchive::Open` does), on a missing file
    yields an empty handle. **Stream A: link `gdx3ds_zipshim` and O2rArchive
    compiles/links on 3DS without libzip**; the target publishes the
    `lus_stubs` include dir so `#include <zip.h>` resolves to the surface the
    shim implements.
  - **Host smoke test** `gdx3ds_fs_host_smoke` (host builds only): fabricates
    the SD layout + fixture archives in a scratch CWD and functionally checks
    duplicate-key resolution, case sensitivity, read/save semantics, atomic
    rename, and the whole zipshim read surface. 27/27 passing.
  - **`tools/prebake/`**: `prebake.py` + README — drives the existing desktop
    `gdx-extract` against a user ROM, mirrors the desktop validation gates
    (ROM SHA-1 from `decomp/config.yml`, clear JP/PAL/rev1/byte-swap errors;
    archive golden SHA-256 + 3610-record gate from `port/gen/gdx_o2r_expected.h`),
    renames `generic.o2r` → `fzerox.o2r`, generates `gdiffuser.o2r` via
    `tools/gen_f3d_o2r.py`, and stages the `sdmc:/3ds/gdiffuser/` layout.
  - Both gates green: `build-3ds` (devkitARM `.3dsx`) and `build-host`
    (`-DGDX_PLATFORM_3DS=ON`), which also compile-checks `gdx3ds_fs_sd.c` on
    host via `gdx3ds_assets_sd_checkbuild`.

## Segment-8 decode budget (worst case 133.95 ms on PC)

`course_track_gfx` (segment 8) is the largest single-record inflate; 133.95 ms
on a desktop CPU will be a multiple of that on the 804 MHz ARM11. Plan:

1. **Measure, not guess**: add a `-DGDX3DS_FS_PROFILE` compile option to
   `gdx3ds_fs_sd.c` that wraps `mz_zip_reader_extract_to_mem` with
   `svcGetSystemTick` deltas and logs `record, comp_size, uncomp_size, ms` to
   `sdmc:/3ds/gdiffuser/fs_profile.csv`. Run the mount + a scripted read of the
   25 `segment_blob` records on (a) Citra (indicative only — JIT'd timing) and
   (b) real New3DS hardware (authoritative). miniz's inflate is within ~2x of
   zlib's on ARM, so a first-order estimate before hardware: PC 133.95 ms x
   (PC ~4 GHz effective IPC / New3DS ARM11) ≈ **0.8–1.5 s** worst case — track
   loads only, never mid-race.
2. **Prewarm fallback (if measurement confirms >1 frame budget, which it
   will)**: segment blobs are loaded at course-load, so route them through the
   existing load screen — issue the `gdx3ds_fs_read_asset` for segment 8 (and
   the course's other segment blobs) at load-screen entry on the loader
   thread, render the load screen from the main thread, and hand the buffer to
   the game when inflate completes. No API change: callers already treat the
   returned buffer as owned memory.
3. **Escape hatch (decision point owned by this stream)**: if SD+inflate
   throughput is still unacceptable on hardware, switch the pre-bake to emit
   segment blobs **stored** (compression level 0) — `tools/prebake` owns the
   archive bytes, `mz_zip_reader_extract_to_mem` memcpys stored entries, and
   the contract header does not change. Costs ~2-3x SD space for those 25
   records only. (Full flat-file sm64-style layout is the further fallback,
   also contract-invisible.)

## Notes / caveats

- miniz on devkitARM uses the plain `ftello/fseeko` stdio path (no LFS) —
  fine, archives are far below 2 GB.
- `gdx3ds_fs_sd.c` assumes single-threaded callers (LUS loader thread). If
  integration adds concurrency, wrap the `mz_zip_reader_*` calls in a
  `LightLock` (miniz readers are not synchronized; desktop libzip got this via
  O2rArchive's per-call handle pool).
- Prebake end-to-end (real ROM → golden-matching `fzerox.o2r`) is untested on
  this machine (no ROM, no desktop build present); the ROM-rejection paths,
  golden-header parsing, and `gdiffuser.o2r` generation are tested. First
  ROM-in-hand run should confirm the golden gate passes.

- Blocked on: —
- Next: profile inflate throughput on Citra/hardware (plan above); wire
  `gdx3ds_fs_init` into the integration boot path (stream B's main once LUS
  lands); EK/64DD (`.ndd` streaming, `n64ddipl.o2r`) post-MVP.
