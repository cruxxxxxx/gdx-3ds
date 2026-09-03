#!/usr/bin/env python3
"""R8 Step 4 (Tier 2) -- the `models` DumpClass for tools/gen_dump_all.py: walk F3DEX2 display lists +
vertex data + texture bindings and export Wavefront OBJ (+MTL referencing the already-dumped texture
PNGs). Registers ONE additional CLASS_REGISTRY entry (`models`) beside the landed textures / dlists /
vertexdata / ... classes, WITHOUT touching gen_dump_all.py's architecture: this module only defines a
DumpClass subclass + a self-contained F3DEX2 interpreter, and is imported by gen_dump_all.py (which
registers MODEL_CLASSES). Same import-after-registry pattern as gen_dump_all_extra / _audio (see the
NOTE in gen_dump_all_extra.py about why we never `import gen_dump_all` here).

OUTPUT LAYOUT (dump/models/):
  <yaml>_<rootSymbol>.obj   one OBJ per ROOT display list (a GFX entry no other GFX push/branch-calls)
  <yaml>_<rootSymbol>.mtl   one MTL per OBJ, one `newmtl <texture-key>` per bound texture, map_Kd -> the
                            relative path of the PNG the `textures` class already dumped (dump/<key>.png)
  ek_<rootSymbol>.obj/.mtl  Expansion-Kit meshes (same, from the 64DD disk archive)
  limb_<symbol>.obj         per-limb geometry for the EAD demo skeleton (FZX:LIMB); skeleton.json holds
                            the hierarchy (dl/pos/scale/rot/child/next) for assembly
  manifest.tsv              one row per emitted/attempted model (per-item failure isolation)

FORMAT DERIVATION (all from the in-repo decomp ground truth, decomp/include/PR/gbi.h; config.yml
declares `gbi: F3DEX2` == F3DEX_GBI_2, gbi.h:90-121):
  - Vtx is 16 bytes, big-endian (gbi.h:1082-1104 Vtx_t/Vtx_tn): s16 ob[3] (x,y,z); u16 flag; s16 tc[2]
    (texture coord, S10.5 == 1/32 texel); then EITHER u8 cn[4] (color+alpha) OR s8 n[3]+u8 a (normal),
    disambiguated at draw time by the G_LIGHTING geometry-mode bit (gbi.h:369).
  - G_VTX 0x01 (F3DEX_GBI_2 form, gbi.h:1842-1857): w0 = cmd:8 | n:8<<12 | (v0+n):7<<1; w1 = segmented
    address of the Vtx[]. Loads n verts into the vertex cache at slot v0 (F-Zero X microcode uses a
    64-slot cache -- verified: machine_models DLs issue `G_VTX n=64 v0=0`).
  - G_TRI1 0x05 / G_TRI2 0x06 / G_QUAD 0x07 (gbi.h:2033-2039, 2076-2080, 2216-2234): each triangle word
    packs three cache slots as (v*2) in bytes [16:24),[8:16),[0:8) -- divide by 2 to index the cache.
    TRI2 emits w0's triangle + w1's triangle; QUAD emits w0's triangle + w1's (both faces of the quad).
  - G_DL 0xDE (gbi.h:1898-1902): w1 = segmented sub-DL address; bit16 of w0 == G_DL_NOPUSH (gbi.h:1039)
    distinguishes a push/call (recurse) from a no-push/branch (tail-jump). G_ENDDL 0xDF ends the DL.
  - G_SETTIMG 0xFD (gbi.h:3098-3117): w1 = segmented address of the bound texture image -> the material.
  - G_TEXTURE 0xD7 (gbi.h:2800-2812): w1 = s:16<<16 | t:16, the S15.16 texture-coord scale applied to
    every vertex tc by the RSP; folded into the exported UVs.
  - G_GEOMETRYMODE 0xD9 (gbi.h:2911-2924): w0 low-24 = ~clearbits, w1 = setbits; tracks G_LIGHTING to
    choose color-vs-normal for the Vtx tail.

SOURCE CHOICE (identical rationale to gen_dump_all_extra.DListDumpClass): the raw, un-relocated F3DEX2
command stream and Vtx[] come straight from baserom.us.rev0.z64 at (segment rom_base + yaml offset), NOT
from the o2r archive's OTR-relocated ODLT/OVTX copies (whose opcodes are rewritten with spliced CRC64
path-hashes). Two of the ten model-bearing cart yamls (machine_models seg 9, course_track_gfx seg 8)
store their segment MIO0-compressed in the ROM (`:config: compression: offset:` == the segment base,
crunch64.mio0 per decomp/tools/compress.py) -- those two segments are MIO0-decompressed here first
(the baserom is the retail/compressed image: the compressed segments literally begin with the "MIO0"
magic). Every other cart segment is stored uncompressed and read directly.

EXPANSION KIT: the 64DD disk assets live in fzerox-disk.o2r as byte-identical `ek/<symbol>` slices
(port/gen/ek_slice_manifest.txt is the index: symbol, .ndd offset, len, type). EK display-list pointers
are stored as FULLY-RESOLVED addresses that match the `D_<hex>` symbol names (verified: a SETTIMG in
ek/D_7020808 points at 0x07004080 == symbol D_7004080), so the EK address space is reconstructed by
placing each `D_<hex>` symbol's bytes at the address its name encodes. FZX:LIMB records are 56-byte
structs (torch EADLimbFactory::parse, EADLimbFactory.cpp:120-162): u32 dl; Vec3f scale; Vec3f pos;
Vec3s rot; s16 pad; u32 next; u32 child; u32 assocLimb; u32 assocLimbDL; s16 limbId.

WHAT OBJ CANNOT CARRY (documented, not silently dropped -- see manifest + per-file comments):
  - Vertex colors are written as the widely-supported `v x y z r g b` extension when lighting is off;
    vertex normals as `vn` when lighting is on. A mesh that needs BOTH per-face (rare here) loses one.
  - G_MTX/hierarchical limb transforms are NOT composed into the geometry (rotation is binary-angle
    Vec3s); per-limb OBJs are in limb-local space and skeleton.json carries the transforms for assembly.
  glTF export (which carries vertex color + normal + node transforms together) is noted as future work.
"""
import bisect
import glob
import json
import os
import re
import struct
import sys
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_asset_bindings as gab  # noqa: E402
import yaml  # noqa: E402

