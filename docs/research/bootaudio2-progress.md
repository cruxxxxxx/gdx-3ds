# Task G — boot audio, second attempt (LOCKED-60 round 2) — progress

Branch `feat/3ds-bootaudio2`, worktree `~/code/gdx-3ds/bootaudio2` (off mainline d980f78),
killswitch `[debug] audioprime2` (default 1; =0 = the round-1 `audioprime` path byte for byte).
Brief: docs/research/locked60-round2.md, section "Task G". Round 1: docs/research/bootaudio-progress.md.

## Hardware symptom (from the brief)
`[audioprime] on=1 t=10736ms state=run primes=1/0 ring=1184 rmin=320 queued=2/2 skips=3 under=2`:
with audioprime on, the ring still dipped to 320 frames (< one 512-frame chunk) somewhere between
t=5.7 s and t=10.7 s of boot although it had been primed to ~2144. The emulator never stalls there.

## Implementation (port-side C only)
- port/3ds/audio/gdx3ds_audio_ndsp.c — `[audioprime2]` block:
  - ring 8192 frames on the audioprime2 path (was 4096) so a 4096-frame target + one 528-frame
    tick can never wrap into drop-oldest;
  - `gdx3ds_audio_producer_target_frames(raceActive)`: 4096 until the first race-active frame
    (gGdxRaceActive, set by the first course load, well before the countdown) or 15 s after init,
    then 2048 (unchanged race value => race-start sync identical); one-shot
    `[audioprime2] boot target released (<reason>)` receipt;
  - producer tick bracket (`gdx3ds_audio_producer_tick_begin/end`, svcGetSystemTick -> us),
    main-thread boot phase tag, preload worker running/core -> `[audioprime2] under edge #n`
    companion line (tick{in for last max n}, main phase, preload state, prod/drain cores) and a
    periodic `[audioprime2] on=1 ...` line on the [audioprime] cadence (max tick since last line);
  - `gdx3ds_audio_grant_syscore()` records the APT_SetAppCpuTimeLimit grant for the HOME-restore
    re-apply when the preload ladder takes core 1.
- port/gdx_audio_thread.cpp: producer paces to `gdx3ds_audio_producer_target_frames(gGdxRaceActive)`
  (off => constant 2048) and brackets gdx_audio_produce_one_tick.
- port/3ds/main_3ds.cpp: `[debug] audioprime2_preload_core` picks the preload worker's first rung:
  2 (DEFAULT) = round-1 ladder (core 2 on New3DS -> syscore -> appcore), 1 = skip core 2 (syscore via
  the recorded grant, else appcore 0x3D), 0 = appcore 0x3D only. Receipt
  `[audioprime2] preload worker on core N (...)`; boot phase tags preload/warm/bootproc/frames.
- Not touched: HOME suspend/park gate, the audioprime prime/queue paths, LUS/decomp.

## Milestones
- M0: brief committed (1d046f8); implementation 6d28904; build-3ds .3dsx + .cia green.
- M1: Azahar chain /tmp/bootaudio2-chain.sh launched (runner /tmp/bootaudio2-run.sh, lock protocol,
  artifacts /tmp/bootaudio2-art/<tag>-log.txt): on1/on2/on3 boots 60 s, off1 control, stalloff30 /
  stallon30 (audioprime=1 both, audioprime2 0/1), raceon/raceoff 400 s A-mash (30-machine race).
- M2 (round A, build 6d28904 = brief item (a) as written, preload forced OFF core 2):
  on1/on2/on3: preload landed on core 1 (syscore), `[audio-blob] preload worker ... resident in 8272 ms`
  (round 1 / key off: 4310-4319 ms) and EVERY boot hit `under=1` at t~4770 ms:
  `[audioprime2] under edge #1: ... tick{in=1 for=112049us ...} main=frames preload=running` and the
  next receipt `tick{... max=2135133us}` = one producer tick lasted 2.1 s, right after
  `[audio-cmd] INIT_SEQPLAYER player=0 seqId=0` / `[fontconv] font=0 ... samples=71`: the title font's
  sample load blocked inside gdx_segment_source's in-flight wait for the audio_table preload.
  audio_blob/audio_table is Deflate-stored in fzerox.o2r (10,744,340 -> 9,732,854 B, 9%): the preload
  is an INFLATE, CPU-bound, halved on the 30% syscore. off1 (same build, key off => core 2): under=0.
  => item (a) is counterproductive; default reverted to the round-1 ladder behind the knob (6be88ae).
  Artifacts: /tmp/bootaudio2-art/roundA/.
- M3 (round B, build 6be88ae, .3dsx 2983728 B + .cia 1742784 B):
  on1/on2/on3 (60 s boots): under=0 drop=0, preload on core 2 in 4310-4323 ms, primed ring=4320 at
  t~0.52 s, boot rmin=3072 (round 1: 1024) until `boot target released (boot window elapsed)` at
  t=15.0 s (ring ~3000-3500), then rmin=1024 as before; no producer tick >= 50 ms. off1: under=0.
  stallsim 30 ms A/B (audioprime=1 both): off (round-1 path) under=1 at push #60 (ring 528 at the
  stall); on: under=0, ring 2576-2960 at each stall (~80-90 ms headroom vs ~16-32 ms).
