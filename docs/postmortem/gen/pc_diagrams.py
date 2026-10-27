"""Small inline-SVG diagram kit for the native, agentic and frontier pages (no libraries).

Every diagram is drawn twice: a wide layout for desktop and print, and a narrow, vertically stacked
layout for phones, toggled by CSS (dual()). Colours come from the site's CSS variables, so both
light and dark themes work, and the print build (light theme) gets the wide layout.
"""
from framework import esc

TONES = {
    # tone: (fill, stroke, title ink)
    "neu": ("var(--bg2)", "var(--ink3)", "var(--ink)"),
    "mute": ("var(--bg3)", "var(--line)", "var(--ink2)"),
    "acc": ("var(--blue-soft)", "var(--blue)", "var(--blue-ink)"),
    "good": ("var(--good-soft)", "var(--good)", "var(--good)"),
    "warn": ("var(--warn-soft)", "var(--warn)", "var(--warn)"),
    "bad": ("var(--bad-soft)", "var(--bad)", "var(--bad)"),
    "gold": ("var(--gold-soft)", "var(--gold)", "var(--ink)"),
}

STYLE = """<style>
.pc-dual .pc-n { display:none; }
@media (max-width: 640px) { .pc-dual .pc-w { display:none; } .pc-dual .pc-n { display:block; } figure.pc-fig { padding:10px 8px 10px; } }
@media print { .pc-dual .pc-w { display:block !important; } .pc-dual .pc-n { display:none !important; } }
.pc-dual svg text { font-family:var(--sans); }
.pc-dual svg .mono { font-family:var(--mono); }
.pc-steps { display:grid; grid-template-columns:repeat(3, minmax(0,1fr)); gap:10px 20px; margin:10px 0 4px; }
.pc-steps > div { background:transparent; border-top:2px solid var(--line); border-radius:0; padding:8px 0 4px; font-size:13.5px; line-height:1.45; color:var(--ink2); }
.pc-steps b { display:block; font-family:var(--sans); font-size:10.5px; letter-spacing:.12em; text-transform:uppercase; color:var(--accent-ink); font-weight:700; margin-bottom:2px; }
.pc-steps > div.now { border-top-color:var(--accent); }
@media (max-width: 640px) { .pc-steps { grid-template-columns:1fr; } }
.pc-case td:first-child { font-weight:600; min-width:9em; }
.pc-case td { font-size:13.5px; }
@media (max-width: 640px) {
  .pc-case thead { display:none; }
  .pc-case table, .pc-case tbody, .pc-case tr, .pc-case td { display:block; width:auto; }
  .pc-case tr { border-bottom:1px solid var(--line); padding:8px 0; }
  .pc-case td { border:0; padding:3px 0; text-align:left !important; white-space:normal !important; }
  .pc-case td:first-child { font-size:15px; min-width:0; }
  .pc-case td:empty { display:none; }
  .pc-case td:not(:first-child)::before { content:attr(data-l); display:block; font-size:10.5px; letter-spacing:.06em; text-transform:uppercase; color:var(--ink3); }
}
@media print { .pc-steps > div { break-inside:avoid; } }
</style>"""


def wrap(text, width_px, fs):
    """Greedy word wrap using an average glyph width for Helvetica Neue / Arial."""
    per_line = max(6, int(width_px / (fs * 0.56)))
    lines, cur = [], ""
    for word in text.split():
        trial = f"{cur} {word}".strip()
        if len(trial) <= per_line or not cur:
            cur = trial
        else:
            lines.append(cur)
            cur = word
    if cur:
        lines.append(cur)
    return lines


def measure(w, title="", body="", fs=13, tfs=None, tag=""):
    """Height a box() of width w needs for this text (same arithmetic as box())."""
    tfs = tfs or fs + 1
    pad = 11
    h = pad + tfs * 0.9
    if title:
        h += len(wrap(title, w - 2 * pad - (40 if tag else 0), tfs * 1.04)) * tfs * 1.25 + 3
    if body:
        h += sum(len(wrap(p, w - 2 * pad, fs)) for p in body.split("\n")) * fs * 1.32
    return h + pad - fs * 0.6


