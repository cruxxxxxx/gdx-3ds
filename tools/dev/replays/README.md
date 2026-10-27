# Committed replays (`gdx-dev emu run --replay <id>`)

Scripts here replace the `/tmp/*-autoinput.txt` files that every agent rewrote and lost
(05-devcli-plan.md §2 item 6). Grammar: `port/input_bridge.c:208-236`, validated at stage
time by `gdxdev/replay.py` — first non-blank line must be `ticks` (seconds mode is
wall-clock and not deterministic), events tick-ascending, at most 64.

Three hard-won rules. The first two are from `docs/research/RESUME.md:150-156`; the third
was found here on 2026-09-07 and cost a whole emulator run:

1. **Interleave `SHOT` lines among the button lines.** A trailing block of SHOTs after the
   last button line is silently dropped at parse: the log says "38 events", the file has
   more, and you get zero BMPs.
2. **Tick budget.** `s_tick` ≈ the `[c3d] frame` counter 1:1. A scripted race reaches the
   course-intro card around tick 3300 and actual driving (`[race-dl]` present) around
   4600, i.e. 6-7 minutes of wall clock in the emulator. Drive SHOTs belong at 4600+.

3. **Keep every line under 62 bytes — comments included.** Builds predating the
   `input_bridge.c` line-buffer fix read the script with `char line[64]`, so a longer
   line was split across two `fgets` calls; the tail does not start with `#`, so an
   over-long COMMENT was parsed as the first data line, the `ticks` sniff failed, and the
   whole script silently ran on the wall-clock seconds timebase with every SHOT dropped.
   The symptom is a log line like `[input] autoinput script loaded: 1 events (seconds
   timebase)` from a file that plainly starts with `ticks`. The port is fixed, but
   replays are also staged onto SD cards and older builds, so `gdx-dev` refuses to stage
   a file with a long line.

4. **A HOLD IS ONE PRESS. Walk menus with taps.** Every menu and the title read
   `gInputButtonPressed`, the rising EDGE of the pad (`common.c:185`, `title.c:233`,
   `main_menu.c:253`); only d-pad cursor moves auto-repeat (`gInputPressed`). So
   `20 A 2980` / `3000 A 2980` ... is one A press per 3000 ticks, not an A-mash. See
   "Why storm2 never reached a race" below.

Shots land as `<label>_L.bmp` (top screen) and `<label>_R.bmp` (bottom screen / DASH)
in the run's `shots/`.

**An autoinput replay ends at its last tick + 120.** `replay_quit=1` (implied by
`--replay`) only fires on a `.gdxrec`'s `[rec] end`; a `.txt` script never logs one, so
these used to run to the `--seconds` kill and lose every exit-time flush. `retro run` now
also implies `[debug] quit_at_tick=<last tick + 120>` for a `.txt` replay (private SD
only; `emu run` warns instead, because the Qt lane's ini is shared and a leftover
`quit_at_tick` would cut every later run short). `--no-replay-quit` or an explicit
`--ini debug.quit_at_tick=N` wins.

