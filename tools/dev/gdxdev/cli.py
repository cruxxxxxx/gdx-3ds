"""Argument parsing and dispatch for `tools/dev/gdx-dev`.

One entry point, subcommands, uniform exit codes (05-devcli-plan.md §4):
  0 OK/PASS · 1 FAIL · 2 usage · 3 preflight · 4 lock timeout · 5 staging ·
  6 no data parsed · 7 delegate tool missing
"""

import argparse
import os
import sys

from . import build as buildmod
from . import capture, delegate, emu, hwrun, report, sd, util
from .util import GdxError

EPILOG = """\
docs: docs/research/native/05-devcli-plan.md (spec), tools/dev/README.md (examples)
exit codes: 0 PASS  1 FAIL  2 usage  3 preflight  4 lock  5 staging  6 no data  7 no delegate
"""


#: The driver-level options. Declared once here so `_common` and the regression
#: test that walks the parser tree (test_gdxdev.py::DriverOptionShadowTest) read
#: the same list.
DRIVER_OPTIONS = ("track", "run_root", "json", "quiet", "dry_run")


def _common(p, driver=False):
    """The driver-level options, repeated on every subparser for discoverability.

    NEXT.md B52. argparse's subparser action parses into a FRESH namespace and then
    copies EVERY key of it onto the driver's namespace — including keys that only
    hold the SUBPARSER's default. So a repeat of a driver-level option silently
    overwrote the value the driver had already parsed: `gdx-dev --track X retro run`
    filed the run under the worktree's name, not X, and `gdx-dev --dry-run retro
    run` really ran. Caught when covdrift.sh reported `RESULT PASS ... shots=49/49`
    for every replay and then `NO RUN` for every replay.

    The fix is that only the DRIVER declares defaults. Every repeat uses
    `argparse.SUPPRESS`, which leaves the key absent from the subnamespace unless it
    was actually typed, so nothing is copied over and the driver's value survives.
    Typing it on both still works and the subcommand's value wins, which is the
    order a reader expects.
    """
    def dflt(value):
        return value if driver else argparse.SUPPRESS

    p.add_argument("--track", default=dflt(None),
                   help="track name (default: worktree basename)")
    p.add_argument("--run-root", default=dflt(None),
                   help="run directory root (default: %s)" % util.DEFAULT_RUN_ROOT)
    p.add_argument("--json", action="store_true", default=dflt(False),
                   help="machine-readable result on stdout")
    p.add_argument("--quiet", action="store_true", default=dflt(False))
    p.add_argument("--dry-run", action="store_true", default=dflt(False),
                   help="print exactly what this invocation would do and do NONE of it: "
                        "nothing launched, written, downloaded or locked")


def _add_3d(p):
    """`--3d` / `--depth-3d`, shared by both lanes (gdxdev/stereo3d.py).

    `--3d` with no value means side-by-side, the one mode where a single captured host
    frame visibly contains both eyes. `dest` is spelled out because an option starting
    with a digit gets no usable automatic attribute name.
    """
    from . import stereo3d
    p.add_argument("--3d", dest="three_d", nargs="?", const=stereo3d.DEFAULT_MODE,
                   default=None, metavar="MODE",
                   help="render the emulator in stereoscopic 3D so the guest's right-eye "
                        "pass actually runs (the emulated 3D slider is 0 otherwise, and "
                        "[stereo] enabled=1 alone does nothing). MODE: %s (default %s). "
                        "Implies [stereo] enabled=1 and [debug] fh_eye=both"
                        % (", ".join(sorted(stereo3d.MODES)), stereo3d.DEFAULT_MODE))
    p.add_argument("--depth-3d", dest="depth_3d", type=int, default=None, metavar="PCT",
                   help="3D depth/separation percentage for --3d, 1..100 (default %d)"
                        % stereo3d.DEFAULT_DEPTH)


