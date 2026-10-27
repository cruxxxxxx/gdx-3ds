# The native renderer

What the native renderer replaces, how it is switched on, how each piece is checked, and what has
actually been measured on hardware. This is the renderer the v1.0.0 release ships with — every
individual lever below still defaults to `0` (the *old*, libultraship-interpreter-parity path) at
its own call site, but `[perf] preset` itself now defaults to `auto`, and `auto` resolves to a
built-in table that turns the native renderer and NRR on for your console model (§4). **A plain
`gdiffuser.ini` with no `[perf]`/`[native]` section at all still boots into the native renderer on
both consoles** — you do not need to write any ini lines for performance. The result numbers and
the honest list of what has *not* been run are in [`RELEASE-NOTES.md`](RELEASE-NOTES.md). For
where this fits in the rest of the port, see [`ARCHITECTURE.md`](ARCHITECTURE.md) §4.3.

---

## 1. What it replaces

Everything the old path draws goes through libultraship's Fast3D interpreter — vendored into this
tree as a frozen carve (`port/3ds/vendor/lus/`, see `ARCHITECTURE.md` §4.2) rather than linked from
the desktop submodule — fed by a bridge (`port/n64_gfx_bridge.cpp`) whose only job is to widen the
game's 32-bit `Gfx` words into the 64-bit words that interpreter wants. Every triangle in
F-Zero X is transformed and lit on the ARM11 CPU, one display-list command at a time.

The native renderer replaces that, piece by piece, with code that writes PICA200 vertex buffers
directly — and keeps the old path alive beside it. It is a **hybrid** renderer by design, not a
rewrite: the menus, the course editor, the ending and the long tail of one-off screens stay on an
interpreter.

Two tiers, which combine:

- **Tier 1 — own the interpreter** (`interp`). A purpose-built F3DEX2→citro3d walker
  (`port/3ds/gfx/n64/`) replaces libultraship's interpreter *and* the widening bridge. Same
  pixels, no conversion pre-pass.
- **Tier 2 — bypass the interpreter for the three things a race spends its frame on.** `track`
  replays the course tessellator's vertex stream straight into a VBO; `machines` draws the 30
  racer meshes from baked, retained lists (150 lists = 30 slots × 5 LODs), one indexed draw per
  material run; `fx_sink` / `fx_sprites` / `fx_hud` / `ownfold` batch the effect sprites and the
  HUD.

### How the tiers combine

Tier 2 works by making the game emit a **marker** where its geometry commands used to be
(`track`, `fx_sink`), or by redirecting a registered display list at `G_DL` (`machines`). The
replays behind those hooks are written against one interpreter seam
(`port/3ds/gfx/gdx3ds_interp_seam.h`) with an adapter per interpreter
(`gdx3ds_seam_lus.cpp`, `n64/gdx_seam_owned.cpp`), so the owned interpreter expands the same markers
and draws the same meshes through the same code the libultraship interpreter does. `all=1` does
**not** imply `interp=1`; the two are set independently and every combination is valid.

---

## 2. The `[native]` ini section

All keys live in `sdmc:/3ds/gdiffuser/gdiffuser.ini`, section `[native]`, and are **read once at
boot** — changing one needs a restart. Section and key names are case-insensitive. Every
*feature* lever defaults to `0` at its own call site, which is the exact prior path — but the
shipped `[perf] preset = auto` default (§4) sets most of them to `1` for you, so the game you
actually boot already has the native renderer on. These per-key defaults are what you fall back
to by setting an individual key explicitly, or by setting `preset = off`.

To try the whole thing by hand instead of relying on the shipped preset:

```ini
[native]
all = 1
interp = 1
```

### 2.1 Feature levers

| key | default | what it does |
|---|---|---|
| `all` | `0` | Master for the **Tier-2 levers only** (`machines`, `track`, `fx_sink`, `fx_sprites`, `fx_hud`, `ownfold`). `1` sets every one of those that is unset (`0` or absent) to `1`. A lever already explicitly set to `2` (verify) is left alone. Never touches `interp`. |
| `interp` | `0` | `1` = the owned F3DEX2→citro3d interpreter draws the frame instead of libultraship (LUS) Fast3D + the bridge. Always explicit — `all` does not imply it. Carries the Tier-2 hooks, so it combines with every lever below. |
| `machines` | `0` | `1` = the 30 racer meshes draw from baked retained lists, one indexed draw per material run. |
| `track` | `0` | `1` = the course tessellator's vertex loads and index templates are recorded and replayed into a native batch, one draw per material run. `2` = verify. |
| `fx_sink` | `0` | `1` = the six effect sprite ring classes (debris, sparks, smoke, explosions, force field, fall explosions) are recorded at the game-side macro and replayed as native per-class batches. `2` = verify. |
| `fx_sprites` | `0` | With `fx_sink`: fold prim/env colour changes into the vertex colour where the backend carries that input per vertex, so a colour change stops splitting the draw. |
| `fx_hud` | `0` | The same fold rule inside **libultraship's** 2D batches — HUD digits, stars, markers stop splitting on every prim colour. Inert under `interp=1`; `ownfold` is its counterpart there. |
| `ownfold` | `0` | The same prim/env fold inside the **owned** interpreter's own batches. Tier-2, so `all=1` turns it on; an explicit `ownfold = 0` still wins. |
| `ek_editmesh` | `0` | Expansion Kit builds only, `interp=1`. The course editor's preview (the 30x30 ground grid and the selected segment's highlight lines; the coloured course line is recorded and verified but held stock until a replay reaches it) keeps each element's packed output and re-emits it while its commands, vertex bytes, matrix and draw state are unchanged, instead of transforming and packing it again every frame (`gfx/n64/gdx_ekpreview.cpp`). Same bits as stock; about 0.8 MiB of linear memory while the editor is open, released after 120 tasks without the preview. Receipt `[ekpv]`. On in both auto presets (§4; compiled-in default `0` under `[perf] preset = off`). |
| `ek_editmesh_verify` | `0` | With `ek_editmesh`: draw stock and compare every cached element with a fresh recording (`[ekpv] ... vfy ok= bad=`). |
| `ek_editmesh_track` | `0` | Expansion Kit builds only, needs `track=1`. Once the edited course has chunks, the editor's course-surface preview (`func_800A95B4`) walks through the track pipeline and draws from the resident bake, re-baked when the chunks are rebuilt and have settled for 4 walks (`game/gdx3ds_ek_editmesh_game.c`, `decomp-3ds-ek-editmesh.patch`). Receipt `[ekmesh]`. On in both auto presets (§4; compiled-in default `0` under `[perf] preset = off`). |
| `ek_nimach` | `0` | Expansion Kit builds only, needs `ni_mach` (its bits pick the screens) and `nrr_mach`. `ni_mach` is compiled out of the EK's machine.c; this turns on the EK version: the machine select grid and the machine settings / GP next machine bodies + reflection model drawn from the NRR's recorded meshes, with the EK loop's render mode, grey env colour (`gGreyscaleMachinePart`) and player-designed machines kept stock (`game/gdx3ds_ek_nimach_game.c`, `decomp-3ds-ek-nimach.patch`). Emulator (Old 3DS, ek-ta-dd): machine select mach 13.7 -> 0 ms, misc 7.9 -> 5.4 ms per rendered frame; gstate identical every tick, 16/16 SHOTs identical; nrr_tol 3d for the "OK?" prompt's 3 frames below y=232 (the non-EK `ni_mach` has the same 3 frames). Receipt `[eknimach]` (+ `[nimach]`). On in both auto presets (§4; compiled-in default `0` under `[perf] preset = off`). |