| id | what it does | needs | typical `--seconds` |
|---|---|---|---|
| `boot` | boots, one title-screen SHOT | — | 60-90 |
| `menu` | taps through the title: `title` 600, `menu1` GP difficulty box, `menu2` NOVICE picked, `menu3` SELECT COURSE | — | 180 |
| `storm2` | taps into a 30-machine GP (JACK, Mute City, race at ~3300): `card` 3400 = the course card, `drive1..5` mid-drive — the campaign's A/B replay | see below | 420 |
| `item3-boostmsg` | storm2's walk; `drive1` 5000, `before_lap2` 5880, `after_lap2` 5990, `after_lap2b` 6080 bracket lap 1 -> 2 (~5930) | **`--ini debug.autodrive=1`** | 180 |
| `ek-*` (`.gdxrec`) | Expansion Kit editor / disk replays (headers explain each) | an EK build, `--ini debug.det=1`, the JP `.ndd` under the instance's `media/` except `ek-editcup` (no disk) | 120-300 |
| `race-pause` (`.gdxrec`, src `.gdxs`) | GP on MUTE CITY; START at 5000 opens the in-race PAUSE box, the cursor walks Retry / Settings / Quit / (wrap) Continue and back, A resumes; START at 7000 + Retry restarts the race (retry transition, intro, countdown). 57 hand-placed SHOTs | `--ini debug.autodrive=1` | 120-300 |
| `race-vs2p-results` (`.gdxrec`, src `.gdxs`) | VS BATTLE 2 PLAYERS on MUTE CITY: P2 (mirrored pad) drives into the wall and RETIREs, P1 (autodrive) wins; the WINNER cards, the VS TOTAL RANKING table and its Select Course / Quit menu, back to COURSE SELECT. 37 SHOTs | `--ini debug.pads=2 --ini debug.autodrive=1` | 120-300 |
| `race-tt-nameentry` (`.gdxrec`) | `race-timeattack-mc-finish` to tick 11099, then the new-record NAME ENTRY keyboard driven key by key (letters, B delete, END) and the saved records table. 100 SHOTs | `--ini debug.autodrive=1` | 120-300 |
| `race-gp-cup` (`.gdxrec`, src `.gdxs`) | a whole JACK cup (NOVICE) PLAYED: six races, every post-race screen, then the GP ENDING (`GAMEMODE_GP_END_CS`, 0x11) reached by play at ~56220, not by `force_mode`, and out to the main menu at ~61380. 118 SHOTs | `--ini debug.autodrive=1 --ini debug.filelog_max_kb=65536` (the 4 MB default log cap stops the log mid-cup) and `--seconds 3000` | 900-1500 |

The four `race-*` rows above (ni-strict-census, 2026-10-21) are hand-placed like the `ek-*` files, not
covsched corpus replays: they exist for the S6/S7 screens no corpus replay reached (native-all-plan.md
§3 S6). Each passes `retro run --ini debug.det=1` (plus its needs) on `--model new` (N3DS build) and
`--model old` (O3DS build).

### storm2: what the drive SHOTs show

Unsteered, the racer hits the wall and RETIREs ~1100 ticks after the race starts; the A
re-press every 500 ticks takes it through the restart card and machine settings and back
into the race, a loop that measured exactly 3000 ticks (races at ~3300, 6100, 9100,
12100, 15100, 18100; the sixth retire ends the GP at ~20600). `drive1..5` (4000, 6850,
9850, 15850, 18850) sit mid-drive in those races. With `--ini debug.autodrive=1` the CPU
drives player 1 instead (race 1 3300-~11000, race 2 Silence from ~13100), and the same
five ticks are all inside a race too. Measured on release-v1 b7f0df0: plain twice on
N3DS and once on O3DS (identical gstate mode timelines, all six SHOTs on the intended
scene), autodrive once on N3DS.

### Why storm2 never reached a race (root cause, 2026-10-20)

It was written (2026-09-07, `feat(devcli)`) as "A-mash" but authored as A HELD for 2980
ticks with a 20-tick release, i.e. one rising edge per 3000 ticks, and it was never run
to a race before being called the campaign's A/B replay; `item3-boostmsg` (2026-10-16)
copied its timing and was never captured either. Nothing in the input bridge changed:
`port/input_bridge.c` has fed autoinput in before the edge accumulator since the first
commit. Two things made the one-edge-per-3000 walk visible as a hang on release-v1:

- the first edge (tick 20) lands during boot. Whether it survives to the title depends on
  boot timing (the logo hold is wall-clock unless `[debug] det=1`), so the title is
  passed at ~700 on one run and not at all on another;
- if it is not, the title's audio-driven attract demo starts at ~900 and the 3000/6000
  edges are spent leaving demos (measured: GP demo 900, VS-2P 3100, VS-4P 5300). Drive3
  landed on the SELECT MODE difficulty box, as reported.

Even with the title passed, one edge per 3000 ticks reaches SELECT COURSE around 9000 and
the race around 15000, never the card at 3400. `menu` had the same first line and got
lucky on the title more often; it now taps at 650, after the title is ready (~380) and
before the attract demo (~900).

### The `.gdxrec` corpus (`<repo>/replays/`)

Checked on the same day on N3DS release-v1 with `--ini debug.det=1` (plus each file's
own "Needs" from `replays/README.md`): every one reaches the scenes its README row names
and completes with every SHOT captured. `race-mutecity-start` / `probe1418` retire ~100
ticks earlier than the recording's `# mode 15 tick 4895` marker (gstate mode 15 by 4800),
a small content drift in the last ~1200 ticks, not a menu desync.
