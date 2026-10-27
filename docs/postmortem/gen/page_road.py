import os
from framework import *
import statistics as st
import pb_tabs


def stats(f):
    f = sorted(f); n = len(f)
    return st.median(f), f[int(0.1 * n)], 100 * sum(1 for x in f if x >= 59.5) / n, n


def hist(fps, bins=(0, 30, 35, 40, 45, 50, 55, 58, 59.5, 61)):
    counts = []
    for lo, hi in zip(bins[:-1], bins[1:]):
        counts.append(sum(1 for x in fps if lo <= x < hi))
    labels = [f"{lo:g}–{hi:g}" if hi < 61 else "≥59.5" for lo, hi in zip(bins[:-1], bins[1:])]
    return labels, [100 * c / len(fps) for c in counts]


def build(D, P):
    S = P["sessions"]; profs = P["profs"]
    b = []
    b.append('<p class="eyebrow">04 · Road to 60</p><h1>The road to sixty</h1>')
    b.append('<p class="lede">F-Zero X runs its logic at 60 Hz and the port never lowered that. Every frame the CPU walks the game\'s '
             'display list, translates it, and feeds the PICA. The road to 60 was the road from a 35–58 ms frame build to under 16.7 ms, '
             'measured first in the emulator and then, from 08-21, on the console. This page is that road, lever by lever.</p>')

    r1, r5 = S[0], S[-1]
    m1, p1, c1, n1 = stats(r1["fps"]); m5, p5, c5, n5 = stats(r5["fps"])
    b.append(tiles([
        (f"{m1:.1f}<small>→</small>{m5:.1f}", "median fps, hardware, round 1 → round 5 of the LOCKED-60 campaign", True),
        (f"{p1:.1f}<small>→</small>{p5:.1f}", "tenth percentile (the crowd floor)", True),
        (f"{c1:.0f}%<small>→</small>{c5:.0f}%", "of one-second beats at the 60 cap", True),
        ("12–20", "fps in the first emulator race, 08-14", False),
        ("35–55", "fps on the first hardware run, 08-21 (no stereo)", False),
        ("2", "levers built to completion and shelved on hardware evidence", False),
    ]))

    b.append('<h2>Where the frame went: the CPU-bound verdict</h2>')
    b.append('<p>The measurement pass of 08-20 settled the strategy. GPU drawing time was flat at 0.4 ms per frame and the loop never once '
             'waited on the PICA; CPU build time was 26 to 58 ms. Fill rate, model decimation and texture compression were struck from the plan '
             'as irrelevant to frame time (the whole race used 8 KB of texture bytes and 957 vertices per frame). The bottleneck was the '
             'software display-list interpreter and the bridge that feeds it. Every lever from then on attacked CPU time, and every profile '
             'was reported as a set of named buckets:</p>')
    b.append(table(["Bucket", "What it is"], [
        ["<code>br</code>", "the bridge pre-pass: converting the game's raw display list into the runtime's command form, resolving addresses"],
        ["<code>dsp</code>", "interpreter dispatch: walking the converted list and executing every command"],
        ["<code>vtx</code>", "vertex transform: projecting N64 vertices to clip space on the CPU"],
        ["<code>tri</code>", "per-triangle work: tile extents, shader lookup, clip parameters, packed VBO emission"],
        ["<code>imp</code>", "texture import: looking up or uploading textures for a draw"],
        ["<code>drw</code>", "issuing the draw to citro3d, including the stereo target switch"],
    ]))

    b.append('<h2>The emulator era: 08-14 → 08-21</h2>')
    b.append('<p>Before the console arrived every number was an Azahar proxy, known to be pessimistic on menus and unrepresentative in '
             'absolute terms. The shape of the work still held.</p>')
    b.append(table(["Date", "Lever", "Measured (emulator)"], [
        ["08-14", "First race", "12–20 fps race, 15–20 fps menus, debug logging on"],
        ["08-20", "Frame pacer double-throttle fix, HLE audio producer to core 2, malloc histogram", "measurement pass: CPU 26–58 ms vs GPU 0.4 ms"],
        ["08-21", "<b>Loadblock discovery.</b> Per-opcode profiler named <code>G_LOADBLOCK</code> at 22.34 ms per frame: each 4 KB load did 512 shared_ptr copies (a thousand atomic RMWs), a 4 KB memcpy and a live getenv", "F3 22.34 → 2.21 ms; dispatch 27.6 → 8.9 ms; menu wall 35–38 → 16.7–19.9 ms. User: \"blazing fast\""],
        ["08-21", "Flat dispatch table replacing libultraship's layered handler tables", "neutral on time, enabled per-opcode profiling"],
        ["08-21 night", "S7 interpreter memo (raw dispatch, geo-diag gate, tri-state memo), async frame-mirror copy, vblank-stall skip, upload thrash 155 → 0.5 per frame, resolutions 93 → 0", "menu 15 → 19.9 fps; race build 26–58 → 24–46 ms; title wall 34 → 21–27 ms"],
        ["08-21", "Texrect state fast path (batch same-state runs)", "E4 texrect bucket 6–9.7 ms attacked"],
    ]))

    b.append('<h2>Hardware, before the campaign: 08-21 → 08-28</h2>')
    b.append(table(["Date", "Build", "Hardware result (user + log)"], [
        ["08-21", "First .cia and .3dsx, rspolish", "35–55 fps; median 48–50, p95 58, max 59 over 8 races; crowd dips 25–35. \"wow it runs amazing\""],
        ["08-27", "Fleet drop: traffic grind (bridge region cache, binary-search asset lookup, pipesync no-op), sky wedge, CCMUX 11, stereo armed, shadow", "\"performance is much better! 3D works great, doesnt seem to affect performance much at all\"; one hard crash (filelog OOM, fixed same day)"],
        ["08-28", "Touch menu, full-bleed display, rival detail option, triloop packed VBO, leak fix (upstream LUS bug)", "median 51, p95 60, max 60; MINIMAL rival detail = +8–9 fps in crowds, floor ~40 (was 25–35)"],
        ["08-28", "GPU vertex transform moonshot", "imperceptible on hardware; venue floor loses texture mapping (24-bit uniform precision). Shelved."],
    ]))

    b.append('<h2>The LOCKED-60 campaign: 09-01 → 09-03</h2>')
    b.append('<p>Five hardware rounds in three days, each a staged branch with a test plan, each measured from the console\'s own '
             'once-per-second fps beats in the log. Stereo was accidentally off in rounds 1 and 2 and on from round 3, which makes the '
             'later gains larger than they look.</p>')
    cats = [f"{s['name']}|{s['sub'][:26]}" for s in S]
    med = [stats(s["fps"])[0] for s in S]; p10 = [stats(s["fps"])[1] for s in S]
    svg = grouped_bars(cats, [("median fps", "--c1", med), ("p10 fps", "--c2", p10)], ymax=66, ylabel="fps (1-second beats)", dec=1, cap=60,
                       ymarks=[0, 20, 40, 60])
    b.append(figure(svg, "<b>Median and floor, round by round.</b> Round 1 is mainline before the campaign (754 beats over 63 minutes). "
                    "Round 5 is a full Grand Prix with the render thread in ahead mode, the bridge on the main core and automatic rival detail.",
                    legend([("median", "--c1"), ("p10", "--c2")])))
    cap = [stats(s["fps"])[2] for s in S]
    svg = grouped_bars(cats, [("% of beats at 60", "--c4", cap)], ymax=70, ylabel="% of beats ≥ 59.5 fps", dec=0, ymarks=[0, 25, 50])
    b.append(figure(svg, "<b>Time spent at the cap.</b> The fraction of one-second windows pinned at 60 went from one in eleven to more than half."))

    b.append(table(["Round", "What it carried", "Conditions", "Beats", "Median", "p10", "At cap"], [
        [esc(s["name"]), esc(s["sub"]), esc(s["cond"]), esc(s["size"]), f"{stats(s['fps'])[0]:.1f}", f"{stats(s['fps'])[1]:.1f}", f"{stats(s['fps'])[2]:.0f}%"]
        for s in S], num_cols=(4, 5, 6)))

    l1, h1 = hist(r1["fps"]); l5, h5 = hist(r5["fps"])
    svg = grouped_bars(l1, [("Round 1", "--c8", h1), ("Round 5", "--c1", h5)], ylabel="% of beats", dec=0, ymax=max(h1 + h5) * 1.15)
    b.append(figure(svg, "<b>The distribution moved, not just the median.</b> Round 1 has a long tail below 45 fps (crowd starts, transitions). "
                    "Round 5 stacks its mass in the top bin.", legend([("Round 1, before", "--c8"), ("Round 5, after", "--c1")])))

    b.append('<h2>Where the milliseconds came from</h2>')
    b.append('<p>Hardware profiles with the trace on (which itself inflates the numbers by a few milliseconds) on the same course, crowd '
             'windows (30 machines in view) and steady windows. Buckets in milliseconds per frame.</p>')
    keys = ["br", "dsp", "vtx", "tri", "imp", "drw"]
    names = ["bridge", "dispatch", "vertex", "triangle", "import", "draw"]
    cols = ["--c6", "--c1", "--c3", "--c2", "--c7", "--c4"]
    cats = [f"{p['name']}|crowd" for p in profs] + [f"{p['name']}|steady" for p in profs]
    series = []
    for k, nm, c in zip(keys, names, cols):
        series.append((nm, c, [p["crowd"][k] for p in profs] + [p["steady"][k] for p in profs]))
    svg = stacked_bars(cats, series, ylabel="ms per frame (trace on)", dec=1, h=340)
    b.append(figure(svg, "<b>Per-bucket frame time on hardware.</b> The campaign levers shaved the triangle and dispatch buckets; the render thread "
                    "took dispatch from 7.8 to 4.9 ms in crowds by overlapping it with game logic on core 2. The 16.7 ms budget line is the whole "
                    "bar; anything under it is a 60 fps frame.", legend(list(zip(names, cols)))))
    b.append(table(["Profile", "Window", "br", "dsp", "vtx", "tri", "imp", "drw", "Total"], [
        [esc(p["name"]) + f' <span class="pill neu">{esc(p["sub"])}</span>', w, p[w]["br"], p[w]["dsp"], p[w]["vtx"], p[w]["tri"], p[w]["imp"], p[w]["drw"], f"<b>{p[w]['total']}</b>"]
        for p in profs for w in ("crowd", "steady")], num_cols=(2, 3, 4, 5, 6, 7, 8)))

    b.append('<h2>Every lever, and its verdict</h2>')
    G = '<span class="pill good">mainline</span>'; N = '<span class="pill bad">NO-GO</span>'; W = '<span class="pill warn">shelved</span>'; O = '<span class="pill neu">default off</span>'
    b.append(table(["Lever", "Idea", "Measured", "Verdict"], [
        ["Loadblock span store", "One record per 4 KB load instead of 512 shared_ptr copies; same-content skip; getenv latch", "F3 22.3 → 2.2 ms, dispatch 27.6 → 8.9 ms (emu)", G],
        ["Flat dispatch + [prof]/[profop]", "Plain table dispatch; per-bucket and per-opcode timers", "neutral; enabled everything after", G],
        ["S7 interpreter memos", "Raw-pointer dispatch, gated geo diagnostics, tri-state memo", "build 34 → 23.5 ms (emu); one 2× regression bisected out (dlcache)", G],
        ["Async frame mirror, vblank-stall skip", "Menus stop waiting on a copy and on a late vblank", "menu 15 → 19.9 fps; wVbl 10–16 ms → 0 (emu)", G],
        ["Texrect fast path", "Batch same-state rectangle runs", "E4 bucket attacked; superseded by the HUD atlas", G],
        ["Traffic grind", "Bridge region cache (no svcQueryMemory per probe), binary-search asset lookup, pipesync no-op", "br 19.7 → 10.4 ms, wall 55.7 → 46.7 ms (emu)", G],
        ["Prim/env value-change flush", "Correctness first: flush on colour change", "fixes machine tint; small cost", G],
        ["Rival detail (NATIVE/REDUCED/MINIMAL)", "Bias the game's own LOD tiers for non-player machines beyond the 5 nearest", "+8–9 fps in crowds on hardware", G],
        ["Triloop packed VBO", "Single-write PICA-layout emission from the interpreter", "kills the double vertex copy", G],
        ["Leak fix (uninitialised ColorCombinerKey::shader_id)", "Upstream LUS bug: combiner map missed per draw", "heap flat; hidden per-draw flush tax gone", G],
        ["Double-height stereo target", "One 400×480 target, per-eye viewport, fewer PICA flushes", "user observed 2D ≈ 3D on hardware; citro3d source says flush cost is GPU-side and the GPU idles", W],
        ["GPU vertex transform (moonshot)", "Unlit vertex transform on the PICA vertex shader", "works (220 k draws, zero fallback, −62% vtx emu); imperceptible on HW; 24-bit uniform precision breaks ±32000-unit floors", W],
        ["Bridge output cache", "Cache converted display lists across frames, epoch-validated", "census: 87% of walked commands are host-built per frame; ceiling ~1.5 ms", N],
        ["brfast (bridge memos)", "Per-list facts memo, address-resolve memo, placeholder/raw-copy/range-class memos", "br 11.5 → 4.9 ms (emu); HW br 6.43 → 3.73 in crowds; median 56.0 → 57.2, p10 42.0 → 47.2", G],
        ["HUD texrect atlas (trectbatch)", "Pack HUD rectangles into atlas pages; batches stay open across views", "crowd draws 136 → 26 (E4 −2.6 ms emu)", G],
        ["trifast", "Fast packed triangle loop; found the S7 memo had never actually hit (flag bug)", "tri −15..−19% (emu); HW tri 5.58 → 4.64 ms crowd", G],
        ["tmemfast", "TMEM load bookkeeping", "F3 −22..−35% (emu)", G],
        ["audioprime / audioprime2", "Boot audio underrun: title font sample load blocked on a 10.7 MB inflate", "store audio_table uncompressed in the o2r: preload 4.3 → 1.97 s", G],
        ["dspfast (batch-break fold)", "Fold prim/env changes to avoid batch splits", "census: texture switches split anyway; cannot pay", O],
        ["Machine texture atlas (atlas3d)", "Atlas machine-part textures to cut switches", "only ~21 imports per frame split a batch; 0.1–0.5 ms best case; two thirds of machine textures wrap or mirror", O],
        ["Render thread, core 2 (mode 1, pipe)", "Interpreter and draws on core 2, forked at osSpTaskStartGo", "HW: stable, HOME clean; median 49 → 56, p10 42 → 48 (trace on); not yet overlapping (waitMain 10–15 ms)", G],
        ["Render thread mode 2 (ahead)", "DP-done acknowledged when the game parks; 2-deep backpressure; true one-frame pipeline", "HW round 5: median 59.6, p10 51.4, 53% at cap", G],
        ["Bridge on main (bridgemain)", "Bridge pre-pass runs on core 0 while core 2 renders", "render-thread br 4 → 0 ms; brMain 3.6 ms; balanced cores", G],
        ["Auto rival detail (dynlod)", "Raise the LOD tier when render > 15 ms, lower when < 12 ms; user setting is the floor", "shipped in round 5; thresholds untuned on HW", G],
        ["Render-thread-owned texture cache", "Fixes the round-5 data abort: clears become requests drained on the render thread", "receipt texcacheMainMut=0; awaiting HW", G],
    ]))

    body = "".join(b)
    return compose(body[body.index('<h2>Where the frame went'):])
    # continuation_note() is deliberately NOT appended here: build.py appends it exactly once, after
    # wrapping this page (for both the live and frozen/reframed paths). Appending it here too would
    # duplicate it on every build — see framework.reframe()'s docstring.


