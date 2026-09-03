#!/bin/sh
# Stream E 32-bit syntax/semantic gate (macOS has no -m32 link support, so
# -fsyntax-only against an ILP32 target). Usage: tools/probe32.sh [--host] file...
# --host probes the 64-bit host target instead (regression check for touched files).
#
# i386-apple-darwin keeps the full SDK header set usable while making the
# compiler genuinely ILP32 (sizeof(void*) == 4), which is all the gate needs.
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TARGET="--target=i386-apple-macosx10.13"
if [ "$1" = "--host" ]; then
    TARGET=""
    shift
fi
SDK="$(xcrun --show-sdk-path)"
CVARS=$(sed -n 's/^set(\(CVAR_[A-Z_0-9]*\) "\([^"]*\)".*/-D\1="\2"/p' "$ROOT/libultraship/cmake/cvars.cmake")

DKA=/opt/devkitpro/devkitARM/bin/arm-none-eabi-gcc

STATUS=0
for f in "$@"; do
    case "$f" in
        *.c)
            # C TUs pull decomp libc headers that clash with the Darwin SDK; probe them
            # with the REAL target compiler instead (armv6k EABI, ILP32, newlib).
            # shellcheck disable=SC2086
            $DKA -fsyntax-only -x c -std=gnu11 -march=armv6k -mfloat-abi=hard \
                -Wpointer-to-int-cast -Wint-to-pointer-cast -Wall -Wno-unused \
                -fno-strict-aliasing -fwrapv -fcommon \
                -I"$ROOT/port" \
                -I"$ROOT/include" \
                -I"$ROOT/decomp" \
                -I"$ROOT/decomp/include" \
                -I"$ROOT/decomp/src" \
                -I"$ROOT/torch/lib/libmio0" \
                -DPORT=1 -DGDIFFUSER_PORT=1 -DNON_MATCHING=1 -DNON_EQUIVALENT=1 -DAVOID_UB=1 \
                -DF3DEX_GBI_2=1 -D_LANGUAGE_C=1 -DVERSION_US=1 -DEXPANSION_KIT=1 \
                "$f" || STATUS=1
            continue
            ;;
        *)   LANGOPTS="-x c++ -std=c++20" ;;
    esac
    # shellcheck disable=SC2086
    clang $TARGET -isysroot "$SDK" -fsyntax-only $LANGOPTS \
        -Wpointer-to-int-cast -Wint-to-pointer-cast -Wshorten-64-to-32 \
        -Wno-inconsistent-missing-override \
        -ferror-limit=50 \
        -I"$ROOT/port/3ds/lus_stubs" \
        -I"$ROOT/libultraship/include" \
        -I"$ROOT/libultraship/src" \
        -I"$ROOT/port" \
        -I"$ROOT/include" \
        -I"$ROOT/decomp" \
        -I"$ROOT/decomp/include" \
        -I"$ROOT/decomp/src" \
        -I"$ROOT/torch/lib/libmio0" \
        -DPORT=1 -DGDIFFUSER_PORT=1 -DNON_MATCHING=1 -DNON_EQUIVALENT=1 -DAVOID_UB=1 \
        -DF3DEX_GBI_2=1 -DVERSION_US=1 -DEXPANSION_KIT=1 \
        $CVARS \
        "$f" || STATUS=1
done
exit $STATUS
