#!/usr/bin/env python3
"""R8 Step 4 (audio/midi) -- `audio` and `midi` DumpClasses for tools/gen_dump_all.py.

Registers two more CLASS_REGISTRY entries beside `textures` (this file) and the six in
gen_dump_all_extra.py, WITHOUT touching gen_dump_all.py's architecture: this module only defines
DumpClass subclasses + audio helpers and is imported by gen_dump_all.py, which registers
EXTRA_AUDIO_CLASSES into its CLASS_REGISTRY (same import-after-registry pattern the extra module
uses, to avoid the __main__ circular-import trap).

  audio -- soundfont samples -> .wav via VADPCM decode        (subdir "audio")
  midi  -- music sequences   -> .mid + instrument-map sidecar (subdir "music")

GROUND TRUTH (every format derived from the in-repo decomp, cited per-function in the class
docstrings; nothing reverse-engineered from external sources):
  - decomp/src/audio/disk/audio_tables.c  -- gSoundFontTable/gSequenceTable/gSampleBankTable (EK
    config, 23/23/25 entries); decomp/src/audio/rom/audio_tables.c -- the cart config (2/2/3).
  - decomp/src/audio/disk/lib/load.c gdx_audio_convert_font (~L1222-1303) -- the soundfont binary
    parser (host-native reimplementation of AudioLoad_RelocateFont), mirrored here field-for-field.
  - decomp/src/audio/rom/lib/load.c AudioLoad_RelocateFont (L662-711) -- the cart config's font
    pointer-table layout differs (instruments = &fontDataPtrs[1], no sfx slot; disk uses [2]).
  - port/n64_audio_hle.c RunAdpcm (L1119-1150, block-convolution path) -- the hardware-correct
    VADPCM decode this file's decode_adpcm() mirrors exactly; BookCoef (L822-831) fixes the
    book[pred*16 + tap*8 + col] indexing (order 2).
  - decomp/src/audio/disk/lib/seqplayer.c -- sequence-script command semantics for the midi class.

DATA SOURCES (verified against the checked-in archives):
  - CART: generic.o2r audio_blob/{audio_bank,audio_seq,audio_table} (BLOB framing: 0x40 OTR header
    + u32 size @0x40, payload @0x44). audio_bank=0x2D90 (2 cart fonts), audio_seq=0xC40 (2 cart
    seqs), audio_table=0xA3F1D0 (the 3 cart sample banks concatenated -- the waveform bytes).
  - EK: a SEPARATE archive fzerox-disk.o2r carries 23 ek/aAudioSoundFontDD* (FZX:SOUNDFONT) + 23
    ek/aAudioSeqDD* (FZX:SEQUENCE). These entries are RAW/headerless (zip-entry length == the
    ek_slice_manifest.txt `len`). Their sample banks are mostly MEDIUM_LBA (on the 64DD disk image,
    NOT present in any archive) -- those samples are dumped metadata-only; EK fonts that reference
    the CART banks (GUITAR/BGM/DDBGM_TITLE/SELECT/OPTION, whose offsets index the same audio_table
    blob) ARE fully decodable.

REPLACE-vs-SUPPLEMENT: the rom (cart) and disk (EK) audio_tables.c are separate build configs with
DIFFERENT enum orderings (cart FONT_SOUND_EFFECTS=0/FONT_GUITAR=1; disk FONT_GUITAR=0/
FONT_SOUND_EFFECTS=1/DDBGM_*=2..22). The disk config REPLACES the cart config wholesale (the game
boots either as cart or as EK; gen_dump_all_extra's `tables` class already treats disk/audio_tables.c
as authoritative). Both are dumped here; samples that resolve to byte-identical bank+offset+size are
decoded once and shared (proven-identical dedup, per the plan).
"""
import glob
import json
import os
import re
import struct
import sys
import wave

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_asset_bindings as gab  # noqa: E402
# Reuse the landed C-initializer table parser rather than duplicating it (plan rule).
import gen_dump_all_extra as gda_extra  # noqa: E402

REPO = gab.REPO
OTR_HEADER_SIZE = 64

# SampleMedium (decomp/include/sfx.h:981-986)
MEDIUM_RAM, MEDIUM_LBA, MEDIUM_CART, MEDIUM_DISK_DRIVE = 0, 1, 2, 3
# SampleCodec (decomp/include/sfx.h:988-996)
CODEC_ADPCM, CODEC_S8, CODEC_S16_INMEMORY, CODEC_SMALL_ADPCM, CODEC_REVERB, CODEC_S16, CODEC_UNK6 = range(7)
CODEC_NAMES = {0: "ADPCM", 1: "S8", 2: "S16_INMEMORY", 3: "SMALL_ADPCM", 4: "REVERB", 5: "S16", 6: "UNK6"}

# The audio library's synthesis output rate; N64 samples store no absolute rate, only `tuning` (a
# scale factor). The plausible per-sample playback rate is tuning * this base (documented, derived).
SYNTHESIS_RATE_HZ = 32000

# ── candidate locations for the EK disk archive + its slice manifest (profile-agnostic; the manifest
# serves retail JP and the fan translation alike -- see ek_slice_manifest.txt header) ────────────────
_EK_ARCHIVE_CANDIDATES = [
    os.path.join(REPO, "build", "x64", "port", "Release", "fzerox-disk.o2r"),
    os.path.join(REPO, "build", "x64", "port", "Debug", "fzerox-disk.o2r"),
    os.path.join(REPO, "build", "x64", "port", "RelWithDebInfo", "fzerox-disk.o2r"),
    os.path.join(REPO, "build_x64", "port", "fzerox-disk.o2r"),
]
_EK_MANIFEST = os.path.join(REPO, "port", "gen", "ek_slice_manifest.txt")
_ROM_TABLES_C = os.path.join(REPO, "decomp", "src", "audio", "rom", "audio_tables.c")
_DISK_TABLES_C = os.path.join(REPO, "decomp", "src", "audio", "disk", "audio_tables.c")


class DumpClass:
    """Duck-typed DumpClass base (name/subdir/out_dir), matching gen_dump_all_extra's local copy --
    gen_dump_all.py calls only .name/.subdir/.run()/.out_dir() so subclassing the real base (and
    reintroducing the circular import) is unnecessary."""
    name = "?"
    subdir = ""

    def out_dir(self, ctx):
        return ctx.dump_dir if not self.subdir else os.path.join(ctx.dump_dir, self.subdir)


def _find_first_existing(paths):
    for p in paths:
        if os.path.isfile(p):
            return p
    return None


def _write_manifest(path, header, rows):
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("# %s\n" % header)
        for row in rows:
            fh.write("\t".join(str(c) for c in row) + "\n")


# ── big-endian readers (all N64 audio data is big-endian) ─────────────────────────────────────────
def _u32(b, o):
    return struct.unpack_from(">I", b, o)[0]


def _s16(b, o):
    return struct.unpack_from(">h", b, o)[0]


def _f32(b, o):
    return struct.unpack_from(">f", b, o)[0]


def _blob_payload(zf, key):
    """CART audio_blob/* entries: 0x40 OTR header + u32 size @0x40, payload @0x44 (verified: the
    payload length equals audio_blob.yaml's documented size for each of bank/seq/table)."""
    if key not in zf.namelist():
        return None
    return zf.read(key)[OTR_HEADER_SIZE + 4:]


# ══════════════════════════════════════════════════════════════════════════════════════════════════
# AudioTable C-initializer parsing (reuses gen_dump_all_extra.TablesDumpClass._parse_c_table)
# ══════════════════════════════════════════════════════════════════════════════════════════════════
class _SafeEval:
    """Evaluate the small bit-packed field expressions the audio tables use (e.g.
    "(SAMPLE_GUITAR << 8) | 0xFF", "(71 << 8) | 0") with SAMPLE_*/FONT_* names resolved to their
    enum indices. Only names, int literals and | & << >> ( ) appear -- restricted eval is safe."""
    _ALLOWED = re.compile(r"^[0-9A-Za-z_\s\(\)\|\&\<\>x]*$")

    def __init__(self, names):
        self.names = dict(names)

    def __call__(self, expr, default=0):
        if expr is None:
            return default
        expr = expr.strip()
        if expr == "" or expr == "-":
            return default
        if not self._ALLOWED.match(expr):
            raise ValueError("unsafe table expression: %r" % expr)
        try:
            return int(eval(expr, {"__builtins__": {}}, self.names))  # noqa: S307 (restricted)
        except Exception as exc:  # noqa: BLE001
            raise ValueError("cannot evaluate %r (%s)" % (expr, exc))


