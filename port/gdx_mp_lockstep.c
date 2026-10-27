/* port/gdx_mp_lockstep.c — MP spike lockstep core. Contract + wire format: gdx_mp_lockstep.h. */
#include "gdx_mp_lockstep.h"

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

static void StorePad(GdxMpLockstep* ls, int player, uint32_t tick, GdxMpPad pad) {
    const uint32_t s = Slot(tick);
    ls->pads[player][s] = pad;
    ls->slotTick[player][s] = tick;
}

static int HasPad(const GdxMpLockstep* ls, int player, uint32_t tick) {
    return ls->slotTick[player][Slot(tick)] == tick;
}

static int Removed(const GdxMpLockstep* ls, int player, uint32_t tick) {
    return ls->removedAt[player] != GDX_MP_NO_TICK && tick >= ls->removedAt[player];
}

static void AdvanceKnown(GdxMpLockstep* ls, int player) {
    while (HasPad(ls, player, ls->known[player])) {
        ls->known[player]++;
    }
}

int gdx_mp_ls_init(GdxMpLockstep* ls, int numPlayers, int localPlayer, int delay, int hashEvery,
                   uint32_t session) {
    int p;
    uint32_t t;
    const GdxMpPad neutral = { 0, 0, 0 };

    memset(ls, 0, sizeof(*ls));
    if (numPlayers < 1 || numPlayers > GDX_MP_MAX_PLAYERS || localPlayer < 0 || localPlayer >= numPlayers ||
        delay < 1 || delay > GDX_MP_MAX_DELAY || 2 * delay + 2 > GDX_MP_REDUNDANCY || hashEvery < 1) {
        return -1;
    }
    ls->numPlayers = numPlayers;
    ls->localPlayer = localPlayer;
    ls->delay = delay;
    ls->hashEvery = hashEvery;
    ls->session = session;
    ls->newestLocalHashTick = GDX_MP_NO_TICK;
    ls->newestFrom = -1;
    ls->desyncPeer = -1;
    for (p = 0; p < GDX_MP_MAX_PLAYERS; p++) {
        for (t = 0; t < GDX_MP_RING; t++) {
            ls->slotTick[p][t] = GDX_MP_NO_TICK;
            ls->remoteHashTick[p][t] = GDX_MP_NO_TICK;
        }
    }
    for (t = 0; t < GDX_MP_RING; t++) {
        ls->localHashTick[t] = GDX_MP_NO_TICK;
    }
    for (p = 0; p < GDX_MP_MAX_PLAYERS; p++) {
        ls->removedAt[p] = GDX_MP_NO_TICK;
    }
    /* Ticks 0..delay-1 are neutral for everybody: nobody sampled anything for them. */
    for (p = 0; p < numPlayers; p++) {
        for (t = 0; t < (uint32_t) delay; t++) {
            StorePad(ls, p, t, neutral);
        }
        ls->known[p] = (uint32_t) delay;
        ls->acked[p] = (uint32_t) delay;
    }
    return 0;
}

int gdx_mp_ls_clamp_pad(GdxMpPad* pad) {
    const GdxMpPad in = *pad;
    pad->buttons = (uint16_t) (pad->buttons & GDX_MP_BUTTON_MASK);
    if (pad->stickX > GDX_MP_STICK_MAX) {
        pad->stickX = GDX_MP_STICK_MAX;
    } else if (pad->stickX < -GDX_MP_STICK_MAX) {
        pad->stickX = -GDX_MP_STICK_MAX;
    }
    if (pad->stickY > GDX_MP_STICK_MAX) {
        pad->stickY = GDX_MP_STICK_MAX;
    } else if (pad->stickY < -GDX_MP_STICK_MAX) {
        pad->stickY = -GDX_MP_STICK_MAX;
    }
    return in.buttons != pad->buttons || in.stickX != pad->stickX || in.stickY != pad->stickY;
}

void gdx_mp_ls_push_local(GdxMpLockstep* ls, uint32_t tick, GdxMpPad pad) {
    if (gdx_mp_ls_clamp_pad(&pad)) {
        ls->padsClamped++;
    }
    StorePad(ls, ls->localPlayer, tick + (uint32_t) ls->delay, pad);
    AdvanceKnown(ls, ls->localPlayer);
}

