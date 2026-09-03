# Golden PCM captures

This directory holds the blessed golden artifacts for the C-R2.3 bit-identical
PCM gate: for each scenario, `<id>.pcm.sha256` (always) and optionally the
`<id>.pcm` itself (large; commit only if the team wants a byte-diff reference).

The golden is the SHA-256 of a `<prefix>.pcm` capture produced by a **legacy
fiber-audio** run (`GDX_AUDIO_THREAD=0`) with **both determinism pins set**
(`GDX_RAND_SEED1` / `GDX_RAND_SEED2`). It is the reference the R2-C delivery swap
must reproduce byte-for-byte: same scenario, same pins, before vs after the
archive-first audio byte path lands.

## Capture procedure (owner-run)

Everything runs from `tools/audio_pcm_harness/`. Python 3, standard library only.

1. **Build the game** (Release):

   ```
   cmake --build build/x64 --config Release --target G-Diffuser
   ```

2. **Prove the scenario is deterministic** (two runs, same pins, identical SHA):

   ```
   python verify_determinism.py \
       --exe  build/x64/port/Release/G-Diffuser.exe \
       --scenario scenarios/01_title_bgm.json
   #   PASS -> prints the golden SHA-256.
   #   FAIL -> names the first divergent sample (frame/channel/value).
   ```

   A FAIL here means a determinism source is still unpinned (a stray `Math_Rand`
   in the audio/AI path, a wall-clock read, an unpinned seed) — fix that before
   blessing, never bless a nondeterministic scenario.

3. **Produce one archival capture and record its golden**:

   ```
   python run_scenario.py \
       --exe  build/x64/port/Release/G-Diffuser.exe \
       --scenario scenarios/01_title_bgm.json \
       --out-dir golden
   #   -> golden/title_bgm.pcm  +  golden/title_bgm.pcm.sha256
   ```

   Keep `golden/<id>.pcm.sha256` (small). The `.pcm` is large (~4 bytes/frame ×
   32000 frames/s); commit it only as a deliberate byte-diff reference.

4. **Regression canary (dedicated audio thread)** — per C-R2.3 the dedicated
   thread run self-compares against ITS OWN golden; it is **not** bit-compared to
   the legacy golden. Capture it with `--extra-env GDX_AUDIO_THREAD=1 --label
   <id>_thread` and keep a separate `<id>_thread.pcm.sha256`.

5. **After the R2-C delivery swap**: re-run `run_scenario.py` for each scenario
   and `compare_pcm.py` the fresh `.pcm.sha256` against the golden here. All
   legacy-path scenarios must match bit-for-bit (the C-R2 exit criterion).

## Scenario status (see scenarios/*.json)

| # | id | status | blocker |
|---|----|--------|---------|
| 1 | `title_bgm`    | ready           | none — zero input, sample-position-driven attract |
| 2 | `gp_course`    | needs-recording | autoinput placeholder; verify no `Math_Rand` in AI/audio first |
| 3 | `ek_course`    | needs-recording | autoinput placeholder; EK disk mounted; last to green |
| 4 | `fault_jingle` | blocked         | trace `leo_fault_dd.c` wall-clock retry gating first |

Do not place a `.sha256` here for a scenario whose status is `needs-recording`
or `blocked` — that would bless a non-deterministic or unrecorded run.
