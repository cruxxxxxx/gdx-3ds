#!/usr/bin/env python3
"""gen_nibottom_template.py -- build a Photoshop-ready DASH template.

COLD TOOL. Desktop-only, not part of the 3DS build, not game code. The user
may want to restyle the pink-backdrop DASH mockup (docs/research/native/
nibottom-mockup.html) themselves in Photoshop rather than hand-edit canvas
JS. This script renders the same DASH composition nibottom-mockup.html's
<script> draws -- same coordinates, same real game textures, same real
Font1/Font2 glyph text -- as a layered PNG export instead of an HTML canvas,
using Pillow (PIL) instead of a browser's 2D context.

Source textures are read from a full PNG extraction of your own archive
(<gallery>/<folder>/<Symbol>.png, passed as --gallery; about 1,495
PNGs, already decoded -- this script does NOT re-decode the .o2r archive,
it reuses that gallery) rather than re-running o2r_texture_extract.py.

The 3DS build ships none of these pixels: port/3ds/gdx3ds_hudbake.c re-runs the drawing steps
of build() below (background, header, standings, lap, timer, gap, energy_bar) at boot against
the user's own fzerox.o2r, bit-exact with the Pillow calls here (the user's Photoshop pass only
moved the header and energy bar; their placement lives in kDashAssetWidgets). Change a recipe
here and there together.

Usage:
    python3 tools/gen_nibottom_template.py --gallery DIR [--out DIR]

Writes, under --out (default <gallery>/template/):
    dash-full.png       -- the complete DASH screen, 320x240, mode 1 (ALL_TOP).
    layers/*.png         -- one transparent 320x240 PNG per widget, already
                             placed at its real coordinates.
    layout.json           -- per-widget x/y/w/h, source texture(s), font used.
    top-all_top.png        -- top-screen preview schematics, 400x240, one
    top-split.png           per placement mode (nibottom-plan.md §6.1).
    top-all_bottom.png
    README.txt             -- canvas sizes, RGB565 colour-limit note, and how
                             an edited layer goes back in (see
                             tools/nibottom_template_import.py).
"""

import argparse
import json
import math
import os
import sys
from pathlib import Path

from PIL import Image, ImageDraw

HERE = Path(__file__).resolve().parent

DASH_W, DASH_H = 320, 240
TOP_W, TOP_H = 400, 240

# -- real-font glyph scheme, mirrors nibottom-mockup.html's KFONT_INDEX /
#    FONT1_WIDTHS / FONT2_WIDTHS, cited from font.c / fzx_font.h (see
#    nibottom-plan.md §2b). --------------------------------------------
FONT1_WIDTHS = [13,15,14,15,14,13,15,15,7,14,15,14,17,14,14,15,15,14,14,14,14,12,16,16,
                14,14,8,8,16,8,16,7,13,7,13,13,13,12,13,12,13,13,8,8,16,16,16,8]
FONT2_WIDTHS = [8,8,8,8,8,8,8,8,4,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,
                8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8]
FONT1_SPACE, FONT2_SPACE = 7, 3


def kfont_index(ch):
    if "A" <= ch <= "Z":
        return ord(ch) - ord("A")
    if "0" <= ch <= "9":
        return 32 + int(ch)
    if ch == ".":
        return 42
    if ch == "-":
        return 30
    return None


def char_advance(widths, space_w, ch):
    if ch == " ":
        return space_w
    idx = kfont_index(ch)
    return widths[idx] if idx is not None else widths[42]


def char_tex_suffix(ch):
    if "A" <= ch <= "Z":
        return ch
    if "0" <= ch <= "9":
        return "Num%s" % ch
    if ch == ".":
        return "Period"
    if ch == "-":
        return "Dash"
    return None


class Gallery:
    """Looks up the coordinator's already-extracted PNGs by symbol name."""

    def __init__(self, root):
        self.root = Path(root)
        self._index = {}
        if not self.root.is_dir():
            raise SystemExit("gallery not found: %s (pass --gallery)" % self.root)
        for p in self.root.rglob("*.png"):
            self._index.setdefault(p.stem, p)

    def find(self, *candidates):
        for name in candidates:
            if name in self._index:
                return self._index[name]
        raise SystemExit("could not find any of %r under %s" % (candidates, self.root))

    def load(self, *candidates):
        return Image.open(self.find(*candidates)).convert("RGBA")


