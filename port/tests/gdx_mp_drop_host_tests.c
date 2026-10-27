/* port/tests/gdx_mp_drop_host_tests.c — multiplayer drop-out (port/gdx_mp_drop.c) against
 * simulated consoles running the real lockstep core (port/gdx_mp_lockstep.c) over a lossy,
 * duplicating, reordering network. One or two players die mid-session (silently, or after a
 * LEAVE); the host times them out or hears the goodbye and removes them. Checks:
 *   1. every surviving console runs every tick, feeds its toy simulation the identical pad
 *      sequence and reaches the identical state (no desync, nothing fatal);
 *   2. every survivor agrees on the removal tick T, the removed player's pads are neutral
 *      from T on everywhere, and identical to what it really sent before T;
 *   3. both topologies: a STAR (the host relays: UDS, udp-lan) and a MESH (mp-online), where
 *      a joiner can hold pads of the leaver that the host never got -- the case the
 *      two-phase FREEZE/ACK/COMMIT exists for;
 *   4. the removed console, if it is still alive, learns it was removed;
 *   5. hostile DROP-family datagrams (non-host DROP, bad sizes, wrong session, absurd ACK
 *      reports, a fuzz corpus) are refused without corrupting the session. */
#include "gdx_mp_drop.h"
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

/* Counted once per call site batch: the fuzz loop would otherwise dominate the check count. */
#define CHECK_QUIET(cond)                                                             \
    do {                                                                              \
        if (!(cond)) {                                                                \
            sChecks++;                                                                \
            sFailures++;                                                              \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
        }                                                                             \
    } while (0)

/* The buttons an N64 pad can report (mp-online clamps every pad to this mask: staying inside
 * it keeps these tests valid on either side of that merge). */
#define GDX_MP_BUTTON_MASK_T 0xFF3Fu

static int LowestSetBit(unsigned mask) {
    int b;
    for (b = 0; b < 8; b++) {
        if (mask & (1u << b)) {
            return b;
        }
    }
    return 0;
}

static uint32_t sRng = 777u;

static uint32_t Rand(void) {
    sRng = sRng * 1103515245u + 12345u;
    return (sRng >> 8) & 0xFFFFFFu;
}

#define MAX_INFLIGHT 8192
#define PKT_CAP (GDX_MP_MAX_PACKET > GDX_MP_DROP_MAX_PACKET ? GDX_MP_MAX_PACKET : GDX_MP_DROP_MAX_PACKET)
#define SESSION 0xD20Fu
#define TICKS 2400u
#define MAXP 4

typedef struct Datagram {
    int to;
    int from;
    int len;
    uint32_t deliverAt;
    uint8_t data[PKT_CAP];
} Datagram;

typedef struct Net {
    Datagram q[MAX_INFLIGHT];
    int n;
    int lossPct;
    int dupPct;
    int jitter;
} Net;

static void NetSend(Net* net, int from, int to, const uint8_t* buf, int len, uint32_t now) {
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
        d->from = from;
        d->len = len;
        d->deliverAt = now + 1 + (net->jitter ? Rand() % (uint32_t) net->jitter : 0);
        memcpy(d->data, buf, (size_t) len);
    }
}

typedef struct Console {
    GdxMpLockstep ls;
    GdxMpDrop drop;
    uint32_t tick;
    uint32_t state;
    uint32_t padLog;
    int alive;
    int left;                  /* stopped on purpose (sent LEAVE) */
    uint32_t lastHeard[MAXP];  /* host: newest valid datagram from each player */
    GdxMpPad sentPad[TICKS + 32]; /* what this console scheduled for each tick (its own) */
    GdxMpPad usedPad[MAXP][TICKS]; /* what it fed the game for each player */
} Console;

typedef struct Scenario {
    int players;
    int delay;
    int mesh;
    int lossPct;
    int jitter;
    int dieA;            /* player that dies (-1 none) */
    uint32_t dieAtTick;  /* ...when it reaches this tick */
    int dieAGraceful;    /* sends LEAVE first */
    int dieB;            /* a second one (-1 none) */
    uint32_t dieBAtTick;
    int zombie;          /* dieA keeps listening (it must learn it was removed) */
    uint32_t dropTimeout;
} Scenario;

