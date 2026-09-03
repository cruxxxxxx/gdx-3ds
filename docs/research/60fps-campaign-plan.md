# 60fps Campaign Plan — shift-by-shift (agent K, 2026-08-14)

Target: rock-solid 60 fps F-Zero X on New3DS (804 MHz + L2, core 2 free under Luma,
core 3 QTM-forbidden), with enough banked GPU headroom to later fund stereoscopic 3D
(~2x GPU cost, exclusive with 800px mode — stereo3d-research.md).

Inputs: docs/research/60fps-research.md, stereo3d-research.md, m1-boot-debug.md (all
shifts), 3ds-memory-budget.md, patch-consolidation-plan.md, port/3ds/gfx/STATUS.md,
plus code verification in this worktree (gfx_citro3d.cpp, gdx_frame_pacer.h,
gdx_interp.h, gdx_fiber.h, gdx3ds_audio_ndsp.c).

## Ground rules (read before executing any shift)

1. **Every fps number in the record so far is an Azahar proxy.** Azahar quantizes to
   vblank divisors (60/2 = 30, 60/3 = 20 — observed exactly in T-TEXCACHE), the pinned
   `log_filter *:Debug` costs real host time once any per-frame svc traffic exists
   (noted explicitly in the 8fps shift), and Azahar emulates the ARM11 with a JIT whose
   cost profile is unrelated to a real 268/804 MHz ARM11 MPCore. Azahar numbers are
   valid for **A/B deltas of CPU-side work under identical scripts** (the texcache A/B
   methodology) and for **counter telemetry** (draws/tris/texUp/heap). They say nothing
   trustworthy about GPU fill rate, memory bandwidth, or the absolute CPU budget.
2. Therefore: **S1 (hardware baseline) executes the moment a New3DS is available**, and
   every subsequent shift re-verifies its win on hardware before it is banked. A shift
   verified only in Azahar is "provisionally landed", not done.
3. The deterministic tick script (`gdx-autoinput.txt`, SHOT labels) is the measurement
   harness for every A/B. Frame-matched windows, identical scripts, per the T-TEXCACHE
   table — that discipline is what made the texcache win provable. Keep it.
4. All fps claims below are stated as ms/frame where possible. 60 fps = 16.67 ms;
   the budget conversation is "who owns which milliseconds of core 0 and of the GPU."

---

## The campaign

Order rationale in one line: **measure (S0) → anchor on hardware (S1) → spend the
cheap, high-confidence CPU wins (S2-S4) → make the native-60 decision with real data
(S5) → spend the expensive/architectural items the decision justifies (S6-S8) → polish
pacing (S9) → open the stereo front (S10).**

### S0 — GPU/fill-rate profiling shift (measurement first)

- **Goal:** split every frame into (a) CPU game logic, (b) CPU interpreter + bridge +
  repack, (c) GPU processing (vertex/command), (d) GPU drawing (fill/resolve), (e) idle/
  vblank wait. Sibling agent G is building this instrumentation NOW; this plan assumes
  its telemetry exists: per-frame C3D processing/drawing timing (the
  `C3D_GetProcessingTime`/`C3D_GetDrawingTime` family plus `gspWaitForP3D` wall deltas —
  60fps-research.md flags this exact methodology as an open question, so G's shift is
  also answering it), folded into the existing `[c3d]` line cadence.
- **Evidence basis:** 60fps-research caveat 1 — *no* surviving claim gives PICA
  profiling numbers; the SM64 AA finding proves fill rate *can* be the wall but not
  that it is *our* wall. Our backend already renders aliased 240x400 (gfx_citro3d.cpp:466,
  1275: "PICA has no MSAA") — the single lever that fixed SM64 is already in our
  baseline, so we genuinely do not know whether we are GPU-bound at all. Race telemetry
  (draws≈440, tris 250-700, texImp 100-200/frame) suggests a light GPU load and a heavy
  CPU load, but that is inference, not measurement.
- **Territory:** port/3ds/gfx/gfx_citro3d.cpp (frame timing), port/3ds/main_3ds.cpp
  (stage timer around the 8 frame-loop stages the watchdog already names),
  port/n64_gfx_bridge.cpp (gdx_gfx_run wall time). Read-only elsewhere.
- **Verification metric:** a per-frame `[c3d-time]` (or equivalent) line: procMs /
  drawMs / cpuLogicMs / cpuGfxMs / idleMs, sampled every 64th frame, stable across two
  identical scripted runs.
- **Expected win:** 0 fps. This shift buys the campaign's steering, not speed.
- **Dependencies:** none (in flight now).
- **Risk / kill criteria:** if the C3D timing APIs prove unreliable under Azahar
  (plausible — emulated GPU timestamps), the shift still lands the CPU-side stage
  timers and the GPU split is deferred to S1 hardware. Do NOT block S2-S4 on GPU
  numbers; they are CPU shifts.

### S1 — Hardware baseline (New3DS) — executes ASAP, then re-runs after every shift

- **Goal:** first real numbers: ms/frame in title / menus / race under the tick script
  on a New3DS (804 MHz+L2 via the existing `osSetSpeedupEnable(true)`), with S0
  telemetry mirrored to a file on SD (svc log does not exist on hardware — route
  `gdx_port_logf` to `sdmc:/3ds/gdiffuser/log.txt`, buffered).
