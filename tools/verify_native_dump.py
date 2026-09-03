#!/usr/bin/env python3
"""Oracle equivalence harness for the Native Dump All port (Wave 1).

Compares two dump trees — the Python oracle (tools/gen_dump_all.py) vs the native
(`gdx-extract dump`) — file-by-file, per class, and reports PASS/FAIL with first-divergence detail.

Comparison rules (per file type):
  PNG          decoded RGBA pixels + dimensions must be equal (PNG *container* bytes legitimately
               differ by encoder: Pillow in the oracle vs stb in native — the decoded pixels are the
               load-bearing equivalence, exactly as gen_dump_all.py's own --verify does).
  JSON         parsed-structure equality. Numbers compare by value (so 145 == 145.0). Provenance
               keys that legitimately differ by environment (`source` — an absolute/relative path to
               the ROM/IPL image the tree was produced from) are ignored and reported separately.
  TSV          data rows (non `#`-comment lines) must match exactly. The leading `#` header comment
               can legitimately embed an environment-specific path (e.g. the baserom path in the
               dlists/vertexdata manifests) and is compared loosely (reported, not failed).
  TXT/BIN/GDG  exact bytes.

The file SET must match too: a file present in one tree and absent in the other is a divergence.

Exit code 0 iff every requested/present class PASSes; 1 otherwise.

Usage:
  verify_native_dump.py <oracle-dump-dir> <native-dump-dir> [--classes a,b,...]
"""
import argparse
import json
import os
import sys

try:
    from PIL import Image
except ImportError:
    sys.stderr.write("verify_native_dump: Pillow (PIL) is required: pip install Pillow\n")
    raise

CLASS_SUBDIRS = {"coursedata", "dlists", "vertexdata", "tables", "ghosts", "fonts", "audio", "music",
                 "models"}
# JSON keys whose values are environment-dependent provenance (paths) and are compared loosely.
PROVENANCE_KEYS = {"source"}


def classify(rel):
    """Map a relative path to its dump class. Anything not under a known class subdir is a texture."""
    top = rel.replace("\\", "/").split("/", 1)[0]
    return top if top in CLASS_SUBDIRS else "textures"


def walk(root):
    out = {}
    for dirpath, _dirs, files in os.walk(root):
        for f in files:
            full = os.path.join(dirpath, f)
            rel = os.path.relpath(full, root).replace("\\", "/")
            out[rel] = full
    return out


def _nums_equal(a, b):
    return a == b  # 145 == 145.0 is True; equal doubles compare equal


def json_equal(a, b, path, provenance):
    """Recursive structural equality. Records provenance-key mismatches instead of failing them."""
    if isinstance(a, dict) and isinstance(b, dict):
        ka, kb = set(a), set(b)
        if ka != kb:
            return "%s: key set differs (only-oracle=%s only-native=%s)" % (
                path or "<root>", sorted(ka - kb), sorted(kb - ka))
        for k in a:
            if k in PROVENANCE_KEYS:
                if a[k] != b[k]:
                    provenance.append("%s/%s: oracle=%r native=%r" % (path, k, a[k], b[k]))
                continue
            err = json_equal(a[k], b[k], "%s/%s" % (path, k), provenance)
            if err:
                return err
        return None
    if isinstance(a, list) and isinstance(b, list):
        if len(a) != len(b):
            return "%s: list length %d != %d" % (path, len(a), len(b))
        for i, (x, y) in enumerate(zip(a, b)):
            err = json_equal(x, y, "%s[%d]" % (path, i), provenance)
            if err:
                return err
        return None
    if isinstance(a, bool) or isinstance(b, bool):
        if a is not b:
            return "%s: %r != %r" % (path, a, b)
        return None
    if isinstance(a, (int, float)) and isinstance(b, (int, float)):
        if not _nums_equal(a, b):
            return "%s: %r != %r" % (path, a, b)
        return None
    if a != b:
        return "%s: %r != %r" % (path, a, b)
    return None


def compare_png(pa, pb):
    ia = Image.open(pa).convert("RGBA")
    ib = Image.open(pb).convert("RGBA")
    if ia.size != ib.size:
        return "size %s != %s" % (ia.size, ib.size)
    if ia.tobytes() != ib.tobytes():
        return "pixel mismatch (%s)" % (ia.size,)
    return None


def compare_json(pa, pb, provenance):
    with open(pa, encoding="utf-8") as fh:
        a = json.load(fh)
    with open(pb, encoding="utf-8") as fh:
        b = json.load(fh)
    return json_equal(a, b, "", provenance)


