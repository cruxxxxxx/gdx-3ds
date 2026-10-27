"""Shared layout, style and SVG chart helpers for the G-Diffuser 3DS post-mortem pages."""
import html
import re

PAGES = [
    ("index.html", "Overview", "What was built, in one screen"),
    ("author.html", "Author's notes", "The one page written by a human"),
    ("timeline.html", "Every step", "The chronology, New 3DS to v1.1.0"),
    ("road-to-60.html", "Road to 60", "The New 3DS performance campaign, round by round"),
    ("native.html", "Road to 60, again", "The Old 3DS needed a new renderer: how it works, and what shipped"),
    ("boundaries.html", "Pushing the boundaries", "How it compares to the field"),
    ("difficulties.html", "Difficulties", "The fights worth retelling"),
    ("frontier.html", "Frontier", "What this port does that others in its position haven't"),
    ("agentic.html", "Agentic workflow", "How the fleet was run, and how the ship was steered"),
    ("cost.html", "Cost", "Tokens, dollars, subagents"),
]

# No web fonts: the Swiss restyle uses the system's Helvetica Neue / Helvetica / Arial. Kept as a name so
# build_pdf.py's import still works.
FONTS = ""

# One palette, two themes. Mostly monochrome; one Swiss red accent; blue and amber only for headline numbers,
# the phase key and charts. Every helper (pb_/pc_/pd_) reads these variables, so both themes follow.
_LIGHT = """
  --bg:#ffffff; --bg2:#ffffff; --bg3:#f3f3f4; --ink:#111113; --ink2:#47474d; --ink3:#808087; --line:#dcdce0; --rule:#111113;
  --accent:#e4002b; --accent-ink:#c8002a; --accent-soft:#fde8ec; --accent-hi:#ff8597; --accent-glow:rgba(228,0,43,.34);
  --blue:#0047ff; --blue-ink:#0039cc; --blue-soft:#e6edff; --blue-hi:#86a8ff; --blue-glow:rgba(0,71,255,.30);
  --gold:#f0a500; --gold-ink:#b46b00; --gold-soft:#fff3d6; --gold-hi:#ffc94d; --gold-glow:rgba(240,165,0,.40);
  --good:#00845a; --good-soft:#dff3ea; --warn:#b46b00; --warn-soft:#fff3d6; --bad:#c8002a; --bad-soft:#fde8ec;
  --c1:#e4002b; --c2:#f0a500; --c3:#0047ff; --c4:#00a06b; --c5:#55555c; --c6:#86a8ff; --c7:#b46b00; --c8:#b9b9bf;
  --shadow:none;
"""
_DARK = """
  --bg:#1c1c1e; --bg2:#232325; --bg3:#2c2c2f; --ink:#f2f2ef; --ink2:#c4c4c8; --ink3:#8e8e94; --line:#3a3a3e; --rule:#f2f2ef;
  --accent:#ff3b4f; --accent-ink:#ff6676; --accent-soft:#3e1a21; --accent-hi:#ffb4be; --accent-glow:rgba(255,59,79,.55);
  --blue:#4d86ff; --blue-ink:#86a8ff; --blue-soft:#1b2640; --blue-hi:#c8d8ff; --blue-glow:rgba(77,134,255,.55);
  --gold:#ffbb2e; --gold-ink:#ffc34d; --gold-soft:#3a2e12; --gold-hi:#ffe9ad; --gold-glow:rgba(255,187,46,.50);
  --good:#3ccf8e; --good-soft:#163527; --warn:#ffb340; --warn-soft:#3a2e12; --bad:#ff6676; --bad-soft:#3e1a21;
  --c1:#ff3b4f; --c2:#ffbb2e; --c3:#4d86ff; --c4:#3ccf8e; --c5:#a1a1a6; --c6:#a3bfff; --c7:#d69a3a; --c8:#69696f;
  --shadow:none;
"""

