#!/usr/bin/env python3
"""Emit port/gen/gdx_disk_expected.h from a validated fzerox-disk.o2r (G-Diffuser R8 Step 1).

Produces the code-level disk golden constants consumed by port/gdx_extract_launch.cpp:

  #define GDX_DISK_EXPECTED_SHA256          "<hex>"   // disk/identity: SHA-256 of the stored image
  #define GDX_DISK_ARCHIVE_EXPECTED_SHA256  "<hex>"   // SHA-256 of the fzerox-disk.o2r container

Like the IPL golden (gen_ipl_expected.py), the disk golden is PER-USER self-consistency, not a single
canonical retail hash: different owners have different (equally valid) disk dumps. The runtime never
gates the mount on these constants — the archive carries its own disk/identity entry and absence falls
back to the R7 managed copy / raw .ndd. These constants exist so a DEV build can WARN on drift from the
owner's reference dump.

Note the disk image is stored VERBATIM (fmt=0). Unlike the cartridge ROM / IPL, the 64DD disk loader
performs no byte-order normalization, so the disk has a single canonical byte order and the identity is
computed over the raw bytes — which equals the R7 managed-copy sha (sidecar disk_sha256) the boot-time
deletion gate compares against.

The generator validates the archive shape (exactly the two frozen entries disk/image + disk/identity,
correct image size, well-formed identity) before writing, and refuses on any mismatch unless --force.

Usage:
  gen_disk_expected.py --archive fzerox-disk.o2r --out port/gen/gdx_disk_expected.h
  gen_disk_expected.py --placeholder --out port/gen/gdx_disk_expected.h   # owner-run-required stub
"""

import argparse
import os
import sys

import o2r_common as oc

# Frozen contract (R8 Step 1 + Step 2).
IMAGE_KEY = "disk/image"
IDENTITY_KEY = "disk/identity"
IMAGE_BYTES = 64931840  # retail/translated 64DD EK image size
IDENTITY_BYTES = 1 + 32  # [u8 fmt][SHA-256]
BASE_ENTRY_COUNT = 2  # disk/image + disk/identity; R8 Step 2 adds ek/<symbol> per-asset entries
EK_PREFIX = "ek/"

MACRO_DISK_SHA = "GDX_DISK_EXPECTED_SHA256"
MACRO_ARCHIVE_SHA = "GDX_DISK_ARCHIVE_EXPECTED_SHA256"
MACRO_ENTRY_COUNT = "GDX_DISK_EXPECTED_ENTRY_COUNT"

PLACEHOLDER = "0" * 64

HEADER_TEMPLATE = """\
/*
 * {basename}
 *
 * GENERATED FILE - do not edit by hand.
 * Generator : tools/o2r_harness/gen_disk_expected.py
 * Source     : {source_desc}
 * Disk identity (stored-image SHA-256)  : {disk_sha}
 * Archive SHA-256 (fzerox-disk.o2r)     : {archive_sha}
 * Archive entry count                   : {entry_count}
 * Byte-order fmt                        : {fmt_desc}
 *
 * PER-USER golden for the 64DD EK disk archive (R8 Step 1 + Step 2). These are self-consistency /
 * dev-drift constants ONLY: the runtime disk step never gates the mount on them (the archive
 * carries its own disk/identity entry, and absence falls back to the R7 managed copy / raw .ndd).
 * The entry count = 2 frozen disk/* entries + the ek/<symbol> per-asset entries (R8 Step 2); it is
 * data-driven (grows if the EK slice manifest gains rows), so the launcher reads it from here.
 * Regenerate with this build's owner disk dump after any change to the extractor's disk path.
 */
#ifndef GDX_DISK_EXPECTED_H
#define GDX_DISK_EXPECTED_H

#define {macro_disk_sha} "{disk_sha}"
#define {macro_archive_sha} "{archive_sha}"
#define {macro_entry_count} {entry_count}

#endif /* GDX_DISK_EXPECTED_H */
"""

FMT_NAMES = {0: "native / as-is (64DD disks are not byte-order-variant)",
             3: "unrecognized (used as-is)"}


def default_out():
    return os.path.join(os.path.dirname(__file__), "..", "..", "port", "gen", "gdx_disk_expected.h")


