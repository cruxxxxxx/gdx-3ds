# Patch consolidation plan — port/3ds/patches → fork commits + pin bumps

Agent R review, 2026-08-14, worktree `~/code/gdx-3ds/m1` (feat/3ds-m1).
Scope: the 9 working-tree patches in `port/3ds/patches/` applied to the `libultraship/`
(pin `7bca0e2`) and `decomp/` (pin `f7fd0fd`) submodules, and how to retire them into
proper fork commits (Zorkats/libultraship, Zorkats/fzerox) plus submodule pin bumps.

## 0. Verified baseline (drift check)

Method: `git archive` of each pinned submodule HEAD into a scratch tree, patches applied
with `git apply` in exact README order, then per-file byte comparison against the live
submodule working trees.

**Result: zero drift.** All 9 patches apply cleanly in README order, and their sum is
byte-identical to both submodule working trees (`libultraship/`: 10 modified files,
`decomp/`: 10 modified files, no untracked files, nothing outside the patch set).
`torch/` and `fzerox-expansion-kit/` working trees are clean — no action needed there.

Fork state (checked via `ls-remote` + fetch):

- Both forks have a `g-diffuser` branch; **both pins are ancestors of the `g-diffuser`
  tips**. LUS tip `59acb68` is 2 commits ahead of the pin (gui/touch/d3d11 files);
  decomp tip `3eb23d1` is 1 commit ahead (expansion-kit course-edit files).
- **None of those ahead commits touch any patch-affected file** — the patches rebase
  cleanly onto the current fork tips if desired, but see §3 for why the bump should be
  based on the current pins instead.

## 1. Per-patch audit

Apply order (canonical, from `port/3ds/patches/README.md`): lus-newlib-portability →
lus-resource-cache-cap → lus-device-path-archives → lus-3ds-settimg-low-address →
lus-texcache-content-hash-span, then decomp-ilp32 → decomp-port-segment-bzero →
decomp-3ds-dma-low-address → decomp-port-audio-specwait-yield.

### 1.1 lus-newlib-portability (128 lines, 8 files)

Pure portability sweep for devkitARM/newlib where `int32_t`/`uint32_t` are `long`-based:
explicit template arguments on `std::min`/`std::max` calls that otherwise fail exact-type
deduction (interpreter.cpp ×4, ResourceManager.cpp ×1), `int` → `int32_t` on two
virtual overrides (`AudioPlayer::Buffered` in NullAudioPlayer.h/SDLAudioPlayer.h) so the
override signature matches on all targets, an `int32_t` cast in BinaryWriter, and three
missing includes (`<unordered_map>`, `ResourceManager.h` ×2, `<cstring>`).

- **Gating:** none, and none needed — every hunk is semantics-preserving on desktop
  (explicit template args pick the same overload; the override was already `int32_t`
  in the base class).
- **Correctness:** no concerns. This is the textbook "helps ANY 32-bit target" patch.
- **Overlap:** touches `src/fast/interpreter.cpp` (hunks @829, @1971, @4230) and
  `src/ship/resource/ResourceManager.cpp` (@57) — shared with texcache/settimg and
  resource-cache-cap respectively, but all hunks are disjoint.
- **Upstream conflict risk:** low per hunk, but interpreter.cpp churns upstream.

### 1.2 lus-resource-cache-cap (182 lines, ResourceManager.h/.cpp)

Adds a byte-budget LRU eviction layer to the resource cache: per-entry accounting
(`CacheAccounting` bytes+tick), LRU tick refresh on cache hit, `EnforceCacheBudgetLocked`
evicting least-recently-used entries whose only reference is the cache (pinned entries
skipped), eviction destructors deferred until after the mutex is released (re-entrancy
safe), and public `SetCacheByteBudget`/`GetCacheByteSize`.

