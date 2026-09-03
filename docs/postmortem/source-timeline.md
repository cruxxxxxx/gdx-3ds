# G-Diffuser 3DS port — dated timeline from repository evidence

**Scope.** F-Zero X N64 decomp (G-Diffuser, libultraship runtime) ported to New Nintendo 3DS. Evidence = git history of `~/Code/G-Diffuser` (all branches; worktrees under `~/code/gdx-3ds/*`) plus the research docs in the most complete worktree, `bridgecache/` (branch `feat/3ds-hwtest4`, the integration tip at `35df5e2`, 2026-09-03 13:26). Compiled 2026-09-03.

**Conventions.**
- Dates are git author/committer dates (local, -0400) unless marked "(doc date)". **Doc-internal dates run ahead of git in September:** `RESUME-2026-09-04.md` is titled "2026-09-03 evening", `RESUME-2026-09-05.md` "2026-09-04 late", `locked60-round3.md` says "2026-09-04" but was committed 2026-09-02 (`57acdab`), `renderthread-progress.md` M7 says "2026-09-05" but was committed 2026-09-03 (`e45410f`). Git dates are treated as authoritative.
- **emu** = Azahar emulator (proxy; the docs repeatedly say it is not a performance oracle). **HW** = the user's real New 3DS. Numbers are quoted with their metric name and source; nothing is extrapolated.
- Branch `main` (`447ee52`, 2026-08-10) is the desktop project; **no 3DS code is on main**. Everything lives on `feat/3ds-*` branches. `docs/research/` on main is an untracked copy of the two founding docs (mtime 2026-08-10 23:37 / 23:53).
- Mainline of the port = `feat/3ds-hwaudio` (tip `eeb3b56`, 2026-09-03 09:35). Staging/HW-verdict branches = `feat/3ds-hwtest`, `-hwtest2`, `-hwtest3`, `-hwtest4`.

---

## 1. Phases (dates, branches, key commits)

### Phase 0 — Research dossier and plan (2026-08-10 → 08-11)
- `docs/research/3ds-port-research.md` (doc date 2026-08-10): 21 sources, 105 claims, 25 adversarially verified (22 confirmed / 3 refuted). Verdict: native port feasible; cannot reuse LUS renderer/platform stack; hand-port a citro3d backend in the sm64-3ds lineage; New3DS primary. Only perf figure: segment-8 `course_track_gfx` "133.95 ms" single-hit PC decode.
- `docs/research/3ds-port-plan.md`: Phase 0 foundation → six parallel Phase-1 streams (A gfx, B os, C audio, D assets, E 32-bit, F audit) → integration gates M1–M5 (M5 = real-hardware pass).
- **First 3DS commits, 2026-08-11:** `dff2669` "docs: add 3DS port research dossier and multi-agent work plan"; `3e76d46` "feat(3ds): Phase 0 skeleton — contract headers, stream directories, stub build".

### Phase 1 — Foundation / toolchain streams (2026-08-12)
- `b15d2ed` carve spike **PASS** (`spike-lus-carve-report.md`: all 36 carved libultraship sources compile under devkitARM GCC 16.1.0 with a 13-line newlib patch → subclass `Fast::GfxRenderingAPI`, no hand-written interpreter fallback). `a02cbe6` carve stub headers + LUS newlib patch.
- Stream branches (all 2026-08-12): `feat/3ds-gfx` (`0cba20d` citro3d backend, `bf68be1` LUS carve build, `6f610f1` vendored headers), `feat/3ds-os` (`5ec4aab` libctru backend, thread-based fibers, INI config), `feat/3ds-audio` (`fc0121f` ndsp backend), `feat/3ds-assets` (`a8fb048` gdx3ds_fs over vendored miniz 3.0.2, `48efae6` zipshim), `feat/3ds-32bit` (`277b2e5` 32-bit sweep: 13 fixes), `feat/3ds-foundation` (`536957b` stream-F census).
- Stream F docs (doc date 2026-08-12): `combiner-census.md` (every statically-seen combiner ≤3 PICA TexEnv stages), `3ds-memory-budget.md` (fits New3DS 124 MB with ~40–50 MB headroom if the LUS cache is capped at 24 MiB; old3DS 64 MB "DOES NOT FIT" → recommendation to drop old3DS).
- Integration branch `feat/3ds` merged E→B→A→D→C on 2026-08-12: `6a9c928`, `a61bc92`, `927e770`, `18b2abe`, `da5acea` (tip; later stale).

### Phase 2 — M1: full link, first boot, first race in emulator (2026-08-13)
Branch `feat/3ds-m1` (worktree `m1/`). All on 2026-08-13:
- `f7c5ed7` compile and LINK the full game (`m1-link-status.md`: "FULL LINK ACHIEVED", zero unresolved symbols); `d876203` decomp ILP32 patch; `7a2308e` boot-trace heartbeat.
- Boot-debug root causes fixed (war log `m1-boot-debug.md`): `396acb6` negative-size bzero over leo BSS stubs; phantom 64DD NULL deref; `bcef4e4` `std::filesystem::absolute` mangling `sdmc:` device paths; `cfef59c` double buffer swap (black top screen) + LUS SETTIMG low-address guard dropping all textures; `f9ad29e` first pixels; `594b400` race-entry freeze via DMA misroute + per-frame svc/vblank overhead; `ca398fb`/`785d976` race "freeze" was an uncaught `bad_alloc` from `EnqueueList`'s worst-case reserve (heap 41→86 MB in one frame; fixed, title 44.7→37–39 MB); `be049fa` LUS resource cache capped at 24 MiB; `b8042fa` route audio to HLE (LLE CVar defaulted on → "RSP LLE crash ~60 s into race"); `ecb5ed5` texcache content-hash span (T-TEXCACHE: cumulative misses 4800→978, −80%; title-static 19.9→29.5 fps emu; menus 14.6–15.0→19.0–19.9 fps emu); `7c1c048` fog LUT depth direction + readback un-rotation; `738089b` synthetic DL harness (36/36 pixel checks).
- Emu state per `RESUME.md`: "~15-20fps menus / ~10fps race in Azahar with debug logging".

### Phase 3 — Bridge, instrumentation, first research round, first pause (2026-08-14 → 08-15)
- 2026-08-14: `e3d55f6` B-BRIDGE (gdl-bad storm closed; "440-vs-3754 tris" = race vs SELECT COURSE screen); `d37a8f3` gpuprof telemetry; `5c1f2eb` stereo foundation (dual targets, per-eye loop, flag-off); `4ef7ac5` `stereo3d-research.md`; `adeddac` `60fps-research.md`; `1861627` `60fps-campaign-plan.md` (S0–S12); `1665219` `patch-consolidation-plan.md` (9 patches); `bb80258` real-hardware runbook `docs/3DS-HARDWARE.md` + `7bcd0dd` SMDH/CIA packaging; `37d1884` SD file-log sink; `7ba1a4b` user/dev guide; `c1bac07` fog exact-params (unverified); `f8d0fd3` "RESUME state — paused at fog blocker".
- 2026-08-15: `2024af3` S12 asset-cost shift; `53b3ca0` `renderer-architecture-research.md`.
- **Pause 2026-08-14 → ~08-19** (`RESUME.md`: "paused 2026-08-14, Fable credits exhausted; resume ~2026-08-19"; in-flight agents C4/F2/P2 "all died at credit/session/API limit").

