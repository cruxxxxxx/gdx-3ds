/* port/3ds/net/gdx3ds_net.c — hardware dev link. Contract + ini keys: gdx3ds_net.h.
 *
 * Everything here is inert unless [net] enabled = 1. Failure is never fatal: each stage
 * logs a "[net] ..." receipt and degrades (no SOC -> no link; no host -> control only;
 * no hb:ldr -> plain exit).
 */
#include "gdx3ds_net.h"

#include "gdx3ds_config.h"
#include "gdx3ds_filelog.h"
#include "gdx_net_enabled_default.h" /* [net] enabled's default resolution, host-testable */

#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include <3ds.h>

/* hw bug n60-3d8, item 3: the real branch@hash now comes from this generated, build-time
 * header (port/3ds/cmake/gdx3ds_gen_build_id.cmake) instead of a configure-time -D; the
 * __has_include guard + #ifndef below is a safety net for a build that never generated or
 * wired up the include path for it. */
#if defined(__has_include)
#if __has_include("gdx3ds_build_id.h")
#include "gdx3ds_build_id.h"
#endif
#endif
#ifndef GDX3DS_BUILD_ID
#define GDX3DS_BUILD_ID "unknown@nohash"
#endif

/* ---- external levers -------------------------------------------------------------- */

/* port/3ds/lus_glue/gdx3ds_input_glue.c — sets the flag main()'s loop condition reads. */
extern void gdx_request_quit(void);

/* ORACLE-TRACK HOOKS, declared weak so this branch links standalone; they become real
 * definitions when the oracle branch merges.
 *
 * MERGE NOTE (read before merging native/oracle): 03-oracle-plan.md §1 "Triggers" spells
 * the arm hook `gdx_oracle_capture_request(const char* tag, int flags)` — TWO arguments.
 * This branch was directed to call the ONE-argument form. C has no name mangling, so a
 * mismatch links silently and hands the callee garbage in r1 under AAPCS. Whoever merges
 * must reconcile the signature in ONE place (here and at the oracle definition) and
 * rebuild; do not assume the link succeeding means it is correct. */
__attribute__((weak)) void gdx_oracle_capture_request(const char* tag);
__attribute__((weak)) void gdx_replay_start(const char* sdmcPath);

/* ---- configuration ---------------------------------------------------------------- */

#define NET_DEFAULT_CTL_PORT 17492
#define NET_DEFAULT_LOG_PORT 17491 /* LINK3DS_COMM_PORT */
#define NET_DEFAULT_TARGET "/3ds/G-Diffuser-3DS.3dsx"
#define NET_DEFAULT_SOC_KB 1024

#define NET_RING_CAP 32768u  /* SPSC log ring (static) */
#define NET_RX_CAP 65536u    /* file-receive staging chunk (static); GET reuses it to send */
#define NET_GET_CAP NET_RX_CAP /* GET size cap -- sRxBuf is the whole buffer, one shot */
#define NET_GET_PREFIX "/3ds/gdiffuser/" /* GET is confined under here (see netPathUnderGdiffuser) */
#define NET_LINE_CAP 512u    /* one control command line */
#define NET_PATH_CAP 192u
#define NET_THREAD_STACK (32 * 1024)
#define NET_THREAD_PRIO 0x3E /* below main/game 0x30; libctru app range is [0x18;0x3F] */
#define NET_THREAD_CORE 0    /* appcore: no APT_SetAppCpuTimeLimit grant needed */
#define NET_POLL_MS 50
#define NET_RECONNECT_MS 2000
#define NET_IO_TIMEOUT_S 4

static int sNetEnabled = 0;
static int sCtlPort = NET_DEFAULT_CTL_PORT;
static int sLogPort = NET_DEFAULT_LOG_PORT;
static int sWantStdio = 0;
static char sTargetPath[NET_PATH_CAP];    /* "/3ds/G-Diffuser-3DS.3dsx" (hb:ldr form) */
static char sTargetSdmc[NET_PATH_CAP];    /* "sdmc:/3ds/G-Diffuser-3DS.3dsx" */
static char sArgv0[NET_PATH_CAP];         /* main()'s argv[0], for HBLDR_SetArgv */
static struct in_addr sHostAddr;          /* log-link peer; 0 = none known */

/* ---- runtime state ---------------------------------------------------------------- */

static u32* sSocBuf = NULL;
static int sSocUp = 0;
static int sSocExited = 0;
static Thread sNetThread = NULL;
static volatile int sNetQuit = 0;
static volatile int sNetSuspended = 0;
static volatile int sNetSuspendAck = 1;
static volatile int sLogFd = -1;
static volatile int sListenFd = -1;
static volatile int sCtlFd = -1;
static volatile int sRelaunchPending = 0;
static unsigned long sRelinkAttempts = 0;
static int sMpQuietLever = 1;              /* [net] mp_net_quiet */
static volatile int sMpQuiet = 0;          /* main: a room / race is up */
static volatile int sMpQuietAck = 0;       /* thread: quiet and holding no socket */
#define NET_QUIET_DRAIN_MS 500
static u64 sBootTick = 0;
static unsigned long sFrameNum = 0;

/* SPSC ring. Producer = whoever holds the filelog lock; consumer = the net thread. */
static char sRing[NET_RING_CAP];
static u32 sRingHead = 0; /* bytes ever produced (the __atomic builtins own ordering) */
static u32 sRingTail = 0; /* bytes ever consumed */
static u32 sRingDrop = 0;
static u32 sRingDropTotal = 0;
static volatile int sRingArmed = -1; /* -1 pre-init (buffer boot lines), 0 off, 1 on */

/* sRxBuf is staging for PUT/RELOAD (net thread fills it, net thread writes it to the FS in
 * NET_RX_CAP-sized chunks) AND the whole-file buffer GET reads into (main thread fills it
 * from the FS in one shot, net thread sends it). Both uses are safe in the SAME buffer only
 * because exactly one control command is ever in flight at a time (netReadCtl dispatches one
 * line, to completion, before reading the next) -- there is never a PUT mid-stream at the
 * same moment a GET is being served. */
static char sRxBuf[NET_RX_CAP];
static char sCtlLine[NET_LINE_CAP];
static unsigned sCtlLineLen = 0;

/* GET's result, written by the main thread inside REQ_GET, read by the net thread right
 * after netPostRequest() returns (which happens-after the main thread's write, same fence
 * the REQ_* protocol already relies on for sReqRc). */
static u32 sGetLen = 0;
static u32 sGetCrc = 0;

/* ---- main-thread request queue ---------------------------------------------------- */

enum {
    REQ_NONE = 0,
    REQ_QUIT,
    REQ_RELOAD,
    REQ_CAPTURE,
    REQ_REPLAY,
    REQ_INI_SET,
    REQ_INI_DEL,
    REQ_INI_SAVE,
    REQ_GET
};

static volatile int sReqKind = REQ_NONE;
static volatile int sReqDone = 1;
static volatile int sReqRc = 0;
static char sReqA[64];
static char sReqB[64];
static char sReqC[NET_PATH_CAP];

