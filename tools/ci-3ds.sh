#!/usr/bin/env bash
# tools/ci-3ds.sh — one-command local CI for the Nintendo 3DS port.
#
# For an O3DS/N3DS x vanilla/Expansion-Kit release build (not this gate),
# use tools/build-3ds.sh instead — it maps a target name to the right
# CMake flags and renames/checksums the output into dist/3ds/.
#
# From a clean checkout (submodules initialized) this script:
#   1. verifies the devkitPro toolchain is present;
#   2. verifies/applies the port/3ds/patches/ decomp patches (idempotent —
#      each submodule's FULL ordered stack is checked as one combined patchset,
#      so already-applied trees are detected via a whole-stack reverse check
#      even when patches share overlapping context; a stack that neither applies
#      nor reverses cleanly fails LOUDLY, or warns under SKIP_PATCH_GATE=1);
#   3. builds the host-side unit tests that run on this machine and runs them
#      (gdx_gfx_pack_tests, gdx_gfx_convert_tests, gdx_track_template_tests,
#       gdx3ds_config_host_tests, gdx3ds_native_cfg_host_tests, gdx3ds_dynlod_host_tests,
#       gdx3ds_frameskip_host_tests, gdx3ds_trkrr_math_host_tests, gdx3ds_replay_math_host_tests,
#       gdx3ds_snapcopy_host_tests (snap-fast: the snapshot copy engine vs memcpy),
#       gdx3ds_perf_preset_host_tests,
#       gdx3ds_bui_host_tests (bottom-menus: the UI kit's screens vs the design's mockups),
#       gdx3ds_hudbake_host_tests (the DASH art baked at boot from the user's fzerox.o2r),
#       gdx3ds_heapcfg_host_tests, gdx3ds_menu_tabbar_host_tests (hw bug n60-3d8 #1: the tab
#         bar's column math, every MAP/RACE combination, no overlap with OFF; plus the EK
#         build's CRS tab), gdx3ds_course_share_host_tests ([ek] course_share: the .gdxcourse
#         codec, its refusals and fuzz, QR frames decoded back with quirc, and the camera
#         scanner fed synthetic camera frames; plain and under ASan + UBSan),
#       gdx_hud_placement_host_tests (nihudtop Phase 3: the per-element top-screen gate, modes
#         0-2 x every element x numPlayers 1-2 against the pre-Phase-3 all-or-nothing rule,
#         plus the CUSTOM mask override and its numPlayers>1 force-off),
#       gdx_mp_lockstep_host_tests (mp-spike: [net] mp's lockstep protocol core),
#       gdx_mp_timesync_host_tests (mp-timesync: frame-advantage pacing, two-clock model),
#       gdx_mp_netab_host_tests (card-3: the per-race A/B of mp_timesync_max_ms / mp_ndm_exclusive),
#       gdx_mp_online_host_tests + mp_rendezvous_tests (mp-online: internet multiplayer),
#       gdx_mp_drop_host_tests + gdx_mp_beacon_host_tests (mp-uds: drop-out, NEARBY beacon fuzz),
#       gdx_net_enabled_default_host_tests (n60lead hwlink: [net] enabled's launch-method
#         default -- an explicit ini key always wins, the 3dslink-netloader-vs-not vote only
#         applies when it's absent; the emulator lanes cannot simulate a netloader launch),
#       gdx3ds_launch_host_tests, gdx3ds_audiocrk_host_tests,
#       gdx_save_replace_tests (n60save: crash-safe save replace on a no-overwrite-rename FS),
#       gdx_disk_savefile_tests (heapfix: the 64DD sidecar flush allocates nothing, never corrupts),
#       gdx_save_ek_layout_tests (ekship: the EK title's own save and its proven-EK one-time seed),
#       gdx3ds_c3d_scan_host_tests (c5submit: the vendored citro3d diet's dirty scan == stock's),
#       gdx_disk_stream_host_tests (against a synthetic 64DD image -- no dump required),
#       gdx_o2r_stream_host_tests (C4d: the audio_table byte source, against a synthetic zip),
#       gdx3ds_ndsphash_host_tests ([debug] ndsphash: the content-addressed render digest --
#         a whole-session audio gate; a frame hash cannot see a dropped sample),
#       gdx_dsp_host_tests (A17: the DSP KERNELS -- VADPCM decode, resampler, FIR, envmixer,
#         reverb -- driven through gdx_audio_hle_run. ndsphash proves the kernels are
#         CONSISTENT run to run; this is the only thing that says they are CORRECT; run twice,
#         the second time on the C4k [debug] audio_simd fast kernels),
#       gdx_hle_fast_parity_tests (C4k: audio_simd fast kernels == reference, byte for byte),
#       gdx3ds_fs_host_smoke,
#      gdx_machines_harness —
#      compiled directly with the host compiler; no desktop configure needed);
#   4. configures + builds the 3DS target (G-Diffuser-3DS.3dsx) and the DL
#      replay harness (gdx3ds-dl-tests.3dsx) with the devkitARM toolchain,
#      preferring Ninja + ccache when both are on PATH and timing configure,
#      full and incremental builds;
#   5. with --emu, boots the game .3dsx in Azahar for ${EMU_BOOT_SECONDS}s and
#      asserts the frame heartbeat advances with zero [fatal] lines
#      (honours the shared /tmp/azahar.lock emulator-lock protocol);
#   6. prints a PASS/FAIL summary table and exits nonzero on any failure.
#
# Usage:  tools/ci-3ds.sh [--emu] [--jobs N] [--no-ninja] [--no-ccache]
#                         [--no-incremental]
#
# Notes:
#  - The LUS carve is VENDORED at port/3ds/vendor/lus/ with all 38 lus-*.patch
#    already folded in (port/3ds/vendor/lus/PROVENANCE.md), so the 3DS lane
#    needs neither the libultraship submodule nor its patch stack. Only the 10
#    decomp-*.patch are applied here.
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
USE_NINJA=1
USE_CCACHE=1
RUN_INCREMENTAL=1
# The file an incremental-build timing touches: the biggest carve TU.
INCR_TOUCH="port/3ds/vendor/lus/src/fast/interpreter.cpp"

EMU_BOOT_SECONDS=45
AZAHAR_APP="/Applications/Azahar.app"
AZAHAR_BIN="${AZAHAR_APP}/Contents/MacOS/azahar"   # existence probe only — see the launch below
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
        --no-ninja)       USE_NINJA=0 ;;
        --no-ccache)      USE_CCACHE=0 ;;
        --no-incremental) RUN_INCREMENTAL=0 ;;
        -h|--help)
            sed -n '2p;7,36p' "$0"; exit 0 ;;
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
# The 3DS lane needs decomp only — the LUS carve is vendored (see the note above),
# so libultraship/ may be absent or empty here and everything must still work.
if [ ! -e "${ROOT}/decomp/Makefile" ]; then
    echo "decomp submodule not initialized. Run: git submodule update --init decomp torch" >&2
    PREFLIGHT_OK=0
fi
if [ ! -e "${ROOT}/port/3ds/vendor/lus/cmake/cvars.cmake" ]; then
    echo "MISSING: port/3ds/vendor/lus/ — the vendored LUS carve is part of this repo" >&2
    PREFLIGHT_OK=0
fi
# fzerox-expansion-kit is NOT needed for this gate (it only builds the shipped, non-EK
# .3dsx) -- but it's a hard configure-time requirement for -DGDX_3DS_EXPANSION_KIT=ON
# (tools/gen_ek_assets.py reads its yaml/patch.py), so say so rather than leaving the
# first EK configure to a cryptic FileNotFoundError.
if [ ! -e "${ROOT}/fzerox-expansion-kit/tools/patch.py" ]; then
    echo "NOTE: fzerox-expansion-kit submodule not initialized -- fine for this gate, but" >&2
    echo "      required to configure an Expansion Kit target. Run:" >&2
    echo "      git submodule update --init fzerox-expansion-kit" >&2
fi
if [ "$PREFLIGHT_OK" -ne 1 ]; then
    record "preflight" FAIL "toolchain/submodules missing (see above)"
    summary; exit 1
fi
echo "devkitPro: ${DEVKITPRO}  cmake: $(cmake --version | head -1)"
record "preflight" PASS "devkitPro + host compiler + decomp + vendored LUS carve present"

# ----------------------------------------------------------- vendor manifest --
# port/3ds/vendor/lus/MANIFEST.sha256 is the vendored carve's content receipt: with the
# libultraship submodule gone from the 3DS build, it is the ONLY thing that says which
# bytes the carve is supposed to contain. It went stale twice (interpreter.h drifted, and
# PROVENANCE.md was never listed) because nothing checked it, so check it here. Editing a
# vendored file is fine and expected — regenerate with `tools/vendor-lus.sh --manifest` in
# the same commit. Needs no submodule and no network: it is a plain checksum -c.
#
# macOS ships `shasum` (perl); a stripped Linux CI container may ship only GNU
# `sha256sum` and no perl at all. Both read the same `<hash>  <path>` lines and both
# print `<path>: OK`, so either verifies the manifest — pick whichever is on PATH
# rather than making perl a build dependency.
banner "vendored LUS manifest"
if command -v shasum >/dev/null 2>&1; then
    sha256_check() { shasum -a 256 -c "$1"; }
elif command -v sha256sum >/dev/null 2>&1; then
    sha256_check() { sha256sum -c "$1"; }
else
    sha256_check() { echo "neither shasum nor sha256sum is on PATH" >&2; return 1; }
fi
VENDOR_LUS="${ROOT}/port/3ds/vendor/lus"
MANIFEST_LOG="${HOST_TEST_DIR:-${ROOT}/build-3ds/host-tests}/vendor-manifest.log"
mkdir -p "$(dirname "${MANIFEST_LOG}")"
VENDOR_UNLISTED="$( (cd "${VENDOR_LUS}" && find . -type f ! -path './patches/*' ! -name MANIFEST.sha256 \
    | sed 's|^\./||' | LC_ALL=C sort) | comm -23 - <(awk '{print $2}' "${VENDOR_LUS}/MANIFEST.sha256" | LC_ALL=C sort) )"
if (cd "${VENDOR_LUS}" && sha256_check MANIFEST.sha256) > "${MANIFEST_LOG}" 2>&1 && [ -z "${VENDOR_UNLISTED}" ]; then
    echo "PASS: MANIFEST.sha256 matches the vendored tree ($(grep -c ': OK$' "${MANIFEST_LOG}") files)"
    record "vendor manifest" PASS "$(grep -c ': OK$' "${MANIFEST_LOG}") files match"
else
    echo "FAIL: port/3ds/vendor/lus/MANIFEST.sha256 is stale — run tools/vendor-lus.sh --manifest" >&2
    grep -v ': OK$' "${MANIFEST_LOG}" | head -10 >&2
    [ -n "${VENDOR_UNLISTED}" ] && echo "unlisted files: ${VENDOR_UNLISTED}" >&2
    record "vendor manifest" FAIL "stale — run tools/vendor-lus.sh --manifest"
fi

