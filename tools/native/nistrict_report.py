#!/usr/bin/env python3
"""nistrict_report.py — rank what still goes through the interpreter, from `[debug] ni_strict=1` runs.

    tools/native/nistrict_report.py --run LABEL:MODEL:LOG [--run ...] [--md OUT.md] [--html OUT.html]
                                    [--json OUT.json] [--title T] [--top N]

Each LOG is a guest log of one replay run with `[debug] ni_strict=1` (native-all-plan.md S7). The
run's LAST full `[nistrict] final` dump is read (it is cumulative); a run that died before its
`final` falls back to its last periodic `sum` dump, which carries only each bucket's top cells and
is flagged `partial` in the output.

What the receipt files, per bucket (game mode x sub-scope: steady / intro / racing / post / change /
demo), is one cell per (context, tag, site, opcode): every command a stock handler ran. The report:

  * sums cells over the runs of one MODEL (frames add, commands add), so cmds/f is a frame-weighted
    mean over every replay that reached that screen;
  * classes each cell:
      disp     the walker's own G_NOOP / G_DL / G_ENDDL in the walk context (what the S7 marker
               dispatcher keeps; not residue);
      direct   a capture run's TEXRECT / LOADBLOCK drawn by the direct emitter (native; not residue);
      steady   present in >= 50% of the bucket's frames;
      first    present in <= 3 frames per visit of the bucket (a first-sight record / build pass);
      inter    the rest (intermittent);
    and a cell of the `change` sub-scope is `transition` whatever its persistence (S4 in progress);
  * ranks residue rows (screen x category x opcode) by cmds/f, the larger model first.

Stdlib only (PROTOCOL.md).
"""

import argparse
import html
import json
import re
import sys
from collections import defaultdict

MODES = ["title", "gp_race", "practice", "vs2p", "vs3p", "vs4p", "records", "main_menu", "mach_select",
         "mach_settings", "course_select", "credits_skip", "credits", "course_edit", "time_attack",
         "gp_next_course", "create_machine", "gp_end_cs", "gp_next_mach", "records_course", "options",
         "death_race", "ead_demo", "other"]
SUBS = ["steady", "intro", "racing", "post", "change", "demo"]

OPS = {
    0x00: "NOOP", 0x01: "VTX", 0x02: "MODIFYVTX", 0x03: "CULLDL", 0x04: "BRANCH_Z", 0x05: "TRI1",
    0x06: "TRI2", 0x07: "QUAD", 0x08: "LINE3D", 0xD7: "TEXTURE", 0xD8: "POPMTX", 0xD9: "GEOMETRYMODE",
    0xDA: "MTX", 0xDB: "MOVEWORD", 0xDC: "MOVEMEM", 0xDD: "LOAD_UCODE", 0xDE: "DL", 0xDF: "ENDDL",
    0xE0: "SPNOOP", 0xE1: "RDPHALF_1", 0xE2: "SETOTHERMODE_L", 0xE3: "SETOTHERMODE_H", 0xE4: "TEXRECT",
    0xE5: "TEXRECTFLIP", 0xE6: "RDPLOADSYNC", 0xE7: "RDPPIPESYNC", 0xE8: "RDPTILESYNC",
    0xE9: "RDPFULLSYNC", 0xEA: "SETKEYGB", 0xEB: "SETKEYR", 0xEC: "SETCONVERT", 0xED: "SETSCISSOR",
    0xEE: "SETPRIMDEPTH", 0xEF: "RDPSETOTHERMODE", 0xF0: "LOADTLUT", 0xF1: "RDPHALF_2",
    0xF2: "SETTILESIZE", 0xF3: "LOADBLOCK", 0xF4: "LOADTILE", 0xF5: "SETTILE", 0xF6: "FILLRECT",
    0xF7: "SETFILLCOLOR", 0xF8: "SETFOGCOLOR", 0xF9: "SETBLENDCOLOR", 0xFA: "SETPRIMCOLOR",
    0xFB: "SETENVCOLOR", 0xFC: "SETCOMBINE", 0xFD: "SETTIMG", 0xFE: "SETZIMG", 0xFF: "SETCIMG",
}
NRR_CATS = ["hud", "trk", "mach", "sky", "dress", "shd", "fx", "self", "deco"]
TAGS = {
    0x00: "", 0x5A: "track seam", 0x5B: "track seam", 0x60: "track_retained seam", 0x5C: "fx seam",
    0x5D: "fx seam", 0x5E: "HUD/fx capture run", 0x5F: "shadow capture run", 0x65: "sky seam",
    0x6D: "gadget run", 0x6E: "gadget run", 0x6B: "course preview (ni_course)", 0x64: "NRR sky learn",
    0x66: "NRR fx learn", 0x67: "NRR deco record", 0xDE: "G_DL hook",
}
SITES = {
    "walk": "walker", "nested": "kept G_DL in a run", "niex": "span EX op", "nistx": "span ST fallback",
    "niseg": "span segment fallback", "nirecov": "span bail -> walk", "hudrun": "capture-run replay",
    "hudirect": "direct emitter", "hudplain": "MENU run refused (stock)", "hookcmd": "hook list re-run",
    "course": "ni_course leave",
}

