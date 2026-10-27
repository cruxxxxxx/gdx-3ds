/* port/tests/gdx_mp_online_wire_probe.c — interop probe: the console's GXRV/GXLB code
 * (port/gdx_mp_online.c, port/gdx_mp_lobby.c) against a REAL tools/mp-rendezvous/gdx_rendezvous.py
 * over loopback UDP. Run by tools/mp-rendezvous/test_rendezvous.py (which starts the servers):
 *   gdx_mp_online_wire_probe <server-port> [mode]
 * Every CREATE / JOIN goes through the console's own [net] mp_rv_cookie state machine
 * (gdx_rv_cookie_*), as port/3ds/net/gdx3ds_mp_p2p.c drives it.
 *   v2 (default; a 2.1.0 server, cookie required): five clients in mode 1: a host CREATEs (zeros ->
 *      COOKIE -> resend -> WELCOME), three JOIN by the returned code, a fifth with another build is
 *      refused with a readable mismatch that says which side should update; every member gets the
 *      4-entry roster; a relayed HELLO reaches exactly the masked members byte for byte; a relayed
 *      packet claiming another slot reaches nobody; the host LEAVEs and the others are told.
 *   v1 (a server run with --allow-v1): the same room in v1 (mode 0), plus an auto client that
 *      still gets in with v2.
 *   refused (a requiring server): what a card before the cookie build sees (a v1 CREATE -> the
 *      readable version mismatch, its detail within the old parser's 0x21..0x7E); an auto client
 *      whose first two v2 tries are lost (falls back to v1, is refused "needs GXRV v2", goes back
 *      to v2, gets in); a v2 client one proto below the minimum (UPDATE REQUIRED) and one above
 *      the maximum (SERVER NEEDS AN UPDATE).
 *   old (a server before 2.0.0: every v2 datagram dropped): auto falls back to v1 after two tries
 *      and gets in; mode 1 never gets an answer.
 * Prints "WIRE PROBE PASS" and exits 0 on success. */
#include "gdx_mp_lobby.h"
#include "gdx_mp_online.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int sFail = 0;
#define EXPECT(c)                                                        \
    do {                                                                 \
        if (!(c)) {                                                      \
            fprintf(stderr, "probe FAIL line %d: %s\n", __LINE__, #c);   \
            sFail++;                                                     \
        }                                                                \
    } while (0)

static struct sockaddr_in sServer;

static int OpenClient(void) {
    struct sockaddr_in a;
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    bind(fd, (struct sockaddr*) &a, sizeof(a));
    return fd;
}

static void Send(int fd, const uint8_t* b, int n) {
    sendto(fd, b, (size_t) n, 0, (struct sockaddr*) &sServer, sizeof(sServer));
}

/* Receive one datagram within ms, 0 on timeout. */
static int Recv(int fd, uint8_t* b, int cap, int ms) {
    struct pollfd p;
    p.fd = fd;
    p.events = POLLIN;
    if (poll(&p, 1, ms) <= 0) {
        return 0;
    }
    return (int) recv(fd, b, (size_t) cap, 0);
}

/* Receive until a datagram of GXRV type `want` arrives (others skipped), 0 on timeout. */
static int RecvType(int fd, int want, uint8_t* b, int cap, int ms) {
    int i;
    for (i = 0; i < 20; i++) {
        const int n = Recv(fd, b, cap, ms);
        if (n <= 0) {
            return 0;
        }
        if (gdx_rv_peek_type(b, n) == want) {
            return n;
        }
    }
    return 0;
}

static uint32_t NowMs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t) (ts.tv_sec * 1000u + (uint32_t) (ts.tv_nsec / 1000000));
}

#define PROBE_RETRY_MS 300 /* the console's is 1000 ms; the state machine only counts tries */
#define PROBE_MAX_SENDS 8

typedef struct ProbeAnswer {
    int type; /* GDX_RV_WELCOME / ERROR / UPDATE, 0 = nothing */
    GdxMpRvWelcome w;
    GdxMpRvError e;
    GdxMpRvUpdate u;
    int version; /* the answer's header version */
    int cookies;
    int fellBack;
    int backToV2;
} ProbeAnswer;

/* One CREATE / JOIN through the console's state machine. The first `lose` sends are dropped
 * before they leave (a lossy first second). */
