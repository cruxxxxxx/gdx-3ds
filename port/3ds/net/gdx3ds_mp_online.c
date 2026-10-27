/* port/3ds/net/gdx3ds_mp_online.c — mp-online controller. Contract + ini keys: gdx3ds_mp_online.h. */
#include "gdx3ds_mp_online.h"
#include "gdx3ds_mp_game.h"
#include "../gdx3ds_mp_pick.h"
#include "gdx3ds_mp_p2p.h"
#include "gdx3ds_mp_socthr.h"
#include "gdx3ds_mp_radio.h"
#include "gdx3ds_mp_discovery.h"
#include "gdx3ds_mp_uds.h"
#include "gdx3ds_mp_ipcstat.h"
#include "gdx3ds_mp_timesync.h"
#include "gdx3ds_mp_netab.h"
#include "gdx_mp_uds_bridge.h"
#include "gdx_mp_lan.h"
#include "gdx_mp_lobby.h"
#include "gdx_mp_remove.h"
#include "gdx_mp_lockstep.h"
#include "gdx_mp_pause.h"

#include "gdx3ds_config.h"
#include "gdx_racemap.h"
#include "gdx3ds_filelog.h"

#include <3ds.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#if defined(__has_include)
#if __has_include("gdx3ds_build_id.h")
#include "gdx3ds_build_id.h"
#endif
#endif
#ifndef GDX3DS_BUILD_ID
#define GDX3DS_BUILD_ID "unknown"
#endif

extern unsigned int gdx_gstate_hash(int* outRacers);
extern unsigned int gdx_mpo_vis_hash(unsigned int h); /* port/decomp_port.c: MP30 visibility masks */
extern unsigned int gdx_mpo_racer_hash(int index);
extern int gdx_mpo_racer_describe(int index, char* out, int cap);
extern int gdx_mpo_racer_raw(int index, void* out, int cap);
extern int gdx_input_game_mode(void);
extern int gRandSeed1;
extern int gRandSeed2;
extern int gCourseIndex;
extern int gTotalRacers; /* decomp: the race's field (MP30: up to 30 with VS COM) */
extern const char* gdx3ds_fs_base_path(void);
/* gdx3ds_mp.c: soc:U, shared with the dev link when it is up. */
extern int gdx3ds_mp_ensure_soc(void);

#define MPO_DEFAULT_SERVER_PORT 27200
#define MPO_DEFAULT_BIND_PORT 27300
#define MPO_HELLO_LOBBY_MS 250
#define MPO_HELLO_RACE_MS 1000
#define MPO_START_RESEND_MS 250
#define MPO_ABORT_RESEND_MS 200
#define MPO_ABORT_REPEAT_MS 2000
#define MPO_LINGER_MS 3000
#define MPO_LAUNCH_GIVE_UP_MS 120000
/* CRASHFIX (hw b5f620a: the New 3DS host died at the race launch and the Old 3DS guest sat frozen
 * on its loaded race): a peer that has sent no race pad yet is still loading, and was given the
 * whole launch's patience (mp_launch_ms, 2 minutes) before it was dropped, inside WaitReady's
 * blocking loop. A console keeps sending HELLOs while it loads (the "[mpo] pN: first race pad"
 * receipt logs the longest silence before its first pad: 0.8-1.1 s on the emulator gates, up to
 * 7.3 s when the emulator itself stalls at the race start), so a peer silent this long has gone,
 * not slowed down: it is removed and the CPU drives its machine on. */
#define MPO_LOAD_SILENCE_MS 15000
#define MPO_TICK_US(t) ((unsigned long) ((t) / (SYSCLOCK_ARM11 / 1000000ull)))

typedef struct MpoConfig {
    GdxMpEndpoint server;
    char room[GDX_MPO_CODE_LEN + 1];
    char name[GDX_MPO_NAME_LEN + 1];
    char autoMode[8];
    char wantCode[GDX_MPO_CODE_LEN + 1];
    int bindPort;
    int forceRelay;
    int rvCookie;     /* [net] mp_rv_cookie: GDX_RV_COOKIE_MODE_* (default auto) */
    int rvProto;      /* [net] mp_rv_proto: test lever, the client proto v2 requests carry */
    int simSymmetric;
    int maxPlayers;
    int delay;
    int waitMs;
    int launchMs;
    int loadSilenceMs; /* [net] mp_load_silence_ms: a still-loading peer silent this long is gone */
    int wifiCheck;
    int hashEvery;
    int autostart;
    int races;
    int course;
    int vsCom;
    int bot;
    int endAfter;
    int desyncAt;
    int leaveAt;
    int vanishAt;
    int suspendAt; /* [net] mp_suspend_at: MP30 test lever, HOME / suspend at race tick N */
    int statsEvery;
    int rateMs;       /* [net] mp_rate_ms: the [mprate] cadence in a race (0 = off) */
    int hashLog;
    int dropMs;
    int waitQuietMs;  /* [net] mp_wait_quiet_ms: a waiting console only heartbeats to a peer this quiet */
    int hashDeep;
    int bgmHoldUntil; /* [net] mp_bgm_hold_until (MP30 test lever) */
    int racers;       /* [net] mp_racers / the lobby's Machines option: 0 = the default (MP30) */
    int racersO3ds30; /* [net] mp_racers_o3ds_30: lift the Old 3DS 16-machine cap (MP30 lever) */
    int dropMode;     /* [net] mp_drop: MPO_DROP_CPU (default) / RETIRE / COAST (MP30) */
    int haveServer;   /* [net] rendezvous parsed: the ONLINE tab works */
    int lan;          /* [net] mp_lan: the NEARBY tab (same-WiFi rooms) */
    int lanBroadcast; /* [net] mp_lan_broadcast */
    char lanPeers[64];
    int lanTimeoutMs;
    int laps;
    int uds;          /* [net] mp_uds: NEARBY also lists / hosts local-wireless rooms */
    int costEvery;    /* [net] mp_cost_every: the [mpcost] receipt's window (frames, 0 = off) */
    int stockPick;    /* [net] mp_stock_pick: rooms pick with the stock top-screen menus */
    int pickMs;       /* [net] mp_pick_ms: the machine step's time (test lever; <= GDX_MPO_PICK_MS) */
    int holdMs;       /* mp-menus #10 [ui] hold_confirm_ms: the pauser's A on a row that ends the race */
    int courseMs;     /* mp-menus [net] mp_course_ms: the host's course step's time (0 = no limit) */
    int raceDiscMs;    /* [net] mp_race_disc_ms: the discovery socket's period in a WiFi race (default
                        * 1000: beacons stay 1 Hz; 0 = every frame as before) */
    int udsDiscPollMs; /* NETTICK [net] mp_uds_disc_poll_ms: the WiFi discovery socket's period in a
                        * local wireless room (no WiFi then; default 250, 0 = every frame as 90049af) */
    int logRaceDefer;  /* [debug] filelog_race_defer: -1 (default) = Old 3DS only, 0 = off, 1 = every
                        * console. From START to the room, explicit log flushes only wake the async
                        * writer (gdx3ds_filelog_set_race_defer); crash and fatal lines stay synchronous. */
    int o3dsExtraDelay; /* [net] mp_o3ds_extra_delay (default 0, 0..4): host-side ticks added to the
                         * RTT delay when an Old 3DS races (d07ae82 had a fixed +3) */
    int launchSyncRaceTicks; /* [debug] filelog_launch_sync_race_ticks (default 2): the
                              * filelog_launch_sync_ms window ends once the race has run this
                              * many ticks (the first race frame, its 2.3 s texture load, is
                              * covered); -1 = the whole window. hw 158a13e: the New 3DS host
                              * spent 3.0 s on 465 synchronous lines in the race's first seconds. */
    int launchSyncO3ds; /* [debug] filelog_launch_sync_o3ds (default 0): an Old 3DS honours
                         * filelog_launch_sync_ms only when 1 (90049af: ~0.5 s of synchronous launch lines) */
    /* mp-online-hw: SERVERLESS ONLINE ([net] mp_direct). A console hosts a NEARBY WiFi room (the
     * GXRV room it already runs for same-WiFi play, port/gdx_mp_lan.h) and a friend joins it by the
     * host's address instead of by beacon: the same room code, secret, roster and relay checks.
     * Across the internet the host's router must forward its game port (UDP mp_bind_port, 27300).
     *   mp_direct = host           a WiFi room this console hosts shows its code (and the log
     *                              says the address to give out)
     *   mp_direct = a.b.c.d[:port] ONLINE gets JOIN BY ADDRESS: that console's room, by its code */
    int directHost;
    int haveDirect;
    GdxMpEndpoint direct;
    int relayHoldL;     /* [net] mp_relay_hold_l (test lever, default 0): L held on HOST ONLINE ROOM asks
                         * the server for a relay-only room (the CREATE flag): a forced-relay race in
                         * the same launch as a direct one */
    int onlineDiscOff;  /* [net] mp_online_disc_off (default 1; 0 = as f9aaebd): no NEARBY discovery
                         * service while in an online-server room (one SOC recvfrom a frame less) */
    int readySkip;      /* [net] mp_ready_skip (default 1; 0 = as f9aaebd): in a socket room (not local
                         * wireless) a race tick whose pads are all in runs without draining the
                         * socket again first (after_tick drains every frame anyway) */
} MpoConfig;

/* NETTICK: the frameskip learns when a lockstep race runs and how long each tick waited for the
 * peers (port/3ds/gdx3ds_frameskip.c). Weak: a harness links the net library without it. */
extern void gdx3ds_frameskip_set_lockstep(int on) __attribute__((weak));
extern void gdx3ds_frameskip_note_net_wait(uint64_t ticks) __attribute__((weak));

enum { MPO_DROP_CPU = 0, MPO_DROP_RETIRE = 1, MPO_DROP_COAST = 2 };

static MpoConfig sCfg;
static int sPhase = GDX_MPO_OFF;
static int sSocketOpen = 0;
static uint32_t sContentHash = 0;
static uint32_t sSimAbi = 0;      /* MP30: gdx3ds_mp_sim_abi(), also folded into sContentHash */
static char sBuild[GDX_MPO_BUILD_LEN + 1];
static GdxMpLobby sLb;
static int sLbInit = 0;
static uint32_t sLbRosterSeen = 0;
static GdxMpLockstep* sLs = NULL;
static uint32_t sSession = 0;
#define MPO_AUTOJOIN_RETRIES 20
#define MPO_AUTOJOIN_EVERY_MS 7000
static int sAutoJoinRetries = 0;
#define MPO_AUTOSTART_STABLE_MS 10000
static int sAutoStableCount = -1;
static uint32_t sAutoStableRoster = 0;
static uint32_t sAutoStableSinceMs = 0;   /* mp_auto=join: ROOM_NOT_FOUND retries so far */
static uint32_t sAutoJoinAtMs = 0; /* 0 = none pending */
static uint16_t sSessionEpoch[GDX_MPO_MAX_SLOTS]; /* each racer's roster epoch when the START was taken */
static char sSessionName[GDX_MPO_MAX_SLOTS][GDX_MPO_NAME_LEN + 1]; /* and its name (it may leave) */
/* Player removal (gdx_mp_remove.h): the protocol state, the players some trigger flagged as gone
 * (the coordinator acts on them), when each slot was last heard, and how far we have run. */
static GdxMpRemoval sRm;
static uint8_t sDropWanted = 0;   /* bit = player index */
static uint8_t sDropReason[GDX_MPO_MAX_SLOTS];
static uint32_t sLastHeardMs[GDX_MPO_MAX_SLOTS];
static uint32_t sLastRmSendMs = 0;
static uint32_t sRanTicks = 0;    /* lockstep ticks run so far (the next one to run) */
static uint8_t sHeardPads = 0;    /* bit = slot: has sent us race pads (before that: still loading) */
static uint32_t sLoadSilenceMaxMs[GDX_MPO_MAX_SLOTS]; /* the longest silence of a still-loading peer */
static uint8_t sRetiredMask = 0;  /* removed players whose machine has retired (RetireRemoved) */
static uint8_t sCpuNotedMask = 0; /* MP30: removed players whose CPU takeover was logged */
static int sVanished = 0;         /* test lever mp_vanish_at: this console went dark */
/* NEARBY: the discovery backend (gdx3ds_mp_discovery.h), our beacon while we host a LAN room. */
static const GdxMpDiscoveryOps* sDisc = NULL;
static int sDiscOpen = 0;
static int sDiscError = 0;
static uint32_t sLanHostId = 0;
static char sLanCode[GDX_MPO_CODE_LEN + 1];
static uint32_t sLanAutoJoinMs = 0;
static int sLanAutoJoinUdsOnly = 0;
/* NEARBY over local wireless (gdx3ds_mp_uds.h + port/gdx_mp_uds_bridge.h): the scan's rooms, our
 * room while we host one, a join waiting for the radio to associate. */
#define MPO_UDS_JOIN_GIVE_UP_MS 20000
#define MPO_UDS_UI_SEEN_MS 3000 /* the MP page repaints once a second */
static GdxMpUdsRoom sUdsRooms[GDX_MP_UDS_NEARBY_MAX];
static int sUdsRoomCount = 0;
static int sUdsScanOn = 0;
static int sUdsHeld = 0;           /* MPFIX: UDS may be initialised: release it once NEARBY closes */
static int sUdsError = 0;          /* the last scan / host failure (GDX_UDS_E_*), 0 = none */
static char sUdsWhy[128];
static uint32_t sUiSeenMs = 0;     /* the MP tab painted us last (gdx3ds_mpo_ui) */
/* MPFIX: the NEARBY sockets open when the list is first painted (or a test lever needs them) and
 * close again once it has been off screen this long with no room: no socket call at all while
 * nobody looks at NEARBY (hw 516011c listened from boot, two recvfrom()s a frame). */
#define MPO_NEARBY_LINGER_MS 10000
#define MPO_SOCKET_RETRY_MS 3000
static uint32_t sSocketRetryAtMs = 0; /* a failed lazy open waits this long before the next try */
/* MPFIX: the [mpcost] receipt -- what multiplayer costs the main thread per frame, in a window of
 * [net] mp_cost_every frames: the frame tail's service (gdx3ds_mpo_after_tick) plus the race's
 * pad exchange (gdx3ds_mpo_exchange, which includes waiting for the others' pads). */
static struct {
    uint32_t frames;    /* frames in this window */
    uint32_t live;      /* ... with a socket open, a scan on or a room */
    uint32_t over2ms;   /* ... costing more than 2 ms */
    uint64_t tailTicks; /* svcGetSystemTick ticks in after_tick */
    uint64_t exchTicks; /* ... in exchange */
    uint64_t waitTicks; /* NETTICK: ... of which WaitReady slept/polled for a peer (the rest is CPU) */
    uint32_t maxTicks;  /* the dearest frame (both) */
    uint32_t frameTicks; /* the current frame so far */
    uint32_t opens;     /* lazy socket opens / closes in this window */
    uint32_t closes;
} sCost;
static int sUdsInRoom = 0;         /* 1 = our room is a local-wireless one (hosted or joined) */
static int sUdsHosting = 0;
static GdxMpBeacon sUdsBeacon;     /* what we advertise while hosting */
static int sUdsJoinPending = 0;
static uint32_t sUdsJoinStartMs = 0;
static char sUdsJoinCode[GDX_MPO_CODE_LEN + 1];
static char sUdsJoinName[GDX_MPO_NAME_LEN + 1];
static int sModel = GDX_MPO_MODEL_UNKNOWN;
static unsigned long long sBaseTick = 0;
static int sHaveTick = 0;
static uint32_t sLastLt = 0;
static int sBgmReadyAll = 1; /* MP30: this race tick's pads all say "BGM ready" (gdx3ds_mpo_pads_bgm_ready) */
static uint32_t sSimLt = 0;  /* MP30: the race tick the game is simulating (its pads were just handed over) */
static int sAnyRemotePad = 0;
static uint32_t sLastHelloMs = 0;
static uint32_t sLastStartSendMs = 0;
static GdxMpLbAbort sAbortMsg;
static uint32_t sAbortUntilMs = 0;
static uint32_t sLastAbortSendMs = 0;
static uint32_t sAbortSlots = 0;
static uint32_t sLingerUntilMs = 0;
static uint32_t sLaunchMs = 0;
static int sLaunchSyncOn = 0; /* a filelog_launch_sync_ms window is open (LaunchSyncLog) */
static int sRacesStarted = 0;
static int sRacesDone = 0;
static int sDesyncReported = 0;
static int sSleepWasAllowed = 1;
static int sSleepHeld = 0;
static uint32_t sBotLcg = 1;
static int sBotStickX = 0;
static uint16_t sBotButtons = 0;
static char sMessage[128];
static int sLoggedPhase = -1;
/* stats */
static void RateReset(void);
static uint32_t sStallTicks = 0;
static uint32_t sStallMaxUs = 0;
static uint64_t sStallUs = 0;
static uint32_t sRejected = 0;
/* stock picks */
static uint32_t sPickDeadlineMs = 0; /* this console's end of the round's machine step (0 = none) */
static int sPickWanted = 0;          /* GDX_MPP_*: a picker to open once the top screen can */
static int sCourseStepOpened = 0;    /* host: the round's course step has been opened */
static int sRoomCourse = 0;          /* host: the course the room races */
/* mp-menus: this console's newest machine pick (any room): a rejoiner's machine select starts on
 * it, and KEEP confirms it in one tap. */
typedef struct LastPick {
    int valid;
    int machine;
    int skin;
    uint32_t engineBits;
} LastPick;
static LastPick sLastPick;
/* mp-menus, host: a player who joins once the room has had a round (a rejoin after a drop, or a
 * late joiner) gets a machine pick of its own: a new round for everyone, as MACHINE SELECT. */
static uint8_t sKnownMask = 0;                   /* slots in the room and heard */
static uint16_t sKnownEpoch[GDX_MPO_MAX_SLOTS];  /* ... and their roster epochs */
static int sAutoRoundSlot = -1;                  /* the joiner waiting for its round, -1 = none */
static int sCourseChosen = 0;                    /* host: a course step finished in this room */
static int sAutoRound = 0;                       /* host: the running round is an automatic one */
/* mp-menus: the room went away under a race (the host left, the server forgot us or went
 * silent): the race goes on over its direct links to its end and its results; then the room is
 * CLOSED -- no more network, the results stay on screen until the player leaves. */
static int sRoomLostInRace = 0;
static int sRoomClosed = 0;
static char sRoomClosedWhy[48];
/* mp-menus #10: the shared pause */
static GdxMpPauseGate sPauseGate;
static int sPauseTouch = 0;     /* the bottom screen's pause button is held */
static int sPauseShown = -1;    /* the pauser the receipt last logged, -1 = not paused */
static char sEndedByName[GDX_MPO_NAME_LEN + 1]; /* the last race was ended from the pause menu by */
/* mp-menus: how this console entered its room as a guest, to rejoin it when its membership
 * expired (HOME or sleep for longer than the server keeps a silent member). */
enum { MPO_REJOIN_NONE = 0, MPO_REJOIN_ONLINE = 1, MPO_REJOIN_LAN = 2, MPO_REJOIN_DIRECT = 3 };
#define MPO_REJOIN_SPACING_MS 60000
static int sRejoinKind = MPO_REJOIN_NONE;
static char sRejoinCode[GDX_MPO_CODE_LEN + 1];
static GdxMpEndpoint sRejoinHost;
static uint32_t sRejoinAtMs = 0; /* the last automatic rejoin (one a minute at most) */
/* mp-menus: the console each slot's running points belong to (a new one starts from 0). */
static uint16_t sPointsEpoch[GDX_MPO_MAX_SLOTS];
static uint8_t sPointsEpochKnown = 0; /* bit = slot */
/* mp-menus, host: the course step's deadline ([net] mp_course_ms), 0 = none running */
static uint32_t sCourseDeadlineMs = 0;
static int sRaceWait = 0; /* a joiner: the room races without this console (ServiceRaceWait) */
/* mp-menus: a NEARBY room picked while it races (its beacon says so): this console waits, out of
 * the room, and joins the moment the beacon says the race is over (ServiceNearbyWait). */
#define MPO_NEARBY_WAIT_GONE_MS 15000
static struct {
    int active;
    uint32_t hostId;
    int medium;
    char name[GDX_MPO_NAME_LEN + 1];
    uint32_t goneSinceMs;
} sNearbyWait;

/* The room's running VS points (the results screen, bottom-menus M3): 5/3/1/0 by place among the
 * humans in the last race's field, per slot; reset when the room closes or the slot empties. A
 * machine a CPU finished (its player was removed) scores nothing. Every console computes the same
 * numbers from the same lockstepped race. */
static const int kVsPoints[4] = {5, 3, 1, 0};
static int sPoints[GDX_MPO_MAX_SLOTS];
static int sRacePoints[GDX_MPO_MAX_SLOTS];
static int sRacePlace[GDX_MPO_MAX_SLOTS];
static uint8_t sByCpuSlots = 0;
/* The race dash's drop toast (gdx3ds_mpo_race_view) and stall banner (gdx3ds_bui_race_stall). */
static int sDropSerial = 0;
static int sDropPlayer = -1;
static uint32_t sStallNoteMs = 0;
static int sStallShown = 0;

static void MpoLog(const char* fmt, ...) {
    char line[640];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n <= 0) {
        return;
    }
    if (n >= (int) sizeof(line)) {
        n = (int) sizeof(line) - 1;
    }
    svcOutputDebugString(line, n);
    gdx3ds_filelog_write(line, (size_t) n);
}

static uint32_t NowMs(void) {
    return (uint32_t) osGetTime();
}

static void SetMessage(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(sMessage, sizeof(sMessage), fmt, ap);
    va_end(ap);
    MpoLog("[mpo] message: %s", sMessage);
}

/* A refused CREATE / JOIN (BUILD / CONTENT mismatch): which side should update, when the server
 * said (a 2.1.0 server tells a v2 joiner the host's proto). */
static void SetMismatchMessage(const GdxP2pStatus* st) {
    GdxMpRvError e;
    char m[128];
    memset(&e, 0, sizeof(e));
    e.code = (uint8_t) st->errorCode;
    e.hostProto = st->errorHostProto;
    snprintf(e.detail, sizeof(e.detail), "%s", st->errorDetail);
    gdx_rv_error_message(&e, st->clientProto, m, sizeof(m));
    SetMessage("%s", m);
}

/* 1 = this console's log flushes are deferred in `phase` ([debug] filelog_race_defer). */
static int LogDefersIn(int phase) {
    const int on = sCfg.logRaceDefer < 0 ? (sModel == GDX_MPO_MODEL_O3DS) : sCfg.logRaceDefer;
    return on && (phase == GDX_MPO_LAUNCH || phase == GDX_MPO_RACE || phase == GDX_MPO_LINGER);
}

/* A room / race: NDM exclusive (infrastructure only), the dev net thread quiet, the log's MP
 * reserve (gdx3ds_mp_radio.h). */
static void ApplyRadioPolicy(void) {
    gdx3ds_mp_radio_update(sPhase >= GDX_MPO_ROOM, !sUdsInRoom && !sUdsJoinPending);
    gdx3ds_socthr_set_sole(sPhase == GDX_MPO_RACE && !sUdsInRoom);
}

static void SetPhase(int phase) {
    if (phase != sPhase) {
        static const char* const kName[] = { "off", "idle", "connecting", "room", "launch", "race", "linger" };
        const int defer = LogDefersIn(phase);
        MpoLog("[mpo] phase %s -> %s", kName[sPhase], kName[phase]);
        if (defer != LogDefersIn(sPhase)) {
            MpoLog("[mpo] log flushes %s (debug.filelog_race_defer=%d)",
                   defer ? "deferred to the async writer until the room" : "synchronous again", sCfg.logRaceDefer);
        }
        gdx3ds_filelog_set_race_defer(defer);
        gdx3ds_filelog_flush();
        if (gdx3ds_frameskip_set_lockstep != NULL) {
            gdx3ds_frameskip_set_lockstep(phase == GDX_MPO_RACE);
        }
    }
    sPhase = phase;
    ApplyRadioPolicy();
}

static int EnsureSocket(void);
static uint32_t Mix32(uint32_t x);

/* ---- config --------------------------------------------------------------------------------- */
static void CopyCfgString(char* dst, int cap, const char* section, const char* key) {
    const char* v = gdx3ds_config_get_string(section, key, "");
    snprintf(dst, (size_t) cap, "%s", v != NULL ? v : "");
}

static int ClampInt(int v, int lo, int hi) {
    if (v < lo) {
        return lo;
    } else if (v > hi) {
        return hi;
    }
    return v;
}

/* [net] rendezvous as this build reads it: the ini's value, else GDX_MPO_DEFAULT_RENDEZVOUS (the
 * hosted server); "" when online rooms are off ("" or "off" in the ini). */
static const char* RendezvousSetting(void) {
    const char* rv = gdx3ds_config_get_string("net", "rendezvous", GDX_MPO_DEFAULT_RENDEZVOUS);
    if (rv == NULL || rv[0] == '\0' || strcasecmp(rv, "off") == 0) {
        return "";
    }
    return rv;
}

/* [net] mp_lan: NEARBY rooms (default GDX_MPO_DEFAULT_LAN, on). */
static int LanSetting(void) {
    return gdx3ds_config_get_bool("net", "mp_lan", GDX_MPO_DEFAULT_LAN) ? 1 : 0;
}

