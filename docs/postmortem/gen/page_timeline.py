import datetime as dt
from framework import *
import pb_tabs
from pb_tabs import entry

# ---------------------------------------------------------------------------------------------------------
# Revisions: claims made in an entry that later evidence revised, overturned or (held=True) checked and kept.
# Worked through in reverse chronological order (latest entry first); the footnote numbers follow that order.
# key: the entry id they attach to; short: a one-line form of the revision (kept as data; not rendered).
# ---------------------------------------------------------------------------------------------------------
REVISIONS = [
    ("c5-arena", "10-26 track arena", "ends the fragmentation failure → emulator-tested only",
     "built for card 6's failure (a 96,768-byte allocation refused with 1.6 MB free: fragmentation, not exhaustion); "
     "tested in the emulator, not yet on hardware.", "10-26", "RELEASE-NOTES.md v1.1.0 known issues; o3ds-card6-analysis.md §5.1"),
    ("c5-onesub", "10-26 one submission", "held; the race gain is suggestive",
     "the menu result (6 vs 9 blocks, ±0.2 fps) is solid; the race figure rests on two blocks per arm. The Old 3DS frame has 9 "
     "queue entries, not the 11 the pre-card notes assumed.", "10-26", "onesubmit-progress.md; o3ds-card6-analysis.md §0", True),
    ("c5-home", "10-26 HOME close", "a 617 KB “heap regression” was a false alarm",
     "while chasing this lock a 617 KB drop in free heap was reported as a regression. The two logs compared were a 2D "
     "multiplayer session and a 1P 3D session; the comparison was wrong, not the build.", "10-26", "o3ds-card6-analysis.md; 9d19a5c"),
    ("c5-ceiling", "10-24 ceiling probe", "“3D is CPU-bound” → true for races, not menus",
     "card 6 found 3D menus waiting 7.66 ms a frame on the GPU queue (per-entry latency); one submission per stereo chunk "
     "took them from 40.2 to 44.6 fps.", "10-26", "o3ds-card6-analysis.md §3"),
    ("c4-crackle", "10-16 audio crackle", "hypothesis refuted: held",
     "v1.1.0 turns the clamp and declick fixes on for the New 3DS, not yet confirmed to cure its crackle; Old 3DS residual "
     "crackle still open.", "10-20", "RELEASE-NOTES.md v1.1.0 Audio; 9ead784", True),
    ("c4-mem", "10-16 memory ceiling", "“left as a known limit” → mitigated, still listed",
     "v1.1.0 moved ~320 KB off the main heap and reserved the track at boot; card 6 showed the "
     "failure mode is fragmentation. Tight memory stays a known issue.", "10-26", "d7010a8; a2905ed; RELEASE-NOTES.md v1.1.0"),
    ("c4-dash", "10-15 DASH", "“well under a millisecond” → 4–5 ms on Old 3DS",
     "on Old 3DS hardware the DASH cost 4.2–5.6 ms a frame and was put on a 1 ms budget.", "10-23", "d9deb37; e99777b"),
    ("c4-intro", "10-08 card 4", "“ended the campaign at 59–60 in 3D” → not a locked 60",
     "v1.0.0 shipped 3D as “not yet a locked 60 fps”; a heavier 16-machine 1P race measured 53.7 fps (10-24); card 6's "
     "baseline ran 56.4 / 59.3 and one submission 59.7–59.8 (10-26).", "10-19 → 10-26", "RELEASE-NOTES.md; cpu-profile-answer.md §A.4"),
    ("c3-card4", "10-08 card 4", "59–60 fps in 3D → see 10-24 / 10-26",
     "a development card, not the shipped build. A 16-machine race later ran 53.7 fps; one submission brought the 1P race to "
     "59.7–59.8.", "10-24 → 10-26", "cpu-profile-answer.md §A.4; o3ds-card6-analysis.md"),
    ("c3-3d1", "10-03 3D first pass", "right eye per chunk → both eyes in one GX entry",
     "each chunk's two eyes became one queue entry (CMDBUF_JUMP): Old 3DS race entries 9 → 7, 3D menus 40.2 → 44.6 fps; v1.1.0 default.",
     "10-26", "ba0692e; a2905ed"),
    ("c3-pmu", "09-30 CPI 4.34", "held for that build; the native renderer changed the mix",
     "native-renderer builds cut instructions ~3.6× and I-misses ~4×, but CPI rose to 5.4–6.4 with I-fetch stalls at 28–32%: "
     "less code, not faster code.", "10-07 → 10-08", "cpu-profile-answer.md §A.3", True),
    ("c3-rel2", "09-30 log flushes", "root cause → refined twice",
     "a full ring later forced 1.1–2.8 s writes; fixed by an async writer, then a log that drops lines rather than block. "
     "v1.0.0's own figure still shows the gap (57.3 vs 51.9 fps).", "10-02 → 10-19", "difficulties.html; RELEASE-NOTES.md v1.0.0"),
    ("c3-rel1", "09-29 REL1 crash", "“latent bug never found”: held",
     "still unproven; code generation stays stock and hot-first ordering was never re-tested on hardware.",
     "10-26", "rel1crash-progress.md; agent-D-cpu-cuts.md", True),
    ("c3-draw", "09-29 10.9 µs per draw", "held (no source says ~9 µs)",
     "DrawProbe measured 10.9 µs flat over 30–300 draws (2,926 cycles). A uniform-free draw is 8.0 µs; a trimmed citro3d reached "
     "~6.8 µs in the emulator, unmeasured on hardware; race draws now bypass it.", "10-26", "cpu-profile-answer.md §A.2", True),
    ("c3-fit", "09-25 fits with 2.65 MiB", "→ real hardware is thinner",
     "a 10-16 hardware capture peaked at heapUsed ≈ 24.2 MB; card 6 logged a race watchdog at heapFree=3264.",
     "10-16 → 10-26", "STATUS.md; o3ds-card6-analysis.md §5.1"),
    ("c1-state", "09-04 state of play", "“GPU vertex transform stayed shelved” → it ships",
     "a machines-only <code>gpuxf = 1</code> (the floor stays on the CPU) is on in both consoles' auto presets in v1.0.0 and v1.1.0.",
     "09-29 → 10-19", "gdx3ds_preset_n60.c; o3dsgpuxf-progress.md"),
    ("c1-r5", "09-03 round 5", "render thread (ahead mode) → off by default",
     "v1.0.0's tuned presets set <code>renderthread = 0</code>: the native race path fits 16.7 ms on one core. Card 6: New 3DS 3D races "
     "at 60 in every window without it.", "10-19 → 10-26", "gdx3ds_preset_n60.c; o3ds-card6-analysis.md §4"),
    ("c1-research2", "09-02 research 2", "cross-core pipelining → built, then dropped",
     "the render thread shipped in v0.9.0 and was left out of the v1.0.0 presets (above).", "10-19", "gdx3ds_preset_n60.c"),
    ("c1-walk", "09-01 “nobody beyond the per-frame walk”", "→ the native fork went past it",
     "the native race renderer (10-01/02) skips building the game's display-list sections on rendered ticks and draws from "
     "GPU-resident meshes.", "10-02", "native.html"),
    ("c1-gpuxf", "08-28 moonshot", "“shelved” → back, for machines only",
     "the broken floor was traced to float24 cancellation on world-space vertices (up to ±32,000); a machines-only transform was "
     "rebuilt for the Old 3DS and ships in both presets.", "09-29", "o3dsgpuxf-progress.md; gdx3ds_preset_n60.c"),
    ("c1-rival", "08-28 rival detail", "+8–9 fps → now an Old 3DS lever",
     "the New 3DS preset runs full rival detail; automatic scaling is used on the Old 3DS.", "10-19", "gdx3ds_preset_n60.c; RELEASE-NOTES.md v1.0.0"),
    ("c1-filelog", "08-27 filelog fix", "the retained ring → three more fixes",
     "the 256 KB cap truncated a session (raised to 1 MB, 09-02); the ring then caused 26–270 ms and 1.1–2.8 s SD-write hitches "
     "(09-30), fixed by an async, dropping writer.", "09-02 → 10-02", "2344e88; difficulties.html"),
    ("c1-cpu", "08-20 CPU-bound verdict", "held",
     "an emulator measurement at the time (GPU 0.4 ms vs CPU 26–58 ms); hardware agreed (GPU 5.6 vs build 16.3 ms), and the Old 3DS "
     "probe agreed on 10-24.", "09-01 → 10-24", "locked60-campaign.md; NATIVE-RENDERER.md ceiling_probe", True),
    ("c1-feas", "08-11 “New 3DS only”", "→ overturned",
     "it fit under simulation on 09-25, booted on an Old 3DS on 09-29, and shipped for the Old 3DS in v1.0.0.",
     "09-25 → 10-19", "this page, chapter 3; RELEASE-NOTES.md v1.0.0"),
]

