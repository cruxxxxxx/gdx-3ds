# Running G-Diffuser on a real Nintendo 3DS

Everything you need to boot the port on hardware, and what to report back from the
first run. Emulator (Azahar) flow is covered by `tools/prebake/README.md`; this page
is the console-side companion.

## 1. Requirements

| What | Why |
|---|---|
| **New Nintendo 3DS / New 2DS** (any region) | The port targets the New3DS CPU profile: 804 MHz + L2 cache. The `.cia` requests these in its exheader (`SystemModeExt 124MB`, `CpuSpeed 804MHz`, `EnableL2Cache`); on an Old3DS it would run at 268 MHz and is expected to be unplayably slow. Old3DS is untested and unsupported for now. |
| **Luma3DS >= 10.1.1** (custom firmware, boot9strap) | Homebrew execution + signature patches for the test-signed `.cia`, and a DSP/audio path new enough for the ndsp backend. Follow <https://3ds.hacks.guide> if the console is stock. |
| **SD card with a few hundred MB free** | `fzerox.o2r` is tens of MB; logs and saves are small. |
| **`sdmc:/3ds/dspfirm.cdc`** (DSP firmware dump) | Required for ANY audio. See section 4 — without it the game runs, but silent. |
| **Your own F-Zero X US rev0 ROM, pre-baked on a PC** | The console never sees the ROM; you bake `fzerox.o2r`/`gdiffuser.o2r` once with `tools/prebake` (see its README) and copy them over. |

## 2. Choose an entry point: `.3dsx` or `.cia`

Both are built from the same ELF and share saves, config, and logs — pick either
(or use both; they coexist).

**`.3dsx` — Homebrew Launcher** (recommended for the first run; trivially updatable):

1. Copy `build-3ds/port/3ds/G-Diffuser-3DS.3dsx` to `sdmc:/3ds/`.
2. Launch the Homebrew Launcher (on Luma any entry point works, e.g. the bundled
   one on Rosalina > Miscellaneous or the HOME-menu title) and start G-Diffuser.

**`.cia` — installed HOME-menu title** (target `G-Diffuser-3DS-cia`; needs
makerom + bannertool, see `port/3ds/packaging/README.md`):

1. Copy `build-3ds/port/3ds/G-Diffuser-3DS.cia` anywhere on the SD card.
2. On the console open **FBI** > `SD` > navigate to the file > `Install CIA`.
3. G-Diffuser appears on the HOME menu (title ID `000400000FF3D500`). Uninstall via
   FBI > `Titles` if ever needed.

The `.cia` is test-signed (standard homebrew practice); Luma's sigpatches make it
install and boot like any title. It will not install on a stock console.

## 3. SD card layout

```
sdmc:/
├── 3ds/
│   ├── G-Diffuser-3DS.3dsx        (if using the Homebrew Launcher route)
│   └── gdiffuser/
│       ├── fzerox.o2r             (from tools/prebake — REQUIRED)
│       ├── gdiffuser.o2r          (from tools/prebake — engine archive)
│       ├── gdiffuser.ini          (optional, section 5)
│       ├── log.txt                (written by the port when debug.filelog=1)
│       └── saves/                 (created by the game; back this up)
└── dspfirm.cdc                    (section 4 — actually at sdmc:/3ds/dspfirm.cdc)
```

Produce the `3ds/gdiffuser/` archives with `tools/prebake` on your PC
(`python3 tools/prebake/prebake.py --rom fzerox-us-rev0.z64 --build-dir build`),
then merge the staged `dist/sdmc/3ds/` folder into the SD card's `3ds/` folder —
full details and archive validation gates in `tools/prebake/README.md`.

## 4. Audio prerequisite: dump `dspfirm.cdc` (once)

The ndsp backend needs the console's DSP firmware blob at `sdmc:/3ds/dspfirm.cdc`.
Nintendo does not ship it as a file, so you dump it from your own console once:

1. Install/launch **DSP1** (homebrew, available in the Universal-Updater store or
   from <https://github.com/zoogie/DSP1/releases>).
2. Press **B** ("Dump DSP firmware"); it writes `sdmc:/3ds/dspfirm.cdc`. Exit.

**Symptom without it:** the game boots and plays normally but is completely
silent. The backend logs `[audio-out] ndspInit FAILED rc=0x........` and the boot
sequence logs `audio output NULL SINK — game will be SILENT ...` (svc debug
channel, `log.txt` with `debug.filelog=1`, and the bottom-screen console with
`debug.console=1`) and degrades to a null sink that keeps engine timing intact —
so silence means "check dspfirm.cdc" first, not "audio is broken".

**Azahar/Citra needs the file too.** libctru loads the DSP component only from
`sdmc:/3ds/dspfirm.cdc`, and that lookup happens inside the emulated app as well
— so copy the same dump into the emulator's **virtual SD** at `3ds/dspfirm.cdc`
(Azahar: Emulation > Configure > System > open the SD root, or the
`sdmc/` directory under Azahar's user folder). Without it Azahar is exactly as
silent as an undumped console. (Earlier docs claimed emulators don't need the
file; that was wrong.)

**Triage receipts:** set `debug.diag_audio = 1` for a ~5 s periodic `[audio-out]`
line (chunks submitted / played / nonzero, producer push totals, a sample OR that
stays `0x0000` only if the game pushed pure silence). Set
`debug.audio_testtone = 1` to replace all game audio with a 440 Hz sine at the
output stage: tone audible = output path works (suspect game/audio content);
still silent = ndsp/DSP path broken.

## 5. `sdmc:/3ds/gdiffuser/gdiffuser.ini` (optional)

All keys with their defaults; omit the file entirely for defaults. `#`/`;` start
comments; keys and section names are case-insensitive.

```ini
[input]
deadzone = 16      ; circle-pad units ignored around center (raw range ~±156)
range = 145        ; circle-pad units mapping to full N64 stick deflection (±80)
y_maps_to_b = 1    ; 3DS Y acts as N64 B (accelerate comfortably with A+Y)

[audio]
lle = 0            ; 1 = route audio tasks through the cxd4 RSP LLE interpreter.
                   ; TEST ONLY: expected to blow the New3DS CPU budget; HLE (0)
                   ; is the supported synthesis path (port/3ds/audio/AUDIO_NOTES.md)

[debug]
console = 0        ; 1 = text console on the bottom screen (boot steps, warnings)
diag_audio = 0     ; 1 = periodic [audio-out] output-path receipts every ~5 s
                   ;     (see section 4 "Triage receipts")
audio_testtone = 0 ; 1 = 440 Hz sine replaces ALL game audio at the output stage
                   ;     (bisects game-content vs output-plumbing silence)
filelog = 0        ; 1 = mirror boot/watchdog/fatal tracer lines to
                   ;     sdmc:/3ds/gdiffuser/log.txt (truncated each boot)
filelog_max_kb = 256 ; log size cap (1-8192); logging stops at the cap with a marker
```

## 6. Controls

In-race semantics verified against the decomp (`decomp/src/game/racer.c`): the
slide/drift pair is N64 **Z + R** (Z = left, R = right; N64 L is not read
in-race at all), **boost** is a fresh N64 **B** press (after lap 1), and
**brake** is N64 **C-DOWN** held.

| 3DS input   | N64 input | In-game effect |
|---|---|---|
| Circle pad  | Stick     | steer (tilt up/down on stick Y) |
| A           | A         | accelerate |
| B           | B         | boost (tap, after lap 1); menus: back |
| Y           | B         | same as B (duplicate; disable with `input.y_maps_to_b = 0`) |
| X           | Z         | slide left / attacks (Old3DS-reachable Z) |
| L           | Z         | slide/drift left |
| R           | R         | slide/drift right |
| ZL          | B         | boost (New3DS) |
| ZR          | B         | boost (New3DS) |
| C-stick     | C buttons | camera; C-stick down doubles as brake (New3DS) |
| D-pad       | D-pad     | menus |
| START       | START     | pause / confirm |
| SELECT      | —         | reserved |

Attacks work like the N64 original through the mapping: double-tap L or R (or X)
for a side attack; hold one of L/R and double-tap the other for a spin attack.
Holding L and R together cancels the drift, as on N64 Z+R. ZL/ZR and the C-stick only
exist on New3DS; on Old3DS boost stays on B/Y and slide-left on X. N64 L is
deliberately unmapped — nothing in-race reads it.

## 7. Known issues / expectations (first hardware pass)

- **Frame rate on hardware is unmeasured.** In the Azahar-based M1 bring-up the
  port runs around ~26 fps in-race after the texture-cache fix; emulator numbers
  do not transfer to the real GPU/CPU, in either direction. Expect anything.
- **Residual texture-animation cache misses** (~1-3 uploads/frame while driving)
  are known and budgeted; visible impact should be none.
- **Attract mode occasionally fails to start** after abnormal emulator teardowns
  in testing; not yet reproduced under normal boots. If the title screen sits
  forever without demo playback, note it (see telemetry below).
- **No ImGui menus / ghost browser / Discord** — dropped by design on 3DS; the
  INI above is the whole configuration surface.
- The **bottom screen** is the debug console (when enabled) — no touch UI yet.

### First-run telemetry — what to report back

Boot once with `debug.console = 1` and `debug.filelog = 1`, play a race (or get
as far as it goes), quit (or power off after a hang/crash), then copy
`sdmc:/3ds/gdiffuser/log.txt` off the SD card and report:

1. **Console + firmware**: model (N3DS/N3DS XL/N2DS), Luma version, system fw.
2. **How far it got**: HOME/HBL > logo > menus > race, and the LAST `[G-Diffuser-3DS]`
   boot step in `log.txt` if it stopped early.
3. **The `[watchdog]` lines** from `log.txt` — one per 5 s; the `frame=N(+delta)`
   deltas are the effective fps × 5 (e.g. `+130` ≈ 26 fps). Note the delta while
   sitting in menus vs. mid-race.
4. **Audio**: sound OK / silent + whether the `ndspInit failed` line is present.
5. **Any `[fatal]` / `[fatal-trace]` lines** (these are the crash tracers) and
   whether Luma's exception screen appeared (photo of it if so).
6. **Visuals**: anything obviously wrong (missing geometry, wrong colors, fog),
   ideally with a photo — plus whether attract mode starts on its own.
7. `log.txt` attached whole (it is capped at 256 KB).

## 8. Troubleshooting quick table

| Symptom | Likely cause | Fix |
|---|---|---|
| "WARNING: no archive mounted" / instant exit to HBL | archives missing/misplaced | check `sdmc:/3ds/gdiffuser/fzerox.o2r` exact path; re-run prebake (its golden gate catches bad bakes) |
| Boots but silent | no DSP dump | section 4 (DSP1 > B) |
| `.cia` won't install in FBI | stock console / no sigpatches | Luma3DS required (section 1) |
| Hang/crash with blank screens | — | enable `debug.filelog=1`, reproduce, pull `log.txt`; the watchdog + fatal tracers name the stuck stage |
| Luma exception screen | crash in guest/port code | photo the screen (PC + LR), grab `log.txt`, report both |
