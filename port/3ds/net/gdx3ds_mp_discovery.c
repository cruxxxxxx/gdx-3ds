/* port/3ds/net/gdx3ds_mp_discovery.c — NEARBY over UDP (contract: gdx3ds_mp_discovery.h; the
 * beacon format and the table are pure and host-tested: port/gdx_mp_lan.c).
 *
 * One socket on the first free port of GDX_LAN_DISCOVERY_PORT .. +3 (several listeners on one
 * machine: the emulator lane runs up to four consoles on one Mac). A host's beacon goes, once a
 * second, to every one of those ports at 255.255.255.255, at the /24 subnet broadcast of our
 * address (some access points drop the limited broadcast) and at each unicast lever address.
 * Listeners PING each row about once a second (20 bytes; the PONG is the same 20 bytes, so a
 * forged source gains nothing) and expire rows after GDX_LAN_EXPIRE_MS of silence. */
#include "gdx3ds_mp_discovery.h"
#include "gdx3ds_mp_ipcstat.h"

#include <3ds.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define DISC_MAX_UNICAST 4
#define DISC_RX_GUARD 32

static int sFd = -1;
static int sPort = 0;
static GdxMpNearby sTable;
static GdxMpLanBeacon sBeacon;
static int sAnnouncing = 0;
static uint32_t sLastBeaconMs = 0;
static GdxMpBucket sPingBucket;
static GdxMpEndpoint sUnicast[DISC_MAX_UNICAST];
static int sUnicastCount = 0;
static uint32_t sTxBeacons = 0;
static uint32_t sTxErrors = 0;
static uint32_t sRxJunk = 0;
static int sBroadcastOk = 0;
static int sBroadcastWanted = 1;

static int BindFirstFree(void) {
    int i;
    for (i = 0; i < GDX_LAN_DISCOVERY_PORTS; i++) {
        struct sockaddr_in a;
        const int fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0) {
            return -100 - errno;
        }
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_port = htons((u16) (GDX_LAN_DISCOVERY_PORT + i));
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(fd, (struct sockaddr*) &a, sizeof(a)) == 0) {
            int one = 1;
            sBroadcastOk = setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one)) == 0;
            fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
            sPort = GDX_LAN_DISCOVERY_PORT + i;
            return fd;
        }
        close(fd);
    }
    return -200;
}

static void ParseUnicast(const char* list) {
    char item[24];
    int n = 0;
    sUnicastCount = 0;
    while (list != NULL && sUnicastCount < DISC_MAX_UNICAST) {
        const char c = *list;
        if (c == ',' || c == '\0') {
            GdxMpEndpoint e;
            item[n] = '\0';
            if (n > 0 && gdx_mpo_parse_hostport(item, &e, GDX_LAN_DISCOVERY_PORT)) {
                sUnicast[sUnicastCount++] = e;
            }
            n = 0;
            if (c == '\0') {
                break;
            }
        } else if (c != ' ' && n < (int) sizeof(item) - 1) {
            item[n++] = c;
        }
        list++;
    }
}

static int UdpOpen(const GdxMpDiscoveryConfig* cfg) {
    if (sFd >= 0) {
        return 0;
    }
    gdx_nearby_init(&sTable, cfg->build, cfg->contentHash);
    gdx_mpo_bucket_init(&sPingBucket, 40, 40);
    ParseUnicast(cfg->unicastPeers);
    sBroadcastWanted = cfg->broadcast;
    sAnnouncing = 0;
    sFd = BindFirstFree();
    return sFd >= 0 ? 0 : sFd;
}

static void UdpClose(void) {
    if (sFd >= 0) {
        close(sFd);
        sFd = -1;
    }
    sAnnouncing = 0;
}

static void SendTo(uint32_t ipNetOrder, int port, const uint8_t* pkt, int len) {
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((u16) port);
    a.sin_addr.s_addr = ipNetOrder;
    {
        const uint64_t t0 = gdx3ds_ipcstat_begin();
        const ssize_t r = sendto(sFd, pkt, (size_t) len, 0, (const struct sockaddr*) &a, sizeof(a));
        gdx3ds_ipcstat_end(GDX_IPC_DISC_SEND, t0);
        if (r < 0) {
            sTxErrors++;
        }
    }
}

static uint32_t IpOf(const GdxMpEndpoint* e) {
    uint32_t v;
    memcpy(&v, e->ip, 4);
    return v;
}