/* ---- receipts --------------------------------------------------------------------- */

__attribute__((format(printf, 1, 2))) static void netLog(const char* fmt, ...) {
    char msg[224];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (n <= 0) {
        return;
    }
    if (n > (int)sizeof(msg) - 1) {
        n = (int)sizeof(msg) - 1;
    }
    svcOutputDebugString(msg, n);
    /* Reaches the SD mirror AND (via the hook below) the ring, so the host sees it too. */
    gdx3ds_filelog_write(msg, (size_t)n);
}

/* ---- CRC-32 (IEEE, reflected) ------------------------------------------------------ */

static u32 sCrcTable[256];
static int sCrcTableReady = 0;

static void netCrcInit(void) {
    if (sCrcTableReady) {
        return;
    }
    for (u32 i = 0; i < 256; i++) {
        u32 c = i;
        for (int k = 0; k < 8; k++) {
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        sCrcTable[i] = c;
    }
    sCrcTableReady = 1;
}

static u32 netCrcUpdate(u32 crc, const void* data, size_t len) {
    const unsigned char* p = (const unsigned char*)data;
    for (size_t i = 0; i < len; i++) {
        crc = sCrcTable[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc;
}

/* ---- SPSC log ring ----------------------------------------------------------------- */

void gdx3ds_net_log_write(const char* msg, size_t len) {
    if (msg == NULL || len == 0 || sRingArmed == 0) {
        return;
    }
    if (len + 1 > NET_RING_CAP) {
        __atomic_add_fetch(&sRingDrop, 1, __ATOMIC_RELAXED);
        __atomic_add_fetch(&sRingDropTotal, 1, __ATOMIC_RELAXED);
        return;
    }
    const u32 head = sRingHead; /* producer-owned */
    const u32 tail = __atomic_load_n(&sRingTail, __ATOMIC_ACQUIRE);
    const u32 used = head - tail;
    if (NET_RING_CAP - used < (u32)len + 1u) {
        __atomic_add_fetch(&sRingDrop, 1, __ATOMIC_RELAXED);
        __atomic_add_fetch(&sRingDropTotal, 1, __ATOMIC_RELAXED);
        return;
    }
    u32 pos = head % NET_RING_CAP;
    u32 first = NET_RING_CAP - pos;
    if (first > (u32)len) {
        first = (u32)len;
    }
    memcpy(sRing + pos, msg, first);
    if ((u32)len > first) {
        memcpy(sRing, msg + first, (u32)len - first);
    }
    sRing[(pos + (u32)len) % NET_RING_CAP] = '\n';
    __atomic_store_n(&sRingHead, head + (u32)len + 1u, __ATOMIC_RELEASE);
}

/* Net thread only. Sends whatever the log fd accepts without blocking; a partial send
 * leaves the rest in the ring for the next pass. */
static void netDrainRing(void) {
    const int fd = sLogFd;
    if (fd < 0) {
        return;
    }
    for (;;) {
        const u32 head = __atomic_load_n(&sRingHead, __ATOMIC_ACQUIRE);
        const u32 tail = sRingTail; /* consumer-owned */
        u32 avail = head - tail;
        if (avail == 0) {
            break;
        }
        u32 pos = tail % NET_RING_CAP;
        u32 chunk = NET_RING_CAP - pos;
        if (chunk > avail) {
            chunk = avail;
        }
        int sent = send(fd, sRing + pos, chunk, 0);
        if (sent <= 0) {
            if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                break; /* host is slow; try again next pass, nothing is lost */
            }
            netLog("[net] log link down rc=%d errno=%d", sent, errno);
            close(fd);
            sLogFd = -1;
            break;
        }
        __atomic_store_n(&sRingTail, tail + (u32)sent, __ATOMIC_RELEASE);
    }
    const u32 dropped = __atomic_exchange_n(&sRingDrop, 0, __ATOMIC_RELAXED);
    if (dropped != 0) {
        netLog("[net] dropped=%lu total=%lu", (unsigned long)dropped,
               (unsigned long)sRingDropTotal);
    }
}

/* ---- sockets ----------------------------------------------------------------------- */

/* libctru's SOC layer implements NEITHER SO_RCVTIMEO NOR SO_SNDTIMEO (sys/socket.h has
 * no such constants), so every timeout in this TU is a poll() budget over a NON-BLOCKING
 * fd. That is also the stronger property: the net thread can never block indefinitely in
 * recv()/send() on a console whose WiFi dropped mid-transfer. */
static void netSetNonBlocking(int fd) {
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
}

/* MPFIX: every wait in this TU is a SLICED poll: poll(..., 0), then sleep a slice, until the
 * budget runs out. NEVER poll() with a timeout. libctru's SOC layer talks to the soc:U service
 * over ONE session for the whole process, and a request on a session is served one at a time:
 * while this thread sat in poll(..., 50 ms) every socket call of every other thread waited
 * behind it. With [net] mp_lan on, the main thread's two per-frame recvfrom()s (NEARBY + the
 * room socket) each queued behind that poll: ~50-100 ms a frame, the New 3DS at ~11 ticks/s
 * (hw 516011c, the hitch receipt's `tl` class). A zero-timeout poll is one short request.
 * [net] poll_slice = 0 restores the blocking poll (A/B only). */
static int sPollSliced = 1;

static int netPollFor(struct pollfd* pf, nfds_t nf, int timeoutMs, int sliceMs) {
    if (!sPollSliced) {
        return poll(pf, nf, timeoutMs);
    }
    const u64 start = svcGetSystemTick();
    const u64 budget = (u64)((double)timeoutMs * CPU_TICKS_PER_MSEC);
    for (;;) {
        const int rc = poll(pf, nf, 0);
        if (rc != 0) {
            return rc;
        }
        if (svcGetSystemTick() - start >= budget) {
            return 0;
        }
        svcSleepThread((s64)sliceMs * 1000000LL);
    }
}

/* Waits up to NET_IO_TIMEOUT_S for `events` on fd. 1 = ready, 0 = timeout/error. */
static int netWaitFd(int fd, int events) {
    struct pollfd pf;
    pf.fd = fd;
    pf.events = events;
    pf.revents = 0;
    if (netPollFor(&pf, 1, NET_IO_TIMEOUT_S * 1000, 1) <= 0) {
        return 0;
    }
    if ((pf.revents & (POLLHUP | POLLERR | POLLNVAL)) != 0) {
        return 0;
    }
    return (pf.revents & events) != 0;
}

static void netCloseCtl(const char* why) {
    const int fd = sCtlFd;
    if (fd >= 0) {
        sCtlFd = -1;
        close(fd);
        sCtlLineLen = 0;
        netLog("[net] ctl closed (%s)", why != NULL ? why : "?");
    }
}

static void netCloseLinks(const char* why) {
    netCloseCtl(why);
    if (sListenFd >= 0) {
        const int fd = sListenFd;
        sListenFd = -1;
        close(fd);
    }
    if (sLogFd >= 0) {
        const int fd = sLogFd;
        sLogFd = -1;
        close(fd);
    }
    netLog("[net] links closed (%s)", why != NULL ? why : "?");
}

static void netEnsureListener(void) {
    if (sListenFd >= 0) {
        return;
    }
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        return;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)); /* fast relaunch rebind */
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((u16)sCtlPort);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr*)&a, sizeof(a)) != 0 || listen(fd, 1) != 0) {
        netLog("[net] listen %d failed errno=%d", sCtlPort, errno);
        close(fd);
        return;
    }
    netSetNonBlocking(fd); /* the thread's only wait is poll() */
    sListenFd = fd;
    netLog("[net] ctl listening 0.0.0.0:%d", sCtlPort);
}

