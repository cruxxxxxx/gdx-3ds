/* port/gdx_mp_lobby.h — online multiplayer: the lobby state machine (pure, host-testable).
 *
 * One GdxMpLobby per console. The server's roster says who is in the room; GXLB HELLOs (4 Hz in
 * the lobby, 1 Hz in a race) say who is reachable, which build and game data they run, their
 * picks, whether they are ready, and what RTT they measure to everybody else. The host (slot 0)
 * turns that into a START: the players (ready, version-matched, heard within
 * GDX_LOBBY_STALE_MS), the course and VS settings, RNG seeds, a random session token, and an
 * input delay chosen from the worst RTT between any two players (gdx_mpo_delay_from_rtt). A
 * joiner adopts the newest valid START it hears from the host; its next HELLO acknowledges it.
 *
 * PLAYER INDEX = rank of the room slot among the START's players (the host is player 0), so the
 * lockstep layer keeps speaking dense indices 0..N-1 while the transport speaks slots. */
#ifndef GDX_MP_LOBBY_H
#define GDX_MP_LOBBY_H

#include "gdx_mp_online.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GDX_LOBBY_STALE_MS 4000 /* no HELLO for this long: not startable / shown as "..." */
#define GDX_LOBBY_RTT_WINDOW 8

typedef struct GdxMpLobbyPeer {
    int present; /* in the server roster */
    uint16_t epoch; /* the roster's epoch for the slot: a change = a different console */
    char name[GDX_MPO_NAME_LEN + 1];
    int heard;
    uint32_t lastHelloMs;
    GdxMpLbHello hello;
    int versionOk;
    /* RTT: the newest sendMs from them (echoed back) and when we got it. */
    uint32_t theirSendMs;
    uint32_t theirSendRxMs;
    int haveTheirSend;
    uint32_t rttMs; /* newest sample */
    uint16_t win[GDX_LOBBY_RTT_WINDOW];
    int winCount;
    int winPos;
    uint32_t samples;
} GdxMpLobbyPeer;

typedef struct GdxMpLobbySettings {
    uint8_t course;
    uint8_t vsCom;
    uint8_t handicap;
    uint8_t vsSlot;
    uint8_t difficulty;
    uint8_t hashEvery;
    uint8_t laps;      /* 1..9 (0 = 3) */
    uint8_t racers;    /* MP30: VS COM machine count 4..30 (0 = stock 4); ignored without vsCom */
    int delayOverride; /* 0 = pick from RTT */
} GdxMpLobbySettings;

typedef struct GdxMpLobby {
    int mySlot;
    uint16_t myEpoch; /* the roster's epoch for our own slot (a START names the console it means) */
    uint64_t secret;
    char build[GDX_MPO_BUILD_LEN + 1];
    uint32_t contentHash;
    uint32_t simAbi; /* MP30: the sim-ABI word (0 until the session sets it); HELLO/START must match */
    char name[GDX_MPO_NAME_LEN + 1];
    /* our own picks */
    uint8_t removedMask; /* racing: removals applied (sent in HELLO as the coordinator's ack) */
    uint8_t model;       /* GDX_MPO_MODEL_* of this console */
    int o3dsExtraDelay;  /* host: ticks added to the RTT delay when an Old 3DS races (0 after init;
                          * the session sets it from [net] mp_o3ds_extra_delay) */
    int ready;
    int machine;
    int skin;
    int engine;
    uint32_t engineBits; /* stockpick: the exact slider value (gdx_mpo_engine_bits); engine is it rounded */
    int bot;
    int state; /* GDX_LB_STATE_* */
    /* stockpick: the stock-menu picks (HELLO picking / pickRound / picked) and the course this console reports
     * on: the host's own pick, a joiner's copy of the host's (gdx_lobby_host_course), with this
     * console's key for it (0 = it does not have that course). */
    int picking; /* GDX_LB_PICK_* */
    /* The machine-select round: the host opens one on every console at once (pickRound + 1, its
     * pickMsLeft counting down from GDX_MPO_PICK_MS); a joiner follows the host's. picked = our
     * machine/skin/engine are a pick for pickRound (confirmed, or our cursor's at the timeout). */
    uint8_t pickRound;
    int picked;
    uint32_t pickMsLeft; /* the host: ms left in the round's machine step; GDX_LB_NO_ROUND = none */
    /* The host: every player must report its pick for the round before a START, until the host
     * stops waiting (pickWaitOver: the step's time and grace are up; a player with no pick then
     * races GDX_MPO_DEFAULT_MACHINE). requirePicks off = the mp_auto test lanes' ready rule. */
    int requirePicks;
    int pickWaitOver;
    /* The host: a START leaves out a member that has gone quiet (no HELLO for GDX_LOBBY_STALE_MS:
     * HOME, sleep, a lost link) instead of waiting for it. Set for one START (START WITHOUT). */
    int skipAway;
    int course;
    uint32_t courseKey;
    GdxMpLobbyPeer peer[GDX_MPO_MAX_SLOTS];
    /* START: the host's own, or the newest valid one heard from the host. */
    GdxMpLbStart start;
    int haveStart;
    uint32_t startGen;
    int startIsNew; /* set by ingest/make_start, cleared by the session when it consumes it */
    /* RM_*: the one just ingested (valid until the next ingest; gdx_mp_remove.h acts on it). */
    GdxMpLbRemove rm;
    /* ABORT: the newest heard (for the session). */
    GdxMpLbAbort abort;
    int abortFrom;
    int abortIsNew;
    uint32_t rejected;
    uint32_t versionRejected;
    uint32_t abiRejected; /* MP30: HELLOs / STARTs whose sim-ABI word differs from ours */
    uint32_t courseRejected; /* STARTs refused: their course / key is not ours */
} GdxMpLobby;

