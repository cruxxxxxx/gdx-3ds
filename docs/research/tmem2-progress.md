# Task B — LOADBLOCK/TMEM bookkeeping ([debug] tmemfast) — progress

Worktree `~/code/gdx-3ds/tmem2`, branch `feat/3ds-tmem2` off 256e397. Brief:
`docs/research/locked60-campaign.md` (Task B). Target: hardware `F3 loadblock` 1.9-2.4 ms per
crowd frame at 144-199 calls (~12 us/call).

## Step 1 — per-phase timers (instrumentation, committed)
- Patch `port/3ds/patches/lus-tmem2-tmemfast.patch` (pure delta on the 42-patch stack; the
  submodule stack is STAGED, never committed, so `git -C libultraship diff` is this delta).
- Phases (ticks under gputrace only): der = byte counts + render-tile stride scan + metadata
  copy; path = resource path + masked lookup; cmp = GdxTmemSameContentLoad; mirr = TMEM
  memcpy; walk = StoreLoadedTexture overlap invalidation; rec = span record + entry copy;
  note = GdxNoteTmemLoadKind + dirty arms; tot = handler entry->exit.
- Receipt: `[tmem2] on= lb=calls/skips lt=calls/skips res= raw= words= mirrB= walk= rel= span=
  fast=hit/miss | us/call ... tick_us= | ms/f=` on the `[race-dl]` cadence (verbose/gputrace).
- Emulator harness: `/tmp/tmem2-art/run.sh <label> <tmemfast> [secs]` (lock protocol, ini
  merge, autoinput = A-mash 30-machine GP with SHOT drive1..5, log snapshots every 25 s).

## Step 1 result — breakdown (run `phase1`, /tmp/tmem2-art/phase1/log.txt, 2026-09-02)
Emulator, A-mash 30-machine GP, 64-frame windows. `[profop] F3` on 67 crowd windows
(nD>=100): 5.91 ms/frame at 161 calls (36.7 us/call INCLUDING the 7 svcGetSystemTick reads
the phases add, tick_us=1.31 each => ~27 us un-instrumented; hardware is ~12 us).

Per-call phase figures (early race windows, tick overhead subtracted once per phase):

| phase | us/call | share | what |
|---|---|---|---|
| walk  | 9.3  | ~50 % | StoreLoadedTexture overlap-invalidation walk: `walk == words` (100-177 iterations per load — every word of the replaced span), `span == calls` (exactly one span replaced), `rel == calls` (only its base is fresh) |
| mirr  | 2.8  | ~15 % | TMEM mirror memcpy (~800 B avg, 180 KB/frame) |
| rec   | 1.8  | ~10 % | span record: fill_n + memset + LoadedTexture copy+move (shared_ptr) |
| der   | 1.0  | ~5 %  | byte counts + render-tile stride scan + metadata copy |
| path  | 0.4  |       | resource path + masked lookup |
| cmp   | 0.2  |       | same-content compare |
| note  | 0.1  |       | note kind + dirty arms |

Census: same-content skips are only 1-2 % of in-race loads (lb=14352/358 in the start
storm, 9712/138 steady) — TMEM slots genuinely cycle between the 30 machines, so the
existing skip is not the lever. 42 % of loads are raw (non-resource) sources, 58 % o2r-backed.

GO: the walk + record are pure bookkeeping that the record step immediately rewrites for the
same words; making the same-range span replacement O(1) removes ~60 % of the handler
(hardware projection ~1.1-1.4 ms/frame of the 1.9-2.4 ms F3, well above the 0.5 ms bar).

## Step 2 — lever: O(1) same-range span replacement ([debug] tmemfast=1, default on)
- `GdxTmem2FastTeardown` (interpreter.cpp, before StoreLoadedTexture): walks the spans
  covering the new range BY SPAN (base map), bails to the legacy walk on any partial overlap,
  fresh interior view (new per-span counter `RDP::tmem_span_fresh`, bumped by
  MaterializeTmemSlot, reset at record/teardown), or a fresh base with a divergent live range.
  Otherwise releases fresh bases (the refs the walk released), drops base-indexed records; if
  the old span IS the new range, the per-word span_base fill is skipped too. One copy into the
  base slot instead of copy+move. Stale memset kept (cheap, keeps the invariant).
- Receipt: `[tmem2] ... fast=hit/miss` on the `[race-dl]` cadence. Off (tmemfast=0) = legacy
  walk, byte-identical; the counter is maintained on both paths.
