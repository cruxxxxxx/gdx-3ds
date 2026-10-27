/* port/tests/gdx_mp_lockstep_host_tests.c — the MP spike's lockstep core (port/gdx_mp_lockstep.c)
 * against simulated consoles: N players, a lossy/reordering/duplicating "network", a toy
 * deterministic simulation fed by the exchanged pads. Checks:
 *   1. every console feeds the game the identical pad sequence and reaches the identical state;
 *   2. the neutral delay prefix, and delay+redundancy surviving a high loss rate;
 *   3. a deliberate state perturbation on one console is caught at the first hash tick after it;
 *   4. hostile datagrams (truncated, wrong magic/version/session, oversize count, own echo,
 *      far-future ticks) are refused without touching state. */
#include "gdx_mp_lockstep.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int sFailures = 0;
static int sChecks = 0;

#define CHECK(cond)                                                                   \
    do {                                                                              \
        sChecks++;                                                                    \
        if (!(cond)) {                                                                \
            sFailures++;                                                              \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
        }                                                                             \
    } while (0)

static uint32_t sRng = 12345u;

static uint32_t Rand(void) {
    sRng = sRng * 1103515245u + 12345u;
    return (sRng >> 8) & 0xFFFFFFu;
}

#define MAX_INFLIGHT 4096

typedef struct Datagram {
    int to;
    int len;
    uint32_t deliverAt;
    uint8_t data[GDX_MP_MAX_PACKET];
} Datagram;

typedef struct Net {
    Datagram q[MAX_INFLIGHT];
    int n;
    int lossPct;
    int dupPct;
    int jitter;
} Net;

static void NetSend(Net* net, int to, const uint8_t* buf, int len, uint32_t now) {
    int copies = 1;
    int c;
    if ((int) (Rand() % 100) < net->lossPct) {
        return;
    }
    if ((int) (Rand() % 100) < net->dupPct) {
        copies = 2;
    }
    for (c = 0; c < copies && net->n < MAX_INFLIGHT; c++) {
        Datagram* d = &net->q[net->n++];
        d->to = to;
        d->len = len;
        d->deliverAt = now + 1 + (net->jitter ? Rand() % (uint32_t) net->jitter : 0);
        memcpy(d->data, buf, (size_t) len);
    }
}

typedef struct Console {
    GdxMpLockstep ls;
    uint32_t tick;     /* next tick to run */
    uint32_t state;    /* toy simulation state */
    uint32_t padLog;   /* digest of the pad sequence fed to the game */
} Console;

static uint32_t Mix(uint32_t h, uint32_t v) {
    h ^= v;
    h *= 16777619u;
    return h;
}

static void SimStep(Console* c, const GdxMpPad* pads, int n) {
    int p;
    for (p = 0; p < n; p++) {
        c->state = Mix(c->state, pads[p].buttons);
        c->state = Mix(c->state, (uint32_t) (uint8_t) pads[p].stickX);
        c->padLog = Mix(c->padLog, (uint32_t) pads[p].buttons | ((uint32_t) (uint8_t) pads[p].stickY << 16));
    }
    c->state = Mix(c->state, c->tick);
}

static GdxMpPad LocalPad(int player, uint32_t tick) {
    GdxMpPad pad;
    pad.buttons = (uint16_t) ((tick * 2654435761u + (uint32_t) player * 40503u) >> 16);
    pad.stickX = (int8_t) ((int) ((tick + (uint32_t) player * 7u) % 161u) - 80);
    pad.stickY = (int8_t) ((int) ((tick * 3u + (uint32_t) player) % 161u) - 80);
    return pad;
}

/* Runs `players` consoles for `ticks` ticks over `net`. perturbAt (or GDX_MP_NO_TICK) flips
 * console 1's state after that tick. Returns the number of simulated time steps used. */
