import re, sys, os
from framework import CSS, FONTS, PAGES
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
.cover h1 { font-size:64px; }
.cover .toc { margin-top:40px; font-size:14px; }
.cover .toc li { margin:6px 0; }
.cover .meta { color:var(--ink3); font-family:"IBM Plex Mono", monospace; font-size:12px; margin-top:30px; }
@page { size:A4; margin:16mm 14mm 18mm 14mm; }
.media { grid-template-columns:repeat(2,1fr); }
.media.four { grid-template-columns:repeat(2,1fr); }
.media figure { break-inside:avoid; }
.media a.open { display:none; }
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
cover = ('<section class="cover"><p class="eyebrow">Post-mortem · 2026-08-11 → 2026-09-03</p>'
         '<h1>G-Diffuser on New 3DS</h1>'
         '<p class="lede">F-Zero X, ported from the libultraship PC runtime to the New Nintendo 3DS at a native 60 fps with stereoscopic 3D, '
         'by one person directing a fleet of Claude agents over 24 days.</p>'
         '<ol class="toc">' + "".join(f'<li><b>{t}</b> — {s}</li>' for _, t, s in PAGES) + '</ol>'
         '<p class="meta">Sources: git history, docs/research, hardware logs, local Claude Code transcripts. Dollar figures use published API list prices. '
         'Generated from docs/postmortem/.</p></section>')
doc = (f'<!doctype html><html lang="en" data-theme="light"><head><meta charset="utf-8"><title>G-Diffuser on New 3DS — Post-mortem</title>'
       f'{FONTS}<style>{CSS}{PRINT_CSS}</style></head><body><div class="shell"><main>{cover}{"".join(parts)}</main></div></body></html>')
open(OUT, "w").write(doc); print(OUT, len(doc))
