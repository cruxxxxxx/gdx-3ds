# 32-bit correctness sweep — Stream E

Scope: make the existing 32-bit code paths in `port/` real and correct for a
32-bit little-endian ARM host (3DS), without changing behavior on 64-bit hosts.

Verification tooling: `tools/probe32.sh` (added by this stream) —
`clang --target=i386-apple-macosx10.13 -fsyntax-only` for C++ TUs (genuine ILP32
against the full SDK header set) and devkitARM `arm-none-eabi-gcc -fsyntax-only
-march=armv6k -mfloat-abi=hard` (the real target) for C TUs, both with
`-Wpointer-to-int-cast -Wint-to-pointer-cast` (+ `-Wshorten-64-to-32` on clang).
64-bit regression: same probes with `--host`, plus the standalone unit tests
`gdx_gfx_pack_tests` and `gdx_gfx_convert_tests` built and run on this machine
(ALL PASS after every change).

## Fixed in this tree

| # | Issue | Where | Fix |
|---|-------|-------|-----|
| 1 | `kHostBuiltGfxStride` was `(sizeof(uintptr_t) > 4) ? 16 : 8`. WRONG on 32-bit: under `PORT` the decomp `GfxW1` is `unsigned long long` on EVERY host (gbi.h:1725), so host-built packets are 16 bytes with w1 at +8 regardless of pointer width. An 8-byte stride on 32-bit would have walked host-built and converted (`gdx::WideGfx`, also fixed 16-byte) lists at the wrong stride — every second packet's w0 read as w1. | port/n64_gfx_bridge.cpp:248 | `constexpr size_t kHostBuiltGfxStride = sizeof(gdx::WideGfx);` + static_assert == 16. All stride-equality consumers (`CommandStrideForSource`, `EstimateRawTextureCopyBytes` :~4620, `ProcessList` wide read :~5127) become correct on both widths. |
| 2 | `EstimateRawTextureCopyBytes` read w1 at `+8` whenever `stride == kHostBuiltGfxStride`; with the old 32-bit stride collapse (8==8) that read the NEXT packet's w0 on every list. Also read only `sizeof(uintptr_t)` bytes. | port/n64_gfx_bridge.cpp:~4620 | Fixed by #1; the read is now an explicit `uint64_t` (full stored word) on all hosts. |
| 3 | `ProcessList` wide-w1 recovery used `uintptr_t w1full` for both the memcpy size and the high32 host-pointer / sign-extension classification; on 32-bit that reads 4 of the 16 packet bytes and makes the `>>32` checks vacuous in a type-unsound way. | port/n64_gfx_bridge.cpp:~5127 | Split into `uint64_t w1word` (full stored word, drives high32 checks) and `uintptr_t w1full` (verbatim-pointer view). Bit-identical behavior on 64-bit. |
| 4 | `gSegments` defined `unsigned long long[16]` (port/decomp_port.c) but decomp declares `extern uintptr_t gSegments[]` (ead_demo_engine.c:204). Same layout on LP64/LLP64 only; on ILP32 the element size differs and indexing tears. | port/decomp_port.c:496, port/n64_gfx_bridge.cpp:108 | Defined/declared as `uintptr_t[16]` (with the decomp stdint.h caveat below). Bonus on 32-bit: 4-byte stores don't tear where u64 stores could. |
| 5 | `0xFFFFFFFF00000000ULL` masks and `+= 0x100000000ULL` window corrections: narrowing errors (`-Wc++11-narrowing`) at 32-bit, and semantically the "restore high half" guesses must self-disable there. | port/n64_gfx_bridge.cpp (12 sites: 4242-4519, 8126) | `kHigh32Mask` / `kLow32WindowSpan` constants that reduce to 0 on 32-bit; every guess then falls into its existing `high == 0 -> skip` branch. No 64-bit change. |
| 6 | `MakeFramebufferToken` 32-bit base `0x30000000` is exactly the 3DS linearAlloc VA range — a synthetic CIMG/ZIMG token could collide with a real allocation and false-match a pointer-identity comparison in LUS. | port/n64_gfx_bridge.cpp:1709 | Base moved to `0xE0000000` (kernel-reserved on 3DS, above the userspace split on 32-bit Linux). Tokens are opaque/never dereferenced. Segment-prefix aliasing (0x00/0x80/0xA0 forms of the same phys FB now merge) is benign: distinct framebuffers always differ in the preserved low 24 bits. 64-bit path untouched. |
| 7 | `gdx_boot_warm_asset_segments` snapshot buffer `unsigned long long[16]` — size mismatch with fixed `gSegments` on 32-bit (caught by its own static_assert). | port/n64_gfx_bridge.cpp:7830 | `uintptr_t[16]`. |
| 8 | EkAssetBindings declared the four `gdx_register_*_range` externs with `unsigned long long size` while the definitions use `size_t`. ABI-coincidental on 64-bit; on 32-bit ARM EABI a u64 argument occupies a register pair and shifts every later argument — real call corruption. | port/gen/EkAssetBindings.c:1543-1546, tools/gen_ek_assets.py:373-380 | Externs now `size_t`, generator emits the same. |
| 9 | AssetBindings pointer→integer casts spelled `(unsigned long long)` / `(unsigned int)(unsigned long long)`. Functionally OK but warn on the target and depend on decomp's (broken) 64-bit uintptr_t. | port/gen/AssetBindings.c (5 sites), tools/gen_asset_bindings.py | Cast through `(size_t)` (true pointer width from the real toolchain). Identical values on 64-bit. |
| 10 | Asset fixup byteswaps deref'd `(unsigned int*)`/`(unsigned short*)` at table offsets with no alignment guarantee — misaligned deref is UB the ARM target may not forgive (LDM/LDRD-class faults; compiler may assume alignment). | port/gen/AssetBindings.c:9173+, tools/gen_asset_bindings.py:596+ | Byte-pointer swap helpers (`gdx_bswap16_at/32_at`); compilers re-fuse them into word ops where legal. |
| 11 | `LeoGetKAdr`/`LeoGetAAdr` defined with `s32` but PR/leo.h declares `int`. ultratypes' s32 is `long` on non-LP64 → conflicting-declaration ERROR on 32-bit ARM. | port/n64_leo.c:336,372 | Definitions now use `int`, matching the decomp header exactly. |
| 12 | `%08X` + `(uint32_t)` printf args: newlib's uint32_t is `unsigned long` → format-type mismatch on devkitARM. | port/gdx_ghost_io.c:494,512,636 | Added `(unsigned int)` to the format arguments. |
| 13 | gfx_pack_tests hard-required a 64-bit host: `(void*)(u64)0x00007FF6...` constants (int→pointer truncation on 32-bit), `check(sizeof(void*)==8)`, and a `(size_t)x >> 32` (UB when size_t is 32-bit). | port/tests/gfx_pack_tests.c | Width-neutralized: constants built via `(size_t)`, high32 expectations keyed on `sizeof(void*)`, shifts widened before `>>32`. Host run: ALL PASS. |

