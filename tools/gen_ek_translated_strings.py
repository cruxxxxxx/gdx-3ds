#!/usr/bin/env python3
"""G-Diffuser Expansion Kit TRANSLATED-DISK text binding table.

WHAT PROBLEM THIS SOLVES. The EK's on-screen text is not a yaml asset, so
tools/gen_ek_assets.py's slice table (port/gen/EkAssetBindings.c) does not cover
it and gdx_ek_assets_fill() never touches it. The port therefore renders whatever
the decompiled C source compiled in, which is Japanese -- while the fan-translated
64DD disk the user actually loads carries a full English version of every one of
those strings, in the very same overlay .data sections. This generator binds the
two together, so the text on screen comes from the loaded disk instead of from a
hand-written translation in the port. Load the retail-JP disk and the Japanese
comes back, from the same mechanism, with no second code path.

TWO BINDING KINDS, because the source has two shapes:

  ARRAY  A named fixed-size array in overlay .data (`u8 D_xk2_80104F74[] = {...}`).
         The port copies the disk's bytes into that array in place. Each such
         array is declared in the decomp with the GDX_EK_TEXT_CAP bound so it can
         hold the longest translated form; see REQUIRED CAPACITY in the report
         this tool prints.

  PTR    An array of `char*` (`D_xk1_800337D0[]`, `sCharacterNamesByNumber[]`).
         Nothing can be copied in place -- the storage is unnamed string literals
         -- so the port copies the disk's text into its own pool and repoints each
         entry. The decomp side needs no change at all for these.

ADDRESS RECOVERY. Most strings kept their retail vram in the translated recompile,
so the disk offset is just the overlay window mapping. The course_edit tooltip
pool did NOT: 'Select a file.' is 14 bytes where the JP symbol was 5, and
everything after it shifted by a varying amount -- in one case BACKWARD
(0x80105174 -> 0x80105170), which is why address-order alignment between the two
disks is not a safe assumption. For those, the address comes from
tools/ek_recovery/map_translated_symbols.py, which differences the two disks'
compiled `lui/addiu` address materializations at identical instruction offsets.
See that module's docstring for why that is sound and how it self-checks.

WHAT IS DELIBERATELY NOT BOUND. A symbol is emitted only when the translated disk
holds printable ASCII there AND the retail-JP disk does not -- i.e. only where the
translation actually replaced Japanese text with English. Anything else (binary
tables, unchanged ASCII format strings like "%02d", the kanji-preload pool
D_xk1_80033808) is left alone. This tool never guesses: a string it cannot prove
an address for simply does not appear in the output, and stays Japanese.

Emits:
  port/gen/EkTranslatedStrings.h  -- struct + extern decls
  port/gen/EkTranslatedStrings.c  -- the rows, plus the pooled PTR text

This tool owns only those two files plus itself. It does not write to decomp/src,
port/gen/EkAssetBindings.c, or port/gen/EkTranslatedOverrides.*.
"""
import os
import re
import struct
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "tools", "ek_recovery"))
from ek_disk_paths import JP_DISK_PATH, TRANSLATED_DISK_PATH  # noqa: E402
from extract_overlay_pointers import OVERLAYS, RAM_LO, RAM_HI  # noqa: E402
from map_translated_symbols import materialized  # noqa: E402

OUT_H = os.path.join(REPO, "port", "gen", "EkTranslatedStrings.h")
OUT_C = os.path.join(REPO, "port", "gen", "EkTranslatedStrings.c")

# Bound the decomp declares for every ARRAY-bound symbol (GDX_EK_TEXT_CAP). Must
# be >= the longest translated string + NUL; the report below fails the run if a
# recovered string would not fit, rather than emitting a row that overflows.
TEXT_CAP = 48

# Overlay source files scanned for ARRAY-shaped text symbols. Only 1-D `u8 NAME[]`
# declarations are considered -- a 2-D table is a different storage shape and is
# listed explicitly under FIXED_TABLES instead.
#
# course_edit only. The expansion_kit and machine_create text tables are reachable
# through pointer arrays (POINTER_TABLES below), which is both stronger evidence
# and a smaller change: repointing needs no capacity widening in the decomp at all.
# Binding the same strings twice, by two mechanisms, would just be a way to
# disagree with itself.
ARRAY_SOURCES = {
    "course_edit": ["src/overlays/course_edit/1A4210.c", "src/overlays/course_edit/191080.c"],
}

