/* port/gdx_mp_remove.c — mp-online: removing a player mid-race. Contract and protocol:
 * gdx_mp_remove.h. Pure: no socket, no clock, no global. */
#include "gdx_mp_remove.h"

#include <string.h>

void gdx_rm_reset(GdxMpRemoval* rm) {
    int i;
    memset(rm, 0, sizeof(*rm));
    rm->stage = GDX_RM_IDLE;
    rm->player = -1;
    rm->goneSlot = -1;
    for (i = 0; i < GDX_MPO_MAX_SLOTS; i++) {
        rm->removedTick[i] = GDX_MP_NO_TICK;
    }
}

static int IsRemoved(const GdxMpRemoval* rm, int player) {
    return player >= 0 && player < GDX_MPO_MAX_SLOTS && ((rm->removedMask >> player) & 1u) != 0;
}

int gdx_rm_coordinator_slot(const GdxMpLobby* lb, const GdxMpRemoval* rm, uint8_t aliveSlots) {
    int p;
    if (!lb->haveStart) {
        return -1;
    }
    for (p = 0; p < lb->start.numPlayers; p++) { /* player order is slot order */
        const int slot = lb->start.player[p].slot;
        if (!IsRemoved(rm, p) && ((aliveSlots >> slot) & 1u)) {
            return slot;
        }
    }
    return -1;
}

static int IsGone(const GdxMpRemoval* rm, int player) {
    return player >= 0 && player < GDX_MPO_MAX_SLOTS && ((rm->goneMask >> player) & 1u) != 0;
}

/* The slots that must answer: every racer but us, p, those already removed and those gone. */
static uint8_t OthersMask(const GdxMpLobby* lb, const GdxMpRemoval* rm, int player) {
    uint8_t mask = 0;
    int p;
    for (p = 0; p < lb->start.numPlayers; p++) {
        const int slot = lb->start.player[p].slot;
        if (p != player && slot != lb->mySlot && !IsRemoved(rm, p) && !IsGone(rm, p)) {
            mask |= (uint8_t) (1u << slot);
        }
    }
    return mask;
}

/* The slots of the gone players (bit = slot). */
static uint8_t GoneSlots(const GdxMpLobby* lb, uint8_t gonePlayers) {
    uint8_t mask = 0;
    int p;
    for (p = 0; p < lb->start.numPlayers; p++) {
        if ((gonePlayers >> p) & 1u) {
            mask |= (uint8_t) (1u << lb->start.player[p].slot);
        }
    }
    return mask;
}

static void Apply(GdxMpRemoval* rm, int player, uint32_t tick) {
    rm->removedMask |= (uint8_t) (1u << player);
    rm->removedTick[player] = tick;
}

static int Finish(GdxMpRemoval* rm, const GdxMpLobby* lb, GdxMpLockstep* ls, uint32_t nowMs) {
    rm->tick = ls->known[rm->player];
    if (gdx_mp_ls_remove(ls, rm->player, rm->tick) != 0) {
        return -1;
    }
    Apply(rm, rm->player, rm->tick);
    rm->goneSlot = lb->start.player[rm->player].slot;
    rm->tellPlayer = rm->player;
    rm->tellUntilMs = nowMs + GDX_RM_TELL_GONE_MS;
    rm->stage = GDX_RM_SETTING;
    rm->needMask = OthersMask(lb, rm, rm->player);
    rm->startMs = nowMs;
    rm->lastSendMs = 0;
    return 1;
}

/* Every racer that must answer has: T is the largest count anyone holds, and we must hold that many. */
static int CompletePoll(GdxMpRemoval* rm, const GdxMpLobby* lb, GdxMpLockstep* ls, uint32_t nowMs) {
    if (ls->known[rm->player] < rm->tick) {
        return -1; /* somebody holds pads of p that nobody could hand us: cannot agree on T */
    }
    return Finish(rm, lb, ls, nowMs);
}

