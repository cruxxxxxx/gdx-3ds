/* port/gdx_mp_online.h — online multiplayer (mp-online track): the pure, host-testable half of
 * the internet transport and the lobby.
 *
 * Nothing here touches a socket, a clock or a global: callers pass timestamps and buffers, so all
 * of it runs in port/tests/gdx_mp_online_host_tests.c. The 3DS half is
 * port/3ds/net/gdx3ds_mp_p2p.c (the socket backend) and port/3ds/net/gdx3ds_mp.c (the session).
 *
 * THREE WIRE FORMATS share one UDP socket, told apart by their first four bytes:
 *   GXRV  console <-> rendezvous server (tools/mp-rendezvous/gdx_rendezvous.py; the README there
 *         has the byte tables). Rooms, endpoints, hole-punch nudges, relay.
 *   GXLB  console <-> console, the lobby: HELLO (ping, ready, picks, build), START (the host's
 *         frozen race settings + session token), ABORT. Every GXLB carries the room secret the
 *         server handed every member, and its `from` is checked against the roster.
 *   GXMP  console <-> console, the lockstep pads (port/gdx_mp_lockstep.h). Online sessions put
 *         the sender's room slot + 1 in header byte 34 so a relay and a receiver can check it.
 * A relayed datagram is forwarded VERBATIM by the server; it arrives from the server's address.
 *
 * ENDPOINT BINDING. A console accepts a GXLB from an unknown address only when the room secret
 * and the roster check pass; that address then becomes the peer's endpoint (a "peer-reflexive"
 * candidate, which is what makes one-sided symmetric NAT work). GXMP is accepted only from a
 * bound endpoint or from the server's relay. A link is DIRECT only when it is confirmed both
 * ways (we hear them directly AND their HELLO says they hear us directly); otherwise it is
 * PUNCHING (send direct and relayed) or RELAY (relayed only). */
#ifndef GDX_MP_ONLINE_H
#define GDX_MP_ONLINE_H

#include <stdint.h>

#include "gdx_mp_lockstep.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GDX_MPO_MAX_SLOTS 4
enum {
    GDX_MPO_MODEL_UNKNOWN = 0,
    GDX_MPO_MODEL_O3DS = 1,
    GDX_MPO_MODEL_N3DS = 2,
};
/* THE CLIENT PROTOCOL VERSION, the one source of truth: bumps with any GXRV / GXLB / GXMP-online
 * or sim-affecting change. Part of the HELLO handshake and the sim-ABI word, and every v2 CREATE /
 * JOIN carries it to the server, which refuses a client below its --min-client-proto (or above
 * its --max-client-proto) with UPDATE. tools/mp-rendezvous/gdx_rendezvous.py CLIENT_PROTO mirrors
 * it (test_rendezvous.py asserts they match): bump both together. */
#define GDX_MPO_PROTO 9
/* 9 (card-4 cookie): GXRV v2 CREATE / JOIN (the return-routability cookie + this number); the
 * server requires both by default. */
/* 8 (mp-menus): START 108+2i carries each player's slot epoch (the roster's: which console held
 * the slot when the host started). A START for a slot's previous console is not this console's. */
/* 7 (mp30 + stockpick): both layouts below in one packet -- HELLO 108..111 simAbi, 112..127 the
 * pick fields; START 82 racers, 83 ramMask, 84..87 simAbi, 88 courseKey, 92+4i engineBits. A build
 * with only one of them (proto 5 or 6) is a different version, refused in the lobby. */
/* 6 (stockpick): HELLO carries picking / pickRound / picked / pickMsLeft (the host opens machine
 * select on every console at once; each player picks with the stock top-screen menus within
 * GDX_MPO_PICK_MS), courseFor / courseKey (the course every console must hold) and engineBits;
 * START carries courseKey and every player's exact engine bits. */
/* 4 (MP30): START carries racers, the VS COM machine count (0 = stock: 4 with VS COM). */
/* 3 (MP30): HELLO and START carry simAbi, the sim-ABI word (gdx3ds_mp_sim_abi): the build flavour
 * (Expansion Kit or not), this protocol and the levers a session forces. Same commit, other
 * flavour = a different game: refused in the lobby, not desynced in the race. */
#define GDX_MPO_BUILD_LEN 40
#define GDX_MPO_NAME_LEN 12
#define GDX_MPO_CODE_LEN 5

