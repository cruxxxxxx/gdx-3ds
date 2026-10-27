"""The Old 3DS part of native.html: status, the card calendar, the five phases, one submission, memory.

Was its own page (old-3ds.html) until revision D merged it into The native fork.
"""
import json, os
import datetime as dt
from framework import *
import pb_calendar
import pb_tabs

# Per-card calendar labels for the v1.0.0 campaign cards in o3ds-data.json (shared with page_difficulties,
# so this page keeps its own presentation data here rather than editing that file).
ABBR = {
    "Shipped build (RDRAM 16)": ("v0.9 ✗ OOM", "bad"),
    "First boot (RDRAM 8)": ("boot · 10 fps", "prog"),
    "C4 run 1: O3DS profile": ("run 1 · 11", "prog"),
    "C4 run 2: narrow audio lock": ("run 2 · 15", "prog"),
    "Wave 2: HOME + audio SIMD": ("HOME / SIMD", "probe"),
    "Wave 3: frameskip + O3DS preset": ("skip · 19–28", "prog"),
    "REL1": ("REL1 ✗ boot", "bad"),
    "REL2": ("REL2 · 15", "prog"),
    "REL3": ("REL3 · 20", "prog"),
    "HWPROF B": ("PMU · CPI 4.3", "probe"),
    "REL4": ("REL4 · replay", "prog"),
    "n60-m1 (NRR milestone 1)": ("NRR ✗ freeze", "bad"),
    "n60-card2": ("card 2 · 57", "prog"),
    "n60-card3": ("card 3 · 60 2D", "good"),
    "interim legacy 3D": ("3D ✗ sky", "bad"),
    "n60-3d": ("3D · 40", "prog"),
    "n60-3d2": ("3D · 48–57", "prog"),
    "n60-3d3": ("3D · ~60 MC", "good"),
    "n60-3d4": ("✗ VRAM", "bad"),
    "card 4 (VRAM fix)": ("card 4 · 3D 60", "good"),
}

# After v1.0.0 (10-19): the v1.1.0 hardware cards, a separate numbering ("v1.1 card N"). Sources are the
# hardware logs and their analyses named in each row; figures copied from them.
AFTER_V1 = [
    {"date": "10-24", "name": "o3ds-3d ceiling probe (0c2668a)", "abbr": "3D · 54 · CPU-bound", "kind": "probe",
     "what": "Lockstep 3D race in D=1 blocks, plain vs the previous frame's GPU work waited out first",
     "hw": "Idling the GPU first did not make the frame cheaper (13.5 → 14.2 ms, 11.8 → 12.3 ms): Old 3DS 3D is CPU-bound; the "
           "deferred-wait plan was dropped. 1P 3D race with 16 machines: wall 18.6 ms = 53.7 fps. 3D multiplayer locked to 30 (ed04b17)",
     "src": "hwlogs/o3ds-0c2; cpu-profile-answer.md; ed04b17"},
    {"date": "10-25", "name": "v1.1 card 3 (3c0c7b7)", "abbr": "MP 2D · 60", "kind": "good",
     "what": "Online 2D race, 2 players + 14 CPU machines",
     "hw": "Race windows at a locked 60 (wall 16.71 ms, 3.8–4.1 ms of vblank slack); the steady governor still flapped 60/30 "
           "at D=1, fixed by the drift rule (a89289f)",
     "src": "hwlogs/o3ds-card3; cpu-profile-answer.md §A.4"},
    {"date": "10-26", "name": "v1.1 card 5 (5e19320)", "abbr": "✗ HOME close", "kind": "bad",
     "what": "Expansion Kit test build, v1.1.0 work",
     "hw": "HOME-close hard lock (a 73 KB allocation in the 64DD save flush)",
     "src": "hwlogs/o3ds-card5"},
    {"date": "10-26", "name": "v1.1 card 6 (ba0692e)", "abbr": "card 6 · 3D 60", "kind": "good",
     "what": "One GX submission for both eyes, four-arm automatic A/B; heap fixes",
     "hw": "1P 3D race 59.7–59.8 fps with one submission (baseline 56.4 / 59.3; two blocks per arm, so suggestive); 3D menu "
           "40.2 → 44.6 fps; HOME close clean",
     "src": "hwlogs/o3ds-card6; onesub-hw/o3ds-card6-analysis.md"},
]
# Other Old 3DS log sets from the v1.1.0 work that this page does not analyse (directory names in hwlogs/).
OTHER_LOGS = {"10-22": ["516", "1859", "b5f"], "10-23": ["900", "4bf", "158", "5c2", "d9d", "52a"],
              "10-24": ["a3b", "908", "b52"], "10-25": ["online1", "card2", "card2b", "card2c"]}

