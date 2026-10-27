# G-Diffuser 3DS — Release Notes

## v1.1.0 — 2026-10-26

The multiplayer release: race friends over local wireless, on the same Wi-Fi, or over the
internet, with each console showing its own racer full screen. Also faster native-renderer menus
on both consoles, optional true widescreen races, new bottom-screen menus with a SETTINGS button,
more Old 3DS performance work, and a change to how the bottom-screen HUD art gets onto your
console (it is now built from your own game files). Same three downloads (O3DS, N3DS, N3DS-EK)
and the same SD card layout: install it over v1.0.0 and your ini, archives and saves carry over.

### Highlights

- **Multiplayer, 2–4 players, on by default.** Pick **VS** on the main menu and the bottom screen
  opens MULTIPLAYER: play nearby (local wireless or the same Wi-Fi) or online with a room code.
  No ini editing needed. Each console shows its own player full screen, and CPU machines can
  fill the field. Player guide: [`MULTIPLAYER.md`](MULTIPLAYER.md).
- **Faster menus.** The menus, menu transitions, course select and machine select now draw through
  the game's native 3DS renderer on both consoles.
- **SETTINGS on the bottom screen.** Home (the title and main menu) has a SETTINGS button for
  display, 3D, controls and volume.
- **True widescreen races (optional, experimental).** Shows more of the course at the top screen's
  real shape, with nothing stretched.
- **New bottom-screen menus**, a bigger speedometer on the DASH, your spare machines in Grand
  Prix, and an FPS counter.
