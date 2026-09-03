# M1 boot debug — black screens in Azahar (feat/3ds-m1)

Symptom: the full-game `G-Diffuser-3DS.3dsx` boots to permanent black on both screens in
Azahar. No crash, no svcBreak; the Azahar log showed only APT calls and then silence.
The DL-test harness `.3dsx` (same carve lib + citro3d backend) rendered fine, so the
toolchain/backend/emulator were healthy.

Method: evidence → hypothesis → test → result, one step at a time. Each run:
`/Applications/Azahar.app/Contents/MacOS/azahar <path>.3dsx`, ~30-45 s, then read
`~/Library/Application Support/Azahar/log/azahar_log.txt` (truncates per session).

## Step 0 — the "silent" boot was a log-filter artifact

- **Evidence:** `svcOutputDebugString("main entered", …)` as the first line of `main`
  produced nothing in the log.
- **Hypothesis:** the debug channel itself wasn't being logged.
- **Test:** checked `~/Library/Application Support/Azahar/config/qt-config.ini`:
  `log_filter=*:Info` (the earlier `*:Debug` setting had not stuck; the
  `log_filter\default=true` flag kept the default). Azahar logs
  `svcOutputDebugString` at **Debug** level under the `Debug.Emulated` class
  (`core/hle/kernel/svc.cpp:OutputDebugString`), so `*:Info` filters every trace out.
- **Result:** set `log_filter=*:Debug` (and `log_filter\default=false`). Immediately the
  full boot trace appeared: `main entered → … → bootproc returned; entering frame loop`.
  **The game had been reaching the frame loop all along.** The M1-core prediction
  (ResourceManager thread-pool starvation pre-main) is disproven: statics, heap layout,
  config load, window init, archive mounts, audio init and `bootproc` all complete in
  ~1.5 s emulated time.

`svcOutputDebugString` is hereby the confirmed boot-trace channel for Azahar, but only
with `log_filter=*:Debug` (or at least `Debug.Emulated:Debug`).

## Step 1 — runaway `bzero` over the leo segment BSS stubs (root cause #1)

- **Evidence:** right after `bootproc returned`, the log spins forever on
  `HW.Memory unmapped Write32 0x00000000 @ 0x009E1000, 0x009E1004, … at PC 0x001DA34C`.
  `addr2line` → `Main_ThreadEntry`, `decomp/src/sys/sys_main.c:235`:
  `bzero(SEGMENT_BSS_START(leo), SEGMENT_BSS_SIZE(leo));`
- **Analysis:** under PORT the overlay link markers are 1-byte stub arrays in
  `port/gen/LinkStubs.c`. The linker places them arbitrarily; in this ELF
  `leo_BSS_END = 0x009C4070` < `leo_BSS_START = 0x009D1160`, so
  `SEGMENT_BSS_SIZE(leo)` is negative → ~4 GB as `size_t`. The bzero zeroes the tail of
  the real `.bss` (0x9D1160–0x9E0210, ~61 KB of live statics) and then walks off the
  mapped image at 0x009E1000 (page-rounded image end), where Azahar turns every write
  into a logged no-op — an infinite one-thread spin, screens black, no fault.
