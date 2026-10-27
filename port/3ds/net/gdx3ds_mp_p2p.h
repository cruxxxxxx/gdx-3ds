/* port/3ds/net/gdx3ds_mp_p2p.h — mp-online: the internet (udp-p2p) transport backend.
 *
 * ONE non-blocking UDP socket on soc:U carries everything: GXRV to the rendezvous server, GXLB
 * lobby and GXMP lockstep datagrams to peers -- directly when a hole-punched path is confirmed
 * both ways, through the server's relay otherwise (port/gdx_mp_online.h, the link state machine).
 * The protocol logic is pure and host-tested (port/gdx_mp_online.c); this TU is the socket, the
 * timers, the receive demux and the receive hygiene.
 *
 * ADDRESSING. The ops in gdx3ds_mp_transport.h address ROOM SLOTS here (0 = the host). The
 * session layer maps lockstep player indices to slots from the START packet.
 *
 * RECEIVE DEMUX (recv op):
 *   from the server: GXRV is consumed inside (WELCOME/ROSTER/KA_ACK/ERROR/PUNCH_NOW); a relayed
 *       GXLB/GXMP is returned with *peerHint = GDX_P2P_HINT_RELAY (the server checked the sender);
 *   from anyone else: a GXLB with the room secret and a roster `from` binds that source as the
 *       peer's learned endpoint and is returned with hint = slot; a GXMP is returned only from a
 *       bound endpoint (hint = slot); everything else is dropped, counted, and repeat offenders
 *       are blocklisted. Per-source token buckets cap what any one peer can make us parse.
 *
 * LAN MODE (NEARBY, port/gdx_mp_lan.h). Host: this socket also runs a one-room GXRV server; our
 *   own client reaches it in-process (server = GDX_LAN_SELF) and joiners reach it at our game
 *   endpoint. Joiner: the "server" is the host console itself, so a GXLB/GXMP from that address
 *   is the host's own traffic (a direct link), not a relayed copy. Online mode is unchanged.
 *
 * THE SOCKET THREAD ([net] mp_soc_thread, default 1: gdx3ds_mp_socthr.h). While the socket is
 *   open one service thread makes every sendto / recvfrom on it; this TU's send path queues the
 *   datagram (tagging a lockstep INPUT with its coalescing stream) and its recv path takes the
 *   next one the thread received. Everything above the socket (the demux, the room, the links,
 *   the buckets) still runs here, on the main thread, in the same order.
 * Main thread only; never blocks. */
#ifndef GDX3DS_MP_P2P_H
#define GDX3DS_MP_P2P_H

#include "gdx3ds_mp_transport.h"
#include "gdx_mp_online.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GDX_P2P_HINT_RELAY (-2)

enum {
    GDX_P2P_MODE_ONLINE = 0, /* the configured rendezvous server */
    GDX_P2P_MODE_LAN_HOST = 1,
    GDX_P2P_MODE_LAN_JOIN = 2,
};

/* GdxP2pStatus.roomLost causes of our own (the server's are GDX_RV_E_*, all > 0). */
enum {
    GDX_P2P_LOST_HOST_SILENT = -10,   /* a LAN / serverless host: nothing from it for GDX_P2P_HOST_SILENT_MS */
    GDX_P2P_LOST_SERVER_SILENT = -11, /* the rendezvous server: nothing for GDX_P2P_SERVER_SILENT_MS */
};
/* GdxP2pStatus.errorCode of a request the server answered with UPDATE (version gating): the
 * numbers are in GdxP2pStatus.update; gdx_rv_update_message says it. */
#define GDX_P2P_E_UPDATE (-20)
#define GDX_P2P_HOST_SILENT_MS 10000
#define GDX_P2P_SERVER_SILENT_MS 20000

enum {
    GDX_P2P_IDLE = 0,
    GDX_P2P_REQUESTING = 1, /* CREATE/JOIN sent, waiting for WELCOME */
    GDX_P2P_IN_ROOM = 2,
    GDX_P2P_FAILED = 3, /* error code + detail set; leave() returns to IDLE */
};

typedef struct GdxP2pConfig {
    GdxMpEndpoint server;
    int bindPort;      /* 0 = any */
    int forceRelay;    /* [net] mp_relay = force */
    int simSymmetric;  /* test lever: drop every direct datagram we receive (we act as if behind a
                          symmetric NAT; the links must fall back to relay on their own) */
    const char* build;
    uint32_t contentHash;
    const char* name;
    uint32_t lanTimeoutMs; /* LAN host: a joiner silent this long leaves the room (0 = default) */
    int rvCookie;          /* [net] mp_rv_cookie: GDX_RV_COOKIE_MODE_* for online CREATE / JOIN */
    int clientProto;       /* what v2 requests carry (GDX_MPO_PROTO; the [net] mp_rv_proto test lever) */
    void (*log)(const char* fmt, ...); /* receipts ("[mpo] rv cookie: ..."); NULL = none */
} GdxP2pConfig;

