# onesubmit: one GX submission for both eyes (Old 3DS 3D CPU)

Branch `onesubmit` (from card-4 `5e19320`). Everything here is lever-gated. It was built default
OFF (a build with no new ini keys behaved exactly like `5e19320`, verified below). After the card-6
hardware A/B ("Hardware result" below), ship6 made `onesubmit = 1` and `onesubmit_narrow = 1` the
shipped defaults (code defaults and both auto presets); `onesubmit_kicks` and `onesubmit_ab` stay 0.

## The question

A 3DS homebrew dev suggested submitting ONE command list for both eyes instead of one per eye, to cut
Old 3DS CPU in 3D. Two parts to that:

1. What we already do: the right eye is NOT re-encoded. Since n60stereo the replay re-submits the
   left eye's own command lists with only the framebuffer bind and the `steye` uniform changed
   (`gdx3ds_stereo_frame.cpp`, STEREO.md "Frame replay"). What remains per eye is GX *queue
   entries*: each body chunk is one `GX_ProcessCommandList` for the left eye plus a second one for
   the right. A hardware 3D race frame at D=1 is 11 entries (`FFL|l|r|l|r|lrYX`: 2 fills, the head
   list, 3 left + 3 right body lists, 2 display transfers), each with a GSP submit, a completion
   IRQ and a pass through libctru's GSP event thread on core 0.
2. What was new to test: make each chunk ONE entry that the GPU runs twice, using the PICA200's
   second command-buffer channel, so the CPU never touches the GPU between the eyes.

## What was built

### A. `[debug] gxcost = 1`: the dispatch cost receipt (`port/3ds/gdx3ds_gxcost.c`)

`--wrap` on `GSPGPU_FlushDataCache`, `svcFlushProcessDataCache`, `gspSubmitGxCommand`,
`GX_ProcessCommandList`, `gxCmdQueueRun`, `gxCmdQueueWait`, `gspWaitForEvent`,
`svcWaitSynchronization` (event thread only), `C3D_FrameBegin`, `C3D_FrameEnd`. Off: one load +
branch per call. On: two tick reads per wrapped call, charged to the calling thread (`main` = thread
of the latest `C3D_FrameBegin`, `ev` = libctru's GSP event thread, `oth` = anything else, each `oth`
thread named once with id/priority). About once a second, and at each A/B arm change:

```
[gxcost] f= arm= ent/f= | main us/f fb= fe= qw= qr= pcl= sub= gfl= sfl= gw= | ev wake/f= us/f= sub= | oth sub= gfl= sfl= | irq/f p3d= ppf= psc0= psc1= dma=
```

`ev us/f` is the event thread's busy time (wake to next wait: interrupt pop, `gxCmdQueueInterrupt`
and its submit, the callbacks). The kernel's IRQ entry / context switch is NOT in it; on hardware
that is still the unmeasured part.

Notes from reading it in the emulator: `gxCmdQueueInterrupt` lives in `gspgpu.o` and cannot be
wrapped; its submit is visible as `ev sub`. In the 3D race most frame-end submits and the
whole-heap flushes land in `oth` (logged as thread id 1, prio 0x30) while `main fe=` reads ~0, so on
the Old 3DS pipe `C3D_FrameEnd`'s work is not on the thread that ran the latest `C3D_FrameBegin`
(not traced further). Read `main + oth` together for the frame's submit-side cost.

Cheap sampled modes for the existing profilers, so a card can carry them:
`[debug] hwsite_every = N` (time `[hwsite]` sites in 1 `[c5cat]` window of N) and
`[debug] c3dprof_every = N` (`[c3ddiet]` profile in 1 64-frame window of N).

### B. `[stereo] onesubmit = 1`: both eyes of a chunk in one GX entry (`gdx3ds_stereo_frame.cpp`)

Memory layout per body chunk, all in the open citro3d command buffer:

```
P  left only: [resume words: main fb + steye -1]  CMDBUF_SIZE1 / ADDR0 / ADDR1 (patched at close)
B  FlushState + the chunk's draws + FRAMEBUFFER_FLUSH / INVALIDATE / EARLYDEPTH_CLEAR [+pad] + JUMP1
R  right fb bind + steye +1, CMDBUF_SIZE0=B SIZE1=E ADDR0=B ADDR1=E, JUMP0
E  GPUCMD_Split's FINALIZE pair
```

