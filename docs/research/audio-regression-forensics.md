# Audio "regression" forensics — 2026-08-21 emulator silence

## Verdict

**There was no regression on feat/3ds-hwaudio. m1 never had an audible emulator
build.** The audible ~13:00 run was misattributed to the m1 12:35 build; it came
from the **audiocontent** stream (feat/3ds-audiocontent) — and specifically from
that worktree's *uncommitted, post-commit* iteration of the rom-audio-driver port,
built at 13:48. Two nested attribution errors had to be unwound:

1. The audio-maker was never on m1: commit `2852eaf` ("port the rom-side audio
   driver to host — root cause of total silence") lives only on
   feat/3ds-audiocontent and was never merged.
2. Even `2852eaf` alone is NOT sufficient: its committed
   `decomp-3ds-rom-audio-port.patch` (736 lines, 13:23) is an earlier iteration.
   The audible 13:48 binary was built from the worktree state that kept evolving
   after the commit (~680 insertions over the sanctioned stack across
   external.c, lib/{audio.h,heap.c,load.c,seqplayer.c,thread.c}). m1 with only
   the committed patch still produced the empty ~27-command mix
   (`[audio-hle] cmds=27 adpcm=0`, `nz=0`) — verified live at 17:08.

Every afternoon "restoration to 2d36485" on m1 therefore faithfully restored a
silent-by-design baseline, which is why every single-variable elimination (ndsp
TU restore, C_FLAGS restore, save park, dspfirm check) kept reading silent.

## Root cause of the silence itself

The 3DS build sets `GDX_EXPANSION_KIT=OFF` (port/3ds/game/CMakeLists.txt:23) and
compiles the ROM-side audio driver `decomp/src/audio/rom/` — a driver the desktop
port **never builds** (desktop is EK=1 → `src/audio/disk/`), so it had received
none of the disk driver's PORT work. Chief kill: `Audio_Update` — the game's
entire per-frame audio pump (engine SE, SE stacks, BGM state machines) — was
stubbed `return` under `#ifdef PORT` with a stale "Audio_Init is skipped on PORT"
rationale. Result: healthy producer/push counters, `nz=0 ck16=0 or=0x0000`
forever. Plus LE AudioCmd union placement, big-endian font relocation, seq
envelope conversion, cmd-ring wiring, and heap/load fixes (the post-commit
iteration) — all mirrored from the disk driver's PORT work.

## Timeline (2026-08-21)

- **12:08–12:11** — `061b0d1`/`eb75dee` ([audio-out] receipts, null-sink unmask),
  `9743e79`/`a4c66ea` (FPS HUD). audio2 worktree artifact (12:08) survives.
- **~12:35** — m1 build (input-remap content, later committed 14:02 as
  `0283b4b`/`2d36485`). Silent by design (stubbed rom driver); the brief credited
  it with the audible run — the central misattribution.
- **12:49** — `dspfirm.cdc` added to Azahar sdmc (MD5 `21fc3d5f…`); game-side
  `ndspInit` starts succeeding in the emulator.
- **~13:00** — USER HEARS MUSIC. This was the audiocontent worktree's rom-driver
  port under test (pre-commit working tree), not m1.
- **13:23** — `2852eaf` committed on feat/3ds-audiocontent; the worktree kept
  evolving past the committed patch (audio.h/heap.c grew; external/thread/load/
  seqplayer diverged).
- **13:48** — audiocontent artifact built from that NEWER state = the only
  surviving audible-class binary (used below as positive control).
- **14:01–14:03** — input worktree rebuilt; m1 CIA target built; the Desktop
  hardware package .3dsx/.cia overwritten (destroying the intended 12:12
  positive control). Both silent-by-design; their silence was misread as "the
  audible-era binary went silent" → launched the false regression hunt.
- **14:44** — `28c02f2` (m1): explicit ndsp master state + ch7 beeper + LINEAR
  interp (hardware prongs). Output-side only; innocent of mix-content silence.
- **15:23** — `6a63bc5` (m1): partial rom-lib sync (lib/thread.c only — no
  Audio_Update un-stub → still silent) + core0 bisect; reverted 16:09
  (`94169b6`), TU hand-restored, romthread patch un-applied.
- **16:37–16:44** — user's manual race run on the 16:24 m1 build: valid verdict
  run (187 [race-dl], sub=16320), `or=0x0000` — correct for a stubbed pump.

## Eliminations verified during forensics

- m1 ndsp TU was byte-identical to `2d36485` (git diff empty).
- m1 decomp tree == sanctioned 7-patch stack EXACTLY (scratch-worktree apply +
  diff-of-diffs); no romthread residue (`6a63bc5`'s patch touched only
  `src/audio/rom/lib/thread.c`, cleanly un-applied).
- m1 libultraship tree == sanctioned 23-patch stack EXACTLY (same method).
- CMakeCache: default `-g -O2`, no `GDX3DS_AUDIO_PRODUCER_CORE0` residue.
- dspfirm.cdc present/correct; gdiffuser.ini `console=1 filelog=1 diag_audio=1`;
  o2r archives untouched (Aug 13).
- Save-file sound theory dead: F-Zero X SOUND option is SURROUND(0)/MONO(1)
  only; mono pans 0.707/0.707 (not silence) and `Audio_SetOutMode` is a PORT
  no-op. (The earlier "save parked, still silent" test was doubly meaningless:
  silent-by-design binary, and menu-only runs read silent trivially.)
- `gResetTimer` mute-gate probe (concurrent session's hypothesis): logged 0 at
  first producer tick — innocent.

## Receipts

### Positive control — audiocontent 13:48 artifact, scripted race run (16:56)

Audible from the title screen on (rom pump feeds SE/BGM immediately):

    t=20s   sub=1001  nz=415   ck16=0xD5517AE3 or=0xFFFF
    t=100s  sub=6038  nz=5360  ck16=0xE6CD275B or=0xFFFF  race-dl=9
    t=200s  sub=11984 nz=11306 ck16=0x79C79883 or=0xFFFF  race-dl=81

race-dl and or=0xFFFF in the same log; nz tracks sub ≈1:1. Harness positively
detects music (protocol gate passed).

### Discriminating run — m1 + committed-patch-only (17:08, concurrent session)

`[audio-rom] Audio_Update pump LIVE` + `[audio-reset] gResetTimer=0`, yet
`[audio-hle] cmds=27 adpcm=0 envmix=0` and `or=0x0000` through race-dl=24 →
the committed patch alone is insufficient; the audible delta was still
uncommitted in the audiocontent worktree. This pinned attribution error #2.

### Concurrent-session interference (17:03–17:10), for the record

A parallel stale agent session was found still bisecting the disproven
"28c02f2 is the breaker" hypothesis. It ran, inside this worktree:
`git stash` + `git revert 28c02f2` (rogue commit `6ccb98f`), a rebuild, sdmc log
deletions, and Azahar relaunches mid-verdict-run; its takeover run at 16:57
produced the `play=/pos=/mvol=` receipt-variant lines seen in the control
monitoring tail. Neutralized non-destructively: `f8a260c` reapplies `28c02f2`
(keeping LINEAR interp, explicit ndsp master state, ch7 beeper — the hardware
bisect fixes), its poll loops were stopped, and the final verdict run used a
private binary copy + a self-reasserting launcher.

### m1 verdict run — complete fix

State: feat/3ds-hwaudio @ `f8a260c` + merge `c24500f` (2852eaf) + regenerated
`decomp-3ds-rom-audio-port.patch` (1015 lines — the audiocontent worktree's
final audio state over the sanctioned stack; roundtrip-verified exact).
Verdict log: `/tmp/verdict-log-m1-fixed2.txt`. PASSED (17:14–17:22 run,
scripted drive into race, one interference re-assert at t=60s then clean):

    [audio-rom] Audio_Update pump LIVE (rom driver, first game-thread tick)
    [audio-rom] cart boot bootstrap: Audio_GuitarSeqStart (INIT_SEQPLAYER guitar + SE)
    [fontconv] font=1 drums=0 insts=1 samples=1 loops=1 books=1 envs=1
    t=160s  race-dl=2   sub=6328  nz=5652  or=0xFFFF   <- race reached, audible
    t=500s  race-dl=257 sub=27551 nz=26847 or=0xFFFF   <- ~5.7 min in-race, audible

    race-dl-count=257  nonzero-audio-out-count=85
    sub=27551 (protocol bar >12000: passed 2.3x); or=0xFFFF and race-dl in the
    SAME log; nz/sub ≈ 97%; TU receipts show interp=linear play/paused/pos/mvol
    (28c02f2 hardware semantics present in the audible binary).

## Restoration applied (this branch)

1. ndsp TU kept at 28c02f2 semantics (LINEAR interp, explicit master state, ch7
   beeper) — required for the hardware bisect. (`f8a260c`)
2. Merged `2852eaf` (rom-driver port, [audio-hle] receipts). (`c24500f`)
3. Regenerated `decomp-3ds-rom-audio-port.patch` from the audiocontent
   worktree's audible final state; applied to m1's decomp working tree on top of
   the sanctioned 7-patch stack; roundtrip-validated on a scratch worktree.
4. Clean rebuild of build-3ds.

## Lessons

- Provenance first: before bisecting "what broke X", prove which artifact ever
  exhibited X. Here BOTH the branch attribution and the commit attribution of
  the good run were wrong; every downstream elimination inherited the error.
- A committed patch is not the working tree: submodule patch-stack workflows can
  leave the load-bearing delta uncommitted. The scratch-worktree stack-diff
  (apply sanctioned patches to a detached worktree, diff-of-diffs against the
  live tree) is cheap and decisive for detecting this.
- Single-variable eliminations are worthless while a masking breaker is present;
  pass the positive-control gate before trusting any silent verdict.
- Serialize agents on exclusive resources: two agents bisecting the same
  emulator/branch produced rogue commits, clobbered artifacts, and false
  attributions. The singleton lock only works if every agent honors one
  protocol (dir-mkdir vs file-touch collided here).
