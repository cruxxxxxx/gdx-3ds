#!/usr/bin/env python3
"""tools/gen_nibottom_idle_assets.py -- bake the bottom-screen IDLE view's G-DIFFUSER wordmark
into a C header.

COLD TOOL. Desktop-only, not part of the 3DS build (no CMake step runs this; its OUTPUT is
committed; it is the project's own G-DIFFUSER branding, not game art -- the DASH's game art is
baked at boot from the user's fzerox.o2r instead, port/3ds/gdx3ds_hudbake.c). Consumer:
port/3ds/gdx3ds_bottomhud.c.

WHY THIS ASSET, NOT A FRESH DRAWING: the brief for the idle view ("outside a race, show the
checkerboard + a G-DIFFUSER logo instead of the debug console") asks to look for an existing logo
asset in the repo and reuse it, converted at build time the same way the DASH's other art is
baked, before falling back to hand-drawn Font2 text. assets/branding/gdiffuser-logo-menu.png IS
that asset -- it is the SAME file the desktop/general port's own title-screen menu logo already
uses (port/gdx_menu.cpp's "gdx-logo" texture, port/CMakeLists.txt's branding copy step, and
tools/gen_f3d_o2r.py's "branding/gdiffuser-logo.png" o2r entry all point at this exact PNG), so
the 3DS bottom screen's idle view now shows the real, already-established G-DIFFUSER wordmark
instead of a second, bespoke logo.

MEMORY: deliberately NOT a second full-320x240-canvas bake (gdx3ds_hudbake.h's own
gDashAssetBackground is already a 150 KB RGB565 bake of the checkerboard that the idle view
REUSES as-is -- see DashPaintIdleBaked/DashPaintBackdropBackground in gdx3ds_bottomhud.c). This
script only bakes the small logo glyph itself at its final on-screen size (default 240x76 RGBA5551
== 36,480 B, DASH_IDLE_LOGO_W/H below), so the idle view's total NEW resident data is a few tens
of KB, not another 150 KB.

Output format matches gdx3ds_hudbake.h's own DashGlyph convention exactly (RGBA5551
r5g5b5a1, MSB-first, a=LSB -- gdx3ds_bottomhud.c's existing Rgba5551To565/DashBlitGlyphRatio
already know how to read it) so gdx3ds_bottomhud.c's DashPaintIdleLogo can blit it with the same
helper every other baked glyph here uses, at num=1,den=1 (already baked at final size, no runtime
scale needed). DashGlyph.w/h are `unsigned char` (gdx3ds_hudbake.h) -- this script refuses
to emit a size that would overflow that (max 255 per axis).

Usage:
    python3 tools/gen_nibottom_idle_assets.py [--src FILE] [--out FILE] [--width N]
"""
import argparse
from pathlib import Path

try:
    from PIL import Image
except ImportError:  # pragma: no cover - desktop tooling only
    raise SystemExit("gen_nibottom_idle_assets.py needs Pillow (pip install Pillow)")

DEFAULT_SRC = Path(__file__).resolve().parent.parent / "assets" / "branding" / "gdiffuser-logo-menu.png"
DEFAULT_OUT = Path(__file__).resolve().parent.parent / "port" / "3ds" / "gdx3ds_bottomhud_idle_assets.h"
DEFAULT_WIDTH = 240  # 75% of the 320px-wide bottom screen; see gdx3ds_bottomhud.c DashPaintIdleLogo
MAX_DIM = 255  # DashGlyph.w/h are `unsigned char` (gdx3ds_hudbake.h)


REPO_ROOT = Path(__file__).resolve().parent.parent


def _display_path(path) -> str:
    """The source path as written into the header comment: repo-relative when it is inside the repo."""
    resolved = Path(path).resolve()
    try:
        return resolved.relative_to(REPO_ROOT).as_posix()
    except ValueError:
        return Path(path).name


def quantize_channel(v8: int, bits: int) -> int:
    return (v8 * ((1 << bits) - 1) + 127) // 255


