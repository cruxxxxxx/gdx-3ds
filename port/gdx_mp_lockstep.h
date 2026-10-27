/* port/gdx_mp_lockstep.h — MP spike: deterministic-lockstep input exchange (mp-spike track).
 *
 * The pure half of `[net] mp = host|join` (port/3ds/net/gdx3ds_mp.c is the 3DS half: config,
 * sockets, the poll hook). Nothing here touches a socket, a clock or a global: the caller
 * passes every timestamp, so the whole protocol is host-testable
 * (port/tests/gdx_mp_lockstep_host_tests.c, run by tools/ci-3ds.sh).
 *
 * MODEL. Every console runs the whole race. The only thing exchanged is each player's pad,
 * one per input tick (one gdx_controller_poll). The pad a console samples at tick t is
 * SCHEDULED for tick t + delay, so the network has `delay` ticks to deliver it before anyone
 * needs it; ticks 0..delay-1 are neutral on every console by construction. A console may run
 * tick t only once it holds every player's pad for t (gdx_mp_ls_ready), so all consoles feed
 * the game the identical pad sequence and, if the simulation is deterministic, reach identical
 * state. That claim is CHECKED, not assumed: every `hashEvery` ticks each console publishes
 * its [gstate] digest for that tick and every receiver compares it against its own — the
 * first mismatch is the desync receipt.
 *
 * WIRE FORMAT (little-endian, GDX_MP_HEADER_BYTES + 4 * count bytes, one datagram):
 *    0  'G' 'X' 'M' 'P'
 *    4  u8  version (GDX_MP_VERSION)
 *    5  u8  type    (GDX_MP_TYPE_INPUT)
 *    6  u8  from    (sender's player index)
 *    7  u8  count   (pads carried, <= GDX_MP_REDUNDANCY)
 *    8  u32 firstTick (tick of the first pad carried)
 *   12  u32 hashTick  (GDX_MP_NO_TICK = none yet)
 *   16  u32 hash
 *   20  u32 sendMs    (sender's clock, for RTT)
 *   24  u32 echoMs    (the sendMs of the newest packet the sender has received from echoFrom)
 *   28  u32 session   (compatibility digest: both ends must agree, or the packet is refused)
 *   32  u8  echoFrom  (0xFF = nothing to echo)
 *   33  u8  flags     (bit 0 GDX_MP_FLAG_ABORT: the sender has left the session)
 *   34  u8  wireSlot  (online: the sender's room slot + 1, checked by the relay and the
 *                      receiver; 0 = unused, the LAN lane)
 *   35  u8  reserved
 *   36  u32 known[GDX_MP_MAX_PLAYERS]  (the sender's contiguous-known count per player: an ACK)
 *   68  count x { u16 buttons, s8 stickX, s8 stickY }
 * Every packet repeats the sender's pads from the OLDEST tick any peer has not yet acknowledged
 * (their known[] for us), up to GDX_MP_REDUNDANCY of them, so a lost datagram costs nothing:
 * the next packet carries the same ticks again. No retransmit timer. A console pushes its pad for
 * lt + delay before it waits on tick lt, and lt can run to lt_peer + delay + 1 while that peer still
 * lacks our pad lt_peer, so the unacknowledged span is <= 2 * delay + 2 <= GDX_MP_REDUNDANCY and the
 * window always reaches it (gdx_mp_ls_init refuses a delay that breaks this). With 16 and delay 8
 * (18 > 16) a burst that lost every carrier of a tick stranded it: the race hung until the abort.
 * A star topology (UDS: the host relays) forwards packets verbatim; `from` says whose pads
 * they are. */
#ifndef GDX_MP_LOCKSTEP_H
#define GDX_MP_LOCKSTEP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GDX_MP_MAX_PLAYERS 8
#define GDX_MP_RING 256 /* power of two; >> any delay the protocol allows */
#define GDX_MP_MAX_DELAY 16
#define GDX_MP_REDUNDANCY 24 /* >= 2 * delay + 2 for every delay the session allows (8) */
#define GDX_MP_VERSION 1
#define GDX_MP_TYPE_INPUT 1
#define GDX_MP_HEADER_BYTES (36 + 4 * GDX_MP_MAX_PLAYERS)
#define GDX_MP_MAX_PACKET (GDX_MP_HEADER_BYTES + 4 * GDX_MP_REDUNDANCY)
#define GDX_MP_NO_TICK 0xFFFFFFFFu
#define GDX_MP_FLAG_ABORT 0x01u
/* Pad hygiene (mp-online): every pad entering the ring, local or remote, is clamped to what an
 * N64 pad can say -- the CONT_MASK buttons (+ GDX_MP_PAD_BGM_READY) and the port's -80..80 stick -- so a hostile peer
 * cannot hand the game a value no controller produces, and both ends clamp identically. */
