#!/usr/bin/env bash
# tools/ci-3ds.sh — one-command local CI for the Nintendo 3DS port.
#
# From a clean checkout (submodules initialized) this script:
#   1. verifies the devkitPro toolchain is present;
#   2. verifies/applies the port/3ds/patches/ submodule patches (idempotent —
#      each submodule's FULL ordered stack is checked as one combined patchset,
#      so already-applied trees are detected via a whole-stack reverse check
#      even when patches share overlapping context; a stack that neither applies
#      nor reverses cleanly fails LOUDLY, or warns under SKIP_PATCH_GATE=1);
#   3. builds the host-side unit tests that run on this machine and runs them
#      (gdx_gfx_pack_tests, gdx_gfx_convert_tests, gdx3ds_fs_host_smoke —
#      compiled directly with the host compiler; no desktop configure needed);
#   4. configures + builds the 3DS target (G-Diffuser-3DS.3dsx) and the DL
#      replay harness (gdx3ds-dl-tests.3dsx) with the devkitARM toolchain;
#   5. with --emu, boots the game .3dsx in Azahar for ${EMU_BOOT_SECONDS}s and
#      asserts the frame heartbeat advances with zero [fatal] lines
#      (honours the shared /tmp/azahar.lock emulator-lock protocol);
#   6. prints a PASS/FAIL summary table and exits nonzero on any failure.
#
# Usage:  tools/ci-3ds.sh [--emu] [--jobs N]
#
# Notes:
#  - Written for macOS's stock bash 3.2 (no associative arrays, BSD stat).
#  - DEVKITPRO defaults to /opt/devkitpro; export DEVKITPRO to override.
#  - The emulator smoke needs the prebaked archives staged on Azahar's virtual
#    SD card and log_filter=*:Debug — see docs/3DS-HARDWARE.md, "Building from source".

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
BUILD_3DS="${ROOT}/build-3ds"
HOST_TEST_DIR="${BUILD_3DS}/host-tests"
JOBS="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"
RUN_EMU=0

EMU_BOOT_SECONDS=45
AZAHAR_BIN="/Applications/Azahar.app/Contents/MacOS/azahar"
AZAHAR_LOG="${HOME}/Library/Application Support/Azahar/log/azahar_log.txt"
AZAHAR_CFG="${HOME}/Library/Application Support/Azahar/config/qt-config.ini"
AZAHAR_SDMC="${HOME}/Library/Application Support/Azahar/sdmc"
AZAHAR_LOCK="/tmp/azahar.lock"
AZAHAR_LOCK_POLL_SECONDS=30
AZAHAR_LOCK_WAIT_MAX_SECONDS=600   # 10 min
AZAHAR_LOCK_STALE_SECONDS=900      # steal if older than 15 min
HOLDING_LOCK=0

while [ $# -gt 0 ]; do
    case "$1" in
        --emu)  RUN_EMU=1 ;;
        --jobs) shift; JOBS="$1" ;;
        -h|--help)
            sed -n '2,27p' "$0"; exit 0 ;;
        *) echo "unknown option: $1 (try --help)" >&2; exit 2 ;;
    esac
    shift
done

# ---------------------------------------------------------------- reporting --
STEP_NAMES=()
STEP_RESULTS=()
STEP_DETAILS=()
FAILED=0

record() { # name result detail
    STEP_NAMES+=("$1"); STEP_RESULTS+=("$2"); STEP_DETAILS+=("$3")
    if [ "$2" = "FAIL" ]; then FAILED=1; fi
}

banner() { printf '\n=== %s ===\n' "$1"; }

summary() {
    printf '\n%s\n' '================ ci-3ds summary ================'
    printf '%-34s %-6s %s\n' 'step' 'result' 'detail'
    printf '%-34s %-6s %s\n' '----' '------' '------'
    local i
    for i in "${!STEP_NAMES[@]}"; do
        printf '%-34s %-6s %s\n' "${STEP_NAMES[$i]}" "${STEP_RESULTS[$i]}" "${STEP_DETAILS[$i]}"
    done
    if [ "$FAILED" -ne 0 ]; then
        printf '\nRESULT: FAIL\n'
    else
        printf '\nRESULT: PASS\n'
    fi
}

release_lock() {
    if [ "$HOLDING_LOCK" -eq 1 ]; then
        rmdir "$AZAHAR_LOCK" 2>/dev/null || true
        HOLDING_LOCK=0
    fi
}
trap 'release_lock' EXIT

