#!/usr/bin/env python3
"""Standalone reimplementation of P0 contract C5 checks 2-4, plus the
complete-or-absent family check (C6).

This mirrors the validation the port performs at first-boot (Wave 1-B) so the
logic can be exercised without booting the game. It intentionally does NOT
cover C5 check 1 (extractor exit code 0) -- that is the caller's concern
(verify_determinism.py / the orchestrator observe the child exit code).

Checks performed:
  2. Zip index readable; entry count == expected (o2r_common.EXPECTED_ENTRY_COUNT /
     GDX_O2R_EXPECTED_ENTRY_COUNT; central-directory RECORDS, dup-inclusive).
  3. Archive SHA-256 == expected (skippable with --skip-hash for pre-determinism archives).
  4. Version entry matches C4 ([0x01 big][u32 BE == 0x78D90EB3]).
  +  Complete-or-absent: every family in family_manifest.json is present with its
     full expected count, or absent entirely. A present-but-short family FAILS
     (Path B silently corrupts textures on a partial archive).

Expected values (entry count + SHA-256) are read, in order of precedence:
  --expected-header PATH  -> parse GDX_O2R_EXPECTED_* macros from a C header
  --expected-sha HEX / --expected-count N -> explicit overrides
  otherwise                -> entry count from o2r_common.EXPECTED_ENTRY_COUNT,
                              SHA-256 required unless --skip-hash.

Usage:
  validate_archive.py --archive generic.o2r --expected-header port/gen/gdx_o2r_expected.h
  validate_archive.py --archive legacy.o2r --skip-hash    # legacy predates determinism

Exit 0 when all (non-skipped) checks pass, 1 otherwise.
"""

import argparse
import json
import os
import re
import sys

import o2r_common as oc


def parse_expected_header(path, profile="us"):
    """Extract the profile's GDX_O2R_EXPECTED_SHA256[_JP] / _ENTRY_COUNT[_JP] from a C header."""
    macro_sha, macro_count = oc.profile_macros(profile)
    text = open(path, "r", encoding="utf-8").read()
    sha = None
    count = None
    m = re.search(r'#define\s+%s\s+"([0-9a-fA-F]+)"' % macro_sha, text)
    if m:
        sha = m.group(1).lower()
    m = re.search(r"#define\s+%s\s+(\d+)" % macro_count, text)
    if m:
        count = int(m.group(1))
    return sha, count


def load_manifest(path):
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def check_family_completeness(archive, manifest):
    """Complete-or-absent: each manifest family is either fully present or absent.

    Returns (ok, findings) where findings is a list of dicts describing every
    family's status. Unknown families (present in archive, not in manifest) are
    also flagged as a failure -- a misnamed key would surface here.
    """
    actual = oc.family_counts(archive)
    fams = manifest["families"]
    findings = []
    ok = True

    for fam in sorted(fams):
        expected = fams[fam]["count"]
        got = actual.get(fam, 0)
        if got == 0:
            status = "absent"          # allowed by complete-or-absent
        elif got == expected:
            status = "complete"
        else:
            status = "partial"         # FAIL
            ok = False
        findings.append(
            {
                "family": fam,
                "expected": expected,
                "actual": got,
                "inert": fams[fam]["inert"],
                "status": status,
            }
        )

    # Families present in the archive but unknown to the manifest = misnamed key.
    for fam in sorted(set(actual) - set(fams)):
        ok = False
        findings.append(
            {
                "family": fam,
                "expected": 0,
                "actual": actual[fam],
                "inert": False,
                "status": "unknown",
            }
        )
    return ok, findings


