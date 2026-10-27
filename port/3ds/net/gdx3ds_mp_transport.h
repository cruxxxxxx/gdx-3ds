/* port/3ds/net/gdx3ds_mp_transport.h — MP spike: the datagram transport behind lockstep.
 *
 * The lockstep/session layer (gdx3ds_mp.c + port/gdx_mp_lockstep.c) speaks ONLY in player
 * indices 0..N-1 and opaque datagrams of <= GDX_MP_TRANSPORT_MTU bytes. Everything that is
 * specific to a medium stays behind these ops:
 *
 *   backend    addressing inside the backend          topology       status
 *   udp-lan    (ip, port) table, learned/configured     star (host relays) or mesh   this spike
 *   uds        player -> NetworkNodeID table, learned     star (the host relays)     gdx3ds_mp_uds.c
 *              like udp's (joiner: host = node 1)
 *   udp-p2p    (ip, port) from a rendezvous server,     mesh, relay fallback         later
 *              hole-punched; relay server for symmetric NAT
 *
 * Semantics every backend must keep (so a backend can be swapped without touching sync):
 *   - unreliable, unordered datagrams; the lockstep layer's ack window covers loss/reorder/dup;
 *   - never blocks: send drops on a full queue, recv returns 0 when nothing is waiting;
 *   - `send(peer)` addresses a PLAYER, never a node/socket id; -1 = every other player;
 *   - `recv` reports the sender's player index only as a HINT (UDS knows the node, UDP knows
 *     an address that may not be bound to a player yet): the authoritative `from` is inside
 *     the datagram and is validated by the session layer;
 *   - payload bytes are untrusted input; the backend never interprets them.
 * Called from the main thread only. */
#ifndef GDX3DS_MP_TRANSPORT_H
#define GDX3DS_MP_TRANSPORT_H

#ifdef __cplusplus
extern "C" {
#endif

#define GDX_MP_TRANSPORT_MTU 1400 /* < UDS_DATAFRAME_MAXSIZE (1478) and a safe UDP payload */
#define GDX_MP_PEER_ANY (-1)

typedef struct GdxMpTransportConfig {
    int isHost;
    int localPlayer;
    int numPlayers;
    /* udp-lan: */
    int bindPort;          /* local UDP port */
    const char* hostAddr;  /* joiners: the host's IPv4 */
    int hostPort;          /* joiners: the host's UDP port */
    /* uds (mp-uds): everything the beacon advertises / a joiner filters on. */
    const char* build;     /* GDX3DS_BUILD_ID: the beacon's build field + the passphrase */
    unsigned int contentHash;
    int delay;             /* host: advertised; joiner: adopted from the beacon */
    int hashEvery;
    const char* joinName;  /* joiner: only a host whose profile name is this ("" = any) */
} GdxMpTransportConfig;

typedef struct GdxMpTransportOps {
    const char* name;
    /* 0 on success; a negative value is a backend error code for the receipt. */
    int (*open)(const GdxMpTransportConfig* cfg);
    /* Bind `player`'s address to the source of the datagram most recently returned by recv
     * (udp: learn a joiner's (ip, port) from its first valid packet; uds: no-op). */
    void (*bind_last_source)(int player);
    /* Bytes sent (>= 0), or negative on a hard error. An unknown peer address is not an
     * error: 0 is returned and nothing is sent. */
    int (*send)(int peer, const void* buf, int len);
    /* Bytes received into buf (> 0), 0 when nothing is waiting, negative on a hard error.
     * *peerHint = the player bound to the source, or GDX_MP_PEER_ANY if none yet. */
    int (*recv)(void* buf, int cap, int* peerHint);
    void (*close)(void);
    /* Re-create the endpoint, keeping every learned peer address (recovery from a wedged
     * socket / a WiFi drop). 0 on success. */
    int (*reopen)(void);
    /* Diagnostics: send one junk datagram to our own endpoint (the next recv should see it)
     * and describe the endpoint into buf. Returns the sendto result. */
    int (*selftest)(char* buf, int cap);
    /* mp-uds. Players the MEDIUM says are still there (bit per player, us included); a player
     * whose address is not learned yet counts as present. NULL = the medium cannot tell (udp):
     * only silence (the session's mp_drop_ms) removes a player then. */
    unsigned int (*present_mask)(void);
} GdxMpTransportOps;

/* udp-lan over soc:U (gdx3ds_mp_udp.c). SOC must already be initialised. */
const GdxMpTransportOps* gdx3ds_mp_udp_ops(void);
/* 3DS local wireless (gdx3ds_mp_uds.c; contract and IDs in gdx3ds_mp_uds.h). */
const GdxMpTransportOps* gdx3ds_mp_uds_ops(void);

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_MP_TRANSPORT_H */