class Canvas:
    def __init__(self, w, h, label):
        self.w, self.h, self.label = w, h, label
        self.under, self.parts = [], []

    def text(self, x, y, s, fs=13, fill="var(--ink2)", anchor="start", weight=400, cls="", italic=False, halo=False):
        c = f' class="{cls}"' if cls else ""
        if halo:
            c += ' stroke="var(--bg2)" stroke-width="4" stroke-linejoin="round" paint-order="stroke"'
        it = ' font-style="italic"' if italic else ""
        self.parts.append(f'<text x="{x:.1f}" y="{y:.1f}" font-size="{fs}" fill="{fill}" text-anchor="{anchor}" '
                          f'font-weight="{weight}"{it}{c}>{esc(s)}</text>')

    def lines(self, x, y, lines, fs=13, lh=None, **kw):
        lh = lh or fs * 1.32
        for i, ln in enumerate(lines):
            self.text(x, y + i * lh, ln, fs=fs, **kw)
        return y + len(lines) * lh

    def box(self, x, y, w, h, title="", body="", tone="neu", fs=13, tfs=None, align="start", dash=False, tag=""):
        """A rounded box with a bold title line and auto-wrapped body text. Returns (x, y, w, h)."""
        fill, stroke, tink = TONES[tone]
        tfs = tfs or fs + 1
        d = ' stroke-dasharray="5 4"' if dash else ""
        self.under.append(f'<rect x="{x:.1f}" y="{y:.1f}" width="{w:.1f}" height="{h:.1f}" rx="7" fill="{fill}" '
                          f'stroke="{stroke}" stroke-width="1.4"{d}/>')
        pad = 11
        tx = x + w / 2 if align == "middle" else x + pad
        cy = y + pad + tfs * 0.9
        if tag:
            self.text(x + w - pad, y + pad + 9, tag, fs=10.5, fill=stroke, anchor="end", weight=600, cls="mono")
        if title:
            for ln in wrap(title, w - 2 * pad - (40 if tag else 0), tfs * 1.04):
                self.text(tx, cy, ln, fs=tfs, fill=tink, anchor=align, weight=600)
                cy += tfs * 1.25
            cy += 3
        if body:
            for para in body.split("\n"):
                for ln in wrap(para, w - 2 * pad, fs):
                    self.text(tx, cy, ln, fs=fs, fill="var(--ink2)", anchor=align)
                    cy += fs * 1.32
        return (x, y, w, h)

    def band(self, x, y, w, h, label="", tone="mute"):
        """A background grouping band with a small caps label in its top-left corner."""
        fill, stroke, _ = TONES[tone]
        self.under.insert(0, f'<rect x="{x:.1f}" y="{y:.1f}" width="{w:.1f}" height="{h:.1f}" rx="10" fill="{fill}" '
                             f'stroke="{stroke}" stroke-width="1" opacity=".75"/>')
        if label:
            self.text(x + 12, y + 18, label.upper(), fs=11, fill="var(--ink3)", weight=600, cls="mono")

    def arrow(self, pts, label="", tone="ink", both=False, dash=False, lfs=11.5, lpos=0.5, loff=(0, -6), lanchor="middle", head=True):
        col = {"ink": "var(--ink3)", "acc": "var(--blue)", "good": "var(--good)", "bad": "var(--bad)",
               "warn": "var(--warn)", "gold": "var(--gold)"}[tone]
        d = ' stroke-dasharray="5 4"' if dash else ""
        path = " ".join(f"{'M' if i == 0 else 'L'}{x:.1f},{y:.1f}" for i, (x, y) in enumerate(pts))
        self.parts.insert(0, f'<path d="{path}" fill="none" stroke="{col}" stroke-width="1.6" stroke-linejoin="round" stroke-linecap="round"{d}/>')
        if head:
            self._head(pts[-2], pts[-1], col)
        if both:
            self._head(pts[1], pts[0], col)
        if label:
            # label sits on the longest segment
            segs = [(pts[i], pts[i + 1]) for i in range(len(pts) - 1)]
            (ax, ay), (bx, by) = max(segs, key=lambda s: abs(s[1][0] - s[0][0]) + abs(s[1][1] - s[0][1]))
            lx, ly = ax + (bx - ax) * lpos + loff[0], ay + (by - ay) * lpos + loff[1]
            for i, ln in enumerate(label.split("|")):
                self.text(lx, ly + i * lfs * 1.25, ln, fs=lfs, fill=col if tone != "ink" else "var(--ink2)", anchor=lanchor, halo=True)

    def _head(self, a, b, col):
        import math
        (ax, ay), (bx, by) = a, b
        ang = math.atan2(by - ay, bx - ax)
        s = 8
        p1 = (bx - s * math.cos(ang - 0.42), by - s * math.sin(ang - 0.42))
        p2 = (bx - s * math.cos(ang + 0.42), by - s * math.sin(ang + 0.42))
        self.parts.append(f'<polygon points="{bx:.1f},{by:.1f} {p1[0]:.1f},{p1[1]:.1f} {p2[0]:.1f},{p2[1]:.1f}" fill="{col}"/>')

    def badge(self, x, y, text, tone="acc", fs=11):
        """A version label: a small filled pill with monospace caps, top-left of a diagram."""
        fill, stroke, tink = TONES[tone]
        # generous width (some renderers lay out the mono caps at ~0.8 em); the label is centred in the pill
        w = len(text) * fs * 0.82 + 20
        self.under.append(f'<rect x="{x:.1f}" y="{y:.1f}" width="{w:.1f}" height="{fs + 11:.1f}" rx="{(fs + 11) / 2:.1f}" '
                          f'fill="{fill}" stroke="{stroke}" stroke-width="1.2"/>')
        self.text(x + w / 2, y + fs + 4, text.upper(), fs=fs, fill=tink, weight=600, cls="mono", anchor="middle")
        return w

    def svg(self):
        return (f'<svg viewBox="0 0 {self.w} {self.h}" role="img" aria-label="{esc(self.label)}">'
                + "".join(self.under) + "".join(self.parts) + '</svg>')


