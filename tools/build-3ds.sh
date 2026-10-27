#!/usr/bin/env bash
# tools/build-3ds.sh — single entry point for a Nintendo 3DS build: name a
# target, get the right CMake flags, a built .3dsx/.elf/.cia and a renamed,
# checksummed drop in dist/3ds/. Nobody has to remember the flag combinations
# below by hand.
#
# Usage:
#   tools/build-3ds.sh <target>... [--jobs N] [--out DIR] [--no-cia] [--clean] [--zip [VERSION]]
#   tools/build-3ds.sh --list
#   tools/build-3ds.sh -h | --help
#
# Targets:
#   o3ds     Old 3DS profile    (-DGDX_3DS_O3DS=ON)
#   n3ds     New 3DS profile    (the default profile — no extra flags)
#   o3ds-ek  Old 3DS + Expansion Kit (-DGDX_3DS_O3DS=ON -DGDX_3DS_EXPANSION_KIT=ON)
#   n3ds-ek  New 3DS + Expansion Kit (-DGDX_3DS_EXPANSION_KIT=ON)
#   all      All four of the above.
#
# Every target always gets, explicitly, on every configure:
#   -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/3DS.cmake -DGDX_PLATFORM_3DS=ON
#   -DCMAKE_BUILD_TYPE=Release -DGDX_3DS_O3DS=<ON|OFF> -DGDX_3DS_EXPANSION_KIT=<ON|OFF>
#   -DGDX_3DS_CIA_UNIQUE_ID=<0xff3d5|0xff3d6> -DGDX_3DS_TITLE_TAG=<""|EK>
#   -DGDX_ALLOW_JP_INPUTS=ON
# The O3DS/EK/UNIQUE_ID/TITLE_TAG values are passed on EVERY configure, ON or
# OFF, so a build-3ds-<target> dir reused from a previous run (or left behind
# by some other invocation) can never keep a stale cached value — see
# port/3ds/CMakeLists.txt around lines 50-110 and 195-240 for where CMake
# would otherwise cache them. GDX_ALLOW_JP_INPUTS defaults ON for 3DS, but a
# build dir configured before that default changed keeps its cached OFF (an
# option() never overwrites a cached value), so it is passed explicitly too.
#
# Each target builds into build-3ds-<target>/ at the repo root (reused across
# runs — only --clean wipes it first) and the default build target (the
# .3dsx + its .elf) plus, unless --no-cia, the CIA target G-Diffuser-3DS-cia.
# The CIA target only exists when makerom + bannertool were found at configure
# time (tools/3ds-bin/, see port/3ds/packaging/README.md); if it is missing
# this script warns and skips the CIA rather than failing the build.
#
# Renamed, checksummed output lands in <out> (default dist/3ds/, relative to
# the repo root unless given as an absolute path):
#   G-Diffuser-3DS-O3DS.{3dsx,elf,cia}  G-Diffuser-3DS-N3DS.{3dsx,elf,cia}
#   G-Diffuser-3DS-O3DS-EK.{3dsx,elf,cia}  G-Diffuser-3DS-N3DS-EK.{3dsx,elf,cia}
#   SHA256SUMS   (covers everything currently in <out>)
#   BUILD-INFO.txt (git describe/HEAD, UTC date, flags for the targets THIS run built)
#
# --zip [VERSION] additionally packages each target built THIS run as
# <out>/G-Diffuser-3DS-<NAME>[-vVERSION].zip, laid out exactly like an SD card drop:
#   3ds/G-Diffuser-3DS-<NAME>.3dsx   (Homebrew Launcher: copy the 3ds/ folder onto the card)
#   G-Diffuser-3DS-<NAME>.cia        (at the zip root: install with your usual CIA tool)
#   README.txt                       (where fzerox.o2r/gdiffuser.o2r go, and, for an -EK
#                                      target, where its .ndd goes)
# VERSION is optional and cosmetic (only the zip's filename); omit it for an unversioned
# zip. The .github/workflows/release-3ds.yml CI job calls this flag so the zip layout has
# exactly one implementation, not one in this script and a second copy in the workflow.
#
# Written for macOS's stock bash 3.2 (no associative arrays, BSD stat) and the
# devkitpro/devkitarm Docker image used by CI — see tools/ci-3ds.sh, the
# existing build+test gate, which this script does not change.
#
# DEVKITPRO defaults to /opt/devkitpro; export DEVKITPRO to override.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"

