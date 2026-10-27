#!/usr/bin/env python3
"""hw.py — the Mac side of the 3DS hardware dev link (track `hwlink`).

Stdlib only, no third-party imports: this has to run on a bare macOS python3.

    hw.py push    [--build] [--ip A.B.C.D] [--3dsx PATH] [--log]
    hw.py log     [--port 17491] [--out artifacts/hw-<ts>.log]
    hw.py ping    [--ip ...]
    hw.py reload  [--3dsx PATH] [--ip ...] [--wait 40]
    hw.py capture <tag> [--ip ...]
    hw.py replay  <file.gdxrec> [--ip ...] [--dest /3ds/gdiffuser/rec/<name>]
    hw.py ini     <section> <key> <value> [--save] [--ip ...]
    hw.py put     <local-file> <sdmc-path> [--ip ...]
    hw.py get     <sdmc-path> [--out LOCAL] [--ip ...]
    hw.py quit    [--ip ...]
    hw.py input   [--demo | --script FILE] [--ip ...]
    hw.py gdb     [--ip ...] [--elf PATH]

The console side is port/3ds/net/gdx3ds_net.c. [net] enabled DEFAULTS ON when 3dslink's
netloader launched the console (which `push` always goes through) and OFF for a plain
hbmenu/SD/CIA launch; either way an explicit `[net] enabled` key in gdiffuser.ini always
wins. So after a `push`, every subcommand below just works with no ini edit; from a
non-netloader launch (SD/CIA), set `[net] enabled = 1` in sdmc:/3ds/gdiffuser/gdiffuser.ini
first (or push one with `hw.py put ... /3ds/gdiffuser/gdiffuser.ini`).

Common one-liners:
    hw.py log --out artifacts/session.log           # stream the live log to a local file
    hw.py put new.ini /3ds/gdiffuser/gdiffuser.ini   # push a file to an sdmc path
    hw.py get /3ds/gdiffuser/log.txt --out log.txt   # pull a file back
    hw.py reload --build                             # relaunch with a fresh build

--ip defaults to `[hw] ip` in tools/dev/hw.ini, which is GITIGNORED — the console's
address is the user's LAN detail and never lands in a commit.

Ports
  17491  log link      (LINK3DS_COMM_PORT; `hw.py log` is the SERVER, the console dials out)
  17492  control       (the console listens; every subcommand below dials in)
  17491  netloader     (hbmenu, spoken by `3dslink`, which `push` wraps)
  4950   InputRedirection (Luma/Rosalina, UDP; `hw.py input`)
  4000   GDB stub      (Luma/Rosalina; `hw.py gdb`)

`input` drives Rosalina's InputRedirection, which patches HID shared memory with NO
frame clock. It is for EXPLORATION and menu driving only. It is NOT a parity oracle and
cannot produce a deterministic replay: for that use `hw.py replay`, which pushes a
`.gdxrec` to the SD card and starts the in-app tick-exact injector (the oracle track owns
the .gdxrec format and the injector; this tool only transports and arms).
"""

import argparse
import binascii
import configparser
import os
import re

import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
HW_INI = os.path.join(HERE, "hw.ini")

DEVKITPRO = os.environ.get("DEVKITPRO", "/opt/devkitpro")
LINK3DS = os.path.join(DEVKITPRO, "tools", "bin", "3dslink")
GDB = os.path.join(DEVKITPRO, "devkitARM", "bin", "arm-none-eabi-gdb")

DEFAULT_3DSX = os.path.join(REPO, "build-3ds", "port", "3ds", "G-Diffuser-3DS.3dsx")
DEFAULT_ELF = os.path.join(REPO, "build-3ds", "port", "3ds", "G-Diffuser-3DS.elf")

LOG_PORT = 17491
CTL_PORT = 17492
IR_PORT = 4950
GDB_PORT = 4000

# The console keeps its own record of what it is called; the netloader writes
# sdmc:/3ds/<basename>, so the basename must stay stable or reload targets a stale file.
TARGET_BASENAME = "G-Diffuser-3DS.3dsx"
TARGET_SDMC = "/3ds/" + TARGET_BASENAME


# --------------------------------------------------------------------------------------
# config / small helpers
# --------------------------------------------------------------------------------------

def load_ip(explicit):
    if explicit:
        return explicit
    env = os.environ.get("GDX_3DS_IP")
    if env:
        return env
    if os.path.exists(HW_INI):
        cp = configparser.ConfigParser()
        cp.read(HW_INI)
        ip = cp.get("hw", "ip", fallback="").strip()
        if ip:
            return ip
    die("no console IP. Pass --ip A.B.C.D, set GDX_3DS_IP, or write tools/dev/hw.ini:\n"
        "    [hw]\n    ip = 192.168.1.42\n"
        "(hw.ini is gitignored — your LAN address never gets committed.)")


def die(msg, code=2):
    sys.stderr.write("hw.py: %s\n" % msg)
    sys.exit(code)


def crc32(path):
    crc = 0
    with open(path, "rb") as f:
        while True:
            b = f.read(1 << 20)
            if not b:
                break
            crc = binascii.crc32(b, crc)
    return crc & 0xFFFFFFFF


