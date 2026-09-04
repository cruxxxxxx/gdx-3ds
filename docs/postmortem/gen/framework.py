"""Shared layout, style and SVG chart helpers for the G-Diffuser 3DS post-mortem pages."""
import html

PAGES = [
    ("index.html", "Overview", "What was built, in one screen"),
    ("author.html", "Author's notes", "The one page written by a human"),
    ("timeline.html", "Every step", "The 24-day chronology"),
    ("road-to-60.html", "Road to 60", "The performance campaign, round by round"),
    ("difficulties.html", "Difficulties", "What fought back, and how it was beaten"),
    ("frontier.html", "Frontier", "Why this is innovative, bold, and new"),
    ("agentic.html", "Agentic workflow", "How the fleet was run, and how the ship was steered"),
    ("cost.html", "Cost", "Tokens, dollars, subagents"),
]

FONTS = ('<link rel="preconnect" href="https://fonts.googleapis.com">'
         '<link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=Barlow+Condensed:wght@500;600;700'
         '&family=IBM+Plex+Sans:ital,wght@0,400;0,500;0,600;1,400&family=IBM+Plex+Mono:wght@400;500&display=swap">')

CSS = r"""
:root {
  --bg:#eef1f6; --bg2:#ffffff; --bg3:#e2e7ef; --ink:#17202e; --ink2:#48546a; --ink3:#7a8598;
  --line:#cfd6e2; --accent:#2457ff; --accent-ink:#1a3fc4; --accent-soft:#dbe3ff; --gold:#e0a71a; --gold-soft:#fbefc9;
  --good:#1f8f4e; --warn:#c77d0a; --bad:#c9302c; --bad-soft:#f8dcdb; --good-soft:#d9f1e3; --warn-soft:#fbe9cc;
  --c1:#2457ff; --c2:#e0a71a; --c3:#7a3be0; --c4:#1f8f4e; --c5:#c9302c; --c6:#0e8fa3; --c7:#8a6d3b; --c8:#5c6577;
  --shadow:0 1px 2px rgba(23,32,46,.08), 0 8px 24px -12px rgba(23,32,46,.25);
}
@media (prefers-color-scheme: dark) {
  :root:not([data-theme="light"]) {
    --bg:#0f141d; --bg2:#161d29; --bg3:#1f2836; --ink:#e7ecf4; --ink2:#b3bccb; --ink3:#7f8a9c;
    --line:#2b3547; --accent:#6d8dff; --accent-ink:#9fb3ff; --accent-soft:#1d2a55; --gold:#f0c04a; --gold-soft:#3a2f10;
    --good:#4cc37c; --warn:#f0a83a; --bad:#f06a66; --bad-soft:#4a1f1e; --good-soft:#153826; --warn-soft:#3f2c0f;
    --c1:#6d8dff; --c2:#f0c04a; --c3:#b08cff; --c4:#4cc37c; --c5:#f06a66; --c6:#3fc3d8; --c7:#c9a26b; --c8:#8f9ab0;
    --shadow:0 1px 2px rgba(0,0,0,.4), 0 8px 24px -12px rgba(0,0,0,.6);
  }
}
:root[data-theme="dark"] {
  --bg:#0f141d; --bg2:#161d29; --bg3:#1f2836; --ink:#e7ecf4; --ink2:#b3bccb; --ink3:#7f8a9c;
  --line:#2b3547; --accent:#6d8dff; --accent-ink:#9fb3ff; --accent-soft:#1d2a55; --gold:#f0c04a; --gold-soft:#3a2f10;
  --good:#4cc37c; --warn:#f0a83a; --bad:#f06a66; --bad-soft:#4a1f1e; --good-soft:#153826; --warn-soft:#3f2c0f;
  --c1:#6d8dff; --c2:#f0c04a; --c3:#b08cff; --c4:#4cc37c; --c5:#f06a66; --c6:#3fc3d8; --c7:#c9a26b; --c8:#8f9ab0;
  --shadow:0 1px 2px rgba(0,0,0,.4), 0 8px 24px -12px rgba(0,0,0,.6);
}
* { box-sizing:border-box; }
html { color-scheme: light dark; }
body { margin:0; background:var(--bg); color:var(--ink); font-family:"IBM Plex Sans", system-ui, -apple-system, "Segoe UI", sans-serif; font-size:16px; line-height:1.55; }
a { color:var(--accent-ink); text-decoration:none; } a:hover { text-decoration:underline; }
code, pre, .mono { font-family:"IBM Plex Mono", ui-monospace, Menlo, Consolas, monospace; font-size:.92em; }
pre { background:var(--bg3); padding:12px 14px; border-radius:6px; overflow-x:auto; line-height:1.45; }
code { background:var(--bg3); padding:1px 5px; border-radius:4px; }
pre code { background:none; padding:0; }
.shell { display:grid; grid-template-columns:230px minmax(0,1fr); min-height:100vh; }
nav.rail { border-right:1px solid var(--line); padding:26px 18px; position:sticky; top:0; height:100vh; overflow:auto; background:var(--bg2); }
nav.rail .brand { font-family:"Barlow Condensed", "Arial Narrow", sans-serif; font-weight:700; font-size:22px; letter-spacing:.02em; text-transform:uppercase; line-height:1.05; margin-bottom:4px; }
nav.rail .sub { color:var(--ink3); font-size:12.5px; margin-bottom:22px; }
nav.rail ol { list-style:none; padding:0; margin:0; counter-reset:p; }
nav.rail li { margin:0 0 6px; }
nav.rail li a { display:block; padding:7px 10px; border-radius:6px; color:var(--ink2); }
nav.rail li a b { display:block; font-weight:600; color:var(--ink); font-size:14.5px; }
nav.rail li a span { font-size:12px; color:var(--ink3); }
nav.rail li a.on { background:var(--accent-soft); } nav.rail li a.on b { color:var(--accent-ink); }
nav.rail li a:hover { text-decoration:none; background:var(--bg3); }
nav.rail .foot { margin-top:26px; font-size:11.5px; color:var(--ink3); line-height:1.5; }
main { padding:40px 56px 80px; max-width:1180px; }
.eyebrow { font-family:"IBM Plex Mono", monospace; font-size:12px; letter-spacing:.12em; text-transform:uppercase; color:var(--accent-ink); margin:0 0 8px; }
h1 { font-family:"Barlow Condensed", "Arial Narrow", sans-serif; font-weight:700; font-size:54px; line-height:.98; letter-spacing:.005em; margin:0 0 14px; text-wrap:balance; text-transform:uppercase; }
h2 { font-family:"Barlow Condensed", "Arial Narrow", sans-serif; font-weight:600; font-size:31px; line-height:1.05; margin:46px 0 12px; text-wrap:balance; text-transform:uppercase; letter-spacing:.01em; }
h3 { font-size:17.5px; font-weight:600; margin:28px 0 8px; }
p, li { max-width:72ch; }
.lede { font-size:19px; color:var(--ink2); max-width:66ch; margin:0 0 26px; }
.tiles { display:grid; grid-template-columns:repeat(auto-fit, minmax(170px,1fr)); gap:12px; margin:22px 0 10px; }
.tile { background:var(--bg2); border:1px solid var(--line); border-radius:8px; padding:14px 16px 12px; }
.tile .n { font-family:"Barlow Condensed", sans-serif; font-weight:700; font-size:34px; overflow-wrap:anywhere; line-height:1; letter-spacing:.005em; font-variant-numeric:tabular-nums; }
.tile .n small { font-size:19px; color:var(--ink3); font-weight:600; margin-left:2px; }
.tile .l { font-size:12.5px; color:var(--ink2); margin-top:6px; line-height:1.35; }
.tile.hot .n { color:var(--accent-ink); }
figure { margin:22px 0 30px; background:var(--bg2); border:1px solid var(--line); border-radius:8px; padding:16px 18px 12px; box-shadow:var(--shadow); }
figure svg { width:100%; height:auto; display:block; }
figcaption { font-size:13px; color:var(--ink2); margin-top:10px; max-width:none; line-height:1.45; }
figcaption b { color:var(--ink); }
.legend { display:flex; flex-wrap:wrap; gap:6px 16px; font-size:12.5px; color:var(--ink2); margin:0 0 8px; }
.legend i { display:inline-block; width:10px; height:10px; border-radius:2px; margin-right:6px; vertical-align:-1px; }
.tbl { overflow-x:auto; margin:16px 0 26px; }
table { border-collapse:collapse; width:100%; font-size:14px; }
th, td { text-align:left; padding:8px 10px; border-bottom:1px solid var(--line); vertical-align:top; }
th { font-size:12px; letter-spacing:.06em; text-transform:uppercase; color:var(--ink2); font-weight:600; }
td.num, th.num { text-align:right; font-variant-numeric:tabular-nums; white-space:nowrap; }
tr:hover td { background:var(--bg3); }
.pill { display:inline-block; font-size:11.5px; font-weight:600; letter-spacing:.04em; padding:2px 8px; border-radius:999px; white-space:nowrap; }
.pill.good { background:var(--good-soft); color:var(--good); } .pill.bad { background:var(--bad-soft); color:var(--bad); }
.pill.warn { background:var(--warn-soft); color:var(--warn); } .pill.neu { background:var(--bg3); color:var(--ink2); }
.pill.acc { background:var(--accent-soft); color:var(--accent-ink); }
.timeline { border-left:2px solid var(--line); margin:20px 0 30px 8px; padding-left:22px; }
.ev { position:relative; margin:0 0 22px; }
.ev::before { content:""; position:absolute; left:-29px; top:8px; width:12px; height:12px; border-radius:50%; background:var(--bg); border:2px solid var(--accent); }
.ev.big::before { background:var(--accent); }
.ev.bad::before { border-color:var(--bad); } .ev.bad.big::before { background:var(--bad); }
.ev.gold::before { border-color:var(--gold); } .ev.gold.big::before { background:var(--gold); }
.ev .d { font-family:"IBM Plex Mono", monospace; font-size:12px; color:var(--ink3); letter-spacing:.04em; }
.ev .t { font-weight:600; font-size:16px; margin:1px 0 3px; }
.ev p { margin:0 0 4px; color:var(--ink2); font-size:14.5px; }
.q { border-left:3px solid var(--gold); padding:6px 14px; margin:14px 0; background:var(--gold-soft); border-radius:0 6px 6px 0; font-size:14.5px; max-width:72ch; }
.q .who { font-family:"IBM Plex Mono", monospace; font-size:11.5px; color:var(--ink3); letter-spacing:.06em; text-transform:uppercase; display:block; margin-bottom:2px; }
.author p { font-size:17px; line-height:1.65; max-width:760px; }
.author ol { max-width:760px; font-size:17px; line-height:1.65; }
.author hr { border:0; border-top:1px solid var(--line); margin:34px 0; max-width:760px; }
.note { background:var(--bg3); border-radius:6px; padding:10px 14px; font-size:14px; color:var(--ink2); max-width:78ch; margin:14px 0; }
.cols2 { display:grid; grid-template-columns:repeat(auto-fit, minmax(300px,1fr)); gap:14px 28px; }
.assume { border:1px dashed var(--line); border-radius:6px; padding:10px 14px; font-size:13.5px; color:var(--ink2); max-width:78ch; }
.next { margin-top:56px; padding-top:18px; border-top:1px solid var(--line); display:flex; justify-content:space-between; font-size:14px; }
@media (max-width: 860px) { .shell { grid-template-columns:1fr; } nav.rail { position:static; height:auto; border-right:0; border-bottom:1px solid var(--line); } main { padding:26px 20px 60px; } h1 { font-size:40px; } }
@media (prefers-reduced-motion: reduce) { * { transition:none !important; } }
"""