static ProbeAnswer Request(int fd, GdxRvCookie* ck, int type, GdxMpRvRequest* r, int lose) {
    ProbeAnswer a;
    uint8_t pkt[GDX_RV_REQ2_BYTES];
    uint8_t b[2048];
    int sends = 0;
    int resendNow = 0;
    const uint32_t t0 = NowMs();
    memset(&a, 0, sizeof(a));
    gdx_rv_cookie_begin(ck, r->nonce, t0, 1);
    while (sends < PROBE_MAX_SENDS) {
        int ev = 0;
        uint32_t deadline;
        const int n = gdx_rv_cookie_build(ck, type, r, NowMs(), pkt, sizeof(pkt), &ev);
        if (ev == GDX_RV_CK_FALLBACK) {
            a.fellBack = 1;
            printf("  [mpo] rv cookie: auto: %d v2 requests unanswered (%u ms) -> fallback to v1\n",
                   GDX_RV_COOKIE_FALLBACK_TRIES, (unsigned) (NowMs() - t0));
        }
        if (sends >= lose || resendNow) {
            Send(fd, pkt, n);
        }
        sends++;
        resendNow = 0;
        deadline = NowMs() + PROBE_RETRY_MS;
        while (!resendNow && (int32_t) (deadline - NowMs()) > 0) {
            const int got = Recv(fd, b, sizeof(b), (int) (deadline - NowMs()));
            int t;
            if (got <= 0) {
                break;
            }
            t = gdx_rv_peek_type(b, got);
            if (gdx_rv_cookie_on_reply(ck, b, got, NowMs()) == GDX_RV_CK_RESEND) {
                a.cookies++;
                resendNow = 1;
            } else if (t == GDX_RV_WELCOME && gdx_rv_parse_welcome(b, got, &a.w) == 0 && a.w.nonce == r->nonce) {
                a.type = t;
            } else if (t == GDX_RV_UPDATE && gdx_rv_parse_update(b, got, &a.u) == 0 && a.u.nonce == r->nonce) {
                a.type = t;
            } else if (t == GDX_RV_ERROR && gdx_rv_parse_error(b, got, &a.e) == 0 && a.e.nonce == r->nonce) {
                if (gdx_rv_cookie_on_error(ck, &a.e) == GDX_RV_CK_RETRY_V2) {
                    a.backToV2 = 1;
                    printf("  [mpo] rv cookie: auto: v1 refused (server needs GXRV v2) -> back to v2\n");
                    resendNow = 1;
                } else {
                    a.type = t;
                }
            }
            if (a.type != 0) {
                a.version = b[4];
                if (ck->useV2 && a.cookies > 0) {
                    printf("  [mpo] rv cookie: v2 request -> COOKIE -> resend %s (rtt=%u ms, cookies=%d)\n",
                           t == GDX_RV_WELCOME ? "ok" : "answered", (unsigned) ck->cookieRttMs, a.cookies);
                }
                return a;
            }
        }
        if (resendNow) {
            sends--; /* an immediate resend is not a retry */
        }
    }
    return a;
}

static GdxMpRvRequest Req(const char* build, const char* name, const char* code, uint32_t nonce) {
    GdxMpRvRequest r;
    memset(&r, 0, sizeof(r));
    r.nonce = nonce;
    r.contentHash = 0x0C0FFEE0u;
    r.priv.ip[0] = 192;
    r.priv.ip[1] = 168;
    r.priv.ip[2] = 2;
    r.priv.ip[3] = (uint8_t) (nonce & 0x7F);
    r.priv.port = 27100;
    r.maxPlayers = 4;
    strcpy(r.build, build);
    strcpy(r.name, name);
    strcpy(r.code, code);
    r.clientProto = GDX_MPO_PROTO;
    return r;
}

/* What a card before the cookie build accepts as an ERROR detail (its GetFixed kind 0). */
static int OldCardPrintable(const char* s) {
    for (; *s != '\0'; s++) {
        if ((unsigned char) *s < 0x21 || (unsigned char) *s > 0x7E) {
            return 0;
        }
    }
    return 1;
}

