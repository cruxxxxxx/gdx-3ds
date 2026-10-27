/* port/3ds/net/gdx3ds_mp_p2p.c — mp-online: the internet transport (contract: gdx3ds_mp_p2p.h,
 * gdx3ds_mp_transport.h). The protocol halves it uses are pure and host-tested:
 * port/gdx_mp_online.c (GXRV, link state, buckets, blocklist). */
#include "gdx3ds_mp_p2p.h"
#include "gdx_mp_lockstep.h"
#include "gdx_mp_lan.h"
#include "gdx_mp_uds_bridge.h"
#include "gdx3ds_mp_uds.h"
#include "gdx3ds_config.h"
#include "gdx3ds_mp_ipcstat.h"
#include "gdx3ds_mp_socthr.h"
#include "gdx_mp_socsvc.h"

#include <3ds.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define P2P_KEEPALIVE_MS 2000
#define P2P_REQUEST_RETRY_MS 1000
#define P2P_REQUEST_GIVE_UP_MS 15000
/* ROOM_FULL / CODE_TAKEN may be our own ghost: this console restarted and its old member still
 * holds the slot until the server sees it silent for 5 s (gdx_rendezvous.py REJOIN_SILENT_S), when
 * the same request from the same address takes it back. Keep asking that long before failing. */
#define P2P_GHOST_RETRY_MS 8000
#define P2P_BIND_TRIES 8
#define P2P_RX_GUARD 96

static int sFd = -1;
static GdxP2pConfig sCfg;
static char sBuild[GDX_MPO_BUILD_LEN + 1];
static char sName[GDX_MPO_NAME_LEN + 1];
static GdxP2pStatus sSt;
static GdxMpLink sLink[GDX_MPO_MAX_SLOTS];
static GdxMpBucket sPeerBucket[GDX_MPO_MAX_SLOTS];
static GdxMpBucket sStrangerBucket;
static GdxMpBlocklist sBlock;
static uint64_t sMemberKey = 0;
static uint32_t sNonce = 0;
static GdxRvCookie sCk; /* GXRV v2 ([net] mp_rv_cookie): the request's version and cookie */
static int sReqType = 0;
static int sReqRelayOnly = 0;  /* this request asks for a relay-only room (gdx3ds_p2p_set_next_relay_only) */
static int sNextRelayOnly = 0;
static int sReqHeldError = 0;  /* a ROOM_FULL / CODE_TAKEN being retried (P2P_GHOST_RETRY_MS) */
static char sReqHeldDetail[GDX_MPO_BUILD_LEN + 1];
static char sReqCode[GDX_MPO_CODE_LEN + 1];
static uint32_t sReqFirstMs = 0;
static uint32_t sReqLastMs = 0;
static uint32_t sLastKaMs = 0;
static int sBoundPort = 0;
static uint32_t sNowMs = 0;
/* LAN mode: the room we host, the configured online server while a LAN mode borrows sCfg.server,
 * and the in-process loopback from our room to our own client. */
static int sMode = GDX_P2P_MODE_ONLINE;
static GdxMpEndpoint sOnlineServer;
static GdxMpLanRoom sLanRoom;
#define P2P_LOOP_SLOTS 8
static uint8_t sLoopPkt[P2P_LOOP_SLOTS][GDX_RV_AUTH_BYTES + GDX_RV_MAX_RELAY_PAYLOAD];
static int sLoopLen[P2P_LOOP_SLOTS];
static int sLoopHead = 0;
static int sLoopCount = 0;
/* Local wireless: endpoints in GDX_UDS_NET are UDS nodes (port/gdx_mp_uds_bridge.h). */
static int sUds = 0;
static uint8_t sUdsRx[GDX_RV_AUTH_BYTES + GDX_RV_MAX_RELAY_PAYLOAD + 64];
/* NETTICK ([net] mp_uds_soc_poll_ms, default 250; 0 = every call as in 90049af): in a local
 * wireless room the radio is NWM's, so the WiFi socket cannot receive anything, yet every Drain
 * asked it (one SOC IPC per Service; on the Old 3DS SOC shares core 1 with the audio threads).
 * Poll it at this period instead -- a stray datagram still gets read and refused. */
static int sUdsSocPollMs = -1;
static uint32_t sUdsSocLastMs = 0;
static int SendTo(const GdxMpEndpoint* e, const void* buf, int len);
static int SendToStream(const GdxMpEndpoint* e, const void* buf, int len, uint32_t stream);

static uint32_t NowMs(void) {
    return (uint32_t) osGetTime();
}

static void ToSockaddr(const GdxMpEndpoint* e, struct sockaddr_in* a) {
    memset(a, 0, sizeof(*a));
    a->sin_family = AF_INET;
    a->sin_port = htons(e->port);
    memcpy(&a->sin_addr.s_addr, e->ip, 4);
}

static void FromSockaddr(const struct sockaddr_in* a, GdxMpEndpoint* e) {
    memcpy(e->ip, &a->sin_addr.s_addr, 4);
    e->port = ntohs(a->sin_port);
}

static int OpenSocket(int port) {
    struct sockaddr_in a;
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return -100 - errno;
    }
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((u16) port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr*) &a, sizeof(a)) != 0) {
        const int e = errno;
        close(fd);
        return -200 - e;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    return fd;
}

static void LanEmit(void* ctx, const GdxMpEndpoint* to, const uint8_t* pkt, int len) {
    (void) ctx;
    if (gdx_mpo_endpoint_eq(to, &GDX_LAN_SELF)) {
        if (sLoopCount < P2P_LOOP_SLOTS && len > 0 && len <= (int) sizeof(sLoopPkt[0])) {
            const int i = (sLoopHead + sLoopCount) % P2P_LOOP_SLOTS;
            memcpy(sLoopPkt[i], pkt, (size_t) len);
            sLoopLen[i] = len;
            sLoopCount++;
        }
        return;
    }
    SendTo(to, pkt, len);
}

/* One sendto on this thread ([net] mp_soc_thread = 0, or the thread could not start). */
static int SendInline(const GdxMpEndpoint* e, const void* buf, int len) {
    struct sockaddr_in a;
    ToSockaddr(e, &a);
    {
        const uint64_t t0 = gdx3ds_ipcstat_begin();
        const ssize_t sent = sendto(sFd, buf, (size_t) len, 0, (const struct sockaddr*) &a, sizeof(a));
        gdx3ds_ipcstat_end(GDX_IPC_SOC_SEND, t0);
        if (sent < 0) {
            return (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS) ? 0 : -errno;
        }
    }
    return len;
}