- Review fix (41dec05): a base detached by the VI-fallback mutator (word count 0) is the one
  shape the legacy walk leaves inconsistent after a partial overwrite, so the fast teardown
  bails on any fresh base whose record is not exactly (base, words). Redundant zero fill
  dropped (the record's fill covers the same range).
- Clean-submodule roundtrip (scratch clone at 7bca0e2 + README list + this patch): tree
  IDENTICAL to the worktree. `.3dsx` + `.cia` built (tools/3ds-bin copied from the
  bootaudio worktree, gitignored).

## Step 3 — A/B (pending: runs fastA = tmemfast 1, fastB = tmemfast 0, same build 41dec05)
Harness: `/tmp/tmem2-art/run.sh`, verification `/tmp/tmem2-art/verify.sh` (frame-aligned
[prof]/[profop] windows via analyze.py, SHOT parity, error lines, heap).

## Step 3 results
### v1 A/B (fastA = tmemfast 1 vs fastB = 0, build 41dec05, emulator, frame-aligned)
| windows | build | dsp | F3 | E4 | F3 calls/f |
|---|---|---|---|---|---|
| CROWD nD>=100 (67) | 33.09 -> 31.75 (-1.34) | 14.80 -> 13.75 (-1.05) | 5.91 -> 4.89 (-1.02) | 7.46 -> 7.49 | 160/161 |
| MID 60-99 (8) | 19.56 -> 18.33 | 10.88 -> 10.48 | 4.05 -> 3.10 (-0.95) | 4.81 -> 4.80 | 108/107 |
| MENU nD<60 (152) | 18.06 -> 17.74 | 7.92 -> 7.59 | 2.55 -> 2.22 (-0.33) | 1.91 -> 1.91 | 58 |
Zero error lines both; heap plateau 44.47 vs 44.50 MB. Receipts: fast hit 56 % over the whole
race (misses = partial overlaps of a larger old span -> legacy walk) and hits still stepped
word-by-word over holes: walk 19.3 -> 15.1 us/call only. Not enough -> v2.
### v2 (f36daf4): live-span list. First run C showed the teardown working (fast=13817/331,
misses all `int` = fresh interior views, walk 10.65 -> 3.78 us/call) but `mirr` 4 -> 14-18 us:
the 130-byte list inserted before `uint8_t tmem[4096]` left TMEM 2-byte aligned, so every
mirror memcpy + hash word read took the unaligned path. v2.1: list declared after
`tmem_generation`, `alignas(8)` on `tmem`. Run C aborted (own instance), C/D relaunched.

### FINAL v2.1 A/B (v2C = tmemfast 1 vs v2D = 0, build f10dd88, emulator, frame-aligned)
| windows | build | dsp | F3 | E4 | F3 calls/f |
|---|---|---|---|---|---|
| CROWD nD>=100 (67) | 33.12 -> 31.75 (-1.37) | 14.77 -> 13.52 (-1.25) | 5.96 -> 4.66 (-1.31, -22 %) | 7.49 -> 7.49 | 160/161 |
| MID 60-99 (8) | 21.25 -> 19.60 (-1.65) | 10.97 -> 10.16 (-0.81) | 4.07 -> 2.66 (-1.41) | 4.79 -> 4.79 | 108/107 |
| MENU nD<60 (151) | 18.12 -> 17.26 (-0.86) | 7.94 -> 7.04 (-0.91) | 2.57 -> 1.67 (-0.90, -35 %) | 1.92 -> 1.91 | 58 |
- `[tmem2]` whole race: fast hit 82 % (misses all `int` = spans with fresh interior render
  views, ~10 % on later courses — follow-up: track interior positions), walk 19.5 -> 6.5,
  rec 4.5 -> 3.5 us/call (each incl. one 1.3 us timer read), total 40.2 -> 26.2 us/call,
  F3 ms/frame over the race 2.97 -> 2.01 (-32 %).
- Zero `[gfxfail]/[gdl-bad]/[gdl-miss]/[datafail]/[fatal]/bad_alloc/[texdiag]` in both runs.
- Heap plateau 44.50 MB (on) vs 44.64 MB (off); same 40.55 MB boot value.
- SHOT parity: title.bmp, modesel.bmp (+_scan) BYTE-IDENTICAL on vs off. drive1-4 differ
  (9-71 KB) because the A-mash race is not deterministic across runs (pack order/positions;
  same on the bridgecache agent's runs); the drive2 pair inspected side by side renders
  identically except the rival portraits in the ranking column.
- Hardware projection: F3 = 1.9-2.4 ms at ~12 us/call; the removed work (walk + record
  share, ~55-60 % of the un-instrumented handler) => ~0.7-1.0 ms/frame on crowd frames,
  ~0.5 ms on menus.
- Artifacts (build f10dd88): build-3ds/port/3ds/G-Diffuser-3DS.{3dsx,cia}; logs+shots under
  /tmp/tmem2-art/{phase1,fastA,fastB,v2C,v2D}.

## HW test plan (for the user)
ini `[debug]`: console=1 filelog=1 filelog_max_kb=4096 diag_audio=1 verbose=1 gputrace=1
tmemfast=1 (then a second run with tmemfast=0; same build). In log.txt: `[profop] F3=` per
frame on crowd windows (expect ~1.9-2.4 -> ~1.2-1.6 ms), `[prof] dsp`, and the `[tmem2]`
receipt: on=1 with fast=hit/miss hit >> miss, walk << words; on=0 shows fast=0/0 and
walk == words. Watch for any [gfxfail]/[gdl-*]/[texdiag] line and for texture corruption on
machines/track (the lever only touches TMEM span bookkeeping; TMEM bytes are unchanged).