### Phase 4 — Fog blocker solved, visual-bug campaign, measurement verdict (2026-08-19 → 08-20)
- 2026-08-19 (doc date, `RESUME.md`): batch-split fix `45d3294` **DISPROVEN**; fog root cause = PICA depth-LUT fog with the whole track in a ~0.01-wide clip-depth band.
- 2026-08-20: `c695e31` (`feat/3ds-fog`) per-vertex fog blend — **blocker solved**; `5758075` consolidation merge (fog + gpuprof + stereo + perf + cull); `2ddcbf4` `measurement-2026-08-20.md`: race **CPU-bound** — GPU 0.4 ms flat vs CPU build 26–58 ms (emu), `wP3D` 0.0 ms every frame; S11 fill-rate MOOT, S12 struck. `dd596f5` HLE audio producer → core 2 (S4). `885a1b8` night branch promoted (shadow decal bias 0.004, HUD strips, boost fix). `0dfe571` `reference-diff-2026-08-20.md` (scanout oracle off-by-one; RGBA16 word-swap premise false). `21f231a` sky verticalRange overscan **dead end**. `8dd344f` (22:47) **HUD garble root cause**: bridge dropped every o2r-filepath SETTIMG on ILP32 (`[race-dl] skip 13→0`, "user-confirmed live"). Diagnostic branches: `-sky`, `-skyfix/2/3`, `-roof`, `-cull`, `-shadow`, `-shadowgeo`, `-lod`, `-kmh`, `-texfix`, `-hud`, `-rgba16-decode/2` (invalidated), `-tilebind`, `-shiptex`, `-texdump`, `-oraclefix`, `-bldfix`, `-cidocs` (`7d74244` ci-3ds.sh, 11 patches).

### Phase 5 — Perf sprint, first hardware run, audio on hardware (2026-08-21)
- Night sprint (`morning-status-2026-08-21.md`, all emu): `03a3482` SETTIMG resolution memo (rl 93→0/frame) + gradient coalesce (224→8 rects); `d8f3e7c` S7 interpreter opts (menu-nav 13.8→19.8 fps; race build 26–58→~24–46 ms); `3b6c21a` skip pre-frame vblank stall (wVbl 10–16→0 ms; title wall 34→21–27 ms); `e60d84c` async frame-mirror copy (menu 15→19.9 fps); `3d2c1db` prim/env flush (rival-colored bodies); `f5371cf` flat dispatch; `63e2287` **LOADBLOCK**: "F3 22.34 → 2.21 ms, dsp 27.6 → 8.9 ms, menu wall 35-38 → 16.7-19.9 ms" (commit body); `a79b96d` texrect run memo.
- `f1f0412` (10:43) hardware packaging (SMDH, CIA target, runbook) → **first run on the user's New 3DS** (`RESUME-2026-08-22.md`: "RUNS ON REAL NEW3DS — video + audio + input + perf all working"). `a4c66ea` bottom-screen FPS HUD; `2d36485` input remap.
- Audio: `061b0d1` audiofix (440 Hz test tone, null-sink unmask); `2852eaf` **rom-side audio driver ported to host — root cause of total silence** (patch `decomp-3ds-rom-audio-port.patch`); `28c02f2` HW silence bisect (LINEAR ndsp interp; POLYPHASE silent on real DSP); reverts `6ccb98f`, `94169b6`, reapply `f8a260c`; `bf8e747` `audio-regression-forensics.md` ("There was no regression… m1 never had an audible emulator build"). Branch `feat/3ds-hwaudio` becomes mainline (`c24500f`).
- `f1247a9` ship livery clamp fix; `db38c47` race-start hitch (gfx blob preload); `854b9d6` (21:37) resume checkpoint.
- **Pause 2026-08-21 → 08-27** (`RESUME-2026-08-22.md`: "Fable-5 tokens out").

### Phase 6 — Fleet batch, touch menu, lifecycle, release-grade (2026-08-27 → 08-28)
- 2026-08-27 03:22 fleet merges into `feat/3ds-hwaudio`: `feat/3ds-traffic` (`c7e3726` PIPESYNC no-op, `f42f26a` tri-memo/clip masks, `e96fa91` bridge region cache: br 19.7→10.4 ms, wall 55.7→46.7 ms emu), `-skywedge3` (`cad09fb` clamp UVs to content edge — sky wedge solved), `-ccmux` (`b7b73d6` ccmux 11), `-stereo2` (`433c8c4` exact off-axis stereo projection in clip space), `-shadow2` (mirror-period audit).
- 2026-08-27 14:06 `7a7001e`: `-leak` (`d58241d` uninitialized `ColorCombinerKey::shader_id` → heap climb; upstream candidate) + `-transition` (`c60ae4b` synchronous readbacks). 15:02 `b1cca12` bottom-screen touch menu v1; `5c973d2` full-bleed default (`1749cd8` border modes via projection scaling). 17:23 `dbf82c3`: `-timetrial` (`4e7c241` TIME TRIAL post-GP freeze) + `-crowd2` (`d3a9820` tile-state value gate, −1.2 ms crowd emu).
- 2026-08-28 00:23 `812ba4c` "merge: perftest — RIVAL DETAIL option (+8-9fps HW crowds) + triloop packed VBO" (`0bb27dd`, `21f46bf`); 00:49 `5f0bb25` canonical APT servicing (POWER/HOME/sleep). `feat/3ds-gputransform` (`0e1068f`, `1bc25a6`) — GPU vertex transform, **shelved** (see levers). `180d8b8`/`e90500e` rival-detail patch regenerated as pure delta (double-carry bug, twice). `a9eb47f` `README-3DS.md`; `c586871` (09:41) "resume checkpoint — release-grade; bridge cache is the final lever".
- HW state (`RESUME-2026-09-02.md`): "Median ~51 fps, p95 60, max 60 — TOUCHES AND HOLDS 60. Crowds w/ RIVAL DETAIL: MINIMAL floor ~40 (was 25-35)… Zero known crashes."
- **Pause 2026-08-28 → 09-01** ("next Fable-5 credits ~Sep 2").

### Phase 7 — Bridge cache NO-GO → brfast; HOME crash (2026-09-01)
- `102d38a` bridge-cache brief; `9795fbb` census; `052bf1b` **M1 census verdict NO-GO** (static lists = 12.8 % of crowd commands / 16.1 % whole run; cache ceiling ~1.5 ms); pivot to per-list memos `[debug] brfast`: `ed3e4cf`, `b918202` fastA (−29 %), `88c8ac3`/`23d0b1c` M3 (br 11.49→5.04 ms, crowd mean 8.84→4.21 ms, −52 % emu), `ac8e284` M4, `33dc583`/`013deb6` M5 storm (3 courses, zero anomalies).
- `aadfac3` `render-architecture-alternatives.md` (99 agents; no shipped N64 port retains translated lists).
- `177ac5d` `home-crash-audit.md`: HOME press crashed instantly on HW (.3dsx under Luma hbldr); `e23a7f5` park audio threads across HOME/sleep; then close-from-HOME hang → `5190c1d` (2026-09-02).
- Staging `feat/3ds-hwtest`: `f475cd2`, `5f6e97d`, `70d0e25` (home + bridgecache).

