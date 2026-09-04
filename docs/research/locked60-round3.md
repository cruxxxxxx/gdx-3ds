# LOCKED-60 ROUND 3 — three parallel tasks (2026-09-04)

Common rules: docs/research/locked60-campaign.md in each worktree (patch-as-pure-delta over the
full current stack; killswitch; receipts; Azahar lock protocol — up to 4 agents share it, wait
in-turn, never kill; merge ini keys; progress file per milestone; combined race screenshot via
`swift /tmp/wid.swift` -> `screencapture -x -l <id>` before claiming parity; report format).
Mainline `feat/3ds-hwaudio` @ ac2fed2 = all campaign levers + render thread mode 1 + boot
audio v2 + console filter. Hardware crowd frame today (trace on, stereo on): logic ~7-9 ms on
core 0, render ~12-15 ms on core 2 (`dsp 4.9 tri 4.2 br 3.1 drw 1.2 vtx 1.0 imp 0.7`), main
waits 10-15 ms/frame for the renderer (`[rt] waitMain`). Goal: raise the fps FLOOR (p10) in
30-machine crowd frames toward a locked 60.

## Task I — balance the cores (branch feat/3ds-balance, worktree ~/code/gdx-3ds/balance, based on feat/3ds-renderthread)
Agent H is concurrently building `renderthread=2` (one-frame-ahead: deferred DP-done ack,
2-deep backpressure) on feat/3ds-renderthread. After that lands, main has slack (logic ~8 vs
render ~12-15). Your lever: run the bridge pre-pass (ConvertRoot/ProcessList, ~3 ms) on MAIN
before the task is handed to the render thread, so core 2 only walks already-converted
commands. Read docs/research/renderthread-audit.md + renderthread-progress.md and
port/3ds/gdx3ds_renderthread.cpp first: the handoff is osSpTaskStartGo (port/n64_sched.c) ->
render thread runs gdx_gfx_run (bridge + interpreter + draws). Design: split gdx_gfx_run so the
bridge stage (adapter construction, ConvertRoot, the per-task memos/segment snapshot) executes
on the submitting thread and produces the converted root + the per-task state the interpreter
needs; the render thread then runs interpreter+draws only. Hazards: the bridge's per-task
statics/thread-local views (gSegments view, ListFacts memo, resolve memos — all assume one
thread per task; make the ownership explicit), the ConvertedList recycle pool (single-thread),
gDmaDirtyRanges lock, the [prof] BR bracket (now on main — keep the receipt honest: report br
on main separately), and the audio notify/menu tick on main (they must not starve: the bridge
adds ~3 ms to main's frame). Killswitch `[debug] bridgemain` (default 1 on the branch).
Receipts: `[rt]` gains `brMain=ms`. Verify: Azahar parity + zero errors + `[rt]` ratio
(waitMain shrinks by ~br); rebase onto feat/3ds-renderthread when H's M6 lands (coordinate:
poll `git -C ~/code/gdx-3ds/renderthread log --oneline -1` occasionally; if M6 is not
there when you are done, deliver on top of mode 1 and document the rebase). .3dsx + .cia.

## Task J — dynamic rival detail (branch feat/3ds-dynlod, worktree ~/code/gdx-3ds/dynlod)
RIVAL DETAIL (menu [perf] rival_detail 0/1/2 = FULL/REDUCED/MINIMAL) biases the game's native
LOD tiers for non-player machines beyond the 5 nearest (decomp patch
port/3ds/patches/decomp-port-rival-detail.patch, port-side level in port/3ds/gdx3ds_menu.c
`gdx_rival_detail_level`). Hand-set MINIMAL is worth +8-9 fps in crowds on hardware. Make it
AUTOMATIC: a port-side controller that measures the render time per frame (the render thread's
per-task wall, or `[gpu] build` from port/3ds/gfx/gdx3ds_gpu_prof.c — pick the cheapest signal
that exists without gputrace) and raises the effective rival-detail tier when the frame exceeds
a budget (start: raise one tier when the last N=4 frames average > 15.0 ms, lower one tier
when < 12.0 ms for 30 frames; hysteresis; never above the user's ceiling setting; the user's
manual setting becomes the FLOOR tier, a new [perf] rival_detail_auto=1 default on, menu row
"AUTO LOD" on the STAT or DISP tab following the existing patterns). The game reads the level
per frame (Racer_Draw) so no decomp change should be needed — verify; if a decomp change is
unavoidable, a new pure-delta patch. Receipt `[dynlod] tier=.. raises=.. lowers=.. ms=..`
on the race-dl cadence. Killswitch `[perf] rival_detail_auto=0`. Verify in Azahar with the
30-machine race: tier transitions logged, no visible popping on the 5 nearest machines
(screenshot), zero errors; then .3dsx + .cia. Report under 25 lines with what to watch on HW
(tier timeline vs `[gpu] build`).

## Task E — machine texture atlas (branch feat/3ds-atlas, worktree ~/code/gdx-3ds/atlas)
The remaining crowd batch breaks are per-part TEXTURE switches on machines (Task F's `[dsp2]`
flush map with prim/env folded: tex=82/frame; Task A's atlas handles HUD texrects only).
Build on Task A's machinery in port/3ds/gfx/gfx_citro3d.cpp (atlas pages, views, UV affine,
LRU eviction, `[c3d] atlasEv`) and `lus-trectbatch-atlas.patch` (the interpreter side: view
arming around ImportTexture, hasOff UV offset in BOTH packed loops — legacy and trifast).
Do first (census, commit): for each crowd frame, the machine-part textures: count, sizes,
formats, tile cms/cmt (clamp vs wrap vs mirror), mask bits, and the UV range the vertices use
vs the tile window (a wrapped tile whose UVs stay inside [0,1] is atlas-safe with the 1-texel
gutter; anything that really wraps/mirrors is not). Emit `[atlas3d] parts=.. clampSafe=..
wrapInside=.. unsafe=.. switches=..` and, from the census, the projected draw reduction.
Go/no-go at the brief's 0.5 ms rule (each avoided switch ~= 20 us on HW plus the import
lookup). Then: arm the atlas for eligible TRIANGLE draws (not just rects) — eligibility judged
per tile at import, the view's UV affine applied in the packed loops (already there), batches
kept open across views on the same page (same as rects), and the `vt=` receipt (triangles
sampling a view) becomes expected rather than a bug flag. Keep wrap/mirror tiles standalone.
Killswitch `[debug] atlas3d` (default 1 on the branch). Verify: Azahar A/B by vtxN bucket
(draws/frame, drw, imp, dsp), race screenshots with liveries inspected closely (the Falcon's
stripe decal bug history: clamp semantics must be exact), zero errors, heap/linear flat,
clean-stack roundtrip, .3dsx + .cia. Report under 40 lines.