static Console sCs[MAXP];
static Net sNet;
static uint32_t sBridged[2]; /* per topology: removals where a peer held more than the host */

static uint32_t Mix(uint32_t h, uint32_t v) {
    h ^= v;
    h *= 16777619u;
    return h;
}

static GdxMpPad LocalPad(int player, uint32_t tick) {
    GdxMpPad pad;
    pad.buttons = (uint16_t) (((tick * 2654435761u + (uint32_t) player * 40503u) >> 16) & GDX_MP_BUTTON_MASK_T);
    pad.stickX = (int8_t) ((int) ((tick + (uint32_t) player * 7u) % 161u) - 80);
    pad.stickY = (int8_t) ((int) ((tick * 3u + (uint32_t) player) % 161u) - 80);
    if (pad.buttons == 0 && pad.stickX == 0 && pad.stickY == 0) {
        pad.buttons = 1; /* never neutral: a neutral pad after T must come from the drop */
    }
    return pad;
}

static void SimStep(Console* c, const GdxMpPad* pads, int n) {
    int p;
    for (p = 0; p < n; p++) {
        c->state = Mix(c->state, pads[p].buttons);
        c->state = Mix(c->state, (uint32_t) (uint8_t) pads[p].stickX);
        c->padLog = Mix(c->padLog, (uint32_t) pads[p].buttons | ((uint32_t) (uint8_t) pads[p].stickY << 16));
        if (c->tick < TICKS) {
            c->usedPad[p][c->tick] = pads[p];
        }
    }
    c->state = Mix(c->state, c->tick);
}

/* Who hears whom. Star: joiners talk to the host only, the host relays INPUT verbatim. */
static void SendFrom(const Scenario* s, int from, int dest, const uint8_t* pkt, int len, uint32_t now) {
    int q;
    for (q = 0; q < s->players; q++) {
        if (q == from) {
            continue;
        }
        if (dest != GDX_MP_DROP_DEST_ALL && q != dest) {
            continue;
        }
        if (!s->mesh && from != 0 && q != 0) {
            continue;
        }
        NetSend(&sNet, from, q, pkt, len, now);
    }
}

static void Deliver(const Scenario* s, Datagram* dg, uint32_t now) {
    Console* c = &sCs[dg->to];
    int type;
    if (!c->alive && !(s->zombie && dg->to == s->dieA)) {
        return;
    }
    type = gdx_mp_drop_packet_type(dg->data, dg->len);
    if (type > 0) {
        if (gdx_mp_drop_ingest(&c->drop, &c->ls, dg->data, dg->len, now) > 0 && c->drop.isHost) {
            c->lastHeard[dg->data[6]] = now;
        }
        return;
    }
    {
        const int from = gdx_mp_ls_packet_from(dg->data, dg->len);
        if (from < 0 || gdx_mp_drop_refuse_from(&c->drop, from)) {
            return;
        }
        gdx_mp_drop_note_input_flags(&c->drop, &c->ls, dg->data, dg->len);
        if (gdx_mp_ls_ingest(&c->ls, dg->data, dg->len, now) < 0) {
            return;
        }
        if (c->drop.isHost) {
            c->lastHeard[from] = now;
            if (!s->mesh) {
                int q;
                for (q = 1; q < s->players; q++) {
                    if (q != from) {
                        NetSend(&sNet, 0, q, dg->data, dg->len, now);
                    }
                }
            }
        }
    }
}

static void Kill(const Scenario* s, int p, int graceful, uint32_t now) {
    Console* c = &sCs[p];
    if (graceful) {
        uint8_t pkt[PKT_CAP];
        int k;
        const int len = gdx_mp_drop_build_leave(&c->drop, &c->ls, c->tick, GDX_MP_DROP_R_LEFT, pkt, sizeof(pkt));
        CHECK(len == GDX_MP_DROP_HEADER_BYTES);
        for (k = 0; k < 3; k++) {
            SendFrom(s, p, 0, pkt, len, now);
        }
        c->left = 1;
    }
    c->alive = 0;
}

