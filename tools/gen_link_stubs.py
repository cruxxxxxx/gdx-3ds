#!/usr/bin/env python3
"""G-Diffuser link-stub generator (Slice 4c / R4+R5 bring-up; R5 profile-parameterized).

Reads the undefined symbols from a top-build.log and emits port/gen/LinkStubs.c defining each
so the executable LINKS. Data-like symbols (segment/overlay/framebuffer linker markers, globals,
microcode blobs) become 1-byte placeholders; the rest become no-op functions returning 0.

These are PLACEHOLDERS to reach a linked binary. Real implementations (segment system, overlay
loader, save/leo/mfs, arena, audio microcode) are R5/R6 — verified on a real desktop runtime.

R5 (C-R5.2) — profile parameterization:
  --profile us/rev0  (default)  -> port/gen/LinkStubs.c    (table sizes from decomp/assets/yaml/us/rev0)
  --profile jp/rev0             -> port/gen/LinkStubs.jp.c (table sizes from decomp/assets/yaml/jp/rev0)
  --out <path>                  -> write to an arbitrary path.

IMPORTANT — the undefined-symbol list comes from top-build.log, which is a US build's link log. A
JP (VERSION_JP) build references a DIFFERENT undefined-symbol set (JP machine-name text, kanji glyph
tables, JP-only Leo font helpers, JP audio-lib branches). Therefore LinkStubs.jp.c is EXPERIMENTAL:
it MUST be regenerated from a JP build's OWN top-build.log once the JP target links far enough to
produce one. When the current log has no undefined symbols (a clean/stale log), the JP profile still
emits a placeholder file carrying only the JP yaml table-symbol data definitions (which are
log-independent) so the JP gen file compiles; the function stubs are then filled from a JP log.
"""
import argparse
import glob
import os
import re

import yaml

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOG = os.path.join(REPO, "top-build.log")


def yaml_table_sizes(asset_yaml_dir):
    """Real byte sizes for yaml :config: table symbols (e.g. aPositionDigitTexs).

    Game code addresses these as interior pointers (base + index * stride), and
    the runtime asset resolvers match interior references against the symbol's
    range. A 1-byte placeholder makes that range window swallow every stub
    symbol packed after it, so table symbols MUST be defined at their real size.
    Mirrors the table scan in gen_asset_bindings.py.
    """
    sizes = {}
    for path in sorted(glob.glob(os.path.join(asset_yaml_dir, "*.yaml"))):
        with open(path) as f:
            data = yaml.safe_load(f.read()) or {}
        tables = (data.get(":config", {}) or {}).get("tables") or {}
        for table_name, table_val in tables.items():
            if not isinstance(table_val, dict):
                continue
            table_range = table_val.get("range")
            if not isinstance(table_range, (list, tuple)) or len(table_range) < 2:
                continue
            size = int(table_range[1]) - int(table_range[0])
            if size > 0:
                sizes[table_name] = size
    return sizes

