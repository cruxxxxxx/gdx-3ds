from framework import *
import pc_topology
from pc_diagrams import STYLE as PC_STYLE


def build(D):
    b = [PC_STYLE]
    b.append('<p class="eyebrow">10 · Agentic workflow</p><h1>How the fleet was run, and how the ship was steered</h1>')
    b.append('<p class="lede">One human, one orchestrating Claude session that lived for the whole project. In the first '
             'phase (2026-08-11 to 09-03), ' + esc(D["subagents"]) + ' subagents worked in ' + esc(D["worktrees"]) + ' git '
             'worktrees. A second phase (09-07 to 10-19) ran the same discipline at a larger scale, for longer, with a '
             'wider toolset — covered in its own section below. Throughout, the human never opened an editor, and held '
             'the only console, the only SD card, and the only pair of eyes that could be trusted.</p>')

    b.append(tiles([
        (esc(D["prompts"]), "messages typed by the user in the first phase (08-11 to 09-03)", True),
        (esc(D["subagents"]), "background subagent transcripts, first phase (the Agent tool only; the 1,103 "
                               "workflow agents are counted separately, see Cost)", True),
        (esc(D["worktrees"]), "separate git worktrees, each on its own branch, first phase", False),
        (esc(D["branches"]), "feat/3ds-* branches, first phase", False),
        (esc(D["workflows"]), "deep-research workflow runs, first phase", False),
        (esc(D["active_days"]), "active days between 08-11 and 09-03", False),
    ]))

    b.append('<h2>The topology</h2>')
    b.append('<p>One long-lived session was the orchestrator. It held the project memory, wrote the briefs, merged the branches, '
             'and talked to the user. Work was fanned out to background agents, each given a worktree, a branch named for its lever, a brief '
             'file committed into <code>docs/research/</code>, and a progress file it had to write before it was allowed to finish. '
             'Agents never talked to the user; the orchestrator relayed. Agents never touched mainline; the orchestrator merged after '
             'an emulator smoke test and, for anything risky, after a hardware verdict.</p>')
    b.append(pc_topology.figure_html())

    b.append('<h2>The rules the fleet lived by</h2>')
    b.append('<p>These accreted from failures (see <a href="difficulties.html">Difficulties</a>) and were copied into every brief as a '
             '\"common rules\" block:</p>')
    b.append(table(["Rule", "Born from"], [
        ["<b>Patch as pure delta.</b> Upstream submodules stay pristine; every change is a patch generated against the full current stack; the README lists the order; a clean-stack round-trip must pass.", "Double-carried hunks in the rival-detail patch, twice."],
        ["<b>Killswitch for everything.</b> Every lever has an ini key defaulting on, and later a row in the bottom-screen DBG tab, so hardware can A/B without a rebuild.", "The first hardware rounds could only compare builds."],
        ["<b>Receipt for everything.</b> A lever must print a log line proving it engaged (<code>[rt] mode=ahead</code>, <code>[c3d] anchor=…</code>, <code>texcacheMainMut=0</code>).", "A silent audio agent whose run was attributed to the wrong binary."],
        ["<b>Commit early, progress file first.</b> Agents die on token limits and API overloads; a dead agent with zero commits costs a day.", "Traffic, sky3b, P2 and the first bridge-cache agent."],
        ["<b>One emulator, one lock.</b> <code>/tmp/azahar.lock</code>, touched every 30 s while held; wait in turn; never kill another run.", "Agents killing each other's Azahar and deleting SD artifacts."],
        ["<b>Combined-lever screenshot before staging.</b> A race frame with every lever on, captured through the window id, inspected by eye.", "The HUD-destroying interaction between trifast and the atlas."],
        ["<b>Merge ini keys, never overwrite.</b> The user's SD ini holds their 3D and display choices.", "A round that shipped with stereo silently off."],
        ["<b>Never re-apply the patch list onto a patched tree.</b> Reset both submodules first.", "A corrupted decomp tree in the final week."],
    ]))

    b.append('<h2>Steering from outside the code</h2>')
    b.append('<p>The user\'s messages are short, frequent, and almost entirely about direction, evidence and verdicts. '
             'Reading all ' + esc(D["prompts"]) + ' of them in order, the pattern is a captain, not a programmer: set the goal, '
             'demand parallelism, bring the physical evidence, say yes or no, and protect the budget.</p>')
    b.append('<h3>Setting the goal and refusing the safe path</h3>')
    b.append(quote("08-11 03:44", "lets create a plan for team of agents to work. can we segment plan so we can have multiple work trees and multiple contributing agents ?"))
    b.append(quote("08-14 05:05", "only list options that work towards this running acceptably in 3D at 60fps"))
    b.append(quote("08-21 22:49", "played a bunch, running well. no hitches. music still plays. rest of issues are what's left. would love to see the framerate at a crispy 60. then... maybe 3D?"))
    b.append(quote("08-28 04:27", "moonshot! lets go for it"))
    b.append('<h3>Demanding parallelism</h3>')
    b.append(quote("08-14 01:10", "i didnt see any hard crashes right now, so lets see about the visual fixes. any work that can be done in parallel ?"))
    b.append(quote("09-02 21:59", "lets do /deep-research on whats left, come up with plan, and execute subagents for all parallel work"))
    b.append('<h3>Being the oracle</h3>')
    b.append('<p>The agents could not see the console. The user ran the SD-card loop dozens of times (\"SD in\", \"SD returned\", '
             '\"SD card is back in\" appear ' + esc(D["sd_mentions"]) + ' times) and reported what a human sees: shapes, colours, '
             'hitches, feel. Those reports were the only ground truth for the visual bugs and for every fps number on the '
             '<a href="road-to-60.html">Road to 60</a> page.</p>')
    b.append(quote("08-14 13:21", "yep definitely looks like the game but its still quite buggy. some of the elements jitter/shimmer, textures change. opening tunnel doesnt have a roof, might have to do with backface culling?"))
    b.append(quote("08-21 15:54", "wow it runs amazing ! like you were suggesting, maybe even better than the emu. sound doesn't work btw. sound has never worked on the emu either."))
    b.append(quote("08-28 04:22", "minimal appears to be a 8 to 9 fps gain ! sometimes a little more. def worth it."))
    b.append(quote("09-03 16:32", "next thing to consider: 3D works great for 3D elements, but UI elements are a little less consistent. one thing thats really jarring is the 1P/2P/3P markers during a race. they sit on the most foreground in 3D but the ships are somewhere in the distance"))
    b.append('<h3>Pushing back on the machine</h3>')
    b.append(quote("08-21 00:23", "nothing new to report here. all the same issues. anything we can do ? instanity is doing the same things and expecting different results. /deep-research different methods of fixing these issues."))
    b.append(quote("08-21 20:40", "uh dude no, i heard the tone sound before even. and i heard the music fine earlier. its not azahar."))
    b.append(quote("08-15 16:33", "also mark that it was done with opus and not fable so it should be reconsidered if this is the best approach"))
    b.append('<h3>Guarding the budget and the record</h3>')
    b.append(quote("08-14 14:06", "we ran out of fable credits until next week, lets capture all the inflight tasks and store state so we can pick up the work later."))
    b.append(quote("08-28 13:17", "lets try pushing the last lever. lets also write up the README for release. lets make sure we clearly declare this was just for educational purposes, not affiliated with nintendo, doesnt provide ROM or assets"))
    b.append(quote("09-03 13:20", "also, is there any way you could create a webpage with some histograms of the performance + documentation on how the changes all affected performance?"))
    b.append('<h3>Working through the night</h3>')
    b.append('<p>Twice (08-20 and 08-21) the user went to sleep with the instruction to keep working. Each night session ran a fleet '
             'against a frozen mainline, wrote a morning-status document, and left a candidate branch to promote after the user\'s eyeball. '
             'The 08-20 candidate was promoted with \"sure, promote it. i dont see any large regressions. probably better\"; '
             'the 08-21 night produced the S7 interpreter memo and the async frame-mirror copy that took the menus from 15 to 20 fps in the emulator.</p>')

    b.append('<h2>What the orchestrator did that an agent could not</h2>')
    b.append('<ul>')
    b.append('<li><b>Held the memory.</b> A single project memory file (' + esc(D["memory_lines"]) + ' lines by the end) was rewritten at every '
             'milestone with state, gotchas, and the next step, and three RESUME documents were committed at every credit pause. '
             'Every context compaction and every new session restarted from it.</li>')
    b.append('<li><b>Wrote the briefs.</b> Each lever got a brief with the hypothesis, the file locations, the measurement protocol, the '
             'killswitch name, the receipt format and the report format. The briefs are in the repository.</li>')
    b.append('<li><b>Finished what agents dropped.</b> The texture-cache crash fix, the viewport fix and the stereo anchor were completed by '
             'the orchestrator after the assigned agents died on API overloads.</li>')
    b.append('<li><b>Ran the hardware loop.</b> Build, package (.3dsx + .cia + test plan), copy to SD, eject, read the log back, '
             'compute the fps distribution, decide merge or revert.</li>')
    b.append('</ul>')

    b.append('<h2>The second phase: longer, wider, with an adversary</h2>')
    b.append('<p>The first phase was a 24-day sprint to a single target. The second (09-07 to 10-19, 43 calendar days) '
             'built a different renderer, a second console tier and a second game mode on top of a build that already '
             'worked, which changed the shape of the work more than its length: many more parallel workstreams, each '
             'still in its own worktree with its own brief and progress file, left behind 199 internal progress notes '
             'by the end, far more than the first phase\'s smaller written record. The rules in the table above (patch '
             'as pure delta, killswitch for everything, receipt for everything, commit early) held and were leaned on '
             'harder, because more of the work now ran unattended for longer between human checks.</p>')
    b.append('<p>Three things were added to the loop that the first phase did not have:</p>')
    b.append('<ul>')
    b.append('<li><b>A network link to the console.</b> The first phase\'s only path to hardware was ejecting the SD '
             'card, copying files, and reinserting it. The second phase added a lightweight network link (a '
             'homebrew-loader push plus a small device-link tool) so a build could reach a development console '
             'directly over Wi-Fi, cutting the time between a build finishing and a human seeing it run.</li>')
    b.append('<li><b>An emulator replay lane for proof without hardware.</b> A record-and-replay harness captured '
             'exact input sequences and per-frame state hashes, so a new render path could be checked frame-for-frame '
             'against the old one — bit-identical pixels, identical game-state hashes — without needing the console '
             'or a human at all. This is what let dozens of rendering changes be verified as behaviourally identical '
             'before ever being staged for a hardware round, the same role the emulator played in the first phase but '
             'run far more often and more mechanically.</li>')
    b.append('<li><b>A standing adversarial review.</b> After a window where many parallel branches landed in quick '
             'succession, a dedicated review pass audited the merged result for what the individual branches\' own '
             'testing could not catch — not new bugs in any one branch, but damage from combining them: two agents '
             'measuring the same thing with code that quietly disagreed, a comparison tool whose flag got pasted out '
             'of existence, a safety wait that had become unreachable. Findings were ranked by severity and marked '
             'confirmed or suspected, and the practice repeated after later merge windows.</li>')
    b.append('</ul>')
    b.append('<p>What stayed the same: one orchestrating session, one human approving every merge into the branch that '
             'builds toward release, and no agent ever touching the only SD card or the only console directly.</p>')

    b.append('<h2>Where the workflow was pioneering</h2>')
    b.append('<p>Multi-agent coding is usually demonstrated on web apps with a test suite. This project ran it against a target '
             'the agents could not observe, with a human-in-the-loop measurement cycle measured in SD-card round trips, on a codebase '
             'made of three repositories (decomp, runtime, port) with a patch stack instead of a fork. The <a href="frontier.html">'
             'Frontier</a> page names the devices that made that work transferable. What this page adds: the second phase runs the same '
             'argument at a larger scale — the same devices and discipline, held across more than twice the calendar time, more console '
             'configurations, and a ground-up renderer rewrite.</p>')
    return page("agentic.html", "Agentic Workflow", "".join(b))
