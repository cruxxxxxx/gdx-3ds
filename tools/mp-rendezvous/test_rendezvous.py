#!/usr/bin/env python3
"""Host tests for tools/mp-rendezvous/gdx_rendezvous.py (stdlib unittest, no network for the
core tests; the interop test starts the real server on a loopback port and drives it with the
console's own C code, port/tests/gdx_mp_online_wire_probe.c, when a C compiler is present).

    python3 tools/mp-rendezvous/test_rendezvous.py        # run by tools/ci-3ds.sh
"""

import json
import os
import random
import re
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)

import gdx_rendezvous as R  # noqa: E402

BUILD = "mp-online@test"


def req(t, nonce=1, build=BUILD, name="MIKE", code="", maxp=4, content=0xC0DE, priv=(192, 168, 2, 9),
        priv_port=27100, cflags=0):
    return (R.GXRV_MAGIC + bytes([R.GXRV_VERSION, t, 0, 0]) +
            struct.pack("<II4sHBB", nonce, content, bytes(priv), priv_port, maxp, cflags) +
            R.pad_bytes(build, 40) + R.pad_bytes(name, 12) + R.pad_bytes(code, 8))


def auth(t, code, slot, key, arg=0, extra=b""):
    return (R.GXRV_MAGIC + bytes([R.GXRV_VERSION, t, 0, 0]) + R.pad_bytes(code, 8) +
            struct.pack("<BBHQ", slot, arg, 0, key) + extra)


def keepalive(code, slot, key, ver=0, ms=0):
    return auth(R.T_KEEPALIVE, code, slot, key, extra=struct.pack("<II", ver, ms))


def parse_welcome(pkt):
    assert pkt[5] == R.T_WELCOME and len(pkt) == 48
    nonce = struct.unpack_from("<I", pkt, 8)[0]
    code = pkt[12:20].rstrip(b"\0").decode()
    slot, maxp, flags, key, secret = struct.unpack_from("<BBBxQQ", pkt, 20)
    return {"nonce": nonce, "code": code, "slot": slot, "max": maxp, "flags": flags, "key": key, "secret": secret}


def parse_error(pkt):
    assert pkt[5] == R.T_ERROR and len(pkt) in (16, 56)
    return pkt[12], pkt[16:56].rstrip(b"\0").decode() if len(pkt) == 56 else ""


def gxlb(from_slot, size=108):
    return b"GXLB" + bytes([1, 1, from_slot, 0xFF]) + bytes(size - 8)


def gxmp(slot1):
    p = bytearray(b"GXMP" + bytes([1, 1, 0, 1]) + bytes(68 - 8 + 4))
    p[34] = slot1
    return bytes(p)


def legacy_server(**kw):
    """Most room-logic tests speak v1 (the shortest requests): a server run with --allow-v1. The
    default (cookie required) has its own tests in VersionGateTests."""
    kw.setdefault("require_cookie", False)
    return R.Rendezvous(**kw)


class Clock:
    def __init__(self):
        self.t = 1000.0


