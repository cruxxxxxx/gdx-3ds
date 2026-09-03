#!/usr/bin/env python3
"""R8 Step 4 (partial) -- Tier-1 typed dumps + ghosts/fonts DumpClasses for tools/gen_dump_all.py.

Registers six additional CLASS_REGISTRY entries (coursedata, dlists, vertexdata, tables, ghosts,
fonts) beside the landed `textures` class, WITHOUT touching gen_dump_all.py's architecture: this
module only defines DumpClass subclasses + a couple of source-reading helpers and is imported by
gen_dump_all.py, which registers them. See docs/investigation/2026-07-18/o2r-migration/
R8_DISK_ARCHIVE_AND_DUMP_ALL_PLAN.md (Steps 4/4b) for scope.

SOURCES (evidence trail, see the per-class docstrings below for exact file:line citations):
  - segment_blob/course_data (generic.o2r, BLOB framing) -- 26 raw CourseData structs (fzx_course.h)
  - GFX/VTX yaml entries -- raw N64 command/vertex bytes read directly from the baserom (NOT the
    o2r archive's OTR-relocated ODLT/OVTX copies -- see DListDumpClass docstring for why)
  - audio_blob/audio_bank + audio_blob/audio_seq (generic.o2r) + decomp/src/audio/disk/audio_tables.c
    (the compiled-in AudioTable initializers -- ground truth for entry counts/offsets)
  - staff_ghost_records/* (generic.o2r, FZX:GHOST/XGRD framing) -- parsed per torch's own
    GhostRecordFactory/GhostRecordBinaryExporter (torch/src/factories/fzerox/GhostRecordFactory.cpp)
  - N64DDIPLROM.n64 (raw 64DD IPL ROM image) font block + segment_blob/kanji_tables (generic.o2r)
"""
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
from PIL import Image  # noqa: E402

# NOTE: deliberately NOT `import gen_dump_all` here -- gen_dump_all.py imports THIS module (to
# register EXTRA_CLASSES into its CLASS_REGISTRY), and when gen_dump_all.py is run directly as a
# script it loads under the name "__main__", not "gen_dump_all"; an `import gen_dump_all` from here
# would then trigger a SECOND, independent execution of gen_dump_all.py under that module name,
# re-entering this file mid-import and crashing before EXTRA_CLASSES exists. Everything needed
# (REPO, OTR_HEADER_SIZE, a DumpClass-shaped base) is duck-typed locally instead -- no cycle.
REPO = gab.REPO
OTR_HEADER_SIZE = 64  # universal libultraship ResourceInitData header (verified in gen_dump_all.py)


class DumpClass:
    """Local duplicate of gen_dump_all.DumpClass's `out_dir()` contract (name/subdir/out_dir) --
    gen_dump_all.py only ever calls cls.name/.subdir/.run()/.out_dir() by duck typing (no isinstance
    check), so subclassing gen_dump_all.DumpClass itself is unnecessary and would reintroduce the
    circular import described above."""
    name = "?"
    subdir = ""

    def out_dir(self, ctx):
        return ctx.dump_dir if not self.subdir else os.path.join(ctx.dump_dir, self.subdir)


def _find_first_existing(paths):
    for p in paths:
        if os.path.isfile(p):
            return p
    return None


# candidate build-tree locations for the two raw binaries the o2r archive doesn't (fully) carry
_ROM_CANDIDATES = [
    os.path.join(REPO, "build", "x64", "port", "Release", "baserom.us.rev0.z64"),
    os.path.join(REPO, "build", "x64", "port", "Debug", "baserom.us.rev0.z64"),
    os.path.join(REPO, "build_x64", "port", "baserom.us.rev0.z64"),
]
_IPL_CANDIDATES = [
    os.path.join(REPO, "build", "x64", "port", "Release", "N64DDIPLROM.n64"),
    os.path.join(REPO, "build", "x64", "port", "Debug", "N64DDIPLROM.n64"),
]


def _read_rom():
    path = _find_first_existing(_ROM_CANDIDATES)
    if path is None:
        return None, None
    with open(path, "rb") as fh:
        return fh.read(), path


def _read_ipl():
    path = _find_first_existing(_IPL_CANDIDATES)
    if path is None:
        return None, None
    with open(path, "rb") as fh:
        return fh.read(), path


def _archive_zip(ctx):
    return ctx.source.zip


def _write_manifest(path, header, rows):
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("# %s\n" % header)
        for row in rows:
            fh.write("\t".join(str(c) for c in row) + "\n")


def _yaml_dir_files(yaml_dir):
    return [p for p in sorted(glob.glob(os.path.join(yaml_dir, "*.yaml")))
            if os.path.basename(p) not in gab.BLOB_RECIPE_FILENAMES]


def _walk_segment_items(yaml_dir, wanted_type):
    """Yield (yaml_stem, symbol, offset, count_or_None, segment_id, rom_base) for every entry of
    `wanted_type` ("GFX" or "VTX") in a yaml carrying a `:config: segments:` block -- the SAME
    segment/rom_base resolution walk_textures() in gen_dump_all.py uses for TEXTURE entries.
    GFX/VTX never appear in common_assets_compressed.yaml (verified: W0_CONSUMER_CONTRACT.md §1
    lists common_assets_compressed as OTEX/OBLB only), so no "is_common" branch is needed here.
    """
    out = []
    for path in _yaml_dir_files(yaml_dir):
        with open(path) as fh:
            data = yaml.safe_load(fh) or {}
        config = data.get(":config", {}) or {}
        segs = config.get("segments") or []
        if not (segs and isinstance(segs[0], (list, tuple)) and len(segs[0]) >= 2):
            continue
        segment_id = int(segs[0][0])
        rom_base = int(segs[0][1])
        yaml_stem = os.path.splitext(os.path.basename(path))[0]
        for key, val in data.items():
            if not isinstance(val, dict) or str(key).startswith(":"):
                continue
            if val.get("type") != wanted_type:
                continue
            if val.get("offset") is None:
                continue
            sym = val.get("symbol", key)
            count = val.get("count")
            out.append((yaml_stem, sym, int(val["offset"]), count, segment_id, rom_base))
    return out


# ── shared: F3DEX2 opcode mnemonic table (top byte of w0) ──────────────────────────────────────────
# decomp/config.yml declares `gbi: F3DEX2` for every profile -- this is the standard libultra
# gbi.h F3DEX2 opcode table (SP commands 0x00-0x08, DP/other commands 0xD3-0xFF); cross-checked
# against torch/src/factories/DisplayListFactory.cpp's gF3DEx2Table (VTX=0x01, DL=0xDE, MTX=0xDA,
# ENDDL=0xDF, SETTIMG=0xFD, MOVEMEM=0xDC, TRI2=0x06, QUAD=0x07 -- all match below).
F3DEX2_MNEMONICS = {
    0x00: "G_NOOP", 0x01: "G_VTX", 0x02: "G_MODIFYVTX", 0x03: "G_CULLDL", 0x04: "G_BRANCH_Z",
    0x05: "G_TRI1", 0x06: "G_TRI2", 0x07: "G_QUAD", 0x08: "G_LINE3D",
    0xD6: "G_DMA_IO", 0xD7: "G_TEXTURE", 0xD8: "G_POPMTX", 0xD9: "G_GEOMETRYMODE", 0xDA: "G_MTX",
    0xDB: "G_MOVEWORD", 0xDC: "G_MOVEMEM", 0xDD: "G_LOAD_UCODE", 0xDE: "G_DL", 0xDF: "G_ENDDL",
    0xE0: "G_SPNOOP", 0xE1: "G_RDPHALF_1", 0xE2: "G_SETOTHERMODE_L", 0xE3: "G_SETOTHERMODE_H",
    0xE4: "G_TEXRECT", 0xE5: "G_TEXRECTFLIP", 0xE6: "G_RDPLOADSYNC", 0xE7: "G_RDPPIPESYNC",
    0xE8: "G_RDPTILESYNC", 0xE9: "G_RDPFULLSYNC", 0xEA: "G_SETKEYGB", 0xEB: "G_SETKEYR",
    0xEC: "G_SETCONVERT", 0xED: "G_SETSCISSOR", 0xEE: "G_SETPRIMDEPTH", 0xEF: "G_RDPSETOTHERMODE",
    0xF0: "G_LOADTLUT", 0xF1: "G_RDPHALF_2", 0xF2: "G_SETTILESIZE", 0xF3: "G_LOADBLOCK",
    0xF4: "G_LOADTILE", 0xF5: "G_SETTILE", 0xF6: "G_FILLRECT", 0xF7: "G_SETFILLCOLOR",
    0xF8: "G_SETFOGCOLOR", 0xF9: "G_SETBLENDCOLOR", 0xFA: "G_SETPRIMCOLOR", 0xFB: "G_SETENVCOLOR",
    0xFC: "G_SETCOMBINE", 0xFD: "G_SETTIMG", 0xFE: "G_SETZIMG", 0xFF: "G_SETCIMG",
}
G_ENDDL = 0xDF
G_DL = 0xDE
G_DL_NOPUSH = 1  # decomp/include/PR/gbi.h:1039


