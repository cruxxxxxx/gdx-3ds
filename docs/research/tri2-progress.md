# TRI2 — per-triangle cost (LOCKED-60 Task C) — progress

Branch `feat/3ds-tri2`, worktree `~/code/gdx-3ds/tri2`, off mainline 256e397.
Brief: `docs/research/locked60-campaign.md` (Task C). Killswitch `[debug] trifast`.
Artifacts: `/tmp/tri2-art/<tag>/{log.txt,autotest/,gdiffuser.ini,status.txt}`; runner
`/tmp/tri2-art/run.sh <tag> <trifast> <seconds>` (lock-honoring, A-mash 30-machine GP).

## M1 — per-phase census (`[tri2]` line)

`lus-tri2-phase-census.patch` (closes the lus series, after triloop-packed-vbo): seven
svcGetSystemTick phase buckets inside GfxSpTri1, exclusive of DRW/IMP/nested-TRI children via
the [prof] child ledger, plus counters (calls/rect/early/fan/memo hit+miss/batch/packed/legacy).
Gate: gputrace (gdx3ds_prof_active) AND verbose (bridge mirrors ini/Dev-Tools verbose into
`gdx_tri2_census_on`). Port side: `port/n64_gfx_bridge.cpp` prints
`[tri2] calls= rect= early= fan= memo=h/m batch= packed= legacy= | pre= state= memo= prg= begin= pack= tail=`
(ms PER FRAME over the 64-race-frame [race-dl] window) and resets.

Phases: pre = reject box / clip pre-scan / clip_rej / cull; state = depth/decal/viewport probes +
cc-option derivation + combiner lookup; memo = s7 tile-extent memo hit copy-out or full
re-derivation (samplers, imports excluded); prg = shader select + ShaderGetInfo/clip memo;
begin = packed batch begin + aux record (first tri of each batch); pack = clamp resolve + 3-vertex
emission; tail = counters + batch-full flush check.

Census run (`/tmp/tri2-art/census`, build 95643a8, A-mash GP, frames to 14070, 172 race
windows, 67 crowd [prof] windows, zero error lines, heap plateau 44.6 MB):

| phase (emulator, ms/frame, 64-frame window means) | pre | state | memo | prg | begin | pack | tail |
|---|---|---|---|---|---|---|---|
| race mean over 172 windows | 3.23 | 0.90 | 1.20 | 0.67 | 0.26 | 2.75 | 0.52 |
| 30-machine start window (calls=71625/64) | 3.79 | 1.61 | 2.32 | 1.26 | 0.71 | 5.49 | 0.95 |

Counters (whole run): calls 8.08M, rect 1.33M, early (reject/cull) 3.36M = 42 %, clip-fan
0.78M, batches 0.67M (~5.8 tris/batch), packed 100 % / legacy 0, **s7 memo hit 0 / miss
3.94M** — the per-tri draw-state memo NEVER hits in race (tri or rect). Root cause: the hit
test requires `!textures_changed[0] && !textures_changed[1]` but the slow path only clears the
flag of a unit the combiner USES; every SETTILE/SETTILESIZE/LOAD arms both, so a single-texture
material leaves [1] armed forever and every draw re-derives extents (software integer divides,
ApplyTileMaskExtent, sampler compares, ShaderGetInfo + GetClipParameters virtual calls).
Emulator caveat: `pre` is inflated by emulated CP15 reads (`thread_local` under -mtp=soft) and
the probe's own svc entry; hardware ranking expected memo-miss > pack > pre.
Go/no-go: GO — memo fix alone projects ~0.6-1.1 ms/frame on hardware (560 calls x 1-2 us),
pack divides/library calls another ~0.5 ms.

## M2 — `[debug] trifast` lever (`lus-trifast-tri-memo-pack.patch`, commit e15625a)

1. s7 memo predicate tests textures_changed only for units the combiner uses (unused unit
   contributes nothing to the banked derivation; its flag stays armed for the next combiner
   that uses it, which misses on the comb key and imports).
