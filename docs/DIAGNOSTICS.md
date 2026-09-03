# G-Diffuser — Diagnostics and Bug Reports

**Almost every diagnostic in this port is silent by default.** A log that looks complete is usually a
log with the interesting lines switched off. This document lists every switch, what each one reveals,
and which ones change behaviour instead of merely observing it.

Audience: anyone filing a bug report, and anyone reproducing one.
Source of truth: `port/gdx_dev_gates.c` (the gate table), `port/gdx_dev_gates.h` (the policy),
`port/port_log.h` (the log sinks). Gate descriptions below are taken from that table.

> **There are two diagnostic systems, not one.** The 28 gates in the table above are CVar-backed and
> toggleable from the menu. A second set of roughly two dozen probes lives inside `libultraship` and
> reads the environment directly — no CVar, no checkbox, and most of them latch once per process. The
> two overlap on exactly one name, and that overlap is a trap. See
> [The second diagnostic system](#the-second-diagnostic-system) before you conclude a probe found
> nothing.

---

## Quick path: capture a useful log

Set the three logging variables, run the game, reproduce the bug once, quit, and attach the log.

**PowerShell (Windows)**

```powershell
$env:GDX_LOG = "1"
$env:GDX_TRACE = "1"
$env:GDX_DIAG_VERBOSE = "1"
.\G-Diffuser.exe
```

**POSIX shell (Linux)**

```sh
GDX_LOG=1 GDX_TRACE=1 GDX_DIAG_VERBOSE=1 ./G-Diffuser
```

The log is written to **`gdiffuser-run.log`, in the same directory as the executable** — not the
working directory. If the executable's own path cannot be determined, the sink falls back to a
plain `gdiffuser-run.log` relative to the process CWD.

The file is opened in **append** mode, so a second run adds to the same file rather than replacing
it. Delete it before a clean repro if you want only one run's output.

Alternatively, tick the same boxes in-game under **F1 > Dev Tools > Logging**. That path persists
your choice, so the *next* launch has the log armed from boot. It cannot retroactively produce lines
for events that already happened this session.

### Crash reports are separate and always on

`gdiffuser-crash.txt` (also beside the executable) is written by the crash handler and **bypasses
every gate**. A tester running a plain double-clicked build with nothing set still produces this file
on a crash. Attach it whenever the game crashed.

**Copy the build's `.pdb` next to the executable.** The symbolizer already exists
(`port/n64_sched.c:411-460`) and degrades silently without it — you get a raw RVA and nothing else.
`crash-address.txt` also carries no timestamp and no build identifier, and **appends across builds
forever** (`CrashHandler.cpp:415-436`), so records from different binaries accumulate in one file and
cannot be told apart afterwards. Delete it before a crash-hunting session, or note the build yourself.

---

## The four logging gates

These four are "Bucket D" in the source's own terminology: the persisted setting is the authority,
an environment variable overrides it for one run, and none of them is ever compiled out.

| Variable | Setting | Default | What it unlocks |
|---|---|---|---|
| `GDX_LOG` | `gDevTools.Log.FileSink` | off | Opens `gdiffuser-run.log`. Implied by any of the other three. |
| `GDX_TRACE` | `gDevTools.Log.Trace` | **Release: OFF / Debug: ON** | Every probe built on `gdx_ck` / `gdx_cki` / `gdx_ckp`. That is far more than "breadcrumbs" — see below. |
| `GDX_DIAG_VERBOSE` | `gDevTools.Log.Verbose` | off (both configs) | The per-frame diagnostic families listed below. |
| `GDX_DIAG_UNLOCK` | `gDevTools.Log.Unlock` | off | The unlock-code / audio-unlock decision path. |

Setting any one of the four also opens the log file, so `GDX_TRACE=1` alone is enough to get a file.

### `GDX_TRACE` is tri-state, and its Release default is OFF

This single fact has cost this project multiple wasted investigations. A Release build with logging
enabled produces a log that *looks* complete while every breadcrumb the decomp emits is suppressed.

| `GDX_TRACE` value | Release build | Debug build |
|---|---|---|
| unset | **off** | on |
| `0` | off | off at boot, then see the caveat below |
| `1` (or any non-`0`) | on | on |
| set but **empty** | keeps the compiled default (off) | keeps the compiled default (on) |

So: **if you are debugging a Release build, `GDX_TRACE=1` is not optional.** Without it the log
contains only the always-on families (boot, ROM/disk load, scheduler init, transition timings, crash
handler) and none of the per-frame decomp trace.

#### What `GDX_TRACE` mutes is not cosmetic

`gdx_ck`, `gdx_cki` and `gdx_ckp` each open with `if (!gdx_trace_enabled()) return;`
(`port/n64_sched.c:503-516`). Every probe built on them disappears — including the entire
asset-delivery failure telemetry:

| Family | Reports | Consequence when muted |
|---|---|---|
| `[reg-miss]` | A texture-registry lookup returned NULL, with the symbol and the live registry count | A NULL palette skips the TLUT upload and CI text draws against the previously loaded palette. This is the named mechanism behind pause-menu text corruption (`decomp/src/game/object.c:653-658`) — and it logs nothing. |
| `[asset] MISS`, `[rom] MISS` | A common asset was not delivered | A missing or garbled menu texture produces **zero** log evidence |
| `[mio0] OVERFLOW PREVENTED` | A decompression bound was hit | Silent truncation |
| `[o2r]` | Archive-side resolution | Silent |
| `[GDX ceremony]` | Ending/podium sequence state, including firework allocation | The one probe that distinguishes "not allocated" from "allocated but not drawn" |

A run that produces none of these has not proven anything is healthy. It has proven `GDX_TRACE` was
off. Four separate defect investigations in this project reached a false "no problem found" this way.

The "set but empty" row is genuinely unusual and applies only to this gate — an exported but empty
`GDX_TRACE` is treated as *present with no opinion* and the compiled default wins. Every other gate
treats presence or a leading character as decisive.

> **Caveat, traced from the gate logic rather than observed at runtime.** `GDX_TRACE=0` on a **Debug**
> build silences the trace during boot, but it is not "env-pinned" (pinning requires the variable to
> resolve *on*), so the first per-frame refresh re-reads `gDevTools.Log.Trace` using the compiled
> Debug default of 1 and turns the trace back on — unless you have persisted that setting to 0 in the
> menu. To profile a Debug build with the trace genuinely off, untick it in **F1 > Dev Tools >
> Logging** so the preference is saved, rather than relying on `GDX_TRACE=0` alone. This reading comes
> from `gdx_dev_gates_refresh()` and `gdx_dev_gate_is_env_pinned()` in `port/gdx_dev_gates.c`; it has
> not been confirmed by running a build.

### What `GDX_DIAG_VERBOSE` unlocks

Without it, these families never appear at all. They are the per-frame aggregates you almost always
want for a rendering bug.

| Family | Emitted from | Reports |
|---|---|---|
| `[bigtri]` | `port/n64_gfx_bridge.cpp` | Oversized triangles: the three clip-space vertices, geometry mode, combiner, tile, texture pointer, viewport. Budgeted separately for race frames (large) and menu frames (small), because menu screens used to exhaust the whole budget before a race ever started. |
| `[geodiag]` | `port/n64_gfx_bridge.cpp` | Per-frame geometry census: vertices loaded/invalid, non-positive W, near/far rejects, NDC ranges, triangles in/clipped/culled/emitted, GPU draws. |
| `[gpustate]` | `port/n64_gfx_bridge.cpp` | Viewport and scissor rectangles. A `0.0x0.0` scissor here means the interpreter is clipping everything away. |
| `[phasegeom]` | `port/n64_gfx_bridge.cpp` | Microcode variant switches and pre-flex vertex/triangle counts. |
| `[gfxdiag]` | `port/n64_gfx_bridge.cpp` | The big per-frame command census: list counts, no-op/missing/bad display lists with their raw addresses, fallback and skipped data commands, texture-copy bytes, per-opcode counts, microcode switches. |
| `[game]`, `[seg]`, `[sched]` | `port/n64_sched.c` and the decomp | Game-mode checkpoints, segment resolution (`raw` vs `resolved` pointer), scheduler dispatch. |
| `[datafail]`, `[tex-census]`, `[ci-dump]`, `[dl-census]`, `[seg-dl]` (success), `[seg-dl-race]`, `[seed]` (quad probe) | `port/n64_gfx_bridge.cpp` | Additional per-frame bridge aggregates. |

`[seg]` needs **both** `GDX_TRACE` and `GDX_DIAG_VERBOSE` — it checks the trace gate first, then the
verbose gate. This is why setting only one of the two can still leave segment resolution invisible.

### Families that are always on

You get these with `GDX_LOG=1` alone, and they are usually high-signal:

`[bridge-init]` (one-shot boot state), `[segment]` (mode-transition segment loads), `[stub-miss]`
(bounded to 24 unique pointers), `[gdl-miss]` and `[gdl-bad]` (error families, per-phase budget),
`[transition]` / `[gdxcap]` / `[dump]` / `[vifallback]` boot and capture one-shots, and the
`[transition]` timing probe that quantifies each mode change in milliseconds.

---

## Diagnostic gates vs behaviour switches

**Getting this distinction wrong produces misleading evidence.** The source separates them
deliberately:

| | Diagnostic (bucket A / D) | Behaviour (bucket B) |
|---|---|---|
| Effect | Observes. Rendering and game logic are unchanged. | **Changes what is rendered or what runs.** |
| Safe in a bug report? | Yes — the log describes the real bug. | **No.** Any screenshot or log is of a modified game. |
| Compiled into Release? | Always. | **No** — compiled out entirely unless `GDX_DEV_TOOLS` is defined. In a normal Release build these variables are not even read. |
| Naming | Mostly `GDX_DIAG_*` | Mostly `GDX_TEST_*`, plus the named exceptions below. |

If you enable a behaviour switch while investigating, say so explicitly in the report, and re-confirm
the bug with all of them off before filing.

The whole design rests on one invariant: **0 always means stock behaviour.** Some of the original
variables were opt-in (`=1` turns something on) and some were kill switches (`=0` turns something
off); every gate is normalised so that 0 reproduces shipping behaviour and 1 is the deviation. Note
the consequence for the two kill switches: it is `GDX_G2_CONVERT=0` and `GDX_HLE_FILTER=0` that
*activate* a deviation.

---

## The second diagnostic system

Everything above describes the **gate table** — 28 gates in `port/gdx_dev_gates.c`, each with a CVar,
a menu checkbox, and an environment variable that are genuinely interchangeable.

`libultraship` has its own, older set of roughly two dozen probes that predates the gate table and was
never migrated. They read `std::getenv` directly. For these:

- **There is no CVar and no checkbox.** Ticking a box in Dev Tools cannot arm them.
- **Most latch once per process.** They are declared `static const` — 26 such reads in
  `interpreter.cpp` alone — so the value at the first call wins for the rest of the run.
- **They are not compiled out of Release.** `interpreter.cpp` contains no `GDX_DEV_TOOLS` guard at
  all, so the bucket-B "behaviour switches vanish in Release" guarantee does not cover them.

### The one that overlaps

`GDX_DIAG_SETTIMG` exists in **both** systems, and they are not wired together.

| You do this | Bridge-side `[settimg]` trace | libultraship-side `[tmem] store` / `lookup` |
|---|---|---|
| Tick `gDevTools.Diag.SetTimg` in the menu | armed | **silent** |
| `export GDX_DIAG_SETTIMG=1` before launch | armed | armed |
| `--diag-settimg` on the command line (Windows) | armed | armed |

The interpreter reads the raw environment (`libultraship/src/fast/interpreter.cpp:4404`). The
`_putenv` shim that bridges the two fires only for the command-line flag
(`port/n64_gfx_bridge.cpp:7367`), and its own comment says it stays "until interpreter.cpp is migrated
off getenv".

**So: for anything TMEM- or texture-cache-related, export the variable. Do not tick the box.**

### Env-only probes

Observational — safe in a bug report:

`GDX_DIAG_BLENDMODE`, `GDX_DIAG_CI_LATCH`, `GDX_DIAG_EFFECTDRAW`, `GDX_DIAG_EFFECTTILE`,
`GDX_DIAG_FOG`, `GDX_DIAG_FONT_MACHINE`, `GDX_DIAG_PALETTE_EVICT`, `GDX_DIAG_PITUV`,
`GDX_DIAG_RGBA16`, `GDX_DIAG_SETTILE`, `GDX_DIAG_SOLIDROAD`, `GDX_DIAG_TMEMCHK`,
`GDX_DIAG_UVPROBE`, `GDX_FONT_CONTENT_HASH`, `GDX_MINIMAP_PROBE`

**These change rendering despite the naming, and they ship live in Release:**

| Variable | Effect |
|---|---|
| `GDX_NO_TMEM` | Disables TMEM tracking and the TMEM cache path entirely |
| `GDX_CI_PALETTE_HASH` | Adds palette *content* to the CI texture cache key — the key otherwise omits it |
| `GDX_DIAG_SKIP_COMBINE` | Takes a hex combiner mux and skips matching draws |
| `GDX_RECT_HALF_TEXEL` | Half-texel rect offset. **Inverted:** only a leading `1` enables it; unset means disabled |
| `GDX_DIAG_TEXEL1_FROM_BASE` | Forces TEXEL1 to the base tile |
| `GDX_DIAG_DISABLE_PREFLX_DEPTH`, `GDX_DIAG_DISABLE_PREFLX_FOG`, `GDX_DIAG_FORCE_PREFLX_OPAQUE_ALPHA`, `GDX_DIAG_FORCE_PREFLX_SIMPLE_MATERIAL` | Pre-flex material and depth overrides |

Treat that second group exactly like a bucket-B switch: name it in any report, and re-confirm the bug
with it unset.

---

## Probe budgets

Nearly every probe is capped so one bad frame cannot fill the disk. A probe that stops reporting has
usually exhausted its budget rather than stopped finding things, and **an earlier game mode can burn
the entire budget before you reach the screen you care about**.

| Probe | Budget | Scope |
|---|---|---|
| `[tmem] store` | 400 lines | process, race-gated |
| `[reg-miss]` | 24 lines | process |
| `[stub-miss]` | 24 unique pointers | process |
| `settimg-trace` | 6000 entries | per PID, cap resets on game-mode change |
| `[bigtri]` | separate race and menu budgets | per phase |

Practical consequence: **boot straight into the mode you are testing.** Walking through three menus
first can leave nothing for the race.

---

## Complete gate reference

Grouped as the source groups them. "Env" is the variable name; the in-game setting is the CVar. All
default to off unless noted. Descriptions are the table's own.

### Logging (bucket D — boot-seeded)

Covered above: `GDX_LOG`, `GDX_TRACE`, `GDX_DIAG_VERBOSE`, `GDX_DIAG_UNLOCK`.

### Graphics — diagnostics

| Env | Setting | Reports |
|---|---|---|
| `GDX_DIAG_RIVAL` | `gDevTools.Diag.RivalIcon` | Rival-icon and 1ST/2ND/3RD position-marker draw conditions, once per second, plus emitted texrects. |
| `GDX_DIAG_CUSTOMMACHINE` | `gDevTools.Diag.CustomMachine` | The `gCustomMachine` record the Create Machine draw path actually reads. |
| `GDX_DIAG_SETUPDL` | `gDevTools.Diag.SetupDl` | How the segment-8 course material setup display lists are classified and converted. |
| `GDX_DIAG_TRECT` | `gDevTools.Diag.TexRect` | TEXRECT count per screen-transition instance, and the per-instance total. |
| `GDX_DIAG_COUNTDOWN` | `gDevTools.Diag.Countdown` | The race countdown quad: raw command words, object-space rect, modelview matrix. |
| `GDX_PRESENT_PATH_TRACE` | `gDevTools.Diag.PresentPath` | Which present path each frame took (GPU hold, readback, VI fallback). |
| `GDX_DIAG_HOLD` | `gDevTools.Diag.HoldTick` | Every hold tick and whether content changed — proves the readback is gone. |
| `GDX_DIAG_TRANSITION_DUMP` | `gDevTools.Diag.TransitionDump` | Writes `transition-capture.bmp` beside the executable. **Slow**: per-pixel conversion plus a file write. |
| `GDX_DIAG_CAPTURE_PROBE` | `gDevTools.Diag.CaptureProbe` | Fingerprints captured framebuffer content, separating a stale capture from a mislaid one. |
| `GDX_INTERP_DETERMINISM` | `gDevTools.Diag.InterpDeterminism` | Per-tick RNG fingerprint, to localise an interpolation-induced sim divergence. |
| `GDX_DIAG_LOOKAT` | `gDevTools.Diag.LookAt` | The LookAt source matrix and resulting texgen basis at each machine reflection-pass setup. |

### Assets — diagnostics

| Env | Setting | Reports |
|---|---|---|
| `GDX_DIAG_NODEINFO` | `gDevTools.Diag.NodeInfo` | Segment-9 resolution for the Course Edit node-info overlay. Shared with a SETTIMG-side probe, so one run captures both ends. |
| `GDX_DIAG_SETTIMG` | `gDevTools.Diag.SetTimg` | Fingerprints the bytes behind every resolved texture source during a race and flags MIO0 streams. |

### Scheduling — diagnostics

| Env | Setting | Reports |
|---|---|---|
| `GDX_PERF` | `gDevTools.Diag.PerfTelemetry` | Per-phase spike attribution plus periodic p50/p95/p99 frame-time summaries. |

### Graphics — behaviour switches (change rendering)

| Env | Setting | Changes |
|---|---|---|
| `GDX_TEST_BODYENV` | `gDevTools.Behavior.TestBodyEnv` | Repaints the custom machine body env colour magenta and the cockpit yellow, to attribute hull pixels. |
| `GDX_TEST_NOPARTDL` | `gDevTools.Behavior.TestNoPartDl` | Skips the custom machine part display lists (companion to the body-paint test). |
| `GDX_TEST_NO_OVLMODE` | `gDevTools.Behavior.TestNoOvlMode` | Skips `G_RM_ZB_OVL_SURF` before the machine reflection pass (one-variable blend test). |
| `GDX_RAIL_COLOR_TEST` | `gDevTools.Behavior.RailColorTest` | Freezes the rail chevron colour sawtooth to a constant, to isolate the interpolation strobe. |
| `GDX_DIAG_NO_SRCWIN` | `gDevTools.Behavior.NoSourceWindow` | Disables source-window matrix reconstruction, so a regression can be bisected without a rebuild. **Named `DIAG` but it is a behaviour switch.** |
| `GDX_G2_CONVERT` | `gDevTools.Behavior.NoG2Convert` | **Kill switch:** `GDX_G2_CONVERT=0` disables the G2 binary-to-wide display-list converter. |
| `GDX_INTERP_P0` | `gDevTools.Behavior.InterpP0` | Forces the experimental P0 matrix-interpolation path on, independent of the user-facing setting. |
| `GDX_INTERP_CAMERA` | `gDevTools.Behavior.InterpCamera` | Also reroutes `G_MTX_PROJECTION` pool matrices through the interpolation scratch. `race.c:250` loads the combined projection×view camera with that flag and `course.c` has no matrix of its own, so both are frozen at 60 Hz while this is off. |

### Assets — behaviour switches

| Env | Setting | Changes |
|---|---|---|
| `GDX_LEGACY_RESOLVE` | `gDevTools.Behavior.LegacyResolve` | Re-enables the bridge resolver's legacy address-guessing branches, which a soak test retired. |

### Audio — behaviour switches

| Env | Setting | Changes |
|---|---|---|
| `GDX_NO_REVERB` | `gDevTools.Behavior.NoReverb` | `GDX_NO_REVERB=1` removes the reverb wet-to-dry return from the mixed buses. Only a leading `1` counts. |
| `GDX_HLE_FILTER` | `gDevTools.Behavior.NoHleFilter` | **Kill switch:** `GDX_HLE_FILTER=0` disables the HLE synthesis low-pass FIR. Lets the reverb loop diverge. |
| `GDX_SEQ_ADPCM` | `gDevTools.Behavior.SeqAdpcm` | `GDX_SEQ_ADPCM=1` restores the old sequential VADPCM decode instead of the hardware-correct block convolution. Only a leading `1` counts. |

---

## Gates with unusual semantics

Worth knowing before you conclude that a variable "did nothing".

| Semantics | Which gates | Rule |
|---|---|---|
| Presence is enough | `GDX_DIAG_SETUPDL`, `GDX_DIAG_TRECT`, `GDX_DIAG_COUNTDOWN`, `GDX_PRESENT_PATH_TRACE`, `GDX_DIAG_HOLD`, `GDX_DIAG_TRANSITION_DUMP`, `GDX_DIAG_CAPTURE_PROBE`, `GDX_DIAG_NODEINFO`, `GDX_DIAG_SETTIMG`, `GDX_DIAG_LOOKAT`, `GDX_DIAG_NO_SRCWIN` | Set at all — **even to `0`** — turns it on. `GDX_DIAG_SETUPDL=0` enables it. Unset the variable to disable. |
| Opt-in | `GDX_LOG`, `GDX_DIAG_VERBOSE`, `GDX_DIAG_UNLOCK`, `GDX_PERF`, `GDX_DIAG_RIVAL`, `GDX_DIAG_CUSTOMMACHINE`, `GDX_INTERP_DETERMINISM`, and the `GDX_TEST_*` / `GDX_LEGACY_RESOLVE` / `GDX_INTERP_P0` switches | Any non-empty, non-`0` value turns it on. `=0` leaves it off. |
| Leading `1` only | `GDX_NO_REVERB`, `GDX_SEQ_ADPCM` | Only a value starting with `1`. |
| Kill switch | `GDX_G2_CONVERT`, `GDX_HLE_FILTER` | Only a value starting with `0`, which *disables* a default-on feature. |
| Tri-state | `GDX_TRACE` | See the table above. Set-but-empty keeps the compiled default. |

Two further mechanics:

- **A value longer than 31 characters** is treated as *present with no value*. Only the first
  character of a gate value is ever consulted, so this only matters for the presence-based and
  tri-state gates.
- **A variable that resolves to ON pins its gate for the whole run** (bucket D only): the in-game
  checkbox is disabled and labelled as env-pinned, and toggling it cannot take effect. A variable
  that resolves to *off* does not pin — that was deliberately changed so an exported `GDX_LOG=0`
  no longer locks you out of your own setting.

### Compiled out of Release builds

Every behaviour switch (bucket B) is **hard-wired to 0 with no environment read and no setting read**
unless the build defines `GDX_DEV_TOOLS`. On a normal Release build, exporting `GDX_TEST_BODYENV=1`
does nothing whatsoever. If a behaviour switch appears to be ignored, check whether the build has dev
tools compiled in before investigating further.

**How to tell which build you have:** open **F1 > Dev Tools**. If the behaviour section reads
*"Behavior overrides (not in this build)"*, they are compiled out. If it draws live checkboxes, they
are compiled in. `GDX_DEV_TOOLS` is defined for every configuration **except Release**; a Release
binary can be built with them included by configuring `-DGDX_FORCE_DEV_TOOLS=ON`.

The diagnostic and logging gates (buckets A and D) are always compiled in, in every configuration.

---

## Variables that are not gates

These are consumed before the settings system exists, or carry a value rather than a flag, so they
stay plain environment variables and do not appear in the Dev Tools gate list:

| Variable | Purpose |
|---|---|
| `FZEROX_ROM` | Path to the user's ROM for the boot/extraction path. |
| `GDX_INPUT_SCRIPT` | Path to a deterministic tick-level input script, for unattended tests. |
| `GDX_STRICT_ARCHIVE`, `GDX_DUMP_SELFTEST` | Boot and tooling paths. |
| `GDX_CAPTURE_FRAMES`, `GDX_CAPTURE_MODE`, `GDX_CAPTURE_WINDOW` | Framebuffer captures, in `"start:count"` form. |
| `GDX_PCM_CAPTURE`, `GDX_PCM_CAPTURE_FRAMES` | Audio capture prefix and length. |
| `GDX_RAND_SEED1`, `GDX_RAND_SEED2` | Numeric RNG determinism pins. |
| `GDX_SEED_BOOT_LOGO`, `GDX_AUDIO_THREAD`, `GDX_AI_CUSHION` | Decided before the menu exists. |
| `GDX_INTERP_P1`, `GDX_INTERP_P2` | Interpolation test overrides. |

### Command-line switches (Windows only)

Some probes are also reachable as command-line arguments, because a double-clicked launch carries no
shell environment at all. These are parsed inside a `#ifdef _WIN32` block and are **not available on
Linux** — use the environment variable there.

| Switch | Equivalent |
|---|---|
| `--diag-settimg` | Arms `GDX_DIAG_SETTIMG` |
| `--diag-texel1-base` | Sets `GDX_DIAG_TEXEL1_FROM_BASE=1` for the renderer's TEXEL1 A/B bisect |
| `--seed-boot-logo` | Enables the boot-logo seed path |

---

## What to attach to a bug report

- [ ] **Platform** — Windows or Linux, and the version.
- [ ] **Build configuration** — Release or Debug, and whether dev tools are compiled in. This decides
      whether `GDX_TRACE` and the behaviour switches were live at all.
- [ ] **Renderer / graphics backend** — as reported in the menu, plus GPU and driver version.
- [ ] **ROM region and revision**, and whether the 64DD Expansion Kit disk image is in use.
- [ ] **`gdiffuser-run.log`** from a run with `GDX_LOG=1 GDX_TRACE=1 GDX_DIAG_VERBOSE=1`. Without
      those, the log is missing exactly the lines that matter.
- [ ] **`gdiffuser-crash.txt`** if the game crashed.
- [ ] **Repro steps** — from launch to symptom, including the mode transitions taken to get there
      (many defects in this port are transition- and segment-residency-dependent).
- [ ] **Which gates were on.** Name them. In particular, state whether any *behaviour* switch was
      enabled, and confirm the bug still reproduces with all of them off.
- [ ] **A screenshot or short clip** for anything visual.

Keep the log to one reproduction. `GDX_TRACE=1` with `GDX_DIAG_VERBOSE=1` emits per-frame output and
grows quickly; a ten-minute session is mostly noise.

---

## Reading `[interp-p2]` — the frame-interpolation health line

With Frame Interpolation on, one `[interp-p2]` line prints every 120 ticks (~2 s). It is the
line that decides whether a pacing or rendering change shipped clean, and its two most important
fields measure different clocks — conflating them cost this project a full day once.

```
[interp-p2] ticks=9241 subframes=2 dropped=1 avg_m=2.34 sim_hz=59.9 t_last=1.000 tasks=1
            lerped=31 snapped=0 vp=6/0 vtx=4/0 presents/s=143.8 pair_max=0 pair_susp=0/1510 idem_div=0/8968
```

| Field | Meaning | Healthy |
| --- | --- | --- |
| `sim_hz` | **Measured** simulation rate — the game clock. Rolling 30-tick window. | ≈ 59.9, always. Below ~59 the game is running in slow motion regardless of frame rate. This is the hard contract. |
| `presents/s` | Real presented frames per second (rolling ~2 s meter). | Near the target (144 on a 144 Hz panel). May legitimately dip under load — the budget guard drops sub-frames to protect `sim_hz`. A dip that parks at exactly 120 is the guard's integer plateau (all-2s × 60), not a limiter. |
| `ticks=` | Cumulative sim ticks this session. Divide its delta by wall-clock time for an independent `sim_hz` check. | monotonic |
| `subframes=` / `dropped=` | Last tick's presented sub-frames and refused/shed passes. | drops rare outside load spikes |
| `avg_m` | Cumulative average sub-frames per tick since boot. **Cumulative** — do not read it as a per-window value. | → target/60 (2.40 at 144 Hz) over time |
| `lerped=` / `snapped=` | Pool **matrices** tweened vs snapped last tick. | in-race: lerped ≫ snapped. On machine select, `lerped=31` is the expected count (29 frozen cells + 1 spinning + projection). |
| `vp=a/b` | Carousel **viewports** lerped/snapped (course select only). | `6/0` while the carousel exists |
| `vtx=a/b` | **Effect vertex batches** (boost flames, side-attack quads) lerped/snapped. Zero during unboosted driving is normal — these are boost-gated effects. | lerped dominating while boosting |
| `pair_max` / `pair_susp` | Worst matrix pairing delta since the last line / suspect pairings over total. | ≈ 0 |
| `idem_div` | Sub-frame replay divergence (state leaking across replays) over multi-pass ticks. | `0/N`, always |

The companion `[interp-pace]` line (every 120 multi-pass ticks) shows the burst itself:
`passes=<sized> planned=<wanted>` with `budget=` counting passes the tick-budget guard shed. A
`planned=3` tick sized to `passes=2` presents t=½ and t=1 — even spacing, newest pose always lands.

Two A/B gates exist for this system, both rebuild-free: `GDX_NO_INTERP_BUDGET=1` disables the
budget guard (expect slow motion under load — that is the disease the guard cures), and
`GDX_INTERP_FORCE_T1=1` pins every sub-frame to t=1 (measurement mode; announces itself in the log
at first arm — an A/B whose arm state is not in the log is not an experiment).

## Related

- `port/gdx_dev_gates.h` — the Dev Tools page policy (which gates are safe to expose and why), and
  what adding a new gate requires.
- `docs/ARCHITECTURE.md` — the porting model, and a symptom-to-subsystem triage table that points
  back at the gates above.
