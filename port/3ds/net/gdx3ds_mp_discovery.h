/* port/3ds/net/gdx3ds_mp_discovery.h — mp-online NEARBY: finding rooms on the same network.
 *
 * THE INTERFACE every local-discovery backend implements (the lobby UI and the controller speak
 * only this):
 *
 *   backend   announces / hears via                       joins via                  status
 *   udp-lan   GXLN beacon, UDP broadcast + unicast list    the host's game socket    this file
 *             (port/gdx_mp_lan.h), ~1 Hz, ping per row     (GXRV to a LAN room)
 *   uds       NWM beacon appdata (the radio's own scan)    a UDS connect, then GXRV  gdx3ds_mp_uds.c
 *                                                          to node 1's room
 * The udp-lan backend is behind these ops; the UDS scan keeps its own ops (gdx3ds_mp_uds_nearby.h)
 * and the controller merges both into one list with gdx_nearby_rows (port/gdx_mp_uds_bridge.h).
 *
 * A backend fills the shared table (GdxMpNearby / GdxMpNearbyRoom, port/gdx_mp_lan.h): one row
 * per room heard, expiring when it goes quiet, with `compatible` / `joinable` judged against our
 * build + content hash, `signal` 0..3 and `pingMs`. What a row's join needs is the backend's
 * business (`room` endpoint + code for udp-lan; a UDS backend keeps its own per-row handle and
 * may reuse `room`/`hostId` as it likes).
 *
 * Called from the main thread only. Never blocks. All input is untrusted: a beacon is display
 * data, joining still goes through the room's own checks. */
#ifndef GDX3DS_MP_DISCOVERY_H
#define GDX3DS_MP_DISCOVERY_H

#include "gdx_mp_lan.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GdxMpDiscoveryConfig {
    const char* build;
    uint32_t contentHash;
    /* udp-lan: extra unicast targets for the beacon (dotted IPv4, comma separated; "" = none).
     * The emulator lane uses 127.0.0.1; on hardware broadcast does the job. */
    const char* unicastPeers;
    int broadcast; /* udp-lan: 1 = also broadcast (the default; the emulator lane turns it off) */
} GdxMpDiscoveryConfig;

typedef struct GdxMpDiscoveryOps {
    const char* name;
    /* 0 on success (listening); negative = backend error (the NEARBY tab says so). */
    int (*open)(const GdxMpDiscoveryConfig* cfg);
    void (*close)(void);
    /* Receive, expire, ping, and (when announcing) beacon. Every frame. */
    void (*service)(uint32_t nowMs);
    /* Start / refresh announcing our room (`b` is copied; call again when it changes). NULL = stop. */
    void (*announce)(const GdxMpLanBeacon* b);
    /* The rooms heard (never our own). Valid until the next service(). */
    const GdxMpNearby* (*table)(void);
    /* Diagnostics for the receipt. */
    int (*describe)(char* buf, int cap);
} GdxMpDiscoveryOps;

/* udp-lan (gdx3ds_mp_discovery.c). SOC must already be initialised. */
const GdxMpDiscoveryOps* gdx3ds_mp_discovery_udp_ops(void);

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_MP_DISCOVERY_H */