static void LoadDirectConfig(void) {
    char direct[32];
    CopyCfgString(direct, sizeof(direct), "net", "mp_direct");
    sCfg.directHost = strcmp(direct, "host") == 0;
    sCfg.haveDirect = !sCfg.directHost && direct[0] != '\0' &&
                      gdx_mpo_parse_hostport(direct, &sCfg.direct, MPO_DEFAULT_BIND_PORT);
    if (direct[0] != '\0' && !sCfg.directHost && !sCfg.haveDirect) {
        MpoLog("[mpo] init: [net] mp_direct=\"%s\" is neither \"host\" nor a dotted IPv4 host[:port]", direct);
    }
}

static void LoadConfig(void) {
    char nat[16];
    char relay[16];
    char rvCookie[16];
    char rawName[32];
    memset(&sCfg, 0, sizeof(sCfg));
    CopyCfgString(sCfg.room, sizeof(sCfg.room), "net", "room");
    CopyCfgString(sCfg.autoMode, sizeof(sCfg.autoMode), "net", "mp_auto");
    CopyCfgString(sCfg.wantCode, sizeof(sCfg.wantCode), "net", "mp_code");
    CopyCfgString(rawName, sizeof(rawName), "net", "mp_name");
    CopyCfgString(nat, sizeof(nat), "net", "mp_sim_nat");
    CopyCfgString(relay, sizeof(relay), "net", "mp_relay");
    CopyCfgString(rvCookie, sizeof(rvCookie), "net", "mp_rv_cookie");
    CopyCfgString(sCfg.lanPeers, sizeof(sCfg.lanPeers), "net", "mp_lan_peers");
    sCfg.lan = LanSetting();
    sCfg.lanBroadcast = gdx3ds_config_get_bool("net", "mp_lan_broadcast", 1);
    sCfg.lanTimeoutMs = ClampInt(gdx3ds_config_get_int("net", "mp_lan_timeout_ms", 15000), 3000, 3600000);
    sCfg.laps = ClampInt(gdx3ds_config_get_int("net", "mp_laps", 3), 1, 9);
    sCfg.racers = gdx3ds_config_get_int("net", "mp_racers", 0);
    if (sCfg.racers != 0 && sCfg.racers != 4 && sCfg.racers != 8 && sCfg.racers != 16 && sCfg.racers != 30) {
        sCfg.racers = 0;
    }
    sCfg.racersO3ds30 = gdx3ds_config_get_bool("net", "mp_racers_o3ds_30", 0) ? 1 : 0;
    sCfg.uds = gdx3ds_config_get_bool("net", "mp_uds", 1);
    sCfg.costEvery = ClampInt(gdx3ds_config_get_int("net", "mp_cost_every", 600), 0, 100000);
    if (rawName[0] != '\0') {
        gdx_mpo_name_sanitise(rawName, sCfg.name);
    } /* else "3DS-<last octet>" once SOC is up (gethostid needs it) */
    sCfg.bindPort = ClampInt(gdx3ds_config_get_int("net", "mp_bind_port", MPO_DEFAULT_BIND_PORT), 1024, 65000);
    sCfg.forceRelay = strcmp(relay, "force") == 0;
    sCfg.rvCookie = gdx_rv_cookie_mode_parse(rvCookie);
    sCfg.rvProto = ClampInt(gdx3ds_config_get_int("net", "mp_rv_proto", GDX_MPO_PROTO), 1, 65535);
    sCfg.simSymmetric = strcmp(nat, "symmetric") == 0;
    sCfg.maxPlayers = ClampInt(gdx3ds_config_get_int("net", "mp_players", 4), 2, GDX_MPO_MAX_SLOTS);
    sCfg.delay = ClampInt(gdx3ds_config_get_int("net", "mp_delay", 0), 0, GDX_MPO_MAX_DELAY);
    sCfg.waitMs = ClampInt(gdx3ds_config_get_int("net", "mp_wait_ms", 20000), 1000, 3600000);
    sCfg.launchMs = ClampInt(gdx3ds_config_get_int("net", "mp_launch_ms", MPO_LAUNCH_GIVE_UP_MS), 5000, 3600000);
    sCfg.loadSilenceMs = ClampInt(gdx3ds_config_get_int("net", "mp_load_silence_ms", MPO_LOAD_SILENCE_MS), 500,
                                  3600000);
    sCfg.wifiCheck = gdx3ds_config_get_bool("net", "mp_wifi_check", 1);
    sCfg.hashEvery = ClampInt(gdx3ds_config_get_int("net", "mp_hash_every", 1), 1, 60);
    sCfg.autostart = ClampInt(gdx3ds_config_get_int("net", "mp_autostart", 0), 0, GDX_MPO_MAX_SLOTS);
    sCfg.races = ClampInt(gdx3ds_config_get_int("net", "mp_races", 1), 1, 1000);
    sCfg.course = gdx3ds_config_get_int("net", "mp_course", -1);
    sCfg.vsCom = gdx3ds_config_get_int("net", "mp_vscom", -1);
    sCfg.bot = ClampInt(gdx3ds_config_get_int("net", "mp_bot", 0), 0, 2); /* 2 = ram mode (MP30) */
    sCfg.endAfter = ClampInt(gdx3ds_config_get_int("net", "mp_end_after", 0), 0, 1000000);
    sCfg.desyncAt = gdx3ds_config_get_int("net", "mp_desync_at", -1);
    sCfg.leaveAt = gdx3ds_config_get_int("net", "mp_leave_at", -1);
    sCfg.vanishAt = gdx3ds_config_get_int("net", "mp_vanish_at", -1);
    sCfg.suspendAt = gdx3ds_config_get_int("net", "mp_suspend_at", -1);
    sCfg.statsEvery = ClampInt(gdx3ds_config_get_int("net", "mp_stats_every", 600), 60, 100000);
    sCfg.rateMs = ClampInt(gdx3ds_config_get_int("net", "mp_rate_ms", 1000), 0, 600000);
    sCfg.dropMs = ClampInt(gdx3ds_config_get_int("net", "mp_drop_ms", 3000), 500, 3600000);
    sCfg.waitQuietMs = ClampInt(gdx3ds_config_get_int("net", "mp_wait_quiet_ms", 500), 0, 3600000);
    sCfg.hashLog = ClampInt(gdx3ds_config_get_int("net", "mp_hash_log", 0), 0, 100000);
    sCfg.hashDeep = gdx3ds_config_get_bool("net", "mp_hash_deep", 0) ? 1 : 0;
    {
        const char* dm = gdx3ds_mp_drop_mode_name();
        sCfg.dropMode = strcmp(dm, "retire") == 0 ? MPO_DROP_RETIRE
                                                  : (strcmp(dm, "coast") == 0 ? MPO_DROP_COAST : MPO_DROP_CPU);
    }
    sCfg.bgmHoldUntil = ClampInt(gdx3ds_config_get_int("net", "mp_bgm_hold_until", 0), 0, 1000000);
    sCfg.stockPick = gdx3ds_config_get_bool("net", "mp_stock_pick", 1);
    sCfg.pickMs = ClampInt(gdx3ds_config_get_int("net", "mp_pick_ms", GDX_MPO_PICK_MS), 5000, GDX_MPO_PICK_MS);
    sCfg.holdMs = ClampInt(gdx3ds_config_get_int("ui", "hold_confirm_ms", 1500), 0, 10000);
    sCfg.courseMs = gdx3ds_config_get_int("net", "mp_course_ms", 30000);
    sCfg.courseMs = sCfg.courseMs <= 0 ? 0 : ClampInt(sCfg.courseMs, 5000, 120000);
    sCfg.udsDiscPollMs = ClampInt(gdx3ds_config_get_int("net", "mp_uds_disc_poll_ms", 250), 0, 60000);
    sCfg.raceDiscMs = ClampInt(gdx3ds_config_get_int("net", "mp_race_disc_ms", 1000), 0, 60000);
    sCfg.logRaceDefer = ClampInt(gdx3ds_config_get_int("debug", "filelog_race_defer", -1), -1, 1);
    sCfg.o3dsExtraDelay = ClampInt(gdx3ds_config_get_int("net", "mp_o3ds_extra_delay", 0), 0, 4);
    LoadDirectConfig();
    sCfg.relayHoldL = ClampInt(gdx3ds_config_get_int("net", "mp_relay_hold_l", 0), 0, 2); /* 2 = as if held (scripted) */
    sCfg.onlineDiscOff = gdx3ds_config_get_bool("net", "mp_online_disc_off", 1) ? 1 : 0;
    sCfg.readySkip = gdx3ds_config_get_bool("net", "mp_ready_skip", 1) ? 1 : 0;
    sCfg.launchSyncO3ds = gdx3ds_config_get_int("debug", "filelog_launch_sync_o3ds", 0) != 0;
    sCfg.launchSyncRaceTicks = ClampInt(gdx3ds_config_get_int("debug", "filelog_launch_sync_race_ticks", 2), -1, 1000000);
    if (sCfg.course > GDX_MPO_MAX_COURSE || !gdx_mpo_course_raceable(sCfg.course)) {
        sCfg.course = -1;
    }
    gdx_mpo_code_normalise(sCfg.room);
    gdx_mpo_code_normalise(sCfg.wantCode);
}

/* FNV over both archives' zip central directories: two consoles agree on this only if every
 * member of every archive has the same name, CRC-32 and uncompressed size, however the PC that
 * prebaked them compressed it (gdx_mpo_zip_cd_hash). */
static uint32_t ContentHash(void) {
    static const char* const kArchives[2] = { "fzerox.o2r", "gdiffuser.o2r" };
    uint32_t h = 2166136261u;
    int i;
    for (i = 0; i < 2; i++) {
        char path[256];
        int rc;
        snprintf(path, sizeof(path), "%s%s", gdx3ds_fs_base_path(), kArchives[i]);
        rc = gdx_mpo_zip_cd_hash(path, &h);
        MpoLog("[mpo] content %s rc=%d hash=%08lx", kArchives[i], rc, (unsigned long) h);
        if (rc == -1) {
            h ^= 0xA5A5A5A5u; /* absent: still a (different) value */
        }
    }
    return h;
}

/* MP30: the sim-ABI word. Two consoles agree on it only if they run the same GAME, not just the
 * same commit: the build flavour (the Expansion Kit build compiles a different game:
 * EXPANSION_KIT changes racer.c, the countdown, the courses, the audio), this protocol, and the
 * set of levers a session forces neutral (gdx3ds_autodrive.c gdx3ds_mp_sim_levers_hash). It is
 * folded into the content hash every compatibility check already carries (the rendezvous
 * server's CREATE/JOIN, the NEARBY beacon and UDS passphrase, HELLO, the session digest), and
 * sent on its own in HELLO and START so the lobby can say why. Deliberately NOT in it: the Old
 * vs New 3DS profile (codegen -Os/-O2, the RDRAM size, the presets): those must stay compatible,
 * and the cross-build gstate A/B and the mixed lockstep runs are what prove they are. */
extern unsigned int gdx3ds_mp_sim_levers_hash(void) __attribute__((weak));

/* MP30: [net] mp_drop, shared by both lanes: what a removed player's machine does from its
 * removal tick T. cpu (default) = the CPU driver takes it over (gdx3ds_mpo_drop_cpu_racer);
 * retire = it retires (Racer_RetireRacer); coast = it rolls on a neutral pad; off = (the spike
 * only) a lost peer ends the session on that console. It changes the race, so it is in the
 * sim-ABI word: consoles set differently refuse each other in the lobby. */
const char* gdx3ds_mp_drop_mode_name(void) {
    const char* dm = gdx3ds_config_get_string("net", "mp_drop", "cpu");
    if (dm != NULL && strcmp(dm, "retire") == 0) {
        return "retire";
    } else if (dm != NULL && strcmp(dm, "coast") == 0) {
        return "coast";
    } else if (dm != NULL && strcmp(dm, "off") == 0) {
        return "off";
    }
    return "cpu";
}

uint32_t gdx3ds_mp_sim_abi(void) {
    static const char kTag[] = "gdx-sim-abi-1";
#ifdef GDX3DS_NET_EK
    const uint32_t ek = 1u;
#else
    const uint32_t ek = 0u;
#endif
    const uint32_t proto = GDX_MPO_PROTO;
    const uint32_t levers = (&gdx3ds_mp_sim_levers_hash != 0) ? gdx3ds_mp_sim_levers_hash() : 0u;
    const char* dropMode;
    uint32_t h = 2166136261u;
    size_t i;
    for (i = 0; i < sizeof(kTag) - 1; i++) {
        h ^= (uint8_t) kTag[i];
        h *= 16777619u;
    }
    for (i = 0; i < 4; i++) {
        h ^= (uint8_t) (ek >> (8 * i));
        h *= 16777619u;
    }
    for (i = 0; i < 4; i++) {
        h ^= (uint8_t) (proto >> (8 * i));
        h *= 16777619u;
    }
    for (i = 0; i < 4; i++) {
        h ^= (uint8_t) (levers >> (8 * i));
        h *= 16777619u;
    }
    for (dropMode = gdx3ds_mp_drop_mode_name(); *dropMode != '\0'; dropMode++) {
        h ^= (uint8_t) *dropMode;
        h *= 16777619u;
    }
    return h;
}

/* The 3DS profile name (System Settings > Profile > User Name): UTF-16, up to 10 characters.
 * Folded to the name alphabet: ASCII letters/digits/space/_.- kept, Latin-1 accents lose their
 * accent, anything else is dropped. "" when CFGU cannot be read. */
static void ProfileName(char* out, int cap) {
    static const char kFold[65] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
    u16 raw[0x1C / 2];
    char ascii[16];
    int n = 0;
    int i;
    out[0] = '\0';
    if (R_FAILED(cfguInit())) {
        return;
    }
    memset(raw, 0, sizeof(raw));
    if (R_SUCCEEDED(CFGU_GetConfigInfoBlk2(0x1C, 0x000A0000, raw))) {
        for (i = 0; i < 10 && raw[i] != 0 && n < (int) sizeof(ascii) - 1; i++) {
            const u16 c = raw[i];
            char a = 0;
            if (c < 0x80) {
                a = (char) c;
            } else if (c >= 0xC0 && c <= 0xFF) {
                a = kFold[c - 0xC0];
            }
            if ((a >= 'A' && a <= 'Z') || (a >= 'a' && a <= 'z') || (a >= '0' && a <= '9') || a == ' ' || a == '_' ||
                a == '.' || a == '-') {
                ascii[n++] = a;
            }
        }
    }
    cfguExit();
    ascii[n] = '\0';
    while (n > 0 && ascii[n - 1] == ' ') {
        ascii[--n] = '\0';
    }
    if (n > 0) {
        snprintf(out, (size_t) cap, "%s", ascii);
    }
}

void gdx3ds_mpo_init(void) {
    const char* rv = RendezvousSetting();
    const int haveRv = rv[0] != '\0';
    const char* direct = gdx3ds_config_get_string("net", "mp_direct", "");
    if (!haveRv && !LanSetting() && (direct == NULL || direct[0] == '\0')) {
        return; /* KILLSWITCH (rendezvous off, mp_lan 0, no mp_direct): not one receipt */
    }
    LoadConfig();
    if (haveRv && gdx3ds_config_get_string("net", "rendezvous", NULL) == NULL) {
        MpoLog("[mpo] init: no [net] rendezvous in the ini: the hosted server %s (temporary default)",
               GDX_MPO_DEFAULT_RENDEZVOUS);
    }
    if (haveRv) {
        sCfg.haveServer = gdx_mpo_parse_hostport(rv, &sCfg.server, MPO_DEFAULT_SERVER_PORT);
        if (!sCfg.haveServer) {
            MpoLog("[mpo] init: [net] rendezvous=\"%s\" is not a dotted IPv4 host[:port]: ONLINE off", rv);
            if (!sCfg.lan) {
                return;
            }
        }
    }
    {
        bool isNew = false;
        APT_CheckNew3DS(&isNew);
        sModel = isNew ? GDX_MPO_MODEL_N3DS : GDX_MPO_MODEL_O3DS;
    }
    if (sCfg.name[0] == '\0') {
        char prof[16];
        ProfileName(prof, sizeof(prof));
        if (prof[0] != '\0') {
            gdx_mpo_name_sanitise(prof, sCfg.name);
        }
    }
    snprintf(sBuild, sizeof(sBuild), "%s", GDX3DS_BUILD_ID);
    sSimAbi = gdx3ds_mp_sim_abi();
    sContentHash = ContentHash() ^ sSimAbi; /* MP30: every compatibility check carries the flavour */
    sBotLcg = (uint32_t) svcGetSystemTick() | 1u;
    sMessage[0] = '\0';
    SetPhase(GDX_MPO_IDLE);
    MpoLog("[mpo] init server=%u.%u.%u.%u:%u name=%s build=%s content=%08lx sim_abi=%08lx relay=%s rv_cookie=%s proto=%d "
           "sim_nat=%s auto=%s room=%s bot=%d delay=%d lan=%d uds=%d model=%s",
           sCfg.server.ip[0], sCfg.server.ip[1], sCfg.server.ip[2], sCfg.server.ip[3], sCfg.server.port, sCfg.name,
           sBuild, (unsigned long) sContentHash, (unsigned long) sSimAbi, sCfg.forceRelay ? "force" : "auto",
           gdx_rv_cookie_mode_name(sCfg.rvCookie), sCfg.rvProto,
           sCfg.simSymmetric ? "symmetric" : "-", sCfg.autoMode[0] ? sCfg.autoMode : "-",
           sCfg.room[0] ? sCfg.room : "-", sCfg.bot, sCfg.delay, sCfg.lan, sCfg.lan && sCfg.uds,
           sModel == GDX_MPO_MODEL_N3DS ? "N3DS" : "O3DS");
    /* MPFIX: no socket at boot. NEARBY opens its sockets when its list is first painted
     * (ServiceSockets), the room actions open them on demand (CanEnterRoomVia). */
    if (strcmp(sCfg.autoMode, "host") == 0) {
        gdx3ds_mpo_ui_host();
    } else if (strcmp(sCfg.autoMode, "lanhost") == 0) {
        gdx3ds_mpo_ui_lan_host();
    } else if (strcmp(sCfg.autoMode, "udshost") == 0) {
        gdx3ds_mpo_ui_uds_host();
    } else if (strcmp(sCfg.autoMode, "lanjoin") == 0 || strcmp(sCfg.autoMode, "udsjoin") == 0) {
        sLanAutoJoinMs = NowMs() + 3000; /* join the first joinable NEARBY room once one is heard */
        sLanAutoJoinUdsOnly = strcmp(sCfg.autoMode, "udsjoin") == 0;
    } else if (strcmp(sCfg.autoMode, "djoin") == 0) {
        gdx3ds_mpo_ui_direct_join(sCfg.room); /* test lever: serverless join of [net] mp_direct */
    } else if (strcmp(sCfg.autoMode, "join") == 0 || sCfg.room[0] != '\0') {
        gdx3ds_mpo_ui_join(sCfg.room);
    }
    gdx3ds_filelog_flush();
}

int gdx3ds_mpo_enabled(void) {
    return sPhase != GDX_MPO_OFF;
}

int gdx3ds_mpo_in_session(void) {
    return sPhase == GDX_MPO_LAUNCH || sPhase == GDX_MPO_RACE;
}

int gdx3ds_mpo_pads_bgm_ready(void) {
    if (sPhase != GDX_MPO_RACE || !sHaveTick) {
        return 1;
    }
    return sBgmReadyAll;
}

/* MP30: a removed human's machine is driven by the CPU from its removal tick T ([net] mp_drop =
 * cpu): gdx3ds_autodrive.c gdx_autodrive_racer asks this from inside the game update of race tick
 * sSimLt, and every console answers from the agreed removal tick, so the takeover starts on the
 * same tick everywhere. */
int gdx3ds_mpo_drop_cpu_racer(int racerId) {
    if (sPhase != GDX_MPO_RACE || !sHaveTick || sLs == NULL || sCfg.dropMode != MPO_DROP_CPU) {
        return 0;
    }
    if (racerId < 0 || racerId >= sLs->numPlayers || !gdx_mp_ls_is_removed(sLs, racerId, sSimLt)) {
        return 0;
    }
    if (!((sCpuNotedMask >> racerId) & 1u)) {
        sCpuNotedMask |= (uint8_t) (1u << racerId);
        MpoLog("[mpo] p%d: CPU driver takes over at tick %lu", racerId, (unsigned long) sSimLt);
    }
    return 1;
}

int gdx3ds_mpo_local_player(void) {
    if (!gdx3ds_mpo_in_session() || sLs == NULL) {
        return -1;
    }
    return sLs->localPlayer;
}

/* ---- room lifecycle ------------------------------------------------------------------------- */
static int EnsureSocket(void);

static void OpenDiscovery(void) {
    GdxMpDiscoveryConfig dc;
    int rc;
    if (!sCfg.lan || sDiscOpen) {
        return;
    }
    sDisc = gdx3ds_mp_discovery_udp_ops();
    memset(&dc, 0, sizeof(dc));
    dc.build = sBuild;
    dc.contentHash = sContentHash;
    dc.unicastPeers = sCfg.lanPeers;
    dc.broadcast = sCfg.lanBroadcast;
    rc = sDisc->open(&dc);
    sDiscOpen = (rc == 0);
    sDiscError = rc;
    {
        char d[160];
        sDisc->describe(d, sizeof(d));
        MpoLog("[mpo] nearby: %s rc=%d %s", sDisc->name, rc, d);
    }
}
/* ---- NEARBY over local wireless ------------------------------------------------------------- */
static int UdsEnabled(void) {
    return sCfg.lan && sCfg.uds;
}

/* The NEARBY list both media share (joinable first, then by name). */
static int NearbyRows(GdxMpNearbyRoom* out, int cap) {
    GdxMpUdsSeen seen[GDX_MP_UDS_NEARBY_MAX];
    const GdxMpNearby* lan = (sDiscOpen && sDisc != NULL) ? sDisc->table() : NULL;
    int i;
    for (i = 0; i < sUdsRoomCount; i++) {
        seen[i].beacon = sUdsRooms[i].beacon;
        seen[i].signalBars = sUdsRooms[i].signalBars;
    }
    return gdx_nearby_rows(lan, seen, sUdsRoomCount, sBuild, sContentHash, out, cap);
}

/* Tear down the local-wireless side of a room (after the room's LEAVE went out over it). */
static void EndUds(void) {
    if (!sUdsInRoom && !sUdsJoinPending) {
        return;
    }
    gdx3ds_mp_uds_leave();
    gdx3ds_p2p_set_uds(0);
    sUdsInRoom = 0;
    sUdsHosting = 0;
    sUdsJoinPending = 0;
    sUdsScanOn = 0; /* the scan restarts when the NEARBY tab is up again */
    MpoLog("[mpo] uds: left the local wireless network");
}

/* The scan runs only while the NEARBY list can be seen (or a test lever waits for a room) and we
 * are in no room: a scan may suspend the WiFi link on some consoles, and it costs radio time. */
static void ServiceUdsScan(uint32_t now) {
    const int want = UdsEnabled() && sPhase == GDX_MPO_IDLE && !sUdsJoinPending &&
                     ((sUiSeenMs != 0 && (int32_t) (now - sUiSeenMs) < MPO_UDS_UI_SEEN_MS) || sLanAutoJoinMs != 0 ||
                      sNearbyWait.active);
    if (want && !sUdsScanOn) {
        char why[128];
        int rc;
        why[0] = '\0';
        gdx3ds_mp_uds_set_proto(GDX_UDS_MPO_PROTO);
        rc = gdx3ds_mp_uds_scan(1, sBuild, sContentHash, why, sizeof(why));
        sUdsScanOn = 1;
        sUdsHeld = 1;
        if (rc != sUdsError) {
            MpoLog("[mpo] uds: scan rc=%d %s", rc, why);
        }
        sUdsError = rc;
        snprintf(sUdsWhy, sizeof(sUdsWhy), "%s", why);
    } else if (!want && sUdsScanOn) {
        gdx3ds_mp_uds_scan(0, sBuild, sContentHash, NULL, 0);
        sUdsScanOn = 0;
    }
    if (!want && sUdsHeld && sPhase == GDX_MPO_IDLE && !sUdsJoinPending && !sUdsInRoom) {
        /* MPFIX: NEARBY closed with no local room: leave UDS, or the WiFi link stays down. */
        if (gdx3ds_mp_uds_release()) {
            sUdsHeld = 0;
        }
    }
    if (UdsEnabled() && sUdsError == 0 && (sUdsScanOn || sPhase == GDX_MPO_IDLE)) {
        const int n = gdx3ds_mp_uds_discovery()->list(sUdsRooms, GDX_MP_UDS_NEARBY_MAX);
        sUdsRoomCount = n > 0 ? n : 0;
    } else {
        sUdsRoomCount = 0;
    }
}

