"""Replay resolution + staging (autoinput .txt and the oracle track's .gdxrec).

Autoinput grammar is fixed by port/input_bridge.c:208-236 and is validated here so a
typo fails at stage time instead of producing a run that silently did nothing:

    ticks                      <- literal first non-blank line (tick mode; required for
                                  determinism — seconds mode is wall-clock)
    <tick> <INPUT> [holdTicks] <- holdTicks defaults to 15 (~0.25 s at 60 Hz)
    <tick> SHOT <label>        <- one-shot framebuffer dump -> autotest/<label>.bmp
    # comment / blank          <- ignored
  max 64 events, tick-ascending.

.gdxrec is the oracle track's tick-exact recorder (03-oracle-plan.md §2). We only stage
it — the format is theirs. `[debug] input_replay=<name>` wins over autoinput.
"""

import os
import re
import shutil

from . import inifile, util
from .util import GdxError

INPUT_TOKENS = {"START", "A", "B", "UP", "DOWN", "LEFT", "RIGHT",
                "STICK_UP", "STICK_DOWN", "STICK_LEFT", "STICK_RIGHT"}
MAX_EVENTS = 64

# port/input_bridge.c used to read the script with `char line[64]`, so a line of 64 bytes
# or more was SPLIT across fgets calls. The tail of a split line does not start with '#',
# so an over-long COMMENT silently became the first "data" line — and because it is not
# the word `ticks`, the whole script fell back to the wall-clock seconds timebase and
# every SHOT was lost. Measured 2026-09-07: a 91-char comment header produced
# "[input] autoinput script loaded: 1 events (seconds timebase)" and zero BMPs.
#
# The port side is fixed (bigger buffer + the over-long remainder is drained, so one
# physical line is one logical line at any length). This check stays because a replay is
# also staged onto SD cards and onto builds that predate the fix, and 62 bytes is the
# limit that is safe everywhere.
MAX_LINE_BYTES = 62   # 63 usable bytes in the old buffer minus the newline


def replays_dir():
    return os.path.join(util.tools_dev(), "replays")


def replay_dirs():
    """Everywhere a replay id may live, in search order.

    The oracle track keeps its `.gdxrec` recordings in `<repo>/replays/` while the
    autoinput scripts live in `tools/dev/replays/`, so `--replay <id>` has to look in
    both or half the corpus is unreachable by name.
    """
    return [replays_dir(), os.path.join(util.repo_root(), "replays")]


def resolve(ident):
    """id or path -> (path, kind). kind is 'autoinput' or 'gdxrec'."""
    cands = [ident]
    for d in replay_dirs():
        cands += [os.path.join(d, ident),
                  os.path.join(d, ident + ".gdxrec"),
                  os.path.join(d, ident + ".txt")]
    for c in cands:
        if os.path.isfile(c):
            return os.path.abspath(c), ("gdxrec" if c.endswith(".gdxrec") else "autoinput")
    avail = []
    for d in replay_dirs():
        if os.path.isdir(d):
            avail += [n for n in sorted(os.listdir(d))
                      if n.endswith((".txt", ".gdxrec"))]
    raise GdxError("no replay %r (looked in %s; have: %s)"
                   % (ident, " and ".join(replay_dirs()), ", ".join(avail) or "none"),
                   util.EXIT_STAGING)


