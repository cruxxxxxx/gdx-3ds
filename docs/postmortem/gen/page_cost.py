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

    def acost(a, key="cost_B"):
        if key in a:
            return a[key]
        # fallback: price it here at the 1-hour cache-write rate
        p = {"claude-opus-4-8": (5, 25), "claude-opus-5": (5, 25), "claude-fable-5": (5, 25), "claude-fable-5-1": (5, 25),
             "claude-haiku-4-5-20251001": (1, 5)}
        m = (a.get("model") or "").split(",")[-1]
        i, o = p.get(m, (0, 0))
        return (a["input"] * i + a["output"] * o + a["cache_creation"] * i * 2 + a["cache_read"] * i * 0.1) / 1e6

    b = []
    b.append('<p class="eyebrow">11 · Cost</p><h1>What it cost to build</h1>')
    b.append('<div class="note"><b>This page prices the first phase only</b> (2026-08-11 to 09-03, the sprint that reached v0.9.0 on '
             'the New 3DS). A second phase ran 2026-09-07 to 10-19 and added Old 3DS support, a native renderer and the Expansion '
             'Kit; it is not priced below. See "What the second phase would add" at the end of this page for why, and for the one '
             'honest number we do have for it.</div>')
    b.append('<p class="lede">Every API call the first phase made is in the session transcripts this page counts. '
             'Token counts are exact. Dollar figures apply Anthropic\'s published API list prices to those counts; '
             'cache writes are billed at the 1-hour rate, the cache TTL Claude Code used in these sessions.</p>')
    b.append(tiles([
        (big(tot["input"] + tot["output"] + tot["cache_creation"] + tot["cache_read"]), "tokens processed, all sessions, agents and workflows", True),
        (big(tot["output"]), "output tokens written by the models", True),
        (f"{tot['turns']:,}", "API calls (assistant turns) after de-duplication", False),
        (money(tB), "at published API list prices, 1-hour cache writes (the TTL these sessions used)", True),
        (f"{C['subagents']['subagent_transcripts']}", "background subagents launched by the Agent tool", False),
        (f"{C['subagents']['workflow_agent_transcripts']:,}", "agents inside 10 deep-research workflow runs", False),
        (f"{C['human_prompts']['count_strict']}", "human prompts, 19.8 K characters typed in total", False),
    ]))

    b.append('<h2>Prices used</h2>')
    b.append('<p>Published Claude API list prices (platform.claude.com, pricing page, fetched 2026-09-03), in dollars per million tokens. '
             'Cache writes are billed at the 1-hour rate (2× input), the cache TTL Claude Code used in these sessions; billed at the '
             '5-minute rate (1.25× input) instead, the total would be ' + money(tA) + '. Cache reads are 0.1× input on every model except Fable 5.1, where they are 0.025×.</p>')
    b.append(table(["Model", "Input", "Output", "Cache write (1 h)", "Cache read"], [
        ["Claude Fable 5.1", "$10", "$50", "$20", "$0.25"],
        ["Claude Fable 5", "$10", "$50", "$20", "$1.00"],
        ["Claude Opus 5 / Opus 4.8", "$5", "$25", "$10", "$0.50"],
        ["Claude Haiku 4.5", "$1", "$5", "$2", "$0.10"],
    ], num_cols=(1, 2, 3, 4)))
    b.append('<div class="assume">The user\'s Claude subscription is not billed per token; these figures are what the same usage would have cost '
             'on the API. Synthetic records (API errors, interrupts) carry zero usage. Web searches ($10 per 1,000 on the API) are not '
             'included; there were 900 of them, about $9.</div>')

    b.append('<h2>Where the tokens went</h2>')
    cats = ["Orchestrator|(main session)", "Subagents|(Agent tool)", "Workflow agents|(deep-research)"]
    keys = ["main", "subagent", "workflow"]
    svg = stacked_bars(cats, [
        ("cache read", "--c8", [grp[k]["cache_read"] / 1e9 for k in keys]),
        ("cache write", "--c6", [grp[k]["cache_creation"] / 1e9 for k in keys]),
        ("output", "--c1", [grp[k]["output"] / 1e9 for k in keys]),
        ("input (uncached)", "--c2", [grp[k]["input"] / 1e9 for k in keys]),
    ], ylabel="billions of tokens", dec=2)
    b.append(figure(svg, "<b>Cache reads dominate volume, output dominates cost.</b> Of 3.17 billion tokens, 96% were prompt-cache reads, re-read at a tenth "
                    "(Fable 5.1: a fortieth) of the input price. The 13.2 million output tokens, the text and code actually written, are the largest line on the bill.",
                    legend([("cache read", "--c8"), ("cache write", "--c6"), ("output", "--c1"), ("input, uncached", "--c2")])))

    b.append(table(["Scope", "Input", "Output", "Cache write", "Cache read", "API calls", "Cost"], [
        [{"main": "Orchestrator session", "subagent": "Subagents", "workflow": "Workflow agents"}[k],
         f"{grp[k]['input']:,}", f"{grp[k]['output']:,}", f"{grp[k]['cache_creation']:,}", f"{grp[k]['cache_read']:,}", f"{grp[k]['turns']:,}",
         money(sum(costB["per_model"][m][k] for m in costB["per_model"]))]
        for k in keys
    ] + [["<b>Total</b>", f"<b>{tot['input']:,}</b>", f"<b>{tot['output']:,}</b>", f"<b>{tot['cache_creation']:,}</b>", f"<b>{tot['cache_read']:,}</b>",
          f"<b>{tot['turns']:,}</b>", f"<b>{money(tB)}</b>"]], num_cols=(1, 2, 3, 4, 5, 6)))

    b.append('<h2>By model</h2>')
    b.append('<p>Three generations of model did the work. Fable 5 carried the first three weeks; Opus 4.8 carried the week the Fable credits '
             'were out (08-15 to 08-20, when the fog fix landed); Fable 5.1 carried the final week and the multi-core push.</p>')
    models = [m for m in pm if m != "<synthetic>"]
    rows = []
    for m in sorted(models, key=lambda m: -costB["per_model"][m]["combined"]):
        v = pm[m]["combined"]
        rows.append([f"<code>{esc(m)}</code>", f"{v['turns']:,}", f"{v['output']:,}", f"{v['cache_read']:,}",
                     money(costB["per_model"][m]["combined"])])
    b.append(table(["Model", "API calls", "Output tokens", "Cache-read tokens", "Cost"], rows, num_cols=(1, 2, 3, 4)))

    b.append('<h2>Spend by day</h2>')
    dl = [d["day"] for d in days]
    svg = day_bars(dl, [d["cost_B"] for d in days], ylabel="USD per day", fmt=lambda v: f"${v:,.0f}",
                   marks={"2026-08-14": "first race", "2026-08-21": "hardware + audio", "2026-08-27": "fleet drop", "2026-09-02": "LOCKED-60"})
    b.append(figure(svg, "<b>Four days account for most of the spend.</b> 08-14 (the boot-to-race sprint and the first fleet of eight), "
                    "08-21 (the loadblock breakthrough, first hardware run, the audio afternoon), 08-27 (the five-agent fleet drop) and "
                    "09-02 (the LOCKED-60 campaign). Every gap is a credit pause."))
    b.append(table(["Day", "API calls", "Human prompts", "Subagents started", "Workflow agents", "Output tokens", "Cost"], [
        [d["day"], f"{d['turns']:,}", d["human_prompts"], d["subagents_started"], d["workflow_agents_started"], f"{d['output']:,}",
         money(d["cost_B"])] for d in days], num_cols=(1, 2, 3, 4, 5, 6)))

    b.append('<h2>Subagents</h2>')
    b.append(f'<p>{C["subagents"]["subagent_transcripts"]} background agents were launched with the Agent tool '
             f'({C["subagents"]["subagents_launched_from"].get("main", 0)} by the orchestrator, '
             f'{C["subagents"]["subagents_launched_from"].get("subagent", 0)} by other agents), and 10 deep-research workflows spawned '
             f'{C["subagents"]["workflow_agent_transcripts"]:,} short-lived search, fetch and verification agents. '
             'The table below is the twenty most expensive background agents; between them they are most of the subagent bill.</p>')
    top = sorted(subs, key=lambda a: -acost(a))[:20]
    b.append(table(["Started", "Agent", "Model", "Turns", "Output", "Cost"], [
        [a["first_ts"][:16].replace("T", " "), esc(a["description"]), f"<code>{esc((a.get('model') or '').split(',')[-1].replace('claude-', ''))}</code>",
         f"{a['turns']:,}", f"{a['output']:,}", money(acost(a))] for a in top], num_cols=(3, 4, 5)))
    b.append('<p>The two 900-turn agents of 08-14 (GPU profiling telemetry and the stereo foundation) were left running through the '
             'first credit pause and each re-read a very large context hundreds of times. They are the clearest cost lesson of the '
             'project: a long-lived agent with a big context is expensive per turn, and an agent that has finished its brief should be stopped.</p>')

    b.append('<h2>Tool calls</h2>')
    tc = C["tools_combined"]
    rows = sorted(tc.items(), key=lambda kv: -kv[1])[:12]
    bash_n, edit_n, write_n = tc.get("Bash", 0), tc.get("Edit", 0), tc.get("Write", 0)
    fetch_n, search_n = tc.get("WebFetch", 0), tc.get("WebSearch", 0)
    # distinct-files-edited (469) and the git-commit-containing-Bash-call count (334) are not broken out in
    # tools_combined; they come straight from source-census.md §4/§7, which this paragraph must stay equal to.
    b.append(figure(hbar([(k, v) for k, v in rows], fmt=lambda v: f"{v:,.0f}"),
                    f"<b>{bash_n:,} shell commands.</b> The project was driven through Bash: builds, patches, emulator runs, log analysis, SD-card copies. "
                    f"{edit_n:,} Edits and {write_n:,} Writes touched 469 distinct files. 334 shell commands contained <code>git commit</code>. "
                    f"{fetch_n:,} web fetches and {search_n:,} web searches belong almost entirely to the deep-research workflows."))

    b.append('<h2>Reconciling with what /usage and /stats show</h2>')
    b.append('<p>The user\'s impression was that the project cost more than this page says. Three things explain the gap, and one '
             'real omission was checked and found small.</p>')
    b.append(table(["Check", "Finding"], [
        ["<b>Claude Code\'s own stats file counts every streamed record.</b>", "Each assistant message is written to the transcript once per content block (2 to 5 records with the same message id). "
         "Claude Code\'s own local usage-stats cache sums them all: its 08-13 Fable figure, 1,730,401, equals this census\'s <i>un-deduplicated</i> input+output sum for that day exactly. "
         "Priced that way the project reads as 5.94 B tokens and <b>$7,961</b>. The API bills per request, so the deduplicated 3.17 B tokens and $4,974 is the honest figure."],
        ["<b>/usage shows plan utilisation, not dollars.</b>", "The Fable weekly allowance was exhausted three times (08-14, 08-22, 08-28). Reaching a cap says nothing about the dollar value of the tokens under it."],
        ["<b>Other projects shared the same allowance.</b>", "Between 08-15 and 08-28 the same account ran Opus sessions on five other projects (a Unity game, an image folder, three code repos: 447 prompts in the history file). Their usage sits in the same /usage bars and the same stats file, and none of it is in this census."],
        ["<b>Missing transcripts.</b>", "The prompt history lists nine G-Diffuser sessions; four are on disk. The five absent ones are each a single <code>/resume</code> that went nowhere."],
        ["<b>Calls the transcript does not log.</b>", "Four context compactions (contexts of 0.5–1.0 M tokens each, about $4 in total at Fable rates), session-title and tool-summary side calls on Haiku (cents), and 900 web searches, almost all inside the research workflows (about $9 at API rates). Retries after 529 overloads are not billed."],
    ]))

    b.append('<h2>What a human team would have cost</h2>')
    b.append('<p>A rough comparison, not a quote. The scope a team would face is the scope this project faced: a new fixed-function GPU '
             'backend, a 32-bit sweep across three codebases, a ROM-side audio driver port, an ARM11 performance grind to native 60 on a '
             'software display-list interpreter, stereo, a multi-core render pipeline, lifecycle, menus and packaging. About 25,000 lines '
             'of low-level code (port plus patches — ' + D["loc_all"] + ' lines exactly, the same figure the "Cost per outcome" table '
             'below uses) plus the research and the hardware loop.</p>')
    b.append('<h3>Estimate to reach this state</h3>')
    b.append(table(["Team", "Calendar", "Effort", "Cost at US rates"], [
        ["Two senior engineers (graphics/runtime, systems/audio/perf) plus part-time QA on hardware", "5–8 months", "12–18 person-months", "$250k–$450k"],
        ["One very senior generalist, full time", "9–14 months", "9–14 person-months", "$180k–$300k"],
        ["One skilled hobbyist, evenings and weekends", "12–24 months", "1,000–1,500 hours", "$0 paid, a year of unpaid labour"],
    ], num_cols=(1, 2, 3)))
    b.append('<p>Basis: $150–200 per hour contractor rates or $20–25k per month loaded per senior engineer; low-level port work lands at '
             '100–200 shipped lines per engineer-day once debugging and hardware loops are counted, which puts ' + D["loc_all"] + ' lines at '
             'roughly 125–250 engineer-days, about six to twelve person-months, before the performance grind. A human team would spend its time in the same places the agents did (the 32-bit '
             'address-space heuristics, the fog LUT, the HUD backstop, the audio driver, then months of profiler-driven grinding), would very '
             'likely have shipped 30 Hz plus interpolation first and deferred stereo, and might never have attempted the core-2 render thread.</p>')
    b.append('<p>For scale, not as a direct comparison: other N64-on-3DS and libultraship ports take hobbyists or small volunteer teams '
             'months to years to reach playable, per their own READMEs and press — see <a href="boundaries.html">Pushing the boundaries</a> '
             'for those projects with sources.</p>')
    b.append('<h3>Side by side</h3>')
    b.append('<p>The \"human team\" column below is the midpoint of the two-senior-engineers row above ($250k–$450k, 5–8 months): the closest '
             'match to this project\'s scope.</p>')
    b.append(table(["", "This project", "Human team, mid estimate"], [
        ["Calendar", "24 days, 15 active", "about 6 months"],
        ["Cost", money(tB) + " API-equivalent", "about $350k"],
        ["Human time", "one person, about 15 days of direction and SD-card loops, no code written", "a full-time team"],
        ["Polish delivered", "playable, 60 fps median, stereo, lifecycle; known backlog (tunnel roof, boot-audio underrun, LOD thresholds)", "a QA pass and the backlog closed"],
    ]))
    b.append('<div class="note">Ratio: roughly 70× cheaper and 7–8× faster, with the caveats that a professional team ships more polish '
             'and that the hobbyist route is free if the year is not counted.</div>')

    b.append('<h2>Cost per outcome</h2>')
    b.append('<p>Some ways to read the bottom line, at published prices with 1-hour cache writes:</p>')
    per_prompt = tB / C["human_prompts"]["count_strict"]
    b.append(table(["Measure", "Value"], [
        ["Per human prompt", money(per_prompt)],
        ["Per active day (15 days)", money(tB / 15)],
        ["Per commit touching the 3DS port (" + D["commits_3ds"] + ", frozen 2026-10-08, not re-verified against final history)", money(tB / int(D["commits_3ds"].replace(",", "")))],
        ["Per line of port code, patches included (" + D["loc_all"] + " lines)", f"${tB / int(D['loc_all'].replace(',', '')):.2f}"],
        ["Per fps of median gained on hardware (from ~12–20 in the first emulator race to " + D["final_median"] + ")", money(tB / (float(D["final_median"]) - 16))],
    ], num_cols=(1,)))
    b.append('<div class="note">For scale: a single experienced console-port engineer at a modest contractor rate would bill more than the whole project '
             'in its first week. The point is not that the machine was cheap. It is that the machine was cheap enough to try five ideas '
             'and shelve three of them on evidence.</div>')

    b.append('<h2>What the second phase would add</h2>')
    b.append('<p>The census above comes from the same per-transcript, per-turn method described above in "Reconciling with what /usage '
             'and /stats show". That script was not kept in the repository, and re-running it over the second phase\'s transcripts — '
             '2026-09-07 to 10-19 — was out of scope for this documentation pass: the honest path was to say so clearly rather than '
             'approximate it.</p>')
    b.append('<p>What can be said without re-running that method, from the project\'s own git history: as of commit <code>cd83b92</code> '
             '(2026-10-19 21:07 UTC — the count below was still climbing as release work landed, so treat it as a floor, not a final tally), '
             'the second phase had added <b>798 commits touching <code>port/3ds</code></b> (1,628 commits in total, not port/3ds-scoped) over '
             '43 calendar days, versus the ' + D["commits_3ds"] + ' port/3ds commits this page prices for the 15-active-day first phase — '
             'about 3.2× as many, like for like. Commit count is not a cost measure — the two phases\' commits are not the same size or shape '
             'of change, and a quiet research day can produce zero commits and a large bill, or the reverse — but it is a real, verifiable '
             'sense of scale for a phase this page otherwise has nothing priced to say about.</p>')
    b.append('<div class="note">If this page is revisited: the method above, applied to the second phase\'s transcripts with the same '
             'de-duplication and pricing rules, is how to extend it. Until then, every dollar and token figure on this page is first-phase '
             'only, and should be read that way.</div>')
    return page("cost.html", "What It Cost", "".join(b))


