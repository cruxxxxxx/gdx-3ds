#!/usr/bin/env python3
"""tools/mp-rendezvous/emu_matrix.py — mp-online on the emulator lane: N consoles, one server.

Runs a local gdx_rendezvous.py and N `gdx-dev retro run` instances of one .3dsx on this Mac,
all on 127.0.0.1: the host creates a room with a fixed code, the others join it, every player
is a test bot ([net] mp_bot=1: the game's CPU driver plus a share of its exchanged, jittered
pad), the host starts a VS race as soon as everyone is ready, and the run ends when every
console has logged its `[mpo] race end` (or an ABORT / DESYNC). Verdict:

    PASS  every console finished the same number of races, at the same lockstep tick, with
          no DESYNC and no ABORT, no [nullread] line (a NULL string in a stock font draw), and every
          console checked the others' state digests.

Cases (--case): the plain path; --relay force (every link through the server's relay);
--sim-symmetric SLOTS (the server advertises those slots as if behind a symmetric NAT) or
--sim-nat (the consoles themselves drop direct datagrams); --relay-delay-ms / -jitter / -loss
(the server's relay becomes a latency/loss proxy -- combine with --relay force).

GUEST TIME. Azahar's guest clock is emulated time: a guest that is waiting runs it fast, a
slow guest runs it slow. Every guest-side timeout here is therefore set very large, and an
injected WALL latency shows up scaled in the guest's own RTT (the summary reports both).

Nothing here touches the network beyond 127.0.0.1. Instances are named <--prefix, mpo>-<case>-<i>;
the script stops the server and the drivers it started; --cleanup also deletes those instance
directories afterwards. The server requires GXRV v2 (2.1.0's default): a build before the cookie
needs --server-arg=--allow-v1, or --expect-message to check what it shows instead.

    tools/mp-rendezvous/emu_matrix.py build-3ds-n3ds/port/3ds/G-Diffuser-3DS.3dsx \\
        --players 2 --case p2 --run-root /path/to/runs --seconds 3600

MIXED BUILDS (MP30). THREEDSX may be a comma list, one .3dsx per instance (the last one repeats),
with --model picking each instance's console model: e.g. an Old 3DS EK build and a New 3DS EK
build in one room. --ini-for I:K=V sets a lever on one instance only (e.g. the EK BGM-delay
lever net.mp_bgm_hold_until on player 2); --leave-at / --vanish-at / --suspend-at may each be
given more than once (player:tick), and every survivor must remove every gone player at one
agreed tick. The verdict also compares, across the survivors, the CPU-takeover ticks of the
dropped players and the Expansion Kit countdown releases.

    tools/mp-rendezvous/emu_matrix.py o3ds-ek.3dsx,n3ds-ek.3dsx --model old,new,new,old \\
        --players 4 --vscom 1 --bot 2 --ini net.mp_racers=30 --ini net.mp_racers_o3ds_30=1 \\
        --leave-at 3:2400 --suspend-at 1:3600 --ini-for 2:net.mp_bgm_hold_until=90 ...
"""
import argparse
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
GDX_DEV = os.path.join(ROOT, "tools", "dev", "gdx-dev")
INSTANCE_ROOT = os.path.expanduser("~/Library/Application Support/gdx-retro")
ALPHABET = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"


def guest_log_dir(instance):
    return os.path.join(INSTANCE_ROOT, instance, "saves", "Azahar", "Azahar", "sdmc", "3ds", "gdiffuser")


def guest_log(instance):
    # An Expansion Kit build names its log log-EK.txt (the shipped build: log.txt).
    ek = os.path.join(guest_log_dir(instance), "log-EK.txt")
    return ek if os.path.exists(ek) else os.path.join(guest_log_dir(instance), "log.txt")


def room_code(case):
    h = 2166136261
    for ch in case.encode():
        h = ((h ^ ch) * 16777619) & 0xFFFFFFFF
    out = ""
    for _ in range(5):
        out += ALPHABET[h % 32]
        h //= 32
    return out