# Symbols we implement for real elsewhere (shims.c, decomp_port.c) — never stub these.
EXCLUDE = {
    "Arena_Allocate", "Arena_StartInit", "Arena_DefaultStartInit", "Arena_EndInit",
    # Real MIO0 decoded-size reader in decomp/src/game/object.c.
    "func_800AA6BC",
    # R6: decomp's libultra/os/*.c is now compiled (real N64 cooperative scheduler) — these
    # are DEFINED by the decomp, so they must not be stubbed (would be duplicate symbols).
    "osCreateThread", "osStartThread", "osStopThread", "osDestroyThread", "osYieldThread",
    "osSetThreadPri", "osGetThreadPri", "osGetActiveQueue", "__osGetActiveQueue",
    "osCreateMesgQueue", "osSendMesg", "osRecvMesg", "osJamMesg", "osSetEventMesg",
    "osPhysicalToVirtual", "osVirtualToPhysical", "osGetMemSize", "osMemSize", "osInitialize",
    "osSetGlobalIntMask", "osResetGlobalIntMask",
    # R6: port-provided primitives (port/n64_sched.c context switch + gfx bridge) — we WRITE
    # these, so don't stub them either.
    "__osDispatchThread", "__osEnqueueAndYield", "__osEnqueueThread", "__osPopThread",
    "__osDequeueThread", "__osThreadTail", "__osDisableInt", "__osRestoreInt",
    "__osCleanupThread", "__osGetCurrFaultedThread",
    "osSpTaskStart", "osSpTaskLoad", "osSpTaskStartGo", "osSpTaskYield", "osSpTaskYielded",
    # R6 VI bridge (port/n64_vi.c) — we provide these (libultraship os_vi.cpp is disabled).
    "osViSwapBuffer", "osViGetCurrentFramebuffer", "osViGetNextFramebuffer", "osViSetEvent",
    "osCreateViManager", "osViSetMode", "osViBlack", "osViSetSpecialFeatures",
    "osViSetXScale", "osViSetYScale", "osMemSize",
    # PC has no 64DD medium; shims.c reports that state with the real signature.
    "LeoTestUnitReady",
    # Real libultra formatter in shims.c (vsnprintf-backed). Stubbing it to `return 0`
    # is silent rather than fatal — callers gate their draw on the returned character
    # count — which blanked the Course Edit info panels, the Create Machine machine
    # names, the disk file list, and the N64 crash screen.
    "_Printf",
    # libultra globals the decomp uses as ARRAYS (must be real writable data, not function stubs).
    "osAppNMIBuffer",
    # Save-system slice: decomp/src/overlays/ovl_i2/save.c is now compiled (real
    # cart-SRAM save logic, host-backed via port/sram_buffer.cpp) — these are DEFINED
    # by the decomp there, so they must not be stubbed (would be duplicate symbols).
    "Save_Init", "Save_InitGhost", "Save_Load", "Save_LoadGhost", "Save_LoadGhostInfo",
    "Save_SaveCourseRecordProfiles", "Save_SaveDeathRaceProfiles", "Save_SaveGhost",
    "Save_UpdateCharacterSave", "Save_UpdateCourseCharacterSave", "Save_UpdateCupCompletion",
    "Save_UpdateCupSave", "Sram_Init", "Sram_ReadWrite", "Save_LoadStaffGhostRecord",
    "Save_SaveSettingsProfiles", "D_i2_8010ADE0", "gSettingSoundMode", "gSramPiHandlePtr",
    "func_i2_801017B8", "func_i2_801039BC",
    # EK save/ghost surface also defined for real in save.c now.
    "Save_CalculateGhostRecordChecksum", "Save_CalculateSaveCourseRecordChecksum",
    "Save_ClearCourseRecord", "Save_ClearGhostRecord", "Save_GetDDStaffGhostCompletion",
    "Save_GetDDStaffGhostRecordTime", "Save_InitCourseRecord", "Save_LoadGhostData",
    "Save_ReadGhostData", "Save_SaveGhostData", "Save_SaveGhostRecord",
    "Save_SetDDStaffGhostComplete", "Save_WriteGhostData", "Save_WriteGhostRecord",
    "sDDStaffGhostRecordTimes", "func_i2_800A8CE4", "D_i2_80111848",
}

# A3 (E4 follow-up): venue-bank symbols declared in decomp/include/fzx_segmentA.h but never
# referenced by any currently-compiled decomp source (confirmed: no hits under decomp/src), so
# they can never appear as "undefined symbol" in top-build.log and the normal log-driven path
# below can never stub them on its own -- added explicitly so the bridge's kBankLow32[] venue-bank
# alias table can cover banks 9-10 alongside 0-8/11. D_A00B000 (bank 11, the sibling
# gRoadTypeMenuItems ACTUALLY references -- decomp/src/overlays/expansion_kit/A3AE0.c:534-544) is
# deliberately EXCLUDED here: it already has a real, correctly-sized stub in
# port/gen/EkLinkStubs.c (EK: deferred-subsystem placeholders); adding it here too would
# duplicate-define it and fail the link.
EXTRA_DATA_SYMS = ["D_A009000", "D_A00A000"]


