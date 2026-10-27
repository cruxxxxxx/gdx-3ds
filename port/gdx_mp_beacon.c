/* port/gdx_mp_beacon.c — nearby multiplayer: the room beacon. Contract: gdx_mp_beacon.h. */
#include "gdx_mp_beacon.h"

#include <stdio.h>
#include <string.h>

static const uint8_t kMagic[4] = { 'G', 'X', 'B', 'N' };

#define OFF_NAME 24
#define OFF_BUILD 56
#define OFF_RESERVED 80
#define OFF_CHECK 84

static void PutU32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t) (v & 0xFF);
    p[1] = (uint8_t) ((v >> 8) & 0xFF);
    p[2] = (uint8_t) ((v >> 16) & 0xFF);
    p[3] = (uint8_t) (v >> 24);
}

static uint32_t GetU32(const uint8_t* p) {
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
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

uint32_t gdx_mp_beacon_build_hash(const char* build) {
    const char* s = (build != NULL) ? build : "";
    return Fnv(2166136261u, s, strlen(s));
}

/* Length of the valid UTF-8 sequence at s (n bytes available), 0 if malformed. Rejects
 * overlongs, surrogates and code points above U+10FFFF. *cp = the code point. */
static int Utf8Seq(const uint8_t* s, int n, uint32_t* cp) {
    uint32_t c;
    int len;
    int i;
    if (n <= 0) {
        return 0;
    }
    if (s[0] < 0x80) {
        *cp = s[0];
        return 1;
    } else if ((s[0] & 0xE0) == 0xC0) {
        len = 2;
        c = s[0] & 0x1F;
    } else if ((s[0] & 0xF0) == 0xE0) {
        len = 3;
        c = s[0] & 0x0F;
    } else if ((s[0] & 0xF8) == 0xF0) {
        len = 4;
        c = s[0] & 0x07;
    } else {
        return 0;
    }
    if (n < len) {
        return 0;
    }
    for (i = 1; i < len; i++) {
        if ((s[i] & 0xC0) != 0x80) {
            return 0;
        }
        c = (c << 6) | (s[i] & 0x3F);
    }
    if ((len == 2 && c < 0x80) || (len == 3 && c < 0x800) || (len == 4 && c < 0x10000) || c > 0x10FFFF ||
        (c >= 0xD800 && c <= 0xDFFF)) {
        return 0;
    }
    *cp = c;
    return len;
}

static int IsControl(uint32_t cp) {
    return cp < 0x20 || (cp >= 0x7F && cp < 0xA0) || cp == 0x2028 || cp == 0x2029 || cp == 0xFEFF;
}

int gdx_mp_utf8_sanitise(const char* in, char* out, int cap) {
    const uint8_t* s = (const uint8_t*) (in != NULL ? in : "");
    int n = (int) strlen((const char*) s);
    int o = 0;
    int i = 0;
    if (out == NULL || cap <= 0) {
        return 0;
    }
    while (i < n) {
        uint32_t cp = 0;
        const int len = Utf8Seq(s + i, n - i, &cp);
        if (len == 0 || IsControl(cp)) {
            if (o + 1 > cap - 1) {
                break;
            }
            out[o++] = '?';
            i += (len == 0) ? 1 : len;
            continue;
        }
        if (o + len > cap - 1) {
            break; /* truncate at a character boundary */
        }
        memcpy(out + o, s + i, (size_t) len);
        o += len;
        i += len;
    }
    out[o] = '\0';
    return o;
}

static int PutUtf8(uint32_t cp, char* out, int room) {
    if (cp < 0x80) {
        if (room < 1) {
            return 0;
        }
        out[0] = (char) cp;
        return 1;
    } else if (cp < 0x800) {
        if (room < 2) {
            return 0;
        }
        out[0] = (char) (0xC0 | (cp >> 6));
        out[1] = (char) (0x80 | (cp & 0x3F));
        return 2;
    } else if (cp < 0x10000) {
        if (room < 3) {
            return 0;
        }
        out[0] = (char) (0xE0 | (cp >> 12));
        out[1] = (char) (0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char) (0x80 | (cp & 0x3F));
        return 3;
    }
    if (room < 4) {
        return 0;
    }
    out[0] = (char) (0xF0 | (cp >> 18));
    out[1] = (char) (0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char) (0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char) (0x80 | (cp & 0x3F));
    return 4;
}

int gdx_mp_name_from_utf16(const uint16_t* in, int units, char* out, int cap) {
    int o = 0;
    int i = 0;
    if (out == NULL || cap <= 0) {
        return 0;
    }
    out[0] = '\0';
    if (in == NULL) {
        return 0;
    }
    while (i < units && in[i] != 0) {
        uint32_t cp = in[i];
        int w;
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < units && in[i + 1] >= 0xDC00 && in[i + 1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (uint32_t) (in[i + 1] - 0xDC00);
            i += 2;
        } else if (cp >= 0xD800 && cp <= 0xDFFF) {
            cp = '?';
            i += 1;
        } else {
            i += 1;
        }
        if (IsControl(cp)) {
            cp = '?';
        }
        w = PutUtf8(cp, out + o, cap - 1 - o);
        if (w == 0) {
            break;
        }
        o += w;
    }
    out[o] = '\0';
    return o;
}

/* Copies a string into a fixed field: truncated at a UTF-8 boundary, NUL padded. */
static void PutField(uint8_t* dst, int size, const char* src) {
    char tmp[GDX_MP_BEACON_NAME_BYTES > GDX_MP_BEACON_BUILD_BYTES ? GDX_MP_BEACON_NAME_BYTES
                                                                   : GDX_MP_BEACON_BUILD_BYTES];
    int n;
    memset(dst, 0, (size_t) size);
    n = gdx_mp_utf8_sanitise(src, tmp, size);
    memcpy(dst, tmp, (size_t) n);
}

int gdx_mp_beacon_encode(const GdxMpBeacon* b, uint8_t* out, int cap) {
    if (b == NULL || out == NULL || cap < GDX_MP_BEACON_BYTES) {
        return 0;
    }
    if (b->maxPlayers < 2 || b->maxPlayers > GDX_MP_BEACON_MAX_PLAYERS || b->players > b->maxPlayers) {
        return 0;
    }
    memset(out, 0, GDX_MP_BEACON_BYTES);
    memcpy(out, kMagic, 4);
    out[4] = b->version;
    out[5] = b->flags;
    out[6] = b->players;
    out[7] = b->maxPlayers;
    out[8] = b->model;
    out[9] = b->proto;
    out[10] = b->delay;
    out[11] = b->hashEvery;
    PutU32(out + 12, b->buildHash);
    PutU32(out + 16, b->contentHash);
    PutU32(out + 20, b->roomId);
    PutField(out + OFF_NAME, GDX_MP_BEACON_NAME_BYTES, b->name);
    PutField(out + OFF_BUILD, GDX_MP_BEACON_BUILD_BYTES, b->build);
    PutU32(out + OFF_RESERVED, 0);
    PutU32(out + OFF_CHECK, Fnv(2166136261u, out, OFF_CHECK));
    return GDX_MP_BEACON_BYTES;
}

/* A fixed string field must hold a NUL; the bytes before it are sanitised for display. */
static int GetField(const uint8_t* src, int size, char* dst) {
    int i;
    for (i = 0; i < size; i++) {
        if (src[i] == 0) {
            break;
        }
    }
    if (i == size) {
        return -1;
    }
    gdx_mp_utf8_sanitise((const char*) src, dst, size);
    return 0;
}

/* The raw field (NUL-terminated: GetField checked) is printable ASCII. */
static int PrintableAscii(const uint8_t* s) {
    for (; *s != 0; s++) {
        if (*s < 0x20 || *s > 0x7E) {
            return 0;
        }
    }
    return 1;
}

int gdx_mp_beacon_decode(const uint8_t* in, int len, GdxMpBeacon* out) {
    GdxMpBeacon b;
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    if (in == NULL || out == NULL || len < GDX_MP_BEACON_BYTES || len > GDX_MP_BEACON_APPDATA_MAX) {
        return GDX_MP_BEACON_E_SHORT;
    }
    if (memcmp(in, kMagic, 4) != 0) {
        return GDX_MP_BEACON_E_MAGIC;
    }
    if (in[4] == 0 || (in[4] == GDX_MP_BEACON_VERSION && len != GDX_MP_BEACON_BYTES)) {
        return GDX_MP_BEACON_E_VERSION;
    }
    if (GetU32(in + len - 4) != Fnv(2166136261u, in, (size_t) (len - 4))) {
        return GDX_MP_BEACON_E_CHECK;
    }
    memset(&b, 0, sizeof(b));
    b.version = in[4];
    b.flags = in[5];
    b.players = in[6];
    b.maxPlayers = in[7];
    b.model = in[8];
    b.proto = in[9];
    b.delay = in[10];
    b.hashEvery = in[11];
    b.buildHash = GetU32(in + 12);
    b.contentHash = GetU32(in + 16);
    b.roomId = GetU32(in + 20);
    if (b.maxPlayers < 2 || b.maxPlayers > GDX_MP_BEACON_MAX_PLAYERS || b.players < 1 || b.players > b.maxPlayers) {
        return GDX_MP_BEACON_E_FIELDS;
    }
    if (GetField(in + OFF_NAME, GDX_MP_BEACON_NAME_BYTES, b.name) != 0 ||
        GetField(in + OFF_BUILD, GDX_MP_BEACON_BUILD_BYTES, b.build) != 0 || !PrintableAscii(in + OFF_BUILD)) {
        return GDX_MP_BEACON_E_FIELDS;
    }
    *out = b;
    return GDX_MP_BEACON_OK;
}

int gdx_mp_beacon_compat(const GdxMpBeacon* b, uint32_t buildHash, uint32_t contentHash, int proto) {
    if (b->version != GDX_MP_BEACON_VERSION || b->proto != (uint8_t) proto) {
        return GDX_MP_COMPAT_PROTO;
    }
    if (b->buildHash != buildHash) {
        return GDX_MP_COMPAT_BUILD;
    }
    if (b->contentHash != contentHash) {
        return GDX_MP_COMPAT_CONTENT;
    }
    if (b->flags & GDX_MP_BEACON_F_RACING) {
        return GDX_MP_COMPAT_RACING;
    }
    if (b->players >= b->maxPlayers || !(b->flags & GDX_MP_BEACON_F_OPEN)) {
        return GDX_MP_COMPAT_FULL;
    }
    return GDX_MP_COMPAT_OK;
}

const char* gdx_mp_compat_text(int compat) {
    switch (compat) {
        case GDX_MP_COMPAT_OK:
            return "ok";
        case GDX_MP_COMPAT_PROTO:
            return "newer or older multiplayer version";
        case GDX_MP_COMPAT_BUILD:
            return "different build";
        case GDX_MP_COMPAT_CONTENT:
            return "different game data";
        case GDX_MP_COMPAT_FULL:
            return "room full";
        case GDX_MP_COMPAT_RACING:
            return "race in progress";
        default:
            return "?";
    }
}

const char* gdx_mp_model_name(int model) {
    switch (model) {
        case GDX_MP_MODEL_3DS:
            return "O3DS";
        case GDX_MP_MODEL_3DSXL:
            return "O3DSXL";
        case GDX_MP_MODEL_N3DS:
            return "N3DS";
        case GDX_MP_MODEL_2DS:
            return "2DS";
        case GDX_MP_MODEL_N3DSXL:
            return "N3DSXL";
        case GDX_MP_MODEL_N2DSXL:
            return "N2DSXL";
        default:
            return "3DS?";
    }
}

int gdx_mp_model_is_new(int model) {
    return model == GDX_MP_MODEL_N3DS || model == GDX_MP_MODEL_N3DSXL || model == GDX_MP_MODEL_N2DSXL;
}

int gdx_mp_signal_bars(int strength) {
    if (strength < 0 || strength > 3) {
        return -1;
    }
    return strength;
}

/* out[0..n) may end inside a multi-byte character: the length without that partial tail. */
static int TrimPartialUtf8(const char* out, int n) {
    int lead = n;
    int need;
    unsigned char c;
    while (lead > 0 && ((unsigned char) out[lead - 1] & 0xC0) == 0x80) {
        lead--;
    }
    if (lead == 0) {
        return 0;
    }
    c = (unsigned char) out[lead - 1];
    if (c < 0x80) {
        return n; /* ends on ASCII (any continuation bytes after it were not ours) */
    }
    need = ((c & 0xE0) == 0xC0) ? 2 : ((c & 0xF0) == 0xE0) ? 3 : ((c & 0xF8) == 0xF0) ? 4 : 1;
    if ((lead - 1) + need > n) {
        return lead - 1;
    }
    return n;
}

int gdx_mp_beacon_label(const GdxMpBeacon* b, int signalBars, char* out, int cap) {
    static const char* const kBar[3] = { "\xE2\x96\x82", "\xE2\x96\x84", "\xE2\x96\x86" }; /* U+2582/4/6 */
    char name[GDX_MP_BEACON_NAME_BYTES];
    int n;
    int i;
    if (out == NULL || cap <= 0) {
        return 0;
    }
    gdx_mp_utf8_sanitise(b->name[0] != '\0' ? b->name : "3DS", name, sizeof(name));
    n = snprintf(out, (size_t) cap, "%s's room %u/%u %s", name, (unsigned) b->players, (unsigned) b->maxPlayers,
                 gdx_mp_model_name(b->model));
    if (n < 0) {
        out[0] = '\0';
        return 0;
    }
    if (n >= cap) {
        /* snprintf cut at cap - 1 bytes: never leave half a UTF-8 character at the cut. */
        n = TrimPartialUtf8(out, cap - 1);
        out[n] = '\0';
        return n;
    }
    if (signalBars < 0 || signalBars > 3) {
        return n;
    }
    if (n + 1 < cap) {
        out[n++] = ' ';
        out[n] = '\0';
    }
    for (i = 0; i < 3; i++) {
        const char* piece = (i < signalBars) ? kBar[i] : "_";
        const int pl = (int) strlen(piece);
        if (n + pl > cap - 1) {
            break;
        }
        memcpy(out + n, piece, (size_t) pl);
        n += pl;
        out[n] = '\0';
    }
    return n;
}

void gdx_mp_uds_passphrase(const char* build, uint32_t contentHash, int proto, char* out) {
    static const char kHex[] = "0123456789abcdef";
    static const uint32_t kSeeds[4] = { 2166136261u, 0x9E3779B9u, 0x85EBCA6Bu, 0xC2B2AE35u };
    const char* s = (build != NULL) ? build : "";
    uint8_t tail[8];
    int r;
    int k;
    PutU32(tail, contentHash);
    PutU32(tail + 4, (uint32_t) proto);
    for (r = 0; r < 4; r++) {
        uint32_t h = kSeeds[r];
        h = Fnv(h, "gdx3ds-uds-v1|", 14);
        h = Fnv(h, s, strlen(s));
        h = Fnv(h, "|", 1);
        h = Fnv(h, tail, sizeof(tail));
        h ^= h >> 15;
        h *= 0x2C1B3C6Du;
        h ^= h >> 12;
        for (k = 0; k < 8; k++) {
            out[r * 8 + k] = kHex[(h >> (28 - 4 * k)) & 0xF];
        }
    }
    out[GDX_MP_UDS_PASSPHRASE_LEN] = '\0';
}