# Reuse constants + ROM/manifest helpers from the landed extra module (do NOT re-implement).
import gen_dump_all_extra as gde  # noqa: E402

REPO = gab.REPO
OTR_HEADER_SIZE = gde.OTR_HEADER_SIZE
F3DEX2_MNEMONICS = gde.F3DEX2_MNEMONICS
_read_rom = gde._read_rom
_ROM_CANDIDATES = gde._ROM_CANDIDATES
_write_manifest = gde._write_manifest
_find_first_existing = gde._find_first_existing
_yaml_dir_files = gde._yaml_dir_files
DumpClass = gde.DumpClass

# ── F3DEX2 opcodes (decomp/include/PR/gbi.h:90-121) ───────────────────────────────────────────────────
G_VTX = 0x01
G_TRI1 = 0x05
G_TRI2 = 0x06
G_QUAD = 0x07
G_LINE3D = 0x08
G_TEXTURE = 0xD7
G_GEOMETRYMODE = 0xD9
G_MTX = 0xDA
G_DL = 0xDE
G_ENDDL = 0xDF
G_SETTIMG = 0xFD
G_SETTILE = 0xF5
G_DL_NOPUSH = 1            # gbi.h:1039 (bit16 of w0)
G_LIGHTING = 0x00020000   # gbi.h:369
VTX_SIZE = 16             # sizeof(Vtx), gbi.h:1082-1104
VTX_CACHE = 64            # F-Zero X microcode cache depth (verified: G_VTX n=64 v0=0)
MAX_CMDS = 20000          # per-DL safety cap (recursion + branch guard)

_EK_ARCHIVE_CANDIDATES = [
    os.path.join(REPO, "build", "x64", "port", "Release", "fzerox-disk.o2r"),
    os.path.join(REPO, "build", "x64", "port", "Debug", "fzerox-disk.o2r"),
    os.path.join(REPO, "build_x64", "port", "fzerox-disk.o2r"),
]
_EK_MANIFEST = os.path.join(REPO, "port", "gen", "ek_slice_manifest.txt")


# ── MIO0 decompression (crunch64.mio0 inverse; decomp/tools/compress.py uses crunch64.mio0.compress) ──
def mio0_decompress(data):
    """Decode a standard MIO0 blob. Header (big-endian): 'MIO0', u32 uncompressedSize, u32
    compressedDataOffset, u32 uncompressedDataOffset. Body: a layout bitmap (1=literal byte from the
    uncompressed region, 0=back-reference: u16 = (len-3)<<12 | (dist-1))."""
    if data[:4] != b"MIO0":
        raise ValueError("not a MIO0 blob (magic %r)" % data[:4])
    dest_size, comp_off, uncomp_off = struct.unpack_from(">III", data, 4)
    out = bytearray()
    layout_off = 16
    layout_bit = 0
    while len(out) < dest_size:
        byte = data[layout_off + (layout_bit >> 3)]
        bit = (byte >> (7 - (layout_bit & 7))) & 1
        layout_bit += 1
        if bit:
            out.append(data[uncomp_off])
            uncomp_off += 1
        else:
            w = (data[comp_off] << 8) | data[comp_off + 1]
            comp_off += 2
            length = (w >> 12) + 3
            dist = (w & 0x0FFF) + 1
            start = len(out) - dist
            for i in range(length):
                out.append(out[start + i])
    return bytes(out)


