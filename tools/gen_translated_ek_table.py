#!/usr/bin/env python3
"""G-Diffuser Expansion Kit TRANSLATED-DISK geometry override table (EK reconciliation Phase 2).

tools/gen_ek_assets.py builds port/gen/EkAssetBindings.c's disk-fill table from the
RETAIL-JP asset yamls (fzerox-expansion-kit/assets/yaml/jp). That table is correct
for the retail-JP .ndd and for the vast majority of the fan-translated .ndd
(LuigiBlood/Zoinkity English release) too, because gdx_ek_assets_fill() does a raw
byte copy: as long as a symbol's disk slot (offset + size) is unchanged between the
two disks, an in-place translated caption/string still lands in the right place
and renders correctly with the existing table entry, regardless of what the pixels
say. This generator does NOT duplicate that table. It emits a small, SPARSE
override list -- one entry per symbol whose slot (offset and/or width/height)
actually changed in the translated recompile -- for gdx_ek_assets_fill (or its
Phase 3 caller) to consult ONLY when the loaded disk is the translated variant.

RECOVERY METHOD (see the analysis scripts referenced below for the full replay):
  1. Byte-diff retail-JP vs translated .ndd across the three recompiled EK overlay
     windows (LBA598 expansion_kit/xk1, LBA656 course_edit/xk2, LBA672
     machine_create/xk3), physical-offset-mapped via the disk's LBA->byte block
     math (leo_tbl.c-equivalent, reimplemented read-only in ek_overlay_lba.py).
  2. For every symbol in the CURRENT retail-JP-derived table (port/gen/
     ek_slice_manifest.txt, the exact table gdx_ek_assets_fill() uses today),
     classify JP-vs-TR at that JP offset+size as byte-identical (existing table
     already correct, needs no override) or differing (content changed -- MOST
     differing entries are same-size in-place caption swaps, e.g. every
     aCourseEdit*Tex/aExpansionKit*Tex node-panel and menu caption. See
     classify_ek_assets.py's overlay-window scan: 43/52 machine_create,
     49/62 expansion_kit_textures, 122/184 course_edit_textures symbols are
     byte-identical; nearly all the rest are same-slot content swaps).
  3. For symbols still ambiguous after that (garbage/noise at the JP offset),
     anchor-scan the surrounding bytes at candidate pixel widths and look for
     legible English text (measure_extents.py's row-histogram: a real glyph has
     content rows terminated by a clean all-zero row at the true image height;
     noise does not). Corroborate against the machine_create_draw.c call sites
     (decomp/src/overlays/machine_create/machine_create_draw.c:383,806,808 --
     MachineCreate_DrawColorGradientTextureBlockI8/DrawTextureBlockI8 pass width
     and height as literal C arguments in the JP-matching decompiled source; the
     translated recompile's own build of that call changed those literals, which
     is the "two blit call sites" reconciliation-plan context refers to).
  4. Cross-check with the annotated subfile-boundary diff (diff_ek_overlays.py /
     diff_ek_summary.py): every boundary elsewhere in all three overlays lines up
     byte-for-byte between JP and TR (zero shift), which rules out any other
     symbol changing size/moving -- the Name/Settings swap + Weight shrink is a
     closed, in-place reflow of machine_create's own .data machine_create_assets
     pool (net size unchanged), not a global relayout.
  5. Render every recovered (offset, width, height, format) at the FINAL claimed
     geometry to PNG and eyeball it for legible English text (validate_all.py).

CONFIRMED OVERRIDES (all four re-decoded clean and legible from the translated
disk at the geometry below -- see port/gen/ek_translated_validate/*.png):
  * aMachineCreateMachineNameTex: retail slot 0x00C8B070 (48x12 I8, 576B) is
    NOT where "Machine Name" lives on the translated disk -- decodes there to a
    legible, but misplaced, "Settings" (see aMachineCreateSettingsTex below).
    "Machine Name" itself now lives at the OLD Settings offset, 0x00C8B2B0,
    reshaped to 96x9 (864B -- exactly Settings' old byte budget, just a
    different aspect ratio; ends exactly at the byte where Weight's slot
    begins, 0x00C8B610, with zero slack).
  * aMachineCreateSettingsTex: moved into the OLD Name slot, 0x00C8B070,
    shrunk to 48x12 (576B -- exactly Name's old byte budget). The two symbols
    swapped storage locations; this is why the retired port/gdx_ek_disk_
    overrides.c audit (2026-07-16) found "Settings" at Name's address and
    "noise" at Settings' address when it only tried each symbol's OLD (JP)
    width/height at these offsets -- it never tried the swapped dimensions.
  * D_xk3_80138930 (Weight caption): stays at its own retail offset
    0x00C8B610, but shrinks from 32x16 (512B) to 40x12 (480B); the trailing
    32 bytes of the original 512B slot are zero padding, preserving the
    boundary before D_xk3_80138B30 (Save) at 0x00C8B810 exactly.
  All three retired-file claims that "the correct SETTINGS/WEIGHT glyphs are
  not present at any recoverable offset on this disk" are REFUTED by this
  recovery: they are present, just at different dimensions than retail-JP.

NOT overridden (existing retail-JP table entry is already correct -- same slot,
same size; content differs only because it is a translated caption, which
gdx_ek_assets_fill's raw byte copy already handles with no table change):
  * D_xk3_80138CB0 (Delete, 0x00C8B990, 48x16 I4) -- decodes to "DELETE".
  * Every aCourseEdit*Tex / aExpansionKit*Tex caption/menu texture that shows
    up as "differs" in classify_ek_assets.py but at an unchanged slot size
    (spot-validated: aCourseEditBankTex/WidthTex/SaveTex/LoadTex all decode
    clean at their unchanged retail offsets -- see validate_all.py output).

OUT OF SCOPE for this table (flagged, not fixed here -- see the report):
  * D_xk3_80138B30 (Save button icon, MACHINE_MODE_MNAME, 0x00C8B810, 48x16
    I4) decodes CLEANLY to "LOAD" at its unchanged, correct slot. This is not
    an offset/geometry problem -- the slot is right and the table needs no
    change -- it looks like a translation-content mismatch (wrong glyph
    authored into that slot upstream) unrelated to disk-layout recovery.
  * The keyboard confirmation strings (decomp/src/overlays/expansion_kit/
    expansion_kit_text.c, D_xk1_80032C.. array) and the course_edit tooltip
    glyph bytes (decomp/src/overlays/course_edit/1A4210.c, D_xk2_80104F..
    arrays) are literal C initializers COMPILED INTO the decomp source, not
    disk-fetched assets (classify_ek_assets.py finds zero EK-yaml manifest
    entries in that physical window). gdx_ek_assets_fill() never touches them
    on either disk, so a disk-offset table cannot retarget them for the
    translated disk; fixing those requires a source-level or runtime string
    override keyed by disk variant, which is a different mechanism than this
    phase's asset table.

Emits:
  port/gen/EkTranslatedOverrides.h  -- struct + extern decls for Phase 3/4.
  port/gen/EkTranslatedOverrides.c  -- the override rows (referencing the
                                       SAME extern symbol arrays EkAssetBindings.c
                                       defines, so `dest` is a real compile-time
                                       pointer -- no runtime string lookup needed).

This tool owns ONLY these two new generated files plus itself; it does not read
or write port/gen/EkAssetBindings.c, tools/gen_ek_assets.py, or anything under
decomp/src.
"""
import os

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_H = os.path.join(REPO, "port", "gen", "EkTranslatedOverrides.h")
OUT_C = os.path.join(REPO, "port", "gen", "EkTranslatedOverrides.c")