# =====================================================================================================
# 1) coursedata -- Tier 1: 26 XCRS course records
# =====================================================================================================
class CourseDataDumpClass(DumpClass):
    """Source: generic.o2r `segment_blob/course_data` (BLOB framing, payload at OTR_HEADER_SIZE+4 =
    0x44 -- verified: entry file_size 52484 = 64 (OTR header) + 4 (u32 size subheader) + 52416
    (26 * 0x7E0), matching decomp/assets/yaml/us/rev0/segment_blob.yaml's documented
    `course_data: offset 0x2AD1E0 size 0xCCC0` span exactly). Struct layout: decomp/include/
    fzx_course.h:14-34 (CourseData, sizeof 0x7E0), all fields big-endian (raw ROM bytes, no
    Torch relocation -- BLOB entries are byte-identical rom slices, unlike GFX/VTX -- see
    DListDumpClass). Slot order confirmed against decomp/assets/yaml/us/rev0/course_data.yaml's
    26 FZX:COURSE entries, sorted by `offset`: monotonic +0x7E0 starting at 0x2AD1E0, i.e. slot N
    == course_data.yaml's Nth entry by ascending offset (verified programmatically, see PR notes).

    `flag` (u8 @0x008) and `trackSegmentInfo` (s32 in each ControlPoint) are the two fields without
    a fully-resolved semantic in this pass: `flag`'s bit meaning is not documented anywhere in the
    decomp tree, so it is emitted as a raw hex byte, honestly labeled "unk_meaning". Every
    trackSegmentInfo IS decodable, via the TRACK_* bit-layout macros fzx_course.h:307-351 define
    (type/shape/join/chunk-join/form/flags), so it gets a typed breakdown.
    """
    name = "coursedata"
    subdir = "coursedata"

    VENUE_NAMES = ["VENUE_MUTE_CITY", "VENUE_PORT_TOWN", "VENUE_BIG_BLUE", "VENUE_SAND_OCEAN",
                   "VENUE_DEVILS_FOREST", "VENUE_WHITE_LAND", "VENUE_SECTOR", "VENUE_RED_CANYON",
                   "VENUE_FIRE_FIELD", "VENUE_SILENCE", "VENUE_ENDING"]
    SKYBOX_NAMES = ["SKYBOX_PURPLE", "SKYBOX_TURQUOISE", "SKYBOX_DESERT", "SKYBOX_BLUE",
                    "SKYBOX_NIGHT", "SKYBOX_ORANGE", "SKYBOX_SUNSET", "SKYBOX_SKY_BLUE"]

    TRACK_SHAPE_NAMES = ["ROAD", "WALLED_ROAD", "PIPE", "CYLINDER", "HALF_PIPE", "TUNNEL", "AIR",
                         "BORDERLESS_ROAD"]

    @staticmethod
    def _name_or_unk(names, idx):
        if 0 <= idx < len(names):
            return names[idx]
        return "unk_%d" % idx

    @classmethod
    def _decode_track_segment_info(cls, v):
        uv = v & 0xFFFFFFFF
        shape_idx = (uv & 0x1C0) >> 6
        flags = []
        for bit, name in ((0x8000000, "FLAG_8000000"), (0x10000000, "JOINABLE"),
                          (0x20000000, "INSIDE"), (0x40000000, "CONTINUOUS"),
                          (0x80000000, "FLAG_80000000")):
            if uv & bit:
                flags.append(name)
        return {
            "raw": "0x%08X" % uv,
            "type": uv & 0x3F if (uv & 0x3F) != 0x3F else "NONE",
            "shapeIndex": shape_idx,
            "shapeName": cls._name_or_unk(cls.TRACK_SHAPE_NAMES, shape_idx),
            "join": (uv & 0x600) >> 9,        # 0 none,1 prev,2 next,3 both
            "chunkJoinEnd": (uv & 0x1800) >> 11,
            "chunkJoinStart": (uv & 0x6000) >> 13,
            "form": (uv & 0x38000) >> 15,     # 0 straight,1 left,2 right,3 s,4 s_flipped
            "flags": flags,
        }

    def _decode_course(self, raw, slot, symbol, rom_offset):
        u8 = lambda o: raw[o]
        s8 = lambda o: struct.unpack_from(">b", raw, o)[0]
        u32 = lambda o: struct.unpack_from(">I", raw, o)[0]

        creator_id = u8(0)
        cp_count = s8(1)
        venue = s8(2)
        skybox = s8(3)
        checksum = u32(4)
        flag = u8(8)
        file_name = raw[9:9 + 22].split(b"\x00", 1)[0].decode("ascii", "replace")
        bgm = s8(0x1F)

        control_points = []
        for i in range(64):
            base = 0x20 + i * 0x14
            x, y, z = struct.unpack_from(">fff", raw, base)
            radius_left, radius_right = struct.unpack_from(">hh", raw, base + 0x0C)
            tsi = struct.unpack_from(">i", raw, base + 0x10)[0]
            cp = {"pos": [x, y, z], "radiusLeft": radius_left, "radiusRight": radius_right,
                  "trackSegmentInfo": self._decode_track_segment_info(tsi)}
            control_points.append(cp)

        def s8_array(base):
            return list(struct.unpack_from(">64b", raw, base))

        bank_angle = list(struct.unpack_from(">64h", raw, 0x520))

        return {
            "symbol": symbol, "slot": slot, "romOffset": "0x%X" % rom_offset,
            "creatorId": creator_id, "creatorIsNintendo": creator_id == 4,
            "controlPointCount": cp_count,
            "venue": venue, "venueName": self._name_or_unk(self.VENUE_NAMES, venue),
            "skybox": skybox, "skyboxName": self._name_or_unk(self.SKYBOX_NAMES, skybox),
            "checksum": "0x%08X" % checksum,
            "flag": flag, "flag_unk_meaning": True,
            "fileName": file_name,
            "bgm": bgm,
            "controlPoints": control_points[:max(cp_count, 0)],
            "controlPointsRawCount": 64,
            "note": "controlPoints[] truncated to controlPointCount; bankAngle/pit/dash/dirt/ice/"
                    "jump/landmine/gate/building/sign are the full 64-slot raw ROM arrays -- only "
                    "indices < controlPointCount are meaningful, the rest is stale/padding ROM data.",
            "bankAngle": bank_angle,
            "pit": s8_array(0x5A0), "dash": s8_array(0x5E0), "dirt": s8_array(0x620),
            "ice": s8_array(0x660), "jump": s8_array(0x6A0), "landmine": s8_array(0x6E0),
            "gate": s8_array(0x720), "building": s8_array(0x760), "sign": s8_array(0x7A0),
        }

    def run(self, ctx):
        zf = _archive_zip(ctx)
        key = "segment_blob/course_data"
        if key not in zf.namelist():
            print("  coursedata: %s not found in archive -- skipping" % key)
            return {"class": self.name, "dumped": 0, "skipped": 0, "failed": 1, "total": 0}
        data = zf.read(key)
        payload = data[OTR_HEADER_SIZE + 4:]  # 64-byte OTR header + 4-byte BLOB size subheader
        stride = 0x7E0
        expected = 26 * stride
        if len(payload) != expected:
            sys.stderr.write("  warn: coursedata payload is %d bytes, expected %d (26*0x7E0)\n"
                             % (len(payload), expected))

        yaml_dir, _ = gab.resolve_paths(ctx.profile, None)
        cd_path = os.path.join(yaml_dir, "course_data.yaml")
        with open(cd_path) as fh:
            cd_yaml = yaml.safe_load(fh) or {}
        entries = sorted(
            ((v["offset"], k, v.get("symbol", k)) for k, v in cd_yaml.items()
             if isinstance(v, dict) and v.get("type") == "FZX:COURSE"),
        )

        out_dir = self.out_dir(ctx)
        os.makedirs(out_dir, exist_ok=True)
        manifest_rows = []
        dumped = skipped = failed = 0
        for slot, (rom_offset, _key, symbol) in enumerate(entries):
            bin_path = os.path.join(out_dir, symbol + ".bin")
            json_path = os.path.join(out_dir, symbol + ".json")
            if os.path.exists(bin_path) and os.path.exists(json_path):
                skipped += 1
                manifest_rows.append((symbol, slot, "0x%X" % rom_offset, "skip-existing"))
                continue
            raw = payload[slot * stride:(slot + 1) * stride]
            if len(raw) != stride:
                sys.stderr.write("  warn: coursedata slot %d (%s) short read\n" % (slot, symbol))
                failed += 1
                continue
            with open(bin_path, "wb") as fh:
                fh.write(raw)
            course = self._decode_course(raw, slot, symbol, rom_offset)
            with open(json_path, "w", encoding="utf-8") as fh:
                json.dump(course, fh, indent=2)
            dumped += 1
            manifest_rows.append((symbol, slot, "0x%X" % rom_offset, "ok"))

        _write_manifest(os.path.join(out_dir, "manifest.tsv"),
                        "symbol\tslot\tromOffset\tstatus   (26 CourseData records, fzx_course.h)",
                        manifest_rows)
        print("  coursedata: %d dumped, %d skipped, %d failed (of %d)"
              % (dumped, skipped, failed, len(entries)))
        return {"class": self.name, "dumped": dumped, "skipped": skipped, "failed": failed,
                "total": len(entries)}