/* A local-wireless join: the radio associates on the worker, then the room's JOIN goes to node 1. */
static void ServiceUdsJoin(uint32_t now) {
    char why[128];
    int st;
    if (!sUdsJoinPending) {
        return;
    }
    why[0] = '\0';
    st = gdx3ds_mp_uds_join_state(why, sizeof(why));
    if (st == GDX_UDS_JOIN_CONNECTED) {
        GdxMpEndpoint host;
        gdx_uds_node_endpoint(1, &host);
        sUdsJoinPending = 0;
        sUdsInRoom = 1;
        gdx3ds_p2p_set_uds(1);
        MpoLog("[mpo] uds: associated with %s's room as node %d after %lu ms; joining %s", sUdsJoinName,
               gdx3ds_mp_uds_local_node(), (unsigned long) (now - sUdsJoinStartMs), sUdsJoinCode);
        gdx3ds_p2p_lan_join(&host, sUdsJoinCode);
        return;
    }
    if (st == GDX_UDS_JOIN_FAILED || (int32_t) (now - sUdsJoinStartMs) > MPO_UDS_JOIN_GIVE_UP_MS) {
        MpoLog("[mpo] uds: join failed (state %d): %s", st, why);
        sUdsJoinPending = 0;
        sUdsInRoom = 1; /* so EndUds tears the half-made link down */
        EndUds();
        SetPhase(GDX_MPO_IDLE);
        SetMessage("%s", why[0] != '\0' ? why : "could not join over local wireless");
    }
}

/* While we host a local-wireless room: keep its beacon current (players, racing). */
static void ServiceUdsHost(void) {
    GdxMpBeacon b;
    if (!sUdsHosting || !sLbInit || sPhase < GDX_MPO_ROOM) {
        return;
    }
    gdx_uds_make_beacon(&b, sCfg.name, sModel, gdx3ds_p2p_lan_room_count(), sCfg.maxPlayers, sPhase != GDX_MPO_ROOM,
                        sBuild, sContentHash, sUdsBeacon.roomId);
    if (b.flags != sUdsBeacon.flags) {
        gdx3ds_mp_uds_set_racing(sPhase != GDX_MPO_ROOM); /* also blocks / opens new connections */
    }
    if (memcmp(&b, &sUdsBeacon, sizeof(b)) != 0) {
        sUdsBeacon = b;
        gdx3ds_mp_uds_host_beacon(&sUdsBeacon);
    }
}

static int WifiOk(void) {
    u32 status = 0;
    Result rc;
    if (!sCfg.wifiCheck) {
        return 1;
    }
    rc = acInit();
    if (R_FAILED(rc)) {
        return 1; /* cannot tell: let the network say so */
    }
    rc = ACU_GetWifiStatus(&status);
    acExit();
    return R_FAILED(rc) || status != 0;
}

static int EnsureSocket(void) {
    GdxP2pConfig c;
    int rc;
    if (sSocketOpen) {
        return 0;
    }
    rc = gdx3ds_mp_ensure_soc();
    if (rc != 0) {
        SetMessage("network unavailable (SOC %d)", rc);
        return -1;
    }
    if (sCfg.name[0] == '\0') {
        const uint32_t ip = (uint32_t) gethostid(); /* network order in memory: last octet on top */
        char raw[16];
        snprintf(raw, sizeof(raw), "3DS-%u", (unsigned) ((ip >> 24) & 0xFF));
        gdx_mpo_name_sanitise(raw, sCfg.name);
    }
    memset(&c, 0, sizeof(c));
    c.server = sCfg.server;
    c.bindPort = sCfg.bindPort;
    c.forceRelay = sCfg.forceRelay;
    c.simSymmetric = sCfg.simSymmetric;
    c.build = sBuild;
    c.contentHash = sContentHash;
    c.name = sCfg.name;
    c.lanTimeoutMs = (uint32_t) sCfg.lanTimeoutMs;
    c.rvCookie = sCfg.rvCookie;
    c.clientProto = sCfg.rvProto;
    c.log = MpoLog;
    rc = gdx3ds_p2p_open(&c);
    if (rc != 0) {
        SetMessage("cannot open a UDP socket (%d)", rc);
        return -1;
    }
    sSocketOpen = 1;
    {
        const GdxMpEndpoint me = gdx3ds_p2p_local_endpoint();
        MpoLog("[mpo] socket up local=%u.%u.%u.%u:%u", me.ip[0], me.ip[1], me.ip[2], me.ip[3], me.port);
    }
    OpenDiscovery();
    return 0;
}

/* MPFIX: NEARBY's sockets live only while someone can use them. */
static int NearbyWanted(uint32_t now) {
    if (!sCfg.lan) {
        return 0;
    }
    if (sLanAutoJoinMs != 0) {
        return 1; /* test lever mp_auto=lanjoin / udsjoin: listening for a room to join */
    }
    return sUiSeenMs != 0 && (int32_t) (now - sUiSeenMs) < MPO_NEARBY_LINGER_MS;
}

static void CloseSockets(const char* why) {
    if (sDiscOpen && sDisc != NULL) {
        sDisc->close();
    }
    sDiscOpen = 0;
    sDiscError = 0;
    if (sSocketOpen) {
        gdx3ds_p2p_close();
        sSocketOpen = 0;
    }
    sCost.closes++;
    MpoLog("[mpo] nearby: sockets closed (%s)", why);
}

/* Every frame while multiplayer is enabled, before the network service. Opens and closes only in
 * IDLE: a room, a join on its way or an ABORT still being repeated keeps whatever is open. */
static void ServiceSockets(uint32_t now) {
    const int quiet = sPhase == GDX_MPO_IDLE && !sUdsJoinPending && !sUdsInRoom && sAutoJoinAtMs == 0 &&
                      (sAbortSlots == 0 || (int32_t) (sAbortUntilMs - now) <= 0);
    if (!quiet) {
        return;
    }
    if (NearbyWanted(now)) {
        if (!sSocketOpen && (sSocketRetryAtMs == 0 || (int32_t) (now - sSocketRetryAtMs) >= 0)) {
            sCost.opens++;
            if (EnsureSocket() != 0) {
                sSocketRetryAtMs = now + MPO_SOCKET_RETRY_MS;
                if (sSocketRetryAtMs == 0) {
                    sSocketRetryAtMs = 1;
                }
            } else {
                sSocketRetryAtMs = 0;
            }
        }
    } else if (sSocketOpen || sDiscOpen) {
        CloseSockets("NEARBY off screen");
    }
}

static int CostLive(void) {
    return sSocketOpen || sDiscOpen || sUdsScanOn || sUdsInRoom || sUdsJoinPending || sPhase > GDX_MPO_IDLE;
}

/* Closes one frame's account (from after_tick, the frame's last multiplayer call) and emits the
 * window. A window with no live frame (nothing open, no room) says nothing: MP idle costs one
 * branch a frame. */
static void CostFrameEnd(uint32_t tailTicks) {
    const uint32_t t = sCost.frameTicks + tailTicks;
    const int live = CostLive();
    sCost.tailTicks += tailTicks;
    sCost.frames++;
    if (live) {
        sCost.live++;
    }
    if (t > sCost.maxTicks) {
        sCost.maxTicks = t;
    }
    if (t > (uint32_t) (2 * CPU_TICKS_PER_MSEC)) {
        sCost.over2ms++;
    }
    sCost.frameTicks = 0;
    if (sCfg.costEvery <= 0 || sCost.frames < (uint32_t) sCfg.costEvery) {
        return;
    }
    if (sCost.live > 0 || sCost.opens > 0 || sCost.closes > 0) {
        static const char* const kName[] = { "off", "idle", "connecting", "room", "launch", "race", "linger" };
        const double usPerTick = 1000.0 / CPU_TICKS_PER_MSEC;
        char ipc[640];
        MpoLog("[mpcost] frames=%lu live=%lu avg_us=%lu tail_us=%lu exch_us=%lu wait_us=%lu max_us=%lu over2ms=%lu "
               "sock=%d disc=%d uds_scan=%d uds_room=%d opens=%lu closes=%lu phase=%s",
               (unsigned long) sCost.frames, (unsigned long) sCost.live,
               (unsigned long) ((double) (sCost.tailTicks + sCost.exchTicks) * usPerTick / sCost.frames),
               (unsigned long) ((double) sCost.tailTicks * usPerTick / sCost.frames),
               (unsigned long) ((double) sCost.exchTicks * usPerTick / sCost.frames),
               (unsigned long) ((double) sCost.waitTicks * usPerTick / sCost.frames),
               (unsigned long) ((double) sCost.maxTicks * usPerTick), (unsigned long) sCost.over2ms, sSocketOpen,
               sDiscOpen, sUdsScanOn, sUdsInRoom, (unsigned long) sCost.opens, (unsigned long) sCost.closes,
               kName[sPhase]);
        gdx3ds_ipcstat_line(ipc, (int) sizeof(ipc), (unsigned long) sCost.frames);
        if (ipc[0] != '\0') {
            MpoLog("%s", ipc);
        }
        gdx3ds_mp_uds_svc_line(ipc, (int) sizeof(ipc));
        if (ipc[0] != '\0') {
            MpoLog("%s", ipc);
        }
        gdx3ds_socthr_line(ipc, (int) sizeof(ipc));
        if (ipc[0] != '\0') {
            MpoLog("%s", ipc);
        }
    }
    memset(&sCost, 0, sizeof(sCost));
}

static void HoldSleep(int hold) {
    if (hold && !sSleepHeld) {
        sSleepWasAllowed = aptIsSleepAllowed();
        aptSetSleepAllowed(false);
        sSleepHeld = 1;
    } else if (!hold && sSleepHeld) {
        aptSetSleepAllowed(sSleepWasAllowed);
        sSleepHeld = 0;
    }
}

/* mp-menus: how to get back into the room being joined (MPO_REJOIN_*). Hosting, local wireless
 * and the test lanes forget it: those rooms are not rejoined automatically. */
static void RememberRejoin(int kind, const char* code, const GdxMpEndpoint* host) {
    sRejoinKind = kind;
    snprintf(sRejoinCode, sizeof(sRejoinCode), "%s", code != NULL ? code : "");
    if (host != NULL) {
        sRejoinHost = *host;
    }
}

static int CanEnterRoomVia(int needWifi) {
    if (sPhase == GDX_MPO_OFF) {
        return 0;
    }
    if (sPhase != GDX_MPO_IDLE || sUdsJoinPending) {
        SetMessage("leave the current room first");
        return 0;
    }
    if (needWifi && !WifiOk()) {
        SetMessage("WiFi is off: turn it on to play online");
        return 0;
    }
    return EnsureSocket() == 0;
}

static int CanEnterRoom(void) {
    return CanEnterRoomVia(1);
}

void gdx3ds_mpo_ui_host(void) {
    if (!sCfg.haveServer) {
        SetMessage("no server set: [net] rendezvous in gdiffuser.ini");
        return;
    }
    if (!CanEnterRoom()) {
        return;
    }
    sLbInit = 0;
    sRacesStarted = 0;
    sRacesDone = 0;
    RememberRejoin(MPO_REJOIN_NONE, NULL, NULL);
    {
        /* Test lever mp_relay_hold_l: L held = a relay-only room (every link through the server). */
        const int relayOnly = sCfg.relayHoldL == 2 || (sCfg.relayHoldL == 1 && (hidKeysHeld() & KEY_L) != 0);
        gdx3ds_p2p_set_next_relay_only(relayOnly);
        if (relayOnly) {
            MpoLog("[mpo] host: L held, asking for a relay-only room (mp_relay_hold_l)");
        }
        gdx3ds_p2p_host(sCfg.maxPlayers, sCfg.wantCode);
        SetPhase(GDX_MPO_CONNECTING);
        SetMessage(relayOnly ? "creating a relay-only room..." : "creating a room...");
    }
}

/* Serverless online ([net] mp_direct = a.b.c.d[:port]): join the room the console at that address
 * hosts on its WiFi socket, by the code its lobby shows. Everything after the JOIN is a NEARBY
 * WiFi room's code path: the host's room checks the code, build and content, hands out the
 * secret and the roster, and relays for joiner pairs that cannot reach each other. */
void gdx3ds_mpo_ui_direct_join(const char* code) {
    char c[GDX_MPO_CODE_LEN + 1];
    const GdxMpEndpoint* e = &sCfg.direct;
    snprintf(c, sizeof(c), "%s", code != NULL ? code : "");
    if (!gdx_mpo_code_normalise(c)) {
        SetMessage("room codes are 5 letters/digits");
        return;
    }
    if (!sCfg.haveDirect) {
        SetMessage("no host address: [net] mp_direct in gdiffuser.ini");
        return;
    }
    if (!CanEnterRoom()) {
        return;
    }
    sLbInit = 0;
    sRacesStarted = 0;
    sRacesDone = 0;
    RememberRejoin(MPO_REJOIN_DIRECT, c, NULL);
    gdx3ds_p2p_lan_join(e, c);
    SetPhase(GDX_MPO_CONNECTING);
    SetMessage("joining %u.%u.%u.%u:%u...", e->ip[0], e->ip[1], e->ip[2], e->ip[3], e->port);
    MpoLog("[mpo] direct: joining room %s at %u.%u.%u.%u:%u (no server)", c, e->ip[0], e->ip[1], e->ip[2], e->ip[3],
           e->port);
}

int gdx3ds_mpo_direct_addr(char* out, int cap) {
    const GdxMpEndpoint* e = &sCfg.direct;
    if (out == NULL || cap <= 0) {
        return 0;
    }
    out[0] = '\0';
    if (!sCfg.haveDirect) {
        return 0;
    }
    snprintf(out, (size_t) cap, "%u.%u.%u.%u:%u", e->ip[0], e->ip[1], e->ip[2], e->ip[3], e->port);
    return 1;
}

void gdx3ds_mpo_ui_join(const char* code) {
    char c[GDX_MPO_CODE_LEN + 1];
    snprintf(c, sizeof(c), "%s", code != NULL ? code : "");
    if (!gdx_mpo_code_normalise(c)) {
        SetMessage("room codes are 5 letters/digits");
        return;
    }
    if (!sCfg.haveServer) {
        SetMessage("no server set: [net] rendezvous in gdiffuser.ini");
        return;
    }
    if (!CanEnterRoom()) {
        return;
    }
    sLbInit = 0;
    RememberRejoin(MPO_REJOIN_ONLINE, c, NULL);
    gdx3ds_p2p_join(c);
    SetPhase(GDX_MPO_CONNECTING);
    SetMessage("joining %s...", c);
}

/* NEARBY: host a room on this console. The code is random (the beacon carries it; nobody types
 * it), the host id tells our own beacon apart. */
void gdx3ds_mpo_ui_lan_host(void) {
    uint32_t r = (uint32_t) svcGetSystemTick() ^ (uint32_t) osGetTime() ^ sBotLcg;
    uint64_t entropy;
    int i;
    if (!CanEnterRoom()) {
        return;
    }
    for (i = 0; i < GDX_MPO_CODE_LEN; i++) {
        r = Mix32(r + 0x9E3779B9u);
        sLanCode[i] = gdx_mpo_code_alphabet[r % 32u];
    }
    sLanCode[GDX_MPO_CODE_LEN] = '\0';
    if (sCfg.directHost && sCfg.wantCode[0] != '\0') {
        snprintf(sLanCode, sizeof(sLanCode), "%s", sCfg.wantCode); /* test lever mp_code: a scripted djoin */
    }
    r = Mix32(r ^ 0xA511E9B3u);
    sLanHostId = r != 0 ? r : 1;
    entropy = ((uint64_t) Mix32(r ^ (uint32_t) svcGetSystemTick()) << 32) | Mix32(r + 0x632BE5ABu);
    sLbInit = 0;
    sRacesStarted = 0;
    sRacesDone = 0;
    RememberRejoin(MPO_REJOIN_NONE, NULL, NULL);
    gdx3ds_p2p_lan_host(sCfg.maxPlayers, sLanCode, entropy);
    SetPhase(GDX_MPO_CONNECTING);
    SetMessage("opening a room on this WiFi...");
}

/* NEARBY over local wireless (no router): the same console-hosted room, its datagrams carried as
 * UDS frames; the room code comes from the beacon's room id, so nobody types it. */
void gdx3ds_mpo_ui_uds_host(void) {
    uint32_t r = (uint32_t) svcGetSystemTick() ^ (uint32_t) osGetTime() ^ sBotLcg ^ 0x5D5Du;
    uint64_t entropy;
    char why[128];
    char code[GDX_MPO_CODE_LEN + 1];
    int rc;
    if (!UdsEnabled()) {
        SetMessage("local wireless is off ([net] mp_uds)");
        return;
    }
    if (!CanEnterRoomVia(0)) {
        return;
    }
    r = Mix32(r + 0x9E3779B9u);
    if (r == 0) {
        r = 1;
    }
    gdx3ds_mp_uds_scan(0, sBuild, sContentHash, NULL, 0);
    sUdsScanOn = 0;
    gdx3ds_mp_uds_set_proto(GDX_UDS_MPO_PROTO);
    gdx_uds_make_beacon(&sUdsBeacon, sCfg.name, sModel, 1, sCfg.maxPlayers, 0, sBuild, sContentHash, r);
    why[0] = '\0';
    rc = gdx3ds_mp_uds_host_room(&sUdsBeacon, why, sizeof(why));
    sUdsHeld = 1;
    if (rc != GDX_UDS_OK) {
        MpoLog("[mpo] uds: host failed rc=%d %s", rc, why);
        gdx3ds_mp_uds_leave();
        SetMessage("%s", why[0] != '\0' ? why : "could not open a local wireless room");
        return;
    }
    gdx_uds_room_code(r, code);
    entropy = ((uint64_t) Mix32(r ^ (uint32_t) svcGetSystemTick()) << 32) | Mix32(r + 0x632BE5ABu);
    sUdsInRoom = 1;
    sUdsHosting = 1;
    sLbInit = 0;
    RememberRejoin(MPO_REJOIN_NONE, NULL, NULL);
    sRacesStarted = 0;
    sRacesDone = 0;
    gdx3ds_p2p_set_uds(1);
    gdx3ds_p2p_lan_host(sCfg.maxPlayers, code, entropy);
    SetPhase(GDX_MPO_CONNECTING);
    SetMessage("opening a local wireless room...");
    MpoLog("[mpo] uds: hosting room %08lx code %s max=%d", (unsigned long) r, code, sCfg.maxPlayers);
}

/* NEARBY: join row `row` of the sorted list (gdx3ds_mpo_ui's nearby[]). */
static void BeginNearbyWait(const GdxMpNearbyRoom* r) {
    if (!CanEnterRoom()) {
        return;
    }
    sNearbyWait.active = 1;
    sNearbyWait.hostId = r->b.hostId;
    sNearbyWait.medium = r->medium;
    sNearbyWait.goneSinceMs = 0;
    snprintf(sNearbyWait.name, sizeof(sNearbyWait.name), "%s", r->b.name);
    SetMessage("race in progress: you join when it ends");
    MpoLog("[mpo] %s's room is racing: this console waits to join it (no mid-race join)", r->b.name);
}

static void EndNearbyWait(const char* why) {
    if (!sNearbyWait.active) {
        return;
    }
    sNearbyWait.active = 0;
    MpoLog("[mpo] waiting for %s's race: %s", sNearbyWait.name, why);
}

/* The room waited for: its row in the NEARBY list, -1 = not heard now. */
static int NearbyWaitRow(const GdxMpNearbyRoom* rows, int n) {
    int i;
    for (i = 0; i < n; i++) {
        if (rows[i].b.hostId == sNearbyWait.hostId && rows[i].medium == sNearbyWait.medium) {
            return i;
        }
    }
    return -1;
}

void gdx3ds_mpo_ui_lan_join(int row);

static void ServiceNearbyWait(uint32_t now) {
    GdxMpNearbyRoom rows[GDX_LAN_MAX_NEARBY + GDX_MP_UDS_NEARBY_MAX];
    int n;
    int i;
    if (!sNearbyWait.active || sPhase != GDX_MPO_IDLE) {
        return;
    }
    n = NearbyRows(rows, (int) (sizeof(rows) / sizeof(rows[0])));
    i = NearbyWaitRow(rows, n);
    if (i < 0) {
        sNearbyWait.goneSinceMs = sNearbyWait.goneSinceMs != 0 ? sNearbyWait.goneSinceMs : (now != 0 ? now : 1);
        if ((int32_t) (now - sNearbyWait.goneSinceMs) >= MPO_NEARBY_WAIT_GONE_MS) {
            EndNearbyWait("the room is gone");
            SetMessage("room closed: %s's room is gone", sNearbyWait.name);
        }
        return;
    }
    sNearbyWait.goneSinceMs = 0;
    if (rows[i].joinable) {
        EndNearbyWait("the race is over, joining");
        gdx3ds_mpo_ui_lan_join(i);
    } else if (strcmp(rows[i].whyNot, "racing") != 0) {
        EndNearbyWait(rows[i].whyNot);
        SetMessage("cannot join: %s", rows[i].whyNot);
    }
}

void gdx3ds_mpo_ui_lan_join(int row) {
    GdxMpNearbyRoom rows[GDX_LAN_MAX_NEARBY + GDX_MP_UDS_NEARBY_MAX];
    const GdxMpNearbyRoom* r;
    const int n = NearbyRows(rows, (int) (sizeof(rows) / sizeof(rows[0])));
    if (row < 0 || row >= n) {
        return;
    }
    r = &rows[row];
    if (!r->joinable && r->compatible && strcmp(r->whyNot, "racing") == 0) {
        BeginNearbyWait(r);
        return;
    }
    if (!r->joinable) {
        SetMessage("cannot join: %s", r->whyNot);
        return;
    }
    if (r->medium == GDX_NEARBY_MEDIUM_UDS) {
        int rc;
        if (!CanEnterRoomVia(0) || r->srcIndex < 0 || r->srcIndex >= sUdsRoomCount) {
            return;
        }
        gdx3ds_mp_uds_scan(0, sBuild, sContentHash, NULL, 0);
        sUdsScanOn = 0;
        rc = gdx3ds_mp_uds_join_room(&sUdsRooms[r->srcIndex]);
        sUdsHeld = 1;
        if (rc != 0) {
            SetMessage("cannot join over local wireless (%d)", rc);
            return;
        }
        sLbInit = 0;
        RememberRejoin(MPO_REJOIN_NONE, NULL, NULL);
        sUdsJoinPending = 1;
        sUdsJoinStartMs = NowMs();
        snprintf(sUdsJoinCode, sizeof(sUdsJoinCode), "%s", r->b.code);
        snprintf(sUdsJoinName, sizeof(sUdsJoinName), "%s", r->b.name);
        SetPhase(GDX_MPO_CONNECTING);
        SetMessage("joining %s's room (local wireless)...", r->b.name);
        MpoLog("[mpo] uds: joining %s's room %08lx code %s", r->b.name, (unsigned long) r->b.hostId, r->b.code);
        return;
    }
    if (!CanEnterRoom()) {
        return;
    }
    sLbInit = 0;
    RememberRejoin(MPO_REJOIN_LAN, r->b.code, &r->room);
    gdx3ds_p2p_lan_join(&r->room, r->b.code);
    SetPhase(GDX_MPO_CONNECTING);
    SetMessage("joining %s's room...", r->b.name);
}

static void FreeLockstep(void) {
    free(sLs);
    sLs = NULL;
    sHaveTick = 0;
}

static void ResetPoints(void) {
    sPointsEpochKnown = 0;
    memset(sPoints, 0, sizeof(sPoints));
    memset(sRacePoints, 0, sizeof(sRacePoints));
    memset(sRacePlace, 0, sizeof(sRacePlace));
    sByCpuSlots = 0;
}

static void ClearPointsOfSlot(int s) {
    sPoints[s] = 0;
    sRacePoints[s] = 0;
    sRacePlace[s] = 0;
    sByCpuSlots &= (uint8_t) ~(1u << s);
}

/* mp-menus: the slot's console is not the one the points were scored by (it changed hands: a new
 * roster epoch since the points were last looked at). */
static int SlotChangedHands(int s) {
    const uint8_t bit = (uint8_t) (1u << s);
    const int changed = (sPointsEpochKnown & bit) && sPointsEpoch[s] != sLb.peer[s].epoch;
    sPointsEpoch[s] = sLb.peer[s].epoch;
    sPointsEpochKnown |= bit;
    return changed;
}

/* A slot that emptied, or changed hands, starts from zero: whoever takes it next is someone else. */
static void ClearPointsOfEmptySlots(void) {
    int s;
    for (s = 0; s < GDX_MPO_MAX_SLOTS; s++) {
        if (s == sLb.mySlot) {
            continue;
        }
        if (!sLb.peer[s].present) {
            sPointsEpochKnown &= (uint8_t) ~(1u << s);
            ClearPointsOfSlot(s);
        } else if (SlotChangedHands(s)) {
            MpoLog("[mpo] slot %d changed hands: its points start from zero", s);
            ClearPointsOfSlot(s);
        }
    }
}

static void LeaveRoom(const char* why) {
    if (gdx3ds_mpo_in_session()) {
        gdx3ds_mpg_leave();
    }
    sRoomLostInRace = 0;
    sRoomClosed = 0;
    sRoomClosedWhy[0] = '\0';
    sEndedByName[0] = '\0';
    sCourseDeadlineMs = 0;
    sRaceWait = 0;
    sNearbyWait.active = 0;
    gdx3ds_p2p_leave();
    EndUds();
    FreeLockstep();
    gdx3ds_mpp_close();
    sPickWanted = GDX_MPP_NONE;
    sPickDeadlineMs = 0;
    sLbInit = 0;
    ResetPoints();
    HoldSleep(0);
    SetPhase(GDX_MPO_IDLE);
    if (why != NULL) {
        SetMessage("%s", why);
    }
}

