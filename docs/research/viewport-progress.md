# Task V — sub-viewport 3D placement (machine select / ship detail) — DONE in emulator

Cause: F-Zero X positions menu 3D by a FULL-SIZE N64 viewport translated onto each grid cell
(ovl_i4/machine.c: vscale = 160/120, vtrans = 57+40*col, 57+34*row), so the backend received
viewports with negative origins / extents beyond the target. GfxRenderingAPIC3D::SetViewport
clamped a negative fb-y to 0 and cast a negative y to u32, mangling exactly those rects
(ships ~1.6x, shifted). Telemetry (`[vp]`, verbose gate, after 45 s of uptime) confirmed the
rects: x = -128 + 50*col, y = 63 - 34*row, 400x240.

Fix (`[debug] vpfix`, default 1; 0 = old path; 2/3 = diagnostics: offsets zeroed / x=-0.5 probe):
- Out-of-range viewport -> GPU viewport stays full-screen; the rect is folded into the
  projection as an NDC scale+offset (M'[i].x = M[i].x*sx, M'[i].y = M[i].y*sy,
  M'[i].w += M[i].x*tx + M[i].y*ty), applied per draw on the stack copy of the base matrix.
- The x offset takes the interpreter's hor+ compression (gdx_get_widescreen_geometry_xscale,
  0.8 here) because the clip x of everything is already compressed about the screen centre.
- Scissor is intersected with the visible part of the rect (N64 clips to the viewport).
- Folded-viewport draws use the border mode's UI treatment (never the scene magnification),
  since they are menu 3D anchored to 2D artwork.
Verified in Azahar against the user's real-game references: machine select 6x5 grid inside the
panel with the starburst on the selected ship (authentic + full-bleed), ship detail model
bottom-right clear of the character art. Race unaffected (full-window viewports never fold).
Captures: /tmp/vp-art/fix4-machsel-b0.png, fix5-machsel-b1.png, fix5-detail-b0.png.
