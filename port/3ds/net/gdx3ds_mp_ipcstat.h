/* port/3ds/net/gdx3ds_mp_ipcstat.h — NETTICK: how long each network sysmodule call takes on the
 * main thread.
 *
 * Why: on the Old 3DS the multiplayer service costs ~3.8 ms a frame even in the lobby (New 3DS:
 * ~0.25 ms, [mpcost] tail_us), and a race tick spends another ~4.7 ms in the exchange. The code
 * there is a handful of udsPullPacket / udsSendTo / recvfrom calls, i.e. IPC to NWM and SOC, and
 * on the Old 3DS those sysmodules run on core 1, where the game holds 60% for its audio threads
 * ([core1] syscore_pct=60). This receipt measures each call kind so that hypothesis is settled by
 * the next hardware log rather than by a model:
 *
 *   [mpipc] frames=600 uds_pull=n/avg_us/max_us/h(<250us,<1ms,<4ms,4ms+) uds_send=... soc_recv=...
 *           soc_send=... disc_recv=... q_push=... q_pop=... st_recv=... st_send=...
 *           gated=<udsPullPacket calls the bind event let us skip>
 * soc_recv / soc_send are the MAIN thread's socket calls (0 with [net] mp_soc_thread on);
 * st_recv / st_send the socket service thread's; q_push / q_pop the main thread's ring copies.
 *
 * Any thread (the UDS service thread records its udsSendTo / udsPullPacket here too; a LightLock
 * guards the counters). One svcGetSystemTick pair per call: ~1 us. */
#ifndef GDX3DS_MP_IPCSTAT_H
#define GDX3DS_MP_IPCSTAT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    GDX_IPC_UDS_PULL = 0, /* udsPullPacket (NWM) */
    GDX_IPC_UDS_SEND,     /* udsSendTo (NWM) */
    GDX_IPC_SOC_RECV,     /* recvfrom on the mp-online socket (SOC) */
    GDX_IPC_SOC_SEND,     /* sendto on the mp-online socket (SOC) */
    GDX_IPC_DISC_RECV,    /* recvfrom on the NEARBY discovery socket (SOC) */
    GDX_IPC_Q_PUSH,       /* main thread: a datagram into the UDS service thread's tx ring */
    GDX_IPC_Q_POP,        /* main thread: a datagram out of its rx ring */
    GDX_IPC_ST_RECV,      /* the socket service thread's recvfrom (gdx3ds_mp_socthr.c) */
    GDX_IPC_ST_SEND,      /* the socket service thread's sendto */
    GDX_IPC_ST_POLL,      /* its blocking poll() ([net] mp_soc_block_poll, sole SOC user) */
    GDX_IPC_DISC_SEND,    /* sendto on the NEARBY discovery socket (beacons, pings) */
    GDX_IPC_KINDS
};

/* Before a second thread records: sets up the lock (idempotent; main thread). */
void gdx3ds_ipcstat_init(void);
uint64_t gdx3ds_ipcstat_begin(void);
void gdx3ds_ipcstat_end(int kind, uint64_t t0);
/* A call the cheap pre-check made unnecessary (UDS bind event not signalled, SOC poll throttled). */
void gdx3ds_ipcstat_gated(int kind);
/* Writes the window's line ("" when nothing was counted) and resets the window. */
void gdx3ds_ipcstat_line(char* buf, int cap, unsigned long frames);

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_MP_IPCSTAT_H */
