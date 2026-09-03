#!/usr/bin/env python3
"""Emit port/gen/gdx_ipl_expected.h from a validated n64ddipl.o2r (G-Diffuser R3).

Produces the code-level IPL golden constants consumed by port/gdx_extract_launch.cpp:

  #define GDX_IPL_EXPECTED_SHA256          "<hex>"   // ipl/identity: SHA-256 of the normalized IPL
  #define GDX_IPL_ARCHIVE_EXPECTED_SHA256  "<hex>"   // SHA-256 of the n64ddipl.o2r container

Unlike the cartridge golden (gen_expected_header.py), the IPL golden is PER-USER
self-consistency, not a single canonical retail hash: different owners have
different (equally valid) IPL dumps. The runtime never gates the mount on these
constants — the archive carries its own ipl/identity entry and absence falls back
to the raw IPL. These constants exist so a DEV build can WARN on drift from the
owner's reference dump.

The generator validates the archive shape (exactly the two frozen C-R3.1 entries
ipl/font_block + ipl/identity, correct font-block size, well-formed identity)
before writing, and refuses on any mismatch unless --force.

Usage:
  gen_ipl_expected.py --archive n64ddipl.o2r --out port/gen/gdx_ipl_expected.h
  gen_ipl_expected.py --placeholder --out port/gen/gdx_ipl_expected.h   # owner-run-required stub
"""

import argparse
import os
import sys

import o2r_common as oc

# Frozen contract C-R3.1.
FONT_BLOCK_KEY = "ipl/font_block"
IDENTITY_KEY = "ipl/identity"
FONT_BLOCK_BYTES = 0x140000 - 0xA0000  # 0xA0000
IDENTITY_BYTES = 1 + 32  # [u8 fmt][SHA-256]
EXPECTED_ENTRY_COUNT = 2

MACRO_IPL_SHA = "GDX_IPL_EXPECTED_SHA256"
MACRO_ARCHIVE_SHA = "GDX_IPL_ARCHIVE_EXPECTED_SHA256"

PLACEHOLDER = "0" * 64

HEADER_TEMPLATE = """\
/*
 * {basename}
 *
 * GENERATED FILE - do not edit by hand.
 * Generator : tools/o2r_harness/gen_ipl_expected.py
 * Source     : {source_desc}
 * IPL identity (normalized IPL SHA-256): {ipl_sha}
 * Archive SHA-256 (n64ddipl.o2r)       : {archive_sha}
 * Byte-order fmt                        : {fmt_desc}
 *
 * PER-USER golden for the 64DD IPL archive (R3 contract C-R3.5). These are
 * self-consistency / dev-drift constants ONLY: the runtime IPL step never gates
 * the mount on them (the archive carries its own ipl/identity entry, and absence
 * falls back to the raw IPL). Regenerate with this build's owner IPL dump after
 * any change to the extractor's IPL path.
 */
#ifndef GDX_IPL_EXPECTED_H
#define GDX_IPL_EXPECTED_H

#define {macro_ipl_sha} "{ipl_sha}"
#define {macro_archive_sha} "{archive_sha}"

#endif /* GDX_IPL_EXPECTED_H */
"""

FMT_NAMES = {0: "z64/native big-endian", 1: "v64 (16-bit-swapped)", 2: "n64 (32-bit-LE)",
             3: "unrecognized (used as-is)"}


def default_out():
    return os.path.join(os.path.dirname(__file__), "..", "..", "port", "gen", "gdx_ipl_expected.h")


def write_header(out_path, source_desc, ipl_sha, archive_sha, fmt_desc):
    out_dir = os.path.dirname(os.path.abspath(out_path))
    os.makedirs(out_dir, exist_ok=True)
    content = HEADER_TEMPLATE.format(
        basename=os.path.basename(out_path),
        source_desc=source_desc,
        ipl_sha=ipl_sha,
        archive_sha=archive_sha,
        fmt_desc=fmt_desc,
        macro_ipl_sha=MACRO_IPL_SHA,
        macro_archive_sha=MACRO_ARCHIVE_SHA,
    )
    with open(out_path, "w", encoding="utf-8", newline="\n") as f:
        f.write(content)


def main(argv):
    ap = argparse.ArgumentParser(description="Emit port/gen/gdx_ipl_expected.h.")
    ap.add_argument("--archive", help="validated n64ddipl.o2r")
    ap.add_argument("--out", default=default_out(), help="output header path")
    ap.add_argument("--placeholder", action="store_true",
                    help="write an OWNER-RUN-REQUIRED stub (zero hashes) instead of reading an archive")
    ap.add_argument("--force", action="store_true",
                    help="write the header even if structural checks fail (NOT recommended)")
    args = ap.parse_args(argv)

    if args.placeholder or not args.archive:
        # OWNER-RUN-REQUIRED: no IPL dump available at generation time. The placeholder zeros compile
        # cleanly and simply suppress the dev-drift warning until the owner regenerates on their machine.
        write_header(args.out,
                     "OWNER-RUN-REQUIRED placeholder (no IPL dump was available at generation time)",
                     PLACEHOLDER, PLACEHOLDER, "unknown (placeholder)")
        print("Wrote PLACEHOLDER %s (OWNER-RUN-REQUIRED: regenerate with --archive n64ddipl.o2r)"
              % args.out)
        return 0

    if not os.path.isfile(args.archive):
        print("ERROR: archive not found: %s" % args.archive, file=sys.stderr)
        return 2

    records = oc.read_records(args.archive)
    names = [r.name for r in records]
    archive_sha = oc.sha256_file(args.archive)

    problems = []
    if len(records) != EXPECTED_ENTRY_COUNT:
        problems.append("entry count %d != expected %d" % (len(records), EXPECTED_ENTRY_COUNT))
    if FONT_BLOCK_KEY not in names:
        problems.append("missing entry %s" % FONT_BLOCK_KEY)
    if IDENTITY_KEY not in names:
        problems.append("missing entry %s" % IDENTITY_KEY)

    ipl_sha = PLACEHOLDER
    fmt_desc = "unknown"
    if FONT_BLOCK_KEY in names:
        fb = oc.read_payload(args.archive, FONT_BLOCK_KEY)
        if len(fb) != FONT_BLOCK_BYTES:
            problems.append("%s is %d bytes, expected %d" % (FONT_BLOCK_KEY, len(fb), FONT_BLOCK_BYTES))
    if IDENTITY_KEY in names:
        idb = oc.read_payload(args.archive, IDENTITY_KEY)
        if len(idb) != IDENTITY_BYTES:
            problems.append("%s is %d bytes, expected %d" % (IDENTITY_KEY, len(idb), IDENTITY_BYTES))
        else:
            fmt = idb[0]
            ipl_sha = idb[1:].hex()
            fmt_desc = "%d (%s)" % (fmt, FMT_NAMES.get(fmt, "?"))

    print("archive       : %s" % args.archive)
    print("entries       : %s" % ", ".join(names))
    print("archive sha256: %s" % archive_sha)
    print("ipl identity  : %s  (fmt %s)" % (ipl_sha, fmt_desc))

    if problems and not args.force:
        print("", file=sys.stderr)
        for p in problems:
            print("REFUSING to write header: %s" % p, file=sys.stderr)
        print("(use --force to override; this is almost always a bug)", file=sys.stderr)
        return 1

    write_header(args.out, "deterministic gdx-extract ipl output (%s)" % os.path.basename(args.archive),
                 ipl_sha, archive_sha, fmt_desc)
    print("")
    print("Wrote %s" % args.out)
    if problems:
        print("WARNING: written under --force despite: %s" % "; ".join(problems))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