int gdx_rm_begin(GdxMpRemoval* rm, const GdxMpLobby* lb, GdxMpLockstep* ls, int player, int reason, uint32_t nowMs) {
    const int me = gdx_lobby_my_player(lb);
    if (rm->stage != GDX_RM_IDLE || !lb->haveStart || player < 0 || player >= lb->start.numPlayers ||
        player == me || me < 0 || IsRemoved(rm, player) || player >= ls->numPlayers) {
        return 0;
    }
    gdx_mp_ls_freeze(ls, player);
    rm->player = player;
    rm->reason = reason;
    rm->gen = lb->start.gen;
    rm->tick = ls->known[player];
    rm->startMs = nowMs;
    rm->lastSendMs = 0;
    rm->needMask = OthersMask(lb, rm, player);
    rm->stage = GDX_RM_POLLING;
    if (rm->needMask == 0) {
        Finish(rm, lb, ls, nowMs);
    }
    return 1;
}

static int BuildRemove(const GdxMpLobby* lb, int type, int dst, uint32_t nowMs, const GdxMpLbRemove* body, uint8_t* out,
                       int cap) {
    GdxMpLbHeader h;
    memset(&h, 0, sizeof(h));
    h.type = (uint8_t) type;
    h.from = (uint8_t) lb->mySlot;
    h.to = (uint8_t) dst;
    h.secret = lb->secret;
    h.sendMs = nowMs;
    h.holdMs = 0xFFFFu;
    return gdx_lb_build(&h, body, out, cap);
}

static void FillPads(GdxMpLbRemove* b, const GdxMpLockstep* ls, int player, uint32_t endTick) {
    uint32_t first = 0;
    GdxMpPad pads[GDX_LB_RM_PADS];
    int n = gdx_mp_ls_newest_pads(ls, player, GDX_LB_RM_PADS, &first, pads);
    int i;
    b->count = 0;
    b->first = first;
    for (i = 0; i < n && first + (uint32_t) i < endTick; i++) {
        b->pads[i] = pads[i];
        b->count++;
    }
}

static int TellingGone(const GdxMpRemoval* rm, int dst, uint32_t nowMs) {
    return rm->goneSlot >= 0 && dst == rm->goneSlot && (int32_t) (rm->tellUntilMs - nowMs) > 0;
}

int gdx_rm_wants_send(const GdxMpRemoval* rm, uint32_t nowMs) {
    return rm->stage != GDX_RM_IDLE || (rm->goneSlot >= 0 && (int32_t) (rm->tellUntilMs - nowMs) > 0);
}

int gdx_rm_build_for(const GdxMpRemoval* rm, const GdxMpLobby* lb, const GdxMpLockstep* ls, int dst, uint32_t nowMs,
                     uint8_t* out, int cap) {
    GdxMpLbRemove b;
    if (dst < 0 || dst >= GDX_MPO_MAX_SLOTS) {
        return 0;
    }
    if (TellingGone(rm, dst, nowMs)) {
        memset(&b, 0, sizeof(b));
        b.gen = rm->gen;
        b.player = (uint8_t) rm->tellPlayer;
        b.reason = (uint8_t) GDX_LB_ABORT_PEER_LOST;
        b.tick = rm->removedTick[rm->tellPlayer];
        return BuildRemove(lb, GDX_LB_RM_SET, dst, nowMs, &b, out, cap); /* no pads: p needs none */
    }
    if (rm->stage == GDX_RM_IDLE || ((rm->needMask >> dst) & 1u) == 0) {
        return 0;
    }
    memset(&b, 0, sizeof(b));
    b.gen = rm->gen;
    b.player = (uint8_t) rm->player;
    b.reason = (uint8_t) rm->reason;
    if (rm->stage == GDX_RM_POLLING) {
        return BuildRemove(lb, GDX_LB_RM_POLL, dst, nowMs, &b, out, cap);
    }
    b.tick = rm->tick;
    FillPads(&b, ls, rm->player, rm->tick);
    return BuildRemove(lb, GDX_LB_RM_SET, dst, nowMs, &b, out, cap);
}