HEAD_RE = re.compile(r"\[nistrict\] (final|sum|hosttest) b=([0-9A-F]{2})\.(\d) (\S+)/(\S+) frames=(\d+) visits=(\d+) "
                     r"strict/f=([\d.]+) disp/f=([\d.]+) direct/f=([\d.]+) maxf=(\d+) zero=(\d+)")
HIST_RE = re.compile(r"hist=(\d+)/(\d+)/(\d+)/(\d+)/(\d+) cells=(\d+) ovf=(\d+)")
ENT_RE = re.compile(r"\[nistrict\] final b=([0-9A-F]{2})\.(\d) e c=(\S+) t=([0-9A-F]{2}) s=(\S+) op=([0-9A-F]{2}) "
                    r"n=(\d+) fr=(\d+) vis=(\d+) max=(\d+) first=(\d+) last=(\d+)")
TOP_RE = re.compile(r"(\w+)/(\w+)/([0-9A-F]{2})/([0-9A-F]{2})=([\d.]+)")


def tag_name(ctx, tag):
    if tag >= 0xC0 and tag < 0xC0 + len(NRR_CATS):
        return "NRR " + NRR_CATS[tag - 0xC0]
    return TAGS.get(tag, "tag %02X" % tag)


def category(ctx, tag, site):
    t = tag_name(ctx, tag)
    base = ctx if not t else "%s: %s" % (ctx, t)
    return "%s [%s]" % (base, SITES.get(site, site))


def parse_log(path):
    """The last complete dump of the log: {'why', 'buckets': {(mode, sub): {...}}, 'partial'}."""
    dumps = []
    cur = None
    with open(path, errors="replace") as fh:
        for line in fh:
            if "[nistrict]" not in line:
                continue
            m = re.search(r"\[nistrict\] (final|sum) begin dump=(\d+)", line)
            if m:
                cur = {"why": m.group(1), "buckets": {}, "partial": m.group(1) != "final"}
                continue
            if cur is None:
                continue
            if re.search(r"\[nistrict\] (final|sum) end dump=", line):
                dumps.append(cur)
                cur = None
                continue
            m = HEAD_RE.search(line)
            if m and " e c=" not in line:
                key = (int(m.group(2), 16), int(m.group(3)))
                b = cur["buckets"].setdefault(key, {"cells": {}})
                b.update(frames=int(m.group(6)), visits=int(m.group(7)), strict=float(m.group(8)),
                         disp=float(m.group(9)), direct=float(m.group(10)), maxf=int(m.group(11)),
                         zero=int(m.group(12)))
                h = HIST_RE.search(line)
                if h:
                    b["hist"] = [int(h.group(i)) for i in range(1, 6)]
                    b["ovf"] = int(h.group(7))
                continue
            m = ENT_RE.search(line)
            if m:
                key = (int(m.group(1), 16), int(m.group(2)))
                b = cur["buckets"].setdefault(key, {"cells": {}})
                ck = (m.group(3), int(m.group(4), 16), m.group(5), int(m.group(6), 16))
                b["cells"][ck] = {"n": int(m.group(7)), "fr": int(m.group(8)), "vis": int(m.group(9)),
                                  "max": int(m.group(10))}
                continue
            if cur["why"] == "sum" and " top " in line:
                m2 = re.search(r"b=([0-9A-F]{2})\.(\d) top(.*)", line)
                if m2:
                    key = (int(m2.group(1), 16), int(m2.group(2)))
                    b = cur["buckets"].setdefault(key, {"cells": {}})
                    for t in TOP_RE.finditer(m2.group(3)):
                        ck = (t.group(1), int(t.group(3), 16), t.group(2), int(t.group(4), 16))
                        b["cells"][ck] = {"n": float(t.group(5)) * max(b.get("frames", 1), 1), "fr": 0, "vis": 0,
                                          "max": 0}
    finals = [d for d in dumps if d["why"] == "final"]
    if finals:
        return finals[-1]
    return dumps[-1] if dumps else None


def classify(mode, sub, ctx, site, op, frames, visits, c):
    if site == "hudirect":
        return "direct"
    if ctx == "walk" and site == "walk" and op in (0x00, 0xDE, 0xDF):
        return "disp"
    if sub == 4:
        return "transition"
    if frames > 0 and c["fr"] >= 0.5 * frames:
        return "steady"
    if c["vis"] > 0 and c["fr"] <= 3 * c["vis"]:
        return "first"
    return "inter"


