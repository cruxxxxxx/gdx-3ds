/* port/gdx_mp_drop.h — multiplayer drop-out: "player p removed at tick T" (pure, host-testable).
 *
 * WHAT IT GUARANTEES. When a player leaves (closes the app, loses the link, stops answering),
 * every remaining console keeps racing in lockstep: from one agreed lockstep tick T on, the
 * removed player's pad is NEUTRAL on every console, and (game side, gdx3ds_mp.c) its machine
 * auto-retires at the first race tick >= T. Every console reaches the same T with the same
 * pads before it, so the simulation stays identical -- the per-tick state digest keeps
 * checking that.
 *
 * WHY TWO PHASES. A console may run tick t only once it holds every player's pad for t, so
 * no two consoles disagree about a pad they USED. But they may HOLD different amounts of the
 * leaver's pads: in a star (UDS, udp-lan: the host relays) a joiner only ever holds what the
 * host relayed, but in a mesh (mp-online) a joiner can hold, and may already have run with, a
 * pad the host never received. So the host, which alone decides removals, asks first:
 *
 *   host            FREEZE(p)  -> every remaining peer       "stop taking p's pads"
 *   each peer       DROP_ACK(p, known_p, its last <=16 pads of p)
 *   host            T = max(every known_p, its own), after merging the tails it was sent
 *   host            COMMIT(p, T, p's pads [T-16, T))  -> every remaining peer
 *   each peer       fills its gap up to T from the commit, ticks >= T are neutral, ACKs
 * The host resends FREEZE / COMMIT every GDX_MP_DROP_RESEND_MS until every remaining peer
 * acknowledged that phase; a peer answers every copy. A peer that never answers is itself
 * being removed (the session layer times it out) and is no longer waited for. The spread of
 * known_p across consoles is <= 2 * delay + 2 <= GDX_MP_REDUNDANCY, so a 24-pad tail always
 * bridges it; if it ever did not, the console that cannot fill reports `fatal` instead of
 * guessing (the session ends with a receipt; it can never desync silently).
 *
 * LEAVE. A console that is quitting says so (DROP type LEAVE, sent a few times): a joiner's
 * LEAVE makes the host start removing it at once instead of after a timeout; the host's LEAVE
 * ends the session for everyone (no host migration: the host is the UDS network itself).
 * mp-online's GXMP header flag GDX_MP_FLAG_ABORT (byte 33 bit 0, "the sender has left the
 * session") means the same thing and is honoured as a LEAVE (gdx_mp_drop_note_input_flags).
 *
 * WIRE FORMAT (little-endian, GDX_MP_DROP_HEADER_BYTES + 4 * count bytes, one datagram). The
 * first 8 bytes match GXMP INPUT so a relay can route without parsing:
 *    0  'G' 'X' 'M' 'P'
 *    4  u8  version (GDX_MP_VERSION)
 *    5  u8  type    (GDX_MP_TYPE_DROP / _DROP_ACK / _LEAVE)
 *    6  u8  from    (sender's player index)
 *    7  u8  count   (pads carried, <= GDX_MP_REDUNDANCY)
 *    8  u32 session (the same compatibility digest as INPUT)
 *   12  u8  player  (the player being removed; LEAVE: the sender)
 *   13  u8  phase   (DROP/ACK: GDX_MP_DROP_FREEZE or _COMMIT; LEAVE: 0)
 *   14  u8  removedMask (the sender's removed players, informational)
 *   15  u8  wireSlot (mp-online: the sender's room slot + 1; 0 = LAN/UDS)
 *   16  u32 tick    (COMMIT: T; ACK of FREEZE: the sender's known count of `player`;
 *                    ACK of COMMIT: T; LEAVE: the sender's next lockstep tick)
 *   20  u32 firstTick (tick of the first pad carried)
 *   24  u8  reason  (GDX_MP_DROP_R_*)
 *   25  u8[3] reserved (0)
 *   28  count x { u16 buttons, s8 stickX, s8 stickY }
 *
 * TRUST. As with INPUT, `from` is authoritative only after the transport bound it (udp: the
 * host learned the endpoint; UDS: the node). DROP is accepted only from player 0, ACK only by
 * player 0, every count and tick is range-checked, and carried pads are only ever used for
 * ticks inside the live window and below T. */
#ifndef GDX_MP_DROP_H
#define GDX_MP_DROP_H

#include "gdx_mp_lockstep.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GDX_MP_TYPE_DROP 2
#define GDX_MP_TYPE_DROP_ACK 3
#define GDX_MP_TYPE_LEAVE 4
#define GDX_MP_DROP_HEADER_BYTES 28
#define GDX_MP_DROP_MAX_PACKET (GDX_MP_DROP_HEADER_BYTES + 4 * GDX_MP_REDUNDANCY)
#define GDX_MP_DROP_RESEND_MS 50
#define GDX_MP_DROP_DEST_ALL (-1) /* == GDX_MP_PEER_ANY (gdx3ds_mp_transport.h): every other player */

