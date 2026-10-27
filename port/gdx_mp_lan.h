/* port/gdx_mp_lan.h — mp-online NEARBY: same-WiFi rooms without a rendezvous server. Pure (no
 * socket, no clock, no global): port/tests/gdx_mp_online_host_tests.c drives all of it.
 *
 * TWO PIECES.
 *
 * 1. THE LAN ROOM. On the same WiFi the HOST CONSOLE is the room server: it runs one room of the
 *    GXRV protocol (tools/mp-rendezvous/README.md) on its own game socket, so everything above the
 *    transport -- roster, epochs, the lobby, lockstep, removal -- is the SAME code as online. A
 *    joiner simply uses the host's (ip, port) as its "rendezvous server". One room, at most 4
 *    members, no relay needed but kept (verbatim, validated as the server does) so a joiner pair
 *    whose direct link fails still works through the host. The host's own client talks to the
 *    room in-process (its member address is GDX_LAN_SELF, never a real address).
 *    Hostile input: every request is validated as gdx_rendezvous.py validates it; requests are
 *    rate-limited per room (a LAN has few sources; a flood just loses to the bucket); member keys
 *    and the room secret come from the caller's entropy.
 *
 * 2. THE BEACON (GXLN). A host announces its room about once a second on the discovery port(s);
 *    a listener keeps a table of rooms heard recently (entries expire), pings each one for a
 *    round-trip time, and marks a room joinable only when its build, content hash and protocol
 *    match ours. Payload: room code, host name, players n/max, console model, build, content hash.
 *    A beacon is display data: joining still goes through the room's JOIN checks.
 *
 * The 3DS side (sockets, broadcast, the timer) is port/3ds/net/gdx3ds_mp_discovery.c behind
 * gdx3ds_mp_discovery.h, an interface a UDS (local wireless) backend implements too. */
#ifndef GDX_MP_LAN_H
#define GDX_MP_LAN_H

#include "gdx_mp_online.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 1. the LAN room ------------------------------------------------------------------------- */
#define GDX_LAN_MEMBER_TIMEOUT_MS 15000 /* the default; gdx_lan_room_set_timeout changes it */
#define GDX_LAN_REJOIN_SILENT_MS 5000 /* as gdx_rendezvous.py REJOIN_SILENT_S */

/* The host's own client: an address no real datagram can come from. */
extern const GdxMpEndpoint GDX_LAN_SELF;

typedef struct GdxMpLanMember {
    int present;
    GdxMpEndpoint addr;
    GdxMpEndpoint priv;
    char name[GDX_MPO_NAME_LEN + 1];
    uint64_t key;
    uint32_t nonce;
    uint32_t lastSeenMs;
    uint16_t epoch;
    GdxMpBucket ctl;
    GdxMpBucket relay;
} GdxMpLanMember;

typedef struct GdxMpLanRoom {
    int open;
    char code[GDX_MPO_CODE_LEN + 1];
    char build[GDX_MPO_BUILD_LEN + 1];
    uint32_t contentHash;
    int maxPlayers;
    uint64_t secret;
    uint32_t rosterVersion;
    uint16_t joins;
    int locked;
    uint64_t rng; /* xorshift state for member keys (seeded from the caller's entropy) */
    GdxMpLanMember member[GDX_MPO_MAX_SLOTS];
    GdxMpBucket requests;    /* every CREATE / JOIN, from anyone */
    GdxMpBucket failedJoins; /* wrong-code JOINs */
    uint32_t memberTimeoutMs;
    uint32_t rx, junk, rateLimited, relayed;
} GdxMpLanRoom;

/* Where the room's replies go. `to` may be GDX_LAN_SELF (the host's own client). */
typedef void (*GdxMpLanEmit)(void* ctx, const GdxMpEndpoint* to, const uint8_t* pkt, int len);

/* Opens the room: `code` (5 code chars) is what the beacon advertises and JOIN must carry;
 * `entropy` seeds the room secret and the member keys. The host is not a member until its own
 * CREATE (from GDX_LAN_SELF, with this code) arrives. Returns 0, or -1 on a bad argument. */
int gdx_lan_room_open(GdxMpLanRoom* r, const char* code, const char* build, uint32_t contentHash, int maxPlayers,
                      uint64_t entropy, uint32_t nowMs);
void gdx_lan_room_close(GdxMpLanRoom* r, GdxMpLanEmit emit, void* ctx); /* tells joiners HOST_LEFT */
/* One GXRV datagram from `from`. Returns 1 if it was a GXRV request (handled or dropped), 0 if
 * it is not GXRV at all (the caller handles it as game traffic). */
int gdx_lan_room_handle(GdxMpLanRoom* r, const uint8_t* pkt, int len, const GdxMpEndpoint* from, uint32_t nowMs,
                        GdxMpLanEmit emit, void* ctx);
/* Timers: drops members silent for GDX_LAN_MEMBER_TIMEOUT_MS (never the host). */
void gdx_lan_room_expire(GdxMpLanRoom* r, uint32_t nowMs, GdxMpLanEmit emit, void* ctx);
int gdx_lan_room_count(const GdxMpLanRoom* r);
/* An authenticated game datagram (GXLB with the room secret / GXMP from a bound endpoint) came
 * from `slot`: it is alive even if its keepalives are late (a long load, a busy main loop). */
