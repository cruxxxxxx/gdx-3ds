"""Diagrams and the case-by-case table for native.html: the convention, where we were, where we ended up.

Facts are cited to the tree at v1.1.0 work (HEAD of card-4):
  convention / v0.9.0 path ...... docs/NATIVE-RENDERER.md §1, docs/ARCHITECTURE.md, port/n64_gfx_bridge.cpp
  per-draw and I-cache numbers .. scratchpad cpu-profile-answer.md (Old 3DS hardware probes: a clean
                                  C3D_DrawArrays 2,926 cycles = 10.9 us; 43.6% of render cycles in
                                  I-fetch stalls before the NRR; first Old 3DS boot 96 ms frame, GPU 3.6 ms)
  categories .................... port/3ds/gfx/nrr/*.h, gdx3ds_track_resident.h, gdx3ds_hud_capture.c,
                                  gdx3ds_bottomhud.c, port/3ds/gfx/STEREO.md, gdx3ds_preset_n60.c
  per-category ms ............... o3ds-data.json "cats" (Old 3DS, interpreter + early levers -> card 3)

Each diagram carries its own version badge, and every box uses one body size (FS) so the three read as a set.
"""
import json
import os

from pc_diagrams import Canvas, measure, tree, fig, pipeline

FS = 12.5
HERE = os.path.dirname(os.path.abspath(__file__))


# ---------- 1. the convention ----------

CONV = [
    ("The game", "Builds an F3DEX2 display list every frame, as it did for the N64's RSP.", "neu"),
    ("A CPU interpreter", "libultraship's Fast3D walks every command: matrices, transform and lighting, clipping, "
                          "the combiner turned into a shader.", "acc"),
    ("A graphics API", "OpenGL, Metal or DirectX calls, re-emitted from scratch each frame.", "neu"),
    ("A desktop GPU", "Fast enough that nobody minds the CPU doing the N64's geometry work.", "neu"),
]


def convention():
    c, _ = pipeline(CONV, fs=FS, label="How N64 PC ports render", gap=40,
                    badge="N64 PC ports · SoH, 2Ship, Starship, G-Diffuser PC")
    narrow = tree([(0, t, b, tone, "") for t, b, tone in CONV], label="How N64 PC ports render", chain=True,
                  badge="N64 PC ports")
    return fig(c.svg(), narrow,
               "The display list is rebuilt and re-walked on the CPU every frame: portable, exact, and cheap on a desktop CPU.")


# ---------- 2. where we were (v0.9.0, the New 3DS chapter) ----------

WAS = [
    ("The game", "Builds 32-bit F3DEX2 words every frame.", "neu"),
    ("The bridge", "Widens each command to the interpreter's 64-bit form (12,353 lines).", "warn"),
    ("libultraship's interpreter", "Every vertex transformed and lit on the ARM11 (10,466 lines).", "warn"),
    ("citro3d backend", "One C3D_DrawArrays per batch, with state and uniform upload.", "warn"),
    ("PICA200", "Mostly waiting for the CPU.", "neu"),
]
WAS_COST = [
    (2, "Instruction cache", "~219 KB of hot code against a 16 KB L1-I: 43.6% of Old 3DS render cycles stalled on fetch."),
    (3, "Per-draw cost", "10.9 µs per clean citro3d draw on Old 3DS; 72–98 µs per game draw through the interpreter."),
    (4, "CPU-bound", "First Old 3DS boot: a 96 ms frame, GPU busy 3.6 ms of it."),
]


def where_we_were():
    c, boxes = pipeline(WAS, w=980, fs=FS, gap=30, label="The v0.9.0 render path on 3DS", badge="v0.9.0 · 09-04")
    top = boxes[0][1] + boxes[0][3]
    ch = max(measure(boxes[bi][2], t, b, fs=FS) for bi, t, b in WAS_COST)
    y = top + 38
    c.text(boxes[1][0] + boxes[1][2], y + 16, "MEASURED COST", fs=11, fill="var(--bad)", anchor="end", weight=600, cls="mono")
    for bi, t, b in WAS_COST:
        bx, by, bw, bh = boxes[bi]
        c.box(bx, y, bw, ch, t, b, tone="bad", fs=FS)
        c.arrow([(bx + bw / 2, top + 3), (bx + bw / 2, y - 3)], tone="bad", dash=True)
    c.h = y + ch + 8
    nodes = []
    cost_by_box = {bi: (t, b) for bi, t, b in WAS_COST}
    for i, (t, b, tone) in enumerate(WAS):
        nodes.append((0, t, b, tone, ""))
        if i in cost_by_box:
            nodes.append((1, *cost_by_box[i], "bad", ""))
    narrow = tree(nodes, label="The v0.9.0 render path on 3DS", chain=True, badge="v0.9.0 · 09-04")
    return fig(c.svg(), narrow,
               "The convention, unchanged, with citro3d as the API. Line counts are the 09-07 snapshot; costs are Old 3DS "
               "hardware probes from the fork. The New 3DS reached a 59.6 fps median only by running this a frame ahead on a "
               "second core.")


