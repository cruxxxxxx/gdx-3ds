/* port/3ds/net/gdx3ds_mp_socthr.c — the mp-online socket service thread (contract:
 * gdx3ds_mp_socthr.h; the loop body: port/gdx_mp_socsvc.h). */
#include "gdx3ds_mp_socthr.h"
#include "gdx_mp_socsvc.h"
#include "gdx3ds_config.h"
#include "gdx3ds_filelog.h"
#include "gdx3ds_mp_ipcstat.h"
#include "gdx3ds_net.h"

#include <3ds.h>
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

/* The queues are static byte queues (gdx_mp_socsvc.h): an Old 3DS in a race has ~3-13 KiB of heap
 * slack (hardware online1 [watchdog] heapFree), so nothing is allocated at room open. A full rx
 * queue is back-pressure, not loss: the thread stops reading and the rest waits in the socket's
 * own buffer. 16 KiB holds ~110-170 lockstep datagrams (96-144 B each as records): ~3 s of a main
 * thread that does not drain (a load frame) at a peer's ~30-60 datagrams/s. */
#define SOCTHR_TX_BYTES (8 * 1024)
#define SOCTHR_RX_BYTES (16 * 1024)
#define SOCTHR_STACK (16 * 1024)
#define SOCTHR_MAX_SLEEP_MS 250
#define SOCTHR_PARK_WAIT_MS 300

static GdxMpSocSvc sSvc;
static uint64_t sTxBuf[SOCTHR_TX_BYTES / sizeof(uint64_t)];
static uint64_t sRxBuf[SOCTHR_RX_BYTES / sizeof(uint64_t)];
static Thread sThread = NULL;
static Handle sWake = 0;
static Handle sRxReady = 0;  /* signalled when a drain queued datagrams (the main thread's wait) */
static volatile int sSoleWanted = 0; /* main: a race, and the main thread makes no SOC call */
static int sBlockPoll = 0;   /* [net] mp_soc_block_poll: block in poll() while the sole SOC user */
static volatile uint32_t sPolls = 0;
static int sFd = -1;
static volatile int sQuit = 0;
static volatile int sParkWanted = 0;
static volatile int sParked = 0;
static volatile uint32_t sWakes = 0;
static int sLever = -1;      /* [net] mp_soc_thread (default 1) */
static int sTxMinMs = 33;    /* [net] mp_soc_tx_min_ms */
static int sTxRepeats = 2;   /* [net] mp_soc_tx_repeat: steady-cadence repeats of a new INPUT */
static int sRxPollMs = 8;    /* [net] mp_soc_rx_poll_ms */
static int sRarePollMs = 250; /* [net] mp_uds_soc_poll_ms: a local wireless room's socket */
static int sCore = -2;       /* [net] mp_soc_thread_core */
static int sRare = 0;
static int sFailed = 0;      /* could not start: inline calls for the rest of the run */

