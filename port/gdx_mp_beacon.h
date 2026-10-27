/* port/gdx_mp_beacon.h — nearby multiplayer: the room beacon (pure, host-testable).
 *
 * A host advertises its room in the appdata of its UDS beacon (udsSetApplicationData, at most
 * 0xC8 bytes; port/3ds/net/gdx3ds_mp_uds.c). The same bytes are the payload a LAN discovery
 * reply can carry, so the NEARBY list decodes one format whatever the medium.
 *
 * WIRE FORMAT v1 (little-endian, GDX_MP_BEACON_BYTES = 88 bytes):
 *    0  'G' 'X' 'B' 'N'
 *    4  u8  version     (GDX_MP_BEACON_VERSION; a newer one is listed, greyed out)
 *    5  u8  flags       (GDX_MP_BEACON_F_*)
 *    6  u8  players     (in the room now, host included)
 *    7  u8  maxPlayers  (2..GDX_MP_BEACON_MAX_PLAYERS)
 *    8  u8  model       (CFG_SystemModel of the host: 0 3DS, 1 3DSXL, 2 N3DS, 3 2DS, 4 N3DSXL,
 *                        5 N2DSXL; 0xFF unknown)
 *    9  u8  proto       (multiplayer protocol generation, GDX_MP_BEACON_PROTO)
 *   10  u8  delay       (the session's input delay in ticks; 0 = the lobby picks it)
 *   11  u8  hashEvery   (state digest cadence; 0 = the lobby picks it)
 *   12  u32 buildHash   (FNV-1a of the build id string: the simulation code)
 *   16  u32 contentHash (the game data the build loads: .o2r archives)
 *   20  u32 roomId      (random per hosted room: tells two rooms of one host apart)
 *   24  char name[32]   (the host's 3DS profile name, UTF-8, NUL padded, NUL terminated)
 *   56  char build[24]  (the build id, printable ASCII, NUL padded: "host v1.0.2" in a refusal)
 *   80  u32 reserved    (0)
 *   84  u32 check       (FNV-1a of bytes 0..83: random junk is refused, not shown)
 * A future version may be longer; its check is then the last 4 bytes and the v1 fields keep
 * their offsets, so an old build still lists (and greys out) a newer room.
 *
 * COMPATIBILITY. A room is joinable only when proto, buildHash and contentHash all match ours.
 * The UDS passphrase is derived from the same build + content (gdx_mp_uds_passphrase), so a
 * mismatched build cannot even associate: the greyed entry and the refused connect agree.
 *
 * Every byte that arrives here is untrusted: decode validates the shape, the check, every
 * count, the string terminations, and makes the strings safe to draw (gdx_mp_beacon_label). */
#ifndef GDX_MP_BEACON_H
#define GDX_MP_BEACON_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GDX_MP_BEACON_VERSION 1
#define GDX_MP_BEACON_PROTO 1
#define GDX_MP_BEACON_BYTES 88
#define GDX_MP_BEACON_APPDATA_MAX 0xC8 /* udsNetworkStruct.appdata */
#define GDX_MP_BEACON_NAME_BYTES 32
#define GDX_MP_BEACON_BUILD_BYTES 24
#define GDX_MP_BEACON_MAX_PLAYERS 8 /* the format's ceiling; the session cap is 4 today */

enum {
    GDX_MP_BEACON_F_OPEN = 0x01,   /* accepting players (in the lobby) */
    GDX_MP_BEACON_F_RACING = 0x02, /* a race is on: no new players */
};

enum {
    GDX_MP_MODEL_3DS = 0,
    GDX_MP_MODEL_3DSXL = 1,
    GDX_MP_MODEL_N3DS = 2,
    GDX_MP_MODEL_2DS = 3,
    GDX_MP_MODEL_N3DSXL = 4,
    GDX_MP_MODEL_N2DSXL = 5,
    GDX_MP_MODEL_UNKNOWN = 0xFF,
};

/* Decode results. */
enum {
    GDX_MP_BEACON_OK = 0,
    GDX_MP_BEACON_E_SHORT = -1,   /* fewer than GDX_MP_BEACON_BYTES, or over the appdata max */
    GDX_MP_BEACON_E_MAGIC = -2,
    GDX_MP_BEACON_E_CHECK = -3,   /* the FNV check does not match */
    GDX_MP_BEACON_E_FIELDS = -4,  /* counts out of range, or an unterminated string */
    GDX_MP_BEACON_E_VERSION = -5, /* version 0, or a v1 beacon of the wrong length */
};