- **Gating: NONE — this is the one patch that genuinely changes desktop behavior.**
  `mCacheByteBudget = 24u*1024u*1024u` is the compiled-in default with no `#ifdef`.
  The in-patch comment "this patch ships only in the 3DS build" is only true by the
  convention that patches are applied per-worktree; a desktop build made from a patched
  worktree (the default CMake path `add_subdirectory(libultraship)` from the same tree)
  gets a 24 MiB cache cap. **Nothing in the port ever calls `SetCacheByteBudget`** —
  the budget exists solely as the baked default.
- **Correctness:** the mechanism itself looks sound (O(n) scan justified in-comment,
  pinning via `use_count() > 1`, deferred destruction). `use_count()` on a shared_ptr
  held under the mutex is fine here since all cache mutations hold `mMutex`.
- **Conversion requirement:** flip the default to `0` (unbounded) in the fork commit
  and add a port-side `SetCacheByteBudget(24u*1024u*1024u)` call at 3DS boot —
  natural site: `port/3ds/lus_glue/gdx3ds_context_stub.cpp` `Context::InitResourceManager`
  (line ~92, right after `mResourceManager = std::make_shared<ResourceManager>()`).
  These two changes MUST land in the same repo commit as the pin bump or the 3DS build
  silently loses its 24 MiB cap (memory-budget regression, docs/research/3ds-memory-budget.md).
- **Overlap:** ResourceManager.cpp shared with newlib (disjoint hunks).
- **Upstream conflict risk:** medium — ResourceManager evolves upstream; the mechanism
  is generic enough to eventually PR to Kenix3 (default-off).

### 1.3 lus-device-path-archives (51 lines, ArchiveManager.cpp)

Adds `ResolveArchivePath`: paths with a `<name>:/` device prefix (devkitPro/newlib
devoptab grammar, e.g. `sdmc:/3ds/...`) pass through untouched; everything else keeps
`std::filesystem::absolute()`. Fixes `absolute()` prepending cwd to device paths
(`/sdmc:/...` → fopen ENOSYS), the M1 "every o2r key missing" root cause.