void gdx_lobby_init(GdxMpLobby* lb, int mySlot, uint64_t secret, const char* build, uint32_t contentHash,
                    const char* name);
/* stockpick. set_course: the course this console reports on and its key for it (the host: its
 * pick; a joiner: the host's, once it has computed its own key). host_course: the host's pick as
 * the host announces it (the host: lb->course), -1 = not heard yet. host_picking: the host's
 * HELLO.picking (the host: its own). */
void gdx_lobby_set_course(GdxMpLobby* lb, int course, uint32_t key);
int gdx_lobby_host_course(const GdxMpLobby* lb);
int gdx_lobby_host_picking(const GdxMpLobby* lb);
/* The host's machine-select round (the host: its own; 0 = none or not heard) and the ms left in its
 * machine step as the host last said (GDX_LB_NO_ROUND = none). peer_picked: slot (us too) has
 * reported a pick for the host's current round. picks_in: every present player has (us too). */
int gdx_lobby_host_round(const GdxMpLobby* lb);
/* mp-menus: a joiner hears the host launching or racing (HELLO.state): a race this console is not
 * in is running, so it waits for it to end (no mid-race joins on any transport). 0 for the host. */
int gdx_lobby_host_racing(const GdxMpLobby* lb);
uint32_t gdx_lobby_host_ms_left(const GdxMpLobby* lb);
int gdx_lobby_peer_picked(const GdxMpLobby* lb, int slot);
int gdx_lobby_picks_in(const GdxMpLobby* lb);
/* The server's roster: who is in the room. Departed peers are forgotten. */
void gdx_lobby_set_roster(GdxMpLobby* lb, const GdxMpRvRoster* roster);

/* Datagram builders for peer `dst`; 0 = nothing to send (dst absent / bad args). */
int gdx_lobby_build_hello(GdxMpLobby* lb, int dst, uint32_t nowMs, int directMask, uint8_t* out, int cap);
int gdx_lobby_build_start(GdxMpLobby* lb, int dst, uint32_t nowMs, uint8_t* out, int cap);
int gdx_lobby_build_abort(GdxMpLobby* lb, int dst, uint32_t nowMs, const GdxMpLbAbort* a, uint8_t* out, int cap);

/* One GXLB datagram. Returns its type (>= 1), or < 0 when refused (counted). *fromOut = the
 * sender slot. A START is adopted only from slot 0, only when newer than what we hold, and
 * only if it lists us (startIsNew is set). */
int gdx_lobby_ingest(GdxMpLobby* lb, const uint8_t* pkt, int len, uint32_t nowMs, int* fromOut);

/* 1 if the peer HELLOed recently and runs the same proto/build/content as us. */
int gdx_lobby_peer_live(const GdxMpLobby* lb, int slot, uint32_t nowMs);
/* RTT estimate (window max) and jitter ((max - min) / 2); GDX_LB_NO_RTT if none. */
uint32_t gdx_lobby_peer_rtt(const GdxMpLobby* lb, int slot);
uint32_t gdx_lobby_peer_jitter(const GdxMpLobby* lb, int slot);

/* Host only. How many players a START would take now (us + live, ready, version-matched peers),
 * and why not if < 2 (a short UI string). Every present peer must be live and ready: a room with
 * an unready member does not start without them. stockpick: nor while anyone (the host too) is in
 * a stock picker, nor before every player reports the host's course with the host's key. */
int gdx_lobby_host_startable(const GdxMpLobby* lb, uint32_t nowMs, const char** whyNot);
/* Host only. What a START WITHOUT would take (the quiet members left out) when only quiet
 * members hold the room up: the player count (>= 2) and the first quiet member's slot; 0 when
 * a normal START is possible, or when something else (a pick, the course, readiness) blocks it. */
int gdx_lobby_host_startable_without_away(const GdxMpLobby* lb, uint32_t nowMs, int* firstAwaySlot);
/* The worst RTT/jitter between any two of the would-be players (our own measurements and the
 * RTTs their HELLOs report), and the delay that implies. */
int gdx_lobby_pick_delay(const GdxMpLobby* lb, uint32_t nowMs, uint32_t* worstRtt, uint32_t* worstJitter);
/* Freezes a START (gen + 1). Returns the player count, 0 if not startable. */
int gdx_lobby_host_make_start(GdxMpLobby* lb, const GdxMpLobbySettings* s, uint32_t seed1, uint32_t mask1,
                              uint32_t seed2, uint32_t mask2, uint64_t token, uint32_t nowMs);
/* 1 when every player of the current START has acknowledged it (HELLO startGen >= gen). A player
 * whose console is no longer in its slot (gone, or the slot's epoch moved) or that was removed
 * from the race (removedMask) is not waited for: nothing would ever answer for it. */
int gdx_lobby_start_acked(const GdxMpLobby* lb);
/* 1 when player p's slot still holds the console the START named (present, same epoch). */
int gdx_lobby_start_player_here(const GdxMpLobby* lb, int p);

/* Our player index in the current START, -1 if we are not in it. */
int gdx_lobby_my_player(const GdxMpLobby* lb);
/* Slot of player index p in the current START, -1 if none. */
int gdx_lobby_player_slot(const GdxMpLobby* lb, int p);

#ifdef __cplusplus
}
#endif

#endif /* GDX_MP_LOBBY_H */