# ── address space: resolve a (segmented or fully-qualified) pointer to raw bytes ──────────────────────
class AddressSpace:
    """A set of [start, end) address ranges, each backed by a bytes buffer with a fixed delta so that
    `buffer[ptr + delta]` is the byte at address `ptr`. Cart segments add one range each (uncompressed
    -> backed by the ROM with delta=rom_base-(seg<<24); compressed -> backed by the decompressed segment
    with delta=-(seg<<24)); the EK space adds one range per `ek/<symbol>` slice."""

    def __init__(self):
        self._starts = []
        self._ranges = []  # parallel to _starts: (start, end, data, delta)

    def add(self, start, length, data, delta):
        self._ranges.append((start, start + length, data, delta))

    def finalize(self):
        self._ranges.sort(key=lambda r: r[0])
        self._starts = [r[0] for r in self._ranges]

    def read(self, ptr, nbytes):
        i = bisect.bisect_right(self._starts, ptr) - 1
        if i < 0:
            return None
        start, end, data, delta = self._ranges[i]
        if ptr < start or ptr + nbytes > end:
            return None
        idx = ptr + delta
        if idx < 0 or idx + nbytes > len(data):
            return None
        return data[idx:idx + nbytes]


# ── F3DEX2 -> mesh interpreter ────────────────────────────────────────────────────────────────────────
class Mesh:
    def __init__(self):
        self.pos = []       # (x, y, z) floats
        self.uv = []        # (u, v) floats
        self.color = []     # (r, g, b) 0..1 or None
        self.normal = []    # (nx, ny, nz) or None
        self.faces = []     # (i0, i1, i2, material, group)  0-based into pos
        self.materials = {}  # key -> (rel_png_path, exists_bool)
        self.stats = {"vtx_loads": 0, "tris": 0, "dl_calls": 0, "mtx": 0,
                      "unresolved_vtx": 0, "unresolved_dl": 0, "unresolved_timg": 0,
                      "dropped_tris": 0}