def new_layer(w=DASH_W, h=DASH_H):
    return Image.new("RGBA", (w, h), (0, 0, 0, 0))


def nn_resize(im, w, h):
    return im.resize((max(1, int(w)), max(1, int(h))), Image.NEAREST)


def draw_font_string(gallery, font_prefix, widths, space_w, canvas, x, y, scale, text, native_h):
    """Pastes real Font1/Font2 glyph PNGs left-to-right using the game's own
    proportional advance widths -- not CSS, not a system font."""
    cx = x
    for ch in text:
        adv = char_advance(widths, space_w, ch) * scale
        if ch != " ":
            suffix = char_tex_suffix(ch)
            if suffix is not None:
                try:
                    g = gallery.load(
                        "common_assets_compressed/%s%sTex" % (font_prefix, suffix),
                        "%s%sTex" % (font_prefix, suffix),
                    )
                except SystemExit:
                    g = None
                if g is not None:
                    gw, gh = g.size
                    scaled = nn_resize(g, gw * scale, gh * scale)
                    canvas.alpha_composite(scaled, (int(round(cx)), int(round(y))))
        cx += adv
    return cx


def font_string_width(widths, space_w, scale, text):
    return sum(char_advance(widths, space_w, ch) * scale for ch in text)


def glyph_strip(gallery, canvas, x, y, scale, atlas_dict, order, text):
    """atlas_dict: {char: Image}; draws each cell at its native size * scale,
    advancing by the cell width (fixed-cell HUD digit atlases, unlike the
    proportional Font1/Font2 text)."""
    cx = x
    for ch in text:
        im = atlas_dict.get(ch)
        if im is not None:
            w, h = im.size
            scaled = nn_resize(im, w * scale, h * scale)
            canvas.alpha_composite(scaled, (int(round(cx)), int(round(y))))
            cx += w * scale
    return cx


def format_time(ms):
    ms = max(0, round(ms))
    mm = ms // 60000
    ss = (ms % 60000) // 1000
    hh = (ms % 1000) // 10
    return "%d'%02d\"%02d" % (mm, ss, hh)


def format_interval(ms):
    sign = "-" if ms < 0 else "+"
    return sign + format_time(abs(ms))


def ordinal_suffix(n):
    return {1: "ST", 2: "ND", 3: "RD"}.get(n % 10, "TH")


def recolor_checker_pink(tile):
    """Same remap nibottom-mockup.html's buildPinkTile() does client-side:
    the real decal is pure opaque black/white, remapped to two pink tones --
    the same kind of recolour the game performs on this texture across its
    own machine custom-paint palettes (nibottom-plan.md §2c)."""
    tile = tile.convert("RGBA")
    px = tile.load()
    w, h = tile.size
    dark, light = (58, 12, 36, 255), (230, 90, 150, 255)
    for j in range(h):
        for i in range(w):
            r, g, b, a = px[i, j]
            px[i, j] = light if r > 127 else dark
    return tile


def tile_pattern(tile, w, h):
    out = Image.new("RGBA", (w, h))
    tw, th = tile.size
    for y in range(0, h, th):
        for x in range(0, w, tw):
            out.paste(tile, (x, y))
    return out


def draw_background(gallery, w, h):
    bg = Image.new("RGBA", (w, h), (23, 8, 15, 255))  # PANEL_BG, #17080f
    checker = gallery.load("machine_custom_gfx/aDecalCheckeredTex", "aDecalCheckeredTex")
    pink_tile = recolor_checker_pink(checker)
    pattern = tile_pattern(pink_tile, w, h)
    pattern.putalpha(Image.eval(pattern.getchannel("A"), lambda a: int(a * 0.16)))
    bg.alpha_composite(pattern)
    logo = gallery.load("common_assets_compressed/aTitleLogoTex", "aTitleLogoTex")
    lw, lh = int(logo.width * 0.55), int(logo.height * 0.55)
    logo_s = nn_resize(logo, lw, lh)
    logo_s.putalpha(Image.eval(logo_s.getchannel("A"), lambda a: int(a * 0.16)))
    bg.alpha_composite(logo_s, (w - lw - 4, h - lh - 4))
    return bg