- M4 (round B race A/B, 400 s A-mash 30-machine GP race, same autoinput): raceon (key on) sub=24274
  done=24272 drop=0 under=0 skips=0; raceoff (key off) sub=24641 done=24639 drop=0 under=0 skips=0.
  `[audio-seq] task=` series IDENTICAL line for line (47 lines; race music enabled at task=513 both),
  INIT_SEQPLAYER order identical. Zero gfxfail/gdl-bad/gdl-miss/datafail/fatal/bad_alloc/texdiag.
  Heap: beat 1 on-off = +15.9 KB (= the 8192-frame ring), beat 60 44.65 vs 44.51 MB = within the
  round-1 same-config run-to-run spread (44.62 vs 44.51 MB); plateau flat. SHOT BMPs differ on vs off
  exactly as round 1's two same-config runs did (AI race captured at fixed ticks: nondeterministic).
  Boot target released by "boot window elapsed" (t=15.0 s) in every run: the first course loads well
  after 15 s, so race-start pacing is the unchanged 2048 in both arms.
  Final build 6be88ae: .3dsx 2983728 B, .cia 1742784 B (copies in /tmp/bootaudio2-art/).

## HW test (user)
ini `[debug] filelog=1 filelog_max_kb=4096 diag_audio=1 audioprime=1 audioprime2=1` (control:
`audioprime2=0`; optional A/B `audioprime2_preload_core=1` = preload off core 2). Watch log.txt:
`[audioprime2] ON: ring=8192 ... preload_core=2`, `[audioprime2] preload worker on core 2`,
`[audioprime] primed: ring=~4320`, boot-burst `[audioprime] ... rmin=` should sit ~3072 (was ~1024),
`[audio-blob] preload worker: 7/7 families ... resident in N ms` (N is the audio_table inflate time on
hardware), `[audioprime2] boot target released (boot window elapsed): t=15000ms ... under=0`, and
`[audio-out] ... under=` staying 0 through boot and a race. If it still splices, the pair
`[audioprime] under edge #1: t=...` + `[audioprime2] under edge #1: ... tick{in=1 for=Xus last=Yus
max=Zus n=N} main=<phase> preload=running|done/coreC cores=prod2/drain2` says whether the producer was
mid-tick (and for how long), and the next `[audioprime2] on=1 ... tick{... max=...}` line gives that
tick's full duration; `preload=running` + a multi-hundred-ms tick right after
`[audio-cmd] INIT_SEQPLAYER player=0 seqId=0` = the title font's sample load waiting on the
audio_table inflate (the emulator mechanism), which no cushion covers.

## Recommended next lever (out of this task's port-side scope)
Store audio_blob/audio_table uncompressed in fzerox.o2r (Deflate saves only 9% of 10.7 MB): the boot
preload becomes an SD read instead of an inflate, the title font's sample load can no longer wait on
it, and the preload worker's core stops mattering. Alternative: a streaming loader that publishes
partial residency so a first-touch read inside the already-copied span proceeds.

## Root fix — audio_table stored uncompressed in fzerox.o2r (follow-up, commit d4da0a2)
Inventory of the current archive (unzip -v, entries >= 256 KB): `audio_blob/audio_table`
10,744,340 B deflated to 9,732,854 (9%), `segment_blob/common_assets_compressed` 2,534,084 -> 1,877,819
(26%, MIO0 inside), `segment_blob/machine_global_gfx` 298,228 -> 93,900 (69%, kept deflated). The
first two are the boot preload worker's big reads; both now STORED by tools/prebake/prebake.py
(post-golden-gate rewrite: same order/names/sizes/CRCs, everything else re-deflated).
Exact command used (extractor from the bridgecache desktop build; the ROM stays in place):
```
python3 tools/prebake/prebake.py --rom "~/Downloads/F-Zero X (USA).z64" \
  --extractor ~/code/gdx-3ds/bridgecache/build-mac/gdx-extract/install/bin/gdx-extract \
  --out /tmp/bootaudio2-art/sdmc --skip-golden
```
(`--build-dir build` works instead of `--extractor` when the desktop tree has built gdx-extract.)
`--skip-golden` was needed because the extractor's raw output (sha256 7d60d975...) is byte-identical to
the archive the Azahar SD has run all campaign but does NOT match port/gen/gdx_o2r_expected.h
(1b95e895...): the golden header is stale relative to the current recipes, a desktop-side item.
Result: /tmp/bootaudio2-art/sdmc/3ds/gdiffuser/fzerox.o2r = 17,150,811 B (was 15,499,571; +1.65 MB),
sha256 b3203707..., 3610 records; installed into the Azahar SD (old one backed up at
/tmp/bootaudio2-art/fzerox.o2r.bak). THE HARDWARE SD NEEDS THIS NEW fzerox.o2r for the fix to apply.