# Frame-rate points on a real time axis: (date, fps, mode, estimate?, label). Race figures unless labelled.
FPS_POINTS = [
    ("09-29", 10.4, "2d", False, "first boot"), ("09-29", 14.7, "2d", False, "audio lock"),
    ("09-30", 15, "2d", False, "REL2"), ("09-30", 20, "2d", False, "REL3"), ("10-01", 20, "2d", False, "REL4"),
    ("10-02", 57.3, "2d", False, "card 2"), ("10-02", 60, "2d", False, "card 3"),
    ("10-03", 40, "3d", False, "3D first"), ("10-03", 52, "3d", True, "3D chunked"), ("10-07", 60, "3d", True, "Mute City"),
    ("10-08", 59.5, "3d", False, "card 4"), ("10-24", 53.7, "3d", False, "10-24 1P 3D"),
    ("10-25", 60, "mp", False, "MP 2D, 16"), ("10-26", 59.7, "3d", False, "card 6"),
]


def _x(date, x0, pw, first, days):
    d = dt.date(2026, int(date[:2]), int(date[3:]))
    return x0 + pw * (d - first).days / days


def fps_over_time():
    w, h = 920, 300
    L, R, T, B = 46, 16, 20, 52
    pw, ph = w - L - R, h - T - B
    first, last = dt.date(2026, 9, 28), dt.date(2026, 10, 27)
    days = (last - first).days
    y = lambda v: T + ph - ph * v / 66
    out = [f'<svg viewBox="0 0 {w} {h}" role="img" aria-label="Old 3DS race frame rate over time">']
    for a, b, lab in (("10-03", "10-07", "pause"), ("10-08", "10-15", "pause")):
        xa, xb = _x(a, L, pw, first, days) + 6, _x(b, L, pw, first, days) - 6
        out.append(f'<rect x="{xa:.1f}" y="{T}" width="{xb-xa:.1f}" height="{ph}" fill="var(--bg3)"/>')
        out.append(f'<text x="{(xa+xb)/2:.1f}" y="{T+ph-6}" text-anchor="middle" font-size="10.5" fill="var(--ink3)">{lab}</text>')
    for tv in (0, 20, 40, 60):
        out.append(f'<line x1="{L}" y1="{y(tv):.1f}" x2="{w-R}" y2="{y(tv):.1f}" stroke="var(--line)"/>')
        out.append(f'<text x="{L-8}" y="{y(tv)+4:.1f}" text-anchor="end" font-size="11" fill="var(--ink3)">{tv}</text>')
    out.append(f'<line x1="{L}" y1="{y(60):.1f}" x2="{w-R}" y2="{y(60):.1f}" stroke="var(--bad)" stroke-dasharray="5 4" stroke-width="1.3"/>')
    for d in ("09-29", "10-01", "10-08", "10-15", "10-19", "10-22", "10-26"):
        xx = _x(d, L, pw, first, days)
        out.append(f'<text x="{xx:.1f}" y="{T+ph+16}" text-anchor="middle" font-size="11" fill="var(--ink2)">{d}</text>')
    xv = _x("10-19", L, pw, first, days)
    out.append(f'<line x1="{xv:.1f}" y1="{T}" x2="{xv:.1f}" y2="{T+ph}" stroke="var(--blue)" stroke-width="1.2" stroke-dasharray="2 3"/>')
    out.append(f'<text x="{xv+4:.1f}" y="{T+10}" font-size="11" fill="var(--blue-ink)">v1.0.0</text>')
    cols = {"2d": "--c1", "3d": "--c3", "mp": "--c4"}
    for d, v, mode, est, lab in FPS_POINTS:
        xx = _x(d, L, pw, first, days) + {"2d": -4, "3d": 4, "mp": 0}[mode]
        fill = "none" if est else f"var({cols[mode]})"
        out.append(f'<circle cx="{xx:.1f}" cy="{y(v):.1f}" r="5.5" fill="{fill}" stroke="var({cols[mode]})" stroke-width="2">'
                   f'<title>{esc(d)} · {esc(lab)}: {v:g} fps{" (estimate)" if est else ""}</title></circle>')
    for d, v, mode, est, lab in FPS_POINTS:
        if lab in ("first boot", "REL3", "card 3", "3D first", "card 4", "10-24 1P 3D", "card 6"):
            xx = _x(d, L, pw, first, days)
            dy = -10 if lab not in ("10-24 1P 3D", "3D first", "first boot") else 18
            out.append(f'<text x="{xx:.1f}" y="{y(v)+dy:.1f}" text-anchor="middle" font-size="10.5" fill="var(--ink2)">{esc(lab)}</text>')
    out.append(f'<line x1="{L}" y1="{T+ph}" x2="{w-R}" y2="{T+ph}" stroke="var(--ink3)"/>')
    out.append(f'<text x="{L}" y="{T-6}" font-size="11" fill="var(--ink3)">race frames per second</text>')
    out.append('</svg>')
    return "".join(out)