static int SendTo(const GdxMpEndpoint* e, const void* buf, int len) {
    return SendToStream(e, buf, len, 0);
}

/* `stream` (gdx_mp_socsvc_stream): a lockstep INPUT the socket thread may coalesce; 0 = not one. */
static int SendToStream(const GdxMpEndpoint* e, const void* buf, int len, uint32_t stream) {
    if (sMode == GDX_P2P_MODE_LAN_HOST && gdx_mpo_endpoint_eq(e, &GDX_LAN_SELF)) {
        /* our own client -> our own room, in-process */
        gdx_lan_room_handle(&sLanRoom, (const uint8_t*) buf, len, &GDX_LAN_SELF, NowMs(), LanEmit, NULL);
        return len;
    }
    {
        const int node = gdx_uds_endpoint_node(e);
        if (node != 0) {
            return sUds ? gdx3ds_mp_uds_raw_send(node, buf, len) : 0;
        }
    }
    if (sFd < 0 || !gdx_mpo_endpoint_valid(e)) {
        return 0;
    }
    if (gdx3ds_socthr_active()) {
        return gdx3ds_socthr_send(e, buf, len, stream);
    }
    return SendInline(e, buf, len);
}

#define P2P_LOG(...)              \
    do {                          \
        if (sCfg.log != NULL) {   \
            sCfg.log(__VA_ARGS__); \
        }                         \
    } while (0)

static void Fail(int code, const char* detail) {
    if (code != GDX_P2P_E_UPDATE) {
        memset(&sSt.update, 0, sizeof(sSt.update));
    }
    if (code != GDX_RV_E_BUILD_MISMATCH && code != GDX_RV_E_CONTENT_MISMATCH) {
        sSt.errorHostProto = 0;
    }
    sSt.phase = GDX_P2P_FAILED;
    sSt.errorCode = code;
    snprintf(sSt.errorDetail, sizeof(sSt.errorDetail), "%s", detail != NULL ? detail : "");
}

/* The room is gone; the links stay up for a race that can finish without it (GdxP2pStatus). */
static void LoseRoom(int code, const char* detail) {
    if (sSt.roomLost != 0) {
        return;
    }
    sSt.roomLost = code;
    snprintf(sSt.roomLostDetail, sizeof(sSt.roomLostDetail), "%s", detail != NULL ? detail : "");
}

static void ForgetRoom(void) {
    int s;
    for (s = 0; s < GDX_MPO_MAX_SLOTS; s++) {
        gdx_link_reset(&sLink[s]);
        gdx_mpo_bucket_init(&sPeerBucket[s], 900, 300);
    }
    sMemberKey = 0;
    sSt.mySlot = -1;
    sSt.haveRoster = 0;
    sSt.roomSecret = 0;
    sSt.roomLost = 0;
    sSt.roomLostDetail[0] = '\0';
    memset(&sSt.roster, 0, sizeof(sSt.roster));
    sSt.code[0] = '\0';
    sReqType = 0;
}

GdxMpEndpoint gdx3ds_p2p_local_endpoint(void) {
    GdxMpEndpoint e;
    const long id = gethostid(); /* the console's IPv4, network order in memory */
    const uint32_t v = (uint32_t) id;
    memcpy(e.ip, &v, 4);
    e.port = (uint16_t) sBoundPort;
    return e;
}

int gdx3ds_p2p_open(const GdxP2pConfig* cfg) {
    int i;
    int fd = -1;
    memset(&sSt, 0, sizeof(sSt));
    sCfg = *cfg;
    sOnlineServer = cfg->server;
    sMode = GDX_P2P_MODE_ONLINE;
    snprintf(sBuild, sizeof(sBuild), "%s", cfg->build != NULL ? cfg->build : "unknown");
    gdx_mpo_name_sanitise(cfg->name != NULL ? cfg->name : "", sName);
    sCfg.build = sBuild;
    sCfg.name = sName;
    gdx_mpo_bucket_init(&sStrangerBucket, 200, 100);
    gdx_mpo_block_init(&sBlock, 50);
    ForgetRoom();
    sSt.phase = GDX_P2P_IDLE;
    sSt.forceRelay = cfg->forceRelay;
    for (i = 0; i < P2P_BIND_TRIES; i++) {
        const int port = cfg->bindPort > 0 ? cfg->bindPort + i : 0;
        fd = OpenSocket(port);
        if (fd >= 0 || cfg->bindPort <= 0) {
            break;
        }
    }
    if (fd < 0) {
        return fd;
    }
    sFd = fd;
    gdx3ds_socthr_start(fd);
    gdx3ds_socthr_set_rare_poll(sUds);
    {
        struct sockaddr_in name;
        socklen_t nlen = sizeof(name);
        memset(&name, 0, sizeof(name));
        sBoundPort = (getsockname(fd, (struct sockaddr*) &name, &nlen) == 0) ? ntohs(name.sin_port) : 0;
        if (sBoundPort == 0 && cfg->bindPort > 0) {
            sBoundPort = cfg->bindPort + i;
        }
    }
    sNonce = (uint32_t) svcGetSystemTick() ^ 0x9E3779B9u;
    gdx_rv_cookie_init(&sCk, cfg->rvCookie);
    sSt.clientProto = cfg->clientProto > 0 ? cfg->clientProto : GDX_MPO_PROTO;
    return 0;
}

void gdx3ds_p2p_close(void) {
    gdx3ds_socthr_stop();
    if (sFd >= 0) {
        close(sFd);
        sFd = -1;
    }
    sSt.phase = GDX_P2P_IDLE;
}

