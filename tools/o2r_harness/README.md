# O2R Extraction Harness

Deterministic-extraction validation gauntlet for `generic.o2r` (P0 contracts
C3/C4/C5/C6, see `docs/investigation/2026-07-18/o2r-migration/P0_CONTRACTS.md`).

Python 3, standard library only. All scripts import `o2r_common.py`, so run them
from inside `tools/o2r_harness/` (or with this directory on `sys.path`).

## What each script does

| Script | Purpose | Fails (exit 1) when |
|---|---|---|
| `verify_determinism.py` | Runs `gdx-extract` **twice** into separate temp dirs, byte-compares the two archives (C2). | The two runs differ, or the extractor exits non-zero. |
| `validate_archive.py` | C5 checks 2-4 + the C6 complete-or-absent family check, without booting the game. | Entry count wrong, SHA-256 mismatch, bad `version` entry, or any family present-but-partial / misnamed. |
| `gen_expected_header.py` | Emits `port/gen/gdx_o2r_expected.h` (`GDX_O2R_EXPECTED_SHA256` + `GDX_O2R_EXPECTED_ENTRY_COUNT`) from a validated archive; prints the embedded ROM CRC. | Entry count != `o2r_common.EXPECTED_ENTRY_COUNT` (currently 3610) or `version` entry fails C4 (refuses to write unless `--force`). |
| `compare_archives.py` | Entry-level diff of two `.o2r` files; reports container-level vs payload-level equality separately. | Payloads differ (key set, record counts, or bytes). Container-only differences do **not** fail. |
| `gen_family_manifest.py` | (Re)generates `family_manifest.json` from a reference archive. Run once; committed. | Archive missing. |

### Key facts baked into the harness