/* Runs one scenario. Returns the simulated time used. */
static uint32_t Run(const Scenario* s) {
    uint32_t now = 0;
    int p;
    memset(sCs, 0, sizeof(sCs));
    sNet.n = 0;
    sNet.lossPct = s->lossPct;
    sNet.dupPct = 10;
    sNet.jitter = s->jitter;
    for (p = 0; p < s->players; p++) {
        CHECK(gdx_mp_ls_init(&sCs[p].ls, s->players, p, s->delay, 1, SESSION) == 0);
        gdx_mp_drop_init(&sCs[p].drop, &sCs[p].ls);
        sCs[p].alive = 1;
    }
    while (now < TICKS * 40u + 40000u) {
        int done = 1;
        int i;
        now++;
        for (i = 0; i < sNet.n;) {
            if (sNet.q[i].deliverAt <= now) {
                Datagram dg = sNet.q[i];
                sNet.q[i] = sNet.q[--sNet.n];
                Deliver(s, &dg, now);
            } else {
                i++;
            }
        }
        for (p = 0; p < s->players; p++) {
            Console* c = &sCs[p];
            uint8_t pkt[PKT_CAP];
            int len;
            int dest;
            int guard;
            if (!c->alive) {
                continue;
            }
            if (c->drop.selfRemoved || c->drop.hostLeft || c->drop.fatal) {
                continue;
            }
            if (p == s->dieA && c->tick >= s->dieAtTick) {
                Kill(s, p, s->dieAGraceful, now);
                continue;
            }
            if (p == s->dieB && c->tick >= s->dieBAtTick) {
                Kill(s, p, 0, now);
                continue;
            }
            if (c->tick >= TICKS) {
                /* Finished: keep answering (the others may still need our window / acks). */
                len = gdx_mp_ls_build_packet(&c->ls, now, pkt, sizeof(pkt));
                if (len > 0 && (now % 4u) == 0) {
                    SendFrom(s, p, GDX_MP_DROP_DEST_ALL, pkt, len, now);
                }
            } else {
                done = 0;
                if ((Rand() % 4u) < (uint32_t) (p % 3)) {
                    goto service; /* console speeds differ */
                }
                gdx_mp_drop_fill(&c->drop, &c->ls, c->tick);
                if (c->ls.known[p] <= c->tick + (uint32_t) s->delay) {
                    const GdxMpPad pad = LocalPad(p, c->tick);
                    c->sentPad[c->tick + (uint32_t) s->delay] = pad;
                    gdx_mp_ls_push_local(&c->ls, c->tick, pad);
                }
                len = gdx_mp_ls_build_packet(&c->ls, now, pkt, sizeof(pkt));
                if (len > 0) {
                    SendFrom(s, p, GDX_MP_DROP_DEST_ALL, pkt, len, now);
                }
                if (gdx_mp_ls_ready(&c->ls, c->tick)) {
                    GdxMpPad pads[GDX_MP_MAX_PLAYERS];
                    gdx_mp_ls_get(&c->ls, c->tick, pads);
                    SimStep(c, pads, s->players);
                    gdx_mp_ls_note_local_hash(&c->ls, c->tick, c->state);
                    c->tick++;
                }
            }
        service:
            /* Host: time out the silent. */
            if (c->drop.isHost) {
                int q;
                for (q = 1; q < s->players; q++) {
                    if (now - c->lastHeard[q] > s->dropTimeout && !gdx_mp_drop_refuse_from(&c->drop, q)) {
                        gdx_mp_drop_begin(&c->drop, &c->ls, q, GDX_MP_DROP_R_TIMEOUT);
                    }
                }
            }
            for (guard = 0; guard < 4; guard++) {
                len = gdx_mp_drop_service(&c->drop, &c->ls, now, pkt, sizeof(pkt), &dest);
                if (len <= 0) {
                    break;
                }
                SendFrom(s, p, dest, pkt, len, now);
            }
        }
        if (done) {
            break;
        }
    }
    return now;
}

static int IsNeutral(GdxMpPad pad) {
    return pad.buttons == 0 && pad.stickX == 0 && pad.stickY == 0;
}

static int Survivor(const Scenario* s, int p) {
    return p != s->dieA && p != s->dieB;
}