### 2.2 Interpreter semantics (defaults are libultraship parity)

| key | default | what it does |
|---|---|---|
| `cpuclip` | `1` | The owned interpreter clips triangles on the CPU the way LUS does. `0` = let the PICA clip: fewer draws, **different pixels**, and it diverges on tunnel interiors. A bisect lever, not a speed setting. |
| `blendflush` | `1` | Flush the batch before a blend-alpha change, so alpha compare uses the register in force at the primitive (LUS parity). |
| `palhash` | `1` | Fold the live TLUT bytes into the CI texture-cache key, so a palette rewritten in place does not serve a stale decode. |
| `texhash_ci` | `1` | Content-key the CI4 / CI8 / RGBA32 classes (decoded source span plus the live TLUT) rather than keying on the palette's DRAM address alone. |
| `texinval` | `1` | Honour the game's queued texture-cache invalidations (the arena-rewind clear, the per-race minimap delete) on the owned interpreter. |
| `texstalefix` | `1` | An import whose decode bailed is no longer marked uploaded, so a later lookup cannot become a cache hit over a recycled id's old pixels. |
| `vtxclamp` | `1` | An over-range `G_VTX` loads its in-range prefix instead of dropping the whole command. |
| `rawcopywhole` | `1` | The bridge's persistent texture copy tracks what the last refresh actually filled, and is not clamped to the registered extent of the buffer the load starts in. |
| `timgmemo` | `1` | Memoise `G_SETTIMG` operand resolution. Not Tier-2: `all` has no say over it. |
| `dlfacts` | `1` | Memoise display-list facts in the bridge. Not Tier-2. |
| `machines_defer_free` | `1` | Retire retained mesh buffers on a deferred queue rather than freeing them under the draw. |
| `atlas_race_reset` | `0` (`1` in both auto presets) | On the mode change into a race (GP, practice, time attack, VS, death race, attract demos), retire every cache entry that lives in the HUD texture atlas and free the atlas pages, so the race HUD packs onto fresh pages. Without it the race HUD's atlas cells depended on which menus came before, and a lever that changes menu texture traffic (`ni_menu`, `tex_keep`) moved a few edge pixels of scaled HUD sprites. `[atlas] race entry:` receipt. Owned interpreter (`interp = 1`) only. |

Each of these defaults **is** the fix. They are killswitches kept so a regression can be bisected
from an ini file on the console, not tuning knobs.

### 2.3 Verify and bisect

| key | default | what it does |
|---|---|---|
| `interp_verify` | `0` | `1` = run **both** interpreters over every job and compare the packed batches field by field (`[nverify]`). Slow; a correctness tool, not a play mode. Forces both 2D folds off so it compares like with like. |
| `machines_verify` | `0` | `1` = per draw, the walk pass is drawn and captured, the native pass is captured with GPU submit suppressed, and the triangle streams are compared bit-exact. Its counters print unconditionally. |
| `machines_walk_only` | `0` | `1` = replay the baked mesh back through the interpreter's own vertex/triangle path. A stepping stone, kept as a bisect. |
| `machines_mask` | `ffffffff` | Hex per-slot mask (bit *i* = machine slot *i*, 30 slots) — bisect which machine breaks. |
| `machines_lod_mask` | `1f` | Hex per-LOD mask (bit *l* = LOD *l*+1). |
| `tmemskip`, `tmemcls` | `0` | TMEM emulation bisect levels. |

`track = 2` and `fx_sink = 2` are the verify modes for those two levers: the stock path is drawn
and the native replay is hash-compared against it in-process.

> **Set the matching census key with a verify mode.** `track=2` reports no coverage without
> `trkcensus=1`, and `fx_sink=2` reports none without `fxcensus=1`. In both cases the MISMATCH
> line is unconditional but the "how much was checked" totals are not — so a verify run with the
> census off looks like a clean pass and proves nothing. `machines_verify` is the exception.

### 2.4 Censuses (diagnostics; no effect on what is drawn)

| key | default | what it does |
|---|---|---|
| `trkcensus` | `0` | `[trk]` per-frame course census. |
| `fxcensus` | `0` | `[fx]` per-class sprite/triangle/draw census. |
| `ncensus` | `0` | `[ncensus]` batch-split attribution on the owned interpreter. |
| `dspcensus` | `0` | Per-opcode dispatch census on the owned interpreter. |
| `fxceiling` | `0` | Measurement lever: report the draw count an ideal effect batching would reach. |
| `[debug] ni_strict` | `0` | Development only. `1` = **count**: every display-list command a stock interpreter handler still runs (the walker, a span program's kept stock op or bail, a capture run's stock replay, a first-sight record pass) is filed by screen (game mode and race sub-scope), context, the marker or hook that opened it, the loop that ran it and opcode, with how many frames it appeared in. Nothing is dropped, so the output is unchanged. `[nistrict] init` at boot, a compact `[nistrict] sum` every `[debug] ni_strict_every` frames (default 3600) and the full table as `[nistrict] final` at exit; `tools/native/nistrict_report.py` ranks it. `2` (drop the counted commands) is not implemented yet and counts like `1`. Runs on the `nicensus` counters, so it costs nothing when off. |

### 2.5 Boot receipt

Every resolved value is named once in the log and on the DBG tab's `NATIVE` / `NATIVE FX` rows:

