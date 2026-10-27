/* port/3ds/net/gdx3ds_mp_uds.h — mp-uds: 3DS local wireless (UDS / NWM) multiplayer transport and
 * NEARBY discovery, behind the same ops as udp-lan (gdx3ds_mp_transport.h) and the NEARBY
 * discovery interface (gdx3ds_mp_uds_nearby.h).
 *
 * IDS (fixed for this app; change GDX_UDS_ID8 for a different KIND of network, never the comm ID):
 *   wlancommID  GDX_UDS_WLANCOMMID = 0x47445833 ('G''D''X''3' big-endian). NWM only shows a
 *               console beacons carrying the same ID, so other games never list our rooms and
 *               we never list theirs. Arbitrary (homebrew has no assigned range); picked to be
 *               unlike retail IDs, which derive from the title's unique ID.
 *   id8         GDX_UDS_ID8 = 0x01: "a race room". It is NOT a protocol version: a room of a
 *               different build must still be SEEN (greyed out), so versions live in the beacon.
 *   data_channel GDX_UDS_DATA_CHANNEL = 1 (lockstep), GDX_UDS_PROBE_CHANNEL = 2 (the
 *               [net] mp_uds_probe_s throughput probe only).
 *
 * CRYPTO. NWM encrypts every data frame with a key derived from the network passphrase. Ours
 * is gdx_mp_uds_passphrase(build id, content hash, proto) (port/gdx_mp_beacon.h): every copy of
 * one build + game data derives the same 32 hex chars, any other derives a different one, so a
 * mismatched console cannot even associate (udsConnectNetwork fails) -- and the NEARBY list
 * already greys it out from the beacon. It is integrity / compatibility gating, not secrecy.
 * (Azahar's emulated NWM does not check the passphrase; the session digest still refuses.)
 *
 * TOPOLOGY: a star, the host is NetworkNodeID 1 (player 0). A joiner sends only to the host; the
 * host broadcasts its own packets in ONE frame (UDS_BROADCAST_NETWORKNODEID) and relays each
 * joiner's packet to the other joiners by unicast (the session layer's relay). Players map to
 * nodes through a learned table, exactly like udp's addresses: the host binds a node to a
 * player on that player's first valid packet. The lobby (or [net] mp_player) picks the player
 * index; by default a joiner is player = its node - 1.
 *
 * LIMITS (UDS_*, libctru 3ds/services/uds.h; measure with [net] mp_uds_probe_s):
 *   data frame <= UDS_DATAFRAME_MAXSIZE = 1478 B; the session's datagrams stay <= 1400
 *   (GDX_MP_TRANSPORT_MTU) and a GXMP INPUT is <= 132 B, a DROP <= 92 B;
 *   appdata <= 0xC8 = 200 B (the beacon is 88 B); max 16 nodes, capped at GDX_UDS_MAX_NODES = 4;
 *   receive buffer UDS_DEFAULT_RECVBUFSIZE = 0x2E30 B per bind (~100 lockstep packets).
 *
 * WIRELESS OFF. udsInit / create / connect failing with the wireless switch off (or the system
 * in airplane mode) is reported as GDX_UDS_E_WIRELESS_OFF with a sentence for the user; every
 * other failure keeps its raw NWM result code in the receipt.
 *
 * THREADS. udsScanBeacons and udsConnectNetwork block for hundreds of ms, so a worker thread
 * scans (and connects); the main thread only reads snapshots. Everything else (send, recv,
 * status) is main-thread only and happens only while no worker call is in flight. */
#ifndef GDX3DS_MP_UDS_H
#define GDX3DS_MP_UDS_H

#include "gdx3ds_mp_uds_nearby.h"
#include "gdx3ds_mp_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GDX_UDS_WLANCOMMID 0x47445833u
#define GDX_UDS_ID8 0x01
#define GDX_UDS_DATA_CHANNEL 1
#define GDX_UDS_PROBE_CHANNEL 2
#define GDX_UDS_MAX_NODES 4
#define GDX_UDS_SHAREDMEM_SIZE 0x6000 /* two binds (lockstep + probe) of UDS_DEFAULT_RECVBUFSIZE 0x2E30 */
#define GDX_UDS_SCAN_BUF_SIZE 0x4000
#define GDX_UDS_ROOM_STALE_MS 4000    /* a room not seen in a scan for this long leaves the list */

