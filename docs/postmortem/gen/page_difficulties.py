from framework import *



def build(D):
    b = []
    b.append('<p class="eyebrow">07 · Difficulties</p><h1>What fought back</h1>')
    b.append('<p class="lede">A New 3DS is an 804 MHz ARM11 with 124 MB for the app, a GPU that speaks neither OpenGL nor '
             'anything a PC port has met, and a debugger that is a text file on an SD card. Every layer of the stack '
             'assumed a PC. These are the fights worth retelling: each one\'s root cause and what it took.</p>')

    b.append('<h2>The shape of the problem</h2>')
    b.append('<p>G-Diffuser is the F-Zero X decompilation running on libultraship, a runtime that replays the N64 display '
             'lists on a modern GPU through a software Fast3D interpreter. On a PC that interpreter is a rounding error. '
             'On the 3DS it was the entire frame budget: a 16.7 ms frame with a CPU that spent 35 to 58 ms just building '
             'the frame in the first emulator measurements. The GPU was idle the whole time. The port therefore turned '
             'into a CPU project disguised as a graphics project, which is the opposite of what a console port normally is.</p>')
    b.append(table(["Constraint", "Why it hurt"], [
        ["ILP32 pointers", "Every \"small address means N64 offset\" heuristic in libultraship and the bridge misfired: the image sits at 0x100000 and the heap at 0x8000000, both inside the N64's 8 MB RDRAM range."],
        ["PICA200 fixed-function GPU", "No fragment shaders. Every combiner mode had to fit three TexEnv stages, and fog goes through a depth LUT that F-Zero X's depth range defeats."],
        ["Debugging on hardware", "Mostly a log file on the SD card and Luma exception dumps. The Old 3DS test unit has no GDB stub, so profiling there meant performance counters."],
        ["Azahar is not a 3DS", "Slower than hardware on some screens, faster on others, no instruction cache, no memory permissions, stale screenshots. Hardware was the only truth, and hardware needed the user's hands."],
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
             'written into the primary-colour alpha and blended in one TexEnv stage. Solved 08-20 by an Opus session while the weekly usage '
             'allowance for the project\'s regular model (the "Fable" tier) was out.</p></div>')
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

    b.append('<h2>HOME, twice</h2>')
    b.append('<p>On 09-01 the user found that pressing HOME crashed the game instantly. The audio threads were hitting the DSP through '
             'the ndsp suspend. Fixing that exposed a second bug: closing from the HOME menu hung the console. libctru wakes the app with '
             '<code>APTCMD_WAKEUP_CANCEL</code>, no restore; the drain thread woke on a shared sticky park event, re-waited, and '
             '<code>LightEvent_Wait</code> on an already-signalled sticky event returns instantly, so a hot spinner on the producer\'s core '
             'starved the producer and the join never returned. Fix: per-thread park events, stop-before-signal, flushed exit receipts. '
             'Verified on hardware 09-02: HOME in, HOME out, close from HOME, lid sleep, power off.</p>')
    b.append('<p>Seven weeks later, on the Old 3DS, closing from HOME hard-locked the console (10-26). At exit the 64DD save flush asked '
             'for one contiguous 73 KB buffer; about 390 KB was free, but not in one piece. <code>bad_alloc</code> went to '
             '<code>std::terminate</code>, then <code>svcBreak</code>, which locks the console instead of exiting. The flush now streams '
             'from its journal with no heap allocation, and a break during teardown exits.</p>')

    b.append('<h2>Concurrency: the round-4 hard crash</h2>')
    b.append('<p>The render thread on core 2 delivered the best numbers of the project and, after a full Grand Prix, a data abort in the '
             'texture cache LRU splice while the main thread was mutating the same list during podium and venue loads. The fix made the '
             'render thread the sole owner of the cache: clears become requests drained on the render thread, deletes are queued, and a '
             'receipt (<code>texcacheMainMut=0</code>) proves no main-thread mutation happened.</p>')

    b.append(old3ds())
    b.append(process())
    return page("difficulties.html", "What Fought Back", "".join(b))


def old3ds():
    b = []
    b.append('<h2>Bugs only the Old 3DS could show (09-24 → 10-26)</h2>')
    b.append('<p>Each of these looked fine in the emulator. The per-card numbers are on <a href="native.html#old-3ds">The road to 60, all over again</a>.</p>')
    b.append(table(["Fight", "Symptom", "Root cause and fix"], [
        ["HOME freeze", "HOME froze the game image; power-off needed",
         "Not audio, not the file system, not core 1: probe apps ruled each out. The bottom-backlight display session was held open for "
         "the process lifetime and the HOME menu blocked on it. Open, call, close per toggle; 7/7 resumes clean"],
        ["Cold boot crash", "The first release candidate crashed after graphics setup, no dump",
         "A four-build bisect: size-optimised code crashes cold, stock code boots. Code generation reverted to stock; the underlying bug was never found"],
        ["Verify-mode freeze", "The first native-renderer build froze at race start",
         "A state save/restore copied the GPU's live command queue too, rolling back a completion the hardware had already acted on. "
         "The emulator's equivalent is synchronous and could not show it"],
        ["Saves never persisted", "Records lost after the first save",
         "The SD rename fallback triggers on one error code; the SD archive answers a different one. A crash-safe replace with boot "
         "recovery fixed it, on both models"],
        ["\"all 3 crashed\"", "Random pixels at boot, no log",
         "Framebuffers moved to VRAM, which an installed title maps read-only for the CPU; the first bottom-screen write aborted. The "
         "emulator does not enforce that permission"],
        ["Fragmentation, not exhaustion", "After six course loads the fast track path quietly fell back to the slow one",
         "A 96,768-byte allocation failed with 1.6 MB free. The track now lives in one arena reserved at boot"],
    ]))
    return "".join(b)


def process():
    b = []
    b.append('<h2>Process failures, honestly</h2><ul>')
    b.append('<li><b>Agents died</b> on token limits and API overloads, sometimes with zero commits. The rule became "commit per '
             'milestone, write the progress file first". On 09-30 all seven running agents hit the limit together.</li>')
    b.append('<li><b>Agents tested levers singly.</b> Four LOCKED-60 levers each passed alone and together broke the HUD. A '
             'combined-lever screenshot became mandatory.</li>')
    b.append('<li><b>Agents fought over the emulator</b>, killing each other\'s sessions and deleting SD artifacts, until a lock file. '
             'A cleanup command with an empty run id later deleted the shared emulator directory; scripts now refuse an empty id.</li>')
    b.append('<li><b>The screenshot oracle lied.</b> Captures were a frame or two stale. The user\'s eyes became the only trusted display oracle.</li>')
    b.append('<li><b>Gate holes.</b> Parity gates ran with stereo on, so two categories refused and were never exercised; found and re-run.</li>')
    b.append('<li><b>Bad comparisons.</b> A 617 KB "heap regression" compared a 2D multiplayer log with a 1P 3D log; an early per-category '
             'baseline was measured on warm-up frames.</li>')
    b.append('<li><b>Pauses.</b> The weekly allowance ran out three times in the first phase and stopped the fleet twice in the second; '
             'each pause left a RESUME document.</li>')
    b.append('</ul>')
    return "".join(b)
