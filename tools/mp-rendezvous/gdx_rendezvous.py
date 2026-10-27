#!/usr/bin/env python3
"""gdx_rendezvous.py -- the G-Diffuser-3DS online multiplayer rendezvous + relay server.

One UDP port, Python 3.8+ standard library only, no accounts, no disk state. It does four jobs:

  1. rooms: a host CREATEs a room and gets a short code; up to 3 more consoles JOIN by code;
  2. endpoints: every member learns every other member's public (as this server sees it) and
     private (self-reported LAN) UDP endpoint -- the ROSTER;
  3. hole punching: the roster goes to everyone at once, and PUNCH_REQ makes the server nudge one
     peer so both sides fire at each other at the same moment;
  4. relay fallback: RELAY forwards a game datagram VERBATIM to other members of the same room
     (for symmetric / carrier-grade NAT, where no direct path opens).

Everything that arrives is hostile until proven otherwise: exact sizes, magic/version/type,
printable-ASCII build ids, a name whitelist, a code alphabet, per-IP and per-member token
buckets, a junk-source blocklist, failed-join throttling (code guessing), bounded tables, room
expiry, and no reply to an unauthenticated request is larger than the request (no amplification).
Relayed payloads must be a G-Diffuser game datagram whose sender field is the relaying member's
own slot, so a member cannot speak for another one.

Return routability (mp-server-ops). A CREATE / JOIN from a spoofed source address gets its WELCOME
sent to the victim, never back to the sender, so the sender can never send the first authenticated
KEEPALIVE. Such a room or member stays UNCONFIRMED: it lives at most --pending-timeout seconds (a
real console keepalives right after its WELCOME), unconfirmed rooms have their own bounded table
(oldest evicted first), and they never count against the real room table. GXRV version 2 adds a
stateless HMAC cookie (as DTLS HelloVerify): a v2 CREATE / JOIN carries a 16-byte cookie the
server handed out to that exact address, so the server knows the source is real before it keeps
any state. Since 2.1.0 the cookie is REQUIRED by default: a v1 request (every card before
card-4's cookie build) gets a readable "version mismatch" ERROR; --allow-v1 is the escape hatch for
legacy cards. README.md "Protocol" has both layouts.

Version gating (2.1.0). A v2 CREATE / JOIN also carries the client protocol version (CLIENT_PROTO,
the console's GDX_MPO_PROTO). Below --min-client-proto (default: this release's) or above
--max-client-proto (default: this release's; 0 = no ceiling) the cookie-proven request gets UPDATE
(60 B: the range, the client's number and --update-url), and the console shows UPDATE REQUIRED or
SERVER NEEDS AN UPDATE. A BUILD / CONTENT mismatch on JOIN tells a v2 joiner the host's proto, so it
can say which side should update.

Operations: --metrics-port serves Prometheus text (/metrics, /healthz) on --metrics-bind; JSON
event lines go to stdout (journald) and, with --loki-url, to a Loki push endpoint; --log-ip picks
how source addresses appear in those lines. SIGTERM stops cleanly; SIGUSR1 drains (no new rooms,
exit 75 once the last room is gone or after --drain-max seconds). Under systemd Type=notify it
reports READY and pings the watchdog.

Wire format: tools/mp-rendezvous/README.md ("Protocol"). The console side is
port/gdx_mp_online.{h,c}; keep the two in step (GXRV_VERSION).

Test-only switches (never on a public instance): --allow-custom-codes, --force-relay,
--relay-delay-ms / --relay-jitter-ms / --relay-loss-pct (a latency/loss proxy for the relay path),
--sim-symmetric SLOTS (advertise a dead public port for those slots, as a symmetric NAT would).
"""

import argparse
import collections
import hashlib
import heapq
import hmac
import json
import os
import random
import secrets
import selectors
import signal
import socket
import struct
import sys
import threading
import time
import urllib.request

SERVER_VERSION = "2.1.0"

# THE CLIENT PROTOCOL VERSION this release speaks: a mirror of GDX_MPO_PROTO in
# port/gdx_mp_online.h (test_rendezvous.py asserts they match). Bump both together, on any GXRV /
# GXLB / GXMP-online or sim-affecting change.
CLIENT_PROTO = 9

GXRV_MAGIC = b"GXRV"
GXRV_VERSION = 1          # what every card shipped so far speaks
GXRV_VERSION_COOKIE = 2   # version 1 + the return-routability cookie on CREATE / JOIN
GXRV_VERSIONS = (GXRV_VERSION, GXRV_VERSION_COOKIE)

# client -> server
T_CREATE = 1
T_JOIN = 2
T_KEEPALIVE = 3
T_LEAVE = 4
T_RELAY = 5
T_PUNCH_REQ = 6
T_LOCK = 7
# server -> client
T_WELCOME = 0x81
T_ROSTER = 0x82
T_PUNCH_NOW = 0x83
T_ERROR = 0x84
T_KA_ACK = 0x85
T_COOKIE = 0x86  # v2 only
T_UPDATE = 0x87  # v2 only: the client's proto is outside [--min-client-proto, --max-client-proto]

TYPE_NAMES = {
    T_CREATE: "create", T_JOIN: "join", T_KEEPALIVE: "keepalive", T_LEAVE: "leave", T_RELAY: "relay",
    T_PUNCH_REQ: "punch_req", T_LOCK: "lock", T_WELCOME: "welcome", T_ROSTER: "roster",
    T_PUNCH_NOW: "punch_now", T_ERROR: "error", T_KA_ACK: "ka_ack", T_COOKIE: "cookie", T_UPDATE: "update",
}

E_BAD_REQUEST = 1
E_ROOM_NOT_FOUND = 2
E_ROOM_FULL = 3
E_BUILD_MISMATCH = 4
E_CONTENT_MISMATCH = 5
E_RATE_LIMITED = 6
E_SERVER_FULL = 7
E_ROOM_LOCKED = 8
E_AUTH = 9
E_HOST_LEFT = 10
E_CODE_TAKEN = 11

ERROR_NAMES = {
    E_BAD_REQUEST: "bad-request", E_ROOM_NOT_FOUND: "room-not-found", E_ROOM_FULL: "room-full",
    E_BUILD_MISMATCH: "build-mismatch", E_CONTENT_MISMATCH: "content-mismatch",
    E_RATE_LIMITED: "rate-limited", E_SERVER_FULL: "server-full", E_ROOM_LOCKED: "room-locked",
    E_AUTH: "auth", E_HOST_LEFT: "host-left", E_CODE_TAKEN: "code-taken",
}

# What a v1 console shows for a server that requires the cookie: "version mismatch: host runs <this>".
# No spaces: every card before the cookie build drops an ERROR detail with a space in it (its
# parser takes 0x21..0x7E only). Mirrored as GDX_RV_NEEDS_V2_DETAIL in port/gdx_mp_online.h.
NEEDS_V2_DETAIL = "newer-version(GXRV-v2):PLEASE-UPDATE"

HDR = 8
REQ_LEN = 84          # CREATE / JOIN
COOKIE_LEN = 16       # u32 issue time + 12-byte HMAC-SHA256 tag
REQ2_LEN = REQ_LEN + COOKIE_LEN + 4  # 104: v1 body, cookie at 84, u16 client proto at 100, u16 0
COOKIE_REPLY_LEN = HDR + 4 + COOKIE_LEN  # 28: header, nonce, cookie
UPDATE_TEXT_LEN = 40
UPDATE_LEN = HDR + 12 + UPDATE_TEXT_LEN  # 60: header, nonce, min, max, yours, 0, text
AUTH_LEN = 28         # KEEPALIVE / LEAVE / RELAY / PUNCH_REQ / LOCK prefix
KEEPALIVE_LEN = 36
MAX_DATAGRAM = 1300
MAX_RELAY_PAYLOAD = 1200
MAX_PLAYERS = 4
AUTH_TYPES = (T_KEEPALIVE, T_LEAVE, T_RELAY, T_PUNCH_REQ, T_LOCK)