def stage_for(mode, sub, ctx, tag, site, cls):
    """The native-all-plan.md stage that owns a residue row (a heuristic, stated in the report)."""
    name = MODES[mode] if mode < len(MODES) else "other"
    if cls == "transition":
        return "S4 (in progress, native-menus-s4)"
    if cls == "first" and ctx in ("mesh", "nrr"):
        return "S8 (first-sight recorder)"
    if name in ("course_edit", "create_machine", "ead_demo"):
        return "S5 (EK)"
    if name == "gp_end_cs" or name in ("credits", "credits_skip"):
        return "S6 (ending / credits)"
    race = name in ("gp_race", "practice", "time_attack", "death_race", "vs2p", "vs3p", "vs4p")
    if race and sub == 1 and ctx in ("hud", "nrrhud"):
        return "S6 (countdown TRI2)"
    if sub == 5:
        return "S6 (attract demo)"
    if name in ("vs2p", "vs3p", "vs4p"):
        return "S6 (multi-view)"
    if name in ("gp_race", "practice", "time_attack", "death_race"):
        if ctx in ("hud", "nrrhud") or tag in (0x5E, 0x5F):
            return "S2/S6 (race HUD runs)"
        return "S6 (race stock leftovers)"
    if name == "records":
        return "S2b (records glyphs) / S6"
    if ctx in ("hud", "nrrhud") or site in ("hudplain", "hudrun", "nested"):
        return "S2 (2D runs)"
    if ctx == "walk":
        return "S2a (frame root)"
    return "S2/S6"


def aggregate(runs):
    """{model: {(mode, sub): {'frames','visits','strict_n','disp_n','direct_n','replays', 'cells': {ck: agg}}}}"""
    out = defaultdict(dict)
    for label, model, dump in runs:
        if dump is None:
            continue
        for (mode, sub), b in dump["buckets"].items():
            if "frames" not in b:
                continue
            a = out[model].setdefault((mode, sub), {"frames": 0, "visits": 0, "strict_n": 0.0, "disp_n": 0.0,
                                                    "direct_n": 0.0, "maxf": 0, "replays": set(), "cells": {},
                                                    "partial": False})
            f = b["frames"]
            a["frames"] += f
            a["visits"] += b["visits"]
            a["strict_n"] += b["strict"] * f
            a["disp_n"] += b["disp"] * f
            a["direct_n"] += b["direct"] * f
            a["maxf"] = max(a["maxf"], b["maxf"])
            a["replays"].add(label)
            a["partial"] = a["partial"] or dump["partial"]
            for ck, c in b["cells"].items():
                cc = a["cells"].setdefault(ck, {"n": 0, "fr": 0, "vis": 0, "max": 0, "replays": set(),
                                                "frames_in": 0, "visits_in": 0})
                cc["n"] += c["n"]
                cc["fr"] += c["fr"]
                cc["vis"] += c["vis"]
                cc["max"] = max(cc["max"], c["max"])
                cc["replays"].add(label)
    return out


def rows_for(agg):
    rows = []
    for model, buckets in agg.items():
        total = sum(a["frames"] for a in buckets.values()) or 1
        for (mode, sub), a in buckets.items():
            for (ctx, tag, site, op), c in a["cells"].items():
                cls = classify(mode, sub, ctx, site, op, a["frames"], a["visits"], c)
                rows.append({
                    "model": model, "mode": mode, "sub": sub,
                    "screen": "%s/%s" % (MODES[mode] if mode < len(MODES) else "other", SUBS[sub]),
                    "ctx": ctx, "tag": tag, "site": site, "op": op,
                    "opname": OPS.get(op, "%02X" % op), "category": category(ctx, tag, site),
                    "cmdsf": c["n"] / a["frames"] if a["frames"] else 0.0,
                    "corpus": c["n"] / total,
                    "persist": c["fr"] / a["frames"] if a["frames"] else 0.0,
                    "fr": c["fr"], "vis": c["vis"], "max": c["max"], "n": c["n"],
                    "frames": a["frames"], "visits": a["visits"], "cls": cls,
                    "stage": stage_for(mode, sub, ctx, tag, site, cls),
                    "replays": sorted(c["replays"]),
                })
    return rows


def merge_models(rows):
    """One row per (screen, category, opcode) with both models' cmds/f side by side."""
    m = {}
    for r in rows:
        k = (r["mode"], r["sub"], r["ctx"], r["tag"], r["site"], r["op"])
        e = m.setdefault(k, {"screen": r["screen"], "category": r["category"], "op": r["op"], "opname": r["opname"],
                             "mode": r["mode"], "sub": r["sub"], "ctx": r["ctx"], "tag": r["tag"], "site": r["site"],
                             "by": {}, "cls": r["cls"], "stage": r["stage"], "replays": set()})
        e["by"][r["model"]] = r
        e["replays"].update(r["replays"])
        # the class of the model where the row weighs most decides
        best = max(e["by"].values(), key=lambda x: x["cmdsf"])
        e["cls"] = best["cls"]
        e["stage"] = best["stage"]
    out = list(m.values())
    for e in out:
        e["key"] = max(v["cmdsf"] for v in e["by"].values())
    out.sort(key=lambda e: -e["key"])
    return out


