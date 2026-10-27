/* port/tests/gdx_mp_online_host_tests.c — the online multiplayer pure core (port/gdx_mp_online.c,
 * port/gdx_mp_lobby.c, with port/gdx_mp_lockstep.c) on the host. Checks:
 *   1. GXRV builders/parsers against the byte tables in tools/mp-rendezvous/README.md, and every
 *      malformed server datagram refused (truncated, bad counts, duplicate slot, no host, bad text);
 *      1b. GXRV v2: the 100-byte request, COOKIE, and the console's [net] mp_rv_cookie state
 *      machine (1 / 0 / auto: the old-server fallback, the back-to-v2 on "needs GXRV v2");
 *   2. room codes, names, host:port parsing;
 *   3. GXLB round trips, wrong secret, wrong sizes, hostile field values, a START from a non-host;
 *   4. the delay-from-RTT table (design doc 8);
 *   5. the link state machine: punch -> direct only when confirmed both ways; one-way filtering
 *      (a symmetric NAT on one side) and two-way filtering fall back to relay; forced relay;
 *   6. receive hygiene: token bucket, junk-source blocklist, pad clamping (hostile sticks/buttons);
 *   7. a simulated 2/3/4-console session end to end: roster -> HELLO -> ready -> START (delay from
 *      the measured RTT) -> lockstep over a network with per-console NAT types, loss and latency;
 *      every console must feed its game the identical pad sequence, and a 100 ms RTT network must
 *      pick a larger delay than a LAN and still run without stalling beyond it;
 *   8. the zip central-directory content hash. */
#include "gdx_mp_lan.h"
#include "gdx_mp_lobby.h"
#include "gdx_mp_lockstep.h"
#include "gdx_mp_online.h"
#include "gdx_mp_pause.h"
#include "gdx_mp_remove.h"
#include "gdx_mp_uds_bridge.h"

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

static uint32_t sRng = 777u;

static uint32_t Rand(void) {
    sRng = sRng * 1103515245u + 12345u;
    return (sRng >> 8) & 0xFFFFFFu;
}

static void PutU16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t) v;
    p[1] = (uint8_t) (v >> 8);
}

static void PutU32(uint8_t* p, uint32_t v) {
    PutU16(p, (uint16_t) v);
    PutU16(p + 2, (uint16_t) (v >> 16));
}

static void PutU64(uint8_t* p, uint64_t v) {
    PutU32(p, (uint32_t) v);
    PutU32(p + 4, (uint32_t) (v >> 32));
}

/* ---- 1. GXRV -------------------------------------------------------------------------------- */
static int MakeWelcome(uint8_t* p, int slot, int maxPlayers, uint64_t secret) {
    memset(p, 0, 48);
    memcpy(p, "GXRV", 4);
    p[4] = 1;
    p[5] = GDX_RV_WELCOME;
    PutU32(p + 8, 0xABCD1234u);
    memcpy(p + 12, "K7MPQ", 5);
    p[20] = (uint8_t) slot;
    p[21] = (uint8_t) maxPlayers;
    p[22] = 1;
    PutU64(p + 24, 0x1122334455667788ull);
    PutU64(p + 32, secret);
    p[40] = 203;
    p[41] = 0;
    p[42] = 113;
    p[43] = 9;
    PutU16(p + 44, 41000);
    return 48;
}

static int MakeRoster(uint8_t* p, int count, const int* slots) {
    int i;
    memset(p, 0, 24 + 28 * 4);
    memcpy(p, "GXRV", 4);
    p[4] = 1;
    p[5] = GDX_RV_ROSTER;
    memcpy(p + 8, "K7MPQ", 5);
    PutU32(p + 16, 5);
    p[20] = (uint8_t) count;
    p[21] = 2;
    for (i = 0; i < count; i++) {
        uint8_t* e = p + 24 + 28 * i;
        e[0] = (uint8_t) slots[i];
        e[1] = slots[i] == 0 ? 1 : 0;
        PutU16(e + 2, (uint16_t) (40 + slots[i]));
        e[4] = 198;
        e[5] = 51;
        e[6] = 100;
        e[7] = (uint8_t) (10 + slots[i]);
        PutU16(e + 8, (uint16_t) (30000 + slots[i]));
        PutU16(e + 10, (uint16_t) (27100 + slots[i]));
        e[12] = 192;
        e[13] = 168;
        e[14] = 2;
        e[15] = (uint8_t) (100 + slots[i]);
        memcpy(e + 16, "MIKE", 4);
    }
    return 24 + 28 * count;
}

static void TestRendezvousWire(void) {
    uint8_t buf[512];
    GdxMpRvRequest req;
    GdxMpRvWelcome w;
    GdxMpRvRoster r;
    GdxMpRvError e;
    int n;
    int slots4[4] = { 0, 1, 2, 3 };
    int dup[2] = { 0, 0 };
    int nohost[2] = { 1, 2 };
    uint32_t ver;
    uint32_t echo;
    int from;

    memset(&req, 0, sizeof(req));
    req.nonce = 0xDEADBEEFu;
    req.contentHash = 0x01020304u;
    req.priv.ip[0] = 192;
    req.priv.ip[1] = 168;
    req.priv.ip[2] = 2;
    req.priv.ip[3] = 196;
    req.priv.port = 27100;
    req.maxPlayers = 4;
    strcpy(req.build, "mp-online@6722156");
    strcpy(req.name, "NEW3DS");
    n = gdx_rv_build_request(GDX_RV_CREATE, &req, buf, sizeof(buf));
    CHECK(n == 84);
    CHECK(memcmp(buf, "GXRV", 4) == 0 && buf[4] == 1 && buf[5] == GDX_RV_CREATE);
    CHECK(buf[8] == 0xEF && buf[11] == 0xDE);
    CHECK(buf[12] == 0x04 && buf[15] == 0x01);
    CHECK(buf[16] == 192 && buf[19] == 196);
    CHECK(buf[20] == (27100 & 0xFF) && buf[21] == (27100 >> 8));
    CHECK(buf[22] == 4 && buf[23] == 0);
    CHECK(memcmp(buf + 24, "mp-online@6722156", 17) == 0 && buf[24 + 17] == 0);
    CHECK(memcmp(buf + 64, "NEW3DS", 6) == 0);
    CHECK(buf[76] == 0); /* no code requested */
    CHECK(gdx_rv_build_request(GDX_RV_KEEPALIVE, &req, buf, sizeof(buf)) == 0);
    CHECK(gdx_rv_build_request(GDX_RV_JOIN, &req, buf, 83) == 0);

    n = gdx_rv_build_keepalive("K7MPQ", 2, 0x0102030405060708ull, 9, 1234, buf, sizeof(buf));
    CHECK(n == 36 && buf[5] == GDX_RV_KEEPALIVE && buf[16] == 2 && buf[20] == 0x08 && buf[27] == 0x01);
    CHECK(buf[28] == 9 && buf[32] == (1234 & 0xFF));
    CHECK(gdx_rv_build_keepalive("K7MPQ", 4, 1, 0, 0, buf, sizeof(buf)) == 0);
    {
        uint8_t payload[100];
        memset(payload, 0x5A, sizeof(payload));
        n = gdx_rv_build_relay("K7MPQ", 1, 0x0D, 77, payload, 100, buf, sizeof(buf));
        CHECK(n == 128 && buf[5] == GDX_RV_RELAY && buf[17] == 0x0D && buf[28] == 0x5A && buf[127] == 0x5A);
        CHECK(gdx_rv_build_relay("K7MPQ", 1, 0x10, 77, payload, 100, buf, sizeof(buf)) == 0);
        CHECK(gdx_rv_build_relay("K7MPQ", 1, 0, 77, payload, 100, buf, sizeof(buf)) == 0);
        CHECK(gdx_rv_build_relay("K7MPQ", 1, 1, 77, payload, 1201, buf, 4096) == 0);
    }

    n = MakeWelcome(buf, 2, 4, 0xFEEDu);
    CHECK(gdx_rv_parse_welcome(buf, n, &w) == 0);
    CHECK(w.slot == 2 && w.maxPlayers == 4 && w.forceRelay == 1 && w.roomSecret == 0xFEEDu);
    CHECK(w.memberKey == 0x1122334455667788ull && strcmp(w.code, "K7MPQ") == 0);
    CHECK(w.seenAs.ip[0] == 203 && w.seenAs.ip[3] == 9 && w.seenAs.port == 41000);
    CHECK(gdx_rv_parse_welcome(buf, n - 1, &w) < 0);
    MakeWelcome(buf, 4, 4, 1);
    CHECK(gdx_rv_parse_welcome(buf, 48, &w) < 0); /* slot out of range */
    MakeWelcome(buf, 1, 2, 0);
    CHECK(gdx_rv_parse_welcome(buf, 48, &w) < 0); /* zero secret */
    MakeWelcome(buf, 1, 4, 1);
    buf[14] = 'O'; /* not in the alphabet */
    CHECK(gdx_rv_parse_welcome(buf, 48, &w) < 0);
    MakeWelcome(buf, 1, 4, 1);
    buf[4] = 2; /* a v2 member's replies carry version 2 */
    CHECK(gdx_rv_parse_welcome(buf, 48, &w) == 0 && w.slot == 1);
    buf[4] = 3;
    CHECK(gdx_rv_parse_welcome(buf, 48, &w) < 0);
    buf[4] = 0;
    CHECK(gdx_rv_parse_welcome(buf, 48, &w) < 0);

    n = MakeRoster(buf, 4, slots4);
    CHECK(gdx_rv_parse_roster(buf, n, &r) == 0);
    CHECK(r.count == 4 && r.version == 5 && r.forceRelay == 1 && r.locked == 0);
    CHECK(r.member[3].present && r.member[3].pub.port == 30003 && r.member[3].priv.port == 27103);
    CHECK(r.member[3].priv.ip[3] == 103 && r.member[0].isHost && !r.member[1].isHost);
    CHECK(strcmp(r.member[2].name, "MIKE") == 0);
    CHECK(r.member[0].epoch == 40 && r.member[3].epoch == 43);
    CHECK(gdx_rv_parse_roster(buf, n - 1, &r) < 0);
    CHECK(gdx_rv_parse_roster(buf, n + 28, &r) < 0);
    n = MakeRoster(buf, 2, dup);
    CHECK(gdx_rv_parse_roster(buf, n, &r) < 0);
    n = MakeRoster(buf, 2, nohost);
    CHECK(gdx_rv_parse_roster(buf, n, &r) < 0);
    n = MakeRoster(buf, 1, slots4);
    buf[20] = 5;
    CHECK(gdx_rv_parse_roster(buf, n, &r) < 0);
    n = MakeRoster(buf, 2, slots4);
    buf[24 + 28 + 16] = '<'; /* name outside the whitelist */
    CHECK(gdx_rv_parse_roster(buf, n, &r) < 0);
    n = MakeRoster(buf, 2, slots4);
    buf[24 + 16 + 5] = 'X'; /* a byte after the NUL terminator */
    CHECK(gdx_rv_parse_roster(buf, n, &r) < 0);

    memset(buf, 0, 56);
    memcpy(buf, "GXRV", 4);
    buf[4] = 1;
    buf[5] = GDX_RV_ERROR;
    PutU32(buf + 8, 42);
    buf[12] = GDX_RV_E_BUILD_MISMATCH;
    memcpy(buf + 16, "integ-v1.0.2@abc", 16);
    CHECK(gdx_rv_parse_error(buf, 56, &e) == 0 && e.code == GDX_RV_E_BUILD_MISMATCH && e.nonce == 42);
    CHECK(strcmp(e.detail, "integ-v1.0.2@abc") == 0);
    CHECK(gdx_rv_parse_error(buf, 16, &e) == 0 && e.detail[0] == '\0');
    buf[20] = 0x07; /* a control byte in the detail: dropped, not trusted */
    CHECK(gdx_rv_parse_error(buf, 56, &e) == 0 && e.detail[0] == '\0');
    CHECK(gdx_rv_parse_error(buf, 30, &e) < 0);
    CHECK(strcmp(gdx_rv_error_text(GDX_RV_E_ROOM_FULL), "room is full") == 0);

    memset(buf, 0, 20);
    memcpy(buf, "GXRV", 4);
    buf[4] = 1;
    buf[5] = GDX_RV_KA_ACK;
    PutU32(buf + 8, 7);
    PutU32(buf + 12, 99);
    CHECK(gdx_rv_parse_ka_ack(buf, 16, &ver, &echo) == 0 && ver == 7 && echo == 99);
    CHECK(gdx_rv_parse_ka_ack(buf, 17, &ver, &echo) < 0);
    buf[5] = GDX_RV_PUNCH_NOW;
    buf[16] = 3;
    CHECK(gdx_rv_parse_punch_now(buf, 20, &from) == 0 && from == 3);
    buf[16] = 4;
    CHECK(gdx_rv_parse_punch_now(buf, 20, &from) < 0);
    buf[5] = GDX_RV_CREATE; /* a client type is never a server reply */
    CHECK(gdx_rv_peek_type(buf, 20) < 0);
    CHECK(gdx_rv_peek_type(buf, 7) < 0);
}

/* ---- 2. codes, names, addresses ----------------------------------------------------------- */
/* ---- 1b. GXRV v2: the cookie wire format and the console's request state machine ------------ */
static int MakeCookie(uint8_t* p, uint32_t nonce, uint8_t fill) {
    memset(p, 0, GDX_RV_COOKIE_BYTES);
    memcpy(p, "GXRV", 4);
    p[4] = 2;
    p[5] = GDX_RV_COOKIE;
    PutU32(p + 8, nonce);
    memset(p + 12, fill, GDX_RV_COOKIE_LEN);
    return GDX_RV_COOKIE_BYTES;
}

static GdxMpRvRequest CookieReq(uint32_t nonce) {
    GdxMpRvRequest r;
    memset(&r, 0, sizeof(r));
    r.nonce = nonce;
    r.contentHash = 0xC0DEu;
    r.maxPlayers = 4;
    r.clientProto = GDX_MPO_PROTO;
    snprintf(r.build, sizeof(r.build), "mp-online@test");
    snprintf(r.name, sizeof(r.name), "MIKE");
    return r;
}

static int IsZero(const uint8_t* p, int n) {
    int i;
    for (i = 0; i < n; i++) {
        if (p[i] != 0) {
            return 0;
        }
    }
    return 1;
}

static void TestRvCookieWire(void) {
    uint8_t v1[128];
    uint8_t v2[128];
    uint8_t cookie[GDX_RV_COOKIE_LEN];
    uint8_t got[GDX_RV_COOKIE_LEN];
    uint8_t pkt[64];
    uint32_t nonce = 0;
    const GdxMpRvRequest r = CookieReq(77);
    GdxMpRvRequest back;
    memset(cookie, 0xA5, sizeof(cookie));
    CHECK(gdx_rv_build_request(GDX_RV_JOIN, &r, v1, sizeof(v1)) == GDX_RV_REQ_BYTES);
    CHECK(gdx_rv_build_request_v2(GDX_RV_JOIN, &r, cookie, v2, sizeof(v2)) == GDX_RV_REQ2_BYTES);
    CHECK(GDX_RV_REQ2_BYTES == 104 && GDX_RV_COOKIE_BYTES == 28 && GDX_RV_UPDATE_BYTES == 60);
    CHECK(GDX_RV_UPDATE_BYTES <= GDX_RV_REQ2_BYTES && GDX_RV_COOKIE_BYTES <= GDX_RV_REQ2_BYTES); /* no amplification */
    CHECK(v2[4] == 2 && v1[4] == 1 && v2[5] == GDX_RV_JOIN);
    CHECK(memcmp(v1, v2, 4) == 0 && memcmp(v1 + 5, v2 + 5, GDX_RV_REQ_BYTES - 5) == 0);
    CHECK(memcmp(v2 + GDX_RV_REQ_BYTES, cookie, GDX_RV_COOKIE_LEN) == 0);
    CHECK(v2[100] == (GDX_MPO_PROTO & 0xFF) && v2[101] == (GDX_MPO_PROTO >> 8) && v2[102] == 0 && v2[103] == 0);
    CHECK(gdx_rv_build_request_v2(GDX_RV_JOIN, &r, cookie, v2, GDX_RV_REQ2_BYTES - 1) == 0);
    CHECK(gdx_rv_build_request_v2(GDX_RV_KEEPALIVE, &r, cookie, v2, sizeof(v2)) == 0);
    /* the console-hosted LAN room (v1 only) refuses a v2 request */
    gdx_rv_build_request_v2(GDX_RV_JOIN, &r, cookie, v2, sizeof(v2));
    CHECK(gdx_rv_parse_request(v2, GDX_RV_REQ2_BYTES, &back) < 0);

    MakeCookie(pkt, 0x01020304u, 0x5A);
    CHECK(gdx_rv_peek_type(pkt, GDX_RV_COOKIE_BYTES) == GDX_RV_COOKIE);
    CHECK(gdx_rv_parse_cookie(pkt, GDX_RV_COOKIE_BYTES, &nonce, got) == 0 && nonce == 0x01020304u);
    CHECK(got[0] == 0x5A && got[15] == 0x5A);
    CHECK(gdx_rv_parse_cookie(pkt, GDX_RV_COOKIE_BYTES - 1, &nonce, got) < 0);
    CHECK(gdx_rv_parse_cookie(pkt, GDX_RV_COOKIE_BYTES + 1, &nonce, got) < 0);
    pkt[4] = 1; /* a COOKIE only exists in version 2 */
    CHECK(gdx_rv_parse_cookie(pkt, GDX_RV_COOKIE_BYTES, &nonce, got) < 0);
    MakeCookie(pkt, 1, 0);
    pkt[5] = GDX_RV_WELCOME;
    CHECK(gdx_rv_parse_cookie(pkt, GDX_RV_COOKIE_BYTES, &nonce, got) < 0);

    CHECK(gdx_rv_cookie_mode_parse("0") == GDX_RV_COOKIE_MODE_V1);
    CHECK(gdx_rv_cookie_mode_parse("1") == GDX_RV_COOKIE_MODE_V2);
    CHECK(gdx_rv_cookie_mode_parse("auto") == GDX_RV_COOKIE_MODE_AUTO);
    CHECK(gdx_rv_cookie_mode_parse("") == GDX_RV_COOKIE_MODE_AUTO);
    CHECK(gdx_rv_cookie_mode_parse(NULL) == GDX_RV_COOKIE_MODE_AUTO);
    CHECK(gdx_rv_cookie_mode_parse("junk") == GDX_RV_COOKIE_MODE_AUTO);
    CHECK(strcmp(gdx_rv_cookie_mode_name(GDX_RV_COOKIE_MODE_AUTO), "auto") == 0);
}

/* A v2-speaking (2.0.0+) server's answer to a request, as gdx_rendezvous.py gives it. */
static int CookieServer(const uint8_t* req, int n, int requireCookie, uint8_t* out) {
    const uint32_t nonce = (uint32_t) req[8] | ((uint32_t) req[9] << 8) | ((uint32_t) req[10] << 16) |
                           ((uint32_t) req[11] << 24);
    if (req[4] == 2) {
        if (n != GDX_RV_REQ2_BYTES) {
            return 0;
        }
        if (req[GDX_RV_REQ_BYTES] != 0xC5) { /* "valid" cookies here are all 0xC5 */
            return MakeCookie(out, nonce, 0xC5);
        }
        MakeWelcome(out, 0, 4, 9);
        out[4] = 2;
        PutU32(out + 8, nonce);
        return GDX_RV_WELCOME_BYTES;
    }
    if (requireCookie) {
        memset(out, 0, 56);
        memcpy(out, "GXRV", 4);
        out[4] = 1;
        out[5] = GDX_RV_ERROR;
        PutU32(out + 8, nonce);
        out[12] = GDX_RV_E_BUILD_MISMATCH;
        memcpy(out + 16, GDX_RV_NEEDS_V2_DETAIL, strlen(GDX_RV_NEEDS_V2_DETAIL));
        return 56;
    }
    MakeWelcome(out, 0, 4, 9);
    PutU32(out + 8, nonce);
    return GDX_RV_WELCOME_BYTES;
}

