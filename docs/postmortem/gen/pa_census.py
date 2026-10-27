"""Token census over every Claude Code transcript for this project, all phases.

    python3 pa_census.py [--until YYYY-MM-DD]      writes pa_census.json next to this file

Re-implements the first-phase census method (source-census.md) so it can be re-run:
  * transcripts: every *.jsonl under the project's Claude Code directories (top-level session files are
    the orchestrator, subagents/agent-*.jsonl are Agent-tool subagents, subagents/workflows/wf_*/agent-*.jsonl
    are deep-research workflow agents; journal.jsonl files carry no usage and are skipped);
  * one API call per distinct assistant message id; streamed chunks of the same id are merged by taking the
    max of each usage field (output_tokens grows across chunks), so nothing is double counted;
  * dollars = published API list prices x those counts; scenario B bills every cache write at the 1-hour rate
    (2x input, the TTL the first-phase page assumed), scenario A at the 5-minute rate (1.25x input), and
    "logged" uses each record's own 1 h / 5 min split (usage.cache_creation) where it carries one.
Days are UTC. Phases: first 2026-08-11..09-03, second 09-07..10-19, after v1.0.0 10-20 onward.
"""
import json, os, sys, glob, collections, datetime

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECTS = os.path.expanduser("~/.claude/projects")
# The repo lived at ~/Code/G-Diffuser through v1.0.0 and moved to ~/Code/gdx-3ds on 2026-10-20; worktree
# subagents write into their parent session's directory, so these two directories are the whole project.
PROJECT_DIRS = ["-Users-mike-Code-G-Diffuser", "-Users-mike-Code-gdx-3ds"]

# $/MTok: input, output, cache read. Cache writes derive from input (2x for 1 h, 1.25x for 5 min).
PRICES = {
    "claude-fable-5-1": (10, 50, 0.25),
    "claude-fable-5": (10, 50, 1.00),
    "claude-opus-5-5": (4, 20, 0.20),
    "claude-opus-5": (5, 25, 0.50),
    "claude-opus-4-8": (5, 25, 0.50),
    "claude-sonnet-5": (2, 10, 0.20),
    "claude-haiku-4-5-20251001": (1, 5, 0.10),
}
PHASES = [("first", "2026-08-11", "2026-09-03"), ("second", "2026-09-07", "2026-10-19"), ("after_v1", "2026-10-20", "2099-12-31")]
FIELDS = ("input", "output", "cache_creation", "cache_read")
SPLIT = ("cw_1h", "cw_5m")


def norm_model(m):
    return (m or "<synthetic>").replace("[1m]", "")


def cost(model, u, write_mult):
    i, o, r = PRICES.get(model, (0, 0, 0))
    if write_mult is None:
        # as logged: the record's own 1 h / 5 min split where it has one; writes without a split at the 1 h rate
        split = u["cw_1h"] + u["cw_5m"]
        writes = u["cw_1h"] * 2 + u["cw_5m"] * 1.25 + max(u["cache_creation"] - split, 0) * 2
    else:
        writes = u["cache_creation"] * write_mult
    return (u["input"] * i + u["output"] * o + writes * i + u["cache_read"] * r) / 1e6


def scope_of(path):
    if "/workflows/" in path:
        return "workflow"
    if "/subagents/" in path:
        return "subagent"
    return "main"


def phase_of(day):
    for name, a, b in PHASES:
        if a <= day <= b:
            return name
    return "gap"


def is_human_prompt(r):
    if r.get("type") != "user" or r.get("isMeta") or r.get("isCompactSummary"):
        return None
    c = (r.get("message") or {}).get("content")
    if isinstance(c, str):
        text = c
    elif isinstance(c, list):
        if any(isinstance(x, dict) and x.get("type") == "tool_result" for x in c):
            return None
        text = "".join(x.get("text", "") for x in c if isinstance(x, dict) and x.get("type") == "text")
    else:
        return None
    t = text.strip()
    if not t or t.startswith("<") or t.startswith("[Request interrupted") or t.startswith("Caveat:"):
        return None
    return t