ALL_TARGETS="o3ds n3ds o3ds-ek n3ds-ek"
JOBS="$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)"
OUT_DIR="dist/3ds"
NO_CIA=0
DO_CLEAN=0
DO_ZIP=0
ZIP_VERSION=""
REQUESTED=()

# ------------------------------------------------------------------- usage --
usage() {
    sed -n '2,60p' "$0"
}

target_description() { # target
    case "$1" in
        o3ds)    echo "Old 3DS profile — 8 MiB RDRAM arena + linear-heap pin, cartridge-only" ;;
        n3ds)    echo "New 3DS profile — the default profile (no extra flags), cartridge-only" ;;
        o3ds-ek) echo "Old 3DS + Expansion Kit (64DD course editor / machine creator)" ;;
        n3ds-ek) echo "New 3DS + Expansion Kit (64DD course editor / machine creator)" ;;
        all)     echo "All four targets above" ;;
        *)       return 1 ;;
    esac
}

list_targets() {
    echo "targets:"
    local t
    for t in $ALL_TARGETS all; do
        printf '  %-8s %s\n' "$t" "$(target_description "$t")"
    done
}

known_target() { # target
    local t
    for t in $ALL_TARGETS; do
        if [ "$t" = "$1" ]; then return 0; fi
    done
    return 1
}

# -------------------------------------------------------------- arg parsing --
if [ $# -eq 0 ]; then
    usage
    exit 1
fi

while [ $# -gt 0 ]; do
    case "$1" in
        -h|--help)
            usage
            exit 0
            ;;
        --list)
            list_targets
            exit 0
            ;;
        --jobs)
            shift
            JOBS="${1:?--jobs needs a value}"
            ;;
        --out)
            shift
            OUT_DIR="${1:?--out needs a value}"
            ;;
        --no-cia)
            NO_CIA=1
            ;;
        --clean)
            DO_CLEAN=1
            ;;
        --zip)
            DO_ZIP=1
            # VERSION is optional: consume the next arg only if there IS one and it's
            # not another flag, a target name, or "all" (so `--zip` with no version
            # followed by a target still parses the target as a target, not a version).
            if [ $# -gt 1 ]; then
                case "$2" in
                    -*|all) ;;
                    *) if ! known_target "$2"; then
                           ZIP_VERSION="$2"
                           shift
                       fi ;;
                esac
            fi
            ;;
        all)
            REQUESTED+=($ALL_TARGETS)
            ;;
        -*)
            echo "build-3ds.sh: unknown option: $1 (try --help)" >&2
            exit 2
            ;;
        *)
            if ! known_target "$1"; then
                echo "build-3ds.sh: unknown target: $1 (try --list)" >&2
                exit 2
            fi
            REQUESTED+=("$1")
            ;;
    esac
    shift
done

if [ "${#REQUESTED[@]}" -eq 0 ]; then
    echo "build-3ds.sh: no target given (try --list)" >&2
    exit 2
fi

# De-duplicate, preserving first-seen order (bash 3.2: no associative arrays).
TARGETS=()
for t in "${REQUESTED[@]}"; do
    seen=0
    for u in "${TARGETS[@]:-}"; do
        if [ "$u" = "$t" ]; then seen=1; break; fi
    done
    if [ "$seen" -eq 0 ]; then TARGETS+=("$t"); fi
done

if [ "${OUT_DIR#/}" = "${OUT_DIR}" ]; then
    OUT_DIR="${ROOT}/${OUT_DIR}"
fi

# --------------------------------------------------------------- preflight --
preflight() {
    local ok=1
    if [ ! -e "${DEVKITPRO}/cmake/3DS.cmake" ]; then
        echo "MISSING: ${DEVKITPRO}/cmake/3DS.cmake — is devkitPro installed? (export DEVKITPRO to override /opt/devkitpro)" >&2
        ok=0
    fi
    if [ ! -e "${DEVKITPRO}/devkitARM/bin/arm-none-eabi-gcc" ]; then
        echo "MISSING: ${DEVKITPRO}/devkitARM/bin/arm-none-eabi-gcc — devkitARM not found under DEVKITPRO" >&2
        ok=0
    fi
    if ! command -v cmake >/dev/null 2>&1; then
        echo "MISSING: cmake on PATH" >&2
        ok=0
    fi
    if [ "$ok" -ne 1 ]; then
        echo "build-3ds.sh: devkitPro toolchain not fully present, see above" >&2
        exit 1
    fi
}

