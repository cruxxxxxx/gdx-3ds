#!/usr/bin/env python3
"""tools/mp-tickmodel/mp_tickmodel.py -- discrete-event model of a 2-console lockstep race.

Why: the hardware logs (test-combined 90049af, New 3DS host + Old 3DS guest, UDS, 2P+14) give
per-tick bucket averages ([mprof] inp/pace/tail, [mpcost], [frameskip] steady rw/skip) but not
the interplay. This model re-runs the main loop of port/3ds/main_3ds.cpp tick by tick on both
consoles, with the real ordering:

    Exchange (gdx3ds_mpo_exchange: push pad t+delay, SendOurPads, WaitReady(t) -- Service, then
    poll every 0.5 ms, resend every 16 ms)  ->  frameskip decide (steady slot begin: wait for the
    slot's vblank, or start at once when it is gone)  ->  render tick (rw) + present hold to vblank
    S+D-1, or a skip tick  ->  tail (after_tick: hash + Service; telemetry)

and a network that delivers each datagram after a sampled one-way latency (or drops it; the next
packet carries the same pads again -- GDX_MP_REDUNDANCY). A receiver only sees a datagram at its
next Drain (Service), as on hardware. Old 3DS IPC (udsPullPacket / recvfrom / udsSendTo) is a
sampled latency, because the sysmodules share core 1 with the game's audio threads (the app holds
60% of core 1: [core1] syscore_pct=60).

Calibration (the baseline scenario reproduces the hardware receipt within a few %):
    Old 3DS : 26.2 ms/tick, pace 6.1, inp 5.1, tail 8.3, tps 38, D=4      (log o3ds-900 f5120-5632)
    New 3DS : 26.2 ms/tick, inp 17.1, pace 4.4,                tps 38, D=2 (log n3ds-900 f5632)

Standard library only.  `mp_tickmodel.py --list` / `--scenario NAME` / `--all --json out.json`.
"""
import argparse
import heapq
import json
import math
import random
import sys

VBLANK_MS = 1000.0 / 59.8337
TICK_MS = 1000.0 / 59.94  # N64 NTSC field


# --------------------------------------------------------------------------------------------
# Network
# --------------------------------------------------------------------------------------------
class Link:
    """One direction. latency(): one-way ms; loss: drop probability; burst: (period_ms, len_ms)
    an outage window repeating every period (radio interference / a beacon scan)."""

    def __init__(self, rng, base_ms=4.0, jitter_ms=3.0, tail_p=0.02, tail_ms=40.0, loss=0.0, burst=None):
        self.rng = rng
        self.base = base_ms
        self.jitter = jitter_ms
        self.tail_p = tail_p
        self.tail_ms = tail_ms
        self.loss = loss
        self.burst = burst

    def deliver(self, t_send):
        if self.loss > 0 and self.rng.random() < self.loss:
            return None
        lat = self.base + self.rng.expovariate(1.0 / self.jitter) if self.jitter > 0 else self.base
        if self.tail_p > 0 and self.rng.random() < self.tail_p:
            lat += self.rng.expovariate(1.0 / self.tail_ms)
        t = t_send + lat
        if self.burst is not None:
            period, length = self.burst
            phase = t % period
            if phase < length:
                t += length - phase  # held until the outage ends
        return t


# --------------------------------------------------------------------------------------------
# Console
# --------------------------------------------------------------------------------------------
class Ipc:
    """Latency of one sysmodule call from the main thread. fast: (p, ms); slow tail uniform."""

    def __init__(self, rng, fast_ms=0.12, slow_p=0.0, slow_lo=0.0, slow_hi=0.0):
        self.rng = rng
        self.fast = fast_ms
        self.slow_p = slow_p
        self.slow_lo = slow_lo
        self.slow_hi = slow_hi

    def cost(self):
        if self.slow_p > 0 and self.rng.random() < self.slow_p:
            return self.rng.uniform(self.slow_lo, self.slow_hi)
        return self.fast