def validate_autoinput(path):
    """-> event count. Raises GdxError(EXIT_STAGING) with the offending line."""
    text = util.read_text(path)
    for i, raw in enumerate(text.splitlines()):
        n = len(raw.encode("utf-8"))
        if n > MAX_LINE_BYTES:
            raise GdxError(
                "%s:%d: line is %d bytes. Builds predating the input_bridge.c line-buffer "
                "fix split any line of 64+ bytes — INCLUDING a comment — parse its tail as "
                "data, fail the `ticks` sniff, and silently fall back to the seconds "
                "timebase with every SHOT lost. Keep every line <= %d bytes so the script "
                "is portable to older builds and SD cards."
                % (path, i + 1, n, MAX_LINE_BYTES),
                util.EXIT_STAGING)
    lines = [(i + 1, l.strip()) for i, l in enumerate(text.splitlines())]
    body = [(n, l) for n, l in lines if l and not l.startswith("#")]
    if not body:
        raise GdxError("%s: no events" % path, util.EXIT_STAGING)
    n0, first = body[0]
    if first.split()[0].lower() != "ticks":
        raise GdxError(
            "%s:%d: first line must be `ticks` — seconds mode is wall-clock and not "
            "deterministic (port/input_bridge.c:220)" % (path, n0), util.EXIT_STAGING)
    events = 0
    last_tick = -1
    for n, l in body[1:]:
        parts = l.split()
        if len(parts) < 2:
            raise GdxError("%s:%d: expected `<tick> <INPUT> [hold]` or `<tick> SHOT <label>`, got %r"
                           % (path, n, l), util.EXIT_STAGING)
        try:
            tick = int(parts[0])
        except ValueError:
            raise GdxError("%s:%d: tick %r is not an integer" % (path, n, parts[0]),
                           util.EXIT_STAGING)
        if tick < last_tick:
            raise GdxError("%s:%d: ticks must be ascending (%d after %d)"
                           % (path, n, tick, last_tick), util.EXIT_STAGING)
        last_tick = tick
        tok = parts[1].upper()
        if tok == "SHOT":
            if len(parts) < 3:
                raise GdxError("%s:%d: SHOT needs a label" % (path, n), util.EXIT_STAGING)
            if len(parts[2]) > 23:
                raise GdxError("%s:%d: SHOT label >23 chars (port/input_bridge.c shot[24])"
                               % (path, n), util.EXIT_STAGING)
        elif tok not in INPUT_TOKENS:
            raise GdxError("%s:%d: unknown input %r (known: %s)"
                           % (path, n, parts[1], " ".join(sorted(INPUT_TOKENS))),
                           util.EXIT_STAGING)
        elif len(parts) >= 3:
            try:
                int(parts[2])
            except ValueError:
                raise GdxError("%s:%d: holdTicks %r is not an integer" % (path, n, parts[2]),
                               util.EXIT_STAGING)
        events += 1
    if events > MAX_EVENTS:
        raise GdxError("%s: %d events, the parser reads at most %d (port/input_bridge.c)"
                       % (path, events, MAX_EVENTS), util.EXIT_STAGING)
    return events


def autoinput_path(sdmc_root=None):
    """Where the guest looks for the script: the SD ROOT, next to the exe's cwd.

    `sdmc_root` selects the lane: the shared Azahar Qt SD by default, or a RetroArch
    instance's private SD (retro.Instance.sdmc()).
    """
    return os.path.join(sdmc_root or util.AZAHAR_SDMC, "gdx-autoinput.txt")


def rec_dir(sdmc_root=None):
    """Where `.gdxrec` files live: **<sdmc root>/3ds/gdiffuser/rec/**.

    port/3ds/main_3ds.cpp builds the path as "%srec/%s.gdxrec" with
    gdx3ds_fs_base_path() == "sdmc:/3ds/gdiffuser/" (absolute since the residuals track;
    it used to be the cwd-relative "rec/<name>.gdxrec", which made staging depend on the
    launcher's cwd and turned `--replay x.gdxrec` into a silent no-op). Pinned by
    test_rec_path_matches_the_guest_source.
    """
    return os.path.join(sdmc_root or util.AZAHAR_SDMC, "3ds", "gdiffuser", "rec")