# CRC-64/XZ (ECMA-182, reflected 0xC96C5795D7870F42) of the full pristine 64DD
# image -- the SAME algorithm and constant disk_savefile.cpp's crc64() uses.
# Computed directly off both known disks (see jobs scratch compute_crc64.py);
# ship as named constants here so Phase 3/4 doesn't need to recompute or guess.
JP_DISK_CRC64 = 0x8D185CE0BD9EDABB
TRANSLATED_DISK_CRC64 = 0xE456FD866541E5AB
DISK_EXACT_BYTES = 64_931_840

# format -> GdxEkTexFormat enumerator emitted below.
FORMATS = ["I4", "I8", "IA4", "IA8", "IA16", "RGBA16", "RGBA32", "CI4", "CI8"]

# (symbol, ctype, dest_capacity, translated_offset, translated_size, width, height, format, note)
# ctype is deliberately "unsigned char", not the decomp headers' "u8" typedef alias
# for it: this TU only needs an `extern` re-declaration of storage EkAssetBindings.c
# already defines elsewhere, and using the plain builtin type avoids depending on
# global.h (whose macros/typedefs clash with the host CRT in this TU environment --
# see port/gdx_ek_disk_overrides.c's identical avoidance via a local u8 typedef).
# `unsigned char` and `u8` name the same type, so the two TUs' declarations agree.
#
# dest_capacity MUST match the real array size EkAssetBindings.c defines for this
# symbol (that file is the source of truth). It is emitted as an explicit array
# bound on the extern re-declaration below so `sizeof(symbol)` is computable in
# THIS translation unit and self-validates the destCapacity column at compile
# time -- if EkAssetBindings.c's definition and this table ever disagree, the
# linker will not catch it (extern arrays aren't cross-TU size-checked), so
# keep this value in sync by hand whenever EkAssetBindings.c's array size changes.
OVERRIDES = [
    (
        "aMachineCreateMachineNameTex", "unsigned char", 864,
        0x00C8B2B0, 864, 96, 9, "I8",
        "Moved from the old Name slot (0x00C8B070) into the old Settings slot; "
        "reshaped 48x12 -> 96x9 (same 864B budget as old Settings). Decodes clean.",
    ),
    (
        "aMachineCreateSettingsTex", "unsigned char", 864,
        0x00C8B070, 576, 48, 12, "I8",
        "Moved from the old Settings slot (0x00C8B2B0) into the old Name slot; "
        "reshaped 72x12 -> 48x12 (same 576B budget as old Name). Decodes clean.",
    ),
    (
        "D_xk3_80138930", "unsigned char", 512,
        0x00C8B610, 480, 40, 12, "I8",
        "Same retail offset (0x00C8B610); reshaped 32x16 -> 40x12 (480B of the "
        "original 512B slot; trailing 32B are zero padding). Decodes clean.",
    ),
]