typedef struct GdxP2pStatus {
    int phase;
    int errorCode;          /* GDX_RV_E_*, or < 0 for a local error */
    char errorDetail[GDX_MPO_BUILD_LEN + 1];
    char code[GDX_MPO_CODE_LEN + 1];
    int mySlot;
    int maxPlayers;
    int forceRelay;
    uint64_t roomSecret;
    GdxMpRvRoster roster;
    int haveRoster;
    uint32_t rosterChanges; /* bumps on every new roster: the session re-reads it */
    uint32_t serverRttMs;
    uint32_t lastServerRxMs;
    /* mp-menus: the room is gone (the host left, the server forgot us or went silent, a LAN host
     * stopped answering) but the phase stays IN_ROOM, so the links to the other consoles keep
     * working: a race in progress over direct links can finish. GDX_RV_E_* or a GDX_P2P_LOST_*;
     * 0 = the room is fine. The session decides, then leave()s. */
    int roomLost;
    char roomLostDetail[GDX_MPO_BUILD_LEN + 1];
    GdxMpEndpoint seenAs;
    int mode;               /* GDX_P2P_MODE_* of the current / last request */
    int rvVersion;          /* GXRV header version of the current / last CREATE / JOIN (1 or 2) */
    int clientProto;        /* the client proto that request carried (v2) */
    GdxMpRvUpdate update;   /* errorCode == GDX_P2P_E_UPDATE: the server's range */
    uint16_t errorHostProto; /* a BUILD / CONTENT mismatch: the host's proto (0 = unknown) */
    /* counters */
    uint32_t txDirect, txRelay, rxDirect, rxRelay, rxServer;
    uint32_t dropUnknown, dropBlocked, dropRate, dropBadRelay, dropSim;
    uint32_t rxUds;         /* frames that arrived over local wireless */
} GdxP2pStatus;

/* The lockstep wait's sleep: with the socket thread, until it hands over a datagram or `maxMs`;
 * inline (or a local wireless room), 0.5 ms. */
void gdx3ds_p2p_wait_rx(uint32_t maxMs);

/* "s<slot>=<link state> <ip>:<port> (pub|priv|learned)": the endpoint DIRECT sends to (the one
 * the peer was last heard from) and which roster candidate it is. 0 = no such peer. */
int gdx3ds_p2p_route_text(int slot, char* buf, int cap);

/* Opens the socket (SOC must be up). 0 on success. */
int gdx3ds_p2p_open(const GdxP2pConfig* cfg);
void gdx3ds_p2p_close(void);
/* Ask the server for a room: host (wantCode "" = server picks; only a test server honours one) or
 * join by code. Retries every second until WELCOME/ERROR. */
void gdx3ds_p2p_host(int maxPlayers, const char* wantCode);
/* mp-online-hw: the next gdx3ds_p2p_host asks for a relay-only room (the CREATE flag; the server
 * then tells every member to relay). Consumed by that host call. */
void gdx3ds_p2p_set_next_relay_only(int on);
void gdx3ds_p2p_join(const char* code);
/* NEARBY: host a room on this console (code = what the beacon advertises; entropy seeds the
 * room's keys), or join the LAN room at `host`. */
void gdx3ds_p2p_lan_host(int maxPlayers, const char* code, uint64_t entropy);
void gdx3ds_p2p_lan_join(const GdxMpEndpoint* host, const char* code);
/* Local wireless: 1 = endpoints in GDX_UDS_NET (port/gdx_mp_uds_bridge.h) are UDS nodes and UDS
 * frames are received alongside the socket. Set before a UDS room's lan_host / lan_join; the
 * controller clears it when it leaves the room. */
void gdx3ds_p2p_set_uds(int on);
int gdx3ds_p2p_uds(void);
/* APT: 1 = the socket thread sends what is queued, then makes no SOC call until 0. */
void gdx3ds_p2p_park(int park);
/* The LAN room we host (NULL when not hosting): the beacon reads its member count. */
int gdx3ds_p2p_lan_room_count(void);
/* Tell the server we are gone (best effort) and forget the room; the socket stays open. */
void gdx3ds_p2p_leave(void);
/* Timers: request retries, keepalive (every 2 s), link ticks. Call every frame (and while a
 * lockstep tick waits). */
void gdx3ds_p2p_service(uint32_t nowMs);
void gdx3ds_p2p_lock_room(int locked);
const GdxP2pStatus* gdx3ds_p2p_status(void);
/* Links: the HELLO's directMask, the peer's report of hearing us, per-slot state for the UI. */
int gdx3ds_p2p_direct_mask(uint32_t nowMs);
void gdx3ds_p2p_on_peer_report(int slot, int theyHearUs, uint32_t nowMs);
const GdxMpLink* gdx3ds_p2p_link(int slot);
/* Our own LAN endpoint (gethostid + the bound port). */
GdxMpEndpoint gdx3ds_p2p_local_endpoint(void);

/* The transport ops (send/recv by room slot). open/close in the ops are no-ops: use the
 * functions above. send(GDX_MP_PEER_ANY) = every other roster member, coalescing every relayed
 * copy into ONE relay datagram with a slot mask. */
const GdxMpTransportOps* gdx3ds_mp_p2p_ops(void);
/* send with the HELLO probe flag (a RELAY link also tries direct once per GDX_LINK_PROBE_MS). */
int gdx3ds_p2p_send_ex(int slot, const void* buf, int len, int probe);

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_MP_P2P_H */