enum {
    GDX_UDS_OK = 0,
    GDX_UDS_E_WIRELESS_OFF = -1, /* the wireless switch / airplane mode */
    GDX_UDS_E_INIT = -2,         /* udsInit failed for another reason (rc in the receipt) */
    GDX_UDS_E_CREATE = -3,
    GDX_UDS_E_CONNECT = -4,      /* includes a passphrase (= build) mismatch on hardware */
    GDX_UDS_E_NO_HOST = -5,      /* no compatible room found before the timeout */
    GDX_UDS_E_THREAD = -6,
    GDX_UDS_E_BUSY = -7,
};

/* Join state (the session layer and the lobby poll it). */
enum {
    GDX_UDS_JOIN_IDLE = 0,
    GDX_UDS_JOIN_SEARCHING = 1, /* scanning for a matching room (auto-join) */
    GDX_UDS_JOIN_CONNECTING = 2,
    GDX_UDS_JOIN_CONNECTED = 3,
    GDX_UDS_JOIN_FAILED = 4,
};

/* udsInit (once; idempotent). GDX_UDS_OK or a GDX_UDS_E_* with a user-facing sentence in why. */
int gdx3ds_mp_uds_init(char* why, int cap);
/* The 3DS profile name (UTF-8, sanitised, <= 31 bytes) and the console model (CFG_SystemModel). */
void gdx3ds_mp_uds_profile(char* name, int cap, int* model);

/* Host side of the ops' open(): create the network with this beacon. */
int gdx3ds_mp_uds_host_beacon(const GdxMpBeacon* b);
/* Joiner side: auto-join the first compatible room (name filter "" = any). Non-blocking:
 * a worker scans and connects; poll gdx3ds_mp_uds_join_state. */
int gdx3ds_mp_uds_auto_join(const char* nameFilter, int timeoutMs);
int gdx3ds_mp_uds_join_state(char* why, int cap);
/* The joined room's beacon (valid once CONNECTED) and our NetworkNodeID (0 = none). */
const GdxMpBeacon* gdx3ds_mp_uds_joined_beacon(void);
int gdx3ds_mp_uds_local_node(void);
/* Joiner: the session's shape once the room is joined (players from the beacon, our index). */
void gdx3ds_mp_uds_set_session(int numPlayers, int localPlayer);
/* Host: nodes on the network now (host included), from the connection status. */
int gdx3ds_mp_uds_node_count(void);
/* Host: stop / allow new joiners (a race is on) and refresh the beacon. */
void gdx3ds_mp_uds_set_racing(int racing);

/* [net] mp_uds_probe_s: frame-size limits + throughput, both sides, `seconds` long, after the
 * first joiner connected and before the session starts. Receipts: `[uds] probe ...`. */
void gdx3ds_mp_uds_probe(int isHost, int seconds);

/* One `[uds] stats` receipt line (frames, bytes, errors, status) into buf. */
void gdx3ds_mp_uds_describe(char* buf, int cap);

/* ---- mp-online: UDS as a raw datagram medium under its room / lobby / lockstep ----------------
 * (gdx3ds_mp_p2p.c maps NetworkNodeID z to the endpoint GDX_UDS_NET.z, port/gdx_mp_uds_bridge.h).
 * set_proto picks the beacon protocol the passphrase and the compat judgement use (mp-online:
 * GDX_UDS_MPO_PROTO) -- call it before scanning or hosting. scan(1) starts / keeps the NEARBY scan
 * (idempotent; nothing while in a room), scan(0) pauses it. host_room / join_room stop the worker
 * first. leave() closes whatever network we are in and resets the join state. */
void gdx3ds_mp_uds_set_proto(int proto);
int gdx3ds_mp_uds_live(void);
int gdx3ds_mp_uds_scan(int on, const char* build, uint32_t contentHash, char* why, int cap);
int gdx3ds_mp_uds_host_room(const GdxMpBeacon* b, char* why, int cap);
int gdx3ds_mp_uds_join_room(const GdxMpUdsRoom* room);
void gdx3ds_mp_uds_leave(void);
/* udsExit while no room / join / host exists, so the radio can go back to WiFi (UDS keeps it in
 * local-wireless mode). Never blocks: call it every frame until it answers 1 (UDS down). */
int gdx3ds_mp_uds_release(void);
/* One frame to node 1..16 (bytes sent, 0 = not sent) / the next frame, *srcNode = its node. */
int gdx3ds_mp_uds_raw_send(int node, const void* buf, int len);
int gdx3ds_mp_uds_raw_recv(void* buf, int cap, int* srcNode);

/* NEARBY over UDS (gdx3ds_mp_uds_nearby.h). */
const GdxMpUdsNearbyOps* gdx3ds_mp_uds_discovery(void);

/* "[uds] svc ..." counters of the UDS service thread ([net] mp_uds_thread); "" when it is not running. */
void gdx3ds_mp_uds_svc_line(char* buf, int cap);

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_MP_UDS_H */