def emit_header():
    lines = []
    lines.append("// AUTO-GENERATED by tools/gen_translated_ek_table.py. Do not edit by hand.")
    lines.append("// Sparse geometry-override table for the fan-translated Expansion Kit disk")
    lines.append("// (LuigiBlood/Zoinkity English .ndd). See tools/gen_translated_ek_table.py's")
    lines.append("// module docstring for the full recovery method and evidence.")
    lines.append("#ifndef GDX_EK_TRANSLATED_OVERRIDES_H")
    lines.append("#define GDX_EK_TRANSLATED_OVERRIDES_H")
    lines.append("")
    lines.append("#ifdef __cplusplus")
    lines.append('extern "C" {')
    lines.append("#endif")
    lines.append("")
    lines.append(f"#define GDX_EK_JP_DISK_CRC64 0x{JP_DISK_CRC64:016X}ULL")
    lines.append(f"#define GDX_EK_TRANSLATED_DISK_CRC64 0x{TRANSLATED_DISK_CRC64:016X}ULL")
    lines.append(f"#define GDX_EK_DISK_EXACT_BYTES {DISK_EXACT_BYTES}U")
    lines.append("")
    lines.append("enum {")
    for fmt in FORMATS:
        lines.append(f"    GDX_EK_TEX_FMT_{fmt},")
    lines.append("};")
    lines.append("")
    lines.append("typedef struct {")
    lines.append("    const char* name;               /* symbol name, for diagnostic logging only */")
    lines.append("    void* dest;                    /* same array EkAssetBindings.c defines for this symbol */")
    lines.append("    unsigned int translatedDiskOffset; /* physical .ndd byte offset, TRANSLATED disk only */")
    lines.append("    unsigned int size;              /* byte size at that offset on the translated disk */")
    lines.append("    unsigned int destCapacity;      /* sizeof(dest); self-validates size <= dest's real bound */")
    lines.append("    unsigned short width;")
    lines.append("    unsigned short height;")
    lines.append("    unsigned char format;           /* GDX_EK_TEX_FMT_* */")
    lines.append("} GdxEkTranslatedOverride;")
    lines.append("")
    lines.append("extern const GdxEkTranslatedOverride gEkTranslatedOverrides[];")
    lines.append("extern const unsigned int gEkTranslatedOverrideCount;")
    lines.append("")
    lines.append("#ifdef __cplusplus")
    lines.append("}")
    lines.append("#endif")
    lines.append("")
    lines.append("#endif /* GDX_EK_TRANSLATED_OVERRIDES_H */")
    lines.append("")
    return "\n".join(lines)


def emit_source():
    lines = []
    lines.append("// AUTO-GENERATED by tools/gen_translated_ek_table.py. Do not edit by hand.")
    lines.append('#include "EkTranslatedOverrides.h"')
    lines.append("")
    lines.append("// Externs for the symbols EkAssetBindings.c already DEFINES with real storage;")
    lines.append("// redeclaring them here (separate TU) lets this table reference their addresses")
    lines.append("// as compile-time constants, with no runtime symbol-name lookup required. The")
    lines.append("// array bound below is dest_capacity from gen_translated_ek_table.py's OVERRIDES")
    lines.append("// table -- it MUST match EkAssetBindings.c's real definition so sizeof() here")
    lines.append("// equals that array's true byte capacity (see the row's destCapacity column).")
    seen = set()
    caps = {}
    for sym, ctype, cap, *_ in OVERRIDES:
        if sym not in seen:
            seen.add(sym)
            caps[sym] = cap
            lines.append(f"extern {ctype} {sym}[{cap}];")
    lines.append("")
    lines.append("const GdxEkTranslatedOverride gEkTranslatedOverrides[] = {")
    for sym, _ctype, _cap, off, size, w, h, fmt, note in OVERRIDES:
        lines.append(f"    // {sym}: {note}")
        lines.append(
            '    { "%s", %s, 0x%08XU, %uU, sizeof(%s), %u, %u, GDX_EK_TEX_FMT_%s },'
            % (sym, sym, off, size, sym, w, h, fmt)
        )
    lines.append("};")
    lines.append(
        "const unsigned int gEkTranslatedOverrideCount = "
        "sizeof(gEkTranslatedOverrides) / sizeof(gEkTranslatedOverrides[0]);"
    )
    lines.append("")
    return "\n".join(lines)


def main():
    os.makedirs(os.path.dirname(OUT_H), exist_ok=True)
    with open(OUT_H, "w", newline="\n") as f:
        f.write(emit_header())
    with open(OUT_C, "w", newline="\n") as f:
        f.write(emit_source())
    print(f"EK translated-disk overrides: {len(OVERRIDES)} entries -> {OUT_H}, {OUT_C}")


if __name__ == "__main__":
    main()