def write_header(out_path, source_desc, disk_sha, archive_sha, fmt_desc, entry_count):
    out_dir = os.path.dirname(os.path.abspath(out_path))
    os.makedirs(out_dir, exist_ok=True)
    content = HEADER_TEMPLATE.format(
        basename=os.path.basename(out_path),
        source_desc=source_desc,
        disk_sha=disk_sha,
        archive_sha=archive_sha,
        fmt_desc=fmt_desc,
        entry_count=entry_count,
        macro_disk_sha=MACRO_DISK_SHA,
        macro_archive_sha=MACRO_ARCHIVE_SHA,
        macro_entry_count=MACRO_ENTRY_COUNT,
    )
    with open(out_path, "w", encoding="utf-8", newline="\n") as f:
        f.write(content)


def main(argv):
    ap = argparse.ArgumentParser(description="Emit port/gen/gdx_disk_expected.h.")
    ap.add_argument("--archive", help="validated fzerox-disk.o2r")
    ap.add_argument("--out", default=default_out(), help="output header path")
    ap.add_argument("--placeholder", action="store_true",
                    help="write an OWNER-RUN-REQUIRED stub (zero hashes) instead of reading an archive")
    ap.add_argument("--force", action="store_true",
                    help="write the header even if structural checks fail (NOT recommended)")
    args = ap.parse_args(argv)

    if args.placeholder or not args.archive:
        # OWNER-RUN-REQUIRED: no disk dump available at generation time. The placeholder zeros compile
        # cleanly and simply suppress the dev-drift warning until the owner regenerates on their machine.
        write_header(args.out,
                     "OWNER-RUN-REQUIRED placeholder (no disk dump was available at generation time)",
                     PLACEHOLDER, PLACEHOLDER, "unknown (placeholder)", BASE_ENTRY_COUNT)
        print("Wrote PLACEHOLDER %s (OWNER-RUN-REQUIRED: regenerate with --archive fzerox-disk.o2r)"
              % args.out)
        return 0

    if not os.path.isfile(args.archive):
        print("ERROR: archive not found: %s" % args.archive, file=sys.stderr)
        return 2

    records = oc.read_records(args.archive)
    names = [r.name for r in records]
    archive_sha = oc.sha256_file(args.archive)

    entry_count = len(records)
    problems = []
    if entry_count < BASE_ENTRY_COUNT:
        problems.append("entry count %d < the %d frozen disk/* entries" % (entry_count, BASE_ENTRY_COUNT))
    if IMAGE_KEY not in names:
        problems.append("missing entry %s" % IMAGE_KEY)
    if IDENTITY_KEY not in names:
        problems.append("missing entry %s" % IDENTITY_KEY)
    # Every non-disk entry must be an ek/<symbol> per-asset slice (R8 Step 2). Anything else means a
    # stray/garbage entry leaked into the container.
    ek_entries = [n for n in names if n.startswith(EK_PREFIX)]
    stray = [n for n in names if n not in (IMAGE_KEY, IDENTITY_KEY) and not n.startswith(EK_PREFIX)]
    if stray:
        problems.append("unexpected non-ek entries: %s" % ", ".join(sorted(stray)[:8]))

    disk_sha = PLACEHOLDER
    fmt_desc = "unknown"
    if IMAGE_KEY in names:
        img = oc.read_payload(args.archive, IMAGE_KEY)
        if len(img) != IMAGE_BYTES:
            problems.append("%s is %d bytes, expected %d" % (IMAGE_KEY, len(img), IMAGE_BYTES))
    if IDENTITY_KEY in names:
        idb = oc.read_payload(args.archive, IDENTITY_KEY)
        if len(idb) != IDENTITY_BYTES:
            problems.append("%s is %d bytes, expected %d" % (IDENTITY_KEY, len(idb), IDENTITY_BYTES))
        else:
            fmt = idb[0]
            disk_sha = idb[1:].hex()
            fmt_desc = "%d (%s)" % (fmt, FMT_NAMES.get(fmt, "?"))

    print("archive       : %s" % args.archive)
    print("entry count   : %d (2 disk/* + %d ek/*)" % (entry_count, len(ek_entries)))
    print("archive sha256: %s" % archive_sha)
    print("disk identity : %s  (fmt %s)" % (disk_sha, fmt_desc))

    if problems and not args.force:
        print("", file=sys.stderr)
        for p in problems:
            print("REFUSING to write header: %s" % p, file=sys.stderr)
        print("(use --force to override; this is almost always a bug)", file=sys.stderr)
        return 1

    write_header(args.out, "deterministic gdx-extract disk output (%s)" % os.path.basename(args.archive),
                 disk_sha, archive_sha, fmt_desc, entry_count)
    print("")
    print("Wrote %s" % args.out)
    if problems:
        print("WARNING: written under --force despite: %s" % "; ".join(problems))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
