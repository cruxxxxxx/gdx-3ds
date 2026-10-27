/* port/3ds/net/gdx3ds_mp_online.h — mp-online: internet multiplayer, up to 4 consoles, VS races.
 *
 * LAYERS (each replaceable without touching the others; rendering is in none of them):
 *   transport  gdx3ds_mp_p2p.c       one UDP socket: rendezvous, hole punching, relay fallback
 *   lobby      port/gdx_mp_lobby.c   HELLO / START / ABORT between the consoles (pure)
 *   lockstep   port/gdx_mp_lockstep.c  the pad exchange + per-tick state digest (pure)
 *   game       gdx3ds_mp_game.c      launch / pin / end the VS race
 *   this TU    the controller: config, timers, the poll hook, the UI's view of all of it.
 *
 * TOPOLOGY: a full MESH. Every console sends its pads to every other one, each link on its own
 * path (direct after a two-way confirmed punch, else through the server's relay). Why not the
 * spike's star: a star doubles the latency of every joiner<->joiner pair (two hops through the
 * host) and makes the host's uplink carry (N-1)^2 streams; with 4 players the pads are ~130 B a
 * tick, so a mesh is 3 x 60 x 130 = ~23 KB/s up per console -- trivial -- and the input delay is
 * set by the worst PAIR's RTT instead of twice the worst host link. Relay fallback is per link,
 * so one console behind a symmetric NAT costs only its own links a server hop.
 *
 * ON BY DEFAULT: a gdiffuser.ini with no [net] lines gets NEARBY ([net] mp_lan, default 1) and
 * ONLINE rooms on the project's hosted server ([net] rendezvous, default
 * GDX_MPO_DEFAULT_RENDEZVOUS below). Nothing opens at boot either way: NEARBY's sockets open
 * while its list is on screen, a room's when one is entered (MPFIX), so a player who never opens
 * MULTIPLAYER pays one config read and the content hash at boot. [net] enabled (the developer
 * log link, gdx3ds_net.c) is a separate lever: multiplayer brings up its own soc:U and works
 * with it off (its default).
 *
 * KILLSWITCH: [net] rendezvous = (empty) or off, [net] mp_lan = 0 and no [net] mp_direct = inert:
 * no socket, no allocation, no receipt, no menu tab; the input path is the shipped one.
 *
 * TWO WAYS IN (the MP tab's NEARBY | ONLINE): NEARBY = rooms on the same WiFi, found by beacon
 * (gdx3ds_mp_discovery.h) and hosted by the host console itself (port/gdx_mp_lan.h); ONLINE = a
 * room code on a rendezvous server. Both lead to the same lobby, lockstep and removal.
 *
 * ini keys ([net]):
 *   rendezvous   (hosted)   host:port of a tools/mp-rendezvous server (dotted IPv4; :27200 default).
 *                           Absent = GDX_MPO_DEFAULT_RENDEZVOUS; "" or off = no ONLINE rooms
 *   mp_lan       1          the NEARBY tab: local wireless + same-WiFi rooms, no server (0 = off)
 *   mp_lan_broadcast 1      beacon by broadcast (0 = the unicast list only: the emulator lane)
 *   mp_lan_peers ""         extra beacon targets, comma-separated IPv4 (the emulator: 127.0.0.1)
 *   mp_lan_timeout_ms 15000 NEARBY host: a joiner silent this long (no keepalive, no game
 *                           traffic) leaves the room
 *   mp_laps      3          host: laps per race (1..9; the lobby changes it)
 *   mp_uds       1          with mp_lan: NEARBY also lists and hosts 3DS LOCAL WIRELESS rooms (no
 *                           router; the radio scans only while the NEARBY list is on screen)
 *                           NEARBY's UDP sockets open when its list is first painted and close
 *                           10 s after it left the screen with no room (nothing runs at boot)
 *   mp_cost_every 600       an `[mpcost]` receipt every N frames while anything multiplayer is
 *                           live: the main thread's MP cost per frame (0 = off)
 *   room         ""         a room code to join at boot (same as typing it in the MP tab)
 *   mp_uds_rx_gate 1        local wireless: udsPullPacket only after the bind's receive event
 *                           fired (a probe pull every 250 ms turns the gate off if the event
 *                           ever misses a frame); 0 = pull on every drain, as 90049af
 *   mp_uds_soc_poll_ms 250  in a local wireless room the WiFi socket is read this often (0 = every drain)
 *   mp_uds_disc_poll_ms 250 ... and NEARBY discovery serviced this often (0 = every frame)
 *   mp_uds_thread 1         local wireless: a service thread makes every udsSendTo / udsPullPacket
 *                           (the main thread only queues); 0 = inline calls on the main thread
 *   mp_uds_tx_min_ms 33     ... and sends a lockstep INPUT packet at most this often per node, only
 *                           the newest (it carries every unacked pad); 0 = every packet at once
 *   mp_uds_thread_core -2   ... the thread's core (-2 = the app core; 1 = the system core)
 *   mp_soc_thread 1         online / NEARBY WiFi / serverless: a service thread makes every sendto /
 *                           recvfrom of the room socket (the main thread only queues; gdx3ds_mp_socthr.h);
 *                           0 = inline calls on the main thread, as before
 *   mp_soc_tx_min_ms 33/16  ... a lockstep INPUT at most this often per destination and stream, only
 *                           the newest (Old 3DS 33, New 3DS 16; 0 = every packet at once)
 *   mp_soc_rx_poll_ms 8/4   ... the socket is drained (recvfrom to EAGAIN) this often (Old / New 3DS)
 *   mp_soc_thread_core -2   ... the thread's core (-2 = the app core; 1 = the system core)
 *   mp_o3ds_extra_delay 0   host: input-delay ticks added to the RTT pick when an Old 3DS races
 *                           (0..4; d07ae82 had a fixed +3)
 *   mp_name      profile    the name others see (12 chars of [A-Za-z0-9 _.-]); default: the 3DS
 *                           profile's user name (accents folded, other characters dropped),
 *                           else "3DS-<last octet>"
 *   mp_bind_port 27300      local UDP port (tries the next 7 when taken)
 *   mp_relay     auto       force = never punch, every link through the server's relay
 *   mp_rv_cookie auto       GXRV version of online CREATE / JOIN (tools/mp-rendezvous/README.md):
 *                           1 = always v2 (the return-routability cookie; a 2.0.0 server requires
 *                           it by default), 0 = always v1 (old servers / tests), auto = v2 first,
 *                           v1 for the session if two v2 tries get no answer at all (a server
 *                           before 2.0.0), back to v2 if that v1 is refused as "needs GXRV v2"
 *   mp_rv_proto  GDX_MPO_PROTO test lever: the client proto v2 requests carry (lower it to see
 *                           the server's UPDATE REQUIRED; the lobby still uses the real one)
 *   mp_players   4          host: room size (2..4)
 *   mp_delay     0          0 = pick from the measured RTT (2..8); N = force
 *   mp_drop      cpu        what a removed racer's machine does from its removal tick: cpu =
 *                           the CPU driver takes it over (no pad, no energy pin), retire = it
 *                           retires, coast = neutral pad. In the sim-ABI word (all must match)
 *   mp_drop_ms   3000       a racer silent this long is removed (mp_drop decides its machine;
 *                           the race goes on). Before its first race pad: mp_launch_ms
 *   mp_wait_ms   20000      a race tick waiting longer than this for a peer = peer lost -> abort
 *   mp_wait_quiet_ms 500    a waiting console resends every 16 ms while the player it waits for is
 *                           heard; once that player is silent this long, only a heartbeat per 100 ms
 *                           (0 = always every 16 ms, as before)
 *   mp_launch_ms 120000     the same for the first race tick (consoles load at their own pace)
 *   mp_wifi_check 1         refuse to host/join with WiFi off (ACU_GetWifiStatus)
 *   mp_hash_every 1         state digest cadence (ticks)
 *   mp_stock_pick 1         a room picks with the stock top-screen menus: the host opens MACHINE
 *                           SELECT on every console at once, then picks the course in COURSE SELECT
 *                           (0, and always with mp_auto: the bottom-screen machine / ready rule)
 *   mp_pick_ms   60000      the machine step's time (5000..60000; a test lever)
 *   mp_course_ms 30000      mp-menus: the host's course step; then the highlighted course is the
 *                           pick (5000..120000; 0 = no limit)
 *   test levers (emulator lane / hardware smoke tests):
 *   mp_auto      ""         host | join: host a room / join `room` at boot, and be ready;
 *                           lanhost | lanjoin: the same over NEARBY (lanjoin = the first
 *                           joinable room heard); udshost | udsjoin: over local wireless
 *   mp_code      ""         mp_auto=host: ask for this code (only a --allow-custom-codes server)
 *   mp_autostart 0          host: start as soon as N players are in, live and ready
 *   mp_races     1          host with mp_autostart: start again after each race, N races in all
 *   mp_course    -1         host: the race's course (else the host's current course)
 *   mp_vscom     -1         host: CPU racers on (1) / off (0) (else the host's VS setting)
 *   mp_bot       0          1 = this player is driven by the game's CPU driver, with a share of
 *                           its (jittered, exchanged) pad blended in: lets a race finish unattended
 *                           2 = ram mode (MP30): also the CPU's boosts and attacks, R/Z/spin from
 *                           the pad, no energy pin (machines get destroyed)
 *   mp_end_after 0          end every race after N ticks (0 = run to the finish)
 *   mp_desync_at -1         flip gRandSeed1 after lockstep tick N (proves the desync abort)
 *   mp_sim_nat   ""         symmetric = drop every direct datagram we receive
 *   mp_leave_at  -1         leave the room at race tick N (the others must remove us and race on)
 *   mp_suspend_at -1        MP30: at race tick N do what HOME / sleep does (gdx3ds_mpo_on_suspend:
 *                           ABORT "suspended" to the others, who remove us and race on)
 *   mp_vanish_at -1         go dark at race tick N: no LEAVE, no ABORT, nothing (a power cut)
 *   mp_stats_every 600      a `[mpo] stats` receipt every N race ticks
 *   mp_hash_log  0          log the first N race ticks' digest + per-racer hashes, and every
 *                           racer's fields for ticks 0-1 and the last 64 of the N (desync triage)
 *   mp_hash_deep 0          1 = the digest covers every byte of every racer (desync triage)
 *   mp_racers    0          MP30, host: VS COM machine count 4 | 8 | 16 | 30 (0 = the default: 30,
 *                           or 16 when an Old 3DS is in the room; the lobby's MACHINES row
 *                           changes it, never above the cap)
 *   mp_racers_o3ds_30 0     MP30 lever, host: 1 = allow 30 machines with an Old 3DS in the room
 *   mp_bgm_hold_until 0     MP30 test lever: this console's pads say "BGM still loading" until
 *                           race tick N (the Expansion Kit countdown hold must then last the same
 *                           number of ticks on every console; past 180 held ticks the cap wins)
 *
 * TIME on the emulator lane: as in gdx3ds_mp.h, every *_ms value is guest time.
 * Receipts: `[mpo] ...` lines (filelog + debug output). */