class CoreTests(unittest.TestCase):
    def setUp(self):
        self.srv = legacy_server(rng=random.Random(7))
        self.clock = Clock()

    def send(self, pkt, addr):
        return self.srv.handle(pkt, addr, self.clock.t)

    def room(self, n=4, maxp=4):
        """Create a room with n members; returns (code, [(addr, welcome)])."""
        host = ("198.51.100.1", 40000)
        out = self.send(req(R.T_CREATE, nonce=11, maxp=maxp), host)
        w = parse_welcome(out[0][0])
        members = [(host, w)]
        for i in range(1, n):
            addr = ("198.51.100.%d" % (i + 1), 40000 + i)
            out = self.send(req(R.T_JOIN, nonce=100 + i, code=w["code"], name="P%d" % i), addr)
            members.append((addr, parse_welcome(out[0][0])))
        return w["code"], members

    # ---- lifecycle ---------------------------------------------------------------------------
    def test_create_join_roster(self):
        code, ms = self.room(4)
        self.assertEqual(len(code), 5)
        self.assertTrue(all(c in R.CODE_ALPHABET for c in code))
        self.assertEqual([w["slot"] for _, w in ms], [0, 1, 2, 3])
        self.assertEqual(len({w["secret"] for _, w in ms}), 1)
        self.assertEqual(len({w["key"] for _, w in ms}), 4)
        addr, w = ms[2]
        out = self.send(keepalive(code, 2, w["key"]), addr)
        types = [p[5] for p, _, _ in out]
        self.assertIn(R.T_KA_ACK, types)
        roster = [p for p, _, _ in out if p[5] == R.T_ROSTER][0]
        self.assertEqual(roster[20], 4)
        self.assertEqual(len(roster), 24 + 4 * 28)
        # entry for slot 3: public = what we saw, private = what it reported
        e = roster[24 + 3 * 28: 24 + 4 * 28]
        self.assertEqual(e[0], 3)
        self.assertEqual(socket.inet_ntoa(e[4:8]), "198.51.100.4")
        self.assertEqual(struct.unpack_from("<H", e, 8)[0], 40003)
        self.assertEqual(e[12:16], bytes([192, 168, 2, 9]))

    def test_join_broadcasts_roster_to_existing_only(self):
        host = ("198.51.100.1", 40000)
        w = parse_welcome(self.send(req(R.T_CREATE, nonce=1), host)[0][0])
        out = self.send(req(R.T_JOIN, nonce=2, code=w["code"]), ("198.51.100.2", 1))
        dests = [(p[5], a) for p, a, _ in out]
        self.assertIn((R.T_WELCOME, ("198.51.100.2", 1)), dests)
        self.assertIn((R.T_ROSTER, host), dests)
        self.assertNotIn((R.T_ROSTER, ("198.51.100.2", 1)), dests)

    def test_retransmitted_requests_are_idempotent(self):
        host = ("198.51.100.1", 40000)
        a = parse_welcome(self.send(req(R.T_CREATE, nonce=5), host)[0][0])
        b = parse_welcome(self.send(req(R.T_CREATE, nonce=5), host)[0][0])
        self.assertEqual(a, b)
        self.assertEqual(len(self.srv.rooms), 1)
        j1 = parse_welcome(self.send(req(R.T_JOIN, nonce=9, code=a["code"]), ("10.9.9.9", 5))[0][0])
        j2 = parse_welcome(self.send(req(R.T_JOIN, nonce=9, code=a["code"]), ("10.9.9.9", 5))[0][0])
        self.assertEqual(j1, j2)
        self.assertEqual(len(self.srv.rooms[a["code"]].members), 2)

    def test_mismatch_is_readable(self):
        code, _ = self.room(1)
        err, detail = parse_error(self.send(req(R.T_JOIN, nonce=3, code=code, build="integ-v1.0.1@x"),
                                            ("203.0.113.5", 1))[0][0])
        self.assertEqual(err, R.E_BUILD_MISMATCH)
        self.assertEqual(detail, BUILD)
        err, detail = parse_error(self.send(req(R.T_JOIN, nonce=4, code=code, content=1), ("203.0.113.5", 1))[0][0])
        self.assertEqual(err, R.E_CONTENT_MISMATCH)
        self.assertEqual(detail, "%08x" % 0xC0DE)

    def test_full_locked_unknown(self):
        code, ms = self.room(2, maxp=2)
        err, _ = parse_error(self.send(req(R.T_JOIN, nonce=50, code=code), ("203.0.113.9", 1))[0][0])
        self.assertEqual(err, R.E_ROOM_FULL)
        code, ms = self.room(1)
        (haddr, hw) = ms[0]
        out = self.send(auth(R.T_LOCK, code, 0, hw["key"], arg=1), haddr)
        self.assertTrue(self.srv.rooms[code].locked)
        self.assertEqual(out[0][0][21] & 1, 1)
        err, _ = parse_error(self.send(req(R.T_JOIN, nonce=51, code=code), ("203.0.113.9", 2))[0][0])
        self.assertEqual(err, R.E_ROOM_LOCKED)
        err, _ = parse_error(self.send(req(R.T_JOIN, nonce=52, code="ZZZZZ"), ("203.0.113.9", 3))[0][0])
        self.assertEqual(err, R.E_ROOM_NOT_FOUND)

    def test_restarted_console_takes_its_slot_back(self):
        code, ms = self.room(2, maxp=2)
        addr, w = ms[1]
        # A live member cannot be evicted by a JOIN from its address (spoof or not).
        self.clock.t += 1
        err, _ = parse_error(self.send(req(R.T_JOIN, nonce=77, code=code), addr)[0][0])
        self.assertEqual(err, R.E_ROOM_FULL)
        # Silent past REJOIN_SILENT_S: the same address with a new nonce gets slot 1 again.
        epoch0 = self.srv.rooms[code].members[1].epoch
        self.clock.t += R.REJOIN_SILENT_S + 1
        w2 = parse_welcome(self.send(req(R.T_JOIN, nonce=78, code=code), addr)[0][0])
        self.assertEqual(w2["slot"], 1)
        self.assertNotEqual(self.srv.rooms[code].members[1].epoch, epoch0)
        roster = self.srv._roster_pkt(self.srv.rooms[code])
        self.assertEqual(struct.unpack_from("<H", roster, 24 + 28 + 2)[0], self.srv.rooms[code].members[1].epoch)
        self.assertNotEqual(w2["key"], w["key"])
        self.assertEqual(len(self.srv.rooms[code].members), 2)
        # The old key is dead (a short error, no KA_ACK).
        out = self.send(keepalive(code, 1, w["key"]), addr)
        self.assertTrue(all(p[5] == R.T_ERROR and len(p) == 16 for p, _, _ in out))
        # Another address can never take it.
        self.clock.t += R.REJOIN_SILENT_S + 1
        err, _ = parse_error(self.send(req(R.T_JOIN, nonce=79, code=code), ("203.0.113.70", 1))[0][0])
        self.assertEqual(err, R.E_ROOM_FULL)

    def test_restarted_host_takes_its_code_back(self):
        self.srv.allow_custom_codes = True
        host = ("198.51.100.1", 40000)
        w = parse_welcome(self.send(req(R.T_CREATE, nonce=1, code="K7MPQ"), host)[0][0])
        self.assertEqual(w["code"], "K7MPQ")
        self.send(req(R.T_JOIN, nonce=2, code="K7MPQ"), ("198.51.100.2", 1))
        self.clock.t += 1
        err, _ = parse_error(self.send(req(R.T_CREATE, nonce=3, code="K7MPQ"), host)[0][0])
        self.assertEqual(err, R.E_CODE_TAKEN)  # live host: refused
        self.clock.t += R.REJOIN_SILENT_S + 1
        err, _ = parse_error(self.send(req(R.T_CREATE, nonce=4, code="K7MPQ"), ("203.0.113.1", 1))[0][0])
        self.assertEqual(err, R.E_CODE_TAKEN)  # another address: refused
        out = self.send(req(R.T_CREATE, nonce=5, code="K7MPQ"), host)
        w2 = parse_welcome(out[0][0])
        self.assertEqual((w2["slot"], w2["secret"]), (0, w["secret"]))
        self.assertNotEqual(w2["key"], w["key"])
        self.assertIn((R.T_ROSTER, ("198.51.100.2", 1)), [(p[5], a) for p, a, _ in out])
        self.assertEqual(len(self.srv.rooms["K7MPQ"].members), 2)

    def test_only_host_locks(self):
        code, ms = self.room(2)
        addr, w = ms[1]
        self.assertEqual(self.send(auth(R.T_LOCK, code, 1, w["key"], arg=1), addr), [])
        self.assertFalse(self.srv.rooms[code].locked)

    def test_leave_and_host_leave(self):
        code, ms = self.room(3)
        addr, w = ms[2]
        out = self.send(auth(R.T_LEAVE, code, 2, w["key"]), addr)
        self.assertEqual({a for _, a, _ in out}, {ms[0][0], ms[1][0]})
        self.assertNotIn(2, self.srv.rooms[code].members)
        haddr, hw = ms[0]
        out = self.send(auth(R.T_LEAVE, code, 0, hw["key"]), haddr)
        self.assertNotIn(code, self.srv.rooms)
        self.assertEqual(parse_error(out[0][0])[0], R.E_HOST_LEFT)

    def test_host_leave_orphans_a_room_of_three(self):
        """The host leaves a race of three: the two guests are told (E_HOST_LEFT) but the room
        stays up to relay between them until they leave; nobody new gets in."""
        code, ms = self.room(3)
        haddr, hw = ms[0]
        out = self.send(auth(R.T_LEAVE, code, 0, hw["key"]), haddr)
        self.assertEqual({a for _, a, _ in out}, {ms[1][0], ms[2][0]})
        self.assertTrue(all(parse_error(p)[0] == R.E_HOST_LEFT for p, _, _ in out))
        self.assertIn(code, self.srv.rooms)
        self.assertNotIn(0, self.srv.rooms[code].members)
        a1, w1 = ms[1]
        out = self.send(auth(R.T_RELAY, code, 1, w1["key"], arg=0b0100, extra=gxlb(1)), a1)
        self.assertEqual([a for _, a, _ in out], [ms[2][0]])
        out = self.send(req(R.T_JOIN, nonce=900, code=code, name="LATE"), ("198.51.100.9", 41000))
        self.assertEqual(parse_error(out[0][0])[0], R.E_ROOM_LOCKED)
        self.send(auth(R.T_LEAVE, code, 1, w1["key"]), a1)
        self.assertIn(code, self.srv.rooms)
        a2, w2 = ms[2]
        self.send(auth(R.T_LEAVE, code, 2, w2["key"]), a2)
        self.assertNotIn(code, self.srv.rooms)

    def test_orphaned_room_expires_with_its_last_guest(self):
        self.srv.member_timeout = 30
        code, ms = self.room(3)
        self.clock.t += 31
        a1, w1 = ms[1]
        self.srv.expire(self.clock.t)
        self.assertNotIn(code, self.srv.rooms)  # everyone silent: host first (orphan), then the guests

    def test_expiry(self):
        self.srv.member_timeout = 30
        code, ms = self.room(3)
        self.clock.t += 20
        addr, w = ms[0]
        self.send(keepalive(code, 0, w["key"]), addr)
        a2, w2 = ms[2]
        self.send(keepalive(code, 2, w2["key"]), a2)
        self.clock.t += 15
        self.srv.expire(self.clock.t)
        self.assertIn(code, self.srv.rooms)
        self.assertNotIn(1, self.srv.rooms[code].members)  # slot 1 never kept alive
        self.clock.t += 31
        self.srv.expire(self.clock.t)
        self.assertNotIn(code, self.srv.rooms)  # host timed out -> room closed

    def test_room_max_age(self):
        self.srv.room_max_age = 100
        code, ms = self.room(2)
        for _ in range(12):
            self.clock.t += 10
            for addr, w in ms:
                self.send(keepalive(code, w["slot"], w["key"]), addr)
        self.srv.expire(self.clock.t)
        self.assertNotIn(code, self.srv.rooms)

    def test_nat_rebinding_follows_authenticated_member(self):
        code, ms = self.room(2)
        addr, w = ms[1]
        new = ("198.51.100.2", 55555)
        out = self.send(keepalive(code, 1, w["key"]), new)
        self.assertEqual(self.srv.rooms[code].members[1].addr, new)
        self.assertTrue(any(p[5] == R.T_ROSTER for p, _, _ in out))
        # an unauthenticated move is not followed
        self.send(keepalive(code, 1, w["key"] ^ 1), ("6.6.6.6", 1))
        self.assertEqual(self.srv.rooms[code].members[1].addr, new)

    # ---- relay -------------------------------------------------------------------------------
    def test_relay_verbatim_and_masked(self):
        code, ms = self.room(4)
        addr, w = ms[1]
        payload = gxlb(1)
        out = self.send(auth(R.T_RELAY, code, 1, w["key"], arg=0b1101, extra=payload), addr)
        self.assertEqual(sorted(a for _, a, _ in out), sorted([ms[0][0], ms[2][0], ms[3][0]]))
        self.assertTrue(all(p == payload for p, _, _ in out))
        out = self.send(auth(R.T_RELAY, code, 1, w["key"], arg=0b0001, extra=gxmp(2)), addr)
        self.assertEqual([a for _, a, _ in out], [ms[0][0]])

    def test_relay_refuses_spoofed_sender(self):
        code, ms = self.room(3)
        addr, w = ms[1]
        self.assertEqual(self.send(auth(R.T_RELAY, code, 1, w["key"], arg=0b101, extra=gxlb(2)), addr), [])
        self.assertEqual(self.send(auth(R.T_RELAY, code, 1, w["key"], arg=0b101, extra=gxmp(3)), addr), [])
        self.assertEqual(self.send(auth(R.T_RELAY, code, 1, w["key"], arg=0b101, extra=b"JUNK" + bytes(60)), addr), [])
        self.assertEqual(self.send(auth(R.T_RELAY, code, 1, w["key"], arg=0b101, extra=gxlb(1, size=20)), addr), [])
        self.assertEqual(self.send(auth(R.T_RELAY, code, 1, w["key"], arg=0b101, extra=gxlb(1, size=1201)), addr), [])
        self.assertEqual(self.send(auth(R.T_RELAY, code, 1, w["key"] + 1, arg=0b101, extra=gxlb(1)), addr), [])
        self.assertEqual(self.send(auth(R.T_RELAY, code, 1, w["key"], arg=0b010, extra=gxlb(1)), addr), [])  # self only

    def test_relay_flood_is_capped(self):
        self.srv = legacy_server(relay_pps=100, rng=random.Random(1))
        code, ms = self.room(2)
        addr, w = ms[1]
        pkt = auth(R.T_RELAY, code, 1, w["key"], arg=1, extra=gxlb(1))
        delivered = sum(len(self.send(pkt, addr)) for _ in range(5000))  # all in one instant
        self.assertLessEqual(delivered, 151)
        self.clock.t += 1.0
        delivered = sum(len(self.send(pkt, addr)) for _ in range(5000))
        self.assertLessEqual(delivered, 151)
        self.assertGreater(self.srv.stats["rate_limited"], 9000)

    def test_four_player_relay_room_fits_the_default_budget(self):
        # mp-online-hw: 4 relay-only members at RTT ~200 ms (a 14-pad window: 68 + 4 * 14 = 124 B
        # GXMP), each sending one masked relay datagram per tick at 60 Hz, a second one on every
        # third tick (the 16 ms resend of a tick that waits under jitter) and a 1 Hz HELLO, for 30 s:
        # ~116 KB/s. The 96 KiB/s room budget before (98304) ran dry after its burst; 192 KiB/s
        # carries all of it.
        code, ms = self.room(4)
        pad = bytearray(gxmp(1)) + bytes(124 - 72)
        delivered = 0
        sent = 0
        for tick in range(1800):
            self.clock.t += 1.0 / 60
            for slot, (addr, w) in enumerate(ms):
                p = bytearray(pad)
                p[34] = slot + 1
                mask = 0b1111 & ~(1 << slot)
                for _ in range(2 if tick % 3 == 0 else 1):
                    delivered += len(self.send(auth(R.T_RELAY, code, slot, w["key"], arg=mask, extra=bytes(p)),
                                               addr))
                    sent += 3
                if tick % 60 == slot:
                    delivered += len(self.send(auth(R.T_RELAY, code, slot, w["key"], arg=mask,
                                                    extra=gxlb(slot, size=128)), addr))
                    sent += 3
        self.assertEqual(self.srv.stats["rate_limited"], 0)
        self.assertEqual(delivered, sent)

    def test_create_relay_only_flag(self):
        # mp-online-hw: the CREATE flag a console sets for one room (L held on HOST ONLINE ROOM)
        # makes that room relay-only for every member, with no server switch.
        host = ("198.51.100.1", 40000)
        w = parse_welcome(self.send(req(R.T_CREATE, nonce=5, cflags=1), host)[0][0])
        self.assertEqual(w["flags"] & 1, 1)
        j = parse_welcome(self.send(req(R.T_JOIN, nonce=6, code=w["code"]), ("198.51.100.2", 40001))[0][0])
        self.assertEqual(j["flags"] & 1, 1)
        roster = [p for p, _, _ in self.send(keepalive(w["code"], 0, w["key"]), host) if p[5] == R.T_ROSTER][0]
        self.assertEqual(roster[21] & 2, 2)
        w2 = parse_welcome(self.send(req(R.T_CREATE, nonce=7), ("198.51.100.3", 40002))[0][0])
        self.assertEqual(w2["flags"] & 1, 0)  # the next room is not

    def test_relay_proxy_delay_and_loss(self):
        self.srv = legacy_server(relay_delay_ms=50, relay_jitter_ms=10, relay_loss_pct=50, rng=random.Random(3))
        code, ms = self.room(2)
        addr, w = ms[1]
        outs = []
        for i in range(200):
            self.clock.t += 0.02
            outs += self.send(auth(R.T_RELAY, code, 1, w["key"], arg=1, extra=gxlb(1)), addr)
        self.assertTrue(60 < len(outs) < 140)
        self.assertTrue(all(0.05 <= d <= 0.06 for _, _, d in outs))

    def test_punch_request(self):
        code, ms = self.room(3)
        addr, w = ms[2]
        out = self.send(auth(R.T_PUNCH_REQ, code, 2, w["key"], arg=0), addr)
        self.assertEqual(len(out), 1)
        self.assertEqual(out[0][1], ms[0][0])
        self.assertEqual(out[0][0][5], R.T_PUNCH_NOW)
        self.assertEqual(out[0][0][16], 2)
        self.assertEqual(self.send(auth(R.T_PUNCH_REQ, code, 2, w["key"], arg=2), addr), [])
        self.assertEqual(self.send(auth(R.T_PUNCH_REQ, code, 2, w["key"], arg=3), addr), [])

    def test_sim_symmetric_and_force_relay(self):
        self.srv = legacy_server(sim_symmetric=[1], force_relay=True, rng=random.Random(5))
        code, ms = self.room(2)
        addr, w = ms[0]
        self.assertEqual(w["flags"] & 1, 1)
        roster = [p for p, _, _ in self.send(keepalive(code, 0, w["key"]), addr) if p[5] == R.T_ROSTER][0]
        self.assertEqual(roster[21] & 2, 2)
        e1 = roster[24 + 28: 24 + 56]
        self.assertNotEqual(struct.unpack_from("<H", e1, 8)[0], ms[1][0][1])
        self.assertEqual(e1[12:16], b"\0\0\0\0")

    def test_custom_codes_only_when_allowed(self):
        w = parse_welcome(self.send(req(R.T_CREATE, nonce=1, code="TESTA"), ("1.1.1.1", 1))[0][0])
        self.assertNotEqual(w["code"], "TESTA")
        self.srv.allow_custom_codes = True
        w = parse_welcome(self.send(req(R.T_CREATE, nonce=2, code="TESTB"), ("1.1.1.2", 1))[0][0])
        self.assertEqual(w["code"], "TESTB")
        err, _ = parse_error(self.send(req(R.T_CREATE, nonce=3, code="TESTB"), ("1.1.1.3", 1))[0][0])
        self.assertEqual(err, R.E_CODE_TAKEN)

    # ---- hostile input -----------------------------------------------------------------------
    def test_malformed_requests_never_raise_or_reply_big(self):
        code, ms = self.room(2)
        addr, w = ms[1]
        good = [req(R.T_CREATE, nonce=77), req(R.T_JOIN, nonce=78, code=code), keepalive(code, 1, w["key"]),
                auth(R.T_RELAY, code, 1, w["key"], arg=1, extra=gxlb(1)), auth(R.T_LEAVE, code, 1, w["key"])]
        rng = random.Random(11)
        src = 0
        for pkt in good:
            variants = [pkt[:n] for n in range(len(pkt))] + [pkt + b"\0", pkt + bytes(2000)]
            for _ in range(300):
                b = bytearray(pkt)
                for _ in range(rng.randint(1, 4)):
                    b[rng.randrange(len(b))] = rng.randrange(256)
                variants.append(bytes(b))
            for v in variants:
                src += 1
                a = ("203.0.%d.%d" % ((src >> 8) & 255, src & 255), 1000 + (src % 5000))
                out = self.srv.handle(v, a, self.clock.t)  # must not raise
                authed = len(v) >= 28 and v[8:17] == pkt[8:17] and v[20:28] == pkt[20:28] and pkt[5] not in (R.T_CREATE, R.T_JOIN)
                for p, dst, _ in out:
                    if dst == a and not authed and not p.startswith(b"GXRV\x01\x81"):
                        # nothing larger than what it cost goes back to an unauthenticated sender
                        self.assertLessEqual(len(p), len(v), msg=repr(v[:12]))
        for _ in range(2000):
            src += 1
            v = bytes(rng.randrange(256) for _ in range(rng.randint(0, 1400)))
            self.srv.handle(v, ("192.0.2.%d" % (src & 255), src), self.clock.t)

    def test_amplification(self):
        """Every reply to an unauthenticated sender is no larger than its request."""
        host = ("198.51.100.1", 1)
        pkt = req(R.T_CREATE, nonce=1)
        out = self.send(pkt, host)
        self.assertTrue(all(len(p) <= len(pkt) for p, a, _ in out if a == host))
        code = parse_welcome(out[0][0])["code"]
        pkt = req(R.T_JOIN, nonce=2, code=code)
        victim = ("192.0.2.66", 7)
        out = self.send(pkt, victim)
        self.assertTrue(all(len(p) <= len(pkt) for p, a, _ in out if a == victim))
        pkt = keepalive(code, 1, 12345)
        out = self.send(pkt, ("192.0.2.67", 7))
        self.assertTrue(all(len(p) <= len(pkt) for p, _, _ in out))

    def test_junk_source_is_blocked(self):
        a = ("192.0.2.1", 9)
        for _ in range(60):
            self.send(b"garbage!" * 4, a)
        self.assertGreater(self.srv.ips["192.0.2.1"].blocked_until, self.clock.t)
        # even a valid create is ignored while blocked
        self.assertEqual(self.send(req(R.T_CREATE, nonce=1), a), [])
        self.clock.t += 61
        self.assertNotEqual(self.send(req(R.T_CREATE, nonce=1), a), [])

    def test_create_flood_and_rooms_per_ip(self):
        a = ("192.0.2.2", 9)
        made = 0
        for i in range(1000):
            out = self.send(req(R.T_CREATE, nonce=1000 + i), (a[0], 9000 + i))
            made += sum(1 for p, _, _ in out if p[5] == R.T_WELCOME)
        self.assertLessEqual(made, 8)  # max_rooms_per_ip
        for i in range(30):
            self.clock.t += 1
            self.send(req(R.T_CREATE, nonce=5000 + i), (a[0], 100 + i))
        self.assertLessEqual(self.srv.ips[a[0]].rooms, 8)

    def test_code_guessing_is_throttled(self):
        a = ("192.0.2.3", 9)
        code, _ = self.room(1)
        errs = []
        for i in range(30):
            self.clock.t += 0.3
            out = self.send(req(R.T_JOIN, nonce=i, code="ZZZZ%s" % R.CODE_ALPHABET[i % 32]), a)
            if out:
                errs.append(parse_error(out[0][0])[0])
        self.assertIn(R.E_RATE_LIMITED, errs)
        # even the right code is refused while throttled
        out = self.send(req(R.T_JOIN, nonce=99, code=code), a)
        self.assertEqual(parse_error(out[0][0])[0], R.E_RATE_LIMITED)

    def test_bad_fields(self):
        a = ("192.0.2.4", 1)
        for bad in (req(R.T_CREATE, maxp=1), req(R.T_CREATE, maxp=5)):
            self.clock.t += 1
            err, _ = parse_error(self.send(bad, a)[0][0])
            self.assertEqual(err, R.E_BAD_REQUEST)
        for bad in (req(R.T_CREATE, build=""), req(R.T_CREATE, build="has space"),
                    req(R.T_CREATE, name="<script>"), req(R.T_JOIN, code="ABC"), req(R.T_JOIN, code="ABCDE0")):
            self.clock.t += 1
            self.assertEqual(self.send(bad, a), [])
        # a public priv address is not passed on to peers
        self.clock.t += 1
        w = parse_welcome(self.send(req(R.T_CREATE, nonce=9, priv=(8, 8, 8, 8)), ("192.0.2.5", 1))[0][0])
        self.assertEqual(self.srv.rooms[w["code"]].members[0].priv_ip, b"\0\0\0\0")

    def test_tables_bounded(self):
        self.srv = legacy_server(max_rooms=50, max_ips=500, rng=random.Random(2))
        for i in range(2000):
            self.srv.handle(req(R.T_CREATE, nonce=i), ("10.%d.%d.%d" % (i >> 16, (i >> 8) & 255, i & 255), 1), self.clock.t)
        self.assertLessEqual(len(self.srv.rooms), 50)
        self.assertLessEqual(len(self.srv.ips), 600)