class Interpreter:
    """Walks one root display list against an AddressSpace, accumulating a Mesh. `tex_map` maps a texture
    image address -> (material_key, rel_png_path, exists, width, height). `label_for` maps a resolved DL
    address -> a group name (its symbol) for the o/g grouping."""

    def __init__(self, space, tex_map, label_for=None):
        self.space = space
        self.tex_map = tex_map
        self.label_for = label_for or (lambda addr: None)
        self.mesh = Mesh()
        self.cache = [None] * VTX_CACHE   # cache slot -> global vertex index (or None)
        self.cur_tex = None               # (key, rel_png, exists, w, h) or None
        self.scale_s = 1.0
        self.scale_t = 1.0
        self.geo_mode = 0
        self._visited = set()

    # -- vertex decode (gbi.h:1082-1104) --
    def _load_vtx(self, w0, w1):
        n = (w0 >> 12) & 0xFF
        end = (w0 >> 1) & 0x7F
        v0 = end - n
        if n <= 0 or v0 < 0 or v0 + n > VTX_CACHE:
            self.mesh.stats["unresolved_vtx"] += 1
            return
        raw = self.space.read(w1, n * VTX_SIZE)
        if raw is None:
            self.mesh.stats["unresolved_vtx"] += 1
            return
        self.mesh.stats["vtx_loads"] += 1
        lighting = bool(self.geo_mode & G_LIGHTING)
        w = self.cur_tex[3] if self.cur_tex else 32
        h = self.cur_tex[4] if self.cur_tex else 32
        for i in range(n):
            x, y, z, _flag, tu, tv = struct.unpack_from(">hhhHhh", raw, i * VTX_SIZE)
            # UV: tc is 1/32 texel, scaled by the active G_TEXTURE scale, normalised by texel dims. OBJ
            # V origin is bottom-left vs the N64 top-left tile origin -> flip V.
            u = (tu / 32.0) * self.scale_s / (w if w else 1)
            v = (tv / 32.0) * self.scale_t / (h if h else 1)
            gidx = len(self.mesh.pos)
            self.mesh.pos.append((float(x), float(y), float(z)))
            self.mesh.uv.append((u, 1.0 - v))
            if lighting:
                nx, ny, nz, _a = struct.unpack_from(">bbbB", raw, i * VTX_SIZE + 12)
                self.mesh.normal.append((float(nx), float(ny), float(nz)))
                self.mesh.color.append(None)
            else:
                r, g, b, _a = struct.unpack_from(">BBBB", raw, i * VTX_SIZE + 12)
                self.mesh.color.append((r / 255.0, g / 255.0, b / 255.0))
                self.mesh.normal.append(None)
            self.cache[v0 + i] = gidx

    def _emit_tri(self, s0, s1, s2, group):
        if s0 >= VTX_CACHE or s1 >= VTX_CACHE or s2 >= VTX_CACHE:
            self.mesh.stats["dropped_tris"] += 1
            return
        i0, i1, i2 = self.cache[s0], self.cache[s1], self.cache[s2]
        if i0 is None or i1 is None or i2 is None:
            self.mesh.stats["dropped_tris"] += 1
            return
        mat = self.cur_tex[0] if self.cur_tex else None
        self.mesh.faces.append((i0, i1, i2, mat, group))
        self.mesh.stats["tris"] += 1

    def _set_timg(self, w1):
        info = self.tex_map.get(w1)
        if info is None:
            self.cur_tex = None
            self.mesh.stats["unresolved_timg"] += 1
            return
        key, rel_png, exists, w, h = info
        self.cur_tex = info
        self.mesh.materials[key] = (rel_png, exists)

    def run(self, root_ptr, group_name):
        if root_ptr in self._visited:
            return
        self._visited.add(root_ptr)
        a = root_ptr
        guard = 0
        while guard < MAX_CMDS:
            word = self.space.read(a, 8)
            if word is None:
                self.mesh.stats["unresolved_dl"] += 1
                return
            w0, w1 = struct.unpack(">II", word)
            op = (w0 >> 24) & 0xFF
            a += 8
            guard += 1
            if op == G_VTX:
                self._load_vtx(w0, w1)
            elif op == G_TRI1:
                self._emit_tri(((w0 >> 16) & 0xFF) >> 1, ((w0 >> 8) & 0xFF) >> 1,
                               (w0 & 0xFF) >> 1, group_name)
            elif op in (G_TRI2, G_QUAD):
                self._emit_tri(((w0 >> 16) & 0xFF) >> 1, ((w0 >> 8) & 0xFF) >> 1,
                               (w0 & 0xFF) >> 1, group_name)
                self._emit_tri(((w1 >> 16) & 0xFF) >> 1, ((w1 >> 8) & 0xFF) >> 1,
                               (w1 & 0xFF) >> 1, group_name)
            elif op == G_SETTIMG:
                self._set_timg(w1)
            elif op == G_TEXTURE:
                self.scale_s = ((w1 >> 16) & 0xFFFF) / 65536.0 or 1.0
                self.scale_t = (w1 & 0xFFFF) / 65536.0 or 1.0
            elif op == G_GEOMETRYMODE:
                clear = (~w0) & 0x00FFFFFF
                self.geo_mode = (self.geo_mode & ~clear) | (w1 & 0x00FFFFFF)
            elif op == G_MTX:
                self.mesh.stats["mtx"] += 1  # transform not composed (documented)
            elif op == G_DL:
                self.mesh.stats["dl_calls"] += 1
                sub = self.label_for(w1) or ("dl_%08X" % w1)
                if self.space.read(w1, 8) is None:
                    self.mesh.stats["unresolved_dl"] += 1
                elif ((w0 >> 16) & G_DL_NOPUSH) == G_DL_NOPUSH:
                    a = w1  # branch / tail-jump: continue the stream at the target
                else:
                    self.run(w1, sub)  # push / call: recurse, then resume
            elif op == G_ENDDL:
                return
            # all other RDP/state ops (SETTILE, SETCOMBINE, sync, colours, ...) don't affect geometry
        # fell off the safety cap
        self.mesh.stats["unresolved_dl"] += 1


# ── OBJ / MTL writers ─────────────────────────────────────────────────────────────────────────────────
def _fmt(x):
    """Deterministic compact float (round to 6dp, strip trailing zeros)."""
    s = "%.6f" % (x + 0.0)
    if "." in s:
        s = s.rstrip("0").rstrip(".")
    return s or "0"


