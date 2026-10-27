# Running G-Diffuser on a Nintendo 3DS

A player's guide: pick a build, get the assets onto an SD card, launch it, and tune the
settings that matter. This page covers real hardware. The same prebaked archives (§3) also
work with the Azahar emulator; running Azahar itself isn't covered in this release's docs.

## 1. What you need

| What | Why |
|---|---|
| **A Nintendo 3DS, 2DS, New 3DS, or New 2DS XL** | Any of them works — see §2 for which build to pick. |
| **Luma3DS >= 10.1.1** (custom firmware, boot9strap) | Homebrew execution, signature patches for the test-signed `.cia`, and audio support new enough for this game's sound to work. Follow <https://3ds.hacks.guide> if the console is stock. |
| **A way to run homebrew**: the Homebrew Launcher — reached from the Rosalina menu (always available under Luma) or a HOME-menu shortcut — for `.3dsx`, or **FBI** to install a `.cia` | Either works — see §5 for the install steps. If you don't already have FBI, get it from Universal-Updater or <https://github.com/Steveice10/FBI/releases>. |
| **An SD card with a few hundred MB free** | The game's archives are tens of MB; an Expansion Kit disk image is ~65 MB; saves and logs are small. |
| **`sdmc:/3ds/dspfirm.cdc`** (a DSP firmware dump) | Required for any audio. See §7 — without it the game runs, but silent. |
| **A PC with Python 3.8 or newer installed** | Needed once, to prebake your game assets — see §3. |
| **The `gdx-extract` tool** (downloaded separately) | A small PC-side extractor the prebake script drives — see §3 for where to get it. |
| **Your own F-Zero X (US) ROM**, pre-baked on a PC | The console never touches the ROM itself; see §3. |
| **(Optional) a 64DD F-Zero X Expansion Kit disk image**, if you want the Expansion Kit build | See §6. |

## 2. Pick a build

G-Diffuser 3DS ships as three builds:

| Build | Console | Content | File names |
|---|---|---|---|
| **O3DS** | Old 3DS / 2DS | F-Zero X | `G-Diffuser-3DS-O3DS.3dsx` / `.cia` |
| **N3DS** | New 3DS / New 2DS XL | F-Zero X | `G-Diffuser-3DS-N3DS.3dsx` / `.cia` |
| **N3DS-EK** | New 3DS | F-Zero X + Expansion Kit | `G-Diffuser-3DS-N3DS-EK.3dsx` / `.cia` |
| **O3DS-EK** | Old 3DS / 2DS | F-Zero X + Expansion Kit (**experimental**) | `G-Diffuser-3DS-O3DS-EK.3dsx` / `.cia` |

**O3DS-EK is experimental**: on real Old 3DS hardware it boots and mounts the disk, but 64DD
courses are jittery, the disk music breaks up, and the course editor/machine creator/menus are
laggy — see §11. On a New 3DS, use N3DS-EK.

A few things worth knowing before you pick:

- Old vs. New 3DS is a compile-time memory layout choice, not a quality setting — the console
  model is detected at startup either way, so an Old-3DS build still runs at full New-3DS
  settings if launched on a New 3DS. The reverse isn't true: **don't run the New 3DS build on
  an Old 3DS** — it assumes more RAM than an Old 3DS has, and is expected to fail at or soon
  after launch (see §12).
- The Expansion Kit (EK) adds the 64DD course editor (COURSE EDIT) and machine creator (CREATE
  MACHINE), using the retail F-Zero X 64DD disk. It's a separate build of the whole game, not an
  add-on, and the Old 3DS version is experimental (see above) — see §6 for what you need to supply
  and what happens if you don't.
- Vanilla and Expansion Kit builds install side by side (different HOME-menu titles, different
  save files) and share one SD-card folder for everything else.

If a build isn't available as a ready download, anyone with the devkitPro toolchain can build
it from source instead (§14).

## 3. Prebake the game's assets on a PC (required, every build)

