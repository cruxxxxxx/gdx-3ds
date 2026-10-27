"""The frontier page's list of firsts and differences, each with a three-step journey line.

Every "first" is qualified by the research that backs it; anything without a literature check is
labelled "did differently". Sources: page_boundaries.py field table (10-08 search, "still current at
release"), docs/STATUS.md, docs/RELEASE-NOTES.md (v1.1.0), port/3ds/gfx/STEREO.md,
docs/research/native/onesubmit-progress.md, port/3ds/gdx3ds_preset_n60.c, docs/MULTIPLAYER.md,
port/3ds/vendor/lus/PROVENANCE.md, port/3ds/patches/.
"""
from framework import esc
from pc_diagrams import steps

FIRST = '<span class="pill acc">first, as far as our research found</span>'
DIFF = '<span class="pill neu">did differently</span>'

PILL_CSS = "<style>.pc-item { margin:26px 0 30px; } .pc-item h3 { margin:0 0 4px; } .pc-item .pill { margin:0 0 6px; } .pc-item p { margin:6px 0; } @media print { .pc-item { break-inside:avoid; } }</style>"


def item(n, title, pill, body, journey):
    return (f'<div class="pc-item"><h3>{n}. {title}</h3>{pill}<p>{body}</p>' + steps(journey) + '</div>')


