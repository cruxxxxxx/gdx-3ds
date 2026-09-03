# G-Diffuser Modding Guide — Texture Packs

This guide is for **modders**. It assumes you have never seen the source code. It walks you end to
end: **dump** the textures the game draws, **edit** them as ordinary PNGs, **pack** them into a
`.o2r` file, **install** the pack, and **reload** it live while the game runs.

By the end you will have a working texture pack in the `mods/` folder and see your art on screen.

---

## 1. What a texture pack is

A texture pack is a single file named `something.o2r`. Inside it is a set of replacement images,
one per game texture you want to override. When the pack is installed and enabled, the game draws
your images instead of the originals. Nothing about the original game files is modified — packs are
layered on top at runtime, and removing the `.o2r` restores stock rendering.

Each replacement is matched to an original texture by a **key** — a short text name like
`common_assets_compressed/aTitleLogoTex`. The key is the bridge between "the thing I saw on screen"
and "the file the game loads". You get the keys for free from the dump step below.

---

## 2. Before you start

You need:

- A working G-Diffuser build (the `G-Diffuser.exe` you already run).
- **Python 3.8+** with **Pillow**: `pip install Pillow`. This is only needed on your PC to build the
  pack; players do not need it.
- An image editor that can open and save PNG with alpha (GIMP, Photoshop, Krita, Aseprite, ...).

Two folders live **next to `G-Diffuser.exe`**. The game creates them on demand:

| Folder   | What it holds                                                        |
| -------- | -------------------------------------------------------------------- |
| `dump/`  | Dumped assets — PNG plus `manifest.tsv`                              |
| `mods/`  | Installed `.o2r` packs. Everything here is scanned at boot.           |

---

## 3. Step by step

### Step 1 — Dump the textures you want to replace

1. Launch the game and open the in-game menu.
2. Go to the **Workshop** tab → **Asset Dump**.
3. Leave **textures** ticked (all classes are on by default; untick the ones you do not need — each
   class runs as its own subprocess, so a broken class cannot abort the rest).
4. Run it. Assets are decoded straight from the extracted archive — you do **not** need to play
   through the screens you want to change.
5. Click **Open dump folder** to open `dump/` in your file browser.

Inside `dump/` you will find:

- `dump/<key>.png` — one PNG per texture, in sub-folders that mirror the key
  (e.g. `dump/common_assets_compressed/aTitleLogoTex.png`).
- `dump/manifest.tsv` — a table listing every dumped texture: **key, native width, native height,
  N64 format**. You do not edit this; the packer reads it in Step 3.

The keys mirror the game's own symbol names (`aTitleLogoTex`, `aKmhTex`, ...), so `manifest.tsv` is
the fastest way to find the one you want.

> **If you are following an older copy of this guide:** it told you to tick "Dump textures while
> playing" and to open a `dump/index.html` contact sheet. Both are gone. The play-as-you-go dumper was
> replaced by Asset Dump above, which is faster and complete — it does not depend on the game
> happening to walk past a texture. No contact sheet is generated any more.

> Tip: the textures are tiny — they are the raw N64 assets. Open them in an editor that can zoom
> without smoothing, or your first edit will look nothing like what ships.

`tools/gen_dump_all.py` does the same job from the command line if you prefer it, and writes the same
`manifest.tsv`.

### Step 2 — Edit the PNGs

Open the PNG for a texture you want to change and repaint it. Rules that matter:

- **Keep the same aspect and an integer multiple of the original size.** If the original is
  `160×6`, your replacement must be `160×6`, `320×12`, `480×18`, `640×24`, ... The packer enforces
  this and will tell you the allowed sizes if you get it wrong. A larger multiple = a hi-res
  replacement.
- **Keep alpha meaningful.** Many textures use their alpha channel for shape/transparency. Save as
  32-bit PNG with alpha.
- **Do not rename the file or move it out of its sub-folder.** The path *is* the key. Rename it and
  the game will not know which texture it replaces.

Delete any dumped PNGs you are **not** replacing — the packer simply ignores keys you leave out, but
a smaller folder is easier to work with. Textures you do not include keep their stock appearance.

### Step 3 — Pack it

