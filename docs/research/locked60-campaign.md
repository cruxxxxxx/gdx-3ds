# LOCKED-60 CAMPAIGN — shared brief (2026-09-02)

Goal: a locked 60 fps in 30-machine crowd frames on New 3DS. Mainline `feat/3ds-hwaudio` @ 256e397
is release-grade (median ~57 fps HW, floors ~47-55). Crowd frames need ~3 ms more CPU headroom.
Each task below is ONE agent on ONE branch/worktree, killswitched, emulator-verified, then merged
only after the user's hardware verdict. Read this whole file, then your task section.

## Hardware profile (New 3DS, gputrace on, 64-frame windows, per-frame ms) — the ground truth
Race crowd windows: `[prof] br=3.2-3.8 dsp=6.5-7.3 vtx=0.7 tri=3.4-3.6 imp=1.2-1.8 drw=2.3-2.5`
(nD=110-123 draws, nT=550-570 tri cmds, nI=95-110 imports). `[gpu] wall=23.4 build=16.3 gpu=5.6`.
`[profop]` top opcodes (ms/frame over calls/frame): `E4 texrect=3.9-8.9/90-138`,
`06 tri2=2.4-4.4/143-270`, `F3 loadblock=1.9-2.4/144-199`, `01 vtx=0.6-1.2/28-79`,
`FA/FB prim/env=0.5-1.0/25-33 each`. `[race-dl]` per frame: lists=70 cmds=1486 tri=222 trect=41.
Emulator ratios differ (E4=61 us there vs ~40-60 us here); trust hardware for ranking, use the
emulator for A/B deltas on frame-aligned windows.
Conclusion: per-draw / per-texrect / per-load BOOKKEEPING dominates, not vertex math (vtx is
~1 ms, so GPU vertex transform is NOT a lever for this game — it is shelved for good).

## Common rules (every task)
- Worktree given in your section (submodules + 42-patch stack applied; `build-3ds` configured
  and built). Build: `export DEVKITPRO=/opt/devkitpro; cmake --build build-3ds -j8`. CIA:
  `--target G-Diffuser-3DS-cia`.
- libultraship changes go in a NEW patch file `port/3ds/patches/lus-<task>-<thing>.patch`,
  generated as a PURE DELTA on top of the full existing stack (diff the patched submodule file
  vs your edited copy: `git -C libultraship diff` after the stack is applied captures exactly
  your delta if you commit nothing in the submodule; verify with a clean-submodule roundtrip:
  `git -C libultraship checkout -- . && apply the README list && apply yours`). Append ONE line
  to the README apply list. Never edit existing patches. decomp changes likewise
  (`decomp-<task>-*.patch`). Port-side code (port/, port/3ds/) is edited directly.
- KILLSWITCH mandatory: `[debug] <key>=0` (3DS ini via gdx3ds_config_get_bool, read live or per
  task) and, for LUS code, a CVar/env equivalent on desktop. Off = byte-identical old path.
- RECEIPTS: a `[<task>]` census line on the `[race-dl]` cadence (same gate: verbose/gputrace)
  with hit/miss/merged/skipped counters so the hardware log proves the lever engaged.
- Emulator verification (Azahar): singleton lock `/tmp/azahar.lock` — `mkdir` to acquire; if it
  exists and its mtime is < 60 s old, another agent owns Azahar: WAIT in an until-loop (never
  kill their run); `touch` it every <= 25 s while running; `rm -rf` when done. SD at
  `~/Library/Application Support/Azahar/sdmc`; ini `3ds/gdiffuser/gdiffuser.ini` — MERGE keys,
  do not drop others' sections. Measurement ini: console=1 filelog=1 filelog_max_kb=4096
  diag_audio=1 verbose=1 gputrace=1 <yourkey>=1|0. Autoinput: `/tmp/gpt-autoinput.txt` ->
  `sdmc/gdx-autoinput.txt` (A-mash into a 30-machine GP race, ~6 min); menu-navigation scripts
  exist from the bridgecache agent (see docs/research/bridgecache-progress.md M5 for the
  autoinput grammar and a 3-course + machine-swap flow). Snapshot `log.txt` to `/tmp/<task>-art/`
  every 25 s; other agents may delete SD artifacts. `pkill -9 -i azahar` only your own instance.
- A/B = same build, key on vs off, frame-aligned `[prof]` windows (same frame numbers) plus
  `[profop]` opcode totals; report the per-frame delta on crowd windows (nD >= 100) AND on menu /
  machine-select windows (must not regress). Visual parity: SHOT BMPs in `sdmc/autotest`
  compared on vs off (byte-identical or explain every diff). Zero new error lines
  (`[gfxfail]`, `[gdl-bad]`, `[gdl-miss]`, `[datafail]`, `[fatal]`, `bad_alloc`, `[texdiag]`).
  Heap flat (`[watchdog] heapUsed` plateau equal to control).
- Commit early and often; keep `docs/research/<task>-progress.md` updated at every milestone
  so a relaunched agent resumes from the file. Never cat whole logs — grep/tail/awk. Keep the
  final report under 40 lines: HW-profile bucket targeted, design, A/B table, parity, risks,
  commits, and the hardware test instructions (ini key, what to watch in log.txt).
- Do not merge. Do not touch other worktrees. If the census says the lever cannot pay
  (< ~0.5 ms/frame projected on hardware), STOP early and report that instead of building it.

