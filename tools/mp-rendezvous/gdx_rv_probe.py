#!/usr/bin/env python3
"""gdx_rv_probe.py -- check a gdx rendezvous server from the outside and time it.

    gdx_rv_probe.py HOST[:PORT]                 # one ping: a KEEPALIVE that must come back as E_AUTH
    gdx_rv_probe.py HOST[:PORT] --mode room     # a whole room: the v2 cookie handshake, CREATE, KEEPALIVE
                                                # (KA_ACK + ROSTER), LEAVE
    gdx_rv_probe.py HOST[:PORT] --mode room --v1   # the same in v1 (a server before 2.1.0, or --allow-v1)
    gdx_rv_probe.py HOST[:PORT] --serve 9728 --every 15   # a Prometheus exporter (/metrics) that pings

The ping costs the server nothing (no room, no state beyond its per-address rate limit; a server
since 2.0.0 counts it in gdx_rv_probe_pings_total, an older one as one junk keepalive) and its
answer is 16 bytes for 36 sent. Exit
status: 0 when every check passed. Output: one JSON line per check.

Python 3.8+ standard library; it imports the packet constants from gdx_rendezvous.py next to it.
"""

import argparse
import json
import os
import secrets
import socket
import struct
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gdx_rendezvous as R  # noqa: E402

PROBE_BUILD = "gdx-probe"


def parse_target(text):
    host, _, port = text.partition(":")
    return socket.gethostbyname(host), int(port) if port else 27200


def build_request(t, nonce, proto=1, cookie=b"", code=""):
    body = struct.pack("<II4sHBB", nonce, 0, b"\0\0\0\0", 0, 2, 0) + R.pad_bytes(PROBE_BUILD, 40) + \
        R.pad_bytes("PROBE", 12) + R.pad_bytes(code, 8)
    pkt = R.GXRV_MAGIC + bytes([proto, t, 0, 0]) + body
    if proto == R.GXRV_VERSION_COOKIE:
        pkt += (cookie or bytes(R.COOKIE_LEN)) + struct.pack("<HH", R.CLIENT_PROTO, 0)
    return pkt


def build_auth(t, code, slot, key, proto=1, extra=b""):
    return R.GXRV_MAGIC + bytes([proto, t, 0, 0]) + R.pad_bytes(code, 8) + struct.pack("<BBHQ", slot, 0, 0, key) + extra


class Probe:
    def __init__(self, target, timeout=2.0):
        self.target = target
        self.timeout = timeout

    def _exchange(self, sock, pkt, want, deadline):
        """Send pkt, wait for a GXRV datagram of type want (others skipped). Returns (bytes, rtt)."""
        t0 = time.monotonic()
        sock.sendto(pkt, self.target)
        while True:
            left = deadline - time.monotonic()
            if left <= 0:
                raise TimeoutError("no %s from %s:%d" % (R.TYPE_NAMES.get(want, want), *self.target))
            sock.settimeout(left)
            data, src = sock.recvfrom(2048)
            if src[0] != self.target[0] or len(data) < R.HDR or data[:4] != R.GXRV_MAGIC:
                continue
            if data[5] == want:
                return data, time.monotonic() - t0
            if data[5] == R.T_ERROR and want != R.T_ERROR:
                raise RuntimeError("server error %s" % R.ERROR_NAMES.get(data[12], data[12]))

    def ping(self):
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            pkt = build_auth(R.T_KEEPALIVE, "", 0, 0, extra=struct.pack("<II", 0, 0))  # no code: a probe ping
            data, rtt = self._exchange(sock, pkt, R.T_ERROR, time.monotonic() + self.timeout)
            if len(data) != 16 or data[12] != R.E_AUTH:
                raise RuntimeError("unexpected answer (%d bytes, code %d)" % (len(data), data[12]))
            return {"check": "ping", "ok": True, "rtt_ms": round(rtt * 1000, 2)}
        finally:
            sock.close()

    def room(self, v2=False):
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        deadline = time.monotonic() + self.timeout * 3
        out = {"check": "room-v2" if v2 else "room-v1", "ok": False}
        try:
            nonce = secrets.randbits(32)
            proto = R.GXRV_VERSION_COOKIE if v2 else R.GXRV_VERSION
            cookie = b""
            if v2:
                data, rtt = self._exchange(sock, build_request(R.T_CREATE, nonce, proto), R.T_COOKIE, deadline)
                cookie = data[12:28]
                out["cookie_rtt_ms"] = round(rtt * 1000, 2)
            data, rtt = self._exchange(sock, build_request(R.T_CREATE, nonce, proto, cookie), R.T_WELCOME, deadline)
            out["create_rtt_ms"] = round(rtt * 1000, 2)
            if data[4] != proto:
                raise RuntimeError("WELCOME header version %d, wanted %d" % (data[4], proto))
            code = data[12:20].rstrip(b"\0").decode()
            slot, _maxp, _flags, key, _secret = struct.unpack_from("<BBBxQQ", data, 20)
            ka = build_auth(R.T_KEEPALIVE, code, slot, key, proto, struct.pack("<II", 0, 1234))
            data, rtt = self._exchange(sock, ka, R.T_KA_ACK, deadline)
            out["keepalive_rtt_ms"] = round(rtt * 1000, 2)
            if struct.unpack_from("<I", data, 12)[0] != 1234:
                raise RuntimeError("KA_ACK did not echo the clock")
            sock.sendto(build_auth(R.T_LEAVE, code, slot, key, proto), self.target)
            out["ok"] = True
            return out
        finally:
            sock.close()