def bucket_table(agg):
    rows = []
    keys = set()
    for model, buckets in agg.items():
        keys.update(buckets.keys())
    for (mode, sub) in sorted(keys):
        r = {"screen": "%s/%s" % (MODES[mode] if mode < len(MODES) else "other", SUBS[sub]), "mode": mode,
             "sub": sub, "by": {}}
        for model, buckets in agg.items():
            a = buckets.get((mode, sub))
            if a is None or a["frames"] == 0:
                continue
            f = a["frames"]
            steady = sum(c["n"] for ck, c in a["cells"].items()
                         if classify(mode, sub, ck[0], ck[2], ck[3], f, a["visits"], c) == "steady") / f
            first = sum(c["n"] for ck, c in a["cells"].items()
                        if classify(mode, sub, ck[0], ck[2], ck[3], f, a["visits"], c) == "first") / f
            r["by"][model] = {"frames": f, "visits": a["visits"], "strict": a["strict_n"] / f,
                              "disp": a["disp_n"] / f, "direct": a["direct_n"] / f, "maxf": a["maxf"],
                              "steady": steady, "first": first, "replays": sorted(a["replays"]),
                              "partial": a["partial"]}
        rows.append(r)
    rows.sort(key=lambda r: -max(v["strict"] for v in r["by"].values()) if r["by"] else 0)
    return rows


def rollup(merged):
    """screen x category (opcodes summed): the work list's unit."""
    g = {}
    for e in merged:
        if e["cls"] in ("disp", "direct"):
            continue
        k = (e["mode"], e["sub"], e["ctx"], e["tag"], e["site"])
        r = g.setdefault(k, {"screen": e["screen"], "category": e["category"], "mode": e["mode"], "sub": e["sub"],
                             "by": {}, "ops": [], "replays": set(), "cells": []})
        r["cells"].append(e)
        r["replays"].update(e["replays"])
        for m, v in e["by"].items():
            b = r["by"].setdefault(m, {"cmdsf": 0.0, "corpus": 0.0, "persist": 0.0, "max": 0})
            b["cmdsf"] += v["cmdsf"]
            b["corpus"] += v["corpus"]
            b["persist"] = max(b["persist"], v["persist"])
            b["max"] = max(b["max"], v["max"])
    out = []
    for r in g.values():
        r["key"] = max(v["cmdsf"] for v in r["by"].values())
        cells = sorted(r["cells"], key=lambda e: -e["key"])
        r["ops"] = ["%s %s" % (OPS.get(e["op"], "%02X" % e["op"]), fmt(e["key"])) for e in cells[:5]]
        # the class / stage of the cells carrying most of the commands
        w = defaultdict(float)
        for e in cells:
            w[(e["cls"], e["stage"])] += e["key"]
        r["cls"], r["stage"] = max(w.items(), key=lambda kv: kv[1])[0]
        out.append(r)
    out.sort(key=lambda r: -r["key"])
    return out


def fmt(x):
    return "%.1f" % x if x >= 0.05 else ("0" if x == 0 else "<0.1")


def fmt2(x):
    return "%.2f" % x if x >= 0.005 else ("0" if x == 0 else "<0.01")