# ---------- Revision A (2026-10-26): the whole project, every phase, from pa_census.json ----------

PA_STYLE = """<style>
.pa-head { display:grid; grid-template-columns:repeat(auto-fit, minmax(min(300px,100%),1fr)); gap:28px 36px; margin:28px 0 14px; }
.pa-head > div { border-top:1px solid var(--rule); padding:12px 0 4px; container-type:inline-size; min-width:0; }
.pa-head > div.here { border-top:3px solid var(--tone); padding-top:10px; }
.pa-head .k { font-family:var(--sans); font-size:10.5px; font-weight:700; letter-spacing:.14em; text-transform:uppercase; color:var(--ink); }
.pa-head .v { font-family:var(--sans); font-size:32px; font-size:clamp(26px, 7.4cqi, 42px); line-height:1.02; margin:12px 0 12px; text-wrap:balance; }
.pa-head .s { max-width:52ch; }
.pa-head .s { font-size:13.5px; color:var(--ink2); line-height:1.45; }
</style>"""

PA_MODELS = [
    ("claude-opus-5-5", "Opus 5.5", "--c1"), ("claude-fable-5", "Fable 5", "--c2"), ("claude-opus-5", "Opus 5", "--c3"),
    ("claude-fable-5-1", "Fable 5.1", "--c6"), ("claude-sonnet-5", "Sonnet 5", "--c4"), ("claude-opus-4-8", "Opus 4.8", "--c7"),
    ("claude-haiku-4-5-20251001", "Haiku 4.5", "--c8"),
]
PA_PHASES = [
    ("first", "New 3DS, to v0.9.0", "2026-08-11 → 09-03"),
    ("second", "Native fork, to v1.0.0", "2026-09-07 → 10-19"),
    ("after_v1", "v1.1.0 work", "2026-10-20 → 10-26"),
    ("gap", "Between phases", "2026-09-04 → 09-06"),
]