def write_obj_mtl(mesh, obj_path, mtl_path, obj_name, note):
    has_normals = any(nn is not None for nn in mesh.normal)
    lines = ["# %s" % obj_name, "# %s" % note,
             "# vertices=%d faces=%d materials=%d" % (len(mesh.pos), len(mesh.faces),
                                                      len(mesh.materials)),
             "mtllib %s" % os.path.basename(mtl_path), "o %s" % obj_name]
    for i, (x, y, z) in enumerate(mesh.pos):
        col = mesh.color[i]
        if col is not None:
            lines.append("v %s %s %s %s %s %s" % (_fmt(x), _fmt(y), _fmt(z),
                                                  _fmt(col[0]), _fmt(col[1]), _fmt(col[2])))
        else:
            lines.append("v %s %s %s" % (_fmt(x), _fmt(y), _fmt(z)))
    for u, v in mesh.uv:
        lines.append("vt %s %s" % (_fmt(u), _fmt(v)))
    if has_normals:
        for nn in mesh.normal:
            if nn is None:
                lines.append("vn 0 0 1")
            else:
                lines.append("vn %s %s %s" % (_fmt(nn[0]), _fmt(nn[1]), _fmt(nn[2])))

    cur_group = None
    cur_mat = "__init__"
    for i0, i1, i2, mat, group in mesh.faces:
        if group != cur_group:
            lines.append("g %s" % (group or "default"))
            cur_group = group
            cur_mat = "__init__"
        if mat != cur_mat:
            lines.append("usemtl %s" % (mat if mat else "none"))
            cur_mat = mat
        if has_normals:
            lines.append("f %d/%d/%d %d/%d/%d %d/%d/%d"
                         % (i0 + 1, i0 + 1, i0 + 1, i1 + 1, i1 + 1, i1 + 1, i2 + 1, i2 + 1, i2 + 1))
        else:
            lines.append("f %d/%d %d/%d %d/%d" % (i0 + 1, i0 + 1, i1 + 1, i1 + 1, i2 + 1, i2 + 1))
    with open(obj_path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(lines) + "\n")

    mtl_lines = ["# materials for %s" % obj_name,
                 "# map_Kd paths are relative to this .mtl; textures dumped by the `textures` class"]
    mtl_lines.append("newmtl none")
    mtl_lines.append("Kd 0.8 0.8 0.8")
    for key in sorted(mesh.materials):
        rel_png, exists = mesh.materials[key]
        mtl_lines.append("newmtl %s" % key)
        mtl_lines.append("Kd 1 1 1")
        if not exists:
            mtl_lines.append("# NOTE: texture PNG not present on disk (run --classes textures)")
        mtl_lines.append("map_Kd %s" % rel_png.replace("\\", "/"))
    with open(mtl_path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(mtl_lines) + "\n")


# ── model class ──────────────────────────────────────────────────────────────────────────────────────
class CartModel:
    """One cart yaml's segment image + entry tables."""

    def __init__(self, yaml_stem, segment_id, rom_base, compressed_data, rom):
        self.yaml_stem = yaml_stem
        self.segment_id = segment_id
        self.rom_base = rom_base
        self.gfx = []      # (offset, symbol)
        self.tex = {}      # full_addr -> (key, symbol, w, h)
        base_addr = segment_id << 24
        self.space = AddressSpace()
        if compressed_data is not None:
            self.space.add(base_addr, len(compressed_data), compressed_data, -base_addr)
            self._seg_len = len(compressed_data)
        else:
            avail = len(rom) - rom_base
            self.space.add(base_addr, avail, rom, rom_base - base_addr)
            self._seg_len = avail
        self.space.finalize()
        self._label_by_addr = {}

    def label_for(self, addr):
        return self._label_by_addr.get(addr)


def _model_yamls(yaml_dir, rom):
    """Yield a CartModel for every segment-configured cart yaml that carries GFX entries."""
    models = []
    for path in _yaml_dir_files(yaml_dir):
        with open(path) as fh:
            data = yaml.safe_load(fh) or {}
        config = data.get(":config", {}) or {}
        segs = config.get("segments") or []
        if not (segs and isinstance(segs[0], (list, tuple)) and len(segs[0]) >= 2):
            continue
        segment_id = int(segs[0][0])
        rom_base = int(segs[0][1])
        comp = (config.get("compression") or {}).get("offset")
        yaml_stem = os.path.splitext(os.path.basename(path))[0]

        gfx = []
        tex = {}
        for key, val in data.items():
            if not isinstance(val, dict) or str(key).startswith(":"):
                continue
            t = val.get("type")
            if t == "GFX" and val.get("offset") is not None:
                gfx.append((int(val["offset"]), val.get("symbol", key)))
            elif t in ("TEXTURE", "COMPRESSED_TEXTURE") and val.get("offset") is not None \
                    and val.get("width") and val.get("height"):
                full = (segment_id << 24) | int(val["offset"])
                sym = val.get("symbol", key)
                tex[full] = (yaml_stem, sym, int(val["width"]), int(val["height"]))
        if not gfx:
            continue

        compressed_data = None
        if comp is not None:
            base = int(comp)
            blob = rom[base:min(base + 0x400000, len(rom))]
            try:
                compressed_data = mio0_decompress(blob)
            except (ValueError, IndexError) as exc:
                sys.stderr.write("  warn: models %s: MIO0 decompress failed (%s) -- skipping yaml\n"
                                 % (yaml_stem, exc))
                continue

        model = CartModel(yaml_stem, segment_id, rom_base, compressed_data, rom)
        model.gfx = sorted(gfx)
        model.tex = tex
        for offset, symbol in model.gfx:
            model._label_by_addr[(segment_id << 24) | offset] = symbol
        models.append(model)
    return models