The 3DS never reads your ROM directly, and nothing in the repository or the release download
contains game data. Instead, you convert your own legally-owned F-Zero X ROM into two archive
files **once, on a PC**, and copy them to the SD card. The same two archives work for every one
of the three builds above. You'll also need the `gdx-extract` tool for your PC's OS — download it
from the project's latest release page (see `tools/prebake/README.md` step 2) and pass its path
as `--extractor`:

```sh
python3 tools/prebake/prebake.py --rom /path/to/fzerox-us-rev0.z64 --extractor /path/to/gdx-extract
```

Full requirements, the Windows walkthrough, and troubleshooting: `tools/prebake/README.md`.

## 4. SD card layout

```
sdmc:/
├── 3ds/
│   ├── G-Diffuser-3DS-<BUILD>.3dsx       (if using the Homebrew Launcher — §5)
│   ├── dspfirm.cdc                       (§7 — DSP firmware dump, required for audio)
│   └── gdiffuser/
│       ├── fzerox.o2r                    (from §3 — REQUIRED; without it the game won't boot)
│       ├── gdiffuser.o2r                 (from §3 — always generate both together; without it
│       │                                  the game still boots but engine/UI assets are missing)
│       ├── hudbake.cache                 (from §3, optional — the pre-baked bottom-screen HUD; the
│       │                                  game bakes and writes it itself on first boot if absent)
│       ├── gdiffuser.ini                 (optional settings — §9-§10; the game writes this itself)
│       ├── media/
│       │   └── baserom.jp.ek.ndd         (Expansion Kit builds only — §6)
│       ├── saves/                        (created by the game; back this up)
│       │   ├── fzerox.sav
│       │   ├── fzerox-ek.sav             (Expansion Kit builds)
│       │   ├── baserom.jp.ek.ndd.gdd     (Expansion Kit disk sidecars — see §6)
│       │   └── baserom.jp.ek.ndd.dcrc    (same two sidecars, see §6)
│       ├── log.txt                       (vanilla build's log — §13)
│       ├── log-EK.txt                    (Expansion Kit build's log)
│       └── crash.txt / crash-EK.txt      (only if `[debug] crashdump = 1`, default off — §10)
```

`<BUILD>` is whichever of O3DS / N3DS / N3DS-EK you downloaded. The vanilla and
Expansion Kit builds read and write the same `gdiffuser/` folder, so there's only ever one of
each shared file even if you have both builds installed. The `.gdd`/`.dcrc` sidecar names above
are exact examples for the Japanese retail disk — they always take your disk image's full
filename (including its `.ndd` extension) and add `.gdd` or `.dcrc` to the end of it.

## 5. Install: `.3dsx` or `.cia`

Both come from the same build and share saves, settings, and logs — use either one, or both.

**`.3dsx` — Homebrew Launcher** (simplest; just a file copy):

1. Copy `G-Diffuser-3DS-<BUILD>.3dsx` to `sdmc:/3ds/`.
2. Open the Homebrew Launcher (any entry point works under Luma — the Rosalina menu's, or a
   HOME-menu shortcut) and start it from the list.

**`.cia` — installed HOME-menu title** (lets you launch it like any other game):

1. Copy `G-Diffuser-3DS-<BUILD>.cia` anywhere on the SD card.
2. On the console, open **FBI** → browse to the file → **Install CIA**.
3. The game appears on the HOME menu. The vanilla build is named "G-Diffuser"; the Expansion
   Kit build is named "G-Diffuser EK" and shows an "EXPANSION KIT" banner, so they're easy to
   tell apart if you install both. Uninstall later via FBI → **Titles** if you ever want to.

The `.cia` is test-signed, which is standard for homebrew — Luma's signature patches let it
install and run like a normal title. It will not install on a stock (non-CFW) console.

## 6. Expansion Kit: supplying the disk

No prebake step for this one — copy your disk image to the SD card as-is.

The N3DS-EK build needs a 64DD F-Zero X disk image to unlock the course editor and machine
creator. You supply your own; nothing in the repository or the download contains one.