```
[native] all= interp= machines= track= fx_sink= fx_sprites= fx_hud= fx=<bitmask> census trk= fx=
         (resolved: all=tier2 only, interp explicit; ini latched at boot)
[native] interp= path=lus|owned cpuclip= blendflush= verify= hooks=
```

If a lever you set is missing from that line, the ini was not read — check the path and the
section name.

Related `[debug]` keys the development harness uses and a player should leave alone: `det`, `fh`,
`fh_every`, `fh_start`, `input_replay`, `input_record`, `replay_quit`, `gputrace`, `renderthread`,
`cov`, `shots`. `[net] enabled` (the development link to a host) defaults off for a normal SD/CIA
launch and only defaults on when the console was launched through 3dslink's netloader — see
`docs/HW-DEV-LOOP.md`. Set it explicitly if you want to be sure either way.

### 2.6 Memory, disk and Expansion Kit levers

These are `[debug]`, not `[native]` — they are not renderer levers, but they are the keys the
Old 3DS result and the Expansion Kit depend on, and they are documented nowhere else.

| key | default | what it does |
|---|---|---|
| `audio_stream` | `0` | `1` = stream the 10.25 MB audio sample bank out of the `.o2r` through a block cache instead of holding it resident, saving that heap and shortening boot preload. |
| `audio_stream_cache_kb` | `1024` | Size of that cache; raised from an earlier `512` after testing showed the smaller size re-fetching too much. |
| `audio_stream_block_kb` | `16` | Block size for it. |
| `audio_stream_verify` | `0` | `1` = serve every streamed read *both* ways and compare. A correctness tool; it is a byte comparison, not a listening test. |
| `tex_upload_kb` | `0` | `0` = the historical 4 MiB texture staging reservation. Any other value caps it, in KiB; `512` sits above the highest staging footprint seen across the whole replay corpus. The decoders clamp and **log** if a value is set too small, and a CLAMP line means exactly that. |
| `disk_stream` | `2` on 3DS | 64DD image handling for an Expansion Kit build. 3DS defaults to streaming (`2` = stream off the SD card with no resident buffer); `0` (resident image) does not fit either console's memory and has to be set explicitly to use it. There is no useful `1` on this platform. The first mount of a given image pays a whole-image CRC pass (identity check) and caches the result in a `<name>.ndd.dcrc` sidecar next to the save, so later boots sample 256 KiB instead of re-reading the whole disk; the `[diskstream]` receipt names exactly why a boot did or didn't skip it. |
| `disk_stream_cache_kb` | `512` | Block cache for the streamed disk; testing against real hardware found no benefit from a larger cache. |
| `disk_stream_block_kb` | `16` | Block size for it. |
| `disk_stream_report` | `512` | LEO reads between periodic `[diskstream]` receipts. |
| `ek_english` | `1` | Apply the baked English text pack over the mounted disk. Inert in a build where the pack was not baked, which is every default build; skipped automatically on an already-translated image. |
| `ek_fixed_aspect` | `0` | Killswitch for the fixed-4:3 fix that makes the course editor draw at all. `1` restores the course editor's black-screen bug on 3DS. Leave it at `0`. |
| `heapattr_top`, `membudget`, `membudget_top` | `0` | Heap-attribution and memory-budget censuses. `heapattr_top` needs a build configured with `-DGDX_3DS_HEAPATTR=ON`. |

Build options that go with them, all defaulting to the shipped value:

| option | default | what it does |
|---|---|---|
| `GDX_3DS_RDRAM_MB` | `16` | Size of the emulated RDRAM arena; `8` is the Old 3DS recommendation, comfortably above measured peak usage. |
| `GDX_3DS_SIM_O3DS` | `OFF` | Simulate the Old 3DS memtype-0 commit budget so a New 3DS or the emulator reproduces the real grant. **Hazard:** pinning a budget above the host console's real one panics in `crt0` with a black screen and no log. |
| `GDX_3DS_HEAPATTR` | `OFF` | Allocator interposition and the ranked `[heap-attr]` census. A measurement build (~2.13 MB of `.bss`). |
| `GDX_3DS_EXPANSION_KIT` | `OFF` | Compile the 64DD Expansion Kit. **Not additive** — it swaps the ROM subsystems for the disk ones and changes `#ifdef EXPANSION_KIT` behaviour across dozens of translation units in both `decomp/` and `port/`, so an Expansion Kit build and the shipped build are two different binaries, not two settings. Needs your own ~64.9 MB `.ndd` at runtime. |
| `GDX_ALLOW_JP_INPUTS` | `ON` on 3DS | Accepts Japanese-region game data — specifically the retail Japanese 64DD Expansion Kit disk, the only retail EK disk that exists. On 3DS this is on by default (the desktop port defaults it off, since there it also gates a cartridge ROM path with a known audio bug that the 3DS never takes — the console never reads a cartridge ROM at all). |

### 2.7 The `ni_*` levers (the interpreter off the menus)

Seven `[native]` levers move whole screens off the owned interpreter's display-list walk onto
native paths. **Since preset-fix they are on in both preset tables** (§4), at the values every
hardware test card since ni-s0 ran: `ni_menu 1`, `ni_menu_root 1`, `ni_menu_split 3`,
`ni_menu_keep 1`, `ni_menu_ucode 1`, `ni_menu_xition 1`, `ni_course 1`, `ni_mach 3`, `ni_race 1`,
plus `ni_view 0x97A` on the New 3DS only (the Old 3DS table carries the MP-only `ni_fullview 0x97A`
instead). The `GDX_3DS_PRESET_NI` CMake option that used to stage them is gone. `tools/ci-3ds.sh`'s
`gdx3ds_preset_mustship_tests` fails if a preset stops carrying any of them. The "default"
column below is the compiled-in fallback that applies only under `[perf] preset = off`.