# =====================================================================================================
# 2) dlists -- Tier 1: raw display-list dumps + light F3DEX2 disassembly
# =====================================================================================================
class DListDumpClass(DumpClass):
    """Type vocabulary surprise: the yamls do NOT use "DL"/"DLIST" -- Torch's own recipe type name
    is plain `GFX` (confirmed: 203 GFX-typed entries across the us/rev0 tree, alongside 1563 VTX,
    1173 TEXTURE, 419 COMPRESSED_TEXTURE, 89 BLOB, 62 ARRAY, 26 FZX:COURSE, 24 FZX:GHOST, 14 VP,
    2 FZX:SOUNDFONT, 2 FZX:SEQUENCE).

    SOURCE CHOICE -- raw ROM, not the o2r archive: generic.o2r's GFX entries are ODLT resources
    (torch/src/factories/DisplayListFactory.cpp::DListBinaryExporter::Export) whose command stream
    is OTR-RELOCATED, not the original N64 Gfx words: for G_VTX/G_DL/G_MOVEMEM/G_SETTIMG/G_MTX
    opcodes specifically, the exporter rewrites the opcode to a Torch OTR marker and splices in an
    extra 8-byte CRC64 path-hash pair right after -- a naive fixed-8-bytes-per-instruction scan
    would desync at every such opcode (variable-length rewriting keyed by opcode, not declared
    anywhere in the entry). The RAW ROM bytes at (segment rom_base + yaml offset) are the original,
    un-relocated F3DEX2 command stream and decode with a trivial fixed-stride mnemonic table --
    verified end-to-end against hud_gfx/aSetupBoosterDL (ROM 0x1C0468): GEOMETRYMODE, TEXTURE, MTX,
    RDPPIPESYNC, SETOTHERMODE_H/L, SETCOMBINE, SETPRIMCOLOR, SETTIMG, SETTILE, RDPLOADSYNC,
    LOADBLOCK, SETTILESIZE, ENDDL -- a clean, complete HUD-icon display list.

    Length: GFX yaml entries carry only `offset` (no `size`/`count`), because Torch itself
    discovers each DL's extent by scanning forward from `offset` until G_ENDDL or a no-push
    G_DL branch (torch/src/factories/DisplayListFactory.cpp::parse, ~line 486-613). This class
    replicates that exact scan directly against ROM bytes (decomp/include/PR/gbi.h:1038-1039 for
    G_DL_PUSH/G_DL_NOPUSH), so raw .bin length always matches what Torch itself would have bound.
    """
    name = "dlists"
    subdir = "dlists"

    MAX_INSTRUCTIONS = 8192  # safety cap (65536 bytes); no known F-Zero X DL is anywhere near this

    def _scan(self, rom, addr):
        words = []
        i = 0
        a = addr
        while i < self.MAX_INSTRUCTIONS and a + 8 <= len(rom):
            w0, w1 = struct.unpack_from(">II", rom, a)
            words.append((a, w0, w1))
            opcode = (w0 >> 24) & 0xFF
            a += 8
            i += 1
            if opcode == G_ENDDL:
                break
            if opcode == G_DL and ((w0 >> 16) & G_DL_NOPUSH) == G_DL_NOPUSH:
                break
        return words

    def run(self, ctx):
        rom, rom_path = _read_rom()
        if rom is None:
            print("  dlists: no baserom found (checked %s) -- skipping"
                  % ", ".join(os.path.relpath(p, REPO) for p in _ROM_CANDIDATES))
            return {"class": self.name, "dumped": 0, "skipped": 0, "failed": 0, "total": 0}

        items = _walk_segment_items(ctx.yaml_dir, "GFX")
        out_dir = self.out_dir(ctx)
        os.makedirs(out_dir, exist_ok=True)
        manifest_rows = []
        dumped = skipped = failed = 0
        for yaml_stem, sym, offset, _count, segment_id, rom_base in sorted(items, key=lambda it: (it[0], it[1])):
            addr = rom_base + offset
            key = "%s_%s" % (yaml_stem, sym)
            bin_path = os.path.join(out_dir, key + ".bin")
            txt_path = os.path.join(out_dir, key + ".txt")
            if os.path.exists(bin_path):
                skipped += 1
                nwords = os.path.getsize(bin_path) // 8
                manifest_rows.append((key, yaml_stem, sym, "0x%X" % addr, nwords,
                                      os.path.getsize(bin_path), "skip-existing"))
                continue
            if addr < 0 or addr + 8 > len(rom):
                sys.stderr.write("  warn: dlists %s: rom addr 0x%X out of range\n" % (key, addr))
                failed += 1
                continue
            words = self._scan(rom, addr)
            raw = b"".join(struct.pack(">II", w0, w1) for _a, w0, w1 in words)
            with open(bin_path, "wb") as fh:
                fh.write(raw)
            with open(txt_path, "w", encoding="utf-8", newline="\n") as fh:
                fh.write("# %s (yaml=%s symbol=%s) romAddr=0x%X segment=%d words=%d bytes=%d\n"
                         % (key, yaml_stem, sym, addr, segment_id, len(words), len(raw)))
                fh.write("# offset(rel)  w0        w1        mnemonic\n")
                for a, w0, w1 in words:
                    op = (w0 >> 24) & 0xFF
                    mnem = F3DEX2_MNEMONICS.get(op, "op_%02X" % op)
                    fh.write("0x%06X  %08X  %08X  %s\n" % (a - addr, w0, w1, mnem))
            dumped += 1
            manifest_rows.append((key, yaml_stem, sym, "0x%X" % addr, len(words), len(raw), "ok"))

        _write_manifest(os.path.join(out_dir, "manifest.tsv"),
                        "key\tyaml\tsymbol\tromAddr\twords\tbytes\tstatus   (raw F3DEX2 DLs from %s)"
                        % os.path.relpath(rom_path, REPO),
                        manifest_rows)
        print("  dlists: %d dumped, %d skipped, %d failed (of %d GFX entries)"
              % (dumped, skipped, failed, len(items)))
        return {"class": self.name, "dumped": dumped, "skipped": skipped, "failed": failed,
                "total": len(items)}