def continuation_note():
    """build.py appends this once, after page()-wrapping, on every build path. The forward pointer it used to
    carry now lives in the page's own header ("Where we are now", compose() below), so this is empty."""
    return ""


# ---------------------------------------------------------------------------------------------------------
# Current-status header + dated history. In frozen mode (no session scratchpad) the historical body comes
# from pb_road_history.html, a verbatim snapshot of the last page rendered from the raw fps samples; the
# live path (build() above) produces the same body and goes through compose() too.
# ---------------------------------------------------------------------------------------------------------
HERE = os.path.dirname(os.path.abspath(__file__))


def build_current(D):
    return compose(open(os.path.join(HERE, "pb_road_history.html")).read())


def _later(text, when, src, held=False):
    return pb_tabs.later(text, when, esc(src), held=held)


# (anchor text in the historical body, what to put after it). Each anchor must occur exactly once.
ANNOTATIONS = [
    ('<h2>Where the frame went: the CPU-bound verdict</h2>',
     '<p class="pb-era">New 3DS · 08-20 measurement pass (emulator), confirmed on hardware 09-01</p>'),
    ('<h2>The LOCKED-60 campaign: 09-01 → 09-03</h2>',
     '<p class="pb-era">New 3DS hardware · v0.9.0-era libultraship interpreter build · rounds 1–5</p>'),
    ('<h2>Where the milliseconds came from</h2>',
     '<p class="pb-era">New 3DS hardware · 09-02 → 09-03 · trace on</p>'),
    ('<h2>Every lever, and its verdict</h2>',
     '<p class="pb-era">Verdicts as of 09-03 (v0.9.0); later changes noted in the rows</p>'),
    ('receipt texcacheMainMut=0; awaiting HW',
     _later("the render thread is off in the v1.0.0 presets, so this fix is off the default path.", "10-19", "gdx3ds_preset_n60.c")),
    ('breaks ±32000-unit floors',
     _later("a machines-only <code>gpuxf = 1</code> ships on by default on both consoles; the floor stays on the CPU.", "09-29 → 10-19",
            "o3dsgpuxf-progress.md; gdx3ds_preset_n60.c")),
    ('HW round 5: median 59.6, p10 51.4, 53% at cap',
     _later("off by default since v1.0.0 (<code>renderthread = 0</code>): the native race path fits on one core, and New 3DS 3D races "
            "held 60 in every window on card 6 without it.", "10-19 → 10-26", "gdx3ds_preset_n60.c; o3ds-card6-analysis.md §4")),
    ('+8–9 fps in crowds on hardware',
     _later("the New 3DS preset now runs full rival detail; automatic scaling is an Old 3DS lever.", "10-19", "RELEASE-NOTES.md v1.0.0")),
    ('shipped in round 5; thresholds untuned on HW',
     _later("kept in the Old 3DS <code>o3ds-auto</code> preset; not used by the New 3DS preset.", "10-19", "RELEASE-NOTES.md v1.0.0")),
    ('flush cost is GPU-side and the GPU idles',
     _later("both eyes did end up in one GPU submission, by another route (a <code>CMDBUF_JUMP</code> per stereo chunk): 3D menus "
            "+11% on Old 3DS, neutral on New 3DS; v1.1.0 default.", "10-26", "o3ds-card6-analysis.md")),
    ('ceiling ~1.5 ms',
     _later("the NO-GO stands; the native fork removed the bridge from the race path instead.", "10-02", "native.html", held=True)),
]