static void SendRequest(uint32_t nowMs) {
    GdxMpRvRequest r;
    uint8_t pkt[GDX_RV_REQ2_BYTES]; /* v2 is the larger */
    int n;
    memset(&r, 0, sizeof(r));
    r.nonce = sNonce;
    r.contentHash = sCfg.contentHash;
    r.priv = gdx3ds_p2p_local_endpoint();
    r.maxPlayers = (uint8_t) sSt.maxPlayers;
    r.forceRelay = (uint8_t) ((sCfg.forceRelay || sReqRelayOnly) ? 1 : 0);
    snprintf(r.build, sizeof(r.build), "%s", sBuild);
    snprintf(r.name, sizeof(r.name), "%s", sName);
    snprintf(r.code, sizeof(r.code), "%s", sReqCode);
    r.clientProto = (uint16_t) sSt.clientProto;
    {
        int event = GDX_RV_CK_NONE;
        n = gdx_rv_cookie_build(&sCk, sReqType, &r, nowMs, pkt, sizeof(pkt), &event);
        if (event == GDX_RV_CK_FALLBACK) {
            P2P_LOG("[mpo] rv cookie: auto: %d v2 requests unanswered (%lu ms) -> fallback to v1 for this session "
                    "(a server before 2.0.0?)",
                    GDX_RV_COOKIE_FALLBACK_TRIES, (unsigned long) (nowMs - sCk.firstSendMs));
        }
    }
    sSt.rvVersion = sCk.useV2 ? GDX_RV_VERSION_COOKIE : GDX_RV_VERSION;
    if (n > 0) {
        SendTo(&sCfg.server, pkt, n); /* through the socket thread like every other datagram */
    }
    sReqLastMs = nowMs;
}

/* The WELCOME / ERROR that ended a request: what the cookie exchange did. */
static void LogRequestAnswered(const char* what, uint32_t nowMs) {
    if (sMode != GDX_P2P_MODE_ONLINE) {
        return;
    }
    if (sCk.useV2 && sCk.cookies > 0) {
        P2P_LOG("[mpo] rv cookie: v2 request -> COOKIE -> resend %s (rtt=%lu ms, cookies=%lu, total=%lu ms)",
                what, (unsigned long) sCk.cookieRttMs, (unsigned long) sCk.cookies,
                (unsigned long) (nowMs - sCk.firstSendMs));
    } else if (sCk.useV2) {
        P2P_LOG("[mpo] rv cookie: v2 request -> %s without a COOKIE (total=%lu ms)", what,
                (unsigned long) (nowMs - sCk.firstSendMs));
    } else {
        P2P_LOG("[mpo] rv cookie: v1 request -> %s (mp_rv_cookie=%s%s, total=%lu ms)", what,
                gdx_rv_cookie_mode_name(sCk.mode), sCk.fellBack ? ", fell back" : "",
                (unsigned long) (nowMs - sCk.firstSendMs));
    }
}

static void StartRequest(int type, int maxPlayers, const char* code) {
    const uint32_t now = NowMs();
    ForgetRoom();
    if (type != GDX_RV_CREATE || sMode != GDX_P2P_MODE_ONLINE) {
        sReqRelayOnly = 0; /* only an online CREATE carries it (gdx3ds_p2p_host sets it first) */
    }
    sNonce = sNonce * 1664525u + 1013904223u;
    sReqType = type;
    snprintf(sReqCode, sizeof(sReqCode), "%s", code != NULL ? code : "");
    sSt.maxPlayers = maxPlayers;
    sSt.phase = GDX_P2P_REQUESTING;
    sSt.mode = sMode;
    sSt.errorCode = 0;
    sSt.errorDetail[0] = '\0';
    sReqHeldError = 0;
    sReqFirstMs = now;
    gdx_rv_cookie_begin(&sCk, sNonce, now, sMode == GDX_P2P_MODE_ONLINE);
    SendRequest(now);
}

/* Back to the configured server (closing a LAN room we host: its joiners hear HOST_LEFT). */
static void EndLanMode(void) {
    if (sMode == GDX_P2P_MODE_LAN_HOST) {
        gdx_lan_room_close(&sLanRoom, LanEmit, NULL);
    }
    sMode = GDX_P2P_MODE_ONLINE;
    sCfg.server = sOnlineServer;
    sLoopCount = 0;
}

void gdx3ds_p2p_lan_host(int maxPlayers, const char* code, uint64_t entropy) {
    EndLanMode();
    if (maxPlayers < 2) {
        maxPlayers = 2;
    } else if (maxPlayers > GDX_MPO_MAX_SLOTS) {
        maxPlayers = GDX_MPO_MAX_SLOTS;
    }
    if (gdx_lan_room_open(&sLanRoom, code, sBuild, sCfg.contentHash, maxPlayers, entropy, NowMs()) != 0) {
        ForgetRoom();
        Fail(GDX_RV_E_BAD_REQUEST, "lan room");
        return;
    }
    if (sCfg.lanTimeoutMs != 0) {
        gdx_lan_room_set_timeout(&sLanRoom, sCfg.lanTimeoutMs);
    }
    sMode = GDX_P2P_MODE_LAN_HOST;
    sCfg.server = GDX_LAN_SELF;
    StartRequest(GDX_RV_CREATE, maxPlayers, sLanRoom.code);
}

void gdx3ds_p2p_lan_join(const GdxMpEndpoint* host, const char* code) {
    char c[GDX_MPO_CODE_LEN + 1];
    EndLanMode();
    snprintf(c, sizeof(c), "%s", code != NULL ? code : "");
    if (host == NULL || !gdx_mpo_endpoint_valid(host) || !gdx_mpo_code_normalise(c)) {
        ForgetRoom();
        Fail(GDX_RV_E_ROOM_NOT_FOUND, "bad room");
        return;
    }
    sMode = GDX_P2P_MODE_LAN_JOIN;
    sCfg.server = *host;
    StartRequest(GDX_RV_JOIN, GDX_MPO_MAX_SLOTS, c);
}

int gdx3ds_p2p_lan_room_count(void) {
    return sMode == GDX_P2P_MODE_LAN_HOST && sLanRoom.open ? gdx_lan_room_count(&sLanRoom) : 0;
}

void gdx3ds_p2p_set_next_relay_only(int on) {
    sNextRelayOnly = on ? 1 : 0;
}

void gdx3ds_p2p_host(int maxPlayers, const char* wantCode) {
    const int relayOnly = sNextRelayOnly;
    sNextRelayOnly = 0;
    EndLanMode();
    if (maxPlayers < 2) {
        maxPlayers = 2;
    } else if (maxPlayers > GDX_MPO_MAX_SLOTS) {
        maxPlayers = GDX_MPO_MAX_SLOTS;
    }
    sReqRelayOnly = relayOnly;
    StartRequest(GDX_RV_CREATE, maxPlayers, wantCode);
}