2. Packed vertex loop (3DS [triloop] path), each bit-identical: uint8/255 via a 256-entry
   table of the same expression; UV normalise by pow2 extent = multiply by exact reciprocal
   (non-pow2 keeps the divide); shifts 1..10 divide -> multiply by 2^-k; newlib fminf inlined
   (isnan x -> y, isnan y -> x, x<y?x:y — verified against libm.a disassembly); SHADE input
   read from the vertex; fog-line note once per tri.
Killswitch: 3DS ini `[debug] trifast=0` latched in SpReset (per task); desktop opt-in env
GDX_TRIFAST=1. Receipt `[trifast] on= hit= new= miss= dirty= inval= comb= tile= tc0= tc1= |
uvmul= uvdiv= fast=` on the [race-dl] cadence; `[tri2]` now also prints `svc=` (us per
svcGetSystemTick probe) so the census overhead can be subtracted on any host.

Run A (trifast=1, build e15625a) receipt, 30-machine start window:
`[trifast] on=1 hit=19572 new=19572 miss=8360 dirty=8359 inval=0 comb=1 tile=0 tc0=0 tc1=0 |
uvmul=91623 uvdiv=2304 fast=27932` — 70 % memo hits (steady race 80-84 %), EVERY hit is one the
legacy predicate refused, every remaining miss is a real dirtying opcode; 97.5 % of UV
normalisations take the exact-multiply path; 100 % of packed tris through the fast loop.
`[tri2] svc=1.32us`: each svcGetSystemTick probe costs 1.3 us in Azahar, i.e. the 7-lap
census adds ~9 us per GfxSpTri1 call and swamps the emulator phase split (the census
numbers above are probe-dominated; only the counters and the hardware run are trustworthy).
Consequence (commit d8d5f92): the census now also needs `[debug] tri2census=1` (default 0);
the `[trifast]` receipt is unconditional. Runs A/B (census on in both) and A2/B2 (census
off, build d8d5f92) are the A/B pairs.

Hardware instructions: `[debug] console=1 filelog=1 filelog_max_kb=4096 verbose=1 gputrace=1
trifast=1|0` (leave tri2census unset for the A/B; set tri2census=1 once for the phase split,
reading `svc=` first). Watch `[trifast] on=1 hit>>miss new>0` and `[prof] tri`/`[profop] 06`
on frame-aligned windows.

## M3 — emulator A/B (in progress)

Preliminary pair, census ON in both (run A trifast=1 build e15625a vs the census run, whose
trifast-less code path is byte-identical to trifast=0), 220 frame-aligned [prof] windows,
classes by `[gpu] md` (1 = race; 7/8/9/10 = main menu / machine select / settings / course
select), crowd = race windows with nD >= 100:

| class (windows) | tri A | tri B | delta | op06 delta | build delta |
|---|---|---|---|---|---|
| crowd (16) | 14.12 | 15.68 | -1.56 | | |
| race steady (146) | 10.06 | 10.93 | -0.87 | | |
| menu (41) | 9.39 | 11.45 | -2.06 (median -0.99) | | |
(dsp/drw/vtx/imp unchanged within 0.05 ms; br +0.3 is the bridge, untouched by this task.)
Zero error lines in both, heap plateau 44.64 MB both. Drive SHOTs differ in the rankings
portraits + player-machine shading only (rank order / pack collisions differ between runs —
race nondeterminism also seen by the bridgecache agent); the deterministic menu/course/machine
SHOTs are added to the A2/B2 autoinput. `trifast=2` verify mode (commit c59f758) shadows every
fast path with the legacy computation and counts disagreements (`vfy=memo/pack` on the
receipt) — run `runV` queued.

Final pair 1 — run A (trifast=1) vs run B (trifast=0), SAME .3dsx (e15625a), census ON in
both (7 probes/tri, equal in A and B), 214 frame-aligned [prof] windows:

| class (windows) | tri A | tri B | d tri | d op06 | d E4 | d build | d dsp |
|---|---|---|---|---|---|---|---|
| crowd, race nD>=100 (16) | 14.12 | 15.77 | -1.65 (median -1.73) | -1.42 | -0.25 | -1.83 | -0.07 |
| race steady (140) | 10.04 | 10.97 | -0.93 | -0.78 | -0.13 | -0.91 | -0.01 |
| menu md 7/8/9/10 (41) | 9.39 | 11.58 | -2.19 (median -1.04) | -3.25 | -0.35 | -3.57 | +0.05 |
| other (17) | 8.69 | 9.47 | -0.78 | -0.73 | -0.19 | -0.69 | +0.06 |