int gdx_mp_ls_first_missing(const GdxMpLockstep* ls, uint32_t tick) {
    int p;
    for (p = 0; p < ls->numPlayers; p++) {
        if (!HasPad(ls, p, tick) && !Removed(ls, p, tick)) {
            return p;
        }
    }
    return -1;
}

int gdx_mp_ls_ready(const GdxMpLockstep* ls, uint32_t tick) {
    return gdx_mp_ls_first_missing(ls, tick) < 0;
}

void gdx_mp_ls_get(const GdxMpLockstep* ls, uint32_t tick, GdxMpPad* out) {
    int p;
    const GdxMpPad neutral = { 0, 0, 0 };
    for (p = 0; p < ls->numPlayers; p++) {
        out[p] = Removed(ls, p, tick) ? neutral : ls->pads[p][Slot(tick)];
    }
}

int gdx_mp_ls_unacked(const GdxMpLockstep* ls) {
    const int me = ls->localPlayer;
    int i;
    for (i = 0; i < ls->numPlayers; i++) {
        if (i != me && ls->removedAt[i] == GDX_MP_NO_TICK && !ls->frozen[i] && ls->acked[i] < ls->known[me]) {
            return 1;
        }
    }
    return 0;
}

int gdx_mp_ls_build_packet(GdxMpLockstep* ls, uint32_t nowMs, uint8_t* out, int cap) {
    const int me = ls->localPlayer;
    const uint32_t newest = ls->known[me]; /* exclusive */
    uint32_t first;
    uint32_t count;
    uint32_t i;
    uint32_t hashTick = ls->newestLocalHashTick;
    uint32_t hash = 0;
    uint8_t* p;

    if (newest <= (uint32_t) ls->delay) {
        return 0; /* nothing sampled yet: the neutral prefix is implicit on every console */
    }
    first = newest;
    for (i = 0; i < (uint32_t) ls->numPlayers; i++) {
        /* A removed or frozen player acks nothing any more: it must not hold the window back. */
        if ((int) i != me && ls->removedAt[i] == GDX_MP_NO_TICK && !ls->frozen[i] && ls->acked[i] < first) {
            first = ls->acked[i];
        }
    }
    if (first >= newest) {
        first = newest - 1; /* everybody has everything: still send the newest as a heartbeat */
    }
    if (newest - first > GDX_MP_REDUNDANCY) {
        first = newest - GDX_MP_REDUNDANCY;
    }
    count = newest - first;
    if (cap < GDX_MP_HEADER_BYTES + 4 * (int) count) {
        return 0;
    }
    if (hashTick != GDX_MP_NO_TICK) {
        hash = ls->localHash[Slot(hashTick)];
    }
    memcpy(out, kMagic, 4);
    out[4] = GDX_MP_VERSION;
    out[5] = GDX_MP_TYPE_INPUT;
    out[6] = (uint8_t) me;
    out[7] = (uint8_t) count;
    PutU32(out + 8, first);
    PutU32(out + 12, hashTick);
    PutU32(out + 16, hash);
    PutU32(out + 20, nowMs);
    PutU32(out + 24, (ls->newestFrom >= 0) ? ls->peer[ls->newestFrom].newestSendMs : 0);
    PutU32(out + 28, ls->session);
    out[32] = (uint8_t) ((ls->newestFrom >= 0) ? ls->newestFrom : 0xFF);
    out[33] = ls->txFlags;
    out[34] = ls->wireSlot;
    out[35] = 0;
    for (i = 0; i < GDX_MP_MAX_PLAYERS; i++) {
        PutU32(out + 36 + 4 * i, ((int) i < ls->numPlayers) ? ls->known[i] : 0);
    }
    p = out + GDX_MP_HEADER_BYTES;
    for (i = 0; i < count; i++) {
        const GdxMpPad* pad = &ls->pads[me][Slot(first + i)];
        PutU16(p, pad->buttons);
        p[2] = (uint8_t) pad->stickX;
        p[3] = (uint8_t) pad->stickY;
        p += 4;
    }
    return GDX_MP_HEADER_BYTES + 4 * (int) count;
}

