#!/usr/bin/env python3
"""prebake.py -- PC-side asset pre-bake for the Nintendo 3DS port (stream D).

The 3DS never runs Torch or Python: this script runs the EXISTING desktop
extraction flow (the deterministic `gdx-extract` built by the desktop CMake
tree) against a user-supplied F-Zero X US rev0 ROM and lays out the
sdmc:/3ds/gdiffuser/ folder the device expects:

    <out>/3ds/gdiffuser/fzerox.o2r     game archive (renamed generic.o2r)
    <out>/3ds/gdiffuser/gdiffuser.o2r  engine archive (tools/gen_f3d_o2r.py)

ROM validation mirrors the desktop port (port/gdx_extract_launch.cpp): the
whole-file SHA-1 must equal the US rev0 big-endian (.z64) hash. Everything
else -- JP, PAL, rev1, byte-swapped dumps -- fails with a diagnosis instead of
producing a broken archive. Archive validation mirrors first boot: SHA-256 and
central-directory record count against port/gen/gdx_o2r_expected.h.

Python 3.8+, standard library only. See README.md for the end-to-end flow.
"""

import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# US rev0 (big-endian .z64) -- the one supported profile on 3DS. Same constant
# as port/gdx_extract_launch.cpp kExpectedRomSha1Fallback; the desktop resolves
# it from decomp/config.yml first, so we do too.
US_REV0_SHA1_FALLBACK = "5f658e88ffa9de23cba6986a8fd3d3a90d7b4340"
# JP rev0, recognized only to give a better error (desktop treats JP as
# experimental; the 3DS MVP is US-only -- docs/research/3ds-port-plan.md).
JP_REV0_SHA1 = "a418b0151521b76691fa03f8658c8b567c69498b"
# Decimal of the US rev0 ROM CRC 0x78D90EB3. REQUIRED on every gdx-extract run:
# Torch only emits the portVersion record when -u is passed, and without it the
# archive is one record short and can never match the golden SHA-256
# (tools/o2r_harness/README.md "Key facts").
US_REV0_VERSION_ARG = "2027490995"

EXPECTED_HEADER = os.path.join(REPO, "port", "gen", "gdx_o2r_expected.h")
GEN_F3D_O2R = os.path.join(REPO, "tools", "gen_f3d_o2r.py")
RECIPES_DIR = os.path.join(REPO, "decomp")

Z64_MAGIC = b"\x80\x37\x12\x40"  # big-endian (native)
V64_MAGIC = b"\x37\x80\x40\x12"  # byte-swapped
N64_MAGIC = b"\x40\x12\x37\x80"  # little-endian


def fail(msg):
    sys.stderr.write("prebake: ERROR: %s\n" % msg)
    sys.exit(1)


def info(msg):
    print("prebake: %s" % msg)


def sha1_file(path):
    h = hashlib.sha1()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def expected_rom_sha1():
    """US rev0 ROM SHA-1 from decomp/config.yml (the `assets/yaml/us/rev0`
    profile selector), falling back to the compiled-in constant -- the same
    resolution order as the desktop launcher."""
    config = os.path.join(RECIPES_DIR, "config.yml")
    try:
        with open(config, "r", encoding="utf-8", errors="replace") as f:
            text = f.read()
    except OSError:
        return US_REV0_SHA1_FALLBACK
    # config.yml keys each recipe tree by ROM SHA-1:
    #   <sha1>:\n  path: assets/yaml/us/rev0
    for match in re.finditer(r"^([0-9a-fA-F]{40}):", text, re.MULTILINE):
        sha = match.group(1).lower()
        tail = text[match.end():match.end() + 200]
        if "assets/yaml/us/rev0" in tail:
            return sha
    return US_REV0_SHA1_FALLBACK


