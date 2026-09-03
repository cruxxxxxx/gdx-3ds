# Task A — texrect batching (feat/3ds-texrect2) progress

Brief: docs/research/locked60-campaign.md (Task A). Killswitch `[debug] trectbatch`.
Worktree ~/code/gdx-3ds/texrect2, art dir /tmp/texrect2-art (run.sh <label> <key> [s]).

## M0 — census instrumented (lus-trect-census.patch)
- Finding from code reading BEFORE measuring: in this tree consecutive same-state rects
  already accumulate in mBufVbo (README, lus-texrect-run-memo). A Flush happens only on a
  state delta inside GfxSpTri1 (texture import, sampler, shader, alpha, depth, viewport/
  scissor) or from an intervening opcode. So the brief's "each texrect ends a batch" must
  be measured, not assumed: the census attributes every batch break to its cause.
- `[trect] n= r/f runs/f ident=a/b tex=a/b clamp uvin oth=a/b om cc pe sc tl fIn/f fBtw/f imp/f`
  (a = rects of that class per frame, b = those that cost a batch break),
  `[trect2] dist=<distinct textures>/<KB> hist=<runs by distinct-tex 1,2-4,5-8,9-16,>16>
  maxrun=<len>/<distinct> ms/f rect= imp= draw= rest=` (rect ~= [profop] E4+E5).
- Gates: gputrace ([prof] window, menus + race) or verbose ([race-dl] cadence).
- Build green (3dsx). Patch roundtrip clean-submodule OK.

## M1 — census numbers (Azahar, build 4790b2c, 30-machine GP A-mash, gputrace, /tmp/texrect2-art/census)
Per-frame, 64-frame windows. Zero error lines, 8 SHOTs, heap 44.6 MB plateau.

| window | nD | rects/f | ident a/b | tex-only a/b | clamp/uvin | other a/b | breaks fIn/f | E4 ms | rect = imp + draw + rest |
|---|---|---|---|---|---|---|---|---|---|
| race steady (fr 3649-13121) | 45-50 | 39-42 | 14-16 / **0.0** | 18 / 18 | 18 / 18 | 6 / 6 (om) | 24-25 | 2.14-2.24 | 1.96 = 0.40 + 0.54 + 0.98 |
| race start crowd (fr 3137) | 135 | 119 | ~2 / 0 | 112.7 / 112.7 | 112.7 / 112.7 | 4.2 / 4.2 | 117 | 9.86 | 8.8 = ~1.8 + ~2.5 + 3.3 |
| results/rankings (fr 9281) | 163 | 145 | 0 / 0 | 138 / 138 | 138 / 135 | 5 / 5 | 143 | 11.9 | 11.4 = ? + ? + 4.04 |
| 3D-model menu | - | 222-256 | 94-97 / **0.0** | 125-151 / 125-151 | =, uvin 112-149 | 3-7 / 3-7 | 127-153 | ~9.7 | - |

Per rect (steady): 49 us = 16 us import lookup + 22 us DrawTriangles per break + 25 us 2x GfxSpTri1.
dist (distinct rect textures / window): steady 25 / 35 KB; runs/f = 1 (the whole HUD is one run).

Decisions:
- Lever 1 "identical-state run merging": **NO-GO, already the case.** ident=a/0.0 everywhere —
  same-state consecutive rects never split a batch (the run memo + accumulating VBO). The
  Flush-before-ImportTexture hypothesis is refuted by the receipt; the drafted deferred-flush
  lever is NOT applied.
- Lever 2 "HUD atlas": **GO.** Every texture-only rect is a batch break, all clamp-addressed
  with UVs inside the tile: 18/frame steady, 113-138/frame on the crowd/rankings frames the HW
  profile calls out (E4 3.9-8.9 ms at 90-138 rects = these frames). Removing the break costs
  (import lookup + draw submit, ~38 us each here) projects to >= 0.7 ms steady and 3-4 ms on
  crowd frames (emulator); HW ratios are similar (E4/rect 43-64 us there vs 49 here).
- Next: [trect-tex] inventory of the crowd/menu texture sets (sizes, formats, filter, paths)
  to size the atlas and its eligibility, then implement behind [debug] trectbatch.

## M2 — lever built: HUD atlas views + same-page rect merge (commit 3a9f952, killswitch [debug] trectbatch)
Design (see port/3ds/patches/README.md, lus-trectbatch-atlas.patch): backend places small
clamp-addressed rect textures as VIEWS into shared 256x256 RGBA8 pages (shelf packing, 1-texel
replicated gutter = exact N64 clamp sampling, per-view UV affine, refcounted pages, <= 8 pages,
standalone fallback when full). Interpreter: eligible rects (clamp/mask-0 both axes, no mirror,
UVs inside the tile) arm the atlas around their import; a rect draw imports BEFORE the flush
and keeps the open packed batch when the import resolves to a view on the bound page; any
other outcome rebinds the previous id around the Flush (pending batch drawn with its own
texture). Packed vertex write adds the view offset after the clamp. Receipts: [trect2]
tb=on/merged/switched/bypassed/armed vt=<non-rect draws on a view> atlas=placed/full/pages/resets.
Known limitation: a game TRIANGLE sampling a view with wrap UVs would bleed neighbours (F-Zero X
draws HUD glyphs only as rects; `vt` counts any such draw so the log shows it).
Status: built (3dsx + cia), patch roundtrip clean; A/B runs atlasA (=1) / atlasB (=0) queued.