- **Why only on 3DS:** the call site is `#ifndef EXPANSION_KIT`; desktop builds EK=ON,
  so this line had never executed under PORT before. Every other segment-BSS consumer
  (`Dma_LoadOverlay`, `DiskDrive_LoadOverlay*`, `Dma_ClearRomCopy`,
  `Segment_LoadOverlays2`, course_gadgets' `func_800742FC`) was already a PORT no-op or
  `#ifndef PORT`-guarded.
- **Fix:** `#ifndef PORT` guards on the two raw bzeros —
  `decomp/src/sys/sys_main.c:235` (leo) and `decomp/src/sys/rom/disk_drive.c`
  `func_8007515C` (ovl_i11, same inverted-stub layout: END 0x9D7468 < START 0x9DF504).
  Host loader zero-fills BSS already. Captured as
  `port/3ds/patches/decomp-port-segment-bzero.patch` (no submodule commits).
- **Result:** the leo-bzero spin is gone; boot proceeds… into root cause #2.

## Step 2 — phantom 64DD drive: NULL `gDriveRomHandle` deref (root cause #2)

- **Evidence:** next run spins on `unmapped Write32 0x00000002 @ 0x00000013 at
  PC 0x001D6DE8` → `LeoFault_CopyFontToRam`, `decomp/src/sys/rom/leo_fault.c:107`:
  `gDriveRomHandle->transferInfo.cmdType = LEO_CMD_TYPE_2;` with
  `gDriveRomHandle == NULL` (0x13 = offsetof cmdType).
- **Analysis:** the path is gated on `gLeoDriveConnectionState != 0`, set by
  `LeoDD_CheckPresence()` (again `#ifndef EXPANSION_KIT`, desktop-untested).
  It calls `osEPiReadIo(leoHandle, LEO_STATUS, &status)` and tests
  `status & LEO_STATUS_PRESENCE_MASK` — but the port shim was the arg-less stub
  `int osEPiReadIo(void) { return -1; }`, which never writes `status`. The
  uninitialized stack u32 happened to be 0 on 3DS → "drive present" → font DMA
  through a NULL 64DD handle.
- **Fix:** `port/shims.c`: real-signature shim that fills the out-param with
  `0xFFFFFFFF` (floating bus / no device; NULL-tolerant for EA90.c's smoke-test
  callers) so `LeoDD_CheckPresence` deterministically reports "no drive".

## Step 3 — frame-loop liveness (and two self-inflicted red herrings)

- **Test:** heartbeat (`frame N` via `svcOutputDebugString`) plus a per-stage tracer in
  the `main_3ds.cpp` frame loop (`hb:events / vi_tick / audio_notify / start_frame /
  dispatch / save_tick / end_frame / pacer`).
- **Result:** with both fixes the loop runs continuously — 904 frames in a ~68 s
  session at ~15-20 host fps, all stages cycling, zero unmapped accesses, SRAM path
  probed (`saves/fzerox.sav` missing → fresh save, correct for first boot).
- **Red herrings to avoid repeating:**
  1. Two intermediate runs *appeared* to stall (after frame 6, after frame 64). Cause:
     overlapping background `pkill -x azahar` watchdogs from *previous* runs killing the
     current one mid-flight, and (worse) several stale Azahar instances accumulating —
     at one point five `azahar` processes were alive concurrently, sharing one log file.
     Rule: `pkill -9 -x azahar` and verify `pgrep` is empty before every run.
  2. `pkill` (SIGTERM) does not reliably kill Azahar on macOS; use `-9`.

The stage tracer stays compile-gated in `main_3ds.cpp` (`GDX3DS_BOOT_TRACE=1`);
default build keeps the cheap heartbeat (frames 0-7, then every 64th).

## Status / conclusions

- **Root cause of the black screens:** the EK=OFF-only `bzero` over the leo segment
  stub markers (step 1) wiped ~61 KB of live `.bss` and then spun forever on unmapped
  writes before the first frame was ever presented. Secondary NULL-handle crash-spin
  (step 2) sat immediately behind it. Both fixed; both were in code the desktop (EK=ON)
  port never compiles, which is why M1 hit them first.
- **Current boot state:** boots to the frame loop and runs steadily (900+ frames
  verified headless). On-screen output not yet visually confirmed (headless runs);
  the present path is the same citro3d backend the DL-test harness proved renders.
- **Boot-trace channel (verified):** `svcOutputDebugString` → Azahar log class
  `Debug.Emulated` at **Debug** level. Requires `log_filter=*:Debug` AND
  `log_filter\default=false` in `~/Library/Application Support/Azahar/config/qt-config.ini`
  (Azahar rewrites the file on graceful exit; the `\default=true` flag silently reverts
  the filter to `*:Info`, which was the original "no trace at all" mystery).
- **Open items (not black-screen related):**
  - SRAM path is CWD-relative (`sdmc:/saves/fzerox.sav`); should live under
    `sdmc:/3ds/gdiffuser/saves/` (port/sram_buffer.cpp POSIX branch).
  - `gdx-autoinput.txt` / `gdx-audio-debug.txt` probes also hit the SD root.
  - Azahar logs `unimplemented SVC GetCurrentProcessorNumber` once (libctru
    `svcGetProcessorID` during audio-thread init) — harmless, returns to caller.

## Step 4 — first-pixels shift: the [gdl-bad] storm and the never-presented offscreen fb

Evidence (human-verified bottom-screen screenshots, plus static analysis; Azahar was
IOKit-wedged this whole shift so no fresh runtime log exists — see the environment
section below):

1. `[gdl-bad] race=0 raw=00838E88 target=0x8ce76b0 limit=1019439 first=00000000 …
   parent=0x6fb148 index=517 w0=DE000000` storms, raw ∈ {0x838E88, 0x87AA04,
   0x9100CC, 0x89807C, …}.
2. `[transition] capture … firstNonZeroPx=-1`, `[transition-cap] SETTIMG src=0x870NNNN`.
3. `[gfx_citro3d] TODO CopyFramebuffer(3<-1 …)`.
4. `[vifallback] presented VI framebuffer fb=0x5dc300 (320x240 rendersToFb=1)`.

### Decoding the gdl-bad line (arithmetic, not guesswork)

- `limit=1019439` = `(0x1000000 - raw) / 8` EXACTLY, and `target - raw` is constant
  across lines ⇒ target = `gdx_rdram + raw`: the resolver served the child G_DL from the
  **bare-physical-RDRAM-offset fallback** (TryResolveAddress, the
  `raw ∈ [GDX_RDRAM_GFXPOOL_OFFSET, GDX_RDRAM_SIZE)` branch) — 16 MB arena, zeros there,
  `first=00000000`, LooksLikeDisplayList fails, draw noop'd.
- Every observed `raw` is a REAL host pointer into the 3dsx image (nm: 0x838E88 =
  `D_A002000_23EC50+0x4E8`, 0x87AA04 = `D_400F868+0x74`, 0x9100CC = `D_3032878+0x13C`,
  0x89807C = `D_A002000_255100+0xE88`) — all generated ASSET PLACEHOLDER BSS arrays
  (AssetBindings.c rows exist for each; all four land INSIDE symbol bounds, which rules
  out coincidence). `parent=0x6fb148` is inside `D_8024DCE0` (the GfxPool — runtime
  game-built wide DLs, `transition.c`/`background.c`).
- `target != raw` proves ProcessList did NOT take the tagged-host-pointer fast path: the
  pointers were (correctly) re-routed to the resolver as asset placeholders
  (`IsAssetPlaceholderPointer` true ⇒ `LookupAssetSegment` MATCHED). Therefore
  `ResolveGeneratedAssetStub` matched the row too and its failure is downstream:
  **`EnsureAssetSegmentImage` returned 0** (for e.g. `big_blue_textures`, which is not
  mode-owned and offset-in-bounds). The staged SD has NO `baserom.us.rev0.z64`
  (`romUsable=false`), so blob loading via
  `GdxSegmentSourceRead → GDiffuser_LoadArchiveFileBytes → ResourceManager::
  LoadFileProcess → O2rArchive/zipshim` is the only source — and all 25
  `segment_blob/*` families ARE present in the staged `fzerox.o2r`/`gdiffuser.o2r`
  (verified by listing the archives), so **the o2r read path itself fails at runtime on
  3DS** (which stage — LoadFileProcess miss, zero TrueSize, malloc — is what the new
  diagnostics pin). Consistent cross-check: `gdx_boot_warm_asset_segments` pre-decodes
  the venue banks at boot; a warm cache would have made `D_A002000_23EC50` a cache hit,
  so the warm loop must have failed the same way (`[boot-warm] decoded 0/12` expected in
  the next run's log).

### Fixes landed this shift

- **svc log tap** (main_3ds.cpp): every `gdx_port_logf` line is now mirrored to
  `svcOutputDebugString`, so `[gdl-bad]/[segload-fail]/[archive-fail]/[boot-warm]/
  [transition]` reach the Azahar log (`log_filter=*:Debug`) instead of only the
  bottom-screen console.
- **Bounded failure diagnostics**: `[segload-fail]` (EnsureAssetSegmentImage, every
  0-return now names its stage) and `[archive-fail]` (GDiffuser_LoadArchiveFileBytes:
  bad-args / no-context / no-resource-manager / LoadFileProcess-null / null-buffer /
  zero-size). The next run's log identifies the broken stage directly.
- **ILP32 module-identity resolution** (n64_gfx_bridge.cpp,
  `ResolveIlp32ModuleIdentity`): on 32-bit hosts a 32-bit `raw` inside the main module
  image IS the pointer; resolve it verbatim AFTER the asset resolvers and BEFORE the
  bare-physical-RDRAM guess (both in TryResolveAddress and in G2ResolvePhysical). On
  3DS the whole image (0x100000..~0x9E1000) sits below GDX_RDRAM_SIZE, so the
  bare-physical guess otherwise swallows every unclaimed module pointer — the direct
  mechanism of the [gdl-bad] misresolution. No-op on 64-bit hosts; cannot collide with
  segment tokens (top byte ≥ 0x01 ⇒ ≥ 16 MB > __end__).
- **Present path — the second, independent black-screen killer**: nothing on 3DS ever
  set `Interpreter::mGameWindowViewport` (desktop sets it from Fast3dGui::DrawGame; the
  DL harness set it manually — why the harness rendered and the game did not).
  `ViewportMatchesRendererResolution()` was therefore false every frame ⇒
  `mRendersToFb=1` (the `[vifallback] rendersToFb=1` evidence) ⇒ all content rendered
  into the offscreen `mGameFb` texture that nothing composites on 3DS, while fb 0 was
  cleared black. Fixed in lus_glue Fast3dWindow::Init: viewport = {0,0,400,240}. In
  normal play (Widescreen CVar default 1, fixed-aspect only forced in the editors)
  `mRendersToFb` is now false and draws land on the screen target.
- **CopyFramebuffer** (gfx_citro3d.cpp): implemented for the hot same-size
  texture-backed↔texture-backed case (the frame-mirror composite `3<-1` from the
  evidence) via `C3D_FrameSplit` + GX texture copy. Screen↔texture and scaled copies
  still log-and-skip (the texture-fb rotated layout cannot be raw-copied to/from the
  240x400 screen buffer; see follow-ups).
- **ReadFramebufferToCPU** (gfx_citro3d.cpp): implemented — GX display transfer
  (tiled RGBA8 → linear RGB5A1, same bit layout as N64 rgba16), then CPU un-rotation
  (x'=y, y'=−x, matching SetViewport), nearest resample to the requested 320x240,
  coverage bit forced (GL/DX11 contract).
- **SRAM path**: 3DS branch in sram_buffer.cpp routes through `gdx3ds_fs_base_path()`
  → `sdmc:/3ds/gdiffuser/saves/fzerox.sav` (was CWD-relative → `sdmc:/saves/`).

### Follow-ups / risks

- The o2r read-path failure ([archive-fail] stage) still needs the runtime log to pin
  and fix — that is the content half of the gdl-bad storm; the resolution half is fixed.
  Memory pressure is a live suspect (no mallinfo probe yet; image ~10 MB + RDRAM 16 MB +
  audio blobs ~11 MB + LUS cache cap 24 MB vs the 64 MB O3DS app region).
  Host cross-check done this shift: miniz + the staged `fzerox.o2r` were exercised on
  macOS with the shim's exact call sequence (init_file → locate_file_v2 case-sensitive →
  extract_iter) — 3610 entries index, `segment_blob/big_blue_textures` /
  `segment_blob/setup_gfx` / `audio_blob/audio_table` all locate and stream, and the
  0x40-offset payload-size field matches the generated blob table exactly (0x5140 /
  0x3D370 / 0xA3F1D0). The archive data, keys and miniz logic are NOT the problem; the
  failure is 3DS-runtime-only (sdmc stdio, heap exhaustion, or ResourceManager state).
- Transition captures need mirror copies from fb 0 (screen) once rendersToFb=0; that is
  the logged-and-skipped screen→texture CopyFramebuffer case. Also stream A's
  texture-backed fbs pad to (NextPow2(gameW), NextPow2(gameH)) = 512x256 while the
  rotation runs game-x along fb-y (256 tall) — full-screen offscreen renders clip ~144px
  of game-x. Both belong to the transition/editor pass, not first-pixels.
- ReadFramebufferToCPU GX_BUFFER_DIM axis order vs citro3d's internal convention is
  build-verified only; if the first capture looks scrambled, swap the dims first.

## Environment gotcha — Azahar can wedge at startup after SIGKILL storms

After several `pkill -9` cycles, fresh Azahar launches sat forever with no window and a
0-byte log. `sample <pid>` showed the main thread stuck pre-logging in
`InputCommon::SDL::SDLState()` → IOKit `IOServiceOpen` (AppleSyntheticGameController)
— a wedged macOS HID service, nothing to do with the game build. Recovery: wait for
the service to clear, or log out/reboot. Prefer closing Azahar gracefully (SIGTERM
first, `-9` only as fallback) and never leave overlapping kill watchdogs armed.

Update (first-pixels shift): the wedge persisted for the whole shift — every launch
(including with `SDL_JOYSTICK_MFI=0 SDL_JOYSTICK_IOKIT=0 SDL_HIDAPI_DISABLED=1`) stuck
at the same IOServiceOpen, SIGKILLed processes accumulate in unkillable E/UN states,
and the log stayed 0 bytes. This machine needs a logout/reboot before the next
emulator run; all step-4 fixes are build-verified only.

## Repro / verification

```sh
export DEVKITPRO=/opt/devkitpro
cmake --build build-3ds -j8            # → build-3ds/port/3ds/G-Diffuser-3DS.3dsx
pkill -9 -x azahar; sleep 1
/Applications/Azahar.app/Contents/MacOS/azahar build-3ds/port/3ds/G-Diffuser-3DS.3dsx &
sleep 60; pkill -9 -x azahar
grep "OutputDebugString" ~/Library/Application\ Support/Azahar/log/azahar_log.txt | tail
# expect: main entered → … → bootproc returned → frame 0..7, 64, 128, … increasing
```


## Step 5 — archive lookup shift: std::filesystem::absolute() mangles devoptab paths (root cause #3)

Symptom (fresh run after reboot): boot + frame loop healthy but EVERY o2r key failed —
`[archive-fail] key=segment_blob/... stage=LoadFileProcess-null` for all
segment_blob/audio_blob/rsp_blob keys, despite the archive being byte-perfect on host.

Two stacked causes:

1. **Root cause — path mangling in LUS.** `ArchiveManager::GetArchiveListInPaths`
   pushes `std::filesystem::absolute(archivePath).string()`. Under std::filesystem's
   POSIX grammar a devkitPro/newlib device path (`sdmc:/3ds/gdiffuser/fzerox.o2r`) is
   RELATIVE — `sdmc:` is just an ordinary first component — so absolute() prepends the
   cwd (`/` under Azahar) and O2rArchive was constructed with
   `/sdmc:/3ds/gdiffuser/fzerox.o2r`. No devoptab matches a leading-slash device name;
   newlib fopen fails with errno 88 (ENOSYS). Instrumented zipshim log proof:
   `[zipshim] open FAIL path=/sdmc:/3ds/gdiffuser/fzerox.o2r flags=0x10 mzerr=file open
   failed errno=88 cwd=/`. Interestingly `is_regular_file("sdmc:/...")` succeeds (the
   raw string goes straight to stat → devoptab), so the path list was built — mangled.
   Fix: `port/3ds/patches/lus-device-path-archives.patch` — `ResolveArchivePath()` in
   ArchiveManager.cpp passes device-prefixed paths (`<name>:/...`) through untouched
   and keeps absolute() for everything else.

2. **Mask — zipshim ZIP_CREATE empty-handle degradation (now removed).** zip_open under
   ZIP_CREATE returned an empty readable handle when the file could not be opened, so
   `O2rArchive::Open` "succeeded" with 0 entries and the VFS index was silently empty —
   this masked cause 1 for a full shift. gdx3ds_zipshim.c now FAILS the open (NULL +
   ZIP_ER_NOENT) and svc-logs path/flags/miniz error/errno/cwd (bounded, 16 lines);
   host smoke test updated to expect the loud failure.

After both fixes (same build, fresh Azahar session):
`[zipshim] open ok path=sdmc:/3ds/gdiffuser/gdiffuser.o2r entries=4`,
`[zipshim] open ok path=sdmc:/3ds/gdiffuser/fzerox.o2r entries=3610`,
archive-fail count 0 (was: every key), segload-fail count 0, 18 `[seg-src] blob loaded`
lines (audio_bank/audio_seq/audio_table + segment blobs), and
`[boot-warm] decoded 12/12 asset segments in 259.8ms (segments table restored)`.
Frame loop ran past frame 1600 with zero fail/FATAL/WARNING lines. No `[transition]`
capture line this run — those only fire on a game-mode transition, which needs input;
firstNonZeroPx remains unmeasured, not regressed.

## Shift N+1 — top screen black with everything upstream healthy (M1-PRESENT)

Symptom: game boots, runs at speed, GFX tasks execute every frame, RDRAM framebuffer
has content — but the top screen stays black; the bottom-screen console reacts to
input. Two independent root causes were found and fixed; either alone keeps the
screen black.

### Method: prove present, then bisect content

New bounded telemetry (kept, ~2 svc lines/sec):
- `[c3d] frame=N draws=(scr/tex) tris fbBinds bindMiss texUp texUpFail` — every 64th
  `EndFrame` in gfx_citro3d.cpp;
- `[present] frame=N topFb=… nz=X/288000 firstOff center` — main_3ds.cpp scans the
  top screen's CPU-readable back framebuffer (gfxInitDefault → linear heap) after
  `EndFrame`, the definitive "did pixels reach scanout" oracle;
- `GFX_C3D_LOG` now mirrors to `svcOutputDebugString` (was stderr-only, invisible in
  headless Azahar).

Temporary probes (added, used, removed): magenta clear pulse through the normal
`ClearFramebuffer` path; a cumulative per-draw override ladder (alpha test off →
depth off → blend off → combiner forced constant white); a REPLACE-PRIMARY_COLOR vs
REPLACE-TEXTURE0 A/B; a per-upload texel-content checksum.

### Root cause 1 — double buffer swap (present path)

`gdx3ds_os_window_swap()` (port/3ds/os/gdx3ds_os_ctru.c) called `gfxSwapBuffers()`
every frame on top of citro3d's own presentation: `C3D_FrameEnd` queues a display
transfer of the screen render target into the top screen's BACK buffer and citro3d's
render queue calls `gfxScreenSwapBuffers` itself when the transfer completes. The
extra CPU-side swap double-swapped every frame, so the LCD stably scanned out the
buffer citro3d never transferred into — permanently black regardless of content.
The working harness (port/3ds/harness/dl_tests_main.cpp) never swaps manually; its
window backend's `SwapBuffersEnd` is a no-op. Fix: the swap contract now only does
`gfxFlushBuffers()` (cache flush for the bottom console) + `gspWaitForVBlank()` +
`aptMainLoop()`. Proof: magenta pulse became visible in the scanout scan
(`nz=138752`, `center=ff00ff`) immediately after the fix.

### Root cause 2 — LUS SETTIMG guard drops every texture on 3DS (content path)

After fix 1 the scan was still all-zero outside the pulse. The bisect ladder proved
alpha/depth/blend innocent and geometry perfect (combiner forced white → 71% screen
coverage); the A/B probe showed PRIMARY_COLOR fine, TEXTURE0 black; the upload probe
then showed ALL 87 texture uploads were the VI-fallback seed quad (id 0) — the
interpreter never imported a single game texture (bindMiss=0 because the stale seed
texture id 0 stayed bound and is a real, black, texture).

Root cause: `gfx_set_timg_handler_rdp` (libultraship src/fast/interpreter.cpp) guards
against unresolved N64 segmented addresses with `if (i <= 0x0FFFFFFF) return false;`
on non-Windows. On the 3DS the process image maps from 0x00100000 and the malloc
heap sits at 0x08000000 — every valid host texture pointer is inside that range, so
SETTIMG silently dropped ALL textures. Vertices have no such guard, which is why
geometry worked (and why every upstream milestone — DLs, combiners, segments —
passed while textures were black). Fix:
`port/3ds/patches/lus-3ds-settimg-low-address.patch` — a `__3DS__` branch that only
rejects pointers below the process image base (0x00100000).

### Verification (Azahar, log_filter *:Debug)

- texUp: 87 (seed quad only) → 3360 by frame 193 — game textures now upload;
  texUpFail=0, bindMiss=0.
- `[present]` every sampled frame from 129 on: `nz≈122k/288000` nonzero scanout
  bytes, varying frame to frame (121901/122725/122814 — live content, not a stuck
  buffer), firstOff=34593 (content starts after the 4:3 dead band).
- draws=94-191/frame, tris up to 718 — title/attract sequence rendering.

Human eyes still owed the final confirmation, but the scanout buffer now provably
carries the title screen.

## Shift N+1 addendum — 8fps + race-entry freeze (same session, after menus proved out)

Human confirmed menus render. Two follow-ups diagnosed from the same log evidence:

### Race-entry freeze = course DMA rerouted (fixed)

At race entry (attract demo hits it too, ~46s in) the Azahar log exploded with
~18,500/s `HW.Memory unmapped Read32 @ 0x00000008 / Write32 @ 0x00000028 at PC
0x0011655C` — addr2line: `Course_SegmentLengthsInit` (decomp src/game/course.c),
walking an unlinked course-segment list through NULL forever. Cause: 
`Dma_PortRamPointer` classifies any address < 16MB as a raw RDRAM offset; on ILP32
that swallows real host pointers (gCourseCtx sits at 0x003899CC in the 3DS image),
so the course-data DMA wrote into the RDRAM shadow and the segment list never got
built — dma.c's own comment records the identical failure signature on desktop
under ASLR. Fix: `decomp-3ds-dma-low-address.patch` — registered host ranges win
for addresses >= 0x00100000 (the process image base), matching 64-bit semantics
where real pointers always resolve via the registry. Verified: unmapped-access
storm gone (152 lines vs 900k), race loads and draws (tris=3752/frame vs the
menu's ~200-700).

### 8fps

Three contributors identified:
1. The unmapped-access storm above — each access takes Azahar's slow path AND
   writes a log line (the pinned log_filter *:Debug makes this worse). Fixed with
   the freeze.
2. Per-draw `GSPGPU_FlushDataCache` — an svc kernel round trip per DrawTriangles,
   ~190/frame on menus. Fixed: FlushPendingVbo() flushes the frame's appended VBO
   range in ONE svc before submission (EndFrame / both FrameSplit sites).
3. Double vblank wait per frame: gdx3ds_os_window_swap's gspWaitForVBlank on top
   of C3D_FrameBegin(C3D_FRAME_SYNCDRAW)'s own sync quantized every frame down
   (~18.5fps ceiling measured on 1-draw frames). Fixed: swap no longer waits;
   SYNCDRAW is the sole pacer, like the harness.

Note for future perf reads: Azahar's pinned `log_filter *:Debug` itself costs
significant host time once any per-frame svc/log traffic exists; real-hardware
fps will differ (likely better for CPU-bound frames).

### NEXT FRONTIER — RSP LLE crash ~60s into the race path

With the course fix in, the run crashes at t≈90s (race running, audio/task
active): 151 sequential `unmapped Read16/32 @ 0x0868AExx` at PC 0x00278290 —
addr2line: `macf_v_msp` / VMACF scalar path, port/rsp/cxd4/vu/multiply.c:409 —
then a jump to PC 0x00000000. The cxd4 RSP LLE walks reads off the end of a heap
block (0x0868AExx is just past committed heap) with an ascending two-stream
pattern, then the interpreter lands on a NULL function pointer. Untriaged;
suspect ILP32/pointer assumptions inside cxd4 or a bad DMEM/task pointer handed
to it at race SFX/task start. This is the first thing to debug next shift.

### RESOLVED — the "NEXT FRONTIER" RSP crash was the LLE audio path running at all

Root cause of both the race crash AND a large share of the residual slowness: on
3DS the ImGui menu TU that registers `gEnhancements.Audio.LLE` is not built, so
`CVarGetInteger("gEnhancements.Audio.LLE", 1)` returned its DEFAULT of 1 and
EVERY audio task since boot ran the vendored cxd4 RSP LLE interpreter (a full
RSP interpretation per audio task, per frame — pure CPU burn on menus too). The
ILP32 VMACF heap overrun then fired ~90s in when race SFX tasks hit it.

Fix (gate in port/gdx_audio_lle.c + INI doc in gdx3ds_config.h): 3DS resolves
the audio path ONCE from the stream B INI (`[audio] lle`, default 0=HLE) and
logs it; `[audio] lle=1` re-enables the bridge for on-device benchmarking only.
The cxd4 ILP32 fault itself remains unfixed but unreachable on the default
path; it only matters if someone turns the test flag on.

Soak evidence (Azahar): `[audio-3ds] path=HLE (3DS default...)`, HLE task #1 /
#1000 markers, 1400+ frames, unmapped-access count 0, no crash. Boot segment
now 26fps; title steady ~20fps (Azahar vblank-quantized at 60/3 — remaining
cost is emulated game CPU + the pinned *:Debug log filter, not the gfx path).

Remaining known perf lead for race scenes: texture re-upload thrash — texUp
rose ~34/frame during attract/race segments (CPU Morton swizzle per upload).
Candidate next optimization: cache-key or dirty-check uploads in the
interpreter/backend seam before pointing at the swizzle itself.

## Shift M1-RACE-FREEZE — the race "freeze" was never a freeze: uncaught std::bad_alloc, silent guest exit

Symptom (human-verified): race loads and renders, then a few seconds in the Azahar log
stops MID-LINE (`[  43.254119] Debug.Emulat` — truncated), the process stays alive but
writes nothing for 20+ minutes, and the window shows a frozen race frame. Suspected:
svc log flood, audio/scheduler deadlock, or a race-loop hang.

### What it actually was

**Uncaught `std::bad_alloc` → `std::terminate` → `abort` → clean libctru app teardown.**
The guest 3dsx EXITED; Azahar kept displaying the last presented frame of a dead
emulation session, which looks exactly like a freeze. Confirmed twice headlessly in
attract mode (the demo race reproduces it):

1. The "mid-line truncation" was a red herring: Azahar's file log backend buffers
   through stdio and only flushes lazily. The tail of the log — including the entire
   death sequence — was sitting in an unflushed FILE buffer inside the still-alive
   process. Recovery trick (evidence, run 1): `lldb -p <pid> -o "expr (int)fflush(0)"`
   flushed 3.2 KB more log, revealing:
   `Service.APT PrepareToCloseApplication → CloseApplication → LinearFree 0x30000000
   (32 MB) → HeapFree 0x08000000 size=0x5319000 (87.2 MB) → Kernel: Cleaning up
   process 11 → Core Shutdown OK` — a clean, guest-initiated exit at t≈39 s, WITHOUT
   main_3ds.cpp's "exiting" logStep, i.e. NOT the frame loop's normal return: something
   called exit()/abort() or threw.
2. `sample` of the frozen Azahar showed every thread idle (Qt event loop, logger
   waiting on its condvar, no emu thread at all) and /tmp stderr showed
   `Destroyed VkDevice / VkInstance` — the emulation session was torn down, not wedged.
3. With fatal-exit tracers installed (run 2):
   `[fatal] std::terminate: uncaught St9bad_alloc what=std::bad_alloc` followed by
   `svcBreak: Break reason: PANIC`, 0.4 s after a fully healthy watchdog beat
   (race frames rendering, audio ticking, spec-wait idle). Same guest time (~40-44 s)
   as the human's freeze.

### Why the heap dies at race time

Watchdog heap telemetry (menus/title steady state): `heapUsed=44.7 MB` with only
~190 KB free in the arena. The run-2 teardown freed a 0x5319000 = **87.2 MB** heap
region — i.e. the attract demo race grew the malloc arena by ~42 MB before dying at
what is evidently the app-region ceiling (heap 87 MB + linear 32 MB + image/stack ≈
the emulated app region). The dominant race-time growth is course content:
`gLoadedAssetSegments` (port/n64_gfx_bridge.cpp) caches every decoded venue/course
segment image and by DESIGN never evicts (resolved pointers into `loaded.bytes` are
handed out and cached on that guarantee — see the comments at lines ~2320/5212/7913).
Each additional venue visited (menus, attract cycling, race entry) is megabytes of
permanent growth on top of the fixed ~45 MB baseline (image + 16 MB RDRAM + ~11 MB
audio blobs + LUS caches). First race after enough venues → arena growth request hits
the ceiling → `new` throws → nothing catches it.

### Fixes and instrumentation landed (all on feat/3ds-m1)

- **Fatal-exit tracers (port/3ds/main_3ds.cpp):** `__assert_func` and `abort()`
  overrides, a `std::set_terminate` handler (names the exception type + what()), an
  `atexit` tracer, and replaceable global `operator new` that on failure logs
  `size / heapUsed / heapFree / arena / return-address` (addr2line the ra against
  build-3ds/port/3ds/G-Diffuser-3DS.elf) before throwing. Every formerly-silent death
  now produces a named `[fatal]` line plus a loud `svcBreak` in the Azahar log.
- **Watchdog thread (port/3ds/main_3ds.cpp):** priority 0x18 (preempts the 0x30 main
  and audio threads even mid-spin), one svc line per 5 s:
  `[watchdog] beat frame(+delta) stage fiber aud=enter/exit astage spec hle op idx
  heapUsed heapFree`. Stage numbers map the frame-loop phases (1 events … 8 pacer);
  `fiber` is the scheduler's current N64 thread id; `aud`/`astage`/`op`/`idx` localize
  an audio-thread wedge down to the HLE ABI command. Verified live across 5 runs.
- **[vtx-census] probe removed** (port/n64_gfx_bridge.cpp). It was capped at 48 lines
  and provably NOT the killer (storm ended 6 s before death), but it was leftover
  diagnostic noise firing exactly at course load.
- **A_CLEARBUFF clamp (port/n64_audio_hle.c):** `size` is a raw 32-bit word off the
  command list; a corrupt list could spin the audio thread for minutes inside
  DmemSetU8 while holding sAudioCtxMutex. Clamped to GDX_DMEM_SIZE (wraps anyway).
- **decomp-port-audio-specwait-yield.patch (new submodule patch):** `Audio_SetSpec`'s
  `do {} while (!AudioThread_ResetComplete())` is a yield-free spin on the game fiber;
  under PORT the reset only advances on the dedicated audio thread, and the 3DS kernel
  (and Azahar) never preempts a running thread for an equal-priority one — a real spec
  change would starve the audio thread forever. Latent (no current caller passes a
  differing specId — `Audio_AllSoundStop` always passes 0), but it is a guaranteed
  hard-freeze the day a spec change ships; patched in both audio/rom and audio/disk
  variants via `gdx_audio_specwait_yield()` (port/n64_sched.c, 1 ms sleep + counter
  surfaced in the watchdog line as `spec=`).
- **Audio/scheduler lock-graph audit (suspect 2), negative result:** cross-thread
  osSendMesg publishes queue DATA immediately under gdx_mq_lock (only the WAKE is
  deferred to the host loop), the audio thread's produce tick holds sAudioCtxMutex for
  the whole tick but never takes gdx_mq_lock while parked, and the foreign-thread
  yield guard in __osEnqueueAndYield never fired (no `[sched] WARNING` lines in any
  run). No lock-order or fiber-affinity violation found on the HLE path.

### Repro / environment notes for the next shift

- Attract mode DOES reproduce the death (~40 s guest time, demo race) — no input
  needed. BUT: after any SIGKILL of an active session, the next Azahar launch blocks
  on a startup QDialog (0-byte log, `QDialog::exec` on the main thread). Dismiss it
  with an AppleScript BUTTON CLICK (`click button 1 of window 1`), NOT a Return
  keystroke — the keystroke leaks into the game as START and parks it on a menu
  screen (draws=97/tris=194) where attract never runs (runs 3-5 wasted on this).
- Azahar's log tail lies: on any abnormal stop assume 1-4 KB of unflushed evidence
  and either close Azahar gracefully or `lldb -p <pid> -o "expr (int)fflush(0)"`.

### Next steps (the actual fix is a memory-budget workstream)

1. Human race-test with this build: the race will either complete or die loudly with
   `[fatal] operator new failed size=… ra=…` — addr2line that ra to name the exact
   allocation site. Either outcome is progress.
2. The structural fix: an eviction story for `gLoadedAssetSegments` (needs epoch
   invalidation of the cached resolutions that currently rely on never-evict), plus a
   review of the 24 MB LUS resource-cache cap and the 32 MB linear heap split against
   the app-region budget. Boot already sits at 45/87 MB; every venue is ~MBs of
   permanent growth.

## Shift M1-MEMORY — the race bad_alloc was a per-frame reserve blow-up, not a leaking cache

Mission: fix the race-killing OOM (uncaught std::bad_alloc at the ~87 MB heap ceiling,
reproduced twice in the previous shift). The handed-down suspect was `gLoadedAssetSegments`
(never-evicting decoded segment images). That attribution was WRONG, and the proof changed
the fix entirely.

### Step 0 — static bound kills the suspect before any run

Summing every `(segment, rom_base)` family's `image_size` in `port/gen/AssetBindings.c`
bounds `gLoadedAssetSegments` at **1.6 MB total across all 20 families** (the largest single
image is setup_gfx at 245 KB; the 11 venue banks are 48 KB each decoded). A container that
cannot exceed 1.6 MB cannot be a 42 MB grower. No eviction machinery was built — it would
have been complexity spent on the wrong object (and the cached-resolution comments at
n64_gfx_bridge.cpp ~2320/5212 can keep relying on never-evict).

### Step 1 — instrument, then let the failure name its own site

New always-on telemetry (all svc-mirrored, bounded):
- `[mem-census]` (main_3ds.cpp frame loop, every 256 frames): mallinfo used/free/arena,
  `linearSpaceFree()`, per-container bytes from the new `gdx_gfx_mem_census()`
  (loaded segments / persistent texture copies / wide-conversion cache / setup-gfx / host
  ranges), LUS resource-cache accounted bytes, cumulative >=256 KB allocation counters.
- `[big-alloc]` (operator new tracer): every C++ allocation >= 1 MB logs size + return
  address, 96-line cap.

Repro note: the attract demo did not trigger in the first session (game sat on a static
draws=97/tris=194 screen indefinitely — cause unknown, second session attracted normally).
The deterministic driver is the port's own `gdx-autoinput.txt` (tick mode) staged at the
Azahar virtual SD root (`~/Library/Application Support/Azahar/sdmc/`): `ticks` + `400 START`
+ A every 300 ticks navigated title -> menus -> GP race headlessly.

### Step 2 — the killer, named by return address

Seconds into race content the log burst with ~30 lines of
`[big-alloc] size=1341432..1730096 ra=0x1e6e04` followed by
`[fatal] operator new failed size=1378328 heapUsed=86105128 ... ra=0x1e6e04`.
addr2line: `std::vector<Fast::F3DGfx>::reserve` inlined into
**`N64DisplayListAdapter::EnqueueList` (port/n64_gfx_bridge.cpp)**.

Mechanism: EnqueueList must reserve a true upper bound up front — `commands.data()` is
handed out to parents BEFORE ProcessList fills the vector, so reallocation would dangle
those pointers. The bound used was `EffectiveLimit` = KnownCommandLimit = "bytes to the end
of the containing host range / stride". For race-frame GfxPool DLs that is the remaining
pool span: **~1.3-1.7 MB of F3DGfx per list, ~30 lists enqueued in ONE gdx_gfx_run ≈ 45 MB
of transient reserve in a single frame.** The adapter is frame-local, so this was never a
leak a heap trend could show: menus/title sat flat at ~39 MB and the very first heavy race
frame jumped 41 -> 86 MB and died at the ceiling. Desktop never noticed because 64-bit
malloc overcommits untouched reserve pages; the 3DS heap is fully committed physical memory.
`ConvertList` (n64_gfx_convert.cpp) had the same worst-case `reserve(max_commands + 1)`.

### Fixes landed (commits 9da51e1, 35eeedf, e6a1741)

1. **`TerminatorBoundedLimit`** (EnqueueList): pre-scan the source to its first G_ENDDL
   (F3DEX2 0xDF / F3D 0xB8) with the exact ReadCommand access pattern ProcessList uses, and
   reserve that + 1 instead of the range-span worst case. Sources with no terminator inside
   the limit keep today's bound; limits <= 4096 skip the scan. ProcessList pushes at most
   one output per input plus one synthesized terminator, so the bound stays a hard
   no-reallocation contract. Width-neutral: host converter unit tests pass unchanged.
2. **ConvertList seed reserve** capped at `min(max_commands, 1024) + 1` — nothing observes
   `out.data()` mid-build, so geometric growth is safe there. (Title-screen wide-cache
   footprint fell 2.23 MB -> 98 KB from this alone.)
3. **128 KB fiber stacks on 3DS** (n64_sched.c): the 3DS fiber backend commits the full
   stack up front, so the 1 MB default cost ~7 MB for stacks the N64 sized in KB. Saves
   ~6 MB of fixed baseline (title 44.7 -> ~38.5 MB).
4. Census + tracer kept always-on (~1 svc line / 10 s).

### Numbers (Azahar, log_filter *:Debug)

| State | before | after |
|---|---|---|
| Title steady | 44.7 MB (heapFree ~190 KB) | 37.2-39.2 MB |
| Attract demo race | 41 -> 86.1 MB in one burst, bad_alloc, guest exit | tris=3754 demo renders at **38.0 MB** |
| Scripted GP race (menus -> race) | died at race entry | race=1 runs, heap ~44-47 MB |
| Wide-conversion cache (title) | 2.23 MB | 82-148 KB |

Soak (attract rotation, no input, fixed build): 17 minutes / 3300+ frames through the
title demo race and the attract showcase screens — no bad_alloc, no [fatal], no >=1 MB
allocations after boot. **Peak heapUsed 47.3 MB** against the ~87 MB heap ceiling
(>39 MB headroom), decelerating crawl (+0.33 MB per 256-frame census at the end).

### Open follow-ups

- **Slow crawl during race content:** heapUsed rose ~0.5 MB per census (~2 MB/1000 frames)
  during the scripted GP race with all census-accounted containers flat and no >=256 KB
  allocations — i.e. sub-256 KB C/C++ allocations somewhere off the census. Not the acute
  killer; needs a malloc-wrap histogram if it matters for long GP sessions.
- **`[gdl-bad] race=1` budget (400 lines) fully consumed in GP-race context** — module-image
  pointers (raw 0x83D190, 0x892BD4, ...) whose targets decode as ARM code, arriving as G_DL
  children of GfxPool parents, dropped as non-DLs. Fires identically in the attract demo
  race (400-line budget consumed there too), which still renders tris=3754 — so the class
  co-exists with a fully-drawn race and is NOT introduced by the reserve fix. But scripted
  GP-race scenes showed only ~440 tris; whether that is this class dropping track sub-DLs
  or just the pre-race/countdown camera needs a shift with eyes on screen.
- The first-session "attract never starts on a draws=97 screen" anomaly is unexplained.

## V-VISUALS — in-race visual bug hunt (feat/3ds-vfix)

Mission: hunt the known-class visual bugs behind the human's "in-race visuals are
messed up" report — TEXEL1 adjacent-tile materials, screen↔texture CopyFramebuffer,
combiner gaps, fog, decals. Method: extend the DL harness into a self-verifying rig,
let pixel evidence (not eyeballs) name the bugs.

### New verification rig (the big win of this shift)

`gdx3ds-dl-tests` now auto-advances its scenes (~4 s each), dumps each one as a
320x240 BMP to `sdmc:/gdx-harness/sceneNN.bmp` (via the backend's
ReadFramebufferToCPU), and exits by itself — a headless Azahar run needs no input
injection. `port/3ds/harness/check_scene_bmps.py` asserts EXPECTED.md's PASS
conditions per scene as pixel checks. Three scenes added: **TEXEL1** (the
interpreter.cpp:3182-3188 pattern: ONE 32x64 RGBA16 TMEM load, render tile 0 = rows
0-31, render tile 1 = a 32x24 window at TMEM word 256, plus a magenta out-of-window
sentinel), **MACHINE** (census #16, the 271-site 3-stage 2-cycle machine material
incl. the constant-spill prefix stage), **FOG** (depth-tilted quad, G_FOG +
G_RM_FOG_SHADE_A vs the PICA native fog LUT).

**Final result: 36/36 pixel checks pass across all 9 scenes, zero `unmapped
combiner` lines, bindMiss=0.** Run 1 found three real bugs first:

### Bug 1 (fixed): fog LUT depth direction inverted — the prime "race looks messed up" suspect

The FOG scene rendered with the gradient exactly inverted: NEAR geometry fully
fogged, FAR geometry clear. Root cause: `UpdateFogState` fit its per-draw line
against `d = 1 - z/w` (the depth-buffer value under our reversed DepthMap(-1, 0)),
but the PICA fog unit demonstrably indexes its LUT with **z/w directly (0 = near)**.
Race tracks fog heavily (census §2d), and inverted fog paints the nearest track
geometry in fog colour while distant geometry pops out clear — matching a generic
"messed up" description. Fit coordinate changed to `z/w`; harness FOG scene now
passes all four gradient checks (pure red near, monotonic ramp, ~full fog at far).

### Bug 2 (fixed): ReadFramebufferToCPU mirrored every capture

Every run-1 BMP decoded as a horizontally mirrored image offset by the 80px dead
band; cross-referencing STRIP/TEXEL1/MACHINE/FOG showed the *renderer* was correct
and the readback's un-rotation was wrong. Empirical layout (now documented in the
backend): the effective render viewport spans fb-y `[vp.x, vp.x + vp.width)` — game-x
increases along fb-y FROM 0, game-y along fb-x from 0, dead band = fb-y tail. The
readback assumed end-anchored/negated fb-y. This is the same code path as the
in-game screen-transition capture (`gdx_read_current_framebuffer` → transition.c's
RGBA16 strips), so menu↔race transitions would have redrawn a mirrored frame.

### Bug 3 (fixed before it shipped): CopyFramebuffer + texture-fb orientation

Screen↔texture CopyFramebuffer was still log-and-skip (STATUS.md TODO): on 3DS
`mRendersToFb` is false, so BOTH frame-mirror directions (GdxUpdateFrameMirror's
screen→mirror every task frame, the vifallback hold tick's mirror→screen) were
no-ops — the mirror held garbage and autotest/interp-shot dumps were dead. Now: all
surfaces share the rotated tiled-RGBA8 layout, so every copy is a GX texture copy;
differing fb-x strides (screen 240 vs pow2 texture) bridge via the copy engine's
per-line gap (16-byte units), differing fb-y extents via row count, both anchored at
fb-y 0 (run-1 evidence corrected the initial end-anchoring). Prerequisite fix:
texture-backed fbs now allocate PORTRAIT (fb-x = pow2(game height), fb-y =
pow2(game width)); the old landscape allocation gave a 320-wide game viewport a
256px fb-y axis — 64px of game-x clipped off every tex-fb render and readback.

### Verified-good (harness pixel evidence)

- **TEXEL1 adjacent-tile** (target 1): unit-1 texture object binds per TMEM load,
  tile+1 selection picks the second view, the 24-row non-pow2 tile-1 window imports
  and rescales correctly (no padding band, no magenta sentinel bleed), and
  `(T0-T1)*SHADE+T1` blends per-vertex. The shift-2 plumbing was CORRECT — in-race
  track/vehicle "messed up" surfaces are NOT a TEXEL1-plumbing class.
- **MACHINE #16** 3-stage chain with constant spill: correct colours (ENV→PRIM lerp
  by texel; red texel rows → magenta as the per-channel math demands) and the
  cycle-2 shade modulate darkens correctly.
- **DECAL** still passes after all merges (target 5).
- Scenes 1-6 (STRIP/ROTATE/TEXTURE/SCISSOR/DECAL/COMBINE): shift-2 fixes hold.

### Operational notes

- The emulator-lock steal path was exercised: a displaced agent's script clicked/
  launched into MY session mid-run (second Azahar truncates the shared log; stray
  window events leak into the guest as START). Harness hardening: a stray START no
  longer disables auto-advance. When sharing Azahar, treat any log truncation +
  spontaneous scene skips as cross-agent interference.
- lldb `fflush(0)` trick confirmed again as the way to read the log tail of a live
  session.

### Scripted race soak (game .3dsx, tick-mode autoinput, fixed build)

Title → menus → race via the gdx-autoinput tick driver, ~10k+ frames observed across
the session (race content at draws=440 / tris=4784, later scenes 239-242 draws):

- **Combiner coverage (target 3): exactly ONE `unmapped combiner` line all session**
  — `id0=10001000 opts=NOISE`: the transition-dissolve constant-white shader with
  the noise option *ignored-but-drawn* (census NEEDS-APPROXIMATION, non-gameplay).
  No unmapped-fallback draws, no stride mismatches, `bindMiss=0`, `texUpFail=0`.
- **New CopyFramebuffer path live in-game**: frame-mirror machinery active every
  frame (`fbBinds=2`), zero CopyFramebuffer error/skip lines, no GPU faults — the
  screen→mirror and hold-tick mirror→screen copies run silently.
- No `[fatal]`, no svcBreak, no bad_alloc through the session.
- `[gdl-bad] race=1` still consumed its 400-line budget (module-image pointers as
  G_DL children) — the KNOWN bridge-owned open class from M1-MEMORY, out of this
  shift's ownership; unchanged by these fixes.
- Ops warning: Azahar's post-SIGKILL startup dialog, when dismissed by scripted
  click, can RESTART the emulation session mid-run (log truncates, guest reboots).
  Prefer graceful exits; treat log truncation as session-restart evidence.

## T-TEXCACHE — the texture re-upload thrash was a hash-span bug, not pool eviction (feat/3ds-texcache)

Mission: kill the #1 remaining perf lead — ~34 texUp/frame in race/attract scenes,
each a CPU Morton swizzle + linearAlloc traffic. Handed-down suspects: interpreter
cache keying, or the backend pool evicting hot entries. Both partly wrong: the
backend has no pool eviction at all (mTextures is append-only; UploadTexture only
runs when the interpreter misses), and the interpreter's key was CORRECT but
UNSTABLE — its content hash read far beyond the texture.

### Step 1 — split texUp into imports / misses / invalidations

New always-on telemetry (lus-texcache-content-hash-span.patch +
gfx_citro3d.cpp): the interpreter reports every ImportTexture call, every
cache miss and every TextureCacheDelete eviction to the backend; the [c3d] line
grew `texImp/texMiss/texDel` cumulatives, and misses during race content emit up
to 400 bounded `[texmiss] a=<addr> f=<fmt/siz> tm=<tmemWord> ln= sz= t=WxH h=<hash>`
svc lines. First run (old code, attract soak) immediately shaped the problem:

- texImp ~100-200/frame with texUp == texMiss (every upload is a lookup miss);
- texDel flat at 124 for the whole session -> NOT invalidation/palette churn;
- `[texmiss]` showed the same few addresses missing over and over — 138x, 107x,
  52x — and EVERY repeat carried a UNIQUE content hash (400/400 lines, zero hash
  repeats per address). The key itself was unstable.

### Root cause — tmem_content_hash span reaches the end of TMEM

`ImportTexture` hashes the tile's emulated-TMEM span into the cache key (the
staleness signal added for reused-address textures). The span was
`min(remaining TMEM, tile_line_bytes * 64)`: for a 64-byte stride that is 4096
bytes — ALL of TMEM from the tile base — and RGBA16 (every race texture) plus
the I/IA font formats are hashed. So the key of a bit-stable 32x32 texture
(true extent 2048B) folded in 2KB of WHATEVER other textures streamed through
upper TMEM that frame. Any scene with frame-varying TMEM traffic (attract
rotation, animated menus, a moving race camera) minted a brand-new key on every
import: miss -> full RGBA16 re-decode -> backend Morton re-swizzle ->
GSPGPU_FlushDataCache -> re-upload, every frame, per texture; plus unbounded
fresh-key minting churned the 1024-entry LRU (5197 keys by title frame 577).
The old span could also UNDERSHOOT: a stride < 64B decodes up to
remaining/lineBytes rows (> 64 lines), whose tail the hash never covered — a
latent staleness hole, not just overshoot.