## Task A — texrect batching (branch feat/3ds-texrect2, worktree ~/code/gdx-3ds/texrect2)
Target: `E4` = 3.9-8.9 ms/frame at 90-138 texrects/frame (HUD digits, energy bar, minimap,
rankings, boost meter, crowd/portrait sprites). Each texrect today ends a batch and becomes its
own DrawTriangles with its own texture bind/import. Existing work to build on:
`lus-texrect-run-memo.patch` (rect-run draw-state memo, `[texrect]` key at interpreter.cpp:176),
`lus-texrect-viewport-hoist.patch`, `Interpreter::GfxDpTextureRectangle` (interpreter.cpp:6044),
`Interpreter::Flush` (:264) and the packed-VBO triloop (`lus-3ds-triloop-packed-vbo.patch`).
Do first: a census `[trect]` of consecutive texrect RUNS: run length, whether texture (tile
image ptr/fmt/size/tlut), combiner, blend/othermode, prim/env, and viewport are identical across
the run, and how many draws would remain if identical-state runs were merged. Then implement
run merging: accumulate quads of an identical-state run into the same VBO batch (one
DrawTriangles per run) instead of flushing per rect; keep the per-rect UV/texture math exact.
Second lever if the census shows same-texture runs broken only by a texture switch among a
small set (digit glyphs): a small HUD atlas built at import time (pack the run's textures into
one 3DS texture, remap UVs) — only for CLAMP-addressed tiles with UVs inside [0,1]; skip
anything wrapped/mirrored. Killswitch `[debug] trectbatch`.

## Task B — LOADBLOCK/TMEM bookkeeping (branch feat/3ds-tmem2, worktree ~/code/gdx-3ds/tmem2)
Target: `F3` = 1.9-2.4 ms/frame at 144-199 loads/frame (~12 us each) even though the actual
copies are already skipped for same content (`lus-tmem-same-content-skip.patch`,
`lus-tmem-span-store.patch`, interpreter.cpp ~5220-5280; `lus-crowd2-tilestate-value-gate.patch`).
Do first: per-phase timers inside GfxDpLoadBlock / GfxDpLoadTile / the span store (hash of the
source? memcmp? span bookkeeping? texture-cache invalidation? the `textures_changed` re-arm?) —
find the 12 us. Likely levers: (1) memoize the load decision per (tile, source ptr, size, tmem
addr, dma generation) so a repeat load in the same frame or the next frame is O(1) with no
content read; (2) make the same-content hash incremental or sampled where safe; (3) lazy load —
defer the TMEM work until the first triangle/texrect actually references the tile (loads whose
tile is never drawn cost nothing). Killswitch `[debug] tmemfast`.

## Task C — per-triangle cost (branch feat/3ds-tri2, worktree ~/code/gdx-3ds/tri2)
Target: `06` = 2.4-4.4 ms/frame at 143-270 tri2 commands (~16 us per G_TRI2 = 8 us per triangle,
~6400 cycles at 804 MHz — far too many for clip test + 3 packed vertex writes). Build on
`lus-3ds-triloop-packed-vbo.patch` (`[triloop]` packed emission, interpreter.cpp ~248),
`lus-s7-tri-state-memo.patch` (per-tri draw-state memo ~155-195), `lus-traffic-vtx-clipmask.patch`,
`GfxSpTri1` and the clipper (~2833). Do first: per-phase timers in the tri path (state memo
check, clip test, per-vertex packing, batch-break checks, the DrawTriangles handoff) over a
crowd window to find the cycles. Then attack the top phase: candidates are cutting per-vertex
recomputation (pack once per vertex on G_VTX, not per triangle reference), removing per-tri
gate/dev-flag probes, hoisting state-memo validity to per-batch, and avoiding float
conversions on the packed path. Killswitch `[debug] trifast`.

## Task D — boot audio jitter (branch feat/3ds-bootaudio, worktree ~/code/gdx-3ds/bootaudio)
Symptom: 2 ndsp underruns in the first second of boot on hardware (`[audio-out] ... under=2` by
chunk ~640), identical across builds, audible as jitter. Facts: audio drain + HLE producer
threads run on core 2 at priority 0x18 (port/3ds/audio/gdx3ds_audio_ndsp.c ladder ~:322-350,
~:633-660); the asset PRELOAD thread is also created on core 2 (port/3ds/main_3ds.cpp:745-755,
prio 0x30) and does SD reads during boot; ndsp starts before the ring has any headroom.
Do: reproduce in Azahar (`[audio-out]` under counter over the first 2 s), then fix by (a) not
starting DSP playback until the ring holds >= N chunks (prime the ring, then ndspChnWaveBufAdd),
and/or (b) moving the preload thread off core 2 (core 1 via APT_SetAppCpuTimeLimit if granted,
else core 0 at low priority) and/or (c) raising the drain thread's early-boot poll cadence.
Killswitch `[debug] audioprime`. Verify: under=0 through boot in the emulator, no regression in
`[audio-out] sub/done/drop` during a race, and no change to race-start audio sync. Small task;
finish it fully, including the `.cia`, and report in 20 lines.

## Task E (reserve, not launched yet) — machine texture atlasing
`imp` 1.2-1.8 ms + draw breaks between machine parts (95-157 imports/frame). Complex UV wrap
semantics (only CLAMP tiles are atlas-safe). Launch only after A-C land.