void gdx_lan_room_touch(GdxMpLanRoom* r, int slot, uint32_t nowMs);
void gdx_lan_room_set_timeout(GdxMpLanRoom* r, uint32_t memberTimeoutMs);

/* ---- 2. the beacon --------------------------------------------------------------------------- */
#define GDX_LAN_VERSION 1
#define GDX_LAN_DISCOVERY_PORT 27210 /* + 0..GDX_LAN_DISCOVERY_PORTS-1: a beacon goes to all */
#define GDX_LAN_DISCOVERY_PORTS 4    /* several listeners on one machine (the emulator lane) */
#define GDX_LAN_BEACON_MS 1000
#define GDX_LAN_EXPIRE_MS 3500
#define GDX_LAN_PING_MS 1000
#define GDX_LAN_BEACON_BYTES 84
#define GDX_LAN_PING_BYTES 20
enum {
    GDX_LAN_BEACON = 1,
    GDX_LAN_PING = 2,
    GDX_LAN_PONG = 3,
};
enum {
    GDX_LAN_FLAG_RACING = 1, /* a race is on: not joinable now (no mid-race join) */
    GDX_LAN_FLAG_FULL = 2,
};

typedef struct GdxMpLanBeacon {
    uint32_t hostId;  /* random per room: tells two rooms on one address apart, and ourselves */
    uint32_t contentHash;
    uint16_t roomPort; /* the host's game socket; the ip is the beacon's source */
    uint8_t players;
    uint8_t maxPlayers;
    uint8_t model;     /* GDX_MPO_MODEL_* */
    uint8_t flags;     /* GDX_LAN_FLAG_* */
    uint8_t proto;     /* GDX_MPO_PROTO */
    char code[GDX_MPO_CODE_LEN + 1];
    char name[GDX_MPO_NAME_LEN + 1]; /* the host's name */
    char build[GDX_MPO_BUILD_LEN + 1];
} GdxMpLanBeacon;

int gdx_lan_build_beacon(const GdxMpLanBeacon* b, uint8_t* out, int cap);
/* PING (listener -> host) and PONG (host -> listener, the same 20 bytes echoed: no amplification). */
int gdx_lan_build_ping(int type, uint32_t hostId, uint32_t nonce, uint32_t sendMs, uint8_t* out, int cap);
/* Returns the type (BEACON fills *b; PING / PONG fill the three words), -1 when malformed. */
int gdx_lan_parse(const uint8_t* pkt, int len, GdxMpLanBeacon* b, uint32_t* hostId, uint32_t* nonce, uint32_t* sendMs);

/* ---- the nearby table (what the NEARBY tab lists) ------------------------------------------- */
#define GDX_LAN_MAX_NEARBY 8

typedef struct GdxMpNearbyRoom {
    int used;
    GdxMpLanBeacon b;
    GdxMpEndpoint room; /* where JOIN goes: (beacon source ip, roomPort) */
    GdxMpEndpoint beaconFrom;
    uint32_t firstSeenMs;
    uint32_t lastSeenMs;
    uint32_t lastPingMs;
    int pingMs;         /* -1 = not measured */
    uint32_t pingNonce; /* the outstanding PING (0 = none) */
    int signal;         /* 0..3 bars (from the ping here; UDS: the radio's own figure) */
    int compatible;     /* build + content + protocol match ours */
    int joinable;       /* compatible, not racing, not full */
    const char* whyNot; /* when not joinable */
    /* Which medium heard it (GDX_NEARBY_MEDIUM_*; the udp-lan table leaves both 0) and, for a
     * merged list (port/gdx_mp_uds_bridge.h), the row's index in its own backend's table. */
    int medium;
    int srcIndex;
} GdxMpNearbyRoom;

enum {
    GDX_NEARBY_MEDIUM_WIFI = 0, /* udp-lan: GXLN beacon on the same WiFi */
    GDX_NEARBY_MEDIUM_UDS = 1,  /* 3DS local wireless (no router): NWM beacon appdata */
};

typedef struct GdxMpNearby {
    GdxMpNearbyRoom room[GDX_LAN_MAX_NEARBY];
    char build[GDX_MPO_BUILD_LEN + 1];
    uint32_t contentHash;
    uint32_t selfHostId; /* our own room's beacon (0 = not hosting) is never listed */
    uint32_t pingNonce;
    uint32_t heard, junk;
} GdxMpNearby;

void gdx_nearby_init(GdxMpNearby* n, const char* build, uint32_t contentHash);
/* A beacon from `src`. Updates or adds its row (the oldest row goes when the table is full). */
void gdx_nearby_on_beacon(GdxMpNearby* n, const GdxMpLanBeacon* b, const GdxMpEndpoint* src, uint32_t nowMs);
/* A PONG from `src` echoing our PING. */
void gdx_nearby_on_pong(GdxMpNearby* n, uint32_t hostId, uint32_t nonce, uint32_t sendMs, uint32_t nowMs);
/* Expires rows and returns the index of a row due a PING now (its lastPingMs is stamped), or -1. */
int gdx_nearby_tick(GdxMpNearby* n, uint32_t nowMs);
/* The rows, newest-first order is not kept: rows sorted by (joinable, name) into idx[]. */
int gdx_nearby_sorted(const GdxMpNearby* n, int* idx, int max);
int gdx_nearby_signal_from_ping(int pingMs);

#ifdef __cplusplus
}
#endif

#endif /* GDX_MP_LAN_H */
