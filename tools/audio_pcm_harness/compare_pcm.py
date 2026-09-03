#!/usr/bin/env python3
"""Compare two PCM captures (C-R2.3 gate).

Two modes, auto-selected by extension:

  * Both paths end in .sha256  -> compare the digests recorded in the sidecars
    (sha256sum-style: first whitespace token of the first line). Fast, and the
    canonical gate check (bit-identical <=> equal SHA-256).

  * Otherwise                  -> byte-for-byte compare the two .pcm files and,
    on divergence, report the FIRST differing sample: its absolute byte offset,
    the frame index and channel (interleaved s16 stereo => 4 bytes/frame), and
    both s16 values. Also reports a length mismatch.

Usage:
  compare_pcm.py A.pcm B.pcm
  compare_pcm.py A.pcm.sha256 B.pcm.sha256

Exit 0 if identical, 1 if they differ, 2 on a usage/IO error.
"""

import struct
import sys


def read_digest(path):
    with open(path, "r", encoding="utf-8") as f:
        line = f.readline().strip()
    return line.split()[0] if line else ""


def compare_sha(a, b):
    da, db = read_digest(a), read_digest(b)
    print("A %s : %s" % (a, da or "<empty>"))
    print("B %s : %s" % (b, db or "<empty>"))
    if da and da == db:
        print("\nPASS: SHA-256 identical.")
        return 0
    print("\nFAIL: SHA-256 differ.")
    return 1


def compare_pcm(a, b):
    with open(a, "rb") as f:
        da = f.read()
    with open(b, "rb") as f:
        db = f.read()

    print("A %s : %d bytes (%d frames)" % (a, len(da), len(da) // 4))
    print("B %s : %d bytes (%d frames)" % (b, len(db), len(db) // 4))

    if da == db:
        print("\nPASS: byte-identical.")
        return 0

    n = min(len(da), len(db))
    first = None
    for i in range(n):
        if da[i] != db[i]:
            first = i
            break

    print("\nFAIL: captures differ.")
    if first is None:
        # Common prefix identical; one is longer than the other.
        print("  one capture is a prefix of the other; length differs at byte %d "
              "(A=%d, B=%d bytes)" % (n, len(da), len(db)))
        return 1

    frame = first // 4
    within = first % 4
    channel = "L" if within < 2 else "R"
    # Decode the whole s16 pair at that frame from both sides for context.
    off = frame * 4
    la, ra = struct.unpack_from("<hh", da, off) if off + 4 <= len(da) else (None, None)
    lb, rb = struct.unpack_from("<hh", db, off) if off + 4 <= len(db) else (None, None)
    print("  first divergence at byte %d = frame %d, channel %s (byte %d within frame)"
          % (first, frame, channel, within))
    print("  A frame %d: L=%s R=%s" % (frame, la, ra))
    print("  B frame %d: L=%s R=%s" % (frame, lb, rb))
    if len(da) != len(db):
        print("  (note: total lengths also differ: A=%d B=%d bytes)" % (len(da), len(db)))
    return 1


def main(argv):
    if len(argv) != 2:
        print(__doc__)
        return 2
    a, b = argv
    try:
        if a.endswith(".sha256") and b.endswith(".sha256"):
            return compare_sha(a, b)
        return compare_pcm(a, b)
    except OSError as exc:
        print("ERROR: %s" % exc, file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
