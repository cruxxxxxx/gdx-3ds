#!/usr/bin/env python3
"""Build-time generator: emit shippable data files so the native `gdx-extract dump` classes can
reconstruct the exact same output the Python oracle produces WITHOUT decomp source (or the EK yaml
recipe tree) on the user's machine.

Single source of truth: this reuses the SAME parsers/evaluators the oracle runs, over the SAME decomp
source, so the native output and the oracle output cannot diverge (mirrors how tools/gen_ek_assets.py
emits ek_slice_manifest.txt for the native EK path).

Outputs (into --out-dir, normally decomp-recipes):

  audio_tables_data.json
    {"gSoundFontTable": {headerRaw, numEntries, entries:[...]},   <- tables class (raw C source text)
     "gSequenceTable":  {headerRaw, numEntries, entries:[...]},
     "audio": {                                                    <- audio/midi classes (evaluated ints)
        "romFont":  [{name, offset, size, sd1, sd2, sfx}, ...],   (cart gSoundFontTableData)
        "diskFont": [{name, size, sd1, sd2, sfx}, ...],           (EK gSoundFontTable)
        "romSeq":   [{name, offset, size}, ...],                  (cart gSequenceTableData)
        "diskSeq":  [{name, size}, ...],                          (EK gSequenceTable)
        "diskBank": [{name, offset, size, medium}, ...]}}         (gSampleBankTable, sample resolution)
    The tables-class keys keep the RAW C expression text (byte-identical to the oracle's tables JSON).
    The "audio" section pre-evaluates every bit-packed field with the oracle's own _SafeEval so the
    native audio/midi classes need no expression evaluator and cannot diverge. sd1=(bank1<<8|bank2),
    sd2=(numInst<<8|numDrums), sfx=numSoundEffects.

  ek_tlut_map.json (only when --ek-yaml-dir is given)
    {"<full_address_decimal>": "<TLUT symbol>", ...}
    The EK CI4/CI8 palette-resolution map (segment<<24 | offset, or a bare offset for segment-less
    yamls), built by the oracle's own ek_recipe_index() over the SAME EK recipe tree. This lets the
    native `textures` class resolve EK CI8 palettes WITHOUT --ek-yaml-dir (which stays as an override).

Usage: gen_dump_tables_data.py --audio-tables <audio_tables.c> --out-dir <decomp-recipes dir>
                               [--ek-yaml-dir <fzerox-expansion-kit/assets/yaml/jp>]
"""
import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_dump_all_extra as gda_extra  # noqa: E402


def _build_audio_section():
    """Evaluate the rom/disk soundfont, sequence and sample-bank tables into plain integers, using the
    oracle's own _load_audio_tables()/_SafeEval so the emitted values equal what the oracle computes at
    runtime. Returns the "audio" dict, or None if the audio module/tables are unavailable."""
    import gen_dump_all_audio as gda_audio
    tabs = gda_audio._load_audio_tables()
    if tabs.get("disk_bank") is None:
        return None
    ev = tabs["evalr"]

    def font_rows(tbl, with_off):
        rows = []
        if tbl is None:
            return rows
        for e in tbl["entries"]:
            r = {"name": e["name"], "size": ev(e["size"]),
                 "sd1": ev(e["col5_raw"]), "sd2": ev(e["col6_raw"]), "sfx": ev(e["col7_raw"])}
            if with_off:
                r["offset"] = ev(e["offset"])
            rows.append(r)
        return rows

    def seq_rows(tbl, with_off):
        rows = []
        if tbl is None:
            return rows
        for e in tbl["entries"]:
            r = {"name": e["name"], "size": ev(e["size"])}
            if with_off:
                r["offset"] = ev(e["offset"])
            rows.append(r)
        return rows

    def bank_rows(tbl):
        rows = []
        if tbl is None:
            return rows
        for e in tbl["entries"]:
            rows.append({"name": e["name"], "offset": ev(e["offset"]),
                         "size": ev(e["size"]), "medium": ev(e["medium"])})
        return rows

    return {
        "romFont": font_rows(tabs["rom_font"], True),
        "diskFont": font_rows(tabs["disk_font"], False),
        "romSeq": seq_rows(tabs["rom_seq"], True),
        "diskSeq": seq_rows(tabs["disk_seq"], False),
        "diskBank": bank_rows(tabs["disk_bank"]),
    }