int gdx_mp_ls_packet_wire_slot(const uint8_t* pkt, int len) {
    if (gdx_mp_ls_packet_from(pkt, len) < 0) {
        return -1;
    }
    return pkt[34];
}

int gdx_mp_ls_packet_from(const uint8_t* pkt, int len) {
    if (len < GDX_MP_HEADER_BYTES || memcmp(pkt, kMagic, 4) != 0 || pkt[4] != GDX_MP_VERSION ||
        pkt[5] != GDX_MP_TYPE_INPUT || pkt[7] > GDX_MP_REDUNDANCY ||
        len != GDX_MP_HEADER_BYTES + 4 * pkt[7] || pkt[6] >= GDX_MP_MAX_PLAYERS) {
        return -1;
    }
    return pkt[6];
}

static void NoteDesync(GdxMpLockstep* ls, int peer, uint32_t tick, uint32_t local, uint32_t remote) {
    if (ls->desynced) {
        return;
    }
    ls->desynced = 1;
    ls->desyncPeer = peer;
    ls->desyncTick = tick;
    ls->desyncLocal = local;
    ls->desyncRemote = remote;
}

/* Returns 1 when this comparison found the first desync. */
static int CompareHashes(GdxMpLockstep* ls, int peer, uint32_t tick) {
    const uint32_t s = Slot(tick);
    const int wasDesynced = ls->desynced;
    if (ls->localHashTick[s] != tick || ls->remoteHashTick[peer][s] != tick || Removed(ls, peer, tick)) {
        return 0;
    }
    ls->peer[peer].hashesChecked++;
    if (ls->localHash[s] != ls->remoteHash[peer][s]) {
        NoteDesync(ls, peer, tick, ls->localHash[s], ls->remoteHash[peer][s]);
    }
    return !wasDesynced && ls->desynced;
}

int gdx_mp_ls_ingest(GdxMpLockstep* ls, const uint8_t* pkt, int len, uint32_t nowMs) {
    const int from = gdx_mp_ls_packet_from(pkt, len);
    uint32_t first;
    uint32_t i;
    uint32_t hashTick;
    uint32_t knownBefore;
    const uint8_t* p;

    if (from < 0 || from >= ls->numPlayers || from == ls->localPlayer) {
        ls->rejected++;
        return -1;
    }
    if (GetU32(pkt + 28) != ls->session) {
        ls->rejected++;
        ls->sessionRejected++;
        return -1;
    }
    if (ls->frozen[from] || ls->removedAt[from] != GDX_MP_NO_TICK) {
        ls->frozenRejected++;
        return -1;
    }
    first = GetU32(pkt + 8);
    knownBefore = ls->known[from];
    p = pkt + GDX_MP_HEADER_BYTES;
    for (i = 0; i < pkt[7]; i++) {
        const uint32_t tick = first + i;
        /* Only ticks inside the live window: already-known ticks are duplicates, and a tick a
         * full ring ahead of what we know would overwrite a slot still in use. */
        if (tick >= ls->known[from] && tick < ls->known[from] + GDX_MP_RING / 2) {
            GdxMpPad pad;
            pad.buttons = GetU16(p);
            pad.stickX = (int8_t) p[2];
            pad.stickY = (int8_t) p[3];
            if (gdx_mp_ls_clamp_pad(&pad)) {
                ls->padsClamped++;
            }
            StorePad(ls, from, tick, pad);
        }
        p += 4;
    }
    AdvanceKnown(ls, from);
    if (ls->known[from] != knownBefore) {
        ls->knownAtMs[from] = nowMs;
    }
    {
        const uint32_t ack = GetU32(pkt + 36 + 4 * ls->localPlayer);
        /* Monotonic, and never beyond what we have actually scheduled (a hostile ack cannot
         * make us skip ticks: it only shrinks the resend window). */
        if (ack > ls->acked[from] && ack <= ls->known[ls->localPlayer]) {
            ls->acked[from] = ack;
        }
    }

    ls->peerFlags[from] = pkt[33];
    ls->peerWireSlot[from] = pkt[34];
    ls->peer[from].packets++;
    ls->peer[from].bytes += (uint32_t) len;
    ls->peer[from].newestSendMs = GetU32(pkt + 20);
    ls->newestFrom = from;
    if (pkt[32] == (uint8_t) ls->localPlayer) {
        const uint32_t rtt = nowMs - GetU32(pkt + 24);
        ls->peer[from].rttLastMs = rtt;
        ls->peer[from].rttSumMs += rtt;
        ls->peer[from].rttSamples++;
        if (rtt > ls->peer[from].rttMaxMs) {
            ls->peer[from].rttMaxMs = rtt;
        }
    }

    hashTick = GetU32(pkt + 12);
    if (hashTick != GDX_MP_NO_TICK) {
        const uint32_t s = Slot(hashTick);
        ls->remoteHash[from][s] = GetU32(pkt + 16);
        ls->remoteHashTick[from][s] = hashTick;
        CompareHashes(ls, from, hashTick);
    }
    return from;
}