# ---------- 3. where we ended up (v1.1.0 defaults) ----------

GAME = ("The game", "Still runs its own draw code every frame. Patches swap a race's heavy sections for a marker "
        "plus a per-tick snapshot (matrices, LODs, colours).")
NRR = [
    ("Track", "Baked once per course"),
    ("Machines", "Recorded once, replayed"),
    ("Sky", "State learned once"),
    ("HUD", "Memoised programs"),
    ("FX · shadows · scenery", "Sprite batches"),
    ("Decorations", "Recorded at first sight"),
]
MENUS = ("Menus, previews, the walk between markers", "Captured once, replayed as compiled programs (ni_*).")
WALK = ("Everything else", "Our own F3DEX2 walker: credits, editors, transitions, frame setup.")
LUS = ("libultraship's interpreter", "Only with [perf] preset = off.")
GPU = ("PICA200 command buffer", "Precomputed register words written straight in. Track and machine vertices "
       "transformed on the GPU.")
STEREO = ("3D: one submission", "Body built once; the GPU draws it for the left eye, jumps, and draws it again for the right.")
DASH = ("Bottom screen", "DASH and bottom menus: the CPU paints a 320×240 buffer and copies changed columns. No GPU.")


def where_we_ended_up():
    c = Canvas(980, 10, "The shipped native renderer")
    c.badge(6, 6, "v1.1.0 defaults · Old and New 3DS")
    gx, gw = 6, 176
    mx, mw = 236, 470
    rx, rw = 750, 224
    y0, pad = 42, 12
    # NRR band: 3 x 2 grid of category chips
    cwid = (mw - 4 * pad) / 3
    chh = max(measure(cwid, t, b, fs=FS) for t, b in NRR)
    band_h = 34 + 2 * chh + 10 + pad
    c.band(mx, y0, mw, band_h, "native race renderer (NRR)", tone="acc")
    for i, (t, b) in enumerate(NRR):
        col, row = i % 3, i // 3
        c.box(mx + pad + col * (cwid + pad), y0 + 34 + row * (chh + 10), cwid, chh, t, b, tone="acc", fs=FS)
    nrr_mid = y0 + band_h / 2
    # the other lanes, under the band
    y = y0 + band_h + 18
    lanes = []
    for (t, b), tone in ((MENUS, "good"), (WALK, "neu"), (LUS, "mute")):
        h = measure(mw, t, b, fs=FS)
        c.box(mx, y, mw, h, t, b, tone=tone, fs=FS, dash=(tone == "mute"))
        lanes.append((y, h))
        y += h + 14
    bottom_lanes = y - 14
    # the game spans every lane it feeds
    c.box(gx, y0, gw, bottom_lanes - y0, *GAME, fs=FS)
    gr = gx + gw
    c.arrow([(gr + 3, nrr_mid), (mx - 3, nrr_mid)], "race", loff=(0, -7))
    for i, (ly, lh) in enumerate(lanes):
        c.arrow([(gr + 3, ly + lh / 2), (mx - 3, ly + lh / 2)], dash=(i == 2))
    # GPU column: everything that draws on the top screen converges on one command buffer
    gh = measure(rw, *GPU, fs=FS)
    sh = measure(rw, *STEREO, fs=FS)
    c.box(rx, y0, rw, gh, *GPU, tone="good", fs=FS)
    c.box(rx, y0 + gh + 32, rw, sh, *STEREO, tone="gold", fs=FS)
    c.arrow([(rx + rw / 2, y0 + gh + 3), (rx + rw / 2, y0 + gh + 29)], "3D on", lanchor="start", loff=(8, 4))
    mr = mx + mw
    jx = mr + 22
    gmid = y0 + gh / 2
    c.arrow([(mr + 3, nrr_mid), (jx, nrr_mid), (jx, gmid), (rx - 3, gmid)])
    for ly, lh in lanes[:2]:
        c.arrow([(mr + 3, ly + lh / 2), (jx, ly + lh / 2), (jx, nrr_mid)], head=False)
    # bottom screen: off the GPU entirely
    dy = bottom_lanes + 22
    dh = measure(mw, *DASH, fs=FS)
    c.box(mx, dy, mw, dh, *DASH, tone="warn", fs=FS)
    c.arrow([(gx + gw / 2, bottom_lanes + 3), (gx + gw / 2, dy + dh / 2), (mx - 3, dy + dh / 2)])
    c.h = dy + dh + 8
    narrow = tree([
        (0, *GAME, "neu", ""),
        (1, "Native race renderer (NRR)", "; ".join(f"{t}: {b.lower()}" for t, b in NRR) + ".", "acc", "race sections"),
        (1, *MENUS, "good", "menus"),
        (1, *WALK, "neu", "the rest"),
        (1, *LUS, "mute", "escape hatch"),
        (1, *DASH, "warn", "bottom screen"),
        (0, *GPU, "good", ""),
        (0, *STEREO, "gold", ""),
    ], label="The shipped native renderer", chain=True, badge="v1.1.0 defaults")
    return fig(c.svg(), narrow,
               "The game still decides what is drawn, so the pictures match. What changed is who turns that into GPU work: "
               "for what a race spends its frame on, a small path per category writes the PICA200's own command words.")