### Phase 8 — LOCKED-60 campaign, round 1 (Tasks A–D) (2026-09-02, morning → afternoon)
- 10:34 `a03894f` + `2a84e4f`: brfast + HOME fix merged to mainline after HW verdict; `2344e88` filelog cap 256 KB→1 MB; `256e397` resume checkpoint.
- `7fe13ee` `locked60-campaign.md` (HW ground-truth profile; GPU vertex transform "shelved for good").
- Task C `feat/3ds-tri2`: `95643a8` census, `e15625a` **trifast** (s7 memo had never hit: `textures_changed` flag bug), `8697bdc` final A/B (crowd tri −17 %, menu −19 % emu). Task B `feat/3ds-tmem2`: `d3e9201` **tmemfast** (F3 −22 % crowd, −35 % menu emu). Task A `feat/3ds-texrect2`: `4790b2c` census, `3a9f952` **trectbatch** HUD atlas, `de6f1a9` 512×256 pages, `f1be661` LRU eviction (menu E4 −2.47 ms, draws 136→24.5 emu). Task D `feat/3ds-bootaudio`: `399e367` **audioprime** (emu under 3/3→0/3).
- Staging `feat/3ds-hwtest2` `d646789` (16:28); after HW verdict `d980f78` (17:58) merge; `c77531d` `locked60-research-2026-09-03.md` (100 agents: cores, dispatch, batching, stereo, 30 Hz).

### Phase 9 — Round 2 (Tasks F, G, H): render thread on core 2 (2026-09-02, evening)
- Task H `feat/3ds-renderthread`: `8467abe` M1 audit (`renderthread-audit.md`); `7502fac` M2 sync mode on core 2; `b3cffc8` M3 fork/join pipe mode; `d2622c5`/`da6621e` fence fixes; `7c8363a` M6 **ahead mode** (`renderthread=2`); `5e1672a` M6 emu results. Task F `feat/3ds-dspcensus`: `8cbde0c` census, `a04e642` dspfast, `0ab17c6` **default OFF** ("cannot pay"). Task G `feat/3ds-bootaudio2`: `6d28904` audioprime2, `6be88ae` preload-core change reverted, `d4da0a2` **audio_table stored uncompressed in fzerox.o2r** (root fix).
- Staging `feat/3ds-hwtest3`: `4f3736c`/`b018d22` (19:18) renderthread + bootaudio2; `531bbc9` (20:18) console filter device. After HW verdict: `ac2fed2` (21:21) mainline "resume after round 3".