def draw_map_box(gallery, mode_name):
    """mode-aware: shown (stylised stand-in, same as the HTML mockup -- the
    real minimap is a live per-race bitmap, not a static archive asset) in
    ALL_TOP/ALL_BOTTOM, a same-coordinates placeholder in SPLIT."""
    layer = new_layer()
    box = 192
    ox, oy = 64, 24
    d = ImageDraw.Draw(layer)
    if mode_name == "split":
        d.rectangle([ox, oy, ox + box, oy + box], fill=(10, 10, 12, 255), outline=(90, 42, 66, 255))
        draw_label_font2(gallery, layer, ox + 46, oy + 80, 1.0, "MAP ON TOP")
    else:
        d.rectangle([ox, oy, ox + box, oy + box], fill=(0, 0, 0, 255))
        d.ellipse([ox + box * 0.2, oy + box * 0.2, ox + box * 0.8, oy + box * 0.8],
                  outline=(244, 246, 250, 255), width=6)
        d.rectangle([ox - 1, oy - 1, ox + box + 1, oy + box + 1], outline=(90, 42, 66, 255))
    return layer


def draw_label_font1(gallery, canvas, x, y, scale, text):
    draw_font_string(gallery, "aFont1", FONT1_WIDTHS, FONT1_SPACE, canvas, x, y, scale, text, 16)


def draw_label_font2(gallery, canvas, x, y, scale, text):
    draw_font_string(gallery, "aFont2", FONT2_WIDTHS, FONT2_SPACE, canvas, x, y, scale, text, 8)