/* ---- GXRV (rendezvous) ------------------------------------------------------------------------- */
#define GDX_RV_VERSION 1        /* every message a v1 card sends, and all but CREATE / JOIN in v2 */
#define GDX_RV_VERSION_COOKIE 2 /* version 1 + the return-routability cookie on CREATE / JOIN */
enum {
    GDX_RV_CREATE = 1,
    GDX_RV_JOIN = 2,
    GDX_RV_KEEPALIVE = 3,
    GDX_RV_LEAVE = 4,
    GDX_RV_RELAY = 5,
    GDX_RV_PUNCH_REQ = 6,
    GDX_RV_LOCK = 7,
    GDX_RV_WELCOME = 0x81,
    GDX_RV_ROSTER = 0x82,
    GDX_RV_PUNCH_NOW = 0x83,
    GDX_RV_ERROR = 0x84,
    GDX_RV_KA_ACK = 0x85,
    GDX_RV_COOKIE = 0x86, /* version 2 only */
    GDX_RV_UPDATE = 0x87, /* version 2 only: this client's proto is outside the server's range */
};
enum {
    GDX_RV_E_BAD_REQUEST = 1,
    GDX_RV_E_ROOM_NOT_FOUND = 2,
    GDX_RV_E_ROOM_FULL = 3,
    GDX_RV_E_BUILD_MISMATCH = 4,
    GDX_RV_E_CONTENT_MISMATCH = 5,
    GDX_RV_E_RATE_LIMITED = 6,
    GDX_RV_E_SERVER_FULL = 7,
    GDX_RV_E_ROOM_LOCKED = 8,
    GDX_RV_E_AUTH = 9,
    GDX_RV_E_HOST_LEFT = 10,
    GDX_RV_E_CODE_TAKEN = 11,
};
#define GDX_RV_REQ_BYTES 84
#define GDX_RV_AUTH_BYTES 28
#define GDX_RV_KEEPALIVE_BYTES 36
#define GDX_RV_WELCOME_BYTES 48
#define GDX_RV_ROSTER_MIN 24
#define GDX_RV_ROSTER_ENTRY 28
#define GDX_RV_MAX_RELAY_PAYLOAD 1200
#define GDX_RV_COOKIE_LEN 16
/* v2 CREATE / JOIN: the v1 body, the cookie at 84, the client proto (u16) at 100, 0 (u16) at 102 */
#define GDX_RV_REQ2_BYTES (GDX_RV_REQ_BYTES + GDX_RV_COOKIE_LEN + 4) /* 104 */
#define GDX_RV_COOKIE_BYTES 28                                         /* header, nonce, cookie */
#define GDX_RV_UPDATE_BYTES 60 /* header, nonce, min, max, yours, 0, 40 text */
#define GDX_RV_UPDATE_TEXT_LEN 40

typedef struct GdxMpEndpoint {
    uint8_t ip[4]; /* network order, a.b.c.d */
    uint16_t port; /* host order */
} GdxMpEndpoint;

typedef struct GdxMpRvRequest {
    uint32_t nonce;
    uint32_t contentHash;
    GdxMpEndpoint priv;
    uint8_t maxPlayers;
    uint8_t forceRelay;
    char build[GDX_MPO_BUILD_LEN + 1];
    char name[GDX_MPO_NAME_LEN + 1];
    char code[GDX_MPO_CODE_LEN + 1]; /* JOIN: required; CREATE: "" = server picks */
    uint16_t clientProto;            /* v2 only: GDX_MPO_PROTO (a test lever may lower it) */
} GdxMpRvRequest;

typedef struct GdxMpRvWelcome {
    uint32_t nonce;
    char code[GDX_MPO_CODE_LEN + 1];
    uint8_t slot;
    uint8_t maxPlayers;
    uint8_t forceRelay;
    uint64_t memberKey;
    uint64_t roomSecret;
    GdxMpEndpoint seenAs; /* our public endpoint as the server saw it */
} GdxMpRvWelcome;

typedef struct GdxMpRvMember {
    uint8_t present;
    uint8_t isHost;
    uint16_t epoch; /* the server bumps it when a different console takes the slot */
    GdxMpEndpoint pub;
    GdxMpEndpoint priv;
    char name[GDX_MPO_NAME_LEN + 1];
} GdxMpRvMember;

typedef struct GdxMpRvRoster {
    char code[GDX_MPO_CODE_LEN + 1];
    uint32_t version;
    uint8_t locked;
    uint8_t forceRelay;
    int count;
    GdxMpRvMember member[GDX_MPO_MAX_SLOTS];
} GdxMpRvRoster;