int gdx_rm_answer_poll(const GdxMpLobby* lb, GdxMpLockstep* ls, int fromSlot, const GdxMpLbRemove* poll, uint32_t nowMs,
                       uint8_t* out, int cap) {
    GdxMpLbRemove b;
    const int me = gdx_lobby_my_player(lb);
    if (!lb->haveStart || poll->gen != lb->start.gen || poll->player >= ls->numPlayers || (int) poll->player == me) {
        return 0;
    }
    gdx_mp_ls_freeze(ls, poll->player);
    memset(&b, 0, sizeof(b));
    b.gen = poll->gen;
    b.player = poll->player;
    b.reason = poll->reason;
    b.tick = ls->known[poll->player];
    FillPads(&b, ls, poll->player, b.tick);
    return BuildRemove(lb, GDX_LB_RM_HAVE, fromSlot, nowMs, &b, out, cap);
}

int gdx_rm_on_have(GdxMpRemoval* rm, const GdxMpLobby* lb, GdxMpLockstep* ls, int fromSlot,
                   const GdxMpLbRemove* have, uint32_t nowMs) {
    if (rm->stage != GDX_RM_POLLING || have->gen != rm->gen || (int) have->player != rm->player ||
        fromSlot < 0 || fromSlot >= GDX_MPO_MAX_SLOTS || ((rm->needMask >> fromSlot) & 1u) == 0) {
        return 0;
    }
    gdx_mp_ls_fill(ls, rm->player, have->first, have->pads, have->count);
    if (have->tick > rm->tick) {
        rm->tick = have->tick;
    }
    rm->needMask &= (uint8_t) ~(1u << fromSlot);
    if (rm->needMask != 0) {
        return 0;
    }
    return CompletePoll(rm, lb, ls, nowMs);
}

int gdx_rm_set_gone(GdxMpRemoval* rm, const GdxMpLobby* lb, GdxMpLockstep* ls, uint8_t gonePlayers, uint32_t nowMs) {
    uint8_t before;
    rm->goneMask = gonePlayers;
    if (rm->stage == GDX_RM_IDLE || !lb->haveStart) {
        return 0;
    }
    before = rm->needMask;
    rm->needMask &= (uint8_t) ~GoneSlots(lb, gonePlayers);
    if (rm->needMask != 0 || before == 0) {
        return 0;
    }
    if (rm->stage == GDX_RM_SETTING) {
        rm->stage = GDX_RM_IDLE; /* every live racer has acknowledged it */
        return 0;
    }
    return CompletePoll(rm, lb, ls, nowMs);
}

int gdx_rm_on_set(GdxMpRemoval* rm, const GdxMpLobby* lb, GdxMpLockstep* ls, const GdxMpLbRemove* set,
                  uint32_t nextTickToRun) {
    const int me = gdx_lobby_my_player(lb);
    int rc;
    if (!lb->haveStart || set->gen != lb->start.gen || set->player >= ls->numPlayers) {
        return 0;
    }
    if ((int) set->player == me) {
        return -2;
    }
    if (IsRemoved(rm, set->player)) {
        return rm->removedTick[set->player] == set->tick ? 0 : -1;
    }
    if (nextTickToRun > set->tick) {
        return -1; /* we already ran tick T with p's real pad: this console cannot follow */
    }
    gdx_mp_ls_freeze(ls, set->player);
    gdx_mp_ls_fill(ls, set->player, set->first, set->pads, set->count);
    rc = gdx_mp_ls_remove(ls, set->player, set->tick);
    if (rc != 0) {
        return -1;
    }
    Apply(rm, set->player, set->tick);
    return 1;
}

void gdx_rm_on_ack(GdxMpRemoval* rm, const GdxMpLobby* lb, int slot, uint8_t ackMask) {
    (void) lb;
    if (rm->stage != GDX_RM_SETTING || slot < 0 || slot >= GDX_MPO_MAX_SLOTS) {
        return;
    }
    if ((ackMask >> rm->player) & 1u) {
        rm->needMask &= (uint8_t) ~(1u << slot);
    }
    if (rm->needMask == 0) {
        rm->stage = GDX_RM_IDLE;
    }
}

int gdx_rm_tick(GdxMpRemoval* rm, uint32_t nowMs) {
    if (rm->stage == GDX_RM_POLLING && (int32_t) (nowMs - rm->startMs) > GDX_RM_POLL_GIVE_UP_MS) {
        return -1;
    }
    if (rm->stage == GDX_RM_SETTING && rm->needMask == 0) {
        rm->stage = GDX_RM_IDLE;
    }
    return 1;
}