# ---------- 4. the case-by-case table ----------

def _cats():
    try:
        return json.load(open(os.path.join(HERE, "o3ds-data.json")))["cats"]
    except (OSError, KeyError, ValueError):
        return None


def _ms(cats, *names):
    if not cats:
        return ""
    idx = [cats["names"].index(n) for n in names if n in cats["names"]]
    if not idx:
        return ""
    a = sum(cats["rel4"][i] for i in idx)
    b = sum(cats["card3"][i] for i in idx)
    return f'{a:.2f} → <b>{b:.2f}</b>'


def case_table():
    k = _cats()
    rows = [
        ["Machines",
         "All 30 racers rebuilt as display-list commands and transformed on the CPU every frame.",
         "A machine's first ordinary draw <i>is</i> its recording: from the next frame it is a retained mesh drawn as one GPU-"
         "transformed draw per material run, fed by a snapshot of its matrix, LOD and colours. First sight, crashed machines "
         "and time-attack ghosts stay on the ordinary path.",
         _ms(k, "mach+mesh")],
        ["Track",
         "The course tessellator re-wrote every ring's vertices each frame; the interpreter transformed them again.",
         "Every distinct ring is baked once per course load into a GPU buffer, inside one arena reserved at boot (no heap "
         "fragmentation across loads). The game's own chunk cull and walk still choose what is drawn; each frame only "
         "writes indices, from 18 index templates. The GPU adds an exact integer eye offset.",
         _ms(k, "trk")],
        ["Sky and backdrop",
         "Walked and transformed like everything else.",
         "The backdrop's state blocks are learned once; each frame the CPU transforms a handful of snapshot vertices and "
         "draws one packed draw per state block.",
         _ms(k, "sky")],
        ["HUD (top screen)",
         "One draw per texture rectangle, colour changes splitting batches.",
         "The game's HUD code still runs; its commands are compacted behind markers and replayed as memoised, compiled "
         "register programs. About 12 of 14 runs are byte-identical from frame to frame.",
         _ms(k, "hud")],
        ["Effects, shadows, scenery",
         "Sprite by sprite through the interpreter.",
         "One shared engine: state learned once, then the per-tick snapshot drawn as sprite batches. Course decorations "
         "use the machines' record-at-first-sight pattern.",
         _ms(k, "fx", "shd", "dress")],
        ["Fog",
         "The PICA fog unit, plus a combiner stage for the game's shade-alpha fog.",
         "The NRR switches the fog unit off and blends fog in a combiner stage with the colour written as a constant; a "
         "track run that needs fog it cannot express falls back.", ""],
        ["Menus",
         "Every non-race screen walked by the interpreter.",
         "Since v1.1.0, menus, course previews and machine select are captured once and replayed as compiled programs "
         "(<code>ni_*</code>). The spiral transitions and one outline pass still go through the walker.", ""],
        ["Bottom screen",
         "A debug console.",
         "DASH and the bottom-screen menus never touch the GPU: the CPU paints a 320×240 buffer and copies only the changed "
         "columns.", ""],
        ["Stereo 3D",
         "Each draw issued twice, once per eye.",
         "The frame body is emitted once; a per-draw uniform carries the eye shift. Since v1.1.0 both eyes go in one GPU "
         "submission: the GPU runs the body, jumps to a right-eye prologue, runs it again. Old 3DS 3D menu 40.2 → 44.6 fps.", ""],
        ["Still on an interpreter",
         "Everything.",
         "Our own F3DEX2 walker (Tier 1) handles the frame setup between markers, credits, editors, demos and transitions. "
         "libultraship's interpreter runs only under <code>preset = off</code>.", _ms(k, "misc")],
    ]
    heads = ["Category", "Before (v0.9.0)", "Shipped path (v1.1.0 defaults)", "Old 3DS ms"]
    th = "".join(f'<th class="{"num" if i == 3 else ""}">{h}</th>' for i, h in enumerate(heads))
    trs = "".join("<tr>" + "".join(
        f'<td data-l="{heads[i]}" class="{"num" if i == 3 else ""}">{cell}</td>' for i, cell in enumerate(r)
    ) + "</tr>" for r in rows)
    return f'<div class="tbl pc-case"><table><thead><tr>{th}</tr></thead><tbody>{trs}</tbody></table></div>'