static void SendBeacon(void) {
    uint8_t pkt[GDX_LAN_BEACON_BYTES];
    const int n = gdx_lan_build_beacon(&sBeacon, pkt, sizeof(pkt));
    const uint32_t me = (uint32_t) gethostid(); /* network order in memory */
    int p;
    int u;
    if (n <= 0) {
        return;
    }
    for (p = 0; p < GDX_LAN_DISCOVERY_PORTS; p++) {
        const int port = GDX_LAN_DISCOVERY_PORT + p;
        if (sBroadcastOk && sBroadcastWanted) {
            SendTo(htonl(INADDR_BROADCAST), port, pkt, n);
            if (me != 0) {
                SendTo(me | htonl(0x000000FFu), port, pkt, n); /* the /24 directed broadcast */
            }
        }
        for (u = 0; u < sUnicastCount; u++) {
            SendTo(IpOf(&sUnicast[u]), port, pkt, n);
        }
    }
    sTxBeacons++;
}

static void Receive(uint32_t nowMs) {
    int guard;
    for (guard = 0; guard < DISC_RX_GUARD; guard++) {
        uint8_t buf[128];
        struct sockaddr_in a;
        socklen_t alen = sizeof(a);
        GdxMpLanBeacon b;
        GdxMpEndpoint src;
        uint32_t hostId = 0;
        uint32_t nonce = 0;
        uint32_t sendMs = 0;
        int t;
        const uint64_t t0 = gdx3ds_ipcstat_begin();
        const ssize_t got = recvfrom(sFd, buf, sizeof(buf), 0, (struct sockaddr*) &a, &alen);
        gdx3ds_ipcstat_end(GDX_IPC_DISC_RECV, t0);
        if (got < 0) {
            return;
        }
        memcpy(src.ip, &a.sin_addr.s_addr, 4);
        src.port = ntohs(a.sin_port);
        t = gdx_lan_parse(buf, (int) got, &b, &hostId, &nonce, &sendMs);
        if (t == GDX_LAN_BEACON) {
            gdx_nearby_on_beacon(&sTable, &b, &src, nowMs);
        } else if (t == GDX_LAN_PING) {
            if (sAnnouncing && hostId == sBeacon.hostId && gdx_mpo_bucket_take(&sPingBucket, nowMs)) {
                uint8_t pong[GDX_LAN_PING_BYTES];
                const int n = gdx_lan_build_ping(GDX_LAN_PONG, hostId, nonce, sendMs, pong, sizeof(pong));
                SendTo(IpOf(&src), src.port, pong, n);
            }
        } else if (t == GDX_LAN_PONG) {
            gdx_nearby_on_pong(&sTable, hostId, nonce, sendMs, nowMs);
        } else {
            sRxJunk++;
        }
    }
}

static void UdpService(uint32_t nowMs) {
    int due;
    if (sFd < 0) {
        return;
    }
    Receive(nowMs);
    due = gdx_nearby_tick(&sTable, nowMs);
    if (due >= 0) {
        const GdxMpNearbyRoom* r = &sTable.room[due];
        uint8_t ping[GDX_LAN_PING_BYTES];
        const int n = gdx_lan_build_ping(GDX_LAN_PING, r->b.hostId, r->pingNonce, nowMs, ping, sizeof(ping));
        SendTo(IpOf(&r->beaconFrom), r->beaconFrom.port, ping, n);
    }
    if (sAnnouncing && (sLastBeaconMs == 0 || (int32_t) (nowMs - sLastBeaconMs) >= GDX_LAN_BEACON_MS)) {
        sLastBeaconMs = nowMs;
        SendBeacon();
    }
}

static void UdpAnnounce(const GdxMpLanBeacon* b) {
    if (b == NULL) {
        sAnnouncing = 0;
        sTable.selfHostId = 0;
        return;
    }
    if (!sAnnouncing || memcmp(&sBeacon, b, sizeof(*b)) != 0) {
        sLastBeaconMs = 0; /* a change goes out at once */
    }
    sBeacon = *b;
    sAnnouncing = 1;
    sTable.selfHostId = b->hostId;
}

static const GdxMpNearby* UdpTable(void) {
    return &sTable;
}

static int UdpDescribe(char* buf, int cap) {
    return snprintf(buf, (size_t) cap, "udp-lan fd=%d port=%d bcast=%d unicast=%d beacons=%lu txerr=%lu heard=%lu junk=%lu",
                    sFd, sPort, sBroadcastOk, sUnicastCount, (unsigned long) sTxBeacons, (unsigned long) sTxErrors,
                    (unsigned long) sTable.heard, (unsigned long) sRxJunk);
}

static const GdxMpDiscoveryOps kUdpOps = {
    "udp-lan", UdpOpen, UdpClose, UdpService, UdpAnnounce, UdpTable, UdpDescribe,
};

const GdxMpDiscoveryOps* gdx3ds_mp_discovery_udp_ops(void) {
    return &kUdpOps;
}