def v2(pkt):
    """The same message with a version 2 header."""
    return pkt[:4] + bytes([R.GXRV_VERSION_COOKIE]) + pkt[5:]


def req2(t, cookie=bytes(16), client_proto=None, **kw):
    """A v2 CREATE / JOIN: the v1 body (header version 2), the cookie, the client proto, 0."""
    cp = R.CLIENT_PROTO if client_proto is None else client_proto
    return v2(req(t, **kw)) + cookie + struct.pack("<HH", cp, 0)


def parse_cookie(pkt):
    assert pkt[4] == R.GXRV_VERSION_COOKIE and pkt[5] == R.T_COOKIE and len(pkt) == R.COOKIE_REPLY_LEN
    return struct.unpack_from("<I", pkt, 8)[0], pkt[12:28]


PROM_LINE = re.compile(r'^[a-zA-Z_:][a-zA-Z0-9_:]*(\{([a-zA-Z_][a-zA-Z0-9_]*="([^"\\]|\\.)*",?)*\})? '
                       r'[-+0-9.eEInfa]+$')


class OpsTests(unittest.TestCase):
    """mp-server-ops: return routability (unconfirmed rooms, the v2 cookie), metrics, the event log."""

    def setUp(self):
        self.srv = legacy_server(rng=random.Random(9))
        self.t = 5000.0

    def send(self, pkt, addr):
        return self.srv.handle(pkt, addr, self.t)

    def v2_welcome(self, t, addr, nonce, **kw):
        out = self.send(req2(t, nonce=nonce, **kw), addr)
        self.assertEqual(len(out), 1)
        n, cookie = parse_cookie(out[0][0])
        self.assertEqual(n, nonce)
        out = self.send(req2(t, cookie=cookie, nonce=nonce, **kw), addr)
        return out

    # ---- the v2 cookie ---------------------------------------------------------------------------
    def test_v2_cookie_round_trip(self):
        host = ("198.51.100.1", 40000)
        out = self.v2_welcome(R.T_CREATE, host, 7)
        self.assertEqual(out[0][0][4], R.GXRV_VERSION_COOKIE)
        w = parse_welcome(out[0][0])
        room = self.srv.rooms[w["code"]]
        self.assertTrue(room.members[0].confirmed)
        self.assertNotIn(w["code"], self.srv.pending)  # proved its address: never unconfirmed
        # a v1 card joins the same room: each gets replies in its own header version
        g = ("198.51.100.2", 40001)
        out = self.send(req(R.T_JOIN, nonce=8, code=w["code"]), g)
        self.assertEqual({(p[4], a) for p, a, _ in out}, {(1, g), (2, host)})
        gw = parse_welcome(out[0][0])
        out = self.send(keepalive(w["code"], 1, gw["key"]), g)
        self.assertTrue(all(p[4] == 1 for p, _, _ in out))
        out = self.send(v2(keepalive(w["code"], 0, w["key"])), host)
        self.assertTrue(all(p[4] == 2 for p, _, _ in out))
        self.assertIn(R.T_KA_ACK, [p[5] for p, _, _ in out])
        # a v2 RELAY header is accepted and the payload goes out verbatim
        out = self.send(v2(auth(R.T_RELAY, w["code"], 0, w["key"], arg=0b10, extra=gxlb(0))), host)
        self.assertEqual(out, [(gxlb(0), g, 0.0)])

    def test_cookie_is_bound_to_address_nonce_and_time(self):
        a = ("198.51.100.5", 1000)
        _n, cookie = parse_cookie(self.send(req2(R.T_CREATE, nonce=1), a)[0][0])
        # another port, another nonce, another type, a later time: a fresh cookie, no room
        for pkt, src in ((req2(R.T_CREATE, cookie=cookie, nonce=1), ("198.51.100.5", 1001)),
                         (req2(R.T_CREATE, cookie=cookie, nonce=2), a),
                         (req2(R.T_JOIN, cookie=cookie, nonce=1, code="ABCDE"), a)):
            out = self.send(pkt, src)
            self.assertEqual(out[0][0][5], R.T_COOKIE)
        self.t += 31
        self.assertEqual(self.send(req2(R.T_CREATE, cookie=cookie, nonce=1), a)[0][0][5], R.T_COOKIE)
        self.assertEqual(self.srv.rooms, {})
        self.assertGreaterEqual(self.srv.metrics.total("gdx_rv_cookies_rejected_total"), 4)
        forged = bytes(cookie[:4]) + bytes(12)
        self.t -= 31
        self.assertEqual(self.send(req2(R.T_CREATE, cookie=forged, nonce=1), a)[0][0][5], R.T_COOKIE)

    def test_v2_never_amplifies(self):
        rng = random.Random(4)
        for i in range(500):
            a = ("203.0.113.%d" % (i & 255), 1000 + i)
            pkt = req2(R.T_CREATE if i % 2 else R.T_JOIN, nonce=i, code="" if i % 2 else "ABCDE",
                       cookie=bytes(rng.randrange(256) for _ in range(16)) if i % 3 else bytes(16))
            for p, dst, _ in self.send(pkt, a):
                self.assertLessEqual(len(p), len(pkt))
        # a wrong-sized v2 request gets nothing
        self.assertEqual(self.send(v2(req(R.T_CREATE, nonce=1)), ("203.0.113.1", 1)), [])

    def test_require_cookie_refuses_v1_readably(self):
        self.srv.require_cookie = True
        pkt = req(R.T_CREATE, nonce=3)
        out = self.send(pkt, ("198.51.100.9", 1))
        err, detail = parse_error(out[0][0])
        self.assertEqual(err, R.E_BUILD_MISMATCH)  # a v1 card: "version mismatch: host runs <detail>"
        self.assertEqual(detail, R.NEEDS_V2_DETAIL)
        self.assertLessEqual(len(out[0][0]), len(pkt))
        self.assertEqual(self.srv.rooms, {})
        out = self.v2_welcome(R.T_CREATE, ("198.51.100.9", 2), 4)
        self.assertEqual(out[0][0][5], R.T_WELCOME)

    def test_v2_join_survives_a_spoof_poisoned_address(self):
        """Spoofed v1 junk and failed JOINs from a victim's address block it / trip its code-guess
        throttle; the victim's v2 JOIN (cookie-proven) still gets in."""
        host = ("198.51.100.1", 40000)
        w = parse_welcome(self.send(req(R.T_CREATE, nonce=1), host)[0][0])
        victim = ("192.0.2.77", 5000)
        for i in range(60):
            self.send(b"garbage!" * 4, victim)
        for i in range(12):
            self.t += 0.3
            self.send(req(R.T_JOIN, nonce=50 + i, code="ZZZZZ"), victim)
        self.assertGreater(self.srv.ips[victim[0]].blocked_until, self.t)
        self.assertEqual(self.send(req(R.T_JOIN, nonce=99, code=w["code"]), victim), [])  # v1: blocked
        out = self.v2_welcome(R.T_JOIN, victim, 100, code=w["code"])
        self.assertEqual(parse_welcome(out[0][0])["slot"], 1)

    # ---- unconfirmed rooms (v1 return routability) -------------------------------------------------
    def test_unconfirmed_room_expires_fast_confirmed_one_stays(self):
        a = ("198.51.100.20", 1)
        b = ("198.51.100.21", 1)
        wa = parse_welcome(self.send(req(R.T_CREATE, nonce=1), a)[0][0])
        wb = parse_welcome(self.send(req(R.T_CREATE, nonce=2), b)[0][0])
        self.assertEqual(set(self.srv.pending), {wa["code"], wb["code"]})
        self.t += 0.2
        self.send(keepalive(wb["code"], 0, wb["key"]), b)  # what a real console does at once
        self.assertEqual(set(self.srv.pending), {wa["code"]})
        self.t += R.Rendezvous().pending_timeout + 1
        self.srv.expire(self.t)
        self.assertNotIn(wa["code"], self.srv.rooms)
        self.assertIn(wb["code"], self.srv.rooms)
        self.assertEqual(self.srv.metrics.get("gdx_rv_members_left_total", why="unconfirmed"), 1)
        self.assertEqual(self.srv.ips[a[0]].rooms, 0)  # its per-address quota is back

    def test_unconfirmed_joiner_is_dropped(self):
        host = ("198.51.100.1", 40000)
        w = parse_welcome(self.send(req(R.T_CREATE, nonce=1), host)[0][0])
        self.send(keepalive(w["code"], 0, w["key"]), host)
        self.send(req(R.T_JOIN, nonce=2, code=w["code"]), ("192.0.2.200", 9))  # spoofed: never answers
        self.assertEqual(len(self.srv.rooms[w["code"]].members), 2)
        for _ in range(12):
            self.t += 1
            self.send(keepalive(w["code"], 0, w["key"]), host)
            self.srv.expire(self.t)
        self.assertEqual(len(self.srv.rooms[w["code"]].members), 1)

    def test_spoofed_create_flood_cannot_fill_the_room_table(self):
        self.srv = legacy_server(max_rooms=50, rng=random.Random(2))
        for i in range(3000):
            self.srv.handle(req(R.T_CREATE, nonce=i), ("10.%d.%d.%d" % (i >> 16, (i >> 8) & 255, i & 255), 1), self.t)
        self.assertLessEqual(len(self.srv.pending), self.srv.max_pending)
        self.assertEqual(self.srv._confirmed_rooms(), 0)
        self.assertGreater(self.srv.metrics.total("gdx_rv_unconfirmed_evicted_total"), 2000)
        a = ("198.51.100.50", 7)
        w = parse_welcome(self.send(req(R.T_CREATE, nonce=1), a)[0][0])
        self.send(keepalive(w["code"], 0, w["key"]), a)
        self.assertIn(w["code"], self.srv.rooms)
        self.assertEqual(self.srv._confirmed_rooms(), 1)

    def test_blocked_address_keeps_its_authenticated_member(self):
        code_w = parse_welcome(self.send(req(R.T_CREATE, nonce=1), ("198.51.100.1", 40000))[0][0])
        host = ("198.51.100.1", 40000)
        for _ in range(60):
            self.send(b"garbage!" * 4, ("198.51.100.1", 1))  # spoofed junk "from" the host's address
        self.assertGreater(self.srv.ips[host[0]].blocked_until, self.t)
        out = self.send(keepalive(code_w["code"], 0, code_w["key"]), host)
        self.assertIn(R.T_KA_ACK, [p[5] for p, _, _ in out])
        self.assertEqual(self.send(keepalive(code_w["code"], 0, code_w["key"] ^ 1), host), [])

    def test_per_address_packet_cap(self):
        self.srv = legacy_server(ip_pps=100, rng=random.Random(2))
        a = ("192.0.2.9", 1)
        w = parse_welcome(self.send(req(R.T_CREATE, nonce=1), a)[0][0])
        for _ in range(1000):
            self.send(keepalive(w["code"], 0, w["key"]), a)  # authenticated, so never "junk"
        self.assertGreater(self.srv.metrics.get("gdx_rv_ratelimit_total", limit="ip_pps"), 700)

    def test_probe_ping_is_answered_and_not_junk(self):
        a = ("192.0.2.40", 1)
        ping = auth(R.T_KEEPALIVE, "", 0, 0, extra=struct.pack("<II", 0, 0))
        for _ in range(100):
            self.t += 0.5
            out = self.send(ping, a)
            self.assertEqual(len(out), 1)
            self.assertEqual(parse_error(out[0][0]), (R.E_AUTH, ""))
            self.assertLessEqual(len(out[0][0]), len(ping))
        self.assertEqual(self.srv.stats["junk"], 0)
        self.assertEqual(self.srv.ips[a[0]].blocked_until, 0.0)
        self.assertEqual(self.srv.metrics.get("gdx_rv_probe_pings_total"), 100)

    def test_drain_refuses_new_rooms_only(self):
        host = ("198.51.100.1", 40000)
        w = parse_welcome(self.send(req(R.T_CREATE, nonce=1), host)[0][0])
        self.srv.draining = True
        self.assertEqual(parse_error(self.send(req(R.T_CREATE, nonce=2), ("198.51.100.3", 1))[0][0])[0],
                         R.E_SERVER_FULL)
        self.assertEqual(self.send(req(R.T_JOIN, nonce=3, code=w["code"]), ("198.51.100.4", 1))[0][0][5], R.T_WELCOME)

    # ---- metrics ---------------------------------------------------------------------------------
    def test_metrics_cover_traffic_and_drops(self):
        host = ("198.51.100.1", 40000)
        w = parse_welcome(self.send(req(R.T_CREATE, nonce=1), host)[0][0])
        g = ("198.51.100.2", 40001)
        gw = parse_welcome(self.send(req(R.T_JOIN, nonce=2, code=w["code"]), g)[0][0])
        for addr, ww in ((host, w), (g, gw)):
            for p, a, _ in self.send(keepalive(w["code"], ww["slot"], ww["key"]), addr):
                self.srv.count_tx(p, True)
        for p, a, _ in self.send(auth(R.T_RELAY, w["code"], 1, gw["key"], arg=1, extra=gxmp(2)), g):
            self.srv.count_tx(p, True)
        self.send(auth(R.T_RELAY, w["code"], 1, gw["key"], arg=1, extra=gxmp(3)), g)  # spoofed slot
        self.send(b"JUNKJUNK", ("192.0.2.1", 1))
        self.send(req(R.T_JOIN, nonce=9, code="ZZZZZ"), ("192.0.2.2", 1))
        m = self.srv.metrics
        self.assertEqual(m.get("gdx_rv_rx_packets_total", type="create", proto="1"), 1)
        self.assertEqual(m.get("gdx_rv_rx_packets_total", type="keepalive", proto="1"), 2)
        self.assertEqual(m.get("gdx_rv_rx_packets_total", type="invalid", proto="0"), 1)
        self.assertEqual(m.get("gdx_rv_drops_total", reason="relay-spoof"), 1)
        self.assertEqual(m.get("gdx_rv_drops_total", reason="header"), 1)
        self.assertEqual(m.get("gdx_rv_errors_sent_total", code="room-not-found"), 1)
        self.assertEqual(m.get("gdx_rv_relay_packets_total"), 1)
        self.assertEqual(m.get("gdx_rv_tx_packets_total", type="relay"), 1)
        self.assertEqual(m.get("gdx_rv_tx_packets_total", type="ka_ack"), 2)
        text = R.render_metrics(m, self.srv.gauges(self.t) + R.host_gauges())
        for line in text.splitlines():
            if line and not line.startswith("#"):
                self.assertRegex(line, PROM_LINE)
        self.assertIn('gdx_rv_rooms{state="unconfirmed"} 0', text)
        self.assertIn('gdx_rv_rooms{state="open"} 1', text)
        self.assertIn("gdx_rv_members 2", text)
        self.assertIn('gdx_rv_handle_seconds_bucket{le="+Inf"}', text)

    def test_member_sessions_record_relay_use(self):
        host = ("198.51.100.1", 40000)
        w = parse_welcome(self.send(req(R.T_CREATE, nonce=1), host)[0][0])
        g = ("198.51.100.2", 40001)
        gw = parse_welcome(self.send(req(R.T_JOIN, nonce=2, code=w["code"]), g)[0][0])
        self.send(keepalive(w["code"], 0, w["key"]), host)
        self.send(keepalive(w["code"], 1, gw["key"]), g)
        self.send(auth(R.T_RELAY, w["code"], 1, gw["key"], arg=1, extra=gxlb(1)), g)
        self.t += 25
        self.send(auth(R.T_LEAVE, w["code"], 1, gw["key"]), g)
        self.send(auth(R.T_LEAVE, w["code"], 0, w["key"]), host)
        m = self.srv.metrics
        self.assertEqual(m.get("gdx_rv_member_sessions_total", relay="yes"), 1)
        self.assertEqual(m.get("gdx_rv_member_sessions_total", relay="no"), 1)
        self.assertEqual(m.get("gdx_rv_rooms_closed_total", why="leave"), 1)

    def test_host_gauges_from_proc(self):
        tmp = tempfile.mkdtemp(prefix="rdv-proc-")
        self.addCleanup(shutil.rmtree, tmp, True)
        os.makedirs(os.path.join(tmp, "self"))
        files = {"self/stat": "123 (python3 x) S " + " ".join(["0"] * 10) + " 250 50 " + " ".join(["0"] * 8) + " 4096 0\n",
                 "loadavg": "0.25 0.10 0.05 1/90 123\n",
                 "meminfo": "MemTotal: 460000 kB\nMemAvailable: 200000 kB\nSwapFree: 0 kB\n",
                 "stat": "cpu  100 0 50 1000 5 0 1 2 0 0\ncpu0 1 1 1 1\n"}
        for name, text in files.items():
            with open(os.path.join(tmp, name), "w") as f:
                f.write(text)
        g = {name: samples for name, _h, samples in R.host_gauges(tmp)}
        tick = float(os.sysconf("SC_CLK_TCK"))
        self.assertAlmostEqual(g["gdx_rv_process_cpu_seconds"][0][1], 300 / tick)
        self.assertEqual(g["gdx_rv_process_resident_bytes"][0][1], 4096 * os.sysconf("SC_PAGE_SIZE"))
        self.assertEqual(g["gdx_rv_host_load1"][0][1], 0.25)
        self.assertIn(((("kind", "available"),), 200000 * 1024), g["gdx_rv_host_memory_bytes"])
        self.assertIn(((("mode", "steal"),), 2 / tick), g["gdx_rv_host_cpu_seconds"])
        self.assertEqual(R.host_gauges(os.path.join(tmp, "missing")), [])

    # ---- the event log ---------------------------------------------------------------------------
    def test_event_log_ip_modes_and_flood_cap(self):
        import io
        for mode, check in (("full", lambda v: v == "203.0.113.7"), ("prefix", lambda v: v == "203.0.113.0/24"),
                            ("hash", lambda v: len(v) == 16 and "203" not in v), ("none", lambda v: v is None)):
            buf = io.StringIO()
            log = R.EventLog(json_lines=True, ip_mode=mode, stream=buf)
            log("join", code="ABCDE", ip="203.0.113.7", port=4000)
            rec = json.loads(buf.getvalue())
            self.assertTrue(check(rec.get("ip")), (mode, rec))
            self.assertEqual(rec["event"], "join")
            self.assertEqual("port" in rec, mode != "none")
        buf = io.StringIO()
        metrics = R.Metrics()
        log = R.EventLog(json_lines=True, metrics=metrics, stream=buf, clock=lambda: 1.0)
        for i in range(1000):
            log("refuse", op="join", ip="192.0.2.%d" % (i & 255))
        log("create", code="ABCDE")  # not a noisy event: never suppressed
        lines = buf.getvalue().splitlines()
        self.assertLessEqual(len(lines), 102)
        self.assertEqual(json.loads(lines[-1])["event"], "create")
        self.assertGreater(metrics.get("gdx_rv_log_suppressed_total", event="refuse"), 890)

    def test_access_log_lines(self):
        events = []
        self.srv.log = lambda ev, **f: events.append(dict(f, event=ev))
        host = ("198.51.100.1", 40000)
        w = parse_welcome(self.send(req(R.T_CREATE, nonce=1), host)[0][0])
        self.assertEqual(events, [])  # a v1 CREATE is logged once its address answers
        self.send(keepalive(w["code"], 0, w["key"]), host)
        self.send(req(R.T_JOIN, nonce=2, code="ZZZZZ"), ("192.0.2.2", 3))
        self.send(auth(R.T_LEAVE, w["code"], 0, w["key"]), host)
        # a spoofed CREATE that never confirms leaves no line at all
        self.send(req(R.T_CREATE, nonce=3), ("192.0.2.99", 4))
        self.t += 20
        self.srv.expire(self.t)
        kinds = [e["event"] for e in events]
        self.assertEqual(kinds, ["create", "refuse", "room-close"])
        self.assertEqual(events[0]["ip"], host[0])
        self.assertEqual(events[1]["err"], "room-not-found")
        self.assertEqual(events[1]["ip"], "192.0.2.2")
        self.assertEqual(events[2]["why"], "leave")

    def test_loki_sink_pushes_batches(self):
        import http.server
        got = []

        class H(http.server.BaseHTTPRequestHandler):
            def do_POST(self):  # noqa: N802
                got.append(json.loads(self.rfile.read(int(self.headers["Content-Length"]))))
                self.send_response(204)
                self.end_headers()

            def log_message(self, *a):
                pass

        httpd = http.server.HTTPServer(("127.0.0.1", 0), H)
        th = threading.Thread(target=httpd.serve_forever, daemon=True)
        th.start()
        try:
            sink = R.LokiSink("http://127.0.0.1:%d/loki/api/v1/push" % httpd.server_address[1],
                              {"job": "gdx-rendezvous", "host": "t"}, R.Metrics(), flush_s=0.1).start()
            log = R.EventLog(verbose=False, sink=sink)
            log("create", code="ABCDE", ip="203.0.113.1")
            log("join", code="ABCDE", ip="203.0.113.2")
            deadline = time.time() + 5
            while not got and time.time() < deadline:
                time.sleep(0.05)
            sink.stop()
        finally:
            httpd.shutdown()
            httpd.server_close()
        streams = [s for body in got for s in body["streams"]]
        self.assertEqual({s["stream"]["event"] for s in streams}, {"create", "join"})
        self.assertTrue(all(s["stream"]["job"] == "gdx-rendezvous" for s in streams))
        line = json.loads(streams[0]["values"][0][1])
        self.assertIn("code", line)
        self.assertEqual(sink.outcomes["sent"], 2)
        # an unreachable Loki costs a counted drop, never an exception in the event loop
        dead = R.LokiSink("http://127.0.0.1:9/x", {"job": "j"}, R.Metrics(), timeout_s=0.5)
        dead.push("create", 1.0, "{}")
        dead.flush()
        self.assertEqual(dead.outcomes["failed"], 1)