- **Gating:** unconditional, but a runtime no-op on desktop — POSIX absolute paths and
  Windows drive paths never match the `name:/` shape (Windows uses `X:\` or is already
  absolute). Safe everywhere.
- **Correctness:** the colon heuristic (`colon > 0 && raw[colon+1] == '/'`) would also
  match a relative POSIX path containing a `x:/` component mid-string only if the colon
  precedes a slash — edge case is contrived; acceptable. Upstream reviewers may ask for
  a `#ifdef __unix__`-style narrowing or a devoptab probe.
- **Overlap:** none (sole ArchiveManager patch).
- **Upstream conflict risk:** low. **Genuinely upstreamable** — helps every devoptab
  platform (Switch, Wii U, Vita ports of LUS).

### 1.4 lus-3ds-settimg-low-address (20 lines, interpreter.cpp @5986)

Adds a `#elif defined(__3DS__)` branch to `gfx_set_timg_handler_rdp`'s host-pointer
guard: reject only addresses below the 3DS process image base (0x00100000) instead of
the desktop `<= 0x0FFFFFFF` heuristic, which covered the entire 3DS address space and
silently dropped every texture (M1 black-top-screen root cause).

- **Gating:** fully gated on `__3DS__` (devkitPro toolchain define). Desktop untouched.
- **Correctness:** sound for 3DS memory layout.
- **Overlap:** interpreter.cpp, hunk far from newlib/texcache hunks.
- **Upstream conflict risk:** low (small hunk), but lives in the churning file.
- **Category (c) candidate:** the cleanest long-term shape is a configurable
  "minimum valid host pointer" threshold (static member or Interpreter setter the port
  sets at boot) instead of an ifdef ladder per platform — worth proposing when merging
  `3ds-port` into `g-diffuser`, but not worth blocking consolidation on.

### 1.5 lus-texcache-content-hash-span (153 lines, interpreter.cpp)

Two logically distinct changes:
1. **Hash-span correctness/perf fix (ungated):** bounds `tmem_content_hash` to exactly
   the bytes the decode reads (RGBA16 extent mirror of `ImportTextureRgba16` incl.
   mask/CLAMP; font paths: recorded load extent) instead of `min(remaining TMEM,
   lineBytes*64)`, and folds FNV-1a 4 bytes at a time. Fixes per-frame texture-cache
   key churn (re-decode + re-swizzle + re-upload thrash).
2. **3DS telemetry hooks (`#ifdef __3DS__`):** `gdx3ds_texcache_note_import/miss/delete`
   extern-C calls (implemented in `port/3ds/gfx/gfx_citro3d.cpp:79-87`).

- **Gating:** the span fix changes desktop texture-cache keys too. It is a strict
  improvement (fewer spurious misses, tighter content identity, README argues it also
  fixes UNDERSHOOT for strides < 64B) but it IS a desktop behavior change and must be
  presented as such in the fork commit.
- **Correctness:** the RGBA16 extent mirror duplicates `ImportTextureRgba16`'s
  derivation — a maintenance hazard: if the decode extent logic changes, the hash span
  must change in lockstep or staleness detection silently degrades. Flag this in the
  commit message; ideally factor a shared helper when landing.
- **Overlap:** interpreter.cpp — its @2035 hunk sits immediately after newlib's @1971
  region inside `ImportTexture`. Biggest regeneration hazard (see §4).
- **Upstream conflict risk: HIGH** — a 58-line hunk in `ImportTexture`, the most
  actively evolved function in the most actively evolved file. Land it as its own
  commit(s) so it can be rebased/dropped independently.
- **Split on conversion:** commit A = span fix + FNV word-fold (candidate for
  `g-diffuser` proper), commit B = `__3DS__` telemetry (3ds-port; or later a generic
  callback interface — category (c)).

### 1.6 decomp-ilp32 (267 lines, 5 headers)

Prerequisites for any 32-bit build of the decomp:
- `include/libc/stdint.h`: proper ILP32 branch (`intptr_t` = `int`, matching newlib's
  `__INTPTR_TYPE__`), 32-bit `INTPTR_MIN/MAX`/`UINTPTR_MAX`. Gated
  `PORT && __SIZEOF_POINTER__ == 4`.
- `include/libc/stddef.h` / `stdlib.h`: `ptrdiff_t` = `int`, `wchar_t` = `unsigned int`
  to match GCC/newlib spellings (avoid conflicting-typedef hard errors). Same gate.
- `include/PR/os_time.h`: `osGetTime/osSetTime` → `gdx3ds_osGetTime/gdx3ds_osSetTime`
  aliases, gated `PORT && GDX_PLATFORM_3DS` (libctru symbol collision). Shim verified
  present at `port/3ds/lus_glue/gdx3ds_libultra.cpp:59`.
- `include/PR/gbi.h`: `_GFXW1_PTR` host-pointer tagging (`GDX_GFXW1_HOST_TAG` =
  0x47445831 "GDX1" in the spare high32, gated on 32-bit pointers inside the PORT-only
  64-bit-GfxW1 arm) plus rewrite of ~10 static `gs*` initializer macros onto
  `_GFX_STATIC_PTR_INIT` (word-split `GwordsStatic32` union arm on ILP32; expands to
  the original `{{ w0, _GFXW1_PTR(x) }}` shape on 64-bit hosts and N64 — textually
  different macro, identical expansion).
- **Gating:** effectively complete. N64 matching builds and 64-bit hosts see identical
  expansions; the union member addition is `PORT && ptr==4` gated.
- **Correctness:** tag constant verified in sync with `port/n64_gfx_bridge.cpp:286`
  (`kGfxW1HostTag32 = 0x47445831ull`); the bridge comment at :284-285 references the
  patch file by name (update on conversion).
- **Overlap:** none within decomp patches.
- **Upstream conflict risk:** medium-low — gbi.h is large but stable; libc headers are
  near-frozen. The libc-header hunks are upstreamable to the parent decomp project
  (helps any ILP32 port); the gbi.h tag machinery and os_time aliasing are
  G-Diffuser/3DS-specific (they reference GDX symbols and the port bridge contract).

### 1.7 decomp-port-segment-bzero (36 lines, sys_main.c + rom/disk_drive.c)

`#ifndef PORT` around the two remaining raw `bzero(SEGMENT_BSS_START(x),
SEGMENT_BSS_SIZE(x))` calls (leo in `Main_ThreadEntry`, ovl_i11 in `func_8007515C`).
Under PORT the BSS markers are 1-byte LinkStubs; END can land below START, the size
wraps to ~4 GB, and the bzero corrupts host .bss then faults (M1 black-screen hang).

- **Gating:** `#ifndef PORT` — formally changes all PORT desktop builds, but these call
  sites only compile with `EXPANSION_KIT` off, which desktop never builds; and where it
  would compile, the fix is correct for every PORT host (host loader zero-fills BSS).
- **Correctness/overlap:** clean, matches the pattern already used by every other
  segment-BSS consumer. No overlaps. Conflict risk low.
- **Destination:** PORT-generic — belongs on the fork's `g-diffuser` line eventually,
  not a 3DS-only branch.

### 1.8 decomp-3ds-dma-low-address (31 lines, sys/dma.c)

Inside `Dma_PortRamPointer`'s "address < 16MB ⇒ raw RDRAM offset" branch, adds a
`GDX_PLATFORM_3DS`-gated preference: for addresses ≥ 0x00100000, first try
`gdx_resolve_registered_host_address` (the EXE image is registered at boot), restoring
64-bit semantics where real pointers resolve through the registry. Fixes EXE globals
below 16MB (gCourseCtx ~0x0038xxxx) being rerouted into the RDRAM shadow — the
mode+difficulty race-entry freeze.

- **Gating:** fully gated on `GDX_PLATFORM_3DS` (defined only by the 3DS targets).
- **Correctness:** sound; unclaimed ≥0x00100000 addresses still fall through to the
  RDRAM-offset path, sub-0x00100000 stays unambiguous (unmapped on 3DS). Note it is
  really an "ILP32 host" fix wearing a 3DS badge — any future 32-bit target with a
  low image base needs the same branch; fine to generalize later.
- **Overlap:** none. Conflict risk low (dma.c is port-owned in the fork).

### 1.9 decomp-port-audio-specwait-yield (50 lines, audio/{rom,disk}/external.c)

`#ifdef PORT` branch in both `Audio_SetSpec` variants: the raw
`do {} while (!AudioThread_ResetComplete())` spin yields 1 ms per iteration via
`gdx_audio_specwait_yield` (`port/n64_sched.c:899`, also feeds the watchdog `spec=`
counter). Fixes permanent same-priority starvation of the audio thread on the 3DS
kernel/Azahar.

- **Gating:** `#ifdef PORT` — desktop PORT builds get the polite wait too; behaviorally
  benign (preemptive schedulers never needed the yield) and README-acknowledged.
- **Correctness:** the `audio/rom` variant's brace surgery (`}` placement around the
  `#else`) is easy to get wrong on rebase — the patch is correct as-is, but treat this
  hunk carefully in conflicts. `gdx_audio_specwait_yield` must exist in every PORT
  link (it lives in shared `port/n64_sched.c`, so desktop links it too — verified).