- **Evidence basis:** ground rule 1. Also 60fps-research caveat 5: no source anywhere
  demonstrates 60Hz-logic N64 simulation fitting the N3DS budget — only hardware can
  answer the S5 gate.
- **Territory:** logging sink in main_3ds.cpp; the hardware stream's CIA install
  process (already exists — B-BRIDGE ops notes mention a sibling CIA install).
- **Verification metric:** the S0 five-way frame split, on hardware, for the three
  canonical scenes (title static / SELECT COURSE 3752-tri screen / mid-race), plus
  the known-geometry-missing caveat: race content is currently under-emitted
  (game-side chunk cull bug, B-BRIDGE §2b) — record it as a KNOWN LOW baseline and
  re-baseline when the cull fix lands, or the campaign will "lose" fps to a
  correctness fix.
- **Expected win:** 0 fps; converts every later "expected win" from guess to number.
- **Dependencies:** S0 telemetry; New3DS availability.
- **Risk / kill criteria:** none — this shift cannot fail, only be delayed. If
  hardware shows ≥ 60 fps already in-race (unlikely but possible given Azahar
  overhead), the campaign re-scopes immediately to soak/thermal verification + S10.

### S2 — Malloc-crawl elimination + memory steady-state

- **Goal:** kill the residual ~2 MB/1000-frames race-content heap crawl and prove a
  flat long-session heap (GP season length).
- **Evidence basis:** M1-MEMORY follow-ups — crawl observed with all census containers
  flat and no ≥256 KB allocations, i.e. sub-256 KB allocations off-census. Peak 47.3 MB
  vs ~87 MB ceiling is fine *now*, but a crawl plus S6/S7 caches plus stereo's second
  target set erodes it; and on this fully-committed heap, allocation churn is also CPU
  (bad_alloc history proves the allocator is on hot paths).
- **Territory:** malloc-wrap histogram (main_3ds.cpp tracer extension), then whatever
  it names — suspects: per-frame std::vector churn in the bridge/adapter, LUS resource
  path temporaries, log formatting.
- **Verification metric:** `[mem-census]` delta per 1000 race frames → ~0; allocation
  count/frame during race (new counter) reduced to a stable small constant.
- **Expected win:** honest range 0-1 ms/frame CPU (churn removal), plus removal of a
  long-session OOM risk. This is partly a correctness shift wearing a perf badge.
- **Dependencies:** none. Can run before hardware.
- **Risk / kill criteria:** if the histogram shows the crawl is a genuine game-content
  working set (not churn), cap/accept it, document the ceiling math, and close the
  shift — do not build eviction machinery the budget doesn't demand (the
  gLoadedAssetSegments lesson: 1.6 MB bound killed that suspect statically).

### S3 — Compiler pass (LTO, -O3 audit, fast-math safety)

- **Goal:** establish and verify the aggressive-but-safe flag baseline for the whole
  .3dsx: audit current flags, trial `-O3` vs `-O2`, thin-LTO/full LTO, and
  `-ffast-math` (or the safer subset `-fno-math-errno -ffinite-math-only
  -fno-signed-zeros`) across decomp + port + LUS carve + backend.
- **Evidence basis:** 60fps-research finding: the shipped sm64-3ds baseline is
  `-march=armv6k -mtune=mpcore -mfloat-abi=hard -ffast-math -fomit-frame-pointer` —
  hard-float VFPv2 + fast-math is *proven shippable* for float-heavy N64 physics on
  this exact GCC/devkitARM target. Caveat honored: no measured LTO/-O3 deltas exist in
  the record, so this shift measures rather than assumes.
- **Territory:** port/3ds/CMakeLists.txt + top-level toolchain flags (devkitPro
  3DS.cmake defaults), per-target overrides. No game-code edits.
- **Verification metric:** frame-matched scripted A/B (Azahar for CPU delta direction,
  hardware for the real number): cpuLogicMs + cpuGfxMs. Correctness gates: harness
  36/36 pixel checks, a full scripted GP race, audio A/B capture comparison (fast-math
  can perturb the HLE mixer — the sm64 precedent *shipped* inaccurate-math audio, so
  drift is acceptable only if inaudible; use the existing gdx_audio_capture A/B).
- **Expected win:** honest range 5-15% CPU frame time (typical for -O2→-O3+LTO on
  ARM11-class in-order cores with this much C); could be near-zero if already -O3.
- **Dependencies:** S0 CPU timers (to measure); nothing else. Cheap; schedule early.
- **Risk / kill criteria:** physics divergence under fast-math (replay/ghost drift,
  rank changes in the scripted race) ⇒ fall back to the safe subset per-TU: fast-math
  on port/LUS/backend, precise on decomp physics TUs. If LTO breaks the devkitARM
  link or the fiber/asm seams, drop LTO — do not burn more than one shift here.

### S4 — Core-2 offload: audio HLE production (and the swizzle question)

