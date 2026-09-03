# Morning status — 2026-08-21 (night session 2: the perf sprint)

## TL;DR
`feat/3ds-m1` @ `3d2c1db` — smoke-tested green (boot→race, HUD perfect, 0 fatals). Everything below
is MERGED and MEASURED. Launch: `open -a /Applications/Azahar.app --args
~/code/gdx-3ds/m1/build-3ds/port/3ds/G-Diffuser-3DS.3dsx` (manual play; autoinput parked).

## Performance (the sprint's numbers, Azahar proxies, deterministic windows)
| Lever | Measured effect |
|---|---|
| Async frame-mirror copy (was a hard CPU stall/frame) | menu fps 15.0 → 19.9 |
| S7 interpreter opts (raw dispatch, diag gate, tri memo) | menu-nav 13.8 → 19.8 fps; race build 26–58 → ~24–46ms |
| Skip pre-frame vblank stall when frame already late | wVbl 10–16ms → 0; title/menu wall 34 → 21–27ms (37–48 fps windows) |
| Stable resource keys + SETTIMG memo + upload-thrash fix | texture uploads ~155/frame → ~0.5; full resource resolutions ~93/frame → ~0 |
| Select-screen gradient coalesce | 224 fill-rects/frame → ~8 (pixel-identical) |
- Net: menus went from ~15fps feel toward ~30+fps-class wall times; races CPU-bound at ~24–46ms build.
- The S7 "regression" earlier was my bisect error (the bad dlcache rode an early merge); corrected.
- Escape hatches: `[debug] force_syncdraw=1` (re-enable the vblank stall), `skyfill=0` (sky fill off).

## Ship yellow-body: ROOT CAUSE FOUND + FIXED (verify with your eyes)
Not textures at all (every texture layer CRC-verified clean over 4 forensic rounds). It's the same
bug class as the original fog fix: `gDPSetEnvColor` (the per-machine body TINT — the body texture is
colorless I4) doesn't flush pending tris, and the 3DS backend binds ONE constant per batch — so
batches spanning machines painted every body in the FIRST machine's color. Race draws ~6 machines
through one combiner (select screen draws one — why it always looked right there).
Fixed: value-change flush in both set handlers + value-dirty constant application (net cheaper).
`[prim]` receipts confirm each body draw now binds its own machine's color.
**MORNING TEST: deliberately pick Blue Falcon and check the body is blue in-race.** (My scripted runs
mash A through the select grid and may legitimately pick a yellow-striped custom machine — I could
not confirm the visual, only the mechanism.)

## Still open
- **Sky wedge** — `feat/3ds-skyfix3` rebased + kill-switch, NOT merged (needs your display verdict;
  my capture path cannot judge it). If you want to try: it's built at
  `~/code/gdx-3ds/sky3/build-3ds/...`; `[debug] skyfill=0` disables live.
- **Shadow shape / building pop-in** — parked; re-check visually on this build first (the command
  stream changed a lot; they may have moved).
- **Boost tint** — needs a boost in-race to observe.
- Cadence agent PROVED menus are native-60 (not 30) — no game-side ceiling; remaining fps work is
  pure CPU build reduction (S7 continuation) + eventual real hardware.

## Tooling gained tonight
`[cadence]` (task/hold/game ratios), `[prim]` (constant binding receipts), `[interleave]`
(TMEM load→consume sequences), `[gpu] md=/imp=/rl=/rm=` (per-screen profiling), quiet-mode
(per-frame log tax off by default). All INI-gated, all documented in the patches README.