- **Overlap:** none. Conflict risk low-medium (fork actively develops audio files).
- **Destination:** PORT-generic → fork `g-diffuser` line.

## 2. Classification table

| Patch | Gated? | Desktop effect | Destination | Upstream-conflict risk |
|---|---|---|---|---|
| lus-newlib-portability | No (semantics-preserving) | None | (a) fork `3ds-port` → PR to Kenix3/libultraship | Low |
| lus-resource-cache-cap | **No — 24 MiB default is unconditional** | **Caps desktop cache at 24 MiB** | (b) fork, default flipped to 0 + port-side call; (a) later, default-off | Medium |
| lus-device-path-archives | No (runtime no-op off-device) | None | (a) fork → PR to Kenix3 (all devoptab platforms) | Low |
| lus-3ds-settimg-low-address | Yes (`__3DS__`) | None | (b) fork `3ds-port`; (c) long-term: configurable min-host-pointer threshold | Low (small) / file churns |
| lus-texcache-content-hash-span | Span fix ungated; telemetry `__3DS__` | Changes desktop cache keys (improvement, but a change) | Split: span fix (b, `g-diffuser`-worthy); telemetry (b)/(c) callback iface | **High** |
| decomp-ilp32 | Yes (`PORT && ptr==4`; os_time `PORT && GDX_PLATFORM_3DS`; macro rewrite expansion-identical) | None | libc headers (a) upstream decomp; gbi.h tag + os_time (b) `3ds-port` | Medium-low |
| decomp-port-segment-bzero | Yes (`#ifndef PORT`) | None in practice (EK-off only) | (b) fork `g-diffuser` line (PORT-generic) | Low |
| decomp-3ds-dma-low-address | Yes (`GDX_PLATFORM_3DS`) | None | (b) fork `3ds-port` | Low |
| decomp-port-audio-specwait-yield | Yes (`#ifdef PORT`) | Benign 1 ms-yield wait | (b) fork `g-diffuser` line (PORT-generic) | Low-medium |