### Fix — hash exactly what the decode reads

lus-texcache-content-hash-span.patch bounds the span to the decode's true read
extent:

- RGBA16 (TMEM decode path): a line-exact mirror of ImportTextureRgba16's
  extent derivation — width/height from stride, ApplyTileMaskExtent
  (mask-authoritative), CLAMP-window clamps, remaining-TMEM clamp — then
  span = (h-1)*stride + 2*w. Every byte the decoder reads is hashed (strict
  content identity, staleness safety unchanged by construction: hash ⊇ reads);
  no byte it does NOT read can perturb the key.
- Font I/IA (DRAM decode): span = the recorded load extent (the TMEM copy of
  the source bytes), the content the hash was added to guard.
- FNV-1a folded 4 bytes/iteration (base 8-byte aligned): the hash runs on every
  import (~100-200/frame, hit or miss), so the byte loop was real CPU.

Staleness argument: the hash remains a superset of the decoded bytes, the CI
palette handling (delete-on-refresh + opt-in palette hash) is untouched, and
genuinely animated sources (transition-capture strips, framebuffer effects)
still change their hashed bytes -> still miss -> still re-upload. The
64x1-strip capture texture kept missing per-frame after the fix, as it must.

### Numbers (Azahar, log_filter *:Debug; tick-script GP runs are frame-exact A/B)

