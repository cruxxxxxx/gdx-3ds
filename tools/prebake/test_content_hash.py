#!/usr/bin/env python3
"""test_content_hash.py -- the multiplayer content hash must not depend on the PC's zlib.

Rooms compare a hash of each .o2r (port/gdx_mp_online.c gdx_mpo_zip_cd_hash). prebake.py
re-deflates fzerox.o2r with the host Python's zlib, so two PCs prebaking the same ROM can write
different compressed bytes (zlib-ng, another zlib version or level, --no-store). This test builds
a synthetic archive, re-packs it through prebake.store_large_entries at several compression
levels (and as-is, like --no-store), checks that the packing really differs, and checks that the
console's hash (run through port/tests/gdx_mpo_zip_hash_cli.c) is the same for all of them, and
different once a member's content changes.

Usage: test_content_hash.py <path to the built gdx_mpo_zip_hash_cli>
Python 3.8+, standard library only. Exit 0 = PASS.
"""

import os
import random
import subprocess
import sys
import tempfile
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import prebake  # noqa: E402

STORED = ("audio_blob/audio_table", "segment_blob/common_assets_compressed")


def make_source(path, tweak=False):
    """An extractor-like archive: every member deflated, a few large, some incompressible."""
    rng = random.Random(1234)
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        for i in range(40):
            words = [b"track", b"machine", b"course", b"%d" % rng.randrange(1000)]
            data = b" ".join(rng.choice(words) for _ in range(200 + i * 37))
            if tweak and i == 17:
                data = data[:-1] + b"!"
            z.writestr("asset_%02d/blob" % i, data)
        z.writestr(STORED[0], bytes(rng.randrange(256) for _ in range(60000)))
        z.writestr(STORED[1], b"MIO0" + bytes(rng.randrange(64) for _ in range(30000)))


def console_hashes(cli, paths):
    out = subprocess.run([cli] + paths, check=True, stdout=subprocess.PIPE, universal_newlines=True).stdout
    return [line.split()[0] for line in out.splitlines()]


def packing(path):
    with zipfile.ZipFile(path) as z:
        return [(i.filename, i.compress_type, i.compress_size) for i in z.infolist()]


def main():
    if len(sys.argv) != 2 or not os.access(sys.argv[1], os.X_OK):
        print("usage: test_content_hash.py <gdx_mpo_zip_hash_cli>")
        return 2
    cli = sys.argv[1]
    failures = 0
    prebake.info = lambda msg: None  # keep the test output to its own lines
    with tempfile.TemporaryDirectory(prefix="gdx-content-hash-") as tmp:
        src = os.path.join(tmp, "generic.o2r")
        make_source(src)
        variants = [src]  # --no-store: the extractor's archive as-is
        for level in (1, 6, 9):
            dst = os.path.join(tmp, "level%d.o2r" % level)
            prebake.store_large_entries(src, dst, STORED, compresslevel=level)
            variants.append(dst)
        packs = [packing(p) for p in variants]
        distinct = len(set(tuple(p) for p in packs))
        if distinct < 3:
            print("FAIL: the re-packs did not change the packing (%d distinct): the test proves nothing" % distinct)
            failures += 1
        raw = set()
        for p in variants:
            with open(p, "rb") as f:
                raw.add(f.read())
        if len(raw) != len(variants):
            print("FAIL: two re-packs are byte-identical; expected every level to differ")
            failures += 1
        hashes = console_hashes(cli, variants)
        for p, h in zip(variants, hashes):
            print("  %s %s" % (h, os.path.basename(p)))
        if len(set(hashes)) != 1:
            print("FAIL: the content hash depends on the compression: %s" % hashes)
            failures += 1
        tweaked = os.path.join(tmp, "tweaked.o2r")
        make_source(tweaked, tweak=True)
        if console_hashes(cli, [tweaked])[0] == hashes[0]:
            print("FAIL: a changed member did not change the content hash")
            failures += 1
    if failures:
        print("test_content_hash: FAIL (%d)" % failures)
        return 1
    print("test_content_hash: PASS (%d packings, one content hash)" % len(variants))
    return 0


if __name__ == "__main__":
    sys.exit(main())