def validate(archive, expected_sha, expected_count, manifest, skip_hash, expected_crc=oc.EXPECTED_ROM_CRC):
    report = {"archive": archive, "checks": []}
    overall = True

    def record(name, ok, detail):
        nonlocal overall
        if not ok:
            overall = False
        report["checks"].append({"name": name, "ok": ok, "detail": detail})

    # Check 2: zip index readable + entry count.
    # Count central-directory RECORDS (dup-inclusive), matching the runtime gate
    # (port/gdx_extract_launch.cpp:1123, EOCD total-records field).
    try:
        records = oc.read_records(archive)
        readable = True
    except Exception as exc:  # noqa: BLE001 - report any zip parse failure
        record("2.zip_index_readable", False, "cannot read zip index: %s" % exc)
        report["overall"] = False
        return report
    count = len(records)
    unique = len({e.name for e in records})
    record("2.zip_index_readable", readable, "%d records (%d unique names)" % (count, unique))
    if expected_count is None:
        # Owner-run profile (JP): no expected count known — report, do not fail.
        record("2.entry_count", True, "count=%d (no expected count for this profile — owner-run)" % count)
    else:
        record(
            "2.entry_count",
            count == expected_count,
            "count=%d expected=%d" % (count, expected_count),
        )

    # Check 3: SHA-256.
    if skip_hash:
        record("3.sha256", True, "SKIPPED (--skip-hash)")
    elif expected_sha is None:
        record("3.sha256", False, "no expected SHA-256 provided (need header or --expected-sha)")
    else:
        actual_sha = oc.sha256_file(archive)
        record(
            "3.sha256",
            actual_sha == expected_sha,
            "actual=%s expected=%s" % (actual_sha, expected_sha),
        )

    # Check 4: version entry (C4). expected_crc=None (JP/owner-run) reports the CRC without failing.
    ver = oc.check_version_entry(archive, expected_crc)
    detail = ver["reason"]
    if ver.get("crc") is not None:
        detail = "%s (crc=0x%08X, raw=%s)" % (ver["reason"], ver["crc"], ver["raw_hex"])
    record("4.version_entry", ver["ok"], detail)

    # Complete-or-absent family check.
    fam_ok, findings = check_family_completeness(archive, manifest)
    bad = [f for f in findings if f["status"] in ("partial", "unknown")]
    record(
        "5.family_complete_or_absent",
        fam_ok,
        "ok" if fam_ok else "offending families: %s" % ", ".join(
            "%s(%s %d/%d)" % (f["family"], f["status"], f["actual"], f["expected"])
            for f in bad
        ),
    )
    report["family_findings"] = findings
    report["overall"] = overall
    return report


def print_report(report):
    print("=== validate_archive: %s ===" % report["archive"])
    for c in report["checks"]:
        print("  [%s] %s: %s" % ("PASS" if c["ok"] else "FAIL", c["name"], c["detail"]))
    print("")
    print("RESULT: %s" % ("PASS" if report["overall"] else "FAIL"))


def main(argv):
    here = os.path.dirname(__file__)
    ap = argparse.ArgumentParser(description="Validate a .o2r archive (C5 checks 2-4 + C6).")
    ap.add_argument("--archive", required=True, help="path to the .o2r to validate")
    ap.add_argument(
        "--profile", choices=["us", "jp"], default="us",
        help="golden profile (default: us). 'jp' parses the _JP macros and, since the JP cartridge "
             "CRC is owner-run/unknown, reports the version-entry CRC without failing on it.",
    )
    ap.add_argument(
        "--expected-header",
        default=None,
        help="C header with GDX_O2R_EXPECTED_* macros (e.g. port/gen/gdx_o2r_expected.h, "
             "or gdx_o2r_expected.jp.h with --profile jp)",
    )
    ap.add_argument("--expected-sha", default=None, help="override expected SHA-256 (hex)")
    ap.add_argument("--expected-count", type=int, default=None, help="override expected entry count")
    ap.add_argument(
        "--skip-hash",
        action="store_true",
        help="skip C5 check 3 (use for legacy/nondeterministic archives that predate the golden)",
    )
    ap.add_argument(
        "--manifest",
        default=os.path.join(here, "family_manifest.json"),
        help="family manifest for the complete-or-absent check",
    )
    ap.add_argument("--json", action="store_true", help="emit JSON report")
    args = ap.parse_args(argv)

    expected_sha = args.expected_sha.lower() if args.expected_sha else None
    expected_count = args.expected_count

    if args.expected_header:
        hsha, hcount = parse_expected_header(args.expected_header, args.profile)
        if expected_sha is None:
            expected_sha = hsha
        if expected_count is None:
            expected_count = hcount

    if expected_count is None:
        # For JP this is None (owner-run); leave it None so the entry-count check is informational.
        expected_count = oc.profile_expected_count(args.profile)

    expected_crc = oc.profile_expected_crc(args.profile)
    manifest = load_manifest(args.manifest)
    report = validate(args.archive, expected_sha, expected_count, manifest, args.skip_hash, expected_crc)

    if args.json:
        print(json.dumps(report, indent=2))
    else:
        print_report(report)
    return 0 if report["overall"] else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
