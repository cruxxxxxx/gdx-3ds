from framework import *


def build(D):
    b = []
    b.append('<p class="eyebrow">01 · Overview</p><h1>F-Zero X on a New 3DS, at 60, in 3D, in 24 days</h1>')
    b.append('<p class="lede">A post-mortem of porting G-Diffuser, the F-Zero X decompilation on the libultraship runtime, '
             'to the New Nintendo 3DS. One person directed; one long-lived Claude session orchestrated; '
             + esc(D["subagents"]) + ' subagents built. The console went from a black screen to a native-60 stereoscopic racer '
             'between 2026-08-11 and 2026-09-03.</p>')
    b.append(tiles([
        (esc(D["final_median"]) + '<small>fps</small>', "median on hardware, full Grand Prix, final round (from 12–20 fps in the first emulator race)", True),
        (esc(D["final_p10"]) + '<small>fps</small>', "tenth-percentile floor in 30-machine crowds (from 25–35)", True),
        (esc(D["final_cap"]), "of frames at the 60 cap (from 14% one week earlier)", True),
        (esc(D["active_days"]), "active days across 24 calendar days, three credit pauses", False),
        (esc(D["tokens_total_h"]), "tokens across all sessions and agents", False),
        (esc(D["cost_a_h"]), "at published API list prices (Fable $10/$50 per MTok); see Cost", False),
        (esc(D["commits_3ds"]), "commits touching the 3DS port", False),
        (esc(D["loc_port3ds"]), "lines under port/3ds plus " + D["patches"] + " pure-delta patches", False),
    ]))

    b.append('<h2>On hardware</h2>')
    b.append('<div class="media">'
             '<figure class="vid"><video autoplay muted loop playsinline preload="metadata" poster="../media/gameplay-race-poster.jpg" src="../media/gameplay-race.mp4"></video>'
             '<figcaption>Grand Prix on a New 3DS, final build. The bottom screen is the port\'s debug menu: 59.8 fps, heap, build id.</figcaption></figure>'
             '<figure class="vid"><video autoplay muted loop playsinline preload="metadata" poster="../media/gameplay-handheld-poster.jpg" src="../media/gameplay-handheld.mp4"></video>'
             '<figcaption>Handheld, mid-race. Native 60 Hz pacing; no interpolation.</figcaption></figure>'
             '</div>')
    b.append('<div class="media four">'
             '<figure><img src="../media/title.jpg" alt="F-Zero X title screen on the New 3DS" loading="lazy"><figcaption>Title screen, .cia launched from HOME.</figcaption></figure>'
             '<figure><img src="../media/menu.jpg" alt="Select Mode screen" loading="lazy"><figcaption>Select Mode.</figcaption></figure>'
             '<figure><img src="../media/character1.jpg" alt="Select Machine screen" loading="lazy"><figcaption>Select Machine.</figcaption></figure>'
             '<figure><img src="../media/character2.jpg" alt="Blue Falcon stats" loading="lazy"><figcaption>Blue Falcon.</figcaption></figure>'
             '</div>')
    b.append('<h2>What was built</h2>')
    b.append('<p>A New 3DS build of G-Diffuser that boots from the HOME menu (.cia) or the homebrew launcher (.3dsx), '
             'reads prebaked asset archives from the SD card (no ROM ever touches the console), and plays the whole game: '
             'Grand Prix, Time Trial, Practice, Death Race, the machine editor screens, with music and effects on the real DSP, '
             'stereoscopic 3D on the slider, a touch menu on the bottom screen with live performance levers and a log viewer, '
             'and clean HOME suspend, resume, close, lid sleep and power-off. Frame pacing is native 60 Hz, not 30 Hz with interpolation.</p>')
    b.append('<div class="cols2">')
    b.append('<div><h3>The stack</h3><ul>'
             '<li><b>decomp</b>: the F-Zero X decompilation (inspectredc/fzerox), untouched, plus ' + esc(D["patches_decomp"]) + ' patches.</li>'
             '<li><b>libultraship</b>: the N64 runtime (Kenix3), untouched, plus ' + esc(D["patches_lus"]) + ' patches, including a 13-line newlib fix that lets it build on devkitARM.</li>'
             '<li><b>port/3ds</b>: ' + esc(D["loc_port3ds"]) + ' lines of new code: a citro3d rendering backend, stereo, audio, input, menu, lifecycle, render thread, profilers, packaging.</li>'
             '</ul></div>')
    b.append('<div><h3>The headline numbers on hardware</h3>' + table(["When", "Median", "p10", "At cap"], D["headline_rows"], num_cols=(1, 2, 3)) + '</div>')
    b.append('</div>')

    b.append('<h2>The arc in five lines</h2>')
    b.append('<ol>')
    b.append('<li><b>08-11 → 08-14: from research to a race.</b> Feasibility dossier, plan for a team of agents, five parallel foundation streams, six boot bugs, first race at 12–20 fps in the emulator.</li>')
    b.append('<li><b>08-20 → 08-22: from pink to playable, then to a real console.</b> Fog, HUD, livery, shadow, sky fixed; the loadblock discovery took menus from 27 to 60 fps; first hardware run 08-21 (\"wow it runs amazing\"); audio the same evening.</li>')
    b.append('<li><b>08-27 → 08-28: product grade.</b> Five-agent fleet drop, stereo armed, touch menu, rival detail, GPU vertex-transform moonshot built and shelved, README with legal notice, release package.</li>')
    b.append('<li><b>09-01 → 09-02: lifecycle and the last big lever.</b> HOME crash and close-from-HOME hang fixed; bridge output cache proven a NO-GO and pivoted into brfast; LOCKED-60 campaign launched with four parallel levers.</li>')
    b.append('<li><b>09-02 → 09-03: multi-core.</b> Render thread on core 2, one-frame-ahead mode, bridge on main, auto LOD; median 59.6 with 53% of frames at cap; crash fixed; viewport placement fixed; stereo markers anchored.</li>')
    b.append('</ol>')

    b.append('<h2>Read on</h2>')
    b.append('<ul>')
    for f, t, s in PAGES[1:]:
        b.append(f'<li><a href="{f}"><b>{esc(t)}</b></a>: {esc(s)}.</li>')
    b.append('</ul>')
    b.append('<div class="note">Every number on these pages has a source: git history, the research documents under '
             '<code>docs/research/</code>, the hardware logs, or the session transcripts on this machine. Where a value is an estimate '
             '(the dollar figures, which apply published API list prices to exact token counts), the assumption is stated next to it.</div>')
    return page("index.html", "G-Diffuser on New 3DS", "".join(b))
