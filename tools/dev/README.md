# `tools/dev/gdx-dev` — one CLI for the fork's dev loop

Python 3, standard library only. No pip, no venv, no `PYTHONPATH`: from a fresh worktree,
`tools/dev/gdx-dev <subcommand>` just works.

**Why it exists.** Every agent on this fork used to re-derive the same procedures from
prose, write them into `/tmp/*.sh`, and lose them when the session ended — `RESUME.md` §3
records the cost ("all `/tmp` helper scripts are gone", "agents have killed each other's
runs and deleted each other's artifacts", "no committed diff tool"). Those procedures now
live here as committed code. Spec: `docs/research/native/05-devcli-plan.md`.

**If a brief tells you to run shell against Azahar, run `gdx-dev` instead.** If gdx-dev
cannot do what you need, say so in your report — do not write a `/tmp` script.

## Exit codes (uniform across every subcommand)

| code | meaning |
|---|---|
| 0 | OK / PASS |
| 1 | FAIL — an assertion failed (`[fatal]`, no heartbeat, frames differ) |
| 2 | usage error |
| 3 | preflight / environment problem (no Azahar, no `log_filter=*:Debug`, no `fzerox.o2r`) |
| 4 | lock timeout — another agent holds Azahar |
| 5 | staging failure (SD, ini, replay) |
| 6 | nothing parsed (empty or receipt-free log) |
| 7 | delegate tool missing (the `oracle` or `hwlink` track has not merged) |

## Global flags

`--track NAME` (default: worktree basename) · `--run-root DIR` (default `/tmp/gdx-runs`) ·
`--json` (machine-readable result on stdout) · `--quiet` · `--dry-run` (prints exactly what
the invocation would do and does **none** of it: nothing launched, written, downloaded or
locked).

> `retro run --dry-run` used to be a lie: the flag was parsed, documented, and read by
> nobody, so it booted a real instance on a real private SD (NEXT.md B45). It is now the
> first thing `retro run` acts on and `RetroDryRunTest` detonates on every side effect.

`GDX_TRACK` and `GDX_RUN_ROOT` work as environment equivalents.

## Run directories

```
/tmp/gdx-runs/<track>/<YYYYmmdd-HHMMSS-slug>/
    manifest.json   schema-1, every field required (see below)
    log.txt         the guest's sdmc filelog, snapshotted every 20 s
    emu.log         THIS RUN'S slice of the shared azahar_log.txt (byte offset at launch)
    console.txt     the emulator process's own stdout/stderr
    shots/          BMPs from `SHOT <label>` events
    capture/  prof/
    VERDICT         "PASS <detail>" / "FAIL <detail>"
```

`/tmp/<track>-art` is a symlink to the newest run directory, which is what satisfies
PROTOCOL.md's "snapshot your artifacts every 25 s" with no per-agent bookkeeping.

`manifest.json` promotes **`gputrace` to a top-level field**, out of `ini`, on purpose:
PROTOCOL.md requires every perf claim to state it, so no report can quietly omit it.

### `tree`: which tree produced this run

PROTOCOL.md: **a parity cell is a claim about a tree.** Every manifest therefore carries

```json
"tree": {
  "branch": "native/tooling2", "head": "1727d579…",
  "base": "native/main", "base_head": "1727d579…",
  "branch_point": "1727d579…", "behind": 0, "ahead": 4,
  "dirty": false, "current": true
}
```

`branch_point` is the merge-base against `native/main` (override with `GDX_BASE_BRANCH`)
and `behind` is how many commits on main this tree does not have. `runs.validate`
**refuses** a manifest with no branch point, so a parity number cannot be quoted out of a
run without the tree it was measured on. Every run prints the receipt line

```
tree: native/tooling2@1727d579, branch point 1727d579 off native/main@1727d579 (+4/-0)
```

and WARNS when `behind > 0`, because that is exactly the condition under which a claim of
"pre-existing on `native/main`" cannot be checked from where the branch stands: a bug
fixed on main after the branch point is still present in the branch and looks identical to
one the branch found. That cost one agent a full bisect of B42, a bug already fixed. Quote
the receipt line, or forward-merge and re-gate.

---

## `gdx-dev build`

cmake + Ninja + ccache wrapper. Prints the build id, configure/build seconds, artifact
path and sha256.

```sh
tools/dev/gdx-dev build
tools/dev/gdx-dev build --reconfigure --jobs 8
tools/dev/gdx-dev build --touch-patched      # stale-object trap check
```

Two traps it owns for you:

- `GDX3DS_BUILD_ID` is a **configure-time** value (`port/3ds/CMakeLists.txt:59-78`). After
  a merge, a plain rebuild keeps the *old* id and every report you write cites the wrong
  build. `build` compares HEAD against the id baked into the generated build system and
  reconfigures automatically, with a warning.
- `--touch-patched` touches every source named by `port/3ds/patches/*.patch` and asserts
  the `.o` files actually rebuilt (PROTOCOL.md line 11).

`dirty` in the manifest ignores submodules — the 48-patch stack means they are always
modified, so counting them would mark every build dirty and the flag would say nothing.

## `gdx-dev emu run`

The whole emulator lane in one command: take the lock → preflight → merge the ini → stage
the replay → mark the log offset → launch → snapshot every 20 s → stop gracefully →
parse → write `VERDICT`. Exits 0 on PASS, 1 on FAIL.

```sh
tools/dev/gdx-dev emu run build-3ds/port/3ds/G-Diffuser-3DS.3dsx \
    --replay storm2 --ini mylever=1 --seconds 360 --shots --name lever-on
```

| flag | effect |
|---|---|
| `--replay ID\|FILE` | stage a replay from `tools/dev/replays/` (see below) |
| `--ini K=V` | merge one ini key; `[debug]` by default, `section.k=v` to target a section |
| `--seconds N` | run **ceiling** in seconds (default 60; see `retro run` for the staged-replay default) |
| `--gputrace` | set `[debug] gputrace=1` — inflates profiles, recorded in the manifest |
| `--name S` | slug for the run id |
| `--shots` | collect `autotest/*.bmp` written by `SHOT` events |
| `--3d [MODE]` | render in stereoscopic 3D so the guest's right-eye pass runs — see **Stereoscopic 3D** below |
| `--depth-3d PCT` | 3D depth for `--3d`, 1..100 (default 100) |
| `--lock-wait S` | how long to wait for another agent to finish (default 900) |
| `--no-replay-quit` | play PAST the replay's last tick and end on `--seconds` (`[debug] replay_quit=0`) |
| `--dry-run` | print what the run would do and do none of it |

Always sets the measurement ini set (`console=1 filelog=1 filelog_max_kb=4096
verbose=1`). `gputrace` is deliberately **not** in that set.

**The `[fatal]` scoping fix.** `tools/ci-3ds.sh` greps the whole shared `azahar_log.txt`,
so another agent's crash — or your own previous run's — fails your run. `emu run` records
the log's byte offset immediately before launch and slices `emu.log` from there, so the
verdict only ever sees this run's bytes. (If Azahar truncated the log meanwhile, the slice
restarts at 0 and says so.)