GSP is handed `[P][B]` only (its last word is `JUMP1`). The GPU runs P, B (left), jumps to R, runs B
again (right), jumps to E, raises one P3D IRQ. R+E (<= 160 B) are flushed with one
`svcFlushProcessDataCache`; `[P][B]` gets GSP's flush through `GX_CMDLIST_FLUSH`. `steye = 0` is
deferred to the next frame's first list (the `stereo_fold` rule). Chunks that are not exactly one open
list (mid-body rebind, NRR hand-off at the kick, sky_late, a multi-entry chunk) fall back to the
classic re-submitted replay, which starts past P like `resume_fold`'s words (`[stereo] onesub fb=`).

Follow-on levers, only active with one-submit frames (defaults as shipped by ship6; `onesubmit`
itself is 1):

| key | effect |
|---|---|
| `[stereo] onesubmit_kicks = 1` | no mid-frame body kick in a 3D frame (head run kept): one chunk = one entry |
| `[stereo] onesubmit_kicks = 2` | no kick and no head run either |
| `[stereo] onesubmit_narrow = 1` (default 1 since ship6) | the frame end flushes only the `[overlap]` noted dirty ranges (whole heap on overflow) and every list carries GSP's flush, instead of `C3D_FrameEnd`'s whole-linear-heap `GSPGPU_FlushDataCache` |
| `[stereo] onesubmit_ab = 1` | arm per `[cpucut]` block, order `0 1 2 3 2 3 0 1` (0 baseline, 1 onesubmit, 2 +kicks, 3 +narrow); the order puts each arm once on each value of block bit 1 so the card's other bit-1 A/B levers do not line up with one arm |

Receipts: `[stereo] onesubmit init`, `[onesub] blk= arm= (name) frame=` at each arm change,
`[stereo] onesub frames= replayed= osf= os/f= fb= tailw/f= arm=` per replay window,
`[overlap] onesub body_kicks_refused= narrow_frames= narrow_ends=`, `[gputl] tp=` +32 / +64 / +128,
`[gputl] seq` shows `b` for a both-eyes entry, `[gxcost] arm=`.

## Emulator results (Azahar, Old 3DS model, `race-mutecity-start`, det=1)

All legs: `tools/dev/gdx-dev retro run --model old --gfx vulkan --bare-ini --replay race-mutecity-start
--seconds 700 --ini det=1 gstate_every=1 gstate_ext=1 fh_every=1 fh_start=3000 gputl=1`, `--3d
side-by-side` for 3D. Runs in `/tmp/gdx-runs/onesubmit/`.

### Goldens

| leg | vs | [fh] L | [fh] R | [gstate] (6003) | SHOTs |
|---|---|---|---|---|---|
| levers off (this branch) 3D | `5e19320` 3D | 3027 same | 3027 same | same | 147, top screens identical |
| onesubmit | levers off | same | same | same | identical |
| + kicks=1 | levers off | same | same | same | identical |
| + kicks=2 | levers off | same | same | same | identical |
| + narrow | levers off | same | same | same | identical |
| onesubmit_ab=1 (+kicks=1) | levers off | same | same | same | identical |
| 2D, all levers on | `5e19320` 2D | 3027 same | n/a | same | identical |

The only byte differences in any leg are 74-92 bytes of bottom-screen shots in the box x 203-210,
y 193-201: the bottom map's "N FPS" readout (5 vs 4), which tracks host speed. `[gstate]` compared
with `acmd=` (audio command count, menu frames, thread timing) masked. All PASS, 49/49 SHOTs,
0 fatals, `[stereo] onesub fb=0` (no fallback) in every race window.

Limit: the emulator has no CPU data cache, so these goldens prove the command-buffer channel jump,
the per-eye binds and the deferred `steye = 0`, but NOT the cache flushes. The narrow arm in
particular is only proven correct by hardware.

### GX entries per 3D race frame

