# Bridge translation cache — progress log (feat/3ds-bridgecache)

Brief: `docs/research/bridgecache-brief.md`. Worktree `~/code/gdx-3ds/bridgecache`.
Build: `export DEVKITPRO=/opt/devkitpro; cmake --build build-3ds -j8`.
Emulator runs: scratch script `bc-run.sh TAG BRIDGECACHE [DUR]` (env BROP=1 arms `[brop]`),
artifacts under `/tmp/bcache-art/<TAG>-log.txt` (+ `-autotest/`, `-azahar.txt`).
Ini keys the runs set: console=1 filelog=1 diag_audio=1 verbose=1 gputrace=1 bridgecache=N
filelog_max_kb=4096 brop=N. NOTE: the default filelog cap (256 KB) truncates a race run at
~frame 10400; always set filelog_max_kb.

## M1 — go/no-go census (2026-09-01) — VERDICT: NO-GO for a static-list output cache

Instrument: `[bcache-census] cmds_hostbuilt= cmds_static= lists_hostbuilt= lists_static=`
on the [race-dl] cadence (64-race-frame window totals; source class = gWideCache product
vs host-built 16 B-stride GfxPool list). Commit 9795fbb.

Run `census` (Azahar, 30-machine GP, A-mash autoinput, frames 0-10369):
- whole run: cmds host=13.60 M, static=2.61 M -> **static 16.1 %** of walked commands
- in-race crowd windows (host>150k/window, n=12): typical
  `cmds_hostbuilt=186738 cmds_static=27328 lists_hostbuilt=2944 lists_static=1792`
  -> **static 12.8 %** of commands (427/frame of ~3345/frame), 28 static lists/frame
  (avg 15 cmds each) vs 46 host-built lists/frame (avg 63 cmds each).
- [wide] hit~3500/window reval~0 rb=0 confirms the static lists are byte-stable, but they are
  SMALL: F-Zero X builds its track/machine geometry into the per-frame GfxPool in C code, so
  the vtx/tri bulk is host-built and changes every frame.
- `[prof] br` on the frame-aligned crowd windows: frame 3393 br=11.49, frame 9985 br=11.70
  (matches crowd2-fresh-profile.md), i.e. ~3.4 us/cmd.

Ceiling of a perfect static-list cache = ~13-16 % of br (~1.5 ms/frame) — far under the
brief's 60 % threshold. Per the brief: STOP the cache, spend the effort on per-command
micro-optimisation of the host-built ProcessList path (attack the top [brop] opcode).

## Global-state audit of ProcessList + resolvers (kept for the record / future cache work)

Output-affecting reads on a wide-product (static) walk, and what would cover them:
- `gSegments[]` — read by every segmented resolve and by G_MOVEWORD SEGMENT emit; WRITTEN
  in-walk by G_MOVEWORD SEGMENT (L~7000/7277) -> a list containing gSPSegment must be tainted.
  Writers: decomp_port.c Segment_SetAddress (mode loads, epoch-bracketed), bridge L8822
  venue seg 0xA (course load, NOT bracketed), first-load claims (L1065/1269/2004).
- `gGdxSegmentEpoch` seqlock — mode reloads (decomp_port.c:1288/1340); walk drops SETTIMG /
  emits stale MOVEWORD when unstable -> a build that saw an unstable epoch must not be cached.
- `gConvertEpoch` — asset image loads/registration (L1276, L7608), lazily triggered FROM the
  walk (EnsureAssetSegmentForSymbol/ResolveGeneratedAssetStub) -> snapshot before+after.
- `gDmaGeneration`/`gDmaDirtyRanges` — RDRAM writes (already the wide-cache stamp).
- `gConvertedWideIsF3d` — per-wide-buffer F3D dialect; wiped at >8192 entries; store isF3d
  in the entry instead.
