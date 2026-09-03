# Task D — boot audio jitter (LOCKED-60) — progress

Branch `feat/3ds-bootaudio`, worktree `~/code/gdx-3ds/bootaudio`, killswitch `[debug] audioprime`
(default 1; =0 = old path). Diagnostic knob `[debug] audio_stallsim=<ms>` (default 0).

## Analysis (from the driver, no LUS/decomp change)
- An `under` is a PARTIAL chunk (0 < ring < 512 at a DONE slot): a silence tail spliced into the stream.
- Producer paces on buffered() = ring + in-flight to 2048 frames; 3 slots in flight = 1536, so the
  steady-state ring holds only ~512-1040 frames: any producer stall of a few ms past the next DONE splices.
- Boot start feeds the DSP from an empty ring immediately (528-frame ticks vs 512-frame chunks).
- Emulator baseline (bcache census log) shows under=0: Azahar's host CPU never has a slow tick; the boot
  riff's first tick blocks on the audio_table preload BEFORE anything is in the ring (no partial).

## Implementation (port/3ds/audio/gdx3ds_audio_ndsp.c)
- PRIME: drain feeds silence (ring untouched, not an underrun) until ring >= 1024 frames, bounded 1 s after
  the first push; re-armed on gdx3ds_audio_resume (HOME/sleep restarts from an empty ring).
- QUEUE: at most 2 slots in flight (was 3). Producer target unchanged => same latency / race-start sync;
  the cushion moves from the DSP queue into the ring (stall tolerance ~16-32 ms instead of ~0-16 ms).
- Receipts (diag_audio=1): `[audioprime]` line on the [audio-out] cadence + 500 ms burst for 5 s of boot
  (rmin = lowest ring level at a pull since the previous line), `under edge #n` one-shots (first 8),
  prime completion/timeout one-shots, ON/OFF one-shot at init.
- stallsim: producer sleeps N ms before pushes #20/#60/#100 (emulator stand-in for slow HW ticks).

## Milestones
- M0: brief committed, build-3ds green with the implementation.
- M1: build 31792a5 (.3dsx + .cia) green; Azahar chain launched via /tmp/bootaudio-chain.sh
  (runs: on1/on2/on3 boot 60 s, off1 control, stall{off,on}{18,30} A/B, raceon 400 s A-mash);
  artifacts in /tmp/bootaudio-art/<tag>-log.txt. Lock protocol: /tmp/bootaudio-run.sh waits on
  /tmp/azahar.lock (mkdir), touches it every 20 s, removes it when done.
- M2: Azahar results (build 31792a5, ini console/filelog/diag_audio/verbose/gputrace=1):
  - on1/on2/on3 (audioprime=1, 60 s boots): under=0 every boot; `primed: ring=2208 at t=540ms` (first
    push t=517ms, 31 silence chunks); steady rmin=1024 (old path rmin=512, and rmin=0 in the first 500 ms).
  - stall-sim A/B (producer sleeps N ms before pushes #20/#60/#100): 18 ms: off under=3/3, on 0/3;
    30 ms: off 3/3, on 1/3 (edge #1 real=16, ring 528 at stall start = the predicted ~16-32 ms bound).
    Old path had ring=400/128/128 at those pushes, i.e. < one chunk: any stall to the next DONE splices.
- M3 (final build d4f2a8a, .3dsx 2958156 B + .cia 1724352 B, copies in /tmp/bootaudio-art/):
  - onF boot: under=0, primed at t=571 ms, rmin=1024; raceF (400 s A-mash, 30-machine race, 15.4k frames):
    sub=24679 done=24677 drop=0 under=0, skips=6 (queue cap bit 6 times in 395 s), race music starts at
    [audio-seq] task=513 in both new and baseline census logs (same sync), heap plateau 44.5 MB at beat 60
    (baseline 44.6 MB), zero gfxfail/gdl-bad/gdl-miss/datafail/fatal/bad_alloc/texdiag lines.
  - Earlier raceon (31792a5) over-counted `done` while a capped DONE slot sat in the queue; d4f2a8a parks
    it as FREE so each chunk counts once (audio path unchanged).
  - Not exercised in the emulator: HOME suspend/resume (gdx3ds_audio_resume re-arms the prime while both
    threads are parked; the 1 s timeout bounds it). Preload-thread core move (option b) NOT needed: the
    producer's first push lands at t~0.5 s, long before the 4.3 s audio_table preload finishes.

## HW test (user)
ini `[debug] filelog=1 filelog_max_kb=4096 diag_audio=1 audioprime=1` (then `audioprime=0` control).
Watch log.txt: `[audioprime] ON/OFF` at init, `primed: ring=... at t=...`, the 500 ms boot burst
(`rmin` should sit at ~1024 with on, ~512/0 with off), `[audio-out] ... under=` must stay 0 through
boot and a race; any `[audioprime] under edge #n: t=...ms ... push=...` line pins when the producer
fell behind. `[debug] audio_stallsim=18` reproduces the splice on the old path (under=3) and not with
audioprime=1.