# ------------------------------------------------------------------ patches --
# Apply order follows port/3ds/patches/README.md. Every *.patch on disk must be
# listed here — an unknown patch means this script is stale, and that FAILS.
#
# decomp only. The 38 lus-*.patch are folded into the vendored carve at
# port/3ds/vendor/lus/ and live on there as frozen provenance under
# port/3ds/vendor/lus/patches/ — they are NOT applied and NOT listed here (they
# also sit outside PATCH_DIR, so the unknown-patch guard below is not tripped
# by them).
PATCH_LIST=(
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
    "decomp:decomp-native-track-shim.patch"
    "decomp:decomp-port-determinism-pins.patch"
    "decomp:decomp-native-machines-rebuilt.patch"
    "decomp:decomp-port-title-demo-audio-pin.patch"
    "decomp:decomp-native-fx-shim.patch"
    "decomp:decomp-3ds-mainprof-checkpoints.patch"
    "decomp:decomp-3ds-disk-audio-ilp32.patch"
    "decomp:decomp-3ds-disk-audio-heap-ilp32.patch"
    "decomp:decomp-3ds-disk-audio-deadlock.patch"
    "decomp:decomp-3ds-rdram-size-knob.patch"
    "decomp:decomp-3ds-disk-residency-seam.patch"
    "decomp:decomp-3ds-disk-audio-async-guards.patch"
    "decomp:decomp-3ds-ek-diskcourse-decline.patch"
    "decomp:decomp-3ds-disk-audio-load-census.patch"
    "decomp:decomp-port-o3ds-scene-lod.patch"
    "decomp:decomp-3ds-c5cat-markers.patch"
    "decomp:decomp-native-hud-capture.patch"
    "decomp:decomp-native-track-resident-bake.patch"
    "decomp:decomp-native-sky-capture.patch"
    "decomp:decomp-3ds-c5tick-dlb.patch"
    "decomp:decomp-3ds-c5sub-markers.patch"
    "decomp:decomp-native-dress-shim.patch"
    "decomp:decomp-3ds-nrr-mach.patch"
    "decomp:decomp-native-nrr-trk.patch"
    "decomp:decomp-3ds-ships-census.patch"
    "decomp:decomp-3ds-autodrive.patch"
    "decomp:decomp-3ds-prewarm.patch"
    "decomp:decomp-native-finish-direct.patch"
    "decomp:decomp-native-finish-mesh.patch"
    "decomp:decomp-3ds-hud-placement.patch"
    "decomp:decomp-native-hud-lap-announce.patch"
    "decomp:decomp-native-force-lap2.patch"
    "decomp:decomp-native-menu-capture.patch"
    "decomp:decomp-native-force-boot-mode.patch"
    "decomp:decomp-native-menu-trans-capture.patch"
    "decomp:decomp-native-gp-end-setup.patch"
    "decomp:decomp-native-gp-end-mach.patch"
    "decomp:decomp-3ds-nimach.patch"
    "decomp:decomp-port-title-demo-tick.patch"
    "decomp:decomp-native-records-scene.patch"
    "decomp:decomp-native-course-preview.patch"
    "decomp:decomp-native-hud-elem-gate.patch"
    "decomp:decomp-3ds-ek-audio-disk-async.patch"
    "decomp:decomp-3ds-ek-audio-sync-bulk.patch"
    "decomp:decomp-3ds-ek-testdrive-scope.patch"
    "decomp:decomp-3ds-ek-editmesh.patch"
    "decomp:decomp-3ds-ek-nimach.patch"
    "decomp:decomp-native-menu-xition.patch"
    "decomp:decomp-3ds-mp-online.patch"
    "decomp:decomp-3ds-mp-sim.patch"
    "decomp:decomp-3ds-mp-fullview.patch"
    "decomp:decomp-3ds-widescreen-cull.patch"
    "decomp:decomp-3ds-ek-nodisk-audio.patch"
    "decomp:decomp-3ds-ek-vs-onepad.patch"
    "decomp:decomp-3ds-settings-backdrop-full.patch"
    "decomp:decomp-3ds-mp-vis.patch"
    "decomp:decomp-3ds-mp-ekhold.patch"
    "decomp:decomp-3ds-mp-racers.patch"
    "decomp:decomp-3ds-mp-dropcpu.patch"
    "decomp:decomp-3ds-mp-rambot.patch"
    "decomp:decomp-3ds-mp-grid.patch"
    "decomp:decomp-3ds-bui-title-demo-hold.patch"
    "decomp:decomp-3ds-mp-stockpick.patch"
    "decomp:decomp-3ds-font-nullread.patch"
    "decomp:decomp-3ds-mp-vs-overlay-bounds.patch"
    "decomp:decomp-3ds-o3ds60-mp-hud.patch"
    "decomp:decomp-3ds-mp-polish.patch"
    "decomp:decomp-3ds-mp-vs-menu.patch"
    "decomp:decomp-3ds-unlock-3ds-code.patch"
    "decomp:decomp-3ds-mp-fullbleed.patch"
    "decomp:decomp-3ds-snapshot.patch"
    "decomp:decomp-3ds-stereo-sky-margin.patch"
)

banner "submodule patches (decomp)"
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

# The gate itself lives in tools/patch-gate.sh, so it can be unit-tested against a
# throwaway git repo (tools/dev/tests/test_gdxdev.py::PatchGateTest) instead of only
# ever running against this one checkout. PATCH_LIST above stays the single source of
# truth — the gate script is handed the list and knows nothing else.
#
# It keeps the full-stack forward<->reverse roundtrip as the correctness check, and
# adds what NEXT.md B50 asks for: a tree with the first M of N applied (the normal
# state right after a merge that ADDS patches) is no longer reported as "partial
# application / conflicting local edits" pointing at `git -C decomp diff`. The gate
# names the patches that were never applied, applies just those, and re-proves the
# whole stack with the roundtrip.
#
# SKIP_PATCH_GATE=1 downgrades a drift FAIL to a warning and force-applies any
# individual patch that still applies (escape hatch for a knowingly-dirty
# working tree; leaves the build to catch anything genuinely broken).
if [ "$PATCHES_OK" -eq 1 ]; then
    PG_OUT="$(mktemp "${TMPDIR:-/tmp}/gdx-patch-gate.XXXXXX")"
    if bash "${ROOT}/tools/patch-gate.sh" "${ROOT}" "${PATCH_DIR}" "${PATCH_LIST[@]}" \
            | tee "${PG_OUT}"; then
        ALREADY="$(sed -n 's/^PATCHGATE: already=\([0-9]*\) applied=\([0-9]*\)$/\1/p' "${PG_OUT}" | tail -1)"
        APPLIED_NOW="$(sed -n 's/^PATCHGATE: already=\([0-9]*\) applied=\([0-9]*\)$/\2/p' "${PG_OUT}" | tail -1)"
    else
        PATCHES_OK=0
    fi
    rm -f "${PG_OUT}"
fi

if [ "$PATCHES_OK" -ne 1 ]; then
    record "submodule patches" FAIL "partial application / unknown patch (see above)"
    summary; exit 1
fi
record "submodule patches" PASS "${ALREADY} already applied, ${APPLIED_NOW} newly applied"

# ------------------------------------------------- bridge memo ownership --
# [factsrace] The bridge's per-list facts memo (N64DisplayListAdapter::ListFacts) must stay a
# PER-ADAPTER member. As a process-wide `static` table it is reachable from BOTH threads under
# `[native] interp=1` (the owned walker resolves lists on the render thread through
# gdx_gfx_job_resolve_list) while `[debug] bridgemain=1` has main constructing the NEXT task's
# adapter at the same time — the constructor's per-task wipe then nulls a slot the render thread
# holds a reference to. That is the 2026-09-23 New 3DS data abort (far=0x00000003 inside
# FactsScan); see docs/research/native/factsrace-progress.md. This check is a regression fence,
# not a test: the race cannot be reproduced under Azahar, which time-slices both cores onto one
# host thread.
banner "bridge memo ownership"
BRIDGE_SRC="${ROOT}/port/n64_gfx_bridge.cpp"
if grep -nE '(static|thread_local|__thread)[[:space:]]+ListFacts' "${BRIDGE_SRC}" >&2; then
    record "bridge memo ownership" FAIL "ListFacts memo is static/thread-local again — it must be a N64DisplayListAdapter member"
elif grep -q 'mutable ListFacts mFacts\[kListFactsSlots\];' "${BRIDGE_SRC}"; then
    record "bridge memo ownership" PASS "ListFacts memo is a per-adapter member (8 KiB/adapter on ARM32, one task = one walker)"
else
    record "bridge memo ownership" FAIL "per-adapter ListFacts member (mFacts) not found in port/n64_gfx_bridge.cpp"
fi

# ------------------------------------------ bridge cross-thread invariants --
# [resolverace] Three fences over the process-wide bridge state that `[native] interp=1` put on
# two threads (docs/research/native/resolverace-progress.md). None of these can be caught by a
# test: Azahar time-slices both emulated cores onto one host thread, so the races are not
# reproducible in either emulator lane. They are regression fences on the SHAPE of the code.
#
#  1. gGdxResolveTablesVersion / gConvertEpoch / gNativeRgba16Generation are std::atomic and
#     every mutation goes through the bump helpers. Measured: the [resolverace] census reports
#     ver mask=3 under `[native] interp=1` -- both threads read-modify-write that word -- and it
#     is the sole freshness key of four process-wide memos, so a lost `++` serves a stale host
#     pointer.
#     [stubmemo] gNativeRgba16Generation is the sibling resolverace left uncovered: a game-thread
#     writer (gdx_set_native_rgba16_texture_range) and a walking-thread writer (GdxGfxJobRelease's
#     nativeClears retirement, on the render thread under `[native] interp=1`). It is the
#     freshness key of MakePersistentRawTextureCopy's per-frame compare skip AND of loadblock's
#     SETTIMG classification memo, so a lost bump is a persistent texture copy holding the
#     previous transition's screen -- silent, no fault.
#  2. EnsureAssetSegmentImageImpl does not push_back into gHostRanges / gRawN64Ranges /
#     gLoadedAssetSegments from the render thread; a walk-time first load defers to
#     gPendingSegmentPublishes and the game thread drains it. GdxRtFence() above those appends
#     is a JOIN, not a barrier, and returns immediately on the render thread.
#  3. The three tables stay reserved, because the deferral's correctness argument and the walk
#     scans both assume the storage is pinned.
banner "bridge cross-thread invariants"
BRIDGE_SRC="${ROOT}/port/n64_gfx_bridge.cpp"
rr_fail=""
if grep -qE '^static std::atomic<uint32_t> gGdxResolveTablesVersion' "${BRIDGE_SRC}" &&
   grep -qE '^std::atomic<uint32_t> gConvertEpoch' "${BRIDGE_SRC}" &&
   grep -qE '^std::atomic<uint64_t> gNativeRgba16Generation' "${BRIDGE_SRC}"; then
    :
else
    rr_fail="${rr_fail} tables-version/convert-epoch/native-rgba16-generation are not std::atomic;"
fi
# Statement form only (the identifier immediately followed by `;`), so prose that names the
# old `++gConvertEpoch` in a comment does not trip the fence.
rr_ctr='gGdxResolveTablesVersion|gConvertEpoch|gNativeRgba16Generation'
if grep -qE "(\+\+[[:space:]]*(${rr_ctr})[[:space:]]*;)|((${rr_ctr})[[:space:]]*\+\+[[:space:]]*;)" "${BRIDGE_SRC}"; then
    rr_fail="${rr_fail} a plain ++ on a cross-thread counter is back;"
fi
# ... and that each one still has its bump helper, so a new mutation site has somewhere correct
# to go instead of reaching for the raw name.
for rr_bump in GdxBumpResolveTablesVersion GdxBumpConvertEpoch GdxBumpNativeRgba16Generation; do
    if ! grep -qE "^static inline void ${rr_bump}\(\)" "${BRIDGE_SRC}"; then
        rr_fail="${rr_fail} the ${rr_bump} helper is gone;"
    fi
done
if grep -q 'if (GdxOnRenderThread()) {' "${BRIDGE_SRC}" &&
   grep -q 'gPendingSegmentPublishes.emplace_back(std::move(loaded));' "${BRIDGE_SRC}" &&
   grep -q 'static void GdxDrainPendingSegmentPublishes()' "${BRIDGE_SRC}"; then
    :
else
    rr_fail="${rr_fail} the render-thread segment-publish deferral is gone;"
fi
#  4. The six process-wide [brfast] memos keep ONE owner thread: every entry is behind
#     GdxMemoOwnedHere(), which latches the first thread to reach a memo and sends any other
#     thread down the uncached path the memo memoises. Measured: with the latch, the memo masks
#     under `[native] interp=1 interp_verify=1` drop from 3 (both threads) to 1.
# [memotear] ...and the latch is checked PER SITE, by the enum the census names it with, rather
#     than by a total count. A count could be satisfied by six calls at five sites, which is
#     exactly how a latch gets dropped one call site at a time. GdxMemoOwnedFor() is the
#     site-aware wrapper; it also notes the FOREIGN reach, which is the instrument that says
#     whether any thread but the owner ever asked (memotear-progress.md S1d).
for rr_site in GDX_RACE_RESOLVE_MEMO GDX_RACE_STUB_MEMO GDX_RACE_CLASS_MEMO GDX_RACE_N64RANGE_MEMO \
               GDX_RACE_PLACEHOLDER_MEMO GDX_RACE_TEXCOPY; do
    if ! grep -qE "GdxMemoOwnedFor\(${rr_site}\)" "${BRIDGE_SRC}"; then
        rr_fail="${rr_fail} the [brfast] memo owner latch is gone from the ${rr_site} site;"
    fi
done
if ! grep -qE 'static inline bool GdxMemoOwnedFor\(GdxRaceSite site\)' "${BRIDGE_SRC}" ||
   ! grep -qE 'GdxRaceNoteForeign\(site\)' "${BRIDGE_SRC}"; then
    rr_fail="${rr_fail} GdxMemoOwnedFor/the foreign-reach note is gone;"
fi
#  5. [memotear, NEXT.md B37] PAYLOAD BEFORE KEY. Every one of the five memos with a
#     publication word fills RETIRE -> payload+keys -> PUBLISH, with the publication word
#     (`filled` / `valid` / `count`) written LAST and only ever through GdxMemoPublish, and every
#     probe reads that word first and takes GdxMemoAcquire() before any key or payload field.
#     The old order stored the KEY first, so a reader matching a newly stored key could read the
#     PREVIOUS entry's payload -- for gRangeClassMemo that payload is a stride/endianness
#     decision, not a pointer, which is the one site where "silent, no crash" does not hold
#     (memotear-progress.md S2). Like the four above this is a fence on the SHAPE of the code:
#     Azahar time-slices both emulated cores onto one host thread, so no lane can reproduce it.
rr_retire=$(grep -c 'GdxMemoRetire(' "${BRIDGE_SRC}" || true)
rr_publish=$(grep -c 'GdxMemoPublish<' "${BRIDGE_SRC}" || true)
rr_acquire=$(grep -c 'GdxMemoAcquire();' "${BRIDGE_SRC}" || true)
# 5 fills, 5 probes; the helpers' own definitions do not match these patterns.
if [ "${rr_retire}" -lt 5 ] || [ "${rr_publish}" -lt 5 ]; then
    rr_fail="${rr_fail} a [brfast] memo fill no longer publishes payload-before-key (GdxMemoRetire x${rr_retire}, GdxMemoPublish x${rr_publish}, expected >= 5 each);"
