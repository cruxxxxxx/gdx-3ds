/* port/3ds/net/gdx3ds_mp_udp.c — MP spike: the udp-lan transport (contract:
 * gdx3ds_mp_transport.h). One non-blocking UDP socket on soc:U. The host learns each
 * joiner's (ip, port) from its first valid datagram (the session layer calls
 * bind_last_source); a joiner knows only the host. Works between two emulator instances on
 * one Mac (Azahar maps guest sockets 1:1 onto host sockets, so each guest binds its own
 * port) and between consoles on one WiFi network. */
#include "gdx3ds_mp_transport.h"
#include "gdx_mp_lockstep.h" /* GDX_MP_MAX_PLAYERS */

#include <3ds.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int sFd = -1;
static struct sockaddr_in sPeer[GDX_MP_MAX_PLAYERS];
static int sPeerKnown[GDX_MP_MAX_PLAYERS];
static struct sockaddr_in sLastSource;
static int sLocalPlayer = 0;
static int sNumPlayers = 0;
static int sBindPort = 0;

static int OpenSocket(int port) {
    struct sockaddr_in a;
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return -100 - errno;
    }
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((u16) port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr*) &a, sizeof(a)) != 0) {
        const int e = errno;
        close(fd);
        return -200 - e;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    return fd;
}

static int SameAddr(const struct sockaddr_in* a, const struct sockaddr_in* b) {
    return a->sin_addr.s_addr == b->sin_addr.s_addr && a->sin_port == b->sin_port;
}

static int UdpOpen(const GdxMpTransportConfig* cfg) {
    int fd;

    memset(sPeer, 0, sizeof(sPeer));
    memset(sPeerKnown, 0, sizeof(sPeerKnown));
    sLocalPlayer = cfg->localPlayer;
    sNumPlayers = cfg->numPlayers;
    sBindPort = cfg->bindPort;
    fd = OpenSocket(sBindPort);
    if (fd < 0) {
        return fd;
    }
    if (!cfg->isHost) {
        struct sockaddr_in* h = &sPeer[0];
        h->sin_family = AF_INET;
        h->sin_port = htons((u16) cfg->hostPort);
        h->sin_addr.s_addr = inet_addr(cfg->hostAddr != NULL ? cfg->hostAddr : "127.0.0.1");
        sPeerKnown[0] = 1; /* player 0 is the host */
    }
    sFd = fd;
    return 0;
}

static int UdpReopen(void) {
    int fd;
    if (sFd >= 0) {
        close(sFd);
        sFd = -1;
    }
    fd = OpenSocket(sBindPort);
    if (fd < 0) {
        return fd;
    }
    sFd = fd;
    return 0;
}

static void UdpBindLastSource(int player) {
    if (player < 0 || player >= sNumPlayers || player == sLocalPlayer) {
        return;
    }
    sPeer[player] = sLastSource;
    sPeerKnown[player] = 1;
}

static int SendOne(int peer, const void* buf, int len) {
    if (!sPeerKnown[peer]) {
        return 0;
    }
    if (sendto(sFd, buf, (size_t) len, 0, (const struct sockaddr*) &sPeer[peer], sizeof(sPeer[peer])) < 0) {
        /* EAGAIN / ENOBUFS: a dropped datagram, which the ack window absorbs. */
        return (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS) ? 0 : -errno;
    }
    return len;
}

static int UdpSend(int peer, const void* buf, int len) {
    int p;
    int total = 0;
    if (sFd < 0 || len <= 0 || len > GDX_MP_TRANSPORT_MTU) {
        return -1;
    }
    if (peer != GDX_MP_PEER_ANY) {
        if (peer < 0 || peer >= sNumPlayers || peer == sLocalPlayer) {
            return -1;
        }
        return SendOne(peer, buf, len);
    }
    for (p = 0; p < sNumPlayers; p++) {
        if (p != sLocalPlayer) {
            const int n = SendOne(p, buf, len);
            if (n > 0) {
                total += n;
            }
        }
    }
    return total;
}

static int UdpRecv(void* buf, int cap, int* peerHint) {
    socklen_t alen = sizeof(sLastSource);
    ssize_t n;
    int p;
    *peerHint = GDX_MP_PEER_ANY;
    if (sFd < 0) {
        return -1;
    }
    n = recvfrom(sFd, buf, (size_t) cap, 0, (struct sockaddr*) &sLastSource, &alen);
    if (n < 0) {
        return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -errno;
    }
    for (p = 0; p < sNumPlayers; p++) {
        if (sPeerKnown[p] && SameAddr(&sPeer[p], &sLastSource)) {
            *peerHint = p;
            break;
        }
    }
    return (int) n;
}

static void UdpClose(void) {
    if (sFd >= 0) {
        close(sFd);
        sFd = -1;
    }
}

static int UdpSelftest(char* buf, int cap) {
    struct sockaddr_in self;
    struct sockaddr_in name;
    socklen_t nlen = sizeof(name);
    int rc;
    int grc;
    static const char kJunk[8] = { 'S', 'E', 'L', 'F', 0, 0, 0, 0 };
    memset(&self, 0, sizeof(self));
    self.sin_family = AF_INET;
    self.sin_port = htons((u16) sBindPort);
    self.sin_addr.s_addr = inet_addr("127.0.0.1");
    rc = (int) sendto(sFd, kJunk, sizeof(kJunk), 0, (const struct sockaddr*) &self, sizeof(self));
    if (rc < 0) {
        rc = -errno;
    }
    memset(&name, 0, sizeof(name));
    grc = getsockname(sFd, (struct sockaddr*) &name, &nlen);
    {
        int off = snprintf(buf, (size_t) cap, "fd=%d self_send=%d getsockname=%d port=%u me=%d n=%d last=%08lx:%u",
                           sFd, rc, grc < 0 ? -errno : grc, (unsigned) ntohs(name.sin_port), sLocalPlayer,
                           sNumPlayers, (unsigned long) ntohl(sLastSource.sin_addr.s_addr),
                           (unsigned) ntohs(sLastSource.sin_port));
        int p;
        for (p = 0; p < sNumPlayers && p < GDX_MP_MAX_PLAYERS && off < cap - 32; p++) {
            off += snprintf(buf + off, (size_t) (cap - off), " peer%d=%d:%08lx:%u/fam%u", p, sPeerKnown[p],
                            (unsigned long) ntohl(sPeer[p].sin_addr.s_addr), (unsigned) ntohs(sPeer[p].sin_port),
                            (unsigned) sPeer[p].sin_family);
        }
    }
    return rc;
}

static const GdxMpTransportOps kUdpOps = {
    "udp-lan", UdpOpen, UdpBindLastSource, UdpSend, UdpRecv, UdpClose, UdpReopen, UdpSelftest,
};

const GdxMpTransportOps* gdx3ds_mp_udp_ops(void) {
    return &kUdpOps;
}