if command -v shasum >/dev/null 2>&1; then
    sha256_sum() { shasum -a 256 "$@"; }
elif command -v sha256sum >/dev/null 2>&1; then
    sha256_sum() { sha256sum "$@"; }
else
    echo "build-3ds.sh: need shasum or sha256sum on PATH" >&2
    exit 1
fi

# -------------------------------------------------------- per-target flags --
# Echoes the target's CMake flags, one per line, so the caller can read them
# into an array with a `while read` loop (bash 3.2 has no `readarray`).
target_cmake_flags() { # target
    local t="$1" o3ds=OFF ek=OFF uid=0xff3d5 tag=""
    case "$t" in
        o3ds)    o3ds=ON ;;
        n3ds)    ;;
        o3ds-ek) o3ds=ON; ek=ON ;;
        n3ds-ek) ek=ON ;;
    esac
    if [ "$ek" = ON ]; then
        uid=0xff3d6
        tag=EK
    fi
    echo "-DCMAKE_TOOLCHAIN_FILE=${DEVKITPRO}/cmake/3DS.cmake"
    echo "-DGDX_PLATFORM_3DS=ON"
    echo "-DCMAKE_BUILD_TYPE=Release"
    echo "-DGDX_3DS_O3DS=${o3ds}"
    echo "-DGDX_3DS_EXPANSION_KIT=${ek}"
    echo "-DGDX_3DS_CIA_UNIQUE_ID=${uid}"
    echo "-DGDX_3DS_TITLE_TAG=${tag}"
    echo "-DGDX_ALLOW_JP_INPUTS=ON"
}

target_artifact_suffix() { # target
    case "$1" in
        o3ds)    echo "O3DS" ;;
        n3ds)    echo "N3DS" ;;
        o3ds-ek) echo "O3DS-EK" ;;
        n3ds-ek) echo "N3DS-EK" ;;
    esac
}

# Same zip layout .github/workflows/release-3ds.yml's console job produces, so there is
# one implementation: 3ds/<name>.3dsx, <name>.cia at the zip root, a README.txt with the
# SD-card paths (and, for an -EK target, where its .ndd goes), plus LICENSE,
# THIRD_PARTY_NOTICES.md and LICENSES/ -- the MIT/ISC/zlib/CC0 components compiled into the
# binary require their notices to travel with it. has_cia=0 skips the .cia entry (e.g.
# --no-cia, or makerom/bannertool were never found) rather than failing.
make_zip() { # target suffix has_cia
    local t="$1" suffix="$2" has_cia="$3"
    local name="G-Diffuser-3DS-${suffix}"
    local zip_name="${name}.zip"
    if [ -n "$ZIP_VERSION" ]; then
        zip_name="${name}-v${ZIP_VERSION}.zip"
    fi

    if ! command -v zip >/dev/null 2>&1; then
        echo "WARNING: 'zip' not on PATH — skipping the --zip package for '${t}'" >&2
        return 0
    fi

    local console_line="Old 3DS / Old 2DS"
    case "$suffix" in
        N3DS|N3DS-EK) console_line="New 3DS / New 2DS XL" ;;
    esac
    local ek_line=""
    case "$suffix" in
        *-EK)
            ek_line=$'\nThis is the Expansion Kit title: it installs alongside the normal game. For the\nExpansion Kit modes (course editor, machine creator) put your own dump of the retail\nJapanese 64DD disk at:\n  3ds/gdiffuser/media/baserom.jp.ek.ndd\nFan-translated disk images are not supported in this release (they can hang at boot).\nThe first boot checks the whole image once (about a minute). Without a disk it plays\nthe normal game.\n'
            ;;
    esac

    local zip_work="${OUT_DIR}/.zip-${t}"
    rm -rf "$zip_work"
    mkdir -p "${zip_work}/3ds"
    cp "${OUT_DIR}/${name}.3dsx" "${zip_work}/3ds/${name}.3dsx"
    if [ "$has_cia" -eq 1 ]; then
        cp "${OUT_DIR}/${name}.cia" "${zip_work}/${name}.cia"
    fi
    cp "${ROOT}/LICENSE" "${ROOT}/THIRD_PARTY_NOTICES.md" "${zip_work}/"
    cp -R "${ROOT}/LICENSES" "${zip_work}/LICENSES"
    printf '%s\n' \
        "G-Diffuser 3DS -- ${suffix} build${ZIP_VERSION:+ (v${ZIP_VERSION})}" \
        "Console: ${console_line}" \
        "" \
        "Two ways to run it:" \
        "  * Homebrew Launcher: copy 3ds/${name}.3dsx to the SD card's /3ds/ folder, then launch it" \
        "    from the Homebrew Launcher." \
        "  * HOME menu: install ${name}.cia with your usual CIA installer (needs CFW)." \
        "" \
        "Either way, this .3dsx/.cia is the ENGINE ONLY. It reads your own F-Zero X dump: run" \
        "tools/prebake/prebake.py from the repository (with gdx-extract) against your own" \
        "cartridge/ROM dump, then copy the two files it writes onto the SD card at:" \
        "  3ds/gdiffuser/" \
        "${ek_line}" \
        "No ROM, disk image, or game assets are included in this download: the binaries contain" \
        "code compiled from the community decompilation, and you must dump your own cartridge" \
        "(and, for the Expansion Kit, your own 64DD disk)." \
        "" \
        "License: see LICENSE, THIRD_PARTY_NOTICES.md and LICENSES/ in this zip." \
        > "${zip_work}/README.txt"

    rm -f "${OUT_DIR}/${zip_name}"
    ( cd "$zip_work" && zip -q -r "../${zip_name}" README.txt LICENSE THIRD_PARTY_NOTICES.md LICENSES 3ds $( [ "$has_cia" -eq 1 ] && echo "${name}.cia" ) )
    rm -rf "$zip_work"
    echo "zipped: ${OUT_DIR}/${zip_name}"
    ZIP_MADE_NAME="$zip_name"
}

