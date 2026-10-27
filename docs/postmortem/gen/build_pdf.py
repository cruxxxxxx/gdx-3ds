import re, sys, os
from framework import CSS, FONTS, PAGES, DATE_RANGE
SRC = sys.argv[1]; OUT = sys.argv[2]
PRINT_CSS = """
.shell { display:block; } nav.rail { display:none; } main { max-width:none; padding:0; }
body { background:#ffffff; font-size:11.5pt; }
section.pg { page-break-before:always; break-before:page; }
section.pg:first-of-type { page-break-before:auto; }
.next { display:none; } .cols2 { grid-template-columns:1fr; }
figure { break-inside:avoid; box-shadow:none; }
tr, .tile, .ev, .q { break-inside:avoid; }
h2 { break-after:avoid; }
a { color:inherit; }
.cover { min-height:85vh; display:flex; flex-direction:column; justify-content:center; }
.cover h1 { font-size:72px; max-width:none; }
.cover .toc { margin-top:40px; font-size:14px; }
.cover .toc li { margin:6px 0; }
.cover .meta { color:var(--ink3); font-family:var(--sans); font-size:11px; font-weight:700; letter-spacing:.06em; margin-top:30px; }
@page { size:A4; margin:16mm 14mm 18mm 14mm; }
.media { grid-template-columns:repeat(2,1fr); }
.media.four { grid-template-columns:repeat(2,1fr); }
.media figure { break-inside:avoid; }
.media a.open { display:none; }
*, *::before, *::after { animation:none !important; transition:none !important; }
.big-num, .big-num.glow { background:none !important; -webkit-text-fill-color:currentColor !important; filter:none !important;
  -webkit-print-color-adjust:exact; print-color-adjust:exact; }
.tile.hot, .pa-head > div.here, .pa-d, .pill, .legend i { -webkit-print-color-adjust:exact; print-color-adjust:exact; }
.theme-toggle { display:none !important; }
h2 { margin-top:40px; }
tr:hover td { background:none; }
.tiles.hero { grid-template-columns:repeat(3, minmax(0,1fr)) !important; gap:22px 22px; }
.tiles.hero .tile .n { font-size:clamp(34px, 22cqi, 64px); }
h1 { font-size:44px; }
"""
parts = []
for f, title, sub in PAGES:
    html = open(os.path.join(SRC, f)).read()
    m = re.search(r'<main>(.*)</main>', html, re.S)
    body = m.group(1)
    for pf, _, _ in PAGES:
        body = body.replace(f'href="{pf}"', f'href="https://cruxxxxxx.github.io/gdx-3ds/postmortem/{pf}"')
    body = re.sub(r'<video[^>]*poster="([^"]+)"[^>]*></video>', r'<img src="\1" alt="">', body)
    parts.append(f'<section class="pg">{body}</section>')
cover = (f'<section class="cover"><p class="eyebrow">Post-mortem · {DATE_RANGE}</p>'
         '<h1>G-Diffuser on 3DS</h1>'
         '<p class="lede">F-Zero X, ported from the libultraship PC runtime to the New Nintendo 3DS, reaching 59–60 fps with stereoscopic 3D, '
         'by one person directing a fleet of Claude agents over 24 days — then forked to a native renderer that also reached 59–60 fps '
         'on an Old 3DS development build, brought the 64DD Expansion Kit to the New 3DS, and added a bottom-screen dashboard, '
         'shipping as v1.0.0 (stereoscopic 3D ships as not yet a locked 60 fps on either console — see RELEASE-NOTES.md), '
         'then, a week later, v1.1.0: multiplayer, one-submission stereo and Old 3DS memory fixes.</p>'
         '<ol class="toc">' + "".join(f'<li><b>{t}</b> — {s}</li>' for _, t, s in PAGES) + '</ol>'
         '<p class="meta">Sources: git history, docs/research, hardware logs, local Claude Code transcripts. Dollar figures use published API list prices. '
         'Generated from docs/postmortem/.</p></section>')
doc = (f'<!doctype html><html lang="en" data-theme="light"><head><meta charset="utf-8"><title>G-Diffuser on 3DS — Post-mortem</title>'
       f'{FONTS}<style>{CSS}{PRINT_CSS}</style></head><body><div class="shell"><main>{cover}{"".join(parts)}</main></div></body></html>')
open(OUT, "w").write(doc); print(OUT, len(doc))