def esc(s):
    return html.escape(str(s), quote=True)


def nav(current):
    items = []
    for f, t, s in PAGES:
        on = ' class="on"' if f == current else ""
        items.append(f'<li><a href="{f}"{on}><b>{esc(t)}</b><span>{esc(s)}</span></a></li>')
    return ('<nav class="rail"><div class="brand">G-Diffuser<br>on New 3DS</div>'
            '<div class="sub">Post-mortem · F-Zero X, 2026-08-11 → 2026-09-03</div>'
            '<ol>' + "".join(items) + '</ol>'
            '<div class="foot">Sources: git history, docs/research, session transcripts, hardware logs. '
            '<a href="https://github.com/cruxxxxxx/gdx-3ds">Repository</a> · <a href="G-Diffuser-3DS-postmortem.pdf">PDF</a></div></nav>')


def page(current, title, body):
    idx = [f for f, _, _ in PAGES].index(current)
    prev_ = PAGES[idx - 1] if idx > 0 else None
    next_ = PAGES[idx + 1] if idx + 1 < len(PAGES) else None
    nx = ""
    if prev_ or next_:
        nx = '<div class="next">'
        nx += f'<a href="{prev_[0]}">← {esc(prev_[1])}</a>' if prev_ else '<span></span>'
        nx += f'<a href="{next_[0]}">{esc(next_[1])} →</a>' if next_ else '<span></span>'
        nx += '</div>'
    return (f'<!doctype html><html lang="en"><head><meta charset="utf-8">'
            f'<meta name="viewport" content="width=device-width, initial-scale=1">'
            f'<title>{esc(title)}</title>{FONTS}<style>{CSS}</style></head>'
            f'<body><div class="shell">{nav(current)}<main>{body}{nx}</main></div></body></html>')