static void AbortSession(int reason, int culprit, const char* detail);
static void HashRingDump(void);
static uint32_t Mix32(uint32_t x);
static int SessionPlayerOfSlot(int slot);

void gdx3ds_mpo_ui_leave(void) {
    if (sNearbyWait.active) {
        EndNearbyWait("cancelled");
        SetMessage("left the room");
    }
    if (sPhase == GDX_MPO_OFF || sPhase == GDX_MPO_IDLE) {
        return;
    }
    if (gdx3ds_mpo_in_session()) {
        AbortSession(GDX_LB_ABORT_QUIT, sLs != NULL ? sLs->localPlayer : -1, "left the room mid-race");
    }
    LeaveRoom("left the room");
}

/* ---- sending ------------------------------------------------------------------------------- */
static int SendLobby(int slot, const uint8_t* pkt, int n, int probe) {
    if (n <= 0 || sVanished) {
        return 0;
    }
    return gdx3ds_p2p_send_ex(slot, pkt, n, probe);
}

static void SendHellos(uint32_t now) {
    int s;
    const int mask = gdx3ds_p2p_direct_mask(now);
    for (s = 0; s < GDX_MPO_MAX_SLOTS; s++) {
        uint8_t pkt[GDX_LB_HELLO_BYTES];
        const int n = gdx_lobby_build_hello(&sLb, s, now, mask, pkt, sizeof(pkt));
        SendLobby(s, pkt, n, 1);
    }
    sLastHelloMs = now;
}

static void SendStarts(uint32_t now) {
    int i;
    for (i = 0; i < sLb.start.numPlayers; i++) {
        uint8_t pkt[GDX_LB_START_BYTES];
        const int s = sLb.start.player[i].slot;
        const int n = gdx_lobby_build_start(&sLb, s, now, pkt, sizeof(pkt));
        SendLobby(s, pkt, n, 0);
    }
    sLastStartSendMs = now;
}

static void SendAborts(uint32_t now) {
    int s;
    for (s = 0; s < GDX_MPO_MAX_SLOTS; s++) {
        uint8_t pkt[GDX_LB_ABORT_BYTES];
        int n;
        if (!(sAbortSlots & (1u << s))) {
            continue;
        }
        n = gdx_lobby_build_abort(&sLb, s, now, &sAbortMsg, pkt, sizeof(pkt));
        SendLobby(s, pkt, n, 0);
    }
    sLastAbortSendMs = now;
}

static void SendOurPads(uint32_t now) {
    uint8_t pkt[GDX_MP_MAX_PACKET];
    int n;
    int p;
    if (sVanished) {
        return;
    }
    if (sLs == NULL) {
        return;
    }
    n = gdx_mp_ls_build_packet(sLs, now, pkt, sizeof(pkt));
    if (n <= 0) {
        return;
    }
    for (p = 0; p < sLs->numPlayers; p++) {
        const int slot = gdx_lobby_player_slot(&sLb, p);
        if (p != sLs->localPlayer && slot >= 0) {
            gdx3ds_p2p_send_ex(slot, pkt, n, 0);
        }
    }
}

/* ---- session start / end ------------------------------------------------------------------- */
static const char* AbortText(int reason) {
    switch (reason) {
        case GDX_LB_ABORT_QUIT: return "left";
        case GDX_LB_ABORT_DESYNC: return "desync";
        case GDX_LB_ABORT_PEER_LOST: return "connection lost";
        case GDX_LB_ABORT_SUSPEND: return "suspended";
        case GDX_LB_ABORT_LAUNCH_FAILED: return "could not load the race";
        default: return "aborted";
    }
}

static void BeginLaunch(void);

static void BackToLobby(void) {
    FreeLockstep();
    sLb.state = GDX_LB_STATE_LOBBY;
    if (sCfg.autoMode[0] == '\0' && !sCfg.bot) {
        sLb.ready = 0; /* a person readies up again for the next race */
    }
    SetPhase(GDX_MPO_ROOM);
    if (sLb.startIsNew) {
        BeginLaunch(); /* the host's next START arrived while we were still lingering */
    }
}

/* Ends the session at once (desync, a lost peer, an ABORT heard, suspend): tells every other
 * player (repeated for a while), leaves the race for the main menu, back to the lobby. */
static void AbortSession(int reason, int culprit, const char* detail) {
    const uint32_t now = NowMs();
    int i;
    if (!gdx3ds_mpo_in_session()) {
        return;
    }
    memset(&sAbortMsg, 0, sizeof(sAbortMsg));
    sAbortMsg.gen = sLb.start.gen;
    sAbortMsg.reason = (uint8_t) reason;
    sAbortMsg.culprit = (uint8_t) (culprit >= 0 ? culprit : 0xFF);
    sAbortMsg.tick = sLastLt;
    sAbortSlots = 0;
    for (i = 0; i < sLb.start.numPlayers; i++) {
        if (sLb.start.player[i].slot != sLb.mySlot) {
            sAbortSlots |= 1u << sLb.start.player[i].slot;
        }
    }
    if (detail != NULL) {
        SendAborts(now);
        SendAborts(now);
        sAbortUntilMs = now + MPO_ABORT_REPEAT_MS;
    } else {
        sAbortSlots = 0; /* we are answering an ABORT: no need to echo it */
    }
    if (sLs != NULL) {
        sLs->txFlags |= GDX_MP_FLAG_ABORT;
        SendOurPads(now);
    }
    if (reason == GDX_LB_ABORT_DESYNC) {
        HashRingDump();
    }
    MpoLog("[mpo] ABORT reason=%s culprit=%d tick=%lu gen=%lu (%s)", AbortText(reason), culprit,
           (unsigned long) sLastLt, (unsigned long) sLb.start.gen, detail != NULL ? detail : "heard from a peer");
    gdx3ds_mpg_leave();
    BackToLobby();
    gdx3ds_filelog_flush();
}

/* A racer is gone (it left the room, its slot holds a different console now, its ABORT says it
 * quit, it went silent): flag it. The coordinator removes it (gdx_mp_remove.h); everybody else
 * waits for the RM_SET. No mid-race rejoin: a console back in the room waits in the lobby. */
static void WantDrop(int player, int reason, const char* why) {
    if (sLs == NULL || player < 0 || player >= sLs->numPlayers || player == sLs->localPlayer ||
        ((sRm.removedMask >> player) & 1u) || ((sDropWanted >> player) & 1u)) {
        return;
    }
    sDropWanted |= (uint8_t) (1u << player);
    sDropReason[player] = (uint8_t) reason;
    MpoLog("[mpo] p%d is gone (%s): to be removed", player, why);
}

/* The roster changed mid-session: a racer that left the room, or whose slot now holds a different
 * console (the server's epoch moved: it restarted and joined again), will never send the pads we
 * wait for. */
static void DropRacersThatLeft(void) {
    int p;
    if (!gdx3ds_mpo_in_session() || sLs == NULL) {
        return;
    }
    for (p = 0; p < sLs->numPlayers; p++) {
        const int slot = gdx_lobby_player_slot(&sLb, p);
        if (p == sLs->localPlayer || slot < 0) {
            continue;
        }
        if (sLb.peer[slot].present && sLb.peer[slot].epoch == sSessionEpoch[slot]) {
            continue;
        }
        WantDrop(p, GDX_LB_ABORT_QUIT, sLb.peer[slot].present ? "rejoined the room" : "left the room");
    }
}

/* A removal took effect here (as coordinator or from RM_SET): the race goes on without p. */
static void NoteRemoved(int player, uint32_t tick) {
    const int slot = gdx_lobby_player_slot(&sLb, player);
    const char* name = (slot >= 0 && slot < GDX_MPO_MAX_SLOTS) ? sSessionName[slot] : "?";
    sLb.removedMask = sRm.removedMask;
    sDropWanted &= (uint8_t) ~(1u << player);
    gdx3ds_mpg_set_removed(player, tick);
    sDropSerial++;
    sDropPlayer = player;
    SetMessage("%s removed at tick %lu", name[0] ? name : "a player", (unsigned long) tick);
    MpoLog("[mpo] p%d (%s) removed at tick %lu: neutral from there on, the race goes on", player, name,
           (unsigned long) tick);
}

/* How long a session peer may be silent before it is dropped: mp_drop_ms once it has sent race
 * pads; before that it is still loading (an Old 3DS takes longer, and some load steps block its
 * main loop), so mp_load_silence_ms, never less than mp_drop_ms and never more than the launch. */
static int SilenceLimitMs(int slot) {
    int limit;
    if ((sHeardPads >> slot) & 1u) {
        return sCfg.dropMs;
    }
    limit = sCfg.loadSilenceMs;
    if (limit < sCfg.dropMs) {
        limit = sCfg.dropMs;
    }
    if (limit > sCfg.launchMs) {
        limit = sCfg.launchMs;
    }
    return limit;
}

/* Every datagram from a session peer: the longest gap it left while it still loaded (the data
 * behind MPO_LOAD_SILENCE_MS), then it is heard now. */
static void NoteHeard(int slot, uint32_t now) {
    if (slot < 0 || slot >= GDX_MPO_MAX_SLOTS) {
        return;
    }
    if (gdx3ds_mpo_in_session() && !((sHeardPads >> slot) & 1u)) {
        const uint32_t gap = now - sLastHeardMs[slot];
        if (gap > sLoadSilenceMaxMs[slot]) {
            sLoadSilenceMaxMs[slot] = gap;
        }
    }
    sLastHeardMs[slot] = now;
}

/* mp-menus (edge #6): the coordinator does not poll or wait on racers it has flagged too (two
 * consoles that vanished together are removed one after the other, not given up on). -1 = the
 * session ended here. */
static int ServiceGoneSet(uint32_t now) {
    const int player = sRm.player;
    const uint8_t before = sRm.removedMask;
    const int rc = gdx_rm_set_gone(&sRm, &sLb, sLs, sDropWanted, now);
    if (rc < 0) {
        AbortSession(GDX_LB_ABORT_PEER_LOST, player, "removal: the pads do not reach the agreed tick");
        return -1;
    }
    if (rc == 1 && sRm.removedMask != before && player >= 0) {
        NoteRemoved(player, sRm.removedTick[player]);
    }
    return 0;
}

/* The coordinator's side: start a flagged removal, resend RM_POLL / RM_SET, give up. Others: a
 * player silent for mp_drop_ms is flagged too (the coordinator may be the one that is gone). */
static void ServiceRemoval(uint32_t now) {
    uint8_t alive;
    int coord;
    int p;
    if (!gdx3ds_mpo_in_session() || sLs == NULL) {
        return;
    }
    alive = (uint8_t) (1u << sLb.mySlot);
    for (p = 0; p < sLs->numPlayers; p++) {
        const int slot = gdx_lobby_player_slot(&sLb, p);
        if (p == sLs->localPlayer || slot < 0 || ((sRm.removedMask >> p) & 1u)) {
            continue;
        }
        if ((int32_t) (now - sLastHeardMs[slot]) > SilenceLimitMs(slot)) {
            WantDrop(p, GDX_LB_ABORT_PEER_LOST, ((sHeardPads >> slot) & 1u) ? "silent" : "silent while loading");
        }
        if (!((sDropWanted >> p) & 1u)) {
            alive |= (uint8_t) (1u << slot);
        }
    }
    coord = gdx_rm_coordinator_slot(&sLb, &sRm, alive);
    if (coord == sLb.mySlot && ServiceGoneSet(now) < 0) {
        return;
    }
    if (coord == sLb.mySlot && sRm.stage == GDX_RM_IDLE && sDropWanted != 0) {
        for (p = 0; p < sLs->numPlayers; p++) {
            if ((sDropWanted >> p) & 1u) {
                const uint8_t before = sRm.removedMask;
                if (gdx_rm_begin(&sRm, &sLb, sLs, p, sDropReason[p], now)) {
                    MpoLog("[mpo] removing p%d: polling the others (I coordinate)", p);
                    sLastRmSendMs = 0;
                    if (sRm.removedMask != before) {
                        NoteRemoved(p, sRm.removedTick[p]);
                    }
                }
                break;
            }
        }
    }
    if (gdx_rm_tick(&sRm, now) < 0) {
        SetMessage("race ended: two players lost");
        AbortSession(GDX_LB_ABORT_PEER_LOST, sRm.player, "removal: a racer never answered");
        return;
    }
    if (gdx_rm_wants_send(&sRm, now) && (sLastRmSendMs == 0 || (int32_t) (now - sLastRmSendMs) >= GDX_RM_RESEND_MS)) {
        int s;
        sLastRmSendMs = now;
        for (s = 0; s < GDX_MPO_MAX_SLOTS; s++) {
            uint8_t pkt[GDX_LB_RM_BYTES];
            const int n = gdx_rm_build_for(&sRm, &sLb, sLs, s, now, pkt, sizeof(pkt));
            SendLobby(s, pkt, n, 0);
        }
    }
}

/* RM_POLL / RM_HAVE / RM_SET from slot `from` (already validated by the lobby). */
static void HandleRemoval(int type, int from, uint32_t now) {
    const GdxMpLbRemove* b = &sLb.rm;
    if (!gdx3ds_mpo_in_session() || sLs == NULL || SessionPlayerOfSlot(from) < 0) {
        return;
    }
    if (type == GDX_LB_RM_POLL) {
        uint8_t pkt[GDX_LB_RM_BYTES];
        const int n = gdx_rm_answer_poll(&sLb, sLs, from, b, now, pkt, sizeof(pkt));
        SendLobby(from, pkt, n, 0);
    } else if (type == GDX_LB_RM_HAVE) {
        const uint8_t before = sRm.removedMask;
        const int rc = gdx_rm_on_have(&sRm, &sLb, sLs, from, b, now);
        if (rc < 0) {
            AbortSession(GDX_LB_ABORT_PEER_LOST, b->player, "removal: the pads do not reach the agreed tick");
        } else if (sRm.removedMask != before) {
            NoteRemoved(sRm.player, sRm.removedTick[sRm.player]);
        }
    } else if (type == GDX_LB_RM_SET) {
        const int rc = gdx_rm_on_set(&sRm, &sLb, sLs, b, sRanTicks);
        if (rc == 1) {
            NoteRemoved(b->player, b->tick);
        } else if (rc == -2) {
            SetMessage("you were dropped from the race (connection)");
            AbortSession(GDX_LB_ABORT_PEER_LOST, sLs->localPlayer, NULL);
        } else if (rc < 0) {
            AbortSession(GDX_LB_ABORT_PEER_LOST, b->player, "removal: cannot follow (already past the tick)");
        }
    }
}

/* The race's VS points, from its whole field in finishing order (gdx3ds_mpg_last_field). */
static void AwardPoints(int noPoints) {
    const GdxRaceFieldSnapshot* f = gdx3ds_mpg_last_field();
    int rank = 0;
    int i;
    memset(sRacePoints, 0, sizeof(sRacePoints));
    memset(sRacePlace, 0, sizeof(sRacePlace));
    sByCpuSlots = 0;
    for (i = 0; i < f->count; i++) {
        const int p = f->byPos[i].racer;
        int slot;
        if (p < 0 || p >= sLb.start.numPlayers || p >= GDX_MPO_MAX_SLOTS) {
            continue;
        }
        slot = sLb.start.player[p].slot;
        if (slot >= GDX_MPO_MAX_SLOTS) {
            continue;
        }
        sRacePlace[slot] = i + 1;
        if ((sRm.removedMask >> p) & 1u) {
            sByCpuSlots |= (uint8_t) (1u << slot);
        } else if (!noPoints) {
            sRacePoints[slot] = kVsPoints[rank < 4 ? rank : 3];
        }
        sPoints[slot] += sRacePoints[slot];
        rank++;
    }
    MpoLog("[mpo] points race #%d: +%d/+%d/+%d/+%d total %d/%d/%d/%d", sRacesDone, sRacePoints[0], sRacePoints[1],
           sRacePoints[2], sRacePoints[3], sPoints[0], sPoints[1], sPoints[2], sPoints[3]);
}

static void StatsReceipt(void);

static const char* PlayerName(int player);

/* mp-menus #10: a race ended from the pause menu says so on every console ("<name> ended the
 * race") and scores nothing (it was not run to the finish). */
static int NoteEndedBy(void) {
    int choice = 0;
    const int by = gdx3ds_mpg_ended_by(&choice);
    sEndedByName[0] = '\0';
    if (by < 0) {
        return 0;
    }
    snprintf(sEndedByName, sizeof(sEndedByName), "%s", PlayerName(by));
    MpoLog("[mpo] race #%d ended from the pause menu by p%d (%s), choice %d: no points", sRacesDone, by,
           sEndedByName, choice);
    return 1;
}

static void EndRace(void) {
    int endedByPause;
    sRacesDone++;
    sPauseShown = -1;
    sPauseTouch = 0;
    gdx_mp_pause_gate_reset(&sPauseGate);
    endedByPause = NoteEndedBy();
    AwardPoints(endedByPause);
    MpoLog("[mpo] race end #%d tick=%lu desync=0 stalls=%lu stall_max_ms=%lu stall_ms=%lu rej=%lu clamped=%lu "
           "stall_ms_per_min=%lu", sRacesDone,
           (unsigned long) sLastLt, (unsigned long) sStallTicks, (unsigned long) (sStallMaxUs / 1000u),
           (unsigned long) (sStallUs / 1000u), (unsigned long) (sLs != NULL ? sLs->rejected : 0),
           (unsigned long) (sLs != NULL ? sLs->padsClamped : 0),
           (unsigned long) (sLastLt > 0 ? (uint64_t) (sStallUs / 1000u) * 3600u / sLastLt : 0));
    if (sLs != NULL) {
        StatsReceipt(); /* mp-online-hw: the links (direct / punch / relay), RTTs and counters it ended on */
    }
    gdx3ds_mpts_race_end(sLastLt, sStallTicks, sStallUs);
    if (endedByPause) {
        SetMessage("%s ended the race", sEndedByName);
    } else {
        SetMessage("race over (%d)", sRacesDone);
    }
    gdx3ds_mpg_leave();
    sLingerUntilMs = NowMs() + MPO_LINGER_MS;
    sLb.state = GDX_LB_STATE_LOBBY;
    SetPhase(GDX_MPO_LINGER);
    gdx3ds_filelog_flush();
}

static uint32_t Mix32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

static void RememberSessionEpochs(void) {
    int s;
    for (s = 0; s < GDX_MPO_MAX_SLOTS; s++) {
        sSessionEpoch[s] = sLb.peer[s].epoch;
        snprintf(sSessionName[s], sizeof(sSessionName[s]), "%s", s == sLb.mySlot ? sLb.name : sLb.peer[s].name);
    }
}

/* CRASHFIX (hw b5f620a: the New 3DS host died within a second of START and its async log lost
 * every line after "phase room -> launch"): [debug] filelog_launch_sync_ms (default 0) flushes
 * every log line synchronously from START for that long, so the next death names its step. */
static void LaunchSyncLog(void) {
    const int ms = ClampInt(gdx3ds_config_get_int("debug", "filelog_launch_sync_ms", 0), 0, 600000);
    if (ms > 0 && sModel == GDX_MPO_MODEL_O3DS && !sCfg.launchSyncO3ds) {
        MpoLog("[mpo] launch: filelog_launch_sync_ms=%d not applied on an Old 3DS (debug.filelog_launch_sync_o3ds=0)", ms);
        return;
    }
    if (ms > 0) {
        MpoLog("[mpo] launch: every log line synchronous for %d ms (debug.filelog_launch_sync_ms), "
               "up to race tick %d (debug.filelog_launch_sync_race_ticks)", ms, sCfg.launchSyncRaceTicks);
        gdx3ds_filelog_sync_for_ms((unsigned) ms);
        sLaunchSyncOn = 1;
    }
}

/* The launch sync window is for a death between START and the race; in the race its synchronous
 * lines (each [hitch] receipt among them) cost a frame each. */
static void EndLaunchSyncInRace(uint32_t lt) {
    if (!sLaunchSyncOn || sCfg.launchSyncRaceTicks < 0 || lt < (uint32_t) sCfg.launchSyncRaceTicks) {
        return;
    }
    sLaunchSyncOn = 0;
    gdx3ds_filelog_sync_end();
    MpoLog("[mpo] launch sync window closed at race tick %lu (debug.filelog_launch_sync_race_ticks=%d)",
           (unsigned long) lt, sCfg.launchSyncRaceTicks);
}

/* mp-menus receipt (edge #9, not fixed): each racer's link at START -- its state, how long it has
 * been in it, how long since a direct datagram and since the lobby heard it -- and (below) when its
 * first race pad came, so a slow start after a slot changed hands can be laid at the link's door. */
static void StartLinkReceipt(uint32_t now) {
    char links[200];
    int off = 0;
    int i;
    links[0] = '\0';
    for (i = 0; i < sLb.start.numPlayers && off < (int) sizeof(links) - 56; i++) {
        const int slot = sLb.start.player[i].slot;
        const GdxMpLink* l = (slot == sLb.mySlot || slot >= GDX_MPO_MAX_SLOTS) ? NULL : gdx3ds_p2p_link(slot);
        if (l == NULL) {
            continue;
        }
        off += snprintf(links + off, sizeof(links) - (size_t) off, " s%d:%s for=%lu rx_age=%ld heard_age=%lu", slot,
                        gdx_link_state_name(l->state), (unsigned long) (now - l->sinceMs),
                        l->heardDirect ? (long) (now - l->lastDirectRxMs) : -1L,
                        (unsigned long) (now - sLastHeardMs[slot]));
    }
    MpoLog("[mpo] start links:%s", links);
}

static void FirstPadReceipt(int slot, uint32_t now) {
    const GdxMpLink* l;
    if ((sHeardPads >> slot) & 1u) {
        return;
    }
    l = gdx3ds_p2p_link(slot);
    MpoLog("[mpo] first pad from s%d %lu ms after START (link %s)", slot, (unsigned long) (now - sLaunchMs),
           l != NULL ? gdx_link_state_name(l->state) : "none");
}

/* A START was taken (ours or the host's): the lockstep exists from now on, so the pads of a
 * console that reaches its first race tick before us are kept. */
static void BeginLaunch(void) {
    const GdxMpLbStart* st = &sLb.start;
    const int me = gdx_lobby_my_player(&sLb);
    sLb.startIsNew = 0;
    if (me < 0) {
        return;
    }
    FreeLockstep();
    sSession = gdx_mpo_session_digest(sBuild, sContentHash, st);
    sLs = (GdxMpLockstep*) calloc(1, sizeof(GdxMpLockstep));
    if (sLs == NULL || gdx_mp_ls_init(sLs, st->numPlayers, me, st->delay, st->hashEvery, sSession) != 0) {
        MpoLog("[mpo] START refused: lockstep init (players=%d me=%d delay=%d)", st->numPlayers, me, st->delay);
        FreeLockstep();
        return;
    }
    sLs->wireSlot = (uint8_t) (sLb.mySlot + 1);
    StartLinkReceipt(NowMs());
    RememberSessionEpochs();
    gdx_rm_reset(&sRm);
    sRetiredMask = 0;
    sCpuNotedMask = 0;
    sDropWanted = 0;
    sDropSerial = 0;
    sDropPlayer = -1;
    sLb.removedMask = 0;
    sRanTicks = 0;
    sHeardPads = 0;
    {
        int s;
        const uint32_t t = NowMs();
        for (s = 0; s < GDX_MPO_MAX_SLOTS; s++) {
            sLastHeardMs[s] = t;
            sLoadSilenceMaxMs[s] = 0;
        }
    }
    sAnyRemotePad = 0;
    sDesyncReported = 0;
    sStallTicks = 0;
    RateReset();
    sStallMaxUs = 0;
    sStallUs = 0;
    sLaunchMs = NowMs();
    sLb.state = GDX_LB_STATE_LAUNCHING;
    sPickWanted = GDX_MPP_NONE;
    sPickDeadlineMs = 0;
    gdx3ds_mpp_release(); /* the user's settings are back before the race saves them */
    gdx3ds_mpg_arm(st, sCfg.endAfter);
    sEndedByName[0] = '\0';
    sPauseShown = -1;
    gdx_mp_pause_gate_reset(&sPauseGate);
    {
        char who[96];
        int off = 0;
        int i;
        who[0] = '\0';
        for (i = 0; i < st->numPlayers && off < (int) sizeof(who) - 16; i++) {
            off += snprintf(who + off, sizeof(who) - (size_t) off, " p%d=s%d/m%d", i, st->player[i].slot,
                            st->player[i].machine);
        }
        MpoLog("[mpo] START gen=%lu %s players=%d racers=%d (vs_com=%d) me=p%d delay=%d hash_every=%d bots=%02x "
               "session=%08lx%s",
               (unsigned long) st->gen, gdx3ds_mpg_describe(), st->numPlayers,
               st->vsCom ? (st->racers != 0 ? st->racers : 4) : st->numPlayers,
               st->vsCom, me, st->delay, st->hashEvery, st->botMask, (unsigned long) sSession, who);
    }
    gdx3ds_mpab_race_start(st->gen, !sUdsInRoom); /* card-3: the arm of the per-race net A/B, before NDM */
    SetMessage("starting %s", gdx3ds_mpg_describe());
    SetPhase(GDX_MPO_LAUNCH);
    LaunchSyncLog();
}

static int EffectiveRacers(void);

