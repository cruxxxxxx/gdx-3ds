# port/3ds/patches — decomp working-tree patches

Submodule changes are never committed inside the submodules (plan rule); they live here
as patch files applied to the working tree, and land in the respective forks at
integration sign-off.

**libultraship is no longer patched here.** Its 3DS carve is vendored at
`port/3ds/vendor/lus/` with all 41 `lus-*.patch` already folded in, so the 3DS build needs
neither the submodule nor the patch step. Those 41 patch files moved to
`port/3ds/vendor/lus/patches/` (frozen provenance; apply order in `ORDER.txt`) and their
per-patch write-ups moved with them into `port/3ds/vendor/lus/patches/README.md`. Start at
`port/3ds/vendor/lus/PROVENANCE.md`.

Apply after `git submodule update --init decomp torch` (add `fzerox-expansion-kit` too if
you're configuring an Expansion Kit target -- see `.gitmodules`):

```sh
git -C decomp apply ../port/3ds/patches/decomp-ilp32.patch
git -C decomp apply ../port/3ds/patches/decomp-port-segment-bzero.patch
git -C decomp apply ../port/3ds/patches/decomp-3ds-dma-low-address.patch
git -C decomp apply ../port/3ds/patches/decomp-port-audio-specwait-yield.patch
git -C decomp apply ../port/3ds/patches/decomp-race-cull-diagnostics.patch
git -C decomp apply ../port/3ds/patches/decomp-3ds-sky-overscan-deadend-note.patch
git -C decomp apply ../port/3ds/patches/decomp-3ds-machineselect-gradient-coalesce.patch
git -C decomp apply ../port/3ds/patches/decomp-3ds-rom-audio-port.patch
git -C decomp apply ../port/3ds/patches/decomp-port-course-select-state-reset.patch
git -C decomp apply ../port/3ds/patches/decomp-port-rival-detail.patch
git -C decomp apply ../port/3ds/patches/decomp-native-track-shim.patch
git -C decomp apply ../port/3ds/patches/decomp-port-determinism-pins.patch
git -C decomp apply ../port/3ds/patches/decomp-native-machines-rebuilt.patch
git -C decomp apply ../port/3ds/patches/decomp-port-title-demo-audio-pin.patch
git -C decomp apply ../port/3ds/patches/decomp-native-fx-shim.patch
git -C decomp apply ../port/3ds/patches/decomp-3ds-mainprof-checkpoints.patch
git -C decomp apply ../port/3ds/patches/decomp-3ds-disk-audio-ilp32.patch
git -C decomp apply ../port/3ds/patches/decomp-3ds-disk-audio-heap-ilp32.patch
git -C decomp apply ../port/3ds/patches/decomp-3ds-disk-audio-deadlock.patch
git -C decomp apply ../port/3ds/patches/decomp-3ds-rdram-size-knob.patch
git -C decomp apply ../port/3ds/patches/decomp-3ds-disk-residency-seam.patch
git -C decomp apply ../port/3ds/patches/decomp-3ds-disk-audio-async-guards.patch
git -C decomp apply ../port/3ds/patches/decomp-3ds-ek-diskcourse-decline.patch
git -C decomp apply ../port/3ds/patches/decomp-3ds-disk-audio-load-census.patch
git -C decomp apply ../port/3ds/patches/decomp-port-o3ds-scene-lod.patch
git -C decomp apply ../port/3ds/patches/decomp-3ds-c5cat-markers.patch
git -C decomp apply ../port/3ds/patches/decomp-native-hud-capture.patch
git -C decomp apply ../port/3ds/patches/decomp-native-track-resident-bake.patch
git -C decomp apply ../port/3ds/patches/decomp-native-sky-capture.patch
git -C decomp apply ../port/3ds/patches/decomp-3ds-c5tick-dlb.patch
git -C decomp apply ../port/3ds/patches/decomp-3ds-c5sub-markers.patch
git -C decomp apply ../port/3ds/patches/decomp-native-dress-shim.patch
git -C decomp apply ../port/3ds/patches/decomp-3ds-nrr-mach.patch
git -C decomp apply ../port/3ds/patches/decomp-native-nrr-trk.patch
git -C decomp apply ../port/3ds/patches/decomp-3ds-ships-census.patch
git -C decomp apply ../port/3ds/patches/decomp-3ds-autodrive.patch
git -C decomp apply ../port/3ds/patches/decomp-3ds-prewarm.patch
git -C decomp apply ../port/3ds/patches/decomp-native-finish-direct.patch
git -C decomp apply ../port/3ds/patches/decomp-native-finish-mesh.patch
git -C decomp apply ../port/3ds/patches/decomp-3ds-hud-placement.patch
git -C decomp apply ../port/3ds/patches/decomp-native-hud-lap-announce.patch
git -C decomp apply ../port/3ds/patches/decomp-native-force-lap2.patch
git -C decomp apply ../port/3ds/patches/decomp-native-menu-capture.patch
git -C decomp apply ../port/3ds/patches/decomp-native-force-boot-mode.patch
git -C decomp apply ../port/3ds/patches/decomp-native-menu-trans-capture.patch
git -C decomp apply ../port/3ds/patches/decomp-native-gp-end-setup.patch
git -C decomp apply ../port/3ds/patches/decomp-native-gp-end-mach.patch
git -C decomp apply ../port/3ds/patches/decomp-3ds-nimach.patch
git -C decomp apply ../port/3ds/patches/decomp-port-title-demo-tick.patch
git -C decomp apply ../port/3ds/patches/decomp-native-records-scene.patch
git -C decomp apply ../port/3ds/patches/decomp-native-course-preview.patch
git -C decomp apply ../port/3ds/patches/decomp-native-hud-elem-gate.patch
git -C decomp apply ../port/3ds/patches/decomp-3ds-ek-audio-disk-async.patch
git -C decomp apply ../port/3ds/patches/decomp-3ds-ek-audio-sync-bulk.patch
git -C decomp apply ../port/3ds/patches/decomp-3ds-ek-testdrive-scope.patch
git -C decomp apply ../port/3ds/patches/decomp-3ds-ek-editmesh.patch
git -C decomp apply ../port/3ds/patches/decomp-3ds-ek-nimach.patch
git -C decomp apply ../port/3ds/patches/decomp-native-menu-xition.patch
git -C decomp apply ../port/3ds/patches/decomp-3ds-mp-online.patch
git -C decomp apply ../port/3ds/patches/decomp-3ds-mp-sim.patch
git -C decomp apply ../port/3ds/patches/decomp-3ds-mp-fullview.patch
git -C decomp apply ../port/3ds/patches/decomp-3ds-widescreen-cull.patch
git -C decomp apply ../port/3ds/patches/decomp-3ds-ek-nodisk-audio.patch
git -C decomp apply ../port/3ds/patches/decomp-3ds-ek-vs-onepad.patch
git -C decomp apply ../port/3ds/patches/decomp-3ds-settings-backdrop-full.patch
git -C decomp apply ../port/3ds/patches/decomp-3ds-mp-vis.patch
git -C decomp apply ../port/3ds/patches/decomp-3ds-mp-ekhold.patch
git -C decomp apply ../port/3ds/patches/decomp-3ds-mp-racers.patch
git -C decomp apply ../port/3ds/patches/decomp-3ds-mp-dropcpu.patch
git -C decomp apply ../port/3ds/patches/decomp-3ds-mp-rambot.patch
git -C decomp apply ../port/3ds/patches/decomp-3ds-mp-grid.patch
git -C decomp apply ../port/3ds/patches/decomp-3ds-bui-title-demo-hold.patch
git -C decomp apply ../port/3ds/patches/decomp-3ds-mp-stockpick.patch
git -C decomp apply ../port/3ds/patches/decomp-3ds-font-nullread.patch
git -C decomp apply ../port/3ds/patches/decomp-3ds-mp-vs-overlay-bounds.patch
git -C decomp apply ../port/3ds/patches/decomp-3ds-o3ds60-mp-hud.patch
git -C decomp apply ../port/3ds/patches/decomp-3ds-mp-polish.patch
git -C decomp apply ../port/3ds/patches/decomp-3ds-mp-vs-menu.patch
git -C decomp apply ../port/3ds/patches/decomp-3ds-unlock-3ds-code.patch
git -C decomp apply ../port/3ds/patches/decomp-3ds-stereo-sky-margin.patch
```

This list is kept in the same order as `tools/ci-3ds.sh`'s `PATCH_LIST`, which is the
source of truth; `tools/ci-3ds.sh` applies exactly this set, idempotently, and FAILS if a
`*.patch` turns up in this directory that `PATCH_LIST` does not know about.

## decomp-port-segment-bzero.patch (M1 boot debug)

`#ifndef PORT` guards on the two raw `bzero(SEGMENT_BSS_START(x), SEGMENT_BSS_SIZE(x))`
calls (`src/sys/sys_main.c` leo, `src/sys/rom/disk_drive.c` ovl_i11). Under PORT the
overlay BSS markers are 1-byte stubs in `port/gen/LinkStubs.c` that the linker places
arbitrarily; on the 3DS link `leo_BSS_END` (0x009C4070) lands BELOW `leo_BSS_START`
(0x009D1160), so the size goes negative (≈4 GB as `size_t`) and the bzero zeroes the
tail of the host `.bss` before spinning on unmapped writes at the image end — the M1
black-screen boot hang (docs/research/m1-boot-debug.md). Every other segment-BSS
consumer (`Dma_LoadOverlay`, `DiskDrive_LoadOverlay*`, `Dma_ClearRomCopy`,
`func_800742FC`) was already a PORT no-op or guarded; these two were only compiled
with `EXPANSION_KIT` off, which desktop never builds.


## decomp-ilp32.patch (M1 integration; stream E's blockers #1 and #2)

Prerequisites for ANY 32-bit (3DS/devkitARM) build of the decomp — see
docs/research/32bit-sweep.md "Deferred / filed upstream" items 1-4:

- **include/libc/stdint.h** — proper ILP32 branch: `intptr_t`/`uintptr_t` spelled
  `int`/`unsigned int` exactly like newlib (`__INTPTR_TYPE__`), and 32-bit
  `INTPTR_MIN/MAX`, `UINTPTR_MAX`. Previously every non-LP64 host fell into the
  LLP64 branch and got a 64-bit `uintptr_t` (truncating casts everywhere + hard
  conflicting-typedef errors against newlib's `<stdint.h>`).
- **include/libc/stddef.h / stdlib.h** — `ptrdiff_t` (`int`) and `wchar_t`
  (`unsigned int`) spelled to match newlib/GCC on ILP32; `long`-based spellings are
  size-identical but type-distinct and hard-error when a TU sees both headers.
- **include/PR/os_time.h** — on `GDX_PLATFORM_3DS`, `osGetTime`/`osSetTime` are
  aliased to `gdx3ds_osGetTime`/`gdx3ds_osSetTime`: libctru's os.o also exports an
  `osGetTime` (ms since 2000, unrelated semantics) and is always pulled into the link,
  so the port shim (port/3ds/lus_glue/gdx3ds_libultra.cpp) cannot define the colliding
  name. Desktop builds see the original names.
- **include/PR/gbi.h** —
  1. `_GFXW1_PTR` tags host pointers with `GDX_GFXW1_HOST_TAG` ("GDX1") in the spare
     high32 of the u64 `GfxW1` on 32-bit builds: a 32-bit host pointer is otherwise
     value-indistinguishable from a segment token (3DS heap 0x08xxxxxx == segment 8;
     static data 0x00xxxxxx == segment 0). port/n64_gfx_bridge.cpp's ProcessList
     recognizes exactly this tag as its host-pointer fast path (kGfxW1HostTag32 —
     keep the constants in sync).
  2. Static `gs*` initializer forms rewritten onto `_GFX_STATIC_PTR_INIT`: a 32-bit
     toolchain has NO relocation that stores a symbol address into a 64-bit field
     (GCC: "initializer element is not constant"), so on ILP32 the macro initializes
     a word-split `GwordsStatic32` union arm — address relocated into `w1lo`, tag as
     a plain constant in `w1hi`. On 64-bit hosts and N64 builds the macro expands to
     the original `{{ w0, _GFXW1_PTR(x) }}` shape (semantically identical).


## decomp-port-audio-specwait-yield.patch (M1 race freeze)

`Audio_SetSpec` waits for the audio-heap reset with a raw
`do {} while (!AudioThread_ResetComplete())` spin on the game fiber (host thread).
Under PORT the reset stages only advance on the dedicated audio thread
(`gdx_audio_produce_one_tick`), and on the 3DS kernel — and Azahar's faithful
emulation of it — a running thread is never preempted by an equal-priority thread
(the devkitARM thread shim inherits the creator's priority), so the spin starves
the audio thread forever: a permanent, svc-silent, single-core freeze. The PORT
branch (both `audio/rom` and `audio/disk` variants) yields 1 ms per iteration via
`port/n64_sched.c:gdx_audio_specwait_yield`, which also feeds the watchdog's
`spec=` counter. Desktop is unaffected in behavior (preemptive OS scheduling
already saved it) but gets the same polite wait.


## decomp-3ds-dma-low-address.patch (M1 race-entry freeze)

`Dma_PortRamPointer` (src/sys/dma.c) treats any address `< GDX_DMA_RDRAM_SIZE`
(16MB) as a raw RDRAM offset. On 64-bit hosts pointer width alone excludes real
pointers from that branch; on ILP32 it does not, and the 3DS process image maps
from 0x00100000 — EXE globals below 16MB (gCourseCtx at ~0x0038xxxx) had their
DMAs rerouted into the RDRAM shadow. Course data never arrived, the course
segment list never linked, and Course_SegmentLengthsInit spun forever on NULL
segment pointers — the mode+difficulty race-entry freeze and its ~18.5k/s
unmapped-access storm (which also tanked emulator fps). The patch makes the
branch prefer a registered host range (the EXE image is registered at boot) for
addresses >= 0x00100000, restoring the 64-bit semantics; unclaimed addresses
still resolve as RDRAM offsets, and addresses below 0x00100000 (unmapped on the
3DS) stay unambiguous.


## decomp-3ds-machineselect-gradient-coalesce.patch (SELECT-PERF — background gradient)

`MachineSelect_BackgroundDraw` (shared by the machine-select AND machine-settings /
accel-max-speed screens — exactly the two screens reported extra slow) paints its
blue background gradient as one `gDPPipeSync + gDPSetFillColor + gDPFillRectangle`
triplet PER SCANLINE: ~224 (240 borderless) single-row fill rects, ~700 Gfx commands
per frame that the bridge re-translates and the interpreter rasterises as 2 tris each
(with the full per-tri state resolution). No other menu screen does this — main menu
and course select emit zero fill rects, the title screen one. It is a pure
differential CPU tax on the machine screens.

The `>>3` quantisation collapses the 224 rows to at most ~8 distinct 5551 colours
(r∈{0,1}, g=0, b∈0..7), so the patch (GDX_PLATFORM_3DS-gated; desktop DL content
stays byte-identical) coalesces runs of identical fill colour into one tall fill
rect: ~8-9 rects instead of 224/240. Pixel-identical by construction — a multi-row
FILL rect is the exact union of its single-row rects, the colour math per row is
unchanged (including the borderless index clamp), and a PipeSync between same-colour
fills has no visual effect. Expected: machine-screen draws= / tris= in [c3d] drop by
~215 rects / ~430 tris per frame, build= falls correspondingly.


## decomp-race-cull-diagnostics.patch (C2/C3-COURSE-CULL)

Bounded, read-only-except-kill-switch, PORT-gated diagnostics that settled where race
geometry was lost:

- `[cull]` in Course_Draw (C2): per-64th-frame census of the chunk depth/NDC cull
  with its matrix operands and 4 sample chunks; 64 bursts max.
- `[cull2]` (C3): full concatenated-matrix row (m30/m31, col0/col1, p00/p11), a
  reclassification of every depth-in-range rejected chunk by WHICH NDC test rejected
  it, by-index samples with both NDC coordinates, and a near-band (w < 3200) dump —
  this proved every road-ahead chunk passes the cull (ndcx ~0.02 at 200-4400 units)
  and the rejects are geometrically correct side/behind chunks.
- `[grp]` (C3): the chunk-group tessellator walk (both DrawChunkGroup functions),
  logging each group and each emitted span on armed frames — proved the painter's
  order walk emits contiguous spans covering the whole visible course.
- `[rvcull]` in Racer_Draw (C2): racer visibility census with operands for up to 3
  culled-but-active rivals — rivals vis up to 29/30 at race start, gate healthy.
- **`gdx-nocull.txt` kill-switch** (C3, via `gdx_nocull_test_enabled()` in
  port/n64_sched.c): force-passes every depth-in-range chunk, bypassing the NDC
  tests. The discriminating experiment: the race frame was pixel-identical with the
  cull forced open, exonerating the cull end-to-end and (with `gdx-nofog.txt`)
  pinning the invisible road on the fog LUT misbinding fixed by
  lus-3ds-fog-exact-params.patch.

Kept applied: the logs are the fastest regression tripwire for this class. The census
family is OFF by default behind the `GDX_DIAG_CULL` Dev Tools gate (C4: an armed frame
emits ~75 formatted-float lines and the human measured the bursts costing fps); enable
via the env var or F1 > Dev Tools > "Course/racer cull census". No game state is
written unless the nocull file exists; desktop behavior identical (PORT-gated, logging
only, switches off by default).


## decomp-3ds-rom-audio-port.patch (AUDIO-CONTENT — rom-driver host port; silence root cause)

The 3DS build compiles the ROM-side audio driver (`src/audio/rom/`, selected by
`GDX_EXPANSION_KIT=OFF` in port/3ds/game/CMakeLists.txt) — a driver the desktop port
NEVER builds (desktop is EXPANSION_KIT=1 → `src/audio/disk/`), so it had received
none of the disk driver's PORT work. Result on device: HLE ran an empty ~27-command
mix every tick and `[audio-out]` showed `nz=0` forever. Four fixes, all mirrors of
the disk driver's existing PORT blocks:

- **`external.c`**: `Audio_Update` was stubbed `return` under PORT (stale
  "Audio_Init is skipped" rationale from pre-EK desktop bring-up). That killed the
  game's entire per-frame audio pump — engine SE, SE stacks, BGM state machines.
  Un-stubbed (guarded by `gAudioContextInitialized`), plus `Audio_SetOutMode` and
  `Audio_GuitarSeqStart` un-stubbed. One-shot `[audio-rom]` receipt on first pump.
- **`lib/thread.c`**: the little-endian AudioCmd union fix (`QueueCmdS8/U16` stored
  their payload at byte 3 via the console's big-endian shift; every
  `AUDIOCMD_CHANNEL_SET_IO` — including `Audio_SEStart`'s sfxId — read back 0), plus
  the cross-thread wiring: QueueCmd under `gdx_audio_ctx_lock`,
  `ScheduleProcessCmds`/CreateTask drain over the lock-free cmd ring, taskStartQueue
  post to the host atomic. Periodic `[audio-seq]` receipt (seq players + notesOn).
- **`lib/load.c`**: the in-place font "relocation" read the big-endian font image as
  host-endian and patched garbage pointers into it. Replaced with the disk driver's
  `gdx_fontconv` host-native conversion, adapted to the ROM font layout (no SFX
  array; instruments at u32[1+i]; Sample flags `codec:4 medium:2`). `[fontconv]`
  receipt per converted font.
- **`lib/seqplayer.c`**: `AudioSeq_PortConvertSeqEnvelope` for the layer/channel ENV
  ops (seq blob stays big-endian; envelopes must be converted, pointer-keyed cache).

Port-side counterpart (committed normally, not a patch): `[audio-hle]` receipts in
port/n64_audio_hle.c — per-run command count, ADPCM/ENVMIXER voice-op counters, and
a one-shot "FIRST NONZERO output" line naming the first real PCM leaving the DSP.

Note: `decomp-3ds-rom-audio-port.patch` was REGENERATED 2026-08-21 (audio
forensics) from the feat/3ds-audiocontent worktree's final audio state: the
originally committed 736-line patch was an earlier iteration and is NOT
sufficient for audible output (empty ~27-cmd mix, adpcm=0). The regenerated
1015-line patch (external.c, lib/{audio.h,heap.c,load.c,seqplayer.c,thread.c})
is the audible state, roundtrip-verified against the sanctioned stack. See
docs/research/audio-regression-forensics.md.


## decomp-port-course-select-state-reset.patch (TIMETRIAL — post-GP course-select input freeze)
`CourseSelect_Init`'s `sCourseSelectState = COURSE_SELECT_CUP_SELECT` reset is
`#ifdef EXPANSION_KIT` in the decomp: the retail US ROM relied on the course_select
OVERLAY re-DMA restoring the `.data` initializer on every entry, and the EK
re-release added the explicit reset. The port links overlays statically
(`Dma_LoadOverlay` does not restore initialized data), so on the 3DS build — the
only non-EK port build (`port/3ds/game/CMakeLists.txt` sets `GDX_EXPANSION_KIT OFF`;
desktop is EK=ON, which is why desktop never showed this) — a SECOND course-select
visit inherits the previous visit's state (`COURSE_SELECT_CONTINUE` after advancing
to machine select, `COURSE_SELECT_NEXT_COURSE_*` after a GP cup's next-course
screens). `CourseSelect_Update`'s state switch has no case for those values, while
every draw switch does: the screen keeps rendering (identical DL every frame,
`[gpu] md=10 draws≈141 tris≈3760` — the frozen fingerprint in the hardware log)
but ignores ALL input forever. Hardware-verified as the "TIME TRIAL entry wedges
after a GP cup" freeze (watchdog `stage=7 fiber=-1` was a red herring — normal
between-frames snapshot; the `[tt]` park dump added alongside this patch shows all
fibers healthy in that state). Fix: perform the reset under `PORT` as well.

## decomp-port-rival-detail.patch (RIVAL-DETAIL — crowd-frame CPU via earlier rival LOD)
The 30-machine crowd frames pay per-rival triangles + draws + texture switches the
game already knows how to shed: Racer_Draw natively tiers every machine by view
depth (machineLod 1..6 at 230/290/380/470/1500, shadow gate unk_2B2 at 800, effect
tier unk_2B3 at 400/900) and D_800CDDB0 holds six per-machine display lists down to
a far impostor. This patch makes those stock thresholds engage earlier for RIVALS
only, driven by a port hook `gdx_rival_detail_level()` (0 NATIVE / 1 REDUCED /
2 MINIMAL; 3DS: `[perf] rival_detail`, live row on the menu DISP tab, implemented in
port/3ds/gdx3ds_menu.c — latched from the INI even when the menu UI is disabled).
Mechanism: a pre-pass after the sp580 projection row build marks biased racers in
`sGdxRivalBiased[]`; in the main tier loop a biased rival's `temp_fa0` is stretched
`near + (d - near) * bias` before the threshold chain (REDUCED near=250 bias=2.5,
MINIMAL near=120 bias=6). Piecewise-multiplicative: exact native inside the near
band, scale-free beyond it, and ALL downstream thresholds (LOD tiers, shadow,
effects) inherit the one rewrite — no per-threshold constants to keep in sync.
Effective REDUCED: lod5 by ~470 raw, shadows off past ~470 (stock 800), effects
2/1 tiers at ~310/~510 (stock 400/900). MINIMAL: lod5+ past ~180, shadows off past
~233, effects at ~167/~250.
Safety rails: human machines (index < gNumPlayers) are NEVER biased; in MINIMAL the
nearest GDX_RIVAL_PROTECT_COUNT(5) rivals by |view depth| keep native treatment (you
fight your neighbours — a rival right behind the camera still counts as near); the
TIME_ATTACK ghost loop (<=3 machines, not a crowd) and the GP_END_CS/COURSE_EDIT
all-lod-1 paths are untouched; NATIVE (0) leaves every comparison on the stock
constants; non-PORT builds compile the original code. `temp_fa0` is not read after
the tier assignments (verified), so overwriting it in place is safe. Applies
independently of the other decomp patches (racer.c is otherwise untouched by the
stack); listed last of the decomp series.
Applied after `lus-cc-key-uninit-shader-id.patch`.
## decomp-native-track-shim.patch ([native] track — course tessellator output redirect)
Three guarded hunks in src/game/course.c, all under `#if defined(PORT) && defined(GDX_NATIVE_TRACK)`
(the 3DS game target defines it): `#include "gdx3ds_track_shim.h"` after the asset header,
`GDXTRK_BEGIN(cameraIndex)` in Course_Draw right before the two setup display lists, and
`GDXTRK_END()` before its return. The shim (port/3ds/gfx/gdx3ds_track_shim.h) redefines
gSPVertex / gSPDisplayList / gSPModifyVertex / gSPTexture / gDPLoadSync / gDPSetTextureImage /
gDPLoadBlock; the C bodies of the ten chunk-group functions, both chunk walks and both setup
paths are untouched. Inactive (`[native] track=0`, `trkcensus=0` — the default) every macro
forwards to port/3ds/game/gdx3ds_track_gbi.c, a TU that invokes the ORIGINAL macros under the
same gbi.h, so the emitted packets are byte-identical to the unpatched build. Active, the
vertex/template-DL/modify-vertex commands are recorded into a per-GfxPool op list and replaced by
one G_NOOP marker per material run that Interpreter::GdxTrackMarker (lus-3ds-native-track-marker
.patch) replays at that exact display-list position. Non-PORT builds compile the original code.
Applied last of the decomp series (course.c is otherwise touched only by
decomp-race-cull-diagnostics.patch, in different hunks).

## lus-3ds-native-track-marker.patch ([native] track — marker replay hooks)

**Folded into the vendored carve** (`port/3ds/vendor/lus/`, see its PROVENANCE.md); the patch file lives in `port/3ds/vendor/lus/patches/` for provenance only.
Three `__3DS__`-gated additions so the port can replay the course tessellator's recorded
vertex/template ops at the exact display-list position the stock commands occupied:
(1) interpreter.h declares `Interpreter::GdxTrackMarker` (defined by the port in
port/3ds/gfx/gdx3ds_track_replay.cpp), `GdxTrackPackState` and `GdxTrackSetCombine`;
(2) the F3DEX2 G_NOOP handler forwards `p == 0x5A/0x5B` (never emitted by the game or LUS's
OPEN/CLOSE_DISPS 7/8) to GdxTrackMarker; (3) GdxTrackPackState is a read-only snapshot of the
file-static draw-state memo + packed-batch latches (sGfxS7TriMemo, sGdxPack*) the replay's fast
append reproduces, and GdxTrackSetCombine runs the RDP set-combine handler plus the dirty mask
gfx_step would OR for it. Nothing on the stock path changes; desktop builds see only the header
declarations. Applied last of the libultraship series (context is the fully patched
interpreter.cpp).
## decomp-port-determinism-pins.patch

Determinism/ORACLE track. Pure delta
against the full decomp stack above (it touches `src/sys/sys_gfx.c`, which `decomp-ilp32.patch`
also touches, so it is applied LAST of the decomp series). Three wall-clock sources become
pinnable through `port/gdx_determinism.c` accessors; with no pin every original expression
runs byte for byte:

| Site | Original | Pinned |
|---|---|---|
| `src/sys/sys_gfx.c` `Math_Rand1Init` / `Math_Rand2Init` | `osGetTime()` seeds (the old `#ifdef PORT` read `getenv("GDX_RAND_SEED1/2")`, dead on 3DS) | `gdx_det_rand_seed(1|2, &seed)` — `[debug] rand_seed1/2`; desktop keeps the env fallback inside the accessor |
| `src/sys/sys_gfx.c` boot-logo hold | `while (osGetTime()-t0 < 230 frames) gdx_yield();` — one VI tick per yield, so the tick count consumed is wall-clock dependent and every later tick-scheduled event drifts | `gdx_det_boot_hold_ticks()` > 0 -> exactly that many `gdx_yield()`s (`[debug] boot_hold_ticks`, 230 under `det=1`) |
| `src/audio/disk/lib/thread.c`, `src/audio/rom/lib/thread.c` `audioRandom *= osGetCount()` | hardware entropy (the disk lib already pinned it under `gdx_pcm_capture_active()`) | `gdx_det_audio_pin()` OR'd into the existing condition; the ROM lib gets the same block |

Killswitch: absent `[debug] det` / `rand_seed*` / `boot_hold_ticks` / `det_audio` keys the
accessors return "unpinned". Receipt: `[det] init det=.. seed1=.. seed2=.. boot_hold_ticks=..
audio=..` at boot.
## lus-renderthread-texcache-owner.patch (round 4, feat/3ds-balance)
Weak hook `gdx3ds_texcache_note_thread(kind)` at the top of Interpreter::TextureCacheClear /
Lookup / Delete / DeletePalette (3DS only). port/3ds/gdx3ds_renderthread.cpp counts calls that
reach the cache from any thread but the render thread while it owns the cache and reports
`[rt] ... texcacheMainMut=` (must stay 0). Pure delta on the full stack; no behavior change.

## lus-native-machines-dl-hook.patch ([native] machines — G_DL call-out)

**Folded into the vendored carve** (`port/3ds/vendor/lus/`, see its PROVENANCE.md); the patch file lives in `port/3ds/vendor/lus/patches/` for provenance only.

One hunk in `src/fast/interpreter.cpp` `gfx_dl_handler_common`: after `SegAddr`, a weak
`gdx3ds_mesh_dl_hook(interpreter, subGFX)` (port/3ds/gfx/gdx3ds_mesh.cpp) draws a native
machine mesh when the target is one of the registry's stub lists and skips the stub; it
also sets `sGfxS7DirtyMask = 0xFF` because the native draw binds material state directly,
so the tri/rect draw-state memos must re-derive. `#ifdef __3DS__`, weak symbol: desktop
LUS and a tree without the mesh module are byte-identical. Pure delta on top of the full
libultraship stack above (applied last); trivial to fold into the vendored interpreter.

## decomp-native-machines-rebuilt.patch ([native] machines — list generation bump)

`src/game/racer.c`: the three sites that (re)build the static machine model lists
`D_800CDDB0[]` (`func_8008D3C4`, `func_8008D5F4`, `func_8008D97C`) call
`gdx_native_machine_lists_rebuilt()` (port/decomp_port.c, no-op off-3DS) so the mesh
registry re-bakes at the next G_DL. Applied last of the decomp series.

## lus-nverify-diag.patch (native/interp — per-batch verify side channels)

**Folded into the vendored carve** (`port/3ds/vendor/lus/`, see its PROVENANCE.md); the patch file lives in `port/3ds/vendor/lus/patches/` for provenance only.
3DS only, pure delta on the full stack. Two hooks gated on an inline `gdx3ds_verify_mode != 0`
test (false unless `[native] interp_verify=1`; the gate was a `gdx3ds_verify_active()` CALL
until 2026-09-23 -- with no LTO in the tree that was a real per-command and per-triangle cost
on the default `interp=0` path, audit finding 5): `gdx3ds_verify_note_cmd(w0,w1)` at the top of gfx_step
(command-stream differential against the owned walker) and `gdx3ds_verify_note_tri(&vt)` in
GfxSpTri1 just before packed emission (tile window / extents / tm / loaded sizes / othermode /
viewport for the mismatching batch). Zero behavior change; one predictable branch per command
when verify is off. Temporary: goes with the LUS interpreter at native M5.
## decomp-port-title-demo-audio-pin.patch (RESIDUALS — rare boot/title divergence)

Pure delta on top of the stack above; independent of every other patch (it touches only
`src/audio/disk/external.c:func_807442E4` and its ROM-library twin
`src/audio/rom/external.c:func_800B84B8`, which no other patch edits).

**What it fixes.** Those two functions are the *only* place in the game where state the
renderer can see is advanced by the audio library's synthesis position rather than by a
tick count:

```c
temp_lo = (s32) note->synthesisState.samplePosInt / 100;
if ((temp_lo > 2100) && (temp_lo < 2110) && (gGameMode == GAMEMODE_FLX_TITLE) &&
    (gTitleDemoState == TITLE_DEMO_INACTIVE)) {
    gTitleDemoState = TITLE_DEMO_ACTIVE;   /* -> attract demo */
}
```

`Audio_Update()` runs on the game thread once per frame, but `samplePosInt` is advanced by
the dedicated audio producer (`port/gdx_audio_thread.cpp:AudioThreadMain`), which self-pumps
every 5 ms of WALL CLOCK and produces while the DSP ring is below its target — it is not
tied to the VI tick. Emulated, the game runs far slower than realtime, so `temp_lo` advances
by tens per game tick and the arming window is only 9 units wide: whether any tick lands
inside it — and therefore whether the attract demo starts, and on which frame — varies
between otherwise bit-identical replays. Measured (RetroArch lane, `det=1`, three runs of
the same replay): `temp_lo` at input tick 127 = 8 / 4 / 8, at tick 420 = 1340 / 1343 / 1350.
Six title-hold boots of the same replay produced four different frame-700..1300 hash
sequences. This is the "1 run in 7 leaves the logo hold early into the attract demo"
residual, measured on the New 3DS build.

**The pin.** Under `gdx_det_audio_pin()` (`[debug] det_audio`, implied by `[debug] det=1`)
the two `gTitleDemoState` writes are skipped, so the attract demo is never armed from audio
state and the title screen advances on tick count alone. Unpinned — i.e. all real play —
the expression is byte for byte the original, including the
`AUDIOCMD_GLOBAL_INIT_SEQPLAYER` call in the same function, which is deliberately NOT
gated (it is audio-internal and does not touch game state).

**Deliberately not done:** deriving `temp_lo` from a tick counter so the demo still fires at
a fixed tick. That would be the more faithful pin, but it changes what every existing
parity replay records (all of them assume the demo does not fire), so it belongs to a
separate change with its own re-recorded replays.

## decomp-3ds-mainprof-checkpoints.patch (MAINPROF)

`GDX_MP(...)` phase markers inside BOTH of `src/sys/sys_gfx.c`'s per-VI frame functions —
`func_80067D64` (`D_800CCFE4 == 2`) and `func_80067E98` (`== 3`, the triple-buffered path the
game runs in for most of a session; the switch is at the bottom of that file) — plus the macro
block that defines them (`#ifdef PORT` only; off
PORT `GDX_MP` expands to nothing, exactly like the `GDX_CK` block it sits beside).

**Why the decomp and not the port.** `func_80067D64` IS the game thread's frame, and on the
3DS it runs inline on the main thread inside `gdx_vi_tick` (the VI `osSendMesg` dispatches
the fibers from host context — `docs/research/renderthread-audit.md` §8). The phases the
A8 main-thread profile has to separate — `Audio_Update`, `func_800690FC` (the game-mode
update), `Gfx_LoadSegments`, `func_80069698` (the DL build), `Segment_LoadAssets` + the
transition capture, and `Gfx_SetTask` — are plain sequential calls inside that one
function with no port-side boundary between them. There is nowhere else to stand.

**Cost and blast radius.** `GDX_MP(n)` is one call to `gdx_mprof_ck` (`port/n64_sched.c`,
defined for every platform; a no-op off 3DS), whose first statement tests the single int
`gdx3ds_mprof_active`. That int is 0 unless `gdiffuser.ini [debug] mainprof = 1` or
`[debug] gputrace = 1` (C4i: the `[oth]` split reads this clock; gputrace runs it without
the `[mprof]` receipt), both of which default to 0. Nine calls per game frame, in family with the `gdx_diag_verbose()` call the
adjacent `GDX_CK` lines already make. No game state is read or written: the profiler only
reads `svcGetSystemTick`. Armed, the receipt is emitted from the HOST loop at a window
boundary, never from inside a game fiber, so the replay's tick order is untouched —
verified by `[fh]` parity.

## decomp-3ds-disk-audio-ilp32.patch (B11 — Expansion Kit build, ILP32 argument passing)

`src/audio/disk/lib/load.c` declares two port shims with 64-bit parameter types:

```c
extern void* gdx_host_calloc(unsigned long long count, unsigned long long size);
extern void* gdx_rdram_persist_alloc_raw(unsigned long long size, unsigned long long align);
```

Both real definitions take `size_t` (`port/shims.c`, `port/decomp_port.c`). On LP64 desktop
`size_t` and `unsigned long long` are both 64 bits, so the spelling was harmless and nobody
noticed. On the ILP32 3DS build `size_t` is 32 bits, and ARM32 AAPCS passes each `u64`
argument in a **register pair** — so the callee reads its *second* parameter out of the high
half of the first. `gdx_rdram_persist_alloc_raw` therefore saw `align == 0`, computed
`base = (top - size) & ~(0u - 1u) == 0`, decided the arena was exhausted and called
`abort()`. That is the first hard stop of an `GDX_EXPANSION_KIT=ON` 3DS build: it dies at
the first soundfont conversion, around guest frame 180, with
`[rdram] FATAL: arena exhausted (persist)`.

This patch respells both externs as `unsigned int`, exactly as the ROM twin
`src/audio/rom/lib/load.c` already does (that file carries the same fix and the same
comment, applied when the ROM audio driver was ported in
`decomp-3ds-rom-audio-port.patch` — the disk twin was skipped because the 3DS has never
built it).

**Affects only `GDX_EXPANSION_KIT=ON`.** `audio/disk/` is excluded from the shipped
(EK=OFF) source list by `port/cmake/GdxGameSources.cmake`, so the patch changes no byte of
the default build.

## decomp-3ds-disk-audio-heap-ilp32.patch (B11 — Expansion Kit build, static audio heap)

`src/audio/disk/lib/audio_heap.c` sizes the disk audio library's static heap at
`0x2ECA00 * 4` (**12.2 MB**) under `PORT`, and says why:

> The pools are carved by N64-tuned spec sizes, but several cached structs carry 64-bit
> pointers on the host and outgrow their N64 footprints.

That is a **64-bit-host** headroom figure. The overflow it pays for is pointers doubling
from 4 bytes to 8, which does not happen on an ILP32 target. On the 3DS the `x4` takes
12.2 MB of BSS out of a ~49 MB application heap before a single allocation — by itself the
largest single reason an `GDX_EXPANSION_KIT=ON` 3DS build runs out of memory at boot. (For
scale: the ROM audio library's `gAudioHeap` is `0x3B000`, 236 KB.)

This patch makes the multiplier `x2` when `__SIZEOF_POINTER__ == 4`, leaving 2.9 MB of
headroom over the retail carve, and keeps `x4` on every 64-bit host so no desktop build
changes. Total EK BSS: 21.59 MB → 15.45 MB.

**Affects only `GDX_EXPANSION_KIT=ON`.** `audio/disk/` is excluded from the shipped (EK=OFF)
source list, so the patch changes no byte of the default build.

## decomp-3ds-disk-audio-deadlock.patch (B11 — the EK boot deadlock)

Two changes in the Expansion Kit's disk audio library, both port-only (`#ifdef PORT`) and both
in files `GdxGameSources.cmake` excludes from the shipped (EK=OFF) source list, so **no byte of
the default build changes**.

**1. `src/audio/disk/lib/load.c` — `AudioLoad_Dma` must complete the request it declines.**
Two early returns (`gAudioCtx.resetTimer > 16`, and `default:` for any medium that is not
`MEDIUM_CART` or `MEDIUM_DISK_DRIVE`) issue no transfer and post nothing to `reqQueue`. Every
caller then blocks in `osRecvMesg(reqQueue, ..., OS_MESG_BLOCK)` on a queue nobody will post to.
On console that is unreachable — the ROM library's media are all `MEDIUM_CART` — but the EK
tables are `MEDIUM_LBA` throughout, and the measured EK boot parks the **dedicated audio
producer thread** on `gAudioCtx.syncDmaQueue`. Because that thread holds `sAudioCtxMutex` across
its whole tick, the game fiber then blocks forever in `AudioThread_QueueCmd` on a *host* mutex
the cooperative scheduler cannot see, and the entire game freezes in the boot-logo window. The
patch posts the completion, zeroes the destination (deterministic silence rather than heap
garbage read back as a sequence script — the same thing the 3DS `osEPiStartDma` shim does on a
failed segment read) and emits a capped receipt, because a no-op DMA completion is a missing
asset and must not be silent. This is the policy `port/n64_leo.c:79` already states for the
layer below.

**2. `src/audio/disk/lib/seqplayer.c` — a NULL channel must not be "valid".**
`IS_SEQUENCE_CHANNEL_VALID(ptr)` only excludes `&gAudioCtx.sequenceChannelNone`, so a NULL
channel passes it and `AudioSeq_SequenceChannelDisable` writes through address 0. Measured on
the EK boot as 122 unmapped accesses at `channel->finished`/`enabled`, `layers[0..3]`
(0x50..0x5C) and the four `notePool` list heads (0x88..0xB8). Azahar logs those and carries on;
console hardware data-aborts. `AudioSeq_SequencePlayerDisableChannels` now skips NULL with a
capped receipt.

## decomp-3ds-rdram-size-knob.patch (C4g — the RDRAM arena size stops being three constants)

`port/n64_rdram.h`'s `GDX_RDRAM_SIZE` is now `-DGDX_3DS_RDRAM_MB=N` (default 16, the shipped
value). Two decomp TUs carried the same 16 MB as their own literal and would not have followed
it:

- `src/sys/dma.c` defined `GDX_DMA_RDRAM_SIZE ((size_t)0x1000000u)` with a comment saying it
  "must match GDX_RDRAM_SIZE in n64_rdram.h" — a convention, enforced by nothing.
- `src/game/object.c`'s `GDX_RecordAssetWrite` wrote the literal inline with
  `/* GDX_RDRAM_SIZE */` beside it.

Both are **upper-bound range tests on a destination pointer** (`dst < gdx_rdram + SIZE`), so a
stale 16 MB against a smaller arena admits pointers past the end of the buffer and records DMA
over bytes the process does not own. Both now read `GDX_RDRAM_SIZE_BYTES`, which
`port/3ds/CMakeLists.txt` puts on the command line for every TU, with an `#ifndef` fallback to
`0x1000000u` so a host build that never defines it compiles exactly what it compiled before.

No behaviour change at the default size: the macro expands to the same constant.

## decomp-3ds-disk-residency-seam.patch (B11d — the last two `gdx_disk_buffer` indexers)

Two one-line-shaped changes in the Expansion Kit's 64DD path, both inside existing `#ifdef PORT`
blocks and both in files `GdxGameSources.cmake` excludes from the shipped (EK=OFF) source list,
so **no byte of the default build changes**.

They are rows 4 and 5 of `diskstream`'s residency census. Every other row was a port file; these
two are the reason `[debug] disk_stream=2` was an explicit refusal rather than a mode. Both go
through the same new seam in `port/disk_buffer.cpp`:

```c
int gdx_disk_image_read(unsigned int offset, unsigned int length, void* dst); /* resident, else stream */
int gdx_disk_image_available(void);                                           /* either source live */
```

**1. `src/sys/disk/disk_drive_dd.c` — `DiskDrive_LoadData`'s DD-course record read.**
`bcopy(gdx_disk_buffer + phys, vram, diskSize)` becomes `gdx_disk_image_read(phys, diskSize, vram)`.
The bounds check moves inside the seam, which also **leaves `vram` untouched on refusal**, so the
existing zero-fill arm still owns the "nothing readable at this handle" case instead of the read
half-writing one.

**2. `src/sys/disk/disk_mount_dd.c` — the MFS byte-order pass's mount test.**
`gdx_disk_buffer == NULL` becomes `!gdx_disk_image_available()`. In `disk_stream=2` the pointer is
null while a disk **is** mounted, so the old test would have skipped the pass, left `gMfsRamArea`
big-endian, and `Mfs_GetDirectoryIndex` would never have matched `MFS_FILE_ATTR_DIRECTORY` — the
editors' file picker comes up empty and `Mfs_SaveFile` bails with `N64DD_NOT_FOUND`, both
silently. That is the exact failure the pass was written to prevent, so the test had to follow it.

## decomp-3ds-disk-audio-async-guards.patch (B11d — two guards in `AudioLoad_AsyncLoadInner`)

`src/audio/disk/lib/load.c`, inside `#ifdef PORT`, in an EK-only source file
`GdxGameSources.cmake` excludes from the shipped (EK=OFF) list — **no byte of the default build
changes**. Applies after `decomp-3ds-disk-audio-deadlock.patch`, which touches the same file.

`ekdeadlock` fixed `AudioLoad_SyncLoadInner`: its `switch (cachePolicy)` has no `default:` arm,
so a table entry whose `cachePolicy` is outside 0..4 leaves `ramAddr` **uninitialised** and the
load runs against a stack value. **`AudioLoad_AsyncLoadInner` has the identical switch and was
not fixed.** With no disk mounted the EK audio tables are garbage — ekdeadlock measured
`cachePolicy=13`, `medium=0x5F` on exactly this boot — and `cachePolicy` is a *signed* `s8`, so
a negative value also indexes the compiler's jump table out of range.

Measured, not inferred: a no-disk EK boot crashed at guest frame ~69 with
`ExceptionRaised(UndefinedInstruction, pc=0x0010B480)`, which `addr2line` resolves to
`AudioLoad_AsyncLoadInner` — the switch dispatch. It varies **by binary**, not by run, which is
what a garbage table read plus an uninitialised stack slot looks like: one build of this branch
booted the no-disk path cleanly every time and the next two died every time.

**THE GUARD MUST PRECEDE THE SWITCH**, and that is a measurement too. Written after the switch
first — which is where `ekdeadlock` put the sync twin's — the crash simply moved from
`0x0010B480` to `0x0010B4B8` and stayed: the fault is in the jump-table dispatch, which a guard
placed after the switch never reaches. `ekdeadlock`'s `AudioLoad_SyncLoadInner` guard has the
same shape and therefore the same latent exposure; nobody has seen it fire, and it is
deliberately left alone here rather than edited in passing.

The patch refuses the load with a capped receipt, the way every allocation failure inside the
switch already does, and **posts nothing**: an async load that was never started must not report
a completion its caller would read as data (`port/n64_leo.c:79`'s policy).

**Guard 2 — a NULL load table, and this is the one the crash was actually about.** Moving guard 1
before the switch did not stop it: the PC moved again, and the disassembly settles it —

```
10b4d8:  lsl  r6, r6, #4        @ r6 = realId * 16
10b4dc:  ldr  r3, [r6, #20]     @ table->entries[realId].size, with NO base register
10b4e0:  udf  #0                @ GCC's trap for a block it proved undefined
```

`AudioLoad_GetLoadTable` **returns NULL** for any `tableType` outside its three cases — its own
`default:` arm, `load.c:896` — and every caller here dereferences the result unchecked. GCC takes
that at its word: on the null path it constant-folds the base away and terminates the block with
a literal `udf #0`. So the no-disk EK boot is **not** hanging and **not** taking a NULL data
abort; it is executing the compiler's trap for a path the source told it was impossible. The
patch honours the contract the callee already states.

Measured working: with the guard in, the same no-disk boot logs
`[audio-disk] AsyncLoad REFUSED null load table type/id/realId a=00000087 b=00000089 c=00000089`
— `tableType = 0x87`, nowhere near `SEQUENCE_TABLE`/`FONT_TABLE`/`SAMPLE_TABLE` — and the run's
`ExceptionRaised` count goes from **1020 to 0**. Where a `tableType` of 0x87 comes from is NOT
answered here and is worth someone's time: it is a caller argument, not a value read off the
disk, so something upstream is passing garbage.

`AudioLoad_SyncLoadInner` dereferences the same possibly-null table and has the same shape of
guard after rather than before its switch. Both are **filed, not fixed** — editing ekdeadlock's
patch in passing is not this track's call.


## decomp-3ds-ek-diskcourse-decline.patch (B11j — the RECORDS cup-row hard freeze)

**The bug, on real hardware.** In the EK build with no 64DD disk image, selecting a cup on the
RECORDS cup row froze the console outright: no fault, no Luma dump, the last frame held.
`gLeoDriveConnectionState == 0` correctly bounces the cursor off the two SELECT MODE editor
slots (`main_menu.c:203`), and the cup row had no equivalent guard.

**Which cups, measured rather than assumed.** `src/game/course_gadgets.c`'s
`courseIndex >= COURSE_EDIT_1` arm covers courses **24..47** — the EDIT cup
(`COURSE_EDIT_1..6` = 24..29) **and both DD cups** (30..41, starting at `COURSE_SILENCE_3`).
Only `COURSE_X_1` (48) and above take the branches above it. So cup options **10, 11 and 12
are all disk-backed**; an earlier draft of this patch guarded option 10 only, on the assumption
that the DD cups were cart-ROM backed through `gRomSegmentPairs[5]`, and an emulator run
falsified it by firing the decline at `courseIndex 30`.

**Where it hangs.** Landing the cursor on the bottom cup row arms `CourseModel_Init`, whose
async preview loader calls `func_80702448(cup * 6)`; selecting it reaches `Course_Load`. Both
take that arm, where:

1. `DiskDrive_LoadData(SEGMENT_DISK_START(silence_3) + diskCourseIndex, ...)` runs with no
   image. `src/sys/disk/disk_drive_dd.c`'s PORT arm **zero-fills** `vram` when the handle is
   unmapped or the seam refuses — by design, the documented "nothing readable here" case.
2. The validation immediately below then fails (`creatorId != CREATOR_NINTENDO`), and the
   decomp does what the retail drive did: paint error 9 and **halt** —
   `func_8070F8A4(-1, 9); while (true) {}`.

On a 64DD that halt is recoverable: the drive shows the error and the user swaps the disk. On
a 3DS it parks the main game fiber forever with no fault, which is exactly the freeze reported
— and exactly the class `port/n64_leo.c:79` already names: *"an error return that never posts
parks that thread forever."*

**Measured, one binary each, same replay and lever set**
(`tools/dev/replays/ek-editcup.gdxrec`, `[debug] disk_stream=0`, no image):

| build | result |
|---|---|
| before (`3cb24c89e61f7350`) | `RESULT TIMEOUT frame=3453 complete=False shots=3/7`, and **466 consecutive `[watchdog]` beats reading `frame=3453(+0) fiber=5`** while `aud=` keeps climbing — a spinning game fiber, not a parked one |
| after (`8ed8427899fa08fd`) | `RESULT PASS frame=3991 fatals=0 complete=True shots=7/7`, with `[b11] [cupsel] no drive: disk cup row bounced from/to/mode a=00000000 b=00000000 c=00008013` |

**What this patch does**, all of it inside `#if defined(EXPANSION_KIT) && defined(PORT)` arms
so the shipped (EK=OFF) build preprocesses identically — a measurement, not an argument:

- `src/overlays/course_select/course_select.c` — **the decline.** With no drive the cup cursor
  bounces off the whole bottom row (`gCupSelectOption >= 10`) and returns to where it came
  from, exactly as `main_menu.c:203` bounces `MODE_COURSE_EDIT`. Applied both in the input arm
  of `CourseSelect_Update` and in `CourseSelect_Init`, which can otherwise *restore* the cursor
  onto a disk cup from a `gLastRecordsCourseIndex` saved in a session that had a drive.
- `src/game/course_gadgets.c` — **the anti-hang, twice over.** The head of both COURSE_EDIT
  arms (`Course_Load` and `func_80702448`) declines with `gLeoDriveConnectionState == 0`,
  **before** the destructive read; and all four `while (true) {}` parks become a logged
  decline, so a mounted image whose course data does not validate also cannot freeze the
  console. The non-PORT arm keeps the retail halt verbatim.

**The decline is a plain `return` that leaves `COURSE_CONTEXT()->courseData` UNTOUCHED** — the
same contract `gdx_disk_image_read` and `gdx_disk_stream_read` already state: on refusal the
destination keeps its old bytes rather than new wrong ones. The caller then builds its preview
from the previously loaded (valid) course. `Course_GenerateRandomCourse()` was tried here first
and is **not** safe: it is the X-cup generator, and calling it for a DD-cup preview
(`courseIndex 30`) killed the guest immediately after the receipt printed. Measured, not
assumed — that run is `20260926-021935-editcup-fixed4`.

**Every decline reports through `gdx_ek_note`**, the always-on sink. This arm previously
emitted only `PRINTF` (compiled out) and `gdx_cki` (rides `GDX_GATE_TRACE`, which nothing on
3DS sets) — `ekdrive` §2's "two silences", which is why the freeze left no tail. Note that
`func_8070F8A4`'s own `[leo-fault]` lines ride `gdx_cki` too and therefore did not print
either, in the control run or on the console.

**Deliberate divergence, stated:** with a disk mounted and a genuinely corrupt course, the
retail build halts and this one declines with a receipt and a zero-filled course. That second
arm is **not exercised** — it needs a mounted-but-invalid image. A 3DS has no 64DD front panel
and no way to release a parked fiber.

## decomp-3ds-disk-audio-load-census.patch (`[audioload]`, audioasync track)

Phase attribution for the EXPANSION KIT producer tick, in `src/audio/disk/lib/{load,heap,thread}.c`.
Nine nested scopes -- `AudioThread_ProcessCmds`, `AudioHeap_Init`, `AudioLoad_ProcessLoads`,
`AudioLoad_SyncLoadSample`, `AudioLoad_RelocateFontAndPreloadSamples`,
`AudioLoad_PreloadSamplesForFont`, `AudioLoad_SyncDiskDrive`, `AudioLoad_SyncDma` and the port's
`gdx_audio_convert_font` -- each bracketed by `gdx_aload_begin()` / `gdx_aload_end()`
(port/n64_sched.c, contract in port/gdx_audio_load_census.h).

Why: `audioring` settled that the EK producer blocks INSIDE one tick rather than losing the core,
but not WHICH call spends the time, and the obvious answer is not safe to assume -- audioring's
emulator runs produced 1.2 s ticks with **zero** `AudioLoad_DiskDrive ENTER` lines, so the cost
there is not the 64DD read. The scopes NEST, so their totals overlap on purpose: the nesting is
what localises the cost (`cmds` > `heapinit` > `preload` > `disk`).

Off by default (`[debug] audioload`, and `[debug] audioload_ms` for the per-tick report
threshold). With it off each scope costs one `int` compare and reads no clock. Raw externs rather
than an `#include` because `port/3ds/game/CMakeLists.txt` deliberately keeps `port/` off the
decomp target's include path; the site ids are duplicated in each TU and the canonical list with
its rationale is `port/gdx_audio_load_census.h`.

## decomp-port-o3ds-scene-lod.patch (O3DS-SCENE-LOD)

Two visual-only levers for the Old 3DS preset (`port/3ds/gdx3ds_perf_preset.c`,
`[perf] preset = auto|off|o3ds`; `auto` = o3ds on an Old 3DS only). Applies on top of
`decomp-port-rival-detail.patch` (racer.c) and the course.c stack; listed last.

- **racer.c, rival tier 3 (LOW).** `gdx_rival_detail_level()` may now return 3 when the port
  opens it (`[perf] rival_tier_max = 3`; the dynlod controller's ceiling stays 2 otherwise).
  LOW biases an unprotected rival's view depth `80 + (d - 80) * 10`; the nearest
  `GDX_RIVAL_PROTECT_COUNT_LOW` (3) rivals are not exempt as under MINIMAL (5) but get REDUCED's
  curve (`250 + (d - 250) * 2.5`, `GDX_RIVAL_BIAS_MILD`), so their shadow and effects thin out
  too while their mesh stays near-native. Effective for an unprotected rival: LOD 5 past ~119,
  LOD 6 (the far speck) past ~222, shadow past ~152, effects tiers at ~112/~162. Tiers 0-2 are
  byte-for-byte the rival-detail patch's.
- **racer.c, LOD-6 emission (`gdx_rival_lod6_skip()`, 3DS `[perf] lod6_skip`, default 0).**
  LOD 6 is NOT an empty list: `D_7045098` is a 5-command,
  12-vertex far speck, a few pixels on screen. But Racer_Draw emits the machine's whole
  texture-load list `D_800CDD38[i]` (every LOADBLOCK of that machine's textures) in front of it,
  once per LOD-6 machine per frame. Mode 1 drops only that texture-load list and keeps the
  speck: **pixel-identical** on the canonical replay (fhdiff 49/49, race frames included), and
  -5.5% of the Old 3DS emulator race frame at NATIVE, much more at MINIMAL/LOW where most of
  the field is LOD 6. Mode 2 drops the speck too (machines past LOD 6 vanish; measured, not
  shipped). The model matrix is still initialised in both modes.
- **course.c, far cull below stock.** Course_Draw's `gdxFarRenderDistanceScale` (the desktop
  `DrawDistance` CVar, clamped 100..200) is additionally multiplied by the port hook
  `gdx_track_draw_distance_pct()` when it returns 1..99 (3DS: `[perf] draw_distance`, clamped
  40..100). At 100 the multiply is skipped entirely, so stock output is bit-identical.

**Visual only, and why that holds.** Every lever here only changes which display-list
commands are emitted. Game logic reads `racer->machineLod` only as `!= 0`
(visible at all; cpu.c:1082/1172, racer.c:4696/4830/5223), and the bias is applied after the
frustum/2500 tests that decide 0 vs non-zero, so no racer's visibility bit changes;
`unk_2B2`/`unk_2B3` are read only by the shadow/effect draw code. `chunk->drawState` (the far
cull's output) is read only by Course_Draw and Course_GadgetsDraw; neither function nor
Racer_Draw calls the RNG. Measured: the `[debug] gstate_every` digest (RNG streams, frame
counter, every racer's physics and race progress) is identical at all 6002 ticks between the
levers off and on. The desktop build links both hooks from
`port/gdx_linkstubs_desktop.c` (100 and 0: stock).

## decomp-3ds-c5cat-markers.patch (C5 STAGE 0 `[c5cat]`)

`GDX_C5CAT(cat)` scene-category markers in `src/overlays/ovl_i2/race.c` (`Race_Draw`: sky /
track / dressing around the 1P `Background_Draw`, `Course_Draw`, `Course_GadgetsDraw`; HUD
around `Menus_Player1SpecialDraw` and `Menus_Draw`) and `src/game/racer.c` (`Racer_Draw`:
machines at entry and before the highlight/body pass, shadows before `D_303A758`, fx before
`aSetupBoosterDL` (boost flames + the effects bracket), HUD before the race-intro/marker
block, misc at the return), plus the macro block that defines them (`#ifdef PORT` only; off
PORT the macro is empty).

**Why the decomp.** The Old 3DS per-category CPU timers (`port/3ds/gdx3ds_mainprof.h`, the
`[c5cat]` receipt) need to know which part of the scene the owned walker is drawing. The
walker only sees commands; the scene structure exists only in the order `Race_Draw` and
`Racer_Draw` emit them. A marker at each boundary hands that structure to the walker at the
exact stream position, the same mechanism as the track (0x5A/0x5B) and fx (0x5C/0x5D)
markers. p=0x61 is used because the retained-renderer design reserves 0x5E/0x5F/0x60.

**Cost and blast radius.** Each marker is one call to `gdx_c5cat_armed` (`port/n64_sched.c`,
every platform; 0 off 3DS), which returns the 3DS clock's armed int. That is 0 unless
`[debug] gputrace = 1` or `[debug] mainprof = 1`, both default 0; then NOTHING is written and
the display list is byte-identical to the unpatched game. Armed, each marker writes one
`G_NOOP` (w0 = `G_NOOP<<24 | 0x61<<16 | cat`, w1 = 0), about 12 per 1P race frame. A G_NOOP
draws nothing on either interpreter: the owned walker hands it to `OwnedNoopHook`, and the
LUS noop handler ignores an unknown p. No game state is read or written.

## decomp-native-hud-capture.patch (C5 STAGE 2 `[native] hud_batch`)

Three HUD regions are bracketed: `Racer_Draw`'s marker/countdown tail (`src/game/racer.c`,
from the `GDX_C5CAT(HUD)` marker to the return), and `Menus_Player1SpecialDraw` and
`Menus_Draw` in `Race_Draw` (`src/overlays/ovl_i2/race.c`). `GDX_HUD_REGION_BEGIN()` remembers
where the region starts; `GDX_HUD_CAPTURE()` calls `gdxhud_capture(start, gfx)`
(`port/3ds/gfx/gdx3ds_hud_capture.c`) after the section has written its display list. Both
macros exist only under `#if defined(PORT) && defined(GDX_NATIVE_HUD)` (the 3DS game library
defines `GDX_NATIVE_HUD`, like `GDX_NATIVE_FX`); elsewhere they are empty.

**Why the decomp.** The HUD's CPU cost on the owned walker is per command (C5 Stage 0:
25-31% of the race's render work, ~520 walked commands and ~28 texture loads a frame). The
capture moves each run of RDP-state / texture-load / rectangle commands out of the list into a
record and leaves one `G_NOOP` p=0x5E marker in its place; the owned interpreter replays the
run at the marker through its own handlers (`port/3ds/gfx/n64/gdx_hud_replay.cpp`). Only the
place where a HUD region ends is known to the game, so the call sites have to be here.

**What it does not change.** No draw function is skipped, shortened or reordered: the HUD code
(its minimap toggle, sounds, finish timers) runs in full and writes its commands exactly as
before; the compaction happens afterwards and keeps every command that is not in the
recordable set in place. Unarmed (`[native] hud_batch = 0`, the default, or `interp = 0`, or
the `interp_verify` oracle on) `gdxhud_armed()` returns 0 and nothing is touched: the display
list is byte-identical to the unpatched game.

## decomp-native-track-resident-bake.patch (C5 STAGE 3c `[native] track_resident`)

Two calls in `src/game/course.c`, both under `#if defined(PORT) && defined(GDX_NATIVE_TRACK)`
(the shim header `gdx3ds_track_shim.h` declares them):

- **`gdxtrk_course_loaded()` at the end of `Course_Init`**, once `func_800A1954` has chunked the
  spline and the render distances are set. With `[native] track_resident=1` it bakes every
  course ring the per-frame walk can emit (`port/3ds/gfx/gdx3ds_track_resident.cpp`, using the
  decomp's own `Course_SetupChunk*Vtx` through `port/3ds/game/gdx3ds_track_bake_game.c`) into a
  GPU-resident buffer. It runs at every `Course_Init` (race, records, ending), so X Cup's
  generated courses and edited courses are baked at their own load. Lever off: an int test.
- **`gdxtrk_draw_enter()` at the top of `Course_Draw`**: the `[trkcat]` clock's start of the
  cull + grouping (armed only with `[debug] gputrace=1` or `mainprof=1` and `[debug] trkcat=1`).

**Why the decomp.** The bake needs the moment the chunks are complete, which only
`Course_Init` knows. Nothing in the game changes: neither call writes game state (the bake
saves and restores the two globals the setup functions write, `gCourseVtxPtr` and
`D_800F8930`, around each call, and runs before the first `Course_Draw`).

## decomp-native-sky-capture.patch (`[native] sky_retain` / `skyclock`)

`Background_Draw` (`src/overlays/ovl_i3/background.c`) is bracketed: `GDX_SKY_REGION_BEGIN()`
remembers where it starts, `GDX_SKYSEC(HEAD|GRAD|STARS|SPRITES|FLOOR|FIREWORKS|CLOUDS|END)`
writes one `G_NOOP` p=0x65 section marker at each of its sections, and `GDX_SKY_CAPTURE()` calls
`gdxsky_capture(start, gfx, cameraIndex, &identity)` (`port/3ds/gfx/gdx3ds_sky_capture.c`)
before it returns. The bracket is inside `Background_Draw`, so every caller is covered by one
patch site: the race's 1P/2P/3P/4P cameras (`race.c`), `records.c`, the ending and the course
editor. The macros exist only under `#if defined(PORT) && defined(GDX_NATIVE_SKY)` (the 3DS
game library defines it); elsewhere they are empty.

**Why the decomp.** Only the game knows where the sky's sections begin and end, and which
textures the sky draws with this frame (`gdxSkyIdentityFill`: the skybox/floor/cloud/star
textures, each sprite's texture and palette, the palette-replacement count, the course, the game
mode, the skybox flags). The markers let the owned walker time each section (`[skycnt]`); the
capture moves each run of recordable commands (RDP/RSP state, texture loads, rectangles,
triangles, and the segmented `G_MTX`/`G_VTX`) into a record behind one p=0x65 run marker that
the owned interpreter replays at the same position (`port/3ds/gfx/n64/gdx_sky_retain.cpp`); the
identity's generation keys every retained answer.

**What it does not change.** No draw function is skipped, shortened or reordered:
`Background_Draw` and its helpers (the sprite matrices, the night palette flash, the star
projection) run in full and write their commands exactly as before. Unarmed (`[native]
sky_retain = 0` and `skyclock = 0`, the defaults, or `interp = 0`, or the `interp_verify` oracle
on) `gdxsky_armed()` returns 0 and nothing is written: the display list is byte-identical.

## decomp-3ds-c5tick-dlb.patch (C5TICK: the per-tick DL build)

Listed last. Touches `src/game/game.c` (`func_80069698`), `src/overlays/ovl_i2/race.c`
(`Race_Draw`) and `src/game/racer.c` (`Racer_Draw`); includes
`port/3ds/gfx/gdx3ds_dlb_shim.h` only under `#if defined(PORT) && defined(GDX_PLATFORM_3DS)`,
elsewhere the macros are empty (`GDX_DL_SKIP` is 0) and the game is stock. Port side:
`port/3ds/gdx3ds_c5tick.{h,c}`.

**Two things.**
- `GDX_DLB(sec)` section markers: the `[c5dlb]` per-section build time (`[debug] dlbprof`) and
  the write-set check (`[debug] dlwrite`). One call + one int test per marker when both are off.
- `GDX_DL_SKIP(sec, view)` around the side-effect-free sections: in `Race_Draw` the
  `Background_Draw` / `Course_Draw` / `Course_GadgetsDraw` / `Camera_Draw` calls (every player
  count), in `Racer_Draw` the shadow, body (highlight + lights + machines + ghosts), booster
  and effects sections (skipped by `goto` to a label at the section's end, so the patch leaves
  the function body in place). The port answers 1 on a HOLLOW tick (frameskip drops this
  tick's task and `[debug] dl_hollow = 1`) or when a native path has consumed the section on
  rendered ticks (`gdx3ds_c5tick_consume`); 0 otherwise, so with both levers off the display
  list is byte-identical.

**What always runs (it changes game state), and why the rest may skip.** Static audit of every
callee plus the write-set check on the corpus (progress doc §2):
- `Race_Draw` head: `D_i2_80106F10--` (the clear countdown) -- always runs.
- `Racer_Draw` visibility pass: writes `machineLod` / `unk_2B2` / `unk_2B3`; the AI reads
  `machineLod != 0` (cpu.c:1082/1172, racer.c:2303/2352/2404/4701/4739) -- always runs.
- `Racer_Draw` shadow section's `else`: `gRacers[0..3].speed = 0` while `gRaceIntroTimer >= 280`
  -- kept in the skip branch.
- `Racer_Draw` HUD tail: countdown SEs 3/2/1/GO, the BGM start at `gRaceIntroTimer == 1`, the
  approach-warning SE and `soundEffectFlags` -- always runs.
- `Menus_Player1SpecialDraw`, `Menus_Draw`: AI inputs (`func_i3_801175A4`), race finalisation
  (`func_8008AA8C`), timers compared for equality, input edges, RNG, SRAM saves -- always run.
- `Transition_Draw`: outside `Race_Draw` (its FINISHED teardown is game state) -- always runs.
- Skippable: `Background_Draw` (reads only; its RNG fireworks are ending-cutscene only, and hollow
  is scoped to `Race_Draw`), `Course_Draw` + `Course_GadgetsDraw` (per-call cull recomputed for
  every chunk before it is read; the gadget pass reads the track's cull, so a consumer must take
  both), `Camera_Draw` (writes nothing), and Racer_Draw's shadow / body / booster / effects
  sections, which write the GfxPool, the effects vertex cursor (reset by `Race_Draw` every
  tick) and the `*MatrixInit` flags + `modelMatrix` (reset by the racer update every tick, read
  only by `Racer_Draw` after it recomputes them).
- A HOLLOW tick skips only sky / track / dressing / camera / shadows / bodies
  (`kHollowSkippable`): the booster and effects sections keep running because the debris and
  spark triangles read vertex fields no build writes (the previous build's bytes in that GfxPool
  half), so skipping them would change the next frame drawn from that half. With the body section
  skipped, every visible machine's model matrix is still built (`[debug] dl_hollow_poses`,
  default 1) for a pose-replay stage.
- Dynamic check (`[debug] dlwrite`, 32-byte blocks, lever off, canonical / 4p-vs /
  explosion-pileup / gp2-sector): sky and camera change no decomp byte; track/dressing only the
  course.c / course_gadgets.c per-call statics above; shadows/bodies only the gRacers draw scratch
  (and the intro speed clamp, which the skip branch keeps) and `math.c`'s `sDefaultMtxF` output
  sink; the HUD tail (always run) is where the audio command queue is written.

## decomp-3ds-c5sub-markers.patch (C5 STAGE 3b `[c5sub]`)

Diagnostic only. `GDX_C5SUB(piece)` writes ONE `G_NOOP` with p=0x62 and a sub-id at each piece
boundary of `Course_GadgetsDraw` (`src/game/course_gadgets.c`: landmines, jumps, the course
effects setup and each course effect by type, the gate/sign pass, the building pass) and of
`Racer_Draw`'s effects section (`src/game/racer.c`: boost flames and the seven effect classes).
The owned walker's noop hook (`port/3ds/gfx/n64/gdx_rt_glue.cpp`) times and counts each piece
(`[c5sub]` line on the `[c5cat]` cadence). Ids match `GDX3DS_C5SUB_*` in
`port/3ds/gdx3ds_mainprof.h`. Inside the `[native] fx` shim bracket a pending native fx run is
closed with its marker first (what the next class's setup list does anyway), so the run's replay
is charged to its own class.

**Why the decomp.** Only the game knows where one piece of dressing ends and the next begins.

**What it does not change.** Written only while `gdx_c5sub_armed()` (`[debug] c5sub=1` on top of
an armed `[c5cat]` clock: `gputrace=1` or `mainprof=1`); unarmed the display list is
byte-identical. The markers draw nothing on either interpreter.

## decomp-native-dress-shim.patch (C5 STAGE 3b `[native] gadget_batch` / `dress_retained` / `fxshd_direct`)

`src/game/course_gadgets.c`: the gadget pass of `Course_GadgetsDraw` (landmines, jumps, the
course effects) is bracketed by `gdx3ds_gad_shim_begin.h` / `_end.h` (push/pop_macro, like the
fx shim) with `GDXGAD_BEGIN(!IN_COURSE_EDITOR)` / `GDXGAD_END()`; direct-load scope markers
(`GDXDRESS_SCOPE`, G_NOOP p=0x6C) open and close around the gadget pass and the two decoration
passes. `src/game/racer.c`: the same scope marker around the effects section (after
`GDX_C5CAT(FX)`, before `GDX_C5CAT(HUD)`). All of it is under `#if defined(PORT) &&
defined(GDX_NATIVE_DRESS)` (the 3DS game library defines it).

**Why the decomp.** The gadget pass emits its vertices and triangles one small piece at a time
(`gSPVertex` + `gSP2Triangles` per mine / jump / dash plate / strip group); the recorder needs
those call sites, exactly as the fx shim does for Racer_Draw's effect classes. The scopes mark
where the dressing and effect passes begin and end, which only the game knows.

**What it does not change.** No draw function is skipped or reordered; the game logic in the
gadget pass (the per-chunk `drawState` culling, the ST wrap patches) runs as before. With every
lever at 0 (the default), `gdxgad_active` is 0, every shim macro emits the original GBI through
`port/3ds/game/gdx3ds_gad_gbi.c` (compiled with the same gbi.h), and no marker is written: the
display list is byte-identical. The course editor branch (EXPANSION_KIT) always runs stock.

## decomp-3ds-nrr-mach.patch (n60 NRR milestone 2 `[native] nrr_mach`)

`src/game/racer.c`: adds `Racer_NrrMachStub(gfx, sec, view)` in front of `Racer_Draw` (nothing in
`Racer_Draw` changes). The port registers it as the BODY section's consumer
(`gdx3ds_c5tick_consume`, `port/3ds/gfx/nrr/gdx3ds_nrr_mach.cpp`) when `[native] nrr_mach = 1`; it
then runs in place of the body section on rendered ticks (`GDX_DL_SKIP(BODY)`, the
decomp-3ds-c5tick-dlb.patch seam). It makes every game-state write the section makes -- each
visible machine's `modelMatrix` / `modelMatrixInit` (read by the booster section) and highlight
`attackHighlightMatrixInit`, the GfxPool matrices and the view's lights -- then copies what a
native draw needs into the port's per-(GfxPool, view) `GdxNrrMachFrame` and writes one
`G_NOOP p=0x63` marker. Racers the snapshot does not model (no complete record yet, crashed, the
LOD-6 speck, time-attack ghosts) are written as the section's stock commands, in stock order,
after the marker; the not-yet-recorded racers' commands are bracketed by `p=0x63` record markers.
Returns NULL (the section builds the stock way) in the GP ending cutscene and the course editor.

**Why the decomp.** The stub needs the section's statics (`sLastRacer`, the pipe/tunnel fog
tables) and must reproduce its writes exactly; it is the section's own code, regrouped.

**What it does not change.** Nothing runs unless the lever registered the consumer (default 0):
the function is never called and the display list is byte-identical.

## decomp-native-nrr-trk.patch (n60 NRR track, `[native] nrr_trk`)

`src/game/course.c` `Course_Draw`: one call before the far-to-near group walk (the `while (true)`
loop over `sSegmentChunkGroups`) and one label after it, both under `#if defined(PORT) &&
defined(GDX_NATIVE_TRACK)` (the shim header `gdx3ds_track_shim.h` declares the call). When
`gdxtrk_nrr_walk()` returns 1 the stock loop is skipped; the stock tail (the final `gSPTexture`
off, `GDXTRK_END`) runs either way. It returns 0 (stock loop, byte-identical list) unless
`[native] nrr_trk=1`, the pass is camera 0 of a 1P game inside `Race_Draw` (the c5tick section
markers), and the course has an NRR index (`[native] track_resident=1` bake).

**What replaces the loop.** `port/3ds/game/gdx3ds_nrr_trk_game.c`, compiled with the game's
headers, runs the same selection loop, the same two group walks (merge LOD included, same f32
expressions) and the ten shape emitters' decisions, and WRITES EVERY GLOBAL THE STOCK WALK
WRITES with the stock value: `D_800F89D4`, `D_800F892C`, `sLastTrackShapeType`, `D_800F8930`,
`D_800F8958[].chunk`, `D_800F89B8/BC/C0/C8`, `sWorkingSegmentChunk`, `sWorkingNextSegmentChunk`,
`sWorkingChunkJoinInfo`, `gCourseVtxPtr` (advanced by the same counts) and every group's
`averageDepth`. It emits the walk's texture-state GBI (`gSPTexture` pairs, `LoadSync`,
`SetTextureImage`, `LoadBlock`) verbatim through the shim's emitters, so the RDP state the gadget
pass inherits is the stock one, and the stock final `gSPTexture` follows. What it does not do:
write the rings into `courseVtxBuffer` (GfxPool scratch: only this list's own `gSPVertex` read
it) and emit `gSPVertex` / template `gSPDisplayList` / `gSPModifyVertex`; per material run it
writes one G_NOOP p=0x63 marker instead (the render side draws the run's resident rings).

**Side-effect proof.** (1) Static: nothing outside `course.c`'s walk reads `courseVtxBuffer`,
`gCourseVtxPtr` is reset by every caller before `Course_Draw` (race.c, records.c, ending.c, the
course editor) and read only by the walk; the walk globals are written with the stock values
(above); `Course_GadgetsDraw` reads `chunk->drawState`, which the cull writes before this call.
(2) Dynamic: `[native] nrr_trk=2` runs the lean walk dry, puts every global back, lets the stock
walk run recorded, and compares the stock walk's end-of-pass globals and every template op's two
rings (positions and final U/V, vertex for vertex) with the lean walk's (`[nrrtrk] verify ...
bad=0 globals_bad=0`). (3) `[gstate]` (+ `gstate_ext`) identical at every tick, `nrr_trk=0` vs
`1`, over the corpus.

**Fallback.** A group the bake cannot serve (a tunnel-mouth join, whose five join vertices
carry stale U/V; a ring variant outside the bake window) is rewound and walked by the stock
`Course_Draw{Forward,Backward}ChunkGroup` with the legacy recorder armed (the c5s3 retained
path draws it). Both slot chunks are forced to NULL around such a group (a fresh ring pair):
visual only, the walk globals are read by nothing but the walk.

## decomp-3ds-ships-census.patch (`[debug] ships_census` / `logic_split`)

`src/game/racer.c`: `Racer_GdxShipsCensus()` (in front of `Racer_Draw`) counts what the visibility
pass just decided -- active racers, visible ones by `machineLod` 1..6, shadows / flames / flame
trails / highlight passes by the draw loops' own conditions, crashed, time-attack ghosts -- and hands
the counts to `gdx3ds_ships_census_add` (`port/3ds/gdx3ds_ships.c`); one call before the shadow
section, under `[debug] ships_census`. `Racer_UpdateMovement` / `Racer_Update` get `GDX_LSPLIT`
marks (AI = `Cpu_GenerateInputs`, PHYS = `Racer_UpdateFromControls`, COLL = the pair-collision
loop, RPOST = the rest after the movement loop). `src/overlays/ovl_i2/race.c` `Race_Update`: begin /
end of the `[lsplit]` timeline and a mark before each phase (menus, effects, racers, camera,
background, course, func_80074844).

**Why the decomp.** The census reads `Racer` fields right after the pass that writes them; the
split's phases are call sites inside the race update.

**What it does not change.** Read-only: counters and `svcGetSystemTick` reads, receipts emitted by
the host loop. Off (the default) each site is one int test; nothing else runs, and game state, the
display list and `[gstate]` are identical either way.

## decomp-3ds-autodrive.patch (`[debug] autodrive`)

`Racer_UpdateMovement` (racer.c): with `[debug] autodrive = 1` player 1 takes
`Cpu_GenerateInputs` (the game's own post-finish autopilot branch) instead of the pad, so a
replay that only drives the menus finishes a race on the emulator lane (TT FINISH text,
post-finish HUD, results). Measurement lever, default 0; `#if defined(PORT) && defined(__3DS__)`.
Hook: `port/3ds/gdx3ds_autodrive.c`.

## decomp-3ds-prewarm.patch (`[native] prewarm`)

`src/game/racer.c` `Racer_NrrMachStub` (decomp-3ds-nrr-mach.patch): three hooks into
`port/3ds/game/gdx3ds_prewarm_game.c`. `gdx3ds_prewarm_mach_plan()` answers non-zero only in a
race's opaque appear frame (the load tick's list, drawn under `TRANSITION_TYPE_FADE` at alpha 255)
with the lever on; then the stub arms the recorder (`f->rec`), always writes the body prologue,
and after it `gdx3ds_prewarm_mach_bodies()` writes the stock body draw of every (machine in the
race, LOD 1-5) without a complete record, each followed by a far-speck draw, under one pose in
front of the camera; at the section's end `gdx3ds_prewarm_mach_hl()` writes the highlight pass of
the same set (`D_303A7D8`, blend colour, body list) and the body prologue again. The recorder,
the mesh bakes and the material tables see them as first sights.

**Why the decomp.** The draws are the stub's own stock commands; they belong in its stream, after
its prologue and before its legacy bodies (the speck-material rule reads the stock order).

**What it does not change.** With `[native] prewarm = 0` (the default), or in any frame but the
opaque one, `plan` returns 0 and the stub's output is byte-identical. No game state is written in
either case (the pose is a file-static `Mtx` in the port's game unit).

## decomp-native-finish-direct.patch (`[native] finish_direct`)

`src/overlays/ovl_i3/menus.c` `Menus_DrawFinish`: the `gSPDisplayList(aFinishDL)` of the 3D FINISH
banner (drawn after the finish line in GP and Time Attack, ~380 frames per race) is bracketed by
two `gdx_finish_scope_marker` calls (port/3ds/gfx/gdx3ds_dress_sink.c). With `[native]
finish_direct=1` and `fxshd_direct` armed they write the effects section's direct-load scope
markers (G_NOOP p=0x6C, `GDXDRESS_SCOPE_FX` / 0), so the banner's twelve LOADBLOCKs per frame take
that scope's exact path (resolved once, TMEM as debt, the import hash memoised per source).
Otherwise they write nothing and the list is unchanged. Under `GDX_NATIVE_DRESS` only (the 3DS
game target); other builds compile the stock call. Pure delta; applies last.


## decomp-native-finish-mesh.patch (`[native] finish_mesh`)

`src/overlays/ovl_i3/menus.c` `Menus_DrawFinish`, inside finish-direct's `GDX_NATIVE_DRESS` block:
right before `gSPDisplayList(gfx++, aFinishDL)` a `gdx_finish_mesh_marker` call
(port/3ds/gfx/gdx3ds_dress_sink.c) writes a G_NOOP p=0x6A when `[native] finish_mesh=1` and the
owned walker draws (not under interp_verify). The walker hands the next G_DL to the mesh registry,
which draws the banner as a retained GPU-transform mesh (gdx3ds_mesh.cpp, "finish_mesh"). Off, the
list is unchanged. Pure delta on top of decomp-native-finish-direct.patch; applies last.

## decomp-3ds-hud-placement.patch (Phase 2 `[hud] placement`)

`src/overlays/ovl_i3/menus.c` `Menus_Draw`: reads `gdx_hud_top_suppressed(gNumPlayers)` and
`gdx_hud_minimap_suppressed(gNumPlayers)` (port/input_bridge.c, cached into
`gdxHudTopSuppressed`/`gdxHudMapSuppressed` once per frame, same idiom as the existing
`gdxPhotoActive`/`gdxWideHud` locals) and ANDs them into the three call sites that draw the
top-screen race HUD: `Hud_DrawHud` + `Hud_DrawRacePortraits` (gated on `gdxHudTopSuppressed`) and
both `Minimap_DrawCourseMinimap` calls (gated on `gdxHudMapSuppressed`). The predicates fold in the
numPlayers>1 force-ALL_TOP rule themselves (port/gdx_hud_placement.h), so 2-4P is always
unaffected regardless of the lever. `[hud] placement = ALL_TOP` (default): both predicates always
answer 0 and the list is byte-identical to stock -- this is Gate 1 (bottom_race on/off, mode
ALL_TOP, fh+gstate byte-identical). `SPLIT`/`ALL_BOTTOM`: the suppressed elements leave the exact
pixel boxes they used to draw blank; nothing else on the top screen changes (Gate 2). No overlap
with `decomp-native-finish-direct.patch`/`decomp-native-finish-mesh.patch` (same file, lines
~1733-1758, this patch's lines are ~5546-5718) or with `decomp-native-menu-capture.patch`/
`decomp-native-menu-trans-capture.patch` (`src/game/game.c`, a different file -- the `nimenu`-owned
"menu-capture" patches this one must not touch). Pure delta; order-independent among those four.

## decomp-native-menu-capture.patch (nointerp S2 `[native] ni_menu`)

`src/game/game.c` `func_80069698`: the draw-table call `sGamemodeDrawFuncs[GET_MODE(gGameMode)](gfx)`
is bracketed (under `PORT && GDX_NATIVE_HUD`): the list start is kept and, after the draw function
returns, `gdxmenu_capture(start, gfx, mode)` (port/3ds/gfx/gdx3ds_menu_capture.c) compacts the region
like a HUD region (kind MENU, 0x5E markers) for every mode whose draw function is not `Race_Draw`, when
`[native] ni_menu != 0`. The draw functions run unchanged; off, the list is byte-identical. Pure delta.
`decomp-native-menu-trans-capture.patch` widens this same bracket; applies right after it.

## decomp-native-menu-trans-capture.patch (nointerp residue round)

`src/game/game.c` `func_80069698`: widens `decomp-native-menu-capture.patch`'s bracket to close after
`Transition_Draw(gfx)` instead of right after the mode's draw function, so both land in the same MENU
capture region. `gdxMenuStart`/`gdxMenuMode` are now taken unconditionally at function entry (same
pointer value either way, since `gfx` is not touched before the gate), but `gdxmenu_capture` only runs
when `gdxMenuArmed` -- set exactly where the original code's `if` gated the whole native-HUD branch --
is 1, so the capture fires under precisely the same condition as before. Before this patch,
`Transition_Draw`'s own words (the wipe/wide-stretch scissor and, on screens with a perpetual idle
background wipe, its per-frame fill/texrect chain) landed after the capture call and stayed on the
stock walker every frame (a measured "wipe background ~8/f" residue, present on every
`ni_menu` screen regardless of lever state because it was never inside the bracket to begin with). Race
modes are unaffected: `gdxMenuArmed` is still only set when the native-HUD branch's `if` ran, and
`gdxmenu_capture` itself still refuses race modes (`MenuIsRaceDraw`). `[native] ni_menu = 0`: the list
is byte-identical (capture is skipped either way, same as before). Pure delta; applies right after
`decomp-native-menu-capture.patch`.

## decomp-native-force-boot-mode.patch (`[debug] force_mode`)

`src/game/game.c` `func_800690FC`'s `GAMEMODE_CHANGE_INIT` case -- reached exactly once per process,
only from the `gGameMode == -1` check a few lines above -- gets one hook (under `PORT`) right before
`gGameMode = gQueuedGameMode;`: if `gdx_debug_force_boot_mode()` (port/3ds/gdx3ds_force_mode.c,
`[debug] force_mode`) returns `>= 0`, `gQueuedGameMode` is overridden to that `GameMode` (fzx_game.h)
before the assignment, so the game boots straight into it instead of `GAMEMODE_TITLE`. Reach aid for
screens with no menu path and no finishable replay (credits, the GP ending, the EAD demo); default
-1 = off, the list is byte-identical. MEASUREMENT ONLY -- a forced boot mode runs with no antecedent
race/cup state, so its content is real code on its real path but not a result a player could produce.
Pure delta; applies last.

## decomp-native-gp-end-setup.patch (`[debug] force_mode` GP-ending racer setup)

`src/game/game.c`: a cold `force_mode` boot into `GAMEMODE_GP_END_CS` leaves every racer-standings
global (`gRacerPositionsById[]`, `gRacers[]`, `gPlayer1OverallPosition`, ...) at its BSS-zero default.
`Racer_Init`'s `GAMEMODE_GP_END_CS` branch (racer.c) does not expect that: it searches
`gRacerPositionsById[]` for the racers placed 1st/2nd/3rd and indexes `gRacers[]` at wherever the
search stops; an all-zero array never matches, so the search runs off the end and reads
`gRacers[TOTAL_RACER_COUNT]` -- one past the valid 0..29 range. This adds `gdx_force_mode_setup_gp_end_cs`
(called from the same one-shot `force_mode` hook `decomp-native-force-boot-mode.patch` adds, only when
`gdxForcedMode == GAMEMODE_GP_END_CS`): a minimal, safe 3-racer 1st/2nd/3rd placement
(Captain Falcon/Dr. Stewart/Pico, all stock machine skins) at `STANDARD` difficulty / `JACK_CUP`, which
avoids the OOB read and exercises both the podium and the congratulations-text draw paths. Off
(`force_mode` unset, or set to any other mode), nothing changes. Applies right after
decomp-native-force-boot-mode.patch, before decomp-native-gp-end-mach.patch.

## decomp-native-gp-end-mach.patch (GP ending native machine-body coverage)

`src/game/racer.c`: `Racer_NrrMachStub` (the N60MACH machine-body native-coverage stub,
`[native] nrr_mach`) declined with an early `return NULL` whenever `gGameMode ==
GAMEMODE_GP_END_CS`, forcing the GP ending cutscene's machine-body geometry back onto the full
stock/interpreted draw. That exclusion was never load-bearing: `EndingCutscene_Init` places its
racers through the same `Racer_Init`/`Course_Init` path a real race uses, so the section's
snapshot (model/highlight matrices, GfxPool lights, the native-draw marker) is exactly as valid
during the ending as it is mid-race. Removes the `GAMEMODE_GP_END_CS` check, keeping only the
course editor's `GAMEMODE_COURSE_EDIT` exclusion (now `#ifdef EXPANSION_KIT`-only, since that mode
does not exist outside the Expansion Kit build). Measured ~1240 cmds/f of stock-walked
machine-body geometry in the "gp_end_cs/steady" bucket before this fix, dropping to native
coverage after it. Applies right after
decomp-native-gp-end-setup.patch, before decomp-3ds-nimach.patch (same stub, same file).

`src/overlays/ending/ending.c`: that stub alone was not reachable here -- `EndingCutscene_Draw`
calls `Racer_Draw` directly, outside `Race_Draw`'s own `RHEAD..TRANS` bracket, so
`gdx3ds_c5tick.c`'s `sRaceScope` stays false for the whole mode and `gdx_dl_skip` never calls any
registered consumer stub at all (`stub=0 markers=0` for a full run, confirmed by instrumentation).
Wraps the `Racer_Draw(gfx, 0)` call in `gdx_dl_scene_mach(1)`/`(0)` (new, `gdx3ds_c5tick.c`/`.h`),
a scene scope armed by `gdx3ds_nrr_mach_armed()` (new one-line accessor, `gdx3ds_nrr_mach.cpp`) --
the mach category's own settled `[native] ni_mach` state. Deliberately a separate entry point from
`gdx_dl_scene` (the records flyby's own scope, `decomp-native-records-scene.patch`, armed by
niview's `gdx3ds_nrr_scene_ok()`): an earlier draft OR'd `gdx3ds_nrr_mach_armed()` into
`gdx_dl_scene` itself, which would have also opened the flyby's scope whenever `[native] ni_mach`
is armed regardless of niview's own records bit, letting the mach BODY stub claim sections of a
scene it has no business touching. Two named functions make that cross-talk structurally
impossible instead of relying on a convention. `[nrr] mach` went from `stub=0` to `stub=64` per
64-frame window (every frame, all 3 podium racers) once this landed; nicensus residue dropped
~15-18% (`walkW` 1044.1 -> `walkB` ~859-861 at frames=17,999). Gated on-vs-off (49/49 SHOT
checkpoints, byte-identical every frame) and the records flyby on-vs-off with `[native] ni_mach`
armed vs not (unaffected, by construction).

## decomp-3ds-nimach.patch (nointerp S5b `[native] ni_mach`)

`src/overlays/ovl_i4/machine.c`, 3DS port builds only (not the Expansion Kit): with `[native]
ni_mach` bit 1 (`gdx3ds_nimach_level()`, latched by the NRR mach category: needs `nrr=1 nrr_mach=1`),
`MachineSelect_MachineDraw` keeps its stock prologue (projection, `D_303A5F8`, the lights and
`Lights_SetSource`) and then, instead of the 30-slot loop, `MachineSelect_NimachMachines` snapshots
every slot whose (body list, menu pass) has a complete NRR record -- its GfxPool matrix, its `Vp`,
its light set and `func_8009CCBC`'s env colour -- into one `GdxNrrMachMenuSnap` and writes ONE
`G_NOOP p=0x63` marker (`port/3ds/gfx/nrr/gdx3ds_nrr_mach.cpp`, menu kinds). Slots without a record
follow the marker as the loop's stock commands (the marker arms the recorder for them: their legacy
draw is the record pass); the RSP state the stock loop leaves (slot 29's lights, viewport, matrix,
env colour) is written last. No game state is written (the stock loop writes none). Lever 0 (the
default), or the snapshot not taken: the stock loop, byte-identical. Pure delta; applies last.

## decomp-port-title-demo-tick.patch (niview — attract demos under det=1)

Pure delta on top of `decomp-port-title-demo-audio-pin.patch` (same two functions,
`src/audio/rom/external.c:func_800B84B8` and `src/audio/disk/external.c:func_807442E4`). Under the
audio pin (det=1) the attract demo never fires; this adds one PORT call at the top of each function:
`gTitleDemoState = gdx_det_title_demo_step(gGameMode == GAMEMODE_FLX_TITLE, gTitleDemoState)`
(`port/gdx_determinism.c`). With `[debug] det_title_demo = N` (> 0) the demo is armed after N
consecutive updates on the title with the demo inactive, and ended (START_EXIT, the audio window's
own write) after `det_title_demo_len` (default 1500) updates of an active demo outside the title.
Default 0: the step returns the state unchanged, so every existing replay is untouched; unpinned
(real play) the call is not made. Used by `replays/attract-demos.gdxrec`.

## decomp-native-records-scene.patch (niview — the records flyby on the NRR)

Pure delta on top of the stack (only `src/overlays/records/records.c`, which no other patch edits).
`Records_Draw` draws the flyby's sky, course and dressing outside Race_Draw, so the c5tick consume
stubs and the track's race-scope check never saw it. Under `PORT && GDX_PLATFORM_3DS`: the shim header,
`gdx_dl_scene(1)` before `Background_Draw` and `gdx_dl_scene(0)` after `Course_GadgetsDraw`, and the sky
and dressing calls behind `GDX_DL_SKIP(SKY, 0)` / `GDX_DL_SKIP(DRESS, 0)`. The scene scope
(`port/3ds/gdx3ds_c5tick.c`) opens only when `[native] ni_view` has the records bit (0x400) and the NRR
master is on (`gdx3ds_nrr_scene_ok`); closed, `gdx_dl_skip` returns 0 for every section and the
function builds exactly the stock commands. Open, only a section with a consume STUB can skip, the stub may
decline (NULL: stock), the hollow-tick path never applies (hollow stays Race_Draw's), and the course
itself is never skipped (the gadget pass reads its cull; the NRR track runs inside Course_Draw).
Gameplay: the three sections are pure (c5tick §3.1); Records_Draw's state writes (none in these calls
beyond the GfxPool and the c5tick-proven scratch) are unchanged.

## decomp-native-course-preview.patch (`[native] ni_course`)

`src/overlays/course_select/course_model.c` `func_i5_80115E64` (the course select / GP next-course
card / records course select 3D course preview): each `Course_DrawModel` call is bracketed by
`gdx_course_model_mark_begin` / `gdx_course_model_mark_end` (port/3ds/game/gdx3ds_course_game.c).
With `[native] ni_course != 0` (owned walker, not under interp_verify) begin reserves two Gfx before
the model's G_VTX / G_TRI2 run and end fills them with a G_NOOP p=0x6B pair (vertex buffer, command
count, vertex count, content generation); off, both return / write nothing and the list is the stock
list. `src/game/course.c` `func_800A2D2C` (writes a model's vertex buffer) bumps that generation on
entry. The owned walker draws the run from a GPU-resident mesh (port/3ds/gfx/n64/gdx_nicourse.cpp,
gdx3ds_course_mesh.inc) or walks it. Pure delta; applies last.

## decomp-3ds-ek-nimach.patch (ekperf3 `[native] ek_nimach`, Expansion Kit only)

`src/overlays/ovl_i4/machine.c`, every hunk inside `#if defined(PORT) && defined(GDX_PLATFORM_3DS) &&
defined(EXPANSION_KIT)`, so a non-EK build preprocesses exactly as before. The EK twin of
`decomp-3ds-nimach.patch`, whose blocks are `!defined(EXPANSION_KIT)`: `MachineSelect_EkNimachMachines`
(machine select, ni_mach bit 1) and `MachineSettings_EkNimachBodies` + the reflection model marker
(machine settings / GP next machine, bit 2), gated by `gdx_ek_nimach_level()`
(port/3ds/game/gdx3ds_ek_nimach_game.c: 0 unless `[native] ek_nimach = 1`, then the live ni_mach
bits). The EK loop's differences are kept: `G_RM_PASS / G_RM_AA_ZB_OPA_SURF2` is written before the
marker and after the last slot (the body lists never set a render mode), the env colour is the EK
`func_8009CCBC`'s (grey while `gGreyscaleMachinePart`), a player-designed machine
(`CUSTOM_MACHINE_EDITED`) is always a stock slot, and the reflection model marker is written only for
a machine with no custom state (`func_8009CEA0` draws its one stock list). Lever off: level 0, the
stock loops. Pure delta on top of decomp-3ds-nimach.patch; applies last.

## decomp-3ds-ek-editmesh.patch (ekeditmesh `[native] ek_editmesh_track`, Expansion Kit only)

`src/game/course.c`, every hunk inside `#if defined(PORT) && defined(GDX_NATIVE_TRACK) &&
defined(EXPANSION_KIT)`, so a non-EK build preprocesses exactly as before. `func_800A1954` (the
chunk builder) bumps the editor preview's generation on entry. `func_800A95B4` (the course editor's
course-surface preview walk) asks `gdx_ek_editmesh_begin` (port/3ds/game/gdx3ds_ek_editmesh_game.c)
whether to bracket its chunk loop with `GDXTRK_BEGIN(0)` / `GDXTRK_END()`, as `Course_Draw` does,
and re-bakes the resident rings once the generation has settled; both returns go through
`GDX_EKMESH_LEAVE`. While the `[c5cat]` clock is armed it also writes the TRK/MISC category words
around the walk. Lever off: begin returns 0 and the list is stock. The ground grid and the editor's
lines need no decomp change: `[native] ek_editmesh` retains them in the owned walker.
Pure delta; applies last.

## decomp-3ds-mp-fullview.patch (MP phase 3: `[net] mp_fullview`, `[debug] fullview_player`)

In a 2-4P VS race, show only the local player's view k full screen, with the 1P race HUD. Every hunk
sits under `#if defined(PORT) && defined(GDX_PLATFORM_3DS)` or goes through
`port/3ds/gfx/gdx3ds_fullview_shim.h`, whose macros are constants elsewhere; with both levers off
`gdx_fv_k()` is -1 and every hunk is the stock code. Contract: the SIMULATION KEEPS ALL N PLAYERS.
Every view still runs its camera, `Racer_Draw` (machineLod feeds the RNG and the CPU AI) and every
HUD / menu call; only display-list output changes.

- `ovl_i2/race.c` `Race_Draw`: latch k at the top (`gdx_fv_race_draw_begin`, -1 during the VS
  end-of-race viewport transition so the stock results animation runs), release at the end. The
  other views' sky / track / dressing / bodies / effects are dropped by `gdx_dl_skip`
  (port/3ds/gdx3ds_c5tick.c), not here.
- `game/camera.c` `Camera_Draw`: view k's viewport + scissor become `SCISSOR_BOX_FULL_SCREEN` and it
  takes the 1P scissor inset; projection and frustum are untouched (already 4:3 in every count).
- `game/racer.c` race-HUD tail: view k swaps the camera's 2D mirrors to full screen for the markers
  and takes the 1P marker textures; other views' marker output is rewound.
- `ovl_i3/hud.c` `Hud_DrawHud`: fullview draws `Hud_GdxFullviewDrawHud` — the case 2-4 call order
  with k on the 1P layout (the [0][k] position-table slots get the [0][0] values) and every other
  player's draws rewound. `Hud_DrawPosition` draws k's rank 12 px left of the 1P slot, because VS
  has one digit and "/4", so the "/4" would otherwise touch TIME. `Hud_GdxFullviewDrawPortrait`
  draws k's portrait and place label in the 1P portrait slot and does nothing else. Stock
  `Hud_DrawRacePortraits` would draw the race leader and advance portrait state. Lives and KO
  stars are mode-gated to GP/practice/death race and are skipped.
- `ovl_i3/menus.c`, `minimap.c`: layout switches read `GDX_FV_NP` / `GDX_FV_NPI` (1P for k); other
  players' per-player output and the 3P filler box / 4th minimap are rewound; k's minimap is drawn
  at the 1P spot and 1P size. The 0.75 split-screen raster is magnified by the texture-rectangle
  step, which is exactly 1<<10 when the lever is off.
- `ovl_i3/background.c` `Background_DrawStars`: star points map from view k's quadrant to full screen.
- `audio/{rom,disk}/lib/thread.c` `AudioThread_QueueCmd`: `gdx_fv_audio_cmd` centres the local
  player's per-player channels (pan 0x3F) and scales the others' volume by 0.4. Only the queued
  command's argument changes, so the sim never sees it.

Gate: `[gstate]` hash + ext per tick identical, split vs every k, on race-4p-vs and
race-vs2p-results (det=1, old and new model). Applies last.

## decomp-native-hud-elem-gate.patch (Phase 3 per-element top-screen gate)

Builds on `decomp-3ds-hud-placement.patch`: turns the top-screen HUD's all-or-nothing suppression
into a per-GDX_HUD_ELEM_* gate (port/gdx_hud_placement.h/.c), so a future CUSTOM mode can show some
elements on top and hide others, instead of the whole bundle moving together. Two files:

- `src/overlays/ovl_i3/hud.c` `Hud_DrawHud`: computes `gdxTopMask1P = gdx_hud_top_element_mask(1)`
  once (same idiom as the existing `gdxWideHud`/`gdxWideSplitHud` locals), then wraps each
  `case 1:` (1P) piece in its own `if (gdxTopMask1P & kGdxHudBit*)` -- TIME (`Hud_DrawTimeRectangle`
  + `Hud_DrawDeathRaceBest`/`Timer`/`BestTime`), the `Hud_UpdatePlayerHudInfo` vs.
  `Hud_AdvanceLapAnnouncementsOnly` side-effect-preserving swap (also TIME, same pattern the
  menus.c-level swap already uses when the whole function is suppressed), INTERVAL
  (`Hud_UpdateRaceIntervalInfo` + `Hud_DrawPracticeBestLap`/`LapDelta`), ENERGY
  (`Hud_DrawEnergyOutlineRectangle` + `Bar`), LAP (`Hud_DrawLapRectangle` + `Counter`), REVERSE,
  LIVES, KO_STARS. `case 2:`/`3:`/`4:` (2-4P) are UNTOUCHED: FORCED ALL_TOP IN 2-4P
  (`gdx_hud_placement.h`) means every bit they'd ever test is already set, so there is nothing to
  gate there and zero bytes to add. The `kGdxHudBit*` values and the `gdx_hud_element_on_top`
  element numbers used in menus.c below are a LOCAL mirror of `GDX_HUD_ELEM_*`, not an #include
  (decomp/src has no port/ include path); `port/tests/gdx_hud_placement_host_tests.c` pins the
  real enum's numeric values so a future reorder there fails the host test instead of silently
  mis-gating a piece here.
- `src/overlays/ovl_i3/menus.c` `Menus_Draw`: RANK/SPEED/PORTRAITS already had their own
  individual `if` at their own call site (never routed through `Hud_DrawHud`), so each one's
  `!gdxHudTopSuppressed` condition becomes its own cached `gdxHudRankOnTop`/`SpeedOnTop`/
  `PortraitsOnTop` (`gdx_hud_element_on_top`, one call each per frame, read in the per-racer loop
  exactly like the pre-existing `gdxHudTopSuppressed`/`gdxHudMapSuppressed` locals). MINIMAP is
  unchanged (`gdx_hud_minimap_suppressed` already independent pre-Phase-3; now just reads
  `GDX_HUD_ELEM_MINIMAP`'s bit under the hood, see port/gdx_hud_placement.c).

Modes 0-2 (ALL_TOP/SPLIT/ALL_BOTTOM) must answer EXACTLY as `decomp-3ds-hud-placement.patch` did
before this patch existed -- the host test's `test_modes_0_to_2_match_legacy` checks every
(mode, element, numPlayers) combination those modes can reach against the pre-Phase-3 rule
independently re-derived, not against the new code's own mask. CUSTOM (mask-driven, not reachable
through the MODE chip yet -- `gdx_hud_set_custom_top_mask`, dev ini lever only) is the new
behaviour this patch exists to unlock; FORCED ALL_TOP IN 2-4P holds for it too (`numPlayers<=1`
gate inside `gdx_hud_top_element_mask`). Applies after `decomp-3ds-hud-placement.patch` (same
functions, later context) and after `decomp-native-hud-lap-announce.patch` (needs
`Hud_AdvanceLapAnnouncementsOnly` to already exist in `hud.c`); applies last in the stack.

## decomp-3ds-ek-audio-disk-async.patch (`[debug] audio_disk_async`, o3dsekperf)

Expansion Kit only (`src/audio/disk/lib/load.c`). `AudioLoad_ReadWriteDisk` hands a request from
the ASYNC disk loader -- identified by its queue, `D_806F2350.mq`, the one the
`AudioLoad_DiskLoad` state machine polls with `OS_MESG_NOBLOCK` -- to
`gdx_leo_read_async()` (port/n64_leo.c), which runs the unchanged `LeoReadWrite` on the I/O
worker in port/3ds/game/gdx3ds_disk_async.c and posts that queue when the bytes land. Every
other request (the synchronous `AudioLoad_DiskDrive`, queue `D_806F2328`) still runs inline.
The reset busy-drain in `AudioLoad_ProcessAsyncLoadDiskDrive` waits for the worker
(`gdx_leo_async_wait_idle()`) instead of spinning on the queue.

Why: the async loader was only async on console. `audio_disk_step_kb` (load-census patch) cut a
course load to one LBA per producer tick, but that LBA's SD read still ran INSIDE the tick:
emulator, ek-ta-dd, `[audioload]`: 141 consecutive race-load ticks with diskld/leorw 4.3-9.8 ms
each; hardware (release-v1@78ff524f, Old 3DS, JP disk): producer ticks `for=24-30 ms` against
the 16.7 ms a tick produces, and the ring under-runs through the race intro. With the lever off
(`gdx_leo_read_async()` returns 0) the handler runs inline exactly as before. Default 1 on the
Old 3DS, 0 on the New 3DS (port/3ds/include/gdx3ds_disk_async.h).

ekaudio2 extends the hook by one argument: the machine's `bytesRemaining` goes to
`gdx_leo_read_async()` as the read-ahead hint for `[debug] audio_disk_readahead_kb` (the bytes the
load still wants after this request), so the worker can fetch the next LBAs in the same SD
request.

## decomp-3ds-ek-audio-sync-bulk.patch (`[debug] audio_sync_bulk_kb`, ekaudio2)

Expansion Kit only (`src/audio/disk/lib/load.c`), 3DS only. `AudioLoad_SyncDma` offers a
cartridge load to `gdx3ds_ek_audio_sync_bulk()` (port/3ds/game/gdx3ds_ek_audio_bulk.c) before its
0x400-byte DMA walk; a load inside the streamed `audio_table` window is then read straight off
`fzerox.o2r` into the destination in requests of up to the lever's size, and the walk is
skipped. A decline (lever 0, stream not armed, `audio_stream_verify`, outside the window,
16 KB or less, a failed read) runs the walk exactly as before. Not taken while `resetTimer > 16`, where
`AudioLoad_Dma` itself transfers nothing.

Why: the first audio tick of an EK boot is the persistent-sample load, and with the stream armed
every 16th DMA of that walk is one 16 KB SD request on the producer (rc2 hardware log, Old 3DS: a
`max=1054914us` tick before the boot prime timed out and under-ran; 18-21 ms per 16 KB fill).
Default 256 (KB per request) on the Old 3DS, 0 on the New 3DS; `[debug] audio_sync_bulk_verify=1`
re-reads every bulk range through the stream and compares (receipt `[audiobulk] verify ok= bad=`).
Applies after
`decomp-3ds-ek-audio-disk-async.patch` (same file).

## decomp-3ds-ek-testdrive-scope.patch (S5b: the course editor's test drive on the NRR)

Expansion Kit only in effect (`course_edit/` is an EK overlay; the `racer.c` hunk is inside
`#ifdef EXPANSION_KIT`). The editor's TEST DRIVE draws a 1P race -- sky, course, gadgets, the one
racer, minimap, speed -- from `func_xk2_800DF6FC` (`course_edit/191080.c`), never through
`Race_Draw`, so the c5tick race scope (`RHEAD .. TRANS`) never opened for it and every NRR
category drew it on the interpreter. Two files:

- `src/overlays/course_edit/191080.c` `func_xk2_800DF6FC`: the test-run branch calls
  `gdx_dl_testdrive_scope()` first (port/3ds/gdx3ds_c5tick.c: opens the race scope exactly as
  `RHEAD` does, only under `[native] nrr_testdrive=1`, default 0; never hollow), and its
  `Background_Draw` / `Course_Draw` / `Course_GadgetsDraw` calls take `Race_Draw`'s own 1P
  `GDX_DLB(SKY|TRK|DRESS)` + `GDX_DL_SKIP(..., 0)` brackets, plus `RMID` around the racer setup so
  the section clock charges it like the race. `game.c`'s `TRANS` marker closes the scope after the
  mode's draw. With the lever off the scope never opens, `GDX_DL_SKIP` returns 0 outside it, and
  the display list is the stock one.
- `src/game/racer.c` `Racer_NrrMachStub`: the `GAMEMODE_COURSE_EDIT` decline (nimach) keeps
  declining every course-edit draw except one inside the test drive's scope
  (`gdx3ds_c5tick_testdrive_scope()`): `func_xk2_800DEE20` placed that racer with `Racer_Init` +
  `Course_Init`, as a race does. The port's shadow / effects / dressing stubs take the same scope
  (`gdx3ds_nrr_{shd,fx,dress}_game.c`). The track needs nothing: `Course_Init` re-bakes the
  resident rings at the start of every test run (`decomp-native-track-resident-bake.patch`), so
  each run's edited course gets its own `trkres` generation.

Applies after `decomp-3ds-nrr-mach.patch` and `decomp-3ds-nimach.patch` (the stub and its decline);
listed after the two EK audio patches (independent files).

## decomp-3ds-mp-online.patch (`[net] mp_bot`, online multiplayer tests)

`Racer_UpdateMovement` (racer.c), inside the `decomp-3ds-autodrive.patch` branch: when
`gdx_autodrive_mix(racer->id)` answers non-zero (port/3ds/gdx3ds_autodrive.c; only for a racer
an online session's START marked as a test bot, `[net] mp_bot = 1`), the CPU driver's inputs take
a quarter of the player's exchanged stick X and its R/Z lean, so a bot race still depends on every
console's pad. Off (no session, or no bot) it is one call returning 0. Applies last (after the
autodrive patch it extends).

## decomp-3ds-mp-sim.patch (lockstep: the simulation never reads draw state)

`machineLod` is written by `Racer_Draw` -- per view, so it ends up holding the LAST view drawn,
and it keeps a stale value on a frame the port skips drawing. Two places in the simulation read
it here: `Racer_ReceiveDamage` (racer.c: a machine out of every view is never destroyed, its energy
is held at 0.1) and `Cpu_BehaviorBilly` (cpu.c: that CPU driver only spins when seen); the other
seven reads are routed by decomp-3ds-mp-vis.patch (below), so all nine go through one function. In a
lockstep session (`gdx3ds_mp_active()`: `[net] mp` or an online race) two consoles draw
different views, or skip different frames, so both reads go through
`gdx_sim_machine_visible()` (port/3ds/gdx3ds_autodrive.c), which answers "visible" for every
machine while a session runs and `machineLod != 0` otherwise: off a session the game is
unchanged. Applies after decomp-3ds-mp-online.patch.

## decomp-3ds-widescreen-cull.patch (`[display] widescreen`: the racer cull, simulation-safe)

`src/game/racer.c` `Racer_Draw`, every `PORT` build. The PC port's `UltrawideMode` widens the
racer visibility test from the 4:3 NDC band (+-1.0) to the hor+ band (+-`gdx_get_ultrawide_cull_xscale()`,
1.25 on the 3DS's 5:3 top screen), and that test does more than pick what is drawn:
`racer->machineLod != 0` is read by the racer update as "on screen" (spark / explosion /
spin-out effects that draw `Math_Rand1/2`, racer.c ~4751/4789/4885/5304) and by the CPU AI
(`ovl_i3/cpu.c:1082/1172`). Widening it as-is would move the RNG streams and the race.

The patch keeps the wider test for DRAWING and hands the simulation the stock answer: a machine
that passes the widened x test (and the stock y/depth tests) but fails the stock +-1.0 x test is
recorded (`gdxWideOnly[]`, the main pass and the time-attack ghost pass), drawn like any other
visible machine (shadow, body, highlight, boosters, markers), and at `Racer_Draw`'s single return
gets `machineLod = unk_2B2 = unk_2B3 = 0` -- exactly what the stock pass leaves on a culled
machine (the 3P/4P `machineLod++` and `unk_2B3` step-downs never move a 0). `Racer_Draw` runs per
view, so each view's call restores before the next view's pass recomputes. Nothing after the
restore draws.

With `gdx_get_ultrawide_cull_xscale()` at 1.0 (every configuration but `[display] widescreen = 1`
in a race; `port/3ds/gdx3ds_widescreen.h`) no machine can pass the wide test and fail the stock
one, so the list stays empty and the display list is the stock one. Measured: `[gstate]` (and
`gstate_ext`, which hashes the visibility bits) identical with the lever on and off. Credit: the
widened cull itself is the G-Diffuser PC port's (Zorkats/G-Diffuser, decomp f7fd0fd, MIT/CC0).
Applies last, after the mp-* patches (rebased onto decomp-3ds-mp-fullview.patch, which also edits
`Racer_Draw`'s tail: the restore stays at the single return, after mp-fullview's HUD drop); touches only `Racer_Draw`.

## decomp-3ds-ek-nodisk-audio.patch (EK without its disk: no disk, no disk audio)

`src/audio/disk/lib/load.c` and `src/audio/disk/external.c`, the Expansion Kit's disk audio
library only (files `GdxGameSources.cmake` excludes from the shipped EK=OFF source list, so **no
byte of the default build changes**). While no disk is mounted (`gdx_leo_disk_mounted()`,
port/n64_leo.c: the resident buffer or the SD stream is live):

- `AudioLoad_SyncLoad` and `AudioLoad_AsyncLoad` refuse a `MEDIUM_LBA` entry before anything is
  allocated, the way an allocation failure already fails the load (capped
  `[audio-disk] SyncLoad/AsyncLoad REFUSED no disk` receipts). The async refusal still posts its
  completion to `retQueue`, exactly as the cache-hit path does, so `Audio_DDBgmReadyInter`'s
  sample-bank wait (`D_80771C7C`) is released; the load status stays not-loaded.
- `Audio_CheckBgmLoad` stops waiting for a disk BGM that can never load (`sBgmRomLoading = false`).

Why: with no `.ndd` every LBA read fails with `DRIVE_NOT_READY` and copies nothing, yet the loader
marked the entry loaded, so the sequence player ran whatever the audio heap held. On
test-combined's n3ds-ek (lever-off menus-longtail, no disk staged) that stale data spun the
dedicated audio producer forever inside `AudioSynth_Update` while it held `sAudioCtxMutex`, and
the game fiber parked behind it in `AudioThread_QueueCmd` (watchdog
`frame=127(+0) fiber=5 aud=151/150 astage=1 ctx=0+1`). The heap's contents follow the binary's
layout, so the hang came and went with unrelated merges (mp-online's exposed it). Filling the
destination with defined data (sequence = END, font/bank = 0) was tried and still hung
(garbage table sizes/fonts); refusing the loads without the two releases above froze the race
countdown at "1", because `func_807424CC` (the countdown at `gRaceIntroTimer == 40`, and
`sys_gfx.c`) waits for the BGM load. With a disk mounted nothing changes. Applies after
`decomp-3ds-widescreen-cull.patch` (before the ek-vs-onepad and settings-backdrop-full fixes).

## decomp-3ds-ek-vs-onepad.patch (EK main menu: VS BATTLE needs two controllers)

`src/overlays/ovl_i6/main_menu.c` `MainMenu_Update`, `EXPANSION_KIT` + `PORT` builds only (the
non-EK arm is untouched, so O3DS/N3DS builds compile the same code). Hardware report: "versus
mode doesn't do anything". The US arm snaps the SELECT MODE cursor off `MODE_VS_BATTLE` onto
`MODE_DEATH_RACE` while `gControllersConnected < 2`; the EK arm (the JP disk game's grid, built
here as `VERSION_US`) never had that rule, so on a 3DS -- one pad -- the cursor could rest on VS,
and A opened the player-count step only for case 1's `VERSION_US` clamp to bounce it back to the
grid with the cancel sound. The patch gives the EK arm the US rule, before its no-disk
Course Edit / Create Machine redirects. `[debug] pads=N` (N >= 2) and a multiplayer session (the
input bridge reports the remote ports connected) still reach VS. Applies last.

## decomp-3ds-settings-backdrop-full.patch (`[stereo] menu_depth`: the settings backdrop fills the frame)

`src/game/object.c` `OBJECT_MACHINE_SETTINGS_BACKGROUND`, `PORT` + `__3DS__`, runtime-gated on
`gdx3ds_stereo_menu_backdrop_full()` (port/3ds/gfx/gdx3ds_stereo.cpp: 1 with `[stereo] enabled`
and `menu_depth` both on). Hardware report, 3D on: "on the machine settings page the background
doesn't fill the screen". The page's flat grey (24,24,24) is the stock overscan-safe card
`func_8007AC48` draws, 12..307 x 8..231, clipped by the menu scissor of the same box: on the top
screen a grey card in a black frame (emulator, every leg: x 40..347, y 8..231 of 400x240). With
menu_depth the card is a backdrop-sized draw that touches no screen edge, so the layout pushes
it 4 px behind the screen and its free right edge lands on a different column in each eye
(345 vs 349). With the lever on the patch opens the scissor, fills the whole frame with the same
colour (`func_8007AD44`, 0..319 x 0..239) and restores the menu scissor (12, 8, 308, 232, as
`MachineSelect_BackgroundDraw`'s widescreen arm does). A frame-wide draw touches the screen
edges, so the stereo layer's edge rule keeps it on the plane: both eyes fill 0..399 x 0..239,
every other pixel of the page unchanged. Lever off (the shipped default): the stock card, the
same display list as before. Applies last.

mp-polish: the gate no longer needs menu_depth. Only the New 3DS ini turned menu_depth on, so the
Old 3DS (and any 2D build) still showed the grey card in a black frame (hardware: "old 3ds still
had the non-filled grey background"). `gdx3ds_stereo_menu_backdrop_full()` now answers 1 with
menu_depth on, else `[ui] settings_backdrop_full` (default 1; 0 = the stock card as before).
Menu display list only: no simulation state is touched.

## decomp-3ds-mp-vis.patch (MP30: every simulation read of `machineLod` is sim-derived)

Extends decomp-3ds-mp-sim.patch to the seven reads it left: `Racer_ReceiveDamage`'s crash
debris (racer.c), `Racer_HitWall`'s wall sparks, `Racer_UpdateFromControls`'s spin-out debris
(three `Math_Rand1` + one `Math_Rand2` per spawn), its crash explosion and its random body sparks
(six more `Math_Rand*` calls), `Racer_Update`'s pair-collision sparks, and `Cpu_GenerateInputs`'s
`sp70` branch (cpu.c). Each becomes `GDX_SIM_RACER_VISIBLE(racer)` / `GDX_SIM_VISIBLE(racer)`,
i.e. `gdx_sim_machine_visible(racer->machineLod)` (port/3ds/gdx3ds_autodrive.c): `machineLod != 0`
outside a session (the stock expression, so single player is unchanged) and 1 for every machine
in one. Constant 1 is the stock VS behaviour, not a new rule: `Racer_Update` already sets every
racer's `machineLod` to 1 at the top of each tick whenever `gNumPlayers != 1`, before every one of
these reads, so a 2-4 player race never saw `Racer_Draw`'s value; the function keeps that true
whatever the draw side does. In a session the digest also folds both visibility masks
(`gdx_mpo_vis_hash`, port/decomp_port.c), so a draw-side divergence fails on its tick. Applies
last; touches only those seven expressions and the macro next to mp-sim's declaration.

## decomp-3ds-mp-ekhold.patch (MP30: the Expansion Kit countdown hold is a lockstep event)

`Racer_Update` (racer.c, `EXPANSION_KIT`) holds `gRaceIntroTimer` at 40 while
`func_807424CC()` says this console's 64DD disk BGM load is still in flight. Load times differ
per console, so in a session the race would start on different ticks and desync. The test
becomes `gdx_mp_ek_bgm_hold(func_807424CC() != 0, sRaceFrameCount)` (port/3ds/gdx3ds_autodrive.c):
off a session it returns its first argument (stock); in one it holds until the first tick whose
exchanged lockstep pads all carry `GDX_MP_PAD_BGM_READY` (bit 6, no N64 button; each console sets
it from `gdx_mp_local_bgm_ready()`, added here, and the session strips it before the game reads
the pad; a removed player counts as ready), or until 180 held ticks counted from
`sRaceFrameCount`, whichever is first -- both decided from data every console holds identically.
`[net] mp_bgm_hold_until = N` (test lever) withholds one console's bit until race tick N.
Receipt: `[mp] ek hold: countdown released at race frame F after H held ticks (...)`. Applies
last; non-EK builds compile only `gdx_mp_local_bgm_ready` (always 1).

## decomp-3ds-mp-racers.patch (MP30: VS COM with 8, 16 or 30 machines)

The online / NEARBY START packet carries a machine count (`racers`: 0 = stock 4, else 4..30,
VS COM only; the host picks it on the MACHINES lobby row, `[net] mp_racers` sets the default).
`Race_Init` VS COM sets `gTotalRacers = gdx_mp_vs_total_racers(4)` (port/3ds/gdx3ds_autodrive.c:
the stock 4 off a session). `func_80089934` fills the CPU field past 4 with
`Racer_GdxFillCpuField` (same Rand2 character / Rand1 skin draws, re-drawn until the character
is unused, so 30 machines show 30 pilots). Every VS table (`gVsRacePlayerPoints`,
`gVsRacePlayerVictoryCount`, `sVsRacePositionPoints`, `sVsRaceRacerCountPositionPoints`, the
results-screen arrays) is sized for 4 racers, and the stock award indexed them by racer and
place up to `gTotalRacers` -- an out-of-bounds sim write with more. With more than 4 racers the
award and `Menus_DrawVsResultsScreen` rank the human players among themselves
(`gdx_vs_field()` / `gdx_vs_place()`, macros `GDX_VS_FIELD` / `GDX_VS_PLACE` in menus.c);
with 4 or fewer they are `gTotalRacers` and `position`, byte-identical to stock.
`Menus_DrawPlayerWinner`'s `sortedPlayersByPosition` grows to `TOTAL_RACER_COUNT`. Applies last.

## decomp-3ds-mp-dropcpu.patch (MP30: a dropped human becomes a CPU at its removal tick)

With `[net] mp_drop = cpu` (the default; in the sim-ABI word) a player removed from a session
keeps racing: from its agreed removal tick T `gdx_autodrive_racer` (port/3ds/gdx3ds_autodrive.c)
answers 1 for it (`gdx3ds_mp_drop_cpu_racer`, net/gdx3ds_mp.c and gdx3ds_mp_online.c, from the
removal tick every console holds), so `Racer_UpdateMovement` drives it with `Cpu_GenerateInputs`
(throttle and steering, as the other autodriven racers). Unlike the test bots it takes no share
of its (neutral) pad (`gdx_autodrive_mix` 0) and its energy is not pinned: the patch makes the
autodrive branch's `racer->energy = racer->maxEnergy` conditional on the new
`gdx_autodrive_pin_energy(id)`, 1 for the autodrive lever and the bots (stock behaviour), 0 for a
taken-over racer. `retire` (the old behaviour) and `coast` remain. Applies last.

## decomp-3ds-mp-rambot.patch (MP30 test lever: ram-mode bots)

`[net] mp_bot = 2` (synced as START `ramMask`, a subset of `botMask`) makes a lockstep test bot
aggressive: the autodrive branch of `Racer_UpdateMovement` keeps the CPU driver's full buttons
(boosts, spin and side attacks) when the new `gdx_autodrive_full_buttons(id)`
(port/3ds/gdx3ds_autodrive.c) says so, instead of masking to `BTN_A`; the bot's jittered pad
adds R, Z and Z+R; and `gdx_autodrive_pin_energy` is 0 for it, so machines are destroyed.
The 30-machine lockstep tests use it to drive the bump / attack / destruction paths. Off a
session, and for plain bots, the mask is applied as before. Applies last.

## decomp-3ds-mp-grid.patch (MP30: the starting grid of a VS field with more than 4 machines)

Two stock assumptions of a one-row (at most 4 machines) VS grid, both found by the 30-machine
emulator runs (racer 4 had a NaN position from the first tick; the NaN spread through the
collisions into every machine's energy: empty energy bars, mass retirements):

- cpu.c `func_i3_80115E74`: in multiplayer it renumbers racers 0..3 to positions 1..4 after
  handing racers 1..N-1 positions 1..N-1, which with more than 4 racers left racer 4 sharing
  position 4 with racer 3 and position N unused, so `Racer_SetupStartPositions` never placed
  racer 4. With `gTotalRacers > 4` it numbers every racer.
- racer.c `Racer_SetupStartPositions`: the step back between machines is 100 units in 1P modes
  but 0.1 in multiplayer (unused past the first row in stock VS); with `gTotalRacers > 4` it is
  the 1P / GP one, so rows 2..8 do not stack on row 1.

Stock (4 or fewer racers) is unchanged. Applies last.

## decomp-3ds-bui-title-demo-hold.patch (`[ui] bottom_menus`: no attract demo under an open menu flow)

`src/audio/{rom,disk}/external.c`, `PORT`. The title screen arms its attract demo from the title
BGM's position (and, under the det audio pin, by tick count through `gdx_det_title_demo_step`).
A bottom-screen menu flow opened on the title (MULTIPLAYER, the name keyboard ...) takes the
buttons from the game, so the title sees no input and the demo would start under the player.
The patch ORs `gdx_title_demo_held()` (port/gdx_determinism.c; set by port/3ds/gdx3ds_bui.c while
a flow is open on the title) into the existing `gdxDetTitleDemoPin` arm guard: held = the pin's
"never armed" behaviour, and `gdx_det_title_demo_step` does not arm by tick count either. Not
held (no flow open, and always with bottom_menus=0) the expression is the one before the patch,
so replays and det runs are unchanged.

## decomp-3ds-mp-stockpick.patch (multiplayer: each player picks with the stock top-screen menus)

`src/game/game.c`, `src/overlays/ovl_i4/machine.c`, `src/overlays/course_select/course_select.c`,
`PORT`. In a multiplayer room every player picks a machine and engine setting (the host also the
course) with the game's own 1P MACHINE SELECT / MACHINE SETTINGS / COURSE SELECT, run locally on
each console (port/3ds/game/gdx3ds_mp_pick_game.c). game.c passes each mode update's result
through `gdx_mp_pick_filter` (a final confirm is recorded as the player's pick and the confirmed
screen stays; MACHINE SELECT's B is ignored: a round always ends in a pick), and skips
`Save_UpdateCourseCharacterSave` while a pick runs.
machine.c: no `Save_UpdateCharacterSave` read (the engine slider starts at the lobby's value) and
no menu-BGM stop on the confirm (no race follows it). course_select.c: the X cup (a generated
course) is hidden as in time attack (`GDX_CS_NO_X_CUP`), and `gdx_mp_pick_course_refused` turns
the confirm of a course multiplayer cannot race (the EDIT cup, for now) into the error sound.
Every change is gated on `gdx_mp_pick_active()`, 0 outside a room's picker: single player, replays
and det runs take the unpatched paths.

## decomp-3ds-font-nullread.patch (a NULL string in a stock font draw: log it, draw nothing)

ovl_i2/font.c: `Font_GetStringWidth` and `Font_DrawString` return 0 / the unchanged display list for
a NULL string and log `[nullread] <fn> str=NULL caller=<pc> (#n)` (the first 8). The b5f620a New 3DS
host crash was this read (CourseSelect_NameDraw -> Font_GetStringWidth with gCurrentTrackName NULL
after the MP pickers restored it, fixed in gdx3ds_mp_pick_game.c). Hardware faults on a NULL read;
the emulator reads zeros, so the NEARBY gate fails on any `[nullread]` line instead. A valid string
takes the unpatched path, so det runs are unchanged.

## decomp-3ds-mp-vs-overlay-bounds.patch (MP30: the VS race-over overlays stop at 4 racers)

ovl_i3/menus.c, the VS branch of the race HUD: the per-racer loop (final lap, BOOSTER OK, the
winner / finish-time / max-speed / retire overlays) ran to gTotalRacers, which mp30 raises to 16 or
30, while every per-player array it feeds is [4] as in the stock game. Once every player was done
(D_800E5FBC), Menus_DrawPlayerWinner(i) clamped sPlayerWinnerLoserFinishGameoverTimer[i] to 600 for
i up to 15, and index 15 is sCourseMinimapTex in the EK link: the bottom-screen race map then read
from 0x258 and both consoles faulted at the finish (test-combined 90049af). The loop now stops at 4;
a field of 4 or less (every stock and single-player race) runs exactly as before. Applies last.

## decomp-3ds-o3ds60-mp-hud.patch (o3ds60: `[ui] mp_hud_placement`)

`src/overlays/ovl_i3/menus.c`, `Menus_Draw` only. Applies last. With `[ui] mp_hud_placement = 1`
(default on both models since mp-polish; `port/3ds/gdx3ds_bottomhud.c`) the HUD placement predicates
(`port/gdx_hud_placement.c`) answer for an MP fullview race's local view as they do for the 1P
race, instead of forcing ALL_TOP for `gNumPlayers > 1`; the RANK / SPEED / PORTRAITS / MINIMAP call
sites already follow them. This patch covers the one call site that did not: when the top HUD is
suppressed in a fullview build, `Hud_DrawHud` still runs (its fullview body calls every player's HUD
functions: lap announcements, interval bookkeeping, the reverse warning's SE) and only its display
list output is rewound -- the 1P path's `Hud_AdvanceLapAnnouncementsOnly(0)` covers player 0's
announcer alone. Outside fullview (`GDX_FV_K() < 0`) the branch is never taken, so every other
build and mode is unchanged. Render only: nothing the simulation reads is written.

## decomp-3ds-mp-polish.patch (mp-polish: per-console announcer, BGM stop, MP race camera, settings fill)

`PORT` + 3DS hooks, mostly into `port/3ds/game/gdx3ds_fullview_game.c`. Applies last.
Hardware report (New 3DS host + Old 3DS guest, d9deb37): "you got boost power plays from host when
second player crosses a lap, but doesnt play when host crosses a lap after. should play on their
own screen" and "the changes for the widescreen view may have shifted the camera to be tilted
'downward'".

- `src/overlays/ovl_i3/hud.c`: the lap-2 (`NA_SE_17`, "you got boost power") and final-lap
  (`NA_SE_18`) lines are guarded by global once-flags (`sSecondLapStarted` / `sFinalLapStarted`),
  so the first human to reach the lap fired them -- on every console, since every console runs
  every player's HUD logic. The four triggers (`Hud_UpdatePlayerHudInfo`,
  `Hud_AdvanceLapAnnouncementsOnly`) now also require `gdx_fv_announce_for(playerIndex)`: true for
  every player outside a fullview MP race (stock), only the local player inside one. The flags
  then latch on the local player's lap, so each console announces its own player's lap.
- `src/game/racer.c` (finish): stock VS stops the BGM when the first racer finishes; in an MP
  race the console stops it when its own player finishes (`gdx_fv_local_player()`), as 1P does.
- `src/game/camera.c` (`Camera_Update`, after `Camera_UpdateFromSettings`): `gdx_fv_cam_reframe`.
  The VS 2P camera settings (`kWidescreenCameraRaceSettings`, `D_800D544C`) frame a half-height
  strip: fov 100 vs 1P 75 and frustum centre -15 vs 1P 77, which pitches the view down (horizon
  near the top of the screen) once fullview shows that view full screen. In a 2P MP session
  (`[net] mp_fullview_cam`, default 1, part of the sim-ABI word; off-session `[debug]
  fullview_cam_local=1` with `fullview_player`) the camera's fov and frustum centre are mapped to
  the 1P values for the same camera setting; distance, pitch and the settings themselves are
  untouched. Every view of the session takes it, so both consoles' `machineLod` draw masks stay
  equal (the lockstep digest folds them); the simulation never reads machineLod in a session
  (decomp-3ds-mp-vis.patch). 1P and local VS without the lever: unchanged.
- `src/game/object.c` + `src/overlays/ovl_i2/transition.c`: the settings backdrop now fills the
  frame by default (see decomp-3ds-settings-backdrop-full.patch), past the overscan box the
  transitions cover with the borders on, so the exit fade / blackout left a grey rim (emulator,
  O3DS, `race-mutecity-start` tick 3224). `Object_Draw` records the game frame it filled;
  `Transition_Draw` that frame opens its scissor and takes the borders-removed (full-frame)
  fills. Every other frame: unchanged.

## decomp-3ds-mp-vs-menu.patch (mp-menus: the VS icon opens the bottom screen's MULTIPLAYER menu)

`src/overlays/ovl_i6/main_menu.c` `MainMenu_Update`, `PORT` builds only. On a 3DS (one pad) the
SELECT MODE cursor used to skip VS BATTLE (the US arm's rule, and the EK arm's since
decomp-3ds-ek-vs-onepad.patch), and multiplayer lived behind a MULTIPLAYER tile on the bottom
screen's Home. Now, while fewer than two controllers are connected and the bottom-screen menus
offer it (`gdx_mp_vs_menu_available()`, weak, defined by `port/3ds/gdx3ds_bui.c`: `[ui]
bottom_menus` on), the cursor may rest on VS and A / START there calls `gdx_mp_vs_menu_open()`:
the bottom screen opens MULTIPLAYER (NEARBY / ONLINE) and the top screen stays on the grid. The
rooms' races still run as stock VS (`GAMEMODE_VS_2P..4P`, unchanged). Two or more controllers
(`[debug] pads=N`, a running session) keep the stock VS player-count step, and without the hooks
(weak NULL) both snaps are the stock rule. Applies last.

## decomp-3ds-unlock-3ds-code.patch (a 3DS-native unlock-everything code at SELECT MODE)

`src/overlays/ovl_i6/main_menu.c` `MainMenu_UnlockEverythingUpdate`, `PORT` builds only. The
stock code (L, Z, R, C-Up, C-Down, C-Left, C-Right, START at SELECT MODE) cannot be entered on a
3DS: no 3DS button maps to N64 L (`port/3ds/os/gdx3ds_os_ctru.c`: 3DS L and X are Z) and an Old
3DS has no C-stick. The patch adds a second code, checked on press edges beside the stock one:
3DS L, R, D-Up, D-Down, D-Left, D-Right, START (N64 Z, R, D-pad, START). Its completion is the
stock completion's steps exactly (`gSettingEverythingUnlocked`, `Save_SaveSettingsProfiles`,
`NA_SE_46`, the background's flourish); the stock code still works. The D-pad moves the SELECT
MODE cursor while it is typed (harmless). `[unlock] 3ds code ...` lines under the unlock
diagnostic gate. User-facing: `docs/CHEATS.md`. Applies last.

## decomp-3ds-mp-fullbleed.patch (mp-fullbleed: the MP fullview view full bleed under `[display] widescreen`)

`PORT` + 3DS only, draw layout only. Applies last (after decomp-3ds-mp-polish.patch).
Hardware report (New 3DS, `[display] widescreen = 1`): the multiplayer race view kept the overscan
borders while the single-player race was full bleed.

A VS race published the SPLIT widescreen frame kind (`port/input_bridge.c`), so the one
full-screen fullview view never got `RemoveBorders`: it kept the 296x224 CRT-safe frame (bands
under `border_mode` 0, a stretched non-5:3 picture under 1-3), while the 1P race renders the whole
320x240 frame at 5:3. A fullview frame now publishes the 1P kind (`gdx_fv_wide_single_view`,
`port/3ds/game/gdx3ds_fullview_game.c`; `[net] mp_fullview_wide`, default 1, 0 = the split kind
as in 06c5264). `RemoveBorders` only opens the `SCISSOR_BOX_FULL_SCREEN` viewport, scissor and CPU
mirrors (in a VS race only the local view uses that box; every other view keeps its half box and
is dropped anyway) and the backend's border scale: nothing the simulation or the lockstep digests
read. This patch moves the local HUD to match:

- `src/overlays/ovl_i3/hud.c` (`Hud_GdxFullviewDrawHud`): Hud_DrawHud case 1's anchor scopes
  (TIME, timer and energy right; LAP left; interval and reverse centred) when
  `gdx_widescreen_ui_active()` and `GDX_FV_WIDE_1P(k)`.
- `src/overlays/ovl_i3/menus.c`: the minimap / rank / speed anchors take the 1P rules for the
  local view (`GDX_RACE_ANCHOR_NP`), and the fullview portrait takes the 1P portrait's
  ANCHOR_LEFT.

With `[display] widescreen = 0` (no anchors, no frame kinds) or `mp_fullview_wide = 0` every
emitted command is as before.

## decomp-3ds-snapshot.patch (mp-snapshot: game-state snapshot / restore, the synctest gate)

`PORT` + 3DS only. Applies last. Step 1 of rollback / run-ahead
(`artifacts/mp-netcode-research/rollback.md`): the decomp side of `port/3ds/gdx_snapshot.c`
(contract `port/gdx_snapshot.h`). With `[debug] snap_test = 0` and `snap_probe = 0` (the
defaults) every hook below is one int test returning 0, and the section attributes only reorder
objects inside `.bss`: the simulation is unchanged.

The link's script is the installed `3dsx.ld` plus two INCLUDE fragments
(`port/3ds/gdx3ds_snapshot_{data,bss}.ld`, generated by `port/3ds/gdx3ds_snapshot_ld.cmake`)
that gather every decomp TU's `.data` / `.bss` / COMMON (audio, framebuffers, RSP buffers, the
static display-list segment, save buffers and the leo font excluded by file) into one `.data`
block and one `.bss` block: the snapshot. This patch handles the objects whose FILE belongs in the
snapshot but which must not be restored, and the side effects a resimulated tick must not repeat:

- `include/gdx_snap_section.h` (new): `GDX_NOSNAP` (boot / course-load tables, watched by the gate:
  a race tick must not write them), `GDX_NOSNAP_DRAW` (course-load tables the draw pass rewrites
  every frame before reading them; not watched), `GDX_NOSNAP_SCHED` (scheduler objects; never
  rewound, not watched). Empty off the 3DS build.
- `src/sys/sys_main.c`: every thread stack, OSThread, OSMesgQueue and OSMesg buffer ->
  `GDX_NOSNAP_SCHED`. They describe the same parked state at every tick boundary; rewinding them
  under the scheduler would corrupt it.
- `src/sys/math.c` `gSinTable`, `src/sys/sys_gfx.c` `sGdxCourseTrackStage` (a ROM staging buffer),
  `src/overlays/ovl_i6/options_menu.c` `D_i6_8011FB18` (menu vertex buffers) -> `GDX_NOSNAP`
  (-35 KB).
- `src/game/game_context.c` `gSegmentChunks` -> `GDX_NOSNAP_DRAW` (-98 KB). Built at course load;
  during a race only Course_Draw's chunk cull writes it (`depth` / `drawState`, recomputed for
  every chunk each frame before Course_Draw / Course_GadgetsDraw read them). The first gate run
  flagged it as "NOSNAP changed" every window, which is how it got its own class.
- `src/overlays/ovl_i3/cpu.c` `func_i3_80116C4C`: registers the CPU-AI buffer
  (`D_i3_8013DBE8`, 0x400 bytes in the RDRAM arena, outside the linked image) as dynamic snapshot
  slot 0 at every race init.
- `src/audio/{rom,disk}/lib/thread.c` `AudioThread_QueueCmd`: dropped during a resimulated tick
  (every command it would queue was already queued by the real tick it repeats). The audio
  libraries stay OUT of the snapshot: the audio producer thread owns that state, and the simulation
  only writes it.
- `src/audio/{rom,disk}/external.c` `Audio_PlayerTriggerSEStart`, `Audio_TriggerSystemSE`,
  `Audio_Chakuchi`: the one-shot SE gate. Outside a resim it only records (tick, kind, args) while a
  probe window is open; inside one it suppresses the call and counts whether the real ticks
  already played it (de-dup) or not (new).

Misses the gate (`[debug] snap_test = 1`: save at a random tick, run N = 1..8 real ticks, restore,
resimulate them silently, compare the whole image byte for byte) found, and their fixes. The full
list with the port-side fixes is in `port/3ds/gdx_snapshot.c`'s header:

1. `gSegmentChunks` tripped the NOSNAP watch every window (Course_Draw rewrites `depth` /
   `drawState`): reclassified `GDX_NOSNAP_DRAW` (above).
2. Draw-side bytes (Racer `shadowMatrixInit`, course.c's chunk groups, display-list cursors, the
   texture registry) differed in the heavy stretches of race-gp-cup: the C5TICK hollow build
   (decomp-3ds-c5tick-dlb.patch's `GDX_DL_HOLLOW()`) follows a frameskip prediction that reads the
   governor's clock. Port fix: the prediction is a recorded per-tick input.
3. The texture registry `D_800E33E0` (lazily registered glyphs carve arena memory) differed: the
   port's arena bump cursor was outside the snapshot. Port fix: it is snapshot state.

The gameplay digest matched in every miss window: all three were draw-side or allocator state.

## decomp-3ds-stereo-sky-margin.patch (3D: the sky / venue floor / clouds reach past each eye's edge)

`src/overlays/ovl_i3/background.c` `Background_Update`, `PORT` builds. Applies last. New 3DS
report: with the 3D slider all the way up, a band of clear colour at the left and right edges
of the top screen where the background should be (one edge per eye). The frame-replay eye
skew (`port/3ds/gfx/STEREO.md`) shifts far geometry sideways by up to the far-plane parallax,
`[stereo] iod` x slider px per eye (12 px at full slider, up to 40 px with the 3D-depth
setting). The skybox, venue-floor and cloud quads are built only a few % wider than the screen
(`horizontalRange`, the PORT `wideFactor` = 1.02 x the hor+ width), so one edge of each eye
uncovered: the left eye's right edge, the right eye's left edge.

The patch multiplies `horizontalRange` by `1 + gdx3ds_stereo_edge_shift_ndc()` after
`verticalRange` is fixed (the sky gradient does not stretch). The floor and cloud textures are
world-anchored and the skybox texture is constant across x, so only the coverage grows. The
shift (`port/3ds/gfx/gdx3ds_stereo.cpp`) is the largest per-eye skew of this frame in NDC-x
(`sep` at the far plane, `k*dc` at the near one) and exactly 0 while the right-eye pass is off:
2D, the Old 3DS default and slider-down frames build byte-identical vertices. Same number of
quads and triangles in 3D: no draw is added, only the off-screen part of three quads grows (the
GPU clips it).

Not widened: the course chunk / racer cull (`gdx_get_ultrawide_cull_xscale()`). Tried with the
same shift added to the `widescreen_cull_pct` margin: +2.5 % track draws/frame at `iod = 12`,
+9 % at `iod = 40`, and not one pixel changed in 116 eye images of the DD-1 replay at either
setting -- the 1.15 margin already covers the eye shift for chunk centres. Reverted.

Emulator (`gdx-dev retro run --3d --depth-3d 100`, N3DS-EK, DD-1 replay `eksky-gp-dd1-tun`,
widescreen = 1): at `iod = 40` uncovered bands up to 70 px wide in 81 of the 100 race-eye images;
with the patch 8, the dark-scenery baseline the detector also flags at `iod = 12`. At the
default `iod = 12` the margin is nearly used up (13 px on the right in that scene for a 12 px
shift): the patch closes 1-5 px notches where the floor's far corner meets the right edge of the
left eye (5 images); on hardware the band reached ~8 px. 2D (no `--3d`, N3DS, race-pause):
`fhdiff` 373/373 frame hashes identical, all 57 top-screen SHOTs byte-identical.