def parse_log(path):
    """The [mpo] facts of one guest log."""
    facts = {"race_end": [], "abort": [], "void": [], "desync": [], "start": [], "tick0": [], "stats": [],
             "phase": [], "messages": [], "init": None, "removed": [], "cpu_takeover": [], "ek_hold": [],
             "vs_com": [], "sim_abi": None, "nan": 0, "nullread": []}
    try:
        with open(path, "r", errors="replace") as f:
            text = f.read()
    except OSError:
        return facts
    for line in text.splitlines():
        if "[mp] ek hold:" in line:
            m = re.search(r"released at race frame (\d+) after (\d+) held ticks \(([^;]*);", line)
            if m:
                facts["ek_hold"].append((int(m.group(1)), int(m.group(2)), m.group(3)))
            continue
        if "[mp] vs com:" in line:
            m = re.search(r"vs com: (\d+) machines", line)
            if m:
                facts["vs_com"].append(int(m.group(1)))
            continue
        if "[nullread]" in line:
            facts["nullread"].append(line.strip())  # a NULL string in a stock font draw: faults on hardware
            continue
        if "[mpo]" not in line:
            continue
        if "[mpo] hash   r" in line and re.search(r"[=,](7fc00000|ffc00000)", line):
            facts["nan"] += 1  # a NaN racer field ([net] mp_hash_log): every console agrees, still a bug
            continue
        if "CPU driver takes over" in line:
            m = re.search(r"\[mpo\] p(\d+): CPU driver takes over at tick (\d+)", line)
            if m:
                facts["cpu_takeover"].append((int(m.group(1)), int(m.group(2))))
            continue
        if "[mpo] race end" in line:
            m = re.search(r"race end #(\d+) tick=(\d+).*stalls=(\d+) stall_max_ms=(\d+) stall_ms=(\d+) rej=(\d+)",
                          line)
            if m:
                facts["race_end"].append({"n": int(m.group(1)), "tick": int(m.group(2)),
                                          "stalls": int(m.group(3)), "stall_max_ms": int(m.group(4)),
                                          "stall_ms": int(m.group(5)), "rej": int(m.group(6))})
        elif "removed at tick" in line:
            m = re.search(r"\[mpo\] p(\d+) \(.*\) removed at tick (\d+)", line)
            if m:
                facts["removed"].append((int(m.group(1)), int(m.group(2))))
        elif "[mpo] ABORT" in line:
            # A race voided because a console restarted under the driver's early-crash retry (it
            # rejoins; the host starts again) is the harness, not the netcode: kept apart.
            if "left the room" in line and "DESYNC" not in line:
                facts["void"].append(line.strip())
                continue
            facts["abort"].append(line.strip())
            if "DESYNC" in line:
                facts["desync"].append(line.strip())
        elif "[mpo] START" in line:
            m = re.search(r"delay=(\d+)", line)
            facts["start"].append({"line": line.strip(), "delay": int(m.group(1)) if m else None})
        elif "[mpo] race tick 0" in line:
            facts["tick0"].append(line.strip())
        elif "[mpo] stats" in line:
            facts["stats"].append(line.strip())
        elif "[mpo] phase" in line:
            facts["phase"].append(line.strip())
        elif "[mpo] message" in line:
            facts["messages"].append(line.strip())
        elif "[mpo] auto-join nearby" in line or "[mpo] nearby:" in line:
            facts["messages"].append(line.strip())
        elif "[mpo] init" in line:
            facts["init"] = line.strip()
            m = re.search(r"sim_abi=([0-9a-fA-F]+)", line)
            if m:
                facts["sim_abi"] = m.group(1)
    return facts


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("threedsx")
    ap.add_argument("--players", type=int, default=2)
    ap.add_argument("--case", required=True)
    ap.add_argument("--run-root", required=True)
    ap.add_argument("--seconds", type=int, default=3600, help="ceiling per instance (wall)")
    ap.add_argument("--races", type=int, default=1)
    ap.add_argument("--course", type=int, default=0)
    ap.add_argument("--vscom", type=int, default=0)
    ap.add_argument("--end-after", type=int, default=0, help="end each race after N ticks (0 = finish)")
    ap.add_argument("--delay", type=int, default=0, help="force the input delay (0 = from RTT)")
    ap.add_argument("--relay", choices=["auto", "force"], default="auto")
    ap.add_argument("--sim-symmetric", default="", help="server: slots advertised behind symmetric NAT")
    ap.add_argument("--sim-nat", default="", help="comma list of player indices that drop direct datagrams")
    ap.add_argument("--relay-delay-ms", type=float, default=0.0)
    ap.add_argument("--relay-jitter-ms", type=float, default=0.0)
    ap.add_argument("--relay-loss-pct", type=float, default=0.0)
    ap.add_argument("--relay-burst-every-s", type=float, default=0.0, help="a relay loss burst every N s (wall)")
    ap.add_argument("--relay-burst-ms", type=float, default=0.0, help="each burst drops every relayed packet")
    ap.add_argument("--relay-burst-slot", type=int, default=-1, help="burst only one slot's packets (-1 = all)")
    ap.add_argument("--desync-at", default="", help="player:tick, the desync lever on one console")
    ap.add_argument("--leave-at", action="append", default=[],
                    help="player:tick, that console walks out of the room mid-race (repeatable)")
    ap.add_argument("--vanish-at", action="append", default=[],
                    help="player:tick, that console goes dark mid-race (power cut; repeatable)")
    ap.add_argument("--suspend-at", action="append", default=[],
                    help="player:tick, that console does what HOME / sleep does mid-race (repeatable)")
    ap.add_argument("--bot", type=int, default=1, help="[net] mp_bot for every console (2 = ram mode)")
    ap.add_argument("--ini-for", action="append", default=[], help="I:K=V, an extra lever for instance I only")
    ap.add_argument("--drop-ms", type=int, default=0,
                    help="guest ms of silence before a racer is removed (default: never; 300000 with --vanish-at (guest ms: a waiting guest runs its clock fast))")
    ap.add_argument("--lan", action="store_true",
                    help="NEARBY instead of a server: the host console hosts a LAN room, the others find it by "
                         "beacon (unicast to 127.0.0.1; one LAN case at a time: the 4 discovery ports are shared)")
    ap.add_argument("--lan-broadcast", action="store_true", help="--lan: also broadcast the beacon (the Mac's LAN)")
    ap.add_argument("--direct", action="store_true",
                    help="serverless online ([net] mp_direct): the host console hosts a WiFi room, the others "
                         "join it by its address and code (no server, no beacon)")
    ap.add_argument("--model", default="", help="comma list of new/old per player (default all new)")
    ap.add_argument("--ini", action="append", default=[], help="extra K=V for every instance")
    ap.add_argument("--3d", dest="three_d", default="",
                    help="comma list of player indices (or 'all') run with `retro run --3d` (the 3D core: slider on)")
    ap.add_argument("--stagger", type=float, default=8.0, help="seconds between instance launches")
    ap.add_argument("--cleanup", action="store_true", help="delete the instance directories afterwards")
    ap.add_argument("--prefix", default="mpo", help="instance names are PREFIX-<case>-<i> (default mpo)")
    ap.add_argument("--server-arg", action="append", default=[],
                    help="an extra gdx_rendezvous.py argument (e.g. --server-arg=--allow-v1)")
    ap.add_argument("--expect-message", default="",
                    help="a refusal case: PASS when every console logs an [mpo] message matching this regex "
                         "(e.g. a card before the cookie build against a server that requires it)")
    args = ap.parse_args()
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(3))  # still stop the server + drivers

    case_dir = os.path.join(os.path.abspath(args.run_root), args.case)
    os.makedirs(case_dir, exist_ok=True)
    code = room_code(args.case)
    port_file = os.path.join(case_dir, "server.port")
    if os.path.exists(port_file):
        os.unlink(port_file)
    srv_cmd = [sys.executable, os.path.join(HERE, "gdx_rendezvous.py"), "--bind", "127.0.0.1", "--port", "0",
               "--port-file", port_file, "--allow-custom-codes", "--member-timeout", "3600", "--stats-every", "60",
               # An emulated console's clock runs many times faster than the wall clock while it
               # waits for a pad (svcSleepThread returns at once), so its 16 ms resend loop sends
               # thousands of datagrams per real second. The production relay budget would drop
               # them and starve the race; real consoles send ~60/s.
               "--relay-pps", "100000", "--room-bps", "100000000"]
    if args.relay == "force":
        srv_cmd.append("--force-relay")
    srv_cmd += args.server_arg
    if args.sim_symmetric:
        srv_cmd += ["--sim-symmetric", args.sim_symmetric]
    if args.relay_delay_ms or args.relay_jitter_ms or args.relay_loss_pct:
        srv_cmd += ["--relay-delay-ms", str(args.relay_delay_ms), "--relay-jitter-ms", str(args.relay_jitter_ms),
                    "--relay-loss-pct", str(args.relay_loss_pct)]
    if args.relay_burst_every_s and args.relay_burst_ms:
        srv_cmd += ["--relay-burst-every-s", str(args.relay_burst_every_s), "--relay-burst-ms",
                    str(args.relay_burst_ms), "--relay-burst-slot", str(args.relay_burst_slot)]
    srv_log = open(os.path.join(case_dir, "server.log"), "w")
    srv = subprocess.Popen(srv_cmd, stdout=srv_log, stderr=subprocess.STDOUT)
    drivers = []
    instances = ["%s-%s-%d" % (args.prefix, args.case, i) for i in range(args.players)]
    expect = re.compile(args.expect_message) if args.expect_message else None

    def expected_message(inst):
        if expect is None:
            return False
        try:
            text = open(guest_log(inst), errors="replace").read()
        except OSError:
            return False
        return any(expect.search(line) for line in text.splitlines() if "[mpo] message: " in line)
    t_start = time.time()
    verdict = {"lan": args.lan, "direct": args.direct, "case": args.case, "players": args.players, "code": code, "relay": args.relay,
               "sim_symmetric": args.sim_symmetric, "sim_nat": args.sim_nat,
               "relay_delay_ms": args.relay_delay_ms, "relay_jitter_ms": args.relay_jitter_ms,
               "relay_loss_pct": args.relay_loss_pct, "instances": instances}
    try:
        for _ in range(100):
            if os.path.exists(port_file) and open(port_file).read().strip():
                break
            time.sleep(0.1)
        port = int(open(port_file).read().strip())
        verdict["server_port"] = port
        sim_nat = {int(x) for x in args.sim_nat.split(",") if x.strip()}
        desync_player, desync_tick = (None, None)
        if args.desync_at:
            a, b = args.desync_at.split(":")
            desync_player, desync_tick = int(a), int(b)
        gone = {}  # player -> (tick, lever)
        for opts, key in ((args.leave_at, "mp_leave_at"), (args.vanish_at, "mp_vanish_at"),
                          (args.suspend_at, "mp_suspend_at")):
            for opt in opts:
                a, b = opt.split(":")
                gone[int(a)] = (int(b), key)
        drop_ms = args.drop_ms or (300000 if args.vanish_at else 3600000)
        verdict["gone"] = {str(p): list(v) for p, v in gone.items()}
        builds = [b for b in args.threedsx.split(",") if b]
        verdict["builds"] = builds
        ini_for = {}
        for spec in args.ini_for:
            a, kv = spec.split(":", 1)
            ini_for.setdefault(int(a), []).append(kv)
        models = [m for m in args.model.split(",") if m] if args.model else []
        env = dict(os.environ)
        env["GDX_RETRO_SLOTS"] = "0"  # a deliberate N-instance run: the per-host gate would deadlock it
        for i, inst in enumerate(instances):
            # A stale guest log from an earlier run of this instance would be read as this run's.
            for name in ("log.txt", "log-EK.txt"):
                try:
                    os.unlink(os.path.join(guest_log_dir(inst), name))
                except OSError:
                    pass
            ini = ["net.mp_bind_port=%d" % (port % 20000 + 30000 + 10 * i),
                   "net.mp_bot=%d" % args.bot, "net.mp_wait_ms=3600000", "net.mp_launch_ms=3600000",
                   "net.mp_wifi_check=0", "net.mp_name=EMU%d" % i, "net.mp_stats_every=600",
                   "net.mp_relay=%s" % args.relay, "net.mp_drop_ms=%d" % drop_ms]
            host_port = port % 20000 + 30000
            if args.direct:
                ini += ["net.mp_lan=1", "net.mp_lan_timeout_ms=3600000", "net.mp_lan_broadcast=0"]
                ini.append("net.mp_direct=%s" % ("host" if i == 0 else "127.0.0.1:%d" % host_port))
            elif args.lan:
                ini += ["net.mp_lan=1", "net.mp_lan_peers=127.0.0.1", "net.mp_lan_timeout_ms=3600000",
                        "net.mp_lan_broadcast=%d" % (1 if args.lan_broadcast else 0)]
            else:
                ini.append("net.rendezvous=127.0.0.1:%d" % port)
            if i == 0:
                ini += ["net.mp_auto=%s" % ("lanhost" if (args.lan or args.direct) else "host"), "net.mp_code=%s" % code, "net.mp_autostart=%d" % args.players,
                        "net.mp_races=%d" % args.races, "net.mp_course=%d" % args.course,
                        "net.mp_vscom=%d" % args.vscom, "net.mp_players=%d" % max(2, args.players)]
                if args.delay:
                    ini.append("net.mp_delay=%d" % args.delay)
            elif args.direct:
                ini += ["net.mp_auto=djoin", "net.room=%s" % code]
            elif args.lan:
                ini += ["net.mp_auto=lanjoin"]
            else:
                ini += ["net.mp_auto=join", "net.room=%s" % code]
            if args.end_after:
                ini.append("net.mp_end_after=%d" % args.end_after)
            if i in sim_nat:
                ini.append("net.mp_sim_nat=symmetric")
            if i in gone:
                ini.append("net.%s=%d" % (gone[i][1], gone[i][0]))
            if desync_player == i:
                ini.append("net.mp_desync_at=%d" % desync_tick)
            ini += args.ini
            ini += ini_for.get(i, [])
            build = builds[min(i, len(builds) - 1)]
            cmd = [GDX_DEV, "retro", "run", os.path.abspath(build), "--instance", inst, "--track", "mpo",
                   "--run-root", case_dir, "--seconds", str(args.seconds), "--name", inst]
            if i < len(models) and models[i] in ("new", "old"):
                cmd += ["--model", models[i]]
            for kv in ini:
                cmd += ["--ini", kv]
            if args.three_d == "all" or str(i) in [x.strip() for x in args.three_d.split(",") if x.strip()]:
                cmd += ["--3d"]
            log = open(os.path.join(case_dir, "%s.driver.log" % inst), "w")
            drivers.append((inst, subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT, env=env,
                                                   start_new_session=True), log))
            print("[emu_matrix] launched %s" % inst, flush=True)
            time.sleep(args.stagger)
            if i == 0:
                # Joiners wait for the host's room: a join before it exists is a failed join, and
                # every instance here shares 127.0.0.1, i.e. ONE per-IP code-guessing budget.
                t_wait = time.time()
                while time.time() - t_wait < 900:
                    try:
                        if "phase connecting -> room" in open(guest_log(inst), errors="replace").read():
                            break
                        if expected_message(inst):
                            break
                    except OSError:
                        pass
                    time.sleep(2)
        want = args.races
        deadline = t_start + args.seconds + 120
        last_note = 0
        while time.time() < deadline:
            facts = [parse_log(guest_log(inst)) for inst in instances]
            live = [f for i, f in enumerate(facts) if i not in gone]
            done = all(len(f["race_end"]) >= want for f in live)
            bad = any(f["abort"] for f in live)
            dead = [inst for (inst, p, _) in drivers if p.poll() is not None]
            if time.time() - last_note > 60:
                print("[emu_matrix] %.0fs: race_end=%s abort=%s tick0=%s dead=%s" % (
                    time.time() - t_start, [len(f["race_end"]) for f in facts], [len(f["abort"]) for f in facts],
                    [len(f["tick0"]) for f in facts], dead), flush=True)
                last_note = time.time()
            if expect is not None and all(expected_message(inst) for inst in instances):
                time.sleep(5)  # the dialog is up: a screenshot can see it
                break
            if done or bad:
                time.sleep(10)  # the linger + the abort repeats land in the logs
                break
            if len(dead) == len(drivers):
                break
            time.sleep(5)
        facts = [parse_log(guest_log(inst)) for inst in instances]
        verdict["wall_s"] = round(time.time() - t_start, 1)
        verdict["per_console"] = []
        for inst, f in zip(instances, facts):
            verdict["per_console"].append({"instance": inst, "race_end": f["race_end"], "aborts": f["abort"],
                                           "voided": f["void"],
                                           "desync": f["desync"], "start": [s["line"] for s in f["start"]],
                                           "delay": [s["delay"] for s in f["start"]], "tick0": f["tick0"],
                                           "last_stats": f["stats"][-2:], "messages": f["messages"][-6:],
                                           "init": f["init"], "removed": f["removed"],
                                           "cpu_takeover": f["cpu_takeover"], "ek_hold": f["ek_hold"],
                                           "vs_com": f["vs_com"], "sim_abi": f["sim_abi"],
                                           "nan_fields": f["nan"], "nullread": f["nullread"][:8]})
        live = [f for i, f in enumerate(facts) if i not in gone]
        ends = [[e["tick"] for e in f["race_end"]] for f in live]
        ok = (all(len(e) >= want for e in ends) and all(e[:want] == ends[0][:want] for e in ends) and
              not any(f["abort"] for f in live) and not any(f["nullread"] for f in facts))
        if gone:
            # Every survivor removed every gone player once, at one agreed tick after its lever
            # fired, and (mp_drop = cpu) the CPU took each over at that same tick everywhere.
            # The levers name INSTANCES; the logs name lockstep PLAYERS (START's "me=pN").
            me = {}
            for i, f in enumerate(facts):
                m = re.search(r" me=p(\d+)", f["start"][0]["line"]) if f["start"] else None
                if m:
                    me[i] = int(m.group(1))
            gone_players = {me.get(i, i): v for i, v in gone.items()}
            verdict["gone_players"] = {str(p): list(v) for p, v in gone_players.items()}
            rm = [sorted(set(f["removed"])) for f in live]
            ok = ok and all(r == rm[0] for r in rm) and sorted(p for p, _ in rm[0]) == sorted(gone_players)
            ok = ok and all(t >= gone_players[p][0] for p, t in rm[0])
            cpu = [sorted(set(f["cpu_takeover"])) for f in live]
            ok = ok and all(c == cpu[0] for c in cpu)
            verdict["expect"] = "survivors remove %s at one tick each and finish identically" % sorted(gone)
        # A NaN in a racer field (mp_hash_log dumps) is a bug even when every console agrees on it.
        ok = ok and not any(f["nan"] for f in facts)
        # The Expansion Kit countdown hold releases on the same race frame on every console.
        holds = [[h[0] for h in f["ek_hold"]] for f in live]
        if any(holds):
            ok = ok and all(h == holds[0] for h in holds)
        if args.desync_at:
            ok = all(f["desync"] or any("DESYNC" in m or "desync" in m for m in f["messages"]) for f in facts) or \
                all(f["abort"] for f in facts)
            verdict["expect"] = "every console aborts (desync lever)"
        if expect is not None:
            got = [expected_message(inst) for inst in instances]
            verdict["expect"] = "every console shows a message matching %r" % args.expect_message
            verdict["expected_message"] = got
            ok = all(got)
        verdict["desyncs"] = sum(len(f["desync"]) for f in facts)
        verdict["verdict"] = "PASS" if ok else "FAIL"
    finally:
        for inst, p, log in drivers:
            if p.poll() is None:
                try:
                    os.killpg(p.pid, signal.SIGTERM)
                except OSError:
                    pass
        for inst, p, log in drivers:
            try:
                p.wait(timeout=60)
            except subprocess.TimeoutExpired:
                try:
                    os.killpg(p.pid, signal.SIGKILL)
                except OSError:
                    pass
            log.close()
        for inst in instances:
            try:
                shutil.copy(guest_log(inst), os.path.join(case_dir, "%s.guest.log" % inst))
            except OSError:
                pass
        srv.send_signal(signal.SIGTERM)
        try:
            srv.wait(timeout=10)
        except subprocess.TimeoutExpired:
            srv.kill()
        srv_log.close()
        if args.cleanup:
            for inst in instances:
                d = os.path.join(INSTANCE_ROOT, inst)
                if os.path.isdir(d) and os.path.basename(d).startswith(args.prefix + "-"):
                    shutil.rmtree(d, ignore_errors=True)
    with open(os.path.join(case_dir, "verdict.json"), "w") as f:
        json.dump(verdict, f, indent=1)
    print(json.dumps({k: verdict.get(k) for k in ("case", "players", "verdict", "wall_s", "desyncs")}))
    for c in verdict.get("per_console", []):
        print("  %s ends=%s delay=%s aborts=%d removed=%s cpu=%s ek_hold=%s vs_com=%s" % (
            c["instance"], [(e["tick"], e["stalls"]) for e in c["race_end"]], c["delay"], len(c["aborts"]),
            c.get("removed"), c.get("cpu_takeover"), c.get("ek_hold"), c.get("vs_com")))
    return 0 if verdict.get("verdict") == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