FN = {}
for _i, _r in enumerate(REVISIONS, 1):
    FN.setdefault(_r[0], []).append((_i, _r))


def notes(key):
    out = []
    for i, r in FN.get(key, []):
        held = len(r) > 6 and r[6]
        out.append((str(i), pb_tabs.later(r[3], r[4], esc(r[5]), held=held, fid=f"tl-fn{i}")))
    return out


def E(date, title, body, kind="", key=None):
    return entry(date, title, body, kind, notes(key) if key else (), eid=f"tl-{key}" if key else None)


def day_range(a, b):
    d0, d1 = dt.date(2026, int(a[:2]), int(a[3:])), dt.date(2026, int(b[:2]), int(b[3:]))
    return [(d0 + dt.timedelta(n)).strftime("%m-%d") for n in range((d1 - d0).days + 1)]


def chapter_chart(cpd, a, b, caption, label="commits per day"):
    days = day_range(a, b)
    svg = grouped_bars(days, [("commits", "--c1", [cpd.get(d, 0) for d in days])], ylabel=label, dec=0, h=200, vals_on=False)
    return figure(svg, caption)


NATIVE_CPD = {"09-12": 45, "09-13": 34, "09-17": 42, "09-20": 18, "09-23": 317, "09-24": 122, "09-25": 8,
              "09-26": 84, "09-27": 31, "09-29": 127, "09-30": 120, "10-01": 163, "10-02": 95, "10-03": 20,
              "10-07": 78, "10-08": 139, "10-15": 79, "10-16": 31, "10-17": 26, "10-18": 11, "10-19": 9}