/* mp-soc-thread: the wait before the next log-link attempt. Every attempt is a connect plus a
 * 1.5 s poll(…, 0) every 10 ms on the process's one soc:U session (MPFIX): ~75 SOC requests a
 * second while the log host is unreachable. Hardware online1 (an Old 3DS racing online with a
 * stale [net] host): 210 failed attempts across the session, queued in front of the race
 * socket's own sendto / recvfrom. After 3 failures the wait doubles up to NET_RECONNECT_MAX_MS;
 * [net] relink_backoff = 0 keeps the fixed NET_RECONNECT_MS (A/B). */
#define NET_RECONNECT_MAX_MS 30000
static int sRelinkBackoff = 1;

static u32 netRelinkWaitMs(void) {
    u32 ms = NET_RECONNECT_MS;
    unsigned long n;
    if (!sRelinkBackoff) {
        return ms;
    }
    for (n = 3; n < sRelinkAttempts && ms < NET_RECONNECT_MAX_MS; n++) {
        ms *= 2;
    }
    return ms < NET_RECONNECT_MAX_MS ? ms : NET_RECONNECT_MAX_MS;
}

static void netEnsureLogLink(u64* lastAttemptTick) {
    if (sLogFd >= 0 || sHostAddr.s_addr == 0) {
        return;
    }
    const u64 now = svcGetSystemTick();
    const u64 budget = (u64)((double)netRelinkWaitMs() * CPU_TICKS_PER_MSEC);
    if (*lastAttemptTick != 0 && (now - *lastAttemptTick) < budget) {
        return;
    }
    *lastAttemptTick = now;

    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        return;
    }
    netSetNonBlocking(fd);
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((u16)sLogPort);
    a.sin_addr = sHostAddr;
    int rc = connect(fd, (struct sockaddr*)&a, sizeof(a));
    if (rc != 0 && errno != EINPROGRESS && errno != EALREADY) {
        close(fd);
        sRelinkAttempts++;
        if (sRelinkAttempts <= 3 || (sRelinkAttempts % 30) == 0) {
            netLog("[net] relink attempt=%lu errno=%d", sRelinkAttempts, errno);
        }
        return;
    }
    if (rc != 0) {
        struct pollfd pf;
        pf.fd = fd;
        pf.events = POLLOUT;
        pf.revents = 0;
        if (netPollFor(&pf, 1, 1500, 10) <= 0 || (pf.revents & POLLOUT) == 0) {
            close(fd);
            sRelinkAttempts++;
            if (sRelinkAttempts <= 3 || (sRelinkAttempts % 30) == 0) {
                netLog("[net] relink attempt=%lu timeout", sRelinkAttempts);
            }
            return;
        }
    }
    sLogFd = fd; /* stays non-blocking: the drain never waits on a slow host */
    sRelinkAttempts = 0;
    /* First line on a fresh link names the build so the host can tell a RELOAD landed. */
    netLog("[net] link up build=%s host=%s:%d", GDX3DS_BUILD_ID, inet_ntoa(sHostAddr),
           sLogPort);
}

/* Control replies are tiny; a stalled peer costs at most NET_IO_TIMEOUT_S. */
static void netCtlSend(const char* s) {
    const int fd = sCtlFd;
    if (fd < 0) {
        return;
    }
    const size_t len = strlen(s);
    size_t off = 0;
    while (off < len) {
        int n = send(fd, s + off, len - off, 0);
        if (n > 0) {
            off += (size_t)n;
            continue;
        }
        if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
            netCloseCtl("send failed");
            return;
        }
        if (!netWaitFd(fd, POLLOUT)) {
            netCloseCtl("send timeout");
            return;
        }
    }
}

__attribute__((format(printf, 1, 2))) static void netCtlReply(const char* fmt, ...) {
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(msg, sizeof(msg) - 2, fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    if (n > (int)sizeof(msg) - 3) {
        n = (int)sizeof(msg) - 3;
    }
    msg[n] = '\n';
    msg[n + 1] = '\0';
    netCtlSend(msg);
}

/* Binary-safe counterpart of netCtlSend: an explicit length instead of strlen, for GET's raw
 * file bytes (which may contain '\0' or anything else). Same bounded-stall contract: a
 * stalled peer costs at most NET_IO_TIMEOUT_S per send() retry. */
static void netCtlSendRaw(const char* buf, size_t len) {
    const int fd = sCtlFd;
    if (fd < 0) {
        return;
    }
    size_t off = 0;
    while (off < len) {
        int n = send(fd, buf + off, len - off, 0);
        if (n > 0) {
            off += (size_t)n;
            continue;
        }
        if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
            netCloseCtl("send failed");
            return;
        }
        if (!netWaitFd(fd, POLLOUT)) {
            netCloseCtl("send timeout");
            return;
        }
    }
}

/* ---- main-thread request hand-off --------------------------------------------------- */

static int netPostRequest(int kind, const char* a, const char* b, const char* c) {
    if (__atomic_load_n(&sReqKind, __ATOMIC_ACQUIRE) != REQ_NONE) {
        return -2; /* one command in flight */
    }
    sReqA[0] = '\0';
    sReqB[0] = '\0';
    sReqC[0] = '\0';
    if (a != NULL) {
        snprintf(sReqA, sizeof(sReqA), "%s", a);
    }
    if (b != NULL) {
        snprintf(sReqB, sizeof(sReqB), "%s", b);
    }
    if (c != NULL) {
        snprintf(sReqC, sizeof(sReqC), "%s", c);
    }
    sReqRc = 0;
    __atomic_store_n(&sReqDone, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&sReqKind, kind, __ATOMIC_RELEASE);
    for (int i = 0; i < 300; i++) { /* 3 s: a wedged frame loop must not wedge the link */
        if (__atomic_load_n(&sReqDone, __ATOMIC_ACQUIRE) != 0) {
            return sReqRc;
        }
        svcSleepThread(10000000LL);
    }
    return -3; /* main thread is not draining */
}

/* Defined below, in "file send (GET)" -- forward-declared here because gdx3ds_net_tick
 * calls it (main-thread execution per the comment above netGetFileForHost itself) and the
 * REQ_* dispatch functions are grouped earlier in the file than the file-send section. */
static int netGetFileForHost(const char* sdmcPath, u32* outLen, u32* outCrc);