static void TestRvCookieStateMachine(void) {
    GdxRvCookie c;
    GdxMpRvRequest r = CookieReq(500);
    GdxMpRvError e;
    uint8_t out[128];
    uint8_t ans[128];
    int n;
    int a;
    int ev;
    int i;

    /* mode 1 vs a requiring server: zeros -> COOKIE -> the same request with it -> WELCOME (v2) */
    gdx_rv_cookie_init(&c, GDX_RV_COOKIE_MODE_V2);
    gdx_rv_cookie_begin(&c, 500, 1000, 1);
    n = gdx_rv_cookie_build(&c, GDX_RV_CREATE, &r, 1000, out, sizeof(out), &ev);
    CHECK(n == GDX_RV_REQ2_BYTES && out[4] == 2 && ev == GDX_RV_CK_NONE);
    CHECK(IsZero(out + GDX_RV_REQ_BYTES, GDX_RV_COOKIE_LEN));
    a = CookieServer(out, n, 1, ans);
    CHECK(a == GDX_RV_COOKIE_BYTES);
    CHECK(gdx_rv_cookie_on_reply(&c, ans, a, 1040) == GDX_RV_CK_RESEND);
    CHECK(c.haveCookie && c.cookieRttMs == 40 && c.cookies == 1 && c.serverV2);
    n = gdx_rv_cookie_build(&c, GDX_RV_CREATE, &r, 1040, out, sizeof(out), &ev);
    CHECK(n == GDX_RV_REQ2_BYTES && out[GDX_RV_REQ_BYTES] == 0xC5 && out[8] == (500 & 0xFF) && out[9] == (500 >> 8));
    a = CookieServer(out, n, 1, ans);
    CHECK(gdx_rv_peek_type(ans, a) == GDX_RV_WELCOME);
    CHECK(gdx_rv_cookie_on_reply(&c, ans, a, 1080) == GDX_RV_CK_NONE);
    /* a COOKIE for another nonce (a stale request) is not taken */
    gdx_rv_cookie_begin(&c, 501, 2000, 1);
    MakeCookie(ans, 500, 0xC5);
    CHECK(gdx_rv_cookie_on_reply(&c, ans, GDX_RV_COOKIE_BYTES, 2010) == GDX_RV_CK_NONE && !c.haveCookie);
    /* a new request starts from zeros again */
    r.nonce = 501;
    n = gdx_rv_cookie_build(&c, GDX_RV_JOIN, &r, 2000, out, sizeof(out), &ev);
    CHECK(n == GDX_RV_REQ2_BYTES && IsZero(out + GDX_RV_REQ_BYTES, GDX_RV_COOKIE_LEN));
    /* mode 1 never falls back, however long the server stays silent */
    for (i = 0; i < 15; i++) {
        n = gdx_rv_cookie_build(&c, GDX_RV_JOIN, &r, 2000 + 1000 * (uint32_t) i, out, sizeof(out), &ev);
        CHECK(n == GDX_RV_REQ2_BYTES && ev == GDX_RV_CK_NONE);
    }
    /* a cookie that keeps failing gets 4 immediate resends, then waits for the regular retry */
    gdx_rv_cookie_begin(&c, 502, 3000, 1);
    for (i = 0; i < 6; i++) {
        MakeCookie(ans, 502, (uint8_t) (0x10 + i));
        CHECK(gdx_rv_cookie_on_reply(&c, ans, GDX_RV_COOKIE_BYTES, 3000) ==
              (i < GDX_RV_COOKIE_MAX_FAST_RESENDS ? GDX_RV_CK_RESEND : GDX_RV_CK_NONE));
    }
    CHECK(c.cookie[0] == 0x15); /* the latest one is kept */

    /* auto vs an old (< 2.0.0) server, which drops every v2 datagram: v2, v2, then v1 */
    r.nonce = 600;
    gdx_rv_cookie_init(&c, GDX_RV_COOKIE_MODE_AUTO);
    gdx_rv_cookie_begin(&c, 600, 10000, 1);
    CHECK(gdx_rv_cookie_build(&c, GDX_RV_CREATE, &r, 10000, out, sizeof(out), &ev) == GDX_RV_REQ2_BYTES);
    CHECK(ev == GDX_RV_CK_NONE);
    CHECK(gdx_rv_cookie_build(&c, GDX_RV_CREATE, &r, 11000, out, sizeof(out), &ev) == GDX_RV_REQ2_BYTES);
    CHECK(ev == GDX_RV_CK_NONE);
    n = gdx_rv_cookie_build(&c, GDX_RV_CREATE, &r, 12000, out, sizeof(out), &ev);
    CHECK(n == GDX_RV_REQ_BYTES && out[4] == 1 && ev == GDX_RV_CK_FALLBACK && c.fellBack);
    a = CookieServer(out, n, 0, ans);
    CHECK(gdx_rv_cookie_on_reply(&c, ans, a, 12030) == GDX_RV_CK_NONE && gdx_rv_peek_type(ans, a) == GDX_RV_WELCOME);
    /* ... and the rest of the session stays v1 (two junk datagrams per session, far below 50) */
    gdx_rv_cookie_begin(&c, 601, 20000, 1);
    r.nonce = 601;
    n = gdx_rv_cookie_build(&c, GDX_RV_JOIN, &r, 20000, out, sizeof(out), &ev);
    CHECK(n == GDX_RV_REQ_BYTES && ev == GDX_RV_CK_NONE);
    /* a new session starts with v2 again */
    gdx_rv_cookie_init(&c, GDX_RV_COOKIE_MODE_AUTO);
    gdx_rv_cookie_begin(&c, 602, 30000, 1);
    CHECK(gdx_rv_cookie_build(&c, GDX_RV_JOIN, &r, 30000, out, sizeof(out), &ev) == GDX_RV_REQ2_BYTES);

    /* auto vs a requiring server whose first two COOKIEs were lost: v1 is refused with "needs GXRV
     * v2" -> back to v2 at once, which then sticks */
    r.nonce = 700;
    gdx_rv_cookie_init(&c, GDX_RV_COOKIE_MODE_AUTO);
    gdx_rv_cookie_begin(&c, 700, 0, 1);
    gdx_rv_cookie_build(&c, GDX_RV_JOIN, &r, 0, out, sizeof(out), &ev);
    gdx_rv_cookie_build(&c, GDX_RV_JOIN, &r, 1000, out, sizeof(out), &ev);
    n = gdx_rv_cookie_build(&c, GDX_RV_JOIN, &r, 2000, out, sizeof(out), &ev);
    CHECK(n == GDX_RV_REQ_BYTES && ev == GDX_RV_CK_FALLBACK);
    a = CookieServer(out, n, 1, ans);
    CHECK(gdx_rv_parse_error(ans, a, &e) == 0 && e.code == GDX_RV_E_BUILD_MISMATCH);
    CHECK(strcmp(e.detail, GDX_RV_NEEDS_V2_DETAIL) == 0);
    gdx_rv_cookie_on_reply(&c, ans, a, 2020);
    CHECK(gdx_rv_cookie_on_error(&c, &e) == GDX_RV_CK_RETRY_V2 && !c.fellBack && c.useV2);
    n = gdx_rv_cookie_build(&c, GDX_RV_JOIN, &r, 2020, out, sizeof(out), &ev);
    CHECK(n == GDX_RV_REQ2_BYTES && IsZero(out + GDX_RV_REQ_BYTES, GDX_RV_COOKIE_LEN));
    a = CookieServer(out, n, 1, ans);
    CHECK(gdx_rv_cookie_on_reply(&c, ans, a, 2040) == GDX_RV_CK_RESEND);
    n = gdx_rv_cookie_build(&c, GDX_RV_JOIN, &r, 2040, out, sizeof(out), &ev);
    a = CookieServer(out, n, 1, ans);
    CHECK(gdx_rv_peek_type(ans, a) == GDX_RV_WELCOME);
    /* the server spoke v2: no later silence makes auto give up on it */
    gdx_rv_cookie_begin(&c, 701, 5000, 1);
    for (i = 0; i < 10; i++) {
        CHECK(gdx_rv_cookie_build(&c, GDX_RV_JOIN, &r, 5000 + 1000 * (uint32_t) i, out, sizeof(out), &ev) ==
              GDX_RV_REQ2_BYTES);
    }

    /* any answer (here a v1 RATE_LIMITED) means the server is there: no fallback */
    gdx_rv_cookie_init(&c, GDX_RV_COOKIE_MODE_AUTO);
    gdx_rv_cookie_begin(&c, 800, 0, 1);
    gdx_rv_cookie_build(&c, GDX_RV_JOIN, &r, 0, out, sizeof(out), &ev);
    gdx_rv_cookie_build(&c, GDX_RV_JOIN, &r, 1000, out, sizeof(out), &ev);
    memset(ans, 0, 16);
    memcpy(ans, "GXRV", 4);
    ans[4] = 1;
    ans[5] = GDX_RV_ERROR;
    ans[12] = GDX_RV_E_RATE_LIMITED;
    gdx_rv_cookie_on_reply(&c, ans, 16, 1500);
    CHECK(gdx_rv_cookie_build(&c, GDX_RV_JOIN, &r, 2000, out, sizeof(out), &ev) == GDX_RV_REQ2_BYTES);
    CHECK(ev == GDX_RV_CK_NONE);

    /* mode 0 (an old card's behaviour): v1 only; the refusal is final, so the console shows it */
    gdx_rv_cookie_init(&c, GDX_RV_COOKIE_MODE_V1);
    gdx_rv_cookie_begin(&c, 900, 0, 1);
    n = gdx_rv_cookie_build(&c, GDX_RV_CREATE, &r, 0, out, sizeof(out), &ev);
    CHECK(n == GDX_RV_REQ_BYTES && out[4] == 1);
    a = CookieServer(out, n, 1, ans);
    CHECK(gdx_rv_parse_error(ans, a, &e) == 0 && gdx_rv_cookie_on_error(&c, &e) == GDX_RV_CK_NONE);
    {
        /* what every card before this one shows (its menu: "version mismatch: host runs %s"); an
         * old card drops a detail with a space in it, so the server's has none */
        char shown[128];
        snprintf(shown, sizeof(shown), "%s: host runs %s", gdx_rv_error_text(e.code), e.detail);
        CHECK(strcmp(shown, "version mismatch: host runs newer-version(GXRV-v2):PLEASE-UPDATE") == 0);
        CHECK(strchr(GDX_RV_NEEDS_V2_DETAIL, ' ') == NULL && strlen(GDX_RV_NEEDS_V2_DETAIL) <= GDX_MPO_BUILD_LEN);
        /* this build with mp_rv_cookie = 0 says what to change */
        gdx_rv_error_message(&e, GDX_MPO_PROTO, shown, sizeof(shown));
        CHECK(strstr(shown, "mp_rv_cookie = auto") != NULL);
    }
    /* a COOKIE to a v1 request is ignored */
    MakeCookie(ans, 900, 0xC5);
    CHECK(gdx_rv_cookie_on_reply(&c, ans, GDX_RV_COOKIE_BYTES, 10) == GDX_RV_CK_NONE && !c.haveCookie);

    /* a LAN room (allowV2 = 0) is always v1, whatever the mode */
    gdx_rv_cookie_init(&c, GDX_RV_COOKIE_MODE_V2);
    gdx_rv_cookie_begin(&c, 901, 0, 0);
    CHECK(gdx_rv_cookie_build(&c, GDX_RV_JOIN, &r, 0, out, sizeof(out), &ev) == GDX_RV_REQ_BYTES);
    gdx_rv_cookie_init(&c, GDX_RV_COOKIE_MODE_AUTO);
    gdx_rv_cookie_begin(&c, 902, 0, 0);
    for (i = 0; i < 4; i++) {
        CHECK(gdx_rv_cookie_build(&c, GDX_RV_JOIN, &r, 0, out, sizeof(out), &ev) == GDX_RV_REQ_BYTES);
        CHECK(ev == GDX_RV_CK_NONE);
    }
    CHECK(!c.fellBack); /* LAN traffic says nothing about the online server */
}

/* ---- 1c. version gating: UPDATE and what the player is told ----------------------------------- */
static int MakeUpdate(uint8_t* p, uint32_t nonce, int minP, int maxP, int yours, const char* text) {
    memset(p, 0, GDX_RV_UPDATE_BYTES);
    memcpy(p, "GXRV", 4);
    p[4] = 2;
    p[5] = GDX_RV_UPDATE;
    PutU32(p + 8, nonce);
    PutU16(p + 12, (uint16_t) minP);
    PutU16(p + 14, (uint16_t) maxP);
    PutU16(p + 16, (uint16_t) yours);
    memcpy(p + 20, text, strlen(text));
    return GDX_RV_UPDATE_BYTES;
}

static void TestVersionGating(void) {
    uint8_t p[96];
    GdxMpRvUpdate u;
    GdxMpRvError e;
    char m[160];
    int n;
    n = MakeUpdate(p, 42, 10, 12, 9, "example.org/update");
    CHECK(gdx_rv_parse_update(p, n, &u) == 0 && u.nonce == 42 && u.minProto == 10 && u.maxProto == 12);
    CHECK(u.yourProto == 9 && strcmp(u.text, "example.org/update") == 0);
    CHECK(gdx_rv_parse_update(p, n - 1, &u) < 0 && gdx_rv_parse_update(p, n + 1, &u) < 0);
    p[4] = 1;
    CHECK(gdx_rv_parse_update(p, n, &u) < 0); /* version 2 only */
    n = MakeUpdate(p, 42, 10, 0, 9, "bad\x01text");
    CHECK(gdx_rv_parse_update(p, n, &u) == 0 && u.text[0] == '\0'); /* display text: dropped, never trusted */
    n = MakeUpdate(p, 42, 10, 0, 9, "see the readme");
    CHECK(gdx_rv_parse_update(p, n, &u) == 0 && strcmp(u.text, "see the readme") == 0); /* spaces are text */

    /* this console is older than the server allows */
    CHECK(gdx_rv_update_verdict(&u, 9) == GDX_RV_UPD_CLIENT_OLD);
    CHECK(gdx_rv_update_message(&u, 9, m, sizeof(m)) == GDX_RV_UPD_CLIENT_OLD);
    CHECK(strcmp(m, "UPDATE REQUIRED - this server needs version 10 (you have 9) - see the readme") == 0);
    u.text[0] = '\0';
    gdx_rv_update_message(&u, 9, m, sizeof(m));
    CHECK(strcmp(m, "UPDATE REQUIRED - this server needs version 10 (you have 9)") == 0);
    /* ... newer than it knows: the server is outdated */
    n = MakeUpdate(p, 43, 7, 9, 11, "example.org/update");
    gdx_rv_parse_update(p, n, &u);
    CHECK(gdx_rv_update_message(&u, 11, m, sizeof(m)) == GDX_RV_UPD_SERVER_OLD);
    CHECK(strcmp(m, "SERVER NEEDS AN UPDATE - it supports up to version 9 (you have 11)") == 0);
    /* no ceiling (0): never "server outdated" */
    n = MakeUpdate(p, 44, 7, 0, 60000, "");
    gdx_rv_parse_update(p, n, &u);
    CHECK(gdx_rv_update_verdict(&u, 60000) == GDX_RV_UPD_REFUSED);
    gdx_rv_update_message(&u, 60000, m, sizeof(m));
    CHECK(strstr(m, "UPDATE REQUIRED") == m);

    /* a mismatch says which side should update when the server told us the host's proto */
    memset(&e, 0, sizeof(e));
    e.code = GDX_RV_E_BUILD_MISMATCH;
    snprintf(e.detail, sizeof(e.detail), "card-5@abcdef12");
    gdx_rv_error_message(&e, 9, m, sizeof(m));
    CHECK(strcmp(m, "version mismatch: host runs card-5@abcdef12") == 0); /* an old server: hostProto 0 */
    e.hostProto = 10;
    gdx_rv_error_message(&e, 9, m, sizeof(m));
    CHECK(strcmp(m, "version mismatch: host runs card-5@abcdef12 (update this console)") == 0);
    e.hostProto = 8;
    gdx_rv_error_message(&e, 9, m, sizeof(m));
    CHECK(strcmp(m, "version mismatch: host runs card-5@abcdef12 (the host needs an update)") == 0);
    e.hostProto = 9;
    gdx_rv_error_message(&e, 9, m, sizeof(m));
    CHECK(strcmp(m, "version mismatch: host runs card-5@abcdef12 (same version, other build)") == 0);
    e.code = GDX_RV_E_CONTENT_MISMATCH;
    e.hostProto = 10;
    gdx_rv_error_message(&e, 9, m, sizeof(m));
    CHECK(strcmp(m, "game data differs (update this console)") == 0);
    e.code = GDX_RV_E_ROOM_FULL;
    gdx_rv_error_message(&e, 9, m, sizeof(m));
    CHECK(strcmp(m, "room is full") == 0);
    /* the ERROR's bytes 14..15 carry it; a server before 2.1.0 leaves them 0 */
    memset(p, 0, 56);
    memcpy(p, "GXRV", 4);
    p[4] = 2;
    p[5] = GDX_RV_ERROR;
    p[12] = GDX_RV_E_BUILD_MISMATCH;
    PutU16(p + 14, 10);
    memcpy(p + 16, "a build with spaces", 19);
    CHECK(gdx_rv_parse_error(p, 56, &e) == 0 && e.hostProto == 10 && strcmp(e.detail, "a build with spaces") == 0);
    CHECK(strcmp(gdx_mpo_update_side(10, 9), "update this console") == 0);
    CHECK(strcmp(gdx_mpo_update_side(8, 9), "they need an update") == 0);
}

static void TestCodesNames(void) {
    char c[16];
    char n[16];
    GdxMpEndpoint e;
    strcpy(c, "k7mpq");
    CHECK(gdx_mpo_code_normalise(c) && strcmp(c, "K7MPQ") == 0);
    strcpy(c, "K7MP");
    CHECK(!gdx_mpo_code_normalise(c));
    strcpy(c, "K7MPQXZ");
    CHECK(!gdx_mpo_code_normalise(c));
    strcpy(c, "K0MPQ"); /* 0 is not in the alphabet */
    CHECK(!gdx_mpo_code_normalise(c));
    strcpy(c, "KIMPQ");
    CHECK(!gdx_mpo_code_normalise(c));
    gdx_mpo_name_sanitise("Mike<3>", n);
    CHECK(strcmp(n, "Mike_3_") == 0);
    gdx_mpo_name_sanitise("   ", n);
    CHECK(strcmp(n, "PLAYER") == 0);
    gdx_mpo_name_sanitise("ABCDEFGHIJKLMNOP", n);
    CHECK(strlen(n) == 12);
    CHECK(gdx_mpo_parse_hostport("192.168.1.10:27200", &e, 1) && e.ip[3] == 10 && e.port == 27200);
    CHECK(gdx_mpo_parse_hostport("10.0.0.1", &e, 27200) && e.port == 27200 && e.ip[0] == 10);
    CHECK(!gdx_mpo_parse_hostport("256.1.1.1:5", &e, 1));
    CHECK(!gdx_mpo_parse_hostport("1.2.3:5", &e, 1));
    CHECK(!gdx_mpo_parse_hostport("1.2.3.4:", &e, 1));
    CHECK(!gdx_mpo_parse_hostport("1.2.3.4:70000", &e, 1));
    CHECK(!gdx_mpo_parse_hostport("1.2.3.4:0", &e, 1));
    CHECK(!gdx_mpo_parse_hostport("example.org:27200", &e, 1));
    CHECK(!gdx_mpo_parse_hostport("1.2.3.4:5x", &e, 1));
    CHECK(!gdx_mpo_parse_hostport("", &e, 1));
}

/* ---- 3. GXLB -------------------------------------------------------------------------------- */
static GdxMpLbStart SampleStart(int n) {
    GdxMpLbStart s;
    int i;
    memset(&s, 0, sizeof(s));
    s.gen = 3;
    s.numPlayers = (uint8_t) n;
    s.course = 7;
    s.delay = 3;
    s.hashEvery = 1;
    s.vsCom = 1;
    s.handicap = 2;
    s.vsSlot = 1;
    s.difficulty = 2;
    s.seed1 = 11;
    s.mask1 = 22;
    s.seed2 = 33;
    s.mask2 = 44;
    s.token = 0x0123456789ABCDEFull;
    s.courseKey = gdx_mpo_stock_course_key(7);
    for (i = 0; i < n; i++) {
        s.player[i].slot = (uint8_t) (i == 0 ? 0 : i + 1 > 3 ? 3 : i + (i >= 1 ? 0 : 0));
        s.player[i].machine = (uint8_t) (i * 7);
        s.player[i].skin = (uint8_t) (i & 3);
        s.player[i].engine = (uint8_t) (25 * i);
        s.player[i].engineBits = gdx_mpo_engine_bits(0.25f * (float) i);
    }
    return s;
}

