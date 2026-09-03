#!/usr/bin/env python3
"""Generate gdiffuser.o2r (the port's engine archive) from the MIT-licensed Fast3D shaders.
Named after the port, it contains zero game-derived content.

Fast3D loads shaders from ``shaders/<backend>/...`` during window init. The shaders always come from
this checkout's libultraship rather than another fork. Runtime UI is drawn with ImGui primitives or
uses separately licensed fonts, so this archive does not collect arbitrary image files.

The archive also carries the optional D3D11 compiled-shader seed (``shadercache/d3d11.gdxshc``),
which lets a fresh install skip the 9-15ms-per-variant runtime HLSL compile on its very first
run instead of only from the second run onward. See "Recording the seed" below.

The archive is written deterministically (sorted entry order, fixed timestamps) so identical
inputs produce byte-identical output on any machine — required for content hashing, build
caching, and meaningful copy_if_different behavior.

Recording the seed
------------------
The runtime sidecar and the shipped seed use one identical file format, so recording a seed is
just keeping a sidecar that saw enough of the game::

    1. Build, then play through menus, machine select and at least one race per venue.
    2. Copy ``gdiffuser-shadercache-d3d11.bin`` from beside the executable to
       ``port/shadercache/d3d11.gdxshc``.
    3. Rebuild. This script packs it and the next fresh install boots warm.

DXBC is driver-independent, so a seed recorded on one machine is valid on every other. It is NOT
valid across shader-generator edits: the store header carries the configure-time fingerprint from
libultraship/src/fast/CMakeLists.txt, and a stale seed is rejected on load with a warning rather
than trusted. Re-record after touching an emitter or a shader template.

There is deliberately no OpenGL seed. GL program binaries are specific to the vendor, GPU and
driver version, so nothing recorded here would load on anyone else's machine; that backend builds
its own sidecar on first run.
"""
import os
import sys
import zipfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(REPO, "libultraship", "src", "fast", "shaders")
SEED_SRC = os.path.join(REPO, "port", "shadercache")
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(REPO, "build", "x64", "port", "gdiffuser.o2r")

# Fixed timestamp for all entries: zip format's epoch (1980-01-01).
FIXED_DATE = (1980, 1, 1, 0, 0, 0)


def collect(base, prefix, predicate=lambda name: True):
    entries = []
    for root, dirs, files in os.walk(base):
        dirs.sort()
        for f in sorted(files):
            if not predicate(f):
                continue
            full = os.path.join(root, f)
            arc = prefix + os.path.relpath(full, base).replace(os.sep, "/")
            entries.append((arc, full))
    return sorted(entries)


def write_deterministic(z, arc, full):
    with open(full, "rb") as fh:
        data = fh.read()
    info = zipfile.ZipInfo(arc, date_time=FIXED_DATE)
    info.compress_type = zipfile.ZIP_DEFLATED
    info.external_attr = 0o644 << 16
    z.writestr(info, data)


entries = collect(SRC, "shaders/")

# Port branding: the menu-resolution logo the About page renders via Fast3dGui's
# LoadTextureFromRawImage. Original artwork by Kiziio (https://github.com/Kiziio1), owned by the
# project — like the shaders, zero game-derived content, so it belongs in this archive under the
# same licensing story. The full-resolution original stays in assets/branding for the README; this
# 1024-wide copy exists so the GPU never holds a 3531px texture for a menu header.
BRANDING_LOGO = os.path.join(REPO, "assets", "branding", "gdiffuser-logo-menu.png")
if os.path.isfile(BRANDING_LOGO):
    entries.append(("branding/gdiffuser-logo.png", BRANDING_LOGO))
    entries = sorted(entries)

# Optional: absent on a checkout that has never recorded one, which is not an error. The port
# then behaves exactly as it did before seeds existed — first run compiles, later runs read the
# sidecar it wrote.
if os.path.isdir(SEED_SRC):
    entries += collect(SEED_SRC, "shadercache/", lambda name: name.endswith(".gdxshc"))
    entries = sorted(entries)

with zipfile.ZipFile(OUT, "w", zipfile.ZIP_DEFLATED) as z:
    for arc, full in entries:
        write_deterministic(z, arc, full)

print("wrote", OUT)
for n in zipfile.ZipFile(OUT).namelist():
    print(" ", n)