static int ModeRefused(void) {
    const int fd = OpenClient();
    GdxRvCookie ck;
    GdxMpRvRequest r = Req("mp-online@probe", "OLDCARD", "", 4001);
    ProbeAnswer a;
    char m[160];
    /* a card before the cookie build: v1 only */
    gdx_rv_cookie_init(&ck, GDX_RV_COOKIE_MODE_V1);
    a = Request(fd, &ck, GDX_RV_CREATE, &r, 0);
    EXPECT(a.type == GDX_RV_ERROR && a.version == 1 && a.e.code == GDX_RV_E_BUILD_MISMATCH);
    EXPECT(strcmp(a.e.detail, GDX_RV_NEEDS_V2_DETAIL) == 0);
    EXPECT(OldCardPrintable(a.e.detail)); /* else an old card would show an empty detail */
    printf("  old card shows: \"%s: host runs %s\"\n", gdx_rv_error_text(a.e.code), a.e.detail);
    gdx_rv_error_message(&a.e, GDX_MPO_PROTO, m, sizeof(m));
    printf("  this build with mp_rv_cookie=0 shows: \"%s\"\n", m);
    /* auto whose first two v2 tries are lost: v1 -> refused -> back to v2 -> in */
    gdx_rv_cookie_init(&ck, GDX_RV_COOKIE_MODE_AUTO);
    r.nonce = 4002;
    a = Request(fd, &ck, GDX_RV_CREATE, &r, GDX_RV_COOKIE_FALLBACK_TRIES);
    EXPECT(a.fellBack && a.backToV2 && a.type == GDX_RV_WELCOME && a.version == 2 && a.cookies >= 1);
    /* one proto below the minimum: UPDATE REQUIRED */
    gdx_rv_cookie_init(&ck, GDX_RV_COOKIE_MODE_V2);
    r = Req("mp-online@probe", "STALE", "", 4003);
    r.clientProto = GDX_MPO_PROTO - 1;
    a = Request(fd, &ck, GDX_RV_CREATE, &r, 0);
    EXPECT(a.type == GDX_RV_UPDATE && a.u.minProto == GDX_MPO_PROTO && a.u.yourProto == GDX_MPO_PROTO - 1);
    EXPECT(gdx_rv_update_message(&a.u, r.clientProto, m, sizeof(m)) == GDX_RV_UPD_CLIENT_OLD);
    EXPECT(strstr(m, "UPDATE REQUIRED") == m);
    printf("  stale client shows: \"%s\"\n", m);
    printf("  [mpo] server requires client proto>=%u (have %d)\n", (unsigned) a.u.minProto, r.clientProto);
    /* one above the maximum: the server needs an update */
    r.nonce = 4004;
    r.clientProto = GDX_MPO_PROTO + 1;
    a = Request(fd, &ck, GDX_RV_CREATE, &r, 0);
    EXPECT(a.type == GDX_RV_UPDATE && a.u.maxProto == GDX_MPO_PROTO);
    EXPECT(gdx_rv_update_message(&a.u, r.clientProto, m, sizeof(m)) == GDX_RV_UPD_SERVER_OLD);
    printf("  too-new client shows: \"%s\"\n", m);
    close(fd);
    return 0;
}

static int ModeOld(void) {
    const int fd = OpenClient();
    GdxRvCookie ck;
    GdxMpRvRequest r = Req("mp-online@probe", "AUTO", "", 5001);
    ProbeAnswer a;
    gdx_rv_cookie_init(&ck, GDX_RV_COOKIE_MODE_AUTO);
    a = Request(fd, &ck, GDX_RV_CREATE, &r, 0);
    EXPECT(a.fellBack && a.type == GDX_RV_WELCOME && a.version == 1);
    printf("  auto vs a server before 2.0.0: fell back to v1, room %s\n", a.w.code);
    /* the rest of the session stays v1: a second request is v1 at once */
    r = Req("mp-online@probe", "AUTO2", a.w.code, 5002);
    {
        const int fd2 = OpenClient();
        GdxRvCookie ck2 = ck;
        ProbeAnswer j = Request(fd2, &ck2, GDX_RV_JOIN, &r, 0);
        EXPECT(j.type == GDX_RV_WELCOME && !j.fellBack && j.version == 1);
        close(fd2);
    }
    gdx_rv_cookie_init(&ck, GDX_RV_COOKIE_MODE_V2);
    r.nonce = 5003;
    a = Request(fd, &ck, GDX_RV_CREATE, &r, 0);
    EXPECT(a.type == 0); /* mode 1 only speaks v2: an old server never answers */
    close(fd);
    return 0;
}