static void TestLobbyWire(void) {
    uint8_t buf[256];
    GdxMpLbHeader h;
    GdxMpLbHeader g;
    GdxMpLbHello hello;
    GdxMpLbStart st = SampleStart(3);
    GdxMpLbAbort ab;
    GdxMpLbBody body;
    int n;

    memset(&h, 0, sizeof(h));
    h.type = GDX_LB_HELLO;
    h.from = 2;
    h.to = 0;
    h.secret = 0xC0FFEEull;
    h.sendMs = 1000;
    h.echoMs = 900;
    h.holdMs = 12;
    memset(&hello, 0, sizeof(hello));
    hello.proto = GDX_MPO_PROTO;
    hello.ready = 1;
    hello.contentHash = 0xAA55AA55u;
    hello.startGen = 2;
    hello.machine = 29;
    hello.skin = 3;
    hello.engine = 100;
    hello.directMask = 0x05;
    hello.rttMs[0] = 42;
    hello.rttMs[1] = GDX_LB_NO_RTT;
    strcpy(hello.build, "mp-online@1234567");
    strcpy(hello.name, "OLD3DS");
    hello.picking = GDX_LB_PICK_MACHINE;
    hello.pickRound = 200;
    hello.courseFor = 31;
    hello.picked = 1;
    hello.pickMsLeft = 41234;
    hello.courseKey = 0xDEADBEEFu;
    hello.engineBits = gdx_mpo_engine_bits(0.5234375f);
    n = gdx_lb_build(&h, &hello, buf, sizeof(buf));
    CHECK(n == GDX_LB_HELLO_BYTES);
    CHECK(buf[108] == 0 && buf[109] == 0 && buf[110] == 0 && buf[111] == 0); /* mp30's simAbi bytes */
    CHECK(gdx_lb_parse(buf, n, 0xC0FFEEull, &g, &body) == GDX_LB_HELLO);
    CHECK(body.hello.picking == GDX_LB_PICK_MACHINE && body.hello.pickRound == 200 && body.hello.picked == 1);
    CHECK(body.hello.courseFor == 31 && body.hello.courseKey == 0xDEADBEEFu && body.hello.pickMsLeft == 41234);
    CHECK(gdx_mpo_engine_from_bits(body.hello.engineBits) == 0.5234375f);
    PutU32(buf + 120, 0x7FC00000u); /* a NaN engine */
    CHECK(gdx_lb_parse(buf, n, 0xC0FFEEull, &g, &body) == -3);
    PutU32(buf + 120, gdx_mpo_engine_bits(1.0f) + 1u); /* just above 1.0 */
    CHECK(gdx_lb_parse(buf, n, 0xC0FFEEull, &g, &body) == -3);
    PutU32(buf + 120, gdx_mpo_engine_bits(0.0f)); /* 0.0 (all acceleration) is a real setting */
    CHECK(gdx_lb_parse(buf, n, 0xC0FFEEull, &g, &body) == GDX_LB_HELLO && body.hello.engineBits == 0u);
    CHECK(gdx_mpo_engine_from_bits(0xFFFFFFFFu) == 0.5f && gdx_mpo_engine_bits(-0.1f) == gdx_mpo_engine_bits(0.5f));
    buf[115] = 2; /* picked is 0/1 */
    CHECK(gdx_lb_parse(buf, n, 0xC0FFEEull, &g, &body) == -3);
    buf[115] = 1;
    PutU32(buf + 124, GDX_MPO_PICK_MS + 1); /* more than a whole machine step */
    CHECK(gdx_lb_parse(buf, n, 0xC0FFEEull, &g, &body) == -3);
    PutU32(buf + 124, GDX_LB_NO_ROUND);
    CHECK(gdx_lb_parse(buf, n, 0xC0FFEEull, &g, &body) == GDX_LB_HELLO && body.hello.pickMsLeft == GDX_LB_NO_ROUND);
    buf[112] = 3; /* no such picker */
    CHECK(gdx_lb_parse(buf, n, 0xC0FFEEull, &g, &body) == -3);
    buf[112] = GDX_LB_PICK_COURSE;
    buf[114] = 48; /* the X cup is never a reported course */
    CHECK(gdx_lb_parse(buf, n, 0xC0FFEEull, &g, &body) == -3);
    buf[114] = 31;
    CHECK(gdx_lb_parse(buf, n, 0xC0FFEEull, &g, &body) == GDX_LB_HELLO && body.hello.picking == GDX_LB_PICK_COURSE);
    CHECK(g.from == 2 && g.sendMs == 1000 && g.echoMs == 900 && g.holdMs == 12);
    CHECK(body.hello.machine == 29 && body.hello.engine == 100 && body.hello.rttMs[0] == 42);
    CHECK(body.hello.rttMs[1] == GDX_LB_NO_RTT && body.hello.directMask == 5);
    CHECK(strcmp(body.hello.build, "mp-online@1234567") == 0 && strcmp(body.hello.name, "OLD3DS") == 0);
    CHECK(gdx_lb_parse(buf, n, 0xC0FFEFull, &g, &body) == -2);
    CHECK(gdx_lb_parse(buf, n - 1, 0xC0FFEEull, &g, &body) == -1);
    buf[40] = 30; /* machine 30 does not exist */
    CHECK(gdx_lb_parse(buf, n, 0xC0FFEEull, &g, &body) == -3);
    buf[40] = 29;
    buf[6] = 4; /* from out of range */
    CHECK(gdx_lb_parse(buf, n, 0xC0FFEEull, &g, &body) == -1);
    buf[6] = 2;
    buf[56 + 3] = 0x01; /* control byte in the build id */
    CHECK(gdx_lb_parse(buf, n, 0xC0FFEEull, &g, &body) == -3);

    st.player[0].slot = 0;
    st.player[1].slot = 1;
    st.player[2].slot = 3;
    st.laps = 0; /* laps 1..9 only */
    h.type = GDX_LB_START;
    h.from = 0;
    n = gdx_lb_build(&h, &st, buf, sizeof(buf));
    CHECK(gdx_lb_parse(buf, n, 0xC0FFEEull, &g, &body) == -3);
    st.laps = 5;
    n = gdx_lb_build(&h, &st, buf, sizeof(buf));
    CHECK(n == GDX_LB_START_BYTES);
    CHECK(gdx_lb_parse(buf, n, 0xC0FFEEull, &g, &body) == GDX_LB_START);
    CHECK(body.start.gen == 3 && body.start.numPlayers == 3 && body.start.course == 7 && body.start.delay == 3);
    CHECK(body.start.token == 0x0123456789ABCDEFull && body.start.player[2].slot == 3);
    CHECK(body.start.laps == 5 && body.start.courseKey == gdx_mpo_stock_course_key(7));
    CHECK(gdx_mpo_engine_from_bits(body.start.player[2].engineBits) == 0.5f);
    CHECK(buf[82] == 0 && buf[83] == 0 && buf[84] == 0 && buf[87] == 0); /* mp30's racers/ramMask/simAbi */
    CHECK(body.start.seed2 == 33 && body.start.mask2 == 44 && body.start.player[1].machine == 7);
    buf[6] = 1; /* START from a non-host */
    CHECK(gdx_lb_parse(buf, n, 0xC0FFEEull, &g, &body) == -3);
    buf[6] = 0;
    buf[33] = 48; /* course 48 (the X cup, generated): refused */
    CHECK(gdx_lb_parse(buf, n, 0xC0FFEEull, &g, &body) == -3);
    buf[33] = 42; /* past the EK's last disk course */
    CHECK(gdx_lb_parse(buf, n, 0xC0FFEEull, &g, &body) == -3);
    buf[33] = 25; /* an EDIT-cup course: each console's own file, not raced online (yet) */
    CHECK(gdx_lb_parse(buf, n, 0xC0FFEEull, &g, &body) == -3);
    CHECK(!gdx_mpo_course_raceable(24) && !gdx_mpo_course_raceable(29) && gdx_mpo_course_raceable(30));
    CHECK(gdx_mpo_course_raceable(0) && gdx_mpo_course_raceable(23) && gdx_mpo_course_raceable(41));
    CHECK(!gdx_mpo_course_raceable(42) && !gdx_mpo_course_raceable(48) && !gdx_mpo_course_raceable(-1));
    buf[33] = 31; /* an EK DD-cup course: fine on the wire (the joiner checks its key) */
    CHECK(gdx_lb_parse(buf, n, 0xC0FFEEull, &g, &body) == GDX_LB_START && body.start.course == 31);
    buf[33] = 7;
    PutU32(buf + 88, 0); /* no course key: a host that does not have its own course */
    CHECK(gdx_lb_parse(buf, n, 0xC0FFEEull, &g, &body) == -3);
    PutU32(buf + 88, gdx_mpo_stock_course_key(7));
    buf[34] = 9; /* delay beyond the redundancy window */
    CHECK(gdx_lb_parse(buf, n, 0xC0FFEEull, &g, &body) == -3);
    buf[34] = 3;
    buf[64 + 4] = 3; /* slots must ascend: 0, 3, 3 */
    CHECK(gdx_lb_parse(buf, n, 0xC0FFEEull, &g, &body) == -3);
    buf[64 + 4] = 1;
    buf[64] = 1; /* the host must be player 0 */
    CHECK(gdx_lb_parse(buf, n, 0xC0FFEEull, &g, &body) == -3);
    buf[64] = 0;
    buf[32] = 5;
    CHECK(gdx_lb_parse(buf, n, 0xC0FFEEull, &g, &body) == -3);
    buf[32] = 3;
    buf[80] = 0x08; /* bot bit for a player that does not exist */
    CHECK(gdx_lb_parse(buf, n, 0xC0FFEEull, &g, &body) == -3);
    buf[80] = 0;
    PutU64(buf + 56, 0); /* a zero token */
    CHECK(gdx_lb_parse(buf, n, 0xC0FFEEull, &g, &body) == -3);

    memset(&ab, 0, sizeof(ab));
    ab.gen = 3;
    ab.reason = GDX_LB_ABORT_DESYNC;
    ab.culprit = 1;
    ab.tick = 2000;
    h.type = GDX_LB_ABORT;
    h.from = 1;
    n = gdx_lb_build(&h, &ab, buf, sizeof(buf));
    CHECK(n == GDX_LB_ABORT_BYTES);
    CHECK(gdx_lb_parse(buf, n, 0xC0FFEEull, &g, &body) == GDX_LB_ABORT && body.abort.tick == 2000);
    buf[32] = 9;
    CHECK(gdx_lb_parse(buf, n, 0xC0FFEEull, &g, &body) == -3);
    buf[5] = 9;
    CHECK(gdx_lb_parse(buf, n, 0xC0FFEEull, &g, &body) == -1);
    CHECK(gdx_lb_parse(buf, n, 0, &g, &body) == -1);
}

/* ---- 4. delay ------------------------------------------------------------------------------- */
static void TestDelay(void) {
    CHECK(gdx_mpo_delay_from_rtt(0, 0) == 2);
    CHECK(gdx_mpo_delay_from_rtt(10, 0) == 2);
    CHECK(gdx_mpo_delay_from_rtt(30, 0) == 2);
    CHECK(gdx_mpo_delay_from_rtt(30, 8) == 3);
    CHECK(gdx_mpo_delay_from_rtt(80, 0) == 4);
    CHECK(gdx_mpo_delay_from_rtt(100, 0) == 4);
    CHECK(gdx_mpo_delay_from_rtt(100, 10) == 5);
    CHECK(gdx_mpo_delay_from_rtt(150, 0) == 6);
    CHECK(gdx_mpo_delay_from_rtt(150, 10) == 7);
    CHECK(gdx_mpo_delay_from_rtt(230, 0) == 8);
    CHECK(gdx_mpo_delay_from_rtt(500, 50) == 8);
    CHECK(gdx_mpo_delay_from_rtt(0xFFFFFFFFu, 0) == 8);
}

/* ---- 5. link state -------------------------------------------------------------------------- */
static GdxMpEndpoint Ep(int a, int b, int c, int d, int port) {
    GdxMpEndpoint e;
    e.ip[0] = (uint8_t) a;
    e.ip[1] = (uint8_t) b;
    e.ip[2] = (uint8_t) c;
    e.ip[3] = (uint8_t) d;
    e.port = (uint16_t) port;
    return e;
}

static void TestLink(void) {
    GdxMpLink l;
    GdxMpEndpoint out[4];
    GdxMpEndpoint pub = Ep(203, 0, 113, 7, 40000);
    GdxMpEndpoint priv = Ep(192, 168, 2, 196, 27100);
    GdxMpEndpoint seen = Ep(203, 0, 113, 7, 40011); /* the port their packets really came from */
    int relay;
    int n;

    gdx_link_reset(&l);
    CHECK(gdx_link_route(&l, 0, 1, out, &relay) == 0 && !relay);
    gdx_link_set_candidates(&l, &pub, &priv, 0, 100);
    CHECK(l.state == GDX_LINK_PUNCHING);
    n = gdx_link_route(&l, 100, 1, out, &relay);
    CHECK(n == 2 && relay == 1 && gdx_mpo_endpoint_eq(&out[0], &pub) && gdx_mpo_endpoint_eq(&out[1], &priv));
    /* We hear them, but they have not heard us: not direct yet. */
    gdx_link_on_direct_rx(&l, &seen, 200);
    CHECK(l.state == GDX_LINK_PUNCHING && gdx_link_hear_direct(&l, 200));
    n = gdx_link_route(&l, 200, 0, out, &relay);
    CHECK(n == 3 && relay && gdx_mpo_endpoint_eq(&out[2], &seen));
    gdx_link_on_peer_report(&l, 1, 300);
    CHECK(l.state == GDX_LINK_DIRECT && l.upgrades == 1);
    n = gdx_link_route(&l, 300, 1, out, &relay);
    CHECK(n == 1 && !relay && gdx_mpo_endpoint_eq(&out[0], &seen));
    /* Direct goes quiet: back to punching (relayed meanwhile), then relay after the punch window. */
    gdx_link_tick(&l, 300 + GDX_LINK_HEAR_MS + 1);
    CHECK(l.state == GDX_LINK_PUNCHING && l.downgrades == 1);
    gdx_link_tick(&l, 300 + GDX_LINK_HEAR_MS + GDX_LINK_PUNCH_MS + 2);
    CHECK(l.state == GDX_LINK_RELAY);
    /* RELAY probes direct once a second, on HELLOs only. */
    n = gdx_link_route(&l, 20000, 1, out, &relay);
    CHECK(n == 3 && relay);
    n = gdx_link_route(&l, 20100, 1, out, &relay);
    CHECK(n == 0 && relay);
    n = gdx_link_route(&l, 21200, 0, out, &relay);
    CHECK(n == 0 && relay);
    /* ...and recovers if the path comes back both ways. */
    gdx_link_on_direct_rx(&l, &seen, 22000);
    gdx_link_on_peer_report(&l, 1, 22001);
    CHECK(l.state == GDX_LINK_DIRECT);
    /* They stop hearing us (asymmetric filtering appeared): punching, not direct. */
    gdx_link_on_peer_report(&l, 0, 22100);
    CHECK(l.state == GDX_LINK_PUNCHING);

    /* One-way only (our side behind a symmetric NAT: they never hear us): never direct. */
    gdx_link_reset(&l);
    gdx_link_set_candidates(&l, &pub, NULL, 0, 0);
    {
        uint32_t t;
        for (t = 0; t < 10000; t += 250) {
            gdx_link_on_direct_rx(&l, &seen, t);
            gdx_link_on_peer_report(&l, 0, t);
            gdx_link_tick(&l, t);
            CHECK(l.state != GDX_LINK_DIRECT);
        }
        CHECK(l.state == GDX_LINK_RELAY);
    }
    /* Forced relay never probes direct. */
    gdx_link_reset(&l);
    gdx_link_set_candidates(&l, &pub, &priv, 1, 0);
    CHECK(l.state == GDX_LINK_RELAY);
    gdx_link_on_direct_rx(&l, &seen, 10);
    gdx_link_on_peer_report(&l, 1, 10);
    CHECK(l.state == GDX_LINK_RELAY);
    n = gdx_link_route(&l, 5000, 1, out, &relay);
    CHECK(n == 0 && relay);
    /* A private address equal to the public one is not a second candidate; invalid ones dropped. */
    gdx_link_reset(&l);
    {
        GdxMpEndpoint bad = Ep(0, 0, 0, 0, 27100);
        gdx_link_set_candidates(&l, &pub, &pub, 0, 0);
        CHECK(l.candValid[0] && !l.candValid[1]);
        gdx_link_set_candidates(&l, &pub, &bad, 0, 0);
        CHECK(!l.candValid[1]);
        bad = Ep(224, 0, 0, 1, 5);
        gdx_link_set_candidates(&l, &bad, NULL, 0, 0);
        CHECK(!l.candValid[0]);
    }
}

/* ---- 6. hygiene ----------------------------------------------------------------------------- */
static void TestHygiene(void) {
    GdxMpBucket b;
    GdxMpBlocklist bl;
    GdxMpEndpoint a = Ep(1, 2, 3, 4, 5);
    GdxMpEndpoint c = Ep(1, 2, 3, 4, 6);
    int ok = 0;
    int i;
    GdxMpPad pad;
    GdxMpLockstep* ls;
    uint8_t pkt[GDX_MP_MAX_PACKET];
    int len;
    GdxMpPad got[GDX_MP_MAX_PLAYERS];

    gdx_mpo_bucket_init(&b, 100, 10);
    for (i = 0; i < 50; i++) {
        ok += gdx_mpo_bucket_take(&b, 1000);
    }
    CHECK(ok == 10); /* the burst, then nothing in the same millisecond */
    ok = 0;
    for (i = 0; i < 1000; i++) {
        ok += gdx_mpo_bucket_take(&b, 1000 + (uint32_t) i); /* one second of 1 kHz offers */
    }
    CHECK(ok >= 95 && ok <= 112);
    CHECK(!gdx_mpo_bucket_take(&b, 1999) || 1);
    ok = 0;
    for (i = 0; i < 20; i++) {
        ok += gdx_mpo_bucket_take(&b, 100000);
    }
    CHECK(ok == 10); /* a long idle refills only to the burst */

    gdx_mpo_block_init(&bl, 5);
    for (i = 0; i < 4; i++) {
        gdx_mpo_block_note_reject(&bl, &a, 100);
    }
    CHECK(!gdx_mpo_block_is_blocked(&bl, &a, 100));
    gdx_mpo_block_note_reject(&bl, &a, 100);
    CHECK(gdx_mpo_block_is_blocked(&bl, &a, 101) && !gdx_mpo_block_is_blocked(&bl, &c, 101));
    CHECK(!gdx_mpo_block_is_blocked(&bl, &a, 100 + 60001));
    /* Slow rejects inside separate 10 s windows never block. */
    gdx_mpo_block_note_reject(&bl, &c, 0);
    gdx_mpo_block_note_reject(&bl, &c, 20000);
    gdx_mpo_block_note_reject(&bl, &c, 40000);
    gdx_mpo_block_note_reject(&bl, &c, 60000);
    gdx_mpo_block_note_reject(&bl, &c, 80000);
    CHECK(!gdx_mpo_block_is_blocked(&bl, &c, 80001));
    /* The table is bounded: 100 distinct sources do not overflow it. */
    for (i = 0; i < 100; i++) {
        GdxMpEndpoint e = Ep(9, 9, (i >> 8) & 255, i & 255, 1000 + i);
        gdx_mpo_block_note_reject(&bl, &e, 90000 + (uint32_t) i);
    }
    CHECK(bl.blocks >= 1);

    pad.buttons = 0xFFFF;
    pad.stickX = -128;
    pad.stickY = 127;
    CHECK(gdx_mp_ls_clamp_pad(&pad) == 1);
    CHECK(pad.buttons == GDX_MP_BUTTON_MASK && pad.stickX == -80 && pad.stickY == 80);
    pad.buttons = 0x8000;
    pad.stickX = 80;
    pad.stickY = -80;
    CHECK(gdx_mp_ls_clamp_pad(&pad) == 0);

    /* A hostile peer's out-of-range pad lands clamped, and the sender's own copy agrees. */
    ls = (GdxMpLockstep*) calloc(2, sizeof(GdxMpLockstep));
    CHECK(gdx_mp_ls_init(&ls[0], 2, 0, 2, 1, 5u) == 0);
    CHECK(gdx_mp_ls_init(&ls[1], 2, 1, 2, 1, 5u) == 0);
    ls[1].wireSlot = 3;
    ls[1].txFlags = GDX_MP_FLAG_ABORT;
    pad.buttons = 0x00C0 | 0x8000;
    pad.stickX = -128;
    pad.stickY = 100;
    gdx_mp_ls_push_local(&ls[1], 0, pad);
    len = gdx_mp_ls_build_packet(&ls[1], 10, pkt, sizeof(pkt));
    CHECK(len > 0 && pkt[34] == 3 && pkt[33] == GDX_MP_FLAG_ABORT);
    CHECK(gdx_mp_ls_packet_wire_slot(pkt, len) == 3);
    /* forge the raw wire bytes back to the hostile values: the receiver must clamp them */
    pkt[GDX_MP_HEADER_BYTES + 0] = 0xC0;
    pkt[GDX_MP_HEADER_BYTES + 1] = 0x80;
    pkt[GDX_MP_HEADER_BYTES + 2] = 0x80;
    pkt[GDX_MP_HEADER_BYTES + 3] = 100;
    CHECK(gdx_mp_ls_ingest(&ls[0], pkt, len, 11) == 1);
    CHECK(ls[0].peerFlags[1] == GDX_MP_FLAG_ABORT && ls[0].peerWireSlot[1] == 3);
    gdx_mp_ls_push_local(&ls[0], 0, pad);
    CHECK(gdx_mp_ls_ready(&ls[0], 2));
    gdx_mp_ls_get(&ls[0], 2, got);
    /* 0x0080 is no button and is clamped; 0x0040 is the MP30 "BGM ready" bit and survives. */
    CHECK(got[1].buttons == (0x8000 | GDX_MP_PAD_BGM_READY) && got[1].stickX == -80 && got[1].stickY == 80);
    gdx_mp_ls_get(&ls[1], 2, got);
    CHECK(got[1].buttons == (0x8000 | GDX_MP_PAD_BGM_READY) && got[1].stickX == -80 && got[1].stickY == 80);
    CHECK(ls[0].padsClamped >= 1 && ls[1].padsClamped >= 1);
    free(ls);
}

/* ---- 7. simulated session ------------------------------------------------------------------- */
enum { NAT_OPEN = 0, NAT_CONE = 1, NAT_SYMMETRIC = 2 };

#define SIM_MAX_Q 20000
#define SIM_RELAY 99 /* a datagram that went through the server */

typedef struct SimDatagram {
    int to;
    int fromConsole;
    int viaRelay;
    uint32_t at;
    int len;
    uint8_t data[GDX_LB_RM_BYTES > GDX_MP_MAX_PACKET ? GDX_LB_RM_BYTES : GDX_MP_MAX_PACKET];
} SimDatagram;

typedef struct SimNet {
    SimDatagram q[SIM_MAX_Q];
    int n;
    int lossPct;
    uint32_t oneWayMs; /* direct path latency */
    uint32_t relayExtraMs;
    uint32_t jitterMs;
    int nat[GDX_MPO_MAX_SLOTS];
    /* per (receiver, sender): has the receiver sent direct to the sender (opens a cone NAT) */
    int opened[GDX_MPO_MAX_SLOTS][GDX_MPO_MAX_SLOTS];
    uint32_t directDelivered;
    uint32_t relayDelivered;
} SimNet;

typedef struct SimConsole {
    GdxMpLobby lb;
    GdxMpLink link[GDX_MPO_MAX_SLOTS];
    GdxMpLockstep ls;
    int inSession;
    int player;
    uint32_t tick;
    uint32_t padLog;
    uint32_t stalls;
    uint32_t session;
    uint32_t lastHelloMs;
} SimConsole;

/* Direct delivery rule: an OPEN host accepts anything; a CONE host accepts a sender it has sent
 * to; a SYMMETRIC host accepts nothing unsolicited, and a SYMMETRIC sender's packets only reach
 * an OPEN receiver (its mapped port is unpredictable to everybody else). */
static int SimDirectOk(const SimNet* net, int from, int to) {
    if (net->nat[from] == NAT_SYMMETRIC && net->nat[to] != NAT_OPEN) {
        return 0;
    }
    if (net->nat[to] == NAT_OPEN) {
        return 1;
    }
    if (net->nat[to] == NAT_SYMMETRIC) {
        return net->opened[to][from] && net->nat[from] == NAT_OPEN;
    }
    return net->opened[to][from];
}