Frame-matched A/B, identical deterministic tick script (old span build = same
telemetry, only the span formula reverted):

| Frame window (scripted GP) | old span | fixed |
|---|---|---|
| fr 193 title fade | 48.72 miss/f, 16.3 fps | 0.72 miss/f, 25.1 fps |
| fr 257-385 title static | 0.00 miss/f, 19.9 fps | 0.00 miss/f, 29.5-29.7 fps |
| fr 449-513 animated menus | 2.39-5.06 miss/f, 11.9-15.4 fps | 0.88-2.97 miss/f, 12.0-16.6 fps |
| fr 577-705 menus | 0.00-1.25 miss/f, 14.6-15.0 fps | 0.00 miss/f, 19.0-19.9 fps |
| cumulative misses @ fr 4033 | 4800 | 978 (-80%) |
| race start-line hold (imp/f=42) | 0.00 miss/f, 29.7 fps | 0.00 miss/f, 29.7-30.0 fps |

Non-frame-matched context runs:
- Attract soak (pre-fix build, the regime of the original ~34/frame report):
  SUSTAINED 4.4-10.1 miss/frame at 10.4-20 fps for minutes on end.
- Driving race (fixed build, tick script + held A): active driving 0.9-6.7
  miss/f (mostly 1-3) at 10-15 fps; post-race standings 0.14-0.47 miss/f at
  29 fps. The residual driving misses are (a) cold streaming of new track
  sections and (b) ONE staging address (0xa470ca0) the game rewrites with many
  different textures/tile shapes over time — its [texmiss] hashes now REPEAT
  (content-state cycling), i.e. genuine content changes, not key instability.

