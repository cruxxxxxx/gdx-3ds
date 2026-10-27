# port/3ds/vendor/qrcodegen — provenance

Vendored copy of Project Nayuki's QR Code generator library (C version), used by the Expansion
Kit build's `[ek] course_share` to show a course as QR codes on the bottom screen
(`port/3ds/gdx3ds_course_share_qr.c`). Compiled into EK builds only; the host unit test
(`port/3ds/test/gdx3ds_course_share_host_tests.c`) compiles it too.

## Upstream

| | |
|---|---|
| Repository | `https://github.com/nayuki/QR-Code-generator.git` |
| Commit | `3c6d0b3cefb4e049dc337e82237c9644399716a8` |
| Files | `c/qrcodegen.c`, `c/qrcodegen.h` |
| License | MIT — the notice from the source header, verbatim, in `LICENSE` |

## Local changes

None. Both files are byte-identical to upstream at that commit.

| SHA-256 | File |
|---|---|
| `6a2b9cc65176f2345dde260c74b6d352627e8a0a6385d086ae0e9c5d0913c70c` | `qrcodegen.c` |
| `e82df4bff37d18b5863b9e7486fe6bda1b6cda8c3b9ecebfec473907265cb589` | `qrcodegen.h` |
