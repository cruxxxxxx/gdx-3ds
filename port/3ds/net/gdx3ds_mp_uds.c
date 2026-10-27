/* port/3ds/net/gdx3ds_mp_uds.c — mp-uds: the 3DS local wireless transport + NEARBY discovery.
 * Contract, IDs, crypto and limits: gdx3ds_mp_uds.h. Transport semantics: gdx3ds_mp_transport.h.
 * Beacon format (pure, host-tested): port/gdx_mp_beacon.h. */
#include "gdx3ds_mp_uds.h"

#include "gdx3ds_config.h"
#include "gdx3ds_filelog.h"
#include "gdx3ds_mp_ipcstat.h"
#include "gdx_mp_beacon.h"
#include "gdx_mp_lockstep.h" /* GDX_MP_MAX_PLAYERS */
#include "gdx_mp_txq.h"

#include <3ds.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UDS_QUEUE_FULL 0xC86113F0u /* udsSendTo: not sent, not fatal (UDS_CHECK_SENDTO_FATALERROR) */
#define STATUS_REFRESH_MS 250
#define WORKER_STACK (32 * 1024)
#define SCAN_PERIOD_MS 400

static int sUdsUp = 0;
static int sIsHost = 0;
static int sLocalPlayer = 0;
static int sNumPlayers = 0;
static int sBound = 0;
static udsBindContext sBind;
static u16 sPlayerNode[GDX_MP_MAX_PLAYERS];
static u16 sLastSrcNode = 0;
static u16 sLocalNode = 0;
static u8 sFrame[UDS_DATAFRAME_MAXSIZE];
static char sPass[GDX_MP_UDS_PASSPHRASE_LEN + 1];
static char sBuild[GDX_MP_BEACON_BUILD_BYTES];
static uint32_t sContentHash = 0;
static uint32_t sBuildHash = 0;
static int sProto = GDX_MP_BEACON_PROTO; /* mp-online sets its own (gdx3ds_mp_uds_set_proto) */

/* Host. */
static int sHosting = 0;
static GdxMpBeacon sHostBeacon;
static int sHostNodes = 1;

/* Connection status cache. */
static udsConnectionStatus sStatus;
static u32 sStatusMs = 0;
static int sHaveStatus = 0;

/* Stats. */
static u32 sTxFrames = 0, sTxBytes = 0, sTxQueueFull = 0, sTxFatal = 0;
static u32 sRxFrames = 0, sRxBytes = 0, sRxErr = 0, sRxOversize = 0;
static Result sLastTxRc = 0, sLastRxRc = 0;

/* NETTICK rx gate ([net] mp_uds_rx_gate, default 1): udsPullPacket is an IPC to NWM, and on the
 * Old 3DS NWM shares core 1 with the game's audio threads, so an empty pull can cost
 * milliseconds. The bind event NWM signals on every received frame is a kernel object:
 * svcWaitSynchronization(event, 0) answers "anything new?" without the sysmodule. Pull only when
 * it is signalled, then keep pulling until a pull comes back empty (the check clears the event
 * first, so a frame that lands during the drain re-signals it). 0 = pull on every call (90049af). */
static int sRxGate = -1;
static int sRxMaybe = 1; /* the event fired and the drain has not seen an empty pull since */
/* Gate safety net: while gated, one pull every RX_PROBE_MS anyway. If such a probe finds a frame
 * the event never announced, the event cannot be trusted on this console: the gate turns itself
 * off for the rest of the run (logged once) instead of starving the lockstep. */
#define RX_PROBE_MS 250
static u32 sRxProbeMs = 0;
static int sRxProbing = 0;

/* Discovery / joining (worker thread). */
typedef struct UdsRoom {
    GdxMpUdsRoom room;
    udsNetworkStruct net;
    int used;
} UdsRoom;

static LightLock sLock;
static int sLockInit = 0;
static Thread sWorker = NULL;
static volatile int sWorkerQuit = 0;
/* MPFIX release: the worker leaves its loop and udsExit()s itself (it may be inside a scan, so
 * the main thread never waits for it); gdx3ds_mp_uds_release() reaps it on a later frame. */
static volatile int sReleaseWanted = 0;
static volatile int sWorkerReleased = 0;
static int sAtexitSet = 0;
static volatile int sScanning = 0;
static volatile int sJoinState = GDX_UDS_JOIN_IDLE;
static volatile int sJoinWant = 0;   /* 1 = connect to sJoinMac */
static u8 sJoinMac[6];
static int sAutoJoin = 0;
static char sAutoName[GDX_MP_BEACON_NAME_BYTES];
static u32 sAutoDeadlineMs = 0;
static char sJoinWhy[128];
static UdsRoom sRooms[GDX_MP_UDS_NEARBY_MAX];
static udsNetworkStruct sJoinedNet;
static GdxMpBeacon sJoinedBeacon;
static u32 sScans = 0;
static Result sLastScanRc = 0;
static void* sScanBuf = NULL;
static int sRawLogged = 0;

static void SvcStop(void);

