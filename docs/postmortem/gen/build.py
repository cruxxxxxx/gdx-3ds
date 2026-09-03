import json, os, sys, collections
sys.path.insert(0, os.path.dirname(__file__))
from framework import *
import page_index, page_timeline, page_road, page_difficulties, page_frontier, page_agentic, page_cost

SCRATCH = os.environ.get("PM_SCRATCH", os.path.join(os.path.dirname(__file__), "scratch"))
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(SCRATCH, "pm", "out")
os.makedirs(OUT, exist_ok=True)

C = json.load(open(os.path.join(SCRATCH, "census", "census.json")))
P = json.load(open(os.path.join(SCRATCH, "page-data.json")))

tot = C["totals"]["combined"]
tokens_total = tot["input"] + tot["output"] + tot["cache_creation"] + tot["cache_read"]
costA = C["cost_usd"]["A"]; costB = C["cost_usd"]["B"]
tA = costA["total"] if "total" in costA else sum(v["combined"] for v in costA["per_model"].values())
tB = costB["total"] if "total" in costB else sum(v["combined"] for v in costB["per_model"].values())

# commits per day touching the 3DS port (git log dump, local time)
commits_per_day = collections.Counter()
for line in open(os.path.join(SCRATCH, "git-3ds-log.txt")):
    if line[:4] == "2026":
        commits_per_day[line[:10]] += 1
prompts_per_day = collections.Counter()
for line in open(os.path.join(SCRATCH, "user-prompts.txt")):
    if line[:4] == "2026" and "Image" not in line:
        prompts_per_day[line[:10]] += 1

S = P["sessions"]
import statistics as st
def stats(f):
    f = sorted(f); n = len(f)
    return st.median(f), f[int(0.1 * n)], 100 * sum(1 for x in f if x >= 59.5) / n
r5 = stats(S[-1]["fps"]); r1 = stats(S[0]["fps"])

headline_rows = [
    ["08-14 first race (emulator)", "12–20", "–", "–"],
    ["08-21 first hardware run", "48–50", "–", "–"],
    ["08-28 release-grade build", "~51", "~40 (crowds, MINIMAL)", "–"],
] + [[f"09-0{2 if i < 4 else 3} {s['name']}: {s['sub']}", f"{stats(s['fps'])[0]:.1f}", f"{stats(s['fps'])[1]:.1f}", f"{stats(s['fps'])[2]:.0f}%"] for i, s in enumerate(S)]

D = {
    "subagents": f"{C['subagents']['subagent_transcripts']}",
    "workflow_agents": f"{C['subagents']['workflow_agent_transcripts']:,}",
    "worktrees": "96",
    "branches": "98",
    "workflows": f"{C['subagents']['workflow_runs']}",
    "prompts": f"{C['human_prompts']['count_strict']}",
    "active_days": f"{C['date_range']['active_days']}",
    "sd_mentions": "36",
    "memory_lines": "591",
    "patches": "48",
    "patches_decomp": "10",
    "patches_lus": "38",
    "loc_port3ds": "15,039",
    "loc_all": "25,009",
    "commits_3ds": "250",
    "final_median": f"{r5[0]:.1f}",
    "final_p10": f"{r5[1]:.1f}",
    "final_cap": f"{r5[2]:.0f}%",
    "tokens_total_h": f"{tokens_total/1e9:.2f} B",
    "cost_a_h": f"${tA:,.0f}",
    "cost_b_h": f"${tB:,.0f}",
    "headline_rows": headline_rows,
}

pages = {
    "index.html": page_index.build(D),
    "timeline.html": page_timeline.build(D, commits_per_day, prompts_per_day),
    "road-to-60.html": page_road.build(D, P),
    "difficulties.html": page_difficulties.build(D),
    "frontier.html": page_frontier.build(D),
    "agentic.html": page_agentic.build(D),
    "cost.html": page_cost.build(D, C),
}
for name, html_ in pages.items():
    with open(os.path.join(OUT, name), "w") as f:
        f.write(html_)
    print(name, len(html_))
print("D:", json.dumps({k: v for k, v in D.items() if k != "headline_rows"}))
