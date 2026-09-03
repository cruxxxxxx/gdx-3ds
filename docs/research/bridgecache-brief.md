# BRIDGE TRANSLATION CACHE — agent brief (feat/3ds-bridgecache)

## Mission
Cut the bridge XLATE pre-pass (`[prof] br`, ~11.5 ms/crowd-frame emu, ~3400 race-DL
commands/frame ≈ 3.4 µs/cmd) to ~2-4 ms by caching `ProcessList` OUTPUT across frames for
display lists whose translation is provably frame-invariant. This is the last lever between
"holds 60 mostly" and "locked 60" on New3DS. Killswitch mandatory. Branch only — NOT merged
without the user's hardware verdict.

## Worktree
`~/code/gdx-3ds/bridgecache`, branch `feat/3ds-bridgecache` off `feat/3ds-hwaudio`
@ c586871. Submodules initialized, full 42-patch stack applied, baseline `build-3ds` built.
Build: `export DEVKITPRO=/opt/devkitpro; cmake --build build-3ds -j8`. Fresh configure needs
`-DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/3DS.cmake -DGDX_PLATFORM_3DS=ON -DCMAKE_BUILD_TYPE=Release`
and submodule init needs `git -c protocol.file.allow=always submodule update --init --recursive`.
Artifact: `build-3ds/port/3ds/G-Diffuser-3DS.3dsx`.

Rules: all work in `port/` (C++/C). Do NOT touch `decomp/` or `libultraship/` submodules unless
truly unavoidable; if you must, add a NEW patch file under `port/3ds/patches/` (pure delta,
appended to README apply list) — never edit existing patches. Commit early and often (WIP
commits are fine; squash later is optional). After EVERY milestone append a short entry to
`docs/research/bridgecache-progress.md` and commit — a relaunched agent must be able to resume
from the file alone. Keep tool output small: grep/tail/awk logs, never cat whole logs.

## Architecture you are working in (port/n64_gfx_bridge.cpp, ~11.2k lines)
Two-stage per-frame pipeline, gfx thread only:
1. **Narrow→wide** (`gWideCache`, `gdx::GfxWideCache::GetOrBuild`, ~L4480; stamp/validity
   `G2StampFor`/`G2StampStillValid` ~L1702-1735). PERSISTENT across frames. Validity is exactly
   the model you want: RDRAM-backed lists keyed on `gDmaGeneration` + `HostRangeChanged()`
   range-overlap over `gDmaDirtyRanges` (~L681-735); asset lists keyed on `gConvertEpoch`
   (high-bit namespace). `[wide] hit≈3000 reval rb=0` in-race = narrow DL bytes are stable
   frame-to-frame. STUDY this before designing anything.
2. **Wide→interpreter commands** (`N64DisplayListAdapter`, ~L3272+). `ConvertRoot` →
   `EnqueueList` (~L3360, `mLists` map keyed by wide source ptr, `ConvertedList{vector<Fast::F3DGfx>}`)
   → `ProcessList` (~L5574+, per-command walk: endian/dialect, host-pointer classification,
   placeholder/BSS-alias exceptions, segment resolution, texture pointer translation, G_DL child
   pointer = child `commands.data()`). The adapter is STACK-CONSTRUCTED PER GFX TASK (`~L9500`
   `N64DisplayListAdapter adapter(dl, dl_size, ...)`), so every list is re-walked EVERY frame.
   Only the vectors are recycled (`ConvertedListPool`, 4 MiB cap). THIS is the 11.5 ms.
   The `[prof] BR` bracket (~L9501) covers exactly ConvertRoot; `[brop]` (~L5580 + drain ~L9683)
   gives per-opcode ticks under gputrace+verbose.