### Phase 10 — Round 3 (Tasks I, J, E) and hardware round 5 (2026-09-02 late → 09-03)
- Task I `feat/3ds-balance`: `2c83a2c` **bridgemain** (bridge pre-pass on the submitting thread; render-side br 4.0→0 emu). Task J `feat/3ds-dynlod`: `016aefb` **rival_detail_auto** (raise tier >15 ms, lower <12 ms, hysteresis). Task E `feat/3ds-atlas`: `c912b28` census → `d354207` **default OFF / NO-GO** (8 clamp-safe switches/frame ≈ 0.08–0.16 ms HW).
- Staging `feat/3ds-hwtest4`: `cbf29bb` (21:49) renderthread M6, `0db3e91` balance, `978f31d` (23:37) dynlod = **Round 5 build**.
- 2026-09-03: Round-5 crash (core-2 data abort in `TextureCacheLookup` LRU splice while main cleared the cache at podium/venue load) → `e4ff12d` WIP checkpoint, `b91e229` **M7 texture cache render-thread-owned** (`lus-renderthread-texcache-owner.patch`, patch #48), `e45410f` notes, merged `d96f27a` (10:21). `eeb3b56` (09:35) `locked-sixty-report.html` on mainline.

### Phase 11 — Viewport fix and stereo anchor (2026-09-03, pending HW)
- `a013df4` viewport brief; `87dc3e7`, `99c9619` **vpfix** (`feat/3ds-viewport`): machine-select / ship-detail 3D drawn ~1.6× too large and offset because F-Zero X translates a full-size N64 viewport per grid cell (x = −128 + 50·col, y = 63 − 34·row, 400×240) and `SetViewport` clamped/cast negative origins; out-of-range viewports now folded into the projection. Merged `86f23ee`/`51cecac` (11:36).
- `7dc546a` **stereo anchor** (`feat/3ds-stereo-anchor`): race position markers (ortho prim-depth rects) anchored at their ship's depth (emu: marker eye shift 26.3 px vs far machine 20.7 px vs HUD 0.5 px; receipt `anchor=38/0.99/0.99`). Merged `35df5e2` (13:26) — tip. No HW verdict recorded for Phase 11.

---

## 2. Hardware rounds

Sources: `locked-sixty-report.html` (DATA block: per-session fps beats, median, p10, % beats at cap; three traced CPU profiles), `RESUME-2026-*.md`, `renderthread-progress.md`, commit messages. "Beat" = watchdog 5-second window (frames/5). Report note: "Stereo 3D was off in the first two rounds and on afterwards, which doubles draw calls."

| Date (git) | Build (branch@commit) | What it carried | Verdict / measurement |
|---|---|---|---|
| 2026-08-21 | `feat/3ds-m1` @ `f1f0412` → `feat/3ds-hwaudio` @ `db38c47` | SMDH/CIA packaging, runbook, FPS HUD, input remap, rom-audio port, livery clamp, race-start hitch | **First run on real New 3DS.** `RESUME-2026-08-22.md`: "FPS: median 48-50, p95 58, max 59, traffic dips 25-35"; worst traffic window "wall 50ms=20fps, GPU idle 3ms — CPU bound"; `[prof!] br=13.5-14.1 dsp=8.3-8.5 tri=6.7-7.0`. Audio audible on HW with LINEAR interp (`60f0301` "AUDIBLE final state"). |
| 2026-08-27/28 | `feat/3ds-hwaudio` @ `de49909`…`812ba4c`…`5f0bb25` | fleet batch (traffic, sky wedge, ccmux, stereo projection), leak/transition fixes, touch menu, rival detail, triloop, APT fix | `812ba4c`: "RIVAL DETAIL option (+8-9fps HW crowds)". `RESUME-2026-09-02.md`: "Median ~51 fps, p95 60, max 60… Crowds w/ RIVAL DETAIL: MINIMAL floor ~40 (was 25-35)… clean POWER/HOME/sleep… Zero known crashes." `gputransform`: "HW-imperceptible". |
| 2026-09-01 | `.3dsx` under Luma hbldr (build not named) | — | **HOME press crashed instantly** (`home-crash-audit.md`); after `e23a7f5`: HOME→resume works, HOME→Close wedges (drain thread hot-spin). |
| Report **Round 1** (date not stated; "Mainline before the campaign", stereo off, trace off, 754 beats / 63 min) | `feat/3ds-hwaudio` (commit not named) | baseline | median **51.4** fps, p10 **39.2**, **9 %** of beats at 60. |
| Report **Round 2** (≤2026-09-02 10:34; stereo off, trace off, 78 beats / 6.5 min) | `feat/3ds-hwtest` @ `70d0e25` (home + bridgecache) | **brfast**, HOME/close fixes | median **57.2**, p10 **46.6**, cap **29 %**. Traced: `[prof]` crowd br **6.43→3.73 ms (−42 %)**, all-window mean 5.18→2.83; ledger "median 46 → 49" (trace on); "p10 ~42-48 → ~47-55" (`RESUME-2026-09-03.md`). HOME suspend/resume/close clean. Merged `a03894f`/`2a84e4f`. |
| Report **Round 3** (2026-09-02 ~16:30–17:58; stereo on, trace off, 237 beats / 20 min) | `feat/3ds-hwtest2` @ `d646789` | trectbatch, trifast, tmemfast, audioprime | median **56.0**, p10 **48.0**, cap **32 %**. Traced A/B (levers off→on): steady **16.4→14.9 ms** CPU/frame, crowd **20.2→18.8 ms** (report DATA; `RESUME-2026-09-04.md` says 18.7); crowd tri 5.58→4.64 ms, drw 1.48→1.24, draws/frame 67→55; "fps median 46 -> 49 (trace tax on), p10 39 -> 42". Boot audio: still `under=2` at ~10.7 s (`bootaudio2-progress.md`). Merged `d980f78`. |
| Report **Round 4** (2026-09-02 ~20:20–21:21; stereo on, trace off, 58 beats / 5 min) | `feat/3ds-hwtest3` @ `531bbc9` | **renderthread=1** (core 2, pipe), audioprime2 + uncompressed o2r, console filter | median **57.6**, p10 **44.6**, cap **14 %**. Ledger: "median 49 → 56, p10 42 → 48" (trace on). Traced render-side crowd total **15.1 ms** (dsp 4.89, tri 4.21, br 3.12), steady **12.0 ms**; but `waitMain=14-15 ms` — main serialized behind the render (→ M6). Boot preload **4.3→2.0 s**. Merged `ac2fed2`. |
| Report **Round 5** (2026-09-02 23:37 → 09-03; stereo on, trace off, full GP, 229 beats / 19 min) | `feat/3ds-hwtest4` @ `978f31d` | **renderthread=2** (ahead), **bridgemain**, **rival_detail_auto** | median **59.6**, p10 **51.4**, cap **53 %**. "Crash after a full GP in the menus": render-thread texture-cache LRU splice vs main-core cache clear → M7 fix `b91e229`. |
| (pending) | `feat/3ds-hwtest4` @ `35df5e2` (2026-09-03 13:26) | M7 texcache ownership, vpfix, stereo anchor | No HW verdict in the repository. |

Traced CPU profiles (HW, gputrace on, ms/frame; `locked-sixty-report.html` DATA.profs):

| Session | crowd total | br | dsp | vtx | tri | imp | drw | nD | steady total |
|---|---|---|---|---|---|---|---|---|---|
| Campaign levers off (control) | 20.2 | 2.96 | 8.26 | 1.12 | 5.58 | 0.78 | 1.48 | 67 | 16.4 |
| Campaign levers on | 18.8 | 3.07 | 7.84 | 1.18 | 4.64 | 0.78 | 1.24 | 55 | 14.9 |
| Render thread (pipe), render side | 15.1 | 3.12 | 4.89 | 1.01 | 4.21 | 0.68 | 1.16 | 55 | 12.0 |

Pre-campaign HW crowd profile (`locked60-campaign.md`): `[prof] br=3.2-3.8 dsp=6.5-7.3 vtx=0.7 tri=3.4-3.6 imp=1.2-1.8 drw=2.3-2.5`, `[gpu] wall=23.4 build=16.3 gpu=5.6`; top opcodes E4 texrect 3.9–8.9 ms/90–138 calls, 06 tri2 2.4–4.4/143–270, F3 loadblock 1.9–2.4/144–199.

---

## 3. Levers

Status: **mainline** = merged to `feat/3ds-hwaudio`; **staged** = in `feat/3ds-hwtest4` awaiting HW verdict; **off** = shipped default-OFF; **NO-GO** = census/A/B said it cannot pay; **shelved** = works but parked; **dead end** = approach disproven.

| Lever (key / patch) | Branch, date | Status | Emulator effect | Hardware effect |
|---|---|---|---|---|
| TMEM content-hash span (`lus-texcache-content-hash-span`) | `feat/3ds-texcache` `ecb5ed5`, 08-13 | mainline | cumulative misses 4800→978 (−80 %); title 19.9→29.5 fps; menus 14.6–15.0→19.0–19.9 fps | not separately measured |
| LUS resource cache cap 24 MiB (`lus-resource-cache-cap`) | `be049fa`, 08-13 | mainline | memory: heap plateau ~44–47 MB vs ~87 MB ceiling | — |
| Fog per-vertex blend (blocker fix) | `feat/3ds-fog` `c695e31`, 08-20 | mainline | correctness (road visible) | user-verified |
| HLE audio producer → core 2 (S4) | `dd596f5`, 08-20 | mainline | not measured | — |
| SETTIMG resolution memo + gradient coalesce (`lus-3ds-settimg-resolution-memo`, decomp gradient patch) | `feat/3ds-selperf` 08-21 | mainline | resolutions 93→0/frame; 224→8 rects | — |
| Async frame-mirror copy / quiet mode | `feat/3ds-menuperf` `e60d84c`, 08-21 | mainline | menu 15→19.9 fps | — |
| S7 interpreter opts (raw dispatch, geo-diag gate, tri-state memo) | `feat/3ds-s7-*` `d8f3e7c`, 08-21 | mainline | menu-nav 13.8→19.8 fps; race build 26–58→~24–46 ms | — |
| Skip pre-frame vblank stall when late | `feat/3ds-cadence` `3b6c21a`, 08-21 | mainline | wVbl 10–16→0 ms; title wall 34→21–27 ms | — |
| LOADBLOCK span store + same-content skip (`lus-tmem-span-store`, `lus-tmem-same-content-skip`, `lus-tmem-diag-race-latch`) | `feat/3ds-loadblock` `63e2287`, 08-21 | mainline | F3 22.34→2.21 ms; dsp 27.6→8.9 ms; menu wall 35–38→16.7–19.9 ms | user: "blazing fast" menus (commit body) |
| Flat dispatch table (`lus-flat-dispatch`) | `f5371cf`, 08-21 | mainline | "neutral but clean" (`morning-status-2026-08-21.md` lists it as merged; no delta quoted) | — |
| Texrect run memo + viewport hoist | `feat/3ds-texrect` `a79b96d`, 08-21 | mainline | not quoted | — |
| Bridge region cache + asset binary search (traffic) | `feat/3ds-traffic` `e96fa91`, 08-27 | mainline | br 19.7→10.4 ms; wall 55.7→46.7 ms (storm) | — |
| PIPESYNC no-op (`lus-traffic-pipesync-noop`) | `c7e3726`, 08-27 | mainline | E7 3.50 ms→0 | — |
| Tile-state value gate (`lus-crowd2-tilestate-value-gate`) | `feat/3ds-crowd2` `d3a9820`, 08-27 | mainline | crowd build 42.3→41.1 ms (−1.2 ms, −3 %); whole-run −0.6 % | — |
| Combiner-key leak fix (`lus-cc-key-uninit-shader-id`) | `feat/3ds-leak` `d58241d`, 08-27 | mainline (upstream candidate) | heap flat | — |
| RIVAL DETAIL manual (`decomp-port-rival-detail`) | `feat/3ds-rivallod` `0bb27dd`, 08-27 | mainline | — | "+8-9fps HW crowds" (`812ba4c`); MINIMAL floor ~40 fps (was 25–35) |
| TRILOOP packed VBO (`lus-3ds-triloop-packed-vbo`) | `feat/3ds-triloop` `21f46bf`, 08-27 | mainline | "NOT emulator-verified" (lock contention); reasoned ~0.5–1.2 ms drw + 0.3–0.8 ms tri | not measured |
| GPU vertex transform (`[debug] gputransform`) | `feat/3ds-gputransform` `0e1068f`, 08-28 | **shelved** | unlit course preview: vtx 0.77 ms (on) vs 2.01 ms (off) = −62 %, wall 53.8 vs 56.9 (−3.1 ms); in-race light vtx −26 %; lit grid-storm identical (8.45 vs 8.45); 220,989 GPU-transformed draws | "imperceptible"; venue-floor texture precision bug (PICA 24-bit uniforms on ±32000 coords). Campaign brief: "vtx is ~1 ms on hardware… shelved for good" |
| Bridge translation OUTPUT cache | `feat/3ds-bridgecache` `052bf1b`, 09-01 | **NO-GO** (not built) | static lists 12.8 % crowd / 16.1 % run → ceiling ~1.5 ms | — |
| brfast bridge memos (`[debug] brfast`) | `feat/3ds-bridgecache` `88c8ac3`, 09-01 | mainline (09-02) | br 11.5→4.9–5.0 ms crowd; mean 8.84→4.21 (−52 %) | br **6.43→3.73 ms (−42 %)**; median 46→49 (trace on); p10 ~42-48→~47-55 |
| trectbatch HUD atlas (`lus-trect-census`, `lus-trectbatch-atlas`) | `feat/3ds-texrect2`, 09-02 | mainline | menu E4 −2.47 ms, draws 136→24.5; crowd E4 −0.74, drw −1.68; race-start build 23.6→20.5 | draws/frame 67→55 (traced) |
| texrect identical-state run merging | `feat/3ds-texrect2` | **NO-GO** ("already the case") | ident=a/0.0 everywhere | — |
| trifast s7 memo fix + packed loop (`lus-tri2-phase-census`, `lus-trifast-tri-memo-pack`) | `feat/3ds-tri2` `e15625a`, 09-02 | mainline | crowd tri 8.54→7.07 ms (−17 %); menu −19 %; race steady −15 % | tri 5.58→4.64 ms crowd (traced) |
| tmemfast O(1) span replacement (`lus-tmem2-tmemfast`) | `feat/3ds-tmem2` `d3e9201`, 09-02 | mainline | F3 5.96→4.66 ms crowd (−22 %); menu 2.57→1.67 (−35 %); race F3 2.97→2.01 (−32 %) | "within dispatch bucket" (report) |
| audioprime boot ring priming (`[debug] audioprime`) | `feat/3ds-bootaudio` `399e367`, 09-02 | mainline | underruns 3/3→0/3 (stall-sim 18 ms) | still `under=2` at ~10.7 s (`rmin=320`) |
| audioprime2 + uncompressed audio_table in o2r | `feat/3ds-bootaudio2` `6d28904`/`d4da0a2`, 09-02 | mainline | under=0, boot rmin 3072; preload-core move **reverted** (counterproductive: 8272 ms preload, under=1) | preload **4.3→2.0 s**; "font stall gone" |
| dspfast prim/env colour folding (`[debug] dspfast`) | `feat/3ds-dspcensus` `a04e642`, 09-02 | **off** (cannot pay) | ≈0 (drw 0.70→0.67 steady; prim/env sole cause of only ~4–12 draws/frame) | — |
| Render thread on core 2, pipe (`renderthread=1`) | `feat/3ds-renderthread` `b3cffc8`, 09-02 | mainline | n/a (Azahar time-slices core 2: dsp 18.7 vs 16.4, wall 47.5 vs 41.8 ms with thread ON) | median 49→56, p10 42→48 (trace on); Round 4 median 57.6; waitMain 14–15 ms |
| Ahead mode (`renderthread=2`) | `7c8363a`, 09-02 | staged (Round 5) | waitMain == tk − ovl (ratio evidence); zero errors | Round 5 median 59.6, p10 51.4 (combined with bridgemain + dynlod) |
| bridgemain (`[debug] bridgemain`) | `feat/3ds-balance` `2c83a2c`, 09-02 | staged (Round 5) | render-side br 9.1→0.00; wall 76.0→66.6 crowd; +1.05 MB heap | "see Round 5" |
| rival_detail_auto (dynlod) | `feat/3ds-dynlod` `016aefb`, 09-02 | staged (Round 5) | tier 2 vs 0: build −5 ms/frame | "see Round 5" |
| atlas3d machine-texture atlas (`[debug] atlas3d`) | `feat/3ds-atlas` `d354207`, 09-02 | **off / NO-GO** | draws −7, ≈0 ms | projected 0.08–0.16 ms/frame |
| Texture cache render-thread-owned (`lus-renderthread-texcache-owner`) | `feat/3ds-balance` `b91e229`, 09-03 | staged | correctness (crash fix) | Round-5 crash fix; `texcacheMainMut=` must read 0 |
| vpfix (`[debug] vpfix`) | `feat/3ds-viewport` `99c9619`, 09-03 | staged | correctness (machine select 6×5 grid; detail ship bottom-right) | pending |
| stereo anchor (`[debug] stereo_anchor`) | `feat/3ds-stereo-anchor` `7dc546a`, 09-03 | staged | marker eye-shift 26.3 px vs machine 20.7 px | pending |
| 30 Hz logic + interpolation | — | not pursued ("last resort"; research: sm64 patch doubles interpreter work) | — | — |
| Double-height stereo target / right-eye batching | `crowd2-fresh-profile.md` recommendation, 08-27 | not built (research 09-03: right eye already reuses packed VBO in this port) | — | — |
| S11 fill-rate, S12 ETC1/decimation | `measurement-2026-08-20.md` | MOOT / struck (GPU idle: 0.4 ms) | — | — |
| Sky verticalRange overscan | `feat/3ds-skyfix*`, 08-20 | **dead end** (verts s16-clamped ±32000) — reverted, note patch `decomp-3ds-sky-overscan-deadend-note` | — | — |

---

## 4. Setbacks (dated)

1. **2026-08-13** — M1 boot chain of silent failures: black screen was a log-filter artifact; negative-size `bzero` wiped ~61 KB of `.bss`; NULL 64DD handle; `sdmc:` path mangling (errno 88); double buffer swap; SETTIMG low-address guard dropped all textures; DMA misroute froze race entry (`m1-boot-debug.md`, commits `396acb6`, `bcef4e4`, `cfef59c`, `594b400`).
2. **2026-08-13** — "race freeze" was an uncaught `std::bad_alloc` (heap 41→86 MB in one frame); the handed-down suspect (`gLoadedAssetSegments`) was wrong (`ca398fb`, `785d976`). RSP LLE "crash ~60 s into race" = LLE audio CVar defaulting on without its menu TU (`b8042fa`).
3. **2026-08-14** — **Pause**: "Fable credits exhausted"; three in-flight agents (C4 fog, F2 verify/merge, P2 perf) "all died at credit/session/API limit" (`RESUME.md`). Port paused at the invisible-road fog blocker (`f8d0fd3`).
4. **2026-08-19** — fog batch-split fix `45d3294` DISPROVEN; exact-fog-line fit "fixing the wrong layer" (`RESUME.md`).
5. **2026-08-20** — verification oracle invalid: `_scan.bmp` scanout capture is off-by-one / wrong buffer, "a fix that regressed the current frame could still verify clean"; RGBA16 odd-line word-swap premise false → `feat/3ds-rgba16-decode`/`-decode2` invalidated (`reference-diff-2026-08-20.md`). STALE-OBJECT TRAP "cost a whole verify run". Sky verticalRange overscan **dead end**, reverted (`night-verify-2026-08-20.md`). "8 decode-layer fixes failed" before the HUD root cause (`8dd344f` body: commands never reached the interpreter).
6. **2026-08-21** — false audio "regression" hunt (12:08→17:22): m1 had never been audible; a parallel stale agent ran `git revert 28c02f2` (rogue `6ccb98f`) and deleted sdmc logs mid-verdict; reverts `94169b6`, reapply `f8a260c` (`audio-regression-forensics.md`). S7 "regression" was a bisect error; the bad dlcache "rode an early merge" and was dropped (`morning-status-2026-08-21.md`); per-command validity cache "REGRESSED 2× in Azahar" (hard constraint in `bridgecache-brief.md`). **Pause** 2026-08-21 → 08-27 ("Fable-5 tokens out").
7. **2026-08-27** — TRILOOP could not be emulator-verified (Azahar singleton lock held all session); other agents' runs deleted crowd2's baseline SHOTs and a log mid-session (`crowd2-fresh-profile.md`); `[shadowgeo]` probes "WERE NEVER RUN" (dead spawn).
8. **2026-08-28** — GPU vertex transform **shelved**: HW-imperceptible + venue-floor texture precision bug (`gputransform-report.md`, `RESUME-2026-09-02.md`). Rival-detail decomp patch double-carried neighbouring hunks twice (`180d8b8`, `e90500e`). Bridge-cache agent "DIED on token limit with 0 commits" (`RESUME-2026-09-02.md`). **Pause** 2026-08-28 → 09-01.
9. **2026-09-01** — bridge translation output cache **NO-GO** (`052bf1b`). **HOME press crashed the game instantly** on HW (audio threads driving the DSP through `ndspFinalize`; `home-crash-audit.md`, `177ac5d`); after the park fix, **close-from-HOME hung** (drain thread hot-spinning on a shared sticky LightEvent starving the same-core producer) → `5190c1d` (09-02). Luma ARM11 dump recipe (`sdmc:/luma/dumps/arm11/*.dmp`, `luma_dump.py`) added.
10. **2026-09-02** — filelog 256 KB cap "truncated one HW session before the exit receipts" → 1 MB (`2344e88`). User's SD `gdiffuser.ini` overwritten once (lost `[stereo] enabled=1`). Texrect lever 1 NO-GO; dspfast "cannot pay" (default OFF); atlas3d NO-GO (default OFF); bootaudio2 preload-core move counterproductive, reverted (`6be88ae`); HW still `under=2` after round-1 audioprime. HUD "showed one portrait everywhere" — atlas × trifast interaction (levers verified singly) → `3fafbea`. Touch-menu tab bar scrolled away (stray console writes) → `531bbc9`. Render thread mode 1 did not overlap on HW (`waitMain=14-15 ms`) → M6. Atlas page cells never reclaimed (`atlas=…/22/8`) → LRU eviction `f1be661`.
11. **2026-09-03** — **Round-5 crash after a full GP in the menus**: core-2 data abort in `TextureCacheLookup` LRU splice while the game thread ran `gfx_texture_cache_clear()` (`renderthread-progress.md` M7) → `b91e229`. `e4ff12d` "WIP… checkpoint by orchestrator after API overloads" (agent interrupted). Doc dates drifted 1–2 days ahead of git (see Conventions).
12. **Open, deprioritized:** #27 tunnel roof (one course; cull and depth/clip both ruled out, `tunnel-roof-*.md`); boot-audio jitter cosmetic; gputrace/verbose raise underruns (README known issue).

Grep hits for the requested words are saved in `scratchpad/setbacks-grep.txt` (173 lines). Note: `docs/STATUS.md` "Race-entry crash (~1 in 13)" is the **desktop** project's status (as of 2026-07-31), not a 3DS setback. No repo doc contains the literal phrase "hard crash"; "529" appears only as a line-number cite (`gdx3ds_audio_ndsp.c:521-529`).

---

## 5. Code size (tip = `feat/3ds-hwtest4` @ `35df5e2`, worktree `bridgecache/`; `wc -l` on .c/.cpp/.h/.pica/.cmake)

| Measure | Lines |
|---|---|
| `port/3ds/` excluding `patches/` and `assets/third_party/` (vendored miniz) | **15,039** |
| `port/3ds/` including vendored miniz | 29,709 (miniz ≈ 9,255 in anchor worktree) |
| `port/3ds/` CMakeLists.txt (not .cmake) | 691 |
| by subdir: gfx 5,387 · top-level (main_3ds.cpp, menu, renderthread, fps hud, filelog) 3,482 · lus_glue 1,277 · audio 1,274 · harness 1,135 · os 969 · assets (excl. miniz) 887 · include 319 · lus_stubs 203 · game 106 | |
| Largest files: `gfx/gfx_citro3d.cpp` 3,380 · `audio/gdx3ds_audio_ndsp.c` 1,233 · `main_3ds.cpp` 1,221 · `gdx3ds_menu.c` 817 · `gfx/gdx3ds_gpu_prof.c` 619 · `harness/scenes.cpp` 616 · `gdx3ds_renderthread.cpp` 384 · `gfx/gdx3ds_stereo.cpp` 381 | (anchor worktree, 14,404 total there) |
| Total `port/` excluding third_party & patches | 86,227 (of which `port/` outside `port/3ds`: 71,188) |
| Total `port/` including third_party | 117,942 |
| `git diff --numstat main...HEAD` outside `port/3ds` and `docs` (bridge, sched, tools, CMake…) | +5,285 / −625 |
| **Patch stack** `port/3ds/patches/`: **48** `.patch` files (38 `lus-*` + 10 `decomp-*`) | **+4,685 / −547** lines (lus +3,423; decomp +1,262; largest: `lus-tmem-span-store` +242, `lus-3ds-triloop-packed-vbo` +296, `lus-trect-census` +362, `lus-tmem2-tmemfast` +281, `decomp-3ds-rom-audio-port` +707) |
| Patch count over time | 9 (2026-08-14 `1665219`) → 11 (08-20 `7d74244`) → 14 (08-20 night) → 21 (08-21 `a4629d6`) → 36 (08-27) → 42 (08-28/09-01 brief) → 47 (09-02 round-2 brief) → 48 (09-03) |
| Research docs | 52 `.md/.html` in `bridgecache/docs/research/` (696 KB) + `atlas-progress.md`, `dspcensus-progress.md`, `gputransform-report.md`, `tunnel-roof-depth-clip-analysis.md`, cidocs `README.md` in other worktrees; plus images (`livery2-fixed-drive*.png`, `skywedge3/*.bmp`, `gputransform-ab/*.png`, `stereo-anchor-t92-eyes.png`) |

Git counts: 98 `feat/3ds-*` branches (+ `feat/3ds` integration = 99; 100 local branches incl. `main`); 417 commits since 2026-08-11 (of 496 in `--all`); **250 commits touch `port/3ds/`**; 101 touch `port/3ds/patches/`; 150 touch `docs/research/`; 255 commit subjects mention "3ds". 96 git worktrees under `~/code/gdx-3ds/`. Submodules: `libultraship` (Zorkats fork) `7bca0e2`, `decomp` `f7fd0fd`, `fzerox-expansion-kit` `6cd71e6`, `torch` `c1bdc6f` — patches are applied to submodule working trees, never committed to the submodules ("patch-as-pure-delta").

Patch list (apply order per `port/3ds/patches/README.md`): lus-newlib-portability, lus-resource-cache-cap, lus-device-path-archives, lus-3ds-settimg-low-address, lus-3ds-hud-tall-atlas-extent, lus-texcache-content-hash-span, lus-3ds-hud-speedtex-hash-span, lus-3ds-fog-exact-params, lus-3ds-texcache-resource-stable-key, lus-3ds-settimg-resolution-memo, lus-s7-raw-instance-dispatch, lus-s7-geo-diag-gate, lus-s7-tri-state-memo, lus-3ds-primenv-flush, lus-prof-sections, lus-flat-dispatch, lus-vtx-mtx-hoist, lus-profop, lus-tmem-diag-race-latch, lus-tmem-span-store, lus-tmem-same-content-skip, lus-texrect-run-memo, lus-texrect-viewport-hoist, lus-3ds-livery-ident, lus-traffic-pipesync-noop, lus-traffic-tri-memo-whitelist, lus-traffic-vtx-clipmask, lus-3ds-shade-alpha-ccmux, lus-currentdir-reset-churn, lus-cc-key-uninit-shader-id, lus-crowd2-tilestate-value-gate, lus-3ds-triloop-packed-vbo, lus-tri2-phase-census, lus-trifast-tri-memo-pack, lus-tmem2-tmemfast, lus-trect-census, lus-trectbatch-atlas, lus-renderthread-texcache-owner; decomp-ilp32, decomp-port-segment-bzero, decomp-3ds-dma-low-address, decomp-port-audio-specwait-yield, decomp-race-cull-diagnostics, decomp-3ds-sky-overscan-deadend-note, decomp-3ds-machineselect-gradient-coalesce, decomp-3ds-rom-audio-port, decomp-port-course-select-state-reset, decomp-port-rival-detail.

---

## 6. Research docs (one-liners; date = doc date, else git)

| Doc | Date | One-liner / key numbers |
|---|---|---|
| `3ds-port-research.md` | 2026-08-10 | Founding dossier: port feasible, hand-port citro3d backend, New3DS primary; seg-8 decode 133.95 ms (PC). |
| `3ds-port-plan.md` | 2026-08-10/11 | Multi-agent plan: Phase 0, streams A–F, gates M1–M5, worktree rules. |
| `spike-lus-carve-report.md` | 2026-08-12 (git) | Carve spike PASS: 36 LUS sources compile on devkitARM; 13-line newlib patch. |
| `combiner-census.md` | 2026-08-12 | Static combiner census: all modes ≤3 TexEnv stages. |
| `3ds-memory-budget.md` | 2026-08-12 | Fits New3DS 124 MB (~40–50 MB headroom); old3DS 64 MB does not. |
| `32bit-sweep.md` | 2026-08-12 (git) | ILP32 sweep: 13 fixes, verified-clean list, deferred submodule items. |
| `m1-link-status.md` | 2026-08-13 (git) | Full link achieved; 8 boot risks enumerated; "one syscall per readability probe". |
| `m1-boot-debug.md` | 2026-08-13→14 (git) | War log: every root cause from black screen to playable race; T-TEXCACHE table (misses −80 %, +10 fps emu); heap 41→86 MB burst fixed; 17-min soak peak 47.3 MB. |
| `patch-consolidation-plan.md` | 2026-08-14 | Retire 9 patches into fork branches + one pin bump; "independent revertability is the point". |
| `stereo3d-research.md` | 2026-08-14 | Verified canonical citro3d stereo pattern; off-axis shear valid in clip space; "no measured fps cost of stereo on New3DS". |
| `60fps-research.md` | 2026-08-14 | sm64-3ds reached 60 fps with AA off, audio on core 2 (Luma ≥10.1.1), 30 Hz logic+interp; "no surviving claim contains measured ms/% numbers". |
| `60fps-campaign-plan.md` | 2026-08-14 (+S12 08-15) | Shifts S0–S12 with gates: p95 ≤14 ms → native 60; >18 ms → 30 Hz+interp retrofit. |
| `renderer-architecture-research.md` | 2026-08-15 | Fast3D interpreter is the right choice; RT64 impossible on PICA200; DaedalusX64 20–30 fps analogy. |
| `RESUME.md` | 2026-08-14 (+08-19/20 updates) | Pause state; fog root cause; dead agents; emu 15–20 fps menus / ~10 fps race. |
| `measurement-2026-08-20.md` | 2026-08-20 | CPU-bound verdict: GPU 0.4 ms vs build 26–58 ms (emu); S11 moot, S12 struck. |
| `morning-status-2026-08-20.md` | 2026-08-20 | Fog solved & merged; night branch strict improvement; sky overscan dead end. |
| `night-verify-2026-08-20.md` | 2026-08-20 | fx+sky consolidated; overscan proven ineffective (topGap ~0.31 at 1.41× and 2.70×); decal bias 0.004. |
| `reference-diff-2026-08-20.md` | 2026-08-20 | `_scan.bmp` oracle untrustworthy; RGBA16 word-swap premise false. |
| `sky-backdrop-wedge-analysis.md` | 2026-08-20 / 08-27 | v1: clear-colour gutter (not fog regression); SKY-WEDGE-3 superseding: pow2-padding sampling, magenta flood 55→4957 px, fix drives 2963→53 px. |
| `tunnel-roof-cull-analysis.md` | 2026-08-20 (git) | Face-cull hypothesis ruled out (GPU_CULL_NONE, CPU cull stock). |
| `tunnel-roof-depth-clip-analysis.md` (roof/) | 2026-08-20 (git) | Depth/clip math correct; `[roof]` diagnostic added; no fix. |
| `morning-status-2026-08-21.md` | 2026-08-21 | Perf sprint numbers (menu 15→19.9 fps; 13.8→19.8; wVbl→0; uploads 155→0.5); env-color root cause. |
| `audio-regression-forensics.md` | 2026-08-21 | "There was no regression"; rogue revert; Audio_Update stubbed under `#ifdef PORT`. |
| `RESUME-2026-08-22.md` | 2026-08-21 | Runs on New3DS; HW median 48–50 / p95 58 / max 59 fps, traffic 25–35; `[prof!]` worst window. |
| `crowd2-fresh-profile.md` | 2026-08-27 | Crowd build 39–42 ms emu: br 11.5, dsp 18.2, tri 10.2, drw 7.2; lever 1 −1.2 ms; right-eye batching = top HW lever. |
| `triloop-packed-vbo-2026-08-27.md` | 2026-08-27 | Packed PICA vertex emission; reasoned ~0.5–1.2 ms drw / 0.3–0.8 ms tri; not emu-verified. |
| `shadow2-mirror-period-audit.md` | 2026-08-27 (git) | PICA mirror period is the padded size (latent); provably not the shadow bug; regression test added. |
| `gputransform-report.md` (gputransform/) | 2026-08-28 (git) | GPU T&L phase 1: vtx −62 % unlit / −26 % light race (emu), 220,989 draws; quarantined → shelved. |
| `RESUME-2026-09-02.md` | 2026-08-28 | Release-grade; HW median ~51 / p95 60 / max 60; bridge cache = final lever; gputransform shelved. |
| `bridgecache-brief.md` | 2026-09-01 | Brief: cache ProcessList output; O(1)-per-list validity hard constraint; 60 % go/no-go bar. |
| `bridgecache-progress.md` | 2026-09-01 | M1 NO-GO (static 12.8–16.1 %); brfast M2–M5: br 11.49→5.04 ms, −52 %; storm parity; desktop compile notes. |
| `render-architecture-alternatives.md` | 2026-09-01 | No shipped port retains translated DLs; GPU T&L is field's #1 pick; 87 % of commands host-built per frame. |
| `home-crash-audit.md` | 2026-09-01 | HOME crash ranked causes; audio park fix; Luma dump parser; close-from-HOME hang root cause. |
| `locked60-campaign.md` | 2026-09-02 | Campaign brief + HW profile (br 3.2–3.8, dsp 6.5–7.3, tri 3.4–3.6, E4 3.9–8.9 ms); Tasks A–E; rules. |
| `RESUME-2026-09-03.md` | 2026-09-02 | brfast HW br 6.43→3.73 (−42 %); HOME fix verified; filelog 1 MB. |
| `tri2-progress.md` | 2026-09-02 | s7 memo hit 0/miss 3.94 M; trifast: crowd tri −17 %, menu −19 %, race −15 % (emu); GO. |
| `tmem2-progress.md` | 2026-09-02 | F3 phase split (walk ~50 %); tmemfast F3 −22 % crowd / −35 % menu; HW proj 0.7–1.0 ms. |
| `texrect2-progress.md` | 2026-09-02 | Run merging NO-GO; HUD atlas GO: menu E4 −2.47 ms, draws 136→24.5; race-start build 23.6→20.5. |
| `bootaudio-progress.md` | 2026-09-02 | audioprime: under 3/3→0/3 (18 ms stall-sim); primed ring=2208 at 540 ms. |
| `locked60-research-2026-09-03.md` | 2026-09-02 (git) | Deep research 2: core 2 usable (Luma 0x2000 cap), core 3 unusable; ARM11 indirect jumps 5–9 cycles; ~22 µs/draw is the batch split; ranked levers vs 18.7 ms crowd frame. |
| `locked60-round2.md` | 2026-09-02 (doc "09-03") | Round-2 brief: HW crowd dsp 7.8 tri 4.6 br 3.1 drw 1.2 vtx 1.2 imp 0.8 = ~18.7 ms; Tasks F, G. |
| `renderthread-brief.md` | 2026-09-02 (doc "09-03") | Task H brief: `[gpu] wall=17-22 build=10.5-12.8`; logic ~7–9 + render ~11–13 ms sequential. |
| `renderthread-audit.md` | 2026-09-02 | M1 audit: rendering runs inside a game fiber; fork/join design; shared-state hazard table; §8 why pipe serialized (waitMain 14–15 ms) and ahead mode. |
| `renderthread-progress.md` | 2026-09-02→03 (doc "09-05") | M2–M7: pipe/ahead emu parity (zero errors, heap 44.7 MB); emu tax dsp 18.7 vs 16.4; M7 crash fix. |
| `dspcensus-progress.md` (dspcensus/) | 2026-09-02 | Per-opcode census (F3 4.71 ms, E4 1.45, 06 1.34 crowd emu); dspfast engages but ≈0 → default OFF. |
| `bootaudio2-progress.md` | 2026-09-02 | HW `under=2` at 10.7 s; preload on core 1 counterproductive (reverted); uncompressed audio_table (+1.65 MB archive) fixes it. |
| `RESUME-2026-09-04.md` | 2026-09-02 (doc "09-03 evening") | Campaign merged: HW steady 16.4→14.9 ms, crowd 20.2→18.7; median 46→49, p10 39→42 (trace on). |
| `locked60-round3.md` | 2026-09-02 (doc "09-04") | Round-3 brief: HW logic ~7–9 ms core 0, render ~12–15 ms core 2 (dsp 4.9 tri 4.2 br 3.1); Tasks I, J, E. |
| `balance-progress.md` | 2026-09-02 | bridgemain: br 9.1→0.00 render-side, wall 76.0→66.6 crowd (emu); heap +1.05 MB. |
| `dynlod-progress.md` | 2026-09-02 | Auto rival detail (15/12 ms thresholds); tier 2 vs 0 = −5 ms/frame emu; "hardware MINIMAL was +8-9 fps". |
| `atlas-progress.md` (atlas/) | 2026-09-02 | Machine-texture atlas census: 7.8 clamp-safe merges/frame → 0.08–0.16 ms HW → NO-GO, default OFF. |
| `RESUME-2026-09-05.md` | 2026-09-02 (doc "09-04 late") | Round 3 merged: render thread mode 1 HW median 49→56, p10 42→48; boot preload 4.3→2.0 s; M6 in progress. |
| `viewport-brief.md` / `viewport-progress.md` | 2026-09-03 | Sub-viewport 3D ~1.6× too large; cause = full-size N64 viewport per cell; fixed by folding into projection (emu-verified). |
| `stereo-anchor-progress.md` | 2026-09-03 | Position markers anchored at ship depth; eye shift 26.3 px marker vs 20.7 machine vs 0.5 HUD (emu). |
| `locked-sixty-report.html` | 2026-09-03 (`eeb3b56`) | Ledger of HW rounds 1–5: median 51.4→59.6, p10 39.2→51.4, beats at 60: 9 %→53 %; lever ledger; "Fixed along the way" list. |
| `docs/3DS-HARDWARE.md` | 2026-08-14 (+08-21) | Runbook: New3DS + Luma ≥10.1.1, .3dsx/.cia, SD layout, dspfirm.cdc dump, controls, INI keys, first-run telemetry. |
| `port/3ds/gfx/STEREO.md` | 2026-08-14/27 | Stereo foundation status: dual targets, slider gate, per-eye draw over the same VBO, exact clip-space off-axis shift (iod 12 px, convergence 0.25); remaining work list. |
| `README-3DS.md` | 2026-08-28 | Release README: "~50–60 fps typical"; features, legal, known issues. |
| `port/3ds/gfx/STATUS.md`, `port/3ds/README*` | — | **Not present** in any worktree (`docs/STATUS.md` exists but is the desktop status as of 2026-07-31). |
