#!/usr/bin/env python3
"""tools/dev/snapcensus.py -- snap-fast: map the snapshot dirty census onto the ELF's symbols.

The guest's `[debug] snap_census = 1` (port/3ds/gdx_snapshot.c) dumps, per race and every 300
in-scope frames, one bit per 32-byte block of the snapshot's static regions: set = the block
changed between two consecutive in-race frames at least once this session. This tool ORs the LAST
dump of every log given (all from the SAME build), then reports per symbol how many of its bytes sit
in a block that ever changed.

    tools/dev/snapcensus.py build.elf run1/log.txt run2/log.txt ... [--min 256] [--json out.json]
    tools/dev/snapcensus.py build.elf test-run/log.txt --check-holes

--check-holes reads the `[snap] hole NAME @ADDR size=N` receipts of a snap_test / snap_probe run
and checks every one against the ELF: a symbol of that name at that address whose nm size is N (the
runtime table's sizes are written by hand, so this is what proves they match the link).

A symbol with dirty=0 across the whole corpus never changed during any race the corpus drove: a
candidate for the NOSNAP class (not restored, watched by `[debug] snap_test`). It is a CANDIDATE,
not a proof -- the corpus decides what was exercised, and the gate's NOSNAP watch is the backstop.
Stdlib only.
"""
import argparse
import json
import os
import re
import subprocess
import sys

BLOCK = 32
NM = os.path.join(os.environ.get("DEVKITPRO", "/opt/devkitpro"), "devkitARM/bin/arm-none-eabi-nm")

RE_CENSUS = re.compile(r"\[snap\] census #(\d+) \(([^)]*)\) frames=(\d+) races=(\d+) dirty=(\d+)/(\d+) "
                       r"blocks of \d+ B, per-frame avg=(\d+) max=(\d+)")
RE_REGION = re.compile(r"\[snap\] cregion (\d+) (\S+) base=0x([0-9a-fA-F]+) size=(\d+) first_block=(\d+)")
RE_HOLE = re.compile(r"\[snap\] hole (\S+) @0x([0-9a-fA-F]+) size=(\d+)$")
RE_MAP = re.compile(r"\[snap\] cmap (\d+) ([0-9a-f]+)")