void gdx3ds_net_tick(unsigned long frameNum) {
    sFrameNum = frameNum;
    const int kind = __atomic_load_n(&sReqKind, __ATOMIC_ACQUIRE);
    if (kind == REQ_NONE) {
        return;
    }
    int rc = 0;
    switch (kind) {
        case REQ_QUIT:
            netLog("[net] cmd QUIT -> gdx_request_quit()");
            gdx_request_quit();
            break;
        case REQ_RELOAD:
            netLog("[net] cmd RELOAD staged=%s -> quit + hb:ldr", sTargetPath);
            sRelaunchPending = 1;
            gdx_request_quit();
            break;
        case REQ_CAPTURE:
            if (gdx_oracle_capture_request != NULL) {
                gdx_oracle_capture_request(sReqA);
                netLog("[net] cmd CAPTURE tag=%s armed frame=%lu", sReqA, sFrameNum);
            } else {
                netLog("[net] cmd CAPTURE tag=%s — oracle hook absent in this build", sReqA);
                rc = -1;
            }
            break;
        case REQ_REPLAY:
            if (gdx_replay_start != NULL) {
                gdx_replay_start(sReqC);
                netLog("[net] cmd REPLAY path=%s started", sReqC);
            } else {
                netLog("[net] cmd REPLAY path=%s — replay hook absent in this build", sReqC);
                rc = -1;
            }
            break;
        case REQ_INI_SET:
            rc = gdx3ds_config_set_string(sReqA, sReqB, sReqC);
            netLog("[net] cmd INI [%s] %s=%s rc=%d", sReqA, sReqB, sReqC, rc);
            break;
        case REQ_INI_DEL:
            /* Removing the key is not the same as setting it empty: an empty entry is
             * still an entry, so the getters return "" instead of the DEFAULT and
             * gdx3ds_config_save writes the key straight back out. That is why a replay
             * stayed armed across reloads. rc 3 = the key was already absent. */
            rc = gdx3ds_config_unset(sReqA, sReqB);
            netLog("[net] ini del [%s] %s rc=%d", sReqA, sReqB, rc);
            break;
        case REQ_INI_SAVE:
            rc = gdx3ds_config_save(GDX3DS_CONFIG_DEFAULT_PATH);
            netLog("[net] cmd INI SAVE rc=%d", rc);
            break;
        case REQ_GET:
            /* Flush first: if the target is log.txt, a pending batch in the filelog's own
             * ring would otherwise make the pulled file look older than the live log. */
            gdx3ds_filelog_flush_sync();
            rc = netGetFileForHost(sReqC, &sGetLen, &sGetCrc);
            netLog("[net] cmd GET path=%s rc=%d len=%lu", sReqC, rc, (unsigned long)sGetLen);
            break;
        default:
            rc = -1;
            break;
    }
    sReqRc = rc;
    __atomic_store_n(&sReqKind, REQ_NONE, __ATOMIC_RELEASE);
    __atomic_store_n(&sReqDone, 1, __ATOMIC_RELEASE);
}

/* ---- file receive (RELOAD / PUT) ---------------------------------------------------- */

static int netRecvExact(int fd, char* buf, size_t want) {
    size_t off = 0;
    while (off < want) {
        int n = recv(fd, buf + off, want - off, 0);
        if (n > 0) {
            off += (size_t)n;
            continue;
        }
        if (n == 0) {
            return -1; /* peer closed mid-payload */
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            return -1;
        }
        if (!netWaitFd(fd, POLLIN)) {
            return -1; /* NET_IO_TIMEOUT_S with no fragment: treat as a framing failure */
        }
    }
    return 0;
}

/* mkdir every directory component of "sdmc:/a/b/c.ext" below the file name. Existing
 * directories fail with EEXIST, which is exactly what we want to ignore. */
static void netMkdirParents(const char* sdmcPath) {
    char tmp[NET_PATH_CAP];
    snprintf(tmp, sizeof(tmp), "%s", sdmcPath);
    char* last = strrchr(tmp, '/');
    if (last == NULL) {
        return;
    }
    *last = '\0';
    /* Skip the "sdmc:" device prefix and the leading '/' so the first mkdir is a real
     * directory, not the device root. */
    char* scan = strchr(tmp, ':');
    scan = (scan != NULL) ? scan + 1 : tmp;
    if (*scan == '/') {
        scan++;
    }
    for (char* p = scan; *p != '\0'; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0777);
            *p = '/';
        }
    }
    mkdir(tmp, 0777);
}

/* Streams `size` bytes from fd into "<sdmcPath>.tmp" in NET_RX_CAP chunks, verifies the
 * CRC, then rename()s over sdmcPath. A truncated or corrupt transfer therefore never
 * leaves a half-written target behind. Returns 0 on success. */
static int netRecvFileTo(int fd, const char* sdmcPath, u32 size, u32 wantCrc) {
    char tmpPath[NET_PATH_CAP + 8];
    snprintf(tmpPath, sizeof(tmpPath), "%s.tmp", sdmcPath);
    netMkdirParents(sdmcPath);
    FILE* f = fopen(tmpPath, "wb");
    if (f == NULL) {
        netLog("[net] recv open failed path=%s errno=%d", tmpPath, errno);
        return -1;
    }
    netCrcInit();
    u32 crc = 0xFFFFFFFFu;
    u32 left = size;
    int bad = 0;
    while (left > 0) {
        u32 chunk = left > NET_RX_CAP ? NET_RX_CAP : left;
        if (netRecvExact(fd, sRxBuf, chunk) != 0) {
            netLog("[net] recv short want=%lu left=%lu errno=%d", (unsigned long)size,
                   (unsigned long)left, errno);
            bad = 1;
            break;
        }
        if (fwrite(sRxBuf, 1, chunk, f) != chunk) {
            netLog("[net] recv write failed errno=%d", errno);
            bad = 1;
            break;
        }
        crc = netCrcUpdate(crc, sRxBuf, chunk);
        left -= chunk;
    }
    fclose(f);
    crc ^= 0xFFFFFFFFu;
    if (bad) {
        remove(tmpPath);
        return -1;
    }
    if (crc != wantCrc) {
        netLog("[net] recv crc mismatch want=%08lX got=%08lX", (unsigned long)wantCrc,
               (unsigned long)crc);
        remove(tmpPath);
        return -2;
    }
    remove(sdmcPath); /* newlib rename() does not replace on the 3DS devoptab */
    if (rename(tmpPath, sdmcPath) != 0) {
        netLog("[net] recv rename failed %s -> %s errno=%d", tmpPath, sdmcPath, errno);
        remove(tmpPath);
        return -3;
    }
    netLog("[net] recv ok path=%s size=%lu crc=%08lX", sdmcPath, (unsigned long)size,
           (unsigned long)crc);
    return 0;
}