# ---------- SVG charts (no libraries; theme colors via CSS variables) ----------

def _fmt(v, dec=0):
    if v is None:
        return ""
    if isinstance(v, float) and dec:
        return f"{v:.{dec}f}"
    if abs(v) >= 1000:
        return f"{v:,.0f}"
    return f"{v:.{dec}f}" if dec else f"{v:.0f}"


def grouped_bars(cats, series, ymax=None, ylabel="", h=300, w=920, dec=0, ymarks=None, cap=None, vals_on=True):
    """cats: list of x labels; series: list of (name, cssvar, [values])."""
    n = len(cats); m = len(series)
    L, R, T, B = 52, 16, 18, 58
    pw, ph = w - L - R, h - T - B
    allv = [v for _, _, vs in series for v in vs if v is not None]
    ymax = ymax or (max(allv) * 1.12 if allv else 1)
    gw = pw / max(n, 1); bw = min(34, (gw - 10) / max(m, 1))
    out = [f'<svg viewBox="0 0 {w} {h}" role="img" aria-label="{esc(ylabel)}">']
    ticks = ymarks or [ymax * i / 4 for i in range(5)]
    for tv in ticks:
        y = T + ph - ph * tv / ymax
        out.append(f'<line x1="{L}" y1="{y:.1f}" x2="{w-R}" y2="{y:.1f}" stroke="var(--line)" stroke-width="1"/>')
        out.append(f'<text x="{L-8}" y="{y+4:.1f}" text-anchor="end" font-size="11" fill="var(--ink3)">{_fmt(tv)}</text>')
    if cap is not None:
        y = T + ph - ph * cap / ymax
        out.append(f'<line x1="{L}" y1="{y:.1f}" x2="{w-R}" y2="{y:.1f}" stroke="var(--bad)" stroke-width="1.5" stroke-dasharray="5 4"/>')
        out.append(f'<text x="{w-R}" y="{y-5:.1f}" text-anchor="end" font-size="11" fill="var(--bad)">cap {cap}</text>')
    for i, c in enumerate(cats):
        x0 = L + i * gw + (gw - bw * m - 2 * (m - 1)) / 2
        for j, (name, col, vs) in enumerate(series):
            v = vs[i] if i < len(vs) else None
            if v is None:
                continue
            bh = ph * v / ymax
            x = x0 + j * (bw + 2); y = T + ph - bh
            out.append(f'<rect x="{x:.1f}" y="{y:.1f}" width="{bw:.1f}" height="{bh:.1f}" rx="3" fill="var({col})"><title>{esc(c)} · {esc(name)}: {_fmt(v, dec)}</title></rect>')
            if vals_on:
                out.append(f'<text x="{x+bw/2:.1f}" y="{y-4:.1f}" text-anchor="middle" font-size="10.5" fill="var(--ink2)">{_fmt(v, dec)}</text>')
        lab = esc(c)
        parts = lab.split("|")
        for k, part in enumerate(parts):
            out.append(f'<text x="{L + i*gw + gw/2:.1f}" y="{T+ph+16+k*13}" text-anchor="middle" font-size="11" fill="var(--ink2)">{part}</text>')
    out.append(f'<line x1="{L}" y1="{T+ph}" x2="{w-R}" y2="{T+ph}" stroke="var(--ink3)" stroke-width="1"/>')
    if ylabel:
        out.append(f'<text x="{L}" y="{T-6}" font-size="11" fill="var(--ink3)">{esc(ylabel)}</text>')
    out.append('</svg>')
    return "".join(out)