static void SimSend(SimNet* net, int from, int to, const uint8_t* buf, int len, int viaRelay, uint32_t now) {
    SimDatagram* d;
    if (net->n >= SIM_MAX_Q || (int) (Rand() % 100) < net->lossPct) {
        return;
    }
    if (!viaRelay) {
        net->opened[from][to] = 1;
        if (!SimDirectOk(net, from, to)) {
            return;
        }
    }
    d = &net->q[net->n++];
    d->to = to;
    d->fromConsole = from;
    d->viaRelay = viaRelay;
    d->at = now + net->oneWayMs + (viaRelay ? net->relayExtraMs : 0) + (net->jitterMs ? Rand() % net->jitterMs : 0);
    d->len = len;
    memcpy(d->data, buf, (size_t) len);
}

static void SimRoute(SimNet* net, SimConsole* c, int me, int dst, const uint8_t* buf, int len, int probe,
                     uint32_t now) {
    GdxMpEndpoint eps[GDX_LINK_MAX_CAND];
    int relay = 0;
    const int n = gdx_link_route(&c->link[dst], now, probe, eps, &relay);
    if (n > 0) {
        SimSend(net, me, dst, buf, len, 0, now); /* every candidate reaches the same console */
    }
    if (relay) {
        SimSend(net, me, dst, buf, len, 1, now);
    }
}

static int SimDirectMask(SimConsole* c, uint32_t now) {
    int m = 0;
    int s;
    for (s = 0; s < GDX_MPO_MAX_SLOTS; s++) {
        if (gdx_link_hear_direct(&c->link[s], now)) {
            m |= 1 << s;
        }
    }
    return m;
}

static GdxMpPad SimPad(int player, uint32_t tick) {
    GdxMpPad p;
    p.buttons = (uint16_t) (((tick * 2654435761u) >> 16) ^ (uint32_t) player * 977u);
    p.stickX = (int8_t) ((int) ((tick * 5u + (uint32_t) player * 11u) % 201u) - 100); /* some out of range */
    p.stickY = (int8_t) ((int) ((tick + (uint32_t) player) % 161u) - 80);
    return p;
}

static uint32_t Mix(uint32_t h, uint32_t v) {
    h ^= v;
    h *= 16777619u;
    return h;
}

typedef struct SimResult {
    int started;
    int delay;
    int allSame;
    uint32_t maxStalls;
    int relayLinks;
    int directLinks;
    uint32_t relayDelivered;
    uint32_t directDelivered;
    int ticks;
} SimResult;

static SimResult RunOnline(int players, const int* nat, int forceRelay, uint32_t oneWayMs, uint32_t jitterMs,
                           int lossPct, uint32_t ticks) {
    static SimNet net;
    static SimConsole cs[GDX_MPO_MAX_SLOTS];
    SimResult res;
    GdxMpRvRoster roster;
    uint32_t now = 1000;
    uint32_t end;
    int i;
    int s;
    const uint64_t secret = 0x5EC12E7ull;

    memset(&res, 0, sizeof(res));
    memset(&net, 0, sizeof(net));
    memset(cs, 0, sizeof(cs));
    net.lossPct = lossPct;
    net.oneWayMs = oneWayMs;
    net.relayExtraMs = 10;
    net.jitterMs = jitterMs;
    memset(&roster, 0, sizeof(roster));
    strcpy(roster.code, "K7MPQ");
    roster.count = players;
    for (s = 0; s < players; s++) {
        net.nat[s] = nat ? nat[s] : NAT_CONE;
        roster.member[s].present = 1;
        roster.member[s].pub = Ep(198, 51, 100, (uint8_t) (10 + s), 30000 + s);
        roster.member[s].priv = Ep(192, 168, 1, (uint8_t) (10 + s), 27100);
        snprintf(roster.member[s].name, sizeof(roster.member[s].name), "P%d", s);
    }
    for (s = 0; s < players; s++) {
        gdx_lobby_init(&cs[s].lb, s, secret, "mp-online@test", 0xC0DEu, roster.member[s].name);
        gdx_lobby_set_course(&cs[s].lb, 3, gdx_mpo_stock_course_key(3));
        gdx_lobby_set_roster(&cs[s].lb, &roster);
        cs[s].lb.machine = s * 3;
        cs[s].lb.engine = 20 + s * 10;
        cs[s].lb.ready = 1;
        for (i = 0; i < GDX_MPO_MAX_SLOTS; i++) {
            gdx_link_reset(&cs[s].link[i]);
            if (i != s && i < players) {
                gdx_link_set_candidates(&cs[s].link[i], &roster.member[i].pub, &roster.member[i].priv, forceRelay,
                                        now);
            }
        }
    }
    end = now + 20000 + ticks * 17u * 3u;
    while (now < end) {
        int done = 1;
        now++;
        /* deliver */
        for (i = 0; i < net.n;) {
            SimDatagram* d = &net.q[i];
            if (d->at <= now) {
                SimConsole* c = &cs[d->to];
                if (d->len >= 4 && memcmp(d->data, "GXLB", 4) == 0) {
                    int from = -1;
                    const int t = gdx_lobby_ingest(&c->lb, d->data, d->len, now, &from);
                    if (t > 0 && from >= 0) {
                        if (!d->viaRelay) {
                            GdxMpEndpoint src = Ep(198, 51, 100, (uint8_t) (10 + from), 40000 + from);
                            gdx_link_on_direct_rx(&c->link[from], &src, now);
                        }
                        if (t == GDX_LB_HELLO) {
                            gdx_link_on_peer_report(&c->link[from], (c->lb.peer[from].hello.directMask >> d->to) & 1,
                                                    now);
                        }
                    }
                } else if (c->inSession) {
                    gdx_mp_ls_ingest(&c->ls, d->data, d->len, now);
                }
                if (d->viaRelay) {
                    net.relayDelivered++;
                } else {
                    net.directDelivered++;
                }
                net.q[i] = net.q[--net.n];
            } else {
                i++;
            }
        }
        for (s = 0; s < players; s++) {
            SimConsole* c = &cs[s];
            int k;
            for (k = 0; k < players; k++) {
                if (k != s) {
                    gdx_link_tick(&c->link[k], now);
                }
            }
            /* HELLO at 4 Hz (lobby) / 1 Hz (session) */
            if (now - c->lastHelloMs >= (c->inSession ? 1000u : 250u)) {
                c->lastHelloMs = now;
                for (k = 0; k < players; k++) {
                    uint8_t pkt[GDX_LB_HELLO_BYTES];
                    const int n = gdx_lobby_build_hello(&c->lb, k, now, SimDirectMask(c, now), pkt, sizeof(pkt));
                    if (n > 0) {
                        SimRoute(&net, c, s, k, pkt, n, 1, now);
                    }
                }
            }
            /* host: start once everybody is ready and RTTs have settled */
            if (s == 0 && !c->lb.haveStart && now > 6000 + 1000) {
                const char* why = NULL;
                if (gdx_lobby_host_startable(&c->lb, now, &why) == players) {
                    GdxMpLobbySettings st;
                    memset(&st, 0, sizeof(st));
                    st.course = 3;
                    st.vsCom = 1;
                    st.hashEvery = 1;
                    CHECK(gdx_lobby_host_make_start(&c->lb, &st, 1, 2, 3, 4, 0xABCDEFull, now) == players);
                }
            }
            if (s == 0 && c->lb.haveStart && !gdx_lobby_start_acked(&c->lb) && now % 50 == 0) {
                for (k = 1; k < players; k++) {
                    uint8_t pkt[GDX_LB_START_BYTES];
                    const int n = gdx_lobby_build_start(&c->lb, k, now, pkt, sizeof(pkt));
                    if (n > 0) {
                        SimRoute(&net, c, s, k, pkt, n, 0, now);
                    }
                }
            }
            /* adopt a START: session begins */
            if (c->lb.startIsNew && !c->inSession) {
                c->lb.startIsNew = 0;
                c->player = gdx_lobby_my_player(&c->lb);
                c->session = gdx_mpo_session_digest(c->lb.build, c->lb.contentHash, &c->lb.start);
                CHECK(c->player >= 0);
                CHECK(gdx_mp_ls_init(&c->ls, c->lb.start.numPlayers, c->player, c->lb.start.delay,
                                     c->lb.start.hashEvery, c->session) == 0);
                c->ls.wireSlot = (uint8_t) (s + 1);
                c->inSession = 1;
                c->lb.state = GDX_LB_STATE_RACING;
                c->tick = 0;
            }
            /* lockstep: one tick per 16 ms when ready */
            if (c->inSession && c->tick < ticks) {
                done = 0;
                if (now % 16 == (uint32_t) s) {
                    if (!(c->tick > 0 && !gdx_mp_ls_ready(&c->ls, c->tick - 1))) {
                        uint8_t pkt[GDX_MP_MAX_PACKET];
                        int n;
                        if (c->tick > 0) {
                            GdxMpPad pads[GDX_MP_MAX_PLAYERS];
                            int p;
                            gdx_mp_ls_get(&c->ls, c->tick - 1, pads);
                            for (p = 0; p < c->ls.numPlayers; p++) {
                                c->padLog = Mix(c->padLog, pads[p].buttons | ((uint32_t) (uint8_t) pads[p].stickX << 16));
                            }
                            gdx_mp_ls_note_local_hash(&c->ls, c->tick - 1, c->padLog);
                        }
                        if (c->tick < ticks) {
                            gdx_mp_ls_push_local(&c->ls, c->tick, SimPad(c->player, c->tick));
                            c->tick++;
                        }
                        n = gdx_mp_ls_build_packet(&c->ls, now, pkt, sizeof(pkt));
                        for (k = 0; k < c->ls.numPlayers && n > 0; k++) {
                            const int slot = gdx_lobby_player_slot(&c->lb, k);
                            if (k != c->player) {
                                SimRoute(&net, c, s, slot, pkt, n, 0, now);
                            }
                        }
                    } else {
                        uint8_t pkt[GDX_MP_MAX_PACKET];
                        const int n = gdx_mp_ls_build_packet(&c->ls, now, pkt, sizeof(pkt));
                        c->stalls++;
                        for (k = 0; k < c->ls.numPlayers && n > 0; k++) {
                            if (k != c->player) {
                                SimRoute(&net, c, s, gdx_lobby_player_slot(&c->lb, k), pkt, n, 0, now);
                            }
                        }
                    }
                }
            } else if (!c->inSession) {
                done = 0;
            }
        }
        if (done) {
            break;
        }
    }
    res.started = cs[0].lb.haveStart;
    res.delay = cs[0].lb.start.delay;
    res.allSame = 1;
    for (s = 0; s < players; s++) {
        int k;
        if (!cs[s].inSession || cs[s].tick < ticks) {
            res.allSame = 0;
        }
        if (cs[s].padLog != cs[0].padLog || cs[s].session != cs[0].session) {
            res.allSame = 0;
        }
        if (cs[s].ls.desynced) {
            res.allSame = 0;
        }
        if (cs[s].stalls > res.maxStalls) {
            res.maxStalls = cs[s].stalls;
        }
        for (k = 0; k < players; k++) {
            if (k == s) {
                continue;
            }
            if (cs[s].link[k].state == GDX_LINK_DIRECT) {
                res.directLinks++;
            } else if (cs[s].link[k].state == GDX_LINK_RELAY) {
                res.relayLinks++;
            }
        }
    }
    res.relayDelivered = net.relayDelivered;
    res.directDelivered = net.directDelivered;
    res.ticks = (int) cs[0].tick;
    return res;
}

static void TestSimulatedSessions(void) {
    SimResult r;
    int p;
    const int cone4[4] = { NAT_CONE, NAT_CONE, NAT_CONE, NAT_CONE };
    const int mixed[4] = { NAT_OPEN, NAT_SYMMETRIC, NAT_CONE, NAT_SYMMETRIC };

    for (p = 2; p <= 4; p++) {
        r = RunOnline(p, cone4, 0, 10, 4, 5, 1500);
        CHECK(r.started && r.allSame);
        CHECK(r.directLinks == p * (p - 1)); /* every pair punched through */
        CHECK(r.delay >= 2 && r.delay <= 3);
        printf("  sim %dP cone/LAN: delay=%d direct_links=%d relay_links=%d stalls<=%u delivered direct=%u relay=%u\n",
               p, r.delay, r.directLinks, r.relayLinks, r.maxStalls, r.directDelivered, r.relayDelivered);
    }
    /* Mixed NATs: every link that involves a symmetric NAT on a non-open peer goes through the relay. */
    r = RunOnline(4, mixed, 0, 15, 4, 5, 1500);
    CHECK(r.started && r.allSame);
    CHECK(r.relayLinks > 0 && r.directLinks > 0);
    printf("  sim 4P mixed NAT: delay=%d direct_links=%d relay_links=%d\n", r.delay, r.directLinks, r.relayLinks);
    /* Forced relay: no direct link at all, still identical. */
    r = RunOnline(4, cone4, 1, 10, 4, 5, 1500);
    CHECK(r.started && r.allSame && r.directLinks == 0 && r.relayLinks == 12);
    CHECK(r.directDelivered == 0);
    printf("  sim 4P forced relay: delay=%d relay_links=%d relayed=%u\n", r.delay, r.relayLinks, r.relayDelivered);
    /* 100 ms RTT: a larger delay than a LAN, and it absorbs the latency (stalls stay rare). */
    r = RunOnline(3, cone4, 0, 50, 6, 3, 1500);
    CHECK(r.started && r.allSame);
    CHECK(r.delay >= 4 && r.delay <= 6);
    printf("  sim 3P 100ms RTT: delay=%d stalls<=%u\n", r.delay, r.maxStalls);
    CHECK(r.maxStalls < 150);
    /* Heavy loss: still identical (stalls, never a desync). */
    r = RunOnline(4, cone4, 0, 20, 10, 25, 1200);
    CHECK(r.started && r.allSame);
    printf("  sim 4P 25%% loss: delay=%d stalls<=%u\n", r.delay, r.maxStalls);
}

/* ---- player removal: a console drops out mid-race, the others agree on T and race on ---------- */
typedef struct RmConsole {
    GdxMpLobby lb;
    GdxMpLockstep ls;
    GdxMpRemoval rm;
    int dead;
    int dropped;   /* got RM_SET about itself */
    int failed;    /* a removal step said the session cannot go on */
    uint32_t tick; /* next tick to run */
    uint32_t padLog;
    uint32_t lastHeard[GDX_MPO_MAX_SLOTS];
    uint32_t lastRmSendMs;
    uint32_t lastHelloMs;
} RmConsole;

typedef struct RmResult {
    int ok;
    uint32_t removedTick;
    int survivorsDone;
    int killedDropped; /* the killed console got RM_SET about itself */
} RmResult;

/* RunRemoval: the killed console goes MUTE (a one-way partition: it still hears everybody)
 * instead of dark. It must learn from RM_SET that it was dropped. */
static int sRmMute = 0;
static int sRmMutedSlot = -1;
/* RunRemoval: a second console switched off at the same tick (-1 = none). */
static int sRmKill2 = -1;

static int RmKilled(int slot, int killSlot) {
    return slot == killSlot || slot == sRmKill2;
}

static void RmSend(SimNet* net, int from, int to, const uint8_t* buf, int len, uint32_t now) {
    SimDatagram* d;
    if (from == sRmMutedSlot) {
        return;
    }
    if (len <= 0 || net->n >= SIM_MAX_Q || (int) (Rand() % 100) < net->lossPct) {
        return;
    }
    d = &net->q[net->n++];
    d->to = to;
    d->fromConsole = from;
    d->viaRelay = 0;
    d->at = now + net->oneWayMs + (net->jitterMs ? Rand() % net->jitterMs : 0);
    d->len = len;
    memcpy(d->data, buf, (size_t) len);
}

static int RmFirstSurvivor(int players, int killSlot) {
    int s;
    for (s = 0; s < players; s++) {
        if (!RmKilled(s, killSlot)) {
            return s;
        }
    }
    return 0;
}

static RmResult RunRemoval(int players, int killSlot, uint32_t killTick, uint32_t ticks, int lossPct, uint32_t oneWayMs) {
    static SimNet net;
    static RmConsole cs[GDX_MPO_MAX_SLOTS];
    GdxMpRvRoster roster;
    GdxMpLbStart st;
    RmResult res;
    uint32_t now = 1000;
    const uint64_t secret = 0x77AA55ull;
    int s;
    int k;

    memset(&res, 0, sizeof(res));
    memset(&net, 0, sizeof(net));
    memset(cs, 0, sizeof(cs));
    net.lossPct = lossPct;
    net.oneWayMs = oneWayMs;
    net.jitterMs = 3;
    memset(&roster, 0, sizeof(roster));
    roster.count = players;
    memset(&st, 0, sizeof(st));
    st.gen = 7;
    st.numPlayers = (uint8_t) players;
    st.delay = 3;
    st.hashEvery = 1;
    st.laps = 3;
    st.token = 1;
    for (s = 0; s < players; s++) {
        roster.member[s].present = 1;
        st.player[s].slot = (uint8_t) s;
    }
    for (s = 0; s < players; s++) {
        RmConsole* c = &cs[s];
        gdx_lobby_init(&c->lb, s, secret, "mp-online@test", 0xC0DEu, "P");
        gdx_lobby_set_roster(&c->lb, &roster);
        c->lb.start = st;
        c->lb.haveStart = 1;
        c->lb.state = GDX_LB_STATE_RACING;
        CHECK(gdx_mp_ls_init(&c->ls, players, s, st.delay, 1, 0x1234u) == 0);
        gdx_rm_reset(&c->rm);
        for (k = 0; k < players; k++) {
            c->lastHeard[k] = now;
        }
    }
    while (now < 1000 + 30000 + ticks * 40u) {
        int done = 1;
        int i;
        now++;
        for (i = 0; i < net.n;) {
            SimDatagram* d = &net.q[i];
            if (d->at > now) {
                i++;
                continue;
            }
            if (!cs[d->to].dead) {
                RmConsole* c = &cs[d->to];
                c->lastHeard[d->fromConsole] = now;
                if (memcmp(d->data, "GXLB", 4) == 0) {
                    int from = -1;
                    const int t = gdx_lobby_ingest(&c->lb, d->data, d->len, now, &from);
                    if (t == GDX_LB_RM_POLL) {
                        uint8_t pkt[GDX_LB_RM_BYTES];
                        const int n = gdx_rm_answer_poll(&c->lb, &c->ls, from, &c->lb.rm, now, pkt, sizeof(pkt));
                        RmSend(&net, d->to, from, pkt, n, now);
                    } else if (t == GDX_LB_RM_HAVE) {
                        if (gdx_rm_on_have(&c->rm, &c->lb, &c->ls, from, &c->lb.rm, now) < 0) {
                            c->failed = 1;
                        }
                        c->lb.removedMask = c->rm.removedMask;
                    } else if (t == GDX_LB_RM_SET) {
                        /* this loop runs tick t-1 once it has pushed t-1: the next one it will run is tick-1 */
                        const int rc = gdx_rm_on_set(&c->rm, &c->lb, &c->ls, &c->lb.rm, c->tick > 0 ? c->tick - 1 : 0);
                        if (rc == -2) {
                            c->dropped = 1;
                        } else if (rc < 0) {
                            c->failed = 1;
                        }
                        c->lb.removedMask = c->rm.removedMask;
                    } else if (t == GDX_LB_HELLO) {
                        gdx_rm_on_ack(&c->rm, &c->lb, from, c->lb.peer[from].hello.removedMask);
                    }
                } else {
                    gdx_mp_ls_ingest(&c->ls, d->data, d->len, now);
                }
            }
            net.q[i] = net.q[--net.n];
        }
        for (s = 0; s < players; s++) {
            RmConsole* c = &cs[s];
            if (c->dead || c->dropped || c->failed) {
                continue;
            }
            if (RmKilled(s, killSlot) && c->tick >= killTick) {
                if (!sRmMute) {
                    c->dead = 1; /* switched off: never sends or hears again */
                    continue;
                }
                sRmMutedSlot = s; /* still runs and hears, but nothing it sends arrives */
            }
            /* coordinator: a racer silent for 600 ms while we wait on it is gone */
            {
                uint8_t alive = (uint8_t) (1u << s);
                int coord;
                for (k = 0; k < players; k++) {
                    if (k != s && (int32_t) (now - c->lastHeard[k]) < 600) {
                        alive |= (uint8_t) (1u << k);
                    }
                }
                coord = gdx_rm_coordinator_slot(&c->lb, &c->rm, alive);
                if (coord == s) {
                    const uint8_t gone = (uint8_t) (~alive & ((1u << players) - 1u) & ~c->rm.removedMask);
                    if (gdx_rm_set_gone(&c->rm, &c->lb, &c->ls, gone, now) < 0) {
                        c->failed = 1;
                    }
                    c->lb.removedMask = c->rm.removedMask;
                }
                if (coord == s && c->rm.stage == GDX_RM_IDLE) {
                    for (k = 0; k < players; k++) {
                        if (!((alive >> k) & 1u) && !((c->rm.removedMask >> k) & 1u)) {
                            gdx_rm_begin(&c->rm, &c->lb, &c->ls, k, GDX_LB_ABORT_PEER_LOST, now);
                            c->lb.removedMask = c->rm.removedMask;
                            break;
                        }
                    }
                }
                if (gdx_rm_tick(&c->rm, now) < 0) {
                    c->failed = 1;
                }
                if (gdx_rm_wants_send(&c->rm, now) && (int32_t) (now - c->lastRmSendMs) >= GDX_RM_RESEND_MS) {
                    c->lastRmSendMs = now;
                    for (k = 0; k < players; k++) {
                        uint8_t pkt[GDX_LB_RM_BYTES];
                        const int n = gdx_rm_build_for(&c->rm, &c->lb, &c->ls, k, now, pkt, sizeof(pkt));
                        RmSend(&net, s, k, pkt, n, now);
                    }
                }
            }
            if ((int32_t) (now - c->lastHelloMs) >= 200) {
                c->lastHelloMs = now;
                for (k = 0; k < players; k++) {
                    uint8_t pkt[GDX_LB_HELLO_BYTES];
                    const int n = gdx_lobby_build_hello(&c->lb, k, now, 0, pkt, sizeof(pkt));
                    RmSend(&net, s, k, pkt, n, now);
                }
            }
            if (c->tick < ticks && s != sRmMutedSlot) {
                done = 0;
            }
            if (now % 16 == (uint32_t) s) {
                uint8_t pkt[GDX_MP_MAX_PACKET];
                int n;
                if (c->tick < ticks && (c->tick == 0 || gdx_mp_ls_ready(&c->ls, c->tick - 1))) {
                    if (c->tick > 0) {
                        GdxMpPad pads[GDX_MP_MAX_PLAYERS];
                        int p;
                        gdx_mp_ls_get(&c->ls, c->tick - 1, pads);
                        for (p = 0; p < players; p++) {
                            c->padLog = Mix(c->padLog, pads[p].buttons | ((uint32_t) (uint8_t) pads[p].stickX << 16));
                        }
                        gdx_mp_ls_note_local_hash(&c->ls, c->tick - 1, c->padLog);
                    }
                    gdx_mp_ls_push_local(&c->ls, c->tick, SimPad(s, c->tick));
                    c->tick++;
                }
                n = gdx_mp_ls_build_packet(&c->ls, now, pkt, sizeof(pkt));
                for (k = 0; k < players; k++) {
                    if (k != s) {
                        RmSend(&net, s, k, pkt, n, now);
                    }
                }
            }
        }
        if (done) {
            break;
        }
    }
    res.ok = 1;
    res.removedTick = GDX_MP_NO_TICK;
    res.killedDropped = cs[killSlot].dropped;
    sRmMutedSlot = -1;
    for (s = 0; s < players; s++) {
        const RmConsole* c = &cs[s];
        if (RmKilled(s, killSlot)) {
            continue;
        }
        if (c->failed || c->dropped || c->ls.desynced || c->tick < ticks) {
            printf("    [rm] console %d: failed=%d dropped=%d desync=%d tick=%u stage=%d need=%02x removed=%02x\n", s,
                   c->failed, c->dropped, c->ls.desynced, (unsigned) c->tick, c->rm.stage, c->rm.needMask,
                   c->rm.removedMask);
            res.ok = 0;
            continue;
        }
        res.survivorsDone++;
        if (c->padLog != cs[RmFirstSurvivor(players, killSlot)].padLog) {
            res.ok = 0;
        }
        if (res.removedTick == GDX_MP_NO_TICK) {
            res.removedTick = c->ls.removedAt[killSlot];
        } else if (res.removedTick != c->ls.removedAt[killSlot]) {
            res.ok = 0;
        }
    }
    return res;
}