# ------------------------------------------------------------- build a target --
# Populates the RESULT_* globals for the summary table; appends to
# BUILD_INFO_LINES for BUILD-INFO.txt.
RESULT_TARGETS=()
RESULT_FILES=()   # one entry per target: space-separated renamed basenames
BUILD_INFO_LINES=()

build_one_target() { # target
    local t="$1"
    local build_dir="${ROOT}/build-3ds-${t}"
    local suffix
    suffix="$(target_artifact_suffix "$t")"

    echo
    echo "=== ${t} (build-3ds-${t}, dist name G-Diffuser-3DS-${suffix}) ==="

    if [ "$DO_CLEAN" -eq 1 ] && [ -d "$build_dir" ]; then
        echo "--clean: removing ${build_dir}"
        rm -rf "$build_dir"
    fi

    local flags=()
    while IFS= read -r line; do
        flags+=("$line")
    done < <(target_cmake_flags "$t")
    BUILD_INFO_LINES+=("${t} (G-Diffuser-3DS-${suffix}): ${flags[*]}")

    echo "configuring: cmake -S ${ROOT} -B ${build_dir} ${flags[*]}"
    if ! cmake -S "${ROOT}" -B "${build_dir}" "${flags[@]}" > "${build_dir}.configure.log" 2>&1; then
        echo "build-3ds.sh: configure FAILED for target '${t}' — see ${build_dir}.configure.log" >&2
        tail -40 "${build_dir}.configure.log" >&2
        exit 1
    fi

    echo "building (jobs=${JOBS})"
    if ! cmake --build "${build_dir}" -j"${JOBS}" > "${build_dir}.build.log" 2>&1; then
        echo "build-3ds.sh: build FAILED for target '${t}' — see ${build_dir}.build.log" >&2
        tail -60 "${build_dir}.build.log" >&2
        exit 1
    fi

    local game_dir="${build_dir}/port/3ds"
    local game_3dsx="${game_dir}/G-Diffuser-3DS.3dsx"
    local game_elf="${game_dir}/G-Diffuser-3DS.elf"
    if [ ! -f "$game_3dsx" ] || [ ! -f "$game_elf" ]; then
        echo "build-3ds.sh: build reported success but ${game_3dsx} / ${game_elf} is missing for target '${t}'" >&2
        exit 1
    fi

    local have_cia=0
    local makerom bannertool
    makerom="$(sed -n 's/^GDX_MAKEROM_EXE:FILEPATH=//p' "${build_dir}/CMakeCache.txt")"
    bannertool="$(sed -n 's/^GDX_BANNERTOOL_EXE:FILEPATH=//p' "${build_dir}/CMakeCache.txt")"
    case "$makerom" in *-NOTFOUND|"") makerom="" ;; esac
    case "$bannertool" in *-NOTFOUND|"") bannertool="" ;; esac
    if [ -n "$makerom" ] && [ -n "$bannertool" ]; then
        have_cia=1
    fi

    local game_cia="${game_dir}/G-Diffuser-3DS.cia"
    if [ "$NO_CIA" -eq 1 ]; then
        : # caller explicitly opted out — skip quietly.
    elif [ "$have_cia" -eq 1 ]; then
        echo "building CIA (G-Diffuser-3DS-cia)"
        if ! cmake --build "${build_dir}" -j"${JOBS}" --target G-Diffuser-3DS-cia \
                >> "${build_dir}.build.log" 2>&1; then
            echo "build-3ds.sh: CIA build FAILED for target '${t}' — see ${build_dir}.build.log" >&2
            tail -40 "${build_dir}.build.log" >&2
            exit 1
        fi
        if [ ! -f "$game_cia" ]; then
            echo "build-3ds.sh: G-Diffuser-3DS-cia target ran but ${game_cia} is missing for target '${t}'" >&2
            exit 1
        fi
    else
        echo "WARNING: makerom + bannertool not found (tools/3ds-bin/) — skipping the CIA for '${t}'" >&2
    fi

    mkdir -p "$OUT_DIR"
    local files=()
    local zip_has_cia=0
    cp "$game_3dsx" "${OUT_DIR}/G-Diffuser-3DS-${suffix}.3dsx"
    files+=("G-Diffuser-3DS-${suffix}.3dsx")
    cp "$game_elf" "${OUT_DIR}/G-Diffuser-3DS-${suffix}.elf"
    files+=("G-Diffuser-3DS-${suffix}.elf")
    if [ "$NO_CIA" -eq 0 ] && [ -f "$game_cia" ]; then
        cp "$game_cia" "${OUT_DIR}/G-Diffuser-3DS-${suffix}.cia"
        files+=("G-Diffuser-3DS-${suffix}.cia")
        zip_has_cia=1
    fi

    if [ "$DO_ZIP" -eq 1 ]; then
        ZIP_MADE_NAME=""
        make_zip "$t" "$suffix" "$zip_has_cia"
        if [ -n "$ZIP_MADE_NAME" ]; then
            files+=("$ZIP_MADE_NAME")
        fi
    fi

    RESULT_TARGETS+=("$t")
    RESULT_FILES+=("${files[*]}")
}

