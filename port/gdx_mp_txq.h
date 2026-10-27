/* port/gdx_mp_txq.h — the network service threads' queues and send scheduler (pure, host-tested by
 * port/tests/gdx_mp_txq_host_tests.c). Users: the UDS service thread (port/3ds/net/gdx3ds_mp_uds.c)
 * and the socket service thread (port/3ds/net/gdx3ds_mp_socthr.c).
 *
 * Why: on the Old 3DS one udsSendTo blocks the caller 8-12 ms and one udsPullPacket ~2 ms (hardware
 * 4bfb302, [mpipc] uds_send avg 8771 us, uds_pull avg 2009 us), and over WiFi one sendto 3.3-6.1 ms
 * and one recvfrom 1.5-2.2 ms (hardware online1, ~15 ms of every race frame). The service threads
 * make those calls; the main thread only copies datagrams into and out of these rings.
 *
 * GdxMpRing: a lock-free single-producer / single-consumer ring of fixed-size datagram slots.
 *   One thread pushes, one other thread pops; no locks, no allocation. Push fails (returns 0) when
 *   the ring is full; the caller counts the drop (a datagram network loses datagrams anyway).
 *   The indices are free-running uint32 counters published with release stores and read with
 *   acquire loads, so a slot's bytes are visible before its index moves.
 *
 * GdxMpTxSched: the consumer side's send plan for lockstep INPUT packets, keyed by
 *   (destination, stream). A destination is a UDS node id or a packed IPv4 endpoint; a stream is
 *   whose pads the packet carries (the lockstep `from`, plus for a relayed copy the relay's slot
 *   mask). A newer INPUT packet of one stream to one destination supersedes the older one: it
 *   carries every pad from the lowest pad any peer has not acked up to the newest
 *   (port/gdx_mp_lockstep.c gdx_mp_ls_build_packet), so the older copy says nothing new. Only the
 *   newest one per key is kept ("coalesced" counts the ones replaced), and it is sent no sooner
 *   than minMs after the previous one of that key (0 = at once). A star host relaying another
 *   player's packets verbatim (UDS, 3+ players) sends a different stream to the same node: those
 *   are never coalesced with its own (before the key had a stream, the host's own INPUT could
 *   replace a relayed one). Every other datagram (lobby, drop protocol, RTT) is sent at once, in
 *   order, and never coalesced. With every entry pending the offer is refused and the caller sends
 *   at once. Lockstep semantics are unchanged: the receiver sees a subset of the packets it would
 *   have seen, each newer than the last of its stream, with the same pads.
 */
#ifndef GDX_MP_TXQ_H
#define GDX_MP_TXQ_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GDX_MP_TXQ_PAYLOAD 1480 /* >= UDS_DATAFRAME_MAXSIZE (1478) */
#define GDX_MP_TXQ_INPUT_MAX 256 /* an INPUT packet: 68 B header + 4 B per pad, <= 24 pads */
#define GDX_MP_TXQ_NODES 17     /* UDS node ids 1..16 (index 0 unused) */
#define GDX_MP_TXQ_KEYS 32      /* (destination, stream) pairs the scheduler tracks at once */

typedef struct GdxMpDgram {
    uint16_t node; /* UDS node id: destination (tx) or source (rx) */
    uint16_t len;
    uint32_t stream; /* socket tx: the coalescing stream (0 = send at once, in order) */
    uint64_t dest;   /* socket: the packed endpoint, destination (tx) or source (rx) */
    uint8_t data[GDX_MP_TXQ_PAYLOAD];
} GdxMpDgram;

typedef struct GdxMpRing {
    GdxMpDgram* slot;
    uint32_t cap;           /* a power of two */
    volatile uint32_t head; /* producer: next slot to write (free-running) */
    volatile uint32_t tail; /* consumer: next slot to read (free-running) */
} GdxMpRing;

/* `cap` must be a power of two; `slots` holds cap entries. */
void gdx_mp_ring_init(GdxMpRing* r, GdxMpDgram* slots, uint32_t cap);
/* Producer: copy one datagram in. 1 = queued, 0 = full or len out of range. */
int gdx_mp_ring_push(GdxMpRing* r, uint16_t node, const void* data, int len);
/* The same with a socket destination / source and a coalescing stream (node = 0). */
int gdx_mp_ring_push_to(GdxMpRing* r, uint64_t dest, uint32_t stream, const void* data, int len);
/* Consumer: the oldest queued datagram, or NULL. Valid until gdx_mp_ring_release. */
const GdxMpDgram* gdx_mp_ring_peek(GdxMpRing* r);
void gdx_mp_ring_release(GdxMpRing* r);
/* Either side: datagrams queued now (a snapshot). */
uint32_t gdx_mp_ring_count(const GdxMpRing* r);
/* Only while neither side runs: drop everything. */
void gdx_mp_ring_reset(GdxMpRing* r);

