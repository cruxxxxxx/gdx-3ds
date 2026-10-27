/* port/gdx_mp_remove.h — mp-online: removing a player that dropped out of a race, deterministically.
 *
 * WHY. A console that crashes, loses WiFi or is switched off mid-race must not end the race for
 * everybody else. The others keep racing; its machine coasts (a neutral pad) from one agreed tick
 * on. "Agreed" is the whole problem: every console must switch that player to neutral at the SAME
 * lockstep tick, or they simulate different races.
 *
 * PROTOCOL (pure: no socket, no clock; GXLB RM_POLL / RM_HAVE / RM_SET, gdx_mp_online.h):
 *   1. The COORDINATOR -- the lowest room slot still racing, the host unless the host is the one
 *      leaving -- decides p is gone (it left the room, its slot changed hands, its own ABORT says
 *      it quit, or it has been silent for mp_wait_ms) and FREEZES p: it stops taking p's packets,
 *      so what it holds of p cannot grow. It sends RM_POLL(p) to every other racer.
 *   2. Each racer freezes p too and answers RM_HAVE: how many of p's pads it holds (known[p]) and
 *      the newest GDX_LB_RM_PADS of them.
 *   3. With every answer in, T = the largest known[p] anyone has. No console can have run tick T
 *      or later (that needs p's pad for T, which nobody has), and the coordinator now holds all of
 *      p's pads below T (the answers carried them). It applies REMOVE(p, T) itself and sends
 *      RM_SET(p, T, p's pads up to T) until every racer has acknowledged it (HELLO's removedMask).
 *   4. A racer fills the pads it lacked from RM_SET and applies REMOVE(p, T): from tick T on p is
 *      neutral everywhere and nobody waits for it. Everybody prints "p removed at tick T".
 *   A racer that never answers RM_POLL is gone too: after GDX_RM_POLL_GIVE_UP_MS the session ends.
 *   Racers the session already knows are gone (gdx_rm_set_gone: silent, left) are never polled
 *   or waited for, so two that drop at once are removed one after the other. RM_SET about
 *   ourselves = we were dropped: leave the race.
 * One removal at a time; a second waits for the first to finish. */
#ifndef GDX_MP_REMOVE_H
#define GDX_MP_REMOVE_H

#include "gdx_mp_lobby.h"
#include "gdx_mp_lockstep.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GDX_RM_RESEND_MS 250
#define GDX_RM_POLL_GIVE_UP_MS 8000
#define GDX_RM_TELL_GONE_MS 3000

enum {
    GDX_RM_IDLE = 0,
    GDX_RM_POLLING = 1, /* coordinator: waiting for RM_HAVE from needMask */
    GDX_RM_SETTING = 2, /* coordinator: REMOVE applied, RM_SET out until acked */
};

typedef struct GdxMpRemoval {
    int stage;
    int player;        /* player index being removed */
    int reason;        /* GDX_LB_ABORT_* */
    uint32_t gen;      /* the START it belongs to */
    uint8_t needMask;  /* slots whose RM_HAVE (POLLING) / ack (SETTING) we still need */
    uint32_t tick;     /* T, once known */
    uint32_t startMs;
    uint32_t lastSendMs;
    uint8_t removedMask; /* players removed so far this race (bit = player index) */
    uint8_t goneMask;    /* the session's word: these players are gone too (bit = player index); a
                          * removal never waits on them, so two that drop at once go one by one */
    int goneSlot;        /* coordinator: p's slot, told RM_SET (no ack) until tellUntilMs: a p that is
                          * alive after all (a partition, not a power cut) learns it was dropped
                          * instead of racing on and desyncing */
    int tellPlayer;
    uint32_t tellUntilMs;
    uint32_t removedTick[GDX_MPO_MAX_SLOTS];
} GdxMpRemoval;

void gdx_rm_reset(GdxMpRemoval* rm);

/* The coordinator for a race: the lowest slot among the START's players that is not removed and
 * is in `aliveSlots` (bit = slot: the consoles we still hear, plus ourselves). -1 = none. */
int gdx_rm_coordinator_slot(const GdxMpLobby* lb, const GdxMpRemoval* rm, uint8_t aliveSlots);

/* Coordinator: start removing player p. Freezes it. Returns 1 when started, 0 when busy / already
 * removed / not a racer / we are p. If no other racer needs polling, the removal completes at once
 * (stage SETTING, needMask 0, removedMask has p): the caller compares removedMask before/after. */
int gdx_rm_begin(GdxMpRemoval* rm, const GdxMpLobby* lb, GdxMpLockstep* ls, int player, int reason, uint32_t nowMs);

/* Builds what the coordinator sends to slot `dst` now (RM_POLL while polling, RM_SET while setting);
 * 0 when nothing is due for dst. */
int gdx_rm_build_for(const GdxMpRemoval* rm, const GdxMpLobby* lb, const GdxMpLockstep* ls, int dst, uint32_t nowMs,
                     uint8_t* out, int cap);

/* Coordinator: anything to (re)send now (polling, setting, or still telling p it was dropped)? */
int gdx_rm_wants_send(const GdxMpRemoval* rm, uint32_t nowMs);

/* A racer that got RM_POLL: freezes the player and builds the RM_HAVE answer to the sender. */
int gdx_rm_answer_poll(const GdxMpLobby* lb, GdxMpLockstep* ls, int fromSlot, const GdxMpLbRemove* poll, uint32_t nowMs,
                       uint8_t* out, int cap);

/* Coordinator: an RM_HAVE from slot `fromSlot`. Returns 1 when that completed the poll (REMOVE is
 * applied locally, stage SETTING), 0 otherwise, -1 when the pads do not reach T (the session cannot
 * continue consistently: abort). */
int gdx_rm_on_have(GdxMpRemoval* rm, const GdxMpLobby* lb, GdxMpLockstep* ls, int fromSlot,
                   const GdxMpLbRemove* have, uint32_t nowMs);

/* A racer that got RM_SET: fills and applies. 1 = applied now, 0 = already had it, -1 = cannot
 * (we already ran tick T or later, or pads missing: abort), -2 = it is about us (we were dropped). */
int gdx_rm_on_set(GdxMpRemoval* rm, const GdxMpLobby* lb, GdxMpLockstep* ls, const GdxMpLbRemove* set,
                  uint32_t nextTickToRun);

/* Coordinator: slot `slot`'s HELLO says it has applied removals `ackMask` (bit = player). */
void gdx_rm_on_ack(GdxMpRemoval* rm, const GdxMpLobby* lb, int slot, uint8_t ackMask);

/* The session's gone set (bit = player index: every racer it has flagged, the one being removed
 * among them). Gone racers are not polled and not waited for (RM_HAVE, the RM_SET ack): a second
 * racer that went dark with the first must not hold the first removal up until it gives up.
 * Returns 1 when that completed a poll (REMOVE applied: the caller compares removedMask), -1 when
 * the pads the others hold do not reach T (abort, as gdx_rm_on_have), 0 otherwise. */
int gdx_rm_set_gone(GdxMpRemoval* rm, const GdxMpLobby* lb, GdxMpLockstep* ls, uint8_t gonePlayers, uint32_t nowMs);

/* Timers (coordinator): 1 = keep going, -1 = gave up (a polled racer never answered: abort). */
int gdx_rm_tick(GdxMpRemoval* rm, uint32_t nowMs);

#ifdef __cplusplus
}
#endif

#endif /* GDX_MP_REMOVE_H */
