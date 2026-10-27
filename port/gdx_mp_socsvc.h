/* port/gdx_mp_socsvc.h — the socket service thread's loop body (pure, host-tested by
 * port/tests/gdx_mp_txq_host_tests.c). The 3DS half (the thread, the wake event, sendto /
 * recvfrom, the levers, APT parking) is port/3ds/net/gdx3ds_mp_socthr.c.
 *
 * Why: hardware online1 (New 3DS host, Old 3DS guest, internet room, link DIRECT): the Old 3DS
 * main thread spent ~15 ms of every race frame in SOC calls. Per 600 frames, 2000-6300 recvfrom at
 * 1.5-2.2 ms each (max 30-90 ms) and 700-1200 sendto at 3.3-6.1 ms each (max 20-88 ms): [c5cat]
 * inp=14-20 ms, 24-38 ticks/s, the New 3DS host stalled half the race waiting for it. The SOC
 * sysmodule's answer is slow on that console (core 1, shared with the audio threads), not our CPU,
 * so the fix is to stop the main thread from waiting for it and to ask it less often:
 *
 *   MAIN THREAD (producer of tx, consumer of rx): gdx_mp_socsvc_queue copies a datagram into the
 *     tx ring; gdx_mp_socsvc_take copies one out of the rx ring. No SOC call, no lock.
 *   SERVICE THREAD (gdx_mp_socsvc_step, once per wake):
 *     1. every queued datagram goes out: a lockstep INPUT (stream != 0) through the coalescing
 *        scheduler (port/gdx_mp_txq.h: newest per (destination, stream), at most one per
 *        txMinMs), anything else at once and in order;
 *     2. every scheduled INPUT that is due goes out, and (txRepeats) the newest INPUT of a stream
 *        goes out again every txMinMs, up to txRepeats times, while the main thread offers
 *        nothing newer: a steady send cadence through a render hitch, which a resend of unchanged
 *        content (a stall: nothing heard) does not earn, so a long stall backs off;
 *     3. once per rxPollMs it drains the socket: recv until it would block (one empty call per
 *        drain, not one per main-loop pass), the rx ring is full (the rest stays in the socket's
 *        buffer: back-pressure, not loss), or GDX_MP_SOCSVC_RX_GUARD datagrams;
 *     and returns how long it may sleep (the next rx poll or the next due INPUT, whichever is
 *     first); a queued datagram wakes it early (the caller's event).
 *   The socket calls are the caller's (GdxMpSocSvcOps), so the host test runs the same loop
 *   against a simulated socket.
 *
 * Lockstep is unaffected: the same datagrams cross (minus INPUT copies a newer one of the same
 * stream superseded), only when they leave and arrive moves. */
#ifndef GDX_MP_SOCSVC_H
#define GDX_MP_SOCSVC_H

#include "gdx_mp_txq.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GDX_MP_SOCSVC_RX_GUARD 64 /* datagrams one drain may take */

/* ---- the byte queue: one producer thread, one consumer thread, no locks, no allocation -------
 * Datagrams are variable-length records in one caller-owned buffer (static on the 3DS: an Old 3DS
 * in a race has ~3-13 KiB of heap slack, so nothing here may malloc). A record is a 16-byte
 * header plus the payload, rounded up to 16 bytes; a record that would not fit before the
 * buffer's end leaves a wrap marker there and starts at the beginning. A typical lockstep
 * datagram (76-123 B) costs 96-144 B, against 1.5 KiB for a fixed GdxMpDgram slot. */
#define GDX_MP_BYTEQ_ALIGN 16

typedef struct GdxMpQRec {
    uint16_t len;      /* payload bytes; 0 = a wrap marker (the next record is at offset 0) */
    uint16_t reserved;
    uint32_t stream;   /* tx: the coalescing stream (0 = send at once, in order) */
    uint64_t dest;     /* tx: the destination; rx: the source (gdx_mp_socsvc_pack) */
    uint8_t data[];
} GdxMpQRec;

typedef struct GdxMpByteQ {
    uint8_t* buf;
    uint32_t cap;              /* bytes, a power of two >= 2 * the largest record */
    volatile uint32_t head;    /* producer: free-running byte offset of the next record */
    volatile uint32_t tail;    /* consumer: free-running byte offset of the oldest record */
    volatile uint32_t pushed;  /* producer: records published */
    volatile uint32_t popped;  /* consumer: records released */
} GdxMpByteQ;