# ------------------------------------------------------------ implied settings --
# NEXT.md B19: `--replay` alone never ENDS a run. The guest reaches the replay's last tick
# and then keeps playing — unscripted, unrecorded — until `--seconds` expires. On the
# canonical replay that is ~150 s of garbage appended to every run, and it is also what
# made `--seconds 60` look like a crash (B45): the run can only ever be cut short.
# `[debug] replay_quit=1` leaves the frame loop at `[rec] end` (main_3ds.cpp:1051), which
# is the natural end of a staged replay, so staging one implies it.
#
# It is IMPLIED, not forced. Some diagnostic runs deliberately play past the replay's end:
# the wobble investigation watches post-replay drift and the dynlod one watches LOD
# settling after the scripted inputs stop. Both need the guest to keep running, and both
# get it with `--no-replay-quit` (or the equivalent `--ini debug.replay_quit=0`), which
# this never overrides.
REPLAY_QUIT_KEY = ("debug", "replay_quit")
# replay_quit only acts on a .gdxrec's `[rec] end` (main_3ds.cpp oracleReplayEnded). An
# autoinput .txt never logs one, so on its own the implied replay_quit=1 did nothing for
# storm2/menu/boot: the guest played on to --seconds, the harness killed RetroArch, and
# every exit-time flush (pcm_dump, ...) was lost. For autoinput the same intent is spelled
# `[debug] quit_at_tick` = the script's last tick + AUTOINPUT_QUIT_MARGIN (the margin lets a
# SHOT scheduled on the last tick land). It is only implied on a PRIVATE SD (the retro
# lane): the Qt lane merges into the user's shared ini, where a leftover quit_at_tick cuts
# every later run short (that is how `quit_at_tick = 3400` got there, retro.py).
QUIT_AT_TICK_KEY = ("debug", "quit_at_tick")
AUTOINPUT_QUIT_MARGIN = 120


def implied_settings(kind=None):
    """Ini keys a staged replay implies but does not force. -> {(section, key): value}.

    The caller must apply these with setdefault semantics against anything the user typed,
    so `--ini debug.replay_quit=0` and `--no-replay-quit` still win.
    """
    return {REPLAY_QUIT_KEY: "1"}


def apply_implied(settings, explicit, kind=None, out=None, last=None, private_sd=False):
    """Merge implied_settings() into `settings`, never over a key in `explicit`.

    `explicit` is the set of (section, key) the invocation named itself. `last` is the
    replay's last scheduled tick; with `private_sd` (an instance SD nobody else reads) an
    autoinput replay also gets the quit_at_tick that makes replay_quit mean something.
    """
    for key, value in implied_settings(kind).items():
        if key in explicit:
            if out and key == REPLAY_QUIT_KEY and not inifile.truthy(settings.get(key, "0")):
                out.say("[%s] %s=%s as asked: this run plays PAST the replay's last tick "
                        "and ends on --seconds (wobble/dynlod-style diagnostics)"
                        % (key[0], key[1], settings.get(key)))
            continue
        settings[key] = value
        if out and key == REPLAY_QUIT_KEY:
            out.say("[debug] replay_quit=1 implied by --replay: the run ends at the "
                    "replay's last tick ('[rec] end'), not on --seconds. "
                    "Pass --no-replay-quit to play past it.")
    _imply_autoinput_quit(settings, explicit, kind, out, last, private_sd)
    return settings


def _imply_autoinput_quit(settings, explicit, kind, out, last, private_sd):
    if kind != "autoinput" or last is None or QUIT_AT_TICK_KEY in explicit:
        return
    if not inifile.truthy(settings.get(REPLAY_QUIT_KEY, "0")):
        return
    if not private_sd:
        if out:
            out.warn("autoinput replays never log '[rec] end', so replay_quit cannot end "
                     "this run; pass --ini debug.quit_at_tick=%d to stop at the script's "
                     "end (not implied on the shared SD: it would outlive this run)"
                     % (last + AUTOINPUT_QUIT_MARGIN))
        return
    settings[QUIT_AT_TICK_KEY] = str(last + AUTOINPUT_QUIT_MARGIN)
    if out:
        out.say("[debug] quit_at_tick=%d implied by the autoinput --replay (last tick %d + "
                "%d): an autoinput script never logs '[rec] end', so this is what ends it"
                % (last + AUTOINPUT_QUIT_MARGIN, last, AUTOINPUT_QUIT_MARGIN))


