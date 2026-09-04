from framework import *
import json


def money(v):
    return f"${v:,.0f}"


def big(v):
    if v >= 1e9:
        return f"{v/1e9:.2f} B"
    if v >= 1e6:
        return f"{v/1e6:.1f} M"
    if v >= 1e3:
        return f"{v/1e3:.0f} K"
    return f"{v:.0f}"


def build(D, C):
    tot = C["totals"]["combined"]
    grp = C["totals"]["per_group"]
    pm = C["totals"]["per_model"]
    costA = C["cost_usd"]["A"]; costB = C["cost_usd"]["B"]
    tA = costA["total"] if "total" in costA else sum(v["combined"] for v in costA["per_model"].values())
    tB = costB["total"] if "total" in costB else sum(v["combined"] for v in costB["per_model"].values())
    days = C["per_day"]
    agents = [a for a in C["subagents"]["agents"] if a.get("kind") == "subagent" or a.get("attribution_agent") != "workflow-subagent"]
    subs = [a for a in C["subagents"]["agents"] if a.get("description")]

    def acost(a, key="cost_A"):
        if key in a:
            return a[key]
        # fallback: price it here at scenario A
        p = {"claude-opus-4-8": (5, 25), "claude-opus-5": (5, 25), "claude-fable-5": (5, 25), "claude-fable-5-1": (5, 25),
             "claude-haiku-4-5-20251001": (1, 5)}
        m = (a.get("model") or "").split(",")[-1]
        i, o = p.get(m, (0, 0))
        return (a["input"] * i + a["output"] * o + a["cache_creation"] * i * 1.25 + a["cache_read"] * i * 0.1) / 1e6

    b = []
    b.append('<p class="eyebrow">08 · Cost</p><h1>What it cost to build</h1>')
    b.append('<p class="lede">Every API call the project made is in the session transcripts on this machine. This page counts them. '
             'Token counts are exact. Dollar figures apply Anthropic\'s published API list prices to those counts; '
             'the two scenarios differ only in the prompt-cache write duration.</p>')
    b.append(tiles([
        (big(tot["input"] + tot["output"] + tot["cache_creation"] + tot["cache_read"]), "tokens processed, all sessions, agents and workflows", True),
        (big(tot["output"]), "output tokens written by the models", True),
        (f"{tot['turns']:,}", "API calls (assistant turns) after de-duplication", False),
        (money(tA), "at published list prices, 5-minute cache writes", True),
        (money(tB), "same prices with 1-hour cache writes, the TTL these sessions used", False),
        (f"{C['subagents']['subagent_transcripts']}", "background subagents launched by the Agent tool", False),
        (f"{C['subagents']['workflow_agent_transcripts']:,}", "agents inside 10 deep-research workflow runs", False),
        (f"{C['human_prompts']['count_strict']}", "human prompts, 19.7 K characters typed in total", False),
    ]))

    b.append('<h2>Prices used</h2>')
    b.append('<p>Published Claude API list prices (platform.claude.com, pricing page, fetched 2026-09-03), in dollars per million tokens. '
             'Scenario A bills cache writes at the 5-minute rate (1.25× input); scenario B at the 1-hour rate (2× input), which is the '
             'cache TTL Claude Code used in these sessions. Cache reads are 0.1× input on every model except Fable 5.1, where they are 0.025×.</p>')
    b.append(table(["Model", "Input", "Output", "Cache write 5 min", "Cache write 1 h", "Cache read"], [
        ["Claude Fable 5.1", "$10", "$50", "$12.50", "$20", "$0.25"],
        ["Claude Fable 5", "$10", "$50", "$12.50", "$20", "$1.00"],
        ["Claude Opus 5 / Opus 4.8", "$5", "$25", "$6.25", "$10", "$0.50"],
        ["Claude Haiku 4.5", "$1", "$5", "$1.25", "$2", "$0.10"],
    ], num_cols=(1, 2, 3, 4, 5)))
    b.append('<div class="assume">The user\'s Claude subscription is not billed per token; these figures are what the same usage would have cost '
             'on the API. Synthetic records (API errors, interrupts) carry zero usage. Web searches inside the deep-research workflows '
             '($10 per 1,000 on the API) are not included; there were 899 of them, about $9.</div>')

    b.append('<h2>Where the tokens went</h2>')
    cats = ["Orchestrator|(main session)", "Subagents|(Agent tool)", "Workflow agents|(deep-research)"]
    keys = ["main", "subagent", "workflow"]
    svg = stacked_bars(cats, [
        ("cache read", "--c8", [grp[k]["cache_read"] / 1e9 for k in keys]),
        ("cache write", "--c6", [grp[k]["cache_creation"] / 1e9 for k in keys]),
        ("output", "--c1", [grp[k]["output"] / 1e9 for k in keys]),
        ("input (uncached)", "--c2", [grp[k]["input"] / 1e9 for k in keys]),
    ], ylabel="billions of tokens", dec=2)
    b.append(figure(svg, "<b>Cache reads dominate volume, output dominates cost.</b> Of 3.16 billion tokens, 96% were prompt-cache reads, re-read at a tenth "
                    "(Fable 5.1: a fortieth) of the input price. The 13.2 million output tokens, the text and code actually written, are the largest line on the bill.",
                    legend([("cache read", "--c8"), ("cache write", "--c6"), ("output", "--c1"), ("input, uncached", "--c2")])))

    b.append(table(["Scope", "Input", "Output", "Cache write", "Cache read", "API calls", "Cost A", "Cost B"], [
        [{"main": "Orchestrator session", "subagent": "Subagents", "workflow": "Workflow agents"}[k],
         f"{grp[k]['input']:,}", f"{grp[k]['output']:,}", f"{grp[k]['cache_creation']:,}", f"{grp[k]['cache_read']:,}", f"{grp[k]['turns']:,}",
         money(sum(costA["per_model"][m][k] for m in costA["per_model"])), money(sum(costB["per_model"][m][k] for m in costB["per_model"]))]
        for k in keys
    ] + [["<b>Total</b>", f"<b>{tot['input']:,}</b>", f"<b>{tot['output']:,}</b>", f"<b>{tot['cache_creation']:,}</b>", f"<b>{tot['cache_read']:,}</b>",
          f"<b>{tot['turns']:,}</b>", f"<b>{money(tA)}</b>", f"<b>{money(tB)}</b>"]], num_cols=(1, 2, 3, 4, 5, 6, 7)))

    b.append('<h2>By model</h2>')
    b.append('<p>Three generations of model did the work. Fable 5 carried the first three weeks; Opus 4.8 carried the week the Fable credits '
             'were out (08-15 to 08-20, when the fog fix landed); Fable 5.1 carried the final week and the multi-core push.</p>')
    models = [m for m in pm if m != "<synthetic>"]
    rows = []
    for m in sorted(models, key=lambda m: -costA["per_model"][m]["combined"]):
        v = pm[m]["combined"]
        rows.append([f"<code>{esc(m)}</code>", f"{v['turns']:,}", f"{v['output']:,}", f"{v['cache_read']:,}",
                     money(costA["per_model"][m]["combined"]), money(costB["per_model"][m]["combined"])])
    b.append(table(["Model", "API calls", "Output tokens", "Cache-read tokens", "Cost A", "Cost B"], rows, num_cols=(1, 2, 3, 4, 5)))

    b.append('<h2>Spend by day</h2>')
    dl = [d["day"] for d in days]
    svg = day_bars(dl, [d["cost_A"] for d in days], ylabel="scenario A, USD per day", fmt=lambda v: f"${v:,.0f}",
                   marks={"2026-08-14": "first race", "2026-08-21": "hardware + audio", "2026-08-27": "fleet drop", "2026-09-02": "LOCKED-60"})
    b.append(figure(svg, "<b>Four days account for most of the spend.</b> 08-14 (the boot-to-race sprint and the first fleet of eight), "
                    "08-21 (the loadblock breakthrough, first hardware run, the audio afternoon), 08-27 (the five-agent fleet drop) and "
                    "09-02 (the LOCKED-60 campaign). Every gap is a credit pause."))
    b.append(table(["Day", "API calls", "Human prompts", "Subagents started", "Workflow agents", "Output tokens", "Cost A", "Cost B"], [
        [d["day"], f"{d['turns']:,}", d["human_prompts"], d["subagents_started"], d["workflow_agents_started"], f"{d['output']:,}",
         money(d["cost_A"]), money(d["cost_B"])] for d in days], num_cols=(1, 2, 3, 4, 5, 6, 7)))

    b.append('<h2>Subagents</h2>')
    b.append(f'<p>{C["subagents"]["subagent_transcripts"]} background agents were launched with the Agent tool '
             f'({C["subagents"]["subagents_launched_from"].get("main", 0)} by the orchestrator, '
             f'{C["subagents"]["subagents_launched_from"].get("subagent", 0)} by other agents), and 10 deep-research workflows spawned '
             f'{C["subagents"]["workflow_agent_transcripts"]:,} short-lived search, fetch and verification agents. '
             'The table below is the twenty most expensive background agents; between them they are most of the subagent bill.</p>')
    top = sorted(subs, key=lambda a: -acost(a))[:20]
    b.append(table(["Started", "Agent", "Model", "Turns", "Output", "Cost A"], [
        [a["first_ts"][:16].replace("T", " "), esc(a["description"]), f"<code>{esc((a.get('model') or '').split(',')[-1].replace('claude-', ''))}</code>",
         f"{a['turns']:,}", f"{a['output']:,}", money(acost(a))] for a in top], num_cols=(3, 4, 5)))
    b.append('<p>The two 900-turn agents of 08-14 (GPU profiling telemetry and the stereo foundation) were left running through the '
             'first credit pause and each re-read a very large context hundreds of times. They are the clearest cost lesson of the '
             'project: a long-lived agent with a big context is expensive per turn, and an agent that has finished its brief should be stopped.</p>')

    b.append('<h2>Tool calls</h2>')
    tc = C["tools_combined"]
    rows = sorted(tc.items(), key=lambda kv: -kv[1])[:12]
    b.append(figure(hbar([(k, v) for k, v in rows], fmt=lambda v: f"{v:,.0f}"),
                    "<b>15,085 shell commands.</b> The project was driven through Bash: builds, patches, emulator runs, log analysis, SD-card copies. "
                    "1,282 Edits and 196 Writes touched 462 distinct files. 333 shell commands contained <code>git commit</code>. "
                    "1,907 web fetches and 899 web searches belong almost entirely to the deep-research workflows."))

    b.append('<h2>Reconciling with what /usage and /stats show</h2>')
    b.append('<p>The user\'s impression was that the project cost more than this page says. Three things explain the gap, and one '
             'real omission was checked and found small.</p>')
    b.append(table(["Check", "Finding"], [
        ["<b>Claude Code\'s own stats file counts every streamed record.</b>", "Each assistant message is written to the transcript once per content block (2 to 5 records with the same message id). "
         "<code>~/.claude/stats-cache.json</code> sums them all: its 08-13 Fable figure, 1,730,401, equals this census\'s <i>un-deduplicated</i> input+output sum for that day exactly. "
         "Priced that way the project reads as 5.94 B tokens and <b>$7,961</b>. The API bills per request, so the deduplicated 3.17 B tokens and $4,280–$4,974 is the honest figure."],
        ["<b>/usage shows plan utilisation, not dollars.</b>", "The Fable weekly allowance was exhausted three times (08-14, 08-22, 08-28). Reaching a cap says nothing about the dollar value of the tokens under it."],
        ["<b>Other projects shared the same allowance.</b>", "Between 08-15 and 08-28 the same account ran Opus sessions on five other projects (a Unity game, an image folder, three code repos: 447 prompts in the history file). Their usage sits in the same /usage bars and the same stats file, and none of it is in this census."],
        ["<b>Missing transcripts.</b>", "The prompt history lists nine G-Diffuser sessions; four are on disk. The five absent ones are each a single <code>/resume</code> that went nowhere."],
        ["<b>Calls the transcript does not log.</b>", "Four context compactions (contexts of 0.5–1.0 M tokens each, about $4 in total at Fable rates), session-title and tool-summary side calls on Haiku (cents), and 899 web searches inside the research workflows (about $9 at API rates). Retries after 529 overloads are not billed."],
    ]))

    b.append('<h2>What a human team would have cost</h2>')
    b.append('<p>A rough comparison, not a quote. The scope a team would face is the scope this project faced: a new fixed-function GPU '
             'backend, a 32-bit sweep across three codebases, a ROM-side audio driver port, an ARM11 performance grind to native 60 on a '
             'software display-list interpreter, stereo, a multi-core render pipeline, lifecycle, menus and packaging. About 20,000 lines '
             'of low-level code plus the research and the hardware loop.</p>')
    b.append('<h3>Reference points</h3>')
    b.append('<ul>')
    b.append('<li><b>sm64 on 3DS.</b> Hobbyists, one core developer plus contributors. Playable in months, 60 fps via 30 Hz logic and interpolation, refined over years.</li>')
    b.append('<li><b>Ship of Harkinian console ports</b> (Wii U, Switch). One or two skilled volunteers, several months each, on top of an existing PC runtime, with a shader-capable GPU and no stereo.</li>')
    b.append('<li><b>Professional retro-port studios.</b> Five to eight people for six to twelve months on ports with far more headroom than an 804 MHz ARM11 driving a PICA200.</li>')
    b.append('</ul>')
    b.append('<h3>Estimate to reach this state</h3>')
    b.append(table(["Team", "Calendar", "Effort", "Cost at US rates"], [
        ["Two senior engineers (graphics/runtime, systems/audio/perf) plus part-time QA on hardware", "5–8 months", "12–18 person-months", "$250k–$450k"],
        ["One very senior generalist, full time", "9–14 months", "9–14 person-months", "$180k–$300k"],
        ["One skilled hobbyist, evenings and weekends", "12–24 months", "1,000–1,500 hours", "$0 paid, a year of unpaid labour"],
    ], num_cols=(1, 2, 3)))
    b.append('<p>Basis: $150–200 per hour contractor rates or $20–25k per month loaded per senior engineer; low-level port work lands at '
             '100–200 shipped lines per engineer-day once debugging and hardware loops are counted, which puts 20,000 lines at six or more '
             'person-months before the performance grind. A human team would spend its time in the same places the agents did (the 32-bit '
             'address-space heuristics, the fog LUT, the HUD backstop, the audio driver, then months of profiler-driven grinding), would very '
             'likely have shipped 30 Hz plus interpolation first and deferred stereo, and might never have attempted the core-2 render thread.</p>')
    b.append('<h3>Side by side</h3>')
    b.append(table(["", "This project", "Human team, mid estimate"], [
        ["Calendar", "24 days, 15 active", "about 6 months"],
        ["Cost", money(tA) + "–" + money(tB) + " API-equivalent", "about $350k"],
        ["Human time", "one person, about 15 days of direction and SD-card loops, no code written", "a full-time team"],
        ["Polish delivered", "playable, 60 fps median, stereo, lifecycle; known backlog (tunnel roof, boot-audio underrun, LOD thresholds)", "a QA pass and the backlog closed"],
    ]))
    b.append('<div class="note">Ratio: roughly 70–80× cheaper and 7–8× faster, with the caveats that a professional team ships more polish '
             'and that the hobbyist route is free if the year is not counted.</div>')

    b.append('<h2>Cost per outcome</h2>')
    b.append('<p>Some ways to read the bottom line, at scenario A (published prices, 5-minute cache writes):</p>')
    per_prompt = tA / C["human_prompts"]["count_strict"]
    b.append(table(["Measure", "Value"], [
        ["Per human prompt", money(per_prompt)],
        ["Per active day (15 days)", money(tA / 15)],
        ["Per commit touching the 3DS port (" + D["commits_3ds"] + ")", money(tA / int(D["commits_3ds"].replace(",", "")))],
        ["Per line of port code, patches included (" + D["loc_all"] + " lines)", f"${tA / int(D['loc_all'].replace(',', '')):.2f}"],
        ["Per fps of median gained on hardware (from ~12–20 in the first emulator race to " + D["final_median"] + ")", money(tA / (float(D["final_median"]) - 16))],
    ], num_cols=(1,)))
    b.append('<div class="note">For scale: a single experienced console-port engineer at a modest contractor rate would bill more than the whole project '
             'in its first week. The point is not that the machine was cheap. It is that the machine was cheap enough to try five ideas '
             'and shelve three of them on evidence.</div>')
    return page("cost.html", "What It Cost", "".join(b))
