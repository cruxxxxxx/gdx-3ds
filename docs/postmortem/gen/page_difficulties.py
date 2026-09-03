from framework import *


def build(D):
    b = []
    b.append('<p class="eyebrow">04 · Difficulties</p><h1>What fought back</h1>')
    b.append('<p class="lede">A New 3DS is an 804 MHz ARM11 with 124 MB for the app, a GPU that speaks neither OpenGL nor '
             'anything a PC port has met, and a debugger that is a text file on an SD card. Every layer of the stack '
             'assumed a PC. This page is the war log: each fight, its root cause, and what it took to win.</p>')

    b.append('<h2>The shape of the problem</h2>')
    b.append('<p>G-Diffuser is the F-Zero X decompilation running on libultraship, a runtime that replays the N64 display '
             'lists on a modern GPU through a software Fast3D interpreter. On a PC that interpreter is a rounding error. '
             'On the 3DS it was the entire frame budget: a 16.7 ms frame with a CPU that spent 35 to 58 ms just building '
             'the frame in the first emulator measurements. The GPU was idle the whole time. The port therefore turned '
             'into a CPU project disguised as a graphics project, which is the opposite of what a console port normally is.</p>')
    b.append(table(["Constraint", "Why it hurt"], [
        ["ILP32 pointers, 32-bit address space", "Every \"small address means N64 offset\" heuristic in LUS and the bridge misfired: the image sits at 0x100000 and the heap at 0x8000000, both inside the N64's 8 MB RDRAM range."],
        ["124 MB app region, 64 MB on Old 3DS", "Old 3DS formally dropped in week one. Assets prebaked into o2r archives; ROM never touches the console."],
        ["PICA200 fixed-function GPU", "No fragment shaders. Every N64 color-combiner mode mapped to at most 3 TexEnv stages (combiner census, 2026-08-12). Fog goes through a depth-indexed LUT that F-Zero X's depth range defeats."],
        ["Rotated 240×400 framebuffer", "Everything renders portrait and is rotated by a fixup matrix. Viewports, scissors, and the stereo frustum shift all live inside that matrix."],
        ["Three cores, one of them yours", "Core 0 runs the game, core 1 is fractional (APT_SetAppCpuTimeLimit), core 2 exists only if Luma grants the kernel cap, core 3 belongs to the OS."],
        ["No debugger, no stdout", "Receipts written to a filelog ring buffer on the SD card, Luma exception dumps symbolized against the ELF, svcOutputDebugString only visible in the emulator."],
        ["Azahar is not a 3DS", "The emulator's dynarmic JIT and host CPU made it slower than hardware on menus and faster on others, could not time-slice core 2 honestly, and had a stale-frame screenshot oracle. Hardware was the only truth; hardware needed the user's hands."],
    ]))

    b.append('<h2>The boot fights (2026-08-12 → 08-14)</h2>')
    b.append('<p>Getting from a linked binary to a race took three days and six distinct root causes, each of which '
             'presented as the same symptom: a black screen.</p>')
    b.append('<div class="timeline">')
    b.append(event("08-13", "Negative-size bzero", "The non-Expansion-Kit path computed a negative buffer size and cleared memory until it hit an unmapped page. First black-screen hang."))
    b.append(event("08-13", "Phantom 64DD", "A NULL dereference in disk-drive detection code that the PC build never reached."))
    b.append(event("08-13", "std::filesystem::absolute vs sdmc:", "The archive loader normalised <code>sdmc:/…</code> into a nonsense path, so the asset index came back empty and every texture was missing. Fixed by treating device prefixes as roots."))
    b.append(event("08-13", "Double buffer swap", "libultraship and citro3d both presented; frames alternated between real and stale."))
    b.append(event("08-13", "The SETTIMG low-address guard", "libultraship drops texture pointers below a threshold on the assumption that small numbers are N64 offsets. On the 3DS the heap starts at 0x8000000, under that threshold, so <i>every</i> texture was dropped. This is the bug class that recurred for two weeks.", "big"))
    b.append(event("08-13", "DMA resolver misrouting", "Program globals were being redirected to the RDRAM shadow, freezing the race the moment it loaded."))
    b.append(event("08-14 01:01", "First race", "\"omg it works ! i can race ! it looks messed up and runs at 12 to 20fps but it works!\"", "gold big"))
    b.append('</div>')

    b.append('<h2>The visual fights (08-14 → 08-27)</h2>')
    b.append('<div class="cols2">')
    b.append('<div><h3>Pink road: fog through the wrong door</h3><p>citro3d routes fog through the PICA fog unit, a 128-entry lookup '
             'table indexed by fragment depth. F-Zero X compresses the whole track into a clip-depth band about 0.01 wide (0.98 to 0.995), '
             'which lands on two LUT entries, so every road fragment got full fog. A first \"batch split\" fix was proven irrelevant by a '
             'probe line that produced an identical frame. The real fix bypasses the LUT: the interpreter\'s per-vertex fog factor is '
             'written into the primary-colour alpha and blended in one TexEnv stage. Solved 08-20 by an Opus session while Fable credits were out.</p></div>')
    b.append('<div><h3>Scrambled HUD: eight wrong fixes, one right one</h3><p>The speedometer, lap and time readouts drew as static sheared '
             'garbage. Eight decode-layer fixes (RGBA16 word swaps, tile extents, atlas strips) failed because the bug was upstream: '
             'the bridge\'s file-path backstop required non-zero <i>high 32 bits</i> of a pointer, always zero on ILP32, so it silently '
             'dropped 13 SETTIMG commands per frame. The counter that said <code>skip=13</code> had been in the log for days. '
             'User on 08-20: \"omg finally!!\". Lesson recorded: evidence-first tripwires at consumption points beat plausible-layer guessing.</p></div>')
    b.append('<div><h3>Yellow Falcon: the decal that tiled seven times</h3><p>The Blue Falcon wore yellow scribbles. Not a decode bug: libultraship '
             'strips clamp flags for mirror-and-clamp tiles and the backend sampled unclamped, so the Falcon\'s own stripe decal repeated across '
             'the body. Then a second cause: the body tint (<code>gDPSetEnvColor</code>) did not flush pending triangles, so six machines sharing a '
             'batch were all painted in the first machine\'s colour. Right in the select screen (one machine per batch), wrong in the race.</p></div>')
    b.append('<div><h3>Black sky wedge</h3><p>A black polygon swallowed the sky depending on camera pitch. Three theories died (fog regression, '
             '4:3 skybox under-covering the 16:10 display, cloud layer). Fourth was right: a 64×1 sky gradient padded to 64×8 for power-of-two, '
             'clamped at the padded edge, sampled zeroed padding when the game\'s wild T-coordinates ran off the tile window. Proven by flooding the '
             'padding magenta. Third bug of the power-of-two padding family.</p></div>')
    b.append('<div><h3>Explosion flicker: CCMUX 11</h3><p>Ship explosions spammed \"Unsupported ccmux 11\". That is <code>G_CCMUX_SHADE_ALPHA</code> '
             'in the C slot: debris is (1−SHADE)·SHADE_ALPHA+SHADE, a white-hot flicker. Mapped, plus a dead-cycle skip, which made the combiner '
             'translation total across the game.</p></div>')
    b.append('<div><h3>Menu ships 1.6× too large</h3><p>Found by the user on 09-03 with side-by-side screenshots against the real game. '
             'F-Zero X translates a full-size 320×240 viewport onto each grid cell; the backend clamped and unsigned-cast the negative '
             'rectangles. Fix: fold any non-full-window viewport into the per-draw projection (NDC scale and offset, scissor intersection, '
             'hor+ x-scale). Killswitch <code>vpfix</code>. Fixed by the orchestrator after the assigned agents died on API overloads.</p></div>')
    b.append('</div>')

    b.append('<h2>Audio: the fight nobody mentioned</h2>')
    b.append('<p>The first hardware run on 08-21 came with a confession: sound had never worked, in the emulator or on the console, '
             'and the user had not said so because the DSP firmware setup looked correct. The memory file had recorded \"HLE audio works\" '
             'because producer counters ticked. The output was silent. What followed was the roughest afternoon of the project.</p>')
    b.append(quote("User, 08-21 18:08 → 20:40", "noooo the sound didnt work on the machine :( … SD is back in, round trip. no sound again … finally heard the tone ! … SD back in.. no sound :( … uh dude no, i heard the tone sound before even. and i heard the music fine earlier. its not azahar."))
    b.append('<p>Two mistakes compounded. An agent was killed mid-report and its audible test run was attributed to the wrong build, '
             'which started a three-hour phantom hunt. The true cause was that the ROM-side audio driver itself needed host porting '
             '(about 700 lines across the load, sequence player, thread and heap files) plus linear interpolation for real DSP output, '
             'because the polyphase path was silent on hardware and fine in the emulator. At 21:22: \"working! giant win.\" '
             'The process lessons went straight into the memory file: never kill an agent on partial narration, always bind an audible '
             'run to a verified binary, silence verdicts need a long soak.</p>')

    b.append('<h2>Lifecycle: HOME, sleep, close</h2>')
    b.append('<p>On 09-01 the user found that pressing HOME crashed the game instantly. The audio threads were hitting the DSP through '
             'the ndsp suspend. Fixing that exposed a second bug: closing from the HOME menu hung the console. libctru wakes the app with '
             '<code>APTCMD_WAKEUP_CANCEL</code>, no restore; the drain thread woke on a shared sticky park event, re-waited, and '
             '<code>LightEvent_Wait</code> on an already-signalled sticky event returns instantly, so a hot spinner on the producer\'s core '
             'starved the producer and the join never returned. Fix: per-thread park events, stop-before-signal, flushed exit receipts. '
             'Verified on hardware 09-02: HOME in, HOME out, close from HOME, lid sleep, power off.</p>')

    b.append('<h2>Concurrency: the round-4 hard crash</h2>')
    b.append('<p>The render thread on core 2 delivered the best numbers of the project and, after a full Grand Prix, a data abort in the '
             'texture cache LRU splice while the main thread was mutating the same list during podium and venue loads. The fix made the '
             'render thread the sole owner of the cache: clears become requests drained on the render thread, deletes are queued, and a '
             'receipt (<code>texcacheMainMut=0</code>) proves no main-thread mutation happened.</p>')

    b.append('<h2>Process failures, honestly</h2>')
    b.append('<ul>')
    b.append('<li><b>Agents died.</b> On token limits (traffic, sky3b, P2 perf, bridge cache first attempt: zero commits) and, in the '
             'final week, repeatedly on API 529 overloads. The rule became \"commit early, commit per milestone, write the progress file first\".</li>')
    b.append('<li><b>Agents tested levers singly.</b> The four LOCKED-60 levers each passed alone and together broke the entire HUD: '
             'the fast triangle loop lacked the atlas UV offset. A combined-lever race screenshot became mandatory before staging.</li>')
    b.append('<li><b>Agents fought over the emulator.</b> Concurrent runs killed each other\'s Azahar sessions and deleted SD artifacts twice. '
             'A lock file with a touch-every-30-seconds protocol and per-run log provenance markers fixed it.</li>')
    b.append('<li><b>The patch stack bit twice.</b> Regenerating a decomp patch against an already-patched tree double-carried hunks. '
             'Rule: patches are pure deltas against the full stack; reset both submodules before re-applying.</li>')
    b.append('<li><b>The orchestrator\'s own bisect error</b> blamed a menu regression on the wrong branch (a bad display-list cache rode an early merge).</li>')
    b.append('<li><b>The screenshot oracle lied.</b> Scanout captures were a frame or two stale, so \"fixed\" screenshots were not. '
             'The user\'s eyes were declared the only trustworthy display oracle.</li>')
    b.append('<li><b>Three credit pauses.</b> Fable 5 credits ran out on 08-14, 08-22 and 08-28. Each pause produced a RESUME document so the next session could restart cold.</li>')
    b.append('</ul>')
    return page("difficulties.html", "What Fought Back", "".join(b))