def pack_rgba5551(r: int, g: int, b: int, a: int) -> int:
    """Same r5g5b5a1 MSB-first / a=LSB convention tools/nibottom_template_import.py's own
    pack_rgba5551 already uses -- gdx3ds_bottomhud.c's Rgba5551To565 decodes exactly this. Alpha
    is thresholded, not quantised, so logo edges stay either fully opaque or fully transparent
    (no half-alpha fringe for a single-bit alpha channel to misrender)."""
    r5 = quantize_channel(r, 5)
    g5 = quantize_channel(g, 5)
    b5 = quantize_channel(b, 5)
    a1 = 1 if a >= 128 else 0
    return (r5 << 11) | (g5 << 6) | (b5 << 1) | a1


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", default=str(DEFAULT_SRC))
    ap.add_argument("--out", default=str(DEFAULT_OUT))
    ap.add_argument("--width", type=int, default=DEFAULT_WIDTH)
    args = ap.parse_args()

    src_path = Path(args.src)
    im = Image.open(src_path).convert("RGBA")
    src_w, src_h = im.size
    dst_w = args.width
    dst_h = round(src_h * (dst_w / src_w))
    if dst_w > MAX_DIM or dst_h > MAX_DIM:
        raise SystemExit(f"logo bake {dst_w}x{dst_h} exceeds DashGlyph's unsigned-char {MAX_DIM}px "
                          f"limit per axis -- pass a smaller --width")
    # LANCZOS: this is a static, baked-once UI logo (not a nearest-neighbour game texture), so a
    # smooth downscale reads far better than the glyph atlases' nearest-neighbour scaling does.
    im = im.resize((dst_w, dst_h), Image.LANCZOS)
    px = im.load()

    out_path = Path(args.out)
    with out_path.open("w") as out:
        out.write(
            "/* GENERATED by tools/gen_nibottom_idle_assets.py from\n"
            f" * {_display_path(src_path)}\n"
            " * (the repo's existing G-DIFFUSER brand wordmark -- the SAME file port/gdx_menu.cpp's\n"
            " * own title-screen logo uses, see this script's own module doc for the other\n"
            " * consumers). DO NOT HAND-EDIT -- re-run the generator after the source PNG changes.\n"
            " * Consumer: port/3ds/gdx3ds_bottomhud.c (DashPaintIdleLogo). Contract: gdx3ds_bottomhud.h.\n"
            " *\n"
            " * RGBA5551 r5g5b5a1 (a = LSB, alpha=0 is transparent and must be skipped at blit time),\n"
            " * same RGBA5551 convention gdx3ds_hudbake.h\'s baked art uses --\n"
            " * host order, ready for DashBlitGlyphRatio via a local DashGlyph literal. */\n"
            "#ifndef GDX3DS_BOTTOMHUD_IDLE_ASSETS_H\n"
            "#define GDX3DS_BOTTOMHUD_IDLE_ASSETS_H\n\n"
            f"#define DASH_IDLE_LOGO_W {dst_w}\n"
            f"#define DASH_IDLE_LOGO_H {dst_h}\n\n"
            f"static const unsigned short kDashIdleLogoPixels[{dst_w * dst_h}] = {{\n"
        )
        values = []
        for y in range(dst_h):
            for x in range(dst_w):
                r, g, b, a = px[x, y]
                values.append(pack_rgba5551(r, g, b, a))
        for i in range(0, len(values), 12):
            row = values[i:i + 12]
            out.write("    " + ",".join(str(v) for v in row) + ",\n")
        out.write("};\n\n")
        out.write("#endif /* GDX3DS_BOTTOMHUD_IDLE_ASSETS_H */\n")

    total_bytes = dst_w * dst_h * 2
    print(f"wrote {out_path} ({dst_w}x{dst_h}, {total_bytes} bytes of pixel data, "
          f"source {src_path.name} {src_w}x{src_h})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