- **Goal:** move the N64 audio HLE producer off core 0. Today only the tiny ndsp
  *drain* thread ladders to core 2 (gdx3ds_audio_ndsp.c core ladder); the actual HLE
  mixer work (VADPCM decode, envelopes, resample, mix — `port/gdx_audio_thread.cpp`
  producer + `port/n64_audio_hle.c`) runs on a default thread, i.e. core 0, stealing
  milliseconds from logic+interpreter every frame.
- **Evidence basis:** 60fps-research: sm64-3ds runs its audio thread on N3DS core 2 —
  the proven pattern; core 2 is freely usable under Luma (kernel cap auto-injected;
  threads must pass explicit core id 2); core 3 forbidden (QTM). m1-boot-debug: the
  accidental cxd4 LLE-on-by-default episode proved audio processing cost is large
  enough to move the needle in Azahar; HLE task-per-frame markers exist
  (`aud=enter/exit astage`).
- **Thread-safety analysis (required, first half of the shift):** the port already has
  the load-bearing invariants documented:
  - gdx_fiber.h: ALL fiber operations must happen on the scheduler-owning thread; the
    audio-thread affinity guard in n64_sched.c exists precisely because the audio
    thread must never touch fibers. Moving the producer to core 2 does not change
    which thread it is — the invariant is thread-identity, not core — so the fiber
    contract is unaffected *provided* the producer thread object itself migrates (use
    libctru threadCreate with core 2 + the existing ladder, replacing std::thread on
    3DS).
  - M1-RACE-FREEZE lock audit (negative result, already done): osSendMesg publishes
    queue data under gdx_mq_lock with the wake deferred to the host loop; the produce
    tick holds sAudioCtxMutex for the whole tick; no lock-order violation on the HLE
    path. That audit is the green light — cite it, re-verify the two mutexes'
    acquisition sites haven't grown, and go.
  - 3DS scheduler is strictly priority-preemptive with NO timeslicing (research
    finding): the core-2 producer must block on its event/mesg queue, never poll.
    The specwait-yield patch pattern is the model.
- **Texture-swizzle offload (second half, only if S0 shows swizzle time matters):**
  post-texcache, steady-state uploads are ~0-3/frame (residual genuine content
  changes) — the Morton swizzle is probably no longer worth a worker thread. Measure
  `UploadTexture` wall time first; if < 0.5 ms/frame steady-state, explicitly close
  this half as "moot after T-TEXCACHE" rather than building a cross-thread linearAlloc
  hand-off (which has real hazards: GSPGPU_FlushDataCache ordering, upload-before-draw
  fencing).