void gdx3ds_p2p_join(const char* code) {
    char c[GDX_MPO_CODE_LEN + 1];
    snprintf(c, sizeof(c), "%s", code != NULL ? code : "");
    if (!gdx_mpo_code_normalise(c)) {
        ForgetRoom();
        Fail(GDX_RV_E_ROOM_NOT_FOUND, "bad code");
        return;
    }
    EndLanMode();
    StartRequest(GDX_RV_JOIN, GDX_MPO_MAX_SLOTS, c);
}

void gdx3ds_p2p_leave(void) {
    if (sSt.phase == GDX_P2P_IN_ROOM && sMemberKey != 0) {
        uint8_t pkt[GDX_RV_AUTH_BYTES];
        const int n = gdx_rv_build_auth(GDX_RV_LEAVE, sSt.code, sSt.mySlot, 0, sMemberKey, pkt, sizeof(pkt));
        int i;
        for (i = 0; i < 2 && n > 0; i++) {
            SendTo(&sCfg.server, pkt, n);
        }
    }
    ForgetRoom();
    EndLanMode();
    sSt.phase = GDX_P2P_IDLE;
}

void gdx3ds_p2p_lock_room(int locked) {
    uint8_t pkt[GDX_RV_AUTH_BYTES];
    int n;
    if (sSt.phase != GDX_P2P_IN_ROOM || sSt.mySlot != 0) {
        return;
    }
    n = gdx_rv_build_auth(GDX_RV_LOCK, sSt.code, 0, locked ? 1 : 0, sMemberKey, pkt, sizeof(pkt));
    if (n > 0) {
        SendTo(&sCfg.server, pkt, n);
    }
}

static void SendKeepalive(uint32_t nowMs) {
    uint8_t pkt[GDX_RV_KEEPALIVE_BYTES];
    const int n = gdx_rv_build_keepalive(sSt.code, sSt.mySlot, sMemberKey, sSt.haveRoster ? sSt.roster.version : 0,
                                         nowMs, pkt, sizeof(pkt));
    if (n > 0) {
        SendTo(&sCfg.server, pkt, n);
    }
    sLastKaMs = nowMs;
}

static void ApplyRoster(const GdxMpRvRoster* ro, uint32_t nowMs) {
    int s;
    const int forced = sCfg.forceRelay || ro->forceRelay;
    for (s = 0; s < GDX_MPO_MAX_SLOTS; s++) {
        if (s == sSt.mySlot) {
            continue;
        }
        if (!ro->member[s].present) {
            gdx_link_reset(&sLink[s]);
            continue;
        }
        if (s == 0 && sMode == GDX_P2P_MODE_LAN_JOIN) {
            /* the host's room sees itself as GDX_LAN_SELF: where we reach it is where we joined */
            gdx_link_set_candidates(&sLink[s], &sCfg.server, &ro->member[s].priv, forced, nowMs);
            continue;
        }
        gdx_link_set_candidates(&sLink[s], &ro->member[s].pub, &ro->member[s].priv, forced, nowMs);
    }
    sSt.roster = *ro;
    sSt.haveRoster = 1;
    sSt.forceRelay = forced;
    sSt.rosterChanges++;
}

/* UPDATE: this client's proto is outside the server's range. Final for this request. */
static void HandleUpdate(const uint8_t* pkt, int n, uint32_t nowMs) {
    GdxMpRvUpdate u;
    if (sSt.phase != GDX_P2P_REQUESTING || gdx_rv_parse_update(pkt, n, &u) != 0 || u.nonce != sNonce) {
        return;
    }
    LogRequestAnswered("UPDATE", nowMs);
    if (gdx_rv_update_verdict(&u, sSt.clientProto) == GDX_RV_UPD_SERVER_OLD) {
        P2P_LOG("[mpo] server supports client proto<=%u (have %d): the server needs an update",
                (unsigned) u.maxProto, sSt.clientProto);
    } else {
        P2P_LOG("[mpo] server requires client proto>=%u (have %d)%s%s", (unsigned) u.minProto, sSt.clientProto,
                u.text[0] != '\0' ? " update: " : "", u.text);
    }
    sSt.update = u;
    Fail(GDX_P2P_E_UPDATE, u.text);
}

static int IsMagicGxrv(const uint8_t* p, int n) {
    return n >= 4 && memcmp(p, "GXRV", 4) == 0;
}