class Cfg:
    def __init__(self, **kw):
        self.name = "console"
        self.D = 1               # steady divisor (1 = render every tick, paced by vsync)
        self.hold = True         # steady: hold the present to vblank S+D-1
        self.align = True        # steady: wait for the slot's start vblank
        self.rw = 3.0            # render tick work (slot begin -> present), ms
        self.rw_sd = 0.5
        self.skip = 1.0          # a skipped-render (hollow) tick's work
        self.other = 1.0         # per-tick telemetry / non-MP tail
        self.hash_ms = 0.01      # gstate digest
        self.ipc_send = 1        # IPCs per send (udsSendTo)
        self.ipc_service = 2     # IPCs per Service with nothing waiting (udsPull empty + recvfrom)
        self.ipc_per_pkt = 1     # extra udsPull per datagram drained
        self.ipc_tail_extra = 1  # discovery recvfrom in after_tick
        self.service_cpu = 0.03  # non-IPC CPU per Service
        self.poll_ms = 0.5       # WaitReady sleep between polls
        self.resend_ms = 16.0
        self.net_thread = False  # IPC on a service thread: main never pays IPC, sees pads at arrival+0.2
        self.event_gate = False  # check the UDS bind event before udsPullPacket (no empty pulls)
        self.soc_in_uds = True   # recvfrom on the SOC socket on every Service while in a UDS room
        self.input_every = 1     # exchange (and wait) every N ticks; the pad is held N ticks
        self.time_sync = False   # frame-advantage controller (sleep when ahead of the peer)
        self.hitch_every_ms = 0  # periodic main-thread hitch (sync log flush)
        self.hitch_ms = 0.0
        self.hitch_rare_p = 0.0  # per-flush chance of the SD outlier
        self.hitch_rare_ms = 0.0
        self.vbl_phase = 0.0
        for k, v in kw.items():
            if not hasattr(self, k):
                raise KeyError(k)
            setattr(self, k, v)


class Stats:
    def __init__(self):
        self.ticks = 0
        self.rendered = 0
        self.t_first = None
        self.t_last = 0.0
        self.wait = []        # genuine lockstep wait per tick (sleep time), ms
        self.bucket = {"inp_cpu": 0.0, "inp_wait": 0.0, "pace": 0.0, "work": 0.0, "tail": 0.0, "sync": 0.0}
        self.stall_timer = []  # what WaitReady's own timer reports (Service CPU + wait), ms
        self.render_times = []