typedef struct GdxMpRvError {
    uint32_t nonce;
    uint8_t code;
    uint16_t hostProto; /* BUILD / CONTENT mismatch to a v2 joiner: the host's client proto (0 = unknown) */
    char detail[GDX_MPO_BUILD_LEN + 1];
} GdxMpRvError;

typedef struct GdxMpRvUpdate {
    uint32_t nonce;
    uint16_t minProto;
    uint16_t maxProto;  /* 0 = no ceiling */
    uint16_t yourProto; /* what the server read from the request */
    char text[GDX_RV_UPDATE_TEXT_LEN + 1]; /* the server's --update-url ("" = none) */
} GdxMpRvUpdate;

/* Builders return the byte count, 0 on a bad argument or a too-small buffer. */
int gdx_rv_build_request(int type, const GdxMpRvRequest* req, uint8_t* out, int cap);
int gdx_rv_build_auth(int type, const char* code, int slot, int arg, uint64_t memberKey, uint8_t* out, int cap);
int gdx_rv_build_keepalive(const char* code, int slot, uint64_t memberKey, uint32_t rosterVersion, uint32_t sendMs,
                           uint8_t* out, int cap);
/* RELAY: auth prefix (arg = destination slot mask) + the payload, verbatim. */
int gdx_rv_build_relay(const char* code, int slot, int dstMask, uint64_t memberKey, const uint8_t* payload,
                       int len, uint8_t* out, int cap);

/* v2 CREATE / JOIN: the 84-byte v1 body with header version 2, then the 16 cookie bytes (all zero
 * on the first try), then req->clientProto. 104 bytes, or 0. */
int gdx_rv_build_request_v2(int type, const GdxMpRvRequest* req, const uint8_t cookie[GDX_RV_COOKIE_LEN],
                            uint8_t* out, int cap);

/* The GXRV message type of a server datagram (>= 0x80), or -1 if it is not a well-formed one.
 * Header version 1 or 2 (a v2 member's replies carry 2). */
int gdx_rv_peek_type(const uint8_t* pkt, int len);
/* COOKIE (version 2, 28 bytes): the nonce it answers and the 16 cookie bytes. 0 = ok. */
int gdx_rv_parse_cookie(const uint8_t* pkt, int len, uint32_t* nonce, uint8_t cookie[GDX_RV_COOKIE_LEN]);
/* UPDATE (version 2, 60 bytes). 0 = ok. */
int gdx_rv_parse_update(const uint8_t* pkt, int len, GdxMpRvUpdate* out);

/* ---- version gating: what the player is told -------------------------------------------------- */
enum {
    GDX_RV_UPD_REFUSED = 0,    /* an UPDATE that does not say which side (inconsistent numbers) */
    GDX_RV_UPD_CLIENT_OLD = 1, /* this console is below the server's minimum: update the game */
    GDX_RV_UPD_SERVER_OLD = 2, /* this console is above the server's maximum: the server is outdated */
};
int gdx_rv_update_verdict(const GdxMpRvUpdate* u, int myProto);
/* "UPDATE REQUIRED - this server needs version 10 (you have 9) - <text>" /
 * "SERVER NEEDS AN UPDATE - it supports up to version 9 (you have 10)". Returns the verdict. */
int gdx_rv_update_message(const GdxMpRvUpdate* u, int myProto, char* out, int cap);
/* A refused CREATE / JOIN's ERROR as the player reads it, with which side should update when the
 * server said (hostProto) -- "version mismatch: host runs X (update this console)". */
void gdx_rv_error_message(const GdxMpRvError* e, int myProto, char* out, int cap);
/* Which side of a mismatched peer should update, from the two protos: "update this console",
 * "they need an update" or "same version, other build". */
const char* gdx_mpo_update_side(int theirProto, int myProto);
int gdx_rv_parse_welcome(const uint8_t* pkt, int len, GdxMpRvWelcome* out);
int gdx_rv_parse_roster(const uint8_t* pkt, int len, GdxMpRvRoster* out);
int gdx_rv_parse_error(const uint8_t* pkt, int len, GdxMpRvError* out);
int gdx_rv_parse_ka_ack(const uint8_t* pkt, int len, uint32_t* rosterVersion, uint32_t* echoMs);
int gdx_rv_parse_punch_now(const uint8_t* pkt, int len, int* fromSlot);
const char* gdx_rv_error_text(int code);