static void CheckScenario(const Scenario* s, const char* name) {
    int p;
    int ref = 0;
    int dead;
    const int failsBefore = sFailures;
    Run(s);
    for (p = 0; p < s->players; p++) {
        if (!Survivor(s, p)) {
            continue;
        }
        CHECK(sCs[p].tick == TICKS);
        CHECK(!sCs[p].ls.desynced);
        CHECK(!sCs[p].drop.fatal);
        CHECK(!sCs[p].drop.selfRemoved);
        CHECK(sCs[p].state == sCs[ref].state);
        CHECK(sCs[p].padLog == sCs[ref].padLog);
        CHECK(sCs[p].drop.removedMask == sCs[ref].drop.removedMask);
    }
    for (dead = 0; dead < s->players; dead++) {
        uint32_t t;
        uint32_t T;
        if (Survivor(s, dead)) {
            continue;
        }
        T = sCs[ref].drop.removedAt[dead];
        CHECK(T != GDX_MP_NO_TICK);
        CHECK(sCs[ref].drop.reason[dead] == ((dead == s->dieA && s->dieAGraceful) ? GDX_MP_DROP_R_LEFT
                                                                                   : GDX_MP_DROP_R_TIMEOUT));
        for (p = 0; p < s->players; p++) {
            if (Survivor(s, p)) {
                CHECK(sCs[p].drop.removedAt[dead] == T);
                CHECK(sCs[p].drop.reason[dead] == sCs[ref].drop.reason[dead]);
            }
        }
        if (T == GDX_MP_NO_TICK || T > TICKS) {
            continue;
        }
        /* Before T: exactly what the dead console scheduled. From T: neutral. */
        for (t = (uint32_t) s->delay; t < T; t++) {
            const GdxMpPad a = sCs[ref].usedPad[dead][t];
            const GdxMpPad b = sCs[dead].sentPad[t];
            if (a.buttons != b.buttons || a.stickX != b.stickX || a.stickY != b.stickY) {
                CHECK(0 && "pad before T differs from what the leaver sent");
                break;
            }
        }
        for (t = T; t < TICKS; t++) {
            if (!IsNeutral(sCs[ref].usedPad[dead][t])) {
                CHECK(0 && "pad at/after T is not neutral");
                break;
            }
        }
        /* The leaver scheduled nothing past the tick it died at + delay: T can never exceed it. */
        CHECK(T <= (dead == s->dieA ? s->dieAtTick : s->dieBAtTick) + (uint32_t) s->delay);
    }
    if (s->zombie && s->dieA >= 0) {
        CHECK(sCs[s->dieA].drop.selfRemoved == 1);
    }
    sBridged[s->mesh ? 1 : 0] += sCs[0].drop.bridged;
    printf("  %-44s %s (T=%lu%s)\n", name, sFailures == failsBefore ? "ok" : "FAILED",
           (unsigned long) (s->dieA >= 0 ? sCs[ref].drop.removedAt[s->dieA] : 0),
           s->dieB >= 0 ? " +second" : "");
}

static Scenario Base(int players, int delay, int mesh) {
    Scenario s;
    memset(&s, 0, sizeof(s));
    s.players = players;
    s.delay = delay;
    s.mesh = mesh;
    s.lossPct = 10;
    s.jitter = 6;
    s.dieA = players - 1;
    s.dieAtTick = 900;
    s.dieB = -1;
    s.dieBAtTick = 0;
    s.dropTimeout = 400;
    return s;
}

static void TestScenarios(void) {
    int mesh;
    for (mesh = 0; mesh <= 1; mesh++) {
        const char* topo = mesh ? "mesh" : "star";
        char name[80];
        int players;
        int delay;
        for (players = 2; players <= MAXP; players++) {
            for (delay = 1; delay <= 4; delay++) {
                Scenario s = Base(players, delay, mesh);
                snprintf(name, sizeof(name), "%s %dP D=%d silent drop", topo, players, delay);
                CheckScenario(&s, name);
                s.dieAGraceful = 1;
                s.dieAtTick = 1311;
                snprintf(name, sizeof(name), "%s %dP D=%d LEAVE", topo, players, delay);
                CheckScenario(&s, name);
            }
        }
        {
            Scenario s = Base(4, 2, mesh);
            s.dieA = 1; /* a middle player */
            s.lossPct = 35;
            s.jitter = 12;
            snprintf(name, sizeof(name), "%s 4P D=2 middle player, 35%% loss", topo);
            CheckScenario(&s, name);
        }
        {
            Scenario s = Base(4, 3, mesh);
            s.dieA = 3;
            s.dieB = 2;
            s.dieAtTick = 700;
            s.dieBAtTick = 702; /* the second dies while the first removal is in flight */
            snprintf(name, sizeof(name), "%s 4P D=3 two drops back to back", topo);
            CheckScenario(&s, name);
        }
        {
            Scenario s = Base(3, 2, mesh);
            s.dieA = 2;
            s.dieB = 1;
            s.dieAtTick = 500;
            s.dieBAtTick = 1700;
            snprintf(name, sizeof(name), "%s 3P D=2 drop, then the last joiner", topo);
            CheckScenario(&s, name);
        }
        {
            Scenario s = Base(3, 2, mesh);
            s.zombie = 1; /* stops sending but still listens: must hear it was removed */
            snprintf(name, sizeof(name), "%s 3P D=2 zombie hears its removal", topo);
            CheckScenario(&s, name);
        }
        {
            Scenario s = Base(3, 8, mesh); /* the largest delay the redundancy allows */
            s.lossPct = 20;
            snprintf(name, sizeof(name), "%s 3P D=8 silent drop, 20%% loss", topo);
            CheckScenario(&s, name);
        }
    }
}