static void SocLog(const char* fmt, ...) {
    char line[256];
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

static int IsNew3ds(void) {
    bool isNew = false;
    APT_CheckNew3DS(&isNew);
    return isNew ? 1 : 0;
}

static int ClampMs(int v, int lo, int hi) {
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

static void ReadLevers(void) {
    const int isNew = IsNew3ds();
    sLever = gdx3ds_config_get_int("net", "mp_soc_thread", 1) != 0;
    sTxMinMs = ClampMs(gdx3ds_config_get_int("net", "mp_soc_tx_min_ms", isNew ? 16 : 33), 0, 200);
    sRxPollMs = ClampMs(gdx3ds_config_get_int("net", "mp_soc_rx_poll_ms", isNew ? 4 : 8), 1, 100);
    sRarePollMs = ClampMs(gdx3ds_config_get_int("net", "mp_uds_soc_poll_ms", 250), 0, 5000);
    if (sRarePollMs == 0) {
        sRarePollMs = sRxPollMs;
    }
    sCore = gdx3ds_config_get_int("net", "mp_soc_thread_core", -2);
    sTxRepeats = ClampMs(gdx3ds_config_get_int("net", "mp_soc_tx_repeat", 2), 0, 8);
    sBlockPoll = gdx3ds_config_get_int("net", "mp_soc_block_poll", 0) != 0;
    SocLog("[socthr] %s (net.mp_soc_thread) tx_min_ms=%d tx_repeat=%d rx_poll_ms=%d rare_poll_ms=%d core=%d "
           "model=%s queues=%u+%u B static block_poll=%d",
           sLever ? "on" : "off", sTxMinMs, sTxRepeats, sRxPollMs, sRarePollMs, sCore, isNew ? "N3DS" : "O3DS",
           (unsigned) sizeof(sTxBuf), (unsigned) sizeof(sRxBuf), sBlockPoll);
}

/* ---- the thread's socket calls (GdxMpSocSvcOps) ----------------------------------------------- */
static int OpSend(void* ctx, uint64_t dest, const void* buf, int len) {
    struct sockaddr_in a;
    uint8_t ip[4];
    uint16_t port = 0;
    uint64_t t0;
    ssize_t sent;
    (void) ctx;
    gdx_mp_socsvc_unpack(dest, ip, &port);
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    memcpy(&a.sin_addr.s_addr, ip, 4);
    t0 = gdx3ds_ipcstat_begin();
    sent = sendto(sFd, buf, (size_t) len, 0, (const struct sockaddr*) &a, sizeof(a));
    gdx3ds_ipcstat_end(GDX_IPC_ST_SEND, t0);
    if (sent < 0) {
        return (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS) ? 0 : -errno;
    }
    return len;
}

static int OpRecv(void* ctx, void* buf, int cap, uint64_t* src) {
    struct sockaddr_in a;
    socklen_t alen = sizeof(a);
    uint64_t t0;
    ssize_t got;
    uint8_t ip[4];
    (void) ctx;
    t0 = gdx3ds_ipcstat_begin();
    got = recvfrom(sFd, buf, (size_t) cap, 0, (struct sockaddr*) &a, &alen);
    gdx3ds_ipcstat_end(GDX_IPC_ST_RECV, t0);
    if (got < 0) {
        return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -errno;
    }
    memcpy(ip, &a.sin_addr.s_addr, 4);
    *src = gdx_mp_socsvc_pack(ip, ntohs(a.sin_port));
    return (int) got;
}

static const GdxMpSocSvcOps kOps = { OpSend, OpRecv, NULL };

/* ---- the thread ------------------------------------------------------------------------------- */
static void SleepOnWake(uint32_t ms) {
    if (ms > SOCTHR_MAX_SLEEP_MS) {
        ms = SOCTHR_MAX_SLEEP_MS;
    }
    svcWaitSynchronization(sWake, (s64) ms * 1000000LL);
    sWakes++;
}

/* Parked (APT): what was queued has gone; no SOC call until the main thread lets go. */
static void HoldParked(void) {
    if (!sParked) {
        gdx_mp_socsvc_flush(&sSvc, &kOps, NowMs());
        sParked = 1;
    }
    SleepOnWake(50);
}

/* Nothing else touches SOC now ([net] mp_soc_block_poll, a race, the dev net thread quiet with no
 * link): a blocking poll() then costs no other thread a wait (gdx3ds_net.c MPFIX). */
static int SoleSocUser(void) {
    return sBlockPoll && sSoleWanted && !sRare && gdx3ds_net_soc_idle();
}

/* Blocking-poll mode: wake on a datagram, or after at most one rx poll period. A queued send
 * waits for the poll to return (<= rx_poll_ms). */
static void PollWait(uint32_t ms) {
    struct pollfd pf;
    uint64_t t0;
    int r;
    if (ms > (uint32_t) sRxPollMs) {
        ms = (uint32_t) sRxPollMs;
    }
    pf.fd = sFd;
    pf.events = POLLIN;
    pf.revents = 0;
    t0 = gdx3ds_ipcstat_begin();
    r = poll(&pf, 1, (int) ms);
    gdx3ds_ipcstat_end(GDX_IPC_ST_POLL, t0);
    sPolls++;
    if (r > 0 && (pf.revents & POLLIN) != 0) {
        gdx_mp_socsvc_note_readable(&sSvc);
    } else if (r == 0) {
        gdx_mp_socsvc_note_idle(&sSvc, NowMs()); /* nothing came: no recvfrom to learn that again */
    }
}

static void SignalIfReceived(uint32_t rxBefore) {
    if (sSvc.st.rx != rxBefore) {
        svcSignalEvent(sRxReady);
    }
}

static void ThreadMain(void* arg) {
    (void) arg;
    while (!sQuit) {
        uint32_t waitMs;
        uint32_t rxBefore;
        if (sParkWanted) {
            HoldParked();
            continue;
        }
        sParked = 0;
        rxBefore = sSvc.st.rx;
        waitMs = gdx_mp_socsvc_step(&sSvc, &kOps, NowMs());
        SignalIfReceived(rxBefore);
        if (waitMs == 0) {
            continue;
        }
        if (SoleSocUser()) {
            PollWait(waitMs);
        } else {
            SleepOnWake(waitMs);
        }
    }
    if (!sParkWanted) {
        gdx_mp_socsvc_flush(&sSvc, &kOps, NowMs()); /* a LEAVE / ABORT queued just before the close */
    }
}

/* ---- main thread ------------------------------------------------------------------------------ */
static uint32_t PollMs(void) {
    return (uint32_t) (sRare ? sRarePollMs : sRxPollMs);
}

static void InitQueues(void) {
    gdx_mp_socsvc_init(&sSvc, sTxBuf, (uint32_t) sizeof(sTxBuf), sRxBuf, (uint32_t) sizeof(sRxBuf),
                       (uint32_t) sTxMinMs, PollMs());
    sSvc.txRepeats = (uint32_t) sTxRepeats;
}

static void CloseEvents(void) {
    if (sWake != 0) {
        svcCloseHandle(sWake);
        sWake = 0;
    }
    if (sRxReady != 0) {
        svcCloseHandle(sRxReady);
        sRxReady = 0;
    }
}

static void StartFailed(const char* why) {
    sFailed = 1;
    CloseEvents();
    SocLog("[socthr] %s: inline socket calls", why);
}

static s32 ThreadPriority(void) {
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    return prio > 0x18 ? prio - 1 : prio;
}

int gdx3ds_socthr_start(int fd) {
    s32 prio;
    if (sLever < 0) {
        ReadLevers();
    }
    if (!sLever || sFailed || fd < 0) {
        return 0;
    }
    if (sThread != NULL) {
        gdx3ds_socthr_stop();
    }
    gdx3ds_ipcstat_init();
    InitQueues();
    if (R_FAILED(svcCreateEvent(&sWake, RESET_ONESHOT))) {
        sWake = 0;
        StartFailed("svcCreateEvent failed");
        return 0;
    }
    if (R_FAILED(svcCreateEvent(&sRxReady, RESET_ONESHOT))) {
        sRxReady = 0;
        StartFailed("svcCreateEvent (rx) failed");
        return 0;
    }
    sFd = fd;
    sQuit = 0;
    sParkWanted = 0;
    sParked = 0;
    prio = ThreadPriority();
    sThread = threadCreate(ThreadMain, NULL, SOCTHR_STACK, prio, sCore, false);
    if (sThread == NULL) {
        StartFailed("threadCreate failed");
        return 0;
    }
    SocLog("[socthr] thread up fd=%d prio=0x%lX core=%d", fd, (unsigned long) prio, sCore);
    return 1;
}

static void StopReceipt(void) {
    char line[320];
    gdx3ds_socthr_line(line, (int) sizeof(line));
    SocLog("[socthr] thread down | %s", line);
}

void gdx3ds_socthr_stop(void) {
    if (sThread == NULL) {
        return;
    }
    sQuit = 1;
    svcSignalEvent(sWake);
    StopReceipt();
    if (R_FAILED(threadJoin(sThread, 3000000000ULL))) {
        /* Stuck in a SOC call: never free a running thread or its rings; it exits at its next wake. */
        SocLog("[socthr] thread did not stop within 3 s (left detached)");
        threadDetach(sThread);
        sFailed = 1; /* its queues stay its own: never restarted over them */
    } else {
        threadFree(sThread);
        CloseEvents();
    }
    sThread = NULL;
    sWake = 0;
    sRxReady = 0;
    sFd = -1;
}

int gdx3ds_socthr_active(void) {
    return sThread != NULL;
}

int gdx3ds_socthr_send(const GdxMpEndpoint* to, const void* buf, int len, uint32_t stream) {
    const uint64_t t0 = gdx3ds_ipcstat_begin();
    const int ok = gdx_mp_socsvc_queue(&sSvc, gdx_mp_socsvc_pack(to->ip, to->port), stream, buf, len);
    if (ok) {
        svcSignalEvent(sWake);
    }
    gdx3ds_ipcstat_end(GDX_IPC_Q_PUSH, t0);
    return ok ? len : 0;
}

int gdx3ds_socthr_recv(void* buf, int cap, GdxMpEndpoint* src) {
    uint64_t from = 0;
    uint64_t t0;
    int n;
    if (gdx_mp_byteq_count(&sSvc.rx) == 0) {
        return 0;
    }
    t0 = gdx3ds_ipcstat_begin();
    n = gdx_mp_socsvc_take(&sSvc, buf, cap, &from);
    gdx3ds_ipcstat_end(GDX_IPC_Q_POP, t0);
    if (n <= 0) {
        return 0;
    }
    gdx_mp_socsvc_unpack(from, src->ip, &src->port);
    return n;
}

void gdx3ds_socthr_wait_rx(uint32_t maxMs) {
    if (sThread == NULL || sRxReady == 0) {
        svcSleepThread(500000LL);
        return;
    }
    if (gdx_mp_byteq_count(&sSvc.rx) != 0) {
        return;
    }
    svcWaitSynchronization(sRxReady, (s64) (maxMs > 0 ? maxMs : 1) * 1000000LL);
}

void gdx3ds_socthr_set_sole(int sole) {
    sSoleWanted = sole ? 1 : 0;
}

void gdx3ds_socthr_set_rare_poll(int rare) {
    sRare = rare ? 1 : 0;
    if (sThread != NULL) {
        sSvc.rxPollMs = PollMs();
        svcSignalEvent(sWake);
    }
}

void gdx3ds_socthr_park(int park) {
    int waited;
    if (sThread == NULL) {
        return;
    }
    sParkWanted = park ? 1 : 0;
    svcSignalEvent(sWake);
    if (!park) {
        SocLog("[socthr] unparked");
        return;
    }
    for (waited = 0; waited < SOCTHR_PARK_WAIT_MS && !sParked; waited += 5) {
        svcSleepThread(5 * 1000000LL);
    }
    SocLog("[socthr] parked=%d after %d ms (APT)", sParked, waited);
}

void gdx3ds_socthr_line(char* buf, int cap) {
    const GdxMpSocSvcStats* st = &sSvc.st;
    if (cap <= 0) {
        return;
    }
    buf[0] = '\0';
    if (sThread == NULL) {
        return;
    }
    snprintf(buf, (size_t) cap,
             "[socthr] queued=%lu sent=%lu coalesced=%lu repeats=%lu txdrop=%lu refused=%lu blocked=%lu senderr=%lu rx=%lu "
             "empty=%lu rxerr=%lu rxfull=%lu taken=%lu drains=%lu wakes=%lu polls=%lu sole=%d txq=%lu/%lu rxq=%lu/%lu "
             "lasterr=%d tx_min_ms=%d rx_poll_ms=%lu",
             (unsigned long) st->queued, (unsigned long) st->sent, (unsigned long) sSvc.sched.coalesced,
             (unsigned long) sSvc.sched.repeats,
             (unsigned long) st->txDrops, (unsigned long) sSvc.sched.refused, (unsigned long) st->sendBlocked,
             (unsigned long) st->sendErr, (unsigned long) st->rx, (unsigned long) st->rxEmpty,
             (unsigned long) st->rxErr, (unsigned long) st->rxFullStops, (unsigned long) st->taken,
             (unsigned long) st->drains, (unsigned long) sWakes, (unsigned long) sPolls, SoleSocUser(),
             (unsigned long) gdx_mp_byteq_count(&sSvc.tx),
             (unsigned long) st->txDepthMax, (unsigned long) gdx_mp_byteq_count(&sSvc.rx),
             (unsigned long) st->rxDepthMax, st->lastErr, sTxMinMs, (unsigned long) sSvc.rxPollMs);
}
