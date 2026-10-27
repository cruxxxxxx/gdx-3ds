/* port/gdx_mp_lan.c — mp-online NEARBY: the console-hosted LAN room and the GXLN beacon.
 * Contract: gdx_mp_lan.h. Pure: no socket, no clock, no global state. */
#include "gdx_mp_lan.h"

#include <stdio.h>
#include <string.h>

const GdxMpEndpoint GDX_LAN_SELF = { { 0, 0, 0, 1 }, 1 };

static const uint8_t kLnMagic[4] = { 'G', 'X', 'L', 'N' };

/* ---- helpers ------------------------------------------------------------------------------- */
static void PutU16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t) (v & 0xFF);
    p[1] = (uint8_t) (v >> 8);
}

static void PutU32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t) (v & 0xFF);
    p[1] = (uint8_t) ((v >> 8) & 0xFF);
    p[2] = (uint8_t) ((v >> 16) & 0xFF);
    p[3] = (uint8_t) (v >> 24);
}

static uint16_t GetU16(const uint8_t* p) {
    return (uint16_t) (p[0] | (p[1] << 8));
}

static uint32_t GetU32(const uint8_t* p) {
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

static void PutFixed(uint8_t* p, const char* s, int n) {
    int i;
    memset(p, 0, (size_t) n);
    for (i = 0; s != NULL && i < n && s[i] != '\0'; i++) {
        p[i] = (uint8_t) s[i];
    }
}

static int IsNameChar(int c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ' ' || c == '_' ||
           c == '.' || c == '-';
}

static int IsCodeChar(int c) {
    return c != 0 && strchr(gdx_mpo_code_alphabet, c) != NULL;
}

/* kind 0 printable, 1 name chars, 2 code chars; -1 when malformed. */
static int GetFixed(const uint8_t* p, int n, char* out, int kind) {
    int len = n;
    int i;
    for (i = 0; i < n; i++) {
        if (p[i] == 0) {
            len = i;
            break;
        }
    }
    for (i = len; i < n; i++) {
        if (p[i] != 0) {
            return -1;
        }
    }
    for (i = 0; i < len; i++) {
        const int c = p[i];
        if ((kind == 0 && (c < 0x21 || c > 0x7E)) || (kind == 1 && !IsNameChar(c)) || (kind == 2 && !IsCodeChar(c))) {
            return -1;
        }
        out[i] = (char) c;
    }
    out[len] = '\0';
    return len;
}

static uint64_t NextKey(GdxMpLanRoom* r) {
    uint64_t x = r->rng;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    r->rng = x;
    return x != 0 ? x : 1;
}

static int KeyEq(uint64_t a, uint64_t b) {
    uint64_t d = a ^ b; /* constant time */
    d |= d >> 32;
    d |= d >> 16;
    d |= d >> 8;
    return (d & 0xFF) == 0;
}

/* ---- 1. the LAN room ------------------------------------------------------------------------ */
int gdx_lan_room_open(GdxMpLanRoom* r, const char* code, const char* build, uint32_t contentHash, int maxPlayers,
                      uint64_t entropy, uint32_t nowMs) {
    char c[GDX_MPO_CODE_LEN + 1];
    memset(r, 0, sizeof(*r));
    if (code == NULL || build == NULL || build[0] == '\0' || maxPlayers < 2 || maxPlayers > GDX_MPO_MAX_SLOTS) {
        return -1;
    }
    snprintf(c, sizeof(c), "%s", code);
    if (!gdx_mpo_code_normalise(c)) {
        return -1;
    }
    memcpy(r->code, c, sizeof(r->code));
    snprintf(r->build, sizeof(r->build), "%s", build);
    r->contentHash = contentHash;
    r->maxPlayers = maxPlayers;
    r->rng = entropy ^ 0x9E3779B97F4A7C15ull;
    if (r->rng == 0) {
        r->rng = 1;
    }
    r->secret = NextKey(r);
    r->rosterVersion = 1;
    r->memberTimeoutMs = GDX_LAN_MEMBER_TIMEOUT_MS;
    gdx_mpo_bucket_init(&r->requests, 20, 40);
    gdx_mpo_bucket_init(&r->failedJoins, 1, 10);
    (void) nowMs;
    r->open = 1;
    return 0;
}

void gdx_lan_room_touch(GdxMpLanRoom* r, int slot, uint32_t nowMs) {
    if (r->open && slot > 0 && slot < GDX_MPO_MAX_SLOTS && r->member[slot].present) {
        r->member[slot].lastSeenMs = nowMs;
    }
}

void gdx_lan_room_set_timeout(GdxMpLanRoom* r, uint32_t memberTimeoutMs) {
    r->memberTimeoutMs = memberTimeoutMs < 3000u ? 3000u : memberTimeoutMs;
}

int gdx_lan_room_count(const GdxMpLanRoom* r) {
    int s;
    int n = 0;
    for (s = 0; s < GDX_MPO_MAX_SLOTS; s++) {
        n += r->member[s].present ? 1 : 0;
    }
    return n;
}

static void Emit(GdxMpLanEmit emit, void* ctx, const GdxMpEndpoint* to, const uint8_t* pkt, int len) {
    if (emit != NULL && len > 0) {
        emit(ctx, to, pkt, len);
    }
}

static void SendError(GdxMpLanEmit emit, void* ctx, const GdxMpEndpoint* to, uint32_t nonce, int code,
                      const char* detail) {
    uint8_t pkt[56];
    Emit(emit, ctx, to, pkt, gdx_rv_build_error(nonce, code, detail, pkt, sizeof(pkt)));
}

static void SendWelcome(const GdxMpLanRoom* r, int slot, GdxMpLanEmit emit, void* ctx) {
    GdxMpRvWelcome w;
    uint8_t pkt[GDX_RV_WELCOME_BYTES];
    const GdxMpLanMember* m = &r->member[slot];
    memset(&w, 0, sizeof(w));
    w.nonce = m->nonce;
    memcpy(w.code, r->code, sizeof(w.code));
    w.slot = (uint8_t) slot;
    w.maxPlayers = (uint8_t) r->maxPlayers;
    w.memberKey = m->key;
    w.roomSecret = r->secret;
    w.seenAs = m->addr;
    Emit(emit, ctx, &m->addr, pkt, gdx_rv_build_welcome(&w, pkt, sizeof(pkt)));
}

static int BuildRoster(const GdxMpLanRoom* r, uint8_t* pkt, int cap) {
    GdxMpRvRoster ro;
    int s;
    memset(&ro, 0, sizeof(ro));
    memcpy(ro.code, r->code, sizeof(ro.code));
    ro.version = r->rosterVersion;
    ro.locked = (uint8_t) r->locked;
    for (s = 0; s < GDX_MPO_MAX_SLOTS; s++) {
        const GdxMpLanMember* m = &r->member[s];
        if (!m->present) {
            continue;
        }
        ro.member[s].present = 1;
        ro.member[s].epoch = m->epoch;
        ro.member[s].pub = m->addr;
        ro.member[s].priv = m->priv;
        memcpy(ro.member[s].name, m->name, sizeof(ro.member[s].name));
    }
    return gdx_rv_build_roster(&ro, pkt, cap);
}

/* The roster to every member except `skip` (-1 = everyone). */
static void BroadcastRoster(const GdxMpLanRoom* r, int skip, GdxMpLanEmit emit, void* ctx) {
    uint8_t pkt[GDX_RV_ROSTER_MIN + GDX_MPO_MAX_SLOTS * GDX_RV_ROSTER_ENTRY];
    const int n = BuildRoster(r, pkt, sizeof(pkt));
    int s;
    for (s = 0; s < GDX_MPO_MAX_SLOTS; s++) {
        if (r->member[s].present && s != skip) {
            Emit(emit, ctx, &r->member[s].addr, pkt, n);
        }
    }
}

static void Seat(GdxMpLanRoom* r, int slot, const GdxMpRvRequest* req, const GdxMpEndpoint* from, uint32_t nowMs) {
    GdxMpLanMember* m = &r->member[slot];
    memset(m, 0, sizeof(*m));
    m->present = 1;
    m->addr = *from;
    m->priv = req->priv;
    memcpy(m->name, req->name, sizeof(m->name));
    m->key = NextKey(r);
    m->nonce = req->nonce;
    m->lastSeenMs = nowMs;
    r->joins = (uint16_t) (r->joins + 1);
    m->epoch = r->joins;
    gdx_mpo_bucket_init(&m->ctl, 20, 40);
    gdx_mpo_bucket_init(&m->relay, 400, 600);
    r->rosterVersion++;
}

static void OnCreate(GdxMpLanRoom* r, const GdxMpRvRequest* req, const GdxMpEndpoint* from, uint32_t nowMs,
                     GdxMpLanEmit emit, void* ctx) {
    GdxMpLanMember* host = &r->member[0];
    if (!gdx_mpo_endpoint_eq(from, &GDX_LAN_SELF)) {
        SendError(emit, ctx, from, req->nonce, GDX_RV_E_BAD_REQUEST, "lan room"); /* only our own client */
        return;
    }
    if (host->present && host->nonce == req->nonce) {
        SendWelcome(r, 0, emit, ctx); /* a retransmit */
        return;
    }
    if (strcmp(req->code, r->code) != 0 || strcmp(req->build, r->build) != 0 || req->contentHash != r->contentHash) {
        SendError(emit, ctx, from, req->nonce, GDX_RV_E_BAD_REQUEST, "lan room");
        return;
    }
    Seat(r, 0, req, from, nowMs);
    SendWelcome(r, 0, emit, ctx);
    BroadcastRoster(r, 0, emit, ctx);
}

static void OnJoin(GdxMpLanRoom* r, const GdxMpRvRequest* req, const GdxMpEndpoint* from, uint32_t nowMs,
                   GdxMpLanEmit emit, void* ctx) {
    int s;
    int slot = -1;
    char hex[12];
    for (s = 1; s < GDX_MPO_MAX_SLOTS; s++) {
        const GdxMpLanMember* m = &r->member[s];
        if (m->present && gdx_mpo_endpoint_eq(&m->addr, from) && m->nonce == req->nonce) {
            SendWelcome(r, s, emit, ctx); /* a retransmit */
            return;
        }
    }
    if (strcmp(req->code, r->code) != 0) {
        if (!gdx_mpo_bucket_take(&r->failedJoins, nowMs)) {
            r->rateLimited++;
            SendError(emit, ctx, from, req->nonce, GDX_RV_E_RATE_LIMITED, "");
            return;
        }
        SendError(emit, ctx, from, req->nonce, GDX_RV_E_ROOM_NOT_FOUND, "");
        return;
    }
    if (!r->member[0].present) {
        SendError(emit, ctx, from, req->nonce, GDX_RV_E_ROOM_NOT_FOUND, "");
        return;
    }
    if (strcmp(req->build, r->build) != 0) {
        SendError(emit, ctx, from, req->nonce, GDX_RV_E_BUILD_MISMATCH, r->build);
        return;
    }
    if (req->contentHash != r->contentHash) {
        snprintf(hex, sizeof(hex), "%08lx", (unsigned long) r->contentHash);
        SendError(emit, ctx, from, req->nonce, GDX_RV_E_CONTENT_MISMATCH, hex);
        return;
    }
    if (r->locked) {
        SendError(emit, ctx, from, req->nonce, GDX_RV_E_ROOM_LOCKED, "");
        return;
    }
    /* A console that restarted takes its old slot back (same address, silent for a while). */
    for (s = 1; s < GDX_MPO_MAX_SLOTS && slot < 0; s++) {
        const GdxMpLanMember* m = &r->member[s];
        if (m->present && gdx_mpo_endpoint_eq(&m->addr, from) &&
            (int32_t) (nowMs - m->lastSeenMs) > GDX_LAN_REJOIN_SILENT_MS) {
            slot = s;
        }
    }
    for (s = 1; s < r->maxPlayers && slot < 0; s++) {
        if (!r->member[s].present) {
            slot = s;
        }
    }
    if (slot < 0) {
        SendError(emit, ctx, from, req->nonce, GDX_RV_E_ROOM_FULL, "");
        return;
    }
    Seat(r, slot, req, from, nowMs);
    SendWelcome(r, slot, emit, ctx);
    BroadcastRoster(r, slot, emit, ctx); /* the newcomer gets it with its first keepalive */
}

static void Remove(GdxMpLanRoom* r, int slot, GdxMpLanEmit emit, void* ctx) {
    if (slot == 0) {
        gdx_lan_room_close(r, emit, ctx);
        return;
    }
    memset(&r->member[slot], 0, sizeof(r->member[slot]));
    r->rosterVersion++;
    BroadcastRoster(r, -1, emit, ctx);
}

static void OnRelay(GdxMpLanRoom* r, GdxMpLanMember* m, int slot, const uint8_t* pkt, int len, int mask, uint32_t nowMs,
                    GdxMpLanEmit emit, void* ctx) {
    const uint8_t* payload = pkt + GDX_RV_AUTH_BYTES;
    const int n = len - GDX_RV_AUTH_BYTES;
    int s;
    if (n <= 0 || n > GDX_RV_MAX_RELAY_PAYLOAD) {
        return;
    }
    if (n >= 4 && memcmp(payload, "GXMP", 4) == 0) {
        if (n < GDX_MP_HEADER_BYTES || payload[34] != slot + 1) {
            return;
        }
    } else if (n >= 4 && memcmp(payload, "GXLB", 4) == 0) {
        if (n < GDX_LB_HEADER_BYTES || payload[6] != slot) {
            return;
        }
    } else {
        return;
    }
    if (!gdx_mpo_bucket_take(&m->relay, nowMs)) {
        r->rateLimited++;
        return;
    }
    for (s = 0; s < GDX_MPO_MAX_SLOTS; s++) {
        if (s != slot && ((mask >> s) & 1) && r->member[s].present) {
            Emit(emit, ctx, &r->member[s].addr, payload, n);
            r->relayed++;
        }
    }
}

static void OnAuthed(GdxMpLanRoom* r, const GdxMpRvAuth* a, const uint8_t* pkt, int len, const GdxMpEndpoint* from,
                     uint32_t nowMs, GdxMpLanEmit emit, void* ctx) {
    GdxMpLanMember* m = &r->member[a->slot];
    uint8_t out[GDX_RV_ROSTER_MIN + GDX_MPO_MAX_SLOTS * GDX_RV_ROSTER_ENTRY];
    if (!m->present || strcmp(a->code, r->code) != 0 || !KeyEq(m->key, a->memberKey)) {
        r->junk++;
        if (a->type == GDX_RV_KEEPALIVE && gdx_mpo_bucket_take(&r->requests, nowMs)) {
            const int n = gdx_rv_build_error(0, GDX_RV_E_AUTH, "", out, sizeof(out));
            Emit(emit, ctx, from, out, n > 16 ? 16 : n); /* the short form: never bigger than the ask */
        }
        return;
    }
    m->lastSeenMs = nowMs;
    if (a->type == GDX_RV_RELAY) {
        OnRelay(r, m, a->slot, pkt, len, a->arg, nowMs, emit, ctx);
        return;
    }
    if (!gdx_mpo_bucket_take(&m->ctl, nowMs)) {
        r->rateLimited++;
        return;
    }
    if (a->type == GDX_RV_KEEPALIVE) {
        int moved = 0;
        if (a->arg != 0) {
            return;
        }
        if (!gdx_mpo_endpoint_eq(&m->addr, from)) {
            m->addr = *from; /* an authenticated member moved (DHCP renew, a socket reopen) */
            r->rosterVersion++;
            moved = 1;
        }
        Emit(emit, ctx, &m->addr, out, gdx_rv_build_ka_ack(r->rosterVersion, a->sendMs, out, sizeof(out)));
        if (moved) {
            BroadcastRoster(r, -1, emit, ctx);
        } else if (a->rosterVersion != r->rosterVersion) {
            Emit(emit, ctx, &m->addr, out, BuildRoster(r, out, sizeof(out)));
        }
    } else if (a->type == GDX_RV_LEAVE) {
        if (a->arg == 0) {
            Remove(r, a->slot, emit, ctx);
        }
    } else if (a->type == GDX_RV_PUNCH_REQ) {
        if (a->arg < GDX_MPO_MAX_SLOTS && a->arg != a->slot && r->member[a->arg].present) {
            Emit(emit, ctx, &r->member[a->arg].addr, out, gdx_rv_build_punch_now(r->code, a->slot, out, sizeof(out)));
        }
    } else if (a->type == GDX_RV_LOCK) {
        const int want = a->arg != 0;
        if (a->slot == 0 && want != r->locked) {
            r->locked = want;
            r->rosterVersion++;
            BroadcastRoster(r, -1, emit, ctx);
        }
    }
}

int gdx_lan_room_handle(GdxMpLanRoom* r, const uint8_t* pkt, int len, const GdxMpEndpoint* from, uint32_t nowMs,
                        GdxMpLanEmit emit, void* ctx) {
    GdxMpRvRequest req;
    GdxMpRvAuth auth;
    if (pkt == NULL || len < 4 || memcmp(pkt, "GXRV", 4) != 0) {
        return 0;
    }
    if (!r->open || len > 1300 || len < 8 || pkt[5] >= 0x80) {
        r->junk++;
        return 1;
    }
    r->rx++;
    if (pkt[5] == GDX_RV_CREATE || pkt[5] == GDX_RV_JOIN) {
        int t;
        if (!gdx_mpo_endpoint_eq(from, &GDX_LAN_SELF) && !gdx_mpo_bucket_take(&r->requests, nowMs)) {
            r->rateLimited++;
            return 1;
        }
        t = gdx_rv_parse_request(pkt, len, &req);
        if (t == GDX_RV_CREATE) {
            OnCreate(r, &req, from, nowMs, emit, ctx);
        } else if (t == GDX_RV_JOIN) {
            OnJoin(r, &req, from, nowMs, emit, ctx);
        } else {
            r->junk++;
        }
        return 1;
    }
    if (gdx_rv_parse_auth(pkt, len, &auth) < 0) {
        r->junk++;
        return 1;
    }
    OnAuthed(r, &auth, pkt, len, from, nowMs, emit, ctx);
    return 1;
}

void gdx_lan_room_close(GdxMpLanRoom* r, GdxMpLanEmit emit, void* ctx) {
    int s;
    if (!r->open) {
        return;
    }
    for (s = 1; s < GDX_MPO_MAX_SLOTS; s++) {
        if (r->member[s].present) {
            SendError(emit, ctx, &r->member[s].addr, 0, GDX_RV_E_HOST_LEFT, "host closed the room");
        }
    }
    memset(r->member, 0, sizeof(r->member));
    r->open = 0;
}

void gdx_lan_room_expire(GdxMpLanRoom* r, uint32_t nowMs, GdxMpLanEmit emit, void* ctx) {
    int s;
    if (!r->open) {
        return;
    }
    for (s = 1; s < GDX_MPO_MAX_SLOTS; s++) {
        if (r->member[s].present && nowMs - r->member[s].lastSeenMs > r->memberTimeoutMs) {
            Remove(r, s, emit, ctx);
        }
    }
}

/* ---- 2. the beacon ------------------------------------------------------------------------- */
static void PutLnHeader(uint8_t* out, int type) {
    memcpy(out, kLnMagic, 4);
    out[4] = GDX_LAN_VERSION;
    out[5] = (uint8_t) type;
    out[6] = 0;
    out[7] = 0;
}

int gdx_lan_build_beacon(const GdxMpLanBeacon* b, uint8_t* out, int cap) {
    if (cap < GDX_LAN_BEACON_BYTES) {
        return 0;
    }
    memset(out, 0, GDX_LAN_BEACON_BYTES);
    PutLnHeader(out, GDX_LAN_BEACON);
    PutU32(out + 8, b->hostId);
    PutU32(out + 12, b->contentHash);
    PutU16(out + 16, b->roomPort);
    out[18] = b->players;
    out[19] = b->maxPlayers;
    out[20] = b->model;
    out[21] = b->flags;
    out[22] = b->proto;
    PutFixed(out + 24, b->code, 8);
    PutFixed(out + 32, b->name, GDX_MPO_NAME_LEN);
    PutFixed(out + 44, b->build, GDX_MPO_BUILD_LEN);
    return GDX_LAN_BEACON_BYTES;
}

int gdx_lan_build_ping(int type, uint32_t hostId, uint32_t nonce, uint32_t sendMs, uint8_t* out, int cap) {
    if (cap < GDX_LAN_PING_BYTES || (type != GDX_LAN_PING && type != GDX_LAN_PONG)) {
        return 0;
    }
    PutLnHeader(out, type);
    PutU32(out + 8, hostId);
    PutU32(out + 12, nonce);
    PutU32(out + 16, sendMs);
    return GDX_LAN_PING_BYTES;
}

int gdx_lan_parse(const uint8_t* pkt, int len, GdxMpLanBeacon* b, uint32_t* hostId, uint32_t* nonce, uint32_t* sendMs) {
    int t;
    if (pkt == NULL || len < 8 || memcmp(pkt, kLnMagic, 4) != 0 || pkt[4] != GDX_LAN_VERSION || pkt[6] != 0 ||
        pkt[7] != 0) {
        return -1;
    }
    t = pkt[5];
    if (t == GDX_LAN_BEACON) {
        if (len != GDX_LAN_BEACON_BYTES || pkt[23] != 0) {
            return -1;
        }
        memset(b, 0, sizeof(*b));
        b->hostId = GetU32(pkt + 8);
        b->contentHash = GetU32(pkt + 12);
        b->roomPort = GetU16(pkt + 16);
        b->players = pkt[18];
        b->maxPlayers = pkt[19];
        b->model = pkt[20];
        b->flags = pkt[21];
        b->proto = pkt[22];
        if (b->hostId == 0 || b->roomPort == 0 || b->maxPlayers < 2 || b->maxPlayers > GDX_MPO_MAX_SLOTS ||
            b->players < 1 || b->players > b->maxPlayers || b->model > GDX_MPO_MODEL_N3DS) {
            return -1;
        }
        if (GetFixed(pkt + 24, 8, b->code, 2) != GDX_MPO_CODE_LEN || GetFixed(pkt + 32, GDX_MPO_NAME_LEN, b->name, 1) < 0 ||
            GetFixed(pkt + 44, GDX_MPO_BUILD_LEN, b->build, 0) <= 0) {
            return -1;
        }
        return t;
    }
    if (t == GDX_LAN_PING || t == GDX_LAN_PONG) {
        if (len != GDX_LAN_PING_BYTES) {
            return -1;
        }
        *hostId = GetU32(pkt + 8);
        *nonce = GetU32(pkt + 12);
        *sendMs = GetU32(pkt + 16);
        return t;
    }
    return -1;
}

/* ---- the nearby table ---------------------------------------------------------------------- */
void gdx_nearby_init(GdxMpNearby* n, const char* build, uint32_t contentHash) {
    memset(n, 0, sizeof(*n));
    snprintf(n->build, sizeof(n->build), "%s", build != NULL ? build : "");
    n->contentHash = contentHash;
    n->pingNonce = 0x51ED27u;
}

int gdx_nearby_signal_from_ping(int pingMs) {
    if (pingMs < 0) {
        return 0;
    }
    if (pingMs <= 30) {
        return 3;
    }
    if (pingMs <= 90) {
        return 2;
    }
    return 1;
}

static void Judge(const GdxMpNearby* n, GdxMpNearbyRoom* row) {
    row->compatible = row->b.proto == GDX_MPO_PROTO && strcmp(row->b.build, n->build) == 0 &&
                      row->b.contentHash == n->contentHash;
    row->joinable = 0;
    if (row->b.proto != GDX_MPO_PROTO || strcmp(row->b.build, n->build) != 0) {
        row->whyNot = "other version";
    } else if (row->b.contentHash != n->contentHash) {
        row->whyNot = "other ROM";
    } else if (row->b.flags & GDX_LAN_FLAG_RACING) {
        row->whyNot = "racing";
    } else if ((row->b.flags & GDX_LAN_FLAG_FULL) || row->b.players >= row->b.maxPlayers) {
        row->whyNot = "full";
    } else {
        row->whyNot = "";
        row->joinable = 1;
    }
}

void gdx_nearby_on_beacon(GdxMpNearby* n, const GdxMpLanBeacon* b, const GdxMpEndpoint* src, uint32_t nowMs) {
    GdxMpNearbyRoom* row = NULL;
    int i;
    if (b->hostId == n->selfHostId && n->selfHostId != 0) {
        return;
    }
    n->heard++;
    for (i = 0; i < GDX_LAN_MAX_NEARBY && row == NULL; i++) {
        GdxMpNearbyRoom* r = &n->room[i];
        if (r->used && r->b.hostId == b->hostId && memcmp(r->beaconFrom.ip, src->ip, 4) == 0) {
            row = r;
        }
    }
    if (row == NULL) {
        for (i = 0; i < GDX_LAN_MAX_NEARBY && row == NULL; i++) {
            if (!n->room[i].used) {
                row = &n->room[i];
            }
        }
        if (row == NULL) {
            row = &n->room[0];
            for (i = 1; i < GDX_LAN_MAX_NEARBY; i++) {
                if ((int32_t) (n->room[i].lastSeenMs - row->lastSeenMs) < 0) {
                    row = &n->room[i];
                }
            }
        }
        memset(row, 0, sizeof(*row));
        row->used = 1;
        row->firstSeenMs = nowMs;
        row->pingMs = -1;
        row->lastPingMs = nowMs - GDX_LAN_PING_MS;
    }
    row->b = *b;
    row->beaconFrom = *src;
    memcpy(row->room.ip, src->ip, 4);
    row->room.port = b->roomPort;
    row->lastSeenMs = nowMs;
    Judge(n, row);
}

void gdx_nearby_on_pong(GdxMpNearby* n, uint32_t hostId, uint32_t nonce, uint32_t sendMs, uint32_t nowMs) {
    int i;
    for (i = 0; i < GDX_LAN_MAX_NEARBY; i++) {
        GdxMpNearbyRoom* r = &n->room[i];
        if (r->used && r->b.hostId == hostId && r->pingNonce != 0 && r->pingNonce == nonce) {
            const uint32_t rtt = nowMs - sendMs;
            if (rtt < 10000u) {
                r->pingMs = (r->pingMs < 0) ? (int) rtt : (int) ((3u * (uint32_t) r->pingMs + rtt) / 4u);
                r->signal = gdx_nearby_signal_from_ping(r->pingMs);
            }
            r->pingNonce = 0;
        }
    }
}

int gdx_nearby_tick(GdxMpNearby* n, uint32_t nowMs) {
    int i;
    int due = -1;
    for (i = 0; i < GDX_LAN_MAX_NEARBY; i++) {
        GdxMpNearbyRoom* r = &n->room[i];
        if (!r->used) {
            continue;
        }
        if ((int32_t) (nowMs - r->lastSeenMs) > GDX_LAN_EXPIRE_MS) {
            memset(r, 0, sizeof(*r));
            continue;
        }
        if (due < 0 && (int32_t) (nowMs - r->lastPingMs) >= GDX_LAN_PING_MS) {
            due = i;
        }
    }
    if (due >= 0) {
        GdxMpNearbyRoom* r = &n->room[due];
        n->pingNonce = n->pingNonce * 1664525u + 1013904223u;
        if (n->pingNonce == 0) {
            n->pingNonce = 1;
        }
        r->pingNonce = n->pingNonce;
        r->lastPingMs = nowMs;
    }
    return due;
}

int gdx_nearby_sorted(const GdxMpNearby* n, int* idx, int max) {
    int count = 0;
    int i;
    int j;
    for (i = 0; i < GDX_LAN_MAX_NEARBY && count < max; i++) {
        if (n->room[i].used) {
            idx[count++] = i;
        }
    }
    for (i = 1; i < count; i++) { /* insertion sort: joinable first, then by name, then by id */
        const int v = idx[i];
        const GdxMpNearbyRoom* a = &n->room[v];
        for (j = i - 1; j >= 0; j--) {
            const GdxMpNearbyRoom* b = &n->room[idx[j]];
            const int cmp = strcmp(a->b.name, b->b.name);
            const int aFirst = (a->joinable > b->joinable) ||
                               (a->joinable == b->joinable && (cmp < 0 || (cmp == 0 && a->b.hostId < b->b.hostId)));
            if (!aFirst) {
                break;
            }
            idx[j + 1] = idx[j];
        }
        idx[j + 1] = v;
    }
    return count;
}
