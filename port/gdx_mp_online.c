/* port/gdx_mp_online.c — online multiplayer pure core. Contract + wire formats: gdx_mp_online.h,
 * tools/mp-rendezvous/README.md. */
#include "gdx_mp_online.h"

#include <stdio.h>
#include <string.h>

static const uint8_t kRvMagic[4] = { 'G', 'X', 'R', 'V' };
static const uint8_t kLbMagic[4] = { 'G', 'X', 'L', 'B' };

const char gdx_mpo_code_alphabet[33] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";

/* ---- little-endian helpers ----------------------------------------------------------------- */
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

static void PutU64(uint8_t* p, uint64_t v) {
    PutU32(p, (uint32_t) (v & 0xFFFFFFFFu));
    PutU32(p + 4, (uint32_t) (v >> 32));
}

static uint16_t GetU16(const uint8_t* p) {
    return (uint16_t) (p[0] | (p[1] << 8));
}

static uint32_t GetU32(const uint8_t* p) {
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

static uint64_t GetU64(const uint8_t* p) {
    return (uint64_t) GetU32(p) | ((uint64_t) GetU32(p + 4) << 32);
}

static uint32_t Fnv(uint32_t h, const void* data, size_t len) {
    const uint8_t* p = (const uint8_t*) data;
    size_t i;
    for (i = 0; i < len; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

/* Fixed-width NUL-padded field. Writes at most n bytes, always pads. */
static void PutFixed(uint8_t* p, const char* s, int n) {
    int i = 0;
    memset(p, 0, (size_t) n);
    if (s == NULL) {
        return;
    }
    for (i = 0; i < n && s[i] != '\0'; i++) {
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

/* Fixed field -> C string. kind 0 = printable ASCII (0x21..0x7E), 1 = name chars, 2 = code chars,
 * 3 = display text (0x20..0x7E).
 * Returns the length, or -1 when malformed (a non-NUL byte after the first NUL, or a bad byte). */
static int GetFixed(const uint8_t* p, int n, char* out, int kind) {
    int i;
    int len = n;
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
        if (kind == 0 && (c < 0x21 || c > 0x7E)) {
            return -1;
        }
        if (kind == 3 && (c < 0x20 || c > 0x7E)) {
            return -1;
        }
        if (kind == 1 && !IsNameChar(c)) {
            return -1;
        }
        if (kind == 2 && !IsCodeChar(c)) {
            return -1;
        }
        out[i] = (char) c;
    }
    out[len] = '\0';
    return len;
}

static void PutHeader(uint8_t* out, int type) {
    memcpy(out, kRvMagic, 4);
    out[4] = GDX_RV_VERSION;
    out[5] = (uint8_t) type;
    out[6] = 0;
    out[7] = 0;
}

/* ---- GXRV builders -------------------------------------------------------------------------- */
int gdx_rv_build_request(int type, const GdxMpRvRequest* req, uint8_t* out, int cap) {
    if ((type != GDX_RV_CREATE && type != GDX_RV_JOIN) || req == NULL || cap < GDX_RV_REQ_BYTES) {
        return 0;
    }
    PutHeader(out, type);
    PutU32(out + 8, req->nonce);
    PutU32(out + 12, req->contentHash);
    memcpy(out + 16, req->priv.ip, 4);
    PutU16(out + 20, req->priv.port);
    out[22] = req->maxPlayers;
    out[23] = (uint8_t) (req->forceRelay ? 1 : 0);
    PutFixed(out + 24, req->build, GDX_MPO_BUILD_LEN);
    PutFixed(out + 64, req->name, GDX_MPO_NAME_LEN);
    PutFixed(out + 76, req->code, 8);
    return GDX_RV_REQ_BYTES;
}

int gdx_rv_build_request_v2(int type, const GdxMpRvRequest* req, const uint8_t cookie[GDX_RV_COOKIE_LEN],
                            uint8_t* out, int cap) {
    if (cap < GDX_RV_REQ2_BYTES || cookie == NULL || gdx_rv_build_request(type, req, out, cap) != GDX_RV_REQ_BYTES) {
        return 0;
    }
    out[4] = GDX_RV_VERSION_COOKIE;
    memcpy(out + GDX_RV_REQ_BYTES, cookie, GDX_RV_COOKIE_LEN);
    PutU16(out + GDX_RV_REQ_BYTES + GDX_RV_COOKIE_LEN, req->clientProto);
    PutU16(out + GDX_RV_REQ_BYTES + GDX_RV_COOKIE_LEN + 2, 0);
    return GDX_RV_REQ2_BYTES;
}

int gdx_rv_build_auth(int type, const char* code, int slot, int arg, uint64_t memberKey, uint8_t* out, int cap) {
    if (cap < GDX_RV_AUTH_BYTES || slot < 0 || slot >= GDX_MPO_MAX_SLOTS || code == NULL) {
        return 0;
    }
    PutHeader(out, type);
    PutFixed(out + 8, code, 8);
    out[16] = (uint8_t) slot;
    out[17] = (uint8_t) arg;
    PutU16(out + 18, 0);
    PutU64(out + 20, memberKey);
    return GDX_RV_AUTH_BYTES;
}

int gdx_rv_build_keepalive(const char* code, int slot, uint64_t memberKey, uint32_t rosterVersion, uint32_t sendMs,
                           uint8_t* out, int cap) {
    if (cap < GDX_RV_KEEPALIVE_BYTES || gdx_rv_build_auth(GDX_RV_KEEPALIVE, code, slot, 0, memberKey, out, cap) == 0) {
        return 0;
    }
    PutU32(out + 28, rosterVersion);
    PutU32(out + 32, sendMs);
    return GDX_RV_KEEPALIVE_BYTES;
}

int gdx_rv_build_relay(const char* code, int slot, int dstMask, uint64_t memberKey, const uint8_t* payload,
                       int len, uint8_t* out, int cap) {
    if (payload == NULL || len <= 0 || len > GDX_RV_MAX_RELAY_PAYLOAD || cap < GDX_RV_AUTH_BYTES + len ||
        (dstMask & ~((1 << GDX_MPO_MAX_SLOTS) - 1)) != 0 || dstMask == 0) {
        return 0;
    }
    if (gdx_rv_build_auth(GDX_RV_RELAY, code, slot, dstMask, memberKey, out, cap) == 0) {
        return 0;
    }
    memcpy(out + GDX_RV_AUTH_BYTES, payload, (size_t) len);
    return GDX_RV_AUTH_BYTES + len;
}

/* ---- GXRV parsers --------------------------------------------------------------------------- */
int gdx_rv_peek_type(const uint8_t* pkt, int len) {
    if (pkt == NULL || len < 8 || memcmp(pkt, kRvMagic, 4) != 0 || pkt[5] < 0x80) {
        return -1;
    }
    if (pkt[4] != GDX_RV_VERSION && pkt[4] != GDX_RV_VERSION_COOKIE) {
        return -1;
    }
    return pkt[5];
}

int gdx_rv_parse_cookie(const uint8_t* pkt, int len, uint32_t* nonce, uint8_t cookie[GDX_RV_COOKIE_LEN]) {
    if (gdx_rv_peek_type(pkt, len) != GDX_RV_COOKIE || len != GDX_RV_COOKIE_BYTES || pkt[4] != GDX_RV_VERSION_COOKIE) {
        return -1;
    }
    *nonce = GetU32(pkt + 8);
    memcpy(cookie, pkt + 12, GDX_RV_COOKIE_LEN);
    return 0;
}

int gdx_rv_parse_update(const uint8_t* pkt, int len, GdxMpRvUpdate* out) {
    if (gdx_rv_peek_type(pkt, len) != GDX_RV_UPDATE || len != GDX_RV_UPDATE_BYTES || pkt[4] != GDX_RV_VERSION_COOKIE) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    out->nonce = GetU32(pkt + 8);
    out->minProto = GetU16(pkt + 12);
    out->maxProto = GetU16(pkt + 14);
    out->yourProto = GetU16(pkt + 16);
    if (GetFixed(pkt + 20, GDX_RV_UPDATE_TEXT_LEN, out->text, 3) < 0) {
        out->text[0] = '\0'; /* display text: dropped when malformed, never trusted */
    }
    return 0;
}

int gdx_rv_parse_welcome(const uint8_t* pkt, int len, GdxMpRvWelcome* out) {
    if (gdx_rv_peek_type(pkt, len) != GDX_RV_WELCOME || len != GDX_RV_WELCOME_BYTES) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    out->nonce = GetU32(pkt + 8);
    if (GetFixed(pkt + 12, 8, out->code, 2) != GDX_MPO_CODE_LEN) {
        return -1;
    }
    out->slot = pkt[20];
    out->maxPlayers = pkt[21];
    out->forceRelay = (uint8_t) (pkt[22] & 1);
    out->memberKey = GetU64(pkt + 24);
    out->roomSecret = GetU64(pkt + 32);
    memcpy(out->seenAs.ip, pkt + 40, 4);
    out->seenAs.port = GetU16(pkt + 44);
    if (out->slot >= GDX_MPO_MAX_SLOTS || out->maxPlayers < 2 || out->maxPlayers > GDX_MPO_MAX_SLOTS ||
        out->slot >= out->maxPlayers || out->roomSecret == 0) {
        return -1;
    }
    return 0;
}

int gdx_rv_parse_roster(const uint8_t* pkt, int len, GdxMpRvRoster* out) {
    int count;
    int i;
    if (gdx_rv_peek_type(pkt, len) != GDX_RV_ROSTER || len < GDX_RV_ROSTER_MIN) {
        return -1;
    }
    count = pkt[20];
    if (count < 1 || count > GDX_MPO_MAX_SLOTS || len != GDX_RV_ROSTER_MIN + count * GDX_RV_ROSTER_ENTRY) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    if (GetFixed(pkt + 8, 8, out->code, 2) != GDX_MPO_CODE_LEN) {
        return -1;
    }
    out->version = GetU32(pkt + 16);
    out->locked = (uint8_t) (pkt[21] & 1);
    out->forceRelay = (uint8_t) ((pkt[21] >> 1) & 1);
    out->count = count;
    for (i = 0; i < count; i++) {
        const uint8_t* e = pkt + GDX_RV_ROSTER_MIN + i * GDX_RV_ROSTER_ENTRY;
        const int slot = e[0];
        GdxMpRvMember* m;
        if (slot >= GDX_MPO_MAX_SLOTS || out->member[slot].present) {
            return -1;
        }
        m = &out->member[slot];
        m->present = 1;
        m->isHost = (uint8_t) (e[1] & 1);
        m->epoch = GetU16(e + 2);
        memcpy(m->pub.ip, e + 4, 4);
        m->pub.port = GetU16(e + 8);
        m->priv.port = GetU16(e + 10);
        memcpy(m->priv.ip, e + 12, 4);
        if (GetFixed(e + 16, GDX_MPO_NAME_LEN, m->name, 1) < 0) {
            return -1;
        }
    }
    if (!out->member[0].present) {
        return -1; /* a room without its host does not exist */
    }
    return 0;
}

int gdx_rv_parse_error(const uint8_t* pkt, int len, GdxMpRvError* out) {
    if (gdx_rv_peek_type(pkt, len) != GDX_RV_ERROR || (len != 16 && len != 56)) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    out->nonce = GetU32(pkt + 8);
    out->code = pkt[12];
    out->hostProto = GetU16(pkt + 14); /* 0 from every server before 2.1.0 */
    if (len == 56) {
        /* The detail is display text (a build id): printable or it is dropped, never trusted. */
        if (GetFixed(pkt + 16, GDX_MPO_BUILD_LEN, out->detail, 3) < 0) {
            out->detail[0] = '\0';
        }
    }
    return 0;
}

int gdx_rv_parse_ka_ack(const uint8_t* pkt, int len, uint32_t* rosterVersion, uint32_t* echoMs) {
    if (gdx_rv_peek_type(pkt, len) != GDX_RV_KA_ACK || len != 16) {
        return -1;
    }
    *rosterVersion = GetU32(pkt + 8);
    *echoMs = GetU32(pkt + 12);
    return 0;
}

int gdx_rv_parse_punch_now(const uint8_t* pkt, int len, int* fromSlot) {
    if (gdx_rv_peek_type(pkt, len) != GDX_RV_PUNCH_NOW || len != 20 || pkt[16] >= GDX_MPO_MAX_SLOTS) {
        return -1;
    }
    *fromSlot = pkt[16];
    return 0;
}

const char* gdx_rv_error_text(int code) {
    switch (code) {
        case GDX_RV_E_BAD_REQUEST: return "server refused the request";
        case GDX_RV_E_ROOM_NOT_FOUND: return "no room with that code";
        case GDX_RV_E_ROOM_FULL: return "room is full";
        case GDX_RV_E_BUILD_MISMATCH: return "version mismatch";
        case GDX_RV_E_CONTENT_MISMATCH: return "game data differs";
        case GDX_RV_E_RATE_LIMITED: return "too many tries, wait a minute";
        case GDX_RV_E_SERVER_FULL: return "server is full";
        case GDX_RV_E_ROOM_LOCKED: return "race in progress";
        case GDX_RV_E_AUTH: return "membership expired";
        case GDX_RV_E_HOST_LEFT: return "host closed the room";
        case GDX_RV_E_CODE_TAKEN: return "code already in use";
        default: return "server error";
    }
}

/* ---- version gating: what the player is told -------------------------------------------------- */
int gdx_rv_update_verdict(const GdxMpRvUpdate* u, int myProto) {
    if (myProto < (int) u->minProto) {
        return GDX_RV_UPD_CLIENT_OLD;
    } else if (u->maxProto != 0 && myProto > (int) u->maxProto) {
        return GDX_RV_UPD_SERVER_OLD;
    }
    return GDX_RV_UPD_REFUSED;
}

int gdx_rv_update_message(const GdxMpRvUpdate* u, int myProto, char* out, int cap) {
    const int verdict = gdx_rv_update_verdict(u, myProto);
    if (verdict == GDX_RV_UPD_CLIENT_OLD) {
        if (u->text[0] != '\0') {
            snprintf(out, (size_t) cap, "UPDATE REQUIRED - this server needs version %u (you have %d) - %s",
                     (unsigned) u->minProto, myProto, u->text);
        } else {
            snprintf(out, (size_t) cap, "UPDATE REQUIRED - this server needs version %u (you have %d)",
                     (unsigned) u->minProto, myProto);
        }
    } else if (verdict == GDX_RV_UPD_SERVER_OLD) {
        snprintf(out, (size_t) cap, "SERVER NEEDS AN UPDATE - it supports up to version %u (you have %d)",
                 (unsigned) u->maxProto, myProto);
    } else {
        snprintf(out, (size_t) cap, "UPDATE REQUIRED - this server refused version %d", myProto);
    }
    return verdict;
}

const char* gdx_mpo_update_side(int theirProto, int myProto) {
    if (theirProto > myProto) {
        return "update this console";
    } else if (theirProto < myProto) {
        return "they need an update";
    }
    return "same version, other build";
}

void gdx_rv_error_message(const GdxMpRvError* e, int myProto, char* out, int cap) {
    const int mismatch = e->code == GDX_RV_E_BUILD_MISMATCH || e->code == GDX_RV_E_CONTENT_MISMATCH;
    char side[48];
    side[0] = '\0';
    if (e->code == GDX_RV_E_BUILD_MISMATCH && strcmp(e->detail, GDX_RV_NEEDS_V2_DETAIL) == 0) {
        /* only a v1 request gets this: [net] mp_rv_cookie = 0 against a server that requires v2 */
        snprintf(out, (size_t) cap, "version mismatch: this server needs GXRV v2 (set [net] mp_rv_cookie = auto)");
        return;
    }
    if (mismatch && e->hostProto != 0) {
        const char* who = gdx_mpo_update_side(e->hostProto, myProto);
        snprintf(side, sizeof(side), " (%s)", strcmp(who, "they need an update") == 0 ? "the host needs an update" : who);
    }
    if (e->code == GDX_RV_E_BUILD_MISMATCH) {
        snprintf(out, (size_t) cap, "version mismatch: host runs %s%s", e->detail[0] != '\0' ? e->detail : "another build",
                 side);
    } else {
        snprintf(out, (size_t) cap, "%s%s", gdx_rv_error_text(e->code), side);
    }
}

/* ---- GXRV v2, the console's request state machine ------------------------------------------- */
int gdx_rv_cookie_mode_parse(const char* s) {
    if (s != NULL && (strcmp(s, "0") == 0 || strcmp(s, "v1") == 0)) {
        return GDX_RV_COOKIE_MODE_V1;
    }
    if (s != NULL && (strcmp(s, "1") == 0 || strcmp(s, "v2") == 0)) {
        return GDX_RV_COOKIE_MODE_V2;
    }
    return GDX_RV_COOKIE_MODE_AUTO;
}

const char* gdx_rv_cookie_mode_name(int mode) {
    if (mode == GDX_RV_COOKIE_MODE_V1) {
        return "0";
    } else if (mode == GDX_RV_COOKIE_MODE_V2) {
        return "1";
    }
    return "auto";
}

void gdx_rv_cookie_init(GdxRvCookie* c, int mode) {
    memset(c, 0, sizeof(*c));
    c->mode = (mode == GDX_RV_COOKIE_MODE_V1 || mode == GDX_RV_COOKIE_MODE_V2) ? mode : GDX_RV_COOKIE_MODE_AUTO;
}

static void ClearPendingCookie(GdxRvCookie* c) {
    c->haveCookie = 0;
    c->v2Unanswered = 0;
    c->fastResends = 0;
    c->cookies = 0;
    c->cookieRttMs = 0;
    memset(c->cookie, 0, sizeof(c->cookie));
}

void gdx_rv_cookie_begin(GdxRvCookie* c, uint32_t nonce, uint32_t nowMs, int allowV2) {
    ClearPendingCookie(c);
    c->nonce = nonce;
    c->firstSendMs = nowMs;
    c->lastSendMs = nowMs;
    if (!allowV2 || c->mode == GDX_RV_COOKIE_MODE_V1) {
        c->useV2 = 0;
    } else if (c->mode == GDX_RV_COOKIE_MODE_V2) {
        c->useV2 = 1;
    } else {
        c->useV2 = !c->fellBack;
    }
}

static int AutoShouldFallBack(const GdxRvCookie* c) {
    return c->mode == GDX_RV_COOKIE_MODE_AUTO && c->useV2 && !c->serverV2 && !c->haveCookie &&
           c->v2Unanswered >= GDX_RV_COOKIE_FALLBACK_TRIES;
}

int gdx_rv_cookie_build(GdxRvCookie* c, int type, const GdxMpRvRequest* req, uint32_t nowMs, uint8_t* out, int cap,
                        int* event) {
    int n;
    if (event != NULL) {
        *event = GDX_RV_CK_NONE;
    }
    if (AutoShouldFallBack(c)) {
        c->useV2 = 0;
        c->fellBack = 1;
        if (event != NULL) {
            *event = GDX_RV_CK_FALLBACK;
        }
    }
    c->lastSendMs = nowMs;
    if (!c->useV2) {
        return gdx_rv_build_request(type, req, out, cap);
    }
    n = gdx_rv_build_request_v2(type, req, c->cookie, out, cap);
    if (n > 0) {
        c->v2Unanswered++;
    }
    return n;
}

int gdx_rv_cookie_on_reply(GdxRvCookie* c, const uint8_t* pkt, int len, uint32_t nowMs) {
    uint32_t nonce = 0;
    uint8_t cookie[GDX_RV_COOKIE_LEN];
    if (gdx_rv_peek_type(pkt, len) < 0) {
        return GDX_RV_CK_NONE;
    }
    c->v2Unanswered = 0; /* the server is there: whatever it is, it is not ignoring us */
    if (pkt[4] == GDX_RV_VERSION_COOKIE) {
        c->serverV2 = 1;
    }
    if (!c->useV2 || gdx_rv_parse_cookie(pkt, len, &nonce, cookie) != 0 || nonce != c->nonce) {
        return GDX_RV_CK_NONE;
    }
    memcpy(c->cookie, cookie, sizeof(cookie));
    c->haveCookie = 1;
    c->cookies++;
    c->cookieRttMs = nowMs - c->lastSendMs;
    if (c->fastResends >= GDX_RV_COOKIE_MAX_FAST_RESENDS) {
        return GDX_RV_CK_NONE; /* stored; the regular retry carries it */
    }
    c->fastResends++;
    return GDX_RV_CK_RESEND;
}

int gdx_rv_cookie_on_error(GdxRvCookie* c, const GdxMpRvError* e) {
    if (c->mode != GDX_RV_COOKIE_MODE_AUTO || c->useV2 || !c->fellBack || e->code != GDX_RV_E_BUILD_MISMATCH ||
        strcmp(e->detail, GDX_RV_NEEDS_V2_DETAIL) != 0) {
        return GDX_RV_CK_NONE;
    }
    c->fellBack = 0;
    c->serverV2 = 1;
    c->useV2 = 1;
    ClearPendingCookie(c);
    return GDX_RV_CK_RETRY_V2;
}

/* ---- GXRV, the server's half ------------------------------------------------------------------ */
int gdx_mpo_is_private_v4(const uint8_t ip[4]) {
    return ip[0] == 10 || (ip[0] == 172 && ip[1] >= 16 && ip[1] <= 31) || (ip[0] == 192 && ip[1] == 168) ||
           (ip[0] == 169 && ip[1] == 254) || ip[0] == 127;
}

static int IsRvRequestHeader(const uint8_t* pkt, int len) {
    return pkt != NULL && len >= 8 && memcmp(pkt, kRvMagic, 4) == 0 && pkt[4] == GDX_RV_VERSION && pkt[6] == 0 &&
           pkt[7] == 0;
}

int gdx_rv_parse_request(const uint8_t* pkt, int len, GdxMpRvRequest* out) {
    int t;
    int codeLen;
    int i;
    int blank = 1;
    if (!IsRvRequestHeader(pkt, len) || len != GDX_RV_REQ_BYTES) {
        return -1;
    }
    t = pkt[5];
    if (t != GDX_RV_CREATE && t != GDX_RV_JOIN) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    out->nonce = GetU32(pkt + 8);
    out->contentHash = GetU32(pkt + 12);
    memcpy(out->priv.ip, pkt + 16, 4);
    out->priv.port = GetU16(pkt + 20);
    out->maxPlayers = pkt[22];
    out->forceRelay = (uint8_t) (pkt[23] & 1);
    if (GetFixed(pkt + 24, GDX_MPO_BUILD_LEN, out->build, 0) <= 0 || GetFixed(pkt + 64, GDX_MPO_NAME_LEN, out->name, 1) < 0) {
        return -1;
    }
    codeLen = GetFixed(pkt + 76, 8, out->code, 2);
    if (codeLen < 0 || (codeLen != 0 && codeLen != GDX_MPO_CODE_LEN)) {
        return -1;
    }
    for (i = 0; out->name[i] != '\0'; i++) {
        if (out->name[i] != ' ') {
            blank = 0;
        }
    }
    if (blank) {
        snprintf(out->name, sizeof(out->name), "PLAYER");
    }
    if (!gdx_mpo_is_private_v4(out->priv.ip)) {
        memset(out->priv.ip, 0, 4);
    }
    return t;
}

int gdx_rv_parse_auth(const uint8_t* pkt, int len, GdxMpRvAuth* out) {
    int t;
    if (!IsRvRequestHeader(pkt, len) || len < GDX_RV_AUTH_BYTES) {
        return -1;
    }
    t = pkt[5];
    if (t != GDX_RV_KEEPALIVE && t != GDX_RV_LEAVE && t != GDX_RV_RELAY && t != GDX_RV_PUNCH_REQ && t != GDX_RV_LOCK) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    if (GetFixed(pkt + 8, 8, out->code, 2) != GDX_MPO_CODE_LEN || GetU16(pkt + 18) != 0) {
        return -1;
    }
    if (t != GDX_RV_RELAY && len != (t == GDX_RV_KEEPALIVE ? GDX_RV_KEEPALIVE_BYTES : GDX_RV_AUTH_BYTES)) {
        return -1;
    }
    out->type = t;
    out->slot = pkt[16];
    out->arg = pkt[17];
    out->memberKey = GetU64(pkt + 20);
    if (t == GDX_RV_KEEPALIVE) {
        out->rosterVersion = GetU32(pkt + 28);
        out->sendMs = GetU32(pkt + 32);
    }
    if (out->slot >= GDX_MPO_MAX_SLOTS) {
        return -1;
    }
    return t;
}

int gdx_rv_build_welcome(const GdxMpRvWelcome* w, uint8_t* out, int cap) {
    if (cap < GDX_RV_WELCOME_BYTES) {
        return 0;
    }
    memset(out, 0, GDX_RV_WELCOME_BYTES);
    PutHeader(out, GDX_RV_WELCOME);
    PutU32(out + 8, w->nonce);
    PutFixed(out + 12, w->code, 8);
    out[20] = w->slot;
    out[21] = w->maxPlayers;
    out[22] = (uint8_t) (w->forceRelay ? 1 : 0);
    PutU64(out + 24, w->memberKey);
    PutU64(out + 32, w->roomSecret);
    memcpy(out + 40, w->seenAs.ip, 4);
    PutU16(out + 44, w->seenAs.port);
    return GDX_RV_WELCOME_BYTES;
}

int gdx_rv_build_roster(const GdxMpRvRoster* r, uint8_t* out, int cap) {
    int s;
    int count = 0;
    for (s = 0; s < GDX_MPO_MAX_SLOTS; s++) {
        count += r->member[s].present ? 1 : 0;
    }
    if (count < 1 || cap < GDX_RV_ROSTER_MIN + count * GDX_RV_ROSTER_ENTRY) {
        return 0;
    }
    memset(out, 0, (size_t) (GDX_RV_ROSTER_MIN + count * GDX_RV_ROSTER_ENTRY));
    PutHeader(out, GDX_RV_ROSTER);
    PutFixed(out + 8, r->code, 8);
    PutU32(out + 16, r->version);
    out[20] = (uint8_t) count;
    out[21] = (uint8_t) ((r->locked ? 1 : 0) | (r->forceRelay ? 2 : 0));
    count = 0;
    for (s = 0; s < GDX_MPO_MAX_SLOTS; s++) {
        const GdxMpRvMember* m = &r->member[s];
        uint8_t* e;
        if (!m->present) {
            continue;
        }
        e = out + GDX_RV_ROSTER_MIN + count * GDX_RV_ROSTER_ENTRY;
        e[0] = (uint8_t) s;
        e[1] = (uint8_t) (s == 0 ? 1 : 0);
        PutU16(e + 2, m->epoch);
        memcpy(e + 4, m->pub.ip, 4);
        PutU16(e + 8, m->pub.port);
        PutU16(e + 10, m->priv.port);
        memcpy(e + 12, m->priv.ip, 4);
        PutFixed(e + 16, m->name, GDX_MPO_NAME_LEN);
        count++;
    }
    return GDX_RV_ROSTER_MIN + count * GDX_RV_ROSTER_ENTRY;
}

int gdx_rv_build_error(uint32_t nonce, int code, const char* detail, uint8_t* out, int cap) {
    if (cap < 56) {
        return 0;
    }
    memset(out, 0, 56);
    PutHeader(out, GDX_RV_ERROR);
    PutU32(out + 8, nonce);
    out[12] = (uint8_t) code;
    PutFixed(out + 16, detail, GDX_MPO_BUILD_LEN);
    return 56;
}

int gdx_rv_build_ka_ack(uint32_t rosterVersion, uint32_t echoMs, uint8_t* out, int cap) {
    if (cap < 16) {
        return 0;
    }
    PutHeader(out, GDX_RV_KA_ACK);
    PutU32(out + 8, rosterVersion);
    PutU32(out + 12, echoMs);
    return 16;
}

int gdx_rv_build_punch_now(const char* code, int fromSlot, uint8_t* out, int cap) {
    if (cap < 20 || fromSlot < 0 || fromSlot >= GDX_MPO_MAX_SLOTS) {
        return 0;
    }
    memset(out, 0, 20);
    PutHeader(out, GDX_RV_PUNCH_NOW);
    PutFixed(out + 8, code, 8);
    out[16] = (uint8_t) fromSlot;
    return 20;
}

/* ---- codes, names, addresses ---------------------------------------------------------------- */
int gdx_mpo_code_normalise(char* code) {
    int i;
    if (code == NULL) {
        return 0;
    }
    for (i = 0; code[i] != '\0'; i++) {
        if (code[i] >= 'a' && code[i] <= 'z') {
            code[i] = (char) (code[i] - 'a' + 'A');
        }
        if (i >= GDX_MPO_CODE_LEN || !IsCodeChar(code[i])) {
            return 0;
        }
    }
    return i == GDX_MPO_CODE_LEN;
}

void gdx_mpo_name_sanitise(const char* in, char* out) {
    int i;
    int n = 0;
    int nonSpace = 0;
    for (i = 0; in != NULL && in[i] != '\0' && n < GDX_MPO_NAME_LEN; i++) {
        const int c = (unsigned char) in[i];
        out[n++] = IsNameChar(c) ? (char) c : '_';
        if (c != ' ') {
            nonSpace = 1;
        }
    }
    out[n] = '\0';
    if (!nonSpace) {
        strcpy(out, "PLAYER");
    }
}

int gdx_mpo_parse_hostport(const char* s, GdxMpEndpoint* out, uint16_t defaultPort) {
    unsigned v[5] = { 0, 0, 0, 0, 0 };
    int part = 0;
    int digits = 0;
    int hasPort = 0;
    unsigned port;
    const char* p;
    int i;
    if (s == NULL || out == NULL) {
        return 0;
    }
    memset(out, 0, sizeof(*out));
    for (p = s;; p++) {
        if (*p >= '0' && *p <= '9') {
            if (++digits > 5) {
                return 0;
            }
            v[part] = v[part] * 10u + (unsigned) (*p - '0');
            continue;
        }
        if (digits == 0) {
            return 0;
        }
        if (part < 3) {
            if (*p != '.') {
                return 0;
            }
        } else if (part == 3) {
            if (*p == ':') {
                hasPort = 1;
            } else if (*p != '\0') {
                return 0;
            }
        } else if (*p != '\0') {
            return 0;
        }
        if (*p == '\0') {
            break;
        }
        part++;
        digits = 0;
    }
    for (i = 0; i < 4; i++) {
        if (v[i] > 255u) {
            return 0;
        }
        out->ip[i] = (uint8_t) v[i];
    }
    port = hasPort ? v[4] : defaultPort;
    if (port == 0 || port > 65535u) {
        memset(out, 0, sizeof(*out));
        return 0;
    }
    out->port = (uint16_t) port;
    return 1;
}

int gdx_mpo_endpoint_eq(const GdxMpEndpoint* a, const GdxMpEndpoint* b) {
    return memcmp(a->ip, b->ip, 4) == 0 && a->port == b->port;
}

int gdx_mpo_endpoint_valid(const GdxMpEndpoint* e) {
    return e->port != 0 && (e->ip[0] | e->ip[1] | e->ip[2] | e->ip[3]) != 0 && e->ip[0] != 255 &&
           (e->ip[0] & 0xF0) != 0xE0; /* not broadcast-ish, not multicast */
}

/* ---- GXLB ----------------------------------------------------------------------------------- */
static int LbBodyBytes(int type) {
    switch (type) {
        case GDX_LB_HELLO: return GDX_LB_HELLO_BYTES;
        case GDX_LB_START: return GDX_LB_START_BYTES;
        case GDX_LB_ABORT: return GDX_LB_ABORT_BYTES;
        case GDX_LB_RM_POLL:
        case GDX_LB_RM_HAVE:
        case GDX_LB_RM_SET: return GDX_LB_RM_BYTES;
        default: return 0;
    }
}

int gdx_lb_build(const GdxMpLbHeader* h, const void* body, uint8_t* out, int cap) {
    const int total = LbBodyBytes(h->type);
    int i;
    if (total == 0 || cap < total || body == NULL || h->from >= GDX_MPO_MAX_SLOTS) {
        return 0;
    }
    memset(out, 0, (size_t) total);
    memcpy(out, kLbMagic, 4);
    out[4] = GDX_LB_VERSION;
    out[5] = h->type;
    out[6] = h->from;
    out[7] = h->to;
    PutU64(out + 8, h->secret);
    PutU32(out + 16, h->sendMs);
    PutU32(out + 20, h->echoMs);
    PutU16(out + 24, h->holdMs);
    if (h->type == GDX_LB_HELLO) {
        const GdxMpLbHello* b = (const GdxMpLbHello*) body;
        PutU16(out + 28, b->proto);
        out[30] = b->state;
        out[31] = b->ready;
        PutU32(out + 32, b->contentHash);
        PutU32(out + 36, b->startGen);
        out[40] = b->machine;
        out[41] = b->skin;
        out[42] = b->engine;
        out[43] = b->bot;
        out[44] = b->directMask;
        out[45] = b->removedMask;
        out[46] = b->model;
        for (i = 0; i < GDX_MPO_MAX_SLOTS; i++) {
            PutU16(out + 48 + 2 * i, b->rttMs[i]);
        }
        PutFixed(out + 56, b->build, GDX_MPO_BUILD_LEN);
        PutFixed(out + 96, b->name, GDX_MPO_NAME_LEN);
        PutU32(out + 108, b->simAbi);
        out[112] = b->picking;
        out[113] = b->pickRound;
        out[114] = b->courseFor;
        out[115] = b->picked;
        PutU32(out + 116, b->courseKey);
        PutU32(out + 120, b->engineBits);
        PutU32(out + 124, b->pickMsLeft);
    } else if (h->type == GDX_LB_START) {
        const GdxMpLbStart* b = (const GdxMpLbStart*) body;
        PutU32(out + 28, b->gen);
        out[32] = b->numPlayers;
        out[33] = b->course;
        out[34] = b->delay;
        out[35] = b->hashEvery;
        out[36] = b->vsCom;
        out[37] = b->handicap;
        out[38] = b->vsSlot;
        out[39] = b->difficulty;
        PutU32(out + 40, b->seed1);
        PutU32(out + 44, b->mask1);
        PutU32(out + 48, b->seed2);
        PutU32(out + 52, b->mask2);
        PutU64(out + 56, b->token);
        for (i = 0; i < GDX_MPO_MAX_SLOTS; i++) {
            out[64 + 4 * i] = b->player[i].slot;
            out[65 + 4 * i] = b->player[i].machine;
            out[66 + 4 * i] = b->player[i].skin;
            out[67 + 4 * i] = b->player[i].engine;
        }
        out[80] = b->botMask;
        out[81] = b->laps;
        out[82] = b->racers;
        out[83] = b->ramMask;
        PutU32(out + 84, b->simAbi);
        PutU32(out + 88, b->courseKey);
        for (i = 0; i < GDX_MPO_MAX_SLOTS; i++) {
            PutU32(out + 92 + 4 * i, b->player[i].engineBits);
            PutU16(out + 108 + 2 * i, b->player[i].epoch);
        }
    } else if (h->type == GDX_LB_RM_POLL || h->type == GDX_LB_RM_HAVE || h->type == GDX_LB_RM_SET) {
        const GdxMpLbRemove* b = (const GdxMpLbRemove*) body;
        if (b->count > GDX_LB_RM_PADS) {
            return 0;
        }
        PutU32(out + 28, b->gen);
        out[32] = b->player;
        out[33] = b->reason;
        out[34] = b->count;
        PutU32(out + 36, b->tick);
        PutU32(out + 40, b->first);
        for (i = 0; i < b->count; i++) {
            PutU16(out + 44 + 4 * i, b->pads[i].buttons);
            out[46 + 4 * i] = (uint8_t) b->pads[i].stickX;
            out[47 + 4 * i] = (uint8_t) b->pads[i].stickY;
        }
    } else {
        const GdxMpLbAbort* b = (const GdxMpLbAbort*) body;
        PutU32(out + 28, b->gen);
        out[32] = b->reason;
        out[33] = b->culprit;
        PutU32(out + 36, b->tick);
    }
    return total;
}

int gdx_mpo_course_raceable(int course) {
    if (course >= 0 && course < GDX_MPO_STOCK_COURSES) {
        return 1;
    }
    /* TODO(mp-edit-courses): the EDIT cup (24..29) is each console's own CRSD file; racing one needs
     * the host to send its course data to every peer first. Until then it is refused here (START,
     * the lobby) and in the host's stock course picker (gdx3ds_mpp_course_refused). */
    return course >= GDX_MPO_FIRST_DD_COURSE && course <= GDX_MPO_MAX_COURSE;
}

uint32_t gdx_mpo_stock_course_key(int course) {
    if (course < 0 || course >= GDX_MPO_STOCK_COURSES) {
        return GDX_MPO_NO_COURSE_KEY;
    }
    return 0x53544B00u | (uint32_t) course; /* "STK" + the index */
}

typedef union EngineWord {
    float f;
    uint32_t u;
} EngineWord;

uint32_t gdx_mpo_engine_bits(float engine) {
    EngineWord w;
    w.f = engine;
    return gdx_mpo_engine_bits_valid(w.u) ? w.u : 0x3F000000u; /* 0.5f */
}

int gdx_mpo_engine_bits_valid(uint32_t bits) {
    EngineWord w;
    w.u = bits;
    return w.f >= 0.0f && w.f <= 1.0f; /* false for every NaN */
}

float gdx_mpo_engine_from_bits(uint32_t bits) {
    EngineWord w;
    w.u = gdx_mpo_engine_bits_valid(bits) ? bits : 0x3F000000u;
    return w.f;
}

static int ValidStart(const GdxMpLbStart* s) {
    int i;
    int j;
    if (s->numPlayers < 2 || s->numPlayers > GDX_MPO_MAX_SLOTS || !gdx_mpo_course_raceable(s->course) ||
        s->delay < 1 || s->delay > GDX_MPO_MAX_DELAY || s->hashEvery < 1 || s->hashEvery > 60 || s->vsCom > 1 ||
        s->handicap > 2 || s->vsSlot > 1 || s->difficulty > 3 || s->token == 0 ||
        (s->botMask >> s->numPlayers) != 0 || (s->ramMask & ~s->botMask) != 0 ||
        s->courseKey == GDX_MPO_NO_COURSE_KEY) {
        return 0;
    }
    if (s->racers != 0 && (!s->vsCom || s->racers < GDX_MPO_MIN_RACERS || s->racers > GDX_MPO_MAX_RACERS ||
                           s->racers < s->numPlayers)) {
        return 0; /* MP30: a machine count only with VS COM, never below the humans */
    }
    for (i = 0; i < s->numPlayers; i++) {
        const GdxMpStartPlayer* p = &s->player[i];
        if (p->slot >= GDX_MPO_MAX_SLOTS || p->machine > GDX_MPO_MAX_MACHINE || p->skin > 3 || p->engine > 100 ||
            !gdx_mpo_engine_bits_valid(p->engineBits)) {
            return 0;
        }
        for (j = 0; j < i; j++) {
            if (s->player[j].slot >= p->slot) {
                return 0; /* strictly ascending slots: player index order is slot order */
            }
        }
    }
    return s->player[0].slot == 0; /* the host (slot 0) is always player 0 */
}

int gdx_lb_parse(const uint8_t* pkt, int len, uint64_t secret, GdxMpLbHeader* h, GdxMpLbBody* body) {
    int type;
    int i;
    if (pkt == NULL || len < GDX_LB_HEADER_BYTES || memcmp(pkt, kLbMagic, 4) != 0 || pkt[4] != GDX_LB_VERSION) {
        return -1;
    }
    type = pkt[5];
    if (LbBodyBytes(type) == 0 || len != LbBodyBytes(type) || pkt[6] >= GDX_MPO_MAX_SLOTS) {
        return -1;
    }
    if (GetU64(pkt + 8) != secret || secret == 0) {
        return -2;
    }
    memset(h, 0, sizeof(*h));
    memset(body, 0, sizeof(*body));
    h->type = (uint8_t) type;
    h->from = pkt[6];
    h->to = pkt[7];
    h->secret = secret;
    h->sendMs = GetU32(pkt + 16);
    h->echoMs = GetU32(pkt + 20);
    h->holdMs = GetU16(pkt + 24);
    if (type == GDX_LB_HELLO) {
        GdxMpLbHello* b = &body->hello;
        b->proto = GetU16(pkt + 28);
        b->state = pkt[30];
        b->ready = pkt[31];
        b->contentHash = GetU32(pkt + 32);
        b->startGen = GetU32(pkt + 36);
        b->machine = pkt[40];
        b->skin = pkt[41];
        b->engine = pkt[42];
        b->bot = pkt[43];
        b->directMask = pkt[44];
        b->removedMask = pkt[45];
        b->model = pkt[46];
        for (i = 0; i < GDX_MPO_MAX_SLOTS; i++) {
            b->rttMs[i] = GetU16(pkt + 48 + 2 * i);
        }
        if (GetFixed(pkt + 56, GDX_MPO_BUILD_LEN, b->build, 0) < 0 ||
            GetFixed(pkt + 96, GDX_MPO_NAME_LEN, b->name, 1) < 0) {
            return -3;
        }
        b->simAbi = GetU32(pkt + 108);
        b->picking = pkt[112];
        b->pickRound = pkt[113];
        b->courseFor = pkt[114];
        b->picked = pkt[115];
        b->courseKey = GetU32(pkt + 116);
        b->engineBits = GetU32(pkt + 120);
        b->pickMsLeft = GetU32(pkt + 124);
        if (!gdx_mpo_engine_bits_valid(b->engineBits)) {
            return -3;
        }
        if (b->state > GDX_LB_STATE_RACING || b->ready > 1 || b->machine > GDX_MPO_MAX_MACHINE || b->skin > 3 ||
            b->engine > 100 || b->bot > 2 || b->model > GDX_MPO_MODEL_N3DS || b->picking > GDX_LB_PICK_COURSE ||
            b->courseFor > GDX_MPO_MAX_COURSE || b->picked > 1 ||
            (b->pickMsLeft > GDX_MPO_PICK_MS && b->pickMsLeft != GDX_LB_NO_ROUND)) {
            return -3;
        }
    } else if (type == GDX_LB_START) {
        GdxMpLbStart* b = &body->start;
        b->gen = GetU32(pkt + 28);
        b->numPlayers = pkt[32];
        b->course = pkt[33];
        b->delay = pkt[34];
        b->hashEvery = pkt[35];
        b->vsCom = pkt[36];
        b->handicap = pkt[37];
        b->vsSlot = pkt[38];
        b->difficulty = pkt[39];
        b->seed1 = GetU32(pkt + 40);
        b->mask1 = GetU32(pkt + 44);
        b->seed2 = GetU32(pkt + 48);
        b->mask2 = GetU32(pkt + 52);
        b->token = GetU64(pkt + 56);
        for (i = 0; i < GDX_MPO_MAX_SLOTS; i++) {
            b->player[i].slot = pkt[64 + 4 * i];
            b->player[i].machine = pkt[65 + 4 * i];
            b->player[i].skin = pkt[66 + 4 * i];
            b->player[i].engine = pkt[67 + 4 * i];
        }
        b->botMask = pkt[80];
        b->laps = pkt[81];
        b->racers = pkt[82];
        b->ramMask = pkt[83];
        b->simAbi = GetU32(pkt + 84);
        b->courseKey = GetU32(pkt + 88);
        for (i = 0; i < GDX_MPO_MAX_SLOTS; i++) {
            b->player[i].engineBits = GetU32(pkt + 92 + 4 * i);
            b->player[i].epoch = GetU16(pkt + 108 + 2 * i);
        }
        if (h->from != 0 || !ValidStart(b) || b->laps < 1 || b->laps > 9) {
            return -3; /* only the host starts a race */
        }
    } else if (type == GDX_LB_RM_POLL || type == GDX_LB_RM_HAVE || type == GDX_LB_RM_SET) {
        GdxMpLbRemove* b = &body->remove;
        b->gen = GetU32(pkt + 28);
        b->player = pkt[32];
        b->reason = pkt[33];
        b->count = pkt[34];
        b->tick = GetU32(pkt + 36);
        b->first = GetU32(pkt + 40);
        if (b->player >= GDX_MPO_MAX_SLOTS || b->count > GDX_LB_RM_PADS || b->reason < GDX_LB_ABORT_QUIT ||
            b->reason > GDX_LB_ABORT_LAUNCH_FAILED || (type == GDX_LB_RM_POLL && b->count != 0) ||
            b->tick == GDX_MP_NO_TICK || b->first > b->tick || b->tick - b->first < b->count) {
            /* the pads carried end at or before `tick` (HAVE: what we hold; SET: T) */
            return -3;
        }
        for (i = 0; i < b->count; i++) {
            b->pads[i].buttons = GetU16(pkt + 44 + 4 * i);
            b->pads[i].stickX = (int8_t) pkt[46 + 4 * i];
            b->pads[i].stickY = (int8_t) pkt[47 + 4 * i];
        }
    } else {
        GdxMpLbAbort* b = &body->abort;
        b->gen = GetU32(pkt + 28);
        b->reason = pkt[32];
        b->culprit = pkt[33];
        b->tick = GetU32(pkt + 36);
        if (b->reason < GDX_LB_ABORT_QUIT || b->reason > GDX_LB_ABORT_LAUNCH_FAILED) {
            return -3;
        }
    }
    return type;
}

/* ---- delay ---------------------------------------------------------------------------------- */
int gdx_mpo_delay_from_rtt(uint32_t rttMs, uint32_t jitterMs) {
    /* In tenths of a millisecond: (rtt/2 + jitter + 16.7) / 16.7, rounded up. */
    const uint32_t need = rttMs * 5u + jitterMs * 10u + 167u;
    uint32_t d;
    if (rttMs > 10000u || jitterMs > 10000u) {
        return GDX_MPO_MAX_DELAY;
    }
    d = (need + 166u) / 167u;
    if (d < 2u) {
        d = 2u;
    }
    if (d > (uint32_t) GDX_MPO_MAX_DELAY) {
        d = (uint32_t) GDX_MPO_MAX_DELAY;
    }
    return (int) d;
}

uint32_t gdx_mpo_session_digest(const char* build, uint32_t contentHash, const GdxMpLbStart* s) {
    uint8_t wire[GDX_LB_START_BYTES];
    GdxMpLbHeader h;
    uint32_t v = 2166136261u;
    const uint32_t proto = GDX_MPO_PROTO;
    memset(&h, 0, sizeof(h));
    h.type = GDX_LB_START;
    h.secret = 1;
    gdx_lb_build(&h, s, wire, sizeof(wire));
    v = Fnv(v, "gdx-mp-online", 13);
    v = Fnv(v, &proto, sizeof(proto));
    v = Fnv(v, build, strlen(build));
    v = Fnv(v, &contentHash, sizeof(contentHash));
    /* The START body (not its header: sendMs/echo differ per copy). */
    v = Fnv(v, wire + GDX_LB_HEADER_BYTES, GDX_LB_START_BYTES - GDX_LB_HEADER_BYTES);
    return v;
}

/* ---- link state ----------------------------------------------------------------------------- */
void gdx_link_reset(GdxMpLink* l) {
    memset(l, 0, sizeof(*l));
    l->state = GDX_LINK_NONE;
}

static void LinkEnter(GdxMpLink* l, int state, uint32_t nowMs) {
    if (l->state == GDX_LINK_DIRECT && state != GDX_LINK_DIRECT) {
        l->downgrades++;
    }
    if (state == GDX_LINK_DIRECT && l->state != GDX_LINK_DIRECT) {
        l->upgrades++;
    }
    l->state = state;
    l->sinceMs = nowMs;
}

void gdx_link_set_candidates(GdxMpLink* l, const GdxMpEndpoint* pub, const GdxMpEndpoint* priv, int forced,
                             uint32_t nowMs) {
    const int wasNone = (l->state == GDX_LINK_NONE);
    int changed = 0;
    if (pub != NULL && gdx_mpo_endpoint_valid(pub)) {
        changed |= !l->candValid[0] || !gdx_mpo_endpoint_eq(&l->cand[0], pub);
        l->cand[0] = *pub;
        l->candValid[0] = 1;
    } else {
        l->candValid[0] = 0;
    }
    if (priv != NULL && gdx_mpo_endpoint_valid(priv) && !(pub != NULL && gdx_mpo_endpoint_eq(priv, pub))) {
        changed |= !l->candValid[1] || !gdx_mpo_endpoint_eq(&l->cand[1], priv);
        l->cand[1] = *priv;
        l->candValid[1] = 1;
    } else {
        l->candValid[1] = 0;
    }
    l->forced = forced;
    if (forced) {
        if (l->state != GDX_LINK_RELAY) {
            LinkEnter(l, GDX_LINK_RELAY, nowMs);
        }
        return;
    }
    if (wasNone || (changed && l->state == GDX_LINK_RELAY)) {
        LinkEnter(l, GDX_LINK_PUNCHING, nowMs); /* new peer, or it moved: punch (again) */
    }
}

void gdx_link_on_direct_rx(GdxMpLink* l, const GdxMpEndpoint* src, uint32_t nowMs) {
    if (l->state == GDX_LINK_NONE || l->forced) {
        return;
    }
    l->cand[2] = *src;
    l->candValid[2] = 1;
    l->lastDirectRxMs = nowMs;
    l->heardDirect = 1;
    if (l->theyHearUs && l->state != GDX_LINK_DIRECT) {
        LinkEnter(l, GDX_LINK_DIRECT, nowMs);
    }
}

void gdx_link_on_peer_report(GdxMpLink* l, int theyHearUs, uint32_t nowMs) {
    if (l->state == GDX_LINK_NONE || l->forced) {
        return;
    }
    l->theyHearUs = theyHearUs ? 1 : 0;
    if (theyHearUs) {
        l->theyHearUsMs = nowMs;
        if (gdx_link_hear_direct(l, nowMs) && l->state != GDX_LINK_DIRECT) {
            LinkEnter(l, GDX_LINK_DIRECT, nowMs);
        }
    } else if (l->state == GDX_LINK_DIRECT) {
        LinkEnter(l, GDX_LINK_PUNCHING, nowMs); /* they stopped hearing us */
    }
}

int gdx_link_hear_direct(const GdxMpLink* l, uint32_t nowMs) {
    return l->heardDirect && !l->forced && (int32_t) (nowMs - l->lastDirectRxMs) <= GDX_LINK_HEAR_MS;
}

void gdx_link_tick(GdxMpLink* l, uint32_t nowMs) {
    if (l->state == GDX_LINK_DIRECT && !gdx_link_hear_direct(l, nowMs)) {
        LinkEnter(l, GDX_LINK_PUNCHING, nowMs); /* gone quiet directly: relay again while we retry */
    } else if (l->state == GDX_LINK_PUNCHING && (int32_t) (nowMs - l->sinceMs) > GDX_LINK_PUNCH_MS) {
        LinkEnter(l, GDX_LINK_RELAY, nowMs);
    }
}

int gdx_link_route(GdxMpLink* l, uint32_t nowMs, int probe, GdxMpEndpoint* out, int* viaRelay) {
    int n = 0;
    int i;
    *viaRelay = 0;
    switch (l->state) {
        case GDX_LINK_DIRECT:
            out[0] = l->cand[2];
            return 1;
        case GDX_LINK_PUNCHING:
            *viaRelay = 1;
            for (i = 0; i < GDX_LINK_MAX_CAND; i++) {
                if (l->candValid[i]) {
                    out[n++] = l->cand[i];
                }
            }
            return n;
        case GDX_LINK_RELAY:
            *viaRelay = 1;
            if (!l->forced && probe && (int32_t) (nowMs - l->lastProbeMs) >= GDX_LINK_PROBE_MS) {
                l->lastProbeMs = nowMs;
                for (i = 0; i < GDX_LINK_MAX_CAND; i++) {
                    if (l->candValid[i]) {
                        out[n++] = l->cand[i];
                    }
                }
            }
            return n;
        default:
            return 0;
    }
}

const char* gdx_link_state_name(int state) {
    switch (state) {
        case GDX_LINK_PUNCHING: return "punch";
        case GDX_LINK_DIRECT: return "direct";
        case GDX_LINK_RELAY: return "relay";
        default: return "none";
    }
}

/* ---- rate limiting / blocklist -------------------------------------------------------------- */
void gdx_mpo_bucket_init(GdxMpBucket* b, uint32_t ratePerSec, uint32_t burst) {
    memset(b, 0, sizeof(*b));
    b->ratePerSec = ratePerSec;
    b->burst = burst;
    b->tokensX1000 = burst * 1000u;
}

int gdx_mpo_bucket_take(GdxMpBucket* b, uint32_t nowMs) {
    if (!b->primed) {
        b->primed = 1;
        b->stampMs = nowMs;
    } else {
        const uint32_t dt = nowMs - b->stampMs;
        if (dt > 0) {
            const uint64_t add = (uint64_t) dt * b->ratePerSec; /* ms * per-sec = tokens x1000 */
            uint64_t t = (uint64_t) b->tokensX1000 + add;
            if (t > (uint64_t) b->burst * 1000u) {
                t = (uint64_t) b->burst * 1000u;
            }
            b->tokensX1000 = (uint32_t) t;
            b->stampMs = nowMs;
        }
    }
    if (b->tokensX1000 >= 1000u) {
        b->tokensX1000 -= 1000u;
        return 1;
    }
    return 0;
}

void gdx_mpo_block_init(GdxMpBlocklist* bl, uint32_t limit) {
    memset(bl, 0, sizeof(*bl));
    bl->limit = limit;
}

static int BlockFind(const GdxMpBlocklist* bl, const GdxMpEndpoint* src) {
    int i;
    for (i = 0; i < GDX_MPO_BLOCK_SLOTS; i++) {
        if (bl->used[i] && memcmp(bl->ep[i].ip, src->ip, 4) == 0 && bl->ep[i].port == src->port) {
            return i;
        }
    }
    return -1;
}

int gdx_mpo_block_is_blocked(const GdxMpBlocklist* bl, const GdxMpEndpoint* src, uint32_t nowMs) {
    const int i = BlockFind(bl, src);
    return i >= 0 && bl->blockedUntil[i] != 0 && (int32_t) (bl->blockedUntil[i] - nowMs) > 0;
}

void gdx_mpo_block_note_reject(GdxMpBlocklist* bl, const GdxMpEndpoint* src, uint32_t nowMs) {
    int i = BlockFind(bl, src);
    if (i < 0) {
        int j;
        uint32_t oldest = 0;
        i = 0;
        for (j = 0; j < GDX_MPO_BLOCK_SLOTS; j++) {
            if (!bl->used[j]) {
                i = j;
                break;
            }
            if ((uint32_t) (nowMs - bl->windowMs[j]) >= oldest) {
                oldest = nowMs - bl->windowMs[j];
                i = j;
            }
        }
        memset(&bl->ep[i], 0, sizeof(bl->ep[i]));
        memcpy(bl->ep[i].ip, src->ip, 4);
        bl->ep[i].port = src->port;
        bl->used[i] = 1;
        bl->count[i] = 0;
        bl->windowMs[i] = nowMs;
        bl->blockedUntil[i] = 0;
    }
    if ((int32_t) (nowMs - bl->windowMs[i]) > 10000) {
        bl->windowMs[i] = nowMs;
        bl->count[i] = 0;
    }
    if (++bl->count[i] >= bl->limit &&
        (bl->blockedUntil[i] == 0 || (int32_t) (bl->blockedUntil[i] - nowMs) <= 0)) {
        bl->blockedUntil[i] = (nowMs + 60000u) | 1u;
        bl->blocks++;
    }
}

/* ---- content hash: zip central directory ---------------------------------------------------- */
/* Reads exactly `len` bytes (a short read = a malformed archive). */
static int ZipRead(FILE* f, void* dst, size_t len) {
    return fread(dst, 1, len, f) == len ? 0 : -1;
}

/* Skips `len` bytes by reading them (no fseek per record: on the console a seek drops stdio's
 * buffer, and ~3600 records would each cost an SD read). */
static int ZipSkip(FILE* f, uint32_t len, uint8_t* scratch, size_t cap) {
    while (len > 0) {
        const size_t want = len < cap ? len : cap;
        if (ZipRead(f, scratch, want) != 0) {
            return -1;
        }
        len -= (uint32_t) want;
    }
    return 0;
}

/* Finds the end-of-central-directory record: the central directory's size and offset. */
static int ZipFindCd(FILE* f, uint16_t* entries, uint32_t* cdSize, uint32_t* cdOff) {
    static uint8_t tail[65557]; /* EOCD (22) + the longest possible comment (65535) */
    long size;
    long tailLen;
    long i;
    if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 22) {
        return -2;
    }
    tailLen = size < (long) sizeof(tail) ? size : (long) sizeof(tail);
    if (fseek(f, size - tailLen, SEEK_SET) != 0 || ZipRead(f, tail, (size_t) tailLen) != 0) {
        return -2;
    }
    for (i = tailLen - 22; i >= 0; i--) {
        if (tail[i] == 0x50 && tail[i + 1] == 0x4B && tail[i + 2] == 0x05 && tail[i + 3] == 0x06 &&
            i + 22 + GetU16(tail + i + 20) == tailLen) {
            *entries = GetU16(tail + i + 10);
            *cdSize = GetU32(tail + i + 12);
            *cdOff = GetU32(tail + i + 16);
            if (*cdSize == 0xFFFFFFFFu || *cdOff == 0xFFFFFFFFu || *entries == 0xFFFFu ||
                (long) *cdOff + (long) *cdSize > size - tailLen + i || *cdSize > 64u * 1024u * 1024u) {
                return -2; /* zip64 or inconsistent: not something this build wrote */
            }
            return 0;
        }
    }
    return -2;
}

/* The CONTENT of an archive, not its packing: per central record, in directory order, the name,
 * the CRC-32 and the uncompressed size. Deliberately NOT the compression method, the compressed
 * size, the local-header offset, the timestamps or the extra fields: tools/prebake/prebake.py
 * re-deflates fzerox.o2r (and tools/gen_f3d_o2r.py deflates gdiffuser.o2r) with the PC's own
 * zlib, and a different zlib (zlib-ng, another level, --no-store) packs the same members to
 * different sizes, which used to make two consoles with the same ROM refuse each other.
 * The tag keeps this definition from ever equalling the old raw-directory hash. */
int gdx_mpo_zip_cd_hash(const char* path, uint32_t* h) {
    static const char kTag[] = "gdx-content-2";
    uint8_t rec[46];
    uint8_t scratch[1024];
    FILE* f;
    uint16_t entries = 0;
    uint32_t cdSize = 0;
    uint32_t cdOff = 0;
    uint32_t used = 0;
    uint32_t n;
    int rc;
    if (path == NULL || h == NULL) {
        return -1;
    }
    f = fopen(path, "rb");
    if (f == NULL) {
        return -1;
    }
    rc = ZipFindCd(f, &entries, &cdSize, &cdOff);
    if (rc == 0 && fseek(f, (long) cdOff, SEEK_SET) != 0) {
        rc = -2;
    }
    if (rc != 0) {
        fclose(f);
        return rc;
    }
    {
        const uint8_t count[2] = { (uint8_t) (entries & 0xFFu), (uint8_t) (entries >> 8) };
        *h = Fnv(*h, kTag, sizeof(kTag) - 1);
        *h = Fnv(*h, count, sizeof(count)); /* little-endian on every host */
    }
    for (n = 0; n < entries; n++) {
        uint16_t nameLen;
        uint32_t skipLen;
        if (used + 46u > cdSize || ZipRead(f, rec, sizeof(rec)) != 0 || GetU32(rec) != 0x02014B50u) {
            fclose(f);
            return -2;
        }
        nameLen = GetU16(rec + 28);
        skipLen = (uint32_t) GetU16(rec + 30) + GetU16(rec + 32);
        used += 46u + nameLen + skipLen;
        if (used > cdSize || nameLen > sizeof(scratch) || ZipRead(f, scratch, nameLen) != 0) {
            fclose(f);
            return -2;
        }
        *h = Fnv(*h, rec + 28, 2);      /* name length */
        *h = Fnv(*h, scratch, nameLen); /* name */
        *h = Fnv(*h, rec + 16, 4);      /* CRC-32 */
        *h = Fnv(*h, rec + 24, 4);      /* uncompressed size */
        if (ZipSkip(f, skipLen, scratch, sizeof(scratch)) != 0) {
            fclose(f);
            return -2;
        }
    }
    fclose(f);
    return 0;
}