def _build_ek_tlut_map(ek_yaml_dir):
    """Build the EK CI4/CI8 palette-resolution map (full_address -> TLUT symbol) using the oracle's own
    ek_recipe_index() over the same EK recipe tree. Returns {decimal_addr_str: symbol}."""
    import gen_dump_all as gda
    tlut_by_addr, _recipe_symbols = gda.ek_recipe_index(ek_yaml_dir)
    return {str(int(addr)): sym for addr, sym in tlut_by_addr.items()}


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--audio-tables", required=True,
                    help="path to decomp/src/audio/disk/audio_tables.c")
    ap.add_argument("--out-dir", required=True,
                    help="directory to write the data files into (e.g. decomp-recipes)")
    ap.add_argument("--ek-yaml-dir", default=None,
                    help="EK recipe tree (fzerox-expansion-kit/assets/yaml/jp); when given, also emit "
                         "ek_tlut_map.json so the native textures class needs no --ek-yaml-dir")
    args = ap.parse_args()

    if not os.path.isfile(args.audio_tables):
        sys.stderr.write("gen_dump_tables_data: audio_tables.c not found: %s\n" % args.audio_tables)
        return 2
    with open(args.audio_tables, encoding="utf-8") as fh:
        text = fh.read()

    out = {}
    for var in ("gSoundFontTable", "gSequenceTable"):
        parsed = gda_extra.TablesDumpClass._parse_c_table(text, var)
        if parsed is None:
            sys.stderr.write("gen_dump_tables_data: failed to parse %s\n" % var)
            return 1
        out[var] = parsed

    audio = _build_audio_section()
    if audio is None:
        sys.stderr.write("gen_dump_tables_data: could not evaluate the audio tables\n")
        return 1
    out["audio"] = audio

    os.makedirs(args.out_dir, exist_ok=True)
    out_path = os.path.join(args.out_dir, "audio_tables_data.json")
    # Deterministic output (sorted keys, stable separators) so the build artifact is reproducible.
    with open(out_path, "w", encoding="utf-8", newline="\n") as fh:
        json.dump(out, fh, indent=2, sort_keys=True)
        fh.write("\n")
    print("gen_dump_tables_data: wrote %s (%d + %d table entries; %d/%d cart+ek fonts, %d banks)"
          % (out_path, out["gSoundFontTable"]["numEntries"], out["gSequenceTable"]["numEntries"],
             len(audio["romFont"]), len(audio["diskFont"]), len(audio["diskBank"])))

    # The EK TLUT map is a required build artifact (the native textures class resolves EK CI4/CI8
    # palettes from it). A missing or invalid --ek-yaml-dir must FAIL the build loudly rather than
    # silently skip ek_tlut_map.json, which would ship a build that cannot resolve EK palettes.
    if not args.ek_yaml_dir:
        sys.stderr.write("gen_dump_tables_data: --ek-yaml-dir is required (EK TLUT map is a build "
                         "artifact); none was given\n")
        return 2
    if not os.path.isdir(args.ek_yaml_dir):
        sys.stderr.write("gen_dump_tables_data: ek-yaml-dir not found: %s\n" % args.ek_yaml_dir)
        return 2
    ek_map = _build_ek_tlut_map(args.ek_yaml_dir)
    ek_path = os.path.join(args.out_dir, "ek_tlut_map.json")
    with open(ek_path, "w", encoding="utf-8", newline="\n") as fh:
        json.dump(ek_map, fh, indent=2, sort_keys=True)
        fh.write("\n")
    print("gen_dump_tables_data: wrote %s (%d TLUT entries)" % (ek_path, len(ek_map)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