def _add_model(p):
    """`--model new|old` (gdxdev/retro.py MODELS).

    Old 3DS is NEXT.md C4's target: half the FCRAM and no core 2, so the port's render
    thread self-disables there whatever `[debug] renderthread` says. An `old` run is
    therefore a different EXECUTION from every New 3DS cell, not a different setting --
    never quote its frame hashes against one.
    """
    from . import retro
    p.add_argument("--model", dest="model", default=None,
                   choices=sorted(retro.MODELS),
                   help="emulated system model: %s (default: the core's own, New 3DS; "
                        "every run now writes the per-instance core-options redirect, so "
                        "this is a label on the same value the core already defaults to). "
                        "--model old is the Old 3DS lane."
                        % ", ".join("%s (%s)" % (k, v)
                                    for k, v in sorted(retro.MODELS.items())))


def retro_defaults():
    """(no-replay default, staged-replay default) seconds, from the lane itself."""
    from . import retro
    return retro.DEFAULT_SECONDS, retro.REPLAY_SECONDS


def build_parser():
    ap = argparse.ArgumentParser(
        prog="gdx-dev",
        description="One CLI for the 3DS fork's dev loop: build, emulator, profile, SD.",
        epilog=EPILOG, formatter_class=argparse.RawDescriptionHelpFormatter)
    _common(ap, driver=True)
    sub = ap.add_subparsers(dest="cmd")

    # ---- build
    b = sub.add_parser("build", help="cmake/ninja/ccache build; prints build id + times")
    _common(b)
    b.add_argument("--target", default=None, help="cmake target (default %s)"
                   % buildmod.DEFAULT_TARGET)
    b.add_argument("--build-dir", default=buildmod.DEFAULT_BUILD_DIR)
    b.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    b.add_argument("--clean", action="store_true", help="delete the build dir first")
    b.add_argument("--reconfigure", action="store_true",
                   help="force a fresh configure (also picks Ninja if available)")
    b.add_argument("--touch-patched", action="store_true",
                   help="touch patched sources and assert the .o files rebuild "
                        "(the stale-object trap, PROTOCOL.md line 11)")
    b.set_defaults(func=buildmod.do_build)

    # ---- emu
    e = sub.add_parser("emu", help="Azahar lane: run / lock / unlock / status / shot / click")
    esub = e.add_subparsers(dest="sub")

    r = esub.add_parser("run", help="locked, staged, snapshotted emulator run with a verdict")
    _common(r)
    r.add_argument("threedsx", help="absolute or relative path to the .3dsx")
    r.add_argument("--replay", help="replay id under tools/dev/replays/, or a path")
    r.add_argument("--no-replay-quit", dest="no_replay_quit", action="store_true",
                   help="play PAST the replay's last tick and end on --seconds instead "
                        "([debug] replay_quit=0). For diagnostics that watch post-replay "
                        "behaviour (wobble, dynlod). Same as --ini debug.replay_quit=0")
    r.add_argument("--ini", action="append", metavar="K=V",
                   help="merge an ini key ([debug] by default; `section.k=v` to target one)")
    r.add_argument("--seconds", type=int, default=None,
                   help="run length (default %d)" % emu.DEFAULT_SECONDS)
    r.add_argument("--name", help="slug for the run id")
    r.add_argument("--gputrace", action="store_true",
                   help="set [debug] gputrace=1 (inflates profiles; recorded in the manifest)")
    r.add_argument("--shots", action="store_true",
                   help="collect autotest/*.bmp written by SHOT events")
    r.add_argument("--capture", action="store_true", help="host window capture at the end")
    _add_3d(r)
    r.add_argument("--lane", choices=["azahar", "retro"], default="azahar")
    r.add_argument("--lock-wait", type=int, default=900, help="seconds to wait for the lock")
    r.add_argument("--launch", choices=["open", "direct"], default="open",
                   help="`open -a Azahar.app` (default, PROTOCOL.md line 15, no modal "
                        "alert) or the binary directly (gives console.txt, pops the alert)")
    r.add_argument("--build-dir", default=buildmod.DEFAULT_BUILD_DIR)
    r.set_defaults(func=_emu_run_dispatch)

    lk = esub.add_parser("lock", help="take the machine-wide Azahar lock")
    _common(lk)
    lk.add_argument("--wait", type=int, default=600)
    lk.add_argument("--hold", action="store_true",
                    help="daemonise a toucher so the lock survives this process")
    lk.set_defaults(func=emu.do_lock)

    ul = esub.add_parser("unlock", help="release the lock")
    _common(ul)
    ul.add_argument("--force", action="store_true", help="release a lock you do not own")
    ul.set_defaults(func=emu.do_unlock)

    st = esub.add_parser("status", help="who holds the lock, and is Azahar running")
    _common(st)
    st.set_defaults(func=emu.do_status)

    sh = esub.add_parser("shot", help="capture the emulator window (both screens if layout_option=0)")
    _common(sh)
    sh.add_argument("--out", help="destination .png")
    sh.add_argument("--owner", default="azahar", help="window owner substring")
    sh.set_defaults(func=capture.do_shot)

    ck = esub.add_parser("click", help="synthetic click at X,Y (drives the host-side touch menu)")
    _common(ck)
    ck.add_argument("xy", metavar="X,Y")
    ck.add_argument("--double", action="store_true")
    ck.set_defaults(func=capture.do_click)

    # ---- prof
    p = sub.add_parser("prof", help="parse [prof]/[profop]/[rt]/[watchdog] tables from a log or run")
    _common(p)
    p.add_argument("target", nargs="?", help="log file, run dir, or run id")
    p.add_argument("--sections", action="store_true")
    p.add_argument("--ops", action="store_true")
    p.add_argument("--fps", action="store_true", help="p10/p50/p90 from [watchdog] deltas")
    p.add_argument("--by-vtxn", action="store_true",
                   help="bucket [prof] windows by nD (crowd nD>=100 vs menu)")
    p.add_argument("--main", action="store_true",
                   help="[mprof] MAIN-thread section table, ranked (NEXT.md A8); "
                        "add --by-vtxn to bucket it by nD as well")
    p.add_argument("--ab", nargs=2, metavar=("RUN_A", "RUN_B"))
    p.add_argument("--window", type=int, default=0, help="use only the last N windows")
    p.add_argument("--force", action="store_true", help="A/B across different build ids")
    p.add_argument("--fail-on-error", action="store_true", help="exit 1 on any error receipt")
    p.set_defaults(func=report.do_prof)

    # ---- runs / report
    ru = sub.add_parser("runs", help="list / show / path of this track's runs")
    _common(ru)
    ru.add_argument("action", choices=["list", "show", "path"])
    ru.add_argument("ident", nargs="?")
    ru.set_defaults(func=report.do_runs)

    rp = sub.add_parser("report", help="the block you paste into a progress file")
    _common(rp)
    rp.add_argument("ident", nargs="?", help="run id (default: newest)")
    rp.add_argument("--markdown", action="store_true")
    rp.set_defaults(func=_report_dispatch)

    # ---- sd
    s = sub.add_parser("sd", help="physical SD card: stage / pull (dry-run by default)")
    ssub = s.add_subparsers(dest="sub")
    ss = ssub.add_parser("stage", help="copy a build + merge ini onto the card")
    _common(ss)
    ss.add_argument("source", help="file or directory to stage under 3ds/")
    ss.add_argument("--ini", action="append", metavar="K=V")
    ss.add_argument("--volume", default=None, help="default %s" % sd.DEFAULT_VOLUME)
    ss.add_argument("--wipe-log", action="store_true")
    ss.add_argument("--no-eject", action="store_true")
    ss.add_argument("--apply", action="store_true",
                    help="actually write (without this it is a dry run)")
    ss.set_defaults(func=sd.do_stage)
    sp = ssub.add_parser("pull", help="copy log.txt + autotest shots off the card")
    _common(sp)
    sp.add_argument("dest")
    sp.add_argument("--volume", default=None)
    sp.set_defaults(func=sd.do_pull)

    # ---- delegations
    d = sub.add_parser("diff", help="frame/hash diff — delegates to the oracle track")
    _common(d)
    d.add_argument("mode", nargs="?", default="frames",
                   help="`hashes` for [fh] log diff (fhdiff.py); otherwise framediff.py")
    d.add_argument("rest", nargs=argparse.REMAINDER)
    d.add_argument("--mask")
    d.add_argument("--sheet", action="store_true")
    d.set_defaults(func=delegate.do_diff)

    h = sub.add_parser("hw", help="hardware link — delegates to tools/dev/hw.py (hwlink track)")
    _common(h)
    h.add_argument("rest", nargs=argparse.REMAINDER)
    h.set_defaults(func=delegate.do_hw)

    # ---- hw-run: the unattended hardware config matrix (NEXT.md A9)
    hr = sub.add_parser("hw-run", help="unattended hardware A/B: a config matrix x a "
                                       "replay over the WiFi link, with a summary table")
    _common(hr)
    hr.add_argument("--config", action="append", metavar="SPEC",
                    help="one config, repeatable: 'native.all=0,native.interp=0'. "
                         "An EMPTY value deletes the key (`k=`), which is what clearing "
                         "a lever on the guest requires")
    hr.add_argument("--replay", default="race-mutecity-start",
                    help="replay id or path; must be a .gdxrec (tick-exact injector)")
    hr.add_argument("--repeats", type=int, default=1,
                    help="repeats per config; the OUTER loop, so drift hits every config")
    hr.add_argument("--seconds", type=int, default=240,
                    help="ceiling per slice; the slice ends at the replay's `[rec] end`")
    hr.add_argument("--settle", type=int, default=6,
                    help="seconds after the end receipt, so the last [prof] window lands")
    hr.add_argument("--poll", type=int, default=3, help="seconds between completion polls")
    hr.add_argument("--wait", type=int, default=60, help="seconds to wait for a relaunch")
    hr.add_argument("--gputrace", action="store_true",
                    help="[debug] gputrace=1 — inflates every millisecond; recorded in "
                         "each manifest and printed in the header")
    hr.add_argument("--base-ini", default=None, metavar="SPEC",
                    help="ini applied to every config (default %r, plus the replay and "
                         "gputrace state)" % hwrun.DEFAULT_BASE_INI)
    hr.add_argument("--restore", default=None, metavar="SPEC",
                    help="ini applied once at the end (default %r)" % hwrun.DEFAULT_RESTORE)
    hr.add_argument("--no-restore", action="store_true",
                    help="leave the last config armed on the console")
    hr.add_argument("--fps-window", type=float, default=hwrun.DEFAULT_FPS_WINDOW_S,
                    help="seconds of racing to pool fps over (default 60)")
    hr.add_argument("--fps-anchor", default=hwrun.IN_RACE_ANCHOR,
                    help="SHOT label the fps window starts after (default race00)")
    hr.add_argument("--3dsx", dest="threedsx", default=None)
    hr.add_argument("--ip", default=None, help="console IPv4 (default: tools/dev/hw.ini)")
    hr.add_argument("--name", default=None, help="slug for the run id")
    hr.set_defaults(func=hwrun.do_hw_run)

    # ---- retro (spike)
    rt = sub.add_parser("retro", help="RetroArch/libretro lane (spike)")
    rtsub = rt.add_subparsers(dest="sub")
    rr = rtsub.add_parser("run", help="boot a .3dsx in a private RetroArch instance")
    _common(rr)
    rr.add_argument("threedsx")
    rr.add_argument("--instance", default=None,
                    help="instance name -> its own config, private SD and UDP port "
                         "(default: the run id, which is unique — two agents that both "
                         "omit this can never collide)")
    rr.add_argument("--seconds", type=int, default=None,
                    help="run CEILING in seconds. Default: %d with a staged --replay "
                         "(60 truncated every corpus replay — NEXT.md B45), %d without. "
                         "With replay_quit the run ends at the replay's last tick and "
                         "never reaches the ceiling"
                         % (retro_defaults()[1], retro_defaults()[0]))
    rr.add_argument("--frames", type=int, default=None, help="--max-frames N (bounded run)")
    rr.add_argument("--gfx", default=None, metavar="vulkan|opengl|software",
                    help="pin the renderer (case-insensitive). Default: try Vulkan, then "
                         "OpenGL, then Software. See tools/dev/README.md for which is "
                         "stable here")
    rr.add_argument("--name")
    rr.add_argument("--ini", action="append", metavar="K=V")
    rr.add_argument("--replay")
    # NEXT.md B19. A staged replay implies [debug] replay_quit=1 so the run ENDS at the
    # replay's last tick instead of playing on, unscripted, until --seconds expires.
    # The override is a real flag rather than folklore because runs that need it exist:
    # the wobble and dynlod investigations watch what the guest does AFTER the scripted
    # inputs stop, and a hardcoded flip would have silently ended those runs early.
    rr.add_argument("--no-replay-quit", dest="no_replay_quit", action="store_true",
                    help="play PAST the replay's last tick and end on --seconds instead "
                         "([debug] replay_quit=0). For diagnostics that watch post-replay "
                         "behaviour (wobble, dynlod). Same as --ini debug.replay_quit=0")
    rr.add_argument("--build-dir", default=buildmod.DEFAULT_BUILD_DIR)
    rr.add_argument("--bare-ini", dest="bare_ini", action="store_true",
                    help="start the instance ini EMPTY instead of seeding it from the Qt SD's "
                         "user sections ([display] [stereo] [input] [net] [hud] ...): the run "
                         "sees only the measurement keys and its own --ini, i.e. the no-ini "
                         "player's built-in defaults (preset-fix parity)")
    _add_3d(rr)
    _add_model(rr)
    rr.set_defaults(func=_retro_dispatch)
    rs = rtsub.add_parser("setup", help="install/verify the core and write per-instance configs")
    _common(rs)
    rs.add_argument("--instance", default="a")
    rs.set_defaults(func=_retro_dispatch)
    rc = rtsub.add_parser("cmd", help="send a UDP network command (SCREENSHOT, PAUSE_TOGGLE, ...)")
    _common(rc)
    rc.add_argument("verb")
    rc.add_argument("--instance", default="a")
    rc.set_defaults(func=_retro_dispatch)

    rp2 = rtsub.add_parser("reap", help="kill RetroArch processes whose driver is gone")
    _common(rp2)
    rp2.add_argument("--prune", action="store_true",
                     help="also delete idle instance dirs older than --days")
    rp2.add_argument("--days", type=float, default=2,
                     help="age threshold for --prune, in days; fractions allowed (0.05 = 72 min) (default 2)")
    rp2.set_defaults(func=_retro_dispatch)

    rst = rtsub.add_parser("status", help="which instances are running, stale or orphaned")
    _common(rst)
    rst.add_argument("--instance", default=None, help="just this one")
    rst.add_argument("--all", action="store_true", help="include idle instances")
    rst.set_defaults(func=_retro_dispatch)

    return ap


