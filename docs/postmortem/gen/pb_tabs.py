"""Horizontal tabs and "Later:" footnotes for the timeline / Old 3DS / road-to-60 pages.

Pure CSS (radio inputs + labels), so tabs work with JavaScript off and survive build_pdf.py, which keeps
only each page's <main> body: every style these components need is emitted inside the body by STYLE.
In print every panel is shown, stacked, each under its own heading, and the tab bar is hidden.
"""
from framework import esc

STYLE = r"""<style>
.pb-tabs { margin:18px 0 30px; position:relative; }
.pb-tabs > input { position:absolute; opacity:0; width:1px; height:1px; pointer-events:none; }
.pb-tabs .pb-bar { display:flex; gap:0; overflow-x:auto; border-bottom:1px solid var(--rule); scrollbar-width:thin;
  -webkit-overflow-scrolling:touch; contain:inline-size; position:sticky; top:0; background:var(--bg); z-index:2; }
.pb-tabs .pb-bar label { flex:0 0 auto; cursor:pointer; padding:10px 16px 9px 0; margin-right:16px; color:var(--ink2);
  font-size:13px; line-height:1.25; border:0; white-space:nowrap; }
.pb-tabs .pb-bar label b { display:block; font-family:var(--sans); font-weight:700;
  font-size:15.5px; letter-spacing:-.01em; color:var(--ink); }
.pb-tabs .pb-bar label span { font-family:var(--sans); font-size:11px; color:var(--ink3); font-variant-numeric:tabular-nums; }
.pb-tabs .pb-bar label:hover b { color:var(--accent-ink); }
.pb-tabs .pb-panel { display:none; padding-top:6px; }
.pb-tabs .pb-panel > .pb-ph { display:none; }
.pb-tabs .pb-pn { display:flex; justify-content:space-between; gap:12px; margin-top:22px; font-size:13.5px; }
.pb-tabs .pb-pn label { cursor:pointer; color:var(--accent-ink); }
.pb-tabs .pb-pn label:hover { text-decoration:underline; }
.pb-later { display:block; margin:5px 0 2px; padding:4px 10px; border-left:2px solid var(--warn); background:var(--warn-soft);
  border-radius:0; font-size:13px; color:var(--ink); max-width:72ch; }
.pb-later b { color:var(--warn); font-weight:600; }
.pb-later.held { border-left-color:var(--good); background:var(--good-soft); }
.pb-later.held b { color:var(--good); }
sup.pb-fn { font-size:10.5px; font-weight:600; color:var(--warn); margin-left:2px; }
sup.pb-fn a { color:inherit; }
.pb-tabs .tbl, .pb-road .tbl, .pb-now .tbl, .pb-o3 .tbl { contain:inline-size; }
.pb-entries { list-style:none; padding:0; margin:10px 0 18px; }
.pb-entries li { display:grid; grid-template-columns:96px minmax(0,1fr); gap:2px 14px; padding:7px 0; border-bottom:1px solid var(--line); max-width:none; }
.pb-entries li > .d { font-family:var(--sans); font-weight:700; letter-spacing:.04em; font-variant-numeric:tabular-nums; font-size:11.5px; color:var(--ink3); padding-top:2px; }
.pb-entries li > div { font-size:14.5px; color:var(--ink2); min-width:0; }
.pb-entries li > div > .t { font-weight:700; color:var(--ink); }
.pb-entries li.big > div > .t { color:var(--accent-ink); }
.pb-entries li.gold > .d { color:var(--warn); }
.pb-entries li.bad > div > .t { color:var(--bad); }
.pb-entries li:target { background:var(--accent-soft); }
@media (max-width:600px) {
  .pb-entries li { grid-template-columns:1fr; }
  .pb-tabs .pb-bar label { padding:7px 9px 6px; }
}
@media print {
  .pb-tabs .pb-bar, .pb-tabs .pb-pn { display:none !important; }
  .pb-tabs .pb-panel { display:block !important; }
  .pb-tabs .pb-panel > .pb-ph { display:block; font-family:var(--sans); font-weight:700;
    font-size:22px; letter-spacing:-.015em; margin:26px 0 6px; break-after:avoid; }
  .pb-entries li, .pb-later { break-inside:avoid; }
}
</style>"""