def write_md(path, title, models, merged, roll, buckets, top, runs_info, notes_md):
    L = ["# %s" % title, ""]
    L += notes_md
    L.append("")
    L.append("Classes: **steady** = in >= 50% of the screen's frames; **first** = <= 3 frames per visit (first-sight "
             "record / build); **inter** = intermittent; **transition** = mode-change frames (S4 in progress, "
             "native-menus-s4). disp (the walker's own marker / list words) and direct (HUD direct emitter) are "
             "not residue. cmds/f = per rendered frame of that screen; corpus/f = per frame of the whole corpus "
             "(how much the item weighs overall).")
    L.append("")
    L.append("## Per screen (cmds/f a stock handler ran)")
    L.append("")
    L.append("| screen |" + "".join(" %s strict | %s steady | %s first | %s frames |" % (m, m, m, m) for m in models))
    L.append("|---|" + "---|" * (4 * len(models)))
    for r in buckets:
        cells = []
        for m in models:
            v = r["by"].get(m)
            cells += [fmt(v["strict"]), fmt(v["steady"]), fmt(v["first"]), str(v["frames"])] if v else ["-"] * 4
        L.append("| %s | %s |" % (r["screen"], " | ".join(cells)))
    L.append("")
    work = [r for r in roll if r["cls"] != "transition"]
    L.append("## Work list: screen x category, ranked (transitions excluded; top %d)" % top)
    L.append("")
    L.append("| # | screen | category | top opcodes (cmds/f) | " + " | ".join("%s cmds/f | %s corpus/f" % (m, m)
                                                                          for m in models) + " | persist | class | stage |")
    L.append("|---|---|---|---|" + "---|---|" * len(models) + "---|---|---|")
    for k, r in enumerate(work[:top], 1):
        best = max(r["by"].values(), key=lambda x: x["cmdsf"])
        vals = []
        for m in models:
            v = r["by"].get(m)
            vals += [fmt(v["cmdsf"]), fmt2(v["corpus"])] if v else ["-", "-"]
        L.append("| %d | %s | %s | %s | %s | %.0f%% | %s | %s |" % (
            k, r["screen"], r["category"], ", ".join(r["ops"]), " | ".join(vals), 100 * best["persist"], r["cls"],
            r["stage"]))
    L.append("")
    trans = [r for r in roll if r["cls"] == "transition"]
    L.append("## Transition residue (mode-change frames): S4 in progress (top 25)")
    L.append("")
    L.append("| screen | category | top opcodes | " + " | ".join("%s cmds/f | %s corpus/f" % (m, m) for m in models) + " |")
    L.append("|---|---|---|" + "---|---|" * len(models))
    for r in trans[:25]:
        vals = []
        for m in models:
            v = r["by"].get(m)
            vals += [fmt(v["cmdsf"]), fmt2(v["corpus"])] if v else ["-", "-"]
        L.append("| %s | %s | %s | %s |" % (r["screen"], r["category"], ", ".join(r["ops"][:3]), " | ".join(vals)))
    L.append("")
    L.append("## Opcode detail: screen x category x opcode (top %d, transitions excluded)" % top)
    L.append("")
    L.append("| # | screen | category | opcode | " + " | ".join("%s cmds/f" % m for m in models) +
             " | persist | class | stage |")
    L.append("|---|---|---|---|" + "---|" * len(models) + "---|---|---|")
    k = 0
    for e in merged:
        if e["cls"] in ("disp", "direct", "transition"):
            continue
        k += 1
        if k > top:
            break
        best = max(e["by"].values(), key=lambda x: x["cmdsf"])
        L.append("| %d | %s | %s | %02X %s | %s | %.0f%% | %s | %s |" % (
            k, e["screen"], e["category"], e["op"], e["opname"],
            " | ".join(fmt(e["by"][m]["cmdsf"]) if m in e["by"] else "-" for m in models),
            100.0 * best["persist"], e["cls"], e["stage"]))
    L.append("")
    L.append("## Runs")
    L.append("")
    for label, model, logpath, partial in runs_info:
        L.append("- `%s` (%s)%s" % (label, model, " PARTIAL (no final dump)" if partial else ""))
    open(path, "w").write("\n".join(L) + "\n")


CSS = """
:root{--bg:#fbfbfa;--fg:#1d1d1f;--mut:#6b6b70;--line:#e3e3e0;--card:#fff;--acc:#2f5bd3;--acc2:#9db4f0;
--steady:#c2410c;--first:#6d28d9;--inter:#0f766e;--trans:#a16207;--chip:#f1f1ee}
@media (prefers-color-scheme: dark){:root:not([data-theme="light"]){--bg:#141416;--fg:#ececee;--mut:#9a9aa2;
--line:#2a2a2e;--card:#1c1c20;--acc:#7aa2ff;--acc2:#3b4f86;--steady:#fb923c;--first:#a78bfa;--inter:#2dd4bf;
--trans:#facc15;--chip:#26262b}}
:root[data-theme="dark"]{--bg:#141416;--fg:#ececee;--mut:#9a9aa2;--line:#2a2a2e;--card:#1c1c20;--acc:#7aa2ff;
--acc2:#3b4f86;--steady:#fb923c;--first:#a78bfa;--inter:#2dd4bf;--trans:#facc15;--chip:#26262b}
*{box-sizing:border-box}html{-webkit-text-size-adjust:100%}body{margin:0;background:var(--bg);color:var(--fg);
font:14px/1.5 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif}
main{max-width:1240px;margin:0 auto;padding:24px 16px 64px}h1{font-size:22px;margin:0 0 6px}
h2{font-size:17px;margin:36px 0 8px}p,li{color:var(--mut)}p{margin:6px 0}ul{margin:6px 0 6px 18px;padding:0}
b,strong{color:var(--fg)}
.wrap{overflow-x:auto;border:1px solid var(--line);border-radius:8px;background:var(--card);max-height:78vh}
table{border-collapse:collapse;width:100%;font-size:12.5px}th,td{padding:5px 8px;border-bottom:1px solid var(--line);
text-align:left;white-space:nowrap;vertical-align:top}th{position:sticky;top:0;background:var(--card);font-weight:600;
cursor:pointer;z-index:1}td.num,th.num{text-align:right;font-variant-numeric:tabular-nums}tr:last-child td{border-bottom:0}
td.wrapcell{white-space:normal;min-width:220px}
.chip{display:inline-block;padding:1px 7px;border-radius:10px;background:var(--chip);font-size:11.5px}
.steady{color:var(--steady)}.first{color:var(--first)}.inter{color:var(--inter)}.transition{color:var(--trans)}
.bar{display:inline-block;height:8px;background:var(--acc);border-radius:2px;vertical-align:middle;margin-right:6px}
.bar.b2{background:var(--acc2)}
.controls{display:flex;gap:12px;flex-wrap:wrap;margin:8px 0}.controls label{color:var(--mut)}
input,select{background:var(--card);color:var(--fg);border:1px solid var(--line);border-radius:6px;padding:4px 6px;
font:inherit}code{font-size:12px}
.kpis{display:grid;grid-template-columns:repeat(auto-fit,minmax(170px,1fr));gap:10px;margin:14px 0}
.kpi{background:var(--card);border:1px solid var(--line);border-radius:8px;padding:10px 12px}
.kpi .v{font-size:20px;font-weight:600;color:var(--fg);font-variant-numeric:tabular-nums}.kpi .l{color:var(--mut);font-size:12px}
"""