/* ---- file send (GET) ----------------------------------------------------------------
 *
 * MAIN THREAD ONLY, per netPostRequest's REQ_* discipline (this file's header, "LOCK
 * DISCIPLINE") -- the same reason RELOAD/INI go through the request queue rather than
 * being handled straight on the net thread. Unlike PUT/RELOAD (which stream straight to
 * the FS from the net thread because they only ever touch a FRESH .tmp path of their own),
 * GET's target can be log.txt: a file gdx3ds_filelog_write() may be appending to AT THE
 * SAME TIME from another thread (main, watchdog, audio) through its own recursive lock.
 * libctru's sdmc devoptab is not documented safe for two independently-opened FILE* onto
 * the SAME path from two threads at once, so GET's fopen/fread is routed onto the main
 * thread -- the thread the filelog sink itself always flushes from for its OWN writes, so
 * the two paths are at least serialized by the request queue's one-command-at-a-time rule
 * plus main-thread-only execution, never genuinely concurrent with a cross-thread flush. */

/* Returns 0 (ok, *outLen/*outCrc set), -5 (not found/unreadable) or -4 (over NET_GET_CAP). */
static int netGetFileForHost(const char* sdmcPath, u32* outLen, u32* outCrc) {
    long size;
    size_t got;
    FILE* f = fopen(sdmcPath, "rb");
    if (f == NULL) {
        return -5;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -5;
    }
    size = ftell(f);
    if (size < 0) {
        fclose(f);
        return -5;
    }
    if ((u32)size > NET_GET_CAP) {
        fclose(f);
        return -4;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return -5;
    }
    got = (size > 0) ? fread(sRxBuf, 1, (size_t)size, f) : 0;
    fclose(f);
    if (got != (size_t)size) {
        return -5;
    }
    netCrcInit();
    u32 crc = 0xFFFFFFFFu;
    crc = netCrcUpdate(crc, sRxBuf, got);
    crc ^= 0xFFFFFFFFu;
    *outLen = (u32)got;
    *outCrc = crc;
    return 0;
}

/* ---- control command dispatch ------------------------------------------------------- */

/* Splits `line` in place into at most `max` whitespace-separated tokens. */
static int netTokenize(char* line, char** tok, int max) {
    int n = 0;
    char* p = line;
    while (*p != '\0' && n < max) {
        while (*p == ' ' || *p == '\t' || *p == '\r') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        tok[n++] = p;
        while (*p != '\0' && *p != ' ' && *p != '\t' && *p != '\r') {
            p++;
        }
        if (*p != '\0') {
            *p++ = '\0';
        }
    }
    return n;
}

static const char* netReqErr(int rc) {
    if (rc == -2) {
        return "busy";
    }
    if (rc == -3) {
        return "main-thread-stalled";
    }
    return "failed";
}

/* Rejects "..", absolute-escape and non-sdmc paths: the control port is LAN-reachable. */
static int netPathOk(const char* p) {
    if (p == NULL || p[0] != '/' || strstr(p, "..") != NULL) {
        return 0;
    }
    return strlen(p) + 6 < NET_PATH_CAP;
}

/* GET is read access, not write access, and the control port is LAN-reachable: confine it
 * to our own sandbox (NET_GET_PREFIX) rather than the whole sdmc: tree PUT/RELOAD can
 * target. Reuses netPathOk's ".." / length checks first. */
static int netPathUnderGdiffuser(const char* p) {
    if (!netPathOk(p)) {
        return 0;
    }
    return strncmp(p, NET_GET_PREFIX, sizeof(NET_GET_PREFIX) - 1) == 0;
}

static void netHandleCommand(char* line) {
    char* tok[8];
    const int n = netTokenize(line, tok, 8);
    if (n == 0) {
        return;
    }
    const int fd = sCtlFd;

    if (strcmp(tok[0], "PING") == 0) {
        const u64 ms = (svcGetSystemTick() - sBootTick) / (u64)(CPU_TICKS_PER_MSEC);
        netCtlReply("OK PONG %s %lu %llu", GDX3DS_BUILD_ID, sFrameNum, (unsigned long long)ms);
        return;
    }
    if (strcmp(tok[0], "QUIT") == 0) {
        int rc = netPostRequest(REQ_QUIT, NULL, NULL, NULL);
        if (rc == 0) {
            netCtlReply("OK QUIT");
        } else {
            netCtlReply("ERR QUIT %s", netReqErr(rc));
        }
        return;
    }
    if (strcmp(tok[0], "CAPTURE") == 0) {
        if (n < 2) {
            netCtlReply("ERR CAPTURE needs a tag");
            return;
        }
        int rc = netPostRequest(REQ_CAPTURE, tok[1], NULL, NULL);
        if (rc == 0) {
            netCtlReply("OK CAPTURE armed %s", tok[1]);
        } else if (rc == -1) {
            netCtlReply("ERR CAPTURE oracle-hook-absent");
        } else {
            netCtlReply("ERR CAPTURE %s", netReqErr(rc));
        }
        return;
    }
    if (strcmp(tok[0], "REPLAY") == 0) {
        if (n < 2 || !netPathOk(tok[1])) {
            netCtlReply("ERR REPLAY needs an sdmc-relative path");
            return;
        }
        char sdmc[NET_PATH_CAP];
        snprintf(sdmc, sizeof(sdmc), "sdmc:%s", tok[1]);
        int rc = netPostRequest(REQ_REPLAY, NULL, NULL, sdmc);
        if (rc == 0) {
            netCtlReply("OK REPLAY %s", sdmc);
        } else if (rc == -1) {
            netCtlReply("ERR REPLAY hook-absent");
        } else {
            netCtlReply("ERR REPLAY %s", netReqErr(rc));
        }
        return;
    }
    if (strcmp(tok[0], "INI") == 0) {
        if (n == 2 && strcmp(tok[1], "SAVE") == 0) {
            int rc = netPostRequest(REQ_INI_SAVE, NULL, NULL, NULL);
            if (rc == 0) {
                netCtlReply("OK INI saved");
            } else {
                netCtlReply("ERR INI %s", netReqErr(rc));
            }
            return;
        }
        /* Checked BEFORE the set form, which would otherwise read "DEL" as a section
         * name. No real section is called DEL (they are debug/native/display/...), so
         * the shadowing is safe; INI SAVE above is shadowed the same way. */
        if (n >= 2 && strcmp(tok[1], "DEL") == 0) {
            if (n < 4) {
                netCtlReply("ERR INI usage: INI DEL <section> <key>");
                return;
            }
            int rcd = netPostRequest(REQ_INI_DEL, tok[2], tok[3], NULL);
            if (rcd == 0) {
                netCtlReply("OK INI deleted");
            } else {
                netCtlReply("ERR INI %s", netReqErr(rcd));
            }
            return;
        }
        if (n < 4) {
            netCtlReply("ERR INI usage: INI <section> <key> <value> | "
                        "INI DEL <section> <key> | INI SAVE");
            return;
        }
        int rc = netPostRequest(REQ_INI_SET, tok[1], tok[2], tok[3]);
        if (rc == 0) {
            netCtlReply("OK INI");
        } else {
            netCtlReply("ERR INI %s", netReqErr(rc));
        }
        return;
    }
    if (strcmp(tok[0], "PUT") == 0) {
        /* PUT <sdmc-relative-path> <size> <crc32hex> + <size> raw bytes */
        if (n < 4 || !netPathOk(tok[1])) {
            netCtlReply("ERR PUT usage: PUT <path> <size> <crc32>");
            netCloseCtl("bad PUT header");
            return;
        }
        u32 size = (u32)strtoul(tok[2], NULL, 10);
        u32 crc = (u32)strtoul(tok[3], NULL, 16);
        char sdmc[NET_PATH_CAP];
        snprintf(sdmc, sizeof(sdmc), "sdmc:%s", tok[1]);
        netCtlReply("OK PUT ready %lu", (unsigned long)size);
        int rc = netRecvFileTo(fd, sdmc, size, crc);
        if (rc == 0) {
            netCtlReply("OK PUT %s", sdmc);
        } else {
            netCtlReply("ERR PUT rc=%d", rc);
            netCloseCtl("PUT failed");
        }
        return;
    }
    if (strcmp(tok[0], "GET") == 0) {
        /* GET <sdmc-relative-path>, confined under NET_GET_PREFIX and NET_GET_CAP. Reply is
         * "OK GET <size> <crc32hex>" followed by exactly <size> raw bytes, or "ERR GET ..."
         * with no bytes at all -- the host tells them apart by the first word, same as
         * every other reply on this link. */
        if (n < 2 || !netPathUnderGdiffuser(tok[1])) {
            netCtlReply("ERR GET needs a path under %s", NET_GET_PREFIX);
            return;
        }
        char sdmc[NET_PATH_CAP];
        snprintf(sdmc, sizeof(sdmc), "sdmc:%s", tok[1]);
        int rc = netPostRequest(REQ_GET, NULL, NULL, sdmc);
        if (rc == 0) {
            netCtlReply("OK GET %lu %08lX", (unsigned long)sGetLen, (unsigned long)sGetCrc);
            netCtlSendRaw(sRxBuf, sGetLen);
        } else if (rc == -4) {
            netCtlReply("ERR GET too-large cap=%lu", (unsigned long)NET_GET_CAP);
        } else if (rc == -5) {
            netCtlReply("ERR GET not-found-or-unreadable %s", sdmc);
        } else {
            netCtlReply("ERR GET %s", netReqErr(rc));
        }
        return;
    }
    if (strcmp(tok[0], "RELOAD") == 0) {
        /* RELOAD <size> <crc32hex> + <size> raw bytes, into [net] target. */
        if (n < 3) {
            netCtlReply("ERR RELOAD usage: RELOAD <size> <crc32>");
            netCloseCtl("bad RELOAD header");
            return;
        }
        u32 size = (u32)strtoul(tok[1], NULL, 10);
        u32 crc = (u32)strtoul(tok[2], NULL, 16);
        netCtlReply("OK RELOAD ready %lu", (unsigned long)size);
        int rc = netRecvFileTo(fd, sTargetSdmc, size, crc);
        if (rc != 0) {
            netCtlReply("ERR RELOAD rc=%d", rc);
            netCloseCtl("RELOAD failed");
            return;
        }
        int prc = netPostRequest(REQ_RELOAD, NULL, NULL, NULL);
        if (prc != 0) {
            netCtlReply("ERR RELOAD %s", netReqErr(prc));
            return;
        }
        netCtlReply("OK RELOAD");
        netCloseCtl("reload accepted");
        return;
    }
    netCtlReply("ERR unknown command %s", tok[0]);
}