class ProcessOpsTest(unittest.TestCase):
    """The real process: /metrics, /healthz, --version, SIGTERM, SIGUSR1 drain."""

    def start(self, *extra):
        tmp = tempfile.mkdtemp(prefix="rdv-ops-")
        self.addCleanup(shutil.rmtree, tmp, True)
        pf = os.path.join(tmp, "port")
        mf = os.path.join(tmp, "mport")
        proc = subprocess.Popen([sys.executable, os.path.join(HERE, "gdx_rendezvous.py"), "--bind", "127.0.0.1",
                                 "--port", "0", "--port-file", pf, "--metrics-port", "-1", "--metrics-port-file", mf,
                                 "--json"] + list(extra), stdout=subprocess.PIPE, text=True)
        self.addCleanup(lambda: proc.poll() is None and proc.kill())
        def read(path):
            try:
                with open(path) as f:
                    return f.read().strip()
            except OSError:
                return ""

        deadline = time.time() + 10
        while time.time() < deadline:
            if read(mf) and read(pf):
                return proc, int(read(pf)), int(read(mf))
            time.sleep(0.05)
        self.fail("server did not start")

    def test_metrics_endpoint_and_sigterm(self):
        proc, port, mport = self.start("--allow-v1")
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.settimeout(3)
        s.sendto(req(R.T_CREATE, nonce=42), ("127.0.0.1", port))
        w = parse_welcome(s.recv(2048))
        s.sendto(keepalive(w["code"], 0, w["key"]), ("127.0.0.1", port))
        s.recv(2048)
        s.close()
        time.sleep(1.3)  # one render tick
        with urllib.request.urlopen("http://127.0.0.1:%d/metrics" % mport, timeout=3) as r:
            body = r.read().decode()
        self.assertIn('gdx_rv_rx_packets_total{proto="1",type="create"} 1', body)
        self.assertIn('gdx_rv_rooms{state="open"} 1', body)
        self.assertIn("gdx_rv_build_info{", body)
        with urllib.request.urlopen("http://127.0.0.1:%d/healthz" % mport, timeout=3) as r:
            self.assertEqual(r.status, 200)
        proc.send_signal(signal.SIGTERM)
        out, _ = proc.communicate(timeout=10)
        self.assertEqual(proc.returncode, 0)
        events = [json.loads(l)["event"] for l in out.splitlines() if l.startswith("{")]
        self.assertEqual(events[0], "listen")
        self.assertEqual(events[-1], "stop")

    def test_drain_exits_75_when_rooms_are_gone(self):
        proc, port, _mport = self.start("--allow-v1")
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.settimeout(3)
        s.sendto(req(R.T_CREATE, nonce=1), ("127.0.0.1", port))
        w = parse_welcome(s.recv(2048))
        s.sendto(keepalive(w["code"], 0, w["key"]), ("127.0.0.1", port))
        s.recv(2048)
        proc.send_signal(signal.SIGUSR1)
        time.sleep(1.5)
        self.assertIsNone(proc.poll())  # a room is still up
        s.sendto(req(R.T_CREATE, nonce=2), ("127.0.0.1", port))
        while True:
            p = s.recv(2048)
            if p[5] == R.T_ERROR:
                break
        self.assertEqual(parse_error(p)[0], R.E_SERVER_FULL)
        s.sendto(auth(R.T_LEAVE, w["code"], 0, w["key"]), ("127.0.0.1", port))
        s.close()
        proc.communicate(timeout=10)
        self.assertEqual(proc.returncode, 75)

    def test_version(self):
        out = subprocess.run([sys.executable, os.path.join(HERE, "gdx_rendezvous.py"), "--version"],
                             capture_output=True, text=True, timeout=10).stdout
        self.assertRegex(out, r"^gdx_rendezvous \S+ sha256=[0-9a-f]{64} proto=1,2")