def ensure_extra_data_syms(out):
    """Idempotently append EXTRA_DATA_SYMS as 1-byte data placeholders to an EXISTING generated
    LinkStubs.c, touching nothing else in the file. Used when the normal log-driven path did not
    run (a clean/stale top-build.log has no undefined-symbol lines to say anything about symbols
    nothing currently references, so it would otherwise leave the file untouched forever and these
    two would never be added). Returns True if the file was modified."""
    if not os.path.isfile(out):
        return False
    with open(out, encoding="utf-8") as f:
        text = f.read()
    missing = [s for s in EXTRA_DATA_SYMS if re.search(r"\b{}\[".format(re.escape(s)), text) is None]
    if not missing:
        return False
    anchor = "// functions = no-op returning 0. Real impls: R5/R6 (desktop).\n\n"
    insertion = "".join("unsigned char {}[1];\n".format(s) for s in missing)
    idx = text.find(anchor)
    if idx == -1:
        text = text.rstrip("\n") + "\n" + insertion
    else:
        insert_at = idx + len(anchor)
        text = text[:insert_at] + insertion + text[insert_at:]
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    print("gen_link_stubs: appended {} EXTRA_DATA_SYMS placeholder(s) to {}".format(
        len(missing), os.path.relpath(out, REPO)))
    return True


# Heuristic: linker-marker / global / blob symbols are DATA; everything else is a function.
DATA_RE = re.compile(
    r"(_VRAM(_END)?$|_ROM_(START|END)$|_BSS_(START|END)$|_DATA_(START|END|SIZE)$"
    r"|_TEXT_(START|END)$|_RODATA_END$|^ovl_i|^framebuffer|^D_[0-9A-Fa-f]|^g[A-Z]|^s[A-Z]"
    r"|^a[A-Z]|fifo(Text|Data)(Start|End)$|^rspboot|^leoBootID$|qnan|^unk_|osViMode|^osTvType$)"
)


def parse_log_symbols(log_path):
    syms = set()
    if not os.path.isfile(log_path):
        return syms
    with open(log_path, encoding="utf-8", errors="ignore") as f:
        for line in f:
            m = re.search(r"undefined symbol: (\S+)", line)
            if m:
                s = m.group(1).strip()
                if s not in EXCLUDE:
                    syms.add(s)
    return syms


def experimental_banner(profile):
    region = profile.split("/")[0].upper()
    lines = []
    lines.append("// ===========================================================================")
    lines.append("// EXPERIMENTAL {} LINK STUBS -- (C-R5.2).".format(profile.upper()))
    lines.append("//")
    lines.append("// The undefined-symbol list that drives function stubs comes from top-build.log,")
    lines.append("// which is a US (VERSION_US) build's link log. A {} (VERSION_JP) build references a".format(region))
    lines.append("// DIFFERENT undefined-symbol set (JP machine-name text, kanji glyph tables, JP-only")
    lines.append("// Leo font helpers, JP audio-lib branches). This file MUST be regenerated from a {}".format(region))
    lines.append("// build's OWN top-build.log once the {} target links far enough to produce one:".format(region))
    lines.append("//   1. Empty this file's function-stub section, configure -DGDX_BUILD_JP=ON, build.")
    lines.append("//   2. Capture the link log to top-build.log.")
    lines.append("//   3. python tools/gen_link_stubs.py --profile {}".format(profile))
    lines.append("//")
    lines.append("// The {} ROM is NOT on disk in this repo; this is OWNER-RUN-REQUIRED scaffolding.".format(region))
    lines.append("// ===========================================================================")
    return "\n".join(lines) + "\n"


def resolve_paths(profile, out_override):
    parts = profile.split("/")
    asset_yaml_dir = os.path.join(REPO, "decomp", "assets", "yaml", *parts)
    if out_override:
        out = os.path.abspath(out_override)
    elif profile == "us/rev0":
        out = os.path.join(REPO, "port", "gen", "LinkStubs.c")
    else:
        out = os.path.join(REPO, "port", "gen", "LinkStubs.{}.c".format(parts[0]))
    return asset_yaml_dir, out