# Fixed-size tables copied whole, at an unchanged vram. (symbol, overlay, vram, bytes)
FIXED_TABLES = [
    # Track-shape names, 8 entries x 8 bytes. The translated disk fills all eight
    # bytes for "Cylinder"/"Halfpipe" with no NUL; copying the table whole
    # reproduces the disk exactly rather than second-guessing its packing.
    ("D_xk2_800F7090", "course_edit", 0x800F7090, 64),
]

# Pointer arrays whose entries the port repoints.
# (symbol, overlay, vram, count, remap, declared C type in the decomp)
#
# The declared type is carried through so the extern this tool emits MATCHES the
# real definition instead of quietly restating it as something else -- two of these
# are `char*` and one is `const char*`. The table stores dest as void* for that
# reason, and the apply loop does the one cast, in one place, on purpose.
POINTER_TABLES = [
    ("D_xk1_800337D0", "expansion_kit", 0x800337D0, 14, False, "char*"),
    ("D_xk3_801372B8", "machine_create", 0x801372B8, 12, False, "char*"),
    # The driver names are the one table drawn with the BASE CARTRIDGE font
    # (Font_DrawString), not the EK glyph path. The translation targets the JP
    # cartridge, whose font sheet carries 'r', 's' and 'c' at the codepoints the
    # US sheet uses for '(', ')' and '*' -- which is why the disk literally spells
    # "D(. STEWART" and "JAMES M*CLOUD". G-Diffuser runs the US base ROM, so those
    # three codepoints are remapped back to letters on the way in. Evidence: five
    # independent names (Dr. STEWART, Dr. CLASH, Mr. EAD, Mrs. ARROW, JAMES
    # McCLOUD) all resolve correctly and consistently under exactly this mapping.
    ("sCharacterNamesByNumber", "machine_create", 0x80136654, 30, True, "const char*"),
]

# Course Edit node-panel labels the retail build never gave a symbol to: they are
# inline initializers and printf arguments in the source
# (`u8 sp90[5] = {0x83,0xD8,0xAF,0x8F,0}`, `"%c%c%c", 0x90,0xDD,0xB8`), which the
# compiler pools into .rodata. The translation replaced each pooled constant with
# an English string at the SAME address, so the port gets storage of its own here
# and 191080.c draws from it when it is non-empty.
#
# Each row carries the retail-JP bytes that must be at that address. The
# identification is only as good as that check: "Grid" sitting next to "Boost" is
# suggestive, but 83 D8 AF 8F being exactly the glyph bytes the source writes for
# guriddo is proof. A mismatch aborts the run rather than emitting a guess.
# (symbol suffix, overlay, vram, expected retail-JP bytes, what it labels)
INLINE_LABELS = [
    ("grid",   "course_edit", 0x800F7150, "83d8af8f", "node panel, grid mode"),
    ("boost",  "course_edit", 0x800F7158, "8bafbcad", "dash/boost plate count"),
    ("jump",   "course_edit", 0x800F7160, "87acdd97", "jump plate count"),
    ("trap",   "course_edit", 0x800F7168, "c4d7af97", "trap count"),
    ("object", "course_edit", 0x800F7170, "b59287aa", "object count"),
    ("size",   "course_edit", 0x800F7178, "99b2ddc4", "point/size count"),
    ("bank",   "course_edit", 0x800F7274, "256325632563", "node panel, bank angle"),
    ("width",  "course_edit", 0x800F7280, "2563256325632563", "node panel, road width"),
]

FONT_REMAP = {0x28: ord("r"), 0x29: ord("s"), 0x2A: ord("c")}

# Matches both the untouched form (`u8 D_xk2_80104F74[] = {`) and the bound form
# this tool's output requires (`u8 D_xk2_80104F74[GDX_EK_TEXT_CAP] = {`), so
# regenerating after the decomp has been widened finds the same symbols it found
# the first time instead of quietly emitting an empty table.
SYMBOL_RE = re.compile(
    r"^\s*(?:UNUSED\s+)?u8\s+(D_xk[123]_([0-9A-F]{8}))\s*\[\s*(?:GDX_EK_TEXT_CAP)?\s*\]\s*=", re.M)