## `gdx-dev emu lock` / `unlock` / `status`

```sh
tools/dev/gdx-dev emu status              # who holds Azahar, and is one running?
tools/dev/gdx-dev emu lock --hold         # daemonised toucher for interactive work
tools/dev/gdx-dev emu unlock
```

`emu run` takes the lock itself — you only need these for interactive driving.

The lock is `/tmp/azahar.lock` (PROTOCOL.md), and it protects the **shared user-data
directory**: Azahar has no single-instance lock, but there is exactly one
`~/Library/Application Support/Azahar` and no flag to move it. gdx-dev writes
`owner.json` (track, pid, start time) into the lock and heartbeats it every 20 s, so it
steals a lock only when the owning process is **dead** *and* stale — unlike
`ci-3ds.sh`, which steals on age alone and can kill a legitimate long run. A lock with no
`owner.json` is a legacy bash holder and gets ci-3ds.sh's conservative 900 s threshold.

## `gdx-dev emu shot` / `emu click`

```sh
tools/dev/gdx-dev emu shot --out /tmp/devcli-art/title.png
tools/dev/gdx-dev emu click 640,520
```

Azahar has no `--screenshot` flag (confirmed absent from `azahar --help` on 2126.0), so
capture is host-side: `tools/dev/mac/wid.swift` finds the `CGWindowID`, `screencapture -l`
grabs it. Both screens land in one image only with `layout_option=0` — `shot` checks and
warns. `click` drives the host-side bottom-screen touch menu, which guest autoinput
cannot reach. Both helpers are vendored under `tools/dev/mac/` because the `/tmp` copies
have been lost at least three times.

## `gdx-dev prof`

Every table agents used to hand-assemble with grep and awk.

```sh
tools/dev/gdx-dev prof <run_id>                         # fps + sections + ops + errors
tools/dev/gdx-dev prof /path/to/log.txt --fps
tools/dev/gdx-dev prof <run_id> --by-vtxn               # crowd vs menu buckets
tools/dev/gdx-dev prof --ab <run_on> <run_off> --by-vtxn
tools/dev/gdx-dev prof <run_id> --fail-on-error         # exit 1 on any error receipt
```

- `--fps` — p10/p50/p90 from `[watchdog] frame=N(+D)` deltas ÷ 5 s
  (`docs/3DS-HARDWARE.md:177-180`). The **first beat is dropped**: its `(+N)` covers boot
  and asset load, not a frame rate, and including it drags every percentile down.
  Percentiles are nearest-rank, so a printed p50 is a value that actually occurred.
- `--sections` — mean ms/frame per `[prof]` section, with `[prof!]` storm windows on
  their own row.
- `--by-vtxn` — buckets `[prof]` windows by **`nD`** (draws/frame), the campaign's A/B
  convention: `crowd` is `nD>=100`, `menu` is `nD<16`. (The flag keeps the `--by-vtxn`
  name from the spec; mean `vtxN` is printed per bucket alongside.)
- `--ops` — `[profop]` top opcode handlers, mean ms/frame and calls/frame.
- `--ab A B` — frame-aligned delta table plus both runs' error counts. **Refuses** to
  compare runs whose manifests disagree on `gputrace` (it inflates profiles) or on
  `build.id` (a lever A/B must use one build); `--force` overrides the build check.

Parsed receipts: `[prof]` `[prof!]` `[profop]` `[gpu]` `[fill]` `[rt]` `[watchdog]`, and
the error census `[fatal] [fatal-trace] [gfxfail] [gdl-bad] [gdl-miss] [datafail]
[texdiag] bad_alloc`. Parsing is a tolerant `key=value` scan, not a positional regex, so
an added column does not break it.

## `gdx-dev runs` / `gdx-dev report`

```sh
tools/dev/gdx-dev runs list
tools/dev/gdx-dev runs show <run_id>
cd "$(tools/dev/gdx-dev runs path <run_id>)"
tools/dev/gdx-dev report <run_id> --markdown >> docs/research/native/<track>-progress.md
```

`report --markdown` prints the block to paste into a progress file: build id, gputrace
state, replay, ini, verdict, fps percentiles and the section table — everything a claim
needs to cite under PROTOCOL.md. With no run id it uses the newest run.

## `gdx-dev sd stage` / `sd pull`

The physical SD-card loop. **Dry-run by default** — it prints the plan and writes nothing
until `--apply`, because this is the user's real card.

```sh
tools/dev/gdx-dev sd stage build-3ds/port/3ds/G-Diffuser-3DS.3dsx           # dry run
tools/dev/gdx-dev sd stage path/to/stage-dir --ini mylever=1 --wipe-log --apply
tools/dev/gdx-dev sd pull /tmp/devcli-art/hw-run
```