class VersionGateTests(unittest.TestCase):
    """card-4: the cookie is required by default; v2 requests carry the client proto; UPDATE."""

    def setUp(self):
        self.srv = R.Rendezvous(rng=random.Random(11), update_url="example.org/update")
        self.t = 7000.0

    def send(self, pkt, addr):
        return self.srv.handle(pkt, addr, self.t)

    def v2_answer(self, t, addr, nonce, **kw):
        out = self.send(req2(t, nonce=nonce, **kw), addr)
        n, cookie = parse_cookie(out[0][0])
        self.assertEqual(n, nonce)
        return self.send(req2(t, cookie=cookie, nonce=nonce, **kw), addr)

    def test_mirrors_the_console_header(self):
        """One source of truth: the console's header; the server mirrors it."""
        with open(os.path.join(ROOT, "port", "gdx_mp_online.h")) as f:
            h = f.read()
        self.assertEqual(int(re.search(r"#define GDX_MPO_PROTO (\d+)", h).group(1)), R.CLIENT_PROTO)
        self.assertEqual(re.search(r'#define GDX_RV_NEEDS_V2_DETAIL "([^"]*)"', h).group(1), R.NEEDS_V2_DETAIL)
        self.assertIn("#define GDX_RV_VERSION_COOKIE 2", h)
        self.assertIn("GDX_RV_COOKIE = 0x%x" % R.T_COOKIE, h)
        self.assertIn("GDX_RV_UPDATE = 0x%x" % R.T_UPDATE, h)
        self.assertIn("#define GDX_RV_UPDATE_BYTES %d" % R.UPDATE_LEN, h)
        self.assertEqual(R.REQ2_LEN, 104)

    def test_default_requires_the_cookie_and_old_cards_read_why(self):
        self.assertTrue(R.Rendezvous().require_cookie)
        pkt = req(R.T_CREATE, nonce=3)
        out = self.send(pkt, ("198.51.100.9", 1))
        err, detail = parse_error(out[0][0])
        self.assertEqual(err, R.E_BUILD_MISMATCH)
        self.assertEqual(detail, R.NEEDS_V2_DETAIL)
        # A card before the cookie build keeps a detail only if every byte is 0x21..0x7E (its
        # GetFixed kind 0); with a space in it, it would show "version mismatch: host runs ".
        self.assertTrue(all(0x21 <= b <= 0x7E for b in detail.encode()), detail)
        self.assertLessEqual(len(detail), 40)
        self.assertLessEqual(len(out[0][0]), len(pkt))
        self.assertEqual(self.srv.rooms, {})
        out = self.send(req(R.T_JOIN, nonce=4, code="ABCDE"), ("198.51.100.9", 2))
        self.assertEqual(parse_error(out[0][0]), (R.E_BUILD_MISMATCH, R.NEEDS_V2_DETAIL))

    def test_allow_v1_admits_legacy_cards(self):
        srv = R.Rendezvous(rng=random.Random(1), require_cookie=False)
        out = srv.handle(req(R.T_CREATE, nonce=5), ("198.51.100.3", 7), 1.0)
        w = parse_welcome(out[0][0])
        self.assertEqual(out[0][0][4], 1)
        self.assertEqual(srv.rooms[w["code"]].members[0].client_proto, 0)  # a v1 card: unknown

    def test_v2_happy_path_records_the_client_proto(self):
        out = self.v2_answer(R.T_CREATE, ("198.51.100.1", 4000), 21)
        w = parse_welcome(out[0][0])
        self.assertEqual(out[0][0][4], 2)
        self.assertEqual(self.srv.rooms[w["code"]].members[0].client_proto, R.CLIENT_PROTO)

    def test_update_required_below_the_minimum(self):
        a = ("198.51.100.4", 4100)
        first = self.send(req2(R.T_CREATE, nonce=31, client_proto=R.CLIENT_PROTO - 1), a)
        self.assertEqual(first[0][0][5], R.T_COOKIE)  # nothing about versions before the address is proven
        out = self.v2_answer(R.T_CREATE, a, 31, client_proto=R.CLIENT_PROTO - 1)
        pkt = out[0][0]
        self.assertEqual((pkt[4], pkt[5], len(pkt)), (2, R.T_UPDATE, R.UPDATE_LEN))
        self.assertLessEqual(len(pkt), R.REQ2_LEN)  # no amplification
        nonce, lo, hi, yours, rsv = struct.unpack_from("<IHHHH", pkt, 8)
        self.assertEqual((nonce, lo, hi, yours, rsv), (31, R.CLIENT_PROTO, R.CLIENT_PROTO, R.CLIENT_PROTO - 1, 0))
        self.assertEqual(pkt[20:].rstrip(b"\0"), b"example.org/update")
        self.assertEqual(self.srv.rooms, {})
        self.assertEqual(self.srv.metrics.total("gdx_rv_update_required_total"), 1)

    def test_server_outdated_above_the_maximum_and_no_ceiling(self):
        out = self.v2_answer(R.T_JOIN, ("198.51.100.5", 4200), 41, code="ABCDE", client_proto=R.CLIENT_PROTO + 1)
        self.assertEqual(out[0][0][5], R.T_UPDATE)
        self.assertEqual(struct.unpack_from("<H", out[0][0], 14)[0], R.CLIENT_PROTO)
        self.srv.max_client_proto = 0  # --max-client-proto 0: any newer client is fine
        out = self.v2_answer(R.T_CREATE, ("198.51.100.5", 4201), 42, client_proto=R.CLIENT_PROTO + 5)
        self.assertEqual(out[0][0][5], R.T_WELCOME)
        self.srv.min_client_proto = 1  # a host keeping older clients during a transition
        out = self.v2_answer(R.T_CREATE, ("198.51.100.5", 4202), 43, client_proto=R.CLIENT_PROTO - 1)
        self.assertEqual(out[0][0][5], R.T_WELCOME)

    def test_bad_reserved_bytes_are_dropped(self):
        pkt = req2(R.T_CREATE, nonce=1)[:-2] + b"\x01\x00"
        self.assertEqual(self.send(pkt, ("198.51.100.6", 1)), [])

    def test_mismatch_tells_a_v2_joiner_the_host_proto(self):
        out = self.v2_answer(R.T_CREATE, ("198.51.100.1", 4000), 51)
        code = parse_welcome(out[0][0])["code"]
        out = self.v2_answer(R.T_JOIN, ("198.51.100.2", 4001), 52, code=code, build="other@build")
        err, detail = parse_error(out[0][0])
        self.assertEqual((err, detail), (R.E_BUILD_MISMATCH, BUILD))
        self.assertEqual(struct.unpack_from("<H", out[0][0], 14)[0], R.CLIENT_PROTO)
        out = self.v2_answer(R.T_JOIN, ("198.51.100.2", 4002), 53, code=code, content=1)
        self.assertEqual(out[0][0][12], R.E_CONTENT_MISMATCH)
        self.assertEqual(struct.unpack_from("<H", out[0][0], 14)[0], R.CLIENT_PROTO)
        # a v1 joiner (--allow-v1) gets the old layout: 0 there
        self.srv.require_cookie = False
        out = self.send(req(R.T_JOIN, nonce=54, code=code, build="other@build"), ("198.51.100.3", 4003))
        self.assertEqual(struct.unpack_from("<H", out[0][0], 14)[0], 0)

    def test_cli_flags(self):
        for bad in (["--require-cookie", "--allow-v1"], ["--update-url", "x" * 41], ["--update-url", "caf\u00e9"],
                    ["--min-client-proto", "10", "--max-client-proto", "9"]):
            with self.assertRaises(SystemExit):
                with open(os.devnull, "w") as null:
                    old = sys.stderr
                    sys.stderr = null
                    try:
                        R.main(["--port", "1"] + bad)
                    finally:
                        sys.stderr = old