def calendar_cards(O):
    out = []
    for c in O["cards"]:
        abbr, kind = ABBR.get(c["name"], (c["name"], "prog"))
        out.append(dict(c, abbr=esc(abbr), kind=kind, name=esc(c["name"])))
    out += [dict(c, abbr=esc(c["abbr"]), name=esc(c["name"])) for c in AFTER_V1]
    for d, names in OTHER_LOGS.items():
        for n in names:
            out.append({"date": d, "name": f"o3ds-{esc(n)}", "abbr": esc(n), "kind": "log", "what": "",
                        "hw": "Old 3DS hardware log set from the v1.1.0 tuning work; not analysed on this page",
                        "src": f"hwlogs/o3ds-{n}"})
    out.sort(key=lambda c: c["date"])
    return out




def onesubmit():
    """v1.1.0: both eyes in one GX submission. Figures from onesubmit-progress.md and the card-6 analysis."""
    b = ['<h3>One submission for both eyes (v1.1.0)</h3>']
    b.append('<p>After v1.0.0, a 3DS homebrew developer asked in a public comment whether the stereo path submits each eye '
             'separately, and suggested one command list for both. It did not re-encode the right eye, but every replayed list was its '
             'own GX queue entry, with its own GSP submit, completion interrupt and event-thread wake on core 0. Old 3DS probes put a '
             'clean citro3d draw at 10.9 µs and showed 0.4–0.5 ms of GPU dead time per backlogged queue entry (about 40 µs on New 3DS).</p>')
    b.append('<p>The fix uses the PICA200\'s second command-buffer channel: each stereo chunk ends by jumping to a short right-eye '
             'prologue, which jumps back into the same chunk, then to the finalize words. GSP gets one entry; the GPU runs the body twice '
             'and raises one interrupt. In the emulator every frame hash, game-state hash and screenshot matched the old path. On '
             'hardware, one build per console cycled four arms automatically:</p>')
    arms = ["0 baseline", "1 one submit", "2 + no kick", "3 + narrow flush"]
    svg = grouped_bars(arms, [("GX entries / frame", "--c1", [9, 7, 6, 7]), ("completion IRQs / frame", "--c3", [5, 3, 2, 3])],
                       ymax=10.5, ylabel="per frame, Old 3DS, 1P 3D race", dec=0, h=230, w=920, ymarks=[0, 3, 6, 9])
    b.append(figure(svg, "<b>Old 3DS, card 6 (10-26).</b> GX queue entries and completion interrupts per frame, per arm, from the "
                    "<code>[gxcost]</code> receipts.", legend([("GX entries", "--c1"), ("completion IRQs", "--c3")])))
    b.append(table(["Old 3DS", "0 baseline", "1 one submit", "2 + no kick", "3 + narrow flush"], [
        ["3D menu, static screen", "40.2 fps", "44.6", "44.6", "44.6"],
        ["1P 3D race (2 blocks per arm)", "56.4 / 59.3", "59.7–59.8", "59.7–59.8", "59.7–59.8"],
        ["Headroom on the critical path", "–", "≈ 1.5 ms", "≈ 0", "≈ 1.8 ms"],
    ], num_cols=(1, 2, 3, 4)))
    b.append('<p>The menu is the clean result (6 against 9 blocks, ±0.2 fps); the race line is suggestive. Dropping the mid-frame kick '
             'saved CPU but lost CPU/GPU overlap, so it netted zero. The New 3DS was neutral. Arm 3 ships as the v1.1.0 default on both '
             'consoles (<code>[stereo] onesubmit = 1</code>, <code>onesubmit_narrow = 1</code>).</p>')
    return "".join(b)


