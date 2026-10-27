/* port/gdx_mp_uds_bridge.h — mp-online over 3DS local wireless (UDS): the pure glue (host-tested).
 *
 * HOW A UDS ROOM WORKS. A local-wireless room is the SAME console-hosted room as a same-WiFi one
 * (port/gdx_mp_lan.h: GXRV CREATE/JOIN/KEEPALIVE/RELAY, then GXLB lobby + GXMP lockstep + the
 * removal protocol between the consoles). Only the datagram medium differs: the transport
 * (port/3ds/net/gdx3ds_mp_p2p.c) sends to an endpoint in GDX_UDS_NET.x.y.z as a UDS data frame to
 * NetworkNodeID z, and reports a UDS frame from node z as coming from that endpoint. So the room,
 * the lobby, the mesh, punching (node to node), the relay fallback (through the host's room) and
 * removal run unchanged, and are covered by the same host tests as the WiFi path.
 *
 *   fake endpoint   169.254.213.<node>:54741   (link-local, never routed; node 1 = the host)
 *   room code       derived from the beacon's roomId (gdx_uds_room_code): nobody types it, and
 *                   the joiner knows it from the scan without a second exchange
 *   beacon          the GXBN appdata (port/gdx_mp_beacon.h) with proto = GDX_UDS_MPO_PROTO, so
 *                   an mp-online room and an mp-uds spike room ([net] mp=host) never match, and
 *                   their UDS passphrases differ too (NWM refuses the association)
 *
 * NEARBY lists both media in one table: gdx_nearby_rows merges the udp-lan rows and the UDS scan
 * into GdxMpNearbyRoom rows (medium + the source row's index), joinable first, then by name. */
#ifndef GDX_MP_UDS_BRIDGE_H
#define GDX_MP_UDS_BRIDGE_H

#include "gdx_mp_beacon.h"
#include "gdx_mp_lan.h"
#include "gdx_mp_online.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GDX_UDS_NET_A 169
#define GDX_UDS_NET_B 254
#define GDX_UDS_NET_C 213
#define GDX_UDS_FAKE_PORT 54741
#define GDX_UDS_MAX_NODE 16
#define GDX_UDS_MPO_PROTO (0x80 | GDX_MPO_PROTO)

/* A medium-specific room as the UDS scan reports it (mirrors GdxMpUdsRoom's fields this glue
 * reads; the 3DS side copies them in, so this header needs no 3DS type). */
typedef struct GdxMpUdsSeen {
    GdxMpBeacon beacon; /* decoded + validated by gdx_mp_beacon_decode */
    int signalBars;     /* 0..3, -1 = unknown */
} GdxMpUdsSeen;

/* Node <-> endpoint. gdx_uds_endpoint_node returns 1..16, or 0 for any other endpoint. */
void gdx_uds_node_endpoint(int node, GdxMpEndpoint* out);
int gdx_uds_endpoint_node(const GdxMpEndpoint* e);

/* The room code of a UDS room (GDX_MPO_CODE_LEN letters of gdx_mpo_code_alphabet + NUL). */
void gdx_uds_room_code(uint32_t roomId, char* out);

/* GDX_MPO_MODEL_* <-> the beacon's CFG_SystemModel. */
int gdx_uds_model_to_beacon(int mpoModel);
int gdx_uds_model_from_beacon(int beaconModel);

/* The beacon an mp-online UDS host advertises. */
void gdx_uds_make_beacon(GdxMpBeacon* out, const char* name, int mpoModel, int players, int maxPlayers, int racing,
                         const char* build, uint32_t contentHash, uint32_t roomId);

/* One scanned UDS room as a NEARBY row (compatibility judged against our build + content). */
void gdx_uds_nearby_row(const GdxMpUdsSeen* seen, int srcIndex, const char* build, uint32_t contentHash,
                        GdxMpNearbyRoom* out);

/* The NEARBY list: every used row of `lan` (may be NULL) and every entry of `uds`, sorted
 * joinable first, then by name, then WiFi before UDS, then by host id. Returns the count (<= cap). */
int gdx_nearby_rows(const GdxMpNearby* lan, const GdxMpUdsSeen* uds, int nUds, const char* build,
                    uint32_t contentHash, GdxMpNearbyRoom* out, int cap);

#ifdef __cplusplus
}
#endif

#endif /* GDX_MP_UDS_BRIDGE_H */