static void UdsLog(const char* fmt, ...) {
    char line[320];
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

static void EnsureLock(void) {
    if (!sLockInit) {
        LightLock_Init(&sLock);
        sLockInit = 1;
    }
}

/* ---- errors -------------------------------------------------------------------------------- */

/* OS_SharedConfig->network_state is 7 while the wireless hardware is disabled (switch off /
 * airplane mode); logged raw either way so the hardware run can confirm the mapping. */
static int WirelessDisabled(void) {
    return OS_SharedConfig->network_state == 7;
}

static int Explain(const char* what, Result rc, int code, char* why, int cap) {
    const int off = WirelessDisabled();
    UdsLog("[uds] %s FAILED rc=0x%08lX network_state=%u wifi_strength=%u", what, (unsigned long) rc,
           (unsigned) OS_SharedConfig->network_state, (unsigned) OS_SharedConfig->wifi_strength);
    if (why != NULL && cap > 0) {
        if (off) {
            snprintf(why, (size_t) cap,
                     "Wireless is off. Turn the wireless switch on (and leave Airplane Mode), then try again.");
        } else {
            snprintf(why, (size_t) cap,
                     "Local wireless unavailable (%s, NWM 0x%08lX). Check the wireless switch is on.", what,
                     (unsigned long) rc);
        }
    }
    return off ? GDX_UDS_E_WIRELESS_OFF : code;
}

/* ---- init / profile ------------------------------------------------------------------------- */

static void UdsShutdown(void) {
    if (!sUdsUp) {
        return;
    }
    SvcStop();
    sWorkerQuit = 1;
    if (sWorker != NULL) {
        threadJoin(sWorker, 3000000000ULL);
        threadFree(sWorker);
        sWorker = NULL;
    }
    if (sWorkerReleased) {
        sWorkerReleased = 0;
        sUdsUp = 0; /* the worker already left UDS (a release in flight) */
        return;
    }
    if (sHosting) {
        udsDestroyNetwork();
        sHosting = 0;
    } else if (sJoinState == GDX_UDS_JOIN_CONNECTED) {
        udsDisconnectNetwork();
    }
    if (sBound) {
        udsUnbind(&sBind);
        sBound = 0;
    }
    udsExit();
    sUdsUp = 0;
}

int gdx3ds_mp_uds_init(char* why, int cap) {
    Result rc;
    if (sUdsUp) {
        return GDX_UDS_OK;
    }
    EnsureLock();
    rc = udsInit(GDX_UDS_SHAREDMEM_SIZE, NULL);
    if (R_FAILED(rc)) {
        return Explain("udsInit", rc, GDX_UDS_E_INIT, why, cap);
    }
    sUdsUp = 1;
    if (!sAtexitSet) {
        atexit(UdsShutdown); /* once: NEARBY may init and release UDS many times */
        sAtexitSet = 1;
    }
    UdsLog("[uds] init ok sharedmem=0x%X wlancommID=0x%08lX id8=0x%02X network_state=%u", GDX_UDS_SHAREDMEM_SIZE,
           (unsigned long) GDX_UDS_WLANCOMMID, GDX_UDS_ID8, (unsigned) OS_SharedConfig->network_state);
    return GDX_UDS_OK;
}

void gdx3ds_mp_uds_profile(char* name, int cap, int* model) {
    udsNodeInfo ni;
    u8 m = GDX_MP_MODEL_UNKNOWN;
    if (name != NULL && cap > 0) {
        name[0] = '\0';
        memset(&ni, 0, sizeof(ni));
        if (R_SUCCEEDED(udsGenerateNodeInfo(&ni, NULL))) {
            gdx_mp_name_from_utf16(ni.username, 10, name, cap);
        }
        if (name[0] == '\0') {
            snprintf(name, (size_t) cap, "%s", "3DS");
        }
    }
    if (model != NULL) {
        if (R_SUCCEEDED(cfguInit())) {
            if (R_FAILED(CFGU_GetSystemModel(&m))) {
                m = GDX_MP_MODEL_UNKNOWN;
            }
            cfguExit();
        }
        *model = m;
    }
}

static void SetIdentity(const char* build, uint32_t contentHash) {
    snprintf(sBuild, sizeof(sBuild), "%s", build != NULL ? build : "unknown");
    sContentHash = contentHash;
    sBuildHash = gdx_mp_beacon_build_hash(build);
    gdx_mp_uds_passphrase(build, contentHash, sProto, sPass);
}

/* ---- connection status ---------------------------------------------------------------------- */

static void RefreshStatus(int force) {
    const u32 now = NowMs();
    if (!sUdsUp || (!sHosting && sJoinState != GDX_UDS_JOIN_CONNECTED)) {
        return;
    }
    if (!force && sHaveStatus && !udsWaitConnectionStatusEvent(false, false) &&
        (u32) (now - sStatusMs) < STATUS_REFRESH_MS * 4) {
        return;
    }
    if (R_SUCCEEDED(udsGetConnectionStatus(&sStatus))) {
        sHaveStatus = 1;
        sStatusMs = now;
        if (sStatus.cur_NetworkNodeID != 0) {
            sLocalNode = sStatus.cur_NetworkNodeID;
        }
        if (sHosting && sStatus.total_nodes != sHostNodes) {
            sHostNodes = sStatus.total_nodes;
            UdsLog("[uds] host: nodes=%d bitmask=0x%04X", sHostNodes, (unsigned) sStatus.node_bitmask);
            if (sHostNodes >= 1 && sHostNodes <= GDX_MP_BEACON_MAX_PLAYERS) {
                sHostBeacon.players = (uint8_t) sHostNodes;
                gdx3ds_mp_uds_host_beacon(NULL);
            }
        }
    }
}

int gdx3ds_mp_uds_node_count(void) {
    RefreshStatus(0);
    return sHosting ? sHostNodes : (sHaveStatus ? sStatus.total_nodes : 0);
}

int gdx3ds_mp_uds_local_node(void) {
    return sLocalNode;
}

/* ---- host ----------------------------------------------------------------------------------- */

static int PublishBeacon(void) {
    uint8_t appdata[GDX_MP_BEACON_APPDATA_MAX];
    const int n = gdx_mp_beacon_encode(&sHostBeacon, appdata, sizeof(appdata));
    Result rc;
    if (n <= 0) {
        return -1;
    }
    rc = udsSetApplicationData(appdata, (size_t) n);
    if (R_FAILED(rc)) {
        UdsLog("[uds] udsSetApplicationData rc=0x%08lX", (unsigned long) rc);
        return -1;
    }
    return 0;
}

int gdx3ds_mp_uds_host_beacon(const GdxMpBeacon* b) {
    udsNetworkStruct net;
    Result rc;
    if (b != NULL) {
        sHostBeacon = *b;
    }
    if (sHosting) {
        return PublishBeacon();
    }
    if (!sUdsUp) {
        return GDX_UDS_E_INIT;
    }
    udsGenerateDefaultNetworkStruct(&net, GDX_UDS_WLANCOMMID, GDX_UDS_ID8,
                                    (u8) (sHostBeacon.maxPlayers > 0 ? sHostBeacon.maxPlayers : GDX_UDS_MAX_NODES));
    rc = udsCreateNetwork(&net, sPass, strlen(sPass), &sBind, GDX_UDS_DATA_CHANNEL, UDS_DEFAULT_RECVBUFSIZE);
    if (R_FAILED(rc)) {
        return Explain("udsCreateNetwork", rc, GDX_UDS_E_CREATE, NULL, 0);
    }
    sBound = 1;
    sRxMaybe = 1; /* NETTICK: a fresh bind may already hold frames */
    sHosting = 1;
    sLocalNode = UDS_HOST_NETWORKNODEID;
    sHostNodes = 1;
    PublishBeacon();
    {
        u8 ch = 0;
        udsGetChannel(&ch);
        UdsLog("[uds] hosting: \"%s\" max=%u model=%s channel=%u build=%s content=%08lx room=%08lx appdata=%dB",
               sHostBeacon.name, (unsigned) sHostBeacon.maxPlayers, gdx_mp_model_name(sHostBeacon.model),
               (unsigned) ch, sHostBeacon.build, (unsigned long) sHostBeacon.contentHash,
               (unsigned long) sHostBeacon.roomId, GDX_MP_BEACON_BYTES);
    }
    return GDX_UDS_OK;
}

void gdx3ds_mp_uds_set_racing(int racing) {
    Result rc;
    int pub;
    if (!sHosting) {
        return;
    }
    /* CRASHFIX: bracketed (hw b5f620a: the New 3DS host died between "phase room -> launch" and
     * this receipt; with [debug] filelog_launch_sync_ms the "enter" line is on the card first). */
    UdsLog("[uds] host: %s enter", racing ? "race on" : "lobby");
    rc = udsSetNewConnectionsBlocked(racing ? true : false, true, false);
    sHostBeacon.flags = (uint8_t) (racing ? GDX_MP_BEACON_F_RACING : GDX_MP_BEACON_F_OPEN);
    pub = PublishBeacon();
    UdsLog("[uds] host: %s (block rc=0x%08lX beacon=%d)", racing ? "race on: new connections blocked" : "lobby: open",
           (unsigned long) rc, pub);
}

/* ---- scanning / joining (worker) ------------------------------------------------------------ */

static void NoteRoom(const udsNetworkScanInfo* info, u32 now) {
    uint8_t appdata[GDX_MP_BEACON_APPDATA_MAX];
    size_t actual = 0;
    GdxMpBeacon b;
    int slot = -1;
    int oldest = 0;
    int i;
    if (R_FAILED(udsGetNetworkStructApplicationData(&info->network, appdata, sizeof(appdata), &actual)) ||
        gdx_mp_beacon_decode(appdata, (int) actual, &b) != GDX_MP_BEACON_OK) {
        return; /* not one of ours, or junk: never listed */
    }
    if (!sRawLogged) {
        /* Which byte of the reply entry carries the signal is undocumented: dump them once. */
        const nwmBeaconDataReplyEntry* e = &info->datareply_entry;
        sRawLogged = 1;
        UdsLog("[uds] scan entry raw: x4=%02X ch=%u x6=%02X x7=%02X xe=%02X%02X%02X%02X%02X%02X x14=%08lX wifi=%u",
               e->unk_x4, (unsigned) e->channel, e->unk_x6, e->unk_x7, e->unk_xe[0], e->unk_xe[1], e->unk_xe[2],
               e->unk_xe[3], e->unk_xe[4], e->unk_xe[5], (unsigned long) e->unk_x14,
               (unsigned) OS_SharedConfig->wifi_strength);
    }
    LightLock_Lock(&sLock);
    for (i = 0; i < GDX_MP_UDS_NEARBY_MAX; i++) {
        if (sRooms[i].used && memcmp(sRooms[i].room.handle, info->network.host_macaddress, 6) == 0) {
            slot = i;
            break;
        }
        if (!sRooms[i].used && slot < 0) {
            slot = i;
        }
        if (sRooms[i].room.lastSeenMs < sRooms[oldest].room.lastSeenMs) {
            oldest = i;
        }
    }
    if (slot < 0) {
        slot = oldest;
    }
    if (!sRooms[slot].used || memcmp(sRooms[slot].room.handle, info->network.host_macaddress, 6) != 0) {
        char label[96];
        gdx_mp_beacon_label(&b, -1, label, sizeof(label));
        UdsLog("[uds] nearby: row=\"%s\" build=%s compat=%s", label, b.build,
               gdx_mp_compat_text(gdx_mp_beacon_compat(&b, sBuildHash, sContentHash, sProto)));
    }
    memset(&sRooms[slot], 0, sizeof(sRooms[slot]));
    sRooms[slot].used = 1;
    sRooms[slot].net = info->network;
    sRooms[slot].room.medium = GDX_MP_UDS_MEDIUM_UDS;
    sRooms[slot].room.beacon = b;
    sRooms[slot].room.compat = gdx_mp_beacon_compat(&b, sBuildHash, sContentHash, sProto);
    sRooms[slot].room.signalBars = -1; /* see the raw dump: no documented RSSI field yet */
    sRooms[slot].room.lastSeenMs = now;
    memcpy(sRooms[slot].room.handle, info->network.host_macaddress, 6);
    LightLock_Unlock(&sLock);
}

static void ScanOnce(void) {
    udsNetworkScanInfo* nets = NULL;
    size_t total = 0;
    const u32 now = NowMs();
    size_t i;
    Result rc;
    if (sScanBuf == NULL) {
        sScanBuf = malloc(GDX_UDS_SCAN_BUF_SIZE);
        if (sScanBuf == NULL) {
            return;
        }
    }
    memset(sScanBuf, 0, GDX_UDS_SCAN_BUF_SIZE);
    rc = udsScanBeacons(sScanBuf, GDX_UDS_SCAN_BUF_SIZE, &nets, &total, GDX_UDS_WLANCOMMID, GDX_UDS_ID8, NULL, false);
    sScans++;
    if (R_FAILED(rc)) {
        if (rc != sLastScanRc) {
            UdsLog("[uds] udsScanBeacons rc=0x%08lX (scan #%lu)", (unsigned long) rc, (unsigned long) sScans);
        }
        sLastScanRc = rc;
        return;
    }
    sLastScanRc = rc;
    for (i = 0; i < total; i++) {
        NoteRoom(&nets[i], now);
    }
    free(nets);
}

/* The compatible auto-join target, if any (lock held by the caller). */
static int PickAutoTarget(u8* mac) {
    int i;
    for (i = 0; i < GDX_MP_UDS_NEARBY_MAX; i++) {
        const GdxMpUdsRoom* r = &sRooms[i].room;
        if (!sRooms[i].used || r->compat != GDX_MP_COMPAT_OK) {
            continue;
        }
        if (sAutoName[0] != '\0' && strcmp(r->beacon.name, sAutoName) != 0) {
            continue;
        }
        memcpy(mac, r->handle, 6);
        return 1;
    }
    return 0;
}

static void Connect(void) {
    udsNetworkStruct net;
    GdxMpBeacon b;
    int found = 0;
    int i;
    Result rc;
    LightLock_Lock(&sLock);
    for (i = 0; i < GDX_MP_UDS_NEARBY_MAX; i++) {
        if (sRooms[i].used && memcmp(sRooms[i].room.handle, sJoinMac, 6) == 0) {
            net = sRooms[i].net;
            b = sRooms[i].room.beacon;
            found = 1;
            break;
        }
    }
    LightLock_Unlock(&sLock);
    sJoinWant = 0;
    if (!found) {
        snprintf(sJoinWhy, sizeof(sJoinWhy), "%s", "that room is gone");
        sJoinState = GDX_UDS_JOIN_FAILED;
        return;
    }
    sJoinState = GDX_UDS_JOIN_CONNECTING;
    UdsLog("[uds] connecting to \"%s\" (%s, %u/%u)", b.name, gdx_mp_model_name(b.model), (unsigned) b.players,
           (unsigned) b.maxPlayers);
    rc = udsConnectNetwork(&net, sPass, strlen(sPass), &sBind, UDS_BROADCAST_NETWORKNODEID, UDSCONTYPE_Client,
                           GDX_UDS_DATA_CHANNEL, UDS_DEFAULT_RECVBUFSIZE);
    if (R_FAILED(rc)) {
        Explain("udsConnectNetwork", rc, GDX_UDS_E_CONNECT, sJoinWhy, sizeof(sJoinWhy));
        if (!WirelessDisabled()) {
            snprintf(sJoinWhy, sizeof(sJoinWhy), "could not join \"%s\" (NWM 0x%08lX: a different build?)", b.name,
                     (unsigned long) rc);
        }
        sJoinState = sAutoJoin ? GDX_UDS_JOIN_SEARCHING : GDX_UDS_JOIN_FAILED;
        svcSleepThread(1000 * 1000000LL); /* auto-join: back off before trying the room again */
        return;
    }
    sBound = 1;
    sRxMaybe = 1; /* NETTICK: a fresh bind may already hold frames */
    sJoinedNet = net;
    sJoinedBeacon = b;
    {
        uint8_t appdata[GDX_MP_BEACON_APPDATA_MAX];
        size_t actual = 0;
        GdxMpBeacon fresh;
        if (R_SUCCEEDED(udsGetApplicationData(appdata, sizeof(appdata), &actual)) &&
            gdx_mp_beacon_decode(appdata, (int) actual, &fresh) == GDX_MP_BEACON_OK) {
            sJoinedBeacon = fresh;
        }
    }
    if (R_SUCCEEDED(udsGetConnectionStatus(&sStatus))) {
        sHaveStatus = 1;
        sStatusMs = NowMs();
        sLocalNode = sStatus.cur_NetworkNodeID;
    }
    UdsLog("[uds] connected: node=%u nodes=%u bitmask=0x%04X delay=%u hash_every=%u", (unsigned) sLocalNode,
           (unsigned) sStatus.total_nodes, (unsigned) sStatus.node_bitmask, (unsigned) sJoinedBeacon.delay,
           (unsigned) sJoinedBeacon.hashEvery);
    sScanning = 0;
    sJoinState = GDX_UDS_JOIN_CONNECTED;
}

static void Worker(void* arg) {
    u32 lastScan = 0;
    (void) arg;
    while (!sWorkerQuit && !sReleaseWanted) {
        const u32 now = NowMs();
        if (sJoinState == GDX_UDS_JOIN_CONNECTED) {
            break; /* from here on the main thread owns every UDS call */
        }
        if (sJoinWant) {
            Connect();
            continue;
        }
        if (sScanning && (lastScan == 0 || (u32) (now - lastScan) >= SCAN_PERIOD_MS)) {
            lastScan = now;
            ScanOnce();
            if (sAutoJoin) {
                u8 mac[6];
                int pick;
                LightLock_Lock(&sLock);
                pick = PickAutoTarget(mac);
                LightLock_Unlock(&sLock);
                if (pick) {
                    memcpy(sJoinMac, mac, 6);
                    sJoinWant = 1;
                    continue;
                }
                if (sAutoDeadlineMs != 0 && (int) (now - sAutoDeadlineMs) > 0) {
                    snprintf(sJoinWhy, sizeof(sJoinWhy), "%s", "no compatible room nearby");
                    sJoinState = GDX_UDS_JOIN_FAILED;
                    sScanning = 0;
                }
            }
        }
        svcSleepThread(50 * 1000000LL);
    }
    if (sReleaseWanted && !sWorkerQuit) {
        udsExit(); /* the radio goes back to WiFi (the caller checked: no room, no join) */
        sWorkerReleased = 1;
    }
}

static int StartWorker(void) {
    s32 prio = 0x30;
    if (sWorker != NULL) {
        return 0;
    }
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    sWorkerQuit = 0;
    sWorker = threadCreate(Worker, NULL, WORKER_STACK, prio + 1 < 0x3F ? prio + 1 : 0x3F, -2, false);
    if (sWorker == NULL) {
        UdsLog("[uds] worker thread create FAILED");
        return GDX_UDS_E_THREAD;
    }
    return 0;
}

int gdx3ds_mp_uds_auto_join(const char* nameFilter, int timeoutMs) {
    snprintf(sAutoName, sizeof(sAutoName), "%s", nameFilter != NULL ? nameFilter : "");
    sAutoJoin = 1;
    sAutoDeadlineMs = timeoutMs > 0 ? NowMs() + (u32) timeoutMs : 0;
    sJoinWhy[0] = '\0';
    sJoinState = GDX_UDS_JOIN_SEARCHING;
    sScanning = 1;
    UdsLog("[uds] searching for a room (name=\"%s\", %d ms)", sAutoName, timeoutMs);
    return StartWorker();
}

int gdx3ds_mp_uds_join_state(char* why, int cap) {
    if (why != NULL && cap > 0) {
        snprintf(why, (size_t) cap, "%s", sJoinWhy);
    }
    return sJoinState;
}

const GdxMpBeacon* gdx3ds_mp_uds_joined_beacon(void) {
    return sJoinState == GDX_UDS_JOIN_CONNECTED ? &sJoinedBeacon : NULL;
}

/* ---- transport ops -------------------------------------------------------------------------- */

static int UdsOpen(const GdxMpTransportConfig* cfg) {
    int rc;
    char why[160];
    memset(sPlayerNode, 0, sizeof(sPlayerNode));
    sIsHost = cfg->isHost;
    sLocalPlayer = cfg->localPlayer;
    sNumPlayers = cfg->numPlayers;
    rc = gdx3ds_mp_uds_init(why, sizeof(why));
    if (rc != GDX_UDS_OK) {
        UdsLog("[uds] %s", why);
        return rc;
    }
    SetIdentity(cfg->build, cfg->contentHash);
    if (sIsHost) {
        GdxMpBeacon b;
        int model = GDX_MP_MODEL_UNKNOWN;
        memset(&b, 0, sizeof(b));
        gdx3ds_mp_uds_profile(b.name, sizeof(b.name), &model);
        b.version = GDX_MP_BEACON_VERSION;
        b.flags = GDX_MP_BEACON_F_OPEN;
        b.players = 1;
        b.maxPlayers = (uint8_t) (cfg->numPlayers > 1 ? cfg->numPlayers : GDX_UDS_MAX_NODES);
        b.model = (uint8_t) model;
        b.proto = GDX_MP_BEACON_PROTO;
        b.delay = (uint8_t) cfg->delay;
        b.hashEvery = (uint8_t) cfg->hashEvery;
        b.buildHash = sBuildHash;
        b.contentHash = sContentHash;
        b.roomId = (uint32_t) svcGetSystemTick() ^ 0x9E3779B9u;
        snprintf(b.build, sizeof(b.build), "%s", sBuild);
        sPlayerNode[0] = UDS_HOST_NETWORKNODEID;
        return gdx3ds_mp_uds_host_beacon(&b);
    }
    sPlayerNode[0] = UDS_HOST_NETWORKNODEID;
    if (sJoinState == GDX_UDS_JOIN_CONNECTED) {
        return 0; /* the lobby already joined a room */
    }
    return gdx3ds_mp_uds_auto_join(cfg->joinName, 0);
}

void gdx3ds_mp_uds_set_session(int numPlayers, int localPlayer) {
    sNumPlayers = numPlayers;
    sLocalPlayer = localPlayer;
}

static void UdsBindLastSource(int player) {
    if (player < 0 || player >= sNumPlayers || player >= GDX_MP_MAX_PLAYERS || player == sLocalPlayer) {
        return;
    }
    if (sPlayerNode[player] != sLastSrcNode) {
        UdsLog("[uds] player %d = node %u", player, (unsigned) sLastSrcNode);
    }
    sPlayerNode[player] = sLastSrcNode;
}

static int Live(void) {
    return sBound && (sHosting || sJoinState == GDX_UDS_JOIN_CONNECTED);
}

static int SendNode(u16 node, const void* buf, int len) {
    const uint64_t t0 = gdx3ds_ipcstat_begin();
    const Result rc = udsSendTo(node, GDX_UDS_DATA_CHANNEL, UDS_SENDFLAG_Default, buf, (size_t) len);
    gdx3ds_ipcstat_end(GDX_IPC_UDS_SEND, t0);
    sLastTxRc = rc;
    if (UDS_CHECK_SENDTO_FATALERROR(rc)) {
        sTxFatal++;
        return 0; /* a departed node, a dropped link: the session's drop logic decides */
    }
    if (R_FAILED(rc)) {
        sTxQueueFull++;
        return 0;
    }
    sTxFrames++;
    sTxBytes += (u32) len;
    return len;
}

/* ---- the UDS service thread ([net] mp_uds_thread) ----------------------------------------------
 * Hardware 4bfb302 (Old 3DS host, race): udsSendTo blocked the main thread 8.8 ms on average (max
 * 50 ms) about once a frame and udsPullPacket 2 ms about four times a frame: ~11 ms of a ~30 ms
 * frame, and the race ran at 33 ticks/s. With the lever on (default), one thread owns every
 * udsSendTo / udsPullPacket of the data channel while the link is live; the main thread only copies
 * datagrams into a tx ring and out of an rx ring (port/gdx_mp_txq.h, lock-free SPSC).
 *   - The thread sleeps in svcWaitSynchronizationN on the bind's receive event and a wake event the
 *     main thread signals per queued datagram. On the receive event it clears the event and pulls
 *     until a pull comes back empty (a frame landing meanwhile re-signals it).
 *   - Lockstep INPUT packets are coalesced per node (only the newest is sent; it carries every
 *     unacked pad) and sent at most every [net] mp_uds_tx_min_ms (default 33, 0 = every one at
 *     once). Everything else goes out at once, in order.
 *   - With nothing signalled for SVC_PROBE_MS it pulls once anyway; a frame the event did not
 *     announce switches the thread to polling every SVC_POLL_MS ("[uds] svc: ... polling").
 *   - Priority one above the main thread, on [net] mp_uds_thread_core (default -2: the app core).
 * Lockstep semantics are unchanged: the same packets (minus superseded INPUT copies), same pads. */
#define SVC_TX_SLOTS 16
/* hw 158a13e: rxdrop=39, all in the race's first 600 ticks: the New 3DS main thread drains the
 * ring every frame (Drain, up to 128) but its first race frame took 2.3 s (texture load), and the
 * peer's ~30 datagrams/s filled 32 slots. 96 slots (~142 KiB) ride out ~3 s without a drain. */
#define SVC_RX_SLOTS 96
#define SVC_STACK (16 * 1024)
#define SVC_PROBE_MS 250
#define SVC_POLL_MS 2
static GdxMpDgram sSvcTxSlots[SVC_TX_SLOTS];
static GdxMpDgram sSvcRxSlots[SVC_RX_SLOTS];
static GdxMpRing sSvcTx;
static GdxMpRing sSvcRx;
static GdxMpTxSched sSvcSched;
static u8 sSvcFrame[UDS_DATAFRAME_MAXSIZE];
static Thread sSvc = NULL;
static Handle sSvcWake = 0;
static volatile int sSvcQuit = 0;
static int sSvcLever = -1;  /* [net] mp_uds_thread (default 1) */
static int sSvcMinMs = 33;  /* [net] mp_uds_tx_min_ms */
static int sSvcCore = -2;   /* [net] mp_uds_thread_core */
static int sSvcFailed = 0;  /* the thread could not start: inline calls for the rest of the run */
static volatile int sSvcPolling = 0;
static volatile u32 sSvcQueued = 0, sSvcSent = 0, sSvcTxDrops = 0, sSvcRxFrames = 0, sSvcRxDrops = 0;
static volatile u32 sSvcPulls = 0, sSvcEmpty = 0, sSvcWakes = 0;

static int SvcActive(void) {
    return sSvc != NULL;
}

/* Thread: pull until empty; every frame into the rx ring. */
static void SvcDrainRx(void) {
    for (;;) {
        size_t actual = 0;
        u16 src = 0;
        const uint64_t t0 = gdx3ds_ipcstat_begin();
        const Result rc = udsPullPacket(&sBind, sSvcFrame, sizeof(sSvcFrame), &actual, &src);
        gdx3ds_ipcstat_end(GDX_IPC_UDS_PULL, t0);
        sSvcPulls++;
        sLastRxRc = rc;
        if (R_FAILED(rc)) {
            sRxErr++;
            return;
        }
        if (actual == 0) {
            sSvcEmpty++;
            return;
        }
        if (src == sLocalNode || src == 0) {
            continue; /* never our own broadcast */
        }
        sSvcRxFrames++;
        if (!gdx_mp_ring_push(&sSvcRx, src, sSvcFrame, (int) actual)) {
            sSvcRxDrops++;
        }
    }
}

/* Thread: one probe pull with nothing signalled. 1 = it found a frame the event did not announce. */
static int SvcProbe(void) {
    const u32 before = sSvcRxFrames;
    SvcDrainRx();
    return sSvcRxFrames != before && !udsWaitDataAvailable(&sBind, false, false);
}

/* Thread: queued datagrams out (INPUT into the scheduler), then every INPUT that is due. Returns
 * the ms until the next pending INPUT is due (UINT32_MAX = none). */
static uint32_t SvcSend(void) {
    const GdxMpDgram* d;
    u8 buf[GDX_MP_TXQ_INPUT_MAX];
    u16 len = 0;
    uint32_t waitMs = UINT32_MAX;
    while ((d = gdx_mp_ring_peek(&sSvcTx)) != NULL) {
        if (!(gdx_mp_txq_is_input(d->data, d->len) &&
              gdx_mp_txsched_offer_input(&sSvcSched, d->node, d->data, d->len))) {
            if (SendNode(d->node, d->data, d->len) > 0) {
                sSvcSent++;
            }
        }
        gdx_mp_ring_release(&sSvcTx);
    }
    for (;;) {
        const u16 node = gdx_mp_txsched_take_due(&sSvcSched, NowMs(), buf, &len, &waitMs);
        if (node == 0) {
            return waitMs;
        }
        if (SendNode(node, buf, len) > 0) {
            sSvcSent++;
        }
    }
}

static void SvcMain(void* arg) {
    u32 lastRxMs = NowMs();
    int rxDue = 1; /* a fresh start drains whatever the bind already holds */
    (void) arg;
    while (!sSvcQuit) {
        const uint32_t sendWait = SvcSend();
        const u32 now = NowMs();
        uint32_t waitMs;
        Handle h[2];
        s32 which = -1;
        Result rc;
        if (rxDue || sSvcPolling) {
            SvcDrainRx();
            lastRxMs = now;
            rxDue = 0;
        } else if ((u32) (now - lastRxMs) >= SVC_PROBE_MS) {
            lastRxMs = now;
            if (SvcProbe()) {
                sSvcPolling = 1;
                UdsLog("[uds] svc: a probe pull found a frame the receive event did not announce: polling every %d ms",
                       SVC_POLL_MS);
            }
        }
        waitMs = sSvcPolling ? SVC_POLL_MS : SVC_PROBE_MS;
        if (sendWait < waitMs) {
            waitMs = sendWait;
        }
        if (waitMs == 0) {
            continue;
        }
        h[0] = sBind.event;
        h[1] = sSvcWake;
        rc = svcWaitSynchronizationN(&which, h, 2, false, (s64) waitMs * 1000000LL);
        sSvcWakes++;
        if (R_SUCCEEDED(rc) && which == 0) {
            svcClearEvent(sBind.event);
            rxDue = 1;
        }
    }
}

static void SvcStart(void) {
    s32 prio = 0x30;
    if (sSvcLever < 0) {
        sSvcLever = gdx3ds_config_get_int("net", "mp_uds_thread", 1) != 0;
        sSvcMinMs = gdx3ds_config_get_int("net", "mp_uds_tx_min_ms", 33);
        if (sSvcMinMs < 0) {
            sSvcMinMs = 0;
        }
        sSvcCore = gdx3ds_config_get_int("net", "mp_uds_thread_core", -2);
        UdsLog("[uds] svc thread %s (net.mp_uds_thread) tx_min_ms=%d core=%d", sSvcLever ? "on" : "off",
               sSvcMinMs, sSvcCore);
    }
    if (!sSvcLever || sSvcFailed) {
        return;
    }
    gdx3ds_ipcstat_init();
    gdx_mp_ring_init(&sSvcTx, sSvcTxSlots, SVC_TX_SLOTS);
    gdx_mp_ring_init(&sSvcRx, sSvcRxSlots, SVC_RX_SLOTS);
    gdx_mp_txsched_init(&sSvcSched, (uint32_t) sSvcMinMs);
    sSvcPolling = 0;
    sSvcQuit = 0;
    if (R_FAILED(svcCreateEvent(&sSvcWake, RESET_ONESHOT))) {
        sSvcFailed = 1;
        UdsLog("[uds] svc: svcCreateEvent failed: inline UDS calls");
        return;
    }
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    sSvc = threadCreate(SvcMain, NULL, SVC_STACK, prio > 0x18 ? prio - 1 : prio, sSvcCore, false);
    if (sSvc == NULL) {
        svcCloseHandle(sSvcWake);
        sSvcWake = 0;
        sSvcFailed = 1;
        UdsLog("[uds] svc: threadCreate failed (prio 0x%lX core %d): inline UDS calls", (unsigned long) (prio - 1),
               sSvcCore);
        return;
    }
    UdsLog("[uds] svc: thread up (prio 0x%lX core %d)", (unsigned long) (prio > 0x18 ? prio - 1 : prio), sSvcCore);
}

static void SvcStop(void) {
    if (sSvc == NULL) {
        return;
    }
    sSvcQuit = 1;
    svcSignalEvent(sSvcWake);
    if (R_FAILED(threadJoin(sSvc, 3000000000ULL))) {
        /* Stuck in a sysmodule call: never free a running thread; it exits at its next wake. */
        UdsLog("[uds] svc: thread did not stop within 3 s (left detached)");
        threadDetach(sSvc);
    } else {
        threadFree(sSvc);
        svcCloseHandle(sSvcWake);
    }
    sSvc = NULL;
    sSvcWake = 0;
    sRxMaybe = 1; /* the inline path starts with a drain */
    UdsLog("[uds] svc: thread down queued=%lu sent=%lu coalesced=%lu txdrop=%lu rx=%lu rxdrop=%lu pulls=%lu "
           "empty=%lu wakes=%lu",
           (unsigned long) sSvcQueued, (unsigned long) sSvcSent, (unsigned long) sSvcSched.coalesced,
           (unsigned long) sSvcTxDrops, (unsigned long) sSvcRxFrames, (unsigned long) sSvcRxDrops,
           (unsigned long) sSvcPulls, (unsigned long) sSvcEmpty, (unsigned long) sSvcWakes);
}

/* Main thread: the thread runs whenever the link is live and the lever is on. */
static int SvcEnsure(void) {
    if (sSvc == NULL && Live() && !sSvcFailed && sSvcLever != 0) {
        SvcStart();
    }
    return SvcActive();
}

/* Main thread: queue one datagram for `node`. Returns len, 0 when the ring is full. */
static int SvcQueue(u16 node, const void* buf, int len) {
    const uint64_t t0 = gdx3ds_ipcstat_begin();
    const int ok = gdx_mp_ring_push(&sSvcTx, node, buf, len);
    if (ok) {
        sSvcQueued++;
        svcSignalEvent(sSvcWake);
    } else {
        sSvcTxDrops++;
    }
    gdx3ds_ipcstat_end(GDX_IPC_Q_PUSH, t0);
    return ok ? len : 0;
}

/* Main thread: the next received datagram, or 0. */
static int SvcTake(void* buf, int cap, u16* src) {
    const uint64_t t0 = gdx3ds_ipcstat_begin();
    const GdxMpDgram* d = gdx_mp_ring_peek(&sSvcRx);
    int n = 0;
    if (d != NULL) {
        if ((int) d->len > cap) {
            sRxOversize++;
        } else {
            memcpy(buf, d->data, d->len);
            n = d->len;
            *src = d->node;
            sRxFrames++;
            sRxBytes += (u32) n;
        }
        gdx_mp_ring_release(&sSvcRx);
        gdx3ds_ipcstat_end(GDX_IPC_Q_POP, t0);
    }
    return n;
}

void gdx3ds_mp_uds_svc_line(char* buf, int cap) {
    if (cap <= 0) {
        return;
    }
    buf[0] = '\0';
    if (sSvc == NULL) {
        return;
    }
    snprintf(buf, (size_t) cap,
             "[uds] svc queued=%lu sent=%lu coalesced=%lu txdrop=%lu rx=%lu rxdrop=%lu pulls=%lu empty=%lu "
             "wakes=%lu polling=%d tx_min_ms=%d",
             (unsigned long) sSvcQueued, (unsigned long) sSvcSent, (unsigned long) sSvcSched.coalesced,
             (unsigned long) sSvcTxDrops, (unsigned long) sSvcRxFrames, (unsigned long) sSvcRxDrops,
             (unsigned long) sSvcPulls, (unsigned long) sSvcEmpty, (unsigned long) sSvcWakes, sSvcPolling, sSvcMinMs);
}

/* Main thread: one datagram to `node`, through the service thread when it runs. */
static int SendOrQueue(u16 node, const void* buf, int len) {
    return SvcEnsure() ? SvcQueue(node, buf, len) : SendNode(node, buf, len);
}

static int UdsSend(int peer, const void* buf, int len) {
    if (!Live()) {
        return 0;
    }
    if (len <= 0 || len > GDX_MP_TRANSPORT_MTU) {
        return -1;
    }
    if (peer == GDX_MP_PEER_ANY) {
        /* Host: one broadcast frame reaches every joiner. Joiner: the host relays (star). */
        return SendOrQueue(sHosting ? UDS_BROADCAST_NETWORKNODEID : UDS_HOST_NETWORKNODEID, buf, len);
    }
    if (peer < 0 || peer >= sNumPlayers || peer >= GDX_MP_MAX_PLAYERS || peer == sLocalPlayer) {
        return -1;
    }
    if (sPlayerNode[peer] == 0) {
        return 0;
    }
    return SendOrQueue(sPlayerNode[peer], buf, len);
}

/* NETTICK: 1 = a udsPullPacket may find a frame (see sRxGate). */
static int RxMayHaveFrame(void) {
    if (sRxGate < 0) {
        sRxGate = gdx3ds_config_get_int("net", "mp_uds_rx_gate", 1) != 0;
        UdsLog("[uds] rx gate %s (net.mp_uds_rx_gate): udsPullPacket only after the bind event fires",
               sRxGate ? "on" : "off");
    }
    if (!sRxGate || sRxMaybe) {
        return 1;
    }
    if (udsWaitDataAvailable(&sBind, false, false)) {
        sRxMaybe = 1;
        return 1;
    }
    {
        const u32 now = NowMs();
        if ((s32) (now - sRxProbeMs) >= RX_PROBE_MS) {
            sRxProbeMs = now;
            sRxProbing = 1;
            return 1;
        }
    }
    gdx3ds_ipcstat_gated(GDX_IPC_UDS_PULL);
    return 0;
}

/* NETTICK: one timed udsPullPacket; an empty pull closes the drain (sRxMaybe = 0). */
static Result PullFrame(size_t* actual, u16* src) {
    const uint64_t t0 = gdx3ds_ipcstat_begin();
    const Result rc = udsPullPacket(&sBind, sFrame, sizeof(sFrame), actual, src);
    gdx3ds_ipcstat_end(GDX_IPC_UDS_PULL, t0);
    if (sRxProbing) {
        sRxProbing = 0;
        /* A frame that landed after the check re-signalled the event: that is the event working
         * (the check-and-clear leaves it signalled). Only a frame with the event still clear is
         * one the event missed. */
        if (R_SUCCEEDED(rc) && *actual > 0 && !udsWaitDataAvailable(&sBind, false, false)) {
            sRxGate = 0;
            UdsLog("[uds] rx gate off: a probe pull found a frame the bind event did not announce");
        }
        if (R_SUCCEEDED(rc) && *actual > 0) {
            sRxMaybe = 1; /* keep draining */
            return rc;
        }
    }
    if (R_FAILED(rc) || *actual == 0) {
        sRxMaybe = 0;
    }
    return rc;
}

static int UdsRecv(void* buf, int cap, int* peerHint) {
    size_t actual = 0;
    u16 src = 0;
    Result rc;
    int p;
    *peerHint = GDX_MP_PEER_ANY;
    if (!Live()) {
        return 0;
    }
    if (SvcEnsure()) {
        const int n = SvcTake(buf, cap, &src);
        if (n <= 0) {
            return 0;
        }
        sLastSrcNode = src;
        for (p = 0; p < sNumPlayers && p < GDX_MP_MAX_PLAYERS; p++) {
            if (sPlayerNode[p] == src) {
                *peerHint = p;
                break;
            }
        }
        return n;
    }
    if (!RxMayHaveFrame()) {
        return 0;
    }
    rc = PullFrame(&actual, &src);
    sLastRxRc = rc;
    if (R_FAILED(rc)) {
        sRxErr++;
        return 0;
    }
    if (actual == 0) {
        return 0;
    }
    if (src == sLocalNode) {
        return 0; /* never our own broadcast */
    }
    sRxFrames++;
    sRxBytes += (u32) actual;
    sLastSrcNode = src;
    if ((int) actual > cap) {
        sRxOversize++;
        return 0;
    }
    memcpy(buf, sFrame, actual);
    for (p = 0; p < sNumPlayers && p < GDX_MP_MAX_PLAYERS; p++) {
        if (sPlayerNode[p] == src) {
            *peerHint = p;
            break;
        }
    }
    return (int) actual;
}

static void UdsClose(void) {
    SvcStop(); /* the thread never touches a bind that is going away */
    if (sHosting) {
        udsDestroyNetwork();
        sHosting = 0;
    } else if (sJoinState == GDX_UDS_JOIN_CONNECTED) {
        udsDisconnectNetwork();
        sJoinState = GDX_UDS_JOIN_IDLE;
    }
    if (sBound) {
        udsUnbind(&sBind);
        sBound = 0;
    }
    UdsLog("[uds] closed");
}

static int UdsReopen(void) {
    return 0; /* nothing to re-create: NWM owns the link */
}

void gdx3ds_mp_uds_describe(char* buf, int cap) {
    RefreshStatus(0);
    snprintf(buf, (size_t) cap,
             "node=%u nodes=%u bitmask=0x%04X status=0x%lX tx=%lu/%luB qfull=%lu fatal=%lu rc=0x%08lX "
             "rx=%lu/%luB err=%lu big=%lu rc=0x%08lX wifi=%u",
             (unsigned) sLocalNode, (unsigned) (sHaveStatus ? sStatus.total_nodes : 0),
             (unsigned) (sHaveStatus ? sStatus.node_bitmask : 0), (unsigned long) (sHaveStatus ? sStatus.status : 0),
             (unsigned long) sTxFrames, (unsigned long) sTxBytes, (unsigned long) sTxQueueFull,
             (unsigned long) sTxFatal, (unsigned long) sLastTxRc, (unsigned long) sRxFrames, (unsigned long) sRxBytes,
             (unsigned long) sRxErr, (unsigned long) sRxOversize, (unsigned long) sLastRxRc,
             (unsigned) OS_SharedConfig->wifi_strength);
}

static int UdsSelftest(char* buf, int cap) {
    gdx3ds_mp_uds_describe(buf, cap);
    return 0;
}

static unsigned int UdsPresentMask(void) {
    unsigned int mask = 0;
    int p;
    if (!Live()) {
        return 1u << sLocalPlayer;
    }
    RefreshStatus(0);
    for (p = 0; p < sNumPlayers && p < GDX_MP_MAX_PLAYERS; p++) {
        const u16 node = (p == sLocalPlayer) ? sLocalNode : sPlayerNode[p];
        if (p == sLocalPlayer || node == 0 || !sHaveStatus) {
            mask |= 1u << p;
        } else if (node >= 1 && node <= 16 && (sStatus.node_bitmask & (1u << (node - 1)))) {
            mask |= 1u << p;
        }
    }
    return mask;
}

static const GdxMpTransportOps kUdsOps = {
    "uds", UdsOpen, UdsBindLastSource, UdsSend, UdsRecv, UdsClose, UdsReopen, UdsSelftest, UdsPresentMask,
};

const GdxMpTransportOps* gdx3ds_mp_uds_ops(void) {
    return &kUdsOps;
}

/* ---- NEARBY discovery ops ------------------------------------------------------------------- */

static int DiscStart(const char* build, uint32_t contentHash, char* why, int cap) {
    const int rc = gdx3ds_mp_uds_init(why, cap);
    if (rc != GDX_UDS_OK) {
        return rc;
    }
    SetIdentity(build, contentHash);
    sAutoJoin = 0;
    sScanning = 1;
    if (sJoinState != GDX_UDS_JOIN_CONNECTED) {
        sJoinState = GDX_UDS_JOIN_IDLE;
    }
    return StartWorker();
}

static int DiscList(GdxMpUdsRoom* out, int cap) {
    const u32 now = NowMs();
    int n = 0;
    int i;
    EnsureLock();
    LightLock_Lock(&sLock);
    for (i = 0; i < GDX_MP_UDS_NEARBY_MAX; i++) {
        if (sRooms[i].used && (u32) (now - sRooms[i].room.lastSeenMs) > GDX_UDS_ROOM_STALE_MS) {
            sRooms[i].used = 0;
        }
    }
    for (i = 0; i < GDX_MP_UDS_NEARBY_MAX && n < cap; i++) {
        if (sRooms[i].used) {
            int j = n;
            /* newest first */
            while (j > 0 && out[j - 1].lastSeenMs < sRooms[i].room.lastSeenMs) {
                out[j] = out[j - 1];
                j--;
            }
            out[j] = sRooms[i].room;
            n++;
        }
    }
    LightLock_Unlock(&sLock);
    return n;
}

static void DiscStop(void) {
    sScanning = 0;
}

static int DiscJoin(const GdxMpUdsRoom* room) {
    if (room == NULL || room->medium != GDX_MP_UDS_MEDIUM_UDS || room->compat != GDX_MP_COMPAT_OK) {
        return GDX_UDS_E_CONNECT;
    }
    if (sJoinState == GDX_UDS_JOIN_CONNECTING || sJoinState == GDX_UDS_JOIN_CONNECTED) {
        return GDX_UDS_E_BUSY;
    }
    memcpy(sJoinMac, room->handle, 6);
    sAutoJoin = 0;
    sJoinWhy[0] = '\0';
    sJoinState = GDX_UDS_JOIN_CONNECTING;
    sJoinWant = 1;
    return StartWorker();
}

static int DiscHost(const GdxMpBeacon* b) {
    char why[160];
    const int rc = gdx3ds_mp_uds_init(why, sizeof(why));
    if (rc != GDX_UDS_OK) {
        return rc;
    }
    sScanning = 0;
    return gdx3ds_mp_uds_host_beacon(b);
}

static int DiscHostUpdate(const GdxMpBeacon* b) {
    return gdx3ds_mp_uds_host_beacon(b);
}

static const GdxMpUdsNearbyOps kUdsDiscovery = {
    "uds", DiscStart, DiscList, DiscStop, DiscJoin, gdx3ds_mp_uds_join_state, DiscHost, DiscHostUpdate,
};

const GdxMpUdsNearbyOps* gdx3ds_mp_uds_discovery(void) {
    return &kUdsDiscovery;
}

/* ---- mp-online: UDS as a raw datagram medium (gdx3ds_mp_p2p.c) ------------------------------ */

void gdx3ds_mp_uds_set_proto(int proto) {
    sProto = proto;
}

int gdx3ds_mp_uds_live(void) {
    return Live();
}

/* Stop the scan / connect worker and wait for it (no worker call may be in flight while the main
 * thread creates, joins or leaves a network). */
static void StopWorker(void) {
    if (sWorker == NULL) {
        return;
    }
    sWorkerQuit = 1;
    threadJoin(sWorker, 3000000000ULL);
    threadFree(sWorker);
    sWorker = NULL;
    sWorkerQuit = 0;
    sReleaseWanted = 0;
    if (sWorkerReleased) {
        sWorkerReleased = 0;
        sUdsUp = 0; /* it released UDS before it saw the quit: the next use inits again */
    }
}

/* MPFIX: while UDS is initialised the console's radio is in local-wireless mode and its WiFi link
 * is down (hw 516011c+: opening NEARBY with mp_uds=1 took the New 3DS off the network for good --
 * nothing ever called udsExit before the process exit). Called every frame by mp-online while
 * NEARBY is closed and no local room / join exists; never blocks: a worker still inside a scan
 * is asked to udsExit itself and reaped on a later call. 1 = UDS is down (released or never up). */
int gdx3ds_mp_uds_release(void) {
    if (!sUdsUp) {
        return 1;
    }
    if (sHosting || sBound || sJoinWant || sJoinState == GDX_UDS_JOIN_CONNECTED ||
        sJoinState == GDX_UDS_JOIN_CONNECTING || sAutoJoin) {
        return 0;
    }
    if (sWorker != NULL) {
        if (!sReleaseWanted) {
            sScanning = 0;
            sReleaseWanted = 1;
            return 0;
        }
        if (!sWorkerReleased) {
            return 0; /* still finishing a scan */
        }
        threadJoin(sWorker, U64_MAX);
        threadFree(sWorker);
        sWorker = NULL;
        sReleaseWanted = 0;
        sWorkerReleased = 0;
    } else {
        udsExit();
    }
    sUdsUp = 0;
    sJoinState = GDX_UDS_JOIN_IDLE;
    UdsLog("[uds] released: NEARBY closed, no local room (the radio is free for WiFi again)");
    return 1;
}

int gdx3ds_mp_uds_scan(int on, const char* build, uint32_t contentHash, char* why, int cap) {
    if (!on) {
        sScanning = 0;
        return 0;
    }
    if (Live() || sJoinState == GDX_UDS_JOIN_CONNECTING) {
        return 0; /* in a room (or joining one): no scans */
    }
    if (sScanning && sWorker != NULL) {
        return 0;
    }
    StopWorker(); /* a worker that ended (it connected once) is restarted fresh */
    return DiscStart(build, contentHash, why, cap);
}

int gdx3ds_mp_uds_host_room(const GdxMpBeacon* b, char* why, int cap) {
    int rc;
    sScanning = 0;
    StopWorker();
    rc = gdx3ds_mp_uds_init(why, cap);
    if (rc != GDX_UDS_OK) {
        return rc;
    }
    rc = gdx3ds_mp_uds_host_beacon(b);
    if (rc != GDX_UDS_OK && why != NULL && cap > 0) {
        snprintf(why, (size_t) cap, "%s",
                 WirelessDisabled() ? "Wireless is off. Turn the wireless switch on, then try again."
                                    : "could not open a local wireless room");
    }
    return rc;
}

int gdx3ds_mp_uds_join_room(const GdxMpUdsRoom* room) {
    sScanning = 0;
    if (sJoinState == GDX_UDS_JOIN_CONNECTED || Live()) {
        return GDX_UDS_E_BUSY;
    }
    StopWorker(); /* a scan in flight finishes first; the fresh worker only connects */
    if (sJoinState == GDX_UDS_JOIN_CONNECTING) {
        sJoinState = GDX_UDS_JOIN_IDLE;
    }
    return DiscJoin(room);
}

void gdx3ds_mp_uds_leave(void) {
    sScanning = 0;
    StopWorker();
    if (sHosting || sJoinState == GDX_UDS_JOIN_CONNECTED || sBound) {
        UdsClose();
    }
    sJoinState = GDX_UDS_JOIN_IDLE;
    sJoinWant = 0;
    sLocalNode = 0;
    sHaveStatus = 0;
    memset(sPlayerNode, 0, sizeof(sPlayerNode));
}

int gdx3ds_mp_uds_raw_send(int node, const void* buf, int len) {
    if (!Live() || node < 1 || node > 16 || len <= 0 || len > UDS_DATAFRAME_MAXSIZE) {
        return 0;
    }
    return SendOrQueue((u16) node, buf, len);
}

int gdx3ds_mp_uds_raw_recv(void* buf, int cap, int* srcNode) {
    size_t actual = 0;
    u16 src = 0;
    Result rc;
    if (!Live()) {
        return 0;
    }
    if (SvcEnsure()) {
        const int n = SvcTake(buf, cap, &src);
        if (n > 0) {
            *srcNode = src;
        }
        return n;
    }
    if (!RxMayHaveFrame()) {
        return 0;
    }
    rc = PullFrame(&actual, &src);
    sLastRxRc = rc;
    if (R_FAILED(rc)) {
        sRxErr++;
        return 0;
    }
    if (actual == 0 || src == sLocalNode || src == 0) {
        return 0;
    }
    sRxFrames++;
    sRxBytes += (u32) actual;
    if ((int) actual > cap) {
        sRxOversize++;
        return 0;
    }
    memcpy(buf, sFrame, actual);
    *srcNode = src;
    return (int) actual;
}

/* ---- [net] mp_uds_probe_s ------------------------------------------------------------------- */

#define PROBE_SIZES 6
static const int kProbeSize[PROBE_SIZES] = { 32, 132, 512, 1024, 1400, UDS_DATAFRAME_MAXSIZE };

static void PutU32(u8* p, u32 v) {
    p[0] = (u8) (v & 0xFF);
    p[1] = (u8) ((v >> 8) & 0xFF);
    p[2] = (u8) ((v >> 16) & 0xFF);
    p[3] = (u8) (v >> 24);
}

static u32 GetU32(const u8* p) {
    return (u32) p[0] | ((u32) p[1] << 8) | ((u32) p[2] << 16) | ((u32) p[3] << 24);
}

static void ProbeHost(int seconds) {
    const int perSizeMs = (seconds * 1000) / PROBE_SIZES > 500 ? (seconds * 1000) / PROBE_SIZES : 500;
    u32 waitStart = NowMs();
    int s;
    /* Wait (<= 10 s) for a joiner, then give it a moment to bind the probe channel. */
    while (gdx3ds_mp_uds_node_count() < 2 && (u32) (NowMs() - waitStart) < 10000) {
        svcSleepThread(50 * 1000000LL);
    }
    svcSleepThread(1500 * 1000000LL);
    for (s = 0; s < PROBE_SIZES; s++) {
        const int size = kProbeSize[s];
        const u32 start = NowMs();
        u32 seq = 0;
        u32 ok = 0;
        u32 full = 0;
        u32 fatal = 0;
        Result last = 0;
        memset(sFrame, 0xA5, sizeof(sFrame));
        while ((u32) (NowMs() - start) < (u32) perSizeMs) {
            Result rc;
            memcpy(sFrame, "GXPB", 4);
            sFrame[4] = (u8) s;
            PutU32(sFrame + 8, seq);
            PutU32(sFrame + 12, NowMs());
            rc = udsSendTo(UDS_BROADCAST_NETWORKNODEID, GDX_UDS_PROBE_CHANNEL, UDS_SENDFLAG_Default, sFrame,
                           (size_t) size);
            seq++;
            last = rc;
            if (UDS_CHECK_SENDTO_FATALERROR(rc)) {
                fatal++;
                svcSleepThread(1000000LL);
            } else if (R_FAILED(rc)) {
                full++;
                svcSleepThread(200000LL);
            } else {
                ok++;
            }
        }
        {
            const u32 ms = NowMs() - start;
            UdsLog("[uds] probe tx size=%d sent=%lu queue_full=%lu fatal=%lu last_rc=0x%08lX ms=%lu tx_KBps=%lu", size,
                   (unsigned long) ok, (unsigned long) full, (unsigned long) fatal, (unsigned long) last,
                   (unsigned long) ms, (unsigned long) (ms ? (u64) ok * (u64) size / ms : 0));
        }
    }
    {
        static const int kOver[3] = { UDS_DATAFRAME_MAXSIZE + 1, 1500, 2048 };
        int k;
        for (k = 0; k < 3; k++) {
            static u8 big[2048];
            const Result rc = udsSendTo(UDS_BROADCAST_NETWORKNODEID, GDX_UDS_PROBE_CHANNEL, UDS_SENDFLAG_Default,
                                        big, (size_t) kOver[k]);
            UdsLog("[uds] probe oversize len=%d rc=0x%08lX (%s)", kOver[k], (unsigned long) rc,
                   R_FAILED(rc) ? "refused" : "ACCEPTED");
        }
    }
    for (s = 0; s < 8; s++) {
        memcpy(sFrame, "GXPE", 4);
        udsSendTo(UDS_BROADCAST_NETWORKNODEID, GDX_UDS_PROBE_CHANNEL, UDS_SENDFLAG_Default, sFrame, 16);
        svcSleepThread(20 * 1000000LL);
    }
}

static void ProbeJoin(int seconds) {
    udsBindContext ctx;
    u32 frames[PROBE_SIZES];
    u32 maxSeq[PROBE_SIZES];
    u32 first[PROBE_SIZES];
    u32 last[PROBE_SIZES];
    u32 latSum[PROBE_SIZES];
    const u32 start = NowMs();
    int s;
    Result rc;
    memset(frames, 0, sizeof(frames));
    memset(maxSeq, 0, sizeof(maxSeq));
    memset(first, 0, sizeof(first));
    memset(last, 0, sizeof(last));
    memset(latSum, 0, sizeof(latSum));
    rc = udsBind(&ctx, UDS_BROADCAST_NETWORKNODEID, false, GDX_UDS_PROBE_CHANNEL, UDS_DEFAULT_RECVBUFSIZE);
    if (R_FAILED(rc)) {
        UdsLog("[uds] probe bind rc=0x%08lX", (unsigned long) rc);
        return;
    }
    while ((u32) (NowMs() - start) < (u32) (seconds * 1000 + 15000)) {
        size_t actual = 0;
        u16 src = 0;
        if (R_FAILED(udsPullPacket(&ctx, sFrame, sizeof(sFrame), &actual, &src)) || actual == 0) {
            udsWaitDataAvailable(&ctx, false, false);
            svcSleepThread(200000LL);
            continue;
        }
        if (actual >= 16 && memcmp(sFrame, "GXPE", 4) == 0) {
            break;
        }
        if (actual >= 16 && memcmp(sFrame, "GXPB", 4) == 0 && sFrame[4] < PROBE_SIZES) {
            const int k = sFrame[4];
            const u32 seq = GetU32(sFrame + 8);
            const u32 now = NowMs();
            if (frames[k] == 0) {
                first[k] = now;
            }
            frames[k]++;
            last[k] = now;
            if (seq + 1 > maxSeq[k]) {
                maxSeq[k] = seq + 1;
            }
            if ((int) actual != kProbeSize[k]) {
                UdsLog("[uds] probe rx size mismatch: %u != %d", (unsigned) actual, kProbeSize[k]);
            }
        }
    }
    for (s = 0; s < PROBE_SIZES; s++) {
        const u32 ms = last[s] - first[s];
        UdsLog("[uds] probe rx size=%d frames=%lu of>=%lu loss_pct=%lu ms=%lu rx_KBps=%lu", kProbeSize[s],
               (unsigned long) frames[s], (unsigned long) maxSeq[s],
               (unsigned long) (maxSeq[s] ? 100u - (u32) ((u64) frames[s] * 100u / maxSeq[s]) : 100u),
               (unsigned long) ms, (unsigned long) (ms ? (u64) frames[s] * (u64) kProbeSize[s] / ms : 0));
    }
    udsUnbind(&ctx);
}

void gdx3ds_mp_uds_probe(int isHost, int seconds) {
    if (seconds <= 0 || !Live()) {
        return;
    }
    SvcStop(); /* the probe drives the radio itself; the thread restarts on the next datagram */
    UdsLog("[uds] probe %s %d s (the game waits for it)", isHost ? "tx" : "rx", seconds);
    gdx3ds_filelog_flush();
    if (isHost) {
        ProbeHost(seconds);
    } else {
        ProbeJoin(seconds);
    }
    gdx3ds_filelog_flush();
}
