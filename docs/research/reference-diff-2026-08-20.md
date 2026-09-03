# Reference-diff investigation — scanout oracle + desktop-vs-3DS decode paths (2026-08-20)

**Agent:** REFERENCE-DIFF (read-only, no rendering changes). **Worktree:** `~/code/gdx-3ds/m1` (`feat/3ds-m1`).
**Charter:** determine whether the `_scan.bmp` verification oracle is valid, map the desktop (correct) vs 3DS (broken) texture-decode paths, and give fix directions for km/h / shadow / sky grounded in the desktop reference. **The emulator was not run; no rendering code was changed.**

Two premises the effort has been carrying are **wrong**, and they explain a lot of the churn:

1. **The `_scan.bmp` oracle captures the frame BEFORE the current frame is presented** (it reads the still-unswapped back buffer at a point where only frame N-1's image has been transferred to it). It is off-by-one and, worse, it does **not** capture the render target citro3d actually draws into. Details in Part 1.
2. **The RGBA16 "odd-line word-swap on odd-word TMEM lines" does not exist in the code.** There is no odd-line swap anywhere. TMEM emulation is **not 3DS-only** — desktop runs the identical path. So "desktop reads DRAM, 3DS reads scrambled TMEM" is false for RGBA16 by default. Details in Part 2.

---

## Part 1 — Scanout-BMP oracle verdict: OFF-BY-ONE AND WRONG-BUFFER. NOT TRUSTWORTHY.

### 1.1 What the `_scan.bmp` twin actually reads

`GdxDumpTopScanoutBmp` (`port/n64_gfx_bridge.cpp:7247`) reads:

```c
const u8* fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &fbW, &fbH);   // line 7250
```

`gfxGetFramebuffer(GFX_TOP)` returns libctru's **current top-screen BACK buffer** (double-buffered: `gfxSetDoubleBuffering(GFX_TOP, true)`, `port/3ds/os/gdx3ds_os_ctru.c:126`). It encodes that buffer straight to `autotest/<label>_scan.bmp`.

### 1.2 When it fires in the frame — BEFORE this frame's present

Call chain, per host frame N (`port/3ds/main_3ds.cpp:515` loop):

| step | main_3ds.cpp | what happens |
|---|---|---|
| 1 | `gdx_dispatch()` (line 543) | runs the game GFX task → `gdx_gfx_run` → interpreter renders frame N **into the citro3d render target** (`mFramebuffers[0].target`, an off-screen RGBA8 target, `gfx_citro3d.cpp:504`) |
| 2 | still inside `gdx_gfx_run` | `GdxUpdateFrameMirror(interp)` (`n64_gfx_bridge.cpp:9783`) → `GdxDumpNamedFrameIfRequested` (7388) → **`GdxDumpTopScanoutBmp` reads `gfxGetFramebuffer` HERE** (7333) |
| 3 | `w->EndFrame()` (line 548) | citro3d `EndFrame` → `C3D_FrameEnd(0)` (`gfx_citro3d.cpp:628`) → **queues the display transfer of frame N's render target into the back buffer, then swaps** |

So the SHOT scanout reads `gfxGetFramebuffer` at **step 2**, but frame N is not transferred to a scanout buffer until **step 3** (`C3D_FrameEnd`). At step 2 the back buffer still holds **the image citro3d transferred at the previous frame's `C3D_FrameEnd` (frame N-1)**, and even that has typically already been swapped to FRONT — meaning `gfxGetFramebuffer` (back) at step 2 can be pointing at frame **N-2**'s buffer, not N-1's, depending on swap timing.

Key architectural fact: citro3d's render target (`C3D_RenderTargetCreate` + `C3D_RenderTargetSetOutput`, `gfx_citro3d.cpp:504,511`) is a **separate off-screen color buffer**. The GPU draws into IT, and only `C3D_FrameEnd` display-transfers it to the libctru top-screen framebuffer that `gfxGetFramebuffer` returns. **The `_scan.bmp` therefore never sees the render target directly — only a stale post-transfer copy from an earlier frame.**

### 1.3 The contradiction with m1-boot-debug.md, resolved

`m1-boot-debug.md:297-299` describes a **different** probe — the `[present]` telemetry in `main_3ds.cpp:556-585` — which reads `gfxGetFramebuffer` **AFTER `w->EndFrame()`** (it sits at lines 556+ in the loop, past the `w->EndFrame()` at 548). That one is correctly-timed (reads the buffer just transferred this frame). But it only prints a nonzero-byte COUNT to the log; it does not dump an image.

The **`_scan.bmp` twin** (the one the effort actually eyeballs for "does texture X look right") uses the **wrong call site** — inside `gdx_gfx_run`, one-to-two frames early. The comment at `n64_gfx_bridge.cpp:7240-7246` claims it captures "what the LCD shows," citing the `[present]` oracle — but it copied the *buffer source* (`gfxGetFramebuffer`) without copying the *timing* (post-`EndFrame`). That is the latent bug.

### 1.4 Why this makes "scanout-verified" fixes lie — in BOTH directions

- **Off-by-one staleness:** a SHOT armed on the frame a fix first takes effect dumps the PRIOR frame's buffer → the fix "doesn't show." Conversely, in a steady-state scripted race where every frame is ~identical, the staleness is invisible and the BMP looks right — so a fix that *regressed* the current frame could still "verify clean." Either way the human, watching the LIVE LCD (which shows frame N via the correct post-FrameEnd swap), sees something different from the BMP. This is exactly the "NO visible change after scanout-verified fix" report.
- **The `_scan.bmp` and the LCD can legitimately disagree** because they sample different frame generations of a double-buffered surface. In a scene with any per-frame variation (camera motion, HUD counters like km/h ticking), the BMP is literally a different frame than what was on screen when the human looked.

Note the RESUME.md B2 note (`docs/research/RESUME.md:106`, `m1-boot-debug.md:931-934`): the *other* dump — the plain `<label>.bmp` frame-mirror via `CopyFramebuffer`→`ReadFramebufferToCPU` — returns all-black in-game. That is a separate, already-known failure of the mirror path. The scanout twin was introduced to dodge it, but introduced its own timing bug. So **neither** of the two SHOT images is a faithful "what the LCD shows this frame" capture: one is black, the other is stale/off-buffer.

### 1.5 VERDICT

**The `_scan.bmp` scanout twin is NOT a trustworthy on-device oracle.** It reads the correct *kind* of buffer (the libctru scanout back buffer) but at the **wrong time** (before this frame's `C3D_FrameEnd` transfer + swap), so it captures a 1-2 frames stale generation and never the render target citro3d actually drew this frame into. Every "scanout-verified" conclusion about a late-frame pass (HUD digits, sky-fill, fog) is suspect: the BMP may show an old frame while the LCD shows the new one, or vice versa.

### 1.6 The RELIABLE on-device verification method

Capture the actually-presented buffer **after** the frame's transfer+swap has completed:

- **Correct timing:** dump `gfxGetFramebuffer(GFX_TOP, GFX_LEFT)` from the frame loop **after `w->EndFrame()`** (i.e. move the encode to `main_3ds.cpp` right beside the existing `[present]` probe at line 556, which is already correctly placed), and — because the swap flips back↔front at FrameEnd — read the buffer that was just presented. Concretely: after `C3D_FrameEnd`, the buffer that was the render-target's transfer destination is now FRONT; call `gfxGetFramebuffer` on the frame AFTER the SHOT frame's `EndFrame`, or add one `gspWaitForVBlank(GFX_TOP)` after `EndFrame` and read then. The cleanest fix is to arm the SHOT and perform the encode one frame LATER, post-EndFrame.
- **Even better — read the render target directly:** the image citro3d draws into is `mFramebuffers[0].target`'s color texture. Reading THAT (via a `C3D_RenderTargetGetTex` / linear-heap read of the target's `tex`, before the display transfer overwrites nothing — it is persistent) captures exactly the pixels this frame produced, with no double-buffer generation ambiguity and no dependence on swap timing. This is the ground-truth "did the GPU produce the right image this frame" oracle and is immune to the present/swap plumbing that has already burned two probes (double-swap bug, black frame-mirror).
- **Belt-and-suspenders:** keep the `[present]` nonzero-count line (correctly timed already) as a liveness check, but stop treating the `_scan.bmp` image as authoritative until its timing is fixed.

**Action item for the effort:** before spending any more agent-cycles on km/h/shadow/sky "fixes verified by `_scan.bmp`," fix the oracle first (move the encode post-EndFrame or read the render target texture). Otherwise verification remains a coin-flip.

---

## Part 2 — Desktop (correct) vs 3DS (broken) texture-decode path map

Ground-truth file: `libultraship/src/fast/interpreter.cpp` (submodule, working tree present).

### 2.1 The big correction: TMEM emulation is env-gated, NOT platform-gated; and there is no odd-line swap

- The RGBA16 TMEM-emulation branch (`ImportTextureRgba16`, lines **897-1026**) is gated on:
  ```cpp
  static const bool sTmemDisabled = std::getenv("GDX_NO_TMEM") != nullptr;             // 901
  const auto& tmemTile = mRdp->texture_tile[tile];                                     // 902
  if (!sTmemDisabled && !importReplacement && metadata->resource == nullptr
      && tmemTile.siz == G_IM_SIZ_16b) {                                               // 903
  ```
  It is **compiled unconditionally on every platform** — there is no `__3DS__` gate. Desktop and 3DS both run this branch by default. So the working theory "3DS uses TMEM emulation, desktop uses a different DRAM decode" is **false for RGBA16**. Both use TMEM emulation unless `GDX_NO_TMEM=1`.
- The WHY comment (verbatim, lines **897-900**):
  > TMEM-emulation decode path (GDX_NO_TMEM=1 disables it): decode from emulated TMEM using only tile-descriptor state, like hardware, so the result does not depend on per-slot load bookkeeping, which goes stale under heavy per-frame TMEM reuse. OTR resources and HD replacements keep the legacy path; their data never enters TMEM.
- **No odd-line word-swap exists** anywhere in the decode or load paths. A tree-wide search for odd-line / word-swap / interleave / swizzle / xor logic in the decode and TMEM-write paths found only straight row-major byte copies (plus a comment at line ~4568 noting a straight copy is *correct because* DXT interleave cancels). Whatever produced the km/h scramble, it is not an odd-line swap in this file.

### 2.2 RGBA16 — both branches, exact source pointers

**TMEM-emulation branch (default, BOTH platforms), reads from emulated TMEM `mRdp->tmem`:**
- offset `tmemByteOffset = tmemTile.tmem_index * 8` (line ~904); base `tmemSrc = mRdp->tmem + tmemByteOffset` (~940)
- **read row stride = `tmemTile.line_size_bytes`** (~905, used at ~946), row-major, no swap:
  ```cpp
  const uint8_t* row = tmemSrc + (size_t)y * lineBytes;              // ~946
  const uint16_t col16 = (row[2*x] << 8) | row[2*x + 1];            // ~948
  ```

**Legacy/DRAM branch (only when `GDX_NO_TMEM=1`, OR resource/HD-replacement textures), reads from DRAM via `addr`:**
- `addr` = HD-replacement buffer or `mRdp->loaded_texture[...].addr` (raw N64 DRAM pointer) (lines 887-890)
- read stride = `fullImageLineSizeBytes`, row-major (lines 1101-1103):
  ```cpp
  uint32_t clrIdx = (y * (fullImageLineSizeBytes / 2)) + x;         // 1101
  uint16_t col16  = (clrIdx < maxTexel) ? ((addr[2*clrIdx] << 8) | addr[2*clrIdx+1]) : 0;
  ```
  **This branch has zero emulated-TMEM involvement** — pure DRAM read.

### 2.3 The emulated-TMEM WRITE side — where the km/h scramble most likely lives

The read stride is `tmemTile.line_size_bytes` (§2.2). The two TMEM writers use **different** strides:

- **`GfxDpLoadBlock`** (~4446-4616): writes **contiguously**, `memcpy(mRdp->tmem + tmemIndex*8, loaded.addr, copyBytes)` (~4579). No per-row stride at all.
- **`GfxDpLoadTile`** (~4618-4720): writes with `destStride = (tile_line_size_bytes + 7) & ~7` (8-byte-aligned, ~4691); source stride `full_image_line_size_bytes` (~4703).

If, for the km/h 12-wide RGBA16 atlas, the LoadBlock contiguous-write width or the LoadTile `round8(tile_line_size_bytes)` write stride **≠** the read-side `tmemTile.line_size_bytes`, every read row lands at the wrong TMEM offset → each row shears by a fixed byte count → **exactly the "scrambled digit atlas" signature.** This is a **write/read row-stride mismatch**, not an odd-line swap. `tmem_index*8` (word-granular slot base) vs a 12-texel (24-byte) row that is not a multiple of 8 is a prime suspect for the shear.

### 2.4 I4 shadow — NO TMEM path at all

`ImportTextureI4` (~1400-1521) has **no TMEM-emulation branch**. It reads only from `addr` (DRAM/resource) on every platform:
```cpp
uint32_t clrIdx = (y * (fullImageLineSizeBytes * 2)) + x;          // ~1475
uint8_t byte = addr[clrIdx / 2];
uint8_t part = (byte >> (4 - (clrIdx % 2) * 4)) & 0xf;
```
So the I4 shadow "wrong SHAPE on 3DS only" **cannot** be a TMEM read/write mismatch. Since the DRAM decode is identical byte-for-byte on desktop and 3DS, the divergence must be **downstream or in the extent math that feeds it**:
- the I4 width/height/`fullImageLineSizeBytes` and the tile-window crop (~1436-1441 / `ApplyTileMaskExtent`) computing a different extent on 3DS, or
- the **3DS-specific upload/Morton-swizzle** step in `gfx_citro3d.cpp` (the desktop GL upload is a linear blit; the 3DS must Morton-tile the texture for the PICA). A wrong shape strongly implies a width/stride or swizzle mismatch at the 3DS upload, NOT the decode. This should be investigated in `gfx_citro3d.cpp`'s texture-upload/`ImportTexture`/Morton path, using the (correct) desktop linear upload as the reference layout.

### 2.5 Should the 3DS adopt the desktop's decode SOURCE?

- **RGBA16:** "the desktop" already uses the SAME TMEM branch by default — so there is no different desktop source to adopt at the interpreter level. What *is* available is the **DRAM branch** (`GDX_NO_TMEM=1`, lines 1028-1193), which both platforms can take and which has **no write/read stride pair to mismatch** (it is single-stride, `fullImageLineSizeBytes`, shared with desktop). Routing 3DS RGBA16 through the DRAM branch would collapse the scramble surface to the same extent math the desktop uses. **Cost:** it re-exposes the "stale per-slot load bookkeeping under heavy per-frame TMEM reuse" class of bug the TMEM path was added to fix (comment §2.1) — F-Zero X race scenes reuse TMEM slots aggressively per frame. So a blanket `GDX_NO_TMEM` is risky; the surgical fix is to **correct the TMEM write/read stride agreement** (§2.3) rather than abandon TMEM.
- **I4:** already reads from DRAM on both platforms — nothing to change at the decode source. Fix is downstream (upload/swizzle/extent), §2.4.

---

## Part 3 — Sky backdrop: NOT a citro3d bug, NOT a scanout artifact

Per `docs/research/sky-backdrop-wedge-analysis.md` (verified against `gfx_citro3d.cpp:1295-1345` and the decomp refs it cites), the black wedge is the **framebuffer clear colour (`0x000000FF`) showing above an under-sized backdrop skybox quad**:
- The skybox quad is built at **4:3 logical proportions** (`aspectRatio = fovScaleY/fovScaleX = 0.75`, `decomp/.../background.c` `Background_InitBackgroundInfo`/`Background_Update`/`Background_UpdateSkyboxVtx`).
- The 3DS renders **portrait 240×400** then rotates via the vertex-shader fixup (`x'=y, y'=-x`, `gfx_citro3d.cpp:536-542`) onto the **400×240 (~16:10)** top screen. The 4:3 quad's top/bottom edges fall short of the wider screen → clear colour bleeds above the quad.

**Why desktop has no gap:** desktop presents at (or pillarboxes to) 4:3, matching the quad's logical proportions, so the quad reaches the frame edge — no gutter. The 3DS's non-4:3 rotated display is the sole difference; nothing about the 3DS texture-decode or viewport code is wrong here. The `SetViewport`/`SetScissor` portrait remap (`gfx_citro3d.cpp:1134-1155`) is correct and harness-verified; it is not the cause.

**Fix domain:** decomp backdrop geometry — extend the existing `PORT`/widescreen `verticalRange` compensation in `Background_Update`/`Background_UpdateSkyboxVtx` to overscan the 3DS non-4:3 display (the skybox strip samples CLAMP, so vertical overscan is free, exactly as the existing horizontal `1.02f` margin does). This is a **different file and a different bug** from km/h and the shadow — do not conflate them.

---

## Part 4 — Concrete fix directions (grounded in the desktop reference)

1. **FIX THE ORACLE FIRST.** Move the `_scan.bmp` encode to run **after `w->EndFrame()`** (beside the correctly-timed `[present]` probe at `main_3ds.cpp:556`), reading the just-presented buffer; OR read the citro3d render-target texture (`mFramebuffers[0].target`) directly, which is generation-unambiguous. Until this lands, treat all prior `_scan.bmp` verifications as UNRELIABLE. (§1.5-1.6)

2. **km/h RGBA16 scramble — write/read stride mismatch in emulated TMEM (NOT an odd-line swap).**
   - Instrument (log-only) the three strides for the km/h atlas tile: read-side `tmemTile.line_size_bytes` (`interpreter.cpp:905/946`), LoadTile write-side `round8(tile_line_size_bytes)` (~4691), LoadBlock contiguous width (~4579). If they disagree, that is the shear.
   - Preferred surgical fix: make the TMEM write stride and the RGBA16 read stride use the SAME field for a given tile so rows align. Do NOT introduce a swap — none is needed.
   - Fallback probe: route 3DS RGBA16 through the DRAM branch (`GDX_NO_TMEM`, lines 1028-1193) to confirm the DRAM decode renders km/h correctly (it should — same as desktop's DRAM branch). If it does, it localises the bug to the TMEM stride pair and validates the surgical fix direction. (§2.3-2.5)

3. **I4 shadow wrong shape — downstream of a DRAM decode that is identical on both platforms.** Not a TMEM bug (I4 has no TMEM path). Compare the 3DS texture UPLOAD (Morton swizzle / width-stride) in `gfx_citro3d.cpp` against the desktop's linear GL upload as the reference layout, and check the I4 extent math (`interpreter.cpp:1436-1441` / `ApplyTileMaskExtent`) for a 3DS-divergent width/height. A wrong *shape* points at width/stride/swizzle at upload, not the nibble decode. (§2.4)

4. **Sky wedge — decomp backdrop geometry, separate effort.** Extend the `PORT` `verticalRange` compensation to overscan the 3DS non-4:3 rotated display. Do not touch the fog blend or the citro3d viewport. (§3)

---

## Appendix — file:line index

- Scanout twin dump: `port/n64_gfx_bridge.cpp:7247` (`GdxDumpTopScanoutBmp`), armed via `gdx_request_frame_dump` (7303), fired from `GdxDumpNamedFrameIfRequested` (7314) inside `GdxUpdateFrameMirror` (7379), called at end of `gdx_gfx_run` (9783).
- Frame loop / present ordering: `port/3ds/main_3ds.cpp:515` (loop), `gdx_dispatch` (543), `w->EndFrame()` (548), correctly-timed `[present]` probe (556-585).
- citro3d present: `EndFrame`/`C3D_FrameEnd(0)` (`port/3ds/gfx/gfx_citro3d.cpp:622-628`); render target create + output (504, 511); portrait fixup matrix (536-542); `SetViewport`/`SetScissor` remap (1134-1155); sky diagnostic (1295-1345).
- OS window / buffering: `port/3ds/os/gdx3ds_os_ctru.c:122` (`gfxInitDefault`), `126` (`gfxSetDoubleBuffering`), `147` (`gdx3ds_os_window_swap`, no manual swap).
- RGBA16 decode: `libultraship/src/fast/interpreter.cpp` — TMEM branch 897-1026 (gate 901-903, comment 897-900, read 940-948), DRAM branch 1028-1193 (source 887-890, read 1101-1103).
- I4 decode: `interpreter.cpp:1400-1521` (DRAM only, read 1475-1478).
- TMEM writers: `GfxDpLoadBlock` ~4446-4616 (write ~4579), `GfxDpLoadTile` ~4618-4720 (write ~4691, 4703).
- Sky analysis: `docs/research/sky-backdrop-wedge-analysis.md`.