Key observations:
- The fr 257-385 row is the hash-COST result: zero misses in both builds, yet
  +10 fps — the old code hashed up to 4KB per import (~100-200 imports/frame,
  hit or miss); the fix hashes the true extent and folds 4 bytes/iteration.
- The scripted flow's static screens are coincidentally stable under the old
  span (identical per-frame TMEM traffic -> identical garbage tail -> same
  hash); the miss thrash concentrates where content MOVES — attract, animated
  menus, actual driving — exactly the regime of the ~34 texUp/frame report.
- texDel stayed at 124 in every run: bridge invalidation
  (MakePersistentRawTextureCopy / DMA dirty ranges) was never the driver.
- Health: zero [fatal]/svcBreak/bad_alloc across all runs; [present] scans
  nz≈122k-228k varying per frame; bindMiss=0, texUpFail=0 throughout.

### Correctness gate

- Harness: **36/36 pixel checks pass** across all 9 scenes
  (check_scene_bmps.py, fixed build, fresh BMP dumps).
- Staleness by construction: the hash still covers every byte the decode
  reads; animated sources keep missing (the 64x1 transition strip and the
  0xa470ca0 staging buffer re-upload on real content change, as they must).

### Follow-ups / notes for later shifts

- The 1024-entry LRU now holds distinct CONTENT states (one entry per state of
  an animated texture). A content-addressed global dedup (key by hash, not
  addr+hash) could serve A->B->A cycling states from cache without re-decoding;
  only worth it if driving-race profiles show the residual 1-3 miss/f matters.