def ts():
    return time.strftime("%Y%m%d-%H%M%S")


# --------------------------------------------------------------------------------------
# control link (TCP 17492)
# --------------------------------------------------------------------------------------

class Control(object):
    """One command in flight; replies are '\\n'-terminated 'OK ...' / 'ERR ...' lines."""

    def __init__(self, ip, port=CTL_PORT, timeout=10.0):
        self.sock = socket.create_connection((ip, port), timeout=timeout)
        self.sock.settimeout(timeout)
        self.buf = b""

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass

    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.close()

    def send_line(self, line):
        self.sock.sendall((line + "\n").encode("ascii"))

    def read_line(self):
        while b"\n" not in self.buf:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise IOError("control link closed by the console")
            self.buf += chunk
        line, _, self.buf = self.buf.partition(b"\n")
        return line.decode("ascii", "replace").rstrip("\r")

    def command(self, line):
        self.send_line(line)
        return self.read_line()

    def put_file(self, header, path, progress=True):
        """`header` is the full command line minus <size> <crc32>, e.g. 'RELOAD' or
        'PUT /3ds/gdiffuser/rec/a.gdxrec'. The console replies 'OK ... ready <n>' and
        then reads exactly <n> raw bytes."""
        size = os.path.getsize(path)
        crc = crc32(path)
        reply = self.command("%s %d %08X" % (header, size, crc))
        if not reply.startswith("OK"):
            raise IOError("console refused the transfer: %s" % reply)
        sent = 0
        t0 = time.time()
        with open(path, "rb") as f:
            while True:
                b = f.read(64 * 1024)
                if not b:
                    break
                self.sock.sendall(b)
                sent += len(b)
                if progress:
                    pct = 100.0 * sent / size if size else 100.0
                    sys.stdout.write("\r  %6.1f%%  %d/%d B" % (pct, sent, size))
                    sys.stdout.flush()
        if progress:
            dt = time.time() - t0
            rate = (size / 1024.0 / dt) if dt > 0 else 0.0
            sys.stdout.write("\r  100.0%%  %d B in %.1fs (%.0f KiB/s)\n" % (size, dt, rate))
        return self.read_line()

    def get_file(self, sdmc_path, out_path, progress=True):
        """Console-side counterpart of put_file: GET <path> -> 'OK GET <size> <crc32hex>'
        then exactly <size> raw bytes. GET is confined to sdmc:/3ds/gdiffuser/ on the
        console (port/3ds/net/gdx3ds_net.c netPathUnderGdiffuser) and capped at NET_GET_CAP
        (64 KiB) -- it is a dev-loop pull (e.g. log.txt), not a general file browser.

        read_line() may already have pulled part of the raw payload into self.buf while
        looking for the reply's trailing '\\n' (a single recv() can return both), so that
        leftover has to seed `data` here rather than being dropped."""
        reply = self.command("GET %s" % sdmc_path)
        if not reply.startswith("OK GET"):
            raise IOError("console refused the pull: %s" % reply)
        parts = reply.split()
        if len(parts) < 4:
            raise IOError("malformed GET reply: %s" % reply)
        size = int(parts[2])
        want_crc = int(parts[3], 16)
        data = self.buf
        self.buf = b""
        t0 = time.time()
        while len(data) < size:
            chunk = self.sock.recv(min(65536, size - len(data)))
            if not chunk:
                raise IOError("console closed mid-GET (%d/%d B)" % (len(data), size))
            data += chunk
            if progress:
                pct = 100.0 * len(data) / size if size else 100.0
                sys.stdout.write("\r  %6.1f%%  %d/%d B" % (pct, len(data), size))
                sys.stdout.flush()
        if len(data) > size:
            # Nothing else is sent after a GET's payload, but trim defensively rather than
            # ever hand the caller trailing bytes that belong to a later reply.
            self.buf = data[size:] + self.buf
            data = data[:size]
        if progress:
            dt = time.time() - t0
            rate = (size / 1024.0 / dt) if dt > 0 else 0.0
            sys.stdout.write("\r  100.0%%  %d B in %.1fs (%.0f KiB/s)\n" % (size, dt, rate))
        got_crc = binascii.crc32(data) & 0xFFFFFFFF
        if got_crc != want_crc:
            raise IOError("GET crc mismatch want=%08X got=%08X" % (want_crc, got_crc))
        with open(out_path, "wb") as f:
            f.write(data)
        return size


# --------------------------------------------------------------------------------------
# log server (TCP 17491) — we are the server, the console dials out
# --------------------------------------------------------------------------------------

SUMMARY_PREFIXES = ("[fh]", "[prof]", "[watchdog]", "[fatal", "[net]", "[exit]", "[apt]")