def build_items(D):
    out = [PILL_CSS]
    out.append(item(1, "libultraship on a 3DS", FIRST,
        "The whole runtime, its Fast3D interpreter, resource manager and archive loader, compiled for the ARM11 under devkitARM. "
        "When this started, the research passes (08-14, 09-01) found libultraship only on desktops. By the 10-08 field search two other "
        "libultraship-based 3DS projects had appeared (a SpaghettiKart-based Mario Kart 64 and a Ship of Harkinian port); their start "
        "dates are not in our record, so the claim is dated: no libultraship 3DS build was known on 08-12.",
        [("08-12", "A carve-out spike builds libultraship under devkitARM with a 13-line newlib patch."),
         ("First phase", f"Kept as pure-delta patches over two untouched submodules ({esc(D['patches'])} files)."),
         ("Now", "82 decomp patches; the 41 libultraship patches are folded into a vendored carve that is the source of truth, and its "
                 "interpreter runs only under <code>preset = off</code>.")]))
    out.append(item(2, "Native 60 on a software display-list interpreter", DIFF,
        "The first phase's position on the New 3DS, full Grand Prix, trace off: median " + esc(D["final_median"]) + " fps, tenth percentile "
        + esc(D["final_p10"]) + ", " + esc(D["final_cap"]) + " of frames at the cap, without dropping the logic rate or interpolating. "
        "When it landed, the 09-01 research had found no N64 port on 3DS holding 60 this way. The 10-08 field search found others "
        "reaching 60 by interpolating over 30 Hz logic, and Smash 64 for New 3DS aiming at 60 without promising it, so this is "
        "listed as a difference, not a first.",
        [("08-14", "12–20 fps in the emulator; 08-21, 48–50 on hardware."),
         ("09-03", "With the render thread on core 2, the last hardware rounds reach a 59.6 median."),
         ("Now", "History: the shipped default is the native renderer below. This path stays reachable with <code>preset = off</code>.")]))
    out.append(item(3, "Exact stereoscopy, then both eyes in one GPU submission", DIFF,
        "Stereo is an asymmetric-frustum shift folded exactly into the projection, <code>x′ = x ± k·(z − dc·w)</code>, so depth, fog "
        "and the combiner are bit-identical per eye and the 2D race markers can sit at their ship's depth. The second eye has been "
        "produced three ways. Other 3DS N64 ports also replay per eye (rt64-3ds, the SM64 lineage); what is different here is "
        "how little the right eye costs, and that the last step came from a suggestion by a 3DS homebrew developer.",
        [("08-27", "Armed on hardware: each draw re-issued for the right eye from the same packed vertices. \"3D works great.\""),
         ("Native fork", "Frame replay: the body is emitted once; the right eye resubmits the same command lists with one uniform changed."),
         ("v1.1.0", "One submission: the GPU runs the body, jumps to a right-eye prologue, runs it again. Old 3DS 3D menu 40.2 → 44.6 fps. "
                    "Still not a locked 60 in 3D on either console (item 7).")]))
    out.append(item(4, "GPU vertex transform: shelved, then shipped", DIFF,
        "The 08-28 moonshot moved the unlit vertex transform onto the PICA vertex shader: 220,000 draws with zero fallbacks, 62% less "
        "vertex time in the emulator, imperceptible on hardware, and broken on the venue floor because 24-bit floats cannot hold "
        "±32000-unit coordinates. It was shelved on that evidence. The native fork brought it back for the meshes it retains, with the "
        "precision problem solved: the shader adds an integer eye offset first, which float24 represents exactly.",
        [("08-28", "Built, measured, shelved: too small a slice, and a precision bug."),
         ("Native fork", "Revived as <code>gpuxf</code> for retained track and machine meshes, with lighting and texgen."),
         ("Now", "On by default on both consoles.")]))
    out.append(item(5, "A render thread inside the game's own frame protocol", DIFF,
        "Rather than a mailbox, the fork happens where the N64 would have started the RSP task (<code>osSpTaskStartGo</code>) and the join "
        "is the game's own DP-done wait: a true one-frame-ahead pipeline with two-deep backpressure. The 09-02 research found no precedent; "
        "the 10-08 field search found Smash 64 for New 3DS also rendering on another core, so this is a different design, not the only "
        "multi-core renderer.",
        [("09-02", "Brief and first build; core 2 reached through a Luma kernel grant."),
         ("09-03", "Hardware round 4: median 59.6 fps."),
         ("Now", "Off at release on both consoles: the native renderer needs a sequential frame, and core 2 runs audio instead.")]))
    out.append(item(6, "Fixed-function combiner totality", DIFF,
        "Every N64 colour-combiner mode the game uses maps to three or fewer PICA TexEnv stages; fog is a per-vertex factor blended in a "
        "TexEnv stage because the PICA's depth LUT cannot resolve the game's depth range. Zero unsupported modes.",
        [("First phase", "A census of every mode; worst case three stages."),
         ("Fixes", "Decal clamp, environment-colour flush and CCMUX-11 made the translation total."),
         ("Now", "Our own walker reuses the same combiner keys verbatim, and the native race renderer blends fog in the same kind of stage.")]))
    out.append(item(7, "Skipping the display-list walk, on the hardest target", FIRST,
        "For what a race spends its frame on, the native renderer writes the GPU's own command words from recorded and baked data "
        "instead of walking the display list (see <a href=\"native.html\">The road to 60, all over again</a>). It shipped first on, and matters most "
        "for, the Old 3DS: a third of the clock, no L2 cache, no second application core. No other N64 decompilation, recompilation or emulator "
        "project the 10-08 search found claims a sustained 60 on an Old 3DS, and that was still the case at release.",
        [("09-07", "The fork begins; the Old 3DS, ruled out on 08-11, is revisited from 09-24."),
         ("10-08", "An internal card holds 59–60 fps in Grand Prix races on Old 3DS hardware."),
         ("Now", "Shipped binary, Old 3DS, full 30-racer Grand Prix in 2D: 57.3 fps mean (log stalls excluded). Since v1.1.0, menus too. "
                 "3D: 59.7–59.8 in a short one-submission sample, but some courses step down to 30; the CPU is the limit.")]))
    out.append(item(8, "Online lockstep multiplayer across Old and New 3DS", DIFF,
        "Two to four consoles race over the internet, the same Wi-Fi or local wireless, sending only inputs: about 130 bytes per tick per "
        "peer, every console simulating the same race in deterministic lockstep. A small rendezvous server (one standard-library Python "
        "file, UDP 27200) exchanges endpoints from a five-character room code, punches through NATs and relays when it cannot; agents "
        "deployed and operated it on AWS. Old and New 3DS can share a room. No precedent search was done, so this is not claimed as a first.",
        [("v1.0.0", "Split-screen 2–4 players on one console."),
         ("10-21", "Lockstep spike, then online, local-wireless and full-screen-per-console play merged the same day."),
         ("v1.1.0", "Frame-advantage pacing, drop-outs handed to a CPU driver, desync reports. Old 3DS: 2D aims for 60, 3D locked to 30.")]))
    out.append(item(9, "The 64DD Expansion Kit on a handheld", DIFF,
        "The course editor, machine creator and 64DD content run as their own New 3DS build, streaming the player's own retail Japanese "
        "disk image from the SD card instead of holding 62 MB in RAM. We did not search for earlier 64DD efforts on a handheld, so no "
        "first is claimed.",
        [("Native fork", "Expansion Kit builds fit both consoles' memory; the editor's black screen fixed."),
         ("v1.0.0", "Released for New 3DS only: on Old 3DS the editors crawl and disk music breaks up."),
         ("Now", "Mostly emulator-verified; a 10-26 New 3DS hardware card ran an Expansion Kit build and held 60 in a short 3D race log.")]))
    return "".join(out)
