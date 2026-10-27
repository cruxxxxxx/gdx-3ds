/* port/gdx_mp_txq.c — contract: port/gdx_mp_txq.h. */
#include "gdx_mp_txq.h"

#include "gdx_mp_lockstep.h"

#include <string.h>

static uint32_t LoadAcquire(const volatile uint32_t* p) {
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

static void StoreRelease(volatile uint32_t* p, uint32_t v) {
    __atomic_store_n(p, v, __ATOMIC_RELEASE);
}

void gdx_mp_ring_init(GdxMpRing* r, GdxMpDgram* slots, uint32_t cap) {
    r->slot = slots;
    r->cap = cap;
    r->head = 0;
    r->tail = 0;
}

/* Producer: copy one datagram into the next slot, then publish it. */
static int PushCopy(GdxMpRing* r, uint16_t node, uint64_t dest, uint32_t stream, const void* data, int len) {
    const uint32_t head = r->head; /* only this thread writes head */
    const uint32_t tail = LoadAcquire(&r->tail);
    GdxMpDgram* d;
    if (len <= 0 || len > GDX_MP_TXQ_PAYLOAD) {
        return 0;
    }
    if (head - tail >= r->cap) {
        return 0;
    }
    d = &r->slot[head & (r->cap - 1)];
    d->node = node;
    d->dest = dest;
    d->stream = stream;
    d->len = (uint16_t) len;
    memcpy(d->data, data, (size_t) len);
    StoreRelease(&r->head, head + 1);
    return 1;
}

int gdx_mp_ring_push(GdxMpRing* r, uint16_t node, const void* data, int len) {
    return PushCopy(r, node, node, 0, data, len);
}

int gdx_mp_ring_push_to(GdxMpRing* r, uint64_t dest, uint32_t stream, const void* data, int len) {
    return PushCopy(r, 0, dest, stream, data, len);
}

const GdxMpDgram* gdx_mp_ring_peek(GdxMpRing* r) {
    const uint32_t tail = r->tail; /* only this thread writes tail */
    const uint32_t head = LoadAcquire(&r->head);
    if (head == tail) {
        return NULL;
    }
    return &r->slot[tail & (r->cap - 1)];
}

void gdx_mp_ring_release(GdxMpRing* r) {
    StoreRelease(&r->tail, r->tail + 1);
}

uint32_t gdx_mp_ring_count(const GdxMpRing* r) {
    return LoadAcquire(&r->head) - LoadAcquire(&r->tail);
}

void gdx_mp_ring_reset(GdxMpRing* r) {
    StoreRelease(&r->head, 0);
    StoreRelease(&r->tail, 0);
}

int gdx_mp_txq_is_input(const void* data, int len) {
    const uint8_t* p = (const uint8_t*) data;
    if (len < GDX_MP_HEADER_BYTES || len > GDX_MP_TXQ_INPUT_MAX) {
        return 0;
    }
    if (p[0] != 'G' || p[1] != 'X' || p[2] != 'M' || p[3] != 'P') {
        return 0;
    }
    if (p[4] != GDX_MP_VERSION || p[5] != GDX_MP_TYPE_INPUT) {
        return 0;
    }
    return len == GDX_MP_HEADER_BYTES + 4 * (int) p[7];
}

void gdx_mp_txsched_init(GdxMpTxSched* s, uint32_t minMs) {
    memset(s, 0, sizeof(*s));
    s->minMs = minMs;
}

/* The entry of (dest, stream), or NULL. */
static GdxMpTxEntry* FindEntry(GdxMpTxSched* s, uint64_t dest, uint32_t stream) {
    int i;
    for (i = 0; i < GDX_MP_TXQ_KEYS; i++) {
        if (s->e[i].stream == stream && s->e[i].dest == dest) {
            return &s->e[i];
        }
    }
    return NULL;
}

/* An entry for a new key: a free one, else the idle one that sent longest ago (forgetting its
 * send time only lets that key's next packet go at once). NULL = every entry is pending. */
static GdxMpTxEntry* ClaimEntry(GdxMpTxSched* s, uint64_t dest, uint32_t stream) {
    GdxMpTxEntry* best = NULL;
    int i;
    for (i = 0; i < GDX_MP_TXQ_KEYS; i++) {
        GdxMpTxEntry* e = &s->e[i];
        if (e->stream == 0) {
            best = e;
            break;
        }
        if (!e->pending && (best == NULL || (int32_t) (e->lastMs - best->lastMs) < 0)) {
            best = e;
        }
    }
    if (best != NULL) {
        best->dest = dest;
        best->stream = stream;
        best->lastMs = 0;
        best->len = 0;
        best->pending = 0;
        best->sentOnce = 0;
        best->repeatsLeft = 0;
    }
    return best;
}

/* The bytes of an INPUT that only say when it was sent (sendMs, echoMs): a resend differs there. */
#define TXQ_CLOCK_FIRST 20
#define TXQ_CLOCK_END 28

/* Same packet but for its clock words? */
static int SameContent(const GdxMpTxEntry* e, const uint8_t* data, int len) {
    if (e->len != (uint16_t) len || len < TXQ_CLOCK_END) {
        return 0;
    }
    return memcmp(e->data, data, TXQ_CLOCK_FIRST) == 0 &&
           memcmp(e->data + TXQ_CLOCK_END, data + TXQ_CLOCK_END, (size_t) (len - TXQ_CLOCK_END)) == 0;
}

static GdxMpTxEntry* EntryFor(GdxMpTxSched* s, uint64_t dest, uint32_t stream) {
    GdxMpTxEntry* e = FindEntry(s, dest, stream);
    return e != NULL ? e : ClaimEntry(s, dest, stream);
}

int gdx_mp_txsched_offer_repeat(GdxMpTxSched* s, uint64_t dest, uint32_t stream, const void* data, int len,
                                int repeats) {
    GdxMpTxEntry* e;
    if (stream == 0 || len <= 0 || len > GDX_MP_TXQ_INPUT_MAX) {
        return 0;
    }
    e = EntryFor(s, dest, stream);
    if (e == NULL) {
        s->refused++;
        return 0;
    }
    if (e->pending) {
        s->coalesced++;
    }
    if (SameContent(e, (const uint8_t*) data, len)) {
        e->repeatsLeft = 0;
    } else {
        e->repeatsLeft = (uint8_t) (repeats < 0 ? 0 : (repeats > 255 ? 255 : repeats));
    }
    memcpy(e->data, data, (size_t) len);
    e->len = (uint16_t) len;
    e->pending = 1;
    return 1;
}

int gdx_mp_txsched_offer(GdxMpTxSched* s, uint64_t dest, uint32_t stream, const void* data, int len) {
    return gdx_mp_txsched_offer_repeat(s, dest, stream, data, len, 0);
}

int gdx_mp_txsched_rearm(GdxMpTxSched* s, uint32_t nowMs, uint32_t repeatMs, uint32_t* waitMs) {
    uint32_t best = UINT32_MAX;
    int armed = 0;
    int i;
    for (i = 0; i < GDX_MP_TXQ_KEYS; i++) {
        GdxMpTxEntry* e = &s->e[i];
        uint32_t since;
        if (e->stream == 0 || e->pending || !e->sentOnce || e->repeatsLeft == 0) {
            continue;
        }
        since = nowMs - e->lastMs;
        if (since >= repeatMs) {
            e->pending = 1;
            e->repeatsLeft--;
            s->repeats++;
            armed++;
        } else if (repeatMs - since < best) {
            best = repeatMs - since;
        }
    }
    if (waitMs != NULL) {
        *waitMs = best;
    }
    return armed;
}

/* Milliseconds until the entry's pending INPUT may go (0 = now). */
static uint32_t EntryWait(const GdxMpTxSched* s, const GdxMpTxEntry* e, uint32_t nowMs) {
    uint32_t since;
    if (!e->sentOnce || s->minMs == 0) {
        return 0;
    }
    since = nowMs - e->lastMs;
    return since >= s->minMs ? 0 : s->minMs - since;
}

uint32_t gdx_mp_txsched_wait_ms(const GdxMpTxSched* s, uint32_t nowMs) {
    uint32_t best = UINT32_MAX;
    int i;
    for (i = 0; i < GDX_MP_TXQ_KEYS; i++) {
        if (s->e[i].pending) {
            const uint32_t w = EntryWait(s, &s->e[i], nowMs);
            if (w < best) {
                best = w;
            }
        }
    }
    return best;
}

uint32_t gdx_mp_txsched_pending(const GdxMpTxSched* s) {
    uint32_t n = 0;
    int i;
    for (i = 0; i < GDX_MP_TXQ_KEYS; i++) {
        n += s->e[i].pending ? 1u : 0u;
    }
    return n;
}

int gdx_mp_txsched_take(GdxMpTxSched* s, uint32_t nowMs, uint64_t* dest, uint8_t* out, uint16_t* outLen,
                        uint32_t* waitMs) {
    int i;
    int took = 0;
    for (i = 0; i < GDX_MP_TXQ_KEYS && !took; i++) {
        GdxMpTxEntry* e = &s->e[i];
        if (e->pending && EntryWait(s, e, nowMs) == 0) {
            memcpy(out, e->data, e->len);
            *outLen = e->len;
            *dest = e->dest;
            e->pending = 0;
            e->sentOnce = 1;
            e->lastMs = nowMs;
            took = 1;
        }
    }
    if (waitMs != NULL) {
        *waitMs = gdx_mp_txsched_wait_ms(s, nowMs);
    }
    return took;
}

/* UDS: the packet's lockstep `from` + 1 (byte 6 of the GXMP header), so it is never 0. */
static uint32_t InputStream(const void* data, int len) {
    const uint8_t* p = (const uint8_t*) data;
    return 1u + (len > 6 ? (uint32_t) p[6] : 0u);
}

int gdx_mp_txsched_offer_input(GdxMpTxSched* s, uint16_t node, const void* data, int len) {
    if (node == 0 || node >= GDX_MP_TXQ_NODES) {
        return 0;
    }
    return gdx_mp_txsched_offer(s, node, InputStream(data, len), data, len);
}

uint16_t gdx_mp_txsched_take_due(GdxMpTxSched* s, uint32_t nowMs, uint8_t* out, uint16_t* outLen,
                                 uint32_t* waitMs) {
    uint64_t dest = 0;
    return gdx_mp_txsched_take(s, nowMs, &dest, out, outLen, waitMs) ? (uint16_t) dest : 0;
}

void gdx_mp_txsched_clear(GdxMpTxSched* s) {
    int i;
    for (i = 0; i < GDX_MP_TXQ_KEYS; i++) {
        s->e[i].pending = 0;
    }
}