int main(int argc, char** argv) {
    int fd[5];
    GdxMpRvWelcome w[5];
    uint8_t b[2048];
    int n;
    int i;
    const char* build = "mp-online@probe";
    const char* mode = argc >= 3 ? argv[2] : "v2";
    const int ckMode = strcmp(mode, "v1") == 0 ? GDX_RV_COOKIE_MODE_V1 : GDX_RV_COOKIE_MODE_V2;
    GdxRvCookie ck[5];
    if (argc < 2) {
        fprintf(stderr, "usage: %s <server-port> [v2|v1|refused|old]\n", argv[0]);
        return 2;
    }
    memset(&sServer, 0, sizeof(sServer));
    sServer.sin_family = AF_INET;
    sServer.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sServer.sin_port = htons((uint16_t) atoi(argv[1]));
    if (strcmp(mode, "refused") == 0 || strcmp(mode, "old") == 0) {
        if (strcmp(mode, "refused") == 0) {
            ModeRefused();
        } else {
            ModeOld();
        }
        printf(sFail != 0 ? "WIRE PROBE FAIL (%d)\n" : "WIRE PROBE PASS\n", sFail);
        return sFail != 0 ? 1 : 0;
    }
    for (i = 0; i < 5; i++) {
        fd[i] = OpenClient();
        gdx_rv_cookie_init(&ck[i], ckMode);
    }

    {
        GdxMpRvRequest r = Req(build, "HOST", "", 1001);
        ProbeAnswer a = Request(fd[0], &ck[0], GDX_RV_CREATE, &r, 0);
        EXPECT(a.type == GDX_RV_WELCOME && a.version == (ckMode == GDX_RV_COOKIE_MODE_V2 ? 2 : 1));
        EXPECT(ckMode == GDX_RV_COOKIE_MODE_V1 || a.cookies == 1);
        w[0] = a.w;
        EXPECT(w[0].slot == 0 && w[0].nonce == 1001 && w[0].seenAs.ip[0] == 127);
        /* A retransmitted CREATE (the same bytes, cookie and all) gets the same room, not a second one. */
        n = gdx_rv_cookie_build(&ck[0], GDX_RV_CREATE, &r, NowMs(), b, sizeof(b), NULL);
        Send(fd[0], b, n);
        n = RecvType(fd[0], GDX_RV_WELCOME, b, sizeof(b), 2000);
        {
            GdxMpRvWelcome again;
            EXPECT(n > 0 && gdx_rv_parse_welcome(b, n, &again) == 0 && strcmp(again.code, w[0].code) == 0 &&
                   again.memberKey == w[0].memberKey);
        }
    }
    for (i = 1; i < 4; i++) {
        char name[8];
        GdxMpRvRequest r;
        snprintf(name, sizeof(name), "P%d", i);
        r = Req(build, name, w[0].code, 2000u + (uint32_t) i);
        {
            ProbeAnswer a = Request(fd[i], &ck[i], GDX_RV_JOIN, &r, 0);
            EXPECT(a.type == GDX_RV_WELCOME && a.w.slot == i);
            w[i] = a.w;
        }
        EXPECT(w[i].roomSecret == w[0].roomSecret && w[i].memberKey != w[0].memberKey);
    }
    {
        GdxMpRvRequest r = Req("integ-v1.0.1@other", "OLD", w[0].code, 3000);
        ProbeAnswer a = Request(fd[4], &ck[4], GDX_RV_JOIN, &r, 0);
        char m[160];
        EXPECT(a.type == GDX_RV_ERROR && a.e.code == GDX_RV_E_BUILD_MISMATCH);
        EXPECT(strcmp(a.e.detail, build) == 0);
        /* a v2 joiner learns the host's proto (same here: another build of this version) */
        EXPECT(a.e.hostProto == (ckMode == GDX_RV_COOKIE_MODE_V2 ? GDX_MPO_PROTO : 0));
        gdx_rv_error_message(&a.e, GDX_MPO_PROTO, m, sizeof(m));
        printf("  mismatch refused: \"%s\"\n", m);
    }
    for (i = 0; i < 4; i++) {
        GdxMpRvRoster ro;
        uint32_t ver = 0;
        uint32_t echo = 0;
        n = gdx_rv_build_keepalive(w[i].code, w[i].slot, w[i].memberKey, 0, 4242, b, sizeof(b));
        Send(fd[i], b, n);
        n = RecvType(fd[i], GDX_RV_KA_ACK, b, sizeof(b), 2000);
        EXPECT(n > 0 && gdx_rv_parse_ka_ack(b, n, &ver, &echo) == 0 && echo == 4242);
        n = RecvType(fd[i], GDX_RV_ROSTER, b, sizeof(b), 2000);
        EXPECT(n > 0 && gdx_rv_parse_roster(b, n, &ro) == 0 && ro.count == 4 && ro.version == ver);
        EXPECT(ro.member[3].present && strcmp(ro.member[3].name, "P3") == 0 && ro.member[0].isHost);
        EXPECT(ro.member[2].pub.ip[0] == 127 && ro.member[2].priv.ip[0] == 192);
    }
    /* Drain whatever roster broadcasts are still queued from the joins. */
    for (i = 0; i < 4; i++) {
        while (Recv(fd[i], b, sizeof(b), 100) > 0) {
        }
    }
    {
        /* Slot 1 relays a HELLO to slots 0 and 2. */
        GdxMpLobby lb;
        GdxMpRvRoster ro;
        uint8_t hello[GDX_LB_HELLO_BYTES];
        uint8_t got[2048];
        int hn;
        memset(&ro, 0, sizeof(ro));
        for (i = 0; i < 4; i++) {
            ro.member[i].present = 1;
        }
        gdx_lobby_init(&lb, 1, w[1].roomSecret, build, 0x0C0FFEE0u, "P1");
        gdx_lobby_set_roster(&lb, &ro);
        hn = gdx_lobby_build_hello(&lb, 0, 777, 0, hello, sizeof(hello));
        hello[7] = 0xFF; /* addressed to everyone */
        n = gdx_rv_build_relay(w[1].code, 1, 0x05, w[1].memberKey, hello, hn, b, sizeof(b));
        Send(fd[1], b, n);
        n = Recv(fd[0], got, sizeof(got), 2000);
        EXPECT(n == hn && memcmp(got, hello, (size_t) hn) == 0);
        n = Recv(fd[2], got, sizeof(got), 2000);
        EXPECT(n == hn && memcmp(got, hello, (size_t) hn) == 0);
        EXPECT(Recv(fd[3], got, sizeof(got), 300) == 0);
        {
            GdxMpLbHeader h;
            GdxMpLbBody body;
            EXPECT(gdx_lb_parse(got, hn, w[2].roomSecret, &h, &body) == GDX_LB_HELLO && h.from == 1);
        }
        /* The same HELLO claiming slot 2, relayed by slot 1: dropped by the server. */
        hello[6] = 2;
        n = gdx_rv_build_relay(w[1].code, 1, 0x0D, w[1].memberKey, hello, hn, b, sizeof(b));
        Send(fd[1], b, n);
        EXPECT(Recv(fd[0], got, sizeof(got), 300) == 0);
        EXPECT(Recv(fd[3], got, sizeof(got), 300) == 0);
        /* A relay with a wrong member key: dropped. */
        hello[6] = 1;
        n = gdx_rv_build_relay(w[1].code, 1, 0x01, w[1].memberKey ^ 1, hello, hn, b, sizeof(b));
        Send(fd[1], b, n);
        EXPECT(Recv(fd[0], got, sizeof(got), 300) == 0);
        /* A relay of something that is not a game datagram: dropped. */
        memcpy(hello, "JUNK", 4);
        n = gdx_rv_build_relay(w[1].code, 1, 0x01, w[1].memberKey, hello, hn, b, sizeof(b));
        Send(fd[1], b, n);
        EXPECT(Recv(fd[0], got, sizeof(got), 300) == 0);
    }
    {
        GdxMpRvError e;
        n = gdx_rv_build_auth(GDX_RV_LEAVE, w[0].code, 0, 0, w[0].memberKey, b, sizeof(b));
        Send(fd[0], b, n);
        for (i = 1; i < 4; i++) {
            n = RecvType(fd[i], GDX_RV_ERROR, b, sizeof(b), 2000);
            EXPECT(n > 0 && gdx_rv_parse_error(b, n, &e) == 0 && e.code == GDX_RV_E_HOST_LEFT);
        }
    }
    if (ckMode == GDX_RV_COOKIE_MODE_V1) {
        /* a server that admits v1 still takes an auto (v2-first) console without a fallback */
        GdxRvCookie a2;
        GdxMpRvRequest r = Req(build, "AUTO", "", 6001);
        ProbeAnswer a;
        gdx_rv_cookie_init(&a2, GDX_RV_COOKIE_MODE_AUTO);
        a = Request(fd[4], &a2, GDX_RV_CREATE, &r, 0);
        EXPECT(a.type == GDX_RV_WELCOME && a.version == 2 && !a.fellBack);
    }
    for (i = 0; i < 5; i++) {
        close(fd[i]);
    }
    if (sFail != 0) {
        printf("WIRE PROBE FAIL (%d)\n", sFail);
        return 1;
    }
    printf("WIRE PROBE PASS\n");
    return 0;
}
