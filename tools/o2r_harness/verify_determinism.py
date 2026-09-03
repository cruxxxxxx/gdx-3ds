#!/usr/bin/env python3
"""Verify gdx-extract is byte-deterministic.

Runs a given gdx-extract binary TWICE, into two separate temp destination
directories, against the same ROM + recipes, then byte-compares the two output
archives. Determinism (P0 contract C2) is the precondition for the SHA-256
golden gate (C5 check 3) -- if two runs differ, no stable golden constant can
exist.

Divergence diagnostics are entry-level:
  1. Compare the two zip central directories: entry name sets, order, and
     per-entry (CRC, size). Name the FIRST differing entry.
  2. If the central directories match but the raw bytes differ, report the
     first differing file offset (container-level nondeterminism, e.g. a stray
     timestamp).

Invocation shape (C2):
  gdx-extract o2r <rom.z64> -s <recipes> -d <datadir> [extra args]
The output archive is expected at <datadir>/<out-name> (default generic.o2r).

Usage:
  verify_determinism.py --extractor path/to/gdx-extract --rom rom.z64 --recipes decomp-recipes
  [--out-name generic.o2r] [--extra-arg ARG ...] [--keep]

Exit 0 on byte-identical (PASS), 1 on divergence or extractor failure.
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile

import o2r_common as oc


def run_extractor(extractor, rom, recipes, datadir, out_name, extra_args, label):
    """Invoke gdx-extract once. Returns the path to the produced archive."""
    cmd = [extractor, "o2r", rom, "-s", recipes, "-d", datadir] + list(extra_args)
    print("[%s] %s" % (label, " ".join(cmd)))
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.stdout:
        for line in proc.stdout.splitlines():
            print("  [%s out] %s" % (label, line))
    if proc.returncode != 0:
        print("  [%s err] exit code %d" % (label, proc.returncode))
        if proc.stderr:
            for line in proc.stderr.splitlines():
                print("  [%s err] %s" % (label, line))
        raise RuntimeError("extractor exited %d for run %s" % (proc.returncode, label))

    out_path = os.path.join(datadir, out_name)
    if not os.path.isfile(out_path):
        # Fall back to any single .o2r in the datadir.
        found = [f for f in os.listdir(datadir) if f.endswith(".o2r")]
        if len(found) == 1:
            out_path = os.path.join(datadir, found[0])
        else:
            raise RuntimeError(
                "no output archive at %s (datadir contains: %s)"
                % (out_path, ", ".join(os.listdir(datadir)) or "<empty>")
            )
    return out_path


def diff_central_directories(path_a, path_b):
    """Return (identical, message). Names the first differing entry.

    Positional (record-based) so duplicate-named entries are compared by their
    actual central-directory slot, not by a name-keyed lookup.
    """
    records_a = oc.read_records(path_a)
    records_b = oc.read_records(path_b)

    if len(records_a) != len(records_b):
        return False, "record count differs: A=%d B=%d" % (len(records_a), len(records_b))

    for i, (ea, eb) in enumerate(zip(records_a, records_b)):
        if ea.name != eb.name:
            return False, (
                "entry order diverges at index %d: A=%r B=%r" % (i, ea.name, eb.name)
            )
        if ea.crc != eb.crc or ea.size != eb.size:
            return False, (
                "first differing entry %r (index %d): "
                "A crc=0x%08X size=%d | B crc=0x%08X size=%d"
                % (ea.name, i, ea.crc, ea.size, eb.crc, eb.size)
            )
        if ea.compress_type != eb.compress_type:
            return False, (
                "entry %r (index %d): compression differs A=%d B=%d"
                % (ea.name, i, ea.compress_type, eb.compress_type)
            )
    return True, "central directories identical"


def first_byte_divergence(path_a, path_b):
    with open(path_a, "rb") as f:
        a = f.read()
    with open(path_b, "rb") as f:
        b = f.read()
    if a == b:
        return None
    n = min(len(a), len(b))
    for i in range(n):
        if a[i] != b[i]:
            return i, a[i], b[i]
    return n, None, None  # one is a prefix of the other


def verify(extractor, rom, recipes, out_name, extra_args, keep):
    tmp_root = tempfile.mkdtemp(prefix="gdx_determinism_")
    dir_a = os.path.join(tmp_root, "run_a")
    dir_b = os.path.join(tmp_root, "run_b")
    os.makedirs(dir_a)
    os.makedirs(dir_b)
    try:
        arc_a = run_extractor(extractor, rom, recipes, dir_a, out_name, extra_args, "run A")
        arc_b = run_extractor(extractor, rom, recipes, dir_b, out_name, extra_args, "run B")

        sha_a = oc.sha256_file(arc_a)
        sha_b = oc.sha256_file(arc_b)
        print("")
        print("run A sha256: %s" % sha_a)
        print("run B sha256: %s" % sha_b)

        if sha_a == sha_b:
            print("")
            print("PASS: two extractions are byte-identical (deterministic).")
            print("      golden SHA-256 = %s" % sha_a)
            return True

        # Diverged - localize it.
        print("")
        print("FAIL: extractions differ.")
        cd_ok, cd_msg = diff_central_directories(arc_a, arc_b)
        print("  central directory: %s" % cd_msg)
        if cd_ok:
            div = first_byte_divergence(arc_a, arc_b)
            if div is not None:
                off, ba, bb = div
                if ba is None:
                    print("  raw bytes: one archive is a prefix of the other at offset %d" % off)
                else:
                    print(
                        "  raw bytes: first divergence at offset %d: A=0x%02X B=0x%02X "
                        "(container-level nondeterminism, e.g. a timestamp)" % (off, ba, bb)
                    )
        return False
    finally:
        if keep:
            print("")
            print("kept temp dirs at: %s" % tmp_root)
        else:
            shutil.rmtree(tmp_root, ignore_errors=True)


def main(argv):
    ap = argparse.ArgumentParser(description="Verify gdx-extract determinism.")
    ap.add_argument("--extractor", required=True, help="path to the gdx-extract binary")
    ap.add_argument("--rom", required=True, help="path to the big-endian .z64 ROM")
    ap.add_argument("--recipes", required=True, help="recipe dir (config.yml + assets/yaml/...)")
    ap.add_argument("--out-name", default="generic.o2r", help="expected output archive name")
    ap.add_argument(
        "--extra-arg",
        action="append",
        default=[],
        dest="extra_args",
        help="extra argument passed to gdx-extract (repeatable)",
    )
    ap.add_argument("--keep", action="store_true", help="keep the temp dirs for inspection")
    args = ap.parse_args(argv)

    for label, p in (("extractor", args.extractor), ("rom", args.rom), ("recipes", args.recipes)):
        if not os.path.exists(p):
            print("ERROR: %s not found: %s" % (label, p), file=sys.stderr)
            return 2

    try:
        ok = verify(args.extractor, args.rom, args.recipes, args.out_name, args.extra_args, args.keep)
    except RuntimeError as exc:
        print("ERROR: %s" % exc, file=sys.stderr)
        return 1
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