static void HandleServer(const uint8_t* pkt, int n, uint32_t nowMs) {
    const int t = gdx_rv_peek_type(pkt, n);
    sSt.rxServer++;
    sSt.lastServerRxMs = nowMs;
    if (sSt.phase == GDX_P2P_REQUESTING && gdx_rv_cookie_on_reply(&sCk, pkt, n, nowMs) == GDX_RV_CK_RESEND) {
        SendRequest(nowMs); /* the same request, same nonce, now with the cookie */
        return;
    }
    if (t == GDX_RV_WELCOME) {
        GdxMpRvWelcome w;
        if (sSt.phase != GDX_P2P_REQUESTING || gdx_rv_parse_welcome(pkt, n, &w) != 0 || w.nonce != sNonce) {
            return;
        }
        LogRequestAnswered("ok", nowMs);
        sSt.phase = GDX_P2P_IN_ROOM;
        snprintf(sSt.code, sizeof(sSt.code), "%s", w.code);
        sSt.mySlot = w.slot;
        sSt.maxPlayers = w.maxPlayers;
        sSt.roomSecret = w.roomSecret;
        sSt.seenAs = w.seenAs;
        sSt.forceRelay = sCfg.forceRelay || w.forceRelay;
        sMemberKey = w.memberKey;
        SendKeepalive(nowMs); /* the roster comes back with the first keepalive's answer */
        return;
    }
    if (t == GDX_RV_UPDATE) {
        HandleUpdate(pkt, n, nowMs);
        return;
    }
    if (t == GDX_RV_ERROR) {
        GdxMpRvError e;
        if (gdx_rv_parse_error(pkt, n, &e) != 0) {
            return;
        }
        if (sSt.phase == GDX_P2P_REQUESTING && e.nonce == sNonce) {
            if (sMode == GDX_P2P_MODE_ONLINE && gdx_rv_cookie_on_error(&sCk, &e) == GDX_RV_CK_RETRY_V2) {
                P2P_LOG("[mpo] rv cookie: auto: v1 refused (server needs GXRV v2) -> back to v2");
                SendRequest(nowMs);
                return;
            }
            if ((e.code == GDX_RV_E_ROOM_FULL || e.code == GDX_RV_E_CODE_TAKEN) &&
                (int32_t) (nowMs - sReqFirstMs) < P2P_GHOST_RETRY_MS) {
                sReqHeldError = e.code;
                snprintf(sReqHeldDetail, sizeof(sReqHeldDetail), "%s", e.detail);
                return; /* the next retry may find our ghost silent long enough */
            }
            {
                char what[32];
                snprintf(what, sizeof(what), "ERROR %d", e.code);
                LogRequestAnswered(what, nowMs);
            }
            sSt.errorHostProto = e.hostProto;
            Fail(e.code, e.detail);
        } else if (sSt.phase == GDX_P2P_IN_ROOM && (e.code == GDX_RV_E_HOST_LEFT || e.code == GDX_RV_E_AUTH)) {
            LoseRoom(e.code, e.detail);
        }
        return;
    }
    if (sSt.phase != GDX_P2P_IN_ROOM) {
        return;
    }
    if (t == GDX_RV_ROSTER) {
        GdxMpRvRoster ro;
        if (gdx_rv_parse_roster(pkt, n, &ro) != 0 || strcmp(ro.code, sSt.code) != 0) {
            return;
        }
        if (sSt.mySlot < 0 || sSt.mySlot >= GDX_MPO_MAX_SLOTS || !ro.member[sSt.mySlot].present) {
            return;
        }
        if (sSt.haveRoster && ro.version == sSt.roster.version) {
            return;
        }
        ApplyRoster(&ro, nowMs);
    } else if (t == GDX_RV_KA_ACK) {
        uint32_t ver = 0;
        uint32_t echo = 0;
        if (gdx_rv_parse_ka_ack(pkt, n, &ver, &echo) == 0) {
            sSt.serverRttMs = nowMs - echo;
        }
    } else if (t == GDX_RV_PUNCH_NOW) {
        int from = -1;
        if (gdx_rv_parse_punch_now(pkt, n, &from) == 0 && from >= 0 && from < GDX_MPO_MAX_SLOTS) {
            sLink[from].lastProbeMs = nowMs - GDX_LINK_PROBE_MS; /* probe on the next HELLO */
        }
    }
}

/* Our own room's replies to our own client (LAN host): GXRV is handled here; a GXLB/GXMP a
 * joiner relayed to us through our own room stays queued for the recv op (it returns it as a
 * relayed datagram, as the online server's relay would arrive). */
static void DrainLoopback(uint32_t nowMs) {
    int guard;
    for (guard = 0; guard < P2P_LOOP_SLOTS && sLoopCount > 0 && IsMagicGxrv(sLoopPkt[sLoopHead], sLoopLen[sLoopHead]);
         guard++) {
        const int i = sLoopHead;
        sLoopHead = (sLoopHead + 1) % P2P_LOOP_SLOTS;
        sLoopCount--;
        HandleServer(sLoopPkt[i], sLoopLen[i], nowMs);
    }
}

/* The next relayed-to-us datagram from our own room (LAN host), or 0. */
static int PopLoopbackRelay(void* buf, int cap) {
    const int i = sLoopHead;
    int n;
    DrainLoopback(NowMs());
    if (sLoopCount == 0 || sSt.phase != GDX_P2P_IN_ROOM) {
        return 0;
    }
    n = sLoopLen[i];
    sLoopHead = (sLoopHead + 1) % P2P_LOOP_SLOTS;
    sLoopCount--;
    if (n > cap || n < 4 || (memcmp(sLoopPkt[i], "GXLB", 4) != 0 && memcmp(sLoopPkt[i], "GXMP", 4) != 0)) {
        return 0;
    }
    memcpy(buf, sLoopPkt[i], (size_t) n);
    sSt.rxRelay++;
    return n;
}

/* Keepalives get an answer every 2 s; a room whose host console (LAN, serverless) or server has
 * not answered for a while is gone. */
static void WatchServerSilence(uint32_t nowMs) {
    static uint32_t sLastWatchMs = 0;
    const int lanJoin = (sMode == GDX_P2P_MODE_LAN_JOIN);
    const uint32_t limit = lanJoin ? GDX_P2P_HOST_SILENT_MS : GDX_P2P_SERVER_SILENT_MS;
    const uint32_t gap = nowMs - sLastWatchMs;
    sLastWatchMs = nowMs;
    if (sMode == GDX_P2P_MODE_LAN_HOST || sSt.roomLost != 0 || sSt.lastServerRxMs == 0) {
        return; /* the LAN host is its own server */
    }
    if (gap > 1000u && gap < 0x80000000u) {
        sSt.lastServerRxMs += gap; /* we were not listening (a load, HOME): that silence is ours */
    }
    if ((int32_t) (nowMs - sSt.lastServerRxMs) > (int32_t) limit) {
        LoseRoom(lanJoin ? GDX_P2P_LOST_HOST_SILENT : GDX_P2P_LOST_SERVER_SILENT,
                 lanJoin ? "lost the host" : "lost the server");
    }
}

void gdx3ds_p2p_service(uint32_t nowMs) {
    int s;
    sNowMs = nowMs;
    if (sFd < 0) {
        return;
    }
    if (sMode == GDX_P2P_MODE_LAN_HOST) {
        gdx_lan_room_expire(&sLanRoom, nowMs, LanEmit, NULL);
        DrainLoopback(nowMs);
    }
    if (sSt.phase == GDX_P2P_REQUESTING) {
        if (sReqHeldError != 0 && (int32_t) (nowMs - sReqFirstMs) >= P2P_GHOST_RETRY_MS) {
            Fail(sReqHeldError, sReqHeldDetail);
        } else if ((int32_t) (nowMs - sReqFirstMs) > P2P_REQUEST_GIVE_UP_MS) {
            Fail(-1, sMode == GDX_P2P_MODE_LAN_JOIN ? "no answer from the host" : "no answer from the server");
        } else if ((int32_t) (nowMs - sReqLastMs) >= P2P_REQUEST_RETRY_MS) {
            SendRequest(nowMs);
        }
        return;
    }
    if (sSt.phase != GDX_P2P_IN_ROOM) {
        return;
    }
    WatchServerSilence(nowMs);
    if ((int32_t) (nowMs - sLastKaMs) >= P2P_KEEPALIVE_MS || (!sSt.haveRoster && (int32_t) (nowMs - sLastKaMs) >= 500)) {
        SendKeepalive(nowMs);
    }
    for (s = 0; s < GDX_MPO_MAX_SLOTS; s++) {
        if (s != sSt.mySlot) {
            gdx_link_tick(&sLink[s], nowMs);
        }
    }
}