fi
if [ "${rr_acquire}" -lt 5 ]; then
    rr_fail="${rr_fail} a [brfast] memo probe no longer takes GdxMemoAcquire() after reading the publication word (x${rr_acquire}, expected >= 5);"
fi
# The publication words may be READ freely but may only be WRITTEN through the two helpers, so a
# reintroduced `e.filled = 1;` beside the keys fails the build rather than quietly restoring the
# old order. `[^=]` keeps the probes' `== ` comparisons out.
if grep -qE '(e\.filled|memo->valid|memo->count)[[:space:]]*=[^=]' "${BRIDGE_SRC}"; then
    rr_fail="${rr_fail} a [brfast] memo publication word is stored directly instead of through GdxMemoRetire/GdxMemoPublish (key-before-payload is back);"
fi
if ! grep -qE '^static inline int GdxMemoOrder\(\)' "${BRIDGE_SRC}" ||
   ! grep -qE '^extern "C" int gdx_bridge_memoorder\(void\)' "${BRIDGE_SRC}"; then
    rr_fail="${rr_fail} the [native] memoorder lever or its receipt accessor is gone;"
fi
for rr_tbl in gHostRanges gRawN64Ranges gLoadedAssetSegments; do
    if ! grep -qE "^[[:space:]]*${rr_tbl}\.reserve\(" "${BRIDGE_SRC}"; then
        rr_fail="${rr_fail} ${rr_tbl} is no longer reserved;"
    fi
done
if [ -n "${rr_fail}" ]; then
    record "bridge cross-thread invariants" FAIL "${rr_fail# }"
else
    record "bridge cross-thread invariants" PASS "atomic version+epoch+rgba16-generation via bump helpers, render-thread segment publish deferred, 3 walk-scanned tables reserved, 6 brfast memos owner-latched (per site) + 5 publishing payload-before-key"
fi

# ------------------------------------------------------- schedule invariants --
# [covfinish, NEXT.md B48] The parity gate samples the frames a replay's `SHOT` schedule names,
# and what a SHOT does not sample the gate cannot see. Those schedules are DERIVED from a
# `[debug] cov=1` coverage census (coverage2-progress.md), which makes each one a claim about
# the replay CONTENT it was derived from — a claim that goes stale silently, while the gate
# keeps printing `divergent=0` over a shrinking sample.
#
# The full drift check re-runs the census and compares against the committed schedule; it needs
# an emulator, a libretro core and fzerox.o2r, takes ~10 minutes for the corpus, and therefore
# lives in tools/dev/covdrift.sh as a per-release check, NOT here. This is the half that costs
# nothing: static invariants over replays/, no guest run at all. It is not a weaker version of
# the same check — it catches a different failure. B47 was precisely this shape
# (explosion-pileup.gdxs carried the "placement is derived elsewhere" banner while the .gdxrec
# still held 39 hand-placed shots covering 51.9%), and this lint flags that file in 30 ms.
banner "schedule invariants"
SCHED_LOG="${HOST_TEST_DIR}/covsched-lint.log"
mkdir -p "${HOST_TEST_DIR}"
if python3 "${ROOT}/tools/dev/covsched.py" lint "${ROOT}/replays" >"${SCHED_LOG}" 2>&1; then
    record "schedule invariants" PASS "$(grep -c 'SHOTs' "${SCHED_LOG}") replay schedule(s): carried, ascending, on-stream, no gap over 300 ticks"
else
    cat "${SCHED_LOG}" >&2
    record "schedule invariants" FAIL "$(grep -m1 'schedule invariants:' "${SCHED_LOG}" | sed 's/^schedule invariants: //') — see tools/dev/covsched.py lint"
fi

# --------------------------------------------------------------- host tests --
# These are the desktop console tests that matter for the 3DS port (pointer
# packing + DL conversion) plus stream D's fs/zipshim smoke test. They are
# standalone by design, so we compile them directly — a full desktop configure
# (libultraship + SDL) is NOT required on macOS.
banner "host unit tests"
mkdir -p "${HOST_TEST_DIR}"

# Like run_host_test, but a failure is recorded as WARN and does not fail the gate. For tests
# that pass locally (macOS clang, Linux gcc 13) but have not yet been made to pass in the CI
# container; the log is still printed so the failure stays visible.
run_host_test_advisory() { # name bin cwd
    local name="$1" bin="$2" cwd="$3" log="${HOST_TEST_DIR}/$1.log"
    if (cd "$cwd" && "$bin" > "$log" 2>&1); then
        echo "PASS: ${name}  ($(tail -1 "$log"))"
        record "test: ${name}" PASS "$(tail -1 "$log")"
    else
        echo "WARN (advisory): ${name} — last lines:" >&2
        tail -20 "$log" >&2
        record "test: ${name}" WARN "advisory; see ${log#${ROOT}/}"
    fi
}

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

# gdx_track_template_tests — [native] track index-template decoder (port/3ds/gfx/
# gdx3ds_track_templates.c, plain C) over synthetic lists, and over the user's fzerox.o2r records
# (GDX_O2R, else the Azahar SD copy; skipped without one) vs GDXTRK_TEMPLATES_DIGEST.
if cc -O1 -w -c "${ROOT}/port/3ds/assets/third_party/miniz/miniz.c" -o "${HOST_TEST_DIR}/trk_miniz.o" &&
   cc -O1 -Wall \
      "${ROOT}/port/tests/track_template_tests.c" \
      "${ROOT}/port/3ds/gfx/gdx3ds_track_templates.c" "${HOST_TEST_DIR}/trk_miniz.o" \
      -I"${ROOT}/port/3ds/gfx" -I"${ROOT}/port/3ds/assets/third_party/miniz" \
      -o "${HOST_TEST_DIR}/track_template_tests"; then
    run_host_test "gdx_track_template_tests" "${HOST_TEST_DIR}/track_template_tests" "${HOST_TEST_DIR}"
else
    record "test: gdx_track_template_tests" FAIL "compile failed"
fi

# gdx_interp_host_tests — the owned F3DEX2 interpreter (port/3ds/gfx/n64/) against a mock
# backend + mock resolver seam (docs/research/native/06-interp-plan.md §6.3). n64/ first on
# the include path: port/gdx_interp.h is a different header.
if c++ -std=c++20 -O1 \
      "${ROOT}/port/tests/gdx_interp_host_tests.cpp" \
      "${ROOT}"/port/3ds/gfx/n64/gdx_{dlsrc,dl,interp,cc,tmem,texcache,texskip,machsplit,nicensus,nirace,setload,rsp,tri,rect,hud_replay,hud_direct,hud_retain,nrr_hud,sky_retain,dlmemo}.cpp \
      "${ROOT}/port/3ds/gfx/gdx3ds_cov.c" \
      "${ROOT}/port/3ds/gfx/gdx3ds_hitch_ev.c" \
      "${ROOT}/port/3ds/gfx/gdx3ds_cpucut.c" \
      "${ROOT}/port/3ds/gfx/gdx3ds_xf_census.c" \
      "${ROOT}/port/3ds/gfx/gdx3ds_replay_tags.c" \
      -I"${ROOT}/port/3ds/gfx/n64" -I"${ROOT}/port/3ds/gfx" -I"${ROOT}/port/3ds/include" \
      -I"${ROOT}/port" -I"${ROOT}/port/tests" \
      -o "${HOST_TEST_DIR}/gdx_interp_host_tests"; then
    run_host_test "gdx_interp_host_tests" "${HOST_TEST_DIR}/gdx_interp_host_tests" "${HOST_TEST_DIR}"
else
    record "test: gdx_interp_host_tests" FAIL "compile failed"
fi

# gdx_gfx_atlas_uv_tests — the pow2-padding / [trectbatch] atlas-view UV clamp contract
# (RESIDUALS-2): drives gdx3ds_pad_clamp_limit + gdx3ds_uv_prep/gdx3ds_uv_pack, the real
# production derivations, and carries the negative control for "fold uOff into the clamp
# limit" (which would walk an atlas view's sample out of its own cell).
if c++ -std=c++20 -O1 -ffp-contract=off \
      "${ROOT}/port/tests/gfx_atlas_uv_tests.cpp" \
      -I"${ROOT}/port/3ds/gfx" \
      -o "${HOST_TEST_DIR}/gfx_atlas_uv_tests"; then
    run_host_test "gdx_gfx_atlas_uv_tests" "${HOST_TEST_DIR}/gfx_atlas_uv_tests" "${HOST_TEST_DIR}"
else
    record "test: gdx_gfx_atlas_uv_tests" FAIL "compile failed"
fi

# gdx3ds_texswz_host_tests — N60TEX [debug] tex_fast: the upload's word/unrolled texel writer
# (port/3ds/gfx/gdx3ds_texswz.h) leaves the PICA tiled buffer byte-identical to the stock
# per-texel loop, over every extent 1..64 x 1..64 and large ones, padding sentinel-checked.
if cc -O1 -Wall -Wextra -std=c99 \
      "${ROOT}/port/tests/texswz_tests.c" \
      -I"${ROOT}/port/3ds/gfx" \
      -o "${HOST_TEST_DIR}/texswz_tests"; then
    run_host_test "gdx3ds_texswz_host_tests" "${HOST_TEST_DIR}/texswz_tests" "${HOST_TEST_DIR}"
else
    record "test: gdx3ds_texswz_host_tests" FAIL "compile failed"
fi

# gdx3ds_audiocrk_host_tests — [debug] audiocrk (port/3ds/audio/gdx3ds_audio_crk.c), the
# o3dscrackle census that splits "it crackles" into ring underrun / DSP-queue underrun / ndsp
# frame drops / clipping / boundary splice. Checks the classification a hardware card is read
# by: padding is never a boundary, a splice lands in cstep not step, rails and drops count.
if cc -O1 -Wall -Wextra -std=c99 \
      "${ROOT}/port/tests/audiocrk_tests.c" \
      "${ROOT}/port/3ds/audio/gdx3ds_audio_crk.c" \
      -I"${ROOT}/port/3ds/audio" \
      -o "${HOST_TEST_DIR}/audiocrk_tests"; then
    run_host_test "gdx3ds_audiocrk_host_tests" "${HOST_TEST_DIR}/audiocrk_tests" "${HOST_TEST_DIR}"
else
    record "test: gdx3ds_audiocrk_host_tests" FAIL "compile failed"
fi

# gdx3ds_ndsphash_host_tests — [debug] ndsphash, the content-addressed render digest
# (port/gdx_ndsp_hash.c). The audio analogue of a frame hash, and the only audio gate this
# port has: `fhdiff` cannot see a dropped sample, a swapped instrument or a pitch error, and
# `audioring`'s level census cannot either (audioasync §5). Compiles the module UNMODIFIED and
# drives it through its public API -- no emulator, no DSP, no ROM.
# Case 3 is the one that earns its place in CI: it moves every RDRAM address by 1 MB, which is
# what taking a 10.25 MB blob off the heap does (audiodiet), and asserts the digest does NOT
# move. Without that masking the cross-run comparison shares no keys and reports a green line
# over nothing -- the vacuous gate covfinish refused an audio census family over.
if cc -O1 -Wall -Wextra -std=c99 \
      "${ROOT}/port/tests/ndsphash_tests.c" \
      "${ROOT}/port/gdx_ndsp_hash.c" \
      -I"${ROOT}/port" \
      -o "${HOST_TEST_DIR}/ndsphash_tests"; then
    run_host_test "gdx3ds_ndsphash_host_tests" "${HOST_TEST_DIR}/ndsphash_tests" "${HOST_TEST_DIR}"
else
    record "test: gdx3ds_ndsphash_host_tests" FAIL "compile failed"
fi