/* mp-online-hw: what the host's START delay came from, and every link it races over. */
static void DelayPickReceipt(void) {
    const uint32_t now = NowMs();
    uint32_t worstRtt = 0;
    uint32_t worstJitter = 0;
    const int picked = gdx_lobby_pick_delay(&sLb, now, &worstRtt, &worstJitter);
    const GdxP2pStatus* st = gdx3ds_p2p_status();
    char links[160];
    int off = 0;
    int s;
    links[0] = '\0';
    for (s = 0; s < GDX_MPO_MAX_SLOTS && off < (int) sizeof(links) - 40; s++) {
        const GdxMpLink* l = gdx3ds_p2p_link(s);
        if (s == sLb.mySlot || l == NULL || l->state == GDX_LINK_NONE) {
            continue;
        }
        off += snprintf(links + off, sizeof(links) - (size_t) off, " s%d:%s rtt=%lu jit=%lu", s,
                        gdx_link_state_name(l->state), (unsigned long) gdx_lobby_peer_rtt(&sLb, s),
                        (unsigned long) gdx_lobby_peer_jitter(&sLb, s));
    }
    MpoLog("[mpo] delay pick: worst_rtt=%lu worst_jitter=%lu -> %d (mp_delay=%d, used %d) mode=%s relay_only=%d "
           "srv_rtt=%lu |%s",
           (unsigned long) worstRtt, (unsigned long) worstJitter, picked, sCfg.delay, sLb.start.delay,
           st->mode == GDX_P2P_MODE_ONLINE ? "online" : (sUdsInRoom ? "uds" : "wifi"), st->forceRelay,
           (unsigned long) st->serverRttMs, links);
}

static void HostStart(void) {
    GdxMpLobbySettings s;
    uint32_t r[4];
    uint64_t token;
    uint32_t seed = (uint32_t) svcGetSystemTick() ^ (uint32_t) osGetTime() ^ sBotLcg;
    int i;
    int n;
    if (sPhase != GDX_MPO_ROOM || sLb.mySlot != 0 || sRoomClosed) {
        return;
    }
    gdx3ds_mpg_current_settings(&s);
    s.course = (uint8_t) sRoomCourse; /* the course every console reported on (ServiceCourse) */
    if (sCfg.vsCom >= 0) {
        s.vsCom = (uint8_t) (sCfg.vsCom ? 1 : 0);
    }
    s.hashEvery = (uint8_t) sCfg.hashEvery;
    s.delayOverride = sCfg.delay;
    s.laps = (uint8_t) sCfg.laps;
    s.racers = (uint8_t) EffectiveRacers();
    for (i = 0; i < 4; i++) {
        seed = Mix32(seed + 0x9E3779B9u * (uint32_t) (i + 1));
        r[i] = seed;
    }
    token = ((uint64_t) Mix32(seed ^ 0x51ED270Bu) << 32) | Mix32(seed ^ 0x2545F491u);
    n = gdx_lobby_host_make_start(&sLb, &s, r[0], r[1] | 1u, r[2], r[3] | 1u, token, NowMs());
    if (n < 2) {
        const char* why = "";
        gdx_lobby_host_startable(&sLb, NowMs(), &why);
        SetMessage("cannot start: %s", why);
        return;
    }
    sRacesStarted++;
    DelayPickReceipt();
    SendStarts(NowMs());
    BeginLaunch();
}

/* mp-menus (edge #8): the host starts without the members that went quiet (HOME, sleep); they
 * stay in the room and pick up from the next round. */
void gdx3ds_mpo_ui_start_without_away(void) {
    int away = -1;
    if (sPhase != GDX_MPO_ROOM || !sLbInit || sLb.mySlot != 0 ||
        gdx_lobby_host_startable_without_away(&sLb, NowMs(), &away) < 2) {
        return;
    }
    MpoLog("[mpo] START without the quiet members (first: slot %d %s)", away, away >= 0 ? sLb.peer[away].name : "?");
    sLb.skipAway = 1;
    HostStart();
    sLb.skipAway = 0;
}

void gdx3ds_mpo_ui_start(void) {
    if (sPhase != GDX_MPO_ROOM) {
        return;
    }
    if (sLb.mySlot != 0) {
        SetMessage("only the host starts the race");
        return;
    }
    HostStart();
}

void gdx3ds_mpo_ui_toggle_ready(void) {
    if (sPhase == GDX_MPO_ROOM && sLbInit) {
        sLb.ready = !sLb.ready;
        sLastHelloMs = 0; /* tell everyone now */
    }
}

void gdx3ds_mpo_ui_cycle_machine(int dir) {
    if (sPhase == GDX_MPO_ROOM && sLbInit) {
        sLb.machine = (sLb.machine + (dir < 0 ? GDX_MPO_MAX_MACHINE : 1)) % (GDX_MPO_MAX_MACHINE + 1);
        sLastHelloMs = 0;
    }
}

/* MP30: the host's VS COM machine count. 0 in sCfg.racers = the default. */
static const int kRacerChoices[4] = { 4, 8, 16, 30 };

/* MP30: an Old 3DS (268 MHz, 64 MB) in the room caps the field at 16 machines -- 30 machines'
 * AI, physics and draws are New 3DS work -- unless [net] mp_racers_o3ds_30 = 1 on the HOST (a
 * test lever). Counted over this console and every peer heard so far (HELLO's model). */
#define MPO_O3DS_RACERS_CAP 16

static int RoomHasOld3ds(void) {
    int s;
    if (sModel == GDX_MPO_MODEL_O3DS) {
        return 1;
    }
    for (s = 0; s < GDX_MPO_MAX_SLOTS; s++) {
        if (s != sLb.mySlot && sLb.peer[s].present && sLb.peer[s].heard &&
            sLb.peer[s].hello.model == GDX_MPO_MODEL_O3DS) {
            return 1;
        }
    }
    return 0;
}

static int RacersCap(void) {
    if (RoomHasOld3ds() && !sCfg.racersO3ds30) {
        return MPO_O3DS_RACERS_CAP;
    }
    return GDX_MPO_MAX_RACERS;
}

/* The default is the most the room can take: 30 on New 3DS only, 16 with an Old 3DS. */
static int RacersDefault(void) {
    return RoomHasOld3ds() ? MPO_O3DS_RACERS_CAP : GDX_MPO_MAX_RACERS;
}

/* What a START would carry now (never above the cap, never below the humans). */
static int EffectiveRacers(void) {
    int n = sCfg.racers != 0 ? sCfg.racers : RacersDefault();
    if (n > RacersCap()) {
        n = RacersCap();
    }
    if (n < GDX_MPO_MIN_RACERS) {
        n = GDX_MPO_MIN_RACERS;
    }
    return n;
}

void gdx3ds_mpo_ui_cycle_racers(int dir) {
    int i;
    int cur;
    if (sPhase != GDX_MPO_ROOM || !sLbInit || sLb.mySlot != 0) {
        return;
    }
    cur = EffectiveRacers();
    for (i = 0; i < 4 && kRacerChoices[i] != cur; i++) {
    }
    if (i == 4) {
        i = 0;
    }
    do {
        i = (i + (dir < 0 ? 3 : 1)) % 4;
    } while (kRacerChoices[i] > RacersCap());
    sCfg.racers = kRacerChoices[i];
}

void gdx3ds_mpo_ui_cycle_laps(int dir) {
    if (sPhase == GDX_MPO_ROOM && sLbInit && sLb.mySlot == 0) {
        sCfg.laps = 1 + (sCfg.laps - 1 + (dir < 0 ? 8 : 1)) % 9;
    }
}

/* After a race: the host runs the same course again, or the next one. */
void gdx3ds_mpo_ui_rematch(int nextCourse) {
    if (sPhase != GDX_MPO_ROOM || sLb.mySlot != 0) {
        return;
    }
    if (nextCourse) {
        gdx3ds_mpo_ui_cycle_course(1);
    }
    HostStart();
}

void gdx3ds_mpo_ui_cycle_course(int dir) {
    if (sPhase == GDX_MPO_ROOM && sLbInit && sLb.mySlot == 0) {
        const int c = (sRoomCourse >= 0 && sRoomCourse < GDX_MPO_STOCK_COURSES) ? sRoomCourse : 0;
        sRoomCourse = (c + (dir < 0 ? GDX_MPO_STOCK_COURSES - 1 : 1)) % GDX_MPO_STOCK_COURSES;
    }
}

/* ---- stock picks (../gdx3ds_mp_pick.h): rounds of machine select, then the host's course ----- */

static int StockPickOn(void) {
    return sCfg.stockPick && sCfg.autoMode[0] == '\0'; /* the mp_auto lanes keep the ready rule */
}

static int InitialRoomCourse(void) {
    if (sCfg.course >= 0) {
        return sCfg.course;
    }
    return (gCourseIndex >= 0 && gCourseIndex < GDX_MPO_STOCK_COURSES) ? gCourseIndex : 0;
}

/* A console back in a room starts its machine select on its last pick (a rejoin after a drop). */
static void SeedFromLastPick(void) {
    if (!sLastPick.valid || !StockPickOn()) {
        return;
    }
    sLb.machine = sLastPick.machine;
    sLb.skin = sLastPick.skin;
    sLb.engineBits = sLastPick.engineBits;
    sLb.engine = (int) (gdx_mpo_engine_from_bits(sLb.engineBits) * 100.0f + 0.5f);
}

static void ResetPicks(void) {
    sPickDeadlineMs = 0;
    sPickWanted = GDX_MPP_NONE;
    sCourseStepOpened = 0;
    sKnownMask = 0;
    sAutoRoundSlot = -1;
    sCourseChosen = 0;
    sAutoRound = 0;
    sRoomCourse = InitialRoomCourse();
    sLb.requirePicks = StockPickOn();
}

static void AskPicker(int kind) {
    sPickWanted = kind; /* ServicePicker retries until the top screen takes it */
}

static void ServicePicker(void) {
    float engine;
    if (sPickWanted == GDX_MPP_NONE || gdx3ds_mpg_stage() != GDX_MPG_IDLE) {
        return;
    }
    engine = gdx_mpo_engine_from_bits(sLb.engineBits);
    if (gdx3ds_mpp_begin(sPickWanted, sLb.machine, sLb.skin, engine, sRoomCourse)) {
        sPickWanted = GDX_MPP_NONE;
    }
}

static void StartMachineStep(uint32_t now, uint32_t msLeft) {
    sLb.picked = 0;
    sLb.pickWaitOver = 0;
    sCourseStepOpened = 0;
    sPickDeadlineMs = now + msLeft;
    if (sPickDeadlineMs == 0) {
        sPickDeadlineMs = 1;
    }
    AskPicker(GDX_MPP_MACHINE);
    sLastHelloMs = 0; /* tell everyone now */
    MpoLog("[mpo] pick round %d: machine select for %lu ms", sLb.pickRound, (unsigned long) msLeft);
}

/* Host: open the next round on every console at once (its HELLO carries the time left from now). */
static void HostOpenRound(uint32_t now) {
    sAutoRound = 0;
    sLb.pickRound = (uint8_t) (sLb.pickRound == 255 ? 1 : sLb.pickRound + 1);
    StartMachineStep(now, (uint32_t) sCfg.pickMs);
    sLb.pickMsLeft = (uint32_t) sCfg.pickMs;
}

/* Joiner: follow the host into its round, counting from the host's word on the time left (less
 * half the round trip it took to get here). A round whose machine step is over: no picker, the
 * host races us as the default. */
static void FollowHostRound(uint32_t now) {
    const int round = gdx_lobby_host_round(&sLb);
    const uint32_t left = gdx_lobby_host_ms_left(&sLb);
    uint32_t rtt;
    if (round == 0 || round == sLb.pickRound || left == GDX_LB_NO_ROUND) {
        return; /* no round, ours already, or the host has not said how long it runs yet */
    }
    sLb.pickRound = (uint8_t) round;
    if (left == 0) {
        sLb.picked = 0;
        sPickDeadlineMs = 0;
        sLastHelloMs = 0;
        MpoLog("[mpo] pick round %d: joined after its machine step", round);
        return;
    }
    rtt = gdx_lobby_peer_rtt(&sLb, 0);
    rtt = (rtt == GDX_LB_NO_RTT) ? 0 : rtt / 2u;
    StartMachineStep(now, left > rtt ? left - rtt : 1u);
}

static void ApplyMachinePick(int machine, int skin, float engine, const char* how) {
    sLb.machine = machine;
    sLb.skin = skin;
    sLb.engineBits = gdx_mpo_engine_bits(engine);
    sLb.engine = (int) (gdx_mpo_engine_from_bits(sLb.engineBits) * 100.0f + 0.5f);
    sLb.picked = (sLb.pickRound != 0);
    sLb.ready = 1;
    sLastHelloMs = 0;
    sLastPick.valid = 1;
    sLastPick.machine = machine;
    sLastPick.skin = skin;
    sLastPick.engineBits = sLb.engineBits;
    MpoLog("[mpo] pick round %d: machine %d skin %d engine %08lx (%s)", sLb.pickRound, machine, skin,
           (unsigned long) sLb.engineBits, how);
}

static void TakePicks(void) {
    int machine;
    int skin;
    int course;
    float engine;
    if (gdx3ds_mpp_take_machine(&machine, &skin, &engine)) {
        ApplyMachinePick(machine, skin, engine, "picked");
    }
    if (gdx3ds_mpp_take_course(&course) && sLb.mySlot == 0) {
        sCourseDeadlineMs = 0;
        sRoomCourse = course;
        sCourseChosen = 1;
        sLastHelloMs = 0;
        MpoLog("[mpo] pick round %d: course %d", sLb.pickRound, course);
    }
}

/* The machine step's time is up on this console: confirm the cursor's machine. */
static void ServicePickTimer(uint32_t now) {
    if (sPickDeadlineMs == 0 || sLb.picked || (int32_t) (now - sPickDeadlineMs) < 0) {
        return;
    }
    if (sPickWanted == GDX_MPP_MACHINE) {
        sPickWanted = GDX_MPP_NONE; /* the picker never came up: the cursor would have started here */
        ApplyMachinePick(sLb.machine, sLb.skin, gdx_mpo_engine_from_bits(sLb.engineBits), "time up, no picker");
        return;
    }
    gdx3ds_mpp_timeout();
    TakePicks();
    if (!sLb.picked) {
        ApplyMachinePick(sLb.machine, sLb.skin, gdx_mpo_engine_from_bits(sLb.engineBits), "time up");
    }
}

static uint32_t HostMsLeft(uint32_t now) {
    if (sLb.pickRound == 0 || sPickDeadlineMs == 0) {
        return GDX_LB_NO_ROUND;
    }
    if ((int32_t) (now - sPickDeadlineMs) >= 0) {
        return 0;
    }
    return sPickDeadlineMs - now;
}

/* mp-menus (edge #11): the host's course step runs [net] mp_course_ms; then the highlighted
 * course is the pick, so an idle host does not hold the room forever. */
static void AskCourse(uint32_t now) {
    AskPicker(GDX_MPP_COURSE);
    sCourseDeadlineMs = 0;
    if (sCfg.courseMs > 0) {
        sCourseDeadlineMs = now + (uint32_t) sCfg.courseMs;
        sCourseDeadlineMs = sCourseDeadlineMs == 0 ? 1 : sCourseDeadlineMs;
    }
}

static void ServiceCourseTimer(uint32_t now) {
    if (sCourseDeadlineMs == 0 || (int32_t) (now - sCourseDeadlineMs) < 0) {
        return;
    }
    sCourseDeadlineMs = 0;
    if (sPickWanted == GDX_MPP_COURSE) {
        sPickWanted = GDX_MPP_NONE; /* the picker never came up: the room's course stays */
        sCourseChosen = 1;
        MpoLog("[mpo] pick round %d: course step timed out before the picker: course %d stays", sLb.pickRound,
               sRoomCourse);
        return;
    }
    gdx3ds_mpp_course_timeout();
    TakePicks();
    MpoLog("[mpo] pick round %d: course step timed out (course %d)", sLb.pickRound, sRoomCourse);
}

static int CourseSecsLeft(uint32_t now) {
    if (sCourseDeadlineMs == 0 || (int32_t) (sCourseDeadlineMs - now) <= 0) {
        return -1;
    }
    return (int) ((sCourseDeadlineMs - now + 999u) / 1000u);
}

/* Host: once every pick is in (or the step's time and grace are up), the course step. */
static void ServiceHostCourseStep(uint32_t now) {
    sLb.pickMsLeft = HostMsLeft(now);
    sLb.pickWaitOver = sPickDeadlineMs != 0 && (int32_t) (now - (sPickDeadlineMs + GDX_MPO_PICK_GRACE_MS)) >= 0;
    if (sLb.pickRound == 0 || sCourseStepOpened || !sLb.picked) {
        return;
    }
    if (!gdx_lobby_picks_in(&sLb) && !sLb.pickWaitOver) {
        return;
    }
    sCourseStepOpened = 1;
    if (sAutoRound && sCourseChosen) {
        MpoLog("[mpo] pick round %d: every pick is in, the room's course stays (%d)", sLb.pickRound, sRoomCourse);
        return; /* a round the host did not ask for: no course select for it either */
    }
    AskCourse(now);
    MpoLog("[mpo] pick round %d: %s, the host picks the course", sLb.pickRound,
           sLb.pickWaitOver ? "time up" : "every pick is in");
}

/* The course every console reports on: the host's own pick; a joiner's copy of the host's, with
 * this console's key for it. */
static void ServiceCourse(void) {
    const int course = (sLb.mySlot == 0) ? sRoomCourse : gdx_lobby_host_course(&sLb);
    uint32_t key;
    if (course < 0) {
        return;
    }
    key = gdx3ds_mpp_course_key(course);
    if (course != sLb.course || key != sLb.courseKey) {
        gdx_lobby_set_course(&sLb, course, key);
        sLastHelloMs = 0;
    }
}

static void ServicePicking(void) {
    const int kind = gdx3ds_mpp_active();
    const int picking = kind == GDX_MPP_COURSE ? GDX_LB_PICK_COURSE
                        : kind == GDX_MPP_MACHINE ? GDX_LB_PICK_MACHINE
                                                  : GDX_LB_PICK_NONE;
    if (picking != sLb.picking) {
        sLb.picking = picking;
        sLastHelloMs = 0;
    }
}

static void ServiceAutoRound(uint32_t now);

static void ServicePicks(uint32_t now) {
    if (sPhase != GDX_MPO_ROOM || !sLbInit || sRoomClosed) {
        return;
    }
    gdx3ds_mpp_frame();
    TakePicks();
    ServicePicking();
    if (sLb.mySlot != 0) {
        FollowHostRound(now);
    }
    ServicePickTimer(now);
    if (sLb.mySlot == 0) {
        ServiceAutoRound(now);
        ServiceHostCourseStep(now);
        ServiceCourseTimer(now);
    }
    ServiceCourse();
    ServicePicker();
}

/* ---- mp-menus: the automatic round and KEEP --------------------------------------------------- */

/* Host: a slot in the room and heard that was not (or was another console) when last looked;
 * -1 = none. Every look forgets the slots that left, so a rejoin counts as a new joiner. */
static int TakeNewJoiner(void) {
    int found = -1;
    int s;
    for (s = 1; s < GDX_MPO_MAX_SLOTS; s++) {
        const GdxMpLobbyPeer* p = &sLb.peer[s];
        const uint8_t bit = (uint8_t) (1u << s);
        if (!p->present) {
            sKnownMask &= (uint8_t) ~bit;
            continue;
        }
        if (!p->heard) {
            continue;
        }
        if (!(sKnownMask & bit) || sKnownEpoch[s] != p->epoch) {
            sKnownMask |= bit;
            sKnownEpoch[s] = p->epoch;
            found = found < 0 ? s : found;
        }
    }
    return found;
}

static int MachineStepRunning(uint32_t now) {
    return sPickDeadlineMs != 0 && (int32_t) (now - sPickDeadlineMs) < 0;
}

/* mp-menus (edge #3): a joiner follows the running machine step only with this much of it left;
 * with less it would race the default it never saw, so it gets a round of its own after. */
#define MPO_AUTO_FOLLOW_MIN_MS 15000

static int MachineStepHasTimeFor(uint32_t now) {
    return MachineStepRunning(now) && (int32_t) (sPickDeadlineMs - now) >= MPO_AUTO_FOLLOW_MIN_MS;
}

/* Host: the top screen is busy with this room's own picking (the course select, or a picker on
 * its way), or a race is being launched: the round waits. */
static int AutoRoundMustWait(void) {
    return sPickWanted != GDX_MPP_NONE || gdx3ds_mpp_active() == GDX_MPP_COURSE ||
           gdx3ds_mpg_stage() != GDX_MPG_IDLE;
}

static void ServiceAutoRound(uint32_t now) {
    const int joiner = TakeNewJoiner();
    if (sPhase != GDX_MPO_ROOM || !sLbInit || sLb.mySlot != 0 || !StockPickOn()) {
        return;
    }
    if (joiner >= 0 && sLb.pickRound != 0) {
        sAutoRoundSlot = joiner;
        MpoLog("[mpo] auto round: %s joined (slot %d) after round %d", sLb.peer[joiner].name, joiner, sLb.pickRound);
    }
    if (sAutoRoundSlot < 0) {
        return;
    }
    if (MachineStepHasTimeFor(now)) {
        MpoLog("[mpo] auto round: slot %d joins the running round %d", sAutoRoundSlot, sLb.pickRound);
        sAutoRoundSlot = -1; /* it follows the host into this round's machine step */
        return;
    }
    if (MachineStepRunning(now) || AutoRoundMustWait()) {
        return; /* too little of this step left for it: its own round once this one is over */
    }
    HostOpenRound(now);
    sAutoRound = 1;
    SetMessage("%s joined: new pick round", sLb.peer[sAutoRoundSlot].name);
    MpoLog("[mpo] auto round %d for %s (slot %d)", sLb.pickRound, sLb.peer[sAutoRoundSlot].name, sAutoRoundSlot);
    sAutoRoundSlot = -1;
}

/* KEEP: the machine step is running and this console has not picked; its last pick is known. */
static int KeepOffered(uint32_t now) {
    return sPhase == GDX_MPO_ROOM && sLbInit && StockPickOn() && sLb.pickRound != 0 && !sLb.picked &&
           MachineStepRunning(now) && sLastPick.valid;
}

void gdx3ds_mpo_ui_keep_pick(void) {
    const float engine = gdx_mpo_engine_from_bits(sLastPick.engineBits);
    if (!KeepOffered(NowMs())) {
        return;
    }
    if (sPickWanted == GDX_MPP_MACHINE) {
        sPickWanted = GDX_MPP_NONE; /* the picker never came up: nothing to close */
        ApplyMachinePick(sLastPick.machine, sLastPick.skin, engine, "kept");
        return;
    }
    gdx3ds_mpp_keep(sLastPick.machine, sLastPick.skin, engine);
    TakePicks();
}

/* Host: every console into machine select (the first round, or CHANGE MACHINES after a race). */
void gdx3ds_mpo_ui_open_machine_select(void) {
    if (sPhase != GDX_MPO_ROOM || !sLbInit || sLb.mySlot != 0 || !StockPickOn() || sRoomClosed) {
        return;
    }
    HostOpenRound(NowMs());
}

/* Host: the course again (CHANGE COURSE), once its machine is in. */
void gdx3ds_mpo_ui_pick_course(void) {
    if (sPhase != GDX_MPO_ROOM || !sLbInit || sLb.mySlot != 0 || !StockPickOn() || !sLb.picked) {
        return;
    }
    sCourseStepOpened = 1;
    AskCourse(NowMs());
}

/* ---- receiving ------------------------------------------------------------------------------ */
static int SessionPlayerOfSlot(int slot) {
    int i;
    if (!sLb.haveStart) {
        return -1;
    }
    for (i = 0; i < sLb.start.numPlayers; i++) {
        if (sLb.start.player[i].slot == slot) {
            return i;
        }
    }
    return -1;
}

static void HandleLobbyDatagram(const uint8_t* buf, int n, int hint, uint32_t now) {
    int from = -1;
    const int type = gdx_lobby_ingest(&sLb, buf, n, now, &from);
    if (type < 0) {
        sRejected++;
        return;
    }
    if (hint >= 0 && hint != from) {
        sRejected++; /* cannot happen (the transport bound `hint` from this very `from`) */
        return;
    }
    NoteHeard(from, now);
    if (type == GDX_LB_HELLO) {
        gdx3ds_p2p_on_peer_report(from, (sLb.peer[from].hello.directMask >> sLb.mySlot) & 1, now);
        gdx_rm_on_ack(&sRm, &sLb, from, sLb.peer[from].hello.removedMask);
        return;
    }
    if (type == GDX_LB_RM_POLL || type == GDX_LB_RM_HAVE || type == GDX_LB_RM_SET) {
        HandleRemoval(type, from, now);
        return;
    }
    if (type == GDX_LB_START && sLb.startIsNew) {
        if (sPhase == GDX_MPO_ROOM) {
            BeginLaunch();
        } else if (gdx3ds_mpo_in_session()) {
            /* A newer START while we still race the old one: the host left that race (its ABORT
             * got lost). End ours quietly and take the new one. */
            const GdxMpLbStart next = sLb.start;
            SetMessage("the host restarted the race");
            sLb.startIsNew = 0;
            AbortSession(GDX_LB_ABORT_QUIT, 0, NULL);
            sLb.start = next;
            sLb.startIsNew = 1;
            BeginLaunch();
        } /* LINGER: BackToLobby takes it */
        return;
    }
    if (type == GDX_LB_ABORT && sLb.abortIsNew) {
        sLb.abortIsNew = 0;
        if (gdx3ds_mpo_in_session() && sLb.abort.gen == sLb.start.gen && SessionPlayerOfSlot(from) >= 0) {
            if (sLb.abort.reason == GDX_LB_ABORT_DESYNC) {
                SetMessage("race ended: %s %s", sLb.peer[from].name, AbortText(sLb.abort.reason));
                AbortSession(sLb.abort.reason, SessionPlayerOfSlot(from), NULL);
            } else {
                /* it left (quit, suspended, lost us, could not load): the race goes on without it */
                WantDrop(SessionPlayerOfSlot(from), sLb.abort.reason, AbortText(sLb.abort.reason));
            }
        }
    }
}

