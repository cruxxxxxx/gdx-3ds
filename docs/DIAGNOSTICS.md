# G-Diffuser 3DS — Diagnostics and Bug Reports

The console has no process environment, so **nothing here is an environment variable** — every
switch is a key in `sdmc:/3ds/gdiffuser/gdiffuser.ini`, under the `[debug]` section unless noted.
Edit the file by hand, or use the in-game developer console (hold **L+R+SELECT for 1.5 s**, or
set `dev_console = 1` to get its tabs inside SETTINGS) — settings changed there are written back
to the ini for you.

Audience: anyone filing a bug report, and anyone reproducing one.

---

## Quick path: capture a useful log

1. Add to `gdiffuser.ini`:
   ```ini
   [debug]
   filelog = 1
   ```
2. Reproduce the bug once.
3. Attach `sdmc:/3ds/gdiffuser/log.txt` (or `log-EK.txt` for the Expansion Kit title).

`filelog` only gates the **SD-card copy** of the log. A ring buffer of recent lines is kept in RAM
at all times regardless of this setting, which is how a crash report (below) can still show the
tail of what happened right before a crash even if you forgot to turn `filelog` on.

### Crash dumps are opt-in on this platform

Unlike the PC port, a 3DS crash does **not** automatically write a report. Turn it on before you
go looking for a crash:

```ini
[debug]
crashdump = 1
```

With it set, a fatal error writes `sdmc:/3ds/gdiffuser/crash*.txt`, including the tail of the
retained log ring. Attach that file whenever the game crashed. If you didn't have `crashdump` set
when it happened, `log.txt` (if `filelog` was on) is the next log-level evidence — and if the
crash happened early in boot, the boot trail below may already have caught it with no ini change
at all. Otherwise, turn `crashdump` on and try to reproduce it again.

**On Old 3DS with older Luma builds, the exception handler is not guaranteed to fire for every
real crash** — closing an armed title from HOME has, on real hardware, reported "the system will
reboot" with no `crash*.txt` written. Before relying on `crashdump` to catch a real bug, confirm
it actually works on your own console: set `[debug] crashdump_selftest = 1` alongside
`crashdump = 1` and reboot. `crashdump_selftest` deliberately faults on boot so you can check that
`crash*.txt` gets written at all. If the selftest produces nothing either, the handler is not
dispatching on that console/Luma combination, and `log.txt` (with `filelog = 1`) is the only
evidence you'll get — unless the crash was early enough in boot for the boot trail below to have
already recorded it.

### Early-boot crashes: the boot trail (`boot.txt` / `boot-prev.txt`)

Separate from `crashdump` above, and **on by default with no ini key needed**: a crash-proof boot
trail runs on every boot. From `main()` entry — before config loads, before `gfxInit`, before the
file log even starts — through the first couple of game frames, every boot milestone is appended
with a raw, immediately-flushed write to `sdmc:/3ds/gdiffuser/boot.txt`, and the main thread gets
its own early CPU-exception handler armed for that same window. The previous boot's trail survives
one more boot as `boot-prev.txt`. It exists because a real Old-3DS/Luma-6.6 boot crash once left no
log and no crash dump at all (`port/3ds/gdx3ds_boot_trail.c`).

```ini
[debug]
boot_trail = 0         ; default 1 (on); set to 0 to turn it off entirely
boot_trail_frames = 2  ; how many host frames it records before closing itself (default 2)
```

You do not need to set anything to get a boot trail — `boot.txt` / `boot-prev.txt` are already on
the SD card by default. Pull them with `hw.py get` (`docs/HW-DEV-LOOP.md`) or straight off the
card, the same way you would `log.txt`.

### The desktop trace family does not reach this platform

The PC port's `GDX_TRACE` breadcrumbs (`gdx_ck`/`gdx_cki`, the `[reg-miss]`/`[asset] MISS`/`[o2r]`
family, and the whole CVar-backed developer-gate table) are built on reading a process
environment variable. The 3DS has none, and those specific calls are confirmed to never print on
this platform even though the surrounding code (`decomp_port.c`, `n64_sched.c`) is compiled in.
**If you are chasing an asset-delivery or segment-residency bug and expect to see those families
in `log.txt`, stop — they are not there on 3DS, on any build.** The 3DS reports the equivalent
information through its own feature-specific receipts instead (next section) — if you need
asset/segment visibility that no receipt below covers, that is a real documentation gap, not a
switch you forgot.

---

## Debug ini keys worth knowing