# ------------------------------------------------------------------- main --
preflight

for t in "${TARGETS[@]}"; do
    build_one_target "$t"
done

echo
echo "writing ${OUT_DIR}/BUILD-INFO.txt"
GIT_DESCRIBE="$(cd "$ROOT" && git describe --always --dirty 2>/dev/null || echo unknown)"
GIT_HEAD="$(cd "$ROOT" && git rev-parse HEAD 2>/dev/null || echo unknown)"
{
    echo "G-Diffuser 3DS build"
    echo "git describe: ${GIT_DESCRIBE}"
    echo "git HEAD:      ${GIT_HEAD}"
    echo "built (UTC):   $(date -u '+%Y-%m-%d %H:%M:%S')"
    echo "jobs:          ${JOBS}"
    echo
    echo "Targets built by this invocation (flags per target):"
    for line in "${BUILD_INFO_LINES[@]}"; do
        echo "  ${line}"
    done
} > "${OUT_DIR}/BUILD-INFO.txt"

echo "writing ${OUT_DIR}/SHA256SUMS (covers everything currently in ${OUT_DIR})"
(
    cd "$OUT_DIR"
    rm -f SHA256SUMS SHA256SUMS.tmp
    sha256_sum -- * > SHA256SUMS.tmp
    mv SHA256SUMS.tmp SHA256SUMS
)

echo
echo "================ build-3ds summary ================"
printf '%-10s %-34s %s\n' 'target' 'file' 'size'
printf '%-10s %-34s %s\n' '------' '----' '----'
for i in "${!RESULT_TARGETS[@]}"; do
    t="${RESULT_TARGETS[$i]}"
    for f in ${RESULT_FILES[$i]}; do
        size="$(du -h "${OUT_DIR}/${f}" | cut -f1 | tr -d ' ')"
        printf '%-10s %-34s %s\n' "$t" "$f" "$size"
    done
done
echo
echo "RESULT: OK — output in ${OUT_DIR}"