- gWideCache entry rebuild/age-eviction (BeginFrame, >512 entries idle >600 frames) changes
  the wide data() pointer = the cache key -> needs a per-entry build id.
- `gPersistentAllocations` — vtx/mtx copies freed EVERY frame (L10622); only reachable with
  isBig=true, which is never the case for wide products (converter output is host-endian).
- `gRawTextureCopies` (MakePersistentRawTextureCopy via TranslateTexturePointer) — entry
  buffer REPLACED on growth, contents refreshed in place; detection via gDmaGeneration,
  `gNativeRgba16Generation` (native RGBA16 ranges, mutable per transition), memcmp for
  unregistered sources, `sVenueBuildingTexBase` (course load) -> any list whose SETTIMG went
  through a raw copy would have to be tainted or keyed on the copy pointer + generation.
- Workshop texture packs: `gdx_workshop_texture_packs_enabled()` CVar read live +
  `GdxWorkshopLookupOverridePath` pack-epoch cache + `GDiffuser_LookupLoadedAssetKey`
  (null->key as LUS resources load, no counter) -> taint when the override path is taken.
- Interp P0/P1/camera (`mInterpEnabled`, `mInterpCamera`): G_VTX/G_MTX/viewport rerouted to
  per-tick scratch slots when the RESOLVED pointer is in the seg-1 GfxPool span; reachable
  for products (isBig=false) -> bypass the cache entirely when interp is on.
- Append-only, never freed (safe): gHostRanges, gRawN64Ranges, gHostN64CommandRanges,
  gHostWideCommandRanges, gHostPointerStubs, gN64AddressRanges, gLoadedAssetSegments,
  gLoadedAssetBuffers, gN64Framebuffers; static const asset tables; boot constants
  (gdx_rdram, module range, BSS alias latch, dev gates). gF3DAssetRanges is never populated.
- Diagnostic-only (no taint): gGdxRaceActive, gGameMode, sSetupDl*, sTRect*, sSettimg*,
  sSeg4Probes, sDiag*, gDiagTransitionCapture*, gGdxCountdownProbe*, all gdx_dev_gate DIAG_*
  reads, Crc32/memcpy payload peeks used only for logging.
- OS page map (ReadableByteLimit/IsReadableAddress): stable for RDRAM/segment/module memory.

## M2 — pivot: per-list micro-opt of the bridge pre-pass ([debug] brfast, default 1)

[brop] attribution (run `brop`, 64-race-frame window totals, timers ON so relative only):
`enq=67.7ms/3520 DE=140.9/3456 FD=46.3/4160 E7=13.6/8992 F5=13.0/8640 06=11.0/7168 01=10.8/2944`
=> G_DL ~41 us/call (~5.7 ms/frame, half of br), of which EnqueueList ~19 us; SETTIMG ~11 us;
E7 PIPESYNC 1.5 us = per-command floor incl. the two svc tick reads of the timer itself.

Root cause of the G_DL cost (read of the code, confirmed by the fast-path numbers below): each
sub-list was classified/scanned 3-4 times per frame — KnownCommandLimit x3, LooksLikeDisplayList
x2, TerminatorBoundedLimit, DisplayListUsesF3D (three opcode walks to G_ENDDL), plus
CommandStrideForSource / CommandSourceIsBigEndian ~6x — and every segmented token resolve walked
the ~600-row EK overlay table (gN64AddressRanges) newest-first before missing into the segment
table.

Built (commits ed3e4cf + follow-ups), all EXACT re-implementations behind `GdxBrFastOn()`
(`[debug] brfast=0` on 3DS / `GDX_BRFAST=0` desktop restores the legacy path byte-for-byte):
- `ListFacts` per-adapter (per gfx task) per-list memo: stride, endianness, KnownCommandLimit,
  one recorded opcode scan (first G_ENDDL, first non-DL opcode) answering LooksLikeDisplayList /
  TerminatorBoundedLimit / DisplayListUsesF3D for any limit; the shapes the record cannot answer
  (bad opcode before the terminator) fall back to the legacy function. Reset per adapter.
