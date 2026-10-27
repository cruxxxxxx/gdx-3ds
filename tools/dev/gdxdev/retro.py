"""`gdx-dev retro` — the RetroArch + Azahar-libretro lane (05-devcli-plan.md §3, §5 step 8).

WHY this lane exists: Azahar Qt has no way to move its user-data directory, so one
machine gets one virtual SD and one log, and four agents have to serialise behind a /tmp
lock. The libretro core takes its SD root from the frontend's `savefile_directory` /
`system_directory` (core_settings.cpp ParseStorageOptions -> FileUtil::SetUserPath), so a
per-instance `--config` gives every agent a PRIVATE virtual SD and N concurrent instances
with no lock at all.

SPIKE VERDICT (2026-09-07): PASSES. Two instances booted the same .3dsx concurrently,
each reaching the in-race attract demo at ~58 fps with its own guest log.txt, its own
screenshot and independent game state. Working configuration, all four measured here:
  - core: the Azahar 2126.0 release build (azahar-libretro-macos-arm64-2126.0.zip),
    19,160,008 bytes, Mach-O arm64 — NOT the nightly, which is only the fallback;
  - citra_graphics_api = "Vulkan" with RetroArch video_driver = "vulkan" (MoltenVK).
    First try; OpenGL and Software were never needed;
  - citra_is_new_3ds = "enabled" (mandatory: the render thread is New3DS-only) -- this
    spelling turned out NOT to be a legal option value and was silently falling back to
    the option default the whole time; see CORE_OPTIONS_TEMPLATE / the README's
    core_options warning for the actual legal value ("New 3DS") and the emufix fix
    (2026-10-16) that made this file start being read at all;
  - the private SD is <savefile_directory>/Azahar/Azahar/sdmc — the "Azahar" segment
    appears TWICE (see Instance.sdmc()).
Azahar Qt remains the DEFAULT lane; `--lane retro` is opt-in. Details and the two
gotchas are in docs/research/native/devcli-progress.md.
"""

import atexit
import errno
import fcntl
import json
import os
import re
import shutil
import signal
import socket
import subprocess
import time
import urllib.request
import zipfile

from . import build as buildmod
from . import inifile, prof, replay, runs, util
from .emu import guest_exit_reason as emu_guest_exit_reason
from .util import GdxError

RETRO_BIN = util.RETROARCH_BIN
INSTANCE_ROOT = os.path.expanduser("~/Library/Application Support/gdx-retro")
CORE_NAME = "azahar_libretro.dylib"

# Prefer the release core pinned to the same Azahar version as the Qt lane, so the two
# lanes stay comparable; the nightly is the fallback (05-devcli-plan.md §3 item 2).
CORE_RELEASE_URL = ("https://github.com/azahar-emu/azahar/releases/download/2126.0/"
                    "azahar-libretro-macos-arm64-2126.0.zip")
CORE_NIGHTLY_URL = ("https://buildbot.libretro.com/nightly/apple/osx/arm64/latest/"
                    "azahar_libretro.dylib.zip")

BASE_PORT = 55355          # retroarch.cfg network_cmd_port default
# §3 item 7a called the renderer the one unproven link. Measured 2026-09-07: Vulkan works
# on this Mac's MoltenVK on the first try, so it leads the order and the other two are
# only insurance.
GFX_ORDER = ["Vulkan", "OpenGL", "Software"]

# `--gfx` is accepted case-insensitively; these are the core's own option spellings.
GFX_ALIASES = {"vulkan": "Vulkan", "opengl": "OpenGL", "gl": "OpenGL",
               "glcore": "OpenGL", "software": "Software", "soft": "Software"}

# macOS restores "windows from the last session" after an abnormal exit by putting up an
# NSAlert. RetroArch has no window to attach it to at that point, so the alert is
# invisible and the process sits there forever — which is what several runs did after the
# SIGKILL testing. ApplePersistenceIgnoreState turns the whole mechanism off, and
# NSQuitAlwaysKeepsWindows=NO stops the "The last time you opened RetroArch, it
# unexpectedly quit while reopening windows" prompt. Written before every launch rather
# than once, because anything can reset a user default, and to BOTH bundle ids: the
# libretro.com download is `com.libretro.dist.RetroArch`, which the old single id
# (`com.libretro.RetroArch`) never reached. The saved-state folder is removed too, so a
# killed run never leaves a session for the next launch to offer back.
RETROARCH_BUNDLE_IDS = ("com.libretro.dist.RetroArch", "com.libretro.RetroArch")
RETROARCH_DEFAULTS = (("ApplePersistenceIgnoreState", "YES"), ("NSQuitAlwaysKeepsWindows", "NO"))