/* ---- GXRV v2, the console's request state machine ([net] mp_rv_cookie) ------------------------
 * One per socket session. Every CREATE / JOIN goes through it:
 *   begin()  a new request (new nonce): the cookie is cleared;
 *   build()  each (re)send: v2 (100 B, the stored cookie or zeros) or v1 (84 B);
 *   on_reply() every server datagram while the request is pending: a COOKIE for its nonce is
 *            stored and returns GDX_RV_CK_RESEND (send build() again at once, same nonce);
 *   on_error() an ERROR for the request: GDX_RV_CK_RETRY_V2 = do not fail, resend (auto only).
 * AUTO: v2 first; two v2 sends with nothing at all heard back from the server (a server before
 * 2.0.0 drops version 2 as junk) fall back to v1 for the rest of the session; a v1 request then
 * refused with "server needs GXRV v2" (the lossy-first-second case against a requiring server)
 * goes back to v2. A server that has answered in version 2 once is never given up on. */
enum {
    GDX_RV_COOKIE_MODE_V1 = 0,
    GDX_RV_COOKIE_MODE_V2 = 1,
    GDX_RV_COOKIE_MODE_AUTO = 2,
};
enum {
    GDX_RV_CK_NONE = 0,
    GDX_RV_CK_RESEND = 1,   /* on_reply: a fresh cookie, resend now */
    GDX_RV_CK_FALLBACK = 2, /* build: auto fell back to v1 (this send is v1) */
    GDX_RV_CK_RETRY_V2 = 3, /* on_error: auto back to v2, resend now */
};
#define GDX_RV_COOKIE_FALLBACK_TRIES 2
#define GDX_RV_COOKIE_MAX_FAST_RESENDS 4 /* immediate resends per request (a cookie that keeps failing waits for the 1 s retry) */
/* The detail a requiring server puts in a v1 request's BUILD_MISMATCH (gdx_rendezvous.py
 * NEEDS_V2_DETAIL, test_rendezvous.py asserts they match). No spaces: every card before this one
 * drops an ERROR detail with a space in it, and shows "version mismatch: host runs <detail>". */
#define GDX_RV_NEEDS_V2_DETAIL "newer-version(GXRV-v2):PLEASE-UPDATE"

typedef struct GdxRvCookie {
    int mode;           /* GDX_RV_COOKIE_MODE_* */
    int fellBack;       /* auto: v2 went unanswered this session, requests are v1 */
    int serverV2;       /* the server answered in version 2: auto never falls back */
    int useV2;          /* the pending request's version */
    int haveCookie;
    int v2Unanswered;   /* v2 sends of the pending request with nothing heard back */
    int fastResends;
    uint32_t nonce;
    uint8_t cookie[GDX_RV_COOKIE_LEN];
    uint32_t firstSendMs; /* the pending request's first send */
    uint32_t lastSendMs;
    uint32_t cookieRttMs; /* last send -> its COOKIE */
    uint32_t cookies;     /* COOKIEs taken for the pending request */
} GdxRvCookie;

/* "0" | "1" | "auto" (also "v1" / "v2"); anything else, or empty: auto. */
int gdx_rv_cookie_mode_parse(const char* s);
const char* gdx_rv_cookie_mode_name(int mode);
void gdx_rv_cookie_init(GdxRvCookie* c, int mode);
/* allowV2 = 0: this request is v1 whatever the mode (a console-hosted LAN room speaks v1). */
void gdx_rv_cookie_begin(GdxRvCookie* c, uint32_t nonce, uint32_t nowMs, int allowV2);
/* req->nonce must be the begin() nonce. *event (may be NULL): GDX_RV_CK_FALLBACK or NONE. */
int gdx_rv_cookie_build(GdxRvCookie* c, int type, const GdxMpRvRequest* req, uint32_t nowMs, uint8_t* out, int cap,
                        int* event);
int gdx_rv_cookie_on_reply(GdxRvCookie* c, const uint8_t* pkt, int len, uint32_t nowMs);
int gdx_rv_cookie_on_error(GdxRvCookie* c, const GdxMpRvError* e);

/* ---- GXRV, the server's half (the console-hosted LAN room, port/gdx_mp_lan.h) --------------- */
/* A CREATE / JOIN request, validated as gdx_rendezvous.py's _parse_request does: returns the
 * type, or -1. An empty / blank name becomes "PLAYER"; a non-private LAN address is zeroed. */
