/* port/tests/gdx_mp_txq_host_tests.c — the UDS service thread's ring and send scheduler
 * (port/gdx_mp_txq.c):
 *   - ring order, full / bounds refusals, reset;
 *   - a pthread producer and consumer moving 200000 datagrams of varying size: every one arrives,
 *     in order, byte-exact;
 *   - the INPUT classifier against real lockstep packets and look-alikes;
 *   - the scheduler: newest-wins coalescing per node, the minimum interval, wait times;
 *   - keys: a star host's relayed stream to a node is never coalesced with its own; socket
 *     destinations (packed endpoints) and relay streams; a full table refuses (send at once);
 *   - the socket thread's byte queue (port/gdx_mp_socsvc.c): records of odd sizes across many wraps,
 *     refusals, and a pthread producer / consumer moving 200000 records of 5..1480 bytes;
 *   - steady-cadence repeats: a new INPUT repeats every interval up to N times, an unchanged
 *     resend (only its clock words moved) earns none, the plain offer never repeats;
 *   - the socket service loop (port/gdx_mp_socsvc.c) against a simulated socket: in-order sends,
 *     INPUT coalescing, one empty recv per drain and none between polls, a full rx ring leaves the
 *     rest in the socket, oversize, flush, endpoint packing, the stream tag; a pthread service
 *     thread moving 20000 datagrams each way; two lockstep consoles through two service loops
 *     (Old 3DS 33 ms / 8 ms poll vs New 3DS 16 ms / 4 ms, repeats off and on) reaching tick 600 with
 *     identical pads;
 *   - two lockstep consoles whose INPUT packets go through the scheduler (coalesced, at most one per
 *     20 ms, an Old 3DS-like bursty tick pattern, 3 ms latency, 5% loss): both reach tick 600 with
 *     identical pads, with fewer sends than packets built. */
#include "gdx_mp_txq.h"
#include "gdx_mp_socsvc.h"
#include "gdx_mp_lockstep.h"

#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int gChecks = 0;
static int gFailures = 0;