static uint32_t RunSession(Console* cs, int players, int delay, int hashEvery, Net* net, uint32_t ticks,
                           uint32_t perturbAt) {
    uint32_t now = 0;
    int p;
    for (p = 0; p < players; p++) {
        memset(&cs[p], 0, sizeof(cs[p]));
        CHECK(gdx_mp_ls_init(&cs[p].ls, players, p, delay, hashEvery, 0xC0FFEEu) == 0);
    }
    net->n = 0;
    while (now < ticks * 20u + 10000u) {
        int done = 1;
        int i;
        now++;
        /* Deliver what is due. */
        for (i = 0; i < net->n;) {
            if (net->q[i].deliverAt <= now) {
                gdx_mp_ls_ingest(&cs[net->q[i].to].ls, net->q[i].data, net->q[i].len, now);
                net->q[i] = net->q[--net->n];
            } else {
                i++;
            }
        }
        for (p = 0; p < players; p++) {
            Console* c = &cs[p];
            uint8_t pkt[GDX_MP_MAX_PACKET];
            int len;
            int q;
            if (c->tick >= ticks) {
                continue;
            }
            done = 0;
            /* Console speeds differ: console p only advances on some steps. */
            if ((Rand() % 4u) < (uint32_t) (p % 3)) {
                continue;
            }
            if (c->ls.known[p] <= c->tick + (uint32_t) delay) {
                gdx_mp_ls_push_local(&c->ls, c->tick, LocalPad(p, c->tick));
            }
            len = gdx_mp_ls_build_packet(&c->ls, now, pkt, sizeof(pkt));
            for (q = 0; q < players && len > 0; q++) {
                if (q != p) {
                    NetSend(net, q, pkt, len, now);
                }
            }
            if (gdx_mp_ls_ready(&c->ls, c->tick)) {
                GdxMpPad pads[GDX_MP_MAX_PLAYERS];
                gdx_mp_ls_get(&c->ls, c->tick, pads);
                SimStep(c, pads, players);
                if (c->tick == perturbAt && p == 1) {
                    c->state ^= 1u;
                }
                gdx_mp_ls_note_local_hash(&c->ls, c->tick, c->state);
                c->tick++;
            }
        }
        if (done) {
            /* Flush: let the final hashes arrive. */
            for (i = 0; i < 64; i++) {
                for (p = 0; p < players; p++) {
                    uint8_t pkt[GDX_MP_MAX_PACKET];
                    const int len = gdx_mp_ls_build_packet(&cs[p].ls, now, pkt, sizeof(pkt));
                    int q;
                    for (q = 0; q < players && len > 0; q++) {
                        if (q != p) {
                            gdx_mp_ls_ingest(&cs[q].ls, pkt, len, now);
                        }
                    }
                }
            }
            break;
        }
    }
    return now;
}

static void TestAgreement(int players, int delay, int lossPct, int jitter) {
    static Console cs[GDX_MP_MAX_PLAYERS];
    static Net net;
    int p;
    net.lossPct = lossPct;
    net.dupPct = 10;
    net.jitter = jitter;
    RunSession(cs, players, delay, 1, &net, 3000, GDX_MP_NO_TICK);
    for (p = 0; p < players; p++) {
        CHECK(cs[p].tick == 3000);
        CHECK(cs[p].state == cs[0].state);
        CHECK(cs[p].padLog == cs[0].padLog);
        CHECK(!cs[p].ls.desynced);
        CHECK(cs[p].ls.sessionRejected == 0);
    }
    for (p = 1; p < players; p++) {
        CHECK(cs[0].ls.peer[p].hashesChecked > (uint32_t) (3000 * (100 - lossPct) / 200)); /* only the newest hash rides each packet */
    }
}

static void TestDelayPrefixNeutral(void) {
    static GdxMpLockstep ls;
    GdxMpPad pads[GDX_MP_MAX_PLAYERS];
    GdxMpPad pad = { 0x8000, 10, -10 };
    CHECK(gdx_mp_ls_init(&ls, 2, 0, 3, 1, 1) == 0);
    CHECK(gdx_mp_ls_ready(&ls, 0));
    CHECK(gdx_mp_ls_ready(&ls, 2));
    CHECK(!gdx_mp_ls_ready(&ls, 3));
    gdx_mp_ls_get(&ls, 1, pads);
    CHECK(pads[0].buttons == 0 && pads[1].buttons == 0);
    gdx_mp_ls_push_local(&ls, 0, pad);
    CHECK(gdx_mp_ls_first_missing(&ls, 3) == 1);
    CHECK(ls.pads[0][3].buttons == 0x8000);
}

static void TestDesyncCaught(void) {
    static Console cs[GDX_MP_MAX_PLAYERS];
    static Net net;
    net.lossPct = 5;
    net.dupPct = 5;
    net.jitter = 3;
    RunSession(cs, 3, 2, 4, &net, 2000, 1001);
    /* Perturbed after tick 1001 on console 1; the first hash tick at/after it is 1004. */
    CHECK(cs[0].ls.desynced);
    CHECK(cs[0].ls.desyncTick == 1004);
    CHECK(cs[0].ls.desyncPeer == 1);
    CHECK(cs[1].ls.desynced);
    CHECK(cs[1].ls.desyncTick == 1004);
    CHECK(cs[2].ls.desynced && cs[2].ls.desyncTick == 1004);
}