int gdx_rv_parse_request(const uint8_t* pkt, int len, GdxMpRvRequest* out);
/* The auth prefix of KEEPALIVE / LEAVE / RELAY / PUNCH_REQ / LOCK: returns the type, or -1. */
typedef struct GdxMpRvAuth {
    int type;
    char code[GDX_MPO_CODE_LEN + 1];
    uint8_t slot;
    uint8_t arg;
    uint64_t memberKey;
    uint32_t rosterVersion; /* KEEPALIVE only */
    uint32_t sendMs;        /* KEEPALIVE only */
} GdxMpRvAuth;
int gdx_rv_parse_auth(const uint8_t* pkt, int len, GdxMpRvAuth* out);
int gdx_rv_build_welcome(const GdxMpRvWelcome* w, uint8_t* out, int cap);
/* Members in slot order; member[s].present marks who is in. isHost is set for slot 0. */
int gdx_rv_build_roster(const GdxMpRvRoster* r, uint8_t* out, int cap);
int gdx_rv_build_error(uint32_t nonce, int code, const char* detail, uint8_t* out, int cap);
int gdx_rv_build_ka_ack(uint32_t rosterVersion, uint32_t echoMs, uint8_t* out, int cap);
int gdx_rv_build_punch_now(const char* code, int fromSlot, uint8_t* out, int cap);
int gdx_mpo_is_private_v4(const uint8_t ip[4]);

/* ---- room codes -------------------------------------------------------------------------------- */
/* 5 characters of "ABCDEFGHJKLMNPQRSTUVWXYZ23456789" (no I, O, 0, 1). Normalises in place
 * (lower -> upper case only; look-alikes are not guessed). Returns 1 if valid. */
int gdx_mpo_code_normalise(char* code);
extern const char gdx_mpo_code_alphabet[33];

/* Names: up to 12 of [A-Za-z0-9 _.-]; anything else becomes '_'; empty -> "PLAYER". */
void gdx_mpo_name_sanitise(const char* in, char* out /* GDX_MPO_NAME_LEN + 1 */);

/* "host:port" -> endpoint (dotted IPv4 only; a 3DS has no resolver we want to block on). 1 = ok. */
int gdx_mpo_parse_hostport(const char* s, GdxMpEndpoint* out, uint16_t defaultPort);

/* ---- GXLB (lobby) ------------------------------------------------------------------------------ */
#define GDX_LB_VERSION 1
enum {
    GDX_LB_HELLO = 1,
    GDX_LB_START = 2,
    GDX_LB_ABORT = 3,
    GDX_LB_RM_POLL = 4, /* coordinator -> racers: player p is being removed, freeze it, report */
    GDX_LB_RM_HAVE = 5, /* racer -> coordinator: how many of p's pads I hold, and the newest */
    GDX_LB_RM_SET = 6,  /* coordinator -> racers: p is neutral from tick T on (+ p's pads up to T) */
};
enum {
    GDX_LB_STATE_LOBBY = 0,
    GDX_LB_STATE_LAUNCHING = 1,
    GDX_LB_STATE_RACING = 2,
};
enum {
    GDX_LB_ABORT_QUIT = 1,
    GDX_LB_ABORT_DESYNC = 2,
    GDX_LB_ABORT_PEER_LOST = 3,
    GDX_LB_ABORT_SUSPEND = 4,
    GDX_LB_ABORT_LAUNCH_FAILED = 5,
};
#define GDX_LB_HEADER_BYTES 28
#define GDX_LB_HELLO_BYTES 128
#define GDX_LB_START_BYTES 116
#define GDX_LB_ABORT_BYTES 40
#define GDX_LB_RM_PADS 32 /* >= 2 * GDX_MP_REDUNDANCY: covers any spread in what racers hold */
#define GDX_LB_RM_BYTES (44 + 4 * GDX_LB_RM_PADS)
#define GDX_LB_NO_RTT 0xFFFFu

typedef struct GdxMpLbHeader {
    uint8_t type;
    uint8_t from;
    uint8_t to;
    uint64_t secret;
    uint32_t sendMs;
    uint32_t echoMs;
    uint16_t holdMs;
} GdxMpLbHeader;