def memory():
    b = ['<h3>Memory</h3>']
    b.append('<p>The Old 3DS margin was thin at v1.0.0 and stayed the main source of post-release bugs:</p>')
    b.append('<ul>'
             '<li><b>HOME close hard-locked the console</b> (card 5). The 64DD save flush at exit wanted one contiguous 73 KB buffer; '
             'about 390 KB was free, but not in one piece. <code>bad_alloc</code> led to <code>svcBreak</code>, which locks instead of '
             'exiting. The flush now streams with no heap allocation, and a break during teardown exits. Card 6 logged a clean exit.</li>'
             '<li><b>The fast track path dropped after six course loads.</b> A 96,768-byte allocation failed with 1.6 MB free: fragmentation, '
             'not exhaustion. The track now lives in one 777,360-byte arena reserved at boot (emulator-verified over a 6-course Grand Prix).</li>'
             '<li><b>About 320 KB moved off the main heap</b>, mostly the bottom screen\'s art buffers, which had been static arrays in the '
             'program image.</li>'
             '</ul>')
    return "".join(b)


def forced():
    b = ['<h3>What the Old 3DS forced</h3><ul>']
    b.append('<li><b>Leaving the display list in races.</b> A faster interpreter on a second core was enough on the New 3DS; at a third '
             'of the clock with no L2 and no spare core, the interpreter\'s own code did not fit in cache.</li>'
             '<li><b>A hardware profiler.</b> The emulator models neither the I-cache nor the console\'s memory permissions, so its wins '
             'stopped predicting hardware. ARM11 performance counters became the judge.</li>'
             '<li><b>A memory diet.</b> A smaller RDRAM arena, streamed audio tables, capped uploads, a pinned heap layout, and a '
             'high-water mark instead of a free-list figure that had been misread as headroom.</li>'
             '<li><b>Frame pacing as design.</b> With a millisecond or two of margin, when the swap is requested matters: a swap gate, an '
             'end-of-frame mode, and a race-scoped frame-skip guard.</li></ul>')
    return "".join(b)