static void TestHostile(void) {
    static GdxMpLockstep a;
    static GdxMpLockstep b;
    uint8_t pkt[GDX_MP_MAX_PACKET];
    uint8_t bad[GDX_MP_MAX_PACKET + 16];
    GdxMpPad pad = { 1, 2, 3 };
    int len;
    uint32_t knownBefore;
    uint32_t t;

    CHECK(gdx_mp_ls_init(&a, 2, 0, 2, 1, 77) == 0);
    CHECK(gdx_mp_ls_init(&b, 2, 1, 2, 1, 77) == 0);
    CHECK(gdx_mp_ls_init(&a, 9, 0, 2, 1, 77) == -1);
    CHECK(gdx_mp_ls_init(&a, 2, 2, 2, 1, 77) == -1);
    CHECK(gdx_mp_ls_init(&a, 2, 0, 0, 1, 77) == -1);
    CHECK(gdx_mp_ls_init(&a, 2, 0, GDX_MP_REDUNDANCY / 2 + 1, 1, 77) == -1);
    CHECK(gdx_mp_ls_init(&a, 2, 0, 2, 1, 77) == 0);
    for (t = 0; t < 12; t++) {
        gdx_mp_ls_push_local(&a, t, pad);
    }
    len = gdx_mp_ls_build_packet(&a, 5, pkt, sizeof(pkt));
    CHECK(len == GDX_MP_HEADER_BYTES + 4 * 12); /* nothing acked: ticks 2..13 */
    CHECK(gdx_mp_ls_build_packet(&a, 5, pkt, 10) == 0);

    knownBefore = b.known[0];
    /* Truncated. */
    CHECK(gdx_mp_ls_ingest(&b, pkt, len - 1, 6) == -1);
    CHECK(gdx_mp_ls_ingest(&b, pkt, 3, 6) == -1);
    /* Wrong magic / version / type. */
    memcpy(bad, pkt, (size_t) len);
    bad[0] = 'X';
    CHECK(gdx_mp_ls_ingest(&b, bad, len, 6) == -1);
    memcpy(bad, pkt, (size_t) len);
    bad[4] = 99;
    CHECK(gdx_mp_ls_ingest(&b, bad, len, 6) == -1);
    memcpy(bad, pkt, (size_t) len);
    bad[5] = 42;
    CHECK(gdx_mp_ls_ingest(&b, bad, len, 6) == -1);
    /* Count larger than the redundancy window, with a matching length. */
    memset(bad, 0, sizeof(bad));
    memcpy(bad, pkt, (size_t) len);
    bad[7] = GDX_MP_REDUNDANCY + 1;
    CHECK(gdx_mp_ls_ingest(&b, bad, GDX_MP_HEADER_BYTES + 4 * (GDX_MP_REDUNDANCY + 1), 6) == -1);
    /* Wrong session. */
    memcpy(bad, pkt, (size_t) len);
    bad[28] ^= 0xFF;
    CHECK(gdx_mp_ls_ingest(&b, bad, len, 6) == -1);
    CHECK(b.sessionRejected == 1);
    /* Claims to be from us, or from a player outside the session. */
    memcpy(bad, pkt, (size_t) len);
    bad[6] = 1;
    CHECK(gdx_mp_ls_ingest(&b, bad, len, 6) == -1);
    bad[6] = 5;
    CHECK(gdx_mp_ls_ingest(&b, bad, len, 6) == -1);
    CHECK(b.known[0] == knownBefore);
    /* Far-future ticks are ignored, not stored into live slots. */
    memcpy(bad, pkt, (size_t) len);
    bad[8] = 0x00;
    bad[9] = 0x00;
    bad[10] = 0x10;
    bad[11] = 0x00; /* firstTick = 0x100000 */
    CHECK(gdx_mp_ls_ingest(&b, bad, len, 6) == 0);
    CHECK(b.known[0] == knownBefore);
    CHECK(b.slotTick[0][0x100000 & (GDX_MP_RING - 1)] != 0x100000u);
    /* The genuine packet is accepted. */
    CHECK(gdx_mp_ls_ingest(&b, pkt, len, 6) == 0);
    CHECK(b.known[0] == 14);
    CHECK(b.rejected == 9);
}

/* The unacknowledged span can reach 2 * delay + 2: console A pushes its pad for lt_A + delay
 * before it waits on tick lt_A, and lt_A runs up to lt_B + delay + 1 while B still lacks A's pad
 * lt_B. One-sided burst: every A -> B datagram is lost while B waits on tick `delay` (the first
 * non-neutral one) and A runs as far as B's pads allow; then the link heals. With a window
 * shorter than 2 * delay + 2, A's packets never again carry B's missing ticks and B waits
 * forever (the hardware symptom: the 20 s "connection lost" abort). */