| | baseline | onesubmit | + kicks=1 | + kicks=2 | + narrow |
|---|---|---|---|---|---|
| steady race window, `[gxcost] ent/f` / `[gputl] ent` | 7.0 | 6.0 | 6.0 | 6.0 | 6.0 |
| `[stereo] gx/f` (before transfers) | 5.0 | 4.0-5.0 | 4.0 | 4.0 | 4.0 |
| P3D IRQs / frame | 3.0 | 2.0 | 2.0 | 2.0 | 2.0 |
| A/B leg race avg (incl. kicked frames), `ent/f` | 7.8 | 6.6 | 6.1 | (n/a) | 6.2 |
| A/B leg race avg, P3D / frame | 3.8 | 2.5 | 2.1 | | 2.2 |

The det=1 emulator race rarely kicks mid-body (1 chunk most frames). On hardware the shipped 3D
frame has 3 chunks (11 entries); onesubmit makes that 8 (each `l|r` pair becomes one `b`), and
onesubmit_kicks=1 about 6 (fills, head list, one `b`, the left-only tail, 2 transfers).

### CPU in the emulator (emulated µs per frame; Citra HLE service costs, NOT hardware)

A/B leg (`stereo.onesubmit_ab=1 stereo.onesubmit_kicks=1 gxcost=1 receipt_every=256`, det=1),
race portion, per arm:

| arm | ent/f | main qr+pcl+gfl+sfl | main sub n/us | ev busy us | ev sub n/us | oth gfl n/us |
|---|---|---|---|---|---|---|
| 0 baseline | 7.8 | 419 | 3.7 / 234 | 1256 | 1.1 / 71 | 2.2 / 140 |
| 1 onesubmit | 6.6 | 440 (+21) | 3.4 / 216 | 1021 (-235) | 0.1 / 7 | 1.9 / 121 |
| 2 +kicks | 6.1 | 349 (-70) | 3.0 / 191 | 1024 (-232) | 0.1 / 6 | 2.0 / 126 |
| 3 +narrow | 6.2 | 364 (-55) | 3.2 / 197 | 1072 (-184) | 0.0 / 2 | 1.1 / 71 (whole-heap flush 16 MB/f -> ranges) |

Stereo receipt, steady single-chunk race: `enter_us` 349 -> 368 (+19: P's channel words, the arm
decision), `end_us` 10 -> 48 (+38: R + E emission, the split, the 160-byte range flush). The
removed right-eye entry's submit moves off the event thread (`ev sub` 1.1 -> 0.1 per frame).
`onesubmit_kicks=2` drops the head run's `gxCmdQueueRun` from main (`enter_us` 368 -> 39) but that
submit then happens on the event thread (`ev sub` 0 -> 3/frame): it moves work, it does not remove it.

Non-det perf leg (same A/B, `mainprof=1 hwsite=1 hwsite_every=2 c3dprof=2 c3dprof_every=4`): the
`[gxcost]` per-arm pattern is the same; `[c5cat] wall` per arm (20.8 / 19.0 / 17.1 / 19.8 ms) is
within host noise (single windows 16.7-22.8 ms, host load average ~36-40 with 3 emulators). It is
not evidence either way. The sampled `[hwsite]` (51 lines) and `[c3ddiet]` profiles emitted.

Read: in emulated time onesubmit alone is a wash on the submitting thread (+21 µs) and saves the
event thread one wake+submit per chunk; with fewer kicks it is ~-70 µs on main. Emulator service
call costs (a GSP submit ~63 µs, a whole-heap flush ~63 µs) are not the hardware's, so the size of
the hardware effect is unknown until the card runs.

## Hardware result (card-6 `ba0692e`, Old 3DS + New 3DS)

The A/B card below ran on both consoles (`hwlogs/o3ds-card6/log-EK.txt`, ~36,300 frames / 10 min;
`hwlogs/n3ds-card6/log-EK.txt`, 2.5 min before the 1 MB filelog cap). Arms 0 baseline, 1 onesubmit,
2 + kicks, 3 + narrow, per `[cpucut]` block. Analysis: `scratchpad/onesub-hw/o3ds-card6-analysis.md`.

**Old 3DS: it helps; the gain is GPU-queue and GSP event-thread time, not draw CPU.**