def stage(ident, sdmc_root=None, gdx_dir=None, dry_run=False, out=None):
    # gdx_dir is accepted for call-site symmetry with the rest of the SD helpers; both
    # replay kinds live at the SD ROOT (gdx-autoinput.txt and rec/), not under
    # 3ds/gdiffuser/, so it is deliberately unused here.
    """Stage a replay onto a virtual SD. -> manifest `replay` block + ini settings.

    Lane-agnostic on purpose: `emu run` passes nothing and gets the shared Azahar SD;
    `retro run` passes its instance's private SD. Both lanes therefore stage by exactly
    the same rules, which is what lets a brief say `--replay storm2` without naming a lane.
    """
    path, kind = resolve(ident)
    ini_settings = {}
    if kind == "autoinput":
        events = validate_autoinput(path)
        dest = autoinput_path(sdmc_root)
        if not dry_run:
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            shutil.copy2(path, dest + ".part")
            os.replace(dest + ".part", dest)
        # Disarm any .gdxrec another agent left armed: input_replay WINS over autoinput
        # (main_3ds.cpp:772 treats a non-empty string as on), so staging a script without
        # clearing it means the script never runs. The shared Qt SD really does carry a
        # stale `input_replay` between agents.
        ini_settings[("debug", "input_replay")] = ""
        if out:
            out.say("replay: %s (%s, %d events) -> %s" % (ident, kind, events, dest))
    else:
        name = os.path.splitext(os.path.basename(path))[0]
        destdir = rec_dir(sdmc_root)
        dest = os.path.join(destdir, name + ".gdxrec")
        events = sum(1 for l in util.read_text(path).splitlines()
                     if l.strip() and not l.strip().startswith("#"))
        if not dry_run:
            os.makedirs(destdir, exist_ok=True)
            shutil.copy2(path, dest + ".part")
            os.replace(dest + ".part", dest)
        # The guest opens the ABSOLUTE sdmc:/3ds/gdiffuser/rec/<name>.gdxrec (residuals track),
        # so one copy under rec_dir() serves every lane and launcher cwd.
        # 03-oracle-plan.md:128 — input_replay wins over autoinput.
        ini_settings[("debug", "input_replay")] = name
        # A SHOT in a .gdxrec routes through the oracle capture path, which is gated on
        # [debug] fh: with fh=0 the guest logs "capture request ignored" and captures
        # nothing, so the completion check would fail a perfectly good run. Arm it here
        # (fh_every stays off, so this only captures on SHOT), as oracle-run.sh does.
        if shot_labels(path, kind):
            ini_settings[("debug", "fh")] = "1"
        if out:
            out.say("replay: %s (gdxrec, %d lines) -> %s, [debug] input_replay=%s"
                    % (ident, events, dest, name))
            if os.path.exists(autoinput_path(sdmc_root)):
                out.warn("a gdx-autoinput.txt is present on this SD; "
                         "[debug] input_replay overrides it (03-oracle-plan.md:128)")
    return ({"id": ident, "sha256": util.sha256_file(path), "kind": kind,
             "events": events, "path": path,
             "shots": shot_labels(path, kind), "last_tick": last_tick(path, kind),
             "schedule": shot_schedule(path, kind)},
            ini_settings)


# ---------------------------------------------------------------- completion --
# Receipts that prove the replay RAN TO THE END, not merely that it started. A guest core
# that dies mid-replay still logs an advancing heartbeat and zero [fatal] lines, so
# without these a crash at frame 433 of a 6000-frame replay reported
# "PASS heartbeat reached frame 432, 0 [fatal] lines" (integration-verify, 2026-09-17).
END_RECEIPT = "[rec] end"                       # gdx_input_rec.c:241
QUIT_RECEIPT = "[oracle] quit requested"        # gdx3ds_oracle.c:269, incl. replay_quit
# [debug] quit_at_tick fires its own quit (gdx3ds_oracle.c:221). The receipt only exists
# BECAUSE the tick was reached, so its presence is the proof, and the run is complete even
# though the replay was deliberately cut short and never logs "[rec] end".
QUIT_AT_TICK_RECEIPT = "[debug] quit_at_tick reached"
# The configured tick is echoed once, in the "[fh] init ... quit_at_tick=N" receipt.
_QUIT_TICK_RE = re.compile(r"\[fh\] init\b[^\n]*\bquit_at_tick=(\d+)")
# [fh] lines carry tag=<SHOT label>; periodic fh_every captures use the literal "auto"
# (gdx3ds_oracle.c:198), so they must not be counted as SHOTs.
_FH_TAG_RE = re.compile(r"\[fh\][^\n]*\btag=(\S+)")
_AUTOTEST_RE = re.compile(r"\[autotest\] SHOT captured -> autotest/([^_\s.]+)")
AUTO_TAG = "auto"
# The SCHEDULE tag, logged by the replay layer before the dump is requested
# (gdx_input_rec.c). It survives [debug] shots=0, which the two capture receipts
# above do not: that lever returns from gdx_request_frame_dump before either is
# emitted (port/n64_gfx_bridge.cpp:9913). See SHOTS_OFF_RECEIPT.
_REC_SHOT_RE = re.compile(r"\[rec\] SHOT '([^']+)' at tick \d+")
# The guest's own one-shot receipt that the BMP writes are off for this run.
SHOTS_OFF_RECEIPT = "[debug] shots=0"


