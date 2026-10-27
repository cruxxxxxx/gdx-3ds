from framework import *


def link(url, text):
    return f'<a href="{esc(url)}">{text}</a>'


def build(D):
    b = []
    b.append('<p class="eyebrow">06 · Pushing the boundaries</p><h1>Pushing the boundaries</h1>')
    b.append('<p class="lede">How this port compares with other N64 projects on the 3DS, from their own READMEs and press, each claim '
             'linked. Searched on 2026-10-08, still current at release. None of their figures were measured by us, and the games '
             'differ in load, so this is a comparison of claims, not a benchmark.</p>')
    R0 = [["<b>G-Diffuser 3DS (this project)</b>", '<a href="native.html">The road to 60, all over again</a>',
           "F-Zero X decomp on libultraship; own F3DEX2 walker plus a native race renderer writing PICA200 command words",
           "Old 3DS: 2D races at 60 (median frame 16.5 ms); 3D 59–60 on most courses, CPU-bound, some step to 30. New 3DS: 3D races at 60 "
           "in every window. Native 60 Hz logic, no interpolation (our hardware logs)",
           "Yes"]]
    R = [
        ["Super Mario 64 3DS port (mkst)", link("https://github.com/mkst/sm64-port/tree/3ds-port", "mkst/sm64-port, branch 3ds-port"),
         "Decomp PC port; renderer details not in the README",
         "README: \"Naïve frame-skip if frame takes longer than 33.3ms\"; a 60 FPS enhancement patch is listed. Press (2020): \"a solid 30fps\", model not stated (" + link("https://www.videogameschronicle.com/news/mario-64-3ds-port/", "VGC") + ")",
         "Yes (README)"],
        ["Ocarina of Time 64 3DS port", link("https://github.com/Andresac90/OOT-64-3DS-Port", "Andresac90/OOT-64-3DS-Port"),
         "zeldaret/oot decomp; renderer started from the SM64 PC port's Fast3D renderer",
         "README: New 3DS \"60 in 2D and 3D\" by interpolation (measured on hardware). Old 3DS: \"about 15–17 in the biggest scene (Kokiri Forest), around 45 in interiors\" (measured at simulated Old 3DS speed); \"60 everywhere is still being worked on\"",
         "Yes"],
        ["Majora's Mask 64 3DS port", link("https://github.com/Andresac90/MM-64-3DS-Port", "Andresac90/MM-64-3DS-Port"), "decomp", "\"Nothing runs on the 3DS yet\"", "–"],
        ["Smash 64 for New 3DS", link("https://github.com/2gifts/smash64-3ds", "2gifts/smash64-3ds"),
         "decomp; credits the SM64 3DS graphics backend, citro3d; renders on the New 3DS's third core",
         "\"Original 3DS/2DS models are not supported\"; \"a locked 60 FPS is not guaranteed in every situation\"", "Yes"],
        ["Mario Kart 64 3DS", link("https://github.com/EstebanPdN/mario-kart-64-3ds", "EstebanPdN/mario-kart-64-3ds"),
         "SpaghettiKart (libultraship) based; Fast3D vertex/texture ranges, combiner plans compiled once per material",
         "\"retains its original 30 Hz game simulation\"; an adaptive interpolated midpoint frame on Old and New 3DS, \"not a promise of a fixed or sustained 60 FPS mode\"",
         "Not mentioned"],
        ["Ship of Harkinian 3DS", link("https://github.com/999sian/soh-3ds", "999sian/soh-3ds"),
         "libultraship (SoH); PICA200 renderer on citro3d, built for F3DEX2",
         "New 3DS is the main target; \"Old 3DS uses native 20 FPS presentation\"; \"Physical Old 3DS gameplay is not yet verified\"", "Not mentioned"],
        ["rt64-3ds", link("https://github.com/oneup03/rt64-3ds", "oneup03/rt64-3ds"),
         "Renderer for N64ModernRuntime static recompilations: F3DEX2 interpreter, CPU vertex transform and lighting, per-eye replay",
         "\"Under construction. First target is Donkey Kong 64 (New 3DS only)\"; no frame rates", "Yes"],
        ["sm64coopdx New 3DS", link("https://github.com/antoinebou12/sm64coopdx-switch/pull/26", "PR #26"),
         "citro3d", "Not yet called playable; \"30 FPS stability\" listed as a next gate", "–"],
        ["DaedalusX64-3DS (emulator)", link("https://www.gamebrew.org/wiki/DaedalusX64_3DS", "GameBrew") + ", " + link("https://wololo.net/2020/05/01/n64-emulation-masterfeizz-releases-armv6-dynarec-version-of-daedalusx64-for-the-3ds-ports-it-to-the-vita-version-mario-kart-64-super-mario-64-are-now-effectively-playable-on-both-the-vi/", "Wololo 2020"),
         "HLE emulator, ARMv6 dynarec",
         "\"Tested on N3DS, runs about at 20-30FPS on average\" (v1.0, 2022-10-16)", "Not verified"],
        ["Ocarina of Time 3D (Nintendo/Grezzo, 2011)", link("https://www.nintendolife.com/news/2011/05/ocarina_of_time_3d_is_a_smoother_experience_than_before", "Nintendo Life"),
         "Remake, native 3DS engine", "Miyamoto: \"It ran at 20fps on the Nintendo64, but on the 3DS, it runs at 30fps\"", "Yes"],
        ["Star Fox 64 3D (2011)", link("https://www.nintendolife.com/reviews/2011/09/star_fox_64_3d_3ds", "Nintendo Life review"),
         "Remake", "Numeric frame rate: not found", "Yes"],
    ]
    b.append('<h2>The field</h2>')
    b.append(table(["Project", "Source", "Approach", "What it says about Old / New 3DS performance", "Stereo 3D"], R0 + R))
    b.append('<h3>What we can and cannot say</h3>')
    b.append('<ul>'
             '<li>We found no other N64 decompilation, recompilation or emulator project on the 3DS that claims a sustained 60 fps on an '
             '<b>Old</b> 3DS. The nearest are the OoT port (15–45 fps by scene, measured at simulated Old 3DS speed), the MK64 port '
             '(adaptive interpolation over 30 Hz logic), SoH-3DS (20 fps presentation, unverified on hardware) and the SM64 port.</li>'
             '<li>Every renderer we found interprets N64 display lists (Fast3D / F3DEX2 translated to citro3d). We found none that, like this '
             'project\'s native renderer, skips display-list construction for whole categories of geometry and writes precomputed PICA200 '
             'command words instead.</li>'
             '<li>Our 60 was <b>race-only</b> at v1.0.0: menus ran on the interpreter with frame skipping. Since v1.1.0 the menus, '
             'transitions, course select and machine select draw natively too; the post-race results table is '
             'still heavy. Several of the projects above target the whole game and interpolate instead; that is a different trade, not a lesser one.</li>'
             '<li>No other native F-Zero X port on the 3DS was found. Wololo (2020), citing a Reddit compatibility list, names F-Zero X among the games with "playable performance" on the New 3DS under DaedalusX64.</li>'
             '<li>Not verified: Old 3DS frame rates for the SM64 port and DaedalusX64 (the sources are forum threads that returned HTTP 403), '
             'stereo support in DaedalusX64, and a numeric frame rate for Star Fox 64 3D.</li>'
             '<li>Also searched and not found: a Star Fox 64 (Starship), Perfect Dark, Banjo or 2Ship 3DS port; an upstream libultraship '
             '3DS backend; a Mupen64Plus or ParaLLEl-N64 core for 3DS.</li>'
             '<li>Correction to this post-mortem\'s own <a href="frontier.html">Frontier</a> page (written 09-03): it describes the SM64 '
             '3DS port as 30 Hz logic interpolated to 60. The README read for this page describes naive frame skipping below 30 fps and '
             'lists a separate 60 FPS patch; we could not confirm the interpolation description, and Frontier now links here.</li>'
             '</ul>')
    return page("boundaries.html", "Pushing the Boundaries", "".join(b))
