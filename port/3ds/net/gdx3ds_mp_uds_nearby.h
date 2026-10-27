/* port/3ds/net/gdx3ds_mp_uds_nearby.h — NEARBY over UDS local wireless (mp-uds).
 *
 * The UDS backend's own view of the rooms its radio scan hears: decoded GXBN beacons
 * (port/gdx_mp_beacon.h, carried in the NWM beacon's appdata) plus what only the radio knows
 * (signal, the host's MAC to connect to).
 *
 * MERGED (mp-online). The controller (gdx3ds_mp_online.c) reads list() every frame while the
 * NEARBY list is up and merges these rows with the same-WiFi ones (gdx_nearby_rows,
 * port/gdx_mp_uds_bridge.h): one list, a WiFi / LOCAL tag per row. A UDS room then runs the same
 * console-hosted room, lobby, lockstep and removal as a WiFi one, its datagrams carried as UDS
 * frames (gdx3ds_mp_uds_raw_send / _recv).
 *
 * The row the lobby draws: gdx_mp_beacon_label(&room.beacon, room.signalBars, ...) =
 * "<name>'s room n/4 N3DS ▂▄▆"; room.compat != GDX_MP_COMPAT_OK = greyed out, with
 * gdx_mp_compat_text(room.compat) as the reason.
 *
 * Every call is non-blocking and main-thread; a backend scans on its own worker. */
#ifndef GDX3DS_MP_UDS_NEARBY_H
#define GDX3DS_MP_UDS_NEARBY_H

#include "gdx_mp_beacon.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    GDX_MP_UDS_MEDIUM_UDS = 1,
    GDX_MP_UDS_MEDIUM_LAN = 2,
};

#define GDX_MP_UDS_NEARBY_MAX 8

typedef struct GdxMpUdsRoom {
    int medium;          /* GDX_MP_UDS_MEDIUM_* */
    GdxMpBeacon beacon;  /* decoded, validated, strings safe to draw */
    int compat;          /* GDX_MP_COMPAT_*: OK = joinable, else greyed out */
    int signalBars;      /* 0..3, -1 = the medium cannot tell */
    uint32_t lastSeenMs;
    uint8_t handle[8];   /* medium-specific (UDS: the host's MAC address, 6 bytes) */
} GdxMpUdsRoom;

typedef struct GdxMpUdsNearbyOps {
    const char* name;
    /* Start scanning (our build + content decide compat). 0 = scanning; < 0 = an error, with a
     * user-facing sentence in why (e.g. the wireless switch is off). */
    int (*start)(const char* build, uint32_t contentHash, char* why, int cap);
    /* A snapshot of the rooms seen in the last GDX_UDS_ROOM_STALE_MS, newest first. */
    int (*list)(GdxMpUdsRoom* out, int cap);
    void (*stop)(void);
    /* Join a listed room (non-blocking). 0 = connecting; poll join_state. */
    int (*join)(const GdxMpUdsRoom* room);
    int (*join_state)(char* why, int cap); /* GDX_UDS_JOIN_* */
    /* Host a room / refresh what it advertises (players, flags). */
    int (*host)(const GdxMpBeacon* b);
    int (*host_update)(const GdxMpBeacon* b);
} GdxMpUdsNearbyOps;

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_MP_UDS_NEARBY_H */
