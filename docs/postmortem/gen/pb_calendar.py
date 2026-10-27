"""Month-grid calendar of hardware cards, for the Old 3DS page.

Each day cell carries short visible chips (so the grid reads in print and on a phone), and every chip has
a hover / focus tooltip with the full detail. In print the tooltips are hidden and a compact one-line-per-
card key, emitted below the grid, carries the same detail. All CSS ships in the body (see pb_tabs.py).
"""
import calendar as _cal
import datetime as _dt
from framework import esc

STYLE = r"""<style>
.pb-cal { display:grid; contain:inline-size; grid-template-columns:repeat(auto-fit, minmax(min(330px,100%),1fr)); gap:18px 22px; margin:16px 0 8px; }
.pb-month h3 { margin:0 0 8px; font-family:var(--sans); font-weight:700; font-size:20px;
  letter-spacing:-.015em; }
.pb-grid { display:grid; grid-template-columns:repeat(7, minmax(0,1fr)); gap:3px; }
.pb-grid .wd { font-family:var(--sans); font-weight:700; letter-spacing:.06em; text-transform:uppercase; font-size:9.5px; color:var(--ink3); text-align:center; padding-bottom:2px; }
.pb-day { position:relative; min-height:58px; background:var(--bg2); border:1px solid var(--line); border-radius:0; padding:2px 3px 3px;
  min-width:0; }
.pb-day.out { background:transparent; border-style:dashed; opacity:.45; }
.pb-day.blank { background:transparent; border:0; }
.pb-day .n { font-family:var(--sans); font-size:10px; color:var(--ink3); line-height:1.2; }
.pb-day.pause { background:repeating-linear-gradient(135deg, var(--bg3) 0 4px, transparent 4px 9px); }
.pb-day .mk { display:block; font-size:9.5px; line-height:1.15; color:var(--accent-ink); font-weight:600; overflow-wrap:anywhere; }
.pb-day .pz { display:block; font-size:9.5px; line-height:1.15; color:var(--ink3); font-style:italic; }
.pb-chip { display:block; margin-top:2px; padding:1px 3px; border-radius:0; font-size:10px; line-height:1.2; cursor:help;
  overflow-wrap:anywhere; background:var(--bg3); color:var(--ink); outline:none; }
.pb-chip.good { background:var(--good-soft); color:var(--good); font-weight:600; }
.pb-chip.bad { background:var(--bad-soft); color:var(--bad); }
.pb-chip.prog { background:var(--blue-soft); color:var(--blue-ink); }
.pb-chip.probe { background:var(--warn-soft); color:var(--warn); }
.pb-chip.log { background:transparent; color:var(--ink3); border:1px dotted var(--line); }
.pb-chip .pb-tip { display:none; position:absolute; z-index:5; left:0; top:100%; width:min(320px, 80vw); margin-top:2px;
  background:var(--bg2); color:var(--ink); border:1px solid var(--rule); border-radius:0; box-shadow:0 8px 24px -10px rgba(0,0,0,.35);
  padding:8px 10px; font-size:12.5px; line-height:1.4; font-weight:400; cursor:auto; }
.pb-chip:hover .pb-tip, .pb-chip:focus .pb-tip { display:block; }
.pb-grid > div:nth-child(7n) .pb-tip, .pb-grid > div:nth-child(7n-1) .pb-tip { left:auto; right:0; }
.pb-tip b { display:block; margin-bottom:2px; }
.pb-tip .src { display:block; margin-top:4px; font-family:var(--sans); font-size:11px; color:var(--ink3); }
.pb-calkey { display:flex; flex-wrap:wrap; gap:4px 14px; font-size:12px; color:var(--ink2); margin:6px 0 4px; }
.pb-calkey i { display:inline-block; width:12px; height:10px; border-radius:0; margin-right:5px; vertical-align:-1px; }
.pb-calkey i.pb-chip { box-shadow:inset 3px 0 0 currentColor; }
.pb-cardlist { display:none; }
@media (max-width:700px) { .pb-chip .pb-tip { position:fixed; left:10px; right:10px; bottom:10px; top:auto; width:auto; } }
@media (max-width:420px) { .pb-day { min-height:50px; padding:1px 2px; } .pb-chip { font-size:9px; } }
@media print {
  .pb-chip .pb-tip { display:none !important; }
  .pb-cal { grid-template-columns:1fr 1fr; break-inside:avoid; }
  .pb-cardlist { display:block; columns:2; column-gap:20px; font-size:9.5pt; margin:8px 0 0; padding-left:0; list-style:none; }
  .pb-cardlist li { break-inside:avoid; margin:0 0 4px; max-width:none; }
}
</style>"""