From the repo (or anywhere the `tools/` script is reachable), run:

```
python tools/gen_texture_pack.py <path-to-your-edit-folder> my-pack.o2r --name "My Pack" --author you
```

Minimal form — this "just works" using sensible defaults:

```
python tools/gen_texture_pack.py dump/ my-pack.o2r
```

The packer:

- reads `manifest.tsv` from the folder to learn each texture's native size and format,
- re-encodes each of your PNGs into the exact N64 format the game expects (this is required — see
  §6),
- writes `my-pack.o2r`,
- and creates the pack metadata for you from `--name` / `--author` / `--version` if you did not
  supply a `workshop.json` (see §5).

**Validate before you ship** — this writes nothing and lists every problem it finds:

```
python tools/gen_texture_pack.py dump/ --check          # check an edit folder
python tools/gen_texture_pack.py my-pack.o2r --check    # check a built pack
```

If validation reports errors (wrong size, corrupt PNG, ...), fix them and re-run. The packer refuses
to write a pack while any error remains, and it reports **all** problems in one pass so you are not
fixing them one at a time.

### Step 4 — Install

Put `my-pack.o2r` into the `mods/` folder next to `G-Diffuser.exe`. Use **"Open mods folder"** in the
Workshop tab to get there quickly. That is the whole install.

### Step 5 — Enable and reload

In the Workshop tab:

1. Tick **"Texture packs"** (the master switch — off means stock rendering).
2. Your pack appears in the list. Make sure its checkbox is enabled.
3. Click **"Reload packs"**. This re-scans `mods/`, re-mounts packs, and clears the texture cache so
   your edits appear **without restarting the game**. The status line reports how many packs mounted
   and how many overrides are available.

Iterate: edit a PNG → re-run the packer → drop the new `.o2r` in `mods/` → **Reload packs**.

---

## 4. The key scheme

Every texture has a key. Where it comes from decides whether you can replace it:

| Key looks like                                   | Meaning                                | Replaceable?          |
| ------------------------------------------------ | -------------------------------------- | --------------------- |
| `common_assets_compressed/aTitleLogoTex`         | A **named** game asset                 | **Yes**               |
| `machine_custom_gfx/aLogoGoldenFoxTex`           | A **named** game asset                 | **Yes**               |
| `hash/97f5ac8c0cd4fb43`                          | An **unnamed** texture (content hash)  | **No** (dump-only)    |

Named keys are stable across runs and builds (that is what makes them safe to key on). Hash keys are
just a fingerprint of the pixels so unnamed textures still get dumped for reference — but the game
has no name to match them against at draw time, so a pack **cannot** override a `hash/...` texture
today. You can spot them in `manifest.tsv` by the `hash/` prefix on the key.

The build stamps a **key-scheme version** (currently `1`). If a future build renames symbols, the
version bumps and the menu flags older packs as out of date. You normally never touch this.

---

## 5. Pack metadata (`workshop.json`)

Every pack carries a small metadata file **inside** the archive named `workshop.json`:

```json
{
  "name": "My Pack",
  "version": "0.2",
  "author": "you",
  "game_version": "us.rev0",
  "key_scheme_version": "1"
}
```

| Field                | What it does                                                                 |
| -------------------- | --------------------------------------------------------------------------- |
| `name`               | Shown in the Workshop pack list.                                            |
| `version`            | Your pack's version. Free text.                                            |
| `author`             | Shown in the pack list.                                                     |
| `game_version`       | Target build. `us.rev0` is the current port. A different value is flagged. |
| `key_scheme_version` | The key scheme the pack was built against. A mismatch is flagged.          |

You do **not** have to write this file. If it is absent, the packer synthesizes one from the
`--name`, `--author`, `--version`, `--game-version` and `--key-scheme-version` flags, filling any you
omit with safe defaults (`version 0.1`, `game_version us.rev0`, `key_scheme_version 1`, and the name
defaults to the output filename). If you *do* provide a `workshop.json`, any flags you pass override
the matching fields.

> Historical note: the file must be `workshop.json`, **not** `manifest.json` — the latter is a name
> reserved by the engine's archive loader and using it makes packs fail to mount. The packer handles
> this for you.