def quit_at_tick(log_text):
    """-> the tick the run was told to stop at if it actually stopped there, else None."""
    if QUIT_AT_TICK_RECEIPT not in log_text:
        return None
    m = _QUIT_TICK_RE.search(log_text)
    return int(m.group(1)) if m else 0


def shot_schedule(path, kind):
    """[(tick, label), ...] for every SHOT the replay schedules, in order."""
    out, seen = [], set()
    for raw in util.read_text(path).splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split()
        if not parts or not parts[0].isdigit():
            continue
        tick = int(parts[0])
        label = None
        if kind == "autoinput":
            if len(parts) >= 3 and parts[1].upper() == "SHOT":
                label = parts[2]
        elif "SHOT" in parts:
            i = parts.index("SHOT")
            if i + 1 < len(parts):
                label = parts[i + 1]
        if label and label not in seen:
            seen.add(label)
            out.append((tick, label))
    return out


def shot_labels(path, kind):
    """SHOT labels the replay schedules, in order, de-duplicated."""
    labels, seen = [], set()
    for raw in util.read_text(path).splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split()
        label = None
        if kind == "autoinput":
            if len(parts) >= 3 and parts[1].upper() == "SHOT":
                label = parts[2]
        else:
            # gdxrec: "<tick> <btn> <sx> <sy> [touch tx ty] [SHOT <label>]"
            if "SHOT" in parts:
                i = parts.index("SHOT")
                if i + 1 < len(parts):
                    label = parts[i + 1]
        if label and label not in seen:
            seen.add(label)
            labels.append(label)
    return labels


def last_tick(path, kind):
    """The last tick the replay schedules, or None."""
    ticks = []
    for raw in util.read_text(path).splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        head = line.split()[0]
        if head.isdigit():
            ticks.append(int(head))
    return max(ticks) if ticks else None


def shots_seen(log_text):
    """SHOT labels the guest actually captured, from either capture path."""
    seen = set()
    for m in _FH_TAG_RE.finditer(log_text):
        tag = m.group(1)
        if tag != AUTO_TAG:
            seen.add(tag)
    for m in _AUTOTEST_RE.finditer(log_text):
        seen.add(m.group(1))
    return seen


def shots_disabled(log_text):
    """True iff the guest logged that [debug] shots=0 skipped every BMP write."""
    return SHOTS_OFF_RECEIPT in log_text


def shots_reached(log_text):
    """SHOT labels the replay REACHED, from the schedule tag rather than a capture.

    This is the honest completion evidence under [debug] shots=0: the lever keeps the
    replay's structure and its tags and skips only the write, so the schedule can still
    be asserted in full while the capture cannot."""
    return {m.group(1) for m in _REC_SHOT_RE.finditer(log_text)}