enum {
    GDX_MP_DROP_FREEZE = 1,
    GDX_MP_DROP_COMMIT = 2,
};

enum {
    GDX_MP_DROP_R_TIMEOUT = 1, /* nothing heard for [net] mp_drop_ms */
    GDX_MP_DROP_R_LEFT = 2,    /* it said goodbye (LEAVE, or mp-online's ABORT flag) */
    GDX_MP_DROP_R_LINK = 3,    /* the medium says it is gone (UDS node left the network) */
    GDX_MP_DROP_R_FORCED = 4,  /* test lever */
};

typedef struct GdxMpDrop {
    int numPlayers;
    int localPlayer;
    int isHost;
    /* Removal state, the same on every console once committed. */
    uint32_t removedAt[GDX_MP_MAX_PLAYERS]; /* GDX_MP_NO_TICK = still in the session */
    uint8_t reason[GDX_MP_MAX_PLAYERS];
    uint8_t removedMask;
    uint8_t frozenMask; /* pads from these players are refused (pending or removed) */
    /* Host: the removal in progress, and the ones queued behind it. */
    int pending;           /* -1 = none */
    int phase;             /* GDX_MP_DROP_FREEZE / _COMMIT while pending */
    uint8_t queueMask;
    uint8_t queueReason[GDX_MP_MAX_PLAYERS];
    uint8_t ackMask;
    uint32_t reportKnown[GDX_MP_MAX_PLAYERS];
    uint32_t commitTick;
    uint32_t lastSendMs;
    int sentOnce;
    /* Peer: the acknowledgement we owe (answered once per DROP received). */
    int ackDue;            /* -1 = none, else the player the ACK is about */
    int ackPhase;
    uint32_t ackTick;
    /* Events for the session layer. */
    int selfRemoved;       /* the host removed US */
    int hostLeft;          /* the host said LEAVE: the session is over */
    int fatal;             /* a gap the tails could not bridge (never seen; see the header) */
    uint8_t newCommits;    /* players committed since the session layer last cleared this */
    uint32_t rejected;
    uint32_t commits;
    uint32_t bridged; /* host: FREEZE acks that reported more of the leaver than we held */
} GdxMpDrop;

/* Bound to an initialised lockstep state (numPlayers/localPlayer come from it). */
void gdx_mp_drop_init(GdxMpDrop* d, const GdxMpLockstep* ls);

/* Host only: start removing player p. 1 = started or queued, 0 = nothing to do (not the host,
 * p is the host / out of range / already removed or queued). */
int gdx_mp_drop_begin(GdxMpDrop* d, GdxMpLockstep* ls, int player, int reason);

/* 1 when packets from `from` must be refused (pending removal or removed). */
int gdx_mp_drop_refuse_from(const GdxMpDrop* d, int from);

/* 1 when player p's pads are synthetic (neutral) at lockstep tick `tick`. */
int gdx_mp_drop_is_removed(const GdxMpDrop* d, int player, uint32_t tick);

/* The next protocol datagram this console must send now (host: FREEZE/COMMIT resends; peer:
 * the ACK it owes). Returns its length (0 = nothing now); *dest = GDX_MP_DROP_DEST_ALL or a player.
 * Call until it returns 0 (it returns each pending datagram once per call). */
int gdx_mp_drop_service(GdxMpDrop* d, GdxMpLockstep* ls, uint32_t nowMs, uint8_t* out, int cap, int* dest);

/* A LEAVE from this console (the session layer sends it a few times, then stops). */
int gdx_mp_drop_build_leave(const GdxMpDrop* d, const GdxMpLockstep* ls, uint32_t nextTick, int reason,
                            uint8_t* out, int cap);

/* The DROP-family type (GDX_MP_TYPE_DROP/_DROP_ACK/_LEAVE) of a well-formed datagram, whatever
 * its session, or -1. */
int gdx_mp_drop_packet_type(const uint8_t* pkt, int len);

/* Parses one DROP-family datagram. 1 = taken, -1 = refused (counted in `rejected`). */
int gdx_mp_drop_ingest(GdxMpDrop* d, GdxMpLockstep* ls, const uint8_t* pkt, int len, uint32_t nowMs);

/* mp-online interop: an INPUT packet whose header flags (byte 33) carry GDX_MP_FLAG_ABORT
 * (bit 0) from a non-host player starts its removal on the host, as a LEAVE would; from the
 * host it ends the session (hostLeft). Call with any well-formed INPUT packet. */
void gdx_mp_drop_note_input_flags(GdxMpDrop* d, GdxMpLockstep* ls, const uint8_t* pkt, int len);

/* Before asking gdx_mp_ls_ready(tick): writes the neutral pads of every removed player for
 * the ticks the window needs and stops the packet window from waiting on their acks. */
void gdx_mp_drop_fill(GdxMpDrop* d, GdxMpLockstep* ls, uint32_t tick);

/* Players still in the session (bit per player, us included). */
uint32_t gdx_mp_drop_live_mask(const GdxMpDrop* d);

#ifdef __cplusplus
}
#endif

#endif /* GDX_MP_DROP_H */