typedef struct GdxMpLbHello {
    uint16_t proto;
    uint8_t state;
    uint8_t ready;
    uint32_t contentHash;
    uint32_t startGen;
    uint8_t machine;
    uint8_t skin;
    uint8_t engine; /* 0..100 = the machine-settings slider */
    uint8_t bot;    /* test lever: this player is driven by the game's CPU (synced via START) */
    uint8_t directMask;
    uint8_t removedMask; /* racing: the players whose RM_SET we applied (the coordinator's ack) */
    uint8_t model;       /* GDX_MPO_MODEL_* */
    uint16_t rttMs[GDX_MPO_MAX_SLOTS];
    char build[GDX_MPO_BUILD_LEN + 1];
    char name[GDX_MPO_NAME_LEN + 1];
    uint32_t simAbi; /* MP30: the sim-ABI word; must equal ours */
    /* stockpick (proto 6) */
    uint8_t picking;    /* GDX_LB_PICK_*: this console is in a stock picker right now */
    uint8_t pickRound;  /* the host: its current machine-select round (0 = none yet); a joiner: the
                           round it is in (machine/skin/engine are its pick for it when picked) */
    uint8_t courseFor;  /* the course courseKey describes: the host's pick (the host: its own) */
    uint8_t picked;     /* 1 = machine/skin/engine are a confirmed pick for pickRound */
    uint32_t courseKey; /* gdx_mpo_course_key of courseFor on this console; 0 = it does not have it */
    uint32_t engineBits; /* the stock slider's exact value (an IEEE float, 0..1); engine is it rounded */
    uint32_t pickMsLeft; /* the host: ms left in the round's machine step (0 = over); NO_ROUND = none */
} GdxMpLbHello;

typedef struct GdxMpStartPlayer {
    uint8_t slot;
    uint8_t machine; /* character 0..29 */
    uint8_t skin;    /* 0..3 */
    uint8_t engine;  /* 0..100 */
    uint32_t engineBits; /* stockpick: the exact gPlayerEngine float the race pins (0..1) */
    uint16_t epoch;      /* proto 8: the roster's epoch for the slot at START (the console it means) */
} GdxMpStartPlayer;

typedef struct GdxMpLbStart {
    uint32_t gen;
    uint8_t numPlayers;
    uint8_t course;
    uint8_t delay;
    uint8_t hashEvery;
    uint8_t vsCom;
    uint8_t handicap;
    uint8_t vsSlot;
    uint8_t difficulty;
    uint32_t seed1, mask1, seed2, mask2;
    uint64_t token;
    GdxMpStartPlayer player[GDX_MPO_MAX_SLOTS];
    uint8_t botMask; /* bit = player index */
    uint8_t laps;    /* 1..9 */
    uint32_t simAbi; /* MP30: the host's sim-ABI word; a joiner refuses a START that is not its own */
    uint8_t racers;  /* MP30: VS COM machine count, GDX_MPO_MIN_RACERS..GDX_MPO_MAX_RACERS (0 = stock 4) */
    uint8_t ramMask; /* MP30: bit = player index: a test bot in ram mode ([net] mp_bot = 2); within botMask */
    uint32_t courseKey; /* stockpick: the course's key on the host; a joiner whose own differs refuses */
} GdxMpLbStart;

typedef struct GdxMpLbRemove {
    uint32_t gen;
    uint8_t player; /* player index (not slot) being removed */
    uint8_t reason; /* GDX_LB_ABORT_*: why (left, connection lost, suspended) */
    uint8_t count;  /* pads carried (0 for POLL) */
    uint32_t tick;  /* HAVE: how many of p's pads the sender holds; SET: T; POLL: 0 */
    uint32_t first; /* tick of pads[0] */
    GdxMpPad pads[GDX_LB_RM_PADS];
} GdxMpLbRemove;

typedef struct GdxMpLbAbort {
    uint32_t gen;
    uint8_t reason;
    uint8_t culprit;
    uint32_t tick;
} GdxMpLbAbort;

#define GDX_MPO_STOCK_COURSES 24 /* JACK..JOKER, 6 each: every console has them */
/* stockpick: the Expansion Kit's DD cups (30..41, from the disk) race too, when every console holds
 * the same course (courseKey). Out: the EDIT cup (24..29, each console's own files -- see
 * gdx_mpo_course_raceable) and the X cup (48, generated). */