class Console:
    def __init__(self, idx, cfg, sim, rng):
        self.idx = idx
        self.cfg = cfg
        self.sim = sim
        self.rng = rng
        self.ipc = None
        self.now = 0.0
        self.stats = Stats()
        self.inbox = []       # (arrival_ms, newest_known) heap
        self.remote_known = 0  # contiguous pads known from the peer (exclusive)
        self.slot_vbl = None
        self.slot_pos = 0
        self.next_hitch = cfg.hitch_every_ms if cfg.hitch_every_ms else None
        self.peer_tick_est = 0

    # -- helpers -----------------------------------------------------------------------------
    def vblank_at_or_after(self, t):
        ph = self.cfg.vbl_phase
        k = math.ceil((t - ph) / VBLANK_MS)
        return ph + k * VBLANK_MS

    def vblank_at_or_before(self, t):
        ph = self.cfg.vbl_phase
        k = math.floor((t - ph) / VBLANK_MS)
        return ph + k * VBLANK_MS

    def ipc_cost(self, n):
        if self.cfg.net_thread:
            return 0.0
        return sum(self.ipc.cost() for _ in range(n))

    def drain(self):
        """Service: returns its main-thread cost; makes arrived datagrams visible."""
        c = self.cfg
        if c.net_thread:
            # the thread pulled them as they arrived; main reads a ring
            while self.inbox and self.inbox[0][0] + 0.2 <= self.now:
                _, k = heapq.heappop(self.inbox)
                self.remote_known = max(self.remote_known, k)
            return 0.02
        n_ipc = 0
        pulled = 0
        while self.inbox and self.inbox[0][0] <= self.now:
            _, k = heapq.heappop(self.inbox)
            self.remote_known = max(self.remote_known, k)
            pulled += 1
        if c.event_gate:
            n_ipc += pulled * c.ipc_per_pkt + (1 if pulled else 0)
        else:
            n_ipc += 1 + pulled * c.ipc_per_pkt
        if c.soc_in_uds:
            n_ipc += 1
        return self.ipc_cost(n_ipc) + c.service_cpu + 0.01 * pulled

    def send(self, known_local):
        c = self.cfg
        peer = self.sim.consoles[1 - self.idx]
        t = self.sim.links[self.idx].deliver(self.now)
        if t is not None:
            heapq.heappush(peer.inbox, (t, known_local))
        if c.net_thread:
            return 0.03
        return self.ipc_cost(c.ipc_send) + 0.02

    # -- the main loop as a generator: yields absolute resume times ---------------------------
    def run(self, ticks, delay):
        c = self.cfg
        st = self.stats
        last_send = 0.0
        for t in range(ticks):
            if st.t_first is None:
                st.t_first = self.now
            # hitch (sync SD log write from the tail of the previous tick)
            if self.next_hitch is not None and self.now >= self.next_hitch:
                h = c.hitch_ms
                if c.hitch_rare_p and self.rng.random() < c.hitch_rare_p:
                    h = c.hitch_rare_ms
                st.bucket["tail"] += h
                self.now += h
                yield self.now
                self.next_hitch += c.hitch_every_ms
            # ---- time sync (GGPO frame advantage, lockstep flavour) ----
            if c.time_sync:
                peer_tick = self.remote_known - delay  # the peer's newest pad says where it is
                adv = t - peer_tick
                if adv > 1:
                    s = min(4.0, 0.5 * (adv - 1))
                    st.bucket["sync"] += s
                    self.now += s
                    yield self.now
            # ---- Exchange ----
            exch_tick = (t % c.input_every) == 0
            if exch_tick:
                known_local = t + delay + c.input_every  # pads for [.., t+delay+N) now exist
                cost = self.send(known_local)
                last_send = self.now
                st.bucket["inp_cpu"] += cost
                self.now += cost
                yield self.now
                t0 = self.now
                cost = self.drain()
                st.bucket["inp_cpu"] += cost
                self.now += cost
                yield self.now
                cpu_in_timer = cost
                waited = 0.0
                need = t + c.input_every  # the peer's pads for [t, t+N) -- known >= t+N
                while self.remote_known < need:
                    self.now += c.poll_ms
                    waited += c.poll_ms
                    yield self.now
                    if self.now - last_send >= c.resend_ms:
                        cost = self.send(known_local)
                        last_send = self.now
                        st.bucket["inp_cpu"] += cost
                        cpu_in_timer += cost
                        self.now += cost
                        yield self.now
                    cost = self.drain()
                    st.bucket["inp_cpu"] += cost
                    cpu_in_timer += cost
                    self.now += cost
                    yield self.now
                    if waited > 30000:
                        raise RuntimeError("deadlock")
                st.bucket["inp_wait"] += waited
                st.wait.append(waited)
                st.stall_timer.append(self.now - t0)
            else:
                st.wait.append(0.0)
                st.stall_timer.append(0.0)
            # ---- frameskip + tick ----
            render = (self.slot_pos == 0)
            if c.D == 1:
                w = max(0.5, self.rng.gauss(c.rw, c.rw_sd))
                st.bucket["work"] += w
                self.now += w
                yield self.now
                # vsync present: FrameBegin of the next frame waits for the vblank
                v = self.vblank_at_or_after(self.now)
                st.bucket["pace"] += v - self.now
                self.now = v
                yield self.now
                st.rendered += 1
                st.render_times.append(self.now)
            elif render:
                period = VBLANK_MS
                if self.slot_vbl is None:
                    self.slot_vbl = self.vblank_at_or_after(self.now)
                    st.bucket["pace"] += self.slot_vbl - self.now
                    self.now = self.slot_vbl
                    yield self.now
                else:
                    target = self.slot_vbl + c.D * period
                    if c.align and self.now + 1.0 < target:
                        v = self.vblank_at_or_after(target - period / 2)
                        st.bucket["pace"] += v - self.now
                        self.now = v
                        self.slot_vbl = v
                        yield self.now
                    elif self.now > target + period / 2:
                        self.slot_vbl = self.vblank_at_or_before(self.now)
                    else:
                        self.slot_vbl = target
                w = max(0.5, self.rng.gauss(c.rw, c.rw_sd))
                st.bucket["work"] += w
                self.now += w
                yield self.now
                if c.hold:
                    hold_t = self.slot_vbl + (c.D - 1) * period
                    if self.now + 1.0 < hold_t:
                        v = self.vblank_at_or_after(hold_t - period / 2)
                        st.bucket["pace"] += v - self.now
                        self.now = v
                        yield self.now
                st.rendered += 1
                st.render_times.append(self.now)
            else:
                w = max(0.2, self.rng.gauss(c.skip, c.skip * 0.1))
                st.bucket["work"] += w
                self.now += w
                yield self.now
            if c.D > 1:
                self.slot_pos = (self.slot_pos + 1) % c.D
            # ---- tail: after_tick (hash, Service, discovery) + telemetry ----
            cost = c.hash_ms + c.other
            cost += self.drain()
            if not c.net_thread:
                cost += self.ipc_cost(c.ipc_tail_extra)
            st.bucket["tail"] += cost
            self.now += cost
            yield self.now
            st.ticks += 1
            st.t_last = self.now