/* `buf`: `cap` bytes, 8-byte aligned, cap a power of two. */
void gdx_mp_byteq_init(GdxMpByteQ* q, void* buf, uint32_t cap);
/* Producer. 1 = published, 0 = no room (or len outside 1..GDX_MP_TXQ_PAYLOAD). */
int gdx_mp_byteq_push(GdxMpByteQ* q, uint64_t dest, uint32_t stream, const void* data, int len);
/* Consumer: the oldest record (valid until gdx_mp_byteq_release), NULL = empty. */
const GdxMpQRec* gdx_mp_byteq_peek(GdxMpByteQ* q);
void gdx_mp_byteq_release(GdxMpByteQ* q);
/* Records waiting (either thread; a snapshot). */
uint32_t gdx_mp_byteq_count(const GdxMpByteQ* q);
/* Bytes a record of `len` payload bytes takes. */
uint32_t gdx_mp_byteq_record_bytes(int len);

typedef struct GdxMpSocSvcOps {
    /* > 0 sent, 0 = would block / no buffer (the datagram is dropped, as a full socket would),
     * < 0 = an error. */
    int (*send)(void* ctx, uint64_t dest, const void* buf, int len);
    /* > 0 bytes (and *src), 0 = nothing waiting, < 0 = an error. */
    int (*recv)(void* ctx, void* buf, int cap, uint64_t* src);
    void* ctx;
} GdxMpSocSvcOps;

typedef struct GdxMpSocSvcStats {
    uint32_t queued, txDrops;                     /* main: into the tx ring / ring full */
    uint32_t sent, sendBlocked, sendErr;          /* thread: send calls by outcome */
    uint32_t rx, rxEmpty, rxErr, rxFullStops;     /* thread: recv calls by outcome; drains cut by a full ring */
    uint32_t drains, steps;
    uint32_t taken;                               /* main: out of the rx ring */
    uint32_t txDepthMax, rxDepthMax;              /* ring depth high-water marks */
    int lastErr;                                  /* the last send / recv error (negative) */
} GdxMpSocSvcStats;

typedef struct GdxMpSocSvc {
    GdxMpByteQ tx;
    GdxMpByteQ rx;
    GdxMpTxSched sched;
    volatile uint32_t rxPollMs; /* main may change it (a local wireless room polls rarely) */
    uint32_t txRepeats;         /* steady cadence: extra sends of a new INPUT while nothing replaces it
                                 * (one per txMinMs; 0 = off; set after init) */
    uint32_t lastDrainMs;
    int drainedOnce;
    GdxMpSocSvcStats st;
    uint8_t frame[GDX_MP_TXQ_PAYLOAD]; /* the thread's receive buffer */
} GdxMpSocSvc;

/* txBuf / rxBuf: byte queue storage (see gdx_mp_byteq_init), owned by the caller. */
void gdx_mp_socsvc_init(GdxMpSocSvc* s, void* txBuf, uint32_t txBytes, void* rxBuf, uint32_t rxBytes,
                        uint32_t txMinMs, uint32_t rxPollMs);

/* Main thread. 1 = queued (the caller wakes the thread), 0 = the tx ring is full (dropped). */
int gdx_mp_socsvc_queue(GdxMpSocSvc* s, uint64_t dest, uint32_t stream, const void* buf, int len);
/* Main thread: the oldest received datagram (bytes, *src), 0 = none. One larger than `cap` is
 * dropped (returns -1). */
int gdx_mp_socsvc_take(GdxMpSocSvc* s, void* buf, int cap, uint64_t* src);

/* Service thread: one wake's work. Returns the ms it may sleep (>= 0). */
uint32_t gdx_mp_socsvc_step(GdxMpSocSvc* s, const GdxMpSocSvcOps* ops, uint32_t nowMs);
/* Service thread, blocking-poll mode: the socket is readable (the next step drains at once), or
 * a poll found nothing until `nowMs` (the next step does not drain until a poll period later). */
void gdx_mp_socsvc_note_readable(GdxMpSocSvc* s);
void gdx_mp_socsvc_note_idle(GdxMpSocSvc* s, uint32_t nowMs);
/* Service thread: send everything still queued or pending at once (before it parks / stops). */
void gdx_mp_socsvc_flush(GdxMpSocSvc* s, const GdxMpSocSvcOps* ops, uint32_t nowMs);

/* An endpoint as a scheduler / ring destination: ip[0..3] (network order) and port. */
uint64_t gdx_mp_socsvc_pack(const uint8_t ip[4], uint16_t port);
void gdx_mp_socsvc_unpack(uint64_t dest, uint8_t ip[4], uint16_t* port);

/* The coalescing stream of a datagram we send: 0 = not a lockstep INPUT (send at once, in order);
 * else 1 + its `from`, plus (relayMask << 8) for a copy the server relays to those slots. */
uint32_t gdx_mp_socsvc_stream(const void* gxmp, int len, int relayMask);

#ifdef __cplusplus
}
#endif

#endif /* GDX_MP_SOCSVC_H */