def rom_header_diagnosis(rom_path):
    """Best-effort human diagnosis of a ROM that failed the SHA-1 gate, from
    the 64-byte header: byte order, internal name, region code, revision."""
    try:
        with open(rom_path, "rb") as f:
            header = f.read(64)
    except OSError as e:
        return "unreadable ROM header (%s)" % e
    if len(header) < 64:
        return "file is too small to be an N64 ROM"

    magic = header[:4]
    if magic == V64_MAGIC:
        return ("this is a byte-swapped (.v64) dump. Convert it to big-endian .z64 "
                "(e.g. with tool64 or `dd conv=swab`) and re-run")
    if magic == N64_MAGIC:
        return "this is a little-endian (.n64) dump. Convert it to big-endian .z64 and re-run"
    if magic != Z64_MAGIC:
        return "not an N64 ROM (bad header magic %s)" % magic[:4].hex()

    name = header[0x20:0x34].decode("ascii", errors="replace").strip("\x00 ")
    region = chr(header[0x3E])
    revision = header[0x3F]
    regions = {
        "E": "US/NTSC",
        "J": "Japan",
        "P": "PAL/Europe",
        "U": "PAL/Australia",
        "D": "PAL/Germany",
        "F": "PAL/France",
    }
    region_name = regions.get(region, "unknown region '%s'" % region)
    return ("big-endian N64 ROM \"%s\", region %s, revision %d -- not the supported US rev0 dump"
            % (name, region_name, revision))


def validate_rom(rom_path):
    if not os.path.isfile(rom_path):
        fail("ROM not found: %s" % rom_path)

    us_sha1 = expected_rom_sha1()
    got = sha1_file(rom_path)
    if got == us_sha1:
        info("ROM accepted: F-Zero X US rev0 (.z64), sha1 %s" % got)
        return
    if got == JP_REV0_SHA1:
        fail("this is the JP rev0 ROM. The 3DS port supports the US rev0 cartridge only "
             "(the desktop port's JP support is experimental and has no validated golden "
             "archive). Supply the US rev0 .z64 dump.")
    fail("ROM SHA-1 mismatch.\n"
         "  got:      %s\n"
         "  expected: %s (F-Zero X US rev0, big-endian .z64)\n"
         "  detail:   %s" % (got, us_sha1, rom_header_diagnosis(rom_path)))


def find_extractor(args):
    exe = "gdx-extract.exe" if os.name == "nt" else "gdx-extract"
    candidates = []
    if args.extractor:
        candidates.append(args.extractor)
    if args.build_dir:
        # Desktop CMake ExternalProject install prefix (port/CMakeLists.txt).
        candidates.append(os.path.join(args.build_dir, "gdx-extract", "install", "bin", exe))
        # Deployed copy beside the desktop executable, any config subdir.
        for sub in ("port", os.path.join("port", "Release"), os.path.join("port", "Debug")):
            candidates.append(os.path.join(args.build_dir, sub, exe))
    for c in candidates:
        if c and os.path.isfile(c) and os.access(c, os.X_OK):
            return os.path.abspath(c)
    fail("could not find the gdx-extract binary.\n"
         "  Build it from the desktop tree first:\n"
         "    cmake -S . -B build && cmake --build build --target gdx-extract\n"
         "  then pass --build-dir build (or --extractor <path/to/gdx-extract>).\n"
         "  Tried: %s" % ", ".join(candidates or ["<nothing>"]))


def parse_expected_header():
    """GDX_O2R_EXPECTED_SHA256 / GDX_O2R_EXPECTED_ENTRY_COUNT from the golden
    header first boot validates against (port/gen/gdx_o2r_expected.h)."""
    try:
        with open(EXPECTED_HEADER, "r", encoding="utf-8") as f:
            text = f.read()
    except OSError:
        return None, None
    sha = re.search(r'#define\s+GDX_O2R_EXPECTED_SHA256\s+"([0-9a-fA-F]{64})"', text)
    count = re.search(r"#define\s+GDX_O2R_EXPECTED_ENTRY_COUNT\s+(\d+)", text)
    return (sha.group(1).lower() if sha else None, int(count.group(1)) if count else None)


def run_extractor(extractor, rom_path, tmp_dir):
    if not os.path.isdir(RECIPES_DIR):
        fail("recipe tree missing at %s (checkout incomplete?)" % RECIPES_DIR)
    # Exact desktop invocation shape (port/gdx_extract_launch.cpp): NEVER pass
    # -v (debug mode dumps entries into the CWD); pass -d explicitly.
    cmd = [extractor, "o2r", rom_path, "-s", RECIPES_DIR, "-d", tmp_dir, "-u", US_REV0_VERSION_ARG]
    info("running: %s" % " ".join(cmd))
    result = subprocess.run(cmd, cwd=tmp_dir)
    if result.returncode != 0:
        fail("gdx-extract exited with code %d" % result.returncode)
    out = os.path.join(tmp_dir, "generic.o2r")
    if not os.path.isfile(out):
        fail("gdx-extract succeeded but produced no %s" % out)
    return out