def start_server(tmp, name, *extra):
    pf = os.path.join(tmp, name + ".port")
    proc = subprocess.Popen([sys.executable, os.path.join(HERE, "gdx_rendezvous.py"), "--bind", "127.0.0.1",
                             "--port", "0", "--port-file", pf, "--quiet"] + list(extra))
    deadline = time.time() + 10
    while time.time() < deadline:
        if os.path.exists(pf):
            with open(pf) as f:
                txt = f.read().strip()
            if txt:
                return proc, int(txt)
        time.sleep(0.05)
    return proc, None


class OldServer(threading.Thread):
    """A server before 2.0.0 for the probe: the same room logic, but every version-2 datagram is
    dropped as junk (what 1.x did with a header version it did not know)."""

    def __init__(self):
        super().__init__(daemon=True)
        self.core = R.Rendezvous(rng=random.Random(3), require_cookie=False)
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("127.0.0.1", 0))
        self.sock.settimeout(0.2)
        self.port = self.sock.getsockname()[1]
        self.stop = False
        self.dropped_v2 = 0

    def run(self):
        while not self.stop:
            try:
                data, addr = self.sock.recvfrom(2048)
            except OSError:
                continue
            if len(data) > 4 and data[4] == R.GXRV_VERSION_COOKIE:
                self.dropped_v2 += 1
                continue
            for pkt, dst, _delay in self.core.handle(data, addr, time.monotonic()):
                self.sock.sendto(pkt, dst)