- The "attract never starts" anomaly (M1-MEMORY) reproduced twice back-to-back
  after pkill'd sessions (parked on draws=97/tris=194 with no input ever sent);
  scripted runs are immune since START comes from the tick driver. Attract runs
  after a SIGKILL'd Azahar session are unreliable evidence.
- gdx-autoinput.txt gotcha: input_bridge.c reads lines with a 64-char fgets;
  a comment line longer than 63 chars is SPLIT and its tail eats the `ticks`
  marker slot, silently flipping the whole script to seconds mode. Keep
  comments short.


## §B-BRIDGE — residual gdl-bad drops are GONE; the "440-vs-3754" was two mysteries, both solved (feat/3ds-bridge)

Mission: (1) classify + fix the residual `[gdl-bad] race=1` drop classes, (2) settle the
440-vs-3754 tris question, (3) re-run host converter/pack tests. All patched per
port/3ds/patches/README (incl. the new lus-texcache patch); build green.

### 1. The residual [gdl-bad] classes: count is now ZERO — fb07327 was the whole fix

Three scripted GP-race sessions (gdx-autoinput tick driver, ~9,400 / ~4,000 / ~5,000
race frames) on the merged head:

- `[gdl-bad]`: **0** lines (menu AND race; the historical runs consumed the full
  400-line race budget every session).
