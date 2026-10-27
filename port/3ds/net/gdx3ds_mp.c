/* port/3ds/net/gdx3ds_mp.c — MP session layer. Contract + ini keys: gdx3ds_mp.h.
 * Protocol (pure, host-tested): port/gdx_mp_lockstep.c (pads + digests) and port/gdx_mp_drop.c
 * (drop-out). Transports: gdx3ds_mp_transport.h (udp-lan: gdx3ds_mp_udp.c; uds: gdx3ds_mp_uds.c). */
#include "gdx3ds_mp.h"
#include "gdx3ds_mp_online.h"
#include "gdx3ds_mp_transport.h"
#include "gdx3ds_mp_uds.h"
#include "gdx_mp_drop.h"
#include "gdx_mp_lockstep.h"

#include "gdx3ds_config.h"
#include "gdx3ds_filelog.h"
#include "gdx3ds_fs.h"

#include <3ds.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__has_include)
#if __has_include("gdx3ds_build_id.h")
#include "gdx3ds_build_id.h"
#endif
#endif
#ifndef GDX3DS_BUILD_ID
#define GDX3DS_BUILD_ID "unknown"
#endif

/* port/decomp_port.c / port/input_bridge.c (decomp-side state, no decomp headers here). */
extern unsigned int gdx_gstate_hash(int* outRacers);
extern unsigned int gdx_mpo_vis_hash(unsigned int h); /* port/decomp_port.c: MP30 visibility masks */
extern unsigned long long gdx_input_tick(void);
extern int gdx_input_game_mode(void);
extern int gdx_input_in_gameplay(void);
extern int gdx_mp_retire_player(int player);
extern int gRandSeed1;
/* port/3ds/net/gdx3ds_net.c: 1 when the dev link already owns an initialised soc:U. */
extern int gdx3ds_net_soc_up(void);

#define MP_DEFAULT_PORT 27100
#define MP_TICK_US(t) ((unsigned long) ((t) / (SYSCLOCK_ARM11 / 1000000ull)))
#define MP_PKT_CAP (GDX_MP_MAX_PACKET > GDX_MP_DROP_MAX_PACKET ? GDX_MP_MAX_PACKET : GDX_MP_DROP_MAX_PACKET)

enum {
    MP_DROP_OFF = 0,    /* a silent peer ends the session on this console (the spike's behaviour) */
    MP_DROP_COAST = 1,  /* the removed player's pad is neutral from T */
    MP_DROP_RETIRE = 2, /* ...and its machine retires at the first race tick >= T */
    MP_DROP_CPU = 3,    /* ...and the CPU driver drives its machine from T (MP30, default) */
};

static int sActive = 0;
static int sBgmReadyAll = 1; /* MP30: this tick's pads all say "BGM ready" */
static int sLocalPlayer = -1; /* this console's player index once a session opened (sticky) */
static int sIsHost = 0;
static int sRelay = 0;
static int sUds = 0;
static GdxMpLockstep* sLs = NULL;
static GdxMpDrop sDrop;
static const GdxMpTransportOps* sTx = NULL;
static u32* sOwnSocBuf = NULL;
static int sWaitMs = 20000;
static int sConnectMs = 300000;
static int sStatsEvery = 600;
static int sXform = 0;
static long sDesyncAt = -1;
static long sHashFrom = 0;
static unsigned long long sStartTick = 300;
static int sDesyncReported = 0;
static unsigned long long sLastTick = 0;
static int sHaveTick = 0;
static int sPeerSeen[GDX_MP_MAX_PLAYERS];
static int sAnyRemotePad = 0;
static uint32_t sContentHash = 0;

/* Drop-out ([net] mp_drop / mp_drop_ms / mp_leave_at). */
static int sDropMode = MP_DROP_CPU;
static u32 sSimLt = 0; /* MP30: the lockstep tick the game is simulating */
static int sDropMs = 3000;
static long sLeaveAt = -1;
static u32 sLastHeardMs[GDX_MP_MAX_PLAYERS];
static u32 sSessionStartMs = 0;
static int sSessionStarted = 0;
static u32 sLastPresenceMs = 0;

/* UDS joiner: the lockstep starts once the room is joined (its shape comes from the beacon). */
static int sPendingJoin = 0;
static int sCfgPlayer = 0;
static u32 sJoinStartMs = 0;
static int sProbeS = 0;
static int sProbeDone = 0;
static int sRoomClosed = 0; /* uds host: the room filled, new connections blocked */

static int sSleepLocked = 0;
static aptHookCookie sAptCookie;
static int sAptHooked = 0;

/* Stats (receipt window and session totals). */
static u32 sTxPackets = 0;
static u32 sTxBytes = 0;
static u32 sRxPackets = 0;
static u32 sRxBytes = 0;
static u32 sRelayed = 0;
static u32 sRefusedFrozen = 0;
static u32 sLastRxMs = 0;
static int sLastRecvErr = 0;
static int sLastSendErr = 0;
static u32 sLastReopenMs = 0;
static u32 sReopens = 0;
static int sReopenMs = 0;
static u64 sStallUsTotal = 0;
static u32 sStallTicks = 0;      /* ticks that had to wait at all (> 200 us) */
static u32 sStallMaxUs = 0;
static u64 sWinStallUs = 0;
static u32 sWinStallTicks = 0;
static u32 sWinStallMaxUs = 0;
static u32 sWinTxBytes = 0;
static u32 sWinRxBytes = 0;
static u32 sWinTicks = 0;

