"""`gdx-dev emu` — the Azahar lane: lock, preflight, stage, launch, snapshot, verdict.

Replaces tools/ci-3ds.sh:401-481 and every /tmp copy of it. The one substantive
behaviour change over that script is the **[fatal] scoping bug**: ci-3ds.sh greps the
whole shared `azahar_log.txt`, so another agent's crash — or this machine's own previous
run — fails your run. We record the log's byte offset immediately before launch and slice
`emu.log` from there, so the verdict only ever sees this run's bytes
(05-devcli-plan.md §6, "Mitigation: byte-offset slicing at launch").
"""

import contextlib
import os
import shutil
import signal
import subprocess
import time

from . import build as buildmod
from . import inifile, lock, prof, replay, runs, util
from .util import GdxError

SNAPSHOT_S = 20               # PROTOCOL.md line 20 says <=25 s
POLL_S = 2                    # how often we check whether the run is already over

# Receipts that mean THE GUEST is finished, whatever the emulator process is doing.
GUEST_EXIT_RECEIPTS = (
    ("[fatal-trace] atexit:", "exited via exit()"),      # main_3ds.cpp:815
    ("[oracle] quit requested", "oracle quit"),          # replay_quit / quit_at_tick
)
STOP_GRACE_S = 4
DEFAULT_SECONDS = 60

# The measurement set every campaign turns on (05-devcli-plan.md §2 item 5). gputrace is
# NOT here: it inflates profiles (PROTOCOL.md line 22), so it is opt-in per run.
#
# `renderthread` IS here, and it is here for the same reason `strip_run_control` exists on
# the RetroArch lane (rtparity-progress.md). The Qt lane merges into the SHARED virtual SD
# ini, which on this machine has carried `[debug] renderthread = 0` since 2026-09-17 — a
# control value some earlier track left behind. Because `emu run` merges and never
# overwrites, every Qt-lane run since then silently inherited it, including the oracle's
# 6002-frame twin gate. Pinning it here makes the SHIPPING configuration the default a gate
# measures, and makes `--ini debug.renderthread=1` an explicit bisect lever rather than an
# accident of whoever used the SD last.
#
# The shipping value changed: since release-v1 78ff524 the built-in `auto` preset sets
# renderthread = 0 on both models (port/3ds/gdx3ds_preset_n60.c), and the NRR refuses to run
# under the render thread (`[nrr] REFUSED why=renderthread`, gdx3ds_nrr_game.c). An explicit
# ini key beats the preset, so pinning "1" here turned the whole native race renderer OFF on
# every New 3DS lane run while the shipped game runs it ON. (Old 3DS was unaffected: the
# thread self-disables there.) Pin the shipped value.
MEASUREMENT_INI = {
    ("debug", "console"): "1",
    ("debug", "filelog"): "1",
    ("debug", "filelog_max_kb"): "4096",
    ("debug", "verbose"): "1",
    ("debug", "renderthread"): "0",
}


# ------------------------------------------------------------------ preflight --
def preflight(threedsx, out):
    problems = []
    if not os.path.exists(util.AZAHAR_BIN):
        problems.append("Azahar not found at %s" % util.AZAHAR_BIN)
    if not os.path.isfile(threedsx):
        problems.append("no such .3dsx: %s" % threedsx)
    cfg = util.read_text(util.AZAHAR_CFG)
    if "log_filter=*:Debug" not in cfg:
        problems.append("qt-config.ini log_filter is not *:Debug — the frame heartbeat is "
                        "invisible (%s)" % util.AZAHAR_CFG)
    o2r = os.path.join(util.GDX_SDMC_DIR, "fzerox.o2r")
    if not os.path.isfile(o2r):
        problems.append("no fzerox.o2r on the virtual SD (%s) — run tools/prebake" % o2r)
    if problems:
        for p in problems:
            out.err(p)
        raise GdxError("preflight failed (%d problem(s))" % len(problems), util.EXIT_PREFLIGHT)