# =====================================================================================================
# 3) vertexdata -- Tier 1: raw VTX dumps + bounding-box JSON summary
# =====================================================================================================
class VertexDataDumpClass(DumpClass):
    """yaml VTX entries carry an explicit `count` (vertex count; unlike GFX, no scan needed).
    N64Vtx_t is 16 bytes, big-endian: s16 x,y,z; u16 flag; s16 tu,tv (1/32 texel units);
    u8 r,g,b,a (color OR packed normal, format-dependent at draw time -- not disambiguated here).
    Verified against hud_gfx/D_400A4A8 (ROM 0x1C0BC0, count 12): decodes to a small, plausible
    HUD-icon-scale bounding box (x:[145,166] y:[-61,61] z:[-30,15]).

    Same raw-ROM source-choice rationale as DListDumpClass: vertices carry no pointers so Torch's
    OVTX archive copy is likely closer to a straight byte copy, but reading directly from ROM
    keeps this class independent of that assumption and matches the DL class's source for
    consistency (a future model-export Tier-2 pass can cross-check both).
    """
    name = "vertexdata"
    subdir = "vertexdata"

    def run(self, ctx):
        rom, rom_path = _read_rom()
        if rom is None:
            print("  vertexdata: no baserom found -- skipping")
            return {"class": self.name, "dumped": 0, "skipped": 0, "failed": 0, "total": 0}

        items = _walk_segment_items(ctx.yaml_dir, "VTX")
        out_dir = self.out_dir(ctx)
        os.makedirs(out_dir, exist_ok=True)
        manifest_rows = []
        dumped = skipped = failed = 0
        for yaml_stem, sym, offset, count, segment_id, rom_base in sorted(items, key=lambda it: (it[0], it[1])):
            if not count:
                failed += 1
                continue
            addr = rom_base + offset
            key = "%s_%s" % (yaml_stem, sym)
            bin_path = os.path.join(out_dir, key + ".bin")
            json_path = os.path.join(out_dir, key + ".json")
            if os.path.exists(bin_path) and os.path.exists(json_path):
                skipped += 1
                manifest_rows.append((key, yaml_stem, sym, "0x%X" % addr, count,
                                      os.path.getsize(bin_path), "skip-existing"))
                continue
            nbytes = int(count) * 16
            if addr < 0 or addr + nbytes > len(rom):
                sys.stderr.write("  warn: vertexdata %s: rom range out of bounds\n" % key)
                failed += 1
                continue
            raw = rom[addr:addr + nbytes]
            with open(bin_path, "wb") as fh:
                fh.write(raw)
            xs, ys, zs = [], [], []
            for i in range(int(count)):
                x, y, z = struct.unpack_from(">hhh", raw, i * 16)
                xs.append(x)
                ys.append(y)
                zs.append(z)
            summary = {
                "symbol": sym, "yaml": yaml_stem, "romAddr": "0x%X" % addr, "segment": segment_id,
                "count": int(count), "bytes": nbytes,
                "bbox": {"x": [min(xs), max(xs)], "y": [min(ys), max(ys)], "z": [min(zs), max(zs)]},
            }
            with open(json_path, "w", encoding="utf-8") as fh:
                json.dump(summary, fh, indent=2)
            dumped += 1
            manifest_rows.append((key, yaml_stem, sym, "0x%X" % addr, count, nbytes, "ok"))

        _write_manifest(os.path.join(out_dir, "manifest.tsv"),
                        "key\tyaml\tsymbol\tromAddr\tcount\tbytes\tstatus   (raw N64Vtx_t[] from %s)"
                        % os.path.relpath(rom_path, REPO),
                        manifest_rows)
        print("  vertexdata: %d dumped, %d skipped, %d failed (of %d VTX entries)"
              % (dumped, skipped, failed, len(items)))
        return {"class": self.name, "dumped": dumped, "skipped": skipped, "failed": failed,
                "total": len(items)}