END_NOTE = ('<h2>Where this road ended</h2>'
            '<p>The interpreter path topped out here: a median a hair under 60 on the New 3DS, with the crowd start still the floor. '
            'For the Old 3DS it was a dead end, about 10 fps on first boot, far short of 60. That is '
            'what led to the native fork: <a href="native.html">The road to 60, all over again</a>.</p>')

STYLE = """<style>
.pb-era { font-family:var(--sans); font-weight:700; font-size:10.5px; letter-spacing:.12em; text-transform:uppercase; color:var(--ink3);
  margin:-6px 0 10px; }
.pb-now { border-top:3px solid var(--good); padding:2px 0 4px; margin:26px 0 10px; }
.pb-now h2 { margin-top:12px; border-top:0; padding-top:0; }
.pb-hist { margin:48px 0 0; padding:12px 0; border-top:1px solid var(--rule); border-bottom:1px solid var(--line); font-size:14px; color:var(--ink2); max-width:78ch; }
</style>"""


def now_table():
    return table(["Console · mode", "Now (date, build)", "Then"], [
        ["<b>New 3DS</b> · 1P race, 3D", "<b>60 fps</b> in every ~1 s window; render task 2.5–3.8 ms, ~13 ms of headroom "
         "(card 6, 10-26, ba0692e; widescreen on)", "median 59.6, p10 51.4, 53% of beats at 60 (round 5, 09-03)"],
        ["<b>New 3DS</b> · 1P race, 2D", "no separate native-renderer figure on file (v1.0.0 notes: not yet measured); 3D, the heavier mode, holds 60",
         "median 57.2 with stereo off (round 2, 09-02)"],
        ["<b>New 3DS</b> · menus, 3D", "medians 56–61 fps (card 6; few windows, noisy)", "~20 fps in the emulator (08-21)"],
        ["<b>Old 3DS</b> · 1P race, 2D", "<b>60 fps</b>: median frame 16.5 ms, 0 hitches (card 3, 10-02); full-field GP 57.3 fps mean "
         "with diagnostic logging (card 2, 10-02)", "unsupported until 09-28; ~10 fps on first boot (09-29)"],
        ["<b>Old 3DS</b> · 1P race, 3D", "<b>59.7–59.8 fps</b> with one submission per stereo chunk, baseline 56.4 / 59.3 (card 6, 10-26; "
         "two blocks per arm); a heavier 16-machine race 53.7 fps (10-24)", "59–60 in race windows on courses 12–16 (card 4, 10-08)"],
        ["<b>Old 3DS</b> · menus, 3D", "<b>44.6 fps</b>, from 40.2 (card 6, 10-26)", "interpreter menus with frame skipping (v1.0.0)"],
        ["<b>Old 3DS</b> · multiplayer", "2D: <b>60 fps</b> race windows, 2 players + 14 CPU (10-25, 3c0c7b7). 3D: <b>locked 30</b> by design (10-24)",
         "multiplayer is new in v1.1.0"],
        ["<b>New 3DS</b> · multiplayer", "no separate figure on file", "—"],
    ])