static void HandleLockstepDatagram(const uint8_t* buf, int n, int hint, uint32_t now) {
    const int from = gdx_mp_ls_packet_from(buf, n);
    const int wire = gdx_mp_ls_packet_wire_slot(buf, n);
    int slot;
    if (sLs == NULL || from < 0 || from >= sLs->numPlayers || from == sLs->localPlayer) {
        sRejected++;
        return;
    }
    slot = gdx_lobby_player_slot(&sLb, from);
    if (slot < 0 || wire != slot + 1 || (hint >= 0 && hint != slot)) {
        sRejected++; /* a player index claimed by someone else's address / slot */
        return;
    }
    NoteHeard(slot, now);
    if (!((sHeardPads >> slot) & 1u)) {
        MpoLog("[mpo] p%d: first race pad %lu ms after START (longest silence while it loaded: %lu ms, limit %d)",
               from, (unsigned long) (now - sLaunchMs), (unsigned long) sLoadSilenceMaxMs[slot],
               SilenceLimitMs(slot));
    }
    FirstPadReceipt(slot, now);
    sHeardPads |= (uint8_t) (1u << slot);
    if (gdx_mp_ls_ingest(sLs, buf, n, now) < 0) {
        return;
    }
    sAnyRemotePad = 1;
    if ((sLs->peerFlags[from] & GDX_MP_FLAG_ABORT) && gdx3ds_mpo_in_session()) {
        WantDrop(from, GDX_LB_ABORT_QUIT, "its pads say it left");
    }
}

static void Drain(uint32_t now) {
    uint8_t buf[GDX_MP_TRANSPORT_MTU];
    const GdxMpTransportOps* tx = gdx3ds_mp_p2p_ops();
    int guard;
    for (guard = 0; guard < 128; guard++) {
        int hint = GDX_MP_PEER_ANY;
        const int n = tx->recv(buf, sizeof(buf), &hint);
        if (n <= 0) {
            return;
        }
        if (!sLbInit) {
            continue;
        }
        if (n >= 4 && memcmp(buf, "GXLB", 4) == 0) {
            HandleLobbyDatagram(buf, n, hint >= 0 ? hint : -1, now);
        } else if (n >= 4 && memcmp(buf, "GXMP", 4) == 0) {
            HandleLockstepDatagram(buf, n, hint >= 0 ? hint : -1, now);
        }
    }
}

/* ---- mp-menus: the room went away ----------------------------------------------------------- */

static int RoomLostCode(const GdxP2pStatus* st) {
    return st->phase == GDX_P2P_FAILED ? st->errorCode : st->roomLost;
}

static int HostIsGone(int code) {
    return code == GDX_RV_E_HOST_LEFT || code == GDX_P2P_LOST_HOST_SILENT;
}

static const char* RoomLostText(const GdxP2pStatus* st) {
    const int code = RoomLostCode(st);
    if (code == GDX_RV_E_HOST_LEFT) {
        return "the host left";
    }
    if (code == GDX_P2P_LOST_HOST_SILENT) {
        return "lost the host";
    }
    if (code == GDX_P2P_LOST_SERVER_SILENT) {
        return "lost the server";
    }
    if (code > 0) {
        return gdx_rv_error_text(code);
    }
    return st->phase == GDX_P2P_FAILED ? st->errorDetail : st->roomLostDetail;
}

/* The server still relays after the host left an online room of three or more (it keeps the room
 * up for the guests until they leave: gdx_rendezvous.py _orphan_room); a server that went away or
 * forgot us, and a WiFi room's host (its own server), relay nothing. */
static int RelayOutlivesRoom(const GdxP2pStatus* st) {
    return st->mode == GDX_P2P_MODE_ONLINE && st->phase == GDX_P2P_IN_ROOM && RoomLostCode(st) == GDX_RV_E_HOST_LEFT;
}

/* The race can finish without the room: every racer still in it (not removed, not on its way out,
 * not the host that left) is on a direct link, or the server still relays (RelayOutlivesRoom). A
 * host that left is flagged for removal here: its machine goes to the CPU like any player who
 * drops. */
static int RaceCanGoOnAlone(const GdxP2pStatus* st, uint32_t now) {
    const int mask = RelayOutlivesRoom(st) ? 0xFF : gdx3ds_p2p_direct_mask(now);
    const int hostGone = HostIsGone(RoomLostCode(st));
    int p;
    if (sLs == NULL || st->phase == GDX_P2P_FAILED) {
        return 0;
    }
    for (p = 0; p < sLs->numPlayers; p++) {
        const int slot = gdx_lobby_player_slot(&sLb, p);
        if (p == sLs->localPlayer || slot < 0 || ((sRm.removedMask >> p) & 1u) || ((sDropWanted >> p) & 1u)) {
            continue;
        }
        if (hostGone && slot == 0) {
            WantDrop(p, GDX_LB_ABORT_QUIT, "the host left the room");
            continue;
        }
        if (!((mask >> slot) & 1)) {
            return 0;
        }
    }
    return 1;
}

/* After the race the room was lost under: no more network; the lobby keeps its results (and the
 * running points) on screen until the player leaves (gdx3ds_mpo_ui_leave). */
static void CloseRoomAfterRace(void) {
    sRoomClosed = 1;
    sRoomLostInRace = 0;
    FreeLockstep();
    gdx3ds_mpp_close();
    sPickWanted = GDX_MPP_NONE;
    sPickDeadlineMs = 0;
    sCourseDeadlineMs = 0;
    gdx3ds_p2p_leave();
    EndUds();
    HoldSleep(0);
    SetMessage("room closed: %s", sRoomClosedWhy);
    MpoLog("[mpo] room closed after the race (%s): results stay, no network", sRoomClosedWhy);
}

static int RejoinAllowed(int code, uint32_t now) {
    return code == GDX_RV_E_AUTH && sLb.mySlot != 0 && sRejoinKind != MPO_REJOIN_NONE &&
           (sRejoinAtMs == 0 || (int32_t) (now - sRejoinAtMs) >= MPO_REJOIN_SPACING_MS);
}

static void RejoinLan(void) {
    if (!CanEnterRoom()) {
        return;
    }
    sLbInit = 0;
    gdx3ds_p2p_lan_join(&sRejoinHost, sRejoinCode);
    SetPhase(GDX_MPO_CONNECTING);
}

/* The server (or LAN host) forgot this console while it was away (HOME, sleep, a long WiFi gap):
 * back into the same room at once, the way it came in. */
static void RejoinRoom(uint32_t now) {
    const int kind = sRejoinKind;
    char code[GDX_MPO_CODE_LEN + 1];
    snprintf(code, sizeof(code), "%s", sRejoinCode);
    sRejoinAtMs = now != 0 ? now : 1;
    LeaveRoom(NULL);
    MpoLog("[mpo] membership expired: rejoining room %s (kind %d)", code, kind);
    if (kind == MPO_REJOIN_ONLINE) {
        gdx3ds_mpo_ui_join(code);
    } else if (kind == MPO_REJOIN_DIRECT) {
        gdx3ds_mpo_ui_direct_join(code);
    } else {
        RejoinLan();
    }
    SetMessage(sPhase == GDX_MPO_CONNECTING ? "this console was away too long: rejoining the room"
                                            : "room closed: this console was away too long");
}

/* 1 = the room (and any race) ended here; 0 = the race goes on without the room. */
static int ServiceRoomLost(const GdxP2pStatus* st, uint32_t now) {
    const int code = RoomLostCode(st);
    const char* text = RoomLostText(st);
    if (sRoomLostInRace || ((gdx3ds_mpo_in_session() || sPhase == GDX_MPO_LINGER) && RaceCanGoOnAlone(st, now))) {
        if (!sRoomLostInRace) {
            sRoomLostInRace = 1;
            snprintf(sRoomClosedWhy, sizeof(sRoomClosedWhy), "%s", text);
            SetMessage("%s: the race goes on", text);
            MpoLog("[mpo] room lost mid-race (code %d: %s): the race goes on over %s", code, text,
                   RelayOutlivesRoom(st) ? "the links it has (the server still relays)" : "direct links");
        }
        if (sPhase == GDX_MPO_ROOM) {
            CloseRoomAfterRace();
            return 1;
        }
        return 0;
    }
    if (gdx3ds_mpo_in_session()) {
        AbortSession(GDX_LB_ABORT_PEER_LOST, -1, "the server dropped us");
    }
    if (RejoinAllowed(code, now)) {
        RejoinRoom(now);
        return 1;
    }
    LeaveRoom(NULL);
    SetMessage("room closed: %s", text);
    return 1;
}

/* mp-menus (user decision on edge #2): a console that comes into the room (or back) while the
 * room races is not in that race on any transport -- the host's START names its racers and their
 * roster epochs -- and is told so: it waits, and is in the room from the race's results on, where
 * the host's automatic round gives it its pick. */
static void ServiceRaceWait(void) {
    const int racing = sPhase == GDX_MPO_ROOM && sLbInit && sLb.mySlot != 0 && gdx_lobby_host_racing(&sLb);
    if (racing == sRaceWait) {
        return;
    }
    sRaceWait = racing;
    if (racing) {
        SetMessage("race in progress: you join when it ends");
        MpoLog("[mpo] the room is racing: this console waits for the race to end (no mid-race join)");
    } else {
        MpoLog("[mpo] the room's race is over: this console is in the room");
    }
}

/* ---- the network service: every frame, and while a race tick waits ------------------------- */
static void Service(uint32_t now) {
    const GdxP2pStatus* st;
    if (!sSocketOpen || sVanished) {
        return;
    }
    gdx3ds_p2p_service(now);
    Drain(now);
    now = NowMs(); /* the drain may have started a launch or sent with a newer clock */
    st = gdx3ds_p2p_status();
    if (sPhase == GDX_MPO_CONNECTING && sUdsJoinPending) {
        return; /* the radio is still associating (ServiceUdsJoin) */
    }
    if (sPhase == GDX_MPO_CONNECTING) {
        if (st->phase == GDX_P2P_IN_ROOM && st->haveRoster) {
            gdx_lobby_init(&sLb, st->mySlot, st->roomSecret, sBuild, sContentHash, sCfg.name);
            sLb.simAbi = sSimAbi;
            gdx3ds_mpg_current_picks(&sLb.machine, &sLb.skin, &sLb.engine);
            sLb.bot = sCfg.bot;
            sLb.model = (uint8_t) sModel;
            sLb.o3dsExtraDelay = sCfg.o3dsExtraDelay;
            sLb.ready = (sCfg.autoMode[0] != '\0') || sCfg.bot;
            sLb.engineBits = gdx_mpo_engine_bits((float) sLb.engine / 100.0f);
            SeedFromLastPick();
            ResetPicks();
            ResetPoints();
            sLbInit = 1;
            sLbRosterSeen = 0;
            HoldSleep(1);
            SetPhase(GDX_MPO_ROOM);
            if (st->mode == GDX_P2P_MODE_ONLINE) {
                SetMessage("%s room %s (slot %d)", st->mySlot == 0 ? "hosting" : "joined", st->code, st->mySlot);
            } else if (sUdsInRoom) {
                SetMessage("%s a local wireless room (slot %d)", st->mySlot == 0 ? "hosting" : "joined", st->mySlot);
            } else {
                SetMessage("%s a room on this WiFi (slot %d)", st->mySlot == 0 ? "hosting" : "joined", st->mySlot);
            }
            if (st->mode == GDX_P2P_MODE_LAN_HOST && !sUdsInRoom && sCfg.directHost) {
                const GdxMpEndpoint me = gdx3ds_p2p_local_endpoint();
                MpoLog("[mpo] direct host: room %s at %u.%u.%u.%u:%u (joiners: [net] mp_direct = this address; "
                       "over the internet, forward UDP %u to it and give out the public address)",
                       st->code, me.ip[0], me.ip[1], me.ip[2], me.ip[3], me.port, me.port);
            }
        } else if (st->phase == GDX_P2P_FAILED) {
            const char* text = st->errorCode > 0 ? gdx_rv_error_text(st->errorCode) : st->errorDetail;
            const int autoJoin = strcmp(sCfg.autoMode, "join") == 0 || strcmp(sCfg.autoMode, "djoin") == 0;
            if ((st->errorCode == GDX_RV_E_ROOM_NOT_FOUND || (st->errorCode < 0 && st->mode == GDX_P2P_MODE_LAN_JOIN)) &&
                autoJoin && sAutoJoinRetries < MPO_AUTOJOIN_RETRIES) {
                /* Test lever: the host may still be booting. Spaced so the server's code-guessing
                 * throttle (10 misses a minute) never trips. */
                sAutoJoinRetries++;
                sAutoJoinAtMs = now + MPO_AUTOJOIN_EVERY_MS;
            }
            if (st->errorCode == GDX_P2P_E_UPDATE) {
                char m[128];
                gdx_rv_update_message(&st->update, st->clientProto, m, sizeof(m));
                SetMessage("%s", m);
            } else if (st->errorCode == GDX_RV_E_BUILD_MISMATCH || st->errorCode == GDX_RV_E_CONTENT_MISMATCH) {
                SetMismatchMessage(st);
            } else {
                SetMessage("%s", text);
            }
            gdx3ds_p2p_leave();
            EndUds();
            SetPhase(GDX_MPO_IDLE);
        }
        return;
    }
    if (sPhase < GDX_MPO_ROOM || sRoomClosed) {
        return; /* a closed room: its results stay up, nothing goes out */
    }
    if ((st->phase == GDX_P2P_FAILED || st->roomLost != 0) && ServiceRoomLost(st, now)) {
        return;
    }
    if (st->rosterChanges != sLbRosterSeen) {
        sLbRosterSeen = st->rosterChanges;
        gdx_lobby_set_roster(&sLb, &st->roster);
        DropRacersThatLeft();
        ClearPointsOfEmptySlots();
    }
    ServiceRaceWait();
    {
        const int32_t helloEvery = (sPhase == GDX_MPO_ROOM) ? MPO_HELLO_LOBBY_MS : MPO_HELLO_RACE_MS;
        if (sLastHelloMs == 0 || (int32_t) (now - sLastHelloMs) >= helloEvery) {
            SendHellos(now);
        }
    }
    if (sLb.mySlot == 0 && gdx3ds_mpo_in_session() && !gdx_lobby_start_acked(&sLb) &&
        (int32_t) (now - sLastStartSendMs) >= MPO_START_RESEND_MS) {
        SendStarts(now);
    }
    ServiceRemoval(now);
    if (sAbortSlots != 0 && (int32_t) (sAbortUntilMs - now) > 0 &&
        (int32_t) (now - sLastAbortSendMs) >= MPO_ABORT_RESEND_MS) {
        SendAborts(now);
    }
    if (sPhase == GDX_MPO_LINGER) {
        SendOurPads(now); /* a peer may still lack our last ticks */
        if ((int32_t) (now - sLingerUntilMs) >= 0) {
            BackToLobby();
        }
        return;
    }
    if (sPhase == GDX_MPO_ROOM && sLb.mySlot == 0 && sCfg.autostart >= 2 && sRacesStarted < sCfg.races &&
        gdx3ds_mpg_stage() == GDX_MPG_IDLE) {
        const char* why = "";
        const int startable = gdx_lobby_host_startable(&sLb, now, &why);
        /* Test lever: start only once the room has been startable, unchanged, for a while -- an
         * emulator instance that crashes seconds after joining (and is relaunched by the driver)
         * must not be in the START. */
        if (startable != sAutoStableCount || sLbRosterSeen != sAutoStableRoster) {
            sAutoStableCount = startable;
            sAutoStableRoster = sLbRosterSeen;
            sAutoStableSinceMs = now;
        }
        if (startable >= sCfg.autostart && (int32_t) (now - sAutoStableSinceMs) >= MPO_AUTOSTART_STABLE_MS) {
            HostStart();
        }
    }
    if (sPhase == GDX_MPO_LAUNCH && (int32_t) (now - sLaunchMs) > sCfg.launchMs) {
        SetMessage("the race did not load in time");
        AbortSession(GDX_LB_ABORT_LAUNCH_FAILED, sLs != NULL ? sLs->localPlayer : -1, "launch timeout");
    }
}

/* ---- the poll hook ------------------------------------------------------------------------- */
/* racer.c (decomp-3ds-mp-ekhold.patch): this console's own BGM load is done. Weak: a harness
 * links the net library without the game. */
extern int gdx_mp_local_bgm_ready(void) __attribute__((weak));

/* The picks digest's field: every racer the race runs (the humans and, MP30, the CPU fill). */
static int PicksField(int numPlayers) {
    return gTotalRacers > numPlayers ? gTotalRacers : numPlayers;
}

/* What the race runs with, as the game holds it at tick 0 (the emulator gates compare it across
 * consoles): the START's picks and the digest of the racers' pinned machines. */
static void PinnedPicksReceipt(void) {
    const GdxMpLbStart* st = &sLb.start;
    char line[200];
    int off = 0;
    int i;
    line[0] = '\0';
    for (i = 0; i < st->numPlayers && off < (int) sizeof(line) - 40; i++) {
        off += snprintf(line + off, sizeof(line) - (size_t) off, " p%d=m%d/s%d/e%08lx", i, st->player[i].machine,
                        st->player[i].skin, (unsigned long) st->player[i].engineBits);
    }
    MpoLog("[mpo] pinned picks: course=%d game_course=%d key=%08lx%s field=%d racers=%08lx", st->course,
           gCourseIndex, (unsigned long) st->courseKey, line, PicksField(st->numPlayers),
           (unsigned long) gdx3ds_mpp_picks_hash(PicksField(st->numPlayers)));
}

#define MPO_PAD_A 0x8000u     /* N64 bits, as the game reads the pads */
#define MPO_PAD_START 0x1000u

/* The pause menu waits on THIS console's choice and its highlighted row ends the race. */
static int PauseGateOn(const GdxMpgPause* pz) {
    return pz->paused && pz->ready && pz->endsRace && sLs != NULL && pz->player == sLs->localPlayer;
}

/* "[mpo] pause: QUIT held 1500 ms: confirmed" / "... let go after 300 ms: nothing" */
static void PauseHoldReceipt(uint16_t heldBefore, uint32_t since, const GdxMpgPause* pz, uint16_t out, uint32_t now) {
    if (heldBefore == 0 && sPauseGate.holdMask != 0) {
        MpoLog("[mpo] pause: %s held (hold_confirm_ms=%d)", pz->optionName != NULL ? pz->optionName : "?", sCfg.holdMs);
    } else if (heldBefore != 0 && sPauseGate.holdMask == 0) {
        MpoLog("[mpo] pause: %s %s after %lu ms", pz->optionName != NULL ? pz->optionName : "?",
               (out & heldBefore) ? "confirmed" : "let go: nothing", (unsigned long) (now - since));
    }
}

/* mp-menus #10: the pauser's A / START on a row that ends the race goes into the lockstep only
 * once held [ui] hold_confirm_ms (gdx_mp_pause.h); a held bottom-screen pause button is an A. */
static uint16_t PauseGatedButtons(uint16_t buttons) {
    GdxMpgPause pz;
    const uint16_t heldBefore = sPauseGate.holdMask;
    const uint32_t since = sPauseGate.sinceMs;
    const uint32_t now = NowMs();
    uint16_t out;
    gdx3ds_mpg_pause(&pz);
    if (sPauseTouch && pz.paused && sLs != NULL && pz.player == sLs->localPlayer) {
        buttons |= MPO_PAD_A;
    }
    out = gdx_mp_pause_gate(&sPauseGate, buttons, MPO_PAD_A | MPO_PAD_START, PauseGateOn(&pz), pz.option, now,
                            sCfg.holdMs);
    PauseHoldReceipt(heldBefore, since, &pz, out, now);
    return out;
}

static GdxMpPad LocalPad(unsigned short buttons, signed char stickX, signed char stickY, uint32_t lt) {
    GdxMpPad pad;
    pad.buttons = (uint16_t) (PauseGatedButtons((uint16_t) buttons) & ~GDX_MP_PAD_BGM_READY);
    pad.stickX = stickX;
    pad.stickY = stickY;
    if (sCfg.bot) {
        /* A jittered pad that differs per console: the bot blends it in (decomp-3ds-mp-online.patch),
         * so a wrong pad anywhere shows up in the digest. Keeps START/A/B untouched. */
        if ((lt % 30u) == 0) {
            sBotLcg = sBotLcg * 1664525u + 1013904223u;
            sBotStickX = (int) ((sBotLcg >> 16) % 161u) - 80;
            sBotButtons = (uint16_t) (((sBotLcg >> 8) & 3u) == 0 ? 0x0010u : 0u); /* R now and then */
            if (sCfg.bot >= 2) {
                /* ram mode: R, Z or both (lean, side attack, spin attack) two times in three */
                static const uint16_t kRam[3] = { 0x0010u, 0x2000u, 0x2010u };
                const uint32_t r = (sBotLcg >> 8) % 9u;
                sBotButtons = (uint16_t) (r < 6u ? kRam[r % 3u] : 0u);
            }
        }
        pad.stickX = (signed char) sBotStickX;
        pad.buttons = (uint16_t) ((pad.buttons & ~0x2010u) | sBotButtons);
    }
    /* MP30: the EK countdown hold's input (gdx3ds_autodrive.c gdx_mp_ek_bgm_hold). */
    if ((&gdx_mp_local_bgm_ready == 0 || gdx_mp_local_bgm_ready()) && lt >= (uint32_t) sCfg.bgmHoldUntil) {
        pad.buttons |= GDX_MP_PAD_BGM_READY;
    }
    return pad;
}

/* A removed player's machine retires (the game's own Racer_RetireRacer) at the first race tick
 * >= its removal tick T, before that tick's game update: every console runs this at the same lt
 * with the same state, so it is as deterministic as a pad. Until it can (not on track yet), the
 * machine coasts on its neutral pad. */
extern int gdx_mp_retire_player(int player);

static void RetireRemoved(uint32_t lt) {
    int p;
    if (sCfg.dropMode != MPO_DROP_RETIRE) {
        return; /* cpu: the CPU driver takes the machine over (gdx3ds_mpo_drop_cpu_racer); coast */
    }
    for (p = 0; p < sLs->numPlayers; p++) {
        if (((sRetiredMask >> p) & 1u) || !gdx_mp_ls_is_removed(sLs, p, lt)) {
            continue;
        }
        if (gdx_mp_retire_player(p)) {
            sRetiredMask |= (uint8_t) (1u << p);
            MpoLog("[mpo] p%d retired at tick %lu", p, (unsigned long) lt);
        }
    }
}

static void Neutral(unsigned short* buttons, signed char* stickX, signed char* stickY, unsigned char* connected,
                    int capacity, int ports) {
    int p;
    for (p = 0; p < capacity; p++) {
        buttons[p] = 0;
        stickX[p] = 0;
        stickY[p] = 0;
        connected[p] = (unsigned char) (p < ports ? 1 : 0);
    }
}

/* ---- stall episodes: what a stuttering race shows and logs ----------------------------------
 * Hardware online1 (Old 3DS guest, a ~15 s WiFi dropout at tick ~7530): the race crawled a tick
 * every few seconds, so each tick's wait started the banner's clock at 0 again and it blinked on
 * and off; and it said "CPU AFTER 3 S" for 10+ s because a trickle of datagrams kept the host
 * from ever being silent for mp_drop_ms. Now waits less than MPO_STALL_EPISODE_GAP_MS apart are
 * one episode while each of them is long (MPO_STALL_LONG_MS, the banner's own threshold) and
 * they are less than MPO_STALL_EPISODE_GAP_MS apart: the banner counts from the episode's first
 * wait, names who it waits for, and shows the CPU line only while that player is really silent
 * (with the time left, counted from the last datagram heard). A short wait ends an episode (the
 * link carries pads again). An episode of MPO_STALL_EPISODE_LOG_MS or more leaves a receipt. */
#define MPO_STALL_LONG_MS 500
#define MPO_STALL_EPISODE_GAP_MS 1500
#define MPO_STALL_EPISODE_LOG_MS 3000
#define MPO_STALL_SILENT_MS 1000
static int sEpisodeOn = 0;
static uint32_t sEpisodeStartMs = 0;
static uint32_t sEpisodeLastEndMs = 0;
static uint32_t sEpisodeFirstTick = 0;
static uint32_t sEpisodeWaits = 0;
static uint32_t sEpisodeWaitedMs = 0;
static int sEpisodeSlot = -1;