class LogServer(object):
    def __init__(self, port=LOG_PORT, out_path=None, quiet=False):
        self.port = port
        self.quiet = quiet
        self.out_path = out_path
        self.out = None
        self.stop = threading.Event()
        self.lines = []
        self.srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.srv.bind(("0.0.0.0", port))
        self.srv.listen(2)
        self.srv.settimeout(0.5)

    def open_out(self):
        if self.out_path and self.out is None:
            d = os.path.dirname(self.out_path)
            if d:
                os.makedirs(d, exist_ok=True)
            self.out = open(self.out_path, "a", buffering=1)

    def emit(self, line):
        self.lines.append(line)
        stamped = "%s %s" % (time.strftime("%H:%M:%S"), line)
        if not self.quiet:
            print(stamped)
            sys.stdout.flush()
        self.open_out()
        if self.out:
            self.out.write(stamped + "\n")

    def serve_forever(self):
        buf = b""
        conn = None
        while not self.stop.is_set():
            if conn is None:
                try:
                    conn, addr = self.srv.accept()
                except socket.timeout:
                    continue
                except OSError:
                    break
                conn.settimeout(0.5)
                self.emit("--- log link from %s ---" % (addr[0],))
                buf = b""
                continue
            try:
                chunk = conn.recv(8192)
            except socket.timeout:
                continue
            except OSError:
                chunk = b""
            if not chunk:
                self.emit("--- log link closed ---")
                conn.close()
                conn = None
                continue
            buf += chunk
            while b"\n" in buf:
                line, _, buf = buf.partition(b"\n")
                self.emit(line.decode("utf-8", "replace").rstrip("\r"))
        if conn:
            conn.close()
        self.srv.close()

    def start_thread(self):
        t = threading.Thread(target=self.serve_forever, daemon=True)
        t.start()
        return t

    def wait_for(self, needle, timeout):
        """Blocks until a received line contains `needle`. Returns the line or None."""
        deadline = time.time() + timeout
        seen = 0
        while time.time() < deadline:
            while seen < len(self.lines):
                line = self.lines[seen]
                seen += 1
                if needle in line:
                    return line
            time.sleep(0.1)
        return None


# --------------------------------------------------------------------------------------
# subcommands
# --------------------------------------------------------------------------------------

def build_3dsx():
    env = dict(os.environ)
    env.setdefault("DEVKITPRO", DEVKITPRO)
    bdir = os.path.join(REPO, "build-3ds")
    if not os.path.exists(os.path.join(bdir, "CMakeCache.txt")):
        subprocess.check_call(
            ["cmake", "-S", REPO, "-B", bdir,
             "-DCMAKE_TOOLCHAIN_FILE=%s/cmake/3DS.cmake" % DEVKITPRO,
             "-DGDX_PLATFORM_3DS=ON", "-DCMAKE_BUILD_TYPE=Release"], env=env)
    subprocess.check_call(
        ["cmake", "--build", bdir, "--target", "G-Diffuser-3DS_3dsx", "-j8"], env=env)



_LINK_TOTAL_RE = re.compile(r"Sending\s+\S+,\s*(\d+)\s+bytes")
_LINK_SENT_RE = re.compile(r"(\d+)\s+sent\s*\(")