int gdx_mp_ls_note_local_hash(GdxMpLockstep* ls, uint32_t tick, uint32_t hash) {
    const uint32_t s = Slot(tick);
    int p;
    int found = 0;

    if ((tick % (uint32_t) ls->hashEvery) != 0) {
        return 0;
    }
    ls->localHash[s] = hash;
    ls->localHashTick[s] = tick;
    ls->newestLocalHashTick = tick;
    for (p = 0; p < ls->numPlayers; p++) {
        if (p != ls->localPlayer && CompareHashes(ls, p, tick)) {
            found = 1;
        }
    }
    return found;
}

/* ---- player removal ---------------------------------------------------------------------- */
void gdx_mp_ls_freeze(GdxMpLockstep* ls, int player) {
    if (player >= 0 && player < ls->numPlayers && player != ls->localPlayer) {
        ls->frozen[player] = 1;
    }
}

uint32_t gdx_mp_ls_fill(GdxMpLockstep* ls, int player, uint32_t first, const GdxMpPad* pads, int count) {
    int i;
    if (player < 0 || player >= ls->numPlayers || player == ls->localPlayer || count < 0) {
        return 0;
    }
    for (i = 0; i < count; i++) {
        const uint32_t tick = first + (uint32_t) i;
        if (tick >= ls->known[player] && tick < ls->known[player] + GDX_MP_RING / 2 && !HasPad(ls, player, tick)) {
            GdxMpPad pad = pads[i];
            if (gdx_mp_ls_clamp_pad(&pad)) {
                ls->padsClamped++;
            }
            StorePad(ls, player, tick, pad);
        }
    }
    AdvanceKnown(ls, player);
    return ls->known[player];
}

int gdx_mp_ls_newest_pads(const GdxMpLockstep* ls, int player, int max, uint32_t* first, GdxMpPad* out) {
    uint32_t start;
    uint32_t t;
    int n = 0;
    if (player < 0 || player >= ls->numPlayers || max <= 0) {
        *first = 0;
        return 0;
    }
    start = ls->known[player] > (uint32_t) max ? ls->known[player] - (uint32_t) max : 0u;
    if (ls->known[player] - start > GDX_MP_RING / 2) {
        start = ls->known[player] - GDX_MP_RING / 2;
    }
    *first = start;
    for (t = start; t < ls->known[player]; t++) {
        out[n++] = ls->pads[player][Slot(t)];
    }
    return n;
}

int gdx_mp_ls_remove(GdxMpLockstep* ls, int player, uint32_t tick) {
    if (player < 0 || player >= ls->numPlayers || player == ls->localPlayer || tick == GDX_MP_NO_TICK) {
        return -1;
    }
    if (ls->removedAt[player] != GDX_MP_NO_TICK) {
        return ls->removedAt[player] == tick ? 0 : -3;
    }
    if (ls->known[player] < tick) {
        return -2;
    }
    ls->frozen[player] = 1;
    ls->removedAt[player] = tick;
    return 0;
}

int gdx_mp_ls_is_removed(const GdxMpLockstep* ls, int player, uint32_t tick) {
    return player >= 0 && player < ls->numPlayers && Removed(ls, player, tick);
}
