# G-Diffuser 3DS port — performance history (chart-ready extract)

Generated 2026-09-03T14:23. Primary source: `~/code/gdx-3ds/anchor/docs/research/locked-sixty-report.html` (commit eeb3b56, 2026-09-03). Secondary: docs/research/*.md in the `~/code/gdx-3ds/*` worktrees; hardware logs `/tmp/hw-art-1*/log*.txt`. fps method: `[watchdog]` beat frames / 5 s; median = statistics.median; p10 = sorted[floor(0.1n)]; at-cap = beats >= 59.5 fps (this reproduces every report number exactly).

## 1. Hardware rounds (New 3DS)

| Round | Date | Branch @ commit | Levers | Median fps | p10 fps | at cap | Beats | Source |
|---|---|---|---|---|---|---|---|---|
| HW baseline (pre-campaign, first hardware profile) | 2026-08-21 | feat/3ds-hwaudio @ db38c47 | S7 interpreter opts, texcache, hwaudio, (pre-campaign mainline) | 48-50 | n/a | n/a | n/a | research/RESUME-2026-08-22.md |
| HW session 2026-08-28 (release-grade mainline, APT fix) | 2026-08-28 | feat/3ds-hwaudio @ 5f0bb25 / a9eb47f | rival_detail (manual MINIMAL), APT fix, crowd2 lever 1, shadow2/rivallod | 51 | n/a | n/a | n/a | research/RESUME-2026-09-02.md |
| Round 1 — Mainline before the campaign | 2026-09-02 | feat/3ds-hwtest @ 70d0e25 | brfast (bridge memos; toggle available, no toggle logged), HOME fix, DBG toggle | 51.4 | 39.2 | 9% | 754 | research/locked-sixty-report.html |
| Round 2 — Bridge memos (brfast) | 2026-09-02 | feat/3ds-hwtest @ 70d0e25 | brfast (toggled 0/1 in-session via DBG tab) | 57.2 | 46.6 | 29% | 78 | research/locked-sixty-report.html |
| Round 3 — HUD atlas · triangle memo · TMEM · audio | 2026-09-02 | feat/3ds-hwtest2 @ d646789 | brfast, trectbatch (HUD atlas), trifast (tri memo fix), tmemfast, audioprime | 56.0 | 48.0 | 32% | 237 | research/locked-sixty-report.html |
| Round 4 — Render thread on core 2 (pipe) | 2026-09-02 | feat/3ds-hwtest3 @ b018d22 | campaign levers, renderthread=1 (pipe, core 2), audioprime2 + uncompressed o2r, console filter | 57.6 | 44.6 | 14% | 58 | research/locked-sixty-report.html |
| Round 5 — Ahead mode · bridge on main · auto rival detail | 2026-09-03 | feat/3ds-hwtest4 @ 978f31d | campaign levers, renderthread=2 (ahead), bridgemain, rival_detail_auto (dynlod), texture-cache crash fix | 59.6 | 51.4 | 53% | 229 | research/locked-sixty-report.html |

Pre-campaign rows: p95/max instead of p10/cap — 2026-08-21: p95 58, max 59, traffic dips 25-35 (RESUME-2026-08-22.md:28); 2026-08-28: p95 60, max 60, MINIMAL crowd floor ~40 (RESUME-2026-09-02.md:10).

### Report rounds recomputed from the on-disk hardware logs

| Round | Log | Beats | Median | p10 | p95 | at cap (>=59.5) | ==60 | Report says |
|---|---|---|---|---|---|---|---|---|
| Round 1 | `/tmp/hw-art-1788356870/log.txt` | 756 | 51.2 | 38.8 | 60.0 | 9% | 5% | med 51.4 / p10 39.2 / cap 9% / 754 beats |
| Round 2 | `/tmp/hw-art-1788357755/log.txt` | 78 | 57.2 | 46.6 | 60.0 | 29% | 17% | med 57.2 / p10 46.6 / cap 29% / 78 beats |
| Round 3 | `/tmp/hw-art-1788377839/log.txt` | 237 | 56.0 | 48.0 | 60.0 | 32% | 17% | med 56.0 / p10 48.0 / cap 32% / 237 beats |
| Round 4 | `/tmp/hw-art-1788399103/log.txt` | 58 | 57.6 | 44.6 | 60.0 | 14% | 5% | med 57.6 / p10 44.6 / cap 14% / 58 beats |
| Round 5 | `/tmp/hw-art-1788441668/log.txt` | 230 | 59.6 | 51.4 | 60.0 | 53% | 38% | med 59.6 / p10 51.4 / cap 53% / 229 beats |

Round 1 log has 756 beats incl. two trailing 0-fps beats (report trimmed to 754). Round 5 log has 230 beats (report 229).

### Hardware companion sessions (traced / A-B; not fps rows in the report)

| Session | Log | Beats | Median (trace on) | p10 | Crowd CPU ms (vtxN 800-1400, n) | Steady CPU ms (400-800, n) | rt waitMain mean | Doc claim |
|---|---|---|---|---|---|---|---|---|
| Round 2 traced A/B (brfast on vs off, gputrace on) | `/tmp/hw-art-1788359627/log.txt` | 47 | 49.8 | 40.6 | 18.8 (91) | 15.6 (41) | - | brfast HW [prof] crowd windows br 6.43 -> 3.73 ms (-42%), all-window mean 5.18 -> 2.83; other buckets unchanged. Watchdog fps floors rose (p10 ~42-48 -> ~47-55). |
| Round 3 traced A/B — campaign levers OFF (control) | `/tmp/hw-art-1788380826/log.txt` | 31 | 46.0 | 39.0 | 20.2 (42) | 16.4 (55) | - | HW A/B (same course, gputrace on, 3D on): steady race 16.4 -> 14.9 ms CPU/frame, crowd 20.2 -> 18.7; fps median 46 -> 49 (trace tax on), p10 39 -> 42. |
| Round 3 traced A/B — campaign levers ON | `/tmp/hw-art-1788386194/log.txt` | 28 | 49.2 | 41.6 | 18.8 (45) | 14.9 (52) | - |  |
| Round 4 traced — render thread pipe (gputrace on) | `/tmp/hw-art-1788396432/log.txt` | 31 | 56.0 | 48.4 | 15.1 (37) | 12.0 (61) | 13.2 ms | render thread on core 2 mode 1 — HW: fps median 49->56 p10 42->48 (trace on) but main still waits 10-15 ms/frame for the renderer |

brfast toggle split in `/tmp/hw-art-1788359627/log.txt` (nD>=100 windows): brfast=0 br=6.43 ms (n=6) vs default-on br=3.73 ms (n=7) — matches RESUME-2026-09-03.md:6 '6.43 -> 3.73'.

Round 2 fps by brfast toggle (`/tmp/hw-art-1788357755/log.txt`): before-any-toggle(default): 17 beats, median 52.6, p10 31.0, cap 18%; brfast=0: 7 beats, median 56.0, p10 42.0, cap 0%; brfast=1: 54 beats, median 58.0, p10 52.8, cap 37%.

## 2. Per-bucket CPU timings (ms/frame)

### Report profiler sessions (hardware, profiler on; reproduced exactly from the logs)

| Session | Log | Band | n | br | dsp | tri | drw | vtx | imp | total | nD |
|---|---|---|---|---|---|---|---|---|---|---|---|
| Campaign levers off (control, same course) | `hw-art-1788380826/log.txt` | crowd | 42 | 2.96 | 8.26 | 5.58 | 1.48 | 1.12 | 0.78 | 20.2 | 67 |
| Campaign levers off (control, same course) | `hw-art-1788380826/log.txt` | steady | 55 | 2.20 | 7.63 | 3.98 | 1.18 | 0.66 | 0.76 | 16.4 | 54 |
| Campaign levers on (atlas · tri memo · TMEM · brfast) | `hw-art-1788386194/log.txt` | crowd | 45 | 3.07 | 7.84 | 4.64 | 1.24 | 1.18 | 0.78 | 18.8 | 55 |
| Campaign levers on (atlas · tri memo · TMEM · brfast) | `hw-art-1788386194/log.txt` | steady | 52 | 2.17 | 7.35 | 3.22 | 0.79 | 0.65 | 0.72 | 14.9 | 35 |
| Render thread (pipe) (core 2 renders, main waits) | `hw-art-1788396432/log.txt` | crowd | 37 | 3.12 | 4.89 | 4.21 | 1.16 | 1.01 | 0.68 | 15.1 | 55 |
| Render thread (pipe) (core 2 renders, main waits) | `hw-art-1788396432/log.txt` | steady | 61 | 2.72 | 3.87 | 3.12 | 0.95 | 0.70 | 0.66 | 12.0 | 45 |

### All hardware logs with `[prof]` windows (recomputed; crowd = vtxN 800-1400, steady = 400-800)

| Log | Windows | Crowd n | br | dsp | tri | drw | vtx | imp | total | nD | Steady total (n) | [gpu] wall/build/gpu | rt mode / waitMain |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| `hw-art-1788359627/log.txt` | 134 | 91 | 3.85 | 6.89 | 4.64 | 1.53 | 1.14 | 0.80 | 18.8 | 71 | 15.6 (41) | 20.6 / 13.3 / 5.2 | - |
| `hw-art-1788377839/log.txt` | 14 | 0 | - | - | - | - | - | - | - | - | - | 20.6 / 10.2 / 4.7 | - |
| `hw-art-1788380826/log.txt` | 114 | 42 | 2.96 | 8.26 | 5.58 | 1.48 | 1.12 | 0.78 | 20.2 | 67 | 16.4 (55) | 21.7 / 14.0 / 5.2 | - |
| `hw-art-1788386194/log-prev.txt` | 4 | 0 | - | - | - | - | - | - | - | - | - | 19.5 / 11.2 / 3.5 | - |
| `hw-art-1788386194/log.txt` | 108 | 45 | 3.07 | 7.84 | 4.64 | 1.24 | 1.18 | 0.78 | 18.8 | 55 | 14.9 (52) | 20.0 / 12.4 / 5.1 | - |
| `hw-art-1788391964/log-prev.txt` | 4 | 0 | - | - | - | - | - | - | - | - | - | 19.5 / 11.2 / 3.5 | - |
| `hw-art-1788391964/log.txt` | 108 | 45 | 3.07 | 7.84 | 4.64 | 1.24 | 1.18 | 0.78 | 18.8 | 55 | 14.9 (52) | 20.0 / 12.4 / 5.1 | - |
| `hw-art-1788396432/log.txt` | 98 | 37 | 3.12 | 4.89 | 4.21 | 1.16 | 1.01 | 0.68 | 15.1 | 55 | 12.0 (61) | 18.1 / 20.5 / 5.5 | pipe / 13.2 |
| `hw-art-1788399103/log-prev.txt` | 98 | 37 | 3.12 | 4.89 | 4.21 | 1.16 | 1.01 | 0.68 | 15.1 | 55 | 12.0 (61) | 18.1 / 20.5 / 5.5 | pipe / 13.2 |
| `hw-art-1788399103/log.txt` | 131 | 35 | 3.42 | 5.92 | 4.49 | 1.33 | 1.16 | 0.74 | 17.1 | 64 | 11.3 (76) | 18.9 / 10.6 / 5.3 | pipe / 14.0 |
| `hw-art-1788411010/log-prev.txt` | 98 | 37 | 3.12 | 4.89 | 4.21 | 1.16 | 1.01 | 0.68 | 15.1 | 55 | 12.0 (61) | 18.1 / 20.5 / 5.5 | pipe / 13.2 |
| `hw-art-1788411010/log.txt` | 131 | 35 | 3.42 | 5.92 | 4.49 | 1.33 | 1.16 | 0.74 | 17.1 | 64 | 11.3 (76) | 18.9 / 10.6 / 5.3 | pipe / 14.0 |

### Doc-quoted hardware profiles

- **2026-08-21** — worst traffic window, real New3DS: `br=13.5-14.1 dsp=8.3-8.5 tri=6.7-7.0 imp=1.4-3.0 drw=2.3-2.4 vtx=1.8`; profop 06(TRI2)=6.9-7.7ms/430 F3(LOADBLOCK)=2.7/270 E7(PIPESYNC)=2.5/450 01(VTX)=2.0 E4(TEXRECT)=1.8 (RESUME-2026-08-22.md:29-31)
- **2026-09-02 (campaign brief)** — race crowd windows, gputrace on, 64-frame windows: `br=3.2-3.8 dsp=6.5-7.3 vtx=0.7 tri=3.4-3.6 imp=1.2-1.8 drw=2.3-2.5`; profop E4 texrect=3.9-8.9/90-138, 06 tri2=2.4-4.4/143-270, F3 loadblock=1.9-2.4/144-199, 01 vtx=0.6-1.2/28-79, FA/FB prim/env=0.5-1.0/25-33 each (locked60-campaign.md:8-13)
- **2026-09-03 (round-2 brief)** — crowd frame after the campaign, gputrace on, stereo on: `dsp=7.8 tri=4.6 br=3.1 drw=1.2 vtx=1.2 imp=0.8`; total 18.7 ms (locked60-round2.md:9-11)
- **2026-09-04 (round-3 brief)** — crowd frame with render thread mode 1: logic on core 0, render on core 2: `dsp=4.9 tri=4.2 br=3.1 drw=1.2 vtx=1.0 imp=0.7` (locked60-round3.md:7-10)

### Emulator-era profiles (Azahar, directional only)

- **2026-08-20** (Azahar proxy) — S0 measurement, in-race windows 3393-10689: {"build_ms": {"min": 25.1, "median": 26.1, "p95": 53.3, "max": 57.9}, "gpu_ms": 0.4, "draws_median": 66, "tris_median": 318} (measurement-2026-08-20.md:35-44)
- **2026-08-27** (Azahar (stereo on)) — crowd window (draws>120), per-frame ms: {"ms": {"build": "39-42", "br": 11.5, "dsp": 18.2, "tri": 10.2, "drw": 7.2, "vtx": 3.1, "imp": 1.7}, "profop": "06=12.5/424 E4=4.9/79 F3=4.0/256 01=3.6/147 FA=2.6/71 FB=1.5/62"} (crowd2-fresh-profile.md:9-22)

## 3. Levers

| Lever | Status | Effect | Numbers | Sources |
|---|---|---|---|---|
| **Bridge memos (brfast)** `[debug] brfast` | mainline (merged 2026-09-02 after HW verdict) | Emulator br 11.5 -> 4.9 ms; hardware br 6.4 -> 3.7 ms (crowd windows nD>=100: 6.43 -> 3.73, -42%), all-window mean 5.18 -> 2.83; watchdog p10 ~42-48 -> ~47-55. Report ledger also lists 'median 46 -> 49' on this row. | `{"emu_br_ms": [11.5, 4.9], "hw_br_ms_crowd": [6.43, 3.73], "hw_br_ms_all_windows": [5.18, 2.83], "hw_p10_fps_range": ["42-48", "47-55"]}` | locked-sixty-report.html; RESUME-2026-09-03.md:5-7; bridgecache-progress.md:127-131, 142, 213 |
| **HUD texture atlas + texrect batching (trectbatch)** `[debug] trectbatch` | mainline | Emulator (frame-aligned medians): menu E4 -2.47 drw -2.20 build -3.70 wall -3.80 nD 136->24.5; crowd E4 -0.74 drw -1.68 nD 154->72; race-start window E4 10.44->7.85, build 23.6->20.5. Hardware: draws/frame 67 -> 55 (report ledger; matches prof nD 67->55 crowd). | `{"emu_menu_ms": {"E4": -2.47, "drw": -2.2, "build": -3.7}, "emu_crowd_ms": {"E4": -0.74, "drw": -1.68}, "emu_draws_menu": [136, 24.5], "hw_draws_per_frame_crowd": [67, 55]}` | locked-sixty-report.html; texrect2-progress.md:77-86 |
| **Triangle memo fix + packed vertex loop (trifast)** `[debug] trifast` | mainline | s7 per-tri memo had never hit (textures_changed flag bug). Emulator census-off A/B: crowd tri 8.54->7.07 (-1.46, -17%), race steady 6.34->5.39 (-15%), menu 11.94->9.68 (-19%). Hardware: tri 5.6 -> 4.6 ms crowd (5.58 -> 4.64 in report profs). | `{"emu_tri_ms_crowd": [8.54, 7.07], "emu_pct": -17, "hw_tri_ms_crowd": [5.58, 4.64]}` | locked-sixty-report.html; tri2-progress.md:136-144 |
| **TMEM span fast path (tmemfast)** `[debug] tmemfast` | mainline | Emulator v2.1 A/B: crowd F3 5.96->4.66 (-1.31, -22%), dsp 14.77->13.52, build 33.12->31.75; menu F3 -35%; whole race F3 2.97->2.01 (-32%). Also fixed 8-byte TMEM alignment. Hardware: 'within dispatch bucket' (no separate HW number); projection 0.7-1.0 ms/frame crowd. | `{"emu_F3_ms_crowd": [5.96, 4.66], "emu_pct": -22, "hw_projection_ms_crowd": "0.7-1.0"}` | locked-sixty-report.html; tmem2-progress.md:82-100 |
| **Boot audio (audioprime / audioprime2 / uncompressed o2r)** `[debug] audioprime, audioprime2` | mainline | Emulator stall-sim 18 ms: underruns off 3/3 -> on 0/3; 30 ms: 3/3 -> 1/3 (v1), 0 (v2). Hardware: boot preload 4.3 -> 2.0 s (audio table stored uncompressed), font stall gone. HW still showed under=2 at ~10.7 s with v1. | `{"emu_underruns_stallsim18": ["3/3", "0/3"], "hw_boot_preload_s": [4.3, 2.0]}` | locked-sixty-report.html; bootaudio-progress.md:33-35; bootaudio2-progress.md:8-10, 50-55; RESUME-2026-09-05.md:9-10 |
| **Render thread on core 2, pipe mode (renderthread=1)** `[debug] renderthread=1` | mainline (round 3 merged, HW-verified) | Hardware (trace on, same course): fps median 49 -> 56, p10 42 -> 48; render-side crowd CPU 18.8 -> 15.1 ms (report profs); main still waits 10-15 ms/frame (waitMain 14-15 ms on hw-art-1788396432; log mean 13.2). Emulator: n/a (Azahar time-slices core 2; dsp 18.7 vs 16.4 and wall 47.5 vs 41.8 WORSE with thread on = emulator tax). | `{"hw_median_fps_trace_on": [49, 56], "hw_p10_fps_trace_on": [42, 48], "hw_crowd_cpu_ms_render_side": [18.8, 15.1], "hw_waitMain_ms": "14-15", "emu_dsp_ms": [16.4, 18.7]}` | locked-sixty-report.html; RESUME-2026-09-05.md:4-8; renderthread-progress.md:59-60, 75 |
| **Ahead mode (renderthread=2)** `[debug] renderthread=2` | round 5 (on the Round-5 card; HW A/B receipts with trace not on disk) | Emulator: waitMain == tk - ovl (main waits exactly the render time it could not cover); crowd window waitMain 20.71 ovl 1.91 tk 21.64 vs pipe waitMain 22.30 ovl 1.10 tk 21.39. Hardware: see Round 5 (median 59.6, p10 51.4, 53% at cap, with bridgemain + auto rival detail). | `{"emu_waitMain_ms_ahead_vs_pipe": [20.71, 22.3], "emu_ovl_ms_ahead_vs_pipe": [1.91, 1.1]}` | locked-sixty-report.html; renderthread-progress.md:82-97 |
| **Bridge on main (bridgemain)** `[debug] bridgemain` | round 5 | Emulator crowd windows (nD=169): render-side br 9.1 -> 0.00, brMain=3.6 ms/frame on main, tk 21.5 -> 18.7-19.6, ovl 1.9 -> 6.2, waitMain 20.5-20.7 -> 13.6-14.4; 108 aligned windows mean wall 31.7 -> 26.9 (emulator serialises cores). HW expectation: brMain ~3-4, tk 12-15 -> ~9-12. Hardware: see Round 5. | `{"emu_render_side_br_ms": [9.1, 0.0], "emu_brMain_ms": 3.6, "emu_tk_ms": [21.5, "18.7-19.6"], "emu_waitMain_ms": ["20.5-20.7", "13.6-14.4"]}` | locked-sixty-report.html; balance-progress.md:80-91, 106-113 |
| **Auto rival detail (rival_detail_auto / dynlod)** `[perf] rival_detail_auto` | round 5 | Raise tier when 4-frame avg render > 15.0 ms, lower after 30 frames < 12.0 ms. Emulator storm windows: tier 0 build 43.6/40.6/40.2/40.1 vs tier 2 38.1/35.3/35.1/35.2 (-5 ms/frame), nD 125->97. Hardware: hand-set MINIMAL was worth +8-9 fps in crowds (floor ~40, was 25-35). | `{"emu_build_ms_tier0_vs_tier2": [43.6, 38.1], "hw_manual_minimal_fps_gain": "+8-9", "hw_crowd_floor_fps": ["25-35", "~40"]}` | locked-sixty-report.html; dynlod-progress.md:67-73; locked60-round3.md:38; RESUME-2026-09-02.md:10-11 |
| **Prim/env colour folding (dspfast)** `[debug] dspfast` | off by default (cannot pay) | Emulator A/B crowd: build 54.32 -> 54.24, dsp 21.97 -> 22.00, drw 2.46 -> 2.62 (later 2.44), nD 111 -> 107. Texture switch splits the batch anyway; prim/env sole cause of only ~4-12 draws/frame (~0.05-0.1 ms HW). | `{"emu_build_ms_crowd": [54.32, 54.24], "hw_projection_ms": "0.05-0.1"}` | locked-sixty-report.html; dspcensus-progress.md:74-95 |
| **Machine texture atlas (atlas3d)** `[debug] atlas3d` | off by default (NO-GO) | Census: only 7.8 clamp-safe switches/frame in crowds; projection 0.08-0.16 ms HW. Emulator A/B crowd: build -0.04, dsp -0.23, drw -0.12, tri +0.27, nD 115 -> 108 (draws -7). Two thirds of machine textures wrap/mirror. | `{"emu_crowd_ms": {"build": -0.04, "dsp": -0.23, "drw": -0.12, "tri": 0.27}, "draws_per_frame": [115, 108], "hw_projection_ms": "0.08-0.16"}` | locked-sixty-report.html; atlas-progress.md:51-62 |
| **Bridge output cache (static-list translation cache)** `-` | not built (NO-GO census) | Static lists are 12.8-16.1% of walked commands; ceiling ~13-16% of br (~1.5 ms/frame emulator). Report: '87 % of commands are rebuilt every frame'. Effort redirected to brfast. | `{"emu_ceiling_ms": 1.5, "static_share_pct": "12.8-16.1"}` | locked-sixty-report.html; bridgecache-progress.md:11-30; RESUME-2026-09-03.md:19-20 |
| **GPU vertex transform (gputransform)** `[debug] gputransform` | shelved (branch feat/3ds-gputransform preserved) | Emulator: unlit course preview vtx 2.01 -> 0.77 (-62%), wall 56.9 -> 53.8 (-3.1 ms); in-race light vtx 0.93 -> 0.69 (-26%); lit grid identical. 220,989 GPU-transformed draws, zero fallback. Hardware: imperceptible (vtx is ~1 ms on HW) + venue-floor texture-mapping regression (PICA 24-bit uniforms). | `{"emu_vtx_ms_unlit": [2.01, 0.77], "emu_pct_unlit": -62, "emu_wall_ms_preview": [56.9, 53.8], "hw": "imperceptible"}` | locked-sixty-report.html; gputransform-report.md:98-129; RESUME-2026-09-02.md:32-38 |
| **crowd2 lever 1 (SETTILE/LOADBLOCK value gates)** `-` | shipped 2026-08-27 (d3a9820) | Emulator crowd 3393: build 42.3 -> 41.1 (-1.2 ms); tri 10.28->10.00, drw 7.27->6.94, vtx 3.12->2.97; steady ~0 to -0.3 ms; whole-run build mean -0.6%. | `{"emu_build_ms_crowd": [42.3, 41.1]}` | crowd2-fresh-profile.md:31-35 |
| **S7 interpreter opts + async frame-mirror + vblank-stall skip + resource keys (Aug 21 sprint)** `-` | merged 2026-08-21 (feat/3ds-m1 @ 3d2c1db) | Emulator: async frame-mirror menu fps 15.0 -> 19.9; S7 opts menu-nav 13.8 -> 19.8 fps, race build 26-58 -> ~24-46 ms; vblank skip wVbl 10-16 ms -> 0, title/menu wall 34 -> 21-27 ms; texture uploads ~155/frame -> ~0.5; select-screen fill-rects 224 -> ~8. | `{"emu_menu_fps": [15.0, 19.9], "emu_menu_nav_fps": [13.8, 19.8], "emu_race_build_ms": ["26-58", "24-46"]}` | morning-status-2026-08-21.md:9-16 |
| **Triloop packed VBO** `[debug] triloop` | in mainline patch stack (reasoned delta only, never A/B-measured) | Reasoned: ~0.5-1.2 ms off drw and ~0.3-0.8 ms off tri per crowd frame (NOT emulator-verified; sm64-3ds-port precedent 5.6 -> 4.8 ms). | `{"reasoned_drw_ms": "0.5-1.2", "reasoned_tri_ms": "0.3-0.8"}` | triloop-packed-vbo-2026-08-27.md:34-54, 60 |

## 4. Emulator vs hardware notes

- Emulator numbers never rank levers. Azahar time-slices the 3DS cores on one host thread and its per-instruction costs differ; it was used for correctness, A/B deltas on frame-aligned windows, and screenshot parity. Every lever's magnitude claim above comes from the card. — _locked-sixty-report.html_
- Emulator ratios differ (E4=61 us there vs ~40-60 us here); trust hardware for ranking, use the emulator for A/B deltas on frame-aligned windows. — _locked60-campaign.md:14-15_
- The emulator's ratios differ (svc probes cost ~1.3 us each there); use it for A/B deltas and attribution shape, never for absolute ranking. — _locked60-round2.md:12-13_
- gputrace INFLATES the HW profile (profop svc ticks) — true-fps runs use gputrace=0. — _RESUME-2026-09-02.md:55_
- Tracing costs about 2 ms per frame, so those three sessions are compared with each other only. — _locked-sixty-report.html_
- Emulator fps is meaningless under machine load (builds + Ableton) — check `uptime` first. — _RESUME-2026-09-04.md:35_
- M5 — verification (Azahar; multi-core emulation is time-sliced on one host thread, so wall/dsp are NOT perf evidence — engagement, correctness, parity and heap only) — _renderthread-progress.md:45-46_
- Azahar is ~1.2x slower than hardware on this path, so the SHIPPED defaults are hardware-scaled — _dynlod-progress.md:45-46_
- Emulator absolutes UNDERSELL the ARM11: Azahar's dynarmic JIT makes CPU float math nearly free while real-hardware [prof] runs are svc/memory bound — the same class of delta has historically grown 2-4x on the New3DS. — _gputransform-report.md:124-128_
- Every fps number in the record so far is an Azahar proxy. Azahar quantizes to vblank divisors (60/2 = 30, 60/3 = 20 ...) — _60fps-campaign-plan.md:14-18_
- Azahar undersells HW + fakes DSP/threading — HW is truth. — _RESUME-2026-08-22.md:61_
- HW crowd-window selection: use vtxN buckets, not nD (the atlas cuts draws 4x). — _RESUME-2026-09-04.md:32_
- Hardware fps in the report/logs comes from '[watchdog] beat=N frame=F(+D)' lines: fps = D / 5 (5-second windows). No '[fps]' lines exist in any log. Report stats reproduce as: median = statistics.median(beats), p10 = sorted(beats)[floor(0.1*n)], at-cap = share of beats >= 59.5 fps. — _locked-sixty-report.html ('How to read the numbers'); verified against /tmp/hw-art-1*/log.txt_
- The Azahar SD folder logs (~/Library/Application Support/Azahar/sdmc/3ds/gdiffuser/*.txt, Aug 21-27) and /tmp/vp-art, /tmp/dynlod-art, /tmp/balance-art, /tmp/bootaudio-art are EMULATOR runs (agent A/B artifacts); only /tmp/hw-art-1*/ are hardware card snapshots. — _directory provenance + docs (locked60-campaign.md lines 34-43 'Snapshot log.txt to /tmp/<task>-art/'; RESUME-2026-09-04.md line 33 hw-art glob)_

## 5. Emulator logs on disk (Azahar — fps NOT hardware-meaningful)

| Log | mtime | Beats | Median | p10 | Crowd total ms (n) | Receipts |
|---|---|---|---|---|---|---|
| `SDMC/log-crowd2-baseline.txt` | 2026-08-27T15:54 | 94 | 48.2 | 14.6 | - | gpu |
| `SDMC/log-prev-control-audiocontent.txt` | 2026-08-21T16:55 | 167 | 55.4 | 51.4 | - |  |
| `SDMC/log-prev-m1-fixed2.txt` | 2026-08-21T17:13 | 56 | 47.8 | 16.4 | - |  |
| `SDMC/log-prev-other2-1787864612.txt` | 2026-08-27T17:02 | 95 | 17.4 | 12.0 | 26.9 (1) | gpu |
| `SDMC/log-prev-precrowd2.txt` | 2026-08-27T15:25 | 297 | 57.4 | 41.4 | - |  |
| `SDMC/log-prev-pretraffic.txt` | 2026-08-21T20:22 | 39 | 30.0 | 11.8 | 37.0 (3) | gpu |
| `SDMC/log-prev-rivallod-minimal-1787877750.txt` | 2026-08-27T20:42 | 69 | 38.0 | 12.0 | 38.4 (11) | gpu |
| `SDMC/log-prev-rivallod-native-1787877041.txt` | 2026-08-27T17:13 | 114 | 41.2 | 17.2 | 32.9 (11) | gpu |
| `SDMC/log-prev-rivallod-reduced-1787877395.txt` | 2026-08-27T20:36 | 68 | 32.1 | 20.2 | 30.0 (47) | gpu |
| `SDMC/log-prev-rivallod2-minimal-1787879076.txt` | 2026-08-27T21:04 | 79 | 41.0 | 12.0 | 38.3 (10) | gpu |
| `SDMC/log-prev-rivallod2-native-1787878267.txt` | 2026-08-27T20:48 | 69 | 35.0 | 12.0 | 37.5 (8) | gpu |
| `SDMC/log-prev-rivallod2-reduced-1787878671.txt` | 2026-08-27T20:57 | 79 | 41.0 | 12.0 | 33.8 (7) | gpu |
| `SDMC/log-prev-rivallod3-minimal-1787880746.txt` | 2026-08-27T21:32 | 99 | 32.8 | 12.0 | 37.7 (12) | gpu |
| `SDMC/log-prev-rivallod3-native-1787879737.txt` | 2026-08-27T21:11 | 79 | 41.8 | 12.0 | 39.0 (8) | gpu |
| `SDMC/log-prev-rivallod3-reduced-1787880242.txt` | 2026-08-27T21:23 | 99 | 32.8 | 12.0 | 34.8 (7) | gpu |
| `SDMC/log-prev-rspolish-baseline.txt` | 2026-08-21T20:15 | 38 | 45.6 | 16.2 | - |  |
| `SDMC/log-prev-rspolish-runA.txt` | 2026-08-21T20:19 | 36 | 29.7 | 11.8 | 37.1 (3) | gpu |
| `SDMC/log-prev-rspolish-runB.txt` | 2026-08-21T20:22 | 39 | 30.0 | 11.8 | 37.0 (3) | gpu |
| `SDMC/log-prev-transition-base.txt` | 2026-08-27T09:41 | 66 | 45.5 | 28.4 | - |  |
| `SDMC/log.txt` | 2026-09-03T13:23 | 23 | 45.2 | 22.0 | - | audioprime,audioprime2,bcache-census,brfast,rt,tmem2,trect2,trifast |
| `/tmp/vp-art/anchor-burst-log.txt` | 2026-09-03T13:19 | 40 | 46.0 | 23.2 | - | audioprime,audioprime2,bcache-census,brfast,rt,tmem2,trect2,trifast |
| `/tmp/vp-art/anchor-drive-log.txt` | 2026-09-03T13:10 | 53 | 50.8 | 31.4 | - | audioprime,audioprime2,bcache-census,brfast,rt,tmem2,trect2,trifast |
| `/tmp/vp-art/anchor-fine-log.txt` | 2026-09-03T13:23 | 23 | 45.2 | 22.0 | - | audioprime,audioprime2,bcache-census,brfast,rt,tmem2,trect2,trifast |
| `/tmp/vp-art/anchor-race-log.txt` | 2026-09-03T12:46 | 24 | 22.0 | 21.8 | - | audioprime,audioprime2,rt |
| `/tmp/vp-art/anchor-race2-log.txt` | 2026-09-03T12:49 | 25 | 55.4 | 22.0 | - | audioprime,audioprime2,rt |
| `/tmp/vp-art/anchor-race3-log.txt` | 2026-09-03T12:53 | 41 | 55.4 | 22.0 | - | audioprime,audioprime2,rt |
| `/tmp/vp-art/anchor-race4-log.txt` | 2026-09-03T12:57 | 41 | 57.4 | 23.0 | - | audioprime,audioprime2,bcache-census,brfast,rt,tmem2,trect2,trifast |
| `/tmp/vp-art/anchor-sbs-log.txt` | 2026-09-03T12:43 | 31 | 22.0 | 22.0 | - | audioprime,audioprime2,rt |
| `/tmp/vp-art/anchor-seq-log.txt` | 2026-09-03T13:01 | 35 | 58.4 | 22.0 | - | audioprime,audioprime2,bcache-census,brfast,rt,tmem2,trect2,trifast |
| `/tmp/vp-art/anchor-start-log.txt` | 2026-09-03T13:14 | 32 | 58.5 | 22.0 | - | audioprime,audioprime2,bcache-census,brfast,rt,tmem2,trect2,trifast |
| `/tmp/vp-art/detail-b0-log.txt` | 2026-09-03T10:33 | 10 | 50.9 | 29.8 | - | audioprime,audioprime2,rt |
| `/tmp/vp-art/detail-b1-final-log.txt` | 2026-09-03T11:35 | 31 | 57.2 | 23.8 | - | audioprime,audioprime2,dynlod,rt |
| `/tmp/vp-art/detail-b1-log.txt` | 2026-09-03T10:40 | 15 | 32.4 | 23.6 | - | audioprime,audioprime2,rt |
| `/tmp/vp-art/detail-b1-v2-log.txt` | 2026-09-03T11:41 | 31 | 57.6 | 23.4 | - | audioprime,audioprime2,dynlod,rt |
| `/tmp/vp-art/diag-probe-log.txt` | 2026-09-03T11:07 | 18 | 23.8 | 23.4 | - | audioprime,audioprime2,rt |
| `/tmp/vp-art/diag-t0-log.txt` | 2026-09-03T11:02 | 18 | 23.8 | 23.4 | - | audioprime,audioprime2,rt |
| `/tmp/vp-art/diag-t0b-log.txt` | 2026-09-03T11:05 | 18 | 23.8 | 23.4 | - | audioprime,audioprime2,rt |
| `/tmp/vp-art/fix-detail-b1-log.txt` | 2026-09-03T10:46 | 15 | 32.2 | 23.6 | - | audioprime,audioprime2,rt |
| `/tmp/vp-art/fix-machsel-b0-log.txt` | 2026-09-03T10:47 | 15 | 32.4 | 23.6 | - | audioprime,audioprime2,rt |
| `/tmp/vp-art/fix-machsel-b1-log.txt` | 2026-09-03T10:45 | 15 | 32.4 | 23.4 | - | audioprime,audioprime2,rt |
| `/tmp/vp-art/fix2-machsel-b0-log.txt` | 2026-09-03T10:52 | 15 | 32.4 | 23.6 | - | audioprime,audioprime2,rt |
| `/tmp/vp-art/fix3-machsel-b0-log.txt` | 2026-09-03T10:56 | 18 | 23.9 | 23.6 | - | audioprime,audioprime2,rt |
| `/tmp/vp-art/fix4-detail-b1-log.txt` | 2026-09-03T11:17 | 18 | 23.8 | 23.4 | - | audioprime,audioprime2,rt |
| `/tmp/vp-art/fix4-machsel-b0-log.txt` | 2026-09-03T11:15 | 18 | 23.9 | 23.4 | - | audioprime,audioprime2,rt |
| `/tmp/vp-art/fix5-detail-b0-log.txt` | 2026-09-03T11:26 | 18 | 41.2 | 23.4 | - | audioprime,audioprime2,rt |
| `/tmp/vp-art/fix5-machsel-b1-log.txt` | 2026-09-03T11:25 | 18 | 23.9 | 23.4 | - | audioprime,audioprime2,rt |
| `/tmp/vp-art/machsel-b0-log.txt` | 2026-09-03T10:31 | 10 | 51.3 | 32.4 | - | audioprime,audioprime2,rt |
| `/tmp/vp-art/machsel-b1-log.txt` | 2026-09-03T10:38 | 15 | 32.4 | 23.6 | - | audioprime,audioprime2,rt |
| `/tmp/vp-art/machsel-b1-v2-log.txt` | 2026-09-03T11:44 | 31 | 23.4 | 23.4 | - | audioprime,audioprime2,dynlod,rt |
| `/tmp/vp-art/race-check-log.txt` | 2026-09-03T11:32 | 31 | 57.4 | 23.6 | - | audioprime,audioprime2,dynlod,rt |
| `/tmp/dynlod-art/A-log.txt` | 2026-09-02T22:04 | 79 | 34.2 | 13.2 | 36.4 (8) | audioprime,audioprime2,bcache-census,brfast,dynlod,gpu,rt,tmem2,trect2,trifast |
| `/tmp/dynlod-art/B-log.txt` | 2026-09-02T22:17 | 77 | 33.4 | 13.0 | 36.8 (9) | audioprime,audioprime2,bcache-census,brfast,dynlod,gpu,rt,tmem2,trect2,trifast |
| `/tmp/dynlod-art/B2-log.txt` | 2026-09-02T23:08 | 77 | 33.0 | 13.2 | 37.4 (11) | audioprime,audioprime2,bcache-census,brfast,dynlod,gpu,rt,tmem2,trect2,trifast |
| `/tmp/dynlod-art/C-log.txt` | 2026-09-02T23:14 | 63 | 32.2 | 13.0 | 34.4 (6) | audioprime,audioprime2,bcache-census,brfast,dynlod,gpu,rt,tmem2,trect2,trifast |
| `/tmp/dynlod-art/D-log.txt` | 2026-09-02T23:34 | 65 | 17.6 | 13.0 | 41.1 (7) | audioprime,audioprime2,bcache-census,brfast,dynlod,gpu,rt,tmem2,trect2,trifast |
| `/tmp/balance-art/bmA/log.txt` | 2026-09-02T22:23 | 69 | 46.8 | 15.0 | 28.5 (6) | audioprime,bcache-census,brfast,gpu,rt,tmem2,trect2,trifast |
| `/tmp/balance-art/ctrlB/log.txt` | 2026-09-02T22:28 | 69 | 36.8 | 13.2 | 33.0 (6) | audioprime,bcache-census,brfast,gpu,rt,tmem2,trect2,trifast |
| `/tmp/balance-art/stormBM/log.txt` | 2026-09-02T22:35 | 73 | 26.0 | 15.0 | 22.1 (29) | audioprime,bcache-census,brfast,gpu,rt,tmem2,trect2,trifast |
| `/tmp/bootaudio-art/log-displaced-1788362036.txt` | 2026-09-02T11:13 | 84 | 38.2 | 17.4 | 35.1 (7) | bcache-census,brfast,gpu |
| `/tmp/bootaudio-art/log-displaced-1788362102.txt` | 2026-09-02T11:14 | 11 | 37.8 | 25.8 | 25.0 (9) | audioprime,bcache-census,brfast,gpu |
| `/tmp/bootaudio-art/log-displaced-1788362167.txt` | 2026-09-02T11:16 | 11 | 45.4 | 27.0 | 26.2 (5) | audioprime,bcache-census,brfast,gpu |
| `/tmp/bootaudio-art/log-displaced-1788362232.txt` | 2026-09-02T11:17 | 11 | 46.0 | 27.0 | 25.1 (6) | audioprime,bcache-census,brfast,gpu |
| `/tmp/bootaudio-art/log-displaced-1788362297.txt` | 2026-09-02T11:18 | 11 | 45.2 | 26.8 | 22.9 (7) | audioprime,bcache-census,brfast,gpu |
| `/tmp/bootaudio-art/log-displaced-1788362363.txt` | 2026-09-02T11:19 | 11 | 40.6 | 26.6 | 24.1 (9) | audioprime,bcache-census,brfast,gpu |
| `/tmp/bootaudio-art/log-displaced-1788362428.txt` | 2026-09-02T11:20 | 11 | 37.6 | 26.8 | 25.4 (10) | audioprime,bcache-census,brfast,gpu |
| `/tmp/bootaudio-art/log-displaced-1788362493.txt` | 2026-09-02T11:21 | 11 | 39.2 | 26.2 | 25.7 (9) | audioprime,bcache-census,brfast,gpu |
| `/tmp/bootaudio-art/log-displaced-1788362559.txt` | 2026-09-02T11:22 | 11 | 43.2 | 26.8 | 24.9 (7) | audioprime,bcache-census,brfast,gpu |
| `/tmp/bootaudio-art/log-displaced-1788363817.txt` | 2026-09-02T11:43 | 84 | 39.0 | 19.0 | 34.6 (7) | bcache-census,brfast,gpu,trifast |
| `/tmp/bootaudio-art/log-displaced-1788363883.txt` | 2026-09-02T11:44 | 11 | 41.4 | 26.6 | 23.5 (9) | audioprime,bcache-census,brfast,gpu |
| `/tmp/bootaudio-art/off1-log-live.txt` | 2026-09-02T11:17 | 7 | 40.4 | 26.2 | 23.5 (6) | audioprime,bcache-census,brfast,gpu |
| `/tmp/bootaudio-art/off1-log.txt` | 2026-09-02T11:18 | 11 | 45.2 | 26.8 | 22.9 (7) | audioprime,bcache-census,brfast,gpu |
| `/tmp/bootaudio-art/on1-log-live.txt` | 2026-09-02T11:14 | 7 | 37.2 | 25.8 | 26.4 (4) | audioprime,bcache-census,brfast,gpu |
| `/tmp/bootaudio-art/on1-log.txt` | 2026-09-02T11:14 | 11 | 37.8 | 25.8 | 25.0 (9) | audioprime,bcache-census,brfast,gpu |
| `/tmp/bootaudio-art/on2-log-live.txt` | 2026-09-02T11:15 | 7 | 41.0 | 26.8 | 26.2 (5) | audioprime,bcache-census,brfast,gpu |
| `/tmp/bootaudio-art/on2-log.txt` | 2026-09-02T11:16 | 11 | 45.4 | 27.0 | 26.2 (5) | audioprime,bcache-census,brfast,gpu |
| `/tmp/bootaudio-art/on3-log-live.txt` | 2026-09-02T11:16 | 7 | 40.6 | 26.8 | 25.1 (6) | audioprime,bcache-census,brfast,gpu |
| `/tmp/bootaudio-art/on3-log.txt` | 2026-09-02T11:17 | 11 | 46.0 | 27.0 | 25.1 (6) | audioprime,bcache-census,brfast,gpu |
| `/tmp/bootaudio-art/onF-log-live.txt` | 2026-09-02T11:44 | 7 | 38.2 | 25.0 | 24.7 (5) | audioprime,bcache-census,brfast,gpu |
| `/tmp/bootaudio-art/onF-log.txt` | 2026-09-02T11:44 | 11 | 41.4 | 26.6 | 23.5 (9) | audioprime,bcache-census,brfast,gpu |
| `/tmp/bootaudio-art/raceF-log-live.txt` | 2026-09-02T11:51 | 75 | 46.2 | 14.4 | 29.1 (7) | audioprime,bcache-census,brfast,gpu |
| `/tmp/bootaudio-art/raceF-log.txt` | 2026-09-02T11:51 | 79 | 46.0 | 14.4 | 28.2 (8) | audioprime,bcache-census,brfast,gpu |
| `/tmp/bootaudio-art/raceon-log-live.txt` | 2026-09-02T11:28 | 75 | 46.4 | 16.8 | 29.2 (7) | audioprime,bcache-census,brfast,gpu |
| `/tmp/bootaudio-art/raceon-log.txt` | 2026-09-02T11:29 | 79 | 46.2 | 16.8 | 28.3 (8) | audioprime,bcache-census,brfast,gpu |
| `/tmp/bootaudio-art/stalloff18-log-live.txt` | 2026-09-02T11:18 | 7 | 38.2 | 26.2 | 25.4 (5) | audioprime,bcache-census,brfast,gpu |
| `/tmp/bootaudio-art/stalloff18-log.txt` | 2026-09-02T11:19 | 11 | 40.6 | 26.6 | 24.1 (9) | audioprime,bcache-census,brfast,gpu |
| `/tmp/bootaudio-art/stalloff30-log-live.txt` | 2026-09-02T11:21 | 7 | 34.8 | 26.0 | 26.8 (5) | audioprime,bcache-census,brfast,gpu |
| `/tmp/bootaudio-art/stalloff30-log.txt` | 2026-09-02T11:21 | 11 | 39.2 | 26.2 | 25.7 (9) | audioprime,bcache-census,brfast,gpu |
| `/tmp/bootaudio-art/stallon18-log-live.txt` | 2026-09-02T11:20 | 7 | 36.8 | 26.2 | 25.9 (5) | audioprime,bcache-census,brfast,gpu |
| `/tmp/bootaudio-art/stallon18-log.txt` | 2026-09-02T11:20 | 11 | 37.6 | 26.8 | 25.4 (10) | audioprime,bcache-census,brfast,gpu |
| `/tmp/bootaudio-art/stallon30-log-live.txt` | 2026-09-02T11:22 | 7 | 38.6 | 26.0 | 24.5 (5) | audioprime,bcache-census,brfast,gpu |
| `/tmp/bootaudio-art/stallon30-log.txt` | 2026-09-02T11:22 | 11 | 43.2 | 26.8 | 24.9 (7) | audioprime,bcache-census,brfast,gpu |

## 6. Could not find / caveats

- No hardware log files for the 2026-08-21 or 2026-08-28 sessions (only doc summaries in RESUME-2026-08-22.md / RESUME-2026-09-02.md); p10 and at-cap for those sessions are not available.
- No '[fps]' lines in any log; fps derived from '[watchdog]' beats (documented in the report).
- Logs carry no build-id/commit header line; Round 1-3 commits are inferred from branch heads and Desktop TEST-PLAN.txt files. Round 5: report says feat/3ds-hwtest4 @ 978f31d, hwtest4 TEST-PLAN says c73f64c.
- Round 5 hardware log (hw-art-1788441668, identical copy hw-art-1788452996) has gputrace OFF: no [prof]/[rt] ahead-mode receipts (brMain, tk, waitMain) on hardware; the renderthread=2 vs =1 traced A/B requested in hwtest4 TEST-PLAN is not on disk.
- hwtest5 (stereo anchor, 35df5e2) has a TEST-PLAN but no hardware log yet.
- Report ledger puts 'median 46 -> 49' on the brfast row, but RESUME-2026-09-04.md lines 8-9 attribute median 46->49 / p10 39->42 to the campaign-levers OFF/ON traced A/B (logs 1788380826 vs 1788386194, which recompute to 46.0->49.2 / 39.0->41.9). The Round-2 brfast fps toggle segments are small (7 beats off vs 54 on).
- Report Round 1 median 51.4 vs 51.3 recomputed from the report's own 754-beat array (statistics.median); the on-disk log has 756 beats incl. two trailing 0-fps beats (median 51.2).
- Hardware per-lever ms for tmemfast, dspfast, atlas3d, bridgemain, ahead mode, dynlod: not measured on hardware (emulator deltas / projections only).
- dynlod 'hardware MINIMAL was +8-9 fps hand-set' — the underlying Aug-27 hardware log is not on disk (the sdmc rivallod-* logs are emulator runs).
- Triloop packed VBO: no measurement at all (reasoned delta).
- No per-round GPU time on hardware without trace; '[gpu]' lines exist only in traced sessions (gpu ~5 ms/frame).
- hw-art-1788376502 directory is empty (no log).