CSS = (":root {" + _LIGHT + "  --sans:\"Helvetica Neue\", Helvetica, Arial, sans-serif;"
       " --mono:ui-monospace, \"SF Mono\", Menlo, Consolas, monospace; }\n"
       "@media (prefers-color-scheme: dark) { :root:not([data-theme=\"light\"]) {" + _DARK + "} }\n"
       ":root[data-theme=\"dark\"] {" + _DARK + "}\n") + r"""
* { box-sizing:border-box; }
html { color-scheme:light; -webkit-text-size-adjust:100%; }
:root[data-theme="dark"] { color-scheme:dark; }
@media (prefers-color-scheme: dark) { :root:not([data-theme="light"]) { color-scheme:dark; } }
body { margin:0; background:var(--bg); color:var(--ink); font-family:var(--sans); font-size:16.5px; line-height:1.55;
  -webkit-font-smoothing:antialiased; text-rendering:optimizeLegibility; overflow-x:hidden; }
a { color:var(--accent-ink); text-decoration:none; } a:hover { text-decoration:underline; text-underline-offset:.15em; }
code, pre, .mono { font-family:var(--mono); font-size:.86em; }
pre { background:var(--bg3); padding:12px 14px; border-left:2px solid var(--rule); overflow-x:auto; line-height:1.45; }
code { background:var(--bg3); padding:1px 4px; border-radius:2px; overflow-wrap:anywhere; }
pre code { background:none; padding:0; }
b, strong { font-weight:700; }

/* ---- shell: a narrow left column of small text links, content on a flush-left column ---- */
.shell { display:grid; grid-template-columns:240px minmax(0,1fr); min-height:100vh; }
nav.rail { border-right:1px solid var(--line); padding:28px 22px 32px; position:sticky; top:0; height:100vh; overflow:auto; background:var(--bg); }
nav.rail .rail-top { display:flex; align-items:flex-start; justify-content:space-between; gap:10px; }
nav.rail .brand { font-weight:700; font-size:22px; letter-spacing:-.025em; line-height:1.02; margin:0 0 10px; color:var(--ink); }
nav.rail .brand a { color:inherit; }
nav.rail .brand i { font-style:normal; color:var(--accent); }
nav.rail .sub { color:var(--ink3); font-size:11.5px; line-height:1.45; margin-bottom:26px; }
nav.rail .sub b { display:block; font-size:10.5px; letter-spacing:.12em; text-transform:uppercase; color:var(--ink); margin-bottom:4px; }
nav.rail ol { list-style:none; padding:0; margin:0; counter-reset:p; border-bottom:1px solid var(--line); }
nav.rail li { margin:0; counter-increment:p; max-width:none; }
nav.rail li a { display:grid; grid-template-columns:24px minmax(0,1fr); padding:8px 0 9px; border-top:1px solid var(--line); color:var(--ink2); }
nav.rail li a::before { content:counter(p, decimal-leading-zero); font-size:10.5px; font-weight:700; color:var(--ink3);
  padding-top:2px; font-variant-numeric:tabular-nums; grid-row:span 2; }
nav.rail li a b { display:block; font-weight:700; color:var(--ink); font-size:14px; letter-spacing:-.005em; line-height:1.25; }
nav.rail li a span { font-size:11.5px; color:var(--ink3); line-height:1.35; }
nav.rail li a.on { border-top:2px solid var(--accent); padding-top:7px; }
nav.rail li a.on b, nav.rail li a.on::before { color:var(--accent); }
nav.rail li a:hover { text-decoration:none; } nav.rail li a:hover b { color:var(--accent-ink); }
nav.rail .foot { margin-top:24px; font-size:11px; color:var(--ink3); line-height:1.5; }

/* ---- theme toggle (shown only once the head script has run) ---- */
.theme-toggle { display:none; flex:0 0 auto; align-items:center; gap:7px; margin-top:2px; padding:5px 9px 5px 7px; cursor:pointer;
  font:700 10.5px/1 var(--sans); letter-spacing:.1em; text-transform:uppercase; color:var(--ink); background:transparent;
  border:1px solid var(--rule); border-radius:0; }
html.js .theme-toggle { display:inline-flex; }
.theme-toggle:hover { background:var(--ink); color:var(--bg); }
.theme-toggle:focus-visible { outline:2px solid var(--accent); outline-offset:2px; }
.theme-toggle .ico { width:11px; height:11px; border-radius:50%; border:1.5px solid currentColor;
  background:linear-gradient(90deg, currentColor 50%, transparent 50%); }

/* ---- type ---- */
main { padding:56px 64px 96px; max-width:1180px; min-width:0; }
.eyebrow { font-size:11.5px; font-weight:700; letter-spacing:.14em; text-transform:uppercase; color:var(--accent); margin:0 0 14px; }
h1 { font-weight:700; font-size:clamp(36px, 5.2vw, 66px); line-height:1.0; letter-spacing:-.035em; margin:0 0 24px; text-wrap:balance; max-width:20ch; }
h2 { font-weight:700; font-size:clamp(25px, 2.6vw, 33px); line-height:1.08; letter-spacing:-.022em; margin:64px 0 16px;
  padding-top:14px; border-top:1px solid var(--rule); text-wrap:balance; }
h3 { font-size:18px; font-weight:700; letter-spacing:-.01em; margin:34px 0 8px; }
h4 { font-weight:700; }
p, li { max-width:72ch; }
.lede { font-size:clamp(18px, 1.6vw, 21px); line-height:1.45; letter-spacing:-.005em; color:var(--ink2); max-width:60ch; margin:0 0 34px; }

/* ---- headline numbers ---- */
.tiles { display:grid; grid-template-columns:repeat(auto-fit, minmax(180px,1fr)); gap:28px 28px; margin:26px 0 18px; }
.tile { border-top:1px solid var(--rule); padding:12px 0 4px; container-type:inline-size; min-width:0; }
.tile.hot { border-top:3px solid var(--tone, var(--accent)); padding-top:10px; }
.tile .n { font-size:40px; font-size:clamp(30px, 21cqi, 54px); white-space:nowrap; }
.tile .n small { font-size:.4em; color:var(--ink3); -webkit-text-fill-color:var(--ink3); font-weight:700; letter-spacing:0; margin-left:.12em; }
.tile .l { font-size:13px; color:var(--ink2); margin-top:10px; line-height:1.4; max-width:34ch; }
.tiles.hero { gap:40px 32px; margin:30px 0 22px; }
.tiles.hero .tile .n { font-size:64px; font-size:clamp(46px, 25cqi, 96px); }
.tiles.hero .tile .l { font-size:13.5px; }
.big-num { font-weight:700; letter-spacing:-.05em; line-height:.92; font-variant-numeric:tabular-nums lining-nums; color:var(--tone, var(--ink)); }
.t-red { --tone:var(--accent); --tone-hi:var(--accent-hi); --glow:var(--accent-glow); }
.t-blue { --tone:var(--blue); --tone-hi:var(--blue-hi); --glow:var(--blue-glow); }
.t-gold { --tone:var(--gold-ink); --tone-hi:var(--gold-hi); --glow:var(--gold-glow); }
.t-ink { --tone:var(--ink); --tone-hi:var(--ink3); --glow:transparent; }
@supports ((-webkit-background-clip:text) or (background-clip:text)) {
  .big-num.glow { background-image:linear-gradient(100deg, var(--tone) 0%, var(--tone) 40%, var(--tone-hi) 50%, var(--tone) 60%, var(--tone) 100%);
    background-size:300% 100%; background-position:100% 0; -webkit-background-clip:text; background-clip:text;
    -webkit-text-fill-color:transparent; animation:pm-sheen 7s cubic-bezier(.5,0,.3,1) infinite, pm-glow 4.5s ease-in-out infinite; }
}
.tile:nth-child(2) .big-num.glow { animation-delay:.9s, .6s; } .tile:nth-child(3) .big-num.glow { animation-delay:1.8s, 1.2s; }
.tile:nth-child(4) .big-num.glow { animation-delay:2.7s, 1.8s; } .tile:nth-child(5) .big-num.glow { animation-delay:3.6s, 2.4s; }
.tile:nth-child(6) .big-num.glow { animation-delay:4.5s, 3s; }
@keyframes pm-sheen { 0% { background-position:100% 0; } 55%, 100% { background-position:0 0; } }
@keyframes pm-glow { 0%, 100% { filter:drop-shadow(0 0 0 transparent); } 50% { filter:drop-shadow(0 0 16px var(--glow)); } }

/* ---- figures, tables, components: thin rules, no boxes or shadows ---- */
figure { margin:28px 0 36px; background:transparent; border-top:1px solid var(--rule); padding:14px 0 0; }
figure svg { width:100%; height:auto; display:block; }
figcaption { font-size:13px; color:var(--ink2); margin-top:12px; max-width:88ch; line-height:1.5; }
figcaption b { color:var(--ink); }
.legend { display:flex; flex-wrap:wrap; gap:6px 18px; font-size:12px; color:var(--ink2); margin:0 0 10px; }
.legend i { display:inline-block; width:10px; height:10px; border-radius:0; margin-right:6px; vertical-align:-1px; }
.tbl { overflow-x:auto; margin:18px 0 30px; -webkit-overflow-scrolling:touch; }
table { border-collapse:collapse; width:100%; font-size:14px; font-variant-numeric:tabular-nums; }
th, td { text-align:left; padding:9px 12px 9px 0; border-bottom:1px solid var(--line); vertical-align:top; }
th { font-size:10.5px; letter-spacing:.08em; text-transform:uppercase; color:var(--ink); font-weight:700; border-bottom:1px solid var(--rule); padding-bottom:7px; }
td.num, th.num { text-align:right; font-variant-numeric:tabular-nums; white-space:nowrap; padding-left:12px; }
th.num { white-space:normal; }
tr:hover td { background:var(--bg3); }
.pill { display:inline-block; font-size:10.5px; font-weight:700; letter-spacing:.08em; text-transform:uppercase; padding:2px 6px; border-radius:0; white-space:nowrap; }
.pill.good { background:var(--good-soft); color:var(--good); } .pill.bad { background:var(--bad-soft); color:var(--bad); }
.pill.warn { background:var(--warn-soft); color:var(--warn); } .pill.neu { background:var(--bg3); color:var(--ink2); }
.pill.acc { background:var(--accent-soft); color:var(--accent-ink); }
.timeline { border-left:1px solid var(--rule); margin:22px 0 32px 6px; padding-left:24px; }
.ev { position:relative; margin:0 0 24px; }
.ev::before { content:""; position:absolute; left:-30px; top:7px; width:11px; height:11px; border-radius:50%; background:var(--bg); border:2px solid var(--accent); }
.ev.big::before { background:var(--accent); }
.ev.bad::before { border-color:var(--bad); } .ev.bad.big::before { background:var(--bad); }
.ev.gold::before { border-color:var(--gold); } .ev.gold.big::before { background:var(--gold); }
.ev .d { font-size:11px; font-weight:700; color:var(--ink3); letter-spacing:.1em; text-transform:uppercase; font-variant-numeric:tabular-nums; }
.ev .t { font-weight:700; font-size:16.5px; margin:2px 0 3px; letter-spacing:-.01em; }
.ev p { margin:0 0 4px; color:var(--ink2); font-size:15px; }
.q { border-left:2px solid var(--accent); padding:4px 0 4px 16px; margin:18px 0; font-size:16px; max-width:68ch; }
.q .who { font-size:10.5px; font-weight:700; color:var(--ink3); letter-spacing:.12em; text-transform:uppercase; display:block; margin-bottom:3px; }
.author p { font-size:17.5px; line-height:1.62; max-width:68ch; }
.author ol { max-width:68ch; font-size:17.5px; line-height:1.62; }
.author hr { border:0; border-top:1px solid var(--rule); margin:40px 0; max-width:68ch; }
.media { display:grid; grid-template-columns:repeat(auto-fit, minmax(min(280px,100%),1fr)); gap:24px; margin:22px 0 10px; }
.media.four { grid-template-columns:repeat(auto-fit, minmax(min(200px,100%),1fr)); }
.media figure { margin:0; padding:0; border-top:0; }
.media video, .media img { width:100%; height:auto; display:block; border-radius:0; background:#000; }
.media figcaption { margin-top:8px; }
.media a.open { display:block; margin-top:6px; font-size:10.5px; font-weight:700; letter-spacing:.1em; text-transform:uppercase; }
.media a:has(img) img { cursor:zoom-in; }
.note { border-top:1px solid var(--rule); border-bottom:1px solid var(--line); padding:12px 0; font-size:14.5px; color:var(--ink2); max-width:78ch; margin:20px 0; }
.cols2 { display:grid; grid-template-columns:repeat(auto-fit, minmax(min(300px,100%),1fr)); gap:14px 36px; }
.assume { border-left:2px solid var(--line); padding:6px 0 6px 14px; font-size:13.5px; color:var(--ink2); max-width:78ch; }
.next { margin-top:72px; padding-top:16px; border-top:1px solid var(--rule); display:flex; justify-content:space-between; gap:16px;
  font-size:13px; font-weight:700; letter-spacing:.06em; text-transform:uppercase; }

/* ---- phones ---- */
@media (max-width: 860px) {
  .shell { grid-template-columns:minmax(0,1fr); }
  nav.rail { position:static; height:auto; border-right:0; border-bottom:1px solid var(--rule); padding:18px 16px 14px; }
  nav.rail .sub { margin-bottom:14px; }
  nav.rail ol { display:flex; flex-wrap:wrap; gap:2px 16px; border-bottom:0; }
  nav.rail li a { display:block; border-top:0; padding:3px 0; }
  nav.rail li a.on { border-top:0; padding-top:3px; box-shadow:inset 0 -2px 0 var(--accent); }
  nav.rail li a::before, nav.rail li a span { display:none; }
  nav.rail li a b { font-size:13.5px; }
  nav.rail .foot { margin-top:12px; }
  main { padding:30px 16px 64px; }
  h2 { margin-top:48px; }
}
@media (max-width: 520px) { .tiles { gap:22px 18px; } .tiles.hero { gap:28px 18px; } }

/* ---- motion and print ---- */
@media (prefers-reduced-motion: reduce) {
  *, *::before, *::after { transition:none !important; animation:none !important; }
  .big-num.glow { filter:none !important; }
}
@media print {
  *, *::before, *::after { animation:none !important; transition:none !important; }
  .big-num, .big-num.glow { background:none !important; -webkit-text-fill-color:currentColor !important; filter:none !important;
    -webkit-print-color-adjust:exact; print-color-adjust:exact; }
  .theme-toggle { display:none !important; }
}
"""