JS = """
function sortTable(t,col,num){const b=t.tBodies[0];const rows=[...b.rows];const dir=t.dataset.sc==col&&t.dataset.sd=='d'?'a':'d';
t.dataset.sc=col;t.dataset.sd=dir;rows.sort((x,y)=>{let a=x.cells[col].dataset.v??x.cells[col].textContent,
c=y.cells[col].dataset.v??y.cells[col].textContent;if(num){a=parseFloat(a)||0;c=parseFloat(c)||0;return dir=='d'?c-a:a-c}
return dir=='d'?c.localeCompare(a):a.localeCompare(c)});rows.forEach(r=>b.appendChild(r))}
document.querySelectorAll('table.sortable th').forEach((th,i)=>th.addEventListener('click',()=>sortTable(th.closest('table'),i,th.classList.contains('num'))));
function filt(id){const q=document.getElementById(id+'-q').value.toLowerCase(),c=document.getElementById(id+'-cls').value;
document.querySelectorAll('#'+id+' tbody tr').forEach(r=>{const ok=(!q||r.textContent.toLowerCase().includes(q))&&(!c||r.dataset.cls==c);r.style.display=ok?'':'none'})}
['work','detail'].forEach(id=>{const q=document.getElementById(id+'-q');if(!q)return;q.addEventListener('input',()=>filt(id));
document.getElementById(id+'-cls').addEventListener('change',()=>filt(id))});
"""


def _controls(E, tid, with_trans):
    opts = "<option value=''>all</option><option>steady</option><option>first</option><option>inter</option>"
    if with_trans:
        opts += "<option>transition</option>"
    return ("<div class=controls><label>filter <input id=%s-q placeholder='screen, category, opcode, stage'></label>"
            "<label>class <select id=%s-cls>%s</select></label></div>" % (tid, tid, opts))