/* Why a room is not joinable (gdx_mp_beacon_compat). */
enum {
    GDX_MP_COMPAT_OK = 0,
    GDX_MP_COMPAT_PROTO = 1,   /* a different multiplayer protocol (or a newer beacon version) */
    GDX_MP_COMPAT_BUILD = 2,   /* a different build */
    GDX_MP_COMPAT_CONTENT = 3, /* the same build, different game data */
    GDX_MP_COMPAT_FULL = 4,    /* compatible, but no free seat */
    GDX_MP_COMPAT_RACING = 5,  /* compatible, but a race is on */
};

typedef struct GdxMpBeacon {
    uint8_t version;
    uint8_t flags;
    uint8_t players;
    uint8_t maxPlayers;
    uint8_t model;
    uint8_t proto;
    uint8_t delay;
    uint8_t hashEvery;
    uint32_t buildHash;
    uint32_t contentHash;
    uint32_t roomId;
    char name[GDX_MP_BEACON_NAME_BYTES];   /* NUL terminated (at most 31 bytes of UTF-8) */
    char build[GDX_MP_BEACON_BUILD_BYTES]; /* NUL terminated (at most 23 printable ASCII) */
} GdxMpBeacon;

/* FNV-1a 32 over a NUL-terminated string (NULL = the empty string). */
uint32_t gdx_mp_beacon_build_hash(const char* build);

/* Serialises `b` (version/proto are written as given). The strings are truncated at a UTF-8
 * character boundary to fit. Returns GDX_MP_BEACON_BYTES, or 0 when cap is too small or a
 * count is out of range. */
int gdx_mp_beacon_encode(const GdxMpBeacon* b, uint8_t* out, int cap);

/* Parses untrusted appdata. GDX_MP_BEACON_OK fills *out; anything else leaves *out zeroed. */
int gdx_mp_beacon_decode(const uint8_t* in, int len, GdxMpBeacon* out);

/* GDX_MP_COMPAT_* for a decoded beacon against our own build/content/proto. */
int gdx_mp_beacon_compat(const GdxMpBeacon* b, uint32_t buildHash, uint32_t contentHash, int proto);
const char* gdx_mp_compat_text(int compat);

/* "N3DS", "O3DS", "2DS", "N2DSXL", "3DSXL", "N3DSXL", or "3DS?" for unknown. */
const char* gdx_mp_model_name(int model);
/* 1 for the New 3DS family (N3DS, N3DSXL, N2DSXL). */
int gdx_mp_model_is_new(int model);

/* The NEARBY row: "<name>'s room n/max MODEL" plus the signal bars when signalBars is 0..3
 * (U+2582 U+2584 U+2586 for each bar present, '_' for each one missing); -1 = no bars.
 * Control characters and malformed UTF-8 in the name become '?'. Returns the length
 * written (always NUL-terminated when cap > 0). */
int gdx_mp_beacon_label(const GdxMpBeacon* b, int signalBars, char* out, int cap);

/* Makes an untrusted UTF-8 string safe to show: malformed sequences, overlongs, surrogates
 * and control characters become '?', and the result is truncated at a character boundary
 * to cap - 1 bytes. Returns the length. */
int gdx_mp_utf8_sanitise(const char* in, char* out, int cap);

/* UTF-16 (the 3DS profile name: CFG block 0x000A0000, at most 10 code units, NUL ends early)
 * -> UTF-8, sanitised, truncated at a character boundary. Unpaired surrogates become '?'. */
int gdx_mp_name_from_utf16(const uint16_t* in, int units, char* out, int cap);

/* Signal bars 0..3 from a 0..3 strength (osGetWifiStrength's scale); anything else -> -1. */
int gdx_mp_signal_bars(int strength);

/* The UDS network passphrase: 32 hex characters derived from the proto, the build id and the
 * content hash (out must hold GDX_MP_UDS_PASSPHRASE_LEN + 1). It is not a secret -- every copy
 * of the build derives it -- it makes NWM itself refuse a console running other code or data. */
#define GDX_MP_UDS_PASSPHRASE_LEN 32
void gdx_mp_uds_passphrase(const char* build, uint32_t contentHash, int proto, char* out);

#ifdef __cplusplus
}
#endif

#endif /* GDX_MP_BEACON_H */
