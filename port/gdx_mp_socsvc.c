/* port/gdx_mp_socsvc.c — contract: port/gdx_mp_socsvc.h. */
#include "gdx_mp_socsvc.h"

#include <string.h>

/* ---- byte queue ------------------------------------------------------------------------------- */
static uint32_t LoadAcquire(const volatile uint32_t* p) {
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

static void StoreRelease(volatile uint32_t* p, uint32_t v) {
    __atomic_store_n(p, v, __ATOMIC_RELEASE);
}

void gdx_mp_byteq_init(GdxMpByteQ* q, void* buf, uint32_t cap) {
    q->buf = (uint8_t*) buf;
    q->cap = cap;
    q->head = 0;
    q->tail = 0;
    q->pushed = 0;
    q->popped = 0;
}

uint32_t gdx_mp_byteq_record_bytes(int len) {
    const uint32_t raw = (uint32_t) sizeof(GdxMpQRec) + (uint32_t) len;
    return (raw + GDX_MP_BYTEQ_ALIGN - 1u) & ~(uint32_t) (GDX_MP_BYTEQ_ALIGN - 1u);
}

static GdxMpQRec* RecordAt(const GdxMpByteQ* q, uint32_t offset) {
    return (GdxMpQRec*) (q->buf + (offset & (q->cap - 1u)));
}

/* Producer: where the record starts (free-running), after a wrap marker if it must; UINT32_MAX = no room. */
static uint32_t ReserveRecord(GdxMpByteQ* q, uint32_t head, uint32_t need) {
    const uint32_t used = head - LoadAcquire(&q->tail);
    const uint32_t toEnd = q->cap - (head & (q->cap - 1u));
    if (toEnd >= need) {
        return used + need <= q->cap ? head : UINT32_MAX;
    }
    if (used + toEnd + need > q->cap) {
        return UINT32_MAX;
    }
    RecordAt(q, head)->len = 0; /* wrap marker; toEnd >= GDX_MP_BYTEQ_ALIGN = the header */
    return head + toEnd;
}

int gdx_mp_byteq_push(GdxMpByteQ* q, uint64_t dest, uint32_t stream, const void* data, int len) {
    uint32_t need;
    uint32_t at;
    GdxMpQRec* rec;
    if (len <= 0 || len > GDX_MP_TXQ_PAYLOAD) {
        return 0;
    }
    need = gdx_mp_byteq_record_bytes(len);
    if (need > q->cap / 2u) {
        return 0;
    }
    at = ReserveRecord(q, q->head, need);
    if (at == UINT32_MAX) {
        return 0;
    }
    rec = RecordAt(q, at);
    rec->len = (uint16_t) len;
    rec->reserved = 0;
    rec->stream = stream;
    rec->dest = dest;
    memcpy(rec->data, data, (size_t) len);
    StoreRelease(&q->head, at + need);
    StoreRelease(&q->pushed, q->pushed + 1u);
    return 1;
}

const GdxMpQRec* gdx_mp_byteq_peek(GdxMpByteQ* q) {
    const uint32_t head = LoadAcquire(&q->head);
    uint32_t tail = q->tail; /* only this thread writes tail */
    const GdxMpQRec* rec;
    if (head == tail) {
        return NULL;
    }
    rec = RecordAt(q, tail);
    if (rec->len != 0) {
        return rec;
    }
    tail += q->cap - (tail & (q->cap - 1u)); /* skip the wrap marker */
    StoreRelease(&q->tail, tail);
    return head == tail ? NULL : RecordAt(q, tail);
}

void gdx_mp_byteq_release(GdxMpByteQ* q) {
    const GdxMpQRec* rec = gdx_mp_byteq_peek(q);
    if (rec == NULL) {
        return;
    }
    StoreRelease(&q->tail, q->tail + gdx_mp_byteq_record_bytes(rec->len));
    StoreRelease(&q->popped, q->popped + 1u);
}

uint32_t gdx_mp_byteq_count(const GdxMpByteQ* q) {
    return LoadAcquire(&q->pushed) - LoadAcquire(&q->popped);
}

/* ---- the service ------------------------------------------------------------------------------ */
void gdx_mp_socsvc_init(GdxMpSocSvc* s, void* txBuf, uint32_t txBytes, void* rxBuf, uint32_t rxBytes,
                        uint32_t txMinMs, uint32_t rxPollMs) {
    memset(&s->st, 0, sizeof(s->st));
    gdx_mp_byteq_init(&s->tx, txBuf, txBytes);
    gdx_mp_byteq_init(&s->rx, rxBuf, rxBytes);
    gdx_mp_txsched_init(&s->sched, txMinMs);
    s->rxPollMs = rxPollMs;
    s->txRepeats = 0;
    s->lastDrainMs = 0;
    s->drainedOnce = 0;
}

static void NoteDepth(uint32_t depth, uint32_t* max) {
    if (depth > *max) {
        *max = depth;
    }
}

int gdx_mp_socsvc_queue(GdxMpSocSvc* s, uint64_t dest, uint32_t stream, const void* buf, int len) {
    if (!gdx_mp_byteq_push(&s->tx, dest, stream, buf, len)) {
        s->st.txDrops++;
        return 0;
    }
    s->st.queued++;
    NoteDepth(gdx_mp_byteq_count(&s->tx), &s->st.txDepthMax);
    return 1;
}

int gdx_mp_socsvc_take(GdxMpSocSvc* s, void* buf, int cap, uint64_t* src) {
    const GdxMpQRec* d = gdx_mp_byteq_peek(&s->rx);
    int n;
    if (d == NULL) {
        return 0;
    }
    n = (int) d->len <= cap ? (int) d->len : -1;
    if (n > 0) {
        memcpy(buf, d->data, (size_t) n);
        *src = d->dest;
        s->st.taken++;
    }
    gdx_mp_byteq_release(&s->rx);
    return n;
}

/* Thread: one datagram out now. */
static void SendNow(GdxMpSocSvc* s, const GdxMpSocSvcOps* ops, uint64_t dest, const void* buf, int len) {
    const int r = ops->send(ops->ctx, dest, buf, len);
    if (r > 0) {
        s->st.sent++;
    } else if (r == 0) {
        s->st.sendBlocked++;
    } else {
        s->st.sendErr++;
        s->st.lastErr = r;
    }
}

/* Thread: the tx ring empties: INPUT into the scheduler (or out at once when it refuses), the rest
 * out in order. */
static void SendQueued(GdxMpSocSvc* s, const GdxMpSocSvcOps* ops) {
    const GdxMpQRec* d;
    while ((d = gdx_mp_byteq_peek(&s->tx)) != NULL) {
        if (d->stream == 0 ||
            !gdx_mp_txsched_offer_repeat(&s->sched, d->dest, d->stream, d->data, d->len, (int) s->txRepeats)) {
            SendNow(s, ops, d->dest, d->data, d->len);
        }
        gdx_mp_byteq_release(&s->tx);
    }
}

/* Thread: every scheduled INPUT due at `nowMs`. Returns the ms until the next one (UINT32_MAX = none). */
static uint32_t SendDue(GdxMpSocSvc* s, const GdxMpSocSvcOps* ops, uint32_t nowMs) {
    uint8_t buf[GDX_MP_TXQ_INPUT_MAX];
    uint16_t len = 0;
    uint64_t dest = 0;
    uint32_t waitMs = UINT32_MAX;
    while (gdx_mp_txsched_take(&s->sched, nowMs, &dest, buf, &len, &waitMs)) {
        SendNow(s, ops, dest, buf, len);
    }
    return waitMs;
}

/* Thread: room in the rx queue for the largest datagram? */
static int RxHasRoom(const GdxMpSocSvc* s) {
    const uint32_t used = __atomic_load_n(&s->rx.head, __ATOMIC_RELAXED) - __atomic_load_n(&s->rx.tail, __ATOMIC_ACQUIRE);
    /* worst case: a wrap marker as long as the record, then the record */
    return used + 2u * gdx_mp_byteq_record_bytes((int) sizeof(s->frame)) <= s->rx.cap;
}

/* Thread: recv until the socket would block, the rx queue is full, or the guard. */
static void Drain(GdxMpSocSvc* s, const GdxMpSocSvcOps* ops) {
    int guard;
    s->st.drains++;
    for (guard = 0; guard < GDX_MP_SOCSVC_RX_GUARD; guard++) {
        uint64_t src = 0;
        int n;
        if (!RxHasRoom(s)) {
            s->st.rxFullStops++;
            return;
        }
        n = ops->recv(ops->ctx, s->frame, (int) sizeof(s->frame), &src);
        if (n == 0) {
            s->st.rxEmpty++;
            return;
        }
        if (n < 0) {
            s->st.rxErr++;
            s->st.lastErr = n;
            return;
        }
        s->st.rx++;
        gdx_mp_byteq_push(&s->rx, src, 0, s->frame, n); /* room checked above; only this thread pushes */
        NoteDepth(gdx_mp_byteq_count(&s->rx), &s->st.rxDepthMax);
    }
}

/* Thread: ms until the next drain is due (0 = now). */
static uint32_t DrainWait(const GdxMpSocSvc* s, uint32_t nowMs) {
    const uint32_t poll = s->rxPollMs > 0 ? s->rxPollMs : 1;
    const uint32_t since = nowMs - s->lastDrainMs;
    if (!s->drainedOnce || since >= poll) {
        return 0;
    }
    return poll - since;
}

/* Thread: re-arm repeats that are due. Returns the ms until the next one (UINT32_MAX = none). */
static uint32_t RearmRepeats(GdxMpSocSvc* s, uint32_t nowMs) {
    uint32_t waitMs = UINT32_MAX;
    if (s->txRepeats == 0 || s->sched.minMs == 0) {
        return UINT32_MAX;
    }
    gdx_mp_txsched_rearm(&s->sched, nowMs, s->sched.minMs, &waitMs);
    return waitMs;
}

static uint32_t MinU32(uint32_t a, uint32_t b) {
    return a < b ? a : b;
}

uint32_t gdx_mp_socsvc_step(GdxMpSocSvc* s, const GdxMpSocSvcOps* ops, uint32_t nowMs) {
    uint32_t sendWait;
    uint32_t drainWait;
    s->st.steps++;
    SendQueued(s, ops);
    RearmRepeats(s, nowMs);
    sendWait = SendDue(s, ops, nowMs);
    sendWait = MinU32(sendWait, RearmRepeats(s, nowMs)); /* the wait to the next repeat, after this send */
    if (DrainWait(s, nowMs) == 0) {
        Drain(s, ops);
        s->lastDrainMs = nowMs;
        s->drainedOnce = 1;
    }
    drainWait = DrainWait(s, nowMs);
    return MinU32(sendWait, drainWait);
}

void gdx_mp_socsvc_note_readable(GdxMpSocSvc* s) {
    s->drainedOnce = 0;
}

void gdx_mp_socsvc_note_idle(GdxMpSocSvc* s, uint32_t nowMs) {
    s->lastDrainMs = nowMs;
    s->drainedOnce = 1;
}

void gdx_mp_socsvc_flush(GdxMpSocSvc* s, const GdxMpSocSvcOps* ops, uint32_t nowMs) {
    const uint32_t minMs = s->sched.minMs;
    SendQueued(s, ops);
    s->sched.minMs = 0; /* everything pending is due */
    SendDue(s, ops, nowMs);
    s->sched.minMs = minMs;
}

uint64_t gdx_mp_socsvc_pack(const uint8_t ip[4], uint16_t port) {
    return ((uint64_t) ip[0] << 40) | ((uint64_t) ip[1] << 32) | ((uint64_t) ip[2] << 24) | ((uint64_t) ip[3] << 16) |
           port;
}

void gdx_mp_socsvc_unpack(uint64_t dest, uint8_t ip[4], uint16_t* port) {
    ip[0] = (uint8_t) (dest >> 40);
    ip[1] = (uint8_t) (dest >> 32);
    ip[2] = (uint8_t) (dest >> 24);
    ip[3] = (uint8_t) (dest >> 16);
    *port = (uint16_t) dest;
}

uint32_t gdx_mp_socsvc_stream(const void* gxmp, int len, int relayMask) {
    const uint8_t* p = (const uint8_t*) gxmp;
    if (!gdx_mp_txq_is_input(gxmp, len)) {
        return 0;
    }
    return (1u + (uint32_t) p[6]) | ((uint32_t) (relayMask & 0xFFFF) << 8);
}