Key facts:
- Host-built lists (game's per-frame GfxPool, `stride == kHostBuiltGfxStride` 16 B) are already
  wide and change every frame → NOT cacheable by content. Static asset/ROM/RDRAM lists
  (machine models, course sections, setup DLs) come through the wide cache → cacheable prize.
  Roots call leaves via G_DL; leaves hold the geometry (vtx/tri) = bulk of the commands.
- `gGdxSegmentEpoch` (L78) is a seqlock bumped ONLY on mode-transition segment reloads
  (port/decomp_port.c:1288), NOT per frame. gSegments[] values are the real per-list dependency.
- `gConvertEpoch` (L589, bumped L1276) = asset image epoch.
- Interp P0/P1 (`mInterpEnabled`, L4383) is OFF by default; it rewrites G_MTX per tick — if it is
  on, the cache must be bypassed entirely.
- Previous attempt (agent died at 0 commits) and an EARLIER attempt at PER-COMMAND validity
  REGRESSED 2× in Azahar. HARD CONSTRAINT: validity must be O(1)-ish PER LIST (a handful of
  compares), never per command.

## Design (do this, adapt only with evidence)
1. **Go/no-go census first (milestone 1, ~1 hour):** instrument `[race-dl]`-cadence receipt
   `[bcache-census] cmds_hostbuilt=N cmds_static=N lists_hostbuilt=N lists_static=N` (source
   class = wide-cache product vs host-built). If static commands < ~60% of walked commands, STOP,
   write the finding in the progress file, and instead spend the effort on per-command micro-opt
   of the host-built path (profile with [brop], attack the top opcode's resolver cost). Report.
2. **Persistent `ConvertedList` cache** (process-lifetime, gfx thread only, like `gWideCache`):
   key = wide source pointer. Entry = {commands vector (stable heap block — NEVER reallocated
   while cached; on rebuild, if the new size exceeds capacity, allocate a new block and treat
   the entry as RELOCATED), validity snapshot, lastHitFrame, children[] (child entry ptr +
   recorded child `data()`), tainted flag}.
3. **Validity snapshot, recorded DURING translation** (taint model — safe by construction):
   - `wideStamp` = the wide-cache entry's build id/stamp for this source (add a tiny build
     counter to `GfxWideCache` entries if it has none; a wide rebuild ⇒ converted entry invalid).
   - `segMask` + `segBase[16]` snapshot of every `gSegments[s]` the walk RESOLVED through;
     revalidate = for each set bit, `gSegments[s] == snapshot` (≤16 compares).
   - `convertEpoch` snapshot (`gConvertEpoch`).
   - `segmentEpoch` snapshot must be even and unchanged since build OR re-check segBases
     (segBases already cover it; keep the seqlock check for safety, it is 2 loads).
   - Any other global state read by ProcessList/its resolvers must be CLASSIFIED: covered by the
     above, or a new generation counter you add at its mutation site, or → set `tainted` for this
     list (never cached, always re-walked). Do a REAL audit: list every global touched on the
     walk in the progress file (mode-owned carves `gdx_mode_owns_segment`, `gLoadedAssetSegments`,
     placeholder/BSS alias tables, `gConvertedWideIsF3d`, texture translation memo,
     `TerminatorBoundedLimit`, diagnostic gates, stats). Diagnostics-only reads don't taint.
   - Children: on hit, each recorded child must itself be valid AND its `data()` must equal the
     recorded pointer; otherwise the parent misses (rebuild parent — parents are small).
     Memoize per-frame validity per entry so the recursive check is O(lists).
4. **Hit path:** `EnqueueList` finds a valid cached entry → returns its `data()` and does NOT
   enqueue a walk. Stats (`convertedLists`, opCounts) undercount on hits — acceptable; emit hit
   counts in the receipt instead.
5. **Bounds:** total cached bytes cap (start 6 MiB, ini-tunable `[debug] bridgecache_kb`), evict
   entries not hit for 180 frames plus LRU under pressure. Log evictions in the receipt. The 3DS
   heap is tight; the leak-hardening history in the file explains why this matters.
6. **Killswitch:** `[debug] bridgecache=0` (default 1 on this branch) read via
   `gdx3ds_config_get_int("debug","bridgecache",1)` at first use; also a DBG-menu toggle in
   `port/3ds/gdx3ds_menu.c` if cheap (follow the existing toggle pattern). Off ⇒ code path is
   byte-for-byte today's behavior.
7. **Receipts** on the `[race-dl]` cadence (same gate): `[bcache] hit=N miss=N taint=N
   inval_seg=N inval_wide=N inval_child=N evict=N bytes=N lists=N`.
8. Desktop builds: keep compiling (guard 3DS-only calls under `__3DS__`), cache may be
   3DS-only if simpler.

## Verification (emulator, before reporting)
Azahar recipe (copy from `/tmp/gpt-ab.sh`; point ROM at this worktree's .3dsx): singleton lock
`/tmp/azahar.lock` (mkdir to acquire, `touch` every ≤25 s, `rm -rf` when done); `pkill -9 -i
azahar` + `pgrep` empty before launch; SD at `~/Library/Application Support/Azahar/sdmc`,
ini at `3ds/gdiffuser/gdiffuser.ini`; autoinput `/tmp/gpt-autoinput.txt` → `sdmc/gdx-autoinput.txt`
(A-mash into a 30-machine GP race); snapshot `log.txt` to `/tmp/bcache-art/` every 25 s (other
processes may delete SD artifacts). Measurement ini: console=1 filelog=1 diag_audio=1 verbose=1
gputrace=1 bridgecache=<0|1>. Frame-aligned windows: compare `[prof] br` on the same window
frame numbers A vs B (see docs/research/crowd2-fresh-profile.md for the method).
Must show: (a) br reduction on crowd windows with hit≫miss, (b) a FULL GP (all courses —
transitions are the invalidation storm) with zero `[bcache]` anomalies, zero new log errors,
no crash; (c) SHOT BMP visual parity vs killswitch-off on matching frames (autotest dir);
(d) heap flat over the run (existing mem-census line). Also run once with bridgecache=0 to prove
the killswitch restores baseline numbers.
Then build the `.cia` too (`cmake --build build-3ds --target G-Diffuser-3DS-cia`) and stage
both artifacts + a one-paragraph HW test plan for the user in the progress file.

## Report back (final message, keep it under 60 lines)
Census numbers; design as built (what taints, what invalidates); A/B br table; full-GP
result; risks; exact commits; HW test plan. If blocked or the census says no-go, say so plainly.