const GdxP2pStatus* gdx3ds_p2p_status(void) {
    return &sSt;
}

int gdx3ds_p2p_direct_mask(uint32_t nowMs) {
    int s;
    int mask = 0;
    for (s = 0; s < GDX_MPO_MAX_SLOTS; s++) {
        if (s != sSt.mySlot && gdx_link_hear_direct(&sLink[s], nowMs)) {
            mask |= 1 << s;
        }
    }
    return mask;
}

/* Which candidate the learned endpoint is: the roster's public / private one, or neither. */
static const char* LearnedTag(const GdxMpLink* l) {
    if (l->candValid[0] && gdx_mpo_endpoint_eq(&l->cand[2], &l->cand[0])) {
        return "pub";
    }
    if (l->candValid[1] && gdx_mpo_endpoint_eq(&l->cand[2], &l->cand[1])) {
        return "priv";
    }
    return "learned";
}

int gdx3ds_p2p_route_text(int slot, char* buf, int cap) {
    const GdxMpLink* l;
    if (cap <= 0) {
        return 0;
    }
    buf[0] = '\0';
    if (slot < 0 || slot >= GDX_MPO_MAX_SLOTS || slot == sSt.mySlot || sLink[slot].state == GDX_LINK_NONE) {
        return 0;
    }
    l = &sLink[slot];
    if (!l->candValid[2]) {
        snprintf(buf, (size_t) cap, "s%d=%s (no direct endpoint yet)", slot, gdx_link_state_name(l->state));
        return 1;
    }
    snprintf(buf, (size_t) cap, "s%d=%s %u.%u.%u.%u:%u (%s)", slot, gdx_link_state_name(l->state), l->cand[2].ip[0],
             l->cand[2].ip[1], l->cand[2].ip[2], l->cand[2].ip[3], l->cand[2].port, LearnedTag(l));
    return 1;
}

void gdx3ds_p2p_on_peer_report(int slot, int theyHearUs, uint32_t nowMs) {
    if (slot >= 0 && slot < GDX_MPO_MAX_SLOTS && slot != sSt.mySlot) {
        gdx_link_on_peer_report(&sLink[slot], theyHearUs, nowMs);
    }
}

const GdxMpLink* gdx3ds_p2p_link(int slot) {
    if (slot < 0 || slot >= GDX_MPO_MAX_SLOTS) {
        return NULL;
    }
    return &sLink[slot];
}

/* ---- the transport ops ------------------------------------------------------------------------ */
static int OpsOpen(const GdxMpTransportConfig* cfg) {
    (void) cfg;
    return sFd >= 0 ? 0 : -1;
}

static void OpsBindLastSource(int player) {
    (void) player; /* binding happens inside recv, from an authenticated GXLB */
}

/* Sends to the slots in `slotMask` (never ourselves): direct copies per link route, plus ONE
 * relay datagram for every slot that wants a relayed copy. `probe` = a lobby HELLO. */
static int SendMasked(int slotMask, const void* buf, int len, int probe) {
    int s;
    int relayMask = 0;
    int total = 0;
    const uint32_t now = NowMs();
    if (sFd < 0 || len <= 0 || len > GDX_MP_TRANSPORT_MTU || sSt.phase != GDX_P2P_IN_ROOM) {
        return sFd < 0 ? -1 : 0;
    }
    for (s = 0; s < GDX_MPO_MAX_SLOTS; s++) {
        GdxMpEndpoint ep[GDX_LINK_MAX_CAND];
        int via = 0;
        int k;
        int cnt;
        if (s == sSt.mySlot || !(slotMask & (1 << s)) || sLink[s].state == GDX_LINK_NONE) {
            continue;
        }
        cnt = gdx_link_route(&sLink[s], now, probe, ep, &via);
        for (k = 0; k < cnt; k++) {
            const int r = SendToStream(&ep[k], buf, len, gdx_mp_socsvc_stream(buf, len, 0));
            if (r > 0) {
                total += r;
                sSt.txDirect++;
            }
        }
        if (via) {
            relayMask |= 1 << s;
        }
    }
    if (relayMask != 0 && len <= GDX_RV_MAX_RELAY_PAYLOAD) {
        uint8_t pkt[GDX_RV_AUTH_BYTES + GDX_RV_MAX_RELAY_PAYLOAD];
        const int n = gdx_rv_build_relay(sSt.code, sSt.mySlot, relayMask, sMemberKey, (const uint8_t*) buf, len,
                                         pkt, sizeof(pkt));
        if (n > 0) {
            const int r = SendToStream(&sCfg.server, pkt, n, gdx_mp_socsvc_stream(buf, len, relayMask));
            if (r > 0) {
                total += r;
                sSt.txRelay++;
            }
        }
    }
    return total;
}

int gdx3ds_p2p_send_ex(int slot, const void* buf, int len, int probe) {
    if (slot == GDX_MP_PEER_ANY) {
        return SendMasked(0xF & ~(1 << sSt.mySlot), buf, len, probe);
    }
    if (slot < 0 || slot >= GDX_MPO_MAX_SLOTS || slot == sSt.mySlot) {
        return -1;
    }
    return SendMasked(1 << slot, buf, len, probe);
}

static int OpsSend(int peer, const void* buf, int len) {
    return gdx3ds_p2p_send_ex(peer, buf, len, 0);
}

static int IsMagic(const uint8_t* p, int n, const char* m) {
    return n >= 4 && memcmp(p, m, 4) == 0;
}

/* A direct GXLB: the secret and a roster `from` are what make a stranger's address worth
 * binding. Returns the slot, -1 if it fails. The lobby parses the rest. */
