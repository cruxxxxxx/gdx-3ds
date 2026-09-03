# LOCKED-60 ROUND 2 — research-independent tasks (2026-09-03)

Common rules: identical to docs/research/locked60-campaign.md in the worktree (patch-as-pure-delta
over the FULL current stack — now 47 patches incl. lus-tri2-phase-census, lus-trifast-tri-memo-pack,
lus-tmem2-tmemfast, lus-trect-census, lus-trectbatch-atlas; killswitch; receipts; Azahar lock
protocol; merge ini keys; progress file; report format). Base = mainline `feat/3ds-hwaudio`
@ d980f78, which already carries brfast/trectbatch/trifast/tmemfast/audioprime (all default on).

Hardware crowd-frame profile after the campaign (New 3DS, gputrace on, stereo on, ms/frame):
`dsp 7.8  tri 4.6  br 3.1  drw 1.2  vtx 1.2  imp 0.8` = ~18.7 ms CPU; GPU ~5.6 ms (idle-ish).
Steady race ~14.9 ms. Target: locked 60 = 16.7 ms incl. game logic, so ~2-3 ms more in crowds.
The emulator's ratios differ (svc probes cost ~1.3 us each there); use it for A/B deltas and
attribution shape, never for absolute ranking.

## Task F — dispatch-remainder census + first lever (branch feat/3ds-dspcensus, worktree ~/code/gdx-3ds/dspcensus)
`dsp` = everything in the interpreter walk that is not tri/vtx/br/drw/imp: opcode dispatch,
SETTILE/SETTILESIZE (F5 ~130/frame), LOADBLOCK remainder, prim/env color (FA/FB ~30/frame each,
each a batch break = an extra DrawTriangles), texrect remainder, geometry-mode/othermode changes,
matrix ops, and the flush bookkeeping itself. Nobody has measured its composition; [profop] only
prints the top 6 opcodes and attributes child draws to whichever opcode flushed.
Do first (census, commit, progress file): (1) a FULL per-opcode table (all 256, calls + exclusive
ticks, drained on the [race-dl] cadence, gated on gputrace+verbose) and (2) a BATCH-BREAK census:
for every Flush(), which state change caused it (texture switch, combiner change, prim color,
env color, othermode/blend, geometry mode, viewport/scissor, texrect, explicit end-of-list, buffer
full), counted per frame — this is the map of the ~55 draws/frame. (3) the per-command fixed
overhead: ticks between the end of one command's handler and the start of the next (the dispatch
loop itself), as a per-frame total.
Then the lever the census supports, e.g.: fold PRIM/ENV colors into per-vertex attributes for
combiners that use them as a multiplier (removes the batch break; PICA200 has 4 vertex color-ish
attribute slots — check gfx_citro3d's vertex layout and the packed VBO stride), or a
SETTILE/SETTILESIZE no-op value gate beyond the existing crowd2 gate, or a cheaper dispatch loop
(computed goto is not available in this C++ switch; measure whether the flat-dispatch patch's
table is hot). Killswitch `[debug] dspfast`. Receipt `[dsp2]`. A/B on frame-aligned windows by
vtxN bucket (NOT nD — the atlas cuts draws), SHOT parity incl. a race screenshot via
`screencapture -x` during the run, zero error lines, heap flat, clean-stack roundtrip, .3dsx+.cia.

## Task G — boot audio, second attempt (branch feat/3ds-bootaudio2, worktree ~/code/gdx-3ds/bootaudio2)
Hardware still shows `under=2` at ~10.7 s after boot with audioprime on: `[audioprime] on=1
t=10736ms state=run primes=1/0 ring=1184 rmin=320 queued=2/2 skips=3 under=2` — the ring
dipped to 320 frames (below one chunk) during boot even though it was primed to ~2144. The
emulator never stalls, so use the `[debug] audio_stallsim` knob and reasoning from the receipts.
Facts: the asset PRELOAD thread is created on core 2 (port/3ds/main_3ds.cpp ~745-755, prio 0x30)
and does SD reads for ~4 s; the audio drain and HLE producer threads also run on core 2 at 0x18;
the producer is driven by the main thread's frame notify + a 5 ms self-wake and produces only
while `gdx3ds_audio_buffered() < 2048`. Likely: during the boot preload + first asset decode on
the main thread, the producer's ticks (gdx_audio_produce_one_tick, ~131 ms worst case on
transitions per the comment in port/gdx_audio_thread.cpp) stall behind SD I/O or main-thread
work, and 2048 frames (64 ms) is not enough cushion for a 10 s boot with a 4 s preload.
Do: (a) move the preload thread off core 2 (core 1 if APT_SetAppCpuTimeLimit granted a share,
else core 0 at low priority; keep New3DS/Old3DS ladders correct), (b) raise the producer target
during boot only (e.g. 4096 frames until the first race-active frame or for the first 15 s,
then back to 2048 so race-start sync is unchanged), (c) log an `under edge` one-shot with the
producer's last tick duration and which thread was running. Killswitch `[debug] audioprime2`.
Verify in Azahar: 3 boots under=0, stallsim 30 ms A/B, race audio + race-start sync unchanged
(`[audio-seq] task=` enable frame identical), .3dsx+.cia. Report under 20 lines with the HW
receipt to look for.
