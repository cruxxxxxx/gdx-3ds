"""Current numbers for the Overview and Cost pages (post-mortem revision A, 2026-10-26).

Token and dollar figures come from pa_census.json (written by pa_census.py from the Claude Code transcripts).
Hardware figures are from the card-6 test logs and the release notes; each one carries its source so the
pages can say which build, console and mode it is. Human-effort figures are an estimate; the inputs are
here so the method on the Cost page and the numbers it prints cannot drift apart.
"""
import json, os

HERE = os.path.dirname(os.path.abspath(__file__))
C = json.load(open(os.path.join(HERE, "pa_census.json")))

TOK_FIELDS = ("input", "output", "cache_creation", "cache_read")


def tokens(d):
    return sum(d[k] for k in TOK_FIELDS)


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


TOT = C["totals"]
PH = C["per_phase"]
COST_1H = TOT["cost_B"]          # every cache write at the 1-hour rate: the first-phase page's method
COST_LOGGED = TOT["cost_logged"]  # each call's own logged 1 h / 5 min split
FIRST_PUBLISHED = 4974            # what the first-phase Cost page printed (source-census.md, scenario B)
FIRST_TOKENS_PUBLISHED = 3.17e9
AGENTS = C["agents_started"]
SUBAGENTS = sum(v for k, v in AGENTS.items() if k.startswith("subagent|"))
WORKFLOW_AGENTS = sum(v for k, v in AGENTS.items() if k.startswith("workflow|"))
PROMPTS = C["human_prompts"]["count"]
API_DAYS = C["active_days"]
CENSUS_AT = C["generated_at"]

# Hardware. Card-6 = the test build card-4@ba0692e (Expansion Kit configuration on both consoles, base-game
# course 12, 3D slider on, one player against the CPU field), logged 2026-10-26: hwlogs/{o3ds,n3ds}-card6/log-EK.txt.
# fps per window = the [gxcost] receipt's frame count per ~1 s window (full windows only; a window that runs a
# little over a second reads 61). Old 3DS steady race = log lines 3420-4214 (48 windows); with the race start, 64.
HW = {
    "n3ds_race3d_windows": 120, "n3ds_race3d_min": 59,
    "o3ds_race3d_median": 60, "o3ds_race3d_p10": 56, "o3ds_race3d_at60": 85, "o3ds_race3d_windows": 48,
    "o3ds_race3d_p10_with_start": 50,
    "o3ds_race3d_median_ms": "16.7–16.8", "o3ds_race3d_worst_ms": "22.9–32.8",
    "o3ds_menu3d_before": 40.2, "o3ds_menu3d_after": 44.6,
    "o3ds_gp2d_mean": 57.3,       # v1.0.0 release notes, "Measured performance": full 30-racer field, log-flush stalls excluded
    "o3ds_gp2d_mean_all": 51.9,
}

# Lines of code: git ls-files at ac33c30, port/3ds minus vendor/ (vendored libultraship), assets/, third_party/
# (nlohmann json, tinyxml2), *_assets.h, and docs (*.md, *.txt, *.ini, *.json). Includes the patch stack and the
# test, probe and harness code. tools/ (build, prebake, dev harness scripts) counted separately.
LOC_PORT = 181208
LOC_PORT_TESTS = 13349
LOC_TOOLS = 41324
LOC_FIRST_PHASE = 25009

# Human-effort model (an estimate, not a quote).
#   lines a human team would need: half to all of the production code. The agents wrote heavily instrumented code
#   (receipts, A/B arms, probes); a human team would write less of it for the same features.
#   rate: 100-200 shipped lines per engineer-day, the rate the first-phase page used for low-level port work.
#   220 working days per engineer-year; $240k-$300k loaded per senior engineer-year ($20-25k a month, as before).
LOC_PROD = LOC_PORT - LOC_PORT_TESTS
HUMAN_LINES = (LOC_PROD * 0.5, LOC_PROD)
HUMAN_RATE = (200, 100)
DAYS_PER_YEAR = 220
HUMAN_EY = (HUMAN_LINES[0] / HUMAN_RATE[0] / DAYS_PER_YEAR, HUMAN_LINES[1] / HUMAN_RATE[1] / DAYS_PER_YEAR)
LOADED = (240_000, 300_000)
HUMAN_USD = (HUMAN_EY[0] * LOADED[0], HUMAN_EY[1] * LOADED[1])
TEAM = 4
HUMAN_MONTHS = (HUMAN_EY[0] / TEAM * 12, HUMAN_EY[1] / TEAM * 12)

# The author's time. Measured: 777 prompts typed into this project's sessions, on 49 days (US Eastern) between 08-10 and
# 10-26. Grouping prompts less than 20-45 minutes apart into sittings (+15 min each) gives 126-157 hours at the
# keyboard. The author confirms the total was about 125-150 hours: hardware testing, SD-card copies and reading logs
# largely overlapped with sittings or were short, so the total is close to the keyboard time.
AUTHOR_KEYBOARD_H = (126, 157)
AUTHOR_H = (125, 150)
CAL_WEEKS = 11
AUTHOR_RATE = 140   # $/h, the same loaded rate as the human team ($300k / ~2,100 h), to price the author's time
HERE_USD = (COST_LOGGED + AUTHOR_H[0] * AUTHOR_RATE, COST_1H + AUTHOR_H[1] * AUTHOR_RATE)


def ey(v):
    return f"{v:.0f}"


def musd(v):
    return f"${v/1e6:.1f}M" if v >= 4e5 else f"${v/1e3:.0f}k"


HEADLINE_HUMAN = f"~{ey(HUMAN_EY[0])}–{ey(HUMAN_EY[1])} engineer-years, {musd(HUMAN_USD[0])}–{musd(HUMAN_USD[1])}"
HEADLINE_HERE = f"{musd(COST_LOGGED)}–{musd(COST_1H)} of API time and ~{AUTHOR_H[0]}–{AUTHOR_H[1]} hours of one person over {CAL_WEEKS} weeks"