static void TestBurstLossStrandsNoTick(int delay) {
    GdxMpLockstep a;
    GdxMpLockstep b;
    uint8_t pkt[GDX_MP_MAX_PACKET];
    uint32_t ltA = 0;
    uint32_t ltB = 0;
    int pushedA = 0;
    int pushedB = 0;
    uint32_t now = 1000;
    int step;
    int n;
    CHECK(gdx_mp_ls_init(&a, 2, 0, delay, 1, 77u) == 0);
    CHECK(gdx_mp_ls_init(&b, 2, 1, delay, 1, 77u) == 0);
    for (step = 0; step < 4000 && (ltA < 300 || ltB < 300); step++) {
        const int burst = step < 400; /* every A -> B datagram lost */
        now += 4;
        if (!pushedA) {
            gdx_mp_ls_push_local(&a, ltA, LocalPad(0, ltA));
            pushedA = 1;
        }
        if (!pushedB) {
            gdx_mp_ls_push_local(&b, ltB, LocalPad(1, ltB));
            pushedB = 1;
        }
        n = gdx_mp_ls_build_packet(&a, now, pkt, sizeof(pkt));
        if (n > 0 && !burst) {
            gdx_mp_ls_ingest(&b, pkt, n, now);
        }
        n = gdx_mp_ls_build_packet(&b, now, pkt, sizeof(pkt));
        if (n > 0) {
            gdx_mp_ls_ingest(&a, pkt, n, now);
        }
        if (ltA < 300 && gdx_mp_ls_ready(&a, ltA)) {
            ltA++;
            pushedA = 0;
        }
        if (ltB < 300 && gdx_mp_ls_ready(&b, ltB)) {
            ltB++;
            pushedB = 0;
        }
        if (step == 399) {
            /* the burst reached the worst case: B on its first real tick, A as far ahead as it gets */
            CHECK(ltB == (uint32_t) delay);
            CHECK(ltA == (uint32_t) (2 * delay + 1));
        }
    }
    CHECK(ltA == 300 && ltB == 300);
}

/* gdx_mp_ls_unacked: news for a peer after a push, a heartbeat once the peer acked it all. */
static void TestUnacked(void) {
    GdxMpLockstep a;
    GdxMpLockstep b;
    uint8_t pkt[GDX_MP_MAX_PACKET];
    int n;
    CHECK(gdx_mp_ls_init(&a, 2, 0, 3, 1, 9u) == 0);
    CHECK(gdx_mp_ls_init(&b, 2, 1, 3, 1, 9u) == 0);
    CHECK(gdx_mp_ls_unacked(&a) == 0); /* only the implicit neutral prefix so far */
    gdx_mp_ls_push_local(&a, 0, LocalPad(0, 0));
    CHECK(gdx_mp_ls_unacked(&a) == 1);
    n = gdx_mp_ls_build_packet(&a, 10, pkt, sizeof(pkt));
    CHECK(n > 0 && gdx_mp_ls_ingest(&b, pkt, n, 10) >= 0);
    CHECK(gdx_mp_ls_unacked(&a) == 1); /* b has it, but a has not heard b's ack yet */
    gdx_mp_ls_push_local(&b, 0, LocalPad(1, 0));
    n = gdx_mp_ls_build_packet(&b, 20, pkt, sizeof(pkt));
    CHECK(n > 0 && gdx_mp_ls_ingest(&a, pkt, n, 20) >= 0);
    CHECK(gdx_mp_ls_unacked(&a) == 0);
    gdx_mp_ls_remove(&a, 1, 0);
    gdx_mp_ls_push_local(&a, 1, LocalPad(0, 1));
    CHECK(gdx_mp_ls_unacked(&a) == 0); /* a removed player acks nothing and is not waited for */
}

int main(void) {
    TestDelayPrefixNeutral();
    TestAgreement(2, 2, 0, 0);
    TestAgreement(2, 3, 30, 4);
    TestAgreement(4, 4, 20, 6);
    TestAgreement(8, 4, 15, 6);
    TestAgreement(2, 8, 60, 12); /* the max delay under brutal loss: the ack window must never strand a tick */
    TestBurstLossStrandsNoTick(2);
    TestBurstLossStrandsNoTick(4);
    TestBurstLossStrandsNoTick(8);
    TestUnacked();
    TestDesyncCaught();
    TestHostile();
    if (sFailures != 0) {
        printf("gdx_mp_lockstep_host_tests: %d of %d checks FAILED\n", sFailures, sChecks);
        return 1;
    }
    printf("=== ALL PASS (%d checks) ===\n", sChecks);
    return 0;
}
