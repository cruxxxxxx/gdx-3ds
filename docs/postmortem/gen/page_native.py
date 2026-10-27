from framework import *
import pc_native
import pd_o3ds
from pc_diagrams import STYLE as PC_STYLE

# Part dividers. Not class="eyebrow" alone, so framework.number_eyebrow() leaves them alone.
PART_CSS = ('<style>.pd-part { font-family:var(--sans); font-weight:700; font-size:11.5px; letter-spacing:.14em; text-transform:uppercase; '
            'color:var(--accent); border-top:3px solid var(--rule); padding-top:14px; margin:64px 0 -26px; max-width:none; } '
            '.pd-mp img { max-height:420px; object-fit:contain; }</style>')

# The multiplayer gallery: (file in docs/media/, alt text, caption). Both screens, top above bottom.
MP_SHOTS = [
    ("mp-menu.png", "The MULTIPLAYER menu on the bottom screen: PLAY NEARBY and PLAY ONLINE; Select Mode with VS on the top screen",
     "VS opens MULTIPLAYER on the bottom screen."),
    ("mp-join-code.png", "The ROOM CODE keypad on the bottom screen with four of five letters typed",
     "Joining an online room with the host's 5-letter code."),
    ("mp-machine-select.png", "Select Machine on the top screen; the room panel on the bottom screen with every player picking and a round timer",
     "Machine select, with every player's pick on the bottom screen."),
    ("mp-race-countdown.png", "Race countdown with two other players' machines alongside; the DASH lists all four players' standings",
     "The countdown: each console shows its own racer."),
]


def part(label):
    return f'<p class="pd-part">{label}</p>'


def renderer():
    b = [part("Part 1 · The renderer")]
    b.append('<h2>The convention</h2>')
    b.append('<p>Every N64 PC port in this family renders the same way.</p>')
    b.append(pc_native.convention())
    b.append('<h2>Where we were — v0.9.0</h2>')
    b.append('<p>v0.9.0 ran that convention unchanged. On a 3DS it is CPU-bound.</p>')
    b.append(pc_native.where_we_were())
    b.append('<h2>Where we ended up — v1.1.0</h2>')
    b.append('<p>The game still decides <i>what</i> to draw; the native renderer replaces <i>how</i> it reaches the GPU, one '
             'category at a time.</p>')
    b.append(pc_native.where_we_ended_up())

    b.append('<h2>Why fork the renderer</h2>')
    b.append('<p>On 09-07 the 3DS build compiled 36 libultraship files, 15,043 lines, dominated by the Fast3D interpreter (10,466 lines), '
             'plus a 12,353-line bridge that only widens the game\'s 32-bit display-list words to 64 bits. Everything else (window, audio, '
             'input, config) was already the port\'s own code. A census found machines, track and effects each drawn differently, so the '
             'fork is a <b>hybrid</b>: our own interpreter for everything, plus race-only paths for what a race frame spends its time on. '
             'The long tail (editors, ending, one-off screens) stays interpreted.</p>')
    b.append(table(["Tier", "What it does", "At release"], [
        ["1 · Own the interpreter", "A purpose-built F3DEX2 → citro3d walker reads the game's display list directly. No bridge.",
         "Default on both consoles; <code>[perf] preset = off</code> restores the libultraship path."],
        ["2 · Bypass it for races", "Track, machines, sky, HUD, effects and shadows drawn from baked or recorded data: the native race renderer (NRR).",
         "Default on both consoles; since v1.1.0 it also covers menus, course select and machine select."],
    ]))
    b.append('<p>Every lever is an ini killswitch with a verify mode that draws both paths and compares the vertex bytes bit for bit. '
             '<code>[perf] preset = auto</code> resolves per console at startup (<code>n60</code> on New 3DS, <code>o3ds-auto</code> on '
             'Old 3DS) and turns the whole set on; an explicit ini key always wins.</p>')
    b.append('<p>The renderer work waited two weeks for the dev loop: a network listener that accepts a build, writes it to SD and '
             'relaunches through the Homebrew Launcher, and a live log link over 3dslink\'s protocol. One Y-press per power-on; after '
             'that the console reloaded builds on its own.</p>')

    b.append('<h2>Case by case</h2>')
    b.append('<p>Each part of a frame at v1.1.0 defaults. The last column is Old 3DS CPU per rendered 2D race frame: the 10-01 '
             'interpreter build against native-renderer card 3 the next day.</p>')
    b.append(pc_native.case_table())
    return "".join(b)


