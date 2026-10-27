/* port/gdx_mp_uds_bridge.c — mp-online over UDS: the pure glue. Contract: gdx_mp_uds_bridge.h. */
#include "gdx_mp_uds_bridge.h"

#include <stdio.h>
#include <string.h>

void gdx_uds_node_endpoint(int node, GdxMpEndpoint* out) {
    out->ip[0] = GDX_UDS_NET_A;
    out->ip[1] = GDX_UDS_NET_B;
    out->ip[2] = GDX_UDS_NET_C;
    out->ip[3] = (uint8_t) node;
    out->port = GDX_UDS_FAKE_PORT;
}

int gdx_uds_endpoint_node(const GdxMpEndpoint* e) {
    if (e == NULL || e->ip[0] != GDX_UDS_NET_A || e->ip[1] != GDX_UDS_NET_B || e->ip[2] != GDX_UDS_NET_C ||
        e->port != GDX_UDS_FAKE_PORT) {
        return 0;
    }
    if (e->ip[3] < 1 || e->ip[3] > GDX_UDS_MAX_NODE) {
        return 0;
    }
    return e->ip[3];
}

static uint32_t Mix(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

void gdx_uds_room_code(uint32_t roomId, char* out) {
    uint32_t r = roomId ^ 0x55D5C0DEu;
    int i;
    for (i = 0; i < GDX_MPO_CODE_LEN; i++) {
        r = Mix(r + 0x9E3779B9u);
        out[i] = gdx_mpo_code_alphabet[r % 32u];
    }
    out[GDX_MPO_CODE_LEN] = '\0';
}

int gdx_uds_model_to_beacon(int mpoModel) {
    if (mpoModel == GDX_MPO_MODEL_N3DS) {
        return GDX_MP_MODEL_N3DS;
    }
    if (mpoModel == GDX_MPO_MODEL_O3DS) {
        return GDX_MP_MODEL_3DS;
    }
    return GDX_MP_MODEL_UNKNOWN;
}

int gdx_uds_model_from_beacon(int beaconModel) {
    if (beaconModel == GDX_MP_MODEL_UNKNOWN || beaconModel < 0 || beaconModel > GDX_MP_MODEL_N2DSXL) {
        return GDX_MPO_MODEL_UNKNOWN;
    }
    return gdx_mp_model_is_new(beaconModel) ? GDX_MPO_MODEL_N3DS : GDX_MPO_MODEL_O3DS;
}

void gdx_uds_make_beacon(GdxMpBeacon* out, const char* name, int mpoModel, int players, int maxPlayers, int racing,
                         const char* build, uint32_t contentHash, uint32_t roomId) {
    memset(out, 0, sizeof(*out));
    if (maxPlayers < 2) {
        maxPlayers = 2;
    } else if (maxPlayers > GDX_MPO_MAX_SLOTS) {
        maxPlayers = GDX_MPO_MAX_SLOTS;
    }
    if (players < 1) {
        players = 1;
    } else if (players > maxPlayers) {
        players = maxPlayers;
    }
    out->version = GDX_MP_BEACON_VERSION;
    out->flags = (uint8_t) (racing ? GDX_MP_BEACON_F_RACING : GDX_MP_BEACON_F_OPEN);
    out->players = (uint8_t) players;
    out->maxPlayers = (uint8_t) maxPlayers;
    out->model = (uint8_t) gdx_uds_model_to_beacon(mpoModel);
    out->proto = GDX_UDS_MPO_PROTO;
    out->buildHash = gdx_mp_beacon_build_hash(build);
    out->contentHash = contentHash;
    out->roomId = roomId;
    snprintf(out->name, sizeof(out->name), "%s", name != NULL ? name : "");
    snprintf(out->build, sizeof(out->build), "%s", build != NULL ? build : "");
}

void gdx_uds_nearby_row(const GdxMpUdsSeen* seen, int srcIndex, const char* build, uint32_t contentHash,
                        GdxMpNearbyRoom* out) {
    const GdxMpBeacon* b = &seen->beacon;
    const int compat = gdx_mp_beacon_compat(b, gdx_mp_beacon_build_hash(build), contentHash, GDX_UDS_MPO_PROTO);
    char ascii[GDX_MP_BEACON_NAME_BYTES];
    memset(out, 0, sizeof(*out));
    out->used = 1;
    out->medium = GDX_NEARBY_MEDIUM_UDS;
    out->srcIndex = srcIndex;
    out->b.hostId = b->roomId;
    out->b.contentHash = b->contentHash;
    out->b.players = b->players;
    out->b.maxPlayers = b->maxPlayers;
    out->b.model = (uint8_t) gdx_uds_model_from_beacon(b->model);
    out->b.flags = (uint8_t) (((b->flags & GDX_MP_BEACON_F_RACING) ? GDX_LAN_FLAG_RACING : 0) |
                              (b->players >= b->maxPlayers ? GDX_LAN_FLAG_FULL : 0));
    out->b.proto = b->proto;
    gdx_uds_room_code(b->roomId, out->b.code);
    /* The name is drawn in the console font: keep the lobby's own name alphabet. */
    gdx_mp_utf8_sanitise(b->name, ascii, (int) sizeof(ascii));
    gdx_mpo_name_sanitise(ascii, out->b.name);
    snprintf(out->b.build, sizeof(out->b.build), "%s", b->build);
    gdx_uds_node_endpoint(1, &out->room);
    out->beaconFrom = out->room;
    out->pingMs = -1;
    out->signal = (seen->signalBars >= 0 && seen->signalBars <= 3) ? seen->signalBars : -1;
    out->compatible = compat == GDX_MP_COMPAT_OK || compat == GDX_MP_COMPAT_FULL || compat == GDX_MP_COMPAT_RACING;
    out->joinable = compat == GDX_MP_COMPAT_OK;
    switch (compat) {
        case GDX_MP_COMPAT_OK:
            out->whyNot = "";
            break;
        case GDX_MP_COMPAT_CONTENT:
            out->whyNot = "other ROM";
            break;
        case GDX_MP_COMPAT_RACING:
            out->whyNot = "racing";
            break;
        case GDX_MP_COMPAT_FULL:
            out->whyNot = "full";
            break;
        default:
            out->whyNot = "other version";
            break;
    }
}

static int RowFirst(const GdxMpNearbyRoom* a, const GdxMpNearbyRoom* b) {
    int cmp;
    if (a->joinable != b->joinable) {
        return a->joinable > b->joinable;
    }
    cmp = strcmp(a->b.name, b->b.name);
    if (cmp != 0) {
        return cmp < 0;
    }
    if (a->medium != b->medium) {
        return a->medium < b->medium;
    }
    return a->b.hostId < b->b.hostId;
}

int gdx_nearby_rows(const GdxMpNearby* lan, const GdxMpUdsSeen* uds, int nUds, const char* build,
                    uint32_t contentHash, GdxMpNearbyRoom* out, int cap) {
    int n = 0;
    int i;
    int j;
    if (out == NULL || cap <= 0) {
        return 0;
    }
    if (lan != NULL) {
        for (i = 0; i < GDX_LAN_MAX_NEARBY && n < cap; i++) {
            if (lan->room[i].used) {
                out[n] = lan->room[i];
                out[n].medium = GDX_NEARBY_MEDIUM_WIFI;
                out[n].srcIndex = i;
                n++;
            }
        }
    }
    for (i = 0; uds != NULL && i < nUds && n < cap; i++) {
        gdx_uds_nearby_row(&uds[i], i, build, contentHash, &out[n]);
        n++;
    }
    for (i = 1; i < n; i++) { /* insertion sort: stable, n <= 16 */
        const GdxMpNearbyRoom v = out[i];
        for (j = i - 1; j >= 0 && RowFirst(&v, &out[j]); j--) {
            out[j + 1] = out[j];
        }
        out[j + 1] = v;
    }
    return n;
}