class Sim:
    def __init__(self, cfgs, links, ipcs, delay, ticks, seed):
        self.rng = random.Random(seed)
        self.consoles = []
        self.links = links
        for i, c in enumerate(cfgs):
            con = Console(i, c, self, random.Random(seed * 31 + i))
            con.ipc = ipcs[i]
            self.consoles.append(con)
        self.delay = delay
        self.ticks = ticks

    def run(self):
        gens = [c.run(self.ticks, self.delay) for c in self.consoles]
        heap = [(0.0, i) for i in range(len(gens))]
        heapq.heapify(heap)
        alive = len(gens)
        while heap:
            t, i = heapq.heappop(heap)
            try:
                nt = next(gens[i])
            except StopIteration:
                alive -= 1
                continue
            heapq.heappush(heap, (nt, i))
        return self.report()

    def report(self):
        out = {}
        for con in self.consoles:
            st = con.stats
            # steady-state window: skip the first 120 ticks
            n = st.ticks
            dur = st.t_last - st.t_first
            waits = st.wait[120:]
            timer = st.stall_timer[120:]
            w_sorted = sorted(waits)
            per_tick = {k: v / n for k, v in st.bucket.items()}
            stalls_1ms = sum(1 for w in waits if w > 1.0)
            stalls_timer = sum(1 for w in timer if w > 0.2)
            big = sum(1 for w in waits if w > 33.0)
            scale = 1800.0 / max(1, len(waits))
            out[con.cfg.name] = {
                "tps": round(n / (dur / 1000.0), 1),
                "fps": round(st.rendered / (dur / 1000.0), 1),
                "ms_per_tick": round(dur / n, 2),
                "per_tick_ms": {k: round(v, 2) for k, v in per_tick.items()},
                "stalls_per_1800": round(stalls_1ms * scale),
                "timer_stalls_per_1800": round(stalls_timer * scale),
                "stalls_over_33ms_per_1800": round(big * scale, 1),
                "wait_p50": round(w_sorted[len(w_sorted) // 2], 1) if w_sorted else 0,
                "wait_p99": round(w_sorted[int(len(w_sorted) * 0.99)], 1) if w_sorted else 0,
                "wait_max": round(w_sorted[-1], 1) if w_sorted else 0,
            }
        return out


# --------------------------------------------------------------------------------------------
# Calibrated consoles and scenarios
# --------------------------------------------------------------------------------------------
def o3ds(**kw):
    base = dict(name="o3ds", D=4, hold=True, rw=19.0, rw_sd=2.5, skip=2.5, other=3.2, hash_ms=0.05,
                vbl_phase=7.3, hitch_every_ms=5000, hitch_ms=55.0, hitch_rare_p=0.02, hitch_rare_ms=1300.0)
    base.update(kw)
    return Cfg(**base)


def n3ds(**kw):
    base = dict(name="n3ds", D=2, hold=True, rw=3.0, rw_sd=0.6, skip=1.0, other=1.45, hash_ms=0.01,
                vbl_phase=0.0)
    base.update(kw)
    return Cfg(**base)


# Old 3DS sysmodule IPC: most calls ~0.06 ms, ~20% wait for the app's core-1 slice (2..10 ms):
# fits [mpcost] (after_tick 4.5 ms, exchange 4.7 ms) and WaitReady's timer > 200 us on 37% of ticks.
def o3ds_ipc(rng, slow_p=0.2):
    return Ipc(rng, fast_ms=0.06, slow_p=slow_p, slow_lo=2.0, slow_hi=10.0)


def n3ds_ipc(rng):
    return Ipc(rng, fast_ms=0.05, slow_p=0.0)


def uds_links(rng, base=4.0, jitter=3.0, tail_p=0.02, tail_ms=40.0, loss=0.01, burst=None):
    return [Link(rng, base, jitter, tail_p, tail_ms, loss, burst), Link(rng, base, jitter, tail_p, tail_ms, loss, burst)]


FIXES_IPC = dict(event_gate=True, soc_in_uds=False, ipc_tail_extra=0)


def scenario_table():
    """name -> (description, host cfg kw, guest cfg kw, delay, link kw, guest ipc slow_p)."""
    S = {}
    S["baseline"] = ("hardware 90049af: steady hold on, ~6 IPC/tick on the Old 3DS, delay 4",
                     {}, {}, 4, {}, 0.2)
    S["delay7"] = ("d07ae82: delay 7 (+3 for an Old 3DS), nothing else", {}, {}, 7, {}, 0.2)
    S["no_hold"] = ("lockstep race: no present hold on either console", dict(hold=False), dict(hold=False), 4, {}, 0.2)
    S["host_d1"] = ("host frameskip excludes lockstep wait: renders every tick (D=1)", dict(D=1), {}, 4, {}, 0.2)
    S["frameskip_fix"] = ("prototype frameskip half only: no hold in lockstep + host D=1 (IPC unchanged)",
                          dict(D=1, hold=False), dict(hold=False), 4, {}, 0.2)
    S["frameskip_fix_d3"] = ("frameskip_fix with the Old 3DS at D=3", dict(D=1, hold=False), dict(D=3, hold=False), 4, {}, 0.2)
    S["ipc_fix"] = ("event-gated udsPullPacket, no SOC recvfrom / discovery in a UDS race", {}, FIXES_IPC, 4, {}, 0.2)
    S["near_term"] = ("no hold + host D=1 + IPC fix (all main-thread, small diffs)",
                      dict(D=1, hold=False, **FIXES_IPC), dict(hold=False, **FIXES_IPC), 4, {}, 0.2)
    S["near_term_d3"] = ("near_term with the Old 3DS at D=3 (20 fps)",
                         dict(D=1, hold=False, **FIXES_IPC), dict(D=3, hold=False, **FIXES_IPC), 4, {}, 0.2)
    S["near_term_d2"] = ("near_term with the Old 3DS at D=2 (30 fps)",
                         dict(D=1, hold=False, **FIXES_IPC), dict(D=2, hold=False, **FIXES_IPC), 4, {}, 0.2)
    S["net_thread"] = ("near_term + network service thread (IPC off the main thread)",
                       dict(D=1, hold=False, net_thread=True), dict(D=3, hold=False, net_thread=True), 4, {}, 0.2)
    S["net_thread_d2"] = ("net_thread with the Old 3DS at D=2",
                          dict(D=1, hold=False, net_thread=True), dict(D=2, hold=False, net_thread=True), 4, {}, 0.2)
    S["near_term_sync"] = ("near_term_d3 + frame-advantage time sync",
                           dict(D=1, hold=False, time_sync=True, **FIXES_IPC),
                           dict(D=3, hold=False, time_sync=True, **FIXES_IPC), 4, {}, 0.2)
    S["input2"] = ("near_term_d3 + inputs exchanged every 2 ticks (held 2)",
                   dict(D=1, hold=False, input_every=2, **FIXES_IPC),
                   dict(D=3, hold=False, input_every=2, **FIXES_IPC), 4, {}, 0.2)
    S["no_log_hitch"] = ("near_term_d3 without the Old 3DS sync log flushes",
                         dict(D=1, hold=False, **FIXES_IPC),
                         dict(D=3, hold=False, hitch_every_ms=0, **FIXES_IPC), 4, {}, 0.2)
    return S


def build(name, seed=1, ticks=3600, link_override=None, delay_override=None):
    desc, hk, gk, delay, lk, slow_p = scenario_table()[name]
    rng = random.Random(seed)
    lk = dict(lk)
    if link_override:
        lk.update(link_override)
    links = uds_links(rng, **lk)
    cfgs = [n3ds(**hk), o3ds(**gk)]
    ipcs = [n3ds_ipc(random.Random(seed + 7)), o3ds_ipc(random.Random(seed + 9), slow_p)]
    return Sim(cfgs, links, ipcs, delay_override or delay, ticks, seed), desc


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--scenario", action="append")
    ap.add_argument("--all", action="store_true")
    ap.add_argument("--ticks", type=int, default=3600)
    ap.add_argument("--seeds", type=int, default=3)
    ap.add_argument("--sweep", action="store_true", help="latency/jitter/loss/delay sweep of baseline vs near_term_d3")
    ap.add_argument("--json")
    a = ap.parse_args()
    table = scenario_table()
    if a.list:
        for k, v in table.items():
            print("%-16s %s" % (k, v[0]))
        return 0
    names = list(table) if a.all or not a.scenario else a.scenario
    results = {}
    for n in names:
        runs = []
        for s in range(a.seeds):
            sim, desc = build(n, seed=s + 1, ticks=a.ticks)
            runs.append(sim.run())
        results[n] = {"desc": table[n][0], "runs": runs, "mean": mean_runs(runs)}
        pr(n, results[n])
    if a.sweep:
        results["sweep"] = sweep(a)
    if a.json:
        with open(a.json, "w") as f:
            json.dump(results, f, indent=1)
    return 0


def mean_runs(runs):
    out = {}
    for con in runs[0]:
        m = {}
        for k, v in runs[0][con].items():
            if isinstance(v, dict):
                m[k] = {kk: round(sum(r[con][k][kk] for r in runs) / len(runs), 2) for kk in v}
            else:
                m[k] = round(sum(r[con][k] for r in runs) / len(runs), 2)
        out[con] = m
    return out


def pr(name, res):
    m = res["mean"]
    print("== %s: %s" % (name, res["desc"]))
    for con, v in m.items():
        b = v["per_tick_ms"]
        print("   %-5s tps=%5.1f fps=%5.1f ms/tick=%5.2f | inp_cpu=%4.2f inp_wait=%5.2f pace=%4.2f work=%5.2f tail=%4.2f sync=%4.2f"
              " | stalls/1800=%4d (timer %4d, >33ms %4.1f) wait p50/p99/max=%s/%s/%s" % (
                  con, v["tps"], v["fps"], v["ms_per_tick"], b["inp_cpu"], b["inp_wait"], b["pace"], b["work"],
                  b["tail"], b["sync"], v["stalls_per_1800"], v["timer_stalls_per_1800"],
                  v["stalls_over_33ms_per_1800"], v["wait_p50"], v["wait_p99"], v["wait_max"]))


def sweep(a):
    rows = []
    cases = [
        ("uds_measured", dict()),
        ("lat_20ms", dict(base=20.0, jitter=5.0)),
        ("lat_40ms_jit15", dict(base=40.0, jitter=15.0)),
        ("tail10pct_80ms", dict(tail_p=0.10, tail_ms=80.0)),
        ("loss5", dict(loss=0.05)),
        ("loss20", dict(loss=0.20)),
        ("outage_150ms_every_3s", dict(burst=(3000.0, 150.0))),
    ]
    for scen in ("baseline", "near_term_d3", "net_thread"):
        for delay in (2, 3, 4, 6, 7):
            for cname, lk in cases:
                vals = []
                for s in range(a.seeds):
                    sim, _ = build(scen, seed=s + 1, ticks=a.ticks, link_override=lk, delay_override=delay)
                    vals.append(sim.run())
                m = mean_runs(vals)
                rows.append({"scenario": scen, "delay": delay, "link": cname,
                             "host": {k: m["n3ds"][k] for k in ("tps", "fps", "stalls_per_1800", "stalls_over_33ms_per_1800", "wait_p99")},
                             "guest": {k: m["o3ds"][k] for k in ("tps", "fps", "stalls_per_1800", "stalls_over_33ms_per_1800", "wait_p99")}})
                r = rows[-1]
                print("sweep %-13s d=%d %-22s host tps=%5.1f fps=%5.1f stalls=%4d big=%4.1f | guest tps=%5.1f fps=%5.1f stalls=%4d" % (
                    scen, delay, cname, r["host"]["tps"], r["host"]["fps"], r["host"]["stalls_per_1800"],
                    r["host"]["stalls_over_33ms_per_1800"], r["guest"]["tps"], r["guest"]["fps"], r["guest"]["stalls_per_1800"]))
    return rows


if __name__ == "__main__":
    sys.exit(main())