AFTER_CPD = {"10-20": 32, "10-21": 49, "10-22": 38, "10-23": 16, "10-24": 22, "10-25": 24, "10-26": 19}


def build(D, commits_per_day, prompts_per_day):
    b = [pb_tabs.STYLE]
    b.append('<p class="eyebrow">03 · Every step</p><h1>Step by step: twenty-four days, then forty-three more, then one week</h1>')
    b.append('<p class="lede">Five chapters, one per tab: the New 3DS port (five bursts between three credit pauses), the native fork, '
             'the Old 3DS, shipping v1.0.0, and the week after it. Dates are local, from git and the session transcripts; quotes are the '
             'user\'s own words at the time.</p>')
    b.append('<p class="note">Every entry was re-checked against what came after it. Where later evidence revised a number, a root '
             'cause or a decision, the entry carries a numbered <b style="color:var(--warn)">Later</b> note; '
             '<b style="color:var(--good)">Held</b> marks a claim that was checked and stands.</p>')

    tabs = [
        ("1 · New 3DS", "08-11 → 09-03", chapter1(commits_per_day, prompts_per_day)),
        ("2 · Native fork", "09-07 → 09-23", chapter2()),
        ("3 · Old 3DS", "09-24 → 10-08", chapter3()),
        ("4 · Shipping", "10-08 → 10-19", chapter4()),
        ("5 · After v1.0.0", "10-20 → 10-26", chapter5()),
    ]
    b.append(pb_tabs.tabs("tl", tabs))
    b.append(pb_tabs.SCRIPT)
    return page("timeline.html", "Every Step", "".join(b))


