#!/usr/bin/env python3
"""Verify a PCM-capture scenario is byte-deterministic (C-R2.3 precondition).

Runs the SAME scenario TWICE with identical determinism pins (via run_scenario)
into two separate output labels, then compares the two SHA-256 sidecars. If the
two runs differ, no stable golden constant can exist for that scenario, so the
bit-identical gate is unreachable — mirrors tools/o2r_harness/verify_determinism.py.

On divergence it also byte-localizes the difference (first differing sample) via
compare_pcm, so a failure names WHERE the audio streams parted, not just THAT
they did.

Usage:
  verify_determinism.py --exe path/to/G-Diffuser.exe --scenario scenarios/01_title_bgm.json
                        [--out-dir captures] [--keep]

Exit 0 on identical (PASS), 1 on divergence or a run failure.
"""

import argparse
import os
import sys

import run_scenario as rs
import compare_pcm as cp


def one_run(exe, scenario, out_dir, label):
    rc = rs.run(exe, scenario, out_dir, label, extra_env=[], dry_run=False)
    if rc != 0:
        raise RuntimeError("run %s failed (rc=%d)" % (label, rc))
    prefix = os.path.join(os.path.abspath(out_dir), label)
    return prefix + ".pcm", prefix + ".pcm.sha256"


def verify(exe, scenario, out_dir, keep):
    sid = rs.load_scenario(scenario)["id"]
    pcm_a, sha_a = one_run(exe, scenario, out_dir, sid + "_runA")
    pcm_b, sha_b = one_run(exe, scenario, out_dir, sid + "_runB")

    da = cp.read_digest(sha_a)
    db = cp.read_digest(sha_b)
    print("")
    print("run A sha256: %s" % da)
    print("run B sha256: %s" % db)

    ok = bool(da) and da == db
    if ok:
        print("")
        print("PASS: two runs of scenario %r are byte-identical (deterministic)." % sid)
        print("      golden SHA-256 = %s" % da)
    else:
        print("")
        print("FAIL: scenario %r is not deterministic across two runs." % sid)
        # Localize the first divergent sample for the report.
        try:
            cp.compare_pcm(pcm_a, pcm_b)
        except OSError as exc:
            print("  (could not byte-localize: %s)" % exc)

    if not keep:
        for p in (pcm_a, sha_a, pcm_b, sha_b):
            try:
                os.remove(p)
            except OSError:
                pass
    else:
        print("")
        print("kept: %s, %s (+ .sha256)" % (pcm_a, pcm_b))
    return ok


def main(argv):
    ap = argparse.ArgumentParser(description="Verify a PCM scenario is byte-deterministic.")
    ap.add_argument("--exe", required=True, help="path to G-Diffuser.exe")
    ap.add_argument("--scenario", required=True, help="path to a scenarios/*.json file")
    ap.add_argument("--out-dir", default="captures", help="directory for the two runs' artifacts")
    ap.add_argument("--keep", action="store_true", help="keep both captures for inspection")
    args = ap.parse_args(argv)
    try:
        ok = verify(args.exe, args.scenario, args.out_dir, args.keep)
    except RuntimeError as exc:
        print("ERROR: %s" % exc, file=sys.stderr)
        return 1
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