def dual(wide, narrow):
    return f'<div class="pc-dual"><div class="pc-w">{wide}</div><div class="pc-n">{narrow}</div></div>'


def fig(wide, narrow, caption):
    return f'<figure class="pc-fig">{dual(wide, narrow)}<figcaption>{caption}</figcaption></figure>'


def steps(items):
    """A three-cell journey line: [(label, text), ...]; the last cell is highlighted as 'now'."""
    cells = []
    for i, (lab, txt) in enumerate(items):
        now = ' class="now"' if i == len(items) - 1 else ""
        cells.append(f'<div{now}><b>{esc(lab)}</b>{txt}</div>')
    return '<div class="pc-steps">' + "".join(cells) + '</div>'


def tree(nodes, w=320, label="", fs=13, indent=18, gap=26, chain=False, badge=None):
    """Narrow (phone) layout: a vertical tree. nodes: [(depth, title, body, tone, edge_label), ...].

    Each node hangs off the nearest earlier node one level shallower; edge_label is printed in the gap
    above the node, next to the connector, so a phone reads the diagram top to bottom.
    """
    placed = []
    y = 38 if badge else 8
    for depth, title, body, tone, edge in nodes:
        x = 8 + depth * indent
        bw = w - x - 8
        if placed:
            y += gap if not edge else gap + 4
        h = measure(bw, title, body, fs=fs)
        placed.append((depth, x, y, bw, h, title, body, tone, edge))
        y += h
    c = Canvas(w, y + 8, label)
    if badge:
        c.badge(8, 6, badge)
    for i, (depth, x, y0, bw, h, title, body, tone, edge) in enumerate(placed):
        c.box(x, y0, bw, h, title, body, tone=tone, fs=fs, dash=(tone == "mute"))
        parent = next((p for p in reversed(placed[:i]) if p[0] == depth - 1), None) if depth else (placed[i - 1] if i else None)
        if parent is None:
            continue
        pd, px, py, pw, ph = parent[:5]
        if depth == 0:
            sx = x + 30
            c.arrow([(sx, py + ph), (sx, y0)], both=not chain)
            if edge:
                c.text(sx + 10, y0 - 9, edge, fs=11, fill="var(--ink2)", halo=True)
        else:
            sx = px + 12
            c.arrow([(sx, py + ph), (sx, y0 + 16), (x, y0 + 16)])
            if edge:
                c.text(x + 2, y0 - 8, edge, fs=11, fill="var(--ink2)", halo=True)
    return c.svg()


def pipeline(stages, w=980, fs=12.5, gap=34, label="", arrow_labels=None, badge=None):
    """Wide layout: boxes left to right with arrows; stages = [(title, body, tone), ...]. Returns (canvas, boxes).

    badge: an optional version label drawn above the first box (the row moves down to make room)."""
    n = len(stages)
    bw = (w - 12 - gap * (n - 1)) / n
    h = max(measure(bw, t, b, fs=fs) for t, b, _ in stages)
    y0 = 40 if badge else 8
    c = Canvas(w, y0 + h + 8, label)
    if badge:
        c.badge(6, 6, badge)
    boxes = []
    for i, (t, b, tone) in enumerate(stages):
        x = 6 + i * (bw + gap)
        c.box(x, y0, bw, h, t, b, tone=tone, fs=fs, dash=(tone == "mute"))
        boxes.append((x, y0, bw, h))
        if i:
            lab = (arrow_labels or [""] * n)[i] if arrow_labels else ""
            c.arrow([(x - gap + 3, y0 + h / 2), (x - 3, y0 + h / 2)], lab)
    return c, boxes