#ifndef GDX3DS_MP_ONLINE_H
#define GDX3DS_MP_ONLINE_H

#include "gdx_mp_online.h"

/* OUT-OF-THE-BOX MULTIPLAYER: the defaults when gdiffuser.ini leaves the keys out.
 * GDX_MPO_DEFAULT_RENDEZVOUS is the project's hosted rendezvous server (a public IPv4, port 27200).
 * TEMPORARY: it is there so ONLINE works on a fresh install of this release; a later build will
 * drop this default and players will set [net] rendezvous themselves (or play NEARBY / JOIN BY
 * ADDRESS). [net] rendezvous = (empty) or = off turns ONLINE off; any dotted IPv4[:port] replaces
 * it (a self-hosted server: tools/mp-rendezvous/SELF-HOST.md). Read only by
 * RendezvousSetting (gdx3ds_mp_online.c). */
#define GDX_MPO_DEFAULT_RENDEZVOUS "3.149.138.4:27200"
/* [net] mp_lan when absent: NEARBY (local wireless + the same WiFi) on. 0 turns it off. */
#define GDX_MPO_DEFAULT_LAN 1

#ifdef __cplusplus
extern "C" {
#endif

enum {
    GDX_MPO_OFF = 0,     /* [net] rendezvous not set: everything inert */
    GDX_MPO_IDLE = 1,    /* not in a room */
    GDX_MPO_CONNECTING = 2,
    GDX_MPO_ROOM = 3,    /* the lobby */
    GDX_MPO_LAUNCH = 4,  /* START taken: the game is loading the race */
    GDX_MPO_RACE = 5,
    GDX_MPO_LINGER = 6,  /* the race is over: still answering peers for a moment */
};

typedef struct GdxMpoUiPlayer {
    int slot;
    int present;
    int isMe;
    int live;
    int ready;
    int versionOk;
    int machine;
    int rttMs;     /* -1 = not measured yet */
    int link;      /* GDX_LINK_* (ours: GDX_LINK_NONE) */
    int model;     /* GDX_MPO_MODEL_* (0 = not heard yet) */
    int raced;     /* in the last race */
    int resultPos; /* the last race: 1-based finish, 0 = did not finish */
    int resultMs;
    int picking;   /* stock picks: GDX_LB_PICK_* the player's top screen is in */
    int picked;    /* stock picks: machine is its pick for the current round */
    int place;      /* the last race: its machine's place in the whole field (1-based, 0 = none) */
    int racePoints; /* the last race's VS points (5/3/1/0 by place among the humans) */
    int points;     /* the room's running total (reset when the room closes or the slot empties) */
    int byCpu;      /* the last race: removed mid-race, a CPU finished it (no points) */
    int player;     /* the last race: its player index (the race's player colour) */
    char name[GDX_MPO_NAME_LEN + 1];
    char resultName[GDX_MPO_NAME_LEN + 1]; /* mp-menus: its name when the last race started ("" = none) */
} GdxMpoUiPlayer;

/* GdxMpoUi.pickStep: where a stock-pick room is. */
enum {
    GDX_MPO_STEP_LOBBY = 0,    /* no round yet: the host opens machine select */
    GDX_MPO_STEP_MACHINES = 1, /* the round's machine step: someone is still picking */
    GDX_MPO_STEP_COURSE = 2,   /* the host is picking the course */
    GDX_MPO_STEP_READY = 3,    /* every pick is in: the host can start */
};

#define GDX_MPO_UI_NEARBY 6
#define GDX_MPO_FIELD_MAX 30
typedef struct GdxMpoUiNearby {
    char name[GDX_MPO_NAME_LEN + 1]; /* the host's name: the row reads "<name>'s room" */
    int players, maxPlayers;
    int model;
    int signal;   /* 0..3 */
    int pingMs;   /* -1 = not measured */
    int joinable;
    int compatible; /* 0 = other build / ROM: greyed, never joinable */
    int medium;     /* GDX_NEARBY_MEDIUM_WIFI | _UDS (port/gdx_mp_lan.h) */
    char whyNot[16];
} GdxMpoUiNearby;

typedef struct GdxMpoUi {
    int phase;
    int isHost;
    int mySlot;
    char code[GDX_MPO_CODE_LEN + 1];
    int maxPlayers;
    int ready;
    int machine;
    int startable;          /* host: players a START would take now (0 = not startable) */
    const char* whyNot;
    int delayPreview;       /* host: the delay a START would pick now */
    int vsCom;              /* host: a START fills the race with CPU racers (the VS COM option) */
    int racers;             /* host: the machine count a VS COM START would carry (4/8/16/30, MP30) */
    int racersCap;          /* host: the most the room allows now */
    int course;
    int laps;
    int haveServer;         /* ONLINE works ([net] rendezvous) */
    int haveDirect;         /* ONLINE's JOIN BY ADDRESS works ([net] mp_direct = a.b.c.d[:port]) */
    int relayOnly;          /* the room is relay-only (forced: [net] mp_relay, the server, or L held) */
    int showCode;           /* serverless host ([net] mp_direct = host): the WiFi room's code is shown */
    int lan;                /* NEARBY works ([net] mp_lan) */
    int nearbyError;        /* the discovery backend failed to open (its code) */
    int isLan;              /* the room we are in is a NEARBY one */
    int isUds;              /* ... over local wireless */
    int uds;                /* NEARBY lists / hosts local-wireless rooms too ([net] mp_uds) */
    int udsError;           /* the radio's last failure (GDX_UDS_E_*; 0 = fine) */
    char udsWhy[96];        /* its sentence for the user */
    int model;              /* this console */
    int racesDone;          /* races finished in this room (RESULTS shown after the first) */
    char myName[GDX_MPO_NAME_LEN + 1];
    int nearbyCount;
    GdxMpoUiNearby nearby[GDX_MPO_UI_NEARBY];
    char message[128];      /* the newest event worth telling the user ("" = none) */
    /* stock picks ([net] mp_stock_pick; ../gdx3ds_mp_pick.h) */
    int stockPick;          /* this room picks with the stock top-screen menus */
    int pickRound;          /* the host's round (0 = none yet) */
    int pickStep;           /* GDX_MPO_STEP_* */
    int pickSecsLeft;       /* this console's machine-step countdown, -1 = none */
    int hostPicking;        /* GDX_LB_PICK_* of the host's top screen */
    int courseRefused;      /* the host's course picker just refused a course (an EDIT cup) */
    int courseOk;           /* this console can race the host's course (its key matches) */
    int keepMachine;        /* mp-menus: the machine KEEP confirms in this round's machine step (this
                             * console's previous pick), -1 = none offered */
    int roomClosed;         /* mp-menus: the room went away under the last race: its results stay,
                             * nothing more can happen in it but leaving */
    char closedWhy[48];     /* ... why ("the host left", "lost the server") */
    int awayStartable;      /* mp-menus, host: players a START WITHOUT would take (>= 2) when only
                             * members gone quiet (HOME, sleep) hold the room up; 0 = none */
    char awayName[GDX_MPO_NAME_LEN + 1]; /* ... the first quiet member's name */
    int raceWait;           /* mp-menus, a joiner: the room is racing without it; it waits for the
                             * race to end (no mid-race joins) and is in from the results on. Also
                             * outside a room: a NEARBY room picked while it races (joined when
                             * its beacon says the race is over; gdx3ds_mpo_ui_leave cancels) */
    int courseSecsLeft;     /* mp-menus, host: the course step's countdown, -1 = none */
    char endedBy[GDX_MPO_NAME_LEN + 1]; /* mp-menus #10: the last race was ended from the pause
                             * menu by this player ("" = it ran to the finish) */
    /* the last race's whole field (the results screen's CPU places) */
    int fieldCount;                              /* machines in it, 0 = no results */
    signed char fieldChar[GDX_MPO_FIELD_MAX];    /* by finishing place: the character */
    signed char fieldSlot[GDX_MPO_FIELD_MAX];    /* ... a human's slot, -1 = a CPU */
    GdxMpoUiPlayer player[GDX_MPO_MAX_SLOTS];
} GdxMpoUi;

/* The multiplayer race dash's view of the running race (gdx3ds_bui_race.c). */
typedef struct GdxMpoRaceView {
    int live;           /* a session race is running (phase RACE) */
    int numPlayers;
    int localPlayer;
    int delay;          /* input delay (ticks) */
    int linkBars;       /* 0..3: the worst link to a racing peer now (3 = every link fine) */
    int worstRttMs;     /* -1 = not measured */
    unsigned removedMask; /* bit = player index */
    int dropSerial;     /* bumps on every removal this race */
    int dropPlayer;     /* the newest removal's player index, -1 = none */
    int dropCpu;        /* 1 = [net] mp_drop cpu: a CPU drives a removed machine */
    char name[GDX_MPO_MAX_SLOTS][GDX_MPO_NAME_LEN + 1]; /* by player index */
} GdxMpoRaceView;
void gdx3ds_mpo_race_view(GdxMpoRaceView* out);

/* mp-menus #10: the shared pause as the race dash shows it. Any player may pause (START); every
 * console pauses at the same tick (the pads are exchanged). Only the pauser's pad drives the
 * pause menu (the stock rule, deterministic): it resumes, or ends the race for everyone with any
 * other row, held [ui] hold_confirm_ms first (gdx_mp_pause.h). */
typedef struct GdxMpoPauseView {
    int paused;
    int mine;           /* this console paused: its A answers the top screen's menu */
    int player;         /* the pauser's player index */
    char name[GDX_MPO_NAME_LEN + 1];
    char option[20];    /* the highlighted row ("QUIT") */
    int endsRace;       /* ... ends the race (everything but CONTINUE) */
    int holdPermille;   /* the pauser's confirm hold, 0..1000 */
} GdxMpoPauseView;
void gdx3ds_mpo_pause_view(GdxMpoPauseView* out);
/* The bottom screen's pause button is held (touch): this console's A, through the same gate. */
void gdx3ds_mpo_pause_touch(int held);

/* Painted from inside a stalled race tick (the poll spins in WaitReady and the main loop does not
 * run): the bottom screen's "WAITING FOR <name>" banner, about every 100 ms once the stall passes
 * 500 ms; name NULL = the stall is over. dropAfterMs: when the silent player is removed (from the
 * start of its silence), -1 = unknown. Weak: implemented by gdx3ds_bui_race.c. */
void gdx3ds_bui_race_stall(const char* name, int waitedMs, int dropAfterMs) __attribute__((weak));

/* From gdx3ds_mp_init when [net] mp is off. Reads the config; opens nothing yet. */
void gdx3ds_mpo_init(void);
/* MP30: the sim-ABI word (EK flavour, GDX_MPO_PROTO, the session-forced levers); see the .c. */
uint32_t gdx3ds_mp_sim_abi(void);
/* MP30: 1 when every player's pad of the race tick being simulated carries GDX_MP_PAD_BGM_READY
 * (a removed player counts as ready); 1 outside a race. gdx3ds_mp.c dispatches to it. */
int gdx3ds_mpo_pads_bgm_ready(void);
/* MP30: [net] mp_drop normalised ("cpu" default, "retire", "coast", "off"); both lanes read it. */
const char* gdx3ds_mp_drop_mode_name(void);
/* MP30: 1 when racer `racerId` is a removed human the CPU driver now drives (mp_drop = cpu, from
 * its agreed removal tick). gdx3ds_mp.c dispatches to it. */
int gdx3ds_mpo_drop_cpu_racer(int racerId);
int gdx3ds_mpo_enabled(void);
/* The poll hook's online half (see gdx3ds_mp.h gdx_mp_exchange). */
int gdx3ds_mpo_exchange(unsigned long long tick, unsigned short* buttons, signed char* stickX, signed char* stickY,
                        unsigned char* connected, int capacity);
/* Main loop, after the game update: the digest, the lobby timers, the network. */
void gdx3ds_mpo_after_tick(void);
/* 1 while a race is being driven by the session (launching or racing). */
int gdx3ds_mpo_in_session(void);
/* This console's player index in the race being driven (mp-fullview's k), -1 outside one. */
int gdx3ds_mpo_local_player(void);
/* APT: the console is being suspended (HOME). Aborts a race so the others are not left waiting,
 * then parks the socket thread (no SOC call across the suspend). */
void gdx3ds_mpo_on_suspend(void);
/* APT: back from HOME / sleep: the socket thread may run again. */
void gdx3ds_mpo_on_resume(void);

/* The lobby UI (bottom-screen MP tab). */
void gdx3ds_mpo_ui(GdxMpoUi* out);
void gdx3ds_mpo_ui_host(void);
void gdx3ds_mpo_ui_join(const char* code);
/* Serverless online ([net] mp_direct = a.b.c.d[:port]): join the WiFi room that console hosts, by
 * its code. direct_addr: the address as text ("" and 0 when mp_direct names none). */
void gdx3ds_mpo_ui_direct_join(const char* code);
int gdx3ds_mpo_direct_addr(char* out, int cap);
void gdx3ds_mpo_ui_leave(void);
void gdx3ds_mpo_ui_toggle_ready(void);
void gdx3ds_mpo_ui_cycle_machine(int dir);
void gdx3ds_mpo_ui_cycle_course(int dir);
/* Stock picks, host: every console into machine select at once (a new round: the first, or
 * CHANGE MACHINES after a race); the course in the stock course select again (CHANGE COURSE). */
void gdx3ds_mpo_ui_open_machine_select(void);
void gdx3ds_mpo_ui_pick_course(void);
/* mp-menus: confirm this console's previous pick (keepMachine) for the running machine step, in
 * one tap: the top screen's picker takes it as if confirmed there. */
void gdx3ds_mpo_ui_keep_pick(void);
/* mp-menus, host: START with the members that went quiet left out (awayStartable). */
void gdx3ds_mpo_ui_start_without_away(void);
void gdx3ds_mpo_ui_cycle_laps(int dir);
void gdx3ds_mpo_ui_cycle_racers(int dir); /* MP30: host, the Machines option 4 / 8 / 16 / 30 */
void gdx3ds_mpo_ui_start(void);
/* After a race (host): start again on the same course, or on the next one. */
void gdx3ds_mpo_ui_rematch(int nextCourse);
/* NEARBY: host a room on this WiFi / join row `row` of nearby[]. */
void gdx3ds_mpo_ui_lan_host(void);
void gdx3ds_mpo_ui_lan_join(int row);
/* NEARBY: host a room over 3DS local wireless (no router; port/gdx_mp_uds_bridge.h). Rows of
 * nearby[] with medium == GDX_NEARBY_MEDIUM_UDS join through gdx3ds_mpo_ui_lan_join too. */
void gdx3ds_mpo_ui_uds_host(void);
/* The NEARBY list is on screen (call on every paint of it): the local-wireless scan runs only
 * while it is, and while no room is open. */
void gdx3ds_mpo_ui_nearby_shown(void);
/* Bottom-screen product menus (gdx3ds_bui.c). set_name: outside a room only; sets [net]
 * mp_name in the loaded config (the caller saves the ini); 0 = refused. set_room_size: the next
 * HOST's room size (2..4). set_vscom: host, in a room: CPU racers on/off for the next START. */
int gdx3ds_mpo_ui_set_name(const char* name);
void gdx3ds_mpo_ui_set_room_size(int players);
int gdx3ds_mpo_ui_room_size(void);
void gdx3ds_mpo_ui_set_vscom(int on);

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_MP_ONLINE_H */