def compose(history):
    cut = history.index('<h2>What is left</h2>')
    history = history[:cut] + END_NOTE
    for anchor, add in ANNOTATIONS:
        assert history.count(anchor) == 1, anchor
        history = history.replace(anchor, anchor + add)
    b = [pb_tabs.STYLE, STYLE]
    b.append('<p class="eyebrow">04 · Road to 60</p><h1>The road to sixty</h1>')
    b.append('<p class="lede">F-Zero X runs its logic at 60 Hz and the port never lowered that. On 10-26 both consoles race at 60: the New '
             '3DS with headroom to spare, the Old 3DS at the edge in 3D. Most of this page is how the New 3DS got there first, from 08-14 to '
             '09-03, on a renderer that is no longer the default. Every number is labelled with its date and build.</p>')
    b.append('<div class="pb-now"><h2>Where we are now · 10-26, v1.1.0</h2>')
    b.append(tiles([
        ("60<small>fps</small>", "New 3DS 1P race in 3D, every window (card 6, 10-26)", "red"),
        ("59.7<small>fps</small>", "Old 3DS 1P race in 3D with one submission per stereo chunk (card 6, 10-26)", "red"),
        ("60 / 30", "Old 3DS multiplayer: 2D at 60 (10-25), 3D locked at 30 (10-24)", False),
        ("59.6", "New 3DS median at the end of this page's campaign (round 5, 09-03)", False),
    ]))
    b.append(now_table())
    b.append('<p style="font-size:13px;color:var(--ink3)">Since v1.0.0 (10-19) both consoles run the native renderer by default '
             '(<code>[perf] preset = auto</code>: <code>n60</code> on New 3DS, <code>o3ds-auto</code> on Old 3DS), with the render '
             'thread off. Sources: <code>hwlogs/n3ds-card6</code>, <code>o3ds-card6</code>, <code>o3ds-card3</code>, <code>o3ds-0c2</code>; '
             '<a href="../RELEASE-NOTES.md">RELEASE-NOTES.md</a>; <a href="../MULTIPLAYER.md">MULTIPLAYER.md</a>. The renderer and the '
             'Old 3DS road are on <a href="native.html">The road to 60, all over again</a>.</p>')
    b.append('<h3>What the Old 3DS work gave back to the New 3DS</h3><ul>'
             '<li><b>The default route.</b> The native renderer, per-eye stereo replay and end-of-frame pacing are the New 3DS default '
             'since v1.0.0 (<code>n60</code>, render thread off). The render-thread path below is still built in behind an explicit ini.</li>'
             '<li><b>Two audio levers</b> (<code>audio_ctx_narrow</code>, <code>audio_simd</code>) are on in the New 3DS preset too, though it '
             'never lacked audio headroom.</li>'
             '<li><b>Fixes for both models:</b> a backlight/HOME freeze, a save that could be lost on power loss (an SD rename quirk), and a '
             'log that never blocks the frame.</li></ul></div>')
    b.append('<p class="pb-hist"><b>Where we were.</b> Everything below is the New 3DS campaign of 08-14 → 09-03 (v0.9.0), on the '
             'libultraship-derived interpreter with a render thread on core 2. Notes marked <b style="color:var(--warn)">Later</b> say what '
             'changed afterwards.</p>')
    b.append(f'<div class="pb-road">{history}</div>')
    return page("road-to-60.html", "The Road to Sixty", "".join(b))