def run_check(probe, mode, v2):
    t0 = time.time()
    try:
        res = probe.ping() if mode == "ping" else probe.room(v2)
    except (OSError, RuntimeError, TimeoutError) as e:
        res = {"check": mode if mode == "ping" else ("room-v2" if v2 else "room-v1"), "ok": False, "error": str(e)}
    res["t"] = round(t0, 3)
    res["target"] = "%s:%d" % probe.target
    return res


def serve(probe, port, bind, every, mode, v2):
    """A tiny blackbox exporter: probe every `every` s, serve the last result on /metrics."""
    import http.server

    state = {"ok": 0, "rtt": 0.0, "last": 0.0, "runs": 0, "fails": 0}
    lock = threading.Lock()

    def loop():
        while True:
            res = run_check(probe, mode, v2)
            with lock:
                state["runs"] += 1
                state["ok"] = 1 if res["ok"] else 0
                state["fails"] += 0 if res["ok"] else 1
                state["rtt"] = (res.get("rtt_ms") or res.get("keepalive_rtt_ms") or 0.0) / 1000.0
                state["last"] = res["t"]
            if not res["ok"]:
                print(json.dumps(res), flush=True)
            time.sleep(every)

    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):  # noqa: N802
            if self.path.split("?")[0] != "/metrics":
                self.send_response(404)
                self.end_headers()
                return
            with lock:
                lab = '{target="%s:%d",check="%s"}' % (probe.target[0], probe.target[1], mode)
                body = "\n".join([
                    "# HELP gdx_rv_probe_success 1 if the last probe got the right answer",
                    "# TYPE gdx_rv_probe_success gauge", "gdx_rv_probe_success%s %d" % (lab, state["ok"]),
                    "# HELP gdx_rv_probe_rtt_seconds round trip of the last successful probe",
                    "# TYPE gdx_rv_probe_rtt_seconds gauge", "gdx_rv_probe_rtt_seconds%s %.6f" % (lab, state["rtt"]),
                    "# HELP gdx_rv_probe_last_run_timestamp_seconds unix time of the last probe",
                    "# TYPE gdx_rv_probe_last_run_timestamp_seconds gauge",
                    "gdx_rv_probe_last_run_timestamp_seconds%s %.3f" % (lab, state["last"]),
                    "# HELP gdx_rv_probe_runs_total probes run", "# TYPE gdx_rv_probe_runs_total counter",
                    "gdx_rv_probe_runs_total%s %d" % (lab, state["runs"]),
                    "# HELP gdx_rv_probe_failures_total probes that failed",
                    "# TYPE gdx_rv_probe_failures_total counter",
                    "gdx_rv_probe_failures_total%s %d" % (lab, state["fails"]), ""]).encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/plain; version=0.0.4")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, *_a):
            pass

    threading.Thread(target=loop, daemon=True).start()
    httpd = http.server.ThreadingHTTPServer((bind, port), Handler)
    print(json.dumps({"event": "probe-serve", "port": port, "target": "%s:%d" % probe.target, "every": every}),
          flush=True)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description="probe a gdx rendezvous server")
    ap.add_argument("target", help="HOST[:PORT] (default port 27200)")
    ap.add_argument("--mode", choices=["ping", "room"], default="ping")
    ap.add_argument("--v2", action="store_true", help="room mode: the v2 cookie handshake (the default since 2.1.0)")
    ap.add_argument("--v1", action="store_true", help="room mode: v1, no cookie (a server before 2.1.0 / --allow-v1)")
    ap.add_argument("--count", type=int, default=1)
    ap.add_argument("--timeout", type=float, default=2.0)
    ap.add_argument("--serve", type=int, default=0, help="run as an exporter on this TCP port")
    ap.add_argument("--serve-bind", default="127.0.0.1")
    ap.add_argument("--every", type=float, default=15.0)
    args = ap.parse_args(argv)
    args.v2 = not args.v1
    probe = Probe(parse_target(args.target), args.timeout)
    if args.serve:
        return serve(probe, args.serve, args.serve_bind, args.every, args.mode, args.v2)
    ok = True
    for i in range(args.count):
        res = run_check(probe, args.mode, args.v2)
        print(json.dumps(res), flush=True)
        ok = ok and res["ok"]
        if i + 1 < args.count:
            time.sleep(0.3)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