- `[gdl-miss]`: 0. `[vtx-dropped]/[vtx-spike]/[vtx-failsafe]/[mtx-dropped]/
  [mtx-failsafe]/[e2-reject]/[segload-fail]/[archive-fail]`: all 0.
- New always-on race-gated `[race-dl]` census (every 64th race tick, see below):
  `noop=0 miss=0 bad=0` on every sampled race frame in every session.

The predecessor's fb07327 (carve-backed segment bases stored as host pointers in
Segment_Set*) removed the entire storm: the observed historical classes (module-image
raws 0x83xxxx-0x91xxxx targeting ARM code with GfxPool parents) were all downstream of
gSegments[4/7/9] holding raw carve offsets that ResolveIlp32ModuleIdentity claimed
verbatim. No residual resolution-chain class exists to fix; nothing in the chain was
changed this shift (64-bit behavior untouched by construction).

### 2a. "3754 tris" was never the race — it is the SELECT COURSE screen

Deterministic-tick screenshots (new SHOT support, below) + frame-matched [c3d] lines:

- `tris=3752-3754, draws=137, STATIC` == the **SELECT COURSE** menu (Jack Cup / Mute
  City Figure Eight, caricature-crowd background art). Verified by capturing
  `autotest/showcase_scan.bmp` at the exact ticks the counter reads 3752.
- `tris=2152-2162, draws=423-424, STATIC` == **SELECT MACHINE** (the 15-machine grid).
- The actual race runs 250-700 backend tris (an exact `tris=440` frame was captured
  mid-race), varying frame to frame.

Prior shifts' "attract demo race renders tris=3752/3754" readings almost certainly
sampled these static menu/showcase screens in the attract rotation. tris≈440-650 is
what the race genuinely draws today — but see 2b.

### 2b. The race IS missing geometry — and it is GAME-SIDE emission, not a bridge drop

Visual proof (`autotest/drive1_scan.bmp`, two runs, mid-race, tris≈440): HUD complete
(portraits, lap 1/3, rank 30/30, energy, a CORRECT figure-eight minimap), player
machine + exhaust + ONE road chunk under it. Run 3's capture shows nothing else at all
(uniform pink — the Mute City sky color); run 4's shows the city-skyline backdrop and
ground grid rendering fine with a clean-edged uniform pink wedge exactly where the
track ahead (and the 29 rivals somewhere on it) should be. Not legit-sparse-camera;
the track-ahead chunks and rivals are absent from the frame.