static void MpLog(const char* fmt, ...) {
    char line[384];
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

static u32 NowMs(void) {
    return (u32) osGetTime();
}

static u32 Fnv(u32 h, const void* data, size_t len) {
    const u8* p = (const u8*) data;
    size_t i;
    for (i = 0; i < len; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

/* The game data this build loads: the same content hash the online / NEARBY lobby uses
 * (gdx_mpo_zip_cd_hash: each member's name, CRC-32 and uncompressed size, however the PC that
 * prebaked the archive compressed it). Absent = a fixed value. */
static uint32_t ContentHash(void) {
    static const char* const kArchives[2] = { "fzerox.o2r", "gdiffuser.o2r" };
    uint32_t h = 2166136261u;
    int i;
    for (i = 0; i < 2; i++) {
        char path[256];
        snprintf(path, sizeof(path), "%s%s", gdx3ds_fs_base_path(), kArchives[i]);
        if (gdx_mpo_zip_cd_hash(path, &h) == -1) {
            h ^= 0xA5A5A5A5u;
            h *= 16777619u;
        }
    }
    return h;
}

/* Everything both ends must agree on before a single pad means the same thing: the build (the
 * simulation code), the game data, the session shape, and the sim-affecting pins. A mismatch
 * refuses every packet and the receipt says so. A real lobby adds the synced save/unlock state. */
static u32 SessionDigest(int players, int delay, int hashEvery) {
    u32 h = 2166136261u;
    const char* build = GDX3DS_BUILD_ID;
    const char* seed1 = gdx3ds_config_get_string("debug", "rand_seed1", "");
    const char* seed2 = gdx3ds_config_get_string("debug", "rand_seed2", "");
    const int det = gdx3ds_config_get_bool("debug", "det", 0);
    const int pads = gdx3ds_config_get_int("debug", "pads", 1);
    h = Fnv(h, build, strlen(build));
    h = Fnv(h, &sContentHash, sizeof(sContentHash));
    h = Fnv(h, &players, sizeof(players));
    h = Fnv(h, &delay, sizeof(delay));
    h = Fnv(h, &hashEvery, sizeof(hashEvery));
    h = Fnv(h, &det, sizeof(det));
    h = Fnv(h, &pads, sizeof(pads));
    h = Fnv(h, seed1, strlen(seed1));
    h = Fnv(h, seed2, strlen(seed2));
    return h;
}

int gdx3ds_mp_ensure_soc(void) {
    int kb;
    u32 size;
    Result rc;
    if (gdx3ds_net_soc_up() || sOwnSocBuf != NULL) {
        return 0; /* up: the dev link's, or ours from an earlier room (soc:U stays up until exit) */
    }
    kb = gdx3ds_config_get_int("net", "mp_soc_kb", 256);
    if (kb < 64) {
        kb = 64;
    } else if (kb > 1024) {
        kb = 1024;
    }
    kb &= ~3;
    size = (u32) kb * 1024u;
    sOwnSocBuf = (u32*) memalign(0x1000, size);
    if (sOwnSocBuf == NULL) {
        return -1;
    }
    rc = socInit(sOwnSocBuf, size);
    if (R_FAILED(rc)) {
        MpLog("[mp] socInit rc=0x%08lX", (unsigned long) rc);
        free(sOwnSocBuf);
        sOwnSocBuf = NULL;
        return -2;
    }
    atexit((void (*)(void)) socExit);
    MpLog("[mp] soc own=%dKiB (dev link off)", kb);
    return 0;
}

static void LockSleep(int lock) {
    if (lock && !sSleepLocked) {
        aptSetSleepAllowed(false); /* a closed lid would drop the link and stall every peer */
        sSleepLocked = 1;
        MpLog("[mp] sleep disallowed for the session");
    } else if (!lock && sSleepLocked) {
        aptSetSleepAllowed(true);
        sSleepLocked = 0;
        MpLog("[mp] sleep allowed again");
    }
}

/* One line of protocol state for the stall/lost receipts. */
static void StateLine(const char* tag) {
    int p;
    char k[160];
    int off = 0;
    if (sLs == NULL || sTx == NULL) {
        MpLog("[mp] %s state: no session yet (rx=%lu)", tag, (unsigned long) sRxPackets);
        return;
    }
    k[0] = '\0';
    for (p = 0; p < sLs->numPlayers && off < (int) sizeof(k) - 32; p++) {
        off += snprintf(k + off, sizeof(k) - (size_t) off, " p%d:known=%lu acked=%lu%s", p,
                        (unsigned long) sLs->known[p], (unsigned long) sLs->acked[p],
                        (sDrop.removedMask & (1u << p)) ? "(removed)" : "");
    }
    {
        char st[240];
        sTx->selftest(st, sizeof(st));
        MpLog("[mp] %s selftest %s (a following rx/rej bump = our receive path works)", tag, st);
    }
    MpLog("[mp] %s state:%s rx=%lu tx=%lu last_rx=%lums_ago rej=%lu frozen_rej=%lu recv_err=%d send_err=%d "
          "reopens=%lu", tag, k, (unsigned long) sRxPackets, (unsigned long) sTxPackets,
          (unsigned long) (sLastRxMs != 0 ? NowMs() - sLastRxMs : 0), (unsigned long) sLs->rejected, (unsigned long) sRefusedFrozen,
          sLastRecvErr, sLastSendErr, (unsigned long) sReopens);
}

static void Disable(const char* why) {
    StateLine("lost");
    MpLog("[mp] lost: %s at tick=%llu -- MP OFF, this console now plays alone (its race no longer "
          "matches the others)", why, sLastTick);
    gdx3ds_filelog_flush();
    sActive = 0;
    sPendingJoin = 0;
    if (sTx != NULL) {
        sTx->close();
    }
    LockSleep(0);
}

/* We are leaving on purpose: say so (a few copies; datagrams are unreliable), then stop. */
static void Leave(const char* why) {
    u8 pkt[MP_PKT_CAP];
    int k;
    if (!sActive) {
        return;
    }
    if (sLs != NULL) {
        const u32 next = (u32) (sLastTick >= sStartTick ? sLastTick - sStartTick : 0);
        const int len = gdx_mp_drop_build_leave(&sDrop, sLs, next, GDX_MP_DROP_R_LEFT, pkt, sizeof(pkt));
        for (k = 0; k < 4 && len > 0; k++) {
            sTx->send(sIsHost ? GDX_MP_PEER_ANY : 0, pkt, len);
            svcSleepThread(5000000LL);
        }
    }
    MpLog("[mp] leave: %s (%s)", why, sIsHost ? "the host: the session ends for everyone" : "the host removes us");
    Disable(why);
}

static void AptHook(APT_HookType hook, void* param) {
    (void) param;
    if (!sActive) {
        return;
    }
    if (hook == APTHOOK_ONSUSPEND) {
        Leave("HOME / suspend");
    } else if (hook == APTHOOK_ONEXIT) {
        Leave("app closed");
    } else if (hook == APTHOOK_ONSLEEP) {
        Leave("sleep"); /* sleep is disallowed during a session; should it happen anyway */
    }
}

static const char* DropModeName(int m) {
    if (m == MP_DROP_CPU) {
        return "cpu";
    }
    return m == MP_DROP_RETIRE ? "retire" : (m == MP_DROP_COAST ? "coast" : "off");
}

static int StartLockstep(int players, int localPlayer, int delay, int hashEvery) {
    const u32 session = SessionDigest(players, delay, hashEvery);
    int p;
    sLs = (GdxMpLockstep*) calloc(1, sizeof(GdxMpLockstep));
    if (sLs == NULL || gdx_mp_ls_init(sLs, players, localPlayer, delay, hashEvery, session) != 0) {
        MpLog("[mp] init REFUSED: players=%d player=%d delay=%d hash_every=%d out of range (or no memory)", players,
              localPlayer, delay, hashEvery);
        free(sLs);
        sLs = NULL;
        return -1;
    }
    gdx_mp_drop_init(&sDrop, sLs);
    for (p = 0; p < GDX_MP_MAX_PLAYERS; p++) {
        sLastHeardMs[p] = NowMs();
    }
    sRelay = sIsHost && players > 2; /* star: the host forwards joiner packets to the others */
    sLocalPlayer = localPlayer;      /* mp-fullview: a UDS joiner learns it only here */
    return 0;
}

static void InitReceipt(int bindPort, const char* hostAddr, int port) {
    MpLog("[mp] init role=%s player=%d/%d delay=%d hash_every=%d transport=%s bind=%d host=%s:%d "
          "session=%08lx build=%s content=%08lx relay=%d pkt_bytes<=%d xform=%d desync_at=%ld hash_from=%ld "
          "start_tick=%llu drop=%s drop_ms=%d leave_at=%ld",
          sIsHost ? "host" : "join", sLs->localPlayer, sLs->numPlayers, sLs->delay, sLs->hashEvery, sTx->name,
          bindPort, (sIsHost || hostAddr == NULL) ? "-" : hostAddr, port, (unsigned long) sLs->session,
          GDX3DS_BUILD_ID, (unsigned long) sContentHash, sRelay, GDX_MP_MAX_PACKET, sXform, sDesyncAt, sHashFrom,
          sStartTick, DropModeName(sDropMode), sDropMs, sLeaveAt);
    gdx3ds_filelog_flush();
}

/* Which medium: "udp" (the default; the spike's lane), "uds" (local wireless), "auto" (uds unless
 * an mp_host IPv4 is configured or local wireless is unavailable, then udp). 1 = uds, 0 = udp,
 * -1 = uds asked for and unavailable (why says why, for the user). */
static int PickUds(char* why, int cap) {
    const char* t = gdx3ds_config_get_string("net", "mp_transport", "udp");
    const char* host = gdx3ds_config_get_string("net", "mp_host", "");
    int rc;
    if (t == NULL || t[0] == '\0' || strcmp(t, "udp") == 0) {
        return 0;
    }
    if (strcmp(t, "auto") == 0 && host != NULL && host[0] != '\0') {
        MpLog("[mp] transport auto -> udp (mp_host=%s)", host);
        return 0;
    }
    rc = gdx3ds_mp_uds_init(why, cap);
    if (rc == GDX_UDS_OK) {
        return 1;
    }
    if (strcmp(t, "auto") == 0) {
        MpLog("[mp] transport auto -> udp (local wireless unavailable: %s)", why);
        return 0;
    }
    return -1;
}

void gdx3ds_mp_init(void) {
    const char* mode = gdx3ds_config_get_string("net", "mp", "off");
    GdxMpTransportConfig cfg;
    int players;
    int localPlayer;
    int delay;
    int hashEvery;
    int port;
    int rc;
    char why[192];

    if (mode == NULL || (strcmp(mode, "host") != 0 && strcmp(mode, "join") != 0)) {
        gdx3ds_mpo_init(); /* mp-online: NEARBY / ONLINE (default on; its own killswitch) */
        return; /* KILLSWITCH: not one receipt */
    }
    sIsHost = (strcmp(mode, "host") == 0);
    players = gdx3ds_config_get_int("net", "mp_players", 2);
    delay = gdx3ds_config_get_int("net", "mp_delay", 2);
    hashEvery = gdx3ds_config_get_int("net", "mp_hash_every", 1);
    port = gdx3ds_config_get_int("net", "mp_port", MP_DEFAULT_PORT);
    sWaitMs = gdx3ds_config_get_int("net", "mp_wait_ms", 20000);
    sConnectMs = gdx3ds_config_get_int("net", "mp_connect_s", 300) * 1000;
    sStatsEvery = gdx3ds_config_get_int("net", "mp_stats_every", 600);
    sXform = gdx3ds_config_get_int("net", "mp_xform", 0);
    sDesyncAt = gdx3ds_config_get_int("net", "mp_desync_at", -1);
    sHashFrom = gdx3ds_config_get_int("net", "mp_hash_from", 0);
    sReopenMs = gdx3ds_config_get_int("net", "mp_reopen_ms", 0);
    sDropMs = gdx3ds_config_get_int("net", "mp_drop_ms", 3000);
    sLeaveAt = gdx3ds_config_get_int("net", "mp_leave_at", -1);
    sProbeS = gdx3ds_config_get_int("net", "mp_uds_probe_s", 0);
    {
        const char* dm = gdx3ds_mp_drop_mode_name();
        if (strcmp(dm, "off") == 0) {
            sDropMode = MP_DROP_OFF;
        } else if (strcmp(dm, "coast") == 0) {
            sDropMode = MP_DROP_COAST;
        } else if (strcmp(dm, "retire") == 0) {
            sDropMode = MP_DROP_RETIRE;
        } else {
            sDropMode = MP_DROP_CPU;
        }
    }
    {
        const int start = gdx3ds_config_get_int("net", "mp_start_tick", 300);
        sStartTick = (unsigned long long) (start > 0 ? start : 0);
    }
    if (sStatsEvery < 60) {
        sStatsEvery = 60;
    }
    if (sDropMs < 200) {
        sDropMs = 200;
    }
    if (players > GDX_UDS_MAX_NODES) {
        MpLog("[mp] mp_players=%d capped at %d (the human cap)", players, GDX_UDS_MAX_NODES);
        players = GDX_UDS_MAX_NODES;
    }

    why[0] = '\0';
    rc = PickUds(why, sizeof(why));
    if (rc < 0) {
        MpLog("[mp] init REFUSED: transport uds: %s", why);
        gdx3ds_filelog_flush();
        return;
    }
    sUds = rc;
    if (!sUds) {
        rc = gdx3ds_mp_ensure_soc();
        if (rc != 0) {
            MpLog("[mp] init REFUSED: SOC unavailable (%d)", rc);
            return;
        }
    }
    sContentHash = ContentHash() ^ gdx3ds_mp_sim_abi(); /* MP30: the build flavour + forced levers */
    /* udp: the joiner's index is configured (default 1). uds: 0 = take it from our node. */
    localPlayer = sIsHost ? 0 : gdx3ds_config_get_int("net", "mp_player", sUds ? 0 : 1);
    sCfgPlayer = localPlayer;

    memset(&cfg, 0, sizeof(cfg));
    cfg.isHost = sIsHost;
    cfg.localPlayer = localPlayer;
    cfg.numPlayers = players;
    cfg.bindPort = sIsHost ? port : port + localPlayer;
    cfg.hostAddr = gdx3ds_config_get_string("net", "mp_host", "127.0.0.1");
    cfg.hostPort = port;
    cfg.build = GDX3DS_BUILD_ID;
    cfg.contentHash = sContentHash;
    cfg.delay = delay;
    cfg.hashEvery = hashEvery;
    cfg.joinName = gdx3ds_config_get_string("net", "mp_room", "");
    sTx = sUds ? gdx3ds_mp_uds_ops() : gdx3ds_mp_udp_ops();
    memset(sPeerSeen, 0, sizeof(sPeerSeen));

    if (sUds && !sIsHost) {
        /* The room decides the shape: the lockstep starts when we have joined it. */
        rc = sTx->open(&cfg);
        if (rc != 0) {
            MpLog("[mp] init REFUSED: transport uds open rc=%d", rc);
            return;
        }
        sPendingJoin = 1;
        sJoinStartMs = NowMs();
        sActive = 1;
        MpLog("[mp] init role=join transport=uds: looking for a room (name=\"%s\") build=%s content=%08lx "
              "drop=%s", cfg.joinName != NULL ? cfg.joinName : "", GDX3DS_BUILD_ID, (unsigned long) sContentHash,
              DropModeName(sDropMode));
    } else {
        if (StartLockstep(players, localPlayer, delay, hashEvery) != 0) {
            return;
        }
        rc = sTx->open(&cfg);
        if (rc != 0) {
            MpLog("[mp] init REFUSED: transport %s open rc=%d (port %d)", sTx->name, rc, cfg.bindPort);
            return;
        }
        sActive = 1;
        InitReceipt(sUds ? 0 : cfg.bindPort, sUds ? NULL : cfg.hostAddr, sUds ? 0 : port);
    }
    LockSleep(1);
    if (!sAptHooked) {
        aptHook(&sAptCookie, AptHook, NULL);
        sAptHooked = 1;
    }
}

/* MP30: a removed player the CPU driver drives (mp_drop = cpu), from its agreed removal tick. */
int gdx3ds_mp_drop_cpu_racer(int racerId) {
    if (!sActive) {
        return gdx3ds_mpo_drop_cpu_racer(racerId);
    }
    if (sDropMode != MP_DROP_CPU || sLs == NULL || !sHaveTick || racerId < 0 || racerId >= sLs->numPlayers) {
        return 0;
    }
    return gdx_mp_drop_is_removed(&sDrop, racerId, sSimLt);
}

/* MP30: the EK countdown hold's lockstep input (gdx3ds_autodrive.c gdx_mp_ek_bgm_hold). */
int gdx3ds_mp_pads_bgm_ready(void) {
    if (!sActive) {
        return gdx3ds_mpo_pads_bgm_ready();
    }
    return sBgmReadyAll;
}

int gdx3ds_mp_active(void) {
    return sActive || gdx3ds_mpo_in_session();
}

/* UDS joiner: the room is joined -> the lockstep takes the room's shape. */
static void PollJoin(void) {
    char why[128];
    const int st = gdx3ds_mp_uds_join_state(why, sizeof(why));
    const GdxMpBeacon* b;
    int players;
    int me;
    if (!sPendingJoin) {
        return;
    }
    if (st == GDX_UDS_JOIN_FAILED) {
        Disable(why[0] ? why : "could not join a room");
        return;
    }
    if (st != GDX_UDS_JOIN_CONNECTED) {
        return;
    }
    b = gdx3ds_mp_uds_joined_beacon();
    players = b->maxPlayers;
    me = (sCfgPlayer > 0) ? sCfgPlayer : gdx3ds_mp_uds_local_node() - 1;
    if (me <= 0 || me >= players) {
        char msg[96];
        snprintf(msg, sizeof(msg), "no seat: node %d in a %d-player room", gdx3ds_mp_uds_local_node(), players);
        Disable(msg);
        return;
    }
    sPendingJoin = 0;
    if (StartLockstep(players, me, b->delay > 0 ? b->delay : 2, b->hashEvery > 0 ? b->hashEvery : 1) != 0) {
        Disable("the room's session shape is out of range");
        return;
    }
    gdx3ds_mp_uds_set_session(players, me);
    MpLog("[mp] joined \"%s\"'s room (%s) after %lu ms", b->name, gdx_mp_model_name(b->model),
          (unsigned long) (NowMs() - sJoinStartMs));
    InitReceipt(0, NULL, 0);
}

int gdx3ds_mp_local_player(void) {
    if (sLocalPlayer < 0) {
        return gdx3ds_mpo_local_player(); /* mp-online: this console's player in the live race */
    }
    return sLocalPlayer;
}

static void ReportDesyncOnce(void) {
    if (!sLs->desynced || sDesyncReported) {
        return;
    }
    sDesyncReported = 1;
    MpLog("[mp] DESYNC tick=%lu (input tick %llu) peer=%d local=%08lx remote=%08lx mode=%d (first divergent digest; "
          "the race on this console no longer matches)",
          (unsigned long) sLs->desyncTick, (unsigned long long) sLs->desyncTick + sStartTick, sLs->desyncPeer,
          (unsigned long) sLs->desyncLocal, (unsigned long) sLs->desyncRemote, gdx_input_game_mode());
    gdx3ds_filelog_flush();
}

static void SendOurs(void) {
    u8 pkt[GDX_MP_MAX_PACKET];
    const int len = gdx_mp_ls_build_packet(sLs, NowMs(), pkt, sizeof(pkt));
    int n;
    if (len <= 0) {
        return;
    }
    n = sTx->send(GDX_MP_PEER_ANY, pkt, len);
    if (n < 0) {
        sLastSendErr = n;
    }
    if (n > 0) {
        sTxPackets++;
        sTxBytes += (u32) n;
        sWinTxBytes += (u32) n;
    }
}

static const char* ReasonName(int r) {
    switch (r) {
        case GDX_MP_DROP_R_TIMEOUT:
            return "timeout";
        case GDX_MP_DROP_R_LEFT:
            return "left";
        case GDX_MP_DROP_R_LINK:
            return "link";
        case GDX_MP_DROP_R_FORCED:
            return "forced";
        default:
            return "?";
    }
}

static void BeginDrop(int player, int reason, u32 silentMs) {
    if (sDropMode == MP_DROP_OFF) {
        return;
    }
    if (gdx_mp_drop_begin(&sDrop, sLs, player, reason)) {
        MpLog("[mp] drop: removing player %d (%s, %lu ms silent, known=%lu) at local tick=%llu", player,
              ReasonName(reason), (unsigned long) silentMs, (unsigned long) sLs->known[player], sLastTick);
        gdx3ds_filelog_flush();
    }
}

/* Drop-out protocol: send what is due, report commits, end the session if it ended for us. */
static int ServiceDrop(void) {
    u8 pkt[MP_PKT_CAP];
    const u32 now = NowMs();
    int guard;
    int p;
    if (sLs == NULL) {
        return 0;
    }
    /* A joiner's LEAVE reaches the host through the ingest (it queues the removal itself): note it. */
    if (sIsHost && sDrop.pending >= 0 && sDrop.reason[sDrop.pending] == GDX_MP_DROP_R_LEFT && !sDrop.sentOnce) {
        MpLog("[mp] drop: player %d said goodbye (LEAVE) at local tick=%llu", sDrop.pending, sLastTick);
    }
    for (guard = 0; guard < 4; guard++) {
        int dest = GDX_MP_DROP_DEST_ALL;
        const int len = gdx_mp_drop_service(&sDrop, sLs, now, pkt, sizeof(pkt), &dest);
        if (len <= 0) {
            break;
        }
        sTx->send(dest, pkt, len);
    }
    for (p = 0; p < sLs->numPlayers; p++) {
        if (sDrop.newCommits & (1u << p)) {
            MpLog("[mp] DROP player %d removed at tick=%lu (input tick %llu) reason=%s: neutral pad from there%s; "
                  "players left=%d", p, (unsigned long) sDrop.removedAt[p],
                  (unsigned long long) sDrop.removedAt[p] + sStartTick, ReasonName(sDrop.reason[p]),
                  sDropMode == MP_DROP_RETIRE ? ", machine retires"
                                              : (sDropMode == MP_DROP_CPU ? ", the CPU drives its machine" : ""),
                  __builtin_popcount(gdx_mp_drop_live_mask(&sDrop)));
            gdx3ds_filelog_flush();
        }
    }
    sDrop.newCommits = 0;
    if (sDrop.selfRemoved) {
        Disable("the host removed this console from the session");
        return -1;
    }
    if (sDrop.hostLeft) {
        Disable("the host left the session");
        return -1;
    }
    if (sDrop.fatal) {
        Disable("drop-out could not be reconciled (a gap past the 16-pad tail)");
        return -1;
    }
    return 0;
}

/* Who is gone: the medium's word (UDS node list) and, on the host, silence. */
static int CheckPresence(u32 now, int blocked) {
    int p;
    if (sLs == NULL || !sSessionStarted || !sActive) {
        return 0;
    }
    if (sTx->present_mask != NULL && (u32) (now - sLastPresenceMs) >= 100) {
        const unsigned int present = sTx->present_mask();
        sLastPresenceMs = now;
        if (!sIsHost && !(present & 1u)) {
            Disable("the host left the network");
            return -1;
        }
        for (p = 1; sIsHost && p < sLs->numPlayers; p++) {
            if (sPeerSeen[p] && !(present & (1u << p)) && !gdx_mp_drop_refuse_from(&sDrop, p)) {
                BeginDrop(p, GDX_MP_DROP_R_LINK, now - sLastHeardMs[p]);
            }
        }
    }
    if (sIsHost && blocked) {
        for (p = 1; p < sLs->numPlayers; p++) {
            const u32 limit = sPeerSeen[p] ? (u32) sDropMs : (u32) sConnectMs;
            const u32 since = sPeerSeen[p] ? sLastHeardMs[p] : sSessionStartMs;
            if (gdx_mp_drop_refuse_from(&sDrop, p)) {
                continue;
            }
            if ((u32) (now - since) > limit) {
                BeginDrop(p, GDX_MP_DROP_R_TIMEOUT, now - since);
            }
        }
    }
    return 0;
}

static void Pump(void) {
    u8 buf[GDX_MP_TRANSPORT_MTU];
    int guard;
    if (sLs == NULL || !sActive) {
        return;
    }
    for (guard = 0; guard < 64; guard++) {
        int hint = GDX_MP_PEER_ANY;
        const int n = sTx->recv(buf, sizeof(buf), &hint);
        int from;
        if (n <= 0) {
            if (n < 0) {
                sLastRecvErr = n;
            }
            return;
        }
        sLastRxMs = NowMs();
        sRxPackets++;
        sRxBytes += (u32) n;
        sWinRxBytes += (u32) n;
        if (gdx_mp_drop_packet_type(buf, n) > 0) {
            if (gdx_mp_drop_ingest(&sDrop, sLs, buf, n, NowMs()) > 0 && buf[6] < GDX_MP_MAX_PLAYERS) {
                sLastHeardMs[buf[6]] = NowMs();
            }
            continue; /* the drop protocol is host <-> each peer: never relayed */
        }
        from = gdx_mp_ls_packet_from(buf, n);
        if (from >= 0 && gdx_mp_drop_refuse_from(&sDrop, from)) {
            sRefusedFrozen++;
            continue;
        }
        gdx_mp_drop_note_input_flags(&sDrop, sLs, buf, n);
        from = gdx_mp_ls_ingest(sLs, buf, n, NowMs());
        if (from < 0) {
            continue;
        }
        sLastHeardMs[from] = NowMs();
        if (sIsHost && hint != from) {
            /* First valid packet from this player (or it moved): remember where it lives. Only
             * the host binds: in a star a joiner hears every other player THROUGH the host, so
             * the source address of a relayed packet is the host's, not the player's. */
            sTx->bind_last_source(from);
        }
        if (!sPeerSeen[from]) {
            sPeerSeen[from] = 1;
            MpLog("[mp] peer %d joined (first valid packet at local tick=%llu)", from, sLastTick);
        }
        sAnyRemotePad = 1;
        if (sRelay) {
            int p;
            for (p = 1; p < sLs->numPlayers; p++) {
                if (p != from && !gdx_mp_drop_refuse_from(&sDrop, p) && sTx->send(p, buf, n) > 0) {
                    sRelayed++;
                }
            }
        }
        ReportDesyncOnce();
    }
}

static void StatsReceipt(unsigned long long tick) {
    int p;
    char peers[200];
    int off = 0;
    peers[0] = '\0';
    for (p = 0; p < sLs->numPlayers && off < (int) sizeof(peers) - 48; p++) {
        const GdxMpPeerStats* s = &sLs->peer[p];
        if (p == sLs->localPlayer) {
            continue;
        }
        off += snprintf(peers + off, sizeof(peers) - (size_t) off, " p%d:pk=%lu rtt=%lu/%lu/%lu hc=%lu%s", p,
                        (unsigned long) s->packets, (unsigned long) s->rttLastMs,
                        (unsigned long) (s->rttSamples ? s->rttSumMs / s->rttSamples : 0),
                        (unsigned long) s->rttMaxMs, (unsigned long) s->hashesChecked,
                        (sDrop.removedMask & (1u << p)) ? "(removed)" : "");
    }
    MpLog("[mp] stats tick=%llu mode=%d win=%lu stall_ticks=%lu stall_ms=%lu stall_max_ms=%lu "
          "tx_Bps=%lu rx_Bps=%lu tot_tx=%lu/%lu tot_rx=%lu/%lu relayed=%lu rej=%lu/%lu desync=%d removed=0x%02x%s",
          tick, gdx_input_game_mode(), (unsigned long) sWinTicks, (unsigned long) sWinStallTicks,
          (unsigned long) (sWinStallUs / 1000u), (unsigned long) (sWinStallMaxUs / 1000u),
          (unsigned long) (sWinTicks ? (u64) sWinTxBytes * 60u / sWinTicks : 0),
          (unsigned long) (sWinTicks ? (u64) sWinRxBytes * 60u / sWinTicks : 0), (unsigned long) sTxPackets,
          (unsigned long) sTxBytes, (unsigned long) sRxPackets, (unsigned long) sRxBytes,
          (unsigned long) sRelayed, (unsigned long) sLs->rejected, (unsigned long) sLs->sessionRejected,
          sLs->desynced, (unsigned) sDrop.removedMask, peers);
    if (sUds) {
        char u[240];
        gdx3ds_mp_uds_describe(u, sizeof(u));
        MpLog("[uds] stats %s", u);
    }
    sWinStallUs = 0;
    sWinStallTicks = 0;
    sWinStallMaxUs = 0;
    sWinTxBytes = 0;
    sWinRxBytes = 0;
    sWinTicks = 0;
}

extern int gdx_mp_local_bgm_ready(void) __attribute__((weak)); /* racer.c (decomp-3ds-mp-ekhold.patch) */

static GdxMpPad LocalPad(unsigned short buttons, signed char stickX, signed char stickY) {
    GdxMpPad pad;
    pad.buttons = (unsigned short) (buttons & ~GDX_MP_PAD_BGM_READY);
    pad.stickX = stickX;
    pad.stickY = stickY;
    if (sXform == 1 && gdx_input_in_gameplay()) {
        pad.stickX = (signed char) ((stickX == -128) ? 127 : -stickX);
    }
    if (&gdx_mp_local_bgm_ready == 0 || gdx_mp_local_bgm_ready()) {
        pad.buttons |= GDX_MP_PAD_BGM_READY; /* MP30: the EK countdown hold (gdx3ds_autodrive.c) */
    }
    return pad;
}

/* Waits (pumping the transport, re-sending our window, running the drop-out protocol) until
 * every pad for `tick` is in. Returns 0 when ready, -1 when the session ended for us. */
static int WaitReady(u32 tick) {
    const u64 start = svcGetSystemTick();
    const u32 startMs = NowMs();
    const int limitMs = sAnyRemotePad ? sWaitMs : sConnectMs;
    u32 lastSendMs = startMs;
    u32 lastNoteMs = startMs;
    u32 waitedUs;

    Pump();
    gdx_mp_drop_fill(&sDrop, sLs, tick);
    while (!gdx_mp_ls_ready(sLs, tick)) {
        const u32 now = NowMs();
        const int missing = gdx_mp_ls_first_missing(sLs, tick);
        /* With drop-out on, only the host decides that a JOINER is gone: the host never gives
         * up on its own, and a joiner gives up on a missing joiner only after the host had
         * every chance to remove it (on the host itself: at once, after its own wait). */
        const int hostDecides = (sDropMode != MP_DROP_OFF);
        const int giveUpMs = (!hostDecides || missing == 0) ? limitMs : limitMs + 4 * sDropMs;
        if (ServiceDrop() != 0 || CheckPresence(now, 1) != 0 || !sActive) {
            return -1;
        }
        if (!(hostDecides && sIsHost) && (int) (now - startMs) > giveUpMs) {
            char why[96];
            snprintf(why, sizeof(why), "no pad from player %d for tick %lu in %d ms", missing, (unsigned long) tick,
                     giveUpMs);
            Disable(why);
            return -1;
        }
        if (sReopenMs > 0 && sAnyRemotePad && now - sLastRxMs > (u32) sReopenMs && now - sLastReopenMs > (u32) sReopenMs) {
            /* Nothing heard for a while although the peer was alive: re-create the endpoint
             * (keeps learned addresses). Seen on the emulator lane: one direction of a
             * loopback pair went silent while the other kept flowing. */
            const int rrc = sTx->reopen();
            sReopens++;
            sLastReopenMs = now;
            MpLog("[mp] transport reopen #%lu rc=%d (no rx for %lu ms at tick=%lu)", (unsigned long) sReopens, rrc,
                  (unsigned long) (now - sLastRxMs), (unsigned long) tick);
        }
        if (now - lastSendMs >= 16) {
            SendOurs(); /* keep the window flowing while we wait: our ack may be what they lack */
            lastSendMs = now;
        }
        if (now - lastNoteMs >= 5000) {
            MpLog("[mp] waiting tick=%lu for player %d (%lu ms)", (unsigned long) tick, missing,
                  (unsigned long) (now - startMs));
            StateLine("wait");
            lastNoteMs = now;
        }
        svcSleepThread(500000LL); /* 0.5 ms */
        Pump();
        gdx_mp_drop_fill(&sDrop, sLs, tick);
    }
    waitedUs = (u32) MP_TICK_US(svcGetSystemTick() - start);
    sStallUsTotal += waitedUs;
    sWinStallUs += waitedUs;
    if (waitedUs > 200) {
        sStallTicks++;
        sWinStallTicks++;
    }
    if (waitedUs > sStallMaxUs) {
        sStallMaxUs = waitedUs;
    }
    if (waitedUs > sWinStallMaxUs) {
        sWinStallMaxUs = waitedUs;
    }
    return 0;
}

/* UDS joiner reached the session start before it joined a room: wait here (the game holds). */
static int WaitJoined(void) {
    const u32 start = NowMs();
    u32 lastNote = start;
    while (sPendingJoin && sActive) {
        const u32 now = NowMs();
        PollJoin();
        if (!sPendingJoin || !sActive) {
            break;
        }
        if ((int) (now - sJoinStartMs) > sConnectMs) {
            Disable("no compatible room nearby (mp_connect_s)");
            break;
        }
        if (now - lastNote >= 5000) {
            char why[96];
            const int st = gdx3ds_mp_uds_join_state(why, sizeof(why));
            MpLog("[mp] waiting for a room (state=%d %s) %lu ms", st, why, (unsigned long) (now - start));
            lastNote = now;
        }
        svcSleepThread(20 * 1000000LL);
    }
    return (sActive && sLs != NULL) ? 0 : -1;
}

/* Removed players' machines retire at the first race tick >= T (deterministic: every console
 * calls this at the same lockstep tick with the same simulation state). */
static void RetireRemoved(u32 lt) {
    int p;
    if (sDropMode != MP_DROP_RETIRE || sDrop.removedMask == 0) {
        return;
    }
    for (p = 0; p < sLs->numPlayers; p++) {
        if (gdx_mp_drop_is_removed(&sDrop, p, lt) && gdx_mp_retire_player(p)) {
            MpLog("[mp] retired player %d's machine at tick=%lu (input tick %llu) mode=%d", p, (unsigned long) lt,
                  (unsigned long long) lt + sStartTick, gdx_input_game_mode());
        }
    }
}

int gdx_mp_exchange(unsigned long long tick, unsigned short* buttons, signed char* stickX, signed char* stickY,
                    unsigned char* connected, int capacity) {
    GdxMpPad pads[GDX_MP_MAX_PLAYERS];
    int p;
    int numPlayers;

    if (!sActive) {
        return gdx3ds_mpo_enabled() ? gdx3ds_mpo_exchange(tick, buttons, stickX, stickY, connected, capacity) : 0;
    }
    if (tick < sStartTick) {
        /* Before the session starts every console feeds NEUTRAL pads to every player port, so
         * the boots stay identical without being locked together in wall time (a console that
         * blocked at tick 2 for a peer still booting would stall the emulator lane's boot lock,
         * and on hardware it is the lobby that starts a session, not the boot). */
        memset(pads, 0, sizeof(pads));
        sHaveTick = 0;
    } else {
        u32 lt;
        if (sPendingJoin && WaitJoined() != 0) {
            return 0;
        }
        lt = (u32) (tick - sStartTick); /* lockstep ticks count from the session start */
        if (!sSessionStarted) {
            sSessionStarted = 1;
            sSessionStartMs = NowMs();
            for (p = 0; p < GDX_MP_MAX_PLAYERS; p++) {
                sLastHeardMs[p] = sSessionStartMs;
            }
        }
        if (sLeaveAt >= 0 && (long) lt == sLeaveAt) {
            Leave("mp_leave_at lever");
            return 0;
        }
        sLastTick = tick;
        sHaveTick = 1;
        gdx_mp_ls_push_local(sLs, lt, LocalPad(buttons[0], stickX[0], stickY[0]));
        SendOurs();
        if (WaitReady(lt) != 0) {
            return 0;
        }
        gdx_mp_ls_get(sLs, lt, pads);
        sSimLt = lt;
        RetireRemoved(lt);
        sBgmReadyAll = 1;
        for (p = 0; p < sLs->numPlayers; p++) {
            if (!(pads[p].buttons & GDX_MP_PAD_BGM_READY) && !gdx_mp_drop_is_removed(&sDrop, p, lt)) {
                sBgmReadyAll = 0;
            }
            pads[p].buttons = (uint16_t) (pads[p].buttons & ~GDX_MP_PAD_BGM_READY);
        }
    }
    numPlayers = (sLs != NULL) ? sLs->numPlayers : 1;
    for (p = 0; p < capacity; p++) {
        const int src = (p < numPlayers) ? p : 0;
        if (p < numPlayers) {
            connected[p] = 1;
        }
        if (connected[p]) {
            buttons[p] = pads[src].buttons;
            stickX[p] = pads[src].stickX;
            stickY[p] = pads[src].stickY;
        }
    }
    sWinTicks++;
    return 1;
}

/* uds host: once every player's node is on the network, stop new connections and advertise the
 * room as racing (the beacon greys it out). Not at the session start: the joiners connect
 * while the host's first lockstep tick is already waiting for them. */
static void CloseRoomWhenFull(void) {
    if (!sUds || !sIsHost || sRoomClosed) {
        return;
    }
    if (gdx3ds_mp_uds_node_count() >= sLs->numPlayers) {
        sRoomClosed = 1;
        gdx3ds_mp_uds_set_racing(1);
    }
}

/* Every frame, blocked or not: the join, the probe, the drop-out protocol, presence. */
static void ServiceBackground(void) {
    const u32 now = NowMs();
    if (sPendingJoin) {
        PollJoin();
        if (!sActive) {
            return;
        }
    }
    if (sUds && sProbeS > 0 && !sProbeDone) {
        if (sIsHost ? (gdx3ds_mp_uds_node_count() >= 2) : (!sPendingJoin && sLs != NULL)) {
            sProbeDone = 1;
            gdx3ds_mp_uds_probe(sIsHost, sProbeS);
        }
    }
    if (sLs == NULL || !sActive) {
        return;
    }
    CloseRoomWhenFull();
    Pump();
    if (ServiceDrop() != 0) {
        return;
    }
    CheckPresence(now, 0);
}

void gdx3ds_mp_after_tick(void) {
    unsigned int hash;
    int racers = 0;
    int mode;
    if (!sActive) {
        gdx3ds_mpo_after_tick(); /* mp-online: inert (first int test) unless [net] rendezvous / mp_lan */
        return;
    }
    ServiceBackground();
    if (!sActive || !sHaveTick || sLs == NULL) {
        return;
    }
    if (sDesyncAt >= 0 && sLastTick == (unsigned long long) sDesyncAt) {
        gRandSeed1 ^= 1; /* test lever: a one-bit divergence in the simulation's RNG */
        MpLog("[mp] desync lever: flipped gRandSeed1 bit 0 after tick=%llu", sLastTick);
    }
    hash = gdx_gstate_hash(&racers);
    hash = gdx_mpo_vis_hash(hash);
    mode = gdx_input_game_mode();
    hash = Fnv(hash, &mode, sizeof(mode)); /* the menu walk too: [gstate] alone omits the mode */
    if ((long) sLastTick >= sHashFrom) {
        gdx_mp_ls_note_local_hash(sLs, (u32) (sLastTick - sStartTick), hash);
    }
    ReportDesyncOnce();
    if (((sLastTick + 1) % (unsigned long long) sStatsEvery) == 0) {
        StatsReceipt(sLastTick);
    }
}