def section(D):
    O = json.load(open(os.path.join(os.path.dirname(__file__), "o3ds-data.json")))
    cats = O["cats"]
    b = [pb_tabs.STYLE, pb_calendar.STYLE, '<div class="pb-o3">']
    b.append('<h2 id="old-3ds">The Old 3DS road</h2>')
    b.append('<p>The 08-11 feasibility study said the port could not fit an Old 3DS (268 MHz, no L2 cache, 64 MB). It booted there on '
             '09-29 at about 10 fps, raced at 60 in 2D on 10-02 and in 3D on 10-08. Every card was a physical SD-card trip.</p>')
    b.append(table(["Old 3DS, 10-26", "Now", "Measured on", "Before"], [
        ["1P race, 2D", "<b>60 fps</b>, median frame 16.5 ms, 0 hitches", "card 3, 10-02", "~10 fps, 09-29"],
        ["1P race, 3D", "<b>59.7–59.8 fps</b> with one submission (suggestive)", "v1.1 card 6, 10-26", "56.4 / 59.3 baseline; 53.7 with 16 machines (10-24)"],
        ["Menus, 3D", "<b>44.6 fps</b>", "v1.1 card 6, 10-26", "40.2"],
        ["Multiplayer", "2D <b>60 fps</b> (2 players + 14 CPU); 3D <b>locked 30</b> by design", "v1.1 card 3, 10-25; 10-24 probe", "new in v1.1.0"],
    ]))
    b.append('<h3>Every card, on the calendar</h3>')
    b.append('<p>A card is one round trip: build, ini, test plan, play session, log back. Hover or tap a card for what it carried.</p>')
    b.append(pb_calendar.calendar(
        [(2026, 9), (2026, 10)], calendar_cards(O), dt.date(2026, 9, 24), dt.date(2026, 10, 26),
        marks={"09-24": "milestone reopens", "09-25": "fits (sim)", "10-19": "v1.0.0", "10-20": "v1.1.0 work", "10-26": "v1.1.0"},
        pauses=[("10-04", "10-06", "rate limit"), ("10-09", "10-14", "rate limit")]))
    b.append(figure(fps_over_time(),
                    "<b>Old 3DS race frame rate over time.</b> One dot per figure a hardware log named. Hollow dots are estimates from "
                    "documented ranges. Before the native race renderer the game also ran in slow motion. The 10-24 dip is a heavier "
                    "16-machine measurement, not a regression.",
                    legend([("2D, 1P", "--c1"), ("3D, 1P", "--c3"), ("2D multiplayer", "--c4")])))
    b.append('<h3>The machine</h3>')
    b.append(table(["", "Old 3DS", "New 3DS"], [
        ["CPU", "2 × ARM11, 268 MHz, no L2", "4 × ARM11, up to 804 MHz, 2 MB L2"],
        ["L1 instruction cache", "16 KB; a miss costs ~37 cycles (measured)", "misses can hit L2"],
        ["App memory", "64 MB region, 24 MiB heap grant", "124 MB"],
        ["Usable cores", "core 0; core 1 is a 30% share (audio)", "core 0 + core 2"],
    ]))
    b.append('<h3>Five phases</h3>')
    b.append('<p><b>1 · Make it fit (09-24 → 09-29).</b> RDRAM arena halved to 8 MiB, <code>audio_table</code> streamed; replay 49/49 '
             'with 2.65 MiB spare under simulation, and the console matched it byte for byte. One flag, <code>-DGDX_3DS_O3DS=ON</code>, '
             'makes the Old 3DS build.</p>')
    b.append('<p><b>2 · Find the costs (09-29 → 09-30).</b> First race: 96 ms a frame, GPU busy 3.6 ms. The audio lock wait went 25.8 → '
             '0.2 ms, ARMv6 SIMD made synthesis 2.2× cheaper, and frame skipping fixed the slow motion ("playable but nausea inducing"). '
             'A retained renderer won in the emulator and did almost nothing on hardware.</p>')
    cp = O["cpi_probe"]
    svg = grouped_bars([c[0] for c in cp], [("CPI", "--c5", [c[1] for c in cp])], ymax=6.5, ylabel="cycles per instruction", dec=2,
                       h=230, w=460, ymarks=[0, 2, 4, 6])
    b.append('<div class="cols2"><div>'
             '<p><b>3 · The probe that changed the plan (09-30 → 10-01).</b> Performance counters: CPI 4.34, 43.6% of cycles stalled on '
             'instruction fetch. A probe loop ran at one instruction per cycle with 16 KB of hot code and four at 24 KB. Research called '
             'native 60 "marginal" and offered pose replay; the user: "we should go for a real 60!"</p></div>'
             + figure(svg, "<b>The I-cache cliff, Old 3DS hardware.</b> One loop at three hot-code sizes.") + '</div>')
    svg = grouped_bars(cats["names"], [("interpreter + early levers", "--c8", cats["rel4"]), ("card 3 (native renderer)", "--c1", cats["card3"])],
                       ylabel="ms per rendered frame, hardware", dec=2, h=260)
    s4 = sum(cats["rel4"]); s3 = sum(cats["card3"])
    b.append('<p><b>4 · The native race renderer (10-01 → 10-02).</b> One small loop per race category, each sized to the 16 KB L1-I, '
             'writing command words from GPU-resident meshes. Game logic untouched and hash-checked every tick.</p>')
    b.append(figure(svg, f"<b>CPU per rendered frame by category, 10-01 vs card 3 (10-02).</b> Sum {s4:.1f} → {s3:.1f} ms; logic per "
                    f"tick {cats['tick_rel4']['logic']} → {cats['tick_card3']['logic']} ms, unchanged by design.",
                    legend([("before, 10-01", "--c8"), ("card 3, 10-02", "--c1")])))
    b.append('<p><b>5 · 3D (10-03 → 10-08).</b> The frame body is built once and resubmitted for the right eye with a per-eye uniform. '
             'The CPU fit, but frames missed vblank behind a 5–7 ms right-eye tail; interleaving per chunk and end-mode swaps gave '
             'Grand Prix windows at 59–60 on 10-08.</p>'
             + pb_tabs.later("a heavier 16-machine race measured 53.7 fps and idling the GPU first did not help, so 3D here is "
                             "CPU-bound and multiplayer 3D is locked to 30. One submission (below) raised the 1P race to 59.7–59.8.",
                             "10-24 → 10-26", "hwlogs/o3ds-0c2, o3ds-card6; ed04b17"))
    b.append(onesubmit())
    b.append(memory())
    b.append(forced())
    b.append('<h3>Still open</h3><ul>'
             '<li><b>Heavier venues</b> (Silence, Sand Ocean) can step 3D down to 30.</li>'
             '<li><b>The results table</b> after a race is still heavy.</li>'
             '<li><b>Brief audio gaps during loads.</b> The vblank-rate crackle theory was measured and refuted.</li>'
             '<li><b>Memory</b> remains a listed known issue.</li></ul>')
    b.append('</div>')
    return "".join(b)