def stacked_bars(cats, series, h=300, w=920, ylabel="", dec=1, totals=True):
    n = len(cats)
    L, R, T, B = 52, 16, 18, 58
    pw, ph = w - L - R, h - T - B
    sums = [sum((vs[i] or 0) for _, _, vs in series) for i in range(n)]
    ymax = max(sums) * 1.12 if sums else 1
    gw = pw / max(n, 1); bw = min(64, gw * 0.62)
    out = [f'<svg viewBox="0 0 {w} {h}" role="img" aria-label="{esc(ylabel)}">']
    for k in range(5):
        tv = ymax * k / 4; y = T + ph - ph * tv / ymax
        out.append(f'<line x1="{L}" y1="{y:.1f}" x2="{w-R}" y2="{y:.1f}" stroke="var(--line)"/>')
        out.append(f'<text x="{L-8}" y="{y+4:.1f}" text-anchor="end" font-size="11" fill="var(--ink3)">{_fmt(tv, dec)}</text>')
    for i, c in enumerate(cats):
        x = L + i * gw + (gw - bw) / 2; acc = 0
        for name, col, vs in series:
            v = vs[i] or 0
            if v <= 0:
                continue
            bh = ph * v / ymax; y = T + ph - ph * (acc + v) / ymax
            out.append(f'<rect x="{x:.1f}" y="{y:.1f}" width="{bw:.1f}" height="{max(bh-1.5,0):.1f}" fill="var({col})"><title>{esc(c)} · {esc(name)}: {_fmt(v, dec)}</title></rect>')
            acc += v
        if totals:
            out.append(f'<text x="{x+bw/2:.1f}" y="{T+ph-ph*acc/ymax-5:.1f}" text-anchor="middle" font-size="11" fill="var(--ink2)">{_fmt(acc, dec)}</text>')
        for k, part in enumerate(esc(c).split("|")):
            out.append(f'<text x="{x+bw/2:.1f}" y="{T+ph+16+k*13}" text-anchor="middle" font-size="11" fill="var(--ink2)">{part}</text>')
    out.append(f'<line x1="{L}" y1="{T+ph}" x2="{w-R}" y2="{T+ph}" stroke="var(--ink3)"/>')
    if ylabel:
        out.append(f'<text x="{L}" y="{T-6}" font-size="11" fill="var(--ink3)">{esc(ylabel)}</text>')
    out.append('</svg>')
    return "".join(out)