(a) = upstreamable beyond the Zorkats forks; (b) = fork-branch material; (c) = better
expressed as a port-side hook/config eventually.

## 3. Conversion plan

### Branching and commit granularity

Base each fork branch on the **current pin**, not the `g-diffuser` tip, so the pin bump
introduces ONLY patch content (the 2 LUS + 1 decomp tip commits ride in via a separate,
later bump with their own test pass):

- `Zorkats/libultraship` branch **`3ds-port`** from `7bca0e2`, 6 commits in README
  order:
  1. `fix(port): newlib/devkitARM portability (int32 templates, override widths, includes)`
  2. `feat(resource): byte-budget LRU cache eviction (default off)` — **default flipped
     to `mCacheByteBudget = 0`**
  3. `fix(archive): pass devoptab device paths through untouched`
  4. `fix(fast): 3DS host-pointer floor for SETTIMG low-address guard`
  5. `fix(fast): bound tmem_content_hash to the decoded extent; fold FNV by word`
  6. `feat(fast): 3DS texture-cache telemetry hooks`
- `Zorkats/fzerox` branch **`3ds-port`** from `f7fd0fd`, 4 commits:
  1. `fix(include): ILP32 libc types, gbi host-pointer tag, 3DS osGetTime alias`
     (the ilp32 patch is one logical prerequisite bundle; splitting libc/gbi/os_time
     into 3 commits is acceptable if upstreaming the libc part is imminent)
  2. `fix(port): guard segment-BSS bzeros that are stub-marker garbage under PORT`
  3. `fix(port): prefer registered host ranges for low addresses on 3DS DMA`
  4. `fix(port): yield in Audio_SetSpec reset wait under PORT`

One commit per patch (texcache split into two) — each patch is already a coherent,
individually documented unit with its own README paragraph; reuse those paragraphs as
commit bodies verbatim, including the docs/research cross-references. Do NOT squash
across patches: independent revertability is the point (especially texcache, the
highest-conflict item).

Mechanically: `git -C libultraship switch -c 3ds-port` in a scratch clone (or this
worktree after sign-off), `git apply` + `git commit` per patch, amend the cache-cap
default, push `3ds-port` to both forks. Tag the old pins (`pre-3ds-port-consolidation`)
for rollback ergonomics.

### The one repo commit (on feat/3ds-m1 or the integration branch)

A single G-Diffuser commit containing, atomically:
1. Submodule gitlink bumps: `libultraship` → `3ds-port` tip, `decomp` → `3ds-port` tip.
   (`.gitmodules` URLs unchanged; optionally add `branch = 3ds-port` for
   `submodule update --remote` ergonomics.)
2. `Context::InitResourceManager` (`port/3ds/lus_glue/gdx3ds_context_stub.cpp`, after
   the `make_shared<ResourceManager>()` at :92): call
   `mResourceManager->SetCacheByteBudget(24u * 1024u * 1024u);` — compensates the
   flipped default. **This is the single easiest thing to forget.**