def _run_3dslink(ip, path):
    """Run 3dslink, echoing its output, and return (rc, bytes_sent, announced_total).

    3dslink writes progress with carriage returns, so the stream is read in chunks and
    split on both \r and \n rather than by line. bytes_sent is the largest count it
    reported, which is what the caller checks against the file size -- see cmd_push.
    """
    proc = subprocess.Popen([LINK3DS, "-a", ip, path],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    state = {"sent": None, "total": None}

    def scan(piece):
        m = _LINK_TOTAL_RE.search(piece)
        if m:
            state["total"] = int(m.group(1))
        m = _LINK_SENT_RE.search(piece)
        if m:
            n = int(m.group(1))
            state["sent"] = n if state["sent"] is None else max(state["sent"], n)

    buf = ""
    while True:
        chunk = proc.stdout.read(256)
        if not chunk:
            break
        text = chunk.decode("utf-8", "replace")
        sys.stdout.write(text)
        sys.stdout.flush()
        buf += text
        pieces = re.split(r"[\r\n]+", buf)
        for piece in pieces[:-1]:
            scan(piece)
        buf = pieces[-1]
    # The FINAL progress line usually arrives with no trailing \r or \n, so it sits in
    # `buf` when the pipe closes. Scanning only the completed pieces above dropped it and
    # made a completed transfer look like it stopped at whatever the previous line said --
    # which is how a 100% push was twice reported as truncated at 46.18%.
    scan(buf)
    proc.wait()
    return proc.returncode, state["sent"], state["total"]



def _wait_for_guest(ip, timeout):
    """Poll the control port until the freshly pushed guest answers PING. -> reply or None.

    This is the push's real verification: bytes on the wire are what 3dslink claims, a PONG
    is what the console did with them.
    """
    deadline = time.time() + max(1, timeout)
    while time.time() < deadline:
        try:
            with Control(ip, timeout=3.0) as c:
                reply = c.command("PING")
            if reply.startswith("OK"):
                return reply
        except Exception:
            pass
        time.sleep(3.0)
    return None


def _post_push(srv, args):
    if not srv:
        return 0
    line = srv.wait_for("[net] link up", args.wait)
    if line:
        print("=> %s" % line)
    else:
        print("!! no '[net] link up' within %ds. Either [net] enabled=0 in "
              "gdiffuser.ini, or the console cannot reach this Mac." % args.wait)
    try:
        while True:
            time.sleep(0.5)
    except KeyboardInterrupt:
        print("\nstopping log server")
        srv.stop.set()
    return 0


def cmd_push(args):
    if not os.path.exists(LINK3DS):
        die("3dslink not found at %s.\n"
            "It ships with devkitPro's `3dstools` package:\n"
            "    sudo /opt/devkitpro/pacman/bin/dkp-pacman -S 3dstools\n"
            "or set DEVKITPRO to the right prefix." % LINK3DS)
    if args.build:
        build_3dsx()
    path = args.dsx or DEFAULT_3DSX
    if not os.path.exists(path):
        die("no .3dsx at %s (pass --3dsx, or --build)" % path)
    if os.path.basename(path) != TARGET_BASENAME:
        die("the pushed basename must stay %s — hbmenu's netloader writes "
            "sdmc:/3ds/<basename> and RELOAD targets that exact path. Got %s."
            % (TARGET_BASENAME, os.path.basename(path)))
    ip = load_ip(args.ip)

    srv = None
    if args.log:
        # Bind our own log server BEFORE the push: the app dials out within a second or
        # two of booting. We do NOT pass `3dslink -s`, because that makes 3dslink itself
        # occupy port 17491 and we would be fighting it for the console's connection.
        srv = LogServer(port=LOG_PORT, out_path=args.out or default_log_path())
        srv.start_thread()
        print("log server on 0.0.0.0:%d -> %s" % (LOG_PORT, srv.out_path))

    print("3dslink -a %s %s" % (ip, path))
    rc, sent, total = _run_3dslink(ip, path)
    if rc != 0:
        die("3dslink failed (rc=%d). Is hbmenu on the netloader screen? Press Y." % rc)
    # 3dslink can exit 0 having sent only part of the file -- a mid-transfer close looks
    # like a clean one from its side. The console then tries to launch a truncated .3dsx
    # and throws an error that reads like a bad build. Verify the byte count it reported
    # against the file we handed it, and against the announced total.
    want = os.path.getsize(path)
    # 3dslink's byte counter CANNOT verify this transfer. When its stdout is a pipe rather
    # than a tty it stops emitting progress updates partway through -- every piped push here
    # stalls its reporting at ~46% -- so a low final count says nothing about what arrived.
    # Two pushes were wrongly declared TRUNCATED on that evidence while the guest was in fact
    # running. The honest test is whether the console comes up, so ask it.
    if sent is not None and sent >= want:
        print("pushed %d bytes (3dslink reported the full transfer)." % sent)
        return _post_push(srv, args)
    progress = ("3dslink reported %d of %d bytes (%.2f%%)" % (sent, want, 100.0 * sent / want)
                if sent else "3dslink printed no progress line")
    print("%s -- piped output under-reports, so confirming against the console." % progress)
    booted = _wait_for_guest(ip, args.verify_wait)
    if booted:
        print("pushed and CONFIRMED: %s" % booted)
    else:
        print("!! could not confirm the guest within %ds. Either the transfer really was\n"
              "   short (re-arm the netloader screen and push again), [net] enabled=0 in\n"
              "   gdiffuser.ini, or the console cannot reach this Mac." % args.verify_wait)
    return _post_push(srv, args)
    if srv:
        line = srv.wait_for("[net] link up", args.wait)
        if line:
            print("=> %s" % line)
        else:
            print("!! no '[net] link up' within %ds. Either [net] enabled=0 in "
                  "gdiffuser.ini, or the console cannot reach this Mac." % args.wait)
        try:
            while True:
                time.sleep(0.5)
        except KeyboardInterrupt:
            print("\nstopping log server")
            srv.stop.set()
    return 0


def default_log_path():
    return os.path.join(REPO, "artifacts", "hw-%s.log" % ts())


def cmd_log(args):
    srv = LogServer(port=args.port, out_path=args.out or default_log_path())
    print("log server on 0.0.0.0:%d -> %s   (Ctrl-C to stop)" % (args.port, srv.out_path))
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\nsummary of interesting lines:")
        for line in srv.lines:
            if line.startswith(SUMMARY_PREFIXES):
                print("  " + line)
    return 0


def cmd_ping(args):
    with Control(load_ip(args.ip)) as c:
        print(c.command("PING"))
    return 0


def cmd_reload(args):
    path = args.dsx or DEFAULT_3DSX
    if args.build:
        build_3dsx()
    if not os.path.exists(path):
        die("no .3dsx at %s" % path)
    ip = load_ip(args.ip)

    # A `hw.py log` running in another terminal already owns 17491. That is the NORMAL
    # setup, so do not fail on it: fall back to polling PING for a changed build id, which
    # proves a new process came back just as well.
    srv = None
    try:
        srv = LogServer(port=LOG_PORT, out_path=args.out or default_log_path())
        srv.start_thread()
    except OSError as e:
        print("note: port %d is already served (%s) — watching for the relaunch with "
              "PING instead. Your `hw.py log` terminal has the receipts." % (LOG_PORT, e))

    with Control(ip, timeout=30.0) as c:
        before = c.command("PING")
        print("before: %s" % before)
        old_build = before.split()[2] if before.startswith("OK PONG") else None
        print("streaming %s (%d B)" % (path, os.path.getsize(path)))
        print("=> %s" % c.put_file("RELOAD", path))

    # The console quits, Luma relaunches it, the new process dials back in.
    if srv is not None:
        line = srv.wait_for("[net] link up", args.wait)
        srv.stop.set()
        tail = srv.lines[-20:]
    else:
        line, tail = wait_for_relaunch_via_ping(ip, args.wait, old_build)
    if not line:
        print("!! no relaunch within %ds." % args.wait)
        for l in tail:
            print("   " + l)
        print("If the log shows '[net] hbldr unavailable rc=...', this loader is not "
              "Luma's Rosalina: the new image IS already on the card — press A in hbmenu "
              "to relaunch by hand.")
        print("RELOAD LOOP GUARD: do NOT push again until you know why it did not come "
              "back. A boot-crashing build must not be reloaded in a loop.")
        return 1
    print("=> %s" % line)
    if old_build and old_build in line:
        print("!! build id is UNCHANGED (%s) — the old process may have come back, or "
              "you reloaded the same build." % old_build)
    return 0


def wait_for_relaunch_via_ping(ip, timeout, old_build):
    """Fallback when someone else owns 17491: poll the control port until it answers
    again. A fresh PONG means a process is listening, i.e. the relaunch happened."""
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        time.sleep(1.0)
        try:
            with Control(ip, timeout=2.0) as c:
                r = c.command("PING")
            last = r
            if r.startswith("OK PONG"):
                build, frame = r.split()[2], int(r.split()[3])
                # A relaunched process starts its frame counter over; the old one is well
                # past that by the time a reload completes.
                if build != old_build or frame < 300:
                    return ("[ping] %s" % r), []
        except (OSError, IOError):
            continue
    return None, ([last] if last else ["(the control port never answered again)"])


def cmd_capture(args):
    with Control(load_ip(args.ip)) as c:
        print(c.command("CAPTURE %s" % args.tag))
    return 0


def cmd_replay(args):
    """Deterministic replay on hardware = push the .gdxrec to the SD card and start the
    IN-APP tick-exact injector. The oracle track owns both the format and the injector;
    hw.py is transport + arm only. UDP InputRedirection (`hw.py input`) is explicitly NOT
    this: it has no frame clock and cannot be a parity oracle."""
    if args.off:
        # Disarming has to DELETE the key: an empty input_replay is still an entry, and
        # main_3ds.cpp:772 only treats a missing/empty string as off while INI SAVE would
        # keep writing it back — so the replay survived reloads.
        with Control(load_ip(args.ip)) as c:
            print(c.command("INI DEL debug input_replay"))
            print(c.command("INI SAVE"))
        return 0
    if not args.file:
        die("replay needs a .gdxrec file (or --off to disarm)")
    if not os.path.exists(args.file):
        die("no such file: %s" % args.file)
    dest = args.dest or ("/3ds/gdiffuser/rec/" + os.path.basename(args.file))
    if not dest.startswith("/"):
        die("--dest must be an sdmc-relative absolute path, e.g. /3ds/gdiffuser/rec/a.gdxrec")
    with Control(load_ip(args.ip), timeout=30.0) as c:
        print("=> %s" % c.put_file("PUT %s" % dest, args.file))
        print("=> %s" % c.command("REPLAY %s" % dest))
    return 0


def cmd_ini(args):
    """Set or DELETE one ini key at runtime.

    Deleting is not "set it to the empty string": an empty value is still an entry, so
    the getters return "" instead of the key's DEFAULT and `INI SAVE` writes the key
    straight back out. That is why `ini debug input_replay ""` left a replay armed across
    reloads. `INI DEL` removes the entry, and the guest then falls back to the default.
    """
    if args.delete:
        if args.value is not None:
            die("ini --delete takes <section> <key> and no value")
        with Control(load_ip(args.ip)) as c:
            print(c.command("INI DEL %s %s" % (args.section, args.key)))
            if args.save:
                print(c.command("INI SAVE"))
        return 0
    if args.value is None:
        die("ini needs a <value> (or --delete to remove the key)")
    with Control(load_ip(args.ip)) as c:
        print(c.command("INI %s %s %s" % (args.section, args.key, args.value)))
        if args.save:
            print(c.command("INI SAVE"))
    return 0


def cmd_put(args):
    """Push a local file to an arbitrary sdmc-relative path -- the generic console-side
    PUT, same wire command `hw.py replay` already uses for .gdxrec files. The headline use:
    replacing sdmc:/3ds/gdiffuser/gdiffuser.ini without a card swap --
        hw.py put local.ini /3ds/gdiffuser/gdiffuser.ini
    (PUT takes effect on disk immediately; the running process only re-reads the ini on its
    own next boot, so follow with `hw.py reload` to pick it up without a power cycle.)"""
    if not os.path.exists(args.file):
        die("no such file: %s" % args.file)
    if not args.dest.startswith("/"):
        die("dest must be an sdmc-relative absolute path, e.g. /3ds/gdiffuser/gdiffuser.ini")
    with Control(load_ip(args.ip), timeout=30.0) as c:
        print("=> %s" % c.put_file("PUT %s" % args.dest, args.file))
    return 0


def cmd_get(args):
    """Pull a file back from the console -- the counterpart of `put`. Confined to
    sdmc:/3ds/gdiffuser/ and capped at 64 KiB on the console side (gdx3ds_net.c
    NET_GET_PREFIX / NET_GET_CAP), so this is a dev-loop pull (log.txt, gdiffuser.ini), not
    a general file browser. Headline use:
        hw.py get /3ds/gdiffuser/log.txt --out artifacts/log.txt"""
    if not args.path.startswith("/3ds/gdiffuser/"):
        die("GET is confined to /3ds/gdiffuser/ on the console (gdx3ds_net.c "
            "netPathUnderGdiffuser) -- got %r" % args.path)
    out = args.out or os.path.join(REPO, "artifacts", os.path.basename(args.path))
    d = os.path.dirname(out)
    if d:
        os.makedirs(d, exist_ok=True)
    with Control(load_ip(args.ip), timeout=30.0) as c:
        size = c.get_file(args.path, out)
    print("pulled %s -> %s (%d B)" % (args.path, out, size))
    return 0


def cmd_quit(args):
    with Control(load_ip(args.ip)) as c:
        print(c.command("QUIT"))
    return 0


# --------------------------------------------------------------------------------------
# InputRedirection (UDP 4950)
# --------------------------------------------------------------------------------------
#
# Packet layout, from Luma3DS sysmodules/rosalina/source/input_redirection.c: exactly
# 20 bytes, five little-endian u32. Rosalina copies buf[0..11] into HID shared memory
# unconditionally and buf[12..19] only when n >= 20, so ALWAYS send 20.
#
#   [0] hid      12 button bits, ACTIVE-LOW. Neutral 0x00000FFF; clear a bit to press.
#   [1] touch    neutral 0x02000000
#   [2] circle   (y << 12) | x, 12 bits per axis, neutral 0x7FF each => 0x007FF7FF
#   [3] cstick   C-stick / IR, neutral 0x80800081
#   [4] special  bit0 HOME, bit1 POWER, bit2 POWER-long
#
# A stuck stream OUTLIVES our process (it patches HID globally, HOME menu included), so
# neutral-on-exit is mandatory, not politeness.

HID_NEUTRAL = 0x00000FFF
TOUCH_NEUTRAL = 0x02000000
CIRCLE_NEUTRAL = 0x007FF7FF
CSTICK_NEUTRAL = 0x80800081

BUTTON_BITS = {
    "A": 0, "B": 1, "SELECT": 2, "START": 3,
    "RIGHT": 4, "LEFT": 5, "UP": 6, "DOWN": 7,
    "R": 8, "L": 9, "X": 10, "Y": 11,
}
SPECIAL_BITS = {"HOME": 0, "POWER": 1, "POWERLONG": 2}


def ir_packet(buttons=(), circle=(0.0, 0.0), touch=None, special=()):
    """buttons: names from BUTTON_BITS. circle: (x, y) in -1.0..1.0.
    touch: (x, y) in screen pixels, or None for 'not touching'.
    special: names from SPECIAL_BITS."""
    hid = HID_NEUTRAL
    for b in buttons:
        bit = BUTTON_BITS.get(b.upper())
        if bit is None:
            die("unknown button %r (have: %s)" % (b, " ".join(sorted(BUTTON_BITS))))
        hid &= ~(1 << bit)  # active-low: clearing the bit presses it

    def axis(v):
        v = max(-1.0, min(1.0, float(v)))
        return max(0, min(0xFFF, int(round(0x7FF + v * 0x7FF))))

    circ = (axis(circle[1]) << 12) | axis(circle[0])

    if touch is None:
        tw = TOUCH_NEUTRAL
    else:
        tx = max(0, min(0xFFF, int(touch[0])))
        ty = max(0, min(0xFFF, int(touch[1])))
        tw = (ty << 12) | tx | 0x01000000  # touching flag

    sp = 0
    for s in special:
        bit = SPECIAL_BITS.get(s.upper())
        if bit is None:
            die("unknown special %r (have: %s)" % (s, " ".join(sorted(SPECIAL_BITS))))
        sp |= 1 << bit

    return struct.pack("<5I", hid, tw, circ, CSTICK_NEUTRAL, sp)


NEUTRAL_PACKET = ir_packet()


class InputDriver(object):
    def __init__(self, ip, port=IR_PORT, hz=60.0):
        self.addr = (ip, port)
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.period = 1.0 / hz

    def send(self, pkt, repeat=1):
        for _ in range(repeat):
            self.sock.sendto(pkt, self.addr)

    def hold(self, pkt, seconds):
        # UDP has no ACK and packets are lost, so a state change is sent 3x and the state
        # is then re-sent every frame for the whole hold.
        self.send(pkt, repeat=3)
        end = time.time() + seconds
        while time.time() < end:
            self.send(pkt)
            time.sleep(self.period)

    def neutral(self):
        # Mandatory on every exit path: Rosalina keeps applying the LAST packet forever,
        # so a Ctrl-C mid-hold would leave the console accelerating by itself.
        self.send(NEUTRAL_PACKET, repeat=5)


def cmd_input(args):
    ip = load_ip(args.ip)
    drv = InputDriver(ip, hz=args.hz)
    print("InputRedirection -> %s:%d  (Rosalina: L+Down+Select -> Miscellaneous options "
          "-> Enable input redirection)" % (ip, IR_PORT))
    print("NOTE: no frame clock. Exploration and menu driving only — NOT a parity "
          "oracle. Use `hw.py replay` for tick-exact input.")
    try:
        if args.script:
            run_input_script(drv, args.script)
        else:
            demo(drv)
    finally:
        drv.neutral()
        print("pad returned to neutral")
    return 0


def demo(drv):
    print("  A tap")
    drv.hold(ir_packet(buttons=["A"]), 0.15)
    drv.hold(NEUTRAL_PACKET, 0.35)
    print("  circle pad right, 1.0 s")
    drv.hold(ir_packet(circle=(1.0, 0.0)), 1.0)
    drv.hold(NEUTRAL_PACKET, 0.3)
    for b in ("L", "R", "X", "Y", "START"):
        print("  %s tap" % b)
        drv.hold(ir_packet(buttons=[b]), 0.15)
        drv.hold(NEUTRAL_PACKET, 0.25)


def run_input_script(drv, path):
    """Line format:  <seconds> [BUTTON,BUTTON,...] [cx cy] [@x,y]   (@x,y = touch, pixels)
    e.g.
        0.2  A
        1.0  -  1.0 0.0        # hold the circle pad right for a second
        0.5  L,R
        0.3  -                 # neutral
    '#' comments and blank lines are ignored."""
    with open(path) as f:
        for lineno, raw in enumerate(f, 1):
            line = raw.split("#", 1)[0].strip()
            if not line:
                continue
            parts = line.split()
            touch = None
            for tok in [t for t in parts if t.startswith("@")]:
                # "@x,y": touch the bottom screen at (x, y) pixels for this step
                try:
                    tx, ty = tok[1:].split(",")
                    touch = (int(tx), int(ty))
                except ValueError:
                    die("%s:%d: touch token must be @x,y" % (path, lineno))
                parts.remove(tok)
            try:
                secs = float(parts[0])
            except ValueError:
                die("%s:%d: first token must be a duration in seconds" % (path, lineno))
            btns, spec = [], []
            if len(parts) > 1 and parts[1] not in ("-", "NONE"):
                # HOME/POWER are not HID buttons: they ride the special word and NS acts on
                # them, which is the only way to drive a suspend from here.
                for b in parts[1].split(","):
                    if not b:
                        continue
                    (spec if b in SPECIAL_BITS else btns).append(b)
            cx = float(parts[2]) if len(parts) > 2 else 0.0
            cy = float(parts[3]) if len(parts) > 3 else 0.0
            print("  %5.2fs %-14s circle=(%.2f,%.2f)%s"
                  % (secs, ",".join(btns) or "-", cx, cy,
                     (" special=" + ",".join(spec)) if spec else ""))
            if touch is not None:
                print("         touch=(%d,%d)" % touch)
            drv.hold(ir_packet(buttons=btns, circle=(cx, cy), touch=touch, special=spec), secs)


# --------------------------------------------------------------------------------------
# gdb (Rosalina stub, TCP 4000)
# --------------------------------------------------------------------------------------

def cmd_gdb(args):
    if not os.path.exists(GDB):
        die("no arm-none-eabi-gdb at %s (devkitARM package `gdb`)" % GDB)
    elf = args.elf or DEFAULT_ELF
    if not os.path.exists(elf):
        die("no .elf at %s — gdb needs the symbols from the SAME build that is running "
            "on the console" % elf)
    ip = load_ip(args.ip)
    script = "\n".join([
        "set pagination off",
        "set confirm off",
        "target extended-remote %s:%d" % (ip, GDB_PORT),
        "echo \\n==== bt ====\\n",
        "bt",
        "echo \\n==== info threads ====\\n",
        "info threads",
        "echo \\n==== thread apply all bt ====\\n",
        "thread apply all bt",
        "echo \\n==== info registers ====\\n",
        "info registers",
        "detach",
        "quit",
        "",
    ])
    fd, spath = tempfile.mkstemp(prefix="hw-gdb-", suffix=".gdb")
    with os.fdopen(fd, "w") as f:
        f.write(script)
    print("Rosalina: L+Down+Select -> Debugger options -> Enable debugger, then "
          "'Force-debug next application launch' if the process is not attachable.")
    print("%s -batch -x %s %s" % (GDB, spath, elf))
    try:
        return subprocess.call([GDB, "-batch", "-x", spath, elf])
    finally:
        os.unlink(spath)


# --------------------------------------------------------------------------------------

def main(argv):
    ap = argparse.ArgumentParser(prog="hw.py", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd")

    def ip_arg(p):
        p.add_argument("--ip", help="console IPv4 (default: [hw] ip in tools/dev/hw.ini, "
                                    "or $GDX_3DS_IP)")

    p = sub.add_parser("push", help="3dslink the .3dsx to hbmenu's netloader")
    ip_arg(p)
    p.add_argument("--build", action="store_true", help="cmake --build first")
    p.add_argument("--3dsx", dest="dsx", help="path to the .3dsx")
    p.add_argument("--log", action="store_true", help="also run the log server and tail it")
    p.add_argument("--out", help="log file (default artifacts/hw-<ts>.log)")
    p.add_argument("--wait", type=int, default=40, help="seconds to wait for [net] link up")
    p.add_argument("--verify-wait", dest="verify_wait", type=int, default=45,
                   help="seconds to wait for the pushed guest to answer PING on the control "
                        "port. This, not 3dslink's byte counter, is what confirms a push: "
                        "piped 3dslink output stalls its progress reporting partway through.")
    p.set_defaults(fn=cmd_push)

    p = sub.add_parser("log", help="serve the log link on 17491 and tail it")
    p.add_argument("--port", type=int, default=LOG_PORT)
    p.add_argument("--out", help="log file (default artifacts/hw-<ts>.log)")
    p.set_defaults(fn=cmd_log)

    p = sub.add_parser("ping", help="liveness + build id + frame + uptime")
    ip_arg(p)
    p.set_defaults(fn=cmd_ping)

    p = sub.add_parser("reload", help="stream a new .3dsx and self-relaunch via hb:ldr")
    ip_arg(p)
    p.add_argument("--build", action="store_true")
    p.add_argument("--3dsx", dest="dsx")
    p.add_argument("--out")
    p.add_argument("--wait", type=int, default=40)
    p.set_defaults(fn=cmd_reload)

    p = sub.add_parser("capture", help="arm the oracle track's frame capture")
    ip_arg(p)
    p.add_argument("tag")
    p.set_defaults(fn=cmd_capture)

    p = sub.add_parser("replay", help="push a .gdxrec and start the in-app tick-exact replay")
    ip_arg(p)
    p.add_argument("file", nargs="?", default=None)
    p.add_argument("--off", action="store_true",
                   help="disarm: delete [debug] input_replay and save, so the replay does "
                        "not survive the next reload")
    p.add_argument("--dest", help="sdmc-relative destination "
                                  "(default /3ds/gdiffuser/rec/<basename>)")
    p.set_defaults(fn=cmd_replay)

    p = sub.add_parser("ini", help="set or delete an ini key at runtime")
    ip_arg(p)
    p.add_argument("section")
    p.add_argument("key")
    p.add_argument("value", nargs="?", default=None)
    p.add_argument("--delete", action="store_true",
                   help="remove the key so the guest falls back to its DEFAULT "
                        "(setting an empty value does NOT do this)")
    p.add_argument("--save", action="store_true", help="also write gdiffuser.ini back")
    p.set_defaults(fn=cmd_ini)

    p = sub.add_parser("put", help="push a local file to an sdmc-relative path (e.g. a new "
                                   "gdiffuser.ini)")
    ip_arg(p)
    p.add_argument("file", help="local file to push")
    p.add_argument("dest", help="sdmc-relative destination, e.g. /3ds/gdiffuser/gdiffuser.ini")
    p.set_defaults(fn=cmd_put)

    p = sub.add_parser("get", help="pull a file back from sdmc:/3ds/gdiffuser/ (e.g. log.txt)")
    ip_arg(p)
    p.add_argument("path", help="sdmc-relative source, under /3ds/gdiffuser/")
    p.add_argument("--out", help="local destination (default artifacts/<basename>)")
    p.set_defaults(fn=cmd_get)

    p = sub.add_parser("quit", help="clean programmatic exit through the [exit] chain")
    ip_arg(p)
    p.set_defaults(fn=cmd_quit)

    p = sub.add_parser("input", help="drive Rosalina InputRedirection over UDP 4950")
    ip_arg(p)
    g = p.add_mutually_exclusive_group()
    g.add_argument("--demo", action="store_true", help="A tap, circle-pad right, L/R/X/Y")
    g.add_argument("--script", help="input script (see run_input_script docstring)")
    p.add_argument("--hz", type=float, default=60.0)
    p.set_defaults(fn=cmd_input)

    p = sub.add_parser("gdb", help="batch backtrace against the Rosalina GDB stub")
    ip_arg(p)
    p.add_argument("--elf", help="path to the matching .elf")
    p.set_defaults(fn=cmd_gdb)

    args = ap.parse_args(argv)
    if not getattr(args, "fn", None):
        ap.print_help()
        return 2
    try:
        return args.fn(args)
    except (IOError, OSError, socket.error) as e:
        die("%s" % e)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