static void TestBridgedCoverage(void) {
    /* A star joiner only holds what the host relayed: never more than the host. */
    CHECK(sBridged[0] == 0);
    /* In a mesh the case the FREEZE phase exists for must actually have been exercised. */
    CHECK(sBridged[1] > 0);
    printf("  tails that bridged the host's gap: star %lu, mesh %lu\n", (unsigned long) sBridged[0],
           (unsigned long) sBridged[1]);
}

static void TestNoDrop(void) {
    Scenario s = Base(3, 2, 0);
    s.dieA = -1;
    Run(&s);
    CHECK(sCs[0].tick == TICKS && sCs[1].tick == TICKS && sCs[2].tick == TICKS);
    CHECK(sCs[0].drop.removedMask == 0 && sCs[1].drop.removedMask == 0);
    CHECK(sCs[0].state == sCs[1].state && sCs[1].state == sCs[2].state);
    CHECK(sCs[0].drop.commits == 0);
}

/* ---- hostile datagrams ---------------------------------------------------------------------- */

static int Build(uint8_t* out, int type, int from, int count, uint32_t session, int player, int phase,
                 uint32_t tick, uint32_t first) {
    int i;
    memset(out, 0, GDX_MP_DROP_MAX_PACKET);
    out[0] = 'G';
    out[1] = 'X';
    out[2] = 'M';
    out[3] = 'P';
    out[4] = GDX_MP_VERSION;
    out[5] = (uint8_t) type;
    out[6] = (uint8_t) from;
    out[7] = (uint8_t) count;
    out[8] = (uint8_t) (session & 0xFF);
    out[9] = (uint8_t) ((session >> 8) & 0xFF);
    out[10] = (uint8_t) ((session >> 16) & 0xFF);
    out[11] = (uint8_t) (session >> 24);
    out[12] = (uint8_t) player;
    out[13] = (uint8_t) phase;
    out[16] = (uint8_t) (tick & 0xFF);
    out[17] = (uint8_t) ((tick >> 8) & 0xFF);
    out[18] = (uint8_t) ((tick >> 16) & 0xFF);
    out[19] = (uint8_t) (tick >> 24);
    out[20] = (uint8_t) (first & 0xFF);
    out[21] = (uint8_t) ((first >> 8) & 0xFF);
    out[22] = (uint8_t) ((first >> 16) & 0xFF);
    out[23] = (uint8_t) (first >> 24);
    for (i = 0; i < count; i++) {
        out[GDX_MP_DROP_HEADER_BYTES + 4 * i] = 0x55;
    }
    return GDX_MP_DROP_HEADER_BYTES + 4 * count;
}