KINDS = [("good", "60 fps reached"), ("prog", "ran, below 60"), ("bad", "crash / regression"),
         ("probe", "measurement / probe"), ("log", "other log set on file")]


def _chip(c):
    tip = (f'<span class="pb-tip" role="tooltip"><b>{esc(c["date"])} · {c["name"]}</b>{c.get("what", "")}'
           f'{"<br>" if c.get("what") else ""}{c.get("hw", "")}<span class="src">{esc(c.get("src", ""))}</span></span>')
    return f'<span class="pb-chip {c["kind"]}" tabindex="0">{c["abbr"]}{tip}</span>'


def month_grid(year, month, cards, first, last, marks=None, pauses=()):
    marks = marks or {}
    by_day = {}
    for c in cards:
        by_day.setdefault(c["date"], []).append(c)
    out = [f'<div class="pb-month"><h3>{_cal.month_name[month]} {year}</h3><div class="pb-grid">']
    out += [f'<div class="wd">{d}</div>' for d in ("Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun")]
    for week in _cal.Calendar(firstweekday=0).monthdayscalendar(year, month):
        if not any(d and first <= _dt.date(year, month, d) <= last for d in week):
            continue  # weeks wholly outside the range are left out
        for d in week:
            if d == 0:
                out.append('<div class="pb-day blank"></div>')
                continue
            key = f"{month:02d}-{d:02d}"
            date = _dt.date(year, month, d)
            cls = "pb-day"
            if not (first <= date <= last):
                cls += " out"
            pz = [lab for a, b, lab in pauses if a <= key <= b]
            if pz:
                cls += " pause"
            cell = [f'<div class="{cls}"><span class="n">{d}</span>']
            if key in marks:
                cell.append(f'<span class="mk">{marks[key]}</span>')
            if pz and pz[0] and not by_day.get(key) and key == min(a for a, b, lab in pauses if a <= key <= b and lab == pz[0]):
                cell.append(f'<span class="pz">{esc(pz[0])}</span>')
            cell += [_chip(c) for c in by_day.get(key, [])]
            cell.append('</div>')
            out.append("".join(cell))
    out.append('</div></div>')
    return "".join(out)


def calendar(months, cards, first, last, marks=None, pauses=()):
    """months: [(year, month)]; cards: dicts with date 'MM-DD', name, abbr, kind, what, hw, src."""
    key = '<div class="pb-calkey">' + "".join(
        f'<span><i class="pb-chip {k}" style="display:inline-block;margin:0 5px 0 0;padding:0;width:12px;height:10px"></i>{esc(l)}</span>'
        for k, l in KINDS) + '<span><i style="background:repeating-linear-gradient(135deg, var(--bg3) 0 3px, transparent 3px 6px);border:1px solid var(--line)"></i>pause, no cards</span></div>'
    grids = "".join(month_grid(y, m, cards, first, last, marks, pauses) for y, m in months)
    lst = '<ol class="pb-cardlist">' + "".join(
        f'<li><b>{esc(c["date"])} {c["name"]}</b> — {c.get("hw", "")}</li>' for c in cards if c["kind"] != "log") + '</ol>'
    return f'{key}<div class="pb-cal">{grids}</div>{lst}'