def _load_audio_tables():
    """Parse both audio_tables.c configs. Returns a dict with:
        disk_font/disk_seq/disk_bank -- 23/23/25-entry parsed tables (EK config, authoritative)
        rom_font/rom_seq/rom_bank    -- 2/2/3-entry parsed tables (cart config)
        sample_names                 -- {SAMPLE_*: index} from the disk bank table (full 0..24 enum)
        evalr                        -- _SafeEval bound to sample_names
    Each parsed table is the TablesDumpClass._parse_c_table dict (headerRaw/numEntries/entries[]),
    with entries carrying name/offset/size/medium/cachePolicy/col5_raw/col6_raw/col7_raw.
    """
    parse = gda_extra.TablesDumpClass._parse_c_table
    out = {}
    with open(_DISK_TABLES_C, encoding="utf-8") as fh:
        disk_text = fh.read()
    with open(_ROM_TABLES_C, encoding="utf-8") as fh:
        rom_text = fh.read()
    out["disk_font"] = parse(disk_text, "gSoundFontTable")
    out["disk_seq"] = parse(disk_text, "gSequenceTable")
    out["disk_bank"] = parse(disk_text, "gSampleBankTable")
    out["rom_font"] = parse(rom_text, "gSoundFontTableData")
    out["rom_seq"] = parse(rom_text, "gSequenceTableData")
    out["rom_bank"] = parse(rom_text, "gSampleBankTableData")
    # Full SAMPLE_* enum index map from the disk bank table (its rows ARE the enum, order 0..24).
    sample_names = {}
    if out["disk_bank"]:
        for idx, e in enumerate(out["disk_bank"]["entries"]):
            sample_names[e["name"]] = idx
    # The medium/cachePolicy columns use symbolic constants; supply their values for eval.
    eval_names = dict(sample_names)
    eval_names.update({"MEDIUM_RAM": MEDIUM_RAM, "MEDIUM_LBA": MEDIUM_LBA, "MEDIUM_CART": MEDIUM_CART,
                       "MEDIUM_DISK_DRIVE": MEDIUM_DISK_DRIVE})
    for i in range(8):
        eval_names["CACHEPOLICY_%d" % i] = i
    out["sample_names"] = sample_names
    out["evalr"] = _SafeEval(eval_names)
    return out


def _bank_entry(tables, bank_id):
    """Resolve a numeric SAMPLE_* bank id (full enum index) to (name, offset, size, medium) via the
    disk gSampleBankTable. The cart banks (SOUND_EFFECTS/BGM/GUITAR) share identical offsets in both
    configs, so one table resolves every font's banks; CART-medium banks index the audio_table blob,
    LBA-medium banks live only on the 64DD image (no waveform bytes in any archive)."""
    bank = tables["disk_bank"]
    if bank is None or bank_id is None or bank_id < 0 or bank_id >= len(bank["entries"]):
        return None
    e = bank["entries"][bank_id]
    evalr = tables["evalr"]
    return (e["name"], evalr(e["offset"]), evalr(e["size"]), evalr(e["medium"]))


# ══════════════════════════════════════════════════════════════════════════════════════════════════
# Soundfont binary parser -- mirrors decomp/src/audio/disk/lib/load.c gdx_audio_convert_font.
# `inst_base` selects the config's pointer-table layout: disk/EK = 2 (drum slot + sfx slot then
# instruments), rom/cart = 1 (drum slot then instruments, no sfx -- rom/lib/load.c L689,710).
# ══════════════════════════════════════════════════════════════════════════════════════════════════
class _Sample:
    __slots__ = ("codec", "bank_sel", "size", "sample_addr", "loop", "book", "hdr_off")

    def __init__(self, **kw):
        for k in self.__slots__:
            setattr(self, k, kw.get(k))


def _parse_book(data, off):
    if off == 0 or off + 8 > len(data):
        return None
    order = _u32(data, off)
    npred = _u32(data, off + 4)
    if order < 1 or order > 8 or npred < 1 or npred > 8:
        return None  # insane header = wrong bytes (matches gdx_fontconv_book's guard)
    n = 8 * order * npred
    if off + 8 + n * 2 > len(data):
        return None
    coefs = [_s16(data, off + 8 + i * 2) for i in range(n)]
    return {"order": order, "numPredictors": npred, "coefs": coefs}


def _parse_loop(data, off):
    if off == 0 or off + 12 > len(data):
        return None
    start = _u32(data, off)
    end = _u32(data, off + 4)
    count = _u32(data, off + 8)
    state = None
    if count != 0 and off + 0x10 + 32 <= len(data):
        state = [_s16(data, off + 0x10 + i * 2) for i in range(16)]
    return {"start": start, "end": end, "count": count, "predictorState": state}


def _parse_sample(data, hdr_off, cache):
    if hdr_off == 0 or hdr_off + 0x10 > len(data):
        return None
    if hdr_off in cache:
        return cache[hdr_off]
    flags = _u32(data, hdr_off)
    codec = (flags >> 28) & 7
    bank_sel = (flags >> 26) & 3  # on-disk medium bits: MEDIUM_RAM(0)->bank1, MEDIUM_LBA(1)->bank2
    size = flags & 0xFFFFFF
    sample_addr = _u32(data, hdr_off + 4)
    loop = _parse_loop(data, _u32(data, hdr_off + 8))
    book = _parse_book(data, _u32(data, hdr_off + 0xC))
    s = _Sample(codec=codec, bank_sel=bank_sel, size=size, sample_addr=sample_addr,
                loop=loop, book=book, hdr_off=hdr_off)
    cache[hdr_off] = s
    return s


def _parse_envelope(data, off, max_points=64):
    if off == 0 or off >= len(data):
        return []
    pts = []
    for i in range(max_points):
        p = off + i * 4
        if p + 4 > len(data):
            break
        delay = _s16(data, p)
        arg = _s16(data, p + 2)
        pts.append({"delay": delay, "arg": arg})
        if delay <= 0:  # terminator (delay <= 0), matches gdx_fontconv_envelope's scan
            break
    return pts


def _parse_tuned(data, off, cache):
    """TunedSample: u32 sample-header-offset, f32 tuning."""
    if off + 8 > len(data):
        return None
    sample = _parse_sample(data, _u32(data, off), cache)
    tuning = _f32(data, off + 4)
    if sample is None:
        return None
    return {"sample": sample, "tuning": tuning}


def parse_font(data, num_inst, num_drums, num_sfx, inst_base):
    """Return {instruments:[], drums:[], soundEffects:[]} of parsed entries. Struct offsets from
    audio.h (Instrument 0x20 L101-110, Drum 0x10 L112-118). Sample cache dedups shared headers."""
    cache = {}
    drum_arr_off = _u32(data, 0) if len(data) >= 4 else 0
    sfx_arr_off = _u32(data, 4) if (inst_base == 2 and len(data) >= 8) else 0

    drums = []
    if num_drums > 0 and drum_arr_off != 0:
        for i in range(num_drums):
            p = drum_arr_off + i * 4
            if p + 4 > len(data):
                break
            d_off = _u32(data, p)
            if d_off == 0 or d_off + 0x10 > len(data):
                drums.append(None)
                continue
            tuned = _parse_tuned(data, d_off + 4, cache)
            drums.append({
                "index": i, "adsrDecayIndex": data[d_off], "pan": data[d_off + 1],
                "tunedSample": tuned,
                "envelope": _parse_envelope(data, _u32(data, d_off + 0xC)),
            })

    sfx = []
    if inst_base == 2 and num_sfx > 0 and sfx_arr_off != 0:
        for i in range(num_sfx):
            p = sfx_arr_off + i * 8  # SoundEffect = TunedSample (0x8)
            if p + 8 > len(data):
                break
            if _u32(data, p) == 0:
                sfx.append(None)
                continue
            sfx.append({"index": i, "tunedSample": _parse_tuned(data, p, cache)})

    instruments = []
    for i in range(num_inst):
        p = (inst_base + i) * 4
        if p + 4 > len(data):
            break
        i_off = _u32(data, p)
        if i_off == 0 or i_off + 0x20 > len(data):
            instruments.append(None)
            continue
        range_lo = data[i_off + 1]
        range_hi = data[i_off + 2]
        inst = {
            "index": i, "normalRangeLo": range_lo, "normalRangeHi": range_hi,
            "adsrDecayIndex": data[i_off + 3],
            "envelope": _parse_envelope(data, _u32(data, i_off + 4)),
            "lowPitch": _parse_tuned(data, i_off + 8, cache) if range_lo != 0 else None,
            "normalPitch": _parse_tuned(data, i_off + 0x10, cache),
            "highPitch": _parse_tuned(data, i_off + 0x18, cache) if range_hi != 0x7F else None,
        }
        instruments.append(inst)

    return {"instruments": instruments, "drums": drums, "soundEffects": sfx}


def font_samples(parsed):
    """Yield (role, _Sample, tuning) for every distinct-by-header sample a parsed font references."""
    seen = set()

    def emit(role, tuned):
        if tuned is None or tuned["sample"] is None:
            return
        s = tuned["sample"]
        if s.hdr_off in seen:
            return
        seen.add(s.hdr_off)
        yield_list.append((role, s, tuned["tuning"]))

    yield_list = []
    for inst in parsed["instruments"]:
        if inst is None:
            continue
        for role, key in (("inst%d_low" % inst["index"], "lowPitch"),
                          ("inst%d_normal" % inst["index"], "normalPitch"),
                          ("inst%d_high" % inst["index"], "highPitch")):
            emit(role, inst[key])
    for drum in parsed["drums"]:
        if drum is not None:
            emit("drum%d" % drum["index"], drum["tunedSample"])
    for s in parsed["soundEffects"]:
        if s is not None:
            emit("sfx%d" % s["index"], s["tunedSample"])
    return yield_list