#define CHECK(cond)                                                                   \
    do {                                                                              \
        gChecks++;                                                                    \
        if (!(cond)) {                                                                \
            gFailures++;                                                              \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                             \
    } while (0)

static GdxMpDgram gSlots[8];

static void TestRingBasics(void) {
    GdxMpRing r;
    uint8_t buf[GDX_MP_TXQ_PAYLOAD + 1];
    const GdxMpDgram* d;
    int i;
    memset(buf, 0xAB, sizeof(buf));
    gdx_mp_ring_init(&r, gSlots, 8);
    CHECK(gdx_mp_ring_peek(&r) == NULL);
    CHECK(gdx_mp_ring_push(&r, 1, buf, 0) == 0);
    CHECK(gdx_mp_ring_push(&r, 1, buf, GDX_MP_TXQ_PAYLOAD + 1) == 0);
    for (i = 0; i < 8; i++) {
        buf[0] = (uint8_t) i;
        CHECK(gdx_mp_ring_push(&r, (uint16_t) (i + 1), buf, 10 + i) == 1);
    }
    CHECK(gdx_mp_ring_push(&r, 9, buf, 10) == 0); /* full */
    CHECK(gdx_mp_ring_count(&r) == 8);
    for (i = 0; i < 8; i++) {
        d = gdx_mp_ring_peek(&r);
        CHECK(d != NULL && d->node == i + 1 && d->len == 10 + i && d->data[0] == (uint8_t) i);
        gdx_mp_ring_release(&r);
    }
    CHECK(gdx_mp_ring_peek(&r) == NULL && gdx_mp_ring_count(&r) == 0);
    CHECK(gdx_mp_ring_push(&r, 3, buf, GDX_MP_TXQ_PAYLOAD) == 1); /* wraps */
    gdx_mp_ring_reset(&r);
    CHECK(gdx_mp_ring_count(&r) == 0 && gdx_mp_ring_peek(&r) == NULL);
}

#define STRESS_N 200000u
static GdxMpDgram gStressSlots[16];
static GdxMpRing gStress;
static volatile int gStressBad = 0;

static int StressLen(uint32_t i) {
    return 4 + (int) ((i * 2654435761u) >> 22) % 1400;
}

static void* StressProducer(void* arg) {
    uint8_t buf[GDX_MP_TXQ_PAYLOAD];
    uint32_t i;
    (void) arg;
    for (i = 0; i < STRESS_N; i++) {
        const int len = StressLen(i);
        int k;
        memcpy(buf, &i, 4);
        for (k = 4; k < len; k++) {
            buf[k] = (uint8_t) (i + (uint32_t) k);
        }
        while (!gdx_mp_ring_push(&gStress, (uint16_t) (i & 0xFFFF), buf, len)) {
            /* full: spin */
        }
    }
    return NULL;
}

static void* StressConsumer(void* arg) {
    uint32_t i = 0;
    (void) arg;
    while (i < STRESS_N) {
        const GdxMpDgram* d = gdx_mp_ring_peek(&gStress);
        uint32_t seq;
        int k;
        if (d == NULL) {
            continue;
        }
        memcpy(&seq, d->data, 4);
        if (seq != i || d->node != (uint16_t) (i & 0xFFFF) || d->len != StressLen(i)) {
            gStressBad++;
        } else {
            for (k = 4; k < d->len; k++) {
                if (d->data[k] != (uint8_t) (i + (uint32_t) k)) {
                    gStressBad++;
                    break;
                }
            }
        }
        gdx_mp_ring_release(&gStress);
        i++;
    }
    return NULL;
}

static void TestRingThreads(void) {
    pthread_t p;
    pthread_t c;
    gdx_mp_ring_init(&gStress, gStressSlots, 16);
    CHECK(pthread_create(&c, NULL, StressConsumer, NULL) == 0);
    CHECK(pthread_create(&p, NULL, StressProducer, NULL) == 0);
    pthread_join(p, NULL);
    pthread_join(c, NULL);
    CHECK(gStressBad == 0);
    CHECK(gdx_mp_ring_count(&gStress) == 0);
}

static GdxMpPad Pad(int player, uint32_t tick) {
    GdxMpPad pad;
    pad.buttons = (uint16_t) ((tick * 2654435761u + (uint32_t) player * 40503u) >> 16);
    pad.stickX = (int8_t) ((int) ((tick + (uint32_t) player * 7u) % 161u) - 80);
    pad.stickY = (int8_t) ((int) ((tick * 3u + (uint32_t) player) % 161u) - 80);
    return pad;
}

static void TestClassifier(void) {
    GdxMpLockstep a;
    uint8_t pkt[GDX_MP_MAX_PACKET];
    uint32_t t;
    int n;
    CHECK(gdx_mp_ls_init(&a, 2, 0, 8, 1, 5u) == 0);
    for (t = 0; t < 30; t++) {
        gdx_mp_ls_push_local(&a, t, Pad(0, t));
    }
    n = gdx_mp_ls_build_packet(&a, 1, pkt, sizeof(pkt));
    CHECK(n > 0 && n <= GDX_MP_TXQ_INPUT_MAX); /* the 24-pad worst case fits a pending slot */
    CHECK(gdx_mp_txq_is_input(pkt, n) == 1);
    CHECK(gdx_mp_txq_is_input(pkt, n - 1) == 0);
    pkt[5] = (uint8_t) (GDX_MP_TYPE_INPUT + 1);
    CHECK(gdx_mp_txq_is_input(pkt, n) == 0); /* another lockstep type (drop protocol): never coalesced */
    pkt[5] = GDX_MP_TYPE_INPUT;
    pkt[0] = 'X';
    CHECK(gdx_mp_txq_is_input(pkt, n) == 0);
    CHECK(gdx_mp_txq_is_input("GXLB", 4) == 0);
}

static void TestSched(void) {
    GdxMpTxSched s;
    uint8_t out[GDX_MP_TXQ_INPUT_MAX];
    uint8_t a[GDX_MP_TXQ_INPUT_MAX];
    uint16_t len = 0;
    uint32_t wait = 0;
    memset(a, 0, sizeof(a));
    gdx_mp_txsched_init(&s, 20);
    CHECK(gdx_mp_txsched_offer_input(&s, 0, a, 10) == 0);
    CHECK(gdx_mp_txsched_offer_input(&s, 17, a, 10) == 0);
    CHECK(gdx_mp_txsched_offer_input(&s, 2, a, GDX_MP_TXQ_INPUT_MAX + 1) == 0);
    CHECK(gdx_mp_txsched_wait_ms(&s, 0) == UINT32_MAX);
    a[0] = 1;
    CHECK(gdx_mp_txsched_offer_input(&s, 2, a, 10) == 1);
    CHECK(gdx_mp_txsched_take_due(&s, 100, out, &len, &wait) == 2); /* the first goes at once */
    CHECK(len == 10 && out[0] == 1 && wait == UINT32_MAX);
    a[0] = 2;
    gdx_mp_txsched_offer_input(&s, 2, a, 11);
    a[0] = 3;
    gdx_mp_txsched_offer_input(&s, 2, a, 12); /* replaces the 2 */
    CHECK(s.coalesced == 1);
    CHECK(gdx_mp_txsched_take_due(&s, 105, out, &len, &wait) == 0 && wait == 15);
    CHECK(gdx_mp_txsched_take_due(&s, 120, out, &len, &wait) == 2 && out[0] == 3 && len == 12);
    a[0] = 4;
    gdx_mp_txsched_offer_input(&s, 3, a, 10); /* another node has its own interval */
    CHECK(gdx_mp_txsched_take_due(&s, 121, out, &len, &wait) == 3 && out[0] == 4);
    gdx_mp_txsched_offer_input(&s, 2, a, 10);
    gdx_mp_txsched_clear(&s);
    CHECK(gdx_mp_txsched_wait_ms(&s, 200) == UINT32_MAX);
    gdx_mp_txsched_init(&s, 0); /* 0 = no interval */
    gdx_mp_txsched_offer_input(&s, 2, a, 10);
    CHECK(gdx_mp_txsched_take_due(&s, 5, out, &len, NULL) == 2);
    gdx_mp_txsched_offer_input(&s, 2, a, 10);
    CHECK(gdx_mp_txsched_take_due(&s, 5, out, &len, NULL) == 2);
}

static void TestSchedStreams(void) {
    GdxMpTxSched s;
    uint8_t out[GDX_MP_TXQ_INPUT_MAX];
    uint8_t own[GDX_MP_TXQ_INPUT_MAX];
    uint8_t relayed[GDX_MP_TXQ_INPUT_MAX];
    uint16_t len = 0;
    uint64_t dest = 0;
    uint32_t wait = 0;
    int got0 = 0;
    int got1 = 0;
    int i;
    memset(own, 0, sizeof(own));
    memset(relayed, 0, sizeof(relayed));
    own[6] = 0;     /* the host's own pads (from = 0) */
    relayed[6] = 1; /* player 1's packet, relayed verbatim to node 3 */
    gdx_mp_txsched_init(&s, 33);
    CHECK(gdx_mp_txsched_offer_input(&s, 3, relayed, 20) == 1);
    CHECK(gdx_mp_txsched_offer_input(&s, 3, own, 20) == 1);
    CHECK(s.coalesced == 0); /* two streams to one node: both kept */
    CHECK(gdx_mp_txsched_pending(&s) == 2);
    for (i = 0; i < 2; i++) {
        const uint16_t node = gdx_mp_txsched_take_due(&s, 10, out, &len, &wait);
        CHECK(node == 3 && len == 20);
        got0 += out[6] == 0;
        got1 += out[6] == 1;
    }
    CHECK(got0 == 1 && got1 == 1);
    CHECK(gdx_mp_txsched_take_due(&s, 10, out, &len, &wait) == 0 && wait == UINT32_MAX);
    /* sockets: a packed endpoint per destination, a stream per (relay mask, from) */
    gdx_mp_txsched_init(&s, 16);
    CHECK(gdx_mp_txsched_offer(&s, 0x0A000002694Cull, 0, own, 20) == 0); /* stream 0: never coalesced */
    CHECK(gdx_mp_txsched_offer(&s, 0x0A000002694Cull, 1, own, 20) == 1);
    CHECK(gdx_mp_txsched_offer(&s, 0x0A000003694Cull, 1, own, 20) == 1);
    CHECK(gdx_mp_txsched_offer(&s, 0x0A000001694Cull, (0x6u << 8) | 1u, own, 30) == 1);
    CHECK(gdx_mp_txsched_offer(&s, 0x0A000001694Cull, (0x6u << 8) | 1u, own, 31) == 1);
    CHECK(s.coalesced == 1 && gdx_mp_txsched_pending(&s) == 3);
    for (i = 0; i < 3; i++) {
        CHECK(gdx_mp_txsched_take(&s, 50, &dest, out, &len, NULL) == 1);
        if (dest == 0x0A000001694Cull) {
            CHECK(len == 31);
        }
    }
    CHECK(gdx_mp_txsched_offer(&s, 0x0A000002694Cull, 1, own, 20) == 1);
    CHECK(gdx_mp_txsched_take(&s, 60, &dest, out, &len, &wait) == 0 && wait == 6);
    /* every entry pending: refused, the caller sends at once */
    gdx_mp_txsched_init(&s, 16);
    for (i = 0; i < GDX_MP_TXQ_KEYS; i++) {
        CHECK(gdx_mp_txsched_offer(&s, (uint64_t) (100 + i), 1, own, 20) == 1);
    }
    CHECK(gdx_mp_txsched_offer(&s, 999, 1, own, 20) == 0 && s.refused == 1);
    CHECK(gdx_mp_txsched_take(&s, 1, &dest, out, &len, NULL) == 1);
    CHECK(gdx_mp_txsched_offer(&s, 999, 1, own, 20) == 1); /* an idle entry is reused */
}

/* One direction of the simulated link: what the scheduler sent, delivered after LAT_MS. */
#define LINE_CAP 64
#define LAT_MS 3u
typedef struct Line {
    uint32_t at[LINE_CAP];
    uint16_t len[LINE_CAP];
    uint8_t data[LINE_CAP][GDX_MP_TXQ_INPUT_MAX];
    int count;
} Line;

static uint32_t gRng = 12345u;
static uint32_t Rand(void) {
    gRng = gRng * 1103515245u + 12345u;
    return gRng >> 8;
}

static void LineSend(Line* l, uint32_t now, const uint8_t* p, uint16_t n) {
    if (l->count >= LINE_CAP || Rand() % 100u < 5u) {
        return; /* lost */
    }
    l->at[l->count] = now + LAT_MS;
    l->len[l->count] = n;
    memcpy(l->data[l->count], p, n);
    l->count++;
}

static void LineDeliver(Line* l, uint32_t now, GdxMpLockstep* to) {
    int i = 0;
    while (i < l->count) {
        if (l->at[i] <= now) {
            gdx_mp_ls_ingest(to, l->data[i], l->len[i], now);
            l->count--;
            memmove(&l->at[i], &l->at[i + 1], sizeof(l->at[0]) * (size_t) (l->count - i));
            memmove(&l->len[i], &l->len[i + 1], sizeof(l->len[0]) * (size_t) (l->count - i));
            memmove(l->data[i], l->data[i + 1], sizeof(l->data[0]) * (size_t) (l->count - i));
        } else {
            i++;
        }
    }
}

typedef struct SimConsole {
    GdxMpLockstep ls;
    GdxMpTxSched tx;
    uint32_t lt;
    int pushed;
    uint32_t nextTickMs;
    uint32_t lastOfferMs;
    uint32_t built;
    uint32_t sent;
    int bursty; /* Old 3DS-like: ticks at +0, +22, +27 of every 50 ms slot */
    int slotPhase;
    GdxMpPad seen[600][GDX_MP_MAX_PLAYERS]; /* the pads each consumed tick ran with */
} SimConsole;

static void Offer(SimConsole* c, uint32_t now) {
    uint8_t pkt[GDX_MP_MAX_PACKET];
    const int n = gdx_mp_ls_build_packet(&c->ls, now, pkt, sizeof(pkt));
    if (n > 0) {
        CHECK(gdx_mp_txq_is_input(pkt, n));
        gdx_mp_txsched_offer_input(&c->tx, 1, pkt, n);
        c->built++;
        c->lastOfferMs = now;
    }
}

static void SimStep(SimConsole* c, uint32_t now, Line* out, uint32_t ticks) {
    uint8_t buf[GDX_MP_TXQ_INPUT_MAX];
    uint16_t len = 0;
    if (c->lt < ticks && now >= c->nextTickMs) {
        if (!c->pushed) {
            gdx_mp_ls_push_local(&c->ls, c->lt, Pad(c->ls.localPlayer, c->lt));
            c->pushed = 1;
            Offer(c, now); /* SendOurPads */
        }
        if (gdx_mp_ls_ready(&c->ls, c->lt)) {
            gdx_mp_ls_get(&c->ls, c->lt, c->seen[c->lt]);
            c->lt++;
            c->pushed = 0;
            if (c->bursty) {
                static const uint32_t kGap[3] = { 22, 5, 23 };
                c->nextTickMs = now + kGap[c->slotPhase];
                c->slotPhase = (c->slotPhase + 1) % 3;
            } else {
                c->nextTickMs = now + 16;
            }
        } else if (now - c->lastOfferMs >= 16) {
            Offer(c, now); /* WaitReady's resend */
        }
    }
    if (gdx_mp_txsched_take_due(&c->tx, now, buf, &len, NULL) != 0) {
        LineSend(out, now, buf, len);
        c->sent++;
    }
}

static void TestLockstepThroughSched(uint32_t minMs) {
    static SimConsole a;
    static SimConsole b;
    static Line ab;
    static Line ba;
    const uint32_t ticks = 600;
    uint32_t now;
    uint32_t t;
    int same = 1;
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    memset(&ab, 0, sizeof(ab));
    memset(&ba, 0, sizeof(ba));
    CHECK(gdx_mp_ls_init(&a.ls, 2, 0, 4, 1, 31u) == 0);
    CHECK(gdx_mp_ls_init(&b.ls, 2, 1, 4, 1, 31u) == 0);
    gdx_mp_txsched_init(&a.tx, minMs);
    gdx_mp_txsched_init(&b.tx, minMs);
    b.bursty = 1;
    for (now = 1; now < 200000 && (a.lt < ticks || b.lt < ticks); now++) {
        LineDeliver(&ab, now, &b.ls);
        LineDeliver(&ba, now, &a.ls);
        SimStep(&a, now, &ab, ticks);
        SimStep(&b, now, &ba, ticks);
    }
    CHECK(a.lt == ticks && b.lt == ticks);
    for (t = 0; t < ticks; t++) {
        if (memcmp(a.seen[t], b.seen[t], sizeof(GdxMpPad) * 2) != 0) {
            if (same) {
                fprintf(stderr, "tick %u differs\n", (unsigned) t);
            }
            same = 0;
        }
        if (t >= 4) {
            GdxMpPad want = Pad(1, t - 4);
            gdx_mp_ls_clamp_pad(&want);
            if (memcmp(&a.seen[t][1], &want, sizeof(GdxMpPad)) != 0) {
                if (same) {
                    fprintf(stderr, "tick %u: B's pad on A is not the one B pushed\n", (unsigned) t);
                }
                same = 0; /* the remote pad is the one pushed delay ticks earlier */
            }
        }
    }
    CHECK(same);
    if (minMs > 0) {
        CHECK(b.sent < b.built && b.tx.coalesced > 0);
    }
    printf("txq lockstep minMs=%u: %u ms for %u ticks; A sent %u of %u, B (bursty) sent %u of %u\n",
           (unsigned) minMs, (unsigned) now, (unsigned) ticks, (unsigned) a.sent, (unsigned) a.built,
           (unsigned) b.sent, (unsigned) b.built);
}

/* ---- the socket service loop (gdx_mp_socsvc) ------------------------------------------------- */
#define FAKE_CAP 512
typedef struct FakeSock {
    /* inbound: datagrams waiting in the socket */
    uint8_t in[FAKE_CAP][64];
    int inLen[FAKE_CAP];
    uint64_t inSrc[FAKE_CAP];
    int inHead, inCount;
    /* outbound: what the loop sent */
    uint8_t out[FAKE_CAP][GDX_MP_TXQ_INPUT_MAX];
    int outLen[FAKE_CAP];
    uint64_t outDest[FAKE_CAP];
    int outCount;
    int recvCalls, sendCalls;
} FakeSock;

static int FakeSend(void* ctx, uint64_t dest, const void* buf, int len) {
    FakeSock* f = (FakeSock*) ctx;
    f->sendCalls++;
    if (f->outCount < FAKE_CAP) {
        memcpy(f->out[f->outCount], buf, (size_t) (len < GDX_MP_TXQ_INPUT_MAX ? len : GDX_MP_TXQ_INPUT_MAX));
        f->outLen[f->outCount] = len;
        f->outDest[f->outCount] = dest;
        f->outCount++;
    }
    return len;
}

static int FakeRecv(void* ctx, void* buf, int cap, uint64_t* src) {
    FakeSock* f = (FakeSock*) ctx;
    int n;
    f->recvCalls++;
    if (f->inCount == 0) {
        return 0;
    }
    n = f->inLen[f->inHead];
    if (n > cap) {
        n = cap;
    }
    memcpy(buf, f->in[f->inHead], (size_t) n);
    *src = f->inSrc[f->inHead];
    f->inHead = (f->inHead + 1) % FAKE_CAP;
    f->inCount--;
    return n;
}

static void FakeInject(FakeSock* f, uint64_t src, uint8_t tag, int len) {
    const int i = (f->inHead + f->inCount) % FAKE_CAP;
    memset(f->in[i], tag, (size_t) len);
    f->inLen[i] = len;
    f->inSrc[i] = src;
    f->inCount++;
}

static uint64_t gSvcTx[512 / 8];
static uint64_t gSvcRx[4096 / 8];
static GdxMpSocSvc gSvc;
static FakeSock gFake;

static void TestSocSvcBasics(void) {
    GdxMpSocSvcOps ops;
    GdxMpLockstep ls;
    uint8_t pkt[GDX_MP_MAX_PACKET];
    uint8_t buf[GDX_MP_TXQ_PAYLOAD];
    uint64_t src = 0;
    uint8_t ip[4] = { 192, 168, 2, 196 };
    uint8_t ip2[4] = { 0, 0, 0, 0 };
    uint16_t port = 0;
    uint32_t wait;
    uint32_t t;
    int n;
    int i;
    memset(&gFake, 0, sizeof(gFake));
    ops.send = FakeSend;
    ops.recv = FakeRecv;
    ops.ctx = &gFake;
    gdx_mp_socsvc_unpack(gdx_mp_socsvc_pack(ip, 27300), ip2, &port);
    CHECK(memcmp(ip, ip2, 4) == 0 && port == 27300);
    gdx_mp_socsvc_init(&gSvc, gSvcTx, sizeof(gSvcTx), gSvcRx, sizeof(gSvcRx), 33, 8);
    /* stream 0: out at once, in order */
    for (i = 0; i < 3; i++) {
        memset(buf, 'a' + i, 10);
        CHECK(gdx_mp_socsvc_queue(&gSvc, 100 + (uint64_t) i, 0, buf, 10) == 1);
    }
    wait = gdx_mp_socsvc_step(&gSvc, &ops, 1000);
    CHECK(gFake.outCount == 3 && gFake.out[0][0] == 'a' && gFake.out[2][0] == 'c' && gFake.outDest[1] == 101);
    CHECK(gFake.recvCalls == 1 && gSvc.st.rxEmpty == 1); /* the first step drains once */
    CHECK(wait == 8);
    /* no recv until the poll is due */
    gdx_mp_socsvc_step(&gSvc, &ops, 1004);
    CHECK(gFake.recvCalls == 1);
    gdx_mp_socsvc_step(&gSvc, &ops, 1008);
    CHECK(gFake.recvCalls == 2);
    /* INPUT: the newest of a stream wins, then the interval */
    CHECK(gdx_mp_ls_init(&ls, 2, 1, 4, 1, 7u) == 0);
    gFake.outCount = 0;
    for (t = 0; t < 3; t++) {
        gdx_mp_ls_push_local(&ls, t, Pad(1, t));
        n = gdx_mp_ls_build_packet(&ls, 1010 + t, pkt, sizeof(pkt));
        CHECK(gdx_mp_socsvc_stream(pkt, n, 0) == 2u);
        CHECK(gdx_mp_socsvc_stream(pkt, n, 0x6) == (2u | (0x6u << 8)));
        CHECK(gdx_mp_socsvc_queue(&gSvc, 200, gdx_mp_socsvc_stream(pkt, n, 0), pkt, n) == 1);
    }
    CHECK(gdx_mp_socsvc_stream("GXLB", 4, 0) == 0u);
    gdx_mp_socsvc_step(&gSvc, &ops, 1012);
    CHECK(gFake.outCount == 1 && gFake.outLen[0] == n && memcmp(gFake.out[0], pkt, (size_t) n) == 0);
    CHECK(gSvc.sched.coalesced == 2);
    gdx_mp_ls_push_local(&ls, 3, Pad(1, 3));
    n = gdx_mp_ls_build_packet(&ls, 1020, pkt, sizeof(pkt));
    gdx_mp_socsvc_queue(&gSvc, 200, 2, pkt, n);
    wait = gdx_mp_socsvc_step(&gSvc, &ops, 1020);
    CHECK(gFake.outCount == 1 && wait <= 25 && wait >= 1); /* held: 33 ms after 1012 */
    gdx_mp_socsvc_step(&gSvc, &ops, 1045);
    CHECK(gFake.outCount == 2);
    /* a full rx queue stops the drain; the rest stays in the socket. Room is checked for the
     * largest datagram (twice: a wrap marker can cost as much): 4096 - 2 * 1504 = 1088 B, so
     * 48-byte records go in while <= 1088 B are used: 23 of them. */
    for (i = 0; i < 30; i++) {
        FakeInject(&gFake, 300, (uint8_t) i, 20);
    }
    gFake.recvCalls = 0;
    gdx_mp_socsvc_step(&gSvc, &ops, 1100);
    CHECK(gdx_mp_byteq_record_bytes(20) == 48);
    CHECK(gFake.recvCalls == 23 && gSvc.st.rxFullStops == 1 && gFake.inCount == 7);
    CHECK(gdx_mp_byteq_count(&gSvc.rx) == 23);
    for (i = 0; i < 23; i++) {
        CHECK(gdx_mp_socsvc_take(&gSvc, buf, sizeof(buf), &src) == 20 && buf[0] == (uint8_t) i && src == 300);
    }
    CHECK(gdx_mp_socsvc_take(&gSvc, buf, sizeof(buf), &src) == 0);
    gdx_mp_socsvc_step(&gSvc, &ops, 1110); /* the other 7, then empty */
    for (i = 23; i < 30; i++) {
        CHECK(gdx_mp_socsvc_take(&gSvc, buf, sizeof(buf), &src) == 20 && buf[0] == (uint8_t) i);
    }
    CHECK(gFake.inCount == 0);
    /* oversize: dropped */
    FakeInject(&gFake, 300, 9, 30);
    gdx_mp_socsvc_step(&gSvc, &ops, 1140);
    CHECK(gdx_mp_socsvc_take(&gSvc, buf, 10, &src) == -1);
    CHECK(gdx_mp_socsvc_take(&gSvc, buf, sizeof(buf), &src) == 0);
    /* flush: a held INPUT goes now */
    gFake.outCount = 0;
    gdx_mp_socsvc_queue(&gSvc, 200, 2, pkt, n);
    gdx_mp_socsvc_step(&gSvc, &ops, 1150);
    gdx_mp_socsvc_queue(&gSvc, 200, 2, pkt, n);
    gdx_mp_socsvc_step(&gSvc, &ops, 1151);
    CHECK(gFake.outCount == 1);
    gdx_mp_socsvc_flush(&gSvc, &ops, 1152);
    CHECK(gFake.outCount == 2 && gdx_mp_txsched_pending(&gSvc.sched) == 0);
    /* a full tx queue refuses: 512 B holds 16 records of 32 B (15 when a wrap marker eats one) */
    n = 0;
    while (n < 32 && gdx_mp_socsvc_queue(&gSvc, 1, 0, buf, 10) == 1) {
        n++;
    }
    CHECK(n >= 15 && n <= 16 && gSvc.st.txDrops == 1);
    CHECK(gdx_mp_socsvc_queue(&gSvc, 1, 0, buf, 300) == 0); /* > half the queue: never fits */
}

/* The socket thread's byte queue: variable-length records, wrap markers, refusals. */
static uint64_t gBqBuf[1024 / 8];

static void TestByteQBasics(void) {
    GdxMpByteQ q;
    uint8_t buf[GDX_MP_TXQ_PAYLOAD];
    const GdxMpQRec* r;
    int round;
    int i;
    gdx_mp_byteq_init(&q, gBqBuf, sizeof(gBqBuf));
    CHECK(gdx_mp_byteq_peek(&q) == NULL && gdx_mp_byteq_count(&q) == 0);
    CHECK(gdx_mp_byteq_record_bytes(1) == 32 && gdx_mp_byteq_record_bytes(16) == 32 &&
          gdx_mp_byteq_record_bytes(17) == 48);
    CHECK(gdx_mp_byteq_push(&q, 1, 0, buf, 0) == 0);
    CHECK(gdx_mp_byteq_push(&q, 1, 0, buf, GDX_MP_TXQ_PAYLOAD + 1) == 0);
    CHECK(gdx_mp_byteq_push(&q, 1, 0, buf, 497) == 0); /* 528 B > half of 1024 */
    CHECK(gdx_mp_byteq_push(&q, 1, 0, buf, 496) == 1); /* 512 B: exactly half */
    gdx_mp_byteq_release(&q);
    /* many rounds of odd sizes: every record comes back whole, in order, across wraps */
    for (round = 0; round < 2000; round++) {
        const int len = 1 + (round * 37) % 300;
        int pushed = 0;
        for (i = 0; i < 3; i++) {
            memset(buf, (round + i) & 0xFF, (size_t) len);
            pushed += gdx_mp_byteq_push(&q, (uint64_t) round, (uint32_t) i, buf, len);
        }
        CHECK(pushed >= 2 && gdx_mp_byteq_count(&q) == (uint32_t) pushed);
        for (i = 0; i < pushed; i++) {
            r = gdx_mp_byteq_peek(&q);
            CHECK(r != NULL && r->len == len && r->dest == (uint64_t) round && r->stream == (uint32_t) i &&
                  r->data[0] == (uint8_t) ((round + i) & 0xFF) && r->data[len - 1] == (uint8_t) ((round + i) & 0xFF));
            gdx_mp_byteq_release(&q);
        }
        CHECK(gdx_mp_byteq_peek(&q) == NULL);
    }
    gdx_mp_byteq_release(&q); /* on empty: nothing */
    CHECK(gdx_mp_byteq_count(&q) == 0);
}

#define BQ_N 200000
static uint64_t gBqThBuf[8192 / 8];
static GdxMpByteQ gBqTh;
static volatile int gBqBad = 0;

static int BqLen(uint32_t seq) {
    return 5 + (int) ((seq * 2654435761u) % (uint32_t) (GDX_MP_TXQ_PAYLOAD - 5));
}

static void* BqProducer(void* arg) {
    uint8_t buf[GDX_MP_TXQ_PAYLOAD];
    uint32_t seq = 0;
    (void) arg;
    while (seq < BQ_N) {
        const int len = BqLen(seq);
        memset(buf, (int) (seq & 0xFF), (size_t) len);
        memcpy(buf, &seq, 4);
        if (gdx_mp_byteq_push(&gBqTh, seq, seq ^ 0x55u, buf, len)) {
            seq++;
        } else {
            sched_yield();
        }
    }
    return NULL;
}

static void TestByteQThreads(void) {
    pthread_t th;
    uint32_t want = 0;
    long spins = 0;
    gdx_mp_byteq_init(&gBqTh, gBqThBuf, sizeof(gBqThBuf));
    pthread_create(&th, NULL, BqProducer, NULL);
    while (want < BQ_N && spins < 2000000000L) {
        const GdxMpQRec* r = gdx_mp_byteq_peek(&gBqTh);
        uint32_t seq;
        spins++;
        if (r == NULL) {
            continue;
        }
        memcpy(&seq, r->data, 4);
        if (seq != want || r->len != BqLen(seq) || r->dest != seq || r->stream != (seq ^ 0x55u) ||
            r->data[r->len - 1] != (uint8_t) (seq & 0xFF)) {
            gBqBad++;
        }
        gdx_mp_byteq_release(&gBqTh);
        want++;
    }
    pthread_join(th, NULL);
    CHECK(want == BQ_N);
    CHECK(gBqBad == 0);
    CHECK(gdx_mp_byteq_count(&gBqTh) == 0 && gdx_mp_byteq_peek(&gBqTh) == NULL);
}

/* Steady cadence: a new INPUT repeats every interval up to N times; an unchanged resend earns none. */
static void TestSchedRepeats(void) {
    GdxMpTxSched sch;
    GdxMpLockstep ls;
    uint8_t pkt[GDX_MP_MAX_PACKET];
    uint8_t out[GDX_MP_TXQ_INPUT_MAX];
    uint16_t len = 0;
    uint64_t dest = 0;
    uint32_t wait = 0;
    int n;
    int n2;
    gdx_mp_txsched_init(&sch, 33);
    CHECK(gdx_mp_ls_init(&ls, 2, 0, 4, 1, 7u) == 0);
    gdx_mp_ls_push_local(&ls, 0, Pad(0, 0));
    n = gdx_mp_ls_build_packet(&ls, 100, pkt, sizeof(pkt));
    CHECK(gdx_mp_txsched_offer_repeat(&sch, 5, 1, pkt, n, 2) == 1);
    CHECK(gdx_mp_txsched_take(&sch, 100, &dest, out, &len, &wait) == 1 && dest == 5);
    CHECK(gdx_mp_txsched_rearm(&sch, 120, 33, &wait) == 0 && wait == 13);
    CHECK(gdx_mp_txsched_rearm(&sch, 133, 33, &wait) == 1 && wait == UINT32_MAX);
    CHECK(gdx_mp_txsched_take(&sch, 133, &dest, out, &len, &wait) == 1 && memcmp(out, pkt, (size_t) n) == 0);
    CHECK(gdx_mp_txsched_rearm(&sch, 166, 33, NULL) == 1);
    CHECK(gdx_mp_txsched_take(&sch, 166, &dest, out, &len, &wait) == 1);
    CHECK(gdx_mp_txsched_rearm(&sch, 300, 33, &wait) == 0 && wait == UINT32_MAX); /* budget spent */
    CHECK(sch.repeats == 2);
    /* the same pads again, only the clock moved (a resend in a stall): no repeats */
    n2 = gdx_mp_ls_build_packet(&ls, 400, pkt, sizeof(pkt));
    CHECK(n2 == n);
    CHECK(gdx_mp_txsched_offer_repeat(&sch, 5, 1, pkt, n2, 2) == 1);
    CHECK(gdx_mp_txsched_take(&sch, 400, &dest, out, &len, &wait) == 1);
    CHECK(gdx_mp_txsched_rearm(&sch, 500, 33, NULL) == 0);
    /* a new pad: repeats again */
    gdx_mp_ls_push_local(&ls, 1, Pad(0, 1));
    n = gdx_mp_ls_build_packet(&ls, 600, pkt, sizeof(pkt));
    CHECK(gdx_mp_txsched_offer_repeat(&sch, 5, 1, pkt, n, 2) == 1);
    CHECK(gdx_mp_txsched_take(&sch, 600, &dest, out, &len, &wait) == 1);
    CHECK(gdx_mp_txsched_rearm(&sch, 640, 33, NULL) == 1 && sch.repeats == 3);
    /* the plain offer never repeats */
    CHECK(gdx_mp_txsched_take(&sch, 640, &dest, out, &len, &wait) == 1);
    gdx_mp_ls_push_local(&ls, 2, Pad(0, 2));
    n = gdx_mp_ls_build_packet(&ls, 700, pkt, sizeof(pkt));
    CHECK(gdx_mp_txsched_offer(&sch, 5, 1, pkt, n) == 1);
    CHECK(gdx_mp_txsched_take(&sch, 700, &dest, out, &len, &wait) == 1);
    CHECK(gdx_mp_txsched_rearm(&sch, 800, 33, NULL) == 0);
}

/* A service thread against a socket shared under a mutex: 20000 datagrams each way, in order. */
#define TH_N 20000
static uint64_t gThTx[4096 / 8];
static uint64_t gThRx[4096 / 8];
static GdxMpSocSvc gTh;
static pthread_mutex_t gThLock = PTHREAD_MUTEX_INITIALIZER;
static volatile int gThQuit = 0;
static uint32_t gThNextOut = 0;  /* the sequence number the socket expects next */
static uint32_t gThNextIn = 0;   /* the next one the socket delivers */
static int gThBad = 0;

static int ThSend(void* ctx, uint64_t dest, const void* buf, int len) {
    uint32_t seq;
    (void) ctx;
    memcpy(&seq, buf, 4);
    if (seq != gThNextOut || dest != 7u || len != 4 + (int) (seq % 300u)) {
        gThBad++;
    }
    gThNextOut = seq + 1;
    return len;
}

static int ThRecv(void* ctx, void* buf, int cap, uint64_t* src) {
    static uint32_t polls = 0;
    int n = 0;
    (void) ctx;
    pthread_mutex_lock(&gThLock);
    polls++;
    if (gThNextIn < TH_N && (polls % 7u) != 3u) { /* every 7th call finds nothing */
        n = 4 + (int) (gThNextIn % 200u);
        if (n <= cap) {
            memset(buf, (int) (gThNextIn & 0xFF), (size_t) n);
            memcpy(buf, (const void*) &gThNextIn, 4);
            *src = 9u;
        }
        gThNextIn++;
    }
    pthread_mutex_unlock(&gThLock);
    return n;
}

static void* ThMain(void* arg) {
    static const GdxMpSocSvcOps ops = { ThSend, ThRecv, NULL };
    uint32_t now = 0;
    int skip = 0;
    (void) arg;
    while (!__atomic_load_n(&gThQuit, __ATOMIC_ACQUIRE)) {
        gdx_mp_socsvc_step(&gTh, &ops, now);
        now += 1;
        if (++skip % 64 == 0) {
            sched_yield();
        }
    }
    gdx_mp_socsvc_flush(&gTh, &ops, now);
    return NULL;
}

static void TestSocSvcThread(void) {
    pthread_t th;
    uint8_t buf[GDX_MP_TXQ_PAYLOAD];
    uint32_t sent = 0;
    uint32_t got = 0;
    int spins = 0;
    gdx_mp_socsvc_init(&gTh, gThTx, sizeof(gThTx), gThRx, sizeof(gThRx), 0, 1);
    pthread_create(&th, NULL, ThMain, NULL);
    while ((sent < TH_N || got < TH_N) && spins < 200000000) {
        uint64_t src = 0;
        int n;
        spins++;
        if (sent < TH_N) {
            memset(buf, 0, sizeof(buf));
            memcpy(buf, &sent, 4);
            if (gdx_mp_socsvc_queue(&gTh, 7u, 0, buf, 4 + (int) (sent % 300u))) {
                sent++;
            }
        }
        n = gdx_mp_socsvc_take(&gTh, buf, sizeof(buf), &src);
        if (n > 0) {
            uint32_t seq;
            memcpy(&seq, buf, 4);
            if (seq != got || n != 4 + (int) (seq % 200u) || src != 9u) {
                gThBad++;
            }
            got++;
        }
    }
    __atomic_store_n(&gThQuit, 1, __ATOMIC_RELEASE);
    pthread_join(th, NULL);
    CHECK(sent == TH_N && got == TH_N);
    CHECK(gThNextOut == TH_N);
    CHECK(gThBad == 0);
}

/* Two lockstep consoles, each behind a service loop with its own interval and poll (simulated
 * time; the loop runs when its sleep ends or a datagram is queued, as the event wakes it). */
typedef struct NetLine {
    uint32_t at[256];
    int len[256];
    uint8_t data[256][GDX_MP_TXQ_INPUT_MAX];
    int count;
} NetLine;

typedef struct SvcConsole {
    GdxMpLockstep ls;
    GdxMpSocSvc svc;
    uint64_t txBuf[4096 / 8];
    uint64_t rxBuf[8192 / 8];
    NetLine* outLine;
    NetLine* inLine; /* datagrams on their way to this console's socket */
    uint32_t wakeAt;
    uint32_t lt;
    int pushed;
    uint32_t nextTickMs;
    uint32_t lastOfferMs;
    uint32_t built;
    uint32_t sent;
    int bursty;
    int slotPhase;
    GdxMpPad seen[600][GDX_MP_MAX_PLAYERS];
} SvcConsole;

static uint32_t gSimNow = 0;

static int LineSockSend(void* ctx, uint64_t dest, const void* buf, int len) {
    SvcConsole* c = (SvcConsole*) ctx;
    NetLine* l = c->outLine;
    (void) dest;
    c->sent++;
    if (l->count >= 256 || Rand() % 100u < 5u || len > GDX_MP_TXQ_INPUT_MAX) {
        return len; /* lost */
    }
    l->at[l->count] = gSimNow + 3u;
    l->len[l->count] = len;
    memcpy(l->data[l->count], buf, (size_t) len);
    l->count++;
    return len;
}

static int LineSockRecv(void* ctx, void* buf, int cap, uint64_t* src) {
    SvcConsole* c = (SvcConsole*) ctx;
    NetLine* l = c->inLine;
    int n;
    if (l->count == 0 || l->at[0] > gSimNow) {
        return 0;
    }
    n = l->len[0] <= cap ? l->len[0] : cap;
    memcpy(buf, l->data[0], (size_t) n);
    *src = 1u;
    l->count--;
    memmove(&l->at[0], &l->at[1], sizeof(l->at[0]) * (size_t) l->count);
    memmove(&l->len[0], &l->len[1], sizeof(l->len[0]) * (size_t) l->count);
    memmove(l->data[0], l->data[1], sizeof(l->data[0]) * (size_t) l->count);
    return n;
}

static void SvcOffer(SvcConsole* c, uint32_t now) {
    uint8_t pkt[GDX_MP_MAX_PACKET];
    const int n = gdx_mp_ls_build_packet(&c->ls, now, pkt, sizeof(pkt));
    if (n > 0 && gdx_mp_socsvc_queue(&c->svc, 1u, gdx_mp_socsvc_stream(pkt, n, 0), pkt, n)) {
        c->built++;
        c->lastOfferMs = now;
        c->wakeAt = now; /* the event */
    }
}

static void SvcConsoleStep(SvcConsole* c, uint32_t now, uint32_t ticks) {
    GdxMpSocSvcOps ops;
    uint8_t buf[GDX_MP_TXQ_PAYLOAD];
    uint64_t src = 0;
    int n;
    while ((n = gdx_mp_socsvc_take(&c->svc, buf, sizeof(buf), &src)) > 0) {
        gdx_mp_ls_ingest(&c->ls, buf, n, now);
    }
    if (c->lt < ticks && now >= c->nextTickMs) {
        if (!c->pushed) {
            gdx_mp_ls_push_local(&c->ls, c->lt, Pad(c->ls.localPlayer, c->lt));
            c->pushed = 1;
            SvcOffer(c, now);
        }
        if (gdx_mp_ls_ready(&c->ls, c->lt)) {
            gdx_mp_ls_get(&c->ls, c->lt, c->seen[c->lt]);
            c->lt++;
            c->pushed = 0;
            if (c->bursty) {
                static const uint32_t kGap[3] = { 22, 5, 23 };
                c->nextTickMs = now + kGap[c->slotPhase];
                c->slotPhase = (c->slotPhase + 1) % 3;
            } else {
                c->nextTickMs = now + 16;
            }
        } else if (now - c->lastOfferMs >= 16) {
            SvcOffer(c, now);
        }
    }
    if (now >= c->wakeAt) {
        ops.send = LineSockSend;
        ops.recv = LineSockRecv;
        ops.ctx = c;
        c->wakeAt = now + gdx_mp_socsvc_step(&c->svc, &ops, now);
    }
}

static void TestLockstepThroughSocSvc(uint32_t repeats) {
    static SvcConsole a;
    static SvcConsole b;
    static NetLine ab;
    static NetLine ba;
    const uint32_t ticks = 600;
    uint32_t t;
    int same = 1;
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    memset(&ab, 0, sizeof(ab));
    memset(&ba, 0, sizeof(ba));
    CHECK(gdx_mp_ls_init(&a.ls, 2, 0, 4, 1, 31u) == 0);
    CHECK(gdx_mp_ls_init(&b.ls, 2, 1, 4, 1, 31u) == 0);
    gdx_mp_socsvc_init(&a.svc, a.txBuf, sizeof(a.txBuf), a.rxBuf, sizeof(a.rxBuf), 16, 4); /* New 3DS defaults */
    gdx_mp_socsvc_init(&b.svc, b.txBuf, sizeof(b.txBuf), b.rxBuf, sizeof(b.rxBuf), 33, 8); /* Old 3DS defaults */
    a.outLine = &ab;
    a.inLine = &ba;
    b.outLine = &ba;
    b.inLine = &ab;
    b.bursty = 1;
    a.svc.txRepeats = repeats;
    b.svc.txRepeats = repeats;
    for (gSimNow = 1; gSimNow < 200000 && (a.lt < ticks || b.lt < ticks); gSimNow++) {
        SvcConsoleStep(&a, gSimNow, ticks);
        SvcConsoleStep(&b, gSimNow, ticks);
    }
    CHECK(a.lt == ticks && b.lt == ticks);
    for (t = 0; t < ticks; t++) {
        if (memcmp(a.seen[t], b.seen[t], sizeof(GdxMpPad) * 2) != 0) {
            same = 0;
        }
    }
    CHECK(same);
    CHECK(b.svc.sched.coalesced > 0);
    CHECK(repeats > 0 ? a.svc.sched.repeats + b.svc.sched.repeats > 0
                      : (b.sent < b.built && a.svc.sched.repeats + b.svc.sched.repeats == 0));
    printf("socsvc lockstep (repeats %u): %u ms for %u ticks; A (16/4) sent %u of %u, B (33/8, bursty) sent %u "
           "of %u (repeats A=%u B=%u); recv calls A=%u B=%u\n",
           (unsigned) repeats, (unsigned) gSimNow, (unsigned) ticks, (unsigned) a.sent, (unsigned) a.built, (unsigned) b.sent,
           (unsigned) b.built, (unsigned) a.svc.sched.repeats, (unsigned) b.svc.sched.repeats,
           (unsigned) (a.svc.st.rx + a.svc.st.rxEmpty),
           (unsigned) (b.svc.st.rx + b.svc.st.rxEmpty));
}

int main(void) {
    TestRingBasics();
    TestRingThreads();
    TestClassifier();
    TestSched();
    TestSchedStreams();
    TestLockstepThroughSched(0);
    TestLockstepThroughSched(20);
    TestLockstepThroughSched(33);
    TestLockstepThroughSched(40);
    TestLockstepThroughSched(50);
    TestByteQBasics();
    TestByteQThreads();
    TestSchedRepeats();
    TestSocSvcBasics();
    TestSocSvcThread();
    TestLockstepThroughSocSvc(0);
    TestLockstepThroughSocSvc(2);
    if (gFailures != 0) {
        printf("=== %d FAILURES (%d checks) ===\n", gFailures, gChecks);
        return 1;
    }
    printf("=== ALL PASS (%d checks) ===\n", gChecks);
    return 0;
}
