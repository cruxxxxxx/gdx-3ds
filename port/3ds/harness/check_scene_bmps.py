#!/usr/bin/env python3
"""check_scene_bmps.py — headless verifier for the gdx3ds-dl-tests BMP dumps.

The harness (dl_tests_main.cpp) auto-cycles its scenes and writes one
sdmc:/gdx-harness/sceneNN.bmp per scene (24-bit BMP of the 320x240 rendered
area, via the backend's ReadFramebufferToCPU). Azahar's virtual SD is a plain
host directory, so after a headless run:

    python3 port/3ds/harness/check_scene_bmps.py \
        "$HOME/Library/Application Support/Azahar/sdmc/gdx-harness"

Each check below encodes the PASS conditions of EXPECTED.md as pixel
assertions in the 320x240 game space (x right, y down from the top-left of
the rendered area — exactly the BMP layout).
"""

import os
import struct
import sys


def load_bmp(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:2] != b"BM":
        raise ValueError(f"{path}: not a BMP")
    data_off = struct.unpack_from("<I", data, 10)[0]
    w = struct.unpack_from("<i", data, 18)[0]
    h = struct.unpack_from("<i", data, 22)[0]
    bpp = struct.unpack_from("<H", data, 28)[0]
    if bpp != 24:
        raise ValueError(f"{path}: expected 24bpp, got {bpp}")
    row_bytes = (w * 3 + 3) & ~3
    px = [[None] * w for _ in range(h)]
    for y in range(h):
        src_row = h - 1 - y  # bottom-up storage
        base = data_off + src_row * row_bytes
        row = px[y]
        for x in range(w):
            b = data[base + x * 3]
            g = data[base + x * 3 + 1]
            r = data[base + x * 3 + 2]
            row[x] = (r, g, b)
    return px, w, h


def region_mean(px, x0, y0, x1, y1):
    rs = gs = bs = n = 0
    for y in range(y0, y1):
        for x in range(x0, x1):
            r, g, b = px[y][x]
            rs += r
            gs += g
            bs += b
            n += 1
    return (rs / n, gs / n, bs / n)


def region_has(px, x0, y0, x1, y1, pred):
    for y in range(y0, y1):
        for x in range(x0, x1):
            if pred(px[y][x]):
                return True
    return False


FAILURES = []


def check(name, cond, detail=""):
    tag = "PASS" if cond else "FAIL"
    print(f"  [{tag}] {name}" + (f"  ({detail})" if detail else ""))
    if not cond:
        FAILURES.append(name)


def is_reddish(c):
    r, g, b = c
    return r > 150 and g < 110 and b < 110


def is_greenish(c):
    r, g, b = c
    return g > 150 and r < 110 and b < 110


def is_blueish(c):
    r, g, b = c
    return b > 150 and r < 120


def is_yellowish(c):
    r, g, b = c
    return r > 150 and g > 150 and b < 110


def is_magenta(c):
    r, g, b = c
    return r > 160 and b > 160 and g < 90


def is_whiteish(c):
    r, g, b = c
    return r > 180 and g > 180 and b > 180


def near_black(c):
    return max(c) < 40


def scene00(px, w, h):  # STRIP: TL red / TR green / BL blue / BR white
    check("TL red", is_reddish(region_mean(px, 85, 65, 105, 85)))
    check("TR green", is_greenish(region_mean(px, 215, 65, 235, 85)))
    check("BL blue", is_blueish(region_mean(px, 85, 155, 105, 175)))
    check("BR white", is_whiteish(region_mean(px, 215, 155, 235, 175)))


def scene01(px, w, h):  # ROTATE: a gouraud triangle exists (red somewhere near centre band)
    check("triangle content present",
          region_has(px, 60, 20, 260, 220, lambda c: max(c) > 120))


def scene02(px, w, h):  # TEXTURE: red stripe top, green stripe left, white arrow
    check("red stripe on TOP", is_reddish(region_mean(px, 130, 57, 190, 63)))
    check("green stripe on LEFT", is_greenish(region_mean(px, 97, 100, 103, 160)))
    check("white shaft centre", is_whiteish(region_mean(px, 154, 130, 166, 150)))
    check("no red stripe at BOTTOM",
          not is_reddish(region_mean(px, 130, 177, 190, 183)))


def scene03(px, w, h):  # SCISSOR: yellow only top-left quadrant
    check("yellow TL quadrant", is_yellowish(region_mean(px, 20, 20, 140, 100)))
    check("TR black", near_black(region_mean(px, 180, 20, 300, 100)))
    check("BL black", near_black(region_mean(px, 20, 140, 140, 220)))


def scene04(px, w, h):  # DECAL: red centre on blue base, no missing decal
    check("red decal centre", is_reddish(region_mean(px, 140, 105, 180, 135)))
    check("blue base ring", is_blueish(region_mean(px, 65, 110, 95, 130)))