# =====================================================================================================
# 4) tables -- Tier 1: sequence/soundfont table dumps
# =====================================================================================================
class TablesDumpClass(DumpClass):
    """Two distinct things share the name "table" here, and both are emitted:

    (a) RAW REGION DUMPS: generic.o2r `audio_blob/audio_bank` (BLOB, payload 11664 = 0x2D90 bytes)
        and `audio_blob/audio_seq` (BLOB, payload 3136 = 0xC40 bytes) -- these are the CONCATENATED
        soundfont/sequence binary DATA regions the AudioTableEntry.romAddr offsets point into
        (decomp/assets/yaml/us/rev0/audio_blob.yaml:19-27), NOT a serialized AudioTableHeader
        struct -- confirmed empirically: interpreting either payload's first bytes as
        AudioTableHeader (decomp/src/audio/disk/lib/audio.h:732-737: s16 numEntries, s16 diskLba,
        u32 romAddr, 8B pad) yields nonsense (numEntries in the thousands/negative).

    (b) PARSED TABLE JSON: gSoundFontTable / gSequenceTable (decomp/src/audio/disk/audio_tables.c)
        ARE the real AudioTable header+entries (23 entries each, FONT_MAX==SEQ_MAX==23 -- decomp/
        include/sfx.h:162,194) -- they are literal C initializers already reconstructed by the
        decomp project (compiled into the game's .data section on real hardware, never present as
        raw archive bytes), so this pass transcribes them directly from source via a small regex
        parser rather than guessing a binary layout. AudioTableEntry layout (audio.h:739-747) is
        included as a comment for readers cross-checking against AudioLoad_InitTable
        (decomp/src/audio/disk/lib/load.c:339-350, which just adds table->header.romAddr onto
        every CART-medium entry's romAddr at init -- not reproduced here, offsets are emitted as
        declared in source).
    """
    name = "tables"
    subdir = "tables"

    _ROW_RE = re.compile(r"\{([^{}]*)\}\s*,?\s*/\*\s*([A-Za-z0-9_]+)\s*\*/")

    @classmethod
    def _parse_c_table(cls, text, var_name):
        m = re.search(r"AudioTable\s+" + re.escape(var_name) + r"\s*=\s*\{\s*\{([^}]*)\}\s*,\s*\{(.*?)\}\s*\}\s*;",
                      text, re.S)
        if not m:
            return None
        header_text = m.group(1).strip()
        body_text = m.group(2)
        rows = []
        for fields_text, name in cls._ROW_RE.findall(body_text):
            fields = [f.strip() for f in fields_text.split(",")]
            # fields: [offset, size, medium, cachePolicy, col5, col6, col7] -- col5-7 are often
            # bit-packed expressions (e.g. "(SAMPLE_GUITAR << 8) | 0xFF"); kept as raw source text
            # ("parse conservatively" -- resolving every enum would require pulling in sfx.h's
            # SAMPLE_*/FONT_* value tables, out of scope for a Tier-1 raw+typed dump).
            while len(fields) < 7:
                fields.append(None)
            rows.append({
                "name": name,
                "offset": fields[0], "size": fields[1],
                "medium": fields[2], "cachePolicy": fields[3],
                "col5_raw": fields[4], "col6_raw": fields[5], "col7_raw": fields[6],
            })
        return {"headerRaw": header_text, "numEntries": len(rows), "entries": rows}

    def run(self, ctx):
        zf = _archive_zip(ctx)
        out_dir = self.out_dir(ctx)
        os.makedirs(out_dir, exist_ok=True)
        manifest_rows = []
        dumped = skipped = failed = 0

        for key, fname in (("audio_blob/audio_bank", "audio_bank.bin"),
                           ("audio_blob/audio_seq", "audio_seq.bin")):
            if key not in zf.namelist():
                failed += 1
                continue
            out_path = os.path.join(out_dir, fname)
            if os.path.exists(out_path):
                skipped += 1
                manifest_rows.append((fname, key, os.path.getsize(out_path), "raw-region-skip"))
                continue
            data = zf.read(key)
            payload = data[OTR_HEADER_SIZE + 4:]  # BLOB framing, same as coursedata
            with open(out_path, "wb") as fh:
                fh.write(payload)
            dumped += 1
            manifest_rows.append((fname, key, len(payload), "raw-region"))

        tables_c = os.path.join(REPO, "decomp", "src", "audio", "disk", "audio_tables.c")
        if os.path.isfile(tables_c):
            with open(tables_c, encoding="utf-8") as fh:
                text = fh.read()
            for var_name, out_name in (("gSoundFontTable", "soundfont_table.json"),
                                       ("gSequenceTable", "sequence_table.json")):
                out_path = os.path.join(out_dir, out_name)
                if os.path.exists(out_path):
                    skipped += 1
                    with open(out_path, encoding="utf-8") as fh:
                        prev = json.load(fh)
                    manifest_rows.append((out_name, var_name, prev.get("numEntries", "?"),
                                          "parsed-source-skip"))
                    continue
                parsed = self._parse_c_table(text, var_name)
                if parsed is None:
                    failed += 1
                    continue
                parsed["source"] = "decomp/src/audio/disk/audio_tables.c (%s)" % var_name
                parsed["note"] = ("Compiled-in AudioTable initializer, transcribed from decomp "
                                  "source -- not present as raw bytes in generic.o2r (audio_bank/"
                                  "audio_seq are placeholder-sized DATA regions, not this header/"
                                  "entries struct; see class docstring).")
                with open(out_path, "w", encoding="utf-8") as fh:
                    json.dump(parsed, fh, indent=2)
                dumped += 1
                manifest_rows.append((out_name, var_name, parsed["numEntries"], "parsed-source"))
        else:
            failed += 1

        _write_manifest(os.path.join(out_dir, "manifest.tsv"),
                        "file\tsource\tsize_or_entries\tkind",
                        manifest_rows)
        print("  tables: %d dumped, %d skipped, %d failed" % (dumped, skipped, failed))
        return {"class": self.name, "dumped": dumped, "skipped": skipped, "failed": failed,
                "total": dumped + skipped + failed}