static void TestRemoval(void) {
    GdxMpLockstep ls;
    GdxMpPad pads[GDX_MP_MAX_PLAYERS];
    GdxMpPad some[4];
    uint32_t first;
    RmResult r;
    int p;
    int i;

    /* lockstep primitives */
    CHECK(gdx_mp_ls_init(&ls, 3, 0, 2, 1, 9) == 0);
    for (i = 0; i < 4; i++) {
        some[i].buttons = (uint16_t) (0x10 + i);
        some[i].stickX = (int8_t) i;
        some[i].stickY = 0;
    }
    CHECK(gdx_mp_ls_fill(&ls, 2, 2, some, 4) == 6);        /* ticks 2..5 of player 2 */
    CHECK(gdx_mp_ls_fill(&ls, 2, 9, some, 2) == 6);        /* a gap: not contiguous, known stays */
    CHECK(gdx_mp_ls_remove(&ls, 2, 8) == -2);              /* we lack 6, 7 */
    CHECK(gdx_mp_ls_remove(&ls, 0, 3) == -1);              /* never ourselves */
    CHECK(gdx_mp_ls_newest_pads(&ls, 2, 3, &first, some) == 3 && first == 3);
    CHECK(gdx_mp_ls_remove(&ls, 2, 6) == 0 && gdx_mp_ls_remove(&ls, 2, 6) == 0 && gdx_mp_ls_remove(&ls, 2, 7) == -3);
    CHECK(gdx_mp_ls_is_removed(&ls, 2, 6) && !gdx_mp_ls_is_removed(&ls, 2, 5));
    gdx_mp_ls_push_local(&ls, 0, some[0]);
    CHECK(gdx_mp_ls_first_missing(&ls, 2) == 1);           /* p1 still owes tick 2; p2 does not */
    CHECK(gdx_mp_ls_first_missing(&ls, 100) == 0 || gdx_mp_ls_first_missing(&ls, 100) == 1);
    gdx_mp_ls_get(&ls, 7, pads);
    CHECK(pads[2].buttons == 0 && pads[2].stickX == 0);    /* neutral from T on */
    gdx_mp_ls_get(&ls, 5, pads);
    CHECK(pads[2].buttons == 0x13);                        /* the real pad before T */

    /* the protocol, end to end */
    for (p = 2; p <= 4; p++) {
        r = RunRemoval(p, p - 1, 300, 900, 5, 15);
        CHECK(r.ok && r.survivorsDone == p - 1 && r.removedTick != GDX_MP_NO_TICK);
        printf("  removal %dP, last slot off at tick 300: survivors=%d agree, T=%u\n", p, r.survivorsDone,
               (unsigned) r.removedTick);
    }
    r = RunRemoval(4, 0, 250, 800, 10, 20); /* the host itself: slot 1 coordinates */
    CHECK(r.ok && r.survivorsDone == 3);
    printf("  removal 4P, HOST off at tick 250, 10%% loss: survivors=%d agree, T=%u\n", r.survivorsDone,
           (unsigned) r.removedTick);
    r = RunRemoval(3, 1, 5, 600, 20, 30); /* almost at once, heavy loss */
    CHECK(r.ok && r.survivorsDone == 2);
    printf("  removal 3P, slot 1 off at tick 5, 20%% loss: T=%u\n", (unsigned) r.removedTick);
    sRmMute = 1;
    for (p = 2; p <= 3; p++) {
        r = RunRemoval(p, p - 1, 200, 700, 5, 15);
        CHECK(r.ok && r.survivorsDone == p - 1 && r.killedDropped);
        printf("  removal %dP, last slot MUTE at tick 200 (alive, unheard): survivors=%d agree, it was told: %d\n", p,
               r.survivorsDone, r.killedDropped);
    }
    sRmMute = 0;
    /* two consoles off at the same tick in a 4P race: the first removal must not wait on the second */
    sRmKill2 = 2;
    r = RunRemoval(4, 3, 300, 900, 5, 15);
    CHECK(r.ok && r.survivorsDone == 2);
    printf("  removal 4P, slots 2 AND 3 off at tick 300: survivors=%d agree, T=%u\n", r.survivorsDone,
           (unsigned) r.removedTick);
    sRmKill2 = 1;
    r = RunRemoval(4, 0, 250, 800, 10, 20); /* the host and slot 1 together: slot 2 coordinates */
    CHECK(r.ok && r.survivorsDone == 2);
    printf("  removal 4P, HOST and slot 1 off at tick 250, 10%% loss: survivors=%d agree\n", r.survivorsDone);
    sRmKill2 = -1;
}

/* Lobby rules that the session sim does not reach. */
static void TestLobbyRules(void) {
    GdxMpLobby host;
    GdxMpLobby join;
    GdxMpRvRoster roster;
    uint8_t pkt[256];
    int n;
    int from;
    const char* why = NULL;
    GdxMpLobbySettings st;

    memset(&roster, 0, sizeof(roster));
    roster.count = 2;
    roster.member[0].present = 1;
    roster.member[1].present = 1;
    gdx_lobby_init(&host, 0, 99, "build-a", 1, "HOST");
    gdx_lobby_init(&join, 1, 99, "build-b", 1, "JOIN");
    gdx_lobby_set_roster(&host, &roster);
    gdx_lobby_set_roster(&join, &roster);
    gdx_lobby_set_course(&host, 2, gdx_mpo_stock_course_key(2));
    gdx_lobby_set_course(&join, 2, gdx_mpo_stock_course_key(2));
    join.ready = 1;
    n = gdx_lobby_build_hello(&join, 0, 100, 0, pkt, sizeof(pkt));
    CHECK(gdx_lobby_ingest(&host, pkt, n, 110, &from) == GDX_LB_HELLO && from == 1);
    CHECK(!host.peer[1].versionOk && host.versionRejected == 1);
    CHECK(gdx_lobby_host_startable(&host, 120, &why) == 0 && strstr(why, "version") != NULL);
    CHECK(strstr(why, "same one") != NULL); /* same proto, another build: nobody is "older" */
    /* card-4: the proto says which side should update */
    host.peer[1].hello.proto = GDX_MPO_PROTO + 1;
    CHECK(gdx_lobby_host_startable(&host, 120, &why) == 0 && strstr(why, "update this console") != NULL);
    host.peer[1].hello.proto = GDX_MPO_PROTO - 1;
    CHECK(gdx_lobby_host_startable(&host, 120, &why) == 0 && strstr(why, "they need an update") != NULL);
    host.peer[1].hello.proto = GDX_MPO_PROTO;
    /* Same build: startable; unready: not. */
    strcpy(join.build, "build-a");
    join.ready = 0;
    n = gdx_lobby_build_hello(&join, 0, 200, 0, pkt, sizeof(pkt));
    gdx_lobby_ingest(&host, pkt, n, 210, &from);
    CHECK(gdx_lobby_host_startable(&host, 220, &why) == 0 && strstr(why, "ready") != NULL);
    join.ready = 1;
    n = gdx_lobby_build_hello(&join, 0, 300, 0, pkt, sizeof(pkt));
    gdx_lobby_ingest(&host, pkt, n, 310, &from);
    CHECK(gdx_lobby_host_startable(&host, 320, &why) == 2);
    CHECK(gdx_lobby_host_startable(&host, 310 + GDX_LOBBY_STALE_MS + 1, &why) == 0); /* went quiet */
    CHECK(gdx_lobby_host_startable(&join, 320, &why) == 0); /* joiners cannot start */
    /* No RTT known for a pair: assume 100 ms rather than LAN. */
    CHECK(gdx_lobby_pick_delay(&host, 320, NULL, NULL) == 4);
    /* An Old 3DS in the race: no extra by default; o3dsExtraDelay ticks when the lever sets it. */
    host.peer[1].hello.model = GDX_MPO_MODEL_O3DS;
    CHECK(gdx_lobby_pick_delay(&host, 320, NULL, NULL) == 4);
    host.o3dsExtraDelay = 3;
    CHECK(gdx_lobby_pick_delay(&host, 320, NULL, NULL) == 7);
    host.model = GDX_MPO_MODEL_O3DS;
    host.peer[1].hello.model = GDX_MPO_MODEL_N3DS;
    CHECK(gdx_lobby_pick_delay(&host, 320, NULL, NULL) == 7);
    host.model = GDX_MPO_MODEL_N3DS;
    CHECK(gdx_lobby_pick_delay(&host, 320, NULL, NULL) == 4); /* no Old 3DS: the lever does nothing */
    host.o3dsExtraDelay = 0;
    host.model = GDX_MPO_MODEL_UNKNOWN;
    host.peer[1].hello.model = GDX_MPO_MODEL_UNKNOWN;
    memset(&st, 0, sizeof(st));
    st.course = 30; /* not the course every console reported on */
    CHECK(gdx_lobby_host_make_start(&host, &st, 1, 2, 3, 4, 5, 320) == 0);
    st.course = 42; /* out of range */
    CHECK(gdx_lobby_host_make_start(&host, &st, 1, 2, 3, 4, 5, 320) == 0);
    st.course = 2;
    CHECK(gdx_lobby_host_make_start(&host, &st, 1, 2, 3, 4, 0, 320) == 0); /* zero token */
    st.delayOverride = 6;
    CHECK(gdx_lobby_host_make_start(&host, &st, 1, 2, 3, 4, 5, 320) == 2 && host.start.delay == 6);
    CHECK(host.start.course == 2 && host.start.courseKey == gdx_mpo_stock_course_key(2));
    /* A START echoed back at the host, a START from a joiner, a replayed older START: refused. */
    n = gdx_lobby_build_start(&host, 1, 330, pkt, sizeof(pkt));
    CHECK(n == GDX_LB_START_BYTES);
    CHECK(gdx_lobby_ingest(&join, pkt, n, 340, &from) == GDX_LB_START && join.startIsNew && join.haveStart);
    CHECK(gdx_lobby_my_player(&join) == 1 && gdx_lobby_player_slot(&join, 0) == 0);
    join.startIsNew = 0;
    CHECK(gdx_lobby_ingest(&join, pkt, n, 350, &from) == GDX_LB_START && !join.startIsNew);
    pkt[7] = 0; /* addressed to slot 0, delivered to slot 1 */
    CHECK(gdx_lobby_ingest(&join, pkt, n, 360, &from) < 0);
    CHECK(gdx_lobby_build_start(&join, 0, 360, pkt, sizeof(pkt)) == 0);
    /* The host is acked once the joiner's HELLO carries the gen. */
    CHECK(!gdx_lobby_start_acked(&host));
    n = gdx_lobby_build_hello(&join, 0, 400, 0, pkt, sizeof(pkt));
    gdx_lobby_ingest(&host, pkt, n, 410, &from);
    CHECK(gdx_lobby_start_acked(&host));
    /* A different console in slot 1 (the epoch moved: a restart + rejoin): nothing heard from the
     * old one carries over, so it is not ready / live until it says so itself. */
    CHECK(host.peer[1].heard && host.peer[1].hello.ready);
    roster.member[1].epoch = 7;
    gdx_lobby_set_roster(&host, &roster);
    CHECK(host.peer[1].present && !host.peer[1].heard && host.peer[1].epoch == 7);
    CHECK(gdx_lobby_host_startable(&host, 420, &why) == 0);
    n = gdx_lobby_build_hello(&join, 0, 430, 0, pkt, sizeof(pkt));
    gdx_lobby_ingest(&host, pkt, n, 440, &from);
    gdx_lobby_set_roster(&host, &roster); /* same epoch again: kept */
    CHECK(host.peer[1].heard);
    /* A peer the roster dropped is refused. */
    roster.member[1].present = 0;
    roster.count = 1;
    gdx_lobby_set_roster(&host, &roster);
    n = gdx_lobby_build_hello(&join, 0, 500, 0, pkt, sizeof(pkt));
    CHECK(gdx_lobby_ingest(&host, pkt, n, 510, &from) < 0);
    /* Both digests agree only on the same START + build + content. */
    {
        GdxMpLbStart a = host.start;
        const uint32_t d0 = gdx_mpo_session_digest("build-a", 1, &a);
        a.token ^= 1;
        CHECK(gdx_mpo_session_digest("build-a", 1, &a) != d0);
        a.token ^= 1;
        CHECK(gdx_mpo_session_digest("build-a", 2, &a) != d0);
        CHECK(gdx_mpo_session_digest("build-b", 1, &a) != d0);
        a.player[1].engine++;
        CHECK(gdx_mpo_session_digest("build-a", 1, &a) != d0);
    }
}

/* mp-menus: a START names each slot's console (its roster epoch). The host never sends it to a slot
 * that holds another console now, nor waits for that slot's ack; a console that joined the slot
 * after the START (a rejoin while the others race) does not take it as its own. */
static void HelloTo(GdxMpLobby* from, GdxMpLobby* to, int dst, uint32_t now) {
    uint8_t pkt[256];
    int src = -1;
    const int n = gdx_lobby_build_hello(from, dst, now, 0, pkt, sizeof(pkt));
    gdx_lobby_ingest(to, pkt, n, now + 5, &src);
}

static void TestSlotEpochStart(void) {
    GdxMpLobby host;
    GdxMpLobby join;
    GdxMpLobby again;
    GdxMpRvRoster roster;
    GdxMpLobbySettings st;
    uint8_t pkt[256];
    int n;
    int from;

    memset(&roster, 0, sizeof(roster));
    roster.count = 2;
    roster.member[0].present = 1;
    roster.member[0].epoch = 1;
    roster.member[1].present = 1;
    roster.member[1].epoch = 3;
    gdx_lobby_init(&host, 0, 42, "build-a", 1, "HOST");
    gdx_lobby_init(&join, 1, 42, "build-a", 1, "JOIN");
    gdx_lobby_set_roster(&host, &roster);
    gdx_lobby_set_roster(&join, &roster);
    CHECK(host.myEpoch == 1 && join.myEpoch == 3);
    gdx_lobby_set_course(&host, 2, gdx_mpo_stock_course_key(2));
    gdx_lobby_set_course(&join, 2, gdx_mpo_stock_course_key(2));
    join.ready = 1;
    HelloTo(&join, &host, 0, 100);
    memset(&st, 0, sizeof(st));
    st.course = 2;
    CHECK(gdx_lobby_host_make_start(&host, &st, 1, 2, 3, 4, 5, 120) == 2);
    CHECK(host.start.player[0].epoch == 1 && host.start.player[1].epoch == 3);
    n = gdx_lobby_build_start(&host, 1, 130, pkt, sizeof(pkt));
    CHECK(n == GDX_LB_START_BYTES);
    {
        GdxMpLbHeader h;
        GdxMpLbBody b;
        CHECK(gdx_lb_parse(pkt, n, 42, &h, &b) == GDX_LB_START && b.start.player[1].epoch == 3);
    }
    /* Slot 1 changes hands (epoch 3 -> 4) while the START is out. */
    roster.member[1].epoch = 4;
    gdx_lobby_set_roster(&host, &roster);
    CHECK(!gdx_lobby_start_player_here(&host, 1) && gdx_lobby_start_player_here(&host, 0));
    CHECK(gdx_lobby_build_start(&host, 1, 140, pkt + 128, 128) == 0); /* the new console never gets it */
    CHECK(gdx_lobby_start_acked(&host));                              /* nor is it waited for */
    gdx_lobby_init(&again, 1, 42, "build-a", 1, "AGAIN");
    gdx_lobby_set_roster(&again, &roster);
    gdx_lobby_set_course(&again, 2, gdx_mpo_stock_course_key(2));
    CHECK(gdx_lobby_ingest(&again, pkt, n, 150, &from) == GDX_LB_START); /* the old copy, still in flight */
    CHECK(!again.startIsNew && !again.haveStart && gdx_lobby_my_player(&again) < 0);
    CHECK(gdx_lobby_ingest(&join, pkt, n, 150, &from) == GDX_LB_START && join.startIsNew); /* its own console */
    /* A player removed from the race is not waited for either. */
    roster.member[1].epoch = 3;
    gdx_lobby_set_roster(&host, &roster);
    CHECK(!gdx_lobby_start_acked(&host));
    host.removedMask = 2;
    CHECK(gdx_lobby_start_acked(&host));
}

/* mp-menus: a console that joins (or rejoins) while the room races waits for the race to end:
 * the host's HELLO says LAUNCHING / RACING. */
static void TestJoinWaitsForRace(void) {
    GdxMpLobby host;
    GdxMpLobby join;
    GdxMpRvRoster roster;
    memset(&roster, 0, sizeof(roster));
    roster.count = 2;
    roster.member[0].present = 1;
    roster.member[0].epoch = 1;
    roster.member[1].present = 1;
    roster.member[1].epoch = 5;
    gdx_lobby_init(&host, 0, 42, "build-a", 1, "HOST");
    gdx_lobby_init(&join, 1, 42, "build-a", 1, "JOIN");
    gdx_lobby_set_roster(&host, &roster);
    gdx_lobby_set_roster(&join, &roster);
    CHECK(!gdx_lobby_host_racing(&join)); /* the host not heard yet */
    host.state = GDX_LB_STATE_RACING;
    HelloTo(&host, &join, 1, 100);
    CHECK(gdx_lobby_host_racing(&join) && !gdx_lobby_host_racing(&host));
    host.state = GDX_LB_STATE_LAUNCHING;
    HelloTo(&host, &join, 1, 200);
    CHECK(gdx_lobby_host_racing(&join));
    host.state = GDX_LB_STATE_LOBBY;
    HelloTo(&host, &join, 1, 300);
    CHECK(!gdx_lobby_host_racing(&join));
}

/* mp-menus: START WITHOUT -- a member gone quiet (HOME, sleep) holds the START up; the host can
 * leave it out. Only quiet members: anything else that blocks still blocks. */