def completion(block, log_text, max_frame=None, emu_alive_at_end=None):
    """Did the staged replay run to completion? -> dict for the manifest + result line.

    Rules (coordinator, 2026-09-17):
      - .gdxrec : the `[rec] end` receipt AND every scheduled SHOT captured;
      - autoinput: the last scheduled tick reached (the VI tick counter tracks the frame
        counter ~1:1, docs/research/RESUME.md:155) AND every scheduled SHOT captured;
      - either  : the emulator must still have been alive at the end of --seconds, unless
        it exited deliberately via replay_quit / quit_at_tick.
    """
    info = {"replay_complete": None, "shots_expected": None, "shots_seen": None,
            "missing_shots": None, "end_receipt": None, "last_tick": None,
            "quit_at_tick": None, "max_frame": max_frame, "detail": ""}
    if not block:
        return info

    kind = block.get("kind")
    path = block.get("path")
    expected = list(block.get("shots") or (shot_labels(path, kind) if path else []))

    # A run stopped deliberately at quit_at_tick cannot be expected to have fired SHOTs
    # scheduled after that tick, so drop them from the expectation rather than failing a
    # run that did exactly what it was told.
    cut = quit_at_tick(log_text)
    schedule = block.get("schedule") or (shot_schedule(path, kind) if path else [])
    if cut is not None and schedule:
        expected = [l for t, l in schedule if t <= cut]

    # [debug] shots=0 (default 1) skips the BMP write on the main thread, which is what
    # makes a timing run uncontaminated -- 12 of 12 sub-40fps beats in the B38 hardware run
    # were immediately preceded by a dump. It also silences BOTH capture receipts, so
    # scoring such a run on captures marks every one of them FAIL with shots=0/N while the
    # replay in fact ran to its last tick. Fall back to the schedule tag the lever
    # deliberately preserves: the schedule is still asserted in full, only the capture is not.
    shots_off = shots_disabled(log_text)
    seen = shots_reached(log_text) if shots_off else shots_seen(log_text)
    got = [l for l in expected if l in seen]
    missing = [l for l in expected if l not in seen]
    quit_receipt = QUIT_RECEIPT in log_text
    ended = END_RECEIPT in log_text

    info.update({"shots_expected": len(expected), "shots_seen": len(got),
                 "missing_shots": missing, "end_receipt": ended,
                 "shots_disabled": shots_off,
                 "last_tick": block.get("last_tick")})

    problems = []
    if cut is not None:
        info["quit_at_tick"] = cut          # deliberate early stop: end receipt not required
    elif kind == "gdxrec":
        if not ended:
            problems.append("no %r receipt — the replay did not reach its end"
                            % END_RECEIPT)
    else:
        want = block.get("last_tick")
        if want is not None and max_frame is not None and max_frame < want:
            problems.append("stopped at frame %d, short of the last scheduled tick %d"
                            % (max_frame, want))
        elif want is not None and max_frame is None:
            problems.append("no frame counter, cannot prove tick %d was reached" % want)
    if missing:
        problems.append("%d of %d SHOT(s) never %s: %s"
                        % (len(missing), len(expected),
                           "reached" if shots_off else "captured", ", ".join(missing[:6])))
    if emu_alive_at_end is False and not (quit_receipt or ended):
        problems.append("the emulator exited before the run was over and did not quit "
                        "via replay_quit — the core died")

    info["replay_complete"] = not problems
    info["detail"] = "; ".join(problems)
    return info


# What the guest logs when a replay really took. Without checking these, a mis-staged
# replay is invisible: the run boots, never replays, and still PASSes on the heartbeat.
LOADED_RECEIPTS = {
    "autoinput": "autoinput script loaded:",      # port/input_bridge.c
    "gdxrec": "[rec] replay armed",               # port/gdx_input_rec.c:212
}
FAILED_RECEIPTS = (
    "[rec] replay open failed",                   # gdx_input_rec.c:193
    "[rec] replay rejected",                      # gdx_input_rec.c:198
)


def verify_receipts(kind, log_text):
    """Did the guest actually load the replay? -> (ok, detail).

    Called by both lanes so a staging mistake fails the run instead of producing a
    confident PASS from an unscripted boot.
    """
    if not kind:
        return True, ""
    for bad in FAILED_RECEIPTS:
        if bad in log_text:
            line = next((l.strip() for l in log_text.splitlines() if bad in l), bad)
            return False, "guest rejected the replay: %s" % line[:160]
    want = LOADED_RECEIPTS.get(kind)
    if want and want not in log_text:
        return False, ("no %r receipt in the guest log — the replay never loaded "
                       "(wrong path, or [debug] filelog off?)" % want)
    if kind == "autoinput" and "(seconds timebase)" in log_text:
        return False, ("the guest fell back to the seconds timebase — a line over 62 "
                       "bytes split and broke the `ticks` sniff")
    return True, ""


def stale_autoinput_warning(out, sdmc_root=None):
    """No --replay, but a leftover gdx-autoinput.txt would still drive the run."""
    dest = autoinput_path(sdmc_root)
    if os.path.exists(dest):
        out.warn("no --replay given but %s exists (left by an earlier run) — it WILL "
                 "drive this run; pass --replay or delete it" % dest)
        return True
    return False