#define GDX_MPO_FIRST_DD_COURSE 30
#define GDX_MPO_MAX_COURSE 41
#define GDX_MPO_MAX_MACHINE 29
#define GDX_MPO_MIN_RACERS 4  /* MP30: the lobby's Machines option: 4 / 8 / 16 / 30 */
#define GDX_MPO_MAX_RACERS 30
#define GDX_MPO_MAX_DELAY 8   /* 2 * delay + 2 <= GDX_MP_REDUNDANCY (gdx_mp_lockstep.h) */

/* stockpick: HELLO.picking */
enum {
    GDX_LB_PICK_NONE = 0,
    GDX_LB_PICK_MACHINE = 1, /* in the stock MACHINE SELECT / MACHINE SETTINGS */
    GDX_LB_PICK_COURSE = 2,  /* the host, in the stock COURSE SELECT */
};
#define GDX_MPO_NO_COURSE_KEY 0u
#define GDX_LB_NO_ROUND 0xFFFFFFFFu /* HELLO.pickMsLeft: no machine step running */
#define GDX_MPO_PICK_MS 60000      /* every player's time for machine select + settings */
#define GDX_MPO_PICK_GRACE_MS 5000 /* the host waits this much longer for picks in flight */
/* The pick a player races when its console never reported one for the round (it dropped, or
 * joined after the machine step): Blue Falcon (Captain Falcon, character 0), its first colour, the
 * slider's centre. A console that runs out of time confirms its own cursor's machine instead. */
#define GDX_MPO_DEFAULT_MACHINE 0
#define GDX_MPO_DEFAULT_SKIN 0
#define GDX_MPO_DEFAULT_ENGINE 50
/* 1 for a course a multiplayer race can be on: the stock 24 and the DD cups. */
int gdx_mpo_course_raceable(int course);
/* The key of a stock course (0..23): the same on every console, never 0. 0 for any other. A disk
 * course's key is the console's own digest of the course it would load (gdx3ds_mpp_course_key). */
uint32_t gdx_mpo_stock_course_key(int course);
/* The engine slider as wire bits: the float's IEEE bits, and back (an invalid word -- NaN, out of
 * 0..1 -- reads as 0.5, the slider's centre). */
uint32_t gdx_mpo_engine_bits(float engine);
float gdx_mpo_engine_from_bits(uint32_t bits);
int gdx_mpo_engine_bits_valid(uint32_t bits);

int gdx_lb_build(const GdxMpLbHeader* h, const void* body, uint8_t* out, int cap);
/* Validates magic/version/size per type and the secret; fills *h and the body struct of that
 * type (body must point at the right struct for the returned type: pass a union). Returns the
 * type, or -1 (bad shape), -2 (wrong secret), -3 (bad field values). */
typedef union GdxMpLbBody {
    GdxMpLbHello hello;
    GdxMpLbStart start;
    GdxMpLbAbort abort;
    GdxMpLbRemove remove;
} GdxMpLbBody;
int gdx_lb_parse(const uint8_t* pkt, int len, uint64_t secret, GdxMpLbHeader* h, GdxMpLbBody* body);

/* ---- delay from RTT (design doc 8, "Latency budget") ------------------------------------------ */
/* D = ceil((rtt/2 + jitter + one tick of phase) / 16.7 ms), clamped to [2, GDX_MPO_MAX_DELAY]. */
int gdx_mpo_delay_from_rtt(uint32_t rttMs, uint32_t jitterMs);

/* ---- session digest ----------------------------------------------------------------------------- */
/* What every console must agree on before a pad means the same thing: build + content + the
 * START packet (players, picks, settings, seeds, token). Folded into GXMP's session field. */
uint32_t gdx_mpo_session_digest(const char* build, uint32_t contentHash, const GdxMpLbStart* s);

/* ---- link state (one per peer slot) ------------------------------------------------------------ */
enum {
    GDX_LINK_NONE = 0,     /* not in the roster */
    GDX_LINK_PUNCHING = 1, /* sending direct (every candidate) AND relayed */
    GDX_LINK_DIRECT = 2,   /* confirmed both ways: direct only */
    GDX_LINK_RELAY = 3,    /* punch gave up (or forced): relayed only, slow direct probe */
};
#define GDX_LINK_MAX_CAND 3 /* public, private, learned */
#define GDX_LINK_PUNCH_MS 6000
#define GDX_LINK_HEAR_MS 3000
#define GDX_LINK_PROBE_MS 1000

