# Stream C — 3DS audio notes

## 1. Why HLE is the primary synthesis path

The desktop port has two synthesis paths behind the same `M_AUDTASK` seam
(`port/gdx_audio_lle.c`): the cxd4 RSP interpreter running the real US-rev0 `aspMain`
microcode (LLE — the grain-free reference), and the software HLE interpreter
(`gdx_audio_hle_run`). On 3DS, HLE is primary because:

- **CPU budget.** LLE interprets RSP instructions one at a time plus a per-tick
  memory-model bridge (copy-in / w1 rewrite / run / copy-back over a scratch arena).
  That was designed against desktop x86 headroom; an 804 MHz ARM11 sharing core 0 with
  the game loop has none to spare. The plan (docs/research/3ds-port-plan.md, stream C)
  mandates HLE-primary; LLE gets ONE benchmark run on device (see TODO list) and is kept
  only if it fits budget — expected verdict: it does not.
- **Fallback topology already favors HLE.** The LLE bridge aborts any tick it cannot
  confidently marshal and defers to `gdx_audio_hle_run`; host buffers are the source of
  truth across mixed ticks. So HLE is already the correctness backstop the whole audio
  stack is validated against — making it primary removes a component, not a guarantee.
- **No ucode-shipping question on device.** LLE needs the Nintendo-copyrighted `aspMain`
  slices extracted from the user's ROM into the archive. HLE needs nothing extra.
- Precedent: Wyatt-James' sm64-3ds-port runs HLE-style synthesis (Enhanced RSPA) and
  reaches budget on the same silicon.

## 2. Output architecture (implemented in `gdx3ds_audio_ndsp.c`)

- ndsp channel 0, `NDSP_FORMAT_STEREO_PCM16`, 32000 Hz (`ndspChnSetRate`), polyphase
  interpolation for the 32000 → 32728 Hz DSP mix-rate gap, front L/R mix 1.0.
- Push ring: interleaved s16, default 4096 frames (`gdx3ds_audio_init(0)`),
  LightLock-guarded. Lock-free SPSC was rejected because the frozen contract's
  drop-oldest overrun policy makes the *producer* move the read cursor; every critical
  section is a bounded memcpy so `gdx3ds_audio_push` never waits on playback.
- Wave buffers: 3 × 512-frame (16 ms) `ndspWaveBuf`s in linearAlloc (DSP-visible) memory,
  `DSP_FlushDataCache` before every `ndspChnWaveBufAdd`. Drain thread refills DONE/FREE
  slots every 2 ms, padding underruns with silence (partial fills counted in
  `gdx3ds_audio_underrun_chunks()`).
- `gdx3ds_audio_buffered()` = ring frames + real frames still queued on the DSP
  (over-reports by at most the played portion of the current chunk, < 512 frames).

### Core ladder (drain thread placement)

| Rung | Core | Requirement | Notes |
|---|---|---|---|
| 1 | 2 | New3DS (`APT_CheckNew3DS`) | Fully preemptive spare core; the target. |
| 2 | 1 (syscore) | `APT_SetAppCpuTimeLimit(30)` succeeds — homebrew needs **Luma3DS >= 10.1.1** | OS owns most of syscore; we get the granted slice. |
| 3 | 0 (appcore) | always | Shares the core with the game; last resort. |

`threadCreate` returns NULL for an ungrantable core, which is what walks the ladder.
On New3DS `osSetSpeedupEnable(true)` is requested once (804 MHz + L2).
`gdx3ds_audio_drain_core()` reports the rung actually granted (surface it in a boot log
/ INI-visible diagnostic at integration).

### DSP firmware caveat (user-facing — keep in the eventual README)

`ndspInit` **fails unless `sdmc:/3ds/dspfirm.cdc` exists** — libctru's
`ndspFindAndLoadComponent` tries exactly that path (then an `hb:ndsp` env handle) and
nothing else. This applies to real hardware (dump once with the DSP1 homebrew) **and to
Citra-family emulators (Citra/Lime3DS/Azahar), which need the same dump on their emulated
SD root**. (An earlier revision of this note claimed Citra needs no dump; that was wrong,
and it hid weeks of silent-emulator runs.) On failure the backend logs the `Result` on the
`[audio-out]` channel (svc debug + sdmc filelog + console) and degrades to a **null
sink**: the ring drains at real-time rate so pacing (`gdx3ds_audio_buffered`) behaves
identically and the game runs silent instead of crashing. `gdx3ds_audio_init` still
returns 0 (degraded success); `main_3ds.cpp` now logs the outcome in the boot sequence via
`gdx3ds_audio_output_active()`.

### Silence triage ([audio-out] receipts)

