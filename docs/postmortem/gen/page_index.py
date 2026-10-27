from framework import *
import pa_facts as F

PA_STYLE = ('<style>.pa-rel { list-style:none; padding:0; margin:0 0 24px; max-width:72ch; } '
            '.pa-rel { border-top:1px solid var(--rule); } .pa-rel li { padding:10px 0; border-bottom:1px solid var(--line); font-size:16px; } '
            '.pa-rel li b { font-family:var(--sans); font-size:21px; letter-spacing:-.02em; margin-right:8px; } '
            '.pa-rel li span { font-family:var(--sans); font-size:11px; font-weight:700; letter-spacing:.1em; text-transform:uppercase; color:var(--ink3); margin-right:10px; font-variant-numeric:tabular-nums; } '
            '.pa-nw { white-space:nowrap; } .pa-mp img { max-height:440px; object-fit:contain; } '
            '.tiles { grid-template-columns:repeat(3, minmax(0,1fr)); } '
            '.pa-builds { display:grid; grid-template-columns:repeat(3, minmax(0,1fr)); gap:10px 24px; list-style:none; padding:0; margin:14px 0 12px; max-width:none; } '
            '.pa-builds li { border-top:3px solid var(--rule); padding:10px 0 4px; font-size:15px; color:var(--ink2); } '
            '.pa-builds li b { display:block; font-family:var(--sans); font-size:20px; letter-spacing:-.02em; color:var(--ink); } '
            '.pa-feat { display:grid; grid-template-columns:repeat(2, minmax(0,1fr)); gap:22px 32px; margin:24px 0 20px; } '
            '.pa-feat > div { border-top:1px solid var(--rule); padding:10px 0 4px; } '
            '.pa-feat h4 { margin:0 0 6px; font-size:16px; } .pa-feat ul { margin:0; padding-left:18px; } .pa-feat li { margin:4px 0; font-size:15px; } '
            '.pa-stack li { margin:10px 0; } '
            '.pa-ph1 { --ph:var(--blue); --phs:var(--blue-soft); --phi:var(--blue-ink); } .pa-ph2 { --ph:var(--accent); --phs:var(--accent-soft); --phi:var(--accent-ink); } .pa-ph3 { --ph:var(--gold); --phs:var(--gold-soft); --phi:var(--gold-ink); } '
            '.pa-d { display:inline-block; white-space:nowrap; background:var(--phs); border-left:3px solid var(--ph); border-radius:0; padding:3px 8px; font-family:var(--sans); font-size:12.5px; font-weight:700; font-variant-numeric:tabular-nums; } '
            '.pa-d abbr { display:block; text-decoration:none; font-size:10px; font-weight:700; letter-spacing:.1em; color:var(--phi); text-transform:uppercase; } '
            '.pa-key { display:flex; flex-wrap:wrap; gap:6px 16px; font-size:12.5px; color:var(--ink2); margin:10px 0 4px; } '
            '.pa-key span::before { content:""; display:inline-block; width:12px; height:12px; border-radius:0; background:var(--phs); border-left:3px solid var(--ph); vertical-align:-1px; margin-right:6px; } '
            '@media print { .pa-d, .pa-key span::before { -webkit-print-color-adjust:exact; print-color-adjust:exact; } } '
            '@media (max-width: 640px) { .pa-builds, .pa-feat { grid-template-columns:1fr; } } '
            '@media (max-width: 860px) { .tiles { grid-template-columns:repeat(2, minmax(0,1fr)); } } '
            '.pa-src { font-size:12px; color:var(--ink3); margin:4px 0 0; max-width:none; }</style>')