def _scan_dl_calls(space, root_ptr):
    """Return the set of DL target addresses (push AND branch) reachable from root_ptr, so root
    detection can subtract sub-DLs. Bounded by MAX_CMDS and a visited set."""
    calls = set()
    stack = [root_ptr]
    seen = set()
    while stack:
        a = stack.pop()
        if a in seen:
            continue
        seen.add(a)
        guard = 0
        while guard < MAX_CMDS:
            word = space.read(a, 8)
            if word is None:
                break
            w0, w1 = struct.unpack(">II", word)
            op = (w0 >> 24) & 0xFF
            a += 8
            guard += 1
            if op == G_DL:
                calls.add(w1)
                if ((w0 >> 16) & G_DL_NOPUSH) == G_DL_NOPUSH:
                    if space.read(w1, 8) is not None and w1 not in seen:
                        a = w1
                    else:
                        break
                else:
                    stack.append(w1)
            elif op == G_ENDDL:
                break
    return calls


def _rel_png(models_dir, dump_dir, key):
    """Relative path (from dump/models/) to the PNG the `textures` class dumps at dump/<key>.png."""
    png = os.path.join(dump_dir, key + ".png")
    return os.path.relpath(png, models_dir), os.path.isfile(png)


class ModelDumpClass(DumpClass):
    name = "models"
    subdir = "models"

    # --------------------------------------------------------------- cart --
    def _run_cart(self, ctx, rom, out_dir, manifest_rows):
        dumped = skipped = failed = 0
        models = _model_yamls(ctx.yaml_dir, rom)
        for model in models:
            # texture-address -> (material key, rel png, exists, w, h)
            tex_map = {}
            for full, (ystem, sym, w, h) in model.tex.items():
                key = "%s/%s" % (ystem, sym)
                rel, exists = _rel_png(out_dir, ctx.dump_dir, key)
                tex_map[full] = (key, rel, exists, w, h)

            # root detection: a GFX entry no other GFX entry push/branch-calls
            called = set()
            for offset, _sym in model.gfx:
                called |= _scan_dl_calls(model.space, (model.segment_id << 24) | offset)
            roots = [(off, sym) for off, sym in model.gfx
                     if ((model.segment_id << 24) | off) not in called]

            for offset, symbol in roots:
                key = "%s_%s" % (model.yaml_stem, symbol)
                obj_path = os.path.join(out_dir, key + ".obj")
                mtl_path = os.path.join(out_dir, key + ".mtl")
                root_ptr = (model.segment_id << 24) | offset
                if os.path.exists(obj_path):
                    skipped += 1
                    v, f = _obj_counts(obj_path)
                    manifest_rows.append((key, "cart", model.yaml_stem, "0x%X" % root_ptr,
                                          v, f, "skip-existing"))
                    continue
                interp = Interpreter(model.space, tex_map, model.label_for)
                interp.run(root_ptr, symbol)
                mesh = interp.mesh
                if not mesh.faces:
                    manifest_rows.append((key, "cart", model.yaml_stem, "0x%X" % root_ptr,
                                          len(mesh.pos), 0, "no-geometry"))
                    continue
                note = ("cart yaml=%s segment=%d rootAddr=0x%X | vtxLoads=%d dlCalls=%d mtx=%d "
                        "unresolved(vtx=%d dl=%d timg=%d) droppedTris=%d"
                        % (model.yaml_stem, model.segment_id, root_ptr, mesh.stats["vtx_loads"],
                           mesh.stats["dl_calls"], mesh.stats["mtx"], mesh.stats["unresolved_vtx"],
                           mesh.stats["unresolved_dl"], mesh.stats["unresolved_timg"],
                           mesh.stats["dropped_tris"]))
                write_obj_mtl(mesh, obj_path, mtl_path, key, note)
                dumped += 1
                status = "ok"
                if mesh.stats["unresolved_vtx"] or mesh.stats["unresolved_dl"] \
                        or mesh.stats["unresolved_timg"]:
                    status = "ok-partial(uv=%d ud=%d ut=%d)" % (
                        mesh.stats["unresolved_vtx"], mesh.stats["unresolved_dl"],
                        mesh.stats["unresolved_timg"])
                manifest_rows.append((key, "cart", model.yaml_stem, "0x%X" % root_ptr,
                                      len(mesh.pos), len(mesh.faces), status))
        return dumped, skipped, failed

    # ----------------------------------------------------------------- ek --
    def _run_ek(self, ctx, out_dir, manifest_rows):
        dumped = skipped = failed = 0
        arch = _find_first_existing(_EK_ARCHIVE_CANDIDATES)
        if arch is None or not os.path.isfile(_EK_MANIFEST):
            print("  models: no EK archive/manifest found -- skipping EK meshes")
            return dumped, skipped, failed, None
        zf = zipfile.ZipFile(arch)
        names = set(zf.namelist())

        rows = []  # (symbol, offset, length, type)
        for ln in open(_EK_MANIFEST, encoding="utf-8"):
            p = ln.split()
            if len(p) >= 4 and p[1].startswith("0x"):
                rows.append((p[0], int(p[1], 16), int(p[2]), p[3]))

        addr_re = re.compile(r"^D_(?:[A-Za-z0-9]+_)?([0-9A-Fa-f]{6,8})$")

        def sym_addr(name):
            m = addr_re.match(name)
            return int(m.group(1), 16) if m else None

        # build EK address space + label + texture maps from every parseable D_<hex> symbol
        space = AddressSpace()
        label_by_addr = {}
        tex_map = {}
        gfx_syms = []
        limb_syms = []
        for name, offset, length, typ in rows:
            entry = "ek/" + name
            if entry not in names:
                continue
            data = zf.read(entry)
            a = sym_addr(name)
            if a is not None:
                space.add(a, len(data), data, -a)
                label_by_addr[a] = name
                if typ == "TEXTURE":
                    tex_map[a] = name  # dims filled below
            if typ == "GFX" and a is not None:
                gfx_syms.append((a, name, data))
            elif typ == "FZX:LIMB":
                limb_syms.append((name, data))
        space.finalize()

        # EK texture dims from the manifest (w,h columns); PNG path = dump/ek/<symbol>.png (the textures
        # class does not currently dump EK disk textures -> exists is typically False, recorded honestly)
        ek_tex_dims = {}
        for ln in open(_EK_MANIFEST, encoding="utf-8"):
            p = ln.split()
            if len(p) >= 8 and p[3] == "TEXTURE":
                a = sym_addr(p[0])
                if a is not None and p[5].isdigit() and p[6].isdigit():
                    ek_tex_dims[a] = (int(p[5]), int(p[6]))
        ek_texinfo = {}
        for a, name in tex_map.items():
            w, h = ek_tex_dims.get(a, (32, 32))
            key = "ek/%s" % name
            rel, exists = _rel_png(out_dir, ctx.dump_dir, key)
            ek_texinfo[a] = (key, rel, exists, w, h)

        def label_for(addr):
            return label_by_addr.get(addr)

        # root detection over EK GFX
        called = set()
        for a, name, data in gfx_syms:
            called |= _scan_dl_calls(space, a)
        roots = [(a, name) for a, name, data in gfx_syms if a not in called]

        for a, name in sorted(roots):
            key = "ek_%s" % name
            obj_path = os.path.join(out_dir, key + ".obj")
            mtl_path = os.path.join(out_dir, key + ".mtl")
            if os.path.exists(obj_path):
                skipped += 1
                v, f = _obj_counts(obj_path)
                manifest_rows.append((key, "ek", "-", "0x%X" % a, v, f, "skip-existing"))
                continue
            interp = Interpreter(space, ek_texinfo, label_for)
            interp.run(a, name)
            mesh = interp.mesh
            if not mesh.faces:
                manifest_rows.append((key, "ek", "-", "0x%X" % a, len(mesh.pos), 0, "no-geometry"))
                continue
            note = ("expansion-kit rootAddr=0x%X | vtxLoads=%d dlCalls=%d unresolved(vtx=%d dl=%d "
                    "timg=%d) droppedTris=%d" % (a, mesh.stats["vtx_loads"], mesh.stats["dl_calls"],
                    mesh.stats["unresolved_vtx"], mesh.stats["unresolved_dl"],
                    mesh.stats["unresolved_timg"], mesh.stats["dropped_tris"]))
            write_obj_mtl(mesh, obj_path, mtl_path, key, note)
            dumped += 1
            status = "ok"
            if mesh.stats["unresolved_vtx"] or mesh.stats["unresolved_dl"] \
                    or mesh.stats["unresolved_timg"]:
                status = "ok-partial(uv=%d ud=%d ut=%d)" % (mesh.stats["unresolved_vtx"],
                          mesh.stats["unresolved_dl"], mesh.stats["unresolved_timg"])
            manifest_rows.append((key, "ek", "-", "0x%X" % a, len(mesh.pos), len(mesh.faces), status))

        # FZX:LIMB -> per-limb OBJ + skeleton.json hierarchy
        skel = self._run_limbs(space, ek_texinfo, label_for, limb_syms, out_dir,
                               manifest_rows)
        d2 = skel["dumped"]
        s2 = skel["skipped"]
        dumped += d2
        skipped += s2
        return dumped, skipped, failed, skel

    def _run_limbs(self, space, tex_map, label_for, limb_syms, out_dir, manifest_rows):
        """Parse each FZX:LIMB (torch EADLimbFactory::parse layout) -> interpret its display list into a
        limb-local OBJ; record the transform hierarchy in skeleton.json (assembly = future work)."""
        dumped = skipped = 0
        hierarchy = []
        for name, data in sorted(limb_syms):
            if len(data) < 0x38:
                continue
            dl = struct.unpack_from(">I", data, 0x00)[0]
            scale = struct.unpack_from(">fff", data, 0x04)
            pos = struct.unpack_from(">fff", data, 0x10)
            rot = struct.unpack_from(">hhh", data, 0x1C)
            next_limb = struct.unpack_from(">I", data, 0x24)[0]
            child_limb = struct.unpack_from(">I", data, 0x28)[0]
            assoc_limb = struct.unpack_from(">I", data, 0x2C)[0]
            assoc_dl = struct.unpack_from(">I", data, 0x30)[0]
            limb_id = struct.unpack_from(">h", data, 0x34)[0]
            hierarchy.append({
                "symbol": name, "limbId": limb_id,
                "dl": "0x%08X" % dl, "associatedLimbDL": "0x%08X" % assoc_dl,
                "scale": list(scale), "pos": list(pos), "rot": list(rot),
                "nextLimb": "0x%08X" % next_limb, "childLimb": "0x%08X" % child_limb,
                "associatedLimb": "0x%08X" % assoc_limb,
            })
            if dl == 0:
                continue
            key = "limb_%s" % name
            obj_path = os.path.join(out_dir, key + ".obj")
            mtl_path = os.path.join(out_dir, key + ".mtl")
            if os.path.exists(obj_path):
                skipped += 1
                v, f = _obj_counts(obj_path)
                manifest_rows.append((key, "ek-limb", "-", "0x%X" % dl, v, f, "skip-existing"))
                continue
            interp = Interpreter(space, tex_map, label_for)
            interp.run(dl, name)
            mesh = interp.mesh
            if not mesh.faces:
                manifest_rows.append((key, "ek-limb", "-", "0x%X" % dl, len(mesh.pos), 0,
                                      "no-geometry"))
                continue
            note = ("EAD skeleton limb (limb-local space; pos/scale/rot in skeleton.json, NOT composed) "
                    "dl=0x%X pos=%s | vtxLoads=%d droppedTris=%d"
                    % (dl, tuple(round(c, 2) for c in pos), mesh.stats["vtx_loads"],
                       mesh.stats["dropped_tris"]))
            write_obj_mtl(mesh, obj_path, mtl_path, key, note)
            dumped += 1
            manifest_rows.append((key, "ek-limb", "-", "0x%X" % dl, len(mesh.pos),
                                  len(mesh.faces), "ok"))

        if hierarchy:
            skel_path = os.path.join(out_dir, "skeleton.json")
            if not os.path.exists(skel_path):
                with open(skel_path, "w", encoding="utf-8") as fh:
                    json.dump({
                        "source": "fzerox-disk.o2r ek/aEADDemoSkeletonLimb* (FZX:LIMB, "
                                  "torch EADLimbFactory::parse layout)",
                        "note": "Per-limb OBJs are in limb-local space. Full skeleton assembly "
                                "(composing pos/scale + binary-angle Vec3s rot down the child/next "
                                "hierarchy) is documented as future work; the transforms are provided "
                                "here for anyone who wants to assemble the pose.",
                        "limbCount": len(hierarchy), "limbs": hierarchy,
                    }, fh, indent=2)
        return {"dumped": dumped, "skipped": skipped}

    # ----------------------------------------------------------------- run --
    def run(self, ctx):
        out_dir = self.out_dir(ctx)
        os.makedirs(out_dir, exist_ok=True)
        manifest_rows = []

        rom, rom_path = _read_rom()
        cart_d = cart_s = cart_f = 0
        if rom is None:
            print("  models: no baserom found (checked %s) -- cart meshes skipped"
                  % ", ".join(os.path.relpath(p, REPO) for p in _ROM_CANDIDATES))
        else:
            cart_d, cart_s, cart_f = self._run_cart(ctx, rom, out_dir, manifest_rows)

        ek_d, ek_s, ek_f, _skel = self._run_ek(ctx, out_dir, manifest_rows)

        manifest_rows.sort(key=lambda r: (r[1], r[0]))
        _write_manifest(os.path.join(out_dir, "manifest.tsv"),
                        "key\tsource\tyaml\trootAddr\tvertices\tfaces\tstatus   "
                        "(one OBJ+MTL per root display list; F3DEX2 -> Wavefront OBJ)",
                        manifest_rows)
        dumped = cart_d + ek_d
        skipped = cart_s + ek_s
        failed = cart_f + ek_f
        print("  models: %d dumped, %d skipped, %d failed  (cart %d/%d, ek %d/%d) -> %s"
              % (dumped, skipped, failed, cart_d, cart_s, ek_d, ek_s,
                 os.path.relpath(out_dir, REPO)))
        return {"class": self.name, "dumped": dumped, "skipped": skipped, "failed": failed,
                "total": len(manifest_rows)}


def _obj_counts(obj_path):
    """Cheap (v, f) count for an existing OBJ (used on idempotent skip so the manifest row is complete)."""
    v = f = 0
    try:
        with open(obj_path, encoding="utf-8") as fh:
            for line in fh:
                if line.startswith("v "):
                    v += 1
                elif line.startswith("f "):
                    f += 1
    except OSError:
        pass
    return v, f


MODEL_CLASSES = {
    ModelDumpClass.name: ModelDumpClass,
}