# ---------------------------------------------------------------- preflight --
banner "preflight: toolchain"
PREFLIGHT_OK=1
for probe in \
    "${DEVKITPRO}/cmake/3DS.cmake" \
    "${DEVKITPRO}/devkitARM/bin/arm-none-eabi-gcc"; do
    if [ ! -e "$probe" ]; then
        echo "MISSING: $probe" >&2
        PREFLIGHT_OK=0
    fi
done
command -v cmake >/dev/null 2>&1 || { echo "MISSING: cmake on PATH" >&2; PREFLIGHT_OK=0; }
command -v cc    >/dev/null 2>&1 || { echo "MISSING: cc (host compiler)" >&2; PREFLIGHT_OK=0; }
if [ ! -e "${ROOT}/libultraship/CMakeLists.txt" ] || [ ! -e "${ROOT}/decomp/Makefile" ]; then
    echo "Submodules not initialized. Run: git submodule update --init --recursive" >&2
    PREFLIGHT_OK=0
fi
if [ "$PREFLIGHT_OK" -ne 1 ]; then
    record "preflight" FAIL "toolchain/submodules missing (see above)"
    summary; exit 1
fi
echo "devkitPro: ${DEVKITPRO}  cmake: $(cmake --version | head -1)"
record "preflight" PASS "devkitPro + host compiler + submodules present"

# ------------------------------------------------------------------ patches --
# Apply order follows port/3ds/patches/README.md. Every *.patch on disk must be
# listed here — an unknown patch means this script is stale, and that FAILS.
PATCH_LIST=(
    "libultraship:lus-newlib-portability.patch"
    "libultraship:lus-resource-cache-cap.patch"
    "libultraship:lus-device-path-archives.patch"
    "libultraship:lus-3ds-settimg-low-address.patch"
    "libultraship:lus-3ds-hud-tall-atlas-extent.patch"
    "libultraship:lus-texcache-content-hash-span.patch"
    "libultraship:lus-3ds-hud-speedtex-hash-span.patch"
    "libultraship:lus-3ds-fog-exact-params.patch"
    "libultraship:lus-3ds-texcache-resource-stable-key.patch"
    "libultraship:lus-3ds-settimg-resolution-memo.patch"
    "libultraship:lus-s7-raw-instance-dispatch.patch"
    "libultraship:lus-s7-geo-diag-gate.patch"
    "libultraship:lus-s7-tri-state-memo.patch"
    "libultraship:lus-3ds-primenv-flush.patch"
    "libultraship:lus-prof-sections.patch"
    "libultraship:lus-flat-dispatch.patch"
    "libultraship:lus-vtx-mtx-hoist.patch"
    "libultraship:lus-profop.patch"
    "libultraship:lus-tmem-diag-race-latch.patch"
    "libultraship:lus-tmem-span-store.patch"
    "libultraship:lus-tmem-same-content-skip.patch"
    "libultraship:lus-texrect-run-memo.patch"
    "libultraship:lus-texrect-viewport-hoist.patch"
    "libultraship:lus-3ds-livery-ident.patch"
    "libultraship:lus-traffic-pipesync-noop.patch"
    "libultraship:lus-traffic-tri-memo-whitelist.patch"
    "libultraship:lus-traffic-vtx-clipmask.patch"
    "libultraship:lus-3ds-shade-alpha-ccmux.patch"
    "libultraship:lus-currentdir-reset-churn.patch"
    "libultraship:lus-cc-key-uninit-shader-id.patch"
    "libultraship:lus-crowd2-tilestate-value-gate.patch"
    "libultraship:lus-3ds-triloop-packed-vbo.patch"
    "libultraship:lus-tri2-phase-census.patch"
    "libultraship:lus-trifast-tri-memo-pack.patch"
    "libultraship:lus-tmem2-tmemfast.patch"
    "libultraship:lus-trect-census.patch"
    "libultraship:lus-trectbatch-atlas.patch"
    "libultraship:lus-renderthread-texcache-owner.patch"
    "decomp:decomp-ilp32.patch"
    "decomp:decomp-port-segment-bzero.patch"
    "decomp:decomp-3ds-dma-low-address.patch"
    "decomp:decomp-port-audio-specwait-yield.patch"
    "decomp:decomp-race-cull-diagnostics.patch"
    "decomp:decomp-3ds-sky-overscan-deadend-note.patch"
    "decomp:decomp-3ds-machineselect-gradient-coalesce.patch"
    "decomp:decomp-3ds-rom-audio-port.patch"
    "decomp:decomp-port-course-select-state-reset.patch"
    "decomp:decomp-port-rival-detail.patch"
)