def chapter1(commits_per_day, prompts_per_day):
    b = []
    days = day_range("08-11", "09-03")
    svg = grouped_bars(days, [("commits to the 3DS port", "--c1", [commits_per_day.get("2026-" + d, 0) for d in days]),
                              ("user messages", "--c2", [prompts_per_day.get("2026-" + d, 0) for d in days])],
                       ylabel="per day", dec=0, h=220, vals_on=False)
    b.append(figure(svg, "<b>Chapter 1, per day.</b> Commits touching the port and messages typed by the user. The gaps are three pauses "
                    "when the weekly allowance for the regular model (the \"Fable\" tier) ran out.",
                    legend([("commits", "--c1"), ("user messages", "--c2")])))
    b.append('<h3>Burst 1 · Research to first race (08-11 → 08-14)</h3><ol class="pb-entries">')
    b.append(E("08-11 03:16", "Deep research: is a 3DS port feasible?", "Verdict: New 3DS only; the Old 3DS 64 MB region cannot hold it.", key="c1-feas"))
    b.append(E("08-11 03:44", "Plan for a team of agents", "\"can we segment plan so we can have multiple work trees and multiple contributing agents ?\" A memory budget, a five-stream plan and a reviewer agent.", "big"))
    b.append(E("08-11 → 08-12", "Phase 0 and the carve spike", "libultraship compiles on devkitARM with a 13-line newlib patch; Phase 1 streams merged 08-12 21:45."))
    b.append(E("08-13", "\"start the crew\", then black screens", "Full game linked 01:33; then bzero, 64DD, path mangling, double swap, the SETTIMG guard, a DMA misroute. 13:17: \"i saw some random shapes briefly\"."))
    b.append(E("08-14 01:01", "First race", "\"omg it works ! i can race ! it looks messed up and runs at 12 to 20fps but it works!\" (after a race-time bad_alloc fix).", "gold big"))
    b.append(E("08-14 04:26", "Hardware prep, CI, docs", "CIA packaging, a hardware runbook, local CI; research on 60 fps and on stereo 3D."))
    b.append(E("08-14 14:06", "Pause 1", "\"we ran out of fable credits until next week\". Fog (pink road) still open.", "bad"))
    b.append('</ol><h3>Burst 2 · Playable, then hardware (08-20 → 08-22)</h3><ol class="pb-entries">')
    b.append(E("08-20 02:55", "Resume; the road appears", "Fog solved (per-vertex blend, not the depth LUT); fog, GPU profiling, stereo, perf and culling merged.", "big"))
    b.append(E("08-20 05:38", "\"run as many subagents on plausible tasks\"", "A fleet on HUD, shadow, sky, tunnel, km/h, pop-in. The measurement pass: CPU-bound, not GPU-bound.", key="c1-cpu"))
    b.append(E("08-20 22:45", "The HUD bug", "After eight wrong fixes: the bridge's ILP32 high-bits backstop dropped 13 SETTIMG a frame. \"omg finally!!\"", "gold"))
    b.append(E("08-21 13:03", "\"menus … still crazy slow at around 10fps\"", "A per-opcode profiler named G_LOADBLOCK at 22.34 ms a frame; fixed within the hour. \"blazing fast on the UI\".", "gold big"))
    b.append(E("08-21 15:54", "First hardware run", "\"wow it runs amazing ! … sound doesn't work btw\". Audio fixed by 21:22 after eleven SD trips (the ROM-side driver needed host porting).", "gold big"))
    b.append(E("08-21 22:49", "\"framerate at a crispy 60. then... maybe 3D?\"", "Race-start hitch and decal fixes. Hardware median 48–50 fps, p95 58."))
    b.append(E("08-22 01:31", "Pause 2", "\"ran out of tokens again\". Checkpoint 854b9d6.", "bad"))
    b.append('</ol><h3>Burst 3 · Product grade (08-27 → 08-28)</h3><ol class="pb-entries">')
    b.append(E("08-27 06:21", "Fleet drop", "Traffic grind (bridge 19.7 → 10.4 ms), sky wedge, CCMUX 11, off-axis stereo, shadow audit.", "big"))
    b.append(E("08-27 16:00", "Hardware with stereo", "\"3D works great, doesnt seem to affect performance much\"; first hard crash: the filelog grew without bound, fixed with a retained ring.", key="c1-filelog"))
    b.append(E("08-27 16:53", "Touch menu and full bleed", "\"the bottom screen should have a menu for these features\"; full-bleed made default. 19:30: \"i did the first GP!\""))
    b.append(E("08-28 00:18", "Rival detail, packed VBO, HOME exit", "\"minimal appears to be a 8 to 9 fps gain !\"", key="c1-rival"))
    b.append(E("08-28 04:27", "\"moonshot! lets go for it\"", "GPU vertex transform in one session: works, imperceptible on hardware, breaks the venue floor. Shelved.", "gold", key="c1-gpuxf"))
    b.append(E("08-28 13:17", "README and pause 3", "README-3DS and the release package. Then the third pause.", "bad"))
    b.append('</ol><h3>Burst 4 · The last big lever (09-01 → 09-02)</h3><ol class="pb-entries">')
    b.append(E("09-01 21:27", "\"the goal to 60fps\"", "Bridge output cache NO-GO (87% of commands are host-built each frame); brfast memos instead, bridge 11.5 → 4.9 ms. Research: nobody has shipped beyond the per-frame walk.", "big", key="c1-walk"))
    b.append(E("09-01 23:53", "\"the home button crashes the game!\"", "Suspend-path audit and audio park gate; the first combined A/B test card."))
    b.append(E("09-02 14:02", "Hardware round 2", "HOME works; close-from-HOME hang fixed. brfast: median 57.2 vs 56.0, p10 47.2 vs 42.0."))
    b.append(E("09-02 14:41", "LOCKED-60 campaign", "HUD atlas, TMEM bookkeeping, per-triangle cost, boot audio: four agents, landed by 13:40.", "big"))
    b.append(E("09-02 18:35", "\"VERY slow. massive step back\"", "A combined-lever HUD break; fixed, and \"screenshot the combined levers before staging\" became a rule.", "bad"))
    b.append(E("09-02 21:56", "Rounds 3 and 4", "Median 56, p10 48, 32% at the cap with stereo on; trace-on A/B steady 16.4 → 14.9 ms."))
    b.append(E("09-02 21:59", "Research 2", "Core 2 usable, core 3 not, no precedent for cross-core render pipelining. Agents on a dispatch census, boot audio, a render thread.", key="c1-research2"))
    b.append('</ol><h3>Burst 5 · Multi-core (09-02 night → 09-03)</h3><ol class="pb-entries">')
    b.append(E("09-02 → 09-03", "Render thread, sync → pipe → ahead", "Forked at osSpTaskStartGo; ahead mode acks DP-done when the game parks. Pipe mode on hardware: median 49 → 56."))
    b.append(E("09-03 13:20", "Round 5: median 59.6, p10 51.4, 53% at the cap", "Ahead mode, bridge on main, auto rival detail. Then a hard crash (texture-cache LRU on core 2), fixed by render-thread ownership.", "gold big", key="c1-r5"))
    b.append(E("09-03 13:46 → 16:32", "Menu scale and stereo markers", "Machine-select 1.6× too large (viewport fold); 1P/2P markers anchored at the ship's depth."))
    b.append(E("09-03 18:08", "This post-mortem", "\"lets write up a whole post mortem on this project.\"", "big"))
    b.append(E("09-04", "State of play (v0.9.0)", "Ahead mode, bridge-on-main, auto rival detail and the crash fixes all merged. The GPU vertex transform stayed shelved; the tunnel-roof bug (#27) was still open at v1.0.0.", key="c1-state"))
    b.append('</ol>')
    return "".join(b)