def build(D):
    b = [PA_STYLE]
    b.append('<p class="eyebrow">01 · Overview</p><h1>F-Zero X on 3DS — Old and New, native, with the 64DD Expansion Kit</h1>')
    b.append('<p class="lede">How G-Diffuser, the PC port of the F-Zero X decompilation, got onto the Nintendo 3DS: one person '
             'directing, Claude agents writing all of the code, 11 weeks from first research to multiplayer.</p>')
    b.append('<ul class="pa-rel">'
             '<li><b>v0.9.0</b> <span>2026-09-04</span> New 3DS and New 2DS XL only: a 59.6 fps hardware median and stereoscopic 3D, after a 24-day sprint.</li>'
             '<li><b>v1.0.0</b> <span>2026-10-19</span> Old 3DS and 2DS support, a native renderer as the default on both consoles, '
             'the 64DD Expansion Kit on New 3DS, and the DASH bottom screen.</li>'
             '<li><b>v1.1.0</b> <span>2026-10-26</span> Multiplayer for 2–4 players over local wireless, Wi-Fi or the internet; '
             'native menus; both 3D eyes in one GPU submission. Release notes in <code>docs/RELEASE-NOTES.md</code>.</li>'
             '</ul>')
    H = F.HW
    b.append('<h2>The numbers</h2>')
    b.append(tiles([
        ("60<small>fps</small>", f"New 3DS, 3D race: all {H['n3ds_race3d_windows']} one-second windows at 59–61", "red"),
        (f"{H['o3ds_race3d_median']}<small>fps</small>", f"Old 3DS, 3D race: median window; {H['o3ds_race3d_at60']}% at 60, tenth percentile {H['o3ds_race3d_p10']}", "red"),
        (esc(F.big(F.tokens(F.TOT))), "tokens across all sessions and agents, 08-11 to 10-26", "blue"),
        (esc(F.money(F.COST_1H)), f"at API list prices ({F.money(F.COST_LOGGED)} billing cache writes as logged)", "gold"),
        (f"{F.PROMPTS}", f"human prompts; {F.SUBAGENTS} subagents and {F.WORKFLOW_AGENTS:,} workflow agents did the work", "blue"),
        (f"{F.LOC_PORT // 1000}k", "lines of port code under port/3ds, tests included", "red"),
    ], hero=True))
    b.append('<p class="pa-src">Hardware: card-6 test logs, build <code>ba0692e</code>, 10-26 (1P vs CPU field, 3D on); how the numbers got there '
             'is on <a href="road-to-60.html">Road to 60</a>. Tokens and cost: session transcripts '
             'to 10-26, method on <a href="cost.html">Cost</a>.</p>')

    b.append('<h2>On hardware</h2>')
    b.append('<div class="media">'
             '<figure><a href="../media/v1-menu.jpg" target="_blank" rel="noopener"><img src="../media/v1-menu.jpg" alt="v1.0 on a New 3DS: Select Mode with the Expansion Kit entries, the G-DIFFUSER idle view on the bottom screen" loading="lazy"></a>'
             '<figcaption>v1.0, New 3DS: the Expansion Kit\'s Select Mode (COURSE EDIT, CREATE MACHINE); the bottom screen is DASH\'s idle view.</figcaption></figure>'
             '<figure><a href="../media/v1-race.jpg" target="_blank" rel="noopener"><img src="../media/v1-race.jpg" alt="v1.0 on a New 3DS: a race on the top screen, the DASH race dashboard on the bottom" loading="lazy"></a>'
             '<figcaption>v1.0, New 3DS, mid-race: DASH on the bottom screen with energy, position, lap times and the course map.</figcaption></figure>'
             '</div>')
    b.append('<div class="media pa-mp">'
             '<figure><a href="../media/mp-race-countdown.png" target="_blank" rel="noopener"><img src="../media/mp-race-countdown.png" alt="v1.1.0 work, emulator: a four-player Mute City race at the countdown, with the DASH showing standings and the NET indicator on the bottom screen" loading="lazy"></a>'
             '<figcaption>v1.1.0 work, multiplayer, emulator capture (not a photo of hardware): a four-player race on Mute City at the countdown. Each console shows its own racer; the DASH shows the standings and the NET indicator.</figcaption></figure>'
             '<figure><a href="../media/mp-menu.png" target="_blank" rel="noopener"><img src="../media/mp-menu.png" alt="v1.1.0 work, emulator: Select Mode with VS Battle chosen and the bottom-screen MULTIPLAYER menu offering Play Nearby and Play Online" loading="lazy"></a>'
             '<figcaption>v1.1.0 work, emulator capture: picking VS opens the MULTIPLAYER menu on the bottom screen, nearby (local wireless or same Wi-Fi) or online with a room code.</figcaption></figure>'
             '</div>')
    b.append('<p>The clips and photos below are from v0.9 (2026-09), before DASH replaced the debug console on the bottom screen.</p>')
    b.append('<div class="media">'
             '<figure class="vid"><video autoplay muted loop playsinline preload="metadata" poster="../media/gameplay-race-poster.jpg" src="../media/gameplay-race.mp4"></video><a class="open" href="../media/gameplay-race.mp4" target="_blank" rel="noopener">open clip</a>'
             '<figcaption>v0.9: Grand Prix on a New 3DS. The bottom screen is the port\'s debug menu: 59.8 fps, heap, build id.</figcaption></figure>'
             '<figure class="vid"><video autoplay muted loop playsinline preload="metadata" poster="../media/gameplay-handheld-poster.jpg" src="../media/gameplay-handheld.mp4"></video><a class="open" href="../media/gameplay-handheld.mp4" target="_blank" rel="noopener">open clip</a>'
             '<figcaption>v0.9: handheld, mid-race. Native 60 Hz pacing; no interpolation.</figcaption></figure>'
             '</div>')
    b.append('<div class="media four">'
             '<figure><a href="../media/title.jpg" target="_blank" rel="noopener"><img src="../media/title.jpg" alt="F-Zero X title screen on the New 3DS" loading="lazy"></a><figcaption>Title screen, launched from the Homebrew Launcher (.3dsx).</figcaption></figure>'
             '<figure><a href="../media/menu.jpg" target="_blank" rel="noopener"><img src="../media/menu.jpg" alt="Select Mode screen" loading="lazy"></a><figcaption>Select Mode.</figcaption></figure>'
             '<figure><a href="../media/character1.jpg" target="_blank" rel="noopener"><img src="../media/character1.jpg" alt="Select Machine screen" loading="lazy"></a><figcaption>Select Machine.</figcaption></figure>'
             '<figure><a href="../media/character2.jpg" target="_blank" rel="noopener"><img src="../media/character2.jpg" alt="Blue Falcon stats" loading="lazy"></a><figcaption>Blue Falcon.</figcaption></figure>'
             '</div>')
    b.append('<h2>What was built</h2>')
    b.append('<p>Three console builds from one source tree:</p>')
    b.append('<ul class="pa-builds">'
             '<li><b>Old 3DS / 2DS</b>F-Zero X</li>'
             '<li><b>New 3DS / New 2DS XL</b>F-Zero X</li>'
             '<li><b>New 3DS + Expansion Kit</b>F-Zero X with the 64DD add-on</li>'
             '</ul>')
    b.append('<p>Each installs from the HOME menu (.cia) or the Homebrew Launcher (.3dsx). No ROM or disk image ships: players '
             'extract their own.</p>')
    b.append('<div class="pa-feat">'
             '<div><h4>The whole game</h4><ul>'
             '<li>Grand Prix, Time Attack, Practice, Death Race, VS and the machine screens.</li>'
             '<li>With the Expansion Kit: Course Edit and Create Machine.</li></ul></div>'
             '<div><h4>Speed</h4><ul>'
             '<li>New 3DS: 60 fps races in 2D and 3D.</li>'
             '<li>Old 3DS: 2D races at or near 60; 3D races mostly hold 60.</li>'
             '<li>The heaviest Old 3DS courses can still drop to 30 in 3D.</li></ul></div>'
             '<div><h4>The console, used properly</h4><ul>'
             '<li>Stereoscopic 3D on the slider.</li>'
             '<li>Music and effects on the DSP.</li>'
             '<li>A bottom-screen dashboard (DASH).</li>'
             '<li>Clean HOME, sleep and power-off.</li></ul></div>'
             '<div><h4>Multiplayer <small>(v1.1.0 work)</small></h4><ul>'
             '<li>2–4 players, each on their own console.</li>'
             '<li>Nearby or online.</li></ul></div>'
             '</div>')
    b.append('<p>Both consoles draw through the same native renderer by default. The New 3DS\'s first-phase render-thread path is '
             'still built in as an option; <a href="frontier.html">Frontier</a> and <a href="road-to-60.html">Road to 60</a> cover it.</p>')
    b.append('<h3>The stack</h3><ul class="pa-stack">'
             '<li><b>decomp</b>: the F-Zero X decompilation, unmodified upstream, with a small set of patches on top.</li>'
             '<li><b>libultraship</b>: the N64 runtime that PC ports of decompiled games are built on. The port carries a trimmed copy '
             'with its 3DS changes folded in. It loads the game\'s assets; its original renderer remains as a fallback.</li>'
             '<li><b>port/3ds</b>: the port itself, ' + f"{F.LOC_PORT // 1000}k" + ' lines: rendering, stereo, audio, input, menus, '
             'lifecycle, multiplayer and packaging.</li>'
             '<li><b>the native renderer</b>: N64 PC ports normally translate the game\'s drawing commands into GPU calls one at a '
             'time, every frame. This port instead writes the 3DS GPU\'s own commands directly, from data prepared ahead of time, '
             'for races, menus and most transitions: nearly everything on screen. Only a few rare screens, such as the course editor '
             'and the ending, still take the old route. No other port in this family does this, and it is what lets the Old 3DS keep '
             'up. See <a href="native.html">The road to 60, all over again</a>.</li>'
             '</ul>')
    b.append('<h2>How it went</h2>')
    b.append('<p>The first phase (08-11 → 09-03) ported the game to the New 3DS. The second (09-07 → 10-19, not originally planned) '
             'brought it to the Old 3DS through a native renderer and shipped v1.0.0. Then a week of v1.1.0 work.</p>')
    b.append('<div class="pa-key" aria-label="Date colour key"><span class="pa-ph1">v0.9 = New 3DS only (first phase)</span>'
             '<span class="pa-ph2">v1.0 = Old 3DS (second phase, the native fork)</span><span class="pa-ph3">v1.1 = polish and online multiplayer</span></div>')
    b.append(table(["Dates", "Stage", "What happened"], [
        ['<span class="pa-d pa-ph1">08-11 → 08-14<abbr title="New 3DS only (first phase)">v0.9</abbr></span>', "<b>From research to a race</b>", "Feasibility dossier, a plan for a team of agents, five parallel foundation streams, six boot bugs; first race at 12–20 fps in the emulator."],
        ['<span class="pa-d pa-ph1">08-20 → 08-22<abbr title="New 3DS only (first phase)">v0.9</abbr></span>', "<b>Playable, then on a console</b>", "Fog, HUD, livery, shadow and sky fixed; menus from 27 to 60 fps; first hardware run 08-21 (\"wow it runs amazing\"), audio the same evening."],
        ['<span class="pa-d pa-ph1">08-27 → 08-28<abbr title="New 3DS only (first phase)">v0.9</abbr></span>', "<b>Product grade</b>", "Stereo 3D armed, touch menu, rival detail; a GPU vertex-transform moonshot built and shelved; README and release package."],
        ['<span class="pa-d pa-ph1">09-01 → 09-02<abbr title="New 3DS only (first phase)">v0.9</abbr></span>', "<b>Lifecycle and the last big lever</b>", "HOME crash and close-from-HOME hang fixed; per-frame display-list re-scanning replaced by memoized results; the LOCKED-60 campaign launched."],
        ['<span class="pa-d pa-ph1">09-02 → 09-03<abbr title="New 3DS only (first phase)">v0.9</abbr></span>', "<b>Multi-core</b>", "Render thread on core 2, one frame ahead; median 59.6 fps with 53% of frames at the cap. v0.9.0."],
        ['<span class="pa-d pa-ph2">09-07 → 09-29<abbr title="Old 3DS (second phase, the native fork)">v1.0</abbr></span>', "<b>Make it fit</b>", "Old 3DS restarted; build squeezed into its 64 MB app region. First boot on a real Old 3DS: about 10 fps."],
        ['<span class="pa-d pa-ph2">09-29 → 09-30<abbr title="Old 3DS (second phase, the native fork)">v1.0</abbr></span>', "<b>Find the real costs</b>", "Narrower audio lock (−20 ms a frame); frame skipping ends the slow motion; a retained renderer wins in the emulator only."],
        ['<span class="pa-d pa-ph2">09-30 → 10-01<abbr title="Old 3DS (second phase, the native fork)">v1.0</abbr></span>', "<b>The probe</b>", "Hardware counters: the limit is instruction-cache misses, not instruction count. A working pose-replay fallback is shelved: \"we should go for a real 60!\""],
        ['<span class="pa-d pa-ph2">10-01 → 10-02<abbr title="Old 3DS (second phase, the native fork)">v1.0</abbr></span>', "<b>Native renderer</b>", "Small race loops sized to the 16 KB instruction cache. 2D Grand Prix at a 16.5 ms median frame."],
        ['<span class="pa-d pa-ph2">10-03 → 10-08<abbr title="Old 3DS (second phase, the native fork)">v1.0</abbr></span>', "<b>3D at 60</b>", "Per-eye resubmit: 59–60 fps in 3D on most courses; the heaviest can fall to 30."],
        ['<span class="pa-d pa-ph2">10-08 → 10-19<abbr title="Old 3DS (second phase, the native fork)">v1.0</abbr></span>', "<b>Release</b>", "Expansion Kit verified tick-for-tick, released for New 3DS only (too slow on Old); DASH ships; adversarial review; v1.0.0."],
        ['<span class="pa-d pa-ph3">10-20 → 10-26<abbr title="Polish and online multiplayer">v1.1</abbr></span>', "<b>After v1.0.0</b>", "2–4 player multiplayer; one GPU submission for both eyes (Old 3DS 3D menu 40.2 → 44.6 fps); HOME-close lock fixed."],
    ]))
    b.append('<p>See <a href="agentic.html">Agentic workflow</a> for how the second phase was run and '
             '<a href="native.html#old-3ds">The road to 60, all over again</a> for the Old 3DS campaign, card by card.</p>')

    b.append('<h2>Read on</h2>')
    b.append('<ul>')
    for f, t, s in PAGES[1:]:
        b.append(f'<li><a href="{f}"><b>{esc(t)}</b></a>: {esc(s)}.</li>')
    b.append('</ul>')
    b.append('<div class="note">Every number on these pages has a source: git history, the research documents under '
             '<code>docs/research/</code>, the hardware logs, or the session transcripts the <a href="cost.html">Cost</a> page counts. '
             'Where a value is an estimate (the dollar figures, which apply published API list prices to exact token counts), the '
             'assumption is stated next to it.</div>')
    return page("index.html", "G-Diffuser on 3DS", "".join(b))