## Verified clean (no change needed)

- **port/gdx_audio_lle.c** — Acmd w1 tokens are u32 throughout; on 32-bit the
  "low32 of a host pointer" is the whole pointer and `gdx_lle_resolve`'s
  registered-range match becomes exact. Scratch/phys arithmetic is offset-based.
  Zero own-file diagnostics on the ARM probe.
- **port/n64_audio_hle.c, n64_sched.c, gdx_segment_source.c, gdx_vi_convert.c,
  input_bridge.c, shims.c, n64_vi.c, gdx_audio_capture.c, mio0_wrap.c,
  gdx_input_script.c, gdx_dev_gates.c, EkLinkStubs.c, LinkStubs.c** — clean at
  32-bit (own-file) under the warning set.
- **port/n64_gfx_convert.{h,cpp}** — `WideGfx` is explicitly fixed 16-byte /
  u64 w1; converter tests pass on host and compile clean at ILP32.
- **gSegments low32-window resolvers** (`ResolveRegisteredHostPointer`,
  `gdx_resolve_registered_host_address`, physical-window scans) — the window
  arithmetic reduces to exact matching on 32-bit; `UINTPTR_MAX` overflow guards
  already width-safe.
- **GfxPool offset plumbing** (`gdx_gfxpool_sizeof`/`offsetof` helpers in
  decomp_port.c, consumed by gdx_interp.cpp) — all sizeof/offsetof-derived;
  since `sizeof(Gfx)` is 16 on every host the values are width-invariant.
- **LUS-facing adapter output** — `Fast::F3DGfx` words are `uintptr_t`; all
  emission goes through `MakeLusGfx(uintptr_t, uintptr_t)`.

## Deferred / filed upstream (cannot fix in this tree)