## M3 — first A/B (atlasA on / atlasB off, build 3a9f952, 256x256 pages, cell <= 64)
Frame-aligned medians (ms/frame, on - off), Azahar, 197 common windows, zero error lines both,
heap plateau 44.5 (on) vs 44.7 MB (off):
| group | E4 | drw | build | wall | nD | rect = imp + draw + rest |
|---|---|---|---|---|---|---|
| menu (48 win) | -0.64 | -0.68 | +0.05 | +0.05 | -31 | 9.62->9.18 = 3.37 + (2.59->2.02) + 3.74 |
| race-all (149) | -0.25 | -0.20 | -0.20 | -0.20 | -11 | 1.96->1.68 = 0.41 + (0.54->0.23) + 1.01 |
| crowd nD>=100 (21) | +0.38* | -0.28 | -0.70 | -0.80 | -9 | 8.62->8.76 (no merges there, see below) |
(*median skewed by one transition window; the post-start crowd windows 3393-3585 show build
-1.2 ms.) Receipts (steady race): tb=1/merged 16/f/switched 8/f, fIn 24->10.5/f, vt=0,
atlas=157 placed/0 full/3 pages/3 resets. Race SHOTs are NOT run-to-run deterministic even
with the lever off (census vs atlasB drive2: 134 KB differ) — HUD visually identical A vs B;
deterministic MENU SHOTs (m1-m5) added to the autoinput for byte parity in the next pair.
Finding: the crowd/rankings windows' textures are 80x 304x3 + 11x 160x6 (+128x8, 80x12)
RGBA16 clamp/point gradient strips — wider than the cell cap, so none merged. Also the
UV-inside test compared texels against 10.2 fixed tile bounds (lenient) — fixed.
Next: 512x256 pages, cells <= 320x64 (cheaper than the pow2-padded standalone strips), A2/B2.

## M4 — second A/B (atlasA2 on / atlasB2 off, build de6f1a9: 512x256 pages, cells <= 320x64) — DONE
Frame-aligned medians (ms/frame, on - off), Azahar, 198 common windows, zero error lines,
heap plateau 44.67 (on) vs 44.51 MB (off), linear free 25.5 MB with 7 pages (3.5 MB) live:
| group | E4 | drw | tri | build | wall | nD | rect = imp + draw + rest |
|---|---|---|---|---|---|---|---|
| menu (48) | -2.47 | -2.20 | -0.53 | -3.70 | -3.80 | 136->24.5 | 9.62->7.34 = 3.36 + (2.59->0.17) + (3.74->3.55) |
| race-all (150) | -0.27 | -0.26 | -0.06 | -0.20 | -0.20 | 45.5->31 | 1.96->1.66 = 0.40 + (0.54->0.21) + 1.01 |
| crowd, off nD>=100 (25) | -0.74 | -1.68 | -0.29 | 0.00* | +0.10* | 154->72 | 6.01->5.68 = 0.85 + (1.31->0.21) + 3.23 |
| race-start window 3137 | 10.44->7.85 | | | 23.6->20.5 | | 136->26 | tex breaks 112.7->2.8/f, fIn 117->7/f |
| rankings windows 9217-9729 | 12.6->9.4 | | | 25.9->22.1 | | 163->27 | |
(*crowd medians mix the two crowd kinds with the post-start windows where nD stays >= 100
from machine draws; the per-window lines above are the HW-profile frames.)
Receipts: tb=1/merged/switched: steady 17/7 per frame, crowd 113/3.5, menus 138/4; bypassed 0;
vt=0 (no triangle ever sampled a view); atlas placed 684, full 0, pages 7, resets 9.
Parity (byte compare, scanout captures): m2_scan and m4_scan IDENTICAL with the atlas fully
engaged (138 merged rects/frame), m3_scan 19 px, m1_scan 807 faint px in the fading title
(temporal); the 320x240 readback .bmp files differ by animation phase (m2) or are unreadable
stripes in BOTH runs (m4/m5/drive1). Race shots are not run-to-run deterministic even off (census
vs atlasB drive2: 134 KB) — HUD visually identical on drive2/m5 composites (/tmp/texrect2-art/png).
Remaining rect cost: import lookup 0.40 ms steady / 2.9 ms crowd (content hash per rect import,
unchanged by this lever — a tmem_generation-keyed import memo is the next lever, Task B adjacent)
and the 2x GfxSpTri1 path (~25 us/rect).

## M5 — atlas page reclamation (feat/3ds-hwtest2, port/3ds/gfx/gfx_citro3d.cpp only)
Defect: cells were never reclaimed (a page reset only at refcount 0, but the interpreter recycles
ids lazily), so one session reached `atlas=…/22/8/…` — new HUD sets fell back to standalone and
`switched` jumped 128 -> 1466/window (/tmp/hwtest2-art/smoke-log.txt:1173).
Fix: pages stamp mFrameIndex at every bind; when all 8 pages are allocated, AtlasTryPlace evicts
the least-recently-bound page that (a) was not bound in the current frame (frame begin is
C3D_FrameBegin(SYNCDRAW), so the GPU is done with earlier frames) and (b) is not the page behind
mBoundTextureIds[0/1] (an open batch may still reference it). Its views are copied out of the
page into their own pow2 standalone textures (interpreter cache entries stay valid and
renderable; a failed copy-out drops the view to a bind miss and is counted), the page is reset
and reused. No safe candidate -> standalone fallback as before. trectbatch=0 unchanged (eviction
only runs inside an armed placement). Receipts: `[trect2] atlas=placed/full/pages/resets` now
means full = no-candidate fallbacks only and resets includes evictions (the format lives in the
LUS patch, not regenerated); the `[c3d]` line gains `atlasEv=evictions/viewsCopied/copyFails/
noCandidate` whenever the atlas has placed anything. Built 3dsx + cia green; verification by
the coordinator (watch atlasEv climbing instead of full, switched back near 128/window).