3. Delete `port/3ds/patches/` (9 patches + README).
4. Update every reference to the patch files:
   - `port/3ds/gfx/CMakeLists.txt:16` (prereq comment)
   - `port/3ds/harness/README.md:28` (apply instructions)
   - `port/3ds/lus_stubs/README.md:9`
   - `port/n64_gfx_bridge.cpp:285` (comment naming decomp-ilp32.patch → point at the
     fork commit / decomp gbi.h directly)
   - `port/3ds/gfx/STATUS.md:130` and `docs/research/m1-boot-debug.md` /
     `m1-link-status.md` mentions (historical docs may keep the names; add a note that
     patches landed as fork commits).
5. Verification gate before committing: `git -C libultraship diff --quiet && git -C
   decomp diff --quiet` (working trees must now be pristine at the new pins), then a
   3DS build AND a desktop build from the same tree.

### Worktree migration (16 worktrees listed, ~8 active streams)

Patches and pins travel together inside each branch, so worktrees stay self-consistent
until they rebase past the bump. Nothing needs to happen fleet-wide on day one.

Per worktree, when it takes the bump (script this):

```sh
# 0. SAFETY: capture any submodule WIP beyond the canonical 9 patches
git -C <wt>/libultraship diff > /tmp/<wt>-lus.wip   # compare against sum-of-patches!
git -C <wt>/decomp       diff > /tmp/<wt>-decomp.wip
# 1. drop the applied-patch state (ONLY in the same operation as taking the bump)
git -C <wt>/libultraship checkout -- . && git -C <wt>/decomp checkout -- .
# 2. take the bump (rebase/merge the branch that contains it)
git -C <wt> rebase <branch-with-bump>
# 3. move submodules to the new pins
git -C <wt> submodule update --init libultraship decomp
# 4. verify
git -C <wt>/libultraship diff --quiet && git -C <wt>/decomp diff --quiet && echo clean
```

Step 0 is not optional: stream worktrees (texcache, gfx, audio…) may carry submodule
WIP beyond the 9 canonical patches — I verified only m1 matches the patch sum exactly.
Diff each worktree's submodule state against the patch sum first; any excess is WIP that
must be preserved (stash the .wip file, re-apply after step 3).

Rollout order: **fork branches pushed → m1 (this worktree, where the trees already match
the patch sum exactly) → integration (`feat/3ds`) → stream worktrees as each rebases onto
integration.** Worktrees that never rebase past the bump keep working unchanged.

### Risk list

| Risk | Consequence | Mitigation |
|---|---|---|
| `git submodule update` against dirty (patched) submodule trees | Refuses or half-migrates; confusing mixed state | Script enforces checkout-clean before update, `diff --quiet` after |
| Forgetting the port-side `SetCacheByteBudget` call when flipping the default | 3DS cache unbounded → OOM regressions that look unrelated | Same-commit rule (§3 item 2); grep gate in review |
| Basing `3ds-port` on fork tips instead of pins | Bump smuggles 3 unrelated fork commits into M1 testing | Base on `7bca0e2`/`f7fd0fd`; fast-forward check before push |
| Worktree submodule WIP beyond the 9 patches wiped by `checkout -- .` | Silent loss of stream experiments | Mandatory step-0 WIP capture + compare vs patch sum |
| Stale docs/scripts still instructing `git apply` post-bump | Double-apply attempts (git apply fails loudly — annoying, not corrupting) | Reference sweep in §3 item 4 |
| Regenerating an interpreter.cpp patch pre-conversion | The 3 patches sharing the file are hand-partitioned; naive `git diff` merges them | Don't regenerate singly; if needed, use selective staging/interdiff (see §4) |
| Fork force-push/rewrite of `3ds-port` after pinning | Pins dangle for fresh clones | Treat `3ds-port` as append-only; tag pinned SHAs in the forks |

### Rollback story