Guards: every destination is forced under `3ds/` on the card, and the card's
`gdiffuser.ini` is **merged**, never rewritten, so the user's own keys (`border_mode`,
`deadzone`, stereo) survive. Ejects with `diskutil` unless `--no-eject`.

## `mkdisk64.py` — the synthetic 64DD disk image (B11c)

`tools/dev/mkdisk64.py` writes a **structurally correct 64,931,840-byte `.ndd` containing NO
game content**, so the Expansion Kit's disk path — the block reader, the LBA arithmetic, the
cache, the CRC pass and every refusal path — can be tested and CI-gated with nothing
copyrighted in the tree and no dump anywhere near it.

```sh
tools/dev/mkdisk64.py build out.ndd --seed 20260924 [--disk-type 0..6]  # + out.ndd.manifest.json
tools/dev/mkdisk64.py geometry --disk-type 0        # the derived zone/LBA table, writes nothing
tools/dev/mkdisk64.py truncate in.ndd short.ndd --bytes 0x100000
tools/dev/mkdisk64.py corrupt  in.ndd bad.ndd --at 0x200000 --len 64
```

Every structural fact is read out of THIS repository (the decomp's libleo tables and MFS
headers, the port's own constants) and the geometry **self-validates** by reproducing four
independent in-tree constants from its own per-block walk — including all seven
`LEORAM_BYTE[]` entries. The manifest records the seed, the sha256, the CRC-64 and an explicit
implemented / guessed / deliberately-absent breakdown; read it beside the image.

`tools/ci-3ds.sh` generates one into `build-3ds/host-tests/diskimg/` (cached, ~10 s) and runs
`gdx_disk_stream_host_tests` against it. See `docs/research/native/diskstream-progress.md`.

### Running an EK build against a REAL disk (B11d, `ekdrive`)

```sh
# the image goes on the instance's PRIVATE virtual SD, never on a real card
MED=~/Library/Application\ Support/gdx-retro/<instance>/saves/Azahar/Azahar/sdmc/3ds/gdiffuser/media
ln <scratch>/baserom.jp.ek.ndd "$MED/baserom.jp.ek.ndd"    # 64,931,840 B, block-linear .ndd

gdx-dev retro run build-3ds-ek/port/3ds/G-Diffuser-3DS.3dsx --instance <instance> \
  --gfx software --replay ek-courseedit \
  --ini debug.disk_stream=2          `# stream-only: NO 61.9 MB resident image` \
  --ini debug.disk_stream_report=16  `# LEO reads per [diskstream] receipt (default 512)` \
  --ini debug.disk_stream_cache_kb=512
```

Three things that cost `ekdrive` time and are not obvious:

- **`-DGDX_ALLOW_JP_INPUTS=ON` is needed at CONFIGURE time for a Japanese `.ndd`**, and before
  B11d it silently did nothing on 3DS. Without it only `baserom.translated.ek.ndd` is even
  opened, and the failure reads `[leo] no disk image found` with no mention of a gate.
- **`disk_stream_report`'s default 512 can exceed a whole session's LEO read count**, so a
  measurement run gets exactly one receipt — taken at the mount, before the traffic. Lower it.
- **`retro run` does not clear the guest's `autotest/` between runs**, so `shots/` can hold a
  previous run's BMPs under plausible names. Delete
  `<instance>/saves/Azahar/Azahar/sdmc/autotest` before any run you intend to `framediff`.

The receipt's `uniqBlk` / `wsKB` / `refetch` are the working set and what the cache failed to
hold — those, not `hit%`, are what `disk_stream_cache_kb` is chosen against. See
`docs/research/native/ekdrive-progress.md`.

## `gdx-dev diff` / `gdx-dev hw` — delegations

These are thin on purpose and must stay that way: they never implement image comparison
or the 3dslink path.

```sh
tools/dev/gdx-dev diff a.bmp b.bmp --sheet      # -> tools/dev/framediff.py   (oracle)
tools/dev/gdx-dev diff hashes runA.log runB.log # -> tools/dev/fhdiff.py      (oracle)
tools/dev/gdx-dev hw push --ip 192.168.1.50     # -> tools/dev/hw.py          (hwlink)
```

**Clearing an ini key on hardware needs `--delete`, not an empty value.**

```sh
tools/dev/gdx-dev hw ini --delete debug input_replay --save   # remove the key
tools/dev/gdx-dev hw replay --off                             # the same, as one verb
```

An empty value is still an *entry*: the guest getters return `""` instead of the key's
DEFAULT, and `INI SAVE` writes the key straight back out — so `ini debug input_replay ""`
left a replay armed across reloads (and the control protocol dropped the empty token
anyway, so the set never happened). `INI DEL <section> <key>` removes the entry;
`gdx3ds_config_unset` returns 0 when a key was removed and 3 when there was none, and the
guest logs `[net] ini del [<section>] <key> rc=<n>`.

If the peer track has not merged into your branch, they exit **7** and name the track and
the plan section to ask for. That is the signal to merge or to ask — not to reimplement.

## `gdx-dev hw-run` — unattended hardware A/B

One command measures a branch on the console with nobody watching: a config matrix, the
canonical replay, per-config ini + reload, a marker-delimited log slice per config, a
manifest per slice and a summary table with an A/B delta. It replaces the throwaway
`artifacts/hwprof.sh` / `hwfps3.sh` pair that produced the 2026-09-20 hardware numbers.

```sh
tools/dev/gdx-dev hw-run \
    --config 'native.all=0,native.interp=0' \
    --config 'native.all=1,native.interp=1' \
    --replay race-mutecity-start --repeats 3 \
    --3dsx path/to/G-Diffuser-3DS-<track>/G-Diffuser-3DS.3dsx

tools/dev/gdx-dev hw-run --config ... --gputrace      # [prof] ms (inflated, and stated)
tools/dev/gdx-dev hw-run --config ... --dry-run       # the exact sequence, no sockets
```

Preconditions: the console is on the WiFi link (`tools/dev/gdx-dev hw ping` answers
`OK PONG`), `[net] enabled = 1` on the card, and `tools/dev/hw.ini` holds its IP.

- `--config SPEC` (repeatable) — one config, `section.key=value` comma-separated. A bare
  key is `[debug]`. **An empty value DELETES the key** (`--config 'debug.input_replay='`),
  which is the only way to clear a lever: an empty value is still an entry and survives
  `INI SAVE`.
- `--repeats N` — the **outer** loop, so thermal or cache drift over a long session hits
  every config equally instead of loading onto whichever ran last.
- `--seconds` is a **ceiling**, not a sleep: a slice ends at the replay's own `[rec] end`.
- `--gputrace` sets `[debug] gputrace=1`, records it in every manifest and prints it in
  the header. PROTOCOL.md line 22: it inflates every millisecond, so it must be stated.
- `--base-ini` / `--restore` — the ini applied to every config, and once at the end.
  The default restore **deletes** `[debug] input_replay`, so a replay cannot survive the
  session, and turns `gputrace`/`det` off.

**How a config's log is delimited.** Not by byte offset. Before each slice the runner
makes the guest write its own boundary — `INI debug hwrun_mark <token>` is echoed by the
console as `[net] cmd INI [debug] hwrun_mark=<token> rc=0`
(`port/3ds/net/gdx3ds_net.c`) — and the key is deleted immediately, so no `INI SAVE` can
persist it. The boundary therefore travels in the same stream as the data and cannot
drift when another process appends to the log. When `hw-run` also manages to bind
port 17491 it rotates the sink per slice as well and cross-checks the two.

The byte-offset version did drift: `artifacts/hwfps3-r1-all1-interp1.log` opens with the
*previous* config's `race03`/`race04` SHOTs.

**What it writes.**

```
/tmp/gdx-runs/<track>/<run_id>/
    manifest.json        lane=hw, result.slices = every slice id
    hw-link.log          the whole session, if we owned port 17491
    cfg00-rep01/
        log.txt          THIS config's slice, marker to marker
        manifest.json    schema-1: build sha256, ini, replay, gputrace, uptimes
        VERDICT
```

Each slice manifest records the console's build/frame/uptime from `PONG` before and after
and the Mac's uptime, so a console that rebooted or a host that slept mid-matrix is
visible afterwards rather than being an unexplained outlier.

**Completion.** A slice PASSes only with the replay's `[rec] end` receipt *and* every
scheduled SHOT — the rule `gdx-dev emu run` already uses. A guest that dies mid-replay
keeps logging an advancing heartbeat and zero `[fatal]` lines, so without this a crash at
frame 433 of 6001 reads as a healthy run. This matters in practice: two of the three
configs in the published 2026-09-20 `[prof]` table never reached `[rec] end` — the fixed
`sleep 150` reloaded on top of them. `tests/fixtures/hw-prof-all0-interp0.log` is that
slice, and a test asserts it is detected as truncated.

**The summary.** `[prof]` sections by `--by-vtxn` nD bucket per config (repeats pooled),
in-race fps percentiles, the error census, and an A/B delta table of config 0 against
every later config. fps is pooled from `[watchdog]` deltas over the first 60 s after the
`race00` SHOT, counted in **beats, not host timestamps** — the log link flushes in bursts
(beats 30-32 of the 2026-09-20 session share one host second), so a wall-clock window
would cut wherever the network hiccuped.

Exit 1 if any slice is incomplete or carries a `[fatal]`; 7 if `tools/dev/hw.py` (the
hwlink track) has not merged into your branch.

## `gdx-dev retro` — the RetroArch lane

Opt-in second lane whose point is that it has **no singleton**: each instance gets its own
config, its own private virtual SD, its own log and its own UDP port, so N agents can run
at once. See `docs/research/native/devcli-progress.md` for the spike verdict and the exact
working configuration; Azahar Qt remains the default lane.

```sh
tools/dev/gdx-dev retro run build-3ds/port/3ds/G-Diffuser-3DS.3dsx --replay storm2
tools/dev/gdx-dev retro run <3dsx> --replay storm2 --dry-run   # says everything, does nothing
tools/dev/gdx-dev retro status            # who owns which instance; flags orphans
tools/dev/gdx-dev retro reap              # kill RetroArch whose driver is gone
tools/dev/gdx-dev retro reap --prune      # also delete idle instance dirs >2 days old
tools/dev/gdx-dev retro reap --prune --days 7 --dry-run
tools/dev/gdx-dev retro setup --instance a
tools/dev/gdx-dev retro cmd SCREENSHOT --instance a
tools/dev/gdx-dev retro run <3dsx> --3d   # stereoscopic — see "Stereoscopic 3D" below
```

### Renderer (`--gfx`)

`--gfx vulkan|opengl|software`, case-insensitive. With no flag the lane tries Vulkan, then
OpenGL, then Software.

**`OpenGL` is not a legal `citra_graphics_api` value on the pinned 2126.0 core** (verified
by extracting the core's own option strings, emufix 2026-10-16: the "Graphics API" option
declares exactly `Vulkan` and `Software`). `--gfx opengl` still changes RetroArch's OWN
`video_driver` (to `glcore`, which is real and does change the host presentation backend),
but the CORE's internal renderer silently falls back to its option default (`Vulkan`)
either way — unchanged by the core_options fix below, since this was *already* the
behaviour (the option was never read at all before the fix, and now that it is read, an
illegal value still falls back to the default). Only `vulkan` and `software` are honoured
by the core itself.

**Recommendation: leave it alone — the default (Vulkan first) is the stable one here.**
Measured 2026-09-13, five consecutive 60 s boots per renderer on clean instances:

| renderer | runs PASS | boot attempts | early crashes |
|---|---|---|---|
| Vulkan | 4/5 | 5 | **0 (0%)** |
| OpenGL | 5/5 | 7 | 2 (29%) |

Vulkan did not crash once; OpenGL crashed on 2 of 7 boots and only reached 5/5 because the
retry rescued it. Both reach the same guest frame (~4600) in 60 s, so there is no speed
argument either way. Vulkan's single FAIL was a boot that produced no guest log at all —
now retried like any other early crash. Small sample, on a machine running other tracks'
emulators, so treat it as "no reason to pin OpenGL", not as a precise crash rate.

The azahar-libretro core intermittently dies during boot (SIGBUS/SIGSEGV, independent of
the build under test). `retro run`
therefore treats "the guest heartbeat never advanced" as an **early crash** and retries the
same renderer up to 3 attempts total before falling back. Every attempt is recorded in the
manifest's `attempts` array, so a flaky boot is visible afterwards rather than only in the
console. A single `[watchdog]` beat does not count as booted — the crash lands after the
first beat.

**A fresh instance does not inherit another agent's experiment.** The instance ini is
seeded from the shared Azahar Qt SD so real preferences carry over (`[display]`,
`[stereo]`, `[input]`, `[net]`, `[menu]`, `[audio]`), but **every key in `[debug]` and
`[native]` is stripped**. Those two sections are levers and run-control knobs belonging to
whoever used the Qt SD last, and gdx-dev can set any of them itself with `--ini`, so
inheriting them means running a configuration nobody asked for. The rule is by section
rather than by a key list, because a key list has to be chased every time another track
invents a lever — `trectbatch` was missed exactly that way. Measured on this machine, the
shared ini carried `quit_at_tick = 3400` (every seeded run would have stopped at tick
3400), plus `input_replay`, `trectbatch`, `tmemfast`, `trifast` and a whole `[native]`
block: 33 inherited keys. Both lanes then set the same measurement ini explicitly.

**`--bare-ini`** skips that seed altogether: the instance ini starts empty, so the run sees only
the measurement keys and its own `--ini` -- the no-ini player's built-in defaults (preset-fix's
empty-ini parity legs).

`retro run` also sets `ApplePersistenceIgnoreState` on the RetroArch bundle before every
launch. After an abnormal exit macOS otherwise offers to restore the previous session with
an NSAlert that has no window to attach to, so it is invisible and the process hangs
forever — which is what happened to several runs after the orphan-kill testing.

**Omit `--instance` and you get a unique one** (named after the run id), with its own
config, private SD and UDP port. Pass `--instance NAME` only when you deliberately want
to reuse one; `retro run` then refuses to start if a live driver already owns that name,
and tells you which pid, track and run dir hold it (exit 4).

The instance is claimed with an O_EXCL pid file at `<instance>/driver.json`. The driver
kills its RetroArch on every exit path it can observe — `atexit` plus SIGTERM/SIGINT/
SIGHUP — and the emulator is started with `start_new_session=True` so one `killpg` takes
down it and anything it spawned. SIGKILL cannot be caught, which is what `reap` is for:
it kills RetroArch processes that were reparented to pid 1, or whose pid file names a
dead driver, and clears their locks.

`--prune` deletes instance directories that are idle and older than `--days` (default 2).
Unique-per-run instances accumulate one directory each — 74 of them, 852 MB, within a few
days here — so this is how the private-SD win stops eating the disk. It never touches a
directory with a running RetroArch or a live driver lock. Pruning a *named* instance is
safe: the private SD is re-seeded on the next run.

Age comes from `retroarch.cfg`, which gdx-dev rewrites at every launch — **not** from the
newest file in the tree. `seed_sd` copies the user's `dspfirm.cdc`, which is dated 2035 on
this machine, so a newest-file heuristic put every instance nine years in the future and
pruned nothing at any threshold.

`reap` requires **positive** evidence that a driver is gone. A process with a live parent
and no pid file is left alone — several tracks share this machine, and "no lock" means
"started before this feature", not "orphan".

## System model (`--model old|new`, RetroArch lane) — the Old 3DS lane

```sh
tools/dev/gdx-dev retro run <3dsx> --model old --replay race-mutecity-start \
    --ini debug.membudget=1
```

NEXT.md C4. The core declares `citra_is_new_3ds` ("System Model") with the legal pair
`"New 3DS"` / `"Old 3DS"`. **Since emufix (2026-10-16) the per-instance core-options
redirect is written on EVERY run**, mono included (see the core_options warning below) --
without `--model` the EFFECTIVE model is unchanged (still New 3DS, the core's own compiled
default, now PINNED explicitly rather than merely inherited); with `--model old` it is
"Old 3DS" instead. `--gfx` and `--model` both reach the core on every run now, not just
under `--3d`.

Three things it is not:

- **It is not a setting you can A/B against a New 3DS cell.** Old 3DS has no core 2, so
  `gdx3ds_renderthread.cpp` refuses to start the render thread whatever `[debug]
  renderthread` says. An `old` run is a `renderthread=0` **execution**, and PROTOCOL's
  rule applies: parity is same-render-thread-mode only. `platform.model` in the manifest
  follows the flag (`emu-old3ds`) so nothing downstream compares the two by accident.
- **It is not the real Old 3DS memory layout.** Azahar's Old 3DS reports `memtype=2` —
  the 96 MB *extended* layout — not the 64 MB default a 3dsx inherits under hbmenu. The
  lane is more generous than the console and cannot reproduce the binding constraint.
  See `docs/research/native/o3dsbudget-progress.md` §1.4.
- **It is not confirmed by this file.** Read the pin back off the GUEST:
  `[mem-budget] model=old3ds memtype=2` with `[debug] membudget=1`. The first version of
  this flag appended its assignment after the template's own, RetroArch's config parser
  returns the **first** entry for a duplicated key, and the run wrote a file that said
  "Old 3DS" and booted a New 3DS. The harness could not see it; the guest could.

## Stereoscopic 3D (`--3d`, both lanes)

```sh
tools/dev/gdx-dev retro run <3dsx> --3d --replay race-mutecity-start --ini fh_every=1
tools/dev/gdx-dev emu   run <3dsx> --3d --depth-3d 60 --replay race-mutecity-start
tools/dev/fhdiff.py A/log.txt B/log.txt --eye both
```

**`[stereo] enabled = 1` on the guest does nothing on its own.** `Gdx3dsStereo::FrameBegin`
gates the right-eye pass on `osGet3DSliderState() > 0`, and the emulated slider is driven
by the *emulator's* own 3D settings — it reads 0 whenever the host is not rendering in 3D.
Every emulator parity run up to 2026-09-23 set `[stereo] enabled=1` and still rendered
mono, which is why no `[c3d] … eyeR=`/`anchor=` receipt had ever appeared in one. `--3d`
is the host knob:

| lane | what `--3d` sets |
|---|---|
| `emu run` (Azahar Qt) | `qt-config.ini` `[Layout] render_3d` + `factor_3d`, each with its `\default` sibling forced `false`, patched inside the lock and **restored to the previous values afterwards** |
| `retro run` (RetroArch) | the `citra_render_3d` / `citra_factor_3d` core options, against a 3D-capable core |

`--3d` also implies `[stereo] enabled=1` and `[debug] fh_eye=both` on the guest (an
explicit `--ini` still wins). `fh_eye=both` makes every capture emit an `eye=L` **and** an
`eye=R` `[fh]` receipt, and every `SHOT` write `autotest/<tag>_L.bmp` *and* `_RE.bmp` — the
two-eye evidence. (`_RE`, not `_R`: `<tag>_R.bmp` is the bottom screen's, written a tick later by
the touch menu / DASH, and it silently overwrote the right eye on every `--3d` run until
menu-stereo-depth.) Modes: `side-by-side` (default; the only one where a single host frame
visibly contains both eyes), `side-by-side-full`, `anaglyph`, `interlaced`,
`reverse-interlaced`, `cardboard`, `off`.

Read the receipts, not the flag: `[stereo] right-eye pass ACTIVE (slider=…)` once when it
engages, ` eyeR=<n> stereo=1` on every `[c3d]` line, and ` anchor=<draws>/<dmin>/<dmax>`
on the windows that drew a prim-depth-anchored rect. `eye=R inactive` on the `[fh]` lines
means the pass never ran.

### Two traps, both measured

**Qt: the `\default` sibling.** `QtConfig::ReadBasicSetting` reads `<key>\default` first
and, when it is true, throws your stored value away and uses the compiled default. Writing
`render_3d=1` alone is inert — `render_3d\default=false` has to go with it. Azahar also
rewrites the whole file when it exits, so it may only be edited while no Azahar is running.
The enum is **Azahar's**, not Citra's: `Off=0 SideBySide=1 SideBySideFull=2 Anaglyph=3
Interlaced=4 ReverseInterlaced=5 CardboardVR=6` — Azahar inserted `SideBySideFull`.

**RetroArch: the pinned core has no 3D option, and the options file was never read.**
The Azahar 2126.0 release core exposes 30 `citra_*` options and not one of them is stereo
(`core_settings.cpp` never touches `render_3d`, and the core reads no ini). The libretro
**nightly** does expose `citra_render_3d`/`citra_factor_3d`, so `--3d` installs it to
`~/Library/Application Support/gdx-retro/cores-3d/` and runs against that, leaving the
shared release core other agents are mid-run on alone. Two cores are two emulators, so
**a `--3d` run's hashes compare only with another `--3d` run's** — the same-platform rule
one level down.

> ⚠ **`core_options_path` does not work — FIXED for every run, emufix 2026-10-16.**
> RetroArch 1.22 only consults `core_options_path` when `global_core_options` is true;
> otherwise it uses a per-core `<rgui_config_directory>/<corename>/<corename>.opt`, which
> by default is the **shared** `~/Library/Application Support/RetroArch/config/Azahar/Azahar.opt`
> -- the user's own Qt-seeded preferences file, read AND (RetroArch rewrites it on exit)
> WRITTEN by every concurrent instance that omits the redirect. Measured originally
> (2026-09-23): a legal `citra_layout_option = "side_by_side"` in the instance file still
> arrived at the core as `"default"`, and the frontend log said `Saved core options file to
> .../RetroArch/config/Azahar/Azahar.opt`. Every `retro run` before this fix therefore ran
> on the core's compiled defaults, and `--gfx`/`--model` never reached the core (only
> RetroArch's own `video_driver` changed). `--3d` alone used to fix this for itself by
> pointing `rgui_config_directory` at the instance; **`write_config` now does this for
> EVERY run**, mono included, so no lane ever touches the shared Azahar.opt again, and
> `--gfx`/`--model` reach the core on a plain run too. Three values the mono template used
> to write were also illegal for their options (verified against the core's own option
> strings, extracted from the installed `azahar_libretro.dylib`):
> `citra_is_new_3ds = "enabled"` (legal pair `"New 3DS"`/`"Old 3DS"`),
> `citra_resolution_factor = "1x (Native)"` (legal: `"1"`, `"2"`, … -- short indices, not
> the RGUI display labels), `citra_layout_option = "Default Top-Bottom Screen"` (legal:
> `"default"`, `"single_screen"`, `"large_screen"`, `"side_by_side"` -- again the short
> keys, not the labels). They silently fell back to the option default, which happened to
> match the values the fixed template now writes explicitly -- see `CORE_OPTIONS_TEMPLATE`
> in `gdxdev/retro.py`, now a single template shared by every run instead of two
> near-duplicate ones (one illegal, one legal).
>
> **This was investigated as the cause of the mono lane's intermittent host
> `EXC_BAD_ACCESS`/SIGBUS/SIGSEGV boot crash ("last guest frame ~38", "RetroArch exited
> after 4s") -- it is NOT the (sole) cause.** The crash reproduces under BOTH the old and
> the fixed tooling (captured live during this fix's own verification: `Unhandled
> EXC_BAD_ACCESS at pc 0x00006d00000007` / `0x00000400000010` / `0x00007800000080` --
> near-null or ASLR'd host addresses, zero guest `[fatal]` lines every time, the guest log
> advancing cleanly past the reported frame). This matches `docs/research/native/nimenu-
> progress.md` §9.7's independent, already-closed investigation: the crash reproduces even
> with **zero concurrent RetroArch instances**, is host-process (not guest), and predates
> every change on this lane. Root cause is a RetroArch/MoltenVK Vulkan-path boot race in
> the azahar-libretro core itself, independent of which core-options values it reads --
> out of this harness's reach to fix. The existing retry-and-fall-back-renderer logic
> (`EARLY_CRASH_ATTEMPTS`, `GFX_ORDER`) already absorbs it into a recovered PASS/TIMEOUT
> rather than a hard FAIL in every case observed. The core_options fix above is still worth
> having on its own merits (it stops the shared-file contention and makes `--gfx`/`--model`
> actually work), but do not expect it to change the crash rate.
>
> **Behaviour preserved, confirmed by exact comparison.** `[debug] det=1` plus the staged
> replay's own `seed1`/`seed2` (printed in its `[rec] WARNING seed1/seed2` lines) make a
> run deterministic; without them `[fh]`/`[gstate]` differ run-to-run regardless of any
> code change (non-seeded RNG), which is not evidence of anything. With them pinned, one
> `race-gp2-sector` replay to `[debug] quit_at_tick=800`, same binary
> (`e04261246cc1b6a8…`), old tooling vs the fixed tooling: **8/8 `[fh]` hashes identical,
> 802/802 `[gstate]` hashes identical.**

### `fhdiff --eye both`

A `fh_eye=both` run emits two receipts per captured frame sharing one `frame` number.
`--eye both` compares the L and R streams **separately** (an eye is only ever matched
against the same eye), prints a summary line per eye plus `eyes=L,R status=PASS|FAIL`, and
fails if either diverges. A missing R stream is reported as "the right-eye pass did not
run" rather than as a bare no-data. `--eye L` (the default) is unchanged.

## Replays

`tools/dev/replays/` — see its own README for the grammar and the four rules that cost
real time (interleave `SHOT` lines among the button lines; drive SHOTs belong at tick
4600+; keep every line under 62 bytes, comments included; a held button is ONE press —
menus read edges, so walk them with short taps). A `.txt` replay on `retro run` also
implies `[debug] quit_at_tick=<last tick + 120>`, since `replay_quit` only acts on a
`.gdxrec`'s `[rec] end`. `emu run --replay <id>`
validates the script before staging, so a typo — or a long comment — fails immediately
instead of producing a run that silently did nothing.

`.gdxrec` files (the `oracle` track's tick-exact recorder) are staged to
**`sdmc:/rec/<name>.gdxrec`** — the SD **root**, not under `3ds/gdiffuser/` — with
`[debug] input_replay=<name>`, which wins over autoinput. The two are mutually exclusive.
The guest builds the relative path `rec/<name>.gdxrec` and a `.3dsx` runs with the SD root
as its cwd (`port/3ds/main_3ds.cpp:794`), which is why the root is the only place that
works; `tools/dev/oracle-run.sh:129` stages the same way.

Replay ids resolve from **both** `tools/dev/replays/` (autoinput scripts) and
`<repo>/replays/` (the oracle track's `.gdxrec` recordings).

**A replay that does not RUN TO THE END fails the run.** A guest core that dies mid-replay
still leaves an advancing heartbeat and zero `[fatal]` lines, so the heartbeat alone is not
evidence of a good run. When a replay is staged, both lanes additionally require:

| kind | completion evidence |
|---|---|
| `.gdxrec` | the `[rec] end` receipt **and** every scheduled `SHOT` captured |
| autoinput | the last scheduled tick reached **and** every scheduled `SHOT` captured |
| either | the emulator still alive at the end of `--seconds`, unless it exited via `replay_quit` / `quit_at_tick` |

A run stopped deliberately by `[debug] quit_at_tick` counts as complete once the tick is
reached (the `[oracle] quit requested: [debug] quit_at_tick reached` receipt only exists
because it was), and SHOTs scheduled *after* that tick are dropped from the expectation
rather than failing a run that did exactly what it was told.

### `--seconds` is a ceiling, and a staged replay sets its own

`--seconds` bounds a run; it never paces one. The default is **60 s for a bare boot** and
**300 s when a replay is staged** (`GDX_RETRO_REPLAY_SECONDS`). The old flat 60 s was
shorter than every replay in the corpus, so a staged replay could only ever be cut short —
and the cut printed `RESULT FAIL`, which reads like a crash (NEXT.md B45). It no longer
does: a deadline that expires with the emulator alive and the guest still beating scores

```
RESULT TIMEOUT frame=2118 fatals=0 replay=race-mutecity-start(gdxrec) complete=False …
```

whose detail names the frame reached and the tick it was short of. **TIMEOUT is not FAIL**:
FAIL means a fatal, a dead core or a replay that never loaded. Both exit 1.

### A staged replay ends the run (`replay_quit`)

`--replay` alone never *ended* a run: the guest reached the replay's last tick and kept
playing, unscripted and unrecorded, until `--seconds` expired — ~150 s of noise appended to
every canonical-replay run (NEXT.md B19). Staging a replay now implies
`[debug] replay_quit=1` on **both** lanes, so the run leaves the frame loop at `[rec] end`.
That is also what makes the 300 s ceiling free: a well-behaved replay never reaches it.

It is **implied, not forced**. Override it with either spelling:

```sh
tools/dev/gdx-dev retro run <3dsx> --replay race-mutecity-start --no-replay-quit
tools/dev/gdx-dev retro run <3dsx> --replay race-mutecity-start --ini debug.replay_quit=0
```

Use the override for diagnostics that watch what the guest does *after* the scripted inputs
stop — the wobble investigation (post-replay drift) and dynlod (LOD settling) both do, and
a hardcoded flip would have silently truncated them. The run says which way it went, so the
transcript always carries the answer.

The clean exit `replay_quit` causes goes through `exit()`, which logs
`[fatal-trace] atexit: process exiting via exit()`. That receipt is **benign** in
`prof.error_census` (`BENIGN_RECEIPTS`) and is not counted as a fatal — older progress
files that call it a false-negative FAIL predate that fix.

**Both lanes return as soon as the guest is finished**, rather than idling out the rest of
`--seconds`. The emulator routinely outlives the guest — a `.3dsx` that exits leaves
Azahar up with a dead window — so the loop watches the guest log for
`[fatal-trace] atexit:` or `[oracle] quit requested` instead of only the process. Measured:
a `quit_at_tick` run set to `--seconds 240` now returns in 24 s on the retro lane and 168 s
on the Qt lane.

SHOTs are counted by label from `[fh] … tag=<label>` and `[autotest] SHOT captured ->`,
ignoring the periodic `tag=auto` captures. Staging a `.gdxrec` that carries SHOTs also
arms `[debug] fh=1`, because the capture path is gated on it and would otherwise log
"capture request ignored" and silently capture nothing.

Every run writes a one-line `RESULT` file (also printed):

```
RESULT FAIL frame=4969 fatals=0 replay=race-mutecity-start(gdxrec) complete=False shots=5/8 wall_s=57.3 sha256=2be07093d4b47553
```

It quotes the **artifact sha256**, not just the build id: `GDX3DS_BUILD_ID` is a
configure-time value, so after a merge-then-rebuild it names the wrong commit and can lie
about what actually ran. `replay_complete`, `shots_expected`, `shots_seen`,
`missing_shots` and `wall_s` are all in `manifest.json` too.

**A replay that does not load at all fails the run.** Both lanes check the guest log for the
receipt — `[rec] replay armed` for a `.gdxrec`, `autoinput script loaded:` for a script —
and fail on `[rec] replay open failed`, on a missing receipt, or on an autoinput script
that fell back to the seconds timebase. Before this check, a mis-staged replay booted
normally, never replayed, and still reported PASS.

## Tests

```sh
python3 tools/dev/tests/test_gdxdev.py
```

Stdlib `unittest`, and **gated by `tools/ci-3ds.sh`** (step `gdxdev_harness_tests`). It was
not, which is how `--dry-run` shipped not dry-running and how four stale assertions about
the replay corpus sat red on main unnoticed. The harness decides what every run *means*, so
a broken harness is a broken measurement.

Covers the manifest schema (including the `tree` block and the `TIMEOUT` verdict), the ini
merge preserving a foreign agent's key, autoinput grammar rejection, the log parser against
a golden fixture, the `[fatal]` scoping fix, delegate exit 7, and an import under
`python3 -S` (no site-packages) to catch stdlib-only drift.

`RetroDryRunTest` replaces every side-effecting function in `retro` — core download,
instance lock, SD seed, config write, `_attempt`, `runs.create`, `subprocess.Popen` — with
a detonator and asserts a `--dry-run` reaches none of them, plus one test that a REAL run
still does (so the bug cannot be "fixed" by making every run dry). Assertions about the
replay corpus are **derived from the replay files**, never frozen: the coverage track
rewrites those schedules, and a hardcoded copy goes stale silently.

`hw-run` is tested with the console **off**: `tests/fixtures/hw-*.log` are trimmed slices
of the real 2026-09-20 session (one guest boot each, receipt lines only, LAN addresses
scrubbed), and the tests re-derive the published numbers from them — the crowd bucket's
33.97 / 24.54 ms and the fps beat sequence `21 51 59 30 46 60 55 39 15 43 43 43`. The
whole run then executes against a mocked control link (`HwRunSequenceTest`) which proves
the ordering, the ini application, the marker set/delete pairs, the per-slice manifests
and the restore, without a console.

### Host gate (shared Mac)

`retro run` waits before launching when the machine is already busy: at most
`GDX_RETRO_SLOTS` (default 3) gdx-dev RetroArch instances, and a 1-minute load average at or
below `GDX_RETRO_MAXLOAD` (default 40). It polls every 20 s, gives up after
`GDX_RETRO_GATE_TIMEOUT` (default 300 s) and launches anyway with a warning — a stuck gate
must not silently eat a night's work. `GDX_RETRO_SLOTS=0` disables the gate entirely.

Every run records `host.loadavg_at_launch` / `loadavg_at_end` in its manifest. **Treat any ms
or fps number from a run whose load was above ~40 as host contention, not guest cost.** Parity
and mismatch counts stay valid under load; timing does not. This is not theoretical: on
2026-09-23 eleven agents drove the load average to 111 with ten instances up, and at that point
boot SIGBUS becomes common as well.

### `--by-vtxn` and draw-count levers

`prof --ab --by-vtxn` buckets windows by `nD`, so it is only valid when the two runs have the
same draw count. For a lever that *changes* draws it is actively misleading — windows migrate
between buckets and each bucket ends up comparing different scenes. The owned-interpreter batch
fold (−22% draws) read as **+5.9% slower in every bucket** while the whole-run total was flat.
`--ab` now warns when the two runs' mean `nD` differ by more than 5%. Cut the band by scene tag
(between two SHOT receipts) instead.

### Two consoles on the link

`tools/dev/hw.ini` is gitignored (it holds a local IP). It carries the New 3DS under
`[hw]`, and the Old 3DS gets its own `[hw.o3ds]` section: both consoles can be on the
dev link at the same time, so the O3DS is a named entry rather than an `--ip` passed
per session. `gdx-dev hw-run --ip` and `hw.py --ip` still override either. When adding
the O3DS, push a `[net] enabled=1` build to it once by hand with `hw.py push` first --
`hw-run` drives an already-running guest and cannot bootstrap one.