def chapter2():
    b = [chapter_chart(NATIVE_CPD, "09-07", "09-23", "<b>Chapter 2, per day.</b> Commits on the native-fork branch. No session transcripts "
                       "were archived for this period, so commits are the only daily signal.")]
    b.append('<p>A feasibility pass on 09-07 found the fix should start with the development loop, not the renderer: no network path to '
             'the console, no frame oracle, no desktop reference build. Over two weeks that loop was rebuilt (a network control and log link, '
             'input-recorder determinism, a Linux reference render container), and the first renderer tier landed: an owned F3DEX2 '
             'interpreter replacing libultraship\'s, a native race path (track, machines, sky, HUD, effects), and the first 64DD Expansion '
             'Kit pass. In full on <a href="native.html">The road to 60, all over again</a>.</p>')
    return "".join(b)


def chapter3():
    b = [chapter_chart(NATIVE_CPD, "09-24", "10-08", "<b>Chapter 3, per day.</b> Commits on the native-fork branch; per-card numbers "
                       "are on <a href=\"native.html#old-3ds\">The road to 60, all over again</a>.")]
    b.append('<ol class="pb-entries">')
    b.append(E("09-24", "The milestone reopens", "A stale note said the user owned no Old 3DS; they did. Under a simulated 64 MB grant: RDRAM arena halved to 8 MiB, <code>audio_table</code> streamed.", "big"))
    b.append(E("09-25", "It fits, in simulation", "Replay 49/49 under <code>memtype=0</code> with 2.65 MiB to spare.", key="c3-fit"))
    b.append(E("09-28", "The console says <code>memtype=0</code>", "Matches the simulation byte for byte; the shipped RDRAM-16 build dies in its boot preload."))
    b.append(E("09-29 00:10", "\"it ran ! at like 9fps\"", "Wall p50 96 ms, GPU busy 3.6 ms. \"is there anything we can do to avoid the display lists\"", "gold big"))
    b.append(E("09-29 day", "The parallel wave", "Audio-lock wait 25.8 → 0.2 ms, the HOME freeze (backlight session), bit-exact ARMv6 SIMD audio, the slow-motion discovery."))
    b.append(E("09-29 13:09", "\"we want this running at 60fps\"", "A design sub-orchestrator and five research and probe subagents.", "big"))
    b.append(E("09-29 15:16", "\"playable but nausea inducing\"", "Frame skipping: logic 57–60 ticks/s, drawn 19–28 fps. Draw cost measured at 10.9 µs per draw.", key="c3-draw"))
    b.append(E("09-29 19:39", "First release candidate does not boot", "Cold size-optimised code crashes, stock boots; code generation reverted. The latent bug was never found.", "bad", key="c3-rel1"))
    b.append(E("09-30 01:10", "Second candidate boots, hitching", "\"like 15fps ... a lot of awkward hitching\": synchronous SD log flushes, 26–270 ms on main.", key="c3-rel2"))
    b.append(E("09-30 02:09", "\"start as many agents as necessary\"", "Seven agents hit the session limit together and were resumed; then the disk filled.", "big"))
    b.append(E("09-30 → 10-01", "A hardware profiler", "CPI 4.34, 43.6% fetch stalls; a probe found the cliff from CPI 1.06 at 16 KB of hot code to 4.1 at 24 KB.", "gold", key="c3-pmu"))
    b.append(E("10-01 08:57", "\"we should go for a real 60!\"", "Pose replay (40 images/s, artifacts) set aside against a research verdict of \"marginal\"; an I-cache-first native race renderer begins.", "big"))
    b.append(E("10-01 22:41", "First native-renderer build freezes", "A verify mode rolled back the live GPU command queue; hardware only.", "bad"))
    b.append(E("10-02 09:28", "\"time trial was silk 60fps\"", "199,472 draws verified, 0 bad; Grand Prix 45–50 with log hitches. Saves after the first never persisted (fixed)."))
    b.append(E("10-02 23:19", "\"omg it ran amazing!\"", "Card 3: race median 16.5 ms, 0 hitches. \"lets focus on 3D at 60fps\".", "gold big"))
    b.append(E("10-03", "3D, first pass", "Black sky on an interim build, then \"60fps for 2d for both\", 3D in the 40s and 50s. The right eye is interleaved per chunk.", key="c3-3d1"))
    b.append(E("10-03 → 10-07", "Rate-limit pause", "One branch's uncommitted edits recovered on 10-07.", "bad"))
    b.append(E("10-07 22:51", "3D, second pass", "Mute City ~60 in 3D with end-mode pacing; heavier venues 16–18 ms."))
    b.append(E("10-08 02:08", "\"all 3 crashed\"", "Bottom-screen framebuffer in VRAM, which a CIA maps read-only for the CPU; the emulator does not enforce it.", "bad"))
    b.append(E("10-08 02:56", "\"did a long play on main, was good!\"", "VRAM change reverted: 3D Grand Prix race windows at 59–60 fps; 0 bad draws.", "gold big", key="c3-card4"))
    b.append('</ol>')
    return "".join(b)