# =====================================================================================================
# 5) ghosts -- Tier 2: staff ghost records -> raw+JSON, and .gdg where the format actually fits
# =====================================================================================================
class GhostDumpClass(DumpClass):
    """generic.o2r `staff_ghost_records/*` entries are FZX:GHOST (fourcc XGRD) resources, and their
    ARCHIVE byte layout is torch/src/factories/fzerox/GhostRecordFactory.cpp's
    GhostRecordBinaryExporter::Export OUTPUT format -- verified byte-for-byte against
    staff_ghost_records/aMuteCity1StaffGhost: payload starts immediately at the 64-byte OTR header
    boundary (no extra sub-header, unlike BLOB/ODLT -- WriteHeader() always pads to exactly 0x40
    and the exporter writes its first field right after), all multi-byte fields LITTLE-ENDIAN (the
    exporter never calls SetEndianness(Big), unlike GhostRecordFactory::parse which reads the
    original big-endian ROM bytes -- these are two DIFFERENT layouts of the same logical data; the
    archive uses the write-side one). Field order: u16 ghostType, s32 courseEncoding, s32 raceTime,
    u16 unk10, u32 trackNameLen, trackName[trackNameLen], 20B MachineInfo (character..cockpitB,
    decomp/include/unk_structs.h:43-64), s32 lapTimes[3], s32 replayEnd, u32 replaySize, u32
    replayDataLen (redundant with replaySize), replayData[replayDataLen]. Decoded fields for
    aMuteCity1StaffGhost sanity-checked: ghostType=2 (GHOST_STAFF), courseEncoding&0x1F==0 (Mute
    City, course index 0 -- matches the symbol), replaySize==remaining bytes exactly, MachineInfo
    numberR/G/B==0x64 (plausible default gray).

    .gdg DECISION -- raw+JSON only, NOT wrapped in gdx_ghost_io's GDG1 container. Evidence:
      1. port/gdx_ghost_io.h's own docstring (lines 14-17) states its scope is "base-course PLAYER
         ghosts" and explicitly excludes "the 64DD/Expansion-Kit per-course ghost cache ... out of
         scope for this ticket" -- staff ghosts are exactly that excluded cache (loaded via
         Save_LoadStaffGhost_impl / Save_RomCopyGhostRecord+Save_RomCopyGhostData, save.c:2345-2425).
      2. GDG1's payload is a FIXED 0x3FC0 bytes (GhostRecord 0x40 + GhostData 0x3F80, GhostData.
         replayData always the full 16200-byte buffer, zero-padded) -- but the archive's staff-ghost
         entries are tightly variable-length (5669..11975 bytes across the 24 courses), confirming
         they were never meant to round-trip through the fixed-size SRAM-slot container.
      3. The archive format has NO checksum fields at all (GhostRecordBinaryExporter::Export never
         writes mRecordChecksum/mReplayChecksum/mDataChecksum -- confirmed by reading its full body),
         while GDG1 import REQUIRES GhostRecord.checksum == Save_CalculateGhostRecordChecksum(record)
         and GhostData.replayInfo.checksum == Save_CalculateGhostDataChecksum(data) (gdx_ghost_io.h
         lines 72-76) or the import is rejected (GDX_GHOST_ERR_BAD_CHECKSUM).
    Despite (3), the checksum algorithm IS plainly available and portable: decomp/src/overlays/
    ovl_i2/save.c:2070-2078 `Save_CalculateChecksum` is a trivial additive byte-sum truncated to
    u16, and save.c:2110-2115 pin down exactly which byte ranges each checksum covers (GhostRecord:
    bytes [2:0x40); GhostData: bytes [2:0x3F80), i.e. covering the WHOLE fixed replay buffer
    including whatever padding fills the tail beyond replaySize). Because I choose that padding
    (zero-fill) when constructing the .gdg payload myself, the checksum is self-consistent and
    WILL validate under gdx_ghost_io's import check -- so a best-effort .gdg IS emitted per ghost,
    alongside the raw/JSON (which remain the authoritative, format-faithful dump). Byte-swap field
    list cross-checked against the port's own GhostRecord_FromRom/GhostData_FromRom (save.c:2038-
    2067, written for the raw-ROM path but documents exactly which fields are multi-byte).
    """
    name = "ghosts"
    subdir = "ghosts"

    GDG_MAGIC = b"GDG1"
    GDG_VERSION = 1
    GDG_PAYLOAD_SIZE = 0x40 + 0x3F80  # 0x3FC0

    @staticmethod
    def _checksum16(data):
        return sum(data) & 0xFFFF

    @staticmethod
    def _replay_fingerprint(replay_data):
        """Ports FZX::GhostRecordData::CalculateReplayChecksum (torch/src/factories/fzerox/
        GhostRecordFactory.cpp:52-69) -- NOT import-validated by gdx_ghost_io, included only for
        payload completeness (GhostRecord.replayChecksum would otherwise be left 0, since the
        archive format doesn't carry it either)."""
        checksum = 0
        total = 0
        i = 0
        for b in replay_data:
            total += b << ((3 - i) * 8)
            i = (i + 1) % 4
            if i == 0:
                checksum += total
                total = 0
        return checksum & 0xFFFFFFFF

    def _parse_archive_entry(self, raw_payload):
        p = raw_payload
        off = 0
        ghost_type = struct.unpack_from("<H", p, off)[0]; off += 2
        course_encoding = struct.unpack_from("<i", p, off)[0]; off += 4
        race_time = struct.unpack_from("<i", p, off)[0]; off += 4
        unk10 = struct.unpack_from("<H", p, off)[0]; off += 2
        track_name_len = struct.unpack_from("<I", p, off)[0]; off += 4
        track_name = p[off:off + track_name_len]; off += track_name_len
        machine_info = p[off:off + 20]; off += 20
        lap_times = struct.unpack_from("<3i", p, off); off += 12
        replay_end = struct.unpack_from("<i", p, off)[0]; off += 4
        replay_size = struct.unpack_from("<I", p, off)[0]; off += 4
        replay_data_len = struct.unpack_from("<I", p, off)[0]; off += 4
        replay_data = p[off:off + replay_data_len]; off += replay_data_len
        return {
            "ghostType": ghost_type, "courseEncoding": course_encoding, "raceTime": race_time,
            "unk10": unk10, "trackName": track_name.decode("ascii", "replace"),
            "machineInfo": {
                "character": machine_info[0], "customType": machine_info[1],
                "frontType": machine_info[2], "rearType": machine_info[3],
                "wingType": machine_info[4], "logo": machine_info[5], "number": machine_info[6],
                "decal": machine_info[7], "bodyR": machine_info[8], "bodyG": machine_info[9],
                "bodyB": machine_info[10], "numberR": machine_info[11], "numberG": machine_info[12],
                "numberB": machine_info[13], "decalR": machine_info[14], "decalG": machine_info[15],
                "decalB": machine_info[16], "cockpitR": machine_info[17], "cockpitG": machine_info[18],
                "cockpitB": machine_info[19],
            },
            "machineInfoRaw": machine_info.hex(),
            "lapTimes": list(lap_times), "replayEnd": replay_end, "replaySize": replay_size,
            "replayData": replay_data, "consumedBytes": off, "totalBytes": len(p),
        }

    def _build_gdg(self, parsed):
        machine_info = parsed["machineInfoRaw_bytes"]
        replay_data = parsed["replayData"]
        if len(replay_data) > 16200:
            return None, "replaySize %d exceeds the 16200-byte replay buffer" % len(replay_data)

        # -- GhostRecord (0x40) --
        record = bytearray(0x40)
        struct.pack_into("<H", record, 0, 0)  # checksum, filled below
        struct.pack_into("<H", record, 2, parsed["ghostType"])
        replay_fp = self._replay_fingerprint(replay_data)
        if replay_fp >= 0x80000000:
            replay_fp -= 0x100000000
        struct.pack_into("<i", record, 4, replay_fp)  # replayChecksum (not import-validated)
        struct.pack_into("<i", record, 8, parsed["courseEncoding"])
        struct.pack_into("<i", record, 0xC, parsed["raceTime"])
        struct.pack_into("<H", record, 0x10, parsed["unk10"])
        # unk_12[5] left zero -- "padding, not serialized" (save.c:2262-2264)
        name_bytes = parsed["trackName"].encode("ascii", "replace")[:9]
        record[0x17:0x17 + len(name_bytes)] = name_bytes
        record[0x20:0x20 + 20] = machine_info
        # unk_20.unk_14[12] left zero -- "not serialized" (save.c:2278-2280)
        record_checksum = self._checksum16(bytes(record[2:0x40]))
        struct.pack_into("<H", record, 0, record_checksum)

        # -- GhostData (0x3F80) --
        data = bytearray(0x3F80)
        struct.pack_into("<H", data, 0, 0)  # replayInfo.checksum, filled below
        struct.pack_into("<h", data, 2, 0)  # replayInfo.unk_02
        for i, lap in enumerate(parsed["lapTimes"]):
            struct.pack_into("<i", data, 4 + i * 4, lap)
        struct.pack_into("<i", data, 0x10, parsed["replayEnd"])
        struct.pack_into("<I", data, 0x14, parsed["replaySize"])
        # unk_18/unk_1C left zero
        data[0x20:0x20 + len(replay_data)] = replay_data
        # remaining replayData tail + unk_3F68[0x18] left zero
        data_checksum = self._checksum16(bytes(data[2:0x3F80]))
        struct.pack_into("<H", data, 0, data_checksum)

        payload = bytes(record) + bytes(data)
        course_id = parsed["courseEncoding"] & 0x1F
        crc = __import__("zlib").crc32(payload) & 0xFFFFFFFF
        header = struct.pack("<4sIiII", self.GDG_MAGIC, self.GDG_VERSION, course_id,
                             self.GDG_PAYLOAD_SIZE, crc)
        return header + payload, None

    def run(self, ctx):
        zf = _archive_zip(ctx)
        names = sorted(n for n in zf.namelist() if n.startswith("staff_ghost_records/"))
        out_dir = self.out_dir(ctx)
        os.makedirs(out_dir, exist_ok=True)
        manifest_rows = []
        dumped = skipped = failed = 0

        for key in names:
            symbol = key.split("/", 1)[1]
            bin_path = os.path.join(out_dir, symbol + ".bin")
            json_path = os.path.join(out_dir, symbol + ".json")
            gdg_path = os.path.join(out_dir, symbol + ".gdg")
            already_done = os.path.exists(bin_path) and os.path.exists(json_path)

            if already_done:
                # Idempotent rerun: bin/json already present -- still (re-)derive `parsed` from the
                # JSON (not the archive) so a missing .gdg from an interrupted prior run gets
                # repaired, and so the manifest always reflects every entry, not just this run's
                # newly-dumped ones.
                with open(json_path, encoding="utf-8") as fh:
                    saved = json.load(fh)
                parsed = dict(saved)
                parsed["replayData"] = bytes.fromhex(saved["replayData"])
                parsed["machineInfoRaw_bytes"] = bytes.fromhex(saved["machineInfoRaw"])
                skipped += 1
            else:
                entry = zf.read(key)
                raw_payload = entry[OTR_HEADER_SIZE:]  # verified: XGRD has NO extra subheader
                with open(bin_path, "wb") as fh:
                    fh.write(raw_payload)
                try:
                    parsed = self._parse_archive_entry(raw_payload)
                except (struct.error, IndexError) as exc:
                    sys.stderr.write("  warn: ghosts %s: parse failed (%s)\n" % (symbol, exc))
                    failed += 1
                    continue
                parsed["machineInfoRaw_bytes"] = bytes.fromhex(parsed["machineInfoRaw"])
                out_json = dict(parsed)
                out_json["replayData"] = out_json["replayData"].hex()
                del out_json["machineInfoRaw_bytes"]
                out_json["derivedCourseId"] = parsed["courseEncoding"] & 0x1F
                out_json["symbol"] = symbol
                with open(json_path, "w", encoding="utf-8") as fh:
                    json.dump(out_json, fh, indent=2)
                dumped += 1

            course_id = parsed["courseEncoding"] & 0x1F
            if os.path.exists(gdg_path):
                gdg_status = "ok" if already_done else "ok-skip"
            else:
                gdg_bytes, gdg_err = self._build_gdg(parsed)
                if gdg_bytes is None:
                    gdg_status = "skip: %s" % gdg_err
                else:
                    with open(gdg_path, "wb") as fh:
                        fh.write(gdg_bytes)
                    gdg_status = "ok" if not already_done else "ok-repaired"
            manifest_rows.append((symbol, parsed["ghostType"], "0x%X" % (parsed["courseEncoding"] & 0xFFFFFFFF),
                                  course_id, parsed["replaySize"], gdg_status))

        _write_manifest(os.path.join(out_dir, "manifest.tsv"),
                        "symbol\tghostType\tcourseEncoding\tderivedCourseId\treplaySize\tgdg_status",
                        manifest_rows)
        print("  ghosts: %d dumped, %d skipped, %d failed (of %d)"
              % (dumped, skipped, failed, len(names)))
        return {"class": self.name, "dumped": dumped, "skipped": skipped, "failed": failed,
                "total": len(names)}


