from framework import *


def build(D):
    b = []
    b.append('<p class="eyebrow">05 · Frontier</p><h1>Why this is bold, and where it is new</h1>')
    b.append('<p class="lede">The user asked on 08-23 how to explain to an unimpressed friend why this port matters. '
             'The short version: nobody had run a libultraship game on a handheld this weak, nobody had pushed a '
             'software N64 display-list interpreter to a native 60 Hz on an ARM11, and nobody had done it with '
             'stereoscopic 3D, in three and a half weeks, with a fleet of AI agents doing the typing.</p>')

    b.append('<h2>The field before this project</h2>')
    b.append('<p>Two deep-research passes (08-14, 09-01) and a targeted literature check (09-02) mapped the state of the art. '
             'The findings framed every architectural decision:</p>')
    b.append(table(["Precedent", "What it did", "What it means here"], [
        ["sm64_3ds (the well-known Mario 64 3DS port)", "Native decomp with a hand-written 3DS renderer. Runs its game logic at 30 Hz and interpolates to 60. Plain <code>switch</code> dispatch. Audio on core 2.", "The proven fallback is 30 Hz + interpolation. This project refused that and targeted native 60."],
        ["libultraship ports (Ship of Harkinian, 2Ship, Starship, and this game's own PC port)", "Desktop-only. x86-64 and ARM64 with OpenGL, Metal or DirectX. Never compiled for devkitARM.", "The LUS carve-out spike (08-12) proved LUS builds on devkitARM with a 13-line newlib patch. First time."],
        ["Every N64 port ever shipped", "Walks the display list on the CPU every frame and re-emits it to the GPU.", "The 09-01 research verdict: nobody has shipped beyond the per-frame walk. GPU vertex transform was the field's number-one untried idea, listed as a TODO by the sm64 port's author."],
        ["Cross-core render pipelining on 3DS", "No precedent found in any homebrew title. Core 2 needs a Luma kernel grant; core 3 is the OS's.", "The render thread (09-02 → 09-03) is, as far as the research could find, the first game-logic-ahead render pipeline on the console."],
    ]))

    b.append('<h2>Six things this project did first, or did differently</h2>')
    b.append('<div class="cols2">')
    b.append('<div><h3>1. libultraship on a 3DS</h3><p>The whole runtime, including its Fast3D interpreter, resource manager and archive '
             'loader, compiled and running on ARM11 under devkitARM. The port lives as a stack of pure-delta patches (' + esc(D["patches"]) +
             ' files) over two untouched upstream submodules, so the upstream projects could take any of it.</p></div>')
    b.append('<div><h3>2. Native 60 on a software display-list interpreter</h3><p>The measured position at the end of the project on a full '
             'Grand Prix, trace off: median ' + esc(D["final_median"]) + ' fps, tenth percentile ' + esc(D["final_p10"]) +
             ', ' + esc(D["final_cap"]) + ' of frames at the 60 cap. Menus at 60. Achieved without dropping the logic rate or interpolating.</p></div>')
    b.append('<div><h3>3. Exact stereoscopy inside the projection fixup</h3><p>Stereo 3D is implemented as an asymmetric-frustum shift folded '
             'into the projection fixup uniform, in exact clip-space form: <code>shift(d) = ±sep·(d−dc)/(1−dc)</code>. The right eye is a '
             'replay of the packed vertex buffers with a different matrix, no second interpreter walk. The user\'s hardware verdict on 08-27: '
             '\"3D works great, doesnt seem to affect performance much at all.\" The final touch (09-03) anchors the race position markers, '
             'which are 2D rectangles, at the depth of the ship they label by reading the game\'s own prim-depth register.</p></div>')
    b.append('<div><h3>4. GPU vertex transform, working, then shelved on evidence</h3><p>The \"moonshot\" of 08-28 moved the unlit vertex '
             'transform onto the PICA vertex shader: 220,000 draws with zero fallbacks, 62% less vertex time in the emulator. On hardware it was '
             'imperceptible (the unlit slice was too small) and it broke texture mapping on the venue floor because the PICA\'s 24-bit uniforms '
             'cannot hold ±32000-unit coordinates. It was preserved on its branch, documented, and shelved. Nobody in the lineage had built it before.</p></div>')
    b.append('<div><h3>5. A render thread inside the game\'s own frame protocol</h3><p>Rather than a mailbox, the fork happens where the N64 '
             'would have started the RSP task (<code>osSpTaskStartGo</code>) and the join is the game\'s own DP-done wait. Mode 2 acknowledges '
             'DP-done when the game parks, giving a true one-frame-ahead pipeline with two-deep backpressure. The bridge pre-pass moved to the main '
             'core to balance the load. Hardware round 4: median 59.6 fps.</p></div>')
    b.append('<div><h3>6. Fixed-function combiner totality</h3><p>Every N64 colour-combiner mode the game uses maps to three or fewer TexEnv '
             'stages; fog is a per-vertex factor blended in a TexEnv stage because the PICA\'s depth LUT cannot resolve the game\'s depth range; '
             'the decal-clamp, environment-colour-flush and CCMUX-11 fixes made the translation total with zero unsupported modes.</p></div>')
    b.append('</div>')

    b.append('<h2>Why bold</h2>')
    b.append('<p>The conservative plan was available from day one and was declined at every fork:</p>')
    b.append('<ul>')
    b.append('<li><b>Native 60 Hz instead of 30 Hz + interpolation.</b> The deep-research report on 08-21 said the proven path was the sm64 way. '
             'The user\'s instruction on 08-14: \"only list options that work towards this running acceptably in 3D at 60fps.\"</li>')
    b.append('<li><b>Stereo before the frame budget was safe.</b> The stereo foundation was merged flag-off on 08-20 and armed on 08-27 while '
             'crowd frames still dipped to 30 fps, on the argument that it costs CPU submit time, not GPU time, and had to be designed in.</li>')
    b.append('<li><b>Multi-core on a console that hides its cores.</b> Core 2 is only reachable with a kernel capability that Luma grants to '
             'homebrew; the choice to depend on it was made after the 09-02 research and a hardware confirmation that the grant holds for a .3dsx and a .cia.</li>')
    b.append('<li><b>Patch stack, not a fork.</b> Keeping upstream pristine cost real pain (every patch a pure delta, regenerated by three-way '
             'merge) and kept the door open to upstreaming. One leak fix in libultraship was identified as an upstream bug on 08-28.</li>')
    b.append('<li><b>Shelving on evidence, not on effort.</b> GPU transform, the bridge output cache, the DSP batch-break fold, the double-height '
             'stereo target and the machine texture atlas were each built to the point of measurement and then declined. The project\'s '
             'default-off branches are its most expensive documents.</li>')
    b.append('</ul>')

    b.append('<h2>Innovation in method</h2>')
    b.append('<p>The technical results rest on a way of working that is itself new. Every performance change shipped with a '
             'killswitch and a receipt line, so hardware could A/B it without a rebuild. Every hypothesis got a probe before a fix '
             '(the magenta padding flood, the fog probe line, the CRC tripwires that pinned the HUD bug). Every hardware round had a test '
             'plan written for a human and a log format written for a machine. The <a href="agentic.html">agentic workflow</a> page covers '
             'how that discipline was enforced across a fleet of agents that could not see the console.</p>')
    return page("frontier.html", "Frontier", "".join(b))
