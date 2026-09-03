#!/usr/bin/env python3
"""Entry-level diff of two .o2r archives.

Reports, at two independent levels:

  Container level - are the raw .o2r bytes identical? (zip metadata, entry
                    order, timestamps, compression choices all count here).
  Payload level   - ignoring container packaging, do the two archives carry the
                    same set of entry keys with byte-identical decompressed
                    payloads?

The distinction matters for the O2R migration: the deterministic gdx-extract
output is expected to differ from the *legacy* build-time archive at the
container level (zip timestamps / entry order / compression) while remaining
identical at the payload level. Determinism between two runs of the NEW
extractor is checked separately by verify_determinism.py (there, even the
container must match).

Usage:
  compare_archives.py A.o2r B.o2r [--json] [--limit N]

Exit code 0 when payloads are identical (same key set, same payload bytes),
1 otherwise. Container-only differences do NOT fail the exit code (they are
expected vs the legacy archive); they are still reported.
"""

import argparse
import collections
import json
import sys

import o2r_common as oc


def _name_records(records):
    """Group central-directory records by name -> {"count", "sigs"} (multiset).

    Duplicate names are common in these archives (see o2r_common.read_records),
    so comparison is multiset-based: a name present twice in A but once in B is a
    real difference, not a no-op.
    """
    by_name = collections.defaultdict(lambda: {"count": 0, "sigs": collections.Counter()})
    for e in records:
        slot = by_name[e.name]
        slot["count"] += 1
        slot["sigs"][(e.crc, e.size)] += 1
    return by_name


def _payload_multiset(path, name):
    """Sorted list of payload bytes for every record of `name` (dup-aware)."""
    payloads = []
    with __import__("zipfile").ZipFile(path) as z:
        for info in z.infolist():
            if info.filename == name:
                payloads.append(z.read(info))
    return sorted(payloads)


def compare(path_a, path_b, limit=None):
    records_a = oc.read_records(path_a)
    records_b = oc.read_records(path_b)
    by_a = _name_records(records_a)
    by_b = _name_records(records_b)
    names_a = set(by_a)
    names_b = set(by_b)

    # Names entirely absent from the other side (with their record count).
    only_a = ["%s (x%d)" % (n, by_a[n]["count"]) for n in sorted(names_a - names_b)]
    only_b = ["%s (x%d)" % (n, by_b[n]["count"]) for n in sorted(names_b - names_a)]

    # Names present on both sides but with a different record COUNT (dup mismatch).
    count_diff = []
    shared = sorted(names_a & names_b)
    for name in shared:
        if by_a[name]["count"] != by_b[name]["count"]:
            count_diff.append(
                {"name": name, "count_a": by_a[name]["count"], "count_b": by_b[name]["count"]}
            )

    # Names with matching count but differing (crc,size) signatures -> confirm by payload.
    differing = []
    for name in shared:
        if by_a[name]["count"] != by_b[name]["count"]:
            continue
        if by_a[name]["sigs"] == by_b[name]["sigs"]:
            continue
        pa = _payload_multiset(path_a, name)
        pb = _payload_multiset(path_b, name)
        if pa != pb:
            differing.append(
                {
                    "name": name,
                    "sizes_a": [len(p) for p in pa],
                    "sizes_b": [len(p) for p in pb],
                    "sigs_a": ["0x%08X/%d" % k for k in sorted(by_a[name]["sigs"])],
                    "sigs_b": ["0x%08X/%d" % k for k in sorted(by_b[name]["sigs"])],
                }
            )

    # Container-level equality: raw byte compare of the two files.
    with open(path_a, "rb") as f:
        raw_a = f.read()
    with open(path_b, "rb") as f:
        raw_b = f.read()
    container_identical = raw_a == raw_b

    order_a = oc.read_order(path_a)
    order_b = oc.read_order(path_b)
    order_identical = order_a == order_b

    payload_identical = not only_a and not only_b and not count_diff and not differing

    result = {
        "path_a": path_a,
        "path_b": path_b,
        "count_a": len(records_a),
        "count_b": len(records_b),
        "unique_a": len(names_a),
        "unique_b": len(names_b),
        "only_in_a": only_a,
        "only_in_b": only_b,
        "count_mismatches": count_diff,
        "differing_payloads": differing,
        "container_identical": container_identical,
        "order_identical": order_identical,
        "payload_identical": payload_identical,
    }
    if limit is not None:
        result["_truncated"] = {
            "only_in_a": len(only_a),
            "only_in_b": len(only_b),
            "count_mismatches": len(count_diff),
            "differing_payloads": len(differing),
        }
        result["only_in_a"] = only_a[:limit]
        result["only_in_b"] = only_b[:limit]
        result["count_mismatches"] = count_diff[:limit]
        result["differing_payloads"] = differing[:limit]
    return result


def print_report(r):
    print("=== compare_archives ===")
    print("A: %s (%d records, %d unique names)" % (r["path_a"], r["count_a"], r["unique_a"]))
    print("B: %s (%d records, %d unique names)" % (r["path_b"], r["count_b"], r["unique_b"]))
    print("")
    print("container identical (raw bytes): %s" % r["container_identical"])
    print("entry order identical          : %s" % r["order_identical"])
    print("payload identical              : %s" % r["payload_identical"])
    print("")

    def show_list(title, items):
        print("%s: %d" % (title, len(items)))
        for it in items:
            print("    %s" % it)

    show_list("only in A", r["only_in_a"])
    show_list("only in B", r["only_in_b"])
    print("record-count mismatches: %d" % len(r["count_mismatches"]))
    for d in r["count_mismatches"]:
        print("    %s  countA=%d countB=%d" % (d["name"], d["count_a"], d["count_b"]))
    print("differing payloads: %d" % len(r["differing_payloads"]))
    for d in r["differing_payloads"]:
        print(
            "    %s  sizesA=%s sizesB=%s sigsA=%s sigsB=%s"
            % (d["name"], d["sizes_a"], d["sizes_b"], d["sigs_a"], d["sigs_b"])
        )
    if "_truncated" in r:
        print("")
        print("(lists truncated to --limit; full totals: %s)" % r["_truncated"])
    print("")
    if r["payload_identical"]:
        if r["container_identical"]:
            print("PASS: archives are byte-identical (container + payload).")
        else:
            print(
                "PASS (payload): same key set and payload bytes; container differs "
                "(expected vs legacy archive: zip metadata / order / compression)."
            )
    else:
        print("FAIL: payloads differ (see only-in-A / only-in-B / differing above).")


def main(argv):
    ap = argparse.ArgumentParser(description="Entry-level diff of two .o2r archives.")
    ap.add_argument("a", help="first archive (A)")
    ap.add_argument("b", help="second archive (B)")
    ap.add_argument("--json", action="store_true", help="emit JSON instead of text")
    ap.add_argument(
        "--limit",
        type=int,
        default=None,
        help="cap the length of the diff lists (totals still reported)",
    )
    args = ap.parse_args(argv)

    r = compare(args.a, args.b, limit=args.limit)
    if args.json:
        print(json.dumps(r, indent=2))
    else:
        print_report(r)
    return 0 if r["payload_identical"] else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