def tabs(group, items):
    """group: unique id prefix (unique across the whole print.html); items: [(label, sublabel, panel_html)]."""
    n = len(items)
    css = []
    for i in range(n):
        rid = f"{group}-{i}"
        css.append(f'#{rid}:checked ~ .pb-panel.p{i} {{ display:block; }}')
        css.append(f'#{rid}:checked ~ .pb-bar label[for="{rid}"] {{ color:var(--accent-ink); box-shadow:inset 0 -3px 0 var(--accent); }}')
        css.append(f'#{rid}:checked ~ .pb-bar label[for="{rid}"] b {{ color:var(--accent); }}')
        css.append(f'#{rid}:focus-visible ~ .pb-bar label[for="{rid}"] {{ outline:2px solid var(--accent); outline-offset:-2px; }}')
    out = [f'<style>{"".join(css)}</style><div class="pb-tabs" id="{group}">']
    for i in range(n):
        chk = " checked" if i == 0 else ""
        out.append(f'<input type="radio" name="{group}" id="{group}-{i}"{chk} aria-label="{esc(items[i][0])}">')
    out.append('<div class="pb-bar" role="tablist">')
    for i, (lab, sub, _) in enumerate(items):
        out.append(f'<label for="{group}-{i}" role="tab"><b>{lab}</b><span>{sub}</span></label>')
    out.append('</div>')
    for i, (lab, sub, body) in enumerate(items):
        prev_ = f'<label for="{group}-{i-1}">← {lab_text(items[i-1][0])}</label>' if i > 0 else '<span></span>'
        next_ = f'<label for="{group}-{i+1}">{lab_text(items[i+1][0])} →</label>' if i + 1 < n else '<span></span>'
        out.append(f'<section class="pb-panel p{i}" role="tabpanel"><div class="pb-ph">{lab} · {sub}</div>{body}'
                   f'<div class="pb-pn">{prev_}{next_}</div></section>')
    out.append('</div>')
    return "".join(out)


def lab_text(s):
    import re
    return re.sub(r'<[^>]+>', ' ', s).strip()


def later(text, when, source, held=False, fid=None):
    """A concise footnote saying how a claim changed later (or, held=True, that it was checked and held)."""
    tag = "Held" if held else "Later"
    anchor = f' id="{fid}"' if fid else ""
    return (f'<span class="pb-later{" held" if held else ""}"{anchor}><b>{tag} ({esc(when)}):</b> {text} '
            f'<span class="mono" style="color:var(--ink3);font-size:11.5px">— {source}</span></span>')


def entry(date, title, body, kind="", notes=(), eid=None):
    anchor = f' id="{eid}"' if eid else ""
    marks = "".join(f'<sup class="pb-fn">{esc(n[0])}</sup>' for n in notes)
    notes_html = "".join(n[1] for n in notes)
    return (f'<li class="{kind}"{anchor}><span class="d">{esc(date)}</span><div><span class="t">{title}</span>{marks} '
            f'{body}{notes_html}</div></li>')


# Deep links: a link to an entry inside a hidden tab panel selects that tab first. Optional sugar; the
# tabs themselves need no script.
SCRIPT = r"""<script>
(function () {
  function reveal(id) {
    var el = id && document.getElementById(id);
    if (!el) { return; }
    var panel = el.closest('.pb-panel');
    if (panel) {
      var box = panel.parentNode, idx = Array.prototype.indexOf.call(box.querySelectorAll(':scope > .pb-panel'), panel);
      var radio = box.querySelectorAll(':scope > input')[idx];
      if (radio) { radio.checked = true; }
    }
    el.scrollIntoView({block: 'center'});
  }
  document.addEventListener('click', function (e) {
    var a = e.target.closest && e.target.closest('a[href^="#"]');
    if (a && a.getAttribute('href').length > 1) { e.preventDefault(); history.replaceState(null, '', a.getAttribute('href')); reveal(a.getAttribute('href').slice(1)); }
  });
  if (location.hash) { reveal(location.hash.slice(1)); }
})();
</script>"""