/* 1 = a lockstep INPUT packet (magic "GXMP"-family + GDX_MP_TYPE_INPUT), the only kind that may
 * be coalesced. */
int gdx_mp_txq_is_input(const void* data, int len);

typedef struct GdxMpTxEntry {
    uint64_t dest;
    uint32_t stream; /* 0 = a free entry */
    uint32_t lastMs; /* when this key last sent */
    uint16_t len;
    uint8_t pending;
    uint8_t sentOnce;
    uint8_t repeatsLeft; /* steady cadence: sends of this packet still allowed (gdx_mp_txsched_rearm) */
    uint8_t data[GDX_MP_TXQ_INPUT_MAX];
} GdxMpTxEntry;

typedef struct GdxMpTxSched {
    uint32_t minMs;
    GdxMpTxEntry e[GDX_MP_TXQ_KEYS];
    uint32_t coalesced; /* INPUT packets replaced by a newer one before they were sent */
    uint32_t refused;   /* offers refused with every entry pending (the caller sent them at once) */
    uint32_t repeats;   /* sends of an unchanged packet re-armed by gdx_mp_txsched_rearm */
} GdxMpTxSched;

void gdx_mp_txsched_init(GdxMpTxSched* s, uint32_t minMs);
/* An INPUT packet of `stream` (!= 0) for `dest`: it replaces that key's pending one. 0 = refused
 * (bad stream/len, or every entry pending): send it now. */
int gdx_mp_txsched_offer(GdxMpTxSched* s, uint64_t dest, uint32_t stream, const void* data, int len);
/* The next pending INPUT that is due at `nowMs` (copies it out with its destination, clears it,
 * notes the send time): 1, or 0 when none is due. `*waitMs` (may be NULL) gets the time until the
 * earliest pending one is due (UINT32_MAX when nothing is pending). */
int gdx_mp_txsched_take(GdxMpTxSched* s, uint32_t nowMs, uint64_t* dest, uint8_t* out, uint16_t* outLen,
                        uint32_t* waitMs);
/* UDS: an INPUT packet for `node` (1..16), its stream the packet's `from` (byte 6). */
/* As gdx_mp_txsched_offer, and the packet may go out again up to `repeats` more times on the
 * steady cadence (gdx_mp_txsched_rearm) while nothing newer replaces it. A re-offer of the same
 * content (an INPUT equal but for its sendMs / echoMs words, bytes 20-27: a resend while the
 * peers are silent) earns no repeats, so a long stall backs off to the caller's own resend rate. */
int gdx_mp_txsched_offer_repeat(GdxMpTxSched* s, uint64_t dest, uint32_t stream, const void* data, int len,
                                int repeats);
/* Steady cadence: every sent entry with repeats left whose last send is `repeatMs` old becomes
 * pending again. Returns how many it re-armed; *waitMs = ms until the next one would (UINT32_MAX
 * = none). Lockstep is unaffected: a repeat is a byte-identical copy of a packet already sent. */
int gdx_mp_txsched_rearm(GdxMpTxSched* s, uint32_t nowMs, uint32_t repeatMs, uint32_t* waitMs);
int gdx_mp_txsched_offer_input(GdxMpTxSched* s, uint16_t node, const void* data, int len);
/* UDS: gdx_mp_txsched_take returning the node (0 = none due). */
uint16_t gdx_mp_txsched_take_due(GdxMpTxSched* s, uint32_t nowMs, uint8_t* out, uint16_t* outLen,
                                 uint32_t* waitMs);
/* Milliseconds until the earliest pending INPUT is due (0 = now, UINT32_MAX = none pending). */
uint32_t gdx_mp_txsched_wait_ms(const GdxMpTxSched* s, uint32_t nowMs);
/* Pending INPUT packets now. */
uint32_t gdx_mp_txsched_pending(const GdxMpTxSched* s);
/* Forget everything pending (the link closed). */
void gdx_mp_txsched_clear(GdxMpTxSched* s);

#ifdef __cplusplus
}
#endif

#endif