def day_bars(days, values, h=240, w=920, ylabel="", col="--c1", fmt=lambda v: _fmt(v), marks=None):
    """Simple single-series bar chart over an ordered list of day labels; marks: {day: label} annotations."""
    n = len(days)
    L, R, T, B = 60, 16, 22, 46
    pw, ph = w - L - R, h - T - B
    ymax = max(values) * 1.15 if values else 1
    gw = pw / max(n, 1); bw = max(3, gw * 0.7)
    out = [f'<svg viewBox="0 0 {w} {h}" role="img" aria-label="{esc(ylabel)}">']
    for k in range(5):
        tv = ymax * k / 4; y = T + ph - ph * tv / ymax
        out.append(f'<line x1="{L}" y1="{y:.1f}" x2="{w-R}" y2="{y:.1f}" stroke="var(--line)"/>')
        out.append(f'<text x="{L-8}" y="{y+4:.1f}" text-anchor="end" font-size="11" fill="var(--ink3)">{fmt(tv)}</text>')
    for i, (d, v) in enumerate(zip(days, values)):
        x = L + i * gw + (gw - bw) / 2; bh = ph * v / ymax; y = T + ph - bh
        out.append(f'<rect x="{x:.1f}" y="{y:.1f}" width="{bw:.1f}" height="{bh:.1f}" rx="2" fill="var({col})"><title>{esc(d)}: {fmt(v)}</title></rect>')
        if n <= 40 or i % 2 == 0:
            out.append(f'<text x="{x+bw/2:.1f}" y="{T+ph+14}" text-anchor="middle" font-size="9.5" fill="var(--ink3)" transform="rotate(-45 {x+bw/2:.1f} {T+ph+14})">{esc(d[5:])}</text>')
        if marks and d in marks:
            out.append(f'<text x="{x+bw/2:.1f}" y="{y-6:.1f}" text-anchor="middle" font-size="10" fill="var(--accent-ink)">{esc(marks[d])}</text>')
    out.append(f'<line x1="{L}" y1="{T+ph}" x2="{w-R}" y2="{T+ph}" stroke="var(--ink3)"/>')
    if ylabel:
        out.append(f'<text x="{L}" y="{T-8}" font-size="11" fill="var(--ink3)">{esc(ylabel)}</text>')
    out.append('</svg>')
    return "".join(out)