| key | default | value to try | what it nativises |
|---|---|---|---|
| `ni_menu` | `0` | `1` (`2` = verify) | Every non-race screen's 2D: the draw is captured as MENU-kind runs and drawn by compiled programs (needs `nrr_hud = 1`). `[nimenu]` receipts. |
| `ni_course` | `0` | `1` | The 3D course preview of course select, GP next course and records course select. `[nicourse]`. |
| `ni_mach` | `0` | `3` | Bit 1: machine select's 30 machines as NRR mach records. Bit 2: machine settings / GP next machine and their reflection models. `[nimach]`. |
| `ni_view` | `0` | `0x57A` | NRR categories in multi-view races (VS 2-4P, attract demos; bits per category) and `0x400` the records flyby. Gated by gstate and the NRR 3D tolerance class, not bit-exact. |
| `ni_menu_root` | `0` | `1` | With `ni_menu`: the `ni_race` span programs also run each menu frame's root (segment table, colour/z image, setup lists between the MENU runs), taking the menus' remaining walker residue to ~1 command per frame. `[nirace] init ... menu_root=`. |
| `ni_menu_xition` | `0` | `1` | With `ni_menu = 1` (refused under `interp_verify`): the menu transitions. The hide half of every wipe and the `GAMEMODE_CHANGE_INIT` frame run while `gGameModeChangeState != 0`, where `ni_menu_root` never armed; with this on, those frames keep the root's span programs too (race modes excluded), and the game side captures the INIT frame's lone `Transition_Draw` as a MENU region. The 2D wipes (tiles, fades, strips, sliding wipe) then draw as MENU runs on every frame; the tiled whirl / spiral's 64 perspective tiles stay on the walker. `[debug] xition_mask` bisects (1 change-frame roots, 2 INIT capture); `[debug] xition_force = 1..7` makes every random transition that type (test knob, RNG untouched). `[xition] init` / `[xition] window` receipts. |
| `ni_race` | `0` | `1` (`2` = verify) | The 1P race's walk between NRR markers, as span programs. `ni_race_mesh` (default on with it) draws their geometry segments as retained meshes, refused in stereo frame replay. `[nirace]`. |

Not in the staged rows (off everywhere; a card sets them by hand):

| key | default | value to try | what it does |
|---|---|---|---|
| `ni_view` bit `0x800` | off | `0x97A` with `ni_race = 1` | The `ni_race` span programs in VS 2-4P race frames too, each view's spans keyed on their own markers. `[nirace] init ... vs=1`. |
| `ni_menu_split` | `0` | `3` | With `ni_menu`: bit 1 splits FILLRECT chains (the tile wipe), bit 2 cuts texture-load runs before each SETTIMG, so a transition's runs keep their programs. `[nimenu] init ... split=`. |
| `ni_menu_keep` | `0` | `1` | With `ni_menu`: a full memo / program pool is not cleared for a run that also decoded last task. `[nimenu] init ... keep=`. |
| `ni_menu_ucode` | `0` | `1` | A MENU region's ucode load (records screen) stays in place instead of cutting the region. `[hud] init ni_menu_ucode=`. |

MP fullview (`[net] mp_fullview = 1`, or `[debug] fullview_player = k`; port/3ds/game/gdx3ds_fullview_game.c) draws
only view k of a 2-4P VS race, full screen. The NRR still sees a multi-view race (`gNumPlayers` is unchanged), so view k's
categories follow `ni_view` exactly as in split screen. The other views' NRR sections are never reached, because
`gdx_dl_skip` drops them. Stereo frame replay is allowed for that single view (`Gdx3dsStereo::ReplayEnter`). It is refused
again once the results transition brings the split screen back.

Old 3DS MP fullview (o3ds60). Three levers apply only while a fullview frame is drawn (`gdx_fv_k() >= 0`); the
`ni_race` row also covers the 1P race. Split screen and the attract demos are not affected. They are on by default in the
Old 3DS table only; the New 3DS table leaves them off:

| key | O3DS default | what it does |
|---|---|---|
| `[native] ni_fullview` | `0x97A` | ORed into `ni_view` for fullview frames: sky, track, dressing, shadows and FX go to the NRR, and the `0x800` bit (with `ni_race = 1`) gives the walk between markers as span programs. `[nrr] init ... ni_fullview=`, `[nirace] init ... fv=1`. The plain `ni_view` stays 0, so split screen still uses the legacy path. |
| `[native] ni_race` | `1` | The Old 3DS table now turns on `ni_race`, the 1P span programs (exact). This also covers the SP race. |
| `[ui] mp_hud_placement` | `1` | The fullview view follows `[hud] placement` the way the 1P race does. This lifts the 2-4P force-ALL_TOP rule for that view only (port/gdx_hud_placement.c, via `gdx3ds_bottomhud_fullview_placement`). `Hud_DrawHud` still runs, so every player's lap and interval logic runs, and the picture is rewound (decomp-3ds-o3ds60-mp-hud.patch). `[dash] init ... mp_hud=`. |
| `[ui] dash_spread` | `1` | Not a fullview lever: the bottom race DASH (`dash_every` N > 1). While the race draws every tick (no frameskip skip iteration in the last 8, or steady frameskip already at D = 1), iteration i paints only the widgets at paint-order index i % N, instead of every widget on one iteration in N. The same 60/N Hz per widget, without a paint spike every Nth frame (a 1-in-4 spike sits above the frameskip's p90 and blocks the step up to every-tick rendering). `2` = only after 8 ticks without a skip (the 5c24be0 rule: the first ticks of a D = 1 stint still took a whole repaint). `[dash] init ... spread=`. |

Old 3DS lockstep steady frameskip (o3ds60, from the 5c24be0 hardware log). The Old 3DS guest sits at the lockstep horizon:
the host's pads arrive in 30 Hz pairs (`[net] mp_uds_tx_min_ms = 33`), so some ticks wait 16-55 ms for them, most of
them skip ticks. That wait was not counted against the next slot's late start, so D = 1 slots missed "hard", repaid
catch-up skips, and those skips waited at the horizon again (one D = 1 stint: 24 fps, netwaitms 2745 of 4318). These
levers are frameskip-only: they change which ticks render and when, never the simulation or the pads:

| key | O3DS default | N3DS default | what it does |
|---|---|---|---|
| `[debug] steady_net_excuse` | `1` | `0` | The pad wait summed over the whole previous slot explains a late start. A slot that started late by no more than that wait, and whose present fits measured from its own start, is not missed (`netlate`) and re-anchors on the vblank its present makes. `[frameskip] lockstep on ... net_excuse=`. |
| `[debug] steady_net_catchup` | `0` | `1` | `0`: no catch-up skips for a late start while a pad wait happened in the last 16 ticks (the console is ahead of its peers; the next pad wait repays the vblank). `[steadyd1] ... catchupcut=`. |
| `[debug] steady_net_lead` | `2` | `0` | The most vblanks per lockstep race a slot start that waited >= 1 ms for pads takes one vblank later (120 ticks apart; one repeated frame each), so this console runs ahead of the pads instead of at the horizon. hw 5c24be0: the New 3DS host never waited (wait_us=0) while the Old 3DS guest waited 164-6221 us a tick; the host keeps ~100 ms of the 6-tick delay, ~67 ms after two. `[frameskip] lockstep lead +1 vblank`. |

Old 3DS race dash on a budget (o3ds60, from the 52acd44 hardware log: at D = 1 the post-present tail averaged 4.1 ms,
max 7.6, and the dash tick reached 4.5-5.6 ms):

| key | O3DS default | N3DS default | what it does |
|---|---|---|---|
| `[ui] dash_budget` | `1` | `0` | At steady D = 1 a dash tick stops painting widgets once it has used `dash_budget_us`; the rest carry to the next tick, painted first. Also (whatever D): the map box's scaled bitmap is cached while the minimap bitmap and palette are unchanged, the present copies each run of painted columns instead of the whole span between the first and last, and the 120-present self-heal is a 40-column slice every 15. Bottom screens are pixel-identical on the emulator (the FPS digits aside). `[dash] init ... budget=`, `[dashworst]` per 256 ticks. |
| `[ui] dash_budget_us` | `1000` | - | The per-tick budget (100-16000). |
| `[ui] dash_every_d1` | `6` | `0` | The spread's widget period at steady D = 1 (6 = each widget 10 times a second; 0 = `dash_every`). |
| `[ui] dash_map_inc` | `1` | `0` | Needs `dash_budget`. While the cached map box is on screen unchanged, a map tick puts the cache back only under the previous tick's dots / NET chip / FPS text, paints them again, and presents only those rows of their columns. hw a3b4531: a map tick was map_us 1.6-2.0 + pres_us 1.5-2.3 ms (106 KB), and every D = 1 miss with dash_us ~3 ms was one. A whole-frame present without a whole map paint in the same tick, or 50 incremental map ticks off D = 1, paints the whole box again. `[dashmap] inc= whole= cache_builds=` per 256 ticks, `[dashworst] ... map_inc=`. |

Old 3DS 60 fps rules from the a3b4531 hardware log (o3ds60; every D = 1 stint of a 2-machine MP race with 3D on ended
after 3 misses in 4 slots, rw 12.5-14.5 ms avg + post 2.1-4.5 ms). Timing and rendering only: the simulation, the pads
and the lockstep digest are the same with every one of them on or off.

| key | O3DS default | N3DS default | what it does |
|---|---|---|---|
| `[debug] steady_d1_rate` | `1` | `0` | At D = 1 in a lockstep race (`2` = in every race) a slot is missed only when its START lost a vblank (a repeated frame). A present later than the 4 ms margin before its display vblank but on time at its start is one more vblank of latency, not a lost frame, and counts as `latepres=`. hw a3b4531: the D = 1 renders took 12.5-14.5 ms, so nearly every slot "missed" the margin rule. `[frameskip] mode=steady ... d1_rate=`. |
| `[debug] steady_d1_hold_pct` | `85` | `0` | At D = 1 the step-down rules wait until fewer than this % of the last `steady_d1_hold_slots` slots hit (a missed slot repeats one frame; a step down halves the rate for the whole dwell). `[steadyd1] ... holds= hold_pct= hit=%`. |
| `[debug] steady_d1_hold_slots` | `64` | `16` | The hold's window (16..64). A stint younger than the window counts its unseen slots as hits, so a short burst does not leave D = 1; every slot missed still steps down within 10 slots at 85 / 64. hw 9088d3a (3D): stints at 93-97% hit left D = 1 on 4 misses in a row (80% of 16 allows 3). `[steadyd1] ... hold_slots=`. |
| `[debug] steady_d1_retry_ms` / `steady_d1_retry_pct` | `2000` / `85` | `0` (off) / `85` | At D = 2 (D = 1 allowed), step up after this long (a failed try doubles it once, to 2x at most) with no hard miss in 16 slots and the spike-free p90 render under pct% of a vblank. The stock rule needs p90 + the 4 ms present margin inside 80% (p90 <= ~9.3 ms) and backs off to 16 s: hw 9088d3a sat at D = 2 for whole windows with p90 8-12 ms. `[steadyd1] ... retries=`. |
| `[debug] steady_ab` | `0` | `0` | 1 = lockstep race 1 runs the D = 1 rules as configured, race 2 the 9088d3a rules (80% of 16 slots, the stock step up), race 3 on as configured. `[steadyab] race N rule=new\|old`, `[frameskip] mode=steady ... rule=`, `[steadyd1] ... rule=`. |
| `[debug] top_triple` | `1` | `0` | A third top framebuffer (576 KB, both eyes, the top screen's memory; `vendor/libctru_gfx/gfx_split.c`) in lockstep races (`2` = always). The back buffer is the one neither on screen nor waiting for its vblank, so the pace end guard (a frame's display transfer waits until the previous swap latched) never waits. hw a3b4531: `fe=` (EndFrame, guard included) 0.5-2.2 ms avg, max 3.9. A present that replaces a still-pending one drops that frame (`[pipe] ... replaced=`). `[pace] ... tb= tbskip=`. |
| `[ui] dash_pre_frame` | `1` | `0` | In a lockstep race a rendering iteration runs the bottom-screen tick (the race dash and its present) right before StartFrame, where the CPU would wait for the GPU to finish the previous frame (`gsp=`), instead of after EndFrame. |
| `[debug] pipe_ab` | `0` | `0` | N frames (rounded to the 256-frame receipt window): inside every lockstep race, even blocks run as configured, odd blocks flip what `pipe_ab_what` names (1 = `top_triple` + `dash_pre_frame` off, 2 = `kick_3d` flipped), so one launch measures both. `[pipe] frame= block= state=new\|old`, `[steadyd1] ... pipe=1\|0`, `[pace] ... tb=1\|0`. |
| `[debug] kick_3d` | `1` | `1` | 0 = no `[overlap]` mid-frame GPU kick (and no stereo head run) while the right eye runs, so EndFrame has no kicked queue to drain on the CPU. hw 9088d3a (3D, D = 1): `fe=` 4.7 ms avg / 8.2 max with ~2 kicks a frame vs 0.8 ms in 2D. `[steadyd1] ... drain=avg/max kickf=kicked/presents`. |
| `[stereo] mp_3d` | `0` | `1` | An online race: `0` = 2D (one eye: the GPU does half), unless the Home 3D button was pressed in this launch (then the button decides); `1` = as `[stereo] enabled` / the button says. |
| `[stereo] mp_ab` | `0` | `0` | The one-launch 3D A/B. 1: online race 1 of the launch runs 2D, race 2 runs 3D (whatever `mp_3d` and the button say; a 2D-booted session creates the right eye then), later races follow `mp_3d`. 2: races 1 and 2 run 3D (with `steady_ab`: the new D = 1 rules, then the old), later races follow `mp_3d`. `[mpab] race N stereo=on\|off (rule)`, `[mpab] race N end ... frames= right_eye=`. |
| `[debug] bui_3d_button` | `0` | `0` | Developer lever (card-4; was `[ui] home_3d_button`, default 1): Home shows a 3D ON / OFF button. It writes `[stereo] enabled` to the ini and takes effect on the next frame (a session that booted 2D creates the right-eye target then). Players use the 3D slider: `[stereo] enabled` defaults to 1 on both models (`[perf] preset = auto`). `[stereo] button:`. |
| `[debug] stereo_mp_lock30` | `1` | `0` | o3ds3d: while a lockstep race renders in 3D, steady D is pinned at 2 or above (no D = 1 retry; a D = 3 step down on misses still allowed). `1` = the pin and the present hold (every frame presents at vblank S+1 and shows for 2 vblanks; b52ac08 showed D = 2 frames for 1 or 3 with the lockstep hold off), `2` = the pin alone, `0` = off. 2D races and single player never pin. `[lock30] on\|off`, `[lock30] frame= slots= missed= ... pp=` per steady window, `[lock30] race=N end` per race. |
| `[debug] ceiling_probe` | `0` | `0` | o3ds3d, measurement only: lockstep race N of the launch runs `ceiling_probe_blocks` (2) blocks, from racing tick `ceiling_probe_start` (1200) every `ceiling_probe_every` (2400), of `ceiling_probe_ticks` (240) D = 1 ticks as usual then as many with the previous frame's GPU work waited out first (the frame then runs with no GPU work beside it). The controller never sees probe slots; only the render cadence changes (the game state and lockstep inputs do not). Skipping the GPU wait instead is unsafe: one command buffer and a VBO pool that both restart at offset 0 every frame. 3D slots only. `[ceiling] race= block= half=normal\|idle ... rwcpu= post= frame= ... fit=`. hw 0c2668a: the idle-first half did not make the D = 1 3D frame cheaper (14.2 vs 13.5 ms), so the double-cmdbuf / deferred-wait plan is dropped; the lever stays as a measurement. |
| `[native] zclear_elide` | `1` | `1` | Both presets pin it on (exact by construction: the skipped depth fill would rewrite the value already there). `2` = the hardware A/B (on in even 1024-frame blocks, `[gputl] zc=`), which the New 3DS preset shipped until o3ds3d. |
| `[stereo] onesubmit` | `1` | `1` | ship6 (hw card-6 A/B): both eyes of a 3D body chunk in ONE GX entry (the body ends in `CMDBUF_JUMP1` to a right-eye prologue that jumps back into it), so one GSP submit and one completion IRQ per chunk instead of two. Old 3DS: 3D menu 40.2 -> 44.6 fps, 1P 3D race ~1.5-1.8 ms/frame headroom (~1.5 ms onesubmit alone, ~1.8 ms with `onesubmit_narrow`, the shipped pair); New 3DS neutral. `0` = the re-submitted right-eye replay. Code default and both presets. `port/3ds/gfx/STEREO.md` "One submission". |
| `[stereo] onesubmit_narrow` | `1` | `1` | ship6: with `onesubmit`, a 3D frame's end flushes only the `[overlap]` noted dirty ranges instead of the whole linear heap (another ~0.3 ms on the Old 3DS, -0.4 ms GX CPU on the New 3DS). `0` = the whole-heap flush. |
| `[stereo] onesubmit_kicks` / `onesubmit_ab` | `0` | `0` | Not shipped: `kicks` 1/2 drops the mid-frame body kick (and head run), which costs +1.0 / +2.5 ms of frame-begin GPU wait; `ab = 1` is the per-`[cpucut]`-block A/B of the hardware card. |
| `[debug] prewrite` | `0` | `0` | o3ds3d, measurement only: per 256 rendered race frames, C3D_FrameBegin's queue wait (`gsp=`), the CPU time from its return to the frame's first GPU-memory write (`pre=`: VBO handout, texture upload, GX fill/copy, cache flush, or a polled command-buffer move) and `hide=` min(gsp, pre) on frames no vblank wait absorbs -- what moving the wait to the first write would save. `[prewrite]`. Contract in `port/3ds/gfx/gdx3ds_prewrite.h`. |
| `[debug] gpumem` | `0` | `0` | o3ds3d, measurement only: per race, linear-heap free low water (and the largest 2/1.5/1/0.5 MiB block that allocates 600 frames in), VRAM free low water, command-buffer words high water / avg vs its size, VBO bytes high water / avg vs the 1 MiB pool, and whether the largest frame fits half of each (`halves=`). `[gpumem] race=N end\|checkpoint`. |

`[fvse]` counts one online race: cleared when it starts, printed when it ends (`race-end`) and when the bottom screen opens
its results (`results`), per player (engine / level / player SE starts, and the SE volume commands kept or muted).

Announcer lines in an online race (`decomp-3ds-mp-polish.patch`): every system SE plays on seq 0 channel 1, which the
per-player mixer never scales, so a voice one player triggered played on every console. A console that is one MP player's
speaker plays the lap-2 / final-lap lines for its own player (`hud.c`), the "pulling away from the pack" voice (`NA_SE_59`,
a human leader 4000 ahead) only when its own player leads, and the wrong-way voice (`NA_SE_60`) only for its own player
(`racer.c`). Every gated decision logs `[announce] se=NA_SE_59 racer=N local=M players=P played|skipped` (NA_SE_17 / 18 /
59 / 60; the stock trigger fired, the line says which console played it). The race-wide lines (the 3-2-1-GO countdown, pause, the results ticks) and the 1P-only ones are unchanged.

The `[steadyd1]` / `[steadymiss]` lines also carry `fe=`, EndFrame's share of `post`. `[stereo] slider` (every 1024 frames) prints the
3D slider's min/max as the system reports it and the frames with the right eye drawn: every Old 3DS hardware log so far reads 0.39 on
every frame, so on that console 3D can only be turned off with `[stereo] enabled = 0`.

`[steadyd1]` (each 256-tick window with D = 1 slots) gives the D = 1 slots, misses by cause (`begin`, `net`, `late` = the
previous slot's post-present tail overran, `work` = the render itself), and avg/max of the start lateness, the slot's pad
wait, the render work, the post-present tail (EndFrame + dash + telemetry), the GSP wait inside the render and the dash
tick. `[steadymiss]` prints the first 40 missed D = 1 slots of each race one per line.

Emulator receipts (o3ds60, Old 3DS model, `race-vs2p-results` with `[debug] fullview_player = 0`, `det=1`): the race view's
`[nicensus]` NRR line goes from `hud mach` only to `hud trk mach sky dress shd fx deco`, the interpreter walk between markers
runs as span programs, and the 393 `[gstate]` digests (every 30 ticks, through the results) are identical with the three
fullview levers on or off. Old 3DS hardware 158a13e, MP race, same view on the interpreter: walkB 7.3-9.2 ms + seam 6.1-6.9 ms
+ nrrhud 2.7-4.4 ms per drawn frame; the NRR 1P race on the same console: walkB 4.5-5.5 ms (no span programs), no seam.

All four are render-only. The sim reads `gdx_sim_machine_visible`, and none of them writes `machineLod`, which is folded into
`gdx_mpo_vis_hash`.

---

## 3. The compact native race renderer (NRR)

The levers in §2 are the renderer's first generation. A second, more complete generation —
internally called the **NRR** (compact native race renderer) — sits alongside it and covers more
of the frame. Both generations share the same marker mechanism (a `G_NOOP` the game emits where a
draw call used to be, picked up by a hook in the owned interpreter) and the same requirement that
`interp=1` is what makes the owned interpreter walk the list at all; NRR adds *what* gets drawn at
each marker, not a new way of reaching one.

NRR renders by **category**, each gated by its own `[native] nrr_<name>=1` under a master
`[native] nrr=1`:

| Category | Lever | Covers |
|---|---|---|
| HUD | `nrr_hud` | The top-screen race HUD (the bottom-screen DASH is CPU-painted, not an NRR category) |
| Track | `nrr_trk` | The course surface |
| Machines | `nrr_mach` | The racer models |
| Sky | `nrr_sky` | Sky and backdrop |
| Trackside dressing | `nrr_dress` | Trackside scenery |
| Course decorations | `nrr_deco` | Course decorations; depends on `nrr_dress` |
| Shadows | `nrr_shd` | Machine/track shadows |
| Effects | `nrr_fx` | Particle and effect sprites |
| Self-test | `nrr_selftest` | Debug-only; never set in a shipped configuration |

Two things worth knowing before touching these:

- **Sequential host loop only, for now.** NRR's game-side snapshot is taken once per tick and
  consumed by the render side either synchronously (`renderthread=0`) or from the parallel render
  core (`renderthread=1`); the levers above are only validated with `renderthread=0`, which is why
  the shipped bundle in §4 also sets it explicitly.
- **A category reports 0 (drawn) or a counted fallback**, never silently does nothing — there is
  no legacy path left once a category is on, so a steady race reporting fallbacks is a real
  problem, not a quirk.

This generation is deep enough that it is not fully catalogued lever-by-lever in this document —
each category's own source file (`port/3ds/gfx/nrr/`, `port/3ds/gfx/n64/gdx_nrr_*.cpp`) is the
reference for what it specifically changes.

---

## 4. The shipped configuration: `[perf] preset`

Every lever in §2–§3 still defaults to `0` at its own call site — that part hasn't changed, and
it's what makes each one bisectable by hand. What changed for v1.0.0 is the ini's own default:
`[perf] preset` defaults to `auto`, not to being absent, and `auto` is resolved into a built-in
lever table the moment the console model is known (`APT_CheckNew3DS`, before any lever is read).
So **a stock `gdiffuser.ini` with no `[perf]`/`[native]` section now boots straight into the
native renderer and NRR** — there's nothing to add for performance on either console. Explicit
keys you *do* add still override the resolved preset's value for that key only.

```ini
[perf]
preset = auto    ; the default — you don't need to write this line yourself
```

- **New 3DS** → `auto` resolves to the exact table an explicit `preset = n60` would (below).
- **Old 3DS** → `auto` resolves to an internal table (`main_3ds.cpp` calls it `o3ds-auto`; it's
  not a name you type yourself) that is the *same* lever set as `n60`, minus the New-3DS-sized
  `tex_keep_kb` texture headroom, plus three Old-3DS-only levers the hardware-verified Old 3DS
  card needed: `audio_stream=1`, `tex_upload_kb=512` (both memory-fit, §2.6), and
  `syscore_pct_safe=1` (clamps a core-1 CPU-time request to a measured-safe ceiling — 80% was
  measured starving the audio/loading pipeline). The `[perf]` LOD auto-scaling discussed at the
  end of this section runs independently on top of this and is unchanged.
- **`preset = off`** is the escape hatch back to bare stock — every lever at its own compiled-in
  default, byte-identical to the pre-native build. Use it to rule the preset in or out when
  bisecting a regression.
- An explicit, non-`auto` `[perf] preset` you set yourself (`n60`, `off`, or anything else) is
  read exactly as typed; `auto`-resolution only fills in when the key is *absent* or literally
  `auto`.

Source: `port/3ds/gdx3ds_perf_preset.c` (model detection and the LOD-only side of `auto`/`o3ds`)
and `port/3ds/gdx3ds_preset_n60.c` (`kN60Preset`, `kO3dsAutoPreset` — the lever tables themselves,
with the full key-by-key rationale in their header comments).

On a New 3DS build, `preset = n60` (what `auto` resolves to) bulk-sets roughly sixty `[native]`,
`[debug]`, `[stereo]` and `[menu]` keys to the values this release ships with — every one of them
is still an ordinary ini key, so an explicit entry for any of them in your own `gdiffuser.ini`
overrides the preset's value for that key only. Grouped by what each batch is for:

- **Host loop and frame pacing** (`debug.renderthread=0`, `frameskip=race0` and its tuning,
  `pace_swapgate`/`pace_swapgate_3d`, `dl_hollow`, `overlap_kick`, and related keys) — runs the
  sequential host loop NRR needs (§3) and paces frame submission to hit 60 Hz.
- **Texture handling** (`tex_fast`, `tex_keep`, `tex_keep_kb=4096`, `tex_keep_floor_kb`) — a larger
  resident-texture budget than the stock path uses, sized for the New 3DS's spare RAM.
- **Audio and save** (`audio_io_unlock`, `audio_ctx_narrow`, `audio_simd`, `audio_rs_align`,
  `sram_async`, `filelog_race_quiet`) — the same audio-path and off-thread-save fixes called out in
  the release notes.
- **The renderer core**: `native.all=1`, `native.interp=1`, plus every Tier-2 lever from §2 and the
  retained-path variants of each (`track_retained`, `machines_retained`, `hud_retain`,
  `sky_retain`, `shadow_batch`, `fx_batch`, and siblings).
- **NRR**: `native.nrr=1` and every category lever from §3, plus `finish_mesh`, `prewarm`, and
  `zclear_elide`.
- **Stereo**: `stereo_interleave`, `stereo_noreflush`, `stereo.enabled=1` — the 3D slider is live
  under this preset (slider at 0 still renders byte-identical mono) — plus the 3D GPU-path rows
  both tables share: `stereo_fold`, `[stereo] head_kick`, `resume_fold`, and (ship6) `onesubmit =
  1`, `onesubmit_narrow = 1`, `onesubmit_kicks = 0`, `onesubmit_ab = 0`.

**Old 3DS reaches its 60 fps race through the same `[perf] preset` key, not a separate recipe** —
see the `auto` → `o3ds-auto` resolution above. It is not one you type by hand (there's no
documented `preset = o3ds-auto` for a player to write; `auto` is what gets you there), but it is
read from the same ini key as the New 3DS table, applied the same way, with an explicit key of
your own overriding it the same way.

`[perf] preset` also has a second, older meaning, unrelated to the renderer, that runs alongside
the resolution above rather than instead of it: the automatic draw-distance/detail-reduction
system for crowded races (`rival_detail`, `rival_tier_max`, the auto-detail band, `lod6_skip`).
`auto` (the default) selects its Old-3DS-tuned table on an Old 3DS and leaves a New 3DS alone;
`o3ds` forces that table on any model. That system is about *how much* gets drawn, not *how* — it
stacks with either renderer generation, and both systems are driven by the same `[perf] preset`
value without conflicting.

---

## 5. How a lever is checked

Four independent gates, in increasing order of what they can see:

1. **Host tests.** `tools/ci-3ds.sh` builds and runs a large and growing set of host test binaries
   covering the vertex packer, the format converters, the track templates, the owned interpreter,
   the atlas UV arithmetic, the config readers, the heap configuration, the LOD controller, the
   block cache and disk streamer, the `.o2r` stream reader, the MAME ↔ `.ndd` converter (30,288
   checks plus a round-trip), texture swizzling, audio CRCs, DSP kernels/SIMD, save handling, and
   more — the script itself is the current list. It also verifies the vendored-libultraship
   manifest and applies the `decomp-*.patch` stack idempotently, failing on a patch file it does
   not know.
2. **In-process verify modes.** Each lever can draw the stock path and compare the native path's
   packed vertex bytes against it, in the same frame, bit for bit — see §2.3. This catches
   differences that never reach a pixel.
3. **Frame hashes.** `[debug] det=1` pins the game's nondeterminism, `.gdxrec` replays drive tick-
   exact input, and `[fh]` emits a per-frame hash of the presented frame. Two runs are compared
   frame for frame; screenshots are captured on a schedule derived from a per-frame feature
   census (`[debug] cov=1`), so the schedule covers everything the replay actually draws rather
   than points somebody guessed.
4. **Hardware.** A console over a WiFi link, running the same replays.

**Parity is same-platform and same-render-thread-mode only.** The render thread presents each
game frame exactly one host frame later than the sequential renderer, so `renderthread=0` and
`renderthread=1` are two different executions, not two settings, and their frame hashes do not
match without a one-frame shift. The gate runs with the thread **on**, which is the shipping
default.

### What that caught

Widening the replay corpus past the original course found two real parity bugs that every
earlier gate had missed — one in texture-cache keying (`palhash`/`texhash_ci` above are the fix)
and one in a material-binding guard. Both are fixed in this release and all corpus replays report
zero divergent frames — see the post-mortem for the story.

---

## 6. What is measured

See [`RELEASE-NOTES.md`](RELEASE-NOTES.md)'s "Measured performance" section for the renderer's
hardware numbers and the conditions they were taken under, and its "Known issues" section for
what is still rough — including the Old 3DS memory ceiling and the audio gaps during loads.

---

## 7. The Expansion Kit

The 64DD Expansion Kit is compiled by `-DGDX_3DS_EXPANSION_KIT=ON` and is not the shipped
configuration. It boots on a console, reaches the main menu, draws three screens' worth of
vocabulary the shipped build cannot draw, and loads a playable course editor and machine creator
from a disk image streamed off the SD card. It can be baked with an English text pack so a
Japanese dump reads in English without replacing the dump.

It goes through the same renderer: `interp` and every lever in §2–§4 apply to it unchanged, and the
main-menu frames are byte-identical between an Expansion Kit build and the shipped build except
for the two extra signs. It now also fits the Old 3DS's 8 MiB RDRAM arena (an `o3ds-ek` build
boots, reaches the menus, and loads the course editor and machine creator with a streamed disk) —
but fitting in memory isn't the same as running well: on real Old 3DS hardware 64DD courses are
jittery, the disk music breaks up, and the editors are laggy, so `o3ds-ek` is experimental and not
released in v1.0.0 — see `ARCHITECTURE.md` §4.1 for the four console builds and
[`RELEASE-NOTES.md`](RELEASE-NOTES.md) Known issues.

**Course editor test drive (`[native] nrr_testdrive`, on in both auto presets, compiled-in default
`0`, Expansion Kit builds only).** The
editor's test drive draws a 1P race outside `Race_Draw`, so by default every NRR category leaves it to
the interpreter. `nrr_testdrive = 1` opens the race scope for that draw
(`decomp-3ds-ek-testdrive-scope.patch`): sky, track, machines, shadows and effects then draw natively,
and `Course_Init` re-bakes the track at the start of every test run. Receipts:
`[c5tick] nrr_testdrive=N` at boot and `[c5tick] testdrive scope open run=N` per run. On by default
through the auto presets (§4); `nrr_testdrive = 0` turns it off.

What it costs and what is still wrong with it is in [`RELEASE-NOTES.md`](RELEASE-NOTES.md). The
disk-streaming levers are in §2.6 above.