def build_current(D):
    import pa_facts as F
    C, T, PH = F.C, F.TOT, F.PH
    ppm = C["per_phase_model"]
    b = [PA_STYLE]
    b.append('<p class="eyebrow">11 · Cost</p><h1>What it cost to build</h1>')
    b.append('<div class="pa-head">'
             '<div><div class="k">A human team (estimate)</div>'
             f'<div class="v big-num">{F.HEADLINE_HUMAN.replace("~", "")}</div>'
             f'<div class="s">About {F.HUMAN_MONTHS[0]:.0f}–{F.HUMAN_MONTHS[1]:.0f} months with a team of {F.TEAM} senior engineers. '
             'Method and inputs are under "The human estimate" below.</div></div>'
             '<div class="here t-gold"><div class="k">What it took here</div>'
             f'<div class="v big-num glow">{F.musd(F.COST_LOGGED)}–{F.musd(F.COST_1H)} API · ~{F.AUTHOR_H[0]}–{F.AUTHOR_H[1]} h</div>'
             f'<div class="s">Claude at published API list prices, plus one person directing, testing on hardware and copying SD cards, '
             f'over {F.CAL_WEEKS} calendar weeks (2026-08-11 → 10-26). The hours are the author\'s own figure, checked against the transcripts; see "The author\'s time".</div></div>'
             '</div>')
    b.append(f'<p class="lede">Priced the same way, the human route costs roughly {F.HUMAN_USD[0] / F.HERE_USD[1]:.0f}–{F.HUMAN_USD[1] / F.HERE_USD[0]:.0f}× '
             f'more, even after billing the author\'s hours at a senior engineer\'s rate (${F.AUTHOR_RATE}/h, which puts this project at '
             f'{F.musd(F.HERE_USD[0])}–{F.musd(F.HERE_USD[1])}). The token and API figures are counted exactly from the session transcripts; '
             'the human figures are estimates and the author\'s hours come from the transcripts and the author, and the page says how each was made.</p>')
    b.append(f'<div class="note"><b>Updated 2026-10-26.</b> The first version of this page priced only the first phase (08-11 to 09-03): '
             f'3.17 B tokens and {F.money(F.FIRST_PUBLISHED)}. This version counts every phase through 2026-10-26 (census run '
             f'{F.CENSUS_AT}; the last day is still open): <b>{F.big(F.tokens(T))} tokens and {F.money(F.COST_1H)}</b> by the same method, '
             f'or <b>{F.money(F.COST_LOGGED)}</b> billing each cache write at the duration it actually logged (see "Prices used").</div>')
    b.append(tiles([
        (F.big(F.tokens(T)), "tokens processed, all sessions, subagents and workflow agents, 08-11 to 10-26", "blue"),
        (F.money(F.COST_1H), "at API list prices, every cache write at the 1-hour rate (the first-phase page's method)", "gold"),
        (F.money(F.COST_LOGGED), "at API list prices, each cache write at the duration it logged (5 minutes for most)", "gold"),
        (f"{T['turns']:,}", "API calls (assistant turns) after de-duplication", False),
        (big(T["output"]), "output tokens: the text and code the models actually wrote", False),
        (f"{F.PROMPTS}", "human prompts typed into the project's sessions", False),
        (f"{F.SUBAGENTS}", "background subagents launched with the Agent tool", False),
        (f"{F.WORKFLOW_AGENTS:,}", "short-lived agents inside deep-research workflows", False),
    ]))

    b.append('<h2>By phase</h2>')
    rows = []
    for k, name, dates in PA_PHASES:
        p = PH.get(k)
        if not p:
            continue
        ag = f"{F.AGENTS.get('subagent|' + k, 0)} / {F.AGENTS.get('workflow|' + k, 0):,}"
        rows.append([f"<b>{esc(name)}</b>", f'<span style="white-space:nowrap">{dates[5:]}</span>', f"{p['turns']:,}", big(F.tokens(p)), big(p["output"]), ag,
                     money(p["cost_B"]), money(p["cost_logged"])])
    rows.append(["<b>Total</b>", '<span style="white-space:nowrap">08-11 → 10-26</span>', f"<b>{T['turns']:,}</b>", f"<b>{F.big(F.tokens(T))}</b>", f"<b>{big(T['output'])}</b>",
                 f"<b>{F.SUBAGENTS} / {F.WORKFLOW_AGENTS:,}</b>", f"<b>{money(F.COST_1H)}</b>", f"<b>{money(F.COST_LOGGED)}</b>"])
    b.append(table(["Phase", "Dates", "API calls", "Tokens", "Output", "Subagents / workflow agents", "Cost, 1 h writes", "Cost, as logged"],
                   rows, num_cols=(2, 3, 4, 5, 6, 7)))
    cats = [n.replace(", ", "|") for k, n, _ in PA_PHASES if k in PH and k != "gap"]
    keys = [k for k, _, _ in PA_PHASES if k in PH and k != "gap"]
    series = [(lab, col, [ppm.get(k + "|" + m, {}).get("cost_B", 0) / 1e3 for k in keys]) for m, lab, col in PA_MODELS]
    b.append(figure(stacked_bars(cats, series, ylabel="thousands of USD, 1-hour cache writes", dec=1),
                    f"<b>The second phase cost about two and a half times the first.</b> It ran {43} calendar days against 24, on more "
                    "agents in parallel, with larger contexts. Opus 5.5 carried more than half of the second phase's spend and nearly all of "
                    "the work since v1.0.0; Fable 5 carried most of the first phase.",
                    legend([(lab, col) for _, lab, col in PA_MODELS])))

    b.append('<h2>Prices used</h2>')
    b.append('<p>Published Claude API list prices in dollars per million tokens: the same table the first version of this page used, '
             'with the two models that arrived since (Opus 5.5 and Sonnet 5) added at their published rates. Cache reads are 0.1× input '
             'except where listed.</p>')
    b.append(table(["Model", "Input", "Output", "Cache write (1 h)", "Cache write (5 min)", "Cache read"], [
        ["Claude Fable 5.1", "$10", "$50", "$20", "$12.50", "$0.25"],
        ["Claude Fable 5", "$10", "$50", "$20", "$12.50", "$1.00"],
        ["Claude Opus 5 / Opus 4.8", "$5", "$25", "$10", "$6.25", "$0.50"],
        ["Claude Opus 5.5", "$4", "$20", "$8", "$5.00", "$0.20"],
        ["Claude Sonnet 5", "$2", "$10", "$4", "$2.50", "$0.20"],
        ["Claude Haiku 4.5", "$1", "$5", "$2", "$1.25", "$0.10"],
    ], num_cols=(1, 2, 3, 4, 5)))
    cw = T["cw_1h"] + T["cw_5m"]
    b.append(f'<div class="assume"><b>A correction to the first version.</b> It billed every cache write at the 1-hour rate, on the belief that '
             f'1 hour was the TTL these sessions used. The usage records say otherwise: of {big(cw)} cache-write tokens that carry a '
             f'duration, {100 * T["cw_5m"] / cw:.0f}% were 5-minute writes. Billed as logged, the first phase comes to '
             f'{money(PH["first"]["cost_logged"])}, not {money(PH["first"]["cost_B"])}, and the whole project to {money(F.COST_LOGGED)}. '
             'Both figures are shown because the first is the like-for-like update of the number this page used to print and the second '
             'is the better estimate. The user\'s Claude plan is not billed per token; these are what the same usage would have cost on '
             'the API. Web searches and context compactions are not in the transcripts and are not counted (in the first phase they '
             'came to about $13).</div>')

    b.append('<h2>By model</h2>')
    rows = []
    for m, lab, col in PA_MODELS:
        v = C["per_model"].get(m)
        if not v:
            continue
        rows.append([f"<code>{esc(m)}</code>", f"{v['turns']:,}", f"{v['output']:,}", f"{v['cache_read']:,}",
                     money(v["cost_B"]), money(v["cost_logged"])])
    b.append(table(["Model", "API calls", "Output tokens", "Cache-read tokens", "Cost, 1 h writes", "Cost, as logged"], rows, num_cols=(1, 2, 3, 4, 5)))

    b.append('<h2>Spend by day</h2>')
    import datetime
    d0, d1 = (datetime.date.fromisoformat(x) for x in C["date_range"])
    days = [(d0 + datetime.timedelta(n)).isoformat() for n in range((d1 - d0).days + 1)]
    svg = day_bars(days, [C["per_day"].get(d, {}).get("cost_B", 0) for d in days], ylabel="USD per day, 1-hour cache writes", fmt=lambda v: f"${v:,.0f}",
                   marks={"2026-08-14": "first race", "2026-09-29": "Old 3DS boots", "2026-10-19": "v1.0.0"})
    b.append(figure(svg, f"<b>{F.API_DAYS} days with API calls in {len(days)} calendar days.</b> The first phase spent in a few large bursts between "
                    "plan-allowance pauses. The second phase's heaviest days were 09-23 (fitting the Old 3DS build into its memory), "
                    "09-29 to 10-02 (the first Old 3DS hardware session through the first native-renderer race) and 10-08, the end of "
                    f"the 3D-at-60 push and the most expensive day of the project at {money(C['per_day']['2026-10-08']['cost_B'])}. "
                    "Hover a bar for its value."))

    b.append('<h2>The human estimate</h2>')
    b.append('<p>A rough comparison, not a quote. The scope a team would face is what shipped: a citro3d rendering backend, a 32-bit sweep, '
             'audio on the DSP, stereo, lifecycle, an owned native renderer sized to the Old 3DS\'s instruction cache, the 64DD Expansion Kit, '
             'a bottom-screen dashboard and menus, and 2–4 player multiplayer over local wireless and the internet, on two consoles.</p>')
    b.append(table(["Input", "Value", "Basis"], [
        ["Code written for the port", f"{F.LOC_PORT:,} lines", "<code>port/3ds</code> at <code>ac33c30</code>, excluding the vendored "
         "libultraship subset, third-party libraries, generated asset headers and docs; includes the patch stack and "
         f"{F.LOC_PORT_TESTS:,} lines of tests, probes and harness. Build and dev tools add another {F.LOC_TOOLS:,}, not counted below"],
        ["Lines a human team would need", f"{F.HUMAN_LINES[0] / 1e3:.0f}k–{F.HUMAN_LINES[1] / 1e3:.0f}k",
         "Half to all of the production code. The agents wrote heavily instrumented code (receipts, A/B arms, levers); "
         "a team would write less of it for the same result"],
        ["Rate", f"{F.HUMAN_RATE[1]}–{F.HUMAN_RATE[0]} lines per engineer-day", "The rate the first version of this page used for low-level "
         f"port work once debugging and hardware loops are counted; {F.DAYS_PER_YEAR} working days a year"],
        ["Effort", f"{F.HUMAN_EY[0]:.1f}–{F.HUMAN_EY[1]:.1f} engineer-years", "Lines ÷ rate"],
        ["Cost", f"{F.musd(F.HUMAN_USD[0])}–{F.musd(F.HUMAN_USD[1])}", f"${F.LOADED[0] // 1000}k–${F.LOADED[1] // 1000}k loaded per senior "
         "engineer-year (US; $20–25k a month, as before)"],
        ["Calendar", f"about {F.HUMAN_MONTHS[0]:.0f}–{F.HUMAN_MONTHS[1]:.0f} months", f"A team of {F.TEAM}, ignoring hiring and ramp-up"],
    ]))
    b.append('<p>A check against people who have done comparable work: Ship of Harkinian, the Ocarina of Time PC port, was built by a '
             'volunteer team from November 2021, when the decompilation was finished, to its first release in March 2022; 2 Ship 2 '
             'Harkinian, the Majora\'s Mask port on the same libultraship base, shipped in May 2024, six months after its decompilation '
             'was finished (<a href="https://en.wikipedia.org/wiki/Ship_of_Harkinian">Wikipedia</a>). Those are PC ports with no memory '
             'or frame budget to speak of. The N64-on-3DS projects listed on <a href="boundaries.html">Pushing the boundaries</a> are '
             'hobby efforts that, by their own READMEs, mostly target the New 3DS at 30 fps or with interpolation. Nothing here '
             'contradicts the range above; nothing narrows it much either.</p>')

    b.append('<h2>The author\'s time</h2>')
    b.append(f'<p>The transcripts hold {F.PROMPTS} prompts typed into this project\'s sessions on 49 days between 08-10 and 10-26 '
             f'(US Eastern time). Grouping prompts less than 20–45 minutes apart into sittings, and adding 15 minutes to each, gives '
             f'{F.AUTHOR_KEYBOARD_H[0]}–{F.AUTHOR_KEYBOARD_H[1]} hours at the keyboard. The author confirms the total was about '
             f'{F.AUTHOR_H[0]}–{F.AUTHOR_H[1]} hours: hardware testing, SD-card copies, photographing screens and reading the docs '
             'leave no trace in the transcripts, but they largely overlapped with sittings or were short, so the total is close to '
             'the keyboard time. Prompts fall on every day of the week and at every hour from '
             '8 am to 3 am, so this was not only evenings.</p>')
    b.append('<p>No code was written by hand. The author\'s work was direction (what to build next, what to drop), judgement on '
             'evidence (which experiments were real wins), and the hardware loop the agents could not run themselves.</p>')

    b.append('<h3>Side by side</h3>')
    b.append(table(["", "This project", "Human team, estimate"], [
        ["Calendar", f"{F.CAL_WEEKS} weeks (08-11 → 10-26), {F.API_DAYS} days with API activity",
         f"{F.HUMAN_MONTHS[0]:.0f}–{F.HUMAN_MONTHS[1]:.0f} months with {F.TEAM} engineers"],
        ["Money", f"{money(F.COST_LOGGED)}–{money(F.COST_1H)} API-equivalent", f"{F.musd(F.HUMAN_USD[0])}–{F.musd(F.HUMAN_USD[1])}"],
        ["People", f"one person, ~{F.AUTHOR_H[0]}–{F.AUTHOR_H[1]} hours, no code written by hand", f"{F.HUMAN_EY[0]:.0f}–{F.HUMAN_EY[1]:.0f} engineer-years"],
        ["What shipped", "v0.9.0, v1.0.0 (Old and New 3DS, Expansion Kit) and v1.1.0 (multiplayer); known gaps listed in the release notes",
         "the same scope, plus whatever polish a funded team would add"],
    ]))

    b.append('<h2>Cost per outcome</h2>')
    b.append(table(["Measure", "1-hour writes", "As logged"], [
        ["Per human prompt", money(F.COST_1H / F.PROMPTS), money(F.COST_LOGGED / F.PROMPTS)],
        [f"Per day with API activity ({F.API_DAYS})", money(F.COST_1H / F.API_DAYS), money(F.COST_LOGGED / F.API_DAYS)],
        [f"Per line of port code ({F.LOC_PORT:,})", f"${F.COST_1H / F.LOC_PORT:.2f}", f"${F.COST_LOGGED / F.LOC_PORT:.2f}"],
    ], num_cols=(1, 2)))

    b.append('<h2>How the census was made</h2>')
    b.append('<p><code>docs/postmortem/gen/pa_census.py</code> re-implements the first version\'s method and can be re-run. It reads every '
             'transcript under the project\'s two Claude Code directories (<code>~/Code/G-Diffuser</code> through v1.0.0, '
             f'<code>~/Code/gdx-3ds</code> since; {C["files_scanned"]:,} files: orchestrator sessions, Agent-tool subagents and '
             'deep-research workflow agents). Each assistant message id is one API call; the 2–5 streamed records of the same message '
             'are merged by taking the largest value of each usage field, so nothing is counted twice. Sessions started from the home '
             'directory that only mention the project, and every other project on the same account, are excluded.</p>')
    b.append(table(["Check", "Finding"], [
        ["<b>The first phase reproduces.</b>", f"Over 08-11 to 09-03 the script finds {F.big(F.tokens(PH['first']))} tokens and "
         f"{money(PH['first']['cost_B'])} (1-hour writes), against 3.17 B and {money(F.FIRST_PUBLISHED)} published. The first census stopped "
         "at 19:04 UTC on 09-03; this one counts the whole UTC day."],
        ["<b>Claude Code\'s own stats file counts every streamed record.</b>", "Its totals are about 1.9× the de-duplicated count (found in "
         "the first phase, 5.94 B vs 3.17 B). The API bills per request, so the de-duplicated figure is the one used."],
        ["<b>/usage shows plan utilisation, not dollars.</b>", "Reaching a weekly cap says nothing about the dollar value of the tokens under it, "
         "and other projects shared the same allowance."],
        ["<b>Not counted.</b>", "Web searches and fetches billed per use, context compactions and title/summary side calls are not in "
         "the transcripts. In the first phase they came to about $13 in total."],
    ]))
    b.append('<p>The first phase\'s per-subagent and per-tool tables are in <a href="source-census.md">source-census.md</a>. Its clearest '
             'cost lesson still stands: two 900-turn agents of 08-14 were left running through a credit pause and each re-read a very '
             'large context hundreds of times. An agent that has finished its brief should be stopped.</p>')
    return page("cost.html", "What It Cost", "".join(b))
