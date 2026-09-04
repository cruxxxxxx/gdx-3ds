# TASK V — machine-select / ship-detail 3D placement bug (sub-viewport mapping)

Worktree ~/code/gdx-3ds/viewport, branch feat/3ds-viewport off mainline
feat/3ds-hwaudio @ eeb3b56 (47-patch stack applied; build-3ds built). Common rules:
docs/research/locked60-campaign.md (copy from the scratchpad; Azahar lock protocol — other
runs share it, wait in-turn; merge ini keys; window capture via `swift /tmp/wid.swift` ->
`screencapture -x -l <id>`; patch-as-pure-delta; killswitch; report format).

## Symptom (user screenshots, port vs real game)
Reference images in the scratchpad dir <scratch>/:
- `ref-emu-machineselect.webp` = REAL game (Switch Online capture): SELECT MACHINE shows a
  6-column x 5-row grid of 30 small ship models, each in its own cell, evenly spaced across
  the screen, with the selected one highlighted by a starburst.
- `Screenshot 2026-09-03 at 9.48.44 AM.png` = OUR PORT, same screen: the ships render about
  1.6x too large and shifted right/down; only ~3-4 columns fit, cells overlap, the
  left-column ships are cut off at the screen edge and the starburst sits over the BODY/BOOST
  text. The 2D layer (title, portrait, stats, NOVICE, machine name, weight) is placed
  correctly.
- `Screenshot 2026-09-03 at 10.14.54 AM.png` (real) vs `Screenshot 2026-09-03 at 9.48.56 AM.png`
  (port): the ship detail page. Real: the Blue Falcon model sits bottom-right, small, right of
  the character art. Port: the model is larger and sits bottom-left, overlapping the character
  art and the CAPTAIN FALCON name plate. Again the 2D layer is correct.
So: every 3D model drawn into a SUB-viewport (a viewport smaller than the screen) is
mis-scaled and mis-positioned; full-screen 3D (the race) is fine.

## Where to look (port/3ds/gfx/gfx_citro3d.cpp + the LUS interpreter, patched tree)
- gfx_citro3d.cpp ~1005-1030: the border-mode design note says "the interpreter CPU-pre-
  transforms vertices to clip space with the RSP viewport already baked in, and every
  main-target SetViewport/SetScissor call carries the full window (0,0,400x240)" — measured on
  boot AND race frames only. Menu screens with per-ship viewports were never measured. The
  FULL-BLEED mode (border_mode=1, the shipped default) scales clip x/y "about the screen
  center" via the projection fixup matrix, which is only correct for a full-window viewport.
- gfx_citro3d.cpp ~2140 SetViewport: the rotated-target mapping
  `C3D_SetViewport(y, fbH-(x+width), height, width)` — verify for sub-rects (x/y/width/height
  are in window pixels after LUS's aspect adjustment; the 3DS window is 400x240 for a 320x240
  game: check how LUS scales viewport x/width (gfx_adjust_x_for_aspect_ratio / the 4:3 -> 5:3
  mapping) and whether the port's own x scaling is applied twice or not at all).
- LUS interpreter (libultraship/src/fast/interpreter.cpp): GfxSpMovemem viewport ->
  gfx_calc_and_set_viewport (or its Fast:: equivalent) — how the N64 viewport (vscale/vtrans
  in 10.2 fixed, 320x240 space) becomes rapi->SetViewport, and how the vertex pipeline
  ("pre-transform to clip space with the viewport baked in") treats a sub-viewport: if the
  vertices are already mapped into the sub-rect in window space, the backend viewport must be
  the FULL window (else the sub-rect is applied twice = ships magnified and shifted); if the
  vertices are NDC of the sub-viewport, the backend viewport must be the sub-rect. One of the
  two is being double-applied or dropped. Also the port's `[disp-raw]` telemetry
  (grep disp-raw) already logs viewport rects: turn it on for the machine-select screen.
- Stereo (port/3ds/gfx/gdx3ds_stereo.cpp): the right-eye shear and the classification of
  "scene" vs "UI" draws by w; sub-viewport scene draws may be classified/sheared wrongly too.

## Do
1. Reproduce in Azahar: navigate START -> mode select -> GP -> difficulty -> cup -> course ->
   machine select (an autoinput script exists: see docs/research/bridgecache-progress.md M5
   for the grammar and the storm script path; or drive it by hand-timed autoinput). Window-
   capture the machine-select screen and the ship-detail page (press A once more). Compare
   with the references. Log `[disp-raw]` viewport/scissor rects for those frames.
2. Bisect cheaply first: border_mode=0 (AUTHENTIC) vs 1 (FULL-BLEED) in the ini, and
   stereo off vs on, to see which factor moves the ships.
3. Find the double-applied / dropped viewport step and fix it so sub-viewports match the
   reference (grid of 30 in 6x5 cells; detail ship bottom-right, small) in BOTH border modes,
   with the race unchanged (window-capture a race frame and the HUD too). Killswitch
   `[debug] vpfix` (default 1; 0 = today's path) so hardware can A/B. Add a receipt
   `[vp] sub=<count of sub-viewport SetViewport calls per window> full=<count>`.
4. Zero error lines, heap flat, clean-stack roundtrip if LUS is touched (new pure-delta
   patch + README line), .3dsx + .cia. Commit per milestone with docs/research/viewport-
   progress.md. Do not end your turn while a run is in flight. Report under 30 lines with
   before/after captures listed and what to watch on hardware.