def validate_archive(archive_path, skip_golden):
    with zipfile.ZipFile(archive_path) as z:
        records = len(z.infolist())  # central-directory records, dup-inclusive
    sha256 = sha256_file(archive_path)
    info("archive: %d records, sha256 %s" % (records, sha256))

    expected_sha, expected_count = parse_expected_header()
    if expected_sha is None or expected_count is None:
        if skip_golden:
            info("WARNING: golden header missing/unparsable; --skip-golden accepted the archive unverified")
            return
        fail("could not read the golden constants from %s (pass --skip-golden to bypass)" % EXPECTED_HEADER)

    if records != expected_count or sha256 != expected_sha:
        msg = ("archive failed the first-boot golden gate:\n"
               "  records: %d (expected %d)\n"
               "  sha256:  %s\n"
               "  golden:  %s\n"
               "The device would refuse this archive too. Rebuild gdx-extract from this "
               "checkout (stale extractors are the usual cause) and re-run."
               % (records, expected_count, sha256, expected_sha))
        if skip_golden:
            info("WARNING: %s\n  --skip-golden: continuing anyway" % msg)
        else:
            fail(msg)
    else:
        info("golden gate passed (matches port/gen/gdx_o2r_expected.h)")


# Entries the 3DS boot preload worker inflates for (almost) no gain: their payloads are
# already-compressed formats (ADPCM sample data, MIO0). Deflate saves 9% / 26% of them but
# costs a full CPU-bound inflate on the console at every boot (10.7 MB audio_table: ~4 s on
# core 2 in Azahar, longer on hardware) -- long enough that the title font's sample load
# blocks the audio producer (docs/research/bootaudio2-progress.md). Stored, the preload is
# a plain SD read. Everything else stays deflated so the archive stays small.
STORE_ENTRIES_DEFAULT = (
    "audio_blob/audio_table",                  # 10,744,340 B, deflate 9%
    "segment_blob/common_assets_compressed",   #  2,534,084 B, deflate 26% (MIO0 inside)
)