def _emu_run_dispatch(args, out, ctx):
    if args.lane == "retro":
        from . import retro
        return retro.do_run(args, out, ctx)
    return emu.do_run(args, out, ctx)


def _retro_dispatch(args, out, ctx):
    from . import retro
    return {"run": retro.do_run, "setup": retro.do_setup, "cmd": retro.do_cmd,
            "reap": retro.do_reap, "status": retro.do_status}[args.sub](args, out, ctx)


def _report_dispatch(args, out, ctx):
    if not args.ident:
        from . import runs

        lst = runs.listing(ctx["run_root"], ctx["track"])
        if not lst:
            raise GdxError("no runs for track %r" % ctx["track"], util.EXIT_NODATA)
        args.ident = lst[-1].run_id
    return report.do_report(args, out, ctx)


def main(argv=None):
    ap = build_parser()
    args = ap.parse_args(argv)
    if not getattr(args, "func", None):
        ap.print_help()
        return util.EXIT_USAGE

    out = util.Out(quiet=getattr(args, "quiet", False), as_json=getattr(args, "json", False))
    ctx = {
        "root": util.repo_root(),
        "track": getattr(args, "track", None) or os.environ.get("GDX_TRACK")
                 or util.default_track(),
        "run_root": getattr(args, "run_root", None) or os.environ.get("GDX_RUN_ROOT")
                    or util.DEFAULT_RUN_ROOT,
    }
    try:
        return args.func(args, out, ctx) or util.EXIT_OK
    except GdxError as e:
        out.err(str(e))
        return e.code
    except KeyboardInterrupt:
        out.err("interrupted")
        return util.EXIT_FAIL


if __name__ == "__main__":
    sys.exit(main())