# gdx_dsp_host_tests — the DSP KERNELS themselves (port/n64_audio_hle.c), driven through its real
# public entry point gdx_audio_hle_run() with synthetic Acmd command lists. No emulator, no ROM, no
# DSP firmware. Mirrors port/CMakeLists.txt's gdx_dsp_tests target.
#
# Why this is a separate gate from gdx3ds_ndsphash_host_tests, which also says "audio": ndsphash
# hashes the same render request against the bytes it produced, so it proves the kernels are
# CONSISTENT — across runs, across [debug] audio_stream, across the sample cache. It cannot prove
# they are CORRECT: a wrong VADPCM decode is wrong identically every run and every ndsphash fold
# stays green (ndsphash-progress.md §6). This suite is the only correctness claim the port has about
# the decoder, and it holds the audible-artefact cases — loop-wrap continuity (a click), deferred
# clamping across a loud transient (a crushed attack), the last-frame preamble contract (a waveform
# cliff at every tick boundary), reverb decay-not-growth.
#
# It was OUT of CI until A17/dspclick because it had never linked outside CMake; ndsphash supplied
# the two missing stubs and dspclick found that its two standing failures were malformed TEST
# CODEBOOKS, not a decoder bug (dspclick-progress.md). It is GREEN and must stay green.
#
# audiomix added TESTs 21-23: the MIX -- the stage dspclick §6.2 named as the suite's blind spot.
# They also gate port/gdx_audio_mix_census.c, the [debug] audiomix headroom census, which is armed
# BY these tests (gdx_audiomix_configure) and is inert in the shipped build.
#
# C4k: the suite runs TWICE, the second time on the [debug] audio_simd fast kernels
# (GDX_TEST_HLE_FAST=1): every correctness claim it makes must hold on the path the Old 3DS
# ships, not only on the reference.
if cc -O1 -Wall -Wextra -Wno-unused-parameter -std=c99 \
      "${ROOT}/port/tests/dsp_tests.c" \
      "${ROOT}/port/n64_audio_hle.c" \
      "${ROOT}/port/n64_audio_hle_fast.c" \
      "${ROOT}/port/gdx_audio_hle_cost.c" \
      "${ROOT}/port/gdx_ndsp_hash.c" \
      "${ROOT}/port/gdx_audio_mix_census.c" \
      "${ROOT}/port/gdx_dev_gates.c" \
      -I"${ROOT}/port" -lm \
      -o "${HOST_TEST_DIR}/dsp_tests"; then
    run_host_test "gdx_dsp_host_tests" "${HOST_TEST_DIR}/dsp_tests" "${HOST_TEST_DIR}"
    GDX_TEST_HLE_FAST=1 run_host_test "gdx_dsp_host_tests_simd" "${HOST_TEST_DIR}/dsp_tests" \
        "${HOST_TEST_DIR}"
else
    record "test: gdx_dsp_host_tests" FAIL "compile failed"
fi

# gdx_hle_fast_parity_tests — C4k's bit-exactness gate for [debug] audio_simd
# (port/n64_audio_hle_fast.c). The REAL interpreter runs every command list twice, reference
# and fast, from the same state; all of DMEM, every state buffer, every saved byte and the
# [spike] last-writer map must be equal. 15,000 lists from three generators (synthesis.c-shaped
# voices, anything-goes fields, deliberate aliasing), a one-bit negative control, the resample
# table's int32 bound, and a floor on every packed path and every in-kernel fallback so a green
# run cannot mean "the proof always refused". Built at -O2 with -fwrapv like the 3DS build; off
# ARM the packed paths run on exact C emulations of SMLAD/SMUAD/QADD16/SMLAWx.
if cc -O2 -Wall -Wextra -Wno-unused-parameter -std=c99 -fwrapv -fno-strict-aliasing \
      "${ROOT}/port/tests/hle_fast_tests.c" \
      "${ROOT}/port/n64_audio_hle.c" \
      "${ROOT}/port/n64_audio_hle_fast.c" \
      "${ROOT}/port/gdx_audio_hle_cost.c" \
      "${ROOT}/port/gdx_ndsp_hash.c" \
      "${ROOT}/port/gdx_audio_mix_census.c" \
      "${ROOT}/port/gdx_dev_gates.c" \
      -I"${ROOT}/port" -lm \
      -o "${HOST_TEST_DIR}/hle_fast_tests"; then
    run_host_test "gdx_hle_fast_parity_tests" "${HOST_TEST_DIR}/hle_fast_tests" "${HOST_TEST_DIR}"
else
    record "test: gdx_hle_fast_parity_tests" FAIL "compile failed"
fi

# gdx3ds_config_host_tests — the ini table (set / unset / save). gdx3ds_config.c is
# plain C over stdio, so the guest-side behaviour of the net `INI` / `INI DEL` commands
# can be verified on the build machine instead of only on hardware. Needs a scratch CWD:
# the test writes its fixture ini next to itself.
if cc -O1 -Wall \
      "${ROOT}/port/3ds/os/test/gdx3ds_config_host_tests.c" \
      "${ROOT}/port/3ds/os/gdx3ds_config.c" \
      "${ROOT}/port/3ds/gdx3ds_preset_n60.c" \
      -I"${ROOT}/port/3ds/os" -I"${ROOT}/port/3ds" \
      -o "${HOST_TEST_DIR}/config_host_tests"; then
    rm -rf "${HOST_TEST_DIR}/config-cwd"
    mkdir -p "${HOST_TEST_DIR}/config-cwd"
    run_host_test "gdx3ds_config_host_tests" "${HOST_TEST_DIR}/config_host_tests" "${HOST_TEST_DIR}/config-cwd"
else
    record "test: gdx3ds_config_host_tests" FAIL "compile failed"
fi
# preset-fix: gdx3ds_preset_mustship_tests -- every lever the last known-good hardware cards ran
# (TEST-*-0c2668a) must resolve from an EMPTY ini through the per-model auto preset (o3ds-auto /
# n60), and the test-only / user-facing keys must NOT be in either table. card-2 lost them when its
# inis went to "shipped defaults" and the Old 3DS ran 2-3x slower.
if cc -O1 -Wall -Wextra \
      "${ROOT}/port/3ds/os/test/gdx3ds_preset_mustship_tests.c" \
      "${ROOT}/port/3ds/os/gdx3ds_config.c" \
      "${ROOT}/port/3ds/gdx3ds_preset_n60.c" \
      -I"${ROOT}/port/3ds/os" -I"${ROOT}/port/3ds" \
      -o "${HOST_TEST_DIR}/preset_mustship_tests"; then
    rm -rf "${HOST_TEST_DIR}/mustship-cwd"
    mkdir -p "${HOST_TEST_DIR}/mustship-cwd"
    run_host_test "gdx3ds_preset_mustship_tests" "${HOST_TEST_DIR}/preset_mustship_tests" "${HOST_TEST_DIR}/mustship-cwd"
else
    record "test: gdx3ds_preset_mustship_tests" FAIL "compile failed"
fi

# gdx3ds_launch_host_tests — N60LAUNCH launch variants: [launch] hold bindings, the boot-cycle
# pick, [variant.X] -> RAM-only overrides (section.key form, override-table-full reported), log
# tags; over the real ini loader. Scratch CWD: the test writes its fixture ini.
if cc -O1 -Wall -Wextra -Wno-unused-parameter \
      "${ROOT}/port/3ds/test/gdx3ds_launch_host_tests.c" \
      "${ROOT}/port/3ds/gdx3ds_launch_core.c" \
      "${ROOT}/port/3ds/os/gdx3ds_config.c" \
      -I"${ROOT}/port/3ds" -I"${ROOT}/port/3ds/os" \
      -o "${HOST_TEST_DIR}/launch_host_tests"; then
    rm -rf "${HOST_TEST_DIR}/launch-cwd"
    mkdir -p "${HOST_TEST_DIR}/launch-cwd"
    run_host_test "gdx3ds_launch_host_tests" "${HOST_TEST_DIR}/launch_host_tests" "${HOST_TEST_DIR}/launch-cwd"
else
    record "test: gdx3ds_launch_host_tests" FAIL "compile failed"
fi

# gdx3ds_native_cfg_host_tests — the [native] lever resolver (port/3ds/gfx/gdx3ds_native_cfg.c).
# Three rules, each of which has cost a session: `all` covers the Tier-2 set only (interp is
# read as written), `all=1` only ever raises a lever, and interp_verify=1 with interp=1 forces
# fx_hud to 0 because the batch-for-batch oracle cannot compare a folded 2D batch against an
# unfolded one (docs/research/native/verifymatrix-progress.md).
if cc -O1 -Wall \
      "${ROOT}/port/3ds/gfx/test/gdx3ds_native_cfg_host_tests.c" \
      "${ROOT}/port/3ds/gfx/gdx3ds_native_cfg.c" \
      -I"${ROOT}/port/3ds/gfx" \
      -o "${HOST_TEST_DIR}/native_cfg_host_tests"; then
    run_host_test "gdx3ds_native_cfg_host_tests" "${HOST_TEST_DIR}/native_cfg_host_tests" "${HOST_TEST_DIR}"
else
    record "test: gdx3ds_native_cfg_host_tests" FAIL "compile failed"
fi

# gdx3ds_dynlod_host_tests — the DYNLOD rival-detail controller (port/3ds/gdx3ds_dynlod.c)
# driven by synthetic render-task frame times through a stub <3ds.h> whose tick source the
# test owns (port/3ds/test/host/). Each case carries a PLANT — raising the tier makes the
# very frame the controller measures cheaper — because that feedback is what the hysteresis
# has to survive; the gate is tier transitions per 1000 frames, and a transition re-tiers
# every distant rival at once (decomp-port-rival-detail.patch), which is what the hardware
# report saw as wobble and pop-in. See docs/research/native/dynlod-progress-native.md.
if cc -O1 -Wall -Wextra -Wno-unused-parameter -DGDX3DS_DYNLOD_HOST_TEST \
      "${ROOT}/port/3ds/test/gdx3ds_dynlod_host_tests.c" \
      "${ROOT}/port/3ds/gdx3ds_dynlod.c" \
      -I"${ROOT}/port/3ds/test/host" -I"${ROOT}/port/3ds" -I"${ROOT}/port/3ds/os" \
      -I"${ROOT}/port/3ds/include" -I"${ROOT}/port/3ds/gfx" \
      -o "${HOST_TEST_DIR}/dynlod_host_tests"; then
    run_host_test "gdx3ds_dynlod_host_tests" "${HOST_TEST_DIR}/dynlod_host_tests" "${HOST_TEST_DIR}"
else
    record "test: gdx3ds_dynlod_host_tests" FAIL "compile failed"
fi

# gdx_hud_placement_host_tests — the per-element top-screen HUD gate (port/gdx_hud_placement.c,
# nihudtop Phase 3): modes 0-2 x every GDX_HUD_ELEM_* x numPlayers 1-2 must match the pre-Phase-3
# all-or-nothing answer exactly, plus the CUSTOM mask override and its numPlayers>1 force-off.
# -DGDX_HUD_PLACEMENT_HOST_TEST compiles in a mode-override seam the real build never defines
# (see that file's own comment) since there is no gdx3ds_bottomhud.c/ini here to drive it.
if cc -O1 -Wall -Wextra -Wno-unused-parameter -DGDX_HUD_PLACEMENT_HOST_TEST \
      "${ROOT}/port/tests/gdx_hud_placement_host_tests.c" \
      "${ROOT}/port/gdx_hud_placement.c" \
      -I"${ROOT}/port" \
      -o "${HOST_TEST_DIR}/hud_placement_host_tests"; then
    run_host_test "gdx_hud_placement_host_tests" "${HOST_TEST_DIR}/hud_placement_host_tests" "${HOST_TEST_DIR}"
else
    record "test: gdx_hud_placement_host_tests" FAIL "compile failed"
fi

# gdx_net_enabled_default_host_tests — n60lead hwlink round: [net] enabled's DEFAULT
# resolution (port/gdx_net_enabled_default.c). An explicit ini key always wins; only when
# the ini has no usable key does the launch method (3dslink netloader vs. hbmenu/SD/CIA)
# get a vote. Split out of port/3ds/net/gdx3ds_net.c, which pulls in <3ds.h> and cannot
# compile for the host, so this is the real function gdx3ds_net_init calls, not a
# reimplementation of its logic. The emulator lanes cannot simulate a netloader launch
# (no __3dslink_host to set), so this table is this round's stand-in for that A/B.
if cc -O1 -Wall -Wextra -Wno-unused-parameter \
      "${ROOT}/port/tests/gdx_net_enabled_default_host_tests.c" \
      "${ROOT}/port/gdx_net_enabled_default.c" \
      -I"${ROOT}/port" \
      -o "${HOST_TEST_DIR}/net_enabled_default_host_tests"; then
    run_host_test "gdx_net_enabled_default_host_tests" "${HOST_TEST_DIR}/net_enabled_default_host_tests" "${HOST_TEST_DIR}"
else
    record "test: gdx_net_enabled_default_host_tests" FAIL "compile failed"
fi

# gdx_mp_lockstep_host_tests — mp-spike: the lockstep protocol core (port/gdx_mp_lockstep.c,
# the pure half of [net] mp = host|join) against simulated consoles: 2/4/8 players over a lossy,
# duplicating, reordering network must feed every console the identical pad sequence; a one-bit
# state perturbation must be caught at the first hash tick after it; hostile datagrams
# (truncated, wrong magic/version/session, oversize count, spoofed sender, far-future ticks)
# must be refused without touching state.
if cc -O1 -Wall -Wextra -Wno-unused-parameter \
      "${ROOT}/port/tests/gdx_mp_lockstep_host_tests.c" \
      "${ROOT}/port/gdx_mp_lockstep.c" \
      -I"${ROOT}/port" \
      -o "${HOST_TEST_DIR}/mp_lockstep_host_tests"; then
    run_host_test "gdx_mp_lockstep_host_tests" "${HOST_TEST_DIR}/mp_lockstep_host_tests" "${HOST_TEST_DIR}"
else
    record "test: gdx_mp_lockstep_host_tests" FAIL "compile failed"
fi