| Key | Default | What it does |
|---|---|---|
| `filelog` | `0` | Mirrors the log to `log.txt` / `log-EK.txt` on the SD card. |
| `crashdump` | `0` | Writes `crash*.txt` on a fatal error. See above — opt in *before* reproducing. |
| `crashdump_selftest` | `0` | Deliberately faults right after boot so you can confirm `crashdump` actually writes a file on your console. See above. |
| `boot_breadcrumbs` | `0` | Logs the decomp's own boot checkpoints for the given number of host frames. Useful for a hang or crash during the earliest part of boot, before the normal log path is even running. |
| `boot_trail` | `1` | Crash-proof boot-milestone trail to `boot.txt`/`boot-prev.txt`, on by default; see "Early-boot crashes" above. Set to `0` to disable. |
| `boot_trail_frames` | `2` | How many host frames the boot trail records before it closes itself. |
| `gputrace` | `0` | Arms the per-phase CPU/GPU profiler (`[prof]`/`[gpu]`/`[fill]` tables) and the DBG tab's live frame-time rows. **Costs real frame time** — confirm a bug still reproduces with it off before filing. |
| `mainprof` | `0` | The same per-phase clock as `gputrace`, without the GPU-side instrumentation — a lighter option when you only need CPU-phase timing. |
| `det` | `0` | Pins game nondeterminism for a reproducible run (frame-hash / replay tooling). See `NATIVE-RENDERER.md` §5. |
| `disk_stream`, `audio_stream`, and the related cache/block-size keys | see `NATIVE-RENDERER.md` §2.6 | Expansion Kit disk streaming and audio sample-bank streaming. Documented there, not repeated here. |
| `[native] *` | see `NATIVE-RENDERER.md` §2 | Every renderer lever, and how to bisect a renderer regression. |

### The four narrow rendering probes that *do* reach the console

A second, older diagnostic style exists on desktop as raw environment variables (`GDX_DIAG_*`),
read directly by the vendored libultraship interpreter
(`port/3ds/vendor/lus/src/fast/interpreter.cpp`). On 3DS, with no environment to read, **four**
of that family were explicitly bridged to an ini key instead, because a specific bug
investigation needed them on hardware. The bridge itself — the `gdx3ds_config_get_int("debug",
...)` calls that read these keys — lives in `port/3ds/gfx/gfx_citro3d.cpp`, not in
`interpreter.cpp`:

| Key | Diagnoses |
|---|---|
| `diag_prim` | Primitive-colour resolution path |
| `diag_livery` | Custom-machine livery/paint texture identity |
| `diag_padfill` | A texture-fill discriminator used to isolate a sky/backdrop coverage bug |
| `diag_sky` | Sky/backdrop draw coverage, per frame |

Every other `GDX_DIAG_*` name you might find mentioned in a source comment or in a desktop-port
discussion is **not reachable on 3DS** — there is no environment to set it in, and no ini bridge
was added for it. `interpreter.cpp` itself still reads a few env-only names on 3DS
(`GDX_DIAG_VERBOSE`, `GDX_DIAG_TEXBIND`, `GDX_DIAG_CI_LATCH`) — they exist in the code but, with
no process environment on console, there is no way to set them there. Don't spend time trying to
arm one of these; if you need it, it needs a new ini bridge first.

---

## What to attach to a bug report

- [ ] **Console model** — Old 3DS/2DS or New 3DS/New 2DS XL, and which build you installed
      (`o3ds`, `n3ds`, `o3ds-ek`, `n3ds-ek`). The runtime still detects your actual model and
      picks performance settings for it (`ARCHITECTURE.md` §4.7), but the build still has to fit
      your console's memory budget.
- [ ] **Entry point** — `.3dsx` (Homebrew Launcher) or `.cia` (HOME menu), and your Luma3DS
      version.
- [ ] **`log.txt`** (or `log-EK.txt`) from a run with `[debug] filelog = 1`.
- [ ] **`crash*.txt`** if the game crashed and `[debug] crashdump = 1` was set. If it wasn't, say
      so and try to reproduce with it set. If `crashdump` was set but no file appeared, try
      `crashdump_selftest` (see above) before assuming the crash itself produced nothing.
- [ ] **`boot.txt` / `boot-prev.txt`**, especially for a hang or crash very early in boot (before
      the title even gets far enough for `log.txt` or `crash*.txt` to help) — these are written
      by default with no ini change needed; see "Early-boot crashes" above.
- [ ] **Repro steps**, including which modes/menus you passed through before the symptom —
      several defects in this port only show up after a particular sequence of mode changes
      (`ARCHITECTURE.md` §5).
- [ ] **Which `[native]` / `[debug]` keys you had set**, if any, and confirm the bug still
      reproduces with them back at their defaults. A screenshot or short clip for anything visual.
- [ ] **Expansion Kit only**: which disk image (retail JP vs. the fan translation), and whether
      the EK-only menu entries were available at all (an EK boot with no disk mounted plays the
      normal game but logs that those entries are disabled — that is expected, not a bug).

Keep the log to one reproduction — `gputrace`/`mainprof` and a long session produce a lot of
output that is mostly noise for anyone reading it afterwards.

---

## Related

- `docs/ARCHITECTURE.md` §6 — a symptom-to-subsystem table pointing back at this file and at the
  native renderer's own bisect levers.
- `docs/NATIVE-RENDERER.md` — every `[native]` ini key, the verify/bisect levers, and the boot
  receipt line that names every resolved value once per run.
- `docs/HW-DEV-LOOP.md` — pulling a log or a crash file off a console over WiFi without removing
  the SD card (`hw.py get`).