def suppress_restore_prompt(out=None):
    ok = True
    for bundle_id in RETROARCH_BUNDLE_IDS:
        for key, value in RETROARCH_DEFAULTS:
            p = subprocess.run(["defaults", "write", bundle_id, key, "-bool", value],
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            if p.returncode != 0:
                ok = False
                if out:
                    out.warn("could not set %s %s: %s" % (bundle_id, key, (p.stdout or "").strip()))
        saved_state = os.path.expanduser(
            "~/Library/Saved Application State/%s.savedState" % bundle_id)
        shutil.rmtree(saved_state, ignore_errors=True)
    return ok


def normalise_gfx(value):
    if not value:
        return None
    key = str(value).strip().lower()
    if key not in GFX_ALIASES:
        raise GdxError("unknown --gfx %r (want: vulkan, opengl or software)" % value,
                       util.EXIT_USAGE)
    return GFX_ALIASES[key]


# ------------------------------------------------------------------ instance --
class Instance:
    """One private RetroArch: its own config, virtual SD, log, screenshots, UDP port."""

    def __init__(self, name):
        self.name = name
        self.dir = os.path.join(INSTANCE_ROOT, name)

    @property
    def cfg(self):
        return os.path.join(self.dir, "retroarch.cfg")

    @property
    def core_options(self):
        return os.path.join(self.dir, "retroarch-core-options.cfg")

    @property
    def system(self):
        return os.path.join(self.dir, "system")

    @property
    def saves(self):
        return os.path.join(self.dir, "saves")

    @property
    def screenshots(self):
        return os.path.join(self.dir, "screenshots")

    @property
    def log(self):
        return os.path.join(self.dir, "retroarch.log")

    @property
    def lock(self):
        """Per-instance driver lock + pid file.

        Two agents on one instance means two emulators on ONE private SD, which is the
        exact failure the lane exists to prevent. Created with O_EXCL so the check and
        the claim are one atomic step.
        """
        return os.path.join(self.dir, "driver.json")

    @property
    def port(self):
        # Deterministic per-instance port so two agents never collide.
        return BASE_PORT + (sum(ord(c) for c in self.name) % 100)

    def sdmc(self):
        """The core's virtual SD root under this instance's save dir.

        MEASURED, not assumed (the plan flagged this as "confirm in the spike"):
        core_settings.cpp appends "Azahar/" to GetSaveDir() to form the UserDir, and
        FileUtil::SetUserPath then puts the SD at <UserDir>/Azahar/sdmc — so the "Azahar"
        segment appears TWICE. Verified 2026-09-07 from the core's own
        archive_sdmc.cpp path errors:
          .../gdx-retro/a/saves/Azahar/Azahar/sdmc/3ds/gdiffuser/fzerox.o2r
        """
        return os.path.join(self.saves, "Azahar", "Azahar", "sdmc")

    def gdx_dir(self):
        return os.path.join(self.sdmc(), "3ds", "gdiffuser")


# --------------------------------------------------------- instance locking --
# Every live `retro run` is registered here so the atexit and signal handlers can take
# its RetroArch down with it. A driver that dies without running these (SIGKILL, or the
# whole session torn down) leaves an orphan, which is what `retro reap` is for.
_ACTIVE = []          # list of [Instance, child_pid or None]
_GUARDS_INSTALLED = False


def read_lock(inst):
    try:
        with open(inst.lock) as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def _write_lock(fd, inst, child_pid, ctx, run_dir):
    info = {"driver_pid": os.getpid(), "retro_pid": child_pid,
            "instance": inst.name, "track": (ctx or {}).get("track"),
            "run_dir": run_dir, "started": util.iso_now(),
            "host": os.uname().nodename}
    os.write(fd, (json.dumps(info) + "\n").encode())
    return info


def acquire_instance(inst, ctx, run_dir, out):
    """Claim the instance, or explain who owns it. Auto-reaps a dead driver's orphan."""
    os.makedirs(inst.dir, exist_ok=True)
    while True:
        try:
            fd = os.open(inst.lock, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o644)
        except OSError as e:
            if e.errno != errno.EEXIST:
                raise
            held = read_lock(inst)
            if held is None:
                # Unreadable or half-written lock: treat as stale and retry once.
                _drop_lock(inst)
                continue
            driver = held.get("driver_pid")
            if driver and util.pid_alive(driver):
                raise GdxError(
                    "instance %r is in use: driver pid %s (track %s) started %s, run %s.\n"
                    "  Pick another --instance, or omit --instance to get a unique one."
                    % (inst.name, driver, held.get("track"), held.get("started"),
                       held.get("run_dir")), util.EXIT_LOCK)
            # Driver is gone. Anything it left running is an orphan on THIS private SD,
            # and starting a second emulator on it would corrupt both runs' evidence.
            stray = held.get("retro_pid")
            if stray and util.pid_alive(stray):
                out.warn("instance %r: driver pid %s is dead but its RetroArch pid %s is "
                         "still running — reaping it before reusing the instance"
                         % (inst.name, driver, stray))
                kill_group(stray)
            else:
                out.warn("instance %r: clearing a stale lock from dead driver pid %s"
                         % (inst.name, driver))
            _drop_lock(inst)
            continue
        try:
            _write_lock(fd, inst, None, ctx, run_dir)
        finally:
            os.close(fd)
        _ACTIVE.append([inst, None])
        install_guards()
        return True


def set_child(inst, child_pid, ctx=None, run_dir=None):
    """Record the RetroArch pid in the lock so `reap` can find it after we die."""
    for entry in _ACTIVE:
        if entry[0].dir == inst.dir:
            entry[1] = child_pid
    held = read_lock(inst) or {}
    held.update({"retro_pid": child_pid, "driver_pid": os.getpid()})
    if ctx is not None:
        held.setdefault("track", ctx.get("track"))
    if run_dir is not None:
        held.setdefault("run_dir", run_dir)
    tmp = inst.lock + ".tmp"
    with open(tmp, "w") as f:
        json.dump(held, f)
        f.write("\n")
    os.replace(tmp, inst.lock)


def _drop_lock(inst):
    try:
        os.unlink(inst.lock)
    except OSError:
        pass


def release_instance(inst):
    for i, entry in enumerate(list(_ACTIVE)):
        if entry[0].dir == inst.dir:
            _ACTIVE.pop(i)
            break
    _drop_lock(inst)


def _gone(pid):
    """Dead for our purposes: reaped, or a zombie awaiting its parent's wait().

    A terminated child stays visible to kill(pid, 0) until it is reaped, so polling
    pid_alive alone makes a successful kill look like a failure and burns the whole
    grace period before a pointless SIGKILL.
    """
    if not util.pid_alive(pid):
        return True
    p = subprocess.run(["ps", "-o", "state=", "-p", str(pid)],
                       stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    return (p.stdout or "").strip().startswith("Z")


def kill_group(pid, grace=3.0):
    """SIGTERM then SIGKILL the child's whole process group.

    The child is started with start_new_session=True, so it leads its own group and one
    killpg takes down anything it spawned. Refuses to signal our OWN group, which would
    kill the driver (and the agent's shell) along with it.
    """
    if not pid or _gone(pid):
        return False
    if int(pid) == os.getpid():
        # Suicide guard. Reaching here means a caller mistook the driver for its child;
        # signalling ourselves would kill the driver (and the agent's shell) instead.
        return False
    try:
        pgid = os.getpgid(pid)
    except OSError:
        pgid = None
    mine = os.getpgid(0)
    # killpg only when the target really leads its own group (start_new_session worked).
    # If it somehow shares ours, signal the single pid so we never take the driver with it.
    use_group = pgid is not None and pgid != mine
    for sig in (signal.SIGTERM, signal.SIGKILL):
        try:
            if use_group:
                os.killpg(pgid, sig)
            else:
                os.kill(pid, sig)
        except OSError:
            return True
        deadline = time.time() + (grace if sig == signal.SIGTERM else 1.0)
        while time.time() < deadline:
            if _gone(pid):
                return True
            time.sleep(0.1)
    return _gone(pid)


def _cleanup_all():
    for inst, child in list(_ACTIVE):
        if child:
            kill_group(child, grace=2.0)
        _drop_lock(inst)
    del _ACTIVE[:]


def install_guards():
    """Kill our RetroArch on every exit path we can observe.

    atexit covers normal return and uncaught exceptions; the handlers cover the signals
    a supervisor actually sends. SIGKILL cannot be caught — that case is what the pid
    file and `retro reap` exist for.
    """
    global _GUARDS_INSTALLED
    if _GUARDS_INSTALLED:
        return
    _GUARDS_INSTALLED = True
    atexit.register(_cleanup_all)

    def handler(signum, frame):
        _cleanup_all()
        signal.signal(signum, signal.SIG_DFL)
        os.kill(os.getpid(), signum)

    for sig in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
        try:
            signal.signal(sig, handler)
        except (ValueError, OSError):
            pass          # not the main thread, or not supported


# ------------------------------------------------------- process discovery --
_CFG_RE = re.compile(r"-c\s+(\S[^\n]*?)/([^/]+)/retroarch\.cfg")


# ---- host gate -------------------------------------------------------------------
# Several agents share one Mac. Ten concurrent instances took the 1-minute load average
# to 111 on 2026-09-23: at that point boot SIGBUS becomes common and every ms measured is
# host contention rather than guest cost. Both limits are advisory defaults, overridable
# for a deliberate stress test.
MAX_CONCURRENT = int(os.environ.get("GDX_RETRO_SLOTS", "3"))
MAX_LOADAVG = float(os.environ.get("GDX_RETRO_MAXLOAD", "40"))
GATE_TIMEOUT = float(os.environ.get("GDX_RETRO_GATE_TIMEOUT", "300"))


def host_load():
    try:
        return os.getloadavg()[0]
    except (OSError, AttributeError):
        return 0.0


def wait_for_host(out=None, poll=20.0, timeout=None):
    """Block until the host can take another instance. Returns the load we launched at.

    A caller that wants the old unthrottled behaviour sets GDX_RETRO_SLOTS=0.
    """
    if MAX_CONCURRENT <= 0:
        return host_load()
    deadline = time.time() + (GATE_TIMEOUT if timeout is None else timeout)
    said = False
    while True:
        live = len([p for p in our_retro_processes() if not _gone(p["pid"])])
        load = host_load()
        # Instance count is the hard limit — that is the resource emulator runs actually
        # contend for. Load is advisory: with a dozen agents compiling, the 1-minute average
        # sits high for reasons no emulator run can wait out, so back off briefly and then
        # proceed, recording the load rather than stalling the night. Timing taken above the
        # threshold is discardable; a parity run blocked for half an hour is just lost work.
        if live < MAX_CONCURRENT and load <= MAX_LOADAVG:
            return load
        if time.time() >= deadline:
            if out:
                why = ("%d instance(s) still live" % live if live >= MAX_CONCURRENT
                       else "load %.1f above %.1f" % (load, MAX_LOADAVG))
                out.say("host gate: %s after %.0fs — launching anyway. Parity from this run is "
                        "valid; any ms or fps figure is host contention, not guest cost."
                        % (why, GATE_TIMEOUT))
            return load
        if out and not said:
            out.say("host gate: waiting (live=%d/%d, load=%.1f/%.1f) — another agent is running"
                    % (live, MAX_CONCURRENT, load, MAX_LOADAVG))
            said = True
        time.sleep(poll)


def our_retro_processes():
    """Every RetroArch on this machine that gdx-dev started, by instance name.

    Identified from the `-c .../gdx-retro/<name>/retroarch.cfg` argument, so a RetroArch
    the user launched by hand is never a candidate for reaping.
    """
    p = subprocess.run(["ps", "-o", "pid,ppid,etime,command", "-ax"],
                       stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    out = []
    for line in (p.stdout or "").splitlines():
        if "RetroArch" not in line or INSTANCE_ROOT not in line:
            continue
        parts = line.split(None, 3)
        if len(parts) < 4 or not parts[0].isdigit():
            continue
        pid, ppid, etime, cmd = int(parts[0]), int(parts[1]), parts[2], parts[3]
        m = _CFG_RE.search(cmd)
        name = m.group(2) if m and m.group(1).endswith("gdx-retro") else None
        out.append({"pid": pid, "ppid": ppid, "etime": etime, "instance": name})
    return out


def instance_names():
    if not os.path.isdir(INSTANCE_ROOT):
        return []
    return sorted(d for d in os.listdir(INSTANCE_ROOT)
                  if os.path.isdir(os.path.join(INSTANCE_ROOT, d)))


def core_path():
    for p in (os.path.expanduser("~/Library/Application Support/RetroArch/cores/" + CORE_NAME),
              os.path.join(INSTANCE_ROOT, "cores", CORE_NAME)):
        if os.path.exists(p):
            return p
    return None


def ensure_core(out, prefer_nightly=False):
    p = core_path()
    if p:
        return p
    dest_dir = os.path.expanduser("~/Library/Application Support/RetroArch/cores")
    os.makedirs(dest_dir, exist_ok=True)
    urls = [CORE_NIGHTLY_URL, CORE_RELEASE_URL] if prefer_nightly \
        else [CORE_RELEASE_URL, CORE_NIGHTLY_URL]
    for url in urls:
        out.say("fetching core: %s" % url)
        try:
            tmp = os.path.join(dest_dir, "core-download.zip")
            urllib.request.urlretrieve(url, tmp)
            with zipfile.ZipFile(tmp) as z:
                member = next((n for n in z.namelist() if n.endswith(CORE_NAME)), None)
                if not member:
                    out.warn("%s contains no %s (members: %s)"
                             % (url, CORE_NAME, ", ".join(z.namelist()[:5])))
                    continue
                with z.open(member) as src, open(os.path.join(dest_dir, CORE_NAME), "wb") as dst:
                    shutil.copyfileobj(src, dst)
            os.unlink(tmp)
            out.say("installed %s" % os.path.join(dest_dir, CORE_NAME))
            return os.path.join(dest_dir, CORE_NAME)
        except Exception as e:            # network, zip, or layout surprise
            out.warn("core download failed (%s): %s" % (url, e))
    raise GdxError("no %s available (tried the 2126.0 release and the nightly)" % CORE_NAME,
                   util.EXIT_PREFLIGHT)


CFG_TEMPLATE = """\
# gdx-dev per-instance RetroArch config for instance %(name)s.
# The point of this file: savefile_directory + system_directory are what the Azahar core
# turns into its virtual SD root (core_settings.cpp ParseStorageOptions), so this file IS
# the private-SD mechanism. Never share one between instances.
savefile_directory = "%(saves)s"
savestate_directory = "%(saves)s"
system_directory = "%(system)s"
screenshot_directory = "%(shots)s"
core_options_path = "%(coreopts)s"
libretro_directory = "%(cores)s"
log_dir = "%(dir)s"
network_cmd_enable = "true"
network_cmd_port = "%(port)d"
video_fullscreen = "false"
video_windowed_fullscreen = "false"
pause_nonactive = "false"
menu_show_load_core = "false"
config_save_on_exit = "false"
log_verbosity = "true"
frontend_log_level = "0"
audio_enable = "%(audio)s"
video_driver = "%(video_driver)s"
%(extra)s"""

# `core_options_path` DOES NOT WORK, measured 2026-09-23 (three A/B boots, stereo lane).
# RetroArch 1.22 only consults it when `global_core_options` is true; otherwise it builds a
# PER-CORE path `<rgui_config_directory>/<core name>/<core name>.opt` and uses that. With
# the stock `rgui_config_directory` that is the SHARED
# ~/Library/Application Support/RetroArch/config/Azahar/Azahar.opt, so every instance's
# retroarch-core-options.cfg has been ignored and the core has been running on its compiled
# defaults ("Saved core options file to .../RetroArch/config/Azahar/Azahar.opt" in the
# frontend log; `GET_VARIABLE citra_layout_option = "default"` with `side_by_side` in the
# instance file). Pointing rgui_config_directory at the instance makes the file land — and
# because the same shared .opt is what every concurrent agent reads, redirecting it is also
# what keeps a --3d run from turning on 3D underneath somebody else's mono parity run.
#
# This redirection used to be applied ONLY under --3d/--model, which left every mono run
# reading AND (RetroArch rewrites the file on exit) WRITING that one shared file. With up to
# MAX_CONCURRENT instances doing that at once, a mono boot COULD be handed a torn or
# half-written Azahar.opt by a sibling instance exiting mid-boot. This was investigated
# (emufix, 2026-10-16) as the cause of the mono lane's intermittent host
# EXC_BAD_ACCESS/SIGBUS boot crash ("last guest frame ~38") -- it is NOT the (sole) cause:
# the crash reproduces under the fixed tooling too, with zero concurrent instances, matching
# docs/research/native/nimenu-progress.md §9.7's independent finding that it is a host-
# process RetroArch/MoltenVK boot race unrelated to guest code or config. This fix is worth
# having regardless: it is now applied on EVERY run, so mono gets the same private,
# per-instance options file --3d always had, and no lane ever touches the user's shared
# Azahar.opt again -- it just should not be expected to change the crash rate.
CFG_COREOPT_REDIRECT = 'rgui_config_directory = "%s"\n'
# Kept under its old name for any caller that still spells it that way.
CFG_3D_EXTRA = CFG_COREOPT_REDIRECT

# `--model`. The core declares `citra_is_new_3ds` as "System Model" ("Select whether to
# emulate the original 3DS or New 3DS. New 3DS has additional CPU power and memory"), and
# the legal values are these two labels -- verified against the pinned core's own option
# strings, not assumed. `--model old` is the Old 3DS lane (NEXT.md C4): the render thread
# self-disables there (gdx3ds_renderthread.cpp refuses without APT_CheckNew3DS), so an
# `old` run is a renderthread=0 execution NO MATTER what [debug] renderthread says --
# which makes it a different execution from every N3DS parity cell, never a comparison
# against one (PROTOCOL: parity is same-render-thread-mode only).
MODELS = {"new": "New 3DS", "old": "Old 3DS"}


def model_or_die(name):
    if name is None:
        return None
    key = str(name).strip().lower()
    if key not in MODELS:
        raise GdxError("unknown --model %r; expected one of: %s"
                       % (name, ", ".join(sorted(MODELS))), util.EXIT_USAGE)
    return key

# The ONE set of core-option values this lane writes, for every run. Spelled so every one
# of them is LEGAL for the core (verified against the pinned core's own option strings --
# see the README's core_options warning): `citra_is_new_3ds = "enabled"`,
# `citra_resolution_factor = "1x (Native)"` and `citra_layout_option = "Default Top-Bottom
# Screen"` were all illegal display-label guesses, not the core's actual value strings
# ("New 3DS"/"Old 3DS", "1", "default"), and an illegal value silently falls back to the
# option default instead of erroring -- which is the only reason nothing visibly broke
# before this file was ever actually read. New 3DS, 1x and the default layout happen to BE
# the defaults, so honouring this file changes only citra_graphics_api (which was always
# asked for the value it was already defaulting to) and, under --3d, the stereo keys below.
# This used to be two near-identical templates (one for mono, with the illegal values
# above; one for --3d/--model, with the legal ones) written to two different files --
# now there is one template and one piece of text written to both.
CORE_OPTIONS_TEMPLATE = """\
# Written by `gdx-dev retro run`. core_options_path is inert for this RetroArch (see the
# README's core_options warning); the file actually read is the one at the
# rgui_config_directory redirect (see CFG_COREOPT_REDIRECT), and every value here has to be
# one the core DECLARES or it silently falls back to the option default.
# citra_is_new_3ds is MANDATORY: the render thread is New3DS-only (port/3ds/gdx3ds_renderthread.cpp).
# It is SUBSTITUTED rather than appended to, because RetroArch's config parser returns the
# FIRST entry for a duplicated key -- appending a second assignment wrote a file that said
# "Old 3DS" and booted a New 3DS, which only the guest's own [mem-budget] receipt caught.
citra_is_new_3ds = "%(model)s"
citra_graphics_api = "%(gfx)s"
citra_use_hw_shader = "enabled"
citra_resolution_factor = "1"
citra_layout_option = "default"
citra_use_virtual_sd = "enabled"
citra_use_libretro_save_path = "LibRetro Default"
citra_region_value = "Auto"
"""


def write_config(inst, gfx="Vulkan", audio="false", video_driver="vulkan",
                 core=None, three_d=None, model=None):
    """Write this instance's retroarch.cfg + core-options file.

    `three_d` is (mode, depth) from `--3d`, or None. `model` is "new"/"old" from
    `--model`, or None (-> the core's own default, New 3DS).

    The per-instance `rgui_config_directory` redirect and options file are written on
    EVERY run now, mono included -- `core_options_path` does not work (README), so without
    it every instance falls through to the SHARED
    ~/Library/Application Support/RetroArch/config/Azahar/Azahar.opt, which up to
    MAX_CONCURRENT instances read and (RetroArch rewrites it on exit) WRITE at once. This
    was investigated (emufix, 2026-10-16) as the cause of the mono lane's intermittent host
    EXC_BAD_ACCESS/SIGBUS boot crash -- it reproduces under the fixed tooling too (and with
    zero concurrent instances, per nimenu-progress.md §9.7), so it is NOT the crash's
    (sole) cause; see the README's core_options warning. The values chosen (model defaults
    to New 3DS, gfx to whatever --gfx asked for) are the same ones the core was already
    defaulting to, so this changes what the lane RUNS ON (a private file instead of a
    contended shared one) without changing what it asks for -- except that --gfx, and
    --model, now actually reach the core, which they never did before. Confirmed
    behaviour-preserving by an exact `[fh]`/`[gstate]` comparison, old tooling vs this one,
    same binary, same deterministic seed (README).
    """
    for d in (inst.dir, inst.system, inst.saves, inst.screenshots):
        os.makedirs(d, exist_ok=True)
    cfg_dir = os.path.join(inst.dir, "config")
    with open(inst.cfg, "w") as f:
        f.write(CFG_TEMPLATE % {
            "name": inst.name, "saves": inst.saves, "system": inst.system,
            "shots": inst.screenshots, "coreopts": inst.core_options,
            "cores": os.path.dirname(core or core_path() or ""), "dir": inst.dir,
            "port": inst.port, "audio": audio, "video_driver": video_driver,
            "extra": CFG_COREOPT_REDIRECT % cfg_dir,
        })
    opt_text = CORE_OPTIONS_TEMPLATE % {"gfx": gfx, "model": MODELS[model or "new"]}
    if three_d:
        from . import stereo3d
        opt_text += ("# --3d: the HOST knob. Without it the emulated 3D slider reads 0 and\n"
                     "# the guest skips its right-eye pass whatever [stereo] enabled says.\n")
        opt_text += "".join('%s = "%s"\n' % kv for kv in stereo3d.core_options(*three_d))
    # core_options_path is inert (see CFG_COREOPT_REDIRECT), so this file is never read by
    # the core itself -- kept only so `retro setup` and _last_used() have something to show
    # / stat. The file RetroArch actually reads is the redirected Azahar.opt below, and it
    # gets the exact same text: one template, one value set, written to both places.
    with open(inst.core_options, "w") as f:
        f.write(opt_text)
    # The path RetroArch actually reads. "Azahar" is the core's `corename`, which is both
    # the directory and the file stem of a per-core options file (see CFG_COREOPT_REDIRECT).
    opt = os.path.join(cfg_dir, "Azahar", "Azahar.opt")
    os.makedirs(os.path.dirname(opt), exist_ok=True)
    with open(opt, "w") as f:
        f.write(opt_text)
    return inst.cfg


# `--3d` needs a core that declares citra_render_3d. The pinned 2126.0 release does not
# (stereo was never wired into its core_settings.cpp); the libretro nightly does. It is
# installed BESIDE the shared core, under the instance root, so a `--3d` run cannot change
# which emulator another agent's in-flight run is using.
CORE_3D_DIR = os.path.join(INSTANCE_ROOT, "cores-3d")


def core_3d_path():
    p = os.path.join(CORE_3D_DIR, CORE_NAME)
    return p if os.path.exists(p) else None


def ensure_core_3d(out):
    """-> path to a core that exposes citra_render_3d, installing the nightly if needed."""
    from . import stereo3d
    have = core_path()
    if have and stereo3d.core_exposes_3d(have):
        out.say("core exposes %s: %s" % (stereo3d.CORE_OPTION_KEY, have))
        return have
    p = core_3d_path()
    if p and stereo3d.core_exposes_3d(p):
        return p
    os.makedirs(CORE_3D_DIR, exist_ok=True)
    out.say("the pinned core has no %s option; fetching the nightly for --3d: %s"
            % (stereo3d.CORE_OPTION_KEY, CORE_NIGHTLY_URL))
    dest = os.path.join(CORE_3D_DIR, CORE_NAME)
    tmp = dest + ".download"
    try:
        urllib.request.urlretrieve(CORE_NIGHTLY_URL, tmp + ".zip")
        with zipfile.ZipFile(tmp + ".zip") as z:
            member = next((n for n in z.namelist() if n.endswith(CORE_NAME)), None)
            if not member:
                raise GdxError("nightly zip contains no %s" % CORE_NAME, util.EXIT_PREFLIGHT)
            with z.open(member) as src, open(tmp, "wb") as dst:
                shutil.copyfileobj(src, dst)
        os.chmod(tmp, 0o755)
        os.replace(tmp, dest)
        os.unlink(tmp + ".zip")
    except GdxError:
        raise
    except Exception as e:
        raise GdxError("could not install a 3D-capable core (%s): %s" % (CORE_NIGHTLY_URL, e),
                       util.EXIT_PREFLIGHT)
    if not stereo3d.core_exposes_3d(dest):
        raise GdxError("the nightly core at %s does not declare %s either — the RetroArch "
                       "lane cannot render 3D with it; use `gdx-dev emu run --3d` (Qt lane)"
                       % (dest, stereo3d.CORE_OPTION_KEY), util.EXIT_PREFLIGHT)
    out.say("installed 3D-capable core: %s" % dest)
    return dest


# The instance ini is seeded from the shared Azahar Qt SD so REAL user preferences carry
# over — border_mode, deadzone, stereo, audio, menu. Everything in [debug] and [native] is
# a lever or a run-control knob belonging to whoever used the Qt SD last, and gdx-dev can
# set any of them itself via --ini, so inheriting them means running a configuration
# nobody asked for. Measured on this machine: the shared ini carried `quit_at_tick = 3400`
# (every seeded run would stop at tick 3400), `input_replay`, `trectbatch`, `tmemfast`,
# `trifast`, and a whole [native] block with interp/machines/blendflush on.
#
# So the rule is by SECTION, not by a key list that has to be chased every time another
# track adds a lever: strip all of [debug] and [native], keep every other section.
# gdx-dev re-establishes the logging keys it needs (emu.MEASUREMENT_INI / the retro
# defaults) immediately afterwards.
LEVER_SECTIONS = ("debug", "native")


def strip_run_control(path):
    """Remove every inherited lever from a freshly seeded instance ini."""
    present = inifile.read(path)
    drop = [(sec, key) for sec in LEVER_SECTIONS for key in present.get(sec, {})]
    if not drop:
        return []
    lines = util.read_text(path).splitlines()
    out_lines, cur, removed = [], None, []
    for line in lines:
        m = inifile._SECTION_RE.match(line)
        if m:
            cur = m.group(1).strip()
            out_lines.append(line)
            continue
        km = inifile._KEY_RE.match(line)
        if km and cur is not None and (cur, km.group(2)) in drop:
            removed.append("%s.%s" % (cur, km.group(2)))
            continue
        out_lines.append(line)
    tmp = path + ".gdxtmp"
    with open(tmp, "w") as f:
        f.write("\n".join(out_lines).rstrip("\n") + "\n")
    os.replace(tmp, path)
    return removed


# preset-fix: `retro run --bare-ini`. Set by do_run; seed_sd then writes an EMPTY instance ini
# (the measurement keys and --ini still merge on top) instead of copying the Qt SD's user sections.
_BARE_INI = False


def seed_sd(inst, out, ini_settings=None):
    """Copy the o2r archives + ini into this instance's private SD.

    NEVER commit these: fzerox.o2r is the user's ROM-derived archive (PROTOCOL.md line 30).
    They are copied from the Azahar Qt SD, which is where the prebake stages them.
    """
    dst = inst.gdx_dir()
    os.makedirs(dst, exist_ok=True)
    copied = []
    for name in ("fzerox.o2r", "gdiffuser.o2r"):
        src = os.path.join(util.GDX_SDMC_DIR, name)
        if not os.path.exists(src):
            raise GdxError("no %s at %s — the Qt lane's SD is the source of these"
                           % (name, src), util.EXIT_STAGING)
        d = os.path.join(dst, name)
        if not os.path.exists(d) or os.path.getsize(d) != os.path.getsize(src):
            # Hardlink rather than copy: fzerox.o2r is 16 MB and the guest only reads it,
            # so a unique instance per run costs nothing. Falls back to a copy across
            # filesystems.
            try:
                if os.path.exists(d):
                    os.unlink(d)
                os.link(src, d)
            except OSError:
                shutil.copy2(src, d)
            copied.append(name)
    # Optional: the DSP dump. Without it ndspInit fails and the game runs silent, which
    # does not affect a boot/render proof. Never committed (PROTOCOL.md line 30).
    dsp_src = os.path.join(util.AZAHAR_SDMC, "3ds", "dspfirm.cdc")
    dsp_dst = os.path.join(os.path.dirname(dst), "dspfirm.cdc")
    if os.path.exists(dsp_src) and not os.path.exists(dsp_dst):
        shutil.copy2(dsp_src, dsp_dst)
        copied.append("dspfirm.cdc")
    src_ini = util.GDX_INI
    dst_ini = os.path.join(dst, "gdiffuser.ini")
    # Re-seed on EVERY run, not just the first. An instance ini persists, so a lever set by
    # an earlier run used to survive into the next one silently: an agent measuring a
    # "default" run got it carrying the previous run's fxceiling=2. A run's configuration
    # must be a function of its command line and nothing else.
    fresh = not os.path.exists(dst_ini)
    if _BARE_INI:
        with open(dst_ini, "w") as f:
            f.write("; gdx-dev retro run --bare-ini: no seeded user sections\n")
        if out:
            out.say("bare ini: the instance ini starts empty (no seeded [display]/[stereo]/[input]/... keys)")
    elif os.path.exists(src_ini):
        shutil.copy2(src_ini, dst_ini)
        stripped = strip_run_control(dst_ini)
        if fresh:
            copied.append("gdiffuser.ini")
        if stripped and out:
            out.say("dropped %d inherited run-control key(s) from the seeded ini: %s"
                    % (len(stripped), ", ".join(stripped)))
    elif not fresh:
        # No source ini to re-seed from: strip in place so levers still cannot carry over.
        stripped = strip_run_control(dst_ini)
        if stripped and out:
            out.say("cleared %d run-control key(s) left by a previous run: %s"
                    % (len(stripped), ", ".join(stripped)))
    if ini_settings:
        inifile.merge(dst_ini, ini_settings)
    if copied:
        out.say("seeded private SD %s: %s" % (dst, ", ".join(copied)))
    return dst


# ------------------------------------------------------------------ commands --
def send_command(inst, verb, timeout=1.0):
    """UDP network command (RetroArch command.h). stdlib socket, no extra process."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(timeout)
    try:
        s.sendto(verb.encode(), ("127.0.0.1", inst.port))
        try:
            data, _ = s.recvfrom(4096)
            return data.decode("utf-8", errors="replace").strip()
        except socket.timeout:
            return ""          # most verbs are fire-and-forget
    finally:
        s.close()


def do_setup(args, out, ctx):
    """Pre-create a NAMED instance. `retro run` does this itself for its own instance."""
    if not os.path.exists(RETRO_BIN):
        raise GdxError("RetroArch not installed at %s (brew install --cask retroarch)"
                       % RETRO_BIN, util.EXIT_PREFLIGHT)
    core = ensure_core(out)
    inst = Instance(getattr(args, "instance", None) or "a")
    cfg = write_config(inst)
    seed_sd(inst, out)
    out.table(["field", "value"], [
        ["instance", inst.name], ["config", cfg], ["core", core],
        ["private SD", inst.sdmc()], ["udp port", inst.port],
        ["log", inst.log], ["screenshots", inst.screenshots],
    ])
    out.emit({"instance": inst.name, "cfg": cfg, "core": core, "port": inst.port,
              "sdmc": inst.sdmc()})
    return util.EXIT_OK


def do_cmd(args, out, ctx):
    inst = Instance(getattr(args, "instance", None) or "a")
    reply = send_command(inst, args.verb)
    out.say("sent %s to %s:%d%s" % (args.verb, "127.0.0.1", inst.port,
                                    (" -> " + reply) if reply else ""))
    out.emit({"verb": args.verb, "port": inst.port, "reply": reply})
    return util.EXIT_OK


# Set by _attempt so do_run can tell the verdict whether the emulator was still alive
# when the clock ran out — a core that died mid-replay must not report PASS.
_LAST_ATTEMPT = {}

# One boot is retried this many times before moving to the next renderer.
EARLY_CRASH_ATTEMPTS = 3          # i.e. the initial try plus 2 retries

# Boot serialisation. With several agents launching at once, nearly every RetroArch crash on
# this machine (56 of 56 DiagnosticReports on 2026-10-20) was a SIGSEGV/SIGBUS inside the
# core's Vulkan/MoltenVK startup (vulkan_lock_queue, vkQueueSubmit, vkCreateImageView), all
# within 20 s of launch. Each one is retried here, but macOS still puts up "RetroArch quit
# unexpectedly" for it. So only one instance boots at a time: the machine-wide lock is taken
# just before Popen and released as soon as the guest heartbeat advances (past the
# VulkanWorker crash window, guest frame ~35-40), the process exits, or BOOT_LOCK_MAX_S
# passes. Runs still overlap once booted.
BOOT_LOCK_PATH = os.path.join(INSTANCE_ROOT, ".boot.lock")
BOOT_LOCK_MAX_S = 45.0
BOOT_LOCK_WAIT_S = 600.0


class BootLock:
    def __init__(self, out=None):
        self.fd = None
        self.out = out
        self.t_acquired = 0.0

    def acquire(self):
        os.makedirs(INSTANCE_ROOT, exist_ok=True)
        self.fd = os.open(BOOT_LOCK_PATH, os.O_RDWR | os.O_CREAT, 0o644)
        t0 = time.time()
        warned = False
        while True:
            try:
                fcntl.flock(self.fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                break
            except OSError as e:
                if e.errno not in (errno.EAGAIN, errno.EWOULDBLOCK):
                    raise
            if not warned and self.out:
                self.out.say("waiting for another instance to finish booting (boot lock)")
                warned = True
            if time.time() - t0 > BOOT_LOCK_WAIT_S:
                # A stuck holder must not wedge every run: boot unserialised instead.
                if self.out:
                    self.out.warn("boot lock still held after %.0fs; booting anyway" % BOOT_LOCK_WAIT_S)
                os.close(self.fd)
                self.fd = None
                return
            time.sleep(1.0)
        self.t_acquired = time.time()

    def expired(self):
        return self.fd is not None and time.time() - self.t_acquired > BOOT_LOCK_MAX_S

    def release(self):
        if self.fd is None:
            return
        try:
            fcntl.flock(self.fd, fcntl.LOCK_UN)
        finally:
            os.close(self.fd)
            self.fd = None

# `--seconds` is a CEILING, not a schedule: the run ends when the guest ends.
DEFAULT_SECONDS = 60              # no replay: this is a boot smoke test
# NEXT.md B45: a staged replay is not a 60-second thing, so 60 could only ever cut one
# short — and it did, on every corpus replay, while printing RESULT FAIL. The canonical
# replay is 6002 ticks = ~100 s of GUEST time, which has measured 240-420 s of WALL time
# on this host depending on load (residuals2-progress.md). 300 s is the lane's own
# timeout (PROTOCOL.md). With replay_quit implied (B19) the run leaves the frame loop at
# the replay's last tick, so this ceiling costs nothing when the replay behaves and only
# bounds the run when it does not.
REPLAY_SECONDS = int(os.environ.get("GDX_RETRO_REPLAY_SECONDS", "300"))


def resolve_seconds(args, replay_block):
    """-> (seconds, why). An explicit --seconds always wins."""
    asked = getattr(args, "seconds", None)
    if asked:
        return int(asked), "--seconds %d" % int(asked)
    if replay_block:
        return REPLAY_SECONDS, ("default for a staged replay (a 60 s ceiling truncates "
                                "every corpus replay; NEXT.md B45)")
    return DEFAULT_SECONDS, "default for a run with no replay"


def build_settings(args, three_d=None):
    """The guest ini this invocation asks for. -> (settings, explicit keys).

    `explicit` is what the command line NAMED, and nothing implied may overwrite it
    (NEXT.md B19: `--no-replay-quit` and `--ini debug.replay_quit=0` must still win).
    """
    settings = {}
    if three_d:
        # Guest-side defaults --3d implies ([stereo] enabled, [debug] fh_eye=both); an
        # explicit --ini below overrides either of them.
        from . import stereo3d
        settings.update(stereo3d.guest_settings())
    explicit = set()
    for spec in (getattr(args, "ini", None) or []):
        section, key, value = inifile.parse_spec(spec)
        settings[(section, key)] = value
        explicit.add((section, key))
    if getattr(args, "no_replay_quit", False):
        settings[replay.REPLAY_QUIT_KEY] = "0"
        explicit.add(replay.REPLAY_QUIT_KEY)
    # Same measurement set as the Qt lane, so a brief's --ini means the same thing in
    # both. Needed explicitly now that strip_run_control clears ALL of [debug] from a
    # seeded ini — nothing is inherited, so every key we rely on must be set here.
    from . import emu as _emu
    for k, v in _emu.MEASUREMENT_INI.items():
        settings.setdefault(k, v)
    return settings, explicit


def cmd_extra(args):
    """Frontend flags that do not depend on the renderer."""
    extra = []
    if getattr(args, "frames", None):
        # --max-frames counts FRONTEND frames, which race far ahead of guest time while
        # the guest is still loading (measured: 2400 frontend frames burned in ~1.6 s of
        # guest time, so the run ended mid-bootproc and the watchdog never got to beat).
        # Use it only for a deterministic bound; --seconds is the default.
        extra += ["--max-frames", str(args.frames), "--max-frames-ss"]
    return extra


def _attempt(inst, gfx, core, threedsx, extra, seconds, settings, rd, ctx, out, attempt,
             three_d=None, model=None):
    """One boot. -> (ok, early_crash, guest_log_text, detail).

    `early_crash` means RetroArch died on its own before the deadline AND the guest never
    produced a [watchdog] beat — the core crashing on the way up, which is worth retrying.
    A run that stayed up but wrote nothing is a different problem and falls through to the
    next renderer instead.
    """
    video_driver = {"Vulkan": "vulkan", "OpenGL": "glcore", "Software": "gl"}.get(gfx, "gl")
    write_config(inst, gfx=gfx, video_driver=video_driver, core=core, three_d=three_d,
                 model=model)
    seed_sd(inst, out, settings)
    suppress_restore_prompt(out)
    for stale in (inst.log, rd.sub("log-%s.txt" % inst.name)):
        if os.path.exists(stale):
            os.unlink(stale)
    guest_log = os.path.join(inst.gdx_dir(), "log.txt")
    if os.path.exists(guest_log):
        os.unlink(guest_log)
    _BOOT_STARTED["t"] = time.time()

    cmd = [RETRO_BIN, "-c", inst.cfg, "-L", core, threedsx, "-v", "--log-file", inst.log]
    cmd += list(extra)
    if "--max-frames-ss" in cmd:
        cmd += ["--max-frames-ss-path",
                os.path.join(inst.screenshots, "%s-end.png" % inst.name)]
    out.say("citra_graphics_api=%s (video_driver=%s, attempt %d/%d)"
            % (gfx, video_driver, attempt, EARLY_CRASH_ATTEMPTS))

    console = open(rd.sub("console-%s-%s-%d.txt" % (inst.name, gfx.lower(), attempt)), "w")
    boot_lock = BootLock(out)
    boot_lock.acquire()
    try:
        # start_new_session: the child leads its own process group, so one killpg takes down
        # it and anything it spawns even if our own shell is gone.
        proc = subprocess.Popen(cmd, stdout=console, stderr=subprocess.STDOUT,
                                start_new_session=True)
    except BaseException:
        boot_lock.release()
        raise
    set_child(inst, proc.pid, ctx, rd.path)
    t0 = time.time()
    deadline = t0 + seconds
    rc = None
    guest_done = None
    while time.time() < deadline:
        time.sleep(2)
        rc = proc.poll()
        if rc is not None:
            break
        _retro_snapshot(rd, inst)
        if boot_lock.fd is not None:
            beats = prof.parse_text(util.read_text(rd.sub("log-%s.txt" % inst.name))).watchdog
            if len({b["frame"] for b in beats}) >= 2 or boot_lock.expired():
                boot_lock.release()
        # Same early return as the Qt lane: the core can outlive the guest, so waiting on
        # the process alone burns the rest of --seconds after the run is already over.
        guest_done = emu_guest_exit_reason(util.read_text(rd.sub("log-%s.txt" % inst.name)))
        if guest_done:
            out.say("guest exited after %.0fs (%s) — stopping early"
                    % (time.time() - t0, guest_done))
            break
    # A guest that exited on purpose is not the core dying, so it must not count as
    # "the emulator was gone at the end" when the verdict judges completion.
    exited_early = rc is not None and not guest_done
    # NEXT.md B45: "the clock ran out" and "the thing died" are different facts and were
    # reported identically. rc is None and no guest exit receipt == the deadline expired
    # with everything still alive, i.e. a TIMEOUT.
    deadline_hit = rc is None and not guest_done
    _LAST_ATTEMPT["exited_early"] = exited_early
    _LAST_ATTEMPT["deadline_hit"] = deadline_hit
    _LAST_ATTEMPT["guest_exit"] = guest_done
    if rc is None:
        # Time-bounded run: grab the end screenshot over UDP before quitting, which is
        # what replaces --max-frames-ss (and the Qt lane's wid.swift hack).
        reply = send_command(inst, "GET_STATUS")
        if reply:
            out.say("status: %s" % reply)
        send_command(inst, "SCREENSHOT")
        time.sleep(2)
        send_command(inst, "QUIT")
        time.sleep(3)
        if proc.poll() is None:
            kill_group(proc.pid)
    boot_lock.release()
    console.close()
    set_child(inst, None, ctx, rd.path)
    _retro_snapshot(rd, inst)

    sd_log = util.read_text(rd.sub("log-%s.txt" % inst.name))
    console_path = rd.sub("console-%s-%s-%d.txt" % (inst.name, gfx.lower(), attempt))
    secs = time.time() - t0

    if not sd_log.strip():
        # The guest never even opened its log: it died before filelog init, or the core
        # never handed it control. Measured as transient under machine load, so it is
        # retried like any other early crash rather than immediately blaming the renderer.
        crash = _crash_signature(console_path)
        return False, True, sd_log, ("no guest log.txt under %s with "
                                     "citra_graphics_api=%s after %.0fs%s"
                                     % (inst.gdx_dir(), gfx, secs, crash))

    # "Booted" means the heartbeat ADVANCED, not merely that one beat appeared. The core's
    # VulkanWorker crash lands around guest frame 35-40, which is after the first beat, so
    # a single-beat log is exactly the crash we are retrying — an earlier version of this
    # check accepted it as success and the retry never fired.
    beats = prof.parse_text(sd_log).watchdog
    advanced = len({b["frame"] for b in beats}) >= 2
    # A guest that EXITED ON PURPOSE (replay_quit / quit_at_tick) ran; it was not the
    # VulkanWorker crash. probeboot (420 ticks) quits before its second ~300-frame beat,
    # and used to be retried three times and scored FAIL with every SHOT captured.
    if advanced or guest_done:
        out.say("guest log reached the private SD (%d bytes) with %s" % (len(sd_log), gfx))
        return True, False, sd_log, ""

    crash = _crash_signature(console_path)
    where = (" (last guest frame %d)" % beats[-1]["frame"]) if beats else ""
    detail = ("guest heartbeat never advanced after %.0fs%s%s"
              % (secs, where, crash))
    if exited_early:
        detail = "RetroArch exited after %.0fs%s%s" % (secs, where, crash)
    return False, True, sd_log, detail


# Deliberately NOT matching a bare "signal <n>": Azahar's APT tracing logs
# "Sending parameter ... with signal 00000001" on every healthy boot, which made the
# crash reporter cry wolf on runs that never crashed.
_CRASH_RE = re.compile(r"(SIGBUS|SIGSEGV|SIGILL|SIGABRT|Segmentation fault|Bus error)",
                       re.IGNORECASE)


def _crash_signature(console_path):
    text = util.read_text(console_path, limit=200000)
    m = _CRASH_RE.search(text)
    if not m:
        return ""
    worker = " on VulkanWorker" if "VulkanWorker" in text else ""
    return " (%s%s)" % (m.group(1), worker)


def _three_d_or_none(args):
    """--3d -> (mode, depth), or None when the flag was not given."""
    if not getattr(args, "three_d", None):
        return None
    from . import stereo3d
    return (stereo3d.mode_or_die(args.three_d),
            stereo3d.depth_or_die(getattr(args, "depth_3d", None)
                                  or stereo3d.DEFAULT_DEPTH))


def do_run(args, out, ctx):
    global _BARE_INI
    dry = bool(getattr(args, "dry_run", False))
    _BARE_INI = bool(getattr(args, "bare_ini", False))
    if not os.path.exists(RETRO_BIN):
        # A dry run reports the missing frontend instead of refusing: saying what WOULD
        # happen is exactly what the flag is for, and it needs no frontend to say it.
        if not dry:
            raise GdxError("RetroArch not installed at %s (brew install --cask retroarch)"
                           % RETRO_BIN, util.EXIT_PREFLIGHT)
        out.warn("RetroArch is not installed at %s — a real run would fail preflight"
                 % RETRO_BIN)
    threedsx = os.path.abspath(args.threedsx)
    if not os.path.isfile(threedsx):
        raise GdxError("no such .3dsx: %s" % threedsx, util.EXIT_USAGE)

    # --3d: a 3D-capable core (the nightly, installed beside the shared one) plus the
    # citra_render_3d / citra_factor_3d core options. Without the flag nothing below
    # changes and the pinned release core is used exactly as before.
    three_d = _three_d_or_none(args)
    # --model: the emulated system model (NEXT.md C4). None keeps the EFFECTIVE model what
    # it always was -- New 3DS, the core's own compiled default -- by writing that value
    # explicitly rather than leaving it unset (emufix, 2026-10-16: the redirect itself is
    # now written on every run, see write_config).
    model = model_or_die(getattr(args, "model", None))
    if dry:
        # ensure_core() DOWNLOADS and unzips. "Change nothing" means nothing.
        core = core_3d_path() if three_d else core_path()
    elif three_d:
        core = ensure_core_3d(out)
    else:
        core = ensure_core(out)

    want_gfx = normalise_gfx(getattr(args, "gfx", None))
    gfx_list = [want_gfx] if want_gfx else GFX_ORDER
    settings, explicit = build_settings(args, three_d)
    if dry:
        # NEXT.md B45: this used to fall straight through and boot a real instance. The
        # flag that exists to be safe was the one that was not, so the dry-run return is
        # the FIRST thing after the arguments are understood and BEFORE anything that
        # writes, downloads, locks, or launches.
        return _dry_run(args, out, ctx, threedsx, core, gfx_list, settings, explicit,
                        three_d, model)

    suppress_restore_prompt(out)
    load_at_launch = wait_for_host(out)
    rd = runs.create(ctx["run_root"], ctx["track"], getattr(args, "name", None) or "retro")
    out.say("run dir: %s" % rd.path)
    t0 = time.time()

    # Default the instance to the run id, which is unique. A fixed default meant two
    # agents that both omitted --instance shared one private SD — and so did an orphan
    # from a killed driver and the live run that reused the name.
    inst = Instance(getattr(args, "instance", None) or rd.run_id)
    acquire_instance(inst, ctx, rd.path, out)
    out.say("instance %s (udp %d, private SD %s)" % (inst.name, inst.port, inst.sdmc()))

    # Stage the replay into THIS INSTANCE's private SD, by exactly the same rules
    # `emu run` uses — a .txt to the SD root as gdx-autoinput.txt, a .gdxrec to
    # rec/<name>.gdxrec plus [debug] input_replay=<name>. The ini keys have to be in
    # `settings` before the seed_sd() call below, which is what writes the instance ini.
    replay_block = None
    if getattr(args, "replay", None):
        replay_block, extra = replay.stage(args.replay, sdmc_root=inst.sdmc(),
                                           gdx_dir=inst.gdx_dir(), out=out)
        settings.update(extra)
        # NEXT.md B19: a staged replay ENDS the run at its last tick unless the
        # invocation said otherwise. `explicit` is what the command line named, so
        # --no-replay-quit / --ini debug.replay_quit=0 survive this.
        replay.apply_implied(settings, explicit, kind=replay_block.get("kind"), out=out,
                             last=replay_block.get("last_tick"), private_sd=True)
    else:
        replay.stale_autoinput_warning(out, sdmc_root=inst.sdmc())

    # The ceiling can only be chosen once the replay is known: a staged replay needs
    # minutes, a bare boot needs a minute (NEXT.md B45).
    seconds, seconds_why = resolve_seconds(args, replay_block)
    out.say("--seconds ceiling: %d s (%s)" % (seconds, seconds_why))

    last_error = None
    attempts_log = []
    # The azahar-libretro core intermittently takes a SIGBUS/SIGSEGV on its VulkanWorker
    # during boot (integration-verify saw it at guest frames 35 and 39, lever-independent).
    # It is a crash on the way up, not a property of the build under test, so retry the
    # same renderer before blaming it or falling back.
    for gfx in gfx_list:
        for attempt in range(1, EARLY_CRASH_ATTEMPTS + 1):
            ok, early, sd_log, why = _attempt(inst, gfx, core, threedsx, cmd_extra(args),
                                              seconds, settings, rd, ctx, out, attempt,
                                              three_d=three_d, model=model)
            alive_at_end = not _LAST_ATTEMPT.get("exited_early", False)
            attempts_log.append({"gfx": gfx, "attempt": attempt, "ok": ok,
                                 "early_crash": early, "detail": why})
            if ok:
                last_error = None      # a successful retry must not inherit the
                break                  # earlier attempt's failure text in VERDICT
            last_error = why
            if not early:
                break        # a clean run that simply produced nothing: try the next gfx
            if attempt < EARLY_CRASH_ATTEMPTS:
                out.warn("early crash with %s (attempt %d/%d): %s — retrying"
                         % (gfx, attempt, EARLY_CRASH_ATTEMPTS, why))
        if ok:
            break
        out.warn("giving up on citra_graphics_api=%s: %s" % (gfx, why))
    used_gfx = next((a["gfx"] for a in reversed(attempts_log) if a.get("ok")), None)
    if used_gfx and want_gfx and used_gfx != want_gfx:
        out.warn("renderer fell back to %s (asked for %s)" % (used_gfx, want_gfx))
    elif used_gfx and not want_gfx and used_gfx != GFX_ORDER[0]:
        # Frame hashes are renderer-specific, so a silent fallback turns an A/B into a
        # comparison of two renderers. Say so; `gdx-dev prof --ab` and fhdiff also refuse
        # to compare runs whose `platform.gfx` differ.
        out.warn("renderer fell back to %s (default is %s) — do NOT compare frame hashes "
                 "or timings from this run against a %s run"
                 % (used_gfx, GFX_ORDER[0], GFX_ORDER[0]))

    manifest = runs.new_manifest(
        run_id=rd.run_id, track=ctx["track"], lane="retro", started=util.iso_now(),
        ended=util.iso_now(), wall_s=round(time.time() - t0, 1),
        build=buildmod.build_info_for_manifest(ctx["root"],
                                               getattr(args, "build_dir", "build-3ds"),
                                               threedsx),
        # NEXT.md B46 / PROTOCOL.md: a parity cell is a claim about a tree. The branch
        # point goes in the manifest so no number can be quoted without it.
        tree=buildmod.tree_info(ctx["root"]),
        ini={"%s.%s" % k: v for k, v in settings.items()}, replay=replay_block,
        gputrace=inifile.truthy(settings.get(("debug", "gputrace"), "0")), seconds=seconds,
        platform={"host": "darwin-arm64", "azahar": None, "gfx": used_gfx,
                  "gfx_requested": want_gfx or GFX_ORDER[0],
                  "retroarch": retroarch_version(),
                  # The manifest's `model` is a CLAIM about which console was emulated, so
                  # it has to follow --model. An Old 3DS run mislabelled emu-new3ds would
                  # be compared against New 3DS cells by anything that reads this field.
                  "model": "emu-old3ds" if model == "old" else "emu-new3ds"},
        result={"verdict": "UNKNOWN", "fatals": 0, "max_frame": None,
                "distinct_frames": 0, "errors": {}},
        artifacts={"log": "log-%s.txt" % inst.name, "emu_log": "retroarch-%s.log" % inst.name,
                   "shots": sorted(os.listdir(rd.sub("shots")))})
    if three_d:
        from . import stereo3d
        # Which emulator drew this, and in which 3D mode: two --3d runs are only
        # comparable to each other, never to a release-core mono run.
        manifest["stereo3d"] = stereo3d.retro_receipt(three_d[0], three_d[1], core)

    from . import emu as emumod
    sd_text = util.read_text(rd.sub("log-%s.txt" % inst.name))
    ra_text = util.read_text(rd.sub("retroarch-%s.log" % inst.name))
    result = emumod.judge(ra_text, sd_text, out,
                          replay_kind=(replay_block or {}).get("kind"),
                          replay_block=replay_block,
                          emu_alive_at_end=alive_at_end,
                          deadline_hit=_LAST_ATTEMPT.get("deadline_hit"),
                          seconds=seconds)
    manifest["result"] = {k: result[k] for k in
                          ("verdict", "fatals", "max_frame", "distinct_frames", "errors",
                           "replay_complete", "shots_expected", "shots_seen",
                           "missing_shots", "timed_out")}
    # Host contention is the difference between a profile and a fiction: record it so a
    # reader can throw the run out rather than trust it (2026-09-23, load hit 111).
    manifest["host"] = {"loadavg_at_launch": round(load_at_launch, 2),
                        "loadavg_at_end": round(host_load(), 2),
                        "slots": MAX_CONCURRENT, "max_loadavg": MAX_LOADAVG}
    manifest["attempts"] = attempts_log
    rd.write_manifest(manifest)
    rd.write_verdict(result["verdict"], result["detail"] + ("; " + last_error if last_error else ""))
    with open(rd.sub("RESULT"), "w") as f:
        f.write(emumod.result_line(manifest, result) + "\n")
    out.say(emumod.result_line(manifest, result))
    out.say(buildmod.tree_receipt(manifest["tree"]))
    stale = buildmod.tree_warning(manifest["tree"])
    if stale:
        out.warn(stale)
    out.table(["field", "value"], [["run", rd.run_id], ["verdict", result["verdict"]],
                                   ["tree", buildmod.tree_receipt(manifest["tree"])],
                                   ["detail", result["detail"]], ["instance", inst.name],
                                   ["replay complete", result["replay_complete"]],
                                   ["SHOTs", "%s/%s" % (result["shots_seen"],
                                                        result["shots_expected"])],
                                   ["wall_s", manifest["wall_s"]],
                                   ["artifact sha256",
                                    (manifest["build"] or {}).get("sha256")],
                                   ["dir", rd.path]])
    out.emit(manifest)
    release_instance(inst)
    return util.EXIT_OK if result["verdict"] == "PASS" else util.EXIT_FAIL


def _dry_run(args, out, ctx, threedsx, core, gfx_list, settings, explicit, three_d,
             model=None):
    """`--dry-run`: say exactly what a real run would do, and do none of it.

    NEXT.md B45. This flag existed, was documented, was parsed — and `retro run` never
    read it, so `--dry-run` booted a real RetroArch instance on a private SD. The worst
    kind of harness bug: the flag that exists to be safe is the one that is not. Every
    side effect a real run has is listed here as a WOULD, and the function reaches none
    of them: no core download, no instance lock, no SD seed, no ini write, no launch,
    not even a run directory.
    """
    inst = Instance(getattr(args, "instance", None) or "<run-id>")
    replay_block, extra = (None, {})
    if getattr(args, "replay", None):
        # dry_run=True: resolves, VALIDATES and reports the replay, copies nothing.
        replay_block, extra = replay.stage(args.replay, sdmc_root=inst.sdmc(),
                                           gdx_dir=inst.gdx_dir(), dry_run=True, out=out)
        settings.update(extra)
        replay.apply_implied(settings, explicit, kind=replay_block.get("kind"), out=out,
                             last=replay_block.get("last_tick"), private_sd=True)
    seconds, seconds_why = resolve_seconds(args, replay_block)

    # No core installed yet (a fresh host, or a Linux CI runner): report where a real run
    # would download it, and the "core" row below marks it NOT INSTALLED.
    core = core or os.path.expanduser("~/Library/Application Support/RetroArch/cores/"
                                      + CORE_NAME)
    cmd = [RETRO_BIN, "-c", inst.cfg, "-L", core, threedsx, "-v", "--log-file", inst.log]
    cmd += list(cmd_extra(args))
    tree = buildmod.tree_info(ctx["root"])

    out.say("DRY RUN — nothing below was launched, written, downloaded or locked.")
    out.say("")
    out.table(["would", "value"], [
        ["launch", " ".join(cmd)],
        ["renderers, in order", ", ".join(gfx_list)],
        ["instance", inst.name + ("" if getattr(args, "instance", None)
                                  else " (defaulted to the run id, unique per run)")],
        ["private SD", inst.sdmc()],
        ["udp port", inst.port],
        ["core", "%s%s" % (core, "" if os.path.exists(core) else "  (NOT INSTALLED — a "
                                                                "real run downloads it)")],
        ["seconds ceiling", "%d s (%s)" % (seconds, seconds_why)],
        ["replay", "%s (%s, last tick %s, %d SHOT(s))"
         % (replay_block["id"], replay_block["kind"], replay_block["last_tick"],
            len(replay_block["shots"])) if replay_block else "none"],
        ["host gate", "wait for < %d live instance(s) and load <= %.0f (now %.2f)"
         % (MAX_CONCURRENT, MAX_LOADAVG, host_load())],
        ["tree", buildmod.tree_receipt(tree)],
        ["system model", "%s%s" % (MODELS.get(model, "New 3DS"),
                                   "" if model else "  (core default, written explicitly "
                                                    "via the per-instance redirect)")],
    ])
    out.say("")
    out.say("would seed this guest ini (every [debug]/[native] key stripped first):")
    out.table(["section.key", "value", "source"],
              [["%s.%s" % k, settings[k],
                "--ini" if k in explicit else
                ("--replay" if k in extra else
                 ("implied by --replay" if k in (replay.REPLAY_QUIT_KEY, replay.QUIT_AT_TICK_KEY)
                  and replay_block
                  else "lane default"))]
               for k in sorted(settings)])
    stale = buildmod.tree_warning(tree)
    if stale:
        out.warn(stale)
    out.emit({"dry_run": True, "cmd": cmd, "instance": inst.name, "sdmc": inst.sdmc(),
              "seconds": seconds, "seconds_why": seconds_why, "gfx": gfx_list,
              "model": model,
              "replay": replay_block, "tree": tree,
              "ini": {"%s.%s" % k: v for k, v in settings.items()}})
    return util.EXIT_OK


def do_reap(args, out, ctx):
    """Kill RetroArch processes whose driver is gone, and clear their pid files.

    Two independent orphan signals, because either can happen alone: a process whose
    parent died (reparented to pid 1), and an instance whose lock names a dead driver.
    A process with a LIVE driver is never touched — other tracks run on this machine.
    """
    procs = our_retro_processes()
    claimed = {}
    for name in instance_names():
        held = read_lock(Instance(name))
        if held:
            claimed[name] = held

    # Orphan evidence must be POSITIVE. "No lock file" is not evidence: processes started
    # before this fix shipped have no lock and perfectly live drivers, and killing those
    # would destroy another track's run. Only two things prove the driver is gone:
    #   - the process was reparented to pid 1, or
    #   - a lock file names a driver pid that is dead.
    victims, spared = [], []
    for pr in procs:
        held = claimed.get(pr["instance"])
        driver = (held or {}).get("driver_pid")
        driver_alive = bool(driver) and util.pid_alive(driver)
        if pr["ppid"] == 1:
            victims.append((pr, "reparented to pid 1"))
        elif held is not None and not driver_alive:
            victims.append((pr, "driver pid %s dead" % driver))
        elif driver_alive:
            spared.append((pr, "driver pid %s alive" % driver))
        else:
            spared.append((pr, "parent pid %s alive, no lock (pre-fix run)" % pr["ppid"]))

    stale_locks = [n for n, h in claimed.items()
                   if not (h.get("driver_pid") and util.pid_alive(h["driver_pid"]))]

    if spared:
        out.table(["kept pid", "instance", "age", "why"],
                  [[p["pid"], p["instance"], p["etime"], why] for p, why in spared])
    if not victims and not stale_locks:
        pruned = _prune_instances(args, out, claimed) if getattr(args, "prune", False) else []
        out.say("nothing to reap: %d gdx-dev RetroArch process(es), all with live drivers%s"
                % (len(procs), "; pruned %d instance dir(s)" % len(pruned) if pruned else ""))
        out.emit({"killed": [], "locks_cleared": [], "kept": len(spared), "pruned": pruned})
        return util.EXIT_OK

    out.table(["orphan pid", "instance", "age", "why"],
              [[p["pid"], p["instance"], p["etime"], why] for p, why in victims])
    if args.dry_run:
        out.say("DRY RUN — would kill %d process(es) and clear %d stale lock(s): %s"
                % (len(victims), len(stale_locks), ", ".join(stale_locks) or "none"))
        out.emit({"dry_run": True, "would_kill": [p["pid"] for p, _ in victims],
                  "would_clear": stale_locks})
        return util.EXIT_OK

    pruned = _prune_instances(args, out, claimed) if getattr(args, "prune", False) else []

    killed = []
    for pr, _why in victims:
        if kill_group(pr["pid"]):
            killed.append(pr["pid"])
            out.say("killed %s (instance %s)" % (pr["pid"], pr["instance"]))
        else:
            out.warn("could not kill %s (instance %s)" % (pr["pid"], pr["instance"]))
    for name in stale_locks:
        _drop_lock(Instance(name))
    out.say("reaped %d process(es), cleared %d stale lock(s)%s"
            % (len(killed), len(stale_locks),
               ", pruned %d instance dir(s)" % len(pruned) if pruned else ""))
    out.emit({"killed": killed, "locks_cleared": stale_locks, "kept": len(spared),
              "pruned": pruned})
    return util.EXIT_OK


def _last_used(inst):
    """When this instance was last RUN, as a timestamp.

    Deliberately NOT "the newest file in the tree": seed_sd copies the user's
    dspfirm.cdc, which is dated 2035 on this machine, into every instance — so a
    newest-file heuristic put every directory nine years in the future and pruned
    nothing, at --days 0, forever. Only files gdx-dev writes itself are trustworthy
    clocks, and retroarch.cfg is rewritten on every single launch.
    """
    for candidate in (inst.cfg, inst.lock, inst.log, inst.core_options):
        try:
            return os.path.getmtime(candidate)
        except OSError:
            continue
    try:
        return os.path.getmtime(inst.dir)
    except OSError:
        return 0


def _prune_instances(args, out, claimed):
    """Delete instance dirs older than --days. Unique-per-run instances accumulate one
    directory per run, so without this the private-SD win slowly eats the disk.

    Never touches a dir that is locked by a live driver, has a RetroArch running, or was
    modified inside the window — including named instances other tracks reuse.
    """
    days = getattr(args, "days", None)
    if days is None:
        days = 2
    cutoff = time.time() - days * 86400
    live = {p["instance"] for p in our_retro_processes()}
    pruned = []
    for name in instance_names():
        inst = Instance(name)
        held = claimed.get(name)
        if name in live:
            continue
        if held and held.get("driver_pid") and util.pid_alive(held["driver_pid"]):
            continue
        newest = _last_used(inst)
        if newest >= cutoff:
            continue
        age_days = (time.time() - newest) / 86400.0
        if args.dry_run:
            out.say("would prune %s (%.1f days old)" % (name, age_days))
            pruned.append(name)
            continue
        shutil.rmtree(inst.dir, ignore_errors=True)
        out.say("pruned %s (%.1f days old)" % (name, age_days))
        pruned.append(name)
    if not pruned:
        out.say("nothing to prune (no idle instance dir older than %g day(s))" % days)
    return pruned


def do_status(args, out, ctx):
    """Who owns which instance, and is anything orphaned."""
    procs = our_retro_processes()
    by_instance = {}
    for pr in procs:
        by_instance.setdefault(pr["instance"], []).append(pr)

    names = instance_names()
    if getattr(args, "instance", None):
        names = [n for n in names if n == args.instance]
    rows = []
    for name in names:
        held = read_lock(Instance(name))
        live = by_instance.get(name, [])
        driver = (held or {}).get("driver_pid")
        alive = bool(driver) and util.pid_alive(driver)
        orphaned = [p for p in live if p["ppid"] == 1]
        if held and alive:
            state = "RUNNING"
        elif held and live:
            state = "ORPHAN"          # lock names a dead driver, emulator still up
        elif held:
            state = "STALE LOCK"
        elif orphaned:
            state = "ORPHAN"          # reparented to init: its driver is definitely gone
        elif live:
            state = "unmanaged"       # parent alive, no lock: started before this fix
        else:
            state = "idle"
        if not (held or live) and not getattr(args, "all", False):
            continue
        rows.append([name, state, driver or "-", (held or {}).get("track") or "-",
                     ", ".join(str(p["pid"]) for p in live) or "-",
                     (live[0]["etime"] if live else "-"),
                     (held or {}).get("run_dir") or "-"])
    if rows:
        out.table(["instance", "state", "driver", "track", "retroarch", "age", "run dir"],
                  rows)
    else:
        out.say("no active retro instances (%d dir(s) on disk; --all to list them)"
                % len(instance_names()))
    orphans = [r for r in rows if r[1] in ("ORPHAN", "STALE LOCK")]
    if orphans:
        out.warn("%d orphaned/stale instance(s) — run `gdx-dev retro reap`" % len(orphans))
    out.emit({"instances": rows, "orphans": len(orphans),
              "processes": len(procs)})
    return util.EXIT_OK


# n60launch: when the current boot started (set by _attempt), so a launch-variant log-<X>.txt
# left by an earlier boot on a reused instance is never taken for this boot's log.
_BOOT_STARTED = {"t": 0.0}


def _guest_log_path(inst):
    """The guest's live filelog: log.txt, or -- n60launch, `[debug] launch_variants` -- the
    variant's own log-<X>.txt written since this boot started (never a -prev file)."""
    default = os.path.join(inst.gdx_dir(), "log.txt")
    if os.path.exists(default):
        return default
    gdx = inst.gdx_dir()
    try:
        names = os.listdir(gdx)
    except OSError:
        return default
    live = []
    for n in names:
        if not (n.startswith("log-") and n.endswith(".txt")) or "-prev" in n:
            continue
        path = os.path.join(gdx, n)
        if os.path.getmtime(path) >= _BOOT_STARTED["t"] - 2:
            live.append(path)
    if not live:
        return default
    return max(live, key=os.path.getmtime)


def _retro_snapshot(rd, inst):
    rd.snapshot(_guest_log_path(inst), "log-%s.txt" % inst.name)
    rd.snapshot(inst.log, "retroarch-%s.log" % inst.name)
    # Two screenshot sources, and a --replay run needs BOTH: RetroArch's own dir (the
    # UDP SCREENSHOT and --max-frames-ss output) and the guest's autotest/ on the private
    # SD, which is where a replay's `SHOT <label>` events write their BMPs. Collecting
    # only the first left a staged replay's shots stranded on the SD.
    sources = [inst.screenshots, os.path.join(inst.sdmc(), "autotest")]
    for src in sources:
        if not os.path.isdir(src):
            continue
        for n in sorted(os.listdir(src)):
            try:
                shutil.copy2(os.path.join(src, n), rd.sub("shots", n))
            except OSError:
                pass


def retroarch_version():
    if not os.path.exists(RETRO_BIN):
        return None
    p = subprocess.run([RETRO_BIN, "--version"], stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, text=True)
    # Line 1 is the banner ("RetroArch - Frontend for libretro"); line 2 carries the
    # version ("Version: 1.22.2 (Git 69a4f0ea) Nov 20 2025").
    for line in (p.stdout or "").splitlines():
        if line.startswith("Version:"):
            return line.split(":", 1)[1].strip()
    return (p.stdout or "").strip().splitlines()[:1][0] if p.stdout else None