---

## 6. Why format matters (and what "native format" means)

The game decodes each texture using the **original N64 format** recorded in `manifest.tsv` (the `fmt`
column: `RGBA16`, `I4`, `IA8`, ...), *not* whatever format your PNG happens to be. The packer
therefore re-encodes your edited PNG back into that same native format automatically. You always
work in ordinary RGBA PNG; the packer does the conversion. You do not need to understand the N64
formats — just do not fight the size rule in §Step 2.

**Not every format can be re-encoded.** Paletted formats (`CI4`, `CI8`) are **not supported** yet —
the packer skips them with a warning. Those textures are dump-only for now.

---

## 7. Pack ordering and priority

- All `*.o2r` files in `mods/` are mounted, **sorted by filename** (case-insensitive).
- When two packs provide the **same key**, the later-sorted filename wins. Prefix filenames with
  numbers to control order, e.g. `10-base.o2r`, `20-hud-overrides.o2r` (the `20-` pack overrides the
  `10-` pack on any shared key).
- Disabling a pack in the menu adds its filename to a skip list; it is not mounted on the next
  **Reload packs** or boot. Re-enable and reload to bring it back.

---

## 8. What you CANNOT replace yet (current limitations)

Be aware of these before you plan a pack — some of the most-requested targets are not replaceable in
the current release:

1. **Fonts and other multi-tile "atlas" textures.** Text (the title-screen font, machine-name
   glyphs, menu labels rendered from a glyph sheet) is drawn from a single large buffer sampled at
   many offsets. Replacing it with one image garbles the text, so the engine currently **blocks**
   pack overrides of these buffers. They still dump (so you can see them), but a pack override is
   ignored. Per-tile atlas override is designed and will land in a later release.
2. **Unnamed (`hash/...`) textures.** See §4 — no stable name to match at draw time.
3. **Paletted (`CI4`/`CI8`) textures.** See §6 — not encodable yet.

Everything else — named, non-atlas textures in a supported format (title art, logos, HUD elements,
machine graphics, ...) — is replaceable today.

---

## 9. Troubleshooting

**My pack does not show in the list.**
It must be a `*.o2r` file directly inside `mods/` (not a sub-folder). Click **Reload packs**.

**The list shows my pack but nothing changes on screen.**
- Is the **"Texture packs"** master switch on?
- Is the pack's own checkbox enabled?
- Did you click **Reload packs** after installing/editing?
- Is the texture actually replaceable? Check §8 — fonts, `hash/...` and `CI*` textures will not
  change. The Workshop tab shows an **"N override(s) available"** count; if it is `0`, none of your
  keys matched anything the game can override.

**Text turned into garbage after I added a font pack.**
That is the atlas limitation (§8) — remove the font PNGs from your pack and rebuild. The engine
guards against this now, but older packs built before the guard can still contain those keys (they
are simply ignored today).

**`gen_texture_pack.py` reports errors.**
Read them — each line names the offending key and the exact problem (wrong size with the list of
allowed sizes, unreadable PNG, unknown key, unsupported format). Fix them and re-run, or use
`--check` to iterate without writing a pack.

**Where are the logs?**
The game writes `gdiffuser-run.log` next to `G-Diffuser.exe` (and to stderr if you launched it from a
console). Workshop activity is prefixed `[workshop]` — reload results and dump-write failures show up
there. Grep that file when something silently does not work.

---

## 10. Quick reference

```
# Dump: Workshop tab -> Asset Dump -> run -> "Open dump folder"
# Look:  dump/manifest.tsv lists every key with its native size and format
# Edit:  repaint dump/<key>.png (keep size an integer multiple of the original)

# Build a pack (metadata synthesized from flags):
python tools/gen_texture_pack.py dump/ my-pack.o2r --name "My Pack" --author you

# Validate without writing (lists every problem):
python tools/gen_texture_pack.py dump/ --check
python tools/gen_texture_pack.py my-pack.o2r --check

# Install: copy my-pack.o2r into mods/  (Workshop tab -> "Open mods folder")
# Enable:  Workshop tab -> "Texture packs" on -> enable the pack -> "Reload packs"
```
