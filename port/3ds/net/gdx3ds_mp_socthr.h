/* port/3ds/net/gdx3ds_mp_socthr.h — the socket service thread of the mp-online UDP socket
 * ([net] mp_soc_thread, default 1; 0 = every sendto / recvfrom on the main thread as before).
 *
 * The loop body and its queues are pure and host-tested (port/gdx_mp_socsvc.h, which says why:
 * hardware online1's Old 3DS spent ~15 ms of every race frame in SOC calls). This TU is the
 * thread around it: one thread, a priority above the main thread on [net] mp_soc_thread_core
 * (default -2, the app core: it spends its time blocked in SOC IPC, not on a CPU), owns every
 * sendto / recvfrom of the mp-online socket (online, NEARBY WiFi, serverless rooms) while it is
 * open. It sleeps on a wake event (signalled per queued datagram) with a timeout: the next rx poll
 * ([net] mp_soc_rx_poll_ms, default 8 on an Old 3DS, 4 on a New 3DS; 250 in a local wireless
 * room, where the radio is NWM's) or the next coalesced INPUT ([net] mp_soc_tx_min_ms, default 33
 * on an Old 3DS, 16 on a New 3DS: at most one INPUT per stream per destination per interval; a new
 * INPUT repeats on that cadence up to [net] mp_soc_tx_repeat times while nothing newer replaces it,
 * so the radio sees a steady stream through a render hitch). Its queues are static (24 KiB of byte
 * queues: nothing is allocated at room open, where an Old 3DS has a few KiB of heap slack). The
 * main thread's lockstep wait sleeps on the thread's "datagram queued" event.
 *
 * SOC: libctru serves every socket call of the process over ONE soc:U session, one request at a
 * time (gdx3ds_net.c, MPFIX), so the thread never blocks in a socket call: non-blocking fd, a
 * zero-wait recvfrom per poll, sleeping is svcWaitSynchronization on its own event; only with
 * [net] mp_soc_block_poll = 1, in a race, once nothing else makes a SOC call (the dev net thread
 * quiet with no link: gdx3ds_net_soc_idle), does it block in poll() instead. It also never
 * runs across an APT suspend: gdx3ds_socthr_park(1) flushes what is queued (an ABORT must reach
 * the others), then holds it off SOC until gdx3ds_socthr_park(0).
 *
 * Receipt (with [mpcost], every 600 frames): "[socthr] queued= sent= coalesced= txdrop= blocked=
 * senderr= rx= empty= rxerr= rxfull= taken= drains= wakes= txq=<now>/<max> rxq=<now>/<max>
 * tx_min_ms= rx_poll_ms=", plus [mpipc] st_send / st_recv (the thread's call latencies) and
 * soc_send / soc_recv (the main thread's: 0 while the thread runs). Main thread API. */
#ifndef GDX3DS_MP_SOCTHR_H
#define GDX3DS_MP_SOCTHR_H

#include "gdx_mp_online.h"

#ifdef __cplusplus
extern "C" {
#endif

/* After the socket opened: 1 = the thread owns `fd` now, 0 = inline calls (lever off, or it could
 * not start). */
int gdx3ds_socthr_start(int fd);
/* Before the socket closes: flushes what is queued, joins the thread. */
void gdx3ds_socthr_stop(void);
int gdx3ds_socthr_active(void);
/* Queue one datagram (stream: gdx_mp_socsvc_stream). Returns len, 0 when the ring is full. */
int gdx3ds_socthr_send(const GdxMpEndpoint* to, const void* buf, int len, uint32_t stream);
/* The next received datagram: bytes (and *src), 0 = none waiting. */
int gdx3ds_socthr_recv(void* buf, int cap, GdxMpEndpoint* src);
/* The main thread's lockstep wait: until the thread queued a received datagram, or `maxMs`. */
void gdx3ds_socthr_wait_rx(uint32_t maxMs);
/* 1 = a race is up and the main thread makes no SOC call: with [net] mp_soc_block_poll and the dev
 * net thread quiet, the thread may block in poll() (it is then the only SOC user). */
void gdx3ds_socthr_set_sole(int sole);
/* A local wireless room: poll the socket rarely (1) or at the configured rate (0). */
void gdx3ds_socthr_set_rare_poll(int rare);
/* APT: 1 = flush, then no SOC call until 0. */
void gdx3ds_socthr_park(int park);
/* The "[socthr] ..." receipt ("" while the thread is not running). */
void gdx3ds_socthr_line(char* buf, int cap);

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_MP_SOCTHR_H */
