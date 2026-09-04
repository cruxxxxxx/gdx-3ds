import os, re
from framework import *

README = os.path.join(os.path.dirname(__file__), "..", "..", "..", "README.md")


def _inline(s):
    s = esc(s)
    return re.sub(r'\[([^\]]+)\]\((https?://[^)]+)\)', r'<a href="\2">\1</a>', s)


def _section():
    text = open(README).read()
    m = re.search(r"^## Author's notes\n(.*?)(?=^## )", text, re.S | re.M)
    return m.group(1).strip() if m else ""


def _render(md):
    out = []
    for block in re.split(r'\n\s*\n', md):
        block = block.strip()
        if not block:
            continue
        if block == '---':
            out.append('<hr>')
        elif re.match(r'^\d+\. ', block):
            items = re.split(r'\n(?=\d+\. )', block)
            out.append('<ol>' + ''.join('<li>' + _inline(re.sub(r'^\d+\. ', '', i)) + '</li>' for i in items) + '</ol>')
        else:
            out.append('<p>' + _inline(block) + '</p>')
    return ''.join(out)


def build(D):
    b = ['<p class="eyebrow">02 · Author\'s notes</p><h1>A note from the human</h1>',
         '<p class="lede">Everything else on this site was written by the agent that did the work. This page was not.</p>',
         '<div class="author">', _render(_section()), '</div>']
    return page("author.html", "Author's notes", "".join(b))