/* Reads whatever is queued on the control fd, dispatching on each '\n'. */
static void netReadCtl(void) {
    const int fd = sCtlFd;
    if (fd < 0) {
        return;
    }
    char buf[256];
    int n = recv(fd, buf, sizeof(buf), 0);
    if (n == 0) {
        netCloseCtl("peer closed");
        return;
    }
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        }
        netCloseCtl("recv error");
        return;
    }
    for (int i = 0; i < n; i++) {
        const char c = buf[i];
        if (c == '\n') {
            sCtlLine[sCtlLineLen] = '\0';
            sCtlLineLen = 0;
            netHandleCommand(sCtlLine);
            if (sCtlFd < 0) {
                return; /* the command closed the link (RELOAD / framing error) */
            }
        } else if (sCtlLineLen + 1 < NET_LINE_CAP) {
            sCtlLine[sCtlLineLen++] = c;
        } else {
            netCloseCtl("line too long");
            return;
        }
    }
}

static void netAcceptCtl(void) {
    const int lfd = sListenFd;
    if (lfd < 0) {
        return;
    }
    struct sockaddr_in peer;
    socklen_t plen = sizeof(peer);
    int fd = accept(lfd, (struct sockaddr*)&peer, &plen);
    if (fd < 0) {
        return;
    }
    if (sCtlFd >= 0) {
        /* One client at a time: the newcomer wins, the stale one is dropped. */
        netCloseCtl("superseded");
    }
    netSetNonBlocking(fd);
    sCtlFd = fd;
    sCtlLineLen = 0;
    netLog("[net] ctl accept from %s", inet_ntoa(peer.sin_addr));
}

/* ---- net thread --------------------------------------------------------------------- */

/* MP quiet: no relink, no listener; a live log link drains and a live control client is read,
 * both at NET_QUIET_DRAIN_MS. With neither, the thread makes no SOC call (sMpQuietAck). */