# Runs in <head>, before first paint: marks JS as on (so the toggle shows) and applies a stored or ?theme= choice.
HEAD_JS = ("<script>(function(){var d=document.documentElement;d.classList.add('js');var t=null;"
           "try{t=localStorage.getItem('pm-theme');}catch(e){}"
           "var q=/[?&]theme=(light|dark)/.exec(location.search);if(q){t=q[1];}"
           "if(t==='light'||t==='dark'){d.setAttribute('data-theme',t);}})();</script>")

# End of <body>: the theme toggle, and a one-time count-up of headline numbers as they scroll into view.
# The final value is always what the HTML carries, so print and no-JS show it unchanged.
BODY_JS = r"""<script>(function(){
var d=document.documentElement,b=document.querySelector('.theme-toggle');
function cur(){var a=d.getAttribute('data-theme');if(a){return a;}return window.matchMedia&&matchMedia('(prefers-color-scheme: dark)').matches?'dark':'light';}
function label(){if(!b){return;}var dark=cur()==='dark';b.setAttribute('aria-pressed',dark?'true':'false');b.querySelector('.lbl').textContent=dark?'Light':'Dark';b.title=dark?'Switch to light mode':'Switch to dark mode';}
if(b){label();b.addEventListener('click',function(){var n=cur()==='dark'?'light':'dark';d.setAttribute('data-theme',n);try{localStorage.setItem('pm-theme',n);}catch(e){}label();});}
if(window.matchMedia){try{matchMedia('(prefers-color-scheme: dark)').addEventListener('change',label);}catch(e){}}
var still=window.matchMedia&&matchMedia('(prefers-reduced-motion: reduce)').matches;
if(still||!('IntersectionObserver' in window)){return;}
var re=/^([~$]?)(\d[\d,]*(?:\.\d+)?)(\s?[A-Za-z]{0,2})$/;
function run(el){var tn=el.firstChild,fin=tn.nodeValue,m=re.exec(fin);if(!m){return;}
 var raw=m[2].replace(/,/g,''),to=parseFloat(raw),dec=(raw.split('.')[1]||'').length,comma=m[2].indexOf(',')>=0,t0=null,dur=1300;
 function fmt(v){var s=v.toFixed(dec);if(comma){var p=s.split('.');p[0]=p[0].replace(/\B(?=(\d{3})+(?!\d))/g,',');s=p.join('.');}return m[1]+s+m[3];}
 function step(ts){if(t0===null){t0=ts;}var k=Math.min(1,(ts-t0)/dur),e=1-Math.pow(1-k,3);tn.nodeValue=k<1?fmt(to*e):fin;if(k<1){requestAnimationFrame(step);}}
 requestAnimationFrame(step);setTimeout(function(){tn.nodeValue=fin;},dur+400);}
var io=new IntersectionObserver(function(es){es.forEach(function(en){if(en.isIntersecting){io.unobserve(en.target);run(en.target);}});},{threshold:.4});
document.querySelectorAll('.big-num').forEach(function(el){var tn=el.firstChild;if(tn&&tn.nodeType===3&&re.test(tn.nodeValue)&&parseFloat(tn.nodeValue.replace(/[~$,]/g,''))>0){io.observe(el);}});
})();</script>"""