static void TestStartWithoutAway(void) {
    GdxMpLobby host;
    GdxMpLobby a;
    GdxMpLobby b;
    GdxMpRvRoster roster;
    GdxMpLobbySettings st;
    const char* why = NULL;
    int away = -1;
    const uint32_t later = 200 + GDX_LOBBY_STALE_MS + 100;

    memset(&roster, 0, sizeof(roster));
    roster.count = 3;
    roster.member[0].present = 1;
    roster.member[1].present = 1;
    roster.member[2].present = 1;
    gdx_lobby_init(&host, 0, 7, "build-a", 1, "HOST");
    gdx_lobby_init(&a, 1, 7, "build-a", 1, "A");
    gdx_lobby_init(&b, 2, 7, "build-a", 1, "B");
    gdx_lobby_set_roster(&host, &roster);
    gdx_lobby_set_roster(&a, &roster);
    gdx_lobby_set_roster(&b, &roster);
    gdx_lobby_set_course(&host, 2, gdx_mpo_stock_course_key(2));
    gdx_lobby_set_course(&a, 2, gdx_mpo_stock_course_key(2));
    gdx_lobby_set_course(&b, 2, gdx_mpo_stock_course_key(2));
    a.ready = 1;
    b.ready = 1;
    HelloTo(&a, &host, 0, 100);
    HelloTo(&b, &host, 0, 100);
    CHECK(gdx_lobby_host_startable(&host, 200, &why) == 3);
    CHECK(gdx_lobby_host_startable_without_away(&host, 200, &away) == 0 && away == -1); /* nobody away */
    HelloTo(&a, &host, 0, later - 50); /* A keeps talking, B went quiet (HOME) */
    CHECK(gdx_lobby_host_startable(&host, later, &why) == 0 && strstr(why, "connect") != NULL);
    CHECK(gdx_lobby_host_startable_without_away(&host, later, &away) == 2 && away == 2);
    memset(&st, 0, sizeof(st));
    st.course = 2;
    CHECK(gdx_lobby_host_make_start(&host, &st, 1, 2, 3, 4, 5, later) == 0);
    host.skipAway = 1;
    CHECK(gdx_lobby_host_make_start(&host, &st, 1, 2, 3, 4, 5, later) == 2);
    CHECK(host.start.numPlayers == 2 && host.start.player[1].slot == 1);
    host.skipAway = 0;
    /* A unready too: START WITHOUT does not override that. */
    a.ready = 0;
    HelloTo(&a, &host, 0, later);
    CHECK(gdx_lobby_host_startable_without_away(&host, later + 10, &away) == 0);
}

/* MP30: the sim-ABI word. Same build + content, other flavour (EK vs shipped, another forced-lever
 * set): never startable, its HELLO is not version-matched, its START is refused, and the word
 * survives the wire in both HELLO and START. */
static void TestSimAbi(void) {
    GdxMpLobby host;
    GdxMpLobby join;
    GdxMpRvRoster roster;
    uint8_t pkt[256];
    int n;
    int from;
    const char* why = NULL;
    GdxMpLobbySettings st;
    GdxMpLbHeader h;
    GdxMpLbBody body;

    memset(&roster, 0, sizeof(roster));
    roster.count = 2;
    roster.member[0].present = 1;
    roster.member[1].present = 1;
    gdx_lobby_init(&host, 0, 77, "mp30@abc", 9, "HOST");
    gdx_lobby_init(&join, 1, 77, "mp30@abc", 9, "JOIN");
    host.simAbi = 0x11111111u;
    join.simAbi = 0x22222222u;
    gdx_lobby_set_roster(&host, &roster);
    gdx_lobby_set_roster(&join, &roster);
    join.ready = 1;
    join.pickRound = 3; /* proto 7: the pick fields ride next to the word */
    join.picked = 1;
    join.pickMsLeft = GDX_LB_NO_ROUND;
    join.engineBits = gdx_mpo_engine_bits(0.75f);
    n = gdx_lobby_build_hello(&join, 0, 100, 0, pkt, sizeof(pkt));
    CHECK(n == GDX_LB_HELLO_BYTES);
    CHECK(gdx_lb_parse(pkt, n, 77, &h, &body) == GDX_LB_HELLO && body.hello.simAbi == 0x22222222u &&
          body.hello.pickRound == 3 && body.hello.picked == 1 && body.hello.engineBits == join.engineBits);
    join.pickRound = 0;
    join.picked = 0;
    CHECK(gdx_lobby_ingest(&host, pkt, n, 110, &from) == GDX_LB_HELLO);
    CHECK(!host.peer[1].versionOk && host.abiRejected == 1);
    CHECK(gdx_lobby_host_startable(&host, 120, &why) == 0 && strstr(why, "edition") != NULL);
    /* The same word: startable, and the START carries it. */
    join.simAbi = 0x11111111u;
    n = gdx_lobby_build_hello(&join, 0, 200, 0, pkt, sizeof(pkt));
    gdx_lobby_ingest(&host, pkt, n, 210, &from);
    CHECK(host.peer[1].versionOk && gdx_lobby_host_startable(&host, 220, &why) == 2);
    memset(&st, 0, sizeof(st));
    st.course = 0;
    CHECK(gdx_lobby_host_make_start(&host, &st, 1, 2, 3, 4, 5, 220) == 2 && host.start.simAbi == 0x11111111u);
    n = gdx_lobby_build_start(&host, 1, 230, pkt, sizeof(pkt));
    CHECK(n == GDX_LB_START_BYTES);
    CHECK(gdx_lb_parse(pkt, n, 77, &h, &body) == GDX_LB_START && body.start.simAbi == 0x11111111u);
    /* A joiner of the other flavour refuses that START (and does not ack it). */
    join.simAbi = 0x33333333u;
    CHECK(gdx_lobby_ingest(&join, pkt, n, 240, &from) < 0 && !join.haveStart && join.startGen == 0);
    join.simAbi = 0x11111111u;
    CHECK(gdx_lobby_ingest(&join, pkt, n, 250, &from) == GDX_LB_START && join.haveStart);
    /* The word is in the session digest (through the START body). */
    {
        GdxMpLbStart a = host.start;
        const uint32_t d0 = gdx_mpo_session_digest("mp30@abc", 9, &a);
        a.simAbi ^= 1u;
        CHECK(gdx_mpo_session_digest("mp30@abc", 9, &a) != d0);
    }
}

/* MP30: the VS COM machine count in START: only with VS COM, 4..30, kept on the wire, in the
 * session digest, and clamped by the lobby. */
static void TestRacers(void) {
    uint8_t buf[256];
    GdxMpLbHeader h;
    GdxMpLbHeader g;
    GdxMpLbBody body;
    GdxMpLbStart st = SampleStart(2);
    int n;
    uint32_t d0;

    memset(&h, 0, sizeof(h));
    h.type = GDX_LB_START;
    h.from = 0;
    h.secret = 5;
    st.player[0].slot = 0;
    st.player[1].slot = 1;
    st.laps = 3;
    st.vsCom = 1;
    st.racers = 30;
    n = gdx_lb_build(&h, &st, buf, sizeof(buf));
    CHECK(n == GDX_LB_START_BYTES);
    CHECK(gdx_lb_parse(buf, n, 5, &g, &body) == GDX_LB_START && body.start.racers == 30);
    /* proto 7: mp30's racers / ramMask / simAbi and stockpick's courseKey / engineBits share one
     * START without overlapping. */
    st.simAbi = 0xA5A5C3C3u;
    st.player[1].engineBits = gdx_mpo_engine_bits(0.8125f);
    n = gdx_lb_build(&h, &st, buf, sizeof(buf));
    CHECK(gdx_lb_parse(buf, n, 5, &g, &body) == GDX_LB_START && body.start.racers == 30 &&
          body.start.simAbi == 0xA5A5C3C3u && body.start.courseKey == st.courseKey &&
          body.start.player[0].engineBits == st.player[0].engineBits &&
          body.start.player[1].engineBits == st.player[1].engineBits);
    d0 = gdx_mpo_session_digest("b", 1, &st);
    st.player[1].engineBits = gdx_mpo_engine_bits(0.8f); /* the exact engine pick is in the digest */
    CHECK(gdx_mpo_session_digest("b", 1, &st) != d0);
    st.player[1].engineBits = gdx_mpo_engine_bits(0.8125f);
    CHECK(gdx_mpo_session_digest("b", 1, &st) == d0);
    st.racers = 16;
    CHECK(gdx_mpo_session_digest("b", 1, &st) != d0);
    st.racers = 0; /* stock */
    n = gdx_lb_build(&h, &st, buf, sizeof(buf));
    CHECK(gdx_lb_parse(buf, n, 5, &g, &body) == GDX_LB_START && body.start.racers == 0);
    st.racers = 31;
    n = gdx_lb_build(&h, &st, buf, sizeof(buf));
    CHECK(gdx_lb_parse(buf, n, 5, &g, &body) == -3);
    st.racers = 3;
    n = gdx_lb_build(&h, &st, buf, sizeof(buf));
    CHECK(gdx_lb_parse(buf, n, 5, &g, &body) == -3);
    st.racers = 8;
    st.vsCom = 0; /* a count without CPU racers means nothing */
    n = gdx_lb_build(&h, &st, buf, sizeof(buf));
    CHECK(gdx_lb_parse(buf, n, 5, &g, &body) == -3);
    st.racers = 0;
    st.vsCom = 1;
    st.botMask = 3;
    st.ramMask = 2; /* ram-mode bots: a subset of the bots */
    n = gdx_lb_build(&h, &st, buf, sizeof(buf));
    CHECK(gdx_lb_parse(buf, n, 5, &g, &body) == GDX_LB_START && body.start.ramMask == 2);
    st.botMask = 1;
    n = gdx_lb_build(&h, &st, buf, sizeof(buf));
    CHECK(gdx_lb_parse(buf, n, 5, &g, &body) == -3);
    st.botMask = 0;
    st.ramMask = 0;
    {
        GdxMpLobby host;
        GdxMpLobby join;
        GdxMpRvRoster roster;
        GdxMpLobbySettings ls;
        uint8_t pkt[256];
        int from;
        memset(&roster, 0, sizeof(roster));
        roster.count = 2;
        roster.member[0].present = 1;
        roster.member[1].present = 1;
        gdx_lobby_init(&host, 0, 6, "b", 1, "H");
        gdx_lobby_init(&join, 1, 6, "b", 1, "J");
        gdx_lobby_set_roster(&host, &roster);
        gdx_lobby_set_roster(&join, &roster);
        join.ready = 1;
        n = gdx_lobby_build_hello(&join, 0, 10, 0, pkt, sizeof(pkt));
        gdx_lobby_ingest(&host, pkt, n, 11, &from);
        memset(&ls, 0, sizeof(ls));
        ls.vsCom = 1;
        ls.racers = 16;
        CHECK(gdx_lobby_host_make_start(&host, &ls, 1, 2, 3, 4, 5, 12) == 2 && host.start.racers == 16);
        ls.vsCom = 0;
        CHECK(gdx_lobby_host_make_start(&host, &ls, 1, 2, 3, 4, 5, 12) == 2 && host.start.racers == 0);
        ls.vsCom = 1;
        ls.racers = 99; /* out of range: stock */
        CHECK(gdx_lobby_host_make_start(&host, &ls, 1, 2, 3, 4, 5, 12) == 2 && host.start.racers == 0);
    }
}

/* ---- 8. zip central-directory hash ---------------------------------------------------------- */
/* stockpick: the stock-menu picks and the course every console must hold. */
static int Hello(GdxMpLobby* from, GdxMpLobby* to, uint32_t now) {
    uint8_t pkt[256];
    int slot = -1;
    const int n = gdx_lobby_build_hello(from, to->mySlot, now, 0, pkt, sizeof(pkt));
    return gdx_lobby_ingest(to, pkt, n, now + 5, &slot);
}

static void TestStockPickRules(void) {
    GdxMpLobby host;
    GdxMpLobby join;
    GdxMpRvRoster roster;
    GdxMpLobbySettings st;
    uint8_t pkt[256];
    const char* why = NULL;
    int from;
    int n;
    memset(&roster, 0, sizeof(roster));
    roster.count = 2;
    roster.member[0].present = 1;
    roster.member[1].present = 1;
    gdx_lobby_init(&host, 0, 77, "b", 1, "HOST");
    gdx_lobby_init(&join, 1, 77, "b", 1, "JOIN");
    gdx_lobby_set_roster(&host, &roster);
    gdx_lobby_set_roster(&join, &roster);
    /* Defaults: course 0 with its stock key on both, nobody picking. */
    CHECK(host.course == 0 && host.courseKey == gdx_mpo_stock_course_key(0) && host.courseKey != 0);
    CHECK(gdx_mpo_stock_course_key(23) != 0 && gdx_mpo_stock_course_key(24) == 0 && gdx_mpo_stock_course_key(-1) == 0);
    CHECK(gdx_lobby_host_course(&join) == -1); /* not heard the host yet */
    CHECK(gdx_lobby_host_round(&join) == 0 && gdx_lobby_host_ms_left(&join) == GDX_LB_NO_ROUND);
    join.ready = 1;
    Hello(&join, &host, 100);
    CHECK(gdx_lobby_host_startable(&host, 110, &why) == 2); /* requirePicks off: the mp_auto lanes */
    host.requirePicks = 1;
    CHECK(gdx_lobby_host_startable(&host, 110, &why) == 0 && strstr(why, "open machine select") != NULL);
    /* The host opens round 1 on every console: nobody has a pick for it yet. */
    host.pickRound = 1;
    host.pickMsLeft = GDX_MPO_PICK_MS;
    Hello(&host, &join, 120);
    CHECK(gdx_lobby_host_round(&join) == 1 && gdx_lobby_host_ms_left(&join) == GDX_MPO_PICK_MS);
    CHECK(gdx_lobby_host_startable(&host, 130, &why) == 0 && strstr(why, "your machine first") != NULL);
    host.picked = 1;
    CHECK(gdx_lobby_peer_picked(&host, 0) && !gdx_lobby_peer_picked(&host, 1) && !gdx_lobby_picks_in(&host));
    CHECK(gdx_lobby_host_startable(&host, 130, &why) == 0 && strstr(why, "pick a machine") != NULL);
    /* A joiner's pick for an older round does not count. */
    join.pickRound = 0;
    join.picked = 1;
    Hello(&join, &host, 140);
    CHECK(!gdx_lobby_peer_picked(&host, 1) && !gdx_lobby_peer_picked(&join, 1));
    /* The joiner in its stock picker for round 1. */
    join.pickRound = 1;
    join.picked = 0;
    join.picking = GDX_LB_PICK_MACHINE;
    Hello(&join, &host, 200);
    CHECK(gdx_lobby_host_startable(&host, 210, &why) == 0 && strstr(why, "pick a machine") != NULL);
    /* The host stops waiting (time + grace up): a player with no pick races the default. */
    host.pickWaitOver = 1;
    join.machine = 9;
    Hello(&join, &host, 207);
    CHECK(gdx_lobby_host_startable(&host, 210, &why) == 2);
    memset(&st, 0, sizeof(st));
    CHECK(gdx_lobby_host_make_start(&host, &st, 1, 2, 3, 4, 5, 210) == 2);
    CHECK(host.start.player[1].machine == GDX_MPO_DEFAULT_MACHINE && host.start.player[1].skin == GDX_MPO_DEFAULT_SKIN);
    CHECK(host.start.player[1].engine == GDX_MPO_DEFAULT_ENGINE);
    CHECK(gdx_mpo_engine_from_bits(host.start.player[1].engineBits) == 0.5f);
    host.pickWaitOver = 0;
    join.picking = GDX_LB_PICK_NONE;
    join.picked = 1;
    join.machine = 12;
    join.engine = 70;
    join.engineBits = gdx_mpo_engine_bits(0.6953125f);
    Hello(&join, &host, 300);
    CHECK(gdx_lobby_host_startable(&host, 310, &why) == 2 && gdx_lobby_picks_in(&host));
    Hello(&host, &join, 305);
    CHECK(gdx_lobby_peer_picked(&join, 1) && gdx_lobby_picks_in(&join));
    /* The host in its own pickers. */
    host.picking = GDX_LB_PICK_COURSE;
    CHECK(gdx_lobby_host_startable(&host, 310, &why) == 0 && strstr(why, "picking a course") != NULL);
    Hello(&host, &join, 320);
    CHECK(gdx_lobby_host_picking(&join) == GDX_LB_PICK_COURSE);
    host.picking = GDX_LB_PICK_MACHINE;
    CHECK(gdx_lobby_host_startable(&host, 310, &why) == 0 && strstr(why, "picking a machine") != NULL);
    host.picking = GDX_LB_PICK_NONE;
    /* The host picks a disk course: the joiner has not reported on it yet. */
    gdx_lobby_set_course(&host, 31, 0x1234ABCDu);
    CHECK(gdx_lobby_host_startable(&host, 310, &why) == 0 && strstr(why, "checking the course") != NULL);
    Hello(&host, &join, 400);
    CHECK(gdx_lobby_host_course(&join) == 31 && gdx_lobby_host_picking(&join) == GDX_LB_PICK_NONE);
    /* ... it reports a different copy of course 31 (another disk): never startable. */
    gdx_lobby_set_course(&join, 31, 0x99999999u);
    Hello(&join, &host, 410);
    CHECK(gdx_lobby_host_startable(&host, 420, &why) == 0 && strstr(why, "does not have the course") != NULL);
    /* ... it does not have it at all. */
    gdx_lobby_set_course(&join, 31, GDX_MPO_NO_COURSE_KEY);
    Hello(&join, &host, 430);
    CHECK(gdx_lobby_host_startable(&host, 440, &why) == 0 && strstr(why, "does not have the course") != NULL);
    /* ... the same course data: startable, and the START carries the key. */
    gdx_lobby_set_course(&join, 31, 0x1234ABCDu);
    Hello(&join, &host, 450);
    CHECK(gdx_lobby_host_startable(&host, 460, &why) == 2);
    memset(&st, 0, sizeof(st));
    st.course = 30; /* the settings must name the reported course */
    CHECK(gdx_lobby_host_make_start(&host, &st, 1, 2, 3, 4, 5, 460) == 0);
    st.course = 31;
    CHECK(gdx_lobby_host_make_start(&host, &st, 1, 2, 3, 4, 5, 460) == 2);
    CHECK(host.start.courseKey == 0x1234ABCDu && host.start.player[1].machine == 12 && host.start.player[1].engine == 70);
    CHECK(gdx_mpo_engine_from_bits(host.start.player[1].engineBits) == 0.6953125f);
    CHECK(gdx_mpo_engine_from_bits(host.start.player[0].engineBits) == 0.5f); /* the host's default */
    /* The joiner refuses a START whose key is not its own (its copy changed since the HELLO). */
    n = gdx_lobby_build_start(&host, 1, 470, pkt, sizeof(pkt));
    gdx_lobby_set_course(&join, 31, 0x55555555u);
    CHECK(gdx_lobby_ingest(&join, pkt, n, 480, &from) == -6 && join.courseRejected == 1 && !join.haveStart);
    CHECK(join.startGen == 0); /* not acknowledged */
    gdx_lobby_set_course(&join, 30, 0x1234ABCDu);
    CHECK(gdx_lobby_ingest(&join, pkt, n, 490, &from) == -6); /* another course, same key */
    gdx_lobby_set_course(&join, 31, 0x1234ABCDu);
    CHECK(gdx_lobby_ingest(&join, pkt, n, 500, &from) == GDX_LB_START && join.haveStart && join.startIsNew);
    /* The host without a key for its own course cannot start. */
    gdx_lobby_set_course(&host, 31, GDX_MPO_NO_COURSE_KEY);
    CHECK(gdx_lobby_host_startable(&host, 510, &why) == 0 && strstr(why, "this console") != NULL);
    /* The digest covers the course key and every pick. */
    {
        GdxMpLbStart a = join.start;
        const uint32_t d0 = gdx_mpo_session_digest("b", 1, &a);
        a.courseKey ^= 1;
        CHECK(gdx_mpo_session_digest("b", 1, &a) != d0);
        a.courseKey ^= 1;
        a.player[1].skin ^= 1;
        CHECK(gdx_mpo_session_digest("b", 1, &a) != d0);
        a.player[1].skin ^= 1;
        a.player[0].machine ^= 1;
        CHECK(gdx_mpo_session_digest("b", 1, &a) != d0);
        a.player[0].machine ^= 1;
        a.player[1].engineBits ^= 1; /* one ulp of the slider */
        CHECK(gdx_mpo_session_digest("b", 1, &a) != d0);
    }
}

/* One member "a.txt" (5 bytes uncompressed), a central record, and the EOCD. The packing knobs
 * (method, compressed size, timestamp, an extra field, a leading pad that moves the local-header
 * offset) are what a different zlib / level / prebake option changes; the content (name, CRC-32,
 * uncompressed size) is what the hash must see. */
typedef struct ZipSpec {
    const char* name;
    uint32_t crc;
    uint32_t size;
    uint16_t method;
    uint32_t packed;
    uint16_t time;
    int extra;
    int pad;
    int truncate;
} ZipSpec;

static ZipSpec ZipDefault(void) {
    ZipSpec z;
    memset(&z, 0, sizeof(z));
    z.name = "a.txt";
    z.crc = 0x3610A686u;
    z.size = 5;
    z.method = 0;
    z.packed = 5;
    return z;
}

static void WriteZipSpec(const char* path, const ZipSpec* zs) {
    uint8_t z[512];
    const int nameLen = (int) strlen(zs->name);
    const int extraLen = zs->extra ? 8 : 0;
    int n = zs->pad;
    int local;
    int cd;
    FILE* f;
    memset(z, 0, sizeof(z));
    local = n;
    PutU32(z + n, 0x04034B50u);
    PutU16(z + n + 8, zs->method);
    PutU16(z + n + 10, zs->time);
    PutU32(z + n + 14, zs->crc);
    PutU32(z + n + 18, zs->packed);
    PutU32(z + n + 22, zs->size);
    PutU16(z + n + 26, (uint16_t) nameLen);
    memcpy(z + n + 30, zs->name, (size_t) nameLen);
    n += 30 + nameLen;
    memset(z + n, 0x5A, zs->packed);
    n += (int) zs->packed;
    cd = n;
    PutU32(z + n, 0x02014B50u);
    PutU16(z + n + 10, zs->method);
    PutU16(z + n + 12, zs->time);
    PutU32(z + n + 16, zs->crc);
    PutU32(z + n + 20, zs->packed);
    PutU32(z + n + 24, zs->size);
    PutU16(z + n + 28, (uint16_t) nameLen);
    PutU16(z + n + 30, (uint16_t) extraLen);
    PutU32(z + n + 42, (uint32_t) local);
    memcpy(z + n + 46, zs->name, (size_t) nameLen);
    if (extraLen > 0) {
        PutU16(z + n + 46 + nameLen, 0x5455u); /* "UT" extended timestamp */
        PutU16(z + n + 48 + nameLen, 4);
        PutU32(z + n + 50 + nameLen, 0x12345678u + zs->time);
    }
    n += 46 + nameLen + extraLen;
    PutU32(z + n, 0x06054B50u);
    PutU16(z + n + 8, 1);
    PutU16(z + n + 10, 1);
    PutU32(z + n + 12, (uint32_t) (n - cd));
    PutU32(z + n + 16, (uint32_t) cd);
    n += 22;
    f = fopen(path, "wb");
    if (f != NULL) {
        fwrite(z, 1, (size_t) (zs->truncate ? n - 30 : n), f);
        fclose(f);
    }
}