- **Territory:** port/gdx_audio_thread.cpp (3DS thread creation), port/3ds/audio/*,
  n64_sched.c affinity guard re-check. No LUS patch expected.
- **Verification metric:** core-0 cpu frame time drops by the measured audio-tick
  cost; watchdog `aud` cadence unchanged; zero underruns (`underrun_chunks` export);
  audio A/B capture identical.
- **Expected win:** the audio tick's full cost off the critical core — honest range
  1-3 ms/frame on hardware (unknown until S1 measures the HLE tick; the sm64 lineage
  spent multiple optimization passes here, so it is not trivial work even offloaded).
- **Dependencies:** S0 (to size the win), ideally S1 (real cost). Precedes S6 (DSP)
  because it is strictly cheaper and may make S6 unnecessary.
- **Risk / kill criteria:** if hardware shows the audio tick < 0.5 ms/frame, skip
  (and skip S6). If core-2 threadCreate fails on target setups (non-Luma), the ladder
  already degrades to core 1/0 — ship the ladder, not an assumption.

### S5 — DECISION GATE: native 60Hz logic vs 30Hz logic + interpolation

- **Goal:** decide, with hardware numbers, whether F-Zero X's native 60Hz simulation
  (30-machine physics + DL interpretation, every frame, on core 0) fits ~16.7 ms, or
  whether the port adopts the SM64-shape architecture (30Hz logic, two render
  submissions per tick with interpolated matrices).
- **Evidence basis:** 60fps-research is explicit: the SM64 precedent bounds
  *render-side* feasibility only; F-Zero X's 60Hz logic budget "is unproven by any
  source in this record" (caveat 5, open question 3). This is the single biggest
  unknown in the campaign and nothing but hardware measurement answers it.
- **The measurement that decides it:** on New3DS, after S2-S4 have landed, take the
  S0 split during full-field race content (post chunk-cull fix, 30 machines):
  `cpuLogicMs + cpuGfxMs` per frame, p95 over a full race.
  - p95 ≤ ~14 ms → **native 60 stands** (2.7 ms slack for pacing jitter + audio
    notify + present). Campaign continues on S7/S9.
  - p95 > ~18 ms → **30Hz+interpolation retrofit** becomes the mainline (S8).
  - between → spend S7 (interpreter reduction) first, then re-gate once.
- **What a retrofit would cost (honest):** far less than SM64's ~20-file patch,
  because the port ALREADY ships the hard parts for desktop high-fps interpolation:
  `port/gdx_interp.{h,cpp}` — GfxPool double-buffer matrix lerp via G_MTX scratch-slot
  indirection, byte-transparent t=1 contract, teleport snap rules, and P3's
  discontinuity-cut epoch with decomp shims (racer.c, camera.c) already in place; plus
  a host-side sub-frame replay loop (P2). The retrofit is "run logic every OTHER
  vblank, present the lerp at t=0.5 on the off tick" — reusing LerpMtx + the replay
  machinery in the opposite direction. Remaining real costs: HUD/2D elements and
  non-matrix animation (vertex-level effects, texture scroll) that SM64 interpolated
  across ~20 files; and F-Zero X-specific 60Hz couplings (input sampling feel, physics
  determinism at 30Hz — the N64 game *is* 60Hz, so halving logic rate changes game
  feel and is a last resort, not a free trick). Estimate: 2-4 shifts, not 8.
- **Verification metric:** the gate numbers above; if retrofit chosen, a scripted
  race at stable 60 fps presentation with logic at 30Hz and no visible streaking on
  cuts (P3 epochs).
- **Dependencies:** S1 hardware, S2-S4 landed, race geometry emission fixed (the
  chunk-cull bug — without full track + 29 rivals the logic/interpreter cost measured
  is a fiction).
- **Risk / kill criteria:** this shift cannot "fail" — it is a fork in the road. The
  kill criterion it enforces on OTHERS: if native-60 passes the gate, S8 is cancelled
  outright; do not build interpolation "just in case."

### S6 — DSP voice offload for audio resampling/mixing (conditional)

- **Goal:** route per-voice resampling + final mix onto the Teak DSP via ndsp: up to
  24 voices with per-voice `ndspChnSetRate` polyphase/linear resampling, DSP-side
  gain/mix, freeing the ARM11 of the resample+mix stage.
- **Evidence basis:** 60fps-research findings — 24 individually configurable DSP
  voices, per-voice rate multiplier + interpolation mode; **limitation stated
  verbatim: VADPCM decode, ADSR envelopes, and N64-style reverb are NOT offloadable
  to stock DSP firmware** — those stay on the CPU regardless. teak-llvm custom DSP
  programs exist but are "janky, near-single-person userbase" — out of scope.
- **Honest scope of the ndsp rewrite:** this is NOT a flag flip. Today the HLE
  produces one mixed 32 kHz stereo stream into a ring (channel 0). Per-voice offload
  means restructuring the HLE output stage: per-voice post-envelope PCM buffers, one
  ndsp channel each (≤ 24), DSP-side rate conversion and mixing, and re-implementing
  the N64 master-bus behavior (reverb send/return in particular) with ndsp's aux
  bus + biquad approximation — an *approximation*, which changes the sound. Multi-
  shift effort with audible-fidelity risk, saving only the resample+mix fraction of
  a tick that S4 already moved off core 0.
- **Verification metric:** core-2 (post-S4) audio tick ms; A/B capture vs HLE
  reference for audibility.
- **Expected win:** honest range: 30-60% of the audio tick, on a core that (after S4)
  is not the bottleneck core. **Default expectation: this shift is CUT.**
- **Dependencies:** S4 done; S1 numbers showing core-2 saturation or a need to run
  audio on core 0 after all.
- **Risk / kill criteria:** execute ONLY if S1+S4 show (a) the audio tick still
  contends with something that matters (e.g. core 2 also hosts S7 helpers), or (b)
  old3DS support is revived (it is formally dropped per the memory budget). Otherwise
  record as "evaluated, not needed."

### S7 — Interpreter/bridge cost reduction (DL-path CPU)

- **Goal:** cut the per-frame CPU cost of the Fast3D interpretation pipeline: the
  per-frame ProcessList walk (race: 52-150 lists, 1.2-3.2k commands), EnqueueList's
  terminator pre-scan + vector fill, the DrawTriangles repack (variable-stride →
  fixed 12-float layout, per draw, ~137-440 draws/frame), and per-frame re-conversion
  of static DLs.
- **Evidence basis:** T-TEXCACHE is the model and the proof this class pays: a
  cache-key fix + hash-cost reduction bought +10 fps in Azahar on frames with ZERO
  misses (pure CPU-cost removal). 60fps-research open question 2 names the sm64-port
  family precedent (DL caching/batching in later forks). Our own telemetry: menus
  draw 94-191 times/frame with ~200-700 tris — tiny GPU payloads, so per-draw CPU
  overhead dominates the gfx half.
- **Candidate items (measure, then pick):**
  1. Static-DL conversion cache: menu/HUD/course-card DLs are static (STATIC
     signatures in the [c3d] record); cache the converted command vectors / repacked
     VBO spans keyed on source identity + content epoch, invalidated by the existing
     DMA dirty-range machinery. The wide-conversion cache already exists as a seam
     (ConvertList) — extend rather than invent.
  2. Repack elimination: emit interpreter VBO data already in the backend's fixed
     layout (needs an interpreter patch — coordinate with patch consolidation, §
     below) or cache repacked spans for unchanged draws.
  3. Draw merging: consecutive draws sharing texture+state → one C3D_DrawArrays
     (batching precedent, sm64 forks).
  4. EnqueueList terminator-scan cost: the pre-scan re-reads every list every frame;
     a per-source memoization (source ptr + epoch → command count) is cheap.
- **Territory:** port/n64_gfx_bridge.cpp, port/n64_gfx_convert.cpp,
  port/3ds/gfx/gfx_citro3d.cpp; possibly libultraship interpreter.cpp (PATCH ALERT —
  the triple-patched file; see consolidation section).
- **Verification metric:** cpuGfxMs (S0 split) per frame, frame-matched A/B; target
  the SELECT COURSE 3752-tri screen and mid-race as the two canonical loads.
  Correctness gate: harness 36/36 + [race-dl] noop=miss=bad=0.
- **Expected win:** honest range 10-30% of cpuGfxMs; potentially the difference at
  the S5 gate. Wide error bars until S0 splits interpreter vs repack vs submit.
- **Dependencies:** S0 (to target the right sub-cost); benefits from S5's gate
  pressure (if the gate is marginal, this shift runs before re-gating).
- **Risk / kill criteria:** caching DLs that the game mutates in place is the classic
  correctness trap — every cache MUST key on the DMA dirty-range epoch machinery that
  already exists (the texcache staleness lesson: hash exactly what you read). If S0
  shows cpuGfxMs is already small vs cpuLogicMs, reorder: this shift yields to
  physics/game-side profiling (decomp-owned, new stream).

### S8 — 30Hz+interpolation retrofit (ONLY if the S5 gate fails)

- **Goal:** SM64-architecture presentation: logic every other vblank, interpolated
  render pass on off-ticks, C3D_FrameRate(60)-class pacing.
- **Evidence basis:** 60fps-research: verified SM64 mechanism (produce_one_frame +
  patch_interpolations + resubmit; C3D_FrameRate as pacing); our gdx_interp P1/P2/P3
  subsystem as the existing foundation (see S5 for the honest cost breakdown).
- **Territory:** port/gdx_interp.*, port/n64_gfx_bridge.cpp sub-frame replay,
  main_3ds.cpp frame loop cadence, P3 cut-shim coverage widening in decomp (PORT-gated
  one-liners per gdx_interp.h's design).
- **Verification metric:** presented 60 fps on hardware with logic at 30Hz; input
  latency measured (button-to-photon via SHOT captures) and compared against native
  path; no streaking across the P3 cut inventory (race start, respawn, transitions).
- **Expected win:** halves the logic budget requirement — the fallback that
  guarantees 60 fps presentation if simulation cannot fit.
- **Dependencies:** S5 gate result. Mutually exclusive with "native 60 stands."
- **Risk / kill criteria:** game-feel regression (this game is natively 60Hz; halving
  sim rate changes handling). If blind A/B testers can feel it, prefer shipping
  native-60 at a lower-but-stable rate target (e.g. locked 45-50 via pacer) over a
  mushy 60 — that judgment call goes to the orchestrator with the S8 data.

### S9 — Frame-pacing final polish

- **Goal:** one authoritative pacer. Today: `C3D_FrameBegin(C3D_FRAME_SYNCDRAW)` is
  the sole pacer on 3DS (the double-vblank bug fixed in the 8fps shift), while the
  port's own wall-clock pacer (`gdx_frame_pacer_tick`, 59.94 Hz NTSC field rate)
  also runs in the loop (main_3ds.cpp:540). Decide: C3D_FrameRate(60) vs SYNCDRAW vs
  the port pacer; eliminate double pacing; handle the 59.94-vs-60.0 beat (one dropped
  frame every ~16.7 s if both are active).
- **Evidence basis:** 60fps-research: C3D_FrameRate() is the *verified* sm64
  mechanism (the gspWaitForVBlank-gated claim was REFUTED — do not rebuild that).
  **P2's pacing verdict is pending and is an INPUT to this shift** — do not execute
  S9 until P2's conclusion (C3D_FrameRate vs the port pacer) is on file; this shift
  implements that verdict on 3DS and verifies it.
- **Territory:** main_3ds.cpp, gdx_frame_pacer.c (3DS branch), gfx_citro3d.cpp
  StartFrame.
- **Verification metric:** frame-time histogram on hardware (S0 idleMs + present
  timestamps): flat 16.67 ms line, zero double-waits, controlled judder under
  transient overload (the failure mode SYNCDRAW handles by halving — decide if 60→30
  snapping or tearing-free 45-ish via C3D_FrameRate is preferred under load).
- **Expected win:** 0 average fps; eliminates quantization cliffs (the difference
  between "mostly 60" and "solid 60").
- **Dependencies:** P2 verdict; runs late (after the frame cost is settled by
  S2-S8) because pacing polish before the budget is met is wasted twice.
- **Risk / kill criteria:** minimal; if C3D_FrameRate under Azahar behaves unlike
  hardware (likely), verify only on hardware.

### S10 — Stereo integration point (opens the stereo front)

- **Goal:** define and enforce the bar at which sibling agent S's stereo foundation
  is allowed to turn on, and land the stereo-specific perf items.
- **The mono fps bar (the gate):** stereo re-renders the scene per eye with no PICA
  multiview — ~2x GPU processing+drawing (stereo3d-research; cost model verified from
  code structure, not benchmarked — caveat 1). Therefore stereo work may begin
  when, on hardware, over a full race: **sustained 60 fps AND (GPU procMs + drawMs)
  p95 ≤ ~7 ms AND cpuGfxMs has ≥ 2 ms slack** (the second eye also costs CPU: per-eye
  submission, and in the sm64 precedent a full CPU vertex-buffer re-copy — a cost we
  will NOT pay, see below). Until those three numbers hold, stereo stays a foundation
  branch. Note stereo is hardware-exclusive with 800px wide mode — since our target
  is 400x240 native, no resolution decision is coupled here, but AA-equivalent
  post-processing (if S/G ever add a downscale path) must be disabled in stereo per
  the sm64 precedent.
- **Stereo-specific perf items (S's backlog, gated on the bar):**
  1. **Per-eye command-list reuse:** submit the SAME repacked VBO twice with only the
     projection/shear uniform changed between passes — explicitly avoiding the sm64
     re-copy (the "no re-upload" claim was REFUTED for sm64; ours must be true by
     construction: one FlushPendingVbo, two C3D_DrawArrays sequences).
  2. **HUD single-pass options:** F-Zero X's HUD is large (portraits, minimap, lap,
     rank, energy). s2DMode-style per-draw classing (the sm64 G_SPECIAL_1 pattern) at
     zero parallax, and evaluate compositing HUD once into both eyes via GX copy
     instead of drawing twice.
  3. Right-pass skip at slider 0 (free mono when the slider is down — the canonical
     pattern), so stereo cost is only paid when the user asks for it.
  4. VRAM/linear budget re-check: second color+depth target set (fits per
     3ds-memory-budget §2 VRAM math, but re-verify against G's final target layout,
     including the frame-mirror texture).
- **Evidence basis:** stereo3d-research (cost model, exclusivity, s2DMode precedent,
  per-eye pass structure, slider pattern); 3ds-memory-budget VRAM note ("stereo
  doubles color targets and still fits").
- **Verification metric:** with stereo on at full slider, hardware holds 60 fps in
  race; procMs+drawMs ≤ ~15 ms; harness scenes re-run per-eye (extend
  check_scene_bmps to both eyes).
- **Dependencies:** the mono bar (i.e., the whole campaign through S7/S9); S's
  foundation work proceeds in parallel but does not enable by default.
- **Risk / kill criteria:** if hardware shows mono GPU time is already > 8-9 ms after
  S2-S9, stereo at 60 is unaffordable — the honest fallbacks, in order: stereo at
  interpolated-30Hz-logic (if S8 happened anyway), reduced-cost right eye (shared
  depth tricks are NOT available on PICA — do not chase them), or stereo capped at
  30 fps presentation as a user choice. Decide with numbers, not hope.

### S11 — Fill-rate reduction (floating shift — scheduled by S0/S1 data)

Held out of the main sequence deliberately: **we do not yet know we are fill-bound**
(unlike SM64, we already render aliased 400x240; the fill-rate research lesson is
banked in the baseline). If S0/S1 telemetry shows drawMs is a material fraction of
16.7 ms, insert this shift immediately after S1:

- **Candidates, by expected bandwidth:** (a) the VI-mirror composite —
  GdxUpdateFrameMirror does a full-screen GX screen→texture copy EVERY task frame
  (fbBinds=2 in every [c3d] line) solely so transition captures have a source; gate
  it on transition-pending (the capture-ordering contract at n64_gfx_bridge.cpp:2567
  already defines when it is needed) — saves a 384 KB copy + bind churn per frame.
  Note: the in-game mirror readback is currently broken anyway (B-BRIDGE: scan BMPs
  all-black in-game) — fix and gate in the same shift. (b) framebuffer format:
  RGBA8+D24S8 → RGB565/D24 halves color resolve bandwidth if PICA blending precision
  allows (N64 content is RGBA16-native; test banding on gradients/fog). (c) the
  final display transfer format (already RGB8 out — cheap). (d) resolution modes:
  we are at native 400x240; there is no 800px cost to cut, and DO NOT add 800px —
  it forecloses stereo (hardware-exclusive).
- **Kill criterion (the one the prompt asks for):** if S0/S1 shows the GPU idle
  (drawMs+procMs ≪ frame time) — which current draw/tri counts suggest — this shift
  is MOOT; strike it and reinvest the shift into S7. The mirror-gating item survives
  regardless (it is also CPU/GX-queue cost), attached to S7 or the transition fix.

---

### S12 — Asset-cost reduction: ETC1 textures + geometry decimation (floating, data-gated)

> **PROVENANCE / CONFIDENCE CAVEAT — added 2026-08-15 by Opus 4.8, NOT Fable.** The
> rest of this plan (S0–S11, gates, kill-criteria) was authored by the Fable
> planning agent (K) from the verified research corpus. This S12/S13 block is a
> TENTATIVE idea sketched inline by the main-loop model at the user's request, with
> LESS rigor than the K shifts — it was not research-verified, not cross-checked
> against a profiling run, and not adversarially reviewed. **Before executing:
> re-derive it with a dedicated planning/research agent** (is ETC1 actually a net
> win given decode-in-prebake vs runtime? does F-Zero X geometry have enough fat to
> decimate? what's the quality floor?), confirm against S0/S1 telemetry, and
> elaborate the tooling. Treat the numbers here as order-of-magnitude guesses, not
> commitments.

Rationale: two asset-side levers the research/K-plan did not cover. Both are
**data-gated the same way as S11** — worthless if we are neither fill- nor
upload-bound, so they only schedule AFTER S0/S1 says which axis is starved.

- **(a) ETC1 texture conversion (higher-confidence of the two).** PICA200 samples
  ETC1 in hardware (~4–8× smaller in VRAM than RGBA16, native filtering). Convert
  textures to ETC1 in the PC-side prebake (tools/prebake) so the device loads
  pre-compressed data — cuts the decode cache, the per-upload Morton-swizzle cost
  (the T-TEXCACHE hot path), linearAlloc pressure (the ~87 MB ceiling episodes), AND
  fill bandwidth. Wins on BOTH the memory axis and (if upload/fill-bound) the perf
  axis; memory headroom is never wasted, so this is the closest thing to an
  always-safe asset win. OPEN QUESTIONS for the re-derivation: ETC1 is lossy + has
  no alpha (need ETC1+A4 or punchthrough path for N64 alpha textures — many F-Zero X
  textures use alpha); does the LUS interpreter's texture cache key survive a format
  swap; can the citro3d backend bind ETC1 without a decode step; is the quality loss
  acceptable on gradient/HUD art.
- **(b) Geometry decimation (lower-confidence — probably low ROI).** Reduces vertex
  count → less CPU transform in the Fast3D per-vertex path → helps ONLY if
  CPU/interpreter-bound (see S0). Big caveat: it is already an N64 game (races
  ~250–700 tris, peaks ~3700; TMEM-tiny assets; the game does its own distance LOD
  and culling), so there is little fat to cut, and models are baked into display
  lists inside the .o2r — decimating means a prebake pass regenerating simplified
  DLs (real tooling). Expected to be dominated by the blunter lever below.
- **Blunter CPU lever if S5/S0 shows we are logic/draw-count-bound, not per-model
  bound:** draw FEWER things — reduced rival machine count (30→N) and/or tighter
  cull distance. Ugly and gameplay-affecting (make it a menu option, default off),
  but it attacks physics + draw-submission cost directly, which is the axis the N64
  itself struggled with at 30 machines. List it here so it is not forgotten; gate it
  behind the S5 native-60 decision.

- **Measurement to add (the capture the user asked for):** extend agent G's S0
  telemetry with per-frame **texture bytes uploaded** and **unique-texture count**
  (the swizzle/upload cost proxy) and **vertex count transformed** (the decimation
  proxy) — alongside the existing draws/tris. Without those two columns we cannot
  tell whether S12(a) or S12(b) would pay. Fold this column request into S0/G's
  charter when the gpuprof branch is picked back up (task #24).
- **Kill criteria:** ETC1 struck if S0 shows texture upload+fill are both trivial
  fractions AND memory has comfortable headroom post-S2. Decimation struck if GPU is
  idle OR the CPU cost is dominated by physics/audio rather than vertex transform
  (S0 vertex-count column decides).

---

## Patch-consolidation coupling (READ BEFORE the fork-branch conversion executes)

patch-consolidation-plan.md freezes an inventory of exactly 9 working-tree patches
and a byte-verified zero-drift baseline (§0), with commit sequences for the two
`3ds-port` fork branches (§3). **Agents G and S are producing new submodule changes
NOW that will invalidate that inventory if the conversion executes as written.**

Expected new LUS-submodule material:

- **Agent G (GPU/fill-rate instrumentation):** likely backend-only (gfx_citro3d.cpp,
  main_3ds.cpp — repo-side, no patch), BUT any per-frame timing hooks placed inside
  the interpreter (e.g. around ImportTexture / draw dispatch) land in
  **interpreter.cpp — the triple-patched file** (consolidation §4.5). If G ships an
  interpreter hunk, it must be delivered as a 10th patch file with a README section,
  not as loose submodule WIP.
- **Agent S (stereo foundation):** the endorsed architecture (stereo3d-research: the
  sm64 author's own TODO) intercepts perspective matrices in the interpreter
  (gfx_pc-level, before mv*p) — an interpreter.cpp patch by construction — plus a
  probable G_SPECIAL_1/s2DMode-style 2D-classing dispatch (interpreter) and decomp-side
  tag call sites (HUD/menu draw code). Expect **1-2 new LUS patches and possibly 1
  decomp patch**.

Instructions to the orchestrator (binding for this campaign):

1. Before executing consolidation §3, re-run the §0 drift check and **amend the plan's
   §1 audit + §2 classification + §3 commit lists with every G/S patch** (each as its
   own fork commit, same one-commit-per-patch rule; S's interpreter patches join
   texcache in the "high upstream-conflict, independently revertable" class).
2. Any G/S work still in flight at conversion time follows the §3 worktree-migration
   step-0 WIP-capture rule — their worktrees' submodule diffs will EXCEED the 9-patch
   sum by design; capture and diff against the canonical sum before any
   `checkout -- .`.
3. S7 (interpreter cost reduction) may add further interpreter patches later — after
   conversion those become fork commits directly on `3ds-port`; before conversion they
   must join the patch README inventory. Either way, no loose submodule edits.

---

## Kill-criteria summary (stop-and-rethink triggers)

| Shift | Kill / reorder trigger |
|---|---|
| S0 | C3D GPU timers unreliable in Azahar → land CPU timers, defer GPU split to S1 (do not block S2-S4) |
| S1 | none (cannot fail); ≥60 fps in-race on hardware → re-scope campaign to soak + S10 |
| S2 | crawl proves to be genuine working set, not churn → cap, document, close; build no eviction machinery |
| S3 | physics/audio divergence under fast-math → per-TU safe subset; LTO link breakage → drop LTO; max 1 shift |
| S4 | hardware audio tick < 0.5 ms → skip S4 remainder AND S6; swizzle half moot if UploadTexture < 0.5 ms steady |
| S5 | it's a gate, not a shift: pass → cancel S8; fail → S8 mainline; marginal → S7 then re-gate ONCE |
| S6 | default CUT unless S1+S4 show core contention that per-voice DSP offload uniquely relieves |
| S7 | S0 shows cpuGfxMs ≪ cpuLogicMs → yield to game-logic profiling stream; any cache without dirty-epoch keying is auto-reject |
| S8 | perceptible game-feel regression at 30Hz logic → escalate native-60-at-lower-lock option |
| S9 | do not start before P2's pacing verdict is filed |
| S10 | mono GPU > ~8-9 ms after campaign → stereo-at-30 / reduced-eye fallback decision, with numbers |
| S11 | **GPU idle in S0/S1 telemetry → fill-rate shift is moot; strike it, keep only the mirror gating (as CPU work)** |
| S12 | ETC1 struck if upload+fill trivial AND memory has headroom; decimation struck if GPU idle or CPU is physics/audio-bound not vertex-bound (Opus-authored, re-derive first) |

---

## Shift table

| # | Shift | Expected win (honest) | Verification metric | Depends on |
|---|---|---|---|---|
| S0 | GPU/CPU frame-split telemetry (agent G) | 0 (steering) | stable per-frame proc/draw/logic/gfx/idle ms split | — |
| S1 | Hardware baseline (New3DS) + re-verify loop | 0 (truth) | S0 split on hardware, 3 canonical scenes | S0, N3DS |
| S2 | Malloc-crawl elimination | 0-1 ms CPU + OOM-proofing | heap delta/1000 race frames → ~0; allocs/frame | — |
| S3 | Compiler pass (-O3/LTO/fast-math audit) | 5-15% CPU frame time | frame-matched A/B cpu ms; harness 36/36; audio A/B | S0 |
| S4 | Core-2 audio-HLE offload (+swizzle check) | 1-3 ms off core 0 | core-0 frame ms drop = audio tick; 0 underruns | S0 (S1 to size) |
| S5 | GATE: native-60 vs 30Hz+interp | decision | p95 cpuLogic+cpuGfx: ≤14 native / >18 retrofit | S1-S4 + cull fix |
| S6 | DSP voice offload (conditional, default cut) | 30-60% of audio tick, off-critical-core | core-2 tick ms; audio A/B audibility | S4, S1 |
| S7 | Interpreter/DL-path CPU reduction | 10-30% of cpuGfxMs | cpuGfxMs A/B on SELECT COURSE + race; [race-dl] clean | S0 |
| S8 | 30Hz+interp retrofit (only if S5 fails) | halves logic budget need | 60 fps presented @30Hz logic; latency + cut checks | S5 fail |
| S9 | Frame-pacing final polish | 0 avg; kills quantization cliffs | hardware frame-time histogram flat at 16.67 ms | P2 verdict, S2-S8 |
| S10 | Stereo integration gate + per-eye reuse | funds stereo | 60 fps stereo @ full slider; GPU ≤ ~15 ms | mono bar met, agent S |
| S11 | Fill-rate reduction (floating) | unknown until S0; possibly moot | drawMs A/B; mirror copy gated | S0/S1 data |
| S12 | Asset-cost: ETC1 textures + geo decimation (floating, **Opus-sketched, re-derive**) | ETC1: memory + maybe upload/fill; decimation: low | texBytes/uniqueTex/vtx-count columns in S0; A/B | S0/S1 + new telemetry cols |

## Top-3 highest-confidence wins

1. **S4 core-2 audio-HLE offload** — the proven sm64 pattern (research: high-confidence,
   3-0 verified), the port's own thread/lock audit is already done and clean, the drain
   ladder to core 2 already exists as a template, and the LLE-accident episode proved
   audio cost moves the needle. Lowest risk-to-win ratio in the campaign.
2. **S7 interpreter/DL-path CPU reduction** — T-TEXCACHE is the in-repo proof that this
   class pays double-digit percentages (+10 fps in Azahar from cache-key + hash-cost
   fixes alone), and the draw/tri telemetry shows small GPU payloads behind heavy
   per-frame CPU walks — exactly the shape DL/conversion caching attacks.
3. **S3 compiler pass** — the sm64 baseline proves hard-float+fast-math ships on this
   exact toolchain and codebase family; one bounded shift, whole-binary leverage,
   frame-matched A/B makes the result unambiguous, and the fallback (safe subset,
   no LTO) is free.