# gdx_mp_timesync_host_tests — mp-timesync: frame-advantage pacing (port/gdx_mp_timesync.c) over
# the real lockstep core, in a two-clock model of the 3DS race loop (vblank-bound and CPU-bound
# consoles, citro3d's adaptive vblank sync, a lossy jittery network): the fast console's stalls
# against a 10%-slower peer must mostly go with no slower race, equal consoles must not be slowed,
# and every console must consume the identical pad sequence with the lever on or off.
if cc -O1 -Wall -Wextra -Wno-unused-parameter \
      "${ROOT}/port/tests/gdx_mp_timesync_host_tests.c" \
      "${ROOT}/port/gdx_mp_timesync.c" \
      "${ROOT}/port/gdx_mp_lockstep.c" \
      -I"${ROOT}/port" -lm \
      -o "${HOST_TEST_DIR}/mp_timesync_host_tests"; then
    run_host_test "gdx_mp_timesync_host_tests" "${HOST_TEST_DIR}/mp_timesync_host_tests" "${HOST_TEST_DIR}"
else
    record "test: gdx_mp_timesync_host_tests" FAIL "compile failed"
fi

# gdx_mp_netab_host_tests — card-3: the online levers' per-race A/B (port/gdx_mp_netab.c): race k
# from the START gen, mp_timesync_max_ms = ab (3 vs 16 ms, bit 0) and mp_ndm_exclusive = 2 (1 vs 0,
# bit 1) orthogonal over every 4 races, the non-A/B values unchanged, the [netab] receipt.
if cc -O1 -Wall -Wextra \
      "${ROOT}/port/tests/gdx_mp_netab_host_tests.c" \
      "${ROOT}/port/gdx_mp_netab.c" \
      -I"${ROOT}/port" \
      -o "${HOST_TEST_DIR}/mp_netab_host_tests"; then
    run_host_test "gdx_mp_netab_host_tests" "${HOST_TEST_DIR}/mp_netab_host_tests" "${HOST_TEST_DIR}"
else
    record "test: gdx_mp_netab_host_tests" FAIL "compile failed"
fi

# gdx_mp_txq_host_tests — the UDS and socket service threads' lock-free SPSC datagram rings (a
# pthread producer/consumer moving 200000 datagrams in order), the socket thread's static byte
# queue (variable-length records across wraps, a pthread pair moving 200000 of 5..1480 bytes), the
# INPUT coalescing scheduler (newest wins per (destination, stream), the minimum send interval, a
# star host's relayed stream kept apart from its own, steady-cadence repeats that a stall's
# unchanged resend does not earn), the socket thread's loop body (port/gdx_mp_socsvc.c: in-order
# sends, one empty recv per drain, back-pressure, a pthread service thread against a simulated
# socket), and two lockstep consoles through it (bursty Old 3DS ticks, 5% loss, repeats off / on)
# that must consume identical pads.
if cc -O1 -Wall -Wextra -Wno-unused-parameter \
      "${ROOT}/port/tests/gdx_mp_txq_host_tests.c" \
      "${ROOT}/port/gdx_mp_txq.c" \
      "${ROOT}/port/gdx_mp_socsvc.c" \
      "${ROOT}/port/gdx_mp_lockstep.c" \
      -I"${ROOT}/port" -lpthread \
      -o "${HOST_TEST_DIR}/mp_txq_host_tests"; then
    run_host_test "gdx_mp_txq_host_tests" "${HOST_TEST_DIR}/mp_txq_host_tests" "${HOST_TEST_DIR}"
else
    record "test: gdx_mp_txq_host_tests" FAIL "compile failed"
fi

# gdx_mp_online_host_tests — mp-online: the pure halves of internet multiplayer
# (port/gdx_mp_online.c: GXRV/GXLB wire formats, room codes, the per-link punch/relay state
# machine, buckets, blocklist, the content hash; port/gdx_mp_lobby.c: the lobby) plus a
# simulated 2/3/4-console session over open / cone / symmetric NATs, forced relay, 100 ms RTT
# and 25% loss, which must reach identical pads and pick the expected input delay; and
# port/gdx_mp_remove.c, a console dropping out mid-race (the others agree on the tick); and
# port/gdx_mp_lan.c, the console-hosted LAN room + the NEARBY beacon; and
# port/gdx_mp_uds_bridge.c, the same room over 3DS local wireless (node endpoints, the derived
# room code, the UDS beacon both ways, the merged WiFi + local-wireless NEARBY list).
if cc -O1 -Wall -Wextra -Wno-unused-parameter \
      "${ROOT}/port/tests/gdx_mp_online_host_tests.c" \
      "${ROOT}/port/gdx_mp_online.c" \
      "${ROOT}/port/gdx_mp_lobby.c" \
      "${ROOT}/port/gdx_mp_lockstep.c" \
      "${ROOT}/port/gdx_mp_remove.c" \
      "${ROOT}/port/gdx_mp_lan.c" \
      "${ROOT}/port/gdx_mp_uds_bridge.c" \
      "${ROOT}/port/gdx_mp_beacon.c" \
      "${ROOT}/port/gdx_mp_pause.c" \
      -I"${ROOT}/port" \
      -o "${HOST_TEST_DIR}/mp_online_host_tests"; then
    run_host_test "gdx_mp_online_host_tests" "${HOST_TEST_DIR}/mp_online_host_tests" "${HOST_TEST_DIR}"
else
    record "test: gdx_mp_online_host_tests" FAIL "compile failed"
fi

# mp_content_hash_tests — the multiplayer content hash (gdx_mpo_zip_cd_hash) must not depend on
# how the PC that prebaked the archives compressed them: tools/prebake/test_content_hash.py
# re-packs a synthetic archive through prebake.py's store step at zlib levels 1/6/9 (and as-is)
# and runs the console's own hash over each (port/tests/gdx_mpo_zip_hash_cli.c).
if cc -O1 -Wall -Wextra \
      "${ROOT}/port/tests/gdx_mpo_zip_hash_cli.c" \
      "${ROOT}/port/gdx_mp_online.c" \
      -I"${ROOT}/port" \
      -o "${HOST_TEST_DIR}/mpo_zip_hash_cli"; then
    CH_LOG="${HOST_TEST_DIR}/mp_content_hash_tests.log"
    if python3 "${ROOT}/tools/prebake/test_content_hash.py" "${HOST_TEST_DIR}/mpo_zip_hash_cli" > "${CH_LOG}" 2>&1; then
        echo "PASS: mp_content_hash_tests  ($(tail -1 "${CH_LOG}"))"
        record "test: mp_content_hash_tests" PASS "$(tail -1 "${CH_LOG}")"
    else
        echo "FAIL: mp_content_hash_tests — last lines:" >&2
        tail -5 "${CH_LOG}" >&2
        record "test: mp_content_hash_tests" FAIL "see ${CH_LOG#${ROOT}/}"
    fi
else
    record "test: mp_content_hash_tests" FAIL "compile failed"
fi

# mp_rendezvous_tests — tools/mp-rendezvous/gdx_rendezvous.py, the self-hostable rendezvous +
# relay server: every request type, hostile input (fuzz, truncation, spoofed slots and keys,
# amplification, floods, code guessing, table bounds), room expiry, the latency/loss proxy, and
# an interop run of the console's own C wire code against the real server on loopback.
RVTEST_LOG="${HOST_TEST_DIR}/mp_rendezvous_tests.log"
if python3 "${ROOT}/tools/mp-rendezvous/test_rendezvous.py" > "${RVTEST_LOG}" 2>&1; then
    echo "PASS: mp_rendezvous_tests  ($(tail -1 "${RVTEST_LOG}"))"
    record "test: mp_rendezvous_tests" PASS "$(tail -1 "${RVTEST_LOG}")"
else
    echo "FAIL: mp_rendezvous_tests — last lines:" >&2
    tail -20 "${RVTEST_LOG}" >&2
    record "test: mp_rendezvous_tests" FAIL "see ${RVTEST_LOG#${ROOT}/}"
fi

# gdx_mp_drop_host_tests — mp-uds: drop-out (port/gdx_mp_drop.c) over the real lockstep core.
# 2-4 simulated consoles, star (UDS / udp-lan: the host relays) and mesh (mp-online), delay 1-8,
# up to 35% loss: a player dies silently or after a LEAVE (two back to back, a zombie that must
# hear its removal); every survivor must agree on the removal tick T, feed identical pads (the
# leaver's real ones before T, neutral from T) and reach identical state. Hostile DROP-family
# datagrams and a 200k fuzz corpus must be refused without corrupting the session.
if cc -O1 -Wall -Wextra -Wno-unused-parameter \
      "${ROOT}/port/tests/gdx_mp_drop_host_tests.c" \
      "${ROOT}/port/gdx_mp_drop.c" \
      "${ROOT}/port/gdx_mp_lockstep.c" \
      -I"${ROOT}/port" \
      -o "${HOST_TEST_DIR}/mp_drop_host_tests"; then
    run_host_test "gdx_mp_drop_host_tests" "${HOST_TEST_DIR}/mp_drop_host_tests" "${HOST_TEST_DIR}"
else
    record "test: gdx_mp_drop_host_tests" FAIL "compile failed"
fi

# gdx_mp_beacon_host_tests — mp-uds: the NEARBY room beacon (port/gdx_mp_beacon.c): round trip,
# the "<name>'s room n/4 N3DS" label, build/content/proto compatibility, profile-name UTF-16 ->
# UTF-8, the UDS passphrase, and hostile appdata (every truncation, every bit flip, 300k fuzz
# blobs) under AddressSanitizer + UBSan where the host compiler has them.
BEACON_SAN="-fsanitize=address,undefined"
if ! echo 'int main(void){return 0;}' | cc -x c - ${BEACON_SAN} -o "${HOST_TEST_DIR}/san_probe" >/dev/null 2>&1; then
    BEACON_SAN=""
fi
if cc -O1 -g ${BEACON_SAN} -Wall -Wextra -Wno-unused-parameter \
      "${ROOT}/port/tests/gdx_mp_beacon_host_tests.c" \
      "${ROOT}/port/gdx_mp_beacon.c" \
      -I"${ROOT}/port" \
      -o "${HOST_TEST_DIR}/mp_beacon_host_tests"; then
    run_host_test "gdx_mp_beacon_host_tests" "${HOST_TEST_DIR}/mp_beacon_host_tests" "${HOST_TEST_DIR}"
else
    record "test: gdx_mp_beacon_host_tests" FAIL "compile failed"
fi

# gdx3ds_frameskip_host_tests — the FRAMESKIP governor (port/3ds/gdx3ds_frameskip_gov.c, NEXT.md
# C4l) against a simulated console: logic ticks/s must reach ~59.94 on the Old 3DS race and
# time-trial frame costs and never exceed it in a fast scene, the skip cap must bind, FIXED must
# render exactly its phase, and an oracle veto must always force a render.
# See docs/research/native/o3dsskip-progress.md.
if cc -O1 -Wall -Wextra -Wno-unused-parameter \
      "${ROOT}/port/3ds/test/gdx3ds_frameskip_host_tests.c" \
      "${ROOT}/port/3ds/gdx3ds_frameskip_gov.c" \
      "${ROOT}/port/3ds/gdx3ds_frameskip_steady.c" \
      "${ROOT}/port/3ds/gdx3ds_frameskip_race0.c" \
      -I"${ROOT}/port/3ds" \
      -o "${HOST_TEST_DIR}/frameskip_host_tests"; then
    run_host_test "gdx3ds_frameskip_host_tests" "${HOST_TEST_DIR}/frameskip_host_tests" "${HOST_TEST_DIR}"
else
    record "test: gdx3ds_frameskip_host_tests" FAIL "compile failed"
fi

# gdx3ds_trkrr_math_host_tests — [native] track_retained's geometry (NEXT.md C5 Stage 3,
# port/3ds/gfx/gdx3ds_track_retained_math.h): the pass origin and near-plane w recovered from the
# composed matrix, near-plane clip points that are bit-identical for both triangles sharing an
# edge (watertight), and the visible-region float24 bound checked for rigour against 600k
# sampled visible points; plus [native] track_resident's per-ring check (C5 Stage 3c) checked
# conservative against 80k points of accepted rings; plus [native] track_nearclip's lean clipper
# (C5 Stage 3d) checked bit-identical to ClipPoly on 33k strip triangles with its edge cache hit.
# See docs/research/native/c5s3-progress.md, c5s3c-progress.md and c5s3d-progress.md.
if c++ -O1 -std=c++17 -Wall -Wextra -Wno-unused-parameter \
      "${ROOT}/port/3ds/test/gdx3ds_trkrr_math_host_tests.cpp" \
      -I"${ROOT}/port/3ds/gfx" \
      -o "${HOST_TEST_DIR}/trkrr_math_host_tests"; then
    run_host_test "gdx3ds_trkrr_math_host_tests" "${HOST_TEST_DIR}/trkrr_math_host_tests" "${HOST_TEST_DIR}"
else
    record "test: gdx3ds_trkrr_math_host_tests" FAIL "compile failed"
fi