- `gN64RangeMemo`: (raw, requiredBytes) -> EK row index, keyed by the append-only row count.
- `gPlaceholderMemo`: IsAssetPlaceholderPointer(low32) over the const generated tables.
- `gRawTextureCopyIndex`: hash index for the append-only gRawTextureCopies scan per SETTIMG.
- TEMP: `[brop]` line now carries `fd xl/key/copy` and `de src/val` phase sub-timers (strip).
Killswitch ini key for the runs: `brfast` (bc-run.sh's 2nd arg). `[bcache-census]` kept.

Run `fastA` (brfast=1, brop=0, full GP scripted race, 255 [prof] windows, frame 16321):
frame-aligned crowd windows vs the census baseline: 3393 11.49 -> 7.82, 9985 11.70 -> 8.01,
9793 10.41 -> 6.96; mean over the 30 windows with baseline br>5: 8.84 -> 6.28 ms (-29 %).
Zero [gfxfail]/[gdl-bad]/[gdl-miss]/[datafail] lines. Heap: [watchdog] heapUsed 40.55 MB
(beat 1) -> 44.72 MB (beat 84), same shape as the baseline run (see comparison below).
[watchdog] heapUsed at aligned beats (census baseline vs fastA): beat1 40.55/40.55 MB,
beat17 42.27/42.43, beat25 44.19/44.22, beat41 44.23/44.25, beat55 44.76/44.48 -> flat, no
brfast-specific growth (the memos are fixed-size arrays plus one hash index of the append-only
raw-copy table).

## M3 — brfast round 2: attribution + fixes (2026-09-01 evening)

Phase timers (fastBrop2/3/4, timers ON => relative only; each svc tick read ~1.3 us):
per G_DL: resolve-source ~1 us, validate ~6, EnqueueList ~7; per SETTIMG (~234/frame!) ~6 us
real, branches per crowd window: o2r=8517 host=4044 raw=2519 (o2r-filepath emits dominate).
Table sizes in-race: gHostRanges=35 gRawN64Ranges=13 gHostN64CommandRanges=140
gHostWideCommandRanges=0 (the GfxPool is NOT a wide-command range -> every host-built list
classification missed through all three tables) gN64AddressRanges=0 (EK memo moot on 3DS)
gLoadedAssetSegments=13 gRawTextureCopies=175 gNativeRgba16Ranges=6.

Fixes (all behind brfast, exact): TryResolveAddress memo (commit 8181fac: tables version
`gGdxResolveTablesVersion` bumped at every bridge-side table append / gSegments write, plus a
per-adapter FNV snapshot of gSegments[] + `gdx_mode_segment9_state()` + segment epoch;
same-value in-walk gSPSegment writes do not bump), ResolveWideAssetStubPointer memo,
IsAssetPlaceholderPointer memo, gRawTextureCopies hash index, per-list dialect memo,
byte-stride opcode validation scan + IsLikelyDisplayListOpcode table, stride-aware
KnownCommandLimit, range-class memo for CommandStrideForSource/CommandSourceIsBigEndian,
[tex-census] seen-table scans gated behind the verbose gate, SETTIMG copy-size estimator
reads opcode bytes only. Legacy-resolve guessing / seg-9 diag gates bypass the memos.

Result (run fastA2, brop=0, brfast=1, build 88c8ac3) vs census baseline, frame-aligned:
3393 11.49 -> 5.04 ms, 9985 11.70 -> 5.25, 9793 10.41 -> 4.51; crowd mean (30 windows,
base br>5) 8.84 -> 4.21 ms (-52 %). Zero error lines. Heap aligned beats 1/21/41/56:
40.55/42.68/44.23/44.79 (base) vs 40.55/43.65/44.26/44.65 MB (fast) -> flat.
Killswitch (run fastB, brfast=0, build ed3e4cf): 3393 11.45, 9985 11.65, 9793 10.38 = baseline.
SHOT parity: run-to-run emulator variance already makes the control pair (census vs fastB,
same code path) differ by ~7 % of bytes; fastB vs fastA2 differ by the same ~7 % (drive3
census diverged into a different scene). Visual check of drive1/drive4 PNGs below.
Temporary [brop] phase timers/branch counters are left in, gated by [debug] brop=1.

## M4 — verification on the final build (88c8ac3 + docs), .cia, full-GP

- Killswitch on the FINAL build (run finalB, brfast=0): 3393 11.20, 9985 11.39, 9793 10.22
  (baseline 11.49/11.70/10.41; crowd mean 8.56 vs 8.84 -- run variance; the only non-gated
  change is IsLikelyDisplayListOpcode as a 256-entry table, result-identical).
- brfast=1 on the final build (run fullgp, 1500 s, 58.9k frames, 919 windows): 3393 4.89,
  9985 5.11, 9793 4.40. Zero gfxfail/gdl-bad/gdl-miss/datafail/bad_alloc/seg-epoch/
  legacy-resolve/texdiag lines, no crash, heap 40.55 MB (beat 1) -> 44.88 MB (beat 299,
  frame 58836) = the same plateau every 420 s run reaches by beat ~50.
- SHOT parity finalB (off) vs fullgp (on), same build: drive1..4 differ by 14-19k of 288k
  bytes = the control pair's run-to-run variance (rival portrait order / pack positions);
  side-by-side PNGs of drive1/drive4 (fastB vs fastA2) are visually identical.
- CAVEAT: /tmp/gpt-autoinput.txt stops at frame 16000 and only taps A every 300 frames, so
  the 1500 s run idled after the first race (br=1.46 from frame ~20000 on) -- it is NOT a
  full GP. A held-A script (`gp-autoinput.txt`: "tick A 2980" every 3000 ticks to 66000,
  SHOT gpN every 9000) drives run fullgp2 for the course-transition storm.
- .cia: makerom/bannertool copied from ../hardware/tools/3ds-bin (gitignored tools/3ds-bin),
  reconfigure picks them up; `cmake --build build-3ds --target G-Diffuser-3DS-cia` ->
  build-3ds/port/3ds/G-Diffuser-3DS.cia (1.72 MB) next to the .3dsx (2.95 MB).

Full-GP caveat (final): the held-A script (run fullgp2) ALSO ends the first race in an
explosion (energy empty, the same "drive5" explosion the tap-A script reaches at frame
15150) and the game then sits on the retire/results screens (br=1.44, draws~164 from frame
~16k on) -- A-only autoinput cannot steer, so a multi-course GP is NOT reachable in the
emulator with the available scripting. Exercised: boot -> title -> menus -> machine select
-> course load -> 30-machine race -> retire/results, twice for 1500 s, zero anomalies.
The course-transition invalidation storm (segment reloads, gConvertEpoch bumps) is covered
only by the boot -> race transition and by the memo validity design (tables version +
per-adapter gSegments/seg-9/epoch snapshot); HW verdict needed for the rest.

## HW test plan (for the user)
Artifacts: `build-3ds/port/3ds/G-Diffuser-3DS.3dsx` and `.cia` (copies in /tmp/bcache-art/).
1. Copy the .3dsx (or install the .cia) and an ini with `[debug] gputrace=1 filelog=1
   filelog_max_kb=4096 brfast=1`; run a full GP cup (all 6 courses), then repeat with
   `brfast=0`. Compare `[prof] br` on windows with draws>120 -- expect ~11.5 -> ~5 ms
   (emulator figures; HW should scale similarly since the savings are table scans, not svcs).
2. Watch for `[gdl-bad]`, `[gdl-miss]`, `[datafail]`, `[seg-epoch]`, `[texdiag]` lines and any
   visual difference at course transitions / results screens / machine select (the memos
   invalidate on every segment write and asset load; a stale-texture or missing sub-DL there
   would be a memo-validity bug -- flip brfast=0 to confirm).
3. Check `[watchdog] heapUsed` stays on the usual plateau across the cup (the memos are
   fixed-size static tables: ~190 KB total; the raw-copy index grows with gRawTextureCopies).
4. Optional attribution: `[debug] brop=1` prints the per-opcode/phase timers (costs ~10 ms/frame
   of svc reads -- measurement only).

## M5 — menu-driven transition storm + desktop compile (coordinator follow-ups)

Autoinput CAN navigate menus: tokens START/A/B/UP/DOWN/LEFT/RIGHT/STICK_* with `tick NAME
[hold]`. Flow learned by SHOT exploration (runs explore1-4, storm1): START -> mode select
(DOWN = PRACTICE) -> difficulty popup (A,A,A) -> cup select (LEFT/RIGHT) -> A -> course preview
(LEFT/RIGHT cycles the cup's courses) -> A,A -> machine select -> A,A -> machine detail ->
START/A -> race. In-race START = pause menu: Continue/Retry/Settings/Change Machine/Change
Course/Quit (DOWN x3 = Change Machine, x4 = Change Course, then A). Script: scratch
`storm2.txt` (Queen c2 Red Canyon -> King c1 Fire Field -> Jack c3 Sand Ocean -> Change
Machine -> race 4). SHOTs r1/c2a/c2b/r2/c3a/c3b/r3/m4a confirm every screen.
- storm2A (brfast=1, build 6776aed): 3 distinct courses / 3 venues + machine select reached,
  frames 28.5k, ZERO gfxfail/gdl-bad/gdl-miss/datafail/bad_alloc/seg-epoch/legacy-resolve/
  texdiag. `[brfast]` receipt: tables version 194 -> 195 -> 196 across the loads, resolve memo
  hit/miss collapsing to misses right after each load (memo repopulating), gen=64/window
  (one gSegments/seg-9/epoch snapshot per adapter).
- Resolve memo grown 2048 -> 8192 slots after the receipt showed a 34 % miss rate in a
  30-machine race (~1.5k distinct VTX/MTX/SETTIMG keys per frame).
- Desktop (macOS, `cmake -S . -B build-mac -G Ninja -DCMAKE_BUILD_TYPE=Release` after
  `brew install glew spdlog tinyxml2 nlohmann-json`): configure OK; `port/n64_gfx_bridge.cpp`
  COMPILES (only the pre-existing volatile-increment / unused-function warnings), i.e. the
  __3DS__ guards hold. The link fails on PRE-EXISTING macOS incompatibilities unrelated to this
  branch: decomp/include/libc/{stdlib,string}.h + global.h redefine ssize_t/strchr/memcpy
  against the macOS 26 SDK (1190 errors in decomp C files), missing EK asset includes,
  port/gdx_frame_pacer.c. README supports Windows/Linux only.
- storm2B (brfast=0 control, same script, same build): identical screen sequence, zero
  anomaly lines. SHOT parity A vs B: c2a/r2/c3a/m4a BYTE-IDENTICAL (0 differing bytes of
  288 054); c2b/c3b 3.0k/3.4k bytes (animated track outline + arrow blink on the course
  preview), r1 4.5k, r3 12.7k (pack positions). Heap at aligned beats 1/51/101/160:
  40.55/44.00/44.38/44.97 MB (on) vs 40.55/44.14/44.37/44.87 MB (off) -> same plateau.
  br over the 189 aligned windows with off>4 ms: 6.78 -> 3.24 ms (-52 %).
Artifacts rebuilt from 6776aed: build-3ds/port/3ds/G-Diffuser-3DS.{3dsx,cia} (+ /tmp/bcache-art).