| | baseline (0) | onesubmit (1) | + kicks (2) | + narrow (3) |
|---|---|---|---|---|
| 3D static menu, fps (15 blocks) | **40.2** (40.1-40.4) | **44.6** | 44.6 | **44.7-44.9** |
| 3D menu, frame-begin GPU wait `qw` ms/f | 7.66 | 5.76 | 5.73 | 5.65 |
| 1P 3D race (D = 1), GX entries / P3D IRQs per frame | 9 / 5 | 7 / 3 | 6 / 2 | 7 / 3 |
| race GX CPU (main + other + event thread) ms/f | 2.56 | -0.55 | -1.06 | -0.95 |
| race frame-begin GPU wait ms/f | 2.19 | -1.02 | **+1.00** | -0.86 |
| race event thread ms/f | 0.68 (0.4-1.0) | 0.23 (flat) | 0.19 | 0.23 |
| race critical-path headroom gained | | **~1.5 ms** | ~0 | **~1.8 ms** |
| race fps per full block | 56.4 / 59.3 | 59.7-59.8 | 59.8 | 59.8 |

- The menu draws the whole frame without a mid-frame kick, so CPU and GPU run in series there:
  one GX entry and one completion IRQ fewer per chunk finish the previous frame's queue 1.9 ms
  sooner for the same GPU work. Per-entry dead time on the Old 3DS is at the high end of
  `[gputl] tiny`'s 0.4-1.3 ms.
- Race fps rests on two blocks per arm with traffic drifting, so it is suggestive; the GX receipts
  (entries, IRQs, event-thread time) do not depend on draw content and are many sigma apart.
- `onesubmit_kicks` saves the most CPU but removes the CPU/GPU overlap: the next frame starts by
  waiting for the whole body (+1.0 ms on the Old 3DS, +2.3-2.6 ms on the New 3DS). Not shipped.
- `onesubmit_narrow` (range flushes instead of the whole-heap `GSPGPU_FlushDataCache`) is the
  cheapest arm on both consoles (Old 3DS -0.25 ms vs arm 1). The log cannot see stale-cache
  garbage; the tester confirmed no visual glitch in 3D in any arm, arm 3 included.
- `[stereo] onesub fb=0` in every steady menu and race window (fallbacks only across transitions);
  no `vfy`/`bad=`, no fatal, no `gxCmdQueueWait` stall.
- MP 3D races: no per-arm data (the filelog's general budget cut the receipts at the first MP launch).

**New 3DS: neutral.** 60 fps in every window of every arm; `qw` ~1 us except arm 2 (2.5 ms);
entries 11 -> 8 / 6 / 8; GX CPU -0.03 / -0.38 / -0.42 ms; render task +0.30 / +0.92 / -0.33 ms.

**Shipped (ship6):** `[stereo] onesubmit = 1`, `onesubmit_narrow = 1`, `onesubmit_kicks = 0`,
`onesubmit_ab = 0` for both models: code defaults (`gdx3ds_stereo_frame.cpp`) and the
`GDX3DS_PRESET_CARD_ROWS` of both auto presets (`gdx3ds_preset_n60.c`, guarded by
`gdx3ds_preset_mustship_tests`). `[debug] gxcost` stays 0. Every key is still an ini lever:
`onesubmit = 0` restores the re-submitted replay, `onesubmit_narrow = 0` the whole-heap flush.
Emulator re-check of the shipped defaults (Old 3DS model, O3DS-EK build, `--3d side-by-side`,
det = 1, no stereo ini keys) against `ba0692e` with `stereo.onesubmit=1 stereo.onesubmit_narrow=1`:
`race-mutecity-start` 3027/3027 `[fh]` frames identical in both eyes, 6003 `[gstate]` identical;
`race-gp-cup` (six Jack cup courses + the ending) 118/118 L and R identical.

## What the hardware A/B tells (written before the card ran)

One combined Old 3DS card, arms cycling automatically per `[cpucut]` block (512 frames on card5,
8-block cycle = 4096 frames, ~2.3 min at 30 fps). Play a 3D race for at least two cycles (5 min).
Compare per arm:

- `[c5cat] wall / fend / tail` per block: the answer (Old 3DS 3D is CPU-bound, so wall drops only if
  the CPU drops).
- `[gxcost] ent/f`, `p3d=`: confirm 11 -> ~8 (arm 1) -> ~6 (arm 2).
- `[gxcost] ev us/f` and `ev wake/f`: the event-thread time per removed entry, the cost the
  commenter's suggestion targets. Still excludes kernel IRQ entry; compare `[c5cat]` totals.
- `[gxcost] main/oth gfl`: arm 3's whole-heap flush removal (r60probe: 113-116 µs per
  `GSPGPU_FlushDataCache` of >=64 KB; the IPC alone is ~55 µs).