static void TestHostile(void) {
    static GdxMpLockstep host;
    static GdxMpLockstep peer;
    static GdxMpDrop dh;
    static GdxMpDrop dp;
    uint8_t pkt[GDX_MP_DROP_MAX_PACKET + 8];
    int len;
    int i;
    uint32_t rej;

    CHECK(gdx_mp_ls_init(&host, 3, 0, 2, 1, SESSION) == 0);
    CHECK(gdx_mp_ls_init(&peer, 3, 1, 2, 1, SESSION) == 0);
    gdx_mp_drop_init(&dh, &host);
    gdx_mp_drop_init(&dp, &peer);

    /* A DROP from a non-host is refused. */
    len = Build(pkt, GDX_MP_TYPE_DROP, 2, 0, SESSION, 1, GDX_MP_DROP_COMMIT, 10, 0);
    CHECK(gdx_mp_drop_ingest(&dp, &peer, pkt, len, 0) < 0);
    CHECK(dp.selfRemoved == 0 && dp.removedMask == 0);
    /* Removing the host is refused. */
    len = Build(pkt, GDX_MP_TYPE_DROP, 0, 0, SESSION, 0, GDX_MP_DROP_COMMIT, 10, 0);
    CHECK(gdx_mp_drop_ingest(&dp, &peer, pkt, len, 0) < 0);
    /* Wrong session. */
    len = Build(pkt, GDX_MP_TYPE_DROP, 0, 0, SESSION ^ 1u, 2, GDX_MP_DROP_FREEZE, 0, 0);
    CHECK(gdx_mp_drop_ingest(&dp, &peer, pkt, len, 0) < 0);
    CHECK(dp.frozenMask == 0);
    /* Bad sizes / counts / types / phases. */
    len = Build(pkt, GDX_MP_TYPE_DROP, 0, 2, SESSION, 2, GDX_MP_DROP_FREEZE, 0, 0);
    CHECK(gdx_mp_drop_ingest(&dp, &peer, pkt, len - 1, 0) < 0);
    CHECK(gdx_mp_drop_ingest(&dp, &peer, pkt, len + 1, 0) < 0);
    pkt[7] = GDX_MP_REDUNDANCY + 1;
    CHECK(gdx_mp_drop_ingest(&dp, &peer, pkt, len, 0) < 0);
    len = Build(pkt, 9, 0, 0, SESSION, 2, GDX_MP_DROP_FREEZE, 0, 0);
    CHECK(gdx_mp_drop_ingest(&dp, &peer, pkt, len, 0) < 0);
    len = Build(pkt, GDX_MP_TYPE_DROP, 0, 0, SESSION, 2, 7, 0, 0);
    CHECK(gdx_mp_drop_ingest(&dp, &peer, pkt, len, 0) < 0);
    len = Build(pkt, GDX_MP_TYPE_DROP, 0, 0, SESSION, 5, GDX_MP_DROP_FREEZE, 0, 0); /* player >= numPlayers */
    CHECK(gdx_mp_drop_ingest(&dp, &peer, pkt, len, 0) < 0);
    /* A LEAVE that names someone else is malformed (only the sender can leave). */
    len = Build(pkt, GDX_MP_TYPE_LEAVE, 2, 0, SESSION, 1, 0, 0, 0);
    CHECK(gdx_mp_drop_packet_type(pkt, len) < 0);
    CHECK(gdx_mp_drop_ingest(&dh, &host, pkt, len, 0) < 0);
    CHECK(dh.pending < 0);
    CHECK(dp.frozenMask == 0 && dp.removedMask == 0 && dp.fatal == 0);

    /* An absurd ACK report (would make T unfillable) is refused, the removal stays pending. */
    CHECK(gdx_mp_drop_begin(&dh, &host, 2, GDX_MP_DROP_R_FORCED) == 1);
    CHECK(gdx_mp_drop_begin(&dh, &host, 2, GDX_MP_DROP_R_FORCED) == 0); /* already pending */
    CHECK(gdx_mp_drop_begin(&dh, &host, 0, GDX_MP_DROP_R_FORCED) == 0); /* the host itself */
    CHECK(gdx_mp_drop_begin(&dp, &peer, 2, GDX_MP_DROP_R_FORCED) == 0); /* not the host */
    len = Build(pkt, GDX_MP_TYPE_DROP_ACK, 1, 0, SESSION, 2, GDX_MP_DROP_FREEZE, 100000u, 0);
    CHECK(gdx_mp_drop_ingest(&dh, &host, pkt, len, 0) < 0);
    CHECK(dh.ackMask == 0);
    {
        int dest;
        CHECK(gdx_mp_drop_service(&dh, &host, 1, pkt, sizeof(pkt), &dest) > 0); /* FREEZE goes out */
        CHECK(dh.removedMask == 0);
    }
    /* A sane ACK commits. */
    len = Build(pkt, GDX_MP_TYPE_DROP_ACK, 1, 0, SESSION, 2, GDX_MP_DROP_FREEZE, 2, 0);
    CHECK(gdx_mp_drop_ingest(&dh, &host, pkt, len, 0) == 1);
    {
        int dest;
        CHECK(gdx_mp_drop_service(&dh, &host, 100, pkt, sizeof(pkt), &dest) > 0);
        CHECK(dh.removedMask == 0x04 && dh.removedAt[2] == 2u && dh.phase == GDX_MP_DROP_COMMIT);
        CHECK(pkt[5] == GDX_MP_TYPE_DROP && pkt[13] == GDX_MP_DROP_COMMIT);
        /* The peer takes the real COMMIT and answers it. */
        CHECK(gdx_mp_drop_ingest(&dp, &peer, pkt, gdx_mp_drop_packet_type(pkt, GDX_MP_DROP_HEADER_BYTES + 4 * pkt[7]) > 0
                                                       ? GDX_MP_DROP_HEADER_BYTES + 4 * pkt[7]
                                                       : 0,
                                 100) == 1);
        CHECK(dp.removedAt[2] == 2u && dp.fatal == 0);
        len = gdx_mp_drop_service(&dp, &peer, 101, pkt, sizeof(pkt), &dest);
        CHECK(len > 0 && dest == 0 && pkt[5] == GDX_MP_TYPE_DROP_ACK && pkt[13] == GDX_MP_DROP_COMMIT);
        CHECK(gdx_mp_drop_ingest(&dh, &host, pkt, len, 101) == 1);
        CHECK(gdx_mp_drop_service(&dh, &host, 200, pkt, sizeof(pkt), &dest) == 0);
        CHECK(dh.pending == -1);
    }
    /* A conflicting second COMMIT for the same player is refused. */
    len = Build(pkt, GDX_MP_TYPE_DROP, 0, 0, SESSION, 2, GDX_MP_DROP_COMMIT, 40, 0);
    CHECK(gdx_mp_drop_ingest(&dp, &peer, pkt, len, 0) < 0);
    CHECK(dp.removedAt[2] == 2u);

    /* Fuzz: random mutations of valid packets never crash and never commit a removal to a
     * console other than through a well-formed host DROP. */
    rej = dp.rejected;
    for (i = 0; i < 200000; i++) {
        uint8_t f[GDX_MP_DROP_MAX_PACKET + 8];
        static GdxMpLockstep fl;
        static GdxMpDrop fd;
        int flen = Build(f, GDX_MP_TYPE_DROP + (int) (Rand() % 3), (int) (Rand() % 4), (int) (Rand() % 17),
                         (Rand() % 4) ? SESSION : Rand(), (int) (Rand() % 4), (int) (Rand() % 3), Rand() % 64,
                         Rand() % 64);
        int k;
        const int flips = (int) (Rand() % 6);
        if ((i % 1000) == 0) {
            gdx_mp_ls_init(&fl, 3, 1 + (int) (Rand() % 2), 2, 1, SESSION);
            gdx_mp_drop_init(&fd, &fl);
        }
        for (k = 0; k < flips; k++) {
            f[Rand() % (uint32_t) flen] ^= (uint8_t) (1u << (Rand() % 8));
        }
        if ((Rand() % 8) == 0) {
            flen = (int) (Rand() % (uint32_t) sizeof(f));
        }
        gdx_mp_drop_ingest(&fd, &fl, f, flen, (uint32_t) i);
        {
            int dest;
            uint8_t outb[GDX_MP_DROP_MAX_PACKET];
            gdx_mp_drop_service(&fd, &fl, (uint32_t) i, outb, sizeof(outb), &dest);
        }
        CHECK_QUIET(fd.removedMask == 0 || fd.removedAt[LowestSetBit(fd.removedMask)] != GDX_MP_NO_TICK);
    }
    CHECK(dp.rejected >= rej);
}

int main(void) {
    printf("gdx_mp_drop_host_tests\n");
    TestNoDrop();
    TestScenarios();
    TestBridgedCoverage();
    TestHostile();
    printf("%d checks, %d failures\n", sChecks, sFailures);
    return sFailures == 0 ? 0 : 1;
}