def write_stubs(out, profile, data_syms, func_syms, table_sizes, placeholder_only):
    is_us_default = (profile == "us/rev0")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        if not is_us_default:
            f.write(experimental_banner(profile))
        f.write("// AUTO-GENERATED by tools/gen_link_stubs.py. Placeholder defs for unported symbols.\n")
        f.write("// Data = 1-byte buffers (yaml table symbols at their real range size);\n")
        f.write("// functions = no-op returning 0. Real impls: R5/R6 (desktop).\n\n")
        if placeholder_only:
            f.write("// NOTE (%s): top-build.log had no undefined symbols when this was generated, so the\n" % profile)
            f.write("// function-stub section below is EMPTY. Only the yaml table-symbol data definitions\n")
            f.write("// (log-independent) are present. Regenerate from a JP build's link log to fill the\n")
            f.write("// function stubs (see the banner above).\n\n")
        # Data section: log-derived data symbols. For US (and any real log-driven run) this is the
        # ONLY source — byte-identical to the historical generator. The placeholder-only seed has no
        # log, so it instead emits every yaml table symbol at its real size (log-independent) so the
        # JP gen file still defines those interior-indexed tables.
        emitted = set()
        for s in data_syms:
            size = "0x{:X}".format(table_sizes[s]) if s in table_sizes else "1"
            f.write("unsigned char {}[{}];\n".format(s, size))
            emitted.add(s)
        if placeholder_only:
            for s in sorted(table_sizes):
                if s not in emitted:
                    f.write("unsigned char {}[0x{:X}];\n".format(s, table_sizes[s]))
                    emitted.add(s)
        f.write("\n")
        for s in func_syms:
            f.write("long {}() {{ return 0; }}\n".format(s))
    print("link stubs [{}]: {} data, {} funcs -> {}".format(
        profile, len(emitted), len(func_syms), out))


def generate(profile, out_override):
    asset_yaml_dir, out = resolve_paths(profile, out_override)
    is_us_default = (profile == "us/rev0")
    syms = parse_log_symbols(LOG)
    table_sizes = yaml_table_sizes(asset_yaml_dir)

    if not syms:
        # Guard: LinkStubs is generated from a FAILING build log's undefined symbols. If the log is a
        # clean build (no undefined symbols), do NOT overwrite the US file — that would wipe existing
        # (possibly hand-tuned) stubs. To regenerate US, first empty/remove LinkStubs.c, build (it
        # will fail), then run this against that log.
        if is_us_default:
            # A3: a clean log can never speak to EXTRA_DATA_SYMS (nothing currently references
            # them, so they never fail to link), but they still need to exist in the committed
            # file. Patch them in place instead of the old bare return so this generator call
            # stays the single source of truth for LinkStubs.c, without a full log-driven regen.
            if not ensure_extra_data_syms(out):
                print("gen_link_stubs: no 'undefined symbol' lines in {} — leaving {} unchanged.".format(
                    os.path.basename(LOG), os.path.relpath(out, REPO)))
            return
        # Non-US: still emit a placeholder seed carrying only the yaml table-symbol data defs so the
        # JP gen file compiles; function stubs come later from a JP build log (EXPERIMENTAL).
        write_stubs(out, profile, data_syms=sorted(EXTRA_DATA_SYMS), func_syms=[], table_sizes=table_sizes,
                    placeholder_only=True)
        return

    data = sorted(set(s for s in syms if DATA_RE.search(s)) | set(EXTRA_DATA_SYMS))
    funcs = sorted(s for s in syms if not DATA_RE.search(s))
    write_stubs(out, profile, data_syms=data, func_syms=funcs, table_sizes=table_sizes,
                placeholder_only=False)


def main():
    ap = argparse.ArgumentParser(description="Generate port/gen/LinkStubs[.<profile>].c")
    ap.add_argument("--profile", default="us/rev0", choices=["us/rev0", "jp/rev0"],
                    help="asset recipe profile (default: us/rev0)")
    ap.add_argument("--out", default=None, help="override output path")
    args = ap.parse_args()
    generate(args.profile, args.out)


if __name__ == "__main__":
    main()