- Visual check on hardware: arms 1-3 must look identical to arm 0 in 3D (cache-flush correctness
  is the one thing the emulator could not check). Any garbage in a block = that arm's flush is wrong.
- `[stereo] onesub fb=` stays ~0 (no fallbacks).

Ini snippet for the card (card5 already has `mainprof = 1`, `receipt_every = 512`):

```ini
[stereo]
onesubmit_ab = 1
onesubmit_kicks = 1

[debug]
gxcost = 1
hwsite = 1
hwsite_every = 4
c3dprof = 2
c3dprof_every = 8
```

`gputl = 1` adds the `[gputl] seq` view of the `b` entries at the price of its own callbacks; leave
it off for the cleanest CPU comparison (gxcost then registers its own GSP callbacks).

## Measured vs not

Measured (emulator): goldens identical in every arm and in 2D; GX entries and completion IRQs per
frame; where the dispatch work moves between threads; zero fallbacks on the race replay.

Not measured (before card-6; see "Hardware result" for what it answered): any hardware number. The real per-entry CPU (GSP IPC + IRQ + event thread on core 0),
whether the Old 3DS frame time moves, and whether the narrow flush is cache-correct. Prior hardware
data says each entry costs GPU start-up time (`[gputl] tiny` 0.2-1.3 ms per backlogged tiny entry)
and the GSP flush IPC ~55 µs; how much of a removed entry is CPU on core 0 is exactly what the A/B
card answers.

## Reply draft to the commenter

> Thanks, this was worth testing. A correction on the starting point: we do not encode the scene
> twice. The right eye already re-submits the left eye's command lists with only the framebuffer
> and one eye-offset uniform changed, so there is no second draw pass on the CPU. What was still
> per eye is the GX queue: each chunk of the frame went to the GPU as two `ProcessCommandList`
> entries, each with its own GSP submit and completion interrupt (11 entries per 3D race frame on
> the Old 3DS).
>
> Your idea fits that remaining cost, so I built it. Each chunk is now one entry that runs twice:
> the body ends in a `CMDBUF_JUMP1` to a short right-eye prologue that rebinds the framebuffer, sets
> the eye offset and jumps back into the same body on channel 0, then out to the end. No CPU work
> between eyes and one IRQ per chunk. In the emulator the frames are bit-identical to the old path
> in both eyes, and the entries go from 7 to 6 per race frame (11 to about 8 on hardware with three
> chunks, and about 6 if we also drop the mid-frame kicks).
>
> I don't have a hardware number yet. Emulator timing can't tell us what a GSP submit and IRQ really
> cost on core 0, so it goes into the next Old 3DS test card as an automatic A/B against the current
> path. I'll post the result either way.

## Files

- `port/3ds/gdx3ds_gxcost.c`, `port/3ds/include/gdx3ds_gxcost.h`, `port/3ds/CMakeLists.txt` (wraps)
- `port/3ds/gfx/gdx3ds_stereo_frame.cpp` (onesubmit, levers, receipts)
- `port/3ds/gfx/gdx3ds_overlap.c` (kicks policy, narrow flush)
- `port/3ds/gfx/gdx3ds_gputl.c` (`b` entries, gxcost forward), `gdx3ds_hwprof.c`,
  `gdx3ds_c3d_diet.c` (sampled modes), `gdx3ds_cpucut.c` (`gdx3ds_cpucut_block`)
- `port/3ds/gfx/STEREO.md` "One submission"