def hbar(rows, w=920, fmt=lambda v: _fmt(v), col="--c1", label_w=210, rowh=26):
    """rows: list of (label, value, optional color var)."""
    vmax = max((r[1] for r in rows), default=1) or 1
    h = rowh * len(rows) + 10
    out = [f'<svg viewBox="0 0 {w} {h}" role="img">']
    for i, r in enumerate(rows):
        lab, v = r[0], r[1]; c = r[2] if len(r) > 2 else col
        y = 5 + i * rowh; bw = (w - label_w - 90) * v / vmax
        out.append(f'<text x="{label_w-10}" y="{y+rowh*0.68:.1f}" text-anchor="end" font-size="12.5" fill="var(--ink)">{esc(lab)}</text>')
        out.append(f'<rect x="{label_w}" y="{y+5}" width="{bw:.1f}" height="{rowh-11}" rx="3" fill="var({c})"><title>{esc(lab)}: {fmt(v)}</title></rect>')
        out.append(f'<text x="{label_w+bw+8:.1f}" y="{y+rowh*0.68:.1f}" font-size="12" fill="var(--ink2)">{fmt(v)}</text>')
    out.append('</svg>')
    return "".join(out)


def legend(items):
    return '<div class="legend">' + "".join(f'<span><i style="background:var({c})"></i>{esc(n)}</span>' for n, c in items) + '</div>'


def figure(svg, caption, leg=None):
    return f'<figure>{leg or ""}{svg}<figcaption>{caption}</figcaption></figure>'


def tiles(items):
    return '<div class="tiles">' + "".join(
        f'<div class="tile{" hot" if hot else ""}"><div class="n">{n}</div><div class="l">{esc(l)}</div></div>'
        for n, l, hot in items) + '</div>'


def table(headers, rows, num_cols=()):
    th = "".join(f'<th class="{"num" if i in num_cols else ""}">{h}</th>' for i, h in enumerate(headers))
    trs = []
    for r in rows:
        tds = "".join(f'<td class="{"num" if i in num_cols else ""}">{c}</td>' for i, c in enumerate(r))
        trs.append(f'<tr>{tds}</tr>')
    return f'<div class="tbl"><table><thead><tr>{th}</tr></thead><tbody>{"".join(trs)}</tbody></table></div>'


def quote(who, text):
    return f'<div class="q"><span class="who">{esc(who)}</span>{esc(text)}</div>'


def event(date, title, body, kind=""):
    return f'<div class="ev {kind}"><div class="d">{esc(date)}</div><div class="t">{title}</div><p>{body}</p></div>'