TOGGLE = ('<button class="theme-toggle" type="button" aria-pressed="false" title="Switch colour theme">'
          '<span class="ico" aria-hidden="true"></span><span class="lbl">Theme</span></button>')


def esc(s):
    return html.escape(str(s), quote=True)


# Single source of truth for the project's headline date range, so the live site (nav(), below)
# and the PDF cover (build_pdf.py) cannot drift apart on their own copies of these dates.
DATE_RANGE = "New 3DS 2026-08-11 → 09-03 · Native fork 2026-09-07 → 10-19 (v1.0.0) · After v1.0.0 10-20 → 10-26 (v1.1.0)"


def nav(current):
    items = []
    for f, t, s in PAGES:
        on = ' class="on"' if f == current else ""
        items.append(f'<li><a href="{f}"{on}><b>{esc(t)}</b><span>{esc(s)}</span></a></li>')
    return ('<nav class="rail"><div class="rail-top"><div class="brand">G-Diffuser<br>on 3DS</div>' + TOGGLE + '</div>'
            f'<div class="sub">Post-mortem · F-Zero X<br>{DATE_RANGE.replace(" · ", "<br>")}</div>'
            '<ol>' + "".join(items) + '</ol>'
            '<div class="foot">Sources: git history, docs/research, session transcripts, hardware logs. '
            '<a href="https://github.com/cruxxxxxx/gdx-3ds">Repository</a> · <a href="G-Diffuser-3DS-postmortem.pdf">PDF</a><br>'
            'Sourcing evidence (raw, not reader-facing) — full detail through the New 3DS chapter (v0.9.0), plus a native-fork addendum: '
            '<a href="source-timeline.md">source-timeline.md</a>, '
            '<a href="source-perf-data.md">source-perf-data.md</a></div></nav>')