Receipt B (off): `[trifast] on=0 hit=0 new=0 miss=25531 dirty=7472 inval=0 comb=10 tile=0
tc0=161 tc1=17888` — 70 % of all misses are textures_changed[1] on a unit the combiner does
not use, i.e. exactly the predicate bug. Errors: none in either. Heap plateau: A 44.64 MB,
B 44.51 MB (both flat, last == max). Drive SHOTs differ in the same regions as before (rank
portraits, pack-dependent player shading).

Build gotcha (cost one A/B pair): `cmake --build build-3ds --target G-Diffuser-3DS-cia` relinks
the .elf and rebuilds the .cia but does NOT regenerate `G-Diffuser-3DS.3dsx`; always run the
default target first (`cmake --build build-3ds -j8`) and then the cia target. Runs A2/B2
therefore used the e15625a .3dsx (census still on) — a second sample of pair 1, not the
census-off pair. Census-off pair = runs A3/B3 (build d8d5f92+, `trifast4.3dsx`), verify run =
runV (trifast=2).

Pair 2 (A2/B2, same e15625a .3dsx, census on): crowd -1.31 (median -1.49), race steady -0.91,
menu -2.19 (median -1.05), other -0.69 ms/frame tri; op06 -1.02/-0.77/-3.25/-0.45; zero error
lines; heap 44.63 vs 44.43 MB plateau (flat in both). SHOT parity with the added deterministic
screens: menu/course/machsel/settings `_scan.bmp` differ ONLY in a 27x13 px box at
x309-335/y18-30 (the pulsing "OK?" prompt, max channel delta 21/255, mean 13 — animation phase;
the remaining 287.8 KB byte-identical); drive shots differ in rank portraits / pack-dependent
player shading as before.

## M4 — verify run (trifast=2, build c59f758, `runV`)

183 receipt windows: `vfy=0/0` in every window — 7.10 M fast-loop triangles re-emitted by the
legacy loop into scratch with 0 differing floats, and every new-predicate memo hit (~17.6 k per
window, ~3.2 M total) re-derived through the slow path with 0 disagreements in
tm/extents/effective tiles. Zero error lines. (149a515 fixes the verify-mode hit counter that
underflowed on that run's receipt; modes 0/1 untouched.)

## M5 — census-off A/B (runs A3/B3, final code, build 12:38 = 149a515 modes 0/1 identical)

250 frame-aligned windows, `[debug] tri2census` unset (no probes), same .3dsx:

| class (windows) | tri on | tri off | d tri | d op06 | d E4 | d build | d dsp / d drw |
|---|---|---|---|---|---|---|---|
| crowd, race nD>=100 (18) | 7.07 | 8.54 | -1.46 (-17 %) | -1.02 | -0.29 | -1.37 | +0.07 / 0.00 |
| race steady (169) | 5.39 | 6.34 | -0.95 (-15 %) | -0.79 | -0.14 | -0.89 | 0.00 / 0.00 |
| menu md 7/8/9/10 (41) | 9.68 | 11.94 | -2.26 (-19 %) | -3.36 | -0.36 | -2.29 | +0.04 / 0.00 |
| other (22) | 4.66 | 5.53 | -0.87 | -0.92 | -0.36 | -1.13 | -0.08 / -0.02 |

Receipt off: `hit=0 miss=25181 dirty=7443 comb=10 tc0=161 tc1=17567` (70 % tc1); on:
`hit=18239 new=18239 miss=7440 ... uvmul=84855 uvdiv=2304 fast=25679 vfy=0/0`. Zero error
lines both; heap flat (on 44.58 MB, off 44.73 MB, last == max). SHOTs: menu screens differ
only in the pulsing "OK?" box (406-419 bytes), drive shots in rank/pack regions.
Artifacts: `build-3ds/port/3ds/G-Diffuser-3DS.{3dsx,cia}` (12:38/12:39) + copies in
`/tmp/tri2-art/`. Clean-submodule roundtrip of the 34-patch README list: identical, 12 files.