- Boot log always carries: `ndspInit` result, channel format/rate/mix/mastervol, wave-mem
  vaddr + `osConvertVirtToPhys` paddr (0 would mean non-linear memory the DSP cannot DMA),
  drain core, and `audio output ACTIVE / NULL SINK` from `main_3ds.cpp`.
- `[debug] diag_audio=1` adds a ~5 s periodic line:
  `sub/done/nz` (chunks submitted / DONE transitions seen / nonzero-content chunks),
  `ck16` (first-16-sample checksum of the last chunk), ring/inflight/drop/underrun,
  `push=<calls>/<frames> or=<OR-accumulator over all pushed samples>`, slot statuses.
  Reading it: `done=0` while `sub` grows → the DSP never consumes buffers (firmware/ndsp
  problem). `or=0x0000` forever → the producer only ever pushed digital silence (content
  problem upstream of this backend). Both moving with `nz>0` → real audio is reaching the
  DSP.
- `[debug] audio_testtone=1` replaces every submitted chunk's content with a 440 Hz sine
  (ring pacing untouched): tone audible → output plumbing good, investigate producer
  content; still silent → ndsp/DSP path broken regardless of game content.

## 3. A/B validation plan (PC LLE reference vs 3DS HLE)

The capture tap (`port/gdx_audio_capture.h`) sits at decomp
`src/audio/disk/lib/thread.c:87-96`, immediately before `osAiSetNextBuffer` — upstream of
ALL host post-processing — and writes headerless interleaved s16 LE stereo plus a SHA-256
sidecar. While armed it also pins the one entropy source in `AudioThread_CreateTaskImpl`
(the `osGetCount()` RNG at thread.c:205-226), so captures are deterministic run-to-run.

1. **PC LLE reference** (golden): `GDX_PCM_CAPTURE=ref_lle GDX_PCM_CAPTURE_FRAMES=N`
   with `gEnhancements.Audio.LLE=1` over a scripted segment (title → menu → race start).
   `N` fixes the window in frames, not wall-clock.
2. **PC HLE baseline**: same window, `gEnhancements.Audio.LLE=0` → `ref_hle.pcm`.
   Diff vs step 1 = the *intrinsic* HLE-vs-LLE delta on x86. Any 3DS deviation beyond
   this baseline is a port bug, not an HLE-accuracy question.
3. **3DS HLE capture**: the same tap compiled into the 3DS build writing to
   `sdmc:/3ds/gdiffuser/cap_3ds.pcm` (tap code is plain C stdio — expected to compile
   as-is; if the sdmc write stalls the audio tick, buffer to RAM and flush at finalize).
   Same frame cap, same deterministic pin.
4. **Compare**: `sha256 cap_3ds.pcm == sha256 ref_hle.pcm` is the pass. HLE synthesis is
   integer-deterministic, so bit-identical is the expectation; any mismatch is located
   with a first-divergent-frame diff and almost certainly indicts a 32-bit/alignment
   issue (stream E territory) or ARM-specific UB — file it with the frame offset and the
   surrounding Acmd sequence.
5. **Listening pass** (separate, subjective): LLE-vs-HLE grain differences on real
   speakers, per the risk-table tripwire ("HLE audio inaccurate for this title").

Prereq for step 3: streams B/D far enough for a scripted boot on Citra/hardware. Until
then steps 1-2 (pure PC) can land and be committed to the harness.

## 4. Measured-budget TODO list (fill in on device / Citra, New3DS unless noted)

- [ ] `gdx_audio_produce_one_tick` ms/tick (HLE), avg + p99, in menus and at 30 machines.
- [ ] Same with LLE enabled — the one-shot cxd4 benchmark; record verdict, then stop.
- [ ] Drain-core ladder outcome matrix: New3DS (expect 2), Old3DS+Luma (expect 1),
      Old3DS w/o modern Luma (expect 0), Citra.
- [ ] `gdx3ds_audio_underrun_chunks()` / `gdx3ds_audio_dropped_frames()` across a full
      race at target fps — acceptance gate M4 is zero underruns.
- [ ] Chunk-size sweep 256 / 512 / 1024 frames: underruns vs latency (current 512 = 16 ms
      per buffer, 48 ms full pipeline).
- [ ] Ring-size sweep vs the production thread's desired-buffered target (2048 frames
      suggested to match the desktop cushion) — confirm 4096 capacity gives headroom.
- [ ] LightLock contention: worst-case push hold time with drain on core 0 (rung 3).
- [ ] `osSetSpeedupEnable(true)` on/off delta on tick time (New3DS).
- [ ] Null-sink drift over 10 min (drop-oldest churn when silent-mode ring saturates).
