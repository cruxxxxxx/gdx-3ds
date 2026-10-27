"""Build the post-mortem pages.

    PM_SCRATCH=<dir> python3 build.py [OUT_DIR]     (default OUT_DIR: the postmortem directory, one level up)

The New 3DS chapter (index/timeline/difficulties/frontier/agentic numbers; road-to-60 and cost in full)
was first generated from a session scratchpad (census.json, page-data.json, git-3ds-log.txt,
user-prompts.txt) under PM_SCRATCH. Those inputs no longer exist in this tree. When they are missing,
this script reads frozen-new3ds.json (the values that were rendered from them the last time they did
exist) for every other page's numbers, and re-wraps the two pages it cannot recompute without the raw
per-session fps samples and the full token census (road-to-60.html, cost.html) in the current
navigation, unchanged except for a short pointer forward that road-to-60.html gets appended regardless.
The native-fork page (which includes the Old 3DS campaign) needs no session scratchpad: it loads its own data
(o3ds-data.json) or carry their numbers inline, cited to git history and docs/research.
"""
import json, os, sys, collections
sys.path.insert(0, os.path.dirname(__file__))
from framework import *
import page_index, page_author, page_timeline, page_road, page_native, page_boundaries
import page_difficulties, page_frontier, page_agentic, page_cost

HERE = os.path.dirname(os.path.abspath(__file__))
PM = os.path.dirname(HERE)
SCRATCH = os.environ.get("PM_SCRATCH", os.path.join(HERE, "scratch"))
OUT = sys.argv[1] if len(sys.argv) > 1 else PM
os.makedirs(OUT, exist_ok=True)

LIVE = os.path.exists(os.path.join(SCRATCH, "census", "census.json"))


def load_live():
    """The original path: recompute everything from the scratchpad census (kept for reproducibility)."""
    import statistics as st
    C = json.load(open(os.path.join(SCRATCH, "census", "census.json")))
    P = json.load(open(os.path.join(SCRATCH, "page-data.json")))
    tot = C["totals"]["combined"]
    tokens_total = tot["input"] + tot["output"] + tot["cache_creation"] + tot["cache_read"]
    costA = C["cost_usd"]["A"]; costB = C["cost_usd"]["B"]
    tA = costA["total"] if "total" in costA else sum(v["combined"] for v in costA["per_model"].values())
    tB = costB["total"] if "total" in costB else sum(v["combined"] for v in costB["per_model"].values())
    commits_per_day = collections.Counter()
    for line in open(os.path.join(SCRATCH, "git-3ds-log.txt")):
        if line[:4] == "2026":
            commits_per_day[line[:10]] += 1
    prompts_per_day = collections.Counter()
    for line in open(os.path.join(SCRATCH, "user-prompts.txt")):
        if line[:4] == "2026" and "Image" not in line:
            prompts_per_day[line[:10]] += 1
    S = P["sessions"]

    def stats(f):
        f = sorted(f); n = len(f)
        return st.median(f), f[int(0.1 * n)], 100 * sum(1 for x in f if x >= 59.5) / n
    r5 = stats(S[-1]["fps"])
    headline_rows = [
        ["08-14 first race (emulator)", "12–20", "–", "–"],
        ["08-21 first hardware run", "48–50", "–", "–"],
        ["08-28 release-grade build", "~51", "~40 (crowds, MINIMAL)", "–"],
    ] + [[f"09-0{2 if i < 4 else 3} {s['name']}: {s['sub']}", f"{stats(s['fps'])[0]:.1f}", f"{stats(s['fps'])[1]:.1f}", f"{stats(s['fps'])[2]:.0f}%"] for i, s in enumerate(S)]
    frozen = json.load(open(os.path.join(HERE, "frozen-new3ds.json")))["D"]
    D = dict(frozen)
    D.update({
        "subagents": f"{C['subagents']['subagent_transcripts']}",
        "workflow_agents": f"{C['subagents']['workflow_agent_transcripts']:,}",
        "workflows": f"{C['subagents']['workflow_runs']}",
        "prompts": f"{C['human_prompts']['count_strict']}",
        "active_days": f"{C['date_range']['active_days']}",
        "final_median": f"{r5[0]:.1f}", "final_p10": f"{r5[1]:.1f}", "final_cap": f"{r5[2]:.0f}%",
        "tokens_total_h": f"{tokens_total/1e9:.2f} B", "cost_a_h": f"${tA:,.0f}", "cost_b_h": f"${tB:,.0f}",
        "headline_rows": headline_rows,
    })
    road_html = page_road.build(D, P)
    road_html = road_html.replace("</main>", page_road.continuation_note() + "</main>")
    return D, commits_per_day, prompts_per_day, road_html, page_cost.build(D, C)


def load_frozen():
    F = json.load(open(os.path.join(HERE, "frozen-new3ds.json")))
    road = (page_road.build_current(F["D"]) if hasattr(page_road, "build_current")
            else reframe("road-to-60.html", open(os.path.join(PM, "road-to-60.html")).read()))
    road = road.replace("</main>", page_road.continuation_note() + "</main>")
    if hasattr(page_cost, "build_current"):
        cost = page_cost.build_current(F["D"])  # whole-project census from gen/pa_census.json
    else:
        cost = reframe("cost.html", open(os.path.join(PM, "cost.html")).read())
    return F["D"], F["commits_per_day"], F["prompts_per_day"], road, cost


D, commits_per_day, prompts_per_day, road_html, cost_html = load_live() if LIVE else load_frozen()

pages = {
    "index.html": page_index.build(D),
    "author.html": page_author.build(D),
    "timeline.html": page_timeline.build(D, commits_per_day, prompts_per_day),
    "road-to-60.html": road_html,
    "native.html": page_native.build(D),
    "boundaries.html": page_boundaries.build(D),
    "difficulties.html": page_difficulties.build(D),
    "frontier.html": page_frontier.build(D),
    "agentic.html": page_agentic.build(D),
    "cost.html": cost_html,
}
for old, target, title, why in REDIRECTS:
    pages[old] = redirect(old, target, title, why)
for name, html_ in pages.items():
    with open(os.path.join(OUT, name), "w") as f:
        f.write(html_)
    print(name, len(html_))
print("inputs:", "live scratchpad census" if LIVE else "frozen-new3ds.json + re-wrapped road-to-60/cost")
print("D:", json.dumps({k: v for k, v in D.items() if k != "headline_rows"}))