#define GDX_MP_BUTTON_MASK 0xFF7Fu
#define GDX_MP_STICK_MAX 80
/* MP30: bit 6 is no N64 button (CONT_MASK leaves 0x00C0 unused). It rides in every exchanged pad
 * as "this console's BGM is loaded" (the Expansion Kit countdown hold, gdx3ds_autodrive.c), and
 * the session strips it before the game sees the pad. */
#define GDX_MP_PAD_BGM_READY 0x0040u

typedef struct GdxMpPad {
    uint16_t buttons;
    int8_t stickX;
    int8_t stickY;
} GdxMpPad;

typedef struct GdxMpPeerStats {
    uint32_t packets;
    uint32_t bytes;
    uint32_t newestSendMs;   /* their clock, echoed back to them */
    uint32_t rttLastMs;      /* newest RTT sample (includes up to one tick of hold) */
    uint32_t rttMaxMs;
    uint32_t rttSumMs;
    uint32_t rttSamples;
    uint32_t hashesChecked;
} GdxMpPeerStats;

typedef struct GdxMpLockstep {
    int numPlayers;
    int localPlayer;
    int delay;
    int hashEvery;
    uint32_t session;
    /* Per player: the pad ring, the tick each slot holds (GDX_MP_NO_TICK = empty), and the
     * count of CONTIGUOUS ticks known from 0 (ticks [0, known) are all present). */
    GdxMpPad pads[GDX_MP_MAX_PLAYERS][GDX_MP_RING];
    uint32_t slotTick[GDX_MP_MAX_PLAYERS][GDX_MP_RING];
    uint32_t known[GDX_MP_MAX_PLAYERS];
    /* Hash rings: our own digest per tick, and the newest digest each peer published. */
    uint32_t localHash[GDX_MP_RING];
    uint32_t localHashTick[GDX_MP_RING];
    uint32_t remoteHash[GDX_MP_MAX_PLAYERS][GDX_MP_RING];
    uint32_t remoteHashTick[GDX_MP_MAX_PLAYERS][GDX_MP_RING];
    uint32_t newestLocalHashTick;
    /* First desync (sticky). */
    int desynced;
    int desyncPeer;
    uint32_t desyncTick;
    uint32_t desyncLocal;
    uint32_t desyncRemote;
    /* Packets refused: bad magic/version/size, wrong session, own echo, out of range. */
    uint32_t rejected;
    uint32_t sessionRejected;
    GdxMpPeerStats peer[GDX_MP_MAX_PLAYERS];
    int newestFrom; /* the player whose sendMs we echo next (-1 = none) */
    /* acked[q] = how many of OUR pads player q has told us it holds contiguously. */
    uint32_t acked[GDX_MP_MAX_PLAYERS];
    /* mp-online: written into header bytes 33/34 of every packet we build. */
    uint8_t txFlags;
    uint8_t wireSlot;
    /* The newest header flags each peer sent, and the wireSlot it claimed (0 = none). */
    uint8_t peerFlags[GDX_MP_MAX_PLAYERS];
    uint8_t peerWireSlot[GDX_MP_MAX_PLAYERS];
    uint32_t padsClamped;
    /* mp-timesync: the nowMs of the ingest that last advanced known[p] (its newest pad's arrival;
     * receipts and pacing only, never part of the pad schedule). */
    uint32_t knownAtMs[GDX_MP_MAX_PLAYERS];
    /* mp-online player removal (a console that dropped out mid-race): from removedAt[p] on,
     * player p's pad is neutral on every console and nobody waits for it; frozen[p] = we stopped
     * taking p's packets (our known[p] is reported to the coordinator and must not move). */
    uint32_t removedAt[GDX_MP_MAX_PLAYERS]; /* GDX_MP_NO_TICK = racing */
    uint8_t frozen[GDX_MP_MAX_PLAYERS];
    uint32_t frozenRejected;
} GdxMpLockstep;

/* Returns 0 on success, -1 on an out-of-range argument (the state is then unusable). */
int gdx_mp_ls_init(GdxMpLockstep* ls, int numPlayers, int localPlayer, int delay, int hashEvery,
                   uint32_t session);