def vram_to_off(overlay, vram):
    base, phys, size = OVERLAYS[overlay]
    if not (base <= vram < base + size):
        return None
    return phys + (vram - base)


def cstring(blob, off, limit=256):
    end = blob.find(b"\x00", off, off + limit)
    return blob[off:end] if end >= 0 else None


def is_text(raw):
    return raw is not None and len(raw) > 0 and all(0x20 <= b < 0x7F for b in raw)


def build_address_map(jp, tr, overlay):
    """JP vram -> translated vram, for every address the compiled code materializes.

    Only unambiguous mappings are kept: a JP address materialized at two call
    sites that disagree about the translated address is dropped entirely, because
    there is no way to tell which one names the string. See
    tools/ek_recovery/map_translated_symbols.py for the full rationale.
    """
    base, phys, size = OVERLAYS[overlay]
    jw = struct.unpack(">%dI" % (size // 4), jp[phys:phys + (size // 4) * 4])
    tw = struct.unpack(">%dI" % (size // 4), tr[phys:phys + (size // 4) * 4])
    seen = {}
    for i in range(len(jw)):
        a = materialized(jw, i)
        if a is None:
            continue
        b = materialized(tw, i)
        if b is None or a[1] != b[1] or not (RAM_LO <= b[0] < RAM_HI):
            continue
        seen.setdefault(a[0], set()).add(b[0])
    return {k: next(iter(v)) for k, v in seen.items() if len(v) == 1}


def main():
    for path in (JP_DISK_PATH, TRANSLATED_DISK_PATH):
        if not os.path.exists(path):
            raise SystemExit("required disk image missing: %s" % path)
    jp = open(JP_DISK_PATH, "rb").read()
    tr = open(TRANSLATED_DISK_PATH, "rb").read()

    arrays = []   # (symbol, overlay, jp_vram, tr_vram, tr_off, text)
    fixed = []    # (symbol, overlay, tr_off, size)
    pointers = [] # (symbol, count, [(tr_off, text), ...], remap)
    skipped = []
    inline = []     # port-owned storage for labels the retail build inlined
    unproven = []   # text symbols no call site materializes -- left Japanese, on purpose

    addr_maps = {ov: build_address_map(jp, tr, ov) for ov in OVERLAYS}

    for overlay, sources in ARRAY_SOURCES.items():
        for rel in sources:
            src = open(os.path.join(REPO, "decomp", rel), encoding="utf-8", errors="replace").read()
            for name, hexaddr in SYMBOL_RE.findall(src):
                jp_vram = int(hexaddr, 16)
                # No same-vram fallback. The tooltip pool reflowed by varying
                # amounts, so reading a symbol's retail vram on the translated disk
                # lands MID-STRING as often as not (D_xk2_80104F30 reads " file.",
                # the tail of "Select a file."). Only an address the compiled code
                # itself proves is usable; everything else is reported unbound and
                # keeps its Japanese, which is wrong-looking but never wrong.
                if jp_vram not in addr_maps[overlay]:
                    unproven.append(name)
                    continue
                tr_vram = addr_maps[overlay][jp_vram]
                tr_off = vram_to_off(overlay, tr_vram)
                jp_off = vram_to_off(overlay, jp_vram)
                if tr_off is None or jp_off is None:
                    continue
                text = cstring(tr, tr_off)
                # Bind only where the translation actually replaced Japanese with
                # English. Unchanged ASCII (format strings, "%c") and binary data
                # are left exactly as the decomp compiled them.
                if not is_text(text) or is_text(cstring(jp, jp_off)):
                    continue
                if len(text) + 1 > TEXT_CAP:
                    skipped.append((name, len(text) + 1, text))
                    continue
                arrays.append((name, overlay, jp_vram, tr_vram, tr_off, text))

    for suffix, overlay, vram, jp_hex, what in INLINE_LABELS:
        off = vram_to_off(overlay, vram)
        expect = bytes.fromhex(jp_hex)
        actual = jp[off:off + len(expect)]
        if actual != expect:
            raise SystemExit(
                "INLINE_LABELS %r: retail-JP disk holds %s at 0x%08X, expected %s -- the site "
                "identification is wrong, refusing to emit it" % (suffix, actual.hex(), vram, jp_hex))
        text = cstring(tr, off)
        if not is_text(text):
            raise SystemExit("INLINE_LABELS %r: translated disk has no string at 0x%08X" % (suffix, vram))
        inline.append(("gdx_ek_label_" + suffix, overlay, off, text, what))

    for name, overlay, vram, size in FIXED_TABLES:
        off = vram_to_off(overlay, vram)
        fixed.append((name, overlay, off, size))

    for name, overlay, vram, count, remap, ctype in POINTER_TABLES:
        arr_off = vram_to_off(overlay, vram)
        entries = []
        for i in range(count):
            (ptr,) = struct.unpack_from(">I", tr, arr_off + i * 4)
            off = vram_to_off(overlay, ptr)
            raw = cstring(tr, off) if off is not None else None
            if raw is None:
                raise SystemExit("%s[%d]: unreadable translated pointer 0x%08X" % (name, i, ptr))
            if remap:
                raw = bytes(FONT_REMAP.get(b, b) for b in raw)
            entries.append((off, raw))
        pointers.append((name, count, entries, remap, ctype))

    write_outputs(arrays, fixed, pointers, inline)

    print("EK translated-string table")
    print("  ARRAY  %d symbols (in-place copy at GDX_EK_TEXT_CAP=%d)" % (len(arrays), TEXT_CAP))
    print("  FIXED  %d tables (whole-table copy)" % len(fixed))
    print("  INLINE %d labels the retail build had no symbol for (port-owned storage)" % len(inline))
    print("  PTR    %d tables, %d entries (repointed)"
          % (len(pointers), sum(p[1] for p in pointers)))
    longest = max((len(t[5]) + 1 for t in arrays), default=0)
    print("  REQUIRED CAPACITY: %d bytes (longest bound string + NUL)" % longest)
    if unproven:
        print("  unbound (no call site proves a translated address; stays Japanese): %d"
              % len(unproven))
        for name in unproven:
            print("     %s" % name)
    if skipped:
        print("  !! %d symbol(s) EXCEED GDX_EK_TEXT_CAP and were NOT bound:" % len(skipped))
        for name, need, text in skipped:
            print("     %s needs %d: %r" % (name, need, text.decode("ascii")))
        raise SystemExit("raise GDX_EK_TEXT_CAP/TEXT_CAP and regenerate")


def write_outputs(arrays, fixed, pointers, inline):
    h = ["// AUTO-GENERATED by tools/gen_ek_translated_strings.py. Do not edit by hand.",
         "// Text binding for the fan-translated Expansion Kit disk. See that tool's",
         "// module docstring for the recovery method and for what is deliberately unbound.",
         "#ifndef GDX_EK_TRANSLATED_STRINGS_H",
         "#define GDX_EK_TRANSLATED_STRINGS_H",
         "",
         "#ifdef __cplusplus",
         'extern "C" {',
         "#endif",
         "",
         "// Longest translated string + NUL, and therefore the bound every ARRAY-bound",
         "// symbol is declared with in the decomp (GDX_EK_TEXT_CAP in decomp/include/macros.h).",
         "#define GDX_EK_TEXT_CAP %d" % TEXT_CAP,
         "",
         "typedef struct {",
         "    const char* name;      /* symbol name, for diagnostic logging only */",
         "    void* dest;            /* the decomp array this text is copied into */",
         "    unsigned int diskOffset; /* physical .ndd byte offset, TRANSLATED disk only */",
         "    unsigned int size;     /* bytes to copy, including the NUL for strings */",
         "    unsigned int capacity; /* dest's declared bound; size is never allowed past it */",
         "} GdxEkTranslatedString;",
         "",
         "typedef struct {",
         "    const char* name;      /* pointer-array symbol name, for logging */",
         "    /* The pointer array whose entries get repointed. void* because the three",
         "     * arrays do not share a declared type in the decomp (two char*, one",
         "     * const char*), so the one cast lives in the apply loop instead of in a",
         "     * per-symbol extern that would have to misstate one of them. */",
         "    void* dest;",
         "    unsigned int count;    /* entries in that array */",
         "    const char* const* text; /* count replacement strings, in entry order */",
         "} GdxEkTranslatedStringTable;",
         "",
         "// Storage for Course Edit labels the retail build compiled inline, so there is",
         "// no overlay symbol to copy into. Empty until a translated disk is loaded, which",
         "// is exactly the test the draw sites use to decide between English and Japanese.",
         ] + ["extern char %s[GDX_EK_TEXT_CAP];" % n for n, _o, _f, _t, _w in inline] + [
         "",
         "extern const GdxEkTranslatedString gEkTranslatedStrings[];",
         "extern const unsigned int gEkTranslatedStringCount;",
         "extern const GdxEkTranslatedStringTable gEkTranslatedStringTables[];",
         "extern const unsigned int gEkTranslatedStringTableCount;",
         "",
         "#ifdef __cplusplus",
         "}",
         "#endif",
         "",
         "#endif /* GDX_EK_TRANSLATED_STRINGS_H */",
         ""]

    c = ["// AUTO-GENERATED by tools/gen_ek_translated_strings.py. Do not edit by hand.",
         '#include "EkTranslatedStrings.h"',
         "",
         "// Declared here rather than pulled from a decomp header: these symbols are",
         "// file-scope definitions inside their overlay .c files with no header of their",
         "// own. The bound must match what the decomp declares (GDX_EK_TEXT_CAP) so the",
         "// capacity column below is the array's real byte budget.",
         ""]
    for name, _ov, _jv, _tv, _off, _t in arrays:
        c.append("extern unsigned char %s[GDX_EK_TEXT_CAP];" % name)
    for name, _ov, _off, size in fixed:
        c.append("extern unsigned char %s[%d];" % (name, size))
    c.append("")
    for name, count, _e, _r, ctype in pointers:
        c.append("extern %s %s[%d];" % (ctype, name, count))
    c.append("")
    for n, _o, _f, _t, what in inline:
        c.append("char %s[GDX_EK_TEXT_CAP];  // %s" % (n, what))
    c.append("")
    c.append("const GdxEkTranslatedString gEkTranslatedStrings[] = {")
    for name, ov, jv, tv, off, text in arrays:
        moved = "" if jv == tv else "  // moved: retail 0x%08X -> 0x%08X" % (jv, tv)
        c.append('    { "%s", %s, 0x%08XU, %uU, GDX_EK_TEXT_CAP },%s'
                 % (name, name, off, len(text) + 1, moved))
        c.append("    // %s: %r" % (ov, text.decode("ascii")))
    for name, ov, off, size in fixed:
        c.append('    { "%s", %s, 0x%08XU, %uU, %uU },  // %s, whole table'
                 % (name, name, off, size, size, ov))
    for n, ov, off, text, what in inline:
        c.append('    { "%s", %s, 0x%08XU, %uU, GDX_EK_TEXT_CAP },' % (n, n, off, len(text) + 1))
        c.append("    // %s: %r (%s)" % (ov, text.decode("ascii"), what))
    c.append("};")
    c.append("const unsigned int gEkTranslatedStringCount = "
             "sizeof(gEkTranslatedStrings) / sizeof(gEkTranslatedStrings[0]);")
    c.append("")
    for name, count, entries, remap, _ct in pointers:
        note = "  // codepoints remapped to the US cartridge font" if remap else ""
        c.append("static const char* const k%s[%d] = {%s" % (name, count, note))
        for off, raw in entries:
            c.append('    "%s",  // disk 0x%07X' % (c_escape(raw), off))
        c.append("};")
    c.append("")
    c.append("const GdxEkTranslatedStringTable gEkTranslatedStringTables[] = {")
    for name, count, _e, _r, _ct in pointers:
        c.append('    { "%s", (void*) %s, %uU, k%s },' % (name, name, count, name))
    c.append("};")
    c.append("const unsigned int gEkTranslatedStringTableCount = "
             "sizeof(gEkTranslatedStringTables) / sizeof(gEkTranslatedStringTables[0]);")
    c.append("")

    open(OUT_H, "w", encoding="utf-8", newline="\n").write("\n".join(h))
    open(OUT_C, "w", encoding="utf-8", newline="\n").write("\n".join(c))


def c_escape(raw):
    out = []
    for b in raw:
        if b == 0x22:
            out.append('\\"')
        elif b == 0x5C:
            out.append("\\\\")
        elif b == 0x3F:
            out.append("\\077")  # keep trigraph-free
        elif 0x20 <= b < 0x7F:
            out.append(chr(b))
        else:
            out.append("\\%03o" % b)
    return "".join(out)


if __name__ == "__main__":
    main()
