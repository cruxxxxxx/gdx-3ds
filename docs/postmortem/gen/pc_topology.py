"""The agentic page's topology diagram: who talked to whom, and which loops reached real hardware.

Sources: page_agentic.py (first-phase structure), docs/HW-DEV-LOOP.md (netloader push on TCP 17491,
in-game control listener on TCP 17492, the Mac-side log server the console dials), tools/dev/gdxdev/
(lock.py, retro.py), emu-slot.sh (a machine-wide pool of three emulator slots), tools/mp-rendezvous/
README.md (UDP 27200 rendezvous and relay) and the rendezvous ops repo (EC2 t4g.nano, us-east-2).
"""
from pc_diagrams import Canvas, measure, tree, fig

FS = 12.5

HUMAN = ("The human", "Sets goals, plays the builds, judges what he sees, says yes or no to every merge. Never opens an editor.")
ORCH = ("Orchestrator session", "One long-lived Claude Code session: project memory, briefs, merges, the only voice the human hears. "
        "RESUME notes at every credit pause.")
REVIEW = ("Review tools", "Adversarial review passes after merge windows (findings ranked, confirmed or suspected); frame, hash "
          "and screenshot diff tools; a local review page for the post-mortem's own notes.")
AGENTS = ("Subagents", "One worktree and one branch per lever, a committed brief and a progress file. Killswitch + receipt "
          "line for every change. 147 in the first phase alone.")
FLOWS = ("Workflows", "Scripted fan-outs: deep research (search agents → fetch → verify → synthesise), parallel censuses "
         "and reviews.")
EMU = ("Emulator farm", "Azahar (one Qt instance, one lock) plus RetroArch Azahar instances, each with a private SD, "
       "behind a three-slot machine-wide pool. Tick-exact input replays with per-frame state hashes.")
BUILD = ("Hardware lane", "One combined test build per console (auto A/B, auto logs). In: SD card trip, or 3dslink push to "
         "the netloader (TCP 17491), or in-game reload (TCP 17492). Out: live log to a server on the Mac, log.txt on SD.")
CONSOLES = ("New 3DS + Old 3DS", "The only ground truth for frame rate, feel and visual bugs. Old 3DS mostly by SD card.")
AWS = ("Rendezvous server (AWS)", "v1.1.0 online play: UDP 27200 room codes, hole punching, relay fallback. EC2 t4g.nano, "
       "run by agents through Terraform and an ops script.")


def _wide():
    W = 980
    L, Lw = 14, 236          # left column
    C, Cw = 330, 320         # centre column
    R, Rw = 730, 236         # right column
    c = Canvas(W, 10, "Topology of the agent fleet and its hardware loop")
    # row 1
    y1 = 16
    h1 = max(measure(Lw, *HUMAN, fs=FS), measure(Cw, *ORCH, fs=FS), measure(Rw, *REVIEW, fs=FS))
    c.box(L, y1, Lw, h1, *HUMAN, tone="gold", fs=FS)
    c.box(C, y1, Cw, h1, *ORCH, tone="acc", fs=FS)
    c.box(R, y1, Rw, h1, *REVIEW, fs=FS)
    m1 = y1 + h1 / 2
    c.arrow([(L + Lw, m1), (C, m1)], "goals,|verdicts,|photos", both=True, loff=(0, -26))
    c.arrow([(C + Cw, m1), (R, m1)], "audit|the|merges", both=True, loff=(0, -26))
    # row 2: hardware lane (left), agents band (centre), emulator farm (right)
    y2 = y1 + h1 + 70
    aw = (Cw - 60) / 2
    h2 = max(measure(Lw, *BUILD, fs=FS), measure(aw, *AGENTS, fs=FS), measure(aw, *FLOWS, fs=FS), measure(Rw, *EMU, fs=FS))
    c.box(L, y2, Lw, h2, *BUILD, tone="warn", fs=FS)
    ax1 = C + 40
    ax2 = ax1 + aw + 10
    c.band(C + 30, y2 - 26, Cw - 30, h2 + 36, "the fleet")
    c.box(ax1, y2, aw, h2, *AGENTS, fs=FS)
    c.box(ax2, y2, aw, h2, *FLOWS, fs=FS)
    c.box(R, y2, Rw, h2, *EMU, tone="good", fs=FS)
    # orchestrator <-> fleet
    c.arrow([(ax1 + aw / 2, y1 + h1), (ax1 + aw / 2, y2 - 26)], "briefs ↓  commits,|progress ↑", both=True,
            lanchor="start", loff=(8, -4))
    # orchestrator -> hardware lane, through the free lane at x = C + 14
    lane = C + 12
    m2 = y2 + 40
    c.arrow([(lane, y1 + h1), (lane, m2), (L + Lw, m2)], "merge, build|← logs", both=True,
            lanchor="end", loff=(-8, -44))
    # fleet -> emulator
    c.arrow([(C + Cw, y2 + h2 / 2), (R, y2 + h2 / 2)], "smoke,|parity,|shots", loff=(0, -32))
    # row 3: consoles (left), rendezvous (right)
    y3 = y2 + h2 + 64
    h3 = max(measure(Lw, *CONSOLES, fs=FS), measure(Rw, *AWS, fs=FS))
    c.box(L, y3, Lw, h3, *CONSOLES, tone="gold", fs=FS)
    c.box(R, y3, Rw, h3, *AWS, tone="acc", fs=FS, dash=True)
    c.arrow([(L + Lw / 2, y2 + h2), (L + Lw / 2, y3)], "build in ↓  log out ↑", both=True, lanchor="start", loff=(8, 4))
    m3 = y3 + h3 / 2
    c.arrow([(L + Lw, m3), (R, m3)], "online race: endpoints, punch, relay fallback (UDP 27200)|consoles then race in lockstep, ~130 bytes per tick per peer",
            tone="acc", dash=True, loff=(0, -20))
    # the human plays the consoles: a lane down the left edge
    c.arrow([(L + 14, y1 + h1), (L + 14, y2)], "", tone="gold")
    c.text(L + 22, y1 + h1 + 30, "carries the SD card,", fs=11.5, fill="var(--ink2)", halo=True)
    c.text(L + 22, y1 + h1 + 45, "presses Y on the netloader", fs=11.5, fill="var(--ink2)", halo=True)
    c.h = y3 + h3 + 16
    return c.svg()


def _narrow():
    return tree([
        (0, *HUMAN, "gold", ""),
        (0, *ORCH, "acc", "goals and verdicts ↓  reports ↑"),
        (1, *AGENTS, "neu", "briefs ↓  commits ↑"),
        (2, *EMU, "good", "smoke, replay parity, shots"),
        (1, *FLOWS, "neu", "fan-out scripts"),
        (1, *REVIEW, "neu", "audit after merges"),
        (1, *BUILD, "warn", "merge → one build per console"),
        (2, *CONSOLES, "gold", "SD / netloader in, log out"),
        (3, *AWS, "acc", "online races only"),
    ], label="Topology of the agent fleet and its hardware loop")


def figure_html():
    return fig(_wide(), _narrow(),
               "<b>How it actually ran.</b> One human, one orchestrator, a fleet that never talked to the human. "
               "The hardware lane changed over time: SD card only through the first phase and the start of the second, then netloader push, in-game reload and a "
               "live log server, and from 10-21 one combined test build per console instead of one card per feature. The "
               "emulator farm went from one locked Azahar to a slot pool of parallel instances. The rendezvous server "
               "(dashed) exists only for v1.1.0 online play.")