static void netQuietCtlPass(void) {
    struct pollfd pf;
    if (sCtlFd < 0) {
        return;
    }
    pf.fd = sCtlFd;
    pf.events = POLLIN;
    pf.revents = 0;
    if (poll(&pf, 1, 0) > 0 && (pf.revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
        netReadCtl();
    }
}

static void netQuietPass(void) {
    if (sListenFd >= 0) {
        close(sListenFd);
        sListenFd = -1;
    }
    netQuietCtlPass();
    netDrainRing();
    sMpQuietAck = (sLogFd < 0 && sCtlFd < 0) ? 1 : 0;
    svcSleepThread((s64)NET_QUIET_DRAIN_MS * 1000000LL);
}

static void netThreadMain(void* arg) {
    (void)arg;
    u64 lastConnect = 0;
    netLog("[net] thread up prio=0x%02X core=%d", NET_THREAD_PRIO, NET_THREAD_CORE);
    while (!sNetQuit) {
        if (sNetSuspended) {
            if (!sNetSuspendAck) {
                netCloseLinks("apt suspend");
                lastConnect = 0;
                sNetSuspendAck = 1;
            }
            svcSleepThread(50000000LL); /* 50 ms */
            continue;
        }
        if (sMpQuiet) {
            netQuietPass();
            continue;
        }
        sMpQuietAck = 0;
        netEnsureListener();
        netEnsureLogLink(&lastConnect);

        struct pollfd pf[2];
        nfds_t nf = 0;
        if (sListenFd >= 0) {
            pf[nf].fd = sListenFd;
            pf[nf].events = POLLIN;
            pf[nf].revents = 0;
            nf++;
        }
        if (sCtlFd >= 0) {
            pf[nf].fd = sCtlFd;
            pf[nf].events = POLLIN;
            pf[nf].revents = 0;
            nf++;
        }
        if (nf == 0) {
            svcSleepThread((s64)NET_POLL_MS * 1000000LL);
        } else if (netPollFor(pf, nf, NET_POLL_MS, NET_POLL_MS) > 0) {
            for (nfds_t i = 0; i < nf; i++) {
                if ((pf[i].revents & (POLLIN | POLLHUP | POLLERR)) == 0) {
                    continue;
                }
                if (pf[i].fd == sListenFd) {
                    netAcceptCtl();
                } else if (pf[i].fd == sCtlFd) {
                    netReadCtl();
                }
            }
        }
        netDrainRing();
    }
    netDrainRing(); /* the teardown receipts must reach the host */
    netCloseLinks("thread exit");
}

/* ---- APT ----------------------------------------------------------------------------- */

/* HOMEFIX2: returns 1 when the net thread acknowledged the park inside the window, 0 when it
 * did not (the caller logs an [apt] receipt for it -- netLog is a no-op with [net] enabled=0,
 * which is every replay lane and most hardware runs, so the ack could never be seen before).
 * WHO SIGNALS sNetSuspendAck: the net thread, at the TOP of its loop only -- a thread inside
 * netRecvFileTo/netRecvExact misses the whole window and keeps doing SOC IPC across
 * GSPGPU_ReleaseRight and APT_JumpToHomeMenu. Not signalled while it is absent => 1. */
int gdx3ds_net_suspend(void) {
    if (!sNetEnabled || sNetSuspended) {
        return 1;
    }
    sNetSuspendAck = 0;
    sNetSuspended = 1;
    /* Bounded wait for the net thread to actually drop the fds: the OS transition must
     * not run with a send() in flight. 0x3E is the lowest app priority, so give it room. */
    for (int i = 0; i < 24 && !sNetSuspendAck; i++) {
        svcSleepThread(10000000LL); /* 10 ms */
    }
    netLog("[net] suspend ack=%d", sNetSuspendAck);
    return sNetSuspendAck ? 1 : 0;
}

void gdx3ds_net_resume(void) {
    if (!sNetEnabled || !sNetSuspended) {
        return;
    }
    sNetSuspended = 0;
    netLog("[net] resume — relinking");
}

void gdx3ds_net_set_mp_quiet(int quiet) {
    const int want = (quiet && sMpQuietLever) ? 1 : 0;
    if (!sNetEnabled || want == sMpQuiet) {
        return;
    }
    if (!want) {
        sMpQuietAck = 0;
    }
    sMpQuiet = want;
    netLog("[net] mp quiet=%d (net.mp_net_quiet): %s", want,
           want ? "no relink / listener while the room is up" : "relink / listener back");
}

int gdx3ds_net_soc_idle(void) {
    if (!sNetEnabled || sNetThread == NULL) {
        return 1;
    }
    return sMpQuiet && sMpQuietAck;
}

int gdx3ds_net_state(void) {
    if (!sNetEnabled) {
        return -1;
    }
    if (sCtlFd >= 0) {
        return 2;
    }
    if (sLogFd >= 0) {
        return 1;
    }
    return 0;
}

/* ---- hb:ldr self-relaunch -------------------------------------------------------------
 * No libctru header covers hb:ldr; these are the two commands 3ds-hbmenu's Rosalina
 * loader issues (source/loaders/rosalina.c): SetTarget = header 0x20002, static buffer 0;
 * SetArgv = header 0x30002, static buffer 1, buffer[0] = argc followed by NUL-separated
 * strings from buffer+4 (ENTRY_ARGBUFSIZE 0x400). Luma relaunches the target when this
 * process exits. */

#define HBLDR_ARGBUF_SIZE 0x400

static Result hbldrSetTarget(Handle h, const char* path) {
    u32 pathLen = (u32)strlen(path) + 1;
    u32* cmdbuf = getThreadCommandBuffer();
    cmdbuf[0] = IPC_MakeHeader(2, 0, 2); /* 0x20002 */
    cmdbuf[1] = IPC_Desc_StaticBuffer(pathLen, 0);
    cmdbuf[2] = (u32)path;
    Result rc = svcSendSyncRequest(h);
    if (R_SUCCEEDED(rc)) {
        rc = (Result)cmdbuf[1];
    }
    return rc;
}

static Result hbldrSetArgv(Handle h, const void* buf, u32 size) {
    u32* cmdbuf = getThreadCommandBuffer();
    cmdbuf[0] = IPC_MakeHeader(3, 0, 2); /* 0x30002 */
    cmdbuf[1] = IPC_Desc_StaticBuffer(size, 1);
    cmdbuf[2] = (u32)buf;
    Result rc = svcSendSyncRequest(h);
    if (R_SUCCEEDED(rc)) {
        rc = (Result)cmdbuf[1];
    }
    return rc;
}

void gdx3ds_net_relaunch_if_requested(void) {
    /* NOT gated on sNetEnabled: this runs AFTER gdx3ds_net_shutdown(), which clears that
     * flag by design. sRelaunchPending is only ever set by the main thread's REQ_RELOAD,
     * which can only happen while the link was up, so it is the whole condition.
     * (Caught in Azahar: the first version had the sNetEnabled guard and silently skipped
     * the hb:ldr call, so not even the "unavailable" receipt was written.) */
    if (!sRelaunchPending) {
        return;
    }
    Handle h = 0;
    Result rc = svcConnectToPort(&h, "hb:ldr");
    if (R_FAILED(rc)) {
        /* Non-Luma loader, or an emulator with no Rosalina: the user re-presses A in
         * hbmenu. Deliberately not fatal — the new image is already staged on the card. */
        netLog("[net] hbldr unavailable rc=0x%08lX — plain exit, relaunch by hand",
               (unsigned long)rc);
        return;
    }
    rc = hbldrSetTarget(h, sTargetPath);
    netLog("[net] hbldr SetTarget %s rc=0x%08lX", sTargetPath, (unsigned long)rc);
    if (R_SUCCEEDED(rc)) {
        static char argbuf[HBLDR_ARGBUF_SIZE];
        memset(argbuf, 0, sizeof(argbuf));
        char* p = argbuf + 4;
        u32 argc = 0;
        /* argv[0]: the launched path, exactly as hbmenu passes it. */
        const char* a0 = sArgv0[0] != '\0' ? sArgv0 : sTargetSdmc;
        size_t l0 = strlen(a0);
        memcpy(p, a0, l0 + 1);
        p += l0 + 1;
        argc++;
        /* Re-emit the netloader trailer so the relaunched process inherits the host.
         * libctru's __system_initArgv CONSUMES this argument (argc--, argv[argc] = NULL),
         * so it can never be copied out of our own argv — it is rebuilt from the parsed
         * value. Layout, verified against libctru 2.7.0 initArgv.o: the LAST argv entry,
         * strlen 17, "_3DSLINK_" at offset 8, first 8 chars = strtoul(base 16) of
         * in_addr.s_addr. */
        if (sHostAddr.s_addr != 0 && (size_t)(p - argbuf) + 18 < sizeof(argbuf)) {
            snprintf(p, 18, "%08lX_3DSLINK_", (unsigned long)sHostAddr.s_addr);
            p += 18;
            argc++;
        }
        memcpy(argbuf, &argc, sizeof(argc));
        rc = hbldrSetArgv(h, argbuf, sizeof(argbuf));
        netLog("[net] hbldr SetArgv argc=%lu rc=0x%08lX", (unsigned long)argc,
               (unsigned long)rc);
    }
    svcCloseHandle(h);
    gdx3ds_filelog_flush();
}

/* ---- bring-up / teardown --------------------------------------------------------------- */

static void netSocAtexit(void) {
    if (sSocUp && !sSocExited) {
        sSocExited = 1;
        socExit();
    }
}

void gdx3ds_net_init(int argc, char** argv) {
    sBootTick = svcGetSystemTick();

    /* [net] enabled's DEFAULT (not the ini, which always wins when present) depends on how
     * we were launched. __3dslink_host is nonzero here exactly when hbmenu's netloader
     * (3dslink) launched us -- it is injected before main() runs (see this file's header,
     * "Bring-up"), well before this point in a normal boot. The resolution itself (and the
     * GDX_NET_ENABLED_SENTINEL this relies on) lives in port/gdx_net_enabled_default.c,
     * split out so it is host-testable -- this TU is 3DS-only. */
    const int netEnabledExplicit =
        gdx3ds_config_get_bool("net", "enabled", GDX_NET_ENABLED_SENTINEL);
    const int launchedViaNetloader = (__3dslink_host.s_addr != 0);
    const int netEnabledResolved =
        gdx_net_enabled_resolve(netEnabledExplicit, launchedViaNetloader);
    if (!netEnabledResolved) {
        sRingArmed = 0; /* stop buffering boot lines nobody will drain */
        return;         /* KILLSWITCH: not one receipt, not one byte allocated */
    }
    sNetEnabled = 1;
    sRingArmed = 1;

    if (gdx_net_enabled_was_defaulted(netEnabledExplicit)) {
        /* Only reachable when launchedViaNetloader was the deciding vote (the ini had no
         * key and a plain hbmenu/SD/CIA launch would have stayed off), so the receipt never
         * needs an if/else -- it is always the netloader reason. */
        netLog("[net] enabled defaulted ON: launched via 3dslink netloader, no [net] "
               "enabled key in gdiffuser.ini");
    }

    sCtlPort = gdx3ds_config_get_int("net", "port", NET_DEFAULT_CTL_PORT);
    sLogPort = gdx3ds_config_get_int("net", "log_port", NET_DEFAULT_LOG_PORT);
    sWantStdio = gdx3ds_config_get_bool("net", "stdio", 0);
    sPollSliced = gdx3ds_config_get_bool("net", "poll_slice", 1);
    sRelinkBackoff = gdx3ds_config_get_bool("net", "relink_backoff", 1);
    sMpQuietLever = gdx3ds_config_get_bool("net", "mp_net_quiet", 1);
    if (!sPollSliced) {
        netLog("[net] poll_slice=0: the net thread blocks in poll() (stalls every other thread's "
               "socket calls; A/B only)");
    }
    snprintf(sTargetPath, sizeof(sTargetPath), "%s",
             gdx3ds_config_get_string("net", "target", NET_DEFAULT_TARGET));
    snprintf(sTargetSdmc, sizeof(sTargetSdmc), "sdmc:%s", sTargetPath);
    if (argc > 0 && argv != NULL && argv[0] != NULL) {
        snprintf(sArgv0, sizeof(sArgv0), "%s", argv[0]);
    }

    /* Host for the log link: the netloader-injected address, else [net] host. */
    sHostAddr = __3dslink_host;
    const char* hostKey = gdx3ds_config_get_string("net", "host", "");
    if (sHostAddr.s_addr == 0 && hostKey != NULL && hostKey[0] != '\0') {
        sHostAddr.s_addr = inet_addr(hostKey);
        if (sHostAddr.s_addr == INADDR_NONE) {
            sHostAddr.s_addr = 0;
        }
    }

    int socKb = gdx3ds_config_get_int("net", "soc_kb", NET_DEFAULT_SOC_KB);
    if (socKb < 64) {
        socKb = 64;
    } else if (socKb > 2048) {
        socKb = 2048;
    }
    socKb &= ~3; /* socInit wants a multiple of the 0x1000 page */
    const u32 socSize = (u32)socKb * 1024u;

    netLog("[net] init port=%d log_port=%d host=%s soc=%dKiB target=%s build=%s", sCtlPort,
           sLogPort, sHostAddr.s_addr != 0 ? inet_ntoa(sHostAddr) : "(none)", socKb,
           sTargetPath, GDX3DS_BUILD_ID);

    sSocBuf = (u32*)memalign(0x1000, socSize);
    if (sSocBuf == NULL) {
        netLog("[net] soc buffer alloc failed (%lu B) — link disabled",
               (unsigned long)socSize);
        sNetEnabled = 0;
        sRingArmed = 0;
        return;
    }
    Result rc = socInit(sSocBuf, socSize);
    if (R_FAILED(rc)) {
        netLog("[net] socInit rc=0x%08lX — link disabled", (unsigned long)rc);
        free(sSocBuf);
        sSocBuf = NULL;
        sNetEnabled = 0;
        sRingArmed = 0;
        return;
    }
    sSocUp = 1;
    atexit(netSocAtexit); /* mirrors the libctru 3dslink example; idempotent */
    netLog("[net] soc rc=0x%08lX size=%luKiB", (unsigned long)rc, (unsigned long)socKb);

    if (sWantStdio && sHostAddr.s_addr != 0) {
        int fd = link3dsStdio();
        netLog("[net] link3dsStdio fd=%d (process stdout/stderr redirected)", fd);
    }

    sNetThread = threadCreate(netThreadMain, NULL, NET_THREAD_STACK, NET_THREAD_PRIO,
                              NET_THREAD_CORE, false);
    if (sNetThread == NULL) {
        netLog("[net] threadCreate failed — link disabled");
        netSocAtexit();
        free(sSocBuf);
        sSocBuf = NULL;
        sNetEnabled = 0;
        sRingArmed = 0;
        return;
    }
}

/* MP spike (gdx3ds_mp.c): share this TU's soc:U rather than socInit a second time. */
int gdx3ds_net_soc_up(void) {
    return sSocUp;
}

void gdx3ds_net_shutdown(void) {
    if (!sNetEnabled) {
        return;
    }
    sNetEnabled = 0; /* state() reports -1 from here; no further receipts over the link */
    sNetQuit = 1;
    if (sNetThread != NULL) {
        threadJoin(sNetThread, 2000000000LL); /* 2 s; the thread's longest wait is a 1.5 s
                                               * connect poll */
        threadFree(sNetThread);
        sNetThread = NULL;
    }
    netCloseLinks("shutdown");
    sRingArmed = 0;
    netSocAtexit();
    if (sSocBuf != NULL) {
        free(sSocBuf); /* socExit gives the pages back to us */
        sSocBuf = NULL;
    }
    netLog("[net] shutdown done dropped_total=%lu", (unsigned long)sRingDropTotal);
}