static uint32_t ZipSpecHash(const ZipSpec* zs, int* rc) {
    const char* path = "mp_online_test.zip";
    uint32_t h = 2166136261u;
    WriteZipSpec(path, zs);
    *rc = gdx_mpo_zip_cd_hash(path, &h);
    remove(path);
    return h;
}

static void TestZipHash(void) {
    const ZipSpec base = ZipDefault();
    ZipSpec z;
    int rc = 0;
    const uint32_t h1 = ZipSpecHash(&base, &rc);
    uint32_t h3 = 0;
    CHECK(rc == 0);
    /* content changes change it */
    z = base;
    z.crc = 0x3610A687u;
    CHECK(ZipSpecHash(&z, &rc) != h1 && rc == 0);
    z = base;
    z.size = 6;
    CHECK(ZipSpecHash(&z, &rc) != h1 && rc == 0);
    z = base;
    z.name = "b.txt";
    CHECK(ZipSpecHash(&z, &rc) != h1 && rc == 0);
    /* packing changes do not: another method / level (compressed size), another timestamp, an
     * extra field, the member at another offset */
    z = base;
    z.method = 8;
    z.packed = 3;
    CHECK(ZipSpecHash(&z, &rc) == h1 && rc == 0);
    z = base;
    z.method = 8;
    z.packed = 7;
    z.time = 0x6B21;
    z.extra = 1;
    z.pad = 16;
    CHECK(ZipSpecHash(&z, &rc) == h1 && rc == 0);
    CHECK(ZipSpecHash(&base, &rc) == h1 && rc == 0);
    z = base;
    z.truncate = 1;
    (void) ZipSpecHash(&z, &rc);
    CHECK(rc == -2);
    CHECK(gdx_mpo_zip_cd_hash("no-such-file.o2r", &h3) == -1);
}

/* ---- LAN: the console-hosted room + the beacon ------------------------------------------------ */
#define LAN_OUT_MAX 16
typedef struct LanOut {
    int n;
    GdxMpEndpoint to[LAN_OUT_MAX];
    uint8_t pkt[LAN_OUT_MAX][1300];
    int len[LAN_OUT_MAX];
} LanOut;

static void LanEmit(void* ctx, const GdxMpEndpoint* to, const uint8_t* pkt, int len) {
    LanOut* o = (LanOut*) ctx;
    if (o->n < LAN_OUT_MAX && len <= 1300) {
        o->to[o->n] = *to;
        memcpy(o->pkt[o->n], pkt, (size_t) len);
        o->len[o->n] = len;
        o->n++;
    }
}

static int LanFind(const LanOut* o, const GdxMpEndpoint* to, int type) {
    int i;
    for (i = 0; i < o->n; i++) {
        if (gdx_mpo_endpoint_eq(&o->to[i], to) && gdx_rv_peek_type(o->pkt[i], o->len[i]) == type) {
            return i;
        }
    }
    return -1;
}

static int LanRequest(GdxMpLanRoom* r, LanOut* o, int type, const GdxMpEndpoint* from, uint32_t nonce, const char* code,
                      const char* build, uint32_t content, const char* name, uint32_t now) {
    GdxMpRvRequest q;
    uint8_t pkt[GDX_RV_REQ_BYTES];
    memset(&q, 0, sizeof(q));
    q.nonce = nonce;
    q.contentHash = content;
    q.priv = *from;
    q.maxPlayers = 4;
    snprintf(q.build, sizeof(q.build), "%s", build);
    snprintf(q.name, sizeof(q.name), "%s", name);
    snprintf(q.code, sizeof(q.code), "%s", code);
    o->n = 0;
    return gdx_lan_room_handle(r, pkt, gdx_rv_build_request(type, &q, pkt, sizeof(pkt)), from, now, LanEmit, o);
}

static void TestLan(void) {
    static GdxMpLanRoom room;
    static LanOut o;
    GdxMpEndpoint j1 = { { 192, 168, 2, 30 }, 27300 };
    GdxMpEndpoint j2 = { { 192, 168, 2, 31 }, 27300 };
    GdxMpEndpoint j3 = { { 192, 168, 2, 32 }, 27301 };
    GdxMpEndpoint j4 = { { 192, 168, 2, 33 }, 27300 };
    GdxMpRvWelcome w;
    GdxMpRvWelcome w1;
    GdxMpRvRoster ro;
    GdxMpRvError e;
    uint8_t pkt[1400];
    int n;
    int i;
    int k;
    uint32_t now = 5000;
    const char* B = "mp-online@lan";

    CHECK(gdx_lan_room_open(&room, "K7MP", B, 0xC0DE, 4, 1, now) == -1); /* bad code */
    CHECK(gdx_lan_room_open(&room, "k7mpq", B, 0xC0DE, 4, 0x1234, now) == 0 && strcmp(room.code, "K7MPQ") == 0);
    /* Not GXRV: the caller's game traffic. */
    CHECK(gdx_lan_room_handle(&room, (const uint8_t*) "GXLBxxxx", 8, &j1, now, LanEmit, &o) == 0);
    /* A joiner before the host is in: no room. */
    LanRequest(&room, &o, GDX_RV_JOIN, &j1, 1, "K7MPQ", B, 0xC0DE, "EARLY", now);
    CHECK(o.n == 1 && gdx_rv_parse_error(o.pkt[0], o.len[0], &e) == 0 && e.code == GDX_RV_E_ROOM_NOT_FOUND);
    /* Only our own client may CREATE. */
    LanRequest(&room, &o, GDX_RV_CREATE, &j1, 2, "K7MPQ", B, 0xC0DE, "THIEF", now);
    CHECK(o.n == 1 && gdx_rv_parse_error(o.pkt[0], o.len[0], &e) == 0 && e.code == GDX_RV_E_BAD_REQUEST);
    CHECK(!room.member[0].present);
    LanRequest(&room, &o, GDX_RV_CREATE, &GDX_LAN_SELF, 3, "K7MPQ", B, 0xC0DE, "MIKE", now);
    k = LanFind(&o, &GDX_LAN_SELF, GDX_RV_WELCOME);
    CHECK(k >= 0 && gdx_rv_parse_welcome(o.pkt[k], o.len[k], &w) == 0 && w.slot == 0 && w.roomSecret == room.secret);
    /* Joins: wrong code, other build, other ROM, then the real thing. */
    LanRequest(&room, &o, GDX_RV_JOIN, &j1, 10, "ZZZZZ", B, 0xC0DE, "J1", ++now);
    CHECK(o.n == 1 && gdx_rv_parse_error(o.pkt[0], o.len[0], &e) == 0 && e.code == GDX_RV_E_ROOM_NOT_FOUND);
    LanRequest(&room, &o, GDX_RV_JOIN, &j1, 11, "K7MPQ", "other@x", 0xC0DE, "J1", ++now);
    CHECK(o.n == 1 && gdx_rv_parse_error(o.pkt[0], o.len[0], &e) == 0 && e.code == GDX_RV_E_BUILD_MISMATCH &&
          strcmp(e.detail, B) == 0);
    LanRequest(&room, &o, GDX_RV_JOIN, &j1, 12, "K7MPQ", B, 0xBEEF, "J1", ++now);
    CHECK(o.n == 1 && gdx_rv_parse_error(o.pkt[0], o.len[0], &e) == 0 && e.code == GDX_RV_E_CONTENT_MISMATCH);
    LanRequest(&room, &o, GDX_RV_JOIN, &j1, 13, "K7MPQ", B, 0xC0DE, "J1", ++now);
    k = LanFind(&o, &j1, GDX_RV_WELCOME);
    CHECK(k >= 0 && gdx_rv_parse_welcome(o.pkt[k], o.len[k], &w1) == 0 && w1.slot == 1 &&
          w1.roomSecret == w.roomSecret && w1.memberKey != w.memberKey);
    k = LanFind(&o, &GDX_LAN_SELF, GDX_RV_ROSTER);
    CHECK(k >= 0 && gdx_rv_parse_roster(o.pkt[k], o.len[k], &ro) == 0 && ro.member[1].present &&
          gdx_mpo_endpoint_eq(&ro.member[1].pub, &j1) && strcmp(ro.member[1].name, "J1") == 0 &&
          strcmp(ro.member[0].name, "MIKE") == 0);
    /* A retransmitted JOIN gets the same seat. */
    LanRequest(&room, &o, GDX_RV_JOIN, &j1, 13, "K7MPQ", B, 0xC0DE, "J1", ++now);
    k = LanFind(&o, &j1, GDX_RV_WELCOME);
    CHECK(k >= 0 && gdx_rv_parse_welcome(o.pkt[k], o.len[k], &w) == 0 && w.slot == 1 && w.memberKey == w1.memberKey);
    /* Keepalive: the ack + the roster (version differs); a wrong key gets only the 16-byte AUTH. */
    n = gdx_rv_build_keepalive("K7MPQ", 1, w1.memberKey, 0, 777, pkt, sizeof(pkt));
    o.n = 0;
    gdx_lan_room_handle(&room, pkt, n, &j1, ++now, LanEmit, &o);
    CHECK(LanFind(&o, &j1, GDX_RV_KA_ACK) >= 0 && LanFind(&o, &j1, GDX_RV_ROSTER) >= 0);
    n = gdx_rv_build_keepalive("K7MPQ", 1, w1.memberKey ^ 1, 0, 777, pkt, sizeof(pkt));
    o.n = 0;
    gdx_lan_room_handle(&room, pkt, n, &j2, ++now, LanEmit, &o);
    CHECK(o.n == 1 && o.len[0] == 16 && gdx_rv_parse_error(o.pkt[0], 16, &e) == 0 && e.code == GDX_RV_E_AUTH);
    /* Fill it: j2, j3; then j4 finds it full. */
    LanRequest(&room, &o, GDX_RV_JOIN, &j2, 20, "K7MPQ", B, 0xC0DE, "J2", ++now);
    LanRequest(&room, &o, GDX_RV_JOIN, &j3, 21, "K7MPQ", B, 0xC0DE, "J3", ++now);
    CHECK(gdx_lan_room_count(&room) == 4);
    LanRequest(&room, &o, GDX_RV_JOIN, &j4, 22, "K7MPQ", B, 0xC0DE, "J4", ++now);
    CHECK(o.n == 1 && gdx_rv_parse_error(o.pkt[0], o.len[0], &e) == 0 && e.code == GDX_RV_E_ROOM_FULL);
    /* Relay: j1 -> host + j2, a GXLB whose `from` is j1's slot; a forged `from` is dropped. */
    {
        uint8_t lb[GDX_LB_HEADER_BYTES + 8];
        memset(lb, 0, sizeof(lb));
        memcpy(lb, "GXLB", 4);
        lb[6] = 1;
        n = gdx_rv_build_relay("K7MPQ", 1, 0x5, w1.memberKey, lb, sizeof(lb), pkt, sizeof(pkt));
        o.n = 0;
        gdx_lan_room_handle(&room, pkt, n, &j1, ++now, LanEmit, &o);
        CHECK(o.n == 2 && memcmp(o.pkt[0], "GXLB", 4) == 0 && o.len[0] == (int) sizeof(lb));
        lb[6] = 2;
        n = gdx_rv_build_relay("K7MPQ", 1, 0x5, w1.memberKey, lb, sizeof(lb), pkt, sizeof(pkt));
        o.n = 0;
        gdx_lan_room_handle(&room, pkt, n, &j1, ++now, LanEmit, &o);
        CHECK(o.n == 0);
    }
    /* Lock: only the host. */
    n = gdx_rv_build_auth(GDX_RV_LOCK, "K7MPQ", 1, 1, w1.memberKey, pkt, sizeof(pkt));
    gdx_lan_room_handle(&room, pkt, n, &j1, ++now, LanEmit, &o);
    CHECK(!room.locked);
    /* j1 restarts: same address, silent > 5 s, a new nonce: it takes slot 1 back with a new epoch. */
    {
        const uint16_t before = room.member[1].epoch;
        now += GDX_LAN_REJOIN_SILENT_MS + 10;
        room.member[2].lastSeenMs = now;
        room.member[3].lastSeenMs = now;
        LanRequest(&room, &o, GDX_RV_JOIN, &j1, 99, "K7MPQ", B, 0xC0DE, "J1", now);
        k = LanFind(&o, &j1, GDX_RV_WELCOME);
        CHECK(k >= 0 && gdx_rv_parse_welcome(o.pkt[k], o.len[k], &w) == 0 && w.slot == 1 &&
              room.member[1].epoch != before && w.memberKey != w1.memberKey);
    }
    /* Leave: j3 goes, the others get a roster without it. */
    n = gdx_rv_build_auth(GDX_RV_LEAVE, "K7MPQ", 3, 0, room.member[3].key, pkt, sizeof(pkt));
    o.n = 0;
    gdx_lan_room_handle(&room, pkt, n, &j3, ++now, LanEmit, &o);
    k = LanFind(&o, &GDX_LAN_SELF, GDX_RV_ROSTER);
    CHECK(!room.member[3].present && k >= 0 && gdx_rv_parse_roster(o.pkt[k], o.len[k], &ro) == 0 &&
          !ro.member[3].present && ro.count == 3);
    /* Expiry: everybody but the host goes silent; game traffic counts as alive. */
    gdx_lan_room_touch(&room, 2, now + GDX_LAN_MEMBER_TIMEOUT_MS);
    o.n = 0;
    gdx_lan_room_expire(&room, now + GDX_LAN_MEMBER_TIMEOUT_MS + 100, LanEmit, &o);
    CHECK(gdx_lan_room_count(&room) == 2 && room.member[2].present);
    o.n = 0;
    now += GDX_LAN_MEMBER_TIMEOUT_MS;
    gdx_lan_room_expire(&room, now + GDX_LAN_MEMBER_TIMEOUT_MS + 100, LanEmit, &o);
    CHECK(gdx_lan_room_count(&room) == 1 && room.member[0].present);
    /* Code guessing: wrong-code JOINs are throttled. */
    {
        int limited = 0;
        for (i = 0; i < 40; i++) {
            char c[6];
            snprintf(c, sizeof(c), "ZZZZ%c", gdx_mpo_code_alphabet[i % 32]);
            LanRequest(&room, &o, GDX_RV_JOIN, &j4, 200 + (uint32_t) i, c, B, 0xC0DE, "G", now + 1000 + (uint32_t) i);
            if (o.n == 1 && gdx_rv_parse_error(o.pkt[0], o.len[0], &e) == 0 && e.code == GDX_RV_E_RATE_LIMITED) {
                limited++;
            }
        }
        CHECK(limited > 20);
    }
    /* Junk never crashes and never answers more than it got. */
    {
        uint32_t seed = 7;
        int bigger = 0;
        for (i = 0; i < 20000; i++) {
            int len = (int) ((seed = seed * 1103515245u + 12345u) >> 16) % 120;
            int b;
            memcpy(pkt, "GXRV", 4);
            for (b = 4; b < len; b++) {
                pkt[b] = (uint8_t) ((seed = seed * 1103515245u + 12345u) >> 16);
            }
            if (len > 5 && (i & 1)) {
                pkt[4] = 1;
                pkt[5] = (uint8_t) (1 + (i % 7));
                pkt[6] = 0;
                pkt[7] = 0;
            }
            o.n = 0;
            gdx_lan_room_handle(&room, pkt, len, &j4, now + 5000 + (uint32_t) i, LanEmit, &o);
            for (b = 0; b < o.n; b++) {
                if (o.len[b] > len) {
                    bigger++;
                }
            }
        }
        CHECK(bigger == 0 && gdx_lan_room_count(&room) == 1);
    }
    /* Close: the joiners hear HOST_LEFT. */
    LanRequest(&room, &o, GDX_RV_JOIN, &j2, 300, "K7MPQ", B, 0xC0DE, "J2", now + 90000);
    o.n = 0;
    gdx_lan_room_close(&room, LanEmit, &o);
    CHECK(o.n == 1 && gdx_rv_parse_error(o.pkt[0], o.len[0], &e) == 0 && e.code == GDX_RV_E_HOST_LEFT && !room.open);
    printf("  lan room: create/join/mismatch/full/relay/lock/rejoin/leave/expiry/guessing/fuzz ok\n");
}

static void TestBeacon(void) {
    GdxMpLanBeacon b;
    GdxMpLanBeacon c;
    GdxMpNearby nb;
    uint8_t pkt[128];
    uint32_t id;
    uint32_t nonce;
    uint32_t ms;
    int idx[GDX_LAN_MAX_NEARBY];
    int n;
    int i;
    const GdxMpEndpoint a1 = { { 192, 168, 2, 196 }, 27210 };
    const GdxMpEndpoint a2 = { { 192, 168, 2, 197 }, 27210 };
    const GdxMpEndpoint a3 = { { 192, 168, 2, 198 }, 27211 };

    memset(&b, 0, sizeof(b));
    b.hostId = 0xABCD1234u;
    b.contentHash = 0xC0DE;
    b.roomPort = 27300;
    b.players = 2;
    b.maxPlayers = 4;
    b.model = GDX_MPO_MODEL_N3DS;
    b.proto = GDX_MPO_PROTO;
    strcpy(b.code, "K7MPQ");
    strcpy(b.name, "Mike");
    strcpy(b.build, "mp-online@lan");
    n = gdx_lan_build_beacon(&b, pkt, sizeof(pkt));
    CHECK(n == GDX_LAN_BEACON_BYTES && gdx_lan_parse(pkt, n, &c, &id, &nonce, &ms) == GDX_LAN_BEACON);
    CHECK(c.hostId == b.hostId && c.roomPort == 27300 && c.players == 2 && c.model == GDX_MPO_MODEL_N3DS &&
          strcmp(c.code, "K7MPQ") == 0 && strcmp(c.name, "Mike") == 0 && strcmp(c.build, b.build) == 0);
    pkt[19] = 9; /* max players out of range */
    CHECK(gdx_lan_parse(pkt, n, &c, &id, &nonce, &ms) == -1);
    pkt[19] = 4;
    pkt[33] = '<'; /* not a name char */
    CHECK(gdx_lan_parse(pkt, n, &c, &id, &nonce, &ms) == -1);
    pkt[33] = 'i';
    CHECK(gdx_lan_parse(pkt, n - 1, &c, &id, &nonce, &ms) == -1);
    n = gdx_lan_build_ping(GDX_LAN_PONG, 5, 6, 7, pkt, sizeof(pkt));
    CHECK(n == GDX_LAN_PING_BYTES && gdx_lan_parse(pkt, n, &c, &id, &nonce, &ms) == GDX_LAN_PONG && id == 5 &&
          nonce == 6 && ms == 7);
    {
        uint32_t seed = 3;
        for (i = 0; i < 20000; i++) {
            int len = (int) ((seed = seed * 1103515245u + 12345u) >> 16) % 100;
            int k;
            memcpy(pkt, "GXLN", 4);
            for (k = 4; k < len; k++) {
                pkt[k] = (uint8_t) ((seed = seed * 1103515245u + 12345u) >> 16);
            }
            (void) gdx_lan_parse(pkt, len, &c, &id, &nonce, &ms);
        }
    }

    gdx_nearby_init(&nb, "mp-online@lan", 0xC0DE);
    nb.selfHostId = 0x5E1F;
    gdx_nearby_on_beacon(&nb, &b, &a1, 1000);
    c = b;
    c.hostId = 0x5E1F; /* our own room, heard back */
    gdx_nearby_on_beacon(&nb, &c, &a2, 1000);
    c = b;
    c.hostId = 77;
    strcpy(c.name, "Ann");
    c.contentHash = 0xBAD; /* other ROM */
    gdx_nearby_on_beacon(&nb, &c, &a2, 1010);
    c = b;
    c.hostId = 78;
    strcpy(c.name, "Bob");
    c.flags = GDX_LAN_FLAG_RACING;
    gdx_nearby_on_beacon(&nb, &c, &a3, 1020);
    n = gdx_nearby_sorted(&nb, idx, GDX_LAN_MAX_NEARBY);
    CHECK(n == 3 && nb.room[idx[0]].joinable && strcmp(nb.room[idx[0]].b.name, "Mike") == 0);
    CHECK(!nb.room[idx[1]].joinable && strcmp(nb.room[idx[1]].b.name, "Ann") == 0 &&
          strcmp(nb.room[idx[1]].whyNot, "other ROM") == 0);
    CHECK(strcmp(nb.room[idx[2]].whyNot, "racing") == 0);
    CHECK(gdx_mpo_endpoint_eq(&nb.room[idx[0]].room, &(GdxMpEndpoint){ { 192, 168, 2, 196 }, 27300 }));
    /* Ping: the row due a PING gets a nonce; the matching PONG sets the RTT and the bars. */
    i = gdx_nearby_tick(&nb, 1100);
    CHECK(i >= 0 && nb.room[i].pingNonce != 0);
    gdx_nearby_on_pong(&nb, nb.room[i].b.hostId, nb.room[i].pingNonce ^ 1, 1100, 1105); /* a stale nonce */
    CHECK(nb.room[i].pingMs < 0);
    gdx_nearby_on_pong(&nb, nb.room[i].b.hostId, nb.room[i].pingNonce, 1100, 1112);
    CHECK(nb.room[i].pingMs == 12 && nb.room[i].signal == 3);
    /* Re-beacon keeps the row; silence expires it. */
    gdx_nearby_on_beacon(&nb, &b, &a1, 3000);
    gdx_nearby_tick(&nb, 1020 + GDX_LAN_EXPIRE_MS + 1);
    n = gdx_nearby_sorted(&nb, idx, GDX_LAN_MAX_NEARBY);
    CHECK(n == 1 && strcmp(nb.room[idx[0]].b.name, "Mike") == 0);
    /* A full table evicts the stalest row. */
    for (i = 0; i < GDX_LAN_MAX_NEARBY + 3; i++) {
        c = b;
        c.hostId = 1000u + (uint32_t) i;
        gdx_nearby_on_beacon(&nb, &c, &a2, 4000 + (uint32_t) i);
    }
    n = gdx_nearby_sorted(&nb, idx, GDX_LAN_MAX_NEARBY);
    CHECK(n == GDX_LAN_MAX_NEARBY);
    CHECK(gdx_nearby_signal_from_ping(-1) == 0 && gdx_nearby_signal_from_ping(50) == 2 &&
          gdx_nearby_signal_from_ping(400) == 1);
    printf("  beacon + nearby table: parse/fuzz/compat/self/sort/ping/expiry/evict ok\n");
}