- Repo: `git revert <bump-commit>` restores old gitlinks AND the patch directory in one
  step (they are one commit).
- Per worktree already migrated: `git -C libultraship checkout -- . && git -C decomp
  checkout -- .` (no-op, trees are clean at new pins) → `git submodule update` back to
  old pins → re-apply patches per the restored README.
- Fork branches stay pushed regardless — harmless, and required anyway for retrying.
- The `pre-3ds-port-consolidation` tags on the old pins make "where were we" trivial.

## 4. Hygiene findings

1. **No content drift** (§0): patch sum ≡ working trees, both submodules, verified
   byte-for-byte today. The README apply list names all 9 patches in a working order.
2. **README gap:** `lus-resource-cache-cap` is in the apply list but has **no
   description section** in `port/3ds/patches/README.md` (8 sections for 9 patches).
   Its documentation lives in `port/3ds/gfx/STATUS.md:130`. Add the section (or fold
   STATUS.md's paragraph in) — or skip it if conversion happens promptly, since the
   README dies in the same commit.
3. **Headerless patches:** `lus-resource-cache-cap`, `lus-3ds-settimg-low-address`, and
   `decomp-3ds-dma-low-address` are bare `---/+++` unified diffs without
   `diff --git`/`index` lines (the other six are full git diffs). They apply fine but
   lack blob provenance; regenerate via `git diff` for uniformity if patches live on.
4. **Misleading in-patch comment:** resource-cache-cap's "this patch ships only in the
   3DS build" is enforced by nothing (§1.2) — the 24 MiB default compiles into desktop
   builds made from a patched worktree. Fix at conversion (default 0 + port call).
5. **interpreter.cpp is triple-patched** (newlib @829/@1971/@4230, settimg @5986,
   texcache @546–@2105, with texcache's ImportTexture hunk adjacent to newlib's): a
   plain `git -C libultraship diff -- src/fast/interpreter.cpp` yields the merged hunks
   of three patches. Any future regeneration must partition by hand or via selective
   staging — the strongest argument for converting to fork commits soon.
6. **Cross-references to update at conversion:** `port/3ds/gfx/CMakeLists.txt:16`,
   `port/3ds/harness/README.md:28`, `port/3ds/lus_stubs/README.md:9`,
   `port/n64_gfx_bridge.cpp:285`, `port/3ds/gfx/STATUS.md:130`, plus historical
   mentions in `docs/research/m1-boot-debug.md` and `m1-link-status.md`.
7. **Tag-constant sync verified:** `GDX_GFXW1_HOST_TAG` (patch) == `kGfxW1HostTag32`
   (`port/n64_gfx_bridge.cpp:286`) == 0x47445831. Port-side symbols referenced by
   patches all exist (`gdx_audio_specwait_yield` n64_sched.c:899, telemetry hooks
   gfx_citro3d.cpp:79-87, `gdx3ds_osGetTime` gdx3ds_libultra.cpp:59).
8. **No CI involvement:** `.github/workflows/{release-linux,release-windows}.yml` never
   apply patches (desktop builds use pristine submodules on main). No CI edits needed.

## 5. Recommended execution order

1. (Optional, cheap) README hygiene: add the resource-cache-cap section — skip if step 2
   starts this week.
2. Push fork branches: `3ds-port` on both forks, based on current pins, commits per §3
   (cache-cap default flipped in its commit). Tag old pins.
3. Land the single atomic repo commit on `feat/3ds-m1`: pin bumps + `SetCacheByteBudget`
   port call + delete `port/3ds/patches/` + reference sweep. Verify both build flavors
   and pristine submodule trees.
4. Migrate m1 (trivially clean), then integration, then stream worktrees as they rebase
   — always with the step-0 WIP capture.
5. Later, separately: merge fork `g-diffuser` tips (the +2/+1 commits) via a second
   pin bump with its own test pass; open the two genuinely upstreamable PRs
   (newlib-portability, device-path-archives → Kenix3; ilp32 libc headers → parent
   decomp) from the fork branches.