CODE_ALPHABET = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"  # no I, O, 0, 1
CODE_LEN = 5
CODE_CHARS = set(CODE_ALPHABET.encode())
NAME_CHARS = set(b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 _.-")

# The game datagrams a relay may carry (port/gdx_mp_lockstep.h, port/gdx_mp_online.h).
GXMP_MAGIC = b"GXMP"
GXLB_MAGIC = b"GXLB"
GXMP_MIN = 68          # GDX_MP_HEADER_BYTES
GXMP_SLOT_OFFSET = 34  # u8 sender room slot + 1 (online sessions)
GXLB_MIN = 28
GXLB_FROM_OFFSET = 6

REJOIN_SILENT_S = 5.0     # see _join
SESSION_MIN_S = 20.0      # a member counted in gdx_rv_member_sessions_total stayed at least this long
RELAY_ACTIVE_S = 10.0     # "relaying now" in the gauges: relayed within this long


def now_s():
    return time.monotonic()


class TokenBucket:
    __slots__ = ("rate", "burst", "tokens", "stamp")

    def __init__(self, rate, burst, now):
        self.rate = float(rate)
        self.burst = float(burst)
        self.tokens = float(burst)
        self.stamp = now

    def take(self, now, cost=1.0):
        elapsed = now - self.stamp
        if elapsed > 0:
            self.tokens = min(self.burst, self.tokens + elapsed * self.rate)
            self.stamp = now
        if self.tokens >= cost:
            self.tokens -= cost
            return True
        return False


# ---- metrics ---------------------------------------------------------------------------------
METRIC_HELP = {
    "gdx_rv_rx_packets_total": ("counter", "datagrams received, by GXRV type and header version"),
    "gdx_rv_rx_bytes_total": ("counter", "bytes received, by GXRV type and header version"),
    "gdx_rv_tx_packets_total": ("counter", "datagrams sent, by type (relay = a forwarded game datagram)"),
    "gdx_rv_tx_bytes_total": ("counter", "bytes sent, by type"),
    "gdx_rv_tx_failures_total": ("counter", "sendto calls that failed"),
    "gdx_rv_relay_packets_total": ("counter", "game datagrams forwarded (one per destination)"),
    "gdx_rv_relay_bytes_total": ("counter", "game datagram bytes forwarded"),
    "gdx_rv_drops_total": ("counter", "datagrams dropped without an answer, by reason"),
    "gdx_rv_ratelimit_total": ("counter", "rate-limit hits, by limit"),
    "gdx_rv_errors_sent_total": ("counter", "GXRV ERROR replies, by code"),
    "gdx_rv_rooms_created_total": ("counter", "rooms created, by request version"),
    "gdx_rv_joins_total": ("counter", "members admitted by JOIN, by request version"),
    "gdx_rv_rejoins_total": ("counter", "restarted consoles that took their slot back"),
    "gdx_rv_rooms_closed_total": ("counter", "rooms closed, by reason"),
    "gdx_rv_members_left_total": ("counter", "members removed, by reason"),
    "gdx_rv_punch_requests_total": ("counter", "PUNCH_REQ messages answered with a PUNCH_NOW"),
    "gdx_rv_member_sessions_total": ("counter", "members that stayed at least 20 s, by whether any of "
                                     "their datagrams needed the relay (relay=no: every link punched)"),
    "gdx_rv_cookies_issued_total": ("counter", "v2 return-routability cookies handed out"),
    "gdx_rv_cookies_rejected_total": ("counter", "v2 requests whose cookie was stale or forged"),
    "gdx_rv_unconfirmed_evicted_total": ("counter", "unconfirmed rooms evicted to make room"),
    "gdx_rv_probe_pings_total": ("counter", "health-probe pings answered or rate-limited"),
    "gdx_rv_log_suppressed_total": ("counter", "event log lines dropped by the log rate limit"),
    "gdx_rv_loki_lines_total": ("counter", "event lines handed to Loki, by outcome"),
    "gdx_rv_handle_seconds": ("histogram", "time to process one datagram"),
}
HANDLE_BUCKETS = (0.00005, 0.0001, 0.00025, 0.0005, 0.001, 0.0025, 0.005, 0.01, 0.05)


class Metrics:
    """Counters keyed (name, sorted label tuple). Only the event loop thread writes; render() runs on
    that thread too, and the HTTP thread only serves the last rendered text."""

    def __init__(self):
        self.counters = collections.defaultdict(float)
        self.hist_counts = [0] * (len(HANDLE_BUCKETS) + 1)
        self.hist_sum = 0.0

    def inc(self, name, value=1, **labels):
        self.counters[(name, tuple(sorted(labels.items())))] += value

    @staticmethod
    def key(name, **labels):
        """A counter key to keep and pass to add() on a hot path (no per-call label sorting)."""
        return (name, tuple(sorted(labels.items())))

    def add(self, key, value=1):
        self.counters[key] += value

    def get(self, name, **labels):
        return self.counters.get((name, tuple(sorted(labels.items()))), 0.0)

    def total(self, name):
        return sum(v for (n, _l), v in self.counters.items() if n == name)

    def observe_handle(self, seconds):
        self.hist_sum += seconds
        for i, edge in enumerate(HANDLE_BUCKETS):
            if seconds <= edge:
                self.hist_counts[i] += 1
                return
        self.hist_counts[-1] += 1


def _label_text(labels):
    if not labels:
        return ""
    parts = []
    for k, v in labels:
        v = str(v).replace("\\", "\\\\").replace("\n", "\\n").replace('"', '\\"')
        parts.append('%s="%s"' % (k, v))
    return "{" + ",".join(parts) + "}"


def _fmt(v):
    if isinstance(v, float) and v.is_integer() and abs(v) < 1e15:
        return str(int(v))
    return repr(float(v)) if isinstance(v, float) else str(v)


def render_metrics(metrics, gauges):
    """Prometheus text format 0.0.4. gauges: list of (name, help, [(labels tuple, value)])."""
    by_name = collections.defaultdict(list)
    for (name, labels), v in metrics.counters.items():
        by_name[name].append((labels, v))
    lines = []
    for name in sorted(by_name):
        kind, text = METRIC_HELP.get(name, ("counter", name))
        lines.append("# HELP %s %s" % (name, text))
        lines.append("# TYPE %s %s" % (name, kind))
        for labels, v in sorted(by_name[name]):
            lines.append("%s%s %s" % (name, _label_text(labels), _fmt(v)))
    lines.append("# HELP gdx_rv_handle_seconds %s" % METRIC_HELP["gdx_rv_handle_seconds"][1])
    lines.append("# TYPE gdx_rv_handle_seconds histogram")
    cum = 0
    for edge, n in zip(HANDLE_BUCKETS, metrics.hist_counts):
        cum += n
        lines.append('gdx_rv_handle_seconds_bucket{le="%s"} %d' % (repr(edge), cum))
    cum += metrics.hist_counts[-1]
    lines.append('gdx_rv_handle_seconds_bucket{le="+Inf"} %d' % cum)
    lines.append("gdx_rv_handle_seconds_sum %s" % repr(metrics.hist_sum))
    lines.append("gdx_rv_handle_seconds_count %d" % cum)
    for name, text, samples in gauges:
        lines.append("# HELP %s %s" % (name, text))
        lines.append("# TYPE %s gauge" % name)
        for labels, v in samples:
            lines.append("%s%s %s" % (name, _label_text(labels), _fmt(v)))
    return "\n".join(lines) + "\n"


def host_gauges(proc="/proc"):
    """A few process / host gauges from /proc (Linux only; nothing elsewhere), so a small VM needs
    no separate node exporter."""
    out = []
    try:
        with open(os.path.join(proc, "self", "stat")) as f:
            fields = f.read().rsplit(")", 1)[1].split()
        tick = os.sysconf("SC_CLK_TCK")
        page = os.sysconf("SC_PAGE_SIZE")
        out.append(("gdx_rv_process_cpu_seconds", "server process CPU time (user + system)",
                    [((), (int(fields[11]) + int(fields[12])) / float(tick))]))
        out.append(("gdx_rv_process_resident_bytes", "server process resident memory",
                    [((), int(fields[21]) * page)]))
    except (OSError, IndexError, ValueError):
        return out
    try:
        with open(os.path.join(proc, "loadavg")) as f:
            load1 = float(f.read().split()[0])
        out.append(("gdx_rv_host_load1", "host 1-minute load average", [((), load1)]))
        mem = {}
        with open(os.path.join(proc, "meminfo")) as f:
            for line in f:
                k, v = line.split(":", 1)
                mem[k] = int(v.split()[0]) * 1024
        out.append(("gdx_rv_host_memory_bytes", "host memory", [
            ((("kind", "total"),), mem.get("MemTotal", 0)), ((("kind", "available"),), mem.get("MemAvailable", 0)),
            ((("kind", "swap_free"),), mem.get("SwapFree", 0))]))
        with open(os.path.join(proc, "stat")) as f:
            cpu = f.readline().split()[1:9]
        tick = float(os.sysconf("SC_CLK_TCK"))
        modes = ("user", "nice", "system", "idle", "iowait", "irq", "softirq", "steal")
        out.append(("gdx_rv_host_cpu_seconds", "host CPU time by mode (all cores; rate() it)",
                    [((("mode", m),), int(v) / tick) for m, v in zip(modes, cpu)]))
    except (OSError, IndexError, ValueError):
        pass
    return out


def server_sha256():
    try:
        with open(os.path.abspath(__file__), "rb") as f:
            return hashlib.sha256(f.read()).hexdigest()
    except OSError:
        return "unknown"


# ---- room state ----------------------------------------------------------------------------------
class Member:
    __slots__ = ("slot", "addr", "priv_ip", "priv_port", "name", "key", "nonce", "last_seen",
                 "relay_bucket", "ctl_bucket", "relayed", "relay_bytes", "epoch", "joined", "confirmed",
                 "proto", "last_relay", "client_proto")

    def __init__(self, slot, addr, priv_ip, priv_port, name, key, nonce, now, relay_pps, proto=GXRV_VERSION,
                 confirmed=False, client_proto=0):
        self.slot = slot
        self.addr = addr
        self.priv_ip = priv_ip
        self.priv_port = priv_port
        self.name = name
        self.key = key
        self.nonce = nonce
        self.last_seen = now
        self.joined = now
        self.epoch = 0  # the room's join counter when this member got its slot (see _roster_pkt)
        self.relay_bucket = TokenBucket(relay_pps, relay_pps * 1.5, now)
        self.ctl_bucket = TokenBucket(20, 40, now)
        self.relayed = 0
        self.relay_bytes = 0
        self.last_relay = -1e9
        # Has an authenticated message come back from this member's address yet? (A v2 member
        # proved its address with the cookie already.)
        self.confirmed = confirmed
        self.proto = proto  # the header version its replies carry
        self.client_proto = client_proto  # its v2 request's client proto (0 = a v1 card: unknown)


class Room:
    __slots__ = ("code", "build", "content", "max_players", "secret", "members", "created",
                 "roster_ver", "locked", "force_relay", "creator_ip", "relay_bucket", "joins", "orphaned",
                 "peak", "relay_bytes", "punches", "owner", "create_log")

    def __init__(self, code, build, content, max_players, force_relay, creator_ip, now, room_bps):
        self.code = code
        self.build = build
        self.content = content
        self.max_players = max_players
        self.secret = secrets.randbits(64) or 1
        self.members = {}
        self.created = now
        self.roster_ver = 1
        self.joins = 0
        self.locked = False
        self.orphaned = False
        self.force_relay = force_relay
        self.creator_ip = creator_ip
        self.relay_bucket = TokenBucket(room_bps, room_bps * 2, now)
        self.peak = 0
        self.relay_bytes = 0
        self.punches = 0
        self.owner = None  # the IpState whose per-address room count this room uses
        self.create_log = None


class IpState:
    __slots__ = ("req_bucket", "pkt_bucket", "junk", "junk_window", "blocked_until", "failed_joins",
                 "join_window", "rooms", "last_seen")

    def __init__(self, now, ip_pps):
        self.req_bucket = TokenBucket(4, 12, now)
        self.pkt_bucket = TokenBucket(ip_pps, ip_pps * 2, now)
        self.junk = 0
        self.junk_window = now
        self.blocked_until = 0.0
        self.failed_joins = 0
        self.join_window = now
        self.rooms = 0
        self.last_seen = now


def pad_bytes(s, n):
    b = s.encode("ascii")[:n]
    return b + b"\0" * (n - len(b))


def parse_fixed_ascii(raw, allowed=None, printable=False):
    """Fixed-width NUL-padded field -> str, or None if malformed (bytes after the first NUL must
    all be NUL, every byte before it must be allowed)."""
    end = raw.find(b"\0")
    body = raw if end < 0 else raw[:end]
    if end >= 0 and any(raw[end:]):
        return None
    for c in body:
        if printable and not (0x21 <= c <= 0x7E):
            return None
        if allowed is not None and c not in allowed:
            return None
    return body.decode("ascii")


def ip_to_bytes(ip):
    return socket.inet_aton(ip)


def is_private_v4(b):
    a0, a1 = b[0], b[1]
    return a0 == 10 or (a0 == 172 and 16 <= a1 <= 31) or (a0 == 192 and a1 == 168) or \
        (a0 == 169 and a1 == 254) or a0 == 127


class CookieJar:
    """Stateless return-routability cookies: 4-byte issue time (server monotonic seconds) + the first
    12 bytes of HMAC-SHA256(secret, address | port | nonce | type | time). A restart picks a new
    secret, so an old cookie fails and the console just gets a fresh one."""

    def __init__(self, lifetime_s=30.0, secret=None):
        self.secret = secret or secrets.token_bytes(32)
        self.lifetime = lifetime_s

    def _tag(self, addr, nonce, t, stamp):
        msg = struct.pack("<4sHIBI", ip_to_bytes(addr[0]), addr[1] & 0xFFFF, nonce, t, stamp)
        return hmac.new(self.secret, msg, hashlib.sha256).digest()[:12]

    def make(self, addr, nonce, t, now):
        stamp = int(now) & 0xFFFFFFFF
        return struct.pack("<I", stamp) + self._tag(addr, nonce, t, stamp)

    def check(self, cookie, addr, nonce, t, now):
        if len(cookie) != COOKIE_LEN:
            return False
        stamp = struct.unpack_from("<I", cookie, 0)[0]
        age = (int(now) - stamp) & 0xFFFFFFFF
        if age > self.lifetime:
            return False
        return hmac.compare_digest(cookie[4:], self._tag(addr, nonce, t, stamp))


class Rendezvous:
    """The protocol core. No sockets: handle() takes one datagram and returns what to send,
    as (bytes, addr, delay_s) triples, so the unit tests drive it with a fake clock."""

    def __init__(self, max_rooms=2000, max_rooms_per_ip=8, member_timeout=30.0, room_max_age=6 * 3600.0,
                 allow_custom_codes=False, force_relay=False, sim_symmetric=(), relay_delay_ms=0.0,
                 relay_jitter_ms=0.0, relay_loss_pct=0.0, relay_pps=400, room_bps=192 * 1024,
                 max_ips=50000, log=None, rng=None, relay_burst_every_s=0.0, relay_burst_ms=0.0,
                 relay_burst_slot=-1, pending_timeout=10.0, max_pending_rooms=None, require_cookie=True,
                 ip_pps=2000, cookie_pps=2000, min_client_proto=CLIENT_PROTO, max_client_proto=CLIENT_PROTO,
                 update_url=""):
        self.rooms = {}
        self.ips = {}
        self.max_rooms = max_rooms
        self.max_rooms_per_ip = max_rooms_per_ip
        self.member_timeout = member_timeout
        self.room_max_age = room_max_age
        self.allow_custom_codes = allow_custom_codes
        self.force_relay = force_relay
        self.sim_symmetric = set(sim_symmetric)
        self.relay_delay = relay_delay_ms / 1000.0
        self.relay_jitter = relay_jitter_ms / 1000.0
        self.relay_loss = relay_loss_pct / 100.0
        # Burst loss (test-only): for relay_burst_ms out of every relay_burst_every_s seconds, drop
        # every relayed datagram (only the ones slot relay_burst_slot sends, when >= 0).
        self.relay_burst_every = relay_burst_every_s
        self.relay_burst = relay_burst_ms / 1000.0
        self.relay_burst_slot = relay_burst_slot
        self.relay_pps = relay_pps
        self.room_bps = room_bps
        self.max_ips = max_ips
        self.ip_pps = ip_pps
        self.pending_timeout = pending_timeout
        self.max_pending = max_pending_rooms if max_pending_rooms is not None else max(16, max_rooms // 4)
        self.pending = collections.OrderedDict()  # code -> None: rooms whose host has not confirmed
        self.require_cookie = require_cookie
        self.min_client_proto = min_client_proto
        self.max_client_proto = max_client_proto  # 0 = no ceiling
        self.update_url = update_url
        self.cookies = CookieJar()
        self.cookie_bucket = None
        self.cookie_pps = cookie_pps
        self.draining = False
        self.log = log or (lambda *a, **k: None)
        self.rng = rng or random.Random()
        self.metrics = Metrics()
        self.stats = {"rx": 0, "tx": 0, "junk": 0, "blocked": 0, "rate_limited": 0, "relayed": 0,
                      "relay_dropped": 0, "rooms_created": 0, "rooms_expired": 0, "joins": 0,
                      "bad_auth": 0}
        self.abuse = collections.Counter()  # ip -> rate-limit / junk hits since the last stats line
        self._rx_keys = {}
        self._drop_keys = {}
        self._tx_keys = {}
        self._k_relay_pkts = Metrics.key("gdx_rv_relay_packets_total")
        self._k_relay_bytes = Metrics.key("gdx_rv_relay_bytes_total")
        # Series that exist from the start (a rate() over a counter that appears later has a gap).
        for name, labels in (("gdx_rv_relay_packets_total", {}), ("gdx_rv_relay_bytes_total", {}),
                             ("gdx_rv_member_sessions_total", {"relay": "yes"}),
                             ("gdx_rv_member_sessions_total", {"relay": "no"}),
                             ("gdx_rv_rooms_closed_total", {"why": "leave"}),
                             ("gdx_rv_rooms_created_total", {"proto": "1"}), ("gdx_rv_rooms_created_total", {"proto": "2"}),
                             ("gdx_rv_joins_total", {"proto": "1"}), ("gdx_rv_joins_total", {"proto": "2"}),
                             ("gdx_rv_cookies_issued_total", {}), ("gdx_rv_cookies_rejected_total", {}),
                             ("gdx_rv_update_required_total", {"why": "client_old"}),
                             ("gdx_rv_update_required_total", {"why": "server_old"}),
                             ("gdx_rv_unconfirmed_evicted_total", {}), ("gdx_rv_punch_requests_total", {}),
                             ("gdx_rv_drops_total", {"reason": "header"}), ("gdx_rv_tx_failures_total", {})):
            self.metrics.inc(name, 0, **labels)

    # ---- helpers ------------------------------------------------------------------------
    def _ip(self, key, now):
        st = self.ips.get(key)
        if st is None:
            if len(self.ips) >= self.max_ips:
                self._gc_ips(now, force=True)
            st = IpState(now, self.ip_pps)
            self.ips[key] = st
        st.last_seen = now
        return st

    def _drop(self, reason, n=1):
        key = self._drop_keys.get(reason)
        if key is None:
            key = self._drop_keys[reason] = Metrics.key("gdx_rv_drops_total", reason=reason)
        self.metrics.counters[key] += n

    def _limited(self, limit, ip=None):
        self.stats["rate_limited"] += 1
        self.metrics.inc("gdx_rv_ratelimit_total", limit=limit)
        if ip is not None and (ip in self.abuse or len(self.abuse) < 4096):
            self.abuse[ip] += 1

    def _junk(self, st, now, why, ip=None):
        self.stats["junk"] += 1
        self._drop(why)
        if ip is not None and (ip in self.abuse or len(self.abuse) < 4096):
            self.abuse[ip] += 1
        if now - st.junk_window > 10.0:
            st.junk_window = now
            st.junk = 0
        st.junk += 1
        if st.junk > 50 and st.blocked_until <= now:
            st.blocked_until = now + 60.0
            self.log("block", why=why, ip=ip)
        return []

    @staticmethod
    def _hdr(t, proto=GXRV_VERSION):
        return GXRV_MAGIC + bytes([proto, t, 0, 0])

    def _error(self, addr, nonce, err, detail="", proto=GXRV_VERSION, host_proto=0):
        """host_proto (bytes 14..15, 0 before 2.1.0): a BUILD / CONTENT mismatch to a v2 joiner
        carries the host's client proto, so the console can say which side should update."""
        self.metrics.inc("gdx_rv_errors_sent_total", code=ERROR_NAMES.get(err, str(err)))
        pkt = self._hdr(T_ERROR, proto) + struct.pack("<IBxH", nonce, err, host_proto & 0xFFFF) + pad_bytes(detail, 40)
        return [(pkt, addr, 0.0)]

    def _refuse(self, op, req, addr, err, detail="", proto=GXRV_VERSION, host_proto=0):
        """An ERROR answer to a CREATE / JOIN, plus its access-log line."""
        self.log("refuse", op=op, err=ERROR_NAMES.get(err, str(err)), ip=addr[0], port=addr[1],
                 code=req.get("code") or None, proto=proto)
        return self._error(addr, req["nonce"], err, detail, proto, host_proto if proto == GXRV_VERSION_COOKIE else 0)

    def _client_proto_ok(self, client_proto):
        """None when this client proto may use the server, else "client_old" / "server_old"."""
        if client_proto < self.min_client_proto:
            return "client_old"
        if self.max_client_proto and client_proto > self.max_client_proto:
            return "server_old"
        return None

    def _update(self, op, req, addr, why):
        """UPDATE (60 B, never more than the 104 B request): the range this server takes."""
        self.metrics.inc("gdx_rv_update_required_total", why=why)
        self.log("update_required", op=op, why=why, have=req["client_proto"], min=self.min_client_proto,
                 max=self.max_client_proto, ip=addr[0], port=addr[1], build=req["build"])
        pkt = self._hdr(T_UPDATE, GXRV_VERSION_COOKIE) + \
            struct.pack("<IHHHH", req["nonce"], self.min_client_proto & 0xFFFF, self.max_client_proto & 0xFFFF,
                        req["client_proto"] & 0xFFFF, 0) + pad_bytes(self.update_url, UPDATE_TEXT_LEN)
        return [(pkt, addr, 0.0)]

    def _new_code(self):
        for _ in range(64):
            code = "".join(self.rng.choice(CODE_ALPHABET) for _ in range(CODE_LEN))
            if code not in self.rooms:
                return code
        return None

    def _roster_pkt(self, room, proto=GXRV_VERSION):
        entries = b""
        for slot in sorted(room.members):
            m = room.members[slot]
            pub_ip = ip_to_bytes(m.addr[0])
            pub_port = m.addr[1]
            priv_ip = m.priv_ip
            if slot in self.sim_symmetric:
                # What a symmetric NAT looks like from outside: the port we saw is not the port a
                # peer's packet would arrive on, and the LAN address is useless.
                pub_port = ((pub_port ^ 0x5A5A) % 50000) + 10000
                priv_ip = b"\0\0\0\0"
            eflags = 1 if slot == 0 else 0
            # epoch: changes when a slot is taken by a different console (a rejoin after a restart),
            # so the others can tell "same player" from "a new one in the same slot".
            entries += struct.pack("<BBH4sHH4s", slot, eflags, m.epoch, pub_ip, pub_port, m.priv_port, priv_ip)
            entries += pad_bytes(m.name, 12)
        rflags = (1 if room.locked else 0) | (2 if room.force_relay else 0)
        return self._hdr(T_ROSTER, proto) + pad_bytes(room.code, 8) + \
            struct.pack("<IBBH", room.roster_ver, len(room.members), rflags, 0) + entries

    def _roster_to(self, room, members):
        """The roster for each of members, in each one's header version."""
        cache = {}
        out = []
        for m in members:
            if m.proto not in cache:
                cache[m.proto] = self._roster_pkt(room, m.proto)
            out.append((cache[m.proto], m.addr, 0.0))
        return out

    def _broadcast_roster(self, room):
        return self._roster_to(room, list(room.members.values()))

    def _welcome(self, room, m):
        flags = 1 if room.force_relay else 0
        pkt = self._hdr(T_WELCOME, m.proto) + struct.pack("<I", m.nonce) + pad_bytes(room.code, 8) + \
            struct.pack("<BBBxQQ4sHH", m.slot, room.max_players, flags, m.key, room.secret,
                        ip_to_bytes(m.addr[0]), m.addr[1], 0)
        return [(pkt, m.addr, 0.0)]

    def _close_room(self, room, why, now=None):
        out = []
        for m in room.members.values():
            if m.slot != 0:
                out += self._error(m.addr, 0, E_HOST_LEFT, why, m.proto)
        self._drop_room(room, why, now)
        return out

    def _member_gone(self, room, m, why, now):
        self.metrics.inc("gdx_rv_members_left_total", why=why)
        if m.confirmed and now - m.joined >= SESSION_MIN_S:
            self.metrics.inc("gdx_rv_member_sessions_total", relay="yes" if m.relayed else "no")

    def _drop_room(self, room, why, now=None):
        now = now_s() if now is None else now
        for m in room.members.values():
            self._member_gone(room, m, why, now)
        st = room.owner
        if st is not None and st.rooms > 0:
            st.rooms -= 1
        self.rooms.pop(room.code, None)
        unconfirmed = self.pending.pop(room.code, "real") is None
        self.metrics.inc("gdx_rv_rooms_closed_total", why=why)
        if unconfirmed:
            return  # never logged as created either (see _confirm): a spoof flood stays out of the log
        self.log("room-close", code=room.code, why=why, peak=room.peak, relay_bytes=room.relay_bytes,
                 punches=room.punches, age_s=round(now - room.created, 1))

    def _orphan_room(self, room, why, now):
        """The host left (or timed out) with two or more guests in the room: they are told
        (E_HOST_LEFT, as before) but the room stays up for them -- no joins, KEEPALIVE / RELAY /
        LEAVE as before -- so a race they are in can finish over relayed links. It closes when
        the last of them leaves or times out."""
        out = []
        host = room.members.pop(0, None)
        if host is not None:
            self._member_gone(room, host, why, now)
        room.orphaned = True
        room.locked = True
        room.roster_ver += 1
        for m in room.members.values():
            out += self._error(m.addr, 0, E_HOST_LEFT, why, m.proto)
        self.log("orphan", code=room.code, why=why, left=len(room.members))
        return out

    def _remove_member(self, room, slot, why, now=None):
        now = now_s() if now is None else now
        if slot == 0 and len(room.members) >= 3:
            return self._orphan_room(room, why, now)
        if slot == 0:
            return self._close_room(room, why, now)
        m = room.members.pop(slot, None)
        if m is not None:
            self._member_gone(room, m, why, now)
        if room.orphaned and not room.members:
            self._drop_room(room, "empty", now)
            return []
        room.roster_ver += 1
        self.log("leave", code=room.code, slot=slot, why=why, ip=m.addr[0] if m else None)
        return self._broadcast_roster(room)

    def _confirm(self, room, m):
        if m.confirmed:
            return
        m.confirmed = True
        if m.slot == 0 and room.code in self.pending:
            del self.pending[room.code]
            if room.create_log is not None:
                self.log("create", **room.create_log)
                room.create_log = None

    # ---- request parsing ----------------------------------------------------------------
    def _parse_request(self, data):
        if len(data) != REQ_LEN:
            return None
        nonce, content, priv_ip, priv_port, maxp, cflags = struct.unpack_from("<I I 4s H B B", data, 8)
        build = parse_fixed_ascii(data[24:64], printable=True)
        name = parse_fixed_ascii(data[64:76], allowed=NAME_CHARS)
        code = parse_fixed_ascii(data[76:84], allowed=CODE_CHARS)
        if build is None or not build or name is None or code is None:
            return None
        if code and len(code) != CODE_LEN:
            return None
        if not name.strip():
            name = "PLAYER"
        if not is_private_v4(priv_ip):
            priv_ip = b"\0\0\0\0"
        return {"nonce": nonce, "content": content, "priv_ip": priv_ip, "priv_port": priv_port,
                "max_players": maxp, "cflags": cflags, "build": build, "name": name, "code": code,
                "client_proto": 0}

    def _auth(self, data, addr, now, st, count=True):
        """Returns (room, member) for a valid auth prefix, else None (counted when count)."""
        if len(data) < AUTH_LEN:
            if count:
                self._drop("auth-short")
            return None
        code = parse_fixed_ascii(data[8:16], allowed=CODE_CHARS)
        slot, arg, rsv, key = struct.unpack_from("<BBHQ", data, 16)
        if rsv != 0:
            if count:
                self._junk(st, now, "auth-rsv", addr[0])
            return None
        room = self.rooms.get(code) if code else None
        m = room.members.get(slot) if room is not None else None
        if m is None or not secrets.compare_digest(struct.pack("<Q", m.key), struct.pack("<Q", key)):
            if count:
                self.stats["bad_auth"] += 1
                self._junk(st, now, "auth", addr[0])
            return None
        return room, m

    # ---- the dispatcher ------------------------------------------------------------------
    def handle(self, data, addr, now):
        self.stats["rx"] += 1
        n = len(data)
        valid_hdr = HDR <= n <= MAX_DATAGRAM and data[:4] == GXRV_MAGIC and data[4] in GXRV_VERSIONS and \
            data[6] == 0 and data[7] == 0
        t = data[5] if valid_hdr else -1
        proto = data[4] if valid_hdr else 0
        keys = self._rx_keys.get((t, proto))
        if keys is None:
            tname = TYPE_NAMES.get(t, "unknown") if valid_hdr and t < 0x80 else "invalid"
            keys = (Metrics.key("gdx_rv_rx_packets_total", type=tname, proto=str(proto)),
                    Metrics.key("gdx_rv_rx_bytes_total", type=tname, proto=str(proto)))
            self._rx_keys[(t, proto)] = keys
        counters = self.metrics.counters
        counters[keys[0]] += 1
        counters[keys[1]] += n
        if valid_hdr and proto == GXRV_VERSION_COOKIE and t in (T_CREATE, T_JOIN):
            # The cookie path runs before the per-IP block: a spoofer can get a victim's address
            # blocked, but it cannot stop the victim from proving its own address.
            return self._cookie_request(data, addr, now, t)
        st = self._ip(addr[0], now)
        if st.blocked_until > now:
            # Spoofed junk can block an address; an authenticated member at that address keeps
            # its room (its member key proves it is the real one).
            if not (valid_hdr and t in AUTH_TYPES and self._auth(data, addr, now, st, count=False)):
                self.stats["blocked"] += 1
                self._drop("blocked")
                return []
        if not st.pkt_bucket.take(now):
            self._limited("ip_pps", addr[0])
            self._drop("ip-pps")
            return []
        if not valid_hdr:
            return self._junk(st, now, "header", addr[0])
        if t in (T_CREATE, T_JOIN):
            if not st.req_bucket.take(now):
                self._limited("ip_req", addr[0])
                self._drop("rate-req")
                return []
            req = self._parse_request(data)
            if req is None:
                self._junk(st, now, "request", addr[0])
                return []
            if self.require_cookie:
                # A v1 console: "version mismatch: host runs a newer build (...)". 56 B < 84 B.
                return self._refuse("create" if t == T_CREATE else "join", req, addr, E_BUILD_MISMATCH,
                                    NEEDS_V2_DETAIL)
            if t == T_CREATE:
                return self._create(req, addr, now, st, GXRV_VERSION)
            return self._join(req, addr, now, st, GXRV_VERSION)
        if t == T_KEEPALIVE and n == KEEPALIVE_LEN and data[8:16] == bytes(8):
            # A health probe (gdx_rv_probe.py): a KEEPALIVE with no room code, which no console
            # sends. The same 16-byte E_AUTH answer as any failed keepalive, but not counted as
            # junk, so probes never feed the blocklist or the abuse report.
            self.metrics.inc("gdx_rv_probe_pings_total")
            if st.req_bucket.take(now):
                return [(self._error(addr, 0, E_AUTH, "", proto)[0][0][:16], addr, 0.0)]
            return []
        if t in AUTH_TYPES:
            auth = self._auth(data, addr, now, st)
            if auth is None:
                if t == T_KEEPALIVE and n == KEEPALIVE_LEN and st.req_bucket.take(now):
                    # Tell a client whose membership expired so it can rejoin. The short form
                    # (no detail) is 16 bytes: never larger than the 36-byte request.
                    return [(self._error(addr, 0, E_AUTH, "", proto)[0][0][:16], addr, 0.0)]
                return []
            room, m = auth
            if m.addr == addr:
                self._confirm(room, m)
            if t == T_RELAY:
                return self._relay(room, m, data, addr, now)
            if not m.ctl_bucket.take(now):
                self._limited("member_ctl", addr[0])
                self._drop("rate-ctl")
                return []
            if t in (T_KEEPALIVE, T_LEAVE) and data[17] != 0:
                self._drop("shape")
                return []
            if t == T_KEEPALIVE:
                return self._keepalive(room, m, data, addr, now)
            if t == T_LEAVE:
                if n != AUTH_LEN:
                    self._drop("shape")
                    return []
                return self._remove_member(room, m.slot, "leave", now)
            if t == T_PUNCH_REQ:
                return self._punch(room, m, data, now)
            if t == T_LOCK:
                if n != AUTH_LEN or m.slot != 0:
                    self._drop("shape")
                    return []
                want = data[17] != 0
                if want != room.locked:
                    room.locked = want
                    room.roster_ver += 1
                    return self._broadcast_roster(room)
                return []
        return self._junk(st, now, "type", addr[0])

    def _cookie_request(self, data, addr, now, t):
        """A v2 CREATE / JOIN: the 84-byte v1 body + a 16-byte cookie. No valid cookie: answer with
        one (28 B for a 100 B request). A valid one proves the source address, so the request runs
        on a separate per-address state ("v2:" + ip) that spoofed v1 traffic cannot poison."""
        if len(data) != REQ2_LEN:
            self._drop("request")
            return []
        nonce = struct.unpack_from("<I", data, 8)[0]
        cookie = data[REQ_LEN:REQ_LEN + COOKIE_LEN]
        client_proto, rsv = struct.unpack_from("<HH", data, REQ_LEN + COOKIE_LEN)
        if rsv != 0:
            self._drop("request")
            return []
        if not self.cookies.check(cookie, addr, nonce, t, now):
            if any(cookie):
                self.metrics.inc("gdx_rv_cookies_rejected_total")
            if self.cookie_bucket is None:
                self.cookie_bucket = TokenBucket(self.cookie_pps, self.cookie_pps, now)
            if not self.cookie_bucket.take(now):
                self._limited("cookie_global")
                self._drop("rate-cookie")
                return []
            self.metrics.inc("gdx_rv_cookies_issued_total")
            pkt = self._hdr(T_COOKIE, GXRV_VERSION_COOKIE) + struct.pack("<I", nonce) + \
                self.cookies.make(addr, nonce, t, now)
            return [(pkt, addr, 0.0)]
        st = self._ip("v2:" + addr[0], now)
        if st.blocked_until > now:
            self._drop("blocked")
            return []
        if not st.req_bucket.take(now):
            self._limited("ip_req", addr[0])
            self._drop("rate-req")
            return []
        req = self._parse_request(data[:REQ_LEN])
        if req is None:
            self._junk(st, now, "request", addr[0])
            return []
        req["client_proto"] = client_proto
        why = self._client_proto_ok(client_proto)
        if why is not None:
            # Address-proven (the cookie checked out), so the answer goes where it was asked from.
            return self._update("create" if t == T_CREATE else "join", req, addr, why)
        if t == T_CREATE:
            return self._create(req, addr, now, st, GXRV_VERSION_COOKIE)
        return self._join(req, addr, now, st, GXRV_VERSION_COOKIE)

    def _confirmed_rooms(self):
        return len(self.rooms) - len(self.pending)

    def _make_space_for_pending(self, now):
        while len(self.pending) >= self.max_pending:
            code, _ = self.pending.popitem(last=False)
            room = self.rooms.get(code)
            if room is not None:
                self.metrics.inc("gdx_rv_unconfirmed_evicted_total")
                self._drop_room(room, "unconfirmed-evicted", now)

    def _create(self, req, addr, now, st, proto):
        # Retransmitted CREATE (the WELCOME was lost): answer with the same room.
        for room in self.rooms.values():
            host = room.members.get(0)
            if host is not None and host.addr == addr and host.nonce == req["nonce"]:
                return self._welcome(room, host)
        if not 2 <= req["max_players"] <= MAX_PLAYERS:
            return self._refuse("create", req, addr, E_BAD_REQUEST, "max players", proto)
        verified = proto == GXRV_VERSION_COOKIE
        if self.draining or st.rooms >= self.max_rooms_per_ip or \
                (verified and self._confirmed_rooms() >= self.max_rooms):
            return self._refuse("create", req, addr, E_SERVER_FULL, "", proto)
        if not verified:
            if self._confirmed_rooms() >= self.max_rooms:
                return self._refuse("create", req, addr, E_SERVER_FULL, "", proto)
            self._make_space_for_pending(now)
        code = req["code"]
        if code and not self.allow_custom_codes:
            code = ""
        if code and code in self.rooms:
            room = self.rooms[code]
            host = room.members.get(0)
            if (host is not None and not room.orphaned and host.addr == addr and now - host.last_seen > REJOIN_SILENT_S and
                    req["build"] == room.build and req["content"] == room.content):
                # The host restarted and asks for its own code again (same address, new nonce,
                # silent past REJOIN_SILENT_S): it takes slot 0 back, as a JOIN would (see _join).
                return self._rehost(room, req, addr, now, proto)
            return self._refuse("create", req, addr, E_CODE_TAKEN, code, proto)
        if not code:
            code = self._new_code()
            if code is None:
                return self._refuse("create", req, addr, E_SERVER_FULL, "", proto)
        room = Room(code, req["build"], req["content"], req["max_players"],
                    self.force_relay or bool(req["cflags"] & 1), addr[0], now, self.room_bps)
        host = Member(0, addr, req["priv_ip"], req["priv_port"], req["name"], secrets.randbits(64) or 1,
                      req["nonce"], now, self.relay_pps, proto, confirmed=verified, client_proto=req["client_proto"])
        room.members[0] = host
        room.peak = 1
        self.rooms[code] = room
        if not verified:
            self.pending[code] = None
        room.owner = st
        st.rooms += 1
        self.stats["rooms_created"] += 1
        self.metrics.inc("gdx_rv_rooms_created_total", proto=str(proto))
        # The access-log line: at once for a v2 (cookie-proven) host, else when the host's first
        # authenticated message arrives, so spoofed CREATEs never reach the log.
        room.create_log = dict(code=code, build=req["build"], players=req["max_players"], ip=addr[0], port=addr[1],
                               proto=proto, relay_only=room.force_relay)
        if verified:
            self.log("create", **room.create_log)
            room.create_log = None
        # WELCOME only (48 bytes < the 84-byte request): the roster follows the first keepalive,
        # so a spoofed CREATE cannot reflect more bytes than it cost.
        return self._welcome(room, host)

    def _rehost(self, room, req, addr, now, proto):
        old = room.members.get(0)
        host = Member(0, addr, req["priv_ip"], req["priv_port"], req["name"], secrets.randbits(64) or 1,
                      req["nonce"], now, self.relay_pps, proto, confirmed=old.confirmed if old else False,
                      client_proto=req["client_proto"])
        room.joins = (room.joins + 1) & 0xFFFF
        host.epoch = room.joins
        room.members[0] = host
        room.locked = False
        room.roster_ver += 1
        self.log("rehost", code=room.code, ip=addr[0], port=addr[1], proto=proto)
        return self._welcome(room, host) + self._roster_to(room, [o for o in room.members.values() if o is not host])

    @staticmethod
    def _host_proto(room):
        host = room.members.get(0)
        return host.client_proto if host is not None else 0

    def _join(self, req, addr, now, st, proto):
        room = self.rooms.get(req["code"]) if req["code"] else None
        if room is not None:
            for m in room.members.values():
                if m.addr == addr and m.nonce == req["nonce"]:
                    return self._welcome(room, m)
        if now - st.join_window > 60.0:
            st.join_window = now
            st.failed_joins = 0
        if st.failed_joins >= 10:
            self._limited("join_fail", addr[0])
            return self._refuse("join", req, addr, E_RATE_LIMITED, "", proto)
        if room is None:
            st.failed_joins += 1
            return self._refuse("join", req, addr, E_ROOM_NOT_FOUND, "", proto)
        if req["build"] != room.build:
            return self._refuse("join", req, addr, E_BUILD_MISMATCH, room.build, proto, self._host_proto(room))
        if req["content"] != room.content:
            return self._refuse("join", req, addr, E_CONTENT_MISMATCH, "%08x" % room.content, proto,
                                self._host_proto(room))
        if room.locked:
            return self._refuse("join", req, addr, E_ROOM_LOCKED, "", proto)
        slot = next((s for s in range(1, room.max_players) if s not in room.members), None)
        # A console that restarted (crash, power cycle) and joins again from the SAME address with a
        # new nonce takes its old slot back -- else its ghost holds the slot until member-timeout.
        # Only a member silent for REJOIN_SILENT_S can be replaced, so a spoofed JOIN cannot evict a
        # live console (it keepalives every 2 s).
        stale = next((m for m in room.members.values()
                      if m.slot != 0 and m.addr == addr and now - m.last_seen > REJOIN_SILENT_S), None)
        if stale is not None:
            slot = stale.slot
            del room.members[slot]
            self._member_gone(room, stale, "rejoin", now)
            self.metrics.inc("gdx_rv_rejoins_total")
            self.log("rejoin", code=room.code, slot=slot, ip=addr[0], port=addr[1])
        if slot is None:
            return self._refuse("join", req, addr, E_ROOM_FULL, "", proto)
        m = Member(slot, addr, req["priv_ip"], req["priv_port"], req["name"], secrets.randbits(64) or 1,
                   req["nonce"], now, self.relay_pps, proto, confirmed=proto == GXRV_VERSION_COOKIE,
                   client_proto=req["client_proto"])
        room.joins = (room.joins + 1) & 0xFFFF
        m.epoch = room.joins
        room.members[slot] = m
        room.peak = max(room.peak, len(room.members))
        room.roster_ver += 1
        self.stats["joins"] += 1
        self.metrics.inc("gdx_rv_joins_total", proto=str(proto))
        self.log("join", code=room.code, slot=slot, ip=addr[0], port=addr[1], proto=proto)
        # The existing members hear about the newcomer now; the newcomer itself gets the roster on
        # its first keepalive (its WELCOME is the only reply to an unauthenticated request).
        return self._welcome(room, m) + self._roster_to(room, [o for o in room.members.values() if o is not m])

    def _keepalive(self, room, m, data, addr, now):
        if len(data) != KEEPALIVE_LEN:
            self._drop("shape")
            return []
        roster_ver, send_ms = struct.unpack_from("<II", data, 28)
        out = []
        m.last_seen = now
        if m.addr != addr:
            # NAT rebinding: an authenticated member moved. Follow it and tell the room.
            m.addr = addr
            room.roster_ver += 1
            out += self._broadcast_roster(room)
        ack = self._hdr(T_KA_ACK, m.proto) + struct.pack("<II", room.roster_ver, send_ms)
        out.append((ack, m.addr, 0.0))
        if roster_ver != room.roster_ver and not out[:-1]:
            out.append((self._roster_pkt(room, m.proto), m.addr, 0.0))
        return out

    def _punch(self, room, m, data, now):
        if len(data) != AUTH_LEN:
            self._drop("shape")
            return []
        target = room.members.get(data[17])
        if target is None or target.slot == m.slot:
            self._drop("punch-target")
            return []
        room.punches += 1
        self.metrics.inc("gdx_rv_punch_requests_total")
        pkt = self._hdr(T_PUNCH_NOW, target.proto) + pad_bytes(room.code, 8) + struct.pack("<B3x", m.slot)
        return [(pkt, target.addr, 0.0)]

    def _relay_drop(self, reason, n=1):
        self.stats["relay_dropped"] += n
        self._drop(reason, n)
        return []

    def _relay(self, room, m, data, addr, now):
        payload = data[AUTH_LEN:]
        mask = data[17]
        m.last_seen = now
        if not payload or len(payload) > MAX_RELAY_PAYLOAD:
            return self._relay_drop("relay-size")
        magic = payload[:4]
        if magic == GXMP_MAGIC:
            if len(payload) < GXMP_MIN or payload[GXMP_SLOT_OFFSET] != m.slot + 1:
                return self._relay_drop("relay-spoof")
        elif magic == GXLB_MAGIC:
            if len(payload) < GXLB_MIN or payload[GXLB_FROM_OFFSET] != m.slot:
                return self._relay_drop("relay-spoof")
        else:
            return self._relay_drop("relay-magic")
        targets = [room.members[s] for s in sorted(room.members) if s != m.slot and (mask >> s) & 1]
        if not targets:
            self._drop("relay-no-target")
            return []
        if not m.relay_bucket.take(now):
            self._limited("member_relay", addr[0])
            self._drop("rate-relay-member")
            return []
        if not room.relay_bucket.take(now, len(payload) * len(targets)):
            self._limited("room_relay_bytes", addr[0])
            self._drop("rate-relay-room")
            return []
        if m.addr != addr:
            m.addr = addr
            room.roster_ver += 1
        out = []
        if (self.relay_burst_every > 0 and self.relay_burst > 0 and
                (self.relay_burst_slot < 0 or self.relay_burst_slot == m.slot) and
                (now % self.relay_burst_every) < self.relay_burst):
            return self._relay_drop("relay-sim-loss", len(targets))
        m.last_relay = now
        for t in targets:
            if self.relay_loss > 0 and self.rng.random() < self.relay_loss:
                self._relay_drop("relay-sim-loss")
                continue
            delay = self.relay_delay
            if self.relay_jitter > 0:
                delay += self.rng.random() * self.relay_jitter
            out.append((payload, t.addr, delay))
            m.relayed += 1
            m.relay_bytes += len(payload)
            room.relay_bytes += len(payload)
            self.stats["relayed"] += 1
            self.metrics.counters[self._k_relay_pkts] += 1
            self.metrics.counters[self._k_relay_bytes] += len(payload)
        return out

    # ---- housekeeping --------------------------------------------------------------------
    def expire(self, now):
        out = []
        for room in list(self.rooms.values()):
            if now - room.created > self.room_max_age:
                self.stats["rooms_expired"] += 1
                out += self._close_room(room, "expired", now)
                continue
            for slot in sorted(room.members):
                m = room.members.get(slot)
                if m is None:
                    continue
                if not m.confirmed and now - m.joined > self.pending_timeout:
                    why = "unconfirmed"  # its WELCOME went somewhere that never answered: spoofed
                elif now - m.last_seen > self.member_timeout:
                    why = "timeout"
                else:
                    continue
                out += self._remove_member(room, slot, why, now)
                if room.code not in self.rooms:
                    self.stats["rooms_expired"] += 1
                    break
        self._gc_ips(now)
        return out

    def _gc_ips(self, now, force=False):
        horizon = 60.0 if force else 600.0
        for ip in [ip for ip, st in self.ips.items()
                   if st.rooms == 0 and st.blocked_until < now and now - st.last_seen > horizon]:
            del self.ips[ip]
        if force and len(self.ips) >= self.max_ips:
            # Still full of fresh sources (a spoofed-source flood): evict the least recently seen
            # tenth that own no room. Their only state is rate-limit bookkeeping.
            idle = sorted((st.last_seen, ip) for ip, st in self.ips.items() if st.rooms == 0)
            for _seen, ip in idle[:max(1, self.max_ips // 10)]:
                del self.ips[ip]

    # ---- metrics -------------------------------------------------------------------------
    def count_tx(self, pkt, ok):
        if not ok:
            self.metrics.inc("gdx_rv_tx_failures_total")
            return
        self.stats["tx"] += 1
        t = pkt[5] if pkt[:4] == GXRV_MAGIC and len(pkt) >= HDR else -1
        keys = self._tx_keys.get(t)
        if keys is None:
            kind = TYPE_NAMES.get(t, "unknown") if t >= 0 else "relay"
            keys = (Metrics.key("gdx_rv_tx_packets_total", type=kind), Metrics.key("gdx_rv_tx_bytes_total", type=kind))
            self._tx_keys[t] = keys
        counters = self.metrics.counters
        counters[keys[0]] += 1
        counters[keys[1]] += len(pkt)

    def gauges(self, now):
        states = collections.Counter()
        members = 0
        relaying = 0
        unconfirmed_members = 0
        for code, room in self.rooms.items():
            if code in self.pending:
                states["unconfirmed"] += 1
            elif room.orphaned:
                states["orphaned"] += 1
            elif room.locked:
                states["locked"] += 1
            else:
                states["open"] += 1
            for m in room.members.values():
                members += 1
                if not m.confirmed:
                    unconfirmed_members += 1
                if now - m.last_relay <= RELAY_ACTIVE_S:
                    relaying += 1
        blocked = sum(1 for st in self.ips.values() if st.blocked_until > now)
        return [
            ("gdx_rv_rooms", "rooms by state (locked = a race in progress or the host locked it)",
             [((("state", s),), states.get(s, 0)) for s in ("open", "locked", "orphaned", "unconfirmed")]),
            ("gdx_rv_members", "members in all rooms", [((), members)]),
            ("gdx_rv_members_unconfirmed", "members whose address has not answered yet", [((), unconfirmed_members)]),
            ("gdx_rv_members_relaying", "members that relayed in the last 10 s", [((), relaying)]),
            ("gdx_rv_ip_states", "per-address rate-limit states held", [((), len(self.ips))]),
            ("gdx_rv_ips_blocked", "addresses on the junk blocklist now", [((), blocked)]),
            ("gdx_rv_draining", "1 while draining for a restart", [((), 1 if self.draining else 0)]),
            ("gdx_rv_require_cookie", "1 when v1 CREATE / JOIN are refused", [((), 1 if self.require_cookie else 0)]),
            ("gdx_rv_min_client_proto", "the lowest client proto a v2 request may carry", [((), self.min_client_proto)]),
            ("gdx_rv_max_client_proto", "the highest client proto a v2 request may carry (0 = any)",
             [((), self.max_client_proto)]),
        ]


# ---- logging ---------------------------------------------------------------------------------------
NOISY_EVENTS = ("block", "refuse", "abuse")


class EventLog:
    """One JSON object (or one text line) per event, to stdout and optionally to Loki. Source
    addresses ("ip" fields) are written per --log-ip: full, prefix (/24), hash (keyed HMAC, the key
    changes every 24 h and at every restart, so a hash links events within a day only) or none.
    Noisy events (blocks, refusals) share a token bucket so a flood cannot flood the log."""

    def __init__(self, verbose=True, json_lines=True, ip_mode="full", metrics=None, sink=None,
                 noisy_rate=10.0, noisy_burst=100.0, clock=now_s, wall=time.time, stream=None):
        self.verbose = verbose
        self.json_lines = json_lines
        self.ip_mode = ip_mode
        self.metrics = metrics
        self.sink = sink
        self.clock = clock
        self.wall = wall
        self.stream = stream
        self.noisy = TokenBucket(noisy_rate, noisy_burst, clock())
        self._salt = secrets.token_bytes(16)
        self._salt_at = clock()

    def bind_metrics(self, metrics):
        self.metrics = metrics

    def _ip(self, ip):
        if ip is None:
            return None
        if self.ip_mode == "full":
            return ip
        if self.ip_mode == "none":
            return None
        if self.ip_mode == "prefix":
            parts = ip.split(".")
            return ".".join(parts[:3] + ["0"]) + "/24" if len(parts) == 4 else ip
        if self.clock() - self._salt_at > 86400:
            self._salt = secrets.token_bytes(16)
            self._salt_at = self.clock()
        return hmac.new(self._salt, ip.encode(), hashlib.sha256).hexdigest()[:16]

    def __call__(self, event, **fields):
        if event in NOISY_EVENTS and not self.noisy.take(self.clock()):
            if self.metrics is not None:
                self.metrics.inc("gdx_rv_log_suppressed_total", event=event)
            return
        if "ip" in fields:
            fields["ip"] = self._ip(fields["ip"])
            if fields["ip"] is None:
                del fields["ip"]
            if self.ip_mode == "none":
                fields.pop("port", None)
        fields = {k: v for k, v in fields.items() if v is not None}
        fields["event"] = event
        fields["t"] = round(self.wall(), 3)
        line = json.dumps(fields, sort_keys=True)
        if self.sink is not None:
            self.sink.push(event, fields["t"], line)
        if not self.verbose:
            return
        out = self.stream or sys.stdout
        if self.json_lines:
            print(line, file=out, flush=True)
        else:
            text = " ".join("%s=%s" % kv for kv in sorted(fields.items()) if kv[0] not in ("event", "t"))
            print("[rdv] %s %s" % (event, text), file=out, flush=True)


def make_logger(verbose, json_lines):
    """The old helper (kept for scripts that import it)."""
    return EventLog(verbose=verbose, json_lines=json_lines)


class LokiSink:
    """Pushes event lines to a Loki /loki/api/v1/push endpoint from a background thread: bounded
    queue (drop-oldest when full), batches every flush_s or batch lines, a 5 s HTTP timeout, a failed
    batch is dropped (counted) rather than retried forever. Labels: job, host, event."""

    def __init__(self, url, labels, metrics, max_queue=5000, batch=500, flush_s=2.0, timeout_s=5.0):
        self.url = url
        self.labels = dict(labels)
        self.metrics = metrics
        self.q = collections.deque(maxlen=max_queue)
        self.batch = batch
        self.flush_s = flush_s
        self.timeout = timeout_s
        self.lock = threading.Lock()
        self.wake = threading.Event()
        self.stopping = False
        self.outcomes = collections.Counter()  # read by the event loop into metrics (thread-safe enough)
        self.healthy = True
        self.thread = threading.Thread(target=self._run, name="loki", daemon=True)

    def start(self):
        self.thread.start()
        return self

    def push(self, event, t, line):
        with self.lock:
            if len(self.q) == self.q.maxlen:
                self.outcomes["dropped_queue"] += 1
            self.q.append((event, t, line))
        if len(self.q) >= self.batch:
            self.wake.set()

    def _take(self):
        with self.lock:
            items = [self.q.popleft() for _ in range(min(self.batch, len(self.q)))]
        return items

    def body(self, items):
        streams = collections.defaultdict(list)
        for event, t, line in items:
            streams[event].append([str(int(t * 1e9)), line])
        return json.dumps({"streams": [{"stream": dict(self.labels, event=ev), "values": vals}
                                       for ev, vals in sorted(streams.items())]}).encode()

    def send(self, items):
        req = urllib.request.Request(self.url, data=self.body(items), method="POST",
                                     headers={"Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as r:
                r.read()
            self.outcomes["sent"] += len(items)
            return True
        except Exception:  # noqa: BLE001 -- network, HTTP status, anything: drop and count
            self.outcomes["failed"] += len(items)
            return False

    def _run(self):
        backoff = 0.0
        self.healthy = True
        while not self.stopping:
            self.wake.wait(max(self.flush_s, backoff))
            self.wake.clear()
            while self.q and not self.stopping:
                self.healthy = self.send(self._take())
                if self.healthy:
                    backoff = 0.0
                else:
                    # Loki down (or switched off): wait longer each time, up to a minute; the
                    # bounded queue keeps the newest lines meanwhile.
                    backoff = min(60.0, max(2.0, backoff * 2))
                    break

    def flush(self):
        while self.q:
            self.send(self._take())

    def stop(self):
        """At exit: one last short push if Loki was answering, never a long wait (systemd's stop
        timeout is 10 s)."""
        self.stopping = True
        self.wake.set()
        self.thread.join(timeout=2.0)
        if self.healthy and self.q and not self.thread.is_alive():
            self.timeout = 1.0
            self.send(self._take())


# ---- the HTTP side (metrics) -------------------------------------------------------------------
class MetricsServer:
    """/metrics (Prometheus text) and /healthz (200 while the event loop ticked in the last 5 s) on a
    daemon thread. The event loop renders the text; this thread only hands it out."""

    def __init__(self, bind, port):
        import http.server

        owner = self
        self.text = "# no scrape yet\n"
        self.loop_stamp = now_s()

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):  # noqa: N802 -- the stdlib name
                if self.path.split("?")[0] == "/metrics":
                    body = owner.text.encode()
                    self._reply(200, body, "text/plain; version=0.0.4; charset=utf-8")
                elif self.path.split("?")[0] == "/healthz":
                    ok = now_s() - owner.loop_stamp < 5.0
                    self._reply(200 if ok else 503, b"ok\n" if ok else b"stalled\n", "text/plain")
                else:
                    self._reply(404, b"not found\n", "text/plain")

            def _reply(self, code, body, ctype):
                self.send_response(code)
                self.send_header("Content-Type", ctype)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, *_a):
                pass

        self.httpd = http.server.ThreadingHTTPServer((bind, port), Handler)
        self.httpd.daemon_threads = True
        self.port = self.httpd.server_address[1]
        self.thread = threading.Thread(target=self.httpd.serve_forever, name="metrics", daemon=True)

    def start(self):
        self.thread.start()
        return self

    def stop(self):
        self.httpd.shutdown()
        self.httpd.server_close()


def sd_notify(state):
    """systemd's notify protocol without libsystemd (a no-op outside Type=notify)."""
    path = os.environ.get("NOTIFY_SOCKET")
    if not path:
        return False
    if path.startswith("@"):
        path = "\0" + path[1:]
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        try:
            s.sendto(state.encode(), path)
        finally:
            s.close()
        return True
    except OSError:
        return False


def serve(args):
    metrics_holder = []
    sink = None
    log = EventLog(verbose=not args.quiet, json_lines=args.json, ip_mode=args.log_ip)
    core = Rendezvous(max_rooms=args.max_rooms, max_rooms_per_ip=args.max_rooms_per_ip,
                      member_timeout=args.member_timeout, room_max_age=args.room_max_age,
                      allow_custom_codes=args.allow_custom_codes, force_relay=args.force_relay,
                      sim_symmetric=[int(s) for s in args.sim_symmetric.split(",") if s.strip()],
                      relay_delay_ms=args.relay_delay_ms, relay_jitter_ms=args.relay_jitter_ms,
                      relay_loss_pct=args.relay_loss_pct, relay_pps=args.relay_pps, room_bps=args.room_bps, log=log,
                      rng=random.Random(args.seed) if args.seed is not None else None,
                      relay_burst_every_s=args.relay_burst_every_s, relay_burst_ms=args.relay_burst_ms,
                      relay_burst_slot=args.relay_burst_slot, pending_timeout=args.pending_timeout,
                      require_cookie=not args.allow_v1, ip_pps=args.ip_pps,
                      min_client_proto=args.min_client_proto, max_client_proto=args.max_client_proto,
                      update_url=args.update_url)
    log.bind_metrics(core.metrics)
    metrics_holder.append(core.metrics)
    if args.loki_url:
        labels = {"job": "gdx-rendezvous", "host": args.loki_host or socket.gethostname()}
        sink = LokiSink(args.loki_url, labels, core.metrics).start()
        log.sink = sink
    sha = server_sha256()
    started_wall = time.time()
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
    except OSError:
        pass
    sock.bind((args.bind, args.port))
    sock.setblocking(False)
    if args.port_file:
        with open(args.port_file, "w") as f:
            f.write("%d\n" % sock.getsockname()[1])
    http = None
    if args.metrics_port:
        http = MetricsServer(args.metrics_bind, args.metrics_port if args.metrics_port > 0 else 0).start()
        if args.metrics_port_file:
            with open(args.metrics_port_file, "w") as f:
                f.write("%d\n" % http.port)
    sel = selectors.DefaultSelector()
    sel.register(sock, selectors.EVENT_READ)
    pending = []  # heap of (due, seq, bytes, addr): the relay latency proxy
    seq = 0
    next_expire = now_s() + 1.0
    next_stats = now_s() + args.stats_every if args.stats_every > 0 else None
    drain_deadline = None
    stop = {"why": None}
    loop_lag = [0.0]
    sink_seen = collections.Counter()

    def on_term(_sig, _frm):
        stop["why"] = "sigterm"

    def on_drain(_sig, _frm):
        stop["drain"] = True

    signal.signal(signal.SIGTERM, on_term)
    if hasattr(signal, "SIGUSR1"):
        signal.signal(signal.SIGUSR1, on_drain)

    log("listen", bind=args.bind, port=sock.getsockname()[1], version=SERVER_VERSION, sha256=sha,
        force_relay=args.force_relay, relay_delay_ms=args.relay_delay_ms, relay_jitter_ms=args.relay_jitter_ms,
        relay_loss_pct=args.relay_loss_pct, sim_symmetric=args.sim_symmetric, custom_codes=args.allow_custom_codes,
        require_cookie=not args.allow_v1, client_proto=CLIENT_PROTO, min_client_proto=args.min_client_proto,
        max_client_proto=args.max_client_proto, update_url=args.update_url or None,
        metrics_port=http.port if http else 0, log_ip=args.log_ip,
        loki=bool(args.loki_url))
    sd_notify("READY=1\nSTATUS=listening on %s:%d" % (args.bind, sock.getsockname()[1]))

    def send(pkt, addr):
        try:
            sock.sendto(pkt, addr)
            core.count_tx(pkt, True)
        except (BlockingIOError, OSError):
            core.count_tx(pkt, False)

    def emit(items, t):
        nonlocal seq
        for pkt, addr, delay in items:
            if delay > 0:
                seq += 1
                heapq.heappush(pending, (t + delay, seq, pkt, addr))
            else:
                send(pkt, addr)

    def render(t):
        if sink is not None:
            for k, v in list(sink.outcomes.items()):
                if v != sink_seen[k]:
                    core.metrics.inc("gdx_rv_loki_lines_total", v - sink_seen[k], outcome=k)
                    sink_seen[k] = v
        g = core.gauges(t)
        g.append(("gdx_rv_build_info", "server version and the sha256 of the running file",
                  [((("version", SERVER_VERSION), ("sha256", sha), ("proto", "1,2")), 1)]))
        g.append(("gdx_rv_start_time_seconds", "unix time the server started", [((), round(started_wall, 3))]))
        g.append(("gdx_rv_loop_lag_seconds", "worst event-loop wake-up lateness since the last scrape tick",
                  [((), round(loop_lag[0], 6))]))
        g.append(("gdx_rv_relay_queue", "datagrams held by the test-only relay delay proxy", [((), len(pending))]))
        g += host_gauges()
        http.text = render_metrics(core.metrics, g)
        http.loop_stamp = t
        loop_lag[0] = 0.0

    try:
        while stop["why"] is None:
            t = now_s()
            timeout = max(0.0, min(next_expire, pending[0][0] if pending else next_expire) - t)
            timeout = min(timeout, 0.25)
            events = sel.select(timeout=timeout)
            woke = now_s()
            if not events:
                loop_lag[0] = max(loop_lag[0], woke - t - timeout)
            for _key, _ev in events:
                for _ in range(256):
                    try:
                        data, addr = sock.recvfrom(2048)
                    except (BlockingIOError, InterruptedError):
                        break
                    except OSError:
                        break
                    t0 = now_s()
                    emit(core.handle(data, addr, t0), t0)
                    core.metrics.observe_handle(now_s() - t0)
            t = now_s()
            while pending and pending[0][0] <= t:
                _due, _seq, pkt, addr = heapq.heappop(pending)
                send(pkt, addr)
            if stop.pop("drain", False) and not core.draining:
                core.draining = True
                drain_deadline = t + args.drain_max
                log("drain", rooms=len(core.rooms), max_s=args.drain_max)
                sd_notify("STATUS=draining")
            if t >= next_expire:
                emit(core.expire(t), t)
                next_expire = t + 1.0
                sd_notify("WATCHDOG=1")
                if http is not None:
                    render(t)
                if core.draining and (not core.rooms or t >= drain_deadline):
                    stop["why"] = "drained"
            if next_stats is not None and t >= next_stats:
                log("stats", rooms=len(core.rooms), unconfirmed=len(core.pending), ips=len(core.ips), **core.stats)
                if core.abuse:
                    top = core.abuse.most_common(5)
                    log("abuse", top=[[log._ip(ip) or "-", n] for ip, n in top], sources=len(core.abuse))
                    core.abuse.clear()
                next_stats = t + args.stats_every
    except KeyboardInterrupt:
        stop["why"] = "interrupt"
    finally:
        sd_notify("STOPPING=1")
        log("stop", why=stop["why"], rooms=len(core.rooms), **core.stats)
        if http is not None:
            http.stop()
        if sink is not None:
            sink.stop()
        sock.close()
    return 75 if stop["why"] == "drained" else 0


def main(argv=None):
    p = argparse.ArgumentParser(description="G-Diffuser-3DS online multiplayer rendezvous + relay server")
    p.add_argument("--version", action="store_true", help="print the version and the file's sha256, then exit")
    p.add_argument("--bind", default="0.0.0.0", help="address to listen on (default 0.0.0.0)")
    p.add_argument("--port", type=int, default=27200, help="UDP port (default 27200; 0 = pick one)")
    p.add_argument("--port-file", help="write the bound port here (for --port 0)")
    p.add_argument("--max-rooms", type=int, default=2000)
    p.add_argument("--max-rooms-per-ip", type=int, default=8)
    p.add_argument("--member-timeout", type=float, default=30.0, help="seconds without a keepalive")
    p.add_argument("--pending-timeout", type=float, default=10.0,
                   help="seconds an unconfirmed room / member (no authenticated message yet) lives")
    p.add_argument("--room-max-age", type=float, default=6 * 3600.0, help="seconds")
    p.add_argument("--relay-pps", type=int, default=400, help="relayed datagrams per second per member")
    p.add_argument("--room-bps", type=int, default=192 * 1024,
                   help="relayed bytes per second per room (4 relay-only members at RTT 200: ~90 KB/s)")
    p.add_argument("--ip-pps", type=int, default=2000, help="datagrams per second from one address, all types")
    p.add_argument("--require-cookie", action="store_true",
                   help="refuse v1 CREATE / JOIN (the default since 2.1.0; kept for old unit files)")
    p.add_argument("--allow-v1", "--no-require-cookie", dest="allow_v1", action="store_true",
                   help="legacy escape hatch: also admit v1 CREATE / JOIN (cards before the cookie build; "
                        "no return-routability proof, no client-proto check)")
    p.add_argument("--min-client-proto", type=int, default=CLIENT_PROTO,
                   help="v2 clients below this proto get UPDATE REQUIRED (default %d, this release's)" % CLIENT_PROTO)
    p.add_argument("--max-client-proto", type=int, default=CLIENT_PROTO,
                   help="v2 clients above this proto are told the server needs an update (default %d; 0 = no "
                        "ceiling)" % CLIENT_PROTO)
    p.add_argument("--update-url", default="",
                   help="up to 40 printable ASCII characters shown with UPDATE REQUIRED (default none)")
    p.add_argument("--stats-every", type=float, default=60.0, help="stats log line period (0 = off)")
    p.add_argument("--quiet", action="store_true")
    p.add_argument("--json", action="store_true", help="JSON log lines")
    p.add_argument("--log-ip", choices=["full", "prefix", "hash", "none"], default="full",
                   help="how source addresses appear in the log (default full)")
    p.add_argument("--metrics-port", type=int, default=0, help="Prometheus /metrics TCP port (0 = off, -1 = any)")
    p.add_argument("--metrics-bind", default="127.0.0.1", help="address for /metrics (default 127.0.0.1)")
    p.add_argument("--metrics-port-file", help="write the bound metrics port here")
    p.add_argument("--loki-url", default="", help="push event lines here (http://host:3100/loki/api/v1/push)")
    p.add_argument("--loki-host", default="", help="the host label on pushed lines (default: hostname)")
    p.add_argument("--drain-max", type=float, default=1800.0, help="SIGUSR1 drain gives up after this many s")
    p.add_argument("--seed", type=int, help="seed the code generator / proxy (tests)")
    g = p.add_argument_group("test-only (never on a public instance)")
    g.add_argument("--allow-custom-codes", action="store_true", help="honour a CREATE's requested code")
    g.add_argument("--force-relay", action="store_true", help="every room is relay-only")
    g.add_argument("--sim-symmetric", default="", help="comma list of slots advertised as behind symmetric NAT")
    g.add_argument("--relay-delay-ms", type=float, default=0.0, help="added one-way delay on relayed packets")
    g.add_argument("--relay-jitter-ms", type=float, default=0.0, help="uniform extra 0..N ms per relayed packet")
    g.add_argument("--relay-loss-pct", type=float, default=0.0, help="drop this %% of relayed packets")
    g.add_argument("--relay-burst-every-s", type=float, default=0.0, help="a loss burst every N seconds")
    g.add_argument("--relay-burst-ms", type=float, default=0.0, help="each burst drops every relayed packet this long")
    g.add_argument("--relay-burst-slot", type=int, default=-1, help="burst only what this slot sends (-1 = all)")
    args = p.parse_args(argv)
    if args.version:
        print("gdx_rendezvous %s sha256=%s proto=1,2 client_proto=%d" % (SERVER_VERSION, server_sha256(), CLIENT_PROTO))
        return 0
    if not 0 <= args.port <= 65535:
        p.error("bad port")
    if args.require_cookie and args.allow_v1:
        p.error("--require-cookie and --allow-v1 contradict each other")
    if not 0 <= args.min_client_proto <= 65535 or not 0 <= args.max_client_proto <= 65535:
        p.error("client protos are 0..65535")
    if args.max_client_proto and args.max_client_proto < args.min_client_proto:
        p.error("--max-client-proto is below --min-client-proto")
    if len(args.update_url) > UPDATE_TEXT_LEN or any(not 0x20 <= ord(c) <= 0x7E for c in args.update_url):
        p.error("--update-url: at most %d printable ASCII characters" % UPDATE_TEXT_LEN)
    return serve(args)


if __name__ == "__main__":
    sys.exit(main())