def chapter4():
    b = [chapter_chart(NATIVE_CPD, "10-08", "10-19", "<b>Chapter 4, per day.</b> Commits on the native-fork branch; the week-long gap is a "
                       "rate-limit pause.")]
    b.append('<ol class="pb-entries">')
    b.append(E("10-08", "Card 4 closes the 3D push", "59–60 fps in 3D Grand Prix race windows on a development card; the rest of v1.0.0 followed. Commit- and document-sourced from here.", key="c4-intro"))
    b.append(E("10-07 → 10-08", "Expansion Kit: own title and save", "The last blocker to a second, side-by-side install."))
    b.append(E("10-08 → 10-15", "Rate-limit pause", "The longest gap of the project.", "bad"))
    b.append(E("10-15 → 10-17", "The bottom screen becomes DASH", "Lap, position, energy, splits, composited into the framebuffer at well under a millisecond of CPU time.", key="c4-dash"))
    b.append(E("10-16", "The Old 3DS memory ceiling, in simulation", "One large mid-race asset load can exhaust the budget (the generic archive loader); a hardware capture peaked near 24.2 MB. Left as a known limit.", "bad", key="c4-mem"))
    b.append(E("10-16", "An audio-crackle hypothesis, refuted", "Vblank vs audio rate measured and ruled out; a smaller resample fix ships.", key="c4-crackle"))
    b.append(E("10-18", "A New 3DS froze at the title screen", "The attract demo's full-field race overflowed the GPU command buffer; see <a href=\"native.html\">The road to 60, all over again</a>.", "gold big"))
    b.append(E("10-18 → 10-19", "Custom HUD mode shelved", "The three fixed DASH layouts ship; an idle view outside races."))
    b.append(E("10-19", "Disk streaming and Japanese inputs by default", "The 64DD image streams from SD on every build; Japan-region inputs accepted.", "gold"))
    b.append(E("10-19", "v1.0.0", "Four build targets from one tree, three downloads (Old 3DS, New 3DS, New 3DS + Expansion Kit; the Old 3DS Kit build stays from-source), replacing the New-3DS-only v0.9.0.", "gold big"))
    b.append('</ol>')
    return "".join(b)