def parse_log(path):
    """Returns (header, regions, words) of the last complete dump in the log, or None."""
    dumps = []
    cur = None
    with open(path, errors="replace") as f:
        for line in f:
            m = RE_CENSUS.search(line)
            if m:
                cur = {"header": m.groups(), "regions": [], "words": {}}
                dumps.append(cur)
                continue
            if cur is None:
                continue
            m = RE_REGION.search(line)
            if m:
                cur["regions"].append((int(m.group(1)), m.group(2), int(m.group(3), 16), int(m.group(4)),
                                       int(m.group(5))))
                continue
            m = RE_MAP.search(line)
            if m:
                first_word = int(m.group(1)) // 32
                hexs = m.group(2)
                for k in range(len(hexs) // 8):
                    cur["words"][first_word + k] = int(hexs[k * 8:k * 8 + 8], 16)
    for d in reversed(dumps):
        total = int(d["header"][5])
        if len(d["words"]) * 32 >= total:
            return d
    return None


def load_symbols(elf):
    out = subprocess.run([NM, "-S", "-n", elf], capture_output=True, text=True, check=True).stdout
    syms = []
    for line in out.splitlines():
        p = line.split()
        if len(p) == 4:
            syms.append((int(p[0], 16), int(p[1], 16), p[2], p[3]))
    return syms


def check_holes(elf, logs):
    by_addr = {}
    for addr, size, kind, name in load_symbols(elf):
        by_addr.setdefault(addr, []).append((name, size))
    bad = 0
    seen = 0
    for path in logs:
        holes = {}
        with open(path, errors="replace") as f:
            for line in f:
                m = RE_HOLE.search(line.rstrip())
                if m:
                    holes[m.group(1)] = (int(m.group(2), 16), int(m.group(3)))
        for name, (addr, size) in sorted(holes.items(), key=lambda kv: kv[1][0]):
            seen += 1
            match = [s for n, s in by_addr.get(addr, []) if n == name]
            if not match:
                print(f"  BAD  {name} @{addr:08x}: no such symbol at that address")
                bad += 1
            elif match[0] != size:
                print(f"  BAD  {name} @{addr:08x}: table size {size} != ELF size {match[0]}")
                bad += 1
            else:
                print(f"  ok   {name} @{addr:08x} size={size}")
    print(f"check-holes: {seen} holes, {bad} bad")
    if seen == 0:
        return 6
    return 1 if bad else 0


def census_build(elf, logs):
    """One build's logs -> (rows, frames, races, clean bytes, image bytes, logs used)."""
    regions = None
    words = {}
    frames = 0
    races = 0
    used = 0
    for path in logs:
        d = parse_log(path)
        if d is None:
            print(f"  {path}: no complete census dump", file=sys.stderr)
            continue
        h = d["header"]
        if regions is None:
            regions = d["regions"]
        elif [r[2:4] for r in regions] != [r[2:4] for r in d["regions"]]:
            print(f"  {path}: region layout differs from {elf}'s other logs -- skipped", file=sys.stderr)
            continue
        print(f"  {path}: frames={h[2]} races={h[3]} dirty={h[4]}/{h[5]} per-frame avg={h[6]} max={h[7]} blocks")
        frames += int(h[2])
        races += int(h[3])
        used += 1
        for k, v in d["words"].items():
            words[k] = words.get(k, 0) | v
    if regions is None:
        return [], 0, 0, 0, 0, 0

    def dirty(block):
        return (words.get(block // 32, 0) >> (block % 32)) & 1

    syms = load_symbols(elf)
    rows = []
    total_clean = 0
    total_bytes = 0
    for _, rname, base, size, first in regions:
        rclean = 0
        for b in range((size + BLOCK - 1) // BLOCK):
            if not dirty(first + b):
                rclean += min(BLOCK, size - b * BLOCK)
        total_clean += rclean
        total_bytes += size
        print(f"  [{os.path.basename(elf)}] region {rname}: {size} B, {rclean} B in never-changed blocks")
        for addr, ssize, kind, name in syms:
            if ssize == 0 or addr < base or addr + ssize > base + size:
                continue
            b0 = (addr - base) // BLOCK
            b1 = (addr + ssize - 1 - base) // BLOCK
            flags = [dirty(first + b) for b in range(b0, b1 + 1)]
            # interior = blocks the symbol owns alone (an edge block may be a neighbour's change)
            inner = [dirty(first + b) for b in range(b0, b1 + 1)
                     if base + b * BLOCK >= addr and base + (b + 1) * BLOCK <= addr + ssize]
            rows.append({"name": name, "addr": addr, "size": ssize, "kind": kind, "region": rname,
                         "dirty_blocks": sum(flags), "inner_dirty": sum(inner), "blocks": b1 - b0 + 1})
    return rows, frames, races, total_clean, total_bytes, used


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("elf")
    ap.add_argument("logs", nargs="+")
    ap.add_argument("--min", type=int, default=256, help="smallest symbol to list (bytes)")
    ap.add_argument("--json", help="write the per-symbol table here")
    ap.add_argument("--check-holes", action="store_true", help="verify the [snap] hole receipts against the ELF")
    args = ap.parse_args()
    if args.check_holes:
        return check_holes(args.elf, args.logs)

    # Groups: an argument ending in .elf starts a new build; the logs after it are that build's.
    groups = [[args.elf, []]]
    for a in args.logs:
        if a.endswith(".elf"):
            groups.append([a, []])
        else:
            groups[-1][1].append(a)
    merged = {}  # symbol name -> table row (dirty blocks ORed across builds by name)
    frames = 0
    races = 0
    total_clean = 0
    total_bytes = 0
    nlogs = 0
    for elf, logs in groups:
        rows, f, r, c, b, n = census_build(elf, logs)
        frames += f
        races += r
        nlogs += n
        if n:
            total_clean, total_bytes = c, b  # per build; the last build's figure is printed
        for row in rows:
            prev = merged.get(row["name"])
            if prev is None:
                merged[row["name"]] = row
            else:
                prev["dirty_blocks"] = max(prev["dirty_blocks"], row["dirty_blocks"])
                prev["inner_dirty"] = max(prev["inner_dirty"], row["inner_dirty"])
                prev["builds"] = prev.get("builds", 1) + 1
    if nlogs == 0:
        print("no census data", file=sys.stderr)
        return 6
    table = list(merged.values())
    print(f"corpus: {nlogs} logs, {frames} in-race frames, {races} races; "
          f"{total_clean}/{total_bytes} B of the static image never changed")
    clean = sorted([t for t in table if t["dirty_blocks"] == 0 and t["size"] >= args.min], key=lambda t: -t["size"])
    print(f"\nnever-changed symbols >= {args.min} B: {len(clean)}, {sum(t['size'] for t in clean)} B")
    for t in clean:
        print(f"  {t['addr']:08x} {t['size']:8d} {t['kind']} {t['region']:5s} {t['name']}")
    big_dirty = sorted([t for t in table if t["dirty_blocks"] > 0 and t["size"] >= 4096], key=lambda t: -t["size"])
    print(f"\nchanged symbols >= 4096 B: {len(big_dirty)}")
    for t in big_dirty:
        print(f"  {t['addr']:08x} {t['size']:8d} {t['kind']} {t['region']:5s} {t['name']}  "
              f"dirty {t['dirty_blocks']}/{t['blocks']} blocks (interior {t['inner_dirty']})")
    if args.json:
        with open(args.json, "w") as f:
            json.dump(table, f, indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