class InteropTest(unittest.TestCase):
    """The real server process + the console's own C protocol code over loopback: the default
    (cookie required), --allow-v1, and a stand-in for a server before 2.0.0."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix="rdv-test-")
        cls.proc, cls.port = start_server(cls.tmp, "req", "--update-url", "example.org/update")
        cls.proc_v1, cls.port_v1 = start_server(cls.tmp, "v1", "--allow-v1")
        cls.old = OldServer()
        cls.old.start()
        cls.exe = None

    @classmethod
    def tearDownClass(cls):
        for proc in (cls.proc, cls.proc_v1):
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
        cls.old.stop = True
        cls.old.join(timeout=2)
        cls.old.sock.close()
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def probe(self, port, mode):
        cc = shutil.which("cc") or shutil.which("clang") or shutil.which("gcc")
        if cc is None:
            self.skipTest("no C compiler")
        if InteropTest.exe is None:
            exe = os.path.join(self.tmp, "probe")
            src = os.path.join(ROOT, "port")
            subprocess.check_call([cc, "-O1", "-Wall", "-I" + src, os.path.join(src, "tests", "gdx_mp_online_wire_probe.c"),
                                   os.path.join(src, "gdx_mp_online.c"), os.path.join(src, "gdx_mp_lobby.c"), "-o", exe])
            InteropTest.exe = exe
        out = subprocess.run([InteropTest.exe, str(port), mode], capture_output=True, text=True, timeout=60)
        sys.stdout.write(out.stdout)
        sys.stderr.write(out.stderr)
        self.assertEqual(out.returncode, 0)
        self.assertIn("WIRE PROBE PASS", out.stdout)
        return out.stdout

    def test_server_answers(self):
        self.assertIsNotNone(self.port)
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.settimeout(3)
        self.addCleanup(s.close)
        s.sendto(req(R.T_CREATE, nonce=42), ("127.0.0.1", self.port))
        self.assertEqual(parse_error(s.recv(2048)), (R.E_BUILD_MISMATCH, R.NEEDS_V2_DETAIL))
        s.sendto(req2(R.T_CREATE, nonce=43), ("127.0.0.1", self.port))
        _n, cookie = parse_cookie(s.recv(2048))
        s.sendto(req2(R.T_CREATE, nonce=43, cookie=cookie), ("127.0.0.1", self.port))
        self.assertEqual(parse_welcome(s.recv(2048))["nonce"], 43)

    def test_c_wire_probe(self):
        """v2 happy path: the console's state machine (mode 1) against the default server."""
        out = self.probe(self.port, "v2")
        self.assertIn("[mpo] rv cookie: v2 request -> COOKIE -> resend ok", out)
        self.assertIn("(same version, other build)", out)

    def test_c_wire_probe_refused(self):
        """What an old card sees; auto recovering a lossy first second; UPDATE both ways."""
        out = self.probe(self.port, "refused")
        self.assertIn('old card shows: "version mismatch: host runs %s"' % R.NEEDS_V2_DETAIL, out)
        self.assertIn("back to v2", out)
        self.assertIn("UPDATE REQUIRED - this server needs version %d (you have %d) - example.org/update" %
                      (R.CLIENT_PROTO, R.CLIENT_PROTO - 1), out)
        self.assertIn("SERVER NEEDS AN UPDATE - it supports up to version %d (you have %d)" %
                      (R.CLIENT_PROTO, R.CLIENT_PROTO + 1), out)

    def test_c_wire_probe_allow_v1(self):
        self.probe(self.port_v1, "v1")

    def test_c_wire_probe_auto_falls_back_on_an_old_server(self):
        out = self.probe(self.old.port, "old")
        self.assertIn("fallback to v1", out)
        self.assertGreaterEqual(self.old.dropped_v2, 2)


if __name__ == "__main__":
    unittest.main(verbosity=1)