def store_large_entries(src, dst, names):
    """Rewrite src -> dst with `names` STORED (method 0) and every other entry re-deflated.
    Central-directory order, names, payload bytes, CRCs and the record count are preserved
    (only the compression method / compressed bytes change), so the runtime reads the same
    content; the golden SHA-256 gate applies to the extractor's output (validated before this
    step), not to the installed archive."""
    wanted = set(names)
    seen = []
    with zipfile.ZipFile(src) as zin, zipfile.ZipFile(dst, "w", zipfile.ZIP_DEFLATED) as zout:
        for zi in zin.infolist():
            data = zin.read(zi)
            out = zipfile.ZipInfo(zi.filename, date_time=zi.date_time)
            out.external_attr = zi.external_attr
            out.create_system = zi.create_system
            out.comment = zi.comment
            out.extra = zi.extra
            if zi.filename in wanted:
                out.compress_type = zipfile.ZIP_STORED
                seen.append((zi.filename, zi.file_size, zi.compress_size))
            else:
                out.compress_type = zipfile.ZIP_DEFLATED
            zout.writestr(out, data)
    with zipfile.ZipFile(src) as zin, zipfile.ZipFile(dst) as zout:
        a = [(i.filename, i.CRC, i.file_size) for i in zin.infolist()]
        b = [(i.filename, i.CRC, i.file_size) for i in zout.infolist()]
        if a != b:
            fail("store step changed the archive's entry list/CRCs (bug) -- not installing")
    for name, size, was in seen:
        info("stored %s: %d B (was deflated to %d B, %d%% smaller)" % (name, size, was, 100 - was * 100 // max(size, 1)))
    missing = wanted.difference(n for n, _, _ in seen)
    for name in sorted(missing):
        info("WARNING: --store-entry %s not present in the archive" % name)
    info("post-store archive: %d records, sha256 %s, %.1f MB (extractor output %.1f MB)"
         % (record_count_of(dst), sha256_file(dst), os.path.getsize(dst) / (1024.0 * 1024.0),
            os.path.getsize(src) / (1024.0 * 1024.0)))


def record_count_of(path):
    with zipfile.ZipFile(path) as z:
        return len(z.infolist())


def install(src, dst):
    """copy to <dst>.tmp then os.replace, so an aborted run never leaves a
    truncated archive at the final name (mirrors the desktop atomic install)."""
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    tmp = dst + ".tmp"
    shutil.copyfile(src, tmp)
    os.replace(tmp, dst)
    info("installed %s (%.1f MB)" % (dst, os.path.getsize(dst) / (1024.0 * 1024.0)))


def build_gdiffuser_o2r(dst, build_dir):
    """gdiffuser.o2r via the existing deterministic generator (stdlib-only).
    Falls back to copying a desktop build's copy when the generator cannot run
    (e.g. libultraship submodule not initialized)."""
    try:
        subprocess.run([sys.executable, GEN_F3D_O2R, dst + ".tmp"], check=True)
        os.replace(dst + ".tmp", dst)
        info("generated %s (tools/gen_f3d_o2r.py)" % dst)
        return
    except (subprocess.CalledProcessError, OSError) as e:
        info("WARNING: gen_f3d_o2r.py failed (%s); looking for a desktop build's gdiffuser.o2r" % e)
    if build_dir:
        for sub in ("port", os.path.join("port", "Release"), os.path.join("port", "Debug")):
            candidate = os.path.join(build_dir, sub, "gdiffuser.o2r")
            if os.path.isfile(candidate):
                install(candidate, dst)
                return
    fail("could not produce gdiffuser.o2r. Initialize submodules "
         "(git submodule update --init --recursive) so tools/gen_f3d_o2r.py can "
         "pack the Fast3D shaders, or pass --build-dir of a completed desktop build.")


def main():
    parser = argparse.ArgumentParser(
        description="Pre-bake F-Zero X assets for the 3DS port (US rev0 ROM -> sdmc:/3ds/gdiffuser/).")
    parser.add_argument("--rom", required=True, help="path to the F-Zero X US rev0 big-endian ROM (.z64)")
    parser.add_argument("--build-dir", default=None,
                        help="desktop CMake build dir (used to locate gdx-extract; see README)")
    parser.add_argument("--extractor", default=None, help="explicit path to the gdx-extract binary")
    parser.add_argument("--out", default=os.path.join(REPO, "dist", "sdmc"),
                        help="output staging dir (default: dist/sdmc). Copy its 3ds/ folder to the SD root.")
    parser.add_argument("--skip-golden", action="store_true",
                        help="do not fail on a golden SHA-256/entry-count mismatch (development only)")
    parser.add_argument("--store-entry", action="append", default=None, metavar="NAME",
                        help="archive entry to keep STORED (uncompressed) in fzerox.o2r; repeatable. "
                             "Default: %s" % ", ".join(STORE_ENTRIES_DEFAULT))
    parser.add_argument("--no-store", action="store_true",
                        help="install the extractor's archive as-is (every entry deflated; boot preload inflates)")
    args = parser.parse_args()

    if not args.extractor and not args.build_dir:
        fail("pass --build-dir <desktop build> or --extractor <path> so the script can find gdx-extract")

    validate_rom(args.rom)
    extractor = find_extractor(args)
    info("extractor: %s" % extractor)

    target_dir = os.path.join(args.out, "3ds", "gdiffuser")
    with tempfile.TemporaryDirectory(prefix="gdx-prebake-") as tmp_dir:
        generic = run_extractor(extractor, os.path.abspath(args.rom), tmp_dir)
        validate_archive(generic, args.skip_golden)
        if not args.no_store:
            stored = os.path.join(tmp_dir, "generic-stored.o2r")
            store_large_entries(generic, stored, args.store_entry or STORE_ENTRIES_DEFAULT)
            generic = stored
        # The runtime archive name is fzerox.o2r; generic.o2r is only the
        # extractor's output name (desktop first boot does the same rename).
        install(generic, os.path.join(target_dir, "fzerox.o2r"))

    build_gdiffuser_o2r(os.path.join(target_dir, "gdiffuser.o2r"), args.build_dir)

    print()
    info("done. SD card layout staged at: %s" % os.path.abspath(args.out))
    print("  Copy the 3ds/ folder onto the ROOT of your SD card (merge with the existing 3ds/):")
    print("    sdmc:/3ds/gdiffuser/fzerox.o2r")
    print("    sdmc:/3ds/gdiffuser/gdiffuser.o2r")
    print("  Saves will be created by the game under sdmc:/3ds/gdiffuser/saves/.")


if __name__ == "__main__":
    main()