typedef struct GdxMpLink {
    int state;
    int forced;
    GdxMpEndpoint cand[GDX_LINK_MAX_CAND]; /* [0] public, [1] private, [2] learned */
    int candValid[GDX_LINK_MAX_CAND];
    uint32_t sinceMs;        /* entered the current state */
    uint32_t lastDirectRxMs; /* newest authenticated direct datagram from them */
    int heardDirect;         /* ever */
    int theyHearUs;          /* their newest HELLO's directMask has our bit */
    uint32_t theyHearUsMs;
    uint32_t lastProbeMs;
    uint32_t upgrades, downgrades;
} GdxMpLink;

void gdx_link_reset(GdxMpLink* l);
/* A (new) roster entry for this peer: candidate endpoints. forced = relay only. */
void gdx_link_set_candidates(GdxMpLink* l, const GdxMpEndpoint* pub, const GdxMpEndpoint* priv, int forced,
                             uint32_t nowMs);
/* An authenticated datagram arrived DIRECTLY from `src`: it becomes the learned candidate. */
void gdx_link_on_direct_rx(GdxMpLink* l, const GdxMpEndpoint* src, uint32_t nowMs);
/* Their HELLO said whether they hear us directly. */
void gdx_link_on_peer_report(GdxMpLink* l, int theyHearUs, uint32_t nowMs);
/* Advance timers (call once per frame). */
void gdx_link_tick(GdxMpLink* l, uint32_t nowMs);
/* 1 if we hear them directly right now (for our HELLO's directMask). */
int gdx_link_hear_direct(const GdxMpLink* l, uint32_t nowMs);
/* Where a datagram to this peer goes: fills out[] with direct endpoints (count returned) and
 * *viaRelay. `probe` = this is a lobby HELLO (RELAY state probes direct at GDX_LINK_PROBE_MS). */
int gdx_link_route(GdxMpLink* l, uint32_t nowMs, int probe, GdxMpEndpoint* out, int* viaRelay);
const char* gdx_link_state_name(int state);

/* ---- receive-side hygiene ---------------------------------------------------------------------- */
typedef struct GdxMpBucket {
    uint32_t ratePerSec;
    uint32_t burst;
    uint32_t tokensX1000;
    uint32_t stampMs;
    int primed;
} GdxMpBucket;
void gdx_mpo_bucket_init(GdxMpBucket* b, uint32_t ratePerSec, uint32_t burst);
int gdx_mpo_bucket_take(GdxMpBucket* b, uint32_t nowMs);

/* A small table of misbehaving source endpoints: after `limit` rejects inside a 10 s window a
 * source is blocked for 60 s. Bounded (oldest entry evicted). */
#define GDX_MPO_BLOCK_SLOTS 16
typedef struct GdxMpBlocklist {
    GdxMpEndpoint ep[GDX_MPO_BLOCK_SLOTS];
    uint32_t windowMs[GDX_MPO_BLOCK_SLOTS];
    uint32_t count[GDX_MPO_BLOCK_SLOTS];
    uint32_t blockedUntil[GDX_MPO_BLOCK_SLOTS];
    int used[GDX_MPO_BLOCK_SLOTS];
    uint32_t limit;
    uint32_t blocks;
} GdxMpBlocklist;
void gdx_mpo_block_init(GdxMpBlocklist* bl, uint32_t limit);
int gdx_mpo_block_is_blocked(const GdxMpBlocklist* bl, const GdxMpEndpoint* src, uint32_t nowMs);
void gdx_mpo_block_note_reject(GdxMpBlocklist* bl, const GdxMpEndpoint* src, uint32_t nowMs);

int gdx_mpo_endpoint_eq(const GdxMpEndpoint* a, const GdxMpEndpoint* b);
int gdx_mpo_endpoint_valid(const GdxMpEndpoint* e);

/* ---- content hash ------------------------------------------------------------------------------- */
/* FNV-1a over a zip's central directory, per member in directory order: its name, CRC-32 and
 * UNCOMPRESSED size. A cheap digest of an .o2r archive's CONTENT that reads ~the last 100 KB of the
 * file, not all of it, and that does not depend on how the members were compressed (method,
 * compressed size, offsets, timestamps and extra fields are left out), so two PCs whose zlib
 * deflates differently still agree. Folds into `h`. Returns 0 on success, -1 (no file), -2 (not a
 * zip / malformed). */
int gdx_mpo_zip_cd_hash(const char* path, uint32_t* h);

#ifdef __cplusplus
}
#endif

#endif /* GDX_MP_ONLINE_H */
