# G-Diffuser 3DS — project status

**As of 2026-10-19, v1.0.0.**

What is shipped, what is verified on real hardware, and what is still open — per feature,
per console. "Verified" means run and checked on a physical console, not just an emulator.

## Legend

| Symbol | Meaning |
|---|---|
| ✅ | Shipped, verified on real hardware |
| 🟡 | Shipped, but only partly verified on hardware (see note), or verified on hardware with a caveat |
| ⚪ | Shipped, emulator-tested only — no hardware run yet |
| 🔧 | Works, but only verified in an internal development configuration — no documented, player-facing way to turn it on yet |
| ❌ | Not supported on this console |
| — | Not applicable — the row doesn't describe anything on this console |

## Core gameplay

| Feature | Old 3DS / 2DS | New 3DS / New 2DS XL |
|---|---|---|
| Boots and plays F-Zero X, all modes | ✅ | ✅ |
| 60 Hz game logic | ✅ | ✅ |
| Save files (race records, ghosts) | ✅ — the write-corruption bug below is fixed and hardware-confirmed | ✅ — same fix applies |
| Local multiplayer (2–4 player split-screen) | 🟡 — plays; not soak-tested on hardware | 🟡 — plays; not soak-tested on hardware |
| Stereoscopic 3D (3D slider) | 🟡 — on by default (part of `o3ds-auto`); plays, not yet a locked 60 fps, exact hardware figure not confirmed for this release | ⚪ — on by default (part of `n60`); supported in code, not separately hardware-measured yet |
| HOME menu / sleep / power-off | ✅ | ✅ |
| True widescreen races (`[display] widescreen = 1`, default off; from the PC port's widescreen options) | ⚪ — emulator-tested; costs frame rate in crowded races, hardware test pending | ⚪ — emulator-tested (2D, 3D, VS); hardware test pending |

## Native renderer (on by default on both consoles)

On by default on both consoles via `[perf] preset = auto` (the default with no ini, or no
`[perf] preset` key) — see RELEASE-NOTES.md "Performance" for the New 3DS (`n60`) and Old
3DS (`o3ds-auto`) lever sets it resolves to. An explicit `preset` key in `gdiffuser.ini`
always wins over the automatic choice; `preset = off` is the escape hatch back to the
pre-native-renderer defaults. See `docs/NATIVE-RENDERER.md` §4 for the technical detail.

| Feature | Old 3DS / 2DS | New 3DS / New 2DS XL |
|---|---|---|
| Native draw paths (track, racers, HUD, effects) | ✅ — extensively hardware-tested across many sessions; ships on by default (`o3ds-auto`) | ⚪ — hardware-correct in principle (same code path), on by default (`n60`); dedicated New-3DS hardware frame-rate run not done yet |
| Automatic Old-3DS detail scaling in crowded races | ✅ — on by default, no ini needed | — |

### Measured frame rate

This is a separate question from the table above ("is it on hardware, verified, by default"
vs. "how fast"). Full detail and sourcing: RELEASE-NOTES.md "Measured performance."

| Scene | Old 3DS / 2DS | New 3DS / New 2DS XL |
|---|---|---|
| Grand Prix, full 30-racer field | measured on hardware — 57.3 fps mean, log-flush stalls excluded (51.9 fps mean across the whole run including them) | not yet measured on hardware for this release |
| Time Attack, single racer | not yet measured on hardware for this release | not yet measured on hardware for this release |

## Expansion Kit (64DD; needs your own disk dump)

**The Old 3DS Expansion Kit build (`O3DS-EK`) is released as experimental from v1.1.0** (v1.0.0
shipped `N3DS-EK` only). On real Old 3DS hardware it
boots and mounts the disk, but 64DD courses are jittery, the disk music breaks up, and the
course editor, machine creator, and menus are very slow. See RELEASE-NOTES.md's Known issues.

| Feature | Old 3DS / 2DS | New 3DS / New 2DS XL |
|---|---|---|
| Released | 🟡 experimental, from v1.1.0 (`O3DS-EK`) | ✅ |
| Boots, reaches the main menu | ✅ | ✅ |
| Course editor (loads, playable) | 🟡 boots and loads, but very laggy on real hardware; see note above | ⚪ emulator-verified with a disk image, not yet exercised on hardware with one |
| Machine creator (loads, playable) | 🟡 boots and loads, but very laggy on real hardware; see note above | ⚪ same — emulator-verified only |
| DD cups race | 🟡 jittery/slow on real hardware; see note above | ⚪ same — emulator-verified only |
| Disk streamed from SD (no full in-memory copy) | ✅ | ✅ |
| Audio crackle | 🟡 greatly reduced, not eliminated; disk music also breaks up on real hardware (see note above) | 🟡 greatly reduced, not eliminated |

An optional English text pack for the Japanese-only Expansion Kit menus exists, but it is a
source-build-only option that needs your own fan-translated disk image to generate — it is
not in any of the three downloads above and isn't a per-console shipped capability, so it has
no row in this table. See RELEASE-NOTES.md's Expansion Kit section. The disk image itself used
to generate or play against must be the retail Japanese dump — a fan-translated disk image is
not supported in v1.0.0 (see Known open issues below).

## Bottom screen

| Feature | Old 3DS / 2DS | New 3DS / New 2DS XL |
|---|---|---|
| In-race dashboard ("DASH") | ✅ | ✅ |
| Three HUD placement modes | ✅ | ✅ |
| Idle view outside races | ✅ | ✅ |
| Forced to top screen in 2–4 player | ✅ | ✅ |

## Audio

| Feature | Old 3DS / 2DS | New 3DS / New 2DS XL |
|---|---|---|
| Music and sound effects | ✅ | ✅ |
| Resampler crackle fix | ✅ hardware-confirmed improvement | — issue was Old-3DS-specific |
| Residual faint crackle in some sessions | 🟡 open, rare | — |

## Known open issues

| Issue | Scope | Notes |
|---|---|---|
| Old 3DS memory ceiling | Old 3DS only | The real console's memory budget is thinner than most emulators simulate. An unusually large single asset load during a race can exhaust it. Rare in normal play. |
| Residual audio crackle | Old 3DS; markedly worse on Expansion Kit builds — see RELEASE-NOTES.md Known issues | Separate cause from the (fixed) resampler bug; not yet fixed. |
| Brief audio gap during some loads | Both consoles | Short pause while a large asset loads; see RELEASE-NOTES.md Known issues. |
| Rare ~826 KPH speed-readout spike | Both consoles | Cosmetic; appears briefly at the finish line. |
| Stereoscopic 3D not locked to 60 fps | Both consoles | Plays; a confirmed hardware frame-rate figure for this release isn't available yet. |
| Not every mode tested on every console | Both | Coverage is broad (see tables above) but not exhaustive — multiplayer and 3D soak testing in particular. |
| Expansion Kit gameplay content (course editor, machine creator, DD cups) is primarily emulator-verified | Both | The one real-hardware Expansion Kit session ran with no disk image mounted. A hardware pass with an actual disk image mounted hasn't been done yet for this release. |
| Old 3DS Expansion Kit is experimental | Old 3DS | Boots and mounts the disk, but 64DD courses stutter, the disk music breaks up, and the editors are very slow — `O3DS-EK` ships marked experimental; see RELEASE-NOTES.md Known issues. |
| Fan-translated Expansion Kit disk images can hang at boot | Both | Only the retail Japanese `baserom.jp.ek.ndd` is supported; see RELEASE-NOTES.md Known issues. |

## Where the documentation lives

Player- and modder-facing docs only; the repository also has internal/developer-process
docs (architecture notes, the hardware debug loop, diagnostics) not listed here.

| Path | Contains |
|---|---|
| `docs/RELEASE-NOTES.md` | What's new in this release, install/upgrade notes, known issues, measured performance |
| `docs/3DS-HARDWARE.md` | Toolchain, build, SD card layout, full ini reference, troubleshooting |
| `docs/BUILDING.md` | Building the console binary from source, including the one-command build-target tool |
| `docs/NATIVE-RENDERER.md` | What the native renderer is and every `[native]` ini key |
| `docs/MODDING_GUIDE.md` | Texture-pack modding |
| `docs/postmortem/` | The story of how this port was built |