Rename your dump to exactly this name and put it at this path:

- `sdmc:/3ds/gdiffuser/media/baserom.jp.ek.ndd` — the Japanese retail disk (the only retail
  version of this disk) — it works out of the box, no setting to change. Menus inside the
  Expansion Kit modes are Japanese.

**Fan-translated disk images aren't supported in v1.0.0.** A fan English translation of this
disk exists (Zoinkity's translation, adapted to disk form by LuigiBlood — announced at
<https://64dd.org/news/2020-05-24-fzxeng.html>), but the game identifies the disk by an exact
whole-image checksum, and a translation patched onto a different dump doesn't match that
checksum — on hardware, a translated image froze at boot instead of reaching the menu. Use the
retail Japanese dump above for now; English support is planned for a later release. See §11.

**Playing without a disk image works fine.** The Expansion Kit build still boots and plays the
normal game — COURSE EDIT and CREATE MACHINE on the main menu just don't open. The log records
`[leo] no disk image found; Expansion Kit modes stay disabled`, which is expected, not an error,
if you haven't supplied a disk yet.

It must be the full, untrimmed 64DD image, about 64.9 MB. A smaller file (anything under
60 MB) is rejected outright as truncated — that usually means a bad or partial download/dump.

**Streamed, not loaded into RAM.** The disk is read directly off the SD card as you play, not
copied into memory, so it costs almost no extra RAM on either console.

**First boot is slower.** The very first time a disk image is mounted, the game reads the whole
file once to fingerprint it — budget about a minute. It saves that fingerprint next to your
saves, e.g. `sdmc:/3ds/gdiffuser/saves/baserom.jp.ek.ndd.dcrc` — your disk image's exact
filename with `.dcrc` appended — so every later boot skips the wait. If you ever need to force
a re-check, delete that exact file.

Two sidecar files live next to your saves — they're not the same thing:

| File | What it is | Safe to delete? |
|---|---|---|
| **`.dcrc`** | The one-time disk fingerprint described above. | Yes, any time — the game just re-checks the disk and writes a fresh one on the next boot. |
| **`.gdd`** | Your own Expansion Kit edits: custom courses from COURSE EDIT, machines from CREATE MACHINE, and anything else that writes to the disk during play. The disk image itself (your `.ndd`) is never modified, so this file is where that content actually lives. | No — **back it up along with `saves/`**. Delete it (or lose it) and your custom courses and machines are gone, even though the disk image is untouched. |

## 7. Audio: dumping `dspfirm.cdc` (one-time, every console)

The audio backend needs the console's own DSP firmware, which Nintendo does not ship as a
downloadable file — you dump it from your own console once:

1. Install and launch **DSP1** (homebrew; Universal-Updater, or
   <https://github.com/zoogie/DSP1/releases>).
2. Press **B** ("Dump DSP firmware"). It writes `sdmc:/3ds/dspfirm.cdc`. Exit.

**Symptom without it:** the game boots and plays normally but is completely silent.

## 8. Controls

Slide/drift is N64 **Z** (left) and **R** (right) — N64 **L** is never read in-race. **Boost** is
a fresh N64 **B** press, available after lap 1. **Brake** is N64 **C-DOWN**, held.

| 3DS input | N64 input | Effect |
|---|---|---|
| Circle Pad | Control Stick | steer |
| A | A | accelerate |
| B | B | boost (after lap 1); back out of menus |
| Y | B | same as boost (a duplicate — see `y_maps_to_b` in §10) |
| X | Z | slide/drift left |
| L | Z | slide/drift left |
| R | R | slide/drift right |
| ZL / ZR *(New 3DS)* | B | boost |
| C-Stick *(New 3DS)* | C buttons | camera; C-Stick down also brakes |
| D-Pad | D-Pad | menus; steers too if the IN tab's D-PAD setting is FULL or RAMP |
| START | START | pause / confirm |

Attacks work as on the original N64 game: double-tap L or R (or X) for a side attack, or hold
one of L/R and double-tap the other for a spin attack. Holding L and R together cancels a drift.
ZL/ZR and the C-Stick only exist on New 3DS; on Old 3DS, boost stays on B/Y and slide-left on X.
Old 3DS has no C-Stick, so there is no default **Brake** button at all on that console — open
the IN tab (§9.1) and bind Brake to a button before you play.

**Every one of these is remappable.** The bottom screen's IN tab (§9) lets you tap any game
action and press a new 3DS button for it, independently of the table above.

## 9. The bottom screen

Outside the Homebrew Launcher or FBI menus, the bottom screen is G-Diffuser's own console. It has
two faces:

**During a race — DASH.** A live race readout (lap, position, speed, energy). Tap the small
**MODE** chip, or press **SELECT**, to cycle through three layouts:

1. classic — every readout stacked on the top screen, same as the original game
2. split — some readouts move to the bottom screen
3. all-on-bottom — the whole HUD moves to the bottom screen, top screen stays clean

In 2–4 player races the HUD always stays on the top screen (the bottom screen doesn't have room
to share four players' worth of readouts).

In Grand Prix, your spare machines show under ENERGY on the DASH. The frame rate shows as small
text (`60 FPS`) in the top-right corner of the DASH's course map; `[debug] bottom_fps = 0` hides
it (§10).

**At the title and main menu — Home.** The G-Diffuser logo, your pilot and console model along
the bottom, and a **SETTINGS** button at the bottom right. Picking **VS** on the main menu opens
the bottom screen's **MULTIPLAYER** menu (see [`MULTIPLAYER.md`](MULTIPLAYER.md)). Elsewhere in
the menus the bottom screen shows the logo on its own.

**SETTINGS** (tap it, or press **SELECT** on Home) opens the player settings, with these tabs
across the top:

| Tab | What it's for |
|---|---|
| **EXIT** | Back to Home. Always the first tab, black on yellow. **B** does the same outside races. |
| **DSP** | Display (display settings — not to be confused with the dspfirm.cdc DSP firmware from §7): border mode, rival detail, and true widescreen — see §9.1. |
| **3D** | Stereoscopic 3D depth, if you use the 3D slider — see §9.1. |
| **AUD** | Master volume. |
| **IN** | Steering feel, D-Pad steering, and button remapping — see §9.1. |

Everything you change there is saved to `gdiffuser.ini` as you go.

**The developer console.** Holding **L+R+SELECT for 1.5 seconds** opens the full console instead:
the tabs above plus status (**STA**), the on-device log (**LG**), low-level renderer toggles
(**DBG**), the multiplayer state (**MP**) and build information (**AB**). It's meant for
diagnosing problems rather than everyday play — see `DIAGNOSTICS.md` if you're asked to use it.
The same chord closes it. With `[debug] dev_console = 1`, SETTINGS itself carries the developer
tabs after the player ones (there is no DEBUG button on Home), and the race DASH shows a
**SETTINGS** button that opens the same pages.

### 9.1 Settings worth knowing about

- **Border mode (DSP tab):** four choices — AUTHENTIC (the original's letterboxed borders),
  **FULL-BLEED** (the default; the scene fills the whole 400×240 screen), ZOOM, and HYBRID.
  Full-bleed and hybrid can show a sliver of empty sky at the very edges in some scenes, since
  the original backdrops were painted for the narrower 4:3 view — authentic mode doesn't have
  this, at the cost of the black bars it was always drawn with.
- **True widescreen in races (DSP tab or ini, experimental, default off):** the DSP tab's
  WIDESCREEN row (or `[display] widescreen = 1`) takes effect the next time you start the game. It
  makes every race fill the top screen at its real 5:3 shape with nothing stretched — you see
  more of the course to the left and right, the field of view top to bottom is the original's,
  and the race HUD moves out to the screen edges. Menus keep their usual layout and your border
  mode. In VS split screen each view widens the same way. It draws more of the course and field
  (about a fifth more geometry per race frame, measured on the emulator), so on the Old 3DS
  expect a lower frame rate in crowded races. Ported from the G-Diffuser PC port's
  widescreen options; see §10.
- **Rival detail (DSP tab):** NATIVE / REDUCED / MINIMAL / LOW — simplifies distant rival
  machines for more fps in big fields. Default is REDUCED on Old 3DS, NATIVE on New 3DS (part of
  the `[perf] preset = auto` bundle, §10). An AUTO toggle on the same tab raises the cut
  automatically in a crowded race and relaxes it when the field thins out; whatever you pick by
  hand is treated as the floor it won't go below.
- **Stereoscopic 3D (3D tab):** on by default on both consoles, as part of the `[perf] preset =
  auto` bundle (§10) — there's no settings toggle for it, only the ini key (`[stereo] enabled`,
  §10); the 3D slider turns it down. Set `enabled = 0` in `gdiffuser.ini` and relaunch if you'd
  rather play in 2D. Depth (IOD) and convergence are live steppers on the 3D tab while the 3D
  slider is up. Slider down (or `enabled = 0`) renders identically to 2D. On New 3DS the menus
  also get layered 3D depth by default (`[stereo] menu_depth`, §10). On Old 3DS, multiplayer
  races are 2D by default (`[stereo] mp_3d = 1` for 3D, locked at 30 fps there).
- **Steering feel (IN tab):** circle-pad deadzone, range, and response curve, plus whether the
  D-Pad can steer (off / full / ramps up while held) — all four are live steppers with a pad
  readout so you can see the effect immediately.
- **Button remapping (IN tab):** tap any action in the list, then press the 3DS button you want
  it bound to; a RESET DEFAULTS option is on the same tab.

Everything above except `[stereo] enabled` is saved back to `gdiffuser.ini` automatically — you
never have to hand-edit the file unless you want to set something before first boot, or turn
3D off. See §10 for the ini keys directly.

## 10. `gdiffuser.ini` — the keys worth setting by hand

Lives at `sdmc:/3ds/gdiffuser/gdiffuser.ini`. It's plain text (`#`/`;` start comments, section
and key names aren't case-sensitive), and you can omit it entirely — everything below has a
sensible default. Most of these sync automatically from the console UI (§9.1); the rest — and
anything you want to set before first boot — go here by hand.

```ini
[input]
deadzone = 16      ; circle-pad units ignored around centre
range = 145         ; circle-pad units mapped to full stick deflection
curve = 0           ; 0 linear, 1 soft, 2 softer
dpad_steer = 0      ; 0 off, 1 full (D-pad = full stick), 2 ramp (grows while held)
y_maps_to_b = 1      ; 3DS Y also boosts, alongside B (so A+Y accelerate+boost is comfortable)

[display]
border_mode = 1     ; 0 authentic, 1 full-bleed (default), 2 zoom, 3 hybrid
widescreen = 0      ; 1 = races render at the top screen's real 5:3: wider view (Hor+), HUD at
                    ; the screen edges, no stretch (overrides border_mode in 1P races; VS keeps
                    ; border_mode's scene scale). Menus unchanged. Default 0 -- experimental,
                    ; not yet tried on hardware. Restart to apply.
widescreen_cull_pct = 115 ; with widescreen = 1, 1P races: how far past the screen edge the
                    ; course keeps drawing (100..200). 100 = the old exact-edge cull, which let
                    ; trackside buildings/walls vanish in the outer side bands. Display only.

[audio]
master_volume = 100 ; 0-100

[stereo]
enabled = 1          ; ON by default on both consoles (bundled with [perf] preset = auto below);
                     ; set to 0 and reboot to force 2D — not a console-UI toggle
iod = 12             ; depth, 0-40 px; default 12; live on the 3D tab while enabled
convergence = 0.25   ; 0.00-0.90; default 0.25; live on the 3D tab while enabled
menu_depth = 1       ; layered 3D depth in the menus (backgrounds behind, text in front).
                     ; Default 1 on New 3DS (part of the auto preset), 0 on Old 3DS; set
                     ; either way. Races aren't affected
mp_3d = 0            ; Old 3DS only: 1 = 3D in multiplayer races (locked at 30 fps there);
                     ; default 0 = multiplayer races in 2D (New 3DS: 3D as usual)

[net]
mp_lan = 1           ; PLAY NEARBY (local wireless + the same Wi-Fi). Default 1; 0 turns it off
rendezvous = 3.149.138.4:27200 ; PLAY ONLINE's server. Leave it out to use the project's hosted
                     ; server (the default in this release; temporary), set your own server's
                     ; dotted IPv4[:port], or `off` (or empty) to turn online rooms off
mp_name = PLAYER     ; your name in rooms (default: your 3DS profile name)

[perf]
preset = auto        ; auto (default, tuned per console model) | off | o3ds | n60 — see note below
draw_distance = 100  ; percent, 40-100
rival_detail = 0     ; 0 native, 1 reduced, 2 minimal, 3 low (floor for the AUTO toggle)
rival_detail_auto = 1 ; 1 = auto-raise the cut in crowded races (default); 0 = always use rival_detail as a fixed level

[debug]
console = 0          ; 1 = open the settings console on the bottom screen at boot
filelog = 0           ; 1 = mirror the boot/warning/crash log to sdmc:/3ds/gdiffuser/log.txt
filelog_max_kb = 1024 ; log size cap (1-8192); logging stops at the cap with a marker
crashdump = 0        ; 1 = write a crash dump (crash.txt / crash-EK.txt) if the game faults;
                     ; default off — see §13 for bug reports
bottom_fps = 1       ; 0 = hide the FPS counter in the DASH map's top-right corner (default on)
```

**A note on `[perf] preset`.** `auto` (the default) needs no ini at all: at boot it detects your
console model and applies the matching tuned bundle. New 3DS gets the `n60` set — the native
renderer (G-Diffuser's own F3DEX2 interpreter, replacing libultraship's) plus its matching native
track/machine/sky/HUD/fx paths. Old 3DS gets the equivalent `o3ds-auto` set, sized for its
smaller memory budget, plus the existing Old-3DS LOD scaling (rival detail drops to REDUCED;
draw distance stays stock). Both bundles also turn on stereo 3D (`[stereo] enabled = 1`) — set
`enabled = 0` explicitly if you don't want that. `preset = off` goes back to the bare stock
settings (no native renderer, no LOD scaling, stereo off) as an escape hatch; `preset = o3ds` or
`preset = n60` pin one bundle regardless of which console you're on. Any key you set explicitly
yourself — in `[perf]`, `[stereo]`, or elsewhere — always wins over whatever the preset would
have picked.

A few Expansion-Kit-only and diagnostic-only ini keys aren't reproduced here, since normal play
never needs them — see `NATIVE-RENDERER.md` §2 / `DIAGNOSTICS.md` if a bug report ever asks you
to set one.

## 11. Known issues

- A rare, cosmetic speed-readout spike (briefly showing an impossible number, around 826 KPH) can
  appear right at a finish line. It's a display glitch, not a race-affecting bug.
- Short audio gaps can occur during loading transitions.
- The Old 3DS has a firm memory ceiling — some heavy content may be tight on headroom. If
  something crashes only on Old 3DS and not New 3DS, that's the most likely reason.
- Hardware testing has covered the normal range of play, but not every mode on every console
  model yet — if you hit something that looks wrong, see §12.
- **The Old 3DS Expansion Kit build (O3DS-EK) is experimental.** It boots and mounts the disk,
  but 64DD courses stutter, the disk music breaks up, and the course editor, machine creator,
  and menus are very slow (§2).
- **Fan-translated Expansion Kit disk images can hang at boot.** Only the retail Japanese
  `baserom.jp.ek.ndd` is supported in this release — see §6.

## 12. Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| Instant exit back to the Homebrew Launcher, or "no archive mounted" | `fzerox.o2r` missing or misplaced (`gdiffuser.o2r` missing alone won't cause this — it only logs a warning) | Check `sdmc:/3ds/gdiffuser/` has both files; re-run the prebake tool (§3) — it refuses to produce a bad archive. |
| Boots but completely silent | no DSP firmware dump, or Luma3DS older than 10.1.1 (see §1) | §7 — DSP1, press B; update Luma3DS if it's old. |
| `.cia` won't install in FBI | stock console, or Luma not installed | Luma3DS is required — §1. |
| EK build's COURSE EDIT / CREATE MACHINE won't open | no Expansion Kit disk image found | §6 — check the path and exact file name; look for `[leo] no disk image found` in the log. |
| Disk image present but still not detected | wrong filename | §6 — rename it to exactly `baserom.jp.ek.ndd`. |
| First boot of an EK disk is slow (~1 min) | expected — the one-time disk fingerprint pass | Nothing to fix; later boots are fast. |
| EK disk image rejected as truncated | the `.ndd` is incomplete (under 60 MB) | Re-dump/re-download it; it should be about 64.9 MB. |
| Freeze at boot with an EK disk mounted | a fan-translated disk image — not supported in this release | Use the retail Japanese `baserom.jp.ek.ndd` instead — §6. |
| Crashes / black screen right after launch | wrong build for the console (ran the New 3DS build on an Old 3DS) | Use the O3DS build instead — its memory layout assumes more RAM than an Old 3DS has — §2. |
| No 3D even with the slider up | 3D is on by default, so something in `gdiffuser.ini` turned it off: `[stereo] enabled = 0`, or `[perf] preset = off` (the plain settings have stereo off). On Old 3DS, multiplayer races are 2D unless `[stereo] mp_3d = 1` | Remove the key (or set `[stereo] enabled = 1`) and relaunch (§10). |
| Multiplayer problems (CAN'T JOIN, UPDATE REQUIRED, NO SERVER ...) | — | See [`MULTIPLAYER.md`](MULTIPLAYER.md) "Troubleshooting". |
| Blank/frozen screen, or a hang | — | Set `[debug] filelog = 1`, reproduce, then pull `log.txt` (or `log-EK.txt`) — §13. |
| Luma shows its red/pink exception screen | a crash in the game or port code | Photograph the screen (it shows the PC/LR registers), then follow §13 for a bug report. |

## 13. Logs and bug reports

Logs live at `sdmc:/3ds/gdiffuser/log.txt` (vanilla build) or `log-EK.txt` (Expansion Kit
build), and are only written when `[debug] filelog = 1` is set (§10) — set it before
reproducing a problem. The file is capped at `filelog_max_kb` (1024 KB by default) and is
overwritten fresh each boot.

If you're filing a bug report, please include:

1. **Console + firmware**: model (Old 3DS/2DS, New 3DS/New 2DS XL), Luma version.
2. **Which build**: O3DS / N3DS / N3DS-EK / O3DS-EK, and
   `.3dsx` or `.cia`.
3. **How far it got**: menus, a race, a specific mode — and the last line of `log.txt` if it
   stopped partway.
4. **The whole log file** (`log.txt` or `log-EK.txt`) — it's small enough to attach in full.
5. **A photo of Luma's exception screen**, if one appeared.
6. **A photo or description of anything visually wrong**, if that's the issue.
7. **If you turned on `[debug] crashdump = 1` before the crash**, also attach `crash.txt`/
   `crash-EK.txt` (see §10; full setup, and the Old-3DS caveat if no file appears, are in
   `DIAGNOSTICS.md`).

## 14. Building from source

You don't need this to play — it's only for building the console binary yourself instead of
downloading it. One command, once devkitPro is installed, builds and names the output exactly
like a downloaded release:

```sh
tools/build-3ds.sh <o3ds|n3ds|o3ds-ek|n3ds-ek|all>
```

`o3ds-ek` is experimental (§2/§11) — it builds and runs, just poorly on real Old 3DS hardware.

Full toolchain setup and step-by-step instructions: `docs/BUILDING.md`.