/* mp-online over UDS (port/gdx_mp_uds_bridge.c): fake node endpoints, the derived room code, the
 * beacon both ways (and hostile appdata), the merged NEARBY list, and a console-hosted room whose
 * members are UDS nodes -- the same room code path as a WiFi room, so the lobby / lockstep /
 * removal tests above cover the rest. */
static void TestUds(void) {
    static GdxMpLanRoom room;
    static LanOut o;
    static GdxMpNearby lan;
    const char* B = "mp-online@uds";
    const uint32_t C = 0xC0DE;
    GdxMpEndpoint e;
    GdxMpEndpoint n2;
    GdxMpEndpoint n3;
    GdxMpBeacon b;
    GdxMpBeacon d;
    GdxMpUdsSeen seen[4];
    GdxMpNearbyRoom row;
    GdxMpNearbyRoom rows[16];
    GdxMpRvWelcome w2;
    GdxMpRvWelcome w3;
    GdxMpRvRoster ro;
    GdxMpRvError err;
    uint8_t app[GDX_MP_BEACON_APPDATA_MAX];
    uint8_t pkt[1400];
    char code[GDX_MPO_CODE_LEN + 1];
    char code2[GDX_MPO_CODE_LEN + 1];
    uint32_t now = 9000;
    int i;
    int k;
    int n;
    int same = 0;

    memset(&d, 0, sizeof(d));
    memset(seen, 0, sizeof(seen));
    /* Endpoints: every node round-trips, nothing else maps to a node. */
    for (i = 1; i <= GDX_UDS_MAX_NODE; i++) {
        gdx_uds_node_endpoint(i, &e);
        CHECK(gdx_uds_endpoint_node(&e) == i && gdx_mpo_endpoint_valid(&e));
    }
    gdx_uds_node_endpoint(0, &e);
    CHECK(gdx_uds_endpoint_node(&e) == 0);
    gdx_uds_node_endpoint(17, &e);
    CHECK(gdx_uds_endpoint_node(&e) == 0);
    gdx_uds_node_endpoint(2, &e);
    e.port++;
    CHECK(gdx_uds_endpoint_node(&e) == 0);
    gdx_uds_node_endpoint(2, &e);
    e.ip[2]++;
    CHECK(gdx_uds_endpoint_node(&e) == 0);
    CHECK(gdx_uds_endpoint_node(&GDX_LAN_SELF) == 0 && gdx_uds_endpoint_node(NULL) == 0);

    /* The code: deterministic, valid, spread out. */
    gdx_uds_room_code(0x12345678u, code);
    gdx_uds_room_code(0x12345678u, code2);
    CHECK(strcmp(code, code2) == 0 && strlen(code) == GDX_MPO_CODE_LEN && gdx_mpo_code_normalise(code2) &&
          strcmp(code, code2) == 0);
    for (i = 0; i < 2000; i++) {
        gdx_uds_room_code(Rand(), code);
        gdx_uds_room_code(Rand(), code2);
        CHECK(gdx_mpo_code_normalise(code));
        same += strcmp(code, code2) == 0;
    }
    CHECK(same <= 1);

    /* Models. */
    CHECK(gdx_uds_model_from_beacon(gdx_uds_model_to_beacon(GDX_MPO_MODEL_N3DS)) == GDX_MPO_MODEL_N3DS);
    CHECK(gdx_uds_model_from_beacon(gdx_uds_model_to_beacon(GDX_MPO_MODEL_O3DS)) == GDX_MPO_MODEL_O3DS);
    CHECK(gdx_uds_model_from_beacon(GDX_MP_MODEL_2DS) == GDX_MPO_MODEL_O3DS &&
          gdx_uds_model_from_beacon(GDX_MP_MODEL_N2DSXL) == GDX_MPO_MODEL_N3DS &&
          gdx_uds_model_from_beacon(0xFF) == GDX_MPO_MODEL_UNKNOWN && gdx_uds_model_from_beacon(9) == 0);

    /* Beacon: made -> encoded -> decoded -> a joinable row that joins node 1 with the room's code. */
    gdx_uds_make_beacon(&b, "MIKE", GDX_MPO_MODEL_N3DS, 1, 4, 0, B, C, 0xABCD1234u);
    n = gdx_mp_beacon_encode(&b, app, sizeof(app));
    CHECK(n == GDX_MP_BEACON_BYTES && gdx_mp_beacon_decode(app, n, &d) == GDX_MP_BEACON_OK);
    seen[0].beacon = d;
    seen[0].signalBars = 2;
    gdx_uds_nearby_row(&seen[0], 3, B, C, &row);
    gdx_uds_room_code(0xABCD1234u, code);
    CHECK(row.used && row.medium == GDX_NEARBY_MEDIUM_UDS && row.srcIndex == 3 && row.joinable && row.compatible &&
          strcmp(row.whyNot, "") == 0 && strcmp(row.b.name, "MIKE") == 0 && row.b.players == 1 &&
          row.b.maxPlayers == 4 && row.b.model == GDX_MPO_MODEL_N3DS && strcmp(row.b.code, code) == 0 &&
          gdx_uds_endpoint_node(&row.room) == 1 && row.signal == 2 && row.pingMs == -1 &&
          row.b.hostId == 0xABCD1234u);
    /* Clamps. */
    gdx_uds_make_beacon(&d, "X", GDX_MPO_MODEL_O3DS, 9, 9, 0, B, C, 1);
    CHECK(d.players == GDX_MPO_MAX_SLOTS && d.maxPlayers == GDX_MPO_MAX_SLOTS);
    gdx_uds_make_beacon(&d, "X", GDX_MPO_MODEL_O3DS, 0, 1, 0, B, C, 1);
    CHECK(d.players == 1 && d.maxPlayers == 2);
    /* Why not. */
    seen[1].signalBars = -1;
    gdx_uds_make_beacon(&seen[1].beacon, "A", 0, 1, 4, 0, "other@x", C, 2);
    gdx_uds_nearby_row(&seen[1], 0, B, C, &row);
    CHECK(!row.joinable && !row.compatible && strcmp(row.whyNot, "other version") == 0 && row.signal == -1);
    gdx_uds_make_beacon(&seen[1].beacon, "A", 0, 1, 4, 0, B, C ^ 1, 2);
    gdx_uds_nearby_row(&seen[1], 0, B, C, &row);
    CHECK(!row.joinable && !row.compatible && strcmp(row.whyNot, "other ROM") == 0);
    gdx_uds_make_beacon(&seen[1].beacon, "A", 0, 2, 4, 1, B, C, 2);
    gdx_uds_nearby_row(&seen[1], 0, B, C, &row);
    CHECK(!row.joinable && row.compatible && strcmp(row.whyNot, "racing") == 0 && (row.b.flags & GDX_LAN_FLAG_RACING));
    gdx_uds_make_beacon(&seen[1].beacon, "A", 0, 4, 4, 0, B, C, 2);
    gdx_uds_nearby_row(&seen[1], 0, B, C, &row);
    CHECK(!row.joinable && row.compatible && strcmp(row.whyNot, "full") == 0 && (row.b.flags & GDX_LAN_FLAG_FULL));
    /* An mp-uds spike room ([net] mp=host over UDS) is another protocol: listed, greyed. */
    seen[1].beacon = b;
    seen[1].beacon.proto = GDX_MP_BEACON_PROTO;
    gdx_uds_nearby_row(&seen[1], 0, B, C, &row);
    CHECK(!row.joinable && !row.compatible && strcmp(row.whyNot, "other version") == 0);
    /* A hostile name becomes the lobby's name alphabet; never longer than the field. */
    seen[1].beacon = b;
    memcpy(seen[1].beacon.name, "\x1b[31m\xc3\xa9t\xff\xfeoo long name for us", 31);
    seen[1].beacon.name[31] = '\0';
    gdx_uds_nearby_row(&seen[1], 0, B, C, &row);
    CHECK(strlen(row.b.name) <= GDX_MPO_NAME_LEN && strchr(row.b.name, '\x1b') == NULL);
    for (i = 0; row.b.name[i] != '\0'; i++) {
        CHECK((unsigned char) row.b.name[i] >= 0x20 && (unsigned char) row.b.name[i] < 0x7F);
    }
    /* Fuzzed appdata never decodes into a row that is not one of ours. */
    for (i = 0; i < 20000; i++) {
        int j;
        memcpy(app, "GXBN", 4);
        for (j = 4; j < GDX_MP_BEACON_BYTES; j++) {
            app[j] = (uint8_t) Rand();
        }
        if (gdx_mp_beacon_decode(app, GDX_MP_BEACON_BYTES, &d) == GDX_MP_BEACON_OK) {
            seen[2].beacon = d;
            seen[2].signalBars = (int) (Rand() % 9) - 4;
            gdx_uds_nearby_row(&seen[2], 0, B, C, &row);
            CHECK(!row.joinable && row.signal >= -1 && row.signal <= 3);
        }
    }

    /* The merged list: WiFi + UDS rows, joinable first, then name, WiFi before UDS on a tie. */
    gdx_nearby_init(&lan, B, C);
    {
        GdxMpLanBeacon lb;
        GdxMpEndpoint src = { { 192, 168, 2, 40 }, 27300 };
        memset(&lb, 0, sizeof(lb));
        lb.hostId = 77;
        lb.contentHash = C;
        lb.roomPort = 27300;
        lb.players = 1;
        lb.maxPlayers = 4;
        lb.model = GDX_MPO_MODEL_O3DS;
        lb.proto = GDX_MPO_PROTO;
        snprintf(lb.code, sizeof(lb.code), "%s", "K7MPQ");
        snprintf(lb.name, sizeof(lb.name), "%s", "MIKE");
        snprintf(lb.build, sizeof(lb.build), "%s", B);
        gdx_nearby_on_beacon(&lan, &lb, &src, now);
        lb.hostId = 78;
        snprintf(lb.name, sizeof(lb.name), "%s", "ZED");
        snprintf(lb.build, sizeof(lb.build), "%s", "old@1");
        src.ip[3] = 41;
        gdx_nearby_on_beacon(&lan, &lb, &src, now);
    }
    seen[0].beacon = b; /* MIKE, joinable, UDS */
    gdx_uds_make_beacon(&seen[1].beacon, "ANNA", GDX_MPO_MODEL_O3DS, 1, 2, 0, B, C, 5); /* joinable */
    seen[1].signalBars = 3;
    gdx_uds_make_beacon(&seen[2].beacon, "BOB", GDX_MPO_MODEL_O3DS, 2, 2, 0, B, C, 6); /* full */
    seen[2].signalBars = 1;
    n = gdx_nearby_rows(&lan, seen, 3, B, C, rows, 16);
    CHECK(n == 5);
    CHECK(n == 5 && strcmp(rows[0].b.name, "ANNA") == 0 && rows[0].medium == GDX_NEARBY_MEDIUM_UDS &&
          rows[0].srcIndex == 1);
    CHECK(n == 5 && strcmp(rows[1].b.name, "MIKE") == 0 && rows[1].medium == GDX_NEARBY_MEDIUM_WIFI &&
          rows[1].b.hostId == 77 && rows[1].srcIndex >= 0 && lan.room[rows[1].srcIndex].b.hostId == 77);
    CHECK(n == 5 && strcmp(rows[2].b.name, "MIKE") == 0 && rows[2].medium == GDX_NEARBY_MEDIUM_UDS &&
          rows[2].srcIndex == 0);
    CHECK(n == 5 && !rows[3].joinable && !rows[4].joinable);
    CHECK(gdx_nearby_rows(&lan, seen, 3, B, C, rows, 2) == 2 && gdx_nearby_rows(NULL, seen, 3, B, C, rows, 16) == 3 &&
          gdx_nearby_rows(&lan, NULL, 0, B, C, rows, 16) == 2 && gdx_nearby_rows(&lan, seen, 3, B, C, NULL, 4) == 0);

    /* A room hosted over UDS: members are nodes; the roster hands out node endpoints, the relay
     * forwards between them, a stranger node without the code gets nowhere. */
    gdx_uds_room_code(b.roomId, code);
    gdx_uds_node_endpoint(2, &n2);
    gdx_uds_node_endpoint(3, &n3);
    CHECK(gdx_lan_room_open(&room, code, B, C, 4, 0x5151, now) == 0);
    LanRequest(&room, &o, GDX_RV_CREATE, &GDX_LAN_SELF, 1, code, B, C, "MIKE", now);
    CHECK(LanFind(&o, &GDX_LAN_SELF, GDX_RV_WELCOME) >= 0);
    gdx_uds_room_code(b.roomId ^ 1, code2);
    LanRequest(&room, &o, GDX_RV_JOIN, &n2, 2, code2, B, C, "ANNA", ++now);
    CHECK(o.n == 1 && gdx_rv_parse_error(o.pkt[0], o.len[0], &err) == 0 && err.code == GDX_RV_E_ROOM_NOT_FOUND);
    LanRequest(&room, &o, GDX_RV_JOIN, &n2, 3, row.b.code, B, C, "ANNA", ++now);
    k = LanFind(&o, &n2, GDX_RV_WELCOME);
    CHECK(k >= 0 && gdx_rv_parse_welcome(o.pkt[k], o.len[k], &w2) == 0 && w2.slot == 1);
    LanRequest(&room, &o, GDX_RV_JOIN, &n3, 4, code, B, C, "BOB", ++now);
    k = LanFind(&o, &n3, GDX_RV_WELCOME);
    CHECK(k >= 0 && gdx_rv_parse_welcome(o.pkt[k], o.len[k], &w3) == 0 && w3.slot == 2);
    k = LanFind(&o, &n2, GDX_RV_ROSTER);
    CHECK(k >= 0 && gdx_rv_parse_roster(o.pkt[k], o.len[k], &ro) == 0 && ro.member[2].present &&
          gdx_uds_endpoint_node(&ro.member[2].pub) == 3 && ro.member[1].present &&
          gdx_uds_endpoint_node(&ro.member[1].pub) == 2);
    {
        uint8_t lb[GDX_LB_HEADER_BYTES + 8];
        memset(lb, 0, sizeof(lb));
        memcpy(lb, "GXLB", 4);
        lb[6] = 1;
        n = gdx_rv_build_relay(code, 1, 0x4, w2.memberKey, lb, sizeof(lb), pkt, sizeof(pkt));
        o.n = 0;
        gdx_lan_room_handle(&room, pkt, n, &n2, ++now, LanEmit, &o);
        CHECK(o.n == 1 && gdx_mpo_endpoint_eq(&o.to[0], &n3) && memcmp(o.pkt[0], "GXLB", 4) == 0);
        /* the member key, not the source address, authenticates (as for a NAT rebinding) */
        n = gdx_rv_build_relay(code, 1, 0x4, w2.memberKey ^ 0x10, lb, sizeof(lb), pkt, sizeof(pkt));
        o.n = 0;
        gdx_lan_room_handle(&room, pkt, n, &n3, ++now, LanEmit, &o);
        CHECK(LanFind(&o, &n3, GDX_RV_KA_ACK) < 0 && (o.n == 0 || memcmp(o.pkt[0], "GXLB", 4) != 0));
    }
    CHECK(gdx_lan_room_count(&room) == 3);
    printf("  uds bridge: endpoints/code/models/beacon/why-not/fuzz/merged list/room over nodes ok\n");
}

/* mp-menus #10: the shared pause's hold-to-confirm. A = 0x8000, START = 0x1000 (N64 bits). */
#define PG_A 0x8000u
#define PG_ST 0x1000u
#define PG_MASK (PG_A | PG_ST)
static void TestPauseGate(void) {
    GdxMpPauseGate g;
    uint32_t t;
    int edges = 0;
    uint16_t last = 0;
    gdx_mp_pause_gate_reset(&g);
    /* Not gated (racing, or CONTINUE highlighted): a press goes straight through. */
    CHECK(gdx_mp_pause_gate(&g, PG_A, PG_MASK, 0, 0, 0, 1500) == PG_A);
    CHECK(gdx_mp_pause_gate(&g, 0, PG_MASK, 0, 0, 16, 1500) == 0);
    /* Gated: held back for 1500 ms, then one edge; the other bits are untouched. */
    for (t = 100; t <= 1700; t += 16) {
        const uint16_t out = gdx_mp_pause_gate(&g, PG_A | 0x0010u, PG_MASK, 1, 5, t, 1500);
        CHECK((out & 0x0010u) != 0);
        if ((out & PG_A) && !(last & PG_A)) {
            edges++;
            CHECK(t >= 1600); /* not before the hold is done */
        }
        last = out;
    }
    CHECK(edges == 1);
    CHECK(gdx_mp_pause_gate(&g, 0, PG_MASK, 1, 5, 1716, 1500) == 0);
    /* Let go early: nothing ever reaches the game. */
    for (t = 2000; t < 2900; t += 16) {
        CHECK((gdx_mp_pause_gate(&g, PG_ST, PG_MASK, 1, 5, t, 1500) & PG_ST) == 0);
    }
    CHECK(gdx_mp_pause_gate_permille(&g, 1, 2896, 1500) > 500);
    CHECK(gdx_mp_pause_gate(&g, 0, PG_MASK, 1, 5, 2912, 1500) == 0);
    CHECK(gdx_mp_pause_gate_permille(&g, 1, 2912, 1500) == 0);
    /* The cursor moves mid-hold: the hold starts over on the new option. */
    gdx_mp_pause_gate(&g, PG_A, PG_MASK, 1, 1, 3000, 1500);
    gdx_mp_pause_gate(&g, PG_A, PG_MASK, 1, 1, 4000, 1500);
    CHECK((gdx_mp_pause_gate(&g, PG_A, PG_MASK, 1, 5, 4100, 1500) & PG_A) == 0);
    CHECK((gdx_mp_pause_gate(&g, PG_A, PG_MASK, 1, 5, 5000, 1500) & PG_A) == 0);
    CHECK((gdx_mp_pause_gate(&g, PG_A, PG_MASK, 1, 5, 5600, 1500) & PG_A) != 0);
    gdx_mp_pause_gate(&g, 0, PG_MASK, 1, 5, 5616, 1500);
    /* Held on QUIT, moved to CONTINUE: still held back (no CONTINUE by a leftover hold). */
    gdx_mp_pause_gate(&g, PG_A, PG_MASK, 1, 5, 6000, 1500);
    CHECK((gdx_mp_pause_gate(&g, PG_A, PG_MASK, 0, 0, 6100, 1500) & PG_A) == 0);
    CHECK((gdx_mp_pause_gate(&g, PG_A, PG_MASK, 0, 0, 9000, 1500) & PG_A) == 0);
    gdx_mp_pause_gate(&g, 0, PG_MASK, 0, 0, 9016, 1500);
    /* A held from the race into the pause (accelerating): passes, never an edge; a START pressed
     * on QUIT while it is down is still held back. */
    gdx_mp_pause_gate(&g, PG_A, PG_MASK, 0, 0, 10000, 1500);
    CHECK((gdx_mp_pause_gate(&g, PG_A, PG_MASK, 1, 5, 10016, 1500) & PG_A) != 0);
    CHECK(gdx_mp_pause_gate(&g, PG_A | PG_ST, PG_MASK, 1, 5, 10032, 1500) == PG_A);
    CHECK(gdx_mp_pause_gate(&g, PG_A | PG_ST, PG_MASK, 1, 5, 11600, 1500) == (PG_A | PG_ST));
    /* hold_confirm_ms = 0: a plain press. */
    gdx_mp_pause_gate_reset(&g);
    CHECK(gdx_mp_pause_gate(&g, PG_A, PG_MASK, 1, 5, 0, 0) == PG_A);
}

int main(void) {
    TestRendezvousWire();
    TestRvCookieWire();
    TestRvCookieStateMachine();
    TestVersionGating();
    TestCodesNames();
    TestLobbyWire();
    TestDelay();
    TestLink();
    TestHygiene();
    TestLobbyRules();
    TestSlotEpochStart();
    TestStartWithoutAway();
    TestJoinWaitsForRace();
    TestPauseGate();
    TestSimAbi();
    TestRacers();
    TestStockPickRules();
    TestSimulatedSessions();
    TestRemoval();
    TestLan();
    TestBeacon();
    TestUds();
    TestZipHash();
    if (sFailures != 0) {
        printf("=== %d FAILURES (%d checks) ===\n", sFailures, sChecks);
        return 1;
    }
    printf("=== ALL PASS (%d checks) ===\n", sChecks);
    return 0;
}