def shipping():
    b = [part("Part 3 · What shipped")]
    b.append('<h2>The Expansion Kit</h2>')
    b.append('<p>The 64DD Expansion Kit (course editor, machine creator, disk content) is its own build, "G-Diffuser EK", with its own '
             'HOME-menu title, save and log, so it installs beside the plain game. It needs the player\'s own retail Japanese 64DD disk '
             'image, streamed from SD rather than held in RAM (62 MB saved); without one, the EK entries simply don\'t open. A fan-translated '
             'English image froze at boot in testing and is a known issue.</p>')
    b.append('<p>Under the Old 3DS profile in the emulator it fits with ~6 MB spare and matches the plain game tick for tick (6,002/6,002 on a '
             'ROM course, 8,601/8,601 on a disk course). On Old 3DS hardware disk courses stuttered, disk music broke up and the editors '
             'crawled, so it ships for the New 3DS only.</p>')

    b.append('<h2>The bottom screen: DASH</h2>')
    b.append('<p>The second screen started as a debug console and became a race dashboard: lap, position, energy, splits and the map, '
             'painted by the CPU straight into the bottom framebuffer, no GPU. Three layouts (all top, split, all bottom) cycle from a '
             'touch chip or a SELECT long-press. On Old 3DS hardware it cost 4.2–5.6 ms a frame, so it was put on a 1 ms budget. In 2–4 '
             'player races the HUD stays on the top screen. A drag-and-resize layout editor was shelved; a one-frame speed spike (up to '
             '~826 KPH) at the finish line is known and cosmetic.</p>')

    b.append('<h2>The New 3DS freeze</h2>')
    b.append('<p>A real New 3DS froze, with no dump and no log line, every time the attract demo reached its full 30-machine race; the '
             'emulator never reproduced it. Bisection on the console found the GPU command buffer sized at the edge of that frame: with '
             'one lever off, the same frame hit a bounds-checked panic in the buffer writer instead. The buffer went from 2× to 4× the '
             'citro3d default on both consoles, with a log line before any future overflow. Four hardware boots through that frame came back '
             'clean.</p>')

    b.append('<h2>Multiplayer</h2>')
    b.append('<p>Most of v1.1.0: 2–4 players over local wireless, the same Wi-Fi, or online with a room code, each console showing its own '
             'racer, CPU machines filling the field. Old and New 3DS can share a room. On the Old 3DS, multiplayer races run in 2D at 60 '
             'by default; 3D is locked to 30. Player guide: <a href="../MULTIPLAYER.md">MULTIPLAYER.md</a>.</p>')
    figs = "".join(f'<figure><a href="../media/{f}" target="_blank" rel="noopener"><img src="../media/{f}" alt="{esc(a)}" '
                   f'loading="lazy"></a><figcaption>{esc(c)}</figcaption></figure>' for f, a, c in MP_SHOTS)
    b.append(f'<div class="media four pd-mp">{figs}</div>')
    b.append('<p class="note">Emulator captures (Azahar), not hardware: four New 3DS instances in an online room through a local '
             'rendezvous server.</p>')

    b.append('<h2>Release: v1.0.0, then v1.1.0</h2>')
    b.append('<p>v1.0.0 (10-19) is three downloads from one tree: Old 3DS, New 3DS, and New 3DS with the Expansion Kit. All share one SD '
             'folder and pick their preset at startup. v1.1.0 (10-26) keeps the same three downloads and adds multiplayer, native '
             'menus and one-submission 3D. See <a href="../RELEASE-NOTES.md#known-issues">RELEASE-NOTES.md</a> and '
             '<a href="../STATUS.md">STATUS.md</a>.</p>')
    b.append(table(["Known issue (v1.0.0, still open)", "What it means"], [
        ["3D is not a locked 60 on either console", "Old 3DS 3D is CPU-bound: 59–60 on most courses, some step to 30; multiplayer 3D is 30"],
        ["The Old 3DS memory ceiling", "A large mid-race asset load can exhaust the budget in a worst-case emulator run; hardware peaks near 24.2 MB"],
        ["Brief audio gaps during loads", "Short and occasional"],
        ["A speed-readout spike at the finish", "Cosmetic, DASH only"],
        ["Not every mode tested on every model", "Coverage favours the modes used most in development"],
    ]))
    b.append('<p>The day-by-day account is on <a href="timeline.html">Every step</a>; the hardest fights on '
             '<a href="difficulties.html">Difficulties</a>.</p>')
    return "".join(b)


def build(D):
    b = [PC_STYLE, PART_CSS]
    # The leading "05 ·" below is cosmetic placeholder text: framework.page() renumbers it from PAGES.
    b.append('<p class="eyebrow">05 · Road to 60, again</p><h1>The road to 60, all over again</h1>')
    b.append('<p class="lede">On the Old 3DS, the original interpreter could not keep up. So from 09-07 we replaced it, piece by '
             'piece, with code that writes PICA200 commands directly. This page covers that native renderer, the Old 3DS road to 60, '
             'and what shipped with it.</p>')
    b.append(tiles([
        ("~1,600", "commits on the native integration branch, 09-07 → 10-19", True),
        ("187", "feature branches, almost all merged", False),
        ("2", "consoles from one codebase", True),
        ("0<small> / 10,466</small>", "libultraship interpreter lines that run during a race at default settings", True),
    ]))
    b.append(renderer())
    b.append(part("Part 2 · The Old 3DS"))
    b.append(pd_o3ds.section(D))
    b.append(shipping())
    return page("native.html", "The Road to 60, All Over Again", "".join(b))