Localization (all bridge-side accounting says the bridge drew everything it was given):

- `[race-dl]` (input-op census in ProcessList): race frames carry only lists=52-150,
  cmds=1.2-3.2k, tri-cmds=74-410 — matching the backend tri count. Zero-filled DL
  sources would appear as op-0x00 NOP walls inflating cmds (op 0x00 IS "likely" to the
  validator — a zeroed carve renders as silent nothing by design); not observed.
- `[race-seg]` one-shot carve census at race time: segments 3/4/7/8/9 (incl. 8 =
  course_track_gfx) and 0x0A are all POPULATED (nz 160-247k of 256KB scanned);
  RDRAM arena occupancy sits entirely in MB0-2 (carve region), MB3-15 zero.
- The minimap is drawn from the same course spline data the chunk culler uses, and it
  is correct ⇒ control points/segment list loaded fine.

So the game itself emits ~1 road chunk per frame. The course world is CPU-tessellated
per chunk (course.c Course_Draw*ChunkGroup) behind a per-chunk depth + NDC-x/y cull
against camera->projectionMtx/viewMtx (course.c ~4640) and a chunk-group builder;
scenery/background and rival draws are separately gated game-side. Suspect space for
the next shift: that cull math / camera matrices / chunk drawState chain on ILP32 —
decomp-owned code, out of this stream's ownership. The bridge resolution chain is
exonerated by the counters above.

### Instrumentation landed (bridge-owned, all width-neutral, desktop no-op or benign)

- **`<tick> SHOT <label>`** in the gdx-autoinput tick script (input_bridge.c): arms
  gdx_request_frame_dump at a deterministic tick — headless visual oracle.
- **Scanout BMP twin** (n64_gfx_bridge.cpp, 3DS-only): SHOT now also dumps the top
  screen's CPU-readable back framebuffer (the [present] oracle's buffer) as
  `autotest/<label>_scan.bmp`, 400x240 BGR8→BMP24 with the fb rotation unwound.
  NEEDED because the frame-mirror path (CopyFramebuffer fb0→mirror →
  ReadFramebufferToCPU) returns ALL-BLACK in-game on 3DS while [present] counts ~135k
  nonzero scanout bytes — the harness-verified readback does not hold in-game; the
  transition-capture path shares it and should be re-checked (owner: gfx stream).
  Non-Win32 mkdir("autotest") added beside the existing Win32 one.
- **`[race-dl]`** (n64_gfx_bridge.cpp): always-on race-gated per-frame command census
  (~1 line/s): lists/cmds/dl/vtx/mtx/tri/trect/end + noop/miss/bad/skip.
- **`[race-seg]` / `[race-rdram]`** (n64_gfx_bridge.cpp): one-shot (first race tick +
  tick 512) nonzero census of gSegments[1..A] and per-MB RDRAM occupancy.

### 3. Host unit tests

gdx_gfx_pack_tests + gdx_gfx_convert_tests compiled standalone on macOS (same TU set
as the CMake targets) and run before AND after this shift's edits: **ALL PASS**.
probe32.sh ILP32 syntax gate clean on both touched TUs.

### Ops notes

- The emulator-lock steal rule is live: a sibling agent (hardware stream, CIA install)
  legitimately stole /tmp/azahar.lock after this shift let the mtime go stale >15 min
  mid-run — run 3's log was truncated and its session killed. Refresh the lock mtime
  (touch) on every poll loop while a run is active.
- Menu navigation under the tick script is NOT deterministic run-to-run (the same
  script reached the race at tick ~1100, ~2300 and ~2600 in three runs; one previous
  shift's "attract never starts" anomaly is the same class). Schedule SHOTs
  generously and identify screens from the [c3d] signature, not the tick.
- Two more [c3d]/[race-dl] signatures identified visually: `lists=12 tri=1755 STATIC`
  is the course-intro card (crowd art, "1: MUTE CITY FIGURE EIGHT"), and scripted
  races END within ~2000 ticks of starting — the rank-30/30 player, holding A over an
  invisible track, drives off and the game loops back to the course card, then
  re-enters the race. Long "race" soaks under the tick script are really this cycle.
## §H-HARDWARE — real-hardware prep: SMDH/CIA packaging, SD file-log for the tracers (feat/3ds-hardware)

Scope: everything between "boots in Azahar" and "boots on a New3DS someone actually
owns". No behavior changes to the game loop; one new debug facility.

### SMDH (.3dsx metadata)

- `ctr_create_3dsx` was emitting devkitPro's placeholder SMDH (default icon,
  "Built with devkitARM & libctru"). Now wired through `ctr_generate_smdh` with
  short title "G-Diffuser", long description "G-Diffuser — F-Zero X decompilation
  port", publisher "G-Diffuser Project", and a 48x48 icon derived from the repo's
  own branding art (`assets/branding/gdiffuser-icon.png`, 192x192 → 48x48).
  Assets + licensing notes: `port/3ds/packaging/` (all repo-branding derivatives,
  nothing Nintendo-derived).

### CIA packaging (optional target `G-Diffuser-3DS-cia`)

- makerom v0.19.0 (Project_CTR — native macOS arm64 release exists now) +
  bannertool 1.2.3 (carstene1ns fork; original Steveice10 repo is deleted; macOS
  needs a 1-line VLA fix to build — documented in packaging/README.md). Both live
  in gitignored `tools/3ds-bin/`, never committed.
- RSF: inlined from the de-facto homebrew standard (Steveice10 buildtools
  template.rsf). UniqueId **0xff3d5** (inside the 0xf8000-0xfffff eval/homebrew
  range retail never uses) → TitleId **000400000FF3D500**. New3DS exclusive bits
  ON (804MHz, L2, SystemModeExt 124MB) — matches the port's memory/CPU budget
  docs. SaveDataSize 0: saves stay on sdmc:/3ds/gdiffuser/saves/ so .3dsx and
  .cia share state.
- Banner: 256x128 from the menu logo on #101018 + 0.25 s silent WAV (bannertool
  requires audio; silence is deliberate).
- **Verified**: `cmake --build build-3ds --target G-Diffuser-3DS-cia` produces a
  1.6 MB CIA; `azahar -i <abs path>` (CLI install flag exists) installs it —
  title lands in Azahar's virtual SD `Nintendo 3DS/.../title/00040000/0ff3d500/`
  with tmd + .app content. Gotcha: azahar's `-i` fails with "File not found" on
  a relative path; pass it absolute. Emulator-lock protocol observed (single
  instance, pkill + rmdir after).

### File-log sink (the hardware-crash debuggability gap)

- Every tracer built in the M1 shifts (boot steps, watchdog heartbeat, fatal
  handlers, gdx_port_logf tap) terminates in `svcOutputDebugString` — which
  Azahar logs but **retail hardware discards** (nothing listens without a Luma
  debugger attached). A hardware hang would have left zero evidence.
- New `port/3ds/gdx3ds_filelog.c` (+ `include/gdx3ds_filelog.h`):
  sdmc:/3ds/gdiffuser/log.txt, gated on INI `debug.filelog=1`, capped
  (`debug.filelog_max_kb`, default 256, truncation marker), truncated per boot.
  Per-line fflush so lines survive svcBreak/abort where no stdio teardown runs;
  static setvbuf buffer so the sink never mallocs (safe from the operator-new
  failure tracer); RecursiveLock (main + watchdog threads write; recursive so a
  fatal inside a write can't self-deadlock).
- Hooked from main_3ds.cpp at exactly four call sites (logStep, logFatal,
  portLogSvcTap, watchdog heartbeat) + init after config load — kept as
  one-liners with the logic in the new TU because feat/3ds-perf may land
  parallel main_3ds.cpp changes (branch tips were identical at edit time, no
  merge needed). traceBigAlloc stays svc-only by design (documented no-heap,
  no-stdio context).

### Runbook

- `docs/3DS-HARDWARE.md`: New3DS+Luma>=10.1.1 requirements, .3dsx (HBL) vs .cia
  (FBI) flows, DSP1 dspfirm.cdc dump + the "boots fine but silent" symptom it
  fixes (ndsp backend's null-sink degrade already logs the diagnosis), full SD
  layout, complete INI key table (input/audio/debug — audio.lle confirmed read
  in port/gdx_audio_lle.c), known issues, and a numbered first-run telemetry
  request (the watchdog `frame(+delta)` line doubles as the fps probe: delta/5
  = fps).

### Build state

- 3DS build green end-to-end after all 9 submodule patches: .3dsx with real
  SMDH, .cia target, filelog TU all compile; host stub untouched (filelog is
  NINTENDO_3DS-only, hooks live inside the __3DS__ branch of main_3ds.cpp).