static int CheckDirectLobby(const uint8_t* p, int n) {
    uint64_t secret = 0;
    int from;
    int i;
    if (n < GDX_LB_HEADER_BYTES || !sSt.haveRoster) {
        return -1;
    }
    for (i = 0; i < 8; i++) {
        secret |= (uint64_t) p[8 + i] << (8 * i);
    }
    from = p[6];
    if (secret != sSt.roomSecret || from >= GDX_MPO_MAX_SLOTS || from == sSt.mySlot ||
        !sSt.roster.member[from].present) {
        return -1;
    }
    return from;
}

static int BoundSlot(const GdxMpEndpoint* src) {
    int s;
    for (s = 0; s < GDX_MPO_MAX_SLOTS; s++) {
        if (s != sSt.mySlot && sLink[s].state != GDX_LINK_NONE && sLink[s].candValid[2] &&
            gdx_mpo_endpoint_eq(&sLink[s].cand[2], src)) {
            return s;
        }
    }
    return -1;
}

/* The next datagram from either medium: a UDS frame (src = its node's endpoint) first, then the
 * socket. Bytes, 0 = nothing waiting, negative = a socket error. */
/* NETTICK: in a local wireless room, 1 when the WiFi socket is due a poll (sUdsSocPollMs). */
static int UdsSocPollDue(void) {
    uint32_t now;
    if (sUdsSocPollMs < 0) {
        sUdsSocPollMs = gdx3ds_config_get_int("net", "mp_uds_soc_poll_ms", 250);
        if (sUdsSocPollMs < 0) {
            sUdsSocPollMs = 0;
        }
    }
    if (sUdsSocPollMs == 0) {
        return 1;
    }
    now = NowMs();
    if ((int32_t) (now - sUdsSocLastMs) >= sUdsSocPollMs) {
        sUdsSocLastMs = now;
        return 1;
    }
    gdx3ds_ipcstat_gated(GDX_IPC_SOC_RECV);
    return 0;
}

/* [net] mp_lan_relay_rx (default 1; 0 = refuse them, as before mp-online-hw): a LAN joiner takes
 * the datagrams the host's room relays for another joiner. */
static int LanRelayRxOn(void) {
    static int sLever = -1;
    if (sLever < 0) {
        sLever = gdx3ds_config_get_bool("net", "mp_lan_relay_rx", 1) ? 1 : 0;
    }
    return sLever;
}

/* A GXMP / GXLB whose sender is not the host (slot 0): what a LAN room's relay forwards. The
 * session checks the claimed sender against the roster as it does for the online relay. */
static int LanRelayedFromOther(const uint8_t* p, int n) {
    if (IsMagic(p, n, "GXMP")) {
        const int wire = gdx_mp_ls_packet_wire_slot(p, n);
        return wire >= 2 && wire <= GDX_MPO_MAX_SLOTS;
    }
    if (IsMagic(p, n, "GXLB") && n >= GDX_LB_HEADER_BYTES) {
        return p[6] >= 1 && p[6] < GDX_MPO_MAX_SLOTS;
    }
    return 0;
}

static int RawRecv(void* buf, int cap, GdxMpEndpoint* src) {
    struct sockaddr_in a;
    socklen_t alen = sizeof(a);
    ssize_t got;
    uint64_t t0;
    if (sUds) {
        int node = 0;
        const int n = gdx3ds_mp_uds_raw_recv(sUdsRx, (int) sizeof(sUdsRx), &node);
        if (n > 0 && n <= cap) {
            memcpy(buf, sUdsRx, (size_t) n);
            gdx_uds_node_endpoint(node, src);
            sSt.rxUds++;
            return n;
        }
        if (!gdx3ds_socthr_active() && !UdsSocPollDue()) {
            return 0; /* (the socket thread polls it at that period itself) */
        }
    }
    if (gdx3ds_socthr_active()) {
        return gdx3ds_socthr_recv(buf, cap, src);
    }
    t0 = gdx3ds_ipcstat_begin();
    got = recvfrom(sFd, buf, (size_t) cap, 0, (struct sockaddr*) &a, &alen);
    gdx3ds_ipcstat_end(GDX_IPC_SOC_RECV, t0);
    if (got < 0) {
        return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -errno;
    }
    FromSockaddr(&a, src);
    return (int) got;
}

void gdx3ds_p2p_set_uds(int on) {
    sUds = on ? 1 : 0;
    gdx3ds_socthr_set_rare_poll(sUds);
}

void gdx3ds_p2p_wait_rx(uint32_t maxMs) {
    if (gdx3ds_socthr_active() && !sUds) {
        gdx3ds_socthr_wait_rx(maxMs);
    } else {
        svcSleepThread(500000LL); /* inline: poll again in 0.5 ms, as before */
    }
}

void gdx3ds_p2p_park(int park) {
    gdx3ds_socthr_park(park);
}

int gdx3ds_p2p_uds(void) {
    return sUds;
}

