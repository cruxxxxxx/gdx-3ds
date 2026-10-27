#!/usr/bin/env python3
"""tools/dev/snapprobe.py -- snap-fast: summarise `[snap] probe` / `[snap] test` receipts per variant.

    tools/dev/snapprobe.py log.txt [log2.txt ...]

The snapshot module cycles its cost levers per window (port/3ds/gdx_snapshot.c, SNAP-FAST:
snap_copy x snap_sync x snap_buf), so one hardware run with `[debug] snap_probe = 1` A/Bs every
variant. This prints, per variant (copy/sync/buf), the window count and the min / median / max of
save_us, restore_us, resave_us and the bytes a sync restore / resave wrote, plus the run-ahead
estimate: one per-frame save + one restore -- for a sync variant the save is resave_us (a sync save
over the previous image), for a full variant save_us.
A card-4 log (no var= field) is reported as variant "card-4". Stdlib only.
"""
import re
import statistics
import sys

RE_LINE = re.compile(r"\[snap\] (probe|test) #\d+ .*?ok=(\d) bytes=(\d+)(.*)$")
RE_KV = re.compile(r"(\w+)=([^\s]+)")


def summarise(values):
    if not values:
        return "-"
    return f"{min(values)}/{int(statistics.median(values))}/{max(values)}"


def main(paths):
    rows = {}
    bad = 0
    for path in paths:
        with open(path, errors="replace") as f:
            for line in f:
                m = RE_LINE.search(line)
                if not m:
                    continue
                kv = dict(RE_KV.findall(m.group(4)))
                var = kv.get("var", "card-4")
                if m.group(2) != "1":
                    bad += 1
                r = rows.setdefault(var, {"n": 0, "bytes": int(m.group(3)), "save": [], "restore": [], "resave": [],
                                          "restored": [], "resaved": []})
                r["n"] += 1
                for key, dst in (("save_us", "save"), ("restore_us", "restore"), ("resave_us", "resave"),
                                 ("restored", "restored"), ("resaved", "resaved")):
                    if key in kv:
                        r[dst].append(int(kv[key]))
    if not rows:
        print("no [snap] probe/test receipts")
        return 6
    print(f"{'variant':22s} {'n':>3s} {'bytes':>7s}  {'save_us min/med/max':22s} {'restore_us':22s} "
          f"{'resave_us':22s} {'restored med':>12s} {'resaved med':>11s} {'run-ahead us':>12s}")
    for var in sorted(rows):
        r = rows[var]
        med = lambda xs: int(statistics.median(xs)) if xs else 0
        sync = "/sync/" in var
        per_frame = (med(r["resave"]) if sync and r["resave"] else med(r["save"])) + med(r["restore"])
        print(f"{var:22s} {r['n']:3d} {r['bytes']:7d}  {summarise(r['save']):22s} {summarise(r['restore']):22s} "
              f"{summarise(r['resave']):22s} {med(r['restored']):12d} {med(r['resaved']):11d} {per_frame:12d}")
    print(f"windows not ok: {bad}")
    return 1 if bad else 0


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    sys.exit(main(sys.argv[1:]))