static void StallEpisodeClose(uint32_t lt) {
    const uint32_t span = sEpisodeLastEndMs - sEpisodeStartMs;
    if (!sEpisodeOn) {
        return;
    }
    sEpisodeOn = 0;
    if (span >= MPO_STALL_EPISODE_LOG_MS) {
        MpoLog("[mpo] stall episode over at tick %lu: %lu ms from tick %lu (%lu waits, %lu ms waiting) for s%d (%s); "
               "the race runs on",
               (unsigned long) lt, (unsigned long) span, (unsigned long) sEpisodeFirstTick,
               (unsigned long) sEpisodeWaits, (unsigned long) sEpisodeWaitedMs, sEpisodeSlot,
               (sEpisodeSlot >= 0 && sEpisodeSlot < GDX_MPO_MAX_SLOTS) ? sSessionName[sEpisodeSlot] : "?");
    }
}

/* A tick has to wait for a peer: the episode it belongs to (a new one after a long enough run). */
static void StallEpisodeBegin(uint32_t lt, uint32_t now) {
    if (sEpisodeOn && lt < sEpisodeFirstTick) {
        sEpisodeOn = 0; /* a new race */
    }
    if (sEpisodeOn && (uint32_t) (now - sEpisodeLastEndMs) > MPO_STALL_EPISODE_GAP_MS) {
        StallEpisodeClose(lt);
    }
    if (!sEpisodeOn) {
        sEpisodeOn = 1;
        sEpisodeStartMs = now;
        sEpisodeFirstTick = lt;
        sEpisodeWaits = 0;
        sEpisodeWaitedMs = 0;
    }
    sEpisodeSlot = gdx_lobby_player_slot(&sLb, gdx_mp_ls_first_missing(sLs, lt));
}

static void StallEpisodeEnd(uint32_t lt, uint32_t now, uint32_t waitedMs) {
    sEpisodeWaits++;
    sEpisodeWaitedMs += waitedMs;
    if (waitedMs >= MPO_STALL_LONG_MS) {
        sEpisodeLastEndMs = now;
    } else if (sEpisodeWaits == 1) {
        sEpisodeOn = 0; /* one short wait: no episode */
    } else {
        StallEpisodeClose(lt);
    }
}

/* A tick that did not wait: an episode whose last wait is long enough ago is over. */
static void StallEpisodeIdle(uint32_t lt, uint32_t now) {
    if (sEpisodeOn && (uint32_t) (now - sEpisodeLastEndMs) > MPO_STALL_EPISODE_GAP_MS) {
        StallEpisodeClose(lt);
    }
}

/* The CPU takeover's time left for `slot` while it is silent, else -1 (it is heard: no takeover
 * is coming, however long the stall). */
static int StallCpuAfterMs(int slot, uint32_t now) {
    const uint32_t silent = now - sLastHeardMs[slot];
    int left;
    if (sCfg.dropMode != MPO_DROP_CPU || silent < MPO_STALL_SILENT_MS) {
        return -1;
    }
    left = SilenceLimitMs(slot) - (int) silent;
    return left > 0 ? left : 0;
}

/* Another racer of this session goes by the same name (case-insensitive)? */
static int SessionNameShared(int slot) {
    int s;
    for (s = 0; s < GDX_MPO_MAX_SLOTS; s++) {
        if (s != slot && sSessionName[s][0] != '\0' && strcasecmp(sSessionName[s], sSessionName[slot]) == 0) {
            return 1;
        }
    }
    return 0;
}

/* Who the banner names: the player's name, "P<n> <name>" when two racers share it (hardware
 * online1: both consoles were "mike", so "waiting for mike" said nothing), "P<n>" without one. */
static const char* StallDisplayName(int slot, int player) {
    static char text[GDX_MPO_NAME_LEN + 8];
    if (slot < 0 || slot >= GDX_MPO_MAX_SLOTS || player < 0) {
        return "A PLAYER";
    }
    if (sSessionName[slot][0] == '\0') {
        snprintf(text, sizeof(text), "P%d", player + 1);
    } else if (SessionNameShared(slot)) {
        snprintf(text, sizeof(text), "P%d %s", player + 1, sSessionName[slot]);
    } else {
        return sSessionName[slot];
    }
    return text;
}

/* The bottom screen's stall banner (gdx3ds_bui_race_stall): from 0.5 s into an episode (this tick
 * waiting 0.1 s or more), about every 100 ms, naming the first player whose pad is missing. */
static void StallBanner(uint32_t lt, uint32_t now, uint32_t startMs) {
    const uint32_t waited = now - (sEpisodeOn ? sEpisodeStartMs : startMs);
    const char* name = "A PLAYER";
    int player;
    int slot;
    int dropAfterMs = -1;
    if (gdx3ds_bui_race_stall == NULL || waited < MPO_STALL_LONG_MS || now - startMs < 100u ||
        (sStallShown && now - sStallNoteMs < 100u)) {
        return;
    }
    player = gdx_mp_ls_first_missing(sLs, lt);
    slot = gdx_lobby_player_slot(&sLb, player);
    if (slot >= 0 && slot < GDX_MPO_MAX_SLOTS) {
        name = StallDisplayName(slot, player);
        dropAfterMs = StallCpuAfterMs(slot, now);
    }
    sStallNoteMs = now;
    sStallShown = 1;
    gdx3ds_bui_race_stall(name, (int) waited, dropAfterMs);
}

static void StallBannerOff(void) {
    if (!sStallShown) {
        return;
    }
    sStallShown = 0;
    if (gdx3ds_bui_race_stall != NULL) {
        gdx3ds_bui_race_stall(NULL, 0, -1);
    }
}

/* While a console waits for a peer it repeats its packet: every 16 ms while a live peer has not
 * acked our newest pad (the copy may have been lost; the redundancy refills the gap), else only a
 * heartbeat every MPO_WAIT_HEARTBEAT_MS (carries our acks and RTT echo). Before, it was every 16
 * ms regardless: on a host rate-matching a slower Old 3DS that was ~60 extra frames a second for
 * the Old 3DS to pull over NWM IPC, each a copy it already had. Same packets, same lockstep;
 * only fewer duplicates.
 * [net] mp_wait_quiet_ms (default 500; 0 = off): while the player we wait for has sent nothing
 * for that long, only the heartbeat. Hardware online1: through a ~15 s WiFi dropout the Old 3DS
 * kept sending ~60 datagrams/s at 4 ms of SOC each; a keepalive's answer came back 6-15 s late
 * (srv_rtt=6452, 15431), i.e. the datagrams queued rather than vanished, and every extra copy
 * queued behind them. The first heartbeat that gets through carries every unacked pad. */
#define MPO_WAIT_RESEND_MS 16
#define MPO_WAIT_HEARTBEAT_MS 100
static int WaitPeerQuiet(uint32_t lt, uint32_t now) {
    const int slot = gdx_lobby_player_slot(&sLb, gdx_mp_ls_first_missing(sLs, lt));
    if (sCfg.waitQuietMs <= 0 || slot < 0 || slot >= GDX_MPO_MAX_SLOTS) {
        return 0;
    }
    return (int32_t) (now - sLastHeardMs[slot]) > sCfg.waitQuietMs;
}

/* Resend backoff through a long wait (research network.md 2.2): every 16 ms for the first 100 ms,
 * then every 50 ms; past 500 ms only the heartbeat. One packet carries the whole unacked window,
 * so once the link is down faster resends buy nothing and cost the Old 3DS 2-6 ms of core-1 SOC
 * time each (hardware online1: tx x5 in the dropout window). */
#define MPO_WAIT_BACKOFF1_MS 100
#define MPO_WAIT_BACKOFF2_MS 500
#define MPO_WAIT_RESEND2_MS 50

static uint32_t WaitResendEveryMs(uint32_t waitedMs) {
    if (waitedMs < MPO_WAIT_BACKOFF1_MS) {
        return MPO_WAIT_RESEND_MS;
    }
    if (waitedMs < MPO_WAIT_BACKOFF2_MS) {
        return MPO_WAIT_RESEND2_MS;
    }
    return MPO_WAIT_HEARTBEAT_MS;
}

static int WaitShouldResend(uint32_t now, uint32_t lastSendMs, uint32_t waitedMs, int peerQuiet) {
    const uint32_t since = now - lastSendMs;
    if (since >= MPO_WAIT_HEARTBEAT_MS) {
        return 1;
    }
    return !peerQuiet && since >= WaitResendEveryMs(waitedMs) && gdx_mp_ls_unacked(sLs);
}

/* How long the wait loop may sleep: until a datagram lands (the socket thread signals it) or the
 * next resend is due, at most MPO_WAIT_SLICE_MS (the banner and the limits run each pass). */
#define MPO_WAIT_SLICE_MS 8
static uint32_t WaitSliceMs(uint32_t now, uint32_t lastSendMs, uint32_t waitedMs) {
    const uint32_t every = WaitResendEveryMs(waitedMs);
    const uint32_t since = now - lastSendMs;
    const uint32_t left = since >= every ? 1u : every - since;
    return left < MPO_WAIT_SLICE_MS ? left : MPO_WAIT_SLICE_MS;
}

/* Pumps until every pad of `lt` is in. 0 = ready, -1 = the session ended while we waited. */
static int WaitReady(uint32_t lt) {
    const u64 t0 = svcGetSystemTick();
    const uint32_t startMs = NowMs();
    const int limitMs = sAnyRemotePad ? sCfg.waitMs : sCfg.launchMs;
    uint32_t lastSendMs = startMs;
    uint32_t lastNoteMs = startMs;
    uint32_t waitedUs;
    u64 tWait = 0;
    if (!(sCfg.readySkip && !sUdsInRoom && sPhase == GDX_MPO_RACE && sLs != NULL && gdx_mp_ls_ready(sLs, lt))) {
        Service(startMs); /* (mp_ready_skip: a socket room whose tick is ready skips this drain) */
    }
    if (sPhase == GDX_MPO_RACE && sLs != NULL && !gdx_mp_ls_ready(sLs, lt)) {
        tWait = svcGetSystemTick(); /* NETTICK: from here on it is waiting for a peer, not our CPU */
        StallEpisodeBegin(lt, NowMs());
    } else {
        StallEpisodeIdle(lt, NowMs());
    }
    while (sPhase == GDX_MPO_RACE && sLs != NULL && !gdx_mp_ls_ready(sLs, lt)) {
        const uint32_t now = NowMs();
        if ((int) (now - startMs) > limitMs) {
            const int missing = gdx_mp_ls_first_missing(sLs, lt);
            char why[96];
            snprintf(why, sizeof(why), "no pad from p%d for tick %lu in %d ms", missing, (unsigned long) lt, limitMs);
            SetMessage("race ended: %s connection lost", sLb.peer[gdx_lobby_player_slot(&sLb, missing) & 3].name);
            StallBannerOff();
            AbortSession(GDX_LB_ABORT_PEER_LOST, missing, why);
            return -1;
        }
        if (WaitShouldResend(now, lastSendMs, now - startMs, WaitPeerQuiet(lt, now))) {
            SendOurPads(now);
            lastSendMs = now;
        }
        if (now - lastNoteMs >= 5000) {
            char st[200];
            gdx3ds_mp_p2p_ops()->selftest(st, sizeof(st));
            MpoLog("[mpo] waiting tick=%lu for p%d (%lu ms) %s", (unsigned long) lt, gdx_mp_ls_first_missing(sLs, lt),
                   (unsigned long) (now - startMs), st);
            lastNoteMs = now;
        }
        StallBanner(lt, now, startMs);
        gdx3ds_p2p_wait_rx(WaitSliceMs(now, lastSendMs, now - startMs));
        Service(NowMs());
    }
    StallBannerOff();
    if (tWait != 0) {
        const u64 w = svcGetSystemTick() - tWait;
        StallEpisodeEnd(lt, NowMs(), (uint32_t) MPO_TICK_US(w) / 1000u);
        sCost.waitTicks += w;
        if (gdx3ds_frameskip_note_net_wait != NULL) {
            gdx3ds_frameskip_note_net_wait((uint64_t) w); /* not this console's lateness */
        }
    }
    if (sPhase != GDX_MPO_RACE || sLs == NULL) {
        return -1;
    }
    waitedUs = (uint32_t) MPO_TICK_US(svcGetSystemTick() - t0);
    sStallUs += waitedUs;
    if (waitedUs > 200) {
        sStallTicks++;
    }
    if (waitedUs > sStallMaxUs) {
        sStallMaxUs = waitedUs;
    }
    return 0;
}

static const char* PlayerName(int player) {
    if (player < 0 || player >= sLb.start.numPlayers || player >= GDX_MPO_MAX_SLOTS ||
        sLb.start.player[player].slot >= GDX_MPO_MAX_SLOTS) {
        return "a player";
    }
    return sSessionName[sLb.start.player[player].slot];
}

/* "[mpo] pause by p1 (MIKE) at tick 900" / "[mpo] resume at tick 1300": the same ticks on every
 * console (the gates compare them). */
static void PauseReceipt(uint32_t lt) {
    GdxMpgPause pz;
    const int now = gdx3ds_mpg_pause(&pz) ? pz.player : -1;
    if (now == sPauseShown) {
        return;
    }
    if (now >= 0) {
        MpoLog("[mpo] pause by p%d (%s) at tick %lu%s", now, PlayerName(now), (unsigned long) lt,
               (sLs != NULL && now == sLs->localPlayer) ? " (this console: its pad drives the menu)" : "");
    } else {
        MpoLog("[mpo] resume at tick %lu", (unsigned long) lt);
    }
    sPauseShown = now;
}

static int Exchange(unsigned long long tick, unsigned short* buttons, signed char* stickX, signed char* stickY,
                    unsigned char* connected, int capacity);

int gdx3ds_mpo_exchange(unsigned long long tick, unsigned short* buttons, signed char* stickX, signed char* stickY,
                        unsigned char* connected, int capacity) {
    const uint64_t t0 = svcGetSystemTick();
    const int rc = Exchange(tick, buttons, stickX, stickY, connected, capacity);
    const uint32_t dt = (uint32_t) (svcGetSystemTick() - t0);
    sCost.exchTicks += dt;
    sCost.frameTicks += dt;
    return rc;
}

static int Exchange(unsigned long long tick, unsigned short* buttons, signed char* stickX, signed char* stickY,
                    unsigned char* connected, int capacity) {
    GdxMpPad pads[GDX_MP_MAX_PLAYERS];
    const int ports = gdx3ds_mpg_ports();
    int p;
    uint32_t lt;
    uint32_t stallsBefore;
    if (sPhase == GDX_MPO_LAUNCH) {
        if (!gdx3ds_mpg_launch_poll()) {
            Neutral(buttons, stickX, stickY, connected, capacity, ports);
            return 1;
        }
        sBaseTick = tick;
        SetPhase(GDX_MPO_RACE);
        sLb.state = GDX_LB_STATE_RACING;
        MpoLog("[mpo] race tick 0 = input tick %llu (%lu ms after START)", tick,
               (unsigned long) (NowMs() - sLaunchMs));
        PinnedPicksReceipt();
        if (sLs != NULL) {
            gdx3ds_mpts_race_begin(sLs); /* mp-timesync: [net] mp_timesync */
        }
    }
    if (sPhase != GDX_MPO_RACE || sLs == NULL) {
        return 0;
    }
    lt = (uint32_t) (tick - sBaseTick);
    if (gdx3ds_mpg_race_poll(lt)) {
        EndRace();
        sHaveTick = 0;
        return 0;
    }
    sLastLt = lt;
    EndLaunchSyncInRace(lt);
    PauseReceipt(lt);
    if (sCfg.leaveAt >= 0 && lt == (uint32_t) sCfg.leaveAt) {
        /* Test lever: walk out mid-race (LEAVE to the server + ABORT to the racers). */
        MpoLog("[mpo] test lever: leaving the room at lt=%lu", (unsigned long) lt);
        gdx3ds_mpo_ui_leave();
        return 0;
    }
    if (sCfg.suspendAt >= 0 && lt == (uint32_t) sCfg.suspendAt) {
        /* Test lever: what the APT hook does on HOME / sleep (gdx3ds_mpo_on_suspend). */
        MpoLog("[mpo] test lever: suspend (HOME) at lt=%lu", (unsigned long) lt);
        gdx3ds_mpo_on_suspend();
        return 0;
    }
    if (sCfg.vanishAt >= 0 && lt == (uint32_t) sCfg.vanishAt && !sVanished) {
        /* Test lever: go dark (power cut): never send or read anything again. */
        MpoLog("[mpo] test lever: vanishing at lt=%lu", (unsigned long) lt);
        gdx3ds_filelog_flush();
        sVanished = 1;
    }
    gdx_mp_ls_push_local(sLs, lt, LocalPad(buttons[0], stickX[0], stickY[0], lt));
    SendOurPads(NowMs());
    gdx3ds_mpts_pace(sLs, lt); /* mp-timesync: the console that is ahead stretches this tick */
    stallsBefore = sStallTicks;
    if (WaitReady(lt) != 0) {
        sHaveTick = 0;
        Neutral(buttons, stickX, stickY, connected, capacity, ports);
        return 1;
    }
    gdx3ds_mpts_ran(sStallTicks != stallsBefore);
    gdx_mp_ls_get(sLs, lt, pads);
    sRanTicks = lt + 1u;
    sSimLt = lt;
    RetireRemoved(lt);
    sBgmReadyAll = 1;
    for (p = 0; p < sLs->numPlayers; p++) {
        if (!(pads[p].buttons & GDX_MP_PAD_BGM_READY) && !gdx_mp_ls_is_removed(sLs, p, lt)) {
            sBgmReadyAll = 0;
        }
    }
    for (p = 0; p < capacity; p++) {
        if (p < sLs->numPlayers) {
            connected[p] = 1;
            buttons[p] = (unsigned short) (pads[p].buttons & ~GDX_MP_PAD_BGM_READY);
            stickX[p] = pads[p].stickX;
            stickY[p] = pads[p].stickY;
        } else {
            connected[p] = 0;
            buttons[p] = 0;
            stickX[p] = 0;
            stickY[p] = 0;
        }
    }
    sHaveTick = 1;
    return 1;
}

static uint32_t Fnv(uint32_t h, const void* data, size_t len) {
    const uint8_t* q = (const uint8_t*) data;
    size_t i;
    for (i = 0; i < len; i++) {
        h ^= q[i];
        h *= 16777619u;
    }
    return h;
}

static void StatsReceipt(void) {
    char peers[200];
    int off = 0;
    int p;
    peers[0] = '\0';
    for (p = 0; p < sLs->numPlayers && off < (int) sizeof(peers) - 48; p++) {
        const int slot = gdx_lobby_player_slot(&sLb, p);
        const GdxMpLink* l = gdx3ds_p2p_link(slot);
        if (p == sLs->localPlayer || l == NULL) {
            continue;
        }
        off += snprintf(peers + off, sizeof(peers) - (size_t) off, " p%d:%s rtt=%lu/%lu hc=%lu", p,
                        gdx_link_state_name(l->state), (unsigned long) sLs->peer[p].rttLastMs,
                        (unsigned long) sLs->peer[p].rttMaxMs, (unsigned long) sLs->peer[p].hashesChecked);
    }
    {
        char tx[200];
        gdx3ds_mp_p2p_ops()->selftest(tx, sizeof(tx)); /* the transport's counters (UDS: tx, qfull) */
        MpoLog("[mpo] stats lt=%lu stalls=%lu stall_max_ms=%lu stall_ms=%lu delay=%d rej=%lu/%lu%s | %s",
               (unsigned long) sLastLt, (unsigned long) sStallTicks, (unsigned long) (sStallMaxUs / 1000u),
               (unsigned long) (sStallUs / 1000u), sLs->delay, (unsigned long) sLs->rejected,
               (unsigned long) sRejected, peers, tx);
    }
}

/* [net] mp_hash_log = N (diagnostic): the first N race ticks' digest, RNG and per-racer hashes,
 * one line each, so two consoles' logs can be diffed for the first racer that diverged. */
#define MPO_HASH_RING 32
#define MPO_HASH_RING_RACERS 4
#define MPO_HASH_RING_LINE 192
#define MPO_HASH_RING_RAW 2048 /* >= sizeof(Racer) */
typedef struct MpoHashRing {
    uint32_t lt[MPO_HASH_RING];
    char line[MPO_HASH_RING][MPO_HASH_RING_RACERS][MPO_HASH_RING_LINE];
    int rawLen[MPO_HASH_RING][MPO_HASH_RING_RACERS];
    uint32_t raw[MPO_HASH_RING][MPO_HASH_RING_RACERS][MPO_HASH_RING_RAW / 4];
} MpoHashRing;
static MpoHashRing* sHashRing = NULL; /* mp_hash_log only: the last ticks' racer fields */

static void HashRingStore(int racers) {
    const int slot = (int) (sLastLt % MPO_HASH_RING);
    int i;
    if (sHashRing == NULL) {
        sHashRing = (MpoHashRing*) calloc(1, sizeof(MpoHashRing));
        if (sHashRing == NULL) {
            return;
        }
    }
    sHashRing->lt[slot] = sLastLt + 1u; /* 0 = empty */
    for (i = 0; i < MPO_HASH_RING_RACERS; i++) {
        sHashRing->line[slot][i][0] = '\0';
        sHashRing->rawLen[slot][i] = 0;
        if (i < racers) {
            gdx_mpo_racer_describe(i, sHashRing->line[slot][i], MPO_HASH_RING_LINE);
            sHashRing->rawLen[slot][i] = gdx_mpo_racer_raw(i, sHashRing->raw[slot][i], MPO_HASH_RING_RAW);
        }
    }
}

/* A desync abort (detected here or heard): the last MPO_HASH_RING ticks' racer fields, so two
 * consoles' logs show which field of which racer moved first. */
static void HashRingDump(void) {
    uint32_t t;
    int i;
    if (sHashRing == NULL) {
        return;
    }
    for (t = (sLastLt >= MPO_HASH_RING ? sLastLt - MPO_HASH_RING + 1u : 0u); t <= sLastLt; t++) {
        const int slot = (int) (t % MPO_HASH_RING);
        if (sHashRing->lt[slot] != t + 1u) {
            continue;
        }
        for (i = 0; i < MPO_HASH_RING_RACERS; i++) {
            if (sHashRing->line[slot][i][0] != '\0') {
                MpoLog("[mpo] hash@%lu %s", (unsigned long) t, sHashRing->line[slot][i]);
            }
            if (t + 12u > sLastLt) { /* the raw racer words of the last ticks only (big) */
                int w;
                for (w = 0; w < sHashRing->rawLen[slot][i] / 4; w += 16) {
                    char hex[16 * 9 + 1];
                    int k;
                    int o = 0;
                    for (k = 0; k < 16 && w + k < sHashRing->rawLen[slot][i] / 4; k++) {
                        o += snprintf(hex + o, sizeof(hex) - (size_t) o, " %08lx",
                                      (unsigned long) sHashRing->raw[slot][i][w + k]);
                    }
                    MpoLog("[mpo] raw@%lu r%d +%04x%s", (unsigned long) t, i, w * 4, hex);
                }
            }
        }
    }
}

static void HashLogReceipt(unsigned int hash, int racers) {
    char line[200];
    int off;
    int i;
    HashRingStore(racers);
    off = snprintf(line, sizeof(line), "[mpo] hash lt=%lu h=%08x rng=%08x/%08x n=%d", (unsigned long) sLastLt, hash,
                   (unsigned) gRandSeed1, (unsigned) gRandSeed2, racers);
    for (i = 0; i < racers && i < 8 && off < (int) sizeof(line) - 12; i++) {
        off += snprintf(line + off, sizeof(line) - (size_t) off, " %08x", gdx_mpo_racer_hash(i));
    }
    MpoLog("%s", line);
    if (sLastLt < 2u || sLastLt + 64u >= (uint32_t) sCfg.hashLog) { /* field detail: the window's ends */
        for (i = 0; i < racers && i < 8; i++) {
            if (gdx_mpo_racer_describe(i, line, sizeof(line)) > 0) {
                MpoLog("[mpo] hash   %s", line);
            }
        }
    }
}

/* Test lever mp_auto=lanjoin / udsjoin: the first joinable room heard (rows sort joinable first;
 * udsjoin takes local-wireless rooms only). */
static void AutoJoinNearby(uint32_t now) {
    if (sLanAutoJoinMs != 0 && sPhase == GDX_MPO_IDLE && !sUdsJoinPending && (int32_t) (now - sLanAutoJoinMs) >= 0) {
        GdxMpNearbyRoom rows[GDX_LAN_MAX_NEARBY + GDX_MP_UDS_NEARBY_MAX];
        const int n = NearbyRows(rows, (int) (sizeof(rows) / sizeof(rows[0])));
        int i;
        sLanAutoJoinMs = now + 2000;
        for (i = 0; i < n; i++) {
            const GdxMpNearbyRoom* r = &rows[i];
            if (!r->joinable || (sLanAutoJoinUdsOnly && r->medium != GDX_NEARBY_MEDIUM_UDS)) {
                continue;
            }
            MpoLog("[mpo] auto-join nearby: %s's room %s via %s at %u.%u.%u.%u:%u ping=%d", r->b.name, r->b.code,
                   r->medium == GDX_NEARBY_MEDIUM_UDS ? "uds" : "wifi", r->room.ip[0], r->room.ip[1], r->room.ip[2],
                   r->room.ip[3], r->room.port, r->pingMs);
            gdx3ds_mpo_ui_lan_join(i);
            break;
        }
    }
}