def build(gallery, out_dir):
    layers_dir = out_dir / "layers"
    layers_dir.mkdir(parents=True, exist_ok=True)

    layout = []

    # -- background --------------------------------------------------------
    background = draw_background(gallery, DASH_W, DASH_H)
    background.save(layers_dir / "background.png")
    layout.append({"id": "background", "x": 0, "y": 0, "w": DASH_W, "h": DASH_H,
                    "source": ["machine_custom_gfx/aDecalCheckeredTex (recoloured, tiled)",
                               "common_assets_compressed/aTitleLogoTex"], "font": None})

    # -- header: course name + racer count + mode chip ----------------------
    header = new_layer()
    hd = ImageDraw.Draw(header)
    hd.rectangle([0, 0, 320, 22], fill=(33, 12, 24, 255), outline=(90, 42, 66, 255))
    draw_label_font1(gallery, header, 6, 3, 0.56, "MUTE CITY I")
    text = "30 RACERS"
    w2 = font_string_width(FONT2_WIDTHS, FONT2_SPACE, 0.9, text)
    draw_label_font2(gallery, header, 314 - w2, 7, 0.9, text)
    header.save(layers_dir / "header.png")
    layout.append({"id": "header", "x": 0, "y": 0, "w": 320, "h": 22,
                    "source": ["common_assets_compressed/aFont1*Tex", "common_assets_compressed/aFont2*Tex"],
                    "font": "Font1 (course name) / Font2 (racer count)"})

    # -- mode-cycle button (touch target, "MODE" chip) -----------------------
    mode_btn = new_layer()
    md = ImageDraw.Draw(mode_btn)
    chip = (168, 3, 230, 19)
    md.rectangle(chip, fill=(58, 18, 40, 255), outline=(255, 111, 176, 255))
    draw_label_font2(gallery, mode_btn, 173, 7, 0.85, "MODE 1")
    mode_btn.save(layers_dir / "mode_button.png")
    layout.append({"id": "mode_button", "x": 168, "y": 3, "w": 62, "h": 16,
                    "source": ["common_assets_compressed/aFont2*Tex"],
                    "font": "Font2", "note": "touch target; cycles [hud] placement, key-bound to M"})

    # -- map box (the mode-1/3 state; SPLIT's placeholder is a runtime swap,
    #    not a separate Photoshop layer -- see README) -----------------------
    map_box = draw_map_box(gallery, "all_top")
    map_box.save(layers_dir / "map_box.png")
    layout.append({"id": "map_box", "x": 64, "y": 24, "w": 192, "h": 192,
                    "source": ["live per-race 64x64 CI8 bitmap (not a static archive asset)"],
                    "font": None,
                    "note": "stylised stand-in shown here; SPLIT mode replaces this box with a same-coordinates placeholder at runtime"})

    # -- shared helper: a real portrait + real highlight border (if `on`) +
    #    a real 1st..6th rank-ribbon chip for `rank`, matching
    #    nibottom-mockup.html's drawPortrait() exactly. ----------------------
    pos_digits = {str(d): gallery.load("hud_gfx/aPosition%dTex" % d, "aPosition%dTex" % d) for d in range(10)}
    RIBBON_ORDINALS = ["First", "Second", "Third", "Fourth", "Fifth", "Sixth"]

    def paint_portrait(canvas, x, y, size, pilot_suffix, rank, highlighted):
        pimg = gallery.load("hud_gfx/aPortrait%sTex" % pilot_suffix, "aPortrait%sTex" % pilot_suffix)
        canvas.alpha_composite(nn_resize(pimg, size, size), (x, y))
        if highlighted:
            border = gallery.load("hud_gfx/aCharacterPortraitHighlightBorderTex", "aCharacterPortraitHighlightBorderTex")
            canvas.alpha_composite(nn_resize(border, size, size), (x, y))
        else:
            ImageDraw.Draw(canvas).rectangle([x, y, x + size, y + size], outline=(90, 42, 66, 255))
        if 1 <= rank <= 6:
            ribbon = gallery.load("hud_gfx/aPortraitPosition%sTex" % RIBBON_ORDINALS[rank - 1],
                                   "aPortraitPosition%sTex" % RIBBON_ORDINALS[rank - 1])
            rs = size / 32
            rw, rh = int(ribbon.width * rs), int(ribbon.height * rs * 1.6)
            canvas.alpha_composite(nn_resize(ribbon, rw, rh), (x + (size - rw) // 2, y + size - rh - 2))

    # -- OUR position: small portrait + big 2-digit numeral + suffix + "OF n"
    #    Pilot-name text is gone entirely -- several real pilot names
    #    ("CAPTAIN FALCON") didn't fit the 58px column, user feedback --
    #    replaced with content that scales with the column instead of an
    #    arbitrary string length. ----------------------------------------
    our_pos = 4
    portrait = new_layer()
    paint_portrait(portrait, 4, 26, 30, "CaptainFalcon", our_pos, True)
    glyph_strip(gallery, portrait, 36, 26, 0.5, pos_digits, None, "%02d" % our_pos)
    suffix_img = gallery.load("hud_gfx/aPositionOrdinalSuffixTHTex", "aPositionOrdinalSuffixTHTex")
    sw, sh = suffix_img.size
    portrait.alpha_composite(nn_resize(suffix_img, sw * 0.45, sh * 0.45), (36, 43))
    draw_label_font2(gallery, portrait, 4, 60, 0.65, "OF 30")
    ImageDraw.Draw(portrait).rectangle([0, 67, 64, 67], fill=(90, 42, 66, 255))
    portrait.save(layers_dir / "portrait.png")
    layout.append({"id": "portrait", "x": 0, "y": 26, "w": 64, "h": 42,
                    "source": ["hud_gfx/aPortrait<Name>Tex", "hud_gfx/aCharacterPortraitHighlightBorderTex",
                               "hud_gfx/aPosition<0-9>Tex", "hud_gfx/aPositionOrdinalSuffix<ST,ND,RD,TH>Tex",
                               "common_assets_compressed/aFont2*Tex"],
                    "font": "Font2 (OF n)",
                    "note": "OUR position: portrait always flash-highlighted, big numeral+suffix, no name text (dropped, didn't fit)"})

    # -- top-5 standings: gRacersByPosition[], ours highlighted if present --
    # 6 of the 30 real per-character portraits confirmed present
    # (hud_gfx/aPortrait<Name>Tex) are used here; all 30 exist (see
    # nibottom-plan.md §2d) so any character can appear in a live build.
    # Mirrors nibottom-mockup.html's buildTop5(): our own pilot (CaptainFalcon,
    # matching the OUR block above) occupies the row matching our_pos so the
    # highlighted row is actually us, not a different portrait wearing our
    # highlight border; the remaining rows cycle through other pilots.
    others_pool = ["Pico", "SamuraiGoroh", "JodySummer", "MightyGazelle", "MrEad"]
    standings = new_layer()
    row_h = 29
    oi = 0
    for i in range(5):
        rank = i + 1
        if rank == our_pos:
            suffix = "CaptainFalcon"
        else:
            suffix = others_pool[oi % len(others_pool)]
            oi += 1
        paint_portrait(standings, 4, 70 + i * row_h, 24, suffix, rank, rank == our_pos)
    standings.save(layers_dir / "standings.png")
    layout.append({"id": "standings", "x": 0, "y": 67, "w": 64, "h": 148,
                    "source": ["hud_gfx/aPortrait<Name>Tex (any of all 30)",
                               "hud_gfx/aPortraitPosition<First..Sixth>Tex",
                               "hud_gfx/aCharacterPortraitHighlightBorderTex (our row only)"],
                    "font": None,
                    "note": "5 stacked rows, 1st..5th place, real portrait + real rank-ribbon chip per row; our row (if in the top 5) gets the real flash-highlight border, others get a plain frame"})

    # -- lap: LAP label + lap-counter glyphs ---------------------------------
    lap = new_layer()
    lap_label = gallery.load("machine_custom_gfx/aLapTex", "aLapTex")
    lap.alpha_composite(lap_label, (260, 27))
    lap_glyph_names = ["/", "1", "2", "3", "4", "5"]
    # aLapCounterSymbolsTex is a strip, not per-glyph files in the gallery --
    # slice it the same way gen_nibottom_mockup_assets.py does.
    strip = gallery.load("hud_gfx/aLapCounterSymbolsTex", "aLapCounterSymbolsTex")
    sw_, sh_ = strip.size
    cell_h = sh_ // 6
    lap_glyphs = {g: strip.crop((0, i * cell_h, sw_, (i + 1) * cell_h)) for i, g in enumerate(lap_glyph_names)}
    glyph_strip(gallery, lap, 260, 38, 1.15, lap_glyphs, None, "2/3")
    lap.save(layers_dir / "lap.png")
    layout.append({"id": "lap", "x": 258, "y": 26, "w": 60, "h": 28,
                    "source": ["machine_custom_gfx/aLapTex", "hud_gfx/aLapCounterSymbolsTex"], "font": None})

    # -- timer: TIME + BEST labels, timer-digit glyphs -----------------------
    timer = new_layer()
    timer.alpha_composite(gallery.load("hud_gfx/aHudTimeTex", "aHudTimeTex"), (260, 60))
    timer.alpha_composite(gallery.load("common_assets_compressed/aBestTex", "aBestTex"), (260, 96))
    timer_glyph_names = ["0","1","2","3","4","5","6","7","8","9","'",'"',"+","-"]
    tstrip = gallery.load("machine_custom_gfx/aTimerSymbolsTex", "aTimerSymbolsTex")
    tw_, th_ = tstrip.size
    tcell_h = th_ // 14
    timer_glyphs = {g: tstrip.crop((0, i * tcell_h, tw_, (i + 1) * tcell_h)) for i, g in enumerate(timer_glyph_names)}
    glyph_strip(gallery, timer, 260, 76, 0.78, timer_glyphs, None, format_time(42180))
    glyph_strip(gallery, timer, 260, 110, 0.62, timer_glyphs, None, format_time(38940))
    timer.save(layers_dir / "timer.png")
    layout.append({"id": "timer", "x": 258, "y": 59, "w": 60, "h": 69,
                    "source": ["hud_gfx/aHudTimeTex", "common_assets_compressed/aBestTex",
                               "machine_custom_gfx/aTimerSymbolsTex"], "font": None,
                    "note": "covers both the current-lap TIME row and the BEST row (same glyph family)"})

    # -- gap: GAP label + signed timer-glyph interval ------------------------
    gap = new_layer()
    draw_label_font2(gallery, gap, 260, 130, 0.9, "GAP")
    glyph_strip(gallery, gap, 260, 140, 0.62, timer_glyphs, None, format_interval(-1220))
    gap.save(layers_dir / "gap.png")
    layout.append({"id": "gap", "x": 258, "y": 129, "w": 62, "h": 24,
                    "source": ["machine_custom_gfx/aTimerSymbolsTex", "common_assets_compressed/aFont2*Tex"],
                    "font": "Font2 (label)"})

    # -- boost: BOOST label + procedural ready-icon --------------------------
    boost = new_layer()
    draw_label_font2(gallery, boost, 260, 164, 0.9, "BOOST")
    bd = ImageDraw.Draw(boost)
    bx, by, bs = 264, 174, 18
    bd.polygon([
        (bx + bs * 0.55, by), (bx + bs * 0.1, by + bs * 0.58), (bx + bs * 0.42, by + bs * 0.58),
        (bx + bs * 0.3, by + bs), (bx + bs * 0.9, by + bs * 0.38), (bx + bs * 0.55, by + bs * 0.38),
    ], fill=(58, 65, 80, 255))
    boost.save(layers_dir / "boost.png")
    layout.append({"id": "boost", "x": 260, "y": 163, "w": 36, "h": 32,
                    "source": ["common_assets_compressed/aFont2*Tex"], "font": "Font2 (label)",
                    "note": "the ready-icon itself is procedural (not a game texture) in both the HTML mockup and this export"})

    # -- energy bar: ENERGY label + real frame + proportional fill ----------
    energy = new_layer()
    draw_label_font2(gallery, energy, 6, 218, 0.9, "ENERGY")
    ex, ey, ew, eh = 54, 217, 260, 18
    ed = ImageDraw.Draw(energy)
    ed.rectangle([ex, ey, ex + ew, ey + eh], fill=(0, 0, 0, 255))
    frame = gallery.load("hud_gfx/aHudEnergyTex", "aHudEnergyTex")
    frame_s = nn_resize(frame, ew, eh)
    frame_s.putalpha(Image.eval(frame_s.getchannel("A"), lambda a: int(a * 0.35)))
    energy.alpha_composite(frame_s, (ex, ey))
    ed.rectangle([ex, ey, ex + ew, ey + eh], outline=(90, 42, 66, 255))
    rows = ["#9b1313", "#dc1414", "#ff1a1a", "#dc1e1e", "#9b1313"]
    inner_w = round((ew - 2) * 0.63)
    rh = (eh - 2) / len(rows)
    for i, c in enumerate(rows):
        ed.rectangle([ex + 1, ey + 1 + i * rh, ex + 1 + inner_w, ey + 1 + (i + 1) * rh], fill=c)
    energy.save(layers_dir / "energy_bar.png")
    layout.append({"id": "energy_bar", "x": 0, "y": 216, "w": 320, "h": 24,
                    "source": ["hud_gfx/aHudEnergyTex", "common_assets_compressed/aFont2*Tex"],
                    "font": "Font2 (label)",
                    "note": "fill colour rows are sEnergyBarFillColors (hud.c:134-145), not a texture"})

    # -- dash-full.png: flattened composite, mode 1 (ALL_TOP) ----------------
    full = Image.new("RGBA", (DASH_W, DASH_H), (0, 0, 0, 255))
    for name in ["background", "map_box", "header", "mode_button", "portrait",
                 "standings", "lap", "timer", "gap", "boost", "energy_bar"]:
        full.alpha_composite(Image.open(layers_dir / f"{name}.png"))
    full.convert("RGB").save(out_dir / "dash-full.png")

    # -- top-screen previews, one per placement mode -------------------------
    for mode_name, hud_on, map_on in [("all_top", True, True), ("split", False, True), ("all_bottom", False, False)]:
        top = Image.new("RGBA", (TOP_W, TOP_H), (10, 4, 8, 255))
        d = ImageDraw.Draw(top)

        def box(x, y, w, h, text, on):
            fill = (42, 16, 32, 255) if on else (18, 8, 16, 255)
            outline = (255, 111, 176, 255) if on else (58, 32, 48, 255)
            d.rectangle([x, y, x + w, y + h], fill=fill, outline=outline)
            if on:
                draw_label_font2(gallery, top, x + 4, y + 4, 0.8, text)

        box(8, 8, 70, 24, "LAP", hud_on)
        box(8, 36, 70, 24, "TIME", hud_on)
        box(322, 8, 70, 24, "POS", hud_on)
        box(322, 36, 70, 24, "NRG", hud_on)
        box(8, 206, 90, 26, "PORTRAIT", hud_on)
        box(165, 90, 70, 60, "MAP", map_on)
        draw_label_font2(gallery, top, 8, 222, 0.75, "TOP SCREEN -- %s" % mode_name.upper().replace("_", " "))
        top.convert("RGB").save(out_dir / f"top-{mode_name}.png")

    layout_path = out_dir / "layout.json"
    layout_path.write_text(json.dumps({
        "canvas": {"bottom": {"w": DASH_W, "h": DASH_H}, "top": {"w": TOP_W, "h": TOP_H}},
        "widgets": layout,
    }, indent=2))

    readme = out_dir / "README.txt"
    readme.write_text(README_TEXT)

    return layout


README_TEXT = """nibottom DASH template -- for restyling in Photoshop (or any PNG editor)
=========================================================================

This is Phase 1 design material (no game code). It mirrors exactly what
docs/research/native/nibottom-mockup.html draws with <canvas>, as layered
PNGs instead -- so the pink-themed DASH look can be redesigned here instead
of by hand-editing the mockup's JS.

Canvas sizes (fixed, matching the real 3DS screens -- do not resize):
  bottom screen (DASH): 320 x 240 px
  top screen (preview):  400 x 240 px

Colour limits: the bottom screen is NOT a citro3d render target -- it is a
libctru console framebuffer in GSP_RGB565_OES (bottomhud-progress.md §1),
i.e. 16-bit colour, 5 bits red, 6 bits green, 5 bits blue, no alpha. Any
background or widget colour choices made here get quantised to RGB565 at
build time -- fine detail in blue/red (5 bits, 32 levels) will band more
than green (6 bits, 64 levels). Keep flat colour areas rather than smooth
gradients where banding would show. The top screen's real HUD (not this
preview) uses the N64's own textures unchanged; the preview PNGs here are a
schematic, not a pixel-accurate top-screen capture.

Files:
  dash-full.png     the complete DASH screen, 320x240, flattened, mode 1
                     (ALL_TOP) -- all 11 layers composited, for reference.
  layers/*.png       one transparent 320x240 PNG per widget, each already
                     placed at its real coordinates (see layout.json).
                     Required set: background, map_box, portrait, timer,
                     lap, energy_bar, boost, gap, mode_button. "portrait"
                     is OUR position (small portrait + big numeral/suffix
                     + "OF n", no name text -- several real pilot names
                     didn't fit a 58px column, user feedback). One extra
                     layer beyond the original required list: "standings"
                     -- a stacked top-5 list (gRacersByPosition[]), each
                     row a real portrait + real rank-ribbon chip, our own
                     row flash-highlighted if we're in it. header (course
                     name + racer count text) is also an extra convenience
                     layer.
  layout.json         per widget: x, y, w, h (its logical rect inside the
                     320x240 canvas), the source texture(s) it's built
                     from, and which font (if any) drew its text.
  top-all_top.png     top-screen preview schematics, 400x240, one per
  top-split.png        nibottom-plan.md §6.1 placement mode.
  top-all_bottom.png

How to edit:
  1. Open any layers/*.png in Photoshop. It's a normal 320x240 (or 400x240
     for the top-* files) transparent canvas -- edit it like any layer.
  2. Don't move a widget's pixels to a different part of the canvas unless
     you also update its x/y/w/h in layout.json to match -- the importer
     (below) trusts layout.json for where each widget goes, not where the
     pixels happen to sit on the PNG.
  3. The background layer is the one most worth a full redesign -- it's
     currently a tiled, recoloured real decal texture
     (machine_custom_gfx/aDecalCheckeredTex) plus a watermark logo
     (common_assets_compressed/aTitleLogoTex); replace it with anything,
     real-art-sourced or not, as long as the map box and HUD numbers stay
     legible once RGB565-quantised (see above).
  4. When done, run:
         python3 tools/nibottom_template_import.py --layers-dir <edited dir>
     It reads layout.json + every layers/*.png, checks each one is still
     320x240 (400x240 for top-*), RGB565-quantises the colours the real
     framebuffer will actually show, and writes a ready-for-Phase-2 bundle
     (a JSON manifest + the quantised PNGs) -- not game code, just the
     intermediate form an eventual gdx3ds_bottomhud.c painter would read
     pixels from. Phase 2 still needs explicit approval before any of this
     is wired into the build (nibottom-plan.md's STOP line still applies).

Regenerating this template from scratch (e.g. after the mockup's geometry
changes): `python3 tools/gen_nibottom_template.py --gallery DIR`. It reads
an already-extracted PNG gallery
(<gallery>/<folder>/<Symbol>.png) rather
than re-decoding the .o2r archive.
"""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gallery", required=True, metavar="DIR",
                    help="extracted PNG gallery: DIR/<folder>/<Symbol>.png")
    ap.add_argument("--out", default=None, metavar="DIR",
                    help="output directory (default: <gallery>/template)")
    args = ap.parse_args()
    if args.out is None:
        args.out = str(Path(args.gallery) / "template")

    gallery = Gallery(args.gallery)
    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)
    layout = build(gallery, out_dir)
    print("wrote template to %s (%d widgets)" % (out_dir, len(layout)))


if __name__ == "__main__":
    main()