# gdx3ds_replay_math_host_tests — C6 pose-replay (port/3ds/gfx/gdx3ds_replay_math.h): the
# uniform patches (legacy projection, gpuxf xmvp, the resident re-base, the light rotation)
# reproduce the vertex B's own frame would produce, and the PICA command-list scan finds every
# f32 uniform word GPUCMD_Add writes. See docs/research/native/c6replay-progress.md.
if c++ -O1 -std=c++17 -Wall -Wextra -Wno-unused-parameter \
      "${ROOT}/port/3ds/test/gdx3ds_replay_math_host_tests.cpp" \
      -I"${ROOT}/port/3ds/gfx" \
      -o "${HOST_TEST_DIR}/replay_math_host_tests"; then
    run_host_test "gdx3ds_replay_math_host_tests" "${HOST_TEST_DIR}/replay_math_host_tests" "${HOST_TEST_DIR}"
else
    record "test: gdx3ds_replay_math_host_tests" FAIL "compile failed"
fi

# gdx3ds_snapcopy_host_tests — snap-fast: the snapshot copy engine (port/3ds/gdx3ds_snapcopy.c),
# every mode against memcpy (aligned / misaligned, every length to 600 B and a 900 KB image, guard
# bytes intact) and the dirty-block sync (dst == src afterwards, exactly the dirty blocks written).
# The ARM LDM/STM bursts themselves run under the emulator gate ([debug] snap_test).
if cc -O1 -Wall -Wextra \
      "${ROOT}/port/3ds/test/gdx3ds_snapcopy_host_tests.c" "${ROOT}/port/3ds/gdx3ds_snapcopy.c" \
      -I"${ROOT}/port/3ds" \
      -o "${HOST_TEST_DIR}/snapcopy_host_tests"; then
    run_host_test "gdx3ds_snapcopy_host_tests" "${HOST_TEST_DIR}/snapcopy_host_tests" "${HOST_TEST_DIR}"
else
    record "test: gdx3ds_snapcopy_host_tests" FAIL "compile failed"
fi

# gdx3ds_perf_preset_host_tests — [perf] preset (port/3ds/gdx3ds_perf_preset.c, NEXT.md C4p):
# `auto` must leave every [perf] default at its shipped value on a New 3DS, select the o3ds
# values on an Old 3DS, and lose to any explicit ini key in both directions. The console
# model comes from a stub APT_CheckNew3DS the test owns (port/3ds/test/host/3ds.h).
if cc -O1 -Wall -Wextra -Wno-unused-parameter -DGDX3DS_PERF_PRESET_HOST_TEST \
      "${ROOT}/port/3ds/test/gdx3ds_perf_preset_host_tests.c" \
      "${ROOT}/port/3ds/gdx3ds_perf_preset.c" \
      "${ROOT}/port/3ds/gdx3ds_preset_n60.c" \
      -I"${ROOT}/port/3ds/test/host" -I"${ROOT}/port/3ds" -I"${ROOT}/port/3ds/os" \
      -I"${ROOT}/port/3ds/include" \
      -o "${HOST_TEST_DIR}/perf_preset_host_tests"; then
    run_host_test "gdx3ds_perf_preset_host_tests" "${HOST_TEST_DIR}/perf_preset_host_tests" "${HOST_TEST_DIR}"
else
    record "test: gdx3ds_perf_preset_host_tests" FAIL "compile failed"
fi

# gdx3ds_heapcfg_host_tests — libctru's heap-split formula (port/3ds/include/gdx3ds_heapcfg.h),
# checked against the three [mem-budget] grants the guest actually printed. The formula was
# transcribed by hand from a disassembly of the libctru archive this port links, and the whole
# Old 3DS budget — including "the shipped build is ~19 MB short on memtype 0" — rests on that
# transcription, so it gets a test rather than a citation. Header-only: no TU to compile in.
if cc -O1 -Wall -Wextra \
      "${ROOT}/port/3ds/test/gdx3ds_heapcfg_host_tests.c" \
      -I"${ROOT}/port/3ds/include" \
      -o "${HOST_TEST_DIR}/heapcfg_host_tests"; then
    run_host_test "gdx3ds_heapcfg_host_tests" "${HOST_TEST_DIR}/heapcfg_host_tests" "${HOST_TEST_DIR}"
else
    record "test: gdx3ds_heapcfg_host_tests" FAIL "compile failed"
fi

# gdx3ds_bui_host_tests — the bottom-screen product menus (artifacts/bottom-menus-design): the UI
# kit (port/3ds/gdx3ds_bui_kit.c, a call-for-call port of the design's mockup engine) renders the
# design's component specimens and every product screen (the gallery's canned snapshots) and each
# screen's RGB565 hash must equal the golden recorded when it matched the mockup pixel for pixel
# (GDX_BUI_MOCKUP_DIR re-runs that full diff); plus hit tests, focus navigation, the dev-console
# combo and the game-key mask (gdx3ds_bui_layout.h). The game art the screens draw is baked at
# startup from the user's fzerox.o2r (GDX_O2R, else the Azahar SD copy) by the same
# gdx3ds_hudbake.c the console runs at boot; without an archive the pixel goldens are skipped.
BUI_HOST_SRCS=("${ROOT}/port/3ds/test/gdx3ds_bui_host_tests.c" "${ROOT}/port/3ds/gdx3ds_bui_kit.c"
               "${ROOT}/port/3ds/gdx3ds_hudbake.c")
BUI_HOST_DEFS=(-DGDX_BUI_HOST=1)
for f in gdx3ds_bui_screens.c gdx3ds_bui_gallery.c gdx3ds_bui_racedash.c; do
    if [ -f "${ROOT}/port/3ds/${f}" ]; then
        BUI_HOST_SRCS+=("${ROOT}/port/3ds/${f}")
        BUI_HOST_DEFS=(-DGDX_BUI_HOST=1 -DGDX_BUI_HOST_SCREENS=1)
    fi
done
if cc -O1 -w -c "${ROOT}/port/3ds/assets/third_party/miniz/miniz.c" -o "${HOST_TEST_DIR}/bui_miniz.o" &&
   cc -O1 -Wall -Wextra -ffp-contract=off "${BUI_HOST_DEFS[@]}" "${BUI_HOST_SRCS[@]}" "${HOST_TEST_DIR}/bui_miniz.o" \
      -I"${ROOT}/port/3ds" -I"${ROOT}/port/3ds/assets/third_party/miniz" -lm \
      -o "${HOST_TEST_DIR}/bui_host_tests"; then
    run_host_test "gdx3ds_bui_host_tests" "${HOST_TEST_DIR}/bui_host_tests" "${HOST_TEST_DIR}"
else
    record "test: gdx3ds_bui_host_tests" FAIL "compile failed"
fi

# gdx3ds_hudbake_host_tests — the DASH art bake (port/3ds/gdx3ds_hudbake.c): every failure mode (no
# reader, no archive, wrong formats, truncated records) bakes blank glyphs + chrome without a crash,
# and with the user's fzerox.o2r every buffer hashes to the value of the headers it replaced.
# Plus the SD cache: a round trip restores the retail digest; a changed archive, a missing record,
# truncation, a damaged pixel, a bad magic and another build's cache are all refused, buffers untouched.
if cc -O1 -w -c "${ROOT}/port/3ds/assets/third_party/miniz/miniz.c" -o "${HOST_TEST_DIR}/hudbake_miniz.o" &&
   cc -O1 -Wall -Wextra -ffp-contract=off \
      "${ROOT}/port/3ds/test/gdx3ds_hudbake_host_tests.c" "${ROOT}/port/3ds/gdx3ds_hudbake.c" \
      "${HOST_TEST_DIR}/hudbake_miniz.o" \
      -I"${ROOT}/port/3ds" -I"${ROOT}/port/3ds/assets/third_party/miniz" -lm \
      -o "${HOST_TEST_DIR}/hudbake_host_tests"; then
    run_host_test "gdx3ds_hudbake_host_tests" "${HOST_TEST_DIR}/hudbake_host_tests" "${HOST_TEST_DIR}"
else
    record "test: gdx3ds_hudbake_host_tests" FAIL "compile failed"
fi

# gdx3ds_menu_tabbar_host_tests — hw bug n60-3d8, item 1: the MENU tab bar's column layout
# (port/3ds/gdx3ds_menu_tabbar_layout.h), the SAME header gdx3ds_menu.c's MenuLayoutTabs()
# calls, over every MAP/RACE shown-or-hidden combination -- asserting no two visible tabs
# overlap, OFF never overlaps a visible tab (the actual hardware report: "OFF was cover[ing]"
# RACE), and every column stays within 320px / 40 columns. Header-only: no TU to compile in.
if cc -O1 -Wall -Wextra \
      "${ROOT}/port/3ds/test/gdx3ds_menu_tabbar_host_tests.c" \
      -I"${ROOT}/port/3ds" \
      -o "${HOST_TEST_DIR}/menu_tabbar_host_tests"; then
    run_host_test "gdx3ds_menu_tabbar_host_tests" "${HOST_TEST_DIR}/menu_tabbar_host_tests" "${HOST_TEST_DIR}"
else
    record "test: gdx3ds_menu_tabbar_host_tests" FAIL "compile failed"
fi