def number_eyebrow(idx, body):
    """Pages carry a numbered eyebrow ("03 · Road to 60"); keep the number in step with PAGES."""
    return re.sub(r'<p class="eyebrow">\d\d · ', f'<p class="eyebrow">{idx + 1:02d} · ', body, count=1)


def reframe(current, old_html):
    """Re-wrap an already-rendered page in the current navigation, for pages whose raw inputs no longer exist.

    Strips everything from the first prev/next footer onward, not just that footer itself: a page can
    carry content appended *after* its footer on a previous build (e.g. road-to-60.html's continuation
    note), and an end-of-string-anchored strip would miss that on every subsequent rebuild, duplicating
    it forever. page() below adds a fresh footer, and callers that append their own trailing content do
    so after reframe() returns, so this must remove the old footer and anything that followed it.
    """
    title = html.unescape(re.search(r'<title>(.*?)</title>', old_html, re.S).group(1))
    body = re.search(r'<main>(.*)</main>', old_html, re.S).group(1)
    body = re.sub(r'<div class="next">.*', '', body, flags=re.S)
    return page(current, title, body)


def page(current, title, body):
    idx = [f for f, _, _ in PAGES].index(current)
    body = number_eyebrow(idx, body)
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
            f'<title>{esc(title)}</title>{HEAD_JS}<style>{CSS}</style></head>'
            f'<body><div class="shell">{nav(current)}<main>{body}{nx}</main></div>{BODY_JS}</body></html>')