def scene05(px, w, h):  # COMBINE: four mutually-different non-black quads
    means = []
    for q, cx in enumerate((40, 120, 200, 280)):
        m = region_mean(px, cx - 25, 97, cx + 25, 143)
        means.append(m)
        check(f"quad {q + 1} not black", max(m) > 35, f"mean={tuple(round(v) for v in m)}")
    for i in range(4):
        for j in range(i + 1, 4):
            di = sum(abs(a - b) for a, b in zip(means[i], means[j]))
            check(f"quads {i + 1}/{j + 1} differ", di > 45, f"L1={di:.0f}")


def scene06(px, w, h):  # TEXEL1
    # Q1 (control, tile 0): red texel rows at the very top of the quad.
    check("Q1 red top stripe", is_reddish(region_mean(px, 30, 81, 80, 85)))
    check("Q1 grey/white body (not checker)",
          not region_has(px, 20, 90, 90, 155, is_blueish) and
          not region_has(px, 20, 90, 90, 155, is_yellowish))
    # Q2 (TEXEL1): checker must contain BOTH blue and yellow cells.
    check("Q2 has blue cells", region_has(px, 125, 85, 195, 155, is_blueish))
    check("Q2 has yellow cells", region_has(px, 125, 85, 195, 155, is_yellowish))
    check("Q2 not the arrow (no pure red stripe)",
          not is_reddish(region_mean(px, 140, 81, 190, 85)))
    # Sentinel: magenta rows must never be sampled anywhere.
    check("no magenta sentinel anywhere",
          not region_has(px, 0, 0, w, h, is_magenta))
    # UV1 scale: the BOTTOM row of Q2 must still be checker, not padding black.
    check("Q2 bottom edge still checker (UV1 scale)",
          not near_black(region_mean(px, 125, 150, 195, 158)))
    # Q3: left edge tile-0 content (white shade), right edge checker (black shade).
    check("Q3 right edge is checker",
          region_has(px, 290, 85, 310, 155, is_blueish) or
          region_has(px, 290, 85, 310, 155, is_yellowish))
    check("Q3 left edge is arrow-side (no checker)",
          not region_has(px, 232, 90, 248, 155, is_blueish))


def scene07(px, w, h):  # MACHINE (census #16)
    # Left quad: blue-dominant body, red-dominant arrow shaft.
    body = region_mean(px, 36, 145, 56, 160)   # texel cols 2-8, rows ~25 (grey bg)
    check("L body blue-dominant", body[2] > 120 and body[2] > body[0] + 30,
          f"mean={tuple(round(v) for v in body)}")
    shaft = region_mean(px, 74, 120, 86, 140)  # texel cols ~14-17 (white shaft)
    check("L arrow red-dominant", shaft[0] > 140 and shaft[0] > shaft[2] + 30,
          f"mean={tuple(round(v) for v in shaft)}")
    # Right quad: same material, darkening to the right.
    lm = region_mean(px, 195, 100, 215, 140)
    rm = region_mean(px, 265, 100, 285, 140)
    check("R darkens to the right", sum(rm) < sum(lm) * 0.6,
          f"left={sum(lm):.0f} right={sum(rm):.0f}")


def scene08(px, w, h):  # FOG
    left = region_mean(px, 10, 110, 40, 130)
    mid = region_mean(px, 150, 110, 180, 130)
    right = region_mean(px, 290, 110, 316, 130)
    check("left pure red", left[0] > 180 and left[2] < 70,
          f"mean={tuple(round(v) for v in left)}")
    check("right blue-dominant", right[2] > 140 and right[2] > right[0],
          f"mean={tuple(round(v) for v in right)}")
    check("gradient monotonic (blue rises)", left[2] < mid[2] < right[2],
          f"b: {left[2]:.0f} < {mid[2]:.0f} < {right[2]:.0f}")
    check("gradient monotonic (red falls)", left[0] > mid[0] > right[0],
          f"r: {left[0]:.0f} > {mid[0]:.0f} > {right[0]:.0f}")


CHECKS = {
    0: ("STRIP", scene00),
    1: ("ROTATE", scene01),
    2: ("TEXTURE", scene02),
    3: ("SCISSOR", scene03),
    4: ("DECAL", scene04),
    5: ("COMBINE", scene05),
    6: ("TEXEL1", scene06),
    7: ("MACHINE", scene07),
    8: ("FOG", scene08),
}


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    d = sys.argv[1]
    missing = []
    for idx, (name, fn) in sorted(CHECKS.items()):
        path = os.path.join(d, f"scene{idx:02d}.bmp")
        if not os.path.exists(path):
            missing.append(path)
            print(f"scene {idx} {name}: MISSING {path}")
            continue
        px, w, h = load_bmp(path)
        print(f"scene {idx} {name}:")
        fn(px, w, h)
    print()
    if missing:
        print(f"{len(missing)} scene dump(s) missing")
    if FAILURES:
        print(f"FAILED: {len(FAILURES)} check(s): {FAILURES}")
        return 1
    if missing:
        return 1
    print("ALL CHECKS PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