/* The local pad sampled at input tick `tick`; it is scheduled for tick + delay. Call exactly
 * once per tick, in order. */
void gdx_mp_ls_push_local(GdxMpLockstep* ls, uint32_t tick, GdxMpPad pad);

/* 1 when every player's pad for `tick` is present. */
int gdx_mp_ls_ready(const GdxMpLockstep* ls, uint32_t tick);

/* The lowest player index whose pad for `tick` is still missing, -1 if none (receipts). */
int gdx_mp_ls_first_missing(const GdxMpLockstep* ls, uint32_t tick);

/* Copies every player's pad for `tick` into out[0..numPlayers-1]. Only valid when ready. */
void gdx_mp_ls_get(const GdxMpLockstep* ls, uint32_t tick, GdxMpPad* out);

/* Serialises the local player's unacknowledged pads (from the oldest tick a peer still lacks,
 * up to GDX_MP_REDUNDANCY) plus the newest published local hash. Returns the byte count, 0 when there
 * is nothing to send yet or cap is too small. */
int gdx_mp_ls_build_packet(GdxMpLockstep* ls, uint32_t nowMs, uint8_t* out, int cap);
/* 1 = a live peer has not acknowledged every pad we scheduled (what our next packet would carry is
 * still news to someone); 0 = it would only be a heartbeat. Send-timing only, no state change. */
int gdx_mp_ls_unacked(const GdxMpLockstep* ls);

/* Parses one datagram. Returns the sender's player index, or -1 when refused (counted in
 * `rejected`). Stores the pads, advances `known`, records the RTT sample and checks the
 * carried hash. */
int gdx_mp_ls_ingest(GdxMpLockstep* ls, const uint8_t* pkt, int len, uint32_t nowMs);

/* Our own [gstate] digest after running `tick`. Published when tick % hashEvery == 0 and
 * compared against whatever peers already sent for that tick. Returns 1 if this call
 * detected the (first) desync. */
int gdx_mp_ls_note_local_hash(GdxMpLockstep* ls, uint32_t tick, uint32_t hash);

/* 1 when packet `pkt` (len bytes) is a well-formed GDX MP header, whatever its session. Lets a
 * relay forward without parsing the payload. */
int gdx_mp_ls_packet_from(const uint8_t* pkt, int len);

/* The wireSlot byte (34) of a well-formed packet, -1 otherwise. */
int gdx_mp_ls_packet_wire_slot(const uint8_t* pkt, int len);

/* ---- player removal (mp-online) ------------------------------------------------------------
 * The coordinator (the lowest racing room slot) removes a player that left: it FREEZES p
 * everywhere (each console stops taking p's packets and reports how many of p's pads it holds,
 * plus the newest of them), takes T = the highest count anyone holds -- no console can have run
 * tick T or later, since that needs p's pad for it -- fills every console's ring up to T from the
 * pads collected, and has everyone apply REMOVE(p, T): p is neutral from T on, everywhere. */
void gdx_mp_ls_freeze(GdxMpLockstep* ls, int player);
/* Stores pads first..first+count-1 of `player` that we lack (even when frozen); returns known[p]. */
uint32_t gdx_mp_ls_fill(GdxMpLockstep* ls, int player, uint32_t first, const GdxMpPad* pads, int count);
/* Copies up to `max` of player's newest contiguous pads, ending at known[p]; *first = the tick of
 * out[0]. Returns the count. */
int gdx_mp_ls_newest_pads(const GdxMpLockstep* ls, int player, int max, uint32_t* first, GdxMpPad* out);
/* Applies REMOVE(player, tick). 0 = done; -1 = bad argument (the local player, out of range);
 * -2 = we lack some of p's pads before `tick` (fill first); -3 = already removed at another tick. */
int gdx_mp_ls_remove(GdxMpLockstep* ls, int player, uint32_t tick);
/* 1 when player p is removed and `tick` is at or past its removal. */
int gdx_mp_ls_is_removed(const GdxMpLockstep* ls, int player, uint32_t tick);

/* Clamp a pad to GDX_MP_BUTTON_MASK / +-GDX_MP_STICK_MAX. Returns 1 if anything changed. */
int gdx_mp_ls_clamp_pad(GdxMpPad* pad);

#ifdef __cplusplus
}
#endif

#endif /* GDX_MP_LOCKSTEP_H */