SITE = "https://cruxxxxxx.github.io/gdx-3ds/postmortem/"

# Pages that were removed or merged. Each old URL gets a small redirect stub so existing links keep
# working. Stubs are not in PAGES, so they never appear in the nav, print.html or the PDF.
REDIRECTS = [
    ("old-3ds.html", "native.html#old-3ds", "The Old 3DS road",
     "The Old 3DS page was merged into Road to 60, again (The native fork)."),
    ("after-v1.html", "native.html", "After v1.0.0",
     "The After v1.0.0 page was folded into Road to 60, again (The native fork): one-submission stereo, "
     "Old 3DS memory, the bottom screen, multiplayer and the v1.1.0 release."),
]


def redirect(old, target, title, why):
    t = esc(target)
    return ('<!doctype html><html lang="en"><head><meta charset="utf-8">'
            '<meta name="viewport" content="width=device-width, initial-scale=1">'
            f'<title>{esc(title)} has moved</title>'
            '<meta name="robots" content="noindex">'
            f'<meta http-equiv="refresh" content="0; url={t}">'
            f'<link rel="canonical" href="{esc(SITE + target.split("#")[0])}">'
            '<style>:root { color-scheme:light dark; } body { margin:0; padding:48px 16px; background:Canvas; color:CanvasText; '
            'font-family:"Helvetica Neue", Helvetica, Arial, sans-serif; line-height:1.5; } main { max-width:560px; margin:0 auto; }</style>'
            f'</head><body><main><h1>{esc(title)} has moved</h1><p>{esc(why)}</p>'
            f'<p><a href="{t}">Continue to {t}</a></p></main></body></html>')


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


TONES = {True: "red", False: ""}


def tiles(items, hero=False):
    """items: (number_html, label, tone). tone: True/"red", "blue", "gold" or "ink" gives an accented, glowing
    headline number; False/"" a plain one. hero=True is the extra-large first-screen row (index.html)."""
    out = []
    for n, l, tone in items:
        tone = TONES.get(tone, tone) if isinstance(tone, bool) else tone
        tcls = f" hot t-{tone}" if tone else ""
        ncls = " glow" if tone and tone != "ink" else ""
        out.append(f'<div class="tile{tcls}"><div class="n big-num{ncls}">{n}</div><div class="l">{esc(l)}</div></div>')
    return f'<div class="tiles{" hero" if hero else ""}">' + "".join(out) + '</div>'


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