# =====================================================================================================
# 6) fonts -- Tier 2: PNG sheets for the 64DD IPL fault font + kanji block
# =====================================================================================================
class FontDumpClass(DumpClass):
    """Glyph format derivation: decomp/src/sys/disk/leo_fault_dd.c's `LeoFault_CopyFontToRam` DMAs
    exactly 0x80 (128) bytes per glyph (`sLeoFontIoMsg.size = 0x80; // leo font size`), and
    `func_8070F3D4`'s software blit path reads it as 16 ROWS of 8 bytes (`for i in [posY,posY+16):
    for j in [posX,posX+16) step 2, fontPtr++`) with each byte's HIGH nibble = left pixel color
    index, LOW nibble = right pixel color index -- i.e. 16x16 pixels @ 4 bits/pixel, packed 2
    pixels/byte, no row padding (16/2 = 8 bytes/row * 16 rows = 128 = 0x80, exact). The 4-bit index
    is NOT the generic Fast3D I4 SCALE_4_8 ramp -- it indexes `sLeoFontPallete[16]` (leo_fault_dd.c:
    102-109), a custom non-linear grayscale ramp (0,16,32,48,64,80,96,112,136,152,168,184,200,216,
    232,255), used verbatim here as (v,v,v,255) RGBA8 (opaque; the real hardware pixel format is
    RGBA5551 with alpha forced to 1 for every visible palette entry per GPACK_RGBA5551(...,1)).

    Font block location: DDROM_FONT_START = 0xA0000 (decomp/include/PR/leo.h:139), 655360 bytes
    (== 5120 glyph slots * 0x80) -- read from the raw N64DD IPL ROM image (N64DDIPLROM.n64, the
    same file port/disk_buffer.cpp loads into gdx_ddipl_buffer at runtime; there is no
    n64ddipl.o2r in this checkout to source from instead -- R8 Step 1 lands that archive later,
    this pass reads the raw IPL ROM directly, which is the same bytes that archive would wrap).

    Glyph mapping (SJIS code -> font-block slot index) is LeoGetKAdr, shipped only as an incbin'd
    MIPS blob in decomp (decomp/src/leo/lib/getkadr.s) -- but port/n64_leo.c:347-381 is a full C
    port of that exact routine (disassembled from the US rev0 ROM, per its own header comment),
    reused here directly instead of re-deriving anything: kanji block (SJIS 0x8800-0x9872) is pure
    arithmetic (`(cell + 0x30A + row*0xBC) << 7`); symbol/kana block (SJIS 0x8140-0x87FF) resolves
    through a ROM-resident s16 index table, covered by generic.o2r's `segment_blob/kanji_tables`
    entry (decomp/assets/yaml/us/rev0/segment_blob.yaml:159-168, BLOB framing, payload at 0x44,
    exactly spanning GDX_ROM_KANJI_INDEX_TBL as n64_leo.c documents).
    """
    name = "fonts"
    subdir = "fonts"

    PALETTE = [0, 16, 32, 48, 64, 80, 96, 112, 136, 152, 168, 184, 200, 216, 232, 255]
    GLYPH_BYTES = 0x80
    FONT_BLOCK_OFFSET = 0xA0000  # DDROM_FONT_START, decomp/include/PR/leo.h:139
    FONT_BLOCK_SIZE = 655360
    KANJI_TABLE_ROM_BASE = 0x80960  # GDX_ROM_KANJI_INDEX_TBL, port/n64_leo.c:290

    # sLeoFontCharacters (leo_fault_dd.c:98-100), decoded from its octal-escaped C string literal.
    _FAULT_FONT_OCTAL = (
        r"\243\260\243\261\243\262\243\263\243\264\243\265\243\266\243\267\243\270\243\271"
        r"\245\250\245\351\241\274\310\326\271\346\274\350\260\267\300\342\314\300\275\361"
        r"\244\362\244\252\306\311\244\337\244\257\244\300\244\265\244\244\241\243\241\332"
        r"\303\355\260\325\241\333\245\242\245\257\245\273\245\271\245\363\245\327\305\300"
        r"\314\307\303\346\244\313\245\307\245\243\310\264\244\253\244\312\244\307\276\334"
        r"\244\267\244\317\241\242\272\271\271\376\244\363\244\306\264\326\260\343\244\303"
        r"\244\277\244\254\244\336\244\354\244\353\262\304\307\275\134\300\255\244\242\244"
        r"\352\244\271\300\265\270\362\264\271\265\257\306\260\273\376\244\316\275\320\301"
        r"\260\262\363\245\277\272\307\270\345\244\255\244\301\244\310\245\326\244\273\301"
        r"\264\276\303\243\301\245\334\262\241\245\262\245\340\245\263\244\341\245\352\245"
        r"\303\245\310\244\320\244\351\302\324\245\342\245\311\245\354\262\350\314\314\314\341"
    )

    @staticmethod
    def _decode_octal_escapes(s):
        out = bytearray()
        i = 0
        while i < len(s):
            if s[i] == "\\" and i + 3 < len(s) and s[i + 1:i + 4].isdigit():
                out.append(int(s[i + 1:i + 4], 8))
                i += 4
            else:
                out.append(ord(s[i]))
                i += 1
        return bytes(out)

    def _fault_font_codes(self):
        raw = self._decode_octal_escapes(self._FAULT_FONT_OCTAL)
        codes = []
        for i in range(0, len(raw) - 1, 2):
            codes.append((raw[i] << 8) | raw[i + 1])
        return codes[:110]

    @staticmethod
    def _get_k_adr(sjis, kanji_blob):
        """Port of port/n64_leo.c:347-381 LeoGetKAdr. Returns a byte offset into the font block,
        or None if unresolvable (mirrors the -1 sentinel)."""
        if sjis < 0x8140 or sjis >= 0x9873:
            return None
        cell = (sjis & 0xFF) - 0x40
        if cell >= 0x40:
            cell -= 1
        if sjis >= 0x8800:
            row = (sjis >> 8) - 0x88
            return (cell + 0x30A + row * 0xBC) << 7
        row = (sjis >> 8) - 0x81
        tbl_rel = (cell + row * 0xBC) * 2
        if tbl_rel < 0 or tbl_rel + 2 > len(kanji_blob):
            return None
        raw16 = struct.unpack_from(">h", kanji_blob, tbl_rel)[0]
        return raw16 << 7

    def _decode_glyph(self, block, slot):
        base = slot * self.GLYPH_BYTES
        cell = block[base:base + self.GLYPH_BYTES]
        if len(cell) < self.GLYPH_BYTES:
            return None
        out = bytearray(16 * 16 * 4)
        for byte_idx in range(64):
            b = cell[byte_idx]
            hi = (b >> 4) & 0xF
            lo = b & 0xF
            px0 = byte_idx * 2
            for nib, px in ((hi, px0), (lo, px0 + 1)):
                v = self.PALETTE[nib]
                o = px * 4
                out[o] = out[o + 1] = out[o + 2] = v
                out[o + 3] = 255
        return bytes(out)

    def _save_glyph(self, block, slot, path):
        rgba = self._decode_glyph(block, slot)
        if rgba is None:
            return False
        Image.frombytes("RGBA", (16, 16), rgba).save(path)
        return True

    def _save_sheet(self, block, slots, cols, path):
        rows = (len(slots) + cols - 1) // cols
        if rows == 0:
            return
        sheet = Image.new("RGBA", (cols * 16, rows * 16), (0, 0, 0, 0))
        for i, slot in enumerate(slots):
            rgba = self._decode_glyph(block, slot)
            if rgba is None:
                continue
            glyph = Image.frombytes("RGBA", (16, 16), rgba)
            sheet.paste(glyph, ((i % cols) * 16, (i // cols) * 16))
        sheet.save(path)

    def run(self, ctx):
        ipl, ipl_path = _read_ipl()
        if ipl is None:
            print("  fonts: no N64DDIPLROM.n64 found (checked %s) -- skipping"
                  % ", ".join(os.path.relpath(p, REPO) for p in _IPL_CANDIDATES))
            return {"class": self.name, "dumped": 0, "skipped": 0, "failed": 0, "total": 0}
        if len(ipl) < self.FONT_BLOCK_OFFSET + self.FONT_BLOCK_SIZE:
            print("  fonts: IPL image too small for the font block -- skipping")
            return {"class": self.name, "dumped": 0, "skipped": 0, "failed": 1, "total": 0}
        block = ipl[self.FONT_BLOCK_OFFSET:self.FONT_BLOCK_OFFSET + self.FONT_BLOCK_SIZE]
        num_slots = len(block) // self.GLYPH_BYTES

        zf = _archive_zip(ctx)
        kanji_blob = None
        if "segment_blob/kanji_tables" in zf.namelist():
            entry = zf.read("segment_blob/kanji_tables")
            kanji_blob = entry[OTR_HEADER_SIZE + 4:]  # BLOB framing

        out_dir = self.out_dir(ctx)
        fault_dir = os.path.join(out_dir, "fault")
        kanji_dir = os.path.join(out_dir, "kanji")
        os.makedirs(fault_dir, exist_ok=True)
        os.makedirs(kanji_dir, exist_ok=True)
        manifest_rows = []
        dumped = skipped = failed = 0

        # -- fault font (110 fixed glyphs) --
        fault_codes = self._fault_font_codes()
        fault_slots = []
        for idx, code in enumerate(fault_codes):
            slot = self._get_k_adr(code, kanji_blob) if kanji_blob else None
            slot_num = (slot // self.GLYPH_BYTES) if slot is not None else idx
            fault_slots.append(slot_num)
            png_path = os.path.join(fault_dir, "%03d_%04X.png" % (idx, code))
            row = ("fault/%03d_%04X.png" % (idx, code), "0x%04X" % code, slot_num)
            if os.path.exists(png_path):
                skipped += 1
                manifest_rows.append(row + ("skip-existing",))
                continue
            if self._save_glyph(block, slot_num, png_path):
                dumped += 1
                manifest_rows.append(row + ("ok",))
            else:
                failed += 1
        sheet_path = os.path.join(out_dir, "fault_sheet.png")
        if not os.path.exists(sheet_path):
            self._save_sheet(block, fault_slots, 11, sheet_path)

        # -- kanji / symbol-kana block (full font-block enumeration) --
        kanji_index = {}
        if kanji_blob is not None:
            for row in range(7):  # SJIS lead byte 0x81-0x87, symbol/kana block
                for cell in range(188):
                    lowbyte = 0x40 + (cell if cell < 0x3F else cell + 1)
                    sjis = ((0x81 + row) << 8) | lowbyte
                    off = self._get_k_adr(sjis, kanji_blob)
                    if off is None or off < 0:
                        continue  # raw16 index table entry negative/unassigned -- no glyph here
                    slot_num = off // self.GLYPH_BYTES
                    if 0 <= slot_num < num_slots:
                        kanji_index["0x%04X" % sjis] = slot_num
            row = 0
            while True:
                base_slot = 0x30A + row * 0xBC
                if base_slot >= num_slots:
                    break
                for cell in range(188):
                    slot_num = base_slot + cell
                    if slot_num >= num_slots:
                        continue
                    lowbyte = 0x40 + (cell if cell < 0x3F else cell + 1)
                    sjis = ((0x88 + row) << 8) | lowbyte
                    kanji_index["0x%04X" % sjis] = slot_num
                row += 1

        slot_to_sjis = {}
        for sjis_hex, slot_num in kanji_index.items():
            slot_to_sjis.setdefault(slot_num, sjis_hex)

        blank_slot = bytes(self.GLYPH_BYTES)
        for slot in range(num_slots):
            base = slot * self.GLYPH_BYTES
            if block[base:base + self.GLYPH_BYTES] == blank_slot:
                continue  # blank/unused cell (4 of 5120 in the observed IPL image) -- not emitted
            png_path = os.path.join(kanji_dir, "slot_%04d.png" % slot)
            sjis_label = slot_to_sjis.get(slot, "unmapped")
            row = ("kanji/slot_%04d.png" % slot, sjis_label, slot)
            if os.path.exists(png_path):
                skipped += 1
                manifest_rows.append(row + ("skip-existing",))
                continue
            if self._save_glyph(block, slot, png_path):
                dumped += 1
                manifest_rows.append(row + ("ok",))
            else:
                failed += 1
        kanji_sheet_path = os.path.join(out_dir, "kanji_sheet.png")
        if not os.path.exists(kanji_sheet_path):
            self._save_sheet(block, list(range(num_slots)), 64, kanji_sheet_path)

        index_path = os.path.join(out_dir, "kanji_index.json")
        if not os.path.exists(index_path):
            with open(index_path, "w", encoding="utf-8") as fh:
                json.dump({
                    "source": os.path.relpath(ipl_path, REPO),
                    "note": "sjis-code(hex) -> font-block glyph slot index, via ported LeoGetKAdr "
                            "(port/n64_leo.c:347-381); slot N's PNG is kanji/slot_NNNN.png",
                    "numSlots": num_slots, "mapping": kanji_index,
                }, fh, indent=2)
            dumped += 1
        else:
            skipped += 1

        _write_manifest(os.path.join(out_dir, "manifest.tsv"),
                        "path\tsjisOrSlot\tslot\tstatus   (fault font: 110 fixed; kanji: full "
                        "font-block enumeration, blanks skipped)",
                        manifest_rows)
        print("  fonts: %d dumped, %d skipped, %d failed (fault=%d codes, kanji=%d slots, "
              "%d sjis mapped)" % (dumped, skipped, failed, len(fault_codes), num_slots, len(kanji_index)))
        return {"class": self.name, "dumped": dumped, "skipped": skipped, "failed": failed,
                "total": num_slots + len(fault_codes)}


EXTRA_CLASSES = {
    CourseDataDumpClass.name: CourseDataDumpClass,
    DListDumpClass.name: DListDumpClass,
    VertexDataDumpClass.name: VertexDataDumpClass,
    TablesDumpClass.name: TablesDumpClass,
    GhostDumpClass.name: GhostDumpClass,
    FontDumpClass.name: FontDumpClass,
}
