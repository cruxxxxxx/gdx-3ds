/* port/gdx_mp_drop.c — multiplayer drop-out. Contract + wire format: gdx_mp_drop.h. */
#include "gdx_mp_drop.h"

#include <string.h>

static const uint8_t kMagic[4] = { 'G', 'X', 'M', 'P' };

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

static uint16_t GetU16(const uint8_t* p) {
    return (uint16_t) (p[0] | (p[1] << 8));
}

static uint32_t GetU32(const uint8_t* p) {
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

static uint32_t Slot(uint32_t tick) {
    return tick & (GDX_MP_RING - 1);
}

static int HasPad(const GdxMpLockstep* ls, int player, uint32_t tick) {
    return ls->slotTick[player][Slot(tick)] == tick;
}

static void StorePad(GdxMpLockstep* ls, int player, uint32_t tick, GdxMpPad pad) {
    const uint32_t s = Slot(tick);
    ls->pads[player][s] = pad;
    ls->slotTick[player][s] = tick;
}

static uint8_t Bit(int player) {
    return (uint8_t) (1u << player);
}

void gdx_mp_drop_init(GdxMpDrop* d, const GdxMpLockstep* ls) {
    int p;
    memset(d, 0, sizeof(*d));
    d->numPlayers = ls->numPlayers;
    d->localPlayer = ls->localPlayer;
    d->isHost = (ls->localPlayer == 0);
    d->pending = -1;
    d->ackDue = -1;
    for (p = 0; p < GDX_MP_MAX_PLAYERS; p++) {
        d->removedAt[p] = GDX_MP_NO_TICK;
    }
}

int gdx_mp_drop_refuse_from(const GdxMpDrop* d, int from) {
    if (from < 0 || from >= GDX_MP_MAX_PLAYERS) {
        return 1;
    }
    return (d->frozenMask & Bit(from)) != 0;
}

int gdx_mp_drop_is_removed(const GdxMpDrop* d, int player, uint32_t tick) {
    if (player < 0 || player >= GDX_MP_MAX_PLAYERS || d->removedAt[player] == GDX_MP_NO_TICK) {
        return 0;
    }
    return tick >= d->removedAt[player];
}

uint32_t gdx_mp_drop_live_mask(const GdxMpDrop* d) {
    const uint32_t all = (d->numPlayers >= 32) ? 0xFFFFFFFFu : ((1u << d->numPlayers) - 1u);
    return all & ~(uint32_t) d->removedMask;
}

/* Peers the host still waits for in the current phase. */
static uint8_t NeedMask(const GdxMpDrop* d) {
    uint8_t need = 0;
    int q;
    for (q = 0; q < d->numPlayers; q++) {
        if (q == d->localPlayer || q == d->pending) {
            continue;
        }
        if ((d->removedMask | d->queueMask) & Bit(q)) {
            continue;
        }
        need |= Bit(q);
    }
    return need;
}

static void StartPending(GdxMpDrop* d, int player, int reason) {
    d->pending = player;
    d->phase = GDX_MP_DROP_FREEZE;
    d->ackMask = 0;
    memset(d->reportKnown, 0, sizeof(d->reportKnown));
    d->commitTick = GDX_MP_NO_TICK;
    d->frozenMask |= Bit(player);
    d->reason[player] = (uint8_t) reason;
    d->sentOnce = 0;
}

int gdx_mp_drop_begin(GdxMpDrop* d, GdxMpLockstep* ls, int player, int reason) {
    (void) ls;
    if (!d->isHost || player <= 0 || player >= d->numPlayers || player == d->localPlayer) {
        return 0;
    }
    if (((d->removedMask | d->queueMask) & Bit(player)) || player == d->pending) {
        return 0;
    }
    if (d->pending >= 0) {
        d->queueMask |= Bit(player);
        d->queueReason[player] = (uint8_t) reason;
        d->frozenMask |= Bit(player);
        return 1;
    }
    StartPending(d, player, reason);
    return 1;
}

/* Stores carried pads of `player` for ticks in [known, limit) inside the live window, then
 * advances `known` over what is now contiguous, never past `limit`. */
static void MergeTail(GdxMpLockstep* ls, int player, uint32_t first, int count, const uint8_t* pads,
                      uint32_t limit) {
    int i;
    const uint32_t windowEnd = ls->known[player] + GDX_MP_RING / 2;
    if (limit > windowEnd) {
        limit = windowEnd;
    }
    for (i = 0; i < count; i++) {
        const uint32_t t = first + (uint32_t) i;
        if (t >= ls->known[player] && t < limit && !HasPad(ls, player, t)) {
            GdxMpPad pad;
            pad.buttons = GetU16(pads + 4 * i);
            pad.stickX = (int8_t) pads[4 * i + 2];
            pad.stickY = (int8_t) pads[4 * i + 3];
            StorePad(ls, player, t, pad);
        }
    }
    while (ls->known[player] < limit && HasPad(ls, player, ls->known[player])) {
        ls->known[player]++;
    }
}

/* Writes the header + the contiguous pads of `player` ending at `end` (up to the redundancy). */
static int BuildPacket(const GdxMpLockstep* ls, int type, int player, int phase, uint8_t removedMask, uint32_t tick,
                       uint32_t end, int reason, uint8_t* out, int cap) {
    uint32_t first = end;
    uint32_t i;
    int count;
    uint8_t* p;
    if (cap < GDX_MP_DROP_MAX_PACKET) {
        return 0;
    }
    while (first > 0 && end - first < GDX_MP_REDUNDANCY && player >= 0 && HasPad(ls, player, first - 1)) {
        first--;
    }
    count = (int) (end - first);
    memcpy(out, kMagic, 4);
    out[4] = GDX_MP_VERSION;
    out[5] = (uint8_t) type;
    out[6] = (uint8_t) ls->localPlayer;
    out[7] = (uint8_t) count;
    PutU32(out + 8, ls->session);
    out[12] = (uint8_t) player;
    out[13] = (uint8_t) phase;
    out[14] = removedMask;
    out[15] = 0;
    PutU32(out + 16, tick);
    PutU32(out + 20, first);
    out[24] = (uint8_t) reason;
    out[25] = 0;
    out[26] = 0;
    out[27] = 0;
    p = out + GDX_MP_DROP_HEADER_BYTES;
    for (i = 0; i < (uint32_t) count; i++) {
        const GdxMpPad* pad = &ls->pads[player][Slot(first + i)];
        PutU16(p, pad->buttons);
        p[2] = (uint8_t) pad->stickX;
        p[3] = (uint8_t) pad->stickY;
        p += 4;
    }
    return GDX_MP_DROP_HEADER_BYTES + 4 * count;
}

static int LowestBit(uint8_t mask) {
    int p;
    for (p = 0; p < 8; p++) {
        if (mask & (1u << p)) {
            return p;
        }
    }
    return -1;
}

/* Host: move the removal in progress on when every remaining peer answered its phase. */
static void HostAdvance(GdxMpDrop* d, GdxMpLockstep* ls, uint32_t nowMs) {
    int guard;
    for (guard = 0; guard < GDX_MP_MAX_PLAYERS * 2 && d->pending >= 0; guard++) {
        const uint8_t need = NeedMask(d);
        const int p = d->pending;
        if ((d->ackMask & need) != need) {
            return;
        }
        if (d->phase == GDX_MP_DROP_FREEZE) {
            uint32_t t = ls->known[p];
            int q;
            for (q = 0; q < d->numPlayers; q++) {
                if ((need & Bit(q)) && d->reportKnown[q] > t) {
                    t = d->reportKnown[q];
                }
            }
            if (ls->known[p] < t) {
                d->fatal = 1; /* a peer holds pads of p that no tail delivered to us */
                return;
            }
            d->commitTick = t;
            d->removedAt[p] = t;
            d->removedMask |= Bit(p);
            d->newCommits |= Bit(p);
            d->commits++;
            d->phase = GDX_MP_DROP_COMMIT;
            d->ackMask = 0;
            d->sentOnce = 0;
            (void) nowMs;
            continue;
        }
        /* COMMIT acknowledged by everybody: the next queued removal, if any. */
        d->pending = -1;
        if (d->queueMask != 0) {
            const int next = LowestBit(d->queueMask);
            d->queueMask &= (uint8_t) ~Bit(next);
            StartPending(d, next, d->queueReason[next]);
        }
    }
}

int gdx_mp_drop_service(GdxMpDrop* d, GdxMpLockstep* ls, uint32_t nowMs, uint8_t* out, int cap, int* dest) {
    *dest = GDX_MP_DROP_DEST_ALL;
    if (d->ackDue >= 0) {
        const int p = d->ackDue;
        /* A FREEZE ack carries our tail of p (the host may lack it); a COMMIT ack only confirms. */
        const uint32_t end = (d->ackPhase == GDX_MP_DROP_FREEZE) ? ls->known[p] : 0;
        const int n = BuildPacket(ls, GDX_MP_TYPE_DROP_ACK, p, d->ackPhase, d->removedMask, d->ackTick, end, 0,
                                  out, cap);
        d->ackDue = -1;
        if (n > 0) {
            *dest = 0;
            return n;
        }
    }
    if (!d->isHost) {
        return 0;
    }
    HostAdvance(d, ls, nowMs);
    if (d->pending < 0 || NeedMask(d) == 0) {
        return 0;
    }
    if (d->sentOnce && (uint32_t) (nowMs - d->lastSendMs) < GDX_MP_DROP_RESEND_MS) {
        return 0;
    }
    d->sentOnce = 1;
    d->lastSendMs = nowMs;
    if (d->phase == GDX_MP_DROP_FREEZE) {
        return BuildPacket(ls, GDX_MP_TYPE_DROP, d->pending, GDX_MP_DROP_FREEZE, d->removedMask, 0,
                           ls->known[d->pending], d->reason[d->pending], out, cap);
    }
    return BuildPacket(ls, GDX_MP_TYPE_DROP, d->pending, GDX_MP_DROP_COMMIT, d->removedMask, d->commitTick,
                       d->commitTick, d->reason[d->pending], out, cap);
}

int gdx_mp_drop_build_leave(const GdxMpDrop* d, const GdxMpLockstep* ls, uint32_t nextTick, int reason,
                            uint8_t* out, int cap) {
    (void) d;
    if (cap < GDX_MP_DROP_MAX_PACKET) {
        return 0;
    }
    /* No pads: a LEAVE only says who and when. */
    {
        const int n = BuildPacket(ls, GDX_MP_TYPE_LEAVE, ls->localPlayer, 0, d->removedMask, nextTick, 0, reason,
                                  out, cap);
        if (n > 0) {
            out[7] = 0;
            PutU32(out + 20, 0);
        }
        return n > 0 ? GDX_MP_DROP_HEADER_BYTES : 0;
    }
}

int gdx_mp_drop_packet_type(const uint8_t* pkt, int len) {
    int type;
    if (pkt == NULL || len < GDX_MP_DROP_HEADER_BYTES || memcmp(pkt, kMagic, 4) != 0 ||
        pkt[4] != GDX_MP_VERSION) {
        return -1;
    }
    type = pkt[5];
    if (type != GDX_MP_TYPE_DROP && type != GDX_MP_TYPE_DROP_ACK && type != GDX_MP_TYPE_LEAVE) {
        return -1;
    }
    if (pkt[7] > GDX_MP_REDUNDANCY || len != GDX_MP_DROP_HEADER_BYTES + 4 * pkt[7] ||
        pkt[6] >= GDX_MP_MAX_PLAYERS || pkt[12] >= GDX_MP_MAX_PLAYERS) {
        return -1;
    }
    if (type == GDX_MP_TYPE_LEAVE && (pkt[12] != pkt[6] || pkt[7] != 0)) {
        return -1;
    }
    if (type != GDX_MP_TYPE_LEAVE && pkt[13] != GDX_MP_DROP_FREEZE && pkt[13] != GDX_MP_DROP_COMMIT) {
        return -1;
    }
    return type;
}

static int Refuse(GdxMpDrop* d) {
    d->rejected++;
    return -1;
}

static void NoteLeave(GdxMpDrop* d, GdxMpLockstep* ls, int from) {
    if (from == 0) {
        d->hostLeft = 1;
    } else if (d->isHost) {
        gdx_mp_drop_begin(d, ls, from, GDX_MP_DROP_R_LEFT);
    }
}

static int IngestDrop(GdxMpDrop* d, GdxMpLockstep* ls, const uint8_t* pkt) {
    const int p = pkt[12];
    const int phase = pkt[13];
    const uint32_t tick = GetU32(pkt + 16);
    const uint32_t first = GetU32(pkt + 20);
    const int count = pkt[7];
    const uint8_t* pads = pkt + GDX_MP_DROP_HEADER_BYTES;
    if (pkt[6] != 0 || d->isHost) {
        return Refuse(d); /* only the host removes players */
    }
    if (p == d->localPlayer) {
        d->selfRemoved = 1;
        return 1;
    }
    if (p == 0) {
        return Refuse(d);
    }
    if (phase == GDX_MP_DROP_FREEZE) {
        d->frozenMask |= Bit(p);
        if (!(d->removedMask & Bit(p))) {
            MergeTail(ls, p, first, count, pads, GDX_MP_NO_TICK);
        }
        d->ackDue = p;
        d->ackPhase = GDX_MP_DROP_FREEZE;
        d->ackTick = ls->known[p];
        return 1;
    }
    /* COMMIT */
    if (d->removedMask & Bit(p)) {
        if (d->removedAt[p] != tick) {
            return Refuse(d);
        }
    } else {
        d->frozenMask |= Bit(p);
        if (ls->known[p] > tick) {
            d->fatal = 1; /* we already hold (and may have used) pads of p at or past T */
            return 1;
        }
        MergeTail(ls, p, first, count, pads, tick);
        if (ls->known[p] < tick) {
            d->fatal = 1; /* the commit's tail does not reach our gap */
            return 1;
        }
        d->removedAt[p] = tick;
        d->reason[p] = pkt[24]; /* informational only (receipts); never feeds the simulation */
        d->removedMask |= Bit(p);
        d->newCommits |= Bit(p);
        d->commits++;
    }
    d->ackDue = p;
    d->ackPhase = GDX_MP_DROP_COMMIT;
    d->ackTick = tick;
    return 1;
}

static int IngestAck(GdxMpDrop* d, GdxMpLockstep* ls, const uint8_t* pkt) {
    const int from = pkt[6];
    const int p = pkt[12];
    const int phase = pkt[13];
    const uint32_t tick = GetU32(pkt + 16);
    if (!d->isHost || from == 0 || p != d->pending) {
        return (d->isHost && from != 0) ? 1 : Refuse(d); /* a stale ACK is harmless */
    }
    if (phase != d->phase) {
        return 1;
    }
    if (phase == GDX_MP_DROP_FREEZE) {
        /* A report far past what any console can hold is hostile: it would make T unfillable. */
        if (tick > ls->known[p] + 2u * GDX_MP_REDUNDANCY) {
            return Refuse(d);
        }
        if (tick > ls->known[p]) {
            d->bridged++;
        }
        MergeTail(ls, p, GetU32(pkt + 20), pkt[7], pkt + GDX_MP_DROP_HEADER_BYTES, tick);
        d->reportKnown[from] = tick;
        d->ackMask |= Bit(from);
        return 1;
    }
    if (tick == d->commitTick) {
        d->ackMask |= Bit(from);
    }
    return 1;
}

int gdx_mp_drop_ingest(GdxMpDrop* d, GdxMpLockstep* ls, const uint8_t* pkt, int len, uint32_t nowMs) {
    const int type = gdx_mp_drop_packet_type(pkt, len);
    int from;
    (void) nowMs;
    if (type < 0) {
        return Refuse(d);
    }
    from = pkt[6];
    if (from >= d->numPlayers || from == d->localPlayer || pkt[12] >= d->numPlayers) {
        return Refuse(d);
    }
    if (GetU32(pkt + 8) != ls->session) {
        return Refuse(d);
    }
    if (type == GDX_MP_TYPE_LEAVE) {
        NoteLeave(d, ls, from);
        return 1;
    }
    if (type == GDX_MP_TYPE_DROP) {
        return IngestDrop(d, ls, pkt);
    }
    return IngestAck(d, ls, pkt);
}

void gdx_mp_drop_note_input_flags(GdxMpDrop* d, GdxMpLockstep* ls, const uint8_t* pkt, int len) {
    const int from = gdx_mp_ls_packet_from(pkt, len);
    if (from < 0 || from >= d->numPlayers || from == d->localPlayer) {
        return;
    }
    if ((pkt[33] & 0x01u) && GetU32(pkt + 28) == ls->session) {
        NoteLeave(d, ls, from);
    }
}

void gdx_mp_drop_fill(GdxMpDrop* d, GdxMpLockstep* ls, uint32_t tick) {
    const GdxMpPad neutral = { 0, 0, 0 };
    int p;
    if (d->removedMask == 0) {
        return;
    }
    for (p = 0; p < d->numPlayers; p++) {
        uint32_t target;
        uint32_t t;
        if (!(d->removedMask & Bit(p)) || p == d->localPlayer) {
            continue;
        }
        target = tick + (uint32_t) ls->delay + 2u;
        t = ls->known[p] > d->removedAt[p] ? ls->known[p] : d->removedAt[p];
        if (target > t + GDX_MP_RING / 2) {
            t = target - GDX_MP_RING / 2; /* far behind (a long stall): only the live window */
        }
        for (; t < target; t++) {
            StorePad(ls, p, t, neutral);
        }
        if (ls->known[p] < target) {
            ls->known[p] = target;
        }
        /* A removed player acknowledges nothing: never let it hold our resend window open. */
        ls->acked[p] = ls->known[ls->localPlayer];
    }
}