- **HUD art from your own game files.** The DASH's fonts, digits, portraits and chrome are no
  longer compiled into the app: the game builds them on first boot from your own `fzerox.o2r`
  (see [HUD art](#hud-art-comes-from-your-own-game-files)).
- **Closing from the HOME menu no longer hard-locks the Old 3DS.**
- **A 3DS unlock-everything code** that works on any 3DS (see [`CHEATS.md`](CHEATS.md)).

### Multiplayer

**Getting in**
- On the main menu (SELECT MODE), pick **VS** with one console: the bottom screen opens the
  **MULTIPLAYER** menu, and the top screen stays put.
- **PLAY NEARBY**: host or join over the 3DS's **local wireless** (no router needed) or on the
  **same Wi-Fi**, with no internet or server. On by default (`[net] mp_lan = 0` turns it off).
- **PLAY ONLINE**: the host gets a 5-character **room code** and friends type it to join.
  Consoles connect directly where they can, and go through the server's relay when a router
  blocks that. This release points at the project's hosted server by default, so online rooms
  work out of the box. **That default is temporary**: a later release will drop it. Set
  `[net] rendezvous` to use your own server, or `off` to turn online rooms off (see "For
  self-hosters").
- **JOIN BY ADDRESS**: race over the internet with no server, when the host's router forwards one
  UDP port (`[net] mp_direct`).
- Nothing opens at boot: the radio and sockets come up only when you open the MULTIPLAYER menus
  or a room. The developer log link (`[net] enabled`) stays off and isn't needed.
- Your name comes from your 3DS profile and can be changed in the MULTIPLAYER menu.

**In the room**
- Everyone picks a machine on the normal machine select screen and the host picks the course.
  Timers keep things moving (60 s for machines, 30 s for the course).
- The host sets laps, room size (2–4) and how many CPU machines fill the field: up to 30, or up to
  16 when an Old 3DS is in the room.
- After the race: rematch, change the course, or pick machines again.
- **START WITHOUT** starts the race without a player who isn't ready. START RACE and leaving the
  room are held to confirm.

**During the race**
- Each console shows only its own racer, full screen, with the race HUD laid out for one player.
- If someone drops out, a CPU driver takes over their machine and the race goes on.
- Pausing pauses everyone and shows who paused. Ending the race from the pause menu is confirmed
  for everyone.
- Going to the HOME menu or closing the lid takes you out of that race, but you stay in the room.
- If the consoles ever disagree about the race, it stops cleanly and saves a report to the SD
  card.
- Online, if the host leaves a race of three or more, the race carries on for the others. A
  player whose connection drops rejoins the room automatically.
- On Old 3DS, multiplayer races run in 2D by default, aiming for 60 fps. Set `[stereo] mp_3d = 1`
  for 3D, which is locked to 30 fps there.

**Version checks**
- Everyone in a room needs the **same release download**, and the same edition: Expansion Kit and
  non-Expansion-Kit builds can't share a room. If a console doesn't match, the bottom screen says
  which side needs to update. Archives prebaked on different PCs from the same ROM match (the
  check covers what's in the archives, not how the PC compressed them).
- An online server that's newer than your game shows **UPDATE REQUIRED**. One that's older shows
  **SERVER NEEDS AN UPDATE**.

### Old 3DS performance

- The menus, menu transitions, course select and machine select now use the native renderer, as
  they do on New 3DS.
- 3D races ride out short slowdowns instead of dropping straight to 30 fps.
- The DASH is cheaper to draw during races: it updates on a budget and spreads its work across
  frames.
- Multiplayer races draw the local view through the native renderer, and CPU rivals use the
  lowest detail level while a session is live. Neither affects the race itself.
- One GPU timing change that measured slower on Old 3DS is now off there.
- 3D sends both eyes of the scene to the GPU in one submission (`[stereo] onesubmit`,
  `onesubmit_narrow`, on by default on both consoles): 3D menus run at about 45 fps instead of
  40, and 3D races gain about 1.5–1.8 ms of headroom a frame (the upper figure with both levers on, as shipped). No change on New 3DS, which already
  had the headroom.
- **Memory**: about 320 KB has moved off the main heap. The bottom-screen HUD art now lives in a
  single block of linear memory, and the course track data uses one buffer reserved at boot
  (see Fixes).

### Native renderer

These are on by default, as part of the tuned settings every console gets (`[perf] preset = auto`):

- On both consoles: the native menus listed above (`ni_menu*`, `ni_course`, `ni_mach`), the 1P
  race between checkpoints (`ni_race`), the HUD atlas reset on race entry (`atlas_race_reset`),
  the 3D frame's GPU overlap (`stereo_fold`, `[stereo] head_kick`, `[stereo] resume_fold`), and,
  on Expansion Kit builds, the course editor's 3D preview (`ek_editmesh`, `ek_editmesh_track`),
  its test drive (`nrr_testdrive`) and machine select (`ek_nimach`).
- New 3DS also runs VS split-screen and the records flyby on the native renderer (`ni_view`).
- As before, `[perf] preset = off` goes back to the plain, untuned defaults, and any key you set
  yourself wins. Lever by lever: [`NATIVE-RENDERER.md`](NATIVE-RENDERER.md).

### Bottom screen

- **New bottom-screen menus**: a Home screen at the title and main menu, the MULTIPLAYER flow
  (lobby, room code keypad, clear dialogs for every failure), key hints, D-pad navigation in the
  host and race-settings sheets, and hold-to-confirm on actions you can't undo.
- **SETTINGS**: Home's SETTINGS button (or SELECT on Home) opens the player settings: display
  (border mode, rival detail, widescreen), 3D depth, controls and button remapping, and volume.
  Changes are saved to `gdiffuser.ini` as you make them. **EXIT** (black on yellow, always the
  first tab) takes you back; **B** also exits when you're not in a race.
- **The old DEBUG button is gone.** The developer console (status, log viewer, diagnostics) opens
  when you hold **L+R+SELECT for 1.5 seconds**, or its tabs appear inside SETTINGS with
  `[debug] dev_console = 1`.
- **DASH layout pass**: the speedometer is bigger, uses the top screen's own digits and no longer
  sits on top of the map. LAP, TIME, BEST and GAP share one box, and long times and long track
  names are fitted so nothing is cut off. The KO stars move to the bottom of the dashboard.
- **Spare machines (Grand Prix)**: shown under ENERGY. Up to 5 show as one row, 6–10 as two rows,
  and more than that as one icon with a count.
- **FPS counter** in the top-right corner of the DASH map. `[debug] bottom_fps = 0` hides it.

### HUD art comes from your own game files

The DASH's art is no longer compiled into the app: the fonts, the timer, lap, speed and position
digits, all 30 character portraits, the KO star and spare-machine icons, and the dashboard
chrome. On first boot the game builds them from **your own `fzerox.o2r`**, using recipes that
reproduce the old art byte for byte.

- **First boot** takes about 0.5 s extra on Old 3DS and about 0.4 s on New 3DS.
- **Later boots** load the result from `sdmc:/3ds/gdiffuser/hudbake.cache` (about 100 KB) in
  about 40 ms.
- **If the archive is missing or wrong**, those elements are left blank and a line is written to
  the log. Nothing crashes.
- **Optional, on your PC**: the `gdx-extract` download now includes **`gdx-hudpack`**. When it
  sits next to `gdx-extract`, `prebake.py` uses it to write `hudbake.cache` for you, so even the
  first boot skips the bake. A cache from a different archive or a different game build is
  ignored, and the console rebuilds it.
- The track index templates (18 small tables) are now read from your archive at boot in the same
  way.

### Display and 3D

- **True widescreen (experimental, off by default)**: the DSP tab's WIDESCREEN row in SETTINGS (or
  `[display] widescreen = 1`), applied the next time you start the game, makes races fill the top
  screen at its real 5:3 shape. You see more of the course to the sides, the HUD moves to the
  screen edges, and VS split-screen and multiplayer views widen too. It draws about a fifth more
  per frame, so expect a lower frame rate in crowded races on Old 3DS. If scenery at the far
  edges pops, try `widescreen_cull_pct`. Ported from the PC port's widescreen options.
- **Menu depth**: with the 3D slider up, the menus get layered depth: backgrounds slightly behind
  the screen, panels and machine models on it, text slightly in front. On by default on New 3DS
  (part of its tuned settings), off on Old 3DS; `[stereo] menu_depth` sets it either way. Races
  aren't affected.

### Audio

- New 3DS: two crackle fixes that v1.0.0 only applied on Old 3DS are now on by default (a clamp
  for loud sound-effect samples, and a short fade instead of a click when audio runs dry). Turn
  them off with `[debug] adpcm_scale_clamp = 0` / `audio_declick = 0`. We haven't yet confirmed
  they remove the reported crackle; if you still hear it, please report it (see
  `3DS-HARDWARE.md` §13).
- Expansion Kit (built from source for Old 3DS, still unreleased): disk music and sound load in
  fewer, larger SD reads, off the audio thread.

### Expansion Kit

- The Expansion Kit build is still New 3DS only (N3DS-EK). You need your own dump of the retail
  Japanese disk, `baserom.jp.ek.ndd`. Fan-translated images are still not supported.
- Old 3DS EK (`o3ds-ek`) is still an experimental build you make from source, and isn't released.
- Building from source: the `fzerox-expansion-kit` submodule now points at
  `github.com/cruxxxxxx/fzerox-expansion-kit` (see [Assets and licensing](#assets-and-licensing)).
  In an existing clone, run
  `git submodule sync fzerox-expansion-kit && git submodule update --init fzerox-expansion-kit`.

### Fixes

- **Closing from the HOME menu no longer hard-locks the Old 3DS.** Saving the 64DD data at exit
  used to ask for one 73 KB block of memory, and when that wasn't available the app crashed and
  locked the console. The save now streams out without allocating anything and leaves the
  previous save untouched if it fails, and a fatal error during shutdown now exits the app
  instead of locking the console. Seen on an Expansion Kit test build; confirmed fixed on Old
  3DS hardware.
- **3D sky edges**: with the 3D slider high, a strip of plain colour could show at the left and
  right edges of the top screen. The sky, venue floor and cloud layers are now drawn wide enough
  to cover each eye's shift. 2D and slider-down frames don't change.
- Expansion Kit: VS BATTLE no longer does nothing with only one controller (it now opens
  multiplayer).
- Expansion Kit build with no disk image: no more boot hang, and no stuck music waits.
- Machine settings: the backdrop fills the screen with menu depth on.
- The 3DS unlock-everything code (SELECT MODE: L, R, D-Up, D-Down, D-Left, D-Right, START) no
  longer also picks a mode on its last START, and each D-pad press counts once.
- After several course loads in one session (about six on Old 3DS), a course could lose its fast
  track drawing path because memory had become fragmented. The track data now uses one buffer
  reserved at boot and reused for every course.
- `docs/3DS-HARDWARE.md` said 3D was off by default; it has been on (with the auto preset) since
  v1.0.0.

### Assets and licensing

Before this release we audited the repository and the release binaries. That audit showed that
**the v1.0.0 binaries and source contained some ROM-derived data**, which the project had said it
did not ship: the bottom-screen HUD art (generated from the game's textures and committed as C
arrays) and about 12 KB of track index data taken from the game. The upstream Expansion Kit
decompilation pinned as a submodule also had a retail 64DD disk image committed to it, most
likely by accident, so recursive clones and our CI downloaded it.

In v1.1.0:

- **HUD art and track data** are built on your console from your own archive (see
  [HUD art](#hud-art-comes-from-your-own-game-files)). Nothing ROM-derived is compiled into the
  binaries.
- **Disk image**: the EK submodule now points at a standalone copy with that file removed from
  its entire history. Its tree is otherwise identical to the old pin.
- **License files**: every release zip, including the `gdx-extract` zip, now includes `LICENSE`,
  `THIRD_PARTY_NOTICES.md` and `LICENSES/`.
- **README and notices** now say exactly what ships: no ROM, disk image or game assets. The
  binaries contain code compiled from the community decompilation, and you dump your own
  cartridge (and disk).

The v1.0.0 downloads contained some ROM-derived HUD art; v1.1.0 removes it and builds the HUD
from your own files.

The v1.0.0 downloads have been withdrawn from the releases page. v1.1.0 replaces them: use the
[latest release](https://github.com/cruxxxxxx/gdx-3ds/releases/latest).

### Upgrading from v1.0.0

1. Replace the `.3dsx`, or install the new `.cia` over the old one. Your `gdiffuser.ini`, both
   `.o2r` archives and your saves carry over, so you don't need to prebake again.
2. On the first boot the HUD art is built once, then cached. An old `hudbake.cache`, from a test
   build or from another archive, is detected and rebuilt automatically.
3. No ini keys were removed or renamed. Some defaults changed, and every key below is still
   honoured if you set it:
   - **Multiplayer**: NEARBY and the hosted online server are on out of the box.
     `[net] mp_lan = 0` or `[net] rendezvous = off` opts out.
   - **New tuned defaults (`[perf] preset = auto`)**: native menus, course select and machine
     select; one-submission 3D (`[stereo] onesubmit`, `onesubmit_narrow`); menu depth on New
     3DS; the New 3DS audio clamps (`[debug] adpcm_scale_clamp`, `audio_declick`).
     `[perf] preset = off` still returns to the plain defaults, and any key you set yourself
     still wins.
   - **Developer console**: `[debug] dev_console = 1` used to put a DEBUG button on Home. It now
     adds the developer tabs to SETTINGS instead.
4. For online play, everyone should update. A v1.0.0 console has no multiplayer, and v1.1.0
   consoles only join rooms with the same release.

### Known issues in v1.1.0

- Everything in the [v1.0.0 known issues](#known-issues) still applies, including the tight
  memory on Old 3DS and the rare ~826 KPH speed readout at the finish line.
- Widescreen and the fixed track buffer have been tested in the emulator but not yet on
  hardware.
- Multiplayer hasn't been soak-tested on every console pairing. If a race stops with RACE
  STOPPED, please attach the saved report and `log.txt` to a bug report.
- Old 3DS multiplayer in 3D is capped at 30 fps.
- New 3DS audio crackle: the fixes above are on, but not yet confirmed to cure it.
- The hosted online server is temporary. A later release will require you to set
  `[net] rendezvous` yourself.

### For self-hosters

- **The rendezvous server** (`tools/mp-rendezvous/gdx_rendezvous.py`, server 2.1.0, client
  protocol 9): one Python 3.8+ file using only the standard library. It needs one inbound UDP
  port (27200), keeps no accounts and writes nothing to disk. Run it with plain Python, Docker
  Compose (Linux, host networking) or the hardened systemd unit (`gdx-rendezvous.service`).
  Guide: `tools/mp-rendezvous/SELF-HOST.md`. Players point at it with `[net] rendezvous`.
- **Optional monitoring**: `docker compose --profile monitoring up -d` adds Prometheus, Loki,
  Grafana (a dashboard plus alert rules) and a UDP probe, all bound to 127.0.0.1. Settings are in
  `.env` (copy `.env.example`).
- **Version gating**: consoles send their client protocol. `--min-client-proto` and
  `--max-client-proto` (both default to this release's) decide who gets UPDATE REQUIRED or SERVER
  NEEDS AN UPDATE. `--update-url` (up to 40 characters) appears on the console's UPDATE REQUIRED
  screen. When a later release changes the protocol, run the new server with
  `--min-client-proto <previous>` to give players time to update.
- **Anti-spoofing**: CREATE and JOIN use a return-routability cookie (GXRV v2). `--allow-v1`
  admits pre-cookie development builds and should normally stay off.
- **Restarts**: `SIGUSR1` drains the server (no new rooms; it exits with code 75 when the last
  room closes). A plain restart drops every room.
- **Privacy**: the log holds player IP addresses. Use `--log-ip hash` or `none`, and keep logs
  short-lived.
- **No server at all**: NEARBY rooms are hosted by a console, and JOIN BY ADDRESS (`mp_direct`)
  works across the internet when the host forwards UDP 27300.

---

## v1.0.0 — 2026-10-19

> The v1.0.0 downloads have been withdrawn and replaced by v1.1.0 (see
> [Assets and licensing](#assets-and-licensing)). These notes are kept for reference.

The first release to support the **Old 3DS / 2DS** family, and the first to include the
**64DD Expansion Kit** (Course Edit, Create Machine, and the DD cups) on console.

### Highlights

- **Old 3DS / 2DS support.** The previous release required a New 3DS. This one runs on
  the original Old 3DS and 2DS too, with its own memory and performance tuning.
- **64DD Expansion Kit on console**, for New 3DS players who own their own Japanese 64DD
  disk dump. Streamed from the SD card — there is no room to hold the whole ~65 MB disk
  in memory on either console. Old 3DS Expansion Kit support and fan-translated disk
  images are not part of this release; see [Known issues](#known-issues).
- **A new bottom-screen dashboard ("DASH")** during races, with three HUD layouts and an
  idle screen between races.
- **A native 3DS renderer** that redraws part of each frame itself instead of going
  through the general-purpose interpreter the first release used for everything. It runs
  by default on both consoles, tuned per console, with no ini editing needed (see
  [Performance](#performance) below).
- **A New 3DS attract-demo freeze fixed.** See [What changed](#what-changed-since-v090)
  below for the technical detail; this matters for default, out-of-the-box play now that
  the native renderer runs there too.
- **Old 3DS audio crackle greatly reduced** by a resampler fix. Some faint crackle can
  still occur in a minority of sessions; see [Known issues](#known-issues).

### Downloads

Download the file for your console from the [latest release](https://github.com/cruxxxxxx/gdx-3ds/releases/latest).
Each release includes a `SHA256SUMS` file so you can check your download.
Each build also comes as a ready-to-unzip `G-Diffuser-3DS-<BUILD>-v1.0.0.zip`, laid out like an SD card.

| File | Console | Content | Who it's for |
|---|---|---|---|
| `G-Diffuser-3DS-O3DS.3dsx` / `.cia` | Old 3DS, Old 2DS | F-Zero X | Old-3DS-family owners who just want the game |
| `G-Diffuser-3DS-N3DS.3dsx` / `.cia` | New 3DS, New 2DS XL | F-Zero X | New-3DS-family owners who just want the game |
| `G-Diffuser-3DS-N3DS-EK.3dsx` / `.cia` | New 3DS, New 2DS XL | F-Zero X + Expansion Kit | New-3DS owners with their own 64DD disk dump |

Pick the build for your console; add `-EK` only if you have your own 64DD F-Zero X
Expansion Kit disk dump and want the course editor, machine creator, and DD cups — the
Expansion Kit is New 3DS only in this release (see [Known issues](#known-issues)). An
Old 3DS build also runs on a New 3DS, but a New 3DS build does **not** fit an Old
3DS's memory — don't install it there. Vanilla and `-EK` builds can both be installed
side by side; they share one ini, archives, and saves folder on the SD card.

### Installing, and upgrading from v0.9.0

First time installing? See `docs/3DS-HARDWARE.md` for the full setup walkthrough.

**Using the Expansion Kit build (new install or upgrade)?** It needs its own 64DD disk
image, placed at `sdmc:/3ds/gdiffuser/media/baserom.jp.ek.ndd` — the retail Japanese
dump; a fan-translated image is not supported in this release (see
[Known issues](#known-issues)) — see `docs/3DS-HARDWARE.md` §6 for the full layout. Not
sure which disk-image layout your dump is in? Check the file size:
about 71 MB (70,627,520 bytes) is MAME's layout — convert it first with
`tools/dev/mame2ndd.py tondd yourfile.mame baserom.jp.ek.ndd` (needs Python 3 on your PC;
no 3DS build toolchain needed — see Tooling below). About 65 MB (64,931,840 bytes) is
already this port's layout — just rename and copy it as-is. These are the only two sizes
to expect; anything else is a bad or truncated dump, not a third valid layout.

Already running v0.9.0? Here's what's different for you:

- **You still need your own game archives.** Nothing in any download contains game
  data. Bake `fzerox.o2r` and `gdiffuser.o2r` from your own F-Zero X ROM with
  `tools/prebake` (see `docs/3DS-HARDWARE.md`) and copy them to
  `sdmc:/3ds/gdiffuser/`, same as before.
- **Your existing `gdiffuser.ini` keeps working.** We are not aware of any ini key being
  renamed or removed since v0.9.0; anything new simply uses its default in an old ini file.
- **Your save carries over.** The save file format hasn't changed since v0.9.0
  (`sdmc:/3ds/gdiffuser/saves/fzerox.sav`) — a save written by an earlier build still
  loads correctly in this one. (v0.9.0 was New-3DS-only, so there's no old Old-3DS save
  to carry over.)
- **The Expansion Kit is new.** See the disk-image note above; vanilla and `-EK` installs
  share the same ini, archives, and saves folder.
- Settings you changed in-game are written back to `gdiffuser.ini` automatically, same
  as before.

### Performance

The native renderer is **on by default** on both consoles in this release. You don't need
to write any ini lines for performance — with no `gdiffuser.ini`, or no `[perf] preset` key
in one, `preset` resolves to `auto`, which picks the tuned configuration for your console at
startup:

- **New 3DS:** the `n60` lever set — native renderer, native race renderer, and frame-pacing
  tuning, with the render thread off.
- **Old 3DS:** `o3ds-auto` — the same tuning, minus a texture-cache headroom setting the New
  3DS has the RAM for, plus three Old-3DS memory/CPU safety levers, plus the existing
  automatic detail scaling in crowded races.

An explicit `[perf] preset` key in `gdiffuser.ini` always wins over the automatic choice.
On a New 3DS, you can set `preset = n60` by hand to force that same lever set explicitly.
**Don't do this on an Old 3DS:** `n60` applies the New 3DS's texture-cache headroom without
the Old 3DS memory/CPU safety levers that `auto` gives an Old 3DS for free, and it isn't a
supported or tested configuration there. `preset = off` is an escape hatch back to the
pre-native-renderer defaults, on either console:

```ini
[perf]
preset = off    # bare stock defaults; no native renderer
```

### What changed since v0.9.0

**Renderer**

- A native F3DEX2 (the N64's graphics-command format) interpreter and native draw paths
  for the track, the racer models, the bottom-screen dashboard, and the particle effects
  (sparks, smoke, explosions, force field), replacing libultraship's general-purpose
  interpreter for those parts of the frame. On by default on both consoles; see
  [Performance](#performance) for the per-console lever sets and how to turn it off.
- A long list of frame-pacing fixes that came out of chasing stutter on real hardware:
  the GPU command queue, display-list batching, and the stereoscopic 3D draw order were
  all reworked.

**Old 3DS**

- First-ever support for the Old 3DS / 2DS family. Runs with an 8 MiB RDRAM arena (vs.
  16 MiB on New 3DS) and streams some data that New 3DS keeps fully loaded in memory.
- Automatic racer-detail scaling in crowded races, tuned specifically for the Old 3DS's
  slower CPU.
- An audio resampler fix that removes most of the audio crackle reported on Old 3DS
  hardware.
- A save-file corruption bug fixed: on real SD cards (unlike most emulators), overwriting
  an existing save could silently fail, so only your very first save ever took effect.
  Saves, ghosts, and 64DD save sidecars now write safely every time.

**New 3DS**

- Fixed a reproducible freeze in the title screen's attract-mode demo loop (surfaces with
  the native renderer on, which is the default now), caused by a GPU command buffer that
  was too small for the demo's full 30-racer field.
- The same save-write fix as Old 3DS, above.

**Expansion Kit**

- Runs on console for the first time, on both Old 3DS and New 3DS: the build boots and the
  Expansion-Kit-only menu signs draw, confirmed on real hardware. The course editor, machine
  creator, and the Expansion Kit's own cups have been verified with a mounted disk image in
  the emulator; a hardware pass with an actual disk image mounted hasn't been done yet for
  this release (see `docs/STATUS.md`). **Only the New 3DS Expansion Kit build (`N3DS-EK`) is
  released in v1.0.0** — on Old 3DS, 64DD courses are jittery, the disk music breaks up, and
  the editors are very slow, so the Old 3DS Expansion Kit build isn't shipped this time; see
  [Known issues](#known-issues).
- The ~65 MB disk image is streamed from the SD card rather than held in memory — neither
  console has room to hold it whole.
- Source builds can now bake in an optional English text pack for the Expansion Kit's
  Japanese-only menus, generated from your own fan-translated disk dump with
  `tools/gen_ek_text_pack.py` before building. This is **not** in any of the three
  standard downloads above — it needs a translated image you supply and a from-source
  build. Your own disk dump stays the authority for everything except the on-screen
  words.
- Without a disk image, the Expansion Kit build still boots and plays the normal game;
  only the Expansion-Kit-only menu entries stay greyed out.

**Bottom screen**

- A new in-race dashboard ("DASH") showing live race information, with three HUD
  placement modes (classic top-screen HUD, split across both screens, or everything on
  the bottom screen), cycled from the mode button or a SELECT long-press.
- An idle view (checkerboard background, G-Diffuser logo) when not racing.
- In 2–4 player split-screen, the HUD is always on the top screen.

**Audio**

- The Old 3DS crackle fix above.
- Save-related audio stalls reduced: save writes now happen off the main thread.

**Tooling**

- `tools/build-3ds.sh` — for anyone building from source: one command builds any of the
  four targets (or all of them), producing both the `.3dsx` (Homebrew Launcher) and
  `.cia` (HOME menu) file for each, plus a `SHA256SUMS` file, straight into a `dist/3ds/`
  folder (see `docs/BUILDING.md`).
- A WiFi development link (3dslink + a small host tool), for anyone building from
  source, for iterating without repeatedly copying files to an SD card.
- `tools/dev/mame2ndd.py` — a bit-faithful converter between the two 64DD disk-image
  layouts (MAME's and this port's). A standalone script, no build toolchain needed —
  for players too: if your Expansion Kit disk dump is in MAME's layout, run this on it
  first (see the Expansion Kit install note above).

### Known issues

- A rare speed-readout spike (~826 KPH) can appear briefly at the finish line.
- Brief audio gaps are possible during some loading moments.
- **Old 3DS has a tight memory ceiling.** The console's real memory budget is thinner
  than most emulators simulate; an unusually large asset load during a race can run the
  Old 3DS out of memory. Rare in normal play; more likely with non-default content.
- Not every mode has been hardware-tested on every console. Coverage is good but not
  exhaustive — see `docs/STATUS.md` for the current state per feature and console.
- A small amount of audio crackle can still occur on Old 3DS in some sessions, even with
  the resampler fix above. The underlying cause of the fix is confirmed; a second source,
  worse on Expansion Kit builds, is still being tracked down.
- **The Expansion Kit is New 3DS only for now.** An Old 3DS Expansion Kit build boots, but
  64DD courses are jittery, the disk music breaks up, and the course editor, machine
  creator, and menus are very slow — so it isn't released this time (`tools/build-3ds.sh
  o3ds-ek` still builds it from source, as an experimental, unsupported target).
- **Fan-translated Expansion Kit disk images (`baserom.translated.ek.ndd`) are not
  supported.** The game identifies the disk by an exact whole-image checksum, and a
  translation patched onto a different dump doesn't match it; on hardware, a translated
  image froze at boot instead of reaching the menu. Use the retail Japanese
  `baserom.jp.ek.ndd` dump for now — English support is planned for a later release.
- Stereoscopic 3D plays but isn't yet a locked 60 fps on either console; see
  `docs/STATUS.md` for the current state.

### Measured performance

Hardware figures below are from Old 3DS consoles running the native renderer's `o3ds-auto`
lever set — the same one this release now applies by default (see
[Performance](#performance) above) — in races against the game's own AI field (not
WiFi-connected multiplayer).

| Mode | Scene | Excluding log-flush stalls | Whole run, including them |
|---|---|---|---|
| Grand Prix, 2D | Full 30-racer field, in-race | **57.3 fps mean** (4.6% of frames repeated) | 51.9 fps mean |
| Time Attack, 2D | Single racer | Not yet measured on hardware for this release | — |
| Grand Prix, stereoscopic 3D | Full field, 3D slider on | Not yet separately confirmed on hardware for this release | — |

("Log-flush stalls" are brief pauses while buffered debug-log lines get written to the SD
card; the hardware run this table is sourced from ran extra diagnostic logging on top of
normal play to get these numbers, so the "excluding them" column is the more representative
figure for ordinary play.)

New 3DS hardware frame-rate figures for the native renderer (also on by default there, via
the `n60` lever set) have not been captured separately yet — see `docs/STATUS.md`. The New
3DS has comfortably more CPU headroom than the Old 3DS at every setting tested so far.

### Thanks

To the F-Zero X decompilation and 64DD research this port is built on, to the libultraship
project this builds on for the PC side of G-Diffuser, and to everyone who tested beta
builds on real hardware along the way. The full story of how this port was built is in the
[post-mortem](postmortem/).

---

## Previously: v0.9.0 — 2026-09-04

The first public release. New 3DS / New 2DS XL only, built entirely on libultraship's
general-purpose interpreter (no native renderer, no Old 3DS support, no Expansion Kit on
console).