- **Entry count is central-directory RECORDS (dup-inclusive), not unique names.**
  This is exactly what the zipEntryCount EOCD comparison in
  `gdx_extract_launch.cpp` compares — it reads EOCD offset 10 ("total
  central-directory records"). All
  counting goes through `o2r_common.read_records` / `record_count`
  (`len(z.infolist())`), never a name-keyed dict, so it always matches the gate.
  The current deterministic archive has **no duplicate records** (records ==
  unique names == `o2r_common.EXPECTED_ENTRY_COUNT`, currently **3610**: 3576
  pre-R1 base + 22 R1 `segment_blob` families + 3 R2 `audio_blob` families + 3 R4
  `segment_blob` families (`common_assets_compressed`, `kanji_tables`,
  `rom_boot_tuning` -- W-R4.S1 census; `segment_blob` family count is now 25), plus 4 create_machine_textures
  records added on 2026-07-25 by the AssetBindings.c re-slice, plus 2 `rsp_blob`
  records added on 2026-08-08 (aspMain microcode moved out of the source tree
  into the locally-generated archive) -- see the census
  comment on `o2r_common.EXPECTED_ENTRY_COUNT` for why those re-blesses were safe).
  The historical 4,240-records / 664-duplicates / 3,576-unique figures described
  the pre-2026-07-18 archive (Windows double-emit bug) and no longer apply after
  the Torch parity fix.
- **`-u <version>` is REQUIRED on every `gdx-extract` invocation.** Torch only
  emits the `portVersion` record when a version string is passed. The runtime
  passes `-u 2027490995` (decimal of the US-rev0 ROM CRC 0x78D90EB3). Omitting it
  drops `portVersion`, yielding an archive one record short whose SHA-256 will not
  match the runtime archive nor the golden header.
- **`version` entry (C4)** = 5 bytes `01 78 D9 0E B3` = `[endianness=0x01 big]`
  `[u32 big-endian ROM CRC = 0x78D90EB3]`. Validated by
  `o2r_common.parse_version_entry`.
- **Golden SHA-256** must be computed from the archive produced by the
  **deterministic** `gdx-extract`, never from the legacy build-time archive
  (which predates the determinism patches and has an unstable hash).

## The post-build gauntlet (orchestrator runs, in order)

After agent A's `gdx-extract` target builds, run from `tools/o2r_harness/`:

```
# 1. Prove determinism (precondition for a stable golden SHA-256).
#    The `-u 2027490995` MUST be forwarded (see Key facts): it is the US-rev0 ROM
#    CRC that makes Torch emit the portVersion record the runtime archive carries.
python verify_determinism.py \
    --extractor <path/to/gdx-extract> \
    --rom <path/to/rom.z64> \
    --recipes <path/to/decomp-recipes> \
    --extra-arg=-u --extra-arg=2027490995
#    PASS => prints the golden SHA-256. FAIL => names the first differing entry
#    (central-directory) or byte offset (container-level, e.g. a stray timestamp).

# 2. Produce ONE archive to bless (any datadir; then validate that exact file).
<path/to/gdx-extract> o2r <rom.z64> -s <decomp-recipes> -d <out-dir> -u 2027490995
#    => <out-dir>/generic.o2r

# 3. Validate structure + family completeness (no golden hash yet -> --skip-hash).
python validate_archive.py --archive <out-dir>/generic.o2r --skip-hash
#    Must PASS checks 2, 4, 5. (Check 3 is skipped until the header exists.)

# 4. Mint the golden header from the blessed archive.
python gen_expected_header.py --archive <out-dir>/generic.o2r
#    Writes ../../port/gen/gdx_o2r_expected.h and prints the ROM CRC.
#    Refuses if entry count != o2r_common.EXPECTED_ENTRY_COUNT (3610) or version entry is wrong.

# 5. Re-validate WITH the golden hash to close the loop (all four checks).
python validate_archive.py \
    --archive <out-dir>/generic.o2r \
    --expected-header ../../port/gen/gdx_o2r_expected.h
#    Must PASS all of 2, 3, 4, 5.

# 6. (Optional) Diff the deterministic output vs the legacy archive.
python compare_archives.py ../../assets/extracted/generic.o2r <out-dir>/generic.o2r
#    Expected: payload identical, container differs (zip metadata/order/level).
#    A payload difference here means the deterministic recipe output changed —
#    investigate before blessing.
```

`gdx-extract` invocation shape and the `--extra-arg`/`--out-name` flags mirror
C2. Never pass `-v` to the extractor (it dumps entries to CWD). Pass `-d`
explicitly; never rely on inherited CWD.

## Failure-mode guide

- **verify_determinism FAIL, central directory diff** — the recipe/entry order
  is nondeterministic (C2 change #2 `getRecursiveEntries` sort missing) or an
  entry's payload varies run-to-run.
- **verify_determinism FAIL, raw-byte offset** — container-level nondeterminism:
  a wall-clock zip timestamp (C2 change #1 `MINIZ_NO_TIME`) or an unpinned
  compression level (C2 change #3).
- **validate check 2 (entry_count)** — extractor dropped/added entries; count is
  record-based, so a missing duplicate also trips this.
- **validate check 4 (version_entry)** — wrong ROM was extracted (CRC mismatch)
  or Torch's version scheme changed. The printed CRC tells you which ROM.
- **validate check 5 (family complete-or-absent)** — a Path-B texture family is
  half-populated (silent blank-texture bug with no raw fallback, W0 section 3) or
  a key was misnamed (surfaces as an `unknown` family). This is the check the
  runtime cannot do safely.
- **gen_expected_header refuses** — do not `--force` past it in CI; a wrong count
  or version means the archive is not the golden.
- **compare payload differs** — the deterministic output diverged from the
  legacy payload set; the recipe output genuinely changed.

## Regenerating the family manifest

`family_manifest.json` encodes the per-family record counts of the C3 full-recipe
output (33 families, 3,610 records — includes the R1 `segment_blob` family (25
entries after R4), the R2 `audio_blob` family, the 3 R4 `segment_blob`
additions, and the `rsp_blob` family (2 microcode records, 2026-08-08)).
Regenerate only when the recipe set intentionally changes:

```
python gen_family_manifest.py --archive <reference generic.o2r>
```