banner "submodule patches"
PATCH_DIR="${ROOT}/port/3ds/patches"
PATCHES_OK=1
APPLIED_NOW=0
ALREADY=0

known_patch() {
    local f="$1" entry
    for entry in "${PATCH_LIST[@]}"; do
        if [ "${entry#*:}" = "$f" ]; then return 0; fi
    done
    return 1
}
for f in "${PATCH_DIR}"/*.patch; do
    base="$(basename "$f")"
    if ! known_patch "$base"; then
        echo "FAIL: ${base} exists in port/3ds/patches/ but is not in this script's PATCH_LIST." >&2
        echo "      Update tools/ci-3ds.sh (and port/3ds/patches/README.md) together." >&2
        PATCHES_OK=0
    fi
done

# Confirm every listed patch is on disk before we reason about apply state.
for entry in "${PATCH_LIST[@]}"; do
    patch="${entry#*:}"
    if [ ! -f "${PATCH_DIR}/${patch}" ]; then
        echo "FAIL: listed patch missing on disk: ${patch}" >&2
        PATCHES_OK=0
    fi
done

# The gate is verified PER SUBMODULE against the FULL ordered stack, not
# per-patch. Two of the libultraship patches (lus-texcache-content-hash-span
# and lus-3ds-fog-exact-params) extend the SAME `#ifdef __3DS__` hook block in
# src/fast/interpreter.cpp (see port/3ds/patches/README.md): the fog patch's
# hunks are addressed AT the line numbers the texcache patch produces. So the
# two are order-dependent, and a per-patch check on either one against the fully
# patched tree matches neither forward NOR reverse — the old gate false-aborted
# on a correctly patched checkout.
#
# Why not a combined `git apply --check`? `--check` with several patch files
# validates every file against the SAME starting tree, so an order-dependent
# stack fails the combined check even when it applies fine. A real `git apply`
# (no --check), by contrast, applies the listed patches SEQUENTIALLY and is
# atomic — on any failure it touches nothing. We exploit exactly that:
#   * combined forward apply succeeds       -> clean tree, now patched. Done.
#   * else combined reverse apply succeeds   -> tree WAS fully applied; we then
#     re-apply forward to restore it (a proven forward<->reverse roundtrip).
#   * else                                   -> real drift, FAIL.
# Sequential real applies are immune to the inter-patch context overlap that
# broke the per-patch check.
#
# SKIP_PATCH_GATE=1 downgrades a drift FAIL to a warning and force-applies any
# individual patch that still applies (escape hatch for a knowingly-dirty
# working tree; leaves the build to catch anything genuinely broken).
SKIP_PATCH_GATE="${SKIP_PATCH_GATE:-0}"

# Distinct submodules in first-seen order (bash 3.2: no associative arrays).
SUBMODULES=()
for entry in "${PATCH_LIST[@]}"; do
    sub="${entry%%:*}"
    seen=0
    for s in "${SUBMODULES[@]:-}"; do
        if [ "$s" = "$sub" ]; then seen=1; break; fi
    done
    if [ "$seen" -eq 0 ]; then SUBMODULES+=("$sub"); fi
done

# Ordered absolute patch paths for one submodule, on stdout (one per line).
patches_for_sub() { # sub
    local want="$1" entry
    for entry in "${PATCH_LIST[@]}"; do
        if [ "${entry%%:*}" = "$want" ]; then
            printf '%s\n' "${PATCH_DIR}/${entry#*:}"
        fi
    done
}

if [ "$PATCHES_OK" -eq 1 ]; then
  for sub in "${SUBMODULES[@]}"; do
    # Forward order (apply order) and reverse order (undo order) arrays.
    fwd=()
    while IFS= read -r line; do fwd+=("$line"); done < <(patches_for_sub "$sub")
    rev=()
    idx=$(( ${#fwd[@]} - 1 ))
    while [ "$idx" -ge 0 ]; do rev+=("${fwd[$idx]}"); idx=$((idx - 1)); done

    # 1. Clean tree? A real sequential forward apply lands the whole stack.
    #    Atomic: if it can't fully apply it changes nothing, and we fall through.
    if git -C "${ROOT}/${sub}" apply "${fwd[@]}" >/dev/null 2>&1; then
        echo "applied:         ${sub} (${#fwd[@]} patches, clean tree)"
        APPLIED_NOW=$((APPLIED_NOW + ${#fwd[@]}))
    # 2. Already fully applied? A real sequential reverse apply reverts it; if
    #    that succeeds we immediately re-apply forward to restore the tree,
    #    proving a clean forward<->reverse roundtrip without leaving it reverted.
    elif git -C "${ROOT}/${sub}" apply --reverse "${rev[@]}" >/dev/null 2>&1; then
        git -C "${ROOT}/${sub}" apply "${fwd[@]}"
        echo "already applied: ${sub} (${#fwd[@]} patches, full-stack roundtrip)"
        ALREADY=$((ALREADY + ${#fwd[@]}))
    elif [ "$SKIP_PATCH_GATE" = "1" ]; then
        echo "WARN: ${sub} stack neither applies nor reverses as a whole." >&2
        echo "      SKIP_PATCH_GATE=1 set — force-applying still-applicable patches." >&2
        for p in "${fwd[@]}"; do
            if git -C "${ROOT}/${sub}" apply --check "$p" >/dev/null 2>&1; then
                git -C "${ROOT}/${sub}" apply "$p"
                echo "applied (forced): ${sub} <- $(basename "$p")"
                APPLIED_NOW=$((APPLIED_NOW + 1))
            else
                echo "skipped (already/blocked): ${sub} <- $(basename "$p")" >&2
            fi
        done
    else
        echo "FAIL: ${sub} patch stack neither applies nor reverses cleanly." >&2
        echo "      The submodule working tree has drifted (partial application or" >&2
        echo "      conflicting local edits). Inspect with:" >&2
        echo "        git -C ${sub} diff" >&2
        echo "        git -C ${sub} apply --check ${PATCH_DIR#${ROOT}/}/<patch>" >&2
        echo "      Or re-run with SKIP_PATCH_GATE=1 to force past a known-dirty tree." >&2
        PATCHES_OK=0
    fi
  done
fi

if [ "$PATCHES_OK" -ne 1 ]; then
    record "submodule patches" FAIL "partial application / unknown patch (see above)"
    summary; exit 1
fi
record "submodule patches" PASS "${ALREADY} already applied, ${APPLIED_NOW} newly applied"

# --------------------------------------------------------------- host tests --
# These are the desktop console tests that matter for the 3DS port (pointer
# packing + DL conversion) plus stream D's fs/zipshim smoke test. They are
# standalone by design, so we compile them directly — a full desktop configure
# (libultraship + SDL) is NOT required on macOS.
banner "host unit tests"
mkdir -p "${HOST_TEST_DIR}"

run_host_test() { # name log-file -> pass/fail via $?
    local name="$1" bin="$2" cwd="$3" log="${HOST_TEST_DIR}/$1.log"
    if (cd "$cwd" && "$bin" > "$log" 2>&1); then
        echo "PASS: ${name}  ($(tail -1 "$log"))"
        record "test: ${name}" PASS "$(tail -1 "$log")"
    else
        echo "FAIL: ${name} — last lines:" >&2
        tail -5 "$log" >&2
        record "test: ${name}" FAIL "see ${log#${ROOT}/}"
    fi
}

# gdx_gfx_pack_tests — real decomp gbi.h under the game's defines; proves a
# 64-bit host pointer survives in Gfx.w1 (mirrors port/CMakeLists.txt:404).
if cc -O1 "${ROOT}/port/tests/gfx_pack_tests.c" \
      -I"${ROOT}/decomp/include" \
      -DPORT=1 -DGDIFFUSER_PORT=1 -DF3DEX_GBI_2=1 -D_LANGUAGE_C=1 -DVERSION_US=1 \
      -o "${HOST_TEST_DIR}/gfx_pack_tests"; then
    run_host_test "gdx_gfx_pack_tests" "${HOST_TEST_DIR}/gfx_pack_tests" "${HOST_TEST_DIR}"
else
    record "test: gdx_gfx_pack_tests" FAIL "compile failed"
fi

# gdx_gfx_convert_tests — n64_gfx_convert.cpp compiled unmodified
# (mirrors port/CMakeLists.txt:420).
if c++ -std=c++20 -O1 \
      "${ROOT}/port/tests/gfx_convert_tests.cpp" \
      "${ROOT}/port/n64_gfx_convert.cpp" \
      -I"${ROOT}/port" \
      -o "${HOST_TEST_DIR}/gfx_convert_tests"; then
    run_host_test "gdx_gfx_convert_tests" "${HOST_TEST_DIR}/gfx_convert_tests" "${HOST_TEST_DIR}"
else
    record "test: gdx_gfx_convert_tests" FAIL "compile failed"
fi

# gdx3ds_fs_host_smoke — stream D's SD/O2R/zipshim smoke test; needs a scratch
# CWD because "sdmc:" is an ordinary directory to the host
# (mirrors port/3ds/assets/CMakeLists.txt:45).
if cc -O1 \
      "${ROOT}/port/3ds/assets/test/gdx3ds_fs_host_smoke.c" \
      "${ROOT}/port/3ds/assets/gdx3ds_fs_sd.c" \
      "${ROOT}/port/3ds/assets/zipshim/gdx3ds_zipshim.c" \
      "${ROOT}/port/3ds/assets/third_party/miniz/miniz.c" \
      -I"${ROOT}/port/3ds/include" \
      -I"${ROOT}/port/3ds/lus_stubs" \
      -I"${ROOT}/port/3ds/assets/third_party/miniz" \
      -DMINIZ_NO_TIME \
      -o "${HOST_TEST_DIR}/fs_host_smoke"; then
    rm -rf "${HOST_TEST_DIR}/smoke-cwd"
    mkdir -p "${HOST_TEST_DIR}/smoke-cwd"
    run_host_test "gdx3ds_fs_host_smoke" "${HOST_TEST_DIR}/fs_host_smoke" "${HOST_TEST_DIR}/smoke-cwd"
else
    record "test: gdx3ds_fs_host_smoke" FAIL "compile failed"
fi

# ---------------------------------------------------------------- 3DS build --
banner "3DS build (game + harness)"
BUILD_OK=1
mkdir -p "${BUILD_3DS}"
if ! cmake -S "${ROOT}" -B "${BUILD_3DS}" \
        -DCMAKE_TOOLCHAIN_FILE="${DEVKITPRO}/cmake/3DS.cmake" \
        -DGDX_PLATFORM_3DS=ON \
        -DCMAKE_BUILD_TYPE=Release > "${BUILD_3DS}/ci-configure.log" 2>&1; then
    tail -20 "${BUILD_3DS}/ci-configure.log" >&2
    record "3ds configure" FAIL "see build-3ds/ci-configure.log"
    BUILD_OK=0
else
    record "3ds configure" PASS "toolchain=${DEVKITPRO}/cmake/3DS.cmake"
    if cmake --build "${BUILD_3DS}" -j"${JOBS}" > "${BUILD_3DS}/ci-build.log" 2>&1; then
        echo "build OK (-j${JOBS})"
    else
        echo "FAIL: 3DS build — last lines:" >&2
        tail -30 "${BUILD_3DS}/ci-build.log" >&2
        BUILD_OK=0
    fi
    GAME_3DSX="${BUILD_3DS}/port/3ds/G-Diffuser-3DS.3dsx"
    HARNESS_3DSX="${BUILD_3DS}/port/3ds/harness/gdx3ds-dl-tests.3dsx"
    if [ "$BUILD_OK" -eq 1 ] && [ -f "$GAME_3DSX" ]; then
        record "build: G-Diffuser-3DS.3dsx" PASS "$(du -h "$GAME_3DSX" | cut -f1 | tr -d ' ')"
    else
        record "build: G-Diffuser-3DS.3dsx" FAIL "missing artifact or build error (build-3ds/ci-build.log)"
        BUILD_OK=0
    fi
    if [ "$BUILD_OK" -eq 1 ] && [ -f "$HARNESS_3DSX" ]; then
        record "build: gdx3ds-dl-tests.3dsx" PASS "$(du -h "$HARNESS_3DSX" | cut -f1 | tr -d ' ')"
    else
        record "build: gdx3ds-dl-tests.3dsx" FAIL "missing artifact or build error (build-3ds/ci-build.log)"
    fi
fi

# ---------------------------------------------------------------- emu smoke --
lock_mtime_age() { # seconds since the lock dir's mtime (BSD stat)
    local m; m="$(stat -f %m "$AZAHAR_LOCK" 2>/dev/null || echo 0)"
    echo $(( $(date +%s) - m ))
}

acquire_azahar_lock() {
    local waited=0
    while ! mkdir "$AZAHAR_LOCK" 2>/dev/null; do
        if [ "$(lock_mtime_age)" -gt "$AZAHAR_LOCK_STALE_SECONDS" ]; then
            echo "emulator lock stale ($(lock_mtime_age)s old) — stealing"
            rmdir "$AZAHAR_LOCK" 2>/dev/null || true
            continue
        fi
        if [ "$waited" -ge "$AZAHAR_LOCK_WAIT_MAX_SECONDS" ]; then
            return 1
        fi
        echo "emulator lock held (${AZAHAR_LOCK}) — waiting ${AZAHAR_LOCK_POLL_SECONDS}s (${waited}/${AZAHAR_LOCK_WAIT_MAX_SECONDS}s)"
        sleep "$AZAHAR_LOCK_POLL_SECONDS"
        waited=$((waited + AZAHAR_LOCK_POLL_SECONDS))
    done
    HOLDING_LOCK=1
    return 0
}

if [ "$RUN_EMU" -eq 1 ]; then
    banner "Azahar boot smoke (${EMU_BOOT_SECONDS}s)"
    EMU_OK=1
    if [ "$BUILD_OK" -ne 1 ]; then
        record "emu boot smoke" FAIL "skipped: 3DS build failed"
        EMU_OK=0
    elif [ ! -x "$AZAHAR_BIN" ]; then
        record "emu boot smoke" FAIL "Azahar not found at ${AZAHAR_BIN} (install from GitHub releases)"
        EMU_OK=0
    elif [ ! -f "${AZAHAR_SDMC}/3ds/gdiffuser/fzerox.o2r" ]; then
        record "emu boot smoke" FAIL "no fzerox.o2r on the virtual SD — run tools/prebake and stage to ${AZAHAR_SDMC}/3ds/gdiffuser/"
        EMU_OK=0
    elif ! grep -q '^log_filter=\*:Debug' "$AZAHAR_CFG" 2>/dev/null; then
        record "emu boot smoke" FAIL "qt-config.ini log_filter is not *:Debug — heartbeat invisible (docs/3DS-HARDWARE.md, Building from source)"
        EMU_OK=0
    fi

    if [ "$EMU_OK" -eq 1 ]; then
        if ! acquire_azahar_lock; then
            record "emu boot smoke" FAIL "could not acquire ${AZAHAR_LOCK} within ${AZAHAR_LOCK_WAIT_MAX_SECONDS}s"
        else
            # Kill any leftover instances; stale processes share the log file and
            # corrupt the evidence (docs/research/m1-boot-debug.md, step 3).
            pkill -9 -x azahar 2>/dev/null || true
            sleep 2
            if pgrep -x azahar >/dev/null 2>&1; then
                record "emu boot smoke" FAIL "stale azahar process would not die (macOS IOKit wedge? see docs/3DS-HARDWARE.md troubleshooting)"
            else
                "$AZAHAR_BIN" "$GAME_3DSX" >/dev/null 2>&1 &
                AZAHAR_PID=$!
                disown "$AZAHAR_PID" 2>/dev/null || true   # silence bash's "Killed: 9" job notice
                echo "azahar pid ${AZAHAR_PID}; booting for ${EMU_BOOT_SECONDS}s..."
                sleep "$EMU_BOOT_SECONDS"
                # SIGTERM first (graceful exit flushes the log), then -9.
                pkill -x azahar 2>/dev/null || true
                sleep 3
                pkill -9 -x azahar 2>/dev/null || true
                sleep 1

                FRAMES="$(grep -a 'OutputDebugString' "$AZAHAR_LOG" 2>/dev/null \
                          | grep -aEo 'frame [0-9]+' | awk '{print $2}' || true)"
                DISTINCT="$(printf '%s\n' "$FRAMES" | sort -nu | grep -c . || true)"
                MAXFRAME="$(printf '%s\n' "$FRAMES" | sort -n | tail -1)"
                FATALS="$(grep -ac '\[fatal\]' "$AZAHAR_LOG" 2>/dev/null || true)"
                echo "heartbeat: ${DISTINCT} distinct frames, max=${MAXFRAME:-none}; [fatal] lines: ${FATALS:-0}"
                if [ -z "$MAXFRAME" ] || [ "$DISTINCT" -lt 2 ]; then
                    record "emu boot smoke" FAIL "heartbeat not advancing (distinct=${DISTINCT}, max=${MAXFRAME:-none}) — see ${AZAHAR_LOG}"
                elif [ "${FATALS:-0}" -ne 0 ]; then
                    record "emu boot smoke" FAIL "${FATALS} [fatal] line(s) in the Azahar log"
                else
                    record "emu boot smoke" PASS "heartbeat reached frame ${MAXFRAME}, 0 [fatal] lines"
                fi
            fi
            release_lock
        fi
    fi
fi

summary
exit "$FAILED"