def guest_exit_reason(log_text):
    """-> a short reason if the guest has finished, else None."""
    for needle, why in GUEST_EXIT_RECEIPTS:
        if needle in log_text:
            return why
    return None


def azahar_version():
    plist = os.path.join(util.AZAHAR_APP, "Contents", "Info.plist")
    if not os.path.exists(plist):
        return None
    p = subprocess.run(["defaults", "read", plist[:-6], "CFBundleShortVersionString"],
                       stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    v = p.stdout.strip()
    return v or None


# ------------------------------------------------------------- log slicing --
class LogSlicer:
    """Byte-offset slice of the shared azahar_log.txt, taken at launch.

    Azahar truncates the log when it starts, so the offset can end up past EOF; that is
    detected (size < offset) and the slice restarts at 0 rather than yielding nothing.
    """

    def __init__(self, path):
        self.path = path
        try:
            self.offset = os.path.getsize(path)
        except OSError:
            self.offset = 0
        self.rotated = False

    def read(self):
        try:
            size = os.path.getsize(self.path)
        except OSError:
            return ""
        start = self.offset
        if size < self.offset:
            self.rotated = True
            start = 0
        try:
            with open(self.path, "rb") as f:
                f.seek(start)
                return f.read().decode("utf-8", errors="replace")
        except OSError:
            return ""

    def dump(self, dest):
        text = self.read()
        tmp = dest + ".part"
        with open(tmp, "w") as f:
            f.write(text)
        os.replace(tmp, dest)
        return len(text)


# ------------------------------------------------------------------ verdict --
def frames_from_emu_log(text):
    """ci-3ds.sh:466-478 semantics: `frame N` inside OutputDebugString lines."""
    import re

    frames = []
    for line in text.splitlines():
        if "OutputDebugString" not in line:
            continue
        for m in re.finditer(r"\bframe (\d+)\b", line):
            frames.append(int(m.group(1)))
    return frames


def judge(emu_text, sd_text, out, replay_kind=None, replay_block=None,
          emu_alive_at_end=None, deadline_hit=None, seconds=None):
    frames = frames_from_emu_log(emu_text)
    p_emu = prof.parse_text(emu_text)
    p_sd = prof.parse_text(sd_text) if sd_text else prof.Parsed()
    # Watchdog beats reach both sinks; take whichever saw more.
    wd = p_emu.watchdog if len(p_emu.watchdog) >= len(p_sd.watchdog) else p_sd.watchdog
    if wd:
        frames += [b["frame"] for b in wd]
    distinct = len(set(frames))
    max_frame = max(frames) if frames else None
    # [fatal] only from THIS run's slice + our own SD log; never the shared log wholesale.
    # ONE classifier for every path (prof.error_census): it drops benign receipts and
    # deduplicates the two sinks, which logFatal() writes to identically.
    census = prof.error_census(emu_text, sd_text)
    errors = {t.strip("[]"): n for t, n in census["errors"].items()}
    fatals = census["fatals"]

    # A staged replay that never loaded must not pass: the run boots fine and the
    # heartbeat advances, so without this check a mis-staged replay reads as success.
    both = sd_text + emu_text
    replay_ok, replay_detail = replay.verify_receipts(replay_kind, both)

    # ...and neither must one that loaded but DIED PART WAY. A guest core that crashes
    # mid-replay still leaves an advancing heartbeat and zero [fatal] lines.
    comp = replay.completion(replay_block, both, max_frame=max_frame,
                             emu_alive_at_end=emu_alive_at_end)

    # One watchdog beat is not a stalled heartbeat when the replay COMPLETED: a short
    # replay (probeboot, 420 ticks) quits via replay_quit before the second beat.
    if max_frame is None or (distinct < 2 and not comp["replay_complete"]):
        verdict, detail = "FAIL", ("heartbeat not advancing (distinct=%d, max=%s)"
                                   % (distinct, max_frame))
    elif fatals:
        verdict, detail = "FAIL", "%d [fatal] line(s) in this run's slice" % fatals
    elif not replay_ok:
        verdict, detail = "FAIL", replay_detail
    elif comp["replay_complete"] is False and deadline_hit:
        # NEXT.md B45: the run was CUT SHORT, not broken. The emulator was still alive and
        # the guest still beating when `--seconds` expired, so the incomplete replay is a
        # statement about the budget, not about the build. Reporting it as FAIL read like
        # a crash and sent readers looking for one.
        verdict = "TIMEOUT"
        detail = ("--seconds%s elapsed with the run still going (frame %s%s): the replay "
                  "was cut short, NOT crashed. Give it more --seconds, or let "
                  "[debug] replay_quit=1 end it at the replay's last tick. (%s)"
                  % ("" if seconds is None else " %d" % seconds, max_frame,
                     "" if not (replay_block or {}).get("last_tick")
                     else " of ~%d" % replay_block["last_tick"], comp["detail"]))
    elif comp["replay_complete"] is False:
        verdict, detail = "FAIL", "replay did not complete: " + comp["detail"]
    else:
        detail = "heartbeat reached frame %d, 0 [fatal] lines" % max_frame
        if comp["replay_complete"]:
            detail += (", replay complete (%d/%d SHOTs)"
                       % (comp["shots_seen"], comp["shots_expected"]))
        verdict = "PASS"
    return {
        "verdict": verdict, "detail": detail, "fatals": fatals,
        "timed_out": verdict == "TIMEOUT",
        "max_frame": max_frame, "distinct_frames": distinct,
        "errors": {k: v for k, v in errors.items() if k not in ("fatal", "fatal-trace")},
        "replay_complete": comp["replay_complete"],
        "shots_expected": comp["shots_expected"], "shots_seen": comp["shots_seen"],
        "missing_shots": comp["missing_shots"],
    }


# ---------------------------------------------------------------- emu run --
def do_run(args, out, ctx):
    root = ctx["root"]
    track = ctx["track"]
    threedsx = os.path.abspath(args.threedsx)
    seconds = args.seconds if args.seconds is not None else DEFAULT_SECONDS

    settings = dict(MEASUREMENT_INI)
    if args.gputrace:
        settings[("debug", "gputrace")] = "1"
    # --3d: the HOST 3D knob (qt-config.ini [Layout] render_3d/factor_3d, patched inside
    # the lock below) plus the guest-side defaults it implies. Without the flag nothing in
    # this function changes. See gdxdev/stereo3d.py for why the guest ini alone is inert.
    three_d = None
    if getattr(args, "three_d", None):
        from . import stereo3d
        three_d = (stereo3d.mode_or_die(args.three_d),
                   stereo3d.depth_or_die(getattr(args, "depth_3d", None)
                                         or stereo3d.DEFAULT_DEPTH))
        settings.update(stereo3d.guest_settings())
    explicit = set()
    for spec in (args.ini or []):
        section, key, value = inifile.parse_spec(spec)
        settings[(section, key)] = value
        explicit.add((section, key))
    if getattr(args, "no_replay_quit", False):
        settings[replay.REPLAY_QUIT_KEY] = "0"
        explicit.add(replay.REPLAY_QUIT_KEY)

    if args.dry_run:
        out.say("DRY RUN — the legacy equivalent of this invocation:")
        out.say("  mkdir /tmp/azahar.lock   # poll; touch every <=25s; rm -rf at the end")
        out.say("  # merge into %s:" % util.GDX_INI)
        for (s, k), v in sorted(settings.items()):
            out.say("  #   [%s] %s = %s" % (s, k, v))
        if args.replay:
            path, kind = replay.resolve(args.replay)
            out.say("  cp %s %s" % (path, util.AUTOINPUT_SD if kind == "autoinput"
                                    else os.path.join(util.GDX_SDMC_DIR, "rec/")))
        if three_d:
            from . import stereo3d
            out.say("  # --3d: set in [Layout] of %s, RESTORED after the run" % util.AZAHAR_CFG)
            for k, v in stereo3d.qt_keys(three_d[0], three_d[1]):
                out.say("  #   %s=%s" % (k, v))
        out.say("  wc -c '%s'   # byte offset for this run's slice" % util.AZAHAR_LOG)
        out.say("  '%s' '%s' &  sleep %d" % (util.AZAHAR_BIN, threedsx, seconds))
        out.say("  pkill -x azahar; sleep 3; pkill -9 -x azahar")
        out.say("  # then: distinct `frame N`, max frame, [fatal] count -> VERDICT")
        return util.EXIT_OK

    preflight(threedsx, out)

    rd = runs.create(ctx["run_root"], track, args.name or "run")
    out.say("run dir: %s" % rd.path)
    started_wall = time.time()

    manifest = runs.new_manifest(
        run_id=rd.run_id, track=track, lane="azahar", started=util.iso_now(),
        seconds=seconds, gputrace=bool(args.gputrace),
        platform={"host": "darwin-arm64", "azahar": azahar_version(),
                  "retroarch": None, "model": "emu-new3ds"},
        build=buildmod.build_info_for_manifest(root, args.build_dir, threedsx),
        tree=buildmod.tree_info(root),        # NEXT.md B46 / PROTOCOL.md
        ini={"%s.%s" % k: v for k, v in settings.items()},
        replay=None, result={"verdict": "UNKNOWN", "fatals": 0, "max_frame": None,
                             "distinct_frames": 0, "errors": {}},
        artifacts={"log": "log.txt", "emu_log": "emu.log", "shots": []},
        ended=None, wall_s=None)
    rd.write_manifest(manifest)

    with lock.Held(track, wait_s=args.lock_wait, note="emu run " + rd.run_id, out=out):
        # We hold the lock, so any live Azahar is unowned — no agent can hold the lock
        # and be running one. Kill it gracefully; a shared log with two writers is
        # unreadable evidence (docs/research/m1-boot-debug.md step 3).
        stale = util.pgrep("azahar")
        if stale:
            out.warn("killing %d unowned azahar process(es): %s"
                     % (len(stale), ", ".join(str(p) for p in stale)))
            for pid in stale:
                _stop_pid(pid)

        if args.replay:
            block, extra = replay.stage(args.replay, out=out)
            manifest["replay"] = block
            settings.update(extra)
            # NEXT.md B19: the run ends at the replay's last tick unless told otherwise.
            replay.apply_implied(settings, explicit, kind=block.get("kind"), out=out,
                                 last=block.get("last_tick"))
        else:
            manifest["replay"] = None
            replay.stale_autoinput_warning(out)

        changed, before, after = inifile.merge(util.GDX_INI, settings)
        out.say("ini merge into %s (%d key(s)%s)"
                % (util.GDX_INI, len(settings), ", changed" if changed else ", no change"))
        for k in sorted(settings):
            if before.get(k) != str(settings[k]):
                out.say("  [%s] %s: %s -> %s" % (k[0], k[1], before.get(k), settings[k]))
        manifest["ini"] = {"%s.%s" % k: v for k, v in settings.items()}
        manifest["gputrace"] = inifile.truthy(settings.get(("debug", "gputrace"), "0"))

        # The Qt lane merges into the SHARED virtual SD, so every `[debug]` / `[native]` key
        # this run did not set is INHERITED from whoever used that SD last. The RetroArch lane
        # strips both sections from a seeded instance ini for exactly this reason; the Qt lane
        # cannot, because that file is the user's. So name them. Measured cost of not doing so
        # (rtparity-progress.md): the shared SD's `renderthread = 0` silently defined the
        # render-thread state of every Qt-lane parity run the fork ever scored, and a stale
        # `[native] machines/fx_sink/fx_sprites/fx_hud = 1` block turned a "levers off"
        # reference lane into something else without a word in the transcript.
        inherited = []
        live_ini = inifile.read(util.GDX_INI)
        for sec in ("debug", "native"):
            for key, val in sorted(live_ini.get(sec, {}).items()):
                if (sec, key) not in settings and str(val).strip() not in ("", "0"):
                    inherited.append("[%s] %s = %s" % (sec, key, val))
        if inherited:
            out.warn("%d lever/run-control key(s) INHERITED from the shared SD ini — this run "
                     "did not set them and they are not neutral:" % len(inherited))
            for line in inherited:
                out.warn("  " + line)
        manifest["ini_inherited"] = inherited

        # The guest opens log.txt with "w" (port/3ds/gdx3ds_filelog.c:113) so it is
        # per-boot already; move the previous one aside anyway so a boot that never
        # reaches the filelog init cannot pass off stale content as this run's.
        if os.path.exists(util.GDX_SD_LOG):
            try:
                os.replace(util.GDX_SD_LOG, util.GDX_SD_LOG + ".prev-" + track)
            except OSError:
                pass

        slicer = LogSlicer(util.AZAHAR_LOG)
        out.say("azahar log offset for this run: %d bytes" % slicer.offset)

        # The qt-config.ini patch has to open AFTER the stale-azahar kill (Azahar rewrites
        # the whole file when it exits) and close BEFORE the lock is released (so no other
        # agent can launch into a 3D config we are about to take away). This stack gives
        # it exactly that window, restore included on any exit path.
        with contextlib.ExitStack() as stack:
            if three_d:
                from . import stereo3d
                stack.enter_context(stereo3d.QtStereo(three_d[0], three_d[1], out))
                manifest["stereo3d"] = stereo3d.qt_receipt(three_d[0], three_d[1])

            shots_before = _shot_set()
            alive_at_end = True
            pid, console_path = _launch(threedsx, rd, args.launch, out)
            out.say("azahar pid %d; running %ds..." % (pid, seconds))

            deadline = time.time() + seconds
            t0 = time.time()
            next_snap = 0.0
            try:
                while time.time() < deadline:
                    # Poll often, snapshot rarely. The old loop slept a full SNAPSHOT_S
                    # between checks, so a run that finished in 8 s still sat here.
                    time.sleep(min(POLL_S, max(0.1, deadline - time.time())))
                    now = time.time()
                    if now >= next_snap:
                        _snapshot(rd, slicer)
                        next_snap = now + SNAPSHOT_S
                    if not util.pid_alive(pid):
                        alive_at_end = False
                        out.warn("azahar exited early after %.0fs" % (now - t0))
                        break
                    # The EMULATOR outliving the GUEST is the common case: a .3dsx that
                    # exits (replay_quit, quit_at_tick, or its own exit()) leaves Azahar up
                    # with a dead window, so waiting on the process idled out the whole of
                    # --seconds after the run was already over.
                    why = guest_exit_reason(util.read_text(rd.sub("log.txt")))
                    if why:
                        out.say("guest exited after %.0fs (%s) — stopping early" % (now - t0, why))
                        break
            except KeyboardInterrupt:
                out.warn("interrupted — stopping azahar and keeping the artifacts")
            finally:
                _stop_pid(pid)
                _snapshot(rd, slicer)

            shots = _collect_shots(rd, shots_before)

    emu_text = util.read_text(rd.sub("emu.log"))
    sd_text = util.read_text(rd.sub("log.txt"))
    if slicer.rotated:
        out.warn("azahar rotated its log during the run; the slice restarts at byte 0")
    result = judge(emu_text, sd_text, out,
                   replay_kind=(manifest.get("replay") or {}).get("kind"),
                   replay_block=manifest.get("replay"),
                   emu_alive_at_end=alive_at_end)

    manifest["ended"] = util.iso_now()
    manifest["wall_s"] = round(time.time() - started_wall, 1)
    manifest["result"] = {k: result[k] for k in
                          ("verdict", "fatals", "max_frame", "distinct_frames", "errors",
                           "replay_complete", "shots_expected", "shots_seen",
                           "missing_shots")}
    manifest["artifacts"] = {"log": "log.txt", "emu_log": "emu.log", "shots": shots}
    rd.write_manifest(manifest)
    rd.write_verdict(result["verdict"], result["detail"])
    with open(rd.sub("RESULT"), "w") as f:
        f.write(result_line(manifest, result) + "\n")

    out.say("")
    out.say(result_line(manifest, result))
    out.table(["field", "value"], [
        ["run", rd.run_id], ["verdict", result["verdict"]], ["detail", result["detail"]],
        ["distinct frames", result["distinct_frames"]], ["max frame", result["max_frame"]],
        ["replay complete", result["replay_complete"]],
        ["SHOTs", "%s/%s" % (result["shots_seen"], result["shots_expected"])],
        ["gputrace", manifest["gputrace"]], ["wall_s", manifest["wall_s"]],
        ["artifact sha256", (manifest["build"] or {}).get("sha256")],
        ["dir", rd.path],
    ])
    out.emit(manifest)
    return util.EXIT_OK if result["verdict"] == "PASS" else util.EXIT_FAIL


def _launch(threedsx, rd, mode, out):
    """Start Azahar and return (pid, console_path).

    `open -a` is PROTOCOL.md line 15's launch and the DEFAULT here, because running the
    binary directly — which tools/ci-3ds.sh does — pops a modal
    "The `azahar` executable is being run directly rather than via the Azahar.app bundle"
    dialog. Emulation still proceeds behind it, which is why ci-3ds.sh never noticed, but
    the dialog is the frontmost window, so `emu shot` captures the alert instead of the
    game. Measured 2026-09-07.

    The cost of `open` is that the child's stdout is not ours, so console.txt stays empty;
    the two real sinks (this run's azahar-log slice and the guest's SD log) are unaffected.
    Pass `--launch direct` to get console.txt back and accept the dialog.
    """
    console_path = rd.sub("console.txt")
    if mode == "direct":
        console = open(console_path, "w")
        proc = subprocess.Popen([util.AZAHAR_BIN, threedsx],
                                stdout=console, stderr=subprocess.STDOUT)
        out.warn("--launch direct: expect Azahar's \"run directly\" alert on top of the "
                 "window; `emu shot` may capture it instead of the game")
        return proc.pid, console_path

    before = set(util.pgrep("azahar"))
    util.run(["open", "-n", "-a", util.AZAHAR_APP, "--args", threedsx], check=True)
    with open(console_path, "w") as f:
        f.write("(launched via `open -n -a %s`; the child's stdout belongs to launchd, "
                "not to gdx-dev. Use --launch direct if you need it.)\n" % util.AZAHAR_APP)
    deadline = time.time() + 20
    while time.time() < deadline:
        new = [p for p in util.pgrep("azahar") if p not in before]
        if new:
            return new[0], console_path
        time.sleep(0.5)
    raise GdxError("Azahar did not start within 20s of `open`", util.EXIT_PREFLIGHT)


def result_line(manifest, result):
    """The single line a report quotes. Carries the artifact sha256 rather than the build
    id: GDX3DS_BUILD_ID is a CONFIGURE-time value, so after a merge-then-rebuild it names
    the wrong commit and can lie about what actually ran."""
    b = manifest.get("build") or {}
    rep = manifest.get("replay") or {}
    bits = ["%s" % result["verdict"],
            "frame=%s" % result.get("max_frame"),
            "fatals=%s" % result.get("fatals")]
    if rep:
        bits.append("replay=%s(%s)" % (rep.get("id"), rep.get("kind")))
        bits.append("complete=%s" % result.get("replay_complete"))
        bits.append("shots=%s/%s" % (result.get("shots_seen"),
                                     result.get("shots_expected")))
    bits.append("wall_s=%s" % manifest.get("wall_s"))
    bits.append("sha256=%s" % (b.get("sha256") or "?")[:16])
    return "RESULT " + " ".join(bits)


def _snapshot(rd, slicer):
    """Copy-then-rename so a torn read never replaces good evidence (§6)."""
    rd.snapshot(util.GDX_SD_LOG, "log.txt")
    slicer.dump(rd.sub("emu.log"))


def _autotest_dir():
    return os.path.join(util.AZAHAR_SDMC, "autotest")


def _shot_set():
    d = _autotest_dir()
    if not os.path.isdir(d):
        return {}
    out = {}
    for f in os.listdir(d):
        try:
            out[f] = os.path.getmtime(os.path.join(d, f))
        except OSError:
            pass
    return out


def _collect_shots(rd, before):
    d = _autotest_dir()
    got = []
    if not os.path.isdir(d):
        return got
    for f in sorted(os.listdir(d)):
        p = os.path.join(d, f)
        try:
            mt = os.path.getmtime(p)
        except OSError:
            continue
        if f in before and before[f] >= mt:
            continue          # untouched: another agent's shot, not ours
        try:
            shutil.copy2(p, rd.sub("shots", f))
            got.append(f)
        except OSError:
            pass
    return got


def _stop_pid(pid):
    """SIGTERM (flushes the log) then SIGKILL. Only ever our own / unowned pid."""
    try:
        os.kill(pid, signal.SIGTERM)
    except OSError:
        return
    for _ in range(int(STOP_GRACE_S * 4)):
        if not util.pid_alive(pid):
            return
        time.sleep(0.25)
    try:
        os.kill(pid, signal.SIGKILL)
    except OSError:
        pass
    time.sleep(0.5)


# ------------------------------------------------------- lock subcommands --
def do_lock(args, out, ctx):
    if args.hold:
        # Daemonised toucher: an interactive agent keeps the lock across many commands.
        pid = os.fork()
        if pid == 0:
            os.setsid()
            lock.acquire(ctx["track"], args.wait, "hold (daemon)", None)
            try:
                while True:
                    time.sleep(lock.HEARTBEAT_S)
                    lock.touch()
            except Exception:
                lock.release(force=True)
            os._exit(0)
        time.sleep(1.5)
        st = lock.status()
        out.say("lock held by daemon pid %s for track %s"
                % ((st.get("owner") or {}).get("pid"), ctx["track"]))
        out.emit(st)
        return util.EXIT_OK
    lock.acquire(ctx["track"], args.wait, "manual", out)
    out.say("lock acquired for track %s (pid %d) — remember `gdx-dev emu unlock`"
            % (ctx["track"], os.getpid()))
    out.warn("this process exits now, so nothing is refreshing the lock's mtime; "
             "use --hold for an interactive session")
    return util.EXIT_OK


def do_unlock(args, out, ctx):
    owner = lock.read_owner() or {}
    if lock.release(force=args.force):
        out.say("released /tmp/azahar.lock (was: track=%s pid=%s)"
                % (owner.get("track"), owner.get("pid")))
        if owner.get("pid") and util.pid_alive(owner["pid"]) and args.force:
            out.warn("owner pid %s is still alive — you may have stolen a live run"
                     % owner["pid"])
    else:
        out.say("lock was not held")
    return util.EXIT_OK


def do_status(args, out, ctx):
    st = lock.status()
    if not st["held"]:
        out.say("azahar lock: free")
    else:
        o = st.get("owner") or {}
        out.table(["field", "value"], [
            ["held", "yes"], ["track", o.get("track", "(legacy bash holder)")],
            ["pid", o.get("pid")], ["pid alive", st.get("pid_alive")],
            ["started", o.get("started")], ["note", o.get("note")],
            ["age", "%ss" % st.get("age_s")], ["stealable", st.get("stealable")],
        ])
    live = util.pgrep("azahar")
    out.say("azahar processes: %s" % (", ".join(str(p) for p in live) or "none"))
    st["azahar_pids"] = live
    out.emit(st)
    return util.EXIT_OK