def compare_tsv(pa, pb, header_notes):
    def data_rows(p):
        rows = []
        header = None
        with open(p, "rb") as fh:
            for line in fh.read().split(b"\n"):
                if line.startswith(b"#"):
                    if header is None:
                        header = line
                    continue
                rows.append(line)
        return rows, header
    ra, ha = data_rows(pa)
    rb, hb = data_rows(pb)
    if ha != hb:
        header_notes.append("header comment differs: oracle=%r native=%r" % (ha, hb))
    if ra != rb:
        # find first differing row
        for i, (x, y) in enumerate(zip(ra, rb)):
            if x != y:
                return "data row %d differs: oracle=%r native=%r" % (i, x, y)
        return "row count differs: oracle=%d native=%d" % (len(ra), len(rb))
    return None


def compare_bytes(pa, pb):
    with open(pa, "rb") as fh:
        a = fh.read()
    with open(pb, "rb") as fh:
        b = fh.read()
    if a != b:
        return "byte mismatch (oracle %d bytes, native %d bytes)" % (len(a), len(b))
    return None


def compare_file(rel, pa, pb, provenance, header_notes):
    ext = os.path.splitext(rel)[1].lower()
    if ext == ".png":
        return compare_png(pa, pb)
    if ext == ".json":
        return compare_json(pa, pb, provenance)
    if ext == ".tsv":
        return compare_tsv(pa, pb, header_notes)
    return compare_bytes(pa, pb)  # .txt/.bin/.gdg/other


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("oracle", help="oracle dump dir (tools/gen_dump_all.py output)")
    ap.add_argument("native", help="native dump dir (gdx-extract dump output)")
    ap.add_argument("--classes", default=None,
                    help="comma-separated classes to check (default: all present)")
    args = ap.parse_args()

    only = None
    if args.classes:
        only = set(c.strip() for c in args.classes.split(",") if c.strip())

    oracle = walk(args.oracle)
    native = walk(args.native)
    all_rels = sorted(set(oracle) | set(native))

    # Per-class accumulators.
    classes = {}

    def cls(rel):
        c = classify(rel)
        return classes.setdefault(c, {"checked": 0, "failures": [], "provenance": [],
                                       "header_notes": [], "missing": [], "extra": []})

    for rel in all_rels:
        c = classify(rel)
        if only is not None and c not in only:
            continue
        st = cls(rel)
        if rel not in native:
            st["missing"].append(rel)
            continue
        if rel not in oracle:
            st["extra"].append(rel)
            continue
        prov = []
        hdr = []
        try:
            err = compare_file(rel, oracle[rel], native[rel], prov, hdr)
        except Exception as exc:  # noqa: BLE001
            err = "comparison error: %s" % exc
        st["checked"] += 1
        for p in prov:
            st["provenance"].append("%s :: %s" % (rel, p))
        for h in hdr:
            st["header_notes"].append("%s :: %s" % (rel, h))
        if err:
            st["failures"].append("%s: %s" % (rel, err))

    rc = 0
    print("=" * 78)
    print("Native Dump All — oracle equivalence report")
    print("  oracle: %s" % args.oracle)
    print("  native: %s" % args.native)
    print("=" * 78)
    for c in sorted(classes):
        st = classes[c]
        ok = not st["failures"] and not st["missing"] and not st["extra"]
        status = "PASS" if ok else "FAIL"
        if not ok:
            rc = 1
        print("[%-10s] %s  (%d files checked)" % (c, status, st["checked"]))
        for m in st["missing"][:10]:
            print("    MISSING in native: %s" % m)
        for e in st["extra"][:10]:
            print("    EXTRA in native:   %s" % e)
        for f in st["failures"][:10]:
            print("    DIVERGENCE: %s" % f)
        if len(st["failures"]) > 10:
            print("    ... and %d more divergences" % (len(st["failures"]) - 10))
        if st["provenance"]:
            print("    (provenance-only differences, not failures: %d — e.g. %s)"
                  % (len(st["provenance"]), st["provenance"][0]))
        if st["header_notes"]:
            print("    (manifest header-comment differences, not failures: %d — e.g. %s)"
                  % (len(st["header_notes"]), st["header_notes"][0]))
    print("=" * 78)
    print("OVERALL: %s" % ("ALL PASS" if rc == 0 else "FAIL"))
    return rc


if __name__ == "__main__":
    sys.exit(main())