static int OpsRecv(void* buf, int cap, int* peerHint) {
    int guard;
    *peerHint = GDX_MP_PEER_ANY;
    if (sFd < 0) {
        return -1;
    }
    if (sMode == GDX_P2P_MODE_LAN_HOST && sLoopCount > 0) {
        const int n = PopLoopbackRelay(buf, cap);
        if (n > 0) {
            *peerHint = GDX_P2P_HINT_RELAY;
            return n;
        }
    }
    for (guard = 0; guard < P2P_RX_GUARD; guard++) {
        GdxMpEndpoint src;
        const uint32_t now = NowMs();
        uint8_t* p = (uint8_t*) buf;
        const int n = RawRecv(buf, cap, &src);
        int slot;
        if (n <= 0) {
            return n;
        }
        if (sMode == GDX_P2P_MODE_LAN_HOST && IsMagic(p, n, "GXRV")) {
            /* a joiner talking to the room we host (it validates and rate-limits; the blocklist
             * still applies to a source it keeps refusing) */
            if (gdx_mpo_block_is_blocked(&sBlock, &src, now)) {
                sSt.dropBlocked++;
                continue;
            }
            {
                const uint32_t junkBefore = sLanRoom.junk;
                gdx_lan_room_handle(&sLanRoom, p, n, &src, now, LanEmit, NULL);
                if (sLanRoom.junk != junkBefore) {
                    gdx_mpo_block_note_reject(&sBlock, &src, now);
                }
            }
            DrainLoopback(now);
            continue;
        }
        if (gdx_mpo_endpoint_eq(&src, &sCfg.server) && IsMagic(p, n, "GXRV")) {
            HandleServer(p, n, now);
            continue;
        }
        /* LAN join: the "server" is the host console, so its GXLB/GXMP is its own direct traffic. */
        if (sMode != GDX_P2P_MODE_LAN_JOIN && gdx_mpo_endpoint_eq(&src, &sCfg.server)) {
            if (sSt.phase == GDX_P2P_IN_ROOM && (IsMagic(p, n, "GXLB") || IsMagic(p, n, "GXMP"))) {
                sSt.rxRelay++;
                *peerHint = GDX_P2P_HINT_RELAY;
                return n;
            }
            sSt.dropBadRelay++;
            continue;
        }
        if (sMode == GDX_P2P_MODE_LAN_JOIN && gdx_mpo_endpoint_eq(&src, &sCfg.server) && sSt.phase == GDX_P2P_IN_ROOM &&
            LanRelayRxOn() && LanRelayedFromOther(p, n)) {
            /* mp-online-hw: the host's room relayed another joiner's datagram (its relay forwards
             * verbatim, from the host's address). Before, the sender check below refused it, so a
             * joiner pair that could not reach each other directly never heard each other. */
            sSt.rxRelay++;
            *peerHint = GDX_P2P_HINT_RELAY;
            return n;
        }
        if (gdx_mpo_block_is_blocked(&sBlock, &src, now)) {
            sSt.dropBlocked++;
            continue;
        }
        if (sCfg.simSymmetric && !(sMode == GDX_P2P_MODE_LAN_JOIN && gdx_mpo_endpoint_eq(&src, &sCfg.server))) {
            /* (a LAN joiner's "server" is the host console: its traffic is the server's, which a
             * symmetric NAT lets through as it does the rendezvous server's) */
            sSt.dropSim++; /* test lever: as if no unsolicited direct datagram ever got through */
            continue;
        }
        if (sSt.phase != GDX_P2P_IN_ROOM) {
            sSt.dropUnknown++;
            continue;
        }
        slot = BoundSlot(&src);
        if (slot >= 0) {
            if (!gdx_mpo_bucket_take(&sPeerBucket[slot], now)) {
                sSt.dropRate++;
                continue;
            }
            if (IsMagic(p, n, "GXMP")) {
                if (gdx_mp_ls_packet_wire_slot(p, n) != slot + 1) {
                    sSt.dropUnknown++;
                    continue;
                }
                gdx_link_on_direct_rx(&sLink[slot], &src, now);
                if (sMode == GDX_P2P_MODE_LAN_HOST) {
                    gdx_lan_room_touch(&sLanRoom, slot, now);
                }
                sSt.rxDirect++;
                *peerHint = slot;
                return n;
            }
            if (IsMagic(p, n, "GXLB") && CheckDirectLobby(p, n) == slot) {
                if (sMode == GDX_P2P_MODE_LAN_HOST) {
                    gdx_lan_room_touch(&sLanRoom, slot, now);
                }
                gdx_link_on_direct_rx(&sLink[slot], &src, now);
                sSt.rxDirect++;
                *peerHint = slot;
                return n;
            }
            sSt.dropUnknown++;
            continue;
        }
        /* A stranger: only a GXLB with the room secret may introduce itself. */
        if (!gdx_mpo_bucket_take(&sStrangerBucket, now)) {
            sSt.dropRate++;
            continue;
        }
        if (IsMagic(p, n, "GXMP")) {
            /* Not proof of anything (no secret), but not hostile either: while punching a peer
             * sends to every candidate of ours, so a second path of a real peer lands here. */
            sSt.dropUnknown++;
            continue;
        }
        slot = IsMagic(p, n, "GXLB") ? CheckDirectLobby(p, n) : -1;
        if (slot < 0) {
            sSt.dropUnknown++;
            gdx_mpo_block_note_reject(&sBlock, &src, now);
            continue;
        }
        gdx_link_on_direct_rx(&sLink[slot], &src, now); /* the peer-reflexive candidate */
        sSt.rxDirect++;
        *peerHint = slot;
        return n;
    }
    return 0;
}

static void OpsClose(void) {
}

static int OpsReopen(void) {
    int fd;
    gdx3ds_socthr_stop();
    if (sFd >= 0) {
        close(sFd);
        sFd = -1;
    }
    fd = OpenSocket(sBoundPort);
    if (fd < 0) {
        return fd;
    }
    sFd = fd;
    gdx3ds_socthr_start(fd);
    gdx3ds_socthr_set_rare_poll(sUds);
    return 0;
}

static int OpsSelftest(char* buf, int cap) {
    int s;
    int off = snprintf(buf, (size_t) cap, "fd=%d port=%d slot=%d phase=%d srv_rtt=%lu tx=%lu/%lu rx=%lu/%lu/%lu "
                       "drop=%lu/%lu/%lu/%lu/%lu uds=%d/%lu thr=%d", sFd, sBoundPort, sSt.mySlot, sSt.phase,
                       (unsigned long) sSt.serverRttMs, (unsigned long) sSt.txDirect, (unsigned long) sSt.txRelay,
                       (unsigned long) sSt.rxDirect, (unsigned long) sSt.rxRelay, (unsigned long) sSt.rxServer,
                       (unsigned long) sSt.dropUnknown, (unsigned long) sSt.dropBlocked,
                       (unsigned long) sSt.dropRate, (unsigned long) sSt.dropBadRelay, (unsigned long) sSt.dropSim, sUds,
                       (unsigned long) sSt.rxUds, gdx3ds_socthr_active());
    for (s = 0; s < GDX_MPO_MAX_SLOTS && off > 0 && off < cap - 24; s++) {
        if (s != sSt.mySlot && sLink[s].state != GDX_LINK_NONE) {
            off += snprintf(buf + off, (size_t) (cap - off), " s%d=%s", s, gdx_link_state_name(sLink[s].state));
        }
    }
    return 0;
}

static const GdxMpTransportOps kP2pOps = {
    "udp-p2p", OpsOpen, OpsBindLastSource, OpsSend, OpsRecv, OpsClose, OpsReopen, OpsSelftest,
    NULL, /* present_mask: a UDP path cannot tell; the removal protocol decides (gdx_mp_remove) */
};

const GdxMpTransportOps* gdx3ds_mp_p2p_ops(void) {
    return &kP2pOps;
}