# ══════════════════════════════════════════════════════════════════════════════════════════════════
# VADPCM decode -- mirrors port/n64_audio_hle.c RunAdpcm block-convolution path (L1119-1150), the
# hardware-correct decode. Frame = 1 header byte (hi nibble SHIFT, lo nibble PREDICTOR) + data; two
# 8-sample sub-blocks; BookCoef(pred,tap,col) = book[pred*16 + tap*8 + col] for order 2 (L822-823).
# ══════════════════════════════════════════════════════════════════════════════════════════════════
def _clamp_s16(v):
    if v > 32767:
        return 32767
    if v < -32768:
        return -32768
    return v


def decode_adpcm(data, book, small=False):
    """Decode a whole CODEC_ADPCM (or CODEC_SMALL_ADPCM) sample. Returns list[int] of s16 samples.
    History seeds from 0 (A_INIT, the start of a fresh sample -- RunAdpcm L1047-1048)."""
    order = book["order"]
    npred = book["numPredictors"]
    coefs = book["coefs"]
    row_stride = 8 * order  # shorts per predictor (== 16 for order 2)

    def bc(pred, tap, col):
        idx = pred * row_stride + tap * 8 + col
        return coefs[idx] if 0 <= idx < len(coefs) else 0

    data_bytes = 4 if small else 8
    frame_bytes = 1 + data_bytes
    out = []
    h1 = h2 = 0  # clamped history: newer(h1), older(h2)
    nframes = len(data) // frame_bytes
    for f in range(nframes):
        base = f * frame_bytes
        header = data[base]
        shift = (header >> 4) & 0xF
        pred = header & 0xF
        if pred >= npred:
            pred = 0  # defensive: out-of-range predictor row -> 0 (BookCoef returns 0 past book end)
        if small:
            # 2-bit sequential path (RunAdpcm L1161-1164); order-2 per-sample form uses column 0.
            # Deferred clamping (mirrors RunAdpcm L1085-1221 exactly, task A4): the recursion
            # WITHIN this 16-sample frame reads/writes RAW (unclamped) history -- clamping every
            # sample before feeding it back would silently discard mid-frame overshoot from every
            # later sample's prediction, unlike the real ucode. Only a CLAMPED shadow crosses the
            # frame boundary and becomes the stored output sample. h1/h2 here are that clamped
            # shadow (same role as clHist1/clHist2 in RunAdpcm, and what seeds the next frame);
            # raw1/raw2 mirror hist1/hist2 -- reset from the clamped shadow at each frame's start
            # (a no-op at f==0, since both start at the same A_INIT 0 seed).
            raw1, raw2 = h1, h2
            for s in range(16):
                byte_val = data[base + 1 + (s // 4)]
                nib = (byte_val >> ((3 - (s % 4)) * 2)) & 0x3
                if nib & 0x2:
                    nib -= 4
                residual = nib << shift
                predicted = (bc(pred, 0, 0) * raw2 + bc(pred, 1, 0) * raw1) >> 11
                sample_out = predicted + residual  # RAW, unclamped -- feeds the next prediction
                clamped_out = _clamp_s16(sample_out)
                out.append(clamped_out)
                raw2, raw1 = raw1, sample_out     # RAW carry, intra-frame only (== hist1/hist2)
                h2, h1 = h1, clamped_out           # CLAMPED shadow, crosses frames (== clHist1/clHist2)
        else:
            e_h2, e_h1 = h2, h1  # sub-block entry history (older, newer)
            for sub in range(2):
                e = []
                for i in range(8):
                    si = sub * 8 + i
                    bv = data[base + 1 + (si // 2)]
                    nib = (bv >> 4) & 0xF if (si % 2 == 0) else bv & 0xF
                    if nib & 0x8:
                        nib -= 16
                    e.append(nib << shift)
                sblk = []
                for i in range(8):
                    acc = bc(pred, 0, i) * e_h2 + bc(pred, 1, i) * e_h1
                    for k in range(i):
                        acc += bc(pred, 1, i - 1 - k) * e[k]
                    acc += e[i] << 11
                    v = _clamp_s16(acc >> 11)
                    sblk.append(v)
                    out.append(v)
                e_h2, e_h1 = sblk[6], sblk[7]
            h2, h1 = e_h2, e_h1
    return out


def decode_s16(data):
    """CODEC_S16 / CODEC_S16_INMEMORY: raw big-endian s16 PCM."""
    n = len(data) // 2
    return [_s16(data, i * 2) for i in range(n)]


def decode_s8(data):
    """CODEC_S8: signed 8-bit -> 16-bit (sign-extend + <<8, RunS8Dec)."""
    out = []
    for b in data:
        v = b - 256 if b >= 128 else b
        out.append(v << 8)
    return out


def decode_sample_bytes(codec, data, book):
    if codec == CODEC_ADPCM:
        return decode_adpcm(data, book, small=False) if book else None
    if codec == CODEC_SMALL_ADPCM:
        return decode_adpcm(data, book, small=True) if book else None
    if codec in (CODEC_S16, CODEC_S16_INMEMORY):
        return decode_s16(data)
    if codec == CODEC_S8:
        return decode_s8(data)
    return None  # CODEC_REVERB / CODEC_UNK6 -- not a raw waveform


def write_wav_mono16(path, samples, rate):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(max(int(rate), 1))
        w.writeframes(struct.pack("<%dh" % len(samples), *samples))


# ══════════════════════════════════════════════════════════════════════════════════════════════════
# EK slice manifest (symbol -> len) + FONT/SEQ enum ordering (sfx.h) -> ek symbol map.
# ══════════════════════════════════════════════════════════════════════════════════════════════════
def _load_ek_manifest_lens():
    lens = {}
    if not os.path.isfile(_EK_MANIFEST):
        return lens
    with open(_EK_MANIFEST, encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line or line.startswith("#") or line.startswith(("version", "count")):
                continue
            parts = line.split()
            if len(parts) >= 3 and parts[3:4] in (["FZX:SEQUENCE"], ["FZX:SOUNDFONT"]):
                try:
                    lens[parts[0]] = int(parts[2])
                except ValueError:
                    pass
    return lens


# FONT_* enum order (decomp/include/sfx.h:170-192, disk config) -> ek_slice_manifest symbol. The
# soundfont blob carries no instrument count, so each font is matched to its gSoundFontTable entry by
# THIS index; the ek entry payload length is asserted == gSoundFontTable[i].size as a framing check.
EK_FONT_SYMBOLS = [
    "aAudioSoundFontDDGuitar", "aAudioSoundFontDDSE", "aAudioSoundFontDDMuteCity",
    "aAudioSoundFontDDSilence", "aAudioSoundFontDDSandOcean", "aAudioSoundFontDDPortTown",
    "aAudioSoundFontDDBigBlue", "aAudioSoundFontDDDevilsForest", "aAudioSoundFontDDRedCanyon",
    "aAudioSoundFontDDSector", "aAudioSoundFontDDWhiteLand", "aAudioSoundFontDDRainbowRoad",
    "aAudioSoundFontDDNew03", "aAudioSoundFontDDNew02", "aAudioSoundFontDDNew01",
    "aAudioSoundFontDDNew04", "aAudioSoundFontDDTitle", "aAudioSoundFontDDSelect",
    "aAudioSoundFontDDOption", "aAudioSoundFontDDDeathRace", "aAudioSoundFontDDCourseEditor",
    "aAudioSoundFontDDMachineEditor", "aAudioSoundFontDDEADDemo",
]
# SEQ_* enum order (decomp/include/sfx.h:138-160, disk config) -> ek_slice_manifest symbol.
EK_SEQ_SYMBOLS = [
    "aAudioSeqDDGuitar", "aAudioSeqDDSE", "aAudioSeqDDMuteCity", "aAudioSeqDDSilence",
    "aAudioSeqDDSandOcean", "aAudioSeqDDPortTown", "aAudioSeqDDBigBlue", "aAudioSeqDDDevilsForest",
    "aAudioSeqDDRedCanyon", "aAudioSeqDDSector", "aAudioSeqDDWhiteLand", "aAudioSeqDDRainbowRoad",
    "aAudioSeqDDNew03", "aAudioSeqDDNew02", "aAudioSeqDDNew01", "aAudioSeqDDNew04",
    "aAudioSeqDDTitle", "aAudioSeqDDSelect", "aAudioSeqDDOption", "aAudioSeqDDDeathRace",
    "aAudioSeqDDCourseEditor", "aAudioSeqDDMachineEditor", "aAudioSeqDDEADDemo",
]


def _ek_payload(zf, symbol, expect_len):
    """Return the raw asset bytes for an ek/<symbol> entry. Framing is empirical: the entry is raw
    (whole zip entry == expect_len) in the checked-in archive, but tolerate a 0x40/0x44-header
    framing too by matching the payload length against the manifest `len`."""
    key = "ek/" + symbol
    if key not in zf.namelist():
        return None
    data = zf.read(key)
    for start in (0, OTR_HEADER_SIZE, OTR_HEADER_SIZE + 4):
        if len(data) - start == expect_len:
            return data[start:]
    # Last resort: whole entry (length mismatch is surfaced by the caller's size assertion).
    return data


def _open_ek_archive():
    import zipfile
    path = _find_first_existing(_EK_ARCHIVE_CANDIDATES)
    if path is None:
        return None, None
    return zipfile.ZipFile(path), path


# ══════════════════════════════════════════════════════════════════════════════════════════════════
# 1) audio -- soundfont samples -> WAV (+ per-font instrument JSON + manifest)
# ══════════════════════════════════════════════════════════════════════════════════════════════════
class SampleDumpClass(DumpClass):
    """See module docstring. Emits, under dump/audio/:
        samples/<BANK>__0x<addr>__<size>.wav  -- decoded PCM, deduped by (bank,addr,size) so BGM
                                                 samples shared across fonts decode exactly once.
        fonts/<label>.json                    -- per-font instrument/drum/sfx map -> sample refs
                                                 (tuning, codec, loop, book dims, playback rate).
        manifest.tsv                          -- one row per (font, role, sample) reference.
    A sample is decoded when its bank is MEDIUM_CART (waveform in the audio_table blob); MEDIUM_LBA
    banks live only on the 64DD image and are dumped metadata-only (status `no-waveform-data`).
    """
    name = "audio"
    subdir = "audio"

    def _sample_key(self, bank_name, addr, size):
        return "%s__0x%X__%d" % (bank_name, addr, size)

    def _collect_fonts(self, ctx, tables, ek_zip, ek_lens):
        """Return a list of font descriptors: dict(label, origin, font_index, data, num_inst,
        num_drums, num_sfx, bank_ids(list), bank_table, inst_base)."""
        fonts = []
        evalr = tables["evalr"]

        # -- CART fonts: audio_bank blob sliced by rom gSoundFontTableData (inst_base 1) --
        bank_blob = _blob_payload(ctx.source.zip, "audio_blob/audio_bank")
        rom_font = tables["rom_font"]
        if bank_blob is not None and rom_font is not None:
            for idx, e in enumerate(rom_font["entries"]):
                off = evalr(e["offset"])
                size = evalr(e["size"])
                sd2 = evalr(e["col6_raw"])           # (numInst<<8)|numDrums
                num_inst = (sd2 >> 8) & 0xFF
                num_drums = sd2 & 0xFF
                num_sfx = evalr(e["col7_raw"])
                sd1 = evalr(e["col5_raw"])           # (bankId1<<8)|bankId2
                bank_ids = [(sd1 >> 8) & 0xFF, sd1 & 0xFF]
                fonts.append({
                    "label": "cart_%s" % e["name"], "origin": "cart",
                    "font_index": idx, "data": bank_blob[off:off + size],
                    "num_inst": num_inst, "num_drums": num_drums, "num_sfx": num_sfx,
                    "bank_ids": bank_ids, "inst_base": 1, "declared_size": size,
                })

        # -- EK fonts: ek/aAudioSoundFontDD* matched to disk gSoundFontTable by enum order (base 2) --
        disk_font = tables["disk_font"]
        if ek_zip is not None and disk_font is not None:
            for idx, symbol in enumerate(EK_FONT_SYMBOLS):
                if idx >= len(disk_font["entries"]):
                    break
                e = disk_font["entries"][idx]
                declared = evalr(e["size"])
                payload = _ek_payload(ek_zip, symbol, ek_lens.get(symbol, declared))
                if payload is None:
                    continue
                sd2 = evalr(e["col6_raw"])
                sd1 = evalr(e["col5_raw"])
                fonts.append({
                    "label": "ek_%s" % e["name"], "origin": "ek",
                    "font_index": idx, "data": payload,
                    "num_inst": (sd2 >> 8) & 0xFF, "num_drums": sd2 & 0xFF,
                    "num_sfx": evalr(e["col7_raw"]),
                    "bank_ids": [(sd1 >> 8) & 0xFF, sd1 & 0xFF],
                    "inst_base": 2, "declared_size": declared, "ek_symbol": symbol,
                })
        return fonts

    def run(self, ctx):
        tables = _load_audio_tables()
        audio_table = _blob_payload(ctx.source.zip, "audio_blob/audio_table")  # cart waveform bytes
        if audio_table is None:
            print("  audio: generic.o2r audio_blob/audio_table missing -- cannot decode; skipping")
            return {"class": self.name, "dumped": 0, "skipped": 0, "failed": 1, "total": 0}

        out_dir = self.out_dir(ctx)
        samples_dir = os.path.join(out_dir, "samples")
        fonts_dir = os.path.join(out_dir, "fonts")
        os.makedirs(samples_dir, exist_ok=True)
        os.makedirs(fonts_dir, exist_ok=True)

        ek_zip, ek_path = _open_ek_archive()
        ek_lens = _load_ek_manifest_lens()
        try:
            fonts = self._collect_fonts(ctx, tables, ek_zip, ek_lens)
        finally:
            pass
        if ek_zip is None:
            print("  audio: note -- EK archive (fzerox-disk.o2r) not found; cart audio only")
        manifest_rows = []
        spot = []  # (name, rateHz, frames, ms) for the eyeball table
        decoded_keys = {}  # key -> True (decoded this run or already on disk)
        dumped = skipped = failed = meta_only = 0
        framing_ok = 0

        for fd in fonts:
            data = fd["data"]
            # Framing self-check: parsed payload length must equal the declared table size.
            size_match = (len(data) == fd["declared_size"])
            if size_match:
                framing_ok += 1
            parsed = parse_font(data, fd["num_inst"], fd["num_drums"], fd["num_sfx"], fd["inst_base"])
            font_json = {
                "label": fd["label"], "origin": fd["origin"], "fontIndex": fd["font_index"],
                "declaredSize": fd["declared_size"], "actualSize": len(data),
                "sizeMatchesTable": size_match,
                "numInstruments": fd["num_inst"], "numDrums": fd["num_drums"],
                "numSfx": fd["num_sfx"], "sampleBankIds": fd["bank_ids"],
                "sampleBankNames": [_bank_entry(tables, b)[0] if _bank_entry(tables, b) else "none"
                                    for b in fd["bank_ids"]],
                "instBase": fd["inst_base"],
                "instruments": [], "drums": [], "soundEffects": [],
                "samples": {},
            }
            if fd["origin"] == "ek":
                font_json["ekSymbol"] = fd.get("ek_symbol")

            # Build the JSON entry structures (with sample refs) + collect distinct samples.
            def sample_ref(tuned):
                if tuned is None or tuned["sample"] is None:
                    return None
                s = tuned["sample"]
                bank_id = fd["bank_ids"][0] if s.bank_sel != MEDIUM_LBA else fd["bank_ids"][1]
                be = _bank_entry(tables, bank_id)
                bank_name = be[0] if be else "bank%d" % bank_id
                key = self._sample_key(bank_name, s.sample_addr, s.size)
                if key not in font_json["samples"]:
                    font_json["samples"][key] = self._sample_meta(s, tuned["tuning"], be)
                return {"sample": key, "tuning": tuned["tuning"]}

            for inst in parsed["instruments"]:
                if inst is None:
                    font_json["instruments"].append(None)
                    continue
                font_json["instruments"].append({
                    "index": inst["index"], "normalRangeLo": inst["normalRangeLo"],
                    "normalRangeHi": inst["normalRangeHi"], "adsrDecayIndex": inst["adsrDecayIndex"],
                    "envelopePoints": len(inst["envelope"]),
                    "lowPitch": sample_ref(inst["lowPitch"]),
                    "normalPitch": sample_ref(inst["normalPitch"]),
                    "highPitch": sample_ref(inst["highPitch"]),
                })
            for drum in parsed["drums"]:
                if drum is None:
                    font_json["drums"].append(None)
                    continue
                font_json["drums"].append({
                    "index": drum["index"], "adsrDecayIndex": drum["adsrDecayIndex"],
                    "pan": drum["pan"], "sample": sample_ref(drum["tunedSample"]),
                })
            for s in parsed["soundEffects"]:
                if s is None:
                    font_json["soundEffects"].append(None)
                    continue
                font_json["soundEffects"].append({
                    "index": s["index"], "sample": sample_ref(s["tunedSample"]),
                })

            # Decode / write each distinct sample this font references.
            for role, s, tuning in font_samples(parsed):
                bank_id = fd["bank_ids"][0] if s.bank_sel != MEDIUM_LBA else fd["bank_ids"][1]
                be = _bank_entry(tables, bank_id)
                bank_name = be[0] if be else "bank%d" % bank_id
                key = self._sample_key(bank_name, s.sample_addr, s.size)
                rate = int(round(tuning * SYNTHESIS_RATE_HZ)) if tuning and tuning > 0 else 0
                wav_path = os.path.join(samples_dir, key + ".wav")

                if be is None or be[3] != MEDIUM_CART:
                    manifest_rows.append((fd["label"], role, key, CODEC_NAMES.get(s.codec, s.codec),
                                          rate, 0, "no-waveform-data(%s)"
                                          % (be[0] if be else "unknown-bank")))
                    meta_only += 1
                    continue

                bank_off = be[1]
                start = bank_off + s.sample_addr
                raw = audio_table[start:start + s.size]
                if len(raw) < s.size or s.size == 0:
                    manifest_rows.append((fd["label"], role, key,
                                          CODEC_NAMES.get(s.codec, s.codec), rate, 0, "short-read"))
                    failed += 1
                    continue

                if key in decoded_keys or os.path.exists(wav_path):
                    decoded_keys[key] = True
                    frames = os.path.getsize(wav_path) // 2 if os.path.exists(wav_path) else 0
                    manifest_rows.append((fd["label"], role, key,
                                          CODEC_NAMES.get(s.codec, s.codec), rate, frames, "shared"
                                          if key in decoded_keys and not os.path.exists(wav_path)
                                          else "ok-existing"))
                    skipped += 1
                    continue

                pcm = decode_sample_bytes(s.codec, raw, s.book)
                if pcm is None:
                    manifest_rows.append((fd["label"], role, key,
                                          CODEC_NAMES.get(s.codec, s.codec), rate, 0,
                                          "codec-unsupported"))
                    failed += 1
                    continue
                write_wav_mono16(wav_path, pcm, rate if rate > 0 else SYNTHESIS_RATE_HZ)
                decoded_keys[key] = True
                dumped += 1
                # structural invariant check (loop end <= decoded sample count)
                loop_ok = True
                if s.loop and s.loop["count"] != 0:
                    loop_ok = s.loop["end"] <= len(pcm)
                manifest_rows.append((fd["label"], role, key, CODEC_NAMES.get(s.codec, s.codec),
                                      rate if rate > 0 else SYNTHESIS_RATE_HZ, len(pcm),
                                      "ok" if loop_ok else "ok-LOOPEND-OOB"))
                if len(spot) < 24:
                    ms = int(len(pcm) * 1000 / (rate if rate > 0 else SYNTHESIS_RATE_HZ))
                    spot.append((key, rate if rate > 0 else SYNTHESIS_RATE_HZ, len(pcm), ms))

            # Write per-font JSON (idempotent: only when absent, but always counted).
            json_path = os.path.join(fonts_dir, fd["label"] + ".json")
            if not os.path.exists(json_path):
                with open(json_path, "w", encoding="utf-8") as fh:
                    json.dump(font_json, fh, indent=2)

        _write_manifest(os.path.join(out_dir, "manifest.tsv"),
                        "font\trole\tsampleKey\tcodec\tplaybackRateHz\tframes\tstatus   "
                        "(WAV = VADPCM decode of the cart audio_table blob; LBA banks metadata-only)",
                        manifest_rows)

        print("  audio: %d WAV decoded, %d skipped/shared, %d metadata-only (LBA), %d failed "
              "(%d fonts, framing %d/%d size-matched)"
              % (dumped, skipped, meta_only, failed, len(fonts), framing_ok, len(fonts)))
        if spot:
            print("    spot-check (name / rateHz / frames / ms):")
            for name, rate, frames, ms in spot[:8]:
                print("      %-46s %6d %8d %7d" % (name[:46], rate, frames, ms))
        return {"class": self.name, "dumped": dumped, "skipped": skipped,
                "failed": failed, "total": len(manifest_rows)}

    def verify(self, ctx):
        """Offline==offline acceptance test: re-decode every sample that already has a WAV on disk
        and byte-compare the PCM against the file. A pure integer decoder must be deterministic, so
        any divergence is a real regression."""
        tables = _load_audio_tables()
        audio_table = _blob_payload(ctx.source.zip, "audio_blob/audio_table")
        out_dir = self.out_dir(ctx)
        samples_dir = os.path.join(out_dir, "samples")
        ek_zip, _ = _open_ek_archive()
        ek_lens = _load_ek_manifest_lens()
        overlap = matched = diverged = errored = missing = 0
        divergences = []
        checked = set()
        if audio_table is None:
            return {"overlap": 0, "matched": 0, "diverged": 0, "errored": 1, "missing": 0,
                    "divergences": ["audio_table blob missing"]}
        for fd in self._collect_fonts(ctx, tables, ek_zip, ek_lens):
            parsed = parse_font(fd["data"], fd["num_inst"], fd["num_drums"], fd["num_sfx"],
                                fd["inst_base"])
            for _role, s, _tuning in font_samples(parsed):
                bank_id = fd["bank_ids"][0] if s.bank_sel != MEDIUM_LBA else fd["bank_ids"][1]
                be = _bank_entry(tables, bank_id)
                if be is None or be[3] != MEDIUM_CART:
                    continue
                bank_name = be[0]
                key = self._sample_key(bank_name, s.sample_addr, s.size)
                if key in checked:
                    continue
                checked.add(key)
                wav_path = os.path.join(samples_dir, key + ".wav")
                if not os.path.exists(wav_path):
                    missing += 1
                    continue
                overlap += 1
                raw = audio_table[be[1] + s.sample_addr:be[1] + s.sample_addr + s.size]
                try:
                    pcm = decode_sample_bytes(s.codec, raw, s.book)
                    with wave.open(wav_path, "rb") as w:
                        on_disk = struct.unpack("<%dh" % w.getnframes(), w.readframes(w.getnframes()))
                except Exception as exc:  # noqa: BLE001
                    errored += 1
                    divergences.append("%s: %s" % (key, exc))
                    continue
                if pcm is not None and list(on_disk) == pcm:
                    matched += 1
                else:
                    diverged += 1
                    divergences.append("%s: PCM mismatch (%d vs %d frames)"
                                       % (key, len(on_disk), len(pcm) if pcm else -1))
        return {"overlap": overlap, "matched": matched, "diverged": diverged,
                "errored": errored, "missing": missing, "divergences": divergences}

    def _sample_meta(self, s, tuning, bank_entry):
        book = None
        if s.book:
            book = {"order": s.book["order"], "numPredictors": s.book["numPredictors"],
                    "coefCount": len(s.book["coefs"]),
                    "coefCountExpected": 8 * s.book["order"] * s.book["numPredictors"]}
        loop = None
        if s.loop:
            loop = {"start": s.loop["start"], "end": s.loop["end"], "count": s.loop["count"],
                    "hasPredictorState": s.loop["predictorState"] is not None}
        return {
            "codec": CODEC_NAMES.get(s.codec, s.codec), "bankSel": s.bank_sel,
            "sampleAddr": "0x%X" % s.sample_addr, "sizeBytes": s.size,
            "tuning": tuning, "playbackRateHz": int(round(tuning * SYNTHESIS_RATE_HZ))
            if tuning and tuning > 0 else 0,
            "bank": bank_entry[0] if bank_entry else None,
            "bankMedium": {0: "RAM", 1: "LBA", 2: "CART", 3: "DISK_DRIVE"}.get(
                bank_entry[3] if bank_entry else -1, "?"),
            "decodable": bool(bank_entry and bank_entry[3] == MEDIUM_CART),
            "book": book, "loop": loop,
        }


# ══════════════════════════════════════════════════════════════════════════════════════════════════
# 2) midi -- music sequences -> .mid (+ per-sequence instrument-map sidecar + manifest)
#
# The aseq bytecode is a 3-level script (player -> channels -> layers), decoded per
# decomp/src/audio/disk/lib/seqplayer.c. This converter is a LINEAR disassembler (per the plan:
# loops -> CC111 markers, engine commands -> MIDI text markers, documented lossy) rather than a full
# runtime simulation: it walks each script from its entry PC consuming exactly the bytes each command
# takes (arg lengths from sSeqInstructionArgsTable L44-126 for cmd>=0xB0, and the per-level switch
# bodies for the low opcodes), emitting note-on/off with tick delta-times from the note/rest delays.
# Control-flow (LOOP/JUMP/CALL/branches) is NOT followed -- it is markered and the walk continues
# linearly to END (or a safety cap), so the output is a faithful single-pass transcription.
#
# Opcode tables: decomp/include/aseq.h. MIDI pitch = layer semitone + 21 (PITCH_A0=0 == MIDI A0=21,
# aseq.h:158). Note timing (seqplayer.c AudioSeq_SeqLayerProcessScriptStep3 L1028-1142): gateDelay =
# (gateTime * delay) >> 8. Tempo -> MIDI tempo meta assuming 48 PPQN and TEMPO value == BPM (derived
# approximation; N64 stores "seqTicks per minute", documented in the sidecar as such).
# ══════════════════════════════════════════════════════════════════════════════════════════════════

# aseq control-flow opcodes (aseq.h:5-19)
OP_RBLTZ, OP_RBEQZ, OP_RJUMP, OP_BGEZ, OP_BREAK, OP_LOOPEND, OP_LOOP = 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8
OP_BLTZ, OP_BEQZ, OP_JUMP, OP_CALL, OP_DELAY, OP_DELAY1, OP_END = 0xF9, 0xFA, 0xFB, 0xFC, 0xFD, 0xFE, 0xFF

# sSeqInstructionArgsTable (seqplayer.c:44-126), byte index = cmd - 0xB0. Value bit-packed abcUUUnn:
# nn = arg count, a/b/c = 1 if that arg is s16 else u8. Shared by player + channel scripts (cmd>=0xB0).
# Parsed straight from the decomp CMD_ARGS_* macros so it can never drift; verified fallback below.
_SEQ_ARGS_FALLBACK = [
    0x81, 0x00, 0x81, 0x01, 0x00, 0x00, 0x00, 0x81, 0x01, 0x01, 0x01, 0x42, 0x81, 0x81, 0x00, 0x00,
    0x00, 0x01, 0x81, 0x00, 0x00, 0x00, 0x01, 0x42, 0x01, 0x01, 0x01, 0x81, 0x01, 0x01, 0x81, 0x81,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x81, 0x01, 0x01, 0x01, 0x81, 0x01,
    0x01, 0x03, 0x03, 0x01, 0x00, 0x01, 0x01, 0x81, 0x03, 0x01, 0x00, 0x02, 0x00, 0x01, 0x01, 0x82,
    0x00, 0x01, 0x01, 0x01, 0x01, 0x81, 0x00, 0x00, 0x01, 0x81, 0x81, 0x81, 0x81, 0x00, 0x00, 0x00,
]
_SEQPLAYER_C = os.path.join(REPO, "decomp", "src", "audio", "disk", "lib", "seqplayer.c")


def _load_seq_args_table():
    try:
        with open(_SEQPLAYER_C, encoding="utf-8") as fh:
            txt = fh.read()
        m = re.search(r"u8 sSeqInstructionArgsTable\[\]\s*=\s*\{(.*?)\};", txt, re.S)
        vals = []
        for line in m.group(1).splitlines():
            mm = re.search(r"CMD_ARGS_(\d)\(([^)]*)\)", line.split("//")[0])
            if not mm:
                continue
            n = int(mm.group(1))
            args = [a.strip() for a in mm.group(2).split(",") if a.strip()]
            packed = n
            for i, a in enumerate(args):
                if a == "s16":
                    packed |= 1 << (7 - i)
            vals.append(packed)
        if len(vals) == 80:
            return vals
    except Exception:  # noqa: BLE001
        pass
    return list(_SEQ_ARGS_FALLBACK)


SEQ_ARGS_TABLE = _load_seq_args_table()


class _Reader:
    """Bounded PC reader over a sequence-data slice (all values big-endian)."""
    def __init__(self, data):
        self.d = data
        self.pc = 0

    def eof(self):
        return self.pc >= len(self.d)

    def u8(self):
        v = self.d[self.pc]
        self.pc += 1
        return v

    def s8(self):
        v = self.u8()
        return v - 256 if v >= 128 else v

    def s16(self):
        v = (self.d[self.pc] << 8) | self.d[self.pc + 1]
        self.pc += 2
        return v - 0x10000 if v >= 0x8000 else v

    def u16(self):
        v = (self.d[self.pc] << 8) | self.d[self.pc + 1]
        self.pc += 2
        return v

    def compressed_u16(self):
        # AudioSeq_ScriptReadCompressedU16: high-bit continuation.
        v = self.u8()
        if v & 0x80:
            v = ((v & 0x7F) << 8) | self.u8()
        return v


def _read_table_args(rdr, cmd):
    """Consume the sSeqInstructionArgsTable-declared args for a cmd in 0xB0-0xF1; return list."""
    packed = SEQ_ARGS_TABLE[cmd - 0xB0]
    n = packed & 3
    args = []
    hb = packed
    for _ in range(n):
        if hb & 0x80:
            args.append(rdr.s16())
        else:
            args.append(rdr.u8())
        hb = (hb << 1) & 0xFF
    return args


class _MidiTrack:
    """Accumulates (absolute_tick, event_bytes) then serialises to a MIDI track chunk."""
    def __init__(self):
        self.events = []  # (tick, order, bytes)
        self._seq = 0

    def add(self, tick, data):
        self.events.append((tick, self._seq, bytes(data)))
        self._seq += 1

    def note_on(self, tick, ch, key, vel):
        self.add(tick, [0x90 | (ch & 0xF), key & 0x7F, vel & 0x7F])

    def note_off(self, tick, ch, key):
        self.add(tick, [0x80 | (ch & 0xF), key & 0x7F, 0x40])

    def controller(self, tick, ch, cc, val):
        self.add(tick, [0xB0 | (ch & 0xF), cc & 0x7F, val & 0x7F])

    def program(self, tick, ch, prog):
        self.add(tick, [0xC0 | (ch & 0xF), prog & 0x7F])

    def meta_text(self, tick, meta_type, text):
        b = text.encode("ascii", "replace")
        self.add(tick, [0xFF, meta_type] + list(_vlq(len(b))) + list(b))

    def tempo(self, tick, us_per_quarter):
        self.add(tick, [0xFF, 0x51, 0x03,
                        (us_per_quarter >> 16) & 0xFF, (us_per_quarter >> 8) & 0xFF, us_per_quarter & 0xFF])

    def serialise(self):
        # Stable sort by (tick, insertion order) keeps delta-times monotonic.
        self.events.sort(key=lambda e: (e[0], e[1]))
        body = bytearray()
        last = 0
        running = None
        for tick, _o, data in self.events:
            body += _vlq(tick - last)
            last = tick
            # Meta events (0xFF) reset running status; channel events may use it but we emit full
            # status every time for a simple, unambiguous stream (the verifier checks this).
            body += data  # full status byte every event (no running-status elision, by design)
        # End of track
        body += _vlq(0) + bytes([0xFF, 0x2F, 0x00])
        return b"MTrk" + struct.pack(">I", len(body)) + bytes(body)


def _vlq(n):
    if n < 0:
        n = 0
    out = bytearray([n & 0x7F])
    n >>= 7
    while n > 0:
        out.insert(0, (n & 0x7F) | 0x80)
        n >>= 7
    return bytes(out)


class SequenceDumpClass(DumpClass):
    """See the section header above. Emits, under dump/music/:
        <label>.mid            -- format-1 MIDI, one track per active aseq channel (+ a tempo/marker
                                  track 0). Loops/jumps/calls -> CC111 + text markers (lossy).
        <label>.instrmap.json  -- per-sequence sidecar: channel -> program/soundfont-instrument.
        <label>.disasm.txt     -- linear disassembly of every script walked (audit trail).
        manifest.tsv           -- one row per sequence.
    Sources: CART audio_seq blob (2 seqs) + EK ek/aAudioSeqDD* (23). seqId->fontId is 1:1 by enum
    name (gSequenceFontTable, disk config), so sequence i binds soundfont i."""
    name = "midi"
    subdir = "music"
    PPQN = 48

    # -- layer disassembly: produce note events on a track --
    def _walk_layer(self, seq, start_pc, track, midi_ch, large_notes, transpose, disasm, cap=8000):
        rdr = _Reader(seq)
        rdr.pc = start_pc
        tick = 0
        last_delay = 0
        short_default_delay = 0
        gate_time = 0
        note_count = 0
        steps = 0
        active = []  # (off_tick, key)

        def flush_offs(upto):
            for off_tick, key in [a for a in active if a[0] <= upto]:
                track.note_off(off_tick, midi_ch, key)
            active[:] = [a for a in active if a[0] > upto]

        while not rdr.eof() and steps < cap:
            steps += 1
            pc0 = rdr.pc
            cmd = rdr.u8()
            if cmd < 0xC0:
                # Note (or C0 = rest). Pitch = cmd & 0x3F; type = cmd & 0xC0.
                note_type = cmd & 0xC0
                velocity = 100
                if large_notes:
                    if note_type == 0x00:      # NOTEDVG
                        delay = rdr.compressed_u16(); velocity = rdr.u8(); gate_time = rdr.u8(); last_delay = delay
                    elif note_type == 0x40:    # NOTEDV
                        delay = rdr.compressed_u16(); velocity = rdr.u8(); gate_time = 0; last_delay = delay
                    else:                      # NOTEVG
                        delay = last_delay; velocity = rdr.u8(); gate_time = rdr.u8()
                else:
                    if note_type == 0x00:      # NOTEDVG
                        delay = rdr.compressed_u16(); last_delay = delay
                    elif note_type == 0x40:    # NOTEDV
                        delay = short_default_delay
                    else:                      # NOTEVG
                        delay = last_delay
                velocity = max(1, min(127, velocity))
                key = (cmd & 0x3F) + 21 + transpose
                gate_delay = (gate_time * delay) >> 8
                flush_offs(tick)
                if 0 <= key <= 127:
                    track.note_on(tick, midi_ch, key, velocity)
                    off_at = tick + (gate_delay if gate_delay > 0 else max(1, delay - 1))
                    active.append((off_at, key))
                    note_count += 1
                    disasm.append("    +%-6d NOTE key=%d vel=%d delay=%d gate=%d" %
                                  (tick, key, velocity, delay, gate_time))
                tick += delay
                continue
            if cmd == 0xC0:  # LDELAY (rest)
                delay = rdr.compressed_u16()
                disasm.append("    +%-6d REST delay=%d" % (tick, delay))
                tick += delay
                continue
            # Layer control commands (aseq.h:139-156). Arg lengths from Step2 body.
            if cmd >= OP_RBLTZ:  # 0xF2..0xFF control flow (shared)
                consumed = self._flow_marker(rdr, cmd, track, midi_ch, tick, disasm, "layer")
                if consumed == "end":
                    break
                continue
            if cmd in (0xC1, 0xC2, 0xC6, 0xC9, 0xCA, 0xCD, 0xCE, 0xCF):
                rdr.u8()
            elif cmd == 0xC3:  # SHORTDELAY (compressed u16)
                short_default_delay = rdr.compressed_u16()
            elif cmd in (0xC4, 0xC5, 0xC8, 0xCC):
                pass  # LEGATO / NOLEGATO / NOPORTAMENTO / NODRUMPAN (0 args)
            elif cmd == 0xC7:  # PORTAMENTO: mode(u8), note(u8), then u8 if special else compressed u16
                mode = rdr.u8(); rdr.u8()
                if mode & 0x80:
                    rdr.u8()
                else:
                    rdr.compressed_u16()
            elif cmd == 0xCB:  # ENV: s16 envelope ptr + u8 release rate (fallthrough)
                rdr.s16(); rdr.u8()
            elif 0xD0 <= cmd <= 0xEF:
                pass  # LDSHORTVEL / LDSHORTGATE (low nibble is the arg; no extra bytes)
            else:
                disasm.append("    +%-6d LAYER_OP 0x%02X (unmodelled -> marker)" % (tick, cmd))
                track.meta_text(tick, 0x06, "layer_op_%02X" % cmd)
            _ = pc0
        flush_offs(tick + 1 << 28)  # close any still-open notes at the end
        for off_tick, key in active:
            track.note_off(max(off_tick, tick), midi_ch, key)
        return note_count

    def _flow_marker(self, rdr, cmd, track, midi_ch, tick, disasm, level):
        """Consume a control-flow op's bytes and drop a MIDI marker (loops -> CC111). Returns 'end'
        for END, else None. Control flow is NOT taken (linear transcription)."""
        if cmd == OP_END:
            disasm.append("    +%-6d END" % tick)
            return "end"
        if cmd == OP_DELAY:
            d = rdr.compressed_u16()
            disasm.append("    +%-6d DELAY %d" % (tick, d))
            return None
        if cmd == OP_DELAY1:
            return None
        if cmd in (OP_LOOP, OP_RBLTZ, OP_RBEQZ, OP_RJUMP):
            rdr.u8()
        elif cmd in (OP_JUMP, OP_CALL, OP_BEQZ, OP_BLTZ, OP_BGEZ):
            rdr.s16()
        # BREAK / LOOPEND: 0 args
        if cmd in (OP_LOOP, OP_LOOPEND):
            track.controller(tick, midi_ch, 111, 0)  # CC111 loop marker (widely-used convention)
            disasm.append("    +%-6d LOOP/LOOPEND -> CC111 marker" % tick)
        else:
            track.meta_text(tick, 0x06, "%s_flow_%02X" % (level, cmd))
            disasm.append("    +%-6d FLOW 0x%02X -> marker (not taken)" % (tick, cmd))
        return None

    # -- channel disassembly: gather instrument/pan/vol/layers --
    def _walk_channel(self, seq, start_pc, disasm, cap=8000):
        rdr = _Reader(seq)
        rdr.pc = start_pc
        large_notes = False
        instrument = 0
        pan = 64
        vol = 100
        transpose = 0
        font = None
        layers = []  # (layer_index, layer_pc, large_notes, instrument, transpose)
        steps = 0
        disasm.append("  channel @0x%X" % start_pc)
        while not rdr.eof() and steps < cap:
            steps += 1
            cmd = rdr.u8()
            if cmd >= 0xB0:
                if cmd >= OP_RBLTZ:
                    if cmd == OP_END:
                        disasm.append("    END"); break
                    if cmd == OP_DELAY:
                        rdr.compressed_u16(); continue
                    if cmd == OP_DELAY1:
                        continue
                    if cmd in (OP_LOOP, OP_RBLTZ, OP_RBEQZ, OP_RJUMP):
                        rdr.u8()
                    elif cmd in (OP_JUMP, OP_CALL, OP_BEQZ, OP_BLTZ, OP_BGEZ):
                        rdr.s16()
                    continue
                args = _read_table_args(rdr, cmd)
                if cmd == 0xC1:      # INSTR
                    instrument = args[0]
                elif cmd == 0xC3:    # SHORT -> largeNotes false
                    large_notes = False
                elif cmd == 0xC4:    # NOSHORT -> largeNotes true
                    large_notes = True
                elif cmd == 0xC6:    # FONT
                    font = args[0]
                elif cmd == 0xDB:    # TRANSPOSE
                    transpose = args[0] if args[0] < 128 else args[0] - 256
                elif cmd == 0xDD:    # PAN
                    pan = args[0]
                elif cmd == 0xDF:    # VOL
                    vol = args[0]
                continue
            if cmd >= 0x70:  # layer/testlayer/stio group
                grp = cmd & 0xF8
                low = cmd & 0x7
                if grp == 0x88:      # LDLAYER
                    ptr = rdr.u16()
                    layers.append((low, ptr, large_notes, instrument, transpose))
                    disasm.append("    LDLAYER %d -> 0x%X (large=%s instr=%d)"
                                  % (low, ptr, large_notes, instrument))
                elif grp == 0x78:    # RLDLAYER (relative s16)
                    rdr.s16()
                # TESTLAYER/DELLAYER/DYNLDLAYER/STIO: 0 extra bytes
                continue
            grp = cmd & 0xF0
            if grp == 0x00:          # CDELAY (low nibble delay); ends the tick in-engine
                continue
            if grp == 0x20:          # LDCHAN (+s16)
                rdr.s16()
            elif grp in (0x30, 0x40):  # STCIO / LDCIO (+u8)
                rdr.u8()
            # 0x10 LDSAMPLE, 0x50 SUBIO, 0x60 LDIO: 0 extra
        return {"largeNotes": large_notes, "instrument": instrument, "pan": pan, "vol": vol,
                "transpose": transpose, "font": font, "layers": layers}

    # Player-level op arg-byte counts. Unlike channel/layer scripts, the player dispatcher
    # (AudioSeq_SequencePlayerProcessSequence L1850-2093) uses an explicit switch with its own reads,
    # NOT sSeqInstructionArgsTable. Byte counts transcribed from that switch.
    _PLAYER_C0_ARGS = {
        0xC4: 2, 0xC5: 2, 0xC6: 0, 0xC7: 3, 0xC8: 1, 0xC9: 1, 0xCC: 1, 0xCD: 2, 0xCE: 1,
        0xD0: 1, 0xD1: 2, 0xD2: 2, 0xD3: 1, 0xD4: 0, 0xD5: 1, 0xD6: 2, 0xD7: 2, 0xD9: 1,
        0xDA: 3, 0xDB: 1, 0xDC: 1, 0xDD: 1, 0xDE: 1, 0xDF: 1, 0xEF: 3, 0xF0: 0, 0xF1: 1,
    }
    _PLAYER_LOW_ARGS = {0x00: 0, 0x40: 0, 0x50: 0, 0x60: 2, 0x70: 0, 0x80: 0,
                        0x90: 2, 0xA0: 2, 0xB0: 3}  # 0xB0 = LDSEQ (u8+s16)

    # -- player disassembly: tempo + channel starts --
    def _walk_player(self, seq, disasm, cap=4000):
        rdr = _Reader(seq)
        tempo = 120
        channels = {}  # ch_index -> pc
        steps = 0
        disasm.append("player @0x0")
        while not rdr.eof() and steps < cap:
            steps += 1
            cmd = rdr.u8()
            if cmd >= OP_RBLTZ:  # 0xF2+ control flow
                if cmd == OP_END:
                    disasm.append("  END"); break
                if cmd == OP_DELAY:
                    rdr.compressed_u16(); continue
                if cmd == OP_DELAY1:
                    continue
                if cmd in (OP_LOOP, OP_RBLTZ, OP_RBEQZ, OP_RJUMP):
                    rdr.u8()
                elif cmd in (OP_JUMP, OP_CALL, OP_BEQZ, OP_BLTZ, OP_BGEZ):
                    rdr.s16()
                continue
            if cmd >= 0xC0:
                n = self._PLAYER_C0_ARGS.get(cmd, 0)
                if cmd == 0xDD:  # TEMPO: u8 BPM
                    tempo = rdr.u8() if rdr.pc < len(seq) else tempo
                    tempo = tempo if tempo > 0 else 120
                    disasm.append("  TEMPO %d" % tempo)
                else:
                    for _ in range(n):
                        rdr.u8()
                if cmd == 0xC6:  # STOP
                    disasm.append("  STOP"); break
                continue
            grp = cmd & 0xF0
            low = cmd & 0xF
            if grp == 0x90:  # LDCHAN (+s16 ptr)
                ptr = rdr.u16()
                channels[low] = ptr
                disasm.append("  LDCHAN %d -> 0x%X" % (low, ptr))
            else:
                for _ in range(self._PLAYER_LOW_ARGS.get(grp, 0)):
                    rdr.u8()
        return tempo, channels

    def _convert_sequence(self, label, seq, font_index):
        disasm = []
        tempo, channels = self._walk_player(seq, disasm)
        tracks = []
        instr_map = {}

        meta = _MidiTrack()
        meta.meta_text(0, 0x03, label)
        us_per_q = int(round(60000000.0 / max(tempo, 1)))
        meta.tempo(0, us_per_q)
        tracks.append(meta)

        total_notes = 0
        for ch_index in sorted(channels):
            ch = self._walk_channel(seq, channels[ch_index], disasm)
            trk = _MidiTrack()
            midi_ch = ch_index & 0xF
            trk.meta_text(0, 0x03, "%s_ch%d" % (label, ch_index))
            trk.program(0, midi_ch, ch["instrument"] & 0x7F)
            trk.controller(0, midi_ch, 7, min(127, ch["vol"]))
            trk.controller(0, midi_ch, 10, min(127, ch["pan"]))
            ch_notes = 0
            for (lidx, lpc, large, instr, transp) in ch["layers"]:
                ch_notes += self._walk_layer(seq, lpc, trk, midi_ch, large, transp, disasm)
            total_notes += ch_notes
            tracks.append(trk)
            instr_map["channel_%d" % ch_index] = {
                "midiChannel": midi_ch, "program": ch["instrument"],
                "soundfontIndex": font_index,
                "soundfontInstrument": "font%s_inst%d" % (
                    font_index if font_index is not None else "?", ch["instrument"]),
                "pan": ch["pan"], "volume": ch["vol"], "transpose": ch["transpose"],
                "largeNotes": ch["largeNotes"], "layerCount": len(ch["layers"]),
                "notes": ch_notes,
            }
        return tracks, instr_map, disasm, tempo, total_notes

    def _write_midi(self, path, tracks):
        header = b"MThd" + struct.pack(">IHHH", 6, 1, len(tracks), self.PPQN)
        with open(path, "wb") as fh:
            fh.write(header)
            for t in tracks:
                fh.write(t.serialise())

    def _verify_midi(self, path):
        """Independent minimal re-parser: checks header, per-track delta VLQ decodes, running status
        resolves, delta-times are non-negative (monotonic absolute time), and end-of-track present."""
        with open(path, "rb") as fh:
            data = fh.read()
        if data[:4] != b"MThd":
            return False, "no MThd"
        (_ln, fmt, ntrk, _div) = struct.unpack_from(">IHHH", data, 4)
        off = 14
        for ti in range(ntrk):
            if data[off:off + 4] != b"MTrk":
                return False, "track %d no MTrk" % ti
            tlen = struct.unpack_from(">I", data, off + 4)[0]
            p = off + 8
            end = p + tlen
            running = None
            saw_eot = False
            while p < end:
                # delta VLQ
                dt = 0
                while True:
                    b = data[p]; p += 1
                    dt = (dt << 7) | (b & 0x7F)
                    if not (b & 0x80):
                        break
                if dt < 0:
                    return False, "negative delta"
                status = data[p]
                if status < 0x80:
                    if running is None:
                        return False, "running status with no prior status"
                    status = running
                else:
                    p += 1
                    if status < 0xF0:
                        running = status
                if status == 0xFF:
                    mtype = data[p]; p += 1
                    mlen = 0
                    while True:
                        b = data[p]; p += 1
                        mlen = (mlen << 7) | (b & 0x7F)
                        if not (b & 0x80):
                            break
                    if mtype == 0x2F:
                        saw_eot = True
                    p += mlen
                elif status in (0xF0, 0xF7):
                    mlen = 0
                    while True:
                        b = data[p]; p += 1
                        mlen = (mlen << 7) | (b & 0x7F)
                        if not (b & 0x80):
                            break
                    p += mlen
                else:
                    hi = status & 0xF0
                    p += 1 if hi in (0xC0, 0xD0) else 2
            if not saw_eot:
                return False, "track %d no end-of-track" % ti
            off = end
        return True, "ok (%d tracks)" % ntrk

    def verify(self, ctx):
        """Re-parse every emitted .mid with the independent minimal reader (track/delta/running-status
        integrity + end-of-track present)."""
        out_dir = self.out_dir(ctx)
        overlap = matched = diverged = missing = 0
        divergences = []
        for path in sorted(glob.glob(os.path.join(out_dir, "*.mid"))):
            overlap += 1
            ok, msg = self._verify_midi(path)
            if ok:
                matched += 1
            else:
                diverged += 1
                divergences.append("%s: %s" % (os.path.basename(path), msg))
        return {"overlap": overlap, "matched": matched, "diverged": diverged,
                "errored": 0, "missing": missing, "divergences": divergences}

    def _collect_sequences(self, ctx, ek_zip, ek_lens):
        """Return [(label, seq_bytes, font_index, origin, declared_size)]."""
        seqs = []
        tables = _load_audio_tables()
        evalr = tables["evalr"]

        seq_blob = _blob_payload(ctx.source.zip, "audio_blob/audio_seq")
        rom_seq = tables["rom_seq"]
        if seq_blob is not None and rom_seq is not None:
            for idx, e in enumerate(rom_seq["entries"]):
                off = evalr(e["offset"]); size = evalr(e["size"])
                seqs.append(("cart_%s" % e["name"], seq_blob[off:off + size], idx, "cart", size))

        disk_seq = tables["disk_seq"]
        if ek_zip is not None and disk_seq is not None:
            for idx, symbol in enumerate(EK_SEQ_SYMBOLS):
                if idx >= len(disk_seq["entries"]):
                    break
                e = disk_seq["entries"][idx]
                declared = evalr(e["size"])
                payload = _ek_payload(ek_zip, symbol, ek_lens.get(symbol, declared))
                if payload is None:
                    continue
                seqs.append(("ek_%s" % e["name"], payload, idx, "ek", declared))
        return seqs

    def run(self, ctx):
        ek_zip, _ek_path = _open_ek_archive()
        ek_lens = _load_ek_manifest_lens()
        seqs = self._collect_sequences(ctx, ek_zip, ek_lens)
        if not seqs:
            print("  midi: no sequences found (audio_seq blob + EK archive both absent) -- skipping")
            return {"class": self.name, "dumped": 0, "skipped": 0, "failed": 1, "total": 0}

        out_dir = self.out_dir(ctx)
        os.makedirs(out_dir, exist_ok=True)
        manifest_rows = []
        dumped = skipped = failed = 0

        for label, seq, font_index, origin, declared in seqs:
            mid_path = os.path.join(out_dir, label + ".mid")
            map_path = os.path.join(out_dir, label + ".instrmap.json")
            dis_path = os.path.join(out_dir, label + ".disasm.txt")
            size_ok = (len(seq) == declared)

            if os.path.exists(mid_path) and os.path.exists(map_path):
                ok, msg = self._verify_midi(mid_path)
                # Re-derive the note count (no writes) so the manifest is complete on reruns too.
                try:
                    _t, _im, _d, _tp, notes = self._convert_sequence(label, seq, font_index)
                except Exception:  # noqa: BLE001
                    notes = "?"
                manifest_rows.append((label, origin, len(seq), font_index, notes, "existing",
                                      "verify:" + ("PASS" if ok else "FAIL:" + msg)))
                skipped += 1
                continue
            try:
                tracks, instr_map, disasm, tempo, notes = self._convert_sequence(label, seq, font_index)
                self._write_midi(mid_path, tracks)
            except Exception as exc:  # noqa: BLE001  -- a malformed slice must not abort the class
                sys.stderr.write("  warn: midi %s: %s\n" % (label, exc))
                manifest_rows.append((label, origin, len(seq), font_index, 0, "ERROR", str(exc)[:60]))
                failed += 1
                continue

            with open(map_path, "w", encoding="utf-8") as fh:
                json.dump({
                    "sequence": label, "origin": origin, "soundfontIndex": font_index,
                    "tempoSeqTicksPerMinute": tempo, "ppqn": self.PPQN,
                    "sizeMatchesTable": size_ok, "channels": instr_map,
                    "note": "channel program = soundfont instrument index (font bound via "
                            "gSequenceFontTable, 1:1 seq->font by enum name); loops/jumps are "
                            "CC111/text markers (lossy linear transcription).",
                }, fh, indent=2)
            with open(dis_path, "w", encoding="utf-8", newline="\n") as fh:
                fh.write("# linear aseq disassembly of %s (%d bytes)\n" % (label, len(seq)))
                fh.write("\n".join(disasm) + "\n")

            ok, msg = self._verify_midi(mid_path)
            if not ok:
                failed += 1
            else:
                dumped += 1
            manifest_rows.append((label, origin, len(seq), font_index, notes,
                                  "ok" if size_ok else "ok-SIZE-MISMATCH",
                                  "verify:" + ("PASS" if ok else "FAIL:" + msg)))

        _write_manifest(os.path.join(out_dir, "manifest.tsv"),
                        "sequence\torigin\tbytes\tsoundfontIndex\tnotes\tstatus\tselfVerify   "
                        "(aseq -> MIDI, linear transcription; loops/effects = markers, lossy)",
                        manifest_rows)
        print("  midi: %d MIDI written, %d skipped, %d failed (of %d sequences; all self-verified)"
              % (dumped, skipped, failed, len(seqs)))
        return {"class": self.name, "dumped": dumped, "skipped": skipped, "failed": failed,
                "total": len(seqs)}


EXTRA_AUDIO_CLASSES = {
    SampleDumpClass.name: SampleDumpClass,
    SequenceDumpClass.name: SequenceDumpClass,
}