# gdx3ds_course_share_host_tests — [ek] course_share: the .gdxcourse codec
# (port/3ds/gdx3ds_course_share_format.c), its QR transport (gdx3ds_course_share_qr.c) and the
# camera scanner's portable pipeline (gdx3ds_course_share_scan.c), the exact TUs the EK build
# compiles. Golden course checksum, every header and course refusal, a seeded mutation fuzz, QR
# frames rendered with the console's blit and decoded back with the vendored quirc, and
# synthetic camera frames (pages scaled, rotated, mirrored, keystoned, dimmed, blurred, noised,
# converted to YUV422) scanned through the session in any order with duplicates and foreign
# pages. Pure C, no disk data. The vendored libraries are third-party code and are compiled with
# -w; the TUs under test get the full warning set. Run twice: plain, then under ASan + UBSan
# (vendored objects included) when the host compiler links them.
CSHARE_DIR="${HOST_TEST_DIR}/course-share"
mkdir -p "${CSHARE_DIR}"
# $1 = object dir, $2.. = extra cc flags (the sanitizer pass)
build_course_share_vendor_objects() {
    local dir="$1"
    shift
    mkdir -p "${dir}"
    cc -O1 -std=c11 -w "$@" -c "${ROOT}/port/3ds/vendor/qrcodegen/qrcodegen.c" -o "${dir}/qrcodegen.o" || return 1
    local q
    for q in quirc decode identify version_db; do
        cc -O1 -std=c11 -w "$@" -c "${ROOT}/port/3ds/vendor/quirc/${q}.c" -o "${dir}/quirc_${q}.o" || return 1
    done
}
# $1 = object dir, $2 = output, $3.. = extra cc flags
build_course_share_host_tests() {
    local dir="$1"
    local out="$2"
    shift 2
    build_course_share_vendor_objects "${dir}" "$@" \
    && cc -O1 -Wall -Wextra -Werror -std=c11 -ffp-contract=off "$@" \
      "${ROOT}/port/3ds/test/gdx3ds_course_share_host_tests.c" \
      "${ROOT}/port/3ds/gdx3ds_course_share_format.c" \
      "${ROOT}/port/3ds/gdx3ds_course_share_qr.c" \
      "${ROOT}/port/3ds/gdx3ds_course_share_scan.c" \
      "${dir}"/*.o \
      -I"${ROOT}/port/3ds" -lm \
      -o "${out}"
}
if build_course_share_host_tests "${CSHARE_DIR}/plain" "${HOST_TEST_DIR}/course_share_host_tests"; then
    run_host_test_advisory "gdx3ds_course_share_host_tests" "${HOST_TEST_DIR}/course_share_host_tests" "${HOST_TEST_DIR}"
else
    record "test: gdx3ds_course_share_host_tests" FAIL "compile failed"
fi
# The sanitizer pass is skipped (not failed) only when the toolchain cannot link a trivial
# sanitized program; once it can, a compile failure or any report fails the gate.
CSHARE_SAN_FLAGS=(-g -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
if echo 'int main(void){return 0;}' | cc -x c "${CSHARE_SAN_FLAGS[@]}" - -o "${CSHARE_DIR}/san-probe" 2>/dev/null; then
    if build_course_share_host_tests "${CSHARE_DIR}/san" "${HOST_TEST_DIR}/course_share_host_tests_san" "${CSHARE_SAN_FLAGS[@]}"; then
        ASAN_OPTIONS=detect_leaks=0:abort_on_error=0 \
            run_host_test_advisory "gdx3ds_course_share_host_tests_asan_ubsan" "${HOST_TEST_DIR}/course_share_host_tests_san" "${HOST_TEST_DIR}"
    else
        record "test: gdx3ds_course_share_host_tests_asan_ubsan" FAIL "compile failed"
    fi
else
    echo "gdx3ds_course_share_host_tests_asan_ubsan: host compiler cannot link sanitizers, skipped"
fi

# gdx3ds_hudcustom_layout_host_tests — nibottom Phase 2, CUSTOM HUD mode: the pure clamp/snap/
# hit-test/ini-line logic (port/3ds/gdx3ds_hudcustom_layout.h/.c) that gdx3ds_bottomhud.c's
# CUSTOM-mode touch handler and hud.ini [custom] section reader/writer both call into. No
# <3ds.h>/decomp deps, so it compiles and links on the host exactly like the real 3DS TU.
if cc -O1 -Wall -Wextra -std=c11 \
      "${ROOT}/port/3ds/test/gdx3ds_hudcustom_layout_host_tests.c" \
      "${ROOT}/port/3ds/gdx3ds_hudcustom_layout.c" \
      -I"${ROOT}/port/3ds" \
      -o "${HOST_TEST_DIR}/hudcustom_layout_host_tests"; then
    run_host_test "gdx3ds_hudcustom_layout_host_tests" "${HOST_TEST_DIR}/hudcustom_layout_host_tests" "${HOST_TEST_DIR}"
else
    record "test: gdx3ds_hudcustom_layout_host_tests" FAIL "compile failed"
fi

# gdx_save_replace_tests — n60save: the crash-safe fzerox.sav / .gdd / ghost replace against a
# FAT-like stub that refuses rename onto an existing file (the 3DS SD archive does), with a power
# cut after every filesystem operation of the flush and of the boot's recovery.
if cc -O1 -Wall -Wextra -std=c11 \
      "${ROOT}/port/tests/gdx_save_replace_tests.c" \
      "${ROOT}/port/gdx_save_replace.c" \
      -I"${ROOT}/port" \
      -o "${HOST_TEST_DIR}/save_replace_tests"; then
    run_host_test "gdx_save_replace_tests" "${HOST_TEST_DIR}/save_replace_tests" "${HOST_TEST_DIR}"
else
    record "test: gdx_save_replace_tests" FAIL "compile failed"
fi

# gdx_disk_savefile_tests — heapfix (card-5 Old 3DS HOME-close hard lock): the 64DD sidecar flush
# streams the file out of the journal with ZERO C++ allocations (operator new throws during it), the
# file is byte-identical to the pre-fix image, and a failed write or an out-of-memory journal never
# throws and never touches the previous .gdd. The pre-fix flush std::terminates under this test.
if cc -O1 -Wall -Wextra -std=c11 -c "${ROOT}/port/gdx_save_replace.c" -I"${ROOT}/port" \
      -o "${HOST_TEST_DIR}/disk_savefile_replace.o" &&
   cc -O1 -std=c11 -c "${ROOT}/port/gdx_dev_gates.c" -I"${ROOT}/port" \
      -o "${HOST_TEST_DIR}/disk_savefile_gates.o" &&
   c++ -O1 -Wall -Wextra -std=c++17 \
      "${ROOT}/port/tests/gdx_disk_savefile_tests.cpp" "${ROOT}/port/disk_savefile.cpp" \
      "${HOST_TEST_DIR}/disk_savefile_replace.o" "${HOST_TEST_DIR}/disk_savefile_gates.o" \
      -I"${ROOT}/port/tests/stubs" -I"${ROOT}/port" \
      -o "${HOST_TEST_DIR}/disk_savefile_tests"; then
    run_host_test "gdx_disk_savefile_tests" "${HOST_TEST_DIR}/disk_savefile_tests" "${HOST_TEST_DIR}"
else
    record "test: gdx_disk_savefile_tests" FAIL "compile failed"
fi

# gdx_save_ek_layout_tests — ekship (o3dsek-plan S1): the EK title's own save may be seeded ONCE from
# the shared fzerox.sav, only when that file is PROVEN EK-layout (the shipped writer's zero bytes 2..9
# of the edit-cup/DD-cup union), and the seed reads the shared save strictly read-only.
if cc -O1 -Wall -Wextra -std=c11 \
      "${ROOT}/port/tests/gdx_save_ek_layout_tests.c" \
      "${ROOT}/port/gdx_save_ek_layout.c" \
      -I"${ROOT}/port" \
      -o "${HOST_TEST_DIR}/save_ek_layout_tests"; then
    run_host_test "gdx_save_ek_layout_tests" "${HOST_TEST_DIR}/save_ek_layout_tests" "${HOST_TEST_DIR}"
else
    record "test: gdx_save_ek_layout_tests" FAIL "compile failed"
fi

# gdx3ds_c3d_scan_host_tests — c5submit (r60 P3a): the vendored citro3d's word-at-a-time dirty
# scan (port/3ds/vendor/citro3d/gdx/c3d_gdx_scan.h) must find exactly the uniform runs stock
# citro3d 1.7.1's byte loop finds, or [native] c3d_diet is not byte-identical. Header-only.
if cc -O2 -Wall -Wextra -std=c11 \
      "${ROOT}/port/3ds/test/gdx3ds_c3d_scan_host_tests.c" \
      -I"${ROOT}/port/3ds/vendor/citro3d/gdx" \
      -o "${HOST_TEST_DIR}/c3d_scan_host_tests"; then
    run_host_test "gdx3ds_c3d_scan_host_tests" "${HOST_TEST_DIR}/c3d_scan_host_tests" "${HOST_TEST_DIR}"
else
    record "test: gdx3ds_c3d_scan_host_tests" FAIL "compile failed"
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

# gdx_disk_stream_host_tests — B11c, the SD-backed 64DD block reader (port/gdx_disk_stream.cpp)
# plus B11m, the .dcrc identity-cache sidecar (port/gdx_disk_dcrc.cpp, case 9), which needs no
# image at all -- only a scratch directory.
# against a SYNTHETIC disk image. tools/dev/mkdisk64.py builds a structurally correct
# 64,931,840-byte .ndd -- real zone/LBA geometry out of the decomp's own libleo tables, a
# parseable system area, an empty formatted MFS volume -- whose payload is deterministic seeded
# filler and which contains NO GAME CONTENT of any kind. That is the whole point: the reader,
# the LBA arithmetic, the cache, the eviction path, the CRC pass and every refusal path are
# gated here with nothing copyrighted in the tree and no dump required.
#
# The image is ~62 MB and takes ~10 s to generate, so it is CACHED in the build dir and rebuilt
# only when it or its manifest is missing. Deleting build-3ds/host-tests/diskimg/ forces it.
DISKIMG_DIR="${HOST_TEST_DIR}/diskimg"
DISKIMG="${DISKIMG_DIR}/synth.ndd"
DISKMAN="${DISKIMG}.manifest.json"
DISKTRUNC="${DISKIMG_DIR}/synth-trunc.ndd"
mkdir -p "${DISKIMG_DIR}"
DISKIMG_OK=1
if [ ! -f "${DISKIMG}" ] || [ ! -f "${DISKMAN}" ] || [ ! -f "${DISKTRUNC}" ] || \
   [ "${ROOT}/tools/dev/mkdisk64.py" -nt "${DISKMAN}" ]; then
    if python3 "${ROOT}/tools/dev/mkdisk64.py" build "${DISKIMG}" --seed 20260924 \
            > "${HOST_TEST_DIR}/mkdisk64.log" 2>&1 &&
       python3 "${ROOT}/tools/dev/mkdisk64.py" truncate "${DISKIMG}" "${DISKTRUNC}" --bytes 0x100000 \
            >> "${HOST_TEST_DIR}/mkdisk64.log" 2>&1; then
        :
    else
        DISKIMG_OK=0
        echo "FAIL: mkdisk64.py — last lines:" >&2
        tail -8 "${HOST_TEST_DIR}/mkdisk64.log" >&2
        record "test: gdx_disk_stream_host_tests" FAIL "synthetic image generation failed"
    fi
fi
if [ "${DISKIMG_OK}" -eq 1 ]; then
    if c++ -O1 -std=c++17 -Wall -Wextra -Wno-unused-parameter \
          "${ROOT}/port/tests/gdx_disk_stream_tests.cpp" \
          "${ROOT}/port/gdx_disk_stream.cpp" \
          "${ROOT}/port/gdx_block_cache.cpp" \
          "${ROOT}/port/gdx_disk_dcrc.cpp" \
          -o "${HOST_TEST_DIR}/disk_stream_tests"; then
        DS_LOG="${HOST_TEST_DIR}/gdx_disk_stream_host_tests.log"
        # argv[5] is the scratch root case 9 builds its throwaway sidecar tree under; it never
        # touches the source tree or any SD card.
        if "${HOST_TEST_DIR}/disk_stream_tests" "${DISKIMG}" "${DISKMAN}" "${DISKTRUNC}" \
                "${ROOT}/port/gen/EkAssetBindings.c" "${HOST_TEST_DIR}" > "${DS_LOG}" 2>&1; then
            echo "PASS: gdx_disk_stream_host_tests  ($(tail -1 "${DS_LOG}"))"
            record "test: gdx_disk_stream_host_tests" PASS "$(tail -1 "${DS_LOG}")"
        else
            echo "FAIL: gdx_disk_stream_host_tests — last lines:" >&2
            tail -20 "${DS_LOG}" >&2
            record "test: gdx_disk_stream_host_tests" FAIL "see ${DS_LOG#${ROOT}/}"
        fi
    else
        record "test: gdx_disk_stream_host_tests" FAIL "compile failed"
    fi
fi

# mame2ndd_host_tests — B11i, the MAME/Ares <-> .ndd converter (tools/dev/mame2ndd.py).
#
# Two halves of this conversion were already in the tree (leo64dd_python reads the MAME
# physical layout; mkdisk64.py implements the .ndd geometry from leo_tbl.c) and nobody had
# joined them as a committed, tested tool -- so every conversion this project ran was an
# ad-hoc scratch script and ekcart §8 had to file the resulting image's CRC-64 as
# unexplained. The gate is in two parts, and NEITHER needs game content:
#
#   selftest   geometry only, no files. Cross-checks the two INDEPENDENT in-tree
#              transcriptions of the block-size walk for every LBA of every disk type,
#              re-runs mkdisk64's four self-validations, and proves the MAME placement is
#              in range, injective and complete -- including on a disk with a defective
#              track, which shifts the physical mapping while the .ndd layout must not move.
#   roundtrip  .ndd -> MAME -> .ndd over the SYNTHETIC image generated above, asserted
#              byte-identical. That is the strong direction: it says the physical walk
#              addresses every LBA exactly once, at the right size, with nothing dropped,
#              duplicated or shifted. (The other direction cannot be the identity -- 8.064%
#              of a MAME image is spare surface no LBA maps to; `coverage` measures it.)
if [ "${DISKIMG_OK}" -eq 1 ]; then
    M2N_LOG="${HOST_TEST_DIR}/mame2ndd_host_tests.log"
    if python3 "${ROOT}/tools/dev/mame2ndd.py" selftest > "${M2N_LOG}" 2>&1 &&
       python3 "${ROOT}/tools/dev/mame2ndd.py" roundtrip "${DISKIMG}" >> "${M2N_LOG}" 2>&1 &&
       grep -q "^mame2ndd: roundtrip .ndd -> MAME -> .ndd  IDENTICAL$" "${M2N_LOG}"; then
        M2N_SUM="$(grep -h "checks," "${M2N_LOG}" | tr '\n' ' ')"
        echo "PASS: mame2ndd_host_tests  (${M2N_SUM})"
        record "test: mame2ndd_host_tests" PASS "${M2N_SUM}"
    else
        echo "FAIL: mame2ndd_host_tests — last lines:" >&2
        tail -20 "${M2N_LOG}" >&2
        record "test: mame2ndd_host_tests" FAIL "see ${M2N_LOG#${ROOT}/}"
    fi
else
    record "test: mame2ndd_host_tests" FAIL "synthetic image generation failed"
fi

# gdx_o2r_stream_host_tests — C4d, the byte source under the audio_table diet
# (port/gdx_o2r_stream.cpp + the shared port/gdx_block_cache.cpp that diskstream's own 526
# checks above also gate). Every archive it reads is written by the test itself, byte by byte:
# no .o2r, no ROM, no dump.
#
# What it is here to catch is not a crash. `audio_blob/audio_table` is 10.25 MB of VADPCM
# waveform reached only through AudioLoad_DmaSampleData, so a window placed a few bytes off
# changes not one pixel and no frame hash anywhere can see it -- covfinish declined to add an
# audio census family for exactly that reason. Case 6 puts 11 bytes of extra field in the LOCAL
# zip header and none in the central one, so a locator that computes the data offset from the
# central directory fails the gate instead of shipping shifted audio.
if c++ -O1 -std=c++17 -Wall -Wextra -Wno-unused-parameter \
      "${ROOT}/port/tests/gdx_o2r_stream_tests.cpp" \
      "${ROOT}/port/gdx_o2r_stream.cpp" \
      "${ROOT}/port/gdx_block_cache.cpp" \
      -o "${HOST_TEST_DIR}/o2r_stream_tests"; then
    O2R_LOG="${HOST_TEST_DIR}/gdx_o2r_stream_host_tests.log"
    if "${HOST_TEST_DIR}/o2r_stream_tests" "${HOST_TEST_DIR}" > "${O2R_LOG}" 2>&1; then
        echo "PASS: gdx_o2r_stream_host_tests  ($(tail -1 "${O2R_LOG}"))"
        record "test: gdx_o2r_stream_host_tests" PASS "$(tail -1 "${O2R_LOG}")"
    else
        echo "FAIL: gdx_o2r_stream_host_tests — last lines:" >&2
        tail -20 "${O2R_LOG}" >&2
        record "test: gdx_o2r_stream_host_tests" FAIL "see ${O2R_LOG#${ROOT}/}"
    fi
else
    record "test: gdx_o2r_stream_host_tests" FAIL "compile failed"
fi

# gdx_machines_harness — native machine-mesh differential tests (tools/native/
# machines_harness.sh: A walker vs independent decoder over the 263 body emitters,
# B colour-attribution golden table, C gdx3ds_vtx_math.h vs the interpreter
# transcription). Needs fzerox.o2r ($GDX_O2R or the Azahar SD copy); without it the
# script exits 3 and the step is recorded as PASS "skipped".
MH_LOG="${HOST_TEST_DIR}/gdx_machines_harness.log"
if "${ROOT}/tools/native/machines_harness.sh" --out "${HOST_TEST_DIR}/machines" > "${MH_LOG}" 2>&1; then
    echo "PASS: gdx_machines_harness  ($(tail -1 "${MH_LOG}"))"
    record "test: gdx_machines_harness" PASS "$(tail -1 "${MH_LOG}")"
else
    MH_RC=$?
    if [ "${MH_RC}" -eq 3 ]; then
        echo "PASS: gdx_machines_harness  (skipped: no fzerox.o2r)"
        record "test: gdx_machines_harness" PASS "skipped: no fzerox.o2r (set GDX_O2R)"
    else
        echo "FAIL: gdx_machines_harness — last lines:" >&2
        tail -8 "${MH_LOG}" >&2
        record "test: gdx_machines_harness" FAIL "see ${MH_LOG#${ROOT}/}"
    fi
fi

# gdxdev_harness_tests — the DEV HARNESS's own unit tests (tools/dev/tests/test_gdxdev.py,
# stdlib unittest only). These were never wired into CI, and that is precisely how
# `gdx-dev retro run --dry-run` shipped not dry-running and how four assertions about the
# replay corpus sat red on main unnoticed (NEXT.md B45). The harness decides what every
# run MEANS — its verdicts, its manifests, its parity claims — so a broken harness is a
# broken measurement, and it is gated here like any other code.
#
# The harness attributes every run to a TREE by its merge-base against the fork's base
# branch, and REFUSES a manifest with no branch point -- which is correct, and which means
# nineteen of these tests cannot compute their claim in a repository that has no such ref.
# A standalone clone has exactly that shape: the public repo's base branch is `main`, and a
# CI checkout of a tag may have no branch ref at all. So resolve the base from what this
# repository actually has (an explicit GDX_BASE_BRANCH always wins), and if nothing
# resolves, SKIP rather than FAIL -- a tree claim that cannot be computed is not a broken
# harness, and failing here would make the first CI run of a fresh clone red on a check
# about development bookkeeping.
if [ -z "${GDX_BASE_BRANCH:-}" ]; then
    for cand in native/main main origin/main; do
        if git -C "${ROOT}" rev-parse --verify --quiet "${cand}" >/dev/null 2>&1; then
            GDX_BASE_BRANCH="${cand}"
            break
        fi
    done
    export GDX_BASE_BRANCH
fi
GDXDEV_LOG="${HOST_TEST_DIR}/gdxdev_harness_tests.log"
if [ -z "${GDX_BASE_BRANCH:-}" ]; then
    echo "SKIP: gdxdev_harness_tests  (no base branch ref in this checkout)"
    record "test: gdxdev_harness_tests" PASS "skipped: no base branch ref (set GDX_BASE_BRANCH)"
elif python3 "${ROOT}/tools/dev/tests/test_gdxdev.py" > "${GDXDEV_LOG}" 2>&1; then
    echo "PASS: gdxdev_harness_tests  ($(tail -1 "${GDXDEV_LOG}"), base=${GDX_BASE_BRANCH})"
    record "test: gdxdev_harness_tests" PASS "$(tail -1 "${GDXDEV_LOG}") (base=${GDX_BASE_BRANCH})"
else
    echo "FAIL: gdxdev_harness_tests — last lines:" >&2
    tail -20 "${GDXDEV_LOG}" >&2
    record "test: gdxdev_harness_tests" FAIL "see ${GDXDEV_LOG#${ROOT}/}"
fi

# nrr_tol_tests — the NRR visual tolerance tool (tools/native/nrr_tol.py, n60 design 7.3) on
# synthetic BMPs: the 2d / 3d / split classes, blobs, the screen-edge exemption, missing frames.
NRRTOL_LOG="${HOST_TEST_DIR}/nrr_tol_tests.log"
if python3 "${ROOT}/tools/native/test_nrr_tol.py" > "${NRRTOL_LOG}" 2>&1; then
    echo "PASS: nrr_tol_tests  ($(tail -1 "${NRRTOL_LOG}"))"
    record "test: nrr_tol_tests" PASS "$(tail -1 "${NRRTOL_LOG}")"
else
    echo "FAIL: nrr_tol_tests — last lines:" >&2
    tail -20 "${NRRTOL_LOG}" >&2
    record "test: nrr_tol_tests" FAIL "see ${NRRTOL_LOG#${ROOT}/}"
fi

# ---------------------------------------------------------------- 3DS build --
banner "3DS build (game + harness)"
BUILD_OK=1
mkdir -p "${BUILD_3DS}"

# Generator + compiler launcher are chosen from what is actually on PATH, so the
# same script works on a devkitpro container that has neither (before its
# `apt-get install -y ninja-build ccache` lands) and on a dev box that has both.
# --no-ninja / --no-ccache force the fallbacks.
CMAKE_EXTRA=()
GEN_NAME="Unix Makefiles"
if [ "$USE_NINJA" -eq 1 ] && command -v ninja >/dev/null 2>&1; then
    CMAKE_EXTRA+=(-G Ninja)
    GEN_NAME="Ninja"
fi
CCACHE_NAME="off"
if [ "$USE_CCACHE" -eq 1 ] && command -v ccache >/dev/null 2>&1; then
    CMAKE_EXTRA+=(-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache)
    CCACHE_NAME="ccache $(ccache --version | head -1 | awk '{print $3}')"
fi
# Switching generator in place is not supported by CMake — start clean if the
# cached generator is not the one we want.
if [ -f "${BUILD_3DS}/CMakeCache.txt" ] \
   && ! grep -q "^CMAKE_GENERATOR:INTERNAL=${GEN_NAME}$" "${BUILD_3DS}/CMakeCache.txt"; then
    echo "generator changed -> wiping ${BUILD_3DS#${ROOT}/}"
    rm -rf "${BUILD_3DS}"
    mkdir -p "${BUILD_3DS}"
fi
echo "generator: ${GEN_NAME}   compiler launcher: ${CCACHE_NAME}"

# Sub-second resolution matters now: with Ninja + a warm ccache both configure and
# the incremental build land well under 1 s, which whole-second $SECONDS reports as
# "0s". Prefer bash 5's EPOCHREALTIME, then python3, then fall back to whole seconds.
now_ms() {
    if [ -n "${EPOCHREALTIME:-}" ]; then
        printf '%s' "${EPOCHREALTIME%%[.,]*}${EPOCHREALTIME#*[.,]}" | cut -c1-13
    elif command -v python3 >/dev/null 2>&1; then
        python3 -c 'import time; print(int(time.time()*1000))'
    else
        echo $(( $(date +%s) * 1000 ))
    fi
}
fmt_ms() { # start-ms -> "1.23s"
    local d=$(( $(now_ms) - $1 ))
    printf '%d.%02ds' $((d / 1000)) $(((d % 1000) / 10))
}

# ccache hit/miss delta across the full build (best-effort: field names differ
# across ccache majors, so we parse the machine-readable dump when available).
# Field names differ across ccache majors (cache_hit_direct vs direct_cache_hit,
# ...), so sum every spelling we know and fall back to 0.
ccache_stat() { # regex over stat names -> summed value, 0 if ccache absent
    command -v ccache >/dev/null 2>&1 || { echo 0; return; }
    ccache --print-stats 2>/dev/null \
        | awk -v re="$1" '$1 ~ re { n += $2 } END { print n + 0 }'
}
CC_HIT0="$(ccache_stat '^(direct_cache_hit|preprocessed_cache_hit|cache_hit_direct|cache_hit_preprocessed)$')"
CC_MISS0="$(ccache_stat '^cache_miss$')"

T0="$(now_ms)"
if ! cmake -S "${ROOT}" -B "${BUILD_3DS}" \
        "${CMAKE_EXTRA[@]:+${CMAKE_EXTRA[@]}}" \
        -DCMAKE_TOOLCHAIN_FILE="${DEVKITPRO}/cmake/3DS.cmake" \
        -DGDX_PLATFORM_3DS=ON \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
        -DCMAKE_BUILD_TYPE=Release > "${BUILD_3DS}/ci-configure.log" 2>&1; then
    T_CONFIGURE="$(fmt_ms "$T0")"
    tail -20 "${BUILD_3DS}/ci-configure.log" >&2
    record "3ds configure" FAIL "see build-3ds/ci-configure.log"
    BUILD_OK=0
else
    T_CONFIGURE="$(fmt_ms "$T0")"
    record "3ds configure" PASS "${GEN_NAME}, launcher=${CCACHE_NAME}, ${T_CONFIGURE}"

    # Advisor fix (native-review-2026-10-15.md follow-up): patch-gate.sh above already touches
    # every patched decomp file right after it applies the stack, but that happens minutes
    # before this point -- the whole host-test suite runs in between. A second, cheap touch
    # here, right where the build actually reads these files, removes any chance of ninja
    # basing its up-to-date check on a mtime this script set long before the build step runs --
    # belt and suspenders alongside patch-gate.sh's own touch_patched_files.
    git -C "${ROOT}/decomp" diff --name-only HEAD 2>/dev/null \
        | xargs -r -I{} touch -- "${ROOT}/decomp/{}"

    T0="$(now_ms)"
    if cmake --build "${BUILD_3DS}" -j"${JOBS}" > "${BUILD_3DS}/ci-build.log" 2>&1; then
        T_FULL="$(fmt_ms "$T0")"
        echo "build OK (-j${JOBS}, ${T_FULL})"
    else
        T_FULL="$(fmt_ms "$T0")"
        echo "FAIL: 3DS build — last lines:" >&2
        tail -30 "${BUILD_3DS}/ci-build.log" >&2
        BUILD_OK=0
    fi
    CC_HIT1="$(ccache_stat '^(direct_cache_hit|preprocessed_cache_hit|cache_hit_direct|cache_hit_preprocessed)$')"
    CC_MISS1="$(ccache_stat '^cache_miss$')"

    # Incremental timing: touch the biggest carve TU and rebuild. This also
    # re-proves the stale-object trap is gone — with the sources vendored there
    # is no patch step to desynchronise mtimes any more.
    T_INCR="skipped"
    if [ "$BUILD_OK" -eq 1 ] && [ "$RUN_INCREMENTAL" -eq 1 ] && [ -f "${ROOT}/${INCR_TOUCH}" ]; then
        touch "${ROOT}/${INCR_TOUCH}"
        T0="$(now_ms)"
        if cmake --build "${BUILD_3DS}" -j"${JOBS}" > "${BUILD_3DS}/ci-incr.log" 2>&1; then
            T_INCR="$(fmt_ms "$T0")"
            if ! grep -qiE 'interpreter\.(cpp|o)' "${BUILD_3DS}/ci-incr.log"; then
                echo "WARN: incremental build did not mention ${INCR_TOUCH##*/} — stale-object trap?" >&2
            fi
        else
            tail -20 "${BUILD_3DS}/ci-incr.log" >&2
            record "3ds incremental build" FAIL "see build-3ds/ci-incr.log"
            BUILD_OK=0
        fi
    fi
    echo "[time] configure=${T_CONFIGURE} full=${T_FULL} incr=${T_INCR} gen=${GEN_NAME} launcher=${CCACHE_NAME} jobs=${JOBS}"
    if [ "$CCACHE_NAME" != "off" ]; then
        echo "[ccache] hits=+$((CC_HIT1 - CC_HIT0)) misses=+$((CC_MISS1 - CC_MISS0)) over the full build"
        record "3ds build timing" PASS "configure=${T_CONFIGURE} full=${T_FULL} incr=${T_INCR} ccache +$((CC_HIT1 - CC_HIT0))h/+$((CC_MISS1 - CC_MISS0))m"
    else
        record "3ds build timing" PASS "configure=${T_CONFIGURE} full=${T_FULL} incr=${T_INCR} (no ccache)"
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
                # Launch through `open -a`, NOT the bundle's inner binary. Running
                # Contents/MacOS/azahar directly starts a process that sits idle at
                # ~0% CPU and never boots the title: no azahar_log.txt output, no
                # sdmc log.txt, no frames — so the heartbeat probe below always
                # reported "not advancing". `open -a` launches it through LaunchServices
                # the way the shared emulator protocol does, and it boots normally.
                open -a "$AZAHAR_APP" --args "$GAME_3DSX"
                echo "azahar launched (open -a ${AZAHAR_APP}); booting for ${EMU_BOOT_SECONDS}s..."
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