def write_html(path, title, subtitle, models, merged, roll, buckets, top, runs_info, notes, kpis):
    E = html.escape
    out = ["<!doctype html><html lang=en><head><meta charset=utf-8>",
           "<meta name=viewport content='width=device-width,initial-scale=1'>",
           "<title>Interpreter residue census</title><style>%s</style></head><body><main>" % CSS]
    out.append("<h1>%s</h1><p>%s</p>" % (E(title), E(subtitle)))
    if kpis:
        out.append("<div class=kpis>")
        for v, l in kpis:
            out.append("<div class=kpi><div class=v>%s</div><div class=l>%s</div></div>" % (E(v), E(l)))
        out.append("</div>")
    for n in notes:
        out.append(n)
    # work list
    work = [r for r in roll if r["cls"] != "transition"]
    scale = max([r["key"] for r in work] or [1.0])
    out.append("<h2>Work list: screen &times; category, ranked</h2><p>Opcodes summed per (screen, category). "
               "Transition frames are listed separately below (S4 in progress). <b>cmds/f</b> is per rendered frame "
               "of that screen; <b>corpus/f</b> spreads it over every frame of the corpus (overall weight). "
               "Click a header to sort.</p>")
    out.append(_controls(E, "work", False))
    out.append("<div class=wrap><table class=sortable id=work><thead><tr><th class=num>#</th><th>screen</th>"
               "<th>category</th><th>top opcodes (cmds/f)</th>")
    for m in models:
        out.append("<th class=num>%s cmds/f</th><th class=num>%s corpus/f</th>" % (E(m), E(m)))
    out.append("<th class=num>persist</th><th>class</th><th>stage</th><th>replays</th></tr></thead><tbody>")
    for k, r in enumerate(work[:max(top, 80)], 1):
        best = max(r["by"].values(), key=lambda x: x["cmdsf"])
        out.append("<tr data-cls='%s'><td class=num>%d</td><td>%s</td><td>%s</td><td class=wrapcell>%s</td>" % (
            E(r["cls"]), k, E(r["screen"]), E(r["category"]), E(", ".join(r["ops"]))))
        for i, m in enumerate(models):
            v = r["by"].get(m)
            if v is None:
                out.append("<td class=num data-v=0>-</td><td class=num data-v=0>-</td>")
                continue
            w = max(1, int(56 * v["cmdsf"] / scale))
            out.append("<td class=num data-v='%.4f'><span class='bar%s' style='width:%dpx'></span>%s</td>"
                       "<td class=num data-v='%.5f'>%s</td>" % (v["cmdsf"], " b2" if i else "", w, fmt(v["cmdsf"]),
                                                                v["corpus"], fmt2(v["corpus"])))
        out.append("<td class=num data-v='%.4f'>%.0f%%</td><td><span class='chip %s'>%s</span></td><td>%s</td>"
                   "<td class=wrapcell><code>%s</code></td></tr>" % (
                       best["persist"], 100 * best["persist"], E(r["cls"]), E(r["cls"]), E(r["stage"]),
                       E(", ".join(sorted(r["replays"])))))
    out.append("</tbody></table></div>")
    # per screen
    out.append("<h2>Per screen</h2><p>Commands a stock handler ran per rendered frame of the screen: all of it "
               "(strict), the steady-state part and the first-sight part; disp = the walker's own marker / list "
               "words, kept by the S7 dispatcher.</p>")
    out.append("<div class=wrap><table class=sortable><thead><tr><th>screen</th>")
    for m in models:
        out.append("<th class=num>%s strict/f</th><th class=num>%s steady/f</th><th class=num>%s first/f</th>"
                   "<th class=num>%s disp/f</th><th class=num>%s max/f</th><th class=num>%s frames</th>" % ((E(m),) * 6))
    out.append("<th>replays</th></tr></thead><tbody>")
    for r in buckets:
        out.append("<tr><td>%s</td>" % E(r["screen"]))
        reps = set()
        for m in models:
            v = r["by"].get(m)
            if v is None:
                out.append("<td class=num>-</td>" * 6)
                continue
            reps.update(v["replays"])
            out.append("<td class=num data-v='%.3f'>%s</td><td class=num data-v='%.3f'>%s</td>"
                       "<td class=num data-v='%.3f'>%s</td><td class=num data-v='%.3f'>%s</td>"
                       "<td class=num>%d</td><td class=num>%d</td>" % (
                           v["strict"], fmt(v["strict"]), v["steady"], fmt(v["steady"]), v["first"], fmt(v["first"]),
                           v["disp"], fmt(v["disp"]), v["maxf"], v["frames"]))
        out.append("<td class=wrapcell><code>%s</code></td></tr>" % E(", ".join(sorted(reps))))
    out.append("</tbody></table></div>")
    # transitions
    trans = [r for r in roll if r["cls"] == "transition"]
    out.append("<h2>Transition residue: S4 in progress</h2><p>Mode-change frames (the wipe between screens). "
               "Owned by the S4 track (branch native-menus-s4); listed so it is not lost, not ranked into the "
               "work list.</p>")
    out.append("<div class=wrap><table class=sortable><thead><tr><th>screen</th><th>category</th><th>top opcodes</th>")
    for m in models:
        out.append("<th class=num>%s cmds/f</th><th class=num>%s corpus/f</th>" % (E(m), E(m)))
    out.append("<th>stage</th></tr></thead><tbody>")
    for r in trans[:40]:
        out.append("<tr><td>%s</td><td>%s</td><td class=wrapcell>%s</td>" % (E(r["screen"]), E(r["category"]),
                                                                         E(", ".join(r["ops"][:4]))))
        for m in models:
            v = r["by"].get(m)
            out.append("<td class=num data-v='%.4f'>%s</td><td class=num data-v='%.5f'>%s</td>" % (
                v["cmdsf"], fmt(v["cmdsf"]), v["corpus"], fmt2(v["corpus"])) if v else
                       "<td class=num>-</td><td class=num>-</td>")
        out.append("<td>S4 in progress</td></tr>")
    out.append("</tbody></table></div>")
    # opcode detail
    out.append("<h2>Opcode detail: screen &times; category &times; opcode</h2><p>Every residue cell, ranked by the "
               "larger model's cmds/f. persist = share of the screen's frames the cell appeared in; max/f = most in "
               "one frame.</p>")
    out.append(_controls(E, "detail", True))
    out.append("<div class=wrap><table class=sortable id=detail><thead><tr><th class=num>#</th><th>screen</th>"
               "<th>category</th><th>opcode</th>")
    for m in models:
        out.append("<th class=num>%s cmds/f</th>" % E(m))
    out.append("<th class=num>persist</th><th class=num>max/f</th><th>class</th><th>stage</th></tr></thead><tbody>")
    k = 0
    for e in merged:
        if e["cls"] in ("disp", "direct"):
            continue
        k += 1
        if k > 400:
            break
        best = max(e["by"].values(), key=lambda x: x["cmdsf"])
        out.append("<tr data-cls='%s'><td class=num>%d</td><td>%s</td><td>%s</td><td><code>%02X</code> %s</td>" % (
            E(e["cls"]), k, E(e["screen"]), E(e["category"]), e["op"], E(e["opname"])))
        for m in models:
            v = e["by"].get(m)
            out.append("<td class=num data-v='%.4f'>%s</td>" % (v["cmdsf"], fmt(v["cmdsf"])) if v else
                       "<td class=num data-v=0>-</td>")
        out.append("<td class=num data-v='%.4f'>%.0f%%</td><td class=num>%d</td><td><span class='chip %s'>%s</span>"
                   "</td><td>%s</td></tr>" % (best["persist"], 100 * best["persist"], best["max"], E(e["cls"]),
                                              E(e["cls"]), E(e["stage"])))
    out.append("</tbody></table></div>")
    out.append("<h2>Runs</h2><div class=wrap><table><thead><tr><th>replay</th><th>model</th><th>log</th></tr>"
               "</thead><tbody>")
    for label, model, logpath, partial in runs_info:
        out.append("<tr><td>%s%s</td><td>%s</td><td><code>%s</code></td></tr>" % (
            E(label), " (partial)" if partial else "", E(model), E(logpath)))
    out.append("</tbody></table></div></main><script>%s</script></body></html>" % JS)
    open(path, "w").write("".join(out))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--run", action="append", default=[], help="LABEL:MODEL:LOG")
    ap.add_argument("--md")
    ap.add_argument("--html")
    ap.add_argument("--json")
    ap.add_argument("--title", default="ni_strict census: what still goes through the interpreter")
    ap.add_argument("--subtitle", default="")
    ap.add_argument("--note", action="append", default=[], help="an HTML block for the report header")
    ap.add_argument("--note-md", action="append", default=[], help="a Markdown paragraph for the summary header")
    ap.add_argument("--top", type=int, default=60)
    a = ap.parse_args()
    runs = []
    info = []
    for spec in a.run:
        label, model, logpath = spec.split(":", 2)
        d = parse_log(logpath)
        if d is None:
            print("no [nistrict] dump in %s" % logpath, file=sys.stderr)
        runs.append((label, model, d))
        info.append((label, model, logpath, d is None or d["partial"]))
    models = []
    for _, m, _ in runs:
        if m not in models:
            models.append(m)
    agg = aggregate(runs)
    merged = merge_models(rows_for(agg))
    roll = rollup(merged)
    buckets = bucket_table(agg)
    kpis = []
    for m in models:
        tot = sum(a["frames"] for a in agg[m].values()) or 1
        strict = sum(a["strict_n"] for a in agg[m].values()) / tot
        kpis.append(("%s" % fmt(strict), "%s: strict cmds per frame, whole corpus" % m))
        kpis.append(("%d" % tot, "%s: frames censused" % m))
    if a.md:
        write_md(a.md, a.title, models, merged, roll, buckets, a.top, info, a.note_md)
    if a.html:
        write_html(a.html, a.title, a.subtitle, models, merged, roll, buckets, a.top, info, a.note, kpis)
    if a.json:
        def clean(o):
            if isinstance(o, set):
                return sorted(o)
            if isinstance(o, dict):
                return {str(k): clean(v) for k, v in o.items() if k != "cells"}
            if isinstance(o, list):
                return [clean(x) for x in o]
            return o
        json.dump(clean({"buckets": buckets, "worklist": roll[:200]}), open(a.json, "w"), indent=1)
    work = [r for r in roll if r["cls"] != "transition"]
    for k, r in enumerate(work[:20], 1):
        print("%2d %-22s %-50s %s | %s | %s %s" % (
            k, r["screen"], r["category"][:50],
            " ".join("%s=%s/%s" % (m, fmt(r["by"][m]["cmdsf"]), fmt2(r["by"][m]["corpus"])) if m in r["by"] else
                     "%s=-" % m for m in models), ", ".join(r["ops"][:3]), r["cls"], r["stage"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