def chapter5():
    b = [chapter_chart(AFTER_CPD, "10-20", "10-26", "<b>Chapter 5, per day.</b> Commits on the release branch since the v1.0.0 tag. "
                       "The Old 3DS numbers are on <a href=\"native.html#old-3ds\">The road to 60, all over again</a>.")]
    b.append('<ol class="pb-entries">')
    b.append(E("10-20 → 10-25", "Multiplayer", "Local wireless, same-Wi-Fi and online rooms, lockstep races with each console showing its own racer, a relay server, version gating. Old 3DS: 2D 60 in a 16-racer online race; 3D locked to 30. See <a href=\"../MULTIPLAYER.md\">MULTIPLAYER.md</a>."))
    b.append(E("10-24", "The Old 3DS 3D ceiling probe", "Waiting out the previous frame's GPU work first made the frame no cheaper (13.5 → 14.2 ms), so a deferred-wait redesign was dropped: 3D races here are CPU-bound.", key="c5-ceiling"))
    b.append(E("10-26", "HOME close, fixed", "Card 5's Old 3DS hard lock was a 73 KB allocation in the 64DD save flush; the flush now allocates nothing.", key="c5-home"))
    b.append(E("10-26", "One submission for both eyes", "From a homebrew developer's public question. Card 6 A/B: Old 3DS 3D menu 40.2 → 44.6 fps, race GX entries 9 → 7, 1.5–1.8 ms of headroom; New 3DS neutral. v1.1.0 default.", "gold big", key="c5-onesub"))
    b.append(E("10-26", "Track arena and a wider 3D sky", "The course track moves into one 777,360-byte arena reserved at boot, aimed at the fragmentation failure card 6 hit; sky, floor and clouds widened by the eye shift.", key="c5-arena"))
    b.append(E("10-26", "The bottom screen, last pass", "DASH layout, D-pad navigation in the multiplayer sheets, one SETTINGS button."))
    b.append(E("10-26", "v1.1.0, prepared for release", "This page's last entry.", "big"))
    b.append('</ol>')
    return "".join(b)
