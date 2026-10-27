# port/3ds/vendor/quirc — provenance

Vendored copy of quirc, a QR-code recognition library. It is compiled into the Expansion Kit
build, where the CRS tab's SCAN view runs it on the outer camera's frames
(`port/3ds/gdx3ds_course_share_scan.c`, `gdx3ds_course_share_cam.c`), and into the host unit
test `port/3ds/test/gdx3ds_course_share_host_tests.c`, which decodes every `[ek] course_share`
QR frame rendered exactly as the console paints it and synthetic camera pictures of them
(scaled, rotated, mirrored, keystoned, dimmed, blurred, noised). Non-EK builds do not compile it.

## Upstream

| | |
|---|---|
| Repository | `https://github.com/dlbeer/quirc.git` |
| Commit | `927d680904dc95fdff4cd9d022eb374b438ff8f2` |
| Files | `lib/decode.c`, `lib/identify.c`, `lib/quirc.c`, `lib/version_db.c`, `lib/quirc.h`, `lib/quirc_internal.h`, `LICENSE` |
| License | ISC — verbatim in `LICENSE` |

## Local changes

Three, all marked `gdx-3ds local change` in the source: two in `identify.c` (below) and a
guard in `quirc.c`'s `quirc_resize` that skips the `memcpy` when there is nothing to copy (the
first resize passes a NULL source, which UBSan reports). Every other file is
byte-identical to upstream at that commit (`lib/` flattened into this directory; the sources
include each other by bare name, so no edit was needed). Upstream `identify.c` SHA-256 was
`ae858d86adcb12db80ad01f6d941cc2247fb5970abf0754f24dca027ede2ba99`.

1. `test_grouping_pixels`, a fallback for `test_grouping`, run only when `test_grouping` found
   no grid at all. `test_grouping` judges whether two capstones line up with a third through
   the perspective fitted to that one capstone's four corners. At 3 camera pixels a module a
   capstone is ~21 px across and its corners are good to +-1 px, so for a version-10 code (the
   neighbours 50 modules away) the fitted perspective is skewed enough that three clean
   capstones fail its squareness test and no grid is tried. The fallback looks for the same
   right isosceles triangle with plain vectors between capstone centres (arms within 25% of
   each other, |cos| <= 0.3 between them, each arm at least 2 capstone sides) and hands it to
   the same `record_qr_grid`, which orders the corners and fits the grid as usual; a wrong
   triangle fails the format/ECC checks downstream like any other misread. Measured with the
   host test's synthetic camera on a 57-module page: upright-to-90-degree reads at 2.0-3.0 px
   a module went from about half to nearly all.
2. `perspective_map` maps a non-finite result (a degenerate fit) to (-1, -1), off-image, where
   every caller already rejects it, instead of converting inf/NaN to int (undefined; UBSan
   reported it under the host test).

| SHA-256 | File |
|---|---|
| `d4468c55ecd0d2f905a6813513708005e6d609ef0a3d32a17673313c7552a7c1` | `decode.c` |
| `b042ab2c9fa2dd3b0ba932eed04389b676094c5b3a614e713fb6dc4d2026910b` | `identify.c` |
| `0294b6c56f8c021b256c4c153d70483368164c6cf0cce643e1b6be03ed3585c0` | `quirc.c` |
| `6764aa2f245085080e1e5cefd9dcd59b9727718a0d4606956e0502a57f5dff30` | `version_db.c` |
| `49660ea710add2d6f304a1323f53190f5a2bf34db4dd160d633db0c3f22bfba5` | `quirc.h` |
| `e383ed1a0ca70c07b0530d76bfeb6bd5525efda182589f48c978921a9e54676b` | `quirc_internal.h` |
| `a70ef3ea032998eead2e2c7573a170a809eae08e3ca134611f707eda5932c8a9` | `LICENSE` |