def main():
    until = sys.argv[sys.argv.index("--until") + 1] if "--until" in sys.argv else "2099-12-31"
    calls = {}           # message id -> {model, scope, day, usage}
    prompts = []         # (day, chars)
    agents = collections.Counter()
    files = 0
    for d in PROJECT_DIRS:
        for path in glob.glob(os.path.join(PROJECTS, d, "**", "*.jsonl"), recursive=True):
            if path.endswith("journal.jsonl"):
                continue
            files += 1
            scope = scope_of(path)
            first_day = None
            for line in open(path, errors="replace"):
                try:
                    r = json.loads(line)
                except ValueError:
                    continue
                ts = r.get("timestamp") or ""
                day = ts[:10]
                if not day or day > until:
                    continue
                first_day = first_day or day
                if scope == "main":
                    t = is_human_prompt(r)
                    if t:
                        prompts.append((day, len(t)))
                if r.get("type") != "assistant":
                    continue
                m = r.get("message") or {}
                u = m.get("usage")
                mid = m.get("id") or r.get("uuid")
                if not u or not mid:
                    continue
                vals = {"input": u.get("input_tokens", 0) or 0, "output": u.get("output_tokens", 0) or 0,
                        "cache_creation": u.get("cache_creation_input_tokens", 0) or 0,
                        "cache_read": u.get("cache_read_input_tokens", 0) or 0,
                        "cw_1h": (u.get("cache_creation") or {}).get("ephemeral_1h_input_tokens", 0) or 0,
                        "cw_5m": (u.get("cache_creation") or {}).get("ephemeral_5m_input_tokens", 0) or 0}
                cur = calls.get(mid)
                if cur is None:
                    calls[mid] = {"model": norm_model(m.get("model")), "scope": scope, "day": day, "u": vals}
                else:
                    for k in vals:
                        cur["u"][k] = max(cur["u"][k], vals[k])
            if scope != "main" and first_day:
                agents[(scope, phase_of(first_day))] += 1

    def blank():
        return {k: 0 for k in FIELDS + SPLIT} | {"turns": 0, "cost_A": 0.0, "cost_B": 0.0, "cost_logged": 0.0}

    def add(acc, c):
        for k in FIELDS + SPLIT:
            acc[k] += c["u"][k]
        acc["turns"] += 1
        acc["cost_logged"] += cost(c["model"], c["u"], None)
        acc["cost_A"] += cost(c["model"], c["u"], 1.25)
        acc["cost_B"] += cost(c["model"], c["u"], 2.0)

    total, per_phase, per_model, per_scope, per_day = blank(), collections.defaultdict(blank), collections.defaultdict(blank), collections.defaultdict(blank), collections.defaultdict(blank)
    per_phase_model = collections.defaultdict(blank)
    for c in calls.values():
        ph = phase_of(c["day"])
        add(total, c); add(per_phase[ph], c); add(per_model[c["model"]], c); add(per_scope[c["scope"]], c); add(per_day[c["day"]], c)
        add(per_phase_model[ph + "|" + c["model"]], c)
    pd = collections.Counter(d for d, _ in prompts)
    out = {
        "_method": __doc__.strip().splitlines()[2:],
        "prices_per_mtok": {k: {"input": v[0], "output": v[1], "cache_write_1h": v[0] * 2, "cache_read": v[2]} for k, v in PRICES.items()},
        "project_dirs": PROJECT_DIRS, "files_scanned": files, "until": until,
        "generated_at": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d %H:%M UTC"),
        "date_range": [min(per_day), max(per_day)], "active_days": len(per_day),
        "totals": total, "per_phase": dict(per_phase), "per_model": dict(per_model), "per_scope": dict(per_scope),
        "per_phase_model": dict(per_phase_model),
        "per_day": {d: per_day[d] | {"human_prompts": pd.get(d, 0)} for d in sorted(per_day)},
        "agents_started": {f"{s}|{p}": n for (s, p), n in sorted(agents.items())},
        "human_prompts": {"count": len(prompts), "chars": sum(n for _, n in prompts),
                          "per_phase": dict(collections.Counter(phase_of(d) for d, _ in prompts))},
    }
    json.dump(out, open(os.path.join(HERE, "pa_census.json"), "w"), indent=1, sort_keys=True)
    print(json.dumps({"files": files, "calls": total["turns"], "tokens": sum(total[k] for k in FIELDS), "cost_B": round(total["cost_B"]),
                      "per_phase": {p: [sum(v[k] for k in FIELDS), round(v["cost_A"]), round(v["cost_B"]), round(v["cost_logged"]), v["turns"], v["cache_creation"], v["cw_1h"], v["cw_5m"]] for p, v in per_phase.items()},
                      "prompts": out["human_prompts"], "agents": out["agents_started"]}, indent=1))


if __name__ == "__main__":
    main()