/* The discovery socket's service period (0 = every frame). */
static int DiscPeriodMs(void) {
    if (sUdsInRoom && sCfg.udsDiscPollMs > 0) {
        return sCfg.udsDiscPollMs;
    }
    if (sPhase == GDX_MPO_RACE && sCfg.raceDiscMs > 0) {
        return sCfg.raceDiscMs;
    }
    return 0;
}

/* NEARBY: listen every frame; while we host a LAN room, keep its beacon current. */
static void ServiceNearby(uint32_t now) {
    const GdxP2pStatus* st;
    ServiceUdsScan(now);
    ServiceNearbyWait(now);
    ServiceUdsJoin(now);
    ServiceUdsHost();
    if (!sDiscOpen || sDisc == NULL) {
        if (UdsEnabled()) {
            AutoJoinNearby(now);
        }
        return;
    }
    st = gdx3ds_p2p_status();
    if (sCfg.onlineDiscOff && sPhase >= GDX_MPO_ROOM && st->mode == GDX_P2P_MODE_ONLINE) {
        /* mp-online-hw: an online-server room shows no NEARBY list and sends no beacon, so its
         * discovery socket is not asked every frame (an Old 3DS pays each SOC IPC on core 1). */
        gdx3ds_ipcstat_gated(GDX_IPC_DISC_RECV);
        return;
    }
    if (sPhase >= GDX_MPO_ROOM && sLbInit && st->mode == GDX_P2P_MODE_LAN_HOST && sLb.mySlot == 0 && !sUdsHosting) {
        GdxMpLanBeacon b;
        const GdxMpEndpoint me = gdx3ds_p2p_local_endpoint();
        memset(&b, 0, sizeof(b));
        b.hostId = sLanHostId;
        b.contentHash = sContentHash;
        b.roomPort = me.port;
        b.players = (uint8_t) ClampInt(gdx3ds_p2p_lan_room_count(), 1, GDX_MPO_MAX_SLOTS);
        b.maxPlayers = (uint8_t) st->maxPlayers;
        b.model = (uint8_t) sModel;
        b.flags = (uint8_t) ((sPhase != GDX_MPO_ROOM ? GDX_LAN_FLAG_RACING : 0) |
                             (b.players >= b.maxPlayers ? GDX_LAN_FLAG_FULL : 0));
        b.proto = GDX_MPO_PROTO;
        snprintf(b.code, sizeof(b.code), "%s", st->code);
        snprintf(b.name, sizeof(b.name), "%s", sCfg.name);
        snprintf(b.build, sizeof(b.build), "%s", sBuild);
        sDisc->announce(&b);
    } else {
        sDisc->announce(NULL);
    }
    if (DiscPeriodMs() > 0) {
        /* NETTICK: a local wireless room has no WiFi; poll the discovery socket rarely. In a WiFi
         * race too: the socket thread owns the room's socket, and this one (beacons, pings) is
         * the main thread's only SOC call left, so it runs once a [net] mp_race_disc_ms. */
        static uint32_t sDiscLastMs = 0;
        if ((int32_t) (now - sDiscLastMs) < DiscPeriodMs()) {
            gdx3ds_ipcstat_gated(GDX_IPC_DISC_RECV);
            AutoJoinNearby(now);
            return;
        }
        sDiscLastMs = now;
    }
    sDisc->service(now);
    AutoJoinNearby(now);
}

static void AfterTick(void);

void gdx3ds_mpo_after_tick(void) {
    uint64_t t0;
    if (sPhase == GDX_MPO_OFF) {
        return;
    }
    t0 = svcGetSystemTick();
    AfterTick();
    CostFrameEnd((uint32_t) (svcGetSystemTick() - t0));
}

/* ---- per-second race receipts ([net] mp_rate_ms, default 1000; 0 = off) ----------------------
 * "[mprate] lt= rx/s tx/s relay rx/tx stalls ms bars ndm srv_rtt": what one second of the race
 * moved, the WiFi bars (shared page, no IPC) and whether NDM exclusivity holds, so an outage can
 * be placed to the second and told apart from weak signal. "[mpo] route s<n>=..." whenever the
 * endpoint a peer's DIRECT datagrams go to changes (and at the race's first second). */
typedef struct RateWindow {
    uint32_t atMs;
    uint32_t rxDirect, rxRelay, txDirect, txRelay;
    uint32_t stalls, stallMs;
    char route[GDX_MPO_MAX_SLOTS][64];
} RateWindow;
static RateWindow sRate;

static void RateReset(void) {
    memset(&sRate, 0, sizeof(sRate));
}

static void RouteReceipts(void) {
    int s;
    for (s = 0; s < GDX_MPO_MAX_SLOTS; s++) {
        char text[64];
        gdx3ds_p2p_route_text(s, text, (int) sizeof(text));
        if (strcmp(text, sRate.route[s]) != 0) {
            snprintf(sRate.route[s], sizeof(sRate.route[s]), "%s", text);
            if (text[0] != '\0') {
                MpoLog("[mpo] route %s at tick %lu", text, (unsigned long) sLastLt);
            }
        }
    }
}

static void RateMark(uint32_t now, const GdxP2pStatus* st) {
    sRate.atMs = now;
    sRate.rxDirect = st->rxDirect;
    sRate.rxRelay = st->rxRelay;
    sRate.txDirect = st->txDirect;
    sRate.txRelay = st->txRelay;
    sRate.stalls = (uint32_t) sStallTicks;
    sRate.stallMs = (uint32_t) (sStallUs / 1000u);
}

static void RateReceipt(uint32_t now) {
    const GdxP2pStatus* st = gdx3ds_p2p_status();
    const uint32_t span = now - sRate.atMs;
    if (sCfg.rateMs <= 0 || st == NULL) {
        return;
    }
    if (sRate.atMs == 0 || span > 60000u) {
        RateMark(now, st); /* the race's first second (or after a long hold): a baseline */
        RouteReceipts();
        return;
    }
    if (span < (uint32_t) sCfg.rateMs) {
        return;
    }
    MpoLog("[mprate] lt=%lu ms=%lu rx=%lu/%lu tx=%lu/%lu stalls=%lu stall_ms=%lu bars=%d ndm=%d srv_rtt=%lu",
           (unsigned long) sLastLt, (unsigned long) span, (unsigned long) (st->rxDirect - sRate.rxDirect),
           (unsigned long) (st->rxRelay - sRate.rxRelay), (unsigned long) (st->txDirect - sRate.txDirect),
           (unsigned long) (st->txRelay - sRate.txRelay), (unsigned long) ((uint32_t) sStallTicks - sRate.stalls),
           (unsigned long) ((uint32_t) (sStallUs / 1000u) - sRate.stallMs), gdx3ds_mp_radio_bars(), gdx3ds_mp_radio_exclusive(),
           (unsigned long) st->serverRttMs);
    RateMark(now, st);
    RouteReceipts();
}

static void AfterTick(void) {
    ServiceSockets(NowMs());
    ServiceNearby(NowMs());
    if (sPhase == GDX_MPO_IDLE && sAutoJoinAtMs != 0 && (int32_t) (NowMs() - sAutoJoinAtMs) >= 0) {
        sAutoJoinAtMs = 0;
        MpoLog("[mpo] auto-join: retry %d of %d", sAutoJoinRetries, MPO_AUTOJOIN_RETRIES);
        if (strcmp(sCfg.autoMode, "djoin") == 0) {
            gdx3ds_mpo_ui_direct_join(sCfg.room);
        } else {
            gdx3ds_mpo_ui_join(sCfg.room);
        }
    }
    if (sPhase == GDX_MPO_RACE && sHaveTick && sLs != NULL) {
        unsigned int hash;
        int racers = 0;
        int mode;
        if (sCfg.desyncAt >= 0 && sLastLt == (uint32_t) sCfg.desyncAt) {
            gRandSeed1 ^= 1;
            MpoLog("[mpo] desync lever: flipped gRandSeed1 bit 0 after lt=%lu", (unsigned long) sLastLt);
        }
        hash = gdx_gstate_hash(&racers);
        hash = gdx_mpo_vis_hash(hash);
        if (sCfg.hashDeep) {
            /* Triage: every byte of every racer, so the desync fires at the first divergence of
             * ANY field (the stock digest covers the simulated few, which can lag it by seconds). */
            static uint32_t raw[2048 / 4];
            int i;
            for (i = 0; i < racers; i++) {
                const int n = gdx_mpo_racer_raw(i, raw, (int) sizeof(raw));
                hash = Fnv(hash, raw, (size_t) n);
            }
        }
        mode = gdx_input_game_mode();
        hash = Fnv(hash, &mode, sizeof(mode));
        {
            const uint32_t picks = gdx3ds_mpp_picks_hash(PicksField(sLs->numPlayers));
            hash = Fnv(hash, &picks, sizeof(picks)); /* the received picks, as the race pinned them */
        }
        gdx_mp_ls_note_local_hash(sLs, sLastLt, hash);
        if (sLastLt < (uint32_t) sCfg.hashLog) {
            HashLogReceipt(hash, racers);
        }
        if (((sLastLt + 1) % (uint32_t) sCfg.statsEvery) == 0) {
            StatsReceipt();
            gdx3ds_mpts_window(sLastLt, sStallTicks, sStallUs);
        }
        RateReceipt(NowMs());
        sHaveTick = 0;
    }
    if (sPhase == GDX_MPO_RACE && sLs != NULL && sLs->desynced && !sDesyncReported) {
        const int culprit = sLs->desyncPeer;
        char why[128];
        sDesyncReported = 1;
        snprintf(why, sizeof(why), "DESYNC lt=%lu peer=p%d local=%08lx remote=%08lx", (unsigned long) sLs->desyncTick,
                 culprit, (unsigned long) sLs->desyncLocal, (unsigned long) sLs->desyncRemote);
        SetMessage("race ended: out of sync with %s", sLb.peer[gdx_lobby_player_slot(&sLb, culprit) & 3].name);
        AbortSession(GDX_LB_ABORT_DESYNC, culprit, why);
    }
    Service(NowMs());
    ServicePicks(NowMs());
    if (sPhase != GDX_MPO_ROOM) {
        gdx3ds_mpp_frame(); /* a leaving picker finishes outside a room too */
    }
    gdx3ds_mpg_frame();
    if (sLoggedPhase != sPhase) {
        sLoggedPhase = sPhase;
    }
}

void gdx3ds_mpo_ui_nearby_shown(void) {
    const uint32_t now = NowMs();
    sUiSeenMs = now != 0 ? now : 1;
}

void gdx3ds_mpo_on_suspend(void) {
    if (gdx3ds_mpo_in_session()) {
        SetMessage("race ended: this console was suspended");
        AbortSession(GDX_LB_ABORT_SUSPEND, sLs != NULL ? sLs->localPlayer : -1, "HOME / suspend");
    }
    gdx3ds_p2p_park(1); /* after the ABORT is queued: the thread sends it, then stays off SOC */
    gdx3ds_mp_radio_release("HOME / sleep");
}

void gdx3ds_mpo_on_resume(void) {
    gdx3ds_p2p_park(0);
    ApplyRadioPolicy();
}

/* ---- the UI's view ------------------------------------------------------------------------- */
static int PickStep(void) {
    if (gdx_lobby_host_round(&sLb) == 0) {
        return GDX_MPO_STEP_LOBBY;
    }
    if (gdx_lobby_host_picking(&sLb) == GDX_LB_PICK_COURSE) {
        return GDX_MPO_STEP_COURSE;
    }
    if (sLb.mySlot == 0 ? (gdx_lobby_picks_in(&sLb) || sLb.pickWaitOver) && sCourseStepOpened
                        : gdx_lobby_picks_in(&sLb) && gdx_lobby_host_picking(&sLb) == GDX_LB_PICK_NONE) {
        return GDX_MPO_STEP_READY;
    }
    return GDX_MPO_STEP_MACHINES;
}

static void FillPickView(GdxMpoUi* out, uint32_t now) {
    out->stockPick = StockPickOn();
    out->pickRound = gdx_lobby_host_round(&sLb);
    out->pickStep = PickStep();
    out->pickSecsLeft = -1;
    if (sPickDeadlineMs != 0 && (int32_t) (sPickDeadlineMs - now) > 0) {
        out->pickSecsLeft = (int) ((sPickDeadlineMs - now + 999u) / 1000u);
    } else if (sPickDeadlineMs != 0 && !sLb.picked) {
        out->pickSecsLeft = 0;
    }
    out->hostPicking = gdx_lobby_host_picking(&sLb);
    out->courseRefused = gdx3ds_mpp_course_refused_recently();
    out->courseOk = sLb.courseKey != GDX_MPO_NO_COURSE_KEY;
    out->course = sLb.mySlot == 0 ? sRoomCourse : gdx_lobby_host_course(&sLb);
    out->keepMachine = KeepOffered(now) ? sLastPick.machine : -1;
}

/* The results screen: places, points and the whole field (whose places the CPUs took). */
static void FillResultsView(GdxMpoUi* out) {
    const GdxRaceFieldSnapshot* f = gdx3ds_mpg_last_field();
    int s;
    int i;
    for (s = 0; s < GDX_MPO_MAX_SLOTS; s++) {
        out->player[s].place = sRacePlace[s];
        out->player[s].racePoints = sRacePoints[s];
        out->player[s].points = sPoints[s];
        out->player[s].byCpu = (sByCpuSlots >> s) & 1u;
    }
    out->fieldCount = f->count < GDX_MPO_FIELD_MAX ? f->count : GDX_MPO_FIELD_MAX;
    for (i = 0; i < out->fieldCount; i++) {
        const int p = f->byPos[i].racer;
        out->fieldChar[i] = f->byPos[i].character;
        out->fieldSlot[i] = (p >= 0 && p < sLb.start.numPlayers && p < GDX_MPO_MAX_SLOTS)
                                ? (signed char) sLb.start.player[p].slot
                                : (signed char) -1;
    }
}

void gdx3ds_mpo_ui(GdxMpoUi* out) {
    const uint32_t now = NowMs();
    const GdxP2pStatus* st = sSocketOpen ? gdx3ds_p2p_status() : NULL;
    int s;
    memset(out, 0, sizeof(*out));
    out->phase = sPhase;
    out->mySlot = -1;
    out->keepMachine = -1;
    out->courseSecsLeft = -1;
    out->raceWait = sNearbyWait.active;
    out->whyNot = "";
    snprintf(out->message, sizeof(out->message), "%s", sMessage);
    out->course = sCfg.course >= 0 ? sCfg.course : gCourseIndex;
    out->laps = sCfg.laps;
    out->haveServer = sCfg.haveServer;
    out->haveDirect = sCfg.haveDirect;
    out->lan = sCfg.lan;
    out->nearbyError = sCfg.lan && !sDiscOpen ? sDiscError : 0; /* not open yet (MPFIX: lazy) = no error */
    out->model = sModel;
    snprintf(out->myName, sizeof(out->myName), "%s", sCfg.name);
    out->uds = UdsEnabled();
    out->udsError = UdsEnabled() ? sUdsError : 0;
    snprintf(out->udsWhy, sizeof(out->udsWhy), "%s", sUdsError != 0 ? sUdsWhy : "");
    if ((sDiscOpen && sDisc != NULL) || UdsEnabled()) {
        GdxMpNearbyRoom rows[GDX_LAN_MAX_NEARBY + GDX_MP_UDS_NEARBY_MAX];
        int n = NearbyRows(rows, (int) (sizeof(rows) / sizeof(rows[0])));
        if (n > GDX_MPO_UI_NEARBY) {
            n = GDX_MPO_UI_NEARBY;
        }
        for (s = 0; s < n; s++) {
            const GdxMpNearbyRoom* r = &rows[s];
            GdxMpoUiNearby* u = &out->nearby[s];
            u->medium = r->medium;
            snprintf(u->name, sizeof(u->name), "%s", r->b.name);
            u->players = r->b.players;
            u->maxPlayers = r->b.maxPlayers;
            u->model = r->b.model;
            u->signal = r->signal;
            u->pingMs = r->pingMs;
            u->joinable = r->joinable;
            u->compatible = r->compatible;
            snprintf(u->whyNot, sizeof(u->whyNot), "%s", r->whyNot != NULL ? r->whyNot : "");
        }
        out->nearbyCount = n;
    }
    if (st == NULL || !sLbInit || sPhase < GDX_MPO_ROOM) {
        return;
    }
    out->isLan = st->mode != GDX_P2P_MODE_ONLINE;
    out->isUds = sUdsInRoom;
    out->relayOnly = st->forceRelay;
    /* serverless online: the host of a WiFi room shows the code its joiners type */
    out->showCode = out->isLan && !sUdsInRoom && st->mode == GDX_P2P_MODE_LAN_HOST && sCfg.directHost;
    out->racesDone = sRacesDone;
    out->isHost = (sLb.mySlot == 0);
    out->mySlot = sLb.mySlot;
    snprintf(out->code, sizeof(out->code), "%s", st->code);
    out->maxPlayers = st->maxPlayers;
    out->ready = sLb.ready;
    out->machine = sLb.machine;
    FillPickView(out, now);
    out->raceWait = sRaceWait || sNearbyWait.active;
    out->roomClosed = sRoomClosed;
    snprintf(out->closedWhy, sizeof(out->closedWhy), "%s", sRoomClosed ? sRoomClosedWhy : "");
    out->courseSecsLeft = out->isHost ? CourseSecsLeft(now) : -1;
    snprintf(out->endedBy, sizeof(out->endedBy), "%s", sEndedByName);
    if (out->isHost && !sRoomClosed) {
        int away = -1;
        out->awayStartable = gdx_lobby_host_startable_without_away(&sLb, now, &away);
        if (out->awayStartable >= 2 && away >= 0 && away < GDX_MPO_MAX_SLOTS) {
            snprintf(out->awayName, sizeof(out->awayName), "%s", sLb.peer[away].name);
        } else {
            out->awayStartable = 0;
        }
    }
    if (out->isHost) {
        out->startable = gdx_lobby_host_startable(&sLb, now, &out->whyNot);
        out->delayPreview = sCfg.delay > 0 ? sCfg.delay : gdx_lobby_pick_delay(&sLb, now, NULL, NULL);
        {
            GdxMpLobbySettings ls;
            gdx3ds_mpg_current_settings(&ls);
            out->vsCom = sCfg.vsCom >= 0 ? (sCfg.vsCom != 0) : (ls.vsCom != 0);
        }
        out->racers = EffectiveRacers();
        out->racersCap = RacersCap();
    }
    for (s = 0; s < GDX_MPO_MAX_SLOTS; s++) {
        GdxMpoUiPlayer* pl = &out->player[s];
        pl->slot = s;
        pl->rttMs = -1;
        if (s == sLb.mySlot) {
            pl->present = 1;
            pl->isMe = 1;
            pl->live = 1;
            pl->ready = sLb.ready;
            pl->versionOk = 1;
            pl->machine = sLb.machine;
            pl->model = sModel;
            pl->picking = sLb.picking;
            pl->picked = gdx_lobby_peer_picked(&sLb, s);
            snprintf(pl->name, sizeof(pl->name), "%s", sLb.name);
            continue;
        }
        if (!sLb.peer[s].present) {
            continue;
        }
        pl->present = 1;
        pl->live = gdx_lobby_peer_live(&sLb, s, now);
        pl->ready = sLb.peer[s].hello.ready;
        pl->versionOk = !sLb.peer[s].heard || sLb.peer[s].versionOk;
        pl->machine = sLb.peer[s].hello.machine;
        pl->model = sLb.peer[s].hello.model;
        pl->picking = sLb.peer[s].hello.picking;
        pl->picked = gdx_lobby_peer_picked(&sLb, s);
        {
            const uint32_t r = gdx_lobby_peer_rtt(&sLb, s);
            pl->rttMs = (r == GDX_LB_NO_RTT) ? -1 : (int) r;
        }
        {
            const GdxMpLink* l = gdx3ds_p2p_link(s);
            pl->link = (l != NULL) ? l->state : GDX_LINK_NONE;
        }
        snprintf(pl->name, sizeof(pl->name), "%s", sLb.peer[s].name);
    }
    if (sRacesDone > 0 && sLb.haveStart) {
        int p;
        for (p = 0; p < sLb.start.numPlayers; p++) {
            const int slot = sLb.start.player[p].slot;
            if (slot < GDX_MPO_MAX_SLOTS) {
                out->player[slot].resultPos = gdx3ds_mpg_result(p, &out->player[slot].resultMs);
                out->player[slot].raced = 1;
                out->player[slot].player = p;
                snprintf(out->player[slot].resultName, sizeof(out->player[slot].resultName), "%s", sSessionName[slot]);
            }
        }
        FillResultsView(out);
    }
}

/* 0..3 bars from a round trip (the lobby's signal bars use the same steps). */
static int LinkBars(int rttMs) {
    if (rttMs < 0) {
        return 0;
    }
    return rttMs < 80 ? 3 : rttMs < 160 ? 2 : rttMs < 300 ? 1 : 0;
}

/* The worst round trip to a racing peer that has not been removed, -1 = none measured. */
static int WorstRaceRtt(void) {
    int worst = -1;
    int p;
    for (p = 0; p < sLb.start.numPlayers && p < GDX_MPO_MAX_SLOTS; p++) {
        const int slot = sLb.start.player[p].slot;
        uint32_t r;
        if (slot == sLb.mySlot || slot >= GDX_MPO_MAX_SLOTS || ((sRm.removedMask >> p) & 1u)) {
            continue;
        }
        r = gdx_lobby_peer_rtt(&sLb, slot);
        if (r != GDX_LB_NO_RTT && (int) r > worst) {
            worst = (int) r;
        }
    }
    return worst;
}

void gdx3ds_mpo_race_view(GdxMpoRaceView* out) {
    int p;
    memset(out, 0, sizeof(*out));
    out->dropPlayer = -1;
    out->worstRttMs = -1;
    if (sPhase != GDX_MPO_RACE || sLs == NULL || !sLb.haveStart) {
        return;
    }
    out->live = 1;
    out->numPlayers = sLs->numPlayers;
    out->localPlayer = sLs->localPlayer;
    out->delay = sLb.start.delay;
    out->worstRttMs = WorstRaceRtt();
    out->linkBars = LinkBars(out->worstRttMs);
    out->removedMask = sRm.removedMask;
    out->dropSerial = sDropSerial;
    out->dropPlayer = sDropPlayer;
    out->dropCpu = sCfg.dropMode == MPO_DROP_CPU;
    for (p = 0; p < sLs->numPlayers && p < GDX_MPO_MAX_SLOTS; p++) {
        const int slot = sLb.start.player[p].slot;
        if (slot < GDX_MPO_MAX_SLOTS) {
            snprintf(out->name[p], sizeof(out->name[p]), "%s", sSessionName[slot]);
        }
    }
}

void gdx3ds_mpo_pause_view(GdxMpoPauseView* out) {
    GdxMpgPause pz;
    memset(out, 0, sizeof(*out));
    out->player = -1;
    if (sPhase != GDX_MPO_RACE || sLs == NULL || !gdx3ds_mpg_pause(&pz)) {
        return;
    }
    out->paused = 1;
    out->player = pz.player;
    out->mine = pz.player == sLs->localPlayer;
    snprintf(out->name, sizeof(out->name), "%s", PlayerName(pz.player));
    snprintf(out->option, sizeof(out->option), "%s", pz.optionName != NULL ? pz.optionName : "");
    out->endsRace = pz.endsRace;
    out->holdPermille = gdx_mp_pause_gate_permille(&sPauseGate, PauseGateOn(&pz), NowMs(), sCfg.holdMs);
}

void gdx3ds_mpo_pause_touch(int held) {
    sPauseTouch = held != 0;
}

/* ---- bottom-screen product menus (gdx3ds_bui.c): the few settings the touch UI sets directly. */

/* Outside a room only: the name others see ([net] mp_name, sanitised the same way the ini value
 * is). The caller persists the ini. 0 = refused (in a room, or nothing left after sanitising). */
int gdx3ds_mpo_ui_set_name(const char* name) {
    char clean[GDX_MPO_NAME_LEN + 1];
    if (sPhase != GDX_MPO_OFF && sPhase != GDX_MPO_IDLE) {
        return 0;
    }
    gdx_mpo_name_sanitise(name != NULL ? name : "", clean);
    if (clean[0] == '\0') {
        return 0;
    }
    snprintf(sCfg.name, sizeof(sCfg.name), "%s", clean);
    gdx3ds_config_set_string("net", "mp_name", sCfg.name);
    MpoLog("[mpo] name -> %s (bottom menus)", sCfg.name);
    return 1;
}

/* The room size the next HOST opens with (2..4, [net] mp_players for this session). */
void gdx3ds_mpo_ui_set_room_size(int players) {
    sCfg.maxPlayers = ClampInt(players, 2, GDX_MPO_MAX_SLOTS);
}

int gdx3ds_mpo_ui_room_size(void) {
    return sCfg.maxPlayers;
}

/* Host: whether a START fills the race with CPU racers (the VS COM option; [net] mp_vscom). */
void gdx3ds_mpo_ui_set_vscom(int on) {
    if (sPhase == GDX_MPO_ROOM && sLbInit && sLb.mySlot == 0) {
        sCfg.vsCom = on ? 1 : 0;
    }
}