### decomp (submodule — MUST-FIX prerequisites for any 3DS build)

1. **decomp/include/libc/stdint.h — uintptr_t is u64 on every non-LP64 host.**
   The `#elif defined(PORT)` branch assumes Windows LLP64; a 32-bit host lands
   there too and gets `typedef u64 uintptr_t` + `UINTPTR_MAX 0xFFFFFFFF...`.
   Consequences today on ARM: every `(void*)(uintptr_t)x` in decomp+port C TUs
   is a 64→32 truncating cast (hundreds of warnings, e.g. the 12 remaining in
   port/decomp_port.c), and any TU that also sees newlib's real `<stdint.h>`
   gets a hard conflicting-typedef ERROR. Needed: an ILP32 branch
   (`typedef s32/u32`), or defer to the toolchain `<stdint.h>` under PORT.
   Same file's `ssize_t`/`ptrdiff_t`/`wchar_t` shadows (libc/stdlib.h,
   libc/string.h prototypes vs newlib/fortify macros) hit the same class —
   the foundation/build stream should own the header-precedence story.
2. **decomp/include/PR/ultratypes.h — s32/u32 are `long`-based on non-LP64.**
   Size-correct on ILP32 but type-distinct from `int`: C sees conflicting
   declarations wherever a header spells `int` and a definition spells `s32`
   (the two port/n64_leo.c hits are fixed here; decomp-internal ones may
   surface once TUs compile further). Also makes newlib's
   `uint32_t`(=`unsigned long`) vs game `u32` printf mismatches likely.
3. **decomp/include/PR/gbi.h — `_GFXW1_PTR(x) ((GfxW1)(x))`** warns
   (pointer→wider-integer) on every pointer-carrying gSP/gDP macro at 32-bit;
   should be `((GfxW1)(uintptr_t)(x))` once (1) is fixed.
4. **32-bit host-pointer vs segment-token ambiguity (design risk, no code fix
   here).** On 64-bit, a wide packet's w1 with high32 != 0 is proof of a real
   host pointer. On 32-bit that signal does not exist: a real host pointer
   whose top byte lands in 0x01..0x0F (3DS heap starts at 0x08000000 → top
   byte 0x08 == segment 8!) is indistinguishable from a segmented token by
   value alone. Today such pointers route through the resolver, which tries
   asset stubs/segment table before the exact registered-range match — a live
   segment 8 could shadow a genuine 0x08xxxxxx heap pointer.
   RECOMMENDATION: since GfxW1 is u64 on every host, have `_GFXW1_PTR` tag
   host pointers on 32-bit builds (e.g. `(1ull<<32) | (uintptr_t)(x)`); the
   bridge's existing high32 fast path then works unchanged (mask low32 out).
   Needs a coordinated decomp+bridge change; NOT done in this tree. Mitigation
   until then: keep the RDRAM arena and game heaps out of low-VA ranges whose
   top byte is 0x01..0x0F where the allocator allows it, or bind those checks
   earlier. Flagged for the orchestrator + stream B (memory layout).

### libultraship (submodule)

- No 32-bit blockers found in the surfaces this stream touches:
  `Fast::F3DGfx` words are `uintptr_t` (fine), `gSegments` is not a LUS symbol.
  The removed-2023 Wii U (32-bit) backend era suggests the interpreter core was
  32-bit-capable; a full LUS audit belongs to the stream that carves it.

### 3DS platform gaps observed in passing (other streams' charters)

- `port/gdx_frame_pacer.c` uses `clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME)`
  — absent from newlib/libctru; stream B needs a 3DS branch (svcSleepThread-based).
- `port/gdx_fiber_ucontext.c` needs `<ucontext.h>` — already planned:
  `gdx_fiber_3ds.c` (stream B).
- `port/gdx_audio_thread.cpp`: **no hook requests from stream C as of this
  sweep** (port/3ds/audio/STATUS.md still lists filing them as a future step);
  the file itself probes clean at 32-bit, so requests can land as pure ifdef
  hooks later.

## Not runnable here

macOS cannot link or execute 32-bit binaries, so this sweep's 32-bit evidence is
compile-level (ILP32 clang + devkitARM) plus 64-bit runtime tests for behavior
preservation. The plan's "i686 container build that boots and plays" regression
gate still needs a Linux host (orchestrator: CI candidate — probe32.sh's
`--target=i386-apple-macosx10.13` / devkitARM syntax gates are the local
stand-in).
